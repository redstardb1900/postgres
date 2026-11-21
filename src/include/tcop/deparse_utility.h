/*-------------------------------------------------------------------------
 *
 * deparse_utility.h
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/deparse_utility.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEPARSE_UTILITY_H
#define DEPARSE_UTILITY_H

#include "access/attnum.h"
#include "catalog/objectaddress.h"
#include "nodes/nodes.h"
#include "utils/aclchk_internal.h"


/*
 * 跟踪已收集命令的支持。
 */
typedef enum CollectedCommandType
{
	SCT_Simple,
	SCT_AlterTable,
	SCT_Grant,
	SCT_AlterOpFamily,
	SCT_AlterDefaultPrivileges,
	SCT_CreateOpClass,
	SCT_AlterTSConfig
} CollectedCommandType;

/*
 * 对 ALTER TABLE 命令，我们保留其中的子命令列表。
 */
typedef struct CollectedATSubcmd
{
	ObjectAddress address;		/* 受影响的列、约束、索引…… */
	Node	   *parsetree;
} CollectedATSubcmd;

typedef struct CollectedCommand
{
	CollectedCommandType type;

	bool		in_extension;
	Node	   *parsetree;

	union
	{
		/* 多数命令 */
		struct
		{
			ObjectAddress address;
			ObjectAddress secondaryObject;
		}			simple;

		/* ALTER TABLE 及其内部用法 */
		struct
		{
			Oid			objectId;
			Oid			classId;
			List	   *subcmds;
		}			alterTable;

		/* GRANT / REVOKE */
		struct
		{
			InternalGrant *istmt;
		}			grant;

		/* ALTER OPERATOR FAMILY */
		struct
		{
			ObjectAddress address;
			List	   *operators;
			List	   *procedures;
		}			opfam;

		/* CREATE OPERATOR CLASS */
		struct
		{
			ObjectAddress address;
			List	   *operators;
			List	   *procedures;
		}			createopc;

		/* ALTER TEXT SEARCH CONFIGURATION ADD/ALTER/DROP MAPPING */
		struct
		{
			ObjectAddress address;
			Oid		   *dictIds;
			int			ndicts;
		}			atscfg;

		/* ALTER DEFAULT PRIVILEGES */
		struct
		{
			ObjectType	objtype;
		}			defprivs;
	}			d;

	struct CollectedCommand *parent;	/* 嵌套时 */
} CollectedCommand;

#endif							/* DEPARSE_UTILITY_H */
