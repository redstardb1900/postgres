/*-------------------------------------------------------------------------
 *
 * aggregatecmds.c
 *
 *	  操纵聚合的 SQL 命令的例程。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/aggregatecmds.c
 *
 * DESCRIPTION
 *	  "DefineFoo" 例程取出解析树并挑选出
 *	  适当的参数/标志，把结果传给相应的
 *	  "FooDefine" 例程（位于 src/catalog），
 *	  由后者完成真正的目录修改。这些例程
 *	  还会校验用户执行该命令的权限。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static char extractModify(DefElem *defel);


/*
 *	DefineAggregate
 *
 * "oldstyle" 表示旧式（8.2 之前）风格：聚合的输入类型
 * 由参数中的 BASETYPE 元素指定。否则，"args" 是一对，
 * 其第一个元素是 FunctionParameter 结构体列表，定义
 * 聚合的参数（直接参数与聚合参数皆含），第二个元素
 * 是表示直接参数个数的 Integer 节点，若这不是
 * 有序集聚合则为 -1。
 * "parameters" 是表示聚合定义子句的 DefElem 列表。
 */
ObjectAddress
DefineAggregate(ParseState *pstate,
				List *name,
				List *args,
				bool oldstyle,
				List *parameters,
				bool replace)
{
	char	   *aggName;
	Oid			aggNamespace;
	AclResult	aclresult;
	char		aggKind = AGGKIND_NORMAL;
	List	   *transfuncName = NIL;
	List	   *finalfuncName = NIL;
	List	   *combinefuncName = NIL;
	List	   *serialfuncName = NIL;
	List	   *deserialfuncName = NIL;
	List	   *mtransfuncName = NIL;
	List	   *minvtransfuncName = NIL;
	List	   *mfinalfuncName = NIL;
	bool		finalfuncExtraArgs = false;
	bool		mfinalfuncExtraArgs = false;
	char		finalfuncModify = 0;
	char		mfinalfuncModify = 0;
	List	   *sortoperatorName = NIL;
	TypeName   *baseType = NULL;
	TypeName   *transType = NULL;
	TypeName   *mtransType = NULL;
	int32		transSpace = 0;
	int32		mtransSpace = 0;
	char	   *initval = NULL;
	char	   *minitval = NULL;
	char	   *parallel = NULL;
	int			numArgs;
	int			numDirectArgs = 0;
	oidvector  *parameterTypes;
	ArrayType  *allParameterTypes;
	ArrayType  *parameterModes;
	ArrayType  *parameterNames;
	List	   *parameterDefaults;
	Oid			variadicArgType;
	Oid			transTypeId;
	Oid			mtransTypeId = InvalidOid;
	char		transTypeType;
	char		mtransTypeType = 0;
	char		proparallel = PROPARALLEL_UNSAFE;
	ListCell   *pl;

	/* 将名称列表转换为名称与命名空间 */
	aggNamespace = QualifiedNameGetCreationNamespace(name, &aggName);

	/* 检查我们在目标命名空间中是否有创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, aggNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(aggNamespace));

	/* 拆解 aggr_args 语法产生式的输出 */
	if (!oldstyle)
	{
		Assert(list_length(args) == 2);
		numDirectArgs = intVal(lsecond(args));
		if (numDirectArgs >= 0)
			aggKind = AGGKIND_ORDERED_SET;
		else
			numDirectArgs = 0;
		args = linitial_node(List, args);
	}

	/* 检查聚合的定义子句 */
	foreach(pl, parameters)
	{
		DefElem    *defel = lfirst_node(DefElem, pl);

		/*
		 * sfunc1、stype1 与 initcond1 被接受为过时拼写，
		 * 对应 sfunc、stype、initcond。
		 */
		if (strcmp(defel->defname, "sfunc") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "sfunc1") == 0)
			transfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "finalfunc") == 0)
			finalfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "combinefunc") == 0)
			combinefuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "serialfunc") == 0)
			serialfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "deserialfunc") == 0)
			deserialfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "msfunc") == 0)
			mtransfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "minvfunc") == 0)
			minvtransfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "mfinalfunc") == 0)
			mfinalfuncName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "finalfunc_extra") == 0)
			finalfuncExtraArgs = defGetBoolean(defel);
		else if (strcmp(defel->defname, "mfinalfunc_extra") == 0)
			mfinalfuncExtraArgs = defGetBoolean(defel);
		else if (strcmp(defel->defname, "finalfunc_modify") == 0)
			finalfuncModify = extractModify(defel);
		else if (strcmp(defel->defname, "mfinalfunc_modify") == 0)
			mfinalfuncModify = extractModify(defel);
		else if (strcmp(defel->defname, "sortop") == 0)
			sortoperatorName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "basetype") == 0)
			baseType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "hypothetical") == 0)
		{
			if (defGetBoolean(defel))
			{
				if (aggKind == AGGKIND_NORMAL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("only ordered-set aggregates can be hypothetical")));
				aggKind = AGGKIND_HYPOTHETICAL;
			}
		}
		else if (strcmp(defel->defname, "stype") == 0)
			transType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "stype1") == 0)
			transType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "sspace") == 0)
			transSpace = defGetInt32(defel);
		else if (strcmp(defel->defname, "mstype") == 0)
			mtransType = defGetTypeName(defel);
		else if (strcmp(defel->defname, "msspace") == 0)
			mtransSpace = defGetInt32(defel);
		else if (strcmp(defel->defname, "initcond") == 0)
			initval = defGetString(defel);
		else if (strcmp(defel->defname, "initcond1") == 0)
			initval = defGetString(defel);
		else if (strcmp(defel->defname, "minitcond") == 0)
			minitval = defGetString(defel);
		else if (strcmp(defel->defname, "parallel") == 0)
			parallel = defGetString(defel);
		else
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("aggregate attribute \"%s\" not recognized",
							defel->defname)));
	}

	/*
	 * 确保我们拥有必需的定义
	 */
	if (transType == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate stype must be specified")));
	if (transfuncName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("aggregate sfunc must be specified")));

	/*
	 * 若给出了 mtransType，则 mtransfuncName 与
	 * minvtransfuncName 也必须给出；若没有，则任何
	 * 移动聚合选项都不应被给出。
	 */
	if (mtransType != NULL)
	{
		if (mtransfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate msfunc must be specified when mstype is specified")));
		if (minvtransfuncName == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate minvfunc must be specified when mstype is specified")));
	}
	else
	{
		if (mtransfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate msfunc must not be specified without mstype")));
		if (minvtransfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate minvfunc must not be specified without mstype")));
		if (mfinalfuncName != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate mfinalfunc must not be specified without mstype")));
		if (mtransSpace != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate msspace must not be specified without mstype")));
		if (minitval != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate minitcond must not be specified without mstype")));
	}

	/*
	 * 只有知道 aggKind 后才能确定
	 * modify 标志的默认值。
	 */
	if (finalfuncModify == 0)
		finalfuncModify = (aggKind == AGGKIND_NORMAL) ? AGGMODIFY_READ_ONLY : AGGMODIFY_READ_WRITE;
	if (mfinalfuncModify == 0)
		mfinalfuncModify = (aggKind == AGGKIND_NORMAL) ? AGGMODIFY_READ_ONLY : AGGMODIFY_READ_WRITE;

	/*
	 * 查找聚合的输入数据类型。
	 */
	if (oldstyle)
	{
		/*
		 * 旧式：使用 basetype 参数。这支持零个或一个输入的聚合，
		 * 输入类型 ANY 表示零个输入。
		 *
		 * 历史上我们允许命令看起来像 basetype = 'ANY'，
		 * 因此必须对名称 ANY 做大小写不敏感的比较。唉。
		 */
		Oid			aggArgTypes[1];

		if (baseType == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate input type must be specified")));

		if (pg_strcasecmp(TypeNameToString(baseType), "ANY") == 0)
		{
			numArgs = 0;
			aggArgTypes[0] = InvalidOid;
		}
		else
		{
			numArgs = 1;
			aggArgTypes[0] = typenameTypeId(NULL, baseType);
		}
		parameterTypes = buildoidvector(aggArgTypes, numArgs);
		allParameterTypes = NULL;
		parameterModes = NULL;
		parameterNames = NULL;
		parameterDefaults = NIL;
		variadicArgType = InvalidOid;
	}
	else
	{
		/*
		 * 新式：args 是 FunctionParameters 列表（可能为零个）。
		 * 我们共用 functioncmds.c 的代码来处理它们。
		 */
		Oid			requiredResultType;

		if (baseType != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("basetype is redundant with aggregate input type specification")));

		numArgs = list_length(args);
		interpret_function_parameter_list(pstate,
										  args,
										  InvalidOid,
										  OBJECT_AGGREGATE,
										  &parameterTypes,
										  NULL,
										  &allParameterTypes,
										  &parameterModes,
										  &parameterNames,
										  NULL,
										  &parameterDefaults,
										  &variadicArgType,
										  &requiredResultType);
		/* 语法目前不允许参数默认值 */
		Assert(parameterDefaults == NIL);
		/* 也不应该有任何 OUT 参数 */
		Assert(requiredResultType == InvalidOid);
	}

	/*
	 * 查找聚合的 transtype。
	 *
	 * transtype 不能是伪类型，因为我们需要能够
	 * 存储 transtype 类型的值。但在某些情况下可以
	 * 允许多态 transtype（AggregateCreate 会检查）。
	 * 另外，对于想要传递指向私有数据结构指针的函数，
	 * 我们允许 "internal"；但只对超级用户开放，因为把
	 * 不兼容的使用 internal 类型的函数连接到聚合中，
	 * 可能让系统崩溃（或更糟）。
	 */
	transTypeId = typenameTypeId(NULL, transType);
	transTypeType = get_typtype(transTypeId);
	if (transTypeType == TYPTYPE_PSEUDO &&
		!IsPolymorphicType(transTypeId))
	{
		if (transTypeId == INTERNALOID && superuser())
			 /* 可以 */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("aggregate transition data type cannot be %s",
							format_type_be(transTypeId))));
	}

	if (serialfuncName && deserialfuncName)
	{
		/*
		 * 序列化仅对 INTERNAL 类型的 transtype 需要/允许。
		 */
		if (transTypeId != INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("serialization functions may be specified only when the aggregate transition data type is %s",
							format_type_be(INTERNALOID))));
	}
	else if (serialfuncName || deserialfuncName)
	{
		/*
		 * 无法只指定其中一个函数而不指定另一个。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("must specify both or neither of serialization and deserialization functions")));
	}

	/*
	 * 若指定了移动聚合的 transtype，就查找它。
	 * 限制与 transtype 相同。
	 */
	if (mtransType)
	{
		mtransTypeId = typenameTypeId(NULL, mtransType);
		mtransTypeType = get_typtype(mtransTypeId);
		if (mtransTypeType == TYPTYPE_PSEUDO &&
			!IsPolymorphicType(mtransTypeId))
		{
			if (mtransTypeId == INTERNALOID && superuser())
				 /* 可以 */ ;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate transition data type cannot be %s",
								format_type_be(mtransTypeId))));
		}
	}

	/*
	 * 若我们有 initval，且它不用于伪类型
	 * （尤其是多态类型），确保它能为该类型的
	 * 输入函数接受。我们会把 initval 存为文本，
	 * 因为输入函数未必不可变（想想 timestamp 的
	 * "now"），而且我们希望使用值的运行时解释
	 * 而非创建时解释。不过，若值不正确，在
	 * CREATE AGGREGATE 时就报错似乎更友好。
	 */
	if (initval && transTypeType != TYPTYPE_PSEUDO)
	{
		Oid			typinput,
					typioparam;

		getTypeInputInfo(transTypeId, &typinput, &typioparam);
		(void) OidInputFunctionCall(typinput, initval, typioparam, -1);
	}

	/*
	 * 移动聚合的 initval 同理。
	 */
	if (minitval && mtransTypeType != TYPTYPE_PSEUDO)
	{
		Oid			typinput,
					typioparam;

		getTypeInputInfo(mtransTypeId, &typinput, &typioparam);
		(void) OidInputFunctionCall(typinput, minitval, typioparam, -1);
	}

	if (parallel)
	{
		if (strcmp(parallel, "safe") == 0)
			proparallel = PROPARALLEL_SAFE;
		else if (strcmp(parallel, "restricted") == 0)
			proparallel = PROPARALLEL_RESTRICTED;
		else if (strcmp(parallel, "unsafe") == 0)
			proparallel = PROPARALLEL_UNSAFE;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("parameter \"parallel\" must be SAFE, RESTRICTED, or UNSAFE")));
	}

	/*
	 * 大部分参数检查在 AggregateCreate 内部完成
	 */
	return AggregateCreate(aggName, /* 聚合名 */
						   aggNamespace,	/* 命名空间 */
						   replace,
						   aggKind,
						   numArgs,
						   numDirectArgs,
						   parameterTypes,
						   PointerGetDatum(allParameterTypes),
						   PointerGetDatum(parameterModes),
						   PointerGetDatum(parameterNames),
						   parameterDefaults,
						   variadicArgType,
						   transfuncName,	/* 转移函数名 */
						   finalfuncName,	/* 最终函数名 */
						   combinefuncName, /* 组合函数名 */
						   serialfuncName,	/* 序列化函数名 */
						   deserialfuncName,	/* 反序列化函数名 */
						   mtransfuncName,	/* 正向转移函数名 */
						   minvtransfuncName,	/* 逆向转移函数名 */
						   mfinalfuncName,	/* 最终函数名 */
						   finalfuncExtraArgs,
						   mfinalfuncExtraArgs,
						   finalfuncModify,
						   mfinalfuncModify,
						   sortoperatorName,	/* 排序操作符名 */
						   transTypeId, /* 转移数据类型 */
						   transSpace,	/* 转移空间 */
						   mtransTypeId,	/* 转移数据类型 */
						   mtransSpace, /* 转移空间 */
						   initval, /* 初始条件 */
						   minitval,	/* 初始条件 */
						   proparallel);	/* 并行安全？ */
}

/*
 * 把 [m]finalfunc_modify 的字符串形式转换为目录表示
 */
static char
extractModify(DefElem *defel)
{
	char	   *val = defGetString(defel);

	if (strcmp(val, "read_only") == 0)
		return AGGMODIFY_READ_ONLY;
	if (strcmp(val, "shareable") == 0)
		return AGGMODIFY_SHAREABLE;
	if (strcmp(val, "read_write") == 0)
		return AGGMODIFY_READ_WRITE;
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("parameter \"%s\" must be READ_ONLY, SHAREABLE, or READ_WRITE",
					defel->defname)));
	return 0;					/* 让编译器安静 */
}
