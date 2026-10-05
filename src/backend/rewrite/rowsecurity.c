/*
 * rewrite/rowsecurity.c
 *	  Routines to support policies for row-level security (aka RLS).
 *
 * Policies in PostgreSQL provide a mechanism to limit what records are
 * returned to a user and what records a user is permitted to add to a table.
 *
 * Policies can be defined for specific roles, specific commands, or provided
 * by an extension.  Row security can also be enabled for a table without any
 * policies being explicitly defined, in which case a default-deny policy is
 * applied.
 *
 * Any part of the system which is returning records back to the user, or
 * which is accepting records from the user to add to a table, needs to
 * consider the policies associated with the table (if any).  For normal
 * queries, this is handled by calling get_row_security_policies() during
 * rewrite, for each RTE in the query.  This returns the expressions defined
 * by the table's policies as a list that is prepended to the securityQuals
 * list for the RTE.  For queries which modify the table, any WITH CHECK
 * clauses from the table's policies are also returned and prepended to the
 * list of WithCheckOptions for the Query to check each row that is being
 * added to the table.  Other parts of the system (eg: COPY) simply construct
 * a normal query and use that, if RLS is to be applied.
 *
 * The check to see if RLS should be enabled is provided through
 * check_enable_rls(), which returns an enum (defined in rowsecurity.h) to
 * indicate if RLS should be enabled (RLS_ENABLED), or bypassed (RLS_NONE or
 * RLS_NONE_ENV).  RLS_NONE_ENV indicates that RLS should be bypassed
 * in the current environment, but that may change if the row_security GUC or
 * the current role changes.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rowsecurity.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/syscache.h"

static void get_policies_for_relation(Relation relation,
									  CmdType cmd, Oid user_id,
									  List **permissive_policies,
									  List **restrictive_policies);

static void sort_policies_by_name(List *policies);

static int	row_security_policy_cmp(const ListCell *a, const ListCell *b);

static void add_security_quals(int rt_index,
							   List *permissive_policies,
							   List *restrictive_policies,
							   List **securityQuals,
							   bool *hasSubLinks);

static void add_with_check_options(Relation rel,
								   int rt_index,
								   WCOKind kind,
								   List *permissive_policies,
								   List *restrictive_policies,
								   List **withCheckOptions,
								   bool *hasSubLinks,
								   bool force_using);

static bool check_role_for_policy(ArrayType *policy_roles, Oid user_id);

/*
 * 允许扩展加入自己的安全性策略的钩子。
 *
 * row_security_policy_hook_permissive 可用于加入与其他
 * 宽松策略用 OR 组合的策略。
 *
 * row_security_policy_hook_restrictive 可用于加入无论如何
 * 都会强制执行的策略（它们用 AND 组合）。
 */
row_security_policy_hook_type row_security_policy_hook_permissive = NULL;
row_security_policy_hook_type row_security_policy_hook_restrictive = NULL;

/*
 * 取得应当应用到指定 RTE 的任何行安全性限定条件
 * 和 WithCheckOption 检查。
 *
 * 此外，若启用了行安全性，就把 hasRowSecurity 设为真
 * （即使这个 RTE 没有任何行安全性限定条件），若返回的任何限定条件
 * 含有子链接，就把 hasSubLinks 设为真。
 */
void
get_row_security_policies(Query *root, RangeTblEntry *rte, int rt_index,
						  List **securityQuals, List **withCheckOptions,
						  bool *hasRowSecurity, bool *hasSubLinks)
{
	Oid			user_id;
	int			rls_status;
	Relation	rel;
	CmdType		commandType;
	List	   *permissive_policies;
	List	   *restrictive_policies;
	RTEPermissionInfo *perminfo;

	/* 返回值的默认值 */
	*securityQuals = NIL;
	*withCheckOptions = NIL;
	*hasRowSecurity = false;
	*hasSubLinks = false;

	Assert(rte->rtekind == RTE_RELATION);

	/* 若这不是普通关系，立即返回 */
	if (rte->relkind != RELKIND_RELATION &&
		rte->relkind != RELKIND_PARTITIONED_TABLE)
		return;

	perminfo = getRTEPermissionInfo(root->rteperminfos, rte);

	/* 若设置了 checkAsUser，就切换过去 */
	user_id = OidIsValid(perminfo->checkAsUser) ?
		perminfo->checkAsUser : GetUserId();

	/* 确定这里的行安全性状态，显式传入 checkAsUser */
	rls_status = check_enable_rls(rte->relid, perminfo->checkAsUser, false);

	/* 若这张表完全没有行安全性，就无事可做 */
	if (rls_status == RLS_NONE)
		return;

	/*
	 * RLS_NONE_ENV 表示我们现在不做任何行安全性检查，但环境变化后
	 * 情况可能改变，因此我们把它标成 hasRowSecurity，以便
	 * 环境变化时强制重新规划。
	 */
	if (rls_status == RLS_NONE_ENV)
	{
		/*
		 * 表明此查询可能涉及行安全性，因而环境变化时必须
		 * 重新规划（GUCs、角色），但我们这里
		 * 不加入任何东西。
		 */
		*hasRowSecurity = true;

		return;
	}

	/*
	 * 此关系已启用行安全性。
	 *
	 * 按命令类型取得应当应用的安全性策略。注意，若这不是
	 * 目标关系，我们实际想要的是该关系的 SELECT 策略，
	 * 与查询命令类型无关，例如在 UPDATE t1 ... FROM t2 里，
	 * 我们需要应用 t1 的 UPDATE 策略和 t2 的 SELECT
	 * 策略。
	 */
	rel = table_open(rte->relid, NoLock);

	commandType = rt_index == root->resultRelation ?
		root->commandType : CMD_SELECT;

	/*
	 * 有些情形下，我们需要应用与多种命令类型关联的
	 * USING 策略（它们控制记录的可见性）（见
	 * 下面的具体情形）。
	 *
	 * 考虑这些 USING 策略的应用顺序时，我们
	 * 优先应用权限更高的策略，即允许用户
	 * 锁定记录的那些（UPDATE 和 DELETE），然后是不
	 * 允许的那些（SELECT）。
	 *
	 * 注意，规划器可以自由下推并重排使用
	 * 防泄漏函数的限定条件。
	 *
	 * 在所有情形下，若没有策略子句允许对特定操作类型
	 * 访问表中的行，就会加入一条
	 * 始终为假的子句（默认拒绝策略）（见
	 * add_security_quals）。
	 */

	/*
	 * 对 SELECT，若需要 UPDATE 权限（例如用户
	 * 指定了 FOR [KEY] UPDATE/SHARE），则先加入 UPDATE USING
	 * 限定条件。
	 *
	 * 这样，我们滤掉用户不能通过 UPDATE USING 策略
	 * 访问的 SELECT FOR SHARE/UPDATE 记录，
	 * 类似于这些查询需要普通的 UPDATE 权限。
	 */
	if (commandType == CMD_SELECT && perminfo->requiredPerms & ACL_UPDATE)
	{
		List	   *update_permissive_policies;
		List	   *update_restrictive_policies;

		get_policies_for_relation(rel, CMD_UPDATE, user_id,
								  &update_permissive_policies,
								  &update_restrictive_policies);

		add_security_quals(rt_index,
						   update_permissive_policies,
						   update_restrictive_policies,
						   securityQuals,
						   hasSubLinks);
	}

	/*
	 * 对 SELECT、UPDATE 和 DELETE，加入安全性限定条件以强制 USING
	 * 策略。这些安全性限定条件控制对已有表行的访问。
	 * 限制性策略用 AND 组合在一起，宽松
	 * 策略用 OR 组合在一起。
	 */

	get_policies_for_relation(rel, commandType, user_id, &permissive_policies,
							  &restrictive_policies);

	if (commandType == CMD_SELECT ||
		commandType == CMD_UPDATE ||
		commandType == CMD_DELETE)
		add_security_quals(rt_index,
						   permissive_policies,
						   restrictive_policies,
						   securityQuals,
						   hasSubLinks);

	/*
	 * 与上面类似，在 UPDATE、DELETE 或 MERGE 期间，若还需要 SELECT 权限
	 * （例如存在 RETURNING 子句，或用户提供了涉及该关系
	 * 各列的 WHERE 子句），我们收集 CMD_SELECT 策略，并先通过
	 * add_security_quals
	 * 加入它们。
	 *
	 * 这样，我们滤掉通过 ALL 或 SELECT USING 策略
	 * 不可见的任何记录。
	 */
	if ((commandType == CMD_UPDATE || commandType == CMD_DELETE ||
		 commandType == CMD_MERGE) &&
		perminfo->requiredPerms & ACL_SELECT)
	{
		List	   *select_permissive_policies;
		List	   *select_restrictive_policies;

		get_policies_for_relation(rel, CMD_SELECT, user_id,
								  &select_permissive_policies,
								  &select_restrictive_policies);

		add_security_quals(rt_index,
						   select_permissive_policies,
						   select_restrictive_policies,
						   securityQuals,
						   hasSubLinks);
	}

	/*
	 * 对 INSERT 和 UPDATE，加入 withCheckOptions，以验证任何新
	 * 加入的记录与安全性策略一致。这将使用
	 * 每条策略的 WITH CHECK 子句，若没有显式
	 * WITH CHECK 子句，则使用它的 USING 子句。
	 */
	if (commandType == CMD_INSERT || commandType == CMD_UPDATE)
	{
		/* 这应当是目标关系 */
		Assert(rt_index == root->resultRelation);

		add_with_check_options(rel, rt_index,
							   commandType == CMD_INSERT ?
							   WCO_RLS_INSERT_CHECK : WCO_RLS_UPDATE_CHECK,
							   permissive_policies,
							   restrictive_policies,
							   withCheckOptions,
							   hasSubLinks,
							   false);

		/*
		 * 若此关系需要 SELECT 权限（例如使用了 RETURNING），
		 * 取得并加入 ALL/SELECT 策略。它们作为 WCO
		 * 策略而不是安全性限定条件加入，以确保策略被违反时
		 * 会报错；否则，我们可能最终静默
		 * 丢掉要加入的行。
		 */
		if (perminfo->requiredPerms & ACL_SELECT)
		{
			List	   *select_permissive_policies = NIL;
			List	   *select_restrictive_policies = NIL;

			get_policies_for_relation(rel, CMD_SELECT, user_id,
									  &select_permissive_policies,
									  &select_restrictive_policies);
			add_with_check_options(rel, rt_index,
								   commandType == CMD_INSERT ?
								   WCO_RLS_INSERT_CHECK : WCO_RLS_UPDATE_CHECK,
								   select_permissive_policies,
								   select_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks,
								   true);
		}

		/*
		 * 对 INSERT ... ON CONFLICT DO UPDATE，我们需要对可能应用到同一 RTE 的
		 * UPDATE 做额外的策略检查。
		 */
		if (commandType == CMD_INSERT &&
			root->onConflict && root->onConflict->action == ONCONFLICT_UPDATE)
		{
			List	   *conflict_permissive_policies;
			List	   *conflict_restrictive_policies;
			List	   *conflict_select_permissive_policies = NIL;
			List	   *conflict_select_restrictive_policies = NIL;

			/* 取得应用到辅助 UPDATE 的策略 */
			get_policies_for_relation(rel, CMD_UPDATE, user_id,
									  &conflict_permissive_policies,
									  &conflict_restrictive_policies);

			/*
			 * 用 WCOs 而不是安全性限定条件来强制 UPDATE 策略的 USING 子句。
			 * 这确保若冲突行因行安全性而无法被更新，
			 * 就会报错，
			 * 而不是静默丢掉这次改动。
			 */
			add_with_check_options(rel, rt_index,
								   WCO_RLS_CONFLICT_CHECK,
								   conflict_permissive_policies,
								   conflict_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks,
								   true);

			/*
			 * 取得并加入 ALL/SELECT 策略，作为 WCO_RLS_CONFLICT_CHECK WCOs，
			 * 以确保走 INSERT .. ON CONFLICT DO UPDATE 的 UPDATE 路径时
			 * 会考虑它们，若此关系需要 SELECT 权限，同样作为 WCO 策略，
			 * 同样是为了避免静默丢掉数据。
			 * 见上文。
			 */
			if (perminfo->requiredPerms & ACL_SELECT)
			{
				get_policies_for_relation(rel, CMD_SELECT, user_id,
										  &conflict_select_permissive_policies,
										  &conflict_select_restrictive_policies);
				add_with_check_options(rel, rt_index,
									   WCO_RLS_CONFLICT_CHECK,
									   conflict_select_permissive_policies,
									   conflict_select_restrictive_policies,
									   withCheckOptions,
									   hasSubLinks,
									   true);
			}

			/* 强制 UPDATE 策略的 WITH CHECK 子句 */
			add_with_check_options(rel, rt_index,
								   WCO_RLS_UPDATE_CHECK,
								   conflict_permissive_policies,
								   conflict_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks,
								   false);

			/*
			 * 把 ALL/SELECT 策略作为 WCO_RLS_UPDATE_CHECK WCOs 加入，以确保
			 * 走 INSERT .. ON CONFLICT DO UPDATE 的 UPDATE 路径时，
			 * 最终更新后的行可见，前提是此关系
			 * 需要 SELECT 权限。
			 */
			if (perminfo->requiredPerms & ACL_SELECT)
				add_with_check_options(rel, rt_index,
									   WCO_RLS_UPDATE_CHECK,
									   conflict_select_permissive_policies,
									   conflict_select_restrictive_policies,
									   withCheckOptions,
									   hasSubLinks,
									   true);
		}
	}

	/*
	 * 对 MERGE，我们取得 UPDATE、DELETE 和 INSERT（以及 ALL）的策略，
	 * 并设置它们，以便按我们最终采取的动作
	 * 强制相应的策略。
	 *
	 * 上面我们已经取得了 SELECT 策略来检查已有行，
	 * 但若此关系需要 SELECT 权限，我们还必须检查 UPDATE 动作
	 * 创建的新行是否可见。对 INSERT 动作我们不这样做，
	 * 因为 INSERT 命令只有在有 RETURNING 列表时才会做这项
	 * 检查，而 MERGE 不支持 RETURNING。
	 *
	 * 我们不把 UPDATE/DELETE 的 USING 限定条件下推到 RTE，
	 * 因为我们并不真想在扫描该关系时应用它们，
	 * 因为我们不知道最后会做 UPDATE 还是 DELETE。
	 * 一旦决定对目标元组的最终动作，
	 * 我们就应用相应的策略。
	 *
	 * XXX 我们把 USING 限定条件设置成 WITH CHECK。若行安全性禁止
	 * 对目标行做 UPDATE/DELETE，我们会报错，而不是
	 * 静默忽略该行。这与普通
	 * UPDATE/DELETE 的做法不同，更接近 INSERT ON CONFLICT DO UPDATE
	 * 的处理。
	 */
	if (commandType == CMD_MERGE)
	{
		List	   *merge_update_permissive_policies;
		List	   *merge_update_restrictive_policies;
		List	   *merge_delete_permissive_policies;
		List	   *merge_delete_restrictive_policies;
		List	   *merge_insert_permissive_policies;
		List	   *merge_insert_restrictive_policies;

		/*
		 * 取得 UPDATE 策略，并设置它们在做 UPDATE 之前
		 * 对已有目标行执行。
		 */
		get_policies_for_relation(rel, CMD_UPDATE, user_id,
								  &merge_update_permissive_policies,
								  &merge_update_restrictive_policies);

		/*
		 * WCO_RLS_MERGE_UPDATE_CHECK 用来检查已有目标行上的
		 * UPDATE USING 限定条件。
		 */
		add_with_check_options(rel, rt_index,
							   WCO_RLS_MERGE_UPDATE_CHECK,
							   merge_update_permissive_policies,
							   merge_update_restrictive_policies,
							   withCheckOptions,
							   hasSubLinks,
							   true);

		/* 强制 UPDATE 策略的 WITH CHECK 子句 */
		add_with_check_options(rel, rt_index,
							   WCO_RLS_UPDATE_CHECK,
							   merge_update_permissive_policies,
							   merge_update_restrictive_policies,
							   withCheckOptions,
							   hasSubLinks,
							   false);

		/*
		 * 把 ALL/SELECT 策略作为 WCO_RLS_UPDATE_CHECK WCOs 加入，以确保
		 * 执行 UPDATE 动作时更新后的行可见，前提是
		 * 此关系需要 SELECT 权限。
		 */
		if (perminfo->requiredPerms & ACL_SELECT)
		{
			List	   *merge_select_permissive_policies;
			List	   *merge_select_restrictive_policies;

			get_policies_for_relation(rel, CMD_SELECT, user_id,
									  &merge_select_permissive_policies,
									  &merge_select_restrictive_policies);
			add_with_check_options(rel, rt_index,
								   WCO_RLS_UPDATE_CHECK,
								   merge_select_permissive_policies,
								   merge_select_restrictive_policies,
								   withCheckOptions,
								   hasSubLinks,
								   true);
		}

		/*
		 * 取得 DELETE 策略，并设置它们在做 DELETE 之前
		 * 对已有目标行执行。
		 */
		get_policies_for_relation(rel, CMD_DELETE, user_id,
								  &merge_delete_permissive_policies,
								  &merge_delete_restrictive_policies);

		/*
		 * WCO_RLS_MERGE_DELETE_CHECK 用来检查已有目标行上的
		 * DELETE USING 限定条件。
		 */
		add_with_check_options(rel, rt_index,
							   WCO_RLS_MERGE_DELETE_CHECK,
							   merge_delete_permissive_policies,
							   merge_delete_restrictive_policies,
							   withCheckOptions,
							   hasSubLinks,
							   true);

		/*
		 * INSERT 策略不需要特殊处理。它们会在
		 * ExecInsert() 期间被检查并强制执行。但我们必须把它们加入
		 * withCheckOptions。
		 */
		get_policies_for_relation(rel, CMD_INSERT, user_id,
								  &merge_insert_permissive_policies,
								  &merge_insert_restrictive_policies);

		add_with_check_options(rel, rt_index,
							   WCO_RLS_INSERT_CHECK,
							   merge_insert_permissive_policies,
							   merge_insert_restrictive_policies,
							   withCheckOptions,
							   hasSubLinks,
							   false);
	}

	table_close(rel, NoLock);

	/*
	 * 把 checkAsUser 复制到行安全性限定条件和 WithCheckOption 检查里，
	 * 以防它们含有引用其他关系的子查询。
	 */
	setRuleCheckAsUser((Node *) *securityQuals, perminfo->checkAsUser);
	setRuleCheckAsUser((Node *) *withCheckOptions, perminfo->checkAsUser);

	/*
	 * 把此查询标为具有行安全性，以便 plancache 在必要时
	 * 使它失效（例如角色变化）
	 */
	*hasRowSecurity = true;
}

/*
 * get_policies_for_relation
 *
 * 返回要应用到指定关系的宽松策略和限制性策略清单，
 * 依据是命令类型和角色。
 *
 * 这包括扩展加入的任何策略。
 */
static void
get_policies_for_relation(Relation relation, CmdType cmd, Oid user_id,
						  List **permissive_policies,
						  List **restrictive_policies)
{
	ListCell   *item;

	*permissive_policies = NIL;
	*restrictive_policies = NIL;

	/* 先找出该关系的全部内部策略。 */
	foreach(item, relation->rd_rsdesc->policies)
	{
		bool		cmd_matches = false;
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

		/* 若存在 ALL 策略，始终加入它们。 */
		if (policy->polcmd == '*')
			cmd_matches = true;
		else
		{
			/* 检查该策略是否适用于指定的命令类型 */
			switch (cmd)
			{
				case CMD_SELECT:
					if (policy->polcmd == ACL_SELECT_CHR)
						cmd_matches = true;
					break;
				case CMD_INSERT:
					if (policy->polcmd == ACL_INSERT_CHR)
						cmd_matches = true;
					break;
				case CMD_UPDATE:
					if (policy->polcmd == ACL_UPDATE_CHR)
						cmd_matches = true;
					break;
				case CMD_DELETE:
					if (policy->polcmd == ACL_DELETE_CHR)
						cmd_matches = true;
					break;
				case CMD_MERGE:

					/*
					 * 我们不支持为 MERGE 命令单独设策略。
					 * 它改为从为其他
					 * 命令定义的策略派生。
					 */
					break;
				default:
					elog(ERROR, "unrecognized policy command type %d",
						 (int) cmd);
					break;
			}
		}

		/*
		 * 若该策略适用于指定角色，
		 * 就把它加入相应的策略清单。
		 */
		if (cmd_matches && check_role_for_policy(policy->roles, user_id))
		{
			if (policy->permissive)
				*permissive_policies = lappend(*permissive_policies, policy);
			else
				*restrictive_policies = lappend(*restrictive_policies, policy);
		}
	}

	/*
	 * 我们按名称对限制性策略排序，使它们生成的任何 WCOs 按
	 * 明确的顺序被检查。
	 */
	sort_policies_by_name(*restrictive_policies);

	/*
	 * 然后加入扩展定义的任何宽松或限制性策略。
	 * 若它们适用于指定角色，就直接追加到
	 * 内部策略清单。
	 */
	if (row_security_policy_hook_restrictive)
	{
		List	   *hook_policies =
			(*row_security_policy_hook_restrictive) (cmd, relation);

		/*
		 * 与内建限制性策略一样，我们也按名称对钩子提供的
		 * 限制性策略排序。注意，我们也有意
		 * 始终先按名称顺序检查全部内建限制性策略，
		 * 再按名称顺序检查钩子加入的限制性策略。
		 */
		sort_policies_by_name(hook_policies);

		foreach(item, hook_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

			if (check_role_for_policy(policy->roles, user_id))
				*restrictive_policies = lappend(*restrictive_policies, policy);
		}
	}

	if (row_security_policy_hook_permissive)
	{
		List	   *hook_policies =
			(*row_security_policy_hook_permissive) (cmd, relation);

		foreach(item, hook_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

			if (check_role_for_policy(policy->roles, user_id))
				*permissive_policies = lappend(*permissive_policies, policy);
		}
	}
}

/*
 * sort_policies_by_name
 *
 * 这只用于限制性策略，确保它们生成的任何
 * WithCheckOptions 按明确的顺序应用。
 * 对宽松策略没有必要，因为它们全部用 OR
 * 组合成一次 WithCheckOption 检查。
 */
static void
sort_policies_by_name(List *policies)
{
	list_sort(policies, row_security_policy_cmp);
}

/*
 * 按名称对 RowSecurityPolicy 项排序的 list_sort 比较器
 */
static int
row_security_policy_cmp(const ListCell *a, const ListCell *b)
{
	const RowSecurityPolicy *pa = (const RowSecurityPolicy *) lfirst(a);
	const RowSecurityPolicy *pb = (const RowSecurityPolicy *) lfirst(b);

	/* 防范扩展给出的 NULL 策略名 */
	if (pa->policy_name == NULL)
		return pb->policy_name == NULL ? 0 : 1;
	if (pb->policy_name == NULL)
		return -1;

	return strcmp(pa->policy_name, pb->policy_name);
}

/*
 * add_security_quals
 *
 * 加入安全性限定条件，以强制指定的行安全性策略，限制
 * 对表中已有数据的访问。若没有策略控制
 * 对该表的访问，则禁止全部访问，即使用隐式的
 * 默认拒绝策略。
 *
 * 新的安全性限定条件被加入 securityQuals，若加入的任何限定条件
 * 含有子链接子查询，就把 hasSubLinks 设为真。
 */
static void
add_security_quals(int rt_index,
				   List *permissive_policies,
				   List *restrictive_policies,
				   List **securityQuals,
				   bool *hasSubLinks)
{
	ListCell   *item;
	List	   *permissive_quals = NIL;
	Expr	   *rowsec_expr;

	/*
	 * 先收集宽松限定条件。若找不到任何
	 * 宽松策略，就没有任何行可见（这在下面处理）。
	 */
	foreach(item, permissive_policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);

		if (policy->qual != NULL)
		{
			permissive_quals = lappend(permissive_quals,
									   copyObject(policy->qual));
			*hasSubLinks |= policy->hassublinks;
		}
	}

	/*
	 * 我们必须始终有宽松限定条件，否则没有任何行可见。
	 *
	 * 若没有，我们就只返回一条结果为假的限定条件，
	 * 从而没有任何行可见。
	 */
	if (permissive_quals != NIL)
	{
		/*
		 * 现在我们知道存在宽松策略，因此可以加入
		 * 基于限制性策略 USING 子句的安全性限定条件。
		 * 由于它们需要用 AND 组合在一起，我们可以
		 * 一次加一条。
		 */
		foreach(item, restrictive_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
			Expr	   *qual;

			if (policy->qual != NULL)
			{
				qual = copyObject(policy->qual);
				ChangeVarNodes((Node *) qual, 1, rt_index, 0);

				*securityQuals = list_append_unique(*securityQuals, qual);
				*hasSubLinks |= policy->hassublinks;
			}
		}

		/*
		 * 然后加入一条安全性限定条件，把所有宽松策略的 USING
		 * 子句用 OR 组合在一起。
		 */
		if (list_length(permissive_quals) == 1)
			rowsec_expr = (Expr *) linitial(permissive_quals);
		else
			rowsec_expr = makeBoolExpr(OR_EXPR, permissive_quals, -1);

		ChangeVarNodes((Node *) rowsec_expr, 1, rt_index, 0);
		*securityQuals = list_append_unique(*securityQuals, rowsec_expr);
	}
	else

		/*
		 * 必须存在宽松策略，行才可能可见。
		 * 因此，若没有找到宽松策略，就返回一条
		 * 始终为假的子句。
		 */
		*securityQuals = lappend(*securityQuals,
								 makeConst(BOOLOID, -1, InvalidOid,
										   sizeof(bool), BoolGetDatum(false),
										   false, true));
}

/*
 * add_with_check_options
 *
 * 加入指定种类的 WithCheckOptions，以检查 INSERT 或 UPDATE
 * 加入的新记录与指定的行安全性
 * 策略一致。通常新数据必须满足策略的 WITH CHECK 子句。
 * 若策略没有显式 WITH CHECK 子句，就改用它的 USING 子句。
 * 在由 INSERT ... ON CONFLICT DO UPDATE 产生的
 * UPDATE 这一特殊情形里，已有记录先用
 * WCO_RLS_CONFLICT_CHECK WithCheckOption 检查，它始终使用
 * 行安全性策略的 USING 子句。
 *
 * 新的 WCOs 被加入 withCheckOptions，若加入的任何检查子句
 * 含有子链接子查询，就把 hasSubLinks 设为真。
 */
static void
add_with_check_options(Relation rel,
					   int rt_index,
					   WCOKind kind,
					   List *permissive_policies,
					   List *restrictive_policies,
					   List **withCheckOptions,
					   bool *hasSubLinks,
					   bool force_using)
{
	ListCell   *item;
	List	   *permissive_quals = NIL;

#define QUAL_FOR_WCO(policy) \
	( !force_using && \
	  (policy)->with_check_qual != NULL ? \
	  (policy)->with_check_qual : (policy)->qual )

	/*
	 * 先收集宽松策略子句，类似于
	 * add_security_quals。
	 */
	foreach(item, permissive_policies)
	{
		RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
		Expr	   *qual = QUAL_FOR_WCO(policy);

		if (qual != NULL)
		{
			permissive_quals = lappend(permissive_quals, copyObject(qual));
			*hasSubLinks |= policy->hassublinks;
		}
	}

	/*
	 * There must be at least one permissive qual found or no rows are allowed
	 * to be added.  This is the same as in add_security_quals.
	 *
	 * If there are no permissive_quals then we fall through and return a
	 * single 'false' WCO, preventing all new rows.
	 */
	if (permissive_quals != NIL)
	{
		/*
		 * 为全部宽松策略子句加入一条 WithCheckOption，
		 * 用 OR 把它们组合起来。这次检查没有策略名，
		 * 因为检查失败意味着没有任何策略授予执行
		 * 更新的权限，而不是某条特定策略被
		 * 违反。
		 */
		WithCheckOption *wco;

		wco = makeNode(WithCheckOption);
		wco->kind = kind;
		wco->relname = pstrdup(RelationGetRelationName(rel));
		wco->polname = NULL;
		wco->cascaded = false;

		if (list_length(permissive_quals) == 1)
			wco->qual = (Node *) linitial(permissive_quals);
		else
			wco->qual = (Node *) makeBoolExpr(OR_EXPR, permissive_quals, -1);

		ChangeVarNodes(wco->qual, 1, rt_index, 0);

		*withCheckOptions = list_append_unique(*withCheckOptions, wco);

		/*
		 * 现在为每条限制性策略子句加入 WithCheckOptions
		 * （它们将用 AND 组合在一起）。我们为每条限制性策略使用单独的
		 * WithCheckOption，以便策略被违反时能把策略
		 * 名放进错误报告。
		 */
		foreach(item, restrictive_policies)
		{
			RowSecurityPolicy *policy = (RowSecurityPolicy *) lfirst(item);
			Expr	   *qual = QUAL_FOR_WCO(policy);

			if (qual != NULL)
			{
				qual = copyObject(qual);
				ChangeVarNodes((Node *) qual, 1, rt_index, 0);

				wco = makeNode(WithCheckOption);
				wco->kind = kind;
				wco->relname = pstrdup(RelationGetRelationName(rel));
				wco->polname = pstrdup(policy->policy_name);
				wco->qual = (Node *) qual;
				wco->cascaded = false;

				*withCheckOptions = list_append_unique(*withCheckOptions, wco);
				*hasSubLinks |= policy->hassublinks;
			}
		}
	}
	else
	{
		/*
		 * 若没有用来检查新数据的策略子句，就加入一条
		 * 始终为假的 WCO（默认拒绝策略）。
		 */
		WithCheckOption *wco;

		wco = makeNode(WithCheckOption);
		wco->kind = kind;
		wco->relname = pstrdup(RelationGetRelationName(rel));
		wco->polname = NULL;
		wco->qual = (Node *) makeConst(BOOLOID, -1, InvalidOid,
									   sizeof(bool), BoolGetDatum(false),
									   false, true);
		wco->cascaded = false;

		*withCheckOptions = lappend(*withCheckOptions, wco);
	}
}

/*
 * check_role_for_policy -
 *	 判断当前角色是否应当应用该策略
 */
static bool
check_role_for_policy(ArrayType *policy_roles, Oid user_id)
{
	int			i;
	Oid		   *roles = (Oid *) ARR_DATA_PTR(policy_roles);

	/* Quick fall-thru for policies applied to all roles */
	if (roles[0] == ACL_ID_PUBLIC)
		return true;

	for (i = 0; i < ARR_DIMS(policy_roles)[0]; i++)
	{
		if (has_privs_of_role(user_id, roles[i]))
			return true;
	}

	return false;
}
