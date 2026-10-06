/*-------------------------------------------------------------------------
 *
 * pg_type.c
 *	  支持操作 pg_type 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_type.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/typecmds.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/scansup.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* 可能由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_pg_type_oid = InvalidOid;

/* ----------------------------------------------------------------
 *		TypeShellMake
 *
 *		本过程向 pg_type 关系插入一个"外壳"元组。
 *		插入的类型元组具有有效但哑的值，其
 *		"typisdefined" 字段为 false，表明它尚未真正定义。
 *
 *		这样做是为了在系统目录中存在一个元组。该类型的 I/O
 *		函数将链接到此元组。当发出完整的
 *		CREATE TYPE 命令时，这些虚假值将被替换为
 *		正确值，并且"typisdefined" 将被设为 true。
 * ----------------------------------------------------------------
 */
ObjectAddress
TypeShellMake(const char *typeName, Oid typeNamespace, Oid ownerId)
{
	Relation	pg_type_desc;
	TupleDesc	tupDesc;
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_type];
	bool		nulls[Natts_pg_type];
	Oid			typoid;
	NameData	name;
	ObjectAddress address;

	Assert(PointerIsValid(typeName));

	/*
	 * 打开 pg_type
	 */
	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);
	tupDesc = pg_type_desc->rd_att;

	/*
	 * 初始化我们的 *nulls 与 *values 数组
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;	/* 冗余，但安全 */
	}

	/*
	 * 用类型名与哑值初始化 *values
	 *
	 * 表示细节与 int4 相同……只要它们一致，
	 * 具体是什么其实无关紧要。另请注意我们
	 * 给它 typtype = TYPTYPE_PSEUDO，作为额外保险，免得它被
	 * 误认为可用类型。
	 */
	namestrcpy(&name, typeName);
	values[Anum_pg_type_typname - 1] = NameGetDatum(&name);
	values[Anum_pg_type_typnamespace - 1] = ObjectIdGetDatum(typeNamespace);
	values[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_type_typlen - 1] = Int16GetDatum(sizeof(int32));
	values[Anum_pg_type_typbyval - 1] = BoolGetDatum(true);
	values[Anum_pg_type_typtype - 1] = CharGetDatum(TYPTYPE_PSEUDO);
	values[Anum_pg_type_typcategory - 1] = CharGetDatum(TYPCATEGORY_PSEUDOTYPE);
	values[Anum_pg_type_typispreferred - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typisdefined - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typdelim - 1] = CharGetDatum(DEFAULT_TYPDELIM);
	values[Anum_pg_type_typrelid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typelem - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typarray - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typinput - 1] = ObjectIdGetDatum(F_SHELL_IN);
	values[Anum_pg_type_typoutput - 1] = ObjectIdGetDatum(F_SHELL_OUT);
	values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typalign - 1] = CharGetDatum(TYPALIGN_INT);
	values[Anum_pg_type_typstorage - 1] = CharGetDatum(TYPSTORAGE_PLAIN);
	values[Anum_pg_type_typnotnull - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typbasetype - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typtypmod - 1] = Int32GetDatum(-1);
	values[Anum_pg_type_typndims - 1] = Int32GetDatum(0);
	values[Anum_pg_type_typcollation - 1] = ObjectIdGetDatum(InvalidOid);
	nulls[Anum_pg_type_typdefaultbin - 1] = true;
	nulls[Anum_pg_type_typdefault - 1] = true;
	nulls[Anum_pg_type_typacl - 1] = true;

	/* 对 pg_type.oid 使用二进制升级覆盖？ */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type OID value not set when in binary upgrade mode")));

		typoid = binary_upgrade_next_pg_type_oid;
		binary_upgrade_next_pg_type_oid = InvalidOid;
	}
	else
	{
		typoid = GetNewOidWithIndex(pg_type_desc, TypeOidIndexId,
									Anum_pg_type_oid);
	}

	values[Anum_pg_type_oid - 1] = ObjectIdGetDatum(typoid);

	/*
	 * 创建新的类型元组
	 */
	tup = heap_form_tuple(tupDesc, values, nulls);

	/*
	 * 把元组插入关系并取得该元组的 oid。
	 */
	CatalogTupleInsert(pg_type_desc, tup);

	/*
	 * 创建依赖。在引导模式下我们可/必须跳过这一步。
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(tup,
								 pg_type_desc,
								 NULL,
								 NULL,
								 0,
								 false,
								 false,
								 true,	/* 建立扩展依赖 */
								 false);

	/* 新外壳类型的创建后钩子 */
	InvokeObjectPostCreateHook(TypeRelationId, typoid, 0);

	ObjectAddressSet(address, TypeRelationId, typoid);

	/*
	 * 清理并返回 type-oid
	 */
	heap_freetuple(tup);
	table_close(pg_type_desc, RowExclusiveLock);

	return address;
}

/* ----------------------------------------------------------------
 *		TypeCreate
 *
 *		完成定义新类型所需的全部工作。
 *
 *		返回赋给新类型的 ObjectAddress。
 *		若 newTypeOid 为零（正常情形），则创建新 OID；
 *		否则我们精确使用该 OID。
 * ----------------------------------------------------------------
 */
ObjectAddress
TypeCreate(Oid newTypeOid,
		   const char *typeName,
		   Oid typeNamespace,
		   Oid relationOid,		/* 仅用于关系行类型 */
		   char relationKind,	/* 同上 */
		   Oid ownerId,
		   int16 internalSize,
		   char typeType,
		   char typeCategory,
		   bool typePreferred,
		   char typDelim,
		   Oid inputProcedure,
		   Oid outputProcedure,
		   Oid receiveProcedure,
		   Oid sendProcedure,
		   Oid typmodinProcedure,
		   Oid typmodoutProcedure,
		   Oid analyzeProcedure,
		   Oid subscriptProcedure,
		   Oid elementType,
		   bool isImplicitArray,
		   Oid arrayType,
		   Oid baseType,
		   const char *defaultTypeValue,	/* 人类可读表示 */
		   char *defaultTypeBin,	/* 已加工表示 */
		   bool passedByValue,
		   char alignment,
		   char storage,
		   int32 typeMod,
		   int32 typNDims,		/* baseType 的数组维度 */
		   bool typeNotNull,
		   Oid typeCollation)
{
	Relation	pg_type_desc;
	Oid			typeObjectId;
	bool		isDependentType;
	bool		rebuildDeps = false;
	Acl		   *typacl;
	HeapTuple	tup;
	bool		nulls[Natts_pg_type];
	bool		replaces[Natts_pg_type];
	Datum		values[Natts_pg_type];
	NameData	name;
	int			i;
	ObjectAddress address;

	/*
	 * 我们假定调用方已分别校验了各参数，但没有
	 * 检查不良组合。
	 *
	 * 校验大小规格：要么为正（定长），要么为 -1
	 * （varlena），要么为 -2（cstring）。
	 */
	if (!(internalSize > 0 ||
		  internalSize == -1 ||
		  internalSize == -2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal size %d",
						internalSize)));

	if (passedByValue)
	{
		/*
		 * 按值传递的类型必须有固定长度，且为
		 * fetch_att() 与 store_att_byval() 所支持的值之一；并且
		 * 对齐也必须一致。所有这些代码必须与
		 * access/tupmacs.h 匹配！
		 */
		if (internalSize == (int16) sizeof(char))
		{
			if (alignment != TYPALIGN_CHAR)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
		else if (internalSize == (int16) sizeof(int16))
		{
			if (alignment != TYPALIGN_SHORT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
		else if (internalSize == (int16) sizeof(int32))
		{
			if (alignment != TYPALIGN_INT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
#if SIZEOF_DATUM == 8
		else if (internalSize == (int16) sizeof(Datum))
		{
			if (alignment != TYPALIGN_DOUBLE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
#endif
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("internal size %d is invalid for passed-by-value type",
							internalSize)));
	}
	else
	{
		/* varlena 类型必须有 int 对齐或更好 */
		if (internalSize == -1 &&
			!(alignment == TYPALIGN_INT || alignment == TYPALIGN_DOUBLE))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("alignment \"%c\" is invalid for variable-length type",
							alignment)));
		/* cstring 必须有 char 对齐 */
		if (internalSize == -2 && !(alignment == TYPALIGN_CHAR))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("alignment \"%c\" is invalid for variable-length type",
							alignment)));
	}

	/* 只有 varlena 类型可以被 toast */
	if (storage != TYPSTORAGE_PLAIN && internalSize != -1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("fixed-size types must have storage PLAIN")));

	/*
	 * 若它是隐式创建的数组类型，或
	 * 是非复合类型的关系行类型，则为依赖类型。对此类类型
	 * 我们将让 ACL 为空，并跳过创建某些依赖
	 * 记录，因为通过被依赖的类型或关系已有依赖。
	 * （注意：这与 GenerateTypeDependencies 中的某些行为
	 * 紧密交织。）
	 */
	isDependentType = isImplicitArray ||
		(OidIsValid(relationOid) && relationKind != RELKIND_COMPOSITE_TYPE);

	/*
	 * 初始化 heap_form_tuple 或 heap_modify_tuple 所需的数组
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = false;
		replaces[i] = true;
		values[i] = (Datum) 0;
	}

	/*
	 * 插入数据值
	 */
	namestrcpy(&name, typeName);
	values[Anum_pg_type_typname - 1] = NameGetDatum(&name);
	values[Anum_pg_type_typnamespace - 1] = ObjectIdGetDatum(typeNamespace);
	values[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_type_typlen - 1] = Int16GetDatum(internalSize);
	values[Anum_pg_type_typbyval - 1] = BoolGetDatum(passedByValue);
	values[Anum_pg_type_typtype - 1] = CharGetDatum(typeType);
	values[Anum_pg_type_typcategory - 1] = CharGetDatum(typeCategory);
	values[Anum_pg_type_typispreferred - 1] = BoolGetDatum(typePreferred);
	values[Anum_pg_type_typisdefined - 1] = BoolGetDatum(true);
	values[Anum_pg_type_typdelim - 1] = CharGetDatum(typDelim);
	values[Anum_pg_type_typrelid - 1] = ObjectIdGetDatum(relationOid);
	values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(subscriptProcedure);
	values[Anum_pg_type_typelem - 1] = ObjectIdGetDatum(elementType);
	values[Anum_pg_type_typarray - 1] = ObjectIdGetDatum(arrayType);
	values[Anum_pg_type_typinput - 1] = ObjectIdGetDatum(inputProcedure);
	values[Anum_pg_type_typoutput - 1] = ObjectIdGetDatum(outputProcedure);
	values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(receiveProcedure);
	values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(sendProcedure);
	values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(typmodinProcedure);
	values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(typmodoutProcedure);
	values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(analyzeProcedure);
	values[Anum_pg_type_typalign - 1] = CharGetDatum(alignment);
	values[Anum_pg_type_typstorage - 1] = CharGetDatum(storage);
	values[Anum_pg_type_typnotnull - 1] = BoolGetDatum(typeNotNull);
	values[Anum_pg_type_typbasetype - 1] = ObjectIdGetDatum(baseType);
	values[Anum_pg_type_typtypmod - 1] = Int32GetDatum(typeMod);
	values[Anum_pg_type_typndims - 1] = Int32GetDatum(typNDims);
	values[Anum_pg_type_typcollation - 1] = ObjectIdGetDatum(typeCollation);

	/*
	 * 初始化此类型的默认二进制值。当然要
	 * 检查空值。
	 */
	if (defaultTypeBin)
		values[Anum_pg_type_typdefaultbin - 1] = CStringGetTextDatum(defaultTypeBin);
	else
		nulls[Anum_pg_type_typdefaultbin - 1] = true;

	/*
	 * 初始化此类型的默认值。
	 */
	if (defaultTypeValue)
		values[Anum_pg_type_typdefault - 1] = CStringGetTextDatum(defaultTypeValue);
	else
		nulls[Anum_pg_type_typdefault - 1] = true;

	/*
	 * 也初始化该类型的 ACL。但依赖类型没有 ACL。
	 */
	if (isDependentType)
		typacl = NULL;
	else
		typacl = get_user_default_acl(OBJECT_TYPE, ownerId,
									  typeNamespace);
	if (typacl != NULL)
		values[Anum_pg_type_typacl - 1] = PointerGetDatum(typacl);
	else
		nulls[Anum_pg_type_typacl - 1] = true;

	/*
	 * 打开 pg_type 并准备插入或更新一行。
	 *
	 * 注意：在引导模式下更新不能正确工作；但我们不
	 * 期望在引导模式下覆盖任何外壳类型。
	 */
	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(TYPENAMENSP,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_type typform = (Form_pg_type) GETSTRUCT(tup);

		/*
		 * 检查该类型尚未定义。不过它可能作为
		 * 外壳类型存在。
		 */
		if (typform->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		/*
		 * 外壳类型必须由同一所有者创建
		 */
		if (typform->typowner != ownerId)
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TYPE, typeName);

		/* 若调用方想强制 OID 则会出问题 */
		if (OidIsValid(newTypeOid))
			elog(ERROR, "cannot assign new OID to existing shell type");

		replaces[Anum_pg_type_oid - 1] = false;

		/*
		 * 可以更新已有的外壳类型元组了
		 */
		tup = heap_modify_tuple(tup,
								RelationGetDescr(pg_type_desc),
								values,
								nulls,
								replaces);

		CatalogTupleUpdate(pg_type_desc, &tup->t_self, tup);

		typeObjectId = typform->oid;

		rebuildDeps = true;		/* 去掉外壳类型的依赖 */
	}
	else
	{
		/* 若调用方请求则强制 OID */
		if (OidIsValid(newTypeOid))
			typeObjectId = newTypeOid;
		/* 若提供了，则对 pg_type.oid 使用二进制升级覆盖。 */
		else if (IsBinaryUpgrade)
		{
			if (!OidIsValid(binary_upgrade_next_pg_type_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("pg_type OID value not set when in binary upgrade mode")));

			typeObjectId = binary_upgrade_next_pg_type_oid;
			binary_upgrade_next_pg_type_oid = InvalidOid;
		}
		else
		{
			typeObjectId = GetNewOidWithIndex(pg_type_desc, TypeOidIndexId,
											  Anum_pg_type_oid);
		}

		values[Anum_pg_type_oid - 1] = ObjectIdGetDatum(typeObjectId);

		tup = heap_form_tuple(RelationGetDescr(pg_type_desc),
							  values, nulls);

		CatalogTupleInsert(pg_type_desc, tup);
	}

	/*
	 * 创建依赖。在引导模式下我们可/必须跳过这一步。
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(tup,
								 pg_type_desc,
								 (defaultTypeBin ?
								  stringToNode(defaultTypeBin) :
								  NULL),
								 typacl,
								 relationKind,
								 isImplicitArray,
								 isDependentType,
								 true,	/* 建立扩展依赖 */
								 rebuildDeps);

	/* 新类型的创建后钩子 */
	InvokeObjectPostCreateHook(TypeRelationId, typeObjectId, 0);

	ObjectAddressSet(address, TypeRelationId, typeObjectId);

	/*
	 * 收尾
	 */
	table_close(pg_type_desc, RowExclusiveLock);

	return address;
}

/*
 * GenerateTypeDependencies: 构建类型所需的依赖
 *
 * 本函数需要了解的关于该类型的大部分信息通过
 * 新的 pg_type 行 typeTuple 传入。我们还让调用方传入
 * pg_type Relation，以便我们能方便地取得该行的元组描述符。
 *
 * 虽然本函数能从元组中提取 defaultExpr 与 typacl，
 * 但这样做相对昂贵，且调用方可能手头已有这些值。
 * 若方便则传入，否则传 NULL。（typacl 实际上是
 * "Acl *"，但我们把它声明为 "void *"，以免在
 * pg_type.h 中包含 acl.h。）
 *
 * relationKind 与 isImplicitArray 从元组推导也相对昂贵，
 * 因此我们让调用方传入它们（它们不是可选的）。
 *
 * 若这是隐式数组或关系行类型，则 isDependentType 为真；
 * 这意味着它不需要自己对所有者等的依赖。
 *
 * 若我们在扩展脚本中且 makeExtensionDep 为真（且
 * isDependentType 不为真），则建立扩展成员依赖。
 * 创建新类型或替换外壳类型时应使 makeExtensionDep 为真，
 * 但对已有类型的 ALTER TYPE 则否。传入 false
 * 会使该类型的扩展成员关系保持不动。
 *
 * 若这是预先存在的类型，rebuild 应为真。我们将移除
 * 已有依赖并从零重建。这对 ALTER TYPE 以及替换外壳类型
 * 是需要的。不过我们不移除任何已有扩展依赖；因此，若
 * makeExtensionDep 也为真且我们在扩展脚本中，除非该类型
 * 已属于当前扩展，否则会发生错误。这正是我们在替换
 * 外壳类型时所要的行为，而那也是两个标志都为真的
 * 唯一情形。
 */
void
GenerateTypeDependencies(HeapTuple typeTuple,
						 Relation typeCatalog,
						 Node *defaultExpr,
						 void *typacl,
						 char relationKind, /* 仅用于关系行类型 */
						 bool isImplicitArray,
						 bool isDependentType,
						 bool makeExtensionDep,
						 bool rebuild)
{
	Form_pg_type typeForm = (Form_pg_type) GETSTRUCT(typeTuple);
	Oid			typeObjectId = typeForm->oid;
	Datum		datum;
	bool		isNull;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs_normal;

	/* 若调用方未传入则提取 defaultExpr */
	if (defaultExpr == NULL)
	{
		datum = heap_getattr(typeTuple, Anum_pg_type_typdefaultbin,
							 RelationGetDescr(typeCatalog), &isNull);
		if (!isNull)
			defaultExpr = stringToNode(TextDatumGetCString(datum));
	}
	/* 若调用方未传入则提取 typacl */
	if (typacl == NULL)
	{
		datum = heap_getattr(typeTuple, Anum_pg_type_typacl,
							 RelationGetDescr(typeCatalog), &isNull);
		if (!isNull)
			typacl = DatumGetAclPCopy(datum);
	}

	/* 若 rebuild，先冲刷旧依赖，扩展依赖除外 */
	if (rebuild)
	{
		deleteDependencyRecordsFor(TypeRelationId, typeObjectId, true);
		deleteSharedDependencyRecordsFor(TypeRelationId, typeObjectId, 0);
	}

	ObjectAddressSet(myself, TypeRelationId, typeObjectId);

	/*
	 * 建立对命名空间、所有者、ACL、扩展的依赖。
	 *
	 * 对依赖类型跳过这些，因为它会通过其被依赖的
	 * 类型或关系间接拥有此类依赖。
	 */

	/* 所有正常依赖的占位 */
	addrs_normal = new_object_addresses();

	if (!isDependentType)
	{
		ObjectAddressSet(referenced, NamespaceRelationId,
						 typeForm->typnamespace);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		recordDependencyOnOwner(TypeRelationId, typeObjectId,
								typeForm->typowner);

		recordDependencyOnNewAcl(TypeRelationId, typeObjectId, 0,
								 typeForm->typowner, typacl);

		if (makeExtensionDep)
			recordDependencyOnCurrentExtension(&myself, rebuild);
	}

	/* 对 I/O 与支持函数的正常依赖 */
	if (OidIsValid(typeForm->typinput))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typinput);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typoutput))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typoutput);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typreceive))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typreceive);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typsend))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typsend);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typmodin))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typmodin);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typmodout))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typmodout);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typanalyze))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typanalyze);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typsubscript))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typsubscript);
		add_exact_object_address(&referenced, addrs_normal);
	}

	/* 从域到其基类型的正常依赖。 */
	if (OidIsValid(typeForm->typbasetype))
	{
		ObjectAddressSet(referenced, TypeRelationId, typeForm->typbasetype);
		add_exact_object_address(&referenced, addrs_normal);
	}

	/*
	 * 从域到其排序规则的正常依赖。我们知道默认
	 * 排序规则是钉住的，因此不必记录它。
	 */
	if (OidIsValid(typeForm->typcollation) &&
		typeForm->typcollation != DEFAULT_COLLATION_OID)
	{
		ObjectAddressSet(referenced, CollationRelationId, typeForm->typcollation);
		add_exact_object_address(&referenced, addrs_normal);
	}

	record_object_address_dependencies(&myself, addrs_normal, DEPENDENCY_NORMAL);
	free_object_addresses(addrs_normal);

	/* 对默认表达式的正常依赖。 */
	if (defaultExpr)
		recordDependencyOnExpr(&myself, defaultExpr, NIL, DEPENDENCY_NORMAL);

	/*
	 * 若该类型是关系的行类型，则将其标记为在内部
	 * 依赖于该关系，*除非* 它是独立的复合类型
	 * 关系。对后一种情形，我们必须反转依赖。
	 *
	 * 在前一种情形下，这允许在关系被删除时类型被
	 * 自动删除，否则则否。而在后一种情形下，当然我们得到
	 * 相反的效果。
	 */
	if (OidIsValid(typeForm->typrelid))
	{
		ObjectAddressSet(referenced, RelationRelationId, typeForm->typrelid);

		if (relationKind != RELKIND_COMPOSITE_TYPE)
			recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
		else
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * 若该类型是隐式创建的数组类型，则将其标记为在内部
	 * 依赖于元素类型。否则，若它有元素类型，
	 * 则依赖为正常依赖。
	 */
	if (OidIsValid(typeForm->typelem))
	{
		ObjectAddressSet(referenced, TypeRelationId, typeForm->typelem);
		recordDependencyOn(&myself, &referenced,
						   isImplicitArray ? DEPENDENCY_INTERNAL : DEPENDENCY_NORMAL);
	}
}

/*
 * RenameTypeInternal
 *		重命名类型，以及任何关联的数组类型。
 *
 * 调用方必须已经检查过权限。
 *
 * 目前这用于重命名表行类型以及
 * ALTER TYPE RENAME TO 命令。
 */
void
RenameTypeInternal(Oid typeOid, const char *newTypeName, Oid typeNamespace)
{
	Relation	pg_type_desc;
	HeapTuple	tuple;
	Form_pg_type typ;
	Oid			arrayOid;
	Oid			oldTypeOid;

	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typ = (Form_pg_type) GETSTRUCT(tuple);

	/* 我们不应在这里更改模式 */
	Assert(typeNamespace == typ->typnamespace);

	arrayOid = typ->typarray;

	/* 检查是否有冲突的类型名。 */
	oldTypeOid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								 CStringGetDatum(newTypeName),
								 ObjectIdGetDatum(typeNamespace));

	/*
	 * 若有，看它是否是自动生成的数组类型，若是则
	 * 把它重命名到一边。（但对外壳类型必须跳过，
	 * 因为那种情况下 moveArrayTypeName 会做错事。）
	 * 否则，我们至少可以给出比唯一索引冲突更友好的
	 * 错误。
	 */
	if (OidIsValid(oldTypeOid))
	{
		if (get_typisdefined(oldTypeOid) &&
			moveArrayTypeName(oldTypeOid, newTypeName, typeNamespace))
			 /* 成功避开了问题 */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", newTypeName)));
	}

	/* 好，执行重命名 --- 元组是副本，涂改它没问题 */
	namestrcpy(&(typ->typname), newTypeName);

	CatalogTupleUpdate(pg_type_desc, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tuple);
	table_close(pg_type_desc, RowExclusiveLock);

	/*
	 * 若该类型有数组类型，则递归处理。但若我们上面已经
	 * 重命名了该数组类型，则无需再做任何事
	 * （例如把 "foo" 重命名为 "_foo" 时会发生）。
	 */
	if (OidIsValid(arrayOid) && arrayOid != oldTypeOid)
	{
		char	   *arrname = makeArrayTypeName(newTypeName, typeNamespace);

		RenameTypeInternal(arrayOid, arrname, typeNamespace);
		pfree(arrname);
	}
}


/*
 * makeArrayTypeName
 *	  - 给定基类型名，为其生成数组类型名
 *
 * 调用方负责 pfree 结果
 */
char *
makeArrayTypeName(const char *typeName, Oid typeNamespace)
{
	char	   *arr_name;
	int			pass = 0;
	char		suffix[NAMEDATALEN];

	/*
	 * 按古老的 Postgres 传统，数组类型名通过在基类型名前
	 * 加下划线生成。许多客户端代码知道这一约定，因此不要
	 * 乱动。不过，在结果名过长或与已有名冲突的边角情形下，
	 * 传统就不那么清楚了。我们当前的规则是 (1) 按需从右侧
	 * 截断基名，以及 (2) 若有冲突，再追加一个下划线和一些
	 * 选定的数字以使其唯一。这与
	 * ChooseRelationName() 的做法
	 * 类似。
	 *
	 * 实际的名称生成可通过给 makeObjectName() 一个空的第一
	 * 名称组件来外包出去。
	 */

	/* 首先尝试不带数字后缀 */
	arr_name = makeObjectName("", typeName, NULL);

	for (;;)
	{
		if (!SearchSysCacheExists2(TYPENAMENSP,
								   CStringGetDatum(arr_name),
								   ObjectIdGetDatum(typeNamespace)))
			break;

		/* 该尝试冲突了。准备一个带一些数字的新名。 */
		pfree(arr_name);
		snprintf(suffix, sizeof(suffix), "%d", ++pass);
		arr_name = makeObjectName("", typeName, suffix);
	}

	return arr_name;
}


/*
 * moveArrayTypeName
 *	  - 尝试重新分配用户想要使用的数组类型名。
 *
 * 已发现给定类型名已存在（具有给定
 * OID）。若它是自动生成的数组类型，则更改该数组类型的名称
 * 以免冲突。这允许用户先创建类型 "foo" 再创建
 * 类型 "_foo" 而不出问题。（当然，若两个后端并发尝试创建
 * 相似名称的类型，会有竞态条件，但最坏情况只是
 * 不必要的失败 --- 若类型创建因名称冲突而失败，
 * 我们在这里做的任何事都会被回滚。）
 *
 * 注意必须在调用 makeArrayTypeName 以确定
 * 新类型自己的数组类型名 *之前* 调用本函数；否则后者
 * 肯定会挑同一个名字。
 *
 * 成功移动类型则返回 true，否则返回 false。
 *
 * 若给定类型是外壳类型，我们也返回 true。这种情况下
 * 类型尚未被重命名到一边，但仍可预期
 * TypeCreate 会成功。这对大多数调用方很方便 ---
 * 需要区分外壳类型情形的调用方必须自己做
 * typisdefined 测试。
 */
bool
moveArrayTypeName(Oid typeOid, const char *typeName, Oid typeNamespace)
{
	Oid			elemOid;
	char	   *newname;

	/* 若是外壳类型则无需做任何事。 */
	if (!get_typisdefined(typeOid))
		return true;

	/* 若不是自动生成的数组类型则不能更改。 */
	elemOid = get_element_type(typeOid);
	if (!OidIsValid(elemOid) ||
		get_array_type(elemOid) != typeOid)
		return false;

	/*
	 * 好，用 makeArrayTypeName 挑一个未使用的名称变体。
	 * 注意由于 makeArrayTypeName 是迭代过程，这将
	 * 产生一个若我们即将创建的冲突类型已存在时
	 * 它第一次就可能产生的名字。
	 */
	newname = makeArrayTypeName(typeName, typeNamespace);

	/* 应用重命名 */
	RenameTypeInternal(typeOid, newname, typeNamespace);

	/*
	 * 我们必须推进命令计数器，以便后续使用
	 * makeArrayTypeName 时能看到我们刚做的事，不会挑同一个名字。
	 */
	CommandCounterIncrement();

	pfree(newname);

	return true;
}


/*
 * makeMultirangeTypeName
 *	  - 给定范围类型名，为其生成多范围类型名
 *
 * 调用方负责 pfree 结果
 */
char *
makeMultirangeTypeName(const char *rangeTypeName, Oid typeNamespace)
{
	char	   *buf;
	char	   *rangestr;

	/*
	 * 若范围类型名包含 "range" 则将其改为
	 * "multirange"。否则在末尾添加 "_multirange"。
	 */
	rangestr = strstr(rangeTypeName, "range");
	if (rangestr)
	{
		char	   *prefix = pnstrdup(rangeTypeName, rangestr - rangeTypeName);

		buf = psprintf("%s%s%s", prefix, "multi", rangestr);
	}
	else
		buf = psprintf("%s_multirange", pnstrdup(rangeTypeName, NAMEDATALEN - 12));

	/* 在 NAMEDATALEN-1 字节处截断 */
	buf[pg_mbcliplen(buf, strlen(buf), NAMEDATALEN - 1)] = '\0';

	if (SearchSysCacheExists2(TYPENAMENSP,
							  CStringGetDatum(buf),
							  ObjectIdGetDatum(typeNamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" already exists", buf),
				 errdetail("Failed while creating a multirange type for type \"%s\".", rangeTypeName),
				 errhint("You can manually specify a multirange type name using the \"multirange_type_name\" attribute.")));

	return pstrdup(buf);
}
