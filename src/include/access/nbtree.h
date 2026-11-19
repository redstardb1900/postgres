/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  postgres btree 访问方法实现的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/amapi.h"
#include "access/itup.h"
#include "access/sdir.h"
#include "access/tableam.h"
#include "access/xlogreader.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "lib/stringinfo.h"
#include "storage/bufmgr.h"
#include "storage/shm_toc.h"

/* BTPageOpaqueData 中尚有 16 位清理周期 ID 的空间 */
typedef uint16 BTCycleId;

/*
 *	BTPageOpaqueData -- 在每页末尾，我们存放指向
 *	树中两个兄弟的指针。用于做正向/反向
 *	索引扫描。次页链接对恢复也至关重要：当
 *	搜索因并发页分裂或删除而导航到错误页时；
 *	详见 src/backend/access/nbtree/README。
 *
 *	此外还存放页的 btree 层级（从叶子页的
 *	零向上计数）以及若干指示页类型与状态的
 *	标志位。若页已删除，则在元组区存放
 *	BTDeletedPageData 结构，而标准 BTPageOpaqueData
 *	存放在页特殊区。
 *
 *	我们还存放 "清理周期 ID"。当 VACUUM 处理索引时页被
 *	分裂，则与该次 VACUUM 关联的非零值会写入
 *	分裂后两半页。（若 VACUUM 未在运行，两页都收
 *	到零 cycleid。）这让 VACUUM 能检测自启动后页是
 *	否被分裂，仅有小概率在页恰被上次于
 *	MAX_BT_CYCLE_ID 次 VACUUMs 整数倍前分裂时出现
 *	误匹配。分裂时还清除左（原）页的 BTP_SPLIT_END
 *	标志并在右页置位，但仅当其右侧下一页
 *	具有不同的 cycleid 时。
 *
 *	注意：BTP_LEAF 标志位多余，因可测 level==0
 *	代之
 *
 *	注意：btpo_level 曾为联合类型，以便已删页在
 *	同一字段存放 32 位 safexid。我们现改用
 *	BTDeletedPageData 存放 64 位/完整 safexid。
 */

typedef struct BTPageOpaqueData
{
	BlockNumber btpo_prev;		/* 左兄弟，最左则为 P_NONE */
	BlockNumber btpo_next;		/* 右兄弟，最右则为 P_NONE */
	uint32		btpo_level;		/* 树层级 --- 叶子页为零 */
	uint16		btpo_flags;		/* 标志位，见下 */
	BTCycleId	btpo_cycleid;	/* 最近分裂的清理周期 ID */
} BTPageOpaqueData;

typedef BTPageOpaqueData *BTPageOpaque;

#define BTPageGetOpaque(page) ((BTPageOpaque) PageGetSpecialPointer(page))

/* btpo_flags 中定义的位 */
#define BTP_LEAF		(1 << 0)	/* 叶子页，即非内部页 */
#define BTP_ROOT		(1 << 1)	/* 根页（无父） */
#define BTP_DELETED		(1 << 2)	/* 页已从树中删除 */
#define BTP_META		(1 << 3)	/* 元页 */
#define BTP_HALF_DEAD	(1 << 4)	/* 空，但仍在树中 */
#define BTP_SPLIT_END	(1 << 5)	/* 分裂组的最右页 */
#define BTP_HAS_GARBAGE (1 << 6)	/* 页有 LP_DEAD 元组（已弃用） */
#define BTP_INCOMPLETE_SPLIT (1 << 7)	/* 右兄弟的下行链接缺失 */
#define BTP_HAS_FULLXID	(1 << 8)	/* 含 BTDeletedPageData */

/*
 * cycle ID 允许的最大值略小于 64K。这是为了
 * 方便 pg_filedump 及类似工具：我们想用特殊
 * 空间最后 2 字节作索引类型指示，而限制
 * cycle ID 可让 btree 用该空间存清理周期 ID，
 * 同时仍能识别索引类型。
 */
#define MAX_BT_CYCLE_ID		0xFF7F


/*
 * 元页恒为 btree 索引的第一页。
 * 其主要用途是指向 btree 根页的位置。
 * 我们也指向 "快" 根，即当前有效根；
 * 讨论见 README。
 */

typedef struct BTMetaPageData
{
	uint32		btm_magic;		/* 应含 BTREE_MAGIC */
	uint32		btm_version;	/* nbtree 版本（恒 <= BTREE_VERSION） */
	BlockNumber btm_root;		/* 当前根位置 */
	uint32		btm_level;		/* 根页的树层级 */
	BlockNumber btm_fastroot;	/* 当前 "快" 根位置 */
	uint32		btm_fastlevel;	/* 快根页的树层级 */
	/* 其余字段仅当 btm_version >= BTREE_NOVAC_VERSION 时有效 */

	/* 上次清理时已删且不可回收的页数 */
	uint32		btm_last_cleanup_num_delpages;
	/* 上次清理时的堆元组数（已弃用） */
	float8		btm_last_cleanup_num_heap_tuples;

	bool		btm_allequalimage;	/* 各列皆 "equalimage"? */
} BTMetaPageData;

#define BTPageGetMeta(p) \
	((BTMetaPageData *) PageGetContents(p))

/*
 * 当前 Btree 版本为 4。新建索引时得到的就是
 * 新索引。
 *
 * Btree 版本 3 用于 PostgreSQL v11。它与版本 4
 * 大体相同，但堆 TIDs 尚非键空间的一部分。带
 * 重复键的索引元组可按任意顺序存放。我们继续
 * 支持读写 Btree 版本 2 与 3，使其在 pg_upgrade
 * 后不必立刻重建索引。不过要获得新的
 * heapkeyspace 语义，仍需做一次 REINDEX。
 *
 * 当 btm_allequalimage 字段为 true 时，去重可安全
 * 使用。在版本 3 上读该字段是安全的，但只有
 * 版本 4 索引会用到去重。即便是 PostgreSQL v12
 * 上创建的版本 4 索引，要用去重也需 REINDEX，
 * 因为别无他法把 btm_allequalimage 设为
 * true（pg_upgrade 尚未被教会设置该
 * 元页域）。
 *
 * Btree 版本 2 与版本 3 大体相同。版本 3 在元页
 * 引入了两个新字段。版本 2 元页会在首次插入时
 * 自动升级到版本 3。INCLUDE 索引
 * 无法使用版本 2。
 */
#define BTREE_METAPAGE	0		/* 首页为元页 */
#define BTREE_MAGIC		0x053162	/* 元页中的魔数 */
#define BTREE_VERSION	4		/* 当前版本号 */
#define BTREE_MIN_VERSION	2	/* 最低支持版本 */
#define BTREE_NOVAC_VERSION	3	/* 含全部元字段的版本 */

/*
 * btree 索引项的最大尺寸，含其元组头。
 *
 * 实际上每页须能放下三项，故把任一项
 * 限制为每页可用空间的 1/3。
 *
 * 偶有情况 _bt_truncate() 需放大堆索引元组，
 * 以便为决胜堆 TID 属性腾出空间，
 * 我们在此计入这一点。
 */
#define BTMaxItemSize(page) \
	(MAXALIGN_DOWN((PageGetPageSize(page) - \
					MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
					MAXALIGN(sizeof(BTPageOpaqueData))) / 3) - \
					MAXALIGN(sizeof(ItemPointerData)))
#define BTMaxItemSizeNoHeapTid(page) \
	MAXALIGN_DOWN((PageGetPageSize(page) - \
				   MAXALIGN(SizeOfPageHeaderData + 3*sizeof(ItemIdData)) - \
				   MAXALIGN(sizeof(BTPageOpaqueData))) / 3)

/*
 * MaxTIDsPerBTreePage 是 btree 叶子页上可存堆 TIDs
 * 元组数的上界。用于确定每页临时
 * 缓冲区的大小。
 *
 * 注：为保持简单，此处不计入每元组开销
 * （取值基于单数组需有多少堆 TID 元素才能
 * 填满页头与特殊区之间的空间）。结果该值
 * 略偏高（即更保守），这被认为可以
 * 接受。
 */
#define MaxTIDsPerBTreePage \
	(int) ((BLCKSZ - SizeOfPageHeaderData - sizeof(BTPageOpaqueData)) / \
		   sizeof(ItemPointerData))

/*
 * 叶子页填充因子默认 90%，可由用户调整。
 * 叶子层以上的页使用固定 70% 填充因子。
 * 填充因子在索引构建及分裂最右页时应用；
 * 分裂非最右页时我们尽量均分数据。当分裂
 * 一张整页填满同一值（重复）的页时，有效
 * 叶子页填充因子为 96%，不论该页是否为
 * 最右
 * 页.
 */
#define BTREE_MIN_FILLFACTOR		10
#define BTREE_DEFAULT_FILLFACTOR	90
#define BTREE_NONLEAF_FILLFACTOR	70
#define BTREE_SINGLEVAL_FILLFACTOR	96

/*
 *	一般而言，btree 代码尽量把对页布局的知识
 *	局限在少数例程。但在期望页号之处，我们需要
 *	一个特殊值表示 "无页号"。可用零，因我们
 *	从不需要制作指向元数据页的
 *	指针。
 */

#define P_NONE			0

/*
 * 用于测试某页是否为其树层级上最左或最右页的宏，
 * 以及 opaque 数据中保存的其他状态。
 */
#define P_LEFTMOST(opaque)		((opaque)->btpo_prev == P_NONE)
#define P_RIGHTMOST(opaque)		((opaque)->btpo_next == P_NONE)
#define P_ISLEAF(opaque)		(((opaque)->btpo_flags & BTP_LEAF) != 0)
#define P_ISROOT(opaque)		(((opaque)->btpo_flags & BTP_ROOT) != 0)
#define P_ISDELETED(opaque)		(((opaque)->btpo_flags & BTP_DELETED) != 0)
#define P_ISMETA(opaque)		(((opaque)->btpo_flags & BTP_META) != 0)
#define P_ISHALFDEAD(opaque)	(((opaque)->btpo_flags & BTP_HALF_DEAD) != 0)
#define P_IGNORE(opaque)		(((opaque)->btpo_flags & (BTP_DELETED|BTP_HALF_DEAD)) != 0)
#define P_HAS_GARBAGE(opaque)	(((opaque)->btpo_flags & BTP_HAS_GARBAGE) != 0)
#define P_INCOMPLETE_SPLIT(opaque)	(((opaque)->btpo_flags & BTP_INCOMPLETE_SPLIT) != 0)
#define P_HAS_FULLXID(opaque)	(((opaque)->btpo_flags & BTP_HAS_FULLXID) != 0)

/*
 * BTDeletedPageData 是已删页的页内容
 */
typedef struct BTDeletedPageData
{
	FullTransactionId safexid;	/* 见 BTPageIsRecyclable() */
} BTDeletedPageData;

static inline void
BTPageSetDeleted(Page page, FullTransactionId safexid)
{
	BTPageOpaque opaque;
	PageHeader	header;
	BTDeletedPageData *contents;

	opaque = BTPageGetOpaque(page);
	header = ((PageHeader) page);

	opaque->btpo_flags &= ~BTP_HALF_DEAD;
	opaque->btpo_flags |= BTP_DELETED | BTP_HAS_FULLXID;
	header->pd_lower = MAXALIGN(SizeOfPageHeaderData) +
		sizeof(BTDeletedPageData);
	header->pd_upper = header->pd_special;

	/* 在已删页设 safexid */
	contents = ((BTDeletedPageData *) PageGetContents(page));
	contents->safexid = safexid;
}

static inline FullTransactionId
BTPageGetDeleteXid(Page page)
{
	BTPageOpaque opaque;
	BTDeletedPageData *contents;

	/* 我们仅期望以已删页被调用 */
	Assert(!PageIsNew(page));
	opaque = BTPageGetOpaque(page);
	Assert(P_ISDELETED(opaque));

	/* pg_upgrade 来的已删页 -- 现必可安全删除 */
	if (!P_HAS_FULLXID(opaque))
		return FirstNormalFullTransactionId;

	/* 从已删页取 safexid */
	contents = ((BTDeletedPageData *) PageGetContents(page));
	return contents->safexid;
}

/*
 * 现有页是否可回收？
 *
 * 此函数集中规定哪些已删页现已可安全
 * 重用。不过 _bt_pendingfsm_finalize() 因不直接
 * 处理页而复制了部分相同逻辑 -- 二者须保持同步。
 *
 * 注：PageIsNew() 页恒可安全回收，但此处无法
 * 处理（调用者自行负责该情形）。调用者或许
 * 本来就需对新页做特殊处理。
 */
static inline bool
BTPageIsRecyclable(Page page, Relation heaprel)
{
	BTPageOpaque opaque;

	Assert(!PageIsNew(page));
	Assert(heaprel != NULL);

	/* 仅当页已删且 safexid 足够旧时可回收 */
	opaque = BTPageGetOpaque(page);
	if (P_ISDELETED(opaque))
	{
		FullTransactionId safexid = BTPageGetDeleteXid(page);

		/*
		 * 页已删除，但是何时？若刚删，某扫描可能
		 * 已见过指向它的下行链接，稍后还会读该页。
		 * 只要仍可能发生，就必须把已删页留作
		 * 墓碑。
		 *
		 * 为此检查删除 XID 是否仍可能对任何人
		 * 可见。若否，则仍在进行的扫描不可能见过
		 * 其下行链接，便可回收它。
		 */
		return GlobalVisCheckRemovableFullXid(heaprel, safexid);
	}

	return false;
}

/*
 * BTVacState 与 BTPendingFSM 是 VACUUM 期间 nbtree.c 的私有状态。
 * 导出供 nbtpage.c 中与页删除相关的代码使用。
 */
typedef struct BTPendingFSM
{
	BlockNumber target;			/* 当前 VACUUM 删除的页 */
	FullTransactionId safexid;	/* 页的 BTDeletedPageData.safexid */
} BTPendingFSM;

typedef struct BTVacState
{
	IndexVacuumInfo *info;
	IndexBulkDeleteResult *stats;
	IndexBulkDeleteCallback callback;
	void	   *callback_state;
	BTCycleId	cycleid;
	MemoryContext pagedelcontext;

	/*
	 * _bt_pendingfsm_finalize()状态
	 */
	int			bufsize;		/* pendingpages 空间（元素数） */
	int			maxbufsize;		/* 遵守 work_mem 的最大 bufsize */
	BTPendingFSM *pendingpages; /* 每个新删页一条 */
	int			npendingpages;	/* 当前有效 pendingpages数 */
} BTVacState;

/*
 *	Lehman 与 Yao 的算法要求每个非最右页有一条 "高键"。
 *	高键不是用来访问堆的元组。它是一条
 *	枢轴元组（定义见下文 "B-Tree 元组格式说明"）。
 *	页上的高键必须大于或等于该页上出现的任何
 *	其他键。若发现要插入严格大于高键的键，
 *	就知道需要右移（这只应在我们检查了
 *	父页之后该页被分裂时才会
 *	发生）。
 *
 *	我们的插入算法保证可用右兄弟上最初的最小键
 *	作为高键。页一旦创建，其高键仅在页被
 *	分裂时才会改变。
 *
 *	在非最右页上，高键位于第 1 项，数据项从
 *	第 2 项开始。最右页无高键，故数据项从
 *	第 1 项起存放。
 */

#define P_HIKEY				((OffsetNumber) 1)
#define P_FIRSTKEY			((OffsetNumber) 2)
#define P_FIRSTDATAKEY(opaque)	(P_RIGHTMOST(opaque) ? P_HIKEY : P_FIRSTKEY)

/*
 * 关于 B-Tree 元组格式，以及键与非键属性：
 *
 * INCLUDE B-Tree 索引有非键属性。这些是可由
 * 仅索引扫描返回的额外属性，但不影响索引中
 * 项的次序（形式上，非键属性不算键空间的
 * 一部分）。非键属性仅存在于项指针真正指向
 * 堆元组的叶子索引元组中（非枢轴元组）。
 * _bt_check_natts() 强制执行此处
 * 所述规则。
 *
 * 非枢轴元组格式（普通/非倒排）：
 *
 *  t_tid | t_info | 键值 | INCLUDE 列（若有）
 *
 * t_tid 指向堆 TID，自 BTREE_VERSION 4 起它是决胜
 * 键列。
 *
 * 非枢轴元组与仅有键列的枢轴元组互为补充。
 * 枢轴元组的唯一用途是表示键空间如何被
 * 分隔。一般地，层数多于一层的任何 B-Tree 索引
 * （即并非仅由元页与单个叶子根页构成的
 * 索引）必须有若干枢轴元组，因为枢轴元组
 * 用于遍历树。后缀截断可在形成新枢轴时省略
 * 尾部键列，使其逻辑值为负无穷。因
 * BTREE_VERSION 4 索引把堆 TID 当作尾部键列以确保
 * 所有索引元组物理唯一，故有必要在枢轴元组中
 * 把堆 TID 表示为尾部键列，不过它常常可像
 * 其他键列一样被截断掉。（实际上，堆 TID 是
 * 省略而非截断，因其表示法
 * 不同于非枢轴
 * 表示。）
 *
 * 枢轴元组格式:
 *
 *  t_tid | t_info | 键值 | [堆 TID]
 *
 * 我们通过滥用枢轴元组的 t_tid 偏移字段来存放
 * 其中的列数，因枢轴元组从不需要存真实偏移
 * （不过通常在 t_tid 中存下行链接）。仅当
 * INDEX_ALT_TID_MASK 位置位时，偏移字段才存放
 * 列/属性数目，且不计入有时存在于枢轴元组中的
 * 尾部堆 TID 列 -- 那由 BT_PIVOT_HEAP_TID_ATTR 的
 * 存在表示。BTREE_VERSION 4 枢轴元组的 t_info 中
 * INDEX_ALT_TID_MASK 位恒置位，因
 * BTreeTupleIsPivot() 须在 heapkeyspace 版本上可靠工作。
 *
 * 在版本 2 或 3（!heapkeyspace）索引中，枢轴元组里
 * INDEX_ALT_TID_MASK 可能未置位。结果
 * BTreeTupleIsPivot() 将不可靠。所存列数隐含地与
 * 索引列数相同，一如任何非枢轴元组。（所存列数
 * 不应变化，因在任何 !heapkeyspace 索引中对键列做
 * 后缀截断都不安全。）
 *
 * t_tid 偏移号的最低 12 位用于
 * 表示枢轴元组中的键列数目。这留下 4 个状态位
 * （BT_STATUS_OFFSET_MASK 位），由所有置了
 * INDEX_ALT_TID_MASK 位（在 t_info 中）的元组共享，以存放
 * 基本元组元数据。BTreeTupleIsPivot() 与 BTreeTupleIsPosting()
 * 使用这些位。
 *
 * 有时非枢轴元组也使用把 t_tid 改作存放元数据
 * 而非 TID 的表示。PostgreSQL v13 引入了新的非枢轴
 * 元组格式以支持去重：倒排列表
 * 元组。去重把多个相等的非枢轴元组合并成
 * 逻辑等价且节省空间的表示。倒排列表是
 * ItemPointerData 元素的数组。非枢轴元组被惰性
 * 合并成倒排列表元组，发生在否则就得
 * 分裂叶子页之时。
 *
 * 倒排列表元组格式（非枢轴元组的替代表示）：
 *
 *  t_tid | t_info | 键值 | 倒排列表（TID 数组）
 *
 * 倒排列表元组的识别方式是：t_info 中置了
 * INDEX_ALT_TID_MASK 状态位，且 t_tid 偏移号中置了
 * BT_IS_POSTING 状态位。这些标志把倒排列表元组的
 * t_tid 内容重新定义为存放倒排列表位置
 * （而非块号），以及元组中堆 TIDs 的总数
 * （而非真实偏移号）。
 *
 * t_tid 偏移号的最低 12 位用于
 * 表示元组中存在的堆 TID 数目，留下 4 个状态位
 * （BT_STATUS_OFFSET_MASK 位）。与任何非枢轴元组一样，
 * 所存列数恒隐含为索引中的总列数
 * （实践中永不会存非键列，因 INCLUDE 索引不支持
 * 去重）。
 */
#define INDEX_ALT_TID_MASK			INDEX_AM_RESERVED_BIT

/* 项指针偏移位掩码 */
#define BT_OFFSET_MASK				0x0FFF
#define BT_STATUS_OFFSET_MASK		0xF000
/* BT_STATUS_OFFSET_MASK 状态位 */
#define BT_PIVOT_HEAP_TID_ATTR		0x1000
#define BT_IS_POSTING				0x2000

/*
 * 为索引元组中键数目分配的掩码须能容纳
 * 索引属性的最大可能数目
 */
StaticAssertDecl(BT_OFFSET_MASK >= INDEX_MAX_KEYS,
				 "BT_OFFSET_MASK can't fit INDEX_MAX_KEYS");

/*
 * 注：BTreeTupleIsPivot() 在 !heapkeyspace 索引上可能有
 * 假阴性（但不会有假阳性）
 */
static inline bool
BTreeTupleIsPivot(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* 偏移号中无 BT_IS_POSTING 表示枢轴元组 */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & BT_IS_POSTING) != 0)
		return false;

	return true;
}

static inline bool
BTreeTupleIsPosting(IndexTuple itup)
{
	if ((itup->t_info & INDEX_ALT_TID_MASK) == 0)
		return false;
	/* 偏移号中有 BT_IS_POSTING 表示倒排列表元组 */
	if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) & BT_IS_POSTING) == 0)
		return false;

	return true;
}

static inline void
BTreeTupleSetPosting(IndexTuple itup, uint16 nhtids, int postingoffset)
{
	Assert(nhtids > 1);
	Assert((nhtids & BT_STATUS_OFFSET_MASK) == 0);
	Assert((size_t) postingoffset == MAXALIGN(postingoffset));
	Assert(postingoffset < INDEX_SIZE_MASK);
	Assert(!BTreeTupleIsPivot(itup));

	itup->t_info |= INDEX_ALT_TID_MASK;
	ItemPointerSetOffsetNumber(&itup->t_tid, (nhtids | BT_IS_POSTING));
	ItemPointerSetBlockNumber(&itup->t_tid, postingoffset);
}

static inline uint16
BTreeTupleGetNPosting(IndexTuple posting)
{
	OffsetNumber existing;

	Assert(BTreeTupleIsPosting(posting));

	existing = ItemPointerGetOffsetNumberNoCheck(&posting->t_tid);
	return (existing & BT_OFFSET_MASK);
}

static inline uint32
BTreeTupleGetPostingOffset(IndexTuple posting)
{
	Assert(BTreeTupleIsPosting(posting));

	return ItemPointerGetBlockNumberNoCheck(&posting->t_tid);
}

static inline ItemPointer
BTreeTupleGetPosting(IndexTuple posting)
{
	return (ItemPointer) ((char *) posting +
						  BTreeTupleGetPostingOffset(posting));
}

static inline ItemPointer
BTreeTupleGetPostingN(IndexTuple posting, int n)
{
	return BTreeTupleGetPosting(posting) + n;
}

/*
 * 取/设枢轴元组下行链接块号。
 *
 * 注：无法断言该元组是枢轴元组。若如此则
 * !heapkeyspace 索引会出现假阳性断言失败。
 */
static inline BlockNumber
BTreeTupleGetDownLink(IndexTuple pivot)
{
	return ItemPointerGetBlockNumberNoCheck(&pivot->t_tid);
}

static inline void
BTreeTupleSetDownLink(IndexTuple pivot, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&pivot->t_tid, blkno);
}

/*
 * 取元组内的属性数目。
 *
 * 注意这不包括任何隐式决胜堆 TID 属性
 * （若有）。另请注意，键属性数目必须在所有
 * heapkeyspace 枢轴元组中显式表示。
 *
 * 注：定义为宏而非内联函数，为
 * 以免包含 rel.h。
 */
#define BTreeTupleGetNAtts(itup, rel)	\
	( \
		(BTreeTupleIsPivot(itup)) ? \
		( \
			ItemPointerGetOffsetNumberNoCheck(&(itup)->t_tid) & BT_OFFSET_MASK \
		) \
		: \
		IndexRelationGetNumberOfAttributes(rel) \
	)

/*
 * 设置元组中的键属性数目。
 *
 * 此处亦可设置堆 TID 决胜属性位，表示将在
 * 元组末尾存放堆 TID 值（即采用特殊的
 * 枢轴元组表示法）。
 */
static inline void
BTreeTupleSetNAtts(IndexTuple itup, uint16 nkeyatts, bool heaptid)
{
	Assert(nkeyatts <= INDEX_MAX_KEYS);
	Assert((nkeyatts & BT_STATUS_OFFSET_MASK) == 0);
	Assert(!heaptid || nkeyatts > 0);
	Assert(!BTreeTupleIsPivot(itup) || nkeyatts == 0);

	itup->t_info |= INDEX_ALT_TID_MASK;

	if (heaptid)
		nkeyatts |= BT_PIVOT_HEAP_TID_ATTR;

	/* 此处故意不置 BT_IS_POSTING 位 */
	ItemPointerSetOffsetNumber(&itup->t_tid, nkeyatts);
	Assert(BTreeTupleIsPivot(itup));
}

/*
 * 自叶子页高键取/置其"顶父页"链接。用于页
 * 删除。
 *
 * 注：无法断言该元组是枢轴元组。若如此则
 * !heapkeyspace 索引会出现假阳性断言失败。
 */
static inline BlockNumber
BTreeTupleGetTopParent(IndexTuple leafhikey)
{
	return ItemPointerGetBlockNumberNoCheck(&leafhikey->t_tid);
}

static inline void
BTreeTupleSetTopParent(IndexTuple leafhikey, BlockNumber blkno)
{
	ItemPointerSetBlockNumber(&leafhikey->t_tid, blkno);
	BTreeTupleSetNAtts(leafhikey, 0, false);
}

/*
 * 取决胜堆 TID 属性（若有）。
 *
 * 若为倒排列表元组，则返回其中首个/最低的堆 TID。
 */
static inline ItemPointer
BTreeTupleGetHeapTID(IndexTuple itup)
{
	if (BTreeTupleIsPivot(itup))
	{
		/* 枢轴元组的堆 TID 表示？ */
		if ((ItemPointerGetOffsetNumberNoCheck(&itup->t_tid) &
			 BT_PIVOT_HEAP_TID_ATTR) != 0)
			return (ItemPointer) ((char *) itup + IndexTupleSize(itup) -
								  sizeof(ItemPointerData));

		/* 堆 TID 属性已被截断 */
		return NULL;
	}
	else if (BTreeTupleIsPosting(itup))
		return BTreeTupleGetPosting(itup);

	return &itup->t_tid;
}

/*
 * 取最大堆 TID 属性；对无倒排列表的非枢轴
 * 元组而言，它也可能是唯一的 TID。
 *
 * 仅适用于非枢轴元组。
 */
static inline ItemPointer
BTreeTupleGetMaxHeapTID(IndexTuple itup)
{
	Assert(!BTreeTupleIsPivot(itup));

	if (BTreeTupleIsPosting(itup))
	{
		uint16		nposting = BTreeTupleGetNPosting(itup);

		return BTreeTupleGetPostingN(itup, nposting - 1);
	}

	return &itup->t_tid;
}

/*
 *	B-tree 的操作符策略号已移至 access/stratnum.h，
 *	因多处需在 ScanKeyInit() 调用中使用它们。
 *
 *	策略号的选取使得我们能用减法来交换它们，
 *	如下：
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	声明新操作符类时，我们要求用户向我们
 *	提供 amproc 过程 (BTORDER_PROC)，用以判定
 *	两键 a、b 是 a < b、a = b 还是 a > b。该例程
 *	须分别在这三种情形返回 < 0、0、> 0。
 *
 *	为便于加速排序，操作符类可选择再提供
 *	第二个过程 (BTSORTSUPPORT_PROC)。详情见
 *	src/include/utils/sortsupport.h
 *
 *	为支持由 "RANGE offset PRECEDING/FOLLOWING" 定义的窗框，
 *	操作符类可选择再提供第三个 amproc 过程
 *	(BTINRANGE_PROC)，与是否提供 sortsupport 无关。
 *	详情见 doc/src/sgml/btree.sgml。
 *
 *	为便于 B-Tree 去重，操作符类可选择再提供
 *	第四个 amproc 过程 (BTEQUALIMAGE_PROC)。详情见
 *	doc/src/sgml/btree.sgml。
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2
#define BTINRANGE_PROC		3
#define BTEQUALIMAGE_PROC	4
#define BTOPTIONS_PROC		5
#define BTNProcs			5

/*
 *	我们需要能区分对页的读请求与写
 *	请求，以便正确加锁。
 */

#define BT_READ			BUFFER_LOCK_SHARE
#define BT_WRITE		BUFFER_LOCK_EXCLUSIVE

/*
 * BTStackData -- 沿树下行时，把即将跟随其下行链接的
 * 枢轴元组位置压入私有栈。若我们分裂了
 * 叶子页，就用此栈沿树回走，并在父页正确位置
 * 插入数据。若父页随后也分裂，还须
 * 递归插入祖父页。私有栈可能因并发的页
 * 分裂与页删除而过时，但它绝不应给出
 * 无可挽回的
 * 坏图景。
 */
typedef struct BTStackData
{
	BlockNumber bts_blkno;
	OffsetNumber bts_offset;
	struct BTStackData *bts_parent;
} BTStackData;

typedef BTStackData *BTStack;

/*
 * BTScanInsertData 是 btree 私有状态，用于寻找索引扫描的
 * 初始位置，或插入新元组——即"插入
 * 扫描键"（勿与搜索扫描键混淆）。用以
 * _bt_search 沿树下行。
 *
 * heapkeyspace 表示是否期望索引中全部键在物理上
 * 唯一（因堆 TID 用作决胜属性），且枢轴元组中可能
 * 有被截断的键属性。这其实是索引关系本身的
 * 属性（而非某次索引扫描）。heapkeyspace 索引即
 * 版本 >= 4 的索引。把该标志放在近旁很方便，
 * 以免反复访问元页。
 *
 * allequalimage 置位表示该索引可安全去重。
 * 这也是索引关系的属性，而非某次索引扫描。
 *
 * anynullkeys 表示从索引元组构造扫描键时是否有键为
 * NULL（注意：已截断的元组键属性会以 NULL 作为
 * 占位键值，这也会影响 anynullkeys
 * 的取值）。这是唯一索引非枢轴元组插入的便利：
 * 通常会临时清除 scantid，但当 anynullkeys 为真时
 * 不应如此。取值一般与非枢轴元组的 HasNulls
 * 位相符，但插入 INCLUDE 索引时可能不符（元组头取值
 * 受键与非键属性是否为 NULL 共同影响）。
 *
 * 当 nextkey 为假（常见）时，_bt_search 与 _bt_binsrch 将
 * 定位首个 >= scankey 的项。当 nextkey 为真时，它们将定位
 * 首个 > 扫描键项。
 *
 * 调用者若要用由叶子页高键构造的扫描键重新找到
 * 叶子页，就把 pivotsearch 置为真。多数调用者将其置
 * 为假。
 *
 * scantid 是用作最终决胜属性的堆 TID。当索引扫描
 * 无需为某个具体物理元组定位时，将其置为
 * NULL。向 heapkeyspace 索引插入新元组时必须设置，
 * 因为树中每个元组都明确属于一个确切位置
 * （!heapkeyspace 索引则从不设置）。
 * 尽管表示形式不同，nbtree 搜索代码仍把
 * scantid 视为又一个插入扫描键属性。
 *
 * scankeys 是在 scantid 之前比较的属性的扫描键
 * 条目数组（用户可见属性）。keysz 是该数组大小。
 * 插入时每个属性都必须有扫描键，但启动普通
 * 索引扫描时可省略部分。该数组用作柔性数组成员，
 * 但其定长方式使得仍可在栈上分配。完整细节见
 * nbtree/README。
 */
typedef struct BTScanInsertData
{
	bool		heapkeyspace;
	bool		allequalimage;
	bool		anynullkeys;
	bool		nextkey;
	bool		pivotsearch;
	ItemPointer scantid;		/* scankeys 的决胜 */
	int			keysz;			/* scankeys 数组大小 */
	ScanKeyData scankeys[INDEX_MAX_KEYS];	/* 必须最后 */
} BTScanInsertData;

typedef BTScanInsertData *BTScanInsert;

/*
 * BTInsertStateData 是插入期间使用的工作区。
 *
 * 沿树下行至新元组可能所在的首个叶子页后填入。
 * 在执行唯一性检查时跟踪当前位置，
 * 在确定要插入的确切页之前。
 *
 *
 * （本应私有于 nbtinsert.c，但 _bt_binsrch_insert
 * 也会用到）
 */
typedef struct BTInsertStateData
{
	IndexTuple	itup;			/* 正在插入的项 */
	Size		itemsz;			/* itup 大小 -- 应已 MAXALIGN() */
	BTScanInsert itup_key;		/* 插入扫描键 */

	/* 很可能插入 itup 的叶子页所在缓冲区 */
	Buffer		buf;

	/*
	 * 当前缓冲区内的边界缓存。仅用于会调用
	 * _bt_check_unique 的插入。详见 _bt_binsrch_insert 与
	 * _bt_findinsertloc。
	 */
	bool		bounds_valid;
	OffsetNumber low;
	OffsetNumber stricthigh;

	/*
	 * 若 _bt_binsrch_insert 在现有倒排列表内找到位置，
	 * 则保存列表内偏移。哨兵值 -1 表示与带
	 * LP_DEAD 位的现有倒排列表元组重叠。
	 */
	int			postingoff;
} BTInsertStateData;

typedef BTInsertStateData *BTInsertState;

/*
 * 去重期间表示单个待定元组所用的
 * 状态。
 */
typedef struct BTDedupInterval
{
	OffsetNumber baseoff;
	uint16		nitems;
} BTDedupInterval;

/*
 * BTDedupStateData 是去重期间使用的工作区。
 *
 * 状态信息字段跟踪整页去重遍次的状态。
 * 当前待定倒排列表的状态也被跟踪。
 *
 * 待定倒排列表由页上自页偏移号 'baseoff' 起的
 * 一组连续相等项组成。这是新倒排列表的
 * 基元组偏移号。'nitems' 是将合并进新倒排列表
 * 元组的现有项总数，含基元组项。（现有项
 * 本身可以是倒排列表元组，或普通
 * 非枢轴元组。）
 *
 * 处理待定倒排列表时将释放的现有元组总大小
 * 由 'phystupsize' 跟踪。该信息使去重能为
 * 每个新倒排列表元组以及整页遍次整体
 * 计算空间节省。
 */
typedef struct BTDedupStateData
{
	/* 整页遍次的去重状态信息 */
	bool		deduplicate;	/* 仍在对页去重？ */
	int			nmaxitems;		/* 迄今最大尺寸元组数目 */
	Size		maxpostingsize; /* 最终元组大小上限 */

	/* 当前待定倒排列表基元组的元数据 */
	IndexTuple	base;			/* 用于构成新倒排列表 */
	OffsetNumber baseoff;		/* 基的页偏移 */
	Size		basetupsize;	/* 不含原倒排列表的基大小 */

	/* 待定倒排列表的其他元数据 */
	ItemPointer htids;			/* 待定倒排列表中的堆 TIDs */
	int			nhtids;			/* htids 数组中的堆 TIDs 数 */
	int			nitems;			/* 现有元组/行指针数目 */
	Size		phystupsize;	/* 含行指针开销 */

	/*
	 * 将放入新版页的元组数组。每个连续项组
	 * 占一个条目。注意：不会变成倒排列表元组的
	 * 现有元组不出现在数组中（去重遍次对它们
	 * 隐含地保持不变）。
	 */
	int			nintervals;		/* 数组中当前区间数目 */
	BTDedupInterval intervals[MaxIndexTuplesPerPage];
} BTDedupStateData;

typedef BTDedupStateData *BTDedupState;

/*
 * BTVacuumPostingData 描述如何对倒排列表元组做
 * VACUUM（或删除）——当其部分（非全部）TIDs 将被
 * 删除.
 *
 * 约定：输入时 itup 是原倒排列表元组，
 * 输出时是用于覆写现有元组的 palloc() 最终元组。
 */
typedef struct BTVacuumPostingData
{
	/* 将要/已被更新的元组 */
	IndexTuple	itup;
	OffsetNumber updatedoffset;

	/* 在 WAL 中描述最终 itup 所需状态 */
	uint16		ndeletedtids;
	uint16		deletetids[FLEXIBLE_ARRAY_MEMBER];
} BTVacuumPostingData;

typedef BTVacuumPostingData *BTVacuumPosting;

/*
 * BTScanOpaqueData 是索引扫描所需的 btree 私有状态。
 * 包括预处理后的扫描键（预处理细节见
 * _bt_preprocess_keys()）、关于扫描当前位置的
 * 信息，以及（若有）标记位置的信息。（我们用
 * BTScanPosData 表示当前位置与标记位置各自所需的
 * 数据。）此外还可记住若干已知已杀的索引项，
 * 离开当前页前必须把它们标为已杀。
 *
 * 索引扫描按页进行：钉住并读锁该页，识别页上
 * 全部匹配项并保存到 BTScanPosData，然后在向调用者
 * 返回各项以供处理时释放读锁。此法使加锁/
 * 解锁流量最少。注意：我们会保持索引页上的钉住，
 * 直到调用者处理完全部项（VACUUM 同步需要，见
 * nbtree/README）。准备迈向下一页时，若调用者告知
 * 有项被杀死，我们再锁该页将其标为已杀，然后
 * 解锁。最后放下钉住，并沿适当方向迈向
 * 下一页。
 *
 *
 * 若做仅索引扫描，则为每个匹配项保存整个
 * IndexTuple，否则只保存其堆 TID 与偏移。IndexTuples 放入
 * 单独的工作区数组；每个 BTScanPosItem 存放其元组在
 * 该数组中的偏移。倒排列表元组只存一次"基"元组，
 * 使列表内每个 TID 都能返回同一键。
 *
 */

typedef struct BTScanPosItem	/* 每次匹配所记住的内容 */
{
	ItemPointerData heapTid;	/* 所引用堆项的 TID */
	OffsetNumber indexOffset;	/* 索引项在页内的位置 */
	LocationIndex tupleOffset;	/* 工作区中 IndexTuple 偏移(若有) */
} BTScanPosItem;

typedef struct BTScanPosData
{
	Buffer		buf;			/* 若有效则缓冲区已钉住 */

	XLogRecPtr	lsn;			/* 读页时在 WAL 流中的位置 */
	BlockNumber currPage;		/* items 数组所引用的页 */
	BlockNumber nextPage;		/* 扫描时该页的右链接 */

	/*
	 * moreLeft 与 moreRight 分别跟踪我们是否认为当前页
	 * 左侧与右侧可能还有匹配的索引项。
	 * 当 _bt_checkkeys() 返回 continuescan = false 时，可清除
	 * 相应的标志。
	 */
	bool		moreLeft;
	bool		moreRight;

	/*
	 * 若做仅索引扫描，nextTupleOffset 是关联元组存储
	 * 工作区中的首个空闲位置。
	 */
	int			nextTupleOffset;

	/*
	 * items 数组恒按索引序排列（即 indexoffset
	 * 递增）。反向扫描时便于自后向前填充，
	 * 故从最后一个槽位起向下填。
	 * 因此需要首个有效条目与末个有效条目两个计数。
	 * itemIndex 是游标，标出上次返回给调用者的条目。
	 */
	int			firstItem;		/* items[]首个有效下标 */
	int			lastItem;		/* items[]末个有效下标 */
	int			itemIndex;		/* items[]当前下标 */

	BTScanPosItem items[MaxTIDsPerBTreePage];	/* 必须最后 */
} BTScanPosData;

typedef BTScanPosData *BTScanPos;

#define BTScanPosIsPinned(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BufferIsValid((scanpos).buf) \
)
#define BTScanPosUnpin(scanpos) \
	do { \
		ReleaseBuffer((scanpos).buf); \
		(scanpos).buf = InvalidBuffer; \
	} while (0)
#define BTScanPosUnpinIfPinned(scanpos) \
	do { \
		if (BTScanPosIsPinned(scanpos)) \
			BTScanPosUnpin(scanpos); \
	} while (0)

#define BTScanPosIsValid(scanpos) \
( \
	AssertMacro(BlockNumberIsValid((scanpos).currPage) || \
				!BufferIsValid((scanpos).buf)), \
	BlockNumberIsValid((scanpos).currPage) \
)
#define BTScanPosInvalidate(scanpos) \
	do { \
		(scanpos).currPage = InvalidBlockNumber; \
		(scanpos).nextPage = InvalidBlockNumber; \
		(scanpos).buf = InvalidBuffer; \
		(scanpos).lsn = InvalidXLogRecPtr; \
		(scanpos).nextTupleOffset = 0; \
	} while (0)

/* 每个相等型 SK_SEARCHARRAY 扫描键都需要一个 */
typedef struct BTArrayKeyInfo
{
	int			scan_key;		/* arrayKeyData 中关联键的下标 */
	int			cur_elem;		/* elem_values 中当前元素的下标 */
	int			mark_elem;		/* elem_values 中标记元素下标 */
	int			num_elems;		/* 当前数组值中的元素数目 */
	Datum	   *elem_values;	/* num_elems 个 Datum */
} BTArrayKeyInfo;

typedef struct BTScanOpaqueData
{
	/* 除 arraysStarted 外均由 _bt_preprocess_keys() 设置： */
	bool		qual_ok;		/* 若限定永无法满足则为假 */
	bool		arraysStarted;	/* 已启动数组键，但尚未"越过
								 * 全部数组之末"？*/
	int			numberOfKeys;	/* 预处理后的扫描键数目 */
	ScanKey		keyData;		/* 预处理后的扫描键数组 */

	/* SK_SEARCHARRAY 支持用的工作区 */
	ScanKey		arrayKeyData;	/* scan->keyData 的修改副本 */
	int			numArrayKeys;	/* 相等型数组键数目（若有无法
								 * 满足的数组键则为 -1）*/
	int			arrayKeyCount;	/* 表示已处理的数组扫描键数目的
								 * 计数*/
	BTArrayKeyInfo *arrayKeys;	/* 每个相等型数组键的信息 */
	MemoryContext arrayContext; /* 数组数据的扫描期上下文 */

	/* 已杀项信息(killedItems未用则为NULL) */
	int		   *killedItems;	/* currPos.items中已杀项下标 */
	int			numKilled;		/* 当前已存项的数目 */

	/*
	 * 若做仅索引扫描，这些是 currPos 与 markPos
	 * 各自的元组存储工作区。每个大小为
	 * BLCKSZ，故可容纳整页份量的元组。
	 */
	char	   *currTuples;		/* currPos 的元组存储 */
	char	   *markTuples;		/* markPos 的元组存储 */

	/*
	 * 若标记位置与当前位置在同一页，则
	 * 不用 markPos，只把已标记的 itemIndex 保存在 markItemIndex
	 * （currPos 其余部分对标记位置亦有效）。因此，要
	 * 判定是否有标记，先看 markItemIndex，再看
	 * markPos.
	 */
	int			markItemIndex;	/* itemIndex，无效则为 -1 */

	/* 为效率把这些放在结构体末尾 */
	BTScanPosData currPos;		/* 当前位置数据 */
	BTScanPosData markPos;		/* 标记位置(若有) */
} BTScanOpaqueData;

typedef BTScanOpaqueData *BTScanOpaque;

/*
 * 我们在预处理扫描键中使用若干私有 sk_flags 位。允许
 * 使用位 16-31（见 skey.h）。最高若干位从该索引属性的
 * indoption[] 数组项复制而来。
 */
#define SK_BT_REQFWD	0x00010000	/* 继续正向扫描所需 */
#define SK_BT_REQBKWD	0x00020000	/* 继续反向扫描所需 */
#define SK_BT_INDOPTION_SHIFT  24	/* 必须清除上述各位 */
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

typedef struct BTOptions
{
	int32		varlena_header_;	/* varlena 头（勿直接触碰！） */
	int			fillfactor;		/* 页填充因子百分数(0..100) */
	float8		vacuum_cleanup_index_scale_factor;	/* 已弃用 */
	bool		deduplicate_items;	/* 尝试对项去重？ */
} BTOptions;

#define BTGetFillFactor(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BTREE_AM_OID), \
	 (relation)->rd_options ? \
	 ((BTOptions *) (relation)->rd_options)->fillfactor : \
	 BTREE_DEFAULT_FILLFACTOR)
#define BTGetTargetPageFreeSpace(relation) \
	(BLCKSZ * (100 - BTGetFillFactor(relation)) / 100)
#define BTGetDeduplicateItems(relation) \
	(AssertMacro(relation->rd_rel->relkind == RELKIND_INDEX && \
				 relation->rd_rel->relam == BTREE_AM_OID), \
	((relation)->rd_options ? \
	 ((BTOptions *) (relation)->rd_options)->deduplicate_items : true))

/*
 * 进度报告用的常量定义。阶段号必须与 btbuildphasename
 * 相符。
 */
/* PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE 为 1(见 progress.h) */
#define PROGRESS_BTREE_PHASE_INDEXBUILD_TABLESCAN		2
#define PROGRESS_BTREE_PHASE_PERFORMSORT_1				3
#define PROGRESS_BTREE_PHASE_PERFORMSORT_2				4
#define PROGRESS_BTREE_PHASE_LEAF_LOAD					5

/*
 * btree 外部入口点，在 nbtree.c
 */
extern void btbuildempty(Relation index);
extern bool btinsert(Relation rel, Datum *values, bool *isnull,
					 ItemPointer ht_ctid, Relation heapRel,
					 IndexUniqueCheck checkUnique,
					 bool indexUnchanged,
					 struct IndexInfo *indexInfo);
extern IndexScanDesc btbeginscan(Relation rel, int nkeys, int norderbys);
extern Size btestimateparallelscan(void);
extern void btinitparallelscan(void *target);
extern bool btgettuple(IndexScanDesc scan, ScanDirection dir);
extern int64 btgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void btrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					 ScanKey orderbys, int norderbys);
extern void btparallelrescan(IndexScanDesc scan);
extern void btendscan(IndexScanDesc scan);
extern void btmarkpos(IndexScanDesc scan);
extern void btrestrpos(IndexScanDesc scan);
extern IndexBulkDeleteResult *btbulkdelete(IndexVacuumInfo *info,
										   IndexBulkDeleteResult *stats,
										   IndexBulkDeleteCallback callback,
										   void *callback_state);
extern IndexBulkDeleteResult *btvacuumcleanup(IndexVacuumInfo *info,
											  IndexBulkDeleteResult *stats);
extern bool btcanreturn(Relation index, int attno);

/*
 * nbtree.c 内部函数原型
 */
extern bool _bt_parallel_seize(IndexScanDesc scan, BlockNumber *pageno);
extern void _bt_parallel_release(IndexScanDesc scan, BlockNumber scan_page);
extern void _bt_parallel_done(IndexScanDesc scan);
extern void _bt_parallel_advance_array_keys(IndexScanDesc scan);

/*
 * nbtdedup.c 中函数的原型
 */
extern void _bt_dedup_pass(Relation rel, Buffer buf, IndexTuple newitem,
						   Size newitemsz, bool bottomupdedup);
extern bool _bt_bottomupdel_pass(Relation rel, Buffer buf, Relation heapRel,
								 Size newitemsz);
extern void _bt_dedup_start_pending(BTDedupState state, IndexTuple base,
									OffsetNumber baseoff);
extern bool _bt_dedup_save_htid(BTDedupState state, IndexTuple itup);
extern Size _bt_dedup_finish_pending(Page newpage, BTDedupState state);
extern IndexTuple _bt_form_posting(IndexTuple base, ItemPointer htids,
								   int nhtids);
extern void _bt_update_posting(BTVacuumPosting vacposting);
extern IndexTuple _bt_swap_posting(IndexTuple newitem, IndexTuple oposting,
								   int postingoff);

/*
 * nbtinsert.c 中函数的原型
 */
extern bool _bt_doinsert(Relation rel, IndexTuple itup,
						 IndexUniqueCheck checkUnique, bool indexUnchanged,
						 Relation heapRel);
extern void _bt_finish_split(Relation rel, Relation heaprel, Buffer lbuf,
							 BTStack stack);
extern Buffer _bt_getstackbuf(Relation rel, Relation heaprel, BTStack stack,
							  BlockNumber child);

/*
 * nbtsplitloc.c 中函数的原型
 */
extern OffsetNumber _bt_findsplitloc(Relation rel, Page origpage,
									 OffsetNumber newitemoff, Size newitemsz, IndexTuple newitem,
									 bool *newitemonleft);

/*
 * nbtpage.c 中函数的原型
 */
extern void _bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level,
							 bool allequalimage);
extern bool _bt_vacuum_needs_cleanup(Relation rel);
extern void _bt_set_cleanup_info(Relation rel, BlockNumber num_delpages);
extern void _bt_upgrademetapage(Page page);
extern Buffer _bt_getroot(Relation rel, Relation heaprel, int access);
extern Buffer _bt_gettrueroot(Relation rel);
extern int	_bt_getrootheight(Relation rel);
extern void _bt_metaversion(Relation rel, bool *heapkeyspace,
							bool *allequalimage);
extern void _bt_checkpage(Relation rel, Buffer buf);
extern Buffer _bt_getbuf(Relation rel, BlockNumber blkno, int access);
extern Buffer _bt_allocbuf(Relation rel, Relation heaprel);
extern Buffer _bt_relandgetbuf(Relation rel, Buffer obuf,
							   BlockNumber blkno, int access);
extern void _bt_relbuf(Relation rel, Buffer buf);
extern void _bt_lockbuf(Relation rel, Buffer buf, int access);
extern void _bt_unlockbuf(Relation rel, Buffer buf);
extern bool _bt_conditionallockbuf(Relation rel, Buffer buf);
extern void _bt_upgradelockbufcleanup(Relation rel, Buffer buf);
extern void _bt_pageinit(Page page, Size size);
extern void _bt_delitems_vacuum(Relation rel, Buffer buf,
								OffsetNumber *deletable, int ndeletable,
								BTVacuumPosting *updatable, int nupdatable);
extern void _bt_delitems_delete_check(Relation rel, Buffer buf,
									  Relation heapRel,
									  TM_IndexDeleteOp *delstate);
extern void _bt_pagedel(Relation rel, Buffer leafbuf, BTVacState *vstate);
extern void _bt_pendingfsm_init(Relation rel, BTVacState *vstate,
								bool cleanuponly);
extern void _bt_pendingfsm_finalize(Relation rel, BTVacState *vstate);

/*
 * nbtsearch.c 中函数的原型
 */
extern BTStack _bt_search(Relation rel, Relation heaprel, BTScanInsert key,
						  Buffer *bufP, int access, Snapshot snapshot);
extern Buffer _bt_moveright(Relation rel, Relation heaprel, BTScanInsert key,
							Buffer buf, bool forupdate, BTStack stack,
							int access, Snapshot snapshot);
extern OffsetNumber _bt_binsrch_insert(Relation rel, BTInsertState insertstate);
extern int32 _bt_compare(Relation rel, BTScanInsert key, Page page, OffsetNumber offnum);
extern bool _bt_first(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_next(IndexScanDesc scan, ScanDirection dir);
extern Buffer _bt_get_endpoint(Relation rel, uint32 level, bool rightmost,
							   Snapshot snapshot);

/*
 * nbtutils.c 中函数的原型
 */
extern BTScanInsert _bt_mkscankey(Relation rel, IndexTuple itup);
extern void _bt_freestack(BTStack stack);
extern void _bt_preprocess_array_keys(IndexScanDesc scan);
extern void _bt_start_array_keys(IndexScanDesc scan, ScanDirection dir);
extern bool _bt_advance_array_keys(IndexScanDesc scan, ScanDirection dir);
extern void _bt_mark_array_keys(IndexScanDesc scan);
extern void _bt_restore_array_keys(IndexScanDesc scan);
extern void _bt_preprocess_keys(IndexScanDesc scan);
extern bool _bt_checkkeys(IndexScanDesc scan, IndexTuple tuple,
						  int tupnatts, ScanDirection dir, bool *continuescan);
extern void _bt_killitems(IndexScanDesc scan);
extern BTCycleId _bt_vacuum_cycleid(Relation rel);
extern BTCycleId _bt_start_vacuum(Relation rel);
extern void _bt_end_vacuum(Relation rel);
extern void _bt_end_vacuum_callback(int code, Datum arg);
extern Size BTreeShmemSize(void);
extern void BTreeShmemInit(void);
extern bytea *btoptions(Datum reloptions, bool validate);
extern bool btproperty(Oid index_oid, int attno,
					   IndexAMProperty prop, const char *propname,
					   bool *res, bool *isnull);
extern char *btbuildphasename(int64 phasenum);
extern IndexTuple _bt_truncate(Relation rel, IndexTuple lastleft,
							   IndexTuple firstright, BTScanInsert itup_key);
extern int	_bt_keep_natts_fast(Relation rel, IndexTuple lastleft,
								IndexTuple firstright);
extern bool _bt_check_natts(Relation rel, bool heapkeyspace, Page page,
							OffsetNumber offnum);
extern void _bt_check_third_page(Relation rel, Relation heap,
								 bool needheaptidspace, Page page, IndexTuple newtup);
extern bool _bt_allequalimage(Relation rel, bool debugmessage);

/*
 * nbtvalidate.c 中函数的原型
 */
extern bool btvalidate(Oid opclassoid);
extern void btadjustmembers(Oid opfamilyoid,
							Oid opclassoid,
							List *operators,
							List *functions);

/*
 * nbtsort.c 中函数的原型
 */
extern IndexBuildResult *btbuild(Relation heap, Relation index,
								 struct IndexInfo *indexInfo);
extern void _bt_parallel_build_main(dsm_segment *seg, shm_toc *toc);

#endif							/* NBTREE_H */
