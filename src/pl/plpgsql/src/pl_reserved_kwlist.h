/*-------------------------------------------------------------------------
 *
 * pl_reserved_kwlist.h
 *
 * 关键字表各自放在单独源文件里，供
 * 自动工具使用。关键字的确切表示由
 *  PG_KEYWORD 宏决定。本文件不定义它，调用者可
 * 为特殊目的自行定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpgsql/src/pl_reserved_kwlist.h
 *
 *-------------------------------------------------------------------------
 */

/* 这里故意不放 #ifndef PL_RESERVED_KWLIST_H。 */

/*
 * (关键字名, 关键字记号值) 对的列表。
 *
 * 注意不要把同一个词放进 pl_unreserved_kwlist.h。
 *
 * 注意：gen_keywordlist.pl 要求条目按 ASCII 序排列。
 */

/* 名与值 */
PG_KEYWORD("all", K_ALL)
PG_KEYWORD("begin", K_BEGIN)
PG_KEYWORD("by", K_BY)
PG_KEYWORD("case", K_CASE)
PG_KEYWORD("declare", K_DECLARE)
PG_KEYWORD("else", K_ELSE)
PG_KEYWORD("end", K_END)
PG_KEYWORD("execute", K_EXECUTE)
PG_KEYWORD("for", K_FOR)
PG_KEYWORD("foreach", K_FOREACH)
PG_KEYWORD("from", K_FROM)
PG_KEYWORD("if", K_IF)
PG_KEYWORD("in", K_IN)
PG_KEYWORD("into", K_INTO)
PG_KEYWORD("loop", K_LOOP)
PG_KEYWORD("not", K_NOT)
PG_KEYWORD("null", K_NULL)
PG_KEYWORD("or", K_OR)
PG_KEYWORD("strict", K_STRICT)
PG_KEYWORD("then", K_THEN)
PG_KEYWORD("to", K_TO)
PG_KEYWORD("using", K_USING)
PG_KEYWORD("when", K_WHEN)
PG_KEYWORD("while", K_WHILE)
