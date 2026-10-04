/*-------------------------------------------------------------------------
 *
 * copyfuncs.c
 *	  Copy functions for Postgres tree nodes.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/copyfuncs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "utils/datum.h"


/*
 * 简化各类字段复制的宏。尽可能使用它们，
 * 以减少低级笔误的机会。注意，这些宏把
 * Copy 例程中局部变量名为 'newnode' 和 'from'
 * 这一约定写死了。
 */

/* 复制简单标量字段（int、float、bool、enum 等） */
#define COPY_SCALAR_FIELD(fldname) \
	(newnode->fldname = from->fldname)

/* 复制指向某种 Node 或 Node 树的字段 */
#define COPY_NODE_FIELD(fldname) \
	(newnode->fldname = copyObjectImpl(from->fldname))

/* 复制指向 Bitmapset 的字段 */
#define COPY_BITMAPSET_FIELD(fldname) \
	(newnode->fldname = bms_copy(from->fldname))

/* 复制指向 C 字符串的字段，或者也许是 NULL */
#define COPY_STRING_FIELD(fldname) \
	(newnode->fldname = from->fldname ? pstrdup(from->fldname) : (char *) NULL)

/* 复制内联数组字段 */
#define COPY_ARRAY_FIELD(fldname) \
	memcpy(newnode->fldname, from->fldname, sizeof(newnode->fldname))

/* 复制指向大小为 sz 的、简单 palloc 对象的字段 */
#define COPY_POINTER_FIELD(fldname, sz) \
	do { \
		Size	_size = (sz); \
		if (_size > 0) \
		{ \
			newnode->fldname = palloc(_size); \
			memcpy(newnode->fldname, from->fldname, _size); \
		} \
	} while (0)

/* 复制解析位置字段（对 Copy 来说，这与标量情况相同） */
#define COPY_LOCATION_FIELD(fldname) \
	(newnode->fldname = from->fldname)


#include "copyfuncs.funcs.c"


/*
 * 带 custom_copy_equal 属性的节点的支持函数
 */

static Const *
_copyConst(const Const *from)
{
	Const	   *newnode = makeNode(Const);

	COPY_SCALAR_FIELD(consttype);
	COPY_SCALAR_FIELD(consttypmod);
	COPY_SCALAR_FIELD(constcollid);
	COPY_SCALAR_FIELD(constlen);

	if (from->constbyval || from->constisnull)
	{
		/*
		 * 按值传递，因此只复制 datum。另外，值为 null 时不要尝试复制
		 * 结构体！
		 */
		newnode->constvalue = from->constvalue;
	}
	else
	{
		/*
		 * 按引用传递。我们需要一份 palloc 出来的副本。
		 */
		newnode->constvalue = datumCopy(from->constvalue,
										from->constbyval,
										from->constlen);
	}

	COPY_SCALAR_FIELD(constisnull);
	COPY_SCALAR_FIELD(constbyval);
	COPY_LOCATION_FIELD(location);

	return newnode;
}

static A_Const *
_copyA_Const(const A_Const *from)
{
	A_Const    *newnode = makeNode(A_Const);

	COPY_SCALAR_FIELD(isnull);
	if (!from->isnull)
	{
		/* 必须与其他 _copy*() 函数一样复制这部分逻辑。 */
		COPY_SCALAR_FIELD(val.node.type);
		switch (nodeTag(&from->val))
		{
			case T_Integer:
				COPY_SCALAR_FIELD(val.ival.ival);
				break;
			case T_Float:
				COPY_STRING_FIELD(val.fval.fval);
				break;
			case T_Boolean:
				COPY_SCALAR_FIELD(val.boolval.boolval);
				break;
			case T_String:
				COPY_STRING_FIELD(val.sval.sval);
				break;
			case T_BitString:
				COPY_STRING_FIELD(val.bsval.bsval);
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&from->val));
				break;
		}
	}

	COPY_LOCATION_FIELD(location);

	return newnode;
}

static ExtensibleNode *
_copyExtensibleNode(const ExtensibleNode *from)
{
	ExtensibleNode *newnode;
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(from->extnodename, false);
	newnode = (ExtensibleNode *) newNode(methods->node_size,
										 T_ExtensibleNode);
	COPY_STRING_FIELD(extnodename);

	/* 复制私有字段 */
	methods->nodeCopy(newnode, from);

	return newnode;
}

static Bitmapset *
_copyBitmapset(const Bitmapset *from)
{
	return bms_copy(from);
}


/*
 * copyObjectImpl -- copyObject() 的实现；见 nodes/nodes.h
 *
 * 创建 Node 树或列表的副本。这是一次 "深" 拷贝：所有
 * 子结构也会被递归复制。
 */
void *
copyObjectImpl(const void *from)
{
	void	   *retval;

	if (from == NULL)
		return NULL;

	/* 防止过于复杂的表达式导致栈溢出 */
	check_stack_depth();

	switch (nodeTag(from))
	{
#include "copyfuncs.switch.c"

		case T_List:
			retval = list_copy_deep(from);
			break;

			/*
			 * 整数、OIDs 和 XIDs 的列表不需要深拷贝，
			 * 因此我们通过 list_copy() 做浅拷贝
			 */
		case T_IntList:
		case T_OidList:
		case T_XidList:
			retval = list_copy(from);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(from));
			retval = 0;			/* 消除编译器告警 */
			break;
	}

	return retval;
}
