/*-------------------------------------------------------------------------
 *
 * sortsupport.h
 *	  加速排序的框架。
 *
 * 传统上 PostgreSQL 通过对待比较值对反复调用 SQL
 * 可调用比较函数 "cmp(x, y) returns int" 来排序，该
 * 函数是相应 btree 索引 opclass 的 BTORDER_PROC
 * pg_amproc 支持函数。
 *
 * 本文件定义可降低开销的替代 API。为支持低开销排
 * 序，btree 操作符类可提供 BTSORTSUPPORT_PROC
 * pg_amproc 条目，须接受单个 internal 参数并返回
 * void。该参数实际是指向下方 SortSupportData 结构
 * 的指针。
 *
 * 若提供，BTSORTSUPPORT 会在排序设置时调用，且必须
 * 用可执行排序的函数指针初始化所给结构。本 API
 * 允许支持多种加速机制，但 opclass 不必全部提供。
 * BTSORTSUPPORT 对不支持的机制可不设函数指针。
 * 提供 BTSORTSUPPORT 但不提供比较器的 opclass 会
 * 由排序支持自动设 shim。但支持可选缩写键能力
 * 的 opclass 必须始终提供权威比较器，用于平局判定
 * 不确定的缩写比较，并在中止缩写时使用。此外必须
 * 提供转换器与中止/代价函数。
 * 见上。
 * 见上。
 * 见上。
 * 见上。
 *
 * 所有排序支持函数被调用时都会传入
 * SortSupportData 地址，以便按需存额外私有数据。
 * 尤其对 collation 敏感类型，调用 BTSORTSUPPORT 前
 * 会设好 ssup_collation，且对所有支持函数可用。额外
 * opclass 相关数据可用 ssup_extra 存放，应分配在
 * ssup_cxt 内存上下文中。
 * 见上。
 *
 * 注意：因 pg_amproc 按 (lefttype, righttype) 索引，
 * 可将 BTSORTSUPPORT 与跨类型比较关联。这可合理用于
 * 为此类情形提供快速比较器，但大概不适合其他加速
 * 方法。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sortsupport.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SORTSUPPORT_H
#define SORTSUPPORT_H

#include "access/attnum.h"
#include "utils/relcache.h"

typedef struct SortSupportData *SortSupport;

typedef struct SortSupportData
{
	/*
	 * 这些字段在调用 BTSORTSUPPORT 前初始化，之后不应
	 * 再改。
	 */
	MemoryContext ssup_cxt;		/* 含排序信息的上下文 */
	Oid			ssup_collation; /* collation，或 InvalidOid */

	/*
	 * 额外排序参数；但与 ssup_collation 不同，这些可在
	 * 调用 BTSORTSUPPORT 后更改，故勿用其选择排序
	 * 支持函数。
	 */
	bool		ssup_reverse;	/* 降序排序？ */
	bool		ssup_nulls_first;	/* 空值排前？ */

	/*
	 * 这些字段是调用方工作区，opclass 函数勿触碰。
	 * 见上。
	 */
	AttrNumber	ssup_attno;		/* 要排序的列号 */

	/*
	 * ssup_extra 在调用 BTSORTSUPPORT 前清零，之后调用
	 * 方不再触碰。
	 */
	void	   *ssup_extra;		/* opclass 函数的工作区 */

	/*
	 * 函数指针在调用 BTSORTSUPPORT 前清零，由其设置想
	 * 提供的加速方法。比较器指针必须设置，其余可选。
	 * 见上。
	 */

	/*
	 * 比较器 API 与传统 btree 比较函数相同，即按 x
	 * 小于、等于或大于 y 返回 <0、0 或 >0。注意 x、y
	 * 保证非空，也无法返回空。
	 * 见上。
	 *
	 * 可以是权威或缩写比较器。尽管 opclass 支持函数
	 * 最初表明适用缩写，核心仍可改回权威比较器。
	 * 见上。
	 * 见上。
	 */
	int			(*comparator) (Datum x, Datum y, SortSupport ssup);

	/*
	 * 以下为"缩写键"基础设施。
	 *
	 * 使用此可选附加设施的操作符类须设置全部
	 * 回调（除非因故不进行缩写，此时勿设置
	 * abbrev_converter）。
	 * 见上。
	 *
	 * 这允许 opclass 作者提供转换例程，用以创建
	 * 底层类型替代表示（"缩写键"）。须按值传递，
	 * 通常用仅 opclass 知晓的专用格式。还必须提供
	 * 仅用于此替代表示的备选比较器（赋给
	 * "comparator"）。此表示是原 Datum 的简单近似。
	 * 须能用备选比较器互比，且非零返回值须可靠代理
	 * 正式比较结果。返回零并不像传统支持例程 1 那
	 * 样表示相等——而表示无法判定两缩写值如何
	 * 比较。因此须用 "abbrev_full_comparator"/
	 * ApplySortAbbrevFullComparator() 做正式比较。
	 * 许多情况下多数比较只用廉价备选函数，通常
	 * 为仅几条 CPU 指令。缓存未命中代价高；要获
	 * 良好性能，排序基础设施必须重权缓存表现。
	 * 见上。
	 * 见上。
	 * 见上。
	 * 见上。
	 * 见上。
	 * 见上。
	 * 见上。
	 * 见上。
	 *
	 * opclass 作者设计编码方案时须考虑缩写键最终基数。某
	 * 策略在一种用法下可能优于另一策略，而另一种用法
	 * 下则相反。所有这些因素都必须考虑。
	 * 见上。
	 * 见上。
	 */

	/*
	 * "abbreviate" 关乎缩写键优化原则上是否适用（即
	 * sortsupport 例程须知是否在处理能有益地打包缩写表
	 * 示的键。按惯例这是前导属性键）。但注意核心代码
	 * 总是通过检查 opclass 是否设置了 abbrev_converter
	 * 来判定缩写是否启用。这是对 opclass 的单向一次
	 * 消息。
	 * 见上。
	 * 见上。
	 */
	bool		abbreviate;

	/*
	 * 从原始表示到缩写格式的转换器。核心用此回调把
	 * 按引用"原始" Datum 转为按值缩写键 Datum。注意
	 * original 保证非 NULL，因把 NULLness 计入专用代价
	 * 模型无意义。
	 * 见上。
	 *
	 * 用 abbrev_converter 检测是否启用缩写。核心可将其置
	 * NULL 表示不应使用缩写（例程不必关心）。但若
	 * 立即确定不应缩写（例如 !abbreviate 调用，或
	 * 平台阻碍），例程勿设置它。
	 * 见上。
	 * 见上。
	 * 见上。
	 */
	Datum		(*abbrev_converter) (Datum original, SortSupport ssup);

	/*
	 * abbrev_abort 回调让客户用专用代价模型验证
	 * 当前策略是否奏效。若实践中缩写键大量重复，
	 * 宜在转换代价过高前放弃（或许某些 opclass
	 * 专用适配也有用）。
	 * 见上。
	 * 见上。
	 */
	bool		(*abbrev_abort) (int memtupcount, SortSupport ssup);

	/*
	 * 为其生成缩写表示之键的权威比较器，在
	 * 缩写比较不确定时调用
	 * ApplySortAbbrevFullComparator()，或在核心最终
	 * 反对缩写时用来替换 "comparator"。
	 * 见上。
	 */
	int			(*abbrev_full_comparator) (Datum x, Datum y, SortSupport ssup);
} SortSupportData;


/*
 * 应用排序比较器并返回三路比较结果。
 * 负责正确处理逆序与 NULLs 次序。
 */
static inline int
ApplySortComparator(Datum datum1, bool isNull1,
					Datum datum2, bool isNull2,
					SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = ssup->comparator(datum1, datum2, ssup);
		if (ssup->ssup_reverse)
			INVERT_COMPARE_RESULT(compare);
	}

	return compare;
}

static inline int
ApplyUnsignedSortComparator(Datum datum1, bool isNull1,
							Datum datum2, bool isNull2,
							SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = datum1 < datum2 ? -1 : datum1 > datum2 ? 1 : 0;
		if (ssup->ssup_reverse)
			INVERT_COMPARE_RESULT(compare);
	}

	return compare;
}

#if SIZEOF_DATUM >= 8
static inline int
ApplySignedSortComparator(Datum datum1, bool isNull1,
						  Datum datum2, bool isNull2,
						  SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = DatumGetInt64(datum1) < DatumGetInt64(datum2) ? -1 :
			DatumGetInt64(datum1) > DatumGetInt64(datum2) ? 1 : 0;
		if (ssup->ssup_reverse)
			INVERT_COMPARE_RESULT(compare);
	}

	return compare;
}
#endif

static inline int
ApplyInt32SortComparator(Datum datum1, bool isNull1,
						 Datum datum2, bool isNull2,
						 SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = DatumGetInt32(datum1) < DatumGetInt32(datum2) ? -1 :
			DatumGetInt32(datum1) > DatumGetInt32(datum2) ? 1 : 0;
		if (ssup->ssup_reverse)
			INVERT_COMPARE_RESULT(compare);
	}

	return compare;
}

/*
 * 应用排序比较器并用完整权威比较器返回三路比较。
 * 负责正确处理逆序与 NULLs 次序。
 * 见上。
 */
static inline int
ApplySortAbbrevFullComparator(Datum datum1, bool isNull1,
							  Datum datum2, bool isNull2,
							  SortSupport ssup)
{
	int			compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (ssup->ssup_nulls_first)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (ssup->ssup_nulls_first)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = ssup->abbrev_full_comparator(datum1, datum2, ssup);
		if (ssup->ssup_reverse)
			INVERT_COMPARE_RESULT(compare);
	}

	return compare;
}

/*
 * 我们有专用排序例程的 Datum 比较函数。
 * 将其设为比较器或缩写比较器的数据类型有资格获得
 * 更快排序。
 */
extern int	ssup_datum_unsigned_cmp(Datum x, Datum y, SortSupport ssup);
#if SIZEOF_DATUM >= 8
extern int	ssup_datum_signed_cmp(Datum x, Datum y, SortSupport ssup);
#endif
extern int	ssup_datum_int32_cmp(Datum x, Datum y, SortSupport ssup);

/* utils/sort/sortsupport.c 中的其他函数 */
extern void PrepareSortSupportComparisonShim(Oid cmpFunc, SortSupport ssup);
extern void PrepareSortSupportFromOrderingOp(Oid orderingOp, SortSupport ssup);
extern void PrepareSortSupportFromIndexRel(Relation indexRel, int16 strategy,
										   SortSupport ssup);
extern void PrepareSortSupportFromGistIndexRel(Relation indexRel, SortSupport ssup);

#endif							/* SORTSUPPORT_H */
