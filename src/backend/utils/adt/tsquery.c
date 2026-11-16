/*-------------------------------------------------------------------------
 *
 * tsquery.c
 *	  tsquery 的 I/O 函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/pg_crc.h"
#include "varatt.h"

/* FTS 算子优先级，见 ts_type.h */
const int	tsearch_op_priority[OP_COUNT] =
{
	4,							/* OP_NOT */
	2,							/* OP_AND */
	1,							/* OP_OR */
	3							/* OP_PHRASE */
};

/*
 * 解析器状态
 */
typedef enum
{
	WAITOPERAND = 1,
	WAITOPERATOR = 2,
	WAITFIRSTOPERAND = 3
} ts_parserstate;

/*
 * 解析用词位类型
 */
typedef enum
{
	PT_END = 0,
	PT_ERR = 1,
	PT_VAL = 2,
	PT_OPR = 3,
	PT_OPEN = 4,
	PT_CLOSE = 5
} ts_tokentype;

/*
 * 从查询串获取词位
 *
 * 除 "state" 外所有参数均为输出参数。
 *
 * 若返回值为 PT_OPR，则 *operator 会被填入 OP_* 代码，
 * 且在短语算子的情形下 *weight 会包含一个距离值。
 *
 * 若返回值为 PT_VAL，则填充 *lenval、*strval、*weight 和
 * *prefix
 *
 * 若返回 PT_ERR 则表明发生了软错误。若 state->escontext
 * 尚未被填充，则这应当作为一般解析错误上报。
 */
typedef ts_tokentype (*ts_tokenizer) (TSQueryParserState state, int8 *operator,
									  int *lenval, char **strval,
									  int16 *weight, bool *prefix);

struct TSQueryParserStateData
{
	/* 用于解析 tsquery 的分词器 */
	ts_tokenizer gettoken;

	/* 分词器函数的状态 */
	char	   *buffer;			/* 正扫描的整个字符串 */
	char	   *buf;			/* 当前扫描点 */
	int			count;			/* 嵌套计数，由 ( 递增，
								 * 由 ) 递减 */
	ts_parserstate state;

	/* 列表中的 polish 表示法，由 push* 函数填充 */
	List	   *polstr;

	/*
	 * 操作数的串收集在 op 中，curop 指向 op 已用空间
	 * 的末尾。
	 */
	char	   *op;
	char	   *curop;
	int			lenop;			/* op 的分配大小 */
	int			sumlen;			/* op 已用大小 */

	/* 值解析器的状态 */
	TSVectorParseState valstate;

	/* 软错误上下文对象 —— 须匹配 valstate 的 escontext */
	Node	   *escontext;
};

/*
 * 解析修饰符部分（权重与前缀标志）的子例程，如
 * 查询的 ":AB*" 部分。
 */
static char *
get_modifiers(char *buf, int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	if (!t_iseq(buf, ':'))
		return buf;

	buf++;
	while (*buf && pg_mblen(buf) == 1)
	{
		switch (*buf)
		{
			case 'a':
			case 'A':
				*weight |= 1 << 3;
				break;
			case 'b':
			case 'B':
				*weight |= 1 << 2;
				break;
			case 'c':
			case 'C':
				*weight |= 1 << 1;
				break;
			case 'd':
			case 'D':
				*weight |= 1;
				break;
			case '*':
				*prefix = true;
				break;
			default:
				return buf;
		}
		buf++;
	}

	return buf;
}

/*
 * 解析短语算子。该算子
 * 可能采取下列形式：
 *
 *		a <N> b（距离恰为 N 个词位）
 *		a <-> b（默认距离 = 1）
 *
 * 缓冲区应以 "<" 字符开始
 */
static bool
parse_phrase_operator(TSQueryParserState pstate, int16 *distance)
{
	enum
	{
		PHRASE_OPEN = 0,
		PHRASE_DIST,
		PHRASE_CLOSE,
		PHRASE_FINISH
	}			state = PHRASE_OPEN;
	char	   *ptr = pstate->buf;
	char	   *endptr;
	long		l = 1;			/* 默认距离 */

	while (*ptr)
	{
		switch (state)
		{
			case PHRASE_OPEN:
				if (t_iseq(ptr, '<'))
				{
					state = PHRASE_DIST;
					ptr++;
				}
				else
					return false;
				break;

			case PHRASE_DIST:
				if (t_iseq(ptr, '-'))
				{
					state = PHRASE_CLOSE;
					ptr++;
					continue;
				}

				if (!t_isdigit(ptr))
					return false;

				errno = 0;
				l = strtol(ptr, &endptr, 10);
				if (ptr == endptr)
					return false;
				else if (errno == ERANGE || l < 0 || l > MAXENTRYPOS)
					ereturn(pstate->escontext, false,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("distance in phrase operator must be an integer value between zero and %d inclusive",
									MAXENTRYPOS)));
				else
				{
					state = PHRASE_CLOSE;
					ptr = endptr;
				}
				break;

			case PHRASE_CLOSE:
				if (t_iseq(ptr, '>'))
				{
					state = PHRASE_FINISH;
					ptr++;
				}
				else
					return false;
				break;

			case PHRASE_FINISH:
				*distance = (int16) l;
				pstate->buf = ptr;
				return true;
		}
	}

	return false;
}

/*
 * 解析 websearch_to_tsquery() 所用的 OR 算子，如果我们
 * 认为 "OR" 字面量是算子 OR 则返回 true
 */
static bool
parse_or_operator(TSQueryParserState pstate)
{
	char	   *ptr = pstate->buf;

	/* 它应以 "OR" 字面量开始 */
	if (pg_strncasecmp(ptr, "or", 2) != 0)
		return false;

	ptr += 2;

	/*
	 * 它不应是任何词的一部分，但稍后必须出现
	 * 操作数
	 */
	if (*ptr == '\0')			/* no operand */
		return false;

	/* 它不应是任何词的一部分 */
	if (t_iseq(ptr, '-') || t_iseq(ptr, '_') || t_isalnum(ptr))
		return false;

	for (;;)
	{
		ptr += pg_mblen(ptr);

		if (*ptr == '\0')		/* 串结束且无操作数 */
			return false;

		/*
		 * 假定我们找到了操作数，但它可能不正确。
		 * 所以我们仍把 OR 字面量当作算子，即使其操作数
		 * 可能有误，也不当词位检索
		 */
		if (!t_isspace(ptr))
			break;
	}

	pstate->buf += 2;
	return true;
}

static ts_tokentype
gettoken_query_standard(TSQueryParserState state, int8 *operator,
						int *lenval, char **strval,
						int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	while (true)
	{
		switch (state->state)
		{
			case WAITFIRSTOPERAND:
			case WAITOPERAND:
				if (t_iseq(state->buf, '!'))
				{
					state->buf++;
					state->state = WAITOPERAND;
					*operator = OP_NOT;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '('))
				{
					state->buf++;
					state->state = WAITOPERAND;
					state->count++;
					return PT_OPEN;
				}
				else if (t_iseq(state->buf, ':'))
				{
					/* 通用语法错误消息即可 */
					return PT_ERR;
				}
				else if (!t_isspace(state->buf))
				{
					/*
					 * 我们依靠 tsvector 解析器来解析该值
					 *
					 */
					reset_tsvector_parser(state->valstate, state->buf);
					if (gettoken_tsvector(state->valstate, strval, lenval,
										  NULL, NULL, &state->buf))
					{
						state->buf = get_modifiers(state->buf, weight, prefix);
						state->state = WAITOPERATOR;
						return PT_VAL;
					}
					else if (SOFT_ERROR_OCCURRED(state->escontext))
					{
						/* gettoken_tsvector 报告了软错误 */
						return PT_ERR;
					}
					else if (state->state == WAITFIRSTOPERAND)
					{
						return PT_END;
					}
					else
						ereturn(state->escontext, PT_ERR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("no operand in tsquery: \"%s\"",
										state->buffer)));
				}
				break;

			case WAITOPERATOR:
				if (t_iseq(state->buf, '&'))
				{
					state->buf++;
					state->state = WAITOPERAND;
					*operator = OP_AND;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '|'))
				{
					state->buf++;
					state->state = WAITOPERAND;
					*operator = OP_OR;
					return PT_OPR;
				}
				else if (parse_phrase_operator(state, weight))
				{
					/* weight 变量用作距离存储 */
					state->state = WAITOPERAND;
					*operator = OP_PHRASE;
					return PT_OPR;
				}
				else if (SOFT_ERROR_OCCURRED(state->escontext))
				{
					/* parse_phrase_operator 报告了软错误 */
					return PT_ERR;
				}
				else if (t_iseq(state->buf, ')'))
				{
					state->buf++;
					state->count--;
					return (state->count < 0) ? PT_ERR : PT_CLOSE;
				}
				else if (*state->buf == '\0')
				{
					return (state->count) ? PT_ERR : PT_END;
				}
				else if (!t_isspace(state->buf))
				{
					return PT_ERR;
				}
				break;
		}

		state->buf += pg_mblen(state->buf);
	}
}

static ts_tokentype
gettoken_query_websearch(TSQueryParserState state, int8 *operator,
						 int *lenval, char **strval,
						 int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	while (true)
	{
		switch (state->state)
		{
			case WAITFIRSTOPERAND:
			case WAITOPERAND:
				if (t_iseq(state->buf, '-'))
				{
					state->buf++;
					state->state = WAITOPERAND;

					*operator = OP_NOT;
					return PT_OPR;
				}
				else if (t_iseq(state->buf, '"'))
				{
					/* 引号内的一切都作为单个词位处理 */

					/* 跳过起始引号 */
					state->buf++;
					*strval = state->buf;

					/* 迭代到结束引号或字符串末尾 */
					while (*state->buf != '\0' && !t_iseq(state->buf, '"'))
						state->buf++;
					*lenval = state->buf - *strval;

					/* 若未到串尾则跳过结束引号 */
					if (*state->buf != '\0')
						state->buf++;

					state->state = WAITOPERATOR;
					state->count++;
					return PT_VAL;
				}
				else if (ISOPERATOR(state->buf))
				{
					/* 忽略之，否则 gettoken_tsvector() 会报错 */
					state->buf++;
					state->state = WAITOPERAND;
					continue;
				}
				else if (!t_isspace(state->buf))
				{
					/*
					 * 我们依靠 tsvector 解析器来解析该值
					 *
					 */
					reset_tsvector_parser(state->valstate, state->buf);
					if (gettoken_tsvector(state->valstate, strval, lenval,
										  NULL, NULL, &state->buf))
					{
						state->state = WAITOPERATOR;
						return PT_VAL;
					}
					else if (SOFT_ERROR_OCCURRED(state->escontext))
					{
						/* gettoken_tsvector 报告了软错误 */
						return PT_ERR;
					}
					else if (state->state == WAITFIRSTOPERAND)
					{
						return PT_END;
					}
					else
					{
						/* 最终必须提供一个操作数 */
						pushStop(state);
						return PT_END;
					}
				}
				break;

			case WAITOPERATOR:
				if (*state->buf == '\0')
				{
					return PT_END;
				}
				else if (parse_or_operator(state))
				{
					state->state = WAITOPERAND;
					*operator = OP_OR;
					return PT_OPR;
				}
				else if (ISOPERATOR(state->buf))
				{
					/* 在此状态下同样忽略其他算子 */
					state->buf++;
					continue;
				}
				else if (!t_isspace(state->buf))
				{
					/* 操作数之间插入隐式 AND */
					state->state = WAITOPERAND;
					*operator = OP_AND;
					return PT_OPR;
				}
				break;
		}

		state->buf += pg_mblen(state->buf);
	}
}

static ts_tokentype
gettoken_query_plain(TSQueryParserState state, int8 *operator,
					 int *lenval, char **strval,
					 int16 *weight, bool *prefix)
{
	*weight = 0;
	*prefix = false;

	if (*state->buf == '\0')
		return PT_END;

	*strval = state->buf;
	*lenval = strlen(state->buf);
	state->buf += *lenval;
	state->count++;
	return PT_VAL;
}

/*
 * 将算子压入 state->polstr
 */
void
pushOperator(TSQueryParserState state, int8 oper, int16 distance)
{
	QueryOperator *tmp;

	Assert(oper == OP_NOT || oper == OP_AND || oper == OP_OR || oper == OP_PHRASE);

	tmp = (QueryOperator *) palloc0(sizeof(QueryOperator));
	tmp->type = QI_OPR;
	tmp->oper = oper;
	tmp->distance = (oper == OP_PHRASE) ? distance : 0;
	/* left 稍后由 findoprnd 填充 */

	state->polstr = lcons(tmp, state->polstr);
}

static void
pushValue_internal(TSQueryParserState state, pg_crc32 valcrc, int distance, int lenval, int weight, bool prefix)
{
	QueryOperand *tmp;

	if (distance >= MAXSTRPOS)
		ereturn(state->escontext,,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("value is too big in tsquery: \"%s\"",
						state->buffer)));
	if (lenval >= MAXSTRLEN)
		ereturn(state->escontext,,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("operand is too long in tsquery: \"%s\"",
						state->buffer)));

	tmp = (QueryOperand *) palloc0(sizeof(QueryOperand));
	tmp->type = QI_VAL;
	tmp->weight = weight;
	tmp->prefix = prefix;
	tmp->valcrc = (int32) valcrc;
	tmp->length = lenval;
	tmp->distance = distance;

	state->polstr = lcons(tmp, state->polstr);
}

/*
 * 把操作数压入 state->polstr
 *
 * strval 必须指向等于 state->curop 的字符串。lenval 为
 * 其长度。
 */
void
pushValue(TSQueryParserState state, char *strval, int lenval, int16 weight, bool prefix)
{
	pg_crc32	valcrc;

	if (lenval >= MAXSTRLEN)
		ereturn(state->escontext,,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("word is too long in tsquery: \"%s\"",
						state->buffer)));

	INIT_LEGACY_CRC32(valcrc);
	COMP_LEGACY_CRC32(valcrc, strval, lenval);
	FIN_LEGACY_CRC32(valcrc);
	pushValue_internal(state, valcrc, state->curop - state->op, lenval, weight, prefix);

	/* 把值字符串追加到 state.op，必要时先扩大缓冲区 */
	while (state->curop - state->op + lenval + 1 >= state->lenop)
	{
		int			used = state->curop - state->op;

		state->lenop *= 2;
		state->op = (char *) repalloc(state->op, state->lenop);
		state->curop = state->op + used;
	}
	memcpy(state->curop, strval, lenval);
	state->curop += lenval;
	*(state->curop) = '\0';
	state->curop++;
	state->sumlen += lenval + 1 /* \0 */ ;
}


/*
 * 将停用词占位符压入 state->polstr
 */
void
pushStop(TSQueryParserState state)
{
	QueryOperand *tmp;

	tmp = (QueryOperand *) palloc0(sizeof(QueryOperand));
	tmp->type = QI_VALSTOP;

	state->polstr = lcons(tmp, state->polstr);
}


#define STACKDEPTH	32

typedef struct OperatorElement
{
	int8		op;
	int16		distance;
} OperatorElement;

static void
pushOpStack(OperatorElement *stack, int *lenstack, int8 op, int16 distance)
{
	if (*lenstack == STACKDEPTH)	/* 内部错误 */
		elog(ERROR, "tsquery stack too small");

	stack[*lenstack].op = op;
	stack[*lenstack].distance = distance;

	(*lenstack)++;
}

static void
cleanOpStack(TSQueryParserState state,
			 OperatorElement *stack, int *lenstack, int8 op)
{
	int			opPriority = OP_PRIORITY(op);

	while (*lenstack)
	{
		/* NOT 右结合，不同于其他算子 */
		if ((op != OP_NOT && opPriority > OP_PRIORITY(stack[*lenstack - 1].op)) ||
			(op == OP_NOT && opPriority >= OP_PRIORITY(stack[*lenstack - 1].op)))
			break;

		(*lenstack)--;
		pushOperator(state, stack[*lenstack].op,
					 stack[*lenstack].distance);
	}
}

/*
 * 构造查询的 polish 表示法。
 *
 * pushval 的说明见 parse_tsquery。
 */
static void
makepol(TSQueryParserState state,
		PushFunction pushval,
		Datum opaque)
{
	int8		operator = 0;
	ts_tokentype type;
	int			lenval = 0;
	char	   *strval = NULL;
	OperatorElement opstack[STACKDEPTH];
	int			lenstack = 0;
	int16		weight = 0;
	bool		prefix;

	/* 本函数会递归，因此可能被驱动到栈溢出 */
	check_stack_depth();

	while ((type = state->gettoken(state, &operator,
								   &lenval, &strval,
								   &weight, &prefix)) != PT_END)
	{
		switch (type)
		{
			case PT_VAL:
				pushval(opaque, state, strval, lenval, weight, prefix);
				break;
			case PT_OPR:
				cleanOpStack(state, opstack, &lenstack, operator);
				pushOpStack(opstack, &lenstack, operator, weight);
				break;
			case PT_OPEN:
				makepol(state, pushval, opaque);
				break;
			case PT_CLOSE:
				cleanOpStack(state, opstack, &lenstack, OP_OR /* 最低 */ );
				return;
			case PT_ERR:
			default:
				/* 不覆盖 gettoken 函数已保存的软错误 */
				if (!SOFT_ERROR_OCCURRED(state->escontext))
					errsave(state->escontext,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("syntax error in tsquery: \"%s\"",
									state->buffer)));
				return;
		}
		/* 检测 pushval 或递归中的软错误 */
		if (SOFT_ERROR_OCCURRED(state->escontext))
			return;
	}

	cleanOpStack(state, opstack, &lenstack, OP_OR /* 最低 */ );
}

static void
findoprnd_recurse(QueryItem *ptr, uint32 *pos, int nnodes, bool *needcleanup)
{
	/* 本函数会递归，因此可能被驱动到栈溢出。 */
	check_stack_depth();

	if (*pos >= nnodes)
		elog(ERROR, "malformed tsquery: operand not found");

	if (ptr[*pos].type == QI_VAL)
	{
		(*pos)++;
	}
	else if (ptr[*pos].type == QI_VALSTOP)
	{
		*needcleanup = true;	/* 将不得不移除停用词 */
		(*pos)++;
	}
	else
	{
		Assert(ptr[*pos].type == QI_OPR);

		if (ptr[*pos].qoperator.oper == OP_NOT)
		{
			ptr[*pos].qoperator.left = 1;	/* 固定偏移 */
			(*pos)++;

			/* 处理唯一的参数 */
			findoprnd_recurse(ptr, pos, nnodes, needcleanup);
		}
		else
		{
			QueryOperator *curitem = &ptr[*pos].qoperator;
			int			tmp = *pos; /* 保存当前位置 */

			Assert(curitem->oper == OP_AND ||
				   curitem->oper == OP_OR ||
				   curitem->oper == OP_PHRASE);

			(*pos)++;

			/* 处理 RIGHT 参数 */
			findoprnd_recurse(ptr, pos, nnodes, needcleanup);

			curitem->left = *pos - tmp; /* 设左参数偏移 */

			/* 处理 LEFT 参数 */
			findoprnd_recurse(ptr, pos, nnodes, needcleanup);
		}
	}
}


/*
 * 填充先前未填充的 left 字段。
 * 输入 QueryItems 必须为 polish 表示法。
 * 若有 QI_VALSTOP 节点，则把 *needcleanup 设为 true。
 */
static void
findoprnd(QueryItem *ptr, int size, bool *needcleanup)
{
	uint32		pos;

	*needcleanup = false;
	pos = 0;
	findoprnd_recurse(ptr, &pos, size, needcleanup);

	if (pos != size)
		elog(ERROR, "malformed tsquery: extra nodes");
}


/*
 * 解析 "buf" 中的 tsquery。
 *
 * 查询中的每个值（操作数）都传给 pushval。pushval
 * 用 pushValue 和 pushOperator 把简单值变换成任意复杂
 * 的表达式。它必须用 pushValue 压入单个值，或压入
 * 全部操作数的完整表达式，或用 pushStop 压入停用
 * 词占位符，否则前缀表示法将被破坏，产生没有操作数
 * 的算子。
 *
 * opaque 原样传给 pushval，pushval 可以用它来存储
 * 私有状态
 *
 * pushval 函数可以通过 escontext 记录软错误。
 * 调用者须检查 SOFT_ERROR_OCCURRED 来检测它。
 *
 * 标志掩码（见 ts_utils.h）与错误上下文对象均可
 * 提供。若发生软错误，则返回 NULL。
 */
TSQuery
parse_tsquery(char *buf,
			  PushFunction pushval,
			  Datum opaque,
			  int flags,
			  Node *escontext)
{
	struct TSQueryParserStateData state;
	int			i;
	TSQuery		query;
	int			commonlen;
	QueryItem  *ptr;
	ListCell   *cell;
	bool		noisy;
	bool		needcleanup;
	int			tsv_flags = P_TSV_OPR_IS_DELIM | P_TSV_IS_TSQUERY;

	/* plain 不应与 web 一起使用 */
	Assert((flags & (P_TSQ_PLAIN | P_TSQ_WEB)) != (P_TSQ_PLAIN | P_TSQ_WEB));

	/* 选择合适的分词器 */
	if (flags & P_TSQ_PLAIN)
		state.gettoken = gettoken_query_plain;
	else if (flags & P_TSQ_WEB)
	{
		state.gettoken = gettoken_query_websearch;
		tsv_flags |= P_TSV_IS_WEB;
	}
	else
		state.gettoken = gettoken_query_standard;

	/* 仅在不做软错误时才发出扰人的 NOTICE */
	noisy = !(escontext && IsA(escontext, ErrorSaveContext));

	/* 初始化 */
	state.buffer = buf;
	state.buf = buf;
	state.count = 0;
	state.state = WAITFIRSTOPERAND;
	state.polstr = NIL;
	state.escontext = escontext;

	/* 初始值解析器状态 */
	state.valstate = init_tsvector_parser(state.buffer, tsv_flags, escontext);

	/* 初始操作数表 */
	state.sumlen = 0;
	state.lenop = 64;
	state.curop = state.op = (char *) palloc(state.lenop);
	*(state.curop) = '\0';

	/* 解析查询并构造 polish 表示法（后缀式，但反序） */
	makepol(&state, pushval, opaque);

	close_tsvector_parser(state.valstate);

	if (SOFT_ERROR_OCCURRED(escontext))
		return NULL;

	if (state.polstr == NIL)
	{
		if (noisy)
			ereport(NOTICE,
					(errmsg("text-search query doesn't contain lexemes: \"%s\"",
							state.buffer)));
		query = (TSQuery) palloc(HDRSIZETQ);
		SET_VARSIZE(query, HDRSIZETQ);
		query->size = 0;
		return query;
	}

	if (TSQUERY_TOO_BIG(list_length(state.polstr), state.sumlen))
		ereturn(escontext, NULL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("tsquery is too large")));
	commonlen = COMPUTESIZE(list_length(state.polstr), state.sumlen);

	/* QueryItems 打包进最终 TSQuery 结构返回给调用者 */
	query = (TSQuery) palloc0(commonlen);
	SET_VARSIZE(query, commonlen);
	query->size = list_length(state.polstr);
	ptr = GETQUERY(query);

	/* QueryItems 拷入 TSQuery */
	i = 0;
	foreach(cell, state.polstr)
	{
		QueryItem  *item = (QueryItem *) lfirst(cell);

		switch (item->type)
		{
			case QI_VAL:
				memcpy(&ptr[i], item, sizeof(QueryOperand));
				break;
			case QI_VALSTOP:
				ptr[i].type = QI_VALSTOP;
				break;
			case QI_OPR:
				memcpy(&ptr[i], item, sizeof(QueryOperator));
				break;
			default:
				elog(ERROR, "unrecognized QueryItem type: %d", item->type);
		}
		i++;
	}

	/* 把全部操作数串复制到 TSQuery */
	memcpy(GETOPERAND(query), state.op, state.sumlen);
	pfree(state.op);

	/*
	 * 为每个算子设置左操作数指针。顺带，
	 * 检测是否存在 QI_VALSTOP 节点。
	 */
	findoprnd(ptr, query->size, &needcleanup);

	/*
	 * 若有 QI_VALSTOP 节点，则删除它们并化简树。
	 */
	if (needcleanup)
		query = cleanup_tsquery_stopwords(query, noisy);

	return query;
}

static void
pushval_asis(Datum opaque, TSQueryParserState state, char *strval, int lenval,
			 int16 weight, bool prefix)
{
	pushValue(state, strval, lenval, weight, prefix);
}

/*
 * 无词形变化的 in
 */
Datum
tsqueryin(PG_FUNCTION_ARGS)
{
	char	   *in = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;

	PG_RETURN_TSQUERY(parse_tsquery(in,
									pushval_asis,
									PointerGetDatum(NULL),
									0,
									escontext));
}

/*
 * out 函数
 */
typedef struct
{
	QueryItem  *curpol;
	char	   *buf;
	char	   *cur;
	char	   *op;
	int			buflen;
} INFIX;

/* 确保 inf->buf 足以再容纳 "addsize" 个字节 */
#define RESIZEBUF(inf, addsize) \
while( ( (inf)->cur - (inf)->buf ) + (addsize) + 1 >= (inf)->buflen ) \
{ \
	int len = (inf)->cur - (inf)->buf; \
	(inf)->buflen *= 2; \
	(inf)->buf = (char*) repalloc( (void*)(inf)->buf, (inf)->buflen ); \
	(inf)->cur = (inf)->buf + len; \
}

/*
 * 递归遍历该树并
 * 以中缀（人类可读）形式打印
 */
static void
infix(INFIX *in, int parentPriority, bool rightPhraseOp)
{
	/* 本函数会递归，因此可能被驱动到栈溢出。 */
	check_stack_depth();

	if (in->curpol->type == QI_VAL)
	{
		QueryOperand *curpol = &in->curpol->qoperand;
		char	   *op = in->op + curpol->distance;
		int			clen;

		RESIZEBUF(in, curpol->length * (pg_database_encoding_max_length() + 1) + 2 + 6);
		*(in->cur) = '\'';
		in->cur++;
		while (*op)
		{
			if (t_iseq(op, '\''))
			{
				*(in->cur) = '\'';
				in->cur++;
			}
			else if (t_iseq(op, '\\'))
			{
				*(in->cur) = '\\';
				in->cur++;
			}
			COPYCHAR(in->cur, op);

			clen = pg_mblen(op);
			op += clen;
			in->cur += clen;
		}
		*(in->cur) = '\'';
		in->cur++;
		if (curpol->weight || curpol->prefix)
		{
			*(in->cur) = ':';
			in->cur++;
			if (curpol->prefix)
			{
				*(in->cur) = '*';
				in->cur++;
			}
			if (curpol->weight & (1 << 3))
			{
				*(in->cur) = 'A';
				in->cur++;
			}
			if (curpol->weight & (1 << 2))
			{
				*(in->cur) = 'B';
				in->cur++;
			}
			if (curpol->weight & (1 << 1))
			{
				*(in->cur) = 'C';
				in->cur++;
			}
			if (curpol->weight & 1)
			{
				*(in->cur) = 'D';
				in->cur++;
			}
		}
		*(in->cur) = '\0';
		in->curpol++;
	}
	else if (in->curpol->qoperator.oper == OP_NOT)
	{
		int			priority = QO_PRIORITY(in->curpol);

		if (priority < parentPriority)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}
		RESIZEBUF(in, 1);
		*(in->cur) = '!';
		in->cur++;
		*(in->cur) = '\0';
		in->curpol++;

		infix(in, priority, false);
		if (priority < parentPriority)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
	else
	{
		int8		op = in->curpol->qoperator.oper;
		int			priority = QO_PRIORITY(in->curpol);
		int16		distance = in->curpol->qoperator.distance;
		INFIX		nrm;
		bool		needParenthesis = false;

		in->curpol++;
		if (priority < parentPriority ||
		/* 短语算子依赖顺序 */
			(op == OP_PHRASE && rightPhraseOp))
		{
			needParenthesis = true;
			RESIZEBUF(in, 2);
			sprintf(in->cur, "( ");
			in->cur = strchr(in->cur, '\0');
		}

		nrm.curpol = in->curpol;
		nrm.op = in->op;
		nrm.buflen = 16;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);

		/* 取右操作数 */
		infix(&nrm, priority, (op == OP_PHRASE));

		/* 取并打印左操作数 */
		in->curpol = nrm.curpol;
		infix(in, priority, false);

		/* 打印算子与右操作数 */
		RESIZEBUF(in, 3 + (2 + 10 /* 距离 */ ) + (nrm.cur - nrm.buf));
		switch (op)
		{
			case OP_OR:
				sprintf(in->cur, " | %s", nrm.buf);
				break;
			case OP_AND:
				sprintf(in->cur, " & %s", nrm.buf);
				break;
			case OP_PHRASE:
				if (distance != 1)
					sprintf(in->cur, " <%d> %s", distance, nrm.buf);
				else
					sprintf(in->cur, " <-> %s", nrm.buf);
				break;
			default:
				/* OP_NOT 已于上方 if 分支处理 */
				elog(ERROR, "unrecognized operator type: %d", op);
		}
		in->cur = strchr(in->cur, '\0');
		pfree(nrm.buf);

		if (needParenthesis)
		{
			RESIZEBUF(in, 2);
			sprintf(in->cur, " )");
			in->cur = strchr(in->cur, '\0');
		}
	}
}

Datum
tsqueryout(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	INFIX		nrm;

	if (query->size == 0)
	{
		char	   *b = palloc(1);

		*b = '\0';
		PG_RETURN_POINTER(b);
	}
	nrm.curpol = GETQUERY(query);
	nrm.buflen = 32;
	nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
	*(nrm.cur) = '\0';
	nrm.op = GETOPERAND(query);
	infix(&nrm, -1 /* 最低优先级 */ , false);

	PG_FREE_IF_COPY(query, 0);
	PG_RETURN_CSTRING(nrm.buf);
}

/*
 * 二进制输入/输出函数。二进制格式如下：
 *
 * uint32	查询中算子/操作数的数量
 *
 * 随后是算子和操作数，按前缀表示法。对各
 * operand:
 *
 * uint8	类型QI_VAL
 * uint8	权重
 * uint8	前缀
 *			操作数文本，客户端编码，null 结尾
 *
 * 对每个算子：
 *
 * uint8	类型QI_OPR
 * uint8	算子：OP_AND, OP_PHRASE, OP_OR, OP_NOT 之一
 * uint16	距离（仅 OP_PHRASE）
 */
Datum
tsquerysend(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	StringInfoData buf;
	int			i;
	QueryItem  *item = GETQUERY(query);

	pq_begintypsend(&buf);

	pq_sendint32(&buf, query->size);
	for (i = 0; i < query->size; i++)
	{
		pq_sendint8(&buf, item->type);

		switch (item->type)
		{
			case QI_VAL:
				pq_sendint8(&buf, item->qoperand.weight);
				pq_sendint8(&buf, item->qoperand.prefix);
				pq_sendstring(&buf, GETOPERAND(query) + item->qoperand.distance);
				break;
			case QI_OPR:
				pq_sendint8(&buf, item->qoperator.oper);
				if (item->qoperator.oper == OP_PHRASE)
					pq_sendint16(&buf, item->qoperator.distance);
				break;
			default:
				elog(ERROR, "unrecognized tsquery node type: %d", item->type);
		}
		item++;
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

Datum
tsqueryrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TSQuery		query;
	int			i,
				len;
	QueryItem  *item;
	int			datalen;
	char	   *ptr;
	uint32		size;
	const char **operands;
	bool		needcleanup;

	size = pq_getmsgint(buf, sizeof(uint32));
	if (size > (MaxAllocSize / sizeof(QueryItem)))
		elog(ERROR, "invalid size of tsquery");

	/* 分配空间来临时保存操作数串 */
	operands = palloc(size * sizeof(char *));

	/* 为所有 QueryItems 分配空间。 */
	len = HDRSIZETQ + sizeof(QueryItem) * size;
	query = (TSQuery) palloc0(len);
	query->size = size;
	item = GETQUERY(query);

	datalen = 0;
	for (i = 0; i < size; i++)
	{
		item->type = (int8) pq_getmsgint(buf, sizeof(int8));

		if (item->type == QI_VAL)
		{
			size_t		val_len;	/* 重编码为服务器编码后
									 * 长度 */
			uint8		weight;
			uint8		prefix;
			const char *val;
			pg_crc32	valcrc;

			weight = (uint8) pq_getmsgint(buf, sizeof(uint8));
			prefix = (uint8) pq_getmsgint(buf, sizeof(uint8));
			val = pq_getmsgstring(buf);
			val_len = strlen(val);

			/* 健全检查 */

			if (weight > 0xF)
				elog(ERROR, "invalid tsquery: invalid weight bitmap");

			if (val_len > MAXSTRLEN)
				elog(ERROR, "invalid tsquery: operand too long");

			if (datalen > MAXSTRPOS)
				elog(ERROR, "invalid tsquery: total operand length exceeded");

			/* 看似有效 */

			INIT_LEGACY_CRC32(valcrc);
			COMP_LEGACY_CRC32(valcrc, val, val_len);
			FIN_LEGACY_CRC32(valcrc);

			item->qoperand.weight = weight;
			item->qoperand.prefix = (prefix) ? true : false;
			item->qoperand.valcrc = (int32) valcrc;
			item->qoperand.length = val_len;
			item->qoperand.distance = datalen;

			/*
			 * 操作数串在本循环之后才复制到最终结构；
			 * 这里只把它们收进数组
			 */
			operands[i] = val;

			datalen += val_len + 1; /* + 1 为 '\0' 终止符 */
		}
		else if (item->type == QI_OPR)
		{
			int8		oper;

			oper = (int8) pq_getmsgint(buf, sizeof(int8));
			if (oper != OP_NOT && oper != OP_OR && oper != OP_AND && oper != OP_PHRASE)
				elog(ERROR, "invalid tsquery: unrecognized operator type %d",
					 (int) oper);
			if (i == size - 1)
				elog(ERROR, "invalid pointer to right operand");

			item->qoperator.oper = oper;
			if (oper == OP_PHRASE)
				item->qoperator.distance = (int16) pq_getmsgint(buf, sizeof(int16));
		}
		else
			elog(ERROR, "unrecognized tsquery node type: %d", item->type);

		item++;
	}

	/* 扩大缓冲区以为操作数值腾出空间。 */
	query = (TSQuery) repalloc(query, len + datalen);
	item = GETQUERY(query);
	ptr = GETOPERAND(query);

	/*
	 * 填充左指针。顺带检查树的
	 * 良构性。
	 */
	findoprnd(item, size, &needcleanup);

	/* 不可能有 QI_VALSTOP 节点 */
	Assert(!needcleanup);

	/* 复制操作数到输出结构 */
	for (i = 0; i < size; i++)
	{
		if (item->type == QI_VAL)
		{
			memcpy(ptr, operands[i], item->qoperand.length + 1);
			ptr += item->qoperand.length + 1;
		}
		item++;
	}

	pfree(operands);

	Assert(ptr - GETOPERAND(query) == datalen);

	SET_VARSIZE(query, len + datalen);

	PG_RETURN_TSQUERY(query);
}

/*
 * 调试函数，仅用于查看查询
 * 该查询将在索引的非叶子页面上执行
 */
Datum
tsquerytree(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY(0);
	INFIX		nrm;
	text	   *res;
	QueryItem  *q;
	int			len;

	if (query->size == 0)
	{
		res = (text *) palloc(VARHDRSZ);
		SET_VARSIZE(res, VARHDRSZ);
		PG_RETURN_POINTER(res);
	}

	q = clean_NOT(GETQUERY(query), &len);

	if (!q)
	{
		res = cstring_to_text("T");
	}
	else
	{
		nrm.curpol = q;
		nrm.buflen = 32;
		nrm.cur = nrm.buf = (char *) palloc(sizeof(char) * nrm.buflen);
		*(nrm.cur) = '\0';
		nrm.op = GETOPERAND(query);
		infix(&nrm, -1, false);
		res = cstring_to_text_with_len(nrm.buf, nrm.cur - nrm.buf);
		pfree(q);
	}

	PG_FREE_IF_COPY(query, 0);

	PG_RETURN_TEXT_P(res);
}
