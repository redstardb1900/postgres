/*-------------------------------------------------------------------------
 *
 * params.c
 *	  Support for finding the values associated with Param nodes.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/nodes/params.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "mb/stringinfo_mb.h"
#include "nodes/params.h"
#include "parser/parse_node.h"
#include "storage/shmem.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


static void paramlist_parser_setup(ParseState *pstate, void *arg);
static Node *paramlist_param_ref(ParseState *pstate, ParamRef *pref);


/*
 * 分配并初始化一个新的 ParamListInfo 结构体。
 *
 * 要为 "动态" 方式（带钩子）建立新结构，numParams 传 0，
 * 再手动设置 numParams。
 *
 * 会自动提供一个默认的 parserSetup 函数。调用方若愿意
 * 可以覆盖它。（注意，ParamListInfos 的多数使用场景
 * 反正永远不会用到 parserSetup 函数。）
 */
ParamListInfo
makeParamList(int numParams)
{
	ParamListInfo retval;
	Size		size;

	size = offsetof(ParamListInfoData, params) +
		numParams * sizeof(ParamExternData);

	retval = (ParamListInfo) palloc(size);
	retval->paramFetch = NULL;
	retval->paramFetchArg = NULL;
	retval->paramCompile = NULL;
	retval->paramCompileArg = NULL;
	retval->parserSetup = paramlist_parser_setup;
	retval->parserSetupArg = (void *) retval;
	retval->paramValuesStr = NULL;
	retval->numParams = numParams;

	return retval;
}

/*
 * 复制一个 ParamListInfo 结构体。
 *
 * 结果分配在 CurrentMemoryContext 中。
 *
 * 注意：本函数的意图是做出一套静态的、自包含的
 * 参数值。若存在动态参数钩子，我们
 * 有意不把它们复制进结果。而是强制
 * 实例化所有可用的参数值，并复制 datum 值。
 *
 * 也不复制 paramValuesStr。
 */
ParamListInfo
copyParamList(ParamListInfo from)
{
	ParamListInfo retval;

	if (from == NULL || from->numParams <= 0)
		return NULL;

	retval = makeParamList(from->numParams);

	for (int i = 0; i < from->numParams; i++)
	{
		ParamExternData *oprm;
		ParamExternData *nprm = &retval->params[i];
		ParamExternData prmdata;
		int16		typLen;
		bool		typByVal;

		/* 若参数是动态的，给钩子一次机会 */
		if (from->paramFetch != NULL)
			oprm = from->paramFetch(from, i + 1, false, &prmdata);
		else
			oprm = &from->params[i];

		/* 平铺复制参数信息 */
		*nprm = *oprm;

		/* 若它是按引用传递的数据类型，则需要 datumCopy */
		if (nprm->isnull || !OidIsValid(nprm->ptype))
			continue;
		get_typlenbyval(nprm->ptype, &typLen, &typByVal);
		nprm->value = datumCopy(nprm->value, typByVal, typLen);
	}

	return retval;
}


/*
 * 准备解析含有参数引用的查询，
 * 这些参数来自 ParamListInfo。
 */
static void
paramlist_parser_setup(ParseState *pstate, void *arg)
{
	pstate->p_paramref_hook = paramlist_param_ref;
	/* 不需要使用 p_coerce_param_hook */
	pstate->p_ref_hook_state = arg;
}

/*
 * 用来自 ParamListInfo 的参数类型数据变换 ParamRef。
 */
static Node *
paramlist_param_ref(ParseState *pstate, ParamRef *pref)
{
	ParamListInfo paramLI = (ParamListInfo) pstate->p_ref_hook_state;
	int			paramno = pref->number;
	ParamExternData *prm;
	ParamExternData prmdata;
	Param	   *param;

	/* 检查参数编号是否有效 */
	if (paramno <= 0 || paramno > paramLI->numParams)
		return NULL;

	/* 若参数是动态的，给钩子一次机会 */
	if (paramLI->paramFetch != NULL)
		prm = paramLI->paramFetch(paramLI, paramno, false, &prmdata);
	else
		prm = &paramLI->params[paramno - 1];

	if (!OidIsValid(prm->ptype))
		return NULL;

	param = makeNode(Param);
	param->paramkind = PARAM_EXTERN;
	param->paramid = paramno;
	param->paramtype = prm->ptype;
	param->paramtypmod = -1;
	param->paramcollid = get_typcollation(param->paramtype);
	param->location = pref->location;

	return (Node *) param;
}

/*
 * 估算序列化一个 ParamListInfo 所需的空间。
 */
Size
EstimateParamListSpace(ParamListInfo paramLI)
{
	int			i;
	Size		sz = sizeof(int);

	if (paramLI == NULL || paramLI->numParams <= 0)
		return sz;

	for (i = 0; i < paramLI->numParams; i++)
	{
		ParamExternData *prm;
		ParamExternData prmdata;
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;

		/* 若参数是动态的，给钩子一次机会 */
		if (paramLI->paramFetch != NULL)
			prm = paramLI->paramFetch(paramLI, i + 1, false, &prmdata);
		else
			prm = &paramLI->params[i];

		typeOid = prm->ptype;

		sz = add_size(sz, sizeof(Oid)); /* 类型 OID 所需空间 */
		sz = add_size(sz, sizeof(uint16));	/* pflags 所需空间 */

		/* datum/isnull 所需空间 */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* 若没有类型 OID，就假定按值传递，如同 copyParamList 那样。 */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		sz = add_size(sz,
					  datumEstimateSpace(prm->value, prm->isnull, typByVal, typLen));
	}

	return sz;
}

/*
 * 把 ParamListInfo 结构体序列化到调用方提供的存储中。
 *
 * 我们先把参数个数写成 4 字节整数，然后
 * 依次写出每个参数的细节。每个参数的细节
 * 包括 4 字节类型 OID、2 字节标志，然后是 datum，
 * 由 datumSerialize() 序列化。调用方负责确保
 * 有足够的存储来存放将要写入的字节数；
 * 用 EstimateParamListSpace 查出需要多少。
 * *start_address 被更新为指向紧接那些
 * 已写字节之后的字节。
 *
 * 可以用 RestoreParamList 根据序列化表示重建 ParamListInfo；
 * 这将是一份静态的、自包含的副本，
 * 正如 copyParamList 会创建的那样。
 *
 * 不包括 paramValuesStr。
 */
void
SerializeParamList(ParamListInfo paramLI, char **start_address)
{
	int			nparams;
	int			i;

	/* 写出参数个数。 */
	if (paramLI == NULL || paramLI->numParams <= 0)
		nparams = 0;
	else
		nparams = paramLI->numParams;
	memcpy(*start_address, &nparams, sizeof(int));
	*start_address += sizeof(int);

	/* 依次写出每个参数。 */
	for (i = 0; i < nparams; i++)
	{
		ParamExternData *prm;
		ParamExternData prmdata;
		Oid			typeOid;
		int16		typLen;
		bool		typByVal;

		/* 若参数是动态的，给钩子一次机会 */
		if (paramLI->paramFetch != NULL)
			prm = paramLI->paramFetch(paramLI, i + 1, false, &prmdata);
		else
			prm = &paramLI->params[i];

		typeOid = prm->ptype;

		/* 写出类型 OID。 */
		memcpy(*start_address, &typeOid, sizeof(Oid));
		*start_address += sizeof(Oid);

		/* 写出标志。 */
		memcpy(*start_address, &prm->pflags, sizeof(uint16));
		*start_address += sizeof(uint16);

		/* 写出 datum/isnull。 */
		if (OidIsValid(typeOid))
			get_typlenbyval(typeOid, &typLen, &typByVal);
		else
		{
			/* 若没有类型 OID，就假定按值传递，如同 copyParamList 那样。 */
			typLen = sizeof(Datum);
			typByVal = true;
		}
		datumSerialize(prm->value, prm->isnull, typByVal, typLen,
					   start_address);
	}
}

/*
 * 复制一个 ParamListInfo 结构体。
 *
 * 结果分配在 CurrentMemoryContext 中。
 *
 * 注意：本函数的意图是做出一套静态的、自包含的
 * 参数值。若存在动态参数钩子，我们
 * 有意不把它们复制进结果。而是强制
 * 实例化所有可用的参数值，并复制 datum 值。
 */
ParamListInfo
RestoreParamList(char **start_address)
{
	ParamListInfo paramLI;
	int			nparams;

	memcpy(&nparams, *start_address, sizeof(int));
	*start_address += sizeof(int);

	paramLI = makeParamList(nparams);

	for (int i = 0; i < nparams; i++)
	{
		ParamExternData *prm = &paramLI->params[i];

		/* 读取类型 OID。 */
		memcpy(&prm->ptype, *start_address, sizeof(Oid));
		*start_address += sizeof(Oid);

		/* 读取标志。 */
		memcpy(&prm->pflags, *start_address, sizeof(uint16));
		*start_address += sizeof(uint16);

		/* 读取 datum/isnull。 */
		prm->value = datumRestore(start_address, &prm->isnull);
	}

	return paramLI;
}

/*
 * BuildParamLogString
 *		返回表示参数列表的字符串，用于记日志。
 *
 * 若调用方已经知道某些参数的文本表示，它可以
 * 传入恰好 params->numParams 个值的数组作为 knownTextValues，其中
 * 未知的单个值可以是 NULLs。若没有任何已知参数，
 * 可以传入 NULL。
 *
 * 若 maxlen >= 0，那就是任一参数值要打印的
 * 最大字节数；若字符串更长就加上省略号。
 * （加上的引号不计入这一计算。）
 */
char *
BuildParamLogString(ParamListInfo params, char **knownTextValues, int maxlen)
{
	MemoryContext tmpCxt,
				oldCxt;
	StringInfoData buf;

	/*
	 * 注意：不要想着返回 params->paramValuesStr！它可能是用
	 * 不同的 maxlen 生成的，因此不合适。除此之外，
	 * 创建那个字符串用的就是本函数。
	 */

	/*
	 * 若正在使用参数获取钩子，就无事可做。另外，在
	 * 已中止的事务中无法做这件事。（若存在某些 knownTextValues，
	 * 也许可以改进最后这一点，但看起来很棘手。）
	 */
	if (params->paramFetch != NULL ||
		IsAbortedTransactionBlockState())
		return NULL;

	/* 在调用方的内存上下文中初始化输出 stringinfo */
	initStringInfo(&buf);

	/* 用临时上下文调用输出函数，以防万一 */
	tmpCxt = AllocSetContextCreate(CurrentMemoryContext,
								   "BuildParamLogString",
								   ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(tmpCxt);

	for (int paramno = 0; paramno < params->numParams; paramno++)
	{
		ParamExternData *param = &params->params[paramno];

		appendStringInfo(&buf,
						 "%s$%d = ",
						 paramno > 0 ? ", " : "",
						 paramno + 1);

		if (param->isnull || !OidIsValid(param->ptype))
			appendStringInfoString(&buf, "NULL");
		else
		{
			if (knownTextValues != NULL && knownTextValues[paramno] != NULL)
				appendStringInfoStringQuoted(&buf, knownTextValues[paramno],
											 maxlen);
			else
			{
				Oid			typoutput;
				bool		typisvarlena;
				char	   *pstring;

				getTypeOutputInfo(param->ptype, &typoutput, &typisvarlena);
				pstring = OidOutputFunctionCall(typoutput, param->value);
				appendStringInfoStringQuoted(&buf, pstring, maxlen);
			}
		}
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(tmpCxt);

	return buf.data;
}

/*
 * ParamsErrorCallback - 在错误上下文中打印参数的回调
 *
 * 注意，除非事先调用了 BuildParamLogString，否则这是
 * 空操作。
 */
void
ParamsErrorCallback(void *arg)
{
	ParamsErrorCbData *data = (ParamsErrorCbData *) arg;

	if (data == NULL ||
		data->params == NULL ||
		data->params->paramValuesStr == NULL)
		return;

	if (data->portalName && data->portalName[0] != '\0')
		errcontext("portal \"%s\" with parameters: %s",
				   data->portalName, data->params->paramValuesStr);
	else
		errcontext("unnamed portal with parameters: %s",
				   data->params->paramValuesStr);
}
