/*-------------------------------------------------------------------------
 *
 * tupdesc.c
 *	  POSTGRES 元组描述符支持代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/tupdesc.c
 *
 * 说明
 *	  执行器的一些实用代码，例如 "ExecTypeFromTL"，应当
 *	  移到这里。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/toast_compression.h"
#include "access/tupdesc_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/resowner_private.h"
#include "utils/syscache.h"


/*
 * CreateTemplateTupleDesc
 *		本函数分配一个空的元组描述符结构。
 *
 * 元组类型 ID 信息最初按匿名复合类型设置；
 * 调用方可以在需要时覆盖。
 */
TupleDesc
CreateTemplateTupleDesc(int natts)
{
	TupleDesc	desc;

	/*
	 * 健全性检查
	 */
	Assert(natts >= 0);

	/*
	 * 为元组描述符分配足够内存，其中包括
	 * 属性行。
	 *
	 * 注意：属性数组的步长是 sizeof(FormData_pg_attribute)，
	 * 因为为了书写方便，数组元素被声明为
	 * FormData_pg_attribute。不过我们只保证每一项的前
	 * ATTRIBUTE_FIXED_PART_SIZE 个字节有效；大多数复制
	 * tupdesc 项的代码也只复制那么多。原则上由于尾部填充，
	 * 这个数量可能更少，尽管按照 pg_attribute 的当前定义，
	 * 大概并没有任何填充。
	 */
	desc = (TupleDesc) palloc(offsetof(struct TupleDescData, attrs) +
							  natts * sizeof(FormData_pg_attribute));

	/*
	 * 初始化 tupdesc 的其他字段。
	 */
	desc->natts = natts;
	desc->constr = NULL;
	desc->tdtypeid = RECORDOID;
	desc->tdtypmod = -1;
	desc->tdrefcount = -1;		/* 假定不做引用计数 */

	return desc;
}

/*
 * CreateTupleDesc
 *		本函数复制给定的 Form_pg_attribute 数组，
 *		从而分配一个新的 TupleDesc。
 *
 * 元组类型 ID 信息最初按匿名复合类型设置；
 * 调用方可以在需要时覆盖。
 */
TupleDesc
CreateTupleDesc(int natts, Form_pg_attribute *attrs)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(natts);

	for (i = 0; i < natts; ++i)
		memcpy(TupleDescAttr(desc, i), attrs[i], ATTRIBUTE_FIXED_PART_SIZE);

	return desc;
}

/*
 * CreateTupleDescCopy
 *		本函数从已有的
 *		TupleDesc 复制出新的 TupleDesc。
 *
 * !!! 不复制约束和默认值 !!!
 */
TupleDesc
CreateTupleDescCopy(TupleDesc tupdesc)
{
	TupleDesc	desc;
	int			i;

	desc = CreateTemplateTupleDesc(tupdesc->natts);

	/* 整段复制属性数组 */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	/*
	 * 既然不复制约束和默认值，就清除
	 * 与它们相关联的字段。
	 */
	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';
	}

	/* 元组类型标识也可以一并复制 */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * CreateTupleDescCopyConstr
 *		本函数从已有的
 *		TupleDesc（包括其约束和默认值）复制出新的 TupleDesc。
 */
TupleDesc
CreateTupleDescCopyConstr(TupleDesc tupdesc)
{
	TupleDesc	desc;
	TupleConstr *constr = tupdesc->constr;
	int			i;

	desc = CreateTemplateTupleDesc(tupdesc->natts);

	/* 整段复制属性数组 */
	memcpy(TupleDescAttr(desc, 0),
		   TupleDescAttr(tupdesc, 0),
		   desc->natts * sizeof(FormData_pg_attribute));

	/* 若有 TupleConstr 数据结构，则复制它 */
	if (constr)
	{
		TupleConstr *cpy = (TupleConstr *) palloc0(sizeof(TupleConstr));

		cpy->has_not_null = constr->has_not_null;
		cpy->has_generated_stored = constr->has_generated_stored;

		if ((cpy->num_defval = constr->num_defval) > 0)
		{
			cpy->defval = (AttrDefault *) palloc(cpy->num_defval * sizeof(AttrDefault));
			memcpy(cpy->defval, constr->defval, cpy->num_defval * sizeof(AttrDefault));
			for (i = cpy->num_defval - 1; i >= 0; i--)
				cpy->defval[i].adbin = pstrdup(constr->defval[i].adbin);
		}

		if (constr->missing)
		{
			cpy->missing = (AttrMissing *) palloc(tupdesc->natts * sizeof(AttrMissing));
			memcpy(cpy->missing, constr->missing, tupdesc->natts * sizeof(AttrMissing));
			for (i = tupdesc->natts - 1; i >= 0; i--)
			{
				if (constr->missing[i].am_present)
				{
					Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

					cpy->missing[i].am_value = datumCopy(constr->missing[i].am_value,
														 attr->attbyval,
														 attr->attlen);
				}
			}
		}

		if ((cpy->num_check = constr->num_check) > 0)
		{
			cpy->check = (ConstrCheck *) palloc(cpy->num_check * sizeof(ConstrCheck));
			memcpy(cpy->check, constr->check, cpy->num_check * sizeof(ConstrCheck));
			for (i = cpy->num_check - 1; i >= 0; i--)
			{
				cpy->check[i].ccname = pstrdup(constr->check[i].ccname);
				cpy->check[i].ccbin = pstrdup(constr->check[i].ccbin);
				cpy->check[i].ccvalid = constr->check[i].ccvalid;
				cpy->check[i].ccnoinherit = constr->check[i].ccnoinherit;
			}
		}

		desc->constr = cpy;
	}

	/* 元组类型标识也可以一并复制 */
	desc->tdtypeid = tupdesc->tdtypeid;
	desc->tdtypmod = tupdesc->tdtypmod;

	return desc;
}

/*
 * TupleDescCopy
 *		把元组描述符复制到调用方提供的内存中。
 *		该内存可以是映射在任意地址的共享内存，并且必须
 *		足以容纳 TupleDescSize(src) 个字节。
 *
 * !!! 不复制约束和默认值 !!!
 */
void
TupleDescCopy(TupleDesc dst, TupleDesc src)
{
	int			i;

	/* 整段复制头部和属性数组 */
	memcpy(dst, src, TupleDescSize(src));

	/*
	 * 既然不复制约束和默认值，就清除
	 * 与它们相关联的字段。
	 */
	for (i = 0; i < dst->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(dst, i);

		att->attnotnull = false;
		att->atthasdef = false;
		att->atthasmissing = false;
		att->attidentity = '\0';
		att->attgenerated = '\0';
	}
	dst->constr = NULL;

	/*
	 * 另外，假定目标不做引用计数。（无论如何，复制源的
	 * refcount 都是错的。）
	 */
	dst->tdrefcount = -1;
}

/*
 * TupleDescCopyEntry
 *		本函数把单个属性结构从一个元组描述符
 *		复制到另一个。
 *
 * !!! 不复制约束和默认值 !!!
 */
void
TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
				   TupleDesc src, AttrNumber srcAttno)
{
	Form_pg_attribute dstAtt = TupleDescAttr(dst, dstAttno - 1);
	Form_pg_attribute srcAtt = TupleDescAttr(src, srcAttno - 1);

	/*
	 * 健全性检查
	 */
	Assert(PointerIsValid(src));
	Assert(PointerIsValid(dst));
	Assert(srcAttno >= 1);
	Assert(srcAttno <= src->natts);
	Assert(dstAttno >= 1);
	Assert(dstAttno <= dst->natts);

	memcpy(dstAtt, srcAtt, ATTRIBUTE_FIXED_PART_SIZE);

	/*
	 * 除了更新 attno，最好把 attcacheoff 重置一下。
	 *
	 * XXX 实际上，要完全安全的话，还需要重置 dst 中
	 * 所有后续列的 attcacheoff。不过当前的使用场景并不需要
	 * 那样做，因为所有后续列都会由本函数的其他使用或
	 * TupleDescInitEntry 初始化。因此我们偷一点懒，
	 * 以避免无谓的 O(N^2) 代价。
	 */
	dstAtt->attnum = dstAttno;
	dstAtt->attcacheoff = -1;

	/* 既然不复制约束或默认值，就清除这些标志 */
	dstAtt->attnotnull = false;
	dstAtt->atthasdef = false;
	dstAtt->atthasmissing = false;
	dstAtt->attidentity = '\0';
	dstAtt->attgenerated = '\0';
}

/*
 * 释放 TupleDesc 及其全部子结构
 */
void
FreeTupleDesc(TupleDesc tupdesc)
{
	int			i;

	/*
	 * 也许这里应当断言 tdrefcount == 0，以禁止显式释放
	 * 未做引用计数的 tupdesc？
	 */
	Assert(tupdesc->tdrefcount <= 0);

	if (tupdesc->constr)
	{
		if (tupdesc->constr->num_defval > 0)
		{
			AttrDefault *attrdef = tupdesc->constr->defval;

			for (i = tupdesc->constr->num_defval - 1; i >= 0; i--)
				pfree(attrdef[i].adbin);
			pfree(attrdef);
		}
		if (tupdesc->constr->missing)
		{
			AttrMissing *attrmiss = tupdesc->constr->missing;

			for (i = tupdesc->natts - 1; i >= 0; i--)
			{
				if (attrmiss[i].am_present
					&& !TupleDescAttr(tupdesc, i)->attbyval)
					pfree(DatumGetPointer(attrmiss[i].am_value));
			}
			pfree(attrmiss);
		}
		if (tupdesc->constr->num_check > 0)
		{
			ConstrCheck *check = tupdesc->constr->check;

			for (i = tupdesc->constr->num_check - 1; i >= 0; i--)
			{
				pfree(check[i].ccname);
				pfree(check[i].ccbin);
			}
			pfree(check);
		}
		pfree(tupdesc->constr);
	}

	pfree(tupdesc);
}

/*
 * 增加 tupdesc 的引用计数，并把该引用记入
 * CurrentResourceOwner。
 *
 * 不要对未做引用计数的 tupdesc 使用本函数。（状态不确定的
 * tupdesc 请用宏 PinTupleDesc。）
 */
void
IncrTupleDescRefCount(TupleDesc tupdesc)
{
	Assert(tupdesc->tdrefcount >= 0);

	ResourceOwnerEnlargeTupleDescs(CurrentResourceOwner);
	tupdesc->tdrefcount++;
	ResourceOwnerRememberTupleDesc(CurrentResourceOwner, tupdesc);
}

/*
 * 减少 tupdesc 的引用计数，从 CurrentResourceOwner
 * 去掉对应引用，若不再有剩余引用，
 * 就释放该 tupdesc。
 *
 * 不要对未做引用计数的 tupdesc 使用本函数。（状态不确定的
 * tupdesc 请用宏 ReleaseTupleDesc。）
 */
void
DecrTupleDescRefCount(TupleDesc tupdesc)
{
	Assert(tupdesc->tdrefcount > 0);

	ResourceOwnerForgetTupleDesc(CurrentResourceOwner, tupdesc);
	if (--tupdesc->tdrefcount == 0)
		FreeTupleDesc(tupdesc);
}

/*
 * 比较两个 TupleDesc 结构在逻辑上是否相等
 *
 * 注意：我们故意不检查 attrelid 和 tdtypmod 字段。
 * 这样 typcache.c 可以用本例程判断缓存的复合类型是否匹配
 * 所请求的类型，对 relcache.c 的使用也无害。
 * 我们也不比较 tdrefcount。
 */
bool
equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2)
{
	int			i,
				n;

	if (tupdesc1->natts != tupdesc2->natts)
		return false;
	if (tupdesc1->tdtypeid != tupdesc2->tdtypeid)
		return false;

	for (i = 0; i < tupdesc1->natts; i++)
	{
		Form_pg_attribute attr1 = TupleDescAttr(tupdesc1, i);
		Form_pg_attribute attr2 = TupleDescAttr(tupdesc2, i);

		/*
		 * 这里不需要逐一检查每个成员：可以忽略 attrelid 与 attnum
		 * （它们本来就是用来把该行放进 attrs 数组的）。
		 * 看起来似乎也可以不检查 attlen/attbyval/attalign，因为它们
		 * 派生自 atttypid；但对已删除的列必须检查（因为所有已删除列
		 * 的 atttypid 都是零），而且一般而言始终检查
		 * 它们更安全。
		 * 所以不要省略这些检查。
		 *
		 * 不得检查 attcacheoff，因为两份副本中它可能都尚未设置。
		 * 我们也有意忽略 atthasmissing，这在元组描述符里不太
		 * 相关，因为它们没有 attmissingval 成员。
		 */
		if (strcmp(NameStr(attr1->attname), NameStr(attr2->attname)) != 0)
			return false;
		if (attr1->atttypid != attr2->atttypid)
			return false;
		if (attr1->attstattarget != attr2->attstattarget)
			return false;
		if (attr1->attlen != attr2->attlen)
			return false;
		if (attr1->attndims != attr2->attndims)
			return false;
		if (attr1->atttypmod != attr2->atttypmod)
			return false;
		if (attr1->attbyval != attr2->attbyval)
			return false;
		if (attr1->attalign != attr2->attalign)
			return false;
		if (attr1->attstorage != attr2->attstorage)
			return false;
		if (attr1->attcompression != attr2->attcompression)
			return false;
		if (attr1->attnotnull != attr2->attnotnull)
			return false;
		if (attr1->atthasdef != attr2->atthasdef)
			return false;
		if (attr1->attidentity != attr2->attidentity)
			return false;
		if (attr1->attgenerated != attr2->attgenerated)
			return false;
		if (attr1->attisdropped != attr2->attisdropped)
			return false;
		if (attr1->attislocal != attr2->attislocal)
			return false;
		if (attr1->attinhcount != attr2->attinhcount)
			return false;
		if (attr1->attcollation != attr2->attcollation)
			return false;
		/* 变长成员甚至都不存在... */
	}

	if (tupdesc1->constr != NULL)
	{
		TupleConstr *constr1 = tupdesc1->constr;
		TupleConstr *constr2 = tupdesc2->constr;

		if (constr2 == NULL)
			return false;
		if (constr1->has_not_null != constr2->has_not_null)
			return false;
		if (constr1->has_generated_stored != constr2->has_generated_stored)
			return false;
		n = constr1->num_defval;
		if (n != (int) constr2->num_defval)
			return false;
		/* 这里假定两个 AttrDefault 数组都按 adnum 排序 */
		for (i = 0; i < n; i++)
		{
			AttrDefault *defval1 = constr1->defval + i;
			AttrDefault *defval2 = constr2->defval + i;

			if (defval1->adnum != defval2->adnum)
				return false;
			if (strcmp(defval1->adbin, defval2->adbin) != 0)
				return false;
		}
		if (constr1->missing)
		{
			if (!constr2->missing)
				return false;
			for (i = 0; i < tupdesc1->natts; i++)
			{
				AttrMissing *missval1 = constr1->missing + i;
				AttrMissing *missval2 = constr2->missing + i;

				if (missval1->am_present != missval2->am_present)
					return false;
				if (missval1->am_present)
				{
					Form_pg_attribute missatt1 = TupleDescAttr(tupdesc1, i);

					if (!datumIsEqual(missval1->am_value, missval2->am_value,
									  missatt1->attbyval, missatt1->attlen))
						return false;
				}
			}
		}
		else if (constr2->missing)
			return false;
		n = constr1->num_check;
		if (n != (int) constr2->num_check)
			return false;

		/*
		 * 类似地，这里依赖 ConstrCheck 项已按名称排序。
		 * 若存在重名，比较的结果并不确定，
		 * 但那种情况不该发生。
		 */
		for (i = 0; i < n; i++)
		{
			ConstrCheck *check1 = constr1->check + i;
			ConstrCheck *check2 = constr2->check + i;

			if (!(strcmp(check1->ccname, check2->ccname) == 0 &&
				  strcmp(check1->ccbin, check2->ccbin) == 0 &&
				  check1->ccvalid == check2->ccvalid &&
				  check1->ccnoinherit == check2->ccnoinherit))
				return false;
		}
	}
	else if (tupdesc2->constr != NULL)
		return false;
	return true;
}

/*
 * hashTupleDesc
 *		计算元组描述符的散列值。
 *
 * 若两个元组描述符按 equalTupleDescs() 会被视为相等，
 * 则按本函数它们的散列值也相等。
 *
 * 注意：目前约束的内容不参与散列 --- 那样做有点麻烦，
 * 而且仅仅因约束产生冲突的可能性不大。
 */
uint32
hashTupleDesc(TupleDesc desc)
{
	uint32		s;
	int			i;

	s = hash_combine(0, hash_uint32(desc->natts));
	s = hash_combine(s, hash_uint32(desc->tdtypeid));
	for (i = 0; i < desc->natts; ++i)
		s = hash_combine(s, hash_uint32(TupleDescAttr(desc, i)->atttypid));

	return s;
}

/*
 * TupleDescInitEntry
 *		本函数在先前分配的元组描述符中
 *		初始化单个属性结构。
 *
 * 若 attributeName 为 NULL，attname 字段被设成空字符串
 * （用于我们不知道或不需要该字段名字的情形）。
 * 也有调用方用本函数修改已有 tupdesc 中与数据类型相关的字段；
 * 它们传入 attributeName = NameStr(att->attname)，
 * 表示不应修改 attname 字段。
 *
 * 注意：attcollation 被设为指定数据类型的默认排序规则。
 * 若需要非默认排序规则，事后用
 * TupleDescInitEntryCollation 填入。
 */
void
TupleDescInitEntry(TupleDesc desc,
				   AttrNumber attributeNumber,
				   const char *attributeName,
				   Oid oidtypeid,
				   int32 typmod,
				   int attdim)
{
	HeapTuple	tuple;
	Form_pg_type typeForm;
	Form_pg_attribute att;

	/*
	 * 健全性检查
	 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);
	Assert(attdim >= 0);
	Assert(attdim <= PG_INT16_MAX);

	/*
	 * 初始化属性字段
	 */
	att = TupleDescAttr(desc, attributeNumber - 1);

	att->attrelid = 0;			/* 占位值 */

	/*
	 * 注意：attributeName 可以为 NULL，因为规划器并不总是
	 * 在目标列表里填上有效的 resname，对 resjunk 属性尤其如此。
	 * 若调用方想复用旧的 attname，这里也什么都不做。
	 */
	if (attributeName == NULL)
		MemSet(NameStr(att->attname), 0, NAMEDATALEN);
	else if (attributeName != NameStr(att->attname))
		namestrcpy(&(att->attname), attributeName);

	att->attstattarget = -1;
	att->attcacheoff = -1;
	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->atthasmissing = false;
	att->attidentity = '\0';
	att->attgenerated = '\0';
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;
	/* 元组描述符中不存在变长字段 */

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(oidtypeid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", oidtypeid);
	typeForm = (Form_pg_type) GETSTRUCT(tuple);

	att->atttypid = oidtypeid;
	att->attlen = typeForm->typlen;
	att->attbyval = typeForm->typbyval;
	att->attalign = typeForm->typalign;
	att->attstorage = typeForm->typstorage;
	att->attcompression = InvalidCompressionMethod;
	att->attcollation = typeForm->typcollation;

	ReleaseSysCache(tuple);
}

/*
 * TupleDescInitBuiltinEntry
 *		不访问系统目录来初始化元组描述符。只支持
 *		有限范围内的内建类型。
 */
void
TupleDescInitBuiltinEntry(TupleDesc desc,
						  AttrNumber attributeNumber,
						  const char *attributeName,
						  Oid oidtypeid,
						  int32 typmod,
						  int attdim)
{
	Form_pg_attribute att;

	/* 健全性检查 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);
	Assert(attdim >= 0);
	Assert(attdim <= PG_INT16_MAX);

	/* 初始化属性字段 */
	att = TupleDescAttr(desc, attributeNumber - 1);
	att->attrelid = 0;			/* 占位值 */

	/* 与 TupleDescInitEntry 不同，这里需要属性名 */
	Assert(attributeName != NULL);
	namestrcpy(&(att->attname), attributeName);

	att->attstattarget = -1;
	att->attcacheoff = -1;
	att->atttypmod = typmod;

	att->attnum = attributeNumber;
	att->attndims = attdim;

	att->attnotnull = false;
	att->atthasdef = false;
	att->atthasmissing = false;
	att->attidentity = '\0';
	att->attgenerated = '\0';
	att->attisdropped = false;
	att->attislocal = true;
	att->attinhcount = 0;
	/* 元组描述符中不存在变长字段 */

	att->atttypid = oidtypeid;

	/*
	 * 这里的目标只是支持刚好够用的类型，让基本的内建命令
	 * 在没有系统目录访问时也能工作 --- 例如即使在未连接到
	 * 数据库的进程里也能做某些事情。
	 */
	switch (oidtypeid)
	{
		case TEXTOID:
		case TEXTARRAYOID:
			att->attlen = -1;
			att->attbyval = false;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_EXTENDED;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = DEFAULT_COLLATION_OID;
			break;

		case BOOLOID:
			att->attlen = 1;
			att->attbyval = true;
			att->attalign = TYPALIGN_CHAR;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case INT4OID:
			att->attlen = 4;
			att->attbyval = true;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case INT8OID:
			att->attlen = 8;
			att->attbyval = FLOAT8PASSBYVAL;
			att->attalign = TYPALIGN_DOUBLE;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		case OIDOID:
			att->attlen = 4;
			att->attbyval = true;
			att->attalign = TYPALIGN_INT;
			att->attstorage = TYPSTORAGE_PLAIN;
			att->attcompression = InvalidCompressionMethod;
			att->attcollation = InvalidOid;
			break;

		default:
			elog(ERROR, "unsupported type %u", oidtypeid);
	}
}

/*
 * TupleDescInitEntryCollation
 *
 * 给先前初始化的元组描述符项指定非默认排序规则。
 * 填入该项。
 */
void
TupleDescInitEntryCollation(TupleDesc desc,
							AttrNumber attributeNumber,
							Oid collationid)
{
	/*
	 * 健全性检查
	 */
	Assert(PointerIsValid(desc));
	Assert(attributeNumber >= 1);
	Assert(attributeNumber <= desc->natts);

	TupleDescAttr(desc, attributeNumber - 1)->attcollation = collationid;
}


/*
 * BuildDescForRelation
 *
 * 给定关系模式（ColumnDef 节点的列表），构造 TupleDesc。
 *
 * 注意：tdtypeid 需要稍后填入。
 */
TupleDesc
BuildDescForRelation(List *schema)
{
	int			natts;
	AttrNumber	attnum;
	ListCell   *l;
	TupleDesc	desc;
	bool		has_not_null;
	char	   *attname;
	Oid			atttypid;
	int32		atttypmod;
	Oid			attcollation;
	int			attdim;

	/*
	 * 分配一个新的元组描述符
	 */
	natts = list_length(schema);
	desc = CreateTemplateTupleDesc(natts);
	has_not_null = false;

	attnum = 0;

	foreach(l, schema)
	{
		ColumnDef  *entry = lfirst(l);
		AclResult	aclresult;
		Form_pg_attribute att;

		/*
		 * 对列表中的每一项，从列表中取得名字和类型信息，
		 * 并让 TupleDescInitEntry 填入我们所需要的
		 * 属性信息。
		 */
		attnum++;

		attname = entry->colname;
		typenameTypeIdAndMod(NULL, entry->typeName, &atttypid, &atttypmod);

		aclresult = object_aclcheck(TypeRelationId, atttypid, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, atttypid);

		attcollation = GetColumnDefCollation(NULL, entry, atttypid);
		attdim = list_length(entry->typeName->arrayBounds);
		if (attdim > PG_INT16_MAX)
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("too many array dimensions"));

		if (entry->typeName->setof)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("column \"%s\" cannot be declared SETOF",
							attname)));

		TupleDescInitEntry(desc, attnum, attname,
						   atttypid, atttypmod, attdim);
		att = TupleDescAttr(desc, attnum - 1);

		/* 按请求覆盖 TupleDescInitEntry 的设置 */
		TupleDescInitEntryCollation(desc, attnum, attcollation);
		if (entry->storage)
			att->attstorage = entry->storage;

		/* 填入 TupleDescInitEntry 未处理的其余内容 */
		att->attnotnull = entry->is_not_null;
		has_not_null |= entry->is_not_null;
		att->attislocal = entry->is_local;
		att->attinhcount = entry->inhcount;
	}

	if (has_not_null)
	{
		TupleConstr *constr = (TupleConstr *) palloc0(sizeof(TupleConstr));

		constr->has_not_null = true;
		constr->has_generated_stored = false;
		constr->defval = NULL;
		constr->missing = NULL;
		constr->num_defval = 0;
		constr->check = NULL;
		constr->num_check = 0;
		desc->constr = constr;
	}
	else
	{
		desc->constr = NULL;
	}

	return desc;
}

/*
 * BuildDescFromLists
 *
 * 给定列名（String 节点）、列类型 OIDs、typmod
 * 以及排序规则 OIDs 的列表，构造 TupleDesc。
 *
 * 不生成约束。
 *
 * 这本质上是 BuildDescForRelation 的精简版，用于
 * 返回 RECORD 的函数。
 */
TupleDesc
BuildDescFromLists(List *names, List *types, List *typmods, List *collations)
{
	int			natts;
	AttrNumber	attnum;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	ListCell   *l4;
	TupleDesc	desc;

	natts = list_length(names);
	Assert(natts == list_length(types));
	Assert(natts == list_length(typmods));
	Assert(natts == list_length(collations));

	/*
	 * 分配一个新的元组描述符
	 */
	desc = CreateTemplateTupleDesc(natts);

	attnum = 0;
	forfour(l1, names, l2, types, l3, typmods, l4, collations)
	{
		char	   *attname = strVal(lfirst(l1));
		Oid			atttypid = lfirst_oid(l2);
		int32		atttypmod = lfirst_int(l3);
		Oid			attcollation = lfirst_oid(l4);

		attnum++;

		TupleDescInitEntry(desc, attnum, attname, atttypid, atttypmod, 0);
		TupleDescInitEntryCollation(desc, attnum, attcollation);
	}

	return desc;
}
