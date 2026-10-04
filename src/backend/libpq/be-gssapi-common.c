/*-------------------------------------------------------------------------
 *
 * be-gssapi-common.c
 *     Common code for GSSAPI authentication and encryption
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *       src/backend/libpq/be-gssapi-common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/be-gssapi-common.h"

/*
 * 取出某一特定类型的全部错误并追加到 "s"（大小为 len 的缓冲区）。
 * 若得到不止一个字符串，用空格分隔。
 * 对 GSS_CODE 和 MECH_CODE 各调用一次。
 */
static void
pg_GSS_error_int(char *s, size_t len, OM_uint32 stat, int type)
{
	gss_buffer_desc gmsg;
	size_t		i = 0;
	OM_uint32	lmin_s,
				msg_ctx = 0;

	do
	{
		if (gss_display_status(&lmin_s, stat, type, GSS_C_NO_OID,
							   &msg_ctx, &gmsg) != GSS_S_COMPLETE)
			break;
		if (i > 0)
		{
			if (i < len)
				s[i] = ' ';
			i++;
		}
		if (i < len)
			memcpy(s + i, gmsg.value, Min(len - i, gmsg.length));
		i += gmsg.length;
		gss_release_buffer(&lmin_s, &gmsg);
	}
	while (msg_ctx);

	/* 补上空字符结尾 */
	if (i < len)
		s[i] = '\0';
	else
	{
		elog(COMMERROR, "incomplete GSS error report");
		s[len - 1] = '\0';
	}
}

/*
 * 报告由 maj_stat/min_stat 描述的 GSSAPI 错误。
 *
 * errmsg 应当是已经翻译过的主错误消息。
 * GSSAPI 信息作为 errdetail 追加。
 *
 * 错误始终以级别 COMMERROR 报告；我们不敢把它发给客户端，
 * 因为 elog.c 试图写往客户端时，那很可能导致无限递归。
 *
 * 为避免内存分配，错误总大小有上限（主状态和次状态各
 * 128 字节）。没有已知机制会产生超出该上限的错误消息。
 *
 *
 */
void
pg_GSS_error(const char *errmsg,
			 OM_uint32 maj_stat, OM_uint32 min_stat)
{
	char		msg_major[128],
				msg_minor[128];

	/* 取主状态消息 */
	pg_GSS_error_int(msg_major, sizeof(msg_major), maj_stat, GSS_C_GSS_CODE);

	/* 取机制次状态消息 */
	pg_GSS_error_int(msg_minor, sizeof(msg_minor), min_stat, GSS_C_MECH_CODE);

	/*
 * 使用 errmsg_internal，因为前半部分的翻译反正必须在
 * 调用本函数之前完成。
 */
	ereport(COMMERROR,
			(errmsg_internal("%s", errmsg),
			 errdetail_internal("%s: %s", msg_major, msg_minor)));
}

/*
 * 把传入的凭证存进内存缓存，供以后使用。
 *
 * 这样委托给我们的凭证就可以用来连接其他系统，例如
 * 通过 postgres_fdw 或 dblink。
 */
#define GSS_MEMORY_CACHE "MEMORY:"
void
pg_store_delegated_credential(gss_cred_id_t cred)
{
	OM_uint32	major,
				minor;
	gss_OID_set mech;
	gss_cred_usage_t usage;
	gss_key_value_element_desc cc;
	gss_key_value_set_desc ccset;

	cc.key = "ccache";
	cc.value = GSS_MEMORY_CACHE;
	ccset.count = 1;
	ccset.elements = &cc;

	/* 使委托的凭证只对当前进程可用 */
	major = gss_store_cred_into(&minor,
								cred,
								GSS_C_INITIATE, /* 凭证只用于
												 * 启动 libpq 连接 */
								GSS_C_NULL_OID, /* 全部存储 */
								true,	/* 覆盖 */
								true,	/* 设为默认 */
								&ccset,
								&mech,
								&usage);

	if (major != GSS_S_COMPLETE)
	{
		pg_GSS_error("gss_store_cred", major, minor);
	}

	/* 凭证已存储，因此可以释放我们的凭证句柄。 */
	major = gss_release_cred(&minor, &cred);
	if (major != GSS_S_COMPLETE)
	{
		pg_GSS_error("gss_release_cred", major, minor);
	}

	/*
 * 为本后端设置 KRB5CCNAME，以便稍后调用 gss_acquire_cred
 * 时能找到我们存储的委托凭证。
 */
	setenv("KRB5CCNAME", GSS_MEMORY_CACHE, 1);
}
