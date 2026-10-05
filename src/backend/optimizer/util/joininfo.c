/*-------------------------------------------------------------------------
 *
 * joininfo.c
 *	  操作 joininfo 列表的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/joininfo.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"


/*
 * have_relevant_joinclause
 *		检测是否存在涉及
 *		给定两个关系的连接子句。
 *
 * 注意：该连接子句不必只用这两个
 * 关系就能求值。这是有意的。例如考虑
 *		SELECT * FROM a, b, c WHERE a.x = (b.y + c.z)
 * 若 a 比其他表大得多，也许值得
 * 先交叉连接 b 和 c，再对 a.x 做内索引扫描。因此
 * 即使在那一步连接上还不能应用它，也应把这条连接子句当作
 * 把 b 连到 c 的理由。
 */
bool
have_relevant_joinclause(PlannerInfo *root,
						 RelOptInfo *rel1, RelOptInfo *rel2)
{
	bool		result = false;
	List	   *joininfo;
	Relids		other_relids;
	ListCell   *l;

	/*
	 * 可以扫描任一关系的 joininfo 列表；不妨用
	 * 较短的那个。
	 */
	if (list_length(rel1->joininfo) <= list_length(rel2->joininfo))
	{
		joininfo = rel1->joininfo;
		other_relids = rel2->relids;
	}
	else
	{
		joininfo = rel2->joininfo;
		other_relids = rel1->relids;
	}

	foreach(l, joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_overlap(other_relids, rinfo->required_relids))
		{
			result = true;
			break;
		}
	}

	/*
	 * 还需要检查 EquivalenceClass 数据结构，其中可能
	 * 含有没有放进 joininfo 列表的关系。
	 */
	if (!result && rel1->has_eclass_joins && rel2->has_eclass_joins)
		result = have_relevant_eclass_joinclause(root, rel1, rel2);

	return result;
}


/*
 * add_join_clause_to_rels
 *	  把 restrictinfo 加到它所需要的每个关系的 joininfo 列表。
 *
 * 注意同一份 restrictinfo 节点被它所属的全部
 * 列表链接。这让我们可以利用关于
 * 该限制子句的信息缓存（但必须小心这些信息
 * 不依赖于上下文）。
 *
 * restrictinfo 描述该连接子句
 * join_relids 是参与该连接子句的关系集合
 *				 （其中有些可能是外连接）
 */
void
add_join_clause_to_rels(PlannerInfo *root,
						RestrictInfo *restrictinfo,
						Relids join_relids)
{
	int			cur_relid;

	cur_relid = -1;
	while ((cur_relid = bms_next_member(join_relids, cur_relid)) >= 0)
	{
		RelOptInfo *rel = find_base_rel_ignore_join(root, cur_relid);

		/* 我们只需把子句加到基关系上 */
		if (rel == NULL)
			continue;
		rel->joininfo = lappend(rel->joininfo, restrictinfo);
	}
}

/*
 * remove_join_clause_from_rels
 *	  从它所属的全部 joininfo 列表中删除 restrictinfo
 *
 * 这逆转 add_join_clause_to_rels 的效果。当我们发现
 * 某个关系根本不必参与连接时使用。
 *
 * restrictinfo 描述该连接子句
 * join_relids 是参与该连接子句的关系集合
 *				 （其中有些可能是外连接）
 */
void
remove_join_clause_from_rels(PlannerInfo *root,
							 RestrictInfo *restrictinfo,
							 Relids join_relids)
{
	int			cur_relid;

	cur_relid = -1;
	while ((cur_relid = bms_next_member(join_relids, cur_relid)) >= 0)
	{
		RelOptInfo *rel = find_base_rel_ignore_join(root, cur_relid);

		/* 我们本来只会把子句加到基关系上 */
		if (rel == NULL)
			continue;

		/*
		 * 从列表中去掉该 restrictinfo。指针比较
		 * 就足够了。
		 */
		Assert(list_member_ptr(rel->joininfo, restrictinfo));
		rel->joininfo = list_delete_ptr(rel->joininfo, restrictinfo);
	}
}
