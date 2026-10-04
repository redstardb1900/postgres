/*-------------------------------------------------------------------------
 *
 * read.c
 *	  routines to convert a string (legal ascii representation of node) back
 *	  to nodes
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/read.c
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Nov 2, 1994		file creation
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "common/string.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"
#include "nodes/readfuncs.h"
#include "nodes/value.h"


/* pg_strtok 的静态状态 */
static const char *pg_strtok_ptr = NULL;

/* 决定 readfuncs.c 应如何对待位置字段的状态标志 */
#ifdef WRITE_READ_PARSE_PLAN_TREES
bool		restore_location_fields = false;
#endif


/*
 * stringToNode -
 *	  从其字符串表示（假定有效）构建 Node 树
 *
 * restore_loc_fields 指示 readfuncs.c 是恢复位置
 * 字段，还是把它们设为 -1。目前只在
 * 设置了 WRITE_READ_PARSE_PLAN_TREES 调试标志的构建中支持。
 */
static void *
stringToNodeInternal(const char *str, bool restore_loc_fields)
{
	void	   *retval;
	const char *save_strtok;
#ifdef WRITE_READ_PARSE_PLAN_TREES
	bool		save_restore_location_fields;
#endif

	/*
	 * 我们保存并恢复 pg_strtok 先前的状态。这样
	 * stringToNode 的重入调用就是安全的，不必
	 * 把下一个字符的指针传遍所有 readfuncs.c 代码，
	 * 以免带来大量记法上的开销。
	 */
	save_strtok = pg_strtok_ptr;

	pg_strtok_ptr = str;		/* 让 pg_strtok 指向要读的字符串 */

	/*
	 * 若已启用，同样保存并恢复位置字段处理标志。
	 */
#ifdef WRITE_READ_PARSE_PLAN_TREES
	save_restore_location_fields = restore_location_fields;
	restore_location_fields = restore_loc_fields;
#endif

	retval = nodeRead(NULL, 0); /* 执行读取 */

	pg_strtok_ptr = save_strtok;

#ifdef WRITE_READ_PARSE_PLAN_TREES
	restore_location_fields = save_restore_location_fields;
#endif

	return retval;
}

/*
 * 外部可见的入口点
 */
void *
stringToNode(const char *str)
{
	return stringToNodeInternal(str, false);
}

#ifdef WRITE_READ_PARSE_PLAN_TREES

void *
stringToNodeWithLocations(const char *str)
{
	return stringToNodeInternal(str, true);
}

#endif


/*****************************************************************************
 *
 * lisp 词元解析器
 *
 *****************************************************************************/

/*
 * pg_strtok --- 从字符串中取出下一个词元。
 *
 * 有点像 strtok，只是它从不修改源字符串。
 * （它不把空字符写进字符串，而是把词元的长度
 * 返回给调用方。）
 * 另外，什么是词元的规则是写死的，而不是通过
 * 传入一组终止字符来配置。
 *
 * 假定该字符串已经由 stringToNode 初始化过。
 *
 * 词元规则如下：
 *	* 空白（空格、制表符、换行）总是分隔词元。
 *	* 字符 '('、')'、'{'、'}' 即使周围没有任何空白，
 *	  也各自构成单独的词元。
 *	* 否则，一个词元是直到下一个空白
 *	  或这四个特殊字符之一出现之前的全部字符。
 *	* 反斜杠 '\' 可以用来引用空白或这四个
 *	  特殊字符之一，使它被当作普通词元字符。
 *	  反斜杠本身为了一致也必须再加反斜杠。
 *	  任何其他字符也可以加反斜杠，但并非必须。
 *	* 若得到的词元是 '<>'（没有反斜杠），就返回
 *	  指向该词元的非 NULL 指针，但长度 == 0。注意
 *	  没有其他办法得到零长度词元。
 *
 * 返回指向下一个词元起点的指针，并在 *length 中返回
 * 词元的长度（包括任何嵌入的反斜杠！）。若没有
 * 更多词元，就返回 NULL 和 0。
 *
 * 注意：本例程不去掉反斜杠；必要时调用方必须自己做
 * （见 "debackslash"）。
 *
 * 注意：在 7.0 发行之前，本例程还有一个特殊情况，把
 * 以 '"' 开头的词元视为延伸到下一个 '"'。不过这段代码是
 * 坏的，因为它无法应付含有
 * 嵌入 '"' 的字符串。因此我去掉了这个特殊情况，改为
 * 引入用反斜杠引用字符的规则。更高层
 * 代码应当给字符串常量加上反斜杠，以确保它被当作
 * 单个词元。
 */
const char *
pg_strtok(int *length)
{
	const char *local_str;		/* 指向字符串的工作指针 */
	const char *ret_str;		/* 要返回的词元的起点 */

	local_str = pg_strtok_ptr;

	while (*local_str == ' ' || *local_str == '\n' || *local_str == '\t')
		local_str++;

	if (*local_str == '\0')
	{
		*length = 0;
		pg_strtok_ptr = local_str;
		return NULL;			/* 没有更多词元 */
	}

	/*
	 * 现在指向下一个词元的起点。
	 */
	ret_str = local_str;

	if (*local_str == '(' || *local_str == ')' ||
		*local_str == '{' || *local_str == '}')
	{
		/* 特殊的单字符词元 */
		local_str++;
	}
	else
	{
		/* 普通词元，可能含有反斜杠 */
		while (*local_str != '\0' &&
			   *local_str != ' ' && *local_str != '\n' &&
			   *local_str != '\t' &&
			   *local_str != '(' && *local_str != ')' &&
			   *local_str != '{' && *local_str != '}')
		{
			if (*local_str == '\\' && local_str[1] != '\0')
				local_str += 2;
			else
				local_str++;
		}
	}

	*length = local_str - ret_str;

	/* 识别 "空" 词元的特殊情况 */
	if (*length == 2 && ret_str[0] == '<' && ret_str[1] == '>')
		*length = 0;

	pg_strtok_ptr = local_str;

	return ret_str;
}

/*
 * debackslash -
 *	  创建保存给定词元的、palloc 出来的字符串。
 *	  词元中任何起保护作用的反斜杠都会被去掉。
 */
char *
debackslash(const char *token, int length)
{
	char	   *result = palloc(length + 1);
	char	   *ptr = result;

	while (length > 0)
	{
		if (*token == '\\' && length > 1)
			token++, length--;
		*ptr++ = *token++;
		length--;
	}
	*ptr = '\0';
	return result;
}

#define RIGHT_PAREN (1000000 + 1)
#define LEFT_PAREN	(1000000 + 2)
#define LEFT_BRACE	(1000000 + 3)
#define OTHER_TOKEN (1000000 + 4)

/*
 * nodeTokenType -
 *	  返回 token 中所含节点词元的类型。
 *	  它返回下列有效 NodeTags 之一：
 *		T_Integer、T_Float、T_Boolean、T_String、T_BitString
 *	  以及它自己的一些：
 *		RIGHT_PAREN、LEFT_PAREN、LEFT_BRACE、OTHER_TOKEN
 *
 *	  假定：ascii 表示是合法的
 */
static NodeTag
nodeTokenType(const char *token, int length)
{
	NodeTag		retval;
	const char *numptr;
	int			numlen;

	/*
	 * 检查该词元是否为数字
	 */
	numptr = token;
	numlen = length;
	if (*numptr == '+' || *numptr == '-')
		numptr++, numlen--;
	if ((numlen > 0 && isdigit((unsigned char) *numptr)) ||
		(numlen > 1 && *numptr == '.' && isdigit((unsigned char) numptr[1])))
	{
		/*
		 * 是的。判断它是整数还是浮点数；这需要
		 * 语法检查和范围检查。strtoint() 可以两者都为我们做。
		 * 我们知道词元会结束于 strtoint 会停下的字符，
		 * 因此不需要修改该字符串。
		 */
		char	   *endptr;

		errno = 0;
		(void) strtoint(numptr, &endptr, 10);
		if (endptr != token + length || errno == ERANGE)
			return T_Float;
		return T_Integer;
	}

	/*
	 * 这三种情况不需要长度检查，因为 pg_strtok() 会
	 * 始终把它们当作单字节词元
	 */
	else if (*token == '(')
		retval = LEFT_PAREN;
	else if (*token == ')')
		retval = RIGHT_PAREN;
	else if (*token == '{')
		retval = LEFT_BRACE;
	else if ((length == 4 && strncmp(token, "true", 4) == 0) ||
			 (length == 5 && strncmp(token, "false", 5) == 0))
		retval = T_Boolean;
	else if (*token == '"' && length > 1 && token[length - 1] == '"')
		retval = T_String;
	else if (*token == 'b' || *token == 'x')
		retval = T_BitString;
	else
		retval = OTHER_TOKEN;
	return retval;
}

/*
 * nodeRead -
 *	  稍高一层的读取器。
 *
 * 本例程在纯词法的 pg_strtok() 之上应用一些语义知识。
 * 它可以读取
 *	* 值词元节点（整数、浮点数、布尔值或字符串）；
 *	* 一般节点（经由 readfuncs.c 的 parseNodeString()）；
 *	* 上述各项的列表；
 *	* 整数、OIDs 或 TransactionIds 的列表。
 * 返回值声明为 void * 而不是 Node *，以免在赋给不同类型字段的
 * 调用方里必须显式转换。
 *
 * 外部调用方应当始终为这些参数传入 NULL/0。在内部，
 * 当上层递归已经扫描了节点表示的第一个词元时，
 * 可以传入非 NULL 的 token。
 *
 * 我们假定 pg_strtok 已经用一个要读的字符串初始化过（因此
 * 这只应在 stringToNode 操作内部调用）。
 */
void *
nodeRead(const char *token, int tok_len)
{
	Node	   *result;
	NodeTag		type;

	if (token == NULL)			/* 需要读一个词元吗？ */
	{
		token = pg_strtok(&tok_len);

		if (token == NULL)		/* 输入结束 */
			return NULL;
	}

	type = nodeTokenType(token, tok_len);

	switch ((int) type)
	{
		case LEFT_BRACE:
			result = parseNodeString();
			token = pg_strtok(&tok_len);
			if (token == NULL || token[0] != '}')
				elog(ERROR, "did not find '}' at end of input node");
			break;
		case LEFT_PAREN:
			{
				List	   *l = NIL;

				/*----------
				 * Could be an integer list:	(i int int ...)
				 * or an OID list:				(o int int ...)
				 * or an XID list:				(x int int ...)
				 * or a bitmapset:				(b int int ...)
				 * or a list of nodes/values:	(node node ...)
				 *----------
				 */
				token = pg_strtok(&tok_len);
				if (token == NULL)
					elog(ERROR, "unterminated List structure");
				if (tok_len == 1 && token[0] == 'i')
				{
					/* 整数列表 */
					for (;;)
					{
						int			val;
						char	   *endptr;

						token = pg_strtok(&tok_len);
						if (token == NULL)
							elog(ERROR, "unterminated List structure");
						if (token[0] == ')')
							break;
						val = (int) strtol(token, &endptr, 10);
						if (endptr != token + tok_len)
							elog(ERROR, "unrecognized integer: \"%.*s\"",
								 tok_len, token);
						l = lappend_int(l, val);
					}
					result = (Node *) l;
				}
				else if (tok_len == 1 && token[0] == 'o')
				{
					/* OIDs 列表 */
					for (;;)
					{
						Oid			val;
						char	   *endptr;

						token = pg_strtok(&tok_len);
						if (token == NULL)
							elog(ERROR, "unterminated List structure");
						if (token[0] == ')')
							break;
						val = (Oid) strtoul(token, &endptr, 10);
						if (endptr != token + tok_len)
							elog(ERROR, "unrecognized OID: \"%.*s\"",
								 tok_len, token);
						l = lappend_oid(l, val);
					}
					result = (Node *) l;
				}
				else if (tok_len == 1 && token[0] == 'x')
				{
					/* TransactionIds 列表 */
					for (;;)
					{
						TransactionId val;
						char	   *endptr;

						token = pg_strtok(&tok_len);
						if (token == NULL)
							elog(ERROR, "unterminated List structure");
						if (token[0] == ')')
							break;
						val = (TransactionId) strtoul(token, &endptr, 10);
						if (endptr != token + tok_len)
							elog(ERROR, "unrecognized Xid: \"%.*s\"",
								 tok_len, token);
						l = lappend_xid(l, val);
					}
					result = (Node *) l;
				}
				else if (tok_len == 1 && token[0] == 'b')
				{
					/* Bitmapset -- 另见 _readBitmapset() */
					Bitmapset  *bms = NULL;

					for (;;)
					{
						int			val;
						char	   *endptr;

						token = pg_strtok(&tok_len);
						if (token == NULL)
							elog(ERROR, "unterminated Bitmapset structure");
						if (tok_len == 1 && token[0] == ')')
							break;
						val = (int) strtol(token, &endptr, 10);
						if (endptr != token + tok_len)
							elog(ERROR, "unrecognized integer: \"%.*s\"",
								 tok_len, token);
						bms = bms_add_member(bms, val);
					}
					result = (Node *) bms;
				}
				else
				{
					/* 其他节点类型的列表 */
					for (;;)
					{
						/* 我们已经扫描了下一个词元…… */
						if (token[0] == ')')
							break;
						l = lappend(l, nodeRead(token, tok_len));
						token = pg_strtok(&tok_len);
						if (token == NULL)
							elog(ERROR, "unterminated List structure");
					}
					result = (Node *) l;
				}
				break;
			}
		case RIGHT_PAREN:
			elog(ERROR, "unexpected right parenthesis");
			result = NULL;		/* 消除编译器告警 */
			break;
		case OTHER_TOKEN:
			if (tok_len == 0)
			{
				/* 必须是 "<>" --- 表示空指针 */
				result = NULL;
			}
			else
			{
				elog(ERROR, "unrecognized token: \"%.*s\"", tok_len, token);
				result = NULL;	/* 消除编译器告警 */
			}
			break;
		case T_Integer:

			/*
			 * 我们知道该词元结束于 atoi 会停下的字符
			 */
			result = (Node *) makeInteger(atoi(token));
			break;
		case T_Float:
			{
				char	   *fval = (char *) palloc(tok_len + 1);

				memcpy(fval, token, tok_len);
				fval[tok_len] = '\0';
				result = (Node *) makeFloat(fval);
			}
			break;
		case T_Boolean:
			result = (Node *) makeBoolean(token[0] == 't');
			break;
		case T_String:
			/* 需要去掉前导和尾随引号，以及反斜杠 */
			result = (Node *) makeString(debackslash(token + 1, tok_len - 2));
			break;
		case T_BitString:
			/* 需要去掉反斜杠，但没有引号 */
			result = (Node *) makeBitString(debackslash(token, tok_len));
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) type);
			result = NULL;		/* 消除编译器告警 */
			break;
	}

	return (void *) result;
}
