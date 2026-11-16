/*-------------------------------------------------------------------------
 *
 * tid.c
 *	  内置类型 tuple id 的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tid.c
 *
 * NOTES
 *	  输入例程大部分取自 boxin()。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>

#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"


#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define NTIDARGS		2

static ItemPointer currtid_for_view(Relation viewrel, ItemPointer tid);

/* ----------------------------------------------------------------
 *		tidin
 * ----------------------------------------------------------------
 */
Datum
tidin(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	char	   *p,
			   *coord[NTIDARGS];
	int			i;
	ItemPointer result;
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;
	char	   *badp;
	unsigned long cvt;

	for (i = 0, p = str; *p && i < NTIDARGS && *p != RDELIM; p++)
		if (*p == DELIM || (*p == LDELIM && i == 0))
			coord[i++] = p + 1;

	if (i < NTIDARGS)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));

	errno = 0;
	cvt = strtoul(coord[0], &badp, 10);
	if (errno || *badp != DELIM)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));
	blockNumber = (BlockNumber) cvt;

	/*
	 * 应对 unsigned long 可能比 BlockNumber 更宽的情
	 * 形——此时对于某些超出 BlockNumber 范围的值，
	 * strtoul 不会报错。（类似代码见 oidin()。）
	 */
#if SIZEOF_LONG > 4
	if (cvt != (unsigned long) blockNumber &&
		cvt != (unsigned long) ((int32) blockNumber))
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));
#endif

	cvt = strtoul(coord[1], &badp, 10);
	if (errno || *badp != RDELIM ||
		cvt > USHRT_MAX)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"tid", str)));
	offsetNumber = (OffsetNumber) cvt;

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	ItemPointerSet(result, blockNumber, offsetNumber);

	PG_RETURN_ITEMPOINTER(result);
}

/* ----------------------------------------------------------------
 *		tidout
 * ----------------------------------------------------------------
 */
Datum
tidout(PG_FUNCTION_ARGS)
{
	ItemPointer itemPtr = PG_GETARG_ITEMPOINTER(0);
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;
	char		buf[32];

	blockNumber = ItemPointerGetBlockNumberNoCheck(itemPtr);
	offsetNumber = ItemPointerGetOffsetNumberNoCheck(itemPtr);

	/* 或许今后应把它作为记录输出。 */
	snprintf(buf, sizeof(buf), "(%u,%u)", blockNumber, offsetNumber);

	PG_RETURN_CSTRING(pstrdup(buf));
}

/*
 *		tidrecv			- 将外部二进制格式转为 tid
 */
Datum
tidrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	ItemPointer result;
	BlockNumber blockNumber;
	OffsetNumber offsetNumber;

	blockNumber = pq_getmsgint(buf, sizeof(blockNumber));
	offsetNumber = pq_getmsgint(buf, sizeof(offsetNumber));

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	ItemPointerSet(result, blockNumber, offsetNumber);

	PG_RETURN_ITEMPOINTER(result);
}

/*
 *		tidsend			- 将 tid 转为二进制格式
 */
Datum
tidsend(PG_FUNCTION_ARGS)
{
	ItemPointer itemPtr = PG_GETARG_ITEMPOINTER(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, ItemPointerGetBlockNumberNoCheck(itemPtr));
	pq_sendint16(&buf, ItemPointerGetOffsetNumberNoCheck(itemPtr));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*****************************************************************************
 *	 公共例程                  *
 *****************************************************************************/

Datum
tideq(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) == 0);
}

Datum
tidne(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) != 0);
}

Datum
tidlt(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) < 0);
}

Datum
tidle(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) <= 0);
}

Datum
tidgt(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) > 0);
}

Datum
tidge(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_BOOL(ItemPointerCompare(arg1, arg2) >= 0);
}

Datum
bttidcmp(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_INT32(ItemPointerCompare(arg1, arg2));
}

Datum
tidlarger(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_ITEMPOINTER(ItemPointerCompare(arg1, arg2) >= 0 ? arg1 : arg2);
}

Datum
tidsmaller(PG_FUNCTION_ARGS)
{
	ItemPointer arg1 = PG_GETARG_ITEMPOINTER(0);
	ItemPointer arg2 = PG_GETARG_ITEMPOINTER(1);

	PG_RETURN_ITEMPOINTER(ItemPointerCompare(arg1, arg2) <= 0 ? arg1 : arg2);
}

Datum
hashtid(PG_FUNCTION_ARGS)
{
	ItemPointer key = PG_GETARG_ITEMPOINTER(0);

	/*
	 * 虽然对于坚持给 struct ItemPointerData 追加填充
	 * 空间的编译器会麻烦多多，但我们至少能让这段
	 * 代码工作，办法是不使用 sizeof(ItemPointerData)。
	 * 而是依赖对各组成字段大小的了解。
	 */
	return hash_any((unsigned char *) key,
					sizeof(BlockIdData) + sizeof(OffsetNumber));
}

Datum
hashtidextended(PG_FUNCTION_ARGS)
{
	ItemPointer key = PG_GETARG_ITEMPOINTER(0);
	uint64		seed = PG_GETARG_INT64(1);

	/* 同上 */
	return hash_any_extended((unsigned char *) key,
							 sizeof(BlockIdData) + sizeof(OffsetNumber),
							 seed);
}


/*
 *	获取指定元组最新 tid 的函数。
 *
 *	也许这些实现应当被移到别的地方
 */

/*
 * 当前 CTID 函数的实用包装。
 *		返回关系 "rel" 中指向 "tid" 的元组的最新
 *		版本。
 */
static ItemPointer
currtid_internal(Relation rel, ItemPointer tid)
{
	ItemPointer result;
	AclResult	aclresult;
	Snapshot	snapshot;
	TableScanDesc scan;

	result = (ItemPointer) palloc(sizeof(ItemPointerData));

	aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
								  ACL_SELECT);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(rel->rd_rel->relkind),
					   RelationGetRelationName(rel));

	if (rel->rd_rel->relkind == RELKIND_VIEW)
		return currtid_for_view(rel, tid);

	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		elog(ERROR, "cannot look at latest visible tid for relation \"%s.%s\"",
			 get_namespace_name(RelationGetNamespace(rel)),
			 RelationGetRelationName(rel));

	ItemPointerCopy(tid, result);

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan_tid(rel, snapshot);
	table_tuple_get_latest_tid(scan, result);
	table_endscan(scan);
	UnregisterSnapshot(snapshot);

	return result;
}

/*
 *	处理视图的CTID。
 *		应定义在视图中，且必须对应基础
 *		关系的 CTID。
 */
static ItemPointer
currtid_for_view(Relation viewrel, ItemPointer tid)
{
	TupleDesc	att = RelationGetDescr(viewrel);
	RuleLock   *rulelock;
	RewriteRule *rewrite;
	int			i,
				natts = att->natts,
				tididx = -1;

	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(att, i);

		if (strcmp(NameStr(attr->attname), "ctid") == 0)
		{
			if (attr->atttypid != TIDOID)
				elog(ERROR, "ctid isn't of type TID");
			tididx = i;
			break;
		}
	}
	if (tididx < 0)
		elog(ERROR, "currtid cannot handle views with no CTID");
	rulelock = viewrel->rd_rules;
	if (!rulelock)
		elog(ERROR, "the view has no rules");
	for (i = 0; i < rulelock->numLocks; i++)
	{
		rewrite = rulelock->rules[i];
		if (rewrite->event == CMD_SELECT)
		{
			Query	   *query;
			TargetEntry *tle;

			if (list_length(rewrite->actions) != 1)
				elog(ERROR, "only one select rule is allowed in views");
			query = (Query *) linitial(rewrite->actions);
			tle = get_tle_by_resno(query->targetList, tididx + 1);
			if (tle && tle->expr && IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;
				RangeTblEntry *rte;

				if (!IS_SPECIAL_VARNO(var->varno) &&
					var->varattno == SelfItemPointerAttributeNumber)
				{
					rte = rt_fetch(var->varno, query->rtable);
					if (rte)
					{
						ItemPointer result;
						Relation	rel;

						rel = table_open(rte->relid, AccessShareLock);
						result = currtid_internal(rel, tid);
						table_close(rel, AccessShareLock);
						return result;
					}
				}
			}
			break;
		}
	}
	elog(ERROR, "currtid cannot handle this view");
	return NULL;
}

/*
 * currtid_byrelname
 *		获取指向某 CTID 的元组的最新元组版本，其
 *		关系名给定。
 */
Datum
currtid_byrelname(PG_FUNCTION_ARGS)
{
	text	   *relname = PG_GETARG_TEXT_PP(0);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	ItemPointer result;
	RangeVar   *relrv;
	Relation	rel;

	relrv = makeRangeVarFromNameList(textToQualifiedNameList(relname));
	rel = table_openrv(relrv, AccessShareLock);

	/* 抓取与该 CTID 关联的最新元组版本 */
	result = currtid_internal(rel, tid);

	table_close(rel, AccessShareLock);

	PG_RETURN_ITEMPOINTER(result);
}
