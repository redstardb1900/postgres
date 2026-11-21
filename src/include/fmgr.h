/*-------------------------------------------------------------------------
 *
 * fmgr.h
 *	Postgres 函数管理器与函数调用接口的
 *	定义。
 *
 * 凡定义或调用 fmgr 可调函数的 Postgres 模块
 * 皆须包含本文件。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FMGR_H
#define FMGR_H

/* 不想在此含 primnodes.h，故做若干桩引用 */
typedef struct Node *fmNodePtr;
typedef struct Aggref *fmAggrefPtr;

/* 同理，避免在此含 execnodes.h */
typedef void (*fmExprContextCallbackFunction) (Datum arg);

/* 同理，避免在此含 stringinfo.h */
typedef struct StringInfoData *fmStringInfo;


/*
 * 凡可被 fmgr 直接调用的函数须有此签名。
 * （其他函数可通过具有此签名的处理程序
 * 调用。
 */

typedef struct FunctionCallInfoBaseData *FunctionCallInfo;

typedef Datum (*PGFunction) (FunctionCallInfo fcinfo);

/*
 * 本结构保存经 fmgr 调用函数前必须查找的
 * 系统目录信息。若同一函数将被
 * 多次调用，查找只需做一次，并把
 * 信息结构保存复用。
 *
 * 注意 fn_expr 实为解析期确定的关于实参的
 * 信息，而非关于函数本身。但放在此处
 * 比放在 FunctionCallInfoBaseData 更方便，虽后者
 * 更合适。
 *
 * fn_extra 供被调函数使用；其余字段在结构
 * 创建后应视为只读。
 */
typedef struct FmgrInfo
{
	PGFunction	fn_addr;		/* 待调函数或处理程序的指针 */
	Oid			fn_oid;			/* 函数 OID（非处理程序，若有） */
	short		fn_nargs;		/* 输入参数个数(0..FUNC_MAX_ARGS) */
	bool		fn_strict;		/* 函数"严格"（NULL 入 => NULL 出） */
	bool		fn_retset;		/* 函数返回集合 */
	unsigned char fn_stats;		/* track_functions > 此则收集统计 */
	void	   *fn_extra;		/* 处理程序可用额外空间 */
	MemoryContext fn_mcxt;		/* 存放 fn_extra 的内存上下文 */
	fmNodePtr	fn_expr;		/* 调用的表达式解析树，或 NULL */
} FmgrInfo;

/*
 * 本结构是实际传给 fmgr 所调函数的数据。
 *
 * 被调函数应设置 isnull，并可能设置 resultinfo 或
 * resultinfo 所指对象中的字段。不应改其他
 * 字段。（尤其勿涂改参数数组，
 * 因有些调用方假定可用相同参数再调。）
 *
 * 注意须为参数提供足够空间，可用
 * SizeForFunctionCallInfo() 做动态分配，或用
 * LOCAL_FCINFO() 做栈上分配。
 *
 * 本结构名 *BaseData 而非 *Data，以打断 v12 前自行
 * 分配 FunctionCallInfoData 的代码——常因未为参数
 * 留空间而静默破坏旧代码。
 */
typedef struct FunctionCallInfoBaseData
{
	FmgrInfo   *flinfo;			/* 本次调用所用查找信息指针 */
	fmNodePtr	context;		/* 传入调用上下文信息 */
	fmNodePtr	resultinfo;		/* 传入或返回结果的额外信息 */
	Oid			fncollation;	/* 函数所用排序规则 */
#define FIELDNO_FUNCTIONCALLINFODATA_ISNULL 4
	bool		isnull;			/* 若结果为 NULL，函数须置 true */
	short		nargs;			/* 实际传入的参数个数 */
#define FIELDNO_FUNCTIONCALLINFODATA_ARGS 6
	NullableDatum args[FLEXIBLE_ARRAY_MEMBER];
} FunctionCallInfoBaseData;

/*
 * FunctionCallInfoBaseData 所需空间，并留足 `nargs` 参数
 * 空间。
 */
#define SizeForFunctionCallInfo(nargs) \
	(offsetof(FunctionCallInfoBaseData, args) + \
	 sizeof(NullableDatum) * (nargs))

/*
 * 本宏保证 `name` 指向栈分配的
 * FunctionCallInfoBaseData，且有足够空间容纳 `nargs` 个参数。
 */
#define LOCAL_FCINFO(name, nargs) \
	/* 与 FunctionCallInfoBaseData 的联合以保证对齐 */ \
	union \
	{ \
		FunctionCallInfoBaseData fcinfo; \
		/* 确保有足够空间容纳 nargs 个参数 */ \
		char fcinfo_data[SizeForFunctionCallInfo(nargs)]; \
	} name##data; \
	FunctionCallInfo name = &name##data.fcinfo

/*
 * 给定待调函数 OID，填充 FmgrInfo
 * 结构。
 */
extern void fmgr_info(Oid functionId, FmgrInfo *finfo);

/*
 * 同上，当 FmgrInfo 处于比 CurrentMemoryContext
 * 更长寿的内存上下文时。指定上下文将设为 fn_mcxt
 * 并用于保存 finfo 的一切附属数据。
 */
extern void fmgr_info_cxt(Oid functionId, FmgrInfo *finfo,
						  MemoryContext mcxt);

/* 设置 fn_expr 字段的便利宏 */
#define fmgr_info_set_expr(expr, finfo) \
	((finfo)->fn_expr = (expr))

/*
 * 复制 FmgrInfo 结构
 */
extern void fmgr_info_copy(FmgrInfo *dstinfo, FmgrInfo *srcinfo,
						   MemoryContext destcxt);

extern void fmgr_symbol(Oid functionId, char **mod, char **fn);

/*
 * 本宏初始化 FunctionCallInfoBaseData 除 args[] 外的
 * 全部字段。
 */
#define InitFunctionCallInfoData(Fcinfo, Flinfo, Nargs, Collation, Context, Resultinfo) \
	do { \
		(Fcinfo).flinfo = (Flinfo); \
		(Fcinfo).context = (Context); \
		(Fcinfo).resultinfo = (Resultinfo); \
		(Fcinfo).fncollation = (Collation); \
		(Fcinfo).isnull = false; \
		(Fcinfo).nargs = (Nargs); \
	} while (0)

/*
 * 给定已填好的 FunctionCallInfoBaseData，本宏调用函数。
 * 宏结果为返回的 Datum——但注意
 * 调用方仍须检查 fcinfo->isnull！若函数为严格，
 * 验证无空参亦是调用方责任，
 * 再调用。
 *
 * 有些代码多次调用而不重做 InitFunctionCallInfoData，
 * 或改参数值。可以，但每次调用前务必复位
 * fcinfo->isnull，因被调方可假定
 * 其初值为 false。
 */
#define FunctionCallInvoke(fcinfo)	((* (fcinfo)->flinfo->fn_addr) (fcinfo))


/*-------------------------------------------------------------------------
 *	便于编写 fmgr 兼容函数的支持宏
 *
 * C 编写的 fmgr 兼容函数应声明为
 *
 *		Datum
 *		function_name(PG_FUNCTION_ARGS)
 *		{
 *			...
 *		}
 *
 * 应使用适当的 PG_GETARG_xxx 宏取参，
 * 并用 PG_RETURN_xxx 返回结果。
 *
 *-------------------------------------------------------------------------
 */

/* fmgr 兼容函数的标准参数表 */
#define PG_FUNCTION_ARGS	FunctionCallInfo fcinfo

/*
 * 取函数应用的排序规则。
 */
#define PG_GET_COLLATION()	(fcinfo->fncollation)

/*
 * 取得传给函数的参数个数。
 */
#define PG_NARGS() (fcinfo->nargs)

/*
 * 若函数在 pg_proc 中未标 "proisstrict"，须用本宏
 * 检查空参。勿对空参做 GETARG！
 */
#define PG_ARGISNULL(n)  (fcinfo->args[n].isnull)

/*
 * 取可 toast 类型（皆 varlena）去 toast 副本的支持。
 * pg_detoast_datum() 返回输入 datum（若未 toast）或
 * 用 palloc() 分配的去 toast 副本。
 * pg_detoast_datum_copy() 总返回 palloc 副本——若需
 * 可修改的输入副本则用之。调用方应已先
 * 在必要时检查过空输入。
 *
 * pg_detoast_datum_packed() 原样返回 1 字节头 datum。
 * 仍会展开外部 toast 或压缩的 datum。
 * 结果可用 VARSIZE_ANY() 与 VARDATA_ANY() 访问
 * （当心这些宏中的多次求值！）
 *
 * 对对齐不敏感的消费者，调用 PG_DETOAST_DATUM_PACKED()、
 * VARDATA_ANY()、VARSIZE_ANY() 与 VARSIZE_ANY_EXHDR()。他处调用
 * PG_DETOAST_DATUM()、VARDATA()、VARSIZE()。直接取布局中的
 * int16、int32 或更宽字段需要
 * 对齐数据。memcpy() 对对齐不敏感，多数仅含 char 字段的
 * 数据类型（如 text）上的操作亦然。
 *
 * 注意：做成宏更好，但未见不重复求值参数的做法，
 * 而重复求值不可接受。
 */
extern struct varlena *pg_detoast_datum(struct varlena *datum);
extern struct varlena *pg_detoast_datum_copy(struct varlena *datum);
extern struct varlena *pg_detoast_datum_slice(struct varlena *datum,
											  int32 first, int32 count);
extern struct varlena *pg_detoast_datum_packed(struct varlena *datum);

#define PG_DETOAST_DATUM(datum) \
	pg_detoast_datum((struct varlena *) DatumGetPointer(datum))
#define PG_DETOAST_DATUM_COPY(datum) \
	pg_detoast_datum_copy((struct varlena *) DatumGetPointer(datum))
#define PG_DETOAST_DATUM_SLICE(datum,f,c) \
		pg_detoast_datum_slice((struct varlena *) DatumGetPointer(datum), \
		(int32) (f), (int32) (c))
/* WARNING -- 未对齐指针 */
#define PG_DETOAST_DATUM_PACKED(datum) \
	pg_detoast_datum_packed((struct varlena *) DatumGetPointer(datum))

/*
 * 清理输入去 toast 副本的支持。仅可用于
 * 按引用传递的数据类型，通常仅用于
 * 可 toast 类型。若给定指针不同于
 * 原参数，则视为 palloc 的去 toast 副本并 pfree。
 * 注意：多数可 toast 类型上的函数无须关心此，
 * 但目前要求索引支持函数不泄漏
 * 内存
 */
#define PG_FREE_IF_COPY(ptr,n) \
	do { \
		if ((Pointer) (ptr) != PG_GETARG_POINTER(n)) \
			pfree(ptr); \
	} while (0)

/* 取标准类型参数的宏 */

#define PG_GETARG_DATUM(n)	 (fcinfo->args[n].value)
#define PG_GETARG_INT32(n)	 DatumGetInt32(PG_GETARG_DATUM(n))
#define PG_GETARG_UINT32(n)  DatumGetUInt32(PG_GETARG_DATUM(n))
#define PG_GETARG_INT16(n)	 DatumGetInt16(PG_GETARG_DATUM(n))
#define PG_GETARG_UINT16(n)  DatumGetUInt16(PG_GETARG_DATUM(n))
#define PG_GETARG_CHAR(n)	 DatumGetChar(PG_GETARG_DATUM(n))
#define PG_GETARG_BOOL(n)	 DatumGetBool(PG_GETARG_DATUM(n))
#define PG_GETARG_OID(n)	 DatumGetObjectId(PG_GETARG_DATUM(n))
#define PG_GETARG_POINTER(n) DatumGetPointer(PG_GETARG_DATUM(n))
#define PG_GETARG_CSTRING(n) DatumGetCString(PG_GETARG_DATUM(n))
#define PG_GETARG_NAME(n)	 DatumGetName(PG_GETARG_DATUM(n))
#define PG_GETARG_TRANSACTIONID(n)	DatumGetTransactionId(PG_GETARG_DATUM(n))
/* 这些宏隐藏数据类型是否按引用传递： */
#define PG_GETARG_FLOAT4(n)  DatumGetFloat4(PG_GETARG_DATUM(n))
#define PG_GETARG_FLOAT8(n)  DatumGetFloat8(PG_GETARG_DATUM(n))
#define PG_GETARG_INT64(n)	 DatumGetInt64(PG_GETARG_DATUM(n))
/* 若要原始、可能仍 toast 的输入 datum 用此： */
#define PG_GETARG_RAW_VARLENA_P(n)	((struct varlena *) PG_GETARG_POINTER(n))
/* 若要去 toast 的输入 datum 用此： */
#define PG_GETARG_VARLENA_P(n) PG_DETOAST_DATUM(PG_GETARG_DATUM(n))
/* 若能处理 1 字节头 datum 用此： */
#define PG_GETARG_VARLENA_PP(n) PG_DETOAST_DATUM_PACKED(PG_GETARG_DATUM(n))
/* varlena 类型的 DatumGetFoo 宏通常形如： */
#define DatumGetByteaPP(X)			((bytea *) PG_DETOAST_DATUM_PACKED(X))
#define DatumGetTextPP(X)			((text *) PG_DETOAST_DATUM_PACKED(X))
#define DatumGetBpCharPP(X)			((BpChar *) PG_DETOAST_DATUM_PACKED(X))
#define DatumGetVarCharPP(X)		((VarChar *) PG_DETOAST_DATUM_PACKED(X))
#define DatumGetHeapTupleHeader(X)	((HeapTupleHeader) PG_DETOAST_DATUM(X))
/* 亦提供返回可写副本的变体 */
#define DatumGetByteaPCopy(X)		((bytea *) PG_DETOAST_DATUM_COPY(X))
#define DatumGetTextPCopy(X)		((text *) PG_DETOAST_DATUM_COPY(X))
#define DatumGetBpCharPCopy(X)		((BpChar *) PG_DETOAST_DATUM_COPY(X))
#define DatumGetVarCharPCopy(X)		((VarChar *) PG_DETOAST_DATUM_COPY(X))
#define DatumGetHeapTupleHeaderCopy(X)	((HeapTupleHeader) PG_DETOAST_DATUM_COPY(X))
/* 返回自位置 m 起 n 字节的变体 */
#define DatumGetByteaPSlice(X,m,n)	((bytea *) PG_DETOAST_DATUM_SLICE(X,m,n))
#define DatumGetTextPSlice(X,m,n)	((text *) PG_DETOAST_DATUM_SLICE(X,m,n))
#define DatumGetBpCharPSlice(X,m,n) ((BpChar *) PG_DETOAST_DATUM_SLICE(X,m,n))
#define DatumGetVarCharPSlice(X,m,n) ((VarChar *) PG_DETOAST_DATUM_SLICE(X,m,n))
/* varlena 类型的 GETARG 宏通常形如： */
#define PG_GETARG_BYTEA_PP(n)		DatumGetByteaPP(PG_GETARG_DATUM(n))
#define PG_GETARG_TEXT_PP(n)		DatumGetTextPP(PG_GETARG_DATUM(n))
#define PG_GETARG_BPCHAR_PP(n)		DatumGetBpCharPP(PG_GETARG_DATUM(n))
#define PG_GETARG_VARCHAR_PP(n)		DatumGetVarCharPP(PG_GETARG_DATUM(n))
#define PG_GETARG_HEAPTUPLEHEADER(n)	DatumGetHeapTupleHeader(PG_GETARG_DATUM(n))
/* 亦提供返回可写副本的变体 */
#define PG_GETARG_BYTEA_P_COPY(n)	DatumGetByteaPCopy(PG_GETARG_DATUM(n))
#define PG_GETARG_TEXT_P_COPY(n)	DatumGetTextPCopy(PG_GETARG_DATUM(n))
#define PG_GETARG_BPCHAR_P_COPY(n)	DatumGetBpCharPCopy(PG_GETARG_DATUM(n))
#define PG_GETARG_VARCHAR_P_COPY(n) DatumGetVarCharPCopy(PG_GETARG_DATUM(n))
#define PG_GETARG_HEAPTUPLEHEADER_COPY(n)	DatumGetHeapTupleHeaderCopy(PG_GETARG_DATUM(n))
/* 自位置 a 起 b 字节切片——亦可写 */
#define PG_GETARG_BYTEA_P_SLICE(n,a,b) DatumGetByteaPSlice(PG_GETARG_DATUM(n),a,b)
#define PG_GETARG_TEXT_P_SLICE(n,a,b)  DatumGetTextPSlice(PG_GETARG_DATUM(n),a,b)
#define PG_GETARG_BPCHAR_P_SLICE(n,a,b) DatumGetBpCharPSlice(PG_GETARG_DATUM(n),a,b)
#define PG_GETARG_VARCHAR_P_SLICE(n,a,b) DatumGetVarCharPSlice(PG_GETARG_DATUM(n),a,b)
/*
 * 保证返回值 INT 对齐的过时变体。
 * 对这些类型，很少操作需要对齐，主要是把
 * VARDATA 指针转为 int16[] 一类。多数代码应使用
 * ...PP(X) 对应物。不过它们仍常出现在
 * PostgreSQL 8.3 引入 ...PP(X) 之前的代码中。
 */
#define DatumGetByteaP(X)			((bytea *) PG_DETOAST_DATUM(X))
#define DatumGetTextP(X)			((text *) PG_DETOAST_DATUM(X))
#define DatumGetBpCharP(X)			((BpChar *) PG_DETOAST_DATUM(X))
#define DatumGetVarCharP(X)			((VarChar *) PG_DETOAST_DATUM(X))
#define PG_GETARG_BYTEA_P(n)		DatumGetByteaP(PG_GETARG_DATUM(n))
#define PG_GETARG_TEXT_P(n)			DatumGetTextP(PG_GETARG_DATUM(n))
#define PG_GETARG_BPCHAR_P(n)		DatumGetBpCharP(PG_GETARG_DATUM(n))
#define PG_GETARG_VARCHAR_P(n)		DatumGetVarCharP(PG_GETARG_DATUM(n))

/* 自操作类支持函数取选项用此： */
#define PG_HAS_OPCLASS_OPTIONS()	has_fn_opclass_options(fcinfo->flinfo)
#define PG_GET_OPCLASS_OPTIONS()	get_fn_opclass_options(fcinfo->flinfo)

/* 要返回 NULL 则做： */
#define PG_RETURN_NULL()  \
	do { fcinfo->isnull = true; return (Datum) 0; } while (0)

/* 少数内部函数返回 void（与 NULL 不同！） */
#define PG_RETURN_VOID()	 return (Datum) 0

/* 返回标准类型结果的宏 */

#define PG_RETURN_DATUM(x)	 return (x)
#define PG_RETURN_INT32(x)	 return Int32GetDatum(x)
#define PG_RETURN_UINT32(x)  return UInt32GetDatum(x)
#define PG_RETURN_INT16(x)	 return Int16GetDatum(x)
#define PG_RETURN_UINT16(x)  return UInt16GetDatum(x)
#define PG_RETURN_CHAR(x)	 return CharGetDatum(x)
#define PG_RETURN_BOOL(x)	 return BoolGetDatum(x)
#define PG_RETURN_OID(x)	 return ObjectIdGetDatum(x)
#define PG_RETURN_POINTER(x) return PointerGetDatum(x)
#define PG_RETURN_CSTRING(x) return CStringGetDatum(x)
#define PG_RETURN_NAME(x)	 return NameGetDatum(x)
#define PG_RETURN_TRANSACTIONID(x)	return TransactionIdGetDatum(x)
/* 这些宏隐藏数据类型是否按引用传递： */
#define PG_RETURN_FLOAT4(x)  return Float4GetDatum(x)
#define PG_RETURN_FLOAT8(x)  return Float8GetDatum(x)
#define PG_RETURN_INT64(x)	 return Int64GetDatum(x)
#define PG_RETURN_UINT64(x)  return UInt64GetDatum(x)
/* 其他按引用类型的 RETURN 宏通常形如： */
#define PG_RETURN_BYTEA_P(x)   PG_RETURN_POINTER(x)
#define PG_RETURN_TEXT_P(x)    PG_RETURN_POINTER(x)
#define PG_RETURN_BPCHAR_P(x)  PG_RETURN_POINTER(x)
#define PG_RETURN_VARCHAR_P(x) PG_RETURN_POINTER(x)
#define PG_RETURN_HEAPTUPLEHEADER(x)  return HeapTupleHeaderGetDatum(x)


/*-------------------------------------------------------------------------
 *	检测动态加载函数调用约定的支持
 *
 * 动态加载函数目前仅能用版本 1（"新
 * 风格"）调用约定。版本 0（"旧风格"）已不再
 * 支持。版本 1 即本头文件定义的约定，且
 * 必须伴随宏调用
 *
 *		PG_FUNCTION_INFO_V1(function_name);
 *
 * 注意内部函数无须此装饰，因其被假定为
 * 版本 1。
 *
 *-------------------------------------------------------------------------
 */

typedef struct
{
	int			api_version;	/* 指明调用约定版本号 */
	/* 以后可为版本号 > 1 增加更多字段。 */
} Pg_finfo_record;

/* 信息函数的期望签名 */
typedef const Pg_finfo_record *(*PGFInfoFunction) (void);

/*
 *	为给定函数名构建关联信息函数的宏。
 *
 *	为方便，亦为给定函数名提供 "extern" 声明，
 *	使 C 函数作者不必再写一遍。
 *
 *	在 Windows 上，函数与信息函数必须导出。正常
 *	构建过程经 .DEF 或 --export-all-symbols 处理。
 *	使用其他构建过程的模块作者可能须手动
 *	声明函数为 PGDLLEXPORT。此处自动为信息函数
 *	声明，因作者不必显式知晓。
 */
#define PG_FUNCTION_INFO_V1(funcname) \
extern PGDLLEXPORT Datum funcname(PG_FUNCTION_ARGS); \
extern PGDLLEXPORT const Pg_finfo_record * CppConcat(pg_finfo_,funcname)(void); \
const Pg_finfo_record * \
CppConcat(pg_finfo_,funcname) (void) \
{ \
	static const Pg_finfo_record my_finfo = { 1 }; \
	return &my_finfo; \
} \
extern int no_such_variable


/*
 * 集中声明 _PG_init/_PG_fini。历史上各共享库各自
 * 声明；现欲标 PGDLLEXPORT，用集中声明可避免
 * 每个扩展再加。扩展中既有声明若在
 * 其后包含 fmgr.h 仍可用，否则 Windows 编译会
 * 失败。
 */
extern PGDLLEXPORT void _PG_init(void);
extern PGDLLEXPORT void _PG_fini(void);


/*-------------------------------------------------------------------------
 *	核验已加载模块与后端兼容性的支持
 *
 * 要求动态加载模块包含宏调用
 *		PG_MODULE_MAGIC;
 * 以便检查明显不兼容，如为不同
 * 主版本 PostgreSQL 编译。
 *
 * 若要在不支持此功能的 PostgreSQL 版本上编译，
 * 可用 #ifdef/#endif 包住。注意在多源文件
 * 模块中，宏调用应只出现一次。
 *
 * 魔法块所含具体项拟为可定制配置、且若用其他值
 * 编译则尤其易破坏动态加载
 * 模块者。另外，长度字段
 * 可用于检测定义变更。
 *
 * 注意：我们用 memcmp() 比较魔法块，故其中不应有
 * 任何对齐填充字节。
 *
 * 注意：更改魔法块内容时，务必调整
 * dfmgr.c 中的 incompatible_module_error()。
 *-------------------------------------------------------------------------
 */

/* 魔法块结构的定义 */
typedef struct
{
	int			len;			/* sizeof(本结构) */
	int			version;		/* PostgreSQL 主版本 */
	int			funcmaxargs;	/* FUNC_MAX_ARGS */
	int			indexmaxkeys;	/* INDEX_MAX_KEYS */
	int			namedatalen;	/* NAMEDATALEN */
	int			float8byval;	/* FLOAT8PASSBYVAL */
	char		abi_extra[32];	/* 见 pg_config_manual.h */
} Pg_magic_struct;

/* 实际数据块内容 */
#define PG_MODULE_MAGIC_DATA \
{ \
	sizeof(Pg_magic_struct), \
	PG_VERSION_NUM / 100, \
	FUNC_MAX_ARGS, \
	INDEX_MAX_KEYS, \
	NAMEDATALEN, \
	FLOAT8PASSBYVAL, \
	FMGR_ABI_EXTRA, \
}

StaticAssertDecl(sizeof(FMGR_ABI_EXTRA) <= sizeof(((Pg_magic_struct *) 0)->abi_extra),
				 "FMGR_ABI_EXTRA too long");

/*
 * 声明模块魔法函数。须为函数，因后端
 * dlsym 仅保证对函数有效，对数据不保证
 */
typedef const Pg_magic_struct *(*PGModuleMagicFunction) (void);

#define PG_MAGIC_FUNCTION_NAME Pg_magic_func
#define PG_MAGIC_FUNCTION_NAME_STRING "Pg_magic_func"

#define PG_MODULE_MAGIC \
extern PGDLLEXPORT const Pg_magic_struct *PG_MAGIC_FUNCTION_NAME(void); \
const Pg_magic_struct * \
PG_MAGIC_FUNCTION_NAME(void) \
{ \
	static const Pg_magic_struct Pg_magic_data = PG_MODULE_MAGIC_DATA; \
	return &Pg_magic_data; \
} \
extern int no_such_variable


/*-------------------------------------------------------------------------
 *	调用 fmgr 兼容函数的调用方支持例程与宏
 *-------------------------------------------------------------------------
 */

/* 用于以直接计算的参数表调用具名函数。
 * 注意参数与结果皆不得为 NULL。函数亦不能是
 * 需要查看 FmgrInfo 者，因不会有
 * FmgrInfo。
 */
extern Datum DirectFunctionCall1Coll(PGFunction func, Oid collation,
									 Datum arg1);
extern Datum DirectFunctionCall2Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2);
extern Datum DirectFunctionCall3Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3);
extern Datum DirectFunctionCall4Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3, Datum arg4);
extern Datum DirectFunctionCall5Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3, Datum arg4, Datum arg5);
extern Datum DirectFunctionCall6Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3, Datum arg4, Datum arg5,
									 Datum arg6);
extern Datum DirectFunctionCall7Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3, Datum arg4, Datum arg5,
									 Datum arg6, Datum arg7);
extern Datum DirectFunctionCall8Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3, Datum arg4, Datum arg5,
									 Datum arg6, Datum arg7, Datum arg8);
extern Datum DirectFunctionCall9Coll(PGFunction func, Oid collation,
									 Datum arg1, Datum arg2,
									 Datum arg3, Datum arg4, Datum arg5,
									 Datum arg6, Datum arg7, Datum arg8,
									 Datum arg9);

/*
 * 这些函数类似 DirectFunctionCall，但用
 * flinfo 参数初始化调用的 fcinfo。
 * 建议被调方仅用 fn_extra 与 fn_mcxt
 * 字段，因其他字段通常描述调用方而非
 * 被调方。反之，调用方不应已使用
 * fn_extra，除非其用法已知与被调方兼容。
 */
extern Datum CallerFInfoFunctionCall1(PGFunction func, FmgrInfo *flinfo,
									  Oid collation, Datum arg1);
extern Datum CallerFInfoFunctionCall2(PGFunction func, FmgrInfo *flinfo,
									  Oid collation, Datum arg1, Datum arg2);

/* 用于以直接计算的参数表调用先前已查找的函数。
 * 注意参数与结果皆不得为
 * NULL。
 */
extern Datum FunctionCall0Coll(FmgrInfo *flinfo, Oid collation);
extern Datum FunctionCall1Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1);
extern Datum FunctionCall2Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2);
extern Datum FunctionCall3Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3);
extern Datum FunctionCall4Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3, Datum arg4);
extern Datum FunctionCall5Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3, Datum arg4, Datum arg5);
extern Datum FunctionCall6Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3, Datum arg4, Datum arg5,
							   Datum arg6);
extern Datum FunctionCall7Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3, Datum arg4, Datum arg5,
							   Datum arg6, Datum arg7);
extern Datum FunctionCall8Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3, Datum arg4, Datum arg5,
							   Datum arg6, Datum arg7, Datum arg8);
extern Datum FunctionCall9Coll(FmgrInfo *flinfo, Oid collation,
							   Datum arg1, Datum arg2,
							   Datum arg3, Datum arg4, Datum arg5,
							   Datum arg6, Datum arg7, Datum arg8,
							   Datum arg9);

/* 用于以直接计算的参数表按 OID 标识调用函数。
 * 注意参数与结果皆不得为 NULL。实质是
 * fmgr_info() 后接 FunctionCallN()。若同函数将反复调用，则
 * 做一次 fmgr_info() 再使用
 * FunctionCallN()。
 */
extern Datum OidFunctionCall0Coll(Oid functionId, Oid collation);
extern Datum OidFunctionCall1Coll(Oid functionId, Oid collation,
								  Datum arg1);
extern Datum OidFunctionCall2Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2);
extern Datum OidFunctionCall3Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3);
extern Datum OidFunctionCall4Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3, Datum arg4);
extern Datum OidFunctionCall5Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3, Datum arg4, Datum arg5);
extern Datum OidFunctionCall6Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3, Datum arg4, Datum arg5,
								  Datum arg6);
extern Datum OidFunctionCall7Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3, Datum arg4, Datum arg5,
								  Datum arg6, Datum arg7);
extern Datum OidFunctionCall8Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3, Datum arg4, Datum arg5,
								  Datum arg6, Datum arg7, Datum arg8);
extern Datum OidFunctionCall9Coll(Oid functionId, Oid collation,
								  Datum arg1, Datum arg2,
								  Datum arg3, Datum arg4, Datum arg5,
								  Datum arg6, Datum arg7, Datum arg8,
								  Datum arg9);

/* 这些宏允许省略排序规则参数（默认为
 * InvalidOid，即无排序规则）。主要为源码
 * 向后兼容而存在。
 */
#define DirectFunctionCall1(func, arg1) \
	DirectFunctionCall1Coll(func, InvalidOid, arg1)
#define DirectFunctionCall2(func, arg1, arg2) \
	DirectFunctionCall2Coll(func, InvalidOid, arg1, arg2)
#define DirectFunctionCall3(func, arg1, arg2, arg3) \
	DirectFunctionCall3Coll(func, InvalidOid, arg1, arg2, arg3)
#define DirectFunctionCall4(func, arg1, arg2, arg3, arg4) \
	DirectFunctionCall4Coll(func, InvalidOid, arg1, arg2, arg3, arg4)
#define DirectFunctionCall5(func, arg1, arg2, arg3, arg4, arg5) \
	DirectFunctionCall5Coll(func, InvalidOid, arg1, arg2, arg3, arg4, arg5)
#define DirectFunctionCall6(func, arg1, arg2, arg3, arg4, arg5, arg6) \
	DirectFunctionCall6Coll(func, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6)
#define DirectFunctionCall7(func, arg1, arg2, arg3, arg4, arg5, arg6, arg7) \
	DirectFunctionCall7Coll(func, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7)
#define DirectFunctionCall8(func, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8) \
	DirectFunctionCall8Coll(func, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)
#define DirectFunctionCall9(func, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9) \
	DirectFunctionCall9Coll(func, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9)
#define FunctionCall1(flinfo, arg1) \
	FunctionCall1Coll(flinfo, InvalidOid, arg1)
#define FunctionCall2(flinfo, arg1, arg2) \
	FunctionCall2Coll(flinfo, InvalidOid, arg1, arg2)
#define FunctionCall3(flinfo, arg1, arg2, arg3) \
	FunctionCall3Coll(flinfo, InvalidOid, arg1, arg2, arg3)
#define FunctionCall4(flinfo, arg1, arg2, arg3, arg4) \
	FunctionCall4Coll(flinfo, InvalidOid, arg1, arg2, arg3, arg4)
#define FunctionCall5(flinfo, arg1, arg2, arg3, arg4, arg5) \
	FunctionCall5Coll(flinfo, InvalidOid, arg1, arg2, arg3, arg4, arg5)
#define FunctionCall6(flinfo, arg1, arg2, arg3, arg4, arg5, arg6) \
	FunctionCall6Coll(flinfo, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6)
#define FunctionCall7(flinfo, arg1, arg2, arg3, arg4, arg5, arg6, arg7) \
	FunctionCall7Coll(flinfo, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7)
#define FunctionCall8(flinfo, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8) \
	FunctionCall8Coll(flinfo, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)
#define FunctionCall9(flinfo, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9) \
	FunctionCall9Coll(flinfo, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9)
#define OidFunctionCall0(functionId) \
	OidFunctionCall0Coll(functionId, InvalidOid)
#define OidFunctionCall1(functionId, arg1) \
	OidFunctionCall1Coll(functionId, InvalidOid, arg1)
#define OidFunctionCall2(functionId, arg1, arg2) \
	OidFunctionCall2Coll(functionId, InvalidOid, arg1, arg2)
#define OidFunctionCall3(functionId, arg1, arg2, arg3) \
	OidFunctionCall3Coll(functionId, InvalidOid, arg1, arg2, arg3)
#define OidFunctionCall4(functionId, arg1, arg2, arg3, arg4) \
	OidFunctionCall4Coll(functionId, InvalidOid, arg1, arg2, arg3, arg4)
#define OidFunctionCall5(functionId, arg1, arg2, arg3, arg4, arg5) \
	OidFunctionCall5Coll(functionId, InvalidOid, arg1, arg2, arg3, arg4, arg5)
#define OidFunctionCall6(functionId, arg1, arg2, arg3, arg4, arg5, arg6) \
	OidFunctionCall6Coll(functionId, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6)
#define OidFunctionCall7(functionId, arg1, arg2, arg3, arg4, arg5, arg6, arg7) \
	OidFunctionCall7Coll(functionId, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7)
#define OidFunctionCall8(functionId, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8) \
	OidFunctionCall8Coll(functionId, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8)
#define OidFunctionCall9(functionId, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9) \
	OidFunctionCall9Coll(functionId, InvalidOid, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9)


/* 便于调用数据类型 I/O 函数的特例。 */
extern Datum InputFunctionCall(FmgrInfo *flinfo, char *str,
							   Oid typioparam, int32 typmod);
extern bool InputFunctionCallSafe(FmgrInfo *flinfo, char *str,
								  Oid typioparam, int32 typmod,
								  fmNodePtr escontext,
								  Datum *result);
extern bool DirectInputFunctionCallSafe(PGFunction func, char *str,
										Oid typioparam, int32 typmod,
										fmNodePtr escontext,
										Datum *result);
extern Datum OidInputFunctionCall(Oid functionId, char *str,
								  Oid typioparam, int32 typmod);
extern char *OutputFunctionCall(FmgrInfo *flinfo, Datum val);
extern char *OidOutputFunctionCall(Oid functionId, Datum val);
extern Datum ReceiveFunctionCall(FmgrInfo *flinfo, fmStringInfo buf,
								 Oid typioparam, int32 typmod);
extern Datum OidReceiveFunctionCall(Oid functionId, fmStringInfo buf,
									Oid typioparam, int32 typmod);
extern bytea *SendFunctionCall(FmgrInfo *flinfo, Datum val);
extern bytea *OidSendFunctionCall(Oid functionId, Datum val);


/*
 * fmgr.c 例程
 */
extern const Pg_finfo_record *fetch_finfo_record(void *filehandle, const char *funcname);
extern Oid	fmgr_internal_function(const char *proname);
extern Oid	get_fn_expr_rettype(FmgrInfo *flinfo);
extern Oid	get_fn_expr_argtype(FmgrInfo *flinfo, int argnum);
extern Oid	get_call_expr_argtype(fmNodePtr expr, int argnum);
extern bool get_fn_expr_arg_stable(FmgrInfo *flinfo, int argnum);
extern bool get_call_expr_arg_stable(fmNodePtr expr, int argnum);
extern bool get_fn_expr_variadic(FmgrInfo *flinfo);
extern bytea *get_fn_opclass_options(FmgrInfo *flinfo);
extern bool has_fn_opclass_options(FmgrInfo *flinfo);
extern void set_fn_opclass_options(FmgrInfo *flinfo, bytea *options);
extern bool CheckFunctionValidatorAccess(Oid validatorOid, Oid functionOid);

/*
 * dfmgr.c 例程
 */
extern PGDLLIMPORT char *Dynamic_library_path;

extern void *load_external_function(const char *filename, const char *funcname,
									bool signalNotFound, void **filehandle);
extern void *lookup_external_function(void *filehandle, const char *funcname);
extern void load_file(const char *filename, bool restricted);
extern void **find_rendezvous_variable(const char *varName);
extern Size EstimateLibraryStateSpace(void);
extern void SerializeLibraryState(Size maxsize, char *start_address);
extern void RestoreLibraryState(char *start_address);

/*
 * 聚合函数支持
 *
 * 实际在 executor/nodeAgg.c，但声明于此，因
 * 要点是让调用方不必与 nodeAgg 过分亲近。
 */

/* AggCheckCallContext 可返回下列代码之一，或 0： */
#define AGG_CONTEXT_AGGREGATE	1	/* 普通聚合 */
#define AGG_CONTEXT_WINDOW		2	/* 窗口函数 */

extern int	AggCheckCallContext(FunctionCallInfo fcinfo,
								MemoryContext *aggcontext);
extern fmAggrefPtr AggGetAggref(FunctionCallInfo fcinfo);
extern MemoryContext AggGetTempMemoryContext(FunctionCallInfo fcinfo);
extern bool AggStateIsShared(FunctionCallInfo fcinfo);
extern void AggRegisterCallback(FunctionCallInfo fcinfo,
								fmExprContextCallbackFunction func,
								Datum arg);

/*
 * 允许插件模块钩住函数进入/退出。意在
 * 支持可加载安全策略模块，它们可能想在
 * 函数进入或退出时做额外特权检查，或做
 * 其他内部簿记。为此，此类模块不仅须能
 * 支持正常进入与退出，还须能捕获
 * 因错误退出的情形；且须能
 * 阻止内联。
 */
typedef enum FmgrHookEventType
{
	FHET_START,
	FHET_END,
	FHET_ABORT
} FmgrHookEventType;

typedef bool (*needs_fmgr_hook_type) (Oid fn_oid);

typedef void (*fmgr_hook_type) (FmgrHookEventType event,
								FmgrInfo *flinfo, Datum *arg);

extern PGDLLIMPORT needs_fmgr_hook_type needs_fmgr_hook;
extern PGDLLIMPORT fmgr_hook_type fmgr_hook;

#define FmgrHookIsNeeded(fn_oid)							\
	(!needs_fmgr_hook ? false : (*needs_fmgr_hook)(fn_oid))

#endif							/* FMGR_H */
