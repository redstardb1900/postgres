/*-------------------------------------------------------------------------
 *
 * queryjumblefuncs.c
 *	 查询规范化与取指纹。
 *
 * 规范化把相似的查询认作等价，并作为单独一条来跟踪。这些查询通常只在
 * 常量上有所不同（不过确切规则比这
 * 更为微妙）。这对
 * 非预备查询特别有用。
 *
 * 规范化的做法是给查询取指纹：有选择地
 * 序列化每个查询树节点中那些被判定为
 * 对查询必不可少的字段。这称为查询混写。它
 * 不同于常规序列化，各种多余的
 * 信息被略去，视为与查询无关或并不重要，例如
 * Var 的排序规则，以及最显著的一点，常量的值。
 *
 * 每个查询在解析分析结束时取得这一混写，
 * 并将其 64 位散列存入该查询的 Query.queryId 字段。
 * 服务器随后把这个值传递下去，使它出现在由该查询
 * 生成的计划树中。执行器然后可以用这个值
 * 把查询代价记到正确的 queryId 上。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/queryjumblefuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "parser/scansup.h"

#define JUMBLE_SIZE				1024	/* 查询序列化缓冲区大小 */

/* GUC 参数 */
int			compute_query_id = COMPUTE_QUERY_ID_AUTO;

/* 当 compute_query_id 为 ON，或为 AUTO 且有模块请求它们时为 true */
bool		query_id_enabled = false;

static void AppendJumble(JumbleState *jstate,
						 const unsigned char *item, Size size);
static void RecordConstLocation(JumbleState *jstate, int location);
static void _jumbleNode(JumbleState *jstate, Node *node);
static void _jumbleA_Const(JumbleState *jstate, Node *node);
static void _jumbleList(JumbleState *jstate, Node *node);
static void _jumbleRangeTblEntry(JumbleState *jstate, Node *node);

/*
 * 给定一个可能含多条语句的源字符串，把注意力限制在
 * 该字符串的相关部分。
 */
const char *
CleanQuerytext(const char *query, int *location, int *len)
{
	int			query_location = *location;
	int			query_len = *len;

	/* 先应用起始偏移，除非它是 -1（未知）。 */
	if (query_location >= 0)
	{
		Assert(query_location <= strlen(query));
		query += query_location;
		/* 长度为 0（或 -1）表示 "字符串的剩余部分" */
		if (query_len <= 0)
			query_len = strlen(query);
		else
			Assert(query_len <= strlen(query));
	}
	else
	{
		/* 若查询位置未知，同样不要信任 query_len */
		query_location = 0;
		query_len = strlen(query);
	}

	/*
	 * 也丢弃前导和尾随空白。使用 scanner_isspace()
	 * 而不是 libc 的 isspace()，因为我们希望与词法分析器的行为一致。
	 */
	while (query_len > 0 && scanner_isspace(query[0]))
		query++, query_location++, query_len--;
	while (query_len > 0 && scanner_isspace(query[query_len - 1]))
		query_len--;

	*location = query_location;
	*len = query_len;

	return query;
}

JumbleState *
JumbleQuery(Query *query)
{
	JumbleState *jstate = NULL;

	Assert(IsQueryIdEnabled());

	jstate = (JumbleState *) palloc(sizeof(JumbleState));

	/* 为查询混写建立工作区 */
	jstate->jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate->jumble_len = 0;
	jstate->clocations_buf_size = 32;
	jstate->clocations = (LocationLen *)
		palloc(jstate->clocations_buf_size * sizeof(LocationLen));
	jstate->clocations_count = 0;
	jstate->highest_extern_param_id = 0;

	/* 计算查询 ID，并把它标记到 Query 节点上 */
	_jumbleNode(jstate, (Node *) query);
	query->queryId = DatumGetUInt64(hash_any_extended(jstate->jumble,
													  jstate->jumble_len,
													  0));

	/*
	 * 若不幸得到哈希值为 0，普通语句改用 1，
	 * 实用语句改用 2。
	 */
	if (query->queryId == UINT64CONST(0))
	{
		if (query->utilityStmt)
			query->queryId = UINT64CONST(2);
		else
			query->queryId = UINT64CONST(1);
	}

	return jstate;
}

/*
 * 启用查询标识符计算。
 *
 * 第三方插件可以用这个函数通知核心，它们需要
 * 计算查询标识符。
 */
void
EnableQueryId(void)
{
	if (compute_query_id != COMPUTE_QUERY_ID_OFF)
		query_id_enabled = true;
}

/*
 * AppendJumble：把给定查询中有实质意义的值追加到
 * 当前混写。
 */
static void
AppendJumble(JumbleState *jstate, const unsigned char *item, Size size)
{
	unsigned char *jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/*
	 * 每当混写缓冲区满了，我们就对当前内容做哈希，并
	 * 把缓冲区重置为只含该哈希值，从而依靠
	 * 哈希来概括到目前为止的一切。
	 */
	while (size > 0)
	{
		Size		part_size;

		if (jumble_len >= JUMBLE_SIZE)
		{
			uint64		start_hash;

			start_hash = DatumGetUInt64(hash_any_extended(jumble,
														  JUMBLE_SIZE, 0));
			memcpy(jumble, &start_hash, sizeof(start_hash));
			jumble_len = sizeof(start_hash);
		}
		part_size = Min(size, JUMBLE_SIZE - jumble_len);
		memcpy(jumble + jumble_len, item, part_size);
		jumble_len += part_size;
		item += part_size;
		size -= part_size;
	}
	jstate->jumble_len = jumble_len;
}

/*
 * 记录当前正在遍历的查询树的查询字符串中
 * 常量的位置。
 */
static void
RecordConstLocation(JumbleState *jstate, int location)
{
	/* -1 表示未知或未定义的位置 */
	if (location >= 0)
	{
		/* 必要时扩大数组 */
		if (jstate->clocations_count >= jstate->clocations_buf_size)
		{
			jstate->clocations_buf_size *= 2;
			jstate->clocations = (LocationLen *)
				repalloc(jstate->clocations,
						 jstate->clocations_buf_size *
						 sizeof(LocationLen));
		}
		jstate->clocations[jstate->clocations_count].location = location;
		/* 把长度初始化为 -1，以简化第三方模块的使用 */
		jstate->clocations[jstate->clocations_count].length = -1;
		jstate->clocations_count++;
	}
}

#define JUMBLE_NODE(item) \
	_jumbleNode(jstate, (Node *) expr->item)
#define JUMBLE_LOCATION(location) \
	RecordConstLocation(jstate, expr->location)
#define JUMBLE_FIELD(item) \
	AppendJumble(jstate, (const unsigned char *) &(expr->item), sizeof(expr->item))
#define JUMBLE_FIELD_SINGLE(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define JUMBLE_STRING(str) \
do { \
	if (expr->str) \
		AppendJumble(jstate, (const unsigned char *) (expr->str), strlen(expr->str) + 1); \
} while(0)

#include "queryjumblefuncs.funcs.c"

static void
_jumbleNode(JumbleState *jstate, Node *node)
{
	Node	   *expr = node;

	if (expr == NULL)
		return;

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	/*
	 * 我们总是先发出节点的 NodeTag，然后是被认为重要的任何附加字段，
	 * 再递归到任何子节点。
	 */
	JUMBLE_FIELD(type);

	switch (nodeTag(expr))
	{
#include "queryjumblefuncs.switch.c"

		case T_List:
		case T_IntList:
		case T_OidList:
		case T_XidList:
			_jumbleList(jstate, expr);
			break;

		default:
			/* 只是警告，因为我们反正可以勉强继续 */
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(expr));
			break;
	}

	/* 要在自动生成的代码之外处理的特殊情况 */
	switch (nodeTag(expr))
	{
		case T_Param:
			{
				Param	   *p = (Param *) node;

				/*
				 * 更新见过的最高 Param id，以便
				 * 正确地开始规范化。
				 */
				if (p->paramkind == PARAM_EXTERN &&
					p->paramid > jstate->highest_extern_param_id)
					jstate->highest_extern_param_id = p->paramid;
			}
			break;
		default:
			break;
	}
}

static void
_jumbleList(JumbleState *jstate, Node *node)
{
	List	   *expr = (List *) node;
	ListCell   *l;

	switch (expr->type)
	{
		case T_List:
			foreach(l, expr)
				_jumbleNode(jstate, lfirst(l));
			break;
		case T_IntList:
			foreach(l, expr)
				JUMBLE_FIELD_SINGLE(lfirst_int(l));
			break;
		case T_OidList:
			foreach(l, expr)
				JUMBLE_FIELD_SINGLE(lfirst_oid(l));
			break;
		case T_XidList:
			foreach(l, expr)
				JUMBLE_FIELD_SINGLE(lfirst_xid(l));
			break;
		default:
			elog(ERROR, "unrecognized list node type: %d",
				 (int) expr->type);
			return;
	}
}

static void
_jumbleA_Const(JumbleState *jstate, Node *node)
{
	A_Const    *expr = (A_Const *) node;

	JUMBLE_FIELD(isnull);
	if (!expr->isnull)
	{
		JUMBLE_FIELD(val.node.type);
		switch (nodeTag(&expr->val))
		{
			case T_Integer:
				JUMBLE_FIELD(val.ival.ival);
				break;
			case T_Float:
				JUMBLE_STRING(val.fval.fval);
				break;
			case T_Boolean:
				JUMBLE_FIELD(val.boolval.boolval);
				break;
			case T_String:
				JUMBLE_STRING(val.sval.sval);
				break;
			case T_BitString:
				JUMBLE_STRING(val.bsval.bsval);
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&expr->val));
				break;
		}
	}
}

static void
_jumbleRangeTblEntry(JumbleState *jstate, Node *node)
{
	RangeTblEntry *expr = (RangeTblEntry *) node;

	JUMBLE_FIELD(rtekind);
	switch (expr->rtekind)
	{
		case RTE_RELATION:
			JUMBLE_FIELD(relid);
			JUMBLE_NODE(tablesample);
			JUMBLE_FIELD(inh);
			break;
		case RTE_SUBQUERY:
			JUMBLE_NODE(subquery);
			break;
		case RTE_JOIN:
			JUMBLE_FIELD(jointype);
			break;
		case RTE_FUNCTION:
			JUMBLE_NODE(functions);
			break;
		case RTE_TABLEFUNC:
			JUMBLE_NODE(tablefunc);
			break;
		case RTE_VALUES:
			JUMBLE_NODE(values_lists);
			break;
		case RTE_CTE:

			/*
			 * 在这里依赖 CTE 名字并不理想，但这是我们
			 * 用来识别所引用 WITH 项的唯一信息。
			 */
			JUMBLE_STRING(ctename);
			JUMBLE_FIELD(ctelevelsup);
			break;
		case RTE_NAMEDTUPLESTORE:
			JUMBLE_STRING(enrname);
			break;
		case RTE_RESULT:
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) expr->rtekind);
			break;
	}
}
