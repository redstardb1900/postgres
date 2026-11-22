/* -------------------------------------------------------------------------
 *
 * pg_subscription_rel.h
 *	  definition of the system catalog containing the state for each
 *	  replicated table in each subscription (pg_subscription_rel)
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_subscription_rel.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SUBSCRIPTION_REL_H
#define PG_SUBSCRIPTION_REL_H

#include "access/xlogdefs.h"
#include "catalog/genbki.h"
#include "catalog/pg_subscription_rel_d.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_subscription_rel definition. cpp turns this into
 *		typedef struct FormData_pg_subscription_rel
 * ----------------
 */
CATALOG(pg_subscription_rel,6102,SubscriptionRelRelationId)
{
	Oid			srsubid BKI_LOOKUP(pg_subscription);	/*  订阅 Oid  */
	Oid			srrelid BKI_LOOKUP(pg_class);	/*  关系 Oid  */
	char		srsubstate;		/*  关系在订阅中的状态  */

	/*
	 * Although srsublsn is a fixed-width type, it is allowed to be NULL, so
	 * we prevent direct C code access to it just as for a varlena field.
	 */
#ifdef CATALOG_VARLEN			/*  变长字段自此开始  */

	XLogRecPtr	srsublsn BKI_FORCE_NULL;	/* remote LSN of the state change
											 * used for synchronization
											 * coordination, or NULL if not
											 * valid */
#endif
} FormData_pg_subscription_rel;

typedef FormData_pg_subscription_rel *Form_pg_subscription_rel;

DECLARE_UNIQUE_INDEX_PKEY(pg_subscription_rel_srrelid_srsubid_index, 6117, SubscriptionRelSrrelidSrsubidIndexId, on pg_subscription_rel using btree(srrelid oid_ops, srsubid oid_ops));

#ifdef EXPOSE_TO_CLIENT_CODE

/* ----------------
 *		substate constants
 * ----------------
 */
#define SUBREL_STATE_INIT		'i' /* 初始化中(sublsn NULL) */
#define SUBREL_STATE_DATASYNC	'd' /*  正在同步数据(sublsn
									 * NULL) */
#define SUBREL_STATE_FINISHEDCOPY 'f'	/*  表同步复制阶段已完成
										 * (sublsn NULL) */
#define SUBREL_STATE_SYNCDONE	's' /*  同步已在应用之前完成
									 * (sublsn 已设)  */
#define SUBREL_STATE_READY		'r' /* ready (sublsn set) */

/*  从不存入目录,仅用于 IPC。  */
#define SUBREL_STATE_UNKNOWN	'\0'	/* 未知状态 */
#define SUBREL_STATE_SYNCWAIT	'w' /*  等待同步  */
#define SUBREL_STATE_CATCHUP	'c' /*  追赶应用  */

#endif							/* EXPOSE_TO_CLIENT_CODE */

typedef struct SubscriptionRelState
{
	Oid			relid;
	XLogRecPtr	lsn;
	char		state;
} SubscriptionRelState;

extern void AddSubscriptionRelState(Oid subid, Oid relid, char state,
									XLogRecPtr sublsn);
extern void UpdateSubscriptionRelState(Oid subid, Oid relid, char state,
									   XLogRecPtr sublsn);
extern void UpdateSubscriptionRelStateEx(Oid subid, Oid relid, char state,
										 XLogRecPtr sublsn, bool already_locked);
extern char GetSubscriptionRelState(Oid subid, Oid relid, XLogRecPtr *sublsn);
extern void RemoveSubscriptionRel(Oid subid, Oid relid);

extern bool HasSubscriptionRelations(Oid subid);
extern List *GetSubscriptionRelations(Oid subid, bool not_ready);

#endif							/* PG_SUBSCRIPTION_REL_H */
