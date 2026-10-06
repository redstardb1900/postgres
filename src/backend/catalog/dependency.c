/*-------------------------------------------------------------------------
 *
 * dependency.c
 *	  支持对象间依赖的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/dependency.c
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
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/policy.h"
#include "commands/publicationcmds.h"
#include "commands/seclabel.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteRemove.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/*
 * 删除处理需要为每个计划删除的 ObjectAddress 保存额外状态。
 * 为简单并便于代码共享，我们让 ObjectAddresses 同时支持
 * 带或不带该额外状态的数组。
 */
typedef struct
{
	int			flags;			/* 位掩码，见下方的位定义 */
	ObjectAddress dependee;		/* 其删除迫使删除本对象的那个对象 */
} ObjectAddressExtra;

/* ObjectAddressExtra 标志位 */
#define DEPFLAG_ORIGINAL	0x0001	/* 原始删除目标 */
#define DEPFLAG_NORMAL		0x0002	/* 经由普通依赖到达 */
#define DEPFLAG_AUTO		0x0004	/* 经由自动依赖到达 */
#define DEPFLAG_INTERNAL	0x0008	/* 经由内部依赖到达 */
#define DEPFLAG_PARTITION	0x0010	/* 经由分区依赖到达 */
#define DEPFLAG_EXTENSION	0x0020	/* 经由扩展依赖到达 */
#define DEPFLAG_REVERSE		0x0040	/* 反向的内部/扩展链接 */
#define DEPFLAG_IS_PART		0x0080	/* 具有分区依赖 */
#define DEPFLAG_SUBOBJECT	0x0100	/* 另一可删除对象的子对象 */


/* 可扩展的 ObjectAddresses 列表 */
struct ObjectAddresses
{
	ObjectAddress *refs;		/* => palloc 得到的数组 */
	ObjectAddressExtra *extras; /* => palloc 得到的数组，未使用则为 NULL */
	int			numrefs;		/* 当前引用个数 */
	int			maxrefs;		/* 当前 palloc'd array 的大小 */
};

/* typedef ObjectAddresses 出现在 dependency.h */

/* 串起的 ObjectAddresses 列表，用于检测递归 */
typedef struct ObjectAddressStack
{
	const ObjectAddress *object;	/* 正在访问的对象 */
	int			flags;			/* 其当前标志位 */
	struct ObjectAddressStack *next;	/* 外一层栈 */
} ObjectAddressStack;

/* findDependentObjects 中的临时存储 */
typedef struct
{
	ObjectAddress obj;			/* 待删除对象 --- MUST BE FIRST */
	int			subflags;		/* 递归到 obj 时向下传递的标志 */
} ObjectAddressAndFlags;

/* 供 find_expr_references_walker 使用 */
typedef struct
{
	ObjectAddresses *addrs;		/* 正在累积的地址 */
	List	   *rtables;		/* 用于解析 Var 的范围表列表 */
} find_expr_references_context;

/*
 * 本常量表把 ObjectClasses 映射到对应的目录 OIDs。
 * 另见 getObjectClass()。
 */
static const Oid object_classes[] = {
	RelationRelationId,			/* OCLASS_CLASS */
	ProcedureRelationId,		/* OCLASS_PROC */
	TypeRelationId,				/* OCLASS_TYPE */
	CastRelationId,				/* OCLASS_CAST */
	CollationRelationId,		/* OCLASS_COLLATION */
	ConstraintRelationId,		/* OCLASS_CONSTRAINT */
	ConversionRelationId,		/* OCLASS_CONVERSION */
	AttrDefaultRelationId,		/* OCLASS_DEFAULT */
	LanguageRelationId,			/* OCLASS_LANGUAGE */
	LargeObjectRelationId,		/* OCLASS_LARGEOBJECT */
	OperatorRelationId,			/* OCLASS_OPERATOR */
	OperatorClassRelationId,	/* OCLASS_OPCLASS */
	OperatorFamilyRelationId,	/* OCLASS_OPFAMILY */
	AccessMethodRelationId,		/* OCLASS_AM */
	AccessMethodOperatorRelationId, /* OCLASS_AMOP */
	AccessMethodProcedureRelationId,	/* OCLASS_AMPROC */
	RewriteRelationId,			/* OCLASS_REWRITE */
	TriggerRelationId,			/* OCLASS_TRIGGER */
	NamespaceRelationId,		/* OCLASS_SCHEMA */
	StatisticExtRelationId,		/* OCLASS_STATISTIC_EXT */
	TSParserRelationId,			/* OCLASS_TSPARSER */
	TSDictionaryRelationId,		/* OCLASS_TSDICT */
	TSTemplateRelationId,		/* OCLASS_TSTEMPLATE */
	TSConfigRelationId,			/* OCLASS_TSCONFIG */
	AuthIdRelationId,			/* OCLASS_ROLE */
	AuthMemRelationId,			/* OCLASS_ROLE_MEMBERSHIP */
	DatabaseRelationId,			/* OCLASS_DATABASE */
	TableSpaceRelationId,		/* OCLASS_TBLSPACE */
	ForeignDataWrapperRelationId,	/* OCLASS_FDW */
	ForeignServerRelationId,	/* OCLASS_FOREIGN_SERVER */
	UserMappingRelationId,		/* OCLASS_USER_MAPPING */
	DefaultAclRelationId,		/* OCLASS_DEFACL */
	ExtensionRelationId,		/* OCLASS_EXTENSION */
	EventTriggerRelationId,		/* OCLASS_EVENT_TRIGGER */
	ParameterAclRelationId,		/* OCLASS_PARAMETER_ACL */
	PolicyRelationId,			/* OCLASS_POLICY */
	PublicationNamespaceRelationId, /* OCLASS_PUBLICATION_NAMESPACE */
	PublicationRelationId,		/* OCLASS_PUBLICATION */
	PublicationRelRelationId,	/* OCLASS_PUBLICATION_REL */
	SubscriptionRelationId,		/* OCLASS_SUBSCRIPTION */
	TransformRelationId			/* OCLASS_TRANSFORM */
};

/*
 * 确保 object_classes 与 ObjectClass 枚举保持同步。
 */
StaticAssertDecl(lengthof(object_classes) == LAST_OCLASS + 1,
				 "object_classes[] must cover all ObjectClasses");


static void findDependentObjects(const ObjectAddress *object,
								 int objflags,
								 int flags,
								 ObjectAddressStack *stack,
								 ObjectAddresses *targetObjects,
								 const ObjectAddresses *pendingObjects,
								 Relation *depRel);
static void reportDependentObjects(const ObjectAddresses *targetObjects,
								   DropBehavior behavior,
								   int flags,
								   const ObjectAddress *origObject);
static void deleteOneObject(const ObjectAddress *object,
							Relation *depRel, int32 flags);
static void doDeletion(const ObjectAddress *object, int flags);
static bool find_expr_references_walker(Node *node,
										find_expr_references_context *context);
static void process_function_rte_ref(RangeTblEntry *rte, AttrNumber attnum,
									 find_expr_references_context *context);
static void eliminate_duplicate_dependencies(ObjectAddresses *addrs);
static int	object_address_comparator(const void *a, const void *b);
static void add_object_address(ObjectClass oclass, Oid objectId, int32 subId,
							   ObjectAddresses *addrs);
static void add_exact_object_address_extra(const ObjectAddress *object,
										   const ObjectAddressExtra *extra,
										   ObjectAddresses *addrs);
static bool object_address_present_add_flags(const ObjectAddress *object,
											 int flags,
											 ObjectAddresses *addrs);
static bool stack_address_present_add_flags(const ObjectAddress *object,
											int flags,
											ObjectAddressStack *stack);
static void DeleteInitPrivs(const ObjectAddress *object);


/*
 * 遍历给定对象，对其执行收尾动作，并执行
 * 实际删除。
 */
static void
deleteObjectsInList(ObjectAddresses *targetObjects, Relation *depRel,
					int flags)
{
	int			i;

	/*
	 * 若有必要，为事件触发器跟踪这些对象。
	 */
	if (trackDroppedObjectsNeeded() && !(flags & PERFORM_DELETION_INTERNAL))
	{
		for (i = 0; i < targetObjects->numrefs; i++)
		{
			const ObjectAddress *thisobj = &targetObjects->refs[i];
			const ObjectAddressExtra *extra = &targetObjects->extras[i];
			bool		original = false;
			bool		normal = false;

			if (extra->flags & DEPFLAG_ORIGINAL)
				original = true;
			if (extra->flags & DEPFLAG_NORMAL)
				normal = true;
			if (extra->flags & DEPFLAG_REVERSE)
				normal = true;

			if (EventTriggerSupportsObjectClass(getObjectClass(thisobj)))
			{
				EventTriggerSQLDropAddObject(thisobj, original, normal);
			}
		}
	}

	/*
	 * 按正确顺序删除全部对象，但若被指示，则
	 * 应跳过原始 object(s)。
	 */
	for (i = 0; i < targetObjects->numrefs; i++)
	{
		ObjectAddress *thisobj = targetObjects->refs + i;
		ObjectAddressExtra *thisextra = targetObjects->extras + i;

		if ((flags & PERFORM_DELETION_SKIP_ORIGINAL) &&
			(thisextra->flags & DEPFLAG_ORIGINAL))
			continue;

		deleteOneObject(thisobj, depRel, flags);
	}
}

/*
 * performDeletion: 尝试删除指定 object。若指定 CASCADE
 * 行为，则同时（递归地）删除任何依赖方对象。
 * 若指定 RESTRICT 行为，则在存在任何依赖方
 * 对象时报错，但按依赖类型本应隐式删除的那些
 * 对象除外。
 *
 * 这是所有会删除可参与依赖的对象的 DROP 形式的
 * 外层控制例程。注意 performMultipleDeletions
 * 是同一主题的变体；若你改动这里，很可能
 * 也需要修正那边。
 *
 * flags 参数中的位可以包括：
 *
 * PERFORM_DELETION_INTERNAL: 表示此次删除不是
 * 用户发起动作的直接结果。例如，清空临时
 * 模式以便新后端使用，或在添加新默认值时作为
 * 中间步骤删除列默认值，这些都是
 * 内部操作。另一方面，因用户对其发出 DROP
 * 语句而删除某物则不是内部操作。当前
 * 这会抑制调用事件触发器以及做某些权限检查。
 *
 * PERFORM_DELETION_CONCURRENTLY: 并发地执行删除。这目前
 * 除删除索引外对任何对象都无效；不要对
 * 其他对象类型设置它，否则可能得到奇怪结果。
 *
 * PERFORM_DELETION_QUIETLY: 把消息级别从 NOTICE 降到 DEBUG2。
 *
 * PERFORM_DELETION_SKIP_ORIGINAL: 不删除指定对象，
 * 只删除依赖它/它们的对象。
 *
 * PERFORM_DELETION_SKIP_EXTENSIONS: 即使
 * 删除属于某扩展的对象，也不删除扩展。这一般
 * 只应在删除临时对象时使用。
 *
 * PERFORM_DELETION_CONCURRENT_LOCK: 以普通方式删除，但持有
 * 如同并发删除的锁。供 REINDEX CONCURRENTLY 使用。
 *
 */
void
performDeletion(const ObjectAddress *object,
				DropBehavior behavior, int flags)
{
	Relation	depRel;
	ObjectAddresses *targetObjects;

	/*
	 * 只打开一次 pg_depend 并把
	 * Relation 指针向下传给所有递归删除步骤，以节省周期。
	 */
	depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * 在目标对象上获取删除锁。（理想情况下调用者已经
	 * 做过，但许多地方对此很马虎。）
	 */
	AcquireDeletionLock(object, 0);

	/*
	 * 构造待删除对象列表（即给定对象加上
	 * 所有直接或间接依赖它的对象）。
	 */
	targetObjects = new_object_addresses();

	findDependentObjects(object,
						 DEPFLAG_ORIGINAL,
						 flags,
						 NULL,	/* 空栈 */
						 targetObjects,
						 NULL,	/* 无 pendingObjects */
						 &depRel);

	/*
	 * 检查是否允许删除，并报告级联删除情况。
	 */
	reportDependentObjects(targetObjects,
						   behavior,
						   flags,
						   object);

	/* 动手执行 */
	deleteObjectsInList(targetObjects, &depRel, flags);

	/* 然后清理 */
	free_object_addresses(targetObjects);

	table_close(depRel, RowExclusiveLock);
}

/*
 * performMultipleDeletions: 类似于 performDeletion，但一次作用于多个
 * 对象。
 *
 * 与多次调用 performDeletion 的主要差别是：对每个要
 * 删除的对象而言，会被隐式删除的对象列表
 * 是所有对象的隐式对象列表之并。这
 * 使每项检查都更宽松。
 */
void
performMultipleDeletions(const ObjectAddresses *objects,
						 DropBehavior behavior, int flags)
{
	Relation	depRel;
	ObjectAddresses *targetObjects;
	int			i;

	/* 没有对象则无事可做... */
	if (objects->numrefs <= 0)
		return;

	/*
	 * 只打开一次 pg_depend 并把
	 * Relation 指针向下传给所有递归删除步骤，以节省周期。
	 */
	depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * 构造待删除对象列表（即给定对象加上
	 * 所有直接或间接依赖它们的对象）。注意
	 * 因为我们把整个 objects 列表作为 pendingObjects 上下文传入，
	 * 试图删除内部依赖于列表中另一对象的对象时
	 * 不会失败；我们只是跳过该对象，并在到达其所有者时
	 * 再删除它。
	 */
	targetObjects = new_object_addresses();

	for (i = 0; i < objects->numrefs; i++)
	{
		const ObjectAddress *thisobj = objects->refs + i;

		/*
		 * 在每个目标对象上获取删除锁。（理想情况下调用者
		 * 已经做过，但许多地方对此很马虎。）
		 */
		AcquireDeletionLock(thisobj, flags);

		findDependentObjects(thisobj,
							 DEPFLAG_ORIGINAL,
							 flags,
							 NULL,	/* 空栈 */
							 targetObjects,
							 objects,
							 &depRel);
	}

	/*
	 * 检查是否允许删除，并报告级联删除情况。
	 *
	 * 若恰好只删除一个对象，则以与
	 * performDeletion() 相同的方式报告，否则只能说得更含糊。
	 */
	reportDependentObjects(targetObjects,
						   behavior,
						   flags,
						   (objects->numrefs == 1 ? objects->refs : NULL));

	/* 动手执行 */
	deleteObjectsInList(targetObjects, &depRel, flags);

	/* 然后清理 */
	free_object_addresses(targetObjects);

	table_close(depRel, RowExclusiveLock);
}

/*
 * findDependentObjects - 找出所有依赖 'object' 的对象
 *
 * 对每个依赖起始对象的对象，获取删除
 * 锁，把它加入 targetObjects（若尚不在其中），
 * 并递归找出依赖它的对象。某对象的依赖
 * 会排在该对象自身之前放入 targetObjects；这意味着
 * 完成后的列表顺序就是安全的删除顺序。
 *
 * 调用者必须已经持有 'object' 自身上的删除锁，
 * 但不得已把它加入 targetObjects。（注意：存在边角
 * 情况我们也不会加入该对象，并且还会释放
 * 调用者已获取的锁。这有点难看，但 API 这样设计
 * 是为了在加锁后能方便地复查对象是否仍存活。见
 * 函数内部的说明。）
 *
 * 删除整个对象（subId = 0）时，我们也查找
 * 其子对象的依赖。
 *
 *	object: 要加入 targetObjects 并查找依赖它的对象
 *	objflags: 要 ORed 进该对象 targetObjects 条目的标志
 *	flags: 整个删除操作的 PERFORM_DELETION_xxx 标志
 *	stack: 当前递归中正在访问的对象列表；最顶项
 *			是我们从中递归而来的对象（外部调用者为 NULL）
 *	targetObjects: 已安排删除的对象列表
 *	pendingObjects: 其他预定销毁、但
 *			不一定已在 targetObjects 中的对象列表（没有则为 NULL）
 *	*depRel: 已经打开的 pg_depend 关系
 *
 * 注意：objflags 描述此刻访问本对象的原因，
 * 递归时不向下传递。flags 参数
 * 会向下传递，因为它描述我们总体上在做什么。
 */
static void
findDependentObjects(const ObjectAddress *object,
					 int objflags,
					 int flags,
					 ObjectAddressStack *stack,
					 ObjectAddresses *targetObjects,
					 const ObjectAddresses *pendingObjects,
					 Relation *depRel)
{
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress otherObject;
	ObjectAddress owningObject;
	ObjectAddress partitionObject;
	ObjectAddressAndFlags *dependentObjects;
	int			numDependentObjects;
	int			maxDependentObjects;
	ObjectAddressStack mystack;
	ObjectAddressExtra extra;

	/*
	 * 若目标对象已在外层递归
	 * 中被访问，只需把当前 objflags 回报给那一层然后退出。
	 * 这是为了在循环依赖面前避免无限递归。
	 *
	 * 仅靠栈检查会把依赖环在
	 * 任意点断开，即环中第一个被
	 * 访问的成员对象会最后删除。这显然行不通。
	 * 不过，下面的内部依赖检查保证我们
	 * 不会在内部依赖处断开环：若从
	 * "被拥有" 对象进入环，我们会改从 "拥有方" 对象
	 * 开始。若有必要，大概也能拼凑点东西来避免在
	 * 自动依赖处断开。不过没有已知情形
	 * 需要那样做。
	 *
	 */
	if (stack_address_present_add_flags(object, objflags, stack))
		return;

	/*
	 * 由于本函数会递归，可能把栈撑爆，
	 * 原因是依赖树很深，而不只是依赖环。
	 */
	check_stack_depth();

	/*
	 * 目标对象也可能已经被完全
	 * 处理并放入 targetObjects。若是，同样只需把
	 * 指定的 objflags 加到其条目上然后返回。
	 *
	 * （注意：在这些提前退出的情形我们可以释放调用者取得的
	 * 锁，因为该对象大概已被多次加锁；但
	 * 似乎不值得为此花费周期。）
	 */
	if (object_address_present_add_flags(object, objflags, targetObjects))
		return;

	/*
	 * 若目标对象被钉住，我们可以立即报错；它
	 * 不会有任何对象被记录为依赖它。
	 */
	if (IsPinnedObject(object->classId, object->objectId))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop %s because it is required by the database system",
						getObjectDescription(object, false))));

	/*
	 * 目标对象可能内部依赖于某个其他对象
	 * （其"所有者"），和/或是某扩展的成员（也被视为其
	 * 所有者）。若是，且我们并非从拥有方对象递归而来，就必须
	 * 把本次删除请求转换成对
	 * 拥有方对象的删除请求。（我们最终会递归回本对象，但
	 * 拥有方对象必须先被访问，这样它才会后删除。）
	 * 查明这一点的办法是扫描显示
	 * 本对象依赖于什么的 pg_depend 条目。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		/* 只考虑本子对象的依赖 */
		ScanKeyInit(&key[2],
					Anum_pg_depend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
	{
		/* 考虑本对象及其任何子对象的依赖 */
		nkeys = 2;
	}

	scan = systable_beginscan(*depRel, DependDependerIndexId, true,
							  NULL, nkeys, key);

	/* 初始化循环可能会填入的变量 */
	memset(&owningObject, 0, sizeof(owningObject));
	memset(&partitionObject, 0, sizeof(partitionObject));

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		otherObject.classId = foundDep->refclassid;
		otherObject.objectId = foundDep->refobjid;
		otherObject.objectSubId = foundDep->refobjsubid;

		/*
		 * 扫描整个对象的依赖时，可能发现把
		 * 该对象的子对象链到对象自身的行。（通常
		 * 这种依赖是隐式的，但在涉及分区的某些情形
		 * 必须做成显式。）我们必须忽略这类行，以免
		 * 无限递归。
		 */
		if (otherObject.classId == object->classId &&
			otherObject.objectId == object->objectId &&
			object->objectSubId == 0)
			continue;

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
			case DEPENDENCY_AUTO:
			case DEPENDENCY_AUTO_EXTENSION:
				/* 没问题 */
				break;

			case DEPENDENCY_EXTENSION:

				/*
				 * 若被指示，则完全忽略 EXTENSION 依赖。此
				 * 标志通常用于在临时对象清理期间防止删除扩展，
				 * 即使某个临时对象是在
				 * 扩展脚本中创建的。
				 */
				if (flags & PERFORM_DELETION_SKIP_EXTENSIONS)
					break;

				/*
				 * 若另一对象正是当前正在
				 * 创建/修改的扩展，则忽略此依赖并继续
				 * 删除。这允许在扩展的脚本中删除该扩展的
				 * 对象，以及诸如删除在
				 * 这类脚本中创建的临时对象之类的边角
				 * 情况。
				 */
				if (creating_extension &&
					otherObject.classId == ExtensionRelationId &&
					otherObject.objectId == CurrentExtensionObject)
					break;

				/* 否则，按内部依赖处理 */
				/* FALL THRU */

			case DEPENDENCY_INTERNAL:

				/*
				 * 本对象是另一对象内部实现的一部分，
				 * 或是作为另一对象的那个扩展的一部分。
				 * 有三种情形：
				 *
				 * 1. 在最外层递归级，我们必须禁止
				 * DROP。不过，若拥有方对象列在
				 * pendingObjects 中，只需释放调用者的锁并返回；
				 * 当我们到达待处理列表中的那条
				 * 时最终会完成 DROP。
				 *
				 * 注意：上述说法仅当这条 pg_depend
				 * 条目届时仍存在时才成立；因此原则上我们
				 * 可能漏删用户要求删除的项。
				 * 不过不会造成不一致：因为我们在最外
				 * 层，没有对象依赖本对象。
				 */
				if (stack == NULL)
				{
					if (pendingObjects &&
						object_address_present(&otherObject, pendingObjects))
					{
						systable_endscan(scan);
						/* 需要释放调用者的锁；见下方说明 */
						ReleaseDeletionLock(object);
						return;
					}

					/*
					 * 我们推迟到本循环之后才真正发出错误消息，
					 * 以便使行为
					 * 独立于 pg_depend 条目的顺序，至少
					 * 在 INTERNAL 和
					 * EXTENSION 依赖各不超过一条时如此。（若更多，我们会抱怨
					 * 其中随机一条。）优先抱怨
					 * EXTENSION，因为那通常是更重要的
					 * 依赖。
					 */
					if (!OidIsValid(owningObject.classId) ||
						foundDep->deptype == DEPENDENCY_EXTENSION)
						owningObject = otherObject;
					break;
				}

				/*
				 * 2. 从本依赖的另一端递归而来时，
				 * 可以继续删除。当
				 * 从把名义上的另一端作为组成部分包含在内的整个
				 * 对象递归而来时也成立。由于可以有不止
				 * 一个"拥有方"对象，我们必须允许匹配
				 * 栈中更深一层的对象。
				 */
				if (stack_address_present_add_flags(&otherObject, 0, stack))
					break;

				/*
				 * 3. 并非所有拥有方对象都已访问，因此
				 * 把本次删除请求转换成删除这个
				 * 拥有方对象。
				 *
				 * 首先，释放调用者对本对象的锁，并在
				 * 拥有方对象上获取删除锁。（必须释放
				 * 调用者的锁，以免与并发
				 * 删除拥有方对象发生死锁。）
				 */
				ReleaseDeletionLock(object);
				AcquireDeletionLock(&otherObject, 0);

				/*
				 * 在我们等待加锁期间，拥有方对象可能已被删除；
				 * 若是，它和当前对象都不再
				 * 有意义。我们通过检查
				 * pg_depend 条目来测试这一点（见下方说明）。
				 */
				if (!systable_recheck_tuple(scan, tup))
				{
					systable_endscan(scan);
					ReleaseDeletionLock(&otherObject);
					return;
				}

				/*
				 * 无论哪种情况，扫描已经结束；不妨
				 * 在递归前关掉它，以降低峰值
				 * 资源消耗。
				 */
				systable_endscan(scan);

				/*
				 * 好，改为递归到拥有方对象而不是继续处理本对象。
				 *
				 * 我们不必把当前对象入栈；我们希望
				 * 遍历顺序如同原始引用本来
				 * 链到拥有方对象而不是本对象。
				 *
				 * 依赖类型是"反向"依赖：若要删除本对象，
				 * 就需要删除拥有方对象，但
				 * 这条链接从不是自动删除的理由。
				 */
				findDependentObjects(&otherObject,
									 DEPFLAG_REVERSE,
									 flags,
									 stack,
									 targetObjects,
									 pendingObjects,
									 depRel);

				/*
				 * 处理拥有方对象时，当前目标对象应已被加入
				 * targetObjects；但它
				 * 大概只带有与我们正在查看的
				 * 依赖相关的标志位。我们还需要加上
				 * 传给本递归层的 objflags，否则可能
				 * 在 reportDependentObjects 中得到虚假失败（例如
				 * 因分区依赖而被调用时）。
				 *
				 * 若当前对象不知何故没有被安排
				 * 删除，就抱怨。（那意味着有人在递归到达之前
				 * 删除了这条依赖记录。）
				 * 另一种想法是重新获取当前
				 * 对象上的锁并继续尝试删除它，但似乎不
				 * 值得去处理其中固有的竞态。
				 */
				if (!object_address_present_add_flags(object, objflags,
													  targetObjects))
					elog(ERROR, "deletion of owning object %s failed to delete %s",
						 getObjectDescription(&otherObject, false),
						 getObjectDescription(object, false));

				/* 到这里就结束了。 */
				return;

			case DEPENDENCY_PARTITION_PRI:

				/*
				 * 记住本对象具有分区类型依赖。
				 * 依赖扫描之后，若未找到删除其某一分区依赖的
				 * 理由，我们会抱怨。
				 */
				objflags |= DEPFLAG_IS_PART;

				/*
				 * 同时记住主分区所有者，供错误
				 * 消息使用。若有多个主所有者（本
				 * 不应如此），我们会报告其中随机一个。
				 */
				partitionObject = otherObject;
				break;

			case DEPENDENCY_PARTITION_SEC:

				/*
				 * 仅在找不到主所有者时（大概不该发生），才在错误消息中
				 * 使用次分区所有者。
				 */
				if (!(objflags & DEPFLAG_IS_PART))
					partitionObject = otherObject;

				/*
				 * 记住本对象具有分区类型依赖。
				 * 依赖扫描之后，若未找到删除其某一分区依赖的
				 * 理由，我们会抱怨。
				 */
				objflags |= DEPFLAG_IS_PART;
				break;

			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, getObjectDescription(object, false));
				break;
		}
	}

	systable_endscan(scan);

	/*
	 * 若在最外层发现 INTERNAL 或 EXTENSION 依赖，
	 * 现在就抱怨。若同时还发现 PARTITION 依赖，
	 * 我们更愿意报告 PARTITION 依赖。这是任意的，但
	 * 实践中似乎更有用。
	 */
	if (OidIsValid(owningObject.classId))
	{
		char	   *otherObjDesc;

		if (OidIsValid(partitionObject.classId))
			otherObjDesc = getObjectDescription(&partitionObject, false);
		else
			otherObjDesc = getObjectDescription(&owningObject, false);

		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("cannot drop %s because %s requires it",
						getObjectDescription(object, false), otherObjDesc),
				 errhint("You can drop %s instead.", otherObjDesc)));
	}

	/*
	 * 接下来，识别所有直接依赖当前对象的对象。
	 * 为确保可预测的删除顺序，我们把它们收集到
	 * dependentObjects 中，并在真正递归前排序。（
	 * 删除顺序在任何情况下都有效，但这样做能确保
	 * DROP CASCADE 命令输出一致，有利于
	 * 回归测试。）
	 */
	maxDependentObjects = 128;	/* 任意的初始分配 */
	dependentObjects = (ObjectAddressAndFlags *)
		palloc(maxDependentObjects * sizeof(ObjectAddressAndFlags));
	numDependentObjects = 0;

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&key[2],
					Anum_pg_depend_refobjsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(*depRel, DependReferenceIndexId, true,
							  NULL, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);
		int			subflags;

		otherObject.classId = foundDep->classid;
		otherObject.objectId = foundDep->objid;
		otherObject.objectSubId = foundDep->objsubid;

		/*
		 * 若找到的是当前对象的子对象，只需忽略
		 * 它。（通常这种依赖是隐式的，但在涉及分区的某些情形
		 * 必须做成显式。）
		 */
		if (otherObject.classId == object->classId &&
			otherObject.objectId == object->objectId &&
			object->objectSubId == 0)
			continue;

		/*
		 * 递归到依赖方对象之前必须先锁住它。
		 */
		AcquireDeletionLock(&otherObject, 0);

		/*
		 * 在我们等待加锁期间，依赖方对象可能已被删除；
		 * 若是，就不必再对它做任何事。我们可以
		 * 通过查看正在看的 pg_depend 元组是否仍存活，
		 * 便宜且独立于对象类型地测试这一点。（若
		 * 对象已被删除，该元组也会被删除。）
		 */
		if (!systable_recheck_tuple(scan, tup))
		{
			/* 释放现已无用的锁 */
			ReleaseDeletionLock(&otherObject);
			/* 并继续扫描依赖 */
			continue;
		}

		/*
		 * 我们确实需要删除它，因此确定要向下传递的 objflags，
		 * 它们取决于依赖类型。
		 */
		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
				subflags = DEPFLAG_NORMAL;
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_AUTO_EXTENSION:
				subflags = DEPFLAG_AUTO;
				break;
			case DEPENDENCY_INTERNAL:
				subflags = DEPFLAG_INTERNAL;
				break;
			case DEPENDENCY_PARTITION_PRI:
			case DEPENDENCY_PARTITION_SEC:
				subflags = DEPFLAG_PARTITION;
				break;
			case DEPENDENCY_EXTENSION:
				subflags = DEPFLAG_EXTENSION;
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, getObjectDescription(object, false));
				subflags = 0;	/* 让编译器安静 */
				break;
		}

		/* 并把它加入待处理对象列表 */
		if (numDependentObjects >= maxDependentObjects)
		{
			/* 若需要则扩大数组 */
			maxDependentObjects *= 2;
			dependentObjects = (ObjectAddressAndFlags *)
				repalloc(dependentObjects,
						 maxDependentObjects * sizeof(ObjectAddressAndFlags));
		}

		dependentObjects[numDependentObjects].obj = otherObject;
		dependentObjects[numDependentObjects].subflags = subflags;
		numDependentObjects++;
	}

	systable_endscan(scan);

	/*
	 * 现在可以把依赖方对象排成稳定的访问顺序。
	 * 这里可以安全使用 object_address_comparator，因为 obj 字段在
	 * ObjectAddressAndFlags 中位于最前。
	 */
	if (numDependentObjects > 1)
		qsort(dependentObjects, numDependentObjects,
			  sizeof(ObjectAddressAndFlags),
			  object_address_comparator);

	/*
	 * 现在递归到依赖方对象。必须先访问它们，因为
	 * 它们必须在当前对象之前删除。
	 */
	mystack.object = object;	/* 建立新的栈层 */
	mystack.flags = objflags;
	mystack.next = stack;

	for (int i = 0; i < numDependentObjects; i++)
	{
		ObjectAddressAndFlags *depObj = dependentObjects + i;

		findDependentObjects(&depObj->obj,
							 depObj->subflags,
							 flags,
							 &mystack,
							 targetObjects,
							 pendingObjects,
							 depRel);
	}

	pfree(dependentObjects);

	/*
	 * 最后，可以把目标对象加入 targetObjects。注意
	 * 包含从内层递归回传给我们的任何标志。
	 * 把"被依赖对象"记录为：若有则取最重要的
	 * 分区所有者，否则取我们从中递归而来的对象（如果
	 * 有）。（reportDependentObjects() 中的逻辑使得它只
	 * 需要这些对象之一。）
	 */
	extra.flags = mystack.flags;
	if (extra.flags & DEPFLAG_IS_PART)
		extra.dependee = partitionObject;
	else if (stack)
		extra.dependee = *stack->object;
	else
		memset(&extra.dependee, 0, sizeof(extra.dependee));
	add_exact_object_address_extra(object, &extra, targetObjects);
}

/*
 * reportDependentObjects - 报告依赖，并在 RESTRICT 时失败
 *
 * 告知用户我们即将删除的依赖方对象
 * （或本需删除、但被 RESTRICT 模式阻止的对象）；
 * 若有任何此类对象且不是 CASCADE 模式，则报错。
 *
 *	targetObjects: 已安排删除的对象列表
 *	behavior: RESTRICT 或 CASCADE
 *	flags: 删除操作的其他标志
 *	origObject: 删除的基础对象，不可用则为 NULL
 *		（后一情形出现在 DROP OWNED 中）
 */
static void
reportDependentObjects(const ObjectAddresses *targetObjects,
					   DropBehavior behavior,
					   int flags,
					   const ObjectAddress *origObject)
{
	int			msglevel = (flags & PERFORM_DELETION_QUIETLY) ? DEBUG2 : NOTICE;
	bool		ok = true;
	StringInfoData clientdetail;
	StringInfoData logdetail;
	int			numReportedClient = 0;
	int			numNotReportedClient = 0;
	int			i;

	/*
	 * 若需要删除任何分区依赖的对象，确保
	 * 我们也至少删除它们的一个分区依赖。这
	 * 可以通过检查我们曾经由 PARTITION
	 * 依赖到达它们来检测。
	 *
	 * 我们只报告第一个这样的对象，因为多数情况下触发此抱怨的
	 * 唯一办法是显式尝试删除分区对象的
	 * 一个分区。
	 */
	for (i = 0; i < targetObjects->numrefs; i++)
	{
		const ObjectAddressExtra *extra = &targetObjects->extras[i];

		if ((extra->flags & DEPFLAG_IS_PART) &&
			!(extra->flags & DEPFLAG_PARTITION))
		{
			const ObjectAddress *object = &targetObjects->refs[i];
			char	   *otherObjDesc = getObjectDescription(&extra->dependee,
															false);

			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop %s because %s requires it",
							getObjectDescription(object, false), otherObjDesc),
					 errhint("You can drop %s instead.", otherObjDesc)));
		}
	}

	/*
	 * 若不会抛错，且 msglevel 低到既不会显示给
	 * 客户端也不会写入服务器日志，就不必做其余
	 * 工作。
	 */
	if (behavior == DROP_CASCADE &&
		!message_level_is_interesting(msglevel))
		return;

	/*
	 * 向客户端报告的依赖数量限制为
	 * MAX_REPORTED_DEPS，因为客户端软件可能不好处理
	 * 巨大的错误字符串。服务器日志总是得到完整报告。
	 */
#define MAX_REPORTED_DEPS 100

	initStringInfo(&clientdetail);
	initStringInfo(&logdetail);

	/*
	 * 我们从后向前处理列表（即按依赖顺序而非删除
	 * 顺序），因为这样显示更易理解。
	 */
	for (i = targetObjects->numrefs - 1; i >= 0; i--)
	{
		const ObjectAddress *obj = &targetObjects->refs[i];
		const ObjectAddressExtra *extra = &targetObjects->extras[i];
		char	   *objDesc;

		/* 忽略原始删除 target */
		if (extra->flags & DEPFLAG_ORIGINAL)
			continue;

		/* 也忽略子对象；我们会在别处报告整个对象 */
		if (extra->flags & DEPFLAG_SUBOBJECT)
			continue;

		objDesc = getObjectDescription(obj, false);

		/* 正在被并发删除的对象不必报告 */
		if (objDesc == NULL)
			continue;

		/*
		 * 若在递归搜索的任何阶段，我们是经由
		 * AUTO、INTERNAL、PARTITION 或 EXTENSION 依赖到达该对象的，那么
		 * 即使在 RESTRICT 模式下也可以删除它。
		 */
		if (extra->flags & (DEPFLAG_AUTO |
							DEPFLAG_INTERNAL |
							DEPFLAG_PARTITION |
							DEPFLAG_EXTENSION))
		{
			/*
			 * 自动级联在 DEBUG2 报告，而不是 msglevel。我们不
			 * 试图把它们与常规消息合并，因为当
			 * client_min_messages 与
			 * log_min_messages 不同时结果太令人困惑。
			 */
			ereport(DEBUG2,
					(errmsg_internal("drop auto-cascades to %s",
									 objDesc)));
		}
		else if (behavior == DROP_RESTRICT)
		{
			char	   *otherDesc = getObjectDescription(&extra->dependee,
														 false);

			if (otherDesc)
			{
				if (numReportedClient < MAX_REPORTED_DEPS)
				{
					/* 用换行分隔各条目 */
					if (clientdetail.len != 0)
						appendStringInfoChar(&clientdetail, '\n');
					appendStringInfo(&clientdetail, _("%s depends on %s"),
									 objDesc, otherDesc);
					numReportedClient++;
				}
				else
					numNotReportedClient++;
				/* 用换行分隔各条目 */
				if (logdetail.len != 0)
					appendStringInfoChar(&logdetail, '\n');
				appendStringInfo(&logdetail, _("%s depends on %s"),
								 objDesc, otherDesc);
				pfree(otherDesc);
			}
			else
				numNotReportedClient++;
			ok = false;
		}
		else
		{
			if (numReportedClient < MAX_REPORTED_DEPS)
			{
				/* 用换行分隔各条目 */
				if (clientdetail.len != 0)
					appendStringInfoChar(&clientdetail, '\n');
				appendStringInfo(&clientdetail, _("drop cascades to %s"),
								 objDesc);
				numReportedClient++;
			}
			else
				numNotReportedClient++;
			/* 用换行分隔各条目 */
			if (logdetail.len != 0)
				appendStringInfoChar(&logdetail, '\n');
			appendStringInfo(&logdetail, _("drop cascades to %s"),
							 objDesc);
		}

		pfree(objDesc);
	}

	if (numNotReportedClient > 0)
		appendStringInfo(&clientdetail, ngettext("\nand %d other object "
												 "(see server log for list)",
												 "\nand %d other objects "
												 "(see server log for list)",
												 numNotReportedClient),
						 numNotReportedClient);

	if (!ok)
	{
		if (origObject)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop %s because other objects depend on it",
							getObjectDescription(origObject, false)),
					 errdetail_internal("%s", clientdetail.data),
					 errdetail_log("%s", logdetail.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop desired object(s) because other objects depend on them"),
					 errdetail_internal("%s", clientdetail.data),
					 errdetail_log("%s", logdetail.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
	}
	else if (numReportedClient > 1)
	{
		ereport(msglevel,
				(errmsg_plural("drop cascades to %d other object",
							   "drop cascades to %d other objects",
							   numReportedClient + numNotReportedClient,
							   numReportedClient + numNotReportedClient),
				 errdetail_internal("%s", clientdetail.data),
				 errdetail_log("%s", logdetail.data)));
	}
	else if (numReportedClient == 1)
	{
		/* 我们直接原样使用这一条 */
		ereport(msglevel,
				(errmsg_internal("%s", clientdetail.data)));
	}

	pfree(clientdetail.data);
	pfree(logdetail.data);
}

/*
 * 按 OID 删除对象。对大多数目录有效，若不需要特殊处理
 * 的话。
 */
static void
DropObjectById(const ObjectAddress *object)
{
	int			cacheId;
	Relation	rel;
	HeapTuple	tup;

	cacheId = get_object_catcache_oid(object->classId);

	rel = table_open(object->classId, RowExclusiveLock);

	/*
	 * 若存在，则对 oid 列使用系统缓存。
	 */
	if (cacheId >= 0)
	{
		tup = SearchSysCache1(cacheId, ObjectIdGetDatum(object->objectId));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for %s %u",
				 get_object_class_descr(object->classId), object->objectId);

		CatalogTupleDelete(rel, &tup->t_self);

		ReleaseSysCache(tup);
	}
	else
	{
		ScanKeyData skey[1];
		SysScanDesc scan;

		ScanKeyInit(&skey[0],
					get_object_attnum_oid(object->classId),
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(object->objectId));

		scan = systable_beginscan(rel, get_object_oid_index(object->classId), true,
								  NULL, 1, skey);

		/* 我们期望恰好匹配一条 */
		tup = systable_getnext(scan);
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "could not find tuple for %s %u",
				 get_object_class_descr(object->classId), object->objectId);

		CatalogTupleDelete(rel, &tup->t_self);

		systable_endscan(scan);
	}

	table_close(rel, RowExclusiveLock);
}

/*
 * deleteOneObject: 为 performDeletion 删除单个对象。
 *
 * *depRel 是已经打开的 pg_depend 关系。
 */
static void
deleteOneObject(const ObjectAddress *object, Relation *depRel, int flags)
{
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;

	/* 正被移除对象的 DROP 钩子 */
	InvokeObjectDropHookArg(object->classId, object->objectId,
							object->objectSubId, flags);

	/*
	 * 若正在并发删除，则关闭 depRel。对象删除
	 * 子例程会提交当前事务，因此我们不能在
	 * doDeletion() 期间保持该关系打开。
	 */
	if (flags & PERFORM_DELETION_CONCURRENTLY)
		table_close(*depRel, RowExclusiveLock);

	/*
	 * 以依赖于对象类型的方式删除对象本身。
	 *
	 * 我们以前在移除向外的依赖链接之后才做这件事，但
	 * 事先做似乎同样合理。在并发情形下
	 * 我们必须按此顺序，因为在调用 doDeletion() 之前不能做任何事务性
	 * 更新 --- 它们会马上被提交，
	 * 若随后删除失败就不合适。
	 */
	doDeletion(object, flags);

	/*
	 * 若上面关闭过 depRel，则重新打开
	 */
	if (flags & PERFORM_DELETION_CONCURRENTLY)
		*depRel = table_open(DependRelationId, RowExclusiveLock);

	/*
	 * 现在移除任何从本对象链到其他对象的 pg_depend 记录。
	 * （链到本对象的记录应该已经没了。）
	 *
	 * 删除整个对象（subId = 0）时，也移除
	 * 其子对象的全部 pg_depend 记录。
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&key[2],
					Anum_pg_depend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(*depRel, DependDependerIndexId, true,
							  NULL, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		CatalogTupleDelete(*depRel, &tup->t_self);
	}

	systable_endscan(scan);

	/*
	 * 删除与本对象相关的共享依赖引用。同样，若
	 * subId = 0，也移除子对象的记录。
	 */
	deleteSharedDependencyRecordsFor(object->classId, object->objectId,
									 object->objectSubId);


	/*
	 * 删除与本对象关联的任何注释、安全标签或初始权限。
	 * （在这里做这些事很方便，
	 * 而不必让每种对象类型都知道要做。）如上，若 subid 为
	 * 零，这些函数也都必须移除子对象的
	 * 记录。
	 */
	DeleteComments(object->objectId, object->classId, object->objectSubId);
	DeleteSecurityLabel(object);
	DeleteInitPrivs(object);

	/*
	 * 这里做 CommandCounterIncrement，以确保前面的更改
	 * 对下一步删除可见。
	 */
	CommandCounterIncrement();

	/*
	 * 完成了！
	 */
}

/*
 * doDeletion: 实际删除单个对象
 */
static void
doDeletion(const ObjectAddress *object, int flags)
{
	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			{
				char		relKind = get_rel_relkind(object->objectId);

				if (relKind == RELKIND_INDEX ||
					relKind == RELKIND_PARTITIONED_INDEX)
				{
					bool		concurrent = ((flags & PERFORM_DELETION_CONCURRENTLY) != 0);
					bool		concurrent_lock_mode = ((flags & PERFORM_DELETION_CONCURRENT_LOCK) != 0);

					Assert(object->objectSubId == 0);
					index_drop(object->objectId, concurrent, concurrent_lock_mode);
				}
				else
				{
					if (object->objectSubId != 0)
						RemoveAttributeById(object->objectId,
											object->objectSubId);
					else
						heap_drop_with_catalog(object->objectId);
				}

				/*
				 * 对于序列，除删除堆外，还要
				 * 删除 pg_sequence 元组
				 */
				if (relKind == RELKIND_SEQUENCE)
					DeleteSequenceTuple(object->objectId);
				break;
			}

		case OCLASS_PROC:
			RemoveFunctionById(object->objectId);
			break;

		case OCLASS_TYPE:
			RemoveTypeById(object->objectId);
			break;

		case OCLASS_CONSTRAINT:
			RemoveConstraintById(object->objectId);
			break;

		case OCLASS_DEFAULT:
			RemoveAttrDefaultById(object->objectId);
			break;

		case OCLASS_LARGEOBJECT:
			LargeObjectDrop(object->objectId);
			break;

		case OCLASS_OPERATOR:
			RemoveOperatorById(object->objectId);
			break;

		case OCLASS_REWRITE:
			RemoveRewriteRuleById(object->objectId);
			break;

		case OCLASS_TRIGGER:
			RemoveTriggerById(object->objectId);
			break;

		case OCLASS_STATISTIC_EXT:
			RemoveStatisticsById(object->objectId);
			break;

		case OCLASS_TSCONFIG:
			RemoveTSConfigurationById(object->objectId);
			break;

		case OCLASS_EXTENSION:
			RemoveExtensionById(object->objectId);
			break;

		case OCLASS_POLICY:
			RemovePolicyById(object->objectId);
			break;

		case OCLASS_PUBLICATION_NAMESPACE:
			RemovePublicationSchemaById(object->objectId);
			break;

		case OCLASS_PUBLICATION_REL:
			RemovePublicationRelById(object->objectId);
			break;

		case OCLASS_PUBLICATION:
			RemovePublicationById(object->objectId);
			break;

		case OCLASS_CAST:
		case OCLASS_COLLATION:
		case OCLASS_CONVERSION:
		case OCLASS_LANGUAGE:
		case OCLASS_OPCLASS:
		case OCLASS_OPFAMILY:
		case OCLASS_AM:
		case OCLASS_AMOP:
		case OCLASS_AMPROC:
		case OCLASS_SCHEMA:
		case OCLASS_TSPARSER:
		case OCLASS_TSDICT:
		case OCLASS_TSTEMPLATE:
		case OCLASS_FDW:
		case OCLASS_FOREIGN_SERVER:
		case OCLASS_USER_MAPPING:
		case OCLASS_DEFACL:
		case OCLASS_EVENT_TRIGGER:
		case OCLASS_TRANSFORM:
		case OCLASS_ROLE_MEMBERSHIP:
			DropObjectById(object);
			break;

			/*
			 * 这些全局对象类型在此不受支持。
			 */
		case OCLASS_ROLE:
		case OCLASS_DATABASE:
		case OCLASS_TBLSPACE:
		case OCLASS_SUBSCRIPTION:
		case OCLASS_PARAMETER_ACL:
			elog(ERROR, "global objects cannot be deleted by doDeletion");
			break;

			/*
			 * 这里故意没有 default: 分支；我们希望
			 * 若上面未处理新的 OCLASS，编译器会警告。
			 */
	}
}

/*
 * AcquireDeletionLock - 为删除对象获取合适的锁
 *
 * 接受与 performDeletion 相同的标志（不过目前只有
 * PERFORM_DELETION_CONCURRENTLY 有实际作用）。
 *
 * 对关系使用 LockRelation，否则按对象类型酌情使用
 * LockSharedObject 或 LockDatabaseObject。
 */
void
AcquireDeletionLock(const ObjectAddress *object, int flags)
{
	if (object->classId == RelationRelationId)
	{
		/*
		 * 在 DROP INDEX CONCURRENTLY 中，此刻对索引只取
		 * ShareUpdateExclusiveLock。index_drop() 一旦安全就会提升该锁。
		 * 所有其他情形我们需要完全排他
		 * 锁。
		 */
		if (flags & PERFORM_DELETION_CONCURRENTLY)
			LockRelationOid(object->objectId, ShareUpdateExclusiveLock);
		else
			LockRelationOid(object->objectId, AccessExclusiveLock);
	}
	else if (object->classId == AuthMemRelationId)
		LockSharedObject(object->classId, object->objectId, 0,
						 AccessExclusiveLock);
	else
	{
		/* 假定应锁整个对象而非子对象 */
		LockDatabaseObject(object->classId, object->objectId, 0,
						   AccessExclusiveLock);
	}
}

/*
 * ReleaseDeletionLock - 释放对象删除锁
 *
 * AcquireDeletionLock 的配对函数。
 */
void
ReleaseDeletionLock(const ObjectAddress *object)
{
	if (object->classId == RelationRelationId)
		UnlockRelationOid(object->objectId, AccessExclusiveLock);
	else
		/* 假定应锁整个对象而非子对象 */
		UnlockDatabaseObject(object->classId, object->objectId, 0,
							 AccessExclusiveLock);
}

/*
 * recordDependencyOnExpr - 查找表达式依赖
 *
 * 用于查找规则、约束表达式等的
 * 依赖。
 *
 * 给定节点树形式的表达式或查询，找出它引用的
 * 全部对象（表、列、操作符、函数等）。记录
 * 从给定依赖方对象到表达式中提到的每个对象的、
 * 指定类型的依赖。
 *
 * rtable 是用于解释 varlevelsup=0 的 Var 的范围表。
 * 若不预期有这类变量，可以为 NIL。
 */
void
recordDependencyOnExpr(const ObjectAddress *depender,
					   Node *expr, List *rtable,
					   DependencyType behavior)
{
	find_expr_references_context context;

	context.addrs = new_object_addresses();

	/* 为 varlevelsup = 0 的 Var 设置解释 */
	context.rtables = list_make1(rtable);

	/* 扫描表达式树以找出可引用对象 */
	find_expr_references_walker(expr, &context);

	/* 去除任何重复 */
	eliminate_duplicate_dependencies(context.addrs);

	/* 然后记录它们 */
	recordMultipleDependencies(depender,
							   context.addrs->refs, context.addrs->numrefs,
							   behavior);

	free_object_addresses(context.addrs);
}

/*
 * recordDependencyOnSingleRelExpr - 查找表达式依赖
 *
 * 同上，但预期只引用一个关系（
 * varno = 1 且 varlevelsup = 0）。传入关系 OID 而不是
 * 范围表。额外约定是：对该
 * 关系组成列的依赖将标以 'self_behavior'，
 * 而其他一切使用 'behavior'；另外，若 'reverse_self'
 * 为真，这些依赖会被反转，使列
 * 依赖于表而不是相反。
 *
 * NOTE: 调用者应确保若有必要，对该
 * 指定关系的整表依赖另行创建。特别是，
 * 整行 Var "relation.*" 不会使本例程发出任何
 * 依赖项。这对普通查询的子表达式是合适行为，
 * 因此其他情形需按需自行处理。
 */
void
recordDependencyOnSingleRelExpr(const ObjectAddress *depender,
								Node *expr, Oid relId,
								DependencyType behavior,
								DependencyType self_behavior,
								bool reverse_self)
{
	find_expr_references_context context;
	RangeTblEntry rte = {0};

	context.addrs = new_object_addresses();

	/* 我们拼出一个相当假的范围表列表来处理 Var */
	rte.type = T_RangeTblEntry;
	rte.rtekind = RTE_RELATION;
	rte.relid = relId;
	rte.relkind = RELKIND_RELATION; /* 这里不必精确 */
	rte.rellockmode = AccessShareLock;

	context.rtables = list_make1(list_make1(&rte));

	/* 扫描表达式树以找出可引用对象 */
	find_expr_references_walker(expr, &context);

	/* 去除任何重复 */
	eliminate_duplicate_dependencies(context.addrs);

	/* 必要时分离自身依赖 */
	if ((behavior != self_behavior || reverse_self) &&
		context.addrs->numrefs > 0)
	{
		ObjectAddresses *self_addrs;
		ObjectAddress *outobj;
		int			oldref,
					outrefs;

		self_addrs = new_object_addresses();

		outobj = context.addrs->refs;
		outrefs = 0;
		for (oldref = 0; oldref < context.addrs->numrefs; oldref++)
		{
			ObjectAddress *thisobj = context.addrs->refs + oldref;

			if (thisobj->classId == RelationRelationId &&
				thisobj->objectId == relId)
			{
				/* 把此引用移入 self_addrs */
				add_exact_object_address(thisobj, self_addrs);
			}
			else
			{
				/* 把它留在 context.addrs */
				*outobj = *thisobj;
				outobj++;
				outrefs++;
			}
		}
		context.addrs->numrefs = outrefs;

		/* 以适当方向记录自身依赖 */
		if (!reverse_self)
			recordMultipleDependencies(depender,
									   self_addrs->refs, self_addrs->numrefs,
									   self_behavior);
		else
		{
			/* 不能用 recordMultipleDependencies，因此用笨办法 */
			int			selfref;

			for (selfref = 0; selfref < self_addrs->numrefs; selfref++)
			{
				ObjectAddress *thisobj = self_addrs->refs + selfref;

				recordDependencyOn(thisobj, depender, self_behavior);
			}
		}

		free_object_addresses(self_addrs);
	}

	/* 记录外部依赖 */
	recordMultipleDependencies(depender,
							   context.addrs->refs, context.addrs->numrefs,
							   behavior);

	free_object_addresses(context.addrs);
}

/*
 * 递归搜索表达式树以查找对象引用。
 *
 * 注意：很多情形下我们不必为表达式中涉及的数据类型
 * 创建依赖，因为会经由
 * 其他对象有间接依赖。例如 Var 节点依赖于某列，该列又依赖于
 * 数据类型，而 OpExpr 节点依赖于操作符，操作符又依赖于
 * 数据类型。不过若没有这种
 * 间接依赖，我们就需要类型依赖，例如 Const 和 CoerceToDomain 节点。
 *
 * 类似地，除排序规则被新引入表达式之处外，
 * 我们不必为排序规则创建依赖。
 */
static bool
find_expr_references_walker(Node *node,
							find_expr_references_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		List	   *rtable;
		RangeTblEntry *rte;

		/* 查找匹配的范围表条目，找不到则抱怨 */
		if (var->varlevelsup >= list_length(context->rtables))
			elog(ERROR, "invalid varlevelsup %d", var->varlevelsup);
		rtable = (List *) list_nth(context->rtables, var->varlevelsup);
		if (var->varno <= 0 || var->varno > list_length(rtable))
			elog(ERROR, "invalid varno %d", var->varno);
		rte = rt_fetch(var->varno, rtable);

		/*
		 * 整行 Var 不引用特定列，因此不加新
		 * 依赖。（我们假定每个底层范围表条目都会产生
		 * 整表依赖。虽然在找到直接引用某关系的整行 Var 时
		 * 可以记录这种依赖，但如何推广
		 * 到 JOINs 的整行 Var 相当不清楚，因此似乎更好把
		 * 责任留给范围表。注意这对识别独立表达式的
		 * 依赖带来一些风险：
		 * 整表引用可能需要另行创建。）
		 *
		 */
		if (var->varattno == InvalidAttrNumber)
			return false;
		if (rte->rtekind == RTE_RELATION)
		{
			/* 若是普通关系，引用此列 */
			add_object_address(OCLASS_CLASS, rte->relid, var->varattno,
							   context->addrs);
		}
		else if (rte->rtekind == RTE_FUNCTION)
		{
			/* 可能需要添加对复合类型某列的依赖 */
			/* （在行外完成，因为有点臃肿） */
			process_function_rte_ref(rte, var->varattno, context);
		}

		/*
		 * 引用其他 RTE 类型的 Var 无需额外工作。
		 * 特别是，连接别名 Var 可以忽略，因为它必须
		 * 引用合并后的 USING 列。相关的连接输入列
		 * 也会在连接条件中被引用，别名表达式中涉及的任何
		 * 类型强制转换函数会在我们扫描
		 * RTE 本身时处理。
		 */
		return false;
	}
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;
		Oid			objoid;

		/* 常量必须依赖于该常量的数据类型 */
		add_object_address(OCLASS_TYPE, con->consttype, 0,
						   context->addrs);

		/*
		 * 还必须依赖于该常量的排序规则：若 CollateExpr 被常量折叠成
		 * 简单常量，它可能
		 * 与数据类型的不同。不过在最常见的
		 * 排序规则为"default"的情形可以省事，因为我们知道那是钉住的。
		 */
		if (OidIsValid(con->constcollid) &&
			con->constcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, con->constcollid, 0,
							   context->addrs);

		/*
		 * 若它是引用现有对象的 regclass 或类似字面量，
		 * 则添加对该对象的引用。（目前只有
		 * regclass 和 regconfig 情形较可能有用，但我们不妨
		 * 一致地处理所有 OID 别名数据类型。）
		 */
		if (!con->constisnull)
		{
			switch (con->consttype)
			{
				case REGPROCOID:
				case REGPROCEDUREOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(PROCOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_PROC, objoid, 0,
										   context->addrs);
					break;
				case REGOPEROID:
				case REGOPERATOROID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(OPEROID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_OPERATOR, objoid, 0,
										   context->addrs);
					break;
				case REGCLASSOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(RELOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_CLASS, objoid, 0,
										   context->addrs);
					break;
				case REGTYPEOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(TYPEOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_TYPE, objoid, 0,
										   context->addrs);
					break;
				case REGCOLLATIONOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(COLLOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_COLLATION, objoid, 0,
										   context->addrs);
					break;
				case REGCONFIGOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(TSCONFIGOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_TSCONFIG, objoid, 0,
										   context->addrs);
					break;
				case REGDICTIONARYOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(TSDICTOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_TSDICT, objoid, 0,
										   context->addrs);
					break;

				case REGNAMESPACEOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(NAMESPACEOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_SCHEMA, objoid, 0,
										   context->addrs);
					break;

					/*
					 * regrole 的依赖应在所有
					 * 数据库间共享，因此显式禁止为其建立依赖。
					 */
				case REGROLEOID:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("constant of the type %s cannot be used here",
									"regrole")));
					break;
			}
		}
		return false;
	}
	else if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		/* 参数必须依赖于该参数的数据类型 */
		add_object_address(OCLASS_TYPE, param->paramtype, 0,
						   context->addrs);
		/* 以及其排序规则，与 Const 相同 */
		if (OidIsValid(param->paramcollid) &&
			param->paramcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, param->paramcollid, 0,
							   context->addrs);
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *funcexpr = (FuncExpr *) node;

		add_object_address(OCLASS_PROC, funcexpr->funcid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, DistinctExpr))
	{
		DistinctExpr *distinctexpr = (DistinctExpr *) node;

		add_object_address(OCLASS_OPERATOR, distinctexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, NullIfExpr))
	{
		NullIfExpr *nullifexpr = (NullIfExpr *) node;

		add_object_address(OCLASS_OPERATOR, nullifexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		add_object_address(OCLASS_PROC, aggref->aggfnoid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, WindowFunc))
	{
		WindowFunc *wfunc = (WindowFunc *) node;

		add_object_address(OCLASS_PROC, wfunc->winfnoid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, SubscriptingRef))
	{
		SubscriptingRef *sbsref = (SubscriptingRef *) node;

		/*
		 * refexpr 应对 refcontainertype 提供足够依赖，
		 * 而该类型又依赖于 refelemtype。不过，自定义
		 * 下标处理函数可能把 refrestype 设成与这两者都不同的值，
		 * 那时我们最好记录它。
		 */
		if (sbsref->refrestype != sbsref->refcontainertype &&
			sbsref->refrestype != sbsref->refelemtype)
			add_object_address(OCLASS_TYPE, sbsref->refrestype, 0,
							   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, SubPlan))
	{
		/* 若将来需要此情形，这里还要额外工作 */
		elog(ERROR, "already-planned subqueries not supported");
	}
	else if (IsA(node, FieldSelect))
	{
		FieldSelect *fselect = (FieldSelect *) node;
		Oid			argtype = getBaseType(exprType((Node *) fselect->arg));
		Oid			reltype = get_typ_typrelid(argtype);

		/*
		 * 我们需要对 FieldSelect 中点名的那一列建立依赖，
		 * 前提是能识别其 pg_class OID。（目前大概
		 * 总能做到，但将来 argtype 有可能是
		 * RECORDOID。）若能建立列依赖，就
		 * 不必再依赖该列的类型；但若
		 * 不能，则对类型建立依赖，因为它可能不会
		 * 出现在表达式的其他地方。
		 */
		if (OidIsValid(reltype))
			add_object_address(OCLASS_CLASS, reltype, fselect->fieldnum,
							   context->addrs);
		else
			add_object_address(OCLASS_TYPE, fselect->resulttype, 0,
							   context->addrs);
		/* 排序规则也可能没有在别处被引用 */
		if (OidIsValid(fselect->resultcollid) &&
			fselect->resultcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, fselect->resultcollid, 0,
							   context->addrs);
	}
	else if (IsA(node, FieldStore))
	{
		FieldStore *fstore = (FieldStore *) node;
		Oid			reltype = get_typ_typrelid(fstore->resulttype);

		/* 与 FieldSelect 类似的考虑，但是多个 column(s) */
		if (OidIsValid(reltype))
		{
			ListCell   *l;

			foreach(l, fstore->fieldnums)
				add_object_address(OCLASS_CLASS, reltype, lfirst_int(l),
								   context->addrs);
		}
		else
			add_object_address(OCLASS_TYPE, fstore->resulttype, 0,
							   context->addrs);
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *relab = (RelabelType *) node;

		/* 因为没有函数依赖，需要依赖类型 */
		add_object_address(OCLASS_TYPE, relab->resulttype, 0,
						   context->addrs);
		/* 排序规则也可能没有在别处被引用 */
		if (OidIsValid(relab->resultcollid) &&
			relab->resultcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, relab->resultcollid, 0,
							   context->addrs);
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) node;

		/* 因为没有暴露的函数，需要依赖类型 */
		add_object_address(OCLASS_TYPE, iocoerce->resulttype, 0,
						   context->addrs);
		/* 排序规则也可能没有在别处被引用 */
		if (OidIsValid(iocoerce->resultcollid) &&
			iocoerce->resultcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, iocoerce->resultcollid, 0,
							   context->addrs);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

		/* 如上，依赖类型 */
		add_object_address(OCLASS_TYPE, acoerce->resulttype, 0,
						   context->addrs);
		/* 排序规则也可能没有在别处被引用 */
		if (OidIsValid(acoerce->resultcollid) &&
			acoerce->resultcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, acoerce->resultcollid, 0,
							   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *cvt = (ConvertRowtypeExpr *) node;

		/* 因为没有函数依赖，需要依赖类型 */
		add_object_address(OCLASS_TYPE, cvt->resulttype, 0,
						   context->addrs);
	}
	else if (IsA(node, CollateExpr))
	{
		CollateExpr *coll = (CollateExpr *) node;

		add_object_address(OCLASS_COLLATION, coll->collOid, 0,
						   context->addrs);
	}
	else if (IsA(node, RowExpr))
	{
		RowExpr    *rowexpr = (RowExpr *) node;

		add_object_address(OCLASS_TYPE, rowexpr->row_typeid, 0,
						   context->addrs);
	}
	else if (IsA(node, RowCompareExpr))
	{
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;
		ListCell   *l;

		foreach(l, rcexpr->opnos)
		{
			add_object_address(OCLASS_OPERATOR, lfirst_oid(l), 0,
							   context->addrs);
		}
		foreach(l, rcexpr->opfamilies)
		{
			add_object_address(OCLASS_OPFAMILY, lfirst_oid(l), 0,
							   context->addrs);
		}
		/* fall through to examine arguments */
	}
	else if (IsA(node, CoerceToDomain))
	{
		CoerceToDomain *cd = (CoerceToDomain *) node;

		add_object_address(OCLASS_TYPE, cd->resulttype, 0,
						   context->addrs);
	}
	else if (IsA(node, NextValueExpr))
	{
		NextValueExpr *nve = (NextValueExpr *) node;

		add_object_address(OCLASS_CLASS, nve->seqid, 0,
						   context->addrs);
	}
	else if (IsA(node, OnConflictExpr))
	{
		OnConflictExpr *onconflict = (OnConflictExpr *) node;

		if (OidIsValid(onconflict->constraint))
			add_object_address(OCLASS_CONSTRAINT, onconflict->constraint, 0,
							   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, SortGroupClause))
	{
		SortGroupClause *sgc = (SortGroupClause *) node;

		add_object_address(OCLASS_OPERATOR, sgc->eqop, 0,
						   context->addrs);
		if (OidIsValid(sgc->sortop))
			add_object_address(OCLASS_OPERATOR, sgc->sortop, 0,
							   context->addrs);
		return false;
	}
	else if (IsA(node, WindowClause))
	{
		WindowClause *wc = (WindowClause *) node;

		if (OidIsValid(wc->startInRangeFunc))
			add_object_address(OCLASS_PROC, wc->startInRangeFunc, 0,
							   context->addrs);
		if (OidIsValid(wc->endInRangeFunc))
			add_object_address(OCLASS_PROC, wc->endInRangeFunc, 0,
							   context->addrs);
		if (OidIsValid(wc->inRangeColl) &&
			wc->inRangeColl != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, wc->inRangeColl, 0,
							   context->addrs);
		/* fall through to examine substructure */
	}
	else if (IsA(node, CTECycleClause))
	{
		CTECycleClause *cc = (CTECycleClause *) node;

		if (OidIsValid(cc->cycle_mark_type))
			add_object_address(OCLASS_TYPE, cc->cycle_mark_type, 0,
							   context->addrs);
		if (OidIsValid(cc->cycle_mark_collation))
			add_object_address(OCLASS_COLLATION, cc->cycle_mark_collation, 0,
							   context->addrs);
		if (OidIsValid(cc->cycle_mark_neop))
			add_object_address(OCLASS_OPERATOR, cc->cycle_mark_neop, 0,
							   context->addrs);
		/* fall through to examine substructure */
	}
	else if (IsA(node, Query))
	{
		/* 递归进入 RTE 子查询或尚未规划的子链接子查询 */
		Query	   *query = (Query *) node;
		ListCell   *lc;
		bool		result;

		/*
		 * 为子查询范围表中提到的每个普通关系添加整关系引用，
		 * 并确保为连接别名列表中使用的任何类型强制转换
		 * 函数添加引用。
		 *
		 * 注意：query_tree_walker 会负责递归进入 RTE_FUNCTION
		 * RTEs、子查询等，因此这里不必再做。但我们必须
		 * 告诉它不要访问连接别名列表，否则会为连接
		 * 输入列添加引用，无论它们是否真正用在我们的查询中。
		 *
		 * 注意：我们不必担心 RTE_VALUES 或 RTE_CTE RTE 中提到的
		 * 排序规则，因为那些必须只是复制
		 * Query 其他部分引用的排序规则。我们确实必须
		 * 担心 RTE_FUNCTION 中提到的排序规则，但我们在递归到
		 * RangeTblFunction node(s) 时处理它们。
		 */
		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			switch (rte->rtekind)
			{
				case RTE_RELATION:
					add_object_address(OCLASS_CLASS, rte->relid, 0,
									   context->addrs);
					break;
				case RTE_JOIN:

					/*
					 * 只检查已合并 JOIN
					 * USING 列的 joinaliasvars 条目。只有那些条目可能包含
					 * 类型强制转换函数。此外，它们的连接输入
					 * 列必须在连接条件中被引用，因此这
					 * 不会意外为本不使用的连接
					 * 输入列添加引用。（即使合并列在任何地方都没有被显式
					 * 使用，我们也要引用类型强制转换
					 * 函数，以保护连接 RTE 可能展开为
					 * 整行 var，并且允许删除出现在
					 * 我们查询树中的函数似乎不是好主意，
					 * 无论它会不会被调用。）
					 */
					context->rtables = lcons(query->rtable, context->rtables);
					for (int i = 0; i < rte->joinmergedcols; i++)
					{
						Node	   *aliasvar = list_nth(rte->joinaliasvars, i);

						if (!IsA(aliasvar, Var))
							find_expr_references_walker(aliasvar, context);
					}
					context->rtables = list_delete_first(context->rtables);
					break;
				default:
					break;
			}
		}

		/*
		 * 若查询是 INSERT 或 UPDATE，应对每个目标列创建依赖，
		 * 以防止该特定目标列被
		 * 删除。虽然我们会在 query_tree_walker 期间再次访问
		 * TargetEntry 节点，但那时没有足够上下文方便地做这件事，
		 * 因此在这里做。
		 */
		if (query->commandType == CMD_INSERT ||
			query->commandType == CMD_UPDATE)
		{
			RangeTblEntry *rte;

			if (query->resultRelation <= 0 ||
				query->resultRelation > list_length(query->rtable))
				elog(ERROR, "invalid resultRelation %d",
					 query->resultRelation);
			rte = rt_fetch(query->resultRelation, query->rtable);
			if (rte->rtekind == RTE_RELATION)
			{
				foreach(lc, query->targetList)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(lc);

					if (tle->resjunk)
						continue;	/* 忽略垃圾目标列表项 */
					add_object_address(OCLASS_CLASS, rte->relid, tle->resno,
									   context->addrs);
				}
			}
		}

		/*
		 * 为查询的 constraintDeps 中列出的约束添加依赖
		 */
		foreach(lc, query->constraintDeps)
		{
			add_object_address(OCLASS_CONSTRAINT, lfirst_oid(lc), 0,
							   context->addrs);
		}

		/* 检查查询的子结构 */
		context->rtables = lcons(query->rtable, context->rtables);
		result = query_tree_walker(query,
								   find_expr_references_walker,
								   (void *) context,
								   QTW_IGNORE_JOINALIASES |
								   QTW_EXAMINE_SORTGROUP);
		context->rtables = list_delete_first(context->rtables);
		return result;
	}
	else if (IsA(node, SetOperationStmt))
	{
		SetOperationStmt *setop = (SetOperationStmt *) node;

		/* 我们需要查看 groupClauses 中的操作符引用 */
		find_expr_references_walker((Node *) setop->groupClauses, context);
		/* fall through to examine child nodes */
	}
	else if (IsA(node, RangeTblFunction))
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) node;
		ListCell   *ct;

		/*
		 * 为 RECORD 函数的列定义列表中使用的任何数据类型和排序规则
		 * 添加引用。（其他情形
		 * 依赖函数本身应已足够。）
		 */
		foreach(ct, rtfunc->funccoltypes)
		{
			add_object_address(OCLASS_TYPE, lfirst_oid(ct), 0,
							   context->addrs);
		}
		foreach(ct, rtfunc->funccolcollations)
		{
			Oid			collid = lfirst_oid(ct);

			if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
				add_object_address(OCLASS_COLLATION, collid, 0,
								   context->addrs);
		}
	}
	else if (IsA(node, TableFunc))
	{
		TableFunc  *tf = (TableFunc *) node;
		ListCell   *ct;

		/*
		 * 为 TableFunc 中使用的数据类型和排序规则添加引用。
		 */
		foreach(ct, tf->coltypes)
		{
			add_object_address(OCLASS_TYPE, lfirst_oid(ct), 0,
							   context->addrs);
		}
		foreach(ct, tf->colcollations)
		{
			Oid			collid = lfirst_oid(ct);

			if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
				add_object_address(OCLASS_COLLATION, collid, 0,
								   context->addrs);
		}
	}
	else if (IsA(node, TableSampleClause))
	{
		TableSampleClause *tsc = (TableSampleClause *) node;

		add_object_address(OCLASS_PROC, tsc->tsmhandler, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}

	return expression_tree_walker(node, find_expr_references_walker,
								  (void *) context);
}

/*
 * find_expr_references_walker 子例程：处理对
 * RTE_FUNCTION RTE 的 Var 引用
 */
static void
process_function_rte_ref(RangeTblEntry *rte, AttrNumber attnum,
						 find_expr_references_context *context)
{
	int			atts_done = 0;
	ListCell   *lc;

	/*
	 * 识别哪个 RangeTblFunction 产生此 attnum，并看它是否
	 * 返回复合类型。若是，我们最好对
	 * 该复合类型（实际上是其关联
	 * 关系）的被引用列建立依赖。
	 */
	foreach(lc, rte->functions)
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

		if (attnum > atts_done &&
			attnum <= atts_done + rtfunc->funccolcount)
		{
			TupleDesc	tupdesc;

			/* 若它有 coldeflist，则必定返回 RECORD */
			if (rtfunc->funccolnames != NIL)
				tupdesc = NULL; /* 不必费力 */
			else
				tupdesc = get_expr_result_tupdesc(rtfunc->funcexpr, true);
			if (tupdesc && tupdesc->tdtypeid != RECORDOID)
			{
				/*
				 * 具名复合类型，因此个别列可能被
				 * 删除。对本列建立依赖。
				 */
				Oid			reltype = get_typ_typrelid(tupdesc->tdtypeid);

				Assert(attnum - atts_done <= tupdesc->natts);
				if (OidIsValid(reltype))	/* 这会失败吗？ */
					add_object_address(OCLASS_CLASS, reltype,
									   attnum - atts_done,
									   context->addrs);
				return;
			}
			/* 无事可做；函数的结果类型在别处处理 */
			return;
		}
		atts_done += rtfunc->funccolcount;
	}

	/* 若执行到这里，必定是在找序号列 */
	if (rte->funcordinality && attnum == atts_done + 1)
		return;

	/* 这大概不可能发生... */
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_COLUMN),
			 errmsg("column %d of relation \"%s\" does not exist",
					attnum, rte->eref->aliasname)));
}

/*
 * 给定依赖引用数组，消除任何重复。
 */
static void
eliminate_duplicate_dependencies(ObjectAddresses *addrs)
{
	ObjectAddress *priorobj;
	int			oldref,
				newrefs;

	/*
	 * 若数组有"extra"数据则不能排序，因为无法
	 * 保持同步。幸好不需要那种功能组合。
	 *
	 */
	Assert(!addrs->extras);

	if (addrs->numrefs <= 1)
		return;					/* 无事可做 */

	/* 排序引用使重复项相邻 */
	qsort(addrs->refs, addrs->numrefs, sizeof(ObjectAddress),
		  object_address_comparator);

	/* 去除重复 */
	priorobj = addrs->refs;
	newrefs = 1;
	for (oldref = 1; oldref < addrs->numrefs; oldref++)
	{
		ObjectAddress *thisobj = addrs->refs + oldref;

		if (priorobj->classId == thisobj->classId &&
			priorobj->objectId == thisobj->objectId)
		{
			if (priorobj->objectSubId == thisobj->objectSubId)
				continue;		/* 相同，因此丢弃 thisobj */

			/*
			 * 若我们既有对整个对象的引用，又有对同一对象某部分的
			 * 引用，则不需要整对象引用
			 * （例如，不必同时引用表 foo 和
			 * 列 foo.bar）。整对象引用在排序列表中总是
			 * 先出现。
			 */
			if (priorobj->objectSubId == 0)
			{
				/* 用部分引用替换整对象引用 */
				priorobj->objectSubId = thisobj->objectSubId;
				continue;
			}
		}
		/* 不相同，因此把 thisobj 加入输出集 */
		priorobj++;
		*priorobj = *thisobj;
		newrefs++;
	}

	addrs->numrefs = newrefs;
}

/*
 * ObjectAddress 项的 qsort 比较器
 */
static int
object_address_comparator(const void *a, const void *b)
{
	const ObjectAddress *obja = (const ObjectAddress *) a;
	const ObjectAddress *objb = (const ObjectAddress *) b;

	/*
	 * 主排序键是 OID 降序。多数时候这会把
	 * 较新对象放在较旧对象之前，这很可能就是
	 * 正确的删除顺序。
	 */
	if (obja->objectId > objb->objectId)
		return -1;
	if (obja->objectId < objb->objectId)
		return 1;

	/*
	 * 其次按目录 ID 排序，以防相同 OIDs 出现在不同
	 * 目录中。这里的排序方向相当任意。
	 */
	if (obja->classId < objb->classId)
		return -1;
	if (obja->classId > objb->classId)
		return 1;

	/*
	 * 最后按对象 subId 排序。
	 *
	 * 我们把 subId 当作无符号 int 排序，使 0（整个对象）
	 * 排在最前。这对 eliminate_duplicate_dependencies 必不可少，
	 * 也是 findDependentObjects 的最佳顺序。
	 */
	if ((unsigned int) obja->objectSubId < (unsigned int) objb->objectSubId)
		return -1;
	if ((unsigned int) obja->objectSubId > (unsigned int) objb->objectSubId)
		return 1;
	return 0;
}

/*
 * 处理可扩展 ObjectAddress 项数组的例程。
 *
 * new_object_addresses: 创建新的 ObjectAddresses 数组。
 */
ObjectAddresses *
new_object_addresses(void)
{
	ObjectAddresses *addrs;

	addrs = palloc(sizeof(ObjectAddresses));

	addrs->numrefs = 0;
	addrs->maxrefs = 32;
	addrs->refs = (ObjectAddress *)
		palloc(addrs->maxrefs * sizeof(ObjectAddress));
	addrs->extras = NULL;		/* 直到/除非需要 */

	return addrs;
}

/*
 * 向 ObjectAddresses 数组添加一条条目。
 *
 * 用 ObjectClass 指定类比直接用目录 OID
 * 方便。
 */
static void
add_object_address(ObjectClass oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/* 若需要则扩大数组 */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
		Assert(!addrs->extras);
	}
	/* 记录此项 */
	item = addrs->refs + addrs->numrefs;
	item->classId = object_classes[oclass];
	item->objectId = objectId;
	item->objectSubId = subId;
	addrs->numrefs++;
}

/*
 * 向 ObjectAddresses 数组添加一条条目。
 *
 * 同上，但精确指定条目。
 */
void
add_exact_object_address(const ObjectAddress *object,
						 ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/* 若需要则扩大数组 */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
		Assert(!addrs->extras);
	}
	/* 记录此项 */
	item = addrs->refs + addrs->numrefs;
	*item = *object;
	addrs->numrefs++;
}

/*
 * 向 ObjectAddresses 数组添加一条条目。
 *
 * 同上，但精确指定条目并同时提供一些"extra"数据。
 */
static void
add_exact_object_address_extra(const ObjectAddress *object,
							   const ObjectAddressExtra *extra,
							   ObjectAddresses *addrs)
{
	ObjectAddress *item;
	ObjectAddressExtra *itemextra;

	/* 第一次则分配 extra 空间 */
	if (!addrs->extras)
		addrs->extras = (ObjectAddressExtra *)
			palloc(addrs->maxrefs * sizeof(ObjectAddressExtra));

	/* 若需要则扩大数组 */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
		addrs->extras = (ObjectAddressExtra *)
			repalloc(addrs->extras, addrs->maxrefs * sizeof(ObjectAddressExtra));
	}
	/* 记录此项 */
	item = addrs->refs + addrs->numrefs;
	*item = *object;
	itemextra = addrs->extras + addrs->numrefs;
	*itemextra = *extra;
	addrs->numrefs++;
}

/*
 * 测试对象是否出现在 ObjectAddresses 数组中。
 *
 * 若对象是数组中某物的子对象，我们也返回 "true"。
 */
bool
object_address_present(const ObjectAddress *object,
					   const ObjectAddresses *addrs)
{
	int			i;

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		const ObjectAddress *thisobj = addrs->refs + i;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId ||
				thisobj->objectSubId == 0)
				return true;
		}
	}

	return false;
}

/*
 * 同上，只不过若对象存在则还把给定
 * 标志 OR 进其关联的 extra 数据（必须存在）。
 */
static bool
object_address_present_add_flags(const ObjectAddress *object,
								 int flags,
								 ObjectAddresses *addrs)
{
	bool		result = false;
	int			i;

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		ObjectAddress *thisobj = addrs->refs + i;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId)
			{
				ObjectAddressExtra *thisextra = addrs->extras + i;

				thisextra->flags |= flags;
				result = true;
			}
			else if (thisobj->objectSubId == 0)
			{
				/*
				 * 我们到达这里是因为已经决定删除整张表之后，
				 * 又发现需要删除其中一列。显然
				 * 我们不再需要删除该子对象，因此报告
				 * 在数组中找到了该子对象。但不要把它的
				 * 标志贴到整个对象上。
				 */
				result = true;
			}
			else if (object->objectSubId == 0)
			{
				/*
				 * 我们到达这里是因为已经决定删除其中一列之后，
				 * 又发现需要删除整张表。我们
				 * 无法报告整个对象已在数组中，但
				 * 应以整个对象的标志标记该子对象。
				 *
				 * 从数组中物理删除该列的
				 * 条目，或至少标记它不再需要
				 * 单独删除，看似很诱人。但那可能导致例如在删除表之前
				 * 先删除该列的数据类型，这
				 * 似乎不是好主意。实践中这非常罕见，
				 * 因此我们宁愿承担单独执行
				 * DROP COLUMN 动作的代价，即使我们知道稍后
				 * 会删除该表。
				 *
				 * 不过我们可以把这标记为子对象，以免
				 * 单独报告它，否则会令人困惑，因为
				 * 是否发生无法预测。但仅当
				 * flags != 0 时才这样做（flags == 0 是只读探测）。
				 *
				 * 因为数组中还可能有本对象的其他子对象，
				 * 这种情形意味着我们必须始终循环完整
				 * 个数组；无法在匹配时提前退出。
				 */
				ObjectAddressExtra *thisextra = addrs->extras + i;

				if (flags)
					thisextra->flags |= (flags | DEPFLAG_SUBOBJECT);
			}
		}
	}

	return result;
}

/*
 * 与上面类似，只不过我们搜索 ObjectAddressStack。
 */
static bool
stack_address_present_add_flags(const ObjectAddress *object,
								int flags,
								ObjectAddressStack *stack)
{
	bool		result = false;
	ObjectAddressStack *stackptr;

	for (stackptr = stack; stackptr; stackptr = stackptr->next)
	{
		const ObjectAddress *thisobj = stackptr->object;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId)
			{
				stackptr->flags |= flags;
				result = true;
			}
			else if (thisobj->objectSubId == 0)
			{
				/*
				 * 我们正在访问一列，而整表已在栈上。
				 * 如同 object_address_present_add_flags()，可以跳过
				 * 对该子对象的进一步处理，但我们不想把
				 * 子对象的标志传播到整个对象。
				 */
				result = true;
			}
			else if (object->objectSubId == 0)
			{
				/*
				 * 我们正在访问一张表，而某列已在栈上。如同
				 * object_address_present_add_flags()，我们应把
				 * 整个对象的标志传播到它的每个子对象。
				 */
				if (flags)
					stackptr->flags |= (flags | DEPFLAG_SUBOBJECT);
			}
		}
	}

	return result;
}

/*
 * 先去除任何重复，再从 ObjectAddresses 数组记录多条
 * 依赖。
 */
void
record_object_address_dependencies(const ObjectAddress *depender,
								   ObjectAddresses *referenced,
								   DependencyType behavior)
{
	eliminate_duplicate_dependencies(referenced);
	recordMultipleDependencies(depender,
							   referenced->refs, referenced->numrefs,
							   behavior);
}

/*
 * 对 ObjectAddresses 数组中的项排序。
 *
 * 主排序键是 OID 降序，因此多数情况下较新对象会列在
 * 前面。这主要用于确保回归测试输出稳定；
 * 若对象顺序由用户输入决定，例如 DROP 命令中
 * 目标的顺序，则不推荐。
 */
void
sort_object_addresses(ObjectAddresses *addrs)
{
	if (addrs->numrefs > 1)
		qsort(addrs->refs, addrs->numrefs,
			  sizeof(ObjectAddress),
			  object_address_comparator);
}

/*
 * 用完 ObjectAddresses 数组后清理。
 */
void
free_object_addresses(ObjectAddresses *addrs)
{
	pfree(addrs->refs);
	if (addrs->extras)
		pfree(addrs->extras);
	pfree(addrs);
}

/*
 * 确定由 objectAddress 标识的给定对象的类。
 *
 * 本函数本质上是 object_classes[]
 * 表的反向映射。因为 OIDs 不连续，我们实现为函数。
 */
ObjectClass
getObjectClass(const ObjectAddress *object)
{
	/* 只有 pg_class 条目可以有非零 objectSubId */
	if (object->classId != RelationRelationId &&
		object->objectSubId != 0)
		elog(ERROR, "invalid non-zero objectSubId for object class %u",
			 object->classId);

	switch (object->classId)
	{
		case RelationRelationId:
			/* 调用者必须检查 objectSubId */
			return OCLASS_CLASS;

		case ProcedureRelationId:
			return OCLASS_PROC;

		case TypeRelationId:
			return OCLASS_TYPE;

		case CastRelationId:
			return OCLASS_CAST;

		case CollationRelationId:
			return OCLASS_COLLATION;

		case ConstraintRelationId:
			return OCLASS_CONSTRAINT;

		case ConversionRelationId:
			return OCLASS_CONVERSION;

		case AttrDefaultRelationId:
			return OCLASS_DEFAULT;

		case LanguageRelationId:
			return OCLASS_LANGUAGE;

		case LargeObjectRelationId:
			return OCLASS_LARGEOBJECT;

		case OperatorRelationId:
			return OCLASS_OPERATOR;

		case OperatorClassRelationId:
			return OCLASS_OPCLASS;

		case OperatorFamilyRelationId:
			return OCLASS_OPFAMILY;

		case AccessMethodRelationId:
			return OCLASS_AM;

		case AccessMethodOperatorRelationId:
			return OCLASS_AMOP;

		case AccessMethodProcedureRelationId:
			return OCLASS_AMPROC;

		case RewriteRelationId:
			return OCLASS_REWRITE;

		case TriggerRelationId:
			return OCLASS_TRIGGER;

		case NamespaceRelationId:
			return OCLASS_SCHEMA;

		case StatisticExtRelationId:
			return OCLASS_STATISTIC_EXT;

		case TSParserRelationId:
			return OCLASS_TSPARSER;

		case TSDictionaryRelationId:
			return OCLASS_TSDICT;

		case TSTemplateRelationId:
			return OCLASS_TSTEMPLATE;

		case TSConfigRelationId:
			return OCLASS_TSCONFIG;

		case AuthIdRelationId:
			return OCLASS_ROLE;

		case AuthMemRelationId:
			return OCLASS_ROLE_MEMBERSHIP;

		case DatabaseRelationId:
			return OCLASS_DATABASE;

		case TableSpaceRelationId:
			return OCLASS_TBLSPACE;

		case ForeignDataWrapperRelationId:
			return OCLASS_FDW;

		case ForeignServerRelationId:
			return OCLASS_FOREIGN_SERVER;

		case UserMappingRelationId:
			return OCLASS_USER_MAPPING;

		case DefaultAclRelationId:
			return OCLASS_DEFACL;

		case ExtensionRelationId:
			return OCLASS_EXTENSION;

		case EventTriggerRelationId:
			return OCLASS_EVENT_TRIGGER;

		case ParameterAclRelationId:
			return OCLASS_PARAMETER_ACL;

		case PolicyRelationId:
			return OCLASS_POLICY;

		case PublicationNamespaceRelationId:
			return OCLASS_PUBLICATION_NAMESPACE;

		case PublicationRelationId:
			return OCLASS_PUBLICATION;

		case PublicationRelRelationId:
			return OCLASS_PUBLICATION_REL;

		case SubscriptionRelationId:
			return OCLASS_SUBSCRIPTION;

		case TransformRelationId:
			return OCLASS_TRANSFORM;
	}

	/* 不应到达这里 */
	elog(ERROR, "unrecognized object class: %u", object->classId);
	return OCLASS_CLASS;		/* 让编译器安静 */
}

/*
 * 删除扩展对象的初始 ACL
 */
static void
DeleteInitPrivs(const ObjectAddress *object)
{
	Relation	relation;
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	oldtuple;

	relation = table_open(InitPrivsRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_init_privs_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&key[1],
				Anum_pg_init_privs_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&key[2],
					Anum_pg_init_privs_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(relation, InitPrivsObjIndexId, true,
							  NULL, nkeys, key);

	while (HeapTupleIsValid(oldtuple = systable_getnext(scan)))
		CatalogTupleDelete(relation, &oldtuple->t_self);

	systable_endscan(scan);

	table_close(relation, RowExclusiveLock);
}
