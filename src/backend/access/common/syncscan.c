/*-------------------------------------------------------------------------
 *
 * syncscan.c
 *	  扫描同步支持
 *
 * 当多个后端对同一张表做顺序扫描时，我们尽量
 * 让它们保持同步，以减少所需的总 I/O。目标是
 * 把每一页只读入共享缓冲区缓存一次，并让参与
 * 这次共享扫描的所有后端都在该页掉出
 * 缓存之前处理它。
 *
 * 由于一群做顺序扫描的后端里，"领头者" 必须等待
 * I/O，而 "跟随者" 不用等，一旦能让这些后端大致同时
 * 查看表的同一部分，就会有很强的自同步
 * 效果。因此真正需要的，只是让开始顺序扫描的
 * 新后端从其他后端正在读取的位置附近开始。
 * 我们可以环形地扫描该表，从块 X 直到
 * 末尾，再从块 0 到 X-1，这样既能访问全部行，
 * 又仍参与共同的扫描。
 *
 * 为此，我们跟踪每张表的扫描位置，并让
 * 新扫描从先前的 scan(s) 所在位置附近开始。之后我们不再
 * 做额外同步来把各次扫描保持在一起；有些
 * 扫描可能比其他扫描慢得多，例如结果需要经
 * 慢速网络传给客户端时，我们不希望这样的查询
 * 拖慢其他查询。
 *
 * 实际上，任一时刻对不同表进行的大型顺序扫描
 * 只能有少数几个。因此我们只把扫描位置放在
 * 一个很小的 LRU 列表里，每次需要查找或更新
 * 扫描位置时就扫描它。整套机制只用于超过
 * 某个阈值大小的表（但这不是本模块要关心的）。
 *
 * 接口例程
 *		ss_get_location		- 返回某关系的当前扫描位置
 *		ss_report_location	- 更新当前扫描位置
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/syncscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/syncscan.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/rel.h"


/* GUC 变量 */
#ifdef TRACE_SYNCSCAN
bool		trace_syncscan = false;
#endif


/*
 * LRU 列表的大小。
 *
 * 注意：代码假定 SYNC_SCAN_NELEM > 1。
 *
 * XXX：多大才合适？它应当大到能放下同时扫描的
 * 大表的最大数量。但值更大意味着开始新扫描时
 * 要更多地遍历 LRU 列表。
 */
#define SYNC_SCAN_NELEM 20

/*
 * 报告当前扫描位置的间隔，以页为单位。
 *
 * 注意：它应当小于我们用于批量读取的环大小
 * （见 buffer/freelist.c）。否则，加入其他扫描的一次扫描
 * 可能从已经不在缓冲区缓存中的页开始。这一点有些含糊；
 * 反正也不能保证新扫描会在该页离开
 * 缓冲区缓存之前读到它，另一方面，该页很有可能
 * 仍在操作系统缓存中。
 */
#define SYNC_SCAN_REPORT_INTERVAL (128 * 1024 / BLCKSZ)


/*
 * 扫描位置结构本质上是带首尾指针的双向链表 LRU，
 * 但设计成在固定大小的共享内存中存放固定的
 * 最大元素数。
 */
typedef struct ss_scan_location_t
{
	RelFileLocator relfilelocator;	/* 关系的标识 */
	BlockNumber location;		/* 该关系中上次报告的位置 */
} ss_scan_location_t;

typedef struct ss_lru_item_t
{
	struct ss_lru_item_t *prev;
	struct ss_lru_item_t *next;
	ss_scan_location_t location;
} ss_lru_item_t;

typedef struct ss_scan_locations_t
{
	ss_lru_item_t *head;
	ss_lru_item_t *tail;
	ss_lru_item_t items[FLEXIBLE_ARRAY_MEMBER]; /* SYNC_SCAN_NELEM 个元素 */
} ss_scan_locations_t;

#define SizeOfScanLocations(N) \
	(offsetof(ss_scan_locations_t, items) + (N) * sizeof(ss_lru_item_t))

/* 指向共享内存中该结构的指针 */
static ss_scan_locations_t *scan_locations;

/* 内部函数的原型 */
static BlockNumber ss_search(RelFileLocator relfilelocator,
							 BlockNumber location, bool set);


/*
 * SyncScanShmemSize --- 报告所需的共享内存空间大小
 */
Size
SyncScanShmemSize(void)
{
	return SizeOfScanLocations(SYNC_SCAN_NELEM);
}

/*
 * SyncScanShmemInit --- 初始化本模块的共享内存
 */
void
SyncScanShmemInit(void)
{
	int			i;
	bool		found;

	scan_locations = (ss_scan_locations_t *)
		ShmemInitStruct("Sync Scan Locations List",
						SizeOfScanLocations(SYNC_SCAN_NELEM),
						&found);

	if (!IsUnderPostmaster)
	{
		/* 初始化共享内存区 */
		Assert(!found);

		scan_locations->head = &scan_locations->items[0];
		scan_locations->tail = &scan_locations->items[SYNC_SCAN_NELEM - 1];

		for (i = 0; i < SYNC_SCAN_NELEM; i++)
		{
			ss_lru_item_t *item = &scan_locations->items[i];

			/*
			 * 用无效值初始化全部槽。随着扫描开始，
			 * 这些无效项会从 LRU 列表上掉下去，并被
			 * 真正的项替换。
			 */
			item->location.relfilelocator.spcOid = InvalidOid;
			item->location.relfilelocator.dbOid = InvalidOid;
			item->location.relfilelocator.relNumber = InvalidRelFileNumber;
			item->location.location = InvalidBlockNumber;

			item->prev = (i > 0) ?
				(&scan_locations->items[i - 1]) : NULL;
			item->next = (i < SYNC_SCAN_NELEM - 1) ?
				(&scan_locations->items[i + 1]) : NULL;
		}
	}
	else
		Assert(found);
}

/*
 * ss_search --- 在 scan_locations 结构中搜索具有给定
 *		relfilelocator 的项。
 *
 * 若 "set" 为真，就把位置更新为给定位置。若找不到
 * 给定 relfilelocator 的项，就在列表头部用给定位置
 * 创建一项，即便 "set" 为假也如此。
 *
 * 无论哪种情况，都返回可能更新之后的位置。
 *
 * 调用方负责已经在该共享
 * 数据结构上取得适当的锁。
 */
static BlockNumber
ss_search(RelFileLocator relfilelocator, BlockNumber location, bool set)
{
	ss_lru_item_t *item;

	item = scan_locations->head;
	for (;;)
	{
		bool		match;

		match = RelFileLocatorEquals(item->location.relfilelocator,
									 relfilelocator);

		if (match || item->next == NULL)
		{
			/*
			 * 若已到达列表末尾且未找到匹配，就接管
			 * 最后一项
			 */
			if (!match)
			{
				item->location.relfilelocator = relfilelocator;
				item->location.location = location;
			}
			else if (set)
				item->location.location = location;

			/* 把该项移到 LRU 列表前端 */
			if (item != scan_locations->head)
			{
				/* 摘除 */
				if (item == scan_locations->tail)
					scan_locations->tail = item->prev;
				item->prev->next = item->next;
				if (item->next)
					item->next->prev = item->prev;

				/* 链入 */
				item->prev = NULL;
				item->next = scan_locations->head;
				scan_locations->head->prev = item;
				scan_locations->head = item;
			}

			return item->location.location;
		}

		item = item->next;
	}

	/* 不会到达 */
}

/*
 * ss_get_location --- 取得扫描的最佳起始位置
 *
 * 返回该关系上一次顺序扫描上次报告的位置，
 * 若找不到有效位置则返回 0。
 *
 * 我们预期调用方刚刚调用过 RelationGetNumberOfBlocks()，
 * 因此传入那个数字，而不是再算一遍。结果
 * 保证小于 relnblocks（假定它 > 0）。
 */
BlockNumber
ss_get_location(Relation rel, BlockNumber relnblocks)
{
	BlockNumber startloc;

	LWLockAcquire(SyncScanLock, LW_EXCLUSIVE);
	startloc = ss_search(rel->rd_locator, 0, false);
	LWLockRelease(SyncScanLock);

	/*
	 * 若该位置对本扫描不是有效块号，就从 0 开始。
	 *
	 * 例如，若保存位置之后 VACUUM 截断了该表，
	 * 就会发生这种情况。
	 */
	if (startloc >= relnblocks)
		startloc = 0;

#ifdef TRACE_SYNCSCAN
	if (trace_syncscan)
		elog(LOG,
			 "SYNC_SCAN: start \"%s\" (size %u) at %u",
			 RelationGetRelationName(rel), relnblocks, startloc);
#endif

	return startloc;
}

/*
 * ss_report_location --- 更新当前扫描位置
 *
 * 向共享的同步扫描状态写入一条形如
 * (relfilelocator, blocknumber) 的项，覆盖同一
 * relfilelocator 的任何已有项。
 */
void
ss_report_location(Relation rel, BlockNumber location)
{
#ifdef TRACE_SYNCSCAN
	if (trace_syncscan)
	{
		if ((location % 1024) == 0)
			elog(LOG,
				 "SYNC_SCAN: scanning \"%s\" at %u",
				 RelationGetRelationName(rel), location);
	}
#endif

	/*
	 * 为减少锁争用，只每隔 N 页报告一次扫描进度。出于
	 * 同样的原因，若锁不能立刻取得就不要阻塞。
	 * 少报几次更新并无大碍，只是意味着想加入这群扫描的
	 * 新扫描会从扫描头部稍微靠后的地方开始。
	 * 希望那些页仍在操作系统缓存中，扫描
	 * 能很快赶上。
	 */
	if ((location % SYNC_SCAN_REPORT_INTERVAL) == 0)
	{
		if (LWLockConditionalAcquire(SyncScanLock, LW_EXCLUSIVE))
		{
			(void) ss_search(rel->rd_locator, location, true);
			LWLockRelease(SyncScanLock);
		}
#ifdef TRACE_SYNCSCAN
		else if (trace_syncscan)
			elog(LOG,
				 "SYNC_SCAN: missed update for \"%s\" at %u",
				 RelationGetRelationName(rel), location);
#endif
	}
}
