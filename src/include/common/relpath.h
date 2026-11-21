/*-------------------------------------------------------------------------
 *
 * relpath.h
 *		GetRelationPath() 及其友元的声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/relpath.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELPATH_H
#define RELPATH_H

/*
 *	此处需 'pgrminclude ignore'，因 CppAsString2() 在
 *	符号未定义时不报错。
 */
#include "catalog/catversion.h" /* pgrminclude ignore */

/*
 * RelFileNumber 类型标识具体关系文件名。
 */
typedef Oid RelFileNumber;
#define InvalidRelFileNumber		((RelFileNumber) InvalidOid)
#define RelFileNumberIsValid(relnumber) \
				((bool) ((relnumber) != InvalidRelFileNumber))

/*
 * 主版本相关表空间子目录名
 */
#define TABLESPACE_VERSION_DIRECTORY	"PG_" PG_MAJORVERSION "_" \
									CppAsString2(CATALOG_VERSION_NO)

/*  关系路径中 OID 预留字符数  */
#define OIDCHARS		10		/* 略 */

/*
 * 分叉名相关。
 *
 * 关系的物理存储由一个或多个分叉组成。
 * 主分叉总是创建，此外可有存各类元数据的附加
 * 分叉。需引用关系中特定分叉时用 ForkNumber。
 * 略
 */
typedef enum ForkNumber
{
	InvalidForkNumber = -1,
	MAIN_FORKNUM = 0,
	FSM_FORKNUM,
	VISIBILITYMAP_FORKNUM,
	INIT_FORKNUM

	/*
	 * 注意：若加新分叉，改 MAX_FORKNUM 并可能改
	 * 下方 FORKNAMECHARS，并更新
	 * 略
	 */
} ForkNumber;

#define MAX_FORKNUM		INIT_FORKNUM

#define FORKNAMECHARS	4		/* 略 */

extern PGDLLIMPORT const char *const forkNames[];

extern ForkNumber forkname_to_number(const char *forkName);
extern int	forkname_chars(const char *str, ForkNumber *fork);

/*
 * 计算关系文件系统路径名的内容。
 */
extern char *GetDatabasePath(Oid dbOid, Oid spcOid);

extern char *GetRelationPath(Oid dbOid, Oid spcOid, RelFileNumber relNumber,
							 int backendId, ForkNumber forkNumber);

/*
 * GetRelationPath 的包装宏。当心 RelFileLocator
 * 或 RelFileLocatorBackend 参数被多次求值！
 */

/*  第一参数为 RelFileLocator  */
#define relpathbackend(rlocator, backend, forknum) \
	GetRelationPath((rlocator).dbOid, (rlocator).spcOid, (rlocator).relNumber, \
					backend, forknum)

/*  第一参数为 RelFileLocator  */
#define relpathperm(rlocator, forknum) \
	relpathbackend(rlocator, InvalidBackendId, forknum)

/*  第一参数为 RelFileLocatorBackend  */
#define relpath(rlocator, forknum) \
	relpathbackend((rlocator).locator, (rlocator).backend, forknum)

#endif							/* RELPATH_H */
