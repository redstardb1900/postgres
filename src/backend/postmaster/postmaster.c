/*-------------------------------------------------------------------------
 *
 * postmaster.c
 *	  本程序是 POSTGRES 系统的请求中转站。        
 *	  前端程序向 Postmaster 发启动消息，postmaster   
 *	  用消息中的信息来设置一个后端进程。      
 *	                                     
 *
 *	  postmaster 还负责管理系统级操作，例如启动  
 *	  与关机。postmaster 本身并不执行这些操作，    
 *	  而是在恰当时机 fork 出子进程来执行。它还会在   
 *	  后端崩溃时负责重置系统。                           
 *	                       
 *
 *	  postmaster 在启动时创建共享内存与信号量池，   
 *	  但按惯例并不亲自访问它们。它尤其不是后端   
 *	  PGPROC 数组的成员，因此无法参与锁管理器操作。 
 *	  让 postmaster 远离共享内存操作会更简单可靠。     
 *	                                                                    
 *	  postmaster 几乎总能通过重置共享内存，从单个后端  
 *	  的崩溃中恢复；若它频繁使用共享内存，就可能
 *	  随同后端一起崩溃。                                         
 *	                          
 *
 *	  现在收到请求消息就 fork()。子进程对请求进行 
 *	  认证，成功后成为后端。这样认证代码便能以 
 *	  简单的单线程风格编写（而非过去所需的蹩脚    
 *	  "穷人式多任务"代码）。                                  
 *	                                                                
 *	  更重要的是，它确保 SSL 或 PAM 等非多线程库中   
 *	  的阻塞不会导致对其他客户端的拒绝服务。        
 *	          
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/postmaster.c
 *
 * NOTES
 *
 * 初始化：   
 *		Postmaster 为后端建共享内存数据结构。  
 *		                 
 *
 * 同步：       
 *		Postmaster 与后端共享内存，但应避免触碰它，    
 *		以免在崩溃的后端弄坏锁或共享内存时卡住。   
 *		同样，Postmaster 绝不应在前端客户端消息上阻塞。  
 *		                                                     
 *
 * 垃圾回收：    
 *		若后端紧急退出和/或核心转储，Postmaster 会清理  
 *		善后。             
 *
 * 错误报告： 
 *		write_stderr() 只用于报告"交互式"错误（基本  
 *		就是命令行参数无效）。postmaster 启动之后     
 *		应改用 ereport()。                
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/param.h>
#include <netdb.h>
#include <limits.h>

#ifdef USE_BONJOUR
#include <dns_sd.h>
#endif

#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#ifdef HAVE_PTHREAD_IS_THREADED_NP
#include <pthread.h>
#endif

#include "access/transam.h"
#include "access/xlog.h"
#include "access/xlogrecovery.h"
#include "catalog/pg_control.h"
#include "common/file_perm.h"
#include "common/ip.h"
#include "common/pg_prng.h"
#include "common/string.h"
#include "lib/ilist.h"
#include "libpq/auth.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqsignal.h"
#include "nodes/queryjumble.h"
#include "pg_getopt.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "postmaster/autovacuum.h"
#include "postmaster/auxprocess.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/fork_process.h"
#include "postmaster/interrupt.h"
#include "postmaster/pgarch.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "replication/logicallauncher.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/memutils.h"
#include "utils/pidfile.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#ifdef EXEC_BACKEND
#include "storage/spin.h"
#endif


/*
 * 后端类型的可能取值。除作为 struct bkend 的取值外，       
 * 它们也是 SignalSomeChildren() 与 CountChildren()                     
 * 可 OR 标志位。 
 */
#define BACKEND_TYPE_NORMAL		0x0001	/* 普通后端   */
#define BACKEND_TYPE_AUTOVAC	0x0002	/* autovacuum worker 进程  */
#define BACKEND_TYPE_WALSND		0x0004	/* walsender 进程  */
#define BACKEND_TYPE_BGWORKER	0x0008	/* bgworker 进程  */
#define BACKEND_TYPE_ALL		0x000F	/* 以上各项的 OR  */

/*
 * 活动后端（或子进程）的列表；我们其实并不清楚某
 * 个子进程已变成后端还是仍在授权阶段。它主要   
 * 用于掌握我们拥有多少子进程，并在必要时向它们  
 * 发送适当的信号。                                          
 *
 * 如上面那组后端类型所示，此列表不仅包含"普通"的   
 * 客户端会话，还包括 autovacuum worker、walsender 和         
 * 后台工作进程。（注意：启动时 walsender 被标为       
 * BACKEND_TYPE_NORMAL；一旦注意到它们改动了               
 * PMChildFlags 表项，就重新标为 BACKEND_TYPE_WALSND。             
 * 因此，任何需要区分 walsender 与普通后端的操作，     
 * 都须先检查。） 
 *
 * 另外，"dead_end"子进程也在列表中：这类子进程的         
 * 唯一用途是向潜在客户端发送友好的拒绝消息。          
 * 我们必须跟踪它们，因为它们已挂在共享内存上，        
 * 但我们知道它们绝不会变成真正的后端。这类               
 * 子进程没有 PMChildSlot，bkend_type 是 NORMAL。 
 *
 * startup、bgwriter 和 autovacuum launcher 之类的"特殊"            
 * 子任务不在此列表中。它们通过 StartupPID 等下文的    
 * pid_t 变量来跟踪。（因此，任何给定的"特殊"子进       
 * 程类型至多只有一个。BackendList 表项用于所有           
 * 可能有多个的子进程。）      
 */
typedef struct bkend
{
	pid_t		pid;			/* 该后端的进程 ID */
	int32		cancel_key;		/* 此后端的取消密钥                */
	int			child_slot;		/* 此后端的 PMChildSlot（若有） */
	int			bkend_type;		/* 子进程类别，见上文     */
	bool		dead_end;		/* 它是否将发送错误并退出？   */
	bool		bgworker_notify;	/* 接收 bgworker 启停通知           */
	dlist_node	elem;			/* BackendList 链表项    */
} Backend;

static dlist_head BackendList = DLIST_STATIC_INIT(BackendList);

#ifdef EXEC_BACKEND
static Backend *ShmemBackendArray;
#endif

BackgroundWorker *MyBgworkerEntry = NULL;



/* 我们监听连接的套接字编号                  */
int			PostPortNumber = DEF_PGPORT;

/* Unix 套接字的目录名             */
char	   *Unix_socket_directories;

/* TCP 监听地址           */
char	   *ListenAddresses;

/*
 * SuperuserReservedConnections 是为超级用户保留的           
 * 后端槽数，ReservedConnections 是为拥有                          
 * pg_use_reserved_connections 预定义角色特权的角色         
 * 保留的后端槽数。它们从 MaxConnections 个后端槽的           
 * 池中扣除，因此既非超级用户又不具备                       
 * pg_use_reserved_connections 特权的角色可用槽数为     
 * (MaxConnections-SuperuserReservedConnections-ReservedConnections).    
 *
 * 如果剩余槽数小于等于                           
 * SuperuserReservedConnections，则只有超级用户可以                 
 * 建立新连接。如果剩余槽数大于 SuperuserReservedConnections   
 * 且小于等于          
 * (SuperuserReservedConnections + ReservedConnections)，                  
 * 则只有超级用户和拥有 pg_use_reserved_connections       
 * 特权的角色可新建连接。已有的超级  
 * 用户和 pg_use_reserved_connections 连接不计入这些限制。   
 */
int			SuperuserReservedConnections;
int			ReservedConnections;

/* 我们正在监听的套接字。 */
#define MAXLISTEN	64
static pgsocket ListenSocket[MAXLISTEN];

/* 还有更多选项变量    */
bool		EnableSSL = false;

int			PreAuthDelay = 0;
int			AuthenticationTimeout = 60;

bool		log_hostname;		/* 用于 ps 显示和日志  */
bool		Log_connections = false;
bool		Db_user_namespace = false;

bool		enable_bonjour = false;
char	   *bonjour_name;
bool		restart_after_crash = true;
bool		remove_temp_files_after_crash = true;
bool		send_abort_for_crash = false;
bool		send_abort_for_kill = false;

/* 特殊子进程的 PID；未运行时为 0          */
static pid_t StartupPID = 0,
			BgWriterPID = 0,
			CheckpointerPID = 0,
			WalWriterPID = 0,
			WalReceiverPID = 0,
			AutoVacPID = 0,
			PgArchPID = 0,
			SysLoggerPID = 0;

/* startup 进程的状态  */
typedef enum
{
	STARTUP_NOT_RUNNING,
	STARTUP_RUNNING,
	STARTUP_SIGNALED,			/* 已向它发送 SIGQUIT/SIGKILL */
	STARTUP_CRASHED
} StartupStatusEnum;

static StartupStatusEnum StartupStatus = STARTUP_NOT_RUNNING;

/* 启动/关机状态    */
#define			NoShutdown		0
#define			SmartShutdown	1
#define			FastShutdown	2
#define			ImmediateShutdown	3

static int	Shutdown = NoShutdown;

static bool FatalError = false; /* 从后端崩溃中恢复时为真  */

/*
 * 用简单的状态机来控制启动、关机以及崩溃恢复
 * （崩溃恢复颇像先关机再启动）。                      
 *
 * 完成 postmaster 的全部初始化工作后，我们进入           
 * PM_STARTUP 状态并启动 startup 进程。startup 进程              
 * 一开始会读取控制文件并做其他预备初始化。        
 * 正常启动或崩溃恢复之后，startup 进程以退出码         
 * 0 退出，随后我们切换到 PM_RUN 状态。而归档恢复          
 * 被特殊处理，因为它耗时更长，且我们希望在归档          
 * 恢复期间支持热备。         
 *
 * 当 startup 进程准备好开始归档恢复时，它会向               
 * postmaster 发信号，于是我们切换到 PM_RECOVERY                 
 * 状态，同时启动 bgwriter 和 checkpointer，startup                   
 * 进程则继续重放 WAL。若启用了热备，那么在 WAL           
 * 重放达到一致点后，startup 进程会再次发信号，我们   
 * 便切换到 PM_HOT_STANDBY 状态，开始接受连接以执            
 * 行只读查询。归档恢复完成后，startup 进程以退出       
 * 码 0 退出，转 PM_RUN。  
 *
 * 普通子后端只有在我们处于 PM_RUN 或 PM_HOT_STANDBY     
 * 状态时才能启动。（connsAllowed 也可限制启动。）  
 * 在其他状态下，我们通过启动"dead_end"子进程来        
 * 处理连接请求，这类子进程只会向客户端发送错误     
 * 消息然后退出。（我们在 BackendList 中跟踪它们，       
 * 以便知道它们何时全部消失；这一点很重要，因为       
 * 它们仍连接着共享内存，会妨碍销毁共享内存段的       
 * 尝试，可能在新建段时导致 SHMALL 失败。）          
 * 在 PM_WAIT_DEAD_END 状态下，我们等待所有 dead_end          
 * 子进程离开系统，因此会完全停止接受连接请求， 
 * 直到最后一个现存子进程退出（希望这一过程不会     
 * 太久）。           
 *
 * 注意，该状态变量并不区分我们进入 PM_RUN 之后       
 * 各状态的原因；须结合 Shutdown 与 FatalError 才能         
 * 判断。FatalError 在 PM_RECOVERY、PM_HOT_STANDBY、                    
 * PM_RUN 状态下均不为真，PM_SHUTDOWN 状态下也是               
 * （因为从崩溃恢复期间不会进入这些状态）。              
 * PM_STARTUP 状态下可为真，直到成功开始 WAL 重放才清除。  
 */
typedef enum
{
	PM_INIT,					/* postmaster 启动中*/
	PM_STARTUP,					/* 等待 startup 子进程       */
	PM_RECOVERY,				/* 归档恢复模式中    */
	PM_HOT_STANDBY,				/* 热备模式中     */
	PM_RUN,						/* 正常的"数据库存活"状态 */
	PM_STOP_BACKENDS,			/* 需要停止其余的后端     */
	PM_WAIT_BACKENDS,			/* 等待存活的后端退出       */
	PM_SHUTDOWN,				/* waiting for checkpointer to do shutdown
								 * ckpt */
	PM_SHUTDOWN_2,				/* waiting for archiver and walsenders to
								 * finish */
	PM_WAIT_DEAD_END,			/* 等待 dead_end 子进程退出       */
	PM_NO_CHILDREN				/* 所有重要子进程均已退出  */
} PMState;

static PMState pmState = PM_INIT;

/*
 * 执行"智能关闭"时，我们会限制新连接，但仍保持        
 * 在 PM_RUN 或 PM_HOT_STANDBY 状态，直到所有客户端             
 * 后端离开。connsAllowed 是子状态指示器，标示现行     
 * 限制；仅在pmState为PM_RUN/PM_HOT_STANDBY时才有意义。
 */
static bool connsAllowed = true;

/* 立即关闭或子进程崩溃期间 SIGKILL 超时的开始时间     */
/* 为零表示超时未在运行    */
static time_t AbortStartTime = 0;

/* 该超时的时长     */
#define SIGKILL_CHILDREN_AFTER_SECS		5

static bool ReachedNormalRunning = false;	/* 到达 PM_RUN 后为真   */

bool		ClientAuthInProgress = false;	/* T during new-client
											 * authentication */

bool		redirection_done = false;	/* stderr 已重定向给 syslogger?*/

/* 已收到 START_AUTOVAC_LAUNCHER 信号*/
static bool start_autovac_launcher = false;

/* 需要向 launcher 发信号以告知某种状态               */
static bool avlauncher_needs_signal = false;

/* 收到 START_WALRECEIVER 信号   */
static bool WalReceiverRequested = false;

/* 当有需要启动的 worker 时设置                */
static bool StartWorkerNeeded = true;
static bool HaveCrashedWorker = false;

/* 收到信号时设置   */
static volatile sig_atomic_t pending_pm_pmsignal;
static volatile sig_atomic_t pending_pm_child_exit;
static volatile sig_atomic_t pending_pm_reload_request;
static volatile sig_atomic_t pending_pm_shutdown_request;
static volatile sig_atomic_t pending_pm_fast_shutdown_request;
static volatile sig_atomic_t pending_pm_immediate_shutdown_request;

/* 事件多路复用对象  */
static WaitEventSet *pm_wait_set;

#ifdef USE_SSL
/* 当 SSL 已正确初始化时设置               */
static bool LoadedSSL = false;
#endif

#ifdef USE_BONJOUR
static DNSServiceRef bonjour_sdref = NULL;
#endif

/*
 * postmaster.c - 函数原型       
 */
static void CloseServerPorts(int status, Datum arg);
static void unlink_external_pid_file(int status, Datum arg);
static void getInstallationPaths(const char *argv0);
static void checkControlFile(void);
static Port *ConnCreate(int serverFd);
static void ConnFree(Port *port);
static void handle_pm_pmsignal_signal(SIGNAL_ARGS);
static void handle_pm_child_exit_signal(SIGNAL_ARGS);
static void handle_pm_reload_request_signal(SIGNAL_ARGS);
static void handle_pm_shutdown_request_signal(SIGNAL_ARGS);
static void process_pm_pmsignal(void);
static void process_pm_child_exit(void);
static void process_pm_reload_request(void);
static void process_pm_shutdown_request(void);
static void process_startup_packet_die(SIGNAL_ARGS);
static void dummy_handler(SIGNAL_ARGS);
static void StartupPacketTimeoutHandler(void);
static void CleanupBackend(int pid, int exitstatus);
static bool CleanupBackgroundWorker(int pid, int exitstatus);
static void HandleChildCrash(int pid, int exitstatus, const char *procname);
static void LogChildExit(int lev, const char *procname,
						 int pid, int exitstatus);
static void PostmasterStateMachine(void);
static void BackendInitialize(Port *port);
static void BackendRun(Port *port) pg_attribute_noreturn();
static void ExitPostmaster(int status) pg_attribute_noreturn();
static int	ServerLoop(void);
static int	BackendStartup(Port *port);
static int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
static void SendNegotiateProtocolVersion(List *unrecognized_protocol_options);
static void processCancelRequest(Port *port, void *pkt);
static void report_fork_failure_to_client(Port *port, int errnum);
static CAC_state canAcceptConnections(int backend_type);
static bool RandomCancelKey(int32 *cancel_key);
static void signal_child(pid_t pid, int signal);
static void sigquit_child(pid_t pid);
static bool SignalSomeChildren(int signal, int target);
static void TerminateChildren(int signal);

#define SignalChildren(sig)			   SignalSomeChildren(sig, BACKEND_TYPE_ALL)

static int	CountChildren(int target);
static bool assign_backendlist_entry(RegisteredBgWorker *rw);
static void maybe_start_bgworkers(void);
static bool CreateOptsFile(int argc, char *argv[], char *fullprogname);
static pid_t StartChildProcess(AuxProcType type);
static void StartAutovacuumWorker(void);
static void MaybeStartWalReceiver(void);
static void InitPostmasterDeathWatchHandle(void);

/*
 * 按当前 postmaster 状态，archiver 允许启动吗？       
 *
 * 若 WAL 归档始终启用，则即使在恢复期间也允许      
 * 启动 archiver。   
 */
#define PgArchStartupAllowed()	\
	(((XLogArchivingActive() && pmState == PM_RUN) ||			\
	  (XLogArchivingAlways() &&									  \
	   (pmState == PM_RECOVERY || pmState == PM_HOT_STANDBY))) && \
	 PgArchCanRestart())

#ifdef EXEC_BACKEND

#ifdef WIN32
#define WNOHANG 0				/* 参数被忽略，任意整数皆可  */

static pid_t waitpid(pid_t pid, int *exitstatus, int options);
static void WINAPI pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired);

static HANDLE win32ChildQueue;

typedef struct
{
	HANDLE		waitHandle;
	HANDLE		procHandle;
	DWORD		procId;
} win32_deadchild_waitinfo;
#endif							/* WIN32 */

static pid_t backend_forkexec(Port *port);
static pid_t internal_forkexec(int argc, char *argv[], Port *port);

/* 可继承给客户端进程的套接字类型               */
#ifdef WIN32
typedef struct
{
	SOCKET		origsocket;		/* Original socket value, or PGINVALID_SOCKET
								 * if not a socket */
	WSAPROTOCOL_INFO wsainfo;
} InheritableSocket;
#else
typedef int InheritableSocket;
#endif

/*
 * 结构体包含传给 exec 启动的后端的全部变量  
 */
typedef struct
{
	Port		port;
	InheritableSocket portsocket;
	char		DataDir[MAXPGPATH];
	pgsocket	ListenSocket[MAXLISTEN];
	int32		MyCancelKey;
	int			MyPMChildSlot;
#ifndef WIN32
	unsigned long UsedShmemSegID;
#else
	void	   *ShmemProtectiveRegion;
	HANDLE		UsedShmemSegID;
#endif
	void	   *UsedShmemSegAddr;
	slock_t    *ShmemLock;
	VariableCache ShmemVariableCache;
	Backend    *ShmemBackendArray;
#ifndef HAVE_SPINLOCKS
	PGSemaphore *SpinlockSemaArray;
#endif
	int			NamedLWLockTrancheRequests;
	NamedLWLockTranche *NamedLWLockTrancheArray;
	LWLockPadded *MainLWLockArray;
	slock_t    *ProcStructLock;
	PROC_HDR   *ProcGlobal;
	PGPROC	   *AuxiliaryProcs;
	PGPROC	   *PreparedXactProcs;
	PMSignalData *PMSignalState;
	pid_t		PostmasterPid;
	TimestampTz PgStartTime;
	TimestampTz PgReloadTime;
	pg_time_t	first_syslogger_file_time;
	bool		redirection_done;
	bool		IsBinaryUpgrade;
	bool		query_id_enabled;
	int			max_safe_fds;
	int			MaxBackends;
#ifdef WIN32
	HANDLE		PostmasterHandle;
	HANDLE		initial_signal_pipe;
	HANDLE		syslogPipe[2];
#else
	int			postmaster_alive_fds[2];
	int			syslogPipe[2];
#endif
	char		my_exec_path[MAXPGPATH];
	char		pkglib_path[MAXPGPATH];
} BackendParameters;

static void read_backend_variables(char *id, Port *port);
static void restore_backend_variables(BackendParameters *param, Port *port);

#ifndef WIN32
static bool save_backend_variables(BackendParameters *param, Port *port);
#else
static bool save_backend_variables(BackendParameters *param, Port *port,
								   HANDLE childProcess, pid_t childPid);
#endif

static void ShmemBackendArrayAdd(Backend *bn);
static void ShmemBackendArrayRemove(Backend *bn);
#endif							/* EXEC_BACKEND */

#define StartupDataBase()		StartChildProcess(StartupProcess)
#define StartArchiver()			StartChildProcess(ArchiverProcess)
#define StartBackgroundWriter() StartChildProcess(BgWriterProcess)
#define StartCheckpointer()		StartChildProcess(CheckpointerProcess)
#define StartWalWriter()		StartChildProcess(WalWriterProcess)
#define StartWalReceiver()		StartChildProcess(WalReceiverProcess)

/* 检查子进程退出状态的宏              */
#define EXIT_STATUS_0(st)  ((st) == 0)
#define EXIT_STATUS_1(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 1)
#define EXIT_STATUS_3(st)  (WIFEXITED(st) && WEXITSTATUS(st) == 3)

#ifndef WIN32
/*
 * 监视 postmaster 是否存活的管道文件描述符。        
 * 首为 POSTMASTER_FD_WATCH，次为 POSTMASTER_FD_OWN。  
 */
int			postmaster_alive_fds[2] = {-1, -1};
#else
/* Windows 上用于同一用途的 postmaster 进程句柄          */
HANDLE		PostmasterHandle;
#endif

/*
 * Postmaster 主入口       
 */
void
PostmasterMain(int argc, char *argv[])
{
	int			opt;
	int			status;
	char	   *userDoption = NULL;
	bool		listen_addr_saved = false;
	int			i;
	char	   *output_config_variable = NULL;

	InitProcessGlobals();

	PostmasterPid = MyProcPid;

	IsPostmasterEnvironment = true;

	/*
	 * 启动 win32 信号实现            
	 */
#ifdef WIN32
	pgwin32_signal_initialize();
#endif

	/*
	 * 在检查数据目录前不应创建任何文件或目录（见       
	 * checkDataDir()），但以防万一，先把 umask 设为最          
	 * 严格的（仅属主）权限。             
	 *
	 * checkDataDir() 会根据数据目录权限重置 umask。      
	 *             
	 */
	umask(PG_MODE_MASK_OWNER);

	/*
	 * 默认情况下，postmaster 中的 palloc() 请求会分配在     
	 * PostmasterContext——一块可被后端回收的空间。            
	 * 需要让后端可用的已分配数据应改在               
	 * TopMemoryContext 分配。    
	 */
	PostmasterContext = AllocSetContextCreate(TopMemoryContext,
											  "Postmaster",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(PostmasterContext);

	/* 初始化安装文件路径            */
	getInstallationPaths(argv[0]);

	/*
	 * 为 postmaster 进程设置信号处理器。     
	 *
	 * 注意：修改此清单时，须检查对子进程信号处理       
	 * 设置的副作用。参见 tcop/postgres.c、          
	 * bootstrap/bootstrap.c, postmaster/bgwriter.c, postmaster/walwriter.c,
	 * postmaster/autovacuum.c, postmaster/pgarch.c, postmaster/syslogger.c 
	 * postmaster/bgworker.c and postmaster/checkpointer.c 
	 */
	pqinitmask();
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	pqsignal(SIGHUP, handle_pm_reload_request_signal);
	pqsignal(SIGINT, handle_pm_shutdown_request_signal);
	pqsignal(SIGQUIT, handle_pm_shutdown_request_signal);
	pqsignal(SIGTERM, handle_pm_shutdown_request_signal);
	pqsignal(SIGALRM, SIG_IGN); /* 忽略  */
	pqsignal(SIGPIPE, SIG_IGN); /* 忽略  */
	pqsignal(SIGUSR1, handle_pm_pmsignal_signal);
	pqsignal(SIGUSR2, dummy_handler);	/* 未用，为子进程保留  */
	pqsignal(SIGCHLD, handle_pm_child_exit_signal);

	/* 视平台而定，此处可能配置 SIGURG。    */
	InitializeLatchSupport();
	InitProcessLocalLatch();

	/*
	 * Postgres 中其他任何地方都不得触碰 SIGTTIN/SIGTTOU 的    
	 * 处理。在 postmaster 环境里我们忽略这两个信号，这   
	 * 样子进程就不会因写 stderr 而有卡死风险。但对独     
	 * 立后端而言，它们的默认处理是合理的。因此，所     
	 * 有子进程都应直接沿用继承下来的设置。            
	 */
#ifdef SIGTTIN
	pqsignal(SIGTTIN, SIG_IGN); /* 忽略  */
#endif
#ifdef SIGTTOU
	pqsignal(SIGTTOU, SIG_IGN); /* 忽略  */
#endif

	/* 忽略 SIGXFSZ，使 ulimit 超限按磁盘满处理          */
#ifdef SIGXFSZ
	pqsignal(SIGXFSZ, SIG_IGN); /* 忽略  */
#endif

	/* 开始接收信号。    */
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);

	/*
	 * 选项设置 
	 */
	InitializeGUCOptions();

	opterr = 1;

	/*
	 * 解析命令行选项。注意：须与 tcop/postgres.c     
	 * 保持同步（选项集不应冲突），并与 main/main.c      
	 * 的公共 help() 函数一致。      
	 */
	while ((opt = getopt(argc, argv, "B:bC:c:D:d:EeFf:h:ijk:lN:OPp:r:S:sTt:W:-:")) != -1)
	{
		switch (opt)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'b':
				/* 用于二进制升级的未文档化开关 */
				IsBinaryUpgrade = true;
				break;

			case 'C':
				output_config_variable = strdup(optarg);
				break;

			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (opt == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					pfree(name);
					pfree(value);
					break;
				}

			case 'D':
				userDoption = strdup(optarg);
				break;

			case 'd':
				set_debug_options(atoi(optarg), PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'E':
				SetConfigOption("log_statement", "all", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'e':
				SetConfigOption("datestyle", "euro", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'f':
				if (!set_plan_disabling_options(optarg, PGC_POSTMASTER, PGC_S_ARGV))
				{
					write_stderr("%s: invalid argument for option -f: \"%s\"\n",
								 progname, optarg);
					ExitPostmaster(1);
				}
				break;

			case 'h':
				SetConfigOption("listen_addresses", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'i':
				SetConfigOption("listen_addresses", "*", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'j':
				/* 仅交互式后端使用         */
				break;

			case 'k':
				SetConfigOption("unix_socket_directories", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'l':
				SetConfigOption("ssl", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'N':
				SetConfigOption("max_connections", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'O':
				SetConfigOption("allow_system_table_mods", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'P':
				SetConfigOption("ignore_system_indexes", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'p':
				SetConfigOption("port", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'r':
				/* 仅单用户后端使用         */
				break;

			case 'S':
				SetConfigOption("work_mem", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 's':
				SetConfigOption("log_statement_stats", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 'T':

				/*
				 * 此选项曾定义为崩溃后发送 SIGSTOP，但发送 
				 * SIGABRT 看起来更有用。                        
				 */
				SetConfigOption("send_abort_for_crash", "true", PGC_POSTMASTER, PGC_S_ARGV);
				break;

			case 't':
				{
					const char *tmp = get_stats_option_name(optarg);

					if (tmp)
					{
						SetConfigOption(tmp, "true", PGC_POSTMASTER, PGC_S_ARGV);
					}
					else
					{
						write_stderr("%s: invalid argument for option -t: \"%s\"\n",
									 progname, optarg);
						ExitPostmaster(1);
					}
					break;
				}

			case 'W':
				SetConfigOption("post_auth_delay", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;

			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				ExitPostmaster(1);
		}
	}

	/*
	 * Postmaster 不接受任何非选项开关参数。
	 */
	if (optind < argc)
	{
		write_stderr("%s: invalid argument: \"%s\"\n",
					 progname, argv[optind]);
		write_stderr("Try \"%s --help\" for more information.\n",
					 progname);
		ExitPostmaster(1);
	}

	/*
	 * 定位正确的配置文件与数据目录，并首次读取      
	 * postgresql.conf。                 
	 */
	if (!SelectConfigFiles(userDoption, progname))
		ExitPostmaster(2);

	if (output_config_variable != NULL)
	{
		/*
		 * 若这是运行时计算的 GUC，此刻尚未初始化，        
		 * 现值无参考意义。不过，对多数 GUC 而言在这里打  
		 * 印值是安全的：即使服务器已在运行，把          
		 * postmaster 启动推进到这一步也无妨。少数无法   
		 * 给出有意义值的运行时计算 GUC，则留待             
		 * postmaster 启动后期再打印。那些 GUC 将无法  
		 * 在运行中的服务器上使用 -C，但现在就启用该选项
		 * 对它们会得出错误结果。                                 
		 *                                    
		 */
		int			flags = GetConfigOptionFlags(output_config_variable, true);

		if ((flags & GUC_RUNTIME_COMPUTED) == 0)
		{
			/*
			 * 指定了 "-C guc"，故打印 GUC 值并退出。无需  
			 * 额外的权限检查，因为用户读取的是数据目录
			 * 内的文件。     
			 */
			const char *config_val = GetConfigOption(output_config_variable,
													 false, false);

			puts(config_val ? config_val : "");
			ExitPostmaster(0);
		}

		/*
		 * 运行时计算的 GUC 稍后再打印。在我们初始化       
		 * 服务器启动序列时，屏蔽可能出现的日志消息，  
		 * 不让其进入生成输出。FATAL 及更严重消息值得   
		 * 展示出来，即便通常预期至少是 PANIC。而 LOG        
		 * 条目被隐藏。 
		 */
		SetConfigOption("log_min_messages", "FATAL", PGC_SUSET,
						PGC_S_OVERRIDE);
	}

	/* 检查 DataDir 是否合理          */
	checkDataDir();

	/* 检查 pg_control 存在     */
	checkControlFile();

	/* 然后将工作目录切换进去    */
	ChangeToDataDir();

	/*
	 * 检查 GUC 设置的无效组合。            
	 */
	if (SuperuserReservedConnections + ReservedConnections >= MaxConnections)
	{
		write_stderr("%s: superuser_reserved_connections (%d) plus reserved_connections (%d) must be less than max_connections (%d)\n",
					 progname,
					 SuperuserReservedConnections, ReservedConnections,
					 MaxConnections);
		ExitPostmaster(1);
	}
	if (XLogArchiveMode > ARCHIVE_MODE_OFF && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL archival cannot be enabled when wal_level is \"minimal\"")));
	if (max_wal_senders > 0 && wal_level == WAL_LEVEL_MINIMAL)
		ereport(ERROR,
				(errmsg("WAL streaming (max_wal_senders > 0) requires wal_level \"replica\" or \"logical\"")));

	/*
	 * 其他一次性内部完整性检查若够快，可放在这里。  
	 * （慢的处理放到更后面，在创建 postmaster.pid 之后。）
	 */
	if (!CheckDateTokenTables())
	{
		write_stderr("%s: invalid datetoken tables, please fix\n", progname);
		ExitPostmaster(1);
	}

	/*
	 * 既然已处理完 postmaster 参数，现在重置            
	 * getopt(3) 库，使其能在子进程中正确工作。          
	 */
	optind = 1;
#ifdef HAVE_INT_OPTRESET
	optreset = 1;				/* 某些系统亦需此步   */
#endif

	/* 调试用：显示 postmaster 环境          */
	{
#if !defined(WIN32) || defined(_MSC_VER)
		extern char **environ;
#endif
		char	  **p;

		ereport(DEBUG3,
				(errmsg_internal("%s: PostmasterMain: initial environment dump:",
								 progname)));
		ereport(DEBUG3,
				(errmsg_internal("-----------------------------------------")));
		for (p = environ; *p; ++p)
			ereport(DEBUG3,
					(errmsg_internal("\t%s", *p)));
		ereport(DEBUG3,
				(errmsg_internal("-----------------------------------------")));
	}

	/*
	 * 为数据目录创建锁文件。  
	 *
	 * 这一步最好抢在输入套接字之前完成，因为              
	 * 数据目录互锁比套接字文件互锁                    
	 * 更可靠（感谢那位把套接字文件放进 /tmp 的人 :-()     
	 * 同理，最好先抢占 TCP 套接字再抢                       
	 * Unix 套接字 
	 *
	 * 另注意，此步会内部设置 on_proc_exit 函数，该函数    
	 * 负责同时移除数据目录和套接字锁文件；               
	 * 因此必须在打开套接字前完成，这样退出时套接字  
	 * 锁文件会在 CloseServerPorts 后移除。 
	 */
	CreateDataDirLockFile(true);

	/*
	 * 读取控制文件（用于错误检查与配置）。     
	 *
	 * 由于要校验控制文件的 CRC，这会带来一个有用的     
	 * 副作用：在需要运行时测试 CRC 支持指令的机器上。   
	 * postmaster 在启动时做一次该测试，之后其子进程     
	 * 会继承正确的函数指针，无需再                         
	 * 重复测试。 
	 */
	LocalProcessControlFile(false);

	/*
	 * 注册 apply launcher。最好尽早调用这个函数，           
	 * 要赶在任何模块占用后台工作进程槽位之前。        
	 */
	ApplyLauncherRegister();

	/*
	 * 处理那些应在 postmaster 启动时预加载的库            
	 */
	process_shared_preload_libraries();

	/*
	 * 若已指定，初始化 SSL 库。  
	 */
#ifdef USE_SSL
	if (EnableSSL)
	{
		(void) secure_initialize(true);
		LoadedSSL = true;
	}
#endif

	/*
	 * 既然可加载模块已有机会修改任何 GUC，              
	 * 计算 MaxBackends。 
	 */
	InitializeMaxBackends();

	/*
	 * 给预加载的库一个请求额外共享内存的机会。          
	 */
	process_shmem_requests();

	/*
	 * 既然可加载模块已有机会请求额外共享内存，         
	 * 现在确定任意运行时计算的 GUC，其值                   
	 * 取决于所需共享内存的数量。        
	 */
	InitializeShmemGUCs();

	/*
	 * 既然模块已加载，现在就能处理 wal_consistency_checking  
	 * GUC 中指定的自定义资源管理器。            
	 */
	InitializeWalConsistencyChecking();

	/*
	 * 如果 -C 指定的是运行时计算的 GUC，我们此前会       
	 * 暂缓打印其值，因为该 GUC 尚未初始化。我们会      
	 * 在锁定数据目录之前处理大多数 GUC 的 -C，这样选项  
	 * 能在运行中的服务器上使用。然而少数 GUC 属运行时 
	 * 计算型，锁定数据目录之前没有有意义的值，而且     
	 * 运行中的服务器上我们无法提前安全算出其值。    
	 * 此时，这类 GUC 应该已经妥善                      
	 * 初始化完毕，共享内存尚未建立，故现在是处理        
	 * 这些特殊 GUC 的 -C 选项的好时机。       
	 */
	if (output_config_variable != NULL)
	{
		const char *config_val = GetConfigOption(output_config_variable,
												 false, false);

		puts(config_val ? config_val : "");
		ExitPostmaster(0);
	}

	/*
	 * 建立共享内存和信号量。   
	 *
	 * 注意：若使用 SysV 共享内存和/或信号量，则每次    
	 * postmaster 启动通常选中同一 IPC 键，这有助于        
	 * 确保 postmaster 崩溃后重启即清理失效的 IPC 对象。    
	 */
	CreateSharedMemoryAndSemaphores();

	/*
	 * 估算可打开文件的数量。此步必须放在                  
	 * 信号量建立后，因某些平台信号量算作打开文件。   
	 */
	set_max_safe_fds();

	/*
	 * 为栈深度检查设置参照点。         
	 */
	(void) set_stack_base();

	/*
	 * 初始化管道（Windows 上为进程句柄），让子进程能在  
	 * postmaster 死亡时从睡眠唤醒。 
	 */
	InitPostmasterDeathWatchHandle();

#ifdef WIN32

	/*
	 * 初始化 I/O 完成端口，用于传递死亡子进程列表。    
	 */
	win32ChildQueue = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
	if (win32ChildQueue == NULL)
		ereport(FATAL,
				(errmsg("could not create I/O completion port for child queue")));
#endif

#ifdef EXEC_BACKEND
	/* 写出非默认 GUC 设置供子进程使用                 */
	write_nondefault_variables(PGC_POSTMASTER);

	/*
	 * 清理向子进程传递参数所用的临时目录（见        
	 * internal_forkexec）。必须在启动任何子进程之前      
	 * 进行，否则会有竞态条件：我们可能在子进程还       
	 * 没来得及读取参数文件之前就把它删掉。现在       
	 * 这样做是安全的，因为我们此前已验证             
	 * 此数据目录中没有冲突的 Postgres 进程。  
	 */
	RemovePgTempFilesInDir(PG_TEMP_FILES_DIR, true, false);
#endif

	/*
	 * 强制移除那些指示备库提升请求的文件。          
	 * 否则这些文件的存在会过早触发一次提升，              
	 * 无论用户是否愿意。      
	 *
	 * 这种文件移除通常无必要，因为它们只在备库提升 
	 * 期间的几个时刻存在。然而有一个竞态条件：          
	 * 如果 pg_ctl promote 在提升期间执行并创建文件，        
	 * 这些文件可能在服务器晋升为主库之后仍然             
	 * 存在。之后，若一个新备库使用从新主库取得的      
	 * 备份启动，这些文件可能在服务器启动时存在，     
	 * 必须将其移除，以免发生意外的提升。                   
	 *
	 * 还有一点：提升信号文件必须在调用启动进程          
	 * 之前移除。因为此后它们可能被                  
	 * postmaster 的 SIGUSR1 所用。    
	 */
	RemovePromoteSignalFiles();

	/* 对 logrotate 信号文件同理      */
	RemoveLogrotateSignalFiles();

	/* 移除保存现行日志文件名的过时文件。         */
	if (unlink(LOG_METAINFO_DATAFILE) < 0 && errno != ENOENT)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						LOG_METAINFO_DATAFILE)));

	/*
	 * 初始化输入套接字 
	 *
	 * 把它们都标为已关闭，并设置 on_proc_exit            
	 * 函数，负责在 postmaster 关闭时关掉套接字。     
	 */
	for (i = 0; i < MAXLISTEN; i++)
		ListenSocket[i] = PGINVALID_SOCKET;

	on_proc_exit(CloseServerPorts, 0);

	/*
	 * 如果启用，启动 syslogger 收集子进程     
	 */
	SysLoggerPID = SysLogger_Start();

	/*
	 * 把 whereToSendOutput 从初始状态 DestDebug 重置        
	 * 为 DestNone。这样 ereport 就不会往 stderr 发日志，         
	 * 除非 Log_destination 允许。要等 postmaster 完全          
	 * 启动后才做，因为启动失败也该报告给              
	 * stderr 
	 *
	 * 如果确实要禁用 stderr 日志，先发一条日志消息，      
	 * 说明此事，为可能不记得日志被配置到别处的用户     
	 * 留下一条追踪线索。                           
	 */
	if (!(Log_destination & LOG_DESTINATION_STDERR))
		ereport(LOG,
				(errmsg("ending log output to stderr"),
				 errhint("Future log output will go to log destination \"%s\".",
						 Log_destination_string)));

	whereToSendOutput = DestNone;

	/*
	 * 在日志中报告服务器启动。虽可更早发出，            
	 * 但若要用日志收集器，宜在启动之后再做。            
	 *            
	 */
	ereport(LOG,
			(errmsg("starting %s", PG_VERSION_STR)));

	/*
	 * 建立输入套接字。
	 */
	if (ListenAddresses)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* 需要 ListenAddresses 的可修改副本 */
		rawstring = pstrdup(ListenAddresses);

		/* 把字符串解析成主机名列表*/
		if (!SplitGUCList(rawstring, ',', &elemlist))
		{
			/* 列表有语法错误*/
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"listen_addresses")));
		}

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (strcmp(curhost, "*") == 0)
				status = StreamServerPort(AF_UNSPEC, NULL,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSocket, MAXLISTEN);
			else
				status = StreamServerPort(AF_UNSPEC, curhost,
										  (unsigned short) PostPortNumber,
										  NULL,
										  ListenSocket, MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* 把首个成功的主机地址记入锁文件     */
				if (!listen_addr_saved)
				{
					AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, curhost);
					listen_addr_saved = true;
				}
			}
			else
				ereport(WARNING,
						(errmsg("could not create listen socket for \"%s\"",
								curhost)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any TCP/IP sockets")));

		list_free(elemlist);
		pfree(rawstring);
	}

#ifdef USE_BONJOUR
	/* 仅当打开了 TCP 套接字才注册 Bonjour       */
	if (enable_bonjour && ListenSocket[0] != PGINVALID_SOCKET)
	{
		DNSServiceErrorType err;

		/*
		 * 我们为 interface_index 传 0，注册到所有"适用"         
		 * 的接口上。DNS-SD 文档并未完全讲清楚，          
		 * 如果只绑定到可用网络接口的一个子集，           
		 * 这种做法是否妥当。                       
		 */
		err = DNSServiceRegister(&bonjour_sdref,
								 0,
								 0,
								 bonjour_name,
								 "_postgresql._tcp.",
								 NULL,
								 NULL,
								 pg_hton16(PostPortNumber),
								 0,
								 NULL,
								 NULL,
								 NULL);
		if (err != kDNSServiceErr_NoError)
			ereport(LOG,
					(errmsg("DNSServiceRegister() failed: error code %ld",
							(long) err)));

		/*
		 * 我们不去读 mDNS 守护进程的应答，而是预期          
		 * 套接字在 postmaster 终止时关闭，它就会自动          
		 * 注销我们的注册。所以这里无事可做。但          
		 * bonjour_sdref 仍要保留，以便 fork 出的子进程     
		 * 能关闭它们自己那份套接字副本。        
		 */
	}
#endif

	if (Unix_socket_directories)
	{
		char	   *rawstring;
		List	   *elemlist;
		ListCell   *l;
		int			success = 0;

		/* 需要 Unix_socket_directories 的可修改副本 */
		rawstring = pstrdup(Unix_socket_directories);

		/* 把字符串解析成目录列表     */
		if (!SplitDirectoriesString(rawstring, ',', &elemlist))
		{
			/* 列表有语法错误*/
			ereport(FATAL,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid list syntax in parameter \"%s\"",
							"unix_socket_directories")));
		}

		foreach(l, elemlist)
		{
			char	   *socketdir = (char *) lfirst(l);

			status = StreamServerPort(AF_UNIX, NULL,
									  (unsigned short) PostPortNumber,
									  socketdir,
									  ListenSocket, MAXLISTEN);

			if (status == STATUS_OK)
			{
				success++;
				/* 把首个成功的 Unix 套接字记入锁文件    */
				if (success == 1)
					AddToDataDirLockFile(LOCK_FILE_LINE_SOCKET_DIR, socketdir);
			}
			else
				ereport(WARNING,
						(errmsg("could not create Unix-domain socket in directory \"%s\"",
								socketdir)));
		}

		if (!success && elemlist != NIL)
			ereport(FATAL,
					(errmsg("could not create any Unix-domain sockets")));

		list_free_deep(elemlist);
		pfree(rawstring);
	}

	/*
	 * 检查确实有套接字可监听          
	 */
	if (ListenSocket[0] == PGINVALID_SOCKET)
		ereport(FATAL,
				(errmsg("no socket created for listening")));

	/*
	 * 若无有效 TCP 端口，就为监听地址写一个空行，
	 * 表示必须使用 Unix 套接字。注意这一行要等到        
	 * 有套接字支撑它时才会写入锁文件。          
	 */
	if (!listen_addr_saved)
		AddToDataDirLockFile(LOCK_FILE_LINE_LISTEN_ADDR, "");

	/*
	 * 记录 postmaster 选项。推迟到现在才记，以免记下      
	 * 无效选项（如端口不可用）。  
	 */
	if (!CreateOptsFile(argc, argv, my_exec_path))
		ExitPostmaster(1);

	/*
	 * 如果被要求，写外部 PID 文件  
	 */
	if (external_pid_file)
	{
		FILE	   *fpidfile = fopen(external_pid_file, "w");

		if (fpidfile)
		{
			fprintf(fpidfile, "%d\n", MyProcPid);
			fclose(fpidfile);

			/* 让 PID 文件人人可读   */
			if (chmod(external_pid_file, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) != 0)
				write_stderr("%s: could not change permissions of external PID file \"%s\": %s\n",
							 progname, external_pid_file, strerror(errno));
		}
		else
			write_stderr("%s: could not write external PID file \"%s\": %s\n",
						 progname, external_pid_file, strerror(errno));

		on_proc_exit(unlink_external_pid_file, 0);
	}

	/*
	 * 移除旧临时文件。此时不可能再有其他             
	 * Postgres 进程在此目录中运行，故应是安全的。         
	 */
	RemovePgTempFiles();

	/*
	 * 初始化 autovacuum 子系统（仍未启动进程）           
	 */
	autovac_init();

	/*
	 * 加载客户端认证的配置文件。            
	 */
	if (!load_hba())
	{
		/*
		 * 若加载 HBA 文件失败，继续毫无意义，            
		 * 因为这种情况下无法连接到数据库。              
		 */
		ereport(FATAL,
		/* translator: %s is a configuration file */
				(errmsg("could not load %s", HbaFileName)));
	}
	if (!load_ident())
	{
		/*
		 * 没有 IDENT 文件也能启动，尽管这意味着你           
		 * 无法使用需要用户名映射的认证方法登录。        
		 * load_ident() 已把错误详情记录到日志中。               
		 *            
		 */
	}

#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * 在 macOS 上，libintl 会把 setlocale() 替换为            
	 * 一个版本，它在第二参数为 "" 且相关环境变量都       
	 * 未设置或为空时调用 CFLocaleCopyCurrent()。它使          
	 * 进程变为多线程。postmaster 会调用 sigprocmask()，     
	 * 并调用 fork() 而不立即 exec()，两者在多线程程序中    
	 * 都是未定义行为。多线程 postmaster 在 Windows               
	 * 是常态，而 Windows 既无 fork() 也无 sigprocmask()。         
	 * 目前，macOS 是唯一拥有 pthread_is_threaded_np() 的平台，  
	 * 所以无需担心这条 HINT 在别处是否合适。          
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded during startup"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/*
	 * 记住 postmaster 启动时间  
	 */
	PgStartTime = GetCurrentTimestamp();

	/*
	 * 在 postmaster.pid 文件中报告 postmaster 状态，让 pg_ctl 能  
	 * 了解状况。      
	 */
	AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STARTING);

	/* 启动 bgwriter 和 checkpointer，让它们协助恢复       */
	if (CheckpointerPID == 0)
		CheckpointerPID = StartCheckpointer();
	if (BgWriterPID == 0)
		BgWriterPID = StartBackgroundWriter();

	/*
	 * 万事俱备……             
	 */
	StartupPID = StartupDataBase();
	Assert(StartupPID != 0);
	StartupStatus = STARTUP_RUNNING;
	pmState = PM_STARTUP;

	/* 有些工作进程或已排定现在启动 */
	maybe_start_bgworkers();

	status = ServerLoop();

	/*
	 * ServerLoop 理应永不返回，但若真返回，就关闭。        
	 */
	ExitPostmaster(status != STATUS_OK);

	abort();					/* 不会到达*/
}


/*
 * 关闭监听套接字的 on_proc_exit 回调          
 */
static void
CloseServerPorts(int status, Datum arg)
{
	int			i;

	/*
	 * 首先显式关闭所有套接字 FD。过去我们任由            
	 * 退出时隐式发生，但最好赶在移除                      
	 * postmaster.pid 锁文件之前关掉它们；否则，若新          
	 * postmaster 想重用 TCP 端口号，就会有竞态条件。     
	 */
	for (i = 0; i < MAXLISTEN; i++)
	{
		if (ListenSocket[i] != PGINVALID_SOCKET)
		{
			StreamClose(ListenSocket[i]);
			ListenSocket[i] = PGINVALID_SOCKET;
		}
	}

	/*
	 * 接下来，移除 Unix 套接字的文件系统条目。为避免  
	 * 与后到的 postmaster 产生竞态，这一步必须在关闭        
	 * 套接字后、移除锁文件前。       
	 */
	RemoveSocketFiles();

	/*
	 * 这里我们不动套接字锁文件；那些将由             
	 * on_proc_exit 回调移除。             
	 */
}

/*
 * 删除 external_pid_file 的 on_proc_exit 回调 
 */
static void
unlink_external_pid_file(int status, Datum arg)
{
	if (external_pid_file)
		unlink(external_pid_file);
}


/*
 * 计算并检查属于安装的各文件的目录路径             
 * （依据 postgres 可执行文件自身的位置推断）           
 */
static void
getInstallationPaths(const char *argv0)
{
	DIR		   *pdir;

	/* 定位 postgres 可执行文件       */
	if (find_my_exec(argv0, my_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate my own executable path", argv0)));

#ifdef EXEC_BACKEND
	/* 在改变工作目录之前定位可执行后端             */
	if (find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
						postgres_exec_path) < 0)
		ereport(FATAL,
				(errmsg("%s: could not locate matching postgres executable",
						argv0)));
#endif

	/*
	 * 定位 pkglib 目录——必须尽早设置，以防我们随后因   
	 * postgresql.conf 条目而尝试从其中加载模块。            
	 */
	get_pkglib_path(my_exec_path, pkglib_path);

	/*
	 * 验证那里有个可读的目录；否则说明 Postgres 安装      
	 * 不完整或已损坏。（此失败的典型原因是          
	 * postgres 可执行文件被移动或硬链接到了某个              
	 * 与安装的 lib/ 目录不同级的目录。）            
	 *            
	 */
	pdir = AllocateDir(pkglib_path);
	if (pdir == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						pkglib_path),
				 errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has been moved away from its proper location.",
						 my_exec_path)));
	FreeDir(pdir);

	/*
	 * XXX 是否也值得检查 share/ 目录？如果 lib/                
	 * 目录在，那 share/ 大概率也在。       
	 */
}

/*
 * 检查 pg_control 是否存在于数据目录中正确位置。            
 *
 * 这里不会尝试校验 pg_control 的内容。这只是               
 * 一次完整性检查，确认看到的是真实数据目录。       
 */
static void
checkControlFile(void)
{
	char		path[MAXPGPATH];
	FILE	   *fp;

	snprintf(path, sizeof(path), "%s/global/pg_control", DataDir);

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		write_stderr("%s: could not find the database system\n"
					 "Expected to find it in the directory \"%s\",\n"
					 "but could not open file \"%s\": %s\n",
					 progname, DataDir, path, strerror(errno));
		ExitPostmaster(2);
	}
	FreeFile(fp);
}

/*
 * 决定该让 ServerLoop 睡眠多久，以毫秒计。             
 *
 * 正常情况下我们至多等待一分钟，以确保其他             
 * 由 ServerLoop 处理的后台任务即使没有请求到达             
 * 也能完成。不过，如果有后台工作进程正等待启动，     
 * 我们实际上并不睡眠，以便它们得到快速处理。其余      
 * 例外见代码所示。       
 */
static int
DetermineSleepTime(void)
{
	TimestampTz next_wakeup = 0;

	/*
	 * 正常情况：要么根本没有后台工作进程，要么我们     
	 * 正在关闭序列中（完全无视 bgworker）。               
	 */
	if (Shutdown > NoShutdown ||
		(!StartWorkerNeeded && !HaveCrashedWorker))
	{
		if (AbortStartTime != 0)
		{
			int			seconds;

			/* 距中止的剩余时间；若已过期则钳到 0        */
			seconds = SIGKILL_CHILDREN_AFTER_SECS -
				(time(NULL) - AbortStartTime);

			return Max(seconds * 1000, 0);
		}
		else
			return 60 * 1000;
	}

	if (StartWorkerNeeded)
		return 0;

	if (HaveCrashedWorker)
	{
		slist_mutable_iter siter;

		/*
		 * 当有已崩溃的 bgworker 时，我们睡眠时长只要足以
		 * 使它们在自身请求的时刻被重启。扫描列表    
		 * 依据最近崩溃时间和重启间隔求出                     
		 * 各唤醒时刻的最小值。            
		 */
		slist_foreach_modify(siter, &BackgroundWorkerList)
		{
			RegisteredBgWorker *rw;
			TimestampTz this_wakeup;

			rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);

			if (rw->rw_crashed_at == 0)
				continue;

			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART
				|| rw->rw_terminate)
			{
				ForgetBackgroundWorker(&siter);
				continue;
			}

			this_wakeup = TimestampTzPlusMilliseconds(rw->rw_crashed_at,
													  1000L * rw->rw_worker.bgw_restart_time);
			if (next_wakeup == 0 || this_wakeup < next_wakeup)
				next_wakeup = this_wakeup;
		}
	}

	if (next_wakeup != 0)
	{
		int			ms;

		/* TimestampDifferenceMilliseconds 的结果在 [0, INT_MAX] 内*/
		ms = (int) TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												   next_wakeup);
		return Min(60 * 1000, ms);
	}

	return 60 * 1000;
}

/*
 * 激活或停用服务器套接字事件的通知。由于我们        
 * 目前没有办法从既有 WaitEventSet 中移除事件，               
 * 只能销毁再重建整个集合。本函数在关闭期间           
 * 被调用，让我们不收新连接等待后端退出，         
 * 也在崩溃后重新初始化期间调用，那时需要重新     
 * 开始监听新连接。WaitEventSet 在 fork 出的子进程中            
 * 由 ClosePostmasterPorts() 释放  
 */
static void
ConfigurePostmasterWaitSet(bool accept_connections)
{
	int			nsockets;

	if (pm_wait_set)
		FreeWaitEventSet(pm_wait_set);
	pm_wait_set = NULL;

	/* 要等几个服务器套接字？               */
	nsockets = 0;
	if (accept_connections)
	{
		while (nsockets < MAXLISTEN &&
			   ListenSocket[nsockets] != PGINVALID_SOCKET)
			++nsockets;
	}

	pm_wait_set = CreateWaitEventSet(CurrentMemoryContext, 1 + nsockets);
	AddWaitEventToSet(pm_wait_set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch,
					  NULL);

	if (accept_connections)
	{
		for (int i = 0; i < nsockets; i++)
			AddWaitEventToSet(pm_wait_set, WL_SOCKET_ACCEPT, ListenSocket[i],
							  NULL, NULL);
	}
}

/*
 * postmaster 空闲主循环  
 */
static int
ServerLoop(void)
{
	time_t		last_lockfile_recheck_time,
				last_touch_time;
	WaitEvent	events[MAXLISTEN];
	int			nevents;

	ConfigurePostmasterWaitSet(true);
	last_lockfile_recheck_time = last_touch_time = time(NULL);

	for (;;)
	{
		time_t		now;

		nevents = WaitEventSetWait(pm_wait_set,
								   DetermineSleepTime(),
								   events,
								   lengthof(events),
								   0 /* postmaster 不发布 wait_event */ );

		/*
		 * 信号处理器置了 latch，还是某个套接字有待       
		 * 新连接等待处理？若是后者，fork 子进程处理。   
		 */
		for (int i = 0; i < nevents; i++)
		{
			if (events[i].events & WL_LATCH_SET)
				ResetLatch(MyLatch);

			/*
			 * 下面的请求会被无条件处理，即便没有看到     
			 * WL_LATCH_SET。这给关闭与重载请求高优先级：     
			 * 这些请求的 latch 恰巧在 events[] 较晚出现，      
			 * 或稍后 WaitEventSetWait() 报告时。      
			 *                    
			 */
			if (pending_pm_shutdown_request)
				process_pm_shutdown_request();
			if (pending_pm_reload_request)
				process_pm_reload_request();
			if (pending_pm_child_exit)
				process_pm_child_exit();
			if (pending_pm_pmsignal)
				process_pm_pmsignal();

			if (events[i].events & WL_SOCKET_ACCEPT)
			{
				Port	   *port;

				port = ConnCreate(events[i].fd);
				if (port)
				{
					BackendStartup(port);

					/*
					 * 本进程不再需要该打开的套接字或         
					 * 端口结构
					 */
					StreamClose(port->sock);
					ConnFree(port);
				}
			}
		}

		/* 若丢失日志收集进程，则尝试启动新的       */
		if (SysLoggerPID == 0 && Logging_collector)
			SysLoggerPID = SysLogger_Start();

		/*
		 * 若 bgwriter 未在运行，且当前状态并不禁止，     
		 * 就启动一个。即使失败也无妨，稍后再试。    
		 * 对 checkpointer 同样如此。                                  
		 */
		if (pmState == PM_RUN || pmState == PM_RECOVERY ||
			pmState == PM_HOT_STANDBY || pmState == PM_STARTUP)
		{
			if (CheckpointerPID == 0)
				CheckpointerPID = StartCheckpointer();
			if (BgWriterPID == 0)
				BgWriterPID = StartBackgroundWriter();
		}

		/*
		 * 同样，若丢失了 walwriter 进程，则尝试启动新的。  
		 * 但这仅在正常运行时才需要（否则我们不可能     
		 * 在写入新 WAL）     
		 */
		if (WalWriterPID == 0 && pmState == PM_RUN)
			WalWriterPID = StartWalWriter();

		/*
		 * 若丢失了 autovacuum launcher，则尝试启动新的。        
		 * 二进制升级时不希望 autovacuum 运行，因为     
		 * autovacuum 可能会在物理文件就位之前更新           
		 * 空表的 relfrozenxid。       
		 */
		if (!IsBinaryUpgrade && AutoVacPID == 0 &&
			(AutoVacuumingActive() || start_autovac_launcher) &&
			pmState == PM_RUN)
		{
			AutoVacPID = StartAutoVacLauncher();
			if (AutoVacPID != 0)
				start_autovac_launcher = false; /* 信号已处理  */
		}

		/* 若丢失了 archiver，则尝试启动新的。      */
		if (PgArchPID == 0 && PgArchStartupAllowed())
			PgArchPID = StartArchiver();

		/* 若需向 autovacuum launcher 发信号，现在就发  */
		if (avlauncher_needs_signal)
		{
			avlauncher_needs_signal = false;
			if (AutoVacPID != 0)
				kill(AutoVacPID, SIGUSR2);
		}

		/* 若需启动 WAL receiver，现在就试着启动      */
		if (WalReceiverRequested)
			MaybeStartWalReceiver();

		/* 需要时让其余 worker 进程跑起来     */
		if (StartWorkerNeeded || HaveCrashedWorker)
			maybe_start_bgworkers();

#ifdef HAVE_PTHREAD_IS_THREADED_NP

		/*
		 * 在启用断言的构建中，定期检查是否出现了 
		 * 额外线程。各构建都在启动/退出时检查。 
		 */
		Assert(pthread_is_threaded_np() == 0);
#endif

		/*
		 * 最后，检查是否到了做某些事情的时间；这些事  
		 * 情不想每轮循环都做，因为开销有点大。注意： 
		 * 这些任务的执行时间最多偏差约一分钟，因为    
		 * DetermineSleepTime() 允许我们最多睡这么久；唯有      
		 * SIGKILL 超时在那里有专门的特例处理。              
		 *                          
		 */
		now = time(NULL);

		/*
		 * 若我们已向子进程发出 SIGQUIT 而它们迟迟不退， 
		 * 就该向它们发送 SIGKILL（若要求则用 SIGABRT）。  
		 * 正常情况下不会发生这种情况，但某些条件下后端 
		 * 关闭时可能卡住。这是让它们脱困的最后一招。  
		 *               
		 *
		 * 注意，进程崩溃恢复期间我们也会这样做。 
		 */
		if ((Shutdown >= ImmediateShutdown || FatalError) &&
			AbortStartTime != 0 &&
			(now - AbortStartTime) >= SIGKILL_CHILDREN_AFTER_SECS)
		{
			/* 之前对它们很客气，现在不了      */
			ereport(LOG,
			/* translator: %s is SIGKILL or SIGABRT */
					(errmsg("issuing %s to recalcitrant children",
							send_abort_for_kill ? "SIGABRT" : "SIGKILL")));
			TerminateChildren(send_abort_for_kill ? SIGABRT : SIGKILL);
			/* 重置标志，以免再次 SIGKILL  */
			AbortStartTime = 0;
		}

		/*
		 * 每分钟检查一次 postmaster.pid 是否被删除或覆盖。
		 * 若确实如此，就强制关闭。这样可以避免数据库  
		 * 消失后仍有 postmaster 和子进程逗留，也避免在       
		 * 同一位置新建集簇时出问题。它还提供一定       
		 * 的保护：防止 DBA 轻率删除 postmaster.pid 并      
		 * 手动启动新 postmaster。这样数据多半会损坏， 
		 * 但我们仍能尽快中止，把损害降到最低。             
		 *                                                              
		 */
		if (now - last_lockfile_recheck_time >= 1 * SECS_PER_MINUTE)
		{
			if (!RecheckDataDirLockFile())
			{
				ereport(LOG,
						(errmsg("performing immediate shutdown because data directory lock file is invalid")));
				kill(MyProcPid, SIGQUIT);
			}
			last_lockfile_recheck_time = now;
		}

		/*
		 * 每 58 分钟触碰一次 Unix 套接字与锁文件，以防    
		 * 它们被过分热心的 /tmp 清理任务删掉。我们假定    
		 * 没人运行截止时间不足一小时的清理程序……   
		 */
		if (now - last_touch_time >= 58 * SECS_PER_MINUTE)
		{
			TouchSocketFiles();
			TouchSocketLockFiles();
			last_touch_time = now;
		}
	}
}

/*
 * 读取客户端的启动包，并据此做相应的处理。    
 *
 * 返回 STATUS_OK 或 STATUS_ERROR，也可能调用 ereport(FATAL) 
 * 而不返回。   
 *
 * 注意：ereport(FATAL) 的消息会发给客户端，所以只有   
 * 确实想这么做才用它。若不想向客户端发送任何   
 * 东西，就返回 STATUS_ERROR；这通常适用于检测到    
 * 通信故障的情况。               
 *
 * 当加密层协商完成时，设置 ssl_done 和/或 gss_done（  
 * 当前为 TLS 或 GSSAPI）。任一加密层成功协商都会设置      
 * 两个标志，而被拒绝的协商只设置对应层的标志，因为  
 * 客户端可能还想尝试另一层。我们不应就客户端        
 * 发出请求的顺序做任何假设。                                    
 *          
 */
static int
ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done)
{
	int32		len;
	char	   *buf;
	ProtocolVersion proto;
	MemoryContext oldcontext;

	pq_startmsgread();

	/*
	 * 单独抓取长度字的第一个字节，这样我们就能区分    
	 * 到底是完全没有数据还是包不完整。这听起来可能  
	 * 效率不高，但由于 pqcomm.c 中的缓冲，实际并非  
	 * 如此。 
	 */
	if (pq_getbytes((char *) &len, 1) == EOF)
	{
		/*
		 * 若完全收不到数据，别往日志塞抱怨信息；这种  
		 * 情况往往有正当理由。例如：我们可能在响应      
		 * NEGOTIATE_SSL_CODE 之后到达这里，而如果客户端不满意
		 * 我们的响应，多半会直接断开。服务监控软件 
		 * 也常常只是打开再关闭连接，不发任何内容。（   
		 * 端口扫描器也是如此。它们未必那么友善， 
		 * 不过留意这些并非我们的分内之事。）               
		 *               
		 */
		return STATUS_ERROR;
	}

	if (pq_getbytes(((char *) &len) + 1, 3) == EOF)
	{
		/* 长度字不完整，就此抱怨一下        */
		if (!ssl_done && !gss_done)
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}

	len = pg_ntoh32(len);
	len -= 4;

	if (len < (int32) sizeof(ProtocolVersion) ||
		len > MAX_STARTUP_PACKET_LENGTH)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("invalid length of startup packet")));
		return STATUS_ERROR;
	}

	/*
	 * 分配空间来存放启动包，外加一个初始化为零的额   
	 * 外字节。这保证了包内所有字符串都以空字符结尾。 
	 *                               
	 */
	buf = palloc(len + 1);
	buf[len] = '\0';

	if (pq_getbytes(buf, len) == EOF)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("incomplete startup packet")));
		return STATUS_ERROR;
	}
	pq_endmsgread();

	/*
	 * 第一个字段是协议版本号或特殊请求码。          
	 *              
	 */
	port->proto = proto = pg_ntoh32(*((ProtocolVersion *) buf));

	if (proto == CANCEL_REQUEST_CODE)
	{
		if (len != sizeof(CancelRequestPacket))
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid length of startup packet")));
			return STATUS_ERROR;
		}
		processCancelRequest(port, buf);
		/* 这并非真正的错误，只是不想继续处理下去 */
		return STATUS_ERROR;
	}

	if (proto == NEGOTIATE_SSL_CODE && !ssl_done)
	{
		char		SSLok;

#ifdef USE_SSL
		/* 禁用 SSL 或 Unix 套接字时无 SSL */
		if (!LoadedSSL || port->laddr.addr.ss_family == AF_UNIX)
			SSLok = 'N';
		else
			SSLok = 'S';		/* 支持 SSL      */
#else
		SSLok = 'N';			/* 不支持 SSL      */
#endif

retry1:
		if (send(port->sock, &SSLok, 1, 0) != 1)
		{
			if (errno == EINTR)
				goto retry1;	/* 被中断就重试         */
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send SSL negotiation response: %m")));
			return STATUS_ERROR;	/* 关闭连接         */
		}

#ifdef USE_SSL
		if (SSLok == 'S' && secure_open_server(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * 此时我们本不应有任何已缓冲数据。如果存在，  
		 * 说明它是在 SSL 握手之前收到的，因此未经加密，  
		 * 确实可能是由中间人注入的。我们要把这种情况    
		 * 报告给客户端。             
		 */
		if (pq_buffer_has_data())
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after SSL request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * 下面应跟着常规启动包或取消等包，而非另一次  
		 * SSL 协商请求；仅当 SSL 被拒绝才允许 GSS 请求   
		 * （客户端按任一顺序协商都可以。）                 
		 */
		return ProcessStartupPacket(port, true, SSLok == 'S');
	}
	else if (proto == NEGOTIATE_GSS_CODE && !gss_done)
	{
		char		GSSok = 'N';

#ifdef ENABLE_GSS
		/* Unix 套接字上不用 GSSAPI 加密    */
		if (port->laddr.addr.ss_family != AF_UNIX)
			GSSok = 'G';
#endif

		while (send(port->sock, &GSSok, 1, 0) != 1)
		{
			if (errno == EINTR)
				continue;
			ereport(COMMERROR,
					(errcode_for_socket_access(),
					 errmsg("failed to send GSSAPI negotiation response: %m")));
			return STATUS_ERROR;	/* 关闭连接         */
		}

#ifdef ENABLE_GSS
		if (GSSok == 'G' && secure_open_gssapi(port) == -1)
			return STATUS_ERROR;
#endif

		/*
		 * 此时我们本不应有任何已缓冲数据。如果存在，  
		 * 说明它是在 GSS 握手之前收到的，因此未经加密，  
		 * 确实可能是由中间人注入的。我们要把这种情况    
		 * 报告给客户端。             
		 */
		if (pq_buffer_has_data())
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("received unencrypted data after GSSAPI encryption request"),
					 errdetail("This could be either a client-software bug or evidence of an attempted man-in-the-middle attack.")));

		/*
		 * 下面应跟着常规启动包或取消等包，而非另一次  
		 * GSS 协商请求；仅当 GSS 被拒绝才允许 SSL 请求    
		 * （客户端按任一顺序协商都可以。）                 
		 */
		return ProcessStartupPacket(port, GSSok == 'G', true);
	}

	/* 可以在这里添加其他特殊包类型     */

	/*
	 * 现在设置 FrontendProtocol；万一启动期间失败，ereport()   
	 * 即知报文格式。  
	 */
	FrontendProtocol = proto;

	/* 检查主协议版本号是否处于有效范围。*/
	if (PG_PROTOCOL_MAJOR(proto) < PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST) ||
		PG_PROTOCOL_MAJOR(proto) > PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST))
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported frontend protocol %u.%u: server supports %u.0 to %u.%u",
						PG_PROTOCOL_MAJOR(proto), PG_PROTOCOL_MINOR(proto),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_EARLIEST),
						PG_PROTOCOL_MAJOR(PG_PROTOCOL_LATEST),
						PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST))));

	/*
	 * 现在从启动包中取出各参数，保存到 Port 结构里。    
	 * 所有挂在 Port 结构上的数据结构都必须在             
	 * TopMemoryContext 中分配，这样运行中的后端才能          
	 * 继续使用它们（即使 PostmasterContext 已被销毁）。     
	 * 我们无需担心失败时泄漏这些内存，因为此时我们     
	 * 早已脱离 postmaster。 
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	/* 处理协议版本 3 的启动包        */
	{
		int32		offset = sizeof(ProtocolVersion);
		List	   *unrecognized_protocol_options = NIL;

		/*
		 * 扫描包体，找出其中的名称/选项对。凡是在包体 
		 * 内开始的字符串都视为已由空字符终止，这多  
		 * 亏上述字节清零。 
		 */
		port->guc_options = NIL;

		while (offset < len)
		{
			char	   *nameptr = buf + offset;
			int32		valoffset;
			char	   *valptr;

			if (*nameptr == '\0')
				break;			/* 找到了包终止符   */
			valoffset = offset + strlen(nameptr) + 1;
			if (valoffset >= len)
				break;			/* 值缺失，下文会就此报错  */
			valptr = buf + valoffset;

			if (strcmp(nameptr, "database") == 0)
				port->database_name = pstrdup(valptr);
			else if (strcmp(nameptr, "user") == 0)
				port->user_name = pstrdup(valptr);
			else if (strcmp(nameptr, "options") == 0)
				port->cmdline_options = pstrdup(valptr);
			else if (strcmp(nameptr, "replication") == 0)
			{
				/*
				 * 由于向后兼容问题，replication 参数         
				 * 是个混合体：取值既可为布尔值，           
				 * 也可以是 'database' 字符串。后者         
				 * 会连接到特定数据库，逻辑解码需要它。    
				 *                        
				 */
				if (strcmp(valptr, "database") == 0)
				{
					am_walsender = true;
					am_db_walsender = true;
				}
				else if (!parse_bool(valptr, &am_walsender))
					ereport(FATAL,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									"replication",
									valptr),
							 errhint("Valid values are: \"false\", 0, \"true\", 1, \"database\".")));
			}
			else if (strncmp(nameptr, "_pq_.", 5) == 0)
			{
				/*
				 * 任何以 _pq_. 开头的选项都保留作协议级选 
				 * 项，但目前尚未定义任何此类选项。         
				 *         
				 */
				unrecognized_protocol_options =
					lappend(unrecognized_protocol_options, pstrdup(nameptr));
			}
			else
			{
				/* 假定它是通用 GUC 选项    */
				port->guc_options = lappend(port->guc_options,
											pstrdup(nameptr));
				port->guc_options = lappend(port->guc_options,
											pstrdup(valptr));

				/*
				 * 碰到 application_name 就复制到 port 中。这是   
				 * 为了在连接授权消息里记录该名称。 
				 * 注意：本应使用 GUC，但我们还没走过 GUC     
				 * 设置流程。                                   
				 */
				if (strcmp(nameptr, "application_name") == 0)
				{
					port->application_name = pg_clean_ascii(valptr, 0);
				}
			}
			offset = valoffset + strlen(valptr) + 1;
		}

		/*
		 * 若没能在给定包长度末尾恰好找到包终止符，   
		 * 就要提出抱怨。         
		 */
		if (offset != len - 1)
			ereport(FATAL,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("invalid startup packet layout: expected terminator as last byte")));

		/*
		 * 如果客户端请求了更新的协议版本，或者请求了  
		 * 我们不认识的协议选项，就把我们支持的最新的  
		 * 次协议版本号，以及无法识别的选项名称          
		 * 一并告知客户端。 
		 */
		if (PG_PROTOCOL_MINOR(proto) > PG_PROTOCOL_MINOR(PG_PROTOCOL_LATEST) ||
			unrecognized_protocol_options != NIL)
			SendNegotiateProtocolVersion(unrecognized_protocol_options);
	}

	/* 检查是否给出用户名  */
	if (port->user_name == NULL || port->user_name[0] == '\0')
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
				 errmsg("no PostgreSQL user name specified in startup packet")));

	/* 数据库默认取用户名。          */
	if (port->database_name == NULL || port->database_name[0] == '\0')
		port->database_name = pstrdup(port->user_name);

	if (Db_user_namespace)
	{
		/*
		 * 若用户名形如 user@，则是全局用户，去掉 '@'。     
		 * 仅当 '@' 位于末尾且用户串中前面没有 '@' 时才     
		 * 这样做，以免他们假冒别库本地用户连入本库，    
		 *          
		 */
		if (strchr(port->user_name, '@') ==
			port->user_name + strlen(port->user_name) - 1)
			*strchr(port->user_name, '@') = '\0';
		else
		{
			/* 追加 '@' 和 dbname */
			port->user_name = psprintf("%s@%s", port->user_name, port->database_name);
		}
	}

	/*
	 * 把给定数据库与用户名截断到 Postgres 名字长度。    
	 * 这样可以避免超长名称导致查找失败。         
	 */
	if (strlen(port->database_name) >= NAMEDATALEN)
		port->database_name[NAMEDATALEN - 1] = '\0';
	if (strlen(port->user_name) >= NAMEDATALEN)
		port->user_name[NAMEDATALEN - 1] = '\0';

	if (am_walsender)
		MyBackendType = B_WAL_SENDER;
	else
		MyBackendType = B_BACKEND;

	/*
	 * 普通的 walsender 后端（例如流复制用途）并不连接 
	 * 到特定数据库。但用于逻辑复制的 walsender 则需要  
	 * 连接到特定数据库。我们允许在已连接数据库的情    
	 * 况下发出流复制命令，因为先做一个基础备份、然     
	 * 后从该备份起流式传输变更，是说得通的。        
	 *                    
	 */
	if (am_walsender && !am_db_walsender)
		port->database_name[0] = '\0';

	/*
	 * TopMemoryContext 阶段到此为止。 
	 */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 如果因为数据库状态要拒绝连接，现在就说出来，   
	 * 免得把 CPU 周期浪费在认证交互上。（这也让编写      
	 * pg_ping 工具成为可能。）        
	 */
	switch (port->canAcceptConnections)
	{
		case CAC_STARTUP:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is starting up")));
			break;
		case CAC_NOTCONSISTENT:
			if (EnableHotStandby)
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is not yet accepting connections"),
						 errdetail("Consistent recovery state has not been yet reached.")));
			else
				ereport(FATAL,
						(errcode(ERRCODE_CANNOT_CONNECT_NOW),
						 errmsg("the database system is not accepting connections"),
						 errdetail("Hot standby mode is disabled.")));
			break;
		case CAC_SHUTDOWN:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is shutting down")));
			break;
		case CAC_RECOVERY:
			ereport(FATAL,
					(errcode(ERRCODE_CANNOT_CONNECT_NOW),
					 errmsg("the database system is in recovery mode")));
			break;
		case CAC_TOOMANY:
			ereport(FATAL,
					(errcode(ERRCODE_TOO_MANY_CONNECTIONS),
					 errmsg("sorry, too many clients already")));
			break;
		case CAC_OK:
			break;
	}

	return STATUS_OK;
}

/*
 * 向客户端发送 NegotiateProtocolVersion。这让客户端知道，   
 * 自己请求的次协议版本新于我们所能支持的。            
 * 我们会讲自己已知的最高版本；如果这成问题，          
 * 客户端当然可以放弃连接。                  
 *
 * 我们还在响应中附上无法识别的协议选项列表。     
 * 这使客户端可以带上可选参数，这些参数可能存在        
 * 于更新的协议版本或第三方协议扩展中，即使这些  
 * 选项不被理解也无需担心重连，同时还能确保客户     
 * 端清楚哪些选项实际被接受了。                                
 *                                         
 */
static void
SendNegotiateProtocolVersion(List *unrecognized_protocol_options)
{
	StringInfoData buf;
	ListCell   *lc;

	pq_beginmessage(&buf, 'v'); /* NegotiateProtocolVersion */
	pq_sendint32(&buf, PG_PROTOCOL_LATEST);
	pq_sendint32(&buf, list_length(unrecognized_protocol_options));
	foreach(lc, unrecognized_protocol_options)
		pq_sendstring(&buf, lfirst(lc));
	pq_endmessage(&buf);

	/* 不必刷新，后面还会有其他消息       */
}

/*
 * 客户端发来的是取消请求包，而不是         
 * 建立新连接的包。执行必要的处理。                 
 * 不回发任何内容给客户端  
 */
static void
processCancelRequest(Port *port, void *pkt)
{
	CancelRequestPacket *canc = (CancelRequestPacket *) pkt;
	int			backendPID;
	int32		cancelAuthCode;
	Backend    *bp;

#ifndef EXEC_BACKEND
	dlist_iter	iter;
#else
	int			i;
#endif

	backendPID = (int) pg_ntoh32(canc->backendPID);
	cancelAuthCode = (int32) pg_ntoh32(canc->cancelAuthCode);

	/*
	 * 看看有没有匹配的后端。在 EXEC_BACKEND 情形下，我们   
	 * 已无法访问 postmaster 自身的后端列表，得依靠共享   
	 * 内存中的副本数组。      
	 */
#ifndef EXEC_BACKEND
	dlist_foreach(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);
#else
	for (i = MaxLivePostmasterChildren() - 1; i >= 0; i--)
	{
		bp = (Backend *) &ShmemBackendArray[i];
#endif
		if (bp->pid == backendPID)
		{
			if (bp->cancel_key == cancelAuthCode)
			{
				/* 找到匹配；向该后端发信号取消当前操作  */
				ereport(DEBUG2,
						(errmsg_internal("processing cancel request: sending SIGINT to process %d",
										 backendPID)));
				signal_child(bp->pid, SIGINT);
			}
			else
				/* PID 对而密钥不对：没门    */
				ereport(LOG,
						(errmsg("wrong key in cancel request for process %d",
								backendPID)));
			return;
		}
#ifndef EXEC_BACKEND			/* 让 GNU Emacs 26.1 看到括号配平 */
	}
#else
	}
#endif

	/* 无匹配的后端  */
	ereport(LOG,
			(errmsg("PID %d in cancel request did not match any process",
					backendPID)));
}

/*
 * canAcceptConnections —— 检查数据库状态是否允许指定       
 * 类型的连接。backend_type 可为 BACKEND_TYPE_NORMAL、    
 * BACKEND_TYPE_AUTOVAC 或 BACKEND_TYPE_BGWORKER。（注意我们        
 * 尚不知道 NORMAL 连接是否会转变成 walsender。）  
 */
static CAC_state
canAcceptConnections(int backend_type)
{
	CAC_state	result = CAC_OK;

	/*
	 * 处于启动/关闭/不一致恢复状态时不能启动后端。  
	 * 为此目的，autovac worker 与用户后端同等对待。       
	 * 不过 bgworker 不受此检查约束；我们认为               
	 * bgworker_should_start_now() 已判定 DB 状态是否允许它们。 
	 */
	if (pmState != PM_RUN && pmState != PM_HOT_STANDBY &&
		backend_type != BACKEND_TYPE_BGWORKER)
	{
		if (Shutdown > NoShutdown)
			return CAC_SHUTDOWN;	/* 关闭已被挂起  */
		else if (!FatalError && pmState == PM_STARTUP)
			return CAC_STARTUP; /* 正常启动   */
		else if (!FatalError && pmState == PM_RECOVERY)
			return CAC_NOTCONSISTENT;	/* not yet at consistent recovery
										 * state */
		else
			return CAC_RECOVERY;	/* 否则必定是崩溃恢复 */
	}

	/*
	 * "智能关闭"的限制只适用于普通连接，                   
	 * 不适用于 autovac/bgworker。    
	 */
	if (!connsAllowed && backend_type == BACKEND_TYPE_NORMAL)
		return CAC_SHUTDOWN;	/* 关闭已被挂起  */

	/*
	 * 子进程别启动太多。   
	 *
	 * 此处允许的连接数多于后端数上限，因为有些连接   
	 * 可能尚在认证；它们可能认证失败，或者某些已有   
	 * 后端会在认证周期完成前退出。确切的 MaxBackends  
	 * 上限会在新后端试图加入共享失效后端数组时      
	 * 强制执行。            
	 *
	 * 这里的限制必须与每个子进程数组的大小相符；     
	 * 见 MaxLivePostmasterChildren() 注释。    
	 */
	if (CountChildren(BACKEND_TYPE_ALL) >= MaxLivePostmasterChildren())
		result = CAC_TOOMANY;

	return result;
}


/*
 * ConnCreate —— 创建本地连接数据结构      
 *
 * 失败时返回 NULL；只有内存耗尽是致命的。         
 */
static Port *
ConnCreate(int serverFd)
{
	Port	   *port;

	if (!(port = (Port *) calloc(1, sizeof(Port))))
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		ExitPostmaster(1);
	}

	if (StreamConnection(serverFd, port) != STATUS_OK)
	{
		if (port->sock != PGINVALID_SOCKET)
			StreamClose(port->sock);
		ConnFree(port);
		return NULL;
	}

	return port;
}


/*
 * ConnFree —— 释放本地连接数据结构    
 *
 * 调用者已关闭套接字（如有），这里没多少事    
 * 可做。  
 */
static void
ConnFree(Port *port)
{
	free(port);
}


/*
 * ClosePostmasterPorts —— 关闭 postmaster 全部套接字  
 *
 * 在子进程启动期间调用，用来释放该子进程用不着     
 * 的文件描述符。当然，postmaster 自己仍持有它们。   
 *                      
 *
 * 注意：这里以布尔值传入 am_syslogger，因为我们不    
 * 想在此时就设置那个全局变量。  
 */
void
ClosePostmasterPorts(bool am_syslogger)
{
	int			i;

	/* 释放 postmaster 的 WaitEventSet 持有的资源。    */
	if (pm_wait_set)
	{
		FreeWaitEventSetAfterFork(pm_wait_set);
		pm_wait_set = NULL;
	}

#ifndef WIN32

	/*
	 * 关闭 postmaster 死亡监视管道的写端。要尽早做，      
	 * 这一点很重要：这样一旦 postmaster 死亡，别人就不会 
	 * 因为我们还握着管道不放而误以为它仍在运行。   
	 */
	if (close(postmaster_alive_fds[POSTMASTER_FD_OWN]) != 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not close postmaster death monitoring pipe in child process: %m")));
	postmaster_alive_fds[POSTMASTER_FD_OWN] = -1;
	/* 告知 fd.c 已释放一个管道 FD。   */
	ReleaseExternalFD();
#endif

	/*
	 * 关闭 postmaster 的监听套接字。fd.c 并不跟踪它们，    
	 * 因此不用 ReleaseExternalFD()。       
	 */
	for (i = 0; i < MAXLISTEN; i++)
	{
		if (ListenSocket[i] != PGINVALID_SOCKET)
		{
			StreamClose(ListenSocket[i]);
			ListenSocket[i] = PGINVALID_SOCKET;
		}
	}

	/*
	 * 如果使用 syslogger，就关闭管道的读端。这个同样     
	 * 不必在 fd.c 中跟踪。   
	 */
	if (!am_syslogger)
	{
#ifndef WIN32
		if (syslogPipe[0] >= 0)
			close(syslogPipe[0]);
		syslogPipe[0] = -1;
#else
		if (syslogPipe[0])
			CloseHandle(syslogPipe[0]);
		syslogPipe[0] = 0;
#endif
	}

#ifdef USE_BONJOUR
	/* 若用 Bonjour，则关闭与 mDNS 守护进程的连接  */
	if (bonjour_sdref)
		close(DNSServiceRefSockFD(bonjour_sdref));
#endif
}


/*
 * InitProcessGlobals —— 设 MyStartTime、随机种子  
 *
 * 在 postmaster 和各后端中都很早调用。 
 */
void
InitProcessGlobals(void)
{
	MyStartTimestamp = GetCurrentTimestamp();
	MyStartTime = timestamptz_to_time_t(MyStartTimestamp);

	/*
	 * 每个进程中都设置不同的全局种子。我们想要    
	 * 不可预测的东西，所以如有可能，用高质量随机位 
	 * 作为种子。否则就用基于时间戳和 PID 的种子。   
	 */
	if (unlikely(!pg_prng_strong_seed(&pg_global_prng_state)))
	{
		uint64		rseed;

		/*
		 * 由于 PID 与时间戳的低位变化更频繁，将时间戳   
		 * 左移，以便在给定时段内有更大的种子总数。      
		 * 但这样时间戳只剩 20 位，约每 1 秒循环一次，  
		 * 所以还要混入一些更高位。                           
		 *                              
		 */
		rseed = ((uint64) MyProcPid) ^
			((uint64) MyStartTimestamp << 12) ^
			((uint64) MyStartTimestamp >> 20);

		pg_prng_seed(&pg_global_prng_state, rseed);
	}

	/*
	 * 还要确保为 random(3) 设好优质种子。核心 Postgres       
	 * 已不再推荐使用它，但扩展可能会用。         
	 */
#ifndef WIN32
	srandom(pg_prng_uint32(&pg_global_prng_state));
#endif
}

/*
 * 子进程用 SIGUSR1 发送 'pmsignals' 来通知我们。pg_ctl 用 
 * SIGUSR1 请求 postmaster 检查 logrotate 和 promote 文件。   
 */
static void
handle_pm_pmsignal_signal(SIGNAL_ARGS)
{
	int			save_errno = errno;

	pending_pm_pmsignal = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * pg_ctl 使用 SIGHUP 请求重新加载配置文件。            
 */
static void
handle_pm_reload_request_signal(SIGNAL_ARGS)
{
	int			save_errno = errno;

	pending_pm_reload_request = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * 重读配置文件，并让子进程照做。      
 */
static void
process_pm_reload_request(void)
{
	pending_pm_reload_request = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received reload request signal")));

	if (Shutdown <= SmartShutdown)
	{
		ereport(LOG,
				(errmsg("received SIGHUP, reloading configuration files")));
		ProcessConfigFile(PGC_SIGHUP);
		SignalChildren(SIGHUP);
		if (StartupPID != 0)
			signal_child(StartupPID, SIGHUP);
		if (BgWriterPID != 0)
			signal_child(BgWriterPID, SIGHUP);
		if (CheckpointerPID != 0)
			signal_child(CheckpointerPID, SIGHUP);
		if (WalWriterPID != 0)
			signal_child(WalWriterPID, SIGHUP);
		if (WalReceiverPID != 0)
			signal_child(WalReceiverPID, SIGHUP);
		if (AutoVacPID != 0)
			signal_child(AutoVacPID, SIGHUP);
		if (PgArchPID != 0)
			signal_child(PgArchPID, SIGHUP);
		if (SysLoggerPID != 0)
			signal_child(SysLoggerPID, SIGHUP);

		/* 同时重新加载认证配置文件   */
		if (!load_hba())
			ereport(LOG,
			/* translator: %s is a configuration file */
					(errmsg("%s was not reloaded", HbaFileName)));

		if (!load_ident())
			ereport(LOG,
					(errmsg("%s was not reloaded", IdentFileName)));

#ifdef USE_SSL
		/* 同时重新加载 SSL 配置    */
		if (EnableSSL)
		{
			if (secure_initialize(false) == 0)
				LoadedSSL = true;
			else
				ereport(LOG,
						(errmsg("SSL configuration was not reloaded")));
		}
		else
		{
			secure_destroy();
			LoadedSSL = false;
		}
#endif

#ifdef EXEC_BACKEND
		/* 更新供后续子进程使用的起始点文件   */
		write_nondefault_variables(PGC_SIGHUP);
#endif
	}
}

/*
 * pg_ctl 用 SIGTERM、SIGINT、SIGQUIT 请求不同类型的          
 * 关闭。
 */
static void
handle_pm_shutdown_request_signal(SIGNAL_ARGS)
{
	int			save_errno = errno;

	switch (postgres_signal_arg)
	{
		case SIGTERM:
			/* 另两个标志都未设置时即隐含 smart      */
			pending_pm_shutdown_request = true;
			break;
		case SIGINT:
			pending_pm_fast_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
		case SIGQUIT:
			pending_pm_immediate_shutdown_request = true;
			pending_pm_shutdown_request = true;
			break;
	}
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * 处理关闭请求。    
 */
static void
process_pm_shutdown_request(void)
{
	int			mode;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received shutdown request signal")));

	pending_pm_shutdown_request = false;

	/*
	 * 若自上次服务器循环以来收到了不止一个关闭          
	 * 请求信号，则取其中最紧迫的那个。这与按任意 
	 * 顺序逐一处理它们时本会适用的优先级相匹配。        
	 */
	if (pending_pm_immediate_shutdown_request)
	{
		pending_pm_immediate_shutdown_request = false;
		pending_pm_fast_shutdown_request = false;
		mode = ImmediateShutdown;
	}
	else if (pending_pm_fast_shutdown_request)
	{
		pending_pm_fast_shutdown_request = false;
		mode = FastShutdown;
	}
	else
		mode = SmartShutdown;

	switch (mode)
	{
		case SmartShutdown:

			/*
			 * 智能关闭：
			 *
			 * 等待子进程结束其工作，然后关闭。    
			 */
			if (Shutdown >= SmartShutdown)
				break;
			Shutdown = SmartShutdown;
			ereport(LOG,
					(errmsg("received smart shutdown request")));

			/* 报告状态  */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/*
			 * 若已达到正常运行，就直接进入等待客户端  
			 * 后端退出。若已处于 PM_STOP_BACKENDS 或更          
			 * 晚的状态，则不更改。
			 */
			if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
				connsAllowed = false;
			else if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* 此时应无客户端，因此继续停止子进程     */
				pmState = PM_STOP_BACKENDS;
			}

			/*
			 * 现在等待在线备份模式结束以及后端退出。若已
			 * 是这种情况，PostmasterStateMachine 会采取下一      
			 * 步骤。 
			 */
			PostmasterStateMachine();
			break;

		case FastShutdown:

			/*
			 * 快速关闭  
			 *
			 * 用 SIGTERM 中止所有子进程（回滚活动事务并    
			 * 退出），等它们消失后即关闭。 
			 */
			if (Shutdown >= FastShutdown)
				break;
			Shutdown = FastShutdown;
			ereport(LOG,
					(errmsg("received fast shutdown request")));

			/* 报告状态  */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			if (pmState == PM_STARTUP || pmState == PM_RECOVERY)
			{
				/* 直接静默关闭后台进程               */
				pmState = PM_STOP_BACKENDS;
			}
			else if (pmState == PM_RUN ||
					 pmState == PM_HOT_STANDBY)
			{
				/* 报告即将清除存活的客户端会话          */
				ereport(LOG,
						(errmsg("aborting any active transactions")));
				pmState = PM_STOP_BACKENDS;
			}

			/*
			 * PostmasterStateMachine 会发出任何必要的信号，若
			 * 无需杀死任何子进程，则采取下一步。        
			 */
			PostmasterStateMachine();
			break;

		case ImmediateShutdown:

			/*
			 * 立即关闭：    
			 *
			 * 用 SIGQUIT 中止所有子进程，等它们退出， 
			 * 再用 SIGKILL 终止余下的，然后退出而不     
			 * 尝试妥善关闭数据库系统。               
			 */
			if (Shutdown >= ImmediateShutdown)
				break;
			Shutdown = ImmediateShutdown;
			ereport(LOG,
					(errmsg("received immediate shutdown request")));

			/* 报告状态  */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STOPPING);
#ifdef USE_SYSTEMD
			sd_notify(0, "STOPPING=1");
#endif

			/* 让子进程尽快关闭        */
			/* （此处不适用 send_abort_for_crash）      */
			SetQuitSignalReason(PMQUIT_FOR_STOP);
			TerminateChildren(SIGQUIT);
			pmState = PM_WAIT_BACKENDS;

			/* 为其死亡设定计时器   */
			AbortStartTime = time(NULL);

			/*
			 * 现在等待后端退出。若已没有后端，  
			 * PostmasterStateMachine 会采取下一步。   
			 */
			PostmasterStateMachine();
			break;
	}
}

static void
handle_pm_child_exit_signal(SIGNAL_ARGS)
{
	int			save_errno = errno;

	pending_pm_child_exit = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * 子进程死亡后的清理。     
 */
static void
process_pm_child_exit(void)
{
	int			pid;			/* 已死子进程的进程 ID      */
	int			exitstatus;		/* 其退出状态 */

	pending_pm_child_exit = false;

	ereport(DEBUG4,
			(errmsg_internal("reaping dead processes")));

	while ((pid = waitpid(-1, &exitstatus, WNOHANG)) > 0)
	{
		/*
		 * 检查该子进程是否为 startup 进程
		 */
		if (pid == StartupPID)
		{
			StartupPID = 0;

			/*
			 * startup 进程因响应关闭请求而退出（或者是       
			 * 无视关闭请求而正常完成）。                
			 */
			if (Shutdown > NoShutdown &&
				(EXIT_STATUS_0(exitstatus) || EXIT_STATUS_1(exitstatus)))
			{
				StartupStatus = STARTUP_NOT_RUNNING;
				pmState = PM_WAIT_BACKENDS;
				/* 其余由 PostmasterStateMachine 处理    */
				continue;
			}

			if (EXIT_STATUS_3(exitstatus))
			{
				ereport(LOG,
						(errmsg("shutdown at recovery target")));
				StartupStatus = STARTUP_NOT_RUNNING;
				Shutdown = Max(Shutdown, SmartShutdown);
				TerminateChildren(SIGTERM);
				pmState = PM_WAIT_BACKENDS;
				/* 其余由 PostmasterStateMachine 处理    */
				continue;
			}

			/*
			 * startup 进程意外退出（包括 FATAL 退出）在   
			 * PM_STARTUP 期间被视为灾难性事件。此时还     
			 * 没有其他进程在运行，可直接退出。 
			 */
			if (pmState == PM_STARTUP &&
				StartupStatus != STARTUP_SIGNALED &&
				!EXIT_STATUS_0(exitstatus))
			{
				LogChildExit(LOG, _("startup process"),
							 pid, exitstatus);
				ereport(LOG,
						(errmsg("aborting startup due to startup process failure")));
				ExitPostmaster(1);
			}

			/*
			 * PM_STARTUP 之后，startup 进程的任何意外退出        
			 * （包括 FATAL 退出）均为灾难，须杀死其他     
			 * 子进程，并设置 StartupStatus 以免在它们死后  
			 * 再尝试重新初始化。例外：若 StartupStatus 为      
			 * STARTUP_SIGNALED，则说明我们先前已向 startup   
			 * 进程发送过 SIGQUIT，故它多半因此而死，且    
			 * 确实应重启。   
			 *
			 * 这一段还处理另一种情形：我们在 PM_STARTUP 
			 * 期间因某个 dead_end 子进程崩溃而发送 SIGQUIT：  
			 * 在此情形下，若 startup 进程死于该 SIGQUIT，我们
			 * 就要转入 PM_WAIT_BACKENDS 状态，以便允许     
			 * PostmasterStateMachine 重启 startup 进程。（反过       
			 * 来说，若 SIGQUIT 发得太迟，startup 进程或许能   
			 * 正常完成。那样的话我们会顺延进入，     
			 * 开始正常运作。）                
			 */
			if (!EXIT_STATUS_0(exitstatus))
			{
				if (StartupStatus == STARTUP_SIGNALED)
				{
					StartupStatus = STARTUP_NOT_RUNNING;
					if (pmState == PM_STARTUP)
						pmState = PM_WAIT_BACKENDS;
				}
				else
					StartupStatus = STARTUP_CRASHED;
				HandleChildCrash(pid, exitstatus,
								 _("startup process"));
				continue;
			}

			/*
			 * 启动成功，开始正常运作。         
			 */
			StartupStatus = STARTUP_NOT_RUNNING;
			FatalError = false;
			AbortStartTime = 0;
			ReachedNormalRunning = true;
			pmState = PM_RUN;
			connsAllowed = true;

			/*
			 * 启动后台任务，若在进入一致性恢复状态时  
			 * 尚未做过此事。就算失败也不要紧，我们稍后 
			 * 会再次尝试。                        
			 */
			if (CheckpointerPID == 0)
				CheckpointerPID = StartCheckpointer();
			if (BgWriterPID == 0)
				BgWriterPID = StartBackgroundWriter();
			if (WalWriterPID == 0)
				WalWriterPID = StartWalWriter();

			/*
			 * 同样地，按需启动其他特殊子进程。在重启的   
			 * 情形中，其中一些可能已在运行。
			 */
			if (!IsBinaryUpgrade && AutoVacuumingActive() && AutoVacPID == 0)
				AutoVacPID = StartAutoVacLauncher();
			if (PgArchStartupAllowed() && PgArchPID == 0)
				PgArchPID = StartArchiver();

			/* worker 现在可能已被排定启动 */
			maybe_start_bgworkers();

			/* 此刻我们真正开始对外提供服务    */
			ereport(LOG,
					(errmsg("database system is ready to accept connections")));

			/* 报告状态  */
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif

			continue;
		}

		/*
		 * 是 bgwriter 吗？正常退出可以忽略；必要时会在      
		 * postmaster 主循环的下一轮迭代中启动新的。   
		 * 其他任何退出情形均视为崩溃。                 
		 */
		if (pid == BgWriterPID)
		{
			BgWriterPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("background writer process"));
			continue;
		}

		/*
		 * 是 checkpointer 吗？ 
		 */
		if (pid == CheckpointerPID)
		{
			CheckpointerPID = 0;
			if (EXIT_STATUS_0(exitstatus) && pmState == PM_SHUTDOWN)
			{
				/*
				 * 好，checkpointer 被告知关闭后已正常退出。   
				 * 我们预期它已写下关闭检查点。            
				 * （若因某种原因未写，下次 postmaster 启动   
				 * 时会执行恢复。）        
				 *
				 * 此时应无普通后端子进程（否则不会在    
				 * PM_SHUTDOWN 状态）但可能还有需要等待的      
				 * dead_end 子进程。              
				 *
				 * 若有 archiver 子进程，让它做最后一轮归档
				 * 然后退出。同样，若有 walsender 进程，    
				 * 让它们发送剩余的 WAL 然后退出。            
				 */
				Assert(Shutdown > NoShutdown);

				/* 最后一次唤醒 archiver      */
				if (PgArchPID != 0)
					signal_child(PgArchPID, SIGUSR2);

				/*
				 * 最后一次唤醒 walsender。不应再有任何      
				 * 普通后端了。       
				 */
				SignalChildren(SIGUSR2);

				pmState = PM_SHUTDOWN_2;
			}
			else
			{
				/*
				 * checkpointer 任何意外退出（包括 FATAL 退出）
				 * 均按崩溃处理。       
				 */
				HandleChildCrash(pid, exitstatus,
								 _("checkpointer process"));
			}

			continue;
		}

		/*
		 * 是 wal writer 吗？正常退出可以忽略；必要时会在  
		 * postmaster 主循环的下一轮迭代中启动新的。       
		 * 其他任何退出情形均视为崩溃。                 
		 */
		if (pid == WalWriterPID)
		{
			WalWriterPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL writer process"));
			continue;
		}

		/*
		 * 是 wal receiver 吗？若退出状态为 0（正常）或 1（ 
		 * FATAL 退出），我们像对待普通后端一样，认定    
		 * 一切正常。（若需要新的 wal receiver，会在          
		 * postmaster 主循环下轮迭代启动。）  
		 */
		if (pid == WalReceiverPID)
		{
			WalReceiverPID = 0;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("WAL receiver process"));
			continue;
		}

		/*
		 * 是 autovacuum launcher 吗？正常退出可以忽略；我们  
		 * 会在 postmaster 主循环下一轮迭代中启动新的。  
		 * 必要时如此。任何其他退出情形均视为崩溃。 
		 *       
		 */
		if (pid == AutoVacPID)
		{
			AutoVacPID = 0;
			if (!EXIT_STATUS_0(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("autovacuum launcher process"));
			continue;
		}

		/*
		 * 是 archiver 吗？若退出状态为 0（正常）或 1（FATAL   
		 * 退出），就像对待普通后端一样认定一切正常，并
		 * 只须重启新 archiver，立即重试归档余下文件。  
		 * （若失败，则会在 postmaster 主循环以后的        
		 * 循环中再次尝试。）除非我们正在等它关闭；     
		 * 那种情况下不要重启它，而由            
		 * PostmasterStateMachine() 推进到下一个关闭步骤。      
		 */
		if (pid == PgArchPID)
		{
			PgArchPID = 0;
			if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
				HandleChildCrash(pid, exitstatus,
								 _("archiver process"));
			if (PgArchStartupAllowed())
				PgArchPID = StartArchiver();
			continue;
		}

		/* 是系统 logger 吗？若是，尝试启动新的       */
		if (pid == SysLoggerPID)
		{
			SysLoggerPID = 0;
			/* 为安全起见，先启动新 logger        */
			SysLoggerPID = SysLogger_Start();
			if (!EXIT_STATUS_0(exitstatus))
				LogChildExit(LOG, _("system logger process"),
							 pid, exitstatus);
			continue;
		}

		/* 是后台工作进程之一吗？     */
		if (CleanupBackgroundWorker(pid, exitstatus))
		{
			/* 让它重新启动   */
			HaveCrashedWorker = true;
			continue;
		}

		/*
		 * 否则照标准清理后端子进程。
		 */
		CleanupBackend(pid, exitstatus);
	}							/* 遍历挂起的子进程死亡报告  */

	/*
	 * 清空 SIGCHLD 队列后，看看是否有需要做出状态变更    
	 * 或动作。       
	 */
	PostmasterStateMachine();
}

/*
 * 扫描 bgworkers 列表，查看给定的 PID（刚刚停止或崩溃的
 * 那个进程）是否在其中。若是，处理其关闭并返回 true。  
 * 否则返回 false。  
 *
 * 此函数大量借鉴了 CleanupBackend。一个重要区别在于：     
 * 我们还不知道正在死去的进程是否为 bgworker，因此在       
 * 确认前须沉默。  
 */
static bool
CleanupBackgroundWorker(int pid,
						int exitstatus) /* 其退出状态     */
{
	char		namebuf[MAXPGPATH];
	slist_mutable_iter iter;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		if (rw->rw_pid != pid)
			continue;

#ifdef WIN32
		/* 见 CleanupBackend */
		if (exitstatus == ERROR_WAIT_NO_CHILDREN)
			exitstatus = 0;
#endif

		snprintf(namebuf, MAXPGPATH, _("background worker \"%s\""),
				 rw->rw_worker.bgw_type);


		if (!EXIT_STATUS_0(exitstatus))
		{
			/* 记录时间戳，以便知道何时重启该 worker。  */
			rw->rw_crashed_at = GetCurrentTimestamp();
		}
		else
		{
			/* 退出状态为零表示终止   */
			rw->rw_crashed_at = 0;
			rw->rw_terminate = true;
		}

		/*
		 * 此外，与后端一样，退出状态非 0/1 即视为崩溃， 
		 * 并将导致全系统重启。                           
		 */
		if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
		{
			HandleChildCrash(pid, exitstatus, namebuf);
			return true;
		}

		/*
		 * 必须释放 postmaster 子进程槽位。若 worker 未能这样 
		 * 做，即它未自行清理，需一次崩溃重启循环。      
		 *       
		 */
		if (!ReleasePostmasterChildSlot(rw->rw_child_slot))
		{
			HandleChildCrash(pid, exitstatus, namebuf);
			return true;
		}

		/* 将其从 BackendList 中移除并清空其余数据       */
		dlist_delete(&rw->rw_backend->elem);
#ifdef EXEC_BACKEND
		ShmemBackendArrayRemove(rw->rw_backend);
#endif

		/*
		 * 该 worker 可能启动了其他后台工作进程并请求 
		 * 在该 worker 启动或停止时收到通知。若是，则取消  
		 * 所有发给现已死亡后端的此类通知。             
		 *                  
		 */
		if (rw->rw_backend->bgworker_notify)
			BackgroundWorkerStopNotifications(rw->rw_pid);
		free(rw->rw_backend);
		rw->rw_backend = NULL;
		rw->rw_pid = 0;
		rw->rw_child_slot = 0;
		ReportBackgroundWorkerExit(&iter);	/* 报告 child 死亡*/

		LogChildExit(EXIT_STATUS_0(exitstatus) ? DEBUG1 : LOG,
					 namebuf, pid, exitstatus);

		return true;
	}

	return false;
}

/*
 * CleanupBackend——清理已终止的后端。    
 *
 * 移除与后端关联的全部本地状态。  
 *
 * 若修改此处，另见 CleanupBackgroundWorker。  
 */
static void
CleanupBackend(int pid,
			   int exitstatus)	/* 其退出状态。   */
{
	dlist_mutable_iter iter;

	LogChildExit(DEBUG2, _("server process"), pid, exitstatus);

	/*
	 * 若后端死得难看，就必须通知所有其他后端执行        
	 * quickdie。若退出状态为 0（正常）或 1（FATAL 退出），
	 * 我们就认为一切正常，并继续将该后端从活动后端   
	 * 列表中移除。      
	 */

#ifdef WIN32

	/*
	 * 在 win32 上，ERROR_WAIT_NO_CHILDREN (128) 也被视为          
	 * 非致命情形，高负载下进程有时会启动失败（远在     
	 * 它开始使用共享内存之前）。微软称这与互斥锁失败  
	 * 有关：                      
	 * http://archives.postgresql.org/pgsql-hackers/2010-09/msg00790.php
	 */
	if (exitstatus == ERROR_WAIT_NO_CHILDREN)
	{
		LogChildExit(LOG, _("server process"), pid, exitstatus);
		exitstatus = 0;
	}
#endif

	if (!EXIT_STATUS_0(exitstatus) && !EXIT_STATUS_1(exitstatus))
	{
		HandleChildCrash(pid, exitstatus, _("server process"));
		return;
	}

	dlist_foreach_modify(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->pid == pid)
		{
			if (!bp->dead_end)
			{
				if (!ReleasePostmasterChildSlot(bp->child_slot))
				{
					/*
					 * 糟糕，子进程未能自行清理。仍将其视为 
					 * 崩溃处理。 
					 */
					HandleChildCrash(pid, exitstatus, _("server process"));
					return;
				}
#ifdef EXEC_BACKEND
				ShmemBackendArrayRemove(bp);
#endif
			}
			if (bp->bgworker_notify)
			{
				/*
				 * 某 worker 启停时，此后端可能已被安排接收 
				 * SIGUSR1 信号。须取消这些通知，我们不想   
				 * 向非 PostgreSQL 后端的 PID 发信号。若后端从未
				 * 请求过此类通知，（多半常见）则会跳过  
				 * 此步。                                                  
				 *                    
				 */
				BackgroundWorkerStopNotifications(bp->pid);
			}
			dlist_delete(iter.cur);
			free(bp);
			break;
		}
	}
}

/*
 * HandleChildCrash——在后端、bgwriter、checkpointer、walwriter、 
 * autovacuum、archiver 或 bgworker 崩溃后清理。
 *
 * 此处目标在于清理我们关于该子进程的本地状态， 
 * 并向所有其余存留的子进程发信号让其 quickdie。 
 */
static void
HandleChildCrash(int pid, int exitstatus, const char *procname)
{
	dlist_mutable_iter iter;
	slist_iter	siter;
	Backend    *bp;
	bool		take_action;

	/*
	 * 仅当这是首次进程崩溃且非立即关闭时，我们才   
	 * 记录消息并发送信号；否则我们在此只是为了更新    
	 * postmaster 对存活进程的认知。如果我们已经向子进      
	 * 程发过信号，非零退出属意料之中，所以不要      
	 * 塞满日志
	 */
	take_action = !FatalError && Shutdown != ImmediateShutdown;

	if (take_action)
	{
		LogChildExit(LOG, procname, pid, exitstatus);
		ereport(LOG,
				(errmsg("terminating any other active server processes")));
		SetQuitSignalReason(PMQUIT_FOR_CRASH);
	}

	/* 处理后台工作进程。 */
	slist_foreach(siter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, siter.cur);
		if (rw->rw_pid == 0)
			continue;			/* 未在运行*/
		if (rw->rw_pid == pid)
		{
			/*
			 * 找到刚死 worker 表项，故将其移除。   
			 */
			(void) ReleasePostmasterChildSlot(rw->rw_child_slot);
			dlist_delete(&rw->rw_backend->elem);
#ifdef EXEC_BACKEND
			ShmemBackendArrayRemove(rw->rw_backend);
#endif
			free(rw->rw_backend);
			rw->rw_backend = NULL;
			rw->rw_pid = 0;
			rw->rw_child_slot = 0;
			/* 不要重置 crashed_at*/
			/* 也不要报告子进程停止  */
			/* 继续循环，以便向其余 worker 发信号 */
		}
		else
		{
			/*
			 * 该 worker 仍然存活。除非已这样做过，否则叫它
			 * 切腹自尽。     
			 */
			if (take_action)
				sigquit_child(rw->rw_pid);
		}
	}

	/* 处理普通后端       */
	dlist_foreach_modify(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);

		if (bp->pid == pid)
		{
			/*
			 * 找到刚死 backend 的表项，故将其移除。
			 */
			if (!bp->dead_end)
			{
				(void) ReleasePostmasterChildSlot(bp->child_slot);
#ifdef EXEC_BACKEND
				ShmemBackendArrayRemove(bp);
#endif
			}
			dlist_delete(iter.cur);
			free(bp);
			/* 继续循环，以便向其余后端发信号    */
		}
		else
		{
			/*
			 * 该后端仍然存活。除非已这样做过，否则叫它   
			 * 切腹自尽。     
			 *
			 * 本可将 dead_end 子进程排除在外，但至少发送
			 * SIGABRT 时将它们包括在内似乎更好。 
			 *
			 * 后台工作进程上面已处理过；此处忽略。      
			 *      
			 */
			if (bp->bkend_type == BACKEND_TYPE_BGWORKER)
				continue;

			if (take_action)
				sigquit_child(bp->pid);
		}
	}

	/* 也要处理 startup 进程          */
	if (pid == StartupPID)
	{
		StartupPID = 0;
		/* 调用方会调整 StartupStatus，此处别动它   */
	}
	else if (StartupPID != 0 && take_action)
	{
		sigquit_child(StartupPID);
		StartupStatus = STARTUP_SIGNALED;
	}

	/* 也要照管 bgwriter         */
	if (pid == BgWriterPID)
		BgWriterPID = 0;
	else if (BgWriterPID != 0 && take_action)
		sigquit_child(BgWriterPID);

	/* 也要照管 checkpointer         */
	if (pid == CheckpointerPID)
		CheckpointerPID = 0;
	else if (CheckpointerPID != 0 && take_action)
		sigquit_child(CheckpointerPID);

	/* 也要照管 walwriter         */
	if (pid == WalWriterPID)
		WalWriterPID = 0;
	else if (WalWriterPID != 0 && take_action)
		sigquit_child(WalWriterPID);

	/* 也要照管 walreceiver         */
	if (pid == WalReceiverPID)
		WalReceiverPID = 0;
	else if (WalReceiverPID != 0 && take_action)
		sigquit_child(WalReceiverPID);

	/* 也要照管 autovacuum launcher         */
	if (pid == AutoVacPID)
		AutoVacPID = 0;
	else if (AutoVacPID != 0 && take_action)
		sigquit_child(AutoVacPID);

	/* 也要照管 archiver         */
	if (pid == PgArchPID)
		PgArchPID = 0;
	else if (PgArchPID != 0 && take_action)
		sigquit_child(PgArchPID);

	/* 我们绝不重启 syslogger    */

	if (Shutdown != ImmediateShutdown)
		FatalError = true;

	/* 现在转入等待子进程消亡的状态                 */
	if (pmState == PM_RECOVERY ||
		pmState == PM_HOT_STANDBY ||
		pmState == PM_RUN ||
		pmState == PM_STOP_BACKENDS ||
		pmState == PM_SHUTDOWN)
		pmState = PM_WAIT_BACKENDS;

	/*
	 * 而如果它们没有很快消亡，就轮到我们开始计时，    
	 * 毫不留情地杀掉它们。    
	 */
	if (AbortStartTime == 0)
		AbortStartTime = time(NULL);
}

/*
 * 记录子进程的消亡。      
 */
static void
LogChildExit(int lev, const char *procname, int pid, int exitstatus)
{
	/*
	 * activity_buffer 大小随意，但设为等于默认值       
	 * track_activity_query_size
	 */
	char		activity_buffer[1024];
	const char *activity = NULL;

	if (!EXIT_STATUS_0(exitstatus))
		activity = pgstat_get_crashed_backend_activity(pid,
													   activity_buffer,
													   sizeof(activity_buffer));

	if (WIFEXITED(exitstatus))
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) exited with exit code %d",
						procname, pid, WEXITSTATUS(exitstatus)),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
	else if (WIFSIGNALED(exitstatus))
	{
#if defined(WIN32)
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) was terminated by exception 0x%X",
						procname, pid, WTERMSIG(exitstatus)),
				 errhint("See C include file \"ntstatus.h\" for a description of the hexadecimal value."),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) was terminated by signal %d: %s",
						procname, pid, WTERMSIG(exitstatus),
						pg_strsignal(WTERMSIG(exitstatus))),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
#endif
	}
	else
		ereport(lev,

		/*------
		  translator: %s is a noun phrase describing a child process, such as
		  "server process" */
				(errmsg("%s (PID %d) exited with unrecognized status %d",
						procname, pid, exitstatus),
				 activity ? errdetail("Failed process was running: %s", activity) : 0));
}

/*
 * 推进 postmaster 的状态机并酌情采取相应动作。          
 *
 * 这是 process_pm_shutdown_request()、               
 * process_pm_child_exit() 和 process_pm_pmsignal() 的公共代码，        
 * 处理可能需改变状态的信号。 
 */
static void
PostmasterStateMachine(void)
{
	/* 若执行智能关闭，则尝试推进该状态。         */
	if (pmState == PM_RUN || pmState == PM_HOT_STANDBY)
	{
		if (!connsAllowed)
		{
			/*
			 * 没有普通客户端后端在运行时，此状态结束。   
			 * 然后便可停止其他子进程。    
			 */
			if (CountChildren(BACKEND_TYPE_NORMAL) == 0)
				pmState = PM_STOP_BACKENDS;
		}
	}

	/*
	 * 如果已就绪，就通知子进程关闭。（这                 
	 * 并非持久状态，但把它当作一个独立的 pmState           
	 * 可在多条关闭代码路径间共用此代码。）     
	 */
	if (pmState == PM_STOP_BACKENDS)
	{
		/*
		 * 忘掉所有未决的后台 worker 请求，因为我们不再    
		 * 愿意启动任何新 worker。（若有额外请求到来，     
		 * BackgroundWorkerStateChange 会拒绝它们。）     
		 */
		ForgetUnstartedBackgroundWorkers();

		/* 通知除 walsender 外的后端子进程     */
		SignalSomeChildren(SIGTERM,
						   BACKEND_TYPE_ALL - BACKEND_TYPE_WALSND);
		/* 以及 autovac launcher      */
		if (AutoVacPID != 0)
			signal_child(AutoVacPID, SIGTERM);
		/* 以及 bgwriter      */
		if (BgWriterPID != 0)
			signal_child(BgWriterPID, SIGTERM);
		/* 以及 walwriter      */
		if (WalWriterPID != 0)
			signal_child(WalWriterPID, SIGTERM);
		/* 若在恢复中，也停止 startup 与 walreceiver 进程    */
		if (StartupPID != 0)
			signal_child(StartupPID, SIGTERM);
		if (WalReceiverPID != 0)
			signal_child(WalReceiverPID, SIGTERM);
		/* checkpointer、archiver、stats 与 syslogger 暂可继续        */

		/* 现在转入 PM_WAIT_BACKENDS 状态，等待它们消亡        */
		pmState = PM_WAIT_BACKENDS;
	}

	/*
	 * 如果当前状态机状态意味着正在等待后端退出，        
	 * 检查是否都已消失，若是则改变状态。   
	 */
	if (pmState == PM_WAIT_BACKENDS)
	{
		/*
		 * PM_WAIT_BACKENDS 状态在不再有普通后端（含       
		 * autovac worker），没有 bgworker（包括未连接           
		 * 的），也没有 walwriter、autovac launcher 或 bgwriter。  
		 * 如果正在做崩溃恢复或立即关闭，那么预期       
		 * checkpointer 也退出，否则不退出。               
		 * stats 与 syslogger 进程不参与判断，因为它们未连接  
		 * 到共享内存；此处同样不计 dead_end 子进程。         
		 * walsender 与 archiver 也不计入，它们将在稍后        
		 * 写完检查点记录后终止。   
		 */
		if (CountChildren(BACKEND_TYPE_ALL - BACKEND_TYPE_WALSND) == 0 &&
			StartupPID == 0 &&
			WalReceiverPID == 0 &&
			BgWriterPID == 0 &&
			(CheckpointerPID == 0 ||
			 (!FatalError && Shutdown < ImmediateShutdown)) &&
			WalWriterPID == 0 &&
			AutoVacPID == 0)
		{
			if (Shutdown >= ImmediateShutdown || FatalError)
			{
				/*
				 * 开始等待 dead_end 子进程消亡。状态变更   
				 * 使 ServerLoop 不再创建新的此类进程。   
				 */
				pmState = PM_WAIT_DEAD_END;

				/*
				 * archiver 与 stats 进程（若有）早已被我们     
				 * SIGQUIT 过：当启动立即关闭或进入      
				 * FatalError 时   
				 */
			}
			else
			{
				/*
				 * 若走到这里，说明我们正在正常关闭。        
				 * 所有普通子进程都已消失，是时候通知     
				 * checkpointer 做关闭检查点了。    
				 */
				Assert(Shutdown > NoShutdown);
				/* 若未运行则启动 checkpointer    */
				if (CheckpointerPID == 0)
					CheckpointerPID = StartCheckpointer();
				/* 并让它关闭          */
				if (CheckpointerPID != 0)
				{
					signal_child(CheckpointerPID, SIGUSR2);
					pmState = PM_SHUTDOWN;
				}
				else
				{
					/*
					 * 若 fork checkpointer 失败，就直接关闭。   
					 * 所需的清理工作留待下次重启时进行。 
					 * 我们置上 FatalError，以便退出时记录      
					 * "abnormal shutdown"消息
					 *
					 * 此处不查 send_abort_for_crash，因为         
					 * 转储核心不太可能阐明 checkpointer fork       
					 * 失败的原因。            
					 */
					FatalError = true;
					pmState = PM_WAIT_DEAD_END;

					/* 同时杀掉 walsenders 与 archiver */
					SignalChildren(SIGQUIT);
					if (PgArchPID != 0)
						signal_child(PgArchPID, SIGQUIT);
				}
			}
		}
	}

	if (pmState == PM_SHUTDOWN_2)
	{
		/*
		 * 当除 dead_end 子进程外再无其他子进程时，     
		 * PM_SHUTDOWN_2 状态结束。此时不该再有普通后端    
		 * 残留；真正等待的是 walsenders 与 archiver。             
		 *          
		 */
		if (PgArchPID == 0 && CountChildren(BACKEND_TYPE_ALL) == 0)
		{
			pmState = PM_WAIT_DEAD_END;
		}
	}

	if (pmState == PM_WAIT_DEAD_END)
	{
		/* 不再允许新的套接字连接事件。    */
		ConfigurePostmasterWaitSet(false);

		/*
		 * BackendList 完全变空（即已无 dead_end 子进程），     
		 * 且 archiver 消失，PM_WAIT_DEAD_END 状态即告结束。    
		 *
		 * 之所以等待这两个进程，是为了防止新 postmaster   
		 * 启动冲突的子进程；这并非万无一失，        
		 * 但至少有助于关闭后立即重启场景。 
		 * 注意它们已被发送过合适的关闭信号，要么      
		 * 在通往 PM_WAIT_DEAD_END 的正常状态转换期间，      
		 * 要么是在 FatalError 处理期间。                          
		 *                       
		 */
		if (dlist_is_empty(&BackendList) && PgArchPID == 0)
		{
			/* 其余这些进程应该早已消亡    */
			Assert(StartupPID == 0);
			Assert(WalReceiverPID == 0);
			Assert(BgWriterPID == 0);
			Assert(CheckpointerPID == 0);
			Assert(WalWriterPID == 0);
			Assert(AutoVacPID == 0);
			/* 此处不把 syslogger 计入    */
			pmState = PM_NO_CHILDREN;
		}
	}

	/*
	 * 若收到关闭指示，一旦不再有剩余子进程，       
	 * 我们便退出。如果发生过崩溃，清理将留到           
	 * 下次启动时进行。（PostgreSQL 8.3 之前，我们尝试    
	 * 在退出前先恢复崩溃；但如果退出是因为收到 init     
	 * 的 SIGTERM 才退出，那就可能来不及在 init 决定        
	 * SIGKILL 之前完成恢复。）   
	 *
	 * 注意 syslogger 将继续运行。它在输入管道上读到      
	 * EOF 时退出；当再无任何上游进程时就会出现 EOF。   
	 *           
	 */
	if (Shutdown > NoShutdown && pmState == PM_NO_CHILDREN)
	{
		if (FatalError)
		{
			ereport(LOG, (errmsg("abnormal database system shutdown")));
			ExitPostmaster(1);
		}
		else
		{
			/*
			 * postmaster 的正常退出就在此处，无需在此记录   
			 * 任何日志，因为 UnlinkLockFiles proc_exit 回调     
			 * 会代为记录，这应是最后用户可见的动作。   
			 */
			ExitPostmaster(0);
		}
	}

	/*
	 * 如果 startup 进程失败，或者用户不希望在               
	 * 后端崩溃后自动重启，那么等待所有非 syslogger        
	 * 子进程退出后再退出 postmaster。当 startup 进程           
	 * 失败时我们不再尝试重新初始化，因为多半只会        
	 * 再次失败并永远重试。  
	 */
	if (pmState == PM_NO_CHILDREN)
	{
		if (StartupStatus == STARTUP_CRASHED)
		{
			ereport(LOG,
					(errmsg("shutting down due to startup process failure")));
			ExitPostmaster(1);
		}
		if (!restart_after_crash)
		{
			ereport(LOG,
					(errmsg("shutting down because restart_after_crash is off")));
			ExitPostmaster(1);
		}
	}

	/*
	 * 如果需从崩溃中恢复，则等待全部非 syslogger 子进程   
	 * 退出；再重置 shmem 与 StartupDataBase  
	 */
	if (FatalError && pmState == PM_NO_CHILDREN)
	{
		ereport(LOG,
				(errmsg("all server processes terminated; reinitializing")));

		/* 崩溃后清除遗留的临时文件          */
		if (remove_temp_files_after_crash)
			RemovePgTempFiles();

		/* 允许后台 worker 立即重启                */
		ResetBackgroundWorkerCrashTimes();

		shmem_exit(1);

		/* 将控制文件重读进本地内存   */
		LocalProcessControlFile(true);

		/* 重建共享内存与信号量         */
		CreateSharedMemoryAndSemaphores();

		StartupPID = StartupDataBase();
		Assert(StartupPID != 0);
		StartupStatus = STARTUP_RUNNING;
		pmState = PM_STARTUP;
		/* 崩溃恢复开始，重置 SIGKILL 标志 */
		AbortStartTime = 0;

		/* 重新开始接受服务器套接字连接事件      */
		ConfigurePostmasterWaitSet(true);
	}
}


/*
 * 向 postmaster 子进程发送信号       
 *
 * 在拥有 setsid() 的系统上，每个子进程都会把自己     
 * 设为进程组组长。对于通常会被恰当解释的信号，      
 * 我们不只通知直接子进程，而是通知整个进程组。  
 * 例如我们可以 SIGQUIT 掉被阻塞的 archive_recovery             
 * 脚本；或 SIGINT 掉后端经 system() 运行的                   
 * 脚本   
 *
 * 对于新近 fork 出的子进程存在竞态问题：它们可能    
 * 尚未执行过 setsid()。所以我们既直接通知子进程，     
 * 也通知进程组。我们假定这类子进程在试图派生        
 * 任何孙进程之前就会处理好该信号。我们还假定，   
 * 通知子进程两次不会有问题。 
 */
static void
signal_child(pid_t pid, int signal)
{
	if (kill(pid, signal) < 0)
		elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) pid, signal);
#ifdef HAVE_SETSID
	switch (signal)
	{
		case SIGINT:
		case SIGTERM:
		case SIGQUIT:
		case SIGKILL:
		case SIGABRT:
			if (kill(-pid, signal) < 0)
				elog(DEBUG3, "kill(%ld,%d) failed: %m", (long) (-pid), signal);
			break;
		default:
			break;
	}
#endif
}

/*
 * 用于在某子进程崩溃后杀掉另一子进程的便利函数。 
 * 我们以比平常更高的级别记录这一动作，并且用        
 * send_abort_for_crash 来决定发送哪种信号。通常是          
 * SIGQUIT——本文件多数其他注释都基于该假设——       
 * 但开发者可能更愿意用 SIGABRT 来收集每子进程的        
 * 核心转储。                                
 */
static void
sigquit_child(pid_t pid)
{
	ereport(DEBUG2,
			(errmsg_internal("sending %s to process %d",
							 (send_abort_for_crash ? "SIGABRT" : "SIGQUIT"),
							 (int) pid)));
	signal_child(pid, (send_abort_for_crash ? SIGABRT : SIGQUIT));
}

/*
 * 向目标子进程发送信号（但不包括特殊子进程；  
 * dead_end 子进程也从不会被通知）。 
 */
static bool
SignalSomeChildren(int signal, int target)
{
	dlist_iter	iter;
	bool		signaled = false;

	dlist_foreach(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->dead_end)
			continue;

		/*
		 * 由于 target == BACKEND_TYPE_ALL 是最常见情形，          
		 * 我们先测试它，避免逐个触碰共享内存。    
		 */
		if (target != BACKEND_TYPE_ALL)
		{
			/*
			 * 为新报到的 WAL Sender 进程指定 bkend_type     
			 *           
			 */
			if (bp->bkend_type == BACKEND_TYPE_NORMAL &&
				IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = BACKEND_TYPE_WALSND;

			if (!(target & bp->bkend_type))
				continue;
		}

		ereport(DEBUG4,
				(errmsg_internal("sending signal %d to process %d",
								 signal, (int) bp->pid)));
		signal_child(bp->pid, signal);
		signaled = true;
	}
	return signaled;
}

/*
 * 向子进程发送终止信号。这会考虑我们的所有子进程，  
 * syslogger 和 dead_end 后端除外。            
 */
static void
TerminateChildren(int signal)
{
	SignalChildren(signal);
	if (StartupPID != 0)
	{
		signal_child(StartupPID, signal);
		if (signal == SIGQUIT || signal == SIGKILL || signal == SIGABRT)
			StartupStatus = STARTUP_SIGNALED;
	}
	if (BgWriterPID != 0)
		signal_child(BgWriterPID, signal);
	if (CheckpointerPID != 0)
		signal_child(CheckpointerPID, signal);
	if (WalWriterPID != 0)
		signal_child(WalWriterPID, signal);
	if (WalReceiverPID != 0)
		signal_child(WalReceiverPID, signal);
	if (AutoVacPID != 0)
		signal_child(AutoVacPID, signal);
	if (PgArchPID != 0)
		signal_child(PgArchPID, signal);
}

/*
 * BackendStartup——启动后端进程 
 *
 * 返回：fork 失败则 STATUS_ERROR，否则 STATUS_OK。    
 *
 * 注意：改动此代码时也请考虑 StartAutovacuumWorker      
 */
static int
BackendStartup(Port *port)
{
	Backend    *bn;				/* 用于后端清理  */
	pid_t		pid;

	/*
	 * 创建后端数据结构。最好在 fork() 之前完成，以便 
	 * 干净地处理失败  
	 */
	bn = (Backend *) malloc(sizeof(Backend));
	if (!bn)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return STATUS_ERROR;
	}

	/*
	 * 计算将分配给此后端的取消密钥。                    
	 * 后端在 fork 出的进程里的 MyCancelKey 值中持有        
	 * 自己的副本，从而能将密钥传给前端。          
	 */
	if (!RandomCancelKey(&MyCancelKey))
	{
		free(bn);
		ereport(LOG,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random cancel key")));
		return STATUS_ERROR;
	}

	bn->cancel_key = MyCancelKey;

	/* 传递 canAcceptConnections 状态   */
	port->canAcceptConnections = canAcceptConnections(BACKEND_TYPE_NORMAL);
	bn->dead_end = (port->canAcceptConnections != CAC_OK);

	/*
	 * 除非是 dead_end 子进程，否则给它分配子槽号 
	 */
	if (!bn->dead_end)
		bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
	else
		bn->child_slot = 0;

	/* 尚未请求获得任何 bgworker 通知            */
	bn->bgworker_notify = false;

#ifdef EXEC_BACKEND
	pid = backend_forkexec(port);
#else							/* !EXEC_BACKEND */
	pid = fork_process();
	if (pid == 0)				/* child */
	{
		free(bn);

		/* 脱离 postmaster        */
		InitPostmasterChild();

		/* 关闭 postmaster 的套接字 */
		ClosePostmasterPorts(false);

		/* 执行额外初始化并收集启动包                      */
		BackendInitialize(port);

		/*
		 * 在共享内存中为每个后端创建 PGPROC 结构，必须 
		 * 先完成这一步才能使用 LWLock。在 !EXEC_BACKEND       
		 * 情形（此处）中本可再推迟些，但 EXEC_BACKEND       
		 * 要在 PostgresMain() 前用 LWLock，为对称起见也          
		 * 在此执行 
		 */
		InitProcess();

		/* 然后运行后端  */
		BackendRun(port);
	}
#endif							/* EXEC_BACKEND */

	if (pid < 0)
	{
		/* 父进程 fork 失败  */
		int			save_errno = errno;

		if (!bn->dead_end)
			(void) ReleasePostmasterChildSlot(bn->child_slot);
		free(bn);
		errno = save_errno;
		ereport(LOG,
				(errmsg("could not fork new process for connection: %m")));
		report_fork_failure_to_client(port, save_errno);
		return STATUS_ERROR;
	}

	/* 父进程中，fork 成功 */
	ereport(DEBUG2,
			(errmsg_internal("forked new backend, pid=%d socket=%d",
							 (int) pid, (int) port->sock)));

	/*
	 * 一切顺利，可放心将此后端加入我们的后端列表。     
	 *             
	 */
	bn->pid = pid;
	bn->bkend_type = BACKEND_TYPE_NORMAL;	/* 以后可改为 WALSND     */
	dlist_push_head(&BackendList, &bn->elem);

#ifdef EXEC_BACKEND
	if (!bn->dead_end)
		ShmemBackendArrayAdd(bn);
#endif

	return STATUS_OK;
}

/*
 * 关闭连接前，尽量把后端 fork() 失败报给客户端。 
 * 由于不愿冒让 postmaster 阻塞在此连接上的风险，     
 * 我们把连接设为非阻塞模式，并且只尝试一次。          
 *
 * 这是凑合的专用代码；后端 libpq 尚未就绪运行，      
 * 故无法使用它。   
 */
static void
report_fork_failure_to_client(Port *port, int errnum)
{
	char		buffer[1000];
	int			rc;

	/* 格式化错误消息包（总是 V2 协议）       */
	snprintf(buffer, sizeof(buffer), "E%s%s\n",
			 _("could not fork new process for connection: "),
			 strerror(errnum));

	/* 把 port 设为非阻塞。若失败则不做 send()     */
	if (!pg_set_noblock(port->sock))
		return;

	/* 遇到 EINTR 会重试，但忽略其他所有失败   */
	do
	{
		rc = send(port->sock, buffer, strlen(buffer) + 1, 0);
	} while (rc < 0 && errno == EINTR);
}


/*
 * BackendInitialize——初始化交互式（postmaster 子       
 *				进程）后端，并收集客户端的启动包。      
 *
 * 返回：无。若出现任何失败则完全不返回。        
 *
 * 注意：此代码不依赖于对共享内存的任何访问。       
 * 实际上，我们对 SIGTERM/超时的处理方式*要求*     
 * 共享内存尚未被触碰过；详见其中的注释。    
 * 在 EXEC_BACKEND 情形下，我们在物理上已挂接到          
 * 共享内存，但多数指向 shmem 结构的本地指针尚未建立 
 */
static void
BackendInitialize(Port *port)
{
	int			status;
	int			ret;
	char		remote_host[NI_MAXHOST];
	char		remote_port[NI_MAXSERV];
	StringInfoData ps_data;

	/* 为 ps 状态保存 port 等 */
	MyProcPort = port;

	/* 把与 port 关联的长期存活 FD 告知 fd.c           */
	ReserveExternalFD();

	/*
	 * PreAuthDelay 是排查认证环节问题的调试辅助：       
	 * 可以在 postgresql.conf 里设置它，以便有时间用调试器   
	 * 附加到新 fork 的后端上。（另见 PostAuthDelay，   
	 * 我们允许客户端经 PGOPTIONS 传递它，但要到认证之后   
	 * 才会生效。）                         
	 */
	if (PreAuthDelay > 0)
		pg_usleep(PreAuthDelay * 1000000L);

	/* 此标志将保持置位，直到 InitPostgres 完成认证          */
	ClientAuthInProgress = true;	/* 限制日志消息的可见性   */

	/* 把它们置空，以防在正式设置前就需要用到        */
	port->remote_host = "";
	port->remote_port = "";

	/*
	 * 初始化 libpq 并启用向客户端报告 ereport 错误。         
	 * 必须现在就做，因为认证要使用 libpq 发送消息。    
	 */
	pq_init();					/* 初始化 libpq 与客户端通信 */
	whereToSendOutput = DestRemote; /* 现在才能安全 ereport    */

	/*
	 * 若收集启动包时收到 SIGTERM 或超时，则安排 _exit(1)；  
	 * 而 SIGQUIT 导致 _exit(2)。                                   
	 * 否则若有缺陷客户端未及时发包，postmaster 无法     
	 * 干净地以 FAST 或 IMMED 方式关闭数据库。        
	 *
	 * 能用 _exit(1) 退出，是因为尚未触及共享内存；         
	 * 因此进程外没有任何状态需要清理。                  
	 *            
	 */
	pqsignal(SIGTERM, process_startup_packet_die);
	/* SIGQUIT 处理器已由 InitPostmasterChild 设置        */
	InitializeTimeouts();		/* 建立 SIGALRM 处理器    */
	sigprocmask(SIG_SETMASK, &StartupBlockSig, NULL);

	/*
	 * 获取远程主机名和端口，用于日志与状态显示。  
	 */
	remote_host[0] = '\0';
	remote_port[0] = '\0';
	if ((ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								  remote_host, sizeof(remote_host),
								  remote_port, sizeof(remote_port),
								  (log_hostname ? 0 : NI_NUMERICHOST) | NI_NUMERICSERV)) != 0)
		ereport(WARNING,
				(errmsg_internal("pg_getnameinfo_all() failed: %s",
								 gai_strerror(ret))));

	/*
	 * 把 remote_host/remote_port 保存进 port 结构（此后它们    
	 * 将出现在 log_line_prefix 日志数据里）。    
	 */
	port->remote_host = strdup(remote_host);
	port->remote_port = strdup(remote_port);

	/* 现在可以按需发出 Log_connections 消息了          */
	if (Log_connections)
	{
		if (remote_port[0])
			ereport(LOG,
					(errmsg("connection received: host=%s port=%s",
							remote_host,
							remote_port)));
		else
			ereport(LOG,
					(errmsg("connection received: host=%s",
							remote_host)));
	}

	/*
	 * 若做过反向查找得出名字，不妨把结果保存下来，   
	 * 免得认证期间可能重复查找。                         
	 *
	 * 注意我们不想在前面指定 NI_NAMEREQD，否则                 
	 * 对没有 rDNS 记录的客户端将一无所获。因此我们      
	 * 必须检查拿到的是数字 IPv4 还是 IPv6 地址，若是则    
	 * 把它存入 remote_hostname。（该测试偏保守，可能       
	 * 有时会把主机名误判为数字，但那方面的        
	 * 出错也无害；至多导致一次额外查找。）         
	 */
	if (log_hostname &&
		ret == 0 &&
		strspn(remote_host, "0123456789.") < strlen(remote_host) &&
		strspn(remote_host, "0123456789ABCDEFabcdef:") < strlen(remote_host))
		port->remote_hostname = strdup(remote_host);

	/*
	 * 准备开始客户端交互。超过时限即放弃并 _exit(1)，    
	 * 以免出故障的客户端长期霸占一个连接。      
	 * 前面的 PreAuthDelay 和 DNS 交互不计入此时限。            
	 *                        
	 *
	 * 注意：在等待启动包时应用 AuthenticationTimeout，    
	 * 随后在 InitPostgres 中再次应用，贯穿认证全程。        
	 * 这样恶意客户端几乎能把进程占用近两倍          
	 * 的 AuthenticationTimeout，然后我们才会踢掉它。            
	 *
	 * 注意：因 PostgresMain 再次调用 InitializeTimeouts，      
	 * STARTUP_PACKET_TIMEOUT 的注册将丢失。这没有问题，    
	 * 因为本函数之后不再用它。            
	 */
	RegisterTimeout(STARTUP_PACKET_TIMEOUT, StartupPacketTimeoutHandler);
	enable_timeout_after(STARTUP_PACKET_TIMEOUT, AuthenticationTimeout * 1000);

	/*
	 * 接收启动包（它也可能是取消请求包）。                 
	 *         
	 */
	status = ProcessStartupPacket(port, false, false);

	/*
	 * 禁用超时，并阻止再次的 SIGTERM。   
	 */
	disable_timeout(STARTUP_PACKET_TIMEOUT, false);
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);

	/*
	 * 作为安全检查：确认启动阶段尚未做过        
	 * 需撤销的共享内存修改（若上文经 SIGTERM 或超时   
	 * 退出），要检查尚未注册任何 on_shmem_exit 处理器。  
	 * （这并非十分可靠，因为有人可能误用 on_proc_exit     
	 * 处理器来做共享内存清理，但该检查廉价而有用。   
	 * 遗憾的是我们无法禁止 on_proc_exit 处理器，            
	 * 因为 pq_init() 已经注册了一个。）                    
	 */
	check_on_shmem_exit_lists_are_empty();

	/*
	 * 若是坏包或取消包就在此停止。ProcessStartupPacket   
	 * 已做过相应的错误报告。           
	 */
	if (status != STATUS_OK)
		proc_exit(0);

	/*
	 * 既然已拿到用户名和数据库名，就可以设置          
	 * ps 的进程标题了。启动阶段宜尽早完成此事。        
	 */
	initStringInfo(&ps_data);
	if (am_walsender)
		appendStringInfo(&ps_data, "%s ", GetBackendTypeDesc(B_WAL_SENDER));
	appendStringInfo(&ps_data, "%s ", port->user_name);
	if (port->database_name[0] != '\0')
		appendStringInfo(&ps_data, "%s ", port->database_name);
	appendStringInfoString(&ps_data, port->remote_host);
	if (port->remote_port[0] != '\0')
		appendStringInfo(&ps_data, "(%s)", port->remote_port);

	init_ps_display(ps_data.data);
	pfree(ps_data.data);

	set_ps_display("initializing");
}


/*
 * BackendRun——构造后端参数列表并调用 PostgresMain()          
 *
 * 返回: 
 *		完全不返回。    
 */
static void
BackendRun(Port *port)
{
	/*
	 * 确保我们已不再处于 PostmasterContext 中。（不过还不能
	 * 删除它，因为 InitPostgres 还需要 HBA 数据。）     
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	PostgresMain(port->database_name, port->user_name);
}


#ifdef EXEC_BACKEND

/*
 * postmaster_forkexec——fork/exec 出 postmaster 子进程 
 *
 * 调用者必须已构造好 argv 数组，只有 argv[2] 除外，    
 * 该位置将填入临时变量文件名。                   
 *
 * 返回子进程 PID，fork 失败则返回 -1（失败时已记录    
 * 相应的错误消息）。         
 *
 * 本例程的调用都会在子进程中转入 SubPostmasterMain   
 *               
 */
pid_t
postmaster_forkexec(int argc, char *argv[])
{
	Port		port;

	/* 此入口点为 Port 变量传入哑值                     */
	memset(&port, 0, sizeof(port));
	return internal_forkexec(argc, argv, &port);
}

/*
 * backend_forkexec——fork/exec 一个后端进程 
 *
 * 某些操作系统（WIN32）没有 fork()，因此我们必须模拟  
 * 它：把需传给子进程的参数先存放起来，          
 * 然后创建新的子进程。  
 *
 * 返回 fork/exec 出的进程的 pid，失败则返回 -1   
 */
static pid_t
backend_forkexec(Port *port)
{
	char	   *av[4];
	int			ac = 0;

	av[ac++] = "postgres";
	av[ac++] = "--forkbackend";
	av[ac++] = NULL;			/* 由 internal_forkexec 填写   */

	av[ac] = NULL;
	Assert(ac < lengthof(av));

	return internal_forkexec(ac, av, port);
}

#ifndef WIN32

/*
 * internal_forkexec 的非 win32 实现     
 *
 * - 把后端变量写出到参数文件              
 * - 先 fork() 再 exec() 子进程              
 */
static pid_t
internal_forkexec(int argc, char *argv[], Port *port)
{
	static unsigned long tmpBackendFileNum = 0;
	pid_t		pid;
	char		tmpfilename[MAXPGPATH];
	BackendParameters param;
	FILE	   *fp;

	if (!save_backend_variables(&param, port))
		return -1;				/* save_backend_variables 所记日志*/

	/* 计算临时文件名        */
	snprintf(tmpfilename, MAXPGPATH, "%s/%s.backend_var.%d.%lu",
			 PG_TEMP_FILES_DIR, PG_TEMP_FILE_PREFIX,
			 MyProcPid, ++tmpBackendFileNum);

	/* 开文件 */
	fp = AllocateFile(tmpfilename, PG_BINARY_W);
	if (!fp)
	{
		/*
		 * 与 OpenTemporaryFileInTablespace 一样，试着创建临时 
		 * 目录，忽略错误。   
		 */
		(void) MakePGDirectory(PG_TEMP_FILES_DIR);

		fp = AllocateFile(tmpfilename, PG_BINARY_W);
		if (!fp)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m",
							tmpfilename)));
			return -1;
		}
	}

	if (fwrite(&param, sizeof(param), 1, fp) != 1)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmpfilename)));
		FreeFile(fp);
		return -1;
	}

	/* 释放文件 */
	if (FreeFile(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmpfilename)));
		return -1;
	}

	/* 确保调用者已正确设置 argv   */
	Assert(argc >= 3);
	Assert(argv[argc] == NULL);
	Assert(strncmp(argv[1], "--fork", 6) == 0);
	Assert(argv[2] == NULL);

	/* 把临时文件名插到 --fork 参数之后*/
	argv[2] = tmpfilename;

	/* 子进程执行 execv   */
	if ((pid = fork_process()) == 0)
	{
		if (execv(postgres_exec_path, argv) < 0)
		{
			ereport(LOG,
					(errmsg("could not execute server process \"%s\": %m",
							postgres_exec_path)));
			/* 此处已位于子进程中，不能返回            */
			exit(1);
		}
	}

	return pid;					/* 父进程返回 pid，失败则返回 -1  */
}
#else							/* WIN32 */

/*
 * internal_forkexec win32 实现        
 *
 * - 以挂起状态用 CreateProcess() 启动后端         
 * - 把后端变量写出到参数文件              
 *	- 期间复制新进程需继承的                       
 *	  句柄与套接字              
 * - 待后端参数文件写全后即恢复                        
 *	 新进程执行  
 */
static pid_t
internal_forkexec(int argc, char *argv[], Port *port)
{
	int			retry_count = 0;
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	int			i;
	int			j;
	char		cmdLine[MAXPGPATH * 2];
	HANDLE		paramHandle;
	BackendParameters *param;
	SECURITY_ATTRIBUTES sa;
	char		paramHandleStr[32];
	win32_deadchild_waitinfo *childinfo;

	/* 确保调用者已正确设置 argv   */
	Assert(argc >= 3);
	Assert(argv[argc] == NULL);
	Assert(strncmp(argv[1], "--fork", 6) == 0);
	Assert(argv[2] == NULL);

	/* 若需重试，从此处恢复  */
retry:

	/* 设置用于传参的共享内存          */
	ZeroMemory(&sa, sizeof(sa));
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	paramHandle = CreateFileMapping(INVALID_HANDLE_VALUE,
									&sa,
									PAGE_READWRITE,
									0,
									sizeof(BackendParameters),
									NULL);
	if (paramHandle == INVALID_HANDLE_VALUE)
	{
		ereport(LOG,
				(errmsg("could not create backend parameter file mapping: error code %lu",
						GetLastError())));
		return -1;
	}

	param = MapViewOfFile(paramHandle, FILE_MAP_WRITE, 0, 0, sizeof(BackendParameters));
	if (!param)
	{
		ereport(LOG,
				(errmsg("could not map backend parameter memory: error code %lu",
						GetLastError())));
		CloseHandle(paramHandle);
		return -1;
	}

	/* 把临时文件名插到 --fork 参数之后*/
#ifdef _WIN64
	sprintf(paramHandleStr, "%llu", (LONG_PTR) paramHandle);
#else
	sprintf(paramHandleStr, "%lu", (DWORD) paramHandle);
#endif
	argv[2] = paramHandleStr;

	/* 格式化命令行  */
	cmdLine[sizeof(cmdLine) - 1] = '\0';
	cmdLine[sizeof(cmdLine) - 2] = '\0';
	snprintf(cmdLine, sizeof(cmdLine) - 1, "\"%s\"", postgres_exec_path);
	i = 0;
	while (argv[++i] != NULL)
	{
		j = strlen(cmdLine);
		snprintf(cmdLine + j, sizeof(cmdLine) - 1 - j, " \"%s\"", argv[i]);
	}
	if (cmdLine[sizeof(cmdLine) - 2] != '\0')
	{
		ereport(LOG,
				(errmsg("subprocess command line too long")));
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;
	}

	memset(&pi, 0, sizeof(pi));
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);

	/*
	 * 以挂起状态创建子进程。稍后再恢复其运行，           
	 * 待写完参数文件之后。              
	 */
	if (!CreateProcess(NULL, cmdLine, NULL, NULL, TRUE, CREATE_SUSPENDED,
					   NULL, NULL, &si, &pi))
	{
		ereport(LOG,
				(errmsg("CreateProcess() call failed: %m (error code %lu)",
						GetLastError())));
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;
	}

	if (!save_backend_variables(param, port, pi.hProcess, pi.dwProcessId))
	{
		/*
		 * 日志由 save_backend_variables 记录，但我们必须      
		 * 清理半启动的进程。       
		 */
		if (!TerminateProcess(pi.hProcess, 255))
			ereport(LOG,
					(errmsg_internal("could not terminate unstarted process: error code %lu",
									 GetLastError())));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		UnmapViewOfFile(param);
		CloseHandle(paramHandle);
		return -1;				/* save_backend_variables 所记日志*/
	}

	/* 释放现已由后端继承的参数共享内存                      */
	if (!UnmapViewOfFile(param))
		ereport(LOG,
				(errmsg("could not unmap view of backend parameter file: error code %lu",
						GetLastError())));
	if (!CloseHandle(paramHandle))
		ereport(LOG,
				(errmsg("could not close handle to backend parameter file: error code %lu",
						GetLastError())));

	/*
	 * 在恢复子进程之前，预留主共享内存段所用的           
	 * 内存区域。通常应当成功，但若 ASLR 激活，              
	 * 则有时会因栈或堆已映射进该区域而失败。             
	 * 那种情况下，直接终止进程并重试。                
	 *                   
	 */
	if (!pgwin32_ReserveSharedMemoryRegion(pi.hProcess))
	{
		/* pgwin32_ReserveSharedMemoryRegion 已记过日志          */
		if (!TerminateProcess(pi.hProcess, 255))
			ereport(LOG,
					(errmsg_internal("could not terminate process that failed to reserve memory: error code %lu",
									 GetLastError())));
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		if (++retry_count < 100)
			goto retry;
		ereport(LOG,
				(errmsg("giving up after too many tries to reserve shared memory"),
				 errhint("This might be caused by ASLR or antivirus software.")));
		return -1;
	}

	/*
	 * 后端变量写出之后，我们即启动子线程，            
	 * 使其能在我们设置其余父进程状态时开始初始化。 
	 *              
	 */
	if (ResumeThread(pi.hThread) == -1)
	{
		if (!TerminateProcess(pi.hProcess, 255))
		{
			ereport(LOG,
					(errmsg_internal("could not terminate unstartable process: error code %lu",
									 GetLastError())));
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return -1;
		}
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		ereport(LOG,
				(errmsg_internal("could not resume thread of unstarted process: error code %lu",
								 GetLastError())));
		return -1;
	}

	/*
	 * 排入一个等待器，在该子进程消亡时发出信号。        
	 * 该等待将交由操作系统线程池自动处理。其内存      
	 * 稍后由 waitpid() 释放。      
	 */
	childinfo = palloc(sizeof(win32_deadchild_waitinfo));
	childinfo->procHandle = pi.hProcess;
	childinfo->procId = pi.dwProcessId;

	if (!RegisterWaitForSingleObject(&childinfo->waitHandle,
									 pi.hProcess,
									 pgwin32_deadchild_callback,
									 childinfo,
									 INFINITE,
									 WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD))
		ereport(FATAL,
				(errmsg_internal("could not register process for wait: error code %lu",
								 GetLastError())));

	/* 此处别关闭 pi.hProcess，waitpid() 还要用它        */

	CloseHandle(pi.hThread);

	return pi.dwProcessId;
}
#endif							/* WIN32 */


/*
 * SubPostmasterMain——让 fork/exec 出的进程达到                 
 *			与 Unix 上直接 fork 相当的状态，然后           
 *			再转入相应入口。          
 *
 * 前两个命令行参数应为 "--forkFOO"                         
 * （FOO 指明我们要成为哪种 postmaster 子进程），     
 * 以及一个变量文件名，我们读取它即可加载在         
 * Unix 上本可由 fork() 继承的数据。其余参数交给        
 * 子进程 FooMain() 例程。
 */
void
SubPostmasterMain(int argc, char *argv[])
{
	Port		port;

	/* EXEC_BACKEND 情形下我们不会继承这些设置           */
	IsPostmasterEnvironment = true;
	whereToSendOutput = DestNone;

	/* 初始化基本子系统（确保 elog() 行为正常）     */
	InitializeGUCOptions();

	/* 检查参数是否合适      */
	if (argc < 3)
		elog(FATAL, "invalid subpostmaster invocation");

	/* 读入变量文件         */
	memset(&port, 0, sizeof(Port));
	read_backend_variables(argv[2], &port);

	/* 关闭 postmaster 的套接字（一旦知道就关）   */
	ClosePostmasterPorts(strcmp(argv[1], "--forklog") == 0);

	/* postmaster 子进程设置*/
	InitPostmasterChild();

	/*
	 * 如适用，物理性地重新挂接共享内存段。我们要       
	 * 在做任何后续工作之前完成此事，以确保能在           
	 * postmaster 所用的同一地址挂接。反之若选择不           
	 * 重新挂接，可能还需别的清理。    
	 *
	 * 若在 Linux 上测试 EXEC_BACKEND，启动 postmaster 前         
	 * 先以 root 运行：   
	 *
	 * sysctl -w kernel.randomize_va_space=0
	 *
	 * 这可避免使用随机化的栈与代码地址——它们会使    
	 * 子进程的内存映射与父进程不同，从而有时              
	 * 无法在期望地址挂接共享内存。                             
	 * 结束后把该设置改回原值（通常为 '1' 或 '2'）。        
	 */
	if (strcmp(argv[1], "--forkbackend") == 0 ||
		strcmp(argv[1], "--forkavlauncher") == 0 ||
		strcmp(argv[1], "--forkavworker") == 0 ||
		strcmp(argv[1], "--forkaux") == 0 ||
		strncmp(argv[1], "--forkbgworker=", 15) == 0)
		PGSharedMemoryReAttach();
	else
		PGSharedMemoryNoReAttach();

	/* autovacuum 要求在 InitProcess 之前设好此项  */
	if (strcmp(argv[1], "--forkavlauncher") == 0)
		AutovacuumLauncherIAm();
	if (strcmp(argv[1], "--forkavworker") == 0)
		AutovacuumWorkerIAm();

	/* 读入其余 GUC 变量         */
	read_nondefault_variables();

	/*
	 * 检查数据目录是否有效，这同时会检查                 
	 * 数据目录的权限，并更新我们的 umask 与文件/组      
	 * 变量，供日后创建文件之用。注意：此事确实应      
	 * 在创建任何文件或目录之前。   
	 */
	checkDataDir();

	/*
	 * （重新）读取控制文件，因为其中含有配置。      
	 * postmaster 已读过它，但本进程并不知情。            
	 */
	LocalProcessControlFile(false);

	/*
	 * 重新加载 postmaster 预加载的所有库。由于我们         
	 * 是 exec 出来的，那些库并未随之而来；但我们          
	 * 仍应加载进所有子进程，保持与非 EXEC_BACKEND         
	 * 情形一致。           
	 */
	process_shared_preload_libraries();

	/* 运行后端或相应子进程   */
	if (strcmp(argv[1], "--forkbackend") == 0)
	{
		Assert(argc == 3);		/* 不应再有任何参数   */

		/*
		 * 需要在后端中重新初始化 SSL 库，因为            
		 * 上下文结构含有函数指针，无法通过参数           
		 * 文件传递。            
		 *
		 * 若因某种原因重载失败（例如用户装了损坏的    
		 * 密钥文件），那就无 SSL 硬撑；总比全部     
		 * 连接都无法建立要好。  
		 *
		 * XXX 是否应在所有子进程中都这样做？目前只在     
		 * 后端子进程中做即可。      
		 */
#ifdef USE_SSL
		if (EnableSSL)
		{
			if (secure_initialize(false) == 0)
				LoadedSSL = true;
			else
				ereport(LOG,
						(errmsg("SSL configuration could not be loaded in child process")));
		}
#endif

		/*
		 * 执行额外初始化并收集启动包。                   
		 *
		 * 我们希望在 InitProcess() 之前做这件事，原因有二： 
		 * 1. 等待客户端期间不占用 PGPROC 槽位；2. 若      
		 * InitProcess() 因 PGPROC 槽位耗尽而失败，我们也   
		 * 已经初始化好了 libpq，能够向客户端               
		 * 报告该错误。             
		 */
		BackendInitialize(&port);

		/* 恢复基本共享内存指针       */
		InitShmemAccess(UsedShmemSegAddr);

		/* 运行 CreateSharedMemoryAndSemaphores 需要 PGPROC */
		InitProcess();

		/* 把进程挂接到共享数据结构     */
		CreateSharedMemoryAndSemaphores();

		/* 然后运行后端  */
		BackendRun(&port);		/* 不会返回    */
	}
	if (strcmp(argv[1], "--forkaux") == 0)
	{
		AuxProcType auxtype;

		Assert(argc == 4);

		/* 恢复基本共享内存指针       */
		InitShmemAccess(UsedShmemSegAddr);

		/* 运行 CreateSharedMemoryAndSemaphores 需要 PGPROC */
		InitAuxiliaryProcess();

		/* 把进程挂接到共享数据结构     */
		CreateSharedMemoryAndSemaphores();

		auxtype = atoi(argv[3]);
		AuxiliaryProcessMain(auxtype);	/* 不会返回    */
	}
	if (strcmp(argv[1], "--forkavlauncher") == 0)
	{
		/* 恢复基本共享内存指针       */
		InitShmemAccess(UsedShmemSegAddr);

		/* 运行 CreateSharedMemoryAndSemaphores 需要 PGPROC */
		InitProcess();

		/* 把进程挂接到共享数据结构     */
		CreateSharedMemoryAndSemaphores();

		AutoVacLauncherMain(argc - 2, argv + 2);	/* 不会返回    */
	}
	if (strcmp(argv[1], "--forkavworker") == 0)
	{
		/* 恢复基本共享内存指针       */
		InitShmemAccess(UsedShmemSegAddr);

		/* 运行 CreateSharedMemoryAndSemaphores 需要 PGPROC */
		InitProcess();

		/* 把进程挂接到共享数据结构     */
		CreateSharedMemoryAndSemaphores();

		AutoVacWorkerMain(argc - 2, argv + 2);	/* 不会返回    */
	}
	if (strncmp(argv[1], "--forkbgworker=", 15) == 0)
	{
		int			shmem_slot;

		/* 尽早做这件事；尤其是在 InitProcess() 之前            */
		IsBackgroundWorker = true;

		/* 恢复基本共享内存指针       */
		InitShmemAccess(UsedShmemSegAddr);

		/* 运行 CreateSharedMemoryAndSemaphores 需要 PGPROC */
		InitProcess();

		/* 把进程挂接到共享数据结构     */
		CreateSharedMemoryAndSemaphores();

		/* 从共享内存取 MyBgworkerEntry       */
		shmem_slot = atoi(argv[1] + 15);
		MyBgworkerEntry = BackgroundWorkerEntry(shmem_slot);

		StartBackgroundWorker();
	}
	if (strcmp(argv[1], "--forklog") == 0)
	{
		/* 不希望挂接共享内存            */

		SysLoggerMain(argc, argv);	/* 不会返回    */
	}

	abort();					/* 不应到达此处 */
}
#endif							/* EXEC_BACKEND */


/*
 * ExitPostmaster 清理    
 *
 * 切勿直接调用 exit() —— 一律经由此处！ 
 */
static void
ExitPostmaster(int status)
{
#ifdef HAVE_PTHREAD_IS_THREADED_NP

	/*
	 * 尚无已知原因会使 postmaster 在启动后变成多线程。    
	 * 不过我们可能经由错误退出，在到达 PostmasterMain
	 * 中的测试前走到这里，故给出与那里相同的提示。     
	 * 此消息使用 LOG 级别，因为此时若非正常关闭，        
	 * 通常与正常关闭看起来没有太大差别。         
	 */
	if (pthread_is_threaded_np() != 0)
		ereport(LOG,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("postmaster became multithreaded"),
				 errhint("Set the LC_ALL environment variable to a valid locale.")));
#endif

	/* 应当清理共享内存并终止所有后端      */

	/*
	 * 此处语义尚不明确。当 postmaster 死亡时，是否         
	 * 应终止所有后端？不必。    
	 *
	 * 必须-- vadim 05-10-1999
	 */

	proc_exit(status);
}

/*
 * 处理代表后端请求的 pmsignal 条件，                 
 * 并检查来自 pg_ctl 的提升与日志轮转请求。 
 */
static void
process_pm_pmsignal(void)
{
	pending_pm_pmsignal = false;

	ereport(DEBUG2,
			(errmsg_internal("postmaster received pmsignal signal")));

	/*
	 * 在意外状态下，RECOVERY_STARTED 与                    
	 * BEGIN_HOT_STANDBY 信号会被忽略。若 startup 进程             
	 * 快速启动、完成恢复后退出，则可能先处理 startup 
	 * 进程死亡。此时我们不想回到恢复状态。   
	 */
	if (CheckPostmasterSignal(PMSIGNAL_RECOVERY_STARTED) &&
		pmState == PM_STARTUP && Shutdown == NoShutdown)
	{
		/* WAL 重做已开始，已脱离重新初始化。    */
		FatalError = false;
		AbortStartTime = 0;

		/*
		 * 如果我们负责（重新）归档已接收的文件，则启动 
		 *       
		 */
		Assert(PgArchPID == 0);
		if (XLogArchivingAlways())
			PgArchPID = StartArchiver();

		/*
		 * 如果我们不打算稍后进入热备模式，则将      
		 * RECOVERY_STARTED 视为已脱离启动，并据此报告           
		 * 状态。   
		 */
		if (!EnableHotStandby)
		{
			AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_STANDBY);
#ifdef USE_SYSTEMD
			sd_notify(0, "READY=1");
#endif
		}

		pmState = PM_RECOVERY;
	}

	if (CheckPostmasterSignal(PMSIGNAL_BEGIN_HOT_STANDBY) &&
		pmState == PM_RECOVERY && Shutdown == NoShutdown)
	{
		ereport(LOG,
				(errmsg("database system is ready to accept read-only connections")));

		/* 报告状态  */
		AddToDataDirLockFile(LOCK_FILE_LINE_PM_STATUS, PM_STATUS_READY);
#ifdef USE_SYSTEMD
		sd_notify(0, "READY=1");
#endif

		pmState = PM_HOT_STANDBY;
		connsAllowed = true;

		/* 某些 worker 现在可能已排定启动  */
		StartWorkerNeeded = true;
	}

	/* 处理后台 worker 的状态变化。   */
	if (CheckPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE))
	{
		/* 未停止时才接受新的 worker 请求。     */
		BackgroundWorkerStateChange(pmState < PM_STOP_BACKENDS);
		StartWorkerNeeded = true;
	}

	if (StartWorkerNeeded || HaveCrashedWorker)
		maybe_start_bgworkers();

	/* 请求时通知 syslogger 轮转日志文件  */
	if (SysLoggerPID != 0)
	{
		if (CheckLogrotateSignal())
		{
			signal_child(SysLoggerPID, SIGUSR1);
			RemoveLogrotateSignalFiles();
		}
		else if (CheckPostmasterSignal(PMSIGNAL_ROTATE_LOGFILE))
		{
			signal_child(SysLoggerPID, SIGUSR1);
		}
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/*
		 * 即使名义上未启用 autovacuum，也启动一轮              
		 * autovacuum 守护进程迭代。这是为了能对事务 ID        
		 * 回卷保持主动防御。我们设置一个标志，让主循环 
		 * 去执行，而不是在此处尝试执行 —— 这是因为   
		 * autovac 进程自身可能发送该信号，而我们希望在   
		 * 当前一轮完成后尽快启动另一轮来处理它。     
		 *           
		 */
		start_autovac_launcher = true;
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER) &&
		Shutdown <= SmartShutdown && pmState < PM_STOP_BACKENDS)
	{
		/* autovacuum launcher 要求我们启动 worker 进程。     */
		StartAutovacuumWorker();
	}

	if (CheckPostmasterSignal(PMSIGNAL_START_WALRECEIVER))
	{
		/* startup 进程要求我们启动 walreceiver 进程。     */
		/* 如可能则立即启动，否则记住该请求稍后处理。 */
		WalReceiverRequested = true;
		MaybeStartWalReceiver();
	}

	/*
	 * 如果子进程有请求，尝试推进 postmaster 的状态机。
	 *
	 * 需要注意此操作相对于本函数其他操作的顺序。      
	 * 一般来说，它应放在其他操作之后，以防其他操作    
	 * 产生了 PostmasterStateMachine 需要知晓的影响。         
	 * 不过应在 CheckPromoteSignal 步骤之前执行，             
	 * 该步骤对状态机没有（即时）影响，但确实依赖  
	 * 我们当前所处的状态。    
	 */
	if (CheckPostmasterSignal(PMSIGNAL_ADVANCE_STATE_MACHINE))
	{
		PostmasterStateMachine();
	}

	if (StartupPID != 0 &&
		(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
		 pmState == PM_HOT_STANDBY) &&
		CheckPromoteSignal())
	{
		/*
		 * 通知 startup 进程完成恢复。    
		 *
		 * 保留提升信号文件不删，交由 startup 进程            
		 * 去删除。  
		 */
		signal_child(StartupPID, SIGUSR2);
	}
}

/*
 * 处理启动包时收到 SIGTERM。     
 *
 * 从信号处理器中调用 proc_exit() 相当不安全。      
 * 不过由于我们尚未触碰共享内存，可以直接        
 * 切断退出，不运行任何 atexit 处理器。         
 *
 * 有人可能想尝试发送一条消息，或记录一条日志，   
 * 来说明我们为何断开连接。然而这本身也相当不安全。 
 * 另外，向尚未完成认证、甚至尚未给我们发送          
 * 启动包的客户端，似乎都不宜透露数据库状态。       
 *                  
 */
static void
process_startup_packet_die(SIGNAL_ARGS)
{
	_exit(1);
}

/*
 * 空信号处理器  
 *
 * 它用于那些 postmaster 实际不用、而后端会用的信号。
 * 如果 postmaster 对这类信号置 SIG_IGN，                      
 * 那么新启动的后端可能会丢失在其重新配置信号处理    
 * 之前到达的信号。（参见 tcop/postgres.c 中的说明。） 
 *                  
 */
static void
dummy_handler(SIGNAL_ARGS)
{
}

/*
 * 处理启动包时超时。             
 * 同 process_startup_packet_die()，经 _exit(1) 退出。 
 */
static void
StartupPacketTimeoutHandler(void)
{
	_exit(1);
}


/*
 * 生成随机取消键。     
 */
static bool
RandomCancelKey(int32 *cancel_key)
{
	return pg_strong_random(cancel_key, sizeof(int32));
}

/*
 * 统计指定类型的子进程数量（dead_end 子进程               
 * 总是被排除）。
 */
static int
CountChildren(int target)
{
	dlist_iter	iter;
	int			cnt = 0;

	dlist_foreach(iter, &BackendList)
	{
		Backend    *bp = dlist_container(Backend, elem, iter.cur);

		if (bp->dead_end)
			continue;

		/*
		 * 由于 target == BACKEND_TYPE_ALL 是最常见情形，          
		 * 先测它，避免每个子进程都触碰共享内存。 
		 */
		if (target != BACKEND_TYPE_ALL)
		{
			/*
			 * 为最近宣告的 WAL Sender 进程指定 bkend_type  
			 *           
			 */
			if (bp->bkend_type == BACKEND_TYPE_NORMAL &&
				IsPostmasterChildWalSender(bp->child_slot))
				bp->bkend_type = BACKEND_TYPE_WALSND;

			if (!(target & bp->bkend_type))
				continue;
		}

		cnt++;
	}
	return cnt;
}


/*
 * StartChildProcess —— 为 postmaster 启动一个辅助进程  
 *
 * "type" 决定启动哪种子进程。所有类型的子进程         
 * 最初都进入 AuxiliaryProcessMain，由它处理公共设置。   
 *
 * StartChildProcess 的返回值是子进程的 PID，若启动       
 * 失败则为 0。   
 */
static pid_t
StartChildProcess(AuxProcType type)
{
	pid_t		pid;

#ifdef EXEC_BACKEND
	{
		char	   *av[10];
		int			ac = 0;
		char		typebuf[32];

		/*
		 * 为子进程设置命令行参数           
		 */
		av[ac++] = "postgres";
		av[ac++] = "--forkaux";
		av[ac++] = NULL;		/* 由 postmaster_forkexec 填入   */

		snprintf(typebuf, sizeof(typebuf), "%d", type);
		av[ac++] = typebuf;

		av[ac] = NULL;
		Assert(ac < lengthof(av));

		pid = postmaster_forkexec(ac, av);
	}
#else							/* !EXEC_BACKEND */
	pid = fork_process();

	if (pid == 0)				/* 子端*/
	{
		InitPostmasterChild();

		/* 关闭 postmaster 的套接字 */
		ClosePostmasterPorts(false);

		/* 释放 postmaster 的工作内存上下文  */
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextDelete(PostmasterContext);
		PostmasterContext = NULL;

		AuxiliaryProcessMain(type); /* 不返回       */
	}
#endif							/* EXEC_BACKEND */

	if (pid < 0)
	{
		/* 父进程 fork 失败  */
		int			save_errno = errno;

		errno = save_errno;
		switch (type)
		{
			case StartupProcess:
				ereport(LOG,
						(errmsg("could not fork startup process: %m")));
				break;
			case ArchiverProcess:
				ereport(LOG,
						(errmsg("could not fork archiver process: %m")));
				break;
			case BgWriterProcess:
				ereport(LOG,
						(errmsg("could not fork background writer process: %m")));
				break;
			case CheckpointerProcess:
				ereport(LOG,
						(errmsg("could not fork checkpointer process: %m")));
				break;
			case WalWriterProcess:
				ereport(LOG,
						(errmsg("could not fork WAL writer process: %m")));
				break;
			case WalReceiverProcess:
				ereport(LOG,
						(errmsg("could not fork WAL receiver process: %m")));
				break;
			default:
				ereport(LOG,
						(errmsg("could not fork process: %m")));
				break;
		}

		/*
		 * 启动期间 fork 失败是致命的，但若其他类型的      
		 * 子进程启动失败，则无需立即崩溃。
		 */
		if (type == StartupProcess)
			ExitPostmaster(1);
		return 0;
	}

	/*
	 * 父进程中 fork 成功  
	 */
	return pid;
}

/*
 * StartAutovacuumWorker
 *		启动 autovac worker 进程。 
 *
 * 此函数放在这里，是因为它会把得到的 PID 填入    
 * postmaster 私有的后端列表。
 *
 * 注意：此代码与 BackendStartup 大致相似。
 */
static void
StartAutovacuumWorker(void)
{
	Backend    *bn;

	/*
	 * 如果当前条件不允许运行进程，不要尝试，而是      
	 * 像对待 fork 失败那样处理。这通常不会发生，因为    
	 * 该信号只应在时机合适时由 autovacuum launcher 发送，   
	 * 但仍须检查以避免数据库状态变更期间的竞态     
	 *         
	 */
	if (canAcceptConnections(BACKEND_TYPE_AUTOVAC) == CAC_OK)
	{
		/*
		 * 计算将指派给本会话的取消键。我们                
		 * 对 autovac worker 可能并不需要取消键，但         
		 * 该字段中最好有些随机值，以防不怀好意者      
		 * 向它们发送取消请求。      
		 */
		if (!RandomCancelKey(&MyCancelKey))
		{
			ereport(LOG,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random cancel key")));
			return;
		}

		bn = (Backend *) malloc(sizeof(Backend));
		if (bn)
		{
			bn->cancel_key = MyCancelKey;

			/* autovac worker 非 dead_end，需要子进程槽位    */
			bn->dead_end = false;
			bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
			bn->bgworker_notify = false;

			bn->pid = StartAutoVacWorker();
			if (bn->pid > 0)
			{
				bn->bkend_type = BACKEND_TYPE_AUTOVAC;
				dlist_push_head(&BackendList, &bn->elem);
#ifdef EXEC_BACKEND
				ShmemBackendArrayAdd(bn);
#endif
				/* 正常 */
				return;
			}

			/*
			 * fork failed, fall through to report -- actual error message was
			 * logged by StartAutoVacWorker
			 */
			(void) ReleasePostmasterChildSlot(bn->child_slot);
			free(bn);
		}
		else
			ereport(LOG,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
	}

	/*
	 * 如果 launcher 在运行，则向其报告此次失败。（如果   
	 * 未在运行，我们甚至可能尚未连接到共享内存，   
	 * 所以不要尝试调用 AutoVacWorkerFailed。）注意             
	 * 我们还需要向它发信号使其响应这一状况，但此处   
	 * 并不这么做，而是等 ServerLoop 去做。这样可避免     
	 * 在情况变糟时 autovac launcher 与 postmaster 间快速        
	 * 连续信令。 
	 */
	if (AutoVacPID != 0)
	{
		AutoVacWorkerFailed();
		avlauncher_needs_signal = true;
	}
}

/*
 * MaybeStartWalReceiver
 *		若未运行且状态允许，则启动 WAL 接收器进程。      
 *
 * 注意：如果 WalReceiverPID 已非零，看似我们应当           
 * 清除 WalReceiverRequested。然而，如果 walreceiver            
 * 终止且 startup 进程随即请求新进程，就存在竞态：       
 * 完全可能在回收已死亡 walreceiver 进程之前就收到         
 * 该请求的信号。宁可冒险多启动一个 walreceiver， 
 * 也不要错过启动所需的那一个。（walreceiver 代码       
 * 有逻辑识别出自己应在不需要时退出。）       
 */
static void
MaybeStartWalReceiver(void)
{
	if (WalReceiverPID == 0 &&
		(pmState == PM_STARTUP || pmState == PM_RECOVERY ||
		 pmState == PM_HOT_STANDBY) &&
		Shutdown <= SmartShutdown)
	{
		WalReceiverPID = StartWalReceiver();
		if (WalReceiverPID != 0)
			WalReceiverRequested = false;
		/* 否则保持标志置位，以便稍后再试     */
	}
}


/*
 * 创建 opts 文件  
 */
static bool
CreateOptsFile(int argc, char *argv[], char *fullprogname)
{
	FILE	   *fp;
	int			i;

#define OPTS_FILE	"postmaster.opts"

	if ((fp = fopen(OPTS_FILE, "w")) == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	fprintf(fp, "%s", fullprogname);
	for (i = 1; i < argc; i++)
		fprintf(fp, " \"%s\"", argv[i]);
	fputs("\n", fp);

	if (fclose(fp))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m", OPTS_FILE)));
		return false;
	}

	return true;
}


/*
 * MaxLivePostmasterChildren
 *
 * 此函数报告每个子进程数组所需的条目数               
 * （PMChildFlags 数组；若为 EXEC_BACKEND 还有                 
 * ShmemBackendArray）。这些数组涵盖普通后端、          
 * autovac worker 和后台 worker，但不包括特殊子进程与       
 * dead_end 子进程。这使数组可以有固定的最大尺寸，  
 * 即 canAcceptConnections() 强制的同一子进程过多上限。          
 * 具体值不太关键，只要大于 MaxBackends。      
 */
int
MaxLivePostmasterChildren(void)
{
	return 2 * (MaxConnections + autovacuum_max_workers + 1 +
				max_wal_senders + max_worker_processes);
}

/*
 * 将后台 worker 连接到数据库。  
 */
void
BackgroundWorkerInitializeConnection(const char *dbname, const char *username, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;

	/* XXX 这个 errcode 对吗？   */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(dbname, InvalidOid,	/* 要连接的数据库  */
				 username, InvalidOid,	/* 连接所用角色 */
				 false,			/* 从不遵循 session_preload_libraries*/
				 (flags & BGWORKER_BYPASS_ALLOWCONN) != 0,	/* 忽略 datallowconn? */
				 NULL);			/* 无 out_dbname*/

	/* 它最好尚未脱离 "init" 模式             */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * 使用 OID 将后台 worker 连接到数据库。  
 */
void
BackgroundWorkerInitializeConnectionByOid(Oid dboid, Oid useroid, uint32 flags)
{
	BackgroundWorker *worker = MyBgworkerEntry;

	/* XXX 这个 errcode 对吗？   */
	if (!(worker->bgw_flags & BGWORKER_BACKEND_DATABASE_CONNECTION))
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("database connection requirement not indicated during registration")));

	InitPostgres(NULL, dboid,	/* 要连接的数据库  */
				 NULL, useroid, /* 连接所用角色 */
				 false,			/* 从不遵循 session_preload_libraries*/
				 (flags & BGWORKER_BYPASS_ALLOWCONN) != 0,	/* 忽略 datallowconn? */
				 NULL);			/* 无 out_dbname*/

	/* 它最好尚未脱离 "init" 模式             */
	if (!IsInitProcessingMode())
		ereport(ERROR,
				(errmsg("invalid processing mode in background worker")));
	SetProcessingMode(NormalProcessing);
}

/*
 * 阻塞/解除阻塞后台 worker 的信号  
 */
void
BackgroundWorkerBlockSignals(void)
{
	sigprocmask(SIG_SETMASK, &BlockSig, NULL);
}

void
BackgroundWorkerUnblockSignals(void)
{
	sigprocmask(SIG_SETMASK, &UnBlockSig, NULL);
}

#ifdef EXEC_BACKEND
static pid_t
bgworker_forkexec(int shmem_slot)
{
	char	   *av[10];
	int			ac = 0;
	char		forkav[MAXPGPATH];

	snprintf(forkav, MAXPGPATH, "--forkbgworker=%d", shmem_slot);

	av[ac++] = "postgres";
	av[ac++] = forkav;
	av[ac++] = NULL;			/* 由 postmaster_forkexec 填入   */
	av[ac] = NULL;

	Assert(ac < lengthof(av));

	return postmaster_forkexec(ac, av);
}
#endif

/*
 * 启动新 bgworker。
 * 启动时机条件必须已经检查过。              
 *
 * 成功返回 true，失败返回 false。 
 * 两种情况下都要适当更新 RegisteredBgWorker 的状态。   
 *
 * 此代码大量借鉴自 autovacuum.c。        
 */
static bool
do_start_bgworker(RegisteredBgWorker *rw)
{
	pid_t		worker_pid;

	Assert(rw->rw_pid == 0);

	/*
	 * 分配并指派 Backend 元素。注意必须在 fork 之前         
	 * 完成，以便能干净地处理失败（内存耗尽或子进程     
	 * 槽位）。   
	 *
	 * 把失败当作该 worker 已崩溃来处理。这样，      
	 * postmaster 在再次尝试启动它之前会先等待片刻；       
	 * 如果我们立刻重试，多半会再次撞上同样的          
	 * 资源耗尽状况。              
	 */
	if (!assign_backendlist_entry(rw))
	{
		rw->rw_crashed_at = GetCurrentTimestamp();
		return false;
	}

	ereport(DEBUG1,
			(errmsg_internal("starting background worker process \"%s\"",
							 rw->rw_worker.bgw_name)));

#ifdef EXEC_BACKEND
	switch ((worker_pid = bgworker_forkexec(rw->rw_shmem_slot)))
#else
	switch ((worker_pid = fork_process()))
#endif
	{
		case -1:
			/* postmaster 中 fork 失败…  */
			ereport(LOG,
					(errmsg("could not fork worker process: %m")));
			/* 撤销 assign_backendlist_entry 操作 */
			ReleasePostmasterChildSlot(rw->rw_child_slot);
			rw->rw_child_slot = 0;
			free(rw->rw_backend);
			rw->rw_backend = NULL;
			/* 标记条目为已崩溃，稍后重试         */
			rw->rw_crashed_at = GetCurrentTimestamp();
			break;

#ifndef EXEC_BACKEND
		case 0:
			/* postmaster 子进程中 */
			InitPostmasterChild();

			/* 关闭 postmaster 的套接字 */
			ClosePostmasterPorts(false);

			/*
			 * 在销毁 PostmasterContext 之前，先保存此 bgworker 
			 * 的数据供其取用。  
			 */
			MyBgworkerEntry = (BackgroundWorker *)
				MemoryContextAlloc(TopMemoryContext, sizeof(BackgroundWorker));
			memcpy(MyBgworkerEntry, &rw->rw_worker, sizeof(BackgroundWorker));

			/* 释放 postmaster 的工作内存上下文  */
			MemoryContextSwitchTo(TopMemoryContext);
			MemoryContextDelete(PostmasterContext);
			PostmasterContext = NULL;

			StartBackgroundWorker();

			exit(1);			/* 不应到达此处  */
			break;
#endif
		default:
			/* postmaster 中 fork 成功…      */
			rw->rw_pid = worker_pid;
			rw->rw_backend->pid = rw->rw_pid;
			ReportBackgroundWorkerPID(rw);
			/* 将新 worker 加入后端列表    */
			dlist_push_head(&BackendList, &rw->rw_backend->elem);
#ifdef EXEC_BACKEND
			ShmemBackendArrayAdd(rw->rw_backend);
#endif
			return true;
	}

	return false;
}

/*
 * 当前 postmaster 状态是否要求以指定的启动时间        
 * 启动 worker？     
 */
static bool
bgworker_should_start_now(BgWorkerStartTime start_time)
{
	switch (pmState)
	{
		case PM_NO_CHILDREN:
		case PM_WAIT_DEAD_END:
		case PM_SHUTDOWN_2:
		case PM_SHUTDOWN:
		case PM_WAIT_BACKENDS:
		case PM_STOP_BACKENDS:
			break;

		case PM_RUN:
			if (start_time == BgWorkerStart_RecoveryFinished)
				return true;
			/* fall through */

		case PM_HOT_STANDBY:
			if (start_time == BgWorkerStart_ConsistentState)
				return true;
			/* fall through */

		case PM_RECOVERY:
		case PM_STARTUP:
		case PM_INIT:
			if (start_time == BgWorkerStart_PostmasterStart)
				return true;
			/* fall through */
	}

	return false;
}

/*
 * 为已连接的后台 worker 分配 Backend 结构体，但暂时       
 * 不要将其加入后端列表。       
 *
 * 失败时返回 false，且不改变任何 worker 状态。
 *
 * Backend 的部分信息会复制到传入的 rw 中。  
 */
static bool
assign_backendlist_entry(RegisteredBgWorker *rw)
{
	Backend    *bn;

	/*
	 * 检查数据库状态是否允许再建立一个连接。当前    
	 * 唯一可能的失败是 CAC_TOOMANY，所以我们只据此         
	 * 记录一条错误消息，而不精确检查错误码。   
	 */
	if (canAcceptConnections(BACKEND_TYPE_BGWORKER) != CAC_OK)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("no slot available for new worker process")));
		return false;
	}

	/*
	 * 计算将指派给本会话的取消键。我们                
	 * 对后台 worker 可能并不需要取消键，但该字段中         
	 * 最好有些随机值，以防不怀好意者                       
	 * 向其发取消请求。
	 */
	if (!RandomCancelKey(&MyCancelKey))
	{
		ereport(LOG,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate random cancel key")));
		return false;
	}

	bn = malloc(sizeof(Backend));
	if (bn == NULL)
	{
		ereport(LOG,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return false;
	}

	bn->cancel_key = MyCancelKey;
	bn->child_slot = MyPMChildSlot = AssignPostmasterChildSlot();
	bn->bkend_type = BACKEND_TYPE_BGWORKER;
	bn->dead_end = false;
	bn->bgworker_notify = false;

	rw->rw_backend = bn;
	rw->rw_child_slot = bn->child_slot;

	return true;
}

/*
 * 时机一到就启动后台工作进程。       
 *
 * 作为副作用，bgworker 控制变量会被设置或复位，  
 * 视是否还需启动更多工作进程而定。         
 *
 * 我们限制每次调用启动的 worker 数量，以免在            
 * 大量此类请求待处理时占用 postmaster 太久的注意力。   
 * 只要 StartWorkerNeeded 为真，ServerLoop 就不会阻塞，         
 * 而会在处理完其他事务后再次调用本函数。    
 */
static void
maybe_start_bgworkers(void)
{
#define MAX_BGWORKERS_TO_LAUNCH 100
	int			num_launched = 0;
	TimestampTz now = 0;
	slist_mutable_iter iter;

	/*
	 * 崩溃恢复期间，在状态脱离恢复之前，                
	 * 无需调用本函数。   
	 */
	if (FatalError)
	{
		StartWorkerNeeded = false;
		HaveCrashedWorker = false;
		return;
	}

	/* 除非在下面找到理由，否则无需再次被调用。       */
	StartWorkerNeeded = false;
	HaveCrashedWorker = false;

	slist_foreach_modify(iter, &BackgroundWorkerList)
	{
		RegisteredBgWorker *rw;

		rw = slist_container(RegisteredBgWorker, rw_lnode, iter.cur);

		/* 已在运行则忽略。  */
		if (rw->rw_pid != 0)
			continue;

		/* 若标记待终止，则清理并移出列表。   */
		if (rw->rw_terminate)
		{
			ForgetBackgroundWorker(&iter);
			continue;
		}

		/*
		 * 若此 worker 之前崩溃过，可能需要重启它      
		 * （除非注册时声明完全不想重启）。                 
		 * 检查上一次崩溃发生在多久之前。                     
		 * 若上次崩溃距今太近，不要立刻启动；               
		 * 待时间足够后再重启它。        
		 */
		if (rw->rw_crashed_at != 0)
		{
			if (rw->rw_worker.bgw_restart_time == BGW_NEVER_RESTART)
			{
				int			notify_pid;

				notify_pid = rw->rw_worker.bgw_notify_pid;

				ForgetBackgroundWorker(&iter);

				/* 报告 worker 已消失。 */
				if (notify_pid != 0)
					kill(notify_pid, SIGUSR1);

				continue;
			}

			/* 需要时才读取系统时间。 */
			if (now == 0)
				now = GetCurrentTimestamp();

			if (!TimestampDifferenceExceeds(rw->rw_crashed_at, now,
											rw->rw_worker.bgw_restart_time * 1000))
			{
				/* 设置标志，记住稍后还有 worker 待启动。    */
				HaveCrashedWorker = true;
				continue;
			}
		}

		if (bgworker_should_start_now(rw->rw_worker.bgw_start_time))
		{
			/* 尝试启动 worker 前重置崩溃时间。   */
			rw->rw_crashed_at = 0;

			/*
			 * 尝试启动 worker。  
			 *
			 * 若失败，暂时放弃处理 worker，但置位      
			 * StartWorkerNeeded，以便 ServerLoop 下一轮                
			 * 循环时回到此处重试。（不想等待，因为      
			 * 可能还有别的可运行 worker。）本也可置          
			 * HaveCrashedWorker，因为此 worker 现已被            
			 * 标记为已崩溃，但没必要：本函数下次      
			 * 运行自会如此。 
			 */
			if (!do_start_bgworker(rw))
			{
				StartWorkerNeeded = true;
				return;
			}

			/*
			 * 若已启动的 worker 达到允许上限，则退出，    
			 * 但让 ServerLoop 再次调用我们去找别的            
			 * 可运行 worker。可能一个也没有，下回            
			 * 便知。   
			 */
			if (++num_launched >= MAX_BGWORKERS_TO_LAUNCH)
			{
				StartWorkerNeeded = true;
				return;
			}
		}
	}
}

/*
 * 当某个后端请求获知 worker 状态变化时，我们      
 * 在其后端表项中设置标志。后台工作进程机制需要     
 * 知道此类后端何时退出  
 */
bool
PostmasterMarkPIDForWorkerNotify(int pid)
{
	dlist_iter	iter;
	Backend    *bp;

	dlist_foreach(iter, &BackendList)
	{
		bp = dlist_container(Backend, elem, iter.cur);
		if (bp->pid == pid)
		{
			bp->bgworker_notify = true;
			return true;
		}
	}
	return false;
}

#ifdef EXEC_BACKEND

/*
 * 以下各项需供 save/restore_backend_variables 函数使用。它们 
 * 在其所属模块中被标记为 NON_EXEC_STATIC。              
 */
extern slock_t *ShmemLock;
extern slock_t *ProcStructLock;
extern PGPROC *AuxiliaryProcs;
extern PMSignalData *PMSignalState;
extern pg_time_t first_syslogger_file_time;

#ifndef WIN32
#define write_inheritable_socket(dest, src, childpid) ((*(dest) = (src)), true)
#define read_inheritable_socket(dest, src) (*(dest) = *(src))
#else
static bool write_duplicated_handle(HANDLE *dest, HANDLE src, HANDLE child);
static bool write_inheritable_socket(InheritableSocket *dest, SOCKET src,
									 pid_t childPid);
static void read_inheritable_socket(SOCKET *dest, InheritableSocket *src);
#endif


/* 将关键后端变量保存到 BackendParameters 结构中。     */
#ifndef WIN32
static bool
save_backend_variables(BackendParameters *param, Port *port)
#else
static bool
save_backend_variables(BackendParameters *param, Port *port,
					   HANDLE childProcess, pid_t childPid)
#endif
{
	memcpy(&param->port, port, sizeof(Port));
	if (!write_inheritable_socket(&param->portsocket, port->sock, childPid))
		return false;

	strlcpy(param->DataDir, DataDir, MAXPGPATH);

	memcpy(&param->ListenSocket, &ListenSocket, sizeof(ListenSocket));

	param->MyCancelKey = MyCancelKey;
	param->MyPMChildSlot = MyPMChildSlot;

#ifdef WIN32
	param->ShmemProtectiveRegion = ShmemProtectiveRegion;
#endif
	param->UsedShmemSegID = UsedShmemSegID;
	param->UsedShmemSegAddr = UsedShmemSegAddr;

	param->ShmemLock = ShmemLock;
	param->ShmemVariableCache = ShmemVariableCache;
	param->ShmemBackendArray = ShmemBackendArray;

#ifndef HAVE_SPINLOCKS
	param->SpinlockSemaArray = SpinlockSemaArray;
#endif
	param->NamedLWLockTrancheRequests = NamedLWLockTrancheRequests;
	param->NamedLWLockTrancheArray = NamedLWLockTrancheArray;
	param->MainLWLockArray = MainLWLockArray;
	param->ProcStructLock = ProcStructLock;
	param->ProcGlobal = ProcGlobal;
	param->AuxiliaryProcs = AuxiliaryProcs;
	param->PreparedXactProcs = PreparedXactProcs;
	param->PMSignalState = PMSignalState;

	param->PostmasterPid = PostmasterPid;
	param->PgStartTime = PgStartTime;
	param->PgReloadTime = PgReloadTime;
	param->first_syslogger_file_time = first_syslogger_file_time;

	param->redirection_done = redirection_done;
	param->IsBinaryUpgrade = IsBinaryUpgrade;
	param->query_id_enabled = query_id_enabled;
	param->max_safe_fds = max_safe_fds;

	param->MaxBackends = MaxBackends;

#ifdef WIN32
	param->PostmasterHandle = PostmasterHandle;
	if (!write_duplicated_handle(&param->initial_signal_pipe,
								 pgwin32_create_signal_listener(childPid),
								 childProcess))
		return false;
#else
	memcpy(&param->postmaster_alive_fds, &postmaster_alive_fds,
		   sizeof(postmaster_alive_fds));
#endif

	memcpy(&param->syslogPipe, &syslogPipe, sizeof(syslogPipe));

	strlcpy(param->my_exec_path, my_exec_path, MAXPGPATH);

	strlcpy(param->pkglib_path, pkglib_path, MAXPGPATH);

	return true;
}


#ifdef WIN32
/*
 * 复制一个句柄供子进程使用，并将句柄的              
 * 子进程实例写入参数文件。                 
 */
static bool
write_duplicated_handle(HANDLE *dest, HANDLE src, HANDLE childProcess)
{
	HANDLE		hChild = INVALID_HANDLE_VALUE;

	if (!DuplicateHandle(GetCurrentProcess(),
						 src,
						 childProcess,
						 &hChild,
						 0,
						 TRUE,
						 DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
	{
		ereport(LOG,
				(errmsg_internal("could not duplicate handle to be written to backend parameter file: error code %lu",
								 GetLastError())));
		return false;
	}

	*dest = hChild;
	return true;
}

/*
 * 复制一个套接字供子进程使用，并将所得                  
 * 结构写入参数文件。     
 * 这样做是因为 Windows 上非常常见的许多 LSP（分层服务    
 * 提供商，杀毒软件、防火墙、下载工具等）会破坏   
 * 套接字的直接继承。 
 */
static bool
write_inheritable_socket(InheritableSocket *dest, SOCKET src, pid_t childpid)
{
	dest->origsocket = src;
	if (src != 0 && src != PGINVALID_SOCKET)
	{
		/* 真套接字  */
		if (WSADuplicateSocket(src, childpid, &dest->wsainfo) != 0)
		{
			ereport(LOG,
					(errmsg("could not duplicate socket %d for use in backend: error code %d",
							(int) src, WSAGetLastError())));
			return false;
		}
	}
	return true;
}

/*
 * 读回复制的套接字结构，并取得套接字描述符。       
 */
static void
read_inheritable_socket(SOCKET *dest, InheritableSocket *src)
{
	SOCKET		s;

	if (src->origsocket == PGINVALID_SOCKET || src->origsocket == 0)
	{
		/* 非真套接字！ */
		*dest = src->origsocket;
	}
	else
	{
		/* 实际套接字，故从结构创建。 */
		s = WSASocket(FROM_PROTOCOL_INFO,
					  FROM_PROTOCOL_INFO,
					  FROM_PROTOCOL_INFO,
					  &src->wsainfo,
					  0,
					  0);
		if (s == INVALID_SOCKET)
		{
			write_stderr("could not create inherited socket: error code %d\n",
						 WSAGetLastError());
			exit(1);
		}
		*dest = s;

		/*
		 * 为免对同一套接字产生两个引用，关闭原始         
		 * 套接字。（这只在继承真正生效时发生。）     
		 *        
		 */
		closesocket(src->origsocket);
	}
}
#endif

static void
read_backend_variables(char *id, Port *port)
{
	BackendParameters param;

#ifndef WIN32
	/* 非 win32 实现从文件读取。       */
	FILE	   *fp;

	/* 开文件 */
	fp = AllocateFile(id, PG_BINARY_R);
	if (!fp)
	{
		write_stderr("could not open backend variables file \"%s\": %s\n",
					 id, strerror(errno));
		exit(1);
	}

	if (fread(&param, sizeof(param), 1, fp) != 1)
	{
		write_stderr("could not read from backend variables file \"%s\": %s\n",
					 id, strerror(errno));
		exit(1);
	}

	/* 释放文件 */
	FreeFile(fp);
	if (unlink(id) != 0)
	{
		write_stderr("could not remove file \"%s\": %s\n",
					 id, strerror(errno));
		exit(1);
	}
#else
	/* win32 下采用映射文件。 */
	HANDLE		paramHandle;
	BackendParameters *paramp;

#ifdef _WIN64
	paramHandle = (HANDLE) _atoi64(id);
#else
	paramHandle = (HANDLE) atol(id);
#endif
	paramp = MapViewOfFile(paramHandle, FILE_MAP_READ, 0, 0, 0);
	if (!paramp)
	{
		write_stderr("could not map view of backend variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}

	memcpy(&param, paramp, sizeof(BackendParameters));

	if (!UnmapViewOfFile(paramp))
	{
		write_stderr("could not unmap view of backend variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}

	if (!CloseHandle(paramHandle))
	{
		write_stderr("could not close handle to backend parameter variables: error code %lu\n",
					 GetLastError());
		exit(1);
	}
#endif

	restore_backend_variables(&param, port);
}

/* 从 BackendParameters 结构恢复关键后端变量。              */
static void
restore_backend_variables(BackendParameters *param, Port *port)
{
	memcpy(port, &param->port, sizeof(Port));
	read_inheritable_socket(&port->sock, &param->portsocket);

	SetDataDir(param->DataDir);

	memcpy(&ListenSocket, &param->ListenSocket, sizeof(ListenSocket));

	MyCancelKey = param->MyCancelKey;
	MyPMChildSlot = param->MyPMChildSlot;

#ifdef WIN32
	ShmemProtectiveRegion = param->ShmemProtectiveRegion;
#endif
	UsedShmemSegID = param->UsedShmemSegID;
	UsedShmemSegAddr = param->UsedShmemSegAddr;

	ShmemLock = param->ShmemLock;
	ShmemVariableCache = param->ShmemVariableCache;
	ShmemBackendArray = param->ShmemBackendArray;

#ifndef HAVE_SPINLOCKS
	SpinlockSemaArray = param->SpinlockSemaArray;
#endif
	NamedLWLockTrancheRequests = param->NamedLWLockTrancheRequests;
	NamedLWLockTrancheArray = param->NamedLWLockTrancheArray;
	MainLWLockArray = param->MainLWLockArray;
	ProcStructLock = param->ProcStructLock;
	ProcGlobal = param->ProcGlobal;
	AuxiliaryProcs = param->AuxiliaryProcs;
	PreparedXactProcs = param->PreparedXactProcs;
	PMSignalState = param->PMSignalState;

	PostmasterPid = param->PostmasterPid;
	PgStartTime = param->PgStartTime;
	PgReloadTime = param->PgReloadTime;
	first_syslogger_file_time = param->first_syslogger_file_time;

	redirection_done = param->redirection_done;
	IsBinaryUpgrade = param->IsBinaryUpgrade;
	query_id_enabled = param->query_id_enabled;
	max_safe_fds = param->max_safe_fds;

	MaxBackends = param->MaxBackends;

#ifdef WIN32
	PostmasterHandle = param->PostmasterHandle;
	pgwin32_initial_signal_pipe = param->initial_signal_pipe;
#else
	memcpy(&postmaster_alive_fds, &param->postmaster_alive_fds,
		   sizeof(postmaster_alive_fds));
#endif

	memcpy(&syslogPipe, &param->syslogPipe, sizeof(syslogPipe));

	strlcpy(my_exec_path, param->my_exec_path, MAXPGPATH);

	strlcpy(pkglib_path, param->pkglib_path, MAXPGPATH);

	/*
	 * 我们需要恢复 fd.c 中外部打开 FD 的计数；为免      
	 * 混淆，务必在恢复 max_safe_fds 之后再做。（注        
	 * 意：BackendInitialize 会处理 port->sock。） 
	 */
#ifndef WIN32
	if (postmaster_alive_fds[0] >= 0)
		ReserveExternalFD();
	if (postmaster_alive_fds[1] >= 0)
		ReserveExternalFD();
#endif
}


Size
ShmemBackendArraySize(void)
{
	return mul_size(MaxLivePostmasterChildren(), sizeof(Backend));
}

void
ShmemBackendArrayAllocation(void)
{
	Size		size = ShmemBackendArraySize();

	ShmemBackendArray = (Backend *) ShmemAlloc(size);
	/* 将所有槽置为空。*/
	memset(ShmemBackendArray, 0, size);
}

static void
ShmemBackendArrayAdd(Backend *bn)
{
	/* 与我的 PMChildSlot 对应的数组槽应当空闲。       */
	int			i = bn->child_slot - 1;

	Assert(ShmemBackendArray[i].pid == 0);
	ShmemBackendArray[i] = *bn;
}

static void
ShmemBackendArrayRemove(Backend *bn)
{
	int			i = bn->child_slot - 1;

	Assert(ShmemBackendArray[i].pid == bn->pid);
	/* 把该槽标为空。  */
	ShmemBackendArray[i].pid = 0;
}
#endif							/* EXEC_BACKEND */


#ifdef WIN32

/*
 * 面向 Windows 的 waitpid() 子集实现。假定 pid 为 -1       
 * （即检查所有子进程），options 为 WNOHANG（不等待）。   
 */
static pid_t
waitpid(pid_t pid, int *exitstatus, int options)
{
	win32_deadchild_waitinfo *childinfo;
	DWORD		exitcode;
	DWORD		dwd;
	ULONG_PTR	key;
	OVERLAPPED *ovl;

	/* 尝试从队列中取用一个 win32_deadchild_waitinfo。  */
	if (!GetQueuedCompletionStatus(win32ChildQueue, &dwd, &key, &ovl, 0))
	{
		errno = EAGAIN;
		return -1;
	}

	childinfo = (win32_deadchild_waitinfo *) key;
	pid = childinfo->procId;

	/*
	 * 须从等待中移除句柄——即使只设定等待一次。     
	 *     
	 */
	UnregisterWaitEx(childinfo->waitHandle, NULL);

	if (!GetExitCodeProcess(childinfo->procHandle, &exitcode))
	{
		/*
		 * 本不应发生。告知用户并设定固定退出码。 
		 */
		write_stderr("could not read exit code for process\n");
		exitcode = 255;
	}
	*exitstatus = exitcode;

	/*
	 * 关闭进程句柄。只有过了这一步之后，内核          
	 * 才能回收该 PID。 
	 */
	CloseHandle(childinfo->procHandle);

	/*
	 * 释放在调用 RegisterWaitForSingleObject()    
	 * 之前分配的结构体。  
	 */
	pfree(childinfo);

	return pid;
}

/*
 * 注意！以下代码运行于线程池之上！所有操作   
 * 都必须线程安全！切勿使用 elog() 等函数。        
 */
static void WINAPI
pgwin32_deadchild_callback(PVOID lpParameter, BOOLEAN TimerOrWaitFired)
{
	/* 不应发生，因为我们以 INFINITE 为超时值。      */
	if (TimerOrWaitFired)
		return;

	/*
	 * 把 win32_deadchild_waitinfo 对象投递给 waitpid() 处理。若    
	 * 投递失败，我们会泄漏该对象，但那时整个进程     
	 * 都已泄漏且陷入不可挽回的状态，担心这个没什么    
	 * 意义。我们想 panic，但无法从这个线程使用那套      
	 * 基础设施。  
	 */
	if (!PostQueuedCompletionStatus(win32ChildQueue,
									0,
									(ULONG_PTR) lpParameter,
									NULL))
		write_stderr("could not post child completion status\n");

	/* 将 SIGCHLD 入队。 */
	pg_queue_signal(SIGCHLD);
}
#endif							/* WIN32 */

/*
 * 初始化用于监视 postmaster 死亡的唯一句柄。      
 *
 * 在 postmaster 中调用一次，此后子进程即可借此           
 * 监视父进程是否已死。  
 */
static void
InitPostmasterDeathWatchHandle(void)
{
#ifndef WIN32

	/*
	 * 创建一个管道。postmaster 保持管道写端打开      
	 * （POSTMASTER_FD_OWN），子进程持有读端。子进程           
	 * 可把读文件描述符传给 select()，以便 postmaster       
	 * 死亡时被唤醒，或以 (read() == 0) 检测 postmaster            
	 * 是否死亡。子进程必须在 fork 之后尽快关闭写端，
	 * 因为只有所有进程都关闭了写 fd，读端才会收到      
	 * EOF 信号。这由 ClosePostmasterPorts() 负责处理。
	 */
	Assert(MyProcPid == PostmasterPid);
	if (pipe(postmaster_alive_fds) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg_internal("could not create pipe to monitor postmaster death: %m")));

	/* 通知 fd.c：已为管道用掉两个 FD。       */
	ReserveExternalFD();
	ReserveExternalFD();

	/*
	 * 设置 O_NONBLOCK，以便用 read() 探测该 fd 存在与否。  
	 *      
	 */
	if (fcntl(postmaster_alive_fds[POSTMASTER_FD_WATCH], F_SETFL, O_NONBLOCK) == -1)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg_internal("could not set postmaster death monitoring pipe to nonblocking mode: %m")));
#else

	/*
	 * 在 Windows 下我们用进程句柄实现此目的。   
	 */
	if (DuplicateHandle(GetCurrentProcess(),
						GetCurrentProcess(),
						GetCurrentProcess(),
						&PostmasterHandle,
						0,
						TRUE,
						DUPLICATE_SAME_ACCESS) == 0)
		ereport(FATAL,
				(errmsg_internal("could not duplicate postmaster handle: error code %lu",
								 GetLastError())));
#endif							/* WIN32 */
}
