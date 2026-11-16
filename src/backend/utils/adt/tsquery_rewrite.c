/*-------------------------------------------------------------------------
 *
 * tsquery_rewrite.c
 *	  重构 tsquery 的工具函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsquery_rewrite.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


/*
 * 若 "node" 等于 "ex"，返回 "subs" 的副本。
 * 若 "ex" 匹配 node 子节点的子集，则返回修改后的
 * "node"，其中这些子节点被替换为 "subs" 的副本。
 * 否则原样返回 "node"。
 *
 * 成功修改的节点会设置 QTN_NOCHANGE 位，以免徒劳
 * 地递归进去。
 * 此外，替换时把 *isfind 设为 true。
 */
static QTNode *
findeq(QTNode *node, QTNode *ex, QTNode *subs, bool *isfind)
{
	/* 除非签名和节点类型匹配，否则无法匹配。 */
	if ((node->sign & ex->sign) != ex->sign ||
		node->valnode->type != ex->valnode->type)
		return node;

	/* 同时忽略 NOCHANGE 节点。 */
	if (node->flags & QTN_NOCHANGE)
		return node;

	if (node->valnode->type == QI_OPR)
	{
		/* 须为同一算子。 */
		if (node->valnode->qoperator.oper != ex->valnode->qoperator.oper)
			return node;

		if (node->nchild == ex->nchild)
		{
			/*
			 * 简单情形：子节点数相同，相等即匹配。
			 * （这在子节点先前已排序时是可靠的。）
			 */
			if (QTNEq(node, ex))
			{
				/* 匹配；删除节点，改为返回 subs 副本。 */
				QTNFree(node);
				if (subs)
				{
					node = QTNCopy(subs);
					node->flags |= QTN_NOCHANGE;
				}
				else
					node = NULL;
				*isfind = true;
			}
		}
		else if (node->nchild > ex->nchild && ex->nchild > 0)
		{
			/*
			 * AND 与 OR 可交换/可结合，故应查子节点的子集
			 * 是否匹配。例如 node 是 A|B|C 而 ex 是 B|C，
			 * 那么把 node 在概念上转换为 A|(B|C) 后即匹配
			 * 但这不适用于 NOT 或 PHRASE 节点，因为
			 * 这些节点类型的子节点数是固定的，所以我
			 * 们不会到此。
			 *
			 * 因预期子节点已排序，单趟扫描两个列表
			 * 即可找出所有匹配。
			 */
			bool	   *matched;
			int			nmatched;
			int			i,
						j;

			/* 断言子集规则是成立的 */
			Assert(node->valnode->qoperator.oper == OP_AND ||
				   node->valnode->qoperator.oper == OP_OR);

			/* matched[] 将记录 node 哪些子节点匹配了 */
			matched = (bool *) palloc0(node->nchild * sizeof(bool));
			nmatched = 0;
			i = j = 0;
			while (i < node->nchild && j < ex->nchild)
			{
				int			cmp = QTNodeCompare(node->child[i], ex->child[j]);

				if (cmp == 0)
				{
					/* 匹配 */
					matched[i] = true;
					nmatched++;
					i++, j++;
				}
				else if (cmp < 0)
				{
					/* node->child[i] 无匹配，忽略它 */
					i++;
				}
				else
				{
					/* ex->child[j] 无匹配；可以立即放弃 */
					break;
				}
			}

			if (nmatched == ex->nchild)
			{
				/* 剔除 node 已匹配的子节点 */
				j = 0;
				for (i = 0; i < node->nchild; i++)
				{
					if (matched[i])
						QTNFree(node->child[i]);
					else
						node->child[j++] = node->child[i];
				}

				/* 并改为插入 subs 的副本 */
				if (subs)
				{
					subs = QTNCopy(subs);
					subs->flags |= QTN_NOCHANGE;
					node->child[j++] = subs;
				}

				node->nchild = j;

				/*
				 * 此时可能得到子节点数为 0 或 1 的节点，
				 * 应当把它简化。但这留给调用者
				 * （dofindsubquery）来处理。
				 */

				/*
				 * 重新为节点排序，把新子节点放到正确位置
				 * 这有点勉强：这不会影响 findsubquery
				 * 的后续处理，也不足以让树备好再次搜索
				 * （那需重新展平，而我们不想那样，因为
				 * 丢掉新子节点的 QTN_NOCHANGE 标记）。但要
				 * 结果与回归测试的预期一致，所以这一步
				 * 是必需的。
				 */
				QTNSort(node);

				*isfind = true;
			}

			pfree(matched);
		}
	}
	else
	{
		Assert(node->valnode->type == QI_VAL);

		if (node->valnode->qoperand.valcrc != ex->valnode->qoperand.valcrc)
			return node;
		else if (QTNEq(node, ex))
		{
			QTNFree(node);
			if (subs)
			{
				node = QTNCopy(subs);
				node->flags |= QTN_NOCHANGE;
			}
			else
			{
				node = NULL;
			}
			*isfind = true;
		}
	}

	return node;
}

/*
 * findsubquery() 递归主体：尝试在根节点处把 "ex" 换
 * 为 "subs"；如果失败，就递归地对子节点进
 * 行匹配。
 *
 * 删除替换产生的任何空子树。
 * 在下例中 '5' 被空操作数替换：
 *
 *	  AND		->	  6
 *	 /	 \
 *	5	 OR
 *		/  \
 *	   6	5
 */
static QTNode *
dofindsubquery(QTNode *root, QTNode *ex, QTNode *subs, bool *isfind)
{
	/* 本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	/* 另外，既然有点昂贵，就顺便检查查询取消。 */
	CHECK_FOR_INTERRUPTS();

	/* 在节点本身匹配 */
	root = findeq(root, ex, subs, isfind);

	/* 除非在此匹配，否则考虑子节点处的匹配 */
	if (root && (root->flags & QTN_NOCHANGE) == 0 &&
		root->valnode->type == QI_OPR)
	{
		int			i,
					j = 0;

		/*
		 * 任何被替换成 NULL 的子树都必须从树中剔
		 * 除
		 */
		for (i = 0; i < root->nchild; i++)
		{
			root->child[j] = dofindsubquery(root->child[i], ex, subs, isfind);
			if (root->child[j])
				j++;
		}

		root->nchild = j;

		/*
		 * 如果只剩零个或一个子节点，就把该算子节点
		 * 简化掉。
		 */
		if (root->nchild == 0)
		{
			QTNFree(root);
			root = NULL;
		}
		else if (root->nchild == 1 && root->valnode->qoperator.oper != OP_NOT)
		{
			QTNode	   *nroot = root->child[0];

			pfree(root);
			root = nroot;
		}
	}

	return root;
}

/*
 * 在整个根部的 QTNode 树中把 "ex" 替换为 "subs"。
 *
 * 如果 isfind 非 NULL，则以 *isfind 表明是否做过替换。
 *
 * "root" 与 "ex" 都必须先经过 QTNTernary 与 QTNSort，
 * 以确保匹配可靠。
 */
QTNode *
findsubquery(QTNode *root, QTNode *ex, QTNode *subs, bool *isfind)
{
	bool		DidFind = false;

	root = dofindsubquery(root, ex, subs, &DidFind);

	if (isfind)
		*isfind = DidFind;

	return root;
}

Datum
tsquery_rewrite_query(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY_COPY(0);
	text	   *in = PG_GETARG_TEXT_PP(1);
	TSQuery		rewrited = query;
	MemoryContext outercontext = CurrentMemoryContext;
	MemoryContext oldcontext;
	QTNode	   *tree;
	char	   *buf;
	SPIPlanPtr	plan;
	Portal		portal;
	bool		isnull;

	if (query->size == 0)
	{
		PG_FREE_IF_COPY(in, 1);
		PG_RETURN_POINTER(rewrited);
	}

	tree = QT2QTN(GETQUERY(query), GETOPERAND(query));
	QTNTernary(tree);
	QTNSort(tree);

	buf = text_to_cstring(in);

	SPI_connect();

	if ((plan = SPI_prepare(buf, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", buf);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", buf);

	SPI_cursor_fetch(portal, true, 100);

	if (SPI_tuptable == NULL ||
		SPI_tuptable->tupdesc->natts != 2 ||
		SPI_gettypeid(SPI_tuptable->tupdesc, 1) != TSQUERYOID ||
		SPI_gettypeid(SPI_tuptable->tupdesc, 2) != TSQUERYOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("ts_rewrite query must return two tsquery columns")));

	while (SPI_processed > 0 && tree)
	{
		uint64		i;

		for (i = 0; i < SPI_processed && tree; i++)
		{
			Datum		qdata = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull);
			Datum		sdata;

			if (isnull)
				continue;

			sdata = SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2, &isnull);

			if (!isnull)
			{
				TSQuery		qtex = DatumGetTSQuery(qdata);
				TSQuery		qtsubs = DatumGetTSQuery(sdata);
				QTNode	   *qex,
						   *qsubs = NULL;

				if (qtex->size == 0)
				{
					if (qtex != (TSQuery) DatumGetPointer(qdata))
						pfree(qtex);
					if (qtsubs != (TSQuery) DatumGetPointer(sdata))
						pfree(qtsubs);
					continue;
				}

				qex = QT2QTN(GETQUERY(qtex), GETOPERAND(qtex));

				QTNTernary(qex);
				QTNSort(qex);

				if (qtsubs->size)
					qsubs = QT2QTN(GETQUERY(qtsubs), GETOPERAND(qtsubs));

				oldcontext = MemoryContextSwitchTo(outercontext);
				tree = findsubquery(tree, qex, qsubs, NULL);
				MemoryContextSwitchTo(oldcontext);

				QTNFree(qex);
				if (qtex != (TSQuery) DatumGetPointer(qdata))
					pfree(qtex);
				QTNFree(qsubs);
				if (qtsubs != (TSQuery) DatumGetPointer(sdata))
					pfree(qtsubs);

				if (tree)
				{
					/* 让树准备好进行下一轮 */
					QTNClearFlags(tree, QTN_NOCHANGE);
					QTNTernary(tree);
					QTNSort(tree);
				}
			}
		}

		SPI_freetuptable(SPI_tuptable);
		SPI_cursor_fetch(portal, true, 100);
	}

	SPI_freetuptable(SPI_tuptable);
	SPI_cursor_close(portal);
	SPI_freeplan(plan);
	SPI_finish();

	if (tree)
	{
		QTNBinary(tree);
		rewrited = QTN2QT(tree);
		QTNFree(tree);
		PG_FREE_IF_COPY(query, 0);
	}
	else
	{
		SET_VARSIZE(rewrited, HDRSIZETQ);
		rewrited->size = 0;
	}

	pfree(buf);
	PG_FREE_IF_COPY(in, 1);
	PG_RETURN_POINTER(rewrited);
}

Datum
tsquery_rewrite(PG_FUNCTION_ARGS)
{
	TSQuery		query = PG_GETARG_TSQUERY_COPY(0);
	TSQuery		ex = PG_GETARG_TSQUERY(1);
	TSQuery		subst = PG_GETARG_TSQUERY(2);
	TSQuery		rewrited = query;
	QTNode	   *tree,
			   *qex,
			   *subs = NULL;

	if (query->size == 0 || ex->size == 0)
	{
		PG_FREE_IF_COPY(ex, 1);
		PG_FREE_IF_COPY(subst, 2);
		PG_RETURN_POINTER(rewrited);
	}

	tree = QT2QTN(GETQUERY(query), GETOPERAND(query));
	QTNTernary(tree);
	QTNSort(tree);

	qex = QT2QTN(GETQUERY(ex), GETOPERAND(ex));
	QTNTernary(qex);
	QTNSort(qex);

	if (subst->size)
		subs = QT2QTN(GETQUERY(subst), GETOPERAND(subst));

	tree = findsubquery(tree, qex, subs, NULL);

	QTNFree(qex);
	QTNFree(subs);

	if (!tree)
	{
		SET_VARSIZE(rewrited, HDRSIZETQ);
		rewrited->size = 0;
		PG_FREE_IF_COPY(ex, 1);
		PG_FREE_IF_COPY(subst, 2);
		PG_RETURN_POINTER(rewrited);
	}
	else
	{
		QTNBinary(tree);
		rewrited = QTN2QT(tree);
		QTNFree(tree);
	}

	PG_FREE_IF_COPY(query, 0);
	PG_FREE_IF_COPY(ex, 1);
	PG_FREE_IF_COPY(subst, 2);
	PG_RETURN_POINTER(rewrited);
}
