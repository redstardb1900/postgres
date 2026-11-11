/*-------------------------------------------------------------------------
 *
 * fastpath.c
 *	  处理来自前端的函数调用请求的例程      
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/tcop/fastpath.c
 *
 * NOTES
 *	  本文件是 PQfn 的服务器端。  
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "tcop/fastpath.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


/*
 * 此前，这段代码曾尝试缓存 fetch_fp_info 查到的     
 * 函数与类型信息，但只维持单个事务命令之久     
 * （理论上这些信息可能在命令之间变化）。         
 * 这完全没用，因为 postgres.c 把每次 fastpath 调用    
 * 当作单独的事务命令执行，缓存的数据因此       
 * 永远不可能被真正复用。即使按原意生效，           
 * 它也难免因 FmgrInfo 结构体里的悬空引用出问题。       
 * 所以放弃缓存，改为在每次使用时重新执行            
 * syscache 查询——它们并没有*那么*昂贵。         
 */
struct fp_info
{
	Oid			funcid;
	FmgrInfo	flinfo;			/* funcid 的函数查找信息    */
	Oid			namespace;		/* pg_proc 的其他信息  */
	Oid			rettype;
	Oid			argtypes[FUNC_MAX_ARGS];
	char		fname[NAMEDATALEN]; /* 用于日志的函数名  */
};


static int16 parse_fcall_arguments(StringInfo msgBuf, struct fp_info *fip,
								   FunctionCallInfo fcinfo);

/* ----------------
 *		SendFunctionResult
 * ----------------
 */
static void
SendFunctionResult(Datum retval, bool isnull, Oid rettype, int16 format)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'V');

	if (isnull)
	{
		pq_sendint32(&buf, -1);
	}
	else
	{
		if (format == 0)
		{
			Oid			typoutput;
			bool		typisvarlena;
			char	   *outputstr;

			getTypeOutputInfo(rettype, &typoutput, &typisvarlena);
			outputstr = OidOutputFunctionCall(typoutput, retval);
			pq_sendcountedtext(&buf, outputstr, strlen(outputstr), false);
			pfree(outputstr);
		}
		else if (format == 1)
		{
			Oid			typsend;
			bool		typisvarlena;
			bytea	   *outputbytes;

			getTypeBinaryOutputInfo(rettype, &typsend, &typisvarlena);
			outputbytes = OidSendFunctionCall(typsend, retval);
			pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(&buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
			pfree(outputbytes);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}

	pq_endmessage(&buf);
}

/*
 * fetch_fp_info
 *
 * 查找系统目录，为函数 'func_id' 装载                
 * fp_info（fip）。
 */
static void
fetch_fp_info(Oid func_id, struct fp_info *fip)
{
	HeapTuple	func_htp;
	Form_pg_proc pp;

	Assert(fip != NULL);

	/*
	 * 结构体是否有效，取决于 funcid 是否可用；所以   
	 * 我们在此清空 funcid。在即将带着完好的 fp_info      
	 * 结构体返回之前，不能把它设成正确的值；因为       
	 * 随时可能被打断（比如 ereport(ERROR, ...)）。              
	 * [如今已不再跨事务保存 fp_info，这其实不再是    
	 * 问题了，但仍保留这一做法。]                 
	 */
	MemSet(fip, 0, sizeof(struct fp_info));
	fip->funcid = InvalidOid;

	func_htp = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_id));
	if (!HeapTupleIsValid(func_htp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", func_id)));
	pp = (Form_pg_proc) GETSTRUCT(func_htp);

	/* 拒绝不能安全地经 fastpath 调用的 pg_proc 条目  */
	if (pp->prokind != PROKIND_FUNCTION || pp->proretset)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot call function \"%s\" via fastpath interface",
						NameStr(pp->proname))));

	/* 当心参数多于 FUNC_MAX_ARGS 的系统目录条目          */
	if (pp->pronargs > FUNC_MAX_ARGS)
		elog(ERROR, "function %s has more than %d arguments",
			 NameStr(pp->proname), FUNC_MAX_ARGS);

	fip->namespace = pp->pronamespace;
	fip->rettype = pp->prorettype;
	memcpy(fip->argtypes, pp->proargtypes.values, pp->pronargs * sizeof(Oid));
	strlcpy(fip->fname, NameStr(pp->proname), NAMEDATALEN);

	ReleaseSysCache(func_htp);

	fmgr_info(func_id, &fip->flinfo);

	/*
	 * 必须放最后！
	 */
	fip->funcid = func_id;
}


/*
 * HandleFunctionRequest
 *
 * PQfn 的服务器端（前端的 fastpath 函数调用）。    
 * 对应 libpq 协议符号 "F"。                  
 *
 * 输入
 *		postgres.c 已读取消息体，并将通过 msgBuf 传入。   
 *		       
 *
 * 注意：此处与被调函数里的 palloc() 无需显式清理，   
 * 我们在 PostgresMain() 的 MessageContext                     
 * 内存上下文里被调用；控制返回时它自动重置。      
 *                                 
 */
void
HandleFunctionRequest(StringInfo msgBuf)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	Oid			fid;
	AclResult	aclresult;
	int16		rformat;
	Datum		retval;
	struct fp_info my_fp;
	struct fp_info *fip;
	bool		callit;
	bool		was_logged = false;
	char		msec_str[32];

	/*
	 * 只有在已中止的事务里才接受 COMMIT/ABORT，而且       
	 * COMMIT/ABORT 无法经 fastpath 接口执行。                
	 */
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, "
						"commands ignored until end of transaction block")));

	/*
	 * 既然已确认处于有效事务中，就先设置快照，以备  
	 * 函数自身或某个数据类型 I/O 例程需要。         
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * 开始解析缓冲区内容。    
	 */
	fid = (Oid) pq_getmsgint(msgBuf, 4);	/* 函数 OID   */

	/*
	 * 这里曾有过一个蹩脚的查找信息缓存尝试；如今    
	 * 每次调用都直接查找。    
	 */
	fip = &my_fp;
	fetch_fp_info(fid, fip);

	/* 一旦拿到函数 OID 与名字就记录日志  */
	if (log_statement == LOGSTMT_ALL)
	{
		ereport(LOG,
				(errmsg("fastpath function call: \"%s\" (OID %u)",
						fip->fname, fid)));
		was_logged = true;
	}

	/*
	 * 检查访问与调用函数的权限。由于没有经过常规  
	 * 的名字查找，还需要检查 schema 使用权。           
	 */
	aclresult = object_aclcheck(NamespaceRelationId, fip->namespace, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(fip->namespace));
	InvokeNamespaceSearchHook(fip->namespace, true);

	aclresult = object_aclcheck(ProcedureRelationId, fid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   get_func_name(fid));
	InvokeFunctionExecuteHook(fid);

	/*
	 * 准备函数调用信息块并插入参数。         
	 *
	 * 注意：目前传入 collation = InvalidOid，因此对排序      
	 * 规则敏感的函数无法这样调用。或许应该传  
	 * DEFAULT_COLLATION_OID？       
	 */
	InitFunctionCallInfoData(*fcinfo, &fip->flinfo, 0, InvalidOid, NULL, NULL);

	rformat = parse_fcall_arguments(msgBuf, fip, fcinfo);

	/* 确认已在预期位置到达消息末尾               */
	pq_getmsgend(msgBuf);

	/*
	 * 若函数 strict，参数为空不得调用。    
	 */
	callit = true;
	if (fip->flinfo.fn_strict)
	{
		int			i;

		for (i = 0; i < fcinfo->nargs; i++)
		{
			if (fcinfo->args[i].isnull)
			{
				callit = false;
				break;
			}
		}
	}

	if (callit)
	{
		/* 执行……    */
		retval = FunctionCallInvoke(fcinfo);
	}
	else
	{
		fcinfo->isnull = true;
		retval = (Datum) 0;
	}

	/* 保证每次函数调用至少执行一次 CHECK_FOR_INTERRUPTS  */
	CHECK_FOR_INTERRUPTS();

	SendFunctionResult(retval, fcinfo->isnull, fip->rettype, rformat);

	/* 已不再需要该快照       */
	PopActiveSnapshot();

	/*
	 * 适当时记录耗时日志。       
	 */
	switch (check_log_duration(msec_str, was_logged))
	{
		case 1:
			ereport(LOG,
					(errmsg("duration: %s ms", msec_str)));
			break;
		case 2:
			ereport(LOG,
					(errmsg("duration: %s ms  fastpath function call: \"%s\" (OID %u)",
							msec_str, fip->fname, fid)));
			break;
	}
}

/*
 * 解析 3.0 协议消息中的函数参数         
 *
 * 参数值被装入 *fcinfo，期望的结果格式则作为           
 * 返回值。
 */
static int16
parse_fcall_arguments(StringInfo msgBuf, struct fp_info *fip,
					  FunctionCallInfo fcinfo)
{
	int			nargs;
	int			i;
	int			numAFormats;
	int16	   *aformats = NULL;
	StringInfoData abuf;

	/* 取得参数格式编码      */
	numAFormats = pq_getmsgint(msgBuf, 2);
	if (numAFormats > 0)
	{
		aformats = (int16 *) palloc(numAFormats * sizeof(int16));
		for (i = 0; i < numAFormats; i++)
			aformats[i] = pq_getmsgint(msgBuf, 2);
	}

	nargs = pq_getmsgint(msgBuf, 2);	/* 参数个数   */

	if (fip->flinfo.fn_nargs != nargs || nargs > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d arguments but function requires %d",
						nargs, fip->flinfo.fn_nargs)));

	fcinfo->nargs = nargs;

	if (numAFormats > 1 && numAFormats != nargs)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("function call message contains %d argument formats but %d arguments",
						numAFormats, nargs)));

	initStringInfo(&abuf);

	/*
	 * 把给定参数复制进参数数组。 
	 */
	for (i = 0; i < nargs; ++i)
	{
		int			argsize;
		int16		aformat;

		argsize = pq_getmsgint(msgBuf, 4);
		if (argsize == -1)
		{
			fcinfo->args[i].isnull = true;
		}
		else
		{
			fcinfo->args[i].isnull = false;
			if (argsize < 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("invalid argument size %d in function call message",
								argsize)));

			/* 将 abuf 重置为空，插入原始数据       */
			resetStringInfo(&abuf);
			appendBinaryStringInfo(&abuf,
								   pq_getmsgbytes(msgBuf, argsize),
								   argsize);
		}

		if (numAFormats > 1)
			aformat = aformats[i];
		else if (numAFormats > 0)
			aformat = aformats[0];
		else
			aformat = 0;		/* 默认为 text */

		if (aformat == 0)
		{
			Oid			typinput;
			Oid			typioparam;
			char	   *pstring;

			getTypeInputInfo(fip->argtypes[i], &typinput, &typioparam);

			/*
			 * stringinfo.c 连二进制数据也保留结尾 null，    
			 * abuf 的内容是有效的 C 字符串。但调用         
			 * typinput 例程之前必须先做编码转换。          
			 *                 
			 */
			if (argsize == -1)
				pstring = NULL;
			else
				pstring = pg_client_to_server(abuf.data, argsize);

			fcinfo->args[i].value = OidInputFunctionCall(typinput, pstring,
														 typioparam, -1);
			/* 释放编码转换的结果（如有）    */
			if (pstring && pstring != abuf.data)
				pfree(pstring);
		}
		else if (aformat == 1)
		{
			Oid			typreceive;
			Oid			typioparam;
			StringInfo	bufptr;

			/* 调用参数类型的二进制输入转换器   */
			getTypeBinaryInputInfo(fip->argtypes[i], &typreceive, &typioparam);

			if (argsize == -1)
				bufptr = NULL;
			else
				bufptr = &abuf;

			fcinfo->args[i].value = OidReceiveFunctionCall(typreceive, bufptr,
														   typioparam, -1);

			/* 未吃掉整个缓冲区即有问题      */
			if (argsize != -1 && abuf.cursor != abuf.len)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("incorrect binary data format in function argument %d",
								i + 1)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", aformat)));
	}

	/* 返回结果格式编码  */
	return (int16) pq_getmsgint(msgBuf, 2);
}
