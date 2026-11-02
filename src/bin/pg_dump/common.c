/*-------------------------------------------------------------------------
 *
 * common.c
 *	Catalog routines used by pg_dump; long ago these were shared
 *	by another dump tool, but not anymore.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/common.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>

#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_extension_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_publication_d.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "fe_utils/string_utils.h"
#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"

/*
 * 用于将 DumpId 映射到 DumpableObject 的变量
 */
static DumpableObject **dumpIdMap = NULL;
static int	allocedDumpIds = 0;
static DumpId lastDumpId = 0;	/* 注意：0 是 InvalidDumpId */

/*
 * 用于将 CatalogId 映射到 DumpableObject 的基础设施
 *
 * 我们使用由 simplehash.h 生成的哈希表。该基础设施
 * 要求所有哈希表条目大小相同，并且期望在调整表大小时
 * 可以移动它们。因此我们不能让 DumpableObject 直接成为
 * 哈希表的元素；取而代之的是，哈希表元素包含指向
 * DumpableObject 的指针。
 *
 * 实际上，将此数据结构同时用于映射 CatalogId 到拥有的扩展
 * （如有）也很方便。由于扩展成员关系数据在大多数
 * DumpableObject 创建之前被读取，dobj 和 ext 两者中
 * 任一都可能为 NULL。
 */
typedef struct _catalogIdMapEntry
{
	CatalogId	catId;			/* 被索引的 CatalogId */
	uint32		status;			/* 哈希状态 */
	uint32		hashval;		/* CatalogId 的哈希码 */
	DumpableObject *dobj;		/* 关联的 DumpableObject（如有） */
	ExtensionInfo *ext;			/* 拥有的扩展（如有） */
} CatalogIdMapEntry;

#define SH_PREFIX		catalogid
#define SH_ELEMENT_TYPE	CatalogIdMapEntry
#define SH_KEY_TYPE		CatalogId
#define	SH_KEY			catId
#define SH_HASH_KEY(tb, key)	hash_bytes((const unsigned char *) &(key), sizeof(CatalogId))
#define SH_EQUAL(tb, a, b)		((a).oid == (b).oid && (a).tableoid == (b).tableoid)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) (a)->hashval
#define	SH_SCOPE		static inline
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

#define CATALOGIDHASH_INITIAL_SIZE	10000

static catalogid_hash *catalogIdHash = NULL;

static void flagInhTables(Archive *fout, TableInfo *tblinfo, int numTables,
						  InhInfo *inhinfo, int numInherits);
static void flagInhIndexes(Archive *fout, TableInfo *tblinfo, int numTables);
static void flagInhAttrs(DumpOptions *dopt, TableInfo *tblinfo, int numTables);
static int	strInArray(const char *pattern, char **arr, int arr_size);
static IndxInfo *findIndexByOid(Oid oid);


/*
 * getSchemaData
 *	  收集所有可能导出对象的信息
 */
TableInfo *
getSchemaData(Archive *fout, int *numTablesPtr)
{
	TableInfo  *tblinfo;
	ExtensionInfo *extinfo;
	InhInfo    *inhinfo;
	int			numTables;
	int			numTypes;
	int			numFuncs;
	int			numOperators;
	int			numCollations;
	int			numNamespaces;
	int			numExtensions;
	int			numPublications;
	int			numAggregates;
	int			numInherits;
	int			numRules;
	int			numProcLangs;
	int			numCasts;
	int			numTransforms;
	int			numAccessMethods;
	int			numOpclasses;
	int			numOpfamilies;
	int			numConversions;
	int			numTSParsers;
	int			numTSTemplates;
	int			numTSDicts;
	int			numTSConfigs;
	int			numForeignDataWrappers;
	int			numForeignServers;
	int			numDefaultACLs;
	int			numEventTriggers;

	/*
	 * 我们必须先读取扩展和扩展成员信息，
	 * 因为在决定其他对象是否需要导出时，
	 * 需要查询扩展成员关系。
	 */
	pg_log_info("reading extensions");
	extinfo = getExtensions(fout, &numExtensions);

	pg_log_info("identifying extension members");
	getExtensionMembership(fout, extinfo, numExtensions);

	pg_log_info("reading schemas");
	(void) getNamespaces(fout, &numNamespaces);

	/*
	 * getTables 应尽快完成，以最小化
	 * 开始事务和获取每表锁之间的时间窗口。
	 * 然而，我们必须先执行 getNamespaces，因为表
	 * 在 getTables 期间会链接到其包含的命名空间。
	 */
	pg_log_info("reading user-defined tables");
	tblinfo = getTables(fout, &numTables);

	getOwnedSeqs(fout, tblinfo, numTables);

	pg_log_info("reading user-defined functions");
	(void) getFuncs(fout, &numFuncs);

	/* 必须在 getTables 和 getFuncs 之后 */
	pg_log_info("reading user-defined types");
	(void) getTypes(fout, &numTypes);

	/* 也必须在 getFuncs 之后 */
	pg_log_info("reading procedural languages");
	getProcLangs(fout, &numProcLangs);

	pg_log_info("reading user-defined aggregate functions");
	getAggregates(fout, &numAggregates);

	pg_log_info("reading user-defined operators");
	(void) getOperators(fout, &numOperators);

	pg_log_info("reading user-defined access methods");
	getAccessMethods(fout, &numAccessMethods);

	pg_log_info("reading user-defined operator classes");
	getOpclasses(fout, &numOpclasses);

	pg_log_info("reading user-defined operator families");
	getOpfamilies(fout, &numOpfamilies);

	pg_log_info("reading user-defined text search parsers");
	getTSParsers(fout, &numTSParsers);

	pg_log_info("reading user-defined text search templates");
	getTSTemplates(fout, &numTSTemplates);

	pg_log_info("reading user-defined text search dictionaries");
	getTSDictionaries(fout, &numTSDicts);

	pg_log_info("reading user-defined text search configurations");
	getTSConfigurations(fout, &numTSConfigs);

	pg_log_info("reading user-defined foreign-data wrappers");
	getForeignDataWrappers(fout, &numForeignDataWrappers);

	pg_log_info("reading user-defined foreign servers");
	getForeignServers(fout, &numForeignServers);

	pg_log_info("reading default privileges");
	getDefaultACLs(fout, &numDefaultACLs);

	pg_log_info("reading user-defined collations");
	(void) getCollations(fout, &numCollations);

	pg_log_info("reading user-defined conversions");
	getConversions(fout, &numConversions);

	pg_log_info("reading type casts");
	getCasts(fout, &numCasts);

	pg_log_info("reading transforms");
	getTransforms(fout, &numTransforms);

	pg_log_info("reading table inheritance information");
	inhinfo = getInherits(fout, &numInherits);

	pg_log_info("reading event triggers");
	getEventTriggers(fout, &numEventTriggers);

	/* 识别应导出的扩展配置表 */
	pg_log_info("finding extension tables");
	processExtensionTables(fout, extinfo, numExtensions);

	/* 将表链接到父表，标记目标表的父表为感兴趣的 */
	pg_log_info("finding inheritance relationships");
	flagInhTables(fout, tblinfo, numTables, inhinfo, numInherits);

	pg_log_info("reading column info for interesting tables");
	getTableAttrs(fout, tblinfo, numTables);

	pg_log_info("flagging inherited columns in subtables");
	flagInhAttrs(fout->dopt, tblinfo, numTables);

	pg_log_info("reading partitioning data");
	getPartitioningInfo(fout);

	pg_log_info("reading indexes");
	getIndexes(fout, tblinfo, numTables);

	pg_log_info("flagging indexes in partitioned tables");
	flagInhIndexes(fout, tblinfo, numTables);

	pg_log_info("reading extended statistics");
	getExtendedStatistics(fout);

	pg_log_info("reading constraints");
	getConstraints(fout, tblinfo, numTables);

	pg_log_info("reading triggers");
	getTriggers(fout, tblinfo, numTables);

	pg_log_info("reading rewrite rules");
	getRules(fout, &numRules);

	pg_log_info("reading policies");
	getPolicies(fout, tblinfo, numTables);

	pg_log_info("reading publications");
	(void) getPublications(fout, &numPublications);

	pg_log_info("reading publication membership of tables");
	getPublicationTables(fout, tblinfo, numTables);

	pg_log_info("reading publication membership of schemas");
	getPublicationNamespaces(fout);

	pg_log_info("reading subscriptions");
	getSubscriptions(fout);

	free(inhinfo);				/* 不再需要 */

	*numTablesPtr = numTables;
	return tblinfo;
}

/* flagInhTables -
 *	 为需要这些信息的表填充父表链接字段，
 *	 将目标表的父表标记为感兴趣的，并为分区表创建
 *	 具有适当依赖链接的 TableAttachInfo 对象。
 *
 * 注意，只有目标的直接祖先被标记为感兴趣的。
 * 这足够了；我们不太关心它们是否继承了属性。
 *
 * 修改 tblinfo
 *
 *
 */
static void
flagInhTables(Archive *fout, TableInfo *tblinfo, int numTables,
			  InhInfo *inhinfo, int numInherits)
{
	TableInfo  *child = NULL;
	TableInfo  *parent = NULL;
	int			i,
				j;

	/*
	 * 建立子表到其父表的链接。
	 *
	 * 我们曾经尝试跳过那些不需要导出的表的此项工作；
	 * 但可优化的情况在实践中很少见，并且批量设置这些链接
	 * 比旧方法更便宜。（特别注意，子表有多个父表的情况非常罕见。）
	 *
	 */
	for (i = 0; i < numInherits; i++)
	{
		/*
		 * 如果表与上次相同，跳过哈希表查找。
		 * 这对子表不太可能，但对父表可能性更大一些。
		 * （也许我们应该要求后端提供排序后的数组以使此情况更可能？
		 * 但不太清楚排序开销是否值得。）
		 */
		if (child == NULL ||
			child->dobj.catId.oid != inhinfo[i].inhrelid)
		{
			child = findTableByOid(inhinfo[i].inhrelid);

			/*
			 * 如果找不到 TableInfo，假设该 pg_inherits 条目
			 * 是针对分区索引的，我们无需跟踪。
			 */
			if (child == NULL)
				continue;
		}
		if (parent == NULL ||
			parent->dobj.catId.oid != inhinfo[i].inhparent)
		{
			parent = findTableByOid(inhinfo[i].inhparent);
			if (parent == NULL)
				pg_fatal("failed sanity check, parent OID %u of table \"%s\" (OID %u) not found",
						 inhinfo[i].inhparent,
						 child->dobj.name,
						 child->dobj.catId.oid);
		}
		/* 将此父表添加到子表的父表列表中。 */
		if (child->numParents > 0)
			child->parents = pg_realloc_array(child->parents,
											  TableInfo *,
											  child->numParents + 1);
		else
			child->parents = pg_malloc_array(TableInfo *, 1);
		child->parents[child->numParents++] = parent;
	}

	/*
	 * 现在考虑所有子表，根据需要标记父表为感兴趣的。
	 */
	for (i = 0; i < numTables; i++)
	{
		/*
		 * 如果需要，将父表标记为 getTableAttrs 和 getIndexes
		 * 感兴趣的。我们只需要对可导出表的直接父表这样做。
		 *
		 */
		if (tblinfo[i].dobj.dump)
		{
			int			numParents = tblinfo[i].numParents;
			TableInfo **parents = tblinfo[i].parents;

			for (j = 0; j < numParents; j++)
				parents[j]->interesting = true;
		}

		/* 如果需要则创建 TableAttachInfo 对象 */
		if ((tblinfo[i].dobj.dump & DUMP_COMPONENT_DEFINITION) &&
			tblinfo[i].ispartition)
		{
			TableAttachInfo *attachinfo;

			/* 分区表只能有一个父表 */
			if (tblinfo[i].numParents != 1)
				pg_fatal("invalid number of parents %d for table \"%s\"",
						 tblinfo[i].numParents,
						 tblinfo[i].dobj.name);

			attachinfo = (TableAttachInfo *) palloc(sizeof(TableAttachInfo));
			attachinfo->dobj.objType = DO_TABLE_ATTACH;
			attachinfo->dobj.catId.tableoid = 0;
			attachinfo->dobj.catId.oid = 0;
			AssignDumpId(&attachinfo->dobj);
			attachinfo->dobj.name = pg_strdup(tblinfo[i].dobj.name);
			attachinfo->dobj.namespace = tblinfo[i].dobj.namespace;
			attachinfo->parentTbl = tblinfo[i].parents[0];
			attachinfo->partitionTbl = &tblinfo[i];

			/*
			 * 我们必须显式声明 DO_TABLE_ATTACH 对象的依赖关系，
			 * 因为它不会匹配 pg_depend 中的任何内容。
			 *
			 * 让它同时依赖于分区表和父表，
			 * 这样它不会在这两者存在之前执行。
			 * （无需关心它们的创建顺序。）
			 *
			 */
			addObjectDependency(&attachinfo->dobj, tblinfo[i].dobj.dumpId);
			addObjectDependency(&attachinfo->dobj, tblinfo[i].parents[0]->dobj.dumpId);
		}
	}
}

/*
 * flagInhIndexes -
 *	 为分区索引创建 IndexAttachInfo 对象，
 *	 并添加适当的依赖链接。
 */
static void
flagInhIndexes(Archive *fout, TableInfo tblinfo[], int numTables)
{
	int			i,
				j;

	for (i = 0; i < numTables; i++)
	{
		if (!tblinfo[i].ispartition || tblinfo[i].numParents == 0)
			continue;

		Assert(tblinfo[i].numParents == 1);

		for (j = 0; j < tblinfo[i].numIndexes; j++)
		{
			IndxInfo   *index = &(tblinfo[i].indexes[j]);
			IndxInfo   *parentidx;
			IndexAttachInfo *attachinfo;

			if (index->parentidx == 0)
				continue;

			parentidx = findIndexByOid(index->parentidx);
			if (parentidx == NULL)
				continue;

			attachinfo = pg_malloc_object(IndexAttachInfo);

			attachinfo->dobj.objType = DO_INDEX_ATTACH;
			attachinfo->dobj.catId.tableoid = 0;
			attachinfo->dobj.catId.oid = 0;
			AssignDumpId(&attachinfo->dobj);
			attachinfo->dobj.name = pg_strdup(index->dobj.name);
			attachinfo->dobj.namespace = index->indextable->dobj.namespace;
			attachinfo->parentIdx = parentidx;
			attachinfo->partitionIdx = index;

			/*
			 * 我们必须显式声明 DO_INDEX_ATTACH 对象的依赖关系，
			 * 因为它不会匹配 pg_depend 中的任何内容。
			 *
			 * 让它同时依赖于分区索引和父表索引，
			 * 这样它不会在这两者存在之前执行。
			 * （无需关心它们的创建顺序。）
			 *
			 * 此外，让它依赖索引的底层表。
			 * 这对串行恢复顺序来说没有太大价值，
			 * 但确保并行恢复不会尝试在那些表上
			 * 与其他操作并发执行 ATTACH。
			 *
			 *
			 */
			addObjectDependency(&attachinfo->dobj, index->dobj.dumpId);
			addObjectDependency(&attachinfo->dobj, parentidx->dobj.dumpId);
			addObjectDependency(&attachinfo->dobj,
								index->indextable->dobj.dumpId);
			addObjectDependency(&attachinfo->dobj,
								parentidx->indextable->dobj.dumpId);

			/* 跟踪父索引中的分区列表 */
			simple_ptr_list_append(&parentidx->partattaches, &attachinfo->dobj);
		}
	}
}

/* flagInhAttrs -
 *	 为 tblinfo 中每个可导出的表标记其继承的属性
 *
 * 我们需要做的是：
 *
 * - 检测哪些子列从父表继承了 NOT NULL 位，
 *   这样我们就不需要为该子列再次指定。
 *
 * - 检测哪些子列的 DEFAULT 为 NULL 而父表有某个非空默认值。
 *   在这种情况下，我们构造一个虚拟的 AttrDefInfo 对象，
 *   以便正确发出必要的 DEFAULT NULL 子句；
 *   否则后端将对此列应用继承的默认值。
 *
 * - 检测哪些子列具有生成表达式且其所有父表也有
 *   相同的生成表达式，如果是则抑制子列的表达式。
 *   子列将自动继承生成表达式，因此无需导出它。
 *   这提高了导出文件与 pre-v16 服务器的兼容性，
 *   后者不允许显式给出子列的表达式。
 *   例外情况：如果它是分区表或我们处于二进制升级模式，
 *   我们仍然导出此类表达式，因为在这些情况下
 *   继承表会先独立重建，然后再重新附加到父表。
 *   （另见 dumpTableSchema() 中的逻辑。）
 *
 * 修改 tblinfo
 */
static void
flagInhAttrs(DumpOptions *dopt, TableInfo *tblinfo, int numTables)
{
	int			i,
				j,
				k;

	/*
	 * 我们按照 OID 顺序扫描表，因为 tblinfo[] 就是这样排序的。
	 * 因此我们通常会在子表之前访问其父表 ---
	 * 但这*不*是保证的。因此此循环必须小心，
	 * 不要以可能改变其他迭代中子表决策的方式
	 * 来修改表属性。
	 */
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &(tblinfo[i]);
		int			numParents;
		TableInfo **parents;

		/* 某些类型永远不会有父表 */
		if (tbinfo->relkind == RELKIND_SEQUENCE ||
			tbinfo->relkind == RELKIND_VIEW ||
			tbinfo->relkind == RELKIND_MATVIEW)
			continue;

		/* 也不为非目标表计算任何信息 */
		if (!tbinfo->dobj.dump)
			continue;

		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		if (numParents == 0)
			continue;			/* 此处无事，继续前进 */

		/* 对于每一列，在父表中搜索匹配的列名 */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			bool		foundNotNull;	/* 属性在父表中为 NOT NULL */
			bool		foundDefault;	/* 在父表中找到默认值 */
			bool		foundSameGenerated; /* 找到匹配的 GENERATED */
			bool		foundDiffGenerated; /* 找到不匹配的 GENERATED */

			/* 检查已删除的列毫无意义 */
			if (tbinfo->attisdropped[j])
				continue;

			foundNotNull = false;
			foundDefault = false;
			foundSameGenerated = false;
			foundDiffGenerated = false;
			for (k = 0; k < numParents; k++)
			{
				TableInfo  *parent = parents[k];
				int			inhAttrInd;

				inhAttrInd = strInArray(tbinfo->attnames[j],
										parent->attnames,
										parent->numatts);
				if (inhAttrInd >= 0)
				{
					AttrDefInfo *parentDef = parent->attrdefs[inhAttrInd];

					foundNotNull |= parent->notnull[inhAttrInd];
					foundDefault |= (parentDef != NULL &&
									 strcmp(parentDef->adef_expr, "NULL") != 0 &&
									 !parent->attgenerated[inhAttrInd]);
					if (parent->attgenerated[inhAttrInd])
					{
						/* 这些指针空值检查只是出于谨慎 */
						if (parentDef != NULL &&
							tbinfo->attrdefs[j] != NULL &&
							strcmp(parentDef->adef_expr,
								   tbinfo->attrdefs[j]->adef_expr) == 0)
							foundSameGenerated = true;
						else
							foundDiffGenerated = true;
					}
				}
			}

			/* 记住是否找到继承的 NOT NULL */
			tbinfo->inhNotNull[j] = foundNotNull;

			/*
			 * 必要时制造一个 DEFAULT NULL 子句。这违反了上面给出的
			 * 关于避免修改可能在其他循环迭代中被检查的状态的建议。
			 * 我们通过让上面的 foundDefault 测试检查
			 * adef_expr 是否为 "NULL" 来防止问题，
			 * 这样它会在完成此操作前后得出相同的结论。
			 *
			 */
			if (foundDefault && tbinfo->attrdefs[j] == NULL)
			{
				AttrDefInfo *attrDef;

				attrDef = pg_malloc_object(AttrDefInfo);
				attrDef->dobj.objType = DO_ATTRDEF;
				attrDef->dobj.catId.tableoid = 0;
				attrDef->dobj.catId.oid = 0;
				AssignDumpId(&attrDef->dobj);
				attrDef->dobj.name = pg_strdup(tbinfo->dobj.name);
				attrDef->dobj.namespace = tbinfo->dobj.namespace;
				attrDef->dobj.dump = tbinfo->dobj.dump;

				attrDef->adtable = tbinfo;
				attrDef->adnum = j + 1;
				attrDef->adef_expr = pg_strdup("NULL");

				/* 该列会被显式导出吗？ */
				if (shouldPrintColumn(dopt, tbinfo, j))
				{
					attrDef->separate = false;
					/* 无需依赖：NULL 不能有依赖关系 */
				}
				else
				{
					/* 列将被抑制，单独打印默认值 */
					attrDef->separate = true;
					/* 确保在表之后输出 */
					addObjectDependency(&attrDef->dobj,
										tbinfo->dobj.dumpId);
				}

				tbinfo->attrdefs[j] = attrDef;
			}

			/* 如果生成表达式可继承则无需导出 */
			if (foundSameGenerated && !foundDiffGenerated &&
				!tbinfo->ispartition && !dopt->binary_upgrade)
				tbinfo->attrdefs[j]->dobj.dump = DUMP_COMPONENT_NONE;
		}
	}
}

/*
 * AssignDumpId
 *		给定一个新创建的可导出对象，分配一个导出 ID，
 *		并将对象注册到查找表中。
 *
 * 调用者应已填写 objType 和 catId，
 * 但 DumpableObject 的其他标准字段尚未填写。
 */
void
AssignDumpId(DumpableObject *dobj)
{
	dobj->dumpId = ++lastDumpId;
	dobj->name = NULL;			/* 必须稍后设置 */
	dobj->namespace = NULL;		/* 可稍后设置 */
	dobj->dump = DUMP_COMPONENT_ALL;	/* 默认假设 */
	dobj->dump_contains = DUMP_COMPONENT_ALL;	/* 默认假设 */
	/* 所有对象都有定义；我们稍后可能设置更多组件位 */
	dobj->components = DUMP_COMPONENT_DEFINITION;
	dobj->ext_member = false;	/* 默认假设 */
	dobj->depends_on_ext = false;	/* 默认假设 */
	dobj->dependencies = NULL;
	dobj->nDeps = 0;
	dobj->allocDeps = 0;

	/* 将对象添加到 dumpIdMap[]，必要时扩展该数组 */
	while (dobj->dumpId >= allocedDumpIds)
	{
		int			newAlloc;

		if (allocedDumpIds <= 0)
		{
			newAlloc = 256;
			dumpIdMap = pg_malloc_array(DumpableObject *, newAlloc);
		}
		else
		{
			newAlloc = allocedDumpIds * 2;
			dumpIdMap = pg_realloc_array(dumpIdMap, DumpableObject *, newAlloc);
		}
		memset(dumpIdMap + allocedDumpIds, 0,
			   (newAlloc - allocedDumpIds) * sizeof(DumpableObject *));
		allocedDumpIds = newAlloc;
	}
	dumpIdMap[dobj->dumpId] = dobj;

	/* 如果它有有效的 CatalogId，将其注册到哈希表中 */
	if (OidIsValid(dobj->catId.tableoid))
	{
		CatalogIdMapEntry *entry;
		bool		found;

		/* 如果尚未初始化 CatalogId 哈希表则初始化 */
		if (catalogIdHash == NULL)
			catalogIdHash = catalogid_create(CATALOGIDHASH_INITIAL_SIZE, NULL);

		entry = catalogid_insert(catalogIdHash, dobj->catId, &found);
		if (!found)
		{
			entry->dobj = NULL;
			entry->ext = NULL;
		}
		Assert(entry->dobj == NULL);
		entry->dobj = dobj;
	}
}

/*
 * 分配一个不与 DumpableObject 绑定的 DumpId。
 *
 * 在创建不需要参与排序逻辑的 "固定" ArchiveEntry 时使用。
 *
 */
DumpId
createDumpId(void)
{
	return ++lastDumpId;
}

/*
 * 返回到目前为止分配的最大 DumpId
 */
DumpId
getMaxDumpId(void)
{
	return lastDumpId;
}

/*
 * 通过导出 ID 查找 DumpableObject
 *
 * 对于无效 ID 返回 NULL
 */
DumpableObject *
findObjectByDumpId(DumpId dumpId)
{
	if (dumpId <= 0 || dumpId >= allocedDumpIds)
		return NULL;			/* 超出范围？ */
	return dumpIdMap[dumpId];
}

/*
 * 通过目录 ID 查找 DumpableObject
 *
 * 对于未知 ID 返回 NULL
 */
DumpableObject *
findObjectByCatalogId(CatalogId catalogId)
{
	CatalogIdMapEntry *entry;

	if (catalogIdHash == NULL)
		return NULL;			/* 尚无对象存在 */

	entry = catalogid_lookup(catalogIdHash, catalogId);
	if (entry == NULL)
		return NULL;
	return entry->dobj;
}

/*
 * 构建指向所有已知可导出对象的指针数组
 *
 * 这只是创建内部映射的可修改副本。
 */
void
getDumpableObjects(DumpableObject ***objs, int *numObjs)
{
	int			i,
				j;

	*objs = pg_malloc_array(DumpableObject *, allocedDumpIds);
	j = 0;
	for (i = 1; i < allocedDumpIds; i++)
	{
		if (dumpIdMap[i])
			(*objs)[j++] = dumpIdMap[i];
	}
	*numObjs = j;
}

/*
 * 向 DumpableObject 添加依赖链接
 *
 * 注意：当前不消除重复依赖
 */
void
addObjectDependency(DumpableObject *dobj, DumpId refId)
{
	if (dobj->nDeps >= dobj->allocDeps)
	{
		if (dobj->allocDeps <= 0)
		{
			dobj->allocDeps = 16;
			dobj->dependencies = pg_malloc_array(DumpId, dobj->allocDeps);
		}
		else
		{
			dobj->allocDeps *= 2;
			dobj->dependencies = pg_realloc_array(dobj->dependencies,
												  DumpId, dobj->allocDeps);
		}
	}
	dobj->dependencies[dobj->nDeps++] = refId;
}

/*
 * 从 DumpableObject 移除依赖链接
 *
 * 如果存在多个链接，全部移除
 */
void
removeObjectDependency(DumpableObject *dobj, DumpId refId)
{
	int			i;
	int			j = 0;

	for (i = 0; i < dobj->nDeps; i++)
	{
		if (dobj->dependencies[i] != refId)
			dobj->dependencies[j++] = dobj->dependencies[i];
	}
	dobj->nDeps = j;
}


/*
 * findTableByOid
 *	  查找具有给定 OID 的表的 DumpableObject
 *	  未找到则返回 NULL
 */
TableInfo *
findTableByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = RelationRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_TABLE);
	return (TableInfo *) dobj;
}

/*
 * findIndexByOid
 *	  查找具有给定 OID 的索引的 DumpableObject
 *	  未找到则返回 NULL
 */
static IndxInfo *
findIndexByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = RelationRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_INDEX);
	return (IndxInfo *) dobj;
}

/*
 * findTypeByOid
 *	  查找具有给定 OID 的类型的 DumpableObject
 *	  未找到则返回 NULL
 */
TypeInfo *
findTypeByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = TypeRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL ||
		   dobj->objType == DO_TYPE || dobj->objType == DO_DUMMY_TYPE);
	return (TypeInfo *) dobj;
}

/*
 * findFuncByOid
 *	  查找具有给定 OID 的函数的 DumpableObject
 *	  未找到则返回 NULL
 */
FuncInfo *
findFuncByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = ProcedureRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_FUNC);
	return (FuncInfo *) dobj;
}

/*
 * findOprByOid
 *	  查找具有给定 OID 的操作符的 DumpableObject
 *	  未找到则返回 NULL
 */
OprInfo *
findOprByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = OperatorRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_OPERATOR);
	return (OprInfo *) dobj;
}

/*
 * findAccessMethodByOid
 *	  查找具有给定 OID 的访问方法的 DumpableObject
 *	  未找到则返回 NULL
 */
AccessMethodInfo *
findAccessMethodByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = AccessMethodRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_ACCESS_METHOD);
	return (AccessMethodInfo *) dobj;
}

/*
 * findCollationByOid
 *	  查找具有给定 OID 的排序规则的 DumpableObject
 *	  未找到则返回 NULL
 */
CollInfo *
findCollationByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = CollationRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_COLLATION);
	return (CollInfo *) dobj;
}

/*
 * findNamespaceByOid
 *	  查找具有给定 OID 的命名空间的 DumpableObject
 *	  未找到则返回 NULL
 */
NamespaceInfo *
findNamespaceByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = NamespaceRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_NAMESPACE);
	return (NamespaceInfo *) dobj;
}

/*
 * findExtensionByOid
 *	  查找具有给定 OID 的扩展的 DumpableObject
 *	  未找到则返回 NULL
 */
ExtensionInfo *
findExtensionByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = ExtensionRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_EXTENSION);
	return (ExtensionInfo *) dobj;
}

/*
 * findPublicationByOid
 *	  查找具有给定 OID 的发布的 DumpableObject
 *	  未找到则返回 NULL
 */
PublicationInfo *
findPublicationByOid(Oid oid)
{
	CatalogId	catId;
	DumpableObject *dobj;

	catId.tableoid = PublicationRelationId;
	catId.oid = oid;
	dobj = findObjectByCatalogId(catId);
	Assert(dobj == NULL || dobj->objType == DO_PUBLICATION);
	return (PublicationInfo *) dobj;
}


/*
 * recordExtensionMembership
 *	  记录由给定目录 ID 标识的对象属于给定扩展
 *
 */
void
recordExtensionMembership(CatalogId catId, ExtensionInfo *ext)
{
	CatalogIdMapEntry *entry;
	bool		found;

	/* 如果有 ExtensionInfo，CatalogId 哈希表必须存在 */
	Assert(catalogIdHash != NULL);

	/* 添加对 CatalogId 哈希的引用 */
	entry = catalogid_insert(catalogIdHash, catId, &found);
	if (!found)
	{
		entry->dobj = NULL;
		entry->ext = NULL;
	}
	Assert(entry->ext == NULL);
	entry->ext = ext;
}

/*
 * findOwningExtension
 *	  返回指定目录 ID 的所属扩展，无则返回 NULL
 */
ExtensionInfo *
findOwningExtension(CatalogId catalogId)
{
	CatalogIdMapEntry *entry;

	if (catalogIdHash == NULL)
		return NULL;			/* 尚无对象存在 */

	entry = catalogid_lookup(catalogIdHash, catalogId);
	if (entry == NULL)
		return NULL;
	return entry->ext;
}


/*
 * parseOidArray
 *	  将以空格分隔的数字字符串解析为字符数组
 *
 * 注意：实际上这用于 OID 和可能带符号的属性编号。
 * 这不应引起问题，但如果确实有问题，我们可以将函数
 * 拆分为两个具有不同参数类型的函数。
 */

void
parseOidArray(const char *str, Oid *array, int arraysize)
{
	int			j,
				argNum;
	char		temp[100];
	char		s;

	argNum = 0;
	j = 0;
	for (;;)
	{
		s = *str++;
		if (s == ' ' || s == '\0')
		{
			if (j > 0)
			{
				if (argNum >= arraysize)
					pg_fatal("could not parse numeric array \"%s\": too many numbers", str);
				temp[j] = '\0';
				array[argNum++] = atooid(temp);
				j = 0;
			}
			if (s == '\0')
				break;
		}
		else
		{
			if (!(isdigit((unsigned char) s) || s == '-') ||
				j >= sizeof(temp) - 1)
				pg_fatal("could not parse numeric array \"%s\": invalid character in number", str);
			temp[j++] = s;
		}
	}

	while (argNum < arraysize)
		array[argNum++] = InvalidOid;
}


/*
 * strInArray:
 *	  接受一个字符串、一个字符串数组以及字符串数组中的元素数量。
 *	  如果字符串在数组中的某个位置，返回其索引，否则返回 -1
 *
 */

static int
strInArray(const char *pattern, char **arr, int arr_size)
{
	int			i;

	for (i = 0; i < arr_size; i++)
	{
		if (strcmp(pattern, arr[i]) == 0)
			return i;
	}
	return -1;
}
