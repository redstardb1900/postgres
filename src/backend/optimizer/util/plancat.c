/*-------------------------------------------------------------------------
 *
 * plancat.c
 *	   routines for accessing the system catalogs
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/plancat.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/pg_am.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_statistic_ext_data.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "rewrite/rewriteManip.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/* GUC 参数 */
int			constraint_exclusion = CONSTRAINT_EXCLUSION_PARTITION;

/* 供插件在 get_relation_info() 中取得控制权的钩子 */
get_relation_info_hook_type get_relation_info_hook = NULL;


static void get_relation_foreign_keys(PlannerInfo *root, RelOptInfo *rel,
									  Relation relation, bool inhparent);
static bool infer_collation_opclass_match(InferenceElem *elem, Relation idxRel,
										  List *idxExprs);
static List *get_relation_constraints(PlannerInfo *root,
									  Oid relationObjectId, RelOptInfo *rel,
									  bool include_noinherit,
									  bool include_notnull,
									  bool include_partition);
static List *build_index_tlist(PlannerInfo *root, IndexOptInfo *index,
							   Relation heapRelation);
static List *get_relation_statistics(RelOptInfo *rel, Relation relation);
static void set_relation_partition_info(PlannerInfo *root, RelOptInfo *rel,
										Relation relation);
static PartitionScheme find_partition_scheme(PlannerInfo *root,
											 Relation relation);
static void set_baserel_partition_key_exprs(Relation relation,
											RelOptInfo *rel);
static void set_baserel_partition_constraint(Relation relation,
											 RelOptInfo *rel);


/*
 * get_relation_info -
 *	  取出给定关系的目录信息。
 *
 * 给定关系的 Oid，把下列信息填进
 * RelOptInfo 结构的成员：
 *
 *	min_attr	最低的有效 AttrNumber
 *	max_attr	最高的有效 AttrNumber
 *	indexlist	该关系各索引的 IndexOptInfos 列表
 *	statlist	该关系统计对象的 StatisticExtInfo 列表
 *	serverid	若是外部表，则为服务器 OID
 *	fdwroutine	若是外部表，则为 FDW 函数指针
 *	pages		页数
 *	tuples		元组数
 *	rel_parallel_workers 用户定义的并行工作进程数
 *
 * 也把该关系外键的信息加进 root->fkey_list。
 *
 * 同时初始化 attr_needed[] 和 attr_widths[] 数组。多数
 * 情况下它们留为零，但有时需要在这里计算属性
 * 宽度，不妨把结果缓存给 costsize.c。
 *
 * 若 inhparent 为真，只需把属性数组建好：
 * RelOptInfo 实际代表继承树形成的 appendrel，
 * 因此父关系的物理大小和索引信息对它并不
 * 重要；不过对分区表，我们会填充
 * indexlist，因为规划器把唯一索引当作某些
 * 优化的唯一性证明。
 */
void
get_relation_info(PlannerInfo *root, Oid relationObjectId, bool inhparent,
				  RelOptInfo *rel)
{
	Index		varno = rel->relid;
	Relation	relation;
	bool		hasindex;
	List	   *indexinfos = NIL;

	/*
	 * 不必锁这个关系，因为重写器已经锁过，或者
	 * expand_inherited_rtentry() 把它加进查询的
	 * 范围表时已经锁过。
	 */
	relation = table_open(relationObjectId, NoLock);

	/*
	 * 没有表访问方法的关系，只有属于
	 * 特判 relkind 时才能用于查询。这个检查防止以后崩溃，例如
	 * 视图的 ON SELECT 规则已经缺少。注意
	 * table_open() 已经拒绝了索引和复合类型；报错
	 * 用词与它保持一致。
	 */
	if (!relation->rd_tableam)
	{
		if (!(relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE ||
			  relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot open relation \"%s\"",
							RelationGetRelationName(relation)),
					 errdetail_relkind_not_supported(relation->rd_rel->relkind)));
	}

	/* 恢复期间无法访问临时关系和未记录关系。 */
	if (!RelationIsPermanent(relation) && RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot access temporary or unlogged relations during recovery")));

	rel->min_attr = FirstLowInvalidHeapAttributeNumber + 1;
	rel->max_attr = RelationGetNumberOfAttributes(relation);
	rel->reltablespace = RelationGetForm(relation)->reltablespace;

	Assert(rel->max_attr >= rel->min_attr);
	rel->attr_needed = (Relids *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
	rel->attr_widths = (int32 *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));

	/*
	 * 估计关系大小——除非它是继承父表，那时
	 * 我们要的不是该关系自身的大小，而是它的
	 * 继承树的大小。那会在 set_append_rel_size() 里计算。
	 */
	if (!inhparent)
		estimate_rel_size(relation, rel->attr_widths - rel->min_attr,
						  &rel->pages, &rel->tuples, &rel->allvisfrac);

	/* 取出 parallel_workers 关系选项，未设置则为 -1。 */
	rel->rel_parallel_workers = RelationGetParallelWorkers(relation, -1);

	/*
	 * 建立索引列表。若被告知，则忽略系统目录上的索引。
	 * 不必理会传统继承父表的索引。对
	 * 分区表，至少需要一份唯一索引列表，因为它们
	 * 充当某些规划器优化的唯一性证明。不过
	 * 这里不必区别对待，把所有分区索引都记下来，
	 * 不论它们是不是唯一索引。
	 */
	if ((inhparent && relation->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		|| (IgnoreSystemIndexes && IsSystemRelation(relation)))
		hasindex = false;
	else
		hasindex = relation->rd_rel->relhasindex;

	if (hasindex)
	{
		List	   *indexoidlist;
		LOCKMODE	lmode;
		ListCell   *l;

		indexoidlist = RelationGetIndexList(relation);

		/*
		 * 对每个索引，取得执行器将会
		 * 需要的同一种锁，并且不释放。这样少跑几趟
		 * 共享锁管理器，又不会真正损失
		 * 并发，因为我们持有父关系的锁时，索引上不可能发生
		 * 模式变更，而且查询所用的锁类型
		 * 不会阻塞任何其他种类的索引操作。
		 */
		lmode = root->simple_rte_array[varno]->rellockmode;

		foreach(l, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(l);
			Relation	indexRelation;
			Form_pg_index index;
			IndexAmRoutine *amroutine;
			IndexOptInfo *info;
			int			ncolumns,
						nkeycolumns;
			int			i;

			/*
			 * 从索引的关系描述符中抽出信息。
			 */
			indexRelation = index_open(indexoid, lmode);
			index = indexRelation->rd_index;

			/*
			 * 忽略无效索引，因为查询不能安全地使用它们。
			 * 注意这没问题，因为我们正在构造的数据结构
			 * 只供规划器使用——执行器
			 * 仍须向标成 indisready 的无效
			 * 索引里插入。
			 */
			if (!index->indisvalid)
			{
				index_close(indexRelation, NoLock);
				continue;
			}

			/*
			 * 若索引有效，但还无法使用，就忽略它；但
			 * 把正在生成的计划标成临时的。讨论见
			 * src/backend/access/heap/README.HOT。
			 */
			if (index->indcheckxmin &&
				!TransactionIdPrecedes(HeapTupleHeaderGetXmin(indexRelation->rd_indextuple->t_data),
									   TransactionXmin))
			{
				root->glob->transientPlan = true;
				index_close(indexRelation, NoLock);
				continue;
			}

			info = makeNode(IndexOptInfo);

			info->indexoid = index->indexrelid;
			info->reltablespace =
				RelationGetForm(indexRelation)->reltablespace;
			info->rel = rel;
			info->ncolumns = ncolumns = index->indnatts;
			info->nkeycolumns = nkeycolumns = index->indnkeyatts;

			info->indexkeys = (int *) palloc(sizeof(int) * ncolumns);
			info->indexcollations = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
			info->opfamily = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
			info->opcintype = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
			info->canreturn = (bool *) palloc(sizeof(bool) * ncolumns);

			for (i = 0; i < ncolumns; i++)
			{
				info->indexkeys[i] = index->indkey.values[i];
				info->canreturn[i] = index_can_return(indexRelation, i + 1);
			}

			for (i = 0; i < nkeycolumns; i++)
			{
				info->opfamily[i] = indexRelation->rd_opfamily[i];
				info->opcintype[i] = indexRelation->rd_opcintype[i];
				info->indexcollations[i] = indexRelation->rd_indcollation[i];
			}

			info->relam = indexRelation->rd_rel->relam;

			/*
			 * 分区索引没有访问方法，因此只把
			 * 与访问方法有关的成员 NULLify 掉。
			 */
			if (indexRelation->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
			{
				/* 只复制需要的成员，不是 rd_indam 的全部 */
				amroutine = indexRelation->rd_indam;
				info->amcanorderbyop = amroutine->amcanorderbyop;
				info->amoptionalkey = amroutine->amoptionalkey;
				info->amsearcharray = amroutine->amsearcharray;
				info->amsearchnulls = amroutine->amsearchnulls;
				info->amcanparallel = amroutine->amcanparallel;
				info->amhasgettuple = (amroutine->amgettuple != NULL);
				info->amhasgetbitmap = amroutine->amgetbitmap != NULL &&
					relation->rd_tableam->scan_bitmap_next_block != NULL;
				info->amcanmarkpos = (amroutine->ammarkpos != NULL &&
									  amroutine->amrestrpos != NULL);
				info->amcostestimate = amroutine->amcostestimate;
				Assert(info->amcostestimate != NULL);

				/* 取出索引操作符类选项 */
				info->opclassoptions = RelationGetIndexAttOptions(indexRelation, true);

				/*
				 * 若有的话，取出该索引的排序信息。
				 */
				if (info->relam == BTREE_AM_OID)
				{
					/*
					 * 若它是 btree 索引，可以直接把它的操作符族 OIDs
					 * 当作排序所用的操作符族 OIDs。
					 */
					Assert(amroutine->amcanorder);

					info->sortopfamily = info->opfamily;
					info->reverse_sort = (bool *) palloc(sizeof(bool) * nkeycolumns);
					info->nulls_first = (bool *) palloc(sizeof(bool) * nkeycolumns);

					for (i = 0; i < nkeycolumns; i++)
					{
						int16		opt = indexRelation->rd_indoption[i];

						info->reverse_sort[i] = (opt & INDOPTION_DESC) != 0;
						info->nulls_first[i] = (opt & INDOPTION_NULLS_FIRST) != 0;
					}
				}
				else if (amroutine->amcanorder)
				{
					/*
					 * 否则，试着把这个索引的 < 操作符映射到 btree，
					 * 从而找出对应的 btree 操作符族。
					 * 因为 < 唯一决定一种排序
					 * 顺序的行为，这个检验就够了。
					 *
					 * XXX 这个办法相当慢，而且需要
					 * 一个不受欢迎的假定：其他索引访问方法
					 * 给策略编号的方式与 btree 相同。最好能
					 * 显式声明其他索引类型的每个操作符族
					 * 所对应的 btree 操作符族。
					 * 但鉴于目前和可预见的将来都缺少
					 * amcanorder 索引类型，现在不值得再
					 * 多花力气。
					 */
					info->sortopfamily = (Oid *) palloc(sizeof(Oid) * nkeycolumns);
					info->reverse_sort = (bool *) palloc(sizeof(bool) * nkeycolumns);
					info->nulls_first = (bool *) palloc(sizeof(bool) * nkeycolumns);

					for (i = 0; i < nkeycolumns; i++)
					{
						int16		opt = indexRelation->rd_indoption[i];
						Oid			ltopr;
						Oid			btopfamily;
						Oid			btopcintype;
						int16		btstrategy;

						info->reverse_sort[i] = (opt & INDOPTION_DESC) != 0;
						info->nulls_first[i] = (opt & INDOPTION_NULLS_FIRST) != 0;

						ltopr = get_opfamily_member(info->opfamily[i],
													info->opcintype[i],
													info->opcintype[i],
													BTLessStrategyNumber);
						if (OidIsValid(ltopr) &&
							get_ordering_op_properties(ltopr,
													   &btopfamily,
													   &btopcintype,
													   &btstrategy) &&
							btopcintype == info->opcintype[i] &&
							btstrategy == BTLessStrategyNumber)
						{
							/* 映射成功 */
							info->sortopfamily[i] = btopfamily;
						}
						else
						{
							/* 失败……悄悄把索引当成无序的 */
							info->sortopfamily = NULL;
							info->reverse_sort = NULL;
							info->nulls_first = NULL;
							break;
						}
					}
				}
				else
				{
					info->sortopfamily = NULL;
					info->reverse_sort = NULL;
					info->nulls_first = NULL;
				}
			}
			else
			{
				info->amcanorderbyop = false;
				info->amoptionalkey = false;
				info->amsearcharray = false;
				info->amsearchnulls = false;
				info->amcanparallel = false;
				info->amhasgettuple = false;
				info->amhasgetbitmap = false;
				info->amcanmarkpos = false;
				info->amcostestimate = NULL;

				info->sortopfamily = NULL;
				info->reverse_sort = NULL;
				info->nulls_first = NULL;
			}

			/*
			 * 若有的话，取出索引表达式和谓词。必须
			 * 修改从 relcache 得到的副本，使它们具有
			 * 父关系的正确 varno，以便与限定子句
			 * 正确对应。
			 */
			info->indexprs = RelationGetIndexExpressions(indexRelation);
			info->indpred = RelationGetIndexPredicate(indexRelation);
			if (info->indexprs && varno != 1)
				ChangeVarNodes((Node *) info->indexprs, 1, varno, 0);
			if (info->indpred && varno != 1)
				ChangeVarNodes((Node *) info->indpred, 1, varno, 0);

			/* 用填好的 indexprs 数据建立目标列表 */
			info->indextlist = build_index_tlist(root, info, relation);

			info->indrestrictinfo = NIL;	/* 以后在 indxpath.c 里设置 */
			info->predOK = false;	/* 以后在 indxpath.c 里设置 */
			info->unique = index->indisunique;
			info->immediate = index->indimmediate;
			info->hypothetical = false;

			/*
			 * 估计索引大小。若不是部分索引，就把
			 * 元组数估计锁成等于父表；若
			 * 是部分索引，就得用与表相同的办法，
			 * 只是可以肯定索引不会大于
			 * 表。这里必须忽略分区索引，因为
			 * 它们不是物理索引。
			 */
			if (indexRelation->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
			{
				if (info->indpred == NIL)
				{
					info->pages = RelationGetNumberOfBlocks(indexRelation);
					info->tuples = rel->tuples;
				}
				else
				{
					double		allvisfrac; /* 哑（占位） */

					estimate_rel_size(indexRelation, NULL,
									  &info->pages, &info->tuples, &allvisfrac);
					if (info->tuples > rel->tuples)
						info->tuples = rel->tuples;
				}

				if (info->relam == BTREE_AM_OID)
				{
					/*
					 * 对 btree，趁索引还开着取出
					 * 树高
					 */
					info->tree_height = _bt_getrootheight(indexRelation);
				}
				else
				{
					/* 对其他索引类型，目前先设成未知 */
					info->tree_height = -1;
				}
			}
			else
			{
				/* 分区索引把这些清零 */
				info->pages = 0;
				info->tuples = 0.0;
				info->tree_height = -1;
			}

			index_close(indexRelation, NoLock);

			/*
			 * 这里历来用 lcons()。用 lappend() 更合理，
			 * 但那样会在两个索引看起来同样有吸引力时，让规划器
			 * 改变行为。目前
			 * 仍用 lcons()——很少有表的索引多到
			 * lcons() 的 O(N^2) 行为真成问题。
			 */
			indexinfos = lcons(info, indexinfos);
		}

		list_free(indexoidlist);
	}

	rel->indexlist = indexinfos;

	rel->statlist = get_relation_statistics(rel, relation);

	/* 趁 relcache 还在，用它抓取外部表信息 */
	if (relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* 检查对外部表的访问是否受限制 */
		if (unlikely((restrict_nonsystem_relation_kind & RESTRICT_RELKIND_FOREIGN_TABLE) != 0))
		{
			/* 不得存在内建的外部表 */
			Assert(RelationGetRelid(relation) >= FirstNormalObjectId);

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("access to non-system foreign table is restricted")));
		}

		rel->serverid = GetForeignServerIdByRelId(RelationGetRelid(relation));
		rel->fdwroutine = GetFdwRoutineForRelation(relation, true);
	}
	else
	{
		rel->serverid = InvalidOid;
		rel->fdwroutine = NULL;
	}

	/* 若相关，收集该关系外键的信息 */
	get_relation_foreign_keys(root, rel, relation, inhparent);

	/* 收集由该关系的表访问方法实现的函数信息。 */
	if (relation->rd_tableam &&
		relation->rd_tableam->scan_set_tidrange != NULL &&
		relation->rd_tableam->scan_getnextslot_tidrange != NULL)
		rel->amflags |= AMFLAG_HAS_TID_RANGE;

	/*
	 * 若有的话，收集该关系的分区方案信息。只有
	 * 继承父表才可能被分区。
	 */
	if (inhparent && relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		set_relation_partition_info(root, rel, relation);

	table_close(relation, NoLock);

	/*
	 * 允许插件对从目录取得的信息发表意见。
	 * 动作可以包括改掉假定的关系大小、
	 * 去掉一个索引，或往 indexlist 里加一个假想索引。
	 */
	if (get_relation_info_hook)
		(*get_relation_info_hook) (root, relationObjectId, inhparent, rel);
}

/*
 * get_relation_foreign_keys -
 *	  取出给定关系的外键信息。
 *
 * 为相关外键建立 ForeignKeyOptInfos，并加进
 * root->fkey_list。趁 relcache 项还开着现在就做。
 * 若等到全部 RelOptInfos 建好，有时可以避免建立无用的
 * ForeignKeyOptInfos，但重新打开
 * relcache 项的代价多半超过任何节省。
 */
static void
get_relation_foreign_keys(PlannerInfo *root, RelOptInfo *rel,
						  Relation relation, bool inhparent)
{
	List	   *rtable = root->parse->rtable;
	List	   *cachedfkeys;
	ListCell   *lc;

	/*
	 * 若它不是基关系，我们不关心它的外键。另外，若查询
	 * 只引用一个关系，可以跳过查找，因为没有外键
	 * 能满足下面的条件。
	 */
	if (rel->reloptkind != RELOPT_BASEREL ||
		list_length(rtable) < 2)
		return;

	/*
	 * 若它是继承树的父表，忽略它的外键。若发现
	 * 继承树的全部成员都有等价的外键约束，本可以做出
	 * 有用的基于外键的推导，但检测这一点
	 * 需要尚未写出的代码。
	 */
	if (inhparent)
		return;

	/*
	 * 从 relcache 抽出该关系外键的数据。注意这个
	 * 列表属于 relcache，缓存刷新时可能消失，因此
	 * 在本函数里不得再做任何目录访问。
	 */
	cachedfkeys = RelationGetFKeyList(relation);

	/*
	 * 弄清哪些外键与本查询有关，并为它们建立
	 * ForeignKeyOptInfos。只要引用当前查询某个
	 * 其他 RTE 的外键。在含有自连接的查询里，
	 * 被引用的表可能还有不止一个其他 RTE，应当
	 * 为每一次出现建立一个 ForeignKeyOptInfo。
	 *
	 * 理想情况下会忽略对应非基关系的 RTEs，但在这里
	 * 很难认出它们，因此也许会建立一些无用的
	 * ForeignKeyOptInfos。若如此，match_foreign_keys_to_quals() 会再
	 * 把它们去掉。
	 */
	foreach(lc, cachedfkeys)
	{
		ForeignKeyCacheInfo *cachedfk = (ForeignKeyCacheInfo *) lfirst(lc);
		Index		rti;
		ListCell   *lc2;

		/* conrelid 应当总是我们正在考虑的那张表 */
		Assert(cachedfk->conrelid == RelationGetRelid(relation));

		/* 扫描以找出匹配 confrelid 的其他 RTEs */
		rti = 0;
		foreach(lc2, rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc2);
			ForeignKeyOptInfo *info;

			rti++;
			/* 若不是正确的表则忽略 */
			if (rte->rtekind != RTE_RELATION ||
				rte->relid != cachedfk->confrelid)
				continue;
			/* 若它是继承父表则忽略；并不真正匹配 */
			if (rte->inh)
				continue;
			/* 忽略自引用外键；我们只关心连接 */
			if (rti == rel->relid)
				continue;

			/* 好，建立一项 */
			info = makeNode(ForeignKeyOptInfo);
			info->con_relid = rel->relid;
			info->ref_relid = rti;
			info->nkeys = cachedfk->nkeys;
			memcpy(info->conkey, cachedfk->conkey, sizeof(info->conkey));
			memcpy(info->confkey, cachedfk->confkey, sizeof(info->confkey));
			memcpy(info->conpfeqop, cachedfk->conpfeqop, sizeof(info->conpfeqop));
			/* 把将由 match_foreign_keys_to_quals 填写的成员清零 */
			info->nmatched_ec = 0;
			info->nconst_ec = 0;
			info->nmatched_rcols = 0;
			info->nmatched_ri = 0;
			memset(info->eclass, 0, sizeof(info->eclass));
			memset(info->fk_eclass_member, 0, sizeof(info->fk_eclass_member));
			memset(info->rinfos, 0, sizeof(info->rinfos));

			root->fkey_list = lappend(root->fkey_list, info);
		}
	}
}

/*
 * infer_arbiter_indexes -
 *	  确定用于仲裁推测性插入的唯一索引。
 *
 * 用用户给出的推断子句表达式和谓词，去匹配
 * 堆关系（目标）上已定义且就绪的
 * 唯一索引。
 * 列和表达式必须精确匹配（尽管它们可以
 * 以任何顺序出现）。不过用户给出的谓词只需把
 * 插入限制到某张表被某个特定
 * 唯一索引覆盖的某一部分的子集（尤其是部分唯一索引），即可被推断出来。
 *
 * 实现并不考虑任何可用唯一索引属性使用哪个 B-树操作符类，
 * 除非推断说明里指定了一个。
 * 排序规则也一样。尤其是，
 * 推断并不依赖系统的默认操作符类。
 * 若没有指定操作符类（或排序规则），则
 * 所有匹配的索引（就各属性的操作符类/排序规则而言，
 * 它们也许匹配默认，也许不匹配）都用于推断。
 *
 * 注意：在索引 CONCURRENTLY 操作期间，不同事务可能
 * 引用不同的仲裁索引集合。这会导致正常操作中不会出现的
 * 假唯一约束违反。更多
 * 信息见 insert.sgml。
 */
List *
infer_arbiter_indexes(PlannerInfo *root)
{
	OnConflictExpr *onconflict = root->parse->onConflict;

	/* 迭代状态 */
	Index		varno;
	RangeTblEntry *rte;
	Relation	relation;
	Oid			indexOidFromConstraint = InvalidOid;
	List	   *indexList;
	ListCell   *l;

	/* 规范化后的推断属性和推断表达式： */
	Bitmapset  *inferAttrs = NULL;
	List	   *inferElems = NIL;

	/* 结果 */
	List	   *results = NIL;

	/*
	 * 对没有推断说明或具名约束的 ON CONFLICT DO NOTHING，迅速返回 NIL。
	 * ON CONFLICT DO UPDATE 语句
	 * 必须总是提供其中之一（不过解析器应当已经
	 * 抓住了这一点）。
	 */
	if (onconflict->arbiterElems == NIL &&
		onconflict->constraint == InvalidOid)
		return NIL;

	/*
	 * 不必锁这个关系，因为重写器已经锁过，或者
	 * expand_inherited_rtentry() 把它加进查询的
	 * 范围表时已经锁过。
	 */
	varno = root->parse->resultRelation;
	rte = rt_fetch(varno, root->parse->rtable);

	relation = table_open(rte->relid, NoLock);

	/*
	 * 为普通索引属性建立规范化的位图集表示，
	 * 另外再单独列出表达式项。这简化了与
	 * 目录中索引定义的匹配。
	 */
	foreach(l, onconflict->arbiterElems)
	{
		InferenceElem *elem = (InferenceElem *) lfirst(l);
		Var		   *var;
		int			attno;

		if (!IsA(elem->expr, Var))
		{
			/* 若不是普通 Var，先塞进 inferElems */
			inferElems = lappend(inferElems, elem->expr);
			continue;
		}

		var = (Var *) elem->expr;
		attno = var->varattno;

		if (attno == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("whole row unique index inference specifications are not supported")));

		inferAttrs = bms_add_member(inferAttrs,
									attno - FirstLowInvalidHeapAttributeNumber);
	}

	/*
	 * 查找具名约束的索引。不会立刻返回，
	 * 因为还需要一些额外的健全性检查。
	 */
	if (onconflict->constraint != InvalidOid)
	{
		indexOidFromConstraint = get_constraint_index(onconflict->constraint);

		if (indexOidFromConstraint == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("constraint in ON CONFLICT clause has no associated index")));
	}

	/*
	 * 用那种表示，遍历目标关系上的索引列表，
	 * 试着找出匹配
	 */
	indexList = RelationGetIndexList(relation);

	foreach(l, indexList)
	{
		Oid			indexoid = lfirst_oid(l);
		Relation	idxRel;
		Form_pg_index idxForm;
		Bitmapset  *indexedAttrs;
		List	   *idxExprs;
		List	   *predExprs;
		AttrNumber	natt;
		ListCell   *el;

		/*
		 * 从索引的关系描述符中抽出信息。取得
		 * 执行器最终会使用的同一种锁。
		 *
		 * 让执行器直接抱怨非 indimmediate 的情形，因为
		 * 省略推断子句时，强制执行反正也得
		 * 发生在那里。
		 */
		idxRel = index_open(indexoid, rte->rellockmode);
		idxForm = idxRel->rd_index;

		if (!idxForm->indisvalid)
			goto next;

		/*
		 * 注意这里不像 get_relation_info() 那样对照 indcheckxmin
		 * 做检查来淘汰候选，因为
		 * 唯一性检查只关心最近提交的
		 * 元组版本。
		 */

		/*
		 * 寻找 ON constraint_name 这种变体的匹配，它也许不是
		 * 唯一约束。这只能是约束名。
		 */
		if (indexOidFromConstraint == idxForm->indexrelid)
		{
			if (!idxForm->indisunique && onconflict->action == ONCONFLICT_UPDATE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("ON CONFLICT DO UPDATE not supported with exclusion constraints")));

			results = lappend_oid(results, idxForm->indexrelid);
			list_free(indexList);
			index_close(idxRel, NoLock);
			table_close(relation, NoLock);
			return results;
		}
		else if (indexOidFromConstraint != InvalidOid)
		{
			/* 具名约束情形下，索引不必再做进一步工作 */
			goto next;
		}

		/*
		 * 此刻只考虑常规推断（不是具名
		 * 约束），因此若考虑中的索引不唯一，
		 * 可以立刻跳过
		 */
		if (!idxForm->indisunique)
			goto next;

		/* 为普通（非表达式）索引属性建立位图集表示 */
		indexedAttrs = NULL;
		for (natt = 0; natt < idxForm->indnkeyatts; natt++)
		{
			int			attno = idxRel->rd_index->indkey.values[natt];

			if (attno != 0)
				indexedAttrs = bms_add_member(indexedAttrs,
											  attno - FirstLowInvalidHeapAttributeNumber);
		}

		/* 非表达式属性（若有）必须匹配 */
		if (!bms_equal(indexedAttrs, inferAttrs))
			goto next;

		/* 表达式属性（若有）必须匹配 */
		idxExprs = RelationGetIndexExpressions(idxRel);
		if (idxExprs && varno != 1)
			ChangeVarNodes((Node *) idxExprs, 1, varno, 0);

		foreach(el, onconflict->arbiterElems)
		{
			InferenceElem *elem = (InferenceElem *) lfirst(el);

			/*
			 * 确保推断表达式
			 * 元素的排序规则和操作符类方面匹配。尽管这个循环主要关心
			 * 匹配表达式，这里也便于同时检查
			 * 作为推断元素出现的表达式和普通（非表达式）
			 * 属性。
			 */
			if (!infer_collation_opclass_match(elem, idxRel, idxExprs))
				goto next;

			/*
			 * 普通 Var 不计入表达式元素的个数，而且
			 * 它们是否满足索引
			 * 定义已经考虑过了（它们必须满足）。
			 */
			if (IsA(elem->expr, Var))
				continue;

			/*
			 * 在少数 infer_collation_opclass_match() 需要做实事的情形里，
			 * 不妨避免重复检查。
			 * 否则，检查元素表达式是否出现在目录中的
			 * 索引定义里。
			 */
			if (elem->infercollid != InvalidOid ||
				elem->inferopclass != InvalidOid ||
				list_member(idxExprs, elem->expr))
				continue;

			goto next;
		}

		/*
		 * 既然全部推断元素都匹配上了，确保推断子句里的
		 * 表达式元素没有缺少任何
		 * 目录中的表达式。当唯一
		 * 索引多余地重复同一属性，或属性
		 * 在推断子句里多余地出现多次时，这样做是对的。
		 */
		if (list_difference(idxExprs, inferElems) != NIL)
			goto next;

		/*
		 * 若它是部分索引，其谓词必须被 ON
		 * CONFLICT 的 WHERE 子句所蕴含。
		 */
		predExprs = RelationGetIndexPredicate(idxRel);
		if (predExprs && varno != 1)
			ChangeVarNodes((Node *) predExprs, 1, varno, 0);

		if (!predicate_implied_by(predExprs, (List *) onconflict->arbiterWhere, false))
			goto next;

		results = lappend_oid(results, idxForm->indexrelid);
next:
		index_close(idxRel, NoLock);
	}

	list_free(indexList);
	table_close(relation, NoLock);

	if (results == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("there is no unique or exclusion constraint matching the ON CONFLICT specification")));

	return results;
}

/*
 * infer_collation_opclass_match：确保推断元素的操作符类和排序规则匹配。
 *
 * 给定推断说明中的唯一索引推断元素，若
 * 指定了排序规则，或指定了操作符类，就验证至少有
 * 一个匹配的索引属性（偶尔可能不止一个）。
 * 常见情形是推断说明不含
 * 排序规则或操作符类（那时一律匹配，不论索引属性在目录里的
 * 排序规则和操作符类），则跳过这一步。
 *
 * 至少在历史上，Postgres 没有提供把等值理解成
 * 不同于默认的排序规则或操作符类，因此这些额外
 * 标准应当很少才需要。
 *
 * 当某个推断元素匹配了目录里标成已索引、
 * 但不符合额外操作符类或排序规则标准的属性时，不要立刻放弃。
 * 这样做是为了尽可能宽容
 * 目录索引属性中的冗余（或者，用处较小地，
 * 推断说明元素中的冗余）。若排序规则在
 * 看似冗余的已索引属性之间实际不同（索引内部或
 * 跨索引的冗余），那就其实没有这种冗余。
 *
 * 注意，若一个推断元素同时指定了操作符类和排序规则，
 * 则索引目录定义里至少要有一个特定属性
 * 两者都匹配，该推断元素才算
 * 已被推断或满足。
 */
static bool
infer_collation_opclass_match(InferenceElem *elem, Relation idxRel,
							  List *idxExprs)
{
	AttrNumber	natt;
	Oid			inferopfamily = InvalidOid; /* 操作符类所属操作符族的 OID */
	Oid			inferopcinputtype = InvalidOid; /* 操作符类输入类型的 OID */
	int			nplain = 0;		/* 观察到的普通属性个数 */

	/*
	 * 若推断说明元素没有排序规则或操作符类，就
	 * 不必检查精确匹配。
	 */
	if (elem->infercollid == InvalidOid && elem->inferopclass == InvalidOid)
		return true;

	/*
	 * 查找操作符族和输入类型，以便匹配索引
	 */
	if (elem->inferopclass)
	{
		inferopfamily = get_opclass_family(elem->inferopclass);
		inferopcinputtype = get_opclass_input_type(elem->inferopclass);
	}

	for (natt = 1; natt <= idxRel->rd_att->natts; natt++)
	{
		Oid			opfamily = idxRel->rd_opfamily[natt - 1];
		Oid			opcinputtype = idxRel->rd_opcintype[natt - 1];
		Oid			collation = idxRel->rd_indcollation[natt - 1];
		int			attno = idxRel->rd_index->indkey.values[natt - 1];

		if (attno != 0)
			nplain++;

		if (elem->inferopclass != InvalidOid &&
			(inferopfamily != opfamily || inferopcinputtype != opcinputtype))
		{
			/* 属性需要匹配操作符类，但没有 */
			continue;
		}

		if (elem->infercollid != InvalidOid &&
			elem->infercollid != collation)
		{
			/* 属性需要匹配排序规则，但没有 */
			continue;
		}

		/* 若找到一个匹配的索引属性，就够了——返回真 */
		if (IsA(elem->expr, Var))
		{
			if (((Var *) elem->expr)->varattno == attno)
				return true;
		}
		else if (attno == 0)
		{
			Node	   *nattExpr = list_nth(idxExprs, (natt - 1) - nplain);

			/*
			 * 注意，与 match_index_to_operand() 一类例程不同，我们
			 * 不必在意 RelabelType。索引
			 * 定义和推断子句都不应当含有它们。
			 */
			if (equal(elem->expr, nattExpr))
				return true;
		}
	}

	return false;
}

/*
 * estimate_rel_size：估计表或索引的页数和元组数
 *
 * 也估计可见性映射中标成全部可见的页所占比例，
 * 供估计仅索引扫描时使用。
 *
 * 若 attr_widths 不是 NULL，它指向该
 * 关系 attr_widths[] 缓存的零下标项；若为估计而需要计算
 * 属性宽度，就填上它。
 */
void
estimate_rel_size(Relation rel, int32 *attr_widths,
				  BlockNumber *pages, double *tuples, double *allvisfrac)
{
	BlockNumber curpages;
	BlockNumber relpages;
	double		reltuples;
	BlockNumber relallvisible;
	double		density;

	if (RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
	{
		table_relation_estimate_size(rel, attr_widths, pages, tuples,
									 allvisfrac);
	}
	else if (rel->rd_rel->relkind == RELKIND_INDEX)
	{
		/*
		 * XXX：大概最好把这个移进回调，各种
		 * 索引类型例如自己知道有没有元页。
		 */

		/* 它有存储，可以调用 smgr */
		curpages = RelationGetNumberOfBlocks(rel);

		/* 报告估计的页数 */
		*pages = curpages;
		/* 若关系明显为空，迅速退出 */
		if (curpages == 0)
		{
			*tuples = 0;
			*allvisfrac = 0;
			return;
		}

		/* 把 pg_class 里的值强制成更合意的类型 */
		relpages = (BlockNumber) rel->rd_rel->relpages;
		reltuples = (double) rel->rd_rel->reltuples;
		relallvisible = (BlockNumber) rel->rd_rel->relallvisible;

		/*
		 * 估计元组数时把元页打折。这
		 * 是权宜凑合，因为它对索引
		 * 结构假定得过多。目前对 btree、散列和 GIN 索引还可以，
		 * 对 GiST 索引则可疑。
		 */
		if (relpages > 0)
		{
			curpages--;
			relpages--;
		}

		/* 按先前的元组密度估计元组数 */
		if (reltuples >= 0 && relpages > 0)
			density = reltuples / (double) relpages;
		else
		{
			/*
			 * 若因为该关系从未被清理而没有数据，
			 * 就从属性数据类型估计元组宽度。这里假定
			 * 页是完全满的，对表来说可以
			 * （因为它们大概还没被 VACUUMed），但对索引
			 * 多半是高估。好在
			 * get_relation_info() 可以把高估钳到父
			 * 表的大小。
			 *
			 * 注意：这段代码有意不考虑对齐
			 * 问题，因为（a）估计已经这么粗，那样做是画蛇添足，
			 * 而且（b）它会让默认计划出现
			 * 平台依赖，对回归测试相当
			 * 头疼。
			 *
			 * XXX：这段逻辑是否应当更针对具体索引？
			 */
			int32		tuple_width;

			tuple_width = get_rel_data_width(rel, attr_widths);
			tuple_width += MAXALIGN(SizeofHeapTupleHeader);
			tuple_width += sizeof(ItemIdData);
			/* 注意：这里有意做整数除法 */
			density = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
		}
		*tuples = rint(density * (double) curpages);

		/*
		 * relallvisible 按原样使用，不像页数和元组数那样放大，
		 * 理由是自上次 VACUUM 以来新增的页
		 * 最可能没有标成全部可见。但
		 * costsize.c 要的是把它换成一个比例。
		 */
		if (relallvisible == 0 || curpages <= 0)
			*allvisfrac = 0;
		else if ((double) relallvisible >= curpages)
			*allvisfrac = 1;
		else
			*allvisfrac = (double) relallvisible / curpages;
	}
	else
	{
		/*
		 * 就用 pg_class 里已有的值。这覆盖外部表、
		 * 序列，以及没有存储的 relkind（不该走到这里？）；
		 * 见 AddNewRelationTuple() 里的初始化。注意 FDW 必须
		 * 能应付 reltuples 为 -1！
		 */
		*pages = rel->rd_rel->relpages;
		*tuples = rel->rd_rel->reltuples;
		*allvisfrac = 0;
	}
}


/*
 * get_rel_data_width
 *
 * 估计该关系元组（数据部分）的平均宽度。
 *
 * 若 attr_widths 不是 NULL，它指向该
 * 关系 attr_widths[] 缓存的零下标项；适当时使用并更新该缓存。
 *
 * 目前忽略已删除的列。理想情况下它们也该计入
 * 结果，但没有任何办法取得它们的信息；而且
 * 既然它们可能大部分是 NULLs，把它们当成零宽度
 * 也未必就错。
 */
int32
get_rel_data_width(Relation rel, int32 *attr_widths)
{
	int32		tuple_width = 0;
	int			i;

	for (i = 1; i <= RelationGetNumberOfAttributes(rel); i++)
	{
		Form_pg_attribute att = TupleDescAttr(rel->rd_att, i - 1);
		int32		item_width;

		if (att->attisdropped)
			continue;

		/* 若有的话，使用先前缓存的数据 */
		if (attr_widths != NULL && attr_widths[i] > 0)
		{
			tuple_width += attr_widths[i];
			continue;
		}

		/* 这应当与 costsize.c 里的 set_rel_width() 一致 */
		item_width = get_attavgwidth(RelationGetRelid(rel), i);
		if (item_width <= 0)
		{
			item_width = get_typavgwidth(att->atttypid, att->atttypmod);
			Assert(item_width > 0);
		}
		if (attr_widths != NULL)
			attr_widths[i] = item_width;
		tuple_width += item_width;
	}

	return tuple_width;
}

/*
 * get_relation_data_width
 *
 * get_rel_data_width 的外部 API：行为相同，只是必须
 * 打开 relcache 项。
 */
int32
get_relation_data_width(Oid relid, int32 *attr_widths)
{
	int32		result;
	Relation	relation;

	/* 同上，假定关系已经被安全地锁住 */
	relation = table_open(relid, NoLock);

	result = get_rel_data_width(relation, attr_widths);

	table_close(relation, NoLock);

	return result;
}


/*
 * get_relation_constraints
 *
 * 取出给定关系适用的约束表达式。
 *
 * 返回约束表达式的 List（可能为空）。每一条
 * 都已规范化，其 Var 被改成具有
 * rel->relid 所指示的 varno。这样这些表达式就容易
 * 与取自 WHERE 的表达式比较。
 *
 * 若 include_noinherit 为真，可以纳入标成
 * NO INHERIT 的约束。
 *
 * 若 include_notnull 为真，会为每个标了 attnotnull 的列
 * 生成 col IS NOT NULL 表达式并加入结果。
 *
 * 若 include_partition 为真，且该关系是分区，
 * 也纳入分区约束。
 *
 * 注意：目前每个关系在每次规划器运行中最多调用一次，
 * 而且许多情况下根本不会调用，因此似乎没有
 * 必要把数据缓存在 RelOptInfo 里。
 */
static List *
get_relation_constraints(PlannerInfo *root,
						 Oid relationObjectId, RelOptInfo *rel,
						 bool include_noinherit,
						 bool include_notnull,
						 bool include_partition)
{
	List	   *result = NIL;
	Index		varno = rel->relid;
	Relation	relation;
	TupleConstr *constr;

	/*
	 * 假定该关系已经被安全地锁住。
	 */
	relation = table_open(relationObjectId, NoLock);

	constr = relation->rd_att->constr;
	if (constr != NULL)
	{
		int			num_check = constr->num_check;
		int			i;

		for (i = 0; i < num_check; i++)
		{
			Node	   *cexpr;

			/*
			 * 若这条约束尚未完全验证，这里必须
			 * 忽略它。若是 NO INHERIT 且没有被告知
			 * 那样做是安全的，也忽略。
			 */
			if (!constr->check[i].ccvalid)
				continue;
			if (constr->check[i].ccnoinherit && !include_noinherit)
				continue;

			cexpr = stringToNode(constr->check[i].ccbin);

			/*
			 * 把每个表达式送过常量简化和
			 * 规范化。这不只是优化，而且是
			 * 必要的，因为我们会把它与
			 * 同样处理过的限定子句比较，不这样做可能发现不了有效
			 * 匹配。这必须与 preprocess_expression() 对
			 * 限定子句所做的处理一致！（可以跳过
			 * 涉及子查询的部分，因为检查约束里不允许
			 * 有子查询。）
			 */
			cexpr = eval_const_expressions(root, cexpr);

			cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

			/* 把 Var 改成所要的 varno */
			if (varno != 1)
				ChangeVarNodes(cexpr, 1, varno, 0);

			/*
			 * 最后，转换成隐式 AND 形式（也就是 List），并
			 * 把得到的项追加到输出列表。
			 */
			result = list_concat(result,
								 make_ands_implicit((Expr *) cexpr));
		}

		/* 若要求，以表达式形式加入 NOT NULL 约束 */
		if (include_notnull && constr->has_not_null)
		{
			int			natts = relation->rd_att->natts;

			for (i = 1; i <= natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(relation->rd_att, i - 1);

				if (att->attnotnull && !att->attisdropped)
				{
					NullTest   *ntest = makeNode(NullTest);

					ntest->arg = (Expr *) makeVar(varno,
												  i,
												  att->atttypid,
												  att->atttypmod,
												  att->attcollation,
												  0);
					ntest->nulltesttype = IS_NOT_NULL;

					/*
					 * 即使对复合列，argisrow=false 也是对的，
					 * 因为在这种情形下 attnotnull 并不表示 SQL 规范的 IS NOT
					 * NULL 检验，只是 IS DISTINCT FROM NULL。
					 */
					ntest->argisrow = false;
					ntest->location = -1;
					result = lappend(result, ntest);
				}
			}
		}
	}

	/*
	 * 若要求，加入分区约束。
	 */
	if (include_partition && relation->rd_rel->relispartition)
	{
		/* 确保已设置 rel->partition_qual */
		set_baserel_partition_constraint(relation, rel);
		result = list_concat(result, rel->partition_qual);
	}

	table_close(relation, NoLock);

	return result;
}

/*
 * 尝试装入该统计对象的数据。
 *
 * 我们不知道由 statOid 和 inh 值指定的数据是否存在。
 * 结果存放在 stainfos 列表里。
 */
static void
get_relation_statistics_worker(List **stainfos, RelOptInfo *rel,
							   Oid statOid, bool inh,
							   Bitmapset *keys, List *exprs)
{
	Form_pg_statistic_ext_data dataForm;
	HeapTuple	dtup;

	dtup = SearchSysCache2(STATEXTDATASTXOID,
						   ObjectIdGetDatum(statOid), BoolGetDatum(inh));
	if (!HeapTupleIsValid(dtup))
		return;

	dataForm = (Form_pg_statistic_ext_data) GETSTRUCT(dtup);

	/* 为每种已建立的统计加一个 StatisticExtInfo */
	if (statext_is_kind_built(dtup, STATS_EXT_NDISTINCT))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_NDISTINCT;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	if (statext_is_kind_built(dtup, STATS_EXT_DEPENDENCIES))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_DEPENDENCIES;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	if (statext_is_kind_built(dtup, STATS_EXT_MCV))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_MCV;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	if (statext_is_kind_built(dtup, STATS_EXT_EXPRESSIONS))
	{
		StatisticExtInfo *info = makeNode(StatisticExtInfo);

		info->statOid = statOid;
		info->inherit = dataForm->stxdinherit;
		info->rel = rel;
		info->kind = STATS_EXT_EXPRESSIONS;
		info->keys = bms_copy(keys);
		info->exprs = exprs;

		*stainfos = lappend(*stainfos, info);
	}

	ReleaseSysCache(dtup);
}

/*
 * get_relation_statistics
 *		取出定义在该表上的扩展统计。
 *
 * 返回描述这些统计的 StatisticExtInfo 对象的 List（可能为空）。
 * 注意这并不装入实际的统计数据，
 * 只是标识用的元数据。只考虑实际已建立的统计。
 */
static List *
get_relation_statistics(RelOptInfo *rel, Relation relation)
{
	Index		varno = rel->relid;
	List	   *statoidlist;
	List	   *stainfos = NIL;
	ListCell   *l;

	statoidlist = RelationGetStatExtList(relation);

	foreach(l, statoidlist)
	{
		Oid			statOid = lfirst_oid(l);
		Form_pg_statistic_ext staForm;
		HeapTuple	htup;
		Bitmapset  *keys = NULL;
		List	   *exprs = NIL;
		int			i;

		htup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statOid));
		if (!HeapTupleIsValid(htup))
			elog(ERROR, "cache lookup failed for statistics object %u", statOid);
		staForm = (Form_pg_statistic_ext) GETSTRUCT(htup);

		/*
		 * 首先建立所覆盖列的数组。若该对象里
		 * 没有任何统计实际建立过，这最终会被浪费，但
		 * 似乎不值得为那种情形费心。
		 */
		for (i = 0; i < staForm->stxkeys.dim1; i++)
			keys = bms_add_member(keys, staForm->stxkeys.values[i]);

		/*
		 * 预处理表达式（若有）。我们读入表达式，送过
		 * eval_const_expressions，并修正 varno。
		 *
		 * XXX 我们还不知道这个统计对象是否有数据，
		 * 对两个 stxdinherit 值都一样。但合理的假定是至少有
		 * 其中一个，也可能两个都有。因此最好在这里处理
		 * 键和表达式。
		 */
		{
			bool		isnull;
			Datum		datum;

			/* 解码表达式（若有） */
			datum = SysCacheGetAttr(STATEXTOID, htup,
									Anum_pg_statistic_ext_stxexprs, &isnull);

			if (!isnull)
			{
				char	   *exprsString;

				exprsString = TextDatumGetCString(datum);
				exprs = (List *) stringToNode(exprsString);
				pfree(exprsString);

				/*
				 * 把这些表达式送过 eval_const_expressions。这
				 * 不只是优化，而且是必要的，因为
				 * 规划器会把它们与同样处理过的限定
				 * 子句比较，不这样做可能发现不了有效匹配。
				 * 不过不得使用 canonicalize_qual，因为这些
				 * 并不是限定表达式。
				 */
				exprs = (List *) eval_const_expressions(NULL, (Node *) exprs);

				/* 不妨也修正 opfuncids */
				fix_opfuncids((Node *) exprs);

				/*
				 * 修改从 relcache 得到的副本，使它们具有
				 * 父关系的正确 varno，以便与限定子句
				 * 正确对应。
				 */
				if (varno != 1)
					ChangeVarNodes((Node *) exprs, 1, varno, 0);
			}
		}

		/* 抽出 stxdinherit 标志可能取值的统计 */

		get_relation_statistics_worker(&stainfos, rel, statOid, true, keys, exprs);

		get_relation_statistics_worker(&stainfos, rel, statOid, false, keys, exprs);

		ReleaseSysCache(htup);
		bms_free(keys);
	}

	list_free(statoidlist);

	return stainfos;
}

/*
 * relation_excluded_by_constraints
 *
 * 检测该关系是否不必扫描，因为它要么有
 * 自相矛盾的限制，要么有与该
 * 关系适用约束不一致的限制。
 *
 * 注意：这里只检查 rel->relid、rel->reloptkind 和
 * rel->baserestrictinfo；因此可以在填上
 * RelOptInfo 的其他成员之前调用。
 */
bool
relation_excluded_by_constraints(PlannerInfo *root,
								 RelOptInfo *rel, RangeTblEntry *rte)
{
	bool		include_noinherit;
	bool		include_notnull;
	bool		include_partition = false;
	List	   *safe_restrictions;
	List	   *constraint_pred;
	List	   *safe_constraints;
	ListCell   *lc;

	/* 目前，约束排除只对简单关系有效。 */
	Assert(IS_SIMPLE_REL(rel));

	/*
	 * 若没有基限制子句，下面就没有希望证明
	 * 任何东西，因此迅速退出。
	 */
	if (rel->baserestrictinfo == NIL)
		return false;

	/*
	 * 不论 constraint_exclusion 如何设置，都检测
	 * 常量 FALSE 或 NULL 的限制子句。因为常量折叠会把
	 * 任何东西 AND FALSE 化成仅仅 FALSE，这种情形应当
	 * 恰好得到一条 baserestrictinfo。这并不经常
	 * 触发，但似乎足够便宜，值得做。（没有
	 * 这一步，就会错过 9.5 及更早版本用迂回得多的
	 * 办法找到的一些优化。）
	 */
	if (list_length(rel->baserestrictinfo) == 1)
	{
		RestrictInfo *rinfo = (RestrictInfo *) linitial(rel->baserestrictinfo);
		Expr	   *clause = rinfo->clause;

		if (clause && IsA(clause, Const) &&
			(((Const *) clause)->constisnull ||
			 !DatumGetBool(((Const *) clause)->constvalue)))
			return true;
	}

	/*
	 * 依 constraint_exclusion 跳过进一步的检验。
	 */
	switch (constraint_exclusion)
	{
		case CONSTRAINT_EXCLUSION_OFF:
			/* 在 off 模式下，不再做任何进一步检验 */
			return false;

		case CONSTRAINT_EXCLUSION_PARTITION:

			/*
			 * 当 constraint_exclusion 设为 partition 时，
			 * 我们只处理 appendrel 成员。
			 * 分区裁剪已经应用过，因此这里不必
			 * 再考虑该关系的分区约束。
			 */
			if (rel->reloptkind == RELOPT_OTHER_MEMBER_REL)
				break;			/* 是 appendrel 成员，因此处理它 */
			return false;

		case CONSTRAINT_EXCLUSION_ON:

			/*
			 * 在 on 模式下，总是应用约束排除。若我们正在
			 * 考虑一个本身是分区的基关系（也就是它被
			 * 直接点名，而不是从父表展开），那么
			 * 它的分区约束还没被考虑过，因此
			 * 在这里的处理中纳入它们。
			 */
			if (rel->reloptkind == RELOPT_BASEREL)
				include_partition = true;
			break;				/* 总是尝试排除 */
	}

	/*
	 * 检查自相矛盾的限制子句。我们不敢用
	 * 非不可变函数做推导，但任何自相矛盾的不可变子句
	 * 都让我们断定不必扫描。
	 *
	 * 注意：剥掉 RestrictInfo，因为 predicate_refuted_by() 的
	 * 谓词参数里并不预期见到它们。
	 */
	safe_restrictions = NIL;
	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (!contain_mutable_functions((Node *) rinfo->clause))
			safe_restrictions = lappend(safe_restrictions, rinfo->clause);
	}

	/*
	 * 这里可以用弱反驳，因为我们是在把限制
	 * 子句与限制子句比较。
	 */
	if (predicate_refuted_by(safe_restrictions, safe_restrictions, true))
		return true;

	/*
	 * 只有普通关系才有约束，因此其他 rtekinds 到此为止。
	 */
	if (rte->rtekind != RTE_RELATION)
		return false;

	/*
	 * 若只扫描这一张表，可以使用 NO INHERIT 约束，
	 * 但若也扫描它的子表就不行。（注意分区
	 * 表不应当有 NO INHERIT 约束；但这里不必
	 * 由我们假定这一点。）
	 */
	include_noinherit = !rte->inh;

	/*
	 * 目前，除非这是分区表，attnotnull 约束必须当作 NO INHERIT。
	 * 将来也许会更准确地跟踪它们的
	 * 继承状态，从而细化这一点。
	 */
	include_notnull = (!rte->inh || rte->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * 取出适当的那一组约束表达式。
	 */
	constraint_pred = get_relation_constraints(root, rte->relid, rel,
											   include_noinherit,
											   include_notnull,
											   include_partition);

	/*
	 * 目前我们并不强制 CHECK 约束只含
	 * 不可变函数，因此这里必须检查。我们不敢从
	 * 对非不可变函数的规划时求值得出结论。既然它们是
	 * ANDed 在一起的，可以忽略列表中任何可变约束，
	 * 只对其余的做推理。
	 */
	safe_constraints = NIL;
	foreach(lc, constraint_pred)
	{
		Node	   *pred = (Node *) lfirst(lc);

		if (!contain_mutable_functions(pred))
			safe_constraints = lappend(safe_constraints, pred);
	}

	/*
	 * 这些约束实际上是 ANDed 在一起的，因此可以一次尝试
	 * 反驳整个集合。这也许能让我们做出
	 * 逐条拿来就会失败的证明。
	 *
	 * 注意：我们用 rel->baserestrictinfo，而不是看似明显的优化 safe_restrictions。
	 * 有些子句可能是 OR 子句，其中
	 * 既有易变也有非易变的子子句，用非易变部分做
	 * 推导是可以的。
	 *
	 * 我们需要强反驳，因为必须证明约束
	 * 会得出假，而不只是 NULL。
	 */
	if (predicate_refuted_by(safe_constraints, rel->baserestrictinfo, false))
		return true;

	return false;
}


/*
 * build_physical_tlist
 *
 * 建立恰好由该关系用户属性组成的目标列表，
 * 按顺序排列。执行器可以对这种目标列表做特殊处理，从而避免运行时的投影
 * 步骤，因此扫描节点优先使用这种目标列表。
 *
 * 例外：若有任何已删除或缺少的列，就放弃并返回
 * NIL。理想情况下这些情形也想处理。但这
 * 会给 ExecTypeFromTL 带来问题，它可能被要求为含有
 * 已不存在类型的 var 的目标列表建立 tupdesc。理论上我们可以
 * 从该关系的 pg_attribute 项里挖出所需信息，
 * 但那些数据并不是 ExecTypeFromTL 现成能用的。
 * 目前，有已删除的列时
 * 就不做物理目标列表优化。
 *
 * 我们也支持为子查询、函数、
 * 值列表、表表达式和 CTEs 建立物理目标列表，因为同样的优化可以
 * 发生在 SubqueryScan、FunctionScan、ValuesScan、CteScan、TableFunc、
 * NamedTuplestoreScan 和 WorkTableScan 节点上。
 */
List *
build_physical_tlist(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *tlist = NIL;
	Index		varno = rel->relid;
	RangeTblEntry *rte = planner_rt_fetch(varno, root);
	Relation	relation;
	Query	   *subquery;
	Var		   *var;
	ListCell   *l;
	int			attrno,
				numattrs;
	List	   *colvars;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* 假定已经有足够的锁 */
			relation = table_open(rte->relid, NoLock);

			numattrs = RelationGetNumberOfAttributes(relation);
			for (attrno = 1; attrno <= numattrs; attrno++)
			{
				Form_pg_attribute att_tup = TupleDescAttr(relation->rd_att,
														  attrno - 1);

				if (att_tup->attisdropped || att_tup->atthasmissing)
				{
					/* 发现已删除或缺少的列，因此放弃 */
					tlist = NIL;
					break;
				}

				var = makeVar(varno,
							  attrno,
							  att_tup->atttypid,
							  att_tup->atttypmod,
							  att_tup->attcollation,
							  0);

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												attrno,
												NULL,
												false));
			}

			table_close(relation, NoLock);
			break;

		case RTE_SUBQUERY:
			subquery = rte->subquery;
			foreach(l, subquery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);

				/*
				 * 子查询的废列可以反映为
				 * 物理目标列表里的废列；不必放弃。
				 */
				var = makeVarFromTargetEntry(varno, tle);

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												tle->resno,
												NULL,
												tle->resjunk));
			}
			break;

		case RTE_FUNCTION:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:
			/* 并非所有这些都能有已删除的列，但反正共用代码 */
			expandRTE(rte, varno, 0, -1, true /* 包括已删除的 */ ,
					  NULL, &colvars);
			foreach(l, colvars)
			{
				var = (Var *) lfirst(l);

				/*
				 * expandRTE 的输出里若有非 Var，就表示已删除的列；
				 * 必须放弃。
				 */
				if (!IsA(var, Var))
				{
					tlist = NIL;
					break;
				}

				tlist = lappend(tlist,
								makeTargetEntry((Expr *) var,
												var->varattno,
												NULL,
												false));
			}
			break;

		default:
			/* 调用者错误 */
			elog(ERROR, "unsupported RTE kind %d in build_physical_tlist",
				 (int) rte->rtekind);
			break;
	}

	return tlist;
}

/*
 * build_index_tlist
 *
 * 建立表示指定索引各列的目标列表。
 * 每一列用对应基关系
 * 列的 Var 表示，或用基关系 Var 上的表达式，视情况而定。
 *
 * 索引里从来没有已删除的列，因此与
 * build_physical_tlist 不同，这里不需要失败情形。
 */
static List *
build_index_tlist(PlannerInfo *root, IndexOptInfo *index,
				  Relation heapRelation)
{
	List	   *tlist = NIL;
	Index		varno = index->rel->relid;
	ListCell   *indexpr_item;
	int			i;

	indexpr_item = list_head(index->indexprs);
	for (i = 0; i < index->ncolumns; i++)
	{
		int			indexkey = index->indexkeys[i];
		Expr	   *indexvar;

		if (indexkey != 0)
		{
			/* 简单列 */
			const FormData_pg_attribute *att_tup;

			if (indexkey < 0)
				att_tup = SystemAttributeDefinition(indexkey);
			else
				att_tup = TupleDescAttr(heapRelation->rd_att, indexkey - 1);

			indexvar = (Expr *) makeVar(varno,
										indexkey,
										att_tup->atttypid,
										att_tup->atttypmod,
										att_tup->attcollation,
										0);
		}
		else
		{
			/* 表达式列 */
			if (indexpr_item == NULL)
				elog(ERROR, "wrong number of index expressions");
			indexvar = (Expr *) lfirst(indexpr_item);
			indexpr_item = lnext(index->indexprs, indexpr_item);
		}

		tlist = lappend(tlist,
						makeTargetEntry(indexvar,
										i + 1,
										NULL,
										false));
	}
	if (indexpr_item != NULL)
		elog(ERROR, "wrong number of index expressions");

	return tlist;
}

/*
 * restriction_selectivity
 *
 * 返回指定限制操作符子句的选择率。
 * 这段代码通过调用函数管理器，执行登记在
 * 操作符关系里的过程。
 *
 * 其余参数的含义见 clause_selectivity()。
 */
Selectivity
restriction_selectivity(PlannerInfo *root,
						Oid operatorid,
						List *args,
						Oid inputcollid,
						int varRelid)
{
	RegProcedure oprrest = get_oprrest(operatorid);
	float8		result;

	/*
	 * 若 oprrest 过程因任何原因缺少，就用
	 * 0.5 的选择率
	 */
	if (!oprrest)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall4Coll(oprrest,
												 inputcollid,
												 PointerGetDatum(root),
												 ObjectIdGetDatum(operatorid),
												 PointerGetDatum(args),
												 Int32GetDatum(varRelid)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "invalid restriction selectivity: %f", result);

	return (Selectivity) result;
}

/*
 * join_selectivity
 *
 * 返回指定连接操作符子句的选择率。
 * 这段代码通过调用函数管理器，执行登记在
 * 操作符关系里的过程。
 *
 * 其余参数的含义见 clause_selectivity()。
 */
Selectivity
join_selectivity(PlannerInfo *root,
				 Oid operatorid,
				 List *args,
				 Oid inputcollid,
				 JoinType jointype,
				 SpecialJoinInfo *sjinfo)
{
	RegProcedure oprjoin = get_oprjoin(operatorid);
	float8		result;

	/*
	 * 若 oprjoin 过程因任何原因缺少，就用
	 * 0.5 的选择率
	 */
	if (!oprjoin)
		return (Selectivity) 0.5;

	result = DatumGetFloat8(OidFunctionCall5Coll(oprjoin,
												 inputcollid,
												 PointerGetDatum(root),
												 ObjectIdGetDatum(operatorid),
												 PointerGetDatum(args),
												 Int16GetDatum(jointype),
												 PointerGetDatum(sjinfo)));

	if (result < 0.0 || result > 1.0)
		elog(ERROR, "invalid join selectivity: %f", result);

	return (Selectivity) result;
}

/*
 * function_selectivity
 *
 * 返回指定布尔函数子句的选择率。
 * 这段代码通过调用函数管理器，执行登记在
 * pg_proc 关系里的过程。
 *
 * 其余参数的含义见 clause_selectivity()。
 */
Selectivity
function_selectivity(PlannerInfo *root,
					 Oid funcid,
					 List *args,
					 Oid inputcollid,
					 bool is_join,
					 int varRelid,
					 JoinType jointype,
					 SpecialJoinInfo *sjinfo)
{
	RegProcedure prosupport = get_func_support(funcid);
	SupportRequestSelectivity req;
	SupportRequestSelectivity *sresult;

	/*
	 * 若没有提供支持函数，就用我们历史上的默认
	 * 估计 0.3333333。这似乎是个没什么道理的选择，但
	 * Postgres 从 1992 年起就对函数调用用这个估计。
	 * 这行为年头这么久，说明我们不必
	 * 急着改用别的值。
	 */
	if (!prosupport)
		return (Selectivity) 0.3333333;

	req.type = T_SupportRequestSelectivity;
	req.root = root;
	req.funcid = funcid;
	req.args = args;
	req.inputcollid = inputcollid;
	req.is_join = is_join;
	req.varRelid = varRelid;
	req.jointype = jointype;
	req.sjinfo = sjinfo;
	req.selectivity = -1;		/* 用来抓住没把值设上的失败 */

	sresult = (SupportRequestSelectivity *)
		DatumGetPointer(OidFunctionCall1(prosupport,
										 PointerGetDatum(&req)));

	/* 若支持函数失败，就用默认值 */
	if (sresult != &req)
		return (Selectivity) 0.3333333;

	if (req.selectivity < 0.0 || req.selectivity > 1.0)
		elog(ERROR, "invalid function selectivity: %f", req.selectivity);

	return (Selectivity) req.selectivity;
}

/*
 * add_function_cost
 *
 * 估计一个函数的执行代价，并把它加到
 * *cost 的内容上。估计可以同时包含一次性的和
 * 按元组的部分，因为 QualCost 就是这样。
 *
 * funcid 必须总是提供。若它是作为某个
 * 特定语法树节点（FuncExpr、OpExpr、
 * WindowFunc 等）的实现被调用，就把该节点作为 node 传入，否则传 NULL。
 *
 * 有些用法里 root 也可能是 NULL。
 */
void
add_function_cost(PlannerInfo *root, Oid funcid, Node *node,
				  QualCost *cost)
{
	HeapTuple	proctup;
	Form_pg_proc procform;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	if (OidIsValid(procform->prosupport))
	{
		SupportRequestCost req;
		SupportRequestCost *sresult;

		req.type = T_SupportRequestCost;
		req.root = root;
		req.funcid = funcid;
		req.node = node;

		/* 初始化代价成员，这样支持函数就不必自己做 */
		req.startup = 0;
		req.per_tuple = 0;

		sresult = (SupportRequestCost *)
			DatumGetPointer(OidFunctionCall1(procform->prosupport,
											 PointerGetDatum(&req)));

		if (sresult == &req)
		{
			/* 成功，因此把支持函数的估计累加进 *cost */
			cost->startup += req.startup;
			cost->per_tuple += req.per_tuple;
			ReleaseSysCache(proctup);
			return;
		}
	}

	/* 没有支持函数，或它失败了，因此依赖 procost */
	cost->per_tuple += procform->procost * cpu_operator_cost;

	ReleaseSysCache(proctup);
}

/*
 * get_function_rows
 *
 * 估计集合返回函数返回的行数。
 *
 * funcid 必须总是提供。按当前用法，调用节点
 * 总会提供，并且是 FuncExpr 或 OpExpr。
 * 但若它是 NULL 也不失败，是个好主意。
 *
 * 有些用法里 root 也可能是 NULL。
 *
 * 注意：这返回支持函数未经过滤的结果（若有）。
 * 通常最好对结果应用 clamp_row_est()，但
 * 留给调用者去做。
 */
double
get_function_rows(PlannerInfo *root, Oid funcid, Node *node)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	double		result;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	Assert(procform->proretset);	/* 否则是调用者错误 */

	if (OidIsValid(procform->prosupport))
	{
		SupportRequestRows req;
		SupportRequestRows *sresult;

		req.type = T_SupportRequestRows;
		req.root = root;
		req.funcid = funcid;
		req.node = node;

		req.rows = 0;			/* 只是为了健全性 */

		sresult = (SupportRequestRows *)
			DatumGetPointer(OidFunctionCall1(procform->prosupport,
											 PointerGetDatum(&req)));

		if (sresult == &req)
		{
			/* 成功 */
			ReleaseSysCache(proctup);
			return req.rows;
		}
	}

	/* 没有支持函数，或它失败了，因此依赖 prorows */
	result = procform->prorows;

	ReleaseSysCache(proctup);

	return result;
}

/*
 * has_unique_index
 *
 * 检测指定关系的指定属性上是否有唯一索引，
 * 从而可以断定该属性的所有
 * （非空）值都不同。
 *
 * 本函数不检查索引的 indimmediate 属性，这
 * 意味着唯一性在事务内部可能暂时不成立。
 * 做统计估计时这样是合适的，但
 * 用它做任何正确性证明时要当心。
 */
bool
has_unique_index(RelOptInfo *rel, AttrNumber attno)
{
	ListCell   *ilist;

	foreach(ilist, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);

		/*
		 * 注意：忽略部分索引，因为它们不能让我们断定
		 * 所有属性值都不同，除非它们标了 predOK，
		 * 那表示我们知道该索引的谓词被
		 * 查询满足。我们也不关心表达式索引。
		 * 另外，多列唯一索引也不能让我们断定
		 * 仅仅指定的那个属性是唯一的。
		 */
		if (index->unique &&
			index->nkeycolumns == 1 &&
			index->indexkeys[0] == attno &&
			(index->indpred == NIL || index->predOK))
			return true;
	}
	return false;
}


/*
 * has_row_triggers
 *
 * 检测指定关系对给定事件是否有任何行级触发器。
 */
bool
has_row_triggers(PlannerInfo *root, Index rti, CmdType event)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TriggerDesc *trigDesc;
	bool		result = false;

	/* 假定已经有足够的锁 */
	relation = table_open(rte->relid, NoLock);

	trigDesc = relation->trigdesc;
	switch (event)
	{
		case CMD_INSERT:
			if (trigDesc &&
				(trigDesc->trig_insert_after_row ||
				 trigDesc->trig_insert_before_row))
				result = true;
			break;
		case CMD_UPDATE:
			if (trigDesc &&
				(trigDesc->trig_update_after_row ||
				 trigDesc->trig_update_before_row))
				result = true;
			break;
		case CMD_DELETE:
			if (trigDesc &&
				(trigDesc->trig_delete_after_row ||
				 trigDesc->trig_delete_before_row))
				result = true;
			break;
			/* MERGE 没有单独的事件，只有 INSERT/UPDATE/DELETE */
		case CMD_MERGE:
			result = false;
			break;
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) event);
			break;
	}

	table_close(relation, NoLock);
	return result;
}

/*
 * has_transition_tables
 *
 * 检测指定关系对给定事件是否有任何转换表。
 */
bool
has_transition_tables(PlannerInfo *root, Index rti, CmdType event)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TriggerDesc *trigDesc;
	bool		result = false;

	Assert(rte->rtekind == RTE_RELATION);

	/* 目前外部表无法拥有转换表 */
	if (rte->relkind == RELKIND_FOREIGN_TABLE)
		return result;

	/* 假定已经有足够的锁 */
	relation = table_open(rte->relid, NoLock);

	trigDesc = relation->trigdesc;
	switch (event)
	{
		case CMD_INSERT:
			if (trigDesc &&
				trigDesc->trig_insert_new_table)
				result = true;
			break;
		case CMD_UPDATE:
			if (trigDesc &&
				(trigDesc->trig_update_old_table ||
				 trigDesc->trig_update_new_table))
				result = true;
			break;
		case CMD_DELETE:
			if (trigDesc &&
				trigDesc->trig_delete_old_table)
				result = true;
			break;
			/* MERGE 没有单独的事件，只有 INSERT/UPDATE/DELETE */
		case CMD_MERGE:
			result = false;
			break;
		default:
			elog(ERROR, "unrecognized CmdType: %d", (int) event);
			break;
	}

	table_close(relation, NoLock);
	return result;
}

/*
 * has_stored_generated_columns
 *
 * 由 RTI 标识的表是否有任何 STORED GENERATED 列？
 */
bool
has_stored_generated_columns(PlannerInfo *root, Index rti)
{
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TupleDesc	tupdesc;
	bool		result = false;

	/* 假定已经有足够的锁 */
	relation = table_open(rte->relid, NoLock);

	tupdesc = RelationGetDescr(relation);
	result = tupdesc->constr && tupdesc->constr->has_generated_stored;

	table_close(relation, NoLock);

	return result;
}

/*
 * get_dependent_generated_columns
 *
 * 取出该关系中依赖 target_cols 所列任一列的
 * 任何 STORED GENERATED 列的列号。输入和
 * 结果位图集里的列号都偏移了
 * FirstLowInvalidHeapAttributeNumber。
 */
Bitmapset *
get_dependent_generated_columns(PlannerInfo *root, Index rti,
								Bitmapset *target_cols)
{
	Bitmapset  *dependentCols = NULL;
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	Relation	relation;
	TupleDesc	tupdesc;
	TupleConstr *constr;

	/* 假定已经有足够的锁 */
	relation = table_open(rte->relid, NoLock);

	tupdesc = RelationGetDescr(relation);
	constr = tupdesc->constr;

	if (constr && constr->has_generated_stored)
	{
		for (int i = 0; i < constr->num_defval; i++)
		{
			AttrDefault *defval = &constr->defval[i];
			Node	   *expr;
			Bitmapset  *attrs_used = NULL;

			/* 若不是生成列则跳过 */
			if (!TupleDescAttr(tupdesc, defval->adnum - 1)->attgenerated)
				continue;

			/* 认出这个生成列依赖哪些列 */
			expr = stringToNode(defval->adbin);
			pull_varattnos(expr, 1, &attrs_used);

			if (bms_overlap(target_cols, attrs_used))
				dependentCols = bms_add_member(dependentCols,
											   defval->adnum - FirstLowInvalidHeapAttributeNumber);
		}
	}

	table_close(relation, NoLock);

	return dependentCols;
}

/*
 * set_relation_partition_info
 *
 * 为分区表设置分区方案及相关信息。
 */
static void
set_relation_partition_info(PlannerInfo *root, RelOptInfo *rel,
							Relation relation)
{
	PartitionDesc partdesc;

	/*
	 * 若还没有，就建立 PartitionDirectory 基础设施。
	 */
	if (root->glob->partition_directory == NULL)
	{
		root->glob->partition_directory =
			CreatePartitionDirectory(CurrentMemoryContext, true);
	}

	partdesc = PartitionDirectoryLookup(root->glob->partition_directory,
										relation);
	rel->part_scheme = find_partition_scheme(root, relation);
	Assert(partdesc != NULL && rel->part_scheme != NULL);
	rel->boundinfo = partdesc->boundinfo;
	rel->nparts = partdesc->nparts;
	set_baserel_partition_key_exprs(relation, rel);
	set_baserel_partition_constraint(relation, rel);
}

/*
 * find_partition_scheme
 *
 * 为这个 Relation 查找或建立一个 PartitionScheme。
 */
static PartitionScheme
find_partition_scheme(PlannerInfo *root, Relation relation)
{
	PartitionKey partkey = RelationGetPartitionKey(relation);
	ListCell   *lc;
	int			partnatts,
				i;
	PartitionScheme part_scheme;

	/* 分区表应当有分区键。 */
	Assert(partkey != NULL);

	partnatts = partkey->partnatts;

	/* 寻找匹配的分区方案，找到就返回。 */
	foreach(lc, root->part_schemes)
	{
		part_scheme = lfirst(lc);

		/* 匹配分区策略和键的个数。 */
		if (partkey->strategy != part_scheme->strategy ||
			partnatts != part_scheme->partnatts)
			continue;

		/* 匹配分区键的类型属性。 */
		if (memcmp(partkey->partopfamily, part_scheme->partopfamily,
				   sizeof(Oid) * partnatts) != 0 ||
			memcmp(partkey->partopcintype, part_scheme->partopcintype,
				   sizeof(Oid) * partnatts) != 0 ||
			memcmp(partkey->partcollation, part_scheme->partcollation,
				   sizeof(Oid) * partnatts) != 0)
			continue;

		/*
		 * 当 partopcintype 匹配时，长度和 byval 信息
		 * 也应当匹配。
		 */
		Assert(memcmp(partkey->parttyplen, part_scheme->parttyplen,
					  sizeof(int16) * partnatts) == 0);
		Assert(memcmp(partkey->parttypbyval, part_scheme->parttypbyval,
					  sizeof(bool) * partnatts) == 0);

		/*
		 * 若 partopfamily 和 partopcintype 匹配，就必须有相同的
		 * 分区比较函数。注意我们无法可靠地
		 * 对函数结构本身做相等 Assert，因为它们在不同
		 * PartitionKey 之间可能不同，所以只对函数
		 * OIDs 做 Assert。
		 */
#ifdef USE_ASSERT_CHECKING
		for (i = 0; i < partkey->partnatts; i++)
			Assert(partkey->partsupfunc[i].fn_oid ==
				   part_scheme->partsupfunc[i].fn_oid);
#endif

		/* 找到了匹配的分区方案。 */
		return part_scheme;
	}

	/*
	 * 没有找到匹配的分区方案。复制有关
	 * 信息，从 relcache 建立一个。需要复制数组的内容，
	 * 因为我们关闭
	 * 关系之后，relcache 项也许留不下来。
	 */
	part_scheme = (PartitionScheme) palloc0(sizeof(PartitionSchemeData));
	part_scheme->strategy = partkey->strategy;
	part_scheme->partnatts = partkey->partnatts;

	part_scheme->partopfamily = (Oid *) palloc(sizeof(Oid) * partnatts);
	memcpy(part_scheme->partopfamily, partkey->partopfamily,
		   sizeof(Oid) * partnatts);

	part_scheme->partopcintype = (Oid *) palloc(sizeof(Oid) * partnatts);
	memcpy(part_scheme->partopcintype, partkey->partopcintype,
		   sizeof(Oid) * partnatts);

	part_scheme->partcollation = (Oid *) palloc(sizeof(Oid) * partnatts);
	memcpy(part_scheme->partcollation, partkey->partcollation,
		   sizeof(Oid) * partnatts);

	part_scheme->parttyplen = (int16 *) palloc(sizeof(int16) * partnatts);
	memcpy(part_scheme->parttyplen, partkey->parttyplen,
		   sizeof(int16) * partnatts);

	part_scheme->parttypbyval = (bool *) palloc(sizeof(bool) * partnatts);
	memcpy(part_scheme->parttypbyval, partkey->parttypbyval,
		   sizeof(bool) * partnatts);

	part_scheme->partsupfunc = (FmgrInfo *)
		palloc(sizeof(FmgrInfo) * partnatts);
	for (i = 0; i < partnatts; i++)
		fmgr_info_copy(&part_scheme->partsupfunc[i], &partkey->partsupfunc[i],
					   CurrentMemoryContext);

	/* 把分区方案加进 PlannerInfo。 */
	root->part_schemes = lappend(root->part_schemes, part_scheme);

	return part_scheme;
}

/*
 * set_baserel_partition_key_exprs
 *
 * 为给定基关系建立分区键表达式，并填入
 * rel->partexprs。
 */
static void
set_baserel_partition_key_exprs(Relation relation,
								RelOptInfo *rel)
{
	PartitionKey partkey = RelationGetPartitionKey(relation);
	int			partnatts;
	int			cnt;
	List	  **partexprs;
	ListCell   *lc;
	Index		varno = rel->relid;

	Assert(IS_SIMPLE_REL(rel) && rel->relid > 0);

	/* 分区表应当有分区键。 */
	Assert(partkey != NULL);

	partnatts = partkey->partnatts;
	partexprs = (List **) palloc(sizeof(List *) * partnatts);
	lc = list_head(partkey->partexprs);

	for (cnt = 0; cnt < partnatts; cnt++)
	{
		Expr	   *partexpr;
		AttrNumber	attno = partkey->partattrs[cnt];

		if (attno != InvalidAttrNumber)
		{
			/* 单列分区键存成一个 Var 节点。 */
			Assert(attno > 0);

			partexpr = (Expr *) makeVar(varno, attno,
										partkey->parttypid[cnt],
										partkey->parttypmod[cnt],
										partkey->parttypcoll[cnt], 0);
		}
		else
		{
			if (lc == NULL)
				elog(ERROR, "wrong number of partition key expressions");

			/* 用给定的 varno 重新标记该表达式。 */
			partexpr = (Expr *) copyObject(lfirst(lc));
			ChangeVarNodes((Node *) partexpr, 1, varno, 0);
			lc = lnext(partkey->partexprs, lc);
		}

		/* 基关系的每个键只有一个表达式。 */
		partexprs[cnt] = list_make1(partexpr);
	}

	rel->partexprs = partexprs;

	/*
	 * 基关系没有可空的分区键表达式，因为
	 * 不涉及外连接。我们仍然分配一个空
	 * 表达式列表的数组，好让分区键表达式的处理代码保持简单。
	 * 见 build_joinrel_partition_info() 和 match_expr_to_partition_keys()。
	 */
	rel->nullable_partexprs = (List **) palloc0(sizeof(List *) * partnatts);
}

/*
 * set_baserel_partition_constraint
 *
 * 为给定基关系建立分区约束，并设置到
 * 给定的 RelOptInfo 里。所有 Var 节点都用该关系的 relid
 * 重新标记。
 */
static void
set_baserel_partition_constraint(Relation relation, RelOptInfo *rel)
{
	List	   *partconstr;

	if (rel->partition_qual)	/* 已经做过 */
		return;

	/*
	 * 把分区限定条件送过与检查
	 * 约束类似的常量简化。不过跳过 canonicalize_qual，因为分区
	 * 限定条件应当已经是规范形式；另外，由于限定条件是
	 * 隐式 AND 形式，还得显式转换成显式 AND
	 * 形式再转回来。
	 */
	partconstr = RelationGetPartitionQual(relation);
	if (partconstr)
	{
		partconstr = (List *) expression_planner((Expr *) partconstr);
		if (rel->relid != 1)
			ChangeVarNodes((Node *) partconstr, 1, rel->relid, 0);
		rel->partition_qual = partconstr;
	}
}
