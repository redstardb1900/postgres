/*-------------------------------------------------------------------------
 *
 * extension.h
 *	扩展管理命令（创建/删除扩展）。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/extension.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENSION_H
#define EXTENSION_H

#include "catalog/objectaddress.h"
#include "parser/parse_node.h"


/*
 * 仅在跑 CREATE EXTENSION 或 ALTER
 * EXTENSION UPDATE 时为真。指示 recordDependencyOnCurrentExtension()
 * 为脚本所建各 SQL 对象登记对当前 pg_extension 的依赖。
 * 亦指示 performDeletion()
 * 去掉此类依赖且不跟随，以便扩展
 * 脚本可删除成员对象而无须先显式
 * 与扩展解除关联。
 */
extern PGDLLIMPORT bool creating_extension;
extern PGDLLIMPORT Oid CurrentExtensionObject;


extern ObjectAddress CreateExtension(ParseState *pstate, CreateExtensionStmt *stmt);

extern void RemoveExtensionById(Oid extId);

extern ObjectAddress InsertExtensionTuple(const char *extName, Oid extOwner,
										  Oid schemaOid, bool relocatable, const char *extVersion,
										  Datum extConfig, Datum extCondition,
										  List *requiredExtensions);

extern ObjectAddress ExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt);

extern ObjectAddress ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
													ObjectAddress *objAddr);

extern Oid	get_extension_oid(const char *extname, bool missing_ok);
extern char *get_extension_name(Oid ext_oid);
extern Oid	get_extension_schema(Oid ext_oid);
extern bool extension_file_exists(const char *extensionName);

extern ObjectAddress AlterExtensionNamespace(const char *extensionName, const char *newschema,
											 Oid *oldschema);

#endif							/* EXTENSION_H */
