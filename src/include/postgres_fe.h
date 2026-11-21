/*-------------------------------------------------------------------------
 *
 * postgres_fe.h
 *	  PostgreSQL 客户端 .c 文件的主包含文件
 *
 * PostgreSQL 客户端库与应用程序应首先包含本
 * 文件——后端模块则应包含
 * 略
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/postgres_fe.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POSTGRES_FE_H
#define POSTGRES_FE_H

#ifndef FRONTEND
#define FRONTEND 1
#endif

#include "c.h"

#include "common/fe_memutils.h"

#endif							/* POSTGRES_FE_H */
