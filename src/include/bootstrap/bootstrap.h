/*-------------------------------------------------------------------------
 *
 * bootstrap.h
 *	  引导代码的头文件
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/bootstrap/bootstrap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BOOTSTRAP_H
#define BOOTSTRAP_H

#include "nodes/execnodes.h"


/*
 * MAXATTR 为引导期关系所支持的最大属性数
 * （即系统表可能的最大属性数）。
 */
#define MAXATTR 40

#define BOOTCOL_NULL_AUTO			1
#define BOOTCOL_NULL_FORCE_NULL		2
#define BOOTCOL_NULL_FORCE_NOT_NULL 3

extern PGDLLIMPORT Relation boot_reldesc;
extern PGDLLIMPORT Form_pg_attribute attrtypes[MAXATTR];
extern PGDLLIMPORT int numattr;


extern void BootstrapModeMain(int argc, char *argv[], bool check_only) pg_attribute_noreturn();

extern void closerel(char *relname);
extern void boot_openrel(char *relname);

extern void DefineAttr(char *name, char *type, int attnum, int nullness);
extern void InsertOneTuple(void);
extern void InsertOneValue(char *value, int i);
extern void InsertOneNull(int i);

extern void index_register(Oid heap, Oid ind, IndexInfo *indexInfo);
extern void build_indices(void);

extern void boot_get_type_io_data(Oid typid,
								  int16 *typlen,
								  bool *typbyval,
								  char *typalign,
								  char *typdelim,
								  Oid *typioparam,
								  Oid *typinput,
								  Oid *typoutput);

extern int	boot_yyparse(void);

extern int	boot_yylex(void);
extern void boot_yyerror(const char *message) pg_attribute_noreturn();

#endif							/* BOOTSTRAP_H */
