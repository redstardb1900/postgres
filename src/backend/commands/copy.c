/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteHandler.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"

/*
 *	 DoCopy 执行 SQL COPY 语句         
 *
 * 卸载或重载表 <relation> 的内容，取决于 <from>。             
 * （<from> = true 表示我们正在向表中插入数据。）在       
 * "TO" 情形下，我们还支持复制任意 SELECT、INSERT、UPDATE    
 * 或 DELETE。   
 *
 * 如果 <pipe> 为 false，传输就在表与名为                  
 * <filename> 的文件之间。否则，传输发生在表与我们的 
 * 常规输入/输出流之间，后者可为 stdin/stdout 或套  
 * 接字，取决于我们是否在 Postmaster 控制下运行。     
 *
 * 不允许没有 'pg_read_server_files' 或                        
 * 'pg_write_server_files' 角色的用户读写文件。       
 *
 * 如果用户没有访问该表或所请求的特定列的适当权    
 * 限，则不允许进行复制。               
 */
void
DoCopy(ParseState *pstate, const CopyStmt *stmt,
	   int stmt_location, int stmt_len,
	   uint64 *processed)
{
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	Relation	rel;
	Oid			relid;
	RawStmt    *query = NULL;
	Node	   *whereClause = NULL;

	/*
		 * 禁止没有相应角色的用户使用 COPY 读写文件或程序。
		 *
		 */
	if (!pipe)
	{
		if (stmt->is_program)
		{
			if (!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY to or from an external program"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY to or from an external program.",
								   "pg_execute_server_program"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));
		}
		else
		{
			if (is_from && !has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY from a file"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY from a file.",
								   "pg_read_server_files"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));

			if (!is_from && !has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY to a file"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY to a file.",
								   "pg_write_server_files"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));
		}
	}

	if (stmt->relation)
	{
		LOCKMODE	lockmode = is_from ? RowExclusiveLock : AccessShareLock;
		ParseNamespaceItem *nsitem;
		RTEPermissionInfo *perminfo;
		TupleDesc	tupDesc;
		List	   *attnums;
		ListCell   *cur;

		Assert(!stmt->query);

		/* 使用适当的锁类型打开并锁定该关系。 */
		rel = table_openrv(stmt->relation, lockmode);

		relid = RelationGetRelid(rel);

		nsitem = addRangeTableEntryForRelation(pstate, rel, lockmode,
											   NULL, false, false);

		perminfo = nsitem->p_perminfo;
		perminfo->requiredPerms = (is_from ? ACL_INSERT : ACL_SELECT);

		if (stmt->whereClause)
		{
			Bitmapset  *expr_attrs = NULL;
			int			i;

			/* 将 nsitem 添加到查询命名空间 */
			addNSItemToQuery(pstate, nsitem, false, true, true);

			/* 转换原始表达式树 */
			whereClause = transformExpr(pstate, stmt->whereClause, EXPR_KIND_COPY_WHERE);

			/* 确保它产生布尔结果。 */
			whereClause = coerce_to_boolean(pstate, whereClause, "WHERE");

			/* 我们也必须修正其排序规则 */
			assign_expr_collations(pstate, whereClause);

			/*
			 * 检查 WHERE 子句表达式中的所有列。当            
			 * 存在整行引用时，检查该表中的所有列。        
			 * 。       
			 */
			pull_varattnos(whereClause, 1, &expr_attrs);
			if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, expr_attrs))
			{
				expr_attrs = bms_add_range(expr_attrs,
										   1 - FirstLowInvalidHeapAttributeNumber,
										   RelationGetNumberOfAttributes(rel) - FirstLowInvalidHeapAttributeNumber);
				expr_attrs = bms_del_member(expr_attrs, 0 - FirstLowInvalidHeapAttributeNumber);
			}

			i = -1;
			while ((i = bms_next_member(expr_attrs, i)) >= 0)
			{
				AttrNumber	attno = i + FirstLowInvalidHeapAttributeNumber;

				Assert(attno != 0);

				/*
				 * 禁止在 WHERE 子句中使用生成列。存储型   
				 * 生成列在进行过滤时尚未计算。               
				 * 。     
				 */
				if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->attgenerated)
					ereport(ERROR,
							errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
							errmsg("generated columns are not supported in COPY FROM WHERE conditions"),
							errdetail("Column \"%s\" is a generated column.",
									  get_attname(RelationGetRelid(rel), attno, false)));
			}

			whereClause = eval_const_expressions(NULL, whereClause);

			whereClause = (Node *) canonicalize_qual((Expr *) whereClause, false);
			whereClause = (Node *) make_ands_implicit((Expr *) whereClause);
		}

		tupDesc = RelationGetDescr(rel);
		attnums = CopyGetAttnums(tupDesc, rel, stmt->attlist);
		foreach(cur, attnums)
		{
			int			attno;
			Bitmapset **bms;

			attno = lfirst_int(cur) - FirstLowInvalidHeapAttributeNumber;
			bms = is_from ? &perminfo->insertedCols : &perminfo->selectedCols;

			*bms = bms_add_member(*bms, attno);
		}
		ExecCheckPermissions(pstate->p_rtable, list_make1(perminfo), true);

		/*
		 * Permission check for row security policies.
		 *
		 * check_enable_rls will ereport(ERROR) if the user has requested
		 * something invalid and will otherwise indicate if we should enable
		 * RLS (returns RLS_ENABLED) or not for this COPY statement.
		 *
		 * If the relation has a row security policy and we are to apply it
		 * then perform a "query" copy and allow the normal query processing
		 * to handle the policies.
		 *
		 * If RLS is not enabled for this, then just fall through to the
		 * normal non-filtering relation handling.
		 */
		if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		{
			SelectStmt *select;
			ColumnRef  *cr;
			ResTarget  *target;
			RangeVar   *from;
			List	   *targetList = NIL;

			if (is_from)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("COPY FROM not supported with row-level security"),
						 errhint("Use INSERT statements instead.")));

			/*
			 * 建目标列表  
			 *
			 * 如果 COPY 命令属性列表中未指定任何列，则    
			 * 目标列表就是"所有"列。因此，结果 SELECT 语句 
			 * 应使用 '*' 作为目标列表。                       
			 *           
			 *
			 * 如果属性列表中指定了列，则为每列创建一个 
			 * ColumnRef 和 ResTarget，并把它们加入最终 SELECT    
			 * 语句的目标列表。                              
			 */
			if (!stmt->attlist)
			{
				cr = makeNode(ColumnRef);
				cr->fields = list_make1(makeNode(A_Star));
				cr->location = -1;

				target = makeNode(ResTarget);
				target->name = NULL;
				target->indirection = NIL;
				target->val = (Node *) cr;
				target->location = -1;

				targetList = list_make1(target);
			}
			else
			{
				ListCell   *lc;

				foreach(lc, stmt->attlist)
				{
					/*
						 * 为每列构建 ColumnRef。ColumnRef 的 'fields' 属性
						 * 是分别对应列名的 String 节点。
						 *
						 */
					cr = makeNode(ColumnRef);
					cr->fields = list_make1(lfirst(lc));
					cr->location = -1;

					/* 构建 ResTarget 并将 ColumnRef 添加到其中。 */
					target = makeNode(ResTarget);
					target->name = NULL;
					target->indirection = NIL;
					target->val = (Node *) cr;
					target->location = -1;

					/* 将每列添加到 SELECT 语句的目标列表中 */
					targetList = lappend(targetList, target);
				}
			}

			/*
				 * 为 from 子句构建 RangeVar，基于我们已打开并锁定的关系
				 * 进行完全限定。使用 "ONLY" 使得 COPY 仅从目标表中检索行，
				 * 而不是任何继承子表，这与不应用 RLS 时相同。
				 *
				 */
			from = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
								pstrdup(RelationGetRelationName(rel)),
								-1);
			from->inh = false;	/* 应用 ONLY */

			/* 构建查询 */
			select = makeNode(SelectStmt);
			select->targetList = targetList;
			select->fromClause = list_make1(from);

			query = makeNode(RawStmt);
			query->stmt = (Node *) select;
			query->stmt_location = stmt_location;
			query->stmt_len = stmt_len;

			/*
				 * 现在关闭关系，但保留锁以防止从现在到我们启动基于查询的
				 * COPY 之间发生更改。
				 *
				 * 之后我们将作为基于查询的 COPY 的一部分重新打开它。
				 */
			table_close(rel, NoLock);
			rel = NULL;
		}
	}
	else
	{
		Assert(stmt->query);

		/* MERGE 被解析器允许，但尚未实现。目前拒绝 */
		if (IsA(stmt->query, MergeStmt))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("MERGE not supported in COPY"));

		query = makeNode(RawStmt);
		query->stmt = stmt->query;
		query->stmt_location = stmt_location;
		query->stmt_len = stmt_len;

		relid = InvalidOid;
		rel = NULL;
	}

	if (is_from)
	{
		CopyFromState cstate;

		Assert(rel);

		/* 检查只读事务和并行模式 */
		if (XactReadOnly && !rel->rd_islocaltemp)
			PreventCommandIfReadOnly("COPY FROM");

		cstate = BeginCopyFrom(pstate, rel, whereClause,
							   stmt->filename, stmt->is_program,
							   NULL, stmt->attlist, stmt->options);
		*processed = CopyFrom(cstate);	/* 从文件复制到数据库 */
		EndCopyFrom(cstate);
	}
	else
	{
		CopyToState cstate;

		cstate = BeginCopyTo(pstate, rel, query, relid,
							 stmt->filename, stmt->is_program,
							 NULL, stmt->attlist, stmt->options);
		*processed = DoCopyTo(cstate);	/* 从数据库复制到文件 */
		EndCopyTo(cstate);
	}

	if (rel != NULL)
		table_close(rel, NoLock);
}

/*
 * 从 DefElem 中提取 CopyHeaderChoice 值。这类似于
 * defGetBoolean()，但也接受特殊值 "match"。
 */
static CopyHeaderChoice
defGetCopyHeaderChoice(DefElem *def, bool is_from)
{
	/*
	 * 如果未给定参数值，则假定为 "true"。
	 */
	if (def->arg == NULL)
		return COPY_HEADER_TRUE;

	/*
	 * 允许 0、1、"true"、"false"、"on"、"off" 或 "match"。
	 */
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			switch (intVal(def->arg))
			{
				case 0:
					return COPY_HEADER_FALSE;
				case 1:
					return COPY_HEADER_TRUE;
				default:
					/* 否则，在下面报错 */
					break;
			}
			break;
		default:
			{
				char	   *sval = defGetString(def);

				/*
				 * 此处接受的字符串集合应与语法的 opt_boolean_or_string
				 * 产生式相匹配。
				 */
				if (pg_strcasecmp(sval, "true") == 0)
					return COPY_HEADER_TRUE;
				if (pg_strcasecmp(sval, "false") == 0)
					return COPY_HEADER_FALSE;
				if (pg_strcasecmp(sval, "on") == 0)
					return COPY_HEADER_TRUE;
				if (pg_strcasecmp(sval, "off") == 0)
					return COPY_HEADER_FALSE;
				if (pg_strcasecmp(sval, "match") == 0)
				{
					if (!is_from)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot use \"%s\" with HEADER in COPY TO",
										sval)));
					return COPY_HEADER_MATCH;
				}
			}
			break;
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s requires a Boolean value or \"match\"",
					def->defname)));
	return COPY_HEADER_FALSE;	/* 让编译器保持安静 */
}

/*
 * 处理 COPY 的语句选项列表。
 *
 * 扫描选项列表（DefElem 列表）并将信息转换到 *opts_out 中，
 * 同时应用适当的错误检查。
 *
 * 如果 'opts_out' 不为 NULL，则假定它最初已用零填充。
 *
 * 此函数被导出，以便 COPY API 的外部用户可以验证选项列表
 * 的合理性。在这种用法中，'opts_out' 可以传递为 NULL，
 * 收集的数据将泄漏直到 CurrentMemoryContext 被重置。
 *
 * 注意，额外的检查（例如 FORCE QUOTE 中列出的列名是否实际存在）
 * 必须在之后应用。这里只检查选项列表的自洽性。
 *
 */
void
ProcessCopyOptions(ParseState *pstate,
				   CopyFormatOptions *opts_out,
				   bool is_from,
				   List *options)
{
	bool		format_specified = false;
	bool		freeze_specified = false;
	bool		header_specified = false;
	ListCell   *option;

	/* 支持外部使用选项合理性检查 */
	if (opts_out == NULL)
		opts_out = (CopyFormatOptions *) palloc0(sizeof(CopyFormatOptions));

	opts_out->file_encoding = -1;

	/* 从语句节点树中提取选项 */
	foreach(option, options)
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "format") == 0)
		{
			char	   *fmt = defGetString(defel);

			if (format_specified)
				errorConflictingDefElem(defel, pstate);
			format_specified = true;
			if (strcmp(fmt, "text") == 0)
				 /* 默认格式 */ ;
			else if (strcmp(fmt, "csv") == 0)
				opts_out->csv_mode = true;
			else if (strcmp(fmt, "binary") == 0)
				opts_out->binary = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("COPY format \"%s\" not recognized", fmt),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "freeze") == 0)
		{
			if (freeze_specified)
				errorConflictingDefElem(defel, pstate);
			freeze_specified = true;
			opts_out->freeze = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (opts_out->delim)
				errorConflictingDefElem(defel, pstate);
			opts_out->delim = defGetString(defel);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (opts_out->null_print)
				errorConflictingDefElem(defel, pstate);
			opts_out->null_print = defGetString(defel);
		}
		else if (strcmp(defel->defname, "default") == 0)
		{
			if (opts_out->default_print)
				errorConflictingDefElem(defel, pstate);
			opts_out->default_print = defGetString(defel);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (header_specified)
				errorConflictingDefElem(defel, pstate);
			header_specified = true;
			opts_out->header_line = defGetCopyHeaderChoice(defel, is_from);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (opts_out->quote)
				errorConflictingDefElem(defel, pstate);
			opts_out->quote = defGetString(defel);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (opts_out->escape)
				errorConflictingDefElem(defel, pstate);
			opts_out->escape = defGetString(defel);
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			if (opts_out->force_quote || opts_out->force_quote_all)
				errorConflictingDefElem(defel, pstate);
			if (defel->arg && IsA(defel->arg, A_Star))
				opts_out->force_quote_all = true;
			else if (defel->arg && IsA(defel->arg, List))
				opts_out->force_quote = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "force_not_null") == 0)
		{
			if (opts_out->force_notnull)
				errorConflictingDefElem(defel, pstate);
			if (defel->arg && IsA(defel->arg, List))
				opts_out->force_notnull = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "force_null") == 0)
		{
			if (opts_out->force_null)
				errorConflictingDefElem(defel, pstate);
			if (defel->arg && IsA(defel->arg, List))
				opts_out->force_null = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "convert_selectively") == 0)
		{
			/*
				 * 未记录的、SQL 不可访问的选项：仅将命名的列转换为二进制形式，
				 * 其余存储为 NULL。允许列列表为 NIL。
				 *
				 */
			if (opts_out->convert_selectively)
				errorConflictingDefElem(defel, pstate);
			opts_out->convert_selectively = true;
			if (defel->arg == NULL || IsA(defel->arg, List))
				opts_out->convert_select = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "encoding") == 0)
		{
			if (opts_out->file_encoding >= 0)
				errorConflictingDefElem(defel, pstate);
			opts_out->file_encoding = pg_char_to_encoding(defGetString(defel));
			if (opts_out->file_encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a valid encoding name",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
	}

	/*
		 * 检查不兼容的选项（必须在插入默认值之前执行这两个检查）
		 *
		 */
	if (opts_out->binary && opts_out->delim)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify DELIMITER in BINARY mode")));

	if (opts_out->binary && opts_out->null_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify NULL in BINARY mode")));

	if (opts_out->binary && opts_out->default_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify DEFAULT in BINARY mode")));

	/* 为省略的选项设置默认值 */
	if (!opts_out->delim)
		opts_out->delim = opts_out->csv_mode ? "," : "\t";

	if (!opts_out->null_print)
		opts_out->null_print = opts_out->csv_mode ? "" : "\\N";
	opts_out->null_print_len = strlen(opts_out->null_print);

	if (opts_out->csv_mode)
	{
		if (!opts_out->quote)
			opts_out->quote = "\"";
		if (!opts_out->escape)
			opts_out->escape = opts_out->quote;
	}

	/* 仅支持单字节分隔符字符串。 */
	if (strlen(opts_out->delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must be a single one-byte character")));

	/* 禁止行尾字符 */
	if (strchr(opts_out->delim, '\r') != NULL ||
		strchr(opts_out->delim, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be newline or carriage return")));

	if (strchr(opts_out->null_print, '\r') != NULL ||
		strchr(opts_out->null_print, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY null representation cannot use newline or carriage return")));

	if (opts_out->default_print)
	{
		opts_out->default_print_len = strlen(opts_out->default_print);

		if (strchr(opts_out->default_print, '\r') != NULL ||
			strchr(opts_out->default_print, '\n') != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COPY default representation cannot use newline or carriage return")));
	}

	/*
		 * 在非 CSV 模式下禁止不安全的分隔符字符。我们不能允许反斜杠，
		 * 因为它会产生歧义。我们不能允许其他情况，因为匹配分隔符的
		 * 数据字符必须加反斜杠，而某些反斜杠组合会被 COPY IN 字面解释。
		 * 禁止所有小写 ASCII 字母虽然比严格需要的多，但为了保持一致性
		 * 和面向未来性似乎是最好的。同样，我们禁止所有数字，
		 * 尽管只有八进制数字实际上是有危险的。
		 *
		 *
		 */
	if (!opts_out->csv_mode &&
		strchr("\\.abcdefghijklmnopqrstuvwxyz0123456789",
			   opts_out->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be \"%s\"", opts_out->delim)));

	/* 检查 header */
	if (opts_out->binary && opts_out->header_line)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot specify HEADER in BINARY mode")));

	/* 检查 quote */
	if (!opts_out->csv_mode && opts_out->quote != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote available only in CSV mode")));

	if (opts_out->csv_mode && strlen(opts_out->quote) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote must be a single one-byte character")));

	if (opts_out->csv_mode && opts_out->delim[0] == opts_out->quote[0])
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter and quote must be different")));

	/* 检查 escape */
	if (!opts_out->csv_mode && opts_out->escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape available only in CSV mode")));

	if (opts_out->csv_mode && strlen(opts_out->escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape must be a single one-byte character")));

	/* 检查 force_quote */
	if (!opts_out->csv_mode && (opts_out->force_quote || opts_out->force_quote_all))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote available only in CSV mode")));
	if ((opts_out->force_quote || opts_out->force_quote_all) && is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force quote only available using COPY TO")));

	/* 检查 force_notnull */
	if (!opts_out->csv_mode && opts_out->force_notnull != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force not null available only in CSV mode")));
	if (opts_out->force_notnull != NIL && !is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force not null only available using COPY FROM")));

	/* 检查 force_null */
	if (!opts_out->csv_mode && opts_out->force_null != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force null available only in CSV mode")));

	if (opts_out->force_null != NIL && !is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY force null only available using COPY FROM")));

	/* 不允许分隔符出现在 NULL 字符串中。 */
	if (strchr(opts_out->null_print, opts_out->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must not appear in the NULL specification")));

	/* 不允许 CSV 引号字符出现在 NULL 字符串中。 */
	if (opts_out->csv_mode &&
		strchr(opts_out->null_print, opts_out->quote[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSV quote character must not appear in the NULL specification")));

	if (opts_out->default_print)
	{
		if (!is_from)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY DEFAULT only available using COPY FROM")));

		/* 不允许分隔符出现在 DEFAULT 字符串中。 */
		if (strchr(opts_out->default_print, opts_out->delim[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("COPY delimiter must not appear in the DEFAULT specification")));

		/* 不允许 CSV 引号字符出现在 DEFAULT 字符串中。 */
		if (opts_out->csv_mode &&
			strchr(opts_out->default_print, opts_out->quote[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CSV quote character must not appear in the DEFAULT specification")));

		/* 不允许 NULL 和 DEFAULT 字符串相同 */
		if (opts_out->null_print_len == opts_out->default_print_len &&
			strncmp(opts_out->null_print, opts_out->default_print,
					opts_out->null_print_len) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("NULL specification and DEFAULT specification cannot be the same")));
	}
}

/*
 * CopyGetAttnums - 构建要复制的 attnum 整数列表       
 *
 * 输入的 attnamelist 要么是用户指定的列列表，     
 * 要么为 NIL（此时我们需要的是所有未删除的         
 * 列）。
 *
 * 我们不在生成的完整列表中包含生成列，也不允许        
 * 显式指定它们。它们对 COPY FROM 没有意义，但我们     
 * 或许可以允许它们用于 COPY TO。但这样至少能确           
 * 保我们复制出去的任何内容都能被复制回来。  
 *
 * rel 可以是 NULL ……它只用于错误报告。 
 */
List *
CopyGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* 生成默认列列表 */
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			if (TupleDescAttr(tupDesc, i)->attisdropped)
				continue;
			if (TupleDescAttr(tupDesc, i)->attgenerated)
				continue;
			attnums = lappend_int(attnums, i + 1);
		}
	}
	else
	{
		/* 验证用户提供的列表并提取属性号 */
		ListCell   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;
			int			i;

			/* 查找列名 */
			attnum = InvalidAttrNumber;
			for (i = 0; i < tupDesc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(tupDesc, i);

				if (att->attisdropped)
					continue;
				if (namestrcmp(&(att->attname), name) == 0)
				{
					if (att->attgenerated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
								 errmsg("column \"%s\" is a generated column",
										name),
								 errdetail("Generated columns cannot be used in COPY.")));
					attnum = att->attnum;
					break;
				}
			}
			if (attnum == InvalidAttrNumber)
			{
				if (rel != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" of relation \"%s\" does not exist",
									name, RelationGetRelationName(rel))));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									name)));
			}
			/* 检查重复项 */
			if (list_member_int(attnums, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								name)));
			attnums = lappend_int(attnums, attnum);
		}
	}

	return attnums;
}
