/*-------------------------------------------------------------------------
 *
 * reltrigger.h
 *	  POSTGRES 关系触发器定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/reltrigger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELTRIGGER_H
#define RELTRIGGER_H


/*
 * 这些结构本属 trigger.h，但单独放置以便
 * 能干净地被 rel.h 及其他处包含。
 */

typedef struct Trigger
{
	Oid			tgoid;			/* 触发器 OID（pg_trigger） */
	/* 其余字段自 pg_trigger 复制，见 pg_trigger.h */
	char	   *tgname;
	Oid			tgfoid;
	int16		tgtype;
	char		tgenabled;
	bool		tgisinternal;
	bool		tgisclone;
	Oid			tgconstrrelid;
	Oid			tgconstrindid;
	Oid			tgconstraint;
	bool		tgdeferrable;
	bool		tginitdeferred;
	int16		tgnargs;
	int16		tgnattr;
	int16	   *tgattr;
	char	  **tgargs;
	char	   *tgqual;
	char	   *tgoldtable;
	char	   *tgnewtable;
} Trigger;

typedef struct TriggerDesc
{
	Trigger    *triggers;		/* Trigger 结构数组 */
	int			numtriggers;	/* 数组条目数 */

	/*
	 * 这些标志表明数组是否至少含一类触发器。
	 * 用于在没有时跳过搜索数组。
	 */
	bool		trig_insert_before_row;
	bool		trig_insert_after_row;
	bool		trig_insert_instead_row;
	bool		trig_insert_before_statement;
	bool		trig_insert_after_statement;
	bool		trig_update_before_row;
	bool		trig_update_after_row;
	bool		trig_update_instead_row;
	bool		trig_update_before_statement;
	bool		trig_update_after_statement;
	bool		trig_delete_before_row;
	bool		trig_delete_after_row;
	bool		trig_delete_instead_row;
	bool		trig_delete_before_statement;
	bool		trig_delete_after_statement;
	/* 没有行级 truncate 触发器 */
	bool		trig_truncate_before_statement;
	bool		trig_truncate_after_statement;
	/* 是否至少有一个触发器指定了各转移关系？ */
	bool		trig_insert_new_table;
	bool		trig_update_old_table;
	bool		trig_update_new_table;
	bool		trig_delete_old_table;
} TriggerDesc;

#endif							/* RELTRIGGER_H */
