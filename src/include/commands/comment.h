/*
 * src/include/commands/comment.h
 *
 *-------------------------------------------------------------------------
 *
 * comment.h
 *
 * commands/comment.c 中函数的原型
 *
 * Copyright (c) 1999-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#ifndef COMMENT_H
#define COMMENT_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

/*------------------------------------------------------------------
 * 函数原型 --
 *
 * 下列原型定义注释相关例程的公共
 * 函数。CommentObject() 实现 SQL "COMMENT ON"
 * CreateComments 创建（comment 为 NULL 则删）注释
 * 给特定键。两方法均有普通与共享对象
 * 此二方法均有普通对象与共享对象
 * 两套版本。
 *------------------------------------------------------------------
 */

extern ObjectAddress CommentObject(CommentStmt *stmt);

extern void DeleteComments(Oid oid, Oid classoid, int32 subid);

extern void CreateComments(Oid oid, Oid classoid, int32 subid, const char *comment);

extern void DeleteSharedComments(Oid oid, Oid classoid);

extern void CreateSharedComments(Oid oid, Oid classoid, const char *comment);

extern char *GetComment(Oid oid, Oid classoid, int32 subid);

#endif							/* COMMENT_H */
