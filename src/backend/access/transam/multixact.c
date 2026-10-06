/*-------------------------------------------------------------------------
 *
 * multixact.c
 *		PostgreSQL 多事务日志管理器
 *
 * pg_multixact 管理器是类似 pg_xact 的管理器，为
 * 每个 MultiXactId 存储 MultiXactMember
 * 数组。它是共享行锁实现的基础部分。每个 MultiXactMember
 * 由一个 TransactionId 和一组标志位组成。这个名字带有一
 * 点历史色彩：起初 MultiXactId 由多于一个
 * TransactionId 组成（罕见角落情况除外），故称 "multi
 * "。如今，仅包含单个 Xid 的 MultiXactIds 也完全合法。
 *
 * 标志位的含义对本模块不透明，但它们主
 * 要在 heapam.c 中用来标识各
 * 成员事务在给定元组上持有的锁模式。本
 * 模块只提供存储与检索这些数组的支持。
 *
 * 我们使用两个 SLRU 区域：一个存放每个 MultiXactId
 * 在另一个区域中数据起始处的偏移。这一技巧使我们能存储变
 * 长的 TransactionIds 数组。（也可改用
 * 单个区域同时存放计数与 TransactionIds，
 * 由合法 MultiXactId 指向含计数的槽；但那
 * 样似乎更不稳健，因为若有人查询一个指向中间含 XID
 * 槽的伪 MultiXactId，就会完全搞乱。）
 *
 * XLOG 交互：每当新的 OFFSETs 或 MEMBERs
 * 页被初始化为零时，本模块会生成一条记录；每当定义新的
 * MultiXactId 时，还会生成
 * XLOG_MULTIXACT_CREATE_ID 记录。本模块忽
 * 略 WAL 规则 "先写 xlog 再写数据"，因为只要把
 * MultiXactId 记入堆 xmax 的操作遵循该规则即可。
 * 数据页引用 MXID 的唯一途径是 heap_lock_tuple
 * () 或 heap_update() 写入，而它们各自生成的
 * XLOG 记录必须排在我们的记录之后。数据页与该 XLOG 记
 * 录之间的常规 LSN 互锁可确保我们的 XLOG 记录先落盘。
 * 若 SLRU 成员/偏移数据比 XLOG 记录更早落盘，我们也
 * 不在意；恢复后不会有 xmax 引用它。另一方面，为确保所有被
 * 引用的项确实 _do_ 落盘，本模块的 XLOG 记录会完整重
 * 建自上次检查点以来写入的数据。在每个检查点被视为完成之前，我们
 * 会刷新并同步所有脏的 OFFSETs 与 MEMBERs 页。
 *
 * 与 clog.c 一样、与 subtrans.
 * c 不同，我们必须在崩溃后保留状态，并确保 MXID
 * 与偏移编号在崩溃后仍单调递增。做法与事务 ID
 * 相同：WAL 记录保证包含我们需要关心的每个
 * MXID 的证据，我们只需确保重放结束时 next
 * -MXID 与 next-offset
 * 计数器至少不小于重放期间所见的任何值。
 *
 * 通过仔细跟踪每张表用过的值，可以删除不再需要的段：
 * 清理期间，早于某阈值的多事务会被移除；截止值存在
 * pg_class 中。每个数据库内所有表的最小值存
 * 在 pg_database，跨所有数据库的全局最小
 * 值是 pg_control 的一部分并保存在共享内
 * 存中。每当该最小值推进时，就会截断 SLRUs。
 *
 * 创建新的 multixactid
 * 值时，会注意使计数器不会落入考虑
 * 全局最小值后的回卷地平线之内。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/multixact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "postmaster/autovacuum.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * MultiXactOffset 页大小的定义。页大小与
 * Postgres 其他地方使用的 BLCKSZ 相同。
 *
 * 注意：因为 MultiXactOffsets 是 32 位并在 0xFFFFFFFF
 * 处回卷，MultiXact 页编号也会在 0xFFFFFFFF/
 * MULTIXACT_OFFSETS_PER_PAGE 处回卷，段编号在
 * 0xFFFFFFFF/MULTIXACT_OFFSETS_PER_PAGE
 * /SLRU_PAGES_PER_SEGMENT 处回卷。本模块除了在
 * TruncateMultiXact 中比较段号与页号时（见
 * MultiXactOffsetPagePrecedes）外，无需显式处理这一事实。
 */

/* 每个偏移需要四个字节 */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

#define MultiXactIdToOffsetPage(xid) \
	((xid) / (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)
#define MultiXactIdToOffsetEntry(xid) \
	((xid) % (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)
#define MultiXactIdToOffsetSegment(xid) (MultiXactIdToOffsetPage(xid) / SLRU_PAGES_PER_SEGMENT)

/*
 * 成员的情况更复杂一些：我们为每个 TransactionId
 * 额外存储一字节标志位。为避免对齐问题，先存四个字节的
 * 标志，再存对应的 4 个 Xid。每个这样的 5 字
 * （20 字节）集合称为一个 "group"（组），并
 * 作为整体存入页中。因此在 8kB BLCKSZ 下，
 * 每页可放 409 个组。这会浪费每页 12 字节，
 * 但没关系——此处简洁性（以及性能）优先于空间效率。
 *
 * 注意 "offset" 宏使用的是字节偏移而非数组下标，
 * 因此算术运算必须通过 "char *" 指针完成。
 */
/* 每个事务需要八位，故一个事务正好装入一字节 */
#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* 一组中有多少完整的标志字节？ */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* 完整一组的字节大小 */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/*
 * 因为每页项数不是最后一项编号（成员 0
 * xFFFFFFFF）的约数，最后一段不会
 * 用满最大页数，而且其中最后使用的页所含项
 * 数也与前面的页不同。（换言之，成员 0
 * xFFFFFFFF 位于最后一页的中间某
 * 处，故该项之后页上还有一些空闲空间。）
 *
 * 此常量是最后一段最后一页中的成员数。
 */
#define MAX_MEMBERS_IN_LAST_MEMBERS_PAGE \
		((uint32) ((0xFFFFFFFF % MULTIXACT_MEMBERS_PER_PAGE) + 1))

/* 成员所在的页 */
#define MXOffsetToMemberPage(xid) ((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_PAGE)
#define MXOffsetToMemberSegment(xid) (MXOffsetToMemberPage(xid) / SLRU_PAGES_PER_SEGMENT)

/* 给定成员的标志字在页内的位置（字节偏移） */
#define MXOffsetToFlagsOffset(xid) \
	((((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) % \
	  (TransactionId) MULTIXACT_MEMBERGROUPS_PER_PAGE) * \
	 (TransactionId) MULTIXACT_MEMBERGROUP_SIZE)
#define MXOffsetToFlagsBitShift(xid) \
	(((xid) % (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) * \
	 MXACT_MEMBER_BITS_PER_XACT)

/* 给定成员的 TransactionId 在页内的位置（字节偏移） */
#define MXOffsetToMemberOffset(xid) \
	(MXOffsetToFlagsOffset(xid) + MULTIXACT_FLAGBYTES_PER_GROUP + \
	 ((xid) % MULTIXACT_MEMBERS_PER_MEMBERGROUP) * sizeof(TransactionId))

/* 多事务成员回卷阈值。 */
#define MULTIXACT_MEMBER_SAFE_THRESHOLD		(MaxMultiXactOffset / 2)
#define MULTIXACT_MEMBER_DANGER_THRESHOLD	\
	(MaxMultiXactOffset - MaxMultiXactOffset / 4)

#define PreviousMultiXactId(xid) \
	((xid) == FirstMultiXactId ? MaxMultiXactId : (xid) - 1)

/*
 * 指向 MultiXact 控制用共享内存数据结构的链接
 */
static SlruCtlData MultiXactOffsetCtlData;
static SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/*
 * 跨所有后端共享的 MultiXact 状态。所有这些状态都受
 * MultiXactGenLock 保护。（我们也用
 * MultiXactOffsetSLRULock 和
 * MultiXactMemberSLRULock 保护两套 SLRU
 * 缓冲区的访问。为并发起见，我们避免同时持有其中一把以上的锁。）
 */
typedef struct MultiXactStateData
{
	/* 即将分配的下一个 MultiXactId */
	MultiXactId nextMXact;

	/* 即将分配的下一个偏移 */
	MultiXactOffset nextOffset;

	/* 我们是否已完成多事务启动？ */
	bool		finishedStartup;

	/*
	 * 仍可能被某个关系引用的最老
	 * 多事务。早于此的都不应再
	 * 查阅。这些值由清理更新。
	 */
	MultiXactId oldestMultiXactId;
	Oid			oldestMultiXactDB;

	/*
	 * 可能被某个关系所引用的多事务所引用的
	 * 最老多事务偏移。我们并不总能知道该
	 * 值，故用一个标志指示当前是否已知。
	 */
	MultiXactOffset oldestOffset;
	bool		oldestOffsetKnown;

	/* 防回卷措施的支持 */
	MultiXactId multiVacLimit;
	MultiXactId multiWarnLimit;
	MultiXactId multiStopLimit;
	MultiXactId multiWrapLimit;

	/* 成员防回卷措施的支持 */
	MultiXactOffset offsetStopLimit;	/* 在 oldestOffsetKnown 时可知 */

	/*
	 * 每后端数据从这里开始。在紧随
	 * MultiXactStateData 结构之后的区域
	 * 中存有两个数组。每个都按 BackendId 索引。
	 *
	 * 两个数组中都有：先是全部普通后端的槽（1..MaxBackends），
	 * 再是 max_prepared_xacts 个预备事务的槽。合法
	 * BackendIds 从 1 起；每个数组的元素 0 从不使用。
	 *
	 * OldestMemberMXactId[k] 是各后端
	 * 当前事务可能作为其成员的最老 MultiXactId；
	 * 若后端没有可能成为 MultiXact 成员的活动
	 * transaction，则为 InvalidMultiXactId。
	 * 各后端在给定事务中首次获取共享锁之前，将其表项设为
	 * 当前 nextMXact 计数器，并在事务结束时清
	 * 除。（这可行，因为只有在获取共享锁期间或之后，XID
	 * 才可能成为 MultiXact 的成员，且该
	 * MultiXact 必须在获取锁期间或之后创建。）
	 *
	 * OldestVisibleMXactId[k] 是各后
	 * 端当前事务认为可能仍存活的最老 MultiXactId；
	 * 若不在事务中，或事务尚未关注过 MultiXacts，
	 * 则为 InvalidMultiXactId。在给定事务
	 * 中首次需要时计算，事务结束时清除。可取计算时有效
	 * OldestMemberMXactId[] 表项的最小
	 * 值（若皆无效则用 nextMXact）。各后端不得尝试
	 * 访问早于自身 OldestVisibleMXactId
	 * [] 设置的 MultiXactIds 的任何 SLRU
	 * 数据；因为相关 SLRU 数据可能被并发截断掉。
	 *
	 * OldestMemberMXactId[] 与
	 * OldestVisibleMXactId[] 中所有有效值的
	 * 最老者，被清理视为仍可能有活动成员事务的最早可能值——
	 * OldestMxact。更早的值通常从元组头中移除，或通过换
	 * 成新的 xmax 而被"冻结"。VACUUM 有时甚至可以移
	 * 除值 >= 其 OldestMxact 截止的单个 MultiXact
	 * xmax，不过通常仅当没有任何单个成员 XID 仍在
	 * 运行时。详见 FreezeMultiXactId。
	 *
	 * 每当 VACUUM 推进 relminmxid 时，会取其
	 * OldestMxact 截止或表中仍存留的最老 Multi 作为新
	 * 的 pg_class.relminmxid（取其较早者）。每个数据
	 * 库中所有 relminmxid 的最小值存在 pg_database.
	 * datminmxid。这些值的全局最小值又存在 pg_control
	 * 中。清理期间 vac_truncate_clog() 删除
	 * 不需要的段时，它用作 pg_multixact 的截断点。
	 */
	MultiXactId perBackendXactIds[FLEXIBLE_ARRAY_MEMBER];
} MultiXactStateData;

/*
 * OldestMemberMXactId 与 OldestVisibleMXactId 数
 * 组的最后一个元素。有效元素为 (1..MaxOldestSlot)；元素 0 从不使用。
 */
#define MaxOldestSlot	(MaxBackends + max_prepared_xacts)

/* 指向共享内存中状态数据的指针 */
static MultiXactStateData *MultiXactState;
static MultiXactId *OldestMemberMXactId;
static MultiXactId *OldestVisibleMXactId;


/*
 * 后端本地 MultiXactId 缓存的定义。
 *
 * 我们用此缓存存储已知的 MultiXacts，
 * 从而不必每次都去 SLRU 区域。
 *
 * 缓存的生命周期为单个事务，理由是多数表项会包含
 * 我们自己的 TransactionId，到下一
 * 事务开始时它们已不再有意义。（XXX 这一点是
 * 否正确并不清楚——MultiXact 的其他成
 * 员可能比我们活得更久。但也不清楚刷新旧缓存项的
 * 更好策略是什么。）	FIXME 实际上既然多
 * 事务可能包含更新 Xid，这已经完全不对了。
 *
 * 我们在事务结束时删除的内存上下文中
 * 分配缓存项，因此不必逐个释放表项。
 */
typedef struct mXactCacheEnt
{
	MultiXactId multi;
	int			nmembers;
	dlist_node	node;
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} mXactCacheEnt;

#define MAX_CACHE_ENTRIES	256
static dclist_head MXactCache = DCLIST_STATIC_INIT(MXactCache);
static MemoryContext MXactContext = NULL;

#ifdef MULTIXACT_DEBUG
#define debug_elog2(a,b) elog(a,b)
#define debug_elog3(a,b,c) elog(a,b,c)
#define debug_elog4(a,b,c,d) elog(a,b,c,d)
#define debug_elog5(a,b,c,d,e) elog(a,b,c,d,e)
#define debug_elog6(a,b,c,d,e,f) elog(a,b,c,d,e,f)
#else
#define debug_elog2(a,b)
#define debug_elog3(a,b,c)
#define debug_elog4(a,b,c,d)
#define debug_elog5(a,b,c,d,e)
#define debug_elog6(a,b,c,d,e,f)
#endif

/* 内部 MultiXactId 管理 */
static void MultiXactIdSetOldestVisible(void);
static void RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
							   int nmembers, MultiXactMember *members);
static MultiXactId GetNewMultiXactId(int nmembers, MultiXactOffset *offset);

/* MultiXact 缓存管理 */
static int	mxactMemberComparator(const void *arg1, const void *arg2);
static MultiXactId mXactCacheGetBySet(int nmembers, MultiXactMember *members);
static int	mXactCacheGetById(MultiXactId multi, MultiXactMember **members);
static void mXactCachePut(MultiXactId multi, int nmembers,
						  MultiXactMember *members);

static char *mxstatus_to_string(MultiXactStatus status);

/* SLRU 基础设施管理 */
static int	ZeroMultiXactOffsetPage(int pageno, bool writeXlog);
static int	ZeroMultiXactMemberPage(int pageno, bool writeXlog);
static bool MultiXactOffsetPagePrecedes(int page1, int page2);
static bool MultiXactMemberPagePrecedes(int page1, int page2);
static bool MultiXactOffsetPrecedes(MultiXactOffset offset1,
									MultiXactOffset offset2);
static void ExtendMultiXactOffset(MultiXactId multi);
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers);
static bool MultiXactOffsetWouldWrap(MultiXactOffset boundary,
									 MultiXactOffset start, uint32 distance);
static bool SetOffsetVacuumLimit(bool is_startup);
static bool find_multixact_start(MultiXactId multi, MultiXactOffset *result);
static void WriteMZeroPageXlogRec(int pageno, uint8 info);
static void WriteMTruncateXlogRec(Oid oldestMultiDB,
								  MultiXactId startTruncOff,
								  MultiXactId endTruncOff,
								  MultiXactOffset startTruncMemb,
								  MultiXactOffset endTruncMemb);


/*
 * MultiXactIdCreate
 *		构造表示两个 TransactionIds 的 MultiXactId。
 *
 * 两个 XIDs 必须不同，或请求的状态不同。
 *
 * 注意 - 这里不操心本地 MultiXactId
 * 缓存，因为那由更底层例程处理。
 */
MultiXactId
MultiXactIdCreate(TransactionId xid1, MultiXactStatus status1,
				  TransactionId xid2, MultiXactStatus status2)
{
	MultiXactId newMulti;
	MultiXactMember members[2];

	Assert(TransactionIdIsValid(xid1));
	Assert(TransactionIdIsValid(xid2));

	Assert(!TransactionIdEquals(xid1, xid2) || (status1 != status2));

	/* MultiXactIdSetOldestMember() 必须已经调用过。 */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]));

	/*
	 * 注意：与 MultiXactIdExpand 不同，我们不费心
	 * 检查两个 XIDs 是否仍在运行。典型用法中 xid2 是我们
	 * 自己的 XID，且调用方刚检查过 xid1，再检查会是浪费。
	 */

	members[0].xid = xid1;
	members[0].status = status1;
	members[1].xid = xid2;
	members[1].status = status2;

	newMulti = MultiXactIdCreateFromMembers(2, members);

	debug_elog3(DEBUG2, "Create: %s",
				mxid_to_string(newMulti, 2, members));

	return newMulti;
}

/*
 * MultiXactIdExpand
 *		向既有 MultiXactId 添加一个 TransactionId。
 *
 * 若该 TransactionId 已是传入
 * MultiXactId 的成员且状态相同，则原样返回它。
 *
 * 注意我们并不会真正修改既有 MultiXactId
 * 的成员关系；而是创建一个新的。这是为了避免与
 * 等待某个 MultiXactId 结束的代码
 * 发生竞态；见 heapam.c 中的说明。
 *
 * 注意 - 这里不操心本地 MultiXactId
 * 缓存，因为那由更底层例程处理。
 *
 * 注意：至关重要的是，不得传入来自旧集簇的
 * MultiXactIds（即由 pg_upgrade
 * 从早于本特性的集簇升级而来的）。
 */
MultiXactId
MultiXactIdExpand(MultiXactId multi, TransactionId xid, MultiXactStatus status)
{
	MultiXactId newMulti;
	MultiXactMember *members;
	MultiXactMember *newMembers;
	int			nmembers;
	int			i;
	int			j;

	Assert(MultiXactIdIsValid(multi));
	Assert(TransactionIdIsValid(xid));

	/* MultiXactIdSetOldestMember() 必须已经调用过。 */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]));

	debug_elog5(DEBUG2, "Expand: received multi %u, xid %u status %s",
				multi, xid, mxstatus_to_string(status));

	/*
	 * 注意：这里不允许旧的 multi。
	 * 原因是本函数唯一的调用方会
	 * 检查该多事务已不再运行。
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	if (nmembers < 0)
	{
		MultiXactMember member;

		/*
		 * 该 MultiXactId 已过时。这只可
		 * 能发生在：调用方检查之后、传给我们之前，其
		 * 全部成员都已停止运行。把该事实返回给调用方
		 * 会更好，但会复杂化 API，且不太常发生，
		 * 故以创建单例 MultiXact 应对。
		 */
		member.xid = xid;
		member.status = status;
		newMulti = MultiXactIdCreateFromMembers(1, &member);

		debug_elog4(DEBUG2, "Expand: %u has no members, create singleton %u",
					multi, newMulti);
		return newMulti;
	}

	/*
	 * 若该 TransactionId 已是 MultiXactId
	 * 的成员且状态相同，则直接返回既有 MultiXactId。
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdEquals(members[i].xid, xid) &&
			(members[i].status == status))
		{
			debug_elog4(DEBUG2, "Expand: %u is already a member of %u",
						xid, multi);
			pfree(members);
			return multi;
		}
	}

	/*
	 * 判定 MultiXactId 中哪些成员
	 * 仍然有意义。这包括任何仍在运行的事务，
	 * 以及拿到强于单纯锁且已提交的事务。（已
	 * 中止的更新在此无意义；且多事务中有多于
	 * 一个更新 Xid 会在别处导致错误。）
	 *
	 * 移除死成员不只是优化：Xmax 为
	 * multi 的元组冻结依赖此行为。
	 *
	 * 注意这里有与上面相同的竞态：
	 * 循环结束时 j 可能为 0。
	 */
	newMembers = (MultiXactMember *)
		palloc(sizeof(MultiXactMember) * (nmembers + 1));

	for (i = 0, j = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i].xid) ||
			(ISUPDATE_from_mxstatus(members[i].status) &&
			 TransactionIdDidCommit(members[i].xid)))
		{
			newMembers[j].xid = members[i].xid;
			newMembers[j++].status = members[i].status;
		}
	}

	newMembers[j].xid = xid;
	newMembers[j++].status = status;
	newMulti = MultiXactIdCreateFromMembers(j, newMembers);

	pfree(members);
	pfree(newMembers);

	debug_elog3(DEBUG2, "Expand: returning new multi %u", newMulti);

	return newMulti;
}

/*
 * MultiXactIdIsRunning
 *		返回 MultiXactId 是否仍在"运行"。
 *
 * 若给定 MultiXactId 至少有一个成员仍在运行则
 * 返回 true。注意 "false" 结果肯定不会再变，
 * 因为不允许向既有 MultiXactId 添加成员。
 *
 * 调用方应已验证该多事务并非来自经
 * pg_upgraded 的共享锁元组。
 */
bool
MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly)
{
	MultiXactMember *members;
	int			nmembers;
	int			i;

	debug_elog3(DEBUG2, "IsRunning %u?", multi);

	/*
	 * 这里的 "false" 表示我们假定调用方已检查过：给定 multi
	 * 不可能来自经 pg_upgraded 的数据库。
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, isLockOnly);

	if (nmembers <= 0)
	{
		debug_elog2(DEBUG2, "IsRunning: no members");
		return false;
	}

	/*
	 * 检查自己比查共享内存便宜；若
	 * 当前顶层事务的任何活动子事
	 * 务是成员则返回 true。
	 *
	 * 这不是正确性所需，只是快路径。
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsCurrentTransactionId(members[i].xid))
		{
			debug_elog3(DEBUG2, "IsRunning: I (%d) am running!", i);
			pfree(members);
			return true;
		}
	}

	/*
	 * 若在 procarray.c 中另设入口、对所有
	 * 成员只遍历一次 PGPROC 数组，可以更快。但
	 * 多数情况下 nmembers 足够小，影响不大。
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i].xid))
		{
			debug_elog4(DEBUG2, "IsRunning: member %d (%u) is running",
						i, members[i].xid);
			pfree(members);
			return true;
		}
	}

	pfree(members);

	debug_elog3(DEBUG2, "IsRunning: %u is not running", multi);

	return false;
}

/*
 * MultiXactIdSetOldestMember
 *		保存本事务可能作为其成员的最老 MultiXactId。
 *
 * 给定事务首次要做可能需要 MultiXactId 的操作（
 * 元组锁、更新或删除）时，我们设置其
 * OldestMemberMXactId。即使最终使用
 * TransactionId 而非 MultiXactId 也要做，因
 * 为另有事务可能把我们的 XID 加入某个 MultiXactId。
 *
 * 要设置的值是即将分配的下一个 MultiXactId，故应在执
 * 行任何此类可能涉及 MultiXactId 的操作之前调用。
 */
void
MultiXactIdSetOldestMember(void)
{
	if (!MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]))
	{
		MultiXactId nextMXact;

		/*
		 * 你可能觉得这里不必加锁，因为取存
		 * TransactionIds 大概是原子的，
		 * 但其实需要：假设我们取到 nextMXact
		 * 后长时间失去 CPU。别人可能推进
		 * nextMXact，再有人算出的
		 * OldestVisibleMXactId 会
		 * 晚于我们重新获得控制后要存的值。那就错了。
		 *
		 * 注意共享锁就够了，因为它足以阻止别人推
		 * 进 nextMXact；也没有别人会
		 * 写我们的 OldestMember
		 * 表项，只会读（并假定存储是原子的。）
		 */
		LWLockAcquire(MultiXactGenLock, LW_SHARED);

		/*
		 * 必须提防 nextMXact 处于已回
		 * 卷状态的可能。我们此处不修正计数器本身，
		 * 但必须确保在数组表项中存入合法值。
		 */
		nextMXact = MultiXactState->nextMXact;
		if (nextMXact < FirstMultiXactId)
			nextMXact = FirstMultiXactId;

		OldestMemberMXactId[MyBackendId] = nextMXact;

		LWLockRelease(MultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestMember[%d] = %u",
					MyBackendId, nextMXact);
	}
}

/*
 * MultiXactIdSetOldestVisible
 *		保存本事务认为可能仍存活的最老 MultiXactId。
 *
 * 给定事务首次要检查任何 MultiXactId 时设置
 * OldestVisibleMXactId。一旦设置，就保证
 * MultiXactIds >= 我们自己的
 * OldestVisibleMXactId 的 SLRU 数据不会被截断掉。
 *
 * 要设置的值是 nextMXact 与所有有效的每后端
 * OldestMemberMXactId[] 表项中的最老者。由于我们加了锁，
 * 可以确定之后任何对 MultiXactIdSetOldestMember
 * 的调用都不会把 OldestMemberMXactId[] 设得比这里算出
 * 的更老。因此现在或以后都没有活动事务能成为早于我们此处算出的
 * OldestVisibleMXactId 的任何 MultiXactId 的成员。
 */
static void
MultiXactIdSetOldestVisible(void)
{
	if (!MultiXactIdIsValid(OldestVisibleMXactId[MyBackendId]))
	{
		MultiXactId oldestMXact;
		int			i;

		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

		/*
		 * 必须提防 nextMXact 处于已回
		 * 卷状态的可能。我们此处不修正计数器本身，
		 * 但必须确保在数组表项中存入合法值。
		 */
		oldestMXact = MultiXactState->nextMXact;
		if (oldestMXact < FirstMultiXactId)
			oldestMXact = FirstMultiXactId;

		for (i = 1; i <= MaxOldestSlot; i++)
		{
			MultiXactId thisoldest = OldestMemberMXactId[i];

			if (MultiXactIdIsValid(thisoldest) &&
				MultiXactIdPrecedes(thisoldest, oldestMXact))
				oldestMXact = thisoldest;
		}

		OldestVisibleMXactId[MyBackendId] = oldestMXact;

		LWLockRelease(MultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestVisible[%d] = %u",
					MyBackendId, oldestMXact);
	}
}

/*
 * ReadNextMultiXactId
 *		返回即将分配的下一个 MultiXactId，但不分配它
 */
MultiXactId
ReadNextMultiXactId(void)
{
	MultiXactId mxid;

	/* XXX 按理或许可以不加锁就做这件事。 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	mxid = MultiXactState->nextMXact;
	LWLockRelease(MultiXactGenLock);

	if (mxid < FirstMultiXactId)
		mxid = FirstMultiXactId;

	return mxid;
}

/*
 * ReadMultiXactIdRange
 *		获取仍可能被某个关系引用的 ID 范围。
 */
void
ReadMultiXactIdRange(MultiXactId *oldest, MultiXactId *next)
{
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	*oldest = MultiXactState->oldestMultiXactId;
	*next = MultiXactState->nextMXact;
	LWLockRelease(MultiXactGenLock);

	if (*oldest < FirstMultiXactId)
		*oldest = FirstMultiXactId;
	if (*next < FirstMultiXactId)
		*next = FirstMultiXactId;
}


/*
 * MultiXactIdCreateFromMembers
 *		由指定的成员集合创建新的 MultiXactId
 *
 * 为新的 MultiXactId 建立 XLOG、SLRU 与缓存表项，把给定的
 * TransactionIds 记为成员。返回新创建的 MultiXactId。
 *
 * 注意：传入的 members[] 数组将就地排序。
 */
MultiXactId
MultiXactIdCreateFromMembers(int nmembers, MultiXactMember *members)
{
	MultiXactId multi;
	MultiXactOffset offset;
	xl_multixact_create xlrec;

	debug_elog3(DEBUG2, "Create: %s",
				mxid_to_string(InvalidMultiXactId, nmembers, members));

	/*
	 * 查看缓存中是否已有相同的成员集合；若有，则复
	 * 用该 MultiXactId。（注意：或许会
	 * 觉得只查缓存不够，还应搜磁盘看是否已有重复定
	 * 义。但由于我们只创建包含自己 XID 的
	 * MultiXacts，多数情况下此类 MultiXacts
	 * 本就是我们创建的，故会在缓存中。也有别人
	 * 在我们不知情时把我们加入 MultiXact
	 * 的角落情况，但不值得为此检查。）
	 */
	multi = mXactCacheGetBySet(nmembers, members);
	if (MultiXactIdIsValid(multi))
	{
		debug_elog2(DEBUG2, "Create: in cache!");
		return multi;
	}

	/* 验证给定成员中只有一个更新 Xid。 */
	{
		int			i;
		bool		has_update = false;

		for (i = 0; i < nmembers; i++)
		{
			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				if (has_update)
					elog(ERROR, "new multixact has more than one updating member: %s",
						 mxid_to_string(InvalidMultiXactId, nmembers, members));
				has_update = true;
			}
		}
	}

	/*
	 * 分配要用的 MXID 与偏移范围，并确保 OFFSETs
	 * 与 MEMBERs 文件中有空间。注意：本例程会
	 * 执行 START_CRIT_SECTION()。
	 *
	 * 注意：与 MultiXactIdCreate 和
	 * MultiXactIdExpand 不同，我们这里不检查是否已调
	 * 用 MultiXactIdSetOldestMember。因为
	 * 本例程在某些地方用于创建当前后端并非其成员的新 MultiXactIds，
	 * 尤其是清理中冻结 multi 时。特别是在清理期间，若运
	 * 行很久，保持 OldestMulti 设置是不可接受的。
	 */
	multi = GetNewMultiXactId(nmembers, &offset);

	/* 写一条描述新 MXID 的 XLOG 记录。 */
	xlrec.mid = multi;
	xlrec.moff = offset;
	xlrec.nmembers = nmembers;

	/*
	 * XXX 注意：MultiXactMember 中有很多填
	 * 充空间。我们可以为这条 Xlog 记录找更紧凑的表示—
	 * —或许所有状态标志放在一个 XLogRecData，所
	 * 有 xid 放在另一个？不过是否值得费这个劲并不清楚。
	 */
	XLogBeginInsert();
	XLogRegisterData((char *) (&xlrec), SizeOfMultiXactCreate);
	XLogRegisterData((char *) members, nmembers * sizeof(MultiXactMember));

	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_CREATE_ID);

	/* 现在把信息写入 OFFSETs 与 MEMBERs 日志 */
	RecordNewMultiXact(multi, offset, nmembers, members);

	/* 临界区结束 */
	END_CRIT_SECTION();

	/* 也把新的 MultiXactId 存入本地缓存 */
	mXactCachePut(multi, nmembers, members);

	debug_elog2(DEBUG2, "Create: all done");

	return multi;
}

/*
 * RecordNewMultiXact
 *		把新多事务的信息写入 offsets 与 members 文件
 *
 * 从 MultiXactIdCreateFromMembers
 * 拆出，以便 xlog 重放也能使用。
 */
static void
RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
				   int nmembers, MultiXactMember *members)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	int			i;

	LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/*
	 * 注意：我们把 MultiXactId 传给
	 * SimpleLruReadPage 作为有 I/O 错误时要抱
	 * 怨的 "transaction"。这有点牵强，但由于错误总会给
	 * 出完整路径名，应能清楚看出实际涉及的是 MultiXactId。
	 * 也许将来我们会费心把 slru.c 的错误报告代码通用化。
	 */
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;

	*offptr = offset;

	MultiXactOffsetCtl->shared->page_dirty[slotno] = true;

	/* 交换我们的锁 */
	LWLockRelease(MultiXactOffsetSLRULock);

	LWLockAcquire(MultiXactMemberSLRULock, LW_EXCLUSIVE);

	prev_pageno = -1;

	for (i = 0; i < nmembers; i++, offset++)
	{
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= MultiXactStatusUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactMemberSLRULock);
}

/*
 * GetNewMultiXactId
 *		获取下一个 MultiXactId。
 *
 * 同时在 "members" 区域预留所需空间。
 * 预留空间的起始偏移通过 *offset 返回。
 *
 * 这可能为扩展 offsets 和/或 members 文件
 * 生成 XLOG 记录。不幸的是，为避免竞态我们必须在持有
 * MultiXactGenLock 时做这件事——清零页的
 * XLOG 记录必须出现在任何后端可能向该页存数据之前！
 *
 * 我们在推进共享计数器之前进入临界区。调用方
 * 必须在写完 SLRU 数据后结束临界区。
 */
static MultiXactId
GetNewMultiXactId(int nmembers, MultiXactOffset *offset)
{
	MultiXactId result;
	MultiXactOffset nextOffset;

	debug_elog3(DEBUG2, "GetNew: for %d xids", nmembers);

	/* 安全检查，在 HS 备库上绝不应该走到这里 */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign MultiXactIds during recovery");

	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

	/* 处理 nextMXact 计数器的回卷 */
	if (MultiXactState->nextMXact < FirstMultiXactId)
		MultiXactState->nextMXact = FirstMultiXactId;

	/* 分配 MXID */
	result = MultiXactState->nextMXact;

	/*----------
	 * 检查再分配另一个 MultiXactId
	 * 是否安全。这用于防止因多事务回卷导
	 * 致的灾难性数据丢失。基本规则是：
	 *
	 * 若已超过 multiVacLimit 或成员存储空间
	 * 的安全阈值，或尚不知道成员存储的安全阈值，开始设法强
	 * 制自动清理周期。若已超过 multiWarnLimit，
	 * 开始发出警告。若已超过 multiStopLimit，
	 * 拒绝创建新的 MultiXactIds。
	 *
	 * 注意这些与 GetNewTransactionId 中的防护基本相同。
	 *----------
	 */
	if (!MultiXactIdPrecedes(result, MultiXactState->multiVacLimit))
	{
		/*
		 * 为安全起见，发送信号、警告等时释放
		 * MultiXactGenLock。与其说是为
		 * 了在此情况下保持并发，不如说是避免在
		 * get_database_name() 时有任
		 * 何死锁可能。首先，复制本路径需要的全部共享值。
		 */
		MultiXactId multiWarnLimit = MultiXactState->multiWarnLimit;
		MultiXactId multiStopLimit = MultiXactState->multiStopLimit;
		MultiXactId multiWrapLimit = MultiXactState->multiWrapLimit;
		Oid			oldest_datoid = MultiXactState->oldestMultiXactDB;

		LWLockRelease(MultiXactGenLock);

		if (IsUnderPostmaster &&
			!MultiXactIdPrecedes(result, multiStopLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/*
			 * 我们已进入 ERROR
			 * 区域，立即启动自动清理。
			 */
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

			/* 即使该数据库已消失也要抱怨 */
			if (oldest_datname)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands that generate new MultiXactIds to avoid wraparound data loss in database \"%s\"",
								oldest_datname),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands that generate new MultiXactIds to avoid wraparound data loss in database with OID %u",
								oldest_datoid),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}

		/*
		 * 为避免用信号淹没 postmaster，我们
		 * 每生成 64K 个 multi 才发出一次自
		 * 动清理请求。这在真正出问题前仍有充分机会。
		 */
		if (IsUnderPostmaster && (result % 65536) == 0)
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		if (!MultiXactIdPrecedes(result, multiWarnLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/* 即使该数据库已消失也要抱怨 */
			if (oldest_datname)
				ereport(WARNING,
						(errmsg_plural("database \"%s\" must be vacuumed before %u more MultiXactId is used",
									   "database \"%s\" must be vacuumed before %u more MultiXactIds are used",
									   multiWrapLimit - result,
									   oldest_datname,
									   multiWrapLimit - result),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(WARNING,
						(errmsg_plural("database with OID %u must be vacuumed before %u more MultiXactId is used",
									   "database with OID %u must be vacuumed before %u more MultiXactIds are used",
									   multiWrapLimit - result,
									   oldest_datoid,
									   multiWrapLimit - result),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}

		/* 重新获取锁并重来 */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
		result = MultiXactState->nextMXact;
		if (result < FirstMultiXactId)
			result = FirstMultiXactId;
	}

	/* 确保文件中有容纳该 MXID 的空间。  */
	ExtendMultiXactOffset(result);

	/*
	 * 类似上面，预留成员空间。另外注意不要把零作
	 * 为任何多事务的起始偏移返回。动机见
	 * GetMultiXactIdMembers()。
	 */
	nextOffset = MultiXactState->nextOffset;
	if (nextOffset == 0)
	{
		*offset = 1;
		nmembers++;				/* 也分配成员槽 0 */
	}
	else
		*offset = nextOffset;

	/*----------
	 * 同样防止成员空间被
	 * 耗尽，规则如下：
	 *
	 * 若已超过 offsetStopLimit，拒绝再生成 multi。
	 * 若接近 offsetStopLimit，发出警告。
	 *
	 * 随意地，我们在距 offsetStopLimit
	 * 还有 20 个段或更少时开始发出警告。
	 *
	 * 注意我们尚未更新共享状态，故若在此失败，已
	 * 抓取的多事务 ID 仍可供下一个人使用。
	 *
	 * 注意此处强迫运行自动清理
	 * 没有意义：必须降低多事
	 * 务冻结设置才会有效果。
	 *----------
	 */
#define OFFSET_WARN_SEGMENTS	20
	if (MultiXactState->oldestOffsetKnown &&
		MultiXactOffsetWouldWrap(MultiXactState->offsetStopLimit, nextOffset,
								 nmembers))
	{
		/* 见对应的偏移回卷情形中的注释 */
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("multixact \"members\" limit exceeded"),
				 errdetail_plural("This command would create a multixact with %u members, but the remaining space is only enough for %u member.",
								  "This command would create a multixact with %u members, but the remaining space is only enough for %u members.",
								  MultiXactState->offsetStopLimit - nextOffset - 1,
								  nmembers,
								  MultiXactState->offsetStopLimit - nextOffset - 1),
				 errhint("Execute a database-wide VACUUM in database with OID %u with reduced vacuum_multixact_freeze_min_age and vacuum_multixact_freeze_table_age settings.",
						 MultiXactState->oldestMultiXactDB)));
	}

	/*
	 * 检查是否应启动自动清理以防止成员回卷。注意
	 * 我们用比警告限制大得多的窗口来触发自动清理。
	 * 警告只是最后手段——这与
	 * GetNewTransactionId 的行为一致。
	 */
	if (!MultiXactState->oldestOffsetKnown ||
		(MultiXactState->nextOffset - MultiXactState->oldestOffset
		 > MULTIXACT_MEMBER_SAFE_THRESHOLD))
	{
		/*
		 * 为避免用信号淹没 postmaster，
		 * 我们仅在跨越段边界时发出自动清理请求。
		 * 默认编译设置下大约每 50k 成员一
		 * 次。这在真正出问题前仍有充分机会。
		 */
		if ((MXOffsetToMemberPage(nextOffset) / SLRU_PAGES_PER_SEGMENT) !=
			(MXOffsetToMemberPage(nextOffset + nmembers) / SLRU_PAGES_PER_SEGMENT))
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);
	}

	if (MultiXactState->oldestOffsetKnown &&
		MultiXactOffsetWouldWrap(MultiXactState->offsetStopLimit,
								 nextOffset,
								 nmembers + MULTIXACT_MEMBERS_PER_PAGE * SLRU_PAGES_PER_SEGMENT * OFFSET_WARN_SEGMENTS))
		ereport(WARNING,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_plural("database with OID %u must be vacuumed before %d more multixact member is used",
							   "database with OID %u must be vacuumed before %d more multixact members are used",
							   MultiXactState->offsetStopLimit - nextOffset + nmembers,
							   MultiXactState->oldestMultiXactDB,
							   MultiXactState->offsetStopLimit - nextOffset + nmembers),
				 errhint("Execute a database-wide VACUUM in that database with reduced vacuum_multixact_freeze_min_age and vacuum_multixact_freeze_table_age settings.")));

	ExtendMultiXactMember(nextOffset, nmembers);

	/*
	 * 从此处到调用方把数据写入刚预留的 SLRU
	 * 空间为止都是临界区；我们不想在 MultiXact
	 * 结构只写了一部分时出错退出。（尤其是，推进
	 * nextMXact 后未能写入起始偏移会
	 * 实际破坏前一个 MultiXact。）
	 */
	START_CRIT_SECTION();

	/*
	 * 推进计数器。与 GetNewTransactionId
	 * () 中一样，这必须在文件扩展成功之后！
	 *
	 * 我们此处不关心 MultiXactId 回卷；下一轮
	 * 会处理。但注意本例程退出后 nextMXact 可能
	 * 是 InvalidMultiXactId，或某个段起
	 * 始页上的第一个值，因此其他查看该变量的人必须准备好应
	 * 对这两种情况。类似地，nextOffset 可能为
	 * 零，但我们不会把它用作下一个多事务的实际起始偏移。
	 */
	(MultiXactState->nextMXact)++;

	MultiXactState->nextOffset += nmembers;

	LWLockRelease(MultiXactGenLock);

	debug_elog4(DEBUG2, "GetNew: returning %u offset %u", result, *offset);
	return result;
}

/*
 * GetMultiXactIdMembers
 *		返回组成某个 MultiXactId 的 MultiXactMembers 集合
 *
 * 返回值是找到的成员数，若没有则为 -1，并
 * 将 *members 设为新 palloc
 * 的成员数组。用完后由调用方负责释放。
 *
 * 当且仅当该多事务对应的是在 9.2 或更早安装
 * 中被加锁、随后经 pg_upgrade 升级的
 * 元组上的值时（即 infomask 为
 * HEAP_LOCKED_UPGRADED），
 * from_pgupgrade 必须传 true。
 * 此时我们确知不会再有成员仍在运行，故与空多事务一
 * 样直接返回 -1，不再进一步检查。尝试解析这样的
 * 多事务是错误的：要么该多事务仍在当前有效范围内
 * （返回结果会是假的），要么在范围外（会报错）。
 *
 * 在所有其他情况下，传入的多事务必须在已知有效范
 * 围内，即大于等于 oldestMultiXactId
 * 且小于 nextMXact。否则报错。
 *
 * 若调用方确定给定 multi 仅用于锁元组，
 * 则 isLockOnly 须为 true；
 * 为 false 也不损失正确性，但传 true
 * 可让我们快速返回而不检查旧更新。
 */
int
GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
					  bool from_pgupgrade, bool isLockOnly)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	MultiXactOffset offset;
	int			length;
	int			truelength;
	int			i;
	MultiXactId oldestMXact;
	MultiXactId nextMXact;
	MultiXactId tmpMXact;
	MultiXactOffset nextOffset;
	MultiXactMember *ptr;

	debug_elog3(DEBUG2, "GetMembers: asked for %u", multi);

	if (!MultiXactIdIsValid(multi) || from_pgupgrade)
	{
		*members = NULL;
		return -1;
	}

	/* 查看 MultiXactId 是否在本地缓存中 */
	length = mXactCacheGetById(multi, members);
	if (length >= 0)
	{
		debug_elog3(DEBUG2, "GetMembers: found %s in the cache",
					mxid_to_string(multi, length, *members));
		return length;
	}

	/* 若尚未设置，则设置我们的 OldestVisibleMXactId[] 表项 */
	MultiXactIdSetOldestVisible();

	/*
	 * 若已知该 multi 仅用于加锁而非更
	 * 新，则可跳过检查其是否早于我们最老的
	 * 可见 multi。它不可能仍在运行。
	 */
	if (isLockOnly &&
		MultiXactIdPrecedes(multi, OldestVisibleMXactId[MyBackendId]))
	{
		debug_elog2(DEBUG2, "GetMembers: a locker-only multi is too old");
		*members = NULL;
		return -1;
	}

	/*
	 * 在求助于 SLRU 区域之前，我们先检查 MultiXact 的已知限制。
	 *
	 * 早于 MultiXactState->
	 * oldestMultiXactId 的 ID 不可能再有用；
	 * 它已被截断移除，或即将被移除。若传入这样的 ID，则报错。
	 *
	 * 另外，此处不应见到 ID >= nextMXact；
	 * 若见到，意味着发生了未检测的
	 * ID 回卷。这会引发硬错误。
	 *
	 * 这里共享锁就够了，因为我们不修改任何全局状态。持锁时
	 * 间只需足以抓取当前计数器值。我们可能同时需要
	 * nextMXact 与 nextOffset；见下文。
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	oldestMXact = MultiXactState->oldestMultiXactId;
	nextMXact = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;

	LWLockRelease(MultiXactGenLock);

	if (MultiXactIdPrecedes(multi, oldestMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u does no longer exist -- apparent wraparound",
						multi)));

	if (!MultiXactIdPrecedes(multi, nextMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u has not been created yet -- apparent wraparound",
						multi)));

	/*
	 * 找出开始读取 MultiXactMembers
	 * 所需的偏移，以及该多事务中的成员数。后
	 * 者取为本多事务起始偏移与下一个多事务起
	 * 始偏移之差。不过有些角落情况需要注意：
	 *
	 * 1. 本多事务可能是最新创建的，因而没
	 * 有下一个可查看。此时我们刚保存的
	 * nextOffset 值就是正确的终点。
	 *
	 * 2. 下一个多事务可能仍在填充中：即另一进程可能已做
	 * 完 GetNewMultiXactId 但尚未写入该
	 * ID 的偏移表项。在此情形下，可保证该多事务的偏移
	 * 表项存在（因为 GetNewMultiXactId
	 * 在写入前不会释放 MultiXactGenLock
	 * ）但内容为零（因为我们小心地预清零偏移页）。由于
	 * GetNewMultiXactId 从不会把零作为
	 * 多事务的起始偏移返回，当我们读到下一个多事务的偏
	 * 移为零时，就知道是这种情况。我们稍睡片刻再重试。
	 *
	 * 3. 因为 GetNewMultiXactId
	 * 为处理情形 #2 会把偏移零增到一，在偏移
	 * 回卷点附近存在歧义。若看到下一个多事务的偏
	 * 移为 1，那是我们多事务的实际终点，还是它
	 * 在零处结束随后又递增了？我们利用如下知识处
	 * 理：若第零个成员槽未被填充，它会含零，而零
	 * 不是合法事务 ID，故不可能是多事务成员。
	 * 因此，若从成员数组读到零，直接忽略它。
	 *
	 * 这一切相当凌乱，但混乱只出现在少见的角落情况，
	 * 因此看起来比在每次创建多事务时长时间持有
	 * MultiXactGenLock 更好。
	 */
retry:
	LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;

	Assert(offset != 0);

	/*
	 * 使用与 GetNewMultiXactId()
	 * 相同的递增规则，即直到需要时才显式处理回卷。
	 */
	tmpMXact = multi + 1;

	if (nextMXact == tmpMXact)
	{
		/* 角落情况 1：没有下一个多事务 */
		length = nextOffset - offset;
	}
	else
	{
		MultiXactOffset nextMXOffset;

		/* 若需要则处理回卷 */
		if (tmpMXact < FirstMultiXactId)
			tmpMXact = FirstMultiXactId;

		prev_pageno = pageno;

		pageno = MultiXactIdToOffsetPage(tmpMXact);
		entryno = MultiXactIdToOffsetEntry(tmpMXact);

		if (pageno != prev_pageno)
			slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, tmpMXact);

		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		nextMXOffset = *offptr;

		if (nextMXOffset == 0)
		{
			/* 角落情况 2：下一个多事务仍在填充中 */
			LWLockRelease(MultiXactOffsetSLRULock);
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
			goto retry;
		}

		length = nextMXOffset - offset;
	}

	LWLockRelease(MultiXactOffsetSLRULock);

	ptr = (MultiXactMember *) palloc(length * sizeof(MultiXactMember));

	/* 现在获取成员本身。 */
	LWLockAcquire(MultiXactMemberSLRULock, LW_EXCLUSIVE);

	truelength = 0;
	prev_pageno = -1;
	for (i = 0; i < length; i++, offset++)
	{
		TransactionId *xactptr;
		uint32	   *flagsptr;
		int			flagsoff;
		int			bshift;
		int			memberoff;

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		if (!TransactionIdIsValid(*xactptr))
		{
			/* 角落情况 3：我们必须在看未使用的槽零 */
			Assert(offset == 0);
			continue;
		}

		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		ptr[truelength].xid = *xactptr;
		ptr[truelength].status = (*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK;
		truelength++;
	}

	LWLockRelease(MultiXactMemberSLRULock);

	/* 成员数为零的多事务 ID 不应出现 */
	Assert(truelength > 0);

	/*
	 * 把结果复制到本地缓存。
	 */
	mXactCachePut(multi, truelength, ptr);

	debug_elog3(DEBUG2, "GetMembers: no cache for %s",
				mxid_to_string(multi, truelength, ptr));
	*members = ptr;
	return truelength;
}

/*
 * mxactMemberComparator
 *		MultiXactMember 的 qsort 比较函数
 *
 * 我们不能对 XIDs 使用回卷比较，因为那
 * 不满足三角不等式！任意旧的排序次序都可以。
 */
static int
mxactMemberComparator(const void *arg1, const void *arg2)
{
	MultiXactMember member1 = *(const MultiXactMember *) arg1;
	MultiXactMember member2 = *(const MultiXactMember *) arg2;

	if (member1.xid > member2.xid)
		return 1;
	if (member1.xid < member2.xid)
		return -1;
	if (member1.status > member2.status)
		return 1;
	if (member1.status < member2.status)
		return -1;
	return 0;
}

/*
 * mXactCacheGetBySet
 *		根据组成它的 TransactionIds 集
 *		合从缓存返回 MultiXactId，若无匹配
 *		则返回 InvalidMultiXactId。
 *
 * 这很有帮助，例如两个事务要锁一张大表时。借助缓存，
 * 第二个事务会对多数元组使用相同的 MultiXactId，
 * 从而保持 MultiXactId 用量较
 * 低（既节省 I/O 也减少回卷问题）。
 *
 * 注意：传入的 members 数组将就地排序。
 */
static MultiXactId
mXactCacheGetBySet(int nmembers, MultiXactMember *members)
{
	dlist_iter	iter;

	debug_elog3(DEBUG2, "CacheGet: looking for %s",
				mxid_to_string(InvalidMultiXactId, nmembers, members));

	/* 排序数组以便于比较 */
	qsort(members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	dclist_foreach(iter, &MXactCache)
	{
		mXactCacheEnt *entry = dclist_container(mXactCacheEnt, node,
												iter.cur);

		if (entry->nmembers != nmembers)
			continue;

		/*
		 * 我们假定缓存表项已排序，且 "status
		 * " 中未用位已清零。
		 */
		if (memcmp(members, entry->members, nmembers * sizeof(MultiXactMember)) == 0)
		{
			debug_elog3(DEBUG2, "CacheGet: found %u", entry->multi);
			dclist_move_head(&MXactCache, iter.cur);
			return entry->multi;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found :-(");
	return InvalidMultiXactId;
}

/*
 * mXactCacheGetById
 *		若存在，则从缓存返回给定 MultiXactId
 *		的组成 MultiXactMember 集合。
 *
 * 成功时 *xids 设为 MultiXactMember 集合的
 * 一份 palloc 副本的地址。返回值为成员数，失败为 -1。
 */
static int
mXactCacheGetById(MultiXactId multi, MultiXactMember **members)
{
	dlist_iter	iter;

	debug_elog3(DEBUG2, "CacheGet: looking for %u", multi);

	dclist_foreach(iter, &MXactCache)
	{
		mXactCacheEnt *entry = dclist_container(mXactCacheEnt, node,
												iter.cur);

		if (entry->multi == multi)
		{
			MultiXactMember *ptr;
			Size		size;

			size = sizeof(MultiXactMember) * entry->nmembers;
			ptr = (MultiXactMember *) palloc(size);

			memcpy(ptr, entry->members, size);

			debug_elog3(DEBUG2, "CacheGet: found %s",
						mxid_to_string(multi,
									   entry->nmembers,
									   entry->members));

			/*
			 * 注意我们在未使用可修改迭代器
			 * 时修改了列表。这仅因为我们紧
			 * 接着就退出迭代，故可接受。
			 */
			dclist_move_head(&MXactCache, iter.cur);

			*members = ptr;
			return entry->nmembers;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found");
	return -1;
}

/*
 * mXactCachePut
 *		把新的 MultiXactId 及其组成集合加入本地缓存。
 */
static void
mXactCachePut(MultiXactId multi, int nmembers, MultiXactMember *members)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CachePut: storing %s",
				mxid_to_string(multi, nmembers, members));

	if (MXactContext == NULL)
	{
		/* 缓存的生命周期仅限当前事务 */
		debug_elog2(DEBUG2, "CachePut: initializing memory context");
		MXactContext = AllocSetContextCreate(TopTransactionContext,
											 "MultiXact cache context",
											 ALLOCSET_SMALL_SIZES);
	}

	entry = (mXactCacheEnt *)
		MemoryContextAlloc(MXactContext,
						   offsetof(mXactCacheEnt, members) +
						   nmembers * sizeof(MultiXactMember));

	entry->multi = multi;
	entry->nmembers = nmembers;
	memcpy(entry->members, members, nmembers * sizeof(MultiXactMember));

	/* mXactCacheGetBySet 假定表项已排序，故先排序它们 */
	qsort(entry->members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	dclist_push_head(&MXactCache, &entry->node);
	if (dclist_count(&MXactCache) > MAX_CACHE_ENTRIES)
	{
		dlist_node *node;

		node = dclist_tail_node(&MXactCache);
		dclist_delete_from(&MXactCache, node);

		entry = dclist_container(mXactCacheEnt, node, node);
		debug_elog3(DEBUG2, "CachePut: pruning cached multi %u",
					entry->multi);

		pfree(entry);
	}
}

static char *
mxstatus_to_string(MultiXactStatus status)
{
	switch (status)
	{
		case MultiXactStatusForKeyShare:
			return "keysh";
		case MultiXactStatusForShare:
			return "sh";
		case MultiXactStatusForNoKeyUpdate:
			return "fornokeyupd";
		case MultiXactStatusForUpdate:
			return "forupd";
		case MultiXactStatusNoKeyUpdate:
			return "nokeyupd";
		case MultiXactStatusUpdate:
			return "upd";
		default:
			elog(ERROR, "unrecognized multixact status %d", status);
			return "";
	}
}

char *
mxid_to_string(MultiXactId multi, int nmembers, MultiXactMember *members)
{
	static char *str = NULL;
	StringInfoData buf;
	int			i;

	if (str != NULL)
		pfree(str);

	initStringInfo(&buf);

	appendStringInfo(&buf, "%u %d[%u (%s)", multi, nmembers, members[0].xid,
					 mxstatus_to_string(members[0].status));

	for (i = 1; i < nmembers; i++)
		appendStringInfo(&buf, ", %u (%s)", members[i].xid,
						 mxstatus_to_string(members[i].status));

	appendStringInfoChar(&buf, ']');
	str = MemoryContextStrdup(TopMemoryContext, buf.data);
	pfree(buf.data);
	return str;
}

/*
 * AtEOXact_MultiXact
 *		处理 MultiXact 的事务结束
 *
 * 在顶层事务提交或中止时调用（我们不区分是哪一种）。
 */
void
AtEOXact_MultiXact(void)
{
	/*
	 * 重置我们的 OldestMemberMXactId 与
	 * OldestVisibleMXactId 值，二者都应仅在事务内有效。
	 *
	 * 我们假定存储 MultiXactId 是原子的，因
	 * 此不必为此获取 MultiXactGenLock。
	 */
	OldestMemberMXactId[MyBackendId] = InvalidMultiXactId;
	OldestVisibleMXactId[MyBackendId] = InvalidMultiXactId;

	/*
	 * 丢弃本地 MultiXactId 缓存。由于 MXactContext 是作为
	 * TopTransactionContext 的子上下文创建的，我们不必显式删除它。
	 */
	MXactContext = NULL;
	dclist_init(&MXactCache);
}

/*
 * AtPrepare_MultiXact
 *		在 2PC 事务预备时保存多事务状态
 *
 * 在此阶段，我们只把
 * OldestMemberMXactId 值存入两阶段状态文件。
 */
void
AtPrepare_MultiXact(void)
{
	MultiXactId myOldestMember = OldestMemberMXactId[MyBackendId];

	if (MultiXactIdIsValid(myOldestMember))
		RegisterTwoPhaseRecord(TWOPHASE_RM_MULTIXACT_ID, 0,
							   &myOldestMember, sizeof(MultiXactId));
}

/*
 * PostPrepare_MultiXact
 *		在成功的 PREPARE TRANSACTION 之后清理
 */
void
PostPrepare_MultiXact(TransactionId xid)
{
	MultiXactId myOldestMember;

	/*
	 * 把我们的 OldestMemberMXactId
	 * 值转移到为预备事务预留的槽。
	 */
	myOldestMember = OldestMemberMXactId[MyBackendId];
	if (MultiXactIdIsValid(myOldestMember))
	{
		BackendId	dummyBackendId = TwoPhaseGetDummyBackendId(xid, false);

		/*
		 * 尽管存储 MultiXactId 是原子的，
		 * 仍获取锁以确保别人能同时看到两处更改，而
		 * 不仅是当前后端槽被重置。使用 volatile
		 * 指针或许也够，但这里不是热点。
		 */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

		OldestMemberMXactId[dummyBackendId] = myOldestMember;
		OldestMemberMXactId[MyBackendId] = InvalidMultiXactId;

		LWLockRelease(MultiXactGenLock);
	}

	/*
	 * 我们不需要转移
	 * OldestVisibleMXactId 值，
	 * 因为事务一旦预备就不会再查看更多多事务。
	 *
	 * 我们假定存储 MultiXactId 是原子的，因
	 * 此不必为此获取 MultiXactGenLock。
	 */
	OldestVisibleMXactId[MyBackendId] = InvalidMultiXactId;

	/*
	 * 像 AtEOXact_MultiXact 那样丢弃本地 MultiXactId 缓存。
	 */
	MXactContext = NULL;
	dclist_init(&MXactCache);
}

/*
 * multixact_twophase_recover
 *		在启动时恢复预备事务的状态
 */
void
multixact_twophase_recover(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	BackendId	dummyBackendId = TwoPhaseGetDummyBackendId(xid, false);
	MultiXactId oldestMember;

	/*
	 * 从状态文件记录获取最老成员 XID，并把它设到为该预备事
	 * 务预留的 OldestMemberMXactId 槽中。
	 */
	Assert(len == sizeof(MultiXactId));
	oldestMember = *((MultiXactId *) recdata);

	OldestMemberMXactId[dummyBackendId] = oldestMember;
}

/*
 * multixact_twophase_postcommit
 *		类似于 AtEOXact_MultiXact，但用于 COMMIT PREPARED
 */
void
multixact_twophase_postcommit(TransactionId xid, uint16 info,
							  void *recdata, uint32 len)
{
	BackendId	dummyBackendId = TwoPhaseGetDummyBackendId(xid, true);

	Assert(len == sizeof(MultiXactId));

	OldestMemberMXactId[dummyBackendId] = InvalidMultiXactId;
}

/*
 * multixact_twophase_postabort
 *		这实际上与 COMMIT 情形完全相同。
 */
void
multixact_twophase_postabort(TransactionId xid, uint16 info,
							 void *recdata, uint32 len)
{
	multixact_twophase_postcommit(xid, info, recdata, len);
}

/*
 * 初始化 MultiXact 的共享内存。我们使用两个 SLRU
 * 区域，因而内存加倍。还要为共享 MultiXactState 结
 * 构以及每后端 MultiXactId 数组（也是两份）预留空间。
 */
Size
MultiXactShmemSize(void)
{
	Size		size;

	/* 我们需要 2*MaxOldestSlot + 1 个 perBackendXactIds[] 表项 */
#define SHARED_MULTIXACT_STATE_SIZE \
	add_size(offsetof(MultiXactStateData, perBackendXactIds) + sizeof(MultiXactId), \
			 mul_size(sizeof(MultiXactId) * 2, MaxOldestSlot))

	size = SHARED_MULTIXACT_STATE_SIZE;
	size = add_size(size, SimpleLruShmemSize(NUM_MULTIXACTOFFSET_BUFFERS, 0));
	size = add_size(size, SimpleLruShmemSize(NUM_MULTIXACTMEMBER_BUFFERS, 0));

	return size;
}

void
MultiXactShmemInit(void)
{
	bool		found;

	debug_elog2(DEBUG2, "Shared Memory Init for MultiXact");

	MultiXactOffsetCtl->PagePrecedes = MultiXactOffsetPagePrecedes;
	MultiXactMemberCtl->PagePrecedes = MultiXactMemberPagePrecedes;

	SimpleLruInit(MultiXactOffsetCtl,
				  "MultiXactOffset", NUM_MULTIXACTOFFSET_BUFFERS, 0,
				  MultiXactOffsetSLRULock, "pg_multixact/offsets",
				  LWTRANCHE_MULTIXACTOFFSET_BUFFER,
				  SYNC_HANDLER_MULTIXACT_OFFSET);
	SlruPagePrecedesUnitTests(MultiXactOffsetCtl, MULTIXACT_OFFSETS_PER_PAGE);
	SimpleLruInit(MultiXactMemberCtl,
				  "MultiXactMember", NUM_MULTIXACTMEMBER_BUFFERS, 0,
				  MultiXactMemberSLRULock, "pg_multixact/members",
				  LWTRANCHE_MULTIXACTMEMBER_BUFFER,
				  SYNC_HANDLER_MULTIXACT_MEMBER);
	/* 不调用 SimpleLruTruncate()，也不满足单元测试准则 */

	/* 初始化我们的共享状态结构 */
	MultiXactState = ShmemInitStruct("Shared MultiXact State",
									 SHARED_MULTIXACT_STATE_SIZE,
									 &found);
	if (!IsUnderPostmaster)
	{
		Assert(!found);

		/* 确保将每后端状态清零 */
		MemSet(MultiXactState, 0, SHARED_MULTIXACT_STATE_SIZE);
	}
	else
		Assert(found);

	/*
	 * 设置数组指针。注意 perBackendXactIds[0] 是浪
	 * 费的空间，因为每个数组只用索引 1..MaxOldestSlot。
	 */
	OldestMemberMXactId = MultiXactState->perBackendXactIds;
	OldestVisibleMXactId = OldestMemberMXactId + MaxOldestSlot;
}

/*
 * 本函数必须在系统安装时调用一次。它创建初始的 MultiXact
 * 段。（假定 MultiXacts 目录已由 initdb 创
 * 建，且 MultiXactShmemInit 已被调用。）
 */
void
BootStrapMultiXact(void)
{
	int			slotno;

	LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

	/* 创建并清零 offsets 日志的第一页 */
	slotno = ZeroMultiXactOffsetPage(0, false);

	/* 确保已写出 */
	SimpleLruWritePage(MultiXactOffsetCtl, slotno);
	Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactOffsetSLRULock);

	LWLockAcquire(MultiXactMemberSLRULock, LW_EXCLUSIVE);

	/* 创建并清零 members 日志的第一页 */
	slotno = ZeroMultiXactMemberPage(0, false);

	/* 确保已写出 */
	SimpleLruWritePage(MultiXactMemberCtl, slotno);
	Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactMemberSLRULock);
}

/*
 * 把 MultiXactOffset 的一页初始化（或重新初始化）为零。若
 * writeXlog 为 true，还发出一条说明我们做了此事的 XLOG 记录。
 *
 * 页实际上并未写出，只是在共享
 * 内存中建好。返回新页的槽号。
 *
 * 进入时必须持有控制锁，退出时仍持有。
 */
static int
ZeroMultiXactOffsetPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);

	if (writeXlog)
		WriteMZeroPageXlogRec(pageno, XLOG_MULTIXACT_ZERO_OFF_PAGE);

	return slotno;
}

/*
 * 对 MultiXactMember 同样处理
 */
static int
ZeroMultiXactMemberPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactMemberCtl, pageno);

	if (writeXlog)
		WriteMZeroPageXlogRec(pageno, XLOG_MULTIXACT_ZERO_MEM_PAGE);

	return slotno;
}

/*
 * MaybeExtendOffsetSlru
 *		若需要则扩展 offsets SLRU 区域
 *
 * 从 <= 9.2 二进制升级后，pg_multixact
 * /offsets SLRU 区域中的文件可能比
 * 需要的短；若旧安装曾在第一页之后使用过多事务就
 * 会如此（文件无法直接拷贝，因为磁盘表示不同）。
 * pg_upgrade 会更新 pg_control，
 * 把下一个偏移值设到该位置，从而使被此类
 * MultiXacts 标记为已加锁的元组在不
 * 必查阅多事务的情况下仍可见。然而，试图创建并
 * 使用新的 MultiXactId 会出错，因
 * 为新值所在的页不存在。本例程负责创建这些页。
 */
static void
MaybeExtendOffsetSlru(void)
{
	int			pageno;

	pageno = MultiXactIdToOffsetPage(MultiXactState->nextMXact);

	LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

	if (!SimpleLruDoesPhysicalPageExist(MultiXactOffsetCtl, pageno))
	{
		int			slotno;

		/*
		 * 幸运的是，SimpleLruWritePage
		 * 已准备好在我们写入的页并非段内第一页
		 * 时也能创建新段文件，因此这样就够了。
		 */
		slotno = ZeroMultiXactOffsetPage(pageno, false);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno);
	}

	LWLockRelease(MultiXactOffsetSLRULock);
}

/*
 * 必须在 postmaster 或独立后端启动期间调用一次。
 *
 * StartupXLOG 已通过调用 MultiXactSetNextMXact 和/
 * 或 MultiXactAdvanceNextMXact 建立了 nextMXact/
 * nextOffset，以及来自 pg_control 和/或
 * MultiXactAdvanceOldest 的 oldestMulti 信息，但我们尚未重放 WAL。
 */
void
StartupMultiXact(void)
{
	MultiXactId multi = MultiXactState->nextMXact;
	MultiXactOffset offset = MultiXactState->nextOffset;
	int			pageno;

	/*
	 * 初始化 offset 对最新页号的认知。
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	MultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * 初始化 member 对最新页号的认知。
	 */
	pageno = MXOffsetToMemberPage(offset);
	MultiXactMemberCtl->shared->latest_page_number = pageno;
}

/*
 * 必须在启动/恢复结束时调用一次。
 */
void
TrimMultiXact(void)
{
	MultiXactId nextMXact;
	MultiXactOffset offset;
	MultiXactId oldestMXact;
	Oid			oldestMXactDB;
	int			pageno;
	int			entryno;
	int			flagsoff;

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextMXact = MultiXactState->nextMXact;
	offset = MultiXactState->nextOffset;
	oldestMXact = MultiXactState->oldestMultiXactId;
	oldestMXactDB = MultiXactState->oldestMultiXactDB;
	LWLockRelease(MultiXactGenLock);

	/* 清理 offsets 状态 */
	LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

	/*
	 * （重新）初始化我们对 offsets 最新页号的认知。
	 */
	pageno = MultiXactIdToOffsetPage(nextMXact);
	MultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * 把当前 offsets 页的剩余部分清零。背景见 TrimCLOG(
	 * ) 中的说明。与 CLOG 不同，每条 pg_multixact
	 * SLRU 变更都有 WAL 记录覆盖。又由于同样与 CLOG 不同，
	 * 我们忽略 WAL 规则 "先写 xlog 再写数据"，nextMXact
	 * 的后继可能带有过时的非零偏移值。把它们清零，以便
	 * GetMultiXactIdMembers() 的情形 2 能正常工作。
	 */
	entryno = MultiXactIdToOffsetEntry(nextMXact);
	if (entryno != 0)
	{
		int			slotno;
		MultiXactOffset *offptr;

		slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, nextMXact);
		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;

		MemSet(offptr, 0, BLCKSZ - (entryno * sizeof(MultiXactOffset)));

		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactOffsetSLRULock);

	/* 对 members 做同样的事 */
	LWLockAcquire(MultiXactMemberSLRULock, LW_EXCLUSIVE);

	/*
	 * （重新）初始化我们对 members 最新页号的认知。
	 */
	pageno = MXOffsetToMemberPage(offset);
	MultiXactMemberCtl->shared->latest_page_number = pageno;

	/*
	 * 把当前 members 页的剩余部分清零。
	 * 动机见 TrimCLOG() 中的说明。
	 */
	flagsoff = MXOffsetToFlagsOffset(offset);
	if (flagsoff != 0)
	{
		int			slotno;
		TransactionId *xidptr;
		int			memberoff;

		memberoff = MXOffsetToMemberOffset(offset);
		slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, offset);
		xidptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		MemSet(xidptr, 0, BLCKSZ - memberoff);

		/*
		 * 注意：我们不必清零当前组
		 * 中剩余成员的标志位，因
		 * 为写入前总会重置它们。
		 */

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactMemberSLRULock);

	/* 发出信号：我们已正式就绪 */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->finishedStartup = true;
	LWLockRelease(MultiXactGenLock);

	/* 现在计算距离下一次成员回卷还有多远。 */
	SetMultiXactIdLimit(oldestMXact, oldestMXactDB, true);
}

/*
 * 获取要保存在检查点记录中的 MultiXact 数据
 */
void
MultiXactGetCheckptMulti(bool is_shutdown,
						 MultiXactId *nextMulti,
						 MultiXactOffset *nextMultiOffset,
						 MultiXactId *oldestMulti,
						 Oid *oldestMultiDB)
{
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	*nextMulti = MultiXactState->nextMXact;
	*nextMultiOffset = MultiXactState->nextOffset;
	*oldestMulti = MultiXactState->oldestMultiXactId;
	*oldestMultiDB = MultiXactState->oldestMultiXactDB;
	LWLockRelease(MultiXactGenLock);

	debug_elog6(DEBUG2,
				"MultiXact: checkpoint is nextMulti %u, nextOffset %u, oldestMulti %u in DB %u",
				*nextMulti, *nextMultiOffset, *oldestMulti, *oldestMultiDB);
}

/*
 * 执行检查点——无论是关机期间还是联机进行
 */
void
CheckPointMultiXact(void)
{
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(true);

	/*
	 * 把脏的 MultiXact 页写到磁盘。这可能导
	 * 致同步请求排队，稍后由
	 * ProcessSyncRequests() 作为检查点的一部分处理。
	 */
	SimpleLruWriteAll(MultiXactOffsetCtl, true);
	SimpleLruWriteAll(MultiXactMemberCtl, true);

	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_DONE(true);
}

/*
 * 设置即将分配的下一个 MultiXactId 与偏移
 *
 * 当我们能从检查点记录精确确定正确的下一个
 * ID/偏移时使用。虽然这只在 bootstrap
 * 与 XLog 重放期间调用，我们仍
 * 加锁，以防有热备后端在查看这些值。
 */
void
MultiXactSetNextMXact(MultiXactId nextMulti,
					  MultiXactOffset nextMultiOffset)
{
	debug_elog4(DEBUG2, "MultiXact: setting next multi to %u offset %u",
				nextMulti, nextMultiOffset);
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->nextMXact = nextMulti;
	MultiXactState->nextOffset = nextMultiOffset;
	LWLockRelease(MultiXactGenLock);

	/*
	 * 在二进制升级期间，确保 offsets SLRU
	 * 足够大，能容纳即将创建的下一个值。
	 *
	 * 我们需要在二进制升级模式的首次启动中相当早地做这件事：事实
	 * 上要在 StartupMultiXact() 之前，因为
	 * StartupXLOG() 甚至会在那之前调用本例程。而
	 * 又不能比此刻更早，因为在对本例程的第一次调用中我们才确定
	 * MaybeExtendOffsetSlru 需要的
	 * MultiXactState->nextMXact 值。
	 */
	if (IsBinaryUpgrade)
		MaybeExtendOffsetSlru();
}

/*
 * 给定当前最老的 datminmxid（即我们集簇中任何数据库里可能
 * 存在的最老 MultiXactId），以及具有该值的（或某个）数据
 * 库的 OID，确定可分配的最后一个安全 MultiXactId。
 *
 * is_startup 在我们刚启动集簇时为 true，在
 * 运行中的集簇更新状态时为 false。这只影响日志消息。
 */
void
SetMultiXactIdLimit(MultiXactId oldest_datminmxid, Oid oldest_datoid,
					bool is_startup)
{
	MultiXactId multiVacLimit;
	MultiXactId multiWarnLimit;
	MultiXactId multiStopLimit;
	MultiXactId multiWrapLimit;
	MultiXactId curMulti;
	bool		needs_offset_vacuum;

	Assert(MultiXactIdIsValid(oldest_datminmxid));

	/*
	 * 我们假装回卷会发生在多事务 ID 空间的一半处，但
	 * 那并不真确，因为多事务的回卷方式与事务 ID 不同。
	 * 注意，除了多事务 ID 回卷之外，我们还必须确保
	 * 多事务成员不回卷。那方面的限制在
	 * SetOffsetVacuumLimit 中设置，而不是这里。
	 */
	multiWrapLimit = oldest_datminmxid + (MaxMultiXactId >> 1);
	if (multiWrapLimit < FirstMultiXactId)
		multiWrapLimit += FirstMultiXactId;

	/*
	 * 一旦距离数据丢失还有 3M 个 multi，我们将拒绝继续分配
	 * MultiXactIds。见 SetTransactionIdLimit。
	 */
	multiStopLimit = multiWrapLimit - 3000000;
	if (multiStopLimit < FirstMultiXactId)
		multiStopLimit -= FirstMultiXactId;

	/*
	 * 一旦距离数据丢失还有 40M 个 multi，
	 * 我们将开始大声抱怨。这有点随意，但如
	 * 果你的油表只剩 2%，难道不会去找下
	 * 一站加油站吗？这个数字需要相当宽松，
	 * 因为很多场景下多数事务由不会理会警
	 * 告的自动客户端完成。（不，我们不会
	 * 让这可配置。如果你懂到能配置它，你
	 * 也懂到一开始就不会陷入这种麻烦。）
	 */
	multiWarnLimit = multiWrapLimit - 40000000;
	if (multiWarnLimit < FirstMultiXactId)
		multiWarnLimit -= FirstMultiXactId;

	/*
	 * 当 oldest_datminmxid 比
	 * autovacuum_multixact_freeze_max_age 个 mxid 还老时，我们将开始设法强制自动清理。
	 *
	 * 注意：autovacuum_multixact_freeze_max_age
	 * 是 PGC_POSTMASTER 参数，这样我们就不必操心处理其
	 * 值的联机变更。见 SetTransactionIdLimit。
	 */
	multiVacLimit = oldest_datminmxid + autovacuum_multixact_freeze_max_age;
	if (multiVacLimit < FirstMultiXactId)
		multiVacLimit += FirstMultiXactId;

	/* 加锁，时间只需足以设置新的限制值 */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->oldestMultiXactId = oldest_datminmxid;
	MultiXactState->oldestMultiXactDB = oldest_datoid;
	MultiXactState->multiVacLimit = multiVacLimit;
	MultiXactState->multiWarnLimit = multiWarnLimit;
	MultiXactState->multiStopLimit = multiStopLimit;
	MultiXactState->multiWrapLimit = multiWrapLimit;
	curMulti = MultiXactState->nextMXact;
	LWLockRelease(MultiXactGenLock);

	/* 记录该信息 */
	ereport(DEBUG1,
			(errmsg_internal("MultiXactId wrap limit is %u, limited by database with OID %u",
							 multiWrapLimit, oldest_datoid)));

	/*
	 * 只有在数据目录处于一致状态后才可能计算实际限
	 * 制。仍在重放 WAL 时无需计算限制——即使
	 * 可能重放多事务创建，也不会做关于新 multi
	 * 的决策。因此我们只在 TrimMultiXact
	 * () 被调用后再做进一步检查。
	 */
	if (!MultiXactState->finishedStartup)
		return;

	Assert(!InRecovery);

	/* 设置偏移清理的限制。 */
	needs_offset_vacuum = SetOffsetVacuumLimit(is_startup);

	/*
	 * 若已超过自动清理强制点，立即发出自动清理
	 * 请求。原因是自动清理每次调用只处理一个数
	 * 据库。一旦它清理完最老的数据库，就会调
	 * 用到这里；若仍有旧数据库，我们就通知
	 * postmaster 立即开始下一轮。
	 */
	if ((MultiXactIdPrecedes(multiVacLimit, curMulti) ||
		 needs_offset_vacuum) && IsUnderPostmaster)
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

	/* 若已超过回卷警告点，立即给出警告 */
	if (MultiXactIdPrecedes(multiWarnLimit, curMulti))
	{
		char	   *oldest_datname;

		/*
		 * 我们可能在不处于事务中时被调用，例如在
		 * StartupXLOG() 期间。此时无法做
		 * 数据库访问，故只能报告最老数据库的 OID。
		 *
		 * 注意：get_database_name 也
		 * 可能失败并返回 NULL，例如数据库刚被删除。
		 * 我们仍会警告，即使警告此时可能已不必要。
		 */
		if (IsTransactionState())
			oldest_datname = get_database_name(oldest_datoid);
		else
			oldest_datname = NULL;

		if (oldest_datname)
			ereport(WARNING,
					(errmsg_plural("database \"%s\" must be vacuumed before %u more MultiXactId is used",
								   "database \"%s\" must be vacuumed before %u more MultiXactIds are used",
								   multiWrapLimit - curMulti,
								   oldest_datname,
								   multiWrapLimit - curMulti),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		else
			ereport(WARNING,
					(errmsg_plural("database with OID %u must be vacuumed before %u more MultiXactId is used",
								   "database with OID %u must be vacuumed before %u more MultiXactIds are used",
								   multiWrapLimit - curMulti,
								   oldest_datoid,
								   multiWrapLimit - curMulti),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	}
}

/*
 * 确保即将分配的下一个 MultiXactId 至少为 minMulti，
 * 类似地 nextOffset 至少为 minMultiOffset。
 *
 * 当我们能从 XLog 记录（联机检查点或
 * 多事务创建日志项）确定最小安全值时使用。
 * 虽然这只在 XLog 重放期间调用，我
 * 们仍加锁，以防有热备后端在查看这些值。
 */
void
MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset)
{
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	if (MultiXactIdPrecedes(MultiXactState->nextMXact, minMulti))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next multi to %u", minMulti);
		MultiXactState->nextMXact = minMulti;
	}
	if (MultiXactOffsetPrecedes(MultiXactState->nextOffset, minMultiOffset))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next offset to %u",
					minMultiOffset);
		MultiXactState->nextOffset = minMultiOffset;
	}
	LWLockRelease(MultiXactGenLock);
}

/*
 * 更新我们的 oldestMultiXactId
 * 值，但仅当它比我们已有的更新时。
 *
 * 这只能在 WAL 重放期间调用。
 */
void
MultiXactAdvanceOldest(MultiXactId oldestMulti, Oid oldestMultiDB)
{
	Assert(InRecovery);

	if (MultiXactIdPrecedes(MultiXactState->oldestMultiXactId, oldestMulti))
		SetMultiXactIdLimit(oldestMulti, oldestMultiDB, false);
}

/*
 * 确保 MultiXactOffset 有空间容纳新分配的 MultiXactId。
 *
 * 注意：调用时持有 MultiXactGenLock。
 * 我们希望它多数时候非常快；即使不快，除非为
 * 了在共享内存中腾地方而被迫写出脏日志或
 * xlog 页，否则也不需要实际 I/O。
 */
static void
ExtendMultiXactOffset(MultiXactId multi)
{
	int			pageno;

	/*
	 * 除了页的第一个 MultiXactId 外无需工作。但要注意：刚回卷后，第零
	 * 页的第一个 MultiXactId 是 FirstMultiXactId。
	 */
	if (MultiXactIdToOffsetEntry(multi) != 0 &&
		multi != FirstMultiXactId)
		return;

	pageno = MultiXactIdToOffsetPage(multi);

	LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

	/* 清零该页并为此写一条 XLOG 记录 */
	ZeroMultiXactOffsetPage(pageno, true);

	LWLockRelease(MultiXactOffsetSLRULock);
}

/*
 * 确保 MultiXactMember 有空间
 * 容纳新分配 MultiXactId 的成员。
 *
 * 与上面的例程一样，调用时持有
 * MultiXactGenLock；同样的说明适用。
 */
static void
ExtendMultiXactMember(MultiXactOffset offset, int nmembers)
{
	/*
	 * 成员可能跨越 members 文件
	 * 的多页，故我们循环以确保考虑每一页。
	 * 若成员跨越数页，编码并非最优，但
	 * 那种情况似乎少见，不必太担心。
	 */
	while (nmembers > 0)
	{
		int			flagsoff;
		int			flagsbit;
		uint32		difference;

		/*
		 * 仅在页的第一个表项处清零。
		 */
		flagsoff = MXOffsetToFlagsOffset(offset);
		flagsbit = MXOffsetToFlagsBitShift(offset);
		if (flagsoff == 0 && flagsbit == 0)
		{
			int			pageno;

			pageno = MXOffsetToMemberPage(offset);

			LWLockAcquire(MultiXactMemberSLRULock, LW_EXCLUSIVE);

			/* 清零该页并为此写一条 XLOG 记录 */
			ZeroMultiXactMemberPage(pageno, true);

			LWLockRelease(MultiXactMemberSLRULock);
		}

		/*
		 * 计算到当前页末的项数。小心：若无
		 * 符号整数加法回卷，我们就在最后一
		 * 段的最后一页；由于该页容纳的项
		 * 数与其他页不同，需要不同处理。
		 */
		if (offset + MAX_MEMBERS_IN_LAST_MEMBERS_PAGE < offset)
		{
			/*
			 * 这是最后一段的最后一页；
			 * 无需取模运算即可算出
			 * 其中还可分配的项数。
			 */
			difference = MaxMultiXactOffset - offset + 1;
		}
		else
			difference = MULTIXACT_MEMBERS_PER_PAGE - offset % MULTIXACT_MEMBERS_PER_PAGE;

		/*
		 * 前进到下一页，注意正确处理回卷情形。
		 * nmembers 变为负也可以。
		 */
		nmembers -= difference;
		offset += difference;
	}
}

/*
 * GetOldestMultiXactId
 *
 * 返回仍可能被任何运行中事务视为存活的最老
 * MultiXactId。更老的可能仍存在
 * 于磁盘，但已没有任何运行中的成员事务。
 *
 * 按本函数返回值截断 MultiXact SLRU 段并不安全；
 * 不过，若 VACUUM 确知某表不再有 < 该值的 MXIDs，
 * 可把它设为该表的新 relminmxid。只有当没有任何
 * 表仍可能有引用 MXID 时，截断 SLRUs 才安全。
 */
MultiXactId
GetOldestMultiXactId(void)
{
	MultiXactId oldestMXact;
	MultiXactId nextMXact;
	int			i;

	/*
	 * 这是所有 OldestMemberMXactId[] 与
	 * OldestVisibleMXactId[] 表项中最老的有效值；若皆无效则为 nextMXact。
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	/*
	 * 必须提防 nextMXact 处于已
	 * 回卷状态的可能。我们此处不修正计数器
	 * 本身，但必须确保在计算中使用合法值。
	 */
	nextMXact = MultiXactState->nextMXact;
	if (nextMXact < FirstMultiXactId)
		nextMXact = FirstMultiXactId;

	oldestMXact = nextMXact;
	for (i = 1; i <= MaxOldestSlot; i++)
	{
		MultiXactId thisoldest;

		thisoldest = OldestMemberMXactId[i];
		if (MultiXactIdIsValid(thisoldest) &&
			MultiXactIdPrecedes(thisoldest, oldestMXact))
			oldestMXact = thisoldest;
		thisoldest = OldestVisibleMXactId[i];
		if (MultiXactIdIsValid(thisoldest) &&
			MultiXactIdPrecedes(thisoldest, oldestMXact))
			oldestMXact = thisoldest;
	}

	LWLockRelease(MultiXactGenLock);

	return oldestMXact;
}

/*
 * 确定为防止成员回卷
 * 需要多积极地清理。
 *
 * 为此确定最老的成员偏移，并把限制信息安装到
 * MultiXactState，以便用来防
 * 止成员 SLRU 区域中的旧数据被覆写。
 *
 * 返回值为 true 表示需要紧急
 * 自动清理，否则为 false。
 */
static bool
SetOffsetVacuumLimit(bool is_startup)
{
	MultiXactId oldestMultiXactId;
	MultiXactId nextMXact;
	MultiXactOffset oldestOffset = 0;	/* 安抚编译器 */
	MultiXactOffset prevOldestOffset;
	MultiXactOffset nextOffset;
	bool		oldestOffsetKnown = false;
	bool		prevOldestOffsetKnown;
	MultiXactOffset offsetStopLimit = 0;
	MultiXactOffset prevOffsetStopLimit;

	/*
	 * 注意：必须防止并发截断，否则我们可能去查找一
	 * 个正被并发截断掉的 oldestMulti。
	 */
	LWLockAcquire(MultiXactTruncationLock, LW_SHARED);

	/* 从共享内存读取相关字段。 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	oldestMultiXactId = MultiXactState->oldestMultiXactId;
	nextMXact = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;
	prevOldestOffsetKnown = MultiXactState->oldestOffsetKnown;
	prevOldestOffset = MultiXactState->oldestOffset;
	prevOffsetStopLimit = MultiXactState->offsetStopLimit;
	Assert(MultiXactState->finishedStartup);
	LWLockRelease(MultiXactGenLock);

	/*
	 * 确定最老多事务的偏移。通常可从该多事
	 * 务本身读取偏移，但有一个重要特例：若
	 * 根本不存在任何多事务，oldestMXact
	 * 显然不能指向一个。它会转而指向下
	 * 次需要时将分配的多事务 ID。
	 */
	if (oldestMultiXactId == nextMXact)
	{
		/*
		 * 当下一个多事务被创建时，
		 * 它将存储在下一个偏移处。
		 */
		oldestOffset = nextOffset;
		oldestOffsetKnown = true;
	}
	else
	{
		/*
		 * 弄清最老的现存多事务的偏移存在何处。由于
		 * PostgreSQL 9.3.X 与 9.4.
		 * X 早期版本中的缺陷，所谓最早的多事务可能
		 * 实际不存在。我们小心不在那种情况下失败。
		 */
		oldestOffsetKnown =
			find_multixact_start(oldestMultiXactId, &oldestOffset);

		if (oldestOffsetKnown)
			ereport(DEBUG1,
					(errmsg_internal("oldest MultiXactId member is at offset %u",
									 oldestOffset)));
		else
			ereport(LOG,
					(errmsg("MultiXact member wraparound protections are disabled because oldest checkpointed MultiXact %u does not exist on disk",
							oldestMultiXactId)));
	}

	LWLockRelease(MultiXactTruncationLock);

	/*
	 * 若可以，计算限制（并安装到 MultiXactState
	 * ）以防止成员 SLRU 区域中的旧数据被覆
	 * 写。不过只有在最老偏移已知时才能这样做。
	 */
	if (oldestOffsetKnown)
	{
		/* 回退到对应段的起始处 */
		offsetStopLimit = oldestOffset - (oldestOffset %
										  (MULTIXACT_MEMBERS_PER_PAGE * SLRU_PAGES_PER_SEGMENT));

		/* 在回卷点之前总是保留一个段 */
		offsetStopLimit -= (MULTIXACT_MEMBERS_PER_PAGE * SLRU_PAGES_PER_SEGMENT);

		if (!prevOldestOffsetKnown && !is_startup)
			ereport(LOG,
					(errmsg("MultiXact member wraparound protections are now enabled")));

		ereport(DEBUG1,
				(errmsg_internal("MultiXact member stop limit is now %u based on MultiXact %u",
								 offsetStopLimit, oldestMultiXactId)));
	}
	else if (prevOldestOffsetKnown)
	{
		/*
		 * 若这次未能取得最老偏移，
		 * 但此前通过本函数时有值，
		 * 则使用旧值，而不是再次自
		 * 动强制紧急自动清理周期。
		 */
		oldestOffset = prevOldestOffset;
		oldestOffsetKnown = true;
		offsetStopLimit = prevOffsetStopLimit;
	}

	/* 安装计算出的值 */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->oldestOffset = oldestOffset;
	MultiXactState->oldestOffsetKnown = oldestOffsetKnown;
	MultiXactState->offsetStopLimit = offsetStopLimit;
	LWLockRelease(MultiXactGenLock);

	/*
	 * 我们是否需要紧急自动清理？ 若不确定，假定需要。
	 */
	return !oldestOffsetKnown ||
		(nextOffset - oldestOffset > MULTIXACT_MEMBER_SAFE_THRESHOLD);
}

/*
 * 返回把 "distance" 加到 "start" 是否会越过 "boundary"。
 *
 * 我们用它判断加法是否"绕过"了边界
 * 点，故有此名。这里不想用常规的 2
 * ^31 模算术，是因为我们希望能
 * 用满整个 2^32-1 空间，从
 * 而允许比否则能装下的更多多事务。
 */
static bool
MultiXactOffsetWouldWrap(MultiXactOffset boundary, MultiXactOffset start,
						 uint32 distance)
{
	MultiXactOffset finish;

	/*
	 * 注意偏移号 0 不使用（见 GetMultiXactIdMembers
	 * ），因此若加法绕过 UINT_MAX 边界，则跳过该值。
	 */
	finish = start + distance;
	if (finish < start)
		finish++;

	/*-----------------------------------------------------------------------
	 * 当边界在数值上大于起点时，两
	 * 者之间的任何数值都不算回卷：
	 *
	 *	<----S----B---->
	 *	[---)			 = F wrapped past B (and UINT_MAX)
	 *		 [---)		 = F not wrapped
	 *			  [----] = F wrapped past B
	 *
	 * 当边界在数值上小于起点时（即
	 * UINT_MAX 回卷发生在两者
	 * 之间），则中间的所有值都算回卷：
	 *
	 *	<----B----S---->
	 *	[---)			 = F not wrapped past B (but wrapped past UINT_MAX)
	 *		 [---)		 = F wrapped past B (and UINT_MAX)
	 *			  [----] = F not wrapped
	 *-----------------------------------------------------------------------
	 */
	if (start < boundary)
		return finish >= boundary || finish < start;
	else
		return finish >= boundary && finish < start;
}

/*
 * 查找给定 MultiXactId 的起始偏移。
 *
 * 若包含该 multi 的文件在磁盘上不存在则返回 false。
 * 否则返回 true，并把 *result 设为起始成员偏移。
 *
 * 本函数不阻止并发截断，若
 * 需要则由调用方负责防护。
 */
static bool
find_multixact_start(MultiXactId multi, MultiXactOffset *result)
{
	MultiXactOffset offset;
	int			pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;

	Assert(MultiXactState->finishedStartup);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/*
	 * 写出脏数据，以便 PhysicalPageExists 能正确工作。
	 */
	SimpleLruWriteAll(MultiXactOffsetCtl, true);
	SimpleLruWriteAll(MultiXactMemberCtl, true);

	if (!SimpleLruDoesPhysicalPageExist(MultiXactOffsetCtl, pageno))
		return false;

	/* 锁由 SimpleLruReadPage_ReadOnly 获取 */
	slotno = SimpleLruReadPage_ReadOnly(MultiXactOffsetCtl, pageno, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;
	LWLockRelease(MultiXactOffsetSLRULock);

	*result = offset;
	return true;
}

/*
 * 确定当前存在多少多事务，以及多少多事务
 * 成员。若无法确定则返回 false。
 */
static bool
ReadMultiXactCounts(uint32 *multixacts, MultiXactOffset *members)
{
	MultiXactOffset nextOffset;
	MultiXactOffset oldestOffset;
	MultiXactId oldestMultiXactId;
	MultiXactId nextMultiXactId;
	bool		oldestOffsetKnown;

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextOffset = MultiXactState->nextOffset;
	oldestMultiXactId = MultiXactState->oldestMultiXactId;
	nextMultiXactId = MultiXactState->nextMXact;
	oldestOffset = MultiXactState->oldestOffset;
	oldestOffsetKnown = MultiXactState->oldestOffsetKnown;
	LWLockRelease(MultiXactGenLock);

	if (!oldestOffsetKnown)
		return false;

	*members = nextOffset - oldestOffset;
	*multixacts = nextMultiXactId - oldestMultiXactId;
	return true;
}

/*
 * 一旦引用它们的多事务都早于每个 datminmxid，
 * 多事务成员就可以移除。
 * autovacuum_multixact_freeze_max_age
 * 与
 * vacuum_multixact_freeze_table_age
 * 共同确保我们从不会有过多多事务；我们希望至少在正常情况下，这也足
 * 以避免使用过多偏移。然而，若平均每个多事务有很多成员，我们可能在
 * 成员空间耗尽时，仍因成员用量不够高而未能触发 VACUUM 推进
 * relminmxid。那时就只好让创建多事务的操作以错误失败。
 *
 * 为防止如此，若成员空间的使用超过某个阈值比例，我
 * 们就有效地把
 * autovacuum_multixact_freeze_max_age
 * 降到略小于正在使用的多事务数。我们希望这能迅速触发
 * 对具有最老 relminmxid 的表的自动清理，
 * 从而使 datminmxid 推进并移除一些成员。
 *
 * 随着当前成员空间使用比例增大，我们在钳制该值时会更
 * 加积极。这不仅使自动清理加速，也使用户发出的任何手
 * 工清理更积极。这是因为 vacuum_get_cutoffs
 * () 会根据本函数返回的有效
 * autovacuum_multixact_freeze_max_age
 * 钳制冻结表与最小冻结年龄截止。最坏情况下，我
 * 们会声称 freeze_max_age 为零，
 * 于是对任何表的每次清理都会冻结每个多事务。
 */
int
MultiXactMemberFreezeThreshold(void)
{
	MultiXactOffset members;
	uint32		multixacts;
	uint32		victim_multixacts;
	double		fraction;
	int			result;

	/* 若无法确定成员空间利用率，则按最坏情况假定。 */
	if (!ReadMultiXactCounts(&multixacts, &members))
		return 0;

	/* 若成员空间利用率较低，则无需特殊动作。 */
	if (members <= MULTIXACT_MEMBER_SAFE_THRESHOLD)
		return autovacuum_multixact_freeze_max_age;

	/*
	 * 计算 relminmxid 推进的目标。我们试图从系统
	 * 中消除的多事务数，取决于我们超过
	 * MULTIXACT_MEMBER_SAFE_THRESHOLD 有多远。
	 */
	fraction = (double) (members - MULTIXACT_MEMBER_SAFE_THRESHOLD) /
		(MULTIXACT_MEMBER_DANGER_THRESHOLD - MULTIXACT_MEMBER_SAFE_THRESHOLD);
	victim_multixacts = multixacts * fraction;

	/* fraction 可能 > 1.0，但最低可能的冻结年龄为零 */
	if (victim_multixacts > multixacts)
		return 0;
	result = multixacts - victim_multixacts;

	/*
	 * 钳制到
	 * autovacuum_multixact_freeze_max_age，使我们从不让自动清理比原本更不积极。
	 */
	return Min(result, autovacuum_multixact_freeze_max_age);
}

typedef struct mxtruncinfo
{
	int			earliestExistingPage;
} mxtruncinfo;

/*
 * SlruScanDirectory
 *		回调。本回调确定现存最早的页号。
 */
static bool
SlruScanDirCbFindEarliest(SlruCtl ctl, char *filename, int segpage, void *data)
{
	mxtruncinfo *trunc = (mxtruncinfo *) data;

	if (trunc->earliestExistingPage == -1 ||
		ctl->PagePrecedes(segpage, trunc->earliestExistingPage))
	{
		trunc->earliestExistingPage = segpage;
	}

	return false;				/* 继续 */
}


/*
 * 删除成员段 [oldest, newOldest)
 *
 * 与 offsets 的 SLRU 不同，members SLRU
 * 几乎可以一次填满整个范围。这意味着不能简单地使用
 * SimpleLruTruncate()——而是用 offsets SLRU
 * 计算待删除范围。参见 TruncateMultiXact()。
 */
static void
PerformMembersTruncation(MultiXactOffset oldestOffset, MultiXactOffset newOldestOffset)
{
	const int	maxsegment = MXOffsetToMemberSegment(MaxMultiXactOffset);
	int			startsegment = MXOffsetToMemberSegment(oldestOffset);
	int			endsegment = MXOffsetToMemberSegment(newOldestOffset);
	int			segment = startsegment;

	/*
	 * 删除除最后一段外的所有段。最后一段仍
	 * 可能包含（可能是部分的）有效数据。
	 */
	while (segment != endsegment)
	{
		elog(DEBUG2, "truncating multixact members segment %x", segment);
		SlruDeleteSegment(MultiXactMemberCtl, segment);

		/* 移到下一段，正确处理回卷 */
		if (segment == maxsegment)
			segment = 0;
		else
			segment += 1;
	}
}

/*
 * 删除偏移段 [oldest, newOldest)
 */
static void
PerformOffsetsTruncation(MultiXactId oldestMulti, MultiXactId newOldestMulti)
{
	/*
	 * 我们后退一个多事务，以避免在 oldestMulti
	 * 恰好是某页第一项且 oldestMulti
	 * == nextMulti 的罕见情况下传入尚
	 * 未创建的截止页。那时若不减一，就会触发
	 * SimpleLruTruncate 的回卷检测。
	 */
	SimpleLruTruncate(MultiXactOffsetCtl,
					  MultiXactIdToOffsetPage(PreviousMultiXactId(newOldestMulti)));
}

/*
 * 移除所有仍有用的最老段之前的 MultiXactOffset
 * 与 MultiXactMember 段。
 *
 * 这只在主库上作为清理的一部分（通过
 * vac_truncate_clog()）调用。恢复
 * 期间截断通过重放此处记录的截断 WAL 记录完成。
 *
 * newOldestMulti 是当前仍需要的最老多事务，
 * newOldestMultiDB 是阻止 newOldestMulti 增大的数据库之一。
 */
void
TruncateMultiXact(MultiXactId newOldestMulti, Oid newOldestMultiDB)
{
	MultiXactId oldestMulti;
	MultiXactId nextMulti;
	MultiXactOffset newOldestOffset;
	MultiXactOffset oldestOffset;
	MultiXactOffset nextOffset;
	mxtruncinfo trunc;
	MultiXactId earliest;

	Assert(!RecoveryInProgress());
	Assert(MultiXactState->finishedStartup);

	/*
	 * 我们一次只允许一次截断。否则在我们做查找
	 * 等操作时，members 的部分可能消
	 * 失。无需与创建新 multi 等加互锁，
	 * 因为那些受限制约束（限制只增不减）。
	 */
	LWLockAcquire(MultiXactTruncationLock, LW_EXCLUSIVE);

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextMulti = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;
	oldestMulti = MultiXactState->oldestMultiXactId;
	LWLockRelease(MultiXactGenLock);
	Assert(MultiXactIdIsValid(oldestMulti));

	/*
	 * 确保只在有值可截断时才尝试截断。
	 * 正常处理中值不应回退，但有些角
	 * 落情况（由于缺陷）可能发生。
	 */
	if (MultiXactIdPrecedesOrEquals(newOldestMulti, oldestMulti))
	{
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	/*
	 * 注意我们不能直接一头扎进截断；可能根本
	 * 没有可截断的段，这是个问题，因为我们
	 * 要尝试读偏移页以确定从何处截断 members
	 * SLRU。因此我们先扫描目录，
	 * 确定能无错读取的最早偏移页号。
	 *
	 * 当 nextMXact 距 multiWrapLimit 不
	 * 足一个段时，SlruScanDirCbFindEarliest
	 * 可能找到某个早期段而非真正最早的。（
	 * MultiXactOffsetPagePrecedes(EARLIEST,
	 * LATEST) 返回 false，因为并非所有表项对都有相
	 * 同答案。）先前截断尝试 unlink() 失败或从本函数提前
	 * 返回时也可能如此。唯一后果是提前返回，浪费本可释放的空间。
	 *
	 * 注意：oldestMulti 所在页也可能已被截断掉，
	 * 而我们在更新 oldestMulti 之前崩溃了。
	 */
	trunc.earliestExistingPage = -1;
	SlruScanDirectory(MultiXactOffsetCtl, SlruScanDirCbFindEarliest, &trunc);
	earliest = trunc.earliestExistingPage * MULTIXACT_OFFSETS_PER_PAGE;
	if (earliest < FirstMultiXactId)
		earliest = FirstMultiXactId;

	/* 若没什么可移除，我们可以尽早退出。 */
	if (MultiXactIdPrecedes(oldestMulti, earliest))
	{
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	/*
	 * 首先，计算 MultiXactMember
	 * 的安全截断点。这是最老多事务的起始偏移。
	 *
	 * 希望 find_multixact_start 在此总能
	 * 工作，因为我们已经检查过它不早于磁盘上最早的 MultiXact。
	 * 但若失败，则不要截断任何东西，并记录一条消息。
	 */
	if (oldestMulti == nextMulti)
	{
		/* 没有 MultiXacts */
		oldestOffset = nextOffset;
	}
	else if (!find_multixact_start(oldestMulti, &oldestOffset))
	{
		ereport(LOG,
				(errmsg("oldest MultiXact %u not found, earliest MultiXact %u, skipping truncation",
						oldestMulti, earliest)));
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	/*
	 * 其次计算截断到何处。为此查找
	 * newOldestMulti 对应的成员偏移。
	 */
	if (newOldestMulti == nextMulti)
	{
		/* 没有 MultiXacts */
		newOldestOffset = nextOffset;
	}
	else if (!find_multixact_start(newOldestMulti, &newOldestOffset))
	{
		ereport(LOG,
				(errmsg("cannot truncate up to MultiXact %u because it does not exist on disk, skipping truncation",
						newOldestMulti)));
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	elog(DEBUG1, "performing multixact truncation: "
		 "offsets [%u, %u), offsets segments [%x, %x), "
		 "members [%u, %u), members segments [%x, %x)",
		 oldestMulti, newOldestMulti,
		 MultiXactIdToOffsetSegment(oldestMulti),
		 MultiXactIdToOffsetSegment(newOldestMulti),
		 oldestOffset, newOldestOffset,
		 MXOffsetToMemberSegment(oldestOffset),
		 MXOffsetToMemberSegment(newOldestOffset));

	/*
	 * 在临界区中做截断及其 WAL 记录。这样 offsets
	 * /members 就不会再不同步，即一旦一致，
	 * 即使我们在错误时刻崩溃，newOldestMulti
	 * 也总会存在于 members 中。
	 */
	START_CRIT_SECTION();

	/*
	 * 防止检查点被并发调度。这
	 * 很关键，否则崩溃/基础备
	 * 份后可能不重放截断记录，
	 * 尽管数据目录状态需要它。
	 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	/* WAL 记录截断 */
	WriteMTruncateXlogRec(newOldestMultiDB,
						  oldestMulti, newOldestMulti,
						  oldestOffset, newOldestOffset);

	/*
	 * 在执行截断之前、仍在临界区内时更新
	 * 内存中的限制：必须在截断前做，以
	 * 防止并发查找这些值。必须在临界区
	 * 内，否则若调用方在更新限制前崩溃，
	 * 将来对本函数的调用在 offsets
	 * 中查找最老成员时会出错。
	 */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->oldestMultiXactId = newOldestMulti;
	MultiXactState->oldestMultiXactDB = newOldestMultiDB;
	LWLockRelease(MultiXactGenLock);

	/* 先截断 members */
	PerformMembersTruncation(oldestOffset, newOldestOffset);

	/* 再截断 offsets */
	PerformOffsetsTruncation(oldestMulti, newOldestMulti);

	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

	END_CRIT_SECTION();
	LWLockRelease(MultiXactTruncationLock);
}

/*
 * 判定某个 MultiXactOffset 页号对截断而言是
 * 否"更早"。类似于 CLOGPagePrecedes()。
 *
 * 对值做偏移是可选的，因为
 * MultiXactIdPrecedes() 具有平移对称性。
 */
static bool
MultiXactOffsetPagePrecedes(int page1, int page2)
{
	MultiXactId multi1;
	MultiXactId multi2;

	multi1 = ((MultiXactId) page1) * MULTIXACT_OFFSETS_PER_PAGE;
	multi1 += FirstMultiXactId + 1;
	multi2 = ((MultiXactId) page2) * MULTIXACT_OFFSETS_PER_PAGE;
	multi2 += FirstMultiXactId + 1;

	return (MultiXactIdPrecedes(multi1, multi2) &&
			MultiXactIdPrecedes(multi1,
								multi2 + MULTIXACT_OFFSETS_PER_PAGE - 1));
}

/*
 * 判定某个 MultiXactMember 页号对截断而言
 * 是否"更早"。没有"无效偏移号"，故直接使用这些数字。
 */
static bool
MultiXactMemberPagePrecedes(int page1, int page2)
{
	MultiXactOffset offset1;
	MultiXactOffset offset2;

	offset1 = ((MultiXactOffset) page1) * MULTIXACT_MEMBERS_PER_PAGE;
	offset2 = ((MultiXactOffset) page2) * MULTIXACT_MEMBERS_PER_PAGE;

	return (MultiXactOffsetPrecedes(offset1, offset2) &&
			MultiXactOffsetPrecedes(offset1,
									offset2 + MULTIXACT_MEMBERS_PER_PAGE - 1));
}

/*
 * 判定两个 MultiXactIds 哪个更早。
 *
 * XXX 是否需要对 InvalidMultiXactId
 * 做特殊处理？（看起来不需要。）
 */
bool
MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff < 0);
}

/*
 * MultiXactIdPrecedesOrEquals -- multi1 在逻辑上是否 <= multi2？
 *
 * XXX 是否需要对 InvalidMultiXactId
 * 做特殊处理？（看起来不需要。）
 */
bool
MultiXactIdPrecedesOrEquals(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff <= 0);
}


/*
 * 判定两个偏移哪个更早。
 */
static bool
MultiXactOffsetPrecedes(MultiXactOffset offset1, MultiXactOffset offset2)
{
	int32		diff = (int32) (offset1 - offset2);

	return (diff < 0);
}

/*
 * 写一条反映清零 MEMBERs 或 OFFSETs
 * 页的 xlog 记录（info 指明是哪一种）
 */
static void
WriteMZeroPageXlogRec(int pageno, uint8 info)
{
	XLogBeginInsert();
	XLogRegisterData((char *) (&pageno), sizeof(int));
	(void) XLogInsert(RM_MULTIXACT_ID, info);
}

/*
 * 写一条 TRUNCATE xlog 记录
 *
 * 返回前必须把该 xlog 记录刷到磁盘——见
 * TruncateCLOG() 中的说明。
 */
static void
WriteMTruncateXlogRec(Oid oldestMultiDB,
					  MultiXactId startTruncOff, MultiXactId endTruncOff,
					  MultiXactOffset startTruncMemb, MultiXactOffset endTruncMemb)
{
	XLogRecPtr	recptr;
	xl_multixact_truncate xlrec;

	xlrec.oldestMultiDB = oldestMultiDB;

	xlrec.startTruncOff = startTruncOff;
	xlrec.endTruncOff = endTruncOff;

	xlrec.startTruncMemb = startTruncMemb;
	xlrec.endTruncMemb = endTruncMemb;

	XLogBeginInsert();
	XLogRegisterData((char *) (&xlrec), SizeOfMultiXactTruncate);
	recptr = XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_TRUNCATE_ID);
	XLogFlush(recptr);
}

/*
 * MULTIXACT 资源管理器的例程
 */
void
multixact_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* 多事务记录不使用备份块 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(MultiXactOffsetSLRULock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactOffsetPage(pageno, false);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno);
		Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactOffsetSLRULock);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(MultiXactMemberSLRULock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactMemberPage(pageno, false);
		SimpleLruWritePage(MultiXactMemberCtl, slotno);
		Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactMemberSLRULock);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec =
			(xl_multixact_create *) XLogRecGetData(record);
		TransactionId max_xid;
		int			i;

		/* 把数据存回 SLRU 文件 */
		RecordNewMultiXact(xlrec->mid, xlrec->moff, xlrec->nmembers,
						   xlrec->members);

		/* 确保 nextMXact/nextOffset 超过本记录所含内容 */
		MultiXactAdvanceNextMXact(xlrec->mid + 1,
								  xlrec->moff + xlrec->nmembers);

		/*
		 * 确保 nextXid 超过记录中提到的任何 XID。
		 * 这本应不必要，因为这里找到的任何 XID 在 XLOG
		 * 中应有其他证据，但为安全起见还是做一下。
		 */
		max_xid = XLogRecGetXid(record);
		for (i = 0; i < xlrec->nmembers; i++)
		{
			if (TransactionIdPrecedes(max_xid, xlrec->members[i].xid))
				max_xid = xlrec->members[i].xid;
		}

		AdvanceNextFullTransactionIdPastXid(max_xid);
	}
	else if (info == XLOG_MULTIXACT_TRUNCATE_ID)
	{
		xl_multixact_truncate xlrec;
		int			pageno;

		memcpy(&xlrec, XLogRecGetData(record),
			   SizeOfMultiXactTruncate);

		elog(DEBUG1, "replaying multixact truncation: "
			 "offsets [%u, %u), offsets segments [%x, %x), "
			 "members [%u, %u), members segments [%x, %x)",
			 xlrec.startTruncOff, xlrec.endTruncOff,
			 MultiXactIdToOffsetSegment(xlrec.startTruncOff),
			 MultiXactIdToOffsetSegment(xlrec.endTruncOff),
			 xlrec.startTruncMemb, xlrec.endTruncMemb,
			 MXOffsetToMemberSegment(xlrec.startTruncMemb),
			 MXOffsetToMemberSegment(xlrec.endTruncMemb));

		/* 本不应需要，但成本足够低 */
		LWLockAcquire(MultiXactTruncationLock, LW_EXCLUSIVE);

		/*
		 * 推进地平线值，使它们在
		 * 恢复结束时是最新的。
		 */
		SetMultiXactIdLimit(xlrec.endTruncOff, xlrec.oldestMultiDB, false);

		PerformMembersTruncation(xlrec.startTruncMemb, xlrec.endTruncMemb);

		/*
		 * 在 XLOG 重放期间，latest_page_number
		 * 不一定已设置；插入一个合适的值以绕过
		 * SimpleLruTruncate 中的健全性检查。
		 */
		pageno = MultiXactIdToOffsetPage(xlrec.endTruncOff);
		MultiXactOffsetCtl->shared->latest_page_number = pageno;
		PerformOffsetsTruncation(xlrec.startTruncOff, xlrec.endTruncOff);

		LWLockRelease(MultiXactTruncationLock);
	}
	else
		elog(PANIC, "multixact_redo: unknown op code %u", info);
}

Datum
pg_get_multixact_members(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		MultiXactMember *members;
		int			nmembers;
		int			iter;
	} mxact;
	MultiXactId mxid = PG_GETARG_TRANSACTIONID(0);
	mxact	   *multi;
	FuncCallContext *funccxt;

	if (mxid < FirstMultiXactId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid MultiXactId: %u", mxid)));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		funccxt = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funccxt->multi_call_memory_ctx);

		multi = palloc(sizeof(mxact));
		/* 此处无需允许旧值 */
		multi->nmembers = GetMultiXactIdMembers(mxid, &multi->members, false,
												false);
		multi->iter = 0;

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "return type must be a row type");
		funccxt->tuple_desc = tupdesc;
		funccxt->attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funccxt->user_fctx = multi;

		MemoryContextSwitchTo(oldcxt);
	}

	funccxt = SRF_PERCALL_SETUP();
	multi = (mxact *) funccxt->user_fctx;

	while (multi->iter < multi->nmembers)
	{
		HeapTuple	tuple;
		char	   *values[2];

		values[0] = psprintf("%u", multi->members[multi->iter].xid);
		values[1] = mxstatus_to_string(multi->members[multi->iter].status);

		tuple = BuildTupleFromCStrings(funccxt->attinmeta, values);

		multi->iter++;
		pfree(values[0]);
		SRF_RETURN_NEXT(funccxt, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funccxt);
}

/*
 * sync.c 同步 offsets 文件的入口点。
 */
int
multixactoffsetssyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(MultiXactOffsetCtl, ftag, path);
}

/*
 * sync.c 同步 members 文件的入口点。
 */
int
multixactmemberssyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(MultiXactMemberCtl, ftag, path);
}
