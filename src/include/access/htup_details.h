/*-------------------------------------------------------------------------
 *
 * htup_details.h
 *	POSTGRES 堆元组头定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup_details.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_DETAILS_H
#define HTUP_DETAILS_H

#include "access/htup.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "varatt.h"

/*
 * MaxTupleAttributeNumber 限制元组中（用户）列数。
 * 该值的关键限制是：元组固定开销之大小，加上
 * 空值位图之大小（每列 1 位），再加上
 * MAXALIGN 对齐，必须装入 uint8 的 t_hoff。多数
 * 机器上若不加宽 t_hoff，上限大约略
 * 超 1700。此处与 MaxHeapAttributeNumber 均用整数，
 * 以免 HeapTupleHeaderData 布局改动改变所
 * 支持的最大列数。
 */
#define MaxTupleAttributeNumber 1664	/* 8 * 208 */

/*
 * MaxHeapAttributeNumber 限制表中（用户）列数。
 * 应略小于 MaxTupleAttributeNumber。必须
 * 至少再小一，否则无法对最大宽度表做 UPDATE，
 * 因 UPDATE 须构造含 CTID 的工作元组。
 * 实践中还要留些余量，以便能从容地
 * 支持添加隐藏 "resjunk" 列的操作，例如
 * 宽表上带 ORDER BY 隐藏列的查询。
 * 无论如何，视列数据类型而定，列数超过大约
 * 一千时，多半会撞上基于磁盘块的元组总大小上限。
 * TOAST 也无济于事。
 */
#define MaxHeapAttributeNumber	1600	/* 8 * 200 */

/*
 * 堆元组头。为免浪费空间，各字段应
 * 按避免结构填充的方式排布。
 *
 * 复合类型（行类型）的 Datum 与磁盘上元组共享
 * 同一总体结构，以便用相同例程构建与
 * 检查。但需求略有不同：Datum
 * 不需要任何事务可见性信息，却需要
 * 长度字与一些嵌入的类型信息。我们可借
 * 叠加堆元组的 xmin/cmin/xmax/cmax/xvac 字段
 * 与 Datum 情形所需字段来实现。通常，内存中
 * 构建的元组都先用 Datum 字段初始化；但当元组
 * 即将插入表中时，会填入事务字段，
 * 覆盖 datum 字段。
 *
 * 堆元组的总体结构如下：
 *			固定字段（HeapTupleHeaderData）
 *			空值位图（若 t_infomask 置 HEAP_HASNULL）
 *			对齐填充（按需使数据 MAXALIGN 对齐）
 *			对象 ID（若 t_infomask 置 HEAP_HASOID_OLD，现已不创
 *          建)
 *	用户数据字段
 *
 * 我们把五个"虚拟"字段 Xmin/Cmin/Xmax/Cmax/Xvac 存于三个
 * 物理字段中。Xmin 与 Xmax 总是真实存储，但 Cmin、Cmax
 * 与 Xvac 共享一字段。可行是因我们知 Cmin 与 Cmax
 * 分别仅在插入与删除事务的生存期内
 * 才有意义。若元组在同一事务中被插入又删除，
 * 我们存一个可映射到真实 cmin/cmax 的"组合"命令 ID，
 * 但只能借助发起后端内的局部状态。
 * 详见 combocid.c。同时，Xvac 仅由
 * 旧式 VACUUM FULL 设置，它没有任何命令子结构，因而
 * 也不需要 Cmin 或 Cmax。（这要求旧式 VACUUM
 * FULL 永不尝试移动其 Cmin 或 Cmax 仍有意义的元组，
 * 亦即插入进行中或删除进行中的元组。）
 *
 * 关于 t_ctid：每当新元组存入磁盘，其 t_ctid
 * 初始化为自己的 TID（位置）。若该元组随后被更新，
 * 其 t_ctid 改为指向元组的替换版本。或者，
 * 若因分区键更新而使元组从一分区移到另一分区，
 * 则 t_ctid 被设为特殊值以标明此事（见
 * ItemPointerSetMovedPartitions）。故元组为其行最新版，
 * 当且仅当 XMAX 无效或
 * t_ctid 指向自身（此时若 XMAX 有效，则该元组
 * 要么被锁住要么已删除）。可沿 t_ctid 链接链
 * 找到该行最新版本，除非它已被移到另一
 * 分区。但须注意 VACUUM 可能
 * 先擦除被指向的（新）元组，再擦除指向的（旧）
 * 元组。故沿 t_ctid 链接跟随时，必须检查
 * 被引用槽是否为空或含有无关元组。
 * 核对被引用元组的 XMIN 是否等于引用元组的
 * XMAX，以验证它确是后代版本而非
 * VACUUM 新近释放之槽中存入的无关元组。若任一项
 * 检查失败，即可假定没有存活的后代版本。
 *
 * t_ctid 有时用于存放推测插入令牌，而非
 * 真实 TID。正在插入的元组上会设置推测令牌，
 * 直到插入者确定要继续完成
 * 插入。故令牌应只出现在 XMAX 仍为
 * 进行中，或无效/已中止的元组上。插入确认后令牌被
 * 替换为该元组的真实 TID。沿 t_ctid 链接链跟随时
 * 绝不应当见到推测插入令牌，
 * 因为它们不用于更新，仅用于插入。
 *
 * 紧随固定头字段之后存放空值位图（始于
 * t_bits）。若 t_infomask 表明元组中没有空值，则
 * *不* 存放该位图。若存在 OID 字段（由
 * t_infomask 所示），则存于用户数据正前，用户数据始于
 * t_hoff 所示偏移。注意 t_hoff 必须是
 * MAXALIGN.
 */

typedef struct HeapTupleFields
{
	TransactionId t_xmin;		/* 插入事务 ID */
	TransactionId t_xmax;		/* 删除或锁定事务 ID */

	union
	{
		CommandId	t_cid;		/* 插入或删除命令 ID，或二者 */
		TransactionId t_xvac;	/* 旧式 VACUUM FULL 事务 ID */
	}			t_field3;
} HeapTupleFields;

typedef struct DatumTupleFields
{
	int32		datum_len_;		/* varlena 头（勿直接触碰！） */

	int32		datum_typmod;	/* -1，或记录类型标识 */

	Oid			datum_typeid;	/* 复合类型 OID，或 RECORDOID */

	/*
	 * datum_typeid 无法为复合上的域，只能是普通复合，
	 * 即便该 datum 意在作为复合类型上域的值。
	 * 这符合 CoerceToDomain 不改变
	 * 基类型值物理表示的一般原则。
	 *
	 * 注意：字段次序的选取考虑到 Oid 将来可能
	 * 扩到 64 位。
	 */
} DatumTupleFields;

struct HeapTupleHeaderData
{
	union
	{
		HeapTupleFields t_heap;
		DatumTupleFields t_datum;
	}			t_choice;

	ItemPointerData t_ctid;		/* 此或较新元组的当前 TID（或
								 * 推测插入令牌) */

	/* 下方字段须匹配 MinimalTupleData！ */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2 2
	uint16		t_infomask2;	/* 属性数目 + 各种标志 */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK 3
	uint16		t_infomask;		/* 各种标志位，见下 */

#define FIELDNO_HEAPTUPLEHEADERDATA_HOFF 4
	uint8		t_hoff;			/* 头大小（含位图、填充） */

	/* ^ - 23字节 - ^ */

#define FIELDNO_HEAPTUPLEHEADERDATA_BITS 5
	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* 空值位图 */

	/* 更多数据紧随结构体末尾 */
};

/* typedef 见 htup.h */

#define SizeofHeapTupleHeader offsetof(HeapTupleHeaderData, t_bits)

/*
 * t_infomask 所存信息：
 */
#define HEAP_HASNULL			0x0001	/* 有空值属性 */
#define HEAP_HASVARWIDTH		0x0002	/* 有变长属性 */
#define HEAP_HASEXTERNAL		0x0004	/* 有外部存储的属性 */
#define HEAP_HASOID_OLD			0x0008	/* 有对象 ID 字段 */
#define HEAP_XMAX_KEYSHR_LOCK	0x0010	/* xmax 是键共享加锁者 */
#define HEAP_COMBOCID			0x0020	/* t_cid 是 combo CID */
#define HEAP_XMAX_EXCL_LOCK		0x0040	/* xmax 是排他加锁者 */
#define HEAP_XMAX_LOCK_ONLY		0x0080	/* xmax 有效时仅是加锁者 */

 /* xmax 是共享加锁者 */
#define HEAP_XMAX_SHR_LOCK	(HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)

#define HEAP_LOCK_MASK	(HEAP_XMAX_SHR_LOCK | HEAP_XMAX_EXCL_LOCK | \
						 HEAP_XMAX_KEYSHR_LOCK)
#define HEAP_XMIN_COMMITTED		0x0100	/* t_xmin 已提交 */
#define HEAP_XMIN_INVALID		0x0200	/* t_xmin 无效/中止 */
#define HEAP_XMIN_FROZEN		(HEAP_XMIN_COMMITTED|HEAP_XMIN_INVALID)
#define HEAP_XMAX_COMMITTED		0x0400	/* t_xmax 已提交 */
#define HEAP_XMAX_INVALID		0x0800	/* t_xmax 无效/中止 */
#define HEAP_XMAX_IS_MULTI		0x1000	/* t_xmax 是 MultiXactId */
#define HEAP_UPDATED			0x2000	/* 这是行的 UPDATEd 版本 */
#define HEAP_MOVED_OFF			0x4000	/* 由 pre-9.0 移到另一处
										 * VACUUM FULL; 为二进制
										 * 升级而保留*/
#define HEAP_MOVED_IN			0x8000	/* 由 pre-9.0 自另一处移入
										 * VACUUM FULL; 为二进制
										 * 升级而保留*/
#define HEAP_MOVED (HEAP_MOVED_OFF | HEAP_MOVED_IN)

#define HEAP_XACT_MASK			0xFFF0	/* 可见性相关位 */

/*
 * 元组仅被加锁(即未被其 Xmax 更新)，当
 * HEAP_XMAX_LOCK_ONLY 位置位；或为 pg_upgrade，若 Xmax
 * 不是 multi 且 EXCL_LOCK 位置位。
 *
 * 另见 HeapTupleHeaderIsOnlyLocked，它也检查可能的
 * 已中止更新者事务。
 *
 * 当心该参数被多次求值。
 */
#define HEAP_XMAX_IS_LOCKED_ONLY(infomask) \
	(((infomask) & HEAP_XMAX_LOCK_ONLY) || \
	 (((infomask) & (HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK)) == HEAP_XMAX_EXCL_LOCK))

/*
 * 具有 HEAP_XMAX_IS_MULTI 与 HEAP_XMAX_LOCK_ONLY 但既无
 * HEAP_XMAX_EXCL_LOCK 也无 HEAP_XMAX_KEYSHR_LOCK 的元组，必来自
 * 在 9.2 或更早共享锁定并经 pg_upgrade 的。
 *
 * 在 9.2 及更早，HEAP_XMAX_IS_MULTI 仅当存在多个
 * FOR SHARE 加锁者时才置位。那设置 HEAP_XMAX_LOCK_ONLY(旧名
 * 不同)但既无 HEAP_XMAX_EXCL_LOCK 也无
 * HEAP_XMAX_KEYSHR_LOCK。该组合在 9.3 及以上已不
 * 可能，故若见到该组合便可确定该元组曾
 * 在更早版本中被锁；因所有此类加锁者已不在(它们无法
 * 经 pg_upgrade 存活)，此类元组可安全视为未
 * 锁定
 *
 * 我们切勿在本地解析此类 multixact，因为结果会是
 * 伪的，无论其相对当前有效 multixact
 * 范围如何。
 */
#define HEAP_LOCKED_UPGRADED(infomask) \
( \
	 ((infomask) & HEAP_XMAX_IS_MULTI) != 0 && \
	 ((infomask) & HEAP_XMAX_LOCK_ONLY) != 0 && \
	 (((infomask) & (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)) == 0) \
)

/*
 * 用这些测试某特定锁是否加在元组上
 */
#define HEAP_XMAX_IS_SHR_LOCKED(infomask) \
	(((infomask) & HEAP_LOCK_MASK) == HEAP_XMAX_SHR_LOCK)
#define HEAP_XMAX_IS_EXCL_LOCKED(infomask) \
	(((infomask) & HEAP_LOCK_MASK) == HEAP_XMAX_EXCL_LOCK)
#define HEAP_XMAX_IS_KEYSHR_LOCKED(infomask) \
	(((infomask) & HEAP_LOCK_MASK) == HEAP_XMAX_KEYSHR_LOCK)

/* 当 Xmax 将变更时全部关掉这些 */
#define HEAP_XMAX_BITS (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID | \
						HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK | HEAP_XMAX_LOCK_ONLY)

/*
 * t_infomask2 中存放的信息：
 */
#define HEAP_NATTS_MASK			0x07FF	/* 11 位表示属性个数 */
/* 位 0x1800 可用 */
#define HEAP_KEYS_UPDATED		0x2000	/* 元组被更新且键列
										 * 被修改或元组被删除*/
#define HEAP_HOT_UPDATED		0x4000	/* 元组被 HOT 更新 */
#define HEAP_ONLY_TUPLE			0x8000	/* 此为仅堆元组 */

#define HEAP2_XACT_MASK			0xE000	/* 可见性相关位 */

/*
 * HEAP_TUPLE_HAS_MATCH 是哈希连接期间用的临时标志。它
 * 仅用于哈希表中的元组，且那些元组不需要
 * 任何可见性信息，故可叠放在某个可见性标志上
 * 而不必占用专用位。
 */
#define HEAP_TUPLE_HAS_MATCH	HEAP_ONLY_TUPLE /* 元组有连接匹配 */

/*
 * HeapTupleHeader 访问宏
 *
 * 注意：当心 "tup" 实参被多次求值。但 Set
 * 宏对其他实参只求值一次。
 */

/*
 * HeapTupleHeaderGetRawXmin 返回"原始"xmin 字段，即当初
 * 用于插入该元组的 xid。但该元组实际上可能
 * 已冻结(经 HeapTupleHeaderSetXminFrozen)，此时其 xmin
 * 对每个快照均可见。PostgreSQL 9.4 之前，我们实际会把
 *  xmin 改为 FrozenTransactionId，且该值仍可能遇于
 * 盘上.
 */
#define HeapTupleHeaderGetRawXmin(tup) \
( \
	(tup)->t_choice.t_heap.t_xmin \
)

#define HeapTupleHeaderGetXmin(tup) \
( \
	HeapTupleHeaderXminFrozen(tup) ? \
		FrozenTransactionId : HeapTupleHeaderGetRawXmin(tup) \
)

#define HeapTupleHeaderSetXmin(tup, xid) \
( \
	(tup)->t_choice.t_heap.t_xmin = (xid) \
)

#define HeapTupleHeaderXminCommitted(tup) \
( \
	((tup)->t_infomask & HEAP_XMIN_COMMITTED) != 0 \
)

#define HeapTupleHeaderXminInvalid(tup) \
( \
	((tup)->t_infomask & (HEAP_XMIN_COMMITTED|HEAP_XMIN_INVALID)) == \
		HEAP_XMIN_INVALID \
)

#define HeapTupleHeaderXminFrozen(tup) \
( \
	((tup)->t_infomask & (HEAP_XMIN_FROZEN)) == HEAP_XMIN_FROZEN \
)

#define HeapTupleHeaderSetXminCommitted(tup) \
( \
	AssertMacro(!HeapTupleHeaderXminInvalid(tup)), \
	((tup)->t_infomask |= HEAP_XMIN_COMMITTED) \
)

#define HeapTupleHeaderSetXminInvalid(tup) \
( \
	AssertMacro(!HeapTupleHeaderXminCommitted(tup)), \
	((tup)->t_infomask |= HEAP_XMIN_INVALID) \
)

#define HeapTupleHeaderSetXminFrozen(tup) \
( \
	AssertMacro(!HeapTupleHeaderXminInvalid(tup)), \
	((tup)->t_infomask |= HEAP_XMIN_FROZEN) \
)

/*
 * HeapTupleHeaderGetRawXmax 取得原始 Xmax 字段。要弄清更新
 * 某元组的 Xid，若某些位已置位则可能需解析 MultiXactId。
 * HeapTupleHeaderGetUpdateXid 检查这些位并
 * 在必要时加以解析。这可能涉及 multixact I/O，
 * 故仅在绝对必要时才应使用。
 */
#define HeapTupleHeaderGetUpdateXid(tup) \
( \
	(!((tup)->t_infomask & HEAP_XMAX_INVALID) && \
	 ((tup)->t_infomask & HEAP_XMAX_IS_MULTI) && \
	 !((tup)->t_infomask & HEAP_XMAX_LOCK_ONLY)) ? \
		HeapTupleGetUpdateXid(tup) \
	: \
		HeapTupleHeaderGetRawXmax(tup) \
)

#define HeapTupleHeaderGetRawXmax(tup) \
( \
	(tup)->t_choice.t_heap.t_xmax \
)

#define HeapTupleHeaderSetXmax(tup, xid) \
( \
	(tup)->t_choice.t_heap.t_xmax = (xid) \
)

/*
 * HeapTupleHeaderGetRawCommandId 会给出头中所存内容，无论
 * 是否有用。多数代码应改用 HeapTupleHeaderGetCmin 或
 * HeapTupleHeaderGetCmax，但注意它们会 Assert 你能
 * 得到合法结果，即你处于发起该操作的事务中！
 */
#define HeapTupleHeaderGetRawCommandId(tup) \
( \
	(tup)->t_choice.t_heap.t_field3.t_cid \
)

/* SetCmin 相当简单，因我们从不需要 combo CID */
#define HeapTupleHeaderSetCmin(tup, cid) \
do { \
	Assert(!((tup)->t_infomask & HEAP_MOVED)); \
	(tup)->t_choice.t_heap.t_field3.t_cid = (cid); \
	(tup)->t_infomask &= ~HEAP_COMBOCID; \
} while (0)

/* SetCmax 须在 HeapTupleHeaderAdjustCmax 后使用；见 combocid.c */
#define HeapTupleHeaderSetCmax(tup, cid, iscombo) \
do { \
	Assert(!((tup)->t_infomask & HEAP_MOVED)); \
	(tup)->t_choice.t_heap.t_field3.t_cid = (cid); \
	if (iscombo) \
		(tup)->t_infomask |= HEAP_COMBOCID; \
	else \
		(tup)->t_infomask &= ~HEAP_COMBOCID; \
} while (0)

#define HeapTupleHeaderGetXvac(tup) \
( \
	((tup)->t_infomask & HEAP_MOVED) ? \
		(tup)->t_choice.t_heap.t_field3.t_xvac \
	: \
		InvalidTransactionId \
)

#define HeapTupleHeaderSetXvac(tup, xid) \
do { \
	Assert((tup)->t_infomask & HEAP_MOVED); \
	(tup)->t_choice.t_heap.t_field3.t_xvac = (xid); \
} while (0)

StaticAssertDecl(MaxOffsetNumber < SpecTokenOffsetNumber,
				 "invalid speculative token constant");

#define HeapTupleHeaderIsSpeculative(tup) \
( \
	(ItemPointerGetOffsetNumberNoCheck(&(tup)->t_ctid) == SpecTokenOffsetNumber) \
)

#define HeapTupleHeaderGetSpeculativeToken(tup) \
( \
	AssertMacro(HeapTupleHeaderIsSpeculative(tup)), \
	ItemPointerGetBlockNumber(&(tup)->t_ctid) \
)

#define HeapTupleHeaderSetSpeculativeToken(tup, token)	\
( \
	ItemPointerSet(&(tup)->t_ctid, token, SpecTokenOffsetNumber) \
)

#define HeapTupleHeaderIndicatesMovedPartitions(tup) \
	ItemPointerIndicatesMovedPartitions(&(tup)->t_ctid)

#define HeapTupleHeaderSetMovedPartitions(tup) \
	ItemPointerSetMovedPartitions(&(tup)->t_ctid)

#define HeapTupleHeaderGetDatumLength(tup) \
	VARSIZE(tup)

#define HeapTupleHeaderSetDatumLength(tup, len) \
	SET_VARSIZE(tup, len)

#define HeapTupleHeaderGetTypeId(tup) \
( \
	(tup)->t_choice.t_datum.datum_typeid \
)

#define HeapTupleHeaderSetTypeId(tup, typeid) \
( \
	(tup)->t_choice.t_datum.datum_typeid = (typeid) \
)

#define HeapTupleHeaderGetTypMod(tup) \
( \
	(tup)->t_choice.t_datum.datum_typmod \
)

#define HeapTupleHeaderSetTypMod(tup, typmod) \
( \
	(tup)->t_choice.t_datum.datum_typmod = (typmod) \
)

/*
 * 注意，元组或原拟更新事务一旦已知中止，我们即
 * 不再视该元组为 HOT-updated。为求最佳
 * 效率，使用本宏前请先检查元组可见性，以使
 * INVALID 位尽可能保持最新。
 */
#define HeapTupleHeaderIsHotUpdated(tup) \
( \
	((tup)->t_infomask2 & HEAP_HOT_UPDATED) != 0 && \
	((tup)->t_infomask & HEAP_XMAX_INVALID) == 0 && \
	!HeapTupleHeaderXminInvalid(tup) \
)

#define HeapTupleHeaderSetHotUpdated(tup) \
( \
	(tup)->t_infomask2 |= HEAP_HOT_UPDATED \
)

#define HeapTupleHeaderClearHotUpdated(tup) \
( \
	(tup)->t_infomask2 &= ~HEAP_HOT_UPDATED \
)

#define HeapTupleHeaderIsHeapOnly(tup) \
( \
  ((tup)->t_infomask2 & HEAP_ONLY_TUPLE) != 0 \
)

#define HeapTupleHeaderSetHeapOnly(tup) \
( \
  (tup)->t_infomask2 |= HEAP_ONLY_TUPLE \
)

#define HeapTupleHeaderClearHeapOnly(tup) \
( \
  (tup)->t_infomask2 &= ~HEAP_ONLY_TUPLE \
)

#define HeapTupleHeaderHasMatch(tup) \
( \
  ((tup)->t_infomask2 & HEAP_TUPLE_HAS_MATCH) != 0 \
)

#define HeapTupleHeaderSetMatch(tup) \
( \
  (tup)->t_infomask2 |= HEAP_TUPLE_HAS_MATCH \
)

#define HeapTupleHeaderClearMatch(tup) \
( \
  (tup)->t_infomask2 &= ~HEAP_TUPLE_HAS_MATCH \
)

#define HeapTupleHeaderGetNatts(tup) \
	((tup)->t_infomask2 & HEAP_NATTS_MASK)

#define HeapTupleHeaderSetNatts(tup, natts) \
( \
	(tup)->t_infomask2 = ((tup)->t_infomask2 & ~HEAP_NATTS_MASK) | (natts) \
)

#define HeapTupleHeaderHasExternal(tup) \
		(((tup)->t_infomask & HEAP_HASEXTERNAL) != 0)


/*
 * BITMAPLEN(NATTS) -
 *		按数据列数计算空值位图大小。
 */
#define BITMAPLEN(NATTS)	(((int)(NATTS) + 7) / 8)

/*
 * MaxHeapTupleSize 是堆元组允许的最大尺寸，含
 * 头部与 MAXALIGN 对齐填充。基本上等于 BLCKSZ 减去
 * 磁盘页上必须另有的其他内容。因堆页不使用
 * "特殊空间"，故对此不作扣减。
 *
 * 注意：我们已计入必须指向元组的 ItemId，确保
 * 原本为空的页面确实能放下此尺寸的元组。由于
 * ItemId 与元组对齐要求不同，勿假定例如能在
 * 同一页放入 2 个 MaxHeapTupleSize/2 尺寸的元组。
 */
#define MaxHeapTupleSize  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)))
#define MinHeapTupleSize  MAXALIGN(SizeofHeapTupleHeader)

/*
 * MaxHeapTuplesPerPage 是单堆页可容纳元组数的
 * 上限。（注意索引可能更多，因其使用
 * 更小的元组头。）该除数的由来是：每个元组
 * 必须按 MAXALIGN 对齐，且须有关联的行指针。
 *
 * 注意：有 HOT 时，理论上堆页上行指针数（非实际
 * 元组数）可多于此。但我们仍把行指针数
 * 约束到此值，以免行指针过度膨胀，且不
 * 致需增大工作数组的尺寸。
 */
#define MaxHeapTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))))

/*
 * MaxAttrSize 是 char(n) 及类似类型声明尺寸的
 * 某种任意上限。它不必与 varlena 值的
 * *实际*上限有直接关系；后者
 * 当前为 1Gb（见 postgres.h 中的 TOAST 结构）。我把它
 * 设为 10Mb，似乎是个合理数字 --- tgl 8/6/00。
 */
#define MaxAttrSize		(10 * 1024 * 1024)


/*
 * MinimalTuple 是一种替代表示，用于执行器内的瞬时
 * 元组：在不需要事务状态信息、元组行类型已知、且因
 * 要存许多元组而值得省下数字节的场合。该表示的
 * 选取使元组访问例程能经 HeapTupleData 指针结构
 * 同时处理完整或
 * 最小元组。访问例程看不出
 * 差别，只是不可访问事务状态
 * 或 t_ctid 字段，因那些字段不在。
 *
 * 多数情况下，MinimalTuple 应经由 TupleTableSlot
 * 例程访问。这些例程会阻止访问"系统列"，
 * 从而防止误用那些并不存在的字段。
 *
 * MinimalTupleData 含长度字、若干填充，以及与
 * HeapTupleHeaderData 自 t_infomask2 起匹配的字段。填充选取使
 * 两边 offsetof(t_infomask2) 对 MAXIMUM_ALIGNOF 同余。
 * 这使两种情形的数据对齐规则等价。
 *
 * 当经 HeapTupleData 指针访问最小元组时，t_data
 * 被设为指向实际起点之前 MINIMAL_TUPLE_OFFSET 字节处，
 * 亦即与该最小元组数据匹配的完整元组的
 * 数据起点。此技巧使两结构体看似等价。
 *
 * 注意 t_hoff 按完整元组同样计算，故包含
 * MINIMAL_TUPLE_OFFSET 这段距离。但 t_len 不含它。
 *
 * MINIMAL_TUPLE_DATA_OFFSET 是除长度字外首个有用
 * （非填充）数据的偏移。tuplesort.c 与 tuplestore.c 用它避免
 * 把填充写入磁盘。
 */
#define MINIMAL_TUPLE_OFFSET \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) / MAXIMUM_ALIGNOF * MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_PADDING \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) % MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_DATA_OFFSET \
	offsetof(MinimalTupleData, t_infomask2)

struct MinimalTupleData
{
	uint32		t_len;			/* 最小元组的实际长度 */

	char		mt_padding[MINIMAL_TUPLE_PADDING];

	/* 以下字段必须与 HeapTupleHeaderData 匹配! */

	uint16		t_infomask2;	/* 属性数目 + 各种标志 */

	uint16		t_infomask;		/* 各种标志位，见下 */

	uint8		t_hoff;			/* 头大小（含位图、填充） */

	/* ^ - 23字节 - ^ */

	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* 空值位图 */

	/* 更多数据紧随结构体末尾 */
};

/* typedef 见 htup.h */

#define SizeofMinimalTupleHeader offsetof(MinimalTupleData, t_bits)


/*
 * GETSTRUCT - 给定 HeapTuple 指针，返回用户数据地址
 */
#define GETSTRUCT(TUP) ((char *) ((TUP)->t_data) + (TUP)->t_data->t_hoff)

/*
 * 与 HeapTuple 指针配合使用的访问宏。
 */

#define HeapTupleHasNulls(tuple) \
		(((tuple)->t_data->t_infomask & HEAP_HASNULL) != 0)

#define HeapTupleNoNulls(tuple) \
		(!((tuple)->t_data->t_infomask & HEAP_HASNULL))

#define HeapTupleHasVarWidth(tuple) \
		(((tuple)->t_data->t_infomask & HEAP_HASVARWIDTH) != 0)

#define HeapTupleAllFixed(tuple) \
		(!((tuple)->t_data->t_infomask & HEAP_HASVARWIDTH))

#define HeapTupleHasExternal(tuple) \
		(((tuple)->t_data->t_infomask & HEAP_HASEXTERNAL) != 0)

#define HeapTupleIsHotUpdated(tuple) \
		HeapTupleHeaderIsHotUpdated((tuple)->t_data)

#define HeapTupleSetHotUpdated(tuple) \
		HeapTupleHeaderSetHotUpdated((tuple)->t_data)

#define HeapTupleClearHotUpdated(tuple) \
		HeapTupleHeaderClearHotUpdated((tuple)->t_data)

#define HeapTupleIsHeapOnly(tuple) \
		HeapTupleHeaderIsHeapOnly((tuple)->t_data)

#define HeapTupleSetHeapOnly(tuple) \
		HeapTupleHeaderSetHeapOnly((tuple)->t_data)

#define HeapTupleClearHeapOnly(tuple) \
		HeapTupleHeaderClearHeapOnly((tuple)->t_data)

/* common/heaptuple.c 中函数的原型 */
extern Size heap_compute_data_size(TupleDesc tupleDesc,
								   Datum *values, bool *isnull);
extern void heap_fill_tuple(TupleDesc tupleDesc,
							Datum *values, bool *isnull,
							char *data, Size data_size,
							uint16 *infomask, bits8 *bit);
extern bool heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc);
extern Datum nocachegetattr(HeapTuple tup, int attnum,
							TupleDesc tupleDesc);
extern Datum heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
							 bool *isnull);
extern Datum getmissingattr(TupleDesc tupleDesc,
							int attnum, bool *isnull);
extern HeapTuple heap_copytuple(HeapTuple tuple);
extern void heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest);
extern Datum heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc);
extern HeapTuple heap_form_tuple(TupleDesc tupleDescriptor,
								 Datum *values, bool *isnull);
extern HeapTuple heap_modify_tuple(HeapTuple tuple,
								   TupleDesc tupleDesc,
								   Datum *replValues,
								   bool *replIsnull,
								   bool *doReplace);
extern HeapTuple heap_modify_tuple_by_cols(HeapTuple tuple,
										   TupleDesc tupleDesc,
										   int nCols,
										   int *replCols,
										   Datum *replValues,
										   bool *replIsnull);
extern void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
							  Datum *values, bool *isnull);
extern void heap_freetuple(HeapTuple htup);
extern MinimalTuple heap_form_minimal_tuple(TupleDesc tupleDescriptor,
											Datum *values, bool *isnull);
extern void heap_free_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple heap_copy_minimal_tuple(MinimalTuple mtup);
extern HeapTuple heap_tuple_from_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple minimal_tuple_from_heap_tuple(HeapTuple htup);
extern size_t varsize_any(void *p);
extern HeapTuple heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);
extern MinimalTuple minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);

#ifndef FRONTEND
/*
 *	fastgetattr
 *		以 Datum 取用户属性值（可能是一个
 *		值，或指向元组数据区的指针）。
 *
 *		在可能请求系统属性时不得使用。
 *		此外，传入的 attnum 必须有效。若有疑虑请用
 *		heap_getattr()。
 *
 *		此函数常被调用，故对可缓存与 NULL 的
 *		查找用宏，其余调用 nocachegetattr()。
 */
static inline Datum
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(attnum > 0);

	*isnull = false;
	if (HeapTupleNoNulls(tup))
	{
		Form_pg_attribute att;

		att = TupleDescAttr(tupleDesc, attnum - 1);
		if (att->attcacheoff >= 0)
			return fetchatt(att, (char *) tup->t_data + tup->t_data->t_hoff +
							att->attcacheoff);
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, tup->t_data->t_bits))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
}

/*
 *	heap_getattr
 *		提取堆元组的一个属性并以 Datum 返回。
 *		系统或用户属性均可。给定的 attnum
 *	已做妥善范围检查。
 *
 *		若该字段为 NULL，则返回零 Datum
 *		并置 *isnull == true；否则置 *isnull == false。
 *
 *		<tup> 是堆元组指针。<attnum> 是调用方想要的
 *		列（字段）属性号。<tupleDesc> 是
 *		描述该行及其全部字段的结构指针。
 *
 */
static inline Datum
heap_getattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	if (attnum > 0)
	{
		if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
			return getmissingattr(tupleDesc, attnum, isnull);
		else
			return fastgetattr(tup, attnum, tupleDesc, isnull);
	}
	else
		return heap_getsysattr(tup, attnum, tupleDesc, isnull);
}
#endif							/* FRONTEND */

#endif							/* HTUP_DETAILS_H */
