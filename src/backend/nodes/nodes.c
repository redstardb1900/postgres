/*-------------------------------------------------------------------------
 *
 * nodes.c
 *	  support code for nodes (now that we have removed the home-brew
 *	  inheritance system, our support code for nodes is much simpler)
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/nodes.c
 *
 * HISTORY
 *	  Andrew Yu			Oct 20, 1994	file creation
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
