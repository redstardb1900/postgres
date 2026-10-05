/*-------------------------------------------------------------------------
 *
 * scankey.c
 *	  扫描键支持代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/scankey.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/skey.h"
#include "catalog/pg_collation.h"


/*
 * ScanKeyEntryInitialize
 *		用给出的全部字段值初始化一个扫描键项。
 *		目标过程由 OID 指定（但若设置了
 *		SK_SEARCHNULL 或 SK_SEARCHNOTNULL，则它可以无效）。
 *
 * 注意：调用时的 CurrentMemoryContext 应与 ScanKey
 * 本身一样长寿，因为挂在 ScanKey 的 FmgrInfo 记录上的
 * 任何附属信息都会使用它。
 */
void
ScanKeyEntryInitialize(ScanKey entry,
					   int flags,
					   AttrNumber attributeNumber,
					   StrategyNumber strategy,
					   Oid subtype,
					   Oid collation,
					   RegProcedure procedure,
					   Datum argument)
{
	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_subtype = subtype;
	entry->sk_collation = collation;
	entry->sk_argument = argument;
	if (RegProcedureIsValid(procedure))
	{
		fmgr_info(procedure, &entry->sk_func);
	}
	else
	{
		Assert(flags & (SK_SEARCHNULL | SK_SEARCHNOTNULL));
		MemSet(&entry->sk_func, 0, sizeof(entry->sk_func));
	}
}

/*
 * ScanKeyInit
 *		ScanKeyEntryInitialize 的简写：假定 flags 与 subtype
 *		为零（通常如此），排序规则取默认值。
 *
 * 这是系统目录中硬编码查找的推荐版本。
 * 它无法处理 NULL 参数、一元操作符或非默认操作符，
 * 但大多数硬编码查找并不需要这些功能。
 *
 * 我们总是把排序规则设为 C_COLLATION_OID。对系统目录中
 * 所有感知排序规则的列，这都是正确值，对其余列类型
 * 则会被忽略，因此不值得再讲究。
 *
 * 注意：调用时的 CurrentMemoryContext 应与 ScanKey
 * 本身一样长寿，因为挂在 ScanKey 的 FmgrInfo 记录上的
 * 任何附属信息都会使用它。
 */
void
ScanKeyInit(ScanKey entry,
			AttrNumber attributeNumber,
			StrategyNumber strategy,
			RegProcedure procedure,
			Datum argument)
{
	entry->sk_flags = 0;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_subtype = InvalidOid;
	entry->sk_collation = C_COLLATION_OID;
	entry->sk_argument = argument;
	fmgr_info(procedure, &entry->sk_func);
}

/*
 * ScanKeyEntryInitializeWithInfo
 *		用一份已经完成的 FmgrInfo 函数查找记录
 *		初始化扫描键项。
 *
 * 注意：调用时的 CurrentMemoryContext 应与 ScanKey
 * 本身一样长寿，因为挂在 ScanKey 的 FmgrInfo 记录上的
 * 任何附属信息都会使用它。
 */
void
ScanKeyEntryInitializeWithInfo(ScanKey entry,
							   int flags,
							   AttrNumber attributeNumber,
							   StrategyNumber strategy,
							   Oid subtype,
							   Oid collation,
							   FmgrInfo *finfo,
							   Datum argument)
{
	entry->sk_flags = flags;
	entry->sk_attno = attributeNumber;
	entry->sk_strategy = strategy;
	entry->sk_subtype = subtype;
	entry->sk_collation = collation;
	entry->sk_argument = argument;
	fmgr_info_copy(&entry->sk_func, finfo, CurrentMemoryContext);
}
