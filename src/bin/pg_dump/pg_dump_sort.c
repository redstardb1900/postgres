/*-------------------------------------------------------------------------
 *
 * pg_dump_sort.c
 *	  Sort the items of a dump into a safe order for dumping
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_dump_sort.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "catalog/pg_class_d.h"
#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"

/*
 * 数据库对象类型排序优先级。 
 * 对象按类型排序，类型内按名称排序。   
 *
 * 触发器、事件触发器和物化视图被有意排在较后位置。 
 * 触发器必须在全部数据修改之后才恢复，以免干扰    
 * 数据加载。事件触发器倒数第二恢复，以免干扰     
 * 任何类型的对象创建。物化视图刷新排在最后，因为 
 * 它们应在数据库正常状态下执行（例如必须排在     
 * 所有 ACL 恢复之后；此外若它们要查看系统目录，         
 * 就应看到最终的恢复状态）。若想改变这一点，另请     
 * 参见 pg_backup_archiver.c 中的 RestorePass                        
 * 机制。                         
 *
 * 另一方面，类型转换被有意排得比预期更早；            
 * 逻辑上它们应在函数之后，因为它们通常依赖函数。 
 * 这变通解决了后端把使用类型转换的视图记录为     
 * 依赖于该类型转换底层函数的习惯。                    
 * 我们先排序类型转换，转换用到的函数会被提升到 
 * 类型转换之上，而这些函数依赖的视图会被提升到     
 * 函数之上。但未以该方式使用的视图                       
 * 不会被提升。   
 *
 * 注意：对象类型优先级须与 pg_dump.c 的段分配一致；   
 * 即 PRE_DATA 对象必须排在 DO_PRE_DATA_BOUNDARY 之前，             
 * POST_DATA 对象必须排在 DO_POST_DATA_BOUNDARY 之后，             
 * DATA 对象居中。   
 */

/* 此枚举按顺序列出优先级 */
enum dbObjectTypePriorities
{
	PRIO_NAMESPACE = 1,
	PRIO_PROCLANG,
	PRIO_COLLATION,
	PRIO_TRANSFORM,
	PRIO_EXTENSION,
	PRIO_TYPE,					/* 用于 DO_TYPE 和 DO_SHELL_TYPE */
	PRIO_CAST,
	PRIO_FUNC,
	PRIO_AGG,
	PRIO_ACCESS_METHOD,
	PRIO_OPERATOR,
	PRIO_OPFAMILY,				/* 用于 DO_OPFAMILY 和 DO_OPCLASS */
	PRIO_CONVERSION,
	PRIO_TSPARSER,
	PRIO_TSTEMPLATE,
	PRIO_TSDICT,
	PRIO_TSCONFIG,
	PRIO_FDW,
	PRIO_FOREIGN_SERVER,
	PRIO_TABLE,
	PRIO_TABLE_ATTACH,
	PRIO_DUMMY_TYPE,
	PRIO_ATTRDEF,
	PRIO_LARGE_OBJECT,
	PRIO_PRE_DATA_BOUNDARY,		/* 边界！ */
	PRIO_TABLE_DATA,
	PRIO_SEQUENCE_SET,
	PRIO_LARGE_OBJECT_DATA,
	PRIO_POST_DATA_BOUNDARY,	/* 边界！ */
	PRIO_CONSTRAINT,
	PRIO_INDEX,
	PRIO_INDEX_ATTACH,
	PRIO_STATSEXT,
	PRIO_RULE,
	PRIO_TRIGGER,
	PRIO_FK_CONSTRAINT,
	PRIO_POLICY,
	PRIO_PUBLICATION,
	PRIO_PUBLICATION_REL,
	PRIO_PUBLICATION_TABLE_IN_SCHEMA,
	PRIO_SUBSCRIPTION,
	PRIO_DEFAULT_ACL,			/* 在 ACL 遍中完成 */
	PRIO_EVENT_TRIGGER,			/* 必须倒数第二！ */
	PRIO_REFRESH_MATVIEW		/* 必须最后！ */
};

/* 此表按枚举 DumpableObjectType 索引 */
static const int dbObjectTypePriority[] =
{
	PRIO_NAMESPACE,				/* DO_NAMESPACE */
	PRIO_EXTENSION,				/* DO_EXTENSION */
	PRIO_TYPE,					/* DO_TYPE */
	PRIO_TYPE,					/* DO_SHELL_TYPE */
	PRIO_FUNC,					/* DO_FUNC */
	PRIO_AGG,					/* DO_AGG */
	PRIO_OPERATOR,				/* DO_OPERATOR */
	PRIO_ACCESS_METHOD,			/* DO_ACCESS_METHOD */
	PRIO_OPFAMILY,				/* DO_OPCLASS */
	PRIO_OPFAMILY,				/* DO_OPFAMILY */
	PRIO_COLLATION,				/* DO_COLLATION */
	PRIO_CONVERSION,			/* DO_CONVERSION */
	PRIO_TABLE,					/* DO_TABLE */
	PRIO_TABLE_ATTACH,			/* DO_TABLE_ATTACH */
	PRIO_ATTRDEF,				/* DO_ATTRDEF */
	PRIO_INDEX,					/* DO_INDEX */
	PRIO_INDEX_ATTACH,			/* DO_INDEX_ATTACH */
	PRIO_STATSEXT,				/* DO_STATSEXT */
	PRIO_RULE,					/* DO_RULE */
	PRIO_TRIGGER,				/* DO_TRIGGER */
	PRIO_CONSTRAINT,			/* DO_CONSTRAINT */
	PRIO_FK_CONSTRAINT,			/* DO_FK_CONSTRAINT */
	PRIO_PROCLANG,				/* DO_PROCLANG */
	PRIO_CAST,					/* DO_CAST */
	PRIO_TABLE_DATA,			/* DO_TABLE_DATA */
	PRIO_SEQUENCE_SET,			/* DO_SEQUENCE_SET */
	PRIO_DUMMY_TYPE,			/* DO_DUMMY_TYPE */
	PRIO_TSPARSER,				/* DO_TSPARSER */
	PRIO_TSDICT,				/* DO_TSDICT */
	PRIO_TSTEMPLATE,			/* DO_TSTEMPLATE */
	PRIO_TSCONFIG,				/* DO_TSCONFIG */
	PRIO_FDW,					/* DO_FDW */
	PRIO_FOREIGN_SERVER,		/* DO_FOREIGN_SERVER */
	PRIO_DEFAULT_ACL,			/* DO_DEFAULT_ACL */
	PRIO_TRANSFORM,				/* DO_TRANSFORM */
	PRIO_LARGE_OBJECT,			/* DO_LARGE_OBJECT */
	PRIO_LARGE_OBJECT_DATA,		/* DO_LARGE_OBJECT_DATA */
	PRIO_PRE_DATA_BOUNDARY,		/* DO_PRE_DATA_BOUNDARY */
	PRIO_POST_DATA_BOUNDARY,	/* DO_POST_DATA_BOUNDARY */
	PRIO_EVENT_TRIGGER,			/* DO_EVENT_TRIGGER */
	PRIO_REFRESH_MATVIEW,		/* DO_REFRESH_MATVIEW */
	PRIO_POLICY,				/* DO_POLICY */
	PRIO_PUBLICATION,			/* DO_PUBLICATION */
	PRIO_PUBLICATION_REL,		/* DO_PUBLICATION_REL */
	PRIO_PUBLICATION_TABLE_IN_SCHEMA,	/* DO_PUBLICATION_TABLE_IN_SCHEMA */
	PRIO_SUBSCRIPTION			/* DO_SUBSCRIPTION */
};

StaticAssertDecl(lengthof(dbObjectTypePriority) == (DO_SUBSCRIPTION + 1),
				 "array length mismatch");

static DumpId preDataBoundId;
static DumpId postDataBoundId;


static int	DOTypeNameCompare(const void *p1, const void *p2);
static int	pgTypeNameCompare(Oid typid1, Oid typid2);
static int	accessMethodNameCompare(Oid am1, Oid am2);
static bool TopoSort(DumpableObject **objs,
					 int numObjs,
					 DumpableObject **ordering,
					 int *nOrdering);
static void addHeapElement(int val, int *heap, int heapLength);
static int	removeHeapElement(int *heap, int heapLength);
static void findDependencyLoops(DumpableObject **objs, int nObjs, int totObjs);
static int	findLoop(DumpableObject *obj,
					 DumpId startPoint,
					 bool *processed,
					 DumpId *searchFailed,
					 DumpableObject **workspace,
					 int depth);
static void repairDependencyLoop(DumpableObject **loop,
								 int nLoop);
static void describeDumpableObject(DumpableObject *obj,
								   char *buf, int bufsize);


/*
* 将给定对象按类型/名称排序
*
* 通常这只是基于依赖排序的起点。
 *
*/
void
sortDumpableObjectsByTypeName(DumpableObject **objs, int numObjs)
{
	if (numObjs > 1)
		qsort(objs, numObjs, sizeof(DumpableObject *),
			  DOTypeNameCompare);
}

static int
DOTypeNameCompare(const void *p1, const void *p2)
{
	DumpableObject *obj1 = *(DumpableObject *const *) p1;
	DumpableObject *obj2 = *(DumpableObject *const *) p2;
	int			cmpval;

	/* 按类型的优先级排序 */
	cmpval = dbObjectTypePriority[obj1->objType] -
		dbObjectTypePriority[obj2->objType];

	if (cmpval != 0)
		return cmpval;

	/*
	* 按模式排序。通常，相同优先级的所有对象要么都有模式链接，
	* 要么都没有，但也有例外。这种情况下 NULL 模式排在非 NULL 之后。
	 *
	*/
	if (obj1->namespace)
	{
		if (obj2->namespace)
		{
			cmpval = strcmp(obj1->namespace->dobj.name,
							obj2->namespace->dobj.name);
			if (cmpval != 0)
				return cmpval;
		}
		else
			return -1;
	}
	else if (obj2->namespace)
		return 1;

	/*
	* 按名称排序。除少数例外，这里的名称是单个目录列。
	* 要获取更完整的图景，请在 pg_dump.c 中搜索 "dobj.name = "。
	* 这里的名称与纯文本格式输出中的 "Name:" 不匹配，
	* 后者是 _tocEntry.tag。例如，约束的 DumpableObject.name 是
	* pg_constraint.conname，但约束的 _tocEntry.tag 是 relname 和
	* conname 以空格连接。
	*/
	cmpval = strcmp(obj1->name, obj2->name);
	if (cmpval != 0)
		return cmpval;

	/*
	* 按类型排序。这有助于那些共享类型优先级但不共享唯一名称约束的类型，
	* 例如操作符类和操作符族。
	*/
	cmpval = obj1->objType - obj2->objType;
	if (cmpval != 0)
		return cmpval;

	/*
	 * 为获得稳定排序，某些对象类型需要打破平局。     
	 * 大多数目录有自然键，例如 pg_proc_proname_args_nsp_index。 
	 * 当上述"模式"和"名称"比较未涵盖自然键的所有列    
	 * 时，在此比较其余的列。  
	 *
	 * 自然键通常通过代理键引用其他目录。                
	 * 因此，这里将每个这样的引用转换为被引用            
	 * 目录的自然键。这可能沿目录引用下溯多层。        
	 * 例如，要按 pg_proc.proargtypes 排序，需下溯到每个  
	 * pg_type，再进一步下溯到其 pg_namespace，以便整体      
	 * 按 (nspname, typname) 排序。   
	 */
	if (obj1->objType == DO_FUNC || obj1->objType == DO_AGG)
	{
		FuncInfo   *fobj1 = *(FuncInfo *const *) p1;
		FuncInfo   *fobj2 = *(FuncInfo *const *) p2;
		int			i;

		/* 先按参数个数排序，再按参数类型名排序 */
		cmpval = fobj1->nargs - fobj2->nargs;
		if (cmpval != 0)
			return cmpval;
		for (i = 0; i < fobj1->nargs; i++)
		{
			cmpval = pgTypeNameCompare(fobj1->argtypes[i],
									   fobj2->argtypes[i]);
			if (cmpval != 0)
				return cmpval;
		}
	}
	else if (obj1->objType == DO_OPERATOR)
	{
		OprInfo    *oobj1 = *(OprInfo *const *) p1;
		OprInfo    *oobj2 = *(OprInfo *const *) p2;

		/* oprkind 为 'l'、'r' 或 'b'；此排序为前缀、后缀、中缀 */
		cmpval = (oobj2->oprkind - oobj1->oprkind);
		if (cmpval != 0)
			return cmpval;
		/* 同一种 oprkind 内，按参数类型名排序 */
		cmpval = pgTypeNameCompare(oobj1->oprleft, oobj2->oprleft);
		if (cmpval != 0)
			return cmpval;
		cmpval = pgTypeNameCompare(oobj1->oprright, oobj2->oprright);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_OPCLASS)
	{
		OpclassInfo *opcobj1 = *(OpclassInfo *const *) p1;
		OpclassInfo *opcobj2 = *(OpclassInfo *const *) p2;

		/* 按访问方法名排序，遵循 pg_opclass_am_name_nsp_index */
		cmpval = accessMethodNameCompare(opcobj1->opcmethod,
										 opcobj2->opcmethod);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_OPFAMILY)
	{
		OpfamilyInfo *opfobj1 = *(OpfamilyInfo *const *) p1;
		OpfamilyInfo *opfobj2 = *(OpfamilyInfo *const *) p2;

		/* 按访问方法名排序，遵循 pg_opfamily_am_name_nsp_index */
		cmpval = accessMethodNameCompare(opfobj1->opfmethod,
										 opfobj2->opfmethod);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_COLLATION)
	{
		CollInfo   *cobj1 = *(CollInfo *const *) p1;
		CollInfo   *cobj2 = *(CollInfo *const *) p2;

		/*
		* 按编码排序，遵循 pg_collation_name_enc_nsp_index。从技术上讲，
		* 这不是必须的，因为凡此改变导出顺序的地方，恢复导出都会失败。
		* CREATE COLLATION 无法创建需要此排序打破的平局，因为它施加了
		* 限制以确保 (nspname, collname) 在给定 DatabaseEncoding 内唯一
		* 标识排序规则。虽然 pg_import_system_collations() 可以创建平局，
		* 但在 pg_import_system_collations('my_schema') 之后
		* pg_dump+restore 会失败。然而，基于其他地方的那些限制而忽略
		* 一个自然键列收益甚微，因此像处理其他对象类型一样，
		* 尊重完整的自然键。
		 *
		 *
		*/
		cmpval = cobj1->collencoding - cobj2->collencoding;
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_ATTRDEF)
	{
		AttrDefInfo *adobj1 = *(AttrDefInfo *const *) p1;
		AttrDefInfo *adobj2 = *(AttrDefInfo *const *) p2;

		/* 按属性号排序 */
		cmpval = (adobj1->adnum - adobj2->adnum);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_POLICY)
	{
		PolicyInfo *pobj1 = *(PolicyInfo *const *) p1;
		PolicyInfo *pobj2 = *(PolicyInfo *const *) p2;

		/* 按表名排序（表模式已考虑） */
		cmpval = strcmp(pobj1->poltable->dobj.name,
						pobj2->poltable->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_RULE)
	{
		RuleInfo   *robj1 = *(RuleInfo *const *) p1;
		RuleInfo   *robj2 = *(RuleInfo *const *) p2;

		/* 按表名排序（表模式已考虑） */
		cmpval = strcmp(robj1->ruletable->dobj.name,
						robj2->ruletable->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_TRIGGER)
	{
		TriggerInfo *tobj1 = *(TriggerInfo *const *) p1;
		TriggerInfo *tobj2 = *(TriggerInfo *const *) p2;

		/* 按表名排序（表模式已考虑） */
		cmpval = strcmp(tobj1->tgtable->dobj.name,
						tobj2->tgtable->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_CONSTRAINT ||
			 obj1->objType == DO_FK_CONSTRAINT)
	{
		ConstraintInfo *robj1 = *(ConstraintInfo *const *) p1;
		ConstraintInfo *robj2 = *(ConstraintInfo *const *) p2;

		/*
		* 将域约束排在表约束之前，以与我们将 CREATE DOMAIN 排在
		* CREATE TABLE 之前的决定保持一致。
		*/
		if (robj1->condomain)
		{
			if (robj2->condomain)
			{
				/* 按域名排序（域模式已考虑） */
				cmpval = strcmp(robj1->condomain->dobj.name,
								robj2->condomain->dobj.name);
				if (cmpval != 0)
					return cmpval;
			}
			else
				return PRIO_TYPE - PRIO_TABLE;
		}
		else if (robj2->condomain)
			return PRIO_TABLE - PRIO_TYPE;
		else
		{
			/* 按表名排序（表模式已考虑） */
			cmpval = strcmp(robj1->contable->dobj.name,
							robj2->contable->dobj.name);
			if (cmpval != 0)
				return cmpval;
		}
	}
	else if (obj1->objType == DO_DEFAULT_ACL)
	{
		DefaultACLInfo *daclobj1 = *(DefaultACLInfo *const *) p1;
		DefaultACLInfo *daclobj2 = *(DefaultACLInfo *const *) p2;

		/*
		* 按 defaclrole 排序，遵循 pg_default_acl_role_nsp_obj_index。
		* (namespace, name) 匹配 (defaclnamespace, defaclobjtype)。
		*/
		cmpval = strcmp(daclobj1->defaclrole, daclobj2->defaclrole);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_PUBLICATION_REL)
	{
		PublicationRelInfo *probj1 = *(PublicationRelInfo *const *) p1;
		PublicationRelInfo *probj2 = *(PublicationRelInfo *const *) p2;

		/* 按发布名排序，因为 (namespace, name) 匹配 rel */
		cmpval = strcmp(probj1->publication->dobj.name,
						probj2->publication->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}
	else if (obj1->objType == DO_PUBLICATION_TABLE_IN_SCHEMA)
	{
		PublicationSchemaInfo *psobj1 = *(PublicationSchemaInfo *const *) p1;
		PublicationSchemaInfo *psobj2 = *(PublicationSchemaInfo *const *) p2;

		/* 按发布名排序，因为 ->name 仅是 nspname */
		cmpval = strcmp(psobj1->publication->dobj.name,
						psobj2->publication->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}

	/*
	* 正常情况下不会执行到这里，除非目录损坏，但如果真的到了这里，则按 OID 排序。
	* 这可能使逻辑上完全相同的数据库在导出输出中的对象顺序不同。
	* 用户将看到虚假的模式差异。如果回归数据库包含允许测试到达此处的对象，
	* 002_pg_upgrade.pl 测试的 "原始和恢复的回归数据库的导出输出匹配"
	* 将出现不稳定失败。这是该测试使用 "pg_restore -j" 的结果，
	* 它不能完全约束 OID 分配顺序。
	 *
	 *
	*/
	return oidcmp(obj1->catId.oid, obj2->catId.oid);
}

/* 按 nspname 然后按 typname 比较两个 OID 标识的 pg_type 值。 */
static int
pgTypeNameCompare(Oid typid1, Oid typid2)
{
	TypeInfo   *typobj1;
	TypeInfo   *typobj2;
	int			cmpval;

	if (typid1 == typid2)
		return 0;

	typobj1 = findTypeByOid(typid1);
	typobj2 = findTypeByOid(typid2);

	if (!typobj1 || !typobj2)
	{
		/*
		* getTypes() 未找到某个 OID。假定目录损坏，例如 oprright 值
		* 在 pg_type 行中没有对应的 OID。报告为"相等"，
		* 以便调用者使用下一个可用的比较依据，例如下一个函数参数。
		*
		* 一元操作符在 oprleft（oprkind='r' 时）或 oprright（oprkind='l' 时）中
		* 使用 InvalidOid。调用者已按 oprkind 排序，仅对同类操作符调用我们。
		* 因此，"typid1 == typid2" 已处理了 InvalidOid。（v14 移除了后缀操作符
		* 支持。因此，从 v14+ 导出时，只有 oprleft 可能为 InvalidOid。）
		 *
		 *
		*/
		Assert(false);
		return 0;
	}

	if (!typobj1->dobj.namespace || !typobj2->dobj.namespace)
		Assert(false);			/* 目录损坏 */
	else
	{
		cmpval = strcmp(typobj1->dobj.namespace->dobj.name,
						typobj2->dobj.namespace->dobj.name);
		if (cmpval != 0)
			return cmpval;
	}
	return strcmp(typobj1->dobj.name, typobj2->dobj.name);
}

/* 按 amname 比较两个 OID 标识的 pg_am 值。 */
static int
accessMethodNameCompare(Oid am1, Oid am2)
{
	AccessMethodInfo *amobj1;
	AccessMethodInfo *amobj2;

	if (am1 == am2)
		return 0;

	amobj1 = findAccessMethodByOid(am1);
	amobj2 = findAccessMethodByOid(am2);

	if (!amobj1 || !amobj2)
	{
		/* 目录损坏：以与 pgTypeNameCompare() 相同的方式处理 */
		Assert(false);
		return 0;
	}

	return strcmp(amobj1->dobj.name, amobj2->dobj.name);
}


/*
 * 使用依赖信息将给定对象排序为安全导出顺序  
 * （在可获得的范围内）。                
 *
 * PRE_DATA_BOUNDARY、POST_DATA_BOUNDARY 对象的 DumpId 单独传入，
 * 以备在依赖循环修复期间需要用到它们。                   
 */
void
sortDumpableObjects(DumpableObject **objs, int numObjs,
					DumpId preBoundaryId, DumpId postBoundaryId)
{
	DumpableObject **ordering;
	int			nOrdering;

	if (numObjs <= 0)			/* 不会再发生了 ... */
		return;

	/*
	* 将边界 ID 存入静态变量有点笨拙，但似乎比将它们添加到子函数的
	* 参数列表中更好。
	*/
	preDataBoundId = preBoundaryId;
	postDataBoundId = postBoundaryId;

	ordering = (DumpableObject **) pg_malloc(numObjs * sizeof(DumpableObject *));
	while (!TopoSort(objs, numObjs, ordering, &nOrdering))
		findDependencyLoops(ordering, nOrdering, numObjs);

	memcpy(objs, ordering, numObjs * sizeof(DumpableObject *));

	free(ordering);
}

/*
 * TopoSort -- 导出列表的拓扑排序    
 *
 * 生成导出列表的一种重排序，使之满足导出列表中       
 * 所示的全部依赖约束。（每个这样的约束都是             
 * 偏序关系的一个事实。）尽量减少为达成偏序所     
 * 不需要的列表重排。  
 *
 * 输入是 objs[] 中 numObjs 个对象的列表，此列表不会被 
 * 修改。
 *
 * 如果能构建出满足所有依赖约束的排序，则返回 
 * true；否则返回 false（存在相互矛盾的约束）。   
 *
 * 成功时（返回 true），ordering[] 填充为排序后的        
 * DumpableObject 指针数组，长度与输入列表相同。      
 *
 * 失败时（返回 false），ordering[] 填充为未排序的           
 * DumpableObject 指针数组，长度为 *nOrdering，其中列出     
 * 了阻止排序完成的对象。一般而言，这些对象要么        
 * 直接参与某个依赖循环，要么被处于循环中的对象       
 * 所依赖。（后一类对象实际上并无问题，但需要        
 * 进一步分析才能分辨出是哪一类。）           
 *
 * 调用者负责为 *ordering 分配足够的空间。                  
 */
static bool
TopoSort(DumpableObject **objs,
		 int numObjs,
		 DumpableObject **ordering, /* 输出参数 */
		 int *nOrdering)		/* 输出参数 */
{
	DumpId		maxDumpId = getMaxDumpId();
	int		   *pendingHeap;
	int		   *beforeConstraints;
	int		   *idMap;
	DumpableObject *obj;
	int			heapLength;
	int			i,
				j,
				k;

	/*
	 * 这基本上与 Knuth 第一卷中所示的拓扑排序算法        
	 * 相同。不过，我们希望尽量减少对输入顺序        
	 * 的不必要重排；也就是说，当我们有机会选择          
	 * 下一个输出项时，我们总是希望选取原列表中        
	 * 位置最高的那个。因此，我们不像 Knuth 那样        
	 * 维护由待输出项组成的无序链表，而是维护一个       
	 * 由各项编号构成的堆，可将其用作优先队列。      
	 * 这使得算法从 O(N) 变为 O(N log N)，因为每次插入或    
	 * 移除堆项都需要 O(log N) 时间。不过对于此应用     
	 * 而言这仍然足够快。             
	 */

	*nOrdering = numObjs;		/* 用于成功返回 */

	/* 排除空情况 */
	if (numObjs <= 0)
		return true;

	/* 为上述堆创建工作空间 */
	pendingHeap = (int *) pg_malloc(numObjs * sizeof(int));

	/*
	* 扫描约束，对输入中的每一项，生成一个计数，表示有多少约束要求它
	* 必须排在其他项之前。dumpId 为 j 的项的计数存储在
	* beforeConstraints[j] 中。我们还创建一个映射，显示 dumpId 为 j
	* 的项在输入顺序中的索引。
	 *
	*/
	beforeConstraints = (int *) pg_malloc0((maxDumpId + 1) * sizeof(int));
	idMap = (int *) pg_malloc((maxDumpId + 1) * sizeof(int));
	for (i = 0; i < numObjs; i++)
	{
		obj = objs[i];
		j = obj->dumpId;
		if (j <= 0 || j > maxDumpId)
			pg_fatal("invalid dumpId %d", j);
		idMap[j] = i;
		for (j = 0; j < obj->nDeps; j++)
		{
			k = obj->dependencies[j];
			if (k <= 0 || k > maxDumpId)
				pg_fatal("invalid dependency %d", k);
			beforeConstraints[k]++;
		}
	}

	/*
	* 现在通过填入那些已满足 beforeConstraints[id] == 0 的项的  
	* 索引来初始化就绪输出项的堆。
	*
	* 堆的基本属性是 heap[(j-1)/2] >= heap[j] 对 j 在 1..heapLength-1
	* 范围内的每个 j 成立（注意这里使用基于 0 的下标，而 Knuth 的讨论假设
	* 基于 1 的下标）。因此，如果我们简单地将索引以降序填入
	* pendingHeap[]，则在此循环完成时堆不变量（fortiori）得到满足，
	* 无需进行任何上滤比较。
	 *
	*/
	heapLength = 0;
	for (i = numObjs; --i >= 0;)
	{
		if (beforeConstraints[objs[i]->dumpId] == 0)
			pendingHeap[heapLength++] = i;
	}

	/*--------------------
	* 现在输出对象，反向填充输出列表。每一步使用优先堆选择最后一个
	* 没有剩余前置约束的项。我们从堆中移除该项，将其输出到 ordering[]，
	* 并递减它所约束的每个项的前置约束计数。每当某个项的前置约束计数
	* 因此递减为零，我们将其插入优先堆，表示它成为输出候选。
	* 堆变空时完成；如果我们输出了所有元素则成功，否则失败。
	* i = 剩余待输出的 ordering[] 条目数
	* j = 正在输出的项的 objs[] 索引
	* k = 用于扫描项 j 的约束列表的临时变量
	*--------------------
	 *
	 *
	 *
	 *
	*/
	i = numObjs;
	while (heapLength > 0)
	{
		/* 通过移除最大堆成员来选择要输出的对象 */
		j = removeHeapElement(pendingHeap, heapLength--);
		obj = objs[j];
		/* 将候选对象输出到 ordering[] */
		ordering[--i] = obj;
		/* 更新其前驱的 beforeConstraints 计数 */
		for (k = 0; k < obj->nDeps; k++)
		{
			int			id = obj->dependencies[k];

			if ((--beforeConstraints[id]) == 0)
				addHeapElement(idMap[id], pendingHeap, heapLength++);
		}
	}

	/*
	* 如果失败，报告无法输出的对象；这些是 beforeConstraints[] 仍为非零值的对象。
	 *
	*/
	if (i != 0)
	{
		k = 0;
		for (j = 1; j <= maxDumpId; j++)
		{
			if (beforeConstraints[j] != 0)
				ordering[k++] = objs[idMap[j]];
		}
		*nOrdering = k;
	}

	/* 完成 */
	free(pendingHeap);
	free(beforeConstraints);
	free(idMap);

	return (i == 0);
}

/*
* 向堆（优先队列）添加一项
*
* heapLength 是当前堆大小；调用者负责在调用后增加其值。
* *heap 处必须有足够的存储空间。
*/
static void
addHeapElement(int val, int *heap, int heapLength)
{
	int			j;

	/*
	* 按 Knuth 5.2.3 练习 16 对新条目进行上滤。注意 Knuth 使用基于 1
	* 的数组索引，而非基于 0。
	*/
	j = heapLength;
	while (j > 0)
	{
		int			i = (j - 1) >> 1;

		if (val <= heap[i])
			break;
		heap[j] = heap[i];
		j = i;
	}
	heap[j] = val;
}

/*
 * 移除堆（优先队列）中存在的最大项。       
 *
 * heapLength 是当前堆的大小；调用者负责在调用之后        
 * 减小它的值。       
 *
 * 我们移除并返回 heap[0]（它始终是堆的最大元素），
 * 然后"上滤"以维持堆的不变量。                   
 */
static int
removeHeapElement(int *heap, int heapLength)
{
	int			result = heap[0];
	int			val;
	int			i;

	if (--heapLength <= 0)
		return result;
	val = heap[heapLength];		/* 必须重新插入的值 */
	i = 0;						/* i 是"空洞"所在位置 */
	for (;;)
	{
		int			j = 2 * i + 1;

		if (j >= heapLength)
			break;
		if (j + 1 < heapLength &&
			heap[j] < heap[j + 1])
			j++;
		if (val >= heap[j])
			break;
		heap[i] = heap[j];
		i = j;
	}
	heap[i] = val;
	return result;
}

/*
* findDependencyLoops - 识别 TopoSort 失败输出中的循环，
*		并将每个此类循环传递给 repairDependencyLoop() 处理
*
* 一般情况下，TopoSort 返回的对象集中可能存在多个循环；
* 为了提高速度，我们应尽可能多地修复循环后再重试 TopoSort。
* 我们可以安全地修复不相交的循环（没有共同成员）；
* 如果发现重叠的循环，则只修复第一个发现的循环，
* 因为修复第一个循环的操作可能也已修复了其他循环。
* （如果没有，我们会在下一轮修复。）
*
* objs[] 列出 TopoSort 无法排序的对象
* nObjs 是此类对象的数量
* totObjs 是整个范围内的对象总数
 *
*/
static void
findDependencyLoops(DumpableObject **objs, int nObjs, int totObjs)
{
	/*
	 * 这里使用三个数据结构： 
	 *
	 * processed[] 是按 dump ID 索引的 bool 数组，标记本次    
	 * findDependencyLoops() 调用中已处理过的对象。           
	 *
	 * searchFailed[] 是另一个按 dump ID 索引的数组。              
	 * 若已证明不存在从对象 j 回到起点 k 的依赖路径， 
	 * 则把 searchFailed[j] 设为 dump ID k。                          
	 * 这使我们在从 k 到 j 存在多条依赖路径时跳过无用的  
	 * 搜索，这很常见。本可用简单 bool 数组，但那样   
	 * 就要为每个起始点重新清零，造成 O(N^2) 清零工作。  
	 * 把起始点的 dump ID 用作"真"值，                             
	 * 就能在考虑下一个起始点之前省去对数组的清空        
	 * 了。
	 *
	 * workspace[] 是 DumpableObject 指针数组，我们在其中         
	 * 尝试构建构成循环的对象列表。我们把 workspace[] 设  
	 * 得足以容纳 TopoSort 的全部输出，这在多数情况下  
	 * 都极为过度，但若存在一条连接所有对象的依赖链，  
	 * 则理论上可能是必要的。                 
	 */
	bool	   *processed;
	DumpId	   *searchFailed;
	DumpableObject **workspace;
	bool		fixedloop;
	int			i;

	processed = (bool *) pg_malloc0((getMaxDumpId() + 1) * sizeof(bool));
	searchFailed = (DumpId *) pg_malloc0((getMaxDumpId() + 1) * sizeof(DumpId));
	workspace = (DumpableObject **) pg_malloc(totObjs * sizeof(DumpableObject *));
	fixedloop = false;

	for (i = 0; i < nObjs; i++)
	{
		DumpableObject *obj = objs[i];
		int			looplen;
		int			j;

		looplen = findLoop(obj,
						   obj->dumpId,
						   processed,
						   searchFailed,
						   workspace,
						   0);

		if (looplen > 0)
		{
			/* 发现循环，修复它 */
			repairDependencyLoop(workspace, looplen);
			fixedloop = true;
			/* 将循环成员标记为已处理 */
			for (j = 0; j < looplen; j++)
				processed[workspace[j]->dumpId] = true;
		}
		else
		{
			/*
			* 没有以该对象为起点的循环，但仍将其标记为已处理。
			* 这对于正确性并非必需，但可以节省后续 findLoop() 调用
			* 无用地追踪对该对象的引用。
			 *
			*/
			processed[obj->dumpId] = true;
		}
	}

	/* 我们至少应修复了一个循环 */
	if (!fixedloop)
		pg_fatal("could not identify dependency loop");

	free(workspace);
	free(searchFailed);
	free(processed);
}

/*
* 递归搜索不包含任何已处理对象的循环依赖。
*
*	obj: 当前正在检查的对象
*	startPoint: 期望循环的起始对象的 dumpId
*	processed[]: 标记已处理对象的标志数组
*	searchFailed[]: 标记已访问但未成功找到路径的对象的标志数组
*	workspace[]: 正在其中构建循环成员列表的工作数组
*	depth: 调用时 workspace[] 中的有效条目数
*
* 成功时，返回循环长度，workspace[] 填充为循环成员的指针。
* 失败时，返回 0。
*
* 注意：给定的起始对象可能属于多个循环；如果是这样，
* 我们将找到其中任意一个循环。
 *
*/
static int
findLoop(DumpableObject *obj,
		 DumpId startPoint,
		 bool *processed,
		 DumpId *searchFailed,
		 DumpableObject **workspace,
		 int depth)
{
	int			i;

	/*
	* 如果 obj 已处理则拒绝。此检查防止我们发现与先前已处理循环
	* 重叠的循环。
	*/
	if (processed[obj->dumpId])
		return 0;

	/*
	* 如果我们已证明不存在从该对象回到 startPoint 的路径，
	* 则跳过。
	*/
	if (searchFailed[obj->dumpId] == startPoint)
		return 0;

	/*
	* 如果 obj 已存在于 workspace 中则拒绝。此检查防止在以下情况下
	* 进入无限递归：给定的 startPoint 对象链接到它不所属的循环，
	* 并且它保证我们不会溢出 workspace[] 的分配大小。
	 *
	*/
	for (i = 0; i < depth; i++)
	{
		if (workspace[i] == obj)
			return 0;
	}

	/*
	* 好的，暂将 obj 添加到 workspace
	*/
	workspace[depth++] = obj;

	/*
	* 检查是否已找到回到期望 startPoint 的循环；如果是，完成
	*/
	for (i = 0; i < obj->nDeps; i++)
	{
		if (obj->dependencies[i] == startPoint)
			return depth;
	}

	/*
	* 沿每条出边递归
	*/
	for (i = 0; i < obj->nDeps; i++)
	{
		DumpableObject *nextobj = findObjectByDumpId(obj->dependencies[i]);
		int			newDepth;

		if (!nextobj)
			continue;			/* 忽略对未导出对象的依赖 */
		newDepth = findLoop(nextobj,
							startPoint,
							processed,
							searchFailed,
							workspace,
							depth);
		if (newDepth > 0)
			return newDepth;
	}

	/*
	* 记住不存在从此处回到 startPoint 的路径
	*/
	searchFailed[obj->dumpId] = startPoint;

	return 0;
}

/*
* 用户定义的数据类型与其每个 I/O 函数之间存在依赖循环
* （因为这些函数以该数据类型作为输入或输出）。
* 类似地，范围类型与其规范化函数（如果有）之间也存在循环。
* 打破循环的方法是让函数改为依赖于关联的壳类型。
 *
*/
static void
repairTypeFuncLoop(DumpableObject *typeobj, DumpableObject *funcobj)
{
	TypeInfo   *typeInfo = (TypeInfo *) typeobj;

	/* 移除函数对类型的依赖 */
	removeObjectDependency(funcobj, typeobj->dumpId);

	/* 改为添加函数对壳类型的依赖 */
	if (typeInfo->shellType)
	{
		addObjectDependency(funcobj, typeInfo->shellType->dobj.dumpId);

		/*
		* 标记壳类型（始终包含定义，因为我们需要壳类型被定义
		* 才能完全标识函数）如有任何此类函数需要导出则一并导出
		 *
		*/
		if (funcobj->dump)
			typeInfo->shellType->dobj.dump = funcobj->dump |
				DUMP_COMPONENT_DEFINITION;
	}
}

/*
 * 因为我们强制视图依赖于其 ON SELECT 规则，而另一个
 * 方向上存在隐含依赖，所以我们需要打破这个循环。  
 * 如果循环中没有其他对象，我们就可以移除隐含        
 * 依赖，并让 ON SELECT 规则保持非分离状态。          
 * 这也适用于物化视图。    
 */
static void
repairViewRuleLoop(DumpableObject *viewobj,
				   DumpableObject *ruleobj)
{
	/* 移除规则对视图的依赖 */
	removeObjectDependency(ruleobj, viewobj->dumpId);
	/* 两个对象的标志已为此情况正确设置 */
}

/*
* 然而，如果循环中还有其他对象，我们必须通过将 ON SELECT 规则
* 变为单独导出的对象来打破循环。
*
* 因为 findLoop() 先找到较短的循环再找到较长的循环，很可能我们
* 先前已触发了 repairViewRuleLoop() 并移除了规则对视图的依赖。
* 将其放回以确保规则不会在视图之前输出。
*
* 注意：此方法目前不适用于物化视图。
 *
*/
static void
repairViewRuleMultiLoop(DumpableObject *viewobj,
						DumpableObject *ruleobj)
{
	TableInfo  *viewinfo = (TableInfo *) viewobj;
	RuleInfo   *ruleinfo = (RuleInfo *) ruleobj;

	/* 移除视图对规则的依赖 */
	removeObjectDependency(viewobj, ruleobj->dumpId);
	/* 标记视图将以虚拟定义打印 */
	viewinfo->dummy_view = true;
	/* 标记规则需要单独的导出 */
	ruleinfo->separate = true;
	/* 放回规则对视图的依赖 */
	addObjectDependency(ruleobj, viewobj->dumpId);
	/* 现在规则分离了，必须为 post-data */
	addObjectDependency(ruleobj, postDataBoundId);
}

/*
* 如果物化视图涉及多对象循环，目前无法通过分离规则来修复。
* 作为权宜之计，我们尝试通过放弃物化视图必须在 pre-data 段中导出的约束
* 来修复。这足以处理物化视图依赖于某个唯一索引的情况，
* 例如当它有 GROUP BY 时。
*
* 注意"下一个对象"不一定是物化视图本身；
* 例如它可能是物化视图的行类型。在移除所有 pre-data 链接的过程中，
* 我们可能多次经过这里。特别是，如果有其他物化视图依赖于有问题
* 的物化视图，我们会为每个这样的物化视图经过这里，并将它们都标记为
* 延迟。（这之所以有效，是因为所有 MV 初始都有 pre-data 依赖，
* 所以每个 MV 都会被访问到。）
 *
 *
*/
static void
repairMatViewBoundaryMultiLoop(DumpableObject *boundaryobj,
							   DumpableObject *nextobj)
{
	/* 移除边界对循环中其后对象的依赖 */
	removeObjectDependency(boundaryobj, nextobj->dumpId);
	/* 如果该对象是物化视图，将其标记为延迟到 post-data */
	if (nextobj->objType == DO_TABLE)
	{
		TableInfo  *nextinfo = (TableInfo *) nextobj;

		if (nextinfo->relkind == RELKIND_MATVIEW)
			nextinfo->postponed_def = true;
	}
}

/*
* 如果函数涉及多对象循环，目前无法通过将其拆分为两个 DumpableObject
* 来修复。作为权宜之计，我们尝试通过放弃函数必须在 pre-data 段中导出
* 的约束来修复。这足以处理函数依赖于某个唯一索引的情况，
* 例如当它有 GROUP BY 时。
 *
*/
static void
repairFunctionBoundaryMultiLoop(DumpableObject *boundaryobj,
								DumpableObject *nextobj)
{
	/* 移除边界对循环中其后对象的依赖 */
	removeObjectDependency(boundaryobj, nextobj->dumpId);
	/* 如果该对象是函数，将其标记为延迟到 post-data */
	if (nextobj->objType == DO_FUNC)
	{
		FuncInfo   *nextinfo = (FuncInfo *) nextobj;

		nextinfo->postponed_def = true;
	}
}

/*
* 因为我们使表依赖于其 CHECK 约束，而另一个方向存在自动依赖，
* 所以我们需要打破循环。如果循环中没有其他对象，我们可以移除
* 自动依赖并保持 CHECK 约束为非分离的。
 *
*/
static void
repairTableConstraintLoop(DumpableObject *tableobj,
						  DumpableObject *constraintobj)
{
	/* 移除约束对表的依赖 */
	removeObjectDependency(constraintobj, tableobj->dumpId);
}

/*
* 然而，如果循环中还有其他对象，我们必须通过将 CHECK 约束
* 变为单独导出的对象来打破循环。
*
* 因为 findLoop() 先找到较短的循环再找到较长的循环，很可能我们
* 先前已触发了 repairTableConstraintLoop() 并移除了约束对表的依赖。
* 将其放回以确保约束不会在表之前输出...
 *
*/
static void
repairTableConstraintMultiLoop(DumpableObject *tableobj,
							   DumpableObject *constraintobj)
{
	/* 移除表对约束的依赖 */
	removeObjectDependency(tableobj, constraintobj->dumpId);
	/* 标记约束需要单独的导出 */
	((ConstraintInfo *) constraintobj)->separate = true;
	/* 放回约束对表的依赖 */
	addObjectDependency(constraintobj, tableobj->dumpId);
	/* 现在约束分离了，必须为 post-data */
	addObjectDependency(constraintobj, postDataBoundId);
}

/*
* 属性默认值的行为与 CHECK 约束完全相同...
*/
static void
repairTableAttrDefLoop(DumpableObject *tableobj,
					   DumpableObject *attrdefobj)
{
	/* 移除属性默认值对表的依赖 */
	removeObjectDependency(attrdefobj, tableobj->dumpId);
}

static void
repairTableAttrDefMultiLoop(DumpableObject *tableobj,
							DumpableObject *attrdefobj)
{
	/* 移除表对属性默认值的依赖 */
	removeObjectDependency(tableobj, attrdefobj->dumpId);
	/* 标记属性默认值需要单独的导出 */
	((AttrDefInfo *) attrdefobj)->separate = true;
	/* 放回属性默认值对表的依赖 */
	addObjectDependency(attrdefobj, tableobj->dumpId);
}

/*
* 域上的 CHECK 约束与表上的行为完全相同...
*/
static void
repairDomainConstraintLoop(DumpableObject *domainobj,
						   DumpableObject *constraintobj)
{
	/* 移除约束对域的依赖 */
	removeObjectDependency(constraintobj, domainobj->dumpId);
}

static void
repairDomainConstraintMultiLoop(DumpableObject *domainobj,
								DumpableObject *constraintobj)
{
	/* 移除域对约束的依赖 */
	removeObjectDependency(domainobj, constraintobj->dumpId);
	/* 标记约束需要单独的导出 */
	((ConstraintInfo *) constraintobj)->separate = true;
	/* 放回约束对域的依赖 */
	addObjectDependency(constraintobj, domainobj->dumpId);
	/* 现在约束分离了，必须为 post-data */
	addObjectDependency(constraintobj, postDataBoundId);
}

static void
repairIndexLoop(DumpableObject *partedindex,
				DumpableObject *partindex)
{
	removeObjectDependency(partedindex, partindex->dumpId);
}

/*
* 修复依赖循环，或尝试至死...
*
* 此例程主要关注将循环的各种出现形式归约为常见情况，
* 然后交给上面的"修复器"例程处理。
 *
*/
static void
repairDependencyLoop(DumpableObject **loop,
					 int nLoop)
{
	int			i,
				j;

	/* 数据类型及其 I/O 或规范化函数之一 */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TYPE &&
		loop[1]->objType == DO_FUNC)
	{
		repairTypeFuncLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TYPE &&
		loop[0]->objType == DO_FUNC)
	{
		repairTypeFuncLoop(loop[1], loop[0]);
		return;
	}

	/* 视图（包括物化视图）及其 ON SELECT 规则 */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TABLE &&
		loop[1]->objType == DO_RULE &&
		(((TableInfo *) loop[0])->relkind == RELKIND_VIEW ||
		 ((TableInfo *) loop[0])->relkind == RELKIND_MATVIEW) &&
		((RuleInfo *) loop[1])->ev_type == '1' &&
		((RuleInfo *) loop[1])->is_instead &&
		((RuleInfo *) loop[1])->ruletable == (TableInfo *) loop[0])
	{
		repairViewRuleLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TABLE &&
		loop[0]->objType == DO_RULE &&
		(((TableInfo *) loop[1])->relkind == RELKIND_VIEW ||
		 ((TableInfo *) loop[1])->relkind == RELKIND_MATVIEW) &&
		((RuleInfo *) loop[0])->ev_type == '1' &&
		((RuleInfo *) loop[0])->is_instead &&
		((RuleInfo *) loop[0])->ruletable == (TableInfo *) loop[1])
	{
		repairViewRuleLoop(loop[1], loop[0]);
		return;
	}

	/* 涉及视图（但不包括物化视图）和 ON SELECT 规则的间接循环 */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE &&
				((TableInfo *) loop[i])->relkind == RELKIND_VIEW)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_RULE &&
						((RuleInfo *) loop[j])->ev_type == '1' &&
						((RuleInfo *) loop[j])->is_instead &&
						((RuleInfo *) loop[j])->ruletable == (TableInfo *) loop[i])
					{
						repairViewRuleMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/* 涉及物化视图和数据边界的间接循环 */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE &&
				((TableInfo *) loop[i])->relkind == RELKIND_MATVIEW)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_PRE_DATA_BOUNDARY)
					{
						DumpableObject *nextobj;

						nextobj = (j < nLoop - 1) ? loop[j + 1] : loop[0];
						repairMatViewBoundaryMultiLoop(loop[j], nextobj);
						return;
					}
				}
			}
		}
	}

	/* 涉及函数和数据边界的间接循环 */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_FUNC)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_PRE_DATA_BOUNDARY)
					{
						DumpableObject *nextobj;

						nextobj = (j < nLoop - 1) ? loop[j + 1] : loop[0];
						repairFunctionBoundaryMultiLoop(loop[j], nextobj);
						return;
					}
				}
			}
		}
	}

	/* 表和 CHECK 约束 */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TABLE &&
		loop[1]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[1])->contype == 'c' &&
		((ConstraintInfo *) loop[1])->contable == (TableInfo *) loop[0])
	{
		repairTableConstraintLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TABLE &&
		loop[0]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[0])->contype == 'c' &&
		((ConstraintInfo *) loop[0])->contable == (TableInfo *) loop[1])
	{
		repairTableConstraintLoop(loop[1], loop[0]);
		return;
	}

	/* 涉及表和 CHECK 约束的间接循环 */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_CONSTRAINT &&
						((ConstraintInfo *) loop[j])->contype == 'c' &&
						((ConstraintInfo *) loop[j])->contable == (TableInfo *) loop[i])
					{
						repairTableConstraintMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/* 表和属性默认值 */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TABLE &&
		loop[1]->objType == DO_ATTRDEF &&
		((AttrDefInfo *) loop[1])->adtable == (TableInfo *) loop[0])
	{
		repairTableAttrDefLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TABLE &&
		loop[0]->objType == DO_ATTRDEF &&
		((AttrDefInfo *) loop[0])->adtable == (TableInfo *) loop[1])
	{
		repairTableAttrDefLoop(loop[1], loop[0]);
		return;
	}

	/* 分区表上的索引和对应分区上的索引 */
	if (nLoop == 2 &&
		loop[0]->objType == DO_INDEX &&
		loop[1]->objType == DO_INDEX)
	{
		if (((IndxInfo *) loop[0])->parentidx == loop[1]->catId.oid)
		{
			repairIndexLoop(loop[0], loop[1]);
			return;
		}
		else if (((IndxInfo *) loop[1])->parentidx == loop[0]->catId.oid)
		{
			repairIndexLoop(loop[1], loop[0]);
			return;
		}
	}

	/* 涉及表和属性默认值的间接循环 */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TABLE)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_ATTRDEF &&
						((AttrDefInfo *) loop[j])->adtable == (TableInfo *) loop[i])
					{
						repairTableAttrDefMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/* 域和 CHECK 约束 */
	if (nLoop == 2 &&
		loop[0]->objType == DO_TYPE &&
		loop[1]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[1])->contype == 'c' &&
		((ConstraintInfo *) loop[1])->condomain == (TypeInfo *) loop[0])
	{
		repairDomainConstraintLoop(loop[0], loop[1]);
		return;
	}
	if (nLoop == 2 &&
		loop[1]->objType == DO_TYPE &&
		loop[0]->objType == DO_CONSTRAINT &&
		((ConstraintInfo *) loop[0])->contype == 'c' &&
		((ConstraintInfo *) loop[0])->condomain == (TypeInfo *) loop[1])
	{
		repairDomainConstraintLoop(loop[1], loop[0]);
		return;
	}

	/* 涉及域和 CHECK 约束的间接循环 */
	if (nLoop > 2)
	{
		for (i = 0; i < nLoop; i++)
		{
			if (loop[i]->objType == DO_TYPE)
			{
				for (j = 0; j < nLoop; j++)
				{
					if (loop[j]->objType == DO_CONSTRAINT &&
						((ConstraintInfo *) loop[j])->contype == 'c' &&
						((ConstraintInfo *) loop[j])->condomain == (TypeInfo *) loop[i])
					{
						repairDomainConstraintMultiLoop(loop[i], loop[j]);
						return;
					}
				}
			}
		}
	}

	/*
	* 表自身的循环 --- 直接忽略。
	*
	* （实际上，这源自表列对另一列的依赖，在 v15 之前生成列会出现；
	* 或者表列对整个表的依赖，分区时会出现。但我们在收集依赖数据时
	* 未关注子对象 ID，因此这里无法看到这些细节。）
	 *
	 *
	*/
	if (nLoop == 1)
	{
		if (loop[0]->objType == DO_TABLE)
		{
			removeObjectDependency(loop[0], loop[0]->dumpId);
			return;
		}
	}

	/*
	* 如果所有对象都是 TABLE_DATA 项，则必定是循环外键约束集
	* （或单个自引用表）。打印适当的投诉并任意打破循环。
	 *
	*/
	for (i = 0; i < nLoop; i++)
	{
		if (loop[i]->objType != DO_TABLE_DATA)
			break;
	}
	if (i >= nLoop)
	{
		pg_log_warning(ngettext("there are circular foreign-key constraints on this table:",
								"there are circular foreign-key constraints among these tables:",
								nLoop));
		for (i = 0; i < nLoop; i++)
			pg_log_warning_detail("%s", loop[i]->name);
		pg_log_warning_hint("You might not be able to restore the dump without using --disable-triggers or temporarily dropping the constraints.");
		pg_log_warning_hint("Consider using a full dump instead of a --data-only dump to avoid this problem.");
		if (nLoop > 1)
			removeObjectDependency(loop[0], loop[1]->dumpId);
		else					/* 必为自依赖 */
			removeObjectDependency(loop[0], loop[0]->dumpId);
		return;
	}

	/*
	* 如果找不到原则性的方法来打破循环，则投诉并任意打破。
	 *
	*/
	pg_log_warning("could not resolve dependency loop among these items:");
	for (i = 0; i < nLoop; i++)
	{
		char		buf[1024];

		describeDumpableObject(loop[i], buf, sizeof(buf));
		pg_log_warning_detail("%s", buf);
	}

	if (nLoop > 1)
		removeObjectDependency(loop[0], loop[1]->dumpId);
	else						/* 必为自依赖 */
		removeObjectDependency(loop[0], loop[0]->dumpId);
}

/*
* 为错误描述可导出对象
*
* 这可能应该放在其他地方...
*/
static void
describeDumpableObject(DumpableObject *obj, char *buf, int bufsize)
{
	switch (obj->objType)
	{
		case DO_NAMESPACE:
			snprintf(buf, bufsize,
					 "SCHEMA %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_EXTENSION:
			snprintf(buf, bufsize,
					 "EXTENSION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TYPE:
			snprintf(buf, bufsize,
					 "TYPE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_SHELL_TYPE:
			snprintf(buf, bufsize,
					 "SHELL TYPE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FUNC:
			snprintf(buf, bufsize,
					 "FUNCTION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_AGG:
			snprintf(buf, bufsize,
					 "AGGREGATE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_OPERATOR:
			snprintf(buf, bufsize,
					 "OPERATOR %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_ACCESS_METHOD:
			snprintf(buf, bufsize,
					 "ACCESS METHOD %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_OPCLASS:
			snprintf(buf, bufsize,
					 "OPERATOR CLASS %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_OPFAMILY:
			snprintf(buf, bufsize,
					 "OPERATOR FAMILY %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_COLLATION:
			snprintf(buf, bufsize,
					 "COLLATION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_CONVERSION:
			snprintf(buf, bufsize,
					 "CONVERSION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TABLE:
			snprintf(buf, bufsize,
					 "TABLE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TABLE_ATTACH:
			snprintf(buf, bufsize,
					 "TABLE ATTACH %s  (ID %d)",
					 obj->name, obj->dumpId);
			return;
		case DO_ATTRDEF:
			snprintf(buf, bufsize,
					 "ATTRDEF %s.%s  (ID %d OID %u)",
					 ((AttrDefInfo *) obj)->adtable->dobj.name,
					 ((AttrDefInfo *) obj)->adtable->attnames[((AttrDefInfo *) obj)->adnum - 1],
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_INDEX:
			snprintf(buf, bufsize,
					 "INDEX %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_INDEX_ATTACH:
			snprintf(buf, bufsize,
					 "INDEX ATTACH %s  (ID %d)",
					 obj->name, obj->dumpId);
			return;
		case DO_STATSEXT:
			snprintf(buf, bufsize,
					 "STATISTICS %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_REFRESH_MATVIEW:
			snprintf(buf, bufsize,
					 "REFRESH MATERIALIZED VIEW %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_RULE:
			snprintf(buf, bufsize,
					 "RULE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TRIGGER:
			snprintf(buf, bufsize,
					 "TRIGGER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_EVENT_TRIGGER:
			snprintf(buf, bufsize,
					 "EVENT TRIGGER %s (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_CONSTRAINT:
			snprintf(buf, bufsize,
					 "CONSTRAINT %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FK_CONSTRAINT:
			snprintf(buf, bufsize,
					 "FK CONSTRAINT %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_PROCLANG:
			snprintf(buf, bufsize,
					 "PROCEDURAL LANGUAGE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_CAST:
			snprintf(buf, bufsize,
					 "CAST %u to %u  (ID %d OID %u)",
					 ((CastInfo *) obj)->castsource,
					 ((CastInfo *) obj)->casttarget,
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_TRANSFORM:
			snprintf(buf, bufsize,
					 "TRANSFORM %u lang %u  (ID %d OID %u)",
					 ((TransformInfo *) obj)->trftype,
					 ((TransformInfo *) obj)->trflang,
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_TABLE_DATA:
			snprintf(buf, bufsize,
					 "TABLE DATA %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_SEQUENCE_SET:
			snprintf(buf, bufsize,
					 "SEQUENCE SET %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_DUMMY_TYPE:
			snprintf(buf, bufsize,
					 "DUMMY TYPE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSPARSER:
			snprintf(buf, bufsize,
					 "TEXT SEARCH PARSER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSDICT:
			snprintf(buf, bufsize,
					 "TEXT SEARCH DICTIONARY %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSTEMPLATE:
			snprintf(buf, bufsize,
					 "TEXT SEARCH TEMPLATE %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_TSCONFIG:
			snprintf(buf, bufsize,
					 "TEXT SEARCH CONFIGURATION %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FDW:
			snprintf(buf, bufsize,
					 "FOREIGN DATA WRAPPER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_FOREIGN_SERVER:
			snprintf(buf, bufsize,
					 "FOREIGN SERVER %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_DEFAULT_ACL:
			snprintf(buf, bufsize,
					 "DEFAULT ACL %s  (ID %d OID %u)",
					 obj->name, obj->dumpId, obj->catId.oid);
			return;
		case DO_LARGE_OBJECT:
			snprintf(buf, bufsize,
					 "LARGE OBJECT  (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_LARGE_OBJECT_DATA:
			snprintf(buf, bufsize,
					 "LARGE OBJECT DATA  (ID %d)",
					 obj->dumpId);
			return;
		case DO_POLICY:
			snprintf(buf, bufsize,
					 "POLICY (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_PUBLICATION:
			snprintf(buf, bufsize,
					 "PUBLICATION (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_PUBLICATION_REL:
			snprintf(buf, bufsize,
					 "PUBLICATION TABLE (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_PUBLICATION_TABLE_IN_SCHEMA:
			snprintf(buf, bufsize,
					 "PUBLICATION TABLES IN SCHEMA (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_SUBSCRIPTION:
			snprintf(buf, bufsize,
					 "SUBSCRIPTION (ID %d OID %u)",
					 obj->dumpId, obj->catId.oid);
			return;
		case DO_PRE_DATA_BOUNDARY:
			snprintf(buf, bufsize,
					 "PRE-DATA BOUNDARY  (ID %d)",
					 obj->dumpId);
			return;
		case DO_POST_DATA_BOUNDARY:
			snprintf(buf, bufsize,
					 "POST-DATA BOUNDARY  (ID %d)",
					 obj->dumpId);
			return;
	}
	/* 不应执行到这里 */
	snprintf(buf, bufsize,
			 "object type %d  (ID %d OID %u)",
			 (int) obj->objType,
			 obj->dumpId, obj->catId.oid);
}
