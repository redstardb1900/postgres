/*-------------------------------------------------------------------------
 *
 * nodes.c
 *	  节点的支持代码（既然已经去掉了自造的
 *	  继承体系，节点的支持代码就简单多了）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/nodes.c
 *
 * 历史
 *	  Andrew Yu			Oct 20, 1994	创建文件
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/nodes.h"

/*
 * newNode() 宏的支持
 *
 * 在 GCC 构建中不需要全局变量 newNodeMacroHolder。
 * 不过我们仍然创建它，以支持把非 GCC 构建的
 * 可加载模块加载进 GCC 构建的后端这种情况。
 */

Node	   *newNodeMacroHolder;
