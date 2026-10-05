/*-------------------------------------------------------------------------
 *
 * attmap.c
 *	  属性映射支持。
 *
 * 本文件提供实用例程，通过比较输入与输出 TupleDescs
 * 来建立并管理属性映射。这类映射通常由
 * 操作继承树与分区树的 DDL 使用，以便在逻辑上等价、
 * 但列顺序不同的行类型之间做转换，同时考虑
 * 已删除的列。tupconvert.c 中的元组转换例程
 * 也会使用它们。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/attmap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/attmap.h"
#include "access/htup_details.h"
#include "utils/builtins.h"


static bool check_attrmap_match(TupleDesc indesc,
								TupleDesc outdesc,
								AttrMap *attrMap);

/*
 * make_attrmap
 *
 * 在当前内存上下文中分配一份属性映射的
 * 实用例程。
 */
AttrMap *
make_attrmap(int maplen)
{
	AttrMap    *res;

	res = (AttrMap *) palloc0(sizeof(AttrMap));
	res->maplen = maplen;
	res->attnums = (AttrNumber *) palloc0(sizeof(AttrNumber) * maplen);
	return res;
}

/*
 * free_attrmap
 *
 * 释放一份属性映射的实用例程。
 */
void
free_attrmap(AttrMap *map)
{
	pfree(map->attnums);
	pfree(map);
}

/*
 * build_attrmap_by_position
 *
 * 返回一份用 palloc 分配的、供元组转换使用的裸属性映射，按位置
 * 匹配输入列与输出列。输入与输出中的已删除列都忽略，
 * 并标为 0。它通常是 tupconvert.c 中 convert_tuples_by_position
 * 的子程序，但也可以单独使用。
 *
 * 注意：errdetail 消息把 indesc 称为 "返回的" 行类型，
 * 把 outdesc 称为 "预期的" 行类型。对当前用途这没问题，
 * 但将来也许得加以推广。
 */
AttrMap *
build_attrmap_by_position(TupleDesc indesc,
						  TupleDesc outdesc,
						  const char *msg)
{
	AttrMap    *attrMap;
	int			nincols;
	int			noutcols;
	int			n;
	int			i;
	int			j;
	bool		same;

	/*
	 * 长度按预期行类型的属性数计算，因为该计数
	 * 包含已删除的属性。
	 */
	n = outdesc->natts;
	attrMap = make_attrmap(n);

	j = 0;						/* j 是下一个物理输入属性 */
	nincols = noutcols = 0;		/* 它们统计未删除的属性 */
	same = true;
	for (i = 0; i < n; i++)
	{
		Form_pg_attribute att = TupleDescAttr(outdesc, i);
		Oid			atttypid;
		int32		atttypmod;

		if (att->attisdropped)
			continue;			/* attrMap->attnums[i] 已经是 0 */
		noutcols++;
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		for (; j < indesc->natts; j++)
		{
			att = TupleDescAttr(indesc, j);
			if (att->attisdropped)
				continue;
			nincols++;

			/* 找到匹配的列，现在检查类型 */
			if (atttypid != att->atttypid ||
				(atttypmod != att->atttypmod && atttypmod >= 0))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg_internal("%s", _(msg)),
						 errdetail("Returned type %s does not match expected type %s in column %d.",
								   format_type_with_typemod(att->atttypid,
															att->atttypmod),
								   format_type_with_typemod(atttypid,
															atttypmod),
								   noutcols)));
			attrMap->attnums[i] = (AttrNumber) (j + 1);
			j++;
			break;
		}
		if (attrMap->attnums[i] == 0)
			same = false;		/* 下面会报错 */
	}

	/* 检查未使用的输入列 */
	for (; j < indesc->natts; j++)
	{
		if (TupleDescAttr(indesc, j)->attisdropped)
			continue;
		nincols++;
		same = false;			/* 下面会报错 */
	}

	/* 用未删除列的计数报告列数不匹配 */
	if (!same)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg_internal("%s", _(msg)),
				 errdetail("Number of returned columns (%d) does not match "
						   "expected column count (%d).",
						   nincols, noutcols)));

	/* 检查该映射是否一一对应 */
	if (check_attrmap_match(indesc, outdesc, attrMap))
	{
		/* 不需要运行时转换 */
		free_attrmap(attrMap);
		return NULL;
	}

	return attrMap;
}

/*
 * build_attrmap_by_name
 *
 * 返回一份用 palloc 分配的、供元组转换使用的裸属性映射，按名称
 * 匹配输入列与输出列。（输入与输出中的已删除列都忽略。）
 * 它通常是 tupconvert.c 中 convert_tuples_by_name 的子程序，
 * 但也可以单独使用。
 *
 * 若 missing_ok 为真，outdesc 中有而 indesc 中没有的列
 * 不算错误；这种 outdesc 列的 AttrMap.attnums[] 项
 * 在那种情况下为 0。
 */
AttrMap *
build_attrmap_by_name(TupleDesc indesc,
					  TupleDesc outdesc,
					  bool missing_ok)
{
	AttrMap    *attrMap;
	int			outnatts;
	int			innatts;
	int			i;
	int			nextindesc = -1;

	outnatts = outdesc->natts;
	innatts = indesc->natts;

	attrMap = make_attrmap(outnatts);
	for (i = 0; i < outnatts; i++)
	{
		Form_pg_attribute outatt = TupleDescAttr(outdesc, i);
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		int			j;

		if (outatt->attisdropped)
			continue;			/* attrMap->attnums[i] 已经是 0 */
		attname = NameStr(outatt->attname);
		atttypid = outatt->atttypid;
		atttypmod = outatt->atttypmod;

		/*
		 * 现在在 indesc 中搜索同名属性。分区表
		 * 很可能与分区具有相同的属性顺序，因此下面的
		 * 搜索针对这种情况做了优化。有可能列在其中一个
		 * 关系中被删除，而在另一个中没有，所以我们使用
		 * nextindesc 计数器来跟踪搜索的起点。若内层
		 * 循环遇到已删除的列，就得跳过它们，但应把
		 * nextindesc 留在对下一次外层循环而言
		 * 正确的位置上，以便下一轮从那里
		 * 继续。
		 */
		for (j = 0; j < innatts; j++)
		{
			Form_pg_attribute inatt;

			nextindesc++;
			if (nextindesc >= innatts)
				nextindesc = 0;

			inatt = TupleDescAttr(indesc, nextindesc);
			if (inatt->attisdropped)
				continue;
			if (strcmp(attname, NameStr(inatt->attname)) == 0)
			{
				/* 找到了，检查类型 */
				if (atttypid != inatt->atttypid || atttypmod != inatt->atttypmod)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("could not convert row type"),
							 errdetail("Attribute \"%s\" of type %s does not match corresponding attribute of type %s.",
									   attname,
									   format_type_be(outdesc->tdtypeid),
									   format_type_be(indesc->tdtypeid))));
				attrMap->attnums[i] = inatt->attnum;
				break;
			}
		}
		if (attrMap->attnums[i] == 0 && !missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("could not convert row type"),
					 errdetail("Attribute \"%s\" of type %s does not exist in type %s.",
							   attname,
							   format_type_be(outdesc->tdtypeid),
							   format_type_be(indesc->tdtypeid))));
	}
	return attrMap;
}

/*
 * build_attrmap_by_name_if_req
 *
 * 返回 build_attrmap_by_name 建立的映射；若不需要
 * 转换则返回 NULL。这是 tupconvert.c 中 convert_tuples_by_name()
 * 及其他函数的便捷例程，但也可以
 * 单独使用。
 */
AttrMap *
build_attrmap_by_name_if_req(TupleDesc indesc,
							 TupleDesc outdesc,
							 bool missing_ok)
{
	AttrMap    *attrMap;

	/* 验证兼容性并准备属性号映射 */
	attrMap = build_attrmap_by_name(indesc, outdesc, missing_ok);

	/* 检查该映射是否一一对应 */
	if (check_attrmap_match(indesc, outdesc, attrMap))
	{
		/* 不需要运行时转换 */
		free_attrmap(attrMap);
		return NULL;
	}

	return attrMap;
}

/*
 * check_attrmap_match
 *
 * 检查该映射是否一一对应。若是，就不必做元组转换，
 * 属性映射也没有必要。
 */
static bool
check_attrmap_match(TupleDesc indesc,
					TupleDesc outdesc,
					AttrMap *attrMap)
{
	int			i;

	/* 属性数不同则不匹配 */
	if (indesc->natts != outdesc->natts)
		return false;

	for (i = 0; i < attrMap->maplen; i++)
	{
		Form_pg_attribute inatt = TupleDescAttr(indesc, i);
		Form_pg_attribute outatt = TupleDescAttr(outdesc, i);

		/*
		 * 若输入列带有缺少值的属性，就需要做转换。
		 */
		if (inatt->atthasmissing)
			return false;

		if (attrMap->attnums[i] == (i + 1))
			continue;

		/*
		 * 若它是已删除列，且对应的输入列也已删除，
		 * 就不需要转换。不过 attlen 与 attalign
		 * 必须一致。
		 */
		if (attrMap->attnums[i] == 0 &&
			inatt->attisdropped &&
			inatt->attlen == outatt->attlen &&
			inatt->attalign == outatt->attalign)
			continue;

		return false;
	}

	return true;
}
