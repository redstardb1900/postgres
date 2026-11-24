/*-------------------------------------------------------------------------
 *
 * pl_unreserved_kwlist.h
 *
 * 关键字表各自放在单独源文件里，供
 * 自动工具使用。关键字的确切表示由
 *  PG_KEYWORD 宏决定。本文件不定义它，调用者可
 * 为特殊目的自行定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpgsql/src/pl_unreserved_kwlist.h
 *
 *-------------------------------------------------------------------------
 */

/* 这里故意不放 #ifndef PL_UNRESERVED_KWLIST_H。 */

/*
 * (关键字名, 关键字记号值) 对的列表。
 *
 * 注意不要把同一个词放进 pl_reserved_kwlist.h。还要
 * 确保 pl_gram.y 的 unreserved_keyword 产生式与本表一致。
 *
 * 注意：gen_keywordlist.pl 要求条目按 ASCII 序排列。
 */

/* 名与值 */
PG_KEYWORD("absolute", K_ABSOLUTE)
PG_KEYWORD("alias", K_ALIAS)
PG_KEYWORD("and", K_AND)
PG_KEYWORD("array", K_ARRAY)
PG_KEYWORD("assert", K_ASSERT)
PG_KEYWORD("backward", K_BACKWARD)
PG_KEYWORD("call", K_CALL)
PG_KEYWORD("chain", K_CHAIN)
PG_KEYWORD("close", K_CLOSE)
PG_KEYWORD("collate", K_COLLATE)
PG_KEYWORD("column", K_COLUMN)
PG_KEYWORD("column_name", K_COLUMN_NAME)
PG_KEYWORD("commit", K_COMMIT)
PG_KEYWORD("constant", K_CONSTANT)
PG_KEYWORD("constraint", K_CONSTRAINT)
PG_KEYWORD("constraint_name", K_CONSTRAINT_NAME)
PG_KEYWORD("continue", K_CONTINUE)
PG_KEYWORD("current", K_CURRENT)
PG_KEYWORD("cursor", K_CURSOR)
PG_KEYWORD("datatype", K_DATATYPE)
PG_KEYWORD("debug", K_DEBUG)
PG_KEYWORD("default", K_DEFAULT)
PG_KEYWORD("detail", K_DETAIL)
PG_KEYWORD("diagnostics", K_DIAGNOSTICS)
PG_KEYWORD("do", K_DO)
PG_KEYWORD("dump", K_DUMP)
PG_KEYWORD("elseif", K_ELSIF)
PG_KEYWORD("elsif", K_ELSIF)
PG_KEYWORD("errcode", K_ERRCODE)
PG_KEYWORD("error", K_ERROR)
PG_KEYWORD("exception", K_EXCEPTION)
PG_KEYWORD("exit", K_EXIT)
PG_KEYWORD("fetch", K_FETCH)
PG_KEYWORD("first", K_FIRST)
PG_KEYWORD("forward", K_FORWARD)
PG_KEYWORD("get", K_GET)
PG_KEYWORD("hint", K_HINT)
PG_KEYWORD("import", K_IMPORT)
PG_KEYWORD("info", K_INFO)
PG_KEYWORD("insert", K_INSERT)
PG_KEYWORD("is", K_IS)
PG_KEYWORD("last", K_LAST)
PG_KEYWORD("log", K_LOG)
PG_KEYWORD("merge", K_MERGE)
PG_KEYWORD("message", K_MESSAGE)
PG_KEYWORD("message_text", K_MESSAGE_TEXT)
PG_KEYWORD("move", K_MOVE)
PG_KEYWORD("next", K_NEXT)
PG_KEYWORD("no", K_NO)
PG_KEYWORD("notice", K_NOTICE)
PG_KEYWORD("open", K_OPEN)
PG_KEYWORD("option", K_OPTION)
PG_KEYWORD("perform", K_PERFORM)
PG_KEYWORD("pg_context", K_PG_CONTEXT)
PG_KEYWORD("pg_datatype_name", K_PG_DATATYPE_NAME)
PG_KEYWORD("pg_exception_context", K_PG_EXCEPTION_CONTEXT)
PG_KEYWORD("pg_exception_detail", K_PG_EXCEPTION_DETAIL)
PG_KEYWORD("pg_exception_hint", K_PG_EXCEPTION_HINT)
PG_KEYWORD("pg_routine_oid", K_PG_ROUTINE_OID)
PG_KEYWORD("print_strict_params", K_PRINT_STRICT_PARAMS)
PG_KEYWORD("prior", K_PRIOR)
PG_KEYWORD("query", K_QUERY)
PG_KEYWORD("raise", K_RAISE)
PG_KEYWORD("relative", K_RELATIVE)
PG_KEYWORD("return", K_RETURN)
PG_KEYWORD("returned_sqlstate", K_RETURNED_SQLSTATE)
PG_KEYWORD("reverse", K_REVERSE)
PG_KEYWORD("rollback", K_ROLLBACK)
PG_KEYWORD("row_count", K_ROW_COUNT)
PG_KEYWORD("rowtype", K_ROWTYPE)
PG_KEYWORD("schema", K_SCHEMA)
PG_KEYWORD("schema_name", K_SCHEMA_NAME)
PG_KEYWORD("scroll", K_SCROLL)
PG_KEYWORD("slice", K_SLICE)
PG_KEYWORD("sqlstate", K_SQLSTATE)
PG_KEYWORD("stacked", K_STACKED)
PG_KEYWORD("table", K_TABLE)
PG_KEYWORD("table_name", K_TABLE_NAME)
PG_KEYWORD("type", K_TYPE)
PG_KEYWORD("use_column", K_USE_COLUMN)
PG_KEYWORD("use_variable", K_USE_VARIABLE)
PG_KEYWORD("variable_conflict", K_VARIABLE_CONFLICT)
PG_KEYWORD("warning", K_WARNING)
