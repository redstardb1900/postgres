/*-------------------------------------------------------------------------
 *
 * jsonb_util.c
 *	  在 Jsonb 与 JsonbValue 之间转换并进行遍历。
 *
 * Copyright (c) 2014-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_util.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/jsonapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

/*
 * 数组中元素（或对象中键值对）的最大数量。
 * 这受两件事限制：JEntry 数组的大小必须能放进
 * MaxAllocSize，且元素（或对）的数量必须能放进
 * JsonbContainer.header 字段中为它保留的位。
 *
 * （数组或对象的元素总大小还受
 * JENTRY_OFFLENMASK 限制，但这里无须关心。）
 */
#define JSONB_MAX_ELEMS (Min(MaxAllocSize / sizeof(JsonbValue), JB_CMASK))
#define JSONB_MAX_PAIRS (Min(MaxAllocSize / sizeof(JsonbPair), JB_CMASK))

static void fillJsonbValue(JsonbContainer *container, int index,
						   char *base_addr, uint32 offset,
						   JsonbValue *result);
static bool equalsJsonbScalarValue(JsonbValue *a, JsonbValue *b);
static int	compareJsonbScalarValue(JsonbValue *a, JsonbValue *b);
static Jsonb *convertToJsonb(JsonbValue *val);
static void convertJsonbValue(StringInfo buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbArray(StringInfo buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbObject(StringInfo buffer, JEntry *header, JsonbValue *val, int level);
static void convertJsonbScalar(StringInfo buffer, JEntry *header, JsonbValue *scalarVal);

static int	reserveFromBuffer(StringInfo buffer, int len);
static void appendToBuffer(StringInfo buffer, const char *data, int len);
static void copyToBuffer(StringInfo buffer, int offset, const char *data, int len);
static short padBufferToInt(StringInfo buffer);

static JsonbIterator *iteratorFromContainer(JsonbContainer *container, JsonbIterator *parent);
static JsonbIterator *freeAndGetParent(JsonbIterator *it);
static JsonbParseState *pushState(JsonbParseState **pstate);
static void appendKey(JsonbParseState *pstate, JsonbValue *string);
static void appendValue(JsonbParseState *pstate, JsonbValue *scalarVal);
static void appendElement(JsonbParseState *pstate, JsonbValue *scalarVal);
static int	lengthCompareJsonbStringValue(const void *a, const void *b);
static int	lengthCompareJsonbString(const char *val1, int len1,
									 const char *val2, int len2);
static int	lengthCompareJsonbPair(const void *a, const void *b, void *binequal);
static void uniqueifyJsonbObject(JsonbValue *object, bool unique_keys,
								 bool skip_nulls);
static JsonbValue *pushJsonbValueScalar(JsonbParseState **pstate,
										JsonbIteratorToken seq,
										JsonbValue *scalarVal);

void
JsonbToJsonbValue(Jsonb *jsonb, JsonbValue *val)
{
	val->type = jbvBinary;
	val->val.binary.data = &jsonb->root;
	val->val.binary.len = VARSIZE(jsonb) - VARHDRSZ;
}

/*
 * 把内存中的 JsonbValue 转为 Jsonb 供磁盘存储。
 *
 * 通常我们发现，直接遍历 Jsonb
 * 表示并只真正转换嵌套的标量值会更加方便。
 * JsonbIteratorNext() 会做这件事，这样迭代代码的
 * 使用者不必直接处理二进制表示（JsonbDeepContains() 是一个
 * 著名的例外，不过所有例外都限于本模块内部）。
 * 一般而言，接受 JsonbValue 参数的函数所关心的
 * 是标量值的操作，或标量值的简单容器，
 * 在这些场合下去处理大量其他状态会很不方便。
 */
Jsonb *
JsonbValueToJsonb(JsonbValue *val)
{
	Jsonb	   *out;

	if (IsAJsonbScalar(val))
	{
		/* 标量值 */
		JsonbParseState *pstate = NULL;
		JsonbValue *res;
		JsonbValue	scalarArray;

		scalarArray.type = jbvArray;
		scalarArray.val.array.rawScalar = true;
		scalarArray.val.array.nElems = 1;

		pushJsonbValue(&pstate, WJB_BEGIN_ARRAY, &scalarArray);
		pushJsonbValue(&pstate, WJB_ELEM, val);
		res = pushJsonbValue(&pstate, WJB_END_ARRAY, NULL);

		out = convertToJsonb(res);
	}
	else if (val->type == jbvObject || val->type == jbvArray)
	{
		out = convertToJsonb(val);
	}
	else
	{
		Assert(val->type == jbvBinary);
		out = palloc(VARHDRSZ + val->val.binary.len);
		SET_VARSIZE(out, VARHDRSZ + val->val.binary.len);
		memcpy(VARDATA(out), val->val.binary.data, val->val.binary.len);
	}

	return out;
}

/*
 * 获取 Jsonb 节点的变长部分在其容器的变长数据部分
 * 内的偏移。节点由
 * 在容器 JEntry 数组中的下标标识。
 */
uint32
getJsonbOffset(const JsonbContainer *jc, int index)
{
	uint32		offset = 0;
	int			i;

	/*
	 * 本条目的起始偏移等于前一条目的结束偏移。向前
	 * 回溯到最近一个以结束偏移形式存储的条目，
	 * 返回该偏移加上中间各条目的长度。
	 */
	for (i = index - 1; i >= 0; i--)
	{
		offset += JBE_OFFLENFLD(jc->children[i]);
		if (JBE_HAS_OFF(jc->children[i]))
			break;
	}

	return offset;
}

/*
 * 获取 Jsonb 节点的变长部分的长度。
 * 该节点由容器 JEntry 数组中的下标标识。
 */
uint32
getJsonbLength(const JsonbContainer *jc, int index)
{
	uint32		off;
	uint32		len;

	/*
	 * 若长度直接存于 JEntry 中，则直接返回。
	 * 否则，取该条目的起始偏移，并从存储的 end+1
	 * 偏移中减去它。
	 */
	if (JBE_HAS_OFF(jc->children[index]))
	{
		off = getJsonbOffset(jc, index);
		len = JBE_OFFLENFLD(jc->children[index]) - off;
	}
	else
		len = JBE_OFFLENFLD(jc->children[index]);

	return len;
}

/*
 * BT 比较器工作函数。返回小于、等于或大于零的整数，
 * 以指示 a 是小于、等于还是大于 b。这与 B-Tree 算子类的
 * 要求相一致。
 *
 * 字符串按字典序比较，这与其他地方为检索 String 而使用
 * 的简单得多的比较器逻辑形成对照。由于本函数是
 * 从 B-Tree 支持函数 1 调用的，因此我们避免在此泄漏
 * 内存。
 */
int
compareJsonbContainers(JsonbContainer *a, JsonbContainer *b)
{
	JsonbIterator *ita,
			   *itb;
	int			res = 0;

	ita = JsonbIteratorInit(a);
	itb = JsonbIteratorInit(b);

	do
	{
		JsonbValue	va,
					vb;
		JsonbIteratorToken ra,
					rb;

		ra = JsonbIteratorNext(&ita, &va, false);
		rb = JsonbIteratorNext(&itb, &vb, false);

		if (ra == rb)
		{
			if (ra == WJB_DONE)
			{
				/* 断然相等 */
				break;
			}

			if (ra == WJB_END_ARRAY || ra == WJB_END_OBJECT)
			{
				/*
				 * 在这一处理阶段没有数组或对象可比较，
				 * jbvArray/jbvObject 值最初是在 WJB_BEGIN_ARRAY
				 * 与 WJB_BEGIN_OBJECT 标记处进行比较
				 * 的。
				 */
				continue;
			}

			if (va.type == vb.type)
			{
				switch (va.type)
				{
					case jbvString:
					case jbvNull:
					case jbvNumeric:
					case jbvBool:
						res = compareJsonbScalarValue(&va, &vb);
						break;
					case jbvArray:

						/*
						 * 这可能是 "raw scalar" 伪数组。
						 * 它在此仍是个特例，因为我们还是希望
						 * 让基于类型的通用比较继续适用，而且
						 * 在我们看来伪数组就是一个标量。
						 */
						if (va.val.array.rawScalar != vb.val.array.rawScalar)
							res = (va.val.array.rawScalar) ? -1 : 1;
						if (va.val.array.nElems != vb.val.array.nElems)
							res = (va.val.array.nElems > vb.val.array.nElems) ? 1 : -1;
						break;
					case jbvObject:
						if (va.val.object.nPairs != vb.val.object.nPairs)
							res = (va.val.object.nPairs > vb.val.object.nPairs) ? 1 : -1;
						break;
					case jbvBinary:
						elog(ERROR, "unexpected jbvBinary value");
						break;
					case jbvDatetime:
						elog(ERROR, "unexpected jbvDatetime value");
						break;
				}
			}
			else
			{
				/* 类型定义顺序 */
				res = (va.type > vb.type) ? 1 : -1;
			}
		}
		else
		{
			/*
			 * 若两个值是同一容器类型，本来有机会观察到
			 * 元素/对数量上的差异（比如说在处理
			 * WJB_BEGIN_OBJECT 时）。它们要么是两个异构容器，
			 * 要么是一个容器加某个
			 * 标量类型。
			 *
			 * 我们无须在这里考虑 WJB_END_ARRAY 和 WJB_END_OBJECT
			 * 情形，因为我们会先看到对应的
			 * WJB_BEGIN_ARRAY 和 WJB_BEGIN_OBJECT 标记，得出
			 * 它们不匹配的结论。
			 */
			Assert(ra != WJB_END_ARRAY && ra != WJB_END_OBJECT);
			Assert(rb != WJB_END_ARRAY && rb != WJB_END_OBJECT);

			Assert(va.type != vb.type);
			Assert(va.type != jbvBinary);
			Assert(vb.type != jbvBinary);
			/* 类型定义顺序 */
			res = (va.type > vb.type) ? 1 : -1;
		}
	}
	while (res == 0);

	while (ita != NULL)
	{
		JsonbIterator *i = ita->parent;

		pfree(ita);
		ita = i;
	}
	while (itb != NULL)
	{
		JsonbIterator *i = itb->parent;

		pfree(itb);
		itb = i;
	}

	return res;
}

/*
 * 在对象中查找值（即对象中键值对的 "value" 部分），
 * 或者当我们在数组中查找时找到匹配的元素。比较时
 * 仅依据对象键的相等性，或者
 * 仅依据元素值，与调用者提供的值 "key" 比对。参数
 * "flags" 允许调用者指定感兴趣的容器类型。
 *
 * 这个导出的工具函数用于辅助涉及 "containment" 的
 * 各种场景。若被要求查找对象，调用者最好传入一个
 * Jsonb 字符串，因为其键只能是字符串。
 * 否则数组中任何 JsonbValue 都可以。
 *
 * 要继续执行查找，调用者必须已通过适当的标志声明
 * 只对某一种特定容器类型感兴趣，
 * 并且被指向的 Jsonb 容器在顶层属于上述类型之一。
 * （实际上，我们只选择有意义的那一种，省得调用者
 * 自己费心判断——因为容器要么指向一个数组
 * （可能是 "raw scalar" 伪数组），要么指向一个对象，
 * 所以最多只有一种是有意义的。）
 *
 * 注意：在对象上调用本函数时可返回 jbvBinary 类型的
 * JsonbValue，但在数组上绝不会这样做。若调用者要求查找
 * 的容器类型与容器指向不符，则立即落空并返回
 * NULL。若找不到该值，也返回 NULL。否则返回该值
 * 经 palloc() 分配的副本。
 */
JsonbValue *
findJsonbValueFromContainer(JsonbContainer *container, uint32 flags,
							JsonbValue *key)
{
	JEntry	   *children = container->children;
	int			count = JsonContainerSize(container);

	Assert((flags & ~(JB_FARRAY | JB_FOBJECT)) == 0);

	/* 若对象/数组为空则无须 palloc 直接快速返回 */
	if (count <= 0)
		return NULL;

	if ((flags & JB_FARRAY) && JsonContainerIsArray(container))
	{
		JsonbValue *result = palloc(sizeof(JsonbValue));
		char	   *base_addr = (char *) (children + count);
		uint32		offset = 0;
		int			i;

		for (i = 0; i < count; i++)
		{
			fillJsonbValue(container, i, base_addr, offset, result);

			if (key->type == result->type)
			{
				if (equalsJsonbScalarValue(key, result))
					return result;
			}

			JBE_ADVANCE_OFFSET(offset, children[i]);
		}

		pfree(result);
	}
	else if ((flags & JB_FOBJECT) && JsonContainerIsObject(container))
	{
		/* 调用者传入对象键必须是字符串 */
		Assert(key->type == jbvString);

		return getKeyJsonValueFromContainer(container, key->val.string.val,
											key->val.string.len, NULL);
	}

	/* 未找到 */
	return NULL;
}

/*
 * 按键在 Jsonb 对象中查找值并取到 "res" 中，
 * 并返回
 *
 * "res" 可以传入 NULL，这种情况下它在此处被新 palloc。
 */
JsonbValue *
getKeyJsonValueFromContainer(JsonbContainer *container,
							 const char *keyVal, int keyLen, JsonbValue *res)
{
	JEntry	   *children = container->children;
	int			count = JsonContainerSize(container);
	char	   *baseAddr;
	uint32		stopLow,
				stopHigh;

	Assert(JsonContainerIsObject(container));

	/* 若对象为空则无须 palloc 直接快速返回 */
	if (count <= 0)
		return NULL;

	/*
	 * 对容器做二分查找。已知这是对象，要把 JEntry 的
	 * 数量按*对*计
	 */
	baseAddr = (char *) (children + count * 2);
	stopLow = 0;
	stopHigh = count;
	while (stopLow < stopHigh)
	{
		uint32		stopMiddle;
		int			difference;
		const char *candidateVal;
		int			candidateLen;

		stopMiddle = stopLow + (stopHigh - stopLow) / 2;

		candidateVal = baseAddr + getJsonbOffset(container, stopMiddle);
		candidateLen = getJsonbLength(container, stopMiddle);

		difference = lengthCompareJsonbString(candidateVal, candidateLen,
											  keyVal, keyLen);

		if (difference == 0)
		{
			/* 找到目标键，返回对应的值 */
			int			index = stopMiddle + count;

			if (!res)
				res = palloc(sizeof(JsonbValue));

			fillJsonbValue(container, index, baseAddr,
						   getJsonbOffset(container, index),
						   res);

			return res;
		}
		else
		{
			if (difference < 0)
				stopLow = stopMiddle + 1;
			else
				stopHigh = stopMiddle;
		}
	}

	/* 未找到 */
	return NULL;
}

/*
 * 取 Jsonb 数组第 i 个值。
 *
 * 返回该值的 palloc() 副本，若不存在则返回 NULL。
 */
JsonbValue *
getIthJsonbValueFromContainer(JsonbContainer *container, uint32 i)
{
	JsonbValue *result;
	char	   *base_addr;
	uint32		nelements;

	if (!JsonContainerIsArray(container))
		elog(ERROR, "not a jsonb array");

	nelements = JsonContainerSize(container);
	base_addr = (char *) &container->children[nelements];

	if (i >= nelements)
		return NULL;

	result = palloc(sizeof(JsonbValue));

	fillJsonbValue(container, i, base_addr,
				   getJsonbOffset(container, i),
				   result);

	return result;
}

/*
 * 辅助函数：填充 JsonbValue 以表示数组元素，
 * 或对象的键或值。
 *
 * 该节点的 JEntry 位于 container->children[index]，其变长数据
 * 位于 base_addr + offset。我们让调用者来确定偏移，
 * 因为在很多情况下调用者可以把这项工作摊到多个
 * children 上。做不到时可调用 getJsonbOffset()。
 *
 * 嵌套的数组或对象会以 jbvBinary 形式返回，即不会被
 * 展开。
 */
static void
fillJsonbValue(JsonbContainer *container, int index,
			   char *base_addr, uint32 offset,
			   JsonbValue *result)
{
	JEntry		entry = container->children[index];

	if (JBE_ISNULL(entry))
	{
		result->type = jbvNull;
	}
	else if (JBE_ISSTRING(entry))
	{
		result->type = jbvString;
		result->val.string.val = base_addr + offset;
		result->val.string.len = getJsonbLength(container, index);
		Assert(result->val.string.len >= 0);
	}
	else if (JBE_ISNUMERIC(entry))
	{
		result->type = jbvNumeric;
		result->val.numeric = (Numeric) (base_addr + INTALIGN(offset));
	}
	else if (JBE_ISBOOL_TRUE(entry))
	{
		result->type = jbvBool;
		result->val.boolean = true;
	}
	else if (JBE_ISBOOL_FALSE(entry))
	{
		result->type = jbvBool;
		result->val.boolean = false;
	}
	else
	{
		Assert(JBE_ISCONTAINER(entry));
		result->type = jbvBinary;
		/* 从数据指针和长度中去除对齐填充 */
		result->val.binary.data = (JsonbContainer *) (base_addr + INTALIGN(offset));
		result->val.binary.len = getJsonbLength(container, index) -
			(INTALIGN(offset) - offset);
	}
}

/*
 * 把 JsonbValue 压入 JsonbParseState
 *
 * 在解析 JSON 标记以构成 Jsonb 时使用，或把内存中的
 * JsonbValue 转 Jsonb
 *
 * *JsonbParseState 的初始状态为 NULL，因为它最初会在这里
 * 被分配（调用者经引用拿回 JsonbParseState）。
 *
 * 只有与 non-container 类型相关的顺序性标记才应传入
 * JsonbValue。有一个例外——WJB_BEGIN_ARRAY 的调用者可以
 * 传入 "raw scalar" 伪数组来追加它——真正的标量应在随后
 * 传入，它将被添加为数组中唯一的成员。
 *
 * jbvBinary 值（即已合并的数组和对象）在加入
 * 结果之前会被解包。
 */
JsonbValue *
pushJsonbValue(JsonbParseState **pstate, JsonbIteratorToken seq,
			   JsonbValue *jbval)
{
	JsonbIterator *it;
	JsonbValue *res = NULL;
	JsonbValue	v;
	JsonbIteratorToken tok;
	int			i;

	if (jbval && (seq == WJB_ELEM || seq == WJB_VALUE) && jbval->type == jbvObject)
	{
		pushJsonbValue(pstate, WJB_BEGIN_OBJECT, NULL);
		for (i = 0; i < jbval->val.object.nPairs; i++)
		{
			pushJsonbValue(pstate, WJB_KEY, &jbval->val.object.pairs[i].key);
			pushJsonbValue(pstate, WJB_VALUE, &jbval->val.object.pairs[i].value);
		}

		return pushJsonbValue(pstate, WJB_END_OBJECT, NULL);
	}

	if (jbval && (seq == WJB_ELEM || seq == WJB_VALUE) && jbval->type == jbvArray)
	{
		pushJsonbValue(pstate, WJB_BEGIN_ARRAY, NULL);
		for (i = 0; i < jbval->val.array.nElems; i++)
		{
			pushJsonbValue(pstate, WJB_ELEM, &jbval->val.array.elems[i]);
		}

		return pushJsonbValue(pstate, WJB_END_ARRAY, NULL);
	}

	if (!jbval || (seq != WJB_ELEM && seq != WJB_VALUE) ||
		jbval->type != jbvBinary)
	{
		/* 直接贯穿 */
		return pushJsonbValueScalar(pstate, seq, jbval);
	}

	/* 解包二进制并把每段加入 pstate */
	it = JsonbIteratorInit(jbval->val.binary.data);

	if ((jbval->val.binary.data->header & JB_FSCALAR) && *pstate)
	{
		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_BEGIN_ARRAY);
		Assert(v.type == jbvArray && v.val.array.rawScalar);

		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_ELEM);

		res = pushJsonbValueScalar(pstate, seq, &v);

		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_END_ARRAY);
		Assert(it == NULL);

		return res;
	}

	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
		res = pushJsonbValueScalar(pstate, tok,
								   tok < WJB_BEGIN_ARRAY ||
								   (tok == WJB_BEGIN_ARRAY &&
									v.val.array.rawScalar) ? &v : NULL);

	return res;
}

/*
 * 执行实际压入，只接受标量或伪标量数组
 * 值。
 */
static JsonbValue *
pushJsonbValueScalar(JsonbParseState **pstate, JsonbIteratorToken seq,
					 JsonbValue *scalarVal)
{
	JsonbValue *result = NULL;

	switch (seq)
	{
		case WJB_BEGIN_ARRAY:
			Assert(!scalarVal || scalarVal->val.array.rawScalar);
			*pstate = pushState(pstate);
			result = &(*pstate)->contVal;
			(*pstate)->contVal.type = jbvArray;
			(*pstate)->contVal.val.array.nElems = 0;
			(*pstate)->contVal.val.array.rawScalar = (scalarVal &&
													  scalarVal->val.array.rawScalar);
			if (scalarVal && scalarVal->val.array.nElems > 0)
			{
				/* 假定这个数组其实仍是标量 */
				Assert(scalarVal->type == jbvArray);
				(*pstate)->size = scalarVal->val.array.nElems;
			}
			else
			{
				(*pstate)->size = 4;
			}
			(*pstate)->contVal.val.array.elems = palloc(sizeof(JsonbValue) *
														(*pstate)->size);
			break;
		case WJB_BEGIN_OBJECT:
			Assert(!scalarVal);
			*pstate = pushState(pstate);
			result = &(*pstate)->contVal;
			(*pstate)->contVal.type = jbvObject;
			(*pstate)->contVal.val.object.nPairs = 0;
			(*pstate)->size = 4;
			(*pstate)->contVal.val.object.pairs = palloc(sizeof(JsonbPair) *
														 (*pstate)->size);
			break;
		case WJB_KEY:
			Assert(scalarVal->type == jbvString);
			appendKey(*pstate, scalarVal);
			break;
		case WJB_VALUE:
			Assert(IsAJsonbScalar(scalarVal));
			appendValue(*pstate, scalarVal);
			break;
		case WJB_ELEM:
			Assert(IsAJsonbScalar(scalarVal));
			appendElement(*pstate, scalarVal);
			break;
		case WJB_END_OBJECT:
			uniqueifyJsonbObject(&(*pstate)->contVal,
								 (*pstate)->unique_keys,
								 (*pstate)->skip_nulls);
			/* fall through! */
		case WJB_END_ARRAY:
			/* 与 WJB_END_OBJECT 共有的步骤 */
			Assert(!scalarVal);
			result = &(*pstate)->contVal;

			/*
			 * 弹栈，把当前数组/对象作为值压入父容器
			 *
			 */
			*pstate = (*pstate)->next;
			if (*pstate)
			{
				switch ((*pstate)->contVal.type)
				{
					case jbvArray:
						appendElement(*pstate, result);
						break;
					case jbvObject:
						appendValue(*pstate, result);
						break;
					default:
						elog(ERROR, "invalid jsonb container type");
				}
			}
			break;
		default:
			elog(ERROR, "unrecognized jsonb sequential processing token");
	}

	return result;
}

/*
 * pushJsonbValue() 工作函数：迭代式构造 Jsonb
 */
static JsonbParseState *
pushState(JsonbParseState **pstate)
{
	JsonbParseState *ns = palloc(sizeof(JsonbParseState));

	ns->next = *pstate;
	ns->unique_keys = false;
	ns->skip_nulls = false;

	return ns;
}

/*
 * pushJsonbValue() 工作函数：构造 Jsonb 时追加一个对的键
 */
static void
appendKey(JsonbParseState *pstate, JsonbValue *string)
{
	JsonbValue *object = &pstate->contVal;

	Assert(object->type == jbvObject);
	Assert(string->type == jbvString);

	if (object->val.object.nPairs >= JSONB_MAX_PAIRS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of jsonb object pairs exceeds the maximum allowed (%zu)",
						JSONB_MAX_PAIRS)));

	if (object->val.object.nPairs >= pstate->size)
	{
		pstate->size *= 2;
		object->val.object.pairs = repalloc(object->val.object.pairs,
											sizeof(JsonbPair) * pstate->size);
	}

	object->val.object.pairs[object->val.object.nPairs].key = *string;
	object->val.object.pairs[object->val.object.nPairs].order = object->val.object.nPairs;
}

/*
 * pushJsonbValue() 工作函数：构造 Jsonb 时向状态追加对的
 * 值
 */
static void
appendValue(JsonbParseState *pstate, JsonbValue *scalarVal)
{
	JsonbValue *object = &pstate->contVal;

	Assert(object->type == jbvObject);

	object->val.object.pairs[object->val.object.nPairs++].value = *scalarVal;
}

/*
 * pushJsonbValue() 工作函数：构造 Jsonb 时向状态追加一个元素
 */
static void
appendElement(JsonbParseState *pstate, JsonbValue *scalarVal)
{
	JsonbValue *array = &pstate->contVal;

	Assert(array->type == jbvArray);

	if (array->val.array.nElems >= JSONB_MAX_ELEMS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of jsonb array elements exceeds the maximum allowed (%zu)",
						JSONB_MAX_ELEMS)));

	if (array->val.array.nElems >= pstate->size)
	{
		pstate->size *= 2;
		array->val.array.elems = repalloc(array->val.array.elems,
										  sizeof(JsonbValue) * pstate->size);
	}

	array->val.array.elems[array->val.array.nElems++] = *scalarVal;
}

/*
 * 给定 JsonbContainer，将其展开为 JsonbIterator，以便以
 * 展开到内存表示的形式来遍历其中的条目。
 *
 * 内存管理注意事项见 JsonbIteratorNext()。
 */
JsonbIterator *
JsonbIteratorInit(JsonbContainer *container)
{
	return iteratorFromContainer(container, NULL);
}

/*
 * 迭代时获取下一个 JsonbValue
 *
 * 调用者最初应传入原始的迭代器。他们拿回的可能是
 * 在这里 palloc 出来的子迭代器。可以指望本函数释放这些
 * 子迭代器，以免高度嵌套的对象耗用内存过多，
 * 但前提是调用者不提前结束迭代（例如在搜索中找到
 * 目标后即中断）。
 *
 * 这类场景下对长期上下文中内存泄漏敏感的调用者
 * 可从我们留下的最终迭代器沿祖先链向上追溯到最老
 * 的祖先，沿途逐一 pfree()。
 * 他们无须释放之前为迭代器分配的、但无法作为最后
 * 传入迭代器的直接祖先而访问的任何其他内存
 *
 *
 * 返回 "Jsonb 顺序处理" 标记值。迭代器 "state"
 * 以更粗的粒度反映流程当前所处阶段，主要
 * 用于在内部跟踪与特定迭代器相关的
 * 事物。
 *
 * 本函数的使用者不应需要处理任何 jbvBinary 值
 * （因为递归调用会处理它们），只要 skipNested 为 false。
 * 展开 jbvBinary 表示是我们的事，不必为此打扰他们。
 * 不过，使用者不应擅自触碰数组或对象元素/对的缓冲区，
 * 因为其元素/对指针已
 * 失效
 *
 * 当结果为 WJB_DONE、WJB_END_ARRAY 或 WJB_END_OBJECT 时
 * *val 并无意义。但在这些情形下我们会把
 * val->type 设为 jbvNull，调用者可假定它总有定义。
 */
JsonbIteratorToken
JsonbIteratorNext(JsonbIterator **it, JsonbValue *val, bool skipNested)
{
	if (*it == NULL)
	{
		val->type = jbvNull;
		return WJB_DONE;
	}

	/*
	 * 步入嵌套容器时，我们跳回这里处理子容器。
	 * 单次调用中不会进一步递归，子容器的处理总是
	 * 从 JBI_ARRAY_START 或 JBI_OBJECT_START
	 * 状态开始。
	 */
recurse:
	switch ((*it)->state)
	{
		case JBI_ARRAY_START:
			/* 首个数组调用 v 置为数组 */
			val->type = jbvArray;
			val->val.array.nElems = (*it)->nElems;

			/*
			 * v->val.array.elems 实际并未设置，因为我们没有做
			 * 完整转换
			 */
			val->val.array.rawScalar = (*it)->isScalar;
			(*it)->curIndex = 0;
			(*it)->curDataOffset = 0;
			(*it)->curValueOffset = 0;	/* 实际未使用 */
			/* 设下次调用状态 */
			(*it)->state = JBI_ARRAY_ELEM;
			return WJB_BEGIN_ARRAY;

		case JBI_ARRAY_ELEM:
			if ((*it)->curIndex >= (*it)->nElems)
			{
				/*
				 * 数组内所有元素已处理完。向调用者报告，
				 * 并交还原父迭代器（它会独立跟踪自身
				 * 嵌套层级上的迭代进度）。
				 *
				 */
				*it = freeAndGetParent(*it);
				val->type = jbvNull;
				return WJB_END_ARRAY;
			}

			fillJsonbValue((*it)->container, (*it)->curIndex,
						   (*it)->dataProper, (*it)->curDataOffset,
						   val);

			JBE_ADVANCE_OFFSET((*it)->curDataOffset,
							   (*it)->children[(*it)->curIndex]);
			(*it)->curIndex++;

			if (!IsAJsonbScalar(val) && !skipNested)
			{
				/* 递归进入容器。 */
				*it = iteratorFromContainer(val->val.binary.data, *it);
				goto recurse;
			}
			else
			{
				/*
				 * 数组中的标量项，或调用者不愿递归的容器
				 * 进入其中。
				 */
				return WJB_ELEM;
			}

		case JBI_OBJECT_START:
			/* 首个对象调用 v 置为对象 */
			val->type = jbvObject;
			val->val.object.nPairs = (*it)->nElems;

			/*
			 * v->val.object.pairs 并未设置，因为我们没有
			 * 做完整转换
			 */
			(*it)->curIndex = 0;
			(*it)->curDataOffset = 0;
			(*it)->curValueOffset = getJsonbOffset((*it)->container,
												   (*it)->nElems);
			/* 设下次调用状态 */
			(*it)->state = JBI_OBJECT_KEY;
			return WJB_BEGIN_OBJECT;

		case JBI_OBJECT_KEY:
			if ((*it)->curIndex >= (*it)->nElems)
			{
				/*
				 * 对象内所有对已处理完。向调用者报告，
				 * 并交还包含迭代器（它在自身嵌套层级
				 * 上独立跟踪迭代进度）。
				 *
				 */
				*it = freeAndGetParent(*it);
				val->type = jbvNull;
				return WJB_END_OBJECT;
			}
			else
			{
				/* 返回键值对的键。 */
				fillJsonbValue((*it)->container, (*it)->curIndex,
							   (*it)->dataProper, (*it)->curDataOffset,
							   val);
				if (val->type != jbvString)
					elog(ERROR, "unexpected jsonb type as object key");

				/* 设下次调用状态 */
				(*it)->state = JBI_OBJECT_VALUE;
				return WJB_KEY;
			}

		case JBI_OBJECT_VALUE:
			/* 设下次调用状态 */
			(*it)->state = JBI_OBJECT_KEY;

			fillJsonbValue((*it)->container, (*it)->curIndex + (*it)->nElems,
						   (*it)->dataProper, (*it)->curValueOffset,
						   val);

			JBE_ADVANCE_OFFSET((*it)->curDataOffset,
							   (*it)->children[(*it)->curIndex]);
			JBE_ADVANCE_OFFSET((*it)->curValueOffset,
							   (*it)->children[(*it)->curIndex + (*it)->nElems]);
			(*it)->curIndex++;

			/*
			 * 值可能是容器，此时我们用新的子迭代器递归
			 * （除非调用者传 skipNested 要求略过）。
			 *
			 */
			if (!IsAJsonbScalar(val) && !skipNested)
			{
				*it = iteratorFromContainer(val->val.binary.data, *it);
				goto recurse;
			}
			else
				return WJB_VALUE;
	}

	elog(ERROR, "invalid jsonb iterator state");
	/* 让不知道 elog(ERROR) 不会返回的编译器满意 */
	val->type = jbvNull;
	return WJB_DONE;
}

/*
 * 初始化迭代器，用于遍历容器中的所有元素。
 */
static JsonbIterator *
iteratorFromContainer(JsonbContainer *container, JsonbIterator *parent)
{
	JsonbIterator *it;

	it = palloc0(sizeof(JsonbIterator));
	it->container = container;
	it->parent = parent;
	it->nElems = JsonContainerSize(container);

	/* 数组紧跟头部之后 */
	it->children = container->children;

	switch (container->header & (JB_FARRAY | JB_FOBJECT))
	{
		case JB_FARRAY:
			it->dataProper =
				(char *) it->children + it->nElems * sizeof(JEntry);
			it->isScalar = JsonContainerIsScalar(container);
			/* 这是 "raw scalar" 或数组 */
			Assert(!it->isScalar || it->nElems == 1);

			it->state = JBI_ARRAY_START;
			break;

		case JB_FOBJECT:
			it->dataProper =
				(char *) it->children + it->nElems * sizeof(JEntry) * 2;
			it->state = JBI_OBJECT_START;
			break;

		default:
			elog(ERROR, "unknown type of jsonb container");
	}

	return it;
}

/*
 * JsonbIteratorNext() 工作函数：释放当前迭代器并返回父
 *
 */
static JsonbIterator *
freeAndGetParent(JsonbIterator *it)
{
	JsonbIterator *v = it->parent;

	pfree(it);
	return v;
}

/*
 * "contains" 算子的函数的工作函数
 *
 * 严格说来，包含关系是自顶向下的、无序的子树同构。
 *
 * 接受属于某种容器类型的迭代器。所谓这些迭代器
 * "属于"那些值，是指调用者刚针对它们初始化了这些
 * 迭代器（可能是以嵌套方式）。
 *
 * 从顶层调用时，"val" 是 lhs Jsonb，mContained 是 rhs Jsonb。
 * 判断 mContained 是否被包含在 val 中。
 */
bool
JsonbDeepContains(JsonbIterator **val, JsonbIterator **mContained)
{
	JsonbValue	vval,
				vcontained;
	JsonbIteratorToken rval,
				rcont;

	/*
	 * 防止过于复杂的 Jsonb 导致栈溢出。
	 *
	 * 在此被调用的函数各自独立采取了预防措施，
	 * 但由于本函数也是递归的，这可能还不够。
	 */
	check_stack_depth();

	rval = JsonbIteratorNext(val, &vval, false);
	rcont = JsonbIteratorNext(mContained, &vcontained, false);

	if (rval != rcont)
	{
		/*
		 * 不同的返回值可立即视为表明在此嵌套层级上存在
		 * 两个不同的容器类型，这足以让我们完全放弃
		 * （但情形应当是两者都是某种容器类型）。
		 *
		 */
		Assert(rval == WJB_BEGIN_OBJECT || rval == WJB_BEGIN_ARRAY);
		Assert(rcont == WJB_BEGIN_OBJECT || rcont == WJB_BEGIN_ARRAY);
		return false;
	}
	else if (rcont == WJB_BEGIN_OBJECT)
	{
		Assert(vval.type == jbvObject);
		Assert(vcontained.type == jbvObject);

		/*
		 * 若 lhs 的对少于 rhs，便不可能包含 rhs。（
		 * 安全只因我们给所有 Jsonb 对象的键去了重；
		 * 因此数组情形没有对应的优化。）该情形多半
		 * 不会出现，但这个检查如此廉价，不妨做上。
		 *
		 */
		if (vval.val.object.nPairs < vcontained.val.object.nPairs)
			return false;

		/* 遍历 rhs "是否被包含?" 对象 */
		for (;;)
		{
			JsonbValue *lhsVal; /* lhsVal 来自 lhs 对象中的对 */
			JsonbValue	lhsValBuf;

			rcont = JsonbIteratorNext(mContained, &vcontained, false);

			/*
			 * 走遍调用者的 rhs "是否被包含?" 对象而没有
			 * 找不到任何一个值时，它就是被包含
			 * 的。
			 */
			if (rcont == WJB_END_OBJECT)
				return true;

			Assert(rcont == WJB_KEY);
			Assert(vcontained.type == jbvString);

			/* 首先按键查找值…… */
			lhsVal =
				getKeyJsonValueFromContainer((*val)->container,
											 vcontained.val.string.val,
											 vcontained.val.string.len,
											 &lhsValBuf);
			if (!lhsVal)
				return false;

			/*
			 * 在这一阶段显然至少有一个键与该 rhs 对
			 * 匹配。
			 */
			rcont = JsonbIteratorNext(mContained, &vcontained, true);

			Assert(rcont == WJB_VALUE);

			/*
			 * 将 rhs 对的值与 lhs 对中刚用键找到的值比对
			 *
			 */
			if (lhsVal->type != vcontained.type)
			{
				return false;
			}
			else if (IsAJsonbScalar(lhsVal))
			{
				if (!equalsJsonbScalarValue(lhsVal, &vcontained))
					return false;
			}
			else
			{
				/* 嵌套容器值（对象或数组） */
				JsonbIterator *nestval,
						   *nestContained;

				Assert(lhsVal->type == jbvBinary);
				Assert(vcontained.type == jbvBinary);

				nestval = JsonbIteratorInit(lhsVal->val.binary.data);
				nestContained = JsonbIteratorInit(vcontained.val.binary.data);

				/*
				 * 递归匹配 rhs datum 对象的对的 "value" 一侧。
				 * 这是嵌套结构。
				 *
				 * 注意嵌套仍须在正确的子层级上"对齐"，
				 * 不过每层只需有零个或更多匹配的
				 * 对（或元素）（前提是 *rhs* 每层
				 * *全部*匹配），这使得搜索嵌套结构中
				 * 的单个 String 或其他原生类型子 datum 颇为
				 * 有效（前提是 rhs 嵌套结构被构造成
				 * 让我们"知道去哪找"）
				 * 即可。
				 *
				 * 换言之，rhs "vcontained" Jsonb 中容器节点到 lhs
				 * 内部节点的映射是单射，rhs 的父子边
				 * 必须映射到 lhs 的父子边才满足包含条件
				 * （当然被映射的节点还必须
				 * 相等）。
				 *
				 */
				if (!JsonbDeepContains(&nestval, &nestContained))
					return false;
			}
		}
	}
	else if (rcont == WJB_BEGIN_ARRAY)
	{
		JsonbValue *lhsConts = NULL;
		uint32		nLhsElems = vval.val.array.nElems;

		Assert(vval.type == jbvArray);
		Assert(vcontained.type == jbvArray);

		/*
		 * 区分 "raw scalar" 伪数组与真正的
		 * 数组
		 *
		 * 原始标量可包含另一个原始标量，数组可
		 * 包含原始标量，但原始标量不能包含数组。对对象
		 * 情形不做类似处理，因为对象只能包含对，绝不
		 * 会包含原始标量（一个对由带单个被包含对的 rhs
		 * 对象参数表示）。
		 */
		if (vval.val.array.rawScalar && !vcontained.val.array.rawScalar)
			return false;

		/* 遍历 rhs "是否被包含?" 数组 */
		for (;;)
		{
			rcont = JsonbIteratorNext(mContained, &vcontained, true);

			/*
			 * 走遍调用者的 rhs "是否被包含?" 数组而没有
			 * 找不到任何一个值时，它就是被包含
			 * 的。
			 */
			if (rcont == WJB_END_ARRAY)
				return true;

			Assert(rcont == WJB_ELEM);

			if (IsAJsonbScalar(&vcontained))
			{
				if (!findJsonbValueFromContainer((*val)->container,
												 JB_FARRAY,
												 &vcontained))
					return false;
			}
			else
			{
				uint32		i;

				/*
				 * 若这是 rhs 数组中此深度的首个容器，
				 * 则初始化容器的临时 lhs 数组
				 */
				if (lhsConts == NULL)
				{
					uint32		j = 0;

					/* 为所有可能的值腾出空间 */
					lhsConts = palloc(sizeof(JsonbValue) * nLhsElems);

					for (i = 0; i < nLhsElems; i++)
					{
						/* 把 lhs 元素存入临时数组 */
						rcont = JsonbIteratorNext(val, &vval, true);
						Assert(rcont == WJB_ELEM);

						if (vval.type == jbvBinary)
							lhsConts[j++] = vval;
					}

					/* 临时数组中没有容器元素，立即放弃 */
					if (j == 0)
						return false;

					/* 数组可能只被部分填充 */
					nLhsElems = j;
				}

				/* XXX: 嵌套数组包含是 O(N^2) */
				for (i = 0; i < nLhsElems; i++)
				{
					/* 嵌套容器值（对象或数组） */
					JsonbIterator *nestval,
							   *nestContained;
					bool		contains;

					nestval = JsonbIteratorInit(lhsConts[i].val.binary.data);
					nestContained = JsonbIteratorInit(vcontained.val.binary.data);

					contains = JsonbDeepContains(&nestval, &nestContained);

					if (nestval)
						pfree(nestval);
					if (nestContained)
						pfree(nestContained);
					if (contains)
						break;
				}

				/*
				 * 若 rhs 容器无法匹配*某个* lhs 容器，
				 * rhs 容器值未被包含
				 */
				if (i == nLhsElems)
					return false;
			}
		}
	}
	else
	{
		elog(ERROR, "invalid jsonb container type");
	}

	elog(ERROR, "unexpectedly fell off end of jsonb container");
	return false;
}

/*
 * 哈希 JsonbValue 标量值，把哈希值混入调用者提供的
 * 现有哈希中。
 *
 * 某些调用者可对 JB_FOBJECT 与 JB_FARRAY 标志独立异或
 *
 */
void
JsonbHashScalarValue(const JsonbValue *scalarVal, uint32 *hash)
{
	uint32		tmp;

	/* 为 scalarVal 计算哈希值 */
	switch (scalarVal->type)
	{
		case jbvNull:
			tmp = 0x01;
			break;
		case jbvString:
			tmp = DatumGetUInt32(hash_any((const unsigned char *) scalarVal->val.string.val,
										  scalarVal->val.string.len));
			break;
		case jbvNumeric:
			/* 相等 numeric 必有相等哈希码 */
			tmp = DatumGetUInt32(DirectFunctionCall1(hash_numeric,
													 NumericGetDatum(scalarVal->val.numeric)));
			break;
		case jbvBool:
			tmp = scalarVal->val.boolean ? 0x02 : 0x04;

			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
			tmp = 0;			/* 让编译器安静 */
			break;
	}

	/*
	 * 把前一个值左移 1 位、再异或进新键/值/元素的哈希
	 * 值，来合并后继键、值和元素的哈希值。
	 *
	 */
	*hash = pg_rotate_left32(*hash, 1);
	*hash ^= tmp;
}

/*
 * 把一个值哈希为 64 位值，带种子。其余同
 * JsonbHashScalarValue
 */
void
JsonbHashScalarValueExtended(const JsonbValue *scalarVal, uint64 *hash,
							 uint64 seed)
{
	uint64		tmp;

	switch (scalarVal->type)
	{
		case jbvNull:
			tmp = seed + 0x01;
			break;
		case jbvString:
			tmp = DatumGetUInt64(hash_any_extended((const unsigned char *) scalarVal->val.string.val,
												   scalarVal->val.string.len,
												   seed));
			break;
		case jbvNumeric:
			tmp = DatumGetUInt64(DirectFunctionCall2(hash_numeric_extended,
													 NumericGetDatum(scalarVal->val.numeric),
													 UInt64GetDatum(seed)));
			break;
		case jbvBool:
			if (seed)
				tmp = DatumGetUInt64(DirectFunctionCall2(hashcharextended,
														 BoolGetDatum(scalarVal->val.boolean),
														 UInt64GetDatum(seed)));
			else
				tmp = scalarVal->val.boolean ? 0x02 : 0x04;

			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
			break;
	}

	*hash = ROTATE_HIGH_AND_LOW_32BITS(*hash);
	*hash ^= tmp;
}

/*
 * 同类型的两个标量 JsonbValue a 和 b 相等吗？
 */
static bool
equalsJsonbScalarValue(JsonbValue *a, JsonbValue *b)
{
	if (a->type == b->type)
	{
		switch (a->type)
		{
			case jbvNull:
				return true;
			case jbvString:
				return lengthCompareJsonbStringValue(a, b) == 0;
			case jbvNumeric:
				return DatumGetBool(DirectFunctionCall2(numeric_eq,
														PointerGetDatum(a->val.numeric),
														PointerGetDatum(b->val.numeric)));
			case jbvBool:
				return a->val.boolean == b->val.boolean;

			default:
				elog(ERROR, "invalid jsonb scalar type");
		}
	}
	elog(ERROR, "jsonb scalar type mismatch");
	return false;
}

/*
 * 比较两个标量 JsonbValue，返回 -1、0 或 1。
 *
 * 字符串用默认排序规则比较，由 B-tree 算子使用，
 * 那里通常期望字典序的排序顺序。
 */
static int
compareJsonbScalarValue(JsonbValue *a, JsonbValue *b)
{
	if (a->type == b->type)
	{
		switch (a->type)
		{
			case jbvNull:
				return 0;
			case jbvString:
				return varstr_cmp(a->val.string.val,
								  a->val.string.len,
								  b->val.string.val,
								  b->val.string.len,
								  DEFAULT_COLLATION_OID);
			case jbvNumeric:
				return DatumGetInt32(DirectFunctionCall2(numeric_cmp,
														 PointerGetDatum(a->val.numeric),
														 PointerGetDatum(b->val.numeric)));
			case jbvBool:
				if (a->val.boolean == b->val.boolean)
					return 0;
				else if (a->val.boolean > b->val.boolean)
					return 1;
				else
					return -1;
			default:
				elog(ERROR, "invalid jsonb scalar type");
		}
	}
	elog(ERROR, "jsonb scalar type mismatch");
	return -1;
}


/*
 * 操作 convertJsonb 及其子例程所用的可变大小缓冲区
 * 的函数。
 */

/*
 * 在缓冲区末尾保留 "len" 个字节，必要时扩大缓冲区。
 * 返回保留区的偏移。调用者随后应使用 copyToBuffer()
 * 填充该保留区。
 */
static int
reserveFromBuffer(StringInfo buffer, int len)
{
	int			offset;

	/* 必要时扩大空间 */
	enlargeStringInfo(buffer, len);

	/* 记住当前偏移 */
	offset = buffer->len;

	/* 保留该空间 */
	buffer->len += len;

	/*
	 * 保留结尾 null，尽管它对我们没有用处；这样似乎
	 * 最好能保持 StringInfo 的各项不变式。
	 */
	buffer->data[buffer->len] = '\0';

	return offset;
}

/*
 * 把 "len" 字节复制到缓冲区先前保留区域。
 */
static void
copyToBuffer(StringInfo buffer, int offset, const char *data, int len)
{
	memcpy(buffer->data + offset, data, len);
}

/*
 * reserveFromBuffer + copyToBuffer 的简写。
 */
static void
appendToBuffer(StringInfo buffer, const char *data, int len)
{
	int			offset;

	offset = reserveFromBuffer(buffer, len);
	copyToBuffer(buffer, offset, data, len);
}


/*
 * 追加填充，使 StringInfo 的长度按 int 对齐。
 * 返回追加的填充字节数。
 */
static short
padBufferToInt(StringInfo buffer)
{
	int			padlen,
				p,
				offset;

	padlen = INTALIGN(buffer->len) - buffer->len;

	offset = reserveFromBuffer(buffer, padlen);

	/* padlen 必须很小，所以这多半比 memset 更快 */
	for (p = 0; p < padlen; p++)
		buffer->data[offset + p] = '\0';

	return padlen;
}

/*
 * 给定 JsonbValue，转为 Jsonb。结果 palloc 分配。
 */
static Jsonb *
convertToJsonb(JsonbValue *val)
{
	StringInfoData buffer;
	JEntry		jentry;
	Jsonb	   *res;

	/* 不应已经有二进制表示 */
	Assert(val->type != jbvBinary);

	/* 分配输出缓冲区，之后按需扩大 */
	initStringInfo(&buffer);

	/* 给 varlena 头部腾出空间 */
	reserveFromBuffer(&buffer, VARHDRSZ);

	convertJsonbValue(&buffer, &jentry, val, 0);

	/*
	 * 注意：根 JEntry 被丢弃。因此 JsonbContainer 结构
	 * 必须包含足够信息来表明它是哪种
	 * 类型的值。
	 */

	res = (Jsonb *) buffer.data;

	SET_VARSIZE(res, buffer.len);

	return res;
}

/*
 * convertJsonb 的子例程：把 JsonbValue 序列化进缓冲区。
 *
 * 该节点的 JEntry 头部通过 *header 返回。其中填有该值
 * 的长度和适当的类型位。如果我们希望存储结束偏移
 * 而非长度，则调整由调用者
 * 负责。
 *
 * 若值是数组或对象，这里会递归。"level" 仅用于调试
 * 目的。
 */
static void
convertJsonbValue(StringInfo buffer, JEntry *header, JsonbValue *val, int level)
{
	check_stack_depth();

	if (!val)
		return;

	/*
	 * 作为 val 传入的 JsonbValue 绝不应有 jbvBinary 类型，
	 * 其任何子组件也绝不应有。那些值会由 convertJsonbArray
	 * 和 convertJsonbObject 产生，其结果不会再作为参数传回
	 * 本函数。
	 */

	if (IsAJsonbScalar(val))
		convertJsonbScalar(buffer, header, val);
	else if (val->type == jbvArray)
		convertJsonbArray(buffer, header, val, level);
	else if (val->type == jbvObject)
		convertJsonbObject(buffer, header, val, level);
	else
		elog(ERROR, "unknown type of jsonb container to convert");
}

static void
convertJsonbArray(StringInfo buffer, JEntry *header, JsonbValue *val, int level)
{
	int			base_offset;
	int			jentry_offset;
	int			i;
	int			totallen;
	uint32		containerhead;
	int			nElems = val->val.array.nElems;

	/* 记住数组在缓冲区中的起始位置。 */
	base_offset = buffer->len;

	/* 对齐到 4 字节边界（填充也算作我的数据） */
	padBufferToInt(buffer);

	/*
	 * 构造头部 Jentry，把它存进变长载荷的开头。
	 *
	 */
	containerhead = nElems | JB_FARRAY;
	if (val->val.array.rawScalar)
	{
		Assert(nElems == 1);
		Assert(level == 0);
		containerhead |= JB_FSCALAR;
	}

	appendToBuffer(buffer, (char *) &containerhead, sizeof(uint32));

	/* 为元素的 JEntry 预留空间。 */
	jentry_offset = reserveFromBuffer(buffer, sizeof(JEntry) * nElems);

	totallen = 0;
	for (i = 0; i < nElems; i++)
	{
		JsonbValue *elem = &val->val.array.elems[i];
		int			len;
		JEntry		meta;

		/*
		 * 转换元素，产生 JEntry 并把其变长数据
		 * 追加到缓冲区
		 */
		convertJsonbValue(buffer, &meta, elem, level + 1);

		len = JBE_OFFLENFLD(meta);
		totallen += len;

		/*
		 * 若总变长数据超过 JEntry 长度字段容量则中止。
		 * 每轮迭代中都检查一次而非只在最后检查，
		 * 以预防可能的整数溢出。
		 */
		if (totallen > JENTRY_OFFLENMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb array elements exceeds the maximum of %d bytes",
							JENTRY_OFFLENMASK)));

		/*
		 * 把每 JB_OFFSET_STRIDE 个长度转为偏移。
		 */
		if ((i % JB_OFFSET_STRIDE) == 0)
			meta = (meta & JENTRY_TYPEMASK) | totallen | JENTRY_HAS_OFF;

		copyToBuffer(buffer, jentry_offset, (char *) &meta, sizeof(JEntry));
		jentry_offset += sizeof(JEntry);
	}

	/* 总数据大小即已追加到缓冲区的全部内容 */
	totallen = buffer->len - base_offset;

	/* 再次检查长度，因上面的元数据未计入 */
	if (totallen > JENTRY_OFFLENMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("total size of jsonb array elements exceeds the maximum of %d bytes",
						JENTRY_OFFLENMASK)));

	/* 在容器的 JEntry 数组中初始化本节点的头部 */
	*header = JENTRY_ISCONTAINER | totallen;
}

static void
convertJsonbObject(StringInfo buffer, JEntry *header, JsonbValue *val, int level)
{
	int			base_offset;
	int			jentry_offset;
	int			i;
	int			totallen;
	uint32		containerheader;
	int			nPairs = val->val.object.nPairs;

	/* 记住该对象在缓冲区中的起始位置。 */
	base_offset = buffer->len;

	/* 对齐到 4 字节边界（填充也算作我的数据） */
	padBufferToInt(buffer);

	/*
	 * 构造头部 Jentry，把它存进变长载荷的开头。
	 *
	 */
	containerheader = nPairs | JB_FOBJECT;
	appendToBuffer(buffer, (char *) &containerheader, sizeof(uint32));

	/* 为键和值的 JEntry 预留空间。 */
	jentry_offset = reserveFromBuffer(buffer, sizeof(JEntry) * nPairs * 2);

	/*
	 * 先遍历键再遍历值，因为磁盘上的表示采用的正是
	 * 这种顺序。
	 */
	totallen = 0;
	for (i = 0; i < nPairs; i++)
	{
		JsonbPair  *pair = &val->val.object.pairs[i];
		int			len;
		JEntry		meta;

		/*
		 * 转换键，产生一个 JEntry 并把其变长数据追加
		 * 到缓冲区
		 */
		convertJsonbScalar(buffer, &meta, &pair->key);

		len = JBE_OFFLENFLD(meta);
		totallen += len;

		/*
		 * 若总变长数据超过 JEntry 长度字段容量则中止。
		 * 每轮迭代中都检查一次而非只在最后检查，
		 * 以预防可能的整数溢出。
		 */
		if (totallen > JENTRY_OFFLENMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb object elements exceeds the maximum of %d bytes",
							JENTRY_OFFLENMASK)));

		/*
		 * 把每 JB_OFFSET_STRIDE 个长度转为偏移。
		 */
		if ((i % JB_OFFSET_STRIDE) == 0)
			meta = (meta & JENTRY_TYPEMASK) | totallen | JENTRY_HAS_OFF;

		copyToBuffer(buffer, jentry_offset, (char *) &meta, sizeof(JEntry));
		jentry_offset += sizeof(JEntry);
	}
	for (i = 0; i < nPairs; i++)
	{
		JsonbPair  *pair = &val->val.object.pairs[i];
		int			len;
		JEntry		meta;

		/*
		 * 转换值，产生一个 JEntry 并把其变长数据追加
		 * 到缓冲区
		 */
		convertJsonbValue(buffer, &meta, &pair->value, level + 1);

		len = JBE_OFFLENFLD(meta);
		totallen += len;

		/*
		 * 若总变长数据超过 JEntry 长度字段容量则中止。
		 * 每轮迭代中都检查一次而非只在最后检查，
		 * 以预防可能的整数溢出。
		 */
		if (totallen > JENTRY_OFFLENMASK)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("total size of jsonb object elements exceeds the maximum of %d bytes",
							JENTRY_OFFLENMASK)));

		/*
		 * 把每 JB_OFFSET_STRIDE 个长度转为偏移。
		 */
		if (((i + nPairs) % JB_OFFSET_STRIDE) == 0)
			meta = (meta & JENTRY_TYPEMASK) | totallen | JENTRY_HAS_OFF;

		copyToBuffer(buffer, jentry_offset, (char *) &meta, sizeof(JEntry));
		jentry_offset += sizeof(JEntry);
	}

	/* 总数据大小即已追加到缓冲区的全部内容 */
	totallen = buffer->len - base_offset;

	/* 再次检查长度，因上面的元数据未计入 */
	if (totallen > JENTRY_OFFLENMASK)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("total size of jsonb object elements exceeds the maximum of %d bytes",
						JENTRY_OFFLENMASK)));

	/* 在容器的 JEntry 数组中初始化本节点的头部 */
	*header = JENTRY_ISCONTAINER | totallen;
}

static void
convertJsonbScalar(StringInfo buffer, JEntry *header, JsonbValue *scalarVal)
{
	int			numlen;
	short		padlen;

	switch (scalarVal->type)
	{
		case jbvNull:
			*header = JENTRY_ISNULL;
			break;

		case jbvString:
			appendToBuffer(buffer, scalarVal->val.string.val, scalarVal->val.string.len);

			*header = scalarVal->val.string.len;
			break;

		case jbvNumeric:
			numlen = VARSIZE_ANY(scalarVal->val.numeric);
			padlen = padBufferToInt(buffer);

			appendToBuffer(buffer, (char *) scalarVal->val.numeric, numlen);

			*header = JENTRY_ISNUMERIC | (padlen + numlen);
			break;

		case jbvBool:
			*header = (scalarVal->val.boolean) ?
				JENTRY_ISBOOL_TRUE : JENTRY_ISBOOL_FALSE;
			break;

		case jbvDatetime:
			{
				char		buf[MAXDATELEN + 1];
				size_t		len;

				JsonEncodeDateTime(buf,
								   scalarVal->val.datetime.value,
								   scalarVal->val.datetime.typid,
								   &scalarVal->val.datetime.tz);
				len = strlen(buf);
				appendToBuffer(buffer, buf, len);

				*header = len;
			}
			break;

		default:
			elog(ERROR, "invalid jsonb scalar type");
	}
}

/*
 * 比较两个 jbvString JsonbValue 值 a 和 b。
 *
 * 这是一个特殊的 qsort() 比较器，用于在某些只需要
 * 明确定义排序顺序的内部上下文中对字符串排序。特
 * 别是，对象对的键会按这一标准排序，以便在不关心字
 * 典序排序顺序的场合下进行廉价的二分
 * 查找
 *
 * a 和 b 先按长度排序。只有当需要决胜时，我们才
 * 考虑字符串的二进制相等性。
 */
static int
lengthCompareJsonbStringValue(const void *a, const void *b)
{
	const JsonbValue *va = (const JsonbValue *) a;
	const JsonbValue *vb = (const JsonbValue *) b;

	Assert(va->type == jbvString);
	Assert(vb->type == jbvString);

	return lengthCompareJsonbString(va->val.string.val, va->val.string.len,
									vb->val.string.val, vb->val.string.len);
}

/*
 * lengthCompareJsonbStringValue 的子例程
 *
 * 单独用来对 JsonbContainer 做二分查找时它也
 * 有用。
 */
static int
lengthCompareJsonbString(const char *val1, int len1, const char *val2, int len2)
{
	if (len1 == len2)
		return memcmp(val1, val2, len1);
	else
		return len1 > len2 ? 1 : -1;
}

/*
 * 比较 JsonbPair 值的 qsort_arg() 比较器
 *
 * 第三个参数 "binequal" 可以指向 bool。若设置了它，则当 a
 * 和 b 完全二进制相等时 *binequal 被设为 true，因为某些
 * 调用者关心两个值是完全相等还是仅仅等价。
 *
 * N.B: 此处的字符串比较是"按长度"的
 *
 * 键相等的对在排序时保持 order 字段的顺序。
 */
static int
lengthCompareJsonbPair(const void *a, const void *b, void *binequal)
{
	const JsonbPair *pa = (const JsonbPair *) a;
	const JsonbPair *pb = (const JsonbPair *) b;
	int			res;

	res = lengthCompareJsonbStringValue(&pa->key, &pb->key);
	if (res == 0 && binequal)
		*((bool *) binequal) = true;

	/*
	 * 保证相等的对保持原顺序。唯一化算法会优先选择
	 * 首个元素作为值
	 */
	if (res == 0)
		res = (pa->order > pb->order) ? -1 : 1;

	return res;
}

/*
 * 对 JsonbValue 对象中的对排序并去重
 */
static void
uniqueifyJsonbObject(JsonbValue *object, bool unique_keys, bool skip_nulls)
{
	bool		hasNonUniq = false;

	Assert(object->type == jbvObject);

	if (object->val.object.nPairs > 1)
		qsort_arg(object->val.object.pairs, object->val.object.nPairs, sizeof(JsonbPair),
				  lengthCompareJsonbPair, &hasNonUniq);

	if (hasNonUniq && unique_keys)
		ereport(ERROR,
				errcode(ERRCODE_DUPLICATE_JSON_OBJECT_KEY_VALUE),
				errmsg("duplicate JSON object key value"));

	if (hasNonUniq || skip_nulls)
	{
		JsonbPair  *ptr,
				   *res;

		while (skip_nulls && object->val.object.nPairs > 0 &&
			   object->val.object.pairs->value.type == jbvNull)
		{
			/* 若 skip_nulls 为 true，移除开头的 null 项 */
			object->val.object.pairs++;
			object->val.object.nPairs--;
		}

		if (object->val.object.nPairs > 0)
		{
			ptr = object->val.object.pairs + 1;
			res = object->val.object.pairs;

			while (ptr - object->val.object.pairs < object->val.object.nPairs)
			{
				/* 避免复制重复项或 null 项 */
				if (lengthCompareJsonbStringValue(ptr, res) != 0 &&
					(!skip_nulls || ptr->value.type != jbvNull))
				{
					res++;
					if (ptr != res)
						memcpy(res, ptr, sizeof(JsonbPair));
				}
				ptr++;
			}

			object->val.object.nPairs = res + 1 - object->val.object.pairs;
		}
	}
}
