/*-------------------------------------------------------------------------
 *
 * 支持 FE/BE 连接的接口。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/connect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONNECT_H
#define CONNECT_H

/*
 * 本 SQL 安装始终安全的 search_path，使恶意用户
 * 无法夺取控制。未限定名的 CREATE 会失败，因未
 * 选创建模式。不降低 pg_temp，故适于我们控制整段
 * FE/BE 连接之处，不适于 SECURITY DEFINER 函数。
 * 可移植到引入模式的 PostgreSQL 7.3。从可能兼容
 * 旧服务器的代码连到更旧版本时，跳过此项。
 * 略
 */
#define ALWAYS_SECURE_SEARCH_PATH_SQL \
	"SELECT pg_catalog.set_config('search_path', '', false);"

#endif							/* CONNECT_H */
