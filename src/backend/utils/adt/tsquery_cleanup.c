/*-------------------------------------------------------------------------
 *
 * tsquery_cleanup.c
 *	 从查询中清理 NOT 值和/或停用词
 *	 提供修正用的工具函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_cleanup.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "tsearch/ts_utils.h"
#include "varatt.h"

typedef struct NODE
{
	struct NODE *left;
	struct NODE *right;
	QueryItem  *valnode;
} NODE;

/*
 * 从查询的平面视图构建查询树
 */
static NODE *
maketree(QueryItem *in)
{
	NODE	   *node = (NODE *) palloc(sizeof(NODE));

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	node->valnode = in;
	node->right = node->left = NULL;
	if (in->type == QI_OPR)
	{
		node->right = maketree(in + 1);
		if (in->qoperator.oper != OP_NOT)
			node->left = maketree(in + in->qoperator.left);
	}
	return node;
}

/*
 * plaintree 与 plainnode 的内部状态
 */
typedef struct
{
	QueryItem  *ptr;
	int			len;			/* ptr 分配的大小 */
	int			cur;			/* ptr 中的元素个数 */
} PLAINTREE;

static void
plainnode(PLAINTREE *state, NODE *node)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (state->cur == state->len)
	{
		state->len *= 2;
		state->ptr = (QueryItem *) repalloc(state->ptr, state->len * sizeof(QueryItem));
	}
	memcpy(&(state->ptr[state->cur]), node->valnode, sizeof(QueryItem));
	if (node->valnode->type == QI_VAL)
		state->cur++;
	else if (node->valnode->qoperator.oper == OP_NOT)
	{
		state->ptr[state->cur].qoperator.left = 1;
		state->cur++;
		plainnode(state, node->right);
	}
	else
	{
		int			cur = state->cur;

		state->cur++;
		plainnode(state, node->right);
		state->ptr[cur].qoperator.left = state->cur - cur;
		plainnode(state, node->left);
	}
	pfree(node);
}

/*
 * 从 NODE 树表示构建树的平面视图
 */
static QueryItem *
plaintree(NODE *root, int *len)
{
	PLAINTREE	pl;

	pl.cur = 0;
	pl.len = 16;
	if (root && (root->valnode->type == QI_VAL || root->valnode->type == QI_OPR))
	{
		pl.ptr = (QueryItem *) palloc(pl.len * sizeof(QueryItem));
		plainnode(&pl, root);
	}
	else
		pl.ptr = NULL;
	*len = pl.cur;
	return pl.ptr;
}

static void
freetree(NODE *node)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (!node)
		return;
	if (node->left)
		freetree(node->left);
	if (node->right)
		freetree(node->right);
	pfree(node);
}

/*
 * 为 ! 算子清理树。
 * 它对调试有用，但在
 * 其他情况下该视图随索引搜索使用。
 * 算子 ! 总是返回 TRUE
 */
static NODE *
clean_NOT_intree(NODE *node)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (node->valnode->type == QI_VAL)
		return node;

	if (node->valnode->qoperator.oper == OP_NOT)
	{
		freetree(node);
		return NULL;
	}

	/* & 或 | 算子 */
	if (node->valnode->qoperator.oper == OP_OR)
	{
		if ((node->left = clean_NOT_intree(node->left)) == NULL ||
			(node->right = clean_NOT_intree(node->right)) == NULL)
		{
			freetree(node);
			return NULL;
		}
	}
	else
	{
		NODE	   *res = node;

		Assert(node->valnode->qoperator.oper == OP_AND ||
			   node->valnode->qoperator.oper == OP_PHRASE);

		node->left = clean_NOT_intree(node->left);
		node->right = clean_NOT_intree(node->right);
		if (node->left == NULL && node->right == NULL)
		{
			pfree(node);
			res = NULL;
		}
		else if (node->left == NULL)
		{
			res = node->right;
			pfree(node);
		}
		else if (node->right == NULL)
		{
			res = node->left;
			pfree(node);
		}
		return res;
	}
	return node;
}

QueryItem *
clean_NOT(QueryItem *ptr, int *len)
{
	NODE	   *root = maketree(ptr);

	return plaintree(clean_NOT_intree(root), len);
}


/*
 * 移除查询树的 QI_VALSTOP 停用词节点。
 *
 * 若查询退化为空则返回 NULL。输入不得为 NULL。
 *
 * 当我们因移除短语算子的一个或两个参数而
 * 移除某个短语算子时，可能需要调整其父短语
 * 算子距离。如 'a' 是停用词，则：
 *		(b <-> a) <-> c  应变为  b <2> c
 *		b <-> (a <-> c)  应变为  b <2> c
 *		(b <-> (a <-> a)) <-> c  应变为  b <3> c
 *		b <-> ((a <-> a) <-> c)  应变为  b <3> c
 * 为此我们定义两个输出参数：
 *		ladd：要加到本节点左侧短语距离上的量
 *		radd：本节点右侧短语距离所需加上的量
 * 需要两个输出，因为可能要把调整冒泡传递给
 * 两个不同的父短语算子。考虑
 *		w <-> (((a <-> x) <2> (y <3> a)) <-> z)
 * 移除两个 a 之后，当我们考虑 <2> 节点（此时
 * 只剩 x <2> y），有一个 ladd 距离 1 需要向上传
 * 递到最顶上（最左）的 <->，还有一个 radd 距离 3
 * 要向右传给最右边的 <->，最终将得到
 *		w <2> ((x <2> y) <4> z)
 * 在树的底部附近，我们可能遇到只由停用词构
 * 成的子树。这种子树内任何短语算子的距离都
 * 会被求和并同时传播给 ladd 与 radd，因为我们
 * 不知道自己处于最低存活短语算子的哪一侧。
 * 规则是：任何退化为 NULL 的子树必须返回相等
 * 的 ladd 与 radd 值，父节点只应采纳其中一个。
 *
 * 目前，我们只对相邻的短语算子实现这种调整。
 * 例如 'x <-> ((a <-> y) | z)' 将变成 'x <-> (y | z)'，
 * 这并不理想，但若不对 tsquery 结构做某些重新
 * 设计，就没有办法表达真正想要的语义。把它变
 * 成 'x <2> (y | z)' 显然也不会更好。由于这是
 * 一个非常怪异的边角情况，暂且放过它。但在
 * 其中间非短语算子也被移除时便可修复，例如
 * '((x <-> a) | a) <-> y' 变为 'x <2> y'。
 */
static NODE *
clean_stopword_intree(NODE *node, int *ladd, int *radd)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	/* 默认输出参数表示父距离没有变化 */
	*ladd = *radd = 0;

	if (node->valnode->type == QI_VAL)
		return node;
	else if (node->valnode->type == QI_VALSTOP)
	{
		pfree(node);
		return NULL;
	}

	Assert(node->valnode->type == QI_OPR);

	if (node->valnode->qoperator.oper == OP_NOT)
	{
		/* NOT 不改变模式宽度，故只需报告子节点距离 */
		node->right = clean_stopword_intree(node->right, ladd, radd);
		if (!node->right)
		{
			freetree(node);
			return NULL;
		}
	}
	else
	{
		NODE	   *res = node;
		bool		isphrase;
		int			ndistance,
					lladd,
					lradd,
					rladd,
					rradd;

		/* 首先递归 */
		node->left = clean_stopword_intree(node->left, &lladd, &lradd);
		node->right = clean_stopword_intree(node->right, &rladd, &rradd);

		/* 若当前节点是 OP_PHRASE，取其距离 */
		isphrase = (node->valnode->qoperator.oper == OP_PHRASE);
		ndistance = isphrase ? node->valnode->qoperator.distance : 0;

		if (node->left == NULL && node->right == NULL)
		{
			/*
			 * 当把一个短语节点完全折叠掉时，要把其
			 * 自身的距离同时传播到 *ladd 与 *radd；只计
			 * 一次由父节点负责。另外，短语节点的
			 * 来自子节点的距离会求和并向上传给父节
			 * 点（我们假定 lladd == lradd 且 rladd == rradd，
			 * 否则规则在更低层被破坏）。但若这不是
			 * 短语节点，就取两子距离较大者；这与
			 * 合 TS_execute 在非停用词情形下的做法。
			 */
			if (isphrase)
				*ladd = *radd = lladd + ndistance + rladd;
			else
				*ladd = *radd = Max(lladd, rladd);
			freetree(node);
			return NULL;
		}
		else if (node->left == NULL)
		{
			/* 移除该算子与左子节点 */
			/* lladd 与 lradd 相等/冗余，不要两个都计 */
			if (isphrase)
			{
				/* 算子自身的距离必须向左传播 */
				*ladd = lladd + ndistance + rladd;
				*radd = rradd;
			}
			else
			{
				/* 在非短语算子处，完全舍弃左子节点 */
				*ladd = rladd;
				*radd = rradd;
			}
			res = node->right;
			pfree(node);
		}
		else if (node->right == NULL)
		{
			/* 移除该算子与右子节点 */
			/* rladd 与 rradd 相等/冗余，不要两个都计 */
			if (isphrase)
			{
				/* 算子自身的距离必须向右传播 */
				*ladd = lladd;
				*radd = lradd + ndistance + rradd;
			}
			else
			{
				/* 在非短语算子处，完全舍弃右子节点 */
				*ladd = lladd;
				*radd = lradd;
			}
			res = node->left;
			pfree(node);
		}
		else if (isphrase)
		{
			/* 在本层吸收相应的修正 */
			node->valnode->qoperator.distance += lradd + rladd;
			/* 把任何未被计入的修正向上传播 */
			*ladd = lladd;
			*radd = rradd;
		}
		else
		{
			/* 保留的是非短语算子，故 ladd/radd 保持 0 */
		}

		return res;
	}
	return node;
}

/*
 * 查询树中的元素个数
 */
static int32
calcstrlen(NODE *node)
{
	int32		size = 0;

	if (node->valnode->type == QI_VAL)
	{
		size = node->valnode->qoperand.length + 1;
	}
	else
	{
		Assert(node->valnode->type == QI_OPR);

		size = calcstrlen(node->right);
		if (node->valnode->qoperator.oper != OP_NOT)
			size += calcstrlen(node->left);
	}

	return size;
}

/*
 * 从 TSQuery 移除 QI_VALSTOP 停用词节点。
 */
TSQuery
cleanup_tsquery_stopwords(TSQuery in, bool noisy)
{
	int32		len,
				lenstr,
				commonlen,
				i;
	NODE	   *root;
	int			ladd,
				radd;
	TSQuery		out;
	QueryItem  *items;
	char	   *operands;

	if (in->size == 0)
		return in;

	/* 消除停用词 */
	root = clean_stopword_intree(maketree(GETQUERY(in)), &ladd, &radd);
	if (root == NULL)
	{
		if (noisy)
			ereport(NOTICE,
					(errmsg("text-search query contains only stop words or doesn't contain lexemes, ignored")));
		out = palloc(HDRSIZETQ);
		out->size = 0;
		SET_VARSIZE(out, HDRSIZETQ);
		return out;
	}

	/*
	 * 从平面视图构建 TSQuery
	 */

	lenstr = calcstrlen(root);
	items = plaintree(root, &len);
	commonlen = COMPUTESIZE(len, lenstr);

	out = palloc(commonlen);
	SET_VARSIZE(out, commonlen);
	out->size = len;

	memcpy(GETQUERY(out), items, len * sizeof(QueryItem));

	items = GETQUERY(out);
	operands = GETOPERAND(out);
	for (i = 0; i < out->size; i++)
	{
		QueryOperand *op = (QueryOperand *) &items[i];

		if (op->type != QI_VAL)
			continue;

		memcpy(operands, GETOPERAND(in) + op->distance, op->length);
		operands[op->length] = '\0';
		op->distance = operands - GETOPERAND(out);
		operands += op->length + 1;
	}

	return out;
}
