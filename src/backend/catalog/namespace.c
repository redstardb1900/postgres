/*-------------------------------------------------------------------------
 *
 * namespace.c
 *	  用于支持访问和搜索命名空间的代码
 *
 * 本模块与 pg_namespace.c 分开，后者包含直接操作
 * pg_namespace 系统目录的例程。此模块提供与定义
 * "命名空间搜索路径" 以及实现由搜索路径控制的查找
 * 相关的例程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/namespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/sinvaladt.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc_hooks.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * 命名空间搜索路径是一个可能为空的命名空间 OIDs 列表。
 * 除了显式列表之外，还可能包含隐式搜索的
 * 命名空间：
 *
 * 1. 如果本会话中已初始化了 TEMP 表命名空间，
 * 则首先隐式搜索它。（唯一不这样做的情况是
 * 当我们遵循一个覆盖搜索路径规范，其中指定不使用 temp
 * 命名空间，或者 temp 命名空间被包含在显式列表中。）
 *
 * 2. 系统目录命名空间总是被搜索。如果系统
 * 命名空间存在于显式路径中，则按指定的顺序
 * 搜索它；否则它将在 TEMP 表之后、*先于*显式列表
 * 被搜索。（系统命名空间似乎应当隐式地排在最后，
 * 但这种行为似乎是 SQL99 所要求的。此外，这提供了
 * 先搜索系统命名空间的方法，而不会因此使其成为
 * 默认的创建目标命名空间。）
 *
 * 出于安全原因，使用搜索路径进行搜索时，对于除关系和类型
 * 以外的任何对象类型，都会忽略 temp 命名空间。
 * （我们必须允许类型，因为临时表具有行类型。）
 *
 * 默认的创建目标命名空间始终是显式列表的
 * 第一个元素。如果显式列表为空，则没有默认目标。
 *
 * search_path 的文本规范可以包含 "$user"，用以引用
 * 与当前用户同名的命名空间（如果有的话）。（如果没有这样的
 * 命名空间，则忽略它。）	此外，它可以包含 "pg_temp"
 * 以引用当前后端的 temp 命名空间。如果 temp 命名空间尚未
 * 建立，这通常也可以忽略，但有一种特殊情况：
 * 如果 "pg_temp" 出现在最前面，那么它应当是默认的创建
 * 目标。我们对这种情况稍作变通，使得直到第一次尝试在其中
 * 创建对象之前，temp 命名空间都不会被建立。（这样变通的
 * 原因是我们无法在事务之外创建 temp 命名空间，
 * 而 search_path 的初始 GUC 处理发生在事务之外。）
 * 如果 "pg_temp" 出现在字符串最前面，但因为它尚未建立而
 * 未反映到 activeCreationNamespace 中，则
 * activeTempCreationPending 为 true。
 *
 * 在引导模式下，搜索路径被设置为等于 "pg_catalog"，因此
 * 系统命名空间是唯一被搜索或插入的命名空间。
 * initdb 也会小心地为其引导后的独立后端运行
 * 将 search_path 设置为 "pg_catalog"。否则，默认搜索
 * 路径由 GUC 决定。出厂默认路径包含 PUBLIC
 * 命名空间（如果存在），其前面是用户的个人命名空间
 * （如果存在的话）。
 *
 * 我们支持一个 "覆盖" 搜索路径设置栈，供后端代码中
 * 特定部分使用。当覆盖栈非空时，会忽略
 * namespace_search_path。activeSearchPath 始终
 * 是实际活动的路径；它要么指向最顶层栈条目
 * 的搜索列表，要么指向 baseSearchPath，后者是从
 * namespace_search_path 派生出的列表。
 *
 * 如果 baseSearchPathValid 为 false，则需要根据
 * namespace_search_path 重新计算 baseSearchPath（以及其他
 * 派生变量）。当对 namespace_search_path 赋值或收到
 * pg_namespace 的系统缓存失效事件时，我们将其标记为无效。
 * 重新计算会在下一次未被覆盖的查找尝试期间完成。请注意，
 * 覆盖规范从不接受重新计算。
 *
 * namespace_search_path 中提到的、当前用户 ID 无法读取的
 * 任何命名空间都会被直接排除在 baseSearchPath 之外；因此
 * 当当前用户 ID 变化时，我们必须愿意重新计算路径。
 * namespaceUser 是该路径为之计算的用户 ID。
 *
 * 注意：这些 List 变量指向的所有数据都位于 TopMemoryContext 中。
 *
 * activeSearchPath/activeCreationNamespace/activeTempCreationPending
 * 的有效值发生变化时，activePathGeneration 会递增。
 * 这可以用来快速检测自上次检查搜索路径状态以来
 * 是否发生过任何变化。
 */

/* 这些变量定义实际的活动状态： */

static List *activeSearchPath = NIL;

/* 创建对象的默认位置；如果为 InvalidOid，则没有默认值 */
static Oid	activeCreationNamespace = InvalidOid;

/* 如果为 true，则 activeCreationNamespace 有误，它应当是 temp 命名空间 */
static bool activeTempCreationPending = false;

/* 当前代计数器；确保它永远不为零 */
static uint64 activePathGeneration = 1;

/* 这些变量是最近从 namespace_search_path 派生的值： */

static List *baseSearchPath = NIL;

static Oid	baseCreationNamespace = InvalidOid;

static bool baseTempCreationPending = false;

static Oid	namespaceUser = InvalidOid;

/* 上述四个值仅在 baseSearchPathValid 为真时有效 */
static bool baseSearchPathValid = true;

/* 覆盖请求保存在一个 OverrideStackEntry 结构体栈中 */

typedef struct
{
	List	   *searchPath;		/* 期望的搜索路径 */
	Oid			creationNamespace;	/* 期望的创建命名空间 */
	int			nestLevel;		/* 子事务嵌套层级 */
} OverrideStackEntry;

static List *overrideStack = NIL;

/*
 * myTempNamespace 为 InvalidOid，直到某个特定后端会话中
 * 设置 TEMP 命名空间为止（这会在首次执行 CREATE TEMP TABLE
 * 命令时发生）。此后它就是该 temp 命名空间的 OID。
 *
 * myTempToastNamespace 是用于我的临时表的 toast 表的
 * 命名空间 OID。它随 myTempNamespace 一起设置，在此之前为 InvalidOid。
 *
 * myTempNamespaceSubID 显示我们是否已在当前子事务中创建了
 * TEMP 命名空间。该标志会沿子事务树向上传播，
 * 因此如果所有中间子事务都提交，主事务将能
 * 正确识别该标志。当它为 InvalidSubTransactionId 时，
 * 我们要么尚未创建 TEMP 命名空间，要么已成功提交
 * 其创建，具体取决于 myTempNamespace 是否有效。
 */
static Oid	myTempNamespace = InvalidOid;

static Oid	myTempToastNamespace = InvalidOid;

static SubTransactionId myTempNamespaceSubID = InvalidSubTransactionId;

/*
 * 这是用户文本形式的搜索路径规范 --- 即 GUC 变量
 * 'search_path' 的值。
 */
char	   *namespace_search_path = NULL;


/* 本地函数 */
static void recomputeNamespacePath(void);
static void AccessTempTableNamespace(bool force);
static void InitTempTableNamespace(void);
static void RemoveTempRelations(Oid tempNamespaceId);
static void RemoveTempRelationsCallback(int code, Datum arg);
static void NamespaceCallback(Datum arg, int cacheid, uint32 hashvalue);
static bool MatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
						   bool include_out_arguments, int pronargs,
						   int **argnumbers);


/*
 * RangeVarGetRelidExtended
 *		给定一个描述现有关系的 RangeVar，
 *		选择正确的命名空间并查找该关系 OID。
 *
 * 如果找不到模式或关系，则当 flags 包含 RVR_MISSING_OK
 * 时返回 InvalidOid，否则抛出错误。
 *
 * 如果 flags 包含 RVR_NOWAIT，则在必须等待锁时
 * 抛出错误。
 *
 * 如果 flags 包含 RVR_SKIP_LOCKED，则在必须等待
 * 锁时返回 InvalidOid。
 *
 * flags 不能同时包含 RVR_NOWAIT 和 RVR_SKIP_LOCKED。
 *
 * 注意，如果同时指定了 RVR_MISSING_OK 和 RVR_SKIP_LOCKED，
 * 那么返回值为 InvalidOid 可能意味着该关系不存在，
 * 也可能意味着无法锁定它。
 *
 * 回调允许调用方在获取关系锁之前检查权限
 * 或获取额外的锁。
 */
Oid
RangeVarGetRelidExtended(const RangeVar *relation, LOCKMODE lockmode,
						 uint32 flags,
						 RangeVarGetRelidCallback callback, void *callback_arg)
{
	uint64		inval_count;
	Oid			relId;
	Oid			oldRelId = InvalidOid;
	bool		retry = false;
	bool		missing_ok = (flags & RVR_MISSING_OK) != 0;

	/* 验证 flags 不冲突 */
	Assert(!((flags & RVR_NOWAIT) && (flags & RVR_SKIP_LOCKED)));

	/*
	 * 我们检查目录名，然后忽略它。
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							relation->catalogname, relation->schemaname,
							relation->relname)));
	}

	/*
	 * DDL 操作可能改变名称查找的结果。由于所有这类
	 * 操作都会产生失效消息，我们会跟踪在
	 * 执行操作期间是否出现任何此类消息，
	 * 并不断重试，直到要么 (1) 不再出现失效消息，要么 (2)
	 * 答案不再变化。
	 *
	 * 但如果 lockmode = NoLock，那么我们假设要么调用方
	 * 可以接受答案在期间发生变化，要么他们已经持有
	 * 某个合适的锁，因此不检查失效消息，直接返回
	 * 首次得到的答案。此外，如果所请求的锁
	 * 已被持有，LockRelationOid 不会处理失效消息，
	 * 因此我们可能无法察觉变化。我们可以通过在开始
	 * 此循环之前调用 AcceptInvalidationMessages() 来防止这种情况，
	 * 但那会带来相当大的开销，所以目前我们不这样做。
	 */
	for (;;)
	{
		/*
		 * 记住这个值，以便在查找关系名并锁定其 OID 之后，
		 * 我们可以检查是否处理过任何可能需要重做的
		 * 失效消息。
		 */
		inval_count = SharedInvalidMessageCounter;

		/*
		 * 可能指定了某个非默认的 relpersistence 值。
		 * 解析器在简单 DML 中从不会生成这样的 RangeVar，
		 * 但它可能出现在诸如 "CREATE TEMP TABLE foo (f1 int PRIMARY
		 * KEY)" 这样的上下文中。这样的命令会生成一个额外的 CREATE INDEX
		 * 操作，该操作必须小心地找到临时表，即使
		 * pg_temp 不在搜索路径的最前面。
		 */
		if (relation->relpersistence == RELPERSISTENCE_TEMP)
		{
			if (!OidIsValid(myTempNamespace))
				relId = InvalidOid; /* 这大概不会发生？ */
			else
			{
				if (relation->schemaname)
				{
					Oid			namespaceId;

					namespaceId = LookupExplicitNamespace(relation->schemaname, missing_ok);

					/*
					 * 对于 missing_ok，允许不存在的模式名
					 * 返回 InvalidOid。
					 */
					if (namespaceId != myTempNamespace)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("temporary tables cannot specify a schema name")));
				}

				relId = get_relname_relid(relation->relname, myTempNamespace);
			}
		}
		else if (relation->schemaname)
		{
			Oid			namespaceId;

			/* 使用给定的确切模式 */
			namespaceId = LookupExplicitNamespace(relation->schemaname, missing_ok);
			if (missing_ok && !OidIsValid(namespaceId))
				relId = InvalidOid;
			else
				relId = get_relname_relid(relation->relname, namespaceId);
		}
		else
		{
			/* 搜索命名空间路径 */
			relId = RelnameGetRelid(relation->relname);
		}

		/*
		 * 调用调用方提供的回调（如果有）。
		 *
		 * 这个回调是检查权限的好地方：我们还没有
		 * 获取表锁（而且最好在锁定任何东西之前
		 * 就检查权限！），但我们已经走得足够远，知道
		 * 我们认为应当锁定哪个 OID。当然，并发的 DDL 可能
		 * 在我们等待锁期间改变情况，但在那种情况下
		 * 回调会针对新的 OID 再次被调用。
		 */
		if (callback)
			callback(relation, relId, oldRelId, callback_arg);

		/*
		 * 如果未请求锁，我们假设调用方知道自己在
		 * 做什么。他们应该在本条语句处理的更早阶段已经
		 * 在这个关系上获取了重量级锁，因此
		 * 在这里调用 AcceptInvalidationMessages() 并不合适，因为
		 * 那可能会使他们措手不及。
		 */
		if (lockmode == NoLock)
			break;

		/*
		 * 如果在重试时得到与上次相同的 OID，那么
		 * 我们处理过的失效消息没有改变最终答案。
		 * 因此我们完成了。
		 *
		 * 如果得到不同的 OID，那么锁定的是曾
		 * 拥有此名称的关系，而不是现在拥有的那个。因此释放
		 * 该锁。
		 */
		if (retry)
		{
			if (relId == oldRelId)
				break;
			if (OidIsValid(oldRelId))
				UnlockRelationOid(oldRelId, lockmode);
		}

		/*
		 * 锁定关系。这也会接受任何待处理的失效
		 * 消息。如果我们得到 InvalidOid，表示未找到，那么
		 * 没有什么需要锁定，但我们仍接受失效消息，
		 * 以便清除任何可能残留的
		 * 否定 catcache 项。
		 */
		if (!OidIsValid(relId))
			AcceptInvalidationMessages();
		else if (!(flags & (RVR_NOWAIT | RVR_SKIP_LOCKED)))
			LockRelationOid(relId, lockmode);
		else if (!ConditionalLockRelationOid(relId, lockmode))
		{
			int			elevel = (flags & RVR_SKIP_LOCKED) ? DEBUG1 : ERROR;

			if (relation->schemaname)
				ereport(elevel,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s.%s\"",
								relation->schemaname, relation->relname)));
			else
				ereport(elevel,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relation->relname)));

			return InvalidOid;
		}

		/*
		 * 如果没有处理任何失效消息，我们就完成了！
		 */
		if (inval_count == SharedInvalidMessageCounter)
			break;

		/*
		 * 可能发生了变化。让我们重复名称查找，以
		 * 确保此名称仍然引用它之前引用的同一个
		 * 关系。
		 */
		retry = true;
		oldRelId = relId;
	}

	if (!OidIsValid(relId))
	{
		int			elevel = missing_ok ? DEBUG1 : ERROR;

		if (relation->schemaname)
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" does not exist",
							relation->relname)));
	}
	return relId;
}

/*
 * RangeVarGetCreationNamespace
 *		给定一个描述待创建关系的 RangeVar，
 *		选择要在哪个命名空间中创建它。
 *
 * 注意：调用此函数可能导致 CommandCounterIncrement 操作。
 * 这会在任何特定后端运行中首次请求临时表时发生；
 * 我们将需要创建或清理该临时模式。
 */
Oid
RangeVarGetCreationNamespace(const RangeVar *newRelation)
{
	Oid			namespaceId;

	/*
	 * 我们检查目录名，然后忽略它。
	 */
	if (newRelation->catalogname)
	{
		if (strcmp(newRelation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							newRelation->catalogname, newRelation->schemaname,
							newRelation->relname)));
	}

	if (newRelation->schemaname)
	{
		/* 检查 pg_temp 别名 */
		if (strcmp(newRelation->schemaname, "pg_temp") == 0)
		{
			/* 初始化 temp 命名空间 */
			AccessTempTableNamespace(false);
			return myTempNamespace;
		}
		/* 使用给定的确切模式 */
		namespaceId = get_namespace_oid(newRelation->schemaname, false);
		/* 我们在这里不检查 USAGE 权限！ */
	}
	else if (newRelation->relpersistence == RELPERSISTENCE_TEMP)
	{
		/* 初始化 temp 命名空间 */
		AccessTempTableNamespace(false);
		return myTempNamespace;
	}
	else
	{
		/* 使用默认的创建命名空间 */
		recomputeNamespacePath();
		if (activeTempCreationPending)
		{
			/* 需要初始化 temp 命名空间 */
			AccessTempTableNamespace(true);
			return myTempNamespace;
		}
		namespaceId = activeCreationNamespace;
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
	}

	/* 注意：调用方会在适当时检查 CREATE 权限 */

	return namespaceId;
}

/*
 * RangeVarGetAndCheckCreationNamespace
 *
 * 此函数返回应在其中创建具有给定名称的新关系的
 * 命名空间 OID。如果用户对目标命名空间没有 CREATE
 * 权限，则此函数会改为抛出
 * ERROR。
 *
 * 如果非 NULL，*existing_relation_id 会被设置为该命名空间中
 * 已存在的任何同名关系的 OID，如果不存在这样的
 * 关系则为 InvalidOid。
 *
 * 如果 lockmode != NoLock，则在现有关系（如果有）上获取指定的
 * 锁模式，前提是当前用户拥有目标关系。
 * 但是，如果 lockmode != NoLock 且用户不拥有目标
 * 关系，我们会抛出 ERROR，因为我们不得尝试锁定
 * 用户没有权限的关系。
 *
 * 作为副作用，此函数会在目标命名空间上获取 AccessShareLock。
 * 否则，该命名空间可能在我们的事务提交之前被删除，
 * 从而留下 relnamespace 指向一个不再存在的
 * 命名空间的关系。
 *
 * 作为进一步的副作用，如果选定的命名空间是临时命名空间，
 * 我们会将该 RangeVar 标记为 RELPERSISTENCE_TEMP。
 */
Oid
RangeVarGetAndCheckCreationNamespace(RangeVar *relation,
									 LOCKMODE lockmode,
									 Oid *existing_relation_id)
{
	uint64		inval_count;
	Oid			relid;
	Oid			oldrelid = InvalidOid;
	Oid			nspid;
	Oid			oldnspid = InvalidOid;
	bool		retry = false;

	/*
	 * 我们检查目录名，然后忽略它。
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							relation->catalogname, relation->schemaname,
							relation->relname)));
	}

	/*
	 * 与 RangeVarGetRelidExtended() 中一样，我们通过跟踪在进行名称
	 * 查找和获取锁期间是否处理了任何失效消息，来防范并发的
	 * DDL 操作。有关此逻辑的更详细说明，
	 * 请参见该函数中的注释。
	 */
	for (;;)
	{
		AclResult	aclresult;

		inval_count = SharedInvalidMessageCounter;

		/* 查找创建命名空间并检查是否存在已有关系。 */
		nspid = RangeVarGetCreationNamespace(relation);
		Assert(OidIsValid(nspid));
		if (existing_relation_id != NULL)
			relid = get_relname_relid(relation->relname, nspid);
		else
			relid = InvalidOid;

		/*
		 * 在引导处理模式下，我们不理会权限或
		 * 锁定。权限可能尚未正常工作，而锁定
		 * 则是不必要的。
		 */
		if (IsBootstrapProcessingMode())
			break;

		/* 检查命名空间权限。 */
		aclresult = object_aclcheck(NamespaceRelationId, nspid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(nspid));

		if (retry)
		{
			/* 如果没有变化，我们就完成了。 */
			if (relid == oldrelid && nspid == oldnspid)
				break;
			/* 如果创建命名空间已变化，则放弃旧锁。 */
			if (nspid != oldnspid)
				UnlockDatabaseObject(NamespaceRelationId, oldnspid, 0,
									 AccessShareLock);
			/* 如果名称指向不同的对象，则放弃旧锁。 */
			if (relid != oldrelid && OidIsValid(oldrelid) && lockmode != NoLock)
				UnlockRelationOid(oldrelid, lockmode);
		}

		/* 锁定命名空间。 */
		if (nspid != oldnspid)
			LockDatabaseObject(NamespaceRelationId, nspid, 0, AccessShareLock);

		/* 如果需要且有权限，则锁定关系。 */
		if (lockmode != NoLock && OidIsValid(relid))
		{
			if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)),
							   relation->relname);
			if (relid != oldrelid)
				LockRelationOid(relid, lockmode);
		}

		/* 如果没有处理任何失效消息，我们就完成了！ */
		if (inval_count == SharedInvalidMessageCounter)
			break;

		/* 可能发生了变化，因此重新检查我们的工作。 */
		retry = true;
		oldrelid = relid;
		oldnspid = nspid;
	}

	RangeVarAdjustRelationPersistence(relation, nspid);
	if (existing_relation_id != NULL)
		*existing_relation_id = relid;
	return nspid;
}

/*
 * 根据创建命名空间调整即将创建的关系的 relpersistence，
 * 并对无效的组合抛出错误。
 */
void
RangeVarAdjustRelationPersistence(RangeVar *newRelation, Oid nspid)
{
	switch (newRelation->relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			if (!isTempOrTempToastNamespace(nspid))
			{
				if (isAnyTempNamespace(nspid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("cannot create relations in temporary schemas of other sessions")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("cannot create temporary relation in non-temporary schema")));
			}
			break;
		case RELPERSISTENCE_PERMANENT:
			if (isTempOrTempToastNamespace(nspid))
				newRelation->relpersistence = RELPERSISTENCE_TEMP;
			else if (isAnyTempNamespace(nspid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot create relations in temporary schemas of other sessions")));
			break;
		default:
			if (isAnyTempNamespace(nspid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("only temporary relations may be created in temporary schemas")));
	}
}

/*
 * RelnameGetRelid
 *		尝试解析一个非限定关系名。
 *		如果在搜索路径中找到该关系，则返回 OID，否则返回 InvalidOid。
 */
Oid
RelnameGetRelid(const char *relname)
{
	Oid			relid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		relid = get_relname_relid(relname, namespaceId);
		if (OidIsValid(relid))
			return relid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}


/*
 * RelationIsVisible
 *		确定一个（由 OID 标识的）关系在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定关系名可以找到它"。
 */
bool
RelationIsVisible(Oid relid)
{
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			relnamespace;
	bool		visible;

	reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	relnamespace = relform->relnamespace;
	if (relnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, relnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名关系所隐藏。因此
		 * 我们必须进行一次慢速检查，看是否存在冲突的关系。
		 */
		char	   *relname = NameStr(relform->relname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == relnamespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (OidIsValid(get_relname_relid(relname, namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(reltup);

	return visible;
}


/*
 * TypenameGetTypid
 *		用于二进制兼容的包装函数。
 */
Oid
TypenameGetTypid(const char *typname)
{
	return TypenameGetTypidExtended(typname, true);
}

/*
 * TypenameGetTypidExtended
 *		尝试解析一个非限定数据类型名。
 *		如果在搜索路径中找到该类型，则返回 OID，否则返回 InvalidOid。
 *
 * 这基本上与 RelnameGetRelid 相同。
 */
Oid
TypenameGetTypidExtended(const char *typname, bool temp_ok)
{
	Oid			typid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (!temp_ok && namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		typid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								PointerGetDatum(typname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(typid))
			return typid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * TypeIsVisible
 *		确定一个（由 OID 标识的）类型在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定类型名可以找到它"。
 */
bool
TypeIsVisible(Oid typid)
{
	HeapTuple	typtup;
	Form_pg_type typform;
	Oid			typnamespace;
	bool		visible;

	typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typtup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typform = (Form_pg_type) GETSTRUCT(typtup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	typnamespace = typform->typnamespace;
	if (typnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, typnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名类型所隐藏。因此我们
		 * 必须进行一次慢速检查，看是否存在冲突的类型。
		 */
		char	   *typname = NameStr(typform->typname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == typnamespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TYPENAMENSP,
									  PointerGetDatum(typname),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(typtup);

	return visible;
}


/*
 * FuncnameGetCandidates
 *		给定一个可能限定的函数名和参数个数，
 *		检索可能匹配项的列表。
 *
 * 如果 nargs 为 -1，我们返回与给定名称匹配的所有函数，
 * 而不论参数个数。（在这种情况下 argnames 必须为 NIL，且
 * expand_variadic 和 expand_defaults 必须为 false。）
 *
 * 如果 argnames 不为 NIL，则我们考虑的是命名记法或混合记法的调用，
 * 只有拥有所有所列参数名的函数才会被返回。
 * （我们假设 length(argnames) <= nargs，且所有传入的名称互不相同。）
 * 返回的结构体将包含一个 argnumbers 数组，
 * 显示每个逻辑参数位置对应的实际参数索引。
 *
 * 如果 expand_variadic 为 true，则拥有相同数量或更少
 * 参数的变参函数也会被检索到，其中变参参数及任何
 * 额外参数位置用变参元素类型填充。
 * 返回结构体中的 nvargs 被设置为此类参数的个数。
 * 如果 expand_variadic 为 false，则变参参数不作特殊处理，
 * 返回的 nvargs 将始终为零。
 *
 * 如果 expand_defaults 为 true，则插入默认参数值后可能匹配的函数
 * 也会被检索到。在这种情况下，返回的结构体可能
 * 具有 nargs > 传入的 nargs，且 ndargs 被设置为
 * 额外参数的数量（可从函数的
 * proargdefaults 条目中获取）。
 *
 * 如果 include_out_arguments 为 true，则 OUT 模式参数被视为
 * 包含在参数列表中。它们的类型会包含在返回的
 * 数组中，且 argnumbers 是 proallargtypes 而非 proargtypes 中的索引。
 * 我们还会将 nominalnargs 设置为 proallargtypes 而非 proargtypes 的长度。
 * 否则 OUT 模式参数会被忽略。
 *
 * nvargs 和 ndargs 不可能在同一
 * 列表项中都非零，因为默认插入允许匹配具有多于
 * nargs 个参数的函数，而变参变换需要参数个数
 * 相同或更少。
 *
 * 当 argnames 不为 NIL 时，返回的 args[] 类型数组不是
 * 按函数声明排序的，而是按调用排序：
 * 先是任何位置参数，然后是命名参数，接着是默认
 * 参数（如果需要且 expand_defaults 允许）。argnumbers[]
 * 数组可用于将其映射回目录信息。
 * argnumbers[k] 被设置为第 k 个调用参数在
 * proargtypes 或 proallargtypes 中的索引。
 *
 * 如果函数名是限定的，我们只搜索单个命名空间，否则
 * 搜索搜索路径中的所有命名空间。在多命名空间的情况下，
 * 我们安排较早命名空间中的条目掩盖较晚命名空间中
 * 相同的条目。
 *
 * 在展开变参时，如果展开后的参数列表相同，我们安排
 * 非变参函数掩盖变参函数。不过，
 * 不同的变参函数之间仍可能
 * 存在冲突。
 *
 * 保证返回列表永远不会包含参数列表相同的
 * 多个条目。当 expand_defaults 为 true 时，条目
 * 可能超过 nargs 个位置，但我们仍然保证它们在前 nargs
 * 个位置上是不同的。但是，如果 argnames 不为 NIL，或者
 * expand_variadic 或 expand_defaults 为 true，则可能存在多个
 * 展开后参数列表相同的候选函数。与其
 * 在这里抛出错误，我们通过返回一个 oid = 0 的条目
 * 来报告这种情况，它代表这样一组冲突的候选。
 * 调用方最终可能仍会丢弃这样的条目，但如果它选择了
 * 这样的条目，就应当像该调用有歧义一样作出反应。
 *
 * 如果 missing_ok 为 true，则当名称被一个不存在的
 * 模式限定时，返回空列表 (NULL)。同样，如果因其他原因
 * 找不到候选，也返回空列表。
 */
FuncCandidateList
FuncnameGetCandidates(List *names, int nargs, List *argnames,
					  bool expand_variadic, bool expand_defaults,
					  bool include_out_arguments, bool missing_ok)
{
	FuncCandidateList resultList = NULL;
	bool		any_special = false;
	char	   *schemaname;
	char	   *funcname;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* 检查调用方错误 */
	Assert(nargs >= 0 || !(expand_variadic | expand_defaults));

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &funcname);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			return NULL;
	}
	else
	{
		/* 指示我们需要命名空间搜索的标志 */
		namespaceId = InvalidOid;
		recomputeNamespacePath();
	}

	/* 仅按名称搜索系统缓存 */
	catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		Oid		   *proargtypes = procform->proargtypes.values;
		int			pronargs = procform->pronargs;
		int			effective_nargs;
		int			pathpos = 0;
		bool		variadic;
		bool		use_defaults;
		Oid			va_elem_type;
		int		   *argnumbers = NULL;
		FuncCandidateList newResult;

		if (OidIsValid(namespaceId))
		{
			/* 仅考虑指定命名空间中的函数 */
			if (procform->pronamespace != namespaceId)
				continue;
		}
		else
		{
			/*
			 * 仅考虑位于搜索路径中且不在
			 * temp 命名空间中的函数。
			 */
			ListCell   *nsp;

			foreach(nsp, activeSearchPath)
			{
				if (procform->pronamespace == lfirst_oid(nsp) &&
					procform->pronamespace != myTempNamespace)
					break;
				pathpos++;
			}
			if (nsp == NULL)
				continue;		/* 函数不在搜索路径中 */
		}

		/*
		 * 如果要求我们匹配 OUT 参数，则使用
		 * proallargtypes 数组（它包含这些参数）；否则使用
		 * proargtypes（它不包含）。当然，如果 proallargtypes 为 null，
		 * 我们总是使用 proargtypes。
		 */
		if (include_out_arguments)
		{
			Datum		proallargtypes;
			bool		isNull;

			proallargtypes = SysCacheGetAttr(PROCNAMEARGSNSP, proctup,
											 Anum_pg_proc_proallargtypes,
											 &isNull);
			if (!isNull)
			{
				ArrayType  *arr = DatumGetArrayTypeP(proallargtypes);

				pronargs = ARR_DIMS(arr)[0];
				if (ARR_NDIM(arr) != 1 ||
					pronargs < 0 ||
					ARR_HASNULL(arr) ||
					ARR_ELEMTYPE(arr) != OIDOID)
					elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
				Assert(pronargs >= procform->pronargs);
				proargtypes = (Oid *) ARR_DATA_PTR(arr);
			}
		}

		if (argnames != NIL)
		{
			/*
			 * 调用使用命名记法或混合记法
			 *
			 * 命名记法或混合记法只有在
			 * expand_variadic 关闭时才能匹配变参函数；否则无法匹配
			 * 从变参数组展开出的假定无名的参数。
			 */
			if (OidIsValid(procform->provariadic) && expand_variadic)
				continue;
			va_elem_type = InvalidOid;
			variadic = false;

			/*
			 * 检查参数个数。
			 */
			Assert(nargs >= 0); /* 使用 argnames 时不支持 -1 */

			if (pronargs > nargs && expand_defaults)
			{
				/* 如果默认表达式不够则忽略 */
				if (nargs + procform->pronargdefaults < pronargs)
					continue;
				use_defaults = true;
			}
			else
				use_defaults = false;

			/* 如果与请求的参数个数不匹配则忽略 */
			if (pronargs != nargs && !use_defaults)
				continue;

			/* 检查参数名匹配，生成位置映射 */
			if (!MatchNamedCall(proctup, nargs, argnames,
								include_out_arguments, pronargs,
								&argnumbers))
				continue;

			/* 命名参数匹配总是 "特殊的" */
			any_special = true;
		}
		else
		{
			/*
			 * 调用使用位置记法
			 *
			 * 检查函数是否为变参，如果是，则获取变参元素类型。
			 * 如果 expand_variadic 为 false，我们应当直接忽略
			 * 变参性。
			 */
			if (pronargs <= nargs && expand_variadic)
			{
				va_elem_type = procform->provariadic;
				variadic = OidIsValid(va_elem_type);
				any_special |= variadic;
			}
			else
			{
				va_elem_type = InvalidOid;
				variadic = false;
			}

			/*
			 * 检查函数是否可以通过使用参数默认值来匹配。
			 */
			if (pronargs > nargs && expand_defaults)
			{
				/* 如果默认表达式不够则忽略 */
				if (nargs + procform->pronargdefaults < pronargs)
					continue;
				use_defaults = true;
				any_special = true;
			}
			else
				use_defaults = false;

			/* 如果与请求的参数个数不匹配则忽略 */
			if (nargs >= 0 && pronargs != nargs && !variadic && !use_defaults)
				continue;
		}

		/*
		 * 我们必须计算有效参数列表，以便能轻松
		 * 将其与更早的结果比较。如果它被更早的结果
		 * 掩盖，我们会浪费一个 palloc 周期，但那实际上
		 * 是相当少见的情况，所以不值得担心。
		 */
		effective_nargs = Max(pronargs, nargs);
		newResult = (FuncCandidateList)
			palloc(offsetof(struct _FuncCandidateList, args) +
				   effective_nargs * sizeof(Oid));
		newResult->pathpos = pathpos;
		newResult->oid = procform->oid;
		newResult->nominalnargs = pronargs;
		newResult->nargs = effective_nargs;
		newResult->argnumbers = argnumbers;
		if (argnumbers)
		{
			/* 将参数类型重新排列为调用的逻辑顺序 */
			for (int j = 0; j < pronargs; j++)
				newResult->args[j] = proargtypes[argnumbers[j]];
		}
		else
		{
			/* 简单的位置情形，直接原样复制 proargtypes */
			memcpy(newResult->args, proargtypes, pronargs * sizeof(Oid));
		}
		if (variadic)
		{
			newResult->nvargs = effective_nargs - pronargs + 1;
			/* 将变参参数展开为 N 个元素类型的副本 */
			for (int j = pronargs - 1; j < effective_nargs; j++)
				newResult->args[j] = va_elem_type;
		}
		else
			newResult->nvargs = 0;
		newResult->ndargs = use_defaults ? pronargs - nargs : 0;

		/*
		 * 它是否具有与我们已接受的某个结果相同的参数？
		 * 如果是，决定如何处理，以避免返回重复的
		 * 参数列表。对于单命名空间的情形，如果没有进行任何
		 * 特殊（命名、变参或默认值）匹配，我们可以跳过此检查，
		 * 因为那样 pg_proc 上的唯一索引保证所有匹配项
		 * 具有不同的参数列表。
		 */
		if (resultList != NULL &&
			(any_special || !OidIsValid(namespaceId)))
		{
			/*
			 * 如果我们从 SearchSysCacheList 得到的是有序列表（通常
			 * 情况），那么任何冲突的函数必然紧邻此
			 * 条目，所以我们只需查看最新的结果
			 * 项。如果得到的是无序列表，则必须扫描整个
			 * 结果列表。此外，如果当前候选或任何
			 * 先前的候选是特殊匹配，我们就不能假定
			 * 冲突是相邻的。
			 *
			 * 在判断何为匹配时，我们忽略带默认值的参数。
			 */
			FuncCandidateList prevResult;

			if (catlist->ordered && !any_special)
			{
				/* 如果 !any_special，则 ndargs 必须为 0 */
				if (effective_nargs == resultList->nargs &&
					memcmp(newResult->args,
						   resultList->args,
						   effective_nargs * sizeof(Oid)) == 0)
					prevResult = resultList;
				else
					prevResult = NULL;
			}
			else
			{
				int			cmp_nargs = newResult->nargs - newResult->ndargs;

				for (prevResult = resultList;
					 prevResult;
					 prevResult = prevResult->next)
				{
					if (cmp_nargs == prevResult->nargs - prevResult->ndargs &&
						memcmp(newResult->args,
							   prevResult->args,
							   cmp_nargs * sizeof(Oid)) == 0)
						break;
				}
			}

			if (prevResult)
			{
				/*
				 * 我们与先前的结果匹配。决定保留
				 * 哪一个，或者在无法决定时将其标记为有歧义。
				 * 这里的逻辑是：preference > 0 表示偏好旧结果，
				 * preference < 0 表示偏好新结果，preference = 0 表示
				 * 有歧义。
				 */
				int			preference;

				if (pathpos != prevResult->pathpos)
				{
					/*
					 * 偏好搜索路径中更靠前的那个。
					 */
					preference = pathpos - prevResult->pathpos;
				}
				else if (variadic && prevResult->nvargs == 0)
				{
					/*
					 * 对于变参函数，例如可能在同一命名空间中
					 * 同时存在 foo(numeric) 和 foo(variadic numeric[])；
					 * 如果是这样，出于效率考虑我们偏好
					 * 非变参的匹配。
					 */
					preference = 1;
				}
				else if (!variadic && prevResult->nvargs > 0)
				{
					preference = -1;
				}
				else
				{
					/*----------
					 * 我们无法决定。这可能发生在，例如，
					 * 同一命名空间中同时存在 foo(numeric, variadic numeric[]) 和
					 * foo(variadic numeric[])，或者
					 * 同一命名空间中同时存在 foo(int) 和 foo (int, int default something)，
					 * 或者同一命名空间中同时存在 foo(a int, b text)
					 * 和 foo(b text, a int) 时。
					 *----------
					 */
					preference = 0;
				}

				if (preference > 0)
				{
					/* 保留先前的结果 */
					pfree(newResult);
					continue;
				}
				else if (preference < 0)
				{
					/* 从列表中移除先前的结果 */
					if (prevResult == resultList)
						resultList = prevResult->next;
					else
					{
						FuncCandidateList prevPrevResult;

						for (prevPrevResult = resultList;
							 prevPrevResult;
							 prevPrevResult = prevPrevResult->next)
						{
							if (prevResult == prevPrevResult->next)
							{
								prevPrevResult->next = prevResult->next;
								break;
							}
						}
						Assert(prevPrevResult); /* 断言我们找到了它 */
					}
					pfree(prevResult);
					/* fall through to add newResult to list */
				}
				else
				{
					/* 将旧结果标记为有歧义，丢弃新结果 */
					prevResult->oid = InvalidOid;
					pfree(newResult);
					continue;
				}
			}
		}

		/*
		 * 可以将其加入结果列表
		 */
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * MatchNamedCall
 *		给定一个 pg_proc 堆元组和调用的参数名列表，
 *		检查该函数是否可以匹配该调用。
 *
 * 如果函数接受所有提供的参数名，且这些名称位于
 * 最后一个位置参数之后，并且所有未提供的参数都有
 * 默认值，则该调用可以匹配。
 *
 * 如果 include_out_arguments 为 true，我们把 OUT 参数视为
 * 包含在参数列表中。pronargs 是我们正在考虑的
 * 参数个数（proargtypes 或 proallargtypes 之一的长度）。
 *
 * 位置参数的个数为 nargs - list_length(argnames)。
 * 注意调用方已经对参数个数做了基本检查。
 *
 * 匹配时，返回 true 并用一个 palloc 的数组填充 *argnumbers，
 * 该数组显示从调用参数位置到实际函数参数
 * 编号的映射。带默认值的参数也包含在此映射中，位于
 * 最后一个提供的参数之后。
 */
static bool
MatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
			   bool include_out_arguments, int pronargs,
			   int **argnumbers)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
	int			numposargs = nargs - list_length(argnames);
	int			pronallargs;
	Oid		   *p_argtypes;
	char	  **p_argnames;
	char	   *p_argmodes;
	bool		arggiven[FUNC_MAX_ARGS];
	bool		isnull;
	int			ap;				/* 调用参数位置 */
	int			pp;				/* proargs 位置 */
	ListCell   *lc;

	Assert(argnames != NIL);
	Assert(numposargs >= 0);
	Assert(nargs <= pronargs);

	/* 如果函数的 proargnames 为 null 则忽略它 */
	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proargnames,
						   &isnull);
	if (isnull)
		return false;

	/* 好，让我们提取参数名和类型 */
	pronallargs = get_func_arg_info(proctup,
									&p_argtypes, &p_argnames, &p_argmodes);
	Assert(p_argnames != NULL);

	Assert(include_out_arguments ? (pronargs == pronallargs) : (pronargs <= pronallargs));

	/* 初始化用于匹配的状态 */
	*argnumbers = (int *) palloc(pronargs * sizeof(int));
	memset(arggiven, false, pronargs * sizeof(bool));

	/* 命名参数之前有 numposargs 个位置参数 */
	for (ap = 0; ap < numposargs; ap++)
	{
		(*argnumbers)[ap] = ap;
		arggiven[ap] = true;
	}

	/* 现在检查命名参数 */
	foreach(lc, argnames)
	{
		char	   *argname = (char *) lfirst(lc);
		bool		found;
		int			i;

		pp = 0;
		found = false;
		for (i = 0; i < pronallargs; i++)
		{
			/* 仅考虑输入参数，除非使用 include_out_arguments */
			if (!include_out_arguments &&
				p_argmodes &&
				(p_argmodes[i] != FUNC_PARAM_IN &&
				 p_argmodes[i] != FUNC_PARAM_INOUT &&
				 p_argmodes[i] != FUNC_PARAM_VARIADIC))
				continue;
			if (p_argnames[i] && strcmp(p_argnames[i], argname) == 0)
			{
				/* 如果参数名匹配某个位置参数则失败 */
				if (arggiven[pp])
					return false;
				arggiven[pp] = true;
				(*argnumbers)[ap] = pp;
				found = true;
				break;
			}
			/* 仅对纳入考虑的参数递增 pp */
			pp++;
		}
		/* 如果名称不在 proargnames 中则失败 */
		if (!found)
			return false;
		ap++;
	}

	Assert(ap == nargs);		/* 已处理所有实际参数 */

	/* 检查默认参数 */
	if (nargs < pronargs)
	{
		int			first_arg_with_default = pronargs - procform->pronargdefaults;

		for (pp = numposargs; pp < pronargs; pp++)
		{
			if (arggiven[pp])
				continue;
			/* 如果参数未给出且没有可用默认值则失败 */
			if (pp < first_arg_with_default)
				return false;
			(*argnumbers)[ap++] = pp;
		}
	}

	Assert(ap == pronargs);		/* 已处理所有函数参数 */

	return true;
}

/*
 * FunctionIsVisible
 *		确定一个（由 OID 标识的）函数在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定函数名并精确匹配参数可以找到它"。
 */
bool
FunctionIsVisible(Oid funcid)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	Oid			pronamespace;
	bool		visible;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	pronamespace = procform->pronamespace;
	if (pronamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, pronamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名同参数函数所隐藏。
		 * 因此我们必须进行一次慢速检查，看这是否与
		 * FuncnameGetCandidates 会找到的是同一个函数。
		 */
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		FuncCandidateList clist;

		visible = false;

		clist = FuncnameGetCandidates(list_make1(makeString(proname)),
									  nargs, NIL, false, false, false, false);

		for (; clist; clist = clist->next)
		{
			if (memcmp(clist->args, procform->proargtypes.values,
					   nargs * sizeof(Oid)) == 0)
			{
				/* 找到预期的条目；它是正确的函数吗？ */
				visible = (clist->oid == funcid);
				break;
			}
		}
	}

	ReleaseSysCache(proctup);

	return visible;
}


/*
 * OpernameGetOprid
 *		给定一个可能限定的运算符名和精确的输入数据类型，
 *		查找该运算符。如果未找到则返回 InvalidOid。
 *
 * 对于前缀运算符，传入 oprleft = InvalidOid。
 *
 * 如果运算符名未用模式限定，则在当前命名空间
 * 搜索路径中查找它。如果名称用模式限定，且给定的
 * 模式不存在，则返回 InvalidOid。
 */
Oid
OpernameGetOprid(List *names, Oid oprleft, Oid oprright)
{
	char	   *schemaname;
	char	   *opername;
	CatCList   *catlist;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* 仅在给定的确切模式中搜索 */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, true);
		if (OidIsValid(namespaceId))
		{
			HeapTuple	opertup;

			opertup = SearchSysCache4(OPERNAMENSP,
									  CStringGetDatum(opername),
									  ObjectIdGetDatum(oprleft),
									  ObjectIdGetDatum(oprright),
									  ObjectIdGetDatum(namespaceId));
			if (HeapTupleIsValid(opertup))
			{
				Form_pg_operator operclass = (Form_pg_operator) GETSTRUCT(opertup);
				Oid			result = operclass->oid;

				ReleaseSysCache(opertup);
				return result;
			}
		}

		return InvalidOid;
	}

	/* 按名称和参数类型搜索系统缓存 */
	catlist = SearchSysCacheList3(OPERNAMENSP,
								  CStringGetDatum(opername),
								  ObjectIdGetDatum(oprleft),
								  ObjectIdGetDatum(oprright));

	if (catlist->n_members == 0)
	{
		/* 没有希望，提前退出 */
		ReleaseSysCacheList(catlist);
		return InvalidOid;
	}

	/*
	 * 如果存在多个，我们必须找到搜索路径中
	 * 最靠前的那个列表成员。这个双重嵌套循环看起来
	 * 很难看，但实际上通常应当只有很少的 catlist 成员。
	 */
	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);
		int			i;

		if (namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		for (i = 0; i < catlist->n_members; i++)
		{
			HeapTuple	opertup = &catlist->members[i]->tuple;
			Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);

			if (operform->oprnamespace == namespaceId)
			{
				Oid			result = operform->oid;

				ReleaseSysCacheList(catlist);
				return result;
			}
		}
	}

	ReleaseSysCacheList(catlist);
	return InvalidOid;
}

/*
 * OpernameGetCandidates
 *		给定一个可能限定的运算符名和运算符种类，
 *		检索可能匹配项的列表。
 *
 * 如果 oprkind 为 '\0'，我们返回与给定名称匹配的所有运算符，
 * 而不论参数如何。
 *
 * 如果运算符名是限定的，我们只搜索单个命名空间，否则
 * 搜索搜索路径中的所有命名空间。返回列表永远不会包含
 * 参数列表相同的多个条目 --- 在多命名空间
 * 情况下，我们安排较早命名空间中的条目掩盖
 * 较晚命名空间中相同的条目。
 *
 * 返回的项始终有两个 args[] 条目 --- 对于前缀 oprkind
 * 第一个将是 InvalidOid。nargs 也始终为 2。
 */
FuncCandidateList
OpernameGetCandidates(List *names, char oprkind, bool missing_schema_ok)
{
	FuncCandidateList resultList = NULL;
	char	   *resultSpace = NULL;
	int			nextResult = 0;
	char	   *schemaname;
	char	   *opername;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_schema_ok);
		if (missing_schema_ok && !OidIsValid(namespaceId))
			return NULL;
	}
	else
	{
		/* 指示我们需要命名空间搜索的标志 */
		namespaceId = InvalidOid;
		recomputeNamespacePath();
	}

	/* 仅按名称搜索系统缓存 */
	catlist = SearchSysCacheList1(OPERNAMENSP, CStringGetDatum(opername));

	/*
	 * 在典型场景下，catcache 搜索找到的运算符
	 * 大多（即使不是全部）最终都会被返回；而对于诸如
	 * '=' 或 '+' 这样的常见运算符名，可能会有相当多。为减少
	 * 在 palloc 上花费的时间，我们将结果空间分配为一个
	 * 足以容纳所有运算符的数组。此例程最初的实现为每个
	 * 运算符单独做一次 palloc，但性能分析揭示这些 palloc
	 * 占用了不成比例的解析时间。
	 */
#define SPACE_PER_OP MAXALIGN(offsetof(struct _FuncCandidateList, args) + \
							  2 * sizeof(Oid))

	if (catlist->n_members > 0)
		resultSpace = palloc(catlist->n_members * SPACE_PER_OP);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	opertup = &catlist->members[i]->tuple;
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		int			pathpos = 0;
		FuncCandidateList newResult;

		/* 如果请求了特定种类，则忽略种类错误的运算符 */
		if (oprkind && operform->oprkind != oprkind)
			continue;

		if (OidIsValid(namespaceId))
		{
			/* 仅考虑指定命名空间中的运算符 */
			if (operform->oprnamespace != namespaceId)
				continue;
			/* 无需检查参数，它们必定都不同 */
		}
		else
		{
			/*
			 * 仅考虑位于搜索路径中且不在
			 * temp 命名空间中的运算符。
			 */
			ListCell   *nsp;

			foreach(nsp, activeSearchPath)
			{
				if (operform->oprnamespace == lfirst_oid(nsp) &&
					operform->oprnamespace != myTempNamespace)
					break;
				pathpos++;
			}
			if (nsp == NULL)
				continue;		/* 运算符不在搜索路径中 */

			/*
			 * 好，它在搜索路径中，但它的参数是否与我们已接受的
			 * 某个结果相同？如果是，只保留搜索路径中
			 * 更靠前的那个。
			 *
			 * 如果我们从 SearchSysCacheList 得到的是有序列表（通常
			 * 情况），那么任何冲突的运算符必然紧邻此条目，
			 * 所以我们只需查看最新的结果
			 * 项。如果得到的是无序列表，则必须扫描整个
			 * 结果列表。
			 */
			if (resultList)
			{
				FuncCandidateList prevResult;

				if (catlist->ordered)
				{
					if (operform->oprleft == resultList->args[0] &&
						operform->oprright == resultList->args[1])
						prevResult = resultList;
					else
						prevResult = NULL;
				}
				else
				{
					for (prevResult = resultList;
						 prevResult;
						 prevResult = prevResult->next)
					{
						if (operform->oprleft == prevResult->args[0] &&
							operform->oprright == prevResult->args[1])
							break;
					}
				}
				if (prevResult)
				{
					/* 我们与先前的结果匹配 */
					Assert(pathpos != prevResult->pathpos);
					if (pathpos > prevResult->pathpos)
						continue;	/* 保留先前的结果 */
					/* 替换先前的结果 */
					prevResult->pathpos = pathpos;
					prevResult->oid = operform->oid;
					continue;	/* 参数当然相同 */
				}
			}
		}

		/*
		 * 可以将其加入结果列表
		 */
		newResult = (FuncCandidateList) (resultSpace + nextResult);
		nextResult += SPACE_PER_OP;

		newResult->pathpos = pathpos;
		newResult->oid = operform->oid;
		newResult->nominalnargs = 2;
		newResult->nargs = 2;
		newResult->nvargs = 0;
		newResult->ndargs = 0;
		newResult->argnumbers = NULL;
		newResult->args[0] = operform->oprleft;
		newResult->args[1] = operform->oprright;
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * OperatorIsVisible
 *		确定一个（由 OID 标识的）运算符在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定运算符名并精确匹配参数可以找到它"。
 */
bool
OperatorIsVisible(Oid oprid)
{
	HeapTuple	oprtup;
	Form_pg_operator oprform;
	Oid			oprnamespace;
	bool		visible;

	oprtup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oprid));
	if (!HeapTupleIsValid(oprtup))
		elog(ERROR, "cache lookup failed for operator %u", oprid);
	oprform = (Form_pg_operator) GETSTRUCT(oprtup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	oprnamespace = oprform->oprnamespace;
	if (oprnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, oprnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名同参数运算符
		 * 所隐藏。因此我们必须进行一次慢速检查，看这是否
		 * 与 OpernameGetOprid 会找到的是同一个运算符。
		 */
		char	   *oprname = NameStr(oprform->oprname);

		visible = (OpernameGetOprid(list_make1(makeString(oprname)),
									oprform->oprleft, oprform->oprright)
				   == oprid);
	}

	ReleaseSysCache(oprtup);

	return visible;
}


/*
 * OpclassnameGetOpcid
 *		尝试解析一个非限定索引操作符类名。
 *		如果在搜索路径中找到该操作符类，则返回 OID，否则返回 InvalidOid。
 *
 * 这基本上与 TypenameGetTypid 相同，但我们必须为索引 AM OID
 * 多一个参数。
 */
Oid
OpclassnameGetOpcid(Oid amid, const char *opcname)
{
	Oid			opcid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		opcid = GetSysCacheOid3(CLAAMNAMENSP, Anum_pg_opclass_oid,
								ObjectIdGetDatum(amid),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(opcid))
			return opcid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * OpclassIsVisible
 *		确定一个（由 OID 标识的）操作符类在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定操作符类名可以找到它"。
 */
bool
OpclassIsVisible(Oid opcid)
{
	HeapTuple	opctup;
	Form_pg_opclass opcform;
	Oid			opcnamespace;
	bool		visible;

	opctup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opcid));
	if (!HeapTupleIsValid(opctup))
		elog(ERROR, "cache lookup failed for opclass %u", opcid);
	opcform = (Form_pg_opclass) GETSTRUCT(opctup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	opcnamespace = opcform->opcnamespace;
	if (opcnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, opcnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名操作符类所隐藏。
		 * 因此我们必须进行一次慢速检查，看此操作符类是否会被
		 * OpclassnameGetOpcid 找到。
		 */
		char	   *opcname = NameStr(opcform->opcname);

		visible = (OpclassnameGetOpcid(opcform->opcmethod, opcname) == opcid);
	}

	ReleaseSysCache(opctup);

	return visible;
}

/*
 * OpfamilynameGetOpfid
 *		尝试解析一个非限定索引操作符族名。
 *		如果在搜索路径中找到该操作符族，则返回 OID，否则返回 InvalidOid。
 *
 * 这基本上与 TypenameGetTypid 相同，但我们必须为索引 AM OID
 * 多一个参数。
 */
Oid
OpfamilynameGetOpfid(Oid amid, const char *opfname)
{
	Oid			opfid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		opfid = GetSysCacheOid3(OPFAMILYAMNAMENSP, Anum_pg_opfamily_oid,
								ObjectIdGetDatum(amid),
								PointerGetDatum(opfname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(opfid))
			return opfid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * OpfamilyIsVisible
 *		确定一个（由 OID 标识的）操作符族在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定操作符族名可以找到它"。
 */
bool
OpfamilyIsVisible(Oid opfid)
{
	HeapTuple	opftup;
	Form_pg_opfamily opfform;
	Oid			opfnamespace;
	bool		visible;

	opftup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));
	if (!HeapTupleIsValid(opftup))
		elog(ERROR, "cache lookup failed for opfamily %u", opfid);
	opfform = (Form_pg_opfamily) GETSTRUCT(opftup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	opfnamespace = opfform->opfnamespace;
	if (opfnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, opfnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名操作符族所隐藏。
		 * 因此我们必须进行一次慢速检查，看此操作符族是否会被
		 * OpfamilynameGetOpfid 找到。
		 */
		char	   *opfname = NameStr(opfform->opfname);

		visible = (OpfamilynameGetOpfid(opfform->opfmethod, opfname) == opfid);
	}

	ReleaseSysCache(opftup);

	return visible;
}

/*
 * lookup_collation
 *		如果存在给定名称/命名空间的排序规则，且它与
 *		给定编码兼容，则返回其 OID。否则返回 InvalidOid。
 */
static Oid
lookup_collation(const char *collname, Oid collnamespace, int32 encoding)
{
	Oid			collid;
	HeapTuple	colltup;
	Form_pg_collation collform;

	/* 检查特定编码的条目（精确匹配） */
	collid = GetSysCacheOid3(COLLNAMEENCNSP, Anum_pg_collation_oid,
							 PointerGetDatum(collname),
							 Int32GetDatum(encoding),
							 ObjectIdGetDatum(collnamespace));
	if (OidIsValid(collid))
		return collid;

	/*
	 * 检查任意编码的条目。这需要多做一些工作：虽然
	 * collencoding = -1 的 libc 排序规则适用于所有编码，但 ICU
	 * 排序规则只适用于某些编码，因此我们必须在判定
	 * 其匹配之前检查这一点。
	 */
	colltup = SearchSysCache3(COLLNAMEENCNSP,
							  PointerGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(collnamespace));
	if (!HeapTupleIsValid(colltup))
		return InvalidOid;
	collform = (Form_pg_collation) GETSTRUCT(colltup);
	if (collform->collprovider == COLLPROVIDER_ICU)
	{
		if (is_encoding_supported_by_icu(encoding))
			collid = collform->oid;
		else
			collid = InvalidOid;
	}
	else
	{
		collid = collform->oid;
	}
	ReleaseSysCache(colltup);
	return collid;
}

/*
 * CollationGetCollid
 *		尝试解析一个非限定排序规则名。
 *		如果在搜索路径中找到该排序规则，则返回 OID，否则返回 InvalidOid。
 *
 * 注意，这只会找到与当前
 * 数据库编码兼容的排序规则。
 */
Oid
CollationGetCollid(const char *collname)
{
	int32		dbencoding = GetDatabaseEncoding();
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);
		Oid			collid;

		if (namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		collid = lookup_collation(collname, namespaceId, dbencoding);
		if (OidIsValid(collid))
			return collid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * CollationIsVisible
 *		确定一个（由 OID 标识的）排序规则在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定排序规则名可以找到它"。
 *
 * 注意，只有与当前数据库编码兼容的排序规则
 * 才会被视为可见。
 */
bool
CollationIsVisible(Oid collid)
{
	HeapTuple	colltup;
	Form_pg_collation collform;
	Oid			collnamespace;
	bool		visible;

	colltup = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(colltup))
		elog(ERROR, "cache lookup failed for collation %u", collid);
	collform = (Form_pg_collation) GETSTRUCT(colltup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	collnamespace = collform->collnamespace;
	if (collnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, collnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名排序规则所隐藏，
		 * 或者它可能与当前数据库编码不兼容。因此我们
		 * 必须进行一次慢速检查，看此排序规则是否会被
		 * CollationGetCollid 找到。
		 */
		char	   *collname = NameStr(collform->collname);

		visible = (CollationGetCollid(collname) == collid);
	}

	ReleaseSysCache(colltup);

	return visible;
}


/*
 * ConversionGetConid
 *		尝试解析一个非限定转换名。
 *		如果在搜索路径中找到该转换，则返回 OID，否则返回 InvalidOid。
 *
 * 这基本上与 RelnameGetRelid 相同。
 */
Oid
ConversionGetConid(const char *conname)
{
	Oid			conid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		conid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
								PointerGetDatum(conname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(conid))
			return conid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * ConversionIsVisible
 *		确定一个（由 OID 标识的）转换在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定转换名可以找到它"。
 */
bool
ConversionIsVisible(Oid conid)
{
	HeapTuple	contup;
	Form_pg_conversion conform;
	Oid			connamespace;
	bool		visible;

	contup = SearchSysCache1(CONVOID, ObjectIdGetDatum(conid));
	if (!HeapTupleIsValid(contup))
		elog(ERROR, "cache lookup failed for conversion %u", conid);
	conform = (Form_pg_conversion) GETSTRUCT(contup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	connamespace = conform->connamespace;
	if (connamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, connamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名转换所隐藏。
		 * 因此我们必须进行一次慢速检查，看此转换是否会被
		 * ConversionGetConid 找到。
		 */
		char	   *conname = NameStr(conform->conname);

		visible = (ConversionGetConid(conname) == conid);
	}

	ReleaseSysCache(contup);

	return visible;
}

/*
 * get_statistics_object_oid - 通过可能限定的名称查找统计对象
 *
 * 如果未找到，则当 missing_ok 为真时返回 InvalidOid，否则抛出错误
 */
Oid
get_statistics_object_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *stats_name;
	Oid			namespaceId;
	Oid			stats_oid = InvalidOid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &stats_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			stats_oid = InvalidOid;
		else
			stats_oid = GetSysCacheOid2(STATEXTNAMENSP, Anum_pg_statistic_ext_oid,
										PointerGetDatum(stats_name),
										ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */
			stats_oid = GetSysCacheOid2(STATEXTNAMENSP, Anum_pg_statistic_ext_oid,
										PointerGetDatum(stats_name),
										ObjectIdGetDatum(namespaceId));
			if (OidIsValid(stats_oid))
				break;
		}
	}

	if (!OidIsValid(stats_oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("statistics object \"%s\" does not exist",
						NameListToString(names))));

	return stats_oid;
}

/*
 * StatisticsObjIsVisible
 *		确定一个（由 OID 标识的）统计对象在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定统计对象名可以找到它"。
 */
bool
StatisticsObjIsVisible(Oid relid)
{
	HeapTuple	stxtup;
	Form_pg_statistic_ext stxform;
	Oid			stxnamespace;
	bool		visible;

	stxtup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(stxtup))
		elog(ERROR, "cache lookup failed for statistics object %u", relid);
	stxform = (Form_pg_statistic_ext) GETSTRUCT(stxtup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	stxnamespace = stxform->stxnamespace;
	if (stxnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, stxnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名统计对象所隐藏。
		 * 因此我们必须进行一次慢速检查，看是否存在冲突的对象。
		 */
		char	   *stxname = NameStr(stxform->stxname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			if (namespaceId == stxnamespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(STATEXTNAMENSP,
									  PointerGetDatum(stxname),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(stxtup);

	return visible;
}

/*
 * get_ts_parser_oid - 通过可能限定的名称查找 TS 解析器
 *
 * 如果未找到，则当 missing_ok 为真时返回 InvalidOid，否则抛出错误
 */
Oid
get_ts_parser_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *parser_name;
	Oid			namespaceId;
	Oid			prsoid = InvalidOid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &parser_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			prsoid = InvalidOid;
		else
			prsoid = GetSysCacheOid2(TSPARSERNAMENSP, Anum_pg_ts_parser_oid,
									 PointerGetDatum(parser_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			prsoid = GetSysCacheOid2(TSPARSERNAMENSP, Anum_pg_ts_parser_oid,
									 PointerGetDatum(parser_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(prsoid))
				break;
		}
	}

	if (!OidIsValid(prsoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search parser \"%s\" does not exist",
						NameListToString(names))));

	return prsoid;
}

/*
 * TSParserIsVisible
 *		确定一个（由 OID 标识的）解析器在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定解析器名可以找到它"。
 */
bool
TSParserIsVisible(Oid prsId)
{
	HeapTuple	tup;
	Form_pg_ts_parser form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(prsId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search parser %u", prsId);
	form = (Form_pg_ts_parser) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	namespace = form->prsnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名解析器所隐藏。因此
		 * 我们必须进行一次慢速检查，看是否存在冲突的解析器。
		 */
		char	   *name = NameStr(form->prsname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSPARSERNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_dict_oid - 通过可能限定的名称查找 TS 词典
 *
 * 如果未找到，则当 missing_ok 为真时返回 InvalidOid，否则抛出错误
 */
Oid
get_ts_dict_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *dict_name;
	Oid			namespaceId;
	Oid			dictoid = InvalidOid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &dict_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			dictoid = InvalidOid;
		else
			dictoid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
									  PointerGetDatum(dict_name),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			dictoid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
									  PointerGetDatum(dict_name),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(dictoid))
				break;
		}
	}

	if (!OidIsValid(dictoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search dictionary \"%s\" does not exist",
						NameListToString(names))));

	return dictoid;
}

/*
 * TSDictionaryIsVisible
 *		确定一个（由 OID 标识的）词典在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定词典名可以找到它"。
 */
bool
TSDictionaryIsVisible(Oid dictId)
{
	HeapTuple	tup;
	Form_pg_ts_dict form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);
	form = (Form_pg_ts_dict) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	namespace = form->dictnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名词典所隐藏。
		 * 因此我们必须进行一次慢速检查，看是否存在冲突的词典。
		 */
		char	   *name = NameStr(form->dictname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSDICTNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_template_oid - 通过可能限定的名称查找 TS 模板
 *
 * 如果未找到，则当 missing_ok 为真时返回 InvalidOid，否则抛出错误
 */
Oid
get_ts_template_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *template_name;
	Oid			namespaceId;
	Oid			tmploid = InvalidOid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &template_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			tmploid = InvalidOid;
		else
			tmploid = GetSysCacheOid2(TSTEMPLATENAMENSP, Anum_pg_ts_template_oid,
									  PointerGetDatum(template_name),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			tmploid = GetSysCacheOid2(TSTEMPLATENAMENSP, Anum_pg_ts_template_oid,
									  PointerGetDatum(template_name),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(tmploid))
				break;
		}
	}

	if (!OidIsValid(tmploid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search template \"%s\" does not exist",
						NameListToString(names))));

	return tmploid;
}

/*
 * TSTemplateIsVisible
 *		确定一个（由 OID 标识的）模板在当前
 *		搜索路径中是否可见。可见意味着 "通过搜索
 *		非限定模板名可以找到它"。
 */
bool
TSTemplateIsVisible(Oid tmplId)
{
	HeapTuple	tup;
	Form_pg_ts_template form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search template %u", tmplId);
	form = (Form_pg_ts_template) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	namespace = form->tmplnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名模板所隐藏。因此
		 * 我们必须进行一次慢速检查，看是否存在冲突的模板。
		 */
		char	   *name = NameStr(form->tmplname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSTEMPLATENAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_config_oid - 通过可能限定的名称查找 TS 配置
 *
 * 如果未找到，则当 missing_ok 为真时返回 InvalidOid，否则抛出错误
 */
Oid
get_ts_config_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *config_name;
	Oid			namespaceId;
	Oid			cfgoid = InvalidOid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, &config_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			cfgoid = InvalidOid;
		else
			cfgoid = GetSysCacheOid2(TSCONFIGNAMENSP, Anum_pg_ts_config_oid,
									 PointerGetDatum(config_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			cfgoid = GetSysCacheOid2(TSCONFIGNAMENSP, Anum_pg_ts_config_oid,
									 PointerGetDatum(config_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(cfgoid))
				break;
		}
	}

	if (!OidIsValid(cfgoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search configuration \"%s\" does not exist",
						NameListToString(names))));

	return cfgoid;
}

/*
 * TSConfigIsVisible
 *		确定一个（由 OID 标识的）文本搜索配置
 *		在当前搜索路径中是否可见。可见意味着 "通过
 *		搜索非限定文本搜索配置名可以找到它"。
 */
bool
TSConfigIsVisible(Oid cfgid)
{
	HeapTuple	tup;
	Form_pg_ts_config form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgid);
	form = (Form_pg_ts_config) GETSTRUCT(tup);

	recomputeNamespacePath();

	/*
	 * 快速检查：如果它根本不在路径中，就不可见。系统
	 * 命名空间中的项肯定在路径中，因此我们甚至无需
	 * 为它们调用 list_member_oid()。
	 */
	namespace = form->cfgnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被
		 * 路径中更靠前的同名配置所隐藏。
		 * 因此我们必须进行一次慢速检查，看是否存在冲突的配置。
		 */
		char	   *name = NameStr(form->cfgname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* 在路径中最先找到它 */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSCONFIGNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* 在路径中最先找到的是别的对象 */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}


/*
 * DeconstructQualifiedName
 *		给定一个以 String 节点列表表示的、可能限定的名称，
 *		提取模式名和对象名。
 *
 * 如果没有显式模式名，*nspname_p 被设置为 NULL。
 */
void
DeconstructQualifiedName(List *names,
						 char **nspname_p,
						 char **objname_p)
{
	char	   *catalogname;
	char	   *schemaname = NULL;
	char	   *objname = NULL;

	switch (list_length(names))
	{
		case 1:
			objname = strVal(linitial(names));
			break;
		case 2:
			schemaname = strVal(linitial(names));
			objname = strVal(lsecond(names));
			break;
		case 3:
			catalogname = strVal(linitial(names));
			schemaname = strVal(lsecond(names));
			objname = strVal(lthird(names));

			/*
			 * 我们检查目录名，然后忽略它。
			 */
			if (strcmp(catalogname, get_database_name(MyDatabaseId)) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cross-database references are not implemented: %s",
								NameListToString(names))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper qualified name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	*nspname_p = schemaname;
	*objname_p = objname;
}

/*
 * LookupNamespaceNoError
 *		查找一个模式名。
 *
 * 返回命名空间 OID，如果未找到则返回 InvalidOid。
 *
 * 注意这不做任何权限检查 --- 调用方负责
 * 确保进行了适当的检查。
 * 在大多数情况下 LookupExplicitNamespace 更可取。
 */
Oid
LookupNamespaceNoError(const char *nspname)
{
	/* 检查 pg_temp 别名 */
	if (strcmp(nspname, "pg_temp") == 0)
	{
		if (OidIsValid(myTempNamespace))
		{
			InvokeNamespaceSearchHook(myTempNamespace, true);
			return myTempNamespace;
		}

		/*
		 * 由于这仅用于查找现有对象，在这里尝试
		 * 初始化 temp 命名空间没有意义；这样做
		 * 可能给某些调用方带来问题。只需报告 "未找到"。
		 */
		return InvalidOid;
	}

	return get_namespace_oid(nspname, true);
}

/*
 * LookupExplicitNamespace
 *		处理一个显式指定的模式名：查找该模式
 *		并验证我们对其拥有 USAGE（查找）权限。
 *
 * 返回命名空间 OID
 */
Oid
LookupExplicitNamespace(const char *nspname, bool missing_ok)
{
	Oid			namespaceId;
	AclResult	aclresult;

	/* 检查 pg_temp 别名 */
	if (strcmp(nspname, "pg_temp") == 0)
	{
		if (OidIsValid(myTempNamespace))
			return myTempNamespace;

		/*
		 * Since this is used only for looking up existing objects, there is
		 * no point in trying to initialize the temp namespace here; and doing
		 * so might create problems for some callers --- just fall through.
		 */
	}

	namespaceId = get_namespace_oid(nspname, missing_ok);
	if (missing_ok && !OidIsValid(namespaceId))
		return InvalidOid;

	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   nspname);
	/* 用于此查找的模式搜索钩子 */
	InvokeNamespaceSearchHook(namespaceId, true);

	return namespaceId;
}

/*
 * LookupCreationNamespace
 *		查找该模式并验证我们对其拥有 CREATE 权限。
 *
 * 这与 LookupExplicitNamespace 类似，只是权限检查不同，
 * 而且我们愿意在需要时创建 pg_temp。
 *
 * 注意：调用此函数可能导致 CommandCounterIncrement 操作，
 * 如果我们必须创建或清理 temp 命名空间的话。
 */
Oid
LookupCreationNamespace(const char *nspname)
{
	Oid			namespaceId;
	AclResult	aclresult;

	/* 检查 pg_temp 别名 */
	if (strcmp(nspname, "pg_temp") == 0)
	{
		/* 初始化 temp 命名空间 */
		AccessTempTableNamespace(false);
		return myTempNamespace;
	}

	namespaceId = get_namespace_oid(nspname, false);

	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   nspname);

	return namespaceId;
}

/*
 * 切换命名空间时的通用检查。
 *
 * 如果旧命名空间或新命名空间是临时模式
 * （或临时 toast 模式），或者旧命名空间或新命名空间是
 * TOAST 模式，我们会报错。
 */
void
CheckSetNamespace(Oid oldNspOid, Oid nspOid)
{
	/* 禁止重命名进入或离开 temp 模式 */
	if (isAnyTempNamespace(nspOid) || isAnyTempNamespace(oldNspOid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of temporary schemas")));

	/* TOAST 模式同理 */
	if (nspOid == PG_TOAST_NAMESPACE || oldNspOid == PG_TOAST_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of TOAST schema")));
}

/*
 * QualifiedNameGetCreationNamespace
 *		给定一个对象的可能限定名称（List-of-Strings
 *		格式），确定该对象应创建在哪个命名空间中。
 *		同时提取并返回对象名（列表的最后一个组成部分）。
 *
 * 注意：这不应用任何权限检查。调用方必须在适当时
 * 检查所选命名空间的 CREATE 权限。
 *
 * 注意：调用此函数可能导致 CommandCounterIncrement 操作，
 * 如果我们必须创建或清理 temp 命名空间的话。
 */
Oid
QualifiedNameGetCreationNamespace(List *names, char **objname_p)
{
	char	   *schemaname;
	Oid			namespaceId;

	/* 解构名称列表 */
	DeconstructQualifiedName(names, &schemaname, objname_p);

	if (schemaname)
	{
		/* 检查 pg_temp 别名 */
		if (strcmp(schemaname, "pg_temp") == 0)
		{
			/* 初始化 temp 命名空间 */
			AccessTempTableNamespace(false);
			return myTempNamespace;
		}
		/* 使用给定的确切模式 */
		namespaceId = get_namespace_oid(schemaname, false);
		/* 我们在这里不检查 USAGE 权限！ */
	}
	else
	{
		/* 使用默认的创建命名空间 */
		recomputeNamespacePath();
		if (activeTempCreationPending)
		{
			/* 需要初始化 temp 命名空间 */
			AccessTempTableNamespace(true);
			return myTempNamespace;
		}
		namespaceId = activeCreationNamespace;
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
	}

	return namespaceId;
}

/*
 * get_namespace_oid - 给定一个命名空间名，查找其 OID
 *
 * 如果 missing_ok 为 false，则在未找到命名空间名时抛出错误。
 * 如果为 true，则直接返回 InvalidOid。
 */
Oid
get_namespace_oid(const char *nspname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(NAMESPACENAME, Anum_pg_namespace_oid,
						  CStringGetDatum(nspname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", nspname)));

	return oid;
}

/*
 * makeRangeVarFromNameList
 *		用于将限定名称列表转换为 RangeVar 形式的实用例程。
 */
RangeVar *
makeRangeVarFromNameList(List *names)
{
	RangeVar   *rel = makeRangeVar(NULL, NULL, -1);

	switch (list_length(names))
	{
		case 1:
			rel->relname = strVal(linitial(names));
			break;
		case 2:
			rel->schemaname = strVal(linitial(names));
			rel->relname = strVal(lsecond(names));
			break;
		case 3:
			rel->catalogname = strVal(linitial(names));
			rel->schemaname = strVal(lsecond(names));
			rel->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper relation name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	return rel;
}

/*
 * NameListToString
 *		用于将限定名称列表转换为字符串的实用例程。
 *
 * 这主要用于构造错误消息，因此为了
 * 可读性，我们不对列表元素加引号。
 *
 * 在大多数场景下，列表元素应当始终是 String 值，
 * 但为了方便 ColumnRef 处理，我们也允许 A_Star。
 */
char *
NameListToString(List *names)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		Node	   *name = (Node *) lfirst(l);

		if (l != list_head(names))
			appendStringInfoChar(&string, '.');

		if (IsA(name, String))
			appendStringInfoString(&string, strVal(name));
		else if (IsA(name, A_Star))
			appendStringInfoChar(&string, '*');
		else
			elog(ERROR, "unexpected node type in name list: %d",
				 (int) nodeTag(name));
	}

	return string.data;
}

/*
 * NameListToQuotedString
 *		用于将限定名称列表转换为字符串的实用例程。
 *
 * 与上面相同，只是在必要时名称会被双引号括起，
 * 因此该字符串可以被重新解析（例如由 textToQualifiedNameList）。
 */
char *
NameListToQuotedString(List *names)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		if (l != list_head(names))
			appendStringInfoChar(&string, '.');
		appendStringInfoString(&string, quote_identifier(strVal(lfirst(l))));
	}

	return string.data;
}

/*
 * isTempNamespace - 给定的命名空间是我的临时表命名空间吗？
 */
bool
isTempNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempNamespace) && myTempNamespace == namespaceId)
		return true;
	return false;
}

/*
 * isTempToastNamespace - 给定的命名空间是我的临时 toast 表
 *		命名空间吗？
 */
bool
isTempToastNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempToastNamespace) && myTempToastNamespace == namespaceId)
		return true;
	return false;
}

/*
 * isTempOrTempToastNamespace - 给定的命名空间是我的临时表
 *		命名空间还是我的临时 toast 表命名空间？
 */
bool
isTempOrTempToastNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempNamespace) &&
		(myTempNamespace == namespaceId || myTempToastNamespace == namespaceId))
		return true;
	return false;
}

/*
 * isAnyTempNamespace - 给定的命名空间是临时表命名空间吗
 * （我自己的，或另一个后端的）？临时 toast 表命名空间
 * 也包括在内。
 */
bool
isAnyTempNamespace(Oid namespaceId)
{
	bool		result;
	char	   *nspname;

	/* 如果命名空间名以 "pg_temp_" 或 "pg_toast_temp_" 开头则为真 */
	nspname = get_namespace_name(namespaceId);
	if (!nspname)
		return false;			/* 没有这样的命名空间？ */
	result = (strncmp(nspname, "pg_temp_", 8) == 0) ||
		(strncmp(nspname, "pg_toast_temp_", 14) == 0);
	pfree(nspname);
	return result;
}

/*
 * isOtherTempNamespace - 给定的命名空间是某个其他后端的
 * 临时表命名空间（包括临时 toast 表命名空间）吗？
 *
 * 注意：对于 C 代码中的大多数用途，此函数已过时。请改用
 * RELATION_IS_OTHER_TEMP() 来检测非本地的临时关系。
 */
bool
isOtherTempNamespace(Oid namespaceId)
{
	/* 如果是我自己的 temp 命名空间，则返回 "false" */
	if (isTempOrTempToastNamespace(namespaceId))
		return false;
	/* 否则，如果它是任何 temp 命名空间，则返回 "true" */
	return isAnyTempNamespace(namespaceId);
}

/*
 * checkTempNamespaceStatus - 给定的命名空间是否由一个后端
 * 拥有并正在使用？
 *
 * 注意：这可以在扫描 pg_class 中的关系时使用，以检测
 * 孤立的临时表，或检测有后端连接到给定
 * 数据库的命名空间。结果可能很快过时，因此调用方
 * 必须小心如何处理此信息。
 */
TempNamespaceStatus
checkTempNamespaceStatus(Oid namespaceId)
{
	PGPROC	   *proc;
	int			backendId;

	Assert(OidIsValid(MyDatabaseId));

	backendId = GetTempNamespaceBackendId(namespaceId);

	/* 没有这样的命名空间，或者其名称表明它不是 temp？ */
	if (backendId == InvalidBackendId)
		return TEMP_NAMESPACE_NOT_TEMP;

	/* 后端是否存活？ */
	proc = BackendIdGetProc(backendId);
	if (proc == NULL)
		return TEMP_NAMESPACE_IDLE;

	/* 该后端是否连接到我们正在查看的同一个数据库？ */
	if (proc->databaseId != MyDatabaseId)
		return TEMP_NAMESPACE_IDLE;

	/* 该后端是否拥有该临时命名空间？ */
	if (proc->tempNamespaceId != namespaceId)
		return TEMP_NAMESPACE_IDLE;

	/* 是的，所以命名空间正忙 */
	return TEMP_NAMESPACE_IN_USE;
}

/*
 * GetTempNamespaceBackendId - 如果给定的命名空间是临时表
 * 命名空间（我自己的，或另一个后端的），则返回拥有它的
 * BackendId。临时 toast 表命名空间也包括在内。
 * 如果它不是 temp 命名空间，则返回 InvalidBackendId。
 */
int
GetTempNamespaceBackendId(Oid namespaceId)
{
	int			result;
	char	   *nspname;

	/* 查看命名空间名是否以 "pg_temp_" 或 "pg_toast_temp_" 开头 */
	nspname = get_namespace_name(namespaceId);
	if (!nspname)
		return InvalidBackendId;	/* 没有这样的命名空间？ */
	if (strncmp(nspname, "pg_temp_", 8) == 0)
		result = atoi(nspname + 8);
	else if (strncmp(nspname, "pg_toast_temp_", 14) == 0)
		result = atoi(nspname + 14);
	else
		result = InvalidBackendId;
	pfree(nspname);
	return result;
}

/*
 * GetTempToastNamespace - 获取我的临时 toast 表命名空间的 OID，
 * 该命名空间必须已经分配。（这仅在为临时表创建 toast
 * 表时使用，因此我们必须已经执行过 InitTempTableNamespace）
 */
Oid
GetTempToastNamespace(void)
{
	Assert(OidIsValid(myTempToastNamespace));
	return myTempToastNamespace;
}


/*
 * GetTempNamespaceState - 获取会话临时命名空间的状态
 *
 * 这用于将状态传递给并行工作进程，并非
 * 为通用访问而设计。
 */
void
GetTempNamespaceState(Oid *tempNamespaceId, Oid *tempToastNamespaceId)
{
	/* 返回命名空间 OIDs，如果会话尚未创建 temp 命名空间则为 0 */
	*tempNamespaceId = myTempNamespace;
	*tempToastNamespaceId = myTempToastNamespace;
}

/*
 * SetTempNamespaceState - 设置会话临时命名空间的状态
 *
 * 这用于将状态传递给并行工作进程，并非为
 * 通用访问而设计。通过将这些命名空间 OIDs 传递给工作进程，
 * 我们确保它们将与其领导者具有相同的
 * 搜索路径概念。
 */
void
SetTempNamespaceState(Oid tempNamespaceId, Oid tempToastNamespaceId)
{
	/* 工作进程不应创建自己的命名空间 ... */
	Assert(myTempNamespace == InvalidOid);
	Assert(myTempToastNamespace == InvalidOid);
	Assert(myTempNamespaceSubID == InvalidSubTransactionId);

	/* 分配与领导者相同的命名空间 OIDs */
	myTempNamespace = tempNamespaceId;
	myTempToastNamespace = tempToastNamespaceId;

	/*
	 * 将 myTempNamespaceSubID 保留为 InvalidSubTransactionId 是可以的。
	 * 即使该命名空间就领导者而言是新的，对工作进程来说
	 * 它也不是新的，我们当然不希望工作进程尝试
	 * 销毁它。
	 */

	baseSearchPathValid = false;	/* 可能需要重建列表 */
}


/*
 * GetOverrideSearchPath - 以 PushOverrideSearchPath 使用的形式
 * 获取当前搜索路径定义。
 *
 * 结果结构分配在指定的内存上下文中
 * （它可能与 CurrentMemoryContext 相等，也可能不相等）；但重新验证
 * 计算所创建的任何垃圾都会位于 CurrentMemoryContext 中。
 */
OverrideSearchPath *
GetOverrideSearchPath(MemoryContext context)
{
	OverrideSearchPath *result;
	List	   *schemas;
	MemoryContext oldcxt;

	recomputeNamespacePath();

	oldcxt = MemoryContextSwitchTo(context);

	result = (OverrideSearchPath *) palloc0(sizeof(OverrideSearchPath));
	schemas = list_copy(activeSearchPath);
	while (schemas && linitial_oid(schemas) != activeCreationNamespace)
	{
		if (linitial_oid(schemas) == myTempNamespace)
			result->addTemp = true;
		else
		{
			Assert(linitial_oid(schemas) == PG_CATALOG_NAMESPACE);
			result->addCatalog = true;
		}
		schemas = list_delete_first(schemas);
	}
	result->schemas = schemas;
	result->generation = activePathGeneration;

	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * CopyOverrideSearchPath - 复制指定的 OverrideSearchPath。
 *
 * 结果结构分配在 CurrentMemoryContext 中。
 */
OverrideSearchPath *
CopyOverrideSearchPath(OverrideSearchPath *path)
{
	OverrideSearchPath *result;

	result = (OverrideSearchPath *) palloc(sizeof(OverrideSearchPath));
	result->schemas = list_copy(path->schemas);
	result->addCatalog = path->addCatalog;
	result->addTemp = path->addTemp;
	result->generation = path->generation;

	return result;
}

/*
 * OverrideSearchPathMatchesCurrent - 路径是否匹配当前设置？
 *
 * 这在某些常见代码路径中会被反复测试，而在典型
 * 场景中活动搜索路径很少变化，因此它总是会成功。
 * 我们通过维护一个在活动搜索路径变化时
 * 递增的代计数器，使这种情况变快。
 */
bool
OverrideSearchPathMatchesCurrent(OverrideSearchPath *path)
{
	ListCell   *lc,
			   *lcp;

	recomputeNamespacePath();

	/* 如果已知与活动路径相等则快速退出。 */
	if (path->generation == activePathGeneration)
		return true;

	/* 我们沿 activeSearchPath 向下扫描，看它是否与输入匹配。 */
	lc = list_head(activeSearchPath);

	/* 如果 path->addTemp，则第一项应当是我的 temp 命名空间。 */
	if (path->addTemp)
	{
		if (lc && lfirst_oid(lc) == myTempNamespace)
			lc = lnext(activeSearchPath, lc);
		else
			return false;
	}
	/* 如果 path->addCatalog，则下一项应当是 pg_catalog。 */
	if (path->addCatalog)
	{
		if (lc && lfirst_oid(lc) == PG_CATALOG_NAMESPACE)
			lc = lnext(activeSearchPath, lc);
		else
			return false;
	}
	/* 我们现在应当正在查看 activeCreationNamespace。 */
	if (activeCreationNamespace != (lc ? lfirst_oid(lc) : InvalidOid))
		return false;
	/* activeSearchPath 的剩余部分应当与 path->schemas 匹配。 */
	foreach(lcp, path->schemas)
	{
		if (lc && lfirst_oid(lc) == lfirst_oid(lcp))
			lc = lnext(activeSearchPath, lc);
		else
			return false;
	}
	if (lc)
		return false;

	/*
	 * 更新 path->generation，以便未来的测试能快速返回，
	 * 只要活动搜索路径不变。
	 */
	path->generation = activePathGeneration;

	return true;
}

/*
 * PushOverrideSearchPath - 临时覆盖搜索路径
 *
 * 不要使用此函数；几乎任何使用都会引入安全
 * 漏洞。它存在是为了运行在非安全敏感环境中的
 * 遗留代码的利益。
 *
 * 我们允许嵌套覆盖，因此使用 push/pop 术语。在覆盖处于活动
 * 状态期间，GUC 变量 search_path 会被忽略。
 *
 * 有可能 newpath->useTemp 被设置，但已不再有任何
 * 活动的 temp 命名空间，如果该路径是在一个创建了 temp 命名空间
 * 而后又回滚的事务期间保存的。在那种情况下，我们直接
 * 忽略 useTemp。一种看似合理的替代方案是创建一个新的 temp
 * 命名空间，但对于现有调用方来说没有必要，因为空的
 * temp 命名空间无论如何都不会影响它们的结果。
 *
 * 同样值得注意的是，newpath 中列出的其他模式可能也
 * 不再存在。我们不为此担心，因为不匹配任何
 * 现有命名空间的 OIDs 在搜索期间根本不会产生任何命中。
 */
void
PushOverrideSearchPath(OverrideSearchPath *newpath)
{
	OverrideStackEntry *entry;
	List	   *oidlist;
	Oid			firstNS;
	MemoryContext oldcxt;

	/*
	 * 复制该列表以妥善保存，并按需插入隐式搜索的
	 * 命名空间。此代码应当与 recomputeNamespacePath 保持一致。
	 */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	oidlist = list_copy(newpath->schemas);

	/*
	 * 记住显式列表的第一个成员。
	 */
	if (oidlist == NIL)
		firstNS = InvalidOid;
	else
		firstNS = linitial_oid(oidlist);

	/*
	 * 将任何隐式搜索的命名空间加入列表。注意这些位于
	 * 前面，而不是后面；还要注意我们不为这些
	 * 检查 USAGE 权限。
	 */
	if (newpath->addCatalog)
		oidlist = lcons_oid(PG_CATALOG_NAMESPACE, oidlist);

	if (newpath->addTemp && OidIsValid(myTempNamespace))
		oidlist = lcons_oid(myTempNamespace, oidlist);

	/*
	 * 构建新的栈条目，然后将其插入到列表头部。
	 */
	entry = (OverrideStackEntry *) palloc(sizeof(OverrideStackEntry));
	entry->searchPath = oidlist;
	entry->creationNamespace = firstNS;
	entry->nestLevel = GetCurrentTransactionNestLevel();

	overrideStack = lcons(entry, overrideStack);

	/* 并使其成为活动的。 */
	activeSearchPath = entry->searchPath;
	activeCreationNamespace = entry->creationNamespace;
	activeTempCreationPending = false;	/* XXX 这样可以吗？ */

	/*
	 * 在压入/弹出覆盖路径时，我们总是递增 activePathGeneration。
	 * 在当前的用法中，这些操作总会改变有效的路径
	 * 状态，因此检查它是否没有变化
	 * 没有价值。
	 */
	activePathGeneration++;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * PopOverrideSearchPath - 撤销先前的 PushOverrideSearchPath
 *
 * 在（子）事务期间进行的任何压入都会在中止时自动弹出。
 * 但如果压入在正常控制流中没有被弹出，则是调用方错误。
 */
void
PopOverrideSearchPath(void)
{
	OverrideStackEntry *entry;

	/* 完整性检查。 */
	if (overrideStack == NIL)
		elog(ERROR, "bogus PopOverrideSearchPath call");
	entry = (OverrideStackEntry *) linitial(overrideStack);
	if (entry->nestLevel != GetCurrentTransactionNestLevel())
		elog(ERROR, "bogus PopOverrideSearchPath call");

	/* 弹出栈并释放存储。 */
	overrideStack = list_delete_first(overrideStack);
	list_free(entry->searchPath);
	pfree(entry);

	/* 激活向下的下一层。 */
	if (overrideStack)
	{
		entry = (OverrideStackEntry *) linitial(overrideStack);
		activeSearchPath = entry->searchPath;
		activeCreationNamespace = entry->creationNamespace;
		activeTempCreationPending = false;	/* XXX 这样可以吗？ */
	}
	else
	{
		/* 如果 baseSearchPathValid 为假，则这无用但无害 */
		activeSearchPath = baseSearchPath;
		activeCreationNamespace = baseCreationNamespace;
		activeTempCreationPending = baseTempCreationPending;
	}

	/* 如上，代计数总是递增。 */
	activePathGeneration++;
}


/*
 * get_collation_oid - 通过可能限定的名称查找排序规则
 *
 * 注意，这只会找到与当前
 * 数据库编码兼容的排序规则。
 */
Oid
get_collation_oid(List *collname, bool missing_ok)
{
	char	   *schemaname;
	char	   *collation_name;
	int32		dbencoding = GetDatabaseEncoding();
	Oid			namespaceId;
	Oid			colloid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(collname, &schemaname, &collation_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			return InvalidOid;

		colloid = lookup_collation(collation_name, namespaceId, dbencoding);
		if (OidIsValid(colloid))
			return colloid;
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			colloid = lookup_collation(collation_name, namespaceId, dbencoding);
			if (OidIsValid(colloid))
				return colloid;
		}
	}

	/* 在路径中未找到 */
	if (!missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" does not exist",
						NameListToString(collname), GetDatabaseEncodingName())));
	return InvalidOid;
}

/*
 * get_conversion_oid - 通过可能限定的名称查找转换
 */
Oid
get_conversion_oid(List *conname, bool missing_ok)
{
	char	   *schemaname;
	char	   *conversion_name;
	Oid			namespaceId;
	Oid			conoid = InvalidOid;
	ListCell   *l;

	/* 解构名称列表 */
	DeconstructQualifiedName(conname, &schemaname, &conversion_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			conoid = InvalidOid;
		else
			conoid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
									 PointerGetDatum(conversion_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* 在搜索路径中查找它 */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不在 temp 命名空间中查找 */

			conoid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
									 PointerGetDatum(conversion_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(conoid))
				return conoid;
		}
	}

	/* 在路径中未找到 */
	if (!OidIsValid(conoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion \"%s\" does not exist",
						NameListToString(conname))));
	return conoid;
}

/*
 * FindDefaultConversionProc - 查找默认编码转换函数
 */
Oid
FindDefaultConversionProc(int32 for_encoding, int32 to_encoding)
{
	Oid			proc;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不在 temp 命名空间中查找 */

		proc = FindDefaultConversion(namespaceId, for_encoding, to_encoding);
		if (OidIsValid(proc))
			return proc;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * recomputeNamespacePath - 根据需要重新计算由路径派生的变量。
 */
static void
recomputeNamespacePath(void)
{
	Oid			roleid = GetUserId();
	char	   *rawname;
	List	   *namelist;
	List	   *oidlist;
	List	   *newpath;
	ListCell   *l;
	bool		temp_missing;
	Oid			firstNS;
	bool		pathChanged;
	MemoryContext oldcxt;

	/* 如果覆盖搜索规范处于活动状态则什么都不做。 */
	if (overrideStack)
		return;

	/* 如果路径已经有效则什么都不做。 */
	if (baseSearchPathValid && namespaceUser == roleid)
		return;

	/* 需要 namespace_search_path 字符串的可修改副本 */
	rawname = pstrdup(namespace_search_path);

	/* 将字符串解析为标识符列表 */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* 名称列表中有语法错误 */
		/* 如果 GUC 检查了 check_search_path，这不应发生 */
		elog(ERROR, "invalid list syntax");
	}

	/*
	 * 将名称列表转换为 OIDs 列表。如果任何名称无法
	 * 识别，或者我们没有读访问权限，就直接将它们排除在
	 * 列表之外。（我们不能抛出错误，因为 search_path 设置已经
	 * 被接受。）也不要创建重复条目。
	 */
	oidlist = NIL;
	temp_missing = false;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			namespaceId;

		if (strcmp(curname, "$user") == 0)
		{
			/* $user --- 替换为与用户名匹配的命名空间（如果有） */
			HeapTuple	tuple;

			tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
			if (HeapTupleIsValid(tuple))
			{
				char	   *rname;

				rname = NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname);
				namespaceId = get_namespace_oid(rname, true);
				ReleaseSysCache(tuple);
				if (OidIsValid(namespaceId) &&
					!list_member_oid(oidlist, namespaceId) &&
					object_aclcheck(NamespaceRelationId, namespaceId, roleid,
									ACL_USAGE) == ACLCHECK_OK &&
					InvokeNamespaceSearchHook(namespaceId, false))
					oidlist = lappend_oid(oidlist, namespaceId);
			}
		}
		else if (strcmp(curname, "pg_temp") == 0)
		{
			/* pg_temp --- 替换为 temp 命名空间（如果有） */
			if (OidIsValid(myTempNamespace))
			{
				if (!list_member_oid(oidlist, myTempNamespace) &&
					InvokeNamespaceSearchHook(myTempNamespace, false))
					oidlist = lappend_oid(oidlist, myTempNamespace);
			}
			else
			{
				/* 如果它应当是创建命名空间，则设置标志 */
				if (oidlist == NIL)
					temp_missing = true;
			}
		}
		else
		{
			/* 普通命名空间引用 */
			namespaceId = get_namespace_oid(curname, true);
			if (OidIsValid(namespaceId) &&
				!list_member_oid(oidlist, namespaceId) &&
				object_aclcheck(NamespaceRelationId, namespaceId, roleid,
								ACL_USAGE) == ACLCHECK_OK &&
				InvokeNamespaceSearchHook(namespaceId, false))
				oidlist = lappend_oid(oidlist, namespaceId);
		}
	}

	/*
	 * 记住显式列表的第一个成员。（注意：如果 temp_missing，
	 * 这名义上是不对的，但我们仍需要它来区分
	 * pg_catalog 的显式提及与隐式提及。）
	 */
	if (oidlist == NIL)
		firstNS = InvalidOid;
	else
		firstNS = linitial_oid(oidlist);

	/*
	 * 将任何隐式搜索的命名空间加入列表。注意这些位于
	 * 前面，而不是后面；还要注意我们不为这些
	 * 检查 USAGE 权限。
	 */
	if (!list_member_oid(oidlist, PG_CATALOG_NAMESPACE))
		oidlist = lcons_oid(PG_CATALOG_NAMESPACE, oidlist);

	if (OidIsValid(myTempNamespace) &&
		!list_member_oid(oidlist, myTempNamespace))
		oidlist = lcons_oid(myTempNamespace, oidlist);

	/*
	 * 我们希望检测基础搜索路径变量的有效值
	 * 没有变化的情况。只要我们这样做，就可以避免
	 * 不必要地复制 OID 列表。
	 */
	if (baseCreationNamespace == firstNS &&
		baseTempCreationPending == temp_missing &&
		equal(oidlist, baseSearchPath))
	{
		pathChanged = false;
	}
	else
	{
		pathChanged = true;

		/* 必须将 OID 列表保存到永久存储中。 */
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		newpath = list_copy(oidlist);
		MemoryContextSwitchTo(oldcxt);

		/* 现在可以安全地赋值给状态变量。 */
		list_free(baseSearchPath);
		baseSearchPath = newpath;
		baseCreationNamespace = firstNS;
		baseTempCreationPending = temp_missing;
	}

	/* 将路径标记为有效。 */
	baseSearchPathValid = true;
	namespaceUser = roleid;

	/* 并使其成为活动的。 */
	activeSearchPath = baseSearchPath;
	activeCreationNamespace = baseCreationNamespace;
	activeTempCreationPending = baseTempCreationPending;

	/*
	 * 仅在确实发生变化时才递增代计数。（请注意，
	 * 我们比较的是基础路径变量以前的状态；因此
	 * 这并不处理我们刚刚弹出一个覆盖路径并恢复了
	 * 基础路径先前状态的情形。相反，
	 * 我们依赖弹出覆盖的逻辑已经递增了代计数。）
	 */
	if (pathChanged)
		activePathGeneration++;

	/* 清理。 */
	pfree(rawname);
	list_free(namelist);
	list_free(oidlist);
}

/*
 * AccessTempTableNamespace
 *		提供对临时命名空间的访问，如果它尚不存在
 *		则可能创建它。此例程会登记该命名空间是否在
 *		本事务中被使用。可以将 'force' 设置为 true，以允许
 *		调用方强制创建用于此后端的临时命名空间，
 *		这会在其创建处于待处理状态时发生。
 */
static void
AccessTempTableNamespace(bool force)
{
	/*
	 * 记下此临时命名空间已在本
	 * 事务中被访问。
	 */
	MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/*
	 * 如果尝试访问临时模式的调用方期望命名空间的
	 * 创建处于待处理状态并应被强制执行，那么就
	 * 走创建流程。
	 */
	if (!force && OidIsValid(myTempNamespace))
		return;

	/*
	 * 该临时表空间尚不存在且需要它，因此
	 * 初始化它。
	 */
	InitTempTableNamespace();
}

/*
 * InitTempTableNamespace
 *		在特定后端首次使用时初始化临时表命名空间
 */
static void
InitTempTableNamespace(void)
{
	char		namespaceName[NAMEDATALEN];
	Oid			namespaceId;
	Oid			toastspaceId;

	Assert(!OidIsValid(myTempNamespace));

	/*
	 * 首先，进行权限检查，看我们是否有权创建临时
	 * 表。我们在这里使用非标准错误消息，因为 "databasename:
	 * permission denied" 可能有点晦涩。
	 *
	 * 注意 ACL_CREATE_TEMP 权限会在 pg_namespace_aclmask 中重新检查；
	 * 这是必要的，因为当前用户 ID 可能在会话期间变化。
	 * 但在拥有适当权限的人发出临时表创建请求之前，
	 * 根本不需要创建该命名空间。
	 */
	if (object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
						ACL_CREATE_TEMP) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create temporary tables in database \"%s\"",
						get_database_name(MyDatabaseId))));

	/*
	 * 不允许 Hot Standby 会话创建临时表。除了
	 * 修改系统目录的问题外，还有一个命名
	 * 冲突：pg_temp_N 属于主库上 BackendId 为 N 的会话，
	 * 而不属于具有相同 BackendId 的热备会话。由于
	 * XactReadOnly 检查，我们无论如何都不应到达这里，但
	 * 让我们再确认一下。注意这也为允许 XactReadOnly 事务
	 * 修改临时表的各种操作提供了兜底保障；
	 * 如果不是因为这个，它们就需要 RecoveryInProgress 检查。
	 */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot create temporary tables during recovery")));

	/* 并行工作进程也不能创建临时表。 */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot create temporary tables during a parallel operation")));

	snprintf(namespaceName, sizeof(namespaceName), "pg_temp_%d", MyBackendId);

	namespaceId = get_namespace_oid(namespaceName, true);
	if (!OidIsValid(namespaceId))
	{
		/*
		 * 在此数据库中首次使用此 temp 命名空间；创建它。
		 * temp 命名空间始终由超级用户拥有。我们将其
		 * 权限保留为默认 --- 即除超级用户外无人可访问 ---
		 * 以确保非特权用户无法窥视其他后端的
		 * 临时表。这之所以可行，是因为为我自己后端访问 temp
		 * 命名空间的地方会跳过对其的权限检查。
		 */
		namespaceId = NamespaceCreate(namespaceName, BOOTSTRAP_SUPERUSERID,
									  true);
		/* 推进命令计数器以使命名空间可见 */
		CommandCounterIncrement();
	}
	else
	{
		/*
		 * 如果该命名空间已经存在，则清理它（以防先前
		 * 所有者崩溃而没有这样做）。
		 */
		RemoveTempRelations(namespaceId);
	}

	/*
	 * 如果相应的 toast 表命名空间尚不存在，则创建
	 * 它。（我们假设如果它已存在则无需清理，因为
	 * 删除父表应当会使其 toast 表消失。）
	 */
	snprintf(namespaceName, sizeof(namespaceName), "pg_toast_temp_%d",
			 MyBackendId);

	toastspaceId = get_namespace_oid(namespaceName, true);
	if (!OidIsValid(toastspaceId))
	{
		toastspaceId = NamespaceCreate(namespaceName, BOOTSTRAP_SUPERUSERID,
									   true);
		/* 推进命令计数器以使命名空间可见 */
		CommandCounterIncrement();
	}

	/*
	 * 好，我们已经准备好 temp 命名空间 ... 但它尚未提交，
	 * 因此我们所有的工作都可能因事务回滚而被撤销。设置标志
	 * 让 AtEOXact_Namespace 知道该怎么做。
	 */
	myTempNamespace = namespaceId;
	myTempToastNamespace = toastspaceId;

	/*
	 * 将 MyProc 标记为拥有此命名空间，其他进程可用它来
	 * 判断某个临时命名空间是否正在使用。我们假设对
	 * namespaceId 的赋值是原子操作。即使它不是，
	 * 导致创建此临时命名空间的临时关系
	 * 仍然被锁定，直到当前事务提交，
	 * 且其 pg_namespace 行尚不可见。然而这无关紧要：
	 * 此标志使该命名空间被视为正在使用，因此在其上创建的
	 * 任何对象都不会被并发移除。
	 */
	MyProc->tempNamespaceId = namespaceId;

	/* 它不应已经完成。 */
	Assert(myTempNamespaceSubID == InvalidSubTransactionId);
	myTempNamespaceSubID = GetCurrentSubTransactionId();

	baseSearchPathValid = false;	/* 可能需要重建列表 */
}

/*
 * 命名空间的结束事务清理。
 */
void
AtEOXact_Namespace(bool isCommit, bool parallel)
{
	/*
	 * 如果我们在其中选择了 temp 命名空间的事务中中止，
	 * 我们将不得不重新进行任何创建或清理工作。因此，
	 * 直接完全忘记该命名空间，直到下次。另一方面，如果
	 * 我们提交，则注册一个退出回调，在后端关闭时清理
	 * 临时表。（我们只希望每个会话注册一次该回调，
	 * 所以这是一个做这件事的好地方。）
	 */
	if (myTempNamespaceSubID != InvalidSubTransactionId && !parallel)
	{
		if (isCommit)
			before_shmem_exit(RemoveTempRelationsCallback, 0);
		else
		{
			myTempNamespace = InvalidOid;
			myTempToastNamespace = InvalidOid;
			baseSearchPathValid = false;	/* 可能需要重建列表 */

			/*
			 * 重置 MyProc 中的临时命名空间标志。我们假设
			 * 此操作是原子的。
			 *
			 * 由于此事务正在中止，pg_namespace 行
			 * 对其他人本来就不可见，但这也无关紧要：
			 * 如果此命名空间中包含的对象被并发
			 * 移除，也没有问题。
			 */
			MyProc->tempNamespaceId = InvalidOid;
		}
		myTempNamespaceSubID = InvalidSubTransactionId;
	}

	/*
	 * 如果有人未能执行 PopOverrideSearchPath 则进行清理
	 */
	if (overrideStack)
	{
		if (isCommit)
			elog(WARNING, "leaked override search path");
		while (overrideStack)
		{
			OverrideStackEntry *entry;

			entry = (OverrideStackEntry *) linitial(overrideStack);
			overrideStack = list_delete_first(overrideStack);
			list_free(entry->searchPath);
			pfree(entry);
		}
		/* 如果 baseSearchPathValid 为假，则这无用但无害 */
		activeSearchPath = baseSearchPath;
		activeCreationNamespace = baseCreationNamespace;
		activeTempCreationPending = baseTempCreationPending;
		/* 总是递增代计数 --- 参见 recomputeNamespacePath 中的说明 */
		activePathGeneration++;
	}
}

/*
 * AtEOSubXact_Namespace
 *
 * 在子事务提交时，将临时命名空间创建标志
 * 传播到父子事务。
 *
 * 在子事务中止时，如果我们设置过该标志则忘记它。
 */
void
AtEOSubXact_Namespace(bool isCommit, SubTransactionId mySubid,
					  SubTransactionId parentSubid)
{
	OverrideStackEntry *entry;

	if (myTempNamespaceSubID == mySubid)
	{
		if (isCommit)
			myTempNamespaceSubID = parentSubid;
		else
		{
			myTempNamespaceSubID = InvalidSubTransactionId;
			/* TEMP 命名空间创建失败，因此重置状态 */
			myTempNamespace = InvalidOid;
			myTempToastNamespace = InvalidOid;
			baseSearchPathValid = false;	/* 可能需要重建列表 */

			/*
			 * 重置 MyProc 中的临时命名空间标志。我们假设
			 * 此操作是原子的。
			 *
			 * 由于此子事务正在中止，pg_namespace 行
			 * 对其他人本来就不可见，但这也无关紧要：
			 * 如果此命名空间中包含的对象被并发
			 * 移除，也没有问题。
			 */
			MyProc->tempNamespaceId = InvalidOid;
		}
	}

	/*
	 * 如果有人未能执行 PopOverrideSearchPath 则进行清理
	 */
	while (overrideStack)
	{
		entry = (OverrideStackEntry *) linitial(overrideStack);
		if (entry->nestLevel < GetCurrentTransactionNestLevel())
			break;
		if (isCommit)
			elog(WARNING, "leaked override search path");
		overrideStack = list_delete_first(overrideStack);
		list_free(entry->searchPath);
		pfree(entry);
		/* 总是递增代计数 --- 参见 recomputeNamespacePath 中的说明 */
		activePathGeneration++;
	}

	/* 激活向下的下一层。 */
	if (overrideStack)
	{
		entry = (OverrideStackEntry *) linitial(overrideStack);
		activeSearchPath = entry->searchPath;
		activeCreationNamespace = entry->creationNamespace;
		activeTempCreationPending = false;	/* XXX 这样可以吗？ */

		/*
		 * 在这里递增代计数可能没有必要，但这应当
		 * 不是性能攸关的情形，所以宁可过度谨慎。
		 */
		activePathGeneration++;
	}
	else
	{
		/* 如果 baseSearchPathValid 为假，则这无用但无害 */
		activeSearchPath = baseSearchPath;
		activeCreationNamespace = baseCreationNamespace;
		activeTempCreationPending = baseTempCreationPending;

		/*
		 * 如果我们弹出了一个覆盖栈条目，那么我们在上面已经
		 * 递增了代计数。如果我们没有，那么上面的赋值
		 * 什么也没做，我们也无需递增代计数。
		 */
	}
}

/*
 * 移除指定 temp 命名空间中的所有关系。
 *
 * 这会在后端关闭时调用（如果我们创建过任何临时关系）。
 * 它也会在我们开始使用一个先前存在的 temp 命名空间时调用，
 * 以便清理任何可能由崩溃的后端
 * 创建的关系。
 */
static void
RemoveTempRelations(Oid tempNamespaceId)
{
	ObjectAddress object;

	/*
	 * 我们希望清除目标命名空间中的所有内容，但不包括
	 * 命名空间本身（仅仅为了稍后重新创建而删除它是
	 * 浪费周期）。因此，指定 SKIP_ORIGINAL。这也是一次 INTERNAL
	 * 删除，并且我们希望不删除任何可能恰好拥有临时对象的
	 * 扩展。
	 */
	object.classId = NamespaceRelationId;
	object.objectId = tempNamespaceId;
	object.objectSubId = 0;

	performDeletion(&object, DROP_CASCADE,
					PERFORM_DELETION_INTERNAL |
					PERFORM_DELETION_QUIETLY |
					PERFORM_DELETION_SKIP_ORIGINAL |
					PERFORM_DELETION_SKIP_EXTENSIONS);
}

/*
 * 在后端退出时移除临时关系的回调。
 */
static void
RemoveTempRelationsCallback(int code, Datum arg)
{
	if (OidIsValid(myTempNamespace))	/* 应当始终为真 */
	{
		/* 需要确保我们有一个可用的事务。 */
		AbortOutOfAnyTransaction();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		RemoveTempRelations(myTempNamespace);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}
}

/*
 * 从临时命名空间中移除所有临时表。
 */
void
ResetTempTableNamespace(void)
{
	if (OidIsValid(myTempNamespace))
		RemoveTempRelations(myTempNamespace);
}


/*
 * 用于处理 GUC 变量 'search_path' 的例程。
 */

/* check_hook：验证新的 search_path 值 */
bool
check_search_path(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	List	   *namelist;

	/* 需要字符串的可修改副本 */
	rawname = pstrdup(*newval);

	/* 将字符串解析为标识符列表 */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* 名称列表中有语法错误 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawname);
		list_free(namelist);
		return false;
	}

	/*
	 * 我们过去曾试图检查所命名的模式是否存在，但
	 * 有许多合法的使用场景需要 search_path 设置包含
	 * 不存在的模式；而且通常我们在这里并不在事务内部，
	 * 因此无论如何也无法查询系统目录。所以现在，唯一的
	 * 要求是标识符列表在语法上有效。
	 */

	pfree(rawname);
	list_free(namelist);

	return true;
}

/* assign_hook：按需执行额外操作 */
void
assign_search_path(const char *newval, void *extra)
{
	/*
	 * 我们将该路径标记为需要重新计算，但在需要之前
	 * 不做任何事。这避免了在 GUC 初始化期间，或
	 * 在事务之外尝试访问数据库。
	 */
	baseSearchPathValid = false;
}

/*
 * InitializeSearchPath：在 InitPostgres 期间初始化模块。
 *
 * 这在我们已经启动到足以进行目录查找之后调用。
 */
void
InitializeSearchPath(void)
{
	if (IsBootstrapProcessingMode())
	{
		/*
		 * 在引导模式下，搜索路径必须是 'pg_catalog'，以便
		 * 表被创建在正确的命名空间中；忽略 GUC 设置。
		 */
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		baseSearchPath = list_make1_oid(PG_CATALOG_NAMESPACE);
		MemoryContextSwitchTo(oldcxt);
		baseCreationNamespace = PG_CATALOG_NAMESPACE;
		baseTempCreationPending = false;
		baseSearchPathValid = true;
		namespaceUser = GetUserId();
		activeSearchPath = baseSearchPath;
		activeCreationNamespace = baseCreationNamespace;
		activeTempCreationPending = baseTempCreationPending;
		activePathGeneration++; /* 形式上 */
	}
	else
	{
		/*
		 * 在正常模式下，安排一个回调，用于任何对
		 * pg_namespace 或 pg_authid 行的系统缓存失效。
		 * （更改角色名可能影响特殊字符串 $user 的含义。）
		 */
		CacheRegisterSyscacheCallback(NAMESPACEOID,
									  NamespaceCallback,
									  (Datum) 0);
		CacheRegisterSyscacheCallback(AUTHOID,
									  NamespaceCallback,
									  (Datum) 0);
		/* 强制在下次使用时重新计算搜索路径 */
		baseSearchPathValid = false;
	}
}

/*
 * NamespaceCallback
 *		系统缓存失效回调函数
 */
static void
NamespaceCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	/* 强制在下次使用时重新计算搜索路径 */
	baseSearchPathValid = false;
}

/*
 * 获取活动的搜索路径。返回值是一个 palloc 的 OIDs
 * 列表；调用方负责适当地释放此
 * 存储。
 *
 * 返回的列表仅在 includeImplicit 为 true 时
 * 包含隐式前置的命名空间。
 *
 * 注意：调用此函数可能导致 CommandCounterIncrement 操作，
 * 如果我们必须创建或清理 temp 命名空间的话。
 */
List *
fetch_search_path(bool includeImplicit)
{
	List	   *result;

	recomputeNamespacePath();

	/*
	 * 如果 temp 命名空间应当排在最前面，则强制它存在。这是为了
	 * 让调用方可以信任结果能反映实际的默认
	 * 创建命名空间。在这里这样做有点不妥，因为
	 * current_schema() 据称是一个没有副作用的稳定函数，
	 * 但其他替代方案似乎更糟。
	 */
	if (activeTempCreationPending)
	{
		AccessTempTableNamespace(true);
		recomputeNamespacePath();
	}

	result = list_copy(activeSearchPath);
	if (!includeImplicit)
	{
		while (result && linitial_oid(result) != activeCreationNamespace)
			result = list_delete_first(result);
	}

	return result;
}

/*
 * 获取活动的搜索路径到调用方分配的 OIDs 数组中。
 * 返回路径条目的数量。（如果这超过 sarray_len，
 * 则数据无法全部容纳，并未全部存储。）
 *
 * 返回的列表总是包含隐式前置的命名空间，
 * 但绝不包含 temp 命名空间。（这适合现有
 * 用户，它们无论如何都会想忽略 temp 命名空间。）这一
 * 定义使我们无需操心初始化 temp 命名空间。
 */
int
fetch_search_path_array(Oid *sarray, int sarray_len)
{
	int			count = 0;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不包含 temp 命名空间 */

		if (count < sarray_len)
			sarray[count] = namespaceId;
		count++;
	}

	return count;
}


/*
 * 将 FooIsVisible 函数导出为 SQL 可调用的函数。
 *
 * 注意：从 Postgres 8.4 起，如果对不存在的对象 OID
 * 调用，这些函数会静默返回 NULL，而不是报错。这是为了
 * 避免当使用 MVCC 快照扫描目录的查询
 * 使用这些函数之一时出现竞态条件错误。底层的 IsVisible 函数
 * 总是使用最新的快照，因此当对象对事务快照仍然可见
 * 时，它可能已经看到该对象不存在。（在当前的实现中
 * 不存在竞态条件，因为我们在 SearchSysCacheExists 测试
 * 与随后的查找之间不接受 sinval 消息。）
 */

Datum
pg_table_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(RelationIsVisible(oid));
}

Datum
pg_type_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(TypeIsVisible(oid));
}

Datum
pg_function_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(FunctionIsVisible(oid));
}

Datum
pg_operator_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(OPEROID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(OperatorIsVisible(oid));
}

Datum
pg_opclass_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(CLAOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(OpclassIsVisible(oid));
}

Datum
pg_opfamily_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(OPFAMILYOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(OpfamilyIsVisible(oid));
}

Datum
pg_collation_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(COLLOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(CollationIsVisible(oid));
}

Datum
pg_conversion_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(CONVOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(ConversionIsVisible(oid));
}

Datum
pg_statistics_obj_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(STATEXTOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(StatisticsObjIsVisible(oid));
}

Datum
pg_ts_parser_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(TSPARSEROID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(TSParserIsVisible(oid));
}

Datum
pg_ts_dict_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(TSDICTOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(TSDictionaryIsVisible(oid));
}

Datum
pg_ts_template_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(TSTEMPLATEOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(TSTemplateIsVisible(oid));
}

Datum
pg_ts_config_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	if (!SearchSysCacheExists1(TSCONFIGOID, ObjectIdGetDatum(oid)))
		PG_RETURN_NULL();

	PG_RETURN_BOOL(TSConfigIsVisible(oid));
}

Datum
pg_my_temp_schema(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(myTempNamespace);
}

Datum
pg_is_other_temp_schema(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(isOtherTempNamespace(oid));
}
