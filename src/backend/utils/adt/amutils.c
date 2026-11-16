/*-------------------------------------------------------------------------
 *
 * amutils.c
 *	  与索引访问方法相关的 SQL 级 API。
 *
 * Copyright (c) 2016-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/amutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "utils/builtins.h"
#include "utils/syscache.h"


/* 为效率起见，把字符串属性名转成枚举 */
struct am_propname
{
	const char *name;
	IndexAMProperty prop;
};

static const struct am_propname am_propnames[] =
{
	{
		"asc", AMPROP_ASC
	},
	{
		"desc", AMPROP_DESC
	},
	{
		"nulls_first", AMPROP_NULLS_FIRST
	},
	{
		"nulls_last", AMPROP_NULLS_LAST
	},
	{
		"orderable", AMPROP_ORDERABLE
	},
	{
		"distance_orderable", AMPROP_DISTANCE_ORDERABLE
	},
	{
		"returnable", AMPROP_RETURNABLE
	},
	{
		"search_array", AMPROP_SEARCH_ARRAY
	},
	{
		"search_nulls", AMPROP_SEARCH_NULLS
	},
	{
		"clusterable", AMPROP_CLUSTERABLE
	},
	{
		"index_scan", AMPROP_INDEX_SCAN
	},
	{
		"bitmap_scan", AMPROP_BITMAP_SCAN
	},
	{
		"backward_scan", AMPROP_BACKWARD_SCAN
	},
	{
		"can_order", AMPROP_CAN_ORDER
	},
	{
		"can_unique", AMPROP_CAN_UNIQUE
	},
	{
		"can_multi_col", AMPROP_CAN_MULTI_COL
	},
	{
		"can_exclude", AMPROP_CAN_EXCLUDE
	},
	{
		"can_include", AMPROP_CAN_INCLUDE
	},
};

static IndexAMProperty
lookup_prop_name(const char *name)
{
	int			i;

	for (i = 0; i < lengthof(am_propnames); i++)
	{
		if (pg_strcasecmp(am_propnames[i].name, name) == 0)
			return am_propnames[i].prop;
	}

	/* 我们不抛出错误，以便各种 AM 能定义自己的属性 */
	return AMPROP_UNKNOWN;
}

/*
 * 那些仅对 indoptions 做位测试的属性的公共代码。
 *
 * tuple：pg_index 堆元组
 * attno：标识要测试其 indoptions 的索引列。
 * guard：若 false，则强制返回布尔假（省调用者代码）。
 * iopt_mask：关心之 indoption 位掩码。
 * iopt_expect：真结果的值（应为 0 或 iopt_mask）。
 *
 * 返回 false 表示 NULL 结果（即"未知/不适用"），
 * 否则将 *res 设为要返回的布尔值。
 */
static bool
test_indoption(HeapTuple tuple, int attno, bool guard,
			   int16 iopt_mask, int16 iopt_expect,
			   bool *res)
{
	Datum		datum;
	int2vector *indoption;
	int16		indoption_val;

	if (!guard)
	{
		*res = false;
		return true;
	}

	datum = SysCacheGetAttrNotNull(INDEXRELID, tuple, Anum_pg_index_indoption);

	indoption = ((int2vector *) DatumGetPointer(datum));
	indoption_val = indoption->values[attno - 1];

	*res = (indoption_val & iopt_mask) == iopt_expect;

	return true;
}


/*
 * 测试索引 AM、索引或索引列的属性。
 *
 * 这是不同 SQL 级函数的公共代码，因此 amoid 与
 * index_oid 参数互斥；需要时我们会从 index_oid 查出
 * amoid；若未给出索引 OID，我们查看的就是 AM 级属
 * 性。
 */
static Datum
indexam_property(FunctionCallInfo fcinfo,
				 const char *propname,
				 Oid amoid, Oid index_oid, int attno)
{
	bool		res = false;
	bool		isnull = false;
	int			natts = 0;
	IndexAMProperty prop;
	IndexAmRoutine *routine;

	/* 尝试把属性名转换为枚举（未知时不报错） */
	prop = lookup_prop_name(propname);

	/* 若有索引 OID，则查出 AM，并顺便取得列数 */
	if (OidIsValid(index_oid))
	{
		HeapTuple	tuple;
		Form_pg_class rd_rel;

		Assert(!OidIsValid(amoid));
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(index_oid));
		if (!HeapTupleIsValid(tuple))
			PG_RETURN_NULL();
		rd_rel = (Form_pg_class) GETSTRUCT(tuple);
		if (rd_rel->relkind != RELKIND_INDEX &&
			rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			ReleaseSysCache(tuple);
			PG_RETURN_NULL();
		}
		amoid = rd_rel->relam;
		natts = rd_rel->relnatts;
		ReleaseSysCache(tuple);
	}

	/*
	 * 到这里，index_oid 要么是 InvalidOid，要么是有效索
	 * 引 OID。经过这个测试和下面的测试之后，要么
	 * attno == 0（索引级或 AM 级测试），要么是有效索
	 * 引。
	 */
	if (attno < 0 || attno > natts)
		PG_RETURN_NULL();

	/*
	 * 取得 AM 信息。若无有效 AM OID，则返回 NULL。
	 */
	routine = GetIndexAmRoutineByAmId(amoid, true);
	if (routine == NULL)
		PG_RETURN_NULL();

	/*
	 * 若有 AM 属性例程，给它机会覆盖通用逻辑。若返
	 * 回 false 则继续。
	 */
	if (routine->amproperty &&
		routine->amproperty(index_oid, attno, prop, propname,
							&res, &isnull))
	{
		if (isnull)
			PG_RETURN_NULL();
		PG_RETURN_BOOL(res);
	}

	if (attno > 0)
	{
		HeapTuple	tuple;
		Form_pg_index rd_index;
		bool		iskey = true;

		/*
		 * 处理列级属性。其中许多需要 pg_index 行
		 * （也要用它检查非键属性）所以我们先取
		 * 来。
		 */
		tuple = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(index_oid));
		if (!HeapTupleIsValid(tuple))
			PG_RETURN_NULL();
		rd_index = (Form_pg_index) GETSTRUCT(tuple);

		Assert(index_oid == rd_index->indexrelid);
		Assert(attno > 0 && attno <= rd_index->indnatts);

		isnull = true;

		/*
		 * 若 amcaninclude，看的可能是非键列的
		 * attno，我们（一般地）假定其大多数属性为
		 * 空
		 */
		if (routine->amcaninclude
			&& attno > rd_index->indnkeyatts)
			iskey = false;

		switch (prop)
		{
			case AMPROP_ASC:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_DESC, 0, &res))
					isnull = false;
				break;

			case AMPROP_DESC:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_DESC, INDOPTION_DESC, &res))
					isnull = false;
				break;

			case AMPROP_NULLS_FIRST:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_NULLS_FIRST, INDOPTION_NULLS_FIRST, &res))
					isnull = false;
				break;

			case AMPROP_NULLS_LAST:
				if (iskey &&
					test_indoption(tuple, attno, routine->amcanorder,
								   INDOPTION_NULLS_FIRST, 0, &res))
					isnull = false;
				break;

			case AMPROP_ORDERABLE:

				/*
				 * 通用假定是非键列不可排序
				 */
				res = iskey ? routine->amcanorder : false;
				isnull = false;
				break;

			case AMPROP_DISTANCE_ORDERABLE:

				/*
				 * 一列是否可按距离排序，条件完全取决于
				 * AM（撰写时只有 GiST 支持）。规划器有自
				 * 己的想法：能否找到 amoppurpose 为 'o' 的
				 * 算子，但仅凭索引列类型就到那步似乎
				 * 太费事。所以我们期望 AM 在其 amproperty
				 * 例程中来处理。通用结果是：若 AM 说它从
				 * 不支持此属性，或者这是非键列，则返
				 * 回 false；否则返回空值，也就是说我们
				 * 并不知
				 * 情。
				 */
				if (!iskey || !routine->amcanorderbyop)
				{
					res = false;
					isnull = false;
				}
				break;

			case AMPROP_RETURNABLE:

				/* 注意对这个属性我们忽略 iskey */

				isnull = false;
				res = false;

				if (routine->amcanreturn)
				{
					/*
					 * 如有可能，AM 应该在其 amproperty
					 * 函数中做此测试而不打开 rel。
					 * 若它不做，这就是通用回退。
					 */
					Relation	indexrel = index_open(index_oid, AccessShareLock);

					res = index_can_return(indexrel, attno);
					index_close(indexrel, AccessShareLock);
				}
				break;

			case AMPROP_SEARCH_ARRAY:
				if (iskey)
				{
					res = routine->amsearcharray;
					isnull = false;
				}
				break;

			case AMPROP_SEARCH_NULLS:
				if (iskey)
				{
					res = routine->amsearchnulls;
					isnull = false;
				}
				break;

			default:
				break;
		}

		ReleaseSysCache(tuple);

		if (!isnull)
			PG_RETURN_BOOL(res);
		PG_RETURN_NULL();
	}

	if (OidIsValid(index_oid))
	{
		/*
		 * 处理索引级属性。目前这些只取决于 AM，但可能
		 * 不会永远如此，所以我们让用户指定索引而不
		 * 只是 AM。
		 */
		switch (prop)
		{
			case AMPROP_CLUSTERABLE:
				PG_RETURN_BOOL(routine->amclusterable);

			case AMPROP_INDEX_SCAN:
				PG_RETURN_BOOL(routine->amgettuple ? true : false);

			case AMPROP_BITMAP_SCAN:
				PG_RETURN_BOOL(routine->amgetbitmap ? true : false);

			case AMPROP_BACKWARD_SCAN:
				PG_RETURN_BOOL(routine->amcanbackward);

			default:
				PG_RETURN_NULL();
		}
	}

	/*
	 * 处理 AM 级属性（即那些控制你在 CREATE INDEX 中
	 * 所说）。
	 */
	switch (prop)
	{
		case AMPROP_CAN_ORDER:
			PG_RETURN_BOOL(routine->amcanorder);

		case AMPROP_CAN_UNIQUE:
			PG_RETURN_BOOL(routine->amcanunique);

		case AMPROP_CAN_MULTI_COL:
			PG_RETURN_BOOL(routine->amcanmulticol);

		case AMPROP_CAN_EXCLUDE:
			PG_RETURN_BOOL(routine->amgettuple ? true : false);

		case AMPROP_CAN_INCLUDE:
			PG_RETURN_BOOL(routine->amcaninclude);

		default:
			PG_RETURN_NULL();
	}
}

/*
 * 测试 AM OID 指定的 AM 的属性
 */
Datum
pg_indexam_has_property(PG_FUNCTION_ARGS)
{
	Oid			amoid = PG_GETARG_OID(0);
	char	   *propname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	return indexam_property(fcinfo, propname, amoid, InvalidOid, 0);
}

/*
 * 测试索引 OID 指定的索引的属性
 */
Datum
pg_index_has_property(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *propname = text_to_cstring(PG_GETARG_TEXT_PP(1));

	return indexam_property(fcinfo, propname, InvalidOid, relid, 0);
}

/*
 * 测试索引 OID 与列号指定的索引列的属性
 */
Datum
pg_index_column_has_property(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int32		attno = PG_GETARG_INT32(1);
	char	   *propname = text_to_cstring(PG_GETARG_TEXT_PP(2));

	/* 立即拒绝 attno 0，这样 attno > 0 就能标识此情形 */
	if (attno <= 0)
		PG_RETURN_NULL();

	return indexam_property(fcinfo, propname, InvalidOid, relid, attno);
}

/*
 * 按给定 AM 进度报告时的用法，返回给定阶段名
 * 字。
 */
Datum
pg_indexam_progress_phasename(PG_FUNCTION_ARGS)
{
	Oid			amoid = PG_GETARG_OID(0);
	int32		phasenum = PG_GETARG_INT32(1);
	IndexAmRoutine *routine;
	char	   *name;

	routine = GetIndexAmRoutineByAmId(amoid, true);
	if (routine == NULL || !routine->ambuildphasename)
		PG_RETURN_NULL();

	name = routine->ambuildphasename(phasenum);
	if (!name)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(CStringGetTextDatum(name));
}
