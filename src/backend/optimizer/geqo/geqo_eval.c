/*------------------------------------------------------------------------
 *
 * geqo_eval.c
 *	  Routines to evaluate query trees
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/optimizer/geqo/geqo_eval.c
 *
 *-------------------------------------------------------------------------
 */

/* 贡献者：
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
   *  Martin Utesch				 * 自动控制研究所	   *
   =							 = 矿业技术大学 =
   *  utesch@aut.tu-freiberg.de  * 德国弗赖贝格				   *
   =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=
 */

#include "postgres.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include "optimizer/geqo.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "utils/memutils.h"


/* gimme_tree 里已经连接在一起的关系 "clump"（团块） */
typedef struct
{
	RelOptInfo *joinrel;		/* 这组关系的连接关系 */
	int			size;			/* 团块中的输入关系个数 */
} Clump;

static List *merge_clump(PlannerInfo *root, List *clumps, Clump *new_clump,
						 int num_gene, bool force);
static bool desirable_join(PlannerInfo *root,
						   RelOptInfo *outer_rel, RelOptInfo *inner_rel);


/*
 * geqo_eval
 *
 * 把查询树的代价作为种群中一个个体返回。
 *
 * 若从提议的 tour 中抽不出合法连接顺序，
 * 则返回 DBL_MAX。
 */
Cost
geqo_eval(PlannerInfo *root, Gene *tour, int num_gene)
{
	MemoryContext mycontext;
	MemoryContext oldcxt;
	RelOptInfo *joinrel;
	Cost		fitness;
	int			savelength;
	struct HTAB *savehash;

	/*
	 * 创建一个私有内存上下文，用来存放全部临时存储，
	 * 这些存储是在 gimme_tree() 内部分配的。
	 *
	 * 由于 geqo_eval() 会被调用很多次，我们无法让这些
	 * 内存一直留到语句结束才回收。注意，我们把
	 * 临时上下文做成规划器普通上下文的子级，这样即使
	 * 经由 ereport(ERROR) 中止，它也会被释放。
	 */
	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "GEQO",
									  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycontext);

	/*
	 * gimme_tree 会向 root->join_rel_list 添加条目，其中也许已经
	 * 有一些条目，也许还没有。新添加的条目会被
	 * 下面的 MemoryContextDelete 回收，因此必须确保
	 * 退出前把列表恢复成原来的状态。做法是
	 * 把列表截断到原来的长度。注意：这里假定任何
	 * 新条目都追加在末尾！
	 *
	 * 还必须注意不要弄乱外层的 join_rel_hash（如果
	 * 有的话）。只要暂时把该链接设为 NULL 即可。
	 * （如果涉及的连接关系足够多——很可能如此——就会
	 * 在本地新建并使用一张哈希表。）
	 *
	 * join_rel_level[] 不应当处于使用中，因此直接 Assert 它没有。
	 */
	savelength = list_length(root->join_rel_list);
	savehash = root->join_rel_hash;
	Assert(root->join_rel_level == NULL);

	root->join_rel_hash = NULL;

	/* 为给定的关系组合构造最佳路径 */
	joinrel = gimme_tree(root, tour, num_gene);

	/*
	 * 若找到有效连接，则计算适应度
	 *
	 * XXX geqo 目前不支持针对部分结果
	 * 检索的优化，也不考虑可能使用
	 * 参数化路径——如何修复？
	 */
	if (joinrel)
	{
		Path	   *best_path = joinrel->cheapest_total_path;

		fitness = best_path->total_cost;
	}
	else
		fitness = DBL_MAX;

	/*
	 * 把 join_rel_list 恢复成原来的状态，并放回原来的
	 * 哈希表（如果有）。
	 */
	root->join_rel_list = list_truncate(root->join_rel_list,
										savelength);
	root->join_rel_hash = savehash;

	/* 释放在 gimme_tree 内获得的全部内存 */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycontext);

	return fitness;
}

/*
 * gimme_tree
 *	  为按指定顺序构造的连接树
 *	  生成规划器估计。
 *
 *	 'tour' 是提议的连接顺序，长度为 'num_gene'
 *
 * 返回一个新的连接关系，其最低代价路径就是该
 * 连接顺序的最佳计划。注意：若连接顺序无效，且
 * 无法把它改成有效顺序，则返回 NULL。
 *
 * 本例程最初的实现总是按指定顺序连接，
 * 因此只能构造左深计划（以及右深计划和
 * 混合计划，这是 make_join_rel() 对称性的副产品）。
 * 它永远产生不了 "丛状" 计划。这带来几个大问题，
 * 其中最严重的是：在存在连接顺序
 * 限制时，唯一有效的计划是丛状的。
 *
 * 现在的实现把给定 tour 当作指导，但会
 * 推迟违规或按某些启发式规则看来不合适的
 * 连接。这样就能在需要时生成正确的丛状计划，
 * 而且作为不错的副作用，似乎明显提高了所生成计划的
 * 质量。不过请注意，这只是启发式，在某些情况下仍会
 * 失败。（尤其是，我们可能把实际上因 LATERAL 限制而
 * 还不得连接的关系团到一起；
 * 由于没有拆团的机制，这必然导致失败。）
 */
RelOptInfo *
gimme_tree(PlannerInfo *root, Gene *tour, int num_gene)
{
	GeqoPrivateData *private = (GeqoPrivateData *) root->join_search_private;
	List	   *clumps;
	int			rel_count;

	/*
	 * 有时某个关系因启发式
	 * 或实际的语义限制，暂时无法与其他关系连接。我们维护一个团块列表，
	 * 存放已成功连接的关系，较大的团块在前。tour 中的每个
	 * 新关系被加到它能连上的第一个团块；
	 * 若没有这样的团块，它自己成为一个新团块。当我们
	 * 扩大已有团块时，检查它现在能否与
	 * 其他团块合并。tour 全部扫描完后，我们放下
	 * 启发式，尝试强制连接剩余团块。如果
	 * 无法把所有团块并成一个，则失败。
	 */
	clumps = NIL;

	for (rel_count = 0; rel_count < num_gene; rel_count++)
	{
		int			cur_rel_index;
		RelOptInfo *cur_rel;
		Clump	   *cur_clump;

		/* 取下一个输入关系 */
		cur_rel_index = (int) tour[rel_count];
		cur_rel = (RelOptInfo *) list_nth(private->initial_rels,
										  cur_rel_index - 1);

		/* 把它做成只含一个关系的团块 */
		cur_clump = (Clump *) palloc(sizeof(Clump));
		cur_clump->joinrel = cur_rel;
		cur_clump->size = 1;

		/* 只用合意的连接，把它并入团块列表 */
		clumps = merge_clump(root, clumps, cur_clump, num_gene, false);
	}

	if (list_length(clumps) > 1)
	{
		/* 按某种合法顺序强制连接剩余团块 */
		List	   *fclumps;
		ListCell   *lc;

		fclumps = NIL;
		foreach(lc, clumps)
		{
			Clump	   *clump = (Clump *) lfirst(lc);

			fclumps = merge_clump(root, fclumps, clump, num_gene, true);
		}
		clumps = fclumps;
	}

	/* 是否成功形成了单一连接关系？ */
	if (list_length(clumps) != 1)
		return NULL;

	return ((Clump *) linitial(clumps))->joinrel;
}

/*
 * 把一个团块并入 gimme_tree 的已有团块列表。
 *
 * 尝试把该团块并入某个已有团块，成功则
 * 重复。无法再合并时，把该团块
 * 插入列表，并保持列表的排序规则（即
 * 较大的团块排在前面）。
 *
 * 若 force 为 true，只要连接合法就合并，即使这会造成
 * 一次笛卡尔连接。若 force 为 false，只做
 * "合意的" 连接。
 */
static List *
merge_clump(PlannerInfo *root, List *clumps, Clump *new_clump, int num_gene,
			bool force)
{
	ListCell   *lc;
	int			pos;

	/* 寻找 new_clump 可以连上的团块 */
	foreach(lc, clumps)
	{
		Clump	   *old_clump = (Clump *) lfirst(lc);

		if (force ||
			desirable_join(root, old_clump->joinrel, new_clump->joinrel))
		{
			RelOptInfo *joinrel;

			/*
			 * 构造一个 RelOptInfo，表示这两个输入
			 * 关系的连接。注意，我们预期该连接关系还不在
			 * root->join_rel_list 中，因此为它构造的路径
			 * 只会包含我们想要的那些。
			 */
			joinrel = make_join_rel(root,
									old_clump->joinrel,
									new_clump->joinrel);

			/* 若连接顺序无效则继续寻找 */
			if (joinrel)
			{
				/* 为按分区连接创建路径。 */
				generate_partitionwise_join_paths(root, joinrel);

				/*
				 * 除最顶层的扫描/连接关系外，考虑汇集
				 * 部分路径。等知道最终目标列表后，对最顶层的扫描/连接
				 * 关系也会这样做（见
				 * grouping_planner）。
				 */
				if (!bms_equal(joinrel->relids, root->all_query_rels))
					generate_useful_gather_paths(root, joinrel, false);

				/* 找出并保存该连接关系的最低代价路径 */
				set_cheapest(joinrel);

				/* 把新团块吸收进旧团块 */
				old_clump->joinrel = joinrel;
				old_clump->size += new_clump->size;
				pfree(new_clump);

				/* 从列表中去掉 old_clump */
				clumps = foreach_delete_current(clumps, lc);

				/*
				 * 递归尝试把扩大后的 old_clump 与
				 * 其他团块合并。无法再合并时，再把它插回
				 * 列表。
				 */
				return merge_clump(root, clumps, old_clump, num_gene, force);
			}
		}
	}

	/*
	 * 无法再合并，因此把 new_clump 作为独立团块，按
	 * 大小放入正确位置。常见情况可以走快路径：
	 * 大小为 1 时，它总是应该放在末尾。
	 */
	if (clumps == NIL || new_clump->size == 1)
		return lappend(clumps, new_clump);

	/* 否则寻找插入位置 */
	for (pos = 0; pos < list_length(clumps); pos++)
	{
		Clump	   *old_clump = (Clump *) list_nth(clumps, pos);

		if (new_clump->size > old_clump->size)
			break;				/* new_clump 应排在 old_clump 之前 */
	}
	clumps = list_insert_nth(clumps, pos, new_clump);

	return clumps;
}

/*
 * gimme_tree 的启发式：这两个关系要不要连接？
 */
static bool
desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel)
{
	/*
	 * 若有适用的连接子句，或有连接顺序
	 * 限制强制这些关系相连，则进行连接。
	 */
	if (have_relevant_joinclause(root, outer_rel, inner_rel) ||
		have_join_order_restriction(root, outer_rel, inner_rel))
		return true;

	/* 否则把这次连接推迟。 */
	return false;
}
