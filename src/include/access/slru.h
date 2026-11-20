/*-------------------------------------------------------------------------
 *
 * slru.h
 *		事务状态日志文件的简单 LRU 缓冲
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/slru.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SLRU_H
#define SLRU_H

#include "access/xlogdefs.h"
#include "storage/lwlock.h"
#include "storage/sync.h"


/*
 * 定义 SLRU 段大小。页所用 BLCKSZ 与 Postgres 各处
 * 相同。段大小可较为任意地选取；
 * 我们默认取 32 页，即 256Kb，亦即 CLOG 为 1M 事务
 * 或 SUBTRANS 为 64K 事务。
 *
 * 注意：由于 TransactionIds 为 32 位并在 0xFFFFFFFF 处回卷，
 * 页编号亦在 0xFFFFFFFF/xxxx_XACTS_PER_PAGE 回卷（其中
 * xxxx 分别为 CLOG 或 SUBTRANS），段编号则在
 * 0xFFFFFFFF/xxxx_XACTS_PER_PAGE/SLRU_PAGES_PER_SEGMENT。我们
 * 在 slru.c 中不必显式理会该事实，仅当比较
 *  SimpleLruTruncate 段/页号时除外（见 PagePrecedes()）。
 */
#define SLRU_PAGES_PER_SEGMENT	32

/*
 * 页状态码。注意这些不含"脏"位。
 * page_dirty 仅在 VALID 或 WRITE_IN_PROGRESS 态可为真；
 * 后者意味着自写出开始以来页又被
 * 弄脏。
 */
typedef enum
{
	SLRU_PAGE_EMPTY,			/* 缓冲区未使用 */
	SLRU_PAGE_READ_IN_PROGRESS, /* 页正在读入 */
	SLRU_PAGE_VALID,			/* 页有效且未在写出 */
	SLRU_PAGE_WRITE_IN_PROGRESS /* 页正在写出 */
} SlruPageStatus;

/*
 * 共享内存状态
 */
typedef struct SlruSharedData
{
	LWLock	   *ControlLock;

	/* 此 SLRU 结构管理的缓冲区数 */
	int			num_slots;

	/*
	 * 保存各缓冲区槽信息的数组。页号在状态为 EMPTY
	 * 时未定义，page_lru_count 亦然.
	 */
	char	  **page_buffer;
	SlruPageStatus *page_status;
	bool	   *page_dirty;
	int		   *page_number;
	int		   *page_lru_count;
	LWLockPadded *buffer_locks;

	/*
	 * 与 SLRU 页内表项关联的可选 WAL 刷写 LSNs
	 * 数组。若非零/NULL，写页前必须先刷 WAL（
	 * pg_xact 为真，multixact/pg_subtrans/pg_notify 为假）。
	 * group_lsn[] 每缓冲区槽有 lsn_groups_per_page 项，
	 * 每项为该槽页上一连续组 SLRU 表项的最高
	 * LSN.
	 */
	XLogRecPtr *group_lsn;
	int			lsn_groups_per_page;

	/*----------
	 * 将页标为"最近使用"的方式是设置
	 *		page_lru_count[slotno] = ++cur_lru_count;
	 * 故最旧页即为下列差值最大者：
	 *		cur_lru_count - page_lru_count[slotno]
	 * 计数终将回卷，但只要没有任何页的年龄
	 * 超过 INT_MAX 次计数，此计算仍有效。
	 *----------
	 */
	int			cur_lru_count;

	/*
	 * latest_page_number 是日志当前末端的页号；
	 * 这不是关键数据，我们只用它来避免换出
	 * 最新页.
	 */
	int			latest_page_number;

	/* SLRU 的统计用索引（未必唯一） */
	int			slru_stats_idx;
} SlruSharedData;

typedef SlruSharedData *SlruShared;

/*
 * SlruCtlData 是未共享结构，指向共享内存里
 * 的活动信息.
 */
typedef struct SlruCtlData
{
	SlruShared	shared;

	/*
	 * 把同步请求交给 checkpointer 时使用哪个
	 * 同步处理函数。SYNC_HANDLER_NONE 禁用 fsync(如 pg_notify)。
	 */
	SyncRequestHandler sync_handler;

	/*
	 * 判定页是否"更旧"，用于截断及作为按 LRU
	 * 次序淘汰页的提示。若第一参数的每条表项都
	 * 比第二参数的每条表项更旧则返回真。注意
	 * !PagePrecedes(a,b) && !PagePrecedes(b,a) 未必意味 a==b；
	 * 当部分表项更旧、部分不然时也会如此。对使用
	 * SimpleLruTruncate() 的 SLRUs，必须用模算术。（对其余，
	 * 此回调行为无功能影响。）在符合其条件的
	 * SLRUs 中使用 SlruPagePrecedesUnitTests()。
	 */
	bool		(*PagePrecedes) (int, int);

	/*
	 * Dir 在 SimpleLruInit 期间设定，此后不变。因
	 * 其始终相同，无需放在共享内存中。
	 */
	char		Dir[64];
} SlruCtlData;

typedef SlruCtlData *SlruCtl;


extern Size SimpleLruShmemSize(int nslots, int nlsns);
extern void SimpleLruInit(SlruCtl ctl, const char *name, int nslots, int nlsns,
						  LWLock *ctllock, const char *subdir, int tranche_id,
						  SyncRequestHandler sync_handler);
extern int	SimpleLruZeroPage(SlruCtl ctl, int pageno);
extern int	SimpleLruReadPage(SlruCtl ctl, int pageno, bool write_ok,
							  TransactionId xid);
extern int	SimpleLruReadPage_ReadOnly(SlruCtl ctl, int pageno,
									   TransactionId xid);
extern void SimpleLruWritePage(SlruCtl ctl, int slotno);
extern void SimpleLruWriteAll(SlruCtl ctl, bool allow_redirtied);
#ifdef USE_ASSERT_CHECKING
extern void SlruPagePrecedesUnitTests(SlruCtl ctl, int per_page);
#else
#define SlruPagePrecedesUnitTests(ctl, per_page) do {} while (0)
#endif
extern void SimpleLruTruncate(SlruCtl ctl, int cutoffPage);
extern bool SimpleLruDoesPhysicalPageExist(SlruCtl ctl, int pageno);

typedef bool (*SlruScanCallback) (SlruCtl ctl, char *filename, int segpage,
								  void *data);
extern bool SlruScanDirectory(SlruCtl ctl, SlruScanCallback callback, void *data);
extern void SlruDeleteSegment(SlruCtl ctl, int segno);

extern int	SlruSyncFileTag(SlruCtl ctl, const FileTag *ftag, char *path);

/* SlruScanDirectory 的公开回调 */
extern bool SlruScanDirCbReportPresence(SlruCtl ctl, char *filename,
										int segpage, void *data);
extern bool SlruScanDirCbDeleteAll(SlruCtl ctl, char *filename, int segpage,
								   void *data);

#endif							/* SLRU_H */
