/*-------------------------------------------------------------------------
 *
 * async.c
 *	  异步通知：NOTIFY、LISTEN、UNLISTEN
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/async.c
 *
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * 异步通知模型（9.0 起）： 
 *
 * 1. 同一台机器上有多个后端。多个后端监听若干     
 *	  个通道。（在代码的其他部分中，通道也被称     
 *	  为"条件"。）  
 *
 * 2. 磁盘存储中有一个中央队列（目录 pg_notify/），          
 *	  由 slru.c 模块将活跃使用的页面映射到共享内存。       
 *	  所有通知消息都放入队列，随后由监听后端           
 *	  读取出来。       
 *
 *	  没有中央记录来知晓哪个后端监听哪个通道；            
 *	  每个后端都有自己感兴趣的通道列表。    
 *
 *	  虽然只有一个队列，但通知被视为数据库局部的；  
 *	  实现方式是在每条通知消息中包含发送者的数据    
 *	  库 OID。监听后端会忽略与其数据库 OID 不匹配的  
 *	  消息。这一点很重要，因为它确保发送者和接收   
 *	  者具有相同的数据库编码，不会误解释通道名称        
 *	  或载荷字符串中的非 ASCII 文本。                       
 *
 *	  由于并不指望通知在数据库崩溃后幸存，我们能  
 *	  在任何重启时清空 pg_notify 数据，并且不需要        
 *	  WAL 支持或 fsync。                  
 *
 * 3. 每个至少监听一个通道的后端都会通过将其 PID       
 *	  输入 AsyncQueueControl 中的数组来注册。然后它扫描       
 *	  中央队列中所有传入的通知，先比较通知的数据   
 *	  库 OID 与其自身的数据库 OID，然后再将受通知的    
 *	  通道与其监听的通道列表进行比较。如有匹配，        
 *	  它就把通知事件传递给其前端。不匹配的事件则       
 *	  直接跳过。                                   
 *
 * 4. NOTIFY 语句（例程 Async_Notify）把通知存储在              
 *	  一个后端本地列表中，直到事务结束才会处理它。     
 *
 *	  来自同一事务的重复通知只作为一条通知发出。      
 *	  这样做是为了节省工作量，例如当 200 万行表上的      
 *	  触发器为每一个被更改的行触发一条通知时。如果      
 *	  应用程序需要接收已经发出的每一条通知，它可以    
 *	  轻松地在额外载荷参数中加入某个唯一的字符串。     
 *	                    
 *
 *	  当事务准备提交时，PreCommit_Notify() 把待处理的       
 *	  通知加到队列头部。队列的头指针总是指向下一个     
 *	  空闲位置，而一个位置只是一个页号与页内偏移量。 
 *	  这是在 clog 中把事务标记为已提交之前完成               
 *	  的。如果写入通知时遇到问题，我们仍可调用         
 *	  elog(ERROR, ...)，事务将会回滚。                             
 *	                 
 *
 *	  一旦我们把所有通知都放入队列，就返回到             
 *	  CommitTransaction()，由它执行实际的事务提交。           
 *
 *	  提交之后我们会再次被调用（AtCommit_Notify()）。      
 *	  在这里我们对有效监听状态（listenChannels）进行         
 *	  实际的更新。然后向可能对我们消息感兴趣的任   
 *	  何后端发信号（如果正在监听，包括我们自  
 *	  己的后端）。这由 SignalBackends() 来完成，它扫描         
 *	  监听后端的列表，并向每个监听后端发送              
 *	  PROCSIG_NOTIFY_INTERRUPT 信号（我们不知道哪个                
 *	  后端在监听哪个通道，所以必须给它们都发信号）。  
 *	  不过，已是最新的后端可以排除，也可以排            
 *	  除处于其他库的后端（除非落后太多，得踢     
 *	  它）。 
 *
 *	  最后，当我们完全退出事务并即将空闲时，我们扫描  
 *	  队列中需要发给前端的消息（可能是其他后端发 
 *	  来的通知，也可能是来自我们自己的自通知）。这     
 *	  一步不属于 CommitTransaction 序列，原因有两个重要       
 *	  方面。首先，向我们的前端发送数据时可能出错，  
 *	  而提交后清理中发生错误是非常糟糕的。其次，  
 *	  在过程于单个前端命令内发出多次提交的情况下，     
 *	  我们不希望在命令完成之前向我们的前端发送通        
 *	  知；但发给其他后端的通知应在每次提交之后      
 *	  立即发出。                             
 *
 * 5. 收到 PROCSIG_NOTIFY_INTERRUPT 信号后，信号处理             
 *	  程序会设置进程的 latch；若此后端空闲（即正在   
 *	  等待前端命令且不在事务块内），事件就会被立即     
 *	  处理。参见 ProcessClientReadInterrupt()）。 
 *	  否则处理程序可能只设置一个标志，这将使处理在  
 *	  我们下次空闲之前进行。                                    
 *	       
 *
 *	  入站通知处理包括读取自上次扫描以来已到达的所    
 *	  有通知。我们读取每一条通知，直到到达来自未提    
 *	  交事务的通知，或者到达头指针的位置                    
 *	  为止。                   
 *
 * 6. 为了避免 SLRU 回绕并限制磁盘空间消耗，需要推    
 *	  进尾指针，以便截断旧页面。这相对昂贵（值得 
 *	  注意的是它需要独占锁），所以我们不想经常这样    
 *	  做。我们让发送后端在把队列头推进到新页面时做     
 *	  这项工作，但每 QUEUE_CLEANUP_DELAY 页只做一次           
 *	                            
 *
 * 在它所通知的同一通道上监听的应用程序会收到针  
 * 对其自身 NOTIFY 的 NOTIFY 消息。如果没有用处，可          
 * 以通过比较 NOTIFY 消息中的 be_pid 与应用程序自身            
 * 后端的 PID 来忽略它们。（自 FE/BE 协议 2.0 起，       
 * 后端的 PID 在启动期间就提供给前端。）上述设计保     
 * 证忽略自通知不会漏掉来自其他后端的通知。  
 *
 * 用于通知管理的共享内存量（NUM_NOTIFY_BUFFERS）可            
 * 以变化而不影响除性能以外的任何东西。一次可以    
 * 排队的最大通知数据量由 slru.c 的回绕限制决定；       
 * 参见下文的 QUEUE_MAX_PAGE。                      
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <unistd.h>
#include <signal.h>

#include "access/parallel.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "commands/async.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"


/*
 * NOTIFY 负载的最大大小，包括终止 NULL。必须保持足够小以便
 * 通知消息容纳在一个 SLRU 页面中。这里的魔法调节因子并不关键，
 * 只要大于 AsyncQueueEntryEmptySize 即可 --- 我们把它设得
 * 显著更大，这样该数据结构的变更就不会影响用户可见的限制。
*
*
 */
#define NOTIFY_PAYLOAD_MAX_LENGTH	(BLCKSZ - NAMEDATALEN - 128)

/*
 * 表示全局通知队列中条目的结构体
 *
 * 此结构体声明具有最大长度，但实际队列条目中数据区域只够
 * 容纳实际的通道和负载字符串（各自以 null 结尾）。
 * AsyncQueueEntryEmptySize 是最小可能的条目大小，适用于通道
 * 和负载字符串都为空的情况（注意不包括对齐填充）。
 *
 * "length" 字段应始终向上舍入到下一个 QUEUEALIGN 倍数，
 * 以确保所有字段正确对齐。
*
 */
typedef struct AsyncQueueEntry
{
	int			length;			/* 条目的总分配长度 */
	Oid			dboid;			/* 发送者的数据库 OID */
	TransactionId xid;			/* 发送者的 XID */
	int32		srcPid;			/* 发送者的 PID */
	char		data[NAMEDATALEN + NOTIFY_PAYLOAD_MAX_LENGTH];
} AsyncQueueEntry;

/* 当前 AsyncQueueEntry 所有字段均不需要超过 int 对齐 */
#define QUEUEALIGN(len)		INTALIGN(len)

#define AsyncQueueEntryEmptySize	(offsetof(AsyncQueueEntry, data) + 2)

/*
 * 描述队列位置的结构体，以及操作它的相关宏
 */
typedef struct QueuePosition
{
	int			page;			/* SLRU 页码 */
	int			offset;			/* 页面内的字节偏移量 */
} QueuePosition;

#define QUEUE_POS_PAGE(x)		((x).page)
#define QUEUE_POS_OFFSET(x)		((x).offset)

#define SET_QUEUE_POS(x,y,z) \
	do { \
		(x).page = (y); \
		(x).offset = (z); \
	} while (0)

#define QUEUE_POS_EQUAL(x,y) \
	((x).page == (y).page && (x).offset == (y).offset)

#define QUEUE_POS_IS_ZERO(x) \
	((x).page == 0 && (x).offset == 0)

/* 选择逻辑上较小的 QueuePosition */
#define QUEUE_POS_MIN(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (x) : \
	 (x).page != (y).page ? (y) : \
	 (x).offset < (y).offset ? (x) : (y))

/* 选择逻辑上较大的 QueuePosition */
#define QUEUE_POS_MAX(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (y) : \
	 (x).page != (y).page ? (x) : \
	 (x).offset > (y).offset ? (x) : (y))

/*
 * 决定尝试推进尾指针频率的参数：每 QUEUE_CLEANUP_DELAY
 * 页 NOTIFY 数据执行一次。这也是其他数据库中后端需落后多少
 * 才会决定唤醒它来推进其指针的距离。
 *
 * 不要将此值设得非常大。虽然某些地方能节省工作，但其他地方
 * 会增加成本。特别是该值应小于 NUM_NOTIFY_BUFFERS，以确保
 * 后端在需读取的页面从 SLRU 缓存中移除前能够赶上。
*
*
 */
#define QUEUE_CLEANUP_DELAY 4

/*
 * 描述监听后端状态的结构体
 */
typedef struct QueueBackendStatus
{
	int32		pid;			/* PID 或 InvalidPid */
	Oid			dboid;			/* 后端的数据库 OID，或 InvalidOid */
	BackendId	nextListener;	/* 下一个监听者的 ID，或 InvalidBackendId */
	QueuePosition pos;			/* 后端已读取队列到此位置 */
} QueueBackendStatus;

/*
 * LISTEN/NOTIFY 的共享内存状态（不包括 SLRU 相关内容）
 *
 * AsyncQueueControl 结构体受 NotifyQueueLock 和
 * NotifyQueueTailLock 保护。
 *
 * 以 SHARED 模式持有 NotifyQueueLock 时，后端只能检查自己的
 * 条目以及头尾指针。因此可允许后端在仅持有 SHARED 锁的
 * 情况下更新自身记录（因为没有其他后端会检查它）。
 *
 * 以 EXCLUSIVE 模式持有 NotifyQueueLock 时，后端可以检查
 * 其他后端的条目，也可以修改头指针。同时以 EXCLUSIVE 模式
 * 持有 NotifyQueueLock 和 NotifyQueueTailLock 时，后端可以
 * 修改尾指针。
 *
 * NotifySLRULock 用作 pg_notify SLRU 缓冲区的控制锁。为
 * 避免死锁，每当需要多个锁时，先获取 NotifyQueueTailLock，
 * 然后是 NotifyQueueLock，最后是 NotifySLRULock。
 *
 * 每个后端使用 backend[] 数组中索引等于其 BackendId 的
 * 条目（范围从 1 到 MaxBackends）。依赖这一点来使
 * SendProcSignal 快速。
 *
 * 活跃监听后端的 backend[] 数组条目通过 firstListener 和
 * nextListener 链接串联在一起，因此可扫描它们而无需遍历
 * 非活跃条目。按 BackendId 排序此列表，以便有大量活跃
 * 条目时扫描对缓存友好。
*
*
 */
typedef struct AsyncQueueControl
{
	QueuePosition head;			/* head 指向下一个空闲位置 */
	QueuePosition tail;			/* tail 必须 <= 每个监听后端的
					 * 队列位置 */
	int			stopPage;		/* 最旧的未回收页面；必须 <=
					 * tail.page */
	BackendId	firstListener;	/* 第一个监听者的 ID，或 InvalidBackendId */
	TimestampTz lastQueueFillWarn;	/* 上次队列已满消息的时间 */
	QueueBackendStatus backend[FLEXIBLE_ARRAY_MEMBER];
	/* backend[0] 未使用；使用条目从 [1] 到 [MaxBackends] */
} AsyncQueueControl;

static AsyncQueueControl *asyncQueueControl;

#define QUEUE_HEAD					(asyncQueueControl->head)
#define QUEUE_TAIL					(asyncQueueControl->tail)
#define QUEUE_STOP_PAGE				(asyncQueueControl->stopPage)
#define QUEUE_FIRST_LISTENER		(asyncQueueControl->firstListener)
#define QUEUE_BACKEND_PID(i)		(asyncQueueControl->backend[i].pid)
#define QUEUE_BACKEND_DBOID(i)		(asyncQueueControl->backend[i].dboid)
#define QUEUE_NEXT_LISTENER(i)		(asyncQueueControl->backend[i].nextListener)
#define QUEUE_BACKEND_POS(i)		(asyncQueueControl->backend[i].pos)

/*
 * 用于访问通知队列的 SLRU 缓冲区区域
 */
static SlruCtlData NotifyCtlData;

#define NotifyCtl					(&NotifyCtlData)
#define QUEUE_PAGESIZE				BLCKSZ
#define QUEUE_FULL_WARN_INTERVAL	5000	/* 每 5 秒最多警告一次 */

/*
 * 使用段 0000 到 FFFF。每个段包含 SLRU_PAGES_PER_SEGMENT
 * 个页面，即页面从 0 到 SLRU_PAGES_PER_SEGMENT * 0x10000
 * - 1。可用 SlruScanDirectory() 允许的最大段数，但已提供
 * 充足空间，不值得再麻烦。
 *
 * 队列中一次最多 QUEUE_MAX_PAGE/2 页数据，超过会混淆
 * slru.c 令其认为发生回绕。默认 BLCKSZ 下最多 8GB
 * 已排队但未读取的数据。
 *
 * 注意：可将 QUEUE_MAX_PAGE 重定义为
 * SLRU_PAGES_PER_SEGMENT 的较小倍数，便于测试队列满行为。
*
 */
#define QUEUE_MAX_PAGE			(SLRU_PAGES_PER_SEGMENT * 0x10000 - 1)

/*
 * listenChannels 标识实际监听的通道（即已提交 LISTEN
 * 的通道）。它是通道名称的简单列表，分配在
 * TopMemoryContext 中。
 */
static List *listenChannels = NIL;	/* C 字符串列表 */

/*
 * 待处理 LISTEN/UNLISTEN 操作的状态由当前事务中请求的
 * 所有操作的有序列表组成。如上所述，在到达事务提交之前
 * 不实际修改 listenChannels。
 *
 * 该列表保存在 CurTransactionContext 中。在子事务中，
 * 每个子事务在其自己的 CurTransactionContext 中有自己的
 * 列表，但成功的子事务将其列表附加到父事务的列表中。
 * 失败的子事务则丢弃其列表。
 */
typedef enum
{
	LISTEN_LISTEN,
	LISTEN_UNLISTEN,
	LISTEN_UNLISTEN_ALL
} ListenActionKind;

typedef struct
{
	ListenActionKind action;
	char		channel[FLEXIBLE_ARRAY_MEMBER]; /* 以 null 结尾的字符串 */
} ListenAction;

typedef struct ActionList
{
	int			nestingLevel;	/* 当前事务嵌套深度 */
	List	   *actions;		/* ListenAction 结构体列表 */
	struct ActionList *upper;	/* 上层事务级别的详细信息 */
} ActionList;

static ActionList *pendingActions = NULL;

/*
 * 传出通知的状态由当前事务中已被 NOTIFY 的所有通道   
 * +载荷的列表组成。除非且直到事务提交，我们才实际   
 * 执行 NOTIFY。如果当前（子）事务中没有做过              
 * NOTIFY，则 pendingNotifies 为 NULL。                
 *
 * 我们丢弃同一事务中发出的重复通知事件。         
 * 因此，除列表本身外（需要用它跟踪事件的顺           
 * 序，因为我们保证按顺序传递它们），我们还构建     
 * 了一个可以探测以检测重复的哈希表。由于构建哈     
 * 希表有些昂贵，所以只有当当前（子）事务中至少   
 * 排入了 MIN_HASHABLE_NOTIFIES 个事件时才构建它；          
 * 在此之前我们只是线性扫描事件。
 *
 * 该列表保存在 CurTransactionContext 中。子事务            
 * 中，每个子事务在其自己的 CurTransactionContext           
 * 中都有着自己的列表，但成功的子事务会把它们的  
 * 条目加入其父事务的列表。失败的子事务则直接丢   
 * 弃它们的列表。由于这些列表是相互独立的，子事    
 * 务的列表中可能存在与某个祖先（子）事务中的事     
 * 件重复的通知事件；合并时会去掉这些重复         
 *
 * 注意：动作列表和通知列表在一个事务内互不影响。  
 * 特别是，如果一个事务对同一条件名先执行 NOTIFY       
 * 再执行 LISTEN，那么它在提交时会收到一个自通知。    
 * 这有点怪，但与历史行为一致。     
 */
typedef struct Notification
{
	uint16		channel_len;	/* 通道名称字符串的长度 */
	uint16		payload_len;	/* 负载字符串的长度 */
	/* 以 null 结尾的通道名称，后跟以 null 结尾的负载 */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} Notification;

typedef struct NotificationList
{
	int			nestingLevel;	/* 当前事务嵌套深度 */
	List	   *events;			/* Notification 结构体列表 */
	HTAB	   *hashtab;		/* NotificationHash 结构体的哈希表，或 NULL */
	struct NotificationList *upper; /* 上层事务级别的详细信息 */
} NotificationList;

#define MIN_HASHABLE_NOTIFIES 16	/* 构建哈希表的阈值 */

typedef struct NotificationHash
{
	Notification *event;		/* => 实际的 Notification 结构体 */
} NotificationHash;

static NotificationList *pendingNotifies = NULL;

/*
 * 入站通知最初由 HandleNotifyInterrupt() 处理，从信号
 * 处理程序内部调用。它只设置 notifyInterruptPending 标志
 * 并设置进程锁存器。然后在安全处理中断时调用
 * ProcessNotifyInterrupt()。
*
 */
volatile sig_atomic_t notifyInterruptPending = false;

/* 如已注册 on_shmem_exit 清理则为真 */
static bool unlistenExitRegistered = false;

/* 如当前在 asyncQueueControl 中注册为监听者则为真 */
static bool amRegisteredListener = false;

/* 是否已推进到 QUEUE_CLEANUP_DELAY 倍数的页？ */
static bool tryAdvanceTail = false;

/* GUC 参数 */
bool		Trace_notify = false;

/* 本地函数原型 */
static int	asyncQueuePageDiff(int p, int q);
static bool asyncQueuePagePrecedes(int p, int q);
static void queue_listen(ListenActionKind action, const char *channel);
static void Async_UnlistenOnExit(int code, Datum arg);
static void Exec_ListenPreCommit(void);
static void Exec_ListenCommit(const char *channel);
static void Exec_UnlistenCommit(const char *channel);
static void Exec_UnlistenAllCommit(void);
static bool IsListeningOn(const char *channel);
static void asyncQueueUnregister(void);
static bool asyncQueueIsFull(void);
static bool asyncQueueAdvance(volatile QueuePosition *position, int entryLength);
static void asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe);
static ListCell *asyncQueueAddEntries(ListCell *nextNotify);
static double asyncQueueUsage(void);
static void asyncQueueFillWarning(void);
static void SignalBackends(void);
static void asyncQueueReadAllNotifications(void);
static bool asyncQueueProcessPageEntries(volatile QueuePosition *current,
										 QueuePosition stop,
										 char *page_buffer,
										 Snapshot snapshot);
static void asyncQueueAdvanceTail(void);
static void ProcessIncomingNotify(bool flush);
static bool AsyncExistsPendingNotify(Notification *n);
static void AddEventToPendingNotifies(Notification *n);
static uint32 notification_hash(const void *key, Size keysize);
static int	notification_match(const void *key1, const void *key2, Size keysize);
static void ClearPendingActionsAndNotifies(void);

/*
 * 计算两个队列页码之间的差值（即 p - q），考虑回绕。
*
 */
static int
asyncQueuePageDiff(int p, int q)
{
	int			diff;

	/*
		 * 必须对 (QUEUE_MAX_PAGE+1)/2 取模比较。两个输入
		 * 应在 0..QUEUE_MAX_PAGE 范围内。
		 */
	Assert(p >= 0 && p <= QUEUE_MAX_PAGE);
	Assert(q >= 0 && q <= QUEUE_MAX_PAGE);

	diff = p - q;
	if (diff >= ((QUEUE_MAX_PAGE + 1) / 2))
		diff -= QUEUE_MAX_PAGE + 1;
	else if (diff < -((QUEUE_MAX_PAGE + 1) / 2))
		diff += QUEUE_MAX_PAGE + 1;
	return diff;
}

/*
 * 考虑回绕时 p < q 吗？
 *
 * 由于 asyncQueueIsFull() 阻止创建可能领先于任何现有
 * 页面的页面，不需要评估页面内的条目。
 */
static bool
asyncQueuePagePrecedes(int p, int q)
{
	return asyncQueuePageDiff(p, q) < 0;
}

/*
 * 报告共享内存区域所需的空间
 */
Size
AsyncShmemSize(void)
{
	Size		size;

	/* 最好与 AsyncShmemInit 匹配 */
	size = mul_size(MaxBackends + 1, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	size = add_size(size, SimpleLruShmemSize(NUM_NOTIFY_BUFFERS, 0));

	return size;
}

/*
 * 初始化共享内存区域
 */
void
AsyncShmemInit(void)
{
	bool		found;
	Size		size;

	/*
		 * 创建或附加到 AsyncQueueControl 结构体。
		 *
		 * backend[] 数组中使用条目从 1 到 MaxBackends；
		 * 第零个条目未使用但必须分配。
		 */
	size = mul_size(MaxBackends + 1, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	asyncQueueControl = (AsyncQueueControl *)
		ShmemInitStruct("Async Queue Control", size, &found);

	if (!found)
	{
		/* 首次使用，初始化它 */
		SET_QUEUE_POS(QUEUE_HEAD, 0, 0);
		SET_QUEUE_POS(QUEUE_TAIL, 0, 0);
		QUEUE_STOP_PAGE = 0;
		QUEUE_FIRST_LISTENER = InvalidBackendId;
		asyncQueueControl->lastQueueFillWarn = 0;
		/* 第零个条目不会被使用，但还是初始化一下 */
		for (int i = 0; i <= MaxBackends; i++)
		{
			QUEUE_BACKEND_PID(i) = InvalidPid;
			QUEUE_BACKEND_DBOID(i) = InvalidOid;
			QUEUE_NEXT_LISTENER(i) = InvalidBackendId;
			SET_QUEUE_POS(QUEUE_BACKEND_POS(i), 0, 0);
		}
	}

	/*
		 * 设置 pg_notify 数据的 SLRU 管理。
		 */
	NotifyCtl->PagePrecedes = asyncQueuePagePrecedes;
	SimpleLruInit(NotifyCtl, "Notify", NUM_NOTIFY_BUFFERS, 0,
				  NotifySLRULock, "pg_notify", LWTRANCHE_NOTIFY_BUFFER,
				  SYNC_HANDLER_NONE);

	if (!found)
	{
		/*
			 * 启动或重启期间，清理 pg_notify 目录。
			 */
		(void) SlruScanDirectory(NotifyCtl, SlruScanDirCbDeleteAll, NULL);
	}
}


/*
 * pg_notify -
 *	  发送通知事件的 SQL 函数
 */
Datum
pg_notify(PG_FUNCTION_ARGS)
{
	const char *channel;
	const char *payload;

	if (PG_ARGISNULL(0))
		channel = "";
	else
		channel = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (PG_ARGISNULL(1))
		payload = "";
	else
		payload = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* 对于作为语句的 NOTIFY，这在 ProcessUtility 中检查 */
	PreventCommandDuringRecovery("NOTIFY");

	Async_Notify(channel, payload);

	PG_RETURN_VOID();
}


/*
 * Async_Notify
 *
 *		由 SQL notify 命令执行。
 *
 *		将消息添加到待处理通知列表。
 *		实际通知在事务提交期间发生。
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 */
void
Async_Notify(const char *channel, const char *payload)
{
	int			my_level = GetCurrentTransactionNestLevel();
	size_t		channel_len;
	size_t		payload_len;
	Notification *n;
	MemoryContext oldcontext;

	if (IsParallelWorker())
		elog(ERROR, "cannot send notifications from a parallel worker");

	if (Trace_notify)
		elog(DEBUG1, "Async_Notify(%s)", channel);

	channel_len = channel ? strlen(channel) : 0;
	payload_len = payload ? strlen(payload) : 0;

	/* 必须指定通道名称 */
	if (channel_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name cannot be empty")));

	/* 强制执行长度限制 */
	if (channel_len >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name too long")));

	if (payload_len >= NOTIFY_PAYLOAD_MAX_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("payload string too long")));

	/*
		 * 必须构造 Notification 条目，即使最终不使用，
		 * 以便与现有列表条目进行廉价比较。
		 *
		 * 通知列表需存活到事务结束，因此存储在事务上下文中。
		*
		 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	n = (Notification *) palloc(offsetof(Notification, data) +
								channel_len + payload_len + 2);
	n->channel_len = channel_len;
	n->payload_len = payload_len;
	strcpy(n->data, channel);
	if (payload)
		strcpy(n->data + channel_len + 1, payload);
	else
		n->data[channel_len + 1] = '\0';

	if (pendingNotifies == NULL || my_level > pendingNotifies->nestingLevel)
	{
		NotificationList *notifies;

		/*
			 * 当前（子）事务中的第一个通知事件。注意在
			 * TopTransactionContext 中分配 NotificationList；
			 * nestingLevel 可能在之后被 AtSubCommit_Notify 修改。
			 */
		notifies = (NotificationList *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(NotificationList));
		notifies->nestingLevel = my_level;
		notifies->events = list_make1(n);
		/* 肯定还不需要哈希表 */
		notifies->hashtab = NULL;
		notifies->upper = pendingNotifies;
		pendingNotifies = notifies;
	}
	else
	{
		/* 现在检查重复 */
		if (AsyncExistsPendingNotify(n))
		{
			/* 是重复的，丢弃 */
			pfree(n);
			MemoryContextSwitchTo(oldcontext);
			return;
		}

		/* 将更多事件追加到现有列表 */
		AddEventToPendingNotifies(n);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * queue_listen
 *		listen、unlisten、unlisten all 命令的公共代码。
 *
 *		将请求添加到待处理操作列表。
 *		listenChannels 列表的实际更新发生在事务提交期间。
*
 */
static void
queue_listen(ListenActionKind action, const char *channel)
{
	MemoryContext oldcontext;
	ListenAction *actrec;
	int			my_level = GetCurrentTransactionNestLevel();

	/*
		 * 与 Async_Notify 不同，不尝试折叠重复。确保冲突的
		 * LISTEN/UNLISTEN/UNLISTEN_ALL 正确交互太复杂，而且
		 * 在合理应用中不太可能有性能收益。
		*
		 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	/* sizeof(ListenAction) 中包含终止 null 的空间 */
	actrec = (ListenAction *) palloc(offsetof(ListenAction, channel) +
									 strlen(channel) + 1);
	actrec->action = action;
	strcpy(actrec->channel, channel);

	if (pendingActions == NULL || my_level > pendingActions->nestingLevel)
	{
		ActionList *actions;

		/*
			 * 当前子事务中的第一个操作。注意在
			 * TopTransactionContext 中分配 ActionList；
			 * nestingLevel 可能在之后被 AtSubCommit_Notify 修改。
			 */
		actions = (ActionList *)
			MemoryContextAlloc(TopTransactionContext, sizeof(ActionList));
		actions->nestingLevel = my_level;
		actions->actions = list_make1(actrec);
		actions->upper = pendingActions;
		pendingActions = actions;
	}
	else
		pendingActions->actions = lappend(pendingActions->actions, actrec);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Async_Listen
 *
 *		由 SQL listen 命令执行。
 */
void
Async_Listen(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Listen(%s,%d)", channel, MyProcPid);

	queue_listen(LISTEN_LISTEN, channel);
}

/*
 * Async_Unlisten
 *
 *		由 SQL unlisten 命令执行。
 */
void
Async_Unlisten(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Unlisten(%s,%d)", channel, MyProcPid);

	/* 如不可能在监听，则无需排队 */
	if (pendingActions == NULL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN, channel);
}

/*
 * Async_UnlistenAll
 *
 *		由 UNLISTEN * 命令调用，也在后端退出时调用。
 */
void
Async_UnlistenAll(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_UnlistenAll(%d)", MyProcPid);

	/* 如不可能在监听，则无需排队 */
	if (pendingActions == NULL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN_ALL, "");
}

/*
 * SQL 函数：返回此后端正在主动监听的通道名称集合。  
 * 。          
 *
 * 注意：此处的编码依赖于 listenChannels 列表在一个事务  
 * 内不能改变的事实。 
 */
Datum
pg_listening_channels(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	/* 仅在函数首次调用时执行的操作 */
	if (SRF_IS_FIRSTCALL())
	{
		/* 创建用于跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();
	}

	/* 每次函数调用时执行的操作 */
	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < list_length(listenChannels))
	{
		char	   *channel = (char *) list_nth(listenChannels,
												funcctx->call_cntr);

		SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(channel));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Async_UnlistenOnExit
 *
 * 如在此后端中执行过任何 LISTEN，则在后端退出时执行。
 * 如用户已 UNLISTEN 所有内容，可能不再需要，但不尝试
 * 检测这种情况。
 */
static void
Async_UnlistenOnExit(int code, Datum arg)
{
	Exec_UnlistenAllCommit();
	asyncQueueUnregister();
}

/*
 * AtPrepare_Notify
 *
 *		在两阶段事务的准备阶段调用。保存状态以供
 *		稍后可能的提交使用。
 */
void
AtPrepare_Notify(void)
{
	/* 不允许有任何待处理的 LISTEN/UNLISTEN/NOTIFY 操作 */
	if (pendingActions || pendingNotifies)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has executed LISTEN, UNLISTEN, or NOTIFY")));
}

/*
 * PreCommit_Notify
 *
 *		在事务提交时调用，在实际提交到 clog 之前。       
 *		     
 *
 *		如果有待处理的 LISTEN 动作，确保我们被列入共享  
 *		内存监听者数组中。这必须发生在提交之前，以 
 *		确保不错过任何来自恰好在我们之后提交的事   
 *		务的通知。 
 *
 *		如果 pendingNotifies 列表中有向外发送的通知请求， 
 *		就将其加入全局队列。在提交之前这样做，以便
 *		在队列空间不足时仍能抛出错误。         
 */
void
PreCommit_Notify(void)
{
	ListCell   *p;

	if (!pendingActions && !pendingNotifies)
		return;					/* 此事务中无相关语句 */

	if (Trace_notify)
		elog(DEBUG1, "PreCommit_Notify");

	/* 预检查任何待处理的 listen/unlisten 操作 */
	if (pendingActions != NULL)
	{
		foreach(p, pendingActions->actions)
		{
			ListenAction *actrec = (ListenAction *) lfirst(p);

			switch (actrec->action)
			{
				case LISTEN_LISTEN:
					Exec_ListenPreCommit();
					break;
				case LISTEN_UNLISTEN:
					/* 没有 Exec_UnlistenPreCommit() */
					break;
				case LISTEN_UNLISTEN_ALL:
					/* 没有 Exec_UnlistenAllPreCommit() */
					break;
			}
		}
	}

	/* 排队任何待处理的通知（必须在上述操作之后） */
	if (pendingNotifies)
	{
		ListCell   *nextNotify;

		/*
			 * 确保当前事务已分配 XID。已有 XID 时
			 * GetCurrentTransactionId 很便宜，否则不然，
			 * 希望不在持有 NotifyQueueLock 时做这项工作。
			*
			 */
		(void) GetCurrentTransactionId();

		/*
		 * 通过获取持有到提交之后的特殊锁来串行化写入
		 * 者。这确保队列条目按提交顺序出现，特别      
		 * 是绝不会存在未提交的队列条目排在已提交条目
		 * 之前，因此未提交事务无法阻塞可投递通知的  
		 * 投递。                                         
		 *
		 * 我们使用重量级锁，使其在提交或中止后被自动  
		 * 释放。这也使得死锁能够被检测出来，尽管实际
		 * 上这里不应该发生死锁。                             
		 *
		 * 锁加在 "database 0" 上，这很难看，但似乎不值得  
		 * 专门为此发明一个特殊的锁标签类别。           
		 * （历史说明：PG 9.0 之前，"database 0" 上类似的锁被  
		 * flatfiles 机制所使用。）  
		 */
		LockSharedObject(DatabaseRelationId, InvalidOid, 0,
						 AccessExclusiveLock);

		/* 现在将通知推入队列 */
		nextNotify = list_head(pendingNotifies->events);
		while (nextNotify != NULL)
		{
			/*
				 * 将待处理通知添加到队列。每页获取和释放
				 * NotifyQueueLock 一次，这可能过度，但确实
				 * 允许读取者在此期间进入。
				 *
				 * 队列满非常罕见，考虑到 SLRU 页面有大量
				 * 可用空间，实际上不应发生。然而需要处理这
				 * 种可能。注意，到达此处时正在提交事务，
				 * 但尚未提交到 clog，因此此时仍可回滚事务。
				*
				*
				 */
			LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
			asyncQueueFillWarning();
			if (asyncQueueIsFull())
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many notifications in the NOTIFY queue")));
			nextNotify = asyncQueueAddEntries(nextNotify);
			LWLockRelease(NotifyQueueLock);
		}

		/* 注意，不清理 pendingNotifies；AtCommit_Notify 会处理 */
	}
}

/*
 * AtCommit_Notify
 *
 *		在事务提交时调用，在提交到 clog 之后。
 *
 *		更新 listenChannels 并清除事务本地状态。
 *
 *		如我们在事务中发出了通知，向监听后端（可能
 *		包括我们自己）发送信号来处理它们。另外，如用
 *		新通知填充了足够的队列页面，尝试推进队列尾指针。
*
 */
void
AtCommit_Notify(void)
{
	ListCell   *p;

	/*
		 * 允许未执行 LISTEN/UNLISTEN/NOTIFY 的事务尽快返回
		*
		 */
	if (!pendingActions && !pendingNotifies)
		return;

	if (Trace_notify)
		elog(DEBUG1, "AtCommit_Notify");

	/* 执行任何待处理的 listen/unlisten 操作 */
	if (pendingActions != NULL)
	{
		foreach(p, pendingActions->actions)
		{
			ListenAction *actrec = (ListenAction *) lfirst(p);

			switch (actrec->action)
			{
				case LISTEN_LISTEN:
					Exec_ListenCommit(actrec->channel);
					break;
				case LISTEN_UNLISTEN:
					Exec_UnlistenCommit(actrec->channel);
					break;
				case LISTEN_UNLISTEN_ALL:
					Exec_UnlistenAllCommit();
					break;
			}
		}
	}

	/* 如不再监听任何内容，退出监听者数组 */
	if (amRegisteredListener && listenChannels == NIL)
		asyncQueueUnregister();

	/*
		 * 向监听后端发送信号。仅当存在待处理通知时需
		 * 这样做，这些通知之前由 PreCommit_Notify() 添加
		 * 到了共享队列。
		 */
	if (pendingNotifies != NULL)
		SignalBackends();

	/*
		 * 如到了尝试推进全局尾指针的时候，执行此操作。
		 *
		 * （在发送者中执行此操作似乎奇怪，因为监听者
		 * 很可能尚未读取刚发送的消息。但只有发送者执行
		 * 此操作争用更少，且推进全局尾部无紧迫性。因此
		 * 这通常会清理较早发送的消息。）
		*
		 */
	if (tryAdvanceTail)
	{
		tryAdvanceTail = false;
		asyncQueueAdvanceTail();
	}

	/* 然后清理 */
	ClearPendingActionsAndNotifies();
}

/*
 * Exec_ListenPreCommit --- PreCommit_Notify 的子例程
 *
 * 此函数必须确保准备好接收任何传入的消息。
 */
static void
Exec_ListenPreCommit(void)
{
	QueuePosition head;
	QueuePosition max;
	BackendId	prevListener;

	/*
		 * 如已在监听某些内容，或已在此事务中运行过
		 * 此例程，则无需执行任何操作。
		 */
	if (amRegisteredListener)
		return;

	if (Trace_notify)
		elog(DEBUG1, "Exec_ListenPreCommit(%d)", MyProcPid);

	/*
		 * 注册前，确保退出前取消监听。（注意：如稍后
		 * 中止，此操作不会被撤销。）
		 */
	if (!unlistenExitRegistered)
	{
		before_shmem_exit(Async_UnlistenOnExit, 0);
		unlistenExitRegistered = true;
	}

	/*
	 * 这是首个 LISTEN，需建立我们的指针。  
	 *
	 * 我们把指针设为全局尾指针，然后把它向前移          
	 * 动越过已经提交的通知。这确保我们不会错过任何    
	 * 尚未提交的通知。可能多收到几条，但那无害。   
	 *              
	 *
	 * 在某些场景中，可能有很多已提交却尚未被清除的    
	 * 通知（因为某个后端在读取它们方面偷懒）。为了     
	 * 减少启动时间，我们可以查看其他后端，并采用 
	 * 我们数据库中任何后端的最大"pos"指针；它             
	 * 已经越过推进过去的通知肯定都已提交，不需要我   
	 * 们重新检查。（我们必须只考虑连接到我们数据库  
	 * 的后端，因为别的后端不会费心检查我们数据库       
	 * 通知的已提交性。）                      
	 *
	 * 这里需要独占锁，这样我们才能查看其他后端的条   
	 * 目并操作列表链接。   
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	head = QUEUE_HEAD;
	max = QUEUE_TAIL;
	prevListener = InvalidBackendId;
	for (BackendId i = QUEUE_FIRST_LISTENER; i > 0; i = QUEUE_NEXT_LISTENER(i))
	{
		if (QUEUE_BACKEND_DBOID(i) == MyDatabaseId)
			max = QUEUE_POS_MAX(max, QUEUE_BACKEND_POS(i));
		/* 同时找到此后端之前的最后一个监听后端 */
		if (i < MyBackendId)
			prevListener = i;
	}
	QUEUE_BACKEND_POS(MyBackendId) = max;
	QUEUE_BACKEND_PID(MyBackendId) = MyProcPid;
	QUEUE_BACKEND_DBOID(MyBackendId) = MyDatabaseId;
	/* 将后端插入监听者列表的正确位置 */
	if (prevListener > 0)
	{
		QUEUE_NEXT_LISTENER(MyBackendId) = QUEUE_NEXT_LISTENER(prevListener);
		QUEUE_NEXT_LISTENER(prevListener) = MyBackendId;
	}
	else
	{
		QUEUE_NEXT_LISTENER(MyBackendId) = QUEUE_FIRST_LISTENER;
		QUEUE_FIRST_LISTENER = MyBackendId;
	}
	LWLockRelease(NotifyQueueLock);

	/* 现在已列在全局数组中，记住正在监听 */
	amRegisteredListener = true;

	/*
		 * 尝试尽可能向前移动指针。将跳过已提交通知，
		 * 它们可能很陈旧了。注意，尚未监听任何内容，
		 * 因此不会将这些通知传递给前端。此外，尽管
		 * 事务可能执行了 NOTIFY，这些消息尚未入队，
		 * 因此不会跳过它们。
		*
		 */
	if (!QUEUE_POS_EQUAL(max, head))
		asyncQueueReadAllNotifications();
}

/*
 * Exec_ListenCommit --- AtCommit_Notify 的子例程
 *
 * 将通道添加到我正在监听的通道列表中。
 */
static void
Exec_ListenCommit(const char *channel)
{
	MemoryContext oldcontext;

	/* 如已在监听此通道则什么都不做 */
	if (IsListeningOn(channel))
		return;

	/*
	 * 将新通道名加入 listenChannels。    
	 *
	 * XXX 理论上讲，此处可能出现内存不足而失败的情况，
	 * 这会很糟糕，因为我们已经提交。目前看来似乎不 
	 * 值得设法防范，但也许以后可以对此加以改进           
	 * 。   
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	listenChannels = lappend(listenChannels, pstrdup(channel));
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Exec_UnlistenCommit --- AtCommit_Notify 的子例程
 *
 * 从 listenChannels 中移除指定的通道名称。
 */
static void
Exec_UnlistenCommit(const char *channel)
{
	ListCell   *q;

	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenCommit(%s,%d)", channel, MyProcPid);

	foreach(q, listenChannels)
	{
		char	   *lchan = (char *) lfirst(q);

		if (strcmp(lchan, channel) == 0)
		{
			listenChannels = foreach_delete_current(listenChannels, q);
			pfree(lchan);
			break;
		}
	}

	/*
		 * 对取消监听不在监听的内容不作抱怨；
		 * 该抱怨吗？
		 */
}

/*
 * Exec_UnlistenAllCommit --- AtCommit_Notify 的子例程
 *
 *		取消此后端在所有通道上的监听。
 */
static void
Exec_UnlistenAllCommit(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenAllCommit(%d)", MyProcPid);

	list_free_deep(listenChannels);
	listenChannels = NIL;
}

/*
 * 测试是否在主动监听给定的通道名称。
 *
 * 注意：对队列中找到的每个通知执行此函数。
 * 也许值得进一步优化，例如将列表转换为排序数组
 * 以便二分查找。不过实践中列表可能相当短。
*
 */
static bool
IsListeningOn(const char *channel)
{
	ListCell   *p;

	foreach(p, listenChannels)
	{
		char	   *lchan = (char *) lfirst(p);

		if (strcmp(lchan, channel) == 0)
			return true;
	}
	return false;
}

/*
 * 不再监听任何通道时从监听者数组中移除条目。
 * 注意：如已不在监听则不能失败。
 */
static void
asyncQueueUnregister(void)
{
	Assert(listenChannels == NIL);	/* 否则调用者错误 */

	if (!amRegisteredListener)	/* 无事可做 */
		return;

	/*
		 * 此处需独占锁以操作列表链接。
		 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	/* 标记条目为无效 */
	QUEUE_BACKEND_PID(MyBackendId) = InvalidPid;
	QUEUE_BACKEND_DBOID(MyBackendId) = InvalidOid;
	/* 并将其从列表中移除 */
	if (QUEUE_FIRST_LISTENER == MyBackendId)
		QUEUE_FIRST_LISTENER = QUEUE_NEXT_LISTENER(MyBackendId);
	else
	{
		for (BackendId i = QUEUE_FIRST_LISTENER; i > 0; i = QUEUE_NEXT_LISTENER(i))
		{
			if (QUEUE_NEXT_LISTENER(i) == MyBackendId)
			{
				QUEUE_NEXT_LISTENER(i) = QUEUE_NEXT_LISTENER(MyBackendId);
				break;
			}
		}
	}
	QUEUE_NEXT_LISTENER(MyBackendId) = InvalidBackendId;
	LWLockRelease(NotifyQueueLock);

	/* 标记不再列在全局数组中 */
	amRegisteredListener = false;
}

/*
 * 测试是否有空间插入更多通知消息。
 *
 * 调用者必须至少持有共享的 NotifyQueueLock。
 */
static bool
asyncQueueIsFull(void)
{
	int			nexthead;
	int			boundary;

	/*
		 * 如创建新头页面会导致页面在逻辑上领先于
		 * 当前全局尾指针（即头指针相对于尾部回绕），
		 * 则队列已满。不能创建此类头页面以免混淆
		 * slru.c。为安全起见，将尾指针舍入回段边界
		 * （asyncQueueAdvanceTail 中的截断逻辑不做此，
		 * 因此此处可选）。
		 *
		 * 注意，此测试*不*依赖当前头页面空间。这是
		 * 必要的，因 asyncQueueAddEntries 可能在任何
		 * 情况下尝试创建下一个头页面。
		 */
	nexthead = QUEUE_POS_PAGE(QUEUE_HEAD) + 1;
	if (nexthead > QUEUE_MAX_PAGE)
		nexthead = 0;			/* 回绕 */
	boundary = QUEUE_STOP_PAGE;
	boundary -= boundary % SLRU_PAGES_PER_SEGMENT;
	return asyncQueuePagePrecedes(nexthead, boundary);
}

/*
 * 假设当前条目长度为 entryLength，将 QueuePosition
 * 推进到下一个条目。如跳转到新页面返回 true，否则 false。
*
 */
static bool
asyncQueueAdvance(volatile QueuePosition *position, int entryLength)
{
	int			pageno = QUEUE_POS_PAGE(*position);
	int			offset = QUEUE_POS_OFFSET(*position);
	bool		pageJump = false;

	/*
		 * 移至下一写入位置：首先跳过刚写入或读取的内容。
		*
		 */
	offset += entryLength;
	Assert(offset <= QUEUE_PAGESIZE);

	/*
		 * 第二步，检查页面是否可写入另一条目。如可以，
		 * 留在此处，已到达下一个位置。如不可以，需移动
		 * 到下一页。
		 */
	if (offset + QUEUEALIGN(AsyncQueueEntryEmptySize) > QUEUE_PAGESIZE)
	{
		pageno++;
		if (pageno > QUEUE_MAX_PAGE)
			pageno = 0;			/* 回绕 */
		offset = 0;
		pageJump = true;
	}

	SET_QUEUE_POS(*position, pageno, offset);
	return pageJump;
}

/*
 * 用传出通知消息填充 *qe 处的 AsyncQueueEntry。
 */
static void
asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe)
{
	size_t		channellen = n->channel_len;
	size_t		payloadlen = n->payload_len;
	int			entryLength;

	Assert(channellen < NAMEDATALEN);
	Assert(payloadlen < NOTIFY_PAYLOAD_MAX_LENGTH);

	/* 终止符已包含在 AsyncQueueEntryEmptySize 中 */
	entryLength = AsyncQueueEntryEmptySize + payloadlen + channellen;
	entryLength = QUEUEALIGN(entryLength);
	qe->length = entryLength;
	qe->dboid = MyDatabaseId;
	qe->xid = GetCurrentTransactionId();
	qe->srcPid = MyProcPid;
	memcpy(qe->data, n->data, channellen + payloadlen + 2);
}

/*
 * 将待处理的通知添加到队列。
 *
 * 逐页处理，即一旦需转到新页面就停止，但会被再次
 * 调用填充下一页。如条目不适合当前页面，写入带有
 * InvalidOid 作为数据库 OID 的虚拟条目填充页面。因此
 * 每个页面总是用至最后一个字节，简化稍后读取。
 *
 * 被传入包含下一个要写通知的列表单元（在
 * pendingNotifies->events 中），返回第一个仍未写的
 * 单元。最终返回 NULL 表示全部完成。
 *
 * 已从调用者持有 NotifyQueueLock，并在此函数中本地
 * 获取 NotifySLRULock。
*
 */
static ListCell *
asyncQueueAddEntries(ListCell *nextNotify)
{
	AsyncQueueEntry qe;
	QueuePosition queue_head;
	int			pageno;
	int			offset;
	int			slotno;

	/* 此操作期间同时持有 NotifyQueueLock 和 NotifySLRULock */
	LWLockAcquire(NotifySLRULock, LW_EXCLUSIVE);

	/*
		 * 使用 QUEUE_HEAD 本地副本，退出时写回共享内存。
		 * 原因是如需推进到新页面，SimpleLruZeroPage 可能
		 * 失败（如磁盘空间不足），失败则不能推进
		 * QUEUE_HEAD。（否则后续插入会尝试将条目放入
		 * slru.c 认为尚不存在的页面。）因此使用本地位置
		 * 变量。注意如确实失败，已插入的队列条目被遗忘；
		 * 这可以，事务回滚后它们无用。
		*
		*
		 */
	queue_head = QUEUE_HEAD;

	/*
		 * 如这是 postmaster 启动以来首次写入，需初始化
		 * 异步 SLRU 第一页。否则当前页应已初始化，只需
		 * 获取。
		 *
		 * （SLRU 位置刚回绕时也可走第一条路径，但重新
		 * 清零页面无害。）
		 */
	pageno = QUEUE_POS_PAGE(queue_head);
	if (QUEUE_POS_IS_ZERO(queue_head))
		slotno = SimpleLruZeroPage(NotifyCtl, pageno);
	else
		slotno = SimpleLruReadPage(NotifyCtl, pageno, true,
								   InvalidTransactionId);

	/* 注意写前将页面标记为脏 */
	NotifyCtl->shared->page_dirty[slotno] = true;

	while (nextNotify != NULL)
	{
		Notification *n = (Notification *) lfirst(nextNotify);

		/* 在局部变量 qe 中构造有效队列条目 */
		asyncQueueNotificationToEntry(n, &qe);

		offset = QUEUE_POS_OFFSET(queue_head);

		/* 检查条目是否真正适合当前页面 */
		if (offset + qe.length <= QUEUE_PAGESIZE)
		{
			/* 好的，将 nextNotify 推进过此条目 */
			nextNotify = lnext(pendingNotifies->events, nextNotify);
		}
		else
		{
			/*
				 * 写入虚拟条目填满页面。实际上读取者
				 * 只检查 dboid，因不匹配任何读取者的
				 * 数据库 OID，会忽略此条目继续。
				 */
			qe.length = QUEUE_PAGESIZE - offset;
			qe.dboid = InvalidOid;
			qe.data[0] = '\0';	/* 空通道 */
			qe.data[1] = '\0';	/* 空负载 */
		}

		/* 现在将 qe 复制到共享缓冲区页面 */
		memcpy(NotifyCtl->shared->page_buffer[slotno] + offset,
			   &qe,
			   qe.length);

		/* 适当推进 queue_head，检测页面是否已满 */
		if (asyncQueueAdvance(&(queue_head), qe.length))
		{
			/*
				 * 页面已满，在此完成，但首先用零填充下一页。
				 * 这是为确保 slru.c 对头页面的理解始终与
				 * 我们一致，避免 SimpleLruTruncate 中的边界
				 * 问题。asyncQueueIsFull() 的测试确保有空间
				 * 创建此页面而不溢出队列。
				*
				 */
			slotno = SimpleLruZeroPage(NotifyCtl, QUEUE_POS_PAGE(queue_head));

			/*
				 * 如新页面地址是 QUEUE_CLEANUP_DELAY 的
				 * 倍数，设置标志以记住应尝试推进尾指针
				 * （不想在此处实际做）。
				 */
			if (QUEUE_POS_PAGE(queue_head) % QUEUE_CLEANUP_DELAY == 0)
				tryAdvanceTail = true;

			/* 并退出循环 */
			break;
		}
	}

	/* 成功，更新全局 QUEUE_HEAD */
	QUEUE_HEAD = queue_head;

	LWLockRelease(NotifySLRULock);

	return nextNotify;
}

/*
 * 返回当前通知队列占用比例的 SQL 函数。
*
 */
Datum
pg_notification_queue_usage(PG_FUNCTION_ARGS)
{
	double		usage;

	/* 推进队列尾部以不报告过大结果 */
	asyncQueueAdvanceTail();

	LWLockAcquire(NotifyQueueLock, LW_SHARED);
	usage = asyncQueueUsage();
	LWLockRelease(NotifyQueueLock);

	PG_RETURN_FLOAT8(usage);
}

/*
 * 返回当前占用的队列比例。
 *
 * 调用者必须（至少）以共享模式持有 NotifyQueueLock。
 *
 * 注意：测量到逻辑尾页面的距离，而非物理尾页面。
 * 某种意义上这不正确，但物理尾部位置受 SLRU 段边界
 * 等细节影响，基于此的结果会非常不稳定。
*
 */
static double
asyncQueueUsage(void)
{
	int			headPage = QUEUE_POS_PAGE(QUEUE_HEAD);
	int			tailPage = QUEUE_POS_PAGE(QUEUE_TAIL);
	int			occupied;

	occupied = headPage - tailPage;

	if (occupied == 0)
		return (double) 0;		/* 常见情况的快速退出 */

	if (occupied < 0)
	{
		/* 头部已回绕，尾部尚未 */
		occupied += QUEUE_MAX_PAGE + 1;
	}

	return (double) occupied / (double) ((QUEUE_MAX_PAGE + 1) / 2);
}

/*
 * 检查队列是否至少一半满，如是则发出警告。
 *
 * 考虑到队列大小这不太可能但有可能。
 * 警告最多每 QUEUE_FULL_WARN_INTERVAL 显示一次。
 *
 * 调用者必须持有独占 NotifyQueueLock。
 */
static void
asyncQueueFillWarning(void)
{
	double		fillDegree;
	TimestampTz t;

	fillDegree = asyncQueueUsage();
	if (fillDegree < 0.5)
		return;

	t = GetCurrentTimestamp();

	if (TimestampDifferenceExceeds(asyncQueueControl->lastQueueFillWarn,
								   t, QUEUE_FULL_WARN_INTERVAL))
	{
		QueuePosition min = QUEUE_HEAD;
		int32		minPid = InvalidPid;

		for (BackendId i = QUEUE_FIRST_LISTENER; i > 0; i = QUEUE_NEXT_LISTENER(i))
		{
			Assert(QUEUE_BACKEND_PID(i) != InvalidPid);
			min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
			if (QUEUE_POS_EQUAL(min, QUEUE_BACKEND_POS(i)))
				minPid = QUEUE_BACKEND_PID(i);
		}

		ereport(WARNING,
				(errmsg("NOTIFY queue is %.0f%% full", fillDegree * 100),
				 (minPid != InvalidPid ?
				  errdetail("The server process with PID %d is among those with the oldest transactions.", minPid)
				  : 0),
				 (minPid != InvalidPid ?
				  errhint("The NOTIFY queue cannot be emptied until that process ends its current transaction.")
				  : 0)));

		asyncQueueControl->lastQueueFillWarn = t;
	}
}

/*
 * 向监听后端发送信号。
 *
 * 通常只向自己数据库中的后端发送信号，因只有它们
 * 可能对我们发送的通知感兴趣。然而，如数据库中有
 * 通知流量但其他有监听者的数据库中没有，那些监听
 * 者会越来越落后。如足够落后则唤醒它们，以便推进
 * 其队列位置指针，允许全局尾部推进。
 *
 * 由于知道 BackendId 和 Pid，发送信号成本很低。
 *
 * 在 CommitTransaction() 期间调用，因此失败概率
 * 必须非常低。
*
 */
static void
SignalBackends(void)
{
	int32	   *pids;
	BackendId  *ids;
	int			count;

	/*
		 * 识别需发信号的后端。不想在持有
		 * NotifyQueueLock 时发送信号，因此此循环只
		 * 构建目标 PID 列表。
		 *
		 * XXX 原则上这些 palloc 可能失败，会很糟。
		 * 也许预分配数组？不过它们不大。
		 */
	pids = (int32 *) palloc(MaxBackends * sizeof(int32));
	ids = (BackendId *) palloc(MaxBackends * sizeof(BackendId));
	count = 0;

	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	for (BackendId i = QUEUE_FIRST_LISTENER; i > 0; i = QUEUE_NEXT_LISTENER(i))
	{
		int32		pid = QUEUE_BACKEND_PID(i);
		QueuePosition pos;

		Assert(pid != InvalidPid);
		pos = QUEUE_BACKEND_POS(i);
		if (QUEUE_BACKEND_DBOID(i) == MyDatabaseId)
		{
			/*
				 * 始终向自己数据库中的监听者发送信号，
				 * 除非他们已追上（不太可能但可能）。
				 */
			if (QUEUE_POS_EQUAL(pos, QUEUE_HEAD))
				continue;
		}
		else
		{
			/*
				 * 其他数据库中的监听者仅在远远落后时
				 * 才应被发信号。
				 */
			if (asyncQueuePageDiff(QUEUE_POS_PAGE(QUEUE_HEAD),
								   QUEUE_POS_PAGE(pos)) < QUEUE_CLEANUP_DELAY)
				continue;
		}
		/* 好的，需向此进程发信号 */
		pids[count] = pid;
		ids[count] = i;
		count++;
	}
	LWLockRelease(NotifyQueueLock);

	/* 现在发送信号 */
	for (int i = 0; i < count; i++)
	{
		int32		pid = pids[i];

		/*
			 * 如向自己进程发信号，无需涉及内核；
			 * 直接设置标志。
			 */
		if (pid == MyProcPid)
		{
			notifyInterruptPending = true;
			continue;
		}

		/*
			 * 注意：假设一切正常，此处的信号失败仅
			 * 可能在目标后端自我们释放
			 * NotifyQueueLock 后退出时发生；这不太可能
			 * 但确实可能。因此如发生只记录低级别调试消息。
			 */
		if (SendProcSignal(pid, PROCSIG_NOTIFY_INTERRUPT, ids[i]) < 0)
			elog(DEBUG3, "could not signal backend with PID %d: %m", pid);
	}

	pfree(pids);
	pfree(ids);
}

/*
 * AtAbort_Notify
 *
 *	在事务中止时调用。
 *
 *	清除如事务提交将会执行的待处理操作和传出通知。
*
 */
void
AtAbort_Notify(void)
{
	/*
		 * 如 LISTEN 但在 PreCommit_Notify 后回滚事务，
		 * 已注册为监听者但未在 listenChannels 中创建
		 * 条目。此种情况下再次取消注册。
		 */
	if (amRegisteredListener && listenChannels == NIL)
		asyncQueueUnregister();

	/* 然后清理 */
	ClearPendingActionsAndNotifies();
}

/*
 * AtSubCommit_Notify() --- 处理子事务提交。
 *
 * 将待处理列表中的所有项重新分配给父事务。
 */
void
AtSubCommit_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* 如嵌套级别有操作，必须重新分配给父事务 */
	if (pendingActions != NULL &&
		pendingActions->nestingLevel >= my_level)
	{
		if (pendingActions->upper == NULL ||
			pendingActions->upper->nestingLevel < my_level - 1)
		{
			/* 没有需合并的；整体交给父事务 */
			--pendingActions->nestingLevel;
		}
		else
		{
			ActionList *childPendingActions = pendingActions;

			pendingActions = pendingActions->upper;

			/*
			 * 此处一定不要尝试消除重复 --- 参见 queue_listen()
			 */
			pendingActions->actions =
				list_concat(pendingActions->actions,
							childPendingActions->actions);
			pfree(childPendingActions);
		}
	}

	/* 如嵌套级别有通知，必须重新分配给父事务 */
	if (pendingNotifies != NULL &&
		pendingNotifies->nestingLevel >= my_level)
	{
		Assert(pendingNotifies->nestingLevel == my_level);

		if (pendingNotifies->upper == NULL ||
			pendingNotifies->upper->nestingLevel < my_level - 1)
		{
			/* 没有需合并的；整体交给父事务 */
			--pendingNotifies->nestingLevel;
		}
		else
		{
			/*
				 * 以前不在此处消除重复，但现在必须，
				 * 否则会违反 "Assert(!found)"，在此处
				 * 或稍后尝试构建父级哈希表时。
				 */
			NotificationList *childPendingNotifies = pendingNotifies;
			ListCell   *l;

			pendingNotifies = pendingNotifies->upper;
			/* 将子事务所有事件插入父事务，重复的除外 */
			foreach(l, childPendingNotifies->events)
			{
				Notification *childn = (Notification *) lfirst(l);

				if (!AsyncExistsPendingNotify(childn))
					AddEventToPendingNotifies(childn);
			}
			pfree(childPendingNotifies);
		}
	}
}

/*
 * AtSubAbort_Notify() --- 处理子事务中止。
 */
void
AtSubAbort_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
		 * 只需弹出堆栈 --- 此子事务中创建的操作/通知
		 * 不再有意义，CurTransactionContext 回收时空间
		 * 被释放。但仍需释放 ActionList 和
		 * NotificationList 对象本身，因它们分配在
		 * TopTransactionContext 中。
		 *
		 * 注意，可能完全没有条目，或当前子事务级别
		 * 无条目，要么从未创建，要么因子事务中止期间
		 * 出问题重新进入此例程。
		 */
	while (pendingActions != NULL &&
		   pendingActions->nestingLevel >= my_level)
	{
		ActionList *childPendingActions = pendingActions;

		pendingActions = pendingActions->upper;
		pfree(childPendingActions);
	}

	while (pendingNotifies != NULL &&
		   pendingNotifies->nestingLevel >= my_level)
	{
		NotificationList *childPendingNotifies = pendingNotifies;

		pendingNotifies = pendingNotifies->upper;
		pfree(childPendingNotifies);
	}
}

/*
 * HandleNotifyInterrupt
 *
 *		中断处理的信号处理程序部分。让后端知道
 *		有待处理通知中断。如当前正在从客户端读取，
 *		这将中断读取，ProcessClientReadInterrupt()
 *		将调用 ProcessNotifyInterrupt()。
 */
void
HandleNotifyInterrupt(void)
{
	/*
		 * 注意：这由信号处理程序调用。必须非常
		 * 小心在这里做什么。
		 */

	/* 发出需要工作的信号 */
	notifyInterruptPending = true;

	/* 确保事件在适当时被处理 */
	SetLatch(MyLatch);
}

/*
 * ProcessNotifyInterrupt
 *
 *		如看到 notifyInterruptPending 被设置，则在
 *		前端命令结束发送 ReadyForQuery 前调用，也在
 *		从前端读取时收到通知信号时调用。
 *		HandleNotifyInterrupt() 将通过进程锁存器导致
 *		读取被中断，然后调用此例程。如真正空闲
 *		（即*不在*事务块内），处理传入通知。
 *
 *		如 "flush" 为 true，立即强制发送所有前端
 *		消息。在前端命令结束时调用时可为 false，
 *		因发送 ReadyForQuery 后会刷新。
*
 */
void
ProcessNotifyInterrupt(bool flush)
{
	if (IsTransactionOrTransactionBlock())
		return;					/* 并非真正空闲 */

	/* 循环以防发送消息时有另一信号到达 */
	while (notifyInterruptPending)
		ProcessIncomingNotify(flush);
}


/*
 * 从队列读取所有待处理通知，将合适的传递给
 * 前端。到达队列头或未提交通知时停止。
*
 */
static void
asyncQueueReadAllNotifications(void)
{
	volatile QueuePosition pos;
	QueuePosition head;
	Snapshot	snapshot;

	/* page_buffer 必须充分对齐，使用联合 */
	union
	{
		char		buf[QUEUE_PAGESIZE];
		AsyncQueueEntry align;
	}			page_buffer;

	/* 获取当前状态 */
	LWLockAcquire(NotifyQueueLock, LW_SHARED);
	/* 断言检查有有效状态条目 */
	Assert(MyProcPid == QUEUE_BACKEND_PID(MyBackendId));
	pos = QUEUE_BACKEND_POS(MyBackendId);
	head = QUEUE_HEAD;
	LWLockRelease(NotifyQueueLock);

	if (QUEUE_POS_EQUAL(pos, head))
	{
		/* 无事可做，已读取所有通知 */
		return;
	}

	/*----------
	 * 获取我们将用来判定哪些事务仍在进行中的快照。 
	 * 因为竞态条件，这比看上去更难。                   
	 * 考虑下面的例子：       
	 *
	 * 后端 1:				 后端 2:   
	 *
	 * 事务开始      
	 * UPDATE foo SET ...;
	 * NOTIFY foo;
	 * 提交开始 
	 * 将通知消息入队   
	 *								 事务开始      
	 *								 LISTEN foo;  -- 本会话首次 LISTEN 
	 *								 SELECT * FROM foo WHERE ...;
	 * 提交到 clog
	 *								 提交开始 
	 *								 将后端 2 加入监听者数组  
	 *								 前进到队列头（此代码）
	 *								 提交到 clog
	 *
	 * 事务 2 的 SELECT 没有看到 UPDATE 的效果，因为当时    
	 * 它还没有提交。理想情况下应确保客户端 2 最  
	 * 终收到事务 1 的通知消息，但无法做到这一点；  
	 * 在我们进入监听者数组之前，无法保证通知消息不 
	 * 会从队列中被移除。                                
	 *
	 * 因此事务 2 所用的编码手法不安全：应用程序    
	 * 须在检查数据库状态之前先提交 LISTEN，若想         
	 * 确保能看到关于该状态后续更改的通知。             
	 *                       
	 *
	 * 我们保证的是：会看到在此处快照之后提交    
	 * 的所有通知。Exec_ListenPreCommit 已将我们加  
	 * 入监听者数组，因此在我们看到它们之前，尚未 
	 * 提交的消息不会从队列中被移除。                 
	 *                    
	 *----------
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());

	/*
		 * 尝试向前端发送消息时可能失败（如编码转换
		 * 失败）。如发生，关键是不要反复发送同一消息。
		 * 因此在此放置 PG_TRY 块，在失去控制给错误前
		 * 强制推进队列位置。（可重新获取 NotifyQueueLock
		 * 并在处理每条单独消息前移动位置，但锁流量
		 * 过多。）
		*
		*
		 */
	PG_TRY();
	{
		bool		reachedStop;

		do
		{
			int			curpage = QUEUE_POS_PAGE(pos);
			int			curoffset = QUEUE_POS_OFFSET(pos);
			int			slotno;
			int			copysize;

			/*
				 * 从 SLRU 复制数据到本地缓冲区，避免
				 * 在检查条目并可能传输到前端时持有
				 * NotifySLRULock。只复制将实际检查的
				 * 页面部分。
				 */
			slotno = SimpleLruReadPage_ReadOnly(NotifyCtl, curpage,
												InvalidTransactionId);
			if (curpage == QUEUE_POS_PAGE(head))
			{
				/* 只想读到头位置 */
				copysize = QUEUE_POS_OFFSET(head) - curoffset;
				if (copysize < 0)
					copysize = 0;	/* 仅为安全 */
			}
			else
			{
				/* 获取页面剩余部分 */
				copysize = QUEUE_PAGESIZE - curoffset;
			}
			memcpy(page_buffer.buf + curoffset,
				   NotifyCtl->shared->page_buffer[slotno] + curoffset,
				   copysize);
			/* 释放从 SimpleLruReadPage_ReadOnly() 获取的锁 */
			LWLockRelease(NotifySLRULock);

			/*
				 * 处理消息直到停止位置、页面末尾或
				 * 未提交消息。
				 *
				 * 停止位置是进入此函数时发现的头位置。
				 * 可能已更改。如已更改，将接收（或已接收
				 * 并排队）另一信号并再次回到此处。
				 *
				 * 此处不持有 NotifyQueueLock！队列只能
				 * 扩展到头指针外（见上），保留后端指针
				 * 不变，因此无人会在下方截断或重写页面。
				 * 特别是不想在向前端发通知时持有锁。
				*
				*
				 */
			reachedStop = asyncQueueProcessPageEntries(&pos, head,
													   page_buffer.buf,
													   snapshot);
		} while (!reachedStop);
	}
	PG_FINALLY();
	{
		/* 更新共享状态 */
		LWLockAcquire(NotifyQueueLock, LW_SHARED);
		QUEUE_BACKEND_POS(MyBackendId) = pos;
		LWLockRelease(NotifyQueueLock);
	}
	PG_END_TRY();

	/* 快照使用完毕 */
	UnregisterSnapshot(snapshot);
}

/*
 * 从共享队列获取通知（从 position current 开始），
 * 将相关通知传递给前端。
 *
 * 当前页面须已从共享内存获取到 page_buffer（可直接
 * 访问共享内存中页面，但意味着整个例程持有
 * NotifySLRULock）。
 *
 * 到达 "stop" 位置、或未提交事务的通知、或页面
 * 末尾时停止。
 *
 * 到达停止位置或未提交通知时返回 true，页面处理
 * 完毕则返回 false。即：一旦返回 true 无需继续查找。
 * QueuePosition *current 被推进到所有已处理消息之后。
*
 */
static bool
asyncQueueProcessPageEntries(volatile QueuePosition *current,
							 QueuePosition stop,
							 char *page_buffer,
							 Snapshot snapshot)
{
	bool		reachedStop = false;
	bool		reachedEndOfPage;
	AsyncQueueEntry *qe;

	do
	{
		QueuePosition thisentry = *current;

		if (QUEUE_POS_EQUAL(thisentry, stop))
			break;

		qe = (AsyncQueueEntry *) (page_buffer + QUEUE_POS_OFFSET(thisentry));

		/*
			 * 将此消息的 *current 推进，可能到下一页。
			 * 如 asyncQueueReadAllNotifications 注释所述，
			 * 必须在处理消息可能失败前执行此操作。
			 */
		reachedEndOfPage = asyncQueueAdvance(current, qe->length);

		/* 忽略发往其他数据库的消息 */
		if (qe->dboid == MyDatabaseId)
		{
			if (XidInMVCCSnapshot(qe->xid, snapshot))
			{
				/*
					 * 源事务仍在进行中，因此尚不能处理
					 * 此消息。跳出循环，但首先回退 *current
					 * 以便下次重新处理。（注意：
					 * TransactionIdDidCommit 失败不太可能但
					 * 也非不可能，因此处理未提交消息时无法
					 * 真正避免先推进后退回行为。）
					 *
					 * 注意，必须在测试
					 * TransactionIdDidCommit 前测试
					 * XidInMVCCSnapshot，否则可能返回快照
					 * 尚不可见事务的消息；比较
					 * heapam_visibility.c 头部注释。
					 *
					 * 此外，虽自己事务不在快照中列出，
					 * 无需检查
					 * TransactionIdIsCurrentTransactionId，
					 * 因事务（尚）不能排队任何消息。
					 */
				*current = thisentry;
				reachedStop = true;
				break;
			}
			else if (TransactionIdDidCommit(qe->xid))
			{
				/* qe->data 是以 null 结尾的通道名称 */
				char	   *channel = qe->data;

				if (IsListeningOn(channel))
				{
					/* 负载跟在通道名称之后 */
					char	   *payload = qe->data + strlen(channel) + 1;

					NotifyMyFrontEnd(channel, payload, qe->srcPid);
				}
			}
			else
			{
				/*
					 * 源事务已中止或崩溃，忽略其通知。
					*
					 */
			}
		}

		/* 如不在页面末尾则循环回去 */
	} while (!reachedEndOfPage);

	if (QUEUE_POS_EQUAL(*current, stop))
		reachedStop = true;

	return reachedStop;
}

/*
 * 将共享队列尾变量推进到所有按后端尾指针的
 * 最小值。如可能，截断 pg_notify 空间。
 *
 * 通常在 CommitTransaction() 期间调用，因此
 * 失败概率必须非常低。
 */
static void
asyncQueueAdvanceTail(void)
{
	QueuePosition min;
	int			oldtailpage;
	int			newtailpage;
	int			boundary;

	/* 限制任务为每数据库集簇一个后端；参见 SimpleLruTruncate() */
	LWLockAcquire(NotifyQueueTailLock, LW_EXCLUSIVE);

	/*
		 * 计算新尾部。v13 前 QUEUE_TAIL 须精确
		 * （即精确匹配至少一个后端队列位置），因此
		 * 须与实际计算一起原子更新。从 v13 起可不
		 * 如此，但保持这样更审慎。
		 *
		 * 此外，因传入后端从 QUEUE_TAIL 向前扫描，
		 * 截断数据前该值须被推进。因此 QUEUE_TAIL
		 * 是逻辑尾部，QUEUE_STOP_PAGE 是物理尾部
		 * 即最旧未截断页。当 QUEUE_STOP_PAGE !=
		 * QUEUE_POS_PAGE(QUEUE_TAIL) 时，有可截断
		 * 但尚未完成的页面。
		 *
		 * 为并发性，不想在执行 SimpleLruTruncate
		 * 时持有 NotifyQueueLock。这可以，因无后端
		 * 尝试访问正截断的页面。
		 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	min = QUEUE_HEAD;
	for (BackendId i = QUEUE_FIRST_LISTENER; i > 0; i = QUEUE_NEXT_LISTENER(i))
	{
		Assert(QUEUE_BACKEND_PID(i) != InvalidPid);
		min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
	}
	QUEUE_TAIL = min;
	oldtailpage = QUEUE_STOP_PAGE;
	LWLockRelease(NotifyQueueLock);

	/*
	 * 当全局尾部跨过 SLRU 段边界推进时，我们可以        
	 * 进行截断。  
	 *
	 * XXX 每隔几个段才截断一次可能更好，以减少目录扫描 
	 * 的次数。                         
	 */
	newtailpage = QUEUE_POS_PAGE(min);
	boundary = newtailpage - (newtailpage % SLRU_PAGES_PER_SEGMENT);
	if (asyncQueuePagePrecedes(oldtailpage, boundary))
	{
		/*
			 * SimpleLruTruncate() 将请求 NotifySLRULock
			 * 但也将再次释放该锁。
			 */
		SimpleLruTruncate(NotifyCtl, newtailpage);

		/*
			 * 更新 QUEUE_STOP_PAGE。改变
			 * asyncQueueIsFull() 对紧邻旧尾之前段的
			 * 判定，允许新数据进入该段。
			 */
		LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
		QUEUE_STOP_PAGE = newtailpage;
		LWLockRelease(NotifyQueueLock);
	}

	LWLockRelease(NotifyQueueTailLock);
}

/*
 * ProcessIncomingNotify
 *
 *		扫描队列中到达的通知，并把它们报告给前端。这些 
 *		通知可能来自其他会话，也可能来自我们自己；  
 *		此处无需区分它们。         
 *
 *		若 "flush" 为 true，立即强制发出所有前端消息。  
 *
 *		注意：我们不在任何事务中，须自行创建一个。    
 */
static void
ProcessIncomingNotify(bool flush)
{
	MemoryContext oldcontext;

	/* 必须重置标志 */
	notifyInterruptPending = false;

	/* 如未主动监听则无事可做 */
	if (listenChannels == NIL)
		return;

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify");

	set_ps_display("notify interrupt");

	/*
		 * 须在事务内运行 asyncQueueReadAllNotifications，
		 * 否则如出错会很糟。但需保留调用者的内存
		 * 上下文（通常是 MessageContext）。
		 */
	oldcontext = CurrentMemoryContext;

	StartTransactionCommand();

	asyncQueueReadAllNotifications();

	CommitTransactionCommand();

	/* 调用者上下文最好不要是事务本地的 */
	Assert(MemoryContextIsValid(oldcontext));
	MemoryContextSwitchTo(oldcontext);

	/*
		 * 如非命令结束情况，须刷新通知消息
		 * 以确保前端及时收到。
		 */
	if (flush)
		pq_flush();

	set_ps_display("idle");

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify: done");
}

/*
 * 向我的前端发送 NOTIFY 消息。
 */
void
NotifyMyFrontEnd(const char *channel, const char *payload, int32 srcPid)
{
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, 'A');
		pq_sendint32(&buf, srcPid);
		pq_sendstring(&buf, channel);
		pq_sendstring(&buf, payload);
		pq_endmessage(&buf);

		/*
			 * 注意：不在此处执行 pq_flush()。
			 * 上层调用者稍后处理，允许将此消息
			 * 与其他消息组合到同一数据包。
			 */
	}
	else
		elog(INFO, "NOTIFY for \"%s\" payload \"%s\"", channel, payload);
}

/* pendingNotifies 是否包含与给定事件匹配的项？ */
static bool
AsyncExistsPendingNotify(Notification *n)
{
	if (pendingNotifies == NULL)
		return false;

	if (pendingNotifies->hashtab != NULL)
	{
		/* 使用哈希表探测匹配项 */
		if (hash_search(pendingNotifies->hashtab,
						&n,
						HASH_FIND,
						NULL))
			return true;
	}
	else
	{
		/* 必须扫描事件列表 */
		ListCell   *l;

		foreach(l, pendingNotifies->events)
		{
			Notification *oldn = (Notification *) lfirst(l);

			if (n->channel_len == oldn->channel_len &&
				n->payload_len == oldn->payload_len &&
				memcmp(n->data, oldn->data,
					   n->channel_len + n->payload_len + 2) == 0)
				return true;
		}
	}

	return false;
}

/*
 * 将通知事件添加到已存在的 pendingNotifies 列表。
 *
 * 因 pendingNotifies->events 已是非空，所以无论
 * CurrentMemoryContext 是什么，此操作正确。
 */
static void
AddEventToPendingNotifies(Notification *n)
{
	Assert(pendingNotifies->events != NIL);

	/* 如时机合适，创建哈希表 */
	if (list_length(pendingNotifies->events) >= MIN_HASHABLE_NOTIFIES &&
		pendingNotifies->hashtab == NULL)
	{
		HASHCTL		hash_ctl;
		ListCell   *l;

		/* 创建哈希表 */
		hash_ctl.keysize = sizeof(Notification *);
		hash_ctl.entrysize = sizeof(NotificationHash);
		hash_ctl.hash = notification_hash;
		hash_ctl.match = notification_match;
		hash_ctl.hcxt = CurTransactionContext;
		pendingNotifies->hashtab =
			hash_create("Pending Notifies",
						256L,
						&hash_ctl,
						HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

		/* 插入所有已存在的事件 */
		foreach(l, pendingNotifies->events)
		{
			Notification *oldn = (Notification *) lfirst(l);
			NotificationHash *hentry;
			bool		found;

			hentry = (NotificationHash *) hash_search(pendingNotifies->hashtab,
													  &oldn,
													  HASH_ENTER,
													  &found);
			Assert(!found);
			hentry->event = oldn;
		}
	}

	/* 按序将新事件添加到列表 */
	pendingNotifies->events = lappend(pendingNotifies->events, n);

	/* 如需要，将事件添加到哈希表 */
	if (pendingNotifies->hashtab != NULL)
	{
		NotificationHash *hentry;
		bool		found;

		hentry = (NotificationHash *) hash_search(pendingNotifies->hashtab,
												  &n,
												  HASH_ENTER,
												  &found);
		Assert(!found);
		hentry->event = n;
	}
}

/*
 * notification_hash：通知哈希表的哈希函数
 *
 * 哈希"键"是指向 Notification 结构体的指针。
 */
static uint32
notification_hash(const void *key, Size keysize)
{
	const Notification *k = *(const Notification *const *) key;

	Assert(keysize == sizeof(Notification *));
	/* 哈希中不包含负载的尾随 null */
	return DatumGetUInt32(hash_any((const unsigned char *) k->data,
								   k->channel_len + k->payload_len + 1));
}

/*
 * notification_match：与 notification_hash 配合使用的匹配函数
 */
static int
notification_match(const void *key1, const void *key2, Size keysize)
{
	const Notification *k1 = *(const Notification *const *) key1;
	const Notification *k2 = *(const Notification *const *) key2;

	Assert(keysize == sizeof(Notification *));
	if (k1->channel_len == k2->channel_len &&
		k1->payload_len == k2->payload_len &&
		memcmp(k1->data, k2->data,
			   k1->channel_len + k1->payload_len + 2) == 0)
		return 0;				/* 相等 */
	return 1;					/* 不相等 */
}

/* 清空 pendingActions 和 pendingNotifies 列表 */
static void
ClearPendingActionsAndNotifies(void)
{
	/*
		 * 所有内容分配在 TopTransactionContext 或
		 * 对应子事务的上下文中。因此只需重置指针；
		 * 上下文被删除时空间被回收。
		*
		 */
	pendingActions = NULL;
	pendingNotifies = NULL;
}
