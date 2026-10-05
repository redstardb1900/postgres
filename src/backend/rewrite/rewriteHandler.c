/*-------------------------------------------------------------------------
 *
 * rewriteHandler.c
 *		查询重写器的主模块。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/rewrite/rewriteHandler.c
 *
 * 说明
 *	  本文件使用的某些术语具有历史性质："retrieve"
 *	  是 PostQUEL 里对应于今天 SELECT 的关键字。"RIR" 表示
 *	  "Retrieve-Instead-Retrieve"，也就是一条 ON SELECT DO INSTEAD SELECT 规则
 *	  （它必须是无条件的，并且每个
 *	  关系上只能存在一条这样的规则）。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSearchCycle.h"
#include "rewrite/rowsecurity.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/* 我们用这些项的列表来检测 RewriteQuery 里的递归 */
typedef struct rewrite_event
{
	Oid			relation;		/* 拥有规则的关系的 OID */
	CmdType		event;			/* 正在触发的规则类型 */
} rewrite_event;

typedef struct acquireLocksOnSubLinks_context
{
	bool		for_execute;	/* AcquireRewriteLocks 的 forExecute 参数 */
} acquireLocksOnSubLinks_context;

typedef struct fireRIRonSubLink_context
{
	List	   *activeRIRs;
	bool		hasRowSecurity;
} fireRIRonSubLink_context;

static bool acquireLocksOnSubLinks(Node *node,
								   acquireLocksOnSubLinks_context *context);
static Query *rewriteRuleAction(Query *parsetree,
								Query *rule_action,
								Node *rule_qual,
								int rt_index,
								CmdType event,
								bool *returning_flag);
static List *adjustJoinTreeList(Query *parsetree, bool removert, int rt_index);
static List *rewriteTargetListIU(List *targetList,
								 CmdType commandType,
								 OverridingKind override,
								 Relation target_relation,
								 RangeTblEntry *values_rte,
								 int values_rte_index,
								 Bitmapset **unused_values_attrnos);
static TargetEntry *process_matched_tle(TargetEntry *src_tle,
										TargetEntry *prior_tle,
										const char *attrName);
static Node *get_assignment_input(Node *node);
static Bitmapset *findDefaultOnlyColumns(RangeTblEntry *rte);
static bool rewriteValuesRTE(Query *parsetree, RangeTblEntry *rte, int rti,
							 Relation target_relation,
							 Bitmapset *unused_cols);
static void rewriteValuesRTEToNulls(Query *parsetree, RangeTblEntry *rte);
static void markQueryForLocking(Query *qry, Node *jtnode,
								LockClauseStrength strength, LockWaitPolicy waitPolicy,
								bool pushedDown);
static List *matchLocks(CmdType event, RuleLock *rulelocks,
						int varno, Query *parsetree, bool *hasUpdate);
static Query *fireRIRrules(Query *parsetree, List *activeRIRs);
static bool view_has_instead_trigger(Relation view, CmdType event);
static Bitmapset *adjust_view_column_set(Bitmapset *cols, List *targetlist);


/*
 * AcquireRewriteLocks -
 *	  获取查询里提到的所有关系上的合适锁。
 *	  这些锁确保在我们重写、规划和执行查询时，
 *	  关系模式不会在我们脚下改变。
 *
 * 注意：这可能会修改查询树，因此调用者通常应当
 * 已经做过 copyObject()，在当前内存上下文里
 * 做出查询树的可写副本。
 *
 * forExecute 表示查询即将被执行。若是这样，
 * 我们将取得 RTE 的 rellockmode 成员所指定的锁模式。
 * 若 forExecute 为假，则对所有关系取得 AccessShareLock。
 * 这种情形适合 ruleutils.c 之类，我们只需要
 * 模式稳定，并不打算实际修改任何关系。
 *
 * forUpdatePushedDown 表示一条下推的 FOR [KEY] UPDATE/SHARE
 * 作用于当前子查询，要求所有关系至少以
 * RowShareLock 打开。在递归的顶层它应当始终为假。
 * 当它为真时，我们调整 RTE 的 rellockmode 成员以反映
 * 更高的锁级别。若 forExecute 为假，则忽略该标志。
 *
 * 本例程的第二个用途是修正 JOIN RTE 对
 * 已删除列的引用（详见下文）。这类 RTEs 被就地修改。
 *
 * 若查询树刚刚由解析器构造，为了效率可以、
 * 也应当跳过这项处理：解析分析已经取得了
 * 我们这里会取得的全部同样的锁，而且解析器从一开始
 * 就会从 JOINs 里略去已删除的列。但只要我们处理的是
 * 比当前命令更早产生的查询树，就必须做这项处理。
 *
 * 关于 JOIN 和已删除的列：虽然解析器从不把
 * 已经删除的列放进 JOIN RTE 的别名 var 列表，
 * 存储的规则里这样的列表仍可能引用已删除的列。
 * （若该列没有在查询的其他地方被显式引用，
 * 依赖机制不会认为规则使用了它，因此不会
 * 阻止删除该列。）为了支持 get_rte_attribute_is_dropped()，
 * 我们把引用已删除列的连接别名 var 替换成空指针。
 *
 * （在 PostgreSQL 8.0 里，我们不做这项处理，而是让
 * get_rte_attribute_is_dropped() 递归去检测连接里的已删除列。
 * 那种做法的性能不幸很糟；尤其是
 * 构造嵌套连接在嵌套深度上是 O(N^2)。）
 */
void
AcquireRewriteLocks(Query *parsetree,
					bool forExecute,
					bool forUpdatePushedDown)
{
	ListCell   *l;
	int			rt_index;
	acquireLocksOnSubLinks_context context;

	context.for_execute = forExecute;

	/*
	 * 首先处理当前查询层次的 RTEs。
	 */
	rt_index = 0;
	foreach(l, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(l);
		Relation	rel;
		LOCKMODE	lockmode;
		List	   *newaliasvars;
		Index		curinputvarno;
		RangeTblEntry *curinputrte;
		ListCell   *ll;

		++rt_index;
		switch (rte->rtekind)
		{
			case RTE_RELATION:

				/*
				 * 为该关系取得适当的锁类型，并且直到
				 * 事务结束才释放。这保护
				 * 重写器、规划器和执行器，使其免于查询中途的
				 * 模式变更。
				 *
				 * 若 forExecute 为假，则忽略 rellockmode，只使用
				 * AccessShareLock。
				 */
				if (!forExecute)
					lockmode = AccessShareLock;
				else if (forUpdatePushedDown)
				{
					/* 提升 RTE 的锁模式，以反映下推的锁 */
					if (rte->rellockmode == AccessShareLock)
						rte->rellockmode = RowShareLock;
					lockmode = rte->rellockmode;
				}
				else
					lockmode = rte->rellockmode;

				rel = table_open(rte->relid, lockmode);

				/*
				 * 既然关系已经打开，就更新 RTE 的 relkind，
				 * 以防自这条规则建立以来它已经改变。
				 */
				rte->relkind = rel->rd_rel->relkind;

				table_close(rel, NoLock);
				break;

			case RTE_JOIN:

				/*
				 * 扫描连接的别名 var 列表，看是否有列已被
				 * 删除，若有则把那些 Var 替换成空
				 * 指针。
				 *
				 * 由于连接只有两个输入，我们可以预期看到
				 * 对同一输入 RTE 的多次引用；省去多次
				 * 重复读取。
				 */
				newaliasvars = NIL;
				curinputvarno = 0;
				curinputrte = NULL;
				foreach(ll, rte->joinaliasvars)
				{
					Var		   *aliasitem = (Var *) lfirst(ll);
					Var		   *aliasvar = aliasitem;

					/* 看穿任何隐式强制转换 */
					aliasvar = (Var *) strip_implicit_coercions((Node *) aliasvar);

					/*
					 * 若列表项不是简单 Var，则它必定
					 * 表示合并后的列，即 USING 列，因此它
					 * 不可能已被删除，因为它在
					 * 连接子句里被引用。（可以设想它也可能已经是空
					 * 指针？那也没问题。）
					 */
					if (aliasvar && IsA(aliasvar, Var))
					{
						/*
						 * 别名列表的元素必须引用
						 * 同一 rtable 里更早的 RTEs，因为那是
						 * 规划器构造它们的顺序。因此我们已经
						 * 处理过被引用的 RTE，所以可以安全地
						 * 对它使用 get_rte_attribute_is_dropped。（在重写或
						 * 规划之后这可能不再成立，但在这里
						 * 可以这样假定。）
						 */
						Assert(aliasvar->varlevelsup == 0);
						if (aliasvar->varno != curinputvarno)
						{
							curinputvarno = aliasvar->varno;
							if (curinputvarno >= rt_index)
								elog(ERROR, "unexpected varno %d in JOIN RTE %d",
									 curinputvarno, rt_index);
							curinputrte = rt_fetch(curinputvarno,
												   parsetree->rtable);
						}
						if (get_rte_attribute_is_dropped(curinputrte,
														 aliasvar->varattno))
						{
							/* 把连接别名项替换成 NULL */
							aliasitem = NULL;
						}
					}
					newaliasvars = lappend(newaliasvars, aliasitem);
				}
				rte->joinaliasvars = newaliasvars;
				break;

			case RTE_SUBQUERY:

				/*
				 * 子查询 RTE 本身没问题，但我们必须
				 * 递归去处理它所表示的子查询。
				 */
				AcquireRewriteLocks(rte->subquery,
									forExecute,
									(forUpdatePushedDown ||
									 get_parse_rowmark(parsetree, rt_index) != NULL));
				break;

			default:
				/* 忽略其他类型的 RTEs */
				break;
		}
	}

	/* 递归进入 WITH 里的子查询 */
	foreach(l, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(l);

		AcquireRewriteLocks((Query *) cte->ctequery, forExecute, false);
	}

	/*
	 * 也递归进入子链接的子查询。但 rtable 和 cteList 里的
	 * 那些我们已经处理过了。
	 */
	if (parsetree->hasSubLinks)
		query_tree_walker(parsetree, acquireLocksOnSubLinks, &context,
						  QTW_IGNORE_RC_SUBQUERIES);
}

/*
 * 为 AcquireRewriteLocks 查找子链接子查询的遍历器
 */
static bool
acquireLocksOnSubLinks(Node *node, acquireLocksOnSubLinks_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* 做我们来这里要做的事 */
		AcquireRewriteLocks((Query *) sub->subselect,
							context->for_execute,
							false);
		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * 不要递归进入 Query 节点，因为 AcquireRewriteLocks 已经
	 * 为我们处理了子查询的子查询。
	 */
	return expression_tree_walker(node, acquireLocksOnSubLinks, context);
}


/*
 * rewriteRuleAction -
 *	  用合适的限定条件（取自
 *	  触发查询）重写规则动作。
 *
 * 输入参数：
 *	parsetree - 原始查询
 *	rule_action - 规则的一个动作（查询）
 *	rule_qual - 规则的 WHERE 条件，无条件时为 NULL
 *	rt_index - 原始查询里结果关系的 RT 下标
 *	event - 规则事件的类型
 * 输出参数：
 *	*returning_flag - 若重写了 rule_action 里的 RETURNING 子句则设为真
 *					（必须初始化为假）
 * 返回值：
 *	重写后的 rule_action 形式
 */
static Query *
rewriteRuleAction(Query *parsetree,
				  Query *rule_action,
				  Node *rule_qual,
				  int rt_index,
				  CmdType event,
				  bool *returning_flag)
{
	int			current_varno,
				new_varno;
	int			rt_length;
	Query	   *sub_action;
	Query	  **sub_action_ptr;
	acquireLocksOnSubLinks_context context;
	ListCell   *lc;

	context.for_execute = true;

	/*
	 * 制作规则动作和限定条件的可修改副本（传进来的是
	 * relcache 里存放的版本；不要去动它们！）。
	 */
	rule_action = copyObject(rule_action);
	rule_qual = copyObject(rule_qual);

	/*
	 * 取得必要的锁，并修正任何已删除的 JOIN RTE 项。
	 */
	AcquireRewriteLocks(rule_action, true, false);
	(void) acquireLocksOnSubLinks(rule_qual, &context);

	current_varno = rt_index;
	rt_length = list_length(parsetree->rtable);
	new_varno = PRS2_NEW_VARNO + rt_length;

	/*
	 * 调整规则动作和限定条件以偏移其 varno，以便把
	 * 它的 rtable 与主语法分析树的 rtable 合并。
	 *
	 * 若规则动作是 INSERT...SELECT，则 OLD/NEW 的 rtable 项
	 * 会在 SELECT 部分里，我们必须修改那一部分，而不是
	 * 顶层的 INSERT（权宜做法！）。
	 */
	sub_action = getInsertSelectQuery(rule_action, &sub_action_ptr);

	OffsetVarNodes((Node *) sub_action, rt_length, 0);
	OffsetVarNodes(rule_qual, rt_length, 0);
	/* 但对 OLD 的引用应当指向原始的 rt_index */
	ChangeVarNodes((Node *) sub_action,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);
	ChangeVarNodes(rule_qual,
				   PRS2_OLD_VARNO + rt_length, rt_index, 0);

	/*
	 * 若规则动作里的子查询 RTEs 含有
	 * 引用当前查询层次的 Var（对 NEW/OLD 的引用），就把它们标成 LATERAL。
	 * 这些确实是 LATERAL 引用，但我们历来不
	 * 要求用户显式用 LATERAL 标记这类子查询。但
	 * 若非 LATERAL 子查询里存在这样的 Var，规划器会报错，
	 * 因此我们必须在这里把事情修好。
	 */
	foreach(lc, sub_action->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_SUBQUERY && !rte->lateral &&
			contain_vars_of_level((Node *) rte->subquery, 1))
			rte->lateral = true;
	}

	/*
	 * 生成由主语法分析树的 rtable 加上
	 * 规则动作的 rtable 组成的展开 rtable；它成为规则
	 * 动作的完整 rtable。我们重写完成后，其中一些项
	 * 可能不再被使用，但全部留在原处，以免必须调整查询的
	 * varnos。已完成的 jointree 里没有引用的 RT 项
	 * 会被规划器忽略，因此不影响查询语义。
	 *
	 * 同时合并 RTEPermissionInfo 列表，以确保所有权限都
	 * 被正确检查。
	 *
	 * 若规则是 INSTEAD，则原始查询根本不会被
	 * 执行，因此必须保留它的 rteperminfos，这样执行器
	 * 才会对其所引用的关系做正确的权限检查。
	 * 这样我们就能检查调用者是否拥有，比如说，对视图的
	 * 插入权限，即使该视图在结果查询里
	 * 完全没有被语义引用。
	 *
	 * 当规则不是 INSTEAD 时，用复制的项所做的权限检查
	 * 会与执行原始查询时的检查重复，但我们不费心
	 * 对那种情形另作处理。
	 *
	 * 注意：因为规划器会破坏性地改动 rtable 和 rteperminfos，
	 * 我们必须确保规则动作的列表彼此独立，并且与主查询的
	 * 列表不共享任何子结构。因此这里对两者都做
	 * 深拷贝。
	 */
	{
		List	   *rtable_tail = sub_action->rtable;
		List	   *perminfos_tail = sub_action->rteperminfos;

		/*
		 * RewriteQuery 依赖这样一个事实：来自原始
		 * 查询的 RT 项出现在展开后 rtable 的开头，因此我们把
		 * 动作原来的表放在列表末尾。
		 */
		sub_action->rtable = copyObject(parsetree->rtable);
		sub_action->rteperminfos = copyObject(parsetree->rteperminfos);
		CombineRangeTables(&sub_action->rtable, &sub_action->rteperminfos,
						   rtable_tail, perminfos_tail);
	}

	/*
	 * 语法分析树的 rtable 里可能曾有一些 SubLinks，那种
	 * 情况下我们最好把 sub_action 标记正确。
	 */
	if (parsetree->hasSubLinks && !sub_action->hasSubLinks)
	{
		foreach(lc, parsetree->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			switch (rte->rtekind)
			{
				case RTE_RELATION:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->tablesample);
					break;
				case RTE_FUNCTION:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->functions);
					break;
				case RTE_TABLEFUNC:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->tablefunc);
					break;
				case RTE_VALUES:
					sub_action->hasSubLinks =
						checkExprHasSubLink((Node *) rte->values_lists);
					break;
				default:
					/* 其他 RTE 类型不含裸表达式 */
					break;
			}
			sub_action->hasSubLinks |=
				checkExprHasSubLink((Node *) rte->securityQuals);
			if (sub_action->hasSubLinks)
				break;			/* 不必继续扫描 rtable */
		}
	}

	/*
	 * 另外，我们可能已经把一些带行安全性条件的 RTEs 吸收进
	 * sub_action。若是这样，就把它标成 hasRowSecurity，不论那些
	 * RTEs 在我们重写完成后是否还会被引用。（注意：目前
	 * 这是空操作，因为行安全性条件要到稍后才加入，但
	 * 无论如何现在就这样做，似乎是不错的前瞻。）
	 */
	sub_action->hasRowSecurity |= parsetree->hasRowSecurity;

	/*
	 * 每个规则动作的 jointree 应当是主语法分析树的 jointree
	 * 加上该规则的 jointree，但通常不含我们正在替换的
	 * 原始 rtindex（若存在；INSERT 时不会有）。注意
	 * 若规则动作引用 OLD，它的 jointree 会加入一条
	 * 对 rt_index 的引用。若规则动作不引用 OLD，但
	 * rule_qual 或用户查询的限定条件引用了，则需要在
	 * jointree 里保留原始 rtindex，以便为限定条件提供数据。但
	 * 我们不希望原始 rtindex 被连接两次，因此若规则动作
	 * 提到了它，就不要保留。
	 *
	 * 同上，动作的 jointree 不得与主
	 * 语法分析树共享子结构。
	 */
	if (sub_action->commandType != CMD_UTILITY)
	{
		bool		keeporig;
		List	   *newjointree;

		Assert(sub_action->jointree != NULL);
		keeporig = (!rangeTableEntry_used((Node *) sub_action->jointree,
										  rt_index, 0)) &&
			(rangeTableEntry_used(rule_qual, rt_index, 0) ||
			 rangeTableEntry_used(parsetree->jointree->quals, rt_index, 0));
		newjointree = adjustJoinTreeList(parsetree, !keeporig, rt_index);
		if (newjointree != NIL)
		{
			/*
			 * 若 sub_action 是集合操作，摆弄它的 jointree 完全
			 * 没有用，因为那个 jointree 是哑的。（也许将来
			 * 我们可以把连接和限定条件下推到集合操作的
			 * 成员语句？）
			 */
			if (sub_action->setOperations != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("conditional UNION/INTERSECT/EXCEPT statements are not implemented")));

			sub_action->jointree->fromlist =
				list_concat(newjointree, sub_action->jointree->fromlist);

			/*
			 * newjointree 里可能曾有一些 SubLinks，那种
			 * 情况下我们最好把 sub_action 标记正确。
			 */
			if (parsetree->hasSubLinks && !sub_action->hasSubLinks)
				sub_action->hasSubLinks =
					checkExprHasSubLink((Node *) newjointree);
		}
	}

	/*
	 * 若原始查询有任何 CTEs，就把它们复制进规则动作。但
	 * 实用语句动作不需要它们。
	 */
	if (parsetree->cteList != NIL && sub_action->commandType != CMD_UTILITY)
	{
		/*
		 * 恼人的实现限制：因为 CTE 在 cteList 里按
		 * 名称标识，若它与规则动作里的任何 CTE 同名，
		 * 我们没法把原始查询里的 CTE 合并进来。
		 *
		 * 这也许可以用某种内部生成的
		 * ID 代替名称，来把 CTE RTEs 链到它们的 CTEs，从而修好。
		 * 不过，反编译结果会相当令人困惑；注意下面
		 * hasRecursive 标志的合并，它可能改变这类
		 * 重名 CTE 的表面语义。
		 */
		foreach(lc, parsetree->cteList)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
			ListCell   *lc2;

			foreach(lc2, sub_action->cteList)
			{
				CommonTableExpr *cte2 = (CommonTableExpr *) lfirst(lc2);

				if (strcmp(cte->ctename, cte2->ctename) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("WITH query name \"%s\" appears in both a rule action and the query being rewritten",
									cte->ctename)));
			}
		}

		/* 可以了，合并 CTE 列表是安全的 */
		sub_action->cteList = list_concat(sub_action->cteList,
										  copyObject(parsetree->cteList));
		/* ……并且别忘了相关的标志 */
		sub_action->hasRecursive |= parsetree->hasRecursive;
		sub_action->hasModifyingCTE |= parsetree->hasModifyingCTE;

		/*
		 * 若 rule_action 与 sub_action 不同（即规则动作
		 * 是 INSERT...SELECT），则我们可能刚刚加入了一些
		 * 不在顶层查询的数据修改 CTE。解析器
		 * 不允许这样，我们在这里也不得生成
		 * 这样的树，因此报错。
		 *
		 * 可以设想，把原始查询的 CTEs 挂到
		 * rule_action 而不是 sub_action 上，就能支持这类情形。但要那样做，
		 * 我们必须递增从原始查询复制来的 RTEs 和 SubLinks 里的
		 * ctelevelsup。目前看来不值得费这个事。
		 */
		if (sub_action->hasModifyingCTE && rule_action != sub_action)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT ... SELECT rule actions are not supported for queries having data-modifying statements in WITH")));
	}

	/*
	 * 事件限定条件迫使复制语法分析树并拆成两个
	 * 查询，一个带 rule_qual，一个带 NOT rule_qual。同时把用户查询的限定条件
	 * 加到规则动作上
	 */
	AddQual(sub_action, rule_qual);

	AddQual(sub_action, parsetree->jointree->quals);

	/*
	 * 用插入或更新里相应成员名的目标列表项
	 * 右部重写 new.attribute。
	 *
	 * KLUGE ALERT：由于 ReplaceVarsFromTargetList 返回的是变异副本，我们
	 * 不能只把它用在 sub_action 上；还必须记得更新
	 * rule_action 内部的子链接。
	 */
	if ((event == CMD_INSERT || event == CMD_UPDATE) &&
		sub_action->commandType != CMD_UTILITY)
	{
		sub_action = (Query *)
			ReplaceVarsFromTargetList((Node *) sub_action,
									  new_varno,
									  0,
									  rt_fetch(new_varno, sub_action->rtable),
									  parsetree->targetList,
									  (event == CMD_UPDATE) ?
									  REPLACEVARS_CHANGE_VARNO :
									  REPLACEVARS_SUBSTITUTE_NULL,
									  current_varno,
									  NULL);
		if (sub_action_ptr)
			*sub_action_ptr = sub_action;
		else
			rule_action = sub_action;
	}

	/*
	 * 若 rule_action 有 RETURNING 子句，则当触发查询
	 * 没有 RETURNING 子句时把它丢掉，否则重写它，使它发出
	 * 触发查询的 RETURNING 子句所要求的内容。若有
	 * 不止一条规则带 RETURNING 子句，则报错。
	 */
	if (!parsetree->returningList)
		rule_action->returningList = NIL;
	else if (rule_action->returningList)
	{
		if (*returning_flag)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot have RETURNING lists in multiple rules")));
		*returning_flag = true;
		rule_action->returningList = (List *)
			ReplaceVarsFromTargetList((Node *) parsetree->returningList,
									  parsetree->resultRelation,
									  0,
									  rt_fetch(parsetree->resultRelation,
											   parsetree->rtable),
									  rule_action->returningList,
									  REPLACEVARS_REPORT_ERROR,
									  0,
									  &rule_action->hasSubLinks);

		/*
		 * 语法分析树的 returningList 里可能曾有一些 SubLinks，
		 * 那种情况下我们最好把 rule_action 标记正确。
		 */
		if (parsetree->hasSubLinks && !rule_action->hasSubLinks)
			rule_action->hasSubLinks =
				checkExprHasSubLink((Node *) rule_action->returningList);
	}

	return rule_action;
}

/*
 * 复制查询的 jointree 列表，并可选地尝试去掉
 * 作为顶层连接项出现的给定 rt_index（我们不在
 * 连接项内部查找它；这是可以的，因为我们只预期找到
 * 它作为 UPDATE 或 DELETE 的目标关系，那会在连接的
 * 顶层）。返回修改后的 jointree 列表：这是一份独立副本，
 * 与原始节点不共享任何节点。
 */
static List *
adjustJoinTreeList(Query *parsetree, bool removert, int rt_index)
{
	List	   *newjointree = copyObject(parsetree->jointree->fromlist);
	ListCell   *l;

	if (removert)
	{
		foreach(l, newjointree)
		{
			RangeTblRef *rtr = lfirst(l);

			if (IsA(rtr, RangeTblRef) &&
				rtr->rtindex == rt_index)
			{
				newjointree = foreach_delete_current(newjointree, l);
				break;
			}
		}
	}
	return newjointree;
}


/*
 * rewriteTargetListIU - 把 INSERT/UPDATE 目标列表重写成标准形式。
 *
 * 它负有下列职责：
 *
 * 1. 对 INSERT，为任何有默认值且在给定目标列表里
 * 未被赋值的属性加入目标列表项，以计算默认值。
 * （不过，我们不为没有默认值的属性插入任何东西。
 * 规划器稍后会为它们插入 NULLs，但没有理由用额外的
 * 目标列表节点拖慢重写器的处理。）另外，对 INSERT
 * 和 UPDATE，都把显式的 DEFAULT 说明替换成列默认
 * 表达式。
 *
 * 2. 合并同一目标属性的多个项，若不能合并则报错。
 * 多个项只允许用于对数组或记录成员的一部分做
 * INSERT/UPDATE，例如
 *			UPDATE table SET foo[2] = 42, foo[4] = 43;
 * 我们可以把这类操作合并成一次赋值。本质上，
 * 这种情形下我们想产生的表达式类似于
 *		foo = array_set_element(array_set_element(foo, 2, 42), 4, 43)。
 *
 * 3. 把目标列表排成标准顺序：非废成员按 resno 排序，
 * 然后是废成员（这些没有特定顺序）。
 *
 * 我们必须在触发重写规则之前做第 1 和第 2 项，否则被重写的
 * 对 NEW.foo 的引用会产生错误或不完整的结果。第 3
 * 项对重写并非必需，但对规划器有帮助，而且我们
 * 在处理其他项时基本上可以免费完成它。
 *
 * 若 values_rte 非 NULL（即我们在用 VALUES RTE 的值做多行
 * INSERT），我们把 VALUES RTE 里任何未使用列的
 * 属性号填进 *unused_values_attrnos。标识列和生成列
 * 在其目标列表项被替换成生成表达式时会发生这种情况
 * （若使用了 INSERT ... OVERRIDING USER VALUE，或者
 * 所有要插入的值都是 DEFAULT）。rewriteValuesRTE() 需要
 * 这个信息来处理未使用列里的任何 DEFAULT 项。
 * 调用者必须已经把 *unused_values_attrnos 初始化为 NULL。
 */
static List *
rewriteTargetListIU(List *targetList,
					CmdType commandType,
					OverridingKind override,
					Relation target_relation,
					RangeTblEntry *values_rte,
					int values_rte_index,
					Bitmapset **unused_values_attrnos)
{
	TargetEntry **new_tles;
	List	   *new_tlist = NIL;
	List	   *junk_tlist = NIL;
	Form_pg_attribute att_tup;
	int			attrno,
				next_junk_attrno,
				numattrs;
	ListCell   *temp;
	Bitmapset  *default_only_cols = NULL;

	/*
	 * 我们处理普通（非废）属性的做法是把输入目标列表
	 * 扫描一次，把 TLEs 转进一个数组，再扫描该数组来
	 * 构造输出目标列表。这样可以避免属性很多时的
	 * O(N^2) 行为。
	 *
	 * 废属性在同一次目标列表扫描中被扔进单独的列表，
	 * 然后追加到重建后的目标列表。
	 */
	numattrs = RelationGetNumberOfAttributes(target_relation);
	new_tles = (TargetEntry **) palloc0(numattrs * sizeof(TargetEntry *));
	next_junk_attrno = numattrs + 1;

	foreach(temp, targetList)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(temp);

		if (!old_tle->resjunk)
		{
			/* 普通属性：把它收进 new_tles[] */
			attrno = old_tle->resno;
			if (attrno < 1 || attrno > numattrs)
				elog(ERROR, "bogus resno %d in targetlist", attrno);
			att_tup = TupleDescAttr(target_relation->rd_att, attrno - 1);

			/* 我们可以（也必须）忽略已删除的属性 */
			if (att_tup->attisdropped)
				continue;

			/* 与先前对同一属性的赋值合并 */
			new_tles[attrno - 1] =
				process_matched_tle(old_tle,
									new_tles[attrno - 1],
									NameStr(att_tup->attname));
		}
		else
		{
			/*
			 * 把所有 resjunk 目标列表项复制到 junk_tlist，并给它们
			 * 分配高于最后一个真实 resno 的 resno。
			 *
			 * 典型的废项包括 ORDER BY 或 GROUP BY 表达式
			 * （这些在 INSERT 或 UPDATE 里真的可能出现吗？）、系统
			 * 属性引用，等等。
			 */

			/* 把 resno 弄对，但不要做不必要的复制 */
			if (old_tle->resno != next_junk_attrno)
			{
				old_tle = flatCopyTargetEntry(old_tle);
				old_tle->resno = next_junk_attrno;
			}
			junk_tlist = lappend(junk_tlist, old_tle);
			next_junk_attrno++;
		}
	}

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		TargetEntry *new_tle = new_tles[attrno - 1];
		bool		apply_default;

		att_tup = TupleDescAttr(target_relation->rd_att, attrno - 1);

		/* 我们可以（也必须）忽略已删除的属性 */
		if (att_tup->attisdropped)
			continue;

		/*
		 * 处理需要插入默认表达式的两种情形：
		 * 这是 INSERT，且该列没有目标列表项，或者
		 * 目标列表项是 DEFAULT 占位节点。
		 */
		apply_default = ((new_tle == NULL && commandType == CMD_INSERT) ||
						 (new_tle && new_tle->expr && IsA(new_tle->expr, SetToDefault)));

		if (commandType == CMD_INSERT)
		{
			int			values_attrno = 0;

			/* 来自 VALUES RTE 的值的源属性号 */
			if (values_rte && new_tle && IsA(new_tle->expr, Var))
			{
				Var		   *var = (Var *) new_tle->expr;

				if (var->varno == values_rte_index)
					values_attrno = var->varattno;
			}

			/*
			 * 只能向 GENERATED ALWAYS 标识列插入 DEFAULT，
			 * 除非指定了 OVERRIDING USER VALUE，或者指定了
			 * OVERRIDING SYSTEM VALUE。
			 */
			if (att_tup->attidentity == ATTRIBUTE_IDENTITY_ALWAYS && !apply_default)
			{
				if (override == OVERRIDING_USER_VALUE)
					apply_default = true;
				else if (override != OVERRIDING_SYSTEM_VALUE)
				{
					/*
					 * 若该列的值来自 VALUES RTE，则测试
					 * 它是否只含 SetToDefault 项。由于
					 * VALUES 列表可能相当大，我们安排只
					 * 扫描它一次。
					 */
					if (values_attrno != 0)
					{
						if (default_only_cols == NULL)
							default_only_cols = findDefaultOnlyColumns(values_rte);

						if (bms_is_member(values_attrno, default_only_cols))
							apply_default = true;
					}

					if (!apply_default)
						ereport(ERROR,
								(errcode(ERRCODE_GENERATED_ALWAYS),
								 errmsg("cannot insert a non-DEFAULT value into column \"%s\"",
										NameStr(att_tup->attname)),
								 errdetail("Column \"%s\" is an identity column defined as GENERATED ALWAYS.",
										   NameStr(att_tup->attname)),
								 errhint("Use OVERRIDING SYSTEM VALUE to override.")));
				}
			}

			/*
			 * 虽然允许向 GENERATED BY DEFAULT 标识列插入，
			 * 若指定了 OVERRIDING USER VALUE，则应用
			 * 默认值。
			 */
			if (att_tup->attidentity == ATTRIBUTE_IDENTITY_BY_DEFAULT &&
				override == OVERRIDING_USER_VALUE)
				apply_default = true;

			/*
			 * 只能向生成列插入 DEFAULT，不论有没有任何
			 * OVERRIDING 子句。
			 */
			if (att_tup->attgenerated && !apply_default)
			{
				/*
				 * 若该列的值来自 VALUES RTE，则测试
				 * 它是否只含 SetToDefault 项，同上。
				 */
				if (values_attrno != 0)
				{
					if (default_only_cols == NULL)
						default_only_cols = findDefaultOnlyColumns(values_rte);

					if (bms_is_member(values_attrno, default_only_cols))
						apply_default = true;
				}

				if (!apply_default)
					ereport(ERROR,
							(errcode(ERRCODE_GENERATED_ALWAYS),
							 errmsg("cannot insert a non-DEFAULT value into column \"%s\"",
									NameStr(att_tup->attname)),
							 errdetail("Column \"%s\" is a generated column.",
									   NameStr(att_tup->attname))));
			}

			/*
			 * 对来自 VALUES RTE 的 INSERT，返回任何将不再被使用的
			 * VALUES 列的属性号（因为目标列表项被
			 * 默认表达式替换了）。
			 */
			if (values_attrno != 0 && apply_default && unused_values_attrnos)
				*unused_values_attrnos = bms_add_member(*unused_values_attrnos,
														values_attrno);
		}

		/*
		 * 对标识列和生成列的更新遵循与上面相同的规则，
		 * 只是 UPDATE 不接受 OVERRIDING 子句。另外，
		 * 源不可能是 VALUES RTE，因此不必考虑那个。
		 */
		if (commandType == CMD_UPDATE)
		{
			if (att_tup->attidentity == ATTRIBUTE_IDENTITY_ALWAYS &&
				new_tle && !apply_default)
				ereport(ERROR,
						(errcode(ERRCODE_GENERATED_ALWAYS),
						 errmsg("column \"%s\" can only be updated to DEFAULT",
								NameStr(att_tup->attname)),
						 errdetail("Column \"%s\" is an identity column defined as GENERATED ALWAYS.",
								   NameStr(att_tup->attname))));

			if (att_tup->attgenerated && new_tle && !apply_default)
				ereport(ERROR,
						(errcode(ERRCODE_GENERATED_ALWAYS),
						 errmsg("column \"%s\" can only be updated to DEFAULT",
								NameStr(att_tup->attname)),
						 errdetail("Column \"%s\" is a generated column.",
								   NameStr(att_tup->attname))));
		}

		if (att_tup->attgenerated)
		{
			/*
			 * 存储的生成列将在执行器里修正
			 */
			new_tle = NULL;
		}
		else if (apply_default)
		{
			Node	   *new_expr;

			new_expr = build_column_default(target_relation, attrno);

			/*
			 * 若没有默认值（即默认值实际上是 NULL），在
			 * INSERT 情形下可以省略该目标列表项，因为规划器
			 * 自己可以插入 NULL，再在这个项上花费
			 * 更多重写器周期没有意义。但在 UPDATE 情形下
			 * 我们必须显式把该列设为 NULL。
			 */
			if (!new_expr)
			{
				if (commandType == CMD_INSERT)
					new_tle = NULL;
				else
					new_expr = coerce_null_to_domain(att_tup->atttypid,
													 att_tup->atttypmod,
													 att_tup->attcollation,
													 att_tup->attlen,
													 att_tup->attbyval);
			}

			if (new_expr)
				new_tle = makeTargetEntry((Expr *) new_expr,
										  attrno,
										  pstrdup(NameStr(att_tup->attname)),
										  false);
		}

		if (new_tle)
			new_tlist = lappend(new_tlist, new_tle);
	}

	pfree(new_tles);

	return list_concat(new_tlist, junk_tlist);
}


/*
 * 把原始目标列表里匹配的 TLE 转换成正确的新 TLE。
 *
 * 本例程检测并处理对同一目标
 * 属性的多次赋值。（属性名只用于错误信息。）
 */
static TargetEntry *
process_matched_tle(TargetEntry *src_tle,
					TargetEntry *prior_tle,
					const char *attrName)
{
	TargetEntry *result;
	CoerceToDomain *coerce_expr = NULL;
	Node	   *src_expr;
	Node	   *prior_expr;
	Node	   *src_input;
	Node	   *prior_input;
	Node	   *priorbottom;
	Node	   *newexpr;

	if (prior_tle == NULL)
	{
		/*
		 * 这是对该属性的第一次赋值的普通情形。
		 */
		return src_tle;
	}

	/*----------
	 * Multiple assignments to same attribute.  Allow only if all are
	 * FieldStore or SubscriptingRef assignment operations.  This is a bit
	 * tricky because what we may actually be looking at is a nest of
	 * such nodes; consider
	 *		UPDATE tab SET col.fld1.subfld1 = x, col.fld2.subfld2 = y
	 * The two expressions produced by the parser will look like
	 *		FieldStore(col, fld1, FieldStore(placeholder, subfld1, x))
	 *		FieldStore(col, fld2, FieldStore(placeholder, subfld2, y))
	 * However, we can ignore the substructure and just consider the top
	 * FieldStore or SubscriptingRef from each assignment, because it works to
	 * combine these as
	 *		FieldStore(FieldStore(col, fld1,
	 *							  FieldStore(placeholder, subfld1, x)),
	 *				   fld2, FieldStore(placeholder, subfld2, y))
	 * Note the leftmost expression goes on the inside so that the
	 * assignments appear to occur left-to-right.
	 *
	 * For FieldStore, instead of nesting we can generate a single
	 * FieldStore with multiple target fields.  We must nest when
	 * SubscriptingRefs are involved though.
	 *
	 * As a further complication, the destination column might be a domain,
	 * resulting in each assignment containing a CoerceToDomain node over a
	 * FieldStore or SubscriptingRef.  These should have matching target
	 * domains, so we strip them and reconstitute a single CoerceToDomain over
	 * the combined FieldStore/SubscriptingRef nodes.  (Notice that this has
	 * the result that the domain's checks are applied only after we do all
	 * the field or element updates, not after each one.  This is desirable.)
	 *----------
	 */
	src_expr = (Node *) src_tle->expr;
	prior_expr = (Node *) prior_tle->expr;

	if (src_expr && IsA(src_expr, CoerceToDomain) &&
		prior_expr && IsA(prior_expr, CoerceToDomain) &&
		((CoerceToDomain *) src_expr)->resulttype ==
		((CoerceToDomain *) prior_expr)->resulttype)
	{
		/* 我们不检查就假定 resulttypmod/resultcollid 匹配 */
		coerce_expr = (CoerceToDomain *) src_expr;
		src_expr = (Node *) ((CoerceToDomain *) src_expr)->arg;
		prior_expr = (Node *) ((CoerceToDomain *) prior_expr)->arg;
	}

	src_input = get_assignment_input(src_expr);
	prior_input = get_assignment_input(prior_expr);
	if (src_input == NULL ||
		prior_input == NULL ||
		exprType(src_expr) != exprType(prior_expr))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("multiple assignments to same column \"%s\"",
						attrName)));

	/*
	 * 若这样做不止一次，先前的 TLE 可能是一层嵌套赋值。
	 */
	priorbottom = prior_input;
	for (;;)
	{
		Node	   *newbottom = get_assignment_input(priorbottom);

		if (newbottom == NULL)
			break;				/* 找到了原来的 Var 引用 */
		priorbottom = newbottom;
	}
	if (!equal(priorbottom, src_input))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("multiple assignments to same column \"%s\"",
						attrName)));

	/*
	 * 看起来可以把它们嵌套起来。
	 */
	if (IsA(src_expr, FieldStore))
	{
		FieldStore *fstore = makeNode(FieldStore);

		if (IsA(prior_expr, FieldStore))
		{
			/* 把两者合并 */
			memcpy(fstore, prior_expr, sizeof(FieldStore));
			fstore->newvals =
				list_concat_copy(((FieldStore *) prior_expr)->newvals,
								 ((FieldStore *) src_expr)->newvals);
			fstore->fieldnums =
				list_concat_copy(((FieldStore *) prior_expr)->fieldnums,
								 ((FieldStore *) src_expr)->fieldnums);
		}
		else
		{
			/* 一般情形，直接嵌套 */
			memcpy(fstore, src_expr, sizeof(FieldStore));
			fstore->arg = (Expr *) prior_expr;
		}
		newexpr = (Node *) fstore;
	}
	else if (IsA(src_expr, SubscriptingRef))
	{
		SubscriptingRef *sbsref = makeNode(SubscriptingRef);

		memcpy(sbsref, src_expr, sizeof(SubscriptingRef));
		sbsref->refexpr = (Expr *) prior_expr;
		newexpr = (Node *) sbsref;
	}
	else
	{
		elog(ERROR, "cannot happen");
		newexpr = NULL;
	}

	if (coerce_expr)
	{
		/* 把 CoerceToDomain 放回去 */
		CoerceToDomain *newcoerce = makeNode(CoerceToDomain);

		memcpy(newcoerce, coerce_expr, sizeof(CoerceToDomain));
		newcoerce->arg = (Expr *) newexpr;
		newexpr = (Node *) newcoerce;
	}

	result = flatCopyTargetEntry(src_tle);
	result->expr = (Expr *) newexpr;
	return result;
}

/*
 * 若节点是赋值节点，返回它的输入；否则返回 NULL
 */
static Node *
get_assignment_input(Node *node)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, FieldStore))
	{
		FieldStore *fstore = (FieldStore *) node;

		return (Node *) fstore->arg;
	}
	else if (IsA(node, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) node;

		if (sbsref->refassgnexpr == NULL)
			return NULL;

		return (Node *) sbsref->refexpr;
	}

	return NULL;
}

/*
 * 为某列的默认值构造表达式树。
 *
 * 若没有默认值，则返回 NULL。
 */
Node *
build_column_default(Relation rel, int attrno)
{
	TupleDesc	rd_att = rel->rd_att;
	Form_pg_attribute att_tup = TupleDescAttr(rd_att, attrno - 1);
	Oid			atttype = att_tup->atttypid;
	int32		atttypmod = att_tup->atttypmod;
	Node	   *expr = NULL;
	Oid			exprtype;

	if (att_tup->attidentity)
	{
		NextValueExpr *nve = makeNode(NextValueExpr);

		nve->seqid = getIdentitySequence(RelationGetRelid(rel), attrno, false);
		nve->typeId = att_tup->atttypid;

		return (Node *) nve;
	}

	/*
	 * 若关系对该列有默认值，就取出那个表达式。
	 */
	if (att_tup->atthasdef)
	{
		if (rd_att->constr && rd_att->constr->num_defval > 0)
		{
			AttrDefault *defval = rd_att->constr->defval;
			int			ndef = rd_att->constr->num_defval;

			while (--ndef >= 0)
			{
				if (attrno == defval[ndef].adnum)
				{
					/* 找到了，把字符串表示转换成节点树。 */
					expr = stringToNode(defval[ndef].adbin);
					break;
				}
			}
		}
		if (expr == NULL)
			elog(ERROR, "default expression not found for attribute %d of relation \"%s\"",
				 attrno, RelationGetRelationName(rel));
	}

	/*
	 * 没有按列的默认值，因此查找类型本身的默认值。但
	 * 生成列除外。
	 */
	if (expr == NULL && !att_tup->attgenerated)
		expr = get_typdefault(atttype);

	if (expr == NULL)
		return NULL;			/* 任何地方都没有默认值 */

	/*
	 * 确保该值被强制转换成目标列类型；这
	 * 通常已经成立，但似乎有一些涉及域默认值的
	 * 边角情形可能不成立。这应当与
	 * 解析器对非默认表达式的处理一致，见
	 * transformAssignedExpr()。
	 */
	exprtype = exprType(expr);

	expr = coerce_to_target_type(NULL,	/* 这里没有 UNKNOWN 参数 */
								 expr, exprtype,
								 atttype, atttypmod,
								 COERCION_ASSIGNMENT,
								 COERCE_IMPLICIT_CAST,
								 -1);
	if (expr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" is of type %s"
						" but default expression is of type %s",
						NameStr(att_tup->attname),
						format_type_be(atttype),
						format_type_be(exprtype)),
				 errhint("You will need to rewrite or cast the expression.")));

	return expr;
}


/* VALUES RTE 里是否含有任何 SetToDefault 项？ */
static bool
searchForDefault(RangeTblEntry *rte)
{
	ListCell   *lc;

	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
				return true;
		}
	}
	return false;
}


/*
 * 在 VALUES RTE 里搜索只含 SetToDefault 项的列，
 * 返回一个 Bitmapset，其中含有任何这类列的属性号。
 */
static Bitmapset *
findDefaultOnlyColumns(RangeTblEntry *rte)
{
	Bitmapset  *default_only_cols = NULL;
	ListCell   *lc;

	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		ListCell   *lc2;
		int			i;

		if (default_only_cols == NULL)
		{
			/* 用第一行填充初始结果 bitmap */
			i = 0;
			foreach(lc2, sublist)
			{
				Node	   *col = (Node *) lfirst(lc2);

				i++;
				if (IsA(col, SetToDefault))
					default_only_cols = bms_add_member(default_only_cols, i);
			}
		}
		else
		{
			/* 用下一行更新结果 bitmap */
			i = 0;
			foreach(lc2, sublist)
			{
				Node	   *col = (Node *) lfirst(lc2);

				i++;
				if (!IsA(col, SetToDefault))
					default_only_cols = bms_del_member(default_only_cols, i);
			}
		}

		/*
		 * 若迄今读到的行里没有哪一列只含 DEFAULT 项，
		 * 我们就完成了。
		 */
		if (bms_is_empty(default_only_cols))
			break;
	}

	return default_only_cols;
}


/*
 * 处理带 VALUES RTE 的 INSERT ... VALUES（即多个 VALUES
 * 列表）时，必须把 VALUES 列表里的任何 DEFAULT 项替换成
 * 适当的默认表达式。目标列表重写的其他方面
 * 只需应用于查询本身的目标列表。
 *
 * 对自动可更新视图，VALUES 列表里的每个 DEFAULT 项
 * 若视图有默认值，就替换成视图的默认值。否则保持
 * 原样，以便稍后改用底层基关系的默认值
 * （当我们把查询重写成引用基关系而不是视图之后，
 * 再递归到这里时）。
 *
 * 对其他类型的关系，包括由规则和触发器可更新的视图，
 * 所有 DEFAULT 项都会被替换；若目标关系没有
 * 默认值，就把该值显式设为 NULL。
 *
 * 另外，若在 unused_cols 提到的列里发现 DEFAULT 项，
 * 就把它显式设为 NULL。VALUES RTE 里那些列会发生这种情况：
 * 它们对应的目标列表项已经被替换成
 * 关系的默认表达式，因此那些列里的任何值
 * 不再被使用。标识列和生成列可能如此
 * （若使用了 INSERT ... OVERRIDING USER VALUE，或者所有
 * 要插入的值都是 DEFAULT）。原则上我们可以把这样的列里的
 * 所有项都替换成 NULL，不论是否为 DEFAULT；但似乎
 * 不值得费这个事。
 *
 * 注意，我们可能有下标或成员赋值的目标列表项，
 * 以及若已为自动可更新视图递归到这里、来自已替换 DEFAULT 项的
 * 更复杂表达式。不过，除了上面所述的未使用列，
 * 这类项应当不可能被赋上 DEFAULTs；我们应当只需要替换
 * 那些含有引用 VALUES RTE 的简单 Var 的目标列表项里的
 * DEFAULT 项，或者目标列表不再引用的那些
 * 项。
 *
 * 若所有 DEFAULT 项都被替换则返回真，若有些
 * 保持原样则返回假。
 */
static bool
rewriteValuesRTE(Query *parsetree, RangeTblEntry *rte, int rti,
				 Relation target_relation,
				 Bitmapset *unused_cols)
{
	List	   *newValues;
	ListCell   *lc;
	bool		isAutoUpdatableView;
	bool		allReplaced;
	int			numattrs;
	int		   *attrnos;

	/* 下面的步骤对非 INSERT 查询没有意义 */
	Assert(parsetree->commandType == CMD_INSERT);
	Assert(rte->rtekind == RTE_VALUES);

	/*
	 * 在很大的 VALUES 列表里重建所有列表是相当昂贵的，
	 * 而且若没有任何 DEFAULT
	 * 占位符，就是浪费时间。因此先扫描看有没有。
	 */
	if (!searchForDefault(rte))
		return true;			/* 无事可做 */

	/*
	 * 扫描目标列表，找引用 VALUES RTE 的项，并记下
	 * 目标属性。如上所述，我们应当只需要对
	 * 含简单 Var 的目标列表项这样做：VALUES RTE 里的其他东西
	 * 不应当含有 DEFAULT 项（未使用的
	 * 列可能除外），若真出现了我们就报错。
	 */
	numattrs = list_length(linitial(rte->values_lists));
	attrnos = (int *) palloc0(numattrs * sizeof(int));

	foreach(lc, parsetree->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (IsA(tle->expr, Var))
		{
			Var		   *var = (Var *) tle->expr;

			if (var->varno == rti)
			{
				int			attrno = var->varattno;

				Assert(attrno >= 1 && attrno <= numattrs);
				attrnos[attrno - 1] = tle->resno;
			}
		}
	}

	/*
	 * 检查目标关系是否为自动可更新视图，那种情况下
	 * 未解析的默认值会保持原样，而不会被设成
	 * NULL。
	 */
	isAutoUpdatableView = false;
	if (target_relation->rd_rel->relkind == RELKIND_VIEW &&
		!view_has_instead_trigger(target_relation, CMD_INSERT))
	{
		List	   *locks;
		bool		hasUpdate;
		bool		found;
		ListCell   *l;

		/* 查找无条件的 DO INSTEAD 规则 */
		locks = matchLocks(CMD_INSERT, target_relation->rd_rules,
						   parsetree->resultRelation, parsetree, &hasUpdate);

		found = false;
		foreach(l, locks)
		{
			RewriteRule *rule_lock = (RewriteRule *) lfirst(l);

			if (rule_lock->isInstead &&
				rule_lock->qual == NULL)
			{
				found = true;
				break;
			}
		}

		/*
		 * 若没有找到无条件的 DO INSTEAD 规则，就假定该
		 * 视图是自动可更新的。若不是，rewriteTargetView()
		 * 会报错。
		 */
		if (!found)
			isAutoUpdatableView = true;
	}

	newValues = NIL;
	allReplaced = true;
	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newList = NIL;
		ListCell   *lc2;
		int			i;

		Assert(list_length(sublist) == numattrs);

		i = 0;
		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);
			int			attrno = attrnos[i++];

			if (IsA(col, SetToDefault))
			{
				Form_pg_attribute att_tup;
				Node	   *new_expr;

				/*
				 * 若该列未被使用，就把 DEFAULT 替换成
				 * NULL（这种情况下 attrno 会是 0，因为目标列表
				 * 项已被默认表达式替换）。
				 */
				if (bms_is_member(i, unused_cols))
				{
					SetToDefault *def = (SetToDefault *) col;

					newList = lappend(newList,
									  makeNullConst(def->typeId,
													def->typeMod,
													def->collation));
					continue;
				}

				if (attrno == 0)
					elog(ERROR, "cannot set value in column %d to DEFAULT", i);
				Assert(attrno > 0 && attrno <= target_relation->rd_att->natts);
				att_tup = TupleDescAttr(target_relation->rd_att, attrno - 1);

				if (!att_tup->attisdropped)
					new_expr = build_column_default(target_relation, attrno);
				else
					new_expr = NULL;	/* 若已删除则强制为 NULL */

				/*
				 * 若没有默认值（即默认值实际上是 NULL），
				 * 我们必须显式把该列设为 NULL，除非
				 * 目标关系是自动可更新视图。
				 */
				if (!new_expr)
				{
					if (isAutoUpdatableView)
					{
						/* 保持该值原样 */
						newList = lappend(newList, col);
						allReplaced = false;
						continue;
					}

					new_expr = coerce_null_to_domain(att_tup->atttypid,
													 att_tup->atttypmod,
													 att_tup->attcollation,
													 att_tup->attlen,
													 att_tup->attbyval);
				}
				newList = lappend(newList, new_expr);
			}
			else
				newList = lappend(newList, col);
		}
		newValues = lappend(newValues, newList);
	}
	rte->values_lists = newValues;

	pfree(attrnos);

	return allReplaced;
}

/*
 * 把给定 VALUES RTE 里剩余的 DEFAULT 项清理掉，
 * 替换成 NULL 常量。
 *
 * 这用于挂在自动可更新视图上的 DO ALSO 规则所生成的
 * 产物查询。该动作不能依赖目标关系，
 * 因为产物查询可能没有目标关系（它不必是 INSERT）。
 * 本质上，这类查询被当作挂在由规则可更新的
 * 视图上。
 */
static void
rewriteValuesRTEToNulls(Query *parsetree, RangeTblEntry *rte)
{
	List	   *newValues;
	ListCell   *lc;

	newValues = NIL;
	foreach(lc, rte->values_lists)
	{
		List	   *sublist = (List *) lfirst(lc);
		List	   *newList = NIL;
		ListCell   *lc2;

		foreach(lc2, sublist)
		{
			Node	   *col = (Node *) lfirst(lc2);

			if (IsA(col, SetToDefault))
			{
				SetToDefault *def = (SetToDefault *) col;

				newList = lappend(newList, makeNullConst(def->typeId,
														 def->typeMod,
														 def->collation));
			}
			else
				newList = lappend(newList, col);
		}
		newValues = lappend(newValues, newList);
	}
	rte->values_lists = newValues;
}


/*
 * matchLocks -
 *	  匹配锁列表并返回匹配的规则
 */
static List *
matchLocks(CmdType event,
		   RuleLock *rulelocks,
		   int varno,
		   Query *parsetree,
		   bool *hasUpdate)
{
	List	   *matching_locks = NIL;
	int			nlocks;
	int			i;

	if (rulelocks == NULL)
		return NIL;

	/* MERGE 没有规则支持 */
	if (parsetree->commandType == CMD_MERGE)
		return NIL;

	if (parsetree->commandType != CMD_SELECT)
	{
		if (parsetree->resultRelation != varno)
			return NIL;
	}

	nlocks = rulelocks->numLocks;

	for (i = 0; i < nlocks; i++)
	{
		RewriteRule *oneLock = rulelocks->rules[i];

		if (oneLock->event == CMD_UPDATE)
			*hasUpdate = true;

		/*
		 * 抑制被禁用的、或被配置为在当前会话的复制
		 * 角色期间不触发的 ON INSERT/UPDATE/DELETE 规则。
		 * ON SELECT 规则将始终被应用，以便即使在 LOCAL 或 REPLICA
		 * 角色下视图也能工作。
		 */
		if (oneLock->event != CMD_SELECT)
		{
			if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
			{
				if (oneLock->enabled == RULE_FIRES_ON_ORIGIN ||
					oneLock->enabled == RULE_DISABLED)
					continue;
			}
			else				/* ORIGIN 或 LOCAL 角色 */
			{
				if (oneLock->enabled == RULE_FIRES_ON_REPLICA ||
					oneLock->enabled == RULE_DISABLED)
					continue;
			}
		}

		if (oneLock->event == event)
		{
			if (parsetree->commandType != CMD_SELECT ||
				rangeTableEntry_used((Node *) parsetree, varno, 0))
				matching_locks = lappend(matching_locks, oneLock);
		}
	}

	return matching_locks;
}


/*
 * ApplyRetrieveRule - 展开一条 ON SELECT 规则
 */
static Query *
ApplyRetrieveRule(Query *parsetree,
				  RewriteRule *rule,
				  int rt_index,
				  Relation relation,
				  List *activeRIRs)
{
	Query	   *rule_action;
	RangeTblEntry *rte;
	RowMarkClause *rc;
	int			numCols;

	if (list_length(rule->actions) != 1)
		elog(ERROR, "expected just one rule action");
	if (rule->qual != NULL)
		elog(ERROR, "cannot handle qualified ON SELECT rule");

	/* 检查非系统视图的展开是否受限 */
	if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_VIEW) != 0 &&
				 RelationGetRelid(relation) >= FirstNormalObjectId))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access to non-system view \"%s\" is restricted",
						RelationGetRelationName(relation))));

	if (rt_index == parsetree->resultRelation)
	{
		/*
		 * 查询的结果关系是一个视图，而且它没有被
		 * 任何规则重写。若有 INSTEAD OF 触发器会拦住
		 * 对视图行的插入、更新或删除尝试，则支持这种情形。
		 * 执行器会检查这一点；目前先继续
		 * 往前走。我们有两种情形：
		 *
		 * 对 INSERT，我们不必做任何事。未修改的 RTE 就能
		 * 很好地充当结果关系。
		 *
		 * 对 UPDATE/DELETE，我们需要展开视图，以便为操作提供
		 * 源数据。但我们也需要一个未修改的 RTE 来
		 * 充当目标。因此复制该 RTE，并把副本加入
		 * 范围表。注意副本不会被加入 jointree。
		 * 还要注意，fireRIRrules 里有一处权宜做法，避免在到达
		 * 复制出的 RTE 时再次调用本函数。
		 */
		if (parsetree->commandType == CMD_INSERT)
			return parsetree;
		else if (parsetree->commandType == CMD_UPDATE ||
				 parsetree->commandType == CMD_DELETE)
		{
			RangeTblEntry *newrte;
			Var		   *var;
			TargetEntry *tle;

			rte = rt_fetch(rt_index, parsetree->rtable);
			newrte = copyObject(rte);
			parsetree->rtable = lappend(parsetree->rtable, newrte);
			parsetree->resultRelation = list_length(parsetree->rtable);

			/*
			 * 在大多数情况下，引用该视图的 Var 应当保持
			 * 原样，这意味着它们隐式表示 OLD 值。
			 * 但在 RETURNING 列表里（若有），我们希望这样的 Var
			 * 表示 NEW 值，因此把它们改成引用新的 RTE。
			 *
			 * 由于 ChangeVarNodes 就地修改树，为安全起见先复制
			 * RETURNING 列表。
			 */
			parsetree->returningList = copyObject(parsetree->returningList);
			ChangeVarNodes((Node *) parsetree->returningList, rt_index,
						   parsetree->resultRelation, 0);

			/*
			 * 为了让执行器能算出要传给
			 * INSTEAD OF 触发器的原始视图行，我们加入一个 resjunk 的整行 Var，
			 * 引用原来的 RTE。它稍后会被展开
			 * 成计算该视图行全部 OLD 值的 RowExpr。
			 */
			var = makeWholeRowVar(rte, rt_index, 0, false);
			tle = makeTargetEntry((Expr *) var,
								  list_length(parsetree->targetList) + 1,
								  pstrdup("wholerow"),
								  true);

			parsetree->targetList = lappend(parsetree->targetList, tle);

			/* 现在继续展开原来的视图 RTE */
		}
		else
			elog(ERROR, "unrecognized commandType: %d",
				 (int) parsetree->commandType);
	}

	/*
	 * 检查是否有作用于本视图的 FOR [KEY] UPDATE/SHARE 子句。
	 *
	 * 注意：我们不必显式考虑出现在
	 * 祖先查询层次里的这类子句；它们的效果已经被 markQueryForLocking
	 * 下推到这里，并会反映在 rc 里。
	 */
	rc = get_parse_rowmark(parsetree, rt_index);

	/*
	 * 制作视图查询的可修改副本，并取得它所提到的关系上
	 * 所需的锁。若有影响本视图的 FOR [KEY] UPDATE/SHARE 子句，
	 * 则强制所有这些关系至少使用 RowShareLock。
	 */
	rule_action = copyObject(linitial(rule->actions));

	AcquireRewriteLocks(rule_action, true, (rc != NULL));

	/*
	 * 若对视图做 FOR [KEY] UPDATE/SHARE，就把所含的全部表标成
	 * 隐式的 FOR [KEY] UPDATE/SHARE，如同解析器在
	 * 视图的子查询被显式写出时会做的那样。
	 */
	if (rc != NULL)
		markQueryForLocking(rule_action, (Node *) rule_action->jointree,
							rc->strength, rc->waitPolicy, true);

	/*
	 * 递归展开视图内部的任何视图引用。
	 */
	rule_action = fireRIRrules(rule_action, activeRIRs);

	/*
	 * 若视图查询有行安全性，确保本查询也被标成
	 * 具有行安全性。
	 */
	parsetree->hasRowSecurity |= rule_action->hasRowSecurity;

	/*
	 * 现在把视图查询作为子查询接进去，把该关系原来的
	 * RTE 转换成子查询 RTE。
	 */
	rte = rt_fetch(rt_index, parsetree->rtable);

	rte->rtekind = RTE_SUBQUERY;
	rte->subquery = rule_action;
	rte->security_barrier = RelationIsSecurityView(relation);

	/*
	 * 清掉不应当在子查询 RTE 里设置的成员。注意我们
	 * 保留 relid、relkind、rellockmode 和 perminfoindex 成员，以便
	 * 执行前能适当地锁定视图关系，并
	 * 检查它的权限。
	 */
	rte->tablesample = NULL;
	rte->inh = false;			/* 子查询不得设置它 */

	/*
	 * 由于我们允许 CREATE OR REPLACE VIEW 给视图增加列，
	 * rule_action 发出的列可能比当前查询被解析时
	 * 所预期的更多。多处都预期 rte->eref->colnames 与
	 * 子查询的非废输出列一致，因此必要时
	 * 加入一些哑列名来补齐。
	 */
	numCols = ExecCleanTargetListLength(rule_action->targetList);
	while (list_length(rte->eref->colnames) < numCols)
	{
		rte->eref->colnames = lappend(rte->eref->colnames,
									  makeString(pstrdup("?column?")));
	}

	return parsetree;
}

/*
 * 递归地把视图所用的全部关系标成 FOR [KEY] UPDATE/SHARE。
 *
 * 这可能生成无效查询，例如某个子查询使用了
 * 聚合。我们留给规划器去检测。
 *
 * 注意：这必须与解析器的 transformLockingClause() 例程一致。
 * 不过，我们过去必须避免把视图的 OLD 和 NEW 关系标成
 * 要更新，这促使我们扫描 jointree 来确定哪些关系
 * 被使用。现在也许可以简化成只扫描
 * 范围表，像解析器那样。
 */
static void
markQueryForLocking(Query *qry, Node *jtnode,
					LockClauseStrength strength, LockWaitPolicy waitPolicy,
					bool pushedDown)
{
	if (jtnode == NULL)
		return;
	if (IsA(jtnode, RangeTblRef))
	{
		int			rti = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(rti, qry->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			RTEPermissionInfo *perminfo;

			applyLockingClause(qry, rti, strength, waitPolicy, pushedDown);

			perminfo = getRTEPermissionInfo(qry->rteperminfos, rte);
			perminfo->requiredPerms |= ACL_SELECT_FOR_UPDATE;
		}
		else if (rte->rtekind == RTE_SUBQUERY)
		{
			applyLockingClause(qry, rti, strength, waitPolicy, pushedDown);
			/* 子查询的 FOR UPDATE/SHARE 会传播到子查询的关系 */
			markQueryForLocking(rte->subquery, (Node *) rte->subquery->jointree,
								strength, waitPolicy, true);
		}
		/* 其他 RTE 类型不受 FOR UPDATE 影响 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *l;

		foreach(l, f->fromlist)
			markQueryForLocking(qry, lfirst(l), strength, waitPolicy, pushedDown);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		markQueryForLocking(qry, j->larg, strength, waitPolicy, pushedDown);
		markQueryForLocking(qry, j->rarg, strength, waitPolicy, pushedDown);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}


/*
 * fireRIRonSubLink -
 *	对给定树里找到的每个 SubLink（表达式中的子查询）
 *	应用 fireRIRrules()。
 *
 * 注意：虽然这具有遍历器的形式，我们取巧并就地修改
 * SubLink 节点。调用者有责任确保
 * 不会发生不想要的副作用！
 *
 * 这与大多数其他递归进入子查询的例程不同，
 * 因为我们必须在 SubLink 节点处取得控制，以便用
 * 可能已被重写的子查询替换 SubLink 的子查询链接。
 */
static bool
fireRIRonSubLink(Node *node, fireRIRonSubLink_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, SubLink))
	{
		SubLink    *sub = (SubLink *) node;

		/* 做我们来这里要做的事 */
		sub->subselect = (Node *) fireRIRrules((Query *) sub->subselect,
											   context->activeRIRs);

		/*
		 * 记住是否有任何子链接具有行安全性。
		 */
		context->hasRowSecurity |= ((Query *) sub->subselect)->hasRowSecurity;

		/* Fall through to process lefthand args of SubLink */
	}

	/*
	 * 不要递归进入 Query 节点，因为 fireRIRrules 已经
	 * 为我们处理了子查询的子查询。
	 */
	return expression_tree_walker(node, fireRIRonSubLink,
								  (void *) context);
}


/*
 * fireRIRrules -
 *	对给定查询里的每个范围表项应用全部 RIR 规则。
 *
 * activeRIRs 是我们正在为其处理 RIR
 * 规则的视图 OIDs 列表，用来检测并拒绝递归。
 */
static Query *
fireRIRrules(Query *parsetree, List *activeRIRs)
{
	int			origResultRelation = parsetree->resultRelation;
	int			rt_index;
	ListCell   *lc;

	/*
	 * 展开 CTEs 里的 SEARCH 和 CYCLE 子句。
	 *
	 * 这只是做这件事的方便之处，因为我们已经在
	 * 查看每个 Query。
	 */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

		if (cte->search_clause || cte->cycle_clause)
		{
			cte = rewriteSearchAndCycle(cte);
			lfirst(lc) = cte;
		}
	}

	/*
	 * 不要试图把这个改成 foreach 循环，因为 rtable 列表每次
	 * 经过时都可能被改变……
	 */
	rt_index = 0;
	while (rt_index < list_length(parsetree->rtable))
	{
		RangeTblEntry *rte;
		Relation	rel;
		List	   *locks;
		RuleLock   *rules;
		RewriteRule *rule;
		int			i;

		++rt_index;

		rte = rt_fetch(rt_index, parsetree->rtable);

		/*
		 * 子查询 RTE 不可能有关联的规则，因此这一层
		 * 查询无事可做，但我们必须递归进入
		 * 子查询，以展开其中的任何规则引用。
		 */
		if (rte->rtekind == RTE_SUBQUERY)
		{
			rte->subquery = fireRIRrules(rte->subquery, activeRIRs);

			/*
			 * 既然在这里，若它的任何子查询有行安全性，
			 * 就确保本查询也被标成具有行安全性。
			 */
			parsetree->hasRowSecurity |= rte->subquery->hasRowSecurity;

			continue;
		}

		/*
		 * 连接和其他非关系 RTEs 可以完全忽略。
		 */
		if (rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * 始终忽略查询里引用的物化视图的 RIR 规则。
		 * （这并不阻止刷新物化视图，因为它们不会在
		 * 自己的查询定义里被引用。）
		 *
		 * 注意：将来我们也许希望允许物化视图在不可扫描时
		 * 像普通视图那样被有条件地展开。
		 * 那种情况下这项测试需要推迟到我们
		 * 打开该关系之后，以便检查它的状态。
		 */
		if (rte->relkind == RELKIND_MATVIEW)
			continue;

		/*
		 * 在 INSERT ... ON CONFLICT 里，忽略 EXCLUDED 伪关系；
		 * 即使它指向一个视图，我们也不必展开它，而且不应当
		 * 展开，因为我们希望该 RTE 保持 RTE_RELATION 类型。否则，
		 * 它会被改成 RTE_SUBQUERY 类型，那是一种
		 * 未经测试、不受支持的情形。
		 */
		if (parsetree->onConflict &&
			rt_index == parsetree->onConflict->exclRelIndex)
			continue;

		/*
		 * 若该表在查询里未被引用，我们就忽略它。
		 * 这防止因规则展开插入新的 rtable 项而导致的
		 * 无限展开循环。表被引用的条件是：它是
		 * 连接集合的一部分（源表），或被任何 Var
		 * 节点引用，或是结果表。
		 */
		if (rt_index != parsetree->resultRelation &&
			!rangeTableEntry_used((Node *) parsetree, rt_index, 0))
			continue;

		/*
		 * 另外，若这是 ApplyRetrieveRule 引入的新结果关系，
		 * 我们不想再对它做任何事。
		 */
		if (rt_index == parsetree->resultRelation &&
			rt_index != origResultRelation)
			continue;

		/*
		 * 这里可以用 NoLock，因为解析器或
		 * AcquireRewriteLocks 应当已经锁定了该关系。
		 */
		rel = table_open(rte->relid, NoLock);

		/*
		 * 收集我们必须应用的 RIR 规则
		 */
		rules = rel->rd_rules;
		if (rules != NULL)
		{
			locks = NIL;
			for (i = 0; i < rules->numLocks; i++)
			{
				rule = rules->rules[i];
				if (rule->event != CMD_SELECT)
					continue;

				locks = lappend(locks, rule);
			}

			/*
			 * 若找到了任何规则，就应用它们，但先检查递归！
			 */
			if (locks != NIL)
			{
				ListCell   *l;

				if (list_member_oid(activeRIRs, RelationGetRelid(rel)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("infinite recursion detected in rules for relation \"%s\"",
									RelationGetRelationName(rel))));
				activeRIRs = lappend_oid(activeRIRs, RelationGetRelid(rel));

				foreach(l, locks)
				{
					rule = lfirst(l);

					parsetree = ApplyRetrieveRule(parsetree,
												  rule,
												  rt_index,
												  rel,
												  activeRIRs);
				}

				activeRIRs = list_delete_last(activeRIRs);
			}
		}

		table_close(rel, NoLock);
	}

	/* 递归进入 WITH 里的子查询 */
	foreach(lc, parsetree->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

		cte->ctequery = (Node *)
			fireRIRrules((Query *) cte->ctequery, activeRIRs);

		/*
		 * 既然在这里，若它的任何 CTEs 有行安全性，
		 * 就确保本查询也被标成具有行安全性。
		 */
		parsetree->hasRowSecurity |= ((Query *) cte->ctequery)->hasRowSecurity;
	}

	/*
	 * 也递归进入子链接的子查询。但 rtable 和 cteList 里的
	 * 那些我们已经处理过了。
	 */
	if (parsetree->hasSubLinks)
	{
		fireRIRonSubLink_context context;

		context.activeRIRs = activeRIRs;
		context.hasRowSecurity = false;

		query_tree_walker(parsetree, fireRIRonSubLink, (void *) &context,
						  QTW_IGNORE_RC_SUBQUERIES);

		/*
		 * 若它的任何子链接有行安全性，确保本查询也被标成
		 * 具有行安全性。
		 */
		parsetree->hasRowSecurity |= context.hasRowSecurity;
	}

	/*
	 * 应用任何行安全性策略。我们最后才做这件事，因为它
	 * 在新限定条件含有子链接子查询时需要特殊的递归检测，
	 * 而且若我们在上面的循环里做，query_tree_walker 会
	 * 再次递归进入那些限定条件。
	 */
	rt_index = 0;
	foreach(lc, parsetree->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		Relation	rel;
		List	   *securityQuals;
		List	   *withCheckOptions;
		bool		hasRowSecurity;
		bool		hasSubLinks;

		++rt_index;

		/* 只有普通关系才能有行安全性策略 */
		if (rte->rtekind != RTE_RELATION ||
			(rte->relkind != RELKIND_RELATION &&
			 rte->relkind != RELKIND_PARTITIONED_TABLE))
			continue;

		rel = table_open(rte->relid, NoLock);

		/*
		 * 取出必须应用于这个 RTE 的任何新安全性限定条件。
		 */
		get_row_security_policies(parsetree, rte, rt_index,
								  &securityQuals, &withCheckOptions,
								  &hasRowSecurity, &hasSubLinks);

		if (securityQuals != NIL || withCheckOptions != NIL)
		{
			if (hasSubLinks)
			{
				acquireLocksOnSubLinks_context context;
				fireRIRonSubLink_context fire_context;

				/*
				 * 递归处理新的限定条件，检查无限
				 * 递归。
				 */
				if (list_member_oid(activeRIRs, RelationGetRelid(rel)))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("infinite recursion detected in policy for relation \"%s\"",
									RelationGetRelationName(rel))));

				activeRIRs = lappend_oid(activeRIRs, RelationGetRelid(rel));

				/*
				 * get_row_security_policies 刚刚传回了 securityQuals
				 * 和/或 withCheckOptions，并且曾有 SubLinks，确保
				 * 我们锁定任何被引用的关系。
				 *
				 * 这些锁通常由解析器取得，但
				 * securityQuals 和 withCheckOptions 是在解析之后加入的。
				 */
				context.for_execute = true;
				(void) acquireLocksOnSubLinks((Node *) securityQuals, &context);
				(void) acquireLocksOnSubLinks((Node *) withCheckOptions,
											  &context);

				/*
				 * 既然我们已经锁定了 get_row_security_policies 加入的任何东西，
				 * 就为它们触发任何 RIR 规则。
				 */
				fire_context.activeRIRs = activeRIRs;
				fire_context.hasRowSecurity = false;

				expression_tree_walker((Node *) securityQuals,
									   fireRIRonSubLink, (void *) &fire_context);

				expression_tree_walker((Node *) withCheckOptions,
									   fireRIRonSubLink, (void *) &fire_context);

				/*
				 * 我们可以忽略 fire_context.hasRowSecurity 的值，
				 * 因为只有在 hasRowSecurity
				 * 已经为真的情形下才会到达这段代码。
				 */
				Assert(hasRowSecurity);

				activeRIRs = list_delete_last(activeRIRs);
			}

			/*
			 * 把新的安全屏障限定条件加到该 RTE 列表的
			 * 开头，使它们先于任何已有的屏障限定条件被应用
			 * （那些会来自安全屏障视图，并且应当
			 * 比表本身上的行安全性条件优先级更低）。
			 */
			rte->securityQuals = list_concat(securityQuals,
											 rte->securityQuals);

			parsetree->withCheckOptions = list_concat(withCheckOptions,
													  parsetree->withCheckOptions);
		}

		/*
		 * 若行安全性适用，或新限定条件含有子链接，
		 * 确保查询被正确标记。
		 */
		if (hasRowSecurity)
			parsetree->hasRowSecurity = true;
		if (hasSubLinks)
			parsetree->hasSubLinks = true;

		table_close(rel, NoLock);
	}

	return parsetree;
}


/*
 * 修改给定查询，在其限定条件上加上 'AND rule_qual IS NOT TRUE'。
 * 这用来为条件 INSTEAD 规则生成合适的 else 子句。
 * （不幸的是我们必须用 x IS NOT TRUE，
 * 而不能只用规划器聪明得多的 NOT x，否则当限定条件
 * 求值为 NULL 时我们会做错事。）
 *
 * rule_qual 可能含有对 OLD 或 NEW 的引用。OLD 引用被
 * 替换成对指定 rt_index（规则所作用的关系）的引用。
 * NEW 引用只可能出现在对该关系本身的 INSERT 和 UPDATE
 * 查询里，因此应当被替换成查询自身目标列表里
 * 相关项的副本。
 */
static Query *
CopyAndAddInvertedQual(Query *parsetree,
					   Node *rule_qual,
					   int rt_index,
					   CmdType event)
{
	/* 不要就地修改传进来的限定条件（它在 relcache 里！） */
	Node	   *new_qual = copyObject(rule_qual);
	acquireLocksOnSubLinks_context context;

	context.for_execute = true;

	/*
	 * 若限定条件里有子查询，取得必要的锁并
	 * 修正任何已删除的 JOIN RTE 项。（这与
	 * rewriteRuleAction 有些重复，但并不完全……考虑重构，使
	 * 我们只需以这种方式处理限定条件一次。）
	 */
	(void) acquireLocksOnSubLinks(new_qual, &context);

	/* 修正对 OLD 的引用 */
	ChangeVarNodes(new_qual, PRS2_OLD_VARNO, rt_index, 0);
	/* 修正对 NEW 的引用 */
	if (event == CMD_INSERT || event == CMD_UPDATE)
		new_qual = ReplaceVarsFromTargetList(new_qual,
											 PRS2_NEW_VARNO,
											 0,
											 rt_fetch(rt_index,
													  parsetree->rtable),
											 parsetree->targetList,
											 (event == CMD_UPDATE) ?
											 REPLACEVARS_CHANGE_VARNO :
											 REPLACEVARS_SUBSTITUTE_NULL,
											 rt_index,
											 &parsetree->hasSubLinks);
	/* 然后附上修正后的限定条件 */
	AddInvertedQual(parsetree, new_qual);

	return parsetree;
}


/*
 *	fireRules -
 *	   遍历规则锁并应用规则。
 *
 * 输入参数：
 *	parsetree - 原始查询
 *	rt_index - 原始查询里结果关系的 RT 下标
 *	event - 规则事件的类型
 *	locks - 要触发的规则列表
 * 输出参数：
 *	*instead_flag - 若找到任何无限定条件的 INSTEAD 规则则设为真
 *					（必须初始化为假）
 *	*returning_flag - 若我们重写了任何规则里的 RETURNING 子句则设为真
 *					（必须初始化为假）
 *	*qual_product - 若找到任何带限定条件的
 *					INSTEAD 规则，则填入修改后的原始查询（必须初始化为 NULL）
 * 返回值：
 *	调整后供本查询使用的规则动作列表。
 *
 * 带限定条件的 INSTEAD 规则生成加上了限定
 * 条件的动作。它们还生成原始查询的修改版，
 * 加上取反后的限定条件，使它只对
 * 带限定条件的动作不作用的行运行。（若有多条
 * 带限定条件的 INSTEAD 规则，我们把所有取反的限定条件 AND 到一条
 * 修改后的原始查询上。）若找到带限定条件或无限定条件的
 * INSTEAD 规则，我们就不会执行原始的、未修改的
 * 查询。若两者都找到，修改后的原始查询也会被丢弃。
 */
static List *
fireRules(Query *parsetree,
		  int rt_index,
		  CmdType event,
		  List *locks,
		  bool *instead_flag,
		  bool *returning_flag,
		  Query **qual_product)
{
	List	   *results = NIL;
	ListCell   *l;

	foreach(l, locks)
	{
		RewriteRule *rule_lock = (RewriteRule *) lfirst(l);
		Node	   *event_qual = rule_lock->qual;
		List	   *actions = rule_lock->actions;
		QuerySource qsrc;
		ListCell   *r;

		/* 为这些动作确定正确的 QuerySource 值 */
		if (rule_lock->isInstead)
		{
			if (event_qual != NULL)
				qsrc = QSRC_QUAL_INSTEAD_RULE;
			else
			{
				qsrc = QSRC_INSTEAD_RULE;
				*instead_flag = true;	/* 报告无限定条件的 INSTEAD */
			}
		}
		else
			qsrc = QSRC_NON_INSTEAD_RULE;

		if (qsrc == QSRC_QUAL_INSTEAD_RULE)
		{
			/*
			 * 若有带限定条件的 INSTEAD 规则，原始
			 * 查询仍会执行。但所有 INSTEAD 规则的取反
			 * 限定条件都会被加上，使它只在所有 INSTEAD 规则的
			 * 规则限定条件都为假的情形下
			 * 执行动作。可以把它想成 case 里的默认动作。我们把
			 * 它保存在 *qual_product 里，以便 RewriteQuery() 在我们把它
			 * 改得够乱之后把它加进查询列表。
			 *
			 * 若我们已经找到一条无限定条件的 INSTEAD 规则，则
			 * *qual_product 不会被使用，因此不必费心构造它。
			 */
			if (!*instead_flag)
			{
				if (*qual_product == NULL)
					*qual_product = copyObject(parsetree);
				*qual_product = CopyAndAddInvertedQual(*qual_product,
													   event_qual,
													   rt_index,
													   event);
			}
		}

		/* 现在处理规则的动作，并把它们加入结果列表 */
		foreach(r, actions)
		{
			Query	   *rule_action = lfirst(r);

			if (rule_action->commandType == CMD_NOTHING)
				continue;

			rule_action = rewriteRuleAction(parsetree, rule_action,
											event_qual, rt_index, event,
											returning_flag);

			rule_action->querySource = qsrc;
			rule_action->canSetTag = false; /* 稍后可能会变 */

			results = lappend(results, rule_action);
		}
	}

	return results;
}


/*
 * get_view_query - 从视图的 _RETURN 规则取得 Query。
 *
 * 调用者应当已经验证该关系是视图，因此
 * 我们应当找到一条 ON SELECT 动作。
 *
 * 注意返回的指针指向 relcache，因此对调用者必须
 * 当作只读，不得修改或就地涂改。
 */
Query *
get_view_query(Relation view)
{
	int			i;

	Assert(view->rd_rel->relkind == RELKIND_VIEW);

	for (i = 0; i < view->rd_rules->numLocks; i++)
	{
		RewriteRule *rule = view->rd_rules->rules[i];

		if (rule->event == CMD_SELECT)
		{
			/* 一条 _RETURN 规则应当只有一个动作 */
			if (list_length(rule->actions) != 1)
				elog(ERROR, "invalid _RETURN rule action specification");

			return (Query *) linitial(rule->actions);
		}
	}

	elog(ERROR, "failed to find _RETURN rule for view");
	return NULL;				/* 消除编译器告警 */
}


/*
 * view_has_instead_trigger - 视图是否有该事件的 INSTEAD OF 触发器？
 *
 * 若有，我们就不想把它当作自动可更新。这项测试不能
 * 并进 view_query_is_auto_updatable，因为它不是一种错误
 * 条件。
 */
static bool
view_has_instead_trigger(Relation view, CmdType event)
{
	TriggerDesc *trigDesc = view->trigdesc;

	switch (event)
	{
		case CMD_INSERT:
			if (trigDesc && trigDesc->trig_insert_instead_row)
				return true;
			break;
		case CMD_UPDATE:
			if (trigDesc && trigDesc->trig_update_instead_row)
				return true;
			break;
		case CMD_DELETE:
			if (trigDesc && trigDesc->trig_delete_instead_row)
				return true;
			break;
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) event);
			break;
	}
	return false;
}


/*
 * view_col_is_auto_updatable - 测试视图的指定列
 * 是否自动可更新。若该列可以更新则返回 NULL，否则返回
 * 说明它无法更新的原因的消息字符串。
 *
 * 返回的字符串尚未翻译；若它要作为错误
 * 信息显示，调用者应当用 _() 来翻译它。
 *
 * 注意这里所做的检查局部于本视图。我们不检查
 * 底层基关系里被引用的列是否可更新。
 */
static const char *
view_col_is_auto_updatable(RangeTblRef *rtr, TargetEntry *tle)
{
	Var		   *var = (Var *) tle->expr;

	/*
	 * 目前，我们支持的可更新列只是那些引用
	 * 底层基关系的用户列的 Var。
	 *
	 * 视图目标列表可能含有 resjunk 列（例如定义为
	 * "SELECT * FROM t ORDER BY a+b" 的视图是自动可更新的），但这类列
	 * 不是自动可更新的，而且事实上绝不应当出现在外层
	 * 查询的目标列表里。
	 */
	if (tle->resjunk)
		return gettext_noop("Junk view columns are not updatable.");

	if (!IsA(var, Var) ||
		var->varno != rtr->rtindex ||
		var->varlevelsup != 0)
		return gettext_noop("View columns that are not columns of their base relation are not updatable.");

	if (var->varattno < 0)
		return gettext_noop("View columns that refer to system columns are not updatable.");

	if (var->varattno == 0)
		return gettext_noop("View columns that return whole-row references are not updatable.");

	return NULL;				/* 该视图列是可更新的 */
}


/*
 * view_query_is_auto_updatable - 测试指定的视图定义
 * 是否表示自动可更新视图。若视图可以更新则返回 NULL，
 * 否则返回说明它无法更新的原因的消息字符串。

 * 返回的字符串尚未翻译；若它要作为错误
 * 信息显示，调用者应当用 _() 来翻译它。
 *
 * 若 check_cols 为真，则视图至少要有一个可更新
 * 列（INSERT/UPDATE 所必需）。否则不检查视图的列
 * 是否可更新。另见 view_cols_are_auto_updatable。
 *
 * 注意这里所做的检查只基于视图定义。
 * 我们不检查视图所引用的任何基关系是否
 * 可更新。
 */
const char *
view_query_is_auto_updatable(Query *viewquery, bool check_cols)
{
	RangeTblRef *rtr;
	RangeTblEntry *base_rte;

	/*----------
	 * Check if the view is simply updatable.  According to SQL-92 this means:
	 *	- No DISTINCT clause.
	 *	- Each TLE is a column reference, and each column appears at most once.
	 *	- FROM contains exactly one base relation.
	 *	- No GROUP BY or HAVING clauses.
	 *	- No set operations (UNION, INTERSECT or EXCEPT).
	 *	- No sub-queries in the WHERE clause that reference the target table.
	 *
	 * We ignore that last restriction since it would be complex to enforce
	 * and there isn't any actual benefit to disallowing sub-queries.  (The
	 * semantic issues that the standard is presumably concerned about don't
	 * arise in Postgres, since any such sub-query will not see any updates
	 * executed by the outer query anyway, thanks to MVCC snapshotting.)
	 *
	 * We also relax the second restriction by supporting part of SQL:1999
	 * feature T111, which allows for a mix of updatable and non-updatable
	 * columns, provided that an INSERT or UPDATE doesn't attempt to assign to
	 * a non-updatable column.
	 *
	 * In addition we impose these constraints, involving features that are
	 * not part of SQL-92:
	 *	- No CTEs (WITH clauses).
	 *	- No OFFSET or LIMIT clauses (this matches a SQL:2008 restriction).
	 *	- No system columns (including whole-row references) in the tlist.
	 *	- No window functions in the tlist.
	 *	- No set-returning functions in the tlist.
	 *
	 * Note that we do these checks without recursively expanding the view.
	 * If the base relation is a view, we'll recursively deal with it later.
	 *----------
	 */
	if (viewquery->distinctClause != NIL)
		return gettext_noop("Views containing DISTINCT are not automatically updatable.");

	if (viewquery->groupClause != NIL || viewquery->groupingSets)
		return gettext_noop("Views containing GROUP BY are not automatically updatable.");

	if (viewquery->havingQual != NULL)
		return gettext_noop("Views containing HAVING are not automatically updatable.");

	if (viewquery->setOperations != NULL)
		return gettext_noop("Views containing UNION, INTERSECT, or EXCEPT are not automatically updatable.");

	if (viewquery->cteList != NIL)
		return gettext_noop("Views containing WITH are not automatically updatable.");

	if (viewquery->limitOffset != NULL || viewquery->limitCount != NULL)
		return gettext_noop("Views containing LIMIT or OFFSET are not automatically updatable.");

	/*
	 * 我们不得允许目标列表里出现窗口函数或集合返回函数。
	 * 否则我们可能最终把它们插进主查询的限定条件。
	 * 我们还必须检查目标列表里的聚合，
	 * 以防它们出现时没有 GROUP BY。
	 *
	 * 这些限制确保视图的每一行都对应
	 * 底层基关系里的唯一一行。
	 */
	if (viewquery->hasAggs)
		return gettext_noop("Views that return aggregate functions are not automatically updatable.");

	if (viewquery->hasWindowFuncs)
		return gettext_noop("Views that return window functions are not automatically updatable.");

	if (viewquery->hasTargetSRFs)
		return gettext_noop("Views that return set-returning functions are not automatically updatable.");

	/*
	 * 视图查询应当从一个单一基关系里选取，它必须是
	 * 表或另一个视图。
	 */
	if (list_length(viewquery->jointree->fromlist) != 1)
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	rtr = (RangeTblRef *) linitial(viewquery->jointree->fromlist);
	if (!IsA(rtr, RangeTblRef))
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	base_rte = rt_fetch(rtr->rtindex, viewquery->rtable);
	if (base_rte->rtekind != RTE_RELATION ||
		(base_rte->relkind != RELKIND_RELATION &&
		 base_rte->relkind != RELKIND_FOREIGN_TABLE &&
		 base_rte->relkind != RELKIND_VIEW &&
		 base_rte->relkind != RELKIND_PARTITIONED_TABLE))
		return gettext_noop("Views that do not select from a single table or view are not automatically updatable.");

	if (base_rte->tablesample)
		return gettext_noop("Views containing TABLESAMPLE are not automatically updatable.");

	/*
	 * 检查视图至少有一个可更新列。这对
	 * INSERT/UPDATE 是必需的，但对 DELETE 不是。
	 */
	if (check_cols)
	{
		ListCell   *cell;
		bool		found;

		found = false;
		foreach(cell, viewquery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(cell);

			if (view_col_is_auto_updatable(rtr, tle) == NULL)
			{
				found = true;
				break;
			}
		}

		if (!found)
			return gettext_noop("Views that have no updatable columns are not automatically updatable.");
	}

	return NULL;				/* 该视图是可更新的 */
}


/*
 * view_cols_are_auto_updatable - 测试自动可更新视图的全部所需列
 * 是否真的可更新。若全部所需列都可以更新则返回 NULL，否则返回
 * 说明它们无法更新的原因的消息
 * 字符串。
 *
 * 返回的字符串尚未翻译；若它要作为错误
 * 信息显示，调用者应当用 _() 来翻译它。
 *
 * 这应当用于 INSERT/UPDATE，以确保我们不会试图
 * 对任何不可更新的列赋值。
 *
 * 此外，它也可用来取出视图里可更新列的集合，
 * 或者若一个或多个所需列不可更新，则取出
 * 第一个违规的不可更新列的名称。
 *
 * 调用者必须已经用 view_query_is_auto_updatable
 * 验证这是一个自动可更新视图。
 *
 * 注意这里所做的检查只基于视图定义。
 * 我们不检查基关系里被引用的列是否
 * 可更新。
 */
static const char *
view_cols_are_auto_updatable(Query *viewquery,
							 Bitmapset *required_cols,
							 Bitmapset **updatable_cols,
							 char **non_updatable_col)
{
	RangeTblRef *rtr;
	AttrNumber	col;
	ListCell   *cell;

	/*
	 * 调用者应当已经验证该视图是自动可更新的，因此
	 * 应当有一个单一基关系。
	 */
	Assert(list_length(viewquery->jointree->fromlist) == 1);
	rtr = linitial_node(RangeTblRef, viewquery->jointree->fromlist);

	/* 初始化可选的返回值 */
	if (updatable_cols != NULL)
		*updatable_cols = NULL;
	if (non_updatable_col != NULL)
		*non_updatable_col = NULL;

	/* 测试每个视图列是否可更新 */
	col = -FirstLowInvalidHeapAttributeNumber;
	foreach(cell, viewquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(cell);
		const char *col_update_detail;

		col++;
		col_update_detail = view_col_is_auto_updatable(rtr, tle);

		if (col_update_detail == NULL)
		{
			/* 该列是可更新的 */
			if (updatable_cols != NULL)
				*updatable_cols = bms_add_member(*updatable_cols, col);
		}
		else if (bms_is_member(col, required_cols))
		{
			/* 所需的列不可更新 */
			if (non_updatable_col != NULL)
				*non_updatable_col = tle->resname;
			return col_update_detail;
		}
	}

	return NULL;				/* 全部所需的视图列都是可更新的 */
}


/*
 * relation_is_updatable - 确定指定关系
 * 支持哪些更新事件。
 *
 * 注意视图可能混合可更新列与不可更新列。
 * 视图要支持 INSERT/UPDATE，必须至少有一个可更新
 * 列，但 DELETE 没有这种限制。
 * 若 include_cols 非 NULL，则测试可更新性时
 * 只考虑指定的列。
 *
 * 与前面的函数不同，本函数会递归查看视图的
 * 基关系，因此需要检测递归。为此，我们传入
 * 当前正在考虑的外层关系列表。外部调用者只需
 * 传入 NIL。
 *
 * 这用于 information_schema 视图，它们把可更新和
 * 触发器可更新分成不同概念。关系是可更新的，若它
 * 不需要触发器就能被更新（要么因为它有合适的
 * RULE，要么因为它简单到可以自动更新）。
 * 关系是触发器可更新的，若它有合适的 INSTEAD OF 触发器。
 * SQL 标准认为这不一定可更新，大概
 * 是因为无法知道触发器实际上会做什么。
 * 因此 information_schema 视图调用本函数时
 * include_triggers = false。不过，其他调用者也许只关心
 * 修改数据的 SQL 能否工作，因此可以传入 include_triggers = true，
 * 把触发器可更新性也包括进结果。
 *
 * 返回值是规则事件号的位掩码，表明支持
 * INSERT、UPDATE 和 DELETE 中的哪些操作。（我们这样做，
 * 以便一次调用就能测试 UPDATE 加 DELETE 支持。）
 */
int
relation_is_updatable(Oid reloid,
					  List *outer_reloids,
					  bool include_triggers,
					  Bitmapset *include_cols)
{
	int			events = 0;
	Relation	rel;
	RuleLock   *rulelocks;

#define ALL_EVENTS ((1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE))

	/* 由于本函数会递归，它可能被逼到栈溢出 */
	check_stack_depth();

	rel = try_relation_open(reloid, AccessShareLock);

	/*
	 * 若关系不存在，返回零而不是报错。
	 * 这有帮助，因为在 MVCC 规则下扫描 information_schema 视图
	 * 可能引用实际上已经被
	 * 删除的关系。
	 */
	if (rel == NULL)
		return 0;

	/* 若检测到递归视图，就报告它不可更新 */
	if (list_member_oid(outer_reloids, RelationGetRelid(rel)))
	{
		relation_close(rel, AccessShareLock);
		return 0;
	}

	/* 若关系是表，它始终可更新 */
	if (rel->rd_rel->relkind == RELKIND_RELATION ||
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		relation_close(rel, AccessShareLock);
		return ALL_EVENTS;
	}

	/* 查找无条件的 DO INSTEAD 规则，并记下支持的事件 */
	rulelocks = rel->rd_rules;
	if (rulelocks != NULL)
	{
		int			i;

		for (i = 0; i < rulelocks->numLocks; i++)
		{
			if (rulelocks->rules[i]->isInstead &&
				rulelocks->rules[i]->qual == NULL)
			{
				events |= ((1 << rulelocks->rules[i]->event) & ALL_EVENTS);
			}
		}

		/* 若我们有全部事件的规则，就完成了 */
		if (events == ALL_EVENTS)
		{
			relation_close(rel, AccessShareLock);
			return events;
		}
	}

	/* 若要包括触发器，同样查找 INSTEAD OF 触发器 */
	if (include_triggers)
	{
		TriggerDesc *trigDesc = rel->trigdesc;

		if (trigDesc)
		{
			if (trigDesc->trig_insert_instead_row)
				events |= (1 << CMD_INSERT);
			if (trigDesc->trig_update_instead_row)
				events |= (1 << CMD_UPDATE);
			if (trigDesc->trig_delete_instead_row)
				events |= (1 << CMD_DELETE);

			/* 若我们有全部事件的触发器，就完成了 */
			if (events == ALL_EVENTS)
			{
				relation_close(rel, AccessShareLock);
				return events;
			}
		}
	}

	/* 若这是外部表，检查它支持哪些更新事件 */
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		FdwRoutine *fdwroutine = GetFdwRoutineForRelation(rel, false);

		if (fdwroutine->IsForeignRelUpdatable != NULL)
			events |= fdwroutine->IsForeignRelUpdatable(rel);
		else
		{
			/* 假定存在执行器函数就足够了 */
			if (fdwroutine->ExecForeignInsert != NULL)
				events |= (1 << CMD_INSERT);
			if (fdwroutine->ExecForeignUpdate != NULL)
				events |= (1 << CMD_UPDATE);
			if (fdwroutine->ExecForeignDelete != NULL)
				events |= (1 << CMD_DELETE);
		}

		relation_close(rel, AccessShareLock);
		return events;
	}

	/* 检查这是否为自动可更新视图 */
	if (rel->rd_rel->relkind == RELKIND_VIEW)
	{
		Query	   *viewquery = get_view_query(rel);

		if (view_query_is_auto_updatable(viewquery, false) == NULL)
		{
			Bitmapset  *updatable_cols;
			int			auto_events;
			RangeTblRef *rtr;
			RangeTblEntry *base_rte;
			Oid			baseoid;

			/*
			 * 确定视图的哪些列是可更新的。若我们正在查看的
			 * 列集合里一个都没有，则该
			 * 视图不支持 INSERT/UPDATE，但它仍可能支持
			 * DELETE。
			 */
			view_cols_are_auto_updatable(viewquery, NULL,
										 &updatable_cols, NULL);

			if (include_cols != NULL)
				updatable_cols = bms_int_members(updatable_cols, include_cols);

			if (bms_is_empty(updatable_cols))
				auto_events = (1 << CMD_DELETE);	/* 可能支持 DELETE */
			else
				auto_events = ALL_EVENTS;	/* 可能支持全部事件 */

			/*
			 * 基关系也必须支持这些更新命令。
			 * 表始终可更新，但对任何其他种类的基
			 * 关系，我们必须做一次递归检查，限于本视图里
			 * 局部可更新列所引用的列。
			 */
			rtr = (RangeTblRef *) linitial(viewquery->jointree->fromlist);
			base_rte = rt_fetch(rtr->rtindex, viewquery->rtable);
			Assert(base_rte->rtekind == RTE_RELATION);

			if (base_rte->relkind != RELKIND_RELATION &&
				base_rte->relkind != RELKIND_PARTITIONED_TABLE)
			{
				baseoid = base_rte->relid;
				outer_reloids = lappend_oid(outer_reloids,
											RelationGetRelid(rel));
				include_cols = adjust_view_column_set(updatable_cols,
													  viewquery->targetList);
				auto_events &= relation_is_updatable(baseoid,
													 outer_reloids,
													 include_triggers,
													 include_cols);
				outer_reloids = list_delete_last(outer_reloids);
			}
			events |= auto_events;
		}
	}

	/* 若到达这里，该关系可能支持某些更新命令 */
	relation_close(rel, AccessShareLock);
	return events;
}


/*
 * adjust_view_column_set - 按目标列表映射一组列号。
 *
 * 这用于简单可更新视图，把视图列的列权限集合
 * 映射到下层基关系里匹配的列上。
 * 目标列表里的相关项必须是下层
 * 关系的普通 Var（如同上面 view_query_is_auto_updatable 里的检查）。
 */
static Bitmapset *
adjust_view_column_set(Bitmapset *cols, List *targetlist)
{
	Bitmapset  *result = NULL;
	int			col;

	col = -1;
	while ((col = bms_next_member(cols, col)) >= 0)
	{
		/* 位号以 FirstLowInvalidHeapAttributeNumber 为偏移 */
		AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno == InvalidAttrNumber)
		{
			/*
			 * 有一个对视图的整行引用。为了权限
			 * 目的，把它当作对视图可用的每一列的引用。
			 * （我们不应当把它转换成对基关系的整行
			 * 引用，因为视图可能没有触及
			 * 基关系的全部列。）
			 */
			ListCell   *lc;

			foreach(lc, targetlist)
			{
				TargetEntry *tle = lfirst_node(TargetEntry, lc);
				Var		   *var;

				if (tle->resjunk)
					continue;
				var = castNode(Var, tle->expr);
				result = bms_add_member(result,
										var->varattno - FirstLowInvalidHeapAttributeNumber);
			}
		}
		else
		{
			/*
			 * 视图没有系统列，因此我们不预期在这里看到
			 * 任何其他系统属性号。若真找到一个，就适用
			 * 错误情形。
			 */
			TargetEntry *tle = get_tle_by_resno(targetlist, attno);

			if (tle != NULL && !tle->resjunk && IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;

				result = bms_add_member(result,
										var->varattno - FirstLowInvalidHeapAttributeNumber);
			}
			else
				elog(ERROR, "attribute number %d not found in view targetlist",
					 attno);
		}
	}

	return result;
}


/*
 * rewriteTargetView -
 *	  尝试重写目标关系是视图的查询，使
 *	  视图的基关系成为目标关系。
 *
 * 注意这里的基关系本身可能是视图，它可能有、也可能没有
 * INSTEAD OF 触发器或规则来处理更新。那由
 * RewriteQuery 里的递归处理。
 */
static Query *
rewriteTargetView(Query *parsetree, Relation view)
{
	Query	   *viewquery;
	const char *auto_update_detail;
	RangeTblRef *rtr;
	int			base_rt_index;
	int			new_rt_index;
	RangeTblEntry *base_rte;
	RangeTblEntry *view_rte;
	RangeTblEntry *new_rte;
	RTEPermissionInfo *base_perminfo;
	RTEPermissionInfo *view_perminfo;
	RTEPermissionInfo *new_perminfo;
	Relation	base_rel;
	List	   *view_targetlist;
	ListCell   *lc;

	/*
	 * 从视图的 ON SELECT 规则取得 Query。我们要改动该
	 * Query，把视图的基关系变成目标关系，
	 * 途中还有其他各种改动，因此需要制作
	 * 一份副本（get_view_query() 返回指向 relcache 的指针，所以我们
	 * 必须把它当作只读）。
	 */
	viewquery = copyObject(get_view_query(view));

	/* 在外层查询里定位描述该视图的 RTE 和 perminfo */
	view_rte = rt_fetch(parsetree->resultRelation, parsetree->rtable);
	view_perminfo = getRTEPermissionInfo(parsetree->rteperminfos, view_rte);

	/* 视图必须可更新，否则失败 */
	auto_update_detail =
		view_query_is_auto_updatable(viewquery,
									 parsetree->commandType != CMD_DELETE);

	if (auto_update_detail)
	{
		/* 这里的信息应当与 execMain.c 的 CheckValidResultRel 一致 */
		switch (parsetree->commandType)
		{
			case CMD_INSERT:
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot insert into view \"%s\"",
								RelationGetRelationName(view)),
						 errdetail_internal("%s", _(auto_update_detail)),
						 errhint("To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.")));
				break;
			case CMD_UPDATE:
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot update view \"%s\"",
								RelationGetRelationName(view)),
						 errdetail_internal("%s", _(auto_update_detail)),
						 errhint("To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.")));
				break;
			case CMD_DELETE:
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("cannot delete from view \"%s\"",
								RelationGetRelationName(view)),
						 errdetail_internal("%s", _(auto_update_detail)),
						 errhint("To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.")));
				break;
			default:
				elog(ERROR, "unrecognized CmdType: %d",
					 (int) parsetree->commandType);
				break;
		}
	}

	/* 检查非系统视图的展开是否受限 */
	if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_VIEW) != 0 &&
				 RelationGetRelid(view) >= FirstNormalObjectId))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("access to non-system view \"%s\" is restricted",
						RelationGetRelationName(view))));

	/*
	 * 对 INSERT/UPDATE，被修改的列必须全部可更新。
	 */
	if (parsetree->commandType != CMD_DELETE)
	{
		Bitmapset  *modified_cols;
		char	   *non_updatable_col;

		/*
		 * 把被修改列的集合算成结果
		 * RTE 的 insertedCols 和/或 updatedCols 集合里列出的那些，再加上
		 * 查询目标列表的目标列。我们必须考虑查询的
		 * targetlist，因为 rewriteTargetListIU 可能已经为视图默认值加入了额外的
		 * 目标列表项，这些也必须
		 * 可更新。但若它们是 DEFAULT 标记且该列的默认值为 NULL，
		 * rewriteTargetListIU 也可能去掉项，因此
		 * 只考虑目标列表同样是错的。
		 */
		modified_cols = bms_union(view_perminfo->insertedCols,
								  view_perminfo->updatedCols);

		foreach(lc, parsetree->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			if (!tle->resjunk)
				modified_cols = bms_add_member(modified_cols,
											   tle->resno - FirstLowInvalidHeapAttributeNumber);
		}

		if (parsetree->onConflict)
		{
			foreach(lc, parsetree->onConflict->onConflictSet)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				if (!tle->resjunk)
					modified_cols = bms_add_member(modified_cols,
												   tle->resno - FirstLowInvalidHeapAttributeNumber);
			}
		}

		auto_update_detail = view_cols_are_auto_updatable(viewquery,
														  modified_cols,
														  NULL,
														  &non_updatable_col);
		if (auto_update_detail)
		{
			/*
			 * 这是另一种错误，由试图更新
			 * 在其他方面可更新的视图里的不可更新列引起。
			 */
			switch (parsetree->commandType)
			{
				case CMD_INSERT:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot insert into column \"%s\" of view \"%s\"",
									non_updatable_col,
									RelationGetRelationName(view)),
							 errdetail_internal("%s", _(auto_update_detail))));
					break;
				case CMD_UPDATE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot update column \"%s\" of view \"%s\"",
									non_updatable_col,
									RelationGetRelationName(view)),
							 errdetail_internal("%s", _(auto_update_detail))));
					break;
				default:
					elog(ERROR, "unrecognized CmdType: %d",
						 (int) parsetree->commandType);
					break;
			}
		}
	}

	/*
	 * 若到达这里，view_query_is_auto_updatable() 已经验证该
	 * 视图包含一个单一基关系。
	 */
	Assert(list_length(viewquery->jointree->fromlist) == 1);
	rtr = linitial_node(RangeTblRef, viewquery->jointree->fromlist);

	base_rt_index = rtr->rtindex;
	base_rte = rt_fetch(base_rt_index, viewquery->rtable);
	Assert(base_rte->rtekind == RTE_RELATION);
	base_perminfo = getRTEPermissionInfo(viewquery->rteperminfos, base_rte);

	/*
	 * 到目前为止，基关系在我们的查询里完全没被触及。
	 * 在对它做任何事之前，我们需要取得它的锁。
	 * （随后对 RewriteQuery 的递归调用会假定我们
	 * 已经持有正确的锁！）由于它将成为查询的目标
	 * 关系，RowExclusiveLock 始终是正确的选择。
	 */
	base_rel = table_open(base_rte->relid, RowExclusiveLock);

	/*
	 * 既然关系已经打开，就更新 RTE 的 relkind，以防
	 * 自该视图建立以来它已经改变（参见 AcquireRewriteLocks）。
	 */
	base_rte->relkind = base_rel->rd_rel->relkind;

	/*
	 * 若视图查询含有任何子链接子查询，我们还需要
	 * 取得它们所引用的任何关系上的锁。我们知道范围表或
	 * CTEs 里不会有任何子查询，因此可以跳过那些，如同
	 * AcquireRewriteLocks 那样。
	 */
	if (viewquery->hasSubLinks)
	{
		acquireLocksOnSubLinks_context context;

		context.for_execute = true;
		query_tree_walker(viewquery, acquireLocksOnSubLinks, &context,
						  QTW_IGNORE_RC_SUBQUERIES);
	}

	/*
	 * 创建一个描述基关系的新目标 RTE，并把它加入
	 * 外层查询的范围表。（接下来几步所发生的事
	 * 非常像规划器把视图上拉进
	 * 外层查询时所做的。也许将来我们应当把事情重构到
	 * 足以与规划器共享代码。）
	 *
	 * 务必把 rellockmode 设成目标表的正确值。
	 * 由于我们在上面复制了整个 viewquery，可以直接就地修改
	 * base_rte，而不必复制它。
	 */
	new_rte = base_rte;
	new_rte->rellockmode = RowExclusiveLock;

	parsetree->rtable = lappend(parsetree->rtable, new_rte);
	new_rt_index = list_length(parsetree->rtable);

	/*
	 * INSERTs 从不继承。对 UPDATE/DELETE，我们用视图查询的
	 * 继承标志作为基关系的标志。
	 */
	if (parsetree->commandType == CMD_INSERT)
		new_rte->inh = false;

	/*
	 * 调整视图目标列表的 Var，使它们引用新的目标 RTE，即
	 * 把它们的 varno 改成 new_rt_index 而不是 base_rt_index。目标列表里
	 * 不可能有其他关系的 Var，因此这就足以把
	 * 目标列表表达式上拉出来供外层查询使用。该目标列表将
	 * 提供下面 ReplaceVarsFromTargetList
	 * 所用的替换表达式。
	 */
	view_targetlist = viewquery->targetList;

	ChangeVarNodes((Node *) view_targetlist,
				   base_rt_index,
				   new_rt_index,
				   0);

	/*
	 * 若视图设置了 security_invoker，就把新的目标关系
	 * 标成我们要对查询
	 * 调用者强制的权限检查。否则我们要对视图所有者强制它们。
	 *
	 * 在关系层次上，需要与查询调用者对该视图所需的相同
	 * INSERT/UPDATE/DELETE 权限。我们丢掉
	 * new_perminfo->requiredPerms 里最初大概会有的
	 * ACL_SELECT 位。
	 *
	 * 注意：原来视图的 RTEPermissionInfo 仍留在查询的
	 * rteperminfos 里，这样执行器仍会对查询调用者使用该视图
	 * 执行适当的权限检查。
	 *
	 * 不要理会 viewquery->rteperminfos 里 base_rte
	 * 当前会指向的那个 perminfo，因为我们希望它现在指向
	 * 下面将要填充的一个新的。必须把 perminfoindex 设为 0，以免
	 * 绊倒 addRTEPermissionInfo() 里的 Assert。
	 */
	new_rte->perminfoindex = 0;
	new_perminfo = addRTEPermissionInfo(&parsetree->rteperminfos, new_rte);
	if (RelationHasSecurityInvoker(view))
		new_perminfo->checkAsUser = InvalidOid;
	else
		new_perminfo->checkAsUser = view->rd_rel->relowner;
	new_perminfo->requiredPerms = view_perminfo->requiredPerms;

	/*
	 * 现在处理按列的权限位。
	 *
	 * 起初，new_perminfo（即 base_perminfo）含有视图所引用的
	 * 全部基关系列的 selectedCols
	 * 权限检查位，但由于该视图是 SELECT 查询，它的 insertedCols/updatedCols
	 * 是空的。我们把 insertedCols 和 updatedCols 设成包括外层查询
	 * 试图修改的全部列，并按需要调整列号。
	 * 但 selectedCols 保持原样，因此视图所有者必须对
	 * 视图定义里使用的全部列有读权限，即使
	 * 其中一些不被外层查询读取。我们可以尝试把
	 * selectedCols 限制为变换后的查询里使用的列，但那
	 * 并不对应于普通 SELECT 使用视图时发生的事：
	 * 所有被引用的列都必须有读权限，即使规划器
	 * 发现其中一些可以在查询变换期间丢弃。
	 * 我们在这里做的展平也是一种可选优化。（若
	 * 你不信服并想改变这一点，请注意对 view_perminfo->selectedCols
	 * 应用 adjust_view_column_set 显然不是
	 * 正确答案，因为那会忽略视图 WHERE 限定条件里使用的
	 * 基关系列。）
	 *
	 * 这一步需要修改后的视图目标列表，因此我们必须按
	 * 这个顺序做事。
	 */
	Assert(bms_is_empty(new_perminfo->insertedCols) &&
		   bms_is_empty(new_perminfo->updatedCols));

	new_perminfo->selectedCols = base_perminfo->selectedCols;

	new_perminfo->insertedCols =
		adjust_view_column_set(view_perminfo->insertedCols, view_targetlist);

	new_perminfo->updatedCols =
		adjust_view_column_set(view_perminfo->updatedCols, view_targetlist);

	/*
	 * 把任何安全屏障限定条件从视图 RTE 移到新的目标
	 * RTE 上。任何这类限定条件现在应当作用于新的目标 RTE，并且在
	 * 重写后的查询里不会引用原来的视图 RTE。
	 */
	new_rte->securityQuals = view_rte->securityQuals;
	view_rte->securityQuals = NIL;

	/*
	 * 现在更新外层查询里所有引用该视图的 Var，使它们改为
	 * 引用基关系的适当列。
	 */
	parsetree = (Query *)
		ReplaceVarsFromTargetList((Node *) parsetree,
								  parsetree->resultRelation,
								  0,
								  view_rte,
								  view_targetlist,
								  REPLACEVARS_REPORT_ERROR,
								  0,
								  NULL);

	/*
	 * 更新查询里所有指向该视图的其他 RTI 引用
	 * （例如 parsetree->resultRelation 本身），使它们改为指向新的
	 * 基关系。Var 不会受影响，因为它们都不再
	 * 引用 parsetree->resultRelation。
	 */
	ChangeVarNodes((Node *) parsetree,
				   parsetree->resultRelation,
				   new_rt_index,
				   0);
	Assert(parsetree->resultRelation == new_rt_index);

	/*
	 * 对 INSERT/UPDATE，我们还必须更新目标列表里的 resno，使它们引用
	 * 基关系的列，因为那些指明要影响的目标
	 * 列。
	 *
	 * 注意这会破坏目标列表的 resno 顺序，但
	 * 当我们递归经过 RewriteQuery 时会修好，它会再次对更新后的目标列表
	 * 调用 rewriteTargetListIU。
	 */
	if (parsetree->commandType != CMD_DELETE)
	{
		foreach(lc, parsetree->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			TargetEntry *view_tle;

			if (tle->resjunk)
				continue;

			view_tle = get_tle_by_resno(view_targetlist, tle->resno);
			if (view_tle != NULL && !view_tle->resjunk && IsA(view_tle->expr, Var))
				tle->resno = ((Var *) view_tle->expr)->varattno;
			else
				elog(ERROR, "attribute number %d not found in view targetlist",
					 tle->resno);
		}
	}

	/*
	 * 对 INSERT .. ON CONFLICT .. DO UPDATE，我们还必须更新
	 * onConflict 数据结构里的各样东西。
	 */
	if (parsetree->onConflict &&
		parsetree->onConflict->action == ONCONFLICT_UPDATE)
	{
		Index		old_exclRelIndex,
					new_exclRelIndex;
		ParseNamespaceItem *new_exclNSItem;
		RangeTblEntry *new_exclRte;
		List	   *tmp_tlist;

		/*
		 * 与上面的 INSERT/UPDATE 代码一样，更新
		 * 辅助 UPDATE 目标列表里的 resno，使它们引用基
		 * 关系的列。
		 */
		foreach(lc, parsetree->onConflict->onConflictSet)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			TargetEntry *view_tle;

			if (tle->resjunk)
				continue;

			view_tle = get_tle_by_resno(view_targetlist, tle->resno);
			if (view_tle != NULL && !view_tle->resjunk && IsA(view_tle->expr, Var))
				tle->resno = ((Var *) view_tle->expr)->varattno;
			else
				elog(ERROR, "attribute number %d not found in view targetlist",
					 tle->resno);
		}

		/*
		 * 另外，用查询的新基关系为 EXCLUDED 伪关系创建一个新 RTE
		 * （它的列清单很可能与视图不同，
		 * 因此我们需要一份新的列别名列表）。这应当
		 * 与 transformOnConflictClause 一致。尤其要注意，
		 * relkind 被设为 composite，以表明我们处理的不是
		 * 一个实际的关系。
		 */
		old_exclRelIndex = parsetree->onConflict->exclRelIndex;

		new_exclNSItem = addRangeTableEntryForRelation(make_parsestate(NULL),
													   base_rel,
													   RowExclusiveLock,
													   makeAlias("excluded", NIL),
													   false, false);
		new_exclRte = new_exclNSItem->p_rte;
		new_exclRte->relkind = RELKIND_COMPOSITE_TYPE;
		/* 忽略本会加入的 RTEPermissionInfo。 */
		new_exclRte->perminfoindex = 0;

		parsetree->rtable = lappend(parsetree->rtable, new_exclRte);
		new_exclRelIndex = parsetree->onConflict->exclRelIndex =
			list_length(parsetree->rtable);

		/*
		 * 把 EXCLUDED 伪关系的目标列表替换成一个新的，
		 * 表示来自新基关系的列。
		 */
		parsetree->onConflict->exclRelTlist =
			BuildOnConflictExcludedTargetlist(base_rel, new_exclRelIndex);

		/*
		 * 更新 ON CONFLICT 子句里引用旧
		 * EXCLUDED 伪关系的全部 Var。我们想使用
		 * 视图目标列表里定义的列映射，但需要输出引用
		 * 新的 EXCLUDED 伪关系，而不是新的目标 RTE。
		 * 还要注意 EXCLUDED.* 会用视图的
		 * 行类型展开，这看起来是对的。
		 */
		tmp_tlist = copyObject(view_targetlist);

		ChangeVarNodes((Node *) tmp_tlist, new_rt_index,
					   new_exclRelIndex, 0);

		parsetree->onConflict = (OnConflictExpr *)
			ReplaceVarsFromTargetList((Node *) parsetree->onConflict,
									  old_exclRelIndex,
									  0,
									  view_rte,
									  tmp_tlist,
									  REPLACEVARS_REPORT_ERROR,
									  0,
									  &parsetree->hasSubLinks);
	}

	/*
	 * 对 UPDATE/DELETE，把视图里的任何 WHERE 限定条件上拉。我们知道
	 * 限定条件里的任何 Var 都必须引用那一个基关系，因此我们只需
	 * 调整它们的 varno 以引用新目标（与
	 * 我们对视图目标列表所做的一样）。
	 *
	 * 若它是安全屏障视图，它的 WHERE 限定条件必须先于
	 * 外层查询的限定条件被应用，因此我们把它们作为安全
	 * 屏障限定条件挂到 RTE 上，而不是加进主 WHERE 子句。
	 *
	 * 对 INSERT，视图的限定条件在主查询里可以忽略。
	 */
	if (parsetree->commandType != CMD_INSERT &&
		viewquery->jointree->quals != NULL)
	{
		Node	   *viewqual = (Node *) viewquery->jointree->quals;

		/*
		 * 尽管我们在本函数开头已经复制了 viewquery，
		 * 这里仍必须再次复制 viewqual，因为下面为
		 * WithCheckOption 子句可能还要用到这些限定条件。
		 */
		viewqual = copyObject(viewqual);

		ChangeVarNodes(viewqual, base_rt_index, new_rt_index, 0);

		if (RelationIsSecurityView(view))
		{
			/*
			 * 视图的限定条件放在已有屏障限定条件前面：那些
			 * 会来自更外层的安全屏障视图，
			 * 因此必须更晚求值。
			 *
			 * 注意：语法分析树已被改动，因此 new_rte 指针已经
			 * 过时，需要重新计算。
			 */
			new_rte = rt_fetch(new_rt_index, parsetree->rtable);
			new_rte->securityQuals = lcons(viewqual, new_rte->securityQuals);

			/*
			 * 不要设置 parsetree->hasRowSecurity，因为这些不是行安全性
			 * 条件（它们不受启用或禁用行安全性的影响）。
			 */

			/*
			 * 若加入的限定条件含有子链接，确保查询被
			 * 正确标记。
			 */
			if (!parsetree->hasSubLinks)
				parsetree->hasSubLinks = checkExprHasSubLink(viewqual);
		}
		else
			AddQual(parsetree, (Node *) viewqual);
	}

	/*
	 * 对 INSERT/UPDATE，若视图有 WITH CHECK OPTION，或任何父
	 * 视图指定了 WITH CASCADED CHECK OPTION，就把视图的限定条件
	 * 加入查询的 withCheckOptions 列表。
	 */
	if (parsetree->commandType != CMD_DELETE)
	{
		bool		has_wco = RelationHasCheckOption(view);
		bool		cascaded = RelationHasCascadedCheckOption(view);

		/*
		 * 若父视图有级联检查选项，就把本视图当作
		 * 也有级联检查选项。
		 *
		 * 新的 WithCheckOptions 被加到列表开头，因此若
		 * 有级联检查选项，它会是列表里的
		 * 第一项。
		 */
		if (parsetree->withCheckOptions != NIL)
		{
			WithCheckOption *parent_wco =
				(WithCheckOption *) linitial(parsetree->withCheckOptions);

			if (parent_wco->cascaded)
			{
				has_wco = true;
				cascaded = true;
			}
		}

		/*
		 * 把新的 WithCheckOption 加到列表开头，使
		 * 内层视图的检查先于外层视图的检查运行，如同
		 * SQL 标准所要求的。
		 *
		 * 若新检查是 CASCADED，即使本视图
		 * 没有限定条件也需要加上它，因为子视图上可能有限定条件。LOCAL
		 * 检查在本视图没有限定条件时可以省略。
		 */
		if (has_wco && (cascaded || viewquery->jointree->quals != NULL))
		{
			WithCheckOption *wco;

			wco = makeNode(WithCheckOption);
			wco->kind = WCO_VIEW_CHECK;
			wco->relname = pstrdup(RelationGetRelationName(view));
			wco->polname = NULL;
			wco->qual = NULL;
			wco->cascaded = cascaded;

			parsetree->withCheckOptions = lcons(wco,
												parsetree->withCheckOptions);

			if (viewquery->jointree->quals != NULL)
			{
				wco->qual = (Node *) viewquery->jointree->quals;
				ChangeVarNodes(wco->qual, base_rt_index, new_rt_index, 0);

				/*
				 * 若加入的限定条件含有子链接，确保查询被
				 * 正确标记。若查询已经标过，或者命令是
				 * UPDATE，就可以跳过这项检查，那种情况下
				 * 同样的限定条件已经加过，这项
				 * 检查也已经做过。
				 */
				if (!parsetree->hasSubLinks &&
					parsetree->commandType != CMD_UPDATE)
					parsetree->hasSubLinks = checkExprHasSubLink(wco->qual);
			}
		}
	}

	table_close(base_rel, NoLock);

	return parsetree;
}


/*
 * RewriteQuery -
 *	  重写查询，并再次对重写后的查询应用规则。
 *
 * rewrite_events 是尚未结束的查询重写动作列表，以便我们检测
 * 无限递归。
 *
 * orig_rt_length 是发起查询的 rtable 长度，用于 fireRules()
 * 创建的产物查询，否则为 0。这用来跳过
 * 来自原始查询的、已经处理过的 VALUES RTEs。
 */
static List *
RewriteQuery(Query *parsetree, List *rewrite_events, int orig_rt_length)
{
	CmdType		event = parsetree->commandType;
	bool		instead = false;
	bool		returning = false;
	bool		updatableview = false;
	Query	   *qual_product = NULL;
	List	   *rewritten = NIL;
	ListCell   *lc1;

	/*
	 * 首先，递归处理 WITH
	 * 子句里的任何插入、更新或删除语句。（我们必须先做这个，因为 WITH 子句可能被
	 * 复制进下面的规则动作。）
	 */
	foreach(lc1, parsetree->cteList)
	{
		CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc1);
		Query	   *ctequery = castNode(Query, cte->ctequery);
		List	   *newstuff;

		if (ctequery->commandType == CMD_SELECT)
			continue;

		newstuff = RewriteQuery(ctequery, rewrite_events, 0);

		/*
		 * 目前我们只能正确处理无条件的、单语句 DO
		 * INSTEAD 规则；我们必须从重写操作里恰好得到一个非实用
		 * Query，再塞回 CTE 节点。
		 */
		if (list_length(newstuff) == 1)
		{
			/* 必须检查它不是实用命令 */
			ctequery = linitial_node(Query, newstuff);
			if (!(ctequery->commandType == CMD_SELECT ||
				  ctequery->commandType == CMD_UPDATE ||
				  ctequery->commandType == CMD_INSERT ||
				  ctequery->commandType == CMD_DELETE))
			{
				/*
				 * 目前它只可能是 NOTIFY；若我们将来允许规则里有其他实用命令，
				 * 这条错误信息需要再改。
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("DO INSTEAD NOTIFY rules are not supported for data-modifying statements in WITH")));
			}
			/* WITH 查询绝不应当是 canSetTag */
			Assert(!ctequery->canSetTag);
			/* 把那一个 Query 推回 CTE 节点 */
			cte->ctequery = (Node *) ctequery;
		}
		else if (newstuff == NIL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("DO INSTEAD NOTHING rules are not supported for data-modifying statements in WITH")));
		}
		else
		{
			ListCell   *lc2;

			/* 检查这些查询，以决定发出哪条错误信息 */
			foreach(lc2, newstuff)
			{
				Query	   *q = (Query *) lfirst(lc2);

				if (q->querySource == QSRC_QUAL_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("conditional DO INSTEAD rules are not supported for data-modifying statements in WITH")));
				if (q->querySource == QSRC_NON_INSTEAD_RULE)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("DO ALSO rules are not supported for data-modifying statements in WITH")));
			}

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("multi-statement DO INSTEAD rules are not supported for data-modifying statements in WITH")));
		}
	}

	/*
	 * 若语句是插入、更新、删除或合并，按需要调整它的
	 * 目标列表，然后对它触发 INSERT/UPDATE/DELETE 规则。
	 *
	 * SELECT 规则稍后处理，那时我们才有全部应当
	 * 被执行的查询。另外，实用语句完全不被重写（我们还
	 * 需要那项检查吗？）
	 */
	if (event != CMD_SELECT && event != CMD_UTILITY)
	{
		int			result_relation;
		RangeTblEntry *rt_entry;
		Relation	rt_entry_relation;
		List	   *locks;
		int			product_orig_rt_length;
		List	   *product_queries;
		bool		hasUpdate = false;
		int			values_rte_index = 0;
		bool		defaults_remaining = false;

		result_relation = parsetree->resultRelation;
		Assert(result_relation != 0);
		rt_entry = rt_fetch(result_relation, parsetree->rtable);
		Assert(rt_entry->rtekind == RTE_RELATION);

		/*
		 * 这里可以用 NoLock，因为解析器或
		 * AcquireRewriteLocks 应当已经锁定了该关系。
		 */
		rt_entry_relation = table_open(rt_entry->relid, NoLock);

		/*
		 * 按命令类型按需要重写目标列表。
		 */
		if (event == CMD_INSERT)
		{
			ListCell   *lc2;
			RangeTblEntry *values_rte = NULL;

			/*
			 * 通过在 fromlist 里查找 VALUES RTE，测试它是否为多行
			 * INSERT ... VALUES (...), (...), ...。对产物查询，
			 * 我们必须忽略来自原始查询的、已经处理过的
			 * VALUES RTEs。它们出现在范围表的开头。
			 */
			foreach(lc2, parsetree->jointree->fromlist)
			{
				RangeTblRef *rtr = (RangeTblRef *) lfirst(lc2);

				if (IsA(rtr, RangeTblRef) && rtr->rtindex > orig_rt_length)
				{
					RangeTblEntry *rte = rt_fetch(rtr->rtindex,
												  parsetree->rtable);

					if (rte->rtekind == RTE_VALUES)
					{
						/* 不应当找到多于一个 VALUES RTE */
						if (values_rte != NULL)
							elog(ERROR, "more than one VALUES RTE found");

						values_rte = rte;
						values_rte_index = rtr->rtindex;
					}
				}
			}

			if (values_rte)
			{
				Bitmapset  *unused_values_attrnos = NULL;

				/* 处理主目标列表…… */
				parsetree->targetList = rewriteTargetListIU(parsetree->targetList,
															parsetree->commandType,
															parsetree->override,
															rt_entry_relation,
															values_rte,
															values_rte_index,
															&unused_values_attrnos);
				/* ……以及 VALUES 表达式列表 */
				if (!rewriteValuesRTE(parsetree, values_rte, values_rte_index,
									  rt_entry_relation,
									  unused_values_attrnos))
					defaults_remaining = true;
			}
			else
			{
				/* 只处理主目标列表 */
				parsetree->targetList =
					rewriteTargetListIU(parsetree->targetList,
										parsetree->commandType,
										parsetree->override,
										rt_entry_relation,
										NULL, 0, NULL);
			}

			if (parsetree->onConflict &&
				parsetree->onConflict->action == ONCONFLICT_UPDATE)
			{
				parsetree->onConflict->onConflictSet =
					rewriteTargetListIU(parsetree->onConflict->onConflictSet,
										CMD_UPDATE,
										parsetree->override,
										rt_entry_relation,
										NULL, 0, NULL);
			}
		}
		else if (event == CMD_UPDATE)
		{
			Assert(parsetree->override == OVERRIDING_NOT_SET);
			parsetree->targetList =
				rewriteTargetListIU(parsetree->targetList,
									parsetree->commandType,
									parsetree->override,
									rt_entry_relation,
									NULL, 0, NULL);
		}
		else if (event == CMD_MERGE)
		{
			Assert(parsetree->override == OVERRIDING_NOT_SET);

			/*
			 * 分别重写每个动作的目标列表
			 */
			foreach(lc1, parsetree->mergeActionList)
			{
				MergeAction *action = (MergeAction *) lfirst(lc1);

				switch (action->commandType)
				{
					case CMD_NOTHING:
					case CMD_DELETE:	/* 这里无事可做 */
						break;
					case CMD_UPDATE:
					case CMD_INSERT:

						/*
						 * MERGE 动作不允许多行 INSERTs，因此
						 * 这里没有 VALUES RTE 要处理。
						 */
						action->targetList =
							rewriteTargetListIU(action->targetList,
												action->commandType,
												action->override,
												rt_entry_relation,
												NULL, 0, NULL);
						break;
					default:
						elog(ERROR, "unrecognized commandType: %d", action->commandType);
						break;
				}
			}
		}
		else if (event == CMD_DELETE)
		{
			/* 这里无事可做 */
		}
		else
			elog(ERROR, "unrecognized commandType: %d", (int) event);

		/*
		 * 收集并应用适当的规则。
		 */
		locks = matchLocks(event, rt_entry_relation->rd_rules,
						   result_relation, parsetree, &hasUpdate);

		product_orig_rt_length = list_length(parsetree->rtable);
		product_queries = fireRules(parsetree,
									result_relation,
									event,
									locks,
									&instead,
									&returning,
									&qual_product);

		/*
		 * 若我们有一个仍含未触碰 DEFAULT 项的 VALUES RTE，
		 * 并且得到了任何产物查询，就为每个
		 * 产物查询收尾该 VALUES RTE（把剩余的 DEFAULT 项替换成 NULLs）。
		 * 我们不对原始查询这样做，因为我们知道它
		 * 必定是视图上的自动插入，因此任何剩余的 DEFAULT 项应当使用基
		 * 关系的默认值。
		 */
		if (defaults_remaining && product_queries != NIL)
		{
			ListCell   *n;

			/*
			 * 每个产物查询在范围表的同一下标处都有自己的 VALUES RTE 副本，
			 * 因此我们必须逐个收尾。
			 *
			 * 注意，若产物查询是 INSERT ... SELECT，则
			 * VALUES RTE 会在产物查询的 SELECT 部分的
			 * 同一下标处，而不是在顶层产物查询
			 * 本身。
			 */
			foreach(n, product_queries)
			{
				Query	   *pt = (Query *) lfirst(n);
				RangeTblEntry *values_rte;

				if (pt->commandType == CMD_INSERT &&
					pt->jointree && IsA(pt->jointree, FromExpr) &&
					list_length(pt->jointree->fromlist) == 1)
				{
					Node	   *jtnode = (Node *) linitial(pt->jointree->fromlist);

					if (IsA(jtnode, RangeTblRef))
					{
						int			rtindex = ((RangeTblRef *) jtnode)->rtindex;
						RangeTblEntry *src_rte = rt_fetch(rtindex, pt->rtable);

						if (src_rte->rtekind == RTE_SUBQUERY &&
							src_rte->subquery &&
							IsA(src_rte->subquery, Query) &&
							src_rte->subquery->commandType == CMD_SELECT)
							pt = src_rte->subquery;
					}
				}

				values_rte = rt_fetch(values_rte_index, pt->rtable);
				if (values_rte->rtekind != RTE_VALUES)
					elog(ERROR, "failed to find VALUES RTE in product query");

				rewriteValuesRTEToNulls(pt, values_rte);
			}
		}

		/*
		 * 若没有无限定条件的 INSTEAD 规则，且目标关系
		 * 是一个没有任何 INSTEAD OF 触发器的视图，看该视图能否被
		 * 自动更新。若能，我们就在这里做必要的查询
		 * 变换，并把结果查询加入
		 * product_queries 列表，以便必要时
		 * 递归重写它。
		 *
		 * 若该视图无法被自动更新，我们就在这里报错，
		 * 这是可以的，因为该查询在运行时反正会失败。在这里
		 * 报错比执行器检查更可取，因为我们有
		 * 关于视图为何不可更新的更详细
		 * 信息。
		 */
		if (!instead &&
			rt_entry_relation->rd_rel->relkind == RELKIND_VIEW &&
			!view_has_instead_trigger(rt_entry_relation, event))
		{
			/*
			 * 若有任何带限定条件的 INSTEAD 规则，就不允许该视图
			 * 被自动更新（需要一条无限定条件的 INSTEAD 规则或
			 * INSTEAD OF 触发器）。
			 *
			 * 这里的信息应当与 execMain.c 的 CheckValidResultRel
			 * 一致，原则上使执行器里的那些检查不再必要，但
			 * 我们仍保留它们以防万一。
			 */
			if (qual_product != NULL)
			{
				switch (parsetree->commandType)
				{
					case CMD_INSERT:
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot insert into view \"%s\"",
										RelationGetRelationName(rt_entry_relation)),
								 errdetail("Views with conditional DO INSTEAD rules are not automatically updatable."),
								 errhint("To enable inserting into the view, provide an INSTEAD OF INSERT trigger or an unconditional ON INSERT DO INSTEAD rule.")));
						break;
					case CMD_UPDATE:
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot update view \"%s\"",
										RelationGetRelationName(rt_entry_relation)),
								 errdetail("Views with conditional DO INSTEAD rules are not automatically updatable."),
								 errhint("To enable updating the view, provide an INSTEAD OF UPDATE trigger or an unconditional ON UPDATE DO INSTEAD rule.")));
						break;
					case CMD_DELETE:
						ereport(ERROR,
								(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
								 errmsg("cannot delete from view \"%s\"",
										RelationGetRelationName(rt_entry_relation)),
								 errdetail("Views with conditional DO INSTEAD rules are not automatically updatable."),
								 errhint("To enable deleting from the view, provide an INSTEAD OF DELETE trigger or an unconditional ON DELETE DO INSTEAD rule.")));
						break;
					default:
						elog(ERROR, "unrecognized CmdType: %d",
							 (int) parsetree->commandType);
						break;
				}
			}

			/*
			 * 尝试重写查询以自动更新该视图。
			 * 若视图没法被自动
			 * 更新，这里会报错。
			 */
			parsetree = rewriteTargetView(parsetree, rt_entry_relation);

			/*
			 * 此时 product_queries 含有任何 DO ALSO 规则
			 * 动作。把重写后的查询加在那些之前或之后。这
			 * 必须与下面原始查询本会得到的处理
			 * 一致，若我们允许它被再次包括进来。
			 */
			if (parsetree->commandType == CMD_INSERT)
				product_queries = lcons(parsetree, product_queries);
			else
				product_queries = lappend(product_queries, parsetree);

			/*
			 * 设置 instead 标志，仿佛曾有一条无限定条件的
			 * INSTEAD，以防止原始查询在下面被第二次
			 * 包括进来。变换会已经重写任何
			 * RETURNING 列表，因此我们也可以设置 returning，以免
			 * 在下面报错。
			 */
			instead = true;
			returning = true;
			updatableview = true;
		}

		/*
		 * 若得到了任何产物查询，就递归重写它们，但
		 * 先检查递归！
		 */
		if (product_queries != NIL)
		{
			ListCell   *n;
			rewrite_event *rev;

			foreach(n, rewrite_events)
			{
				rev = (rewrite_event *) lfirst(n);
				if (rev->relation == RelationGetRelid(rt_entry_relation) &&
					rev->event == event)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("infinite recursion detected in rules for relation \"%s\"",
									RelationGetRelationName(rt_entry_relation))));
			}

			rev = (rewrite_event *) palloc(sizeof(rewrite_event));
			rev->relation = RelationGetRelid(rt_entry_relation);
			rev->event = event;
			rewrite_events = lappend(rewrite_events, rev);

			foreach(n, product_queries)
			{
				Query	   *pt = (Query *) lfirst(n);
				List	   *newstuff;

				/*
				 * 对可更新视图，pt 可能是原始查询的重写版，
				 * 那种情况下我们把 orig_rt_length 传下去，
				 * 以完成对它所含任何 VALUES RTE 的处理。
				 *
				 * 否则，我们有一个由 fireRules() 创建的产物查询。
				 * 来自原始查询的任何 VALUES RTEs 都已经完全
				 * 处理过，递归时必须跳过。
				 */
				newstuff = RewriteQuery(pt, rewrite_events,
										pt == parsetree ?
										orig_rt_length :
										product_orig_rt_length);
				rewritten = list_concat(rewritten, newstuff);
			}

			rewrite_events = list_delete_last(rewrite_events);
		}

		/*
		 * 若有 INSTEAD，且原始查询有 RETURNING，我们
		 * 必须在规则里找到了 RETURNING，否则失败。（因为
		 * DefineQueryRewrite 只允许无条件 INSTEAD
		 * 规则里有 RETURNING，不必担心被替换的 RETURNING
		 * 是否真的会被执行：它必定会被。）
		 */
		if ((instead || qual_product != NULL) &&
			parsetree->returningList &&
			!returning)
		{
			switch (event)
			{
				case CMD_INSERT:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot perform INSERT RETURNING on relation \"%s\"",
									RelationGetRelationName(rt_entry_relation)),
							 errhint("You need an unconditional ON INSERT DO INSTEAD rule with a RETURNING clause.")));
					break;
				case CMD_UPDATE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot perform UPDATE RETURNING on relation \"%s\"",
									RelationGetRelationName(rt_entry_relation)),
							 errhint("You need an unconditional ON UPDATE DO INSTEAD rule with a RETURNING clause.")));
					break;
				case CMD_DELETE:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot perform DELETE RETURNING on relation \"%s\"",
									RelationGetRelationName(rt_entry_relation)),
							 errhint("You need an unconditional ON DELETE DO INSTEAD rule with a RETURNING clause.")));
					break;
				default:
					elog(ERROR, "unrecognized commandType: %d",
						 (int) event);
					break;
			}
		}

		/*
		 * 可更新视图受 ON CONFLICT 支持，因此不要阻止那种
		 * 情形继续。
		 */
		if (parsetree->onConflict &&
			(product_queries != NIL || hasUpdate) &&
			!updatableview)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("INSERT with ON CONFLICT clause cannot be used with table that has INSERT or UPDATE rules")));

		table_close(rt_entry_relation, NoLock);
	}

	/*
	 * 对 INSERTs，原始查询先做；对 UPDATE/DELETE，它
	 * 最后做。这是必需的，因为更新和删除规则动作若在
	 * 更新或删除执行之后才被调用，可能
	 * 什么都不做。查询执行之间的命令计数器递增
	 * 会使被删除的（也许还有被更新的）元组消失，因此规则动作里
	 * 对它们的扫描无法找到它们。
	 *
	 * 若我们找到了任何无限定条件的 INSTEAD，原始查询就完全
	 * 不做，任何形式都不做。否则，若找到了带限定条件的
	 * INSTEADs，我们就加入修改后的形式，否则加入未修改的形式。
	 */
	if (!instead)
	{
		if (parsetree->commandType == CMD_INSERT)
		{
			if (qual_product != NULL)
				rewritten = lcons(qual_product, rewritten);
			else
				rewritten = lcons(parsetree, rewritten);
		}
		else
		{
			if (qual_product != NULL)
				rewritten = lappend(rewritten, qual_product);
			else
				rewritten = lappend(rewritten, parsetree);
		}
	}

	/*
	 * 若原始查询有 CTE 列表，并且我们生成了不止一个
	 * 非实用结果查询，就必须失败，因为我们会把
	 * CTE 列表复制进每个结果查询。那会打破 CTEs
	 * 只求值一次的预期。这也许可以通过
	 * 重构来修好，使 CTE 列表能在多个 Query
	 * 和 PlannableStatement 节点之间共享。
	 */
	if (parsetree->cteList != NIL)
	{
		int			qcount = 0;

		foreach(lc1, rewritten)
		{
			Query	   *q = (Query *) lfirst(lc1);

			if (q->commandType != CMD_UTILITY)
				qcount++;
		}
		if (qcount > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH cannot be used in a query that is rewritten by rules into multiple queries")));
	}

	return rewritten;
}


/*
 * QueryRewrite -
 *	  查询重写器的主入口。
 *	  通过规则系统重写一条查询，可能返回 0
 *	  条或多条查询。
 *
 * 注意：语法分析树必须要么直接来自解析器，
 * 要么已经由 AcquireRewriteLocks 扫描过以取得合适的锁。
 */
List *
QueryRewrite(Query *parsetree)
{
	uint64		input_query_id = parsetree->queryId;
	List	   *querylist;
	List	   *results;
	ListCell   *l;
	CmdType		origCmdType;
	bool		foundOriginalQuery;
	Query	   *lastInstead;

	/*
	 * 本函数只应用于顶层原始查询
	 */
	Assert(parsetree->querySource == QSRC_ORIGINAL);
	Assert(parsetree->canSetTag);

	/*
	 * 第 1 步：
	 *
	 * 应用全部非 SELECT 规则，可能得到 0 条或多条查询
	 */
	querylist = RewriteQuery(parsetree, NIL, 0);

	/*
	 * 第 2 步：
	 *
	 * 对每个查询应用全部 RIR 规则。
	 *
	 * 这里也是给每个查询标上原始 queryId 的方便之处
	 */
	results = NIL;
	foreach(l, querylist)
	{
		Query	   *query = (Query *) lfirst(l);

		query = fireRIRrules(query, NIL);

		query->queryId = input_query_id;

		results = lappend(results, query);
	}

	/*
	 * 第 3 步：
	 *
	 * 确定结果查询里哪一个（若有）应当设置
	 * 命令结果标签，并相应更新 canSetTag 成员。
	 *
	 * 若原始查询仍在列表里，它设置命令标签。
	 * 否则，与原始查询同类的最后一条 INSTEAD 查询被
	 * 允许设置标签。（注意这些规则可能使我们没有任何查询
	 * 设置标签。tcop 代码必须应对这一点，根据
	 * 原始的、未重写的查询设置一个默认标签。）
	 *
	 * 这些 Assert 验证结果列表里最多有一个查询被标成
	 * canSetTag。若我们不检查断言，一旦找到原始查询
	 * 就可以跳出循环。
	 */
	origCmdType = parsetree->commandType;
	foundOriginalQuery = false;
	lastInstead = NULL;

	foreach(l, results)
	{
		Query	   *query = (Query *) lfirst(l);

		if (query->querySource == QSRC_ORIGINAL)
		{
			Assert(query->canSetTag);
			Assert(!foundOriginalQuery);
			foundOriginalQuery = true;
#ifndef USE_ASSERT_CHECKING
			break;
#endif
		}
		else
		{
			Assert(!query->canSetTag);
			if (query->commandType == origCmdType &&
				(query->querySource == QSRC_INSTEAD_RULE ||
				 query->querySource == QSRC_QUAL_INSTEAD_RULE))
				lastInstead = query;
		}
	}

	if (!foundOriginalQuery && lastInstead != NULL)
		lastInstead->canSetTag = true;

	return results;
}
