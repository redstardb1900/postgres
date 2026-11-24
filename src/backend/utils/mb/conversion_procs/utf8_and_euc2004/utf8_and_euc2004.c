/*-------------------------------------------------------------------------
 *
 *	  EUC_JIS_2004 <--> UTF8
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conversion_procs/utf8_and_euc2004/utf8_and_euc2004.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "../../Unicode/euc_jis_2004_to_utf8.map"
#include "../../Unicode/utf8_to_euc_jis_2004.map"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(euc_jis_2004_to_utf8);
PG_FUNCTION_INFO_V1(utf8_to_euc_jis_2004);

/* ----------
 * conv_proc(
 *		INTEGER,	-- 源编码 id
 *		INTEGER,	-- 目标编码 id
 *		CSTRING,	-- 源串 (NUL 终止 C 串)
 *		CSTRING,	-- 目标串 (NUL 终止 C 串)
 *		INTEGER,	-- 源串长度
 *		BOOL		-- 为真时转换失败不抛错
 * ) returns INTEGER;
 *
 * 返回成功转换的字节数。
 * ----------
 */
Datum
euc_jis_2004_to_utf8(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_EUC_JIS_2004, PG_UTF8);

	converted = LocalToUtf(src, len, dest,
						   &euc_jis_2004_to_unicode_tree,
						   LUmapEUC_JIS_2004_combined, lengthof(LUmapEUC_JIS_2004_combined),
						   NULL,
						   PG_EUC_JIS_2004,
						   noError);

	PG_RETURN_INT32(converted);
}

Datum
utf8_to_euc_jis_2004(PG_FUNCTION_ARGS)
{
	unsigned char *src = (unsigned char *) PG_GETARG_CSTRING(2);
	unsigned char *dest = (unsigned char *) PG_GETARG_CSTRING(3);
	int			len = PG_GETARG_INT32(4);
	bool		noError = PG_GETARG_BOOL(5);
	int			converted;

	CHECK_ENCODING_CONVERSION_ARGS(PG_UTF8, PG_EUC_JIS_2004);

	converted = UtfToLocal(src, len, dest,
						   &euc_jis_2004_from_unicode_tree,
						   ULmapEUC_JIS_2004_combined, lengthof(ULmapEUC_JIS_2004_combined),
						   NULL,
						   PG_EUC_JIS_2004,
						   noError);

	PG_RETURN_INT32(converted);
}
