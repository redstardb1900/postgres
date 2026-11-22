/*-------------------------------------------------------------------------
 *
 * rls.h
 *	  与 rowsecurity 功能一起使用的行级安全（RLS）
 *	  实用命令头文件。
 *
 * Copyright (c) 2007-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/rls.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RLS_H
#define RLS_H

/* GUC 变量 */
extern PGDLLIMPORT bool row_security;

/*
 * 供 check_enable_rls 调用方用。
 *
 * 查询所涉表上 RLS 可能完全关闭（简单情形），
 * 也可能依赖当前环境（运行查询的角色或
 * row_security GUC 的值），或只是照常启用。
 * 见上。
 *
 * 若相关表未开 RLS，则返回 RLS_NONE，表示不必
 * 担心因 RLS 而使查询计划失效。若表开了 RLS 但
 * 我们暂时绕过，则返回 RLS_NONE_ENV，表示环境
 * 一变就需失效并重规划。最后，若查询应打开 RLS，
 * 则返回 RLS_ENABLED，同样表示环境变化时需失效。
 * 见上。
 * 见上。
 *
 * 若 noError 为 true（表示调用方只想知道对此用户
 * 是否应应用 RLS，但不希望因此抛错；用于其他错误
 * 路径，只是决定是否把表中数据回传给用户），也会
 * 返回 RLS_ENABLED。
 * 见上。
 */
enum CheckEnableRlsResult
{
	RLS_NONE,
	RLS_NONE_ENV,
	RLS_ENABLED
};

extern int	check_enable_rls(Oid relid, Oid checkAsUser, bool noError);

#endif							/* RLS_H */
