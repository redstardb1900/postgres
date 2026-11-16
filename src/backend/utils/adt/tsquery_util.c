/*-------------------------------------------------------------------------
 *
 * tsquery_util.c
 *	  tsquery 实用工具
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_util.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "tsearch/ts_utils.h"
#include "varatt.h"

/*
 * 为 QueryItem 数组格式的 tsquery 构建 QTNode 树。
 */
QTNode *
QT2QTN(QueryItem *in, char *operand)
{
	QTNode	   *node = (QTNode *) palloc0(sizeof(QTNode));

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	node->valnode = in;

	if (in->type == QI_OPR)
	{
		node->child = (QTNode **) palloc0(sizeof(QTNode *) * 2);
		node->child[0] = QT2QTN(in + 1, operand);
		node->sign = node->child[0]->sign;
		if (in->qoperator.oper == OP_NOT)
			node->nchild = 1;
		else
		{
			node->nchild = 2;
			node->child[1] = QT2QTN(in + in->qoperator.left, operand);
			node->sign |= node->child[1]->sign;
		}
	}
	else if (operand)
	{
		node->word = operand + in->qoperand.distance;
		node->sign = ((uint32) 1) << (((unsigned int) in->qoperand.valcrc) % 32);
	}

	return node;
}

/*
 * 释放 QTNode 树
 *
 * 被引用的 "word"、"valnode" 条目若被 flags 标为暂态即
 * 释放。
 */
void
QTNFree(QTNode *in)
{
	if (!in)
		return;

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (in->valnode->type == QI_VAL && in->word && (in->flags & QTN_WORDFREE) != 0)
		pfree(in->word);

	if (in->valnode->type == QI_OPR)
	{
		int			i;

		for (i = 0; i < in->nchild; i++)
			QTNFree(in->child[i]);
	}
	if (in->child)
		pfree(in->child);

	if (in->flags & QTN_NEEDFREE)
		pfree(in->valnode);

	pfree(in);
}

/*
 * QTNode 的排序比较器。
 *
 * 排序次序带有一定随意性。
 */
int
QTNodeCompare(QTNode *an, QTNode *bn)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (an->valnode->type != bn->valnode->type)
		return (an->valnode->type > bn->valnode->type) ? -1 : 1;

	if (an->valnode->type == QI_OPR)
	{
		QueryOperator *ao = &an->valnode->qoperator;
		QueryOperator *bo = &bn->valnode->qoperator;

		if (ao->oper != bo->oper)
			return (ao->oper > bo->oper) ? -1 : 1;

		if (an->nchild != bn->nchild)
			return (an->nchild > bn->nchild) ? -1 : 1;

		{
			int			i,
						res;

			for (i = 0; i < an->nchild; i++)
				if ((res = QTNodeCompare(an->child[i], bn->child[i])) != 0)
					return res;
		}

		if (ao->oper == OP_PHRASE && ao->distance != bo->distance)
			return (ao->distance > bo->distance) ? -1 : 1;

		return 0;
	}
	else if (an->valnode->type == QI_VAL)
	{
		QueryOperand *ao = &an->valnode->qoperand;
		QueryOperand *bo = &bn->valnode->qoperand;

		if (ao->valcrc != bo->valcrc)
		{
			return (ao->valcrc > bo->valcrc) ? -1 : 1;
		}

		return tsCompareString(an->word, ao->length, bn->word, bo->length, false);
	}
	else
	{
		elog(ERROR, "unrecognized QueryItem type: %d", an->valnode->type);
		return 0;				/* 防编译器告警 */
	}
}

/*
 * QTNode 指针的 qsort 比较器。
 */
static int
cmpQTN(const void *a, const void *b)
{
	return QTNodeCompare(*(QTNode *const *) a, *(QTNode *const *) b);
}

/*
 * 通过对 AND/OR 节点的子节点排序，把 QTNode 树规
 * 范化成任意但良定义的次序。
 */
void
QTNSort(QTNode *in)
{
	int			i;

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (in->valnode->type != QI_OPR)
		return;

	for (i = 0; i < in->nchild; i++)
		QTNSort(in->child[i]);
	if (in->nchild > 1 && in->valnode->qoperator.oper != OP_PHRASE)
		qsort(in->child, in->nchild, sizeof(QTNode *), cmpQTN);
}

/*
 * 两棵 QTNode 树按 QTNodeCompare 相等吗？
 */
bool
QTNEq(QTNode *a, QTNode *b)
{
	uint32		sign = a->sign & b->sign;

	if (!(sign == a->sign && sign == b->sign))
		return false;

	return (QTNodeCompare(a, b) == 0);
}

/*
 * 移除不必要的中间节点。例如：
 *
 *	OR			OR
 * a  OR	-> a b c
 *	 b	c
 */
void
QTNTernary(QTNode *in)
{
	int			i;

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (in->valnode->type != QI_OPR)
		return;

	for (i = 0; i < in->nchild; i++)
		QTNTernary(in->child[i]);

	/* 只有 AND 与 OR 有结合律，勿展平其他节点类型 */
	if (in->valnode->qoperator.oper != OP_AND &&
		in->valnode->qoperator.oper != OP_OR)
		return;

	for (i = 0; i < in->nchild; i++)
	{
		QTNode	   *cc = in->child[i];

		if (cc->valnode->type == QI_OPR &&
			in->valnode->qoperator.oper == cc->valnode->qoperator.oper)
		{
			int			oldnchild = in->nchild;

			in->nchild += cc->nchild - 1;
			in->child = (QTNode **) repalloc(in->child, in->nchild * sizeof(QTNode *));

			if (i + 1 != oldnchild)
				memmove(in->child + i + cc->nchild, in->child + i + 1,
						(oldnchild - i - 1) * sizeof(QTNode *));

			memcpy(in->child + i, cc->child, cc->nchild * sizeof(QTNode *));
			i += cc->nchild - 1;

			if (cc->flags & QTN_NEEDFREE)
				pfree(cc->valnode);
			pfree(cc);
		}
	}
}

/*
 * 通过插入中间节点把树转换成二叉树。
 * （QTNTernary 之逆）
 */
void
QTNBinary(QTNode *in)
{
	int			i;

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (in->valnode->type != QI_OPR)
		return;

	for (i = 0; i < in->nchild; i++)
		QTNBinary(in->child[i]);

	while (in->nchild > 2)
	{
		QTNode	   *nn = (QTNode *) palloc0(sizeof(QTNode));

		nn->valnode = (QueryItem *) palloc0(sizeof(QueryItem));
		nn->child = (QTNode **) palloc0(sizeof(QTNode *) * 2);

		nn->nchild = 2;
		nn->flags = QTN_NEEDFREE;

		nn->child[0] = in->child[0];
		nn->child[1] = in->child[1];
		nn->sign = nn->child[0]->sign | nn->child[1]->sign;

		nn->valnode->type = in->valnode->type;
		nn->valnode->qoperator.oper = in->valnode->qoperator.oper;

		in->child[0] = nn;
		in->child[1] = in->child[in->nchild - 1];
		in->nchild--;
	}
}

/*
 * 统计树中操作数字符串的总长度（包括 '\0' 终止
 * 符）以及节点总数。
 * 调用者须将 *sumlen 与 *nnode 置零。
 */
static void
cntsize(QTNode *in, int *sumlen, int *nnode)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	*nnode += 1;
	if (in->valnode->type == QI_OPR)
	{
		int			i;

		for (i = 0; i < in->nchild; i++)
			cntsize(in->child[i], sumlen, nnode);
	}
	else
	{
		*sumlen += in->valnode->qoperand.length + 1;
	}
}

typedef struct
{
	QueryItem  *curitem;
	char	   *operand;
	char	   *curoperand;
} QTN2QTState;

/*
 * 把 QTNode 树递归转换成平面 tsquery 格式。
 * 调用者必须已分配好大小正确的数组。
 */
static void
fillQT(QTN2QTState *state, QTNode *in)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (in->valnode->type == QI_VAL)
	{
		memcpy(state->curitem, in->valnode, sizeof(QueryOperand));

		memcpy(state->curoperand, in->word, in->valnode->qoperand.length);
		state->curitem->qoperand.distance = state->curoperand - state->operand;
		state->curoperand[in->valnode->qoperand.length] = '\0';
		state->curoperand += in->valnode->qoperand.length + 1;
		state->curitem++;
	}
	else
	{
		QueryItem  *curitem = state->curitem;

		Assert(in->valnode->type == QI_OPR);

		memcpy(state->curitem, in->valnode, sizeof(QueryOperator));

		Assert(in->nchild <= 2);
		state->curitem++;

		fillQT(state, in->child[0]);

		if (in->nchild == 2)
		{
			curitem->qoperator.left = state->curitem - curitem;
			fillQT(state, in->child[1]);
		}
	}
}

/*
 * 从 QTNode 树构建平面 tsquery。
 */
TSQuery
QTN2QT(QTNode *in)
{
	TSQuery		out;
	int			len;
	int			sumlen = 0,
				nnode = 0;
	QTN2QTState state;

	cntsize(in, &sumlen, &nnode);

	if (TSQUERY_TOO_BIG(nnode, sumlen))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("tsquery is too large")));
	len = COMPUTESIZE(nnode, sumlen);

	out = (TSQuery) palloc0(len);
	SET_VARSIZE(out, len);
	out->size = nnode;

	state.curitem = GETQUERY(out);
	state.operand = state.curoperand = GETOPERAND(out);

	fillQT(&state, in);
	return out;
}

/*
 * 复制 QTNode 树
 *
 * 也会制作可修改的 words 与 valnodes 副本。
 */
QTNode *
QTNCopy(QTNode *in)
{
	QTNode	   *out;

	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	out = (QTNode *) palloc(sizeof(QTNode));

	*out = *in;
	out->valnode = (QueryItem *) palloc(sizeof(QueryItem));
	*(out->valnode) = *(in->valnode);
	out->flags |= QTN_NEEDFREE;

	if (in->valnode->type == QI_VAL)
	{
		out->word = palloc(in->valnode->qoperand.length + 1);
		memcpy(out->word, in->word, in->valnode->qoperand.length);
		out->word[in->valnode->qoperand.length] = '\0';
		out->flags |= QTN_WORDFREE;
	}
	else
	{
		int			i;

		out->child = (QTNode **) palloc(sizeof(QTNode *) * in->nchild);

		for (i = 0; i < in->nchild; i++)
			out->child[i] = QTNCopy(in->child[i]);
	}

	return out;
}

/*
 * 清除 QTNode 树中所有节点的指定标志位。
 */
void
QTNClearFlags(QTNode *in, uint32 flags)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	in->flags &= ~flags;

	if (in->valnode->type != QI_VAL)
	{
		int			i;

		for (i = 0; i < in->nchild; i++)
			QTNClearFlags(in->child[i], flags);
	}
}
