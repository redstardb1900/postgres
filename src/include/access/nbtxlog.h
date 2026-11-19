/*-------------------------------------------------------------------------
 *
 * nbtxlog.h
 *	  postgres btree xlog 例程头文件
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtxlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTXLOG_H
#define NBTXLOG_H

#include "access/transam.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/off.h"

/*
 * btree 操作的 XLOG 记录
 *
 * XLOG 允许把部分信息存在日志高 4 位的
 * xl_info 字段
 */
#define XLOG_BTREE_INSERT_LEAF	0x00	/* 无分裂添加索引元组 */
#define XLOG_BTREE_INSERT_UPPER 0x10	/* 同上,在非叶页 */
#define XLOG_BTREE_INSERT_META	0x20	/* 同上,并更新元页 */
#define XLOG_BTREE_SPLIT_L		0x30	/* 带分裂加索引元组 */
#define XLOG_BTREE_SPLIT_R		0x40	/* 同上,新项在右侧 */
#define XLOG_BTREE_INSERT_POST	0x50	/* 带倒排分裂加索引元组 */
#define XLOG_BTREE_DEDUP		0x60	/* 对一页去重元组 */
#define XLOG_BTREE_DELETE		0x70	/* 删除一页的叶子索引元组 */
#define XLOG_BTREE_UNLINK_PAGE	0x80	/* 删除半死页 */
#define XLOG_BTREE_UNLINK_PAGE_META 0x90	/* 同上,并更新元页 */
#define XLOG_BTREE_NEWROOT		0xA0	/* 新根页 */
#define XLOG_BTREE_MARK_PAGE_HALFDEAD 0xB0	/* 把叶子标为半死 */
#define XLOG_BTREE_VACUUM		0xC0	/* vacuum时删除页上的
										 * 条目*/
#define XLOG_BTREE_REUSE_PAGE	0xD0	/* 旧页即将自以下处重用:
										 * FSM*/
#define XLOG_BTREE_META_CLEANUP	0xE0	/* 更新清理相关数据,在
										 * 元页*/

/*
 * 再生元数据页所需全部内容
 */
typedef struct xl_btree_metadata
{
	uint32		version;
	BlockNumber root;
	uint32		level;
	BlockNumber fastroot;
	uint32		fastlevel;
	uint32		last_cleanup_num_delpages;
	bool		allequalimage;
} xl_btree_metadata;

/*
 * 关于简单(无分裂)插入需知这些。
 *
 * 本数据记录用于 INSERT_LEAF、INSERT_UPPER、INSERT_META 与
 * INSERT_POST。注意 INSERT_META 与 INSERT_UPPER 意味着不是
 * 叶子页，而 INSERT_POST 与 INSERT_LEAF 意味着必须是叶子
 * 页.
 *
 * 备份块 0: 原页
 * 备份块 1: 子页左兄弟,若 INSERT_UPPER 或 INSERT_META
 * 备份块 2: xl_btree_metadata,若 INSERT_META
 *
 * 注意: 新元组实为倒排列表分裂插入情形中的
 * "原"新项(即 INSERT_POST 情形)。倒排列表的分裂
 * 偏移记在原新项之前。恢复两者都要,因其须对已
 * 分裂的现有倒排列表做原地更新作为额外步骤。另
 * 外,恢复会生成"最终"newitem。倒排列表分裂细节
 * 见 _bt_swap_posting()。
 */
typedef struct xl_btree_insert
{
	OffsetNumber offnum;

	/* POSTING SPLIT OFFSET FOLLOWS (INSERT_POST case) */
	/* NEW TUPLE ALWAYS FOLLOWS AT THE END */
} xl_btree_insert;

#define SizeOfBtreeInsert	(offsetof(xl_btree_insert, offnum) + sizeof(OffsetNumber))

/*
 * 带分裂插入时,我们保存进入右兄弟的全部项,
 * 以便能从日志记录完全恢复它。此法比常规做法
 * 少占 xlog 空间,因若按标准做法,
 * XLogInsert 几乎总会认为右页是新页并存储其
 * 整页映像。左页则仍按正常
 * 增量更新方式处理。
 *
 * 注意:XLOG_BTREE_SPLIT_L与XLOG_BTREE_SPLIT_R共用本数据记录。
 * 有两种变体,标明插入的元组进了左还是右分裂页
 * (从而是否存放了新项)。我们总是记录左页高键,
 * 因为后缀截断可用用户定义代码生成新的叶子高
 * 键。内部页也必须如此,因左页高键所依据的
 * firstright 项在右页已被截成零属性(从右页
 * 无法取得
 * 分隔键)。
 *
 * 备份块 0: 原页 / 新左页
 *
 * 左页数据部分含新项,若是 _L 变体。
 * _R 变体分裂记录一般无 newitem(_R 变体叶子
 * 页分裂记录若须处理倒排列表分裂会含显式
 * newitem,但它从不用在右页——它实为更新现有
 * 倒排列表所需的 orignewitem)。左/原页的新
 * 高键总在最后出现(且必须始终出
 * 现)。
 *
 * 需 REDO 例程直接处理倒排列表分裂的页分裂
 * 记录会有显式 newitem,实为
 * orignewitem(倒排列表分裂前而非之后的
 * newitem)。倒排列表分裂时 newitem 总紧跟在
 * 被分裂倒排列表之后(分裂前本会与
 * orignewitem 重叠)。通常 REDO 用 _L 变体页
 * 分裂记录处理倒排列表分裂,且通常新倒排
 * 列表与最终 newitem 都在左页(用新倒排列表
 * 替换旧的插入,最终 newitem 紧挨插入)。但
 * _R 变体分裂记录会在页分裂点恰有
 * lastleft 元组同时又是被分裂倒排列表时含
 * orignewitem(使 newitem 成为页分裂的
 * firstright 元组)。此边角情形的存在不改变
 * REDO 例程对 newitem/orignewitem 的基本事实:
 * 它始终只是左页所用状态。(因此记录的
 * postingoff 字段并非是否在页分裂期间发生了
 * 倒排列表分裂的可靠指示;非零仅表示 REDO
 * 例程必须重建左页所需的新倒排列表
 * 元组。)
 *
 * 此倒排列表分裂处理等价于 xl_btree_insert
 * REDO 例程的 INSERT_POST 处理。此处细节更复杂,
 * 但概念与目标完全相同。倒排列表分裂细节见
 * _bt_swap_posting()。
 *
 * 备份块 1: 新右页
 *
 * 右页数据部分含右页元组,格式由
 * _bt_restore_page 使用。若是 _R 变体则含新项。
 * 任一变体下右页元组也含右页高键(分裂时自
 * 左/原页移来),除非分裂恰好发生在该层最右
 * 页,此时新右页
 * 无高键。
 *
 * 备份块 2: 下一块(原页右链),若有
 * 备份块 3: 子页左兄弟,若非叶分裂
 */
typedef struct xl_btree_split
{
	uint32		level;			/* 被分裂页的树层级 */
	OffsetNumber firstrightoff; /* 右页上首个原页项 */
	OffsetNumber newitemoff;	/* 新项偏移 */
	uint16		postingoff;		/* 原倒排元组内偏移 */
} xl_btree_split;

#define SizeOfBtreeSplit	(offsetof(xl_btree_split, postingoff) + sizeof(uint16))

/*
 * 页去重时,连续等键元组组被合并成
 * 倒排列表元组。
 *
 * WAL 记录表示一次叶子页去重遍历。随后是
 * BTDedupInterval 结构数组。
 */
typedef struct xl_btree_dedup
{
	uint16		nintervals;

	/* DEDUPLICATION INTERVALS FOLLOW */
} xl_btree_dedup;

#define SizeOfBtreeDedup 	(offsetof(xl_btree_dedup, nintervals) + sizeof(uint16))

/*
 * 关于 btree 内页重用需知这些。本记录
 * 仅为热备生成冲突点而存在。
 *
 * 注意必须在记录中含 RelFileLocator,因我们
 * 实际未把缓冲区登记到记录。
 */
typedef struct xl_btree_reuse_page
{
	RelFileLocator locator;
	BlockNumber block;
	FullTransactionId snapshotConflictHorizon;
	bool		isCatalogRel;	/* 处理备库上逻辑解码时的
								 * 恢复冲突*/
} xl_btree_reuse_page;

#define SizeOfBtreeReusePage	(offsetof(xl_btree_reuse_page, isCatalogRel) + sizeof(bool))

/*
 * xl_btree_vacuum 与 xl_btree_delete 记录描述在
 * 叶子页上删除索引元组。前一变体由 VACUUM 使用,
 * 后一变体用于 btinsert() 被调用时有时发生的
 * 临时删除。
 *
 * 两类记录很相似。唯一差别是 xl_btree_delete
 * 有 snapshotConflictHorizon/isCatalogRel 字段供
 * 恢复冲突。(VACUUM 操作可只依赖对表剪枝时更早
 * 生成的冲突,那些表的 TIDs 正是待删索引元组所
 * 指向的。各 REDO 例程还有小差异,此处不
 * 展开。)
 *
 * xl_btree_vacuum 与 xl_btree_delete 都用页偏移号
 * 表示在单张叶子页上删除任意数目的索引元组。两者
 * 也支持索引元组的"更新",即删除现有倒排列表
 * 元组所含 TID 子集的实现方式。
 *
 * 更新后的倒排列表元组用 xl_btree_update 元数据表示。
 * 各 REDO 例程用 xl_btree_update 条目(外加目标
 * 叶子页上对应的原索引元组)生成最终
 * 更新后的元组。
 *
 * 仅当 REDO 例程仍会留下一些 TID 时才用更新。
 * 否则倒排列表元组就直接整项删除。
 */
typedef struct xl_btree_vacuum
{
	uint16		ndeleted;
	uint16		nupdated;

	/*----
	 * In payload of blk 0 :
	 * - DELETED TARGET OFFSET NUMBERS
	 * - UPDATED TARGET OFFSET NUMBERS
	 * - UPDATED TUPLES METADATA (xl_btree_update) ITEMS
	 *----
	 */
} xl_btree_vacuum;

#define SizeOfBtreeVacuum	(offsetof(xl_btree_vacuum, nupdated) + sizeof(uint16))

typedef struct xl_btree_delete
{
	TransactionId snapshotConflictHorizon;
	uint16		ndeleted;
	uint16		nupdated;
	bool		isCatalogRel;	/* 处理备库上逻辑解码时的
								 * 恢复冲突*/

	/*----
	 * In payload of blk 0 :
	 * - DELETED TARGET OFFSET NUMBERS
	 * - UPDATED TARGET OFFSET NUMBERS
	 * - UPDATED TUPLES METADATA (xl_btree_update) ITEMS
	 *----
	 */
} xl_btree_delete;

#define SizeOfBtreeDelete	(offsetof(xl_btree_delete, isCatalogRel) + sizeof(bool))

/*
 * xl_btree_update 元数据中的偏移是元组原倒排
 * 列表内的偏移,不是页偏移号。它们是
 * 从 0 起算。原倒排列表元组的页偏移号来自
 * 主 xl_btree_vacuum/xl_btree_delete 记录。
 */
typedef struct xl_btree_update
{
	uint16		ndeletedtids;

	/* POSTING LIST uint16 OFFSETS TO A DELETED TID FOLLOW */
} xl_btree_update;

#define SizeOfBtreeUpdate	(offsetof(xl_btree_update, ndeletedtids) + sizeof(uint16))

/*
 * 关于把空子树标为待删需知这些。
 * target 标明父页中被移除的元组(注意我们
 * 去掉该元组的下行链接及*随后*元组的键)。注意
 * 叶子页为空,故不必存其内容——恢复时用其余
 * 字段重新初始化即可。
 *
 * 备份块 0: 叶子块
 * 备份块1:最顶父页
 */
typedef struct xl_btree_mark_page_halfdead
{
	OffsetNumber poffset;		/* 父页中已删元组 id */

	/* 重建叶子页所需信息: */
	BlockNumber leafblk;		/* 最终被删的叶子块 */
	BlockNumber leftblk;		/* 叶子块左兄弟,若有 */
	BlockNumber rightblk;		/* 叶子块右兄弟 */
	BlockNumber topparent;		/* 子树中最顶内部页 */
} xl_btree_mark_page_halfdead;

#define SizeOfBtreeMarkPageHalfDead (offsetof(xl_btree_mark_page_halfdead, topparent) + sizeof(BlockNumber))

/*
 * 关于删除 btree 页需知这些。注意我们在已删
 * 页上只留下少量簿记信息(已删页须暂时作为
 * 墓碑保留)。REDO 例程从零再生目标页较方便。
 * 因此 WAL 记录描述了某些其实可直接从
 * 目标页取得的
 * 细节。
 *
 * 备份块 0: 正被删除的目标块
 * 备份块 1: 目标块左兄弟,若有
 * 备份块 2: 目标块右兄弟
 * 备份块 3: 叶子块(若异于目标)
 * 备份块 4: 元页(若右兄弟成新快速根)
 */
typedef struct xl_btree_unlink_page
{
	BlockNumber leftsib;		/* 目标块左兄弟,若有 */
	BlockNumber rightsib;		/* 目标块右兄弟 */
	uint32		level;			/* 目标块层级 */
	FullTransactionId safexid;	/* 目标块 BTPageSetDeleted() XID */

	/*
	 * 重建带正确 topparent 链接的半死叶子页
	 * 所需信息。仅当删除操作的目标页是内部页时
	 * 才用这些字段。REDO 例程从零创建半死页以
	 * 保持简单(与目标页本身所用的同一便捷
	 * 做法)。
	 */
	BlockNumber leafleftsib;
	BlockNumber leafrightsib;
	BlockNumber leaftopparent;	/* 子树中下一层子页 */

	/* xl_btree_metadata FOLLOWS IF XLOG_BTREE_UNLINK_PAGE_META */
} xl_btree_unlink_page;

#define SizeOfBtreeUnlinkPage	(offsetof(xl_btree_unlink_page, leaftopparent) + sizeof(BlockNumber))

/*
 * 新根日志记录。若建立空根则元组数为零,
 * 若是分裂旧根的结果则为两个。
 *
 * 注意虽这意味着改写元数据页,但我们不需要
 * xl_btree_metadata 记录——rootblk 与 level 已够。
 *
 * 备份块 0: 新根页(若分裂旧根则载荷 2 元组)
 * 备份块 1: 左子页(若分裂旧根)
 * 备份块 2: 元页
 */
typedef struct xl_btree_newroot
{
	BlockNumber rootblk;		/* 新根位置(与块 0 重复) */
	uint32		level;			/* 其树层级 */
} xl_btree_newroot;

#define SizeOfBtreeNewroot	(offsetof(xl_btree_newroot, level) + sizeof(uint32))


/*
 * nbtxlog.c 中函数的原型
 */
extern void btree_redo(XLogReaderState *record);
extern void btree_xlog_startup(void);
extern void btree_xlog_cleanup(void);
extern void btree_mask(char *pagedata, BlockNumber blkno);

/*
 * nbtdesc.c 中函数的原型
 */
extern void btree_desc(StringInfo buf, XLogReaderState *record);
extern const char *btree_identify(uint8 info);

#endif							/* NBTXLOG_H */
