/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL 事件触发器支持代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/trigger.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

typedef struct EventTriggerQueryState
{
	/* 此状态对象的内存上下文 */
	MemoryContext cxt;

	/* sql_drop */
	slist_head	SQLDropList;
	bool		in_sql_drop;

	/* table_rewrite */
	Oid			table_rewrite_oid;	/* InvalidOid，或为 table_rewrite 事件
									  * 而设置 */
	int			table_rewrite_reason;	/* AT_REWRITE 原因 */

	/* 支持命令收集 */
	bool		commandCollectionInhibited;
	CollectedCommand *currentCommand;
	List	   *commandList;	/* CollectedCommand 列表；参见
								  * deparse_utility.h */
	struct EventTriggerQueryState *previous;
} EventTriggerQueryState;

static EventTriggerQueryState *currentEventTriggerState = NULL;

/* 对已删除对象的支持 */
typedef struct SQLDropObject
{
	ObjectAddress address;
	const char *schemaname;
	const char *objname;
	const char *objidentity;
	const char *objecttype;
	List	   *addrnames;
	List	   *addrargs;
	bool		original;
	bool		normal;
	bool		istemp;
	slist_node	next;
} SQLDropObject;

static void AlterEventTriggerOwner_internal(Relation rel,
											HeapTuple tup,
											Oid newOwnerId);
static void error_duplicate_filter_variable(const char *defname);
static Datum filter_list_to_array(List *filterlist);
static Oid	insert_event_trigger_tuple(const char *trigname, const char *eventname,
									   Oid evtOwner, Oid funcoid, List *taglist);
static void validate_ddl_tags(const char *filtervar, List *taglist);
static void validate_table_rewrite_tags(const char *filtervar, List *taglist);
static void EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata);
static bool obtain_object_name_namespace(const ObjectAddress *object,
										 SQLDropObject *obj);
static const char *stringify_grant_objtype(ObjectType objtype);
static const char *stringify_adefprivs_objtype(ObjectType objtype);

/*
  * 创建事件触发器。
 */
Oid
CreateEventTrigger(CreateEventTrigStmt *stmt)
{
	HeapTuple	tuple;
	Oid			funcoid;
	Oid			funcrettype;
	Oid			evtowner = GetUserId();
	ListCell   *lc;
	List	   *tags = NULL;

	/*
	  * 让数据库所有者甚至普通用户来做这件事会很好，但存在
	  * 明显的权限提升风险，必须先堵住。
	  *
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create event trigger \"%s\"",
						stmt->trigname),
				 errhint("Must be superuser to create an event trigger.")));

	/* 验证事件名称。 */
	if (strcmp(stmt->eventname, "ddl_command_start") != 0 &&
		strcmp(stmt->eventname, "ddl_command_end") != 0 &&
		strcmp(stmt->eventname, "sql_drop") != 0 &&
		strcmp(stmt->eventname, "table_rewrite") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unrecognized event name \"%s\"",
						stmt->eventname)));

	/* 验证过滤条件。 */
	foreach(lc, stmt->whenclause)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "tag") == 0)
		{
			if (tags != NULL)
				error_duplicate_filter_variable(def->defname);
			tags = (List *) def->arg;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized filter variable \"%s\"", def->defname)));
	}

	/* 验证标签列表（如果有）。 */
	if ((strcmp(stmt->eventname, "ddl_command_start") == 0 ||
		 strcmp(stmt->eventname, "ddl_command_end") == 0 ||
		 strcmp(stmt->eventname, "sql_drop") == 0)
		&& tags != NULL)
		validate_ddl_tags("tag", tags);
	else if (strcmp(stmt->eventname, "table_rewrite") == 0
			 && tags != NULL)
		validate_table_rewrite_tags("tag", tags);

	/*
	  * 若同名事件触发器已存在，给用户一个友好的错误消息。
	  *
	 */
	tuple = SearchSysCache1(EVENTTRIGGERNAME, CStringGetDatum(stmt->trigname));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists",
						stmt->trigname)));

	/* 查找并验证触发器函数。 */
	funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != EVENT_TRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type %s",
						NameListToString(stmt->funcname), "event_trigger")));

	/* 插入目录条目。 */
	return insert_event_trigger_tuple(stmt->trigname, stmt->eventname,
									  evtowner, funcoid, tags);
}

/*
  * 验证 DDL 命令标签。
 */
static void
validate_ddl_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tagstr = strVal(lfirst(lc));
		CommandTag	commandTag = GetCommandTagEnum(tagstr);

		if (commandTag == CMDTAG_UNKNOWN)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("filter value \"%s\" not recognized for filter variable \"%s\"",
							tagstr, filtervar)));
		if (!command_tag_event_trigger_ok(commandTag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
					 errmsg("event triggers are not supported for %s",
							tagstr)));
	}
}

/*
  * 验证 table_rewrite 事件搭配的 DDL 命令标签。
 */
static void
validate_table_rewrite_tags(const char *filtervar, List *taglist)
{
	ListCell   *lc;

	foreach(lc, taglist)
	{
		const char *tagstr = strVal(lfirst(lc));
		CommandTag	commandTag = GetCommandTagEnum(tagstr);

		if (!command_tag_table_rewrite_ok(commandTag))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/* translator: %s represents an SQL statement name */
					 errmsg("event triggers are not supported for %s",
							tagstr)));
	}
}

/*
  * 对重复的过滤变量报错。
 */
static void
error_duplicate_filter_variable(const char *defname)
{
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("filter variable \"%s\" specified more than once",
					defname)));
}

/*
  * 插入新 pg_event_trigger 行并记录依赖关系。
 */
static Oid
insert_event_trigger_tuple(const char *trigname, const char *eventname, Oid evtOwner,
						   Oid funcoid, List *taglist)
{
	Relation	tgrel;
	Oid			trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	NameData	evtnamedata,
				evteventdata;
	ObjectAddress myself,
				referenced;

	/* 打开 pg_event_trigger。 */
	tgrel = table_open(EventTriggerRelationId, RowExclusiveLock);

	/* 构建新的 pg_trigger 元组。 */
	trigoid = GetNewOidWithIndex(tgrel, EventTriggerOidIndexId,
								 Anum_pg_event_trigger_oid);
	values[Anum_pg_event_trigger_oid - 1] = ObjectIdGetDatum(trigoid);
	memset(nulls, false, sizeof(nulls));
	namestrcpy(&evtnamedata, trigname);
	values[Anum_pg_event_trigger_evtname - 1] = NameGetDatum(&evtnamedata);
	namestrcpy(&evteventdata, eventname);
	values[Anum_pg_event_trigger_evtevent - 1] = NameGetDatum(&evteventdata);
	values[Anum_pg_event_trigger_evtowner - 1] = ObjectIdGetDatum(evtOwner);
	values[Anum_pg_event_trigger_evtfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_event_trigger_evtenabled - 1] =
		CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);
	if (taglist == NIL)
		nulls[Anum_pg_event_trigger_evttags - 1] = true;
	else
		values[Anum_pg_event_trigger_evttags - 1] =
			filter_list_to_array(taglist);

	/* 插入堆元组。 */
	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);
	CatalogTupleInsert(tgrel, tuple);
	heap_freetuple(tuple);

	/* 依赖于所有者。 */
	recordDependencyOnOwner(EventTriggerRelationId, trigoid, evtOwner);

	/* 依赖于事件触发器函数。 */
	myself.classId = EventTriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;
	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* 依赖于扩展（如果有）。 */
	recordDependencyOnCurrentExtension(&myself, false);

	/* 新事件触发器的创建后钩子 */
	InvokeObjectPostCreateHook(EventTriggerRelationId, trigoid, 0);

	/* 关闭 pg_event_trigger。 */
	table_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
  * 在解析器中，类似 WHEN tag IN ('cmd1', 'cmd2') 的子句
  * 由一个其值为 String 节点 List 的 DefElem 表示；
  * 在目录中，字符串列表存储为 text 数组。
  * 此函数将前者表示转换为后者表示。
  *
  * 为保持整洁，命令标签在目录中以 text 存储。
  * 未来可能出现大小写敏感的过滤变量（目前尚未预期），
  * 届时此代码需做进一步调整。
  *
 */
static Datum
filter_list_to_array(List *filterlist)
{
	ListCell   *lc;
	Datum	   *data;
	int			i = 0,
				l = list_length(filterlist);

	data = (Datum *) palloc(l * sizeof(Datum));

	foreach(lc, filterlist)
	{
		const char *value = strVal(lfirst(lc));
		char	   *result,
				   *p;

		result = pstrdup(value);
		for (p = result; *p; p++)
			*p = pg_ascii_toupper((unsigned char) *p);
		data[i++] = PointerGetDatum(cstring_to_text(result));
		pfree(result);
	}

	return PointerGetDatum(construct_array_builtin(data, l, TEXTOID));
}

/*
  * ALTER EVENT TRIGGER foo ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
Oid
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
	Oid			trigoid;
	Form_pg_event_trigger evtForm;
	char		tgenabled = stmt->tgenabled;

	tgrel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME,
							  CStringGetDatum(stmt->trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist",
						stmt->trigname)));

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	trigoid = evtForm->oid;

	if (!object_ownercheck(EventTriggerRelationId, trigoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EVENT_TRIGGER,
					   stmt->trigname);

	/* 元组是副本，可在下方修改 */
	evtForm->evtenabled = tgenabled;

	CatalogTupleUpdate(tgrel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  trigoid, 0);

	/* 清理 */
	heap_freetuple(tup);
	table_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
  * 按名称更改事件触发器所有者
 */
ObjectAddress
AlterEventTriggerOwner(const char *name, Oid newOwnerId)
{
	Oid			evtOid;
	HeapTuple	tup;
	Form_pg_event_trigger evtForm;
	Relation	rel;
	ObjectAddress address;

	rel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	evtOid = evtForm->oid;

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, EventTriggerRelationId, evtOid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
  * 按 OID 更改事件触发器所有者
 */
void
AlterEventTriggerOwner_oid(Oid trigOid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger with OID %u does not exist", trigOid)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
  * 更改事件触发器所有者的内部核心函数
 */
static void
AlterEventTriggerOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_event_trigger form;

	form = (Form_pg_event_trigger) GETSTRUCT(tup);

	if (form->evtowner == newOwnerId)
		return;

	if (!object_ownercheck(EventTriggerRelationId, form->oid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EVENT_TRIGGER,
					   NameStr(form->evtname));

	/* 新所有者必须是超级用户 */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of event trigger \"%s\"",
						NameStr(form->evtname)),
				 errhint("The owner of an event trigger must be a superuser.")));

	form->evtowner = newOwnerId;
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* 更新所有者依赖引用 */
	changeDependencyOnOwner(EventTriggerRelationId,
							form->oid,
							newOwnerId);

	InvokeObjectPostAlterHook(EventTriggerRelationId,
							  form->oid, 0);
}

/*
  * get_event_trigger_oid - 按名称查找事件触发器 OID。
  *
  * 若 missing_ok 为 false，找不到时则报错。
  * 若为 true，仅返回 InvalidOid。
 */
Oid
get_event_trigger_oid(const char *trigname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(EVENTTRIGGERNAME, Anum_pg_event_trigger_oid,
						  CStringGetDatum(trigname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", trigname)));
	return oid;
}

/*
  * 需要触发指定事件触发器时返回 true，否则 false，
  * 按会话复制角色及注册标签匹配过滤。
  *
 */
static bool
filter_event_trigger(CommandTag tag, EventTriggerCacheItem *item)
{
	/*
	  * 按会话复制角色过滤，此处永远不会看到禁用项。
	  *
	 */
	if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
	{
		if (item->enabled == TRIGGER_FIRES_ON_ORIGIN)
			return false;
	}
	else
	{
		if (item->enabled == TRIGGER_FIRES_ON_REPLICA)
			return false;
	}

	/* 按标签过滤（如果指定了）。 */
	if (!bms_is_empty(item->tagset) && !bms_is_member(tag, item->tagset))
		return false;

	/* 若执行到此说明未过滤此项 */
	return true;
}

/*
  * 为指定的触发事件做准备。返回值是待运行函数的 OID 列表；
  * 若有，trigdata 填充相应的事件触发器数据供接收。
  *
 */
static List *
EventTriggerCommonSetup(Node *parsetree,
						EventTriggerEvent event, const char *eventstr,
						EventTriggerData *trigdata)
{
	CommandTag	tag;
	List	   *cachelist;
	ListCell   *lc;
	List	   *runlist = NIL;

	/*
	 * 我们希望本过程实际被调用的命令标签列表，能与   
	 * CREATE EVENT TRIGGER 接受的列表完全一致。如果          
	 * 本函数被一个 CREATE EVENT TRIGGER 不接受的命令      
	 * 标签调用，这个调试交叉检查就会报错。（不幸      
	 * 的是，似乎没有任何简单、自动化的方法可以验证     
	 * CREATE EVENT TRIGGER 不会接受那些永远到不了该              
	 * 控制点的多余内容。）    
	 *
	 * 如果这个交叉检查对你失败了，你可能需要调整      
	 * standard_ProcessUtility()，不再针对出问题的命令类         
	 * 型来调用事件触发器，或者调整 event_trigger_ok 来       
	 * 接受相关标签。
	 */
#ifdef USE_ASSERT_CHECKING
	{
		CommandTag	dbgtag;

		dbgtag = CreateCommandTag(parsetree);
		if (event == EVT_DDLCommandStart ||
			event == EVT_DDLCommandEnd ||
			event == EVT_SQLDrop)
		{
			if (!command_tag_event_trigger_ok(dbgtag))
				elog(ERROR, "unexpected command tag \"%s\"", GetCommandTagName(dbgtag));
		}
		else if (event == EVT_TableRewrite)
		{
			if (!command_tag_table_rewrite_ok(dbgtag))
				elog(ERROR, "unexpected command tag \"%s\"", GetCommandTagName(dbgtag));
		}
	}
#endif

	/* 使用缓存查找此事件触发器；若无则快速退出。 */
	cachelist = EventCacheLookup(event);
	if (cachelist == NIL)
		return NIL;

	/* 获取命令标签。 */
	tag = CreateCommandTag(parsetree);

	/*
	 * 按命令标签过滤事件触发器列表，并复制到我们     
	 * 的内存上下文中。一旦开始运行命令触发器，乃至    
	 * 一旦做任何触及目录的操作，失效处理就可能使      
	 *  cachelist 指向垃圾数据，因此必须在能做其他事情      
	 * 之前完成。  
	 */
	foreach(lc, cachelist)
	{
		EventTriggerCacheItem *item = lfirst(lc);

		if (filter_event_trigger(tag, item))
		{
			/* 必须计划触发此触发器。  */
			runlist = lappend_oid(runlist, item->fnoid);
		}
	}

	/* 若无函数需运行则不再花费时间 */
	if (runlist == NIL)
		return NIL;

	trigdata->type = T_EventTriggerData;
	trigdata->event = eventstr;
	trigdata->parsetree = parsetree;
	trigdata->tag = tag;

	return runlist;
}

/*
  * 触发 ddl_command_start 触发器。
 */
void
EventTriggerDDLCommandStart(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	 * 事件触发器在独立模式下被完全禁用。这至少有      
	 * 两个原因：                 
	 *
	 * 1. 一个严重损坏的事件触发器不仅可能使数据库 
	 * 不可用，还会阻止将其自身禁用以修复情况。         
	 * 在这种场景下，以独立模式重启提供了一个逃生   
	 * 口。
	 *
	 * 2. BuildEventTriggerCache 依赖于 systable_beginscan_ordered，  
	 * 因此若 pg_event_trigger 的索引损坏，它将发生故障。   
	 * 为了能从损坏的索引中恢复，我们需要某种事件    
	 * 触发器被禁用的操作模式。（或者我们         
	 * 实现 heapscan-and-sort 逻辑，但让灾难恢复场景依赖   
	 * 于其他情况下未经测试的代码并不令人满意。）      
	 */
	if (!IsUnderPostmaster)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandStart,
									  "ddl_command_start",
									  &trigdata);
	if (runlist == NIL)
		return;

	/* 运行触发器。 */
	EventTriggerInvoke(runlist, &trigdata);

	/* 清理。 */
	list_free(runlist);

	/*
	  * 确保事件触发器所做一切对主命令可见。
	  *
	 */
	CommandCounterIncrement();
}

/*
  * 触发 ddl_command_end 触发器。
 */
void
EventTriggerDDLCommandEnd(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	  * 关于单用户模式下为何禁用事件触发器的讨论，
	  * 参见 EventTriggerDDLCommandStart。
	 */
	if (!IsUnderPostmaster)
		return;

	/*
	  * 若状态未设置也什么都不做，当前 DDL 命令开始
	  * 时若无相关事件触发器状态就不会设置。此测试
	  * 看似可选，实则重要——EventTriggerCommonSetup
	  * 可能找到开始时并不存在的触发器。此函数虽不
	  * 会崩溃，但事件触发器函数可能调用
	  * pg_event_trigger_ddl_commands 导致失败。
	  * 最好在下条命令前什么都不做。
	  *
	 */
	if (!currentEventTriggerState)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_DDLCommandEnd, "ddl_command_end",
									  &trigdata);
	if (runlist == NIL)
		return;

	/*
	  * 确保主命令所做一切对事件触发器可见。
	  *
	 */
	CommandCounterIncrement();

	/* 运行触发器。 */
	EventTriggerInvoke(runlist, &trigdata);

	/* 清理。 */
	list_free(runlist);
}

/*
  * 触发 sql_drop 触发器。
 */
void
EventTriggerSQLDrop(Node *parsetree)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	  * 关于单用户模式下为何禁用事件触发器的讨论，
	  * 参见 EventTriggerDDLCommandStart。
	 */
	if (!IsUnderPostmaster)
		return;

	/*
	 * 用当前状态来判断此事件是否真的会触发。如果    
	 * 没有 sql_drop 事件的触发器，那么我们在这里就   
	 * 无事可做。注意，此情况下已删除对象的收集是      
	 * 被禁用的，因此即使我们尝试运行，列表也会是      
	 * 空的。
	 */
	if (!currentEventTriggerState ||
		slist_is_empty(&currentEventTriggerState->SQLDropList))
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_SQLDrop, "sql_drop",
									  &trigdata);

	/*
	  * 若运行列表为空则无事可做。注意这通常不会发生，
	  * 若无 sql_drop 事件则不会收集待删除对象，上面
	  * 便会退出。但若事件触发器中途被删除则可能发生。
	  *
	 */
	if (runlist == NIL)
		return;

	/*
	  * 确保主命令所做一切对事件触发器可见。
	  *
	 */
	CommandCounterIncrement();

	/*
	  * 确保 pg_event_trigger_dropped_objects 仅在运行
	  * 这些触发器时工作。用 PG_TRY 确保即使一个触发器失败
	  * in_sql_drop 也会重置。（可能非必需，currentState
	  * 变量很快会被调用者删除，但稳妥起见。）
	  *
	 */
	currentEventTriggerState->in_sql_drop = true;

	/* 运行触发器。 */
	PG_TRY();
	{
		EventTriggerInvoke(runlist, &trigdata);
	}
	PG_FINALLY();
	{
		currentEventTriggerState->in_sql_drop = false;
	}
	PG_END_TRY();

	/* 清理。 */
	list_free(runlist);
}


/*
  * 触发 table_rewrite 触发器。
 */
void
EventTriggerTableRewrite(Node *parsetree, Oid tableOid, int reason)
{
	List	   *runlist;
	EventTriggerData trigdata;

	/*
	  * 关于单用户模式下为何禁用事件触发器的讨论，
	  * 参见 EventTriggerDDLCommandStart。
	 */
	if (!IsUnderPostmaster)
		return;

	/*
	  * 若状态未设置也什么都不做，当前 DDL 命令开始
	  * 时若无相关事件触发器状态就不会设置。因此此测试
	  * 可能看似可选，但它是*必需的*——
	  * EventTriggerCommonSetup 可能找到开始时并不存在的
	  * 触发器。
	 */
	if (!currentEventTriggerState)
		return;

	runlist = EventTriggerCommonSetup(parsetree,
									  EVT_TableRewrite,
									  "table_rewrite",
									  &trigdata);
	if (runlist == NIL)
		return;

	/*
	  * 确保 pg_event_trigger_table_rewrite_oid 仅在运行
	  * 这些触发器时工作。用 PG_TRY 确保即使一个触发器
	  * 失败 table_rewrite_oid 也重置。（可能非必需，
	  * currentState 变量很快会被调用者删除，但稳妥起见。）
	  *
	 */
	currentEventTriggerState->table_rewrite_oid = tableOid;
	currentEventTriggerState->table_rewrite_reason = reason;

	/* 运行触发器。 */
	PG_TRY();
	{
		EventTriggerInvoke(runlist, &trigdata);
	}
	PG_FINALLY();
	{
		currentEventTriggerState->table_rewrite_oid = InvalidOid;
		currentEventTriggerState->table_rewrite_reason = 0;
	}
	PG_END_TRY();

	/* 清理。 */
	list_free(runlist);

	/*
	  * 确保事件触发器所做一切对主命令可见。
	  *
	 */
	CommandCounterIncrement();
}

/*
  * 调用事件触发器列表中的每个事件触发器。
 */
static void
EventTriggerInvoke(List *fn_oid_list, EventTriggerData *trigdata)
{
	MemoryContext context;
	MemoryContext oldcontext;
	ListCell   *lc;
	bool		first = true;

	/* 防止递归事件触发器导致栈溢出 */
	check_stack_depth();

	/*
	  * 在各自的内存上下文中评估事件触发器，以便泄漏得到
	  * 及时清理。
	 */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"event trigger context",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);

	/* 调用每个事件触发器。 */
	foreach(lc, fn_oid_list)
	{
		LOCAL_FCINFO(fcinfo, 0);
		Oid			fnoid = lfirst_oid(lc);
		FmgrInfo	flinfo;
		PgStat_FunctionCallUsage fcusage;

		elog(DEBUG1, "EventTriggerInvoke %u", fnoid);

		/*
		  * 让每个事件触发器看到前一个的操作结果。
		  * 调用者负责在事件触发器与事务中其他操作之间
		  * 所需的命令计数器递增。
		  *
		 */
		if (first)
			first = false;
		else
			CommandCounterIncrement();

		/* 查找该函数      */
		fmgr_info(fnoid, &flinfo);

		/* 调用该函数，不传参数但设置一个上下文。      */
		InitFunctionCallInfoData(*fcinfo, &flinfo, 0,
								 InvalidOid, (Node *) trigdata, NULL);
		pgstat_init_function_usage(fcinfo, &fcusage);
		FunctionCallInvoke(fcinfo);
		pgstat_end_function_usage(&fcusage, true);

		/* Reclaim memory. */
		MemoryContextReset(context);
	}

	/* 恢复旧内存上下文并删除临时上下文。 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextDelete(context);
}

/*
  * 事件触发器是否支持此对象类型？
 */
bool
EventTriggerSupportsObjectType(ObjectType obtype)
{
	switch (obtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_TABLESPACE:
		case OBJECT_ROLE:
		case OBJECT_PARAMETER_ACL:
			/* 不支持全局对象         */
			return false;
		case OBJECT_EVENT_TRIGGER:
			/* 不支持事件触发器上的事件触发器   */
			return false;
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLUMN:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFACL:
		case OBJECT_DEFAULT:
		case OBJECT_DOMAIN:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EXTENSION:
		case OBJECT_FDW:
		case OBJECT_FOREIGN_SERVER:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_FUNCTION:
		case OBJECT_INDEX:
		case OBJECT_LANGUAGE:
		case OBJECT_LARGEOBJECT:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_POLICY:
		case OBJECT_PROCEDURE:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROUTINE:
		case OBJECT_RULE:
		case OBJECT_SCHEMA:
		case OBJECT_SEQUENCE:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TABLE:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_TYPE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			return true;

			/*
			  * 故意不放 default: case；
			  * 期望编译器在新 ObjectType 未处理时告警。
			 */
	}

	/* 不应执行到此，若执行到则返回"不支持" */
	return false;
}

/*
  * 事件触发器是否支持此对象类？
 */
bool
EventTriggerSupportsObjectClass(ObjectClass objclass)
{
	switch (objclass)
	{
		case OCLASS_DATABASE:
		case OCLASS_TBLSPACE:
		case OCLASS_ROLE:
		case OCLASS_ROLE_MEMBERSHIP:
		case OCLASS_PARAMETER_ACL:
			/* 不支持全局对象         */
			return false;
		case OCLASS_EVENT_TRIGGER:
			/* 不支持事件触发器上的事件触发器   */
			return false;
		case OCLASS_CLASS:
		case OCLASS_PROC:
		case OCLASS_TYPE:
		case OCLASS_CAST:
		case OCLASS_COLLATION:
		case OCLASS_CONSTRAINT:
		case OCLASS_CONVERSION:
		case OCLASS_DEFAULT:
		case OCLASS_LANGUAGE:
		case OCLASS_LARGEOBJECT:
		case OCLASS_OPERATOR:
		case OCLASS_OPCLASS:
		case OCLASS_OPFAMILY:
		case OCLASS_AM:
		case OCLASS_AMOP:
		case OCLASS_AMPROC:
		case OCLASS_REWRITE:
		case OCLASS_TRIGGER:
		case OCLASS_SCHEMA:
		case OCLASS_STATISTIC_EXT:
		case OCLASS_TSPARSER:
		case OCLASS_TSDICT:
		case OCLASS_TSTEMPLATE:
		case OCLASS_TSCONFIG:
		case OCLASS_FDW:
		case OCLASS_FOREIGN_SERVER:
		case OCLASS_USER_MAPPING:
		case OCLASS_DEFACL:
		case OCLASS_EXTENSION:
		case OCLASS_POLICY:
		case OCLASS_PUBLICATION:
		case OCLASS_PUBLICATION_NAMESPACE:
		case OCLASS_PUBLICATION_REL:
		case OCLASS_SUBSCRIPTION:
		case OCLASS_TRANSFORM:
			return true;

			/*
			  * 故意不放 default: case；
			  * 期望编译器在新 OCLASS 未处理时告警。
			 */
	}

	/* 不应执行到此，若执行到则返回"不支持" */
	return false;
}

/*
  * 为执行新的完整查询准备事件触发器状态（必要时）；
  * 返回是否已准备。若已准备，查询结束时必须
  * 调用 EventTriggerEndCompleteQuery，无论成败——
  * 因此必须使用 PG_TRY 块。
 */
bool
EventTriggerBeginCompleteQuery(void)
{
	EventTriggerQueryState *state;
	MemoryContext cxt;

	/*
	  * 目前，sql_drop、table_rewrite、ddl_command_end 是
	  * 需要事件触发器状态的唯一原因；若无任何一个则不安装。
	  *
	 */
	if (!trackDroppedObjectsNeeded())
		return false;

	cxt = AllocSetContextCreate(TopMemoryContext,
								"event trigger state",
								ALLOCSET_DEFAULT_SIZES);
	state = MemoryContextAlloc(cxt, sizeof(EventTriggerQueryState));
	state->cxt = cxt;
	slist_init(&(state->SQLDropList));
	state->in_sql_drop = false;
	state->table_rewrite_oid = InvalidOid;

	state->commandCollectionInhibited = currentEventTriggerState ?
		currentEventTriggerState->commandCollectionInhibited : false;
	state->currentCommand = NULL;
	state->commandList = NIL;
	state->previous = currentEventTriggerState;
	currentEventTriggerState = state;

	return true;
}

/*
 * 查询完成（或出错）——清理本地状态，恢复上一个状态 
 * 。 
 *
 * 注意：若 EventTriggerBeginCompleteQuery 先前返回 false，         
 * 此时不可调用。     
 *
 * 注意：本例程可能在失败事务的 PG_CATCH 块中被调用，    
 * 因此要当心，不要运行任何不必要的操作。（尤其是     
 * 分配内存恐不明智。）    
 */
void
EventTriggerEndCompleteQuery(void)
{
	EventTriggerQueryState *prevstate;

	prevstate = currentEventTriggerState->previous;

	/* 避免对 SQLDropList 各项逐一 pfree： */
	MemoryContextDelete(currentEventTriggerState->cxt);

	currentEventTriggerState = prevstate;
}

/*
  * 是否需要密切关注正在删除的对象？
  *
  * 这很有用，因为启用它们会带来开销。
 */
bool
trackDroppedObjectsNeeded(void)
{
	/*
	  * 若存在任何 sql_drop、table_rewrite、ddl_command_end
	  * 事件触发器则返回 true
	 */
	return (EventCacheLookup(EVT_SQLDrop) != NIL) ||
		(EventCacheLookup(EVT_TableRewrite) != NIL) ||
		(EventCacheLookup(EVT_DDLCommandEnd) != NIL);
}

/*
  * 支持事件触发器函数中的已删除对象信息。
  *
  * 在当前状态的 SQLDropList（含 SQLDropObject 项）中
  * 保存当前命令删除的对象列表。每次新命令开始，
  * 创建干净的 EventTriggerQueryState；删除对象的
  * 命令执行 dependency.c 的删除流程，填充当前状态的
  * SQLDropList；事件触发器被调用时可通过
  * pg_event_trigger_dropped_objects() 使用该列表。
  * 命令完成时，EventTriggerQueryState 被清除，
  * 恢复前一条命令的状态（无命令执行时当前状态为 NULL）。
  *
  * 所有这些支持了事件触发器函数"可重入地"
  * 删除对象的情况。
  *
 */

/*
  * 将一个对象注册为被当前命令删除。
 */
void
EventTriggerSQLDropAddObject(const ObjectAddress *object, bool original, bool normal)
{
	SQLDropObject *obj;
	MemoryContext oldcxt;

	if (!currentEventTriggerState)
		return;

	Assert(EventTriggerSupportsObjectClass(getObjectClass(object)));

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	obj = palloc0(sizeof(SQLDropObject));
	obj->address = *object;
	obj->original = original;
	obj->normal = normal;

	if (object->classId == NamespaceRelationId)
	{
		/* 临时命名空间需要特殊处理           */
		if (isTempNamespace(object->objectId))
			obj->istemp = true;
		else if (isAnyTempNamespace(object->objectId))
		{
			/* 除自身外不报告临时模式       */
			pfree(obj);
			MemoryContextSwitchTo(oldcxt);
			return;
		}
		obj->objname = get_namespace_name(object->objectId);
	}
	else if (object->classId == AttrDefaultRelationId)
	{
		/* 如果其表为临时表，则列默认值视为临时 */
		ObjectAddress colobject;

		colobject = GetAttrDefaultColumnAddress(object->objectId);
		if (OidIsValid(colobject.objectId))
		{
			if (!obtain_object_name_namespace(&colobject, obj))
			{
				pfree(obj);
				MemoryContextSwitchTo(oldcxt);
				return;
			}
		}
	}
	else if (object->classId == TriggerRelationId)
	{
		/* 同样，若表为临时表则触发器为临时  */
		/* 遗憾的是，lsyscache.c 不支持触发器对象       */
		Relation	pg_trigger_rel;
		ScanKeyData skey[1];
		SysScanDesc sscan;
		HeapTuple	tuple;
		Oid			relid;

		/* 用笨办法获取触发器的表 OID      */
		pg_trigger_rel = table_open(TriggerRelationId, AccessShareLock);
		ScanKeyInit(&skey[0],
					Anum_pg_trigger_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(object->objectId));
		sscan = systable_beginscan(pg_trigger_rel, TriggerOidIndexId, true,
								   NULL, 1, skey);
		tuple = systable_getnext(sscan);
		if (HeapTupleIsValid(tuple))
			relid = ((Form_pg_trigger) GETSTRUCT(tuple))->tgrelid;
		else
			relid = InvalidOid; /* shouldn't happen */
		systable_endscan(sscan);
		table_close(pg_trigger_rel, AccessShareLock);
		/* 未找到触发器则不做任何事     */
		if (OidIsValid(relid))
		{
			ObjectAddress relobject;

			relobject.classId = RelationRelationId;
			relobject.objectId = relid;
			/* 任意地将 objectSubId 设为非零，以免填充 objname  */
			relobject.objectSubId = 1;
			if (!obtain_object_name_namespace(&relobject, obj))
			{
				pfree(obj);
				MemoryContextSwitchTo(oldcxt);
				return;
			}
		}
	}
	else if (object->classId == PolicyRelationId)
	{
		/* 同样，若表为临时表则策略为临时    */
		/* 遗憾的是，lsyscache.c 不支持策略对象         */
		Relation	pg_policy_rel;
		ScanKeyData skey[1];
		SysScanDesc sscan;
		HeapTuple	tuple;
		Oid			relid;

		/* 用笨办法获取策略的表 OID        */
		pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);
		ScanKeyInit(&skey[0],
					Anum_pg_policy_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(object->objectId));
		sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true,
								   NULL, 1, skey);
		tuple = systable_getnext(sscan);
		if (HeapTupleIsValid(tuple))
			relid = ((Form_pg_policy) GETSTRUCT(tuple))->polrelid;
		else
			relid = InvalidOid; /* shouldn't happen */
		systable_endscan(sscan);
		table_close(pg_policy_rel, AccessShareLock);
		/* 未找到策略则不做任何事       */
		if (OidIsValid(relid))
		{
			ObjectAddress relobject;

			relobject.classId = RelationRelationId;
			relobject.objectId = relid;
			/* 任意地将 objectSubId 设为非零，以免填充 objname  */
			relobject.objectSubId = 1;
			if (!obtain_object_name_namespace(&relobject, obj))
			{
				pfree(obj);
				MemoryContextSwitchTo(oldcxt);
				return;
			}
		}
	}
	else
	{
		/* 对所有其他对象类的通用处理       */
		if (!obtain_object_name_namespace(object, obj))
		{
			/* 除自身外不报告临时对象       */
			pfree(obj);
			MemoryContextSwitchTo(oldcxt);
			return;
		}
	}

	/* 对象标识、objname 和 objargs */
	obj->objidentity =
		getObjectIdentityParts(&obj->address, &obj->addrnames, &obj->addrargs,
							   false);

	/* 对象类型 */
	obj->objecttype = getObjectTypeDescription(&obj->address, false);

	slist_push_head(&(currentEventTriggerState->SQLDropList), &obj->next);

	MemoryContextSwitchTo(oldcxt);
}

/*
  * 基于对象填充 obj->objname、obj->schemaname 和 obj->istemp。
  *
  * 若此对象应被报告返回 true，若应被忽略（因为是
  * 其他会话的临时对象）返回 false。
 */
static bool
obtain_object_name_namespace(const ObjectAddress *object, SQLDropObject *obj)
{
	/*
	  * 从对象目录元组中获取模式名（若存在）；
	  * 这让我们跳过临时模式中的对象。信任
	  * ObjectProperty 包含所有可按模式限定的对象类。
	  *
	  * 目前此函数对不在 ObjectProperty 中的对象类
	  * 不做任何处理，但以后可能会单独处理它们。
	  *
	 */
	if (is_objectclass_supported(object->classId))
	{
		Relation	catalog;
		HeapTuple	tuple;

		catalog = table_open(object->classId, AccessShareLock);
		tuple = get_catalog_object_by_oid(catalog,
										  get_object_attnum_oid(object->classId),
										  object->objectId);

		if (tuple)
		{
			AttrNumber	attnum;
			Datum		datum;
			bool		isnull;

			attnum = get_object_attnum_namespace(object->classId);
			if (attnum != InvalidAttrNumber)
			{
				datum = heap_getattr(tuple, attnum,
									 RelationGetDescr(catalog), &isnull);
				if (!isnull)
				{
					Oid			namespaceId;

					namespaceId = DatumGetObjectId(datum);
					/* 临时对象仅在其属于自己时才被报告  */
					if (isTempNamespace(namespaceId))
					{
						obj->schemaname = "pg_temp";
						obj->istemp = true;
					}
					else if (isAnyTempNamespace(namespaceId))
					{
						/* 无须填充 *obj 的任何字段  */
						table_close(catalog, AccessShareLock);
						return false;
					}
					else
					{
						obj->schemaname = get_namespace_name(namespaceId);
						obj->istemp = false;
					}
				}
			}

			if (get_object_namensp_unique(object->classId) &&
				object->objectSubId == 0)
			{
				attnum = get_object_attnum_name(object->classId);
				if (attnum != InvalidAttrNumber)
				{
					datum = heap_getattr(tuple, attnum,
										 RelationGetDescr(catalog), &isnull);
					if (!isnull)
						obj->objname = pstrdup(NameStr(*DatumGetName(datum)));
				}
			}
		}

		table_close(catalog, AccessShareLock);
	}

	return true;
}

/*
  * pg_event_trigger_dropped_objects
  *
  * 将已删除对象列表提供给事件触发器运行的用户函数。
  *
 */
Datum
pg_event_trigger_dropped_objects(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	slist_iter	iter;

	/*
	  * 保护此函数不被越权调用
	 */
	if (!currentEventTriggerState ||
		!currentEventTriggerState->in_sql_drop)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a sql_drop event trigger function",
						"pg_event_trigger_dropped_objects()")));

	/* 构建元组存储以保存结果行 */
	InitMaterializedSRF(fcinfo, 0);

	slist_foreach(iter, &(currentEventTriggerState->SQLDropList))
	{
		SQLDropObject *obj;
		int			i = 0;
		Datum		values[12] = {0};
		bool		nulls[12] = {0};

		obj = slist_container(SQLDropObject, next, iter.cur);

		/* classid */
		values[i++] = ObjectIdGetDatum(obj->address.classId);

		/* objid */
		values[i++] = ObjectIdGetDatum(obj->address.objectId);

		/* objsubid */
		values[i++] = Int32GetDatum(obj->address.objectSubId);

		/* original */
		values[i++] = BoolGetDatum(obj->original);

		/* normal */
		values[i++] = BoolGetDatum(obj->normal);

		/* is_temporary */
		values[i++] = BoolGetDatum(obj->istemp);

		/* object_type */
		values[i++] = CStringGetTextDatum(obj->objecttype);

		/* schema_name */
		if (obj->schemaname)
			values[i++] = CStringGetTextDatum(obj->schemaname);
		else
			nulls[i++] = true;

		/* object_name */
		if (obj->objname)
			values[i++] = CStringGetTextDatum(obj->objname);
		else
			nulls[i++] = true;

		/* object_identity */
		if (obj->objidentity)
			values[i++] = CStringGetTextDatum(obj->objidentity);
		else
			nulls[i++] = true;

		/* address_names 和 address_args */
		if (obj->addrnames)
		{
			values[i++] = PointerGetDatum(strlist_to_textarray(obj->addrnames));

			if (obj->addrargs)
				values[i++] = PointerGetDatum(strlist_to_textarray(obj->addrargs));
			else
				values[i++] = PointerGetDatum(construct_empty_array(TEXTOID));
		}
		else
		{
			nulls[i++] = true;
			nulls[i++] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	return (Datum) 0;
}

/*
  * pg_event_trigger_table_rewrite_oid
  *
  * 将待重写表的 OID 提供给事件触发器运行的用户函数。
  *
 */
Datum
pg_event_trigger_table_rewrite_oid(PG_FUNCTION_ARGS)
{
	/*
	  * 保护此函数不被越权调用
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->table_rewrite_oid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a table_rewrite event trigger function",
						"pg_event_trigger_table_rewrite_oid()")));

	PG_RETURN_OID(currentEventTriggerState->table_rewrite_oid);
}

/*
  * pg_event_trigger_table_rewrite_reason
  *
  * 将重写原因提供给用户。
 */
Datum
pg_event_trigger_table_rewrite_reason(PG_FUNCTION_ARGS)
{
	/*
	  * 保护此函数不被越权调用
	 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->table_rewrite_reason == 0)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in a table_rewrite event trigger function",
						"pg_event_trigger_table_rewrite_reason()")));

	PG_RETURN_INT32(currentEventTriggerState->table_rewrite_reason);
}

/*-------------------------------------------------------------------------
 * 支持 DDL 命令逆解析       
 *
 * 以下例程使事件触发器函数能够获得正在执行的        
 *  DDL 命令的列表。此功能主要有三个部分                  
 * ：     
 *
 * 1) 在 ProcessUtilitySlow 或其某个子例程中，每个 DDL 命令     
 * 都会用下面的例程将自身的 struct CollectedCommand 表            
 * 示加入命令列表。 
 *
 * 2) 在那之后某个时刻，ddl_command_end 触发，命令列表        
 * 通过 pg_event_trigger_ddl_commands() 提供给事件触发                
 * 器函数；完整的命令细节以 pg_ddl_command 类型的列            
 *
 * 3) 扩展可以安装一个能够接受 pg_ddl_command 类型值        
 * 并将其转换成某种外部、用户可见和/或用户可修改    
 * 的表示形式。         
 *-------------------------------------------------------------------------
 */

/*
  * 禁止 DDL 命令收集。
 */
void
EventTriggerInhibitCommandCollection(void)
{
	if (!currentEventTriggerState)
		return;

	currentEventTriggerState->commandCollectionInhibited = true;
}

/*
  * 重新启用 DDL 命令收集。
 */
void
EventTriggerUndoInhibitCommandCollection(void)
{
	if (!currentEventTriggerState)
		return;

	currentEventTriggerState->commandCollectionInhibited = false;
}

/*
 * EventTriggerCollectSimpleCommand
 *		保存刚执行的简单 DDL 命令的数据               
 *
 * address 标识正被操作的对象。secondaryObject 是一个        
 * 与所执行命令以某种方式相关联的对象地址；其         
 * 含义因命令而异。    
 *
 * 例如，对于 ALTER obj SET SCHEMA 命令，objtype 是                
 * 被移动对象的类型，objectId 是其 OID，而 secondaryOid 是      
 * 旧模式的 OID。（目标模式的 OID 可由该对象的系统表     
 * 查得。）   
 */
void
EventTriggerCollectSimpleCommand(ObjectAddress address,
								 ObjectAddress secondaryObject,
								 Node *parsetree)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));

	command->type = SCT_Simple;
	command->in_extension = creating_extension;

	command->d.simple.address = address;
	command->d.simple.secondaryObject = secondaryObject;
	command->parsetree = copyObject(parsetree);

	currentEventTriggerState->commandList = lappend(currentEventTriggerState->commandList,
													command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerAlterTableStart
 *		准备接收即将执行的 ALTER TABLE 命令的数据               
 *
 * 注意我们不立即收集该命令；而是先将其保存在     
 *  currentCommand 中，只有当子命令处理完毕后，才会           
 * 添加到命令列表中。
 */
void
EventTriggerAlterTableStart(Node *parsetree)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));

	command->type = SCT_AlterTable;
	command->in_extension = creating_extension;

	command->d.alterTable.classId = RelationRelationId;
	command->d.alterTable.objectId = InvalidOid;
	command->d.alterTable.subcmds = NIL;
	command->parsetree = copyObject(parsetree);

	command->parent = currentEventTriggerState->currentCommand;
	currentEventTriggerState->currentCommand = command;

	MemoryContextSwitchTo(oldcxt);
}

/*
  * 记录受 ALTER TABLE 影响的对象的 OID。
  *
  * 必要因为某些情况下到稍后才知道 OID。
 */
void
EventTriggerAlterTableRelid(Oid objectId)
{
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	currentEventTriggerState->currentCommand->d.alterTable.objectId = objectId;
}

/*
 * EventTriggerCollectAlterTableSubcmd
 *		保存 ALTER TABLE 单个部分的数据。     
 *
 * 有几种不同的命令经过此路径，但除 ALTER TABLE               
 * 本身之外，其余都涉及内部生成的 AlterTableCmd 节点，     
 * 因此目前此代码只需处理这些即可。                       
 */
void
EventTriggerCollectAlterTableSubcmd(Node *subcmd, ObjectAddress address)
{
	MemoryContext oldcxt;
	CollectedATSubcmd *newsub;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	Assert(IsA(subcmd, AlterTableCmd));
	Assert(currentEventTriggerState->currentCommand != NULL);
	Assert(OidIsValid(currentEventTriggerState->currentCommand->d.alterTable.objectId));

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	newsub = palloc(sizeof(CollectedATSubcmd));
	newsub->address = address;
	newsub->parsetree = copyObject(subcmd);

	currentEventTriggerState->currentCommand->d.alterTable.subcmds =
		lappend(currentEventTriggerState->currentCommand->d.alterTable.subcmds, newsub);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerAlterTableEnd
 *		完成 ALTER TABLE 命令的保存，并加入命令列表。       
 *
 * FIXME 此 API 未考虑到事务/子事务在半途中止的可         
 * 能性。最好添加一个                           
 *  AtEOSubXact_EventTriggers() 修复。  
 */
void
EventTriggerAlterTableEnd(void)
{
	CollectedCommand *parent;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	parent = currentEventTriggerState->currentCommand->parent;

	/* 若无子命令则不收集 */
	if (currentEventTriggerState->currentCommand->d.alterTable.subcmds != NIL)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

		currentEventTriggerState->commandList =
			lappend(currentEventTriggerState->commandList,
					currentEventTriggerState->currentCommand);

		MemoryContextSwitchTo(oldcxt);
	}
	else
		pfree(currentEventTriggerState->currentCommand);

	currentEventTriggerState->currentCommand = parent;
}

/*
 * EventTriggerCollectGrant
 *		保存正在执行的 GRANT/REVOKE 命令的数据   
 *
 * 此函数创建 InternalGrant 的一个副本，因为原件可能      
 * 没有正确生命周期。 
 */
void
EventTriggerCollectGrant(InternalGrant *istmt)
{
	MemoryContext oldcxt;
	CollectedCommand *command;
	InternalGrant *icopy;
	ListCell   *cell;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	/*
	  * 繁琐，但必要。
	 */
	icopy = palloc(sizeof(InternalGrant));
	memcpy(icopy, istmt, sizeof(InternalGrant));
	icopy->objects = list_copy(istmt->objects);
	icopy->grantees = list_copy(istmt->grantees);
	icopy->col_privs = NIL;
	foreach(cell, istmt->col_privs)
		icopy->col_privs = lappend(icopy->col_privs, copyObject(lfirst(cell)));

	/* 用复制的 InternalGrant 收集 */
	command = palloc(sizeof(CollectedCommand));
	command->type = SCT_Grant;
	command->in_extension = creating_extension;
	command->d.grant.istmt = icopy;
	command->parsetree = NULL;

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterOpFam
 *		保存有关 ALTER OPERATOR FAMILY ADD/DROP 命令执行的    
 *		数据  
 */
void
EventTriggerCollectAlterOpFam(AlterOpFamilyStmt *stmt, Oid opfamoid,
							  List *operators, List *procedures)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc(sizeof(CollectedCommand));
	command->type = SCT_AlterOpFamily;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.opfam.address,
					 OperatorFamilyRelationId, opfamoid);
	command->d.opfam.operators = operators;
	command->d.opfam.procedures = procedures;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectCreateOpClass
 *		保存正在执行的 CREATE OPERATOR CLASS 命令的数据   
 */
void
EventTriggerCollectCreateOpClass(CreateOpClassStmt *stmt, Oid opcoid,
								 List *operators, List *procedures)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_CreateOpClass;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.createopc.address,
					 OperatorClassRelationId, opcoid);
	command->d.createopc.operators = operators;
	command->d.createopc.procedures = procedures;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterTSConfig
 *		保存有关 ALTER TEXT SEARCH CONFIGURATION 命令执行的    
 *		数据  
 */
void
EventTriggerCollectAlterTSConfig(AlterTSConfigurationStmt *stmt, Oid cfgId,
								 Oid *dictIds, int ndicts)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_AlterTSConfig;
	command->in_extension = creating_extension;
	ObjectAddressSet(command->d.atscfg.address,
					 TSConfigRelationId, cfgId);
	command->d.atscfg.dictIds = palloc(sizeof(Oid) * ndicts);
	memcpy(command->d.atscfg.dictIds, dictIds, sizeof(Oid) * ndicts);
	command->d.atscfg.ndicts = ndicts;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * EventTriggerCollectAlterDefPrivs
 *		保存有关 ALTER DEFAULT PRIVILEGES 命令执行的    
 *		数据  
 */
void
EventTriggerCollectAlterDefPrivs(AlterDefaultPrivilegesStmt *stmt)
{
	MemoryContext oldcxt;
	CollectedCommand *command;

	/* 若事件触发器上下文未设置或收集禁用则忽略 */
	if (!currentEventTriggerState ||
		currentEventTriggerState->commandCollectionInhibited)
		return;

	oldcxt = MemoryContextSwitchTo(currentEventTriggerState->cxt);

	command = palloc0(sizeof(CollectedCommand));
	command->type = SCT_AlterDefaultPrivileges;
	command->d.defprivs.objtype = stmt->action->objtype;
	command->in_extension = creating_extension;
	command->parsetree = (Node *) copyObject(stmt);

	currentEventTriggerState->commandList =
		lappend(currentEventTriggerState->commandList, command);
	MemoryContextSwitchTo(oldcxt);
}

/*
  * 在 ddl_command_end 事件触发器中，此函数报告正在运行的
  * DDL 命令。
 */
Datum
pg_event_trigger_ddl_commands(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ListCell   *lc;

	/*
	  * 保护此函数不被越权调用
	 */
	if (!currentEventTriggerState)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_EVENT_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("%s can only be called in an event trigger function",
						"pg_event_trigger_ddl_commands()")));

	/* 构建元组存储以保存结果行 */
	InitMaterializedSRF(fcinfo, 0);

	foreach(lc, currentEventTriggerState->commandList)
	{
		CollectedCommand *cmd = lfirst(lc);
		Datum		values[9];
		bool		nulls[9] = {0};
		ObjectAddress addr;
		int			i = 0;

		/*
		  * 对于 IF NOT EXISTS 尝试创建已存在对象的命令，
		  * 返回的 OID 为 Invalid。不返回任何内容。
		  *
		  * 可能的替代方案是查找已存在对象的 OID
		  * 并用它运行逆解析。但解析树可能
		  * 与最初创建对象的不同，可能无论如何
		  * 都无法达到一致状态。
		  *
		 */
		if (cmd->type == SCT_Simple &&
			!OidIsValid(cmd->d.simple.address.objectId))
			continue;

		switch (cmd->type)
		{
			case SCT_Simple:
			case SCT_AlterTable:
			case SCT_AlterOpFamily:
			case SCT_CreateOpClass:
			case SCT_AlterTSConfig:
				{
					char	   *identity;
					char	   *type;
					char	   *schema = NULL;

					if (cmd->type == SCT_Simple)
						addr = cmd->d.simple.address;
					else if (cmd->type == SCT_AlterTable)
						ObjectAddressSet(addr,
										 cmd->d.alterTable.classId,
										 cmd->d.alterTable.objectId);
					else if (cmd->type == SCT_AlterOpFamily)
						addr = cmd->d.opfam.address;
					else if (cmd->type == SCT_CreateOpClass)
						addr = cmd->d.createopc.address;
					else if (cmd->type == SCT_AlterTSConfig)
						addr = cmd->d.atscfg.address;

					/*
					 * 若对象在同一命令中被删除，我们可能会 
					 * 陷入生成了消息却无法再查找对象信息   
					 * 的境地，因此跳过它而不是失败。        
					 * 例如，某些 ALTER TABLE 子命令组合           
					 * 就会出现这种情况。                 
					 */
					identity = getObjectIdentity(&addr, true);
					if (identity == NULL)
						continue;

					/* 该类型永不为 NULL。  */
					type = getObjectTypeDescription(&addr, true);

					/*
					  * 获取模式名（若为 temp 对象则为 "pg_temp"）。
					  * 若对象类不在支持列表中，假设它是无模式的对象类型，
					  * 因此 "schema" 保持 NULL。
					  *
					 */
					if (is_objectclass_supported(addr.classId))
					{
						AttrNumber	nspAttnum;

						nspAttnum = get_object_attnum_namespace(addr.classId);
						if (nspAttnum != InvalidAttrNumber)
						{
							Relation	catalog;
							HeapTuple	objtup;
							Oid			schema_oid;
							bool		isnull;

							catalog = table_open(addr.classId, AccessShareLock);
							objtup = get_catalog_object_by_oid(catalog,
															   get_object_attnum_oid(addr.classId),
															   addr.objectId);
							if (!HeapTupleIsValid(objtup))
								elog(ERROR, "cache lookup failed for object %u/%u",
									 addr.classId, addr.objectId);
							schema_oid =
								heap_getattr(objtup, nspAttnum,
											 RelationGetDescr(catalog), &isnull);
							if (isnull)
								elog(ERROR,
									 "invalid null namespace in object %u/%u/%d",
									 addr.classId, addr.objectId, addr.objectSubId);
							schema = get_namespace_name_or_temp(schema_oid);

							table_close(catalog, AccessShareLock);
						}
					}

					/* classid */
					values[i++] = ObjectIdGetDatum(addr.classId);
					/* objid */
					values[i++] = ObjectIdGetDatum(addr.objectId);
					/* objsubid */
					values[i++] = Int32GetDatum(addr.objectSubId);
					/* command tag */
					values[i++] = CStringGetTextDatum(CreateCommandName(cmd->parsetree));
					/* object_type */
					values[i++] = CStringGetTextDatum(type);
					/* schema */
					if (schema == NULL)
						nulls[i++] = true;
					else
						values[i++] = CStringGetTextDatum(schema);
					/* identity */
					values[i++] = CStringGetTextDatum(identity);
					/* in_extension */
					values[i++] = BoolGetDatum(cmd->in_extension);
					/* command */
					values[i++] = PointerGetDatum(cmd);
				}
				break;

			case SCT_AlterDefaultPrivileges:
				/* classid */
				nulls[i++] = true;
				/* objid */
				nulls[i++] = true;
				/* objsubid */
				nulls[i++] = true;
				/* command tag */
				values[i++] = CStringGetTextDatum(CreateCommandName(cmd->parsetree));
				/* object_type */
				values[i++] = CStringGetTextDatum(stringify_adefprivs_objtype(cmd->d.defprivs.objtype));
				/* schema */
				nulls[i++] = true;
				/* identity */
				nulls[i++] = true;
				/* in_extension */
				values[i++] = BoolGetDatum(cmd->in_extension);
				/* command */
				values[i++] = PointerGetDatum(cmd);
				break;

			case SCT_Grant:
				/* classid */
				nulls[i++] = true;
				/* objid */
				nulls[i++] = true;
				/* objsubid */
				nulls[i++] = true;
				/* command tag */
				values[i++] = CStringGetTextDatum(cmd->d.grant.istmt->is_grant ?
												  "GRANT" : "REVOKE");
				/* object_type */
				values[i++] = CStringGetTextDatum(stringify_grant_objtype(cmd->d.grant.istmt->objtype));
				/* schema */
				nulls[i++] = true;
				/* identity */
				nulls[i++] = true;
				/* in_extension */
				values[i++] = BoolGetDatum(cmd->in_extension);
				/* command */
				values[i++] = PointerGetDatum(cmd);
				break;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	PG_RETURN_VOID();
}

/*
  * 以字符串形式返回 ObjectType，如其在 GRANT 和
  * REVOKE 命令中所用。
 */
static const char *
stringify_grant_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return "COLUMN";
		case OBJECT_TABLE:
			return "TABLE";
		case OBJECT_SEQUENCE:
			return "SEQUENCE";
		case OBJECT_DATABASE:
			return "DATABASE";
		case OBJECT_DOMAIN:
			return "DOMAIN";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPER";
		case OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVER";
		case OBJECT_FUNCTION:
			return "FUNCTION";
		case OBJECT_LANGUAGE:
			return "LANGUAGE";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECT";
		case OBJECT_SCHEMA:
			return "SCHEMA";
		case OBJECT_PARAMETER_ACL:
			return "PARAMETER";
		case OBJECT_PROCEDURE:
			return "PROCEDURE";
		case OBJECT_ROUTINE:
			return "ROUTINE";
		case OBJECT_TABLESPACE:
			return "TABLESPACE";
		case OBJECT_TYPE:
			return "TYPE";
			/* 这些目前未被使用    */
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROLE:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			elog(ERROR, "unsupported object type: %d", (int) objtype);
	}

	return "???";				/* keep compiler quiet */
}

/*
  * 返回 ObjectType 的字符串形式；与上面类似，但使用
  * ALTER DEFAULT PRIVILEGES 命令中的拼写。
  * 通常只是复数形式。
 */
static const char *
stringify_adefprivs_objtype(ObjectType objtype)
{
	switch (objtype)
	{
		case OBJECT_COLUMN:
			return "COLUMNS";
		case OBJECT_TABLE:
			return "TABLES";
		case OBJECT_SEQUENCE:
			return "SEQUENCES";
		case OBJECT_DATABASE:
			return "DATABASES";
		case OBJECT_DOMAIN:
			return "DOMAINS";
		case OBJECT_FDW:
			return "FOREIGN DATA WRAPPERS";
		case OBJECT_FOREIGN_SERVER:
			return "FOREIGN SERVERS";
		case OBJECT_FUNCTION:
			return "FUNCTIONS";
		case OBJECT_LANGUAGE:
			return "LANGUAGES";
		case OBJECT_LARGEOBJECT:
			return "LARGE OBJECTS";
		case OBJECT_SCHEMA:
			return "SCHEMAS";
		case OBJECT_PROCEDURE:
			return "PROCEDURES";
		case OBJECT_ROUTINE:
			return "ROUTINES";
		case OBJECT_TABLESPACE:
			return "TABLESPACES";
		case OBJECT_TYPE:
			return "TYPES";
			/* 这些目前未被使用    */
		case OBJECT_ACCESS_METHOD:
		case OBJECT_AGGREGATE:
		case OBJECT_AMOP:
		case OBJECT_AMPROC:
		case OBJECT_ATTRIBUTE:
		case OBJECT_CAST:
		case OBJECT_COLLATION:
		case OBJECT_CONVERSION:
		case OBJECT_DEFAULT:
		case OBJECT_DEFACL:
		case OBJECT_DOMCONSTRAINT:
		case OBJECT_EVENT_TRIGGER:
		case OBJECT_EXTENSION:
		case OBJECT_FOREIGN_TABLE:
		case OBJECT_INDEX:
		case OBJECT_MATVIEW:
		case OBJECT_OPCLASS:
		case OBJECT_OPERATOR:
		case OBJECT_OPFAMILY:
		case OBJECT_PARAMETER_ACL:
		case OBJECT_POLICY:
		case OBJECT_PUBLICATION:
		case OBJECT_PUBLICATION_NAMESPACE:
		case OBJECT_PUBLICATION_REL:
		case OBJECT_ROLE:
		case OBJECT_RULE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_TRANSFORM:
		case OBJECT_TRIGGER:
		case OBJECT_TSCONFIGURATION:
		case OBJECT_TSDICTIONARY:
		case OBJECT_TSPARSER:
		case OBJECT_TSTEMPLATE:
		case OBJECT_USER_MAPPING:
		case OBJECT_VIEW:
			elog(ERROR, "unsupported object type: %d", (int) objtype);
	}

	return "???";				/* keep compiler quiet */
}
