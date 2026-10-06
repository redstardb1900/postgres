/*-------------------------------------------------------------------------
 *
 * pg_operator.c
 *	  支持操作 pg_operator 关系的例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_operator.c
 *
 * NOTES
 *	  这些例程从 commands/define.c 移至此处并做了一定清理。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static Oid	OperatorGet(const char *operatorName,
						Oid operatorNamespace,
						Oid leftObjectId,
						Oid rightObjectId,
						bool *defined);

static Oid	OperatorLookup(List *operatorName,
						   Oid leftObjectId,
						   Oid rightObjectId,
						   bool *defined);

static Oid	OperatorShellMake(const char *operatorName,
							  Oid operatorNamespace,
							  Oid leftTypeId,
							  Oid rightTypeId);

static Oid	get_other_operator(List *otherOp,
							   Oid otherLeftTypeId, Oid otherRightTypeId,
							   const char *operatorName, Oid operatorNamespace,
							   Oid leftTypeId, Oid rightTypeId,
							   bool isCommutator);


/*
 * 检查提议的操作符名称是否合法
 *
 * 这最好与 parser/scan.l 的行为一致！
 *
 * 需要本检查是因为解析器不够聪明，无法确认
 * CREATE OPERATOR 的 COMMUTATOR、NEGATOR 等子句的参数
 * 是操作符名，而非其他词法实体。
 */
static bool
validOperatorName(const char *name)
{
	size_t		len = strlen(name);

	/* 不能为空或过长 */
	if (len == 0 || len >= NAMEDATALEN)
		return false;

	/* 不能包含任何无效字符 */
	/* 此处测试字符串应与 scan.l 中的 op_chars 一致 */
	if (strspn(name, "~!@#^&|`?+-*/%<>=") != len)
		return false;

	/* 不能包含斜杠-星号或短划线-短划线（注释起始） */
	if (strstr(name, "/*") || strstr(name, "--"))
		return false;

	/*
	 * 为与 SQL 标准兼容，'+' 与 '-' 无法作为多字符操作符
	 * 的最后一个字符，除非该操作符包含不在 SQL
	 * 操作符中的字符。用意是把 '=-' 词法分析为两个操作符，
	 * 但不禁止像 '?-' 这样不可能是标准 SQL 操作符序列的
	 * 操作符名。
	 */
	if (len > 1 &&
		(name[len - 1] == '+' ||
		 name[len - 1] == '-'))
	{
		int			ic;

		for (ic = len - 2; ic >= 0; ic--)
		{
			if (strchr("~!@#^&|`?%", name[ic]))
				break;
		}
		if (ic < 0)
			return false;		/* 不行，不合法 */
	}

	/* != 也不合法，因为解析器会把它转换成 <> */
	if (strcmp(name, "!=") == 0)
		return false;

	return true;
}


/*
 * OperatorGet
 *
 *		按精确规格（名称、命名空间、
 *		左与右类型 ID）查找操作符。
 *
 *		若已定义（非 shell）则 *defined 置为真
 */
static Oid
OperatorGet(const char *operatorName,
			Oid operatorNamespace,
			Oid leftObjectId,
			Oid rightObjectId,
			bool *defined)
{
	HeapTuple	tup;
	Oid			operatorObjectId;

	tup = SearchSysCache4(OPERNAMENSP,
						  PointerGetDatum(operatorName),
						  ObjectIdGetDatum(leftObjectId),
						  ObjectIdGetDatum(rightObjectId),
						  ObjectIdGetDatum(operatorNamespace));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_operator oprform = (Form_pg_operator) GETSTRUCT(tup);

		operatorObjectId = oprform->oid;
		*defined = RegProcedureIsValid(oprform->oprcode);
		ReleaseSysCache(tup);
	}
	else
	{
		operatorObjectId = InvalidOid;
		*defined = false;
	}

	return operatorObjectId;
}

/*
 * OperatorLookup
 *
 *		按可能带限定的名称以及左、右
 *		类型 ID 查找操作符。
 *
 *		若已定义（非 shell）则 *defined 置为真
 */
static Oid
OperatorLookup(List *operatorName,
			   Oid leftObjectId,
			   Oid rightObjectId,
			   bool *defined)
{
	Oid			operatorObjectId;
	RegProcedure oprcode;

	operatorObjectId = LookupOperName(NULL, operatorName,
									  leftObjectId, rightObjectId,
									  true, -1);
	if (!OidIsValid(operatorObjectId))
	{
		*defined = false;
		return InvalidOid;
	}

	oprcode = get_opcode(operatorObjectId);
	*defined = RegProcedureIsValid(oprcode);

	return operatorObjectId;
}


/*
 * OperatorShellMake
 *		为尚不存在的操作符制作一条 "shell" 条目。
 */
static Oid
OperatorShellMake(const char *operatorName,
				  Oid operatorNamespace,
				  Oid leftTypeId,
				  Oid rightTypeId)
{
	Relation	pg_operator_desc;
	Oid			operatorObjectId;
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_operator];
	bool		nulls[Natts_pg_operator];
	NameData	oname;
	TupleDesc	tupDesc;

	/*
	 * 校验操作符名称
	 */
	if (!validOperatorName(operatorName))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("\"%s\" is not a valid operator name",
						operatorName)));

	/*
	 * 打开 pg_operator
	 */
	pg_operator_desc = table_open(OperatorRelationId, RowExclusiveLock);
	tupDesc = pg_operator_desc->rd_att;

	/*
	 * 初始化我们的 *nulls 与 *values 数组
	 */
	for (i = 0; i < Natts_pg_operator; ++i)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;	/* 多余但安全 */
	}

	/*
	 * 用操作符名称与输入数据类型初始化 values[]。注意
	 * oprcode 设为 InvalidOid，表明这是 shell。
	 */
	operatorObjectId = GetNewOidWithIndex(pg_operator_desc, OperatorOidIndexId,
										  Anum_pg_operator_oid);
	values[Anum_pg_operator_oid - 1] = ObjectIdGetDatum(operatorObjectId);
	namestrcpy(&oname, operatorName);
	values[Anum_pg_operator_oprname - 1] = NameGetDatum(&oname);
	values[Anum_pg_operator_oprnamespace - 1] = ObjectIdGetDatum(operatorNamespace);
	values[Anum_pg_operator_oprowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_operator_oprkind - 1] = CharGetDatum(leftTypeId ? 'b' : 'l');
	values[Anum_pg_operator_oprcanmerge - 1] = BoolGetDatum(false);
	values[Anum_pg_operator_oprcanhash - 1] = BoolGetDatum(false);
	values[Anum_pg_operator_oprleft - 1] = ObjectIdGetDatum(leftTypeId);
	values[Anum_pg_operator_oprright - 1] = ObjectIdGetDatum(rightTypeId);
	values[Anum_pg_operator_oprresult - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprcode - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprrest - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_operator_oprjoin - 1] = ObjectIdGetDatum(InvalidOid);

	/*
	 * 创建新的操作符元组
	 */
	tup = heap_form_tuple(tupDesc, values, nulls);

	/*
	 * 插入我们的 "shell" 操作符元组
	 */
	CatalogTupleInsert(pg_operator_desc, tup);

	/* 为该条目添加依赖 */
	makeOperatorDependencies(tup, true, false);

	heap_freetuple(tup);

	/* 新 shell 操作符的创建后钩子 */
	InvokeObjectPostCreateHook(OperatorRelationId, operatorObjectId, 0);

	/*
	 * 确保该元组对后续查找/更新可见。
	 */
	CommandCounterIncrement();

	/*
	 * 关闭操作符关系并返回 oid。
	 */
	table_close(pg_operator_desc, RowExclusiveLock);

	return operatorObjectId;
}

/*
 * OperatorCreate
 *
 * "X" 表示可选参数（即可为 NULL 或 0）
 *		operatorName			新操作符的名称
 *		operatorNamespace		新操作符的命名空间
 *		leftTypeId				X 左类型 ID
 *		rightTypeId				X 右类型 ID
 *		procedureId				操作符的过程 ID
 *		commutatorName			X 交换操作符
 *		negatorName				X 否定操作符
 *		restrictionId			X 限制选择率过程 ID
 *		joinId					X 连接选择率过程 ID
 *		canMerge				归并连接可与此操作符一起使用
 *		canHash					哈希连接可与此操作符一起使用
 *
 * 调用方应已校验作为 OID 引用传入的对象的
 * 属性与权限。但我们必须特殊处理交换与
 * 否定操作符引用，因为它们事先不必
 * 存在。
 *
 * 本例程之所以复杂，是因为它允许用户
 * 指定尚不存在的操作符。例如，若正在
 * 定义操作符 "op"，否定操作符 "negop" 与
 * 交换操作符 "commop" 也可以在只给出
 * 名称的情况下一并定义。由于要把 "op"
 * 加入 PG_OPERATOR 目录，这些操作符的全部
 * Oid 都必须放入 "op" 的字段中，因此会对
 * 交换与否定操作符做前向声明。
 * 这称为创建 shell，其主要效果是在
 * PG_OPERATOR 目录中创建一条仅含操作符
 * 最少信息（仅名称与类型）的元组。
 * 前向声明仅用于此目的，不像类型定义
 * 那样向用户开放。
 */
ObjectAddress
OperatorCreate(const char *operatorName,
			   Oid operatorNamespace,
			   Oid leftTypeId,
			   Oid rightTypeId,
			   Oid procedureId,
			   List *commutatorName,
			   List *negatorName,
			   Oid restrictionId,
			   Oid joinId,
			   bool canMerge,
			   bool canHash)
{
	Relation	pg_operator_desc;
	HeapTuple	tup;
	bool		isUpdate;
	bool		nulls[Natts_pg_operator];
	bool		replaces[Natts_pg_operator];
	Datum		values[Natts_pg_operator];
	Oid			operatorObjectId;
	bool		operatorAlreadyDefined;
	Oid			operResultType;
	Oid			commutatorId,
				negatorId;
	bool		selfCommutator = false;
	NameData	oname;
	int			i;
	ObjectAddress address;

	/*
	 * 健全性检查
	 */
	if (!validOperatorName(operatorName))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("\"%s\" is not a valid operator name",
						operatorName)));

	if (!(OidIsValid(leftTypeId) && OidIsValid(rightTypeId)))
	{
		/* 若不是二元操作符，这些东西不得设置： */
		if (commutatorName)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can have commutators")));
		if (OidIsValid(joinId))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can have join selectivity")));
		if (canMerge)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can merge join")));
		if (canHash)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can hash")));
	}

	operResultType = get_func_rettype(procedureId);

	if (operResultType != BOOLOID)
	{
		/* 若不是布尔操作符，这些东西不得设置： */
		if (negatorName)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have negators")));
		if (OidIsValid(restrictionId))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have restriction selectivity")));
		if (OidIsValid(joinId))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have join selectivity")));
		if (canMerge)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can merge join")));
		if (canHash)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can hash")));
	}

	operatorObjectId = OperatorGet(operatorName,
								   operatorNamespace,
								   leftTypeId,
								   rightTypeId,
								   &operatorAlreadyDefined);

	if (operatorAlreadyDefined)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_FUNCTION),
				 errmsg("operator %s already exists",
						operatorName)));

	/*
	 * 到此为止，若 operatorObjectId 不是 InvalidOid，则我们是在
	 * 填充先前创建的 shell。坚持要求用户拥有任何
	 * 这样的 shell。
	 */
	if (OidIsValid(operatorObjectId) &&
		!object_ownercheck(OperatorRelationId, operatorObjectId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
					   operatorName);

	/*
	 * 设置其他操作符。若它们当前不存在，则创建
	 * shell 以取得 ObjectId。
	 */

	if (commutatorName)
	{
		/* 交换操作符的参数类型相反 */
		commutatorId = get_other_operator(commutatorName,
										  rightTypeId, leftTypeId,
										  operatorName, operatorNamespace,
										  leftTypeId, rightTypeId,
										  true);

		/* 权限检查：必须拥有另一操作符 */
		if (OidIsValid(commutatorId) &&
			!object_ownercheck(OperatorRelationId, commutatorId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
						   NameListToString(commutatorName));

		/*
		 * 指向本操作符的自链接；将在下面修正。注意只有
		 * 交换的自链接才有意义。
		 */
		if (!OidIsValid(commutatorId))
			selfCommutator = true;
	}
	else
		commutatorId = InvalidOid;

	if (negatorName)
	{
		/* 否定操作符的参数类型相同 */
		negatorId = get_other_operator(negatorName,
									   leftTypeId, rightTypeId,
									   operatorName, operatorNamespace,
									   leftTypeId, rightTypeId,
									   false);

		/* 权限检查：必须拥有另一操作符 */
		if (OidIsValid(negatorId) &&
			!object_ownercheck(OperatorRelationId, negatorId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
						   NameListToString(negatorName));
	}
	else
		negatorId = InvalidOid;

	/*
	 * 设置操作符元组中的值
	 */

	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) NULL;
		replaces[i] = true;
		nulls[i] = false;
	}

	namestrcpy(&oname, operatorName);
	values[Anum_pg_operator_oprname - 1] = NameGetDatum(&oname);
	values[Anum_pg_operator_oprnamespace - 1] = ObjectIdGetDatum(operatorNamespace);
	values[Anum_pg_operator_oprowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_operator_oprkind - 1] = CharGetDatum(leftTypeId ? 'b' : 'l');
	values[Anum_pg_operator_oprcanmerge - 1] = BoolGetDatum(canMerge);
	values[Anum_pg_operator_oprcanhash - 1] = BoolGetDatum(canHash);
	values[Anum_pg_operator_oprleft - 1] = ObjectIdGetDatum(leftTypeId);
	values[Anum_pg_operator_oprright - 1] = ObjectIdGetDatum(rightTypeId);
	values[Anum_pg_operator_oprresult - 1] = ObjectIdGetDatum(operResultType);
	values[Anum_pg_operator_oprcom - 1] = ObjectIdGetDatum(commutatorId);
	values[Anum_pg_operator_oprnegate - 1] = ObjectIdGetDatum(negatorId);
	values[Anum_pg_operator_oprcode - 1] = ObjectIdGetDatum(procedureId);
	values[Anum_pg_operator_oprrest - 1] = ObjectIdGetDatum(restrictionId);
	values[Anum_pg_operator_oprjoin - 1] = ObjectIdGetDatum(joinId);

	pg_operator_desc = table_open(OperatorRelationId, RowExclusiveLock);

	/*
	 * 若正在替换操作符 shell，则更新；否则插入
	 */
	if (operatorObjectId)
	{
		isUpdate = true;

		tup = SearchSysCacheCopy1(OPEROID,
								  ObjectIdGetDatum(operatorObjectId));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for operator %u",
				 operatorObjectId);

		replaces[Anum_pg_operator_oid - 1] = false;
		tup = heap_modify_tuple(tup,
								RelationGetDescr(pg_operator_desc),
								values,
								nulls,
								replaces);

		CatalogTupleUpdate(pg_operator_desc, &tup->t_self, tup);
	}
	else
	{
		isUpdate = false;

		operatorObjectId = GetNewOidWithIndex(pg_operator_desc,
											  OperatorOidIndexId,
											  Anum_pg_operator_oid);
		values[Anum_pg_operator_oid - 1] = ObjectIdGetDatum(operatorObjectId);

		tup = heap_form_tuple(RelationGetDescr(pg_operator_desc),
							  values, nulls);

		CatalogTupleInsert(pg_operator_desc, tup);
	}

	/* 为该条目添加依赖 */
	address = makeOperatorDependencies(tup, true, isUpdate);

	/* 新操作符的创建后钩子 */
	InvokeObjectPostCreateHook(OperatorRelationId, operatorObjectId, 0);

	table_close(pg_operator_desc, RowExclusiveLock);

	/*
	 * 若提供了交换和/或否定链接，则更新另一
	 * operator 使其指向本操作符（若它们尚未有链接）。
	 * 这支持另一种操作符定义风格：用户先
	 * 定义一个不给出否定或交换的操作符，
	 * 再定义该对中的另一个并带上正确的交换或
	 * 否定属性。那种风格不需要创建 shell，
	 * 并且是 Postgres 6.5 之前唯一能正确工作的风格。
	 * 本代码也处理新操作符是其自身交换操作符的
	 * 情形。
	 */
	if (selfCommutator)
		commutatorId = operatorObjectId;

	if (OidIsValid(commutatorId) || OidIsValid(negatorId))
		OperatorUpd(operatorObjectId, commutatorId, negatorId, false);

	return address;
}

/*
 * 尝试查找另一操作符（交换操作符等）
 *
 * 若未找到，检查它是否恰好是我们正在定义的
 * 操作符；若是，返回 InvalidOid。（注意此情形仅对
 * 交换操作符有意义，否则我们报错。）若不是
 * 同一操作符，则创建 shell 操作符。
 */
static Oid
get_other_operator(List *otherOp, Oid otherLeftTypeId, Oid otherRightTypeId,
				   const char *operatorName, Oid operatorNamespace,
				   Oid leftTypeId, Oid rightTypeId, bool isCommutator)
{
	Oid			other_oid;
	bool		otherDefined;
	char	   *otherName;
	Oid			otherNamespace;
	AclResult	aclresult;

	other_oid = OperatorLookup(otherOp,
							   otherLeftTypeId,
							   otherRightTypeId,
							   &otherDefined);

	if (OidIsValid(other_oid))
	{
		/* 另一操作符已在目录中 */
		return other_oid;
	}

	otherNamespace = QualifiedNameGetCreationNamespace(otherOp,
													   &otherName);

	if (strcmp(otherName, operatorName) == 0 &&
		otherNamespace == operatorNamespace &&
		otherLeftTypeId == leftTypeId &&
		otherRightTypeId == rightTypeId)
	{
		/*
		 * 指向本操作符的自链接；调用方稍后会修正。注意
		 * 只有交换的自链接才有意义。
		 */
		if (!isCommutator)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("operator cannot be its own negator or sort operator")));
		return InvalidOid;
	}

	/* 不在目录中，且不同于本操作符，因此制作 shell */

	aclresult = object_aclcheck(NamespaceRelationId, otherNamespace, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(otherNamespace));

	other_oid = OperatorShellMake(otherName,
								  otherNamespace,
								  otherLeftTypeId,
								  otherRightTypeId);
	return other_oid;
}

/*
 * OperatorUpd
 *
 *	对给定操作符，查找其否定与交换操作符。
 *	当 isDelete 为 false 时，更新它们的否定与交换字段以
 *	指回给定操作符；当 isDelete 为 true 时，更新那些
 *	字段使其不再指回给定操作符。
 *
 *	!isDelete 情形解决需要插入两个互为否定或
 *	交换的新操作符的用户问题，而
 *	isDelete 情形则是为了在删除操作符后不留下悬空的
 *	OID 链接。
 */
void
OperatorUpd(Oid baseId, Oid commId, Oid negId, bool isDelete)
{
	Relation	pg_operator_desc;
	HeapTuple	tup;

	/*
	 * 若要把某操作符变成其自身的交换操作符，则需要
	 * 命令计数器递增，因为我们刚插入了即将更新的
	 * 元组。但在删除操作符时可以跳过，因为我们处于
	 * 命令开头。
	 */
	if (!isDelete)
		CommandCounterIncrement();

	/* 打开关系。 */
	pg_operator_desc = table_open(OperatorRelationId, RowExclusiveLock);

	/* 取得交换操作符元组的可写副本。 */
	if (OidIsValid(commId))
		tup = SearchSysCacheCopy1(OPEROID, ObjectIdGetDatum(commId));
	else
		tup = NULL;

	/* 若需要则更新交换操作符的元组。 */
	if (HeapTupleIsValid(tup))
	{
		Form_pg_operator t = (Form_pg_operator) GETSTRUCT(tup);
		bool		update_commutator = false;

		/*
		 * 出于谨慎，我们仅在交换操作符的 oprcom 字段具有
		 * 我们预期的确切值时才改它：创建操作符时为
		 * InvalidOid，删除时为 baseId。
		 */
		if (isDelete && t->oprcom == baseId)
		{
			t->oprcom = InvalidOid;
			update_commutator = true;
		}
		else if (!isDelete && !OidIsValid(t->oprcom))
		{
			t->oprcom = baseId;
			update_commutator = true;
		}

		/* 若发现有列需要修改，则更新元组。 */
		if (update_commutator)
		{
			CatalogTupleUpdate(pg_operator_desc, &tup->t_self, tup);

			/*
			 * 做 CCI 以使更新后的元组可见。必须这样做，以防
			 * 交换操作符同时也是否定操作符。（对操作符定义者
			 * 而言那会是逻辑错误，但不足以成为在此失败的
			 * 正当理由。）在删除情形下，对无否定符的自交换
			 * 操作符反正也需要 CCI。
			 */
			CommandCounterIncrement();
		}
	}

	/*
	 * 类似地查找并更新否定操作符（若有）。
	 */
	if (OidIsValid(negId))
		tup = SearchSysCacheCopy1(OPEROID, ObjectIdGetDatum(negId));
	else
		tup = NULL;

	if (HeapTupleIsValid(tup))
	{
		Form_pg_operator t = (Form_pg_operator) GETSTRUCT(tup);
		bool		update_negator = false;

		/*
		 * 出于谨慎，我们仅在否定操作符的 oprnegate 字段具有
		 * 我们预期的确切值时才改它：创建操作符时为
		 * InvalidOid，删除时为 baseId。
		 */
		if (isDelete && t->oprnegate == baseId)
		{
			t->oprnegate = InvalidOid;
			update_negator = true;
		}
		else if (!isDelete && !OidIsValid(t->oprnegate))
		{
			t->oprnegate = baseId;
			update_negator = true;
		}

		/* 若发现有列需要修改，则更新元组。 */
		if (update_negator)
		{
			CatalogTupleUpdate(pg_operator_desc, &tup->t_self, tup);

			/*
			 * 在删除情形下，做 CCI 以使更新后的元组可见。
			 * 必须这样做，以防操作符是其自身的否定符。（对
			 * 操作符定义者而言那会是逻辑错误，但不足以成为
			 * 在此失败的正当理由。）
			 */
			if (isDelete)
				CommandCounterIncrement();
		}
	}

	/* 关闭关系并释放目录锁。 */
	table_close(pg_operator_desc, RowExclusiveLock);
}

/*
 * 为操作符创建依赖（无论是刚插入的完整
 * 操作符、新 shell 操作符、刚更新的 shell，
 * 还是正由 ALTER OPERATOR 修改的操作符）。
 *
 * 在制作新操作符或替换 shell 时应使
 * makeExtensionDep 为真，对 ALTER OPERATOR 则为假。传 false
 * 将阻止操作符的扩展成员关系发生任何变化。
 *
 * NB: 本例程中的 OidIsValid 测试是必要的，以防
 * 给定操作符是 shell。
 */
ObjectAddress
makeOperatorDependencies(HeapTuple tuple,
						 bool makeExtensionDep,
						 bool isUpdate)
{
	Form_pg_operator oper = (Form_pg_operator) GETSTRUCT(tuple);
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs;

	ObjectAddressSet(myself, OperatorRelationId, oper->oid);

	/*
	 * 若正在更新操作符，则删除任何已有条目，但
	 * 扩展成员关系应保持不变除外。
	 */
	if (isUpdate)
	{
		deleteDependencyRecordsFor(myself.classId, myself.objectId, true);
		deleteSharedDependencyRecordsFor(myself.classId, myself.objectId, 0);
	}

	addrs = new_object_addresses();

	/* 对命名空间的依赖 */
	if (OidIsValid(oper->oprnamespace))
	{
		ObjectAddressSet(referenced, NamespaceRelationId, oper->oprnamespace);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对左类型的依赖 */
	if (OidIsValid(oper->oprleft))
	{
		ObjectAddressSet(referenced, TypeRelationId, oper->oprleft);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对右类型的依赖 */
	if (OidIsValid(oper->oprright))
	{
		ObjectAddressSet(referenced, TypeRelationId, oper->oprright);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对结果类型的依赖 */
	if (OidIsValid(oper->oprresult))
	{
		ObjectAddressSet(referenced, TypeRelationId, oper->oprresult);
		add_exact_object_address(&referenced, addrs);
	}

	/*
	 * NOTE: 我们不认为操作符依赖于相关联的
	 * 操作符 oprcom 与 oprnegate。我们不想在那些对象消失时
	 * 删除本操作符，而只想重置链接字段；这不是
	 * 依赖逻辑能处理的功能。（改由
	 * RemoveOperatorById 内手工处理。）
	 */

	/* 对实现函数的依赖 */
	if (OidIsValid(oper->oprcode))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, oper->oprcode);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对限制选择率函数的依赖 */
	if (OidIsValid(oper->oprrest))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, oper->oprrest);
		add_exact_object_address(&referenced, addrs);
	}

	/* 对连接选择率函数的依赖 */
	if (OidIsValid(oper->oprjoin))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, oper->oprjoin);
		add_exact_object_address(&referenced, addrs);
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/* 对所有者的依赖 */
	recordDependencyOnOwner(OperatorRelationId, oper->oid,
							oper->oprowner);

	/* 对扩展的依赖 */
	if (makeExtensionDep)
		recordDependencyOnCurrentExtension(&myself, isUpdate);

	return myself;
}
