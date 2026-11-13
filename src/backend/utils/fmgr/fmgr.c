/*-------------------------------------------------------------------------
 *
 * fmgr.c
 *	Postgres 函数管理器。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/fmgr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/functions.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgrtab.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * 函数调用钩子
 */
PGDLLIMPORT needs_fmgr_hook_type needs_fmgr_hook = NULL;
PGDLLIMPORT fmgr_hook_type fmgr_hook = NULL;

/*
 * 用于快速查找外部 C 函数的哈希表
 */
typedef struct
{
	/* fn_oid 是哈希键，必须放在首位 */
	Oid			fn_oid;			/* 外部 C 函数的 OID */
	TransactionId fn_xmin;		/* 用于检查是否最新 */
	ItemPointerData fn_tid;
	PGFunction	user_fn;		/* 函数的地址 */
	const Pg_finfo_record *inforec; /* 其信息记录的地址 */
} CFuncHashTabEntry;

static HTAB *CFuncHash = NULL;


static void fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
								   bool ignore_security);
static void fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static void fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple);
static CFuncHashTabEntry *lookup_C_func(HeapTuple procedureTuple);
static void record_C_func(HeapTuple procedureTuple,
						  PGFunction user_fn, const Pg_finfo_record *inforec);

/* extern 便可经 JIT 调用 */
extern Datum fmgr_security_definer(PG_FUNCTION_ARGS);


/*
 * 内置函数表的查找例程。可按 Oid 或名称搜索，
 * 但按 Oid 搜索快得多。
 */

static const FmgrBuiltin *
fmgr_isbuiltin(Oid id)
{
	uint16		index;

	/* 仅当原 oid 仍有效时才可快速查找 */
	if (id > fmgr_last_builtin_oid)
		return NULL;

	/*
	 * 查找函数数据。若该范围内未命中，则很可能是
	 * 不存在的函数；此处返回 NULL 稍后会触发 ERROR。
	 */
	index = fmgr_builtin_oid_index[id];
	if (index == InvalidOidBuiltinMapping)
		return NULL;

	return &fmgr_builtins[index];
}

/*
 * 按名称查找内置项。数组中可能有多条同名条目，
 * 但它们都应指向同一个
 * 例程
 */
static const FmgrBuiltin *
fmgr_lookupByName(const char *name)
{
	int			i;

	for (i = 0; i < fmgr_nbuiltins; i++)
	{
		if (strcmp(name, fmgr_builtins[i].funcName) == 0)
			return fmgr_builtins + i;
	}
	return NULL;
}

/*
 * 本例程填充 FmgrInfo 结构体，其入参为
 * 待调用函数的 OID。
 *
 * 调用者的 CurrentMemoryContext 被用作 fn_mcxt，
 * 这意味着附加到信息结构体的任何辅助数据（无论是
 * fmgr_info 本身或之后由调用处理程序附加的）
 * 都会分配在该上下文中。调用者必须确保此上下文
 * 的寿命至少与信息结构体本身相当。典型情况下这
 * 不是问题，因为信息结构体通常位于栈上或新近
 * palloc 的空间中。但如果打算把信息结构体存入长寿
 * 的表中，最好改用 fmgr_info_cxt。
 */
void
fmgr_info(Oid functionId, FmgrInfo *finfo)
{
	fmgr_info_cxt_security(functionId, finfo, CurrentMemoryContext, false);
}

/*
 * 填充 FmgrInfo 结构体，并指定一个内存上下文存放
 * 其辅助数据。
 */
void
fmgr_info_cxt(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt)
{
	fmgr_info_cxt_security(functionId, finfo, mcxt, false);
}

/*
 * 此函数执行实际工作。ignore_security 通常为 false，
 * 当需要避免递归时才置为 true。
 */
static void
fmgr_info_cxt_security(Oid functionId, FmgrInfo *finfo, MemoryContext mcxt,
					   bool ignore_security)
{
	const FmgrBuiltin *fbp;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	Datum		prosrcdatum;
	char	   *prosrc;

	/*
	 * fn_oid *必须* 最后填充。一些代码假定若 fn_oid 有效，
	 * 则整个结构体都有效。有些 FmgrInfo 结构体可
	 * 存活
	 */
	finfo->fn_oid = InvalidOid;
	finfo->fn_extra = NULL;
	finfo->fn_mcxt = mcxt;
	finfo->fn_expr = NULL;		/* 调用者稍后设置 */

	if ((fbp = fmgr_isbuiltin(functionId)) != NULL)
	{
		/*
		 * 内置函数的快速路径：无需查询 pg_proc
		 */
		finfo->fn_nargs = fbp->nargs;
		finfo->fn_strict = fbp->strict;
		finfo->fn_retset = fbp->retset;
		finfo->fn_stats = TRACK_FUNC_ALL;	/* 即从不跟踪 */
		finfo->fn_addr = fbp->func;
		finfo->fn_oid = functionId;
		return;
	}

	/* 否则需要获取 pg_proc 条目 */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	finfo->fn_nargs = procedureStruct->pronargs;
	finfo->fn_strict = procedureStruct->proisstrict;
	finfo->fn_retset = procedureStruct->proretset;

	/*
	 * 若设置了 prosecdef、proconfig 非空，或插件想挂钩
	 * 函数进入/退出，则使用 fmgr_security_definer 调用
	 * 处理程序——除非是 fmgr_security_definer 或
	 * fmgr_info_other_lang
	 *
	 * 使用 fmgr_security_definer 时，函数统计跟踪总是
	 * 在外层被禁用，而是改为在 fmgr_security_definer
	 * 的私有 flinfo 中正确设置该标志，并在
	 * fmgr_security_definer 内部实现跟踪。代价是无法把
	 * fmgr_security_definer 的开销计入该函数，但换
	 * 来了将 track_functions GUC 设为某关注函数
	 * 的本地 GUC 参数，并让正确的事情发生。
	 */
	if (!ignore_security &&
		(procedureStruct->prosecdef ||
		 !heap_attisnull(procedureTuple, Anum_pg_proc_proconfig, NULL) ||
		 FmgrHookIsNeeded(functionId)))
	{
		finfo->fn_addr = fmgr_security_definer;
		finfo->fn_stats = TRACK_FUNC_ALL;	/* 即从不跟踪 */
		finfo->fn_oid = functionId;
		ReleaseSysCache(procedureTuple);
		return;
	}

	switch (procedureStruct->prolang)
	{
		case INTERNALlanguageId:

			/*
			 * 对于普通内置函数，绝不应走到这里，
			 * 因为前面的 fmgr_isbuiltin() 搜索应已成功。
			 * 但若用户用 CREATE FUNCTION 创建了内置函数的
			 * 别名，最终就可能走到这里。那种情况下就
			 * 必须按名称查找该函数。内部函数的名称
			 * 存放在 prosrc 中（它不必与别名的名称
			 * 相同！）
			 */
			prosrcdatum = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												 Anum_pg_proc_prosrc);
			prosrc = TextDatumGetCString(prosrcdatum);
			fbp = fmgr_lookupByName(prosrc);
			if (fbp == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("internal function \"%s\" is not in internal lookup table",
								prosrc)));
			pfree(prosrc);
			/* 是否应检查 nargs、strict、retset 与表匹配？ */
			finfo->fn_addr = fbp->func;
			/* 注意上面的快速路径也假定此策略 */
			finfo->fn_stats = TRACK_FUNC_ALL;	/* 即从不跟踪 */
			break;

		case ClanguageId:
			fmgr_info_C_lang(functionId, finfo, procedureTuple);
			finfo->fn_stats = TRACK_FUNC_PL;	/* 即 ALL 跟踪 */
			break;

		case SQLlanguageId:
			finfo->fn_addr = fmgr_sql;
			finfo->fn_stats = TRACK_FUNC_PL;	/* 即 ALL 跟踪 */
			break;

		default:
			fmgr_info_other_lang(functionId, finfo, procedureTuple);
			finfo->fn_stats = TRACK_FUNC_OFF;	/* 非 OFF 则跟踪 */
			break;
	}

	finfo->fn_oid = functionId;
	ReleaseSysCache(procedureTuple);
}

/*
 * 返回函数 functionId 的实现所在模块与 C 函数名。
 *
 * 若 *mod 与 *fn 均为 NULL，则无已知
 * 实现
 *
 * 若 *mod == NULL 而 *fn != NULL，则函数由主二进制
 * 符号实现。
 *
 * 若 *mod != NULL 且 *fn != NULL，则函数实现在扩展
 * 共享库。
 *
 * 返回的模块名和函数名会 pstrdup 到当前内存
 * 上下文。
 */
void
fmgr_symbol(Oid functionId, char **mod, char **fn)
{
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	Datum		prosrcattr;
	Datum		probinattr;

	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionId));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", functionId);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	if (procedureStruct->prosecdef ||
		!heap_attisnull(procedureTuple, Anum_pg_proc_proconfig, NULL) ||
		FmgrHookIsNeeded(functionId))
	{
		*mod = NULL;			/* 主程序 */
		*fn = pstrdup("fmgr_security_definer");
		ReleaseSysCache(procedureTuple);
		return;
	}

	/* 各具体情况见 fmgr_info_cxt_security */
	switch (procedureStruct->prolang)
	{
		case INTERNALlanguageId:
			prosrcattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												Anum_pg_proc_prosrc);

			*mod = NULL;		/* 主程序 */
			*fn = TextDatumGetCString(prosrcattr);
			break;

		case ClanguageId:
			prosrcattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												Anum_pg_proc_prosrc);

			probinattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
												Anum_pg_proc_probin);

			/*
			 * 无需在此检查符号存在性与 API 版本，
			 * fmgr_info_cxt_security 已查过
			 */
			*mod = TextDatumGetCString(probinattr);
			*fn = TextDatumGetCString(prosrcattr);
			break;

		case SQLlanguageId:
			*mod = NULL;		/* 主程序 */
			*fn = pstrdup("fmgr_sql");
			break;

		default:
			*mod = NULL;
			*fn = NULL;			/* 未知，传指针 */
			break;
	}

	ReleaseSysCache(procedureTuple);
}


/*
 * 针对 C 语言函数的特殊 fmgr_info 处理。注意
 * finfo->fn_oid 尚无效。
 */
static void
fmgr_info_C_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	CFuncHashTabEntry *hashentry;
	PGFunction	user_fn;
	const Pg_finfo_record *inforec;

	/*
	 * 看看是否已缓存该函数的地址
	 */
	hashentry = lookup_C_func(procedureTuple);
	if (hashentry)
	{
		user_fn = hashentry->user_fn;
		inforec = hashentry->inforec;
	}
	else
	{
		Datum		prosrcattr,
					probinattr;
		char	   *prosrcstring,
				   *probinstring;
		void	   *libraryhandle;

		/*
		 * 获取 prosrc 和 probin 字符串（链接符号、库
		 * 名）。虽然这些列一般可为空，但 C 语言函数
		 * 却不得为空。
		 */
		prosrcattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
											Anum_pg_proc_prosrc);
		prosrcstring = TextDatumGetCString(prosrcattr);

		probinattr = SysCacheGetAttrNotNull(PROCOID, procedureTuple,
											Anum_pg_proc_probin);
		probinstring = TextDatumGetCString(probinattr);

		/* 查找函数本身 */
		user_fn = load_external_function(probinstring, prosrcstring, true,
										 &libraryhandle);

		/* 获取函数信息记录（真实的或默认的） */
		inforec = fetch_finfo_record(libraryhandle, prosrcstring);

		/* 缓存地址供后续调用 */
		record_C_func(procedureTuple, user_fn, inforec);

		pfree(prosrcstring);
		pfree(probinstring);
	}

	switch (inforec->api_version)
	{
		case 1:
			/* 新式：直接调用 */
			finfo->fn_addr = user_fn;
			break;
		default:
			/* 若 fetch_finfo_record 尽职则不应走到这里 */
			elog(ERROR, "unrecognized function API version: %d",
				 inforec->api_version);
			break;
	}
}

/*
 * 其他语言函数的专用 fmgr_info 处理。注意
 * 此时 finfo->fn_oid 尚无效。
 */
static void
fmgr_info_other_lang(Oid functionId, FmgrInfo *finfo, HeapTuple procedureTuple)
{
	Form_pg_proc procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);
	Oid			language = procedureStruct->prolang;
	HeapTuple	languageTuple;
	Form_pg_language languageStruct;
	FmgrInfo	plfinfo;

	languageTuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(language));
	if (!HeapTupleIsValid(languageTuple))
		elog(ERROR, "cache lookup failed for language %u", language);
	languageStruct = (Form_pg_language) GETSTRUCT(languageTuple);

	/*
	 * 查找语言的调用处理程序，忽略任何通常会导致
	 * 插入 fmgr_security_definer 的属性。我们需要
	 * 得到指向实际 C 语言函数的裸指针。
	 */
	fmgr_info_cxt_security(languageStruct->lanplcallfoid, &plfinfo,
						   CurrentMemoryContext, true);
	finfo->fn_addr = plfinfo.fn_addr;

	ReleaseSysCache(languageTuple);
}

/*
 * 获取并校验给定外部函数的信息记录。
 * 该函数由库句柄（经 load_external_function 获得）
 * 以及函数名来指定。
 *
 * 若给定名称没有对应的信息函数，则抛出错误。
 *
 * 本函数从 fmgr_info_C_lang 拆出，以便
 * fmgr_c_validator 能校验尚未录入 pg_proc 的
 * 记录
 */
const Pg_finfo_record *
fetch_finfo_record(void *filehandle, const char *funcname)
{
	char	   *infofuncname;
	PGFInfoFunction infofunc;
	const Pg_finfo_record *inforec;

	infofuncname = psprintf("pg_finfo_%s", funcname);

	/* 尝试查找信息函数 */
	infofunc = (PGFInfoFunction) lookup_external_function(filehandle,
														  infofuncname);
	if (infofunc == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not find function information for function \"%s\"",
						funcname),
				 errhint("SQL-callable functions need an accompanying PG_FUNCTION_INFO_V1(funcname).")));
		return NULL;			/* 安抚编译器 */
	}

	/* 找到即调用 */
	inforec = (*infofunc) ();

	/* 尽可能校验结果 */
	if (inforec == NULL)
		elog(ERROR, "null result from info function \"%s\"", infofuncname);
	switch (inforec->api_version)
	{
		case 1:
			/* OK，无其他需校验字段 */
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized API version %d reported by info function \"%s\"",
							inforec->api_version, infofuncname)));
			break;
	}

	pfree(infofuncname);
	return inforec;
}


/*-------------------------------------------------------------------------
 *	用于缓存外部 C 函数查找信息的例程。
 *
 * dfmgr.c 中的例程较慢，因此尽量避免对每个外部
 * 函数每会话只执行一次。我们使用以函数 OID 为键的
 * 哈希表来查找。
 *-------------------------------------------------------------------------
 */

/*
 * lookup_C_func：尝试在哈希表中查找 C 函数
 *
 * 若存在条目且为最新，则返回它；否则返回 NULL
 */
static CFuncHashTabEntry *
lookup_C_func(HeapTuple procedureTuple)
{
	Oid			fn_oid = ((Form_pg_proc) GETSTRUCT(procedureTuple))->oid;
	CFuncHashTabEntry *entry;

	if (CFuncHash == NULL)
		return NULL;			/* 尚无表 */
	entry = (CFuncHashTabEntry *)
		hash_search(CFuncHash,
					&fn_oid,
					HASH_FIND,
					NULL);
	if (entry == NULL)
		return NULL;			/* 无此条目 */
	if (entry->fn_xmin == HeapTupleHeaderGetRawXmin(procedureTuple->t_data) &&
		ItemPointerEquals(&entry->fn_tid, &procedureTuple->t_self))
		return entry;			/* OK */
	return NULL;				/* 条目已过期 */
}

/*
 * record_C_func：在哈希表中录入（或更新）C 函数信息
 */
static void
record_C_func(HeapTuple procedureTuple,
			  PGFunction user_fn, const Pg_finfo_record *inforec)
{
	Oid			fn_oid = ((Form_pg_proc) GETSTRUCT(procedureTuple))->oid;
	CFuncHashTabEntry *entry;
	bool		found;

	/* 若哈希表尚不存在则创建它 */
	if (CFuncHash == NULL)
	{
		HASHCTL		hash_ctl;

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(CFuncHashTabEntry);
		CFuncHash = hash_create("CFuncHash",
								100,
								&hash_ctl,
								HASH_ELEM | HASH_BLOBS);
	}

	entry = (CFuncHashTabEntry *)
		hash_search(CFuncHash,
					&fn_oid,
					HASH_ENTER,
					&found);
	/* OID 已填充完毕 */
	entry->fn_xmin = HeapTupleHeaderGetRawXmin(procedureTuple->t_data);
	entry->fn_tid = procedureTuple->t_self;
	entry->user_fn = user_fn;
	entry->inforec = inforec;
}


/*
 * 复制 FmgrInfo 结构
 *
 * 这本质上就有些不靠谱：语言相关的辅助信息
 * 无法可靠复制。我们的做法是把 fn_extra 清零，
 * 意味着辅助信息必须重新计算。
 */
void
fmgr_info_copy(FmgrInfo *dstinfo, FmgrInfo *srcinfo,
			   MemoryContext destcxt)
{
	memcpy(dstinfo, srcinfo, sizeof(FmgrInfo));
	dstinfo->fn_mcxt = destcxt;
	dstinfo->fn_extra = NULL;
}


/*
 * fmgr_internal_validator 专用查找例程：给定所称
 * 的内部函数名，返回该函数的 OID。
 * 若名称无法识别，则返回 InvalidOid。
 */
Oid
fmgr_internal_function(const char *proname)
{
	const FmgrBuiltin *fbp = fmgr_lookupByName(proname);

	if (fbp == NULL)
		return InvalidOid;
	return fbp->foid;
}


/*
 * 对安全定义者函数与使用 proconfig 的函数的支持。
 * 这两个特性由同一个调用处理程序支持，因为它们
 * 经常一起使用，两层调用处理程序既低效又（在表示
 * 上也显得混乱）没有必要。
 */
struct fmgr_security_definer_cache
{
	FmgrInfo	flinfo;			/* 目标函数的查找信息 */
	Oid			userid;			/* 待设 userid 或 InvalidOid */
	ArrayType  *proconfig;		/* 待设的 GUC 值或 NULL */
	Datum		arg;			/* 传递给插件模块的透传参数 */
};

/*
 * 安全定义者/proconfig/插件挂钩函数的处理函数。
 * 我们提取实际函数的 OID 并再次执行 fmgr 查找。
 * 然后获取 pg_proc 行并复制属主 ID 与 proconfig 字段。
 * （这些信息均在当前查询期间缓存。）
 * 执行调用时，我们临时将 flinfo 替换为缓存并查找
 * 得到的那个，同时保持外层 fcinfo（含全部实际参数
 * 等）完好无损。这不是可重入的，不过 fcinfo 本身
 * 反正也无法被重入使用。
 */
extern Datum
fmgr_security_definer(PG_FUNCTION_ARGS)
{
	Datum		result;
	struct fmgr_security_definer_cache *volatile fcache;
	FmgrInfo   *save_flinfo;
	Oid			save_userid;
	int			save_sec_context;
	volatile int save_nestlevel;
	PgStat_FunctionCallUsage fcusage;

	if (!fcinfo->flinfo->fn_extra)
	{
		HeapTuple	tuple;
		Form_pg_proc procedureStruct;
		Datum		datum;
		bool		isnull;
		MemoryContext oldcxt;

		fcache = MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
										sizeof(*fcache));

		fmgr_info_cxt_security(fcinfo->flinfo->fn_oid, &fcache->flinfo,
							   fcinfo->flinfo->fn_mcxt, true);
		fcache->flinfo.fn_expr = fcinfo->flinfo->fn_expr;

		tuple = SearchSysCache1(PROCOID,
								ObjectIdGetDatum(fcinfo->flinfo->fn_oid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u",
				 fcinfo->flinfo->fn_oid);
		procedureStruct = (Form_pg_proc) GETSTRUCT(tuple);

		if (procedureStruct->prosecdef)
			fcache->userid = procedureStruct->proowner;

		datum = SysCacheGetAttr(PROCOID, tuple, Anum_pg_proc_proconfig,
								&isnull);
		if (!isnull)
		{
			oldcxt = MemoryContextSwitchTo(fcinfo->flinfo->fn_mcxt);
			fcache->proconfig = DatumGetArrayTypePCopy(datum);
			MemoryContextSwitchTo(oldcxt);
		}

		ReleaseSysCache(tuple);

		fcinfo->flinfo->fn_extra = fcache;
	}
	else
		fcache = fcinfo->flinfo->fn_extra;

	/* GetUserIdAndSecContext 足够廉价，浪费一次调用无妨 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	if (fcache->proconfig)		/* 需要新 GUC 嵌套层级 */
		save_nestlevel = NewGUCNestLevel();
	else
		save_nestlevel = 0;		/* 让编译器安静 */

	if (OidIsValid(fcache->userid))
		SetUserIdAndSecContext(fcache->userid,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);

	if (fcache->proconfig)
	{
		ProcessGUCArray(fcache->proconfig,
						(superuser() ? PGC_SUSET : PGC_USERSET),
						PGC_S_SESSION,
						GUC_ACTION_SAVE);
	}

	/* 函数管理器钩子 */
	if (fmgr_hook)
		(*fmgr_hook) (FHET_START, &fcache->flinfo, &fcache->arg);

	/*
	 * 出错时无需恢复 GUC 或用户设置，因为随之而来的
	 * 事务或子事务中止会完成恢复。PG_TRY 块只是
	 * 用于清理 flinfo 链接。
	 */
	save_flinfo = fcinfo->flinfo;

	PG_TRY();
	{
		fcinfo->flinfo = &fcache->flinfo;

		/* 说明见 fmgr_info_cxt_security */
		pgstat_init_function_usage(fcinfo, &fcusage);

		result = FunctionCallInvoke(fcinfo);

		/*
		 * 我们调用的可能是普通函数或集合返回函数，
		 * 所以要测试该用哪个 finalize 标志。
		 */
		pgstat_end_function_usage(&fcusage,
								  (fcinfo->resultinfo == NULL ||
								   !IsA(fcinfo->resultinfo, ReturnSetInfo) ||
								   ((ReturnSetInfo *) fcinfo->resultinfo)->isDone != ExprMultipleResult));
	}
	PG_CATCH();
	{
		fcinfo->flinfo = save_flinfo;
		if (fmgr_hook)
			(*fmgr_hook) (FHET_ABORT, &fcache->flinfo, &fcache->arg);
		PG_RE_THROW();
	}
	PG_END_TRY();

	fcinfo->flinfo = save_flinfo;

	if (fcache->proconfig)
		AtEOXact_GUC(true, save_nestlevel);
	if (OidIsValid(fcache->userid))
		SetUserIdAndSecContext(save_userid, save_sec_context);
	if (fmgr_hook)
		(*fmgr_hook) (FHET_END, &fcache->flinfo, &fcache->arg);

	return result;
}


/*-------------------------------------------------------------------------
 *	为 fmgr 兼容函数的调用者提供的支持例程
 *-------------------------------------------------------------------------
 */

/*
 * 这些函数用于以直接算得的参数列表调用由名称
 * 指定的函数。注意参数与结果均不得为 NULL。
 * 另外，该函数不会是需要查看 FmgrInfo 的函数，
 * 因为根本不会有 FmgrInfo。
 */
Datum
DirectFunctionCall1Coll(PGFunction func, Oid collation, Datum arg1)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 1, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall2Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 2, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall3Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 3, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall4Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4)
{
	LOCAL_FCINFO(fcinfo, 4);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 4, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall5Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5)
{
	LOCAL_FCINFO(fcinfo, 5);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 5, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall6Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6)
{
	LOCAL_FCINFO(fcinfo, 6);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 6, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall7Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6, Datum arg7)
{
	LOCAL_FCINFO(fcinfo, 7);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 7, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall8Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6, Datum arg7, Datum arg8)
{
	LOCAL_FCINFO(fcinfo, 8);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 8, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
DirectFunctionCall9Coll(PGFunction func, Oid collation, Datum arg1, Datum arg2,
						Datum arg3, Datum arg4, Datum arg5,
						Datum arg6, Datum arg7, Datum arg8,
						Datum arg9)
{
	LOCAL_FCINFO(fcinfo, 9);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, NULL, 9, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;
	fcinfo->args[8].value = arg9;
	fcinfo->args[8].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

/*
 * 这些函数与 DirectFunctionCall 系列函数类似，但它们
 * 使用 flinfo 参数初始化本次调用的 fcinfo。建议
 * 被调函数只使用 fn_extra 与 fn_mcxt 字段，因为
 * 其他字段通常描述的是调用方函数而非被调函数。
 * 反之，调用方函数不应使用 fn_extra，除非已知
 * 其用法与被调函数的用法兼容。
 */

Datum
CallerFInfoFunctionCall1(PGFunction func, FmgrInfo *flinfo, Oid collation, Datum arg1)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 1, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

Datum
CallerFInfoFunctionCall2(PGFunction func, FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 2, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;

	result = (*func) (fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %p returned NULL", (void *) func);

	return result;
}

/*
 * 这些函数用于以直接算得的参数列表调用之前
 * 查找过的函数。注意参数与结果均不得为
 * NULL。
 */
Datum
FunctionCall0Coll(FmgrInfo *flinfo, Oid collation)
{
	LOCAL_FCINFO(fcinfo, 0);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 0, collation, NULL, NULL);

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall1Coll(FmgrInfo *flinfo, Oid collation, Datum arg1)
{
	LOCAL_FCINFO(fcinfo, 1);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 1, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall2Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 2, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall3Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall4Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4)
{
	LOCAL_FCINFO(fcinfo, 4);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 4, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall5Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5)
{
	LOCAL_FCINFO(fcinfo, 5);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 5, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall6Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6)
{
	LOCAL_FCINFO(fcinfo, 6);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 6, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall7Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6, Datum arg7)
{
	LOCAL_FCINFO(fcinfo, 7);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 7, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall8Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6, Datum arg7, Datum arg8)
{
	LOCAL_FCINFO(fcinfo, 8);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 8, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}

Datum
FunctionCall9Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
				  Datum arg3, Datum arg4, Datum arg5,
				  Datum arg6, Datum arg7, Datum arg8,
				  Datum arg9)
{
	LOCAL_FCINFO(fcinfo, 9);
	Datum		result;

	InitFunctionCallInfoData(*fcinfo, flinfo, 9, collation, NULL, NULL);

	fcinfo->args[0].value = arg1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = arg2;
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = arg3;
	fcinfo->args[2].isnull = false;
	fcinfo->args[3].value = arg4;
	fcinfo->args[3].isnull = false;
	fcinfo->args[4].value = arg5;
	fcinfo->args[4].isnull = false;
	fcinfo->args[5].value = arg6;
	fcinfo->args[5].isnull = false;
	fcinfo->args[6].value = arg7;
	fcinfo->args[6].isnull = false;
	fcinfo->args[7].value = arg8;
	fcinfo->args[7].isnull = false;
	fcinfo->args[8].value = arg9;
	fcinfo->args[8].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 检查空结果，因为调用者显然不期望空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", flinfo->fn_oid);

	return result;
}


/*
 * 这些函数用于以直接算得的参数列表调用由
 * OID 标识的函数。注意参数与结果均不得为
 * NULL。它们本质上就是先 fmgr_info() 再
 * FunctionCallN()。若同一函数要被反复调用，
 * 则做一次 fmgr_info() 后用 FunctionCallN()。
 */
Datum
OidFunctionCall0Coll(Oid functionId, Oid collation)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall0Coll(&flinfo, collation);
}

Datum
OidFunctionCall1Coll(Oid functionId, Oid collation, Datum arg1)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall1Coll(&flinfo, collation, arg1);
}

Datum
OidFunctionCall2Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall2Coll(&flinfo, collation, arg1, arg2);
}

Datum
OidFunctionCall3Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall3Coll(&flinfo, collation, arg1, arg2, arg3);
}

Datum
OidFunctionCall4Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall4Coll(&flinfo, collation, arg1, arg2, arg3, arg4);
}

Datum
OidFunctionCall5Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall5Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5);
}

Datum
OidFunctionCall6Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall6Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6);
}

Datum
OidFunctionCall7Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6, Datum arg7)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall7Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6, arg7);
}

Datum
OidFunctionCall8Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6, Datum arg7, Datum arg8)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall8Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6, arg7, arg8);
}

Datum
OidFunctionCall9Coll(Oid functionId, Oid collation, Datum arg1, Datum arg2,
					 Datum arg3, Datum arg4, Datum arg5,
					 Datum arg6, Datum arg7, Datum arg8,
					 Datum arg9)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);

	return FunctionCall9Coll(&flinfo, collation, arg1, arg2, arg3, arg4, arg5,
							 arg6, arg7, arg8, arg9);
}


/*
 * 为方便调用数据类型 I/O 函数的特殊用例。
 */

/*
 * 调用之前查找过的数据类型输入函数。
 *
 * "str" 可为 NULL 表示正在读取 NULL。这种情况下
 * 调用者应假定结果为 NULL，但若输入函数非严格
 * 我们仍会调用它。所以这与 FunctionCall3 几乎
 * 一样但不尽相同。
 */
Datum
InputFunctionCall(FmgrInfo *flinfo, char *str, Oid typioparam, int32 typmod)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	if (str == NULL && flinfo->fn_strict)
		return (Datum) 0;		/* 直接返回空结果 */

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid, NULL, NULL);

	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 当且仅当 str 为 NULL 才得空结果 */
	if (str == NULL)
	{
		if (!fcinfo->isnull)
			elog(ERROR, "input function %u returned non-NULL",
				 flinfo->fn_oid);
	}
	else
	{
		if (fcinfo->isnull)
			elog(ERROR, "input function %u returned NULL",
				 flinfo->fn_oid);
	}

	return result;
}

/*
 * 调用之前查找过的数据类型输入函数，以非异常方式
 * 处理"软"错误。
 *
 * 这基本上与 InputFunctionCall 类似，但转换得到的
 * Datum 被返回到 *result 中，而函数结果在成功时为
 * true，失败时为 false。此外，调用者可传入
 * （将其声明为"fmNodePtr"以免在 fmgr.h 中包含 nodes.h。）
 *
 * 若 escontext 指向 ErrorSaveContext，则输入函数检测
 * 到的任何"软"错误都会通过填充 escontext 结构体并
 * 返回 false 来报告。（调用者可以选择测试
 * SOFT_ERROR_OCCURRED()，但检查结果通常更廉价。）
 *
 * 若 escontext 不指向 ErrorSaveContext，则错误经
 * ereport(ERROR) 报告，因此在功能上与
 * InputFunctionCall 无差别；控制返回时结果必为 true。
 */
bool
InputFunctionCallSafe(FmgrInfo *flinfo, char *str,
					  Oid typioparam, int32 typmod,
					  fmNodePtr escontext,
					  Datum *result)
{
	LOCAL_FCINFO(fcinfo, 3);

	if (str == NULL && flinfo->fn_strict)
	{
		*result = (Datum) 0;	/* 直接返回空结果 */
		return true;
	}

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid, escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	*result = FunctionCallInvoke(fcinfo);

	/* 已报错时结果值是无意义的，且可能为空 */
	if (SOFT_ERROR_OCCURRED(escontext))
		return false;

	/* 否则，当且仅当 str 为 NULL 才得空结果 */
	if (str == NULL)
	{
		if (!fcinfo->isnull)
			elog(ERROR, "input function %u returned non-NULL",
				 flinfo->fn_oid);
	}
	else
	{
		if (fcinfo->isnull)
			elog(ERROR, "input function %u returned NULL",
				 flinfo->fn_oid);
	}

	return true;
}

/*
 * 调用直接指名的数据类型输入函数，非异常方式
 * 处理"软"错误。
 *
 * 这与 InputFunctionCallSafe 类似，但给定的是
 * 指向要调用的 C 函数的直接指针。我们假定该函数
 * 是严格的。此外，该函数不会是需要查看
 * FmgrInfo 的函数，因为不会有。
 */
bool
DirectInputFunctionCallSafe(PGFunction func, char *str,
							Oid typioparam, int32 typmod,
							fmNodePtr escontext,
							Datum *result)
{
	LOCAL_FCINFO(fcinfo, 3);

	if (str == NULL)
	{
		*result = (Datum) 0;	/* 直接返回空结果 */
		return true;
	}

	InitFunctionCallInfoData(*fcinfo, NULL, 3, InvalidOid, escontext, NULL);

	fcinfo->args[0].value = CStringGetDatum(str);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	*result = (*func) (fcinfo);

	/* 已报错时结果值是无意义的，且可能为空 */
	if (SOFT_ERROR_OCCURRED(escontext))
		return false;

	/* 否则不应得到空结果 */
	if (fcinfo->isnull)
		elog(ERROR, "input function %p returned NULL", (void *) func);

	return true;
}

/*
 * 调用之前查找过的数据类型输出函数。
 *
 * 勿对 NULL 调用此函数。
 *
 * 目前这几乎只是 FunctionCall1 的门面装饰。
 */
char *
OutputFunctionCall(FmgrInfo *flinfo, Datum val)
{
	return DatumGetCString(FunctionCall1(flinfo, val));
}

/*
 * 调用先前查到的数据类型二进制输入函数。
 *
 * "buf" 可为 NULL 表示正在读取 NULL。这种情况下
 * 调用者应假定结果为 NULL，但若接收函数非严格
 * 我们仍会调用它。所以这与 FunctionCall3 几乎
 * 一样但不尽相同。
 */
Datum
ReceiveFunctionCall(FmgrInfo *flinfo, StringInfo buf,
					Oid typioparam, int32 typmod)
{
	LOCAL_FCINFO(fcinfo, 3);
	Datum		result;

	if (buf == NULL && flinfo->fn_strict)
		return (Datum) 0;		/* 直接返回空结果 */

	InitFunctionCallInfoData(*fcinfo, flinfo, 3, InvalidOid, NULL, NULL);

	fcinfo->args[0].value = PointerGetDatum(buf);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = ObjectIdGetDatum(typioparam);
	fcinfo->args[1].isnull = false;
	fcinfo->args[2].value = Int32GetDatum(typmod);
	fcinfo->args[2].isnull = false;

	result = FunctionCallInvoke(fcinfo);

	/* 当且仅当 buf 为 NULL 才得空结果 */
	if (buf == NULL)
	{
		if (!fcinfo->isnull)
			elog(ERROR, "receive function %u returned non-NULL",
				 flinfo->fn_oid);
	}
	else
	{
		if (fcinfo->isnull)
			elog(ERROR, "receive function %u returned NULL",
				 flinfo->fn_oid);
	}

	return result;
}

/*
 * 调用之前查找过的数据类型二进制输出函数。
 *
 * 勿对 NULL 调用此函数。
 *
 * 这几乎只是 FunctionCall1 的门面装饰，但它确实
 * 保证结果非 toast，而底层函数严格来说不
 * 保证此点。
 */
bytea *
SendFunctionCall(FmgrInfo *flinfo, Datum val)
{
	return DatumGetByteaP(FunctionCall1(flinfo, val));
}

/*
 * 同上，但针对由 OID 标识的 I/O 函数。这些只能用于
 * 很少执行的代码路径。它们不仅慢还会泄漏内存。
 */
Datum
OidInputFunctionCall(Oid functionId, char *str, Oid typioparam, int32 typmod)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return InputFunctionCall(&flinfo, str, typioparam, typmod);
}

char *
OidOutputFunctionCall(Oid functionId, Datum val)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return OutputFunctionCall(&flinfo, val);
}

Datum
OidReceiveFunctionCall(Oid functionId, StringInfo buf,
					   Oid typioparam, int32 typmod)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return ReceiveFunctionCall(&flinfo, buf, typioparam, typmod);
}

bytea *
OidSendFunctionCall(Oid functionId, Datum val)
{
	FmgrInfo	flinfo;

	fmgr_info(functionId, &flinfo);
	return SendFunctionCall(&flinfo, val);
}


/*-------------------------------------------------------------------------
 *	标准可能按引用传递数据类型的支持例程
 *
 * 若 Datum 足够宽，int8 与 float8 可按值传递。
 * （出于向后兼容的原因，即使在可能按值传递时，也
 * 允许在编译期选择按引用传递。）
 *
 * 注意：只有一个开关同时控制 int8 与 float8 的
 * 按值传递选项；这是为了避免让 timestamp 类型
 * 的情况过于复杂，因为它们可能有两种表示。
 *-------------------------------------------------------------------------
 */

#ifndef USE_FLOAT8_BYVAL		/* 也控制 int8 */

Datum
Int64GetDatum(int64 X)
{
	int64	   *retval = (int64 *) palloc(sizeof(int64));

	*retval = X;
	return PointerGetDatum(retval);
}

Datum
Float8GetDatum(float8 X)
{
	float8	   *retval = (float8 *) palloc(sizeof(float8));

	*retval = X;
	return PointerGetDatum(retval);
}
#endif							/* USE_FLOAT8_BYVAL */


/*-------------------------------------------------------------------------
 *	可 toast 数据类型的支持例程
 *-------------------------------------------------------------------------
 */

struct varlena *
pg_detoast_datum(struct varlena *datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return detoast_attr(datum);
	else
		return datum;
}

struct varlena *
pg_detoast_datum_copy(struct varlena *datum)
{
	if (VARATT_IS_EXTENDED(datum))
		return detoast_attr(datum);
	else
	{
		/* 制作 varlena 对象的可修改副本 */
		Size		len = VARSIZE(datum);
		struct varlena *result = (struct varlena *) palloc(len);

		memcpy(result, datum, len);
		return result;
	}
}

struct varlena *
pg_detoast_datum_slice(struct varlena *datum, int32 first, int32 count)
{
	/* 只从 toast 表获取指定部分 */
	return detoast_attr_slice(datum, first, count);
}

struct varlena *
pg_detoast_datum_packed(struct varlena *datum)
{
	if (VARATT_IS_COMPRESSED(datum) || VARATT_IS_EXTERNAL(datum))
		return detoast_attr(datum);
	else
		return datum;
}

/*-------------------------------------------------------------------------
 *	从 fn_expr 语法树提取信息的支持例程
 *
 * 多态函数需要这些例程；多态函数接受多种可能的
 * 输入类型，需要解析器帮忙判断收到的是什么。
 * 此外，有些函数可能想知道某参数是否为常量。
 * 接受 VARIADIC ANY 的函数还需要知道 VARIADIC 关键字。
 *-------------------------------------------------------------------------
 */

/*
 * 获取函数返回类型的实际类型 OID
 *
 * 若信息不可用则返回 InvalidOid
 */
Oid
get_fn_expr_rettype(FmgrInfo *flinfo)
{
	Node	   *expr;

	/*
	 * 若没有 FmgrInfo 或其 fn_expr 节点尚未初始化，
	 * 则无法返回有用信息
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	expr = flinfo->fn_expr;

	return exprType(expr);
}

/*
 * 获取指定函数参数（从 0 计）的实际类型 OID
 *
 * 若信息不可用则返回 InvalidOid
 */
Oid
get_fn_expr_argtype(FmgrInfo *flinfo, int argnum)
{
	/*
	 * 若没有 FmgrInfo 或其 fn_expr 节点尚未初始化，
	 * 则无法返回有用信息
	 */
	if (!flinfo || !flinfo->fn_expr)
		return InvalidOid;

	return get_call_expr_argtype(flinfo->fn_expr, argnum);
}

/*
 * 获取指定函数参数（从 0 计）的实际类型 OID，
 * 但改为从调用表达式树入手而非 FmgrInfo
 *
 * 若信息不可用则返回 InvalidOid
 */
Oid
get_call_expr_argtype(Node *expr, int argnum)
{
	List	   *args;
	Oid			argtype;

	if (expr == NULL)
		return InvalidOid;

	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, ScalarArrayOpExpr))
		args = ((ScalarArrayOpExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else if (IsA(expr, WindowFunc))
		args = ((WindowFunc *) expr)->args;
	else
		return InvalidOid;

	if (argnum < 0 || argnum >= list_length(args))
		return InvalidOid;

	argtype = exprType((Node *) list_nth(args, argnum));

	/*
	 * ScalarArrayOpExpr 的特殊处理：底层函数实际
	 * 收到的是数组的元素类型。
	 */
	if (IsA(expr, ScalarArrayOpExpr) &&
		argnum == 1)
		argtype = get_base_element_type(argtype);

	return argtype;
}

/*
 * 判断某个特定函数参数在查询期间
 * 是否为常量
 *
 * 若信息不可用则返回 false
 */
bool
get_fn_expr_arg_stable(FmgrInfo *flinfo, int argnum)
{
	/*
	 * 若没有 FmgrInfo 或其 fn_expr 节点尚未初始化，
	 * 则无法返回有用信息
	 */
	if (!flinfo || !flinfo->fn_expr)
		return false;

	return get_call_expr_arg_stable(flinfo->fn_expr, argnum);
}

/*
 * 判断某个特定函数参数在查询期间是否为常量，
 * 但改为从调用表达式树入手
 *
 * 若信息不可用则返回 false
 */
bool
get_call_expr_arg_stable(Node *expr, int argnum)
{
	List	   *args;
	Node	   *arg;

	if (expr == NULL)
		return false;

	if (IsA(expr, FuncExpr))
		args = ((FuncExpr *) expr)->args;
	else if (IsA(expr, OpExpr))
		args = ((OpExpr *) expr)->args;
	else if (IsA(expr, DistinctExpr))
		args = ((DistinctExpr *) expr)->args;
	else if (IsA(expr, ScalarArrayOpExpr))
		args = ((ScalarArrayOpExpr *) expr)->args;
	else if (IsA(expr, NullIfExpr))
		args = ((NullIfExpr *) expr)->args;
	else if (IsA(expr, WindowFunc))
		args = ((WindowFunc *) expr)->args;
	else
		return false;

	if (argnum < 0 || argnum >= list_length(args))
		return false;

	arg = (Node *) list_nth(args, argnum);

	/*
	 * 真正的 Const 或外部 Param 的值在查询执行期间
	 * 不会变化。将来我们可能还想考虑其他情形，
	 * 例如 now()。
	 */
	if (IsA(arg, Const))
		return true;
	if (IsA(arg, Param) &&
		((Param *) arg)->paramkind == PARAM_EXTERN)
		return true;

	return false;
}

/*
 * 从函数调用中获取 VARIADIC 标志
 *
 * 若信息不可用则返回 false（默认假定）
 *
 * 注意这通常只有 VARIADIC ANY 函数会关心
 */
bool
get_fn_expr_variadic(FmgrInfo *flinfo)
{
	Node	   *expr;

	/*
	 * 若没有 FmgrInfo 或其 fn_expr 节点尚未初始化，
	 * 则无法返回有用信息
	 */
	if (!flinfo || !flinfo->fn_expr)
		return false;

	expr = flinfo->fn_expr;

	if (IsA(expr, FuncExpr))
		return ((FuncExpr *) expr)->funcvariadic;
	else
		return false;
}

/*
 * 设置 opclass 支持函数的 FmgrInfo 选项
 *
 * opclass 支持函数在表达式之外被调用。正因如此，
 * 我们可以用 fn_expr 把 opclass 选项存为 bytea 常量
 */
void
set_fn_opclass_options(FmgrInfo *flinfo, bytea *options)
{
	flinfo->fn_expr = (Node *) makeConst(BYTEAOID, -1, InvalidOid, -1,
										 PointerGetDatum(options),
										 options == NULL, false);
}

/*
 * 检查 opclass 支持函数是否定义了选项。
 */
bool
has_fn_opclass_options(FmgrInfo *flinfo)
{
	if (flinfo && flinfo->fn_expr && IsA(flinfo->fn_expr, Const))
	{
		Const	   *expr = (Const *) flinfo->fn_expr;

		if (expr->consttype == BYTEAOID)
			return !expr->constisnull;
	}
	return false;
}

/*
 * 获取 opclass 支持函数选项。
 */
bytea *
get_fn_opclass_options(FmgrInfo *flinfo)
{
	if (flinfo && flinfo->fn_expr && IsA(flinfo->fn_expr, Const))
	{
		Const	   *expr = (Const *) flinfo->fn_expr;

		if (expr->consttype == BYTEAOID)
			return expr->constisnull ? NULL : DatumGetByteaP(expr->constvalue);
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("operator class options info is absent in function call context")));

	return NULL;
}

/*-------------------------------------------------------------------------
 *	面向过程语言实现的支持例程
 *-------------------------------------------------------------------------
 */

/*
 * 校验验证器确实与特定函数的语言相关联，且用户
 * 对该语言和该函数都有访问权限。所有验证器都应在
 * 做任何实质性工作之前调用此函数。这样做能确保
 * 用户无法通过显式调用验证器做到任何
 * 用 CREATE FUNCTION 或简单调用现有函数
 * 都做不到的事。
 *
 * 本函数返回 false 时，调用者应跳过全部校验工作
 * 并调用 PG_RETURN_VOID()。目前这从不发生；保留
 * 供将来扩展。
 *
 * 尤其是校验验证器与函数语言相对应，可以让非可信
 * 语言的验证器假定自己只处理超级用户选择的源码。
 * （非可信语言的调用处理程序按定义也如此假定）
 * 缺少语言 USAGE 特权者无法经 CREATE FUNCTION 到达
 * 验证器，所以我们也要检查这一点以阻止显式调用。
 * 检查函数上的 EXECUTE 特权常常是多余的，因为多数
 * 用户都能克隆函数得到可执行的副本。这一检查对
 * 没有数据库 TEMP 权限且没有永久模式 CREATE 权限的
 * 用户才有意义，因为这类用户无法创建任何函数。
 * 另外，若函数通过函数 OID 或名称跟踪持久状态，
 * 校验原函数可能比创建并校验其克隆允许
 * 更多的恶作剧。
 */
bool
CheckFunctionValidatorAccess(Oid validatorOid, Oid functionOid)
{
	HeapTuple	procTup;
	HeapTuple	langTup;
	Form_pg_proc procStruct;
	Form_pg_language langStruct;
	AclResult	aclresult;

	/*
	 * 获取函数的 pg_proc 条目。对坏 OID 抛出面向用户
	 * 的错误，因为验证器可能收到用户指定的 OIDs。
	 */
	procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionOid));
	if (!HeapTupleIsValid(procTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function with OID %u does not exist", functionOid)));
	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	/*
	 * 获取 pg_language 条目，确认这是该 pg_proc
	 * 条目对应的校验函数。
	 */
	langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(procStruct->prolang));
	if (!HeapTupleIsValid(langTup))
		elog(ERROR, "cache lookup failed for language %u", procStruct->prolang);
	langStruct = (Form_pg_language) GETSTRUCT(langTup);

	if (langStruct->lanvalidator != validatorOid)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("language validation function %u called for language %u instead of %u",
						validatorOid, procStruct->prolang,
						langStruct->lanvalidator)));

	/* 先校验我们有权使用该语言 */
	aclresult = object_aclcheck(LanguageRelationId, procStruct->prolang, GetUserId(),
								ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_LANGUAGE,
					   NameStr(langStruct->lanname));

	/*
	 * 检查我们是否被允许执行函数本身。如果能执行它，
	 * 则编译/校验不应有执行做不到的
	 * 副作用。
	 */
	aclresult = object_aclcheck(ProcedureRelationId, functionOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, NameStr(procStruct->proname));

	ReleaseSysCache(procTup);
	ReleaseSysCache(langTup);

	return true;
}
