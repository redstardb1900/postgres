/*-------------------------------------------------------------------------
 *
 * nbtcompare.c
 *	  btree 访问方法的比较函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtcompare.c
 *
 * NOTES
 *
 *	这些函数存放在 pg_amproc 中。对定义在 B-树
 *	上的每个操作符类，它们计算
 *
 *				compare(a, b)：
 *						若 a < b，则 < 0，
 *						若 a == b，则 = 0，
 *						若 a > b，则 > 0。
 *
 *	结果始终是 int32，与输入数据类型无关。
 *
 *	虽然任何负的 int32 都可以用来报告 "<"，
 *	任何正的 int32 都可以用来报告 ">"，但处理
 *	32 位或更宽数据类型的例程不能直接返回 "a - b"。
 *	那样可能溢出并给出错误答案。
 *
 *	注意：比较函数必须对数据类型的所有非 NULL
 *	值施加全序，而且该数据类型的布尔比较操作符
 *	（= < >= 等）必须给出与比较例程一致的结果。
 *	否则可能出现不良行为。
 *	（例如，比较操作符面对 NAN 或其他奇怪的值时
 *	绝不能放弃处理；必须为所有这些值设计某种
 *	排序规则序列。）若数据类型并不简单，最可靠的
 *	做法是让布尔操作符调用与 btree 函数相同的
 *	三路比较代码。因此，本文件只包含 "简单"
 *	数据类型的 btree 支持 --- 其余都在实现其
 *	数据类型的 /utils/adt/ 文件中。
 *
 *	注意：这些例程不得泄漏内存，因为索引访问期间
 *	分配的内存要到查询结束才会回收。这主要影响
 *	可 TOAST 的数据类型的比较例程；它们必须小心
 *	释放输入 datum 的任何已解除 TOAST 的副本。
 *
 *	注意：我们曾经禁止比较函数返回 INT_MIN，
 *	但事实证明这太容易出错，因为某些平台的
 *	memcmp() 等版本可能返回 INT_MIN。作为对调用者
 *	做压力测试的手段，本文件可在定义
 *	STRESS_SORT_INT_MIN 的情况下编译，使许多函数返回
 *	INT_MIN 或 INT_MAX，而不是惯常的 -1/+1。不过
 *	用于生产时这不是个好主意，因为用户或第三方代码可能期望传统结果。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "utils/builtins.h"
#include "utils/sortsupport.h"

#ifdef STRESS_SORT_INT_MIN
#define A_LESS_THAN_B		INT_MIN
#define A_GREATER_THAN_B	INT_MAX
#else
#define A_LESS_THAN_B		(-1)
#define A_GREATER_THAN_B	1
#endif


Datum
btboolcmp(PG_FUNCTION_ARGS)
{
	bool		a = PG_GETARG_BOOL(0);
	bool		b = PG_GETARG_BOOL(1);

	PG_RETURN_INT32((int32) a - (int32) b);
}

Datum
btint2cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int16		b = PG_GETARG_INT16(1);

	PG_RETURN_INT32((int32) a - (int32) b);
}

static int
btint2fastcmp(Datum x, Datum y, SortSupport ssup)
{
	int16		a = DatumGetInt16(x);
	int16		b = DatumGetInt16(y);

	return (int) a - (int) b;
}

Datum
btint2sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = btint2fastcmp;
	PG_RETURN_VOID();
}

Datum
btint4cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btint4sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = ssup_datum_int32_cmp;
	PG_RETURN_VOID();
}

Datum
btint8cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

#if SIZEOF_DATUM < 8
static int
btint8fastcmp(Datum x, Datum y, SortSupport ssup)
{
	int64		a = DatumGetInt64(x);
	int64		b = DatumGetInt64(y);

	if (a > b)
		return A_GREATER_THAN_B;
	else if (a == b)
		return 0;
	else
		return A_LESS_THAN_B;
}
#endif

Datum
btint8sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

#if SIZEOF_DATUM >= 8
	ssup->comparator = ssup_datum_signed_cmp;
#else
	ssup->comparator = btint8fastcmp;
#endif
	PG_RETURN_VOID();
}

Datum
btint48cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btint84cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btint24cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int32		b = PG_GETARG_INT32(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btint42cmp(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int16		b = PG_GETARG_INT16(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btint28cmp(PG_FUNCTION_ARGS)
{
	int16		a = PG_GETARG_INT16(0);
	int64		b = PG_GETARG_INT64(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btint82cmp(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int16		b = PG_GETARG_INT16(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

Datum
btoidcmp(PG_FUNCTION_ARGS)
{
	Oid			a = PG_GETARG_OID(0);
	Oid			b = PG_GETARG_OID(1);

	if (a > b)
		PG_RETURN_INT32(A_GREATER_THAN_B);
	else if (a == b)
		PG_RETURN_INT32(0);
	else
		PG_RETURN_INT32(A_LESS_THAN_B);
}

static int
btoidfastcmp(Datum x, Datum y, SortSupport ssup)
{
	Oid			a = DatumGetObjectId(x);
	Oid			b = DatumGetObjectId(y);

	if (a > b)
		return A_GREATER_THAN_B;
	else if (a == b)
		return 0;
	else
		return A_LESS_THAN_B;
}

Datum
btoidsortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = btoidfastcmp;
	PG_RETURN_VOID();
}

Datum
btoidvectorcmp(PG_FUNCTION_ARGS)
{
	oidvector  *a = (oidvector *) PG_GETARG_POINTER(0);
	oidvector  *b = (oidvector *) PG_GETARG_POINTER(1);
	int			i;

	/* 我们任意地选择先按向量长度排序 */
	if (a->dim1 != b->dim1)
		PG_RETURN_INT32(a->dim1 - b->dim1);

	for (i = 0; i < a->dim1; i++)
	{
		if (a->values[i] != b->values[i])
		{
			if (a->values[i] > b->values[i])
				PG_RETURN_INT32(A_GREATER_THAN_B);
			else
				PG_RETURN_INT32(A_LESS_THAN_B);
		}
	}
	PG_RETURN_INT32(0);
}

Datum
btcharcmp(PG_FUNCTION_ARGS)
{
	char		a = PG_GETARG_CHAR(0);
	char		b = PG_GETARG_CHAR(1);

	/* 比较字符时要小心按无符号处理 */
	PG_RETURN_INT32((int32) ((uint8) a) - (int32) ((uint8) b));
}
