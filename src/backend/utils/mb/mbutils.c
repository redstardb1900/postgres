/*-------------------------------------------------------------------------
 *
 * mbutils.c
 *	  本文件含编码转换相关函数。
 *
 * 本文件中的字符串转换函数有若干 API 怪癖。
 * 注意如下：
 *
 * 若需转换，函数返回 palloc 的、以空字节结尾的
 * 字符串。但若不做转换，则原样返回给定源
 * 串指针。
 *
 * 虽有长度参数，调用方可传入非空终止串，
 * 但仍须当心：若不发生转换，将回传同一
 * 字符串。此类调用方*必须*检查
 * result == src 并区别处理。
 *
 * 若源与目标编码相同，则原样返回源串，
 * 不做任何校验；假定其为合法数据。
 * 若可能并非如此，调用方须另行调用
 * pg_verify_mbstr() 校验该串。只要源与
 * 目标编码不同，函数即保证结果按目标编码
 * 合法编码。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/mbutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "varatt.h"

/*
 * 我们维护一简单链表，缓存当前所选转换函数
 * 的 fmgr 查找信息，以及本会话中曾选过的
 * 任何转换函数。（记住先前设置，是因事务
 * 回滚时必须能恢复先前设置，且不得再做
 * 新的目录访问。）
 *
 * 此数据永不释放，故径置于 TopMemoryContext。
 */
typedef struct ConvProcInfo
{
	int			s_encoding;		/* 服务器与客户端编码 ID */
	int			c_encoding;
	FmgrInfo	to_server_info; /* 转换过程的查找信息 */
	FmgrInfo	to_client_info;
} ConvProcInfo;

static List *ConvProcList = NIL;	/* ConvProcInfo 列表 */

/*
 * 这些变量指向当前活动的转换函数，
 * 无需转换时为 NULL。
 */
static FmgrInfo *ToServerConvProc = NULL;
static FmgrInfo *ToClientConvProc = NULL;

/*
 * 此变量存放把 UTF-8 转到服务器编码的
 * 转换函数。若服务器编码*就是* UTF-8，或
 * 缺少该转换函数，则为 NULL。
 */
static FmgrInfo *Utf8ToServerConvProc = NULL;

/*
 * 这些变量跟踪当前所选编码。
 */
static const pg_enc2name *ClientEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static const pg_enc2name *DatabaseEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];
static const pg_enc2name *MessageEncoding = &pg_enc2name_tbl[PG_SQL_ASCII];

/*
 * 后端启动期间无法设客户端编码，因我们 (a)
 * 无法查找转换函数，且 (b) 可能尚不知数据库
 * 编码。故 SetClientEncoding() 先接受任意值并
 * 记住，供 InitializeClientEncoding() 稍后应用。
 */
static bool backend_startup_complete = false;
static int	pending_client_encoding = PG_SQL_ASCII;


/* 内部函数 */
static char *perform_default_encoding_conversion(const char *src,
												 int len, bool is_client_to_server);
static int	cliplen(const char *str, int len, int limit);


/*
 * 为日后调用 SetClientEncoding 做准备。成功应意味
 * 对此编码请求，SetClientEncoding 保证会成功。
 *
 * 但注意：backend_startup_complete 前的成功并不保证
 * 此后仍成功)
 *
 * 成功返回 0，否则 -1（坏编码或无法支持转换）
 */
int
PrepareClientEncoding(int encoding)
{
	int			current_server_encoding;
	ListCell   *lc;

	if (!PG_VALID_FE_ENCODING(encoding))
		return -1;

	/* 启动期间不能做任何事，见上注 */
	if (!backend_startup_complete)
		return 0;

	current_server_encoding = GetDatabaseEncoding();

	/*
	 * 检查无需转换函数的情形。
	 */
	if (current_server_encoding == encoding ||
		current_server_encoding == PG_SQL_ASCII ||
		encoding == PG_SQL_ASCII)
		return 0;

	if (IsTransactionState())
	{
		/*
		 * 若处于活动事务中，访问目录是安全的，
		 * 故查找函数。即便信息已缓存也重复查找，
		 * 以便能响应
		 * pg_conversion.
		 */
		Oid			to_server_proc,
					to_client_proc;
		ConvProcInfo *convinfo;
		MemoryContext oldcontext;

		to_server_proc = FindDefaultConversionProc(encoding,
												   current_server_encoding);
		if (!OidIsValid(to_server_proc))
			return -1;
		to_client_proc = FindDefaultConversionProc(current_server_encoding,
												   encoding);
		if (!OidIsValid(to_client_proc))
			return -1;

		/*
		 * 把 fmgr 信息载入 TopMemoryContext(此处仍可能失败)
		 */
		convinfo = (ConvProcInfo *) MemoryContextAlloc(TopMemoryContext,
													   sizeof(ConvProcInfo));
		convinfo->s_encoding = current_server_encoding;
		convinfo->c_encoding = encoding;
		fmgr_info_cxt(to_server_proc, &convinfo->to_server_info,
					  TopMemoryContext);
		fmgr_info_cxt(to_client_proc, &convinfo->to_client_info,
					  TopMemoryContext);

		/* 把新信息接到链表头 */
		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		ConvProcList = lcons(convinfo, ConvProcList);
		MemoryContextSwitchTo(oldcontext);

		/*
		 * 尚无法删除同一编码对的较旧条目，因其
		 * 可能仍在使用。SetClientEncoding 会清理。
		 */

		return 0;				/* 成功 */
	}
	else
	{
		/*
		 * 若不在活动事务中，唯一能做的是用缓存
		 * 恢复先前设置。这覆盖全部事务回滚情形。
		 * 唯一可能不适用的情况是试图通过编辑
		 * postgresql.conf 并 SIGHUP 来即时改
		 * client_encoding。那多半本就是蠢
		 * 事。
		 */
		foreach(lc, ConvProcList)
		{
			ConvProcInfo *oldinfo = (ConvProcInfo *) lfirst(lc);

			if (oldinfo->s_encoding == current_server_encoding &&
				oldinfo->c_encoding == encoding)
				return 0;
		}

		return -1;				/* 未缓存，故失败 */
	}
}

/*
 * 设定活动客户端编码并建立转换函数指针。
 * 此前应对此编码调用过 PrepareClientEncoding。
 *
 * 成功返回 0，否则 -1（坏编码或无法支持转换）
 */
int
SetClientEncoding(int encoding)
{
	int			current_server_encoding;
	bool		found;
	ListCell   *lc;

	if (!PG_VALID_FE_ENCODING(encoding))
		return -1;

	/* 启动期间不能做任何事，见上注 */
	if (!backend_startup_complete)
	{
		pending_client_encoding = encoding;
		return 0;
	}

	current_server_encoding = GetDatabaseEncoding();

	/*
	 * 检查无需转换函数的情形。
	 */
	if (current_server_encoding == encoding ||
		current_server_encoding == PG_SQL_ASCII ||
		encoding == PG_SQL_ASCII)
	{
		ClientEncoding = &pg_enc2name_tbl[encoding];
		ToServerConvProc = NULL;
		ToClientConvProc = NULL;
		return 0;
	}

	/*
	 * 在缓存中查找先前由 PrepareClientEncoding
	 * 准备的条目；若无则失败。同时释放重复条目，
	 * 以免反复 Prepare/Set 循环造成
	 * 内存泄漏
	 */
	found = false;
	foreach(lc, ConvProcList)
	{
		ConvProcInfo *convinfo = (ConvProcInfo *) lfirst(lc);

		if (convinfo->s_encoding == current_server_encoding &&
			convinfo->c_encoding == encoding)
		{
			if (!found)
			{
				/* 找到最新条目并建立 */
				ClientEncoding = &pg_enc2name_tbl[encoding];
				ToServerConvProc = &convinfo->to_server_info;
				ToClientConvProc = &convinfo->to_client_info;
				found = true;
			}
			else
			{
				/* 重复条目，释放之 */
				ConvProcList = foreach_delete_current(ConvProcList, lc);
				pfree(convinfo);
			}
		}
	}

	if (found)
		return 0;				/* 成功 */
	else
		return -1;				/* 未缓存，故失败 */
}

/*
 * 初始化客户端编码转换。
 *	由 InitPostgres() 在后端启动时调用一次。
 */
void
InitializeClientEncoding(void)
{
	int			current_server_encoding;

	Assert(!backend_startup_complete);
	backend_startup_complete = true;

	if (PrepareClientEncoding(pending_client_encoding) < 0 ||
		SetClientEncoding(pending_client_encoding) < 0)
	{
		/*
		 * 糟糕，请求的转换不可用。此前无法失败，
		 * 现在可以了。
		 */
		ereport(FATAL,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conversion between %s and %s is not supported",
						pg_enc2name_tbl[pending_client_encoding].name,
						GetDatabaseEncodingName())));
	}

	/*
	 * 若需要也查找 UTF8→服务器转换函数。因任一
	 * 后端进程内服务器编码固定，我们
	 * 不必做超过一次。
	 */
	current_server_encoding = GetDatabaseEncoding();
	if (current_server_encoding != PG_UTF8 &&
		current_server_encoding != PG_SQL_ASCII)
	{
		Oid			utf8_to_server_proc;

		Assert(IsTransactionState());
		utf8_to_server_proc =
			FindDefaultConversionProc(PG_UTF8,
									  current_server_encoding);
		/* 若无此转换，指针保持为 NULL */
		if (OidIsValid(utf8_to_server_proc))
		{
			FmgrInfo   *finfo;

			finfo = (FmgrInfo *) MemoryContextAlloc(TopMemoryContext,
													sizeof(FmgrInfo));
			fmgr_info_cxt(utf8_to_server_proc, finfo,
						  TopMemoryContext);
			/* 数据完全有效后才设 Utf8ToServerConvProc */
			Utf8ToServerConvProc = finfo;
		}
	}
}

/*
 * 返回当前客户端编码
 */
int
pg_get_client_encoding(void)
{
	return ClientEncoding->encoding;
}

/*
 * 返回当前客户端编码名
 */
const char *
pg_get_client_encoding_name(void)
{
	return ClientEncoding->name;
}

/*
 * 把 src 串转到另一编码（一般情形）。
 *
 * 见本文件顶部关于字符串转换函数的说明。
 */
unsigned char *
pg_do_encoding_conversion(unsigned char *src, int len,
						  int src_encoding, int dest_encoding)
{
	unsigned char *result;
	Oid			proc;

	if (len <= 0)
		return src;				/* 空串总是合法的 */

	if (src_encoding == dest_encoding)
		return src;				/* 无需转换，假定合法 */

	if (dest_encoding == PG_SQL_ASCII)
		return src;				/* SQL_ASCII 下任意串皆合法 */

	if (src_encoding == PG_SQL_ASCII)
	{
		/* 无法转换，但必须校验结果 */
		(void) pg_verify_mbstr(dest_encoding, (const char *) src, len, false);
		return src;
	}

	if (!IsTransactionState())	/* 不应发生 */
		elog(ERROR, "cannot perform encoding conversion outside a transaction");

	proc = FindDefaultConversionProc(src_encoding, dest_encoding);
	if (!OidIsValid(proc))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("default conversion function for encoding \"%s\" to \"%s\" does not exist",
						pg_encoding_to_char(src_encoding),
						pg_encoding_to_char(dest_encoding))));

	/*
	 * 为转换结果分配空间，警惕整数溢出。
	 *
	 * len * MAX_CONVERSION_GROWTH 通常远超实际所需，
	 * 故即便结果实际装得下，也可能超过 MaxAllocSize。
	 * 我们不想交回超过 MaxAllocSize 的结果串，
	 * 因调用方可能应付不好——但若只是多分配
	 * 而未用完，则无妨。
	 */
	if ((Size) len >= (MaxAllocHugeSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
				 errdetail("String of %d bytes is too long for encoding conversion.",
						   len)));

	result = (unsigned char *)
		MemoryContextAllocHuge(CurrentMemoryContext,
							   (Size) len * MAX_CONVERSION_GROWTH + 1);

	(void) OidFunctionCall6(proc,
							Int32GetDatum(src_encoding),
							Int32GetDatum(dest_encoding),
							CStringGetDatum((char *) src),
							CStringGetDatum((char *) result),
							Int32GetDatum(len),
							BoolGetDatum(false));

	/*
	 * 若结果很大，值得用 repalloc 释放多要的空间。
	 * 此处阈值略随意，但当 len * MAX_CONVERSION_GROWTH
	 * 超过 MaxAllocSize 时*必须*检查。
	 */
	if (len > 1000000)
	{
		Size		resultlen = strlen((char *) result);

		if (resultlen >= MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory"),
					 errdetail("String of %d bytes is too long for encoding conversion.",
							   len)));

		result = (unsigned char *) repalloc(result, resultlen + 1);
	}

	return result;
}

/*
 * 把源串转到另一编码。
 *
 * 本函数接口与其他转换函数不同。
 * 调用方应已查得默认转换过程
 * 即 FindDefaultConversionProc。与其他函数
 * 不同：结果不经堆分配，写入调用方缓冲。
 *
 *
 * src_encoding   - 源编码
 * dest_encoding  - 目标编码
 * src, srclen    - 输入缓冲及其字节长度
 * dest, destlen  - 目标缓冲及其字节大小
 *
 * 输出以空字节结尾。
 *
 * 若 destlen < srclen * MAX_CONVERSION_INPUT_LENGTH + 1，转换输出
 * 未必装得进输出缓冲，函数将不会
 * 转换全部输入。
 *
 * TODO: 转换函数接口不佳。首先，若能把目标
 * 缓冲大小传给转换函数会很好，这样传入较短
 * 目标缓冲时仍可填满整个缓冲。目前我们只能按
 * 最坏膨胀假定提前停转换，即便目标缓冲实际
 * 仍有空间。其次，若能向调用方返回已写
 * 字节数会很好，以免再调用
 * strlen()。
 * to strlen().
 */
int
pg_do_encoding_conversion_buf(Oid proc,
							  int src_encoding,
							  int dest_encoding,
							  unsigned char *src, int srclen,
							  unsigned char *dest, int destlen,
							  bool noError)
{
	Datum		result;

	/*
	 * 若目标缓冲不足以按最坏情况容纳结果，则限制
	 * 传给转换函数的输入大小。
	 */
	if ((Size) srclen >= ((destlen - 1) / (Size) MAX_CONVERSION_GROWTH))
		srclen = ((destlen - 1) / (Size) MAX_CONVERSION_GROWTH);

	result = OidFunctionCall6(proc,
							  Int32GetDatum(src_encoding),
							  Int32GetDatum(dest_encoding),
							  CStringGetDatum((char *) src),
							  CStringGetDatum((char *) dest),
							  Int32GetDatum(srclen),
							  BoolGetDatum(noError));
	return DatumGetInt32(result);
}

/*
 * 把串转到 encoding_name 所指编码。源
 * 编码为 DB 编码。
 *
 * BYTEA convert_to(TEXT string, NAME encoding_name) */
Datum
pg_convert_to(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		dest_encoding_name = PG_GETARG_DATUM(1);
	Datum		src_encoding_name = DirectFunctionCall1(namein,
														CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	/*
	 * pg_convert 期望首参为 bytea。此处传入
	 * text，依赖二者同为 varlena 类型，因而
	 * 结构相同。
	 */
	result = DirectFunctionCall3(pg_convert, string,
								 src_encoding_name, dest_encoding_name);

	PG_RETURN_DATUM(result);
}

/*
 * 把串从 encoding_name 所指编码转来。目标
 * 编码为 DB 编码。
 *
 * TEXT convert_from(BYTEA string, NAME encoding_name) */
Datum
pg_convert_from(PG_FUNCTION_ARGS)
{
	Datum		string = PG_GETARG_DATUM(0);
	Datum		src_encoding_name = PG_GETARG_DATUM(1);
	Datum		dest_encoding_name = DirectFunctionCall1(namein,
														 CStringGetDatum(DatabaseEncoding->name));
	Datum		result;

	result = DirectFunctionCall3(pg_convert, string,
								 src_encoding_name, dest_encoding_name);

	/*
	 * pg_convert 返回 bytea，我们再当 text 返回，依赖
	 * 二者同为 varlena 类型，因而
	 * 结构相同。虽非所有 bytea 值都是合法 text，
	 * 但此处会是，因我们已让 pg_convert 返回在
	 * 当前数据库编码下作为 text 合法者。
	 */
	PG_RETURN_DATUM(result);
}

/*
 * 在任意两编码间转换字符串。
 *
 * BYTEA convert(BYTEA string, NAME src_encoding_name, NAME dest_encoding_name)
 */
Datum
pg_convert(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	char	   *dest_encoding_name = NameStr(*PG_GETARG_NAME(2));
	int			dest_encoding = pg_char_to_encoding(dest_encoding_name);
	const char *src_str;
	char	   *dest_str;
	bytea	   *retval;
	int			len;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid source encoding name \"%s\"",
						src_encoding_name)));
	if (dest_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid destination encoding name \"%s\"",
						dest_encoding_name)));

	/* 确保源串合法 */
	len = VARSIZE_ANY_EXHDR(string);
	src_str = VARDATA_ANY(string);
	(void) pg_verify_mbstr(src_encoding, src_str, len, false);

	/* 执行转换 */
	dest_str = (char *) pg_do_encoding_conversion((unsigned char *) unconstify(char *, src_str),
												  len,
												  src_encoding,
												  dest_encoding);

	/* 若实际发生转换则更新 len */
	if (dest_str != src_str)
		len = strlen(dest_str);

	/*
	 * 构造 bytea 数据类型结构
	 */
	retval = (bytea *) palloc(len + VARHDRSZ);
	SET_VARSIZE(retval, len + VARHDRSZ);
	memcpy(VARDATA(retval), dest_str, len);

	if (dest_str != src_str)
		pfree(dest_str);

	/* 若由 toaster 分配则释放内存 */
	PG_FREE_IF_COPY(string, 0);

	PG_RETURN_BYTEA_P(retval);
}

/*
 * 按指定编码把串视为 text 时的
 * 长度。数据在该编码下不合法则
 * 报错。
 *
 * INT4 length (BYTEA string, NAME src_encoding_name)
 */
Datum
length_in_encoding(PG_FUNCTION_ARGS)
{
	bytea	   *string = PG_GETARG_BYTEA_PP(0);
	char	   *src_encoding_name = NameStr(*PG_GETARG_NAME(1));
	int			src_encoding = pg_char_to_encoding(src_encoding_name);
	const char *src_str;
	int			len;
	int			retval;

	if (src_encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding name \"%s\"",
						src_encoding_name)));

	len = VARSIZE_ANY_EXHDR(string);
	src_str = VARDATA_ANY(string);

	retval = pg_verify_mbstr_len(src_encoding, src_str, len, false);

	PG_RETURN_INT32(retval);
}

/*
 * 取指定编码下多字节字符的最大长度。
 *
 * 注意编码以数值指定，非如上按名。
 */
Datum
pg_encoding_max_length_sql(PG_FUNCTION_ARGS)
{
	int			encoding = PG_GETARG_INT32(0);

	if (PG_VALID_ENCODING(encoding))
		PG_RETURN_INT32(pg_wchar_table[encoding].maxmblen);
	else
		PG_RETURN_NULL();
}

/*
 * 把客户端编码转到服务器编码。
 *
 * 见本文件顶部关于字符串转换函数的说明。
 */
char *
pg_client_to_server(const char *s, int len)
{
	return pg_any_to_server(s, len, ClientEncoding->encoding);
}

/*
 * 把任意编码转到服务器编码。
 *
 * 见本文件顶部关于字符串转换函数的说明。
 *
 * 与其他字符串转换函数不同，即便 encoding ==
 * DatabaseEncoding->encoding 也会做校验。因本函数
 * 处理来自库外的数据，我们从不愿只是
 * 假定其合法。
 */
char *
pg_any_to_server(const char *s, int len, int encoding)
{
	if (len <= 0)
		return unconstify(char *, s);	/* 空串总是合法的 */

	if (encoding == DatabaseEncoding->encoding ||
		encoding == PG_SQL_ASCII)
	{
		/*
		 * 无需转换，但仍必须校验数据。
		 */
		(void) pg_verify_mbstr(DatabaseEncoding->encoding, s, len, false);
		return unconstify(char *, s);
	}

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/*
		 * 无法转换，但仍必须校验数据，因客户端
		 * 代码可能已按所选 client_encoding 做了串转义。
		 * 若客户端编码是 ASCII 安全的，则直接按该编码
		 * 做校验。对 ASCII 不安全的编码则有问题：不敢把
		 * 此类数据交给解析器，又无法转换。折中办法是
		 * 若含任何非 ASCII 字符则拒绝该
		 * 数据。
		 */
		if (PG_VALID_BE_ENCODING(encoding))
			(void) pg_verify_mbstr(encoding, s, len, false);
		else
		{
			int			i;

			for (i = 0; i < len; i++)
			{
				if (s[i] == '\0' || IS_HIGHBIT_SET(s[i]))
					ereport(ERROR,
							(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
							 errmsg("invalid byte value for encoding \"%s\": 0x%02x",
									pg_enc2name_tbl[PG_SQL_ASCII].name,
									(unsigned char) s[i])));
			}
		}
		return unconstify(char *, s);
	}

	/* 可用缓存转换函数时的快速路径 */
	if (encoding == ClientEncoding->encoding)
		return perform_default_encoding_conversion(s, len, true);

	/* 一般情形 … 事务外不可用 */
	return (char *) pg_do_encoding_conversion((unsigned char *) unconstify(char *, s),
											  len,
											  encoding,
											  DatabaseEncoding->encoding);
}

/*
 * 把服务器编码转到客户端编码。
 *
 * 见本文件顶部关于字符串转换函数的说明。
 */
char *
pg_server_to_client(const char *s, int len)
{
	return pg_server_to_any(s, len, ClientEncoding->encoding);
}

/*
 * 把服务器编码转到任意编码。
 *
 * 见本文件顶部关于字符串转换函数的说明。
 */
char *
pg_server_to_any(const char *s, int len, int encoding)
{
	if (len <= 0)
		return unconstify(char *, s);	/* 空串总是合法的 */

	if (encoding == DatabaseEncoding->encoding ||
		encoding == PG_SQL_ASCII)
		return unconstify(char *, s);	/* 假定数据合法 */

	if (DatabaseEncoding->encoding == PG_SQL_ASCII)
	{
		/* 无法转换，但必须校验结果 */
		(void) pg_verify_mbstr(encoding, s, len, false);
		return unconstify(char *, s);
	}

	/* 可用缓存转换函数时的快速路径 */
	if (encoding == ClientEncoding->encoding)
		return perform_default_encoding_conversion(s, len, false);

	/* 一般情形 … 事务外不可用 */
	return (char *) pg_do_encoding_conversion((unsigned char *) unconstify(char *, s),
											  len,
											  DatabaseEncoding->encoding,
											  encoding);
}

/*
 *	用缓存的 FmgrInfo 做默认编码转换。因本函数
 *	完全不访问数据库，可在事务外安全调用。
 *	若尚未由 SetClientEncoding() 建立转换，则
 *	不执行转换。
 */
static char *
perform_default_encoding_conversion(const char *src, int len,
									bool is_client_to_server)
{
	char	   *result;
	int			src_encoding,
				dest_encoding;
	FmgrInfo   *flinfo;

	if (is_client_to_server)
	{
		src_encoding = ClientEncoding->encoding;
		dest_encoding = DatabaseEncoding->encoding;
		flinfo = ToServerConvProc;
	}
	else
	{
		src_encoding = DatabaseEncoding->encoding;
		dest_encoding = ClientEncoding->encoding;
		flinfo = ToClientConvProc;
	}

	if (flinfo == NULL)
		return unconstify(char *, src);

	/*
	 * 为转换结果分配空间，警惕整数溢出。
	 * 见 pg_do_encoding_conversion 注释。
	 */
	if ((Size) len >= (MaxAllocHugeSize / (Size) MAX_CONVERSION_GROWTH))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("out of memory"),
				 errdetail("String of %d bytes is too long for encoding conversion.",
						   len)));

	result = (char *)
		MemoryContextAllocHuge(CurrentMemoryContext,
							   (Size) len * MAX_CONVERSION_GROWTH + 1);

	FunctionCall6(flinfo,
				  Int32GetDatum(src_encoding),
				  Int32GetDatum(dest_encoding),
				  CStringGetDatum(src),
				  CStringGetDatum(result),
				  Int32GetDatum(len),
				  BoolGetDatum(false));

	/*
	 * 若可能有很多多余空间则释放——见
	 * pg_do_encoding_conversion.
	 */
	if (len > 1000000)
	{
		Size		resultlen = strlen(result);

		if (resultlen >= MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("out of memory"),
					 errdetail("String of %d bytes is too long for encoding conversion.",
							   len)));

		result = (char *) repalloc(result, resultlen + 1);
	}

	return result;
}

/*
 * 把单个 Unicode 码点转成服务器编码下的字符串。
 *
 * 码点 c 被转换并存于 *s，该处必须至少有
 * MAX_UNICODE_EQUIVALENT_STRING+1 字节可用。
 * 输出带尾随 '\0'。若无法转换则
 * 报错。
 *
 * 注意这依赖先前已查找过所需转换函数。
 * 一则为速度，更因解析器可能在事务外或已中止
 * 的事务中调用本函数。
 */
void
pg_unicode_to_server(pg_wchar c, unsigned char *s)
{
	unsigned char c_as_utf8[MAX_MULTIBYTE_CHAR_LEN + 1];
	int			c_as_utf8_len;
	int			server_encoding;

	/*
	 * 若 Unicode 码点无效则抱怨。此处 errcode 的选择
	 * 可商榷，但调用方本应已检查过。
	 */
	if (!is_valid_unicode_codepoint(c))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("invalid Unicode code point")));

	/* 否则，若在 ASCII 范围，转换平凡 */
	if (c <= 0x7F)
	{
		s[0] = (unsigned char) c;
		s[1] = '\0';
		return;
	}

	/* 若服务器编码是 UTF-8，只需重排该码点 */
	server_encoding = GetDatabaseEncoding();
	if (server_encoding == PG_UTF8)
	{
		unicode_to_utf8(c, s);
		s[pg_utf_mblen(s)] = '\0';
		return;
	}

	/* 其余情形必须有可用的转换函数 */
	if (Utf8ToServerConvProc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("conversion between %s and %s is not supported",
						pg_enc2name_tbl[PG_UTF8].name,
						GetDatabaseEncodingName())));

	/* 构造 UTF-8 源串 */
	unicode_to_utf8(c, c_as_utf8);
	c_as_utf8_len = pg_utf_mblen(c_as_utf8);
	c_as_utf8[c_as_utf8_len] = '\0';

	/* 转换，不能则报错 */
	FunctionCall6(Utf8ToServerConvProc,
				  Int32GetDatum(PG_UTF8),
				  Int32GetDatum(server_encoding),
				  CStringGetDatum((char *) c_as_utf8),
				  CStringGetDatum((char *) s),
				  Int32GetDatum(c_as_utf8_len),
				  BoolGetDatum(false));
}

/*
 * 把单个 Unicode 码点转成服务器编码下的字符串。
 *
 * 与 pg_unicode_to_server() 相同，但不报错，
 * 转换失败时只返回 false。
 */
bool
pg_unicode_to_server_noerror(pg_wchar c, unsigned char *s)
{
	unsigned char c_as_utf8[MAX_MULTIBYTE_CHAR_LEN + 1];
	int			c_as_utf8_len;
	int			converted_len;
	int			server_encoding;

	/* 无效 Unicode 码点则失败 */
	if (!is_valid_unicode_codepoint(c))
		return false;

	/* 否则，若在 ASCII 范围，转换平凡 */
	if (c <= 0x7F)
	{
		s[0] = (unsigned char) c;
		s[1] = '\0';
		return true;
	}

	/* 若服务器编码是 UTF-8，只需重排该码点 */
	server_encoding = GetDatabaseEncoding();
	if (server_encoding == PG_UTF8)
	{
		unicode_to_utf8(c, s);
		s[pg_utf_mblen(s)] = '\0';
		return true;
	}

	/* 其余情形必须有可用的转换函数 */
	if (Utf8ToServerConvProc == NULL)
		return false;

	/* 构造 UTF-8 源串 */
	unicode_to_utf8(c, c_as_utf8);
	c_as_utf8_len = pg_utf_mblen(c_as_utf8);
	c_as_utf8[c_as_utf8_len] = '\0';

	/* 转换，但不能则不报错 */
	converted_len = DatumGetInt32(FunctionCall6(Utf8ToServerConvProc,
												Int32GetDatum(PG_UTF8),
												Int32GetDatum(server_encoding),
												CStringGetDatum((char *) c_as_utf8),
												CStringGetDatum((char *) s),
												Int32GetDatum(c_as_utf8_len),
												BoolGetDatum(true)));

	/* 仅当耗尽全部输入时转换才算成功 */
	return (converted_len == c_as_utf8_len);
}


/* 把多字节串转为 wchar */
int
pg_mb2wchar(const char *from, pg_wchar *to)
{
	return pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len((const unsigned char *) from, to, strlen(from));
}

/* 把多字节串转为 wchar，有长度限制 */
int
pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len)
{
	return pg_wchar_table[DatabaseEncoding->encoding].mb2wchar_with_len((const unsigned char *) from, to, len);
}

/* 同上，任意编码 */
int
pg_encoding_mb2wchar_with_len(int encoding,
							  const char *from, pg_wchar *to, int len)
{
	return pg_wchar_table[encoding].mb2wchar_with_len((const unsigned char *) from, to, len);
}

/* 把 wchar 串转为多字节 */
int
pg_wchar2mb(const pg_wchar *from, char *to)
{
	return pg_wchar_table[DatabaseEncoding->encoding].wchar2mb_with_len(from, (unsigned char *) to, pg_wchar_strlen(from));
}

/* 把 wchar 串转为多字节，有长度限制 */
int
pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len)
{
	return pg_wchar_table[DatabaseEncoding->encoding].wchar2mb_with_len(from, (unsigned char *) to, len);
}

/* 同上，任意编码 */
int
pg_encoding_wchar2mb_with_len(int encoding,
							  const pg_wchar *from, char *to, int len)
{
	return pg_wchar_table[encoding].wchar2mb_with_len(from, (unsigned char *) to, len);
}

/* 返回多字节字符的字节长度 */
int
pg_mblen(const char *mbstr)
{
	return pg_wchar_table[DatabaseEncoding->encoding].mblen((const unsigned char *) mbstr);
}

/* 返回多字节字符的显示长度 */
int
pg_dsplen(const char *mbstr)
{
	return pg_wchar_table[DatabaseEncoding->encoding].dsplen((const unsigned char *) mbstr);
}

/* 返回多字节串的长度（按 wchar 计） */
int
pg_mbstrlen(const char *mbstr)
{
	int			len = 0;

	/* 单字节编码的优化 */
	if (pg_database_encoding_max_length() == 1)
		return strlen(mbstr);

	while (*mbstr)
	{
		mbstr += pg_mblen(mbstr);
		len++;
	}
	return len;
}

/* 返回多字节串的长度（按 wchar 计）
 * 不一定以 NULL 结尾
 */
int
pg_mbstrlen_with_len(const char *mbstr, int limit)
{
	int			len = 0;

	/* 单字节编码的优化 */
	if (pg_database_encoding_max_length() == 1)
		return limit;

	while (limit > 0 && *mbstr)
	{
		int			l = pg_mblen(mbstr);

		limit -= l;
		mbstr += l;
		len++;
	}
	return len;
}

/*
 * 返回多字节串的字节长度
 * 不一定以 NULL 结尾
 * 且不超过 limit。
 * 本函数不打破多字节字符边界。
 */
int
pg_mbcliplen(const char *mbstr, int len, int limit)
{
	return pg_encoding_mbcliplen(DatabaseEncoding->encoding, mbstr,
								 len, limit);
}

/*
 * 带指定编码的 pg_mbcliplen；串在该编码下须合法
 */
int
pg_encoding_mbcliplen(int encoding, const char *mbstr,
					  int len, int limit)
{
	mblen_converter mblen_fn;
	int			clen = 0;
	int			l;

	/* 单字节编码的优化 */
	if (pg_encoding_max_length(encoding) == 1)
		return cliplen(mbstr, len, limit);

	mblen_fn = pg_wchar_table[encoding].mblen;

	while (len > 0 && *mbstr)
	{
		l = (*mblen_fn) ((const unsigned char *) mbstr);
		if ((clen + l) > limit)
			break;
		clen += l;
		if (clen == limit)
			break;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/*
 * 类似 pg_mbcliplen，但 limit 参数指定的是
 * 字符长度，而非字节长度。
 */
int
pg_mbcharcliplen(const char *mbstr, int len, int limit)
{
	int			clen = 0;
	int			nch = 0;
	int			l;

	/* 单字节编码的优化 */
	if (pg_database_encoding_max_length() == 1)
		return cliplen(mbstr, len, limit);

	while (len > 0 && *mbstr)
	{
		l = pg_mblen(mbstr);
		nch++;
		if (nch > limit)
			break;
		clen += l;
		len -= l;
		mbstr += l;
	}
	return clen;
}

/* 任意单字节编码的 mbcliplen */
static int
cliplen(const char *str, int len, int limit)
{
	int			l = 0;

	len = Min(len, limit);
	while (l < len && str[l])
		l++;
	return l;
}

void
SetDatabaseEncoding(int encoding)
{
	if (!PG_VALID_BE_ENCODING(encoding))
		elog(ERROR, "invalid database encoding: %d", encoding);

	DatabaseEncoding = &pg_enc2name_tbl[encoding];
	Assert(DatabaseEncoding->encoding == encoding);
}

void
SetMessageEncoding(int encoding)
{
	/* 尚不能 elog() 时也有调用 */
	Assert(PG_VALID_ENCODING(encoding));

	MessageEncoding = &pg_enc2name_tbl[encoding];
	Assert(MessageEncoding->encoding == encoding);
}

#ifdef ENABLE_NLS
/*
 * 做一次 bind_textdomain_codeset() 调用，把 pg_enc 译为
 * gettext codeset。对 gettext 未知的 MULE_INTERNAL 失败；也可
 * 因内存耗尽等 gettext 内部原因失败。
 */
static bool
raw_pg_bind_textdomain_codeset(const char *domainname, int encoding)
{
	bool		elog_ok = (CurrentMemoryContext != NULL);
	int			i;

	for (i = 0; pg_enc2gettext_tbl[i].name != NULL; i++)
	{
		if (pg_enc2gettext_tbl[i].encoding == encoding)
		{
			if (bind_textdomain_codeset(domainname,
										pg_enc2gettext_tbl[i].name) != NULL)
				return true;

			if (elog_ok)
				elog(LOG, "bind_textdomain_codeset failed");
			else
				write_stderr("bind_textdomain_codeset failed");

			break;
		}
	}

	return false;
}

/*
 * 把 gettext 消息域绑定到与数据库编码对应的
 * codeset。对 SQL_ASCII，改绑到 LC_CTYPE 所隐含的 codeset。
 * 返回新设置所隐含的 MessageEncoding。
 *
 * 多数平台上，gettext 默认用 LC_CTYPE 隐含的 codeset。
 * 若与数据库编码一致，则无需做事。在 CREATE
 * DATABASE 中，我们强制或信任区域设置 codeset 与数据库
 * 编码一致，C 区域设置除外。（Windows 上 UTF8 编码下也
 * 允许不一致。）对 C 区域设置，显式把 gettext 绑到
 * 正确的 codeset。
 *
 * 在 Windows 上，gettext 默认用 Windows ANSI 代码页。这对
 * 把串交给 Windows ANSI APIs 的软件很方便，但我们
 * 不做那样。迫使 gettext 用数据库编码，或否则用
 * LC_CTYPE 编码，一如其他平台。
 *
 * 本函数在 elog() 与 palloc() 可用之前调用。
 */
int
pg_bind_textdomain_codeset(const char *domainname)
{
	bool		elog_ok = (CurrentMemoryContext != NULL);
	int			encoding = GetDatabaseEncoding();
	int			new_msgenc;

#ifndef WIN32
	const char *ctype = setlocale(LC_CTYPE, NULL);

	if (pg_strcasecmp(ctype, "C") == 0 || pg_strcasecmp(ctype, "POSIX") == 0)
#endif
		if (encoding != PG_SQL_ASCII &&
			raw_pg_bind_textdomain_codeset(domainname, encoding))
			return encoding;

	new_msgenc = pg_get_encoding_from_locale(NULL, elog_ok);
	if (new_msgenc < 0)
		new_msgenc = PG_SQL_ASCII;

#ifdef WIN32
	if (!raw_pg_bind_textdomain_codeset(domainname, new_msgenc))
		/* 失败时，旧消息编码仍有效。 */
		return GetMessageEncoding();
#endif

	return new_msgenc;
}
#endif

/*
 * 数据库编码也称服务器编码，表示类文本
 * 数据类型中所存数据的编码。受影响类型有
 * 含 cstring 等文本类类型。
 */
int
GetDatabaseEncoding(void)
{
	return DatabaseEncoding->encoding;
}

const char *
GetDatabaseEncodingName(void)
{
	return DatabaseEncoding->name;
}

Datum
getdatabaseencoding(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(namein, CStringGetDatum(DatabaseEncoding->name));
}

Datum
pg_client_encoding(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall1(namein, CStringGetDatum(ClientEncoding->name));
}

Datum
PG_char_to_encoding(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);

	PG_RETURN_INT32(pg_char_to_encoding(NameStr(*s)));
}

Datum
PG_encoding_to_char(PG_FUNCTION_ARGS)
{
	int32		encoding = PG_GETARG_INT32(0);
	const char *encoding_name = pg_encoding_to_char(encoding);

	return DirectFunctionCall1(namein, CStringGetDatum(encoding_name));
}

/*
 * gettext() 以此编码返回消息。这常与数据库编码
 * 一致，但对 SQL_ASCII 库、未附着数据库的进程，以及
 * 缺乏 iconv 支持的数据库编码则不同
 * 支持(MULE_INTERNAL)。
 */
int
GetMessageEncoding(void)
{
	return MessageEncoding->encoding;
}


/*
 * 通用字符递增函数。
 *
 * 因不知所用编码的性质，我们只递增最后一字节，
 * 直到得到合法编码的结果，或用尽可试值。
 * 不尝试递增高位字节，故更宽字符不会增加
 * 运行时。（若尝试，还需考虑 255 在该
 * 编码中未必是合法
 * 末字节的可能。）
 */
static bool
pg_generic_charinc(unsigned char *charptr, int len)
{
	unsigned char *lastbyte = charptr + len - 1;
	mbchar_verifier mbverify;

	/* 可直接调用字符校验器。 */
	mbverify = pg_wchar_table[GetDatabaseEncoding()].mbverifychar;

	while (*lastbyte < (unsigned char) 255)
	{
		(*lastbyte)++;
		if ((*mbverify) (charptr, len) == len)
			return true;
	}

	return false;
}

/*
 * UTF-8 字符递增函数。
 *
 * 对小于 0x7F 的单字节字符，只递增该字节。
 *
 * 对多字节字符，除首字节外每字节须在 0x80 与
 * 0xBF 之间；首字节须在 0xC0 与 0xF4 之间。我们递增
 * 尚未达其最大值的最后一字节。若找不到小于最大
 * 允许值的字节则失败。还需要些特例逻辑以跳过代理对
 * 处理所用区域，因它们不应出现在
 * 合法 UTF-8 中。
 *
 * 注意我们不把低位字节重置为最小值，因负担不起
 * 穷举搜索（见 make_greater_string）。
 */
static bool
pg_utf8_increment(unsigned char *charptr, int length)
{
	unsigned char a;
	unsigned char limit;

	switch (length)
	{
		default:
			/* 暂拒绝长度 5 与 6 */
			return false;
		case 4:
			a = charptr[3];
			if (a < 0xBF)
			{
				charptr[3]++;
				break;
			}
			/* FALL THRU */
		case 3:
			a = charptr[2];
			if (a < 0xBF)
			{
				charptr[2]++;
				break;
			}
			/* FALL THRU */
		case 2:
			a = charptr[1];
			switch (*charptr)
			{
				case 0xED:
					limit = 0x9F;
					break;
				case 0xF4:
					limit = 0x8F;
					break;
				default:
					limit = 0xBF;
					break;
			}
			if (a < limit)
			{
				charptr[1]++;
				break;
			}
			/* FALL THRU */
		case 1:
			a = *charptr;
			if (a == 0x7F || a == 0xDF || a == 0xEF || a == 0xF4)
				return false;
			charptr[0]++;
			break;
	}

	return true;
}

/*
 * EUC-JP 字符递增函数。
 *
 * 若序列以 SS2 (0x8e) 开头，则必为两字节序列，表示
 * JIS X 0201 字符，第二字节在 0xa1 与 0xdf 之间。若末字节
 * 小于 0xdf 则只递增之，否则整序列改写为
 * 0xa1 0xa1。
 *
 * 若序列以 SS3 (0x8f) 开头，则必为三字节序列，末两字节
 * 在 0xa1 与 0xfe 之间。可能则递增末字节，否则递增
 * 倒数第二字节。
 *
 * 若序列以其他值开头且其 MSB 置位，则必为表示
 * JIS X 0208 字符的两字节序列，两字节均在 0xa1 与
 * 0xfe 之间。可能则递增末字节，否则递增
 * 倒数第二字节。
 *
 * 否则，序列为单字节 ASCII 字符。将其
 * 递增至 0x7f。
 */
static bool
pg_eucjp_increment(unsigned char *charptr, int length)
{
	unsigned char c1,
				c2;
	int			i;

	c1 = *charptr;

	switch (c1)
	{
		case SS2:				/* JIS X 0201 */
			if (length != 2)
				return false;

			c2 = charptr[1];

			if (c2 >= 0xdf)
				charptr[0] = charptr[1] = 0xa1;
			else if (c2 < 0xa1)
				charptr[1] = 0xa1;
			else
				charptr[1]++;
			break;

		case SS3:				/* JIS X 0212 */
			if (length != 3)
				return false;

			for (i = 2; i > 0; i--)
			{
				c2 = charptr[i];
				if (c2 < 0xa1)
				{
					charptr[i] = 0xa1;
					return true;
				}
				else if (c2 < 0xfe)
				{
					charptr[i]++;
					return true;
				}
			}

			/* 超出三字节码区 */
			return false;

		default:
			if (IS_HIGHBIT_SET(c1)) /* JIS X 0208? */
			{
				if (length != 2)
					return false;

				for (i = 1; i >= 0; i--)
				{
					c2 = charptr[i];
					if (c2 < 0xa1)
					{
						charptr[i] = 0xa1;
						return true;
					}
					else if (c2 < 0xfe)
					{
						charptr[i]++;
						return true;
					}
				}

				/* 超出两字节码区 */
				return false;
			}
			else
			{					/* ASCII，单字节 */
				if (c1 > 0x7e)
					return false;
				(*charptr)++;
			}
			break;
	}

	return true;
}

/*
 * 取当前数据库编码的字符递增器
 */
mbcharacter_incrementer
pg_database_encoding_character_incrementer(void)
{
	/*
	 * 最终最好在 pg_wchar_table[] 加字段，但目前
	 * 我们只用 switch。
	 */
	switch (GetDatabaseEncoding())
	{
		case PG_UTF8:
			return pg_utf8_increment;

		case PG_EUC_JP:
			return pg_eucjp_increment;

		default:
			return pg_generic_charinc;
	}
}

/*
 * 取当前数据库编码的最大长度
 */
int
pg_database_encoding_max_length(void)
{
	return pg_wchar_table[GetDatabaseEncoding()].maxmblen;
}

/*
 * 校验 mbstr，确保其在当前数据库
 * 编码下合法。其余同 pg_verify_mbstr()。
 */
bool
pg_verifymbstr(const char *mbstr, int len, bool noError)
{
	return pg_verify_mbstr(GetDatabaseEncoding(), mbstr, len, noError);
}

/*
 * 校验 mbstr，确保其在指定编码下
 * 合法。
 */
bool
pg_verify_mbstr(int encoding, const char *mbstr, int len, bool noError)
{
	int			oklen;

	Assert(PG_VALID_ENCODING(encoding));

	oklen = pg_wchar_table[encoding].mbverifystr((const unsigned char *) mbstr, len);
	if (oklen != len)
	{
		if (noError)
			return false;
		report_invalid_encoding(encoding, mbstr + oklen, len - oklen);
	}
	return true;
}

/*
 * 校验 mbstr，确保其在指定编码下
 * 合法。
 *
 * mbstr 不一定以零结尾；其长度由
 * len 指定。
 *
 * 若 OK，返回该编码下的串长。
 * 若发现问题，noError 为真时返回 -1；
 * noError 为假时 ereport() 描述性消息。
 *
 * 注意：无法用更快的编码专用 mbverifystr()，
 * 因我们需要统计串中的字符数。
 */
int
pg_verify_mbstr_len(int encoding, const char *mbstr, int len, bool noError)
{
	mbchar_verifier mbverifychar;
	int			mb_len;

	Assert(PG_VALID_ENCODING(encoding));

	/*
	 * 单字节编码中只需拒绝空字节 (\0)。
	 */
	if (pg_encoding_max_length(encoding) <= 1)
	{
		const char *nullpos = memchr(mbstr, 0, len);

		if (nullpos == NULL)
			return len;
		if (noError)
			return -1;
		report_invalid_encoding(encoding, nullpos, 1);
	}

	/* 函数指针只取一次 */
	mbverifychar = pg_wchar_table[encoding].mbverifychar;

	mb_len = 0;

	while (len > 0)
	{
		int			l;

		/* ASCII 子集字符的快速路径 */
		if (!IS_HIGHBIT_SET(*mbstr))
		{
			if (*mbstr != '\0')
			{
				mb_len++;
				mbstr++;
				len--;
				continue;
			}
			if (noError)
				return -1;
			report_invalid_encoding(encoding, mbstr, len);
		}

		l = (*mbverifychar) ((const unsigned char *) mbstr, len);

		if (l < 0)
		{
			if (noError)
				return -1;
			report_invalid_encoding(encoding, mbstr, len);
		}

		mbstr += l;
		len -= l;
		mb_len++;
	}
	return mb_len;
}

/*
 * check_encoding_conversion_args: 检查转换函数参数
 *
 * expected 参数可以是编码 ID，或 -1 表示由
 * 调用方检查是否接受该 ID。
 *
 * 注意：此处错误并非真正面向用户，故用 elog 而非
 * ereport 即足。另，我们信任 expected 编码参数是
 * 合法编码 ID，但不信任实际值。
 */
void
check_encoding_conversion_args(int src_encoding,
							   int dest_encoding,
							   int len,
							   int expected_src_encoding,
							   int expected_dest_encoding)
{
	if (!PG_VALID_ENCODING(src_encoding))
		elog(ERROR, "invalid source encoding ID: %d", src_encoding);
	if (src_encoding != expected_src_encoding && expected_src_encoding >= 0)
		elog(ERROR, "expected source encoding \"%s\", but got \"%s\"",
			 pg_enc2name_tbl[expected_src_encoding].name,
			 pg_enc2name_tbl[src_encoding].name);
	if (!PG_VALID_ENCODING(dest_encoding))
		elog(ERROR, "invalid destination encoding ID: %d", dest_encoding);
	if (dest_encoding != expected_dest_encoding && expected_dest_encoding >= 0)
		elog(ERROR, "expected destination encoding \"%s\", but got \"%s\"",
			 pg_enc2name_tbl[expected_dest_encoding].name,
			 pg_enc2name_tbl[dest_encoding].name);
	if (len < 0)
		elog(ERROR, "encoding conversion length must not be negative");
}

/*
 * report_invalid_encoding: 抱怨无效多字节字符
 *
 * 注意：len 是串的剩余长度，非字符长度；
 * len 必须大于零（否则会忽略初始化 "buf"）。
 */
void
report_invalid_encoding(int encoding, const char *mbstr, int len)
{
	int			l = pg_encoding_mblen_or_incomplete(encoding, mbstr, len);
	char		buf[8 * 5 + 1];
	char	   *p = buf;
	int			j,
				jlimit;

	jlimit = Min(l, len);
	jlimit = Min(jlimit, 8);	/* 防止缓冲越界 */

	for (j = 0; j < jlimit; j++)
	{
		p += sprintf(p, "0x%02x", (unsigned char) mbstr[j]);
		if (j < jlimit - 1)
			p += sprintf(p, " ");
	}

	ereport(ERROR,
			(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
			 errmsg("invalid byte sequence for encoding \"%s\": %s",
					pg_enc2name_tbl[encoding].name,
					buf)));
}

/*
 * report_untranslatable_char: 抱怨不可翻译字符
 *
 * 注意：len 是串的剩余长度，非字符长度；
 * len 必须大于零（否则会忽略初始化 "buf"）。
 */
void
report_untranslatable_char(int src_encoding, int dest_encoding,
						   const char *mbstr, int len)
{
	int			l;
	char		buf[8 * 5 + 1];
	char	   *p = buf;
	int			j,
				jlimit;

	/*
	 * 大概可用普通 pg_encoding_mblen()，因
	 * gb18030_to_utf8() 在转换前校验。凡转换皆应如此。
	 * 对 src_encoding!=GB18030，len>0 满足 pg_encoding_mblen()。即便
	 * 如此仍须防备，因有缺陷转换可能传入无效数据。
	 * 这不是性能关键路径。
	 */
	l = pg_encoding_mblen_or_incomplete(src_encoding, mbstr, len);
	jlimit = Min(l, len);
	jlimit = Min(jlimit, 8);	/* 防止缓冲越界 */

	for (j = 0; j < jlimit; j++)
	{
		p += sprintf(p, "0x%02x", (unsigned char) mbstr[j]);
		if (j < jlimit - 1)
			p += sprintf(p, " ");
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
			 errmsg("character with byte sequence %s in encoding \"%s\" has no equivalent in encoding \"%s\"",
					buf,
					pg_enc2name_tbl[src_encoding].name,
					pg_enc2name_tbl[dest_encoding].name)));
}


#ifdef WIN32
/*
 * 把 MessageEncoding 转为 palloc 的、空终止的 utf16
 * 串。若 utf16len 非空也传入字符长度。
 * 失败则返回 NULL。MessageEncoding 初始化前，"str"
 * 应为仅 ASCII；此时作用如同 MessageEncoding 为 UTF8。
 */
WCHAR *
pgwin32_message_to_UTF16(const char *str, int len, int *utf16len)
{
	int			msgenc = GetMessageEncoding();
	WCHAR	   *utf16;
	int			dstlen;
	UINT		codepage;

	if (msgenc == PG_SQL_ASCII)
		/* 无法转换，且 SQL_ASCII 从不是 utf16。 */
		return NULL;

	codepage = pg_enc2name_tbl[msgenc].codepage;

	/*
	 * 若有对应代码页则直接用 MultiByteToWideChar，
	 * 否则经 UTF8 做双重转换。双重转换例如用于
	 * ENCODING=LATIN8, LC_CTYPE=C 的数据库。
	 */
	if (codepage != 0)
	{
		utf16 = (WCHAR *) palloc(sizeof(WCHAR) * (len + 1));
		dstlen = MultiByteToWideChar(codepage, 0, str, len, utf16, len);
		utf16[dstlen] = (WCHAR) 0;
	}
	else
	{
		char	   *utf8;

		/*
		 * XXX pg_do_encoding_conversion() 需要事务。若无
		 * 事务，则指望输入为合法 UTF8。
		 */
		if (IsTransactionState())
		{
			utf8 = (char *) pg_do_encoding_conversion((unsigned char *) str,
													  len,
													  msgenc,
													  PG_UTF8);
			if (utf8 != str)
				len = strlen(utf8);
		}
		else
			utf8 = (char *) str;

		utf16 = (WCHAR *) palloc(sizeof(WCHAR) * (len + 1));
		dstlen = MultiByteToWideChar(CP_UTF8, 0, utf8, len, utf16, len);
		utf16[dstlen] = (WCHAR) 0;

		if (utf8 != str)
			pfree(utf8);
	}

	if (dstlen == 0 && len > 0)
	{
		pfree(utf16);
		return NULL;			/* error */
	}

	if (utf16len)
		*utf16len = dstlen;
	return utf16;
}

#endif							/* WIN32 */
