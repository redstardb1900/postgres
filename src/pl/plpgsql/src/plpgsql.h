/*-------------------------------------------------------------------------
 *
 * plpgsql.h：PL/pgSQL
 *	过程语言的定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/plpgsql.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PLPGSQL_H
#define PLPGSQL_H

#include "access/xact.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/expandedrecord.h"
#include "utils/typcache.h"


/**********************************************************************
 * 定义
 **********************************************************************/

/* 定义供翻译用的文本域 */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpgsql")

#undef _
#define _(x) dgettext(TEXTDOMAIN, x)

/*
 * 编译器命名空间项类型
 */
typedef enum PLpgSQL_nsitem_type
{
	PLPGSQL_NSTYPE_LABEL,		/* 块标签 */
	PLPGSQL_NSTYPE_VAR,			/* 标量变量 */
	PLPGSQL_NSTYPE_REC			/* 复合变量 */
} PLpgSQL_nsitem_type;

/*
 * PLPGSQL_NSTYPE_LABEL 栈项必须是下列类型之一
 */
typedef enum PLpgSQL_label_type
{
	PLPGSQL_LABEL_BLOCK,		/* DECLARE/BEGIN 块 */
	PLPGSQL_LABEL_LOOP,			/* 循环构造 */
	PLPGSQL_LABEL_OTHER			/* 其他任何 */
} PLpgSQL_label_type;

/*
 * Datum 数组节点类
 */
typedef enum PLpgSQL_datum_type
{
	PLPGSQL_DTYPE_VAR,
	PLPGSQL_DTYPE_ROW,
	PLPGSQL_DTYPE_REC,
	PLPGSQL_DTYPE_RECFIELD,
	PLPGSQL_DTYPE_PROMISE
} PLpgSQL_datum_type;

/*
 * DTYPE_PROMISE datum 计算承诺的可能方式
 */
typedef enum PLpgSQL_promise_type
{
	PLPGSQL_PROMISE_NONE = 0,	/* 不是承诺，或承诺已满足 */
	PLPGSQL_PROMISE_TG_NAME,
	PLPGSQL_PROMISE_TG_WHEN,
	PLPGSQL_PROMISE_TG_LEVEL,
	PLPGSQL_PROMISE_TG_OP,
	PLPGSQL_PROMISE_TG_RELID,
	PLPGSQL_PROMISE_TG_TABLE_NAME,
	PLPGSQL_PROMISE_TG_TABLE_SCHEMA,
	PLPGSQL_PROMISE_TG_NARGS,
	PLPGSQL_PROMISE_TG_ARGV,
	PLPGSQL_PROMISE_TG_EVENT,
	PLPGSQL_PROMISE_TG_TAG
} PLpgSQL_promise_type;

/*
 * PLpgSQL_type 结构里区分的变体
 */
typedef enum PLpgSQL_type_type
{
	PLPGSQL_TTYPE_SCALAR,		/* 标量类型与域 */
	PLPGSQL_TTYPE_REC,			/* 复合类型，含 RECORD */
	PLPGSQL_TTYPE_PSEUDO		/* 伪类型 */
} PLpgSQL_type_type;

/*
 * 执行树节点类型
 */
typedef enum PLpgSQL_stmt_type
{
	PLPGSQL_STMT_BLOCK,
	PLPGSQL_STMT_ASSIGN,
	PLPGSQL_STMT_IF,
	PLPGSQL_STMT_CASE,
	PLPGSQL_STMT_LOOP,
	PLPGSQL_STMT_WHILE,
	PLPGSQL_STMT_FORI,
	PLPGSQL_STMT_FORS,
	PLPGSQL_STMT_FORC,
	PLPGSQL_STMT_FOREACH_A,
	PLPGSQL_STMT_EXIT,
	PLPGSQL_STMT_RETURN,
	PLPGSQL_STMT_RETURN_NEXT,
	PLPGSQL_STMT_RETURN_QUERY,
	PLPGSQL_STMT_RAISE,
	PLPGSQL_STMT_ASSERT,
	PLPGSQL_STMT_EXECSQL,
	PLPGSQL_STMT_DYNEXECUTE,
	PLPGSQL_STMT_DYNFORS,
	PLPGSQL_STMT_GETDIAG,
	PLPGSQL_STMT_OPEN,
	PLPGSQL_STMT_FETCH,
	PLPGSQL_STMT_CLOSE,
	PLPGSQL_STMT_PERFORM,
	PLPGSQL_STMT_CALL,
	PLPGSQL_STMT_COMMIT,
	PLPGSQL_STMT_ROLLBACK
} PLpgSQL_stmt_type;

/*
 * 执行节点返回码
 */
enum
{
	PLPGSQL_RC_OK,
	PLPGSQL_RC_EXIT,
	PLPGSQL_RC_RETURN,
	PLPGSQL_RC_CONTINUE
};

/*
 * GET DIAGNOSTICS 信息项
 */
typedef enum PLpgSQL_getdiag_kind
{
	PLPGSQL_GETDIAG_ROW_COUNT,
	PLPGSQL_GETDIAG_ROUTINE_OID,
	PLPGSQL_GETDIAG_CONTEXT,
	PLPGSQL_GETDIAG_ERROR_CONTEXT,
	PLPGSQL_GETDIAG_ERROR_DETAIL,
	PLPGSQL_GETDIAG_ERROR_HINT,
	PLPGSQL_GETDIAG_RETURNED_SQLSTATE,
	PLPGSQL_GETDIAG_COLUMN_NAME,
	PLPGSQL_GETDIAG_CONSTRAINT_NAME,
	PLPGSQL_GETDIAG_DATATYPE_NAME,
	PLPGSQL_GETDIAG_MESSAGE_TEXT,
	PLPGSQL_GETDIAG_TABLE_NAME,
	PLPGSQL_GETDIAG_SCHEMA_NAME
} PLpgSQL_getdiag_kind;

/*
 * RAISE 语句选项
 */
typedef enum PLpgSQL_raise_option_type
{
	PLPGSQL_RAISEOPTION_ERRCODE,
	PLPGSQL_RAISEOPTION_MESSAGE,
	PLPGSQL_RAISEOPTION_DETAIL,
	PLPGSQL_RAISEOPTION_HINT,
	PLPGSQL_RAISEOPTION_COLUMN,
	PLPGSQL_RAISEOPTION_CONSTRAINT,
	PLPGSQL_RAISEOPTION_DATATYPE,
	PLPGSQL_RAISEOPTION_TABLE,
	PLPGSQL_RAISEOPTION_SCHEMA
} PLpgSQL_raise_option_type;

/*
 * plpgsql 变量解析的行为模式
 */
typedef enum PLpgSQL_resolve_option
{
	PLPGSQL_RESOLVE_ERROR,		/* 有歧义则抛错 */
	PLPGSQL_RESOLVE_VARIABLE,	/* 优先 plpgsql 变量而非表列 */
	PLPGSQL_RESOLVE_COLUMN		/* 优先表列而非 plpgsql 变量 */
} PLpgSQL_resolve_option;


/**********************************************************************
 * 节点与结构定义
 **********************************************************************/

/*
 * PG 数据类型
 */
typedef struct PLpgSQL_type
{
	char	   *typname;		/* 类型的（简单）名 */
	Oid			typoid;			/* 数据类型的 OID */
	PLpgSQL_type_type ttype;	/* PLPGSQL_TTYPE_ 码 */
	int16		typlen;			/* 复制自其 pg_type 项 */
	bool		typbyval;
	char		typtype;
	Oid			collation;		/* 来自 pg_type，但可被覆盖 */
	bool		typisarray;		/* 是真数组，或其上的域 */
	int32		atttypmod;		/* typmod（取自别处） */
	/* 其余字段只用于具名复合类型（不含 RECORD） */
	TypeName   *origtypname;	/* 用户所写的类型名 */
	TypeCacheEntry *tcache;		/* 复合类型的 typcache 项 */
	uint64		tupdesc_id;		/* 上次所见 tupdesc 标识 */
} PLpgSQL_type;

/*
 * 待规划执行的 SQL 查询
 */
typedef struct PLpgSQL_expr
{
	char	   *query;			/* 查询串，逐字取自函数体 */
	RawParseMode parseMode;		/* raw_parser() 的模式 */
	SPIPlanPtr	plan;			/* 计划，未做出则为 NULL */
	Bitmapset  *paramnos;		/* 本查询引用的全部 dno */

	/* 含本表达式的函数（首次解析查询前未设） */
	struct PLpgSQL_function *func;

	/* 本表达式可见的命名空间链 */
	struct PLpgSQL_nsitem *ns;

	/* 简单表达式快速执行路径的字段： */
	Expr	   *expr_simple_expr;	/* NULL 即非简单表达式 */
	Oid			expr_simple_type;	/* 结果类型Oid(若简单) */
	int32		expr_simple_typmod; /* 结果typmod，若简单 */
	bool		expr_simple_mutable;	/* 若简单式可变则为真 */

	/*
	 * 这些字段用来优化对扩展 datum 变量的赋值。
	 * 若本表达式是赋给一个简单变量的源，
	 * target_param 存该变量的 dno，否则为 -1。
	 * 若把 expr_simple_expr 里的 Param 配上该变量，
	 * 该 Param 的地址存入 expr_rw_param；随后表达式
	 * 代码生成将允许该 Param 的值以读/写方式传递。
	 */
	int			target_param;	/* 赋值目标 dno，无则为 -1。 */
	Param	   *expr_rw_param;	/* 表达式中读/写 Param，若有 */

	/*
	 * 若表达式曾被判定为简单，我们在此记住其
	 * CachedPlanSource 与 CachedPlan。若 expr_simple_plan_lxid 匹配
	 * 当前 LXID，则我们在当前事务里对
	 * expr_simple_plan 持有引用计数；否则重用前须取得。
	 */
	CachedPlanSource *expr_simple_plansource;	/* 从 "plan" 抽出 */
	CachedPlan *expr_simple_plan;	/* 从 "plan" 抽出 */
	LocalTransactionId expr_simple_plan_lxid;

	/*
	 * 若表达式简单且已在当前事务中备好，
	 * 则 expr_simple_state 与 expr_simple_in_use 有效。检验：
	 * 看 expr_simple_lxid 是否匹配当前 LXID。若否，
	 * expr_simple_state 大概指向垃圾！
	 */
	ExprState  *expr_simple_state;	/* expr_simple_expr 的求值树 */
	bool		expr_simple_in_use; /* 求值树活动则为真 */
	LocalTransactionId expr_simple_lxid;
} PLpgSQL_expr;

/*
 * 通用 datum 数组项
 *
 * PLpgSQL_datum 统摄 PLpgSQL_var、PLpgSQL_row、PLpgSQL_rec、
 * PLpgSQL_recfield 等子类型。
 */
typedef struct PLpgSQL_datum
{
	PLpgSQL_datum_type dtype;
	int			dno;
} PLpgSQL_datum;

/*
 * 标量或复合变量
 *
 * 变体 PLpgSQL_var、PLpgSQL_row、PLpgSQL_rec 共用这些
 * 字段
 */
typedef struct PLpgSQL_variable
{
	PLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	PLpgSQL_expr *default_val;
} PLpgSQL_variable;

/*
 * 标量变量
 *
 * DTYPE_VAR 与 DTYPE_PROMISE 的 datum 都用此结构。
 * PROMISE 在多数用途上与 VAR 完全一样，
 * 但若读取时此前从未被赋值，则先算出
 * 一个特殊的 "承诺" 值并赋给该 datum，
 * 然后才读取。此技巧省去计算该变量值
 * 开销，用于我们预期许多函数
 * 永不会读取的情形。
 */
typedef struct PLpgSQL_var
{
	PLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	PLpgSQL_expr *default_val;
	/* PLpgSQL_variable 字段结束 */

	PLpgSQL_type *datatype;

	/*
	 * 声明为 CURSOR FOR <query> 的变量大体像普通的
	 * refcursor 型标量变量，但另有这些
	 * 属性：
	 */
	PLpgSQL_expr *cursor_explicit_expr;
	int			cursor_explicit_argrow;
	int			cursor_options;

	/* 以下字段可在运行时改变 */

	Datum		value;
	bool		isnull;
	bool		freeval;

	/*
	 * promise 字段记下若必须兑现该承诺时要赋的
	 * 承诺值。若它是普通变量，或承诺已兑现，
	 * 此处为 PLPGSQL_PROMISE_NONE。
	 */
	PLpgSQL_promise_type promise;
} PLpgSQL_var;

/*
 * 行变量：表示 INTO 子句、FOR 循环目标列表、
 * 游标参数列表等处列出的一个或多个变量。我们也
 * 用行表示函数有多个 OUT 参数时的那些参数。
 *
 * 注意：PL/pgSQL 代码无法给这样的行命名，
 * 故许多函数不必支持它们。
 *
 * 因此行变量并无真正的名字，我们按惯例把
 * refname 设为 "(unnamed row)"。也可以留 NULL，
 * 但留着更方便：可假定 refname 在 PLpgSQL_variable
 * 的各变体里都有效。
 *
 * 行不支持 isconst、notnull 与 default_val，
 * 故总为 0 或空。行的成员变量应在编译时
 * 已查过可写，故 isconst 正确地设为假。
 * notnull 与 default_val 不适用。
 */
typedef struct PLpgSQL_row
{
	PLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	PLpgSQL_expr *default_val;
	/* PLpgSQL_variable 字段结束 */

	/*
	 * 仅当我们可能要把行转成复合 datum 时才设置
	 * rowtupdesc。目前仅 OUT 参数会如此。
	 * 否则它为 NULL。
	 */
	TupleDesc	rowtupdesc;

	int			nfields;
	char	  **fieldnames;
	int		   *varnos;
} PLpgSQL_row;

/*
 * 记录变量（任何复合类型，含 RECORD）
 */
typedef struct PLpgSQL_rec
{
	PLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	PLpgSQL_expr *default_val;
	/* PLpgSQL_variable 字段结束 */

	/*
	 * 注意：非 RECORD 时，我们可能不时用
	 * datatype->origtypname 重新查找复合类型。这可能
	 * 改变 rectypeid。
	 */

	PLpgSQL_type *datatype;		/* rectypeid 为 RECORDOID 时可为 NULL */
	Oid			rectypeid;		/* 变量的声明类型 */
	/* 本记录的 RECFIELDs 串成链以便访问 */
	int			firstfield;		/* 首个 RECFIELD 的 dno，无则 -1 */

	/* 以下字段可在运行时改变 */

	/* 记录变量总存成 "扩展" 记录 */
	ExpandedRecordHeader *erh;
} PLpgSQL_rec;

/*
 * 记录字段
 */
typedef struct PLpgSQL_recfield
{
	PLpgSQL_datum_type dtype;
	int			dno;
	/* PLpgSQL_datum 字段结束 */

	char	   *fieldname;		/* 字段名 */
	int			recparentno;	/* 父记录的 dno */
	int			nextfield;		/* 下一子项的 dno，无则 -1 */
	uint64		rectupledescid; /* 上次查找时记录的 tupledesc ID */
	ExpandedRecordFieldInfo finfo;	/* attnum 及类型信息 */
	/* 若 rectupledescid 为 INVALID_TUPLEDESC_IDENTIFIER，finfo 无效 */
} PLpgSQL_recfield;

/*
 * 编译器命名空间树中的项
 */
typedef struct PLpgSQL_nsitem
{
	PLpgSQL_nsitem_type itemtype;

	/*
	 * 对标签，itemno 是枚举 PLpgSQL_label_type 的值。对其他
	 * 项类型，itemno 是对应 PLpgSQL_datum 的 dno。
	 */
	int			itemno;
	struct PLpgSQL_nsitem *prev;
	char		name[FLEXIBLE_ARRAY_MEMBER];	/* 以 NUL 结尾的串 */
} PLpgSQL_nsitem;

/*
 * 通用执行节点
 */
typedef struct PLpgSQL_stmt
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;

	/*
	 * 本函数内唯一语句 ID（从 1 起；0 为无效或未
	 * 设置）。性能分析器可用它作为数组下标，数组是
	 * 每语句的度量。
	 */
	unsigned int stmtid;
} PLpgSQL_stmt;

/*
 * 一个 EXCEPTION 条件名
 */
typedef struct PLpgSQL_condition
{
	int			sqlerrstate;	/* SQLSTATE 码 */
	char	   *condname;		/* 条件名（供调试） */
	struct PLpgSQL_condition *next;
} PLpgSQL_condition;

/*
 * EXCEPTION 块
 */
typedef struct PLpgSQL_exception_block
{
	int			sqlstate_varno;
	int			sqlerrm_varno;
	List	   *exc_list;		/* WHEN 子句列表 */
} PLpgSQL_exception_block;

/*
 * 一个 EXCEPTION WHEN 子句
 */
typedef struct PLpgSQL_exception
{
	int			lineno;
	PLpgSQL_condition *conditions;
	List	   *action;			/* 语句列表 */
} PLpgSQL_exception;

/*
 * 语句块
 */
typedef struct PLpgSQL_stmt_block
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	List	   *body;			/* 语句列表 */
	int			n_initvars;		/* initvarnos[] 的长度 */
	int		   *initvarnos;		/* 本块所声明变量的 dno */
	PLpgSQL_exception_block *exceptions;
} PLpgSQL_stmt_block;

/*
 * 赋值语句
 */
typedef struct PLpgSQL_stmt_assign
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			varno;
	PLpgSQL_expr *expr;
} PLpgSQL_stmt_assign;

/*
 * PERFORM 语句
 */
typedef struct PLpgSQL_stmt_perform
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *expr;
} PLpgSQL_stmt_perform;

/*
 * CALL 语句
 */
typedef struct PLpgSQL_stmt_call
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *expr;
	bool		is_call;
	PLpgSQL_variable *target;
} PLpgSQL_stmt_call;

/*
 * COMMIT 语句
 */
typedef struct PLpgSQL_stmt_commit
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		chain;
} PLpgSQL_stmt_commit;

/*
 * ROLLBACK 语句
 */
typedef struct PLpgSQL_stmt_rollback
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		chain;
} PLpgSQL_stmt_rollback;

/*
 * GET DIAGNOSTICS 项
 */
typedef struct PLpgSQL_diag_item
{
	PLpgSQL_getdiag_kind kind;	/* 所需诊断值的标识 */
	int			target;			/* 赋到何处 */
} PLpgSQL_diag_item;

/*
 * GET DIAGNOSTICS 语句
 */
typedef struct PLpgSQL_stmt_getdiag
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		is_stacked;		/* STACKED 还是 CURRENT 诊断区？ */
	List	   *diag_items;		/* PLpgSQL_diag_item 列表 */
} PLpgSQL_stmt_getdiag;

/*
 * IF 语句
 */
typedef struct PLpgSQL_stmt_if
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *cond;			/* THEN 用的布尔表达式 */
	List	   *then_body;		/* 语句列表 */
	List	   *elsif_list;		/* PLpgSQL_if_elsif 结构列表 */
	List	   *else_body;		/* 语句列表 */
} PLpgSQL_stmt_if;

/*
 * IF 的一个 ELSIF 分支
 */
typedef struct PLpgSQL_if_elsif
{
	int			lineno;
	PLpgSQL_expr *cond;			/* 本分支的布尔表达式 */
	List	   *stmts;			/* 语句列表 */
} PLpgSQL_if_elsif;

/*
 * CASE 语句
 */
typedef struct PLpgSQL_stmt_case
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *t_expr;		/* 测试表达式，无则为 NULL */
	int			t_varno;		/* 存测试表达式值的变量 */
	List	   *case_when_list; /* PLpgSQL_case_when 结构列表 */
	bool		have_else;		/* 因列表可能为空而需要的标志 */
	List	   *else_stmts;		/* 语句列表 */
} PLpgSQL_stmt_case;

/*
 * CASE 的一个分支
 */
typedef struct PLpgSQL_case_when
{
	int			lineno;
	PLpgSQL_expr *expr;			/* 本分支的布尔表达式 */
	List	   *stmts;			/* 语句列表 */
} PLpgSQL_case_when;

/*
 * 无条件 LOOP 语句
 */
typedef struct PLpgSQL_stmt_loop
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	List	   *body;			/* 语句列表 */
} PLpgSQL_stmt_loop;

/*
 * WHILE 条件 LOOP 语句
 */
typedef struct PLpgSQL_stmt_while
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	PLpgSQL_expr *cond;
	List	   *body;			/* 语句列表 */
} PLpgSQL_stmt_while;

/*
 * 整数循环变量的 FOR 语句
 */
typedef struct PLpgSQL_stmt_fori
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	PLpgSQL_var *var;
	PLpgSQL_expr *lower;
	PLpgSQL_expr *upper;
	PLpgSQL_expr *step;			/* NULL 表示默认(即 BY 1) */
	int			reverse;
	List	   *body;			/* 语句列表 */
} PLpgSQL_stmt_fori;

/*
 * PLpgSQL_stmt_forq 表示遍历 SQL 查询的 FOR，是
 * PLpgSQL_stmt_fors、PLpgSQL_stmt_forc 与 PLpgSQL_stmt_dynfors
 * 的公共超类型。
 */
typedef struct PLpgSQL_stmt_forq
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	PLpgSQL_variable *var;		/* 循环变量(记录或行) */
	List	   *body;			/* 语句列表 */
} PLpgSQL_stmt_forq;

/*
 * 遍历 SELECT 的 FOR 语句
 */
typedef struct PLpgSQL_stmt_fors
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	PLpgSQL_variable *var;		/* 循环变量(记录或行) */
	List	   *body;			/* 语句列表 */
	/* 须与 PLpgSQL_stmt_forq 匹配的字段结束 */
	PLpgSQL_expr *query;
} PLpgSQL_stmt_fors;

/*
 * 遍历游标的 FOR 语句
 */
typedef struct PLpgSQL_stmt_forc
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	PLpgSQL_variable *var;		/* 循环变量(记录或行) */
	List	   *body;			/* 语句列表 */
	/* 须与 PLpgSQL_stmt_forq 匹配的字段结束 */
	int			curvar;
	PLpgSQL_expr *argquery;		/* 游标参数，若有 */
} PLpgSQL_stmt_forc;

/*
 * 遍历 EXECUTE 的 FOR 语句
 */
typedef struct PLpgSQL_stmt_dynfors
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	PLpgSQL_variable *var;		/* 循环变量(记录或行) */
	List	   *body;			/* 语句列表 */
	/* 须与 PLpgSQL_stmt_forq 匹配的字段结束 */
	PLpgSQL_expr *query;
	List	   *params;			/* USING 表达式 */
} PLpgSQL_stmt_dynfors;

/*
 * FOREACH 数组元素循环
 */
typedef struct PLpgSQL_stmt_foreach_a
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	int			varno;			/* 循环目标变量 */
	int			slice;			/* 切片维度，或 0 */
	PLpgSQL_expr *expr;			/* 数组表达式 */
	List	   *body;			/* 语句列表 */
} PLpgSQL_stmt_foreach_a;

/*
 * OPEN 游标
 */
typedef struct PLpgSQL_stmt_open
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			curvar;
	int			cursor_options;
	PLpgSQL_expr *argquery;
	PLpgSQL_expr *query;
	PLpgSQL_expr *dynquery;
	List	   *params;			/* USING 表达式 */
} PLpgSQL_stmt_open;

/*
 * FETCH 或 MOVE 语句
 */
typedef struct PLpgSQL_stmt_fetch
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_variable *target;	/* 目标(记录或行) */
	int			curvar;			/* 从中取数的游标变量 */
	FetchDirection direction;	/* 取数方向 */
	long		how_many;		/* 计数，常量时 expr 为 NULL */
	PLpgSQL_expr *expr;			/* 表达式计数 */
	bool		is_move;		/* 是 FETCH 还是 MOVE？ */
	bool		returns_multiple_rows;	/* 能返回多于一行吗？ */
} PLpgSQL_stmt_fetch;

/*
 * CLOSE 游标
 */
typedef struct PLpgSQL_stmt_close
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			curvar;
} PLpgSQL_stmt_close;

/*
 * EXIT 或 CONTINUE 语句
 */
typedef struct PLpgSQL_stmt_exit
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		is_exit;		/* 这是 EXIT 还是 CONTINUE？ */
	char	   *label;			/* 无标签的 EXIT/CONTINUE 则为 NULL */
	PLpgSQL_expr *cond;
} PLpgSQL_stmt_exit;

/*
 * RETURN 语句
 */
typedef struct PLpgSQL_stmt_return
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *expr;
	int			retvarno;
} PLpgSQL_stmt_return;

/*
 * RETURN NEXT 语句
 */
typedef struct PLpgSQL_stmt_return_next
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *expr;
	int			retvarno;
} PLpgSQL_stmt_return_next;

/*
 * RETURN QUERY 语句
 */
typedef struct PLpgSQL_stmt_return_query
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *query;		/* 若静态查询 */
	PLpgSQL_expr *dynquery;		/* 若动态查询(RETURN QUERY EXECUTE) */
	List	   *params;			/* 动态查询的 USING 参数 */
} PLpgSQL_stmt_return_query;

/*
 * RAISE 语句
 */
typedef struct PLpgSQL_stmt_raise
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			elog_level;
	char	   *condname;		/* 条件名、SQLSTATE，或 NULL */
	char	   *message;		/* 旧式消息格式字面量，或 NULL */
	List	   *params;			/* 旧式消息用的表达式列表 */
	List	   *options;		/* PLpgSQL_raise_option 列表 */
} PLpgSQL_stmt_raise;

/*
 * RAISE 语句选项
 */
typedef struct PLpgSQL_raise_option
{
	PLpgSQL_raise_option_type opt_type;
	PLpgSQL_expr *expr;
} PLpgSQL_raise_option;

/*
 * ASSERT 语句
 */
typedef struct PLpgSQL_stmt_assert
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *cond;
	PLpgSQL_expr *message;
} PLpgSQL_stmt_assert;

/*
 * 要执行的通用 SQL 语句
 */
typedef struct PLpgSQL_stmt_execsql
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *sqlstmt;
	bool		mod_stmt;		/* 是 INSERT/UPDATE/DELETE/MERGE 吗？ */
	bool		mod_stmt_set;	/* mod_stmt 有效了吗 */
	bool		into;			/* 有 INTO？ */
	bool		strict;			/* INTO STRICT 标 */
	PLpgSQL_variable *target;	/* INTO 目标(记录或行) */
} PLpgSQL_stmt_execsql;

/*
 * 要执行的动态 SQL 串
 */
typedef struct PLpgSQL_stmt_dynexecute
{
	PLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	PLpgSQL_expr *query;		/* 串表达式 */
	bool		into;			/* 有 INTO？ */
	bool		strict;			/* INTO STRICT 标 */
	PLpgSQL_variable *target;	/* INTO 目标(记录或行) */
	List	   *params;			/* USING 表达式 */
} PLpgSQL_stmt_dynexecute;

/*
 * 函数的散列查找键
 */
typedef struct PLpgSQL_func_hashkey
{
	Oid			funcOid;

	bool		isTrigger;		/* DML 触发器调用时为真 */
	bool		isEventTrigger; /* 事件触发器调用时为真 */

	/* 注意本结构的填充字节必须清零！ */

	/*
	 * 对触发器函数，触发器 OID 是散列键的一部分。
	 * 我们要为所用的每个触发器分别编译该函数，
	 * 以防行类型或过渡表名不同。若不是作为
	 * DML 触发器调用，则为零。
	 */
	Oid			trigOid;

	/*
	 * 输入的 collation 也必须作为散列键的一部分，
	 * 因为不同的 collation 设置要生成不同计划
	 * 且 Param 的 collation 不同。
	 */
	Oid			inputCollation;

	/*
	 * 散列键含实际参数类型，以支持多态的
	 * PLpgSQL 函数。注意多出的位置必须清零！
	 */
	Oid			argtypes[FUNC_MAX_ARGS];
} PLpgSQL_func_hashkey;

/*
 * 触发器型
 */
typedef enum PLpgSQL_trigtype
{
	PLPGSQL_DML_TRIGGER,
	PLPGSQL_EVENT_TRIGGER,
	PLPGSQL_NOT_TRIGGER
} PLpgSQL_trigtype;

/*
 * 完整的已编译函数
 */
typedef struct PLpgSQL_function
{
	char	   *fn_signature;
	Oid			fn_oid;
	TransactionId fn_xmin;
	ItemPointerData fn_tid;
	PLpgSQL_trigtype fn_is_trigger;
	Oid			fn_input_collation;
	PLpgSQL_func_hashkey *fn_hashkey;	/* 指回散列表键 */
	MemoryContext fn_cxt;

	Oid			fn_rettype;
	int			fn_rettyplen;
	bool		fn_retbyval;
	bool		fn_retistuple;
	bool		fn_retisdomain;
	bool		fn_retset;
	bool		fn_readonly;
	char		fn_prokind;

	int			fn_nargs;
	int			fn_argvarnos[FUNC_MAX_ARGS];
	int			out_param_varno;
	int			found_varno;
	int			new_varno;
	int			old_varno;

	PLpgSQL_resolve_option resolve_option;

	bool		print_strict_params;

	/* 额外检查 */
	int			extra_warnings;
	int			extra_errors;

	/* 表示函数局部变量的 datum */
	int			ndatums;
	PLpgSQL_datum **datums;
	Size		copiable_size;	/* 供局部实例化 datum 的空间 */

	/* 函数体语法树 */
	PLpgSQL_stmt_block *action;

	/* 解析函数体所得数据 */
	unsigned int nstatements;	/* 分配 stmtid 用的计数器 */
	bool		requires_procedure_resowner;	/* 含 CALL 或 DO？ */

	/* 这些字段在函数被使用时改变 */
	struct PLpgSQL_execstate *cur_estate;
	unsigned long use_count;
} PLpgSQL_function;

/*
 * 运行时执行数据
 */
typedef struct PLpgSQL_execstate
{
	PLpgSQL_function *func;		/* 正在执行的函数 */

	TriggerData *trigdata;		/* 普通触发器时的触发数据 */
	EventTriggerData *evtrigdata;	/* 事件触发器时的触发数据 */

	Datum		retval;
	bool		retisnull;
	Oid			rettype;		/* 当前返回值类型 */

	Oid			fn_rettype;		/* 所声明函数返回类型的信息 */
	bool		retistuple;
	bool		retisset;

	bool		readonly_func;
	bool		atomic;

	char	   *exitlabel;		/* 当前 EXIT/CONTINUE 的目标标签，
								 * 若有的话 */
	ErrorData  *cur_error;		/* 当前异常处理器的错误 */

	Tuplestorestate *tuple_store;	/* SRFs 在此累积结果 */
	TupleDesc	tuple_store_desc;	/* tuple_store 中元组的描述符 */
	MemoryContext tuple_store_cxt;
	ResourceOwner tuple_store_owner;
	ReturnSetInfo *rsi;

	int			found_varno;

	/*
	 * 表示函数局部变量的那些 datum。其中一些
	 * 是本执行状态里的局部存储，另一些只指向
	 * 属于 PLpgSQL_function 的共享副本，取决于我们
	 * 是否需要该 dtype 的按次执行状态。
	 */
	int			ndatums;
	PLpgSQL_datum **datums;
	/* 存变量值的上下文（与函数的 SPI_proc 上下文相同） */
	MemoryContext datum_context;

	/*
	 * paramLI 用来把局部变量的值传给执行器。
	 * 它没有 ParamExternData 数组；我们只是按需
	 * 动态实例化参数数据。按惯例，PARAM_EXTERN
	 * 参数的 paramid 等于所引用局部变量的 dno。
	 */
	ParamListInfo paramLI;

	/* 供 "简单" 表达式求值用的 EState 与 resowner */
	EState	   *simple_eval_estate;
	ResourceOwner simple_eval_resowner;

	/* 若在跑非原子过程或 DO 块，供 CALL 用的 resowner */
	ResourceOwner procedure_resowner;

	/* 执行类型转换时用的查找表 */
	HTAB	   *cast_hash;

	/* 语句寿命临时值用的内存上下文 */
	MemoryContext stmt_mcontext;	/* 当前语句上下文，无则 NULL */
	MemoryContext stmt_mcontext_parent; /* 当前上下文的父级 */

	/* 查询或表达式求值结果的临时状态 */
	SPITupleTable *eval_tuptable;
	uint64		eval_processed;
	ExprContext *eval_econtext; /* 用于执行简单表达式 */

	/* 错误上下文报告用的状态信息 */
	PLpgSQL_stmt *err_stmt;		/* 当前语句 */
	PLpgSQL_variable *err_var;	/* 当前变量，若在 DECLARE 段中 */
	const char *err_text;		/* 附加的状态信息 */

	void	   *plugin_info;	/* 留给可选插件使用 */
} PLpgSQL_execstate;

/*
 * PLpgSQL_plugin 结构表示一个插装插件。
 * 要插装 PL/pgSQL，插件库必须访问会合变量
 * PLpgSQL_plugin，并让它指向一个 PLpgSQL_plugin 结构。
 * 通常该结构可以只是插件库里的静态数据。
 * 我们预期插件会在库加载时（_PG_init()）这样做。
 *
 * 本结构基本上是一组函数指针：在 pl_exec.c 的
 * 各个要紧处，我们调用这些函数
 * 在指针非 NULL 时，让插件有机会观察
 * 我们的动作。
 *
 * 函数开始时调用 func_setup，早于初始化
 * 该函数所定义的局部变量。
 *
 * func_beg 在我们开始函数并初始化局部变量之后
 * 被调用。
 *
 * func_end 在函数结束时被调用。
 *
 * stmt_beg 与 stmt_end 在每条语句前后分别
 * 调用。
 *
 * 另外，在任何 func_setup 调用之前，PL/pgSQL 立刻
 * 把其余字段填上它自己某些函数的指针，以便
 * 插件方便地调用那些函数。暴露的函数是：
 *		plpgsql_exec_error_callback
 *		exec_assign_expr
 *		exec_assign_value
 *		exec_eval_datum
 *		exec_cast_value
 * plpgsql_exec_error_callback 其实不是给插件调用的，
 * 而是让它识别 PL/pgSQL 错误上下文栈帧。
 * 其余的对类似调试器的插件有用，用来检查并
 * 设置变量。
 */
typedef struct PLpgSQL_plugin
{
	/* 由插件设置的函数指针 */
	void		(*func_setup) (PLpgSQL_execstate *estate, PLpgSQL_function *func);
	void		(*func_beg) (PLpgSQL_execstate *estate, PLpgSQL_function *func);
	void		(*func_end) (PLpgSQL_execstate *estate, PLpgSQL_function *func);
	void		(*stmt_beg) (PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt);
	void		(*stmt_end) (PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt);

	/* 由 PL/pgSQL 自己设置的函数指针 */
	void		(*error_callback) (void *arg);
	void		(*assign_expr) (PLpgSQL_execstate *estate,
								PLpgSQL_datum *target,
								PLpgSQL_expr *expr);
	void		(*assign_value) (PLpgSQL_execstate *estate,
								 PLpgSQL_datum *target,
								 Datum value, bool isNull,
								 Oid valtype, int32 valtypmod);
	void		(*eval_datum) (PLpgSQL_execstate *estate, PLpgSQL_datum *datum,
							   Oid *typeId, int32 *typetypmod,
							   Datum *value, bool *isnull);
	Datum		(*cast_value) (PLpgSQL_execstate *estate,
							   Datum value, bool *isnull,
							   Oid valtype, int32 valtypmod,
							   Oid reqtype, int32 reqtypmod);
} PLpgSQL_plugin;

/*
 * 解析时使用的结构类型
 */

typedef struct PLword
{
	char	   *ident;			/* palloc 转换后的标识符 */
	bool		quoted;			/* 加了双引号吗？ */
} PLword;

typedef struct PLcword
{
	List	   *idents;			/* 复合标识符（String 的列表） */
} PLcword;

typedef struct PLwdatum
{
	PLpgSQL_datum *datum;		/* 被引用的变量 */
	char	   *ident;			/* 简单名时有效 */
	bool		quoted;
	List	   *idents;			/* 复合名时有效 */
} PLwdatum;

/**********************************************************************
 * 全局变量声明
 **********************************************************************/

typedef enum
{
	IDENTIFIER_LOOKUP_NORMAL,	/* 正常处理变量名 */
	IDENTIFIER_LOOKUP_DECLARE,	/* 在 DECLARE 中不查找名字 */
	IDENTIFIER_LOOKUP_EXPR		/* SQL 表达式中的特殊情形 */
} IdentifierLookup;

extern IdentifierLookup plpgsql_IdentifierLookup;

extern int	plpgsql_variable_conflict;

extern bool plpgsql_print_strict_params;

extern bool plpgsql_check_asserts;

/* 额外的编译时与运行时检查 */
#define PLPGSQL_XCHECK_NONE						0
#define PLPGSQL_XCHECK_SHADOWVAR				(1 << 1)
#define PLPGSQL_XCHECK_TOOMANYROWS				(1 << 2)
#define PLPGSQL_XCHECK_STRICTMULTIASSIGNMENT	(1 << 3)
#define PLPGSQL_XCHECK_ALL						((int) ~0)

extern int	plpgsql_extra_warnings;
extern int	plpgsql_extra_errors;

extern bool plpgsql_check_syntax;
extern bool plpgsql_DumpExecTree;

extern PLpgSQL_stmt_block *plpgsql_parse_result;

extern int	plpgsql_nDatums;
extern PLpgSQL_datum **plpgsql_Datums;

extern char *plpgsql_error_funcname;

extern PLpgSQL_function *plpgsql_curr_compile;
extern MemoryContext plpgsql_compile_tmp_cxt;

extern PLpgSQL_plugin **plpgsql_plugin_ptr;

/**********************************************************************
 * 函数声明
 **********************************************************************/

/*
 * pl_comp.c 中的函数
 */
extern PGDLLEXPORT PLpgSQL_function *plpgsql_compile(FunctionCallInfo fcinfo,
													 bool forValidator);
extern PLpgSQL_function *plpgsql_compile_inline(char *proc_source);
extern PGDLLEXPORT void plpgsql_parser_setup(struct ParseState *pstate,
											 PLpgSQL_expr *expr);
extern bool plpgsql_parse_word(char *word1, const char *yytxt, bool lookup,
							   PLwdatum *wdatum, PLword *word);
extern bool plpgsql_parse_dblword(char *word1, char *word2,
								  PLwdatum *wdatum, PLcword *cword);
extern bool plpgsql_parse_tripword(char *word1, char *word2, char *word3,
								   PLwdatum *wdatum, PLcword *cword);
extern PLpgSQL_type *plpgsql_parse_wordtype(char *ident);
extern PLpgSQL_type *plpgsql_parse_cwordtype(List *idents);
extern PLpgSQL_type *plpgsql_parse_wordrowtype(char *ident);
extern PLpgSQL_type *plpgsql_parse_cwordrowtype(List *idents);
extern PGDLLEXPORT PLpgSQL_type *plpgsql_build_datatype(Oid typeOid, int32 typmod,
														Oid collation,
														TypeName *origtypname);
extern PLpgSQL_variable *plpgsql_build_variable(const char *refname, int lineno,
												PLpgSQL_type *dtype,
												bool add2namespace);
extern PLpgSQL_rec *plpgsql_build_record(const char *refname, int lineno,
										 PLpgSQL_type *dtype, Oid rectypeid,
										 bool add2namespace);
extern PLpgSQL_recfield *plpgsql_build_recfield(PLpgSQL_rec *rec,
												const char *fldname);
extern PGDLLEXPORT int plpgsql_recognize_err_condition(const char *condname,
													   bool allow_sqlstate);
extern PLpgSQL_condition *plpgsql_parse_err_condition(char *condname);
extern void plpgsql_adddatum(PLpgSQL_datum *newdatum);
extern int	plpgsql_add_initdatums(int **varnos);
extern void plpgsql_HashTableInit(void);

/*
 * pl_exec.c 中的函数
 */
extern Datum plpgsql_exec_function(PLpgSQL_function *func,
								   FunctionCallInfo fcinfo,
								   EState *simple_eval_estate,
								   ResourceOwner simple_eval_resowner,
								   ResourceOwner procedure_resowner,
								   bool atomic);
extern HeapTuple plpgsql_exec_trigger(PLpgSQL_function *func,
									  TriggerData *trigdata);
extern void plpgsql_exec_event_trigger(PLpgSQL_function *func,
									   EventTriggerData *trigdata);
extern void plpgsql_xact_cb(XactEvent event, void *arg);
extern void plpgsql_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
							   SubTransactionId parentSubid, void *arg);
extern PGDLLEXPORT Oid plpgsql_exec_get_datum_type(PLpgSQL_execstate *estate,
												   PLpgSQL_datum *datum);
extern void plpgsql_exec_get_datum_type_info(PLpgSQL_execstate *estate,
											 PLpgSQL_datum *datum,
											 Oid *typeId, int32 *typMod,
											 Oid *collation);

/*
 * pl_funcs.c 中处理命名空间的函数
 */
extern void plpgsql_ns_init(void);
extern void plpgsql_ns_push(const char *label,
							PLpgSQL_label_type label_type);
extern void plpgsql_ns_pop(void);
extern PLpgSQL_nsitem *plpgsql_ns_top(void);
extern void plpgsql_ns_additem(PLpgSQL_nsitem_type itemtype, int itemno, const char *name);
extern PGDLLEXPORT PLpgSQL_nsitem *plpgsql_ns_lookup(PLpgSQL_nsitem *ns_cur, bool localmode,
													 const char *name1, const char *name2,
													 const char *name3, int *names_used);
extern PLpgSQL_nsitem *plpgsql_ns_lookup_label(PLpgSQL_nsitem *ns_cur,
											   const char *name);
extern PLpgSQL_nsitem *plpgsql_ns_find_nearest_loop(PLpgSQL_nsitem *ns_cur);

/*
 * pl_funcs.c 中的其他函数
 */
extern PGDLLEXPORT const char *plpgsql_stmt_typename(PLpgSQL_stmt *stmt);
extern const char *plpgsql_getdiag_kindname(PLpgSQL_getdiag_kind kind);
extern void plpgsql_free_function_memory(PLpgSQL_function *func);
extern void plpgsql_dumptree(PLpgSQL_function *func);

/*
 * pl_scanner.c 里的扫描函数
 */
extern int	plpgsql_base_yylex(void);
extern int	plpgsql_yylex(void);
extern int	plpgsql_token_length(void);
extern void plpgsql_push_back_token(int token);
extern bool plpgsql_token_is_unreserved_keyword(int token);
extern void plpgsql_append_source_text(StringInfo buf,
									   int startlocation, int endlocation);
extern int	plpgsql_peek(void);
extern void plpgsql_peek2(int *tok1_p, int *tok2_p, int *tok1_loc,
						  int *tok2_loc);
extern int	plpgsql_scanner_errposition(int location);
extern void plpgsql_yyerror(const char *message) pg_attribute_noreturn();
extern int	plpgsql_location_to_lineno(int location);
extern int	plpgsql_latest_lineno(void);
extern void plpgsql_scanner_init(const char *str);
extern void plpgsql_scanner_finish(void);

/*
 * gram.y 外部项
 */
extern int	plpgsql_yyparse(void);

#endif							/* PLPGSQL_H */
