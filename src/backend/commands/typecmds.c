/*-------------------------------------------------------------------------
 *
 * typecmds.c
 *	  Routines for SQL commands that manipulate types (and domains).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/typecmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send functions
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/* get_rels_with_domain() 的结果结构 */
typedef struct
{
	Relation	rel;			/* 已锁定并打开的关系 */
	int			natts;			/* 关注的属性数量 */
	int		   *atts;			/* 属性编号 */
	/* atts[] 的分配长度为 RelationGetNumberOfAttributes(rel) */
} RelToCheck;

/* AlterTypeRecurse() 的参数结构 */
typedef struct
{
	/* 指示要更新哪些类型属性的标志 */
	bool		updateStorage;
	bool		updateReceive;
	bool		updateSend;
	bool		updateTypmodin;
	bool		updateTypmodout;
	bool		updateAnalyze;
	bool		updateSubscript;
	/* 相关属性的新值 */
	char		storage;
	Oid			receiveOid;
	Oid			sendOid;
	Oid			typmodinOid;
	Oid			typmodoutOid;
	Oid			analyzeOid;
	Oid			subscriptOid;
} AlterTypeRecurseParams;

/* 可能由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_array_pg_type_oid = InvalidOid;
Oid			binary_upgrade_next_mrng_pg_type_oid = InvalidOid;
Oid			binary_upgrade_next_mrng_array_pg_type_oid = InvalidOid;

static void makeRangeConstructors(const char *name, Oid namespace,
								  Oid rangeOid, Oid subtype);
static void makeMultirangeConstructors(const char *name, Oid namespace,
									   Oid multirangeOid, Oid rangeOid,
									   Oid rangeArrayOid, Oid *castFuncOid);
static Oid	findTypeInputFunction(List *procname, Oid typeOid);
static Oid	findTypeOutputFunction(List *procname, Oid typeOid);
static Oid	findTypeReceiveFunction(List *procname, Oid typeOid);
static Oid	findTypeSendFunction(List *procname, Oid typeOid);
static Oid	findTypeTypmodinFunction(List *procname);
static Oid	findTypeTypmodoutFunction(List *procname);
static Oid	findTypeAnalyzeFunction(List *procname, Oid typeOid);
static Oid	findTypeSubscriptingFunction(List *procname, Oid typeOid);
static Oid	findRangeSubOpclass(List *opcname, Oid subtype);
static Oid	findRangeCanonicalFunction(List *procname, Oid typeOid);
static Oid	findRangeSubtypeDiffFunction(List *procname, Oid subtype);
static void validateDomainConstraint(Oid domainoid, char *ccbin);
static List *get_rels_with_domain(Oid domainOid, LOCKMODE lockmode);
static void checkEnumOwner(HeapTuple tup);
static char *domainAddConstraint(Oid domainOid, Oid domainNamespace,
								 Oid baseTypeOid,
								 int typMod, Constraint *constr,
								 const char *domainName, ObjectAddress *constrAddr);
static Node *replace_domain_constraint_value(ParseState *pstate,
											 ColumnRef *cref);
static void AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
							 HeapTuple tup, Relation catalog,
							 AlterTypeRecurseParams *atparams);


/* 
 * DefineType
 *         注册一个新的基础类型。
 */
ObjectAddress
DefineType(ParseState *pstate, List *names, List *parameters)
{
	char	   *typeName;
	Oid			typeNamespace;
	int16		internalLength = -1;	/* 默认值：变长 */
	List	   *inputName = NIL;
	List	   *outputName = NIL;
	List	   *receiveName = NIL;
	List	   *sendName = NIL;
	List	   *typmodinName = NIL;
	List	   *typmodoutName = NIL;
	List	   *analyzeName = NIL;
	List	   *subscriptName = NIL;
	char		category = TYPCATEGORY_USER;
	bool		preferred = false;
	char		delimiter = DEFAULT_TYPDELIM;
	Oid			elemType = InvalidOid;
	char	   *defaultValue = NULL;
	bool		byValue = false;
	char		alignment = TYPALIGN_INT;	/* 默认对齐方式 */
	char		storage = TYPSTORAGE_PLAIN; /* 默认 TOAST 存储方法 */
	Oid			collation = InvalidOid;
	DefElem    *likeTypeEl = NULL;
	DefElem    *internalLengthEl = NULL;
	DefElem    *inputNameEl = NULL;
	DefElem    *outputNameEl = NULL;
	DefElem    *receiveNameEl = NULL;
	DefElem    *sendNameEl = NULL;
	DefElem    *typmodinNameEl = NULL;
	DefElem    *typmodoutNameEl = NULL;
	DefElem    *analyzeNameEl = NULL;
	DefElem    *subscriptNameEl = NULL;
	DefElem    *categoryEl = NULL;
	DefElem    *preferredEl = NULL;
	DefElem    *delimiterEl = NULL;
	DefElem    *elemTypeEl = NULL;
	DefElem    *defaultValueEl = NULL;
	DefElem    *byValueEl = NULL;
	DefElem    *alignmentEl = NULL;
	DefElem    *storageEl = NULL;
	DefElem    *collatableEl = NULL;
	Oid			inputOid;
	Oid			outputOid;
	Oid			receiveOid = InvalidOid;
	Oid			sendOid = InvalidOid;
	Oid			typmodinOid = InvalidOid;
	Oid			typmodoutOid = InvalidOid;
	Oid			analyzeOid = InvalidOid;
	Oid			subscriptOid = InvalidOid;
	char	   *array_type;
	Oid			array_oid;
	Oid			typoid;
	ListCell   *pl;
	ObjectAddress address;

	/* 从 Postgres 8.4 开始，创建基础类型需要超级用户权限。
	 * 这纯粹是出于谨慎：不正确的类型定义有太多方式可以破坏
	 * 系统（例如，表示参数与 C 代码预期的不匹配）。实际上，
	 * 创建 I/O 函数就需要超级用户权限，因此先前要求拥有 I/O 函数
	 * 所有权的要求就已经强制了超级用户身份。我们在这里
	 * 只是再次确认。
	 * 
	 * 如果移除此检查，请重新启用下面的 NOT_USED 代码段。
	 * 
	 * 
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create a base type")));

	/* 将名称列表转换为名称和命名空间 */
	typeNamespace = QualifiedNameGetCreationNamespace(names, &typeName);

#ifdef NOT_USED
	/* XXX 鉴于上面的超级用户检查，这是不必要的 */
	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));
#endif

	/*
	 * 查看类型是否已存在。     
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/* 如果不是 shell，查看它是否是自动生成的数组类型，如果是，
	 * 则将其重命名移开。
	 * 
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/* 如果此命令是无参数的 CREATE TYPE，那么我们只是要
	 * 创建一个 shell 类型，请执行此操作（如果已存在 shell 则失败）。
	 * 
	 */
	if (parameters == NIL)
	{
		if (OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		address = TypeShellMake(typeName, typeNamespace, GetUserId());
		return address;
	}

	/* 否则，我们必须已经有了一个 shell 类型，因为没有其他
	 * 方式可以创建 I/O 函数。
	 * 
	 */
	if (!OidIsValid(typoid))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" does not exist", typeName),
				 errhint("Create the type as a shell type, then create its I/O functions, then do a full CREATE TYPE.")));

	/* 从参数列表中提取参数 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		DefElem   **defelp;

		if (strcmp(defel->defname, "like") == 0)
			defelp = &likeTypeEl;
		else if (strcmp(defel->defname, "internallength") == 0)
			defelp = &internalLengthEl;
		else if (strcmp(defel->defname, "input") == 0)
			defelp = &inputNameEl;
		else if (strcmp(defel->defname, "output") == 0)
			defelp = &outputNameEl;
		else if (strcmp(defel->defname, "receive") == 0)
			defelp = &receiveNameEl;
		else if (strcmp(defel->defname, "send") == 0)
			defelp = &sendNameEl;
		else if (strcmp(defel->defname, "typmod_in") == 0)
			defelp = &typmodinNameEl;
		else if (strcmp(defel->defname, "typmod_out") == 0)
			defelp = &typmodoutNameEl;
		else if (strcmp(defel->defname, "analyze") == 0 ||
				 strcmp(defel->defname, "analyse") == 0)
			defelp = &analyzeNameEl;
		else if (strcmp(defel->defname, "subscript") == 0)
			defelp = &subscriptNameEl;
		else if (strcmp(defel->defname, "category") == 0)
			defelp = &categoryEl;
		else if (strcmp(defel->defname, "preferred") == 0)
			defelp = &preferredEl;
		else if (strcmp(defel->defname, "delimiter") == 0)
			defelp = &delimiterEl;
		else if (strcmp(defel->defname, "element") == 0)
			defelp = &elemTypeEl;
		else if (strcmp(defel->defname, "default") == 0)
			defelp = &defaultValueEl;
		else if (strcmp(defel->defname, "passedbyvalue") == 0)
			defelp = &byValueEl;
		else if (strcmp(defel->defname, "alignment") == 0)
			defelp = &alignmentEl;
		else if (strcmp(defel->defname, "storage") == 0)
			defelp = &storageEl;
		else if (strcmp(defel->defname, "collatable") == 0)
			defelp = &collatableEl;
		else
		{
			/* WARNING 而非 ERROR，为了保持历史向后兼容 */
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
			continue;
		}
		if (*defelp != NULL)
			errorConflictingDefElem(defel, pstate);
		*defelp = defel;
	}

	/*
	 * 现在解释选项；我们单独执行此操作，以便 LIKE 可  
	 * 被其他选项覆盖，而不受参数列表中顺序的影响。     
	 *      
	 */
	if (likeTypeEl)
	{
		Type		likeType;
		Form_pg_type likeForm;

		likeType = typenameType(NULL, defGetTypeName(likeTypeEl), NULL);
		likeForm = (Form_pg_type) GETSTRUCT(likeType);
		internalLength = likeForm->typlen;
		byValue = likeForm->typbyval;
		alignment = likeForm->typalign;
		storage = likeForm->typstorage;
		ReleaseSysCache(likeType);
	}
	if (internalLengthEl)
		internalLength = defGetTypeLength(internalLengthEl);
	if (inputNameEl)
		inputName = defGetQualifiedName(inputNameEl);
	if (outputNameEl)
		outputName = defGetQualifiedName(outputNameEl);
	if (receiveNameEl)
		receiveName = defGetQualifiedName(receiveNameEl);
	if (sendNameEl)
		sendName = defGetQualifiedName(sendNameEl);
	if (typmodinNameEl)
		typmodinName = defGetQualifiedName(typmodinNameEl);
	if (typmodoutNameEl)
		typmodoutName = defGetQualifiedName(typmodoutNameEl);
	if (analyzeNameEl)
		analyzeName = defGetQualifiedName(analyzeNameEl);
	if (subscriptNameEl)
		subscriptName = defGetQualifiedName(subscriptNameEl);
	if (categoryEl)
	{
		char	   *p = defGetString(categoryEl);

		category = p[0];
		/* 限制为非控制 ASCII 字符 */
		if (category < 32 || category > 126)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid type category \"%s\": must be simple ASCII",
							p)));
	}
	if (preferredEl)
		preferred = defGetBoolean(preferredEl);
	if (delimiterEl)
	{
		char	   *p = defGetString(delimiterEl);

		delimiter = p[0];
		/* XXX 我们不应该限制分隔符吗？ */
	}
	if (elemTypeEl)
	{
		elemType = typenameTypeId(NULL, defGetTypeName(elemTypeEl));
		/* 禁止伪类型的数组 */
		if (get_typtype(elemType) == TYPTYPE_PSEUDO)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("array element type cannot be %s",
							format_type_be(elemType))));
	}
	if (defaultValueEl)
		defaultValue = defGetString(defaultValueEl);
	if (byValueEl)
		byValue = defGetBoolean(byValueEl);
	if (alignmentEl)
	{
		char	   *a = defGetString(alignmentEl);

		/*
		 * 注意：如果参数是无引号标识符，解析器将     
		 * 对其应用翻译，因此请准备好识别翻译后的类型   
		 * 名称以及名义形式。            
		 */
		if (pg_strcasecmp(a, "double") == 0 ||
			pg_strcasecmp(a, "float8") == 0 ||
			pg_strcasecmp(a, "pg_catalog.float8") == 0)
			alignment = TYPALIGN_DOUBLE;
		else if (pg_strcasecmp(a, "int4") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.int4") == 0)
			alignment = TYPALIGN_INT;
		else if (pg_strcasecmp(a, "int2") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.int2") == 0)
			alignment = TYPALIGN_SHORT;
		else if (pg_strcasecmp(a, "char") == 0 ||
				 pg_strcasecmp(a, "pg_catalog.bpchar") == 0)
			alignment = TYPALIGN_CHAR;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("alignment \"%s\" not recognized", a)));
	}
	if (storageEl)
	{
		char	   *a = defGetString(storageEl);

		if (pg_strcasecmp(a, "plain") == 0)
			storage = TYPSTORAGE_PLAIN;
		else if (pg_strcasecmp(a, "external") == 0)
			storage = TYPSTORAGE_EXTERNAL;
		else if (pg_strcasecmp(a, "extended") == 0)
			storage = TYPSTORAGE_EXTENDED;
		else if (pg_strcasecmp(a, "main") == 0)
			storage = TYPSTORAGE_MAIN;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("storage \"%s\" not recognized", a)));
	}
	if (collatableEl)
		collation = defGetBoolean(collatableEl) ? DEFAULT_COLLATION_OID : InvalidOid;

	/*
	 * 确保我们有必需的定义            
	 */
	if (inputName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function must be specified")));
	if (outputName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function must be specified")));

	if (typmodinName == NIL && typmodoutName != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function is useless without a type modifier input function")));

	/* 将 I/O 过程名称转换为 OID
	 * 
	 */
	inputOid = findTypeInputFunction(inputName, typoid);
	outputOid = findTypeOutputFunction(outputName, typoid);
	if (receiveName)
		receiveOid = findTypeReceiveFunction(receiveName, typoid);
	if (sendName)
		sendOid = findTypeSendFunction(sendName, typoid);

	/* 将 typmodin/out 函数过程名称转换为 OID。
	 * 
	 */
	if (typmodinName)
		typmodinOid = findTypeTypmodinFunction(typmodinName);
	if (typmodoutName)
		typmodoutOid = findTypeTypmodoutFunction(typmodoutName);

	/* 将分析函数过程名称转换为 OID。如果未指定分析函数，
	 * 我们将使用零来选择内置默认算法。
	 * 
	 */
	if (analyzeName)
		analyzeOid = findTypeAnalyzeFunction(analyzeName, typoid);

	/*
	 * 同样，查找下标函数（如果存在）。若未指定    
	 * 下标函数，但指定了 typelem，且           
	 * raw_array_subscript_handler 可用，则允许。（这是为了
	 * 向后兼容；也许将来应改为报错。）               
	 */
	if (subscriptName)
		subscriptOid = findTypeSubscriptingFunction(subscriptName, typoid);
	else if (OidIsValid(elemType))
	{
		if (internalLength > 0 && !byValue && get_typlen(elemType) > 0)
			subscriptOid = F_RAW_ARRAY_SUBSCRIPT_HANDLER;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("element type cannot be specified without a subscripting function")));
	}

	/* 检查函数的权限。我们选择要求类型的创建者/拥有者也
	 * 拥有底层函数。由于创建类型相当于授予对这些函数的公共
	 * 执行权限，最低限度的合理检查是带授权选项的执行权限。
	 * 但我们没有办法在授权选项被撤销时使类型消失，
	 * 因此所有权检查更好。
	 * 
	 * 目前，鉴于上面的超级用户检查，这一切都是不必要的。
	 * 如果我们以后放宽此限制，这些调用可能应该移到
	 * findTypeInputFunction 等函数中，以便 AlterType 也能使用。
	 * 
	 * 
	 */
#ifdef NOT_USED
	if (inputOid && !object_ownercheck(ProcedureRelationId, inputOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(inputName));
	if (outputOid && !object_ownercheck(ProcedureRelationId, outputOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(outputName));
	if (receiveOid && !object_ownercheck(ProcedureRelationId, receiveOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(receiveName));
	if (sendOid && !object_ownercheck(ProcedureRelationId, sendOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(sendName));
	if (typmodinOid && !object_ownercheck(ProcedureRelationId, typmodinOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(typmodinName));
	if (typmodoutOid && !object_ownercheck(ProcedureRelationId, typmodoutOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(typmodoutName));
	if (analyzeOid && !object_ownercheck(ProcedureRelationId, analyzeOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(analyzeName));
	if (subscriptOid && !object_ownercheck(ProcedureRelationId, subscriptOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FUNCTION,
					   NameListToString(subscriptName));
#endif

	/*
	 * 好了，检查完毕，现在创建类型。我们必须在调用 
	 *  TypeCreate 之前分配数组类型 OID，因为基础类型和   
	 * 数组类型互相引用。        
	 */
	array_oid = AssignTypeArrayOid();

	/*
	 * 现在让 TypeCreate 做实际工作。  
	 *
	 * 注意：pg_type.oid 在用户表中作为数组元素（基础       
	 * 类型）存于 ArrayType，组合类型中存于 DatumTupleFields   
	 * 。oid 须在二进制升级中保留。 
	 */
	address =
		TypeCreate(InvalidOid,	/* 没有预定的类型 OID */
				   typeName,	/* 类型名称 */
				   typeNamespace,	/* 命名空间 */
				   InvalidOid,	/* 关系 oid（此处不适用） */
				   0,			/* 关系类型（不适用于此处） */
				   GetUserId(), /* 所有者的 ID */
				   internalLength,	/* 内部大小 */
				   TYPTYPE_BASE,	/* 类型-类型（基础类型） */
				   category,	/* 类型分类 */
				   preferred,	/* 是首选类型吗？ */
				   delimiter,	/* 数组元素分隔符 */
				   inputOid,	/* 输入过程 */
				   outputOid,	/* 输出过程 */
				   receiveOid,	/* 接收过程 */
				   sendOid,		/* 发送过程 */
				   typmodinOid, /* typmodin 过程 */
				   typmodoutOid,	/* typmodout 过程 */
				   analyzeOid,	/* 分析过程 */
				   subscriptOid,	/* 下标过程 */
				   elemType,	/* 元素类型 ID */
				   false,		/* 这不是隐式数组类型 */
				   array_oid,	/* 我们即将创建的数组类型 */
				   InvalidOid,	/* 基础类型 ID（仅用于域） */
				   defaultValue,	/* 默认类型值 */
				   NULL,		/* 没有可用的二进制形式 */
				   byValue,		/* 按值传递 */
				   alignment,	/* 所需的对齐方式 */
				   storage,		/* TOAST 策略 */
				   -1,			/* typMod（仅用于域） */
				   0,			/* typbasetype 的数组维度 */
				   false,		/* 类型 NOT NULL */
				   collation);	/* 类型的排序规则 */
	Assert(typoid == address.objectId);

	/*
	 * 创建与之配套的数组类型。    
	 */
	array_type = makeArrayTypeName(typeName, typeNamespace);

	/* 对于数组，对齐必须是 TYPALIGN_INT 或 TYPALIGN_DOUBLE */
	alignment = (alignment == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	TypeCreate(array_oid,		/* 强制分配此类型 OID */
			   array_type,		/* 类型名称 */
			   typeNamespace,	/* 命名空间 */
			   InvalidOid,		/* 关系 oid（此处不适用） */
			   0,				/* 关系类型（不适用于此处） */
			   GetUserId(),		/* 所有者的 ID */
			   -1,				/* 内部大小（始终为 varlena） */
			   TYPTYPE_BASE,	/* 类型-类型（基础类型） */
			   TYPCATEGORY_ARRAY,	/* 类型分类（数组） */
			   false,			/* 数组类型永远不是首选的 */
			   delimiter,		/* 数组元素分隔符 */
			   F_ARRAY_IN,		/* 输入过程 */
			   F_ARRAY_OUT,		/* 输出过程 */
			   F_ARRAY_RECV,	/* 接收过程 */
			   F_ARRAY_SEND,	/* 发送过程 */
			   typmodinOid,		/* typmodin 过程 */
			   typmodoutOid,	/* typmodout 过程 */
			   F_ARRAY_TYPANALYZE,	/* 分析过程 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* 数组下标过程 */
			   typoid,			/* 元素类型 ID */
			   true,			/* 是的，这是一个数组类型 */
			   InvalidOid,		/* 没有其他数组类型 */
			   InvalidOid,		/* 基础类型 ID */
			   NULL,			/* 永远没有默认类型值 */
			   NULL,			/* 二进制默认值也不发送 */
			   false,			/* 从不按值传递 */
			   alignment,		/* 参见上文 */
			   TYPSTORAGE_EXTENDED, /* ARRAY 始终可 toast */
			   -1,				/* typMod（仅用于域） */
			   0,				/* typbasetype 的数组维度 */
			   false,			/* 类型 NOT NULL */
			   collation);		/* 类型的排序规则 */

	pfree(array_type);

	return address;
}

/* 类型删除的核心逻辑。
 * 
 */
void
RemoveTypeById(Oid typeOid)
{
	Relation	relation;
	HeapTuple	tup;

	relation = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);

	CatalogTupleDelete(relation, &tup->t_self);

	/*
	 * 如果是枚举，还要删除 pg_enum 条目；我们没有为这些 
	 * 条目创建依赖项，所以必须在这里"手动"完成。     
	 *      
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_ENUM)
		EnumValuesDelete(typeOid);

	/*
	 * 如果是范围类型，还要删除 pg_range 条目；我们没有  
	 * 为它创建依赖项，所以必须在这里"手动"完成。         
	 *      
	 */
	if (((Form_pg_type) GETSTRUCT(tup))->typtype == TYPTYPE_RANGE)
		RangeDelete(typeOid);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}


/* 
 * DefineDomain
 *         注册一个新的域。
 */
ObjectAddress
DefineDomain(CreateDomainStmt *stmt)
{
	char	   *domainName;
	char	   *domainArrayName;
	Oid			domainNamespace;
	AclResult	aclresult;
	int16		internalLength;
	Oid			inputProcedure;
	Oid			outputProcedure;
	Oid			receiveProcedure;
	Oid			sendProcedure;
	Oid			analyzeProcedure;
	bool		byValue;
	char		category;
	char		delimiter;
	char		alignment;
	char		storage;
	char		typtype;
	Datum		datum;
	bool		isnull;
	char	   *defaultValue = NULL;
	char	   *defaultValueBin = NULL;
	bool		saw_default = false;
	bool		typNotNull = false;
	bool		nullDefined = false;
	int32		typNDims = list_length(stmt->typeName->arrayBounds);
	HeapTuple	typeTup;
	List	   *schema = stmt->constraints;
	ListCell   *listptr;
	Oid			basetypeoid;
	Oid			old_type_oid;
	Oid			domaincoll;
	Oid			domainArrayOid;
	Form_pg_type baseType;
	int32		basetypeMod;
	Oid			baseColl;
	ObjectAddress address;

	/* 将名称列表转换为名称和命名空间 */
	domainNamespace = QualifiedNameGetCreationNamespace(stmt->domainname,
														&domainName);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, domainNamespace, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(domainNamespace));

	/* 检查与现有类型名称的冲突。如果存在且是自动生成的数组，
	 * 我们可以将其重命名移开。
	 * 
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(domainName),
								   ObjectIdGetDatum(domainNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, domainName, domainNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", domainName)));
	}

	/*
	 * 查找基础类型。 
	 */
	typeTup = typenameType(NULL, stmt->typeName, &basetypeMod);
	baseType = (Form_pg_type) GETSTRUCT(typeTup);
	basetypeoid = baseType->oid;

	/* 基础类型必须是普通基础类型、组合类型、另一个域、
	 * 枚举或范围类型。基于伪类型的域会创建安全漏洞。
	 * （将其编码为仅检查伪类型会更短；但显式列出受支持
	 * 的具体 typtype 似乎更安全，而不是假设所有未来的
	 * typtype 都会被自动支持。）
	 * 
	 * 
	 */
	typtype = baseType->typtype;
	if (typtype != TYPTYPE_BASE &&
		typtype != TYPTYPE_COMPOSITE &&
		typtype != TYPTYPE_DOMAIN &&
		typtype != TYPTYPE_ENUM &&
		typtype != TYPTYPE_RANGE &&
		typtype != TYPTYPE_MULTIRANGE)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("\"%s\" is not a valid base type for a domain",
						TypeNameToString(stmt->typeName))));

	aclresult = object_aclcheck(TypeRelationId, basetypeoid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, basetypeoid);

	/* 收集新域的属性。部分从基础类型继承，
	 * 部分不继承。如果您更改了任何继承行为，
	 * 请务必更新 AlterTypeRecurse() 以匹配！
	 * 
	 */

	/* 识别排序规则（如果有的话）
	 * 
	 */
	baseColl = baseType->typcollation;
	if (stmt->collClause)
		domaincoll = get_collation_oid(stmt->collClause->collname, false);
	else
		domaincoll = baseColl;

	/* 如果对不可排序的类型应用 COLLATE，则报错 */
	if (OidIsValid(domaincoll) && !OidIsValid(baseColl))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("collations are not supported by type %s",
						format_type_be(basetypeoid))));

	/* 按值传递 */
	byValue = baseType->typbyval;

	/* 所需的对齐方式 */
	alignment = baseType->typalign;

	/* TOAST 策略 */
	storage = baseType->typstorage;

	/* 存储长度 */
	internalLength = baseType->typlen;

	/* 类型分类 */
	category = baseType->typcategory;

	/* 数组元素分隔符 */
	delimiter = baseType->typdelim;

	/* I/O 函数 */
	inputProcedure = F_DOMAIN_IN;
	outputProcedure = baseType->typoutput;
	receiveProcedure = F_DOMAIN_RECV;
	sendProcedure = baseType->typsend;

	/* 域不接受 typmod，因此不需要 typmodin/typmodout */

	/* 分析函数 */
	analyzeProcedure = baseType->typanalyze;

	/*
	 * 域不需要下标函数，因为它们本身不可下标  
	 * 访问。如果基础类型可下标访问，解析器将在       
	 * 下标访问前将类型规约为基础类型。                 
	 */

	/* 继承的默认值 */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefault, &isnull);
	if (!isnull)
		defaultValue = TextDatumGetCString(datum);

	/* 继承的默认二进制值 */
	datum = SysCacheGetAttr(TYPEOID, typeTup,
							Anum_pg_type_typdefaultbin, &isnull);
	if (!isnull)
		defaultValueBin = TextDatumGetCString(datum);

	/* 手动遍历约束，以避免 DefineRelation() 等执行的
	 * 额外处理。
	 * 
	 */
	foreach(listptr, schema)
	{
		Constraint *constr = lfirst(listptr);

		if (!IsA(constr, Constraint))
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(constr));
		switch (constr->contype)
		{
			case CONSTR_DEFAULT:

				/* 继承的默认值可以被用户通过 DEFAULT <expr> 子句
				 * 覆盖……但只能覆盖一次。
				 * 
				 */
				if (saw_default)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple default expressions")));
				saw_default = true;

				if (constr->raw_expr)
				{
					ParseState *pstate;
					Node	   *defaultExpr;

					/* 为 transformExpr 创建一个虚拟 ParseState */
					pstate = make_parsestate(NULL);

					/* 将 constr->raw_expr 编译成表达式。注意：
					 * 名称严格用于错误消息
					 * 
					 */
					defaultExpr = cookDefault(pstate, constr->raw_expr,
											  basetypeoid,
											  basetypeMod,
											  domainName,
											  0);

					/*
					 * 如果表达式只是 NULL 常量，我们将其视为
					 * 没有默认值。        
					 *
					 * 注意，如果基础类型是另一个域，我们会
					 * 在此处看到 CoerceToDomain 表达式，不会丢弃
					 * 默认值。这一点至关重要，因为域默认值 
					 * 需要被保留，以覆盖基础域可能具有的  
					 * 默认值  
					 */
					if (defaultExpr == NULL ||
						(IsA(defaultExpr, Const) &&
						 ((Const *) defaultExpr)->constisnull))
					{
						defaultValue = NULL;
						defaultValueBin = NULL;
					}
					else
					{
						/* 表达式必须以 nodeToString 结果的形式存储，
						 * 但我们也需要有效的文本表示（主要是为了让
						 * pg_dump 的工作更简单）。
						 * 
						 */
						defaultValue =
							deparse_expression(defaultExpr,
											   NIL, false, false);
						defaultValueBin = nodeToString(defaultExpr);
					}
				}
				else
				{
					/* 没有默认值（这种情况还会发生吗？） */
					defaultValue = NULL;
					defaultValueBin = NULL;
				}
				break;

			case CONSTR_NOTNULL:
				if (nullDefined && !typNotNull)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL constraints")));
				typNotNull = true;
				nullDefined = true;
				break;

			case CONSTR_NULL:
				if (nullDefined && typNotNull)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("conflicting NULL/NOT NULL constraints")));
				typNotNull = false;
				nullDefined = true;
				break;

			case CONSTR_CHECK:

				/*
				 * 检查约束在域创建之后处理，因为它们    
				 * 需要域的 Oid；此时我们只能检查它们未被 
				 * 标记为 NO INHERIT，因为那样会是无效的。      
				 *                
				 */
				if (constr->is_no_inherit)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("check constraints for domains cannot be marked NO INHERIT")));
				break;

				/* 所有其他情况都是错误情况
				 * 
				 */
			case CONSTR_UNIQUE:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unique constraints not possible for domains")));
				break;

			case CONSTR_PRIMARY:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("primary key constraints not possible for domains")));
				break;

			case CONSTR_EXCLUSION:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("exclusion constraints not possible for domains")));
				break;

			case CONSTR_FOREIGN:
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("foreign key constraints not possible for domains")));
				break;

			case CONSTR_ATTR_DEFERRABLE:
			case CONSTR_ATTR_NOT_DEFERRABLE:
			case CONSTR_ATTR_DEFERRED:
			case CONSTR_ATTR_IMMEDIATE:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("specifying constraint deferrability not supported for domains")));
				break;

			default:
				elog(ERROR, "unrecognized constraint subtype: %d",
					 (int) constr->contype);
				break;
		}
	}

	/* 为数组类型分配 OID */
	domainArrayOid = AssignTypeArrayOid();

	/* 让 TypeCreate 执行所有实际工作。
	 * 
	 */
	address =
		TypeCreate(InvalidOid,	/* 没有预定的类型 OID */
				   domainName,	/* 类型名称 */
				   domainNamespace, /* 命名空间 */
				   InvalidOid,	/* 关系 oid（此处不适用） */
				   0,			/* 关系类型（不适用于此处） */
				   GetUserId(), /* 所有者的 ID */
				   internalLength,	/* 内部大小 */
				   TYPTYPE_DOMAIN,	/* 类型-类型（域类型） */
				   category,	/* 类型分类 */
				   false,		/* 域类型永远不是首选的 */
				   delimiter,	/* 数组元素分隔符 */
				   inputProcedure,	/* 输入过程 */
				   outputProcedure, /* 输出过程 */
				   receiveProcedure,	/* 接收过程 */
				   sendProcedure,	/* 发送过程 */
				   InvalidOid,	/* typmodin 过程 - 无 */
				   InvalidOid,	/* typmodout 过程 - 无 */
				   analyzeProcedure,	/* 分析过程 */
				   InvalidOid,	/* 下标过程 - 无 */
				   InvalidOid,	/* 没有数组元素类型 */
				   false,		/* 这不是数组 */
				   domainArrayOid,	/* 我们即将创建的数组类型 */
				   basetypeoid, /* 基础类型 ID */
				   defaultValue,	/* 默认类型值（文本） */
				   defaultValueBin, /* 默认类型值（二进制） */
				   byValue,		/* 按值传递 */
				   alignment,	/* 所需的对齐方式 */
				   storage,		/* TOAST 策略 */
				   basetypeMod, /* typeMod 值 */
				   typNDims,	/* 基础类型的数组维度 */
				   typNotNull,	/* 类型 NOT NULL */
				   domaincoll); /* 类型的排序规则 */

	/*
	 * 创建与之配套的数组类型。    
	 */
	domainArrayName = makeArrayTypeName(domainName, domainNamespace);

	/* 对于数组，对齐必须是 TYPALIGN_INT 或 TYPALIGN_DOUBLE */
	alignment = (alignment == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	TypeCreate(domainArrayOid,	/* 强制分配此类型 OID */
			   domainArrayName, /* 类型名称 */
			   domainNamespace, /* 命名空间 */
			   InvalidOid,		/* 关系 oid（此处不适用） */
			   0,				/* 关系类型（不适用于此处） */
			   GetUserId(),		/* 所有者的 ID */
			   -1,				/* 内部大小（始终为 varlena） */
			   TYPTYPE_BASE,	/* 类型-类型（基础类型） */
			   TYPCATEGORY_ARRAY,	/* 类型分类（数组） */
			   false,			/* 数组类型永远不是首选的 */
			   delimiter,		/* 数组元素分隔符 */
			   F_ARRAY_IN,		/* 输入过程 */
			   F_ARRAY_OUT,		/* 输出过程 */
			   F_ARRAY_RECV,	/* 接收过程 */
			   F_ARRAY_SEND,	/* 发送过程 */
			   InvalidOid,		/* typmodin 过程 - 无 */
			   InvalidOid,		/* typmodout 过程 - 无 */
			   F_ARRAY_TYPANALYZE,	/* 分析过程 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* 数组下标过程 */
			   address.objectId,	/* 元素类型 ID */
			   true,			/* 是的，这是一个数组类型 */
			   InvalidOid,		/* 没有其他数组类型 */
			   InvalidOid,		/* 基础类型 ID */
			   NULL,			/* 永远没有默认类型值 */
			   NULL,			/* 二进制默认值也不发送 */
			   false,			/* 从不按值传递 */
			   alignment,		/* 参见上文 */
			   TYPSTORAGE_EXTENDED, /* ARRAY 始终可 toast */
			   -1,				/* typMod（仅用于域） */
			   0,				/* typbasetype 的数组维度 */
			   false,			/* 类型 NOT NULL */
			   domaincoll);		/* 类型的排序规则 */

	pfree(domainArrayName);

	/* 处理引用 TypeCreate 返回的域 ID 的约束
	 * 
	 */
	foreach(listptr, schema)
	{
		Constraint *constr = lfirst(listptr);

		/* 根据上面的检查，它必须是 Constraint */

		switch (constr->contype)
		{
			case CONSTR_CHECK:
				domainAddConstraint(address.objectId, domainNamespace,
									basetypeoid, basetypeMod,
									constr, domainName, NULL);
				break;

				/* 其他约束类型已在上面完全处理 */

			default:
				break;
		}

		/* 执行 CCI 以便检测重复的约束名称 */
		CommandCounterIncrement();
	}

	/* 现在可以清理了。
	 * 
	 */
	ReleaseSysCache(typeTup);

	return address;
}


/* 
 * DefineEnum
 *         注册一个新的枚举。
 */
ObjectAddress
DefineEnum(CreateEnumStmt *stmt)
{
	char	   *enumName;
	char	   *enumArrayName;
	Oid			enumNamespace;
	AclResult	aclresult;
	Oid			old_type_oid;
	Oid			enumArrayOid;
	ObjectAddress enumTypeAddr;

	/* 将名称列表转换为名称和命名空间 */
	enumNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &enumName);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, enumNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(enumNamespace));

	/* 检查与现有类型名称的冲突。如果存在且是自动生成的数组，
	 * 我们可以将其重命名移开。
	 * 
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(enumName),
								   ObjectIdGetDatum(enumNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, enumName, enumNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", enumName)));
	}

	/* 为数组类型分配 OID */
	enumArrayOid = AssignTypeArrayOid();

	/* 创建 pg_type 条目 */
	enumTypeAddr =
		TypeCreate(InvalidOid,	/* 没有预定的类型 OID */
				   enumName,	/* 类型名称 */
				   enumNamespace,	/* 命名空间 */
				   InvalidOid,	/* 关系 oid（此处不适用） */
				   0,			/* 关系类型（不适用于此处） */
				   GetUserId(), /* 所有者的 ID */
				   sizeof(Oid), /* 内部大小 */
				   TYPTYPE_ENUM,	/* 类型-类型（枚举类型） */
				   TYPCATEGORY_ENUM,	/* 类型分类（枚举类型） */
				   false,		/* 枚举类型永远不是首选的 */
				   DEFAULT_TYPDELIM,	/* 数组元素分隔符 */
				   F_ENUM_IN,	/* 输入过程 */
				   F_ENUM_OUT,	/* 输出过程 */
				   F_ENUM_RECV, /* 接收过程 */
				   F_ENUM_SEND, /* 发送过程 */
				   InvalidOid,	/* typmodin 过程 - 无 */
				   InvalidOid,	/* typmodout 过程 - 无 */
				   InvalidOid,	/* 分析过程 - 默认 */
				   InvalidOid,	/* 下标过程 - 无 */
				   InvalidOid,	/* 元素类型 ID */
				   false,		/* 这不是数组类型 */
				   enumArrayOid,	/* 我们即将创建的数组类型 */
				   InvalidOid,	/* 基础类型 ID（仅用于域） */
				   NULL,		/* 永远没有默认类型值 */
				   NULL,		/* 二进制默认值也不发送 */
				   true,		/* 始终按值传递 */
				   TYPALIGN_INT,	/* int 对齐 */
				   TYPSTORAGE_PLAIN,	/* TOAST 策略始终为 PLAIN */
				   -1,			/* typMod（仅用于域） */
				   0,			/* typbasetype 的数组维度 */
				   false,		/* 类型 NOT NULL */
				   InvalidOid); /* 类型的排序规则 */

	/* 将枚举值插入 pg_enum */
	EnumValuesCreate(enumTypeAddr.objectId, stmt->vals);

	/*
	 * 创建与之配套的数组类型。    
	 */
	enumArrayName = makeArrayTypeName(enumName, enumNamespace);

	TypeCreate(enumArrayOid,	/* 强制分配此类型 OID */
			   enumArrayName,	/* 类型名称 */
			   enumNamespace,	/* 命名空间 */
			   InvalidOid,		/* 关系 oid（此处不适用） */
			   0,				/* 关系类型（不适用于此处） */
			   GetUserId(),		/* 所有者的 ID */
			   -1,				/* 内部大小（始终为 varlena） */
			   TYPTYPE_BASE,	/* 类型-类型（基础类型） */
			   TYPCATEGORY_ARRAY,	/* 类型分类（数组） */
			   false,			/* 数组类型永远不是首选的 */
			   DEFAULT_TYPDELIM,	/* 数组元素分隔符 */
			   F_ARRAY_IN,		/* 输入过程 */
			   F_ARRAY_OUT,		/* 输出过程 */
			   F_ARRAY_RECV,	/* 接收过程 */
			   F_ARRAY_SEND,	/* 发送过程 */
			   InvalidOid,		/* typmodin 过程 - 无 */
			   InvalidOid,		/* typmodout 过程 - 无 */
			   F_ARRAY_TYPANALYZE,	/* 分析过程 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* 数组下标过程 */
			   enumTypeAddr.objectId,	/* 元素类型 ID */
			   true,			/* 是的，这是一个数组类型 */
			   InvalidOid,		/* 没有其他数组类型 */
			   InvalidOid,		/* 基础类型 ID */
			   NULL,			/* 永远没有默认类型值 */
			   NULL,			/* 二进制默认值也不发送 */
			   false,			/* 从不按值传递 */
			   TYPALIGN_INT,	/* 枚举是 int 对齐的，它们的数组也是如此 */
			   TYPSTORAGE_EXTENDED, /* ARRAY 始终可 toast */
			   -1,				/* typMod（仅用于域） */
			   0,				/* typbasetype 的数组维度 */
			   false,			/* 类型 NOT NULL */
			   InvalidOid);		/* 类型的排序规则 */

	pfree(enumArrayName);

	return enumTypeAddr;
}

/* 
 * AlterEnum
 *         向现有枚举添加新标签。
 */
ObjectAddress
AlterEnum(AlterEnumStmt *stmt)
{
	Oid			enum_type_oid;
	TypeName   *typename;
	HeapTuple	tup;
	ObjectAddress address;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(stmt->typeName);
	enum_type_oid = typenameTypeId(NULL, typename);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(enum_type_oid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", enum_type_oid);

	/* 检查它是否为枚举，并检查用户是否有 ALTER 该枚举的权限 */
	checkEnumOwner(tup);

	ReleaseSysCache(tup);

	if (stmt->oldVal)
	{
		/* 重命名现有标签 */
		RenameEnumLabel(enum_type_oid, stmt->oldVal, stmt->newVal);
	}
	else
	{
		/* 添加新标签 */
		AddEnumLabel(enum_type_oid, stmt->newVal,
					 stmt->newValNeighbor, stmt->newValIsAfter,
					 stmt->skipIfNewValExists);
	}

	InvokeObjectPostAlterHook(TypeRelationId, enum_type_oid, 0);

	ObjectAddressSet(address, TypeRelationId, enum_type_oid);

	return address;
}


/* 
 * checkEnumOwner
 * 
 * 检查类型是否确实为枚举，并且当前用户
 * 是否有对其执行 ALTER TYPE 的权限。如果有误则报错。
 */
static void
checkEnumOwner(HeapTuple tup)
{
	Form_pg_type typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 检查这确实是一个枚举 */
	if (typTup->typtype != TYPTYPE_ENUM)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not an enum",
						format_type_be(typTup->oid))));

	/* 权限检查：必须拥有类型 */
	if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);
}


/* 
 * DefineRange
 *         注册一个新的范围类型。
 * 
 * 也许将 pg_type.typelem 设置为基础类型是值得的，
 * 同样在多范围类型上将其设置为范围类型。但是，非零的
 * typelem 在其他地方被视为等同于数组，并且用户可能
 * 有使用相同假设的查询。
 */
ObjectAddress
DefineRange(ParseState *pstate, CreateRangeStmt *stmt)
{
	char	   *typeName;
	Oid			typeNamespace;
	Oid			typoid;
	char	   *rangeArrayName;
	char	   *multirangeTypeName = NULL;
	char	   *multirangeArrayName;
	Oid			multirangeNamespace = InvalidOid;
	Oid			rangeArrayOid;
	Oid			multirangeOid;
	Oid			multirangeArrayOid;
	Oid			rangeSubtype = InvalidOid;
	List	   *rangeSubOpclassName = NIL;
	List	   *rangeCollationName = NIL;
	List	   *rangeCanonicalName = NIL;
	List	   *rangeSubtypeDiffName = NIL;
	Oid			rangeSubOpclass;
	Oid			rangeCollation;
	regproc		rangeCanonical;
	regproc		rangeSubtypeDiff;
	int16		subtyplen;
	bool		subtypbyval;
	char		subtypalign;
	char		alignment;
	AclResult	aclresult;
	ListCell   *lc;
	ObjectAddress address;
	ObjectAddress mltrngaddress PG_USED_FOR_ASSERTS_ONLY;
	Oid			castFuncOid;

	/* 将名称列表转换为名称和命名空间 */
	typeNamespace = QualifiedNameGetCreationNamespace(stmt->typeName,
													  &typeName);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, typeNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(typeNamespace));

	/*
	 * 查看类型是否已存在。     
	 */
	typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
							 CStringGetDatum(typeName),
							 ObjectIdGetDatum(typeNamespace));

	/* 如果不是 shell，查看它是否是自动生成的数组类型，如果是，
	 * 则将其重命名移开。
	 * 
	 */
	if (OidIsValid(typoid) && get_typisdefined(typoid))
	{
		if (moveArrayTypeName(typoid, typeName, typeNamespace))
			typoid = InvalidOid;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));
	}

	/* 与 DefineType() 不同，我们不要求先存在 shell 类型，
	 * 因为仅当用户想要指定规范函数时才需要它。
	 * 
	 */

	/* 从参数列表中提取参数 */
	foreach(lc, stmt->params)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "subtype") == 0)
		{
			if (OidIsValid(rangeSubtype))
				errorConflictingDefElem(defel, pstate);
			/* 我们可以立即查找子类型名称 */
			rangeSubtype = typenameTypeId(NULL, defGetTypeName(defel));
		}
		else if (strcmp(defel->defname, "subtype_opclass") == 0)
		{
			if (rangeSubOpclassName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeSubOpclassName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "collation") == 0)
		{
			if (rangeCollationName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeCollationName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "canonical") == 0)
		{
			if (rangeCanonicalName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeCanonicalName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "subtype_diff") == 0)
		{
			if (rangeSubtypeDiffName != NIL)
				errorConflictingDefElem(defel, pstate);
			rangeSubtypeDiffName = defGetQualifiedName(defel);
		}
		else if (strcmp(defel->defname, "multirange_type_name") == 0)
		{
			if (multirangeTypeName != NULL)
				errorConflictingDefElem(defel, pstate);
			/* 我们可以立即查找子类型名称 */
			multirangeNamespace = QualifiedNameGetCreationNamespace(defGetQualifiedName(defel),
																	&multirangeTypeName);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* 必须有子类型 */
	if (!OidIsValid(rangeSubtype))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("type attribute \"subtype\" is required")));
	/* 禁止伪类型上的范围类型 */
	if (get_typtype(rangeSubtype) == TYPTYPE_PSEUDO)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("range subtype cannot be %s",
						format_type_be(rangeSubtype))));

	/* 识别子操作符类 */
	rangeSubOpclass = findRangeSubOpclass(rangeSubOpclassName, rangeSubtype);

	/* 识别要使用的排序规则（如果有的话） */
	if (type_is_collatable(rangeSubtype))
	{
		if (rangeCollationName != NIL)
			rangeCollation = get_collation_oid(rangeCollationName, false);
		else
			rangeCollation = get_typcollation(rangeSubtype);
	}
	else
	{
		if (rangeCollationName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("range collation specified but subtype does not support collation")));
		rangeCollation = InvalidOid;
	}

	/* 识别支持函数（如果提供了的话） */
	if (rangeCanonicalName != NIL)
	{
		if (!OidIsValid(typoid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot specify a canonical function without a pre-created shell type"),
					 errhint("Create the type as a shell type, then create its canonicalization function, then do a full CREATE TYPE.")));
		rangeCanonical = findRangeCanonicalFunction(rangeCanonicalName,
													typoid);
	}
	else
		rangeCanonical = InvalidOid;

	if (rangeSubtypeDiffName != NIL)
		rangeSubtypeDiff = findRangeSubtypeDiffFunction(rangeSubtypeDiffName,
														rangeSubtype);
	else
		rangeSubtypeDiff = InvalidOid;

	get_typlenbyvalalign(rangeSubtype,
						 &subtyplen, &subtypbyval, &subtypalign);

	/* 对于范围，对齐必须是 TYPALIGN_INT 或 TYPALIGN_DOUBLE */
	alignment = (subtypalign == TYPALIGN_DOUBLE) ? TYPALIGN_DOUBLE : TYPALIGN_INT;

	/* 为数组类型、多范围类型和多范围数组分配 OID */
	rangeArrayOid = AssignTypeArrayOid();
	multirangeOid = AssignTypeMultirangeOid();
	multirangeArrayOid = AssignTypeMultirangeArrayOid();

	/* 创建 pg_type 条目 */
	address =
		TypeCreate(InvalidOid,	/* 没有预定的类型 OID */
				   typeName,	/* 类型名称 */
				   typeNamespace,	/* 命名空间 */
				   InvalidOid,	/* 关系 oid（此处不适用） */
				   0,			/* 关系类型（不适用于此处） */
				   GetUserId(), /* 所有者的 ID */
				   -1,			/* 内部大小（始终为 varlena） */
				   TYPTYPE_RANGE,	/* 类型-类型（范围类型） */
				   TYPCATEGORY_RANGE,	/* 类型分类（范围类型） */
				   false,		/* 范围类型永远不是首选的 */
				   DEFAULT_TYPDELIM,	/* 数组元素分隔符 */
				   F_RANGE_IN,	/* 输入过程 */
				   F_RANGE_OUT, /* 输出过程 */
				   F_RANGE_RECV,	/* 接收过程 */
				   F_RANGE_SEND,	/* 发送过程 */
				   InvalidOid,	/* typmodin 过程 - 无 */
				   InvalidOid,	/* typmodout 过程 - 无 */
				   F_RANGE_TYPANALYZE,	/* 分析过程 */
				   InvalidOid,	/* 下标过程 - 无 */
				   InvalidOid,	/* 元素类型 ID - 无 */
				   false,		/* 这不是数组类型 */
				   rangeArrayOid,	/* 我们即将创建的数组类型 */
				   InvalidOid,	/* 基础类型 ID（仅用于域） */
				   NULL,		/* 永远没有默认类型值 */
				   NULL,		/* 也没有可用的二进制形式 */
				   false,		/* 从不按值传递 */
				   alignment,	/* 对齐方式 */
				   TYPSTORAGE_EXTENDED, /* TOAST 策略（始终为 EXTENDED） */
				   -1,			/* typMod（仅用于域） */
				   0,			/* typbasetype 的数组维度 */
				   false,		/* 类型 NOT NULL */
				   InvalidOid); /* 类型的排序规则（范围类型从不具有） */
	Assert(typoid == InvalidOid || typoid == address.objectId);
	typoid = address.objectId;

	/* 创建与之配套的多范围类型 */
	if (multirangeTypeName)
	{
		Oid			old_typoid;

		/*
		 * 查看多范围类型是否已存在。       
		 */
		old_typoid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
									 CStringGetDatum(multirangeTypeName),
									 ObjectIdGetDatum(multirangeNamespace));

		/* 如果不是 shell，查看它是否是自动生成的数组类型，
		 * 如果是则将其重命名移开。
		 * 
		 */
		if (OidIsValid(old_typoid) && get_typisdefined(old_typoid))
		{
			if (!moveArrayTypeName(old_typoid, multirangeTypeName, multirangeNamespace))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("type \"%s\" already exists", multirangeTypeName)));
		}
	}
	else
	{
		/* 自动生成多范围名称 */
		multirangeNamespace = typeNamespace;
		multirangeTypeName = makeMultirangeTypeName(typeName, multirangeNamespace);
	}

	mltrngaddress =
		TypeCreate(multirangeOid,	/* 强制分配此类型 OID */
				   multirangeTypeName,	/* 类型名称 */
				   multirangeNamespace, /* 命名空间 */
				   InvalidOid,	/* 关系 oid（此处不适用） */
				   0,			/* 关系类型（不适用于此处） */
				   GetUserId(), /* 所有者的 ID */
				   -1,			/* 内部大小（始终为 varlena） */
				   TYPTYPE_MULTIRANGE,	/* 类型-类型（多范围类型） */
				   TYPCATEGORY_RANGE,	/* 类型分类（范围类型） */
				   false,		/* 多范围类型永远不是首选的 */
				   DEFAULT_TYPDELIM,	/* 数组元素分隔符 */
				   F_MULTIRANGE_IN, /* 输入过程 */
				   F_MULTIRANGE_OUT,	/* 输出过程 */
				   F_MULTIRANGE_RECV,	/* 接收过程 */
				   F_MULTIRANGE_SEND,	/* 发送过程 */
				   InvalidOid,	/* typmodin 过程 - 无 */
				   InvalidOid,	/* typmodout 过程 - 无 */
				   F_MULTIRANGE_TYPANALYZE, /* 分析过程 */
				   InvalidOid,	/* 下标过程 - 无 */
				   InvalidOid,	/* 元素类型 ID - 无 */
				   false,		/* 这不是数组类型 */
				   multirangeArrayOid,	/* 我们即将创建的数组类型 */
				   InvalidOid,	/* 基础类型 ID（仅用于域） */
				   NULL,		/* 永远没有默认类型值 */
				   NULL,		/* 也没有可用的二进制形式 */
				   false,		/* 从不按值传递 */
				   alignment,	/* 对齐方式 */
				   'x',			/* TOAST 策略（始终为 EXTENDED） */
				   -1,			/* typMod（仅用于域） */
				   0,			/* typbasetype 的数组维度 */
				   false,		/* 类型 NOT NULL */
				   InvalidOid); /* 类型的排序规则（范围类型从不具有） */
	Assert(multirangeOid == mltrngaddress.objectId);

	/* 在 pg_range 中创建条目 */
	RangeCreate(typoid, rangeSubtype, rangeCollation, rangeSubOpclass,
				rangeCanonical, rangeSubtypeDiff, multirangeOid);

	/*
	 * 创建与之配套的数组类型。    
	 */
	rangeArrayName = makeArrayTypeName(typeName, typeNamespace);

	TypeCreate(rangeArrayOid,	/* 强制分配此类型 OID */
			   rangeArrayName,	/* 类型名称 */
			   typeNamespace,	/* 命名空间 */
			   InvalidOid,		/* 关系 oid（此处不适用） */
			   0,				/* 关系类型（不适用于此处） */
			   GetUserId(),		/* 所有者的 ID */
			   -1,				/* 内部大小（始终为 varlena） */
			   TYPTYPE_BASE,	/* 类型-类型（基础类型） */
			   TYPCATEGORY_ARRAY,	/* 类型分类（数组） */
			   false,			/* 数组类型永远不是首选的 */
			   DEFAULT_TYPDELIM,	/* 数组元素分隔符 */
			   F_ARRAY_IN,		/* 输入过程 */
			   F_ARRAY_OUT,		/* 输出过程 */
			   F_ARRAY_RECV,	/* 接收过程 */
			   F_ARRAY_SEND,	/* 发送过程 */
			   InvalidOid,		/* typmodin 过程 - 无 */
			   InvalidOid,		/* typmodout 过程 - 无 */
			   F_ARRAY_TYPANALYZE,	/* 分析过程 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* 数组下标过程 */
			   typoid,			/* 元素类型 ID */
			   true,			/* 是的，这是一个数组类型 */
			   InvalidOid,		/* 没有其他数组类型 */
			   InvalidOid,		/* 基础类型 ID */
			   NULL,			/* 永远没有默认类型值 */
			   NULL,			/* 二进制默认值也不发送 */
			   false,			/* 从不按值传递 */
			   alignment,		/* 对齐方式 - 与范围相同 */
			   TYPSTORAGE_EXTENDED, /* ARRAY 始终可 toast */
			   -1,				/* typMod（仅用于域） */
			   0,				/* typbasetype 的数组维度 */
			   false,			/* 类型 NOT NULL */
			   InvalidOid);		/* 类型排序规则 */

	pfree(rangeArrayName);

	/* 创建多范围的数组类型 */

	multirangeArrayName = makeArrayTypeName(multirangeTypeName, typeNamespace);

	TypeCreate(multirangeArrayOid,	/* 强制分配此类型 OID */
			   multirangeArrayName, /* 类型名称 */
			   multirangeNamespace, /* 命名空间 */
			   InvalidOid,		/* 关系 oid（此处不适用） */
			   0,				/* 关系类型（不适用于此处） */
			   GetUserId(),		/* 所有者的 ID */
			   -1,				/* 内部大小（始终为 varlena） */
			   TYPTYPE_BASE,	/* 类型-类型（基础类型） */
			   TYPCATEGORY_ARRAY,	/* 类型分类（数组） */
			   false,			/* 数组类型永远不是首选的 */
			   DEFAULT_TYPDELIM,	/* 数组元素分隔符 */
			   F_ARRAY_IN,		/* 输入过程 */
			   F_ARRAY_OUT,		/* 输出过程 */
			   F_ARRAY_RECV,	/* 接收过程 */
			   F_ARRAY_SEND,	/* 发送过程 */
			   InvalidOid,		/* typmodin 过程 - 无 */
			   InvalidOid,		/* typmodout 过程 - 无 */
			   F_ARRAY_TYPANALYZE,	/* 分析过程 */
			   F_ARRAY_SUBSCRIPT_HANDLER,	/* 数组下标过程 */
			   multirangeOid,	/* 元素类型 ID */
			   true,			/* 是的，这是一个数组类型 */
			   InvalidOid,		/* 没有其他数组类型 */
			   InvalidOid,		/* 基础类型 ID */
			   NULL,			/* 永远没有默认类型值 */
			   NULL,			/* 二进制默认值也不发送 */
			   false,			/* 从不按值传递 */
			   alignment,		/* 对齐方式 - 与范围相同 */
			   'x',				/* ARRAY 始终可 toast */
			   -1,				/* typMod（仅用于域） */
			   0,				/* typbasetype 的数组维度 */
			   false,			/* 类型 NOT NULL */
			   InvalidOid);		/* 类型排序规则 */

	/* 为此范围类型创建构造函数 */
	makeRangeConstructors(typeName, typeNamespace, typoid, rangeSubtype);
	makeMultirangeConstructors(multirangeTypeName, typeNamespace,
							   multirangeOid, typoid, rangeArrayOid,
							   &castFuncOid);

	/* 创建范围类型到多范围类型的类型转换 */
	CastCreate(typoid, multirangeOid, castFuncOid, InvalidOid, InvalidOid,
			   COERCION_CODE_EXPLICIT, COERCION_METHOD_FUNCTION,
			   DEPENDENCY_INTERNAL);

	pfree(multirangeArrayName);

	return address;
}

/*
 * 由于同一子类型上可能存在多个范围类型，范围       
 * 类型无法由子类型唯一确定。因此无法定义         
 * 多态构造函数；我们必须为每个范围类型显式生成     
 * 新的构造函数。                                
 *
 * 我们实际定义了 4 个函数，参数从 0 到 3。这只是        
 * 为了给用户提供更多便利。   
 */
static void
makeRangeConstructors(const char *name, Oid namespace,
					  Oid rangeOid, Oid subtype)
{
	static const char *const prosrc[2] = {"range_constructor2",
	"range_constructor3"};
	static const int pronargs[2] = {2, 3};

	Oid			constructorArgTypes[3];
	ObjectAddress myself,
				referenced;
	int			i;

	constructorArgTypes[0] = subtype;
	constructorArgTypes[1] = subtype;
	constructorArgTypes[2] = TEXTOID;

	referenced.classId = TypeRelationId;
	referenced.objectId = rangeOid;
	referenced.objectSubId = 0;

	for (i = 0; i < lengthof(prosrc); i++)
	{
		oidvector  *constructorArgTypesVector;

		constructorArgTypesVector = buildoidvector(constructorArgTypes,
												   pronargs[i]);

		myself = ProcedureCreate(name,	/* 名称：与范围类型相同 */
								 namespace, /* 命名空间 */
								 false, /* 替换 */
								 false, /* 返回集合 */
								 rangeOid,	/* 返回类型 */
								 BOOTSTRAP_SUPERUSERID, /* 过程所有者 */
								 INTERNALlanguageId,	/* 语言 */
								 F_FMGR_INTERNAL_VALIDATOR, /* 语言验证器 */
								 prosrc[i], /* 过程源 */
								 NULL,	/* 过程二进制 */
								 NULL,	/* 过程 SQL 体 */
								 PROKIND_FUNCTION,
								 false, /* security_definer */
								 false, /* 防泄漏 */
								 false, /* 是严格的 */
								 PROVOLATILE_IMMUTABLE, /* 易变性 */
								 PROPARALLEL_SAFE,	/* 并行安全性 */
								 constructorArgTypesVector, /* 参数类型 */
								 PointerGetDatum(NULL), /* 所有参数类型 */
								 PointerGetDatum(NULL), /* 参数模式 */
								 PointerGetDatum(NULL), /* 参数名称 */
								 NIL,	/* 参数默认值 */
								 PointerGetDatum(NULL), /* 转换类型 */
								 PointerGetDatum(NULL), /* 过程配置 */
								 InvalidOid,	/* 过程支持 */
								 1.0,	/* 过程成本 */
								 0.0);	/* 过程行数 */

		/*
		 * 使构造函数内部依赖于范围类型，以便在删除   
		 * 类型时它们会静默消失。注意，pg_dump              
		 * 依赖于此选择，以避免转储构造函数。              
		 */
		recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	}
}

/* 我们为每个范围类型创建单独的多范围构造函数，
 * 这样其名称就可以像范围构造函数一样包含基础类型。
 * 如果我们有 anyrangearray 多态类型，可以在这里使用它，
 * 但由于每个类型都有自己的构造函数名称，所以没有必要。
 * 
 * 将 castFuncOid 设置为可用于从范围到多范围
 * 进行类型转换的新构造函数的 oid。
 * 
 */
static void
makeMultirangeConstructors(const char *name, Oid namespace,
						   Oid multirangeOid, Oid rangeOid, Oid rangeArrayOid,
						   Oid *castFuncOid)
{
	ObjectAddress myself,
				referenced;
	oidvector  *argtypes;
	Datum		allParamTypes;
	ArrayType  *allParameterTypes;
	Datum		paramModes;
	ArrayType  *parameterModes;

	referenced.classId = TypeRelationId;
	referenced.objectId = multirangeOid;
	referenced.objectSubId = 0;

	/* 0 参数构造函数 - 用于空多范围 */
	argtypes = buildoidvector(NULL, 0);
	myself = ProcedureCreate(name,	/* 名称：与多范围类型相同 */
							 namespace,
							 false, /* 替换 */
							 false, /* 返回集合 */
							 multirangeOid, /* 返回类型 */
							 BOOTSTRAP_SUPERUSERID, /* 过程所有者 */
							 INTERNALlanguageId,	/* 语言 */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor0", /* 过程源 */
							 NULL,	/* 过程二进制 */
							 NULL,	/* 过程 SQL 体 */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* 防泄漏 */
							 true,	/* 是严格的 */
							 PROVOLATILE_IMMUTABLE, /* 易变性 */
							 PROPARALLEL_SAFE,	/* 并行安全性 */
							 argtypes,	/* 参数类型 */
							 PointerGetDatum(NULL), /* 所有参数类型 */
							 PointerGetDatum(NULL), /* 参数模式 */
							 PointerGetDatum(NULL), /* 参数名称 */
							 NIL,	/* 参数默认值 */
							 PointerGetDatum(NULL), /* 转换类型 */
							 PointerGetDatum(NULL), /* 过程配置 */
							 InvalidOid,	/* 过程支持 */
							 1.0,	/* 过程成本 */
							 0.0);	/* 过程行数 */

	/*
	 * 使构造函数内部依赖于多范围类型，以便在删除    
	 * 类型时它们会静默消失。注意，pg_dump 依赖于            
	 * 此选择，以避免转储构造函数。               
	 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);

	/*
	 * 1参构造函数用于转换 
	 *
	 * 理论上，这个和可变参数（n 参数）构造函数 
	 * 不应该都要。但单独的 1 参函数使我们能对它定义    
	 * 转换。  
	 */
	argtypes = buildoidvector(&rangeOid, 1);
	myself = ProcedureCreate(name,	/* 名称：与多范围类型相同 */
							 namespace,
							 false, /* 替换 */
							 false, /* 返回集合 */
							 multirangeOid, /* 返回类型 */
							 BOOTSTRAP_SUPERUSERID, /* 过程所有者 */
							 INTERNALlanguageId,	/* 语言 */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor1", /* 过程源 */
							 NULL,	/* 过程二进制 */
							 NULL,	/* 过程 SQL 体 */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* 防泄漏 */
							 true,	/* 是严格的 */
							 PROVOLATILE_IMMUTABLE, /* 易变性 */
							 PROPARALLEL_SAFE,	/* 并行安全性 */
							 argtypes,	/* 参数类型 */
							 PointerGetDatum(NULL), /* 所有参数类型 */
							 PointerGetDatum(NULL), /* 参数模式 */
							 PointerGetDatum(NULL), /* 参数名称 */
							 NIL,	/* 参数默认值 */
							 PointerGetDatum(NULL), /* 转换类型 */
							 PointerGetDatum(NULL), /* 过程配置 */
							 InvalidOid,	/* 过程支持 */
							 1.0,	/* 过程成本 */
							 0.0);	/* 过程行数 */
	/* 同上 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	*castFuncOid = myself.objectId;

	/* n 参数构造函数 - 可变参数 */
	argtypes = buildoidvector(&rangeArrayOid, 1);
	allParamTypes = ObjectIdGetDatum(rangeArrayOid);
	allParameterTypes = construct_array_builtin(&allParamTypes, 1, OIDOID);
	paramModes = CharGetDatum(FUNC_PARAM_VARIADIC);
	parameterModes = construct_array_builtin(&paramModes, 1, CHAROID);
	myself = ProcedureCreate(name,	/* 名称：与多范围类型相同 */
							 namespace,
							 false, /* 替换 */
							 false, /* 返回集合 */
							 multirangeOid, /* 返回类型 */
							 BOOTSTRAP_SUPERUSERID, /* 过程所有者 */
							 INTERNALlanguageId,	/* 语言 */
							 F_FMGR_INTERNAL_VALIDATOR,
							 "multirange_constructor2", /* 过程源 */
							 NULL,	/* 过程二进制 */
							 NULL,	/* 过程 SQL 体 */
							 PROKIND_FUNCTION,
							 false, /* security_definer */
							 false, /* 防泄漏 */
							 true,	/* 是严格的 */
							 PROVOLATILE_IMMUTABLE, /* 易变性 */
							 PROPARALLEL_SAFE,	/* 并行安全性 */
							 argtypes,	/* 参数类型 */
							 PointerGetDatum(allParameterTypes),	/* 所有参数类型 */
							 PointerGetDatum(parameterModes),	/* 参数模式 */
							 PointerGetDatum(NULL), /* 参数名称 */
							 NIL,	/* 参数默认值 */
							 PointerGetDatum(NULL), /* 转换类型 */
							 PointerGetDatum(NULL), /* 过程配置 */
							 InvalidOid,	/* 过程支持 */
							 1.0,	/* 过程成本 */
							 0.0);	/* 过程行数 */
	/* 同上 */
	recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
	pfree(argtypes);
	pfree(allParameterTypes);
	pfree(parameterModes);
}

/*
 * 为类型查找合适的 I/O 和其他支持函数。    
 *
 * typeOid 是类型的 OID（即使仅是 shell 类型也已存在）。 
 *       
 */

static Oid
findTypeInputFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/* 输入函数可以接受一个 CSTRING 类型的参数，或三个参数
	 * （string、typioparam OID、typmod）。
	 * 如果两种形式都存在，则报歧义错误。
	 * 
	 */
	argList[0] = CSTRINGOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type input function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* 如果未找到，在错误消息中引用 1 参数签名 */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* 输入函数必须返回目标类型。 */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/* 如果任何 I/O 函数被标记为 volatile，则打印警告。
	 * 普遍假设 I/O 函数是 stable 或 immutable 的；
	 * 这使我们例如可以将 record_in/record_out 标记为
	 * stable 而不是 volatile。理想情况下，我们应该在此处抛出
	 * 错误而不是警告；但鉴于此检查是 9.5 中新增的，并且
	 * volatility 标记可能只是遗漏导致，而非函数行为的真实
	 * 指示，我们暂时让它以警告形式通过。
	 * 
	 * 
	 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeOutputFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * 输出函数始终接受一个该类型的参数并返回            
	 * cstring.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* 目前只是警告，参见 findTypeInputFunction 中的注释 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeReceiveFunction(List *procname, Oid typeOid)
{
	Oid			argList[3];
	Oid			procOid;
	Oid			procOid2;

	/* 接收函数可以接受一个 INTERNAL 类型的参数，或三个参数
	 * （internal、typioparam OID、typmod）。
	 * 如果两种形式都存在，则报歧义错误。
	 * 
	 */
	argList[0] = INTERNALOID;
	argList[1] = OIDOID;
	argList[2] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	procOid2 = LookupFuncName(procname, 3, argList, true);
	if (OidIsValid(procOid))
	{
		if (OidIsValid(procOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("type receive function %s has multiple matches",
							NameListToString(procname))));
	}
	else
	{
		procOid = procOid2;
		/* 如果未找到，在错误消息中引用 1 参数签名 */
		if (!OidIsValid(procOid))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("function %s does not exist",
							func_signature_string(procname, 1, NIL, argList))));
	}

	/* 接收函数必须返回目标类型。 */
	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s must return type %s",
						NameListToString(procname), format_type_be(typeOid))));

	/* 目前只是警告，参见 findTypeInputFunction 中的注释 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type receive function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeSendFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * 发送函数始终接受一个该类型的参数并返回          
	 * bytea.
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s must return type %s",
						NameListToString(procname), "bytea")));

	/* 目前只是警告，参见 findTypeInputFunction 中的注释 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type send function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeTypmodinFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/* typmodin 函数始终接受一个 cstring[] 参数并返回 int4。
	 * 
	 */
	argList[0] = CSTRINGARRAYOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INT4OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_in function %s must return type %s",
						NameListToString(procname), "integer")));

	/* 目前只是警告，参见 findTypeInputFunction 中的注释 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier input function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeTypmodoutFunction(List *procname)
{
	Oid			argList[1];
	Oid			procOid;

	/* typmodout 函数始终接受一个 int4 参数并返回 cstring。
	 * 
	 */
	argList[0] = INT4OID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != CSTRINGOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_out function %s must return type %s",
						NameListToString(procname), "cstring")));

	/* 目前只是警告，参见 findTypeInputFunction 中的注释 */
	if (func_volatile(procOid) == PROVOLATILE_VOLATILE)
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type modifier output function %s should not be volatile",
						NameListToString(procname))));

	return procOid;
}

static Oid
findTypeAnalyzeFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/* 分析函数始终接受一个 INTERNAL 参数并返回 bool。
	 * 
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type analyze function %s must return type %s",
						NameListToString(procname), "boolean")));

	return procOid;
}

static Oid
findTypeSubscriptingFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;

	/*
	 * 下标支持函数始终接受一个 INTERNAL 参数并返回       
	 *  INTERNAL。（参数未被使用，但必须有它以维护       
	 * 类型安全。）    
	 */
	argList[0] = INTERNALOID;

	procOid = LookupFuncName(procname, 1, argList, true);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != INTERNALOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("type subscripting function %s must return type %s",
						NameListToString(procname), "internal")));

	/* 我们禁止显式选择 array_subscript_handler()，
	 * 因为它只能应用于自动生成的数组类型。
	 * 
	 */
	if (procOid == F_ARRAY_SUBSCRIPT_HANDLER)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("user-defined types cannot use subscripting function %s",
						NameListToString(procname))));

	return procOid;
}

/* 为范围类型查找合适的支持函数和操作符类。
 * 
 */

/* 查找子类型的命名 btree 操作符类，如果 opcname 为 NIL，
 * 则查找默认 btree 操作符类。
 * 
 */
static Oid
findRangeSubOpclass(List *opcname, Oid subtype)
{
	Oid			opcid;
	Oid			opInputType;

	if (opcname != NIL)
	{
		opcid = get_opclass_oid(BTREE_AM_OID, opcname, false);

		/* 验证操作符类接受此数据类型。注意，我们将接受
		 * 二进制兼容性。
		 * 
		 */
		opInputType = get_opclass_input_type(opcid);
		if (!IsBinaryCoercible(subtype, opInputType))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("operator class \"%s\" does not accept data type %s",
							NameListToString(opcname),
							format_type_be(subtype))));
	}
	else
	{
		opcid = GetDefaultOpClass(subtype, BTREE_AM_OID);
		if (!OidIsValid(opcid))
		{
			/* 我们将错误消息拼写为与 ResolveOpClass 完全相同 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("data type %s has no default operator class for access method \"%s\"",
							format_type_be(subtype), "btree"),
					 errhint("You must specify an operator class for the range type or define a default operator class for the subtype.")));
		}
	}

	return opcid;
}

static Oid
findRangeCanonicalFunction(List *procname, Oid typeOid)
{
	Oid			argList[1];
	Oid			procOid;
	AclResult	aclresult;

	/*
	 * 范围规范函数必须接受并返回范围类型，并且必须是  
	 * immutable。 
	 */
	argList[0] = typeOid;

	procOid = LookupFuncName(procname, 1, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 1, NIL, argList))));

	if (get_func_rettype(procOid) != typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must return range type",
						func_signature_string(procname, 1, NIL, argList))));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range canonical function %s must be immutable",
						func_signature_string(procname, 1, NIL, argList))));

	/* 此外，范围类型的创建者必须有权调用该函数 */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(procOid));

	return procOid;
}

static Oid
findRangeSubtypeDiffFunction(List *procname, Oid subtype)
{
	Oid			argList[2];
	Oid			procOid;
	AclResult	aclresult;

	/* 范围子类型 diff 函数必须接受两个子类型参数，
	 * 必须返回 float8，并且必须是 immutable 的。
	 * 
	 */
	argList[0] = subtype;
	argList[1] = subtype;

	procOid = LookupFuncName(procname, 2, argList, true);

	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(procname, 2, NIL, argList))));

	if (get_func_rettype(procOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must return type %s",
						func_signature_string(procname, 2, NIL, argList),
						"double precision")));

	if (func_volatile(procOid) != PROVOLATILE_IMMUTABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("range subtype diff function %s must be immutable",
						func_signature_string(procname, 2, NIL, argList))));

	/* 此外，范围类型的创建者必须有权调用该函数 */
	aclresult = object_aclcheck(ProcedureRelationId, procOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(procOid));

	return procOid;
}

/* 
 *     AssignTypeArrayOid
 * 
 *     预分配类型的数组 OID 以用于 pg_type.typarray
 */
Oid
AssignTypeArrayOid(void)
{
	Oid			type_array_oid;

	/* 对 pg_type.typarray 使用 binary-upgrade 覆盖？ */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_array_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type array OID value not set when in binary upgrade mode")));

		type_array_oid = binary_upgrade_next_array_pg_type_oid;
		binary_upgrade_next_array_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
											Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_array_oid;
}

/* 
 *     AssignTypeMultirangeOid
 * 
 *     预分配范围类型的多范围 OID 以用于 pg_type.oid
 */
Oid
AssignTypeMultirangeOid(void)
{
	Oid			type_multirange_oid;

	/* 对 pg_type.oid 使用 binary-upgrade 覆盖？ */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_mrng_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type multirange OID value not set when in binary upgrade mode")));

		type_multirange_oid = binary_upgrade_next_mrng_pg_type_oid;
		binary_upgrade_next_mrng_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
												 Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_oid;
}

/* 
 *     AssignTypeMultirangeArrayOid
 * 
 *     预分配范围类型的多范围数组 OID 以用于 pg_type.typarray
 */
Oid
AssignTypeMultirangeArrayOid(void)
{
	Oid			type_multirange_array_oid;

	/* 对 pg_type.oid 使用 binary-upgrade 覆盖？ */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_mrng_array_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type multirange array OID value not set when in binary upgrade mode")));

		type_multirange_array_oid = binary_upgrade_next_mrng_array_pg_type_oid;
		binary_upgrade_next_mrng_array_pg_type_oid = InvalidOid;
	}
	else
	{
		Relation	pg_type = table_open(TypeRelationId, AccessShareLock);

		type_multirange_array_oid = GetNewOidWithIndex(pg_type, TypeOidIndexId,
													   Anum_pg_type_oid);
		table_close(pg_type, AccessShareLock);
	}

	return type_multirange_array_oid;
}


/* -------------------------------------------------------------------
 *  DefineCompositeType
 * 
 *  创建组合类型关系。
 *  `DefineRelation' 执行所有工作，我们只需提供正确的
 *  参数！
 * 
 *  如果关系已存在，则 'DefineRelation' 将中止
 *  事务……
 * 
 *  返回类型是新类型的对象地址。
 * -------------------------------------------------------------------
 */
ObjectAddress
DefineCompositeType(RangeVar *typevar, List *coldeflist)
{
	CreateStmt *createStmt = makeNode(CreateStmt);
	Oid			old_type_oid;
	Oid			typeNamespace;
	ObjectAddress address;

	/* 现在设置键/继承等参数。所有这些
	 * 对组合类型来说都无关紧要……
	 * 
	 */
	createStmt->relation = typevar;
	createStmt->tableElts = coldeflist;
	createStmt->inhRelations = NIL;
	createStmt->constraints = NIL;
	createStmt->options = NIL;
	createStmt->oncommit = ONCOMMIT_NOOP;
	createStmt->tablespacename = NULL;
	createStmt->if_not_exists = false;

	/* 检查与现有类型名称的冲突。如果存在且是自动生成的数组，
	 * 我们可以将其重命名移开。此检查主要在这里进行，
	 * 是为了在发生冲突时给出更好的关于"类型"的错误消息，
	 * 而不是下面关于"关系"的错误消息。
	 * 
	 */
	typeNamespace = RangeVarGetAndCheckCreationNamespace(createStmt->relation,
														 NoLock, NULL);
	RangeVarAdjustRelationPersistence(createStmt->relation, typeNamespace);
	old_type_oid =
		GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
						CStringGetDatum(createStmt->relation->relname),
						ObjectIdGetDatum(typeNamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, createStmt->relation->relname, typeNamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", createStmt->relation->relname)));
	}

	/*
	 * 最后创建关系。这也会创建类型。            
	 */
	DefineRelation(createStmt, RELKIND_COMPOSITE_TYPE, InvalidOid, &address,
				   NULL);

	return address;
}

/* 
 * AlterDomainDefault
 * 
 * 实现 ALTER DOMAIN SET/DROP DEFAULT 语句的例程。
 * 
 * 返回修改后的域的 ObjectAddress。
 */
ObjectAddress
AlterDomainDefault(List *names, Node *defaultRaw)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	ParseState *pstate;
	Relation	rel;
	char	   *defaultValue;
	Node	   *defaultExpr = NULL; /* 如果未指定默认值则为 NULL */
	Datum		new_record[Natts_pg_type] = {0};
	bool		new_record_nulls[Natts_pg_type] = {0};
	bool		new_record_repl[Natts_pg_type] = {0};
	HeapTuple	newtuple;
	Form_pg_type typTup;
	ObjectAddress address;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* 在类型表中查找域 */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 检查它是否为域，并检查用户是否有 ALTER DOMAIN 的权限 */
	checkDomainOwner(tup);

	/* 设置新元组 */

	/* 将新的默认值存储到元组中 */
	if (defaultRaw)
	{
		/* 为 transformExpr 创建一个虚拟 ParseState */
		pstate = make_parsestate(NULL);

		/* 将 colDef->raw_expr 编译成表达式。注意：名称
		 * 严格用于错误消息
		 * 
		 */
		defaultExpr = cookDefault(pstate, defaultRaw,
								  typTup->typbasetype,
								  typTup->typtypmod,
								  NameStr(typTup->typname),
								  0);

		/*
		 * 如果表达式只是 NULL 常量，我们将此命令视为   
		 *  ALTER ... DROP DEFAULT。（但参见 DefineDomain 中的  
		 * 注释。）  
		 */
		if (defaultExpr == NULL ||
			(IsA(defaultExpr, Const) && ((Const *) defaultExpr)->constisnull))
		{
			/* 默认值是 NULL，删除它 */
			defaultExpr = NULL;
			new_record_nulls[Anum_pg_type_typdefaultbin - 1] = true;
			new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
			new_record_nulls[Anum_pg_type_typdefault - 1] = true;
			new_record_repl[Anum_pg_type_typdefault - 1] = true;
		}
		else
		{
			/*
			 * 表达式必须以 nodeToString 结果存储，但我们也    
			 * 需要有效的文本表示（主要是为了让 pg_dump   
			 * 更简单）。     
			 */
			defaultValue = deparse_expression(defaultExpr,
											  NIL, false, false);

			/* 形成带有新默认值的更新元组并写回。
			 * 
			 */
			new_record[Anum_pg_type_typdefaultbin - 1] = CStringGetTextDatum(nodeToString(defaultExpr));

			new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
			new_record[Anum_pg_type_typdefault - 1] = CStringGetTextDatum(defaultValue);
			new_record_repl[Anum_pg_type_typdefault - 1] = true;
		}
	}
	else
	{
		/* ALTER ... DROP DEFAULT */
		new_record_nulls[Anum_pg_type_typdefaultbin - 1] = true;
		new_record_repl[Anum_pg_type_typdefaultbin - 1] = true;
		new_record_nulls[Anum_pg_type_typdefault - 1] = true;
		new_record_repl[Anum_pg_type_typdefault - 1] = true;
	}

	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel),
								 new_record, new_record_nulls,
								 new_record_repl);

	CatalogTupleUpdate(rel, &tup->t_self, newtuple);

	/* 重建依赖关系 */
	GenerateTypeDependencies(newtuple,
							 rel,
							 defaultExpr,
							 NULL,	/* 没有现成的 typacl */
							 0, /* 关系类型不适用 */
							 false, /* 域不是隐式数组 */
							 false, /* 也不是任何类型的依赖类型 */
							 false, /* 不要触碰扩展成员关系 */
							 true); /* 我们确实需要重建依赖关系 */

	InvokeObjectPostAlterHook(TypeRelationId, domainoid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* 清理 */
	table_close(rel, RowExclusiveLock);
	heap_freetuple(newtuple);

	return address;
}

/* 
 * AlterDomainNotNull
 * 
 * 实现 ALTER DOMAIN SET/DROP NOT NULL 语句的例程。
 * 
 * 返回修改后的域的 ObjectAddress。
 */
ObjectAddress
AlterDomainNotNull(List *names, bool notNull)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	HeapTuple	tup;
	Form_pg_type typTup;
	ObjectAddress address = InvalidObjectAddress;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* 在类型表中查找域 */
	typrel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 检查它是否为域，并检查用户是否有 ALTER DOMAIN 的权限 */
	checkDomainOwner(tup);

	/* 域是否已设置为所需的约束？ */
	if (typTup->typnotnull == notNull)
	{
		table_close(typrel, RowExclusiveLock);
		return address;
	}

	/* 添加 NOT NULL 约束需要检查现有列 */
	if (notNull)
	{
		List	   *rels;
		ListCell   *rt;

		/* 获取基于此域的属性的关系列表 */
		/* ShareLock 足以防止并发数据更改 */

		rels = get_rels_with_domain(domainoid, ShareLock);

		foreach(rt, rels)
		{
			RelToCheck *rtc = (RelToCheck *) lfirst(rt);
			Relation	testrel = rtc->rel;
			TupleDesc	tupdesc = RelationGetDescr(testrel);
			TupleTableSlot *slot;
			TableScanDesc scan;
			Snapshot	snapshot;

			/* 扫描此关系中的所有元组 */
			snapshot = RegisterSnapshot(GetLatestSnapshot());
			scan = table_beginscan(testrel, snapshot, 0, NULL);
			slot = table_slot_create(testrel, NULL);
			while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
			{
				int			i;

				/* 测试属于该域的属性 */
				for (i = 0; i < rtc->natts; i++)
				{
					int			attnum = rtc->atts[i];
					Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

					if (slot_attisnull(slot, attnum))
					{
						/*
						 * 原则上，此错误的辅助信息应该是  
						 *  errdatatype()，但 errtablecol() 在实践中 
						 * 似乎更有用。由于此代码仅在           
						 *  ALTER DOMAIN 命令中执行，客户端应已    
						 * 知道是哪个域出了问题。                
						 *          
						 */
						ereport(ERROR,
								(errcode(ERRCODE_NOT_NULL_VIOLATION),
								 errmsg("column \"%s\" of table \"%s\" contains null values",
										NameStr(attr->attname),
										RelationGetRelationName(testrel)),
								 errtablecol(testrel, attnum)));
					}
				}
			}
			ExecDropSingleTupleTableSlot(slot);
			table_endscan(scan);
			UnregisterSnapshot(snapshot);

			/* 处理完成后关闭每个 rel，但保持锁定 */
			table_close(testrel, NoLock);
		}
	}

	/* 可以更新 pg_type 行了。我们可以直接修改 typTup，
	 * 因为它是副本。
	 * 
	 */
	typTup->typnotnull = notNull;

	CatalogTupleUpdate(typrel, &tup->t_self, tup);

	InvokeObjectPostAlterHook(TypeRelationId, domainoid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* 清理 */
	heap_freetuple(tup);
	table_close(typrel, RowExclusiveLock);

	return address;
}

/* 
 * AlterDomainDropConstraint
 * 
 * 实现 ALTER DOMAIN DROP CONSTRAINT 语句
 * 
 * 返回修改后的域的 ObjectAddress。
 */
ObjectAddress
AlterDomainDropConstraint(List *names, const char *constrName,
						  DropBehavior behavior, bool missing_ok)
{
	TypeName   *typename;
	Oid			domainoid;
	HeapTuple	tup;
	Relation	rel;
	Relation	conrel;
	SysScanDesc conscan;
	ScanKeyData skey[3];
	HeapTuple	contup;
	bool		found = false;
	ObjectAddress address;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* 在类型表中查找域 */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);

	/* 检查它是否为域，并检查用户是否有 ALTER DOMAIN 的权限 */
	checkDomainOwner(tup);

	/* 获取 pg_constraint 关系上的适当锁 */
	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/* 查找并删除目标约束 */
	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainoid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));

	conscan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
								 NULL, 3, skey);

	/* 最多可以有一行匹配 */
	if ((contup = systable_getnext(conscan)) != NULL)
	{
		ObjectAddress conobj;

		conobj.classId = ConstraintRelationId;
		conobj.objectId = ((Form_pg_constraint) GETSTRUCT(contup))->oid;
		conobj.objectSubId = 0;

		performDeletion(&conobj, behavior, 0);
		found = true;
	}

	/* 扫描后清理 */
	systable_endscan(conscan);
	table_close(conrel, RowExclusiveLock);

	if (!found)
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of domain \"%s\" does not exist",
							constrName, TypeNameToString(typename))));
		else
			ereport(NOTICE,
					(errmsg("constraint \"%s\" of domain \"%s\" does not exist, skipping",
							constrName, TypeNameToString(typename))));
	}

	/* 我们必须为域发送 sinval 消息，以确保任何依赖的计划
	 * 得到重建。由于此命令不会更改域的 pg_type 行，
	 * 所以不会自动发生；需要手动执行。
	 * 
	 */
	CacheInvalidateHeapTuple(rel, tup, NULL);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* 清理 */
	table_close(rel, RowExclusiveLock);

	return address;
}

/* 
 * AlterDomainAddConstraint
 * 
 * 实现 ALTER DOMAIN .. ADD CONSTRAINT 语句。
 */
ObjectAddress
AlterDomainAddConstraint(List *names, Node *newConstraint,
						 ObjectAddress *constrAddr)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	HeapTuple	tup;
	Form_pg_type typTup;
	Constraint *constr;
	char	   *ccbin;
	ObjectAddress address;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* 在类型表中查找域 */
	typrel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 检查它是否为域，并检查用户是否有 ALTER DOMAIN 的权限 */
	checkDomainOwner(tup);

	if (!IsA(newConstraint, Constraint))
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(newConstraint));

	constr = (Constraint *) newConstraint;

	switch (constr->contype)
	{
		case CONSTR_CHECK:
			/* 在下面处理 */
			break;

		case CONSTR_UNIQUE:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unique constraints not possible for domains")));
			break;

		case CONSTR_PRIMARY:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("primary key constraints not possible for domains")));
			break;

		case CONSTR_EXCLUSION:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("exclusion constraints not possible for domains")));
			break;

		case CONSTR_FOREIGN:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("foreign key constraints not possible for domains")));
			break;

		case CONSTR_ATTR_DEFERRABLE:
		case CONSTR_ATTR_NOT_DEFERRABLE:
		case CONSTR_ATTR_DEFERRED:
		case CONSTR_ATTR_IMMEDIATE:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("specifying constraint deferrability not supported for domains")));
			break;

		default:
			elog(ERROR, "unrecognized constraint subtype: %d",
				 (int) constr->contype);
			break;
	}

	/*
	 * 由于所有其他约束类型都抛出错误，这必须是       
	 * 检查约束。首先，处理约束表达式并向 pg_constraint     
	 *  添加条目。 
	 */

	ccbin = domainAddConstraint(domainoid, typTup->typnamespace,
								typTup->typbasetype, typTup->typtypmod,
								constr, NameStr(typTup->typname), constrAddr);

	/* 如果请求验证约束，则测试所有基于该域的属性中
	 * 存储的值，该约束正在被添加到这些属性上。
	 * 
	 */
	if (!constr->skip_validation)
		validateDomainConstraint(domainoid, ccbin);

	/* 我们必须为域发送 sinval 消息，以确保任何依赖的计划
	 * 得到重建。由于此命令不会更改域的 pg_type 行，
	 * 所以不会自动发生；需要手动执行。
	 * 
	 */
	CacheInvalidateHeapTuple(typrel, tup, NULL);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	/* 清理 */
	table_close(typrel, RowExclusiveLock);

	return address;
}

/* 
 * AlterDomainValidateConstraint
 * 
 * 实现 ALTER DOMAIN .. VALIDATE CONSTRAINT 语句。
 */
ObjectAddress
AlterDomainValidateConstraint(List *names, const char *constrName)
{
	TypeName   *typename;
	Oid			domainoid;
	Relation	typrel;
	Relation	conrel;
	HeapTuple	tup;
	Form_pg_constraint con;
	Form_pg_constraint copy_con;
	char	   *conbin;
	SysScanDesc scan;
	Datum		val;
	HeapTuple	tuple;
	HeapTuple	copyTuple;
	ScanKeyData skey[3];
	ObjectAddress address;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	domainoid = typenameTypeId(NULL, typename);

	/* 在类型表中查找域 */
	typrel = table_open(TypeRelationId, AccessShareLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(domainoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", domainoid);

	/* 检查它是否为域，并检查用户是否有 ALTER DOMAIN 的权限 */
	checkDomainOwner(tup);

	/*
	 * 查找并检查目标约束         
	 */
	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainoid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(constrName));

	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 3, skey);

	/* 最多可以有一行匹配 */
	if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("constraint \"%s\" of domain \"%s\" does not exist",
						constrName, TypeNameToString(typename))));

	con = (Form_pg_constraint) GETSTRUCT(tuple);
	if (con->contype != CONSTRAINT_CHECK)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("constraint \"%s\" of domain \"%s\" is not a check constraint",
						constrName, TypeNameToString(typename))));

	val = SysCacheGetAttrNotNull(CONSTROID, tuple, Anum_pg_constraint_conbin);
	conbin = TextDatumGetCString(val);

	validateDomainConstraint(domainoid, conbin);

	/*
	 * 现在我们还在里面，更新目录。          
	 */
	copyTuple = heap_copytuple(tuple);
	copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);
	copy_con->convalidated = true;
	CatalogTupleUpdate(conrel, &copyTuple->t_self, copyTuple);

	InvokeObjectPostAlterHook(ConstraintRelationId, con->oid, 0);

	ObjectAddressSet(address, TypeRelationId, domainoid);

	heap_freetuple(copyTuple);

	systable_endscan(scan);

	table_close(typrel, AccessShareLock);
	table_close(conrel, RowExclusiveLock);

	ReleaseSysCache(tup);

	return address;
}

static void
validateDomainConstraint(Oid domainoid, char *ccbin)
{
	Expr	   *expr = (Expr *) stringToNode(ccbin);
	List	   *rels;
	ListCell   *rt;
	EState	   *estate;
	ExprContext *econtext;
	ExprState  *exprstate;

	/* 需要一个 EState 来运行 ExecEvalExpr */
	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);

	/* 为 expr 构建执行状态 */
	exprstate = ExecPrepareExpr(expr, estate);

	/* 获取基于此域的属性的关系列表 */
	/* ShareLock 足以防止并发数据更改 */

	rels = get_rels_with_domain(domainoid, ShareLock);

	foreach(rt, rels)
	{
		RelToCheck *rtc = (RelToCheck *) lfirst(rt);
		Relation	testrel = rtc->rel;
		TupleDesc	tupdesc = RelationGetDescr(testrel);
		TupleTableSlot *slot;
		TableScanDesc scan;
		Snapshot	snapshot;

		/* 扫描此关系中的所有元组 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(testrel, snapshot, 0, NULL);
		slot = table_slot_create(testrel, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			int			i;

			/* 测试属于该域的属性 */
			for (i = 0; i < rtc->natts; i++)
			{
				int			attnum = rtc->atts[i];
				Datum		d;
				bool		isNull;
				Datum		conResult;
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

				d = slot_getattr(slot, attnum, &isNull);

				econtext->domainValue_datum = d;
				econtext->domainValue_isNull = isNull;

				conResult = ExecEvalExprSwitchContext(exprstate,
													  econtext,
													  &isNull);

				if (!isNull && !DatumGetBool(conResult))
				{
					/*
					 * 原则上，此错误的辅助信息应该是        
					 *  errdomainconstraint()，但 errtablecol()        
					 *  在实践中似乎更有用。由于此代码仅在   
					 *  ALTER DOMAIN 命令中执行，客户端应已   
					 * 知道是哪个域出了问题，以及哪个约束。 
					 *                          
					 */
					ereport(ERROR,
							(errcode(ERRCODE_CHECK_VIOLATION),
							 errmsg("column \"%s\" of table \"%s\" contains values that violate the new constraint",
									NameStr(attr->attname),
									RelationGetRelationName(testrel)),
							 errtablecol(testrel, attnum)));
				}
			}

			ResetExprContext(econtext);
		}
		ExecDropSingleTupleTableSlot(slot);
		table_endscan(scan);
		UnregisterSnapshot(snapshot);

		/* 保持关系锁直到提交（对并发来说不太理想） */
		table_close(testrel, NoLock);
	}

	FreeExecutorState(estate);
}

/* 
 * get_rels_with_domain
 * 
 * 获取所有使用该域的关系/属性
 * 
 * 结果是一个 RelToCheck 结构体的列表，每个不同的
 * 关系一个，每个包含一个或多个属于该域类型的属性编号。
 * 我们已经打开了每个 rel 并获取了指定类型的锁。
 * 
 * 我们通过包含派生域类型的属性来支持嵌套域。
 * 当前调用者不需要区分完全属于给定域的属性和
 * 属于派生域的属性。
 * 
 * XXX 这完全不可靠，因为没有办法在命令运行时锁住域
 * 以防止列被添加或删除。我们可以通过在遇到关系时
 * 锁定它们来部分防止列删除，但仍存在竞争条件（在看出
 * pg_depend 条目和获取关系锁之间的窗口）。
 * 此外，同时持有所有关系的锁会产生不可忽视的
 * 死锁风险。我们可以通过使用最弱合适的锁（大多数调用者
 * 使用 ShareLock）来最小化但不能消除死锁风险。
 * 
 * XXX 此 API 不足以支持检查位于容器类型内部的域值，
 * 例如组合类型、数组或范围。目前，如果包含
 * 目标域的容器类型存储在任何地方，我们只报错。
 * 
 * 通常用于在向域添加新约束时获取
 * 测试列表。
 *
 *
 *
 */
static List *
get_rels_with_domain(Oid domainOid, LOCKMODE lockmode)
{
	List	   *result = NIL;
	char	   *domainTypeName = format_type_be(domainOid);
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	Assert(lockmode != NoLock);

	/* 由于此函数递归，可能会被驱动到栈溢出 */
	check_stack_depth();

	/* 我们扫描 pg_depend 以查找依赖该域的对象。（对于域，
	 * 我们可以忽略 refobjsubid。）
	 * 
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(domainOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		RelToCheck *rtc = NULL;
		ListCell   *rellist;
		Form_pg_attribute pg_att;
		int			ptr;

		/* 检查直接依赖的类型 */
		if (pg_depend->classid == TypeRelationId)
		{
			if (get_typtype(pg_depend->objid) == TYPTYPE_DOMAIN)
			{
				/*
				 * 这是一个子域，因此需要递归地将依赖列    
				 * 添加到输出列表中。这样效率稍低，因为当  
				 * 同一 rel 的属性派生域类型不同时，我们    
				 * 可能无法合并 RelToCheck 条目，但这可能   
				 * 不值得改进。           
				 */
				result = list_concat(result,
									 get_rels_with_domain(pg_depend->objid,
														  lockmode));
			}
			else
			{
				/* 否则，它是使用该域的某个容器类型，因此如果存在
				 * 任何此类型的列，则报错。
				 * 
				 */
				find_composite_type_dependencies(pg_depend->objid,
												 NULL,
												 domainTypeName);
			}
			continue;
		}

		/* 否则，忽略不是关系用户列的依赖对象 */
		/* （我们假设系统列从来不是域类型） */
		if (pg_depend->classid != RelationRelationId ||
			pg_depend->objsubid <= 0)
			continue;

		/* 查看我们是否已有此关系的条目 */
		foreach(rellist, result)
		{
			RelToCheck *rt = (RelToCheck *) lfirst(rellist);

			if (RelationGetRelid(rt->rel) == pg_depend->objid)
			{
				rtc = rt;
				break;
			}
		}

		if (rtc == NULL)
		{
			/* 为此关系找到的第一个属性 */
			Relation	rel;

			/* 获取请求的关系锁 */
			rel = relation_open(pg_depend->objid, lockmode);

			/* 检查 rowtype 是否作为组合类型列存储在任何地方；
			 * 如果是，我们目前必须报错。
			 * 
			 */
			if (OidIsValid(rel->rd_rel->reltype))
				find_composite_type_dependencies(rel->rd_rel->reltype,
												 NULL,
												 domainTypeName);

			/*
			 * 否则，除具有存储和用户所选择列类型的   
			 * 关系外均可忽略。             
			 *
			 * XXX 若仅索引扫描能由合适的表达式索引满足     
			 * "col::some_domain"，那么这里还应当检查表达式     
			 * 索引列。  
			 */
			if (rel->rd_rel->relkind != RELKIND_RELATION &&
				rel->rd_rel->relkind != RELKIND_MATVIEW)
			{
				relation_close(rel, lockmode);
				continue;
			}

			/* 构建 RelToCheck 条目，为所有属性留出足够空间 */
			rtc = (RelToCheck *) palloc(sizeof(RelToCheck));
			rtc->rel = rel;
			rtc->natts = 0;
			rtc->atts = (int *) palloc(sizeof(int) * RelationGetNumberOfAttributes(rel));
			result = lappend(result, rtc);
		}

		/*
		 * 确认列未被删除，并且属于预期的类型。           
		 * 这可以防范恰在我们获取锁之前发生的               
		 *  ALTER DROP COLUMN……但如果整个表被删了，我们     
		 * 会有问题。
		 */
		if (pg_depend->objsubid > RelationGetNumberOfAttributes(rtc->rel))
			continue;
		pg_att = TupleDescAttr(rtc->rel->rd_att, pg_depend->objsubid - 1);
		if (pg_att->attisdropped || pg_att->atttypid != domainOid)
			continue;

		/*
		 * 好的，将该列加入结果。我们按列编号顺序存储   
		 * 这些列；这只是为了提高回归测试输出的            
		 * 小技巧……
		 */
		Assert(rtc->natts < RelationGetNumberOfAttributes(rtc->rel));

		ptr = rtc->natts++;
		while (ptr > 0 && rtc->atts[ptr - 1] > pg_depend->objsubid)
		{
			rtc->atts[ptr] = rtc->atts[ptr - 1];
			ptr--;
		}
		rtc->atts[ptr] = pg_depend->objsubid;
	}

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	return result;
}

/* 
 * checkDomainOwner
 * 
 * 检查类型是否确实为域，并且当前用户
 * 是否有对其执行 ALTER DOMAIN 的权限。如果有误则报错。
 */
void
checkDomainOwner(HeapTuple tup)
{
	Form_pg_type typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 检查这确实是一个域 */
	if (typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typTup->oid))));

	/* 权限检查：必须拥有类型 */
	if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);
}

/* 
 * domainAddConstraint - CREATE 和 ALTER DOMAIN 之间共享的代码
 */
static char *
domainAddConstraint(Oid domainOid, Oid domainNamespace, Oid baseTypeOid,
					int typMod, Constraint *constr,
					const char *domainName, ObjectAddress *constrAddr)
{
	Node	   *expr;
	char	   *ccbin;
	ParseState *pstate;
	CoerceToDomainValue *domVal;
	Oid			ccoid;

	/*
	 * 分配或验证约束名称       
	 */
	if (constr->conname)
	{
		if (ConstraintNameIsUsed(CONSTRAINT_DOMAIN,
								 domainOid,
								 constr->conname))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for domain \"%s\" already exists",
							constr->conname, domainName)));
	}
	else
		constr->conname = ChooseConstraintName(domainName,
											   NULL,
											   "check",
											   domainNamespace,
											   NIL);

	/*
	 * 将 raw_expr 中的 A_EXPR 转换为 EXPR  
	 */
	pstate = make_parsestate(NULL);

	/*
	 * 设置一个 CoerceToDomainValue 来表示 VALUE 在                
	 * 表达式中的出现。注意，它看起来具有的是基础       
	 * 类型而非域的类型。这似乎是正确的，因为在    
	 * 检查表达式内部，我们不应假设输入值可被           
	 * 视为域的成员。
	 */
	domVal = makeNode(CoerceToDomainValue);
	domVal->typeId = baseTypeOid;
	domVal->typeMod = typMod;
	domVal->collation = get_typcollation(baseTypeOid);
	domVal->location = -1;		/* 将在使用时设置 */

	pstate->p_pre_columnref_hook = replace_domain_constraint_value;
	pstate->p_ref_hook_state = (void *) domVal;

	expr = transformExpr(pstate, constr->raw_expr, EXPR_KIND_DOMAIN_CHECK);

	/*
	 * 确保产生布尔结果。          
	 */
	expr = coerce_to_boolean(pstate, expr, "CHECK");

	/*
	 * 修正排序规则信息。  
	 */
	assign_expr_collations(pstate, expr);

	/* 域不允许变量（这可能现在是死代码了，因为
	 * add_missing_from 已成为历史，但为了安全起见保留）。
	 * 
	 */
	if (pstate->p_rtable != NIL ||
		contain_var_clause(expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("cannot use table references in domain check constraint")));

	/* 转换为字符串形式以便存储。
	 * 
	 */
	ccbin = nodeToString(expr);

	/*
	 * 将约束存储在 pg_constraint 中 
	 */
	ccoid =
		CreateConstraintEntry(constr->conname,	/* Constraint Name */
							  domainNamespace,	/* 命名空间 */
							  CONSTRAINT_CHECK, /* Constraint Type */
							  false,	/* Is Deferrable */
							  false,	/* Is Deferred */
							  !constr->skip_validation, /* Is Validated */
							  InvalidOid,	/* 没有父约束 */
							  InvalidOid,	/* 不是关系约束 */
							  NULL,
							  0,
							  0,
							  domainOid,	/* domain constraint */
							  InvalidOid,	/* 没有关联的索引 */
							  InvalidOid,	/* Foreign key fields */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* 不是排他约束 */
							  expr, /* 检查约束的树形式 */
							  ccbin,	/* 检查约束的二进制形式 */
							  true, /* is local */
							  0,	/* inhcount */
							  false,	/* connoinherit */
							  false);	/* is_internal */
	if (constrAddr)
		ObjectAddressSet(*constrAddr, ConstraintRelationId, ccoid);

	/* 返回编译后的约束表达式，以便调用例程可以
	 * 执行任何额外所需的测试。
	 * 
	 */
	return ccbin;
}

/* 域 CHECK 约束解析的解析器 pre_columnref_hook */
static Node *
replace_domain_constraint_value(ParseState *pstate, ColumnRef *cref)
{
	/* 检查对 "value" 的引用，如果是，则用 domainAddConstraint
	 * 为我们准备的 CoerceToDomainValue 替换。（我们将 VALUE 作为名称
	 * 处理而非关键字，以避免破坏许多过去使用 VALUE
	 * 作为列名的应用程序。）
	 * 
	 */
	if (list_length(cref->fields) == 1)
	{
		Node	   *field1 = (Node *) linitial(cref->fields);
		char	   *colname;

		colname = strVal(field1);
		if (strcmp(colname, "value") == 0)
		{
			CoerceToDomainValue *domVal = copyObject(pstate->p_ref_hook_state);

			/* 传播位置信息（如果有的话） */
			domVal->location = cref->location;
			return (Node *) domVal;
		}
	}
	return NULL;
}


/* 
 * 执行 ALTER TYPE RENAME
 */
ObjectAddress
RenameType(RenameStmt *stmt)
{
	List	   *names = castNode(List, stmt->object);
	const char *newTypeName = stmt->newname;
	TypeName   *typename;
	Oid			typeOid;
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;
	ObjectAddress address;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* 在类型表中查找类型 */
	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 检查类型的权限 */
	if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/* 在非域上使用了 ALTER DOMAIN？ */
	if (stmt->renameType == OBJECT_DOMAIN && typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/* 如果是组合类型，我们需要检查它确实是一个
	 * 独立的组合类型，而不是表的 rowtype。我们希望人们
	 * 在这种情况下使用 ALTER TABLE 而非 ALTER TYPE。
	 * 
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE &&
		get_rel_relkind(typTup->typrelid) != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	/* 也不允许直接修改数组类型 */
	if (IsTrueArrayType(typTup))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(typTup->typelem))));

	/* 如果是组合类型，我们还需要重命名关联的 pg_class 条目。
	 * RenameRelationInternal 将自动调用 RenameTypeInternal。
	 * 
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE)
		RenameRelationInternal(typTup->typrelid, newTypeName, false, false);
	else
		RenameTypeInternal(typeOid, newTypeName,
						   typTup->typnamespace);

	ObjectAddressSet(address, TypeRelationId, typeOid);
	/* 清理 */
	table_close(rel, RowExclusiveLock);

	return address;
}

/* 
 * 更改类型的所有者。
 */
ObjectAddress
AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype)
{
	TypeName   *typename;
	Oid			typeOid;
	Relation	rel;
	HeapTuple	tup;
	HeapTuple	newtup;
	Form_pg_type typTup;
	AclResult	aclresult;
	ObjectAddress address;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);

	/* 在此使用 LookupTypeName，以便可以处理 shell 类型 */
	tup = LookupTypeName(NULL, typename, NULL, false);
	if (tup == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("type \"%s\" does not exist",
						TypeNameToString(typename))));
	typeOid = typeTypeId(tup);

	/* 复制 syscache 条目，以便我们可以在下面直接修改 */
	newtup = heap_copytuple(tup);
	ReleaseSysCache(tup);
	tup = newtup;
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/* 不允许在类型上使用 ALTER DOMAIN */
	if (objecttype == OBJECT_DOMAIN && typTup->typtype != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/* 如果是组合类型，我们需要检查它确实是一个
	 * 独立的组合类型，而不是表的 rowtype。我们希望人们
	 * 在这种情况下使用 ALTER TABLE 而非 ALTER TYPE。
	 * 
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE &&
		get_rel_relkind(typTup->typrelid) != RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	/* 也不允许直接修改数组类型 */
	if (IsTrueArrayType(typTup))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(typTup->typelem))));

	/* 如果新所有者与现有所有者相同，视为命令成功。
	 * 这是为了转储恢复目的。
	 * 
	 */
	if (typTup->typowner != newOwnerId)
	{
		/* 超级用户可以始终这样做 */
		if (!superuser())
		{
			/* 否则，必须是现有对象的所有者 */
			if (!object_ownercheck(TypeRelationId, typTup->oid, GetUserId()))
				aclcheck_error_type(ACLCHECK_NOT_OWNER, typTup->oid);

			/* 必须能够成为新的所有者 */
			check_can_set_role(GetUserId(), newOwnerId);

			/* 新所有者必须具有命名空间的 CREATE 权限 */
			aclresult = object_aclcheck(NamespaceRelationId, typTup->typnamespace,
										newOwnerId,
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_SCHEMA,
							   get_namespace_name(typTup->typnamespace));
		}

		AlterTypeOwner_oid(typeOid, newOwnerId, true);
	}

	ObjectAddressSet(address, TypeRelationId, typeOid);

	/* 清理 */
	table_close(rel, RowExclusiveLock);

	return address;
}

/* 
 * AlterTypeOwner_oid - 无条件更改类型所有者
 * 
 * 此函数递归以处理 pg_class 条目（如果需要的话）。
 * 它调用任何必要的访问对象钩子。如果 hasDependEntry 为 true，则
 * 此函数适当地修改 pg_shdepend 条目（仅对表 rowtype
 * 和数组类型应传递 false）。
 * 
 * 这由 ALTER TABLE/TYPE OWNER 命令以及 REASSIGN OWNED BY 使用。
 * 它假设调用者已完成所有必要的检查。
 */
void
AlterTypeOwner_oid(Oid typeOid, Oid newOwnerId, bool hasDependEntry)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	/*
	 * 如果是组合类型，调用 ATExecChangeOwner 以便我们       
	 * 能正确修复 pg_class 条目。这将回调到   
	 *  AlterTypeOwnerInternal 来处理 pg_type 条目。         
	 */
	if (typTup->typtype == TYPTYPE_COMPOSITE)
		ATExecChangeOwner(typTup->typrelid, newOwnerId, true, AccessExclusiveLock);
	else
		AlterTypeOwnerInternal(typeOid, newOwnerId);

	/* 更新所有者依赖引用 */
	if (hasDependEntry)
		changeDependencyOnOwner(TypeRelationId, typeOid, newOwnerId);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	ReleaseSysCache(tup);
	table_close(rel, RowExclusiveLock);
}

/* 
 * AlterTypeOwnerInternal - 最简类型所有者更改。
 * 
 * 此例程仅修改 pg_type 条目的所有者，并递归
 * 处理可能的数组类型。
 */
void
AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typTup;
	Datum		repl_val[Natts_pg_type];
	bool		repl_null[Natts_pg_type];
	bool		repl_repl[Natts_pg_type];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typTup = (Form_pg_type) GETSTRUCT(tup);

	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	repl_repl[Anum_pg_type_typowner - 1] = true;
	repl_val[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(newOwnerId);

	aclDatum = heap_getattr(tup,
							Anum_pg_type_typacl,
							RelationGetDescr(rel),
							&isNull);
	/* Null ACLs 不需要更改 */
	if (!isNull)
	{
		newAcl = aclnewowner(DatumGetAclP(aclDatum),
							 typTup->typowner, newOwnerId);
		repl_repl[Anum_pg_type_typacl - 1] = true;
		repl_val[Anum_pg_type_typacl - 1] = PointerGetDatum(newAcl);
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
							repl_repl);

	CatalogTupleUpdate(rel, &tup->t_self, tup);

	/* 如果它有数组类型，也更新那个 */
	if (OidIsValid(typTup->typarray))
		AlterTypeOwnerInternal(typTup->typarray, newOwnerId);

	/* 清理 */
	table_close(rel, RowExclusiveLock);
}

/* 
 * 执行 ALTER TYPE SET SCHEMA
 */
ObjectAddress
AlterTypeNamespace(List *names, const char *newschema, ObjectType objecttype,
				   Oid *oldschema)
{
	TypeName   *typename;
	Oid			typeOid;
	Oid			nspOid;
	Oid			oldNspOid;
	ObjectAddresses *objsMoved;
	ObjectAddress myself;

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(names);
	typeOid = typenameTypeId(NULL, typename);

	/* 不允许在类型上使用 ALTER DOMAIN */
	if (objecttype == OBJECT_DOMAIN && get_typtype(typeOid) != TYPTYPE_DOMAIN)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a domain",
						format_type_be(typeOid))));

	/* 获取模式 OID 并检查其权限 */
	nspOid = LookupCreationNamespace(newschema);

	objsMoved = new_object_addresses();
	oldNspOid = AlterTypeNamespace_oid(typeOid, nspOid, objsMoved);
	free_object_addresses(objsMoved);

	if (oldschema)
		*oldschema = oldNspOid;

	ObjectAddressSet(myself, TypeRelationId, typeOid);

	return myself;
}

Oid
AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, ObjectAddresses *objsMoved)
{
	Oid			elemOid;

	/* 检查类型的权限 */
	if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
		aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);

	/* 不允许直接修改数组类型 */
	elemOid = get_element_type(typeOid);
	if (OidIsValid(elemOid) && get_array_type(elemOid) == typeOid)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot alter array type %s",
						format_type_be(typeOid)),
				 errhint("You can alter type %s, which will alter the array type as well.",
						 format_type_be(elemOid))));

	/* 然后执行工作 */
	return AlterTypeNamespaceInternal(typeOid, nspOid, false, true, objsMoved);
}

/*
 * 指定类型移至新命名空间。 
 *
 * 调用者必须已检查过权限。        
 *
 * 该函数会自动递归处理类型的数组类型（如果有的   
 * 的话）。isImplicitArray 只应在进行这种内部递归时     
 * 才应为 true（外部调用者绝不能直接移动数组类型）。   
 *
 * 如果 errorOnTableType 为 true，则当类型为表类型时     
 * 函数报错。要用 ALTER TABLE 移动表到新命名空间。  
 *           
 *
 * 返回类型原命名空间 OID。   
 */
Oid
AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool errorOnTableType,
						   ObjectAddresses *objsMoved)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_type typform;
	Oid			oldNspOid;
	Oid			arrayOid;
	bool		isCompositeType;
	ObjectAddress thisobj;

	/*
	 * 确保我们之前没有移动过此对象。     
	 */
	thisobj.classId = TypeRelationId;
	thisobj.objectId = typeOid;
	thisobj.objectSubId = 0;

	if (object_address_present(&thisobj, objsMoved))
		return InvalidOid;

	rel = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typform = (Form_pg_type) GETSTRUCT(tup);

	oldNspOid = typform->typnamespace;
	arrayOid = typform->typarray;

	/* 如果类型已经在那里，我们可以跳过接下来的几个检查。 */
	if (oldNspOid != nspOid)
	{
		/* 切换命名空间的通用检查 */
		CheckSetNamespace(oldNspOid, nspOid);

		/* 检查重复名称（比唯一索引失败更友好） */
		if (SearchSysCacheExists2(TYPENAMENSP,
								  NameGetDatum(&typform->typname),
								  ObjectIdGetDatum(nspOid)))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists in schema \"%s\"",
							NameStr(typform->typname),
							get_namespace_name(nspOid))));
	}

	/* 检测类型是否为组合类型（但不是表 rowtype） */
	isCompositeType =
		(typform->typtype == TYPTYPE_COMPOSITE &&
		 get_rel_relkind(typform->typrelid) == RELKIND_COMPOSITE_TYPE);

	/* 如果要求，强制执行非表类型 */
	if (typform->typtype == TYPTYPE_COMPOSITE && !isCompositeType &&
		errorOnTableType)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is a table's row type",
						format_type_be(typeOid)),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TABLE")));

	if (oldNspOid != nspOid)
	{
		/* 好的，修改 pg_type 行 */

		/* tup 是副本，因此我们可以直接在其上修改 */
		typform->typnamespace = nspOid;

		CatalogTupleUpdate(rel, &tup->t_self, tup);
	}

	/*
	 * 组合类型拥有 pg_class 条目。 
	 *
	 * 我们还需要修改 pg_class 元组，以反映模式的变更。  
	 *        
	 */
	if (isCompositeType)
	{
		Relation	classRel;

		classRel = table_open(RelationRelationId, RowExclusiveLock);

		AlterRelationNamespaceInternal(classRel, typform->typrelid,
									   oldNspOid, nspOid,
									   false, objsMoved);

		table_close(classRel, RowExclusiveLock);

		/* 检查与组合类型关联的约束（我们目前
		 * 不支持这个，但将来可能会支持）。
		 * 
		 */
		AlterConstraintNamespaces(typform->typrelid, oldNspOid,
								  nspOid, false, objsMoved);
	}
	else
	{
		/* 如果是域，它可能有约束 */
		if (typform->typtype == TYPTYPE_DOMAIN)
			AlterConstraintNamespaces(typeOid, oldNspOid, nspOid, true,
									  objsMoved);
	}

	/* 更新模式依赖（如果有的话）——表 rowtype 没有，隐式数组
	 * 也没有。
	 * 
	 */
	if (oldNspOid != nspOid &&
		(isCompositeType || typform->typtype != TYPTYPE_COMPOSITE) &&
		!isImplicitArray)
		if (changeDependencyFor(TypeRelationId, typeOid,
								NamespaceRelationId, oldNspOid, nspOid) != 1)
			elog(ERROR, "failed to change schema dependency for type %s",
				 format_type_be(typeOid));

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	add_exact_object_address(&thisobj, objsMoved);

	/* 递归修改关联的数组类型（如果有的话） */
	if (OidIsValid(arrayOid))
		AlterTypeNamespaceInternal(arrayOid, nspOid, true, true, objsMoved);

	return oldNspOid;
}

/* 
 * AlterType
 *         ALTER TYPE <type> SET (option = ...)
 * 
 * 注意：这里允许的更改集受到许多非显而易见的实现限制。
 * 在考虑添加新的灵活性时要谨慎。
 * 
 */
ObjectAddress
AlterType(AlterTypeStmt *stmt)
{
	ObjectAddress address;
	Relation	catalog;
	TypeName   *typename;
	HeapTuple	tup;
	Oid			typeOid;
	Form_pg_type typForm;
	bool		requireSuper = false;
	AlterTypeRecurseParams atparams;
	ListCell   *pl;

	catalog = table_open(TypeRelationId, RowExclusiveLock);

	/* 创建一个 TypeName，以便使用标准类型查找机制 */
	typename = makeTypeNameFromNameList(stmt->typeName);
	tup = typenameType(NULL, typename, NULL);

	typeOid = typeTypeId(tup);
	typForm = (Form_pg_type) GETSTRUCT(tup);

	/* 处理选项 */
	memset(&atparams, 0, sizeof(atparams));
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "storage") == 0)
		{
			char	   *a = defGetString(defel);

			if (pg_strcasecmp(a, "plain") == 0)
				atparams.storage = TYPSTORAGE_PLAIN;
			else if (pg_strcasecmp(a, "external") == 0)
				atparams.storage = TYPSTORAGE_EXTERNAL;
			else if (pg_strcasecmp(a, "extended") == 0)
				atparams.storage = TYPSTORAGE_EXTENDED;
			else if (pg_strcasecmp(a, "main") == 0)
				atparams.storage = TYPSTORAGE_MAIN;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("storage \"%s\" not recognized", a)));

			/* 验证存储请求。如果类型不是 varlena，
			 * 它肯定不支持非 PLAIN 存储。
			 * 
			 */
			if (atparams.storage != TYPSTORAGE_PLAIN && typForm->typlen != -1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("fixed-size types must have storage PLAIN")));

			/* 允许从 PLAIN 切换到非 PLAIN，但需要超级用户，
			 * 因为我们无法验证类型的 C 函数是否支持。
			 * 禁止从非 PLAIN 切换到 PLAIN，
			 * 因为无法确保没有表具有该类型已 toast 的值。
			 * 在不同非 PLAIN 设置之间切换是可以的，
			 * 因为它只是请求在未来创建的列中使用
			 * 不同策略。
			 * 
			 * 
			 */
			if (atparams.storage != TYPSTORAGE_PLAIN &&
				typForm->typstorage == TYPSTORAGE_PLAIN)
				requireSuper = true;
			else if (atparams.storage == TYPSTORAGE_PLAIN &&
					 typForm->typstorage != TYPSTORAGE_PLAIN)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cannot change type's storage to PLAIN")));

			atparams.updateStorage = true;
		}
		else if (strcmp(defel->defname, "receive") == 0)
		{
			if (defel->arg != NULL)
				atparams.receiveOid =
					findTypeReceiveFunction(defGetQualifiedName(defel),
											typeOid);
			else
				atparams.receiveOid = InvalidOid;	/* NONE，移除函数 */
			atparams.updateReceive = true;
			/* 替换 I/O 函数需要超级用户。 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "send") == 0)
		{
			if (defel->arg != NULL)
				atparams.sendOid =
					findTypeSendFunction(defGetQualifiedName(defel),
										 typeOid);
			else
				atparams.sendOid = InvalidOid;	/* NONE，移除函数 */
			atparams.updateSend = true;
			/* 替换 I/O 函数需要超级用户。 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "typmod_in") == 0)
		{
			if (defel->arg != NULL)
				atparams.typmodinOid =
					findTypeTypmodinFunction(defGetQualifiedName(defel));
			else
				atparams.typmodinOid = InvalidOid;	/* NONE，移除函数 */
			atparams.updateTypmodin = true;
			/* 替换 I/O 函数需要超级用户。 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "typmod_out") == 0)
		{
			if (defel->arg != NULL)
				atparams.typmodoutOid =
					findTypeTypmodoutFunction(defGetQualifiedName(defel));
			else
				atparams.typmodoutOid = InvalidOid; /* NONE，移除函数 */
			atparams.updateTypmodout = true;
			/* 替换 I/O 函数需要超级用户。 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "analyze") == 0)
		{
			if (defel->arg != NULL)
				atparams.analyzeOid =
					findTypeAnalyzeFunction(defGetQualifiedName(defel),
											typeOid);
			else
				atparams.analyzeOid = InvalidOid;	/* NONE，移除函数 */
			atparams.updateAnalyze = true;
			/* 替换分析函数需要超级用户。 */
			requireSuper = true;
		}
		else if (strcmp(defel->defname, "subscript") == 0)
		{
			if (defel->arg != NULL)
				atparams.subscriptOid =
					findTypeSubscriptingFunction(defGetQualifiedName(defel),
												 typeOid);
			else
				atparams.subscriptOid = InvalidOid; /* NONE，移除函数 */
			atparams.updateSubscript = true;
			/* 替换下标函数需要超级用户。 */
			requireSuper = true;
		}

		/* CREATE 所接受的其他选项都无法更改。
		 * 检查它们以便给出有意义的错误消息。
		 * 
		 */
		else if (strcmp(defel->defname, "input") == 0 ||
				 strcmp(defel->defname, "output") == 0 ||
				 strcmp(defel->defname, "internallength") == 0 ||
				 strcmp(defel->defname, "passedbyvalue") == 0 ||
				 strcmp(defel->defname, "alignment") == 0 ||
				 strcmp(defel->defname, "like") == 0 ||
				 strcmp(defel->defname, "category") == 0 ||
				 strcmp(defel->defname, "preferred") == 0 ||
				 strcmp(defel->defname, "default") == 0 ||
				 strcmp(defel->defname, "element") == 0 ||
				 strcmp(defel->defname, "delimiter") == 0 ||
				 strcmp(defel->defname, "collatable") == 0)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" cannot be changed",
							defel->defname)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("type attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* 权限检查。如果我们决定命令需要超级用户，则要求超级用户；
	 * 否则必须拥有类型。
	 * 
	 */
	if (requireSuper)
	{
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to alter a type")));
	}
	else
	{
		if (!object_ownercheck(TypeRelationId, typeOid, GetUserId()))
			aclcheck_error_type(ACLCHECK_NOT_OWNER, typeOid);
	}

	/*
	 * 对于所有非普通基础类型，我们禁止任何形式的        
	 *  ALTER TYPE SET。例如，修改组合类型的             
	 * 发送/接收函数将非常不安全，更不用说毫无意义。  
	 * 此外，pg_dump 不支持更改非基础类型的               
	 * 这些属性。将来可能放宽，但不是现在。     
	 *
	 * 注意：如果放宽此限制到足以允许组合类型，务必  
	 * 调整 AlterTypeRecurse 中 GenerateTypeDependencies 调用  
	 */
	if (typForm->typtype != TYPTYPE_BASE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a base type",
						format_type_be(typeOid))));

	/* 出于相同的原因，不允许直接修改数组类型。
	 * 
	 */
	if (IsTrueArrayType(typForm))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not a base type",
						format_type_be(typeOid))));

	/* 好的，递归更新此类型及其上的任何数组/域 */
	AlterTypeRecurse(typeOid, false, tup, catalog, &atparams);

	/* 清理 */
	ReleaseSysCache(tup);

	table_close(catalog, RowExclusiveLock);

	ObjectAddressSet(address, TypeRelationId, typeOid);

	return address;
}

/* 
 * AlterTypeRecurse：AlterType() 的一步递归
 * 
 * 将由 "atparams" 指定的更改应用于由 "typeOid" 标识的类型，
 * 其现有 pg_type 元组为 "tup"。如果需要，递归更新其
 * 数组类型。然后搜索此类型上的任何域，并递归地将
 * （大部分）相同的更改应用于这些域。
 * 
 * 我们需要这样做，因为系统通常假定域从基础类型
 * 继承许多属性。参见上面的 DefineDomain() 了解
 * 继承的详细信息。数组继承的属性数量较少，
 * 但并非零。
 * 
 * 这里存在竞争条件，某些其他事务可能同时在该基础类型上
 * 添加另一个域；我们将错过更新那个域。因此，对于域
 * 与基础类型不同步会造成严重后果的属性（例如 typlen），
 * 要谨慎允许 ALTER TYPE 更改这些属性。在实践中，
 * 对于 ALTER TYPE 的合理用例，竞争似乎不太可能成为问题。
 * 如果确实发生了，可以通过在所有先前事务提交后，
 * 重新执行相同的 ALTER TYPE 来修复。
 *
 *
 */
static void
AlterTypeRecurse(Oid typeOid, bool isImplicitArray,
				 HeapTuple tup, Relation catalog,
				 AlterTypeRecurseParams *atparams)
{
	Datum		values[Natts_pg_type];
	bool		nulls[Natts_pg_type];
	bool		replaces[Natts_pg_type];
	HeapTuple	newtup;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	domainTup;

	/* 由于此函数递归，可能会被驱动到栈溢出 */
	check_stack_depth();

	/* 更新当前类型的元组 */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(replaces, 0, sizeof(replaces));

	if (atparams->updateStorage)
	{
		replaces[Anum_pg_type_typstorage - 1] = true;
		values[Anum_pg_type_typstorage - 1] = CharGetDatum(atparams->storage);
	}
	if (atparams->updateReceive)
	{
		replaces[Anum_pg_type_typreceive - 1] = true;
		values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(atparams->receiveOid);
	}
	if (atparams->updateSend)
	{
		replaces[Anum_pg_type_typsend - 1] = true;
		values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(atparams->sendOid);
	}
	if (atparams->updateTypmodin)
	{
		replaces[Anum_pg_type_typmodin - 1] = true;
		values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(atparams->typmodinOid);
	}
	if (atparams->updateTypmodout)
	{
		replaces[Anum_pg_type_typmodout - 1] = true;
		values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(atparams->typmodoutOid);
	}
	if (atparams->updateAnalyze)
	{
		replaces[Anum_pg_type_typanalyze - 1] = true;
		values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(atparams->analyzeOid);
	}
	if (atparams->updateSubscript)
	{
		replaces[Anum_pg_type_typsubscript - 1] = true;
		values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(atparams->subscriptOid);
	}

	newtup = heap_modify_tuple(tup, RelationGetDescr(catalog),
							   values, nulls, replaces);

	CatalogTupleUpdate(catalog, &newtup->t_self, newtup);

	/* 重建此类型的依赖关系 */
	GenerateTypeDependencies(newtup,
							 catalog,
							 NULL,	/* 没有现成的 defaultExpr */
							 NULL,	/* 没有现成的 typacl */
							 0, /* 我们在上面拒绝了组合类型 */
							 isImplicitArray,	/* 它可能是一个数组 */
							 isImplicitArray,	/* 当它是数组时才有依赖 */
							 false, /* 不要触碰扩展成员关系 */
							 true);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	/*
	 * 数组继承其基础类型的 typmodin 和 typmodout，            
	 * 但不继承此处关注的其他属性。必要时递归到         
	 * 数组类型。
	 */
	if (!isImplicitArray &&
		(atparams->updateTypmodin || atparams->updateTypmodout))
	{
		Oid			arrtypoid = ((Form_pg_type) GETSTRUCT(newtup))->typarray;

		if (OidIsValid(arrtypoid))
		{
			HeapTuple	arrtup;
			AlterTypeRecurseParams arrparams;

			arrtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(arrtypoid));
			if (!HeapTupleIsValid(arrtup))
				elog(ERROR, "cache lookup failed for type %u", arrtypoid);

			memset(&arrparams, 0, sizeof(arrparams));
			arrparams.updateTypmodin = atparams->updateTypmodin;
			arrparams.updateTypmodout = atparams->updateTypmodout;
			arrparams.typmodinOid = atparams->typmodinOid;
			arrparams.typmodoutOid = atparams->typmodoutOid;

			AlterTypeRecurse(arrtypoid, true, arrtup, catalog, &arrparams);

			ReleaseSysCache(arrtup);
		}
	}

	/* 现在我们需要递归到域。但是，某些属性不被域继承，
	 * 因此清除这些属性的更新标志。
	 * 
	 */
	atparams->updateReceive = false;	/* 域使用 F_DOMAIN_RECV */
	atparams->updateTypmodin = false;	/* 域没有 typmod */
	atparams->updateTypmodout = false;
	atparams->updateSubscript = false;	/* 域没有下标器 */

	/* 如果没有需要做的事，跳过扫描 */
	if (!(atparams->updateStorage ||
		  atparams->updateSend ||
		  atparams->updateAnalyze))
		return;

	/* 在 pg_type 中搜索此类型上可能的域 */
	ScanKeyInit(&key[0],
				Anum_pg_type_typbasetype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = systable_beginscan(catalog, InvalidOid, false,
							  NULL, 1, key);

	while ((domainTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_type domainForm = (Form_pg_type) GETSTRUCT(domainTup);

		/*
		 * 非域类型中不应有非零的 typbasetype，但还是检查
		 *      
		 */
		if (domainForm->typtype != TYPTYPE_DOMAIN)
			continue;

		AlterTypeRecurse(domainForm->oid, false, domainTup, catalog, atparams);
	}

	systable_endscan(scan);
}
