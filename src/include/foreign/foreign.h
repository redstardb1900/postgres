/*-------------------------------------------------------------------------
 *
 * foreign.h
 *	  外部数据包装器、服务器与用户映射的支持。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/foreign/foreign.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FOREIGN_H
#define FOREIGN_H

#include "nodes/parsenodes.h"


/* 取得用户映射用户名的助手 */
#define MappingUserName(userid) \
	(OidIsValid(userid) ? GetUserNameFromId(userid, false) : "public")


typedef struct ForeignDataWrapper
{
	Oid			fdwid;			/* FDW Oid */
	Oid			owner;			/* FDW 属主 Oid */
	char	   *fdwname;		/* FDW 名称 */
	Oid			fdwhandler;		/* 处理器函数 Oid，或 0 */
	Oid			fdwvalidator;	/* 验证器函数 Oid，或 0 */
	List	   *options;		/* fdwoptions 的 DefElem 表 */
} ForeignDataWrapper;

typedef struct ForeignServer
{
	Oid			serverid;		/* server Oid */
	Oid			fdwid;			/* FDW */
	Oid			owner;			/* 服务器属主 Oid */
	char	   *servername;		/* 服务器名称 */
	char	   *servertype;		/* server type, optional */
	char	   *serverversion;	/* 服务器版本，可选 */
	List	   *options;		/* srvoptions 的 DefElem 表 */
} ForeignServer;

typedef struct UserMapping
{
	Oid			umid;			/* 用户映射 Oid */
	Oid			userid;			/* local user Oid */
	Oid			serverid;		/* server Oid */
	List	   *options;		/* useoptions 的 DefElem 表 */
} UserMapping;

typedef struct ForeignTable
{
	Oid			relid;			/* 关系 Oid */
	Oid			serverid;		/* server Oid */
	List	   *options;		/* ftoptions 的 DefElem 表 */
} ForeignTable;

/* GetForeignServerExtended 的标志 */
#define FSV_MISSING_OK	0x01

/* GetForeignDataWrapperExtended 的标志 */
#define FDW_MISSING_OK	0x01


extern ForeignServer *GetForeignServer(Oid serverid);
extern ForeignServer *GetForeignServerExtended(Oid serverid,
											   bits16 flags);
extern ForeignServer *GetForeignServerByName(const char *srvname,
											 bool missing_ok);
extern UserMapping *GetUserMapping(Oid userid, Oid serverid);
extern ForeignDataWrapper *GetForeignDataWrapper(Oid fdwid);
extern ForeignDataWrapper *GetForeignDataWrapperExtended(Oid fdwid,
														 bits16 flags);
extern ForeignDataWrapper *GetForeignDataWrapperByName(const char *fdwname,
													   bool missing_ok);
extern ForeignTable *GetForeignTable(Oid relid);

extern List *GetForeignColumnOptions(Oid relid, AttrNumber attnum);

extern Oid	get_foreign_data_wrapper_oid(const char *fdwname, bool missing_ok);
extern Oid	get_foreign_server_oid(const char *servername, bool missing_ok);

#endif							/* FOREIGN_H */
