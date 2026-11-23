/*-------------------------------------------------------------------------
 *
 * jsonb_gin.c
 *	 jsonb 的 GIN 支持函数
 *
 * Copyright (c) 2014-2023, PostgreSQL Global Development Group
 *
 * 为 jsonb 索引提供两个操作类: jsonb_ops 与 jsonb_path_ops。
 * 说明见 json.sgml 与 jsonb.h 中的注释。
 *
 * 算子还支持 "jsonb @? jsonpath" 与
 * "jsonb @@ jsonpath"。含这些算子的表达式可轻易
 * 互相表达。
 *
 *	jb @? 'path' <=> jb @@ 'EXISTS(path)'
 *	jb @@ 'expr' <=> jb @? '$ ? (expr)'
 *
 * 故下文只论 @@ 算子;对 @? 算子而言
 * 等同于 jb @@ 'EXISTS(path)'。
 *
 * jsonpath 查询提取结果是一棵树,叶节点为索引
 * 条目,非叶节点为 AND/OR 逻辑表达式。我们基本上从
 * jsonpath 提取下列语句:
 *
 *	1) "accessors_chain = const",
 *	2) "EXISTS(accessors_chain)".
 *
 * 访问器链可由 .key、[*] 与 [index] 访问器组成。jsonb_ops
 * 还支持 .* 与 .**。
 *
 * 目前 jsonb_ops 与 jsonb_path_ops 都只支持第 1 种
 * 语句。jsonb_ops 或许也能支持第 2 种,
 * 但因无统计信息,自访问器链提取的键
 * 多半非选择性。故选择不迷惑优化器,
 * 一并跳过第 2 种语句。将来版本或
 * 会改变。
 *
 * 在 jsonb_ops 中第 1 种语句拆成 AND 连接的键与
 * const。有时 const 在 jsonb_ops 中可既当值又当键,
 * 此时第 1 种语句分解为下列表达式。
 *
 *
 *	key1 AND key2 AND ... AND keyN AND (const_as_value OR const_as_key)
 *
 * jsonb_path_ops 把每个第 1 种语句变成下式单个哈希条目。
 *
 *
 *	HASH(key1, key2, ... , keyN, const)
 *
 * 尽管 jsonb_ops 与 jsonb_path_ops 都不支持第 2 种
 * 语句,EXISTS(path) 若可从其过滤器提取第 1 种
 * 语句则仍可能得到支持。
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_gin.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/varlena.h"

typedef struct PathHashStack
{
	uint32		hash;
	struct PathHashStack *parent;
} PathHashStack;

/* GIN 条目缓冲区 */
typedef struct GinEntries
{
	Datum	   *buf;
	int			count;
	int			allocated;
} GinEntries;

typedef enum JsonPathGinNodeType
{
	JSP_GIN_OR,
	JSP_GIN_AND,
	JSP_GIN_ENTRY
} JsonPathGinNodeType;

typedef struct JsonPathGinNode JsonPathGinNode;

/* jsonpath 表达式树节点 */
struct JsonPathGinNode
{
	JsonPathGinNodeType type;
	union
	{
		int			nargs;		/* 对 OR/AND 节点有效 */
		int			entryIndex; /* GinEntries 下标,条目输出后对 ENTRY
								 * 节点有效 */
		Datum		entryDatum; /* 路径哈希或键名/标量,输出前
								 * 对 ENTRY 节点有效 */
	}			val;
	JsonPathGinNode *args[FLEXIBLE_ARRAY_MEMBER];	/* OR/AND 节点有效
													 *  */
};

/*
 * 自 jsonpath 项提取的 jsonb_ops 条目。对应路径项可为:
 * '.key'、'.*'、'.**'、'[index]' 或 '[*]'。
 * 条目类型存在 'type' 字段。
 */
typedef struct JsonPathGinPathItem
{
	struct JsonPathGinPathItem *parent;
	Datum		keyName;		/* 键名(对 .key 路径项)或 NULL */
	JsonPathItemType type;		/* jsonpath 项的类型 */
} JsonPathGinPathItem;

/* 所提取 json 路径的 GIN 表示 */
typedef union JsonPathGinPath
{
	JsonPathGinPathItem *items; /* 路径项列表 (jsonb_ops) */
	uint32		hash;			/* 路径的哈希 (jsonb_path_ops) */
} JsonPathGinPath;

typedef struct JsonPathGinContext JsonPathGinContext;

/* 存路径项信息到 JsonPathGinPath 的回调 */
typedef bool (*JsonPathGinAddPathItemFunc) (JsonPathGinPath *path,
											JsonPathItem *jsp);

/*
 * 自第 1 种语句(scalar != NULL)或第 2 种语句
 * (scalar == NULL)提取节点集的回调。
 */
typedef List *(*JsonPathGinExtractNodesFunc) (JsonPathGinContext *cxt,
											  JsonPathGinPath path,
											  JsonbValue *scalar,
											  List *nodes);

/* jsonpath 条目提取上下文 */
struct JsonPathGinContext
{
	JsonPathGinAddPathItemFunc add_path_item;
	JsonPathGinExtractNodesFunc extract_nodes;
	bool		lax;
};

static Datum make_text_key(char flag, const char *str, int len);
static Datum make_scalar_key(const JsonbValue *scalarVal, bool is_key);

static JsonPathGinNode *extract_jsp_bool_expr(JsonPathGinContext *cxt,
											  JsonPathGinPath path, JsonPathItem *jsp, bool not);


/* 初始化 GinEntries 结构 */
static void
init_gin_entries(GinEntries *entries, int preallocated)
{
	entries->allocated = preallocated;
	entries->buf = preallocated ? palloc(sizeof(Datum) * preallocated) : NULL;
	entries->count = 0;
}

/* 向 GinEntries 加条目 */
static int
add_gin_entry(GinEntries *entries, Datum entry)
{
	int			id = entries->count;

	if (entries->count >= entries->allocated)
	{
		if (entries->allocated)
		{
			entries->allocated *= 2;
			entries->buf = repalloc(entries->buf,
									sizeof(Datum) * entries->allocated);
		}
		else
		{
			entries->allocated = 8;
			entries->buf = palloc(sizeof(Datum) * entries->allocated);
		}
	}

	entries->buf[entries->count++] = entry;

	return id;
}

/*
 *
 * jsonb_ops GIN 操作类支持函数
 *
 */

Datum
gin_compare_jsonb(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		result;
	char	   *a1p,
			   *a2p;
	int			len1,
				len2;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	/* 按 bttextcmp 比较 text,但始终用 C 排序规则 */
	result = varstr_cmp(a1p, len1, a2p, len2, C_COLLATION_OID);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

Datum
gin_extract_jsonb(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = (Jsonb *) PG_GETARG_JSONB_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	int			total = JB_ROOT_COUNT(jb);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	GinEntries	entries;

	/* 若根级为空,则肯定没有任何键 */
	if (total == 0)
	{
		*nentries = 0;
		PG_RETURN_POINTER(NULL);
	}

	/* 否则用 2 * 根计数作为结果大小的初始估计 */
	init_gin_entries(&entries, 2 * total);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (r)
		{
			case WJB_KEY:
				add_gin_entry(&entries, make_scalar_key(&v, true));
				break;
			case WJB_ELEM:
				/* 假装字符串数组元素是键,见 jsonb.h */
				add_gin_entry(&entries, make_scalar_key(&v, v.type == jbvString));
				break;
			case WJB_VALUE:
				add_gin_entry(&entries, make_scalar_key(&v, false));
				break;
			default:
				/* 可忽略结构性项 */
				break;
		}
	}

	*nentries = entries.count;

	PG_RETURN_POINTER(entries.buf);
}

/* 追加 JsonPathGinPathItem 到 JsonPathGinPath(jsonb_ops) */
static bool
jsonb_ops__add_path_item(JsonPathGinPath *path, JsonPathItem *jsp)
{
	JsonPathGinPathItem *pentry;
	Datum		keyName;

	switch (jsp->type)
	{
		case jpiRoot:
			path->items = NULL; /* 清路径 */
			return true;

		case jpiKey:
			{
				int			len;
				char	   *key = jspGetString(jsp, &len);

				keyName = make_text_key(JGINFLAG_KEY, key, len);
				break;
			}

		case jpiAny:
		case jpiAnyKey:
		case jpiAnyArray:
		case jpiIndexArray:
			keyName = PointerGetDatum(NULL);
			break;

		default:
			/* 项方法等其他路径项不受支持 */
			return false;
	}

	pentry = palloc(sizeof(*pentry));

	pentry->type = jsp->type;
	pentry->keyName = keyName;
	pentry->parent = path->items;

	path->items = pentry;

	return true;
}

/* 合并现有路径哈希与下一键哈希 (jsonb_path_ops) */
static bool
jsonb_path_ops__add_path_item(JsonPathGinPath *path, JsonPathItem *jsp)
{
	switch (jsp->type)
	{
		case jpiRoot:
			path->hash = 0;		/* 清路径哈希 */
			return true;

		case jpiKey:
			{
				JsonbValue	jbv;

				jbv.type = jbvString;
				jbv.val.string.val = jspGetString(jsp, &jbv.val.string.len);

				JsonbHashScalarValue(&jbv, &path->hash);
				return true;
			}

		case jpiIndexArray:
		case jpiAnyArray:
			return true;		/* 路径哈希不变 */

		default:
			/* 其他项(通配路径、项方法)不受支持 */
			return false;
	}
}

static JsonPathGinNode *
make_jsp_entry_node(Datum entry)
{
	JsonPathGinNode *node = palloc(offsetof(JsonPathGinNode, args));

	node->type = JSP_GIN_ENTRY;
	node->val.entryDatum = entry;

	return node;
}

static JsonPathGinNode *
make_jsp_entry_node_scalar(JsonbValue *scalar, bool iskey)
{
	return make_jsp_entry_node(make_scalar_key(scalar, iskey));
}

static JsonPathGinNode *
make_jsp_expr_node(JsonPathGinNodeType type, int nargs)
{
	JsonPathGinNode *node = palloc(offsetof(JsonPathGinNode, args) +
								   sizeof(node->args[0]) * nargs);

	node->type = type;
	node->val.nargs = nargs;

	return node;
}

static JsonPathGinNode *
make_jsp_expr_node_args(JsonPathGinNodeType type, List *args)
{
	JsonPathGinNode *node = make_jsp_expr_node(type, list_length(args));
	ListCell   *lc;
	int			i = 0;

	foreach(lc, args)
		node->args[i++] = lfirst(lc);

	return node;
}

static JsonPathGinNode *
make_jsp_expr_node_binary(JsonPathGinNodeType type,
						  JsonPathGinNode *arg1, JsonPathGinNode *arg2)
{
	JsonPathGinNode *node = make_jsp_expr_node(type, 2);

	node->args[0] = arg1;
	node->args[1] = arg2;

	return node;
}

/* 从 jsonpath 追加节点列表 (jsonb_ops)。 */
static List *
jsonb_ops__extract_nodes(JsonPathGinContext *cxt, JsonPathGinPath path,
						 JsonbValue *scalar, List *nodes)
{
	JsonPathGinPathItem *pentry;

	if (scalar)
	{
		JsonPathGinNode *node;

		/*
		 * 仅当提供了标量时才追加路径条目节点。详见
		 * 文件头注释。
		 */
		for (pentry = path.items; pentry; pentry = pentry->parent)
		{
			if (pentry->type == jpiKey) /* 只索引键 */
				nodes = lappend(nodes, make_jsp_entry_node(pentry->keyName));
		}

		/* 为相等查询追加标量节点。 */
		if (scalar->type == jbvString)
		{
			JsonPathGinPathItem *last = path.items;
			GinTernaryValue key_entry;

			/*
			 * 假定 jsonb_ops 视字符串数组元素为键,或可
			 * 提取键或非键条目乃至二者。后者则创建
			 * OR 节点。此可于宽松模式(数组自动展开),或
			 * 严格模式下对 jpiAny 项发生。
			 *
			 */

			if (cxt->lax)
				key_entry = GIN_MAYBE;
			else if (!last)		/* 根 ($) */
				key_entry = GIN_FALSE;
			else if (last->type == jpiAnyArray || last->type == jpiIndexArray)
				key_entry = GIN_TRUE;
			else if (last->type == jpiAny)
				key_entry = GIN_MAYBE;
			else
				key_entry = GIN_FALSE;

			if (key_entry == GIN_MAYBE)
			{
				JsonPathGinNode *n1 = make_jsp_entry_node_scalar(scalar, true);
				JsonPathGinNode *n2 = make_jsp_entry_node_scalar(scalar, false);

				node = make_jsp_expr_node_binary(JSP_GIN_OR, n1, n2);
			}
			else
			{
				node = make_jsp_entry_node_scalar(scalar,
												  key_entry == GIN_TRUE);
			}
		}
		else
		{
			node = make_jsp_entry_node_scalar(scalar, false);
		}

		nodes = lappend(nodes, node);
	}

	return nodes;
}

/* 从 jsonpath 追加节点列表 (jsonb_path_ops)。 */
static List *
jsonb_path_ops__extract_nodes(JsonPathGinContext *cxt, JsonPathGinPath path,
							  JsonbValue *scalar, List *nodes)
{
	if (scalar)
	{
		/* 为相等查询追加路径哈希节点 */
		uint32		hash = path.hash;

		JsonbHashScalarValue(scalar, &hash);

		return lappend(nodes,
					   make_jsp_entry_node(UInt32GetDatum(hash)));
	}
	else
	{
		/* jsonb_path_ops 不支持 EXISTS => 无可追加 */
		return nodes;
	}
}

/*
 * 提取需由调用者 AND 连接的表达式节点列表。
 * 若 scalar 非 NULL,提取式为 'path == scalar';否则为
 * 'EXISTS(path)'。
 */
static List *
extract_jsp_path_expr_nodes(JsonPathGinContext *cxt, JsonPathGinPath path,
							JsonPathItem *jsp, JsonbValue *scalar)
{
	JsonPathItem next;
	List	   *nodes = NIL;

	for (;;)
	{
		switch (jsp->type)
		{
			case jpiCurrent:
				break;

			case jpiFilter:
				{
					JsonPathItem arg;
					JsonPathGinNode *filter;

					jspGetArg(jsp, &arg);

					filter = extract_jsp_bool_expr(cxt, path, &arg, false);

					if (filter)
						nodes = lappend(nodes, filter);

					break;
				}

			default:
				if (!cxt->add_path_item(&path, jsp))

					/*
					 * 索引操作类不支持该路径,只返回已提取的
					 * 过滤器节点。
					 */
					return nodes;
				break;
		}

		if (!jspGetNext(jsp, &next))
			break;

		jsp = &next;
	}

	/*
	 * 把路径表达式自身节点追加到已提取的
	 * 过滤节点列表。
	 */
	return cxt->extract_nodes(cxt, path, scalar, nodes);
}

/*
 * 自下列 jsonpath 路径表达式提取表达式节点:
 *     EXISTS(jsp)    (scalar==NULL)
 *     jsp==scalar(非 NULL)。
 *
 * 当前路径 (@) 经 'path' 传入。
 */
static JsonPathGinNode *
extract_jsp_path_expr(JsonPathGinContext *cxt, JsonPathGinPath path,
					  JsonPathItem *jsp, JsonbValue *scalar)
{
	/* 提取待 AND 连接的节点列表 */
	List	   *nodes = extract_jsp_path_expr_nodes(cxt, path, jsp, scalar);

	if (nodes == NIL)
		/* 未提取到节点 => 此路径需全扫描 */
		return NULL;

	if (list_length(nodes) == 1)
		return linitial(nodes); /* 避多余 AND 节点 */

	/* 为过滤路径建 AND 节点 */
	return make_jsp_expr_node_args(JSP_GIN_AND, nodes);
}

/* 从布尔 jsonpath 表达式递归提取节点。 */
static JsonPathGinNode *
extract_jsp_bool_expr(JsonPathGinContext *cxt, JsonPathGinPath path,
					  JsonPathItem *jsp, bool not)
{
	check_stack_depth();

	switch (jsp->type)
	{
		case jpiAnd:			/* expr && expr */
		case jpiOr:				/* expr || expr */
			{
				JsonPathItem arg;
				JsonPathGinNode *larg;
				JsonPathGinNode *rarg;
				JsonPathGinNodeType type;

				jspGetLeftArg(jsp, &arg);
				larg = extract_jsp_bool_expr(cxt, path, &arg, not);

				jspGetRightArg(jsp, &arg);
				rarg = extract_jsp_bool_expr(cxt, path, &arg, not);

				if (!larg || !rarg)
				{
					if (jsp->type == jpiOr)
						return NULL;

					return larg ? larg : rarg;
				}

				type = not ^ (jsp->type == jpiAnd) ? JSP_GIN_AND : JSP_GIN_OR;

				return make_jsp_expr_node_binary(type, larg, rarg);
			}

		case jpiNot:			/* !expr */
			{
				JsonPathItem arg;

				jspGetArg(jsp, &arg);

				/* 提取子表达式并反转 not 标志 */
				return extract_jsp_bool_expr(cxt, path, &arg, !not);
			}

		case jpiExists:			/* EXISTS(path) */
			{
				JsonPathItem arg;

				if (not)
					return NULL;	/* 不支持 NOT EXISTS */

				jspGetArg(jsp, &arg);

				return extract_jsp_path_expr(cxt, path, &arg, NULL);
			}

		case jpiNotEqual:

			/*
			 * 此处不支持 'not'==true 情形,因为
			 * 一般地 '!(path != scalar)' 不等价于 'path == scalar',
			 * 缘于序列比较语义:
			 * 即 'path == scalar' === 'EXISTS (path, @ == scalar)'
			 * 且 '!(path != scalar)' === 'FOR_ALL(path, @ == scalar)'。
			 * 故应译为 path==scalar||EMPTY(path),但
			 * 两种 jsonb 操作类都不支持 EMPTY(path)
			 * 查询。不过严格模式下若路径恰返回一项(不含
			 * 通配访问器或 .keyvalue() 等项方法),可省略
			 * EMPTY(path) 部分。
			 *
			 */
			return NULL;

		case jpiEqual:			/* path == scalar */
			{
				JsonPathItem left_item;
				JsonPathItem right_item;
				JsonPathItem *path_item;
				JsonPathItem *scalar_item;
				JsonbValue	scalar;

				if (not)
					return NULL;

				jspGetLeftArg(jsp, &left_item);
				jspGetRightArg(jsp, &right_item);

				if (jspIsScalar(left_item.type))
				{
					scalar_item = &left_item;
					path_item = &right_item;
				}
				else if (jspIsScalar(right_item.type))
				{
					scalar_item = &right_item;
					path_item = &left_item;
				}
				else
					return NULL;	/* 至少一个操作数应为标量 */

				switch (scalar_item->type)
				{
					case jpiNull:
						scalar.type = jbvNull;
						break;
					case jpiBool:
						scalar.type = jbvBool;
						scalar.val.boolean = !!*scalar_item->content.value.data;
						break;
					case jpiNumeric:
						scalar.type = jbvNumeric;
						scalar.val.numeric =
							(Numeric) scalar_item->content.value.data;
						break;
					case jpiString:
						scalar.type = jbvString;
						scalar.val.string.val = scalar_item->content.value.data;
						scalar.val.string.len =
							scalar_item->content.value.datalen;
						break;
					default:
						elog(ERROR, "invalid scalar jsonpath item type: %d",
							 scalar_item->type);
						return NULL;
				}

				return extract_jsp_path_expr(cxt, path, path_item, &scalar);
			}

		default:
			return NULL;		/* 不是布尔表达式 */
	}
}

/* 递归发出节点树中找到的全部 GIN 条目 */
static void
emit_jsp_gin_entries(JsonPathGinNode *node, GinEntries *entries)
{
	check_stack_depth();

	switch (node->type)
	{
		case JSP_GIN_ENTRY:
			/* 用其在数组中的下标替换 datum */
			node->val.entryIndex = add_gin_entry(entries, node->val.entryDatum);
			break;

		case JSP_GIN_OR:
		case JSP_GIN_AND:
			{
				int			i;

				for (i = 0; i < node->val.nargs; i++)
					emit_jsp_gin_entries(node->args[i], entries);

				break;
			}
	}
}

/*
 * 从 jsonpath 查询递归提取 GIN 条目。
 * 根表达式节点放入 (*extra_data)[0]。
 */
static Datum *
extract_jsp_query(JsonPath *jp, StrategyNumber strat, bool pathOps,
				  int32 *nentries, Pointer **extra_data)
{
	JsonPathGinContext cxt;
	JsonPathItem root;
	JsonPathGinNode *node;
	JsonPathGinPath path = {0};
	GinEntries	entries = {0};

	cxt.lax = (jp->header & JSONPATH_LAX) != 0;

	if (pathOps)
	{
		cxt.add_path_item = jsonb_path_ops__add_path_item;
		cxt.extract_nodes = jsonb_path_ops__extract_nodes;
	}
	else
	{
		cxt.add_path_item = jsonb_ops__add_path_item;
		cxt.extract_nodes = jsonb_ops__extract_nodes;
	}

	jspInit(&root, jp);

	node = strat == JsonbJsonpathExistsStrategyNumber
		? extract_jsp_path_expr(&cxt, path, &root, NULL)
		: extract_jsp_bool_expr(&cxt, path, &root, false);

	if (!node)
	{
		*nentries = 0;
		return NULL;
	}

	emit_jsp_gin_entries(node, &entries);

	*nentries = entries.count;
	if (!*nentries)
		return NULL;

	*extra_data = palloc0(sizeof(**extra_data) * entries.count);
	**extra_data = (Pointer) node;

	return entries.buf;
}

/*
 * 递归执行 jsonpath 表达式。
 * 'check' 依 'ternary' 标志为 bool[] 或 GinTernaryValue[]。
 */
static GinTernaryValue
execute_jsp_gin_node(JsonPathGinNode *node, void *check, bool ternary)
{
	GinTernaryValue res;
	GinTernaryValue v;
	int			i;

	switch (node->type)
	{
		case JSP_GIN_AND:
			res = GIN_TRUE;
			for (i = 0; i < node->val.nargs; i++)
			{
				v = execute_jsp_gin_node(node->args[i], check, ternary);
				if (v == GIN_FALSE)
					return GIN_FALSE;
				else if (v == GIN_MAYBE)
					res = GIN_MAYBE;
			}
			return res;

		case JSP_GIN_OR:
			res = GIN_FALSE;
			for (i = 0; i < node->val.nargs; i++)
			{
				v = execute_jsp_gin_node(node->args[i], check, ternary);
				if (v == GIN_TRUE)
					return GIN_TRUE;
				else if (v == GIN_MAYBE)
					res = GIN_MAYBE;
			}
			return res;

		case JSP_GIN_ENTRY:
			{
				int			index = node->val.entryIndex;

				if (ternary)
					return ((GinTernaryValue *) check)[index];
				else
					return ((bool *) check)[index] ? GIN_TRUE : GIN_FALSE;
			}

		default:
			elog(ERROR, "invalid jsonpath gin node type: %d", node->type);
			return GIN_FALSE;	/* 让编译器安静 */
	}
}

Datum
gin_extract_jsonb_query(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/* 查询是 jsonb,故直接套用 gin_extract_jsonb... */
		entries = (Datum *)
			DatumGetPointer(DirectFunctionCall2(gin_extract_jsonb,
												PG_GETARG_DATUM(0),
												PointerGetDatum(nentries)));
		/* 不过 "contains {}" 需要全索引扫描 */
		if (*nentries == 0)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbExistsStrategyNumber)
	{
		/* 查询是文本串,当作键处理 */
		text	   *query = PG_GETARG_TEXT_PP(0);

		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		entries[0] = make_text_key(JGINFLAG_KEY,
								   VARDATA_ANY(query),
								   VARSIZE_ANY_EXHDR(query));
	}
	else if (strategy == JsonbExistsAnyStrategyNumber ||
			 strategy == JsonbExistsAllStrategyNumber)
	{
		/* 查询是文本数组;每个元素当作键 */
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i,
					j;

		deconstruct_array_builtin(query, TEXTOID, &key_datums, &key_nulls, &key_count);

		entries = (Datum *) palloc(sizeof(Datum) * key_count);

		for (i = 0, j = 0; i < key_count; i++)
		{
			/* 忽略数组中的空值 */
			if (key_nulls[i])
				continue;
			/* 依赖数组元素未被 toast */
			entries[j++] = make_text_key(JGINFLAG_KEY,
										 VARDATA_ANY(key_datums[i]),
										 VARSIZE_ANY_EXHDR(key_datums[i]));
		}

		*nentries = j;
		/* 无键的 ExistsAll 应匹配一切 */
		if (j == 0 && strategy == JsonbExistsAllStrategyNumber)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		JsonPath   *jp = PG_GETARG_JSONPATH_P(0);
		Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

		entries = extract_jsp_query(jp, strategy, false, nentries, extra_data);

		if (!entries)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else
	{
		elog(ERROR, "unrecognized strategy number: %d", strategy);
		entries = NULL;			/* 让编译器安静 */
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_consistent_jsonb(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * 须始终重检,因无法从索引判断匹配项位置
		 * 是否与查询对象结构相符。(即便能,还得顾虑
		 * 哈希键,及索引无法区分键与字符串数组
		 * 元素。)但除非元组含全部查询键,否则
		 * 肯定不匹配。
		 *
		 */
		*recheck = true;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == JsonbExistsStrategyNumber)
	{
		/*
		 * 虽键肯定在索引中,仍须重检,因 (1) 键可能
		 * 已哈希,(2) 匹配可能落在非 JSON 对象顶层的
		 * 键上。对 (1) 可看查询键是否已哈希以决定
		 * 是否跳过重检,但索引信息不足,无法判定 (2)。
		 *
		 */
		*recheck = true;
		res = true;
	}
	else if (strategy == JsonbExistsAnyStrategyNumber)
	{
		/* 同普通 exists,须重新检查 */
		*recheck = true;
		res = true;
	}
	else if (strategy == JsonbExistsAllStrategyNumber)
	{
		/* 同普通 exists,须重新检查 */
		*recheck = true;
		/* 但除非全部键都在,否则可判为 false */
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		*recheck = true;

		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   false) != GIN_FALSE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}

Datum
gin_triconsistent_jsonb(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_MAYBE;
	int32		i;

	/*
	 * 注意从不返回 GIN_TRUE,只返回 GIN_MAYBE 或
	 * GIN_FALSE;对应常规 consistent 中始终强制
	 * 重新检查,理由见该处所列。
	 */
	if (strategy == JsonbContainsStrategyNumber ||
		strategy == JsonbExistsAllStrategyNumber)
	{
		/* 全部已提取键都必须存在 */
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_FALSE)
			{
				res = GIN_FALSE;
				break;
			}
		}
	}
	else if (strategy == JsonbExistsStrategyNumber ||
			 strategy == JsonbExistsAnyStrategyNumber)
	{
		/* 至少一个已提取键必须存在 */
		res = GIN_FALSE;
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_TRUE ||
				check[i] == GIN_MAYBE)
			{
				res = GIN_MAYBE;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   true);

			/* 应始终重新检查结果 */
			if (res == GIN_TRUE)
				res = GIN_MAYBE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 *
 * jsonb_path_ops GIN 操作类支持函数
 *
 * jsonb_path_ops 索引中 GIN key 为 uint32 哈希,每
 * 个 JSON 值一个;通向该值的 JSON key 也纳入哈希。
 * 故只能支持包含查询,但索引能区分
 * 如 {"foo": 42} 与 {"bar": 42},因其生成不同
 * 哈希。
 *
 */

Datum
gin_extract_jsonb_path(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	int			total = JB_ROOT_COUNT(jb);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	PathHashStack tail;
	PathHashStack *stack;
	GinEntries	entries;

	/* 若根级为空,则肯定没有任何键 */
	if (total == 0)
	{
		*nentries = 0;
		PG_RETURN_POINTER(NULL);
	}

	/* 否则用 2 * 根计数作为结果大小的初始估计 */
	init_gin_entries(&entries, 2 * total);

	/* 维护与父键层级对应的部分哈希栈 */
	tail.parent = NULL;
	tail.hash = 0;
	stack = &tail;

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		PathHashStack *parent;

		switch (r)
		{
			case WJB_BEGIN_ARRAY:
			case WJB_BEGIN_OBJECT:
				/* 为此对象压入一层栈 */
				parent = stack;
				stack = (PathHashStack *) palloc(sizeof(PathHashStack));

				/*
				 * 把外层嵌套级的哈希向前传递,使嵌套值的
				 * 哈希既含外层键也含自身键。
				 *
				 *
				 * 数组套数组不改最内标量元素哈希值,
				 * 但这似乎无关紧要。
				 *
				 */
				stack->hash = parent->hash;
				stack->parent = parent;
				break;
			case WJB_KEY:
				/* 把此键混入当前外层哈希 */
				JsonbHashScalarValue(&v, &stack->hash);
				/* 哈希现已就绪,可并入值 */
				break;
			case WJB_ELEM:
			case WJB_VALUE:
				/* 把元素或值的哈希混入已准备的哈希 */
				JsonbHashScalarValue(&v, &stack->hash);
				/* 发出索引条目 */
				add_gin_entry(&entries, UInt32GetDatum(stack->hash));
				/* 为下一键、值或子对象复位哈希 */
				stack->hash = stack->parent->hash;
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				/* 弹出栈 */
				parent = stack->parent;
				pfree(stack);
				stack = parent;
				/* 为下一键、值或子对象复位哈希 */
				if (stack->parent)
					stack->hash = stack->parent->hash;
				else
					stack->hash = 0;
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", (int) r);
		}
	}

	*nentries = entries.count;

	PG_RETURN_POINTER(entries.buf);
}

Datum
gin_extract_jsonb_query_path(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/* 查询是 jsonb,故直接套用 gin_extract_jsonb_path ... */
		entries = (Datum *)
			DatumGetPointer(DirectFunctionCall2(gin_extract_jsonb_path,
												PG_GETARG_DATUM(0),
												PointerGetDatum(nentries)));

		/* 不过 "contains {}" 需要全索引扫描 */
		if (*nentries == 0)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		JsonPath   *jp = PG_GETARG_JSONPATH_P(0);
		Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

		entries = extract_jsp_query(jp, strategy, true, nentries, extra_data);

		if (!entries)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else
	{
		elog(ERROR, "unrecognized strategy number: %d", strategy);
		entries = NULL;
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_consistent_jsonb_path(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * jsonb_path_ops 必然有损,不仅因哈希冲突,也因
		 * 不保留 JSON 结构完整信息。此外,原始标量
		 * 在数组中的包含有特殊规则此处未处理。故匹配
		 * 须始终重新检查。但若并非全部键都在,元组
		 * 肯定不匹配。
		 *
		 *
		 */
		*recheck = true;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		*recheck = true;

		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   false) != GIN_FALSE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}

Datum
gin_triconsistent_jsonb_path(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_MAYBE;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * 注意从不返回 GIN_TRUE,只返回 GIN_MAYBE 或
		 * GIN_FALSE;对应常规 consistent 中始终强制
		 * 重新检查,理由见该处所列。
		 */
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_FALSE)
			{
				res = GIN_FALSE;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   true);

			/* 应始终重新检查结果 */
			if (res == GIN_TRUE)
				res = GIN_MAYBE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 * 由标志字节与文本表示构造 jsonb_ops GIN 键
 * (不必 NUL 结尾)。本函数负责对超长文本表示
 * 做哈希;若如此则把 JGINFLAG_HASHED 位加进
 * 标志值上。
 */
static Datum
make_text_key(char flag, const char *str, int len)
{
	text	   *item;
	char		hashbuf[10];

	if (len > JGIN_MAXLENGTH)
	{
		uint32		hashval;

		hashval = DatumGetUInt32(hash_any((const unsigned char *) str, len));
		snprintf(hashbuf, sizeof(hashbuf), "%08x", hashval);
		str = hashbuf;
		len = 8;
		flag |= JGINFLAG_HASHED;
	}

	/*
	 * 现构造 text Datum。为简便此处建 4 字节头
	 * 的 varlena Datum,但期望存入索引时转成短
	 * 头格式。
	 */
	item = (text *) palloc(VARHDRSZ + len + 1);
	SET_VARSIZE(item, VARHDRSZ + len + 1);

	*VARDATA(item) = flag;

	memcpy(VARDATA(item) + 1, str, len);

	return PointerGetDatum(item);
}

/*
 * 为 JsonbValue 建文本表示,作 jsonb_ops 索引中
 * 的 GIN 键。若它是键,或是字符串数组元素
 * (因假装那些是键,见 jsonb.h),则 is_key 为
 * 真。
 */
static Datum
make_scalar_key(const JsonbValue *scalarVal, bool is_key)
{
	Datum		item;
	char	   *cstr;

	switch (scalarVal->type)
	{
		case jbvNull:
			Assert(!is_key);
			item = make_text_key(JGINFLAG_NULL, "", 0);
			break;
		case jbvBool:
			Assert(!is_key);
			item = make_text_key(JGINFLAG_BOOL,
								 scalarVal->val.boolean ? "t" : "f", 1);
			break;
		case jbvNumeric:
			Assert(!is_key);

			/*
			 * 需无尾随零的规范化文本表示,使数值相等
			 * 的值产生相等的字符串。
			 *
			 *
			 * 数值以较臃肿文本格式存放并不理想。但
			 * 这是在 GIN B-Tree 中存 "union" 类型的记法上
			 * 方便的方式,且索引 Jsonb 字符串优先。
			 *
			 */
			cstr = numeric_normalize(scalarVal->val.numeric);
			item = make_text_key(JGINFLAG_NUM, cstr, strlen(cstr));
			pfree(cstr);
			break;
		case jbvString:
			item = make_text_key(is_key ? JGINFLAG_KEY : JGINFLAG_STR,
								 scalarVal->val.string.val,
								 scalarVal->val.string.len);
			break;
		default:
			elog(ERROR, "unrecognized jsonb scalar type: %d", scalarVal->type);
			item = 0;			/* 让编译器安静 */
			break;
	}

	return item;
}
