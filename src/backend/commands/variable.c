/*-------------------------------------------------------------------------
 *
 * variable.c
 *		处理专用 SET 变量的例程。             
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/variable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "catalog/pg_authid.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/guc_hooks.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tzparser.h"
#include "utils/varlena.h"

/*
 * DATESTYLE
 */

/*
 * check_datestyle: datestyle 的 GUC check_hook
 */
bool
check_datestyle(char **newval, void **extra, GucSource source)
{
	int			newDateStyle = DateStyle;
	int			newDateOrder = DateOrder;
	bool		have_style = false;
	bool		have_order = false;
	bool		ok = true;
	char	   *rawstring;
	int		   *myextra;
	char	   *result;
	List	   *elemlist;
	ListCell   *l;

	/* 需要字符串的可修改副本 */
	rawstring = pstrdup(*newval);

	/* 将字符串解析为标识符列表 */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* 列表语法错误 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	foreach(l, elemlist)
	{
		char	   *tok = (char *) lfirst(l);

		/* 呃。应该有人写一个表驱动版本 -- mjl */

		if (pg_strcasecmp(tok, "ISO") == 0)
		{
			if (have_style && newDateStyle != USE_ISO_DATES)
				ok = false;		/* 样式冲突 */
			newDateStyle = USE_ISO_DATES;
			have_style = true;
		}
		else if (pg_strcasecmp(tok, "SQL") == 0)
		{
			if (have_style && newDateStyle != USE_SQL_DATES)
				ok = false;		/* 样式冲突 */
			newDateStyle = USE_SQL_DATES;
			have_style = true;
		}
		else if (pg_strncasecmp(tok, "POSTGRES", 8) == 0)
		{
			if (have_style && newDateStyle != USE_POSTGRES_DATES)
				ok = false;		/* 样式冲突 */
			newDateStyle = USE_POSTGRES_DATES;
			have_style = true;
		}
		else if (pg_strcasecmp(tok, "GERMAN") == 0)
		{
			if (have_style && newDateStyle != USE_GERMAN_DATES)
				ok = false;		/* 样式冲突 */
			newDateStyle = USE_GERMAN_DATES;
			have_style = true;
			/* GERMAN 同时设置 DMY，除非显式覆盖 */
			if (!have_order)
				newDateOrder = DATEORDER_DMY;
		}
		else if (pg_strcasecmp(tok, "YMD") == 0)
		{
			if (have_order && newDateOrder != DATEORDER_YMD)
				ok = false;		/* 排序冲突 */
			newDateOrder = DATEORDER_YMD;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "DMY") == 0 ||
				 pg_strncasecmp(tok, "EURO", 4) == 0)
		{
			if (have_order && newDateOrder != DATEORDER_DMY)
				ok = false;		/* 排序冲突 */
			newDateOrder = DATEORDER_DMY;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "MDY") == 0 ||
				 pg_strcasecmp(tok, "US") == 0 ||
				 pg_strncasecmp(tok, "NONEURO", 7) == 0)
		{
			if (have_order && newDateOrder != DATEORDER_MDY)
				ok = false;		/* 排序冲突 */
			newDateOrder = DATEORDER_MDY;
			have_order = true;
		}
		else if (pg_strcasecmp(tok, "DEFAULT") == 0)
		{
			/*
			 * 获取当前 DEFAULT 状态的最简单方法是
			 * 从 guc.c 获取 DEFAULT 字符串并递归解析。
			 *
			 * 我们不能简单地 "return check_datestyle(...)"，
			 * 因为需要处理 "DEFAULT, ISO" 这样的构造。
			 */
			char	   *subval;
			void	   *subextra = NULL;

			subval = guc_strdup(LOG, GetConfigOptionResetString("datestyle"));
			if (!subval)
			{
				ok = false;
				break;
			}
			if (!check_datestyle(&subval, &subextra, source))
			{
				guc_free(subval);
				ok = false;
				break;
			}
			myextra = (int *) subextra;
			if (!have_style)
				newDateStyle = myextra[0];
			if (!have_order)
				newDateOrder = myextra[1];
			guc_free(subval);
			guc_free(subextra);
		}
		else
		{
			GUC_check_errdetail("Unrecognized key word: \"%s\".", tok);
			pfree(rawstring);
			list_free(elemlist);
			return false;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	if (!ok)
	{
		GUC_check_errdetail("Conflicting \"datestyle\" specifications.");
		return false;
	}

	/*
	 * 准备规范字符串以返回。GUC 要求用 guc_malloc 分配。
	 */
	result = (char *) guc_malloc(LOG, 32);
	if (!result)
		return false;

	switch (newDateStyle)
	{
		case USE_ISO_DATES:
			strcpy(result, "ISO");
			break;
		case USE_SQL_DATES:
			strcpy(result, "SQL");
			break;
		case USE_GERMAN_DATES:
			strcpy(result, "German");
			break;
		default:
			strcpy(result, "Postgres");
			break;
	}
	switch (newDateOrder)
	{
		case DATEORDER_YMD:
			strcat(result, ", YMD");
			break;
		case DATEORDER_DMY:
			strcat(result, ", DMY");
			break;
		default:
			strcat(result, ", MDY");
			break;
	}

	guc_free(*newval);
	*newval = result;

	/*
	 * 设置 assign_datestyle 实际使用的 "extra" 结构。
	 */
	myextra = (int *) guc_malloc(LOG, 2 * sizeof(int));
	if (!myextra)
		return false;
	myextra[0] = newDateStyle;
	myextra[1] = newDateOrder;
	*extra = (void *) myextra;

	return true;
}

/*
 * assign_datestyle: datestyle 的 GUC assign_hook
 */
void
assign_datestyle(const char *newval, void *extra)
{
	int		   *myextra = (int *) extra;

	DateStyle = myextra[0];
	DateOrder = myextra[1];
}


/*
 * TIMEZONE
 */

/*
 * check_timezone: timezone 的 GUC check_hook
 */
bool
check_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;
	long		gmtoffset;
	char	   *endptr;
	double		hours;

	if (pg_strncasecmp(*newval, "interval", 8) == 0)
	{
		/*
		 * 支持 INTERVAL 'foo'。这是为了 SQL 规范合规，
		 * 并非有任何实际用途。
		 */
		const char *valueptr = *newval;
		char	   *val;
		Interval   *interval;

		valueptr += 8;
		while (isspace((unsigned char) *valueptr))
			valueptr++;
		if (*valueptr++ != '\'')
			return false;
		val = pstrdup(valueptr);
		/* 检查并移除尾部引号 */
		endptr = strchr(val, '\'');
		if (!endptr || endptr[1] != '\0')
		{
			pfree(val);
			return false;
		}
		*endptr = '\0';

		/*
		 * 尝试解析。XXX 无效的区间格式会导致
		 * ereport(ERROR)，这对 GUC 来说不理想。
		 * 我们在 flatten_set_variable_args 中已经尽力防范，
		 * 但来自 postgresql.conf 的字符串可能包含任何内容。
		 */
		interval = DatumGetIntervalP(DirectFunctionCall3(interval_in,
														 CStringGetDatum(val),
														 ObjectIdGetDatum(InvalidOid),
														 Int32GetDatum(-1)));

		pfree(val);
		if (interval->month != 0)
		{
			GUC_check_errdetail("Cannot specify months in time zone interval.");
			pfree(interval);
			return false;
		}
		if (interval->day != 0)
		{
			GUC_check_errdetail("Cannot specify days in time zone interval.");
			pfree(interval);
			return false;
		}

		/* 这里我们从 SQL 符号约定更改为 Unix 符号约定 */
		gmtoffset = -(interval->time / USECS_PER_SEC);
		new_tz = pg_tzset_offset(gmtoffset);

		pfree(interval);
	}
	else
	{
		/*
		 * 尝试作为数字小时数（可能带小数）。
		 */
		hours = strtod(*newval, &endptr);
		if (endptr != *newval && *endptr == '\0')
		{
			/* 这里我们从 SQL 符号约定更改为 Unix 符号约定 */
			gmtoffset = -hours * SECS_PER_HOUR;
			new_tz = pg_tzset_offset(gmtoffset);
		}
		else
		{
			/*
			 * 否则假定它是时区名称，并尝试加载。
			 */
			new_tz = pg_tzset(*newval);

			if (!new_tz)
			{
				/* 这里在 errdetail 中似乎没有太大价值 */
				return false;
			}

			if (!pg_tz_acceptable(new_tz))
			{
				GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
								 *newval);
				GUC_check_errdetail("PostgreSQL does not support leap seconds.");
				return false;
			}
		}
	}

	/* 测试 pg_tzset_offset 失败，我们假设是超出范围 */
	if (!new_tz)
	{
		GUC_check_errdetail("UTC timezone offset is out of range.");
		return false;
	}

	/*
	 * 传递数据供 assign_timezone 使用
	 */
	*extra = guc_malloc(LOG, sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_timezone: timezone 的 GUC assign_hook
 */
void
assign_timezone(const char *newval, void *extra)
{
	session_timezone = *((pg_tz **) extra);
}

/*
 * show_timezone: timezone 的 GUC show_hook
 */
const char *
show_timezone(void)
{
	const char *tzn;

	/* 始终显示区域的规范名称 */
	tzn = pg_get_timezone_name(session_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * LOG_TIMEZONE
 *
 * 对于 log_timezone，我们不支持基于区间的时区设置方法，
 * 这些方法仅用于 SQL 规范合规，
 * 并非实际有用。
 */

/*
 * check_log_timezone: log_timezone 的 GUC check_hook
 */
bool
check_log_timezone(char **newval, void **extra, GucSource source)
{
	pg_tz	   *new_tz;

	/*
	 * 假定它是时区名称，并尝试加载。
	 */
	new_tz = pg_tzset(*newval);

	if (!new_tz)
	{
		/* 这里在 errdetail 中似乎没有太大价值 */
		return false;
	}

	if (!pg_tz_acceptable(new_tz))
	{
		GUC_check_errmsg("time zone \"%s\" appears to use leap seconds",
						 *newval);
		GUC_check_errdetail("PostgreSQL does not support leap seconds.");
		return false;
	}

	/*
	 * 传递数据供 assign_log_timezone 使用
	 */
	*extra = guc_malloc(LOG, sizeof(pg_tz *));
	if (!*extra)
		return false;
	*((pg_tz **) *extra) = new_tz;

	return true;
}

/*
 * assign_log_timezone: log_timezone 的 GUC assign_hook
 */
void
assign_log_timezone(const char *newval, void *extra)
{
	log_timezone = *((pg_tz **) extra);
}

/*
 * show_log_timezone: log_timezone 的 GUC show_hook
 */
const char *
show_log_timezone(void)
{
	const char *tzn;

	/* 始终显示区域的规范名称 */
	tzn = pg_get_timezone_name(log_timezone);

	if (tzn != NULL)
		return tzn;

	return "unknown";
}


/*
 * TIMEZONE_ABBREVIATIONS
 */

/*
 * timezone_abbreviations 的 GUC check_hook
 */
bool
check_timezone_abbreviations(char **newval, void **extra, GucSource source)
{
	/*
	 * timezone_abbreviations 的 boot_val 是 NULL。
	 * 当我们看到它时什么也不做。如果配置文件中没有覆盖该值，
	 * 则 pg_timezone_abbrev_initialize() 最终会用 "Default" 替换它。
	 * 此 hack 有两个目的：避免浪费周期加载
	 * 可能很快会被配置文件覆盖的值，
	 * 以及避免在 InitializeGUCOptions() 期间尝试读取时区缩写文件。
	 * 后者在 EXEC_BACKEND 子进程中不起作用，因为 my_exec_path 尚未设置，
	 * 因此无法定位 PGSHAREDIR。
	 */
	if (*newval == NULL)
	{
		Assert(source == PGC_S_DEFAULT);
		return true;
	}

	/* 好的，加载文件并生成 guc_malloc 分配的 TimeZoneAbbrevTable */
	*extra = load_tzoffsets(*newval);

	/* tzparser.c 在失败时返回 NULL，通过 GUC_check_errmsg 报告 */
	if (!*extra)
		return false;

	return true;
}

/*
 * timezone_abbreviations 的 GUC assign_hook
 */
void
assign_timezone_abbreviations(const char *newval, void *extra)
{
	/* 对于 boot_val 默认值 NULL 不执行任何操作 */
	if (!extra)
		return;

	InstallTimeZoneAbbrevs((TimeZoneAbbrevTable *) extra);
}


/*
 * SET TRANSACTION READ ONLY 和 SET TRANSACTION READ WRITE
 *
 * 我们允许在任何时候进行幂等变更（r/w -> r/w 和 r/o -> r/o），
 * 并且始终允许从读写更改为只读。但是，
 * 只读只能在尚未获取初始快照的顶级事务中
 * 更改为读写。
 * 在热备中也不能更改。
 *
 * 如果根本不在事务中，则直接允许更改；这没有任何意义，
 * 因为 XactReadOnly 将在下次 StartTransaction() 时被重置。
 * IsTransactionState() 测试保护我们免于在不访问共享内存的上下文中
 * 尝试检查 RecoveryInProgress()。
 * （同样，如果我们在并行工作进程中恢复状态，
 * 也直接允许更改。）
 */
bool
check_transaction_read_only(bool *newval, void **extra, GucSource source)
{
	if (*newval == false && XactReadOnly && IsTransactionState() && !InitializingParallelWorker)
	{
		/* 不能在只读事务内切换到读写模式 */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("cannot set transaction read-write mode inside a read-only transaction");
			return false;
		}
		/* 顶级事务在获取第一个快照后无法更改为读写。 */
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("transaction read-write mode must be set before any query");
			return false;
		}
		/* 恢复期间无法切换到读写模式 */
		if (RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot set transaction read-write mode during recovery");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION ISOLATION LEVEL
 *
 * 我们允许在任何时候进行幂等变更，但除此之外，
 * 只能在尚未获取快照的顶级事务中更改。
 *
 * 与 check_transaction_read_only 一样，如果不在事务中也允许。
 */
bool
check_transaction_isolation(int *newval, void **extra, GucSource source)
{
	int			newXactIsoLevel = *newval;

	if (newXactIsoLevel != XactIsoLevel && IsTransactionState())
	{
		if (FirstSnapshotSet)
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must be called before any query");
			return false;
		}
		/* 我们忽略子事务将其设置为现有值。 */
		if (IsSubTransaction())
		{
			GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
			GUC_check_errmsg("SET TRANSACTION ISOLATION LEVEL must not be called in a subtransaction");
			return false;
		}
		/* 恢复期间无法切换到可序列化模式 */
		if (newXactIsoLevel == XACT_SERIALIZABLE && RecoveryInProgress())
		{
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errmsg("cannot use serializable mode in a hot standby");
			GUC_check_errhint("You can use REPEATABLE READ instead.");
			return false;
		}
	}

	return true;
}

/*
 * SET TRANSACTION [NOT] DEFERRABLE
 */

bool
check_transaction_deferrable(bool *newval, void **extra, GucSource source)
{
	if (IsSubTransaction())
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE cannot be called within a subtransaction");
		return false;
	}
	if (FirstSnapshotSet)
	{
		GUC_check_errcode(ERRCODE_ACTIVE_SQL_TRANSACTION);
		GUC_check_errmsg("SET TRANSACTION [NOT] DEFERRABLE must be called before any query");
		return false;
	}

	return true;
}

/*
 * 随机数种子
 *
 * 我们不能在出错时回滚随机序列，也不希望配置
 * 文件重载影响它，因此只允许交互式 SET SEED 命令设置它。
 * 我们使用 "extra" 存储来确保回滚
 * 不会再次尝试执行该操作。
 */

bool
check_random_seed(double *newval, void **extra, GucSource source)
{
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	/* 仅当值来源是交互式 SET 时才启用 assign */
	*((int *) *extra) = (source >= PGC_S_INTERACTIVE);

	return true;
}

void
assign_random_seed(double newval, void *extra)
{
	/* 对于 GUC 变量的任何设置，我们最多执行一次此操作 */
	if (*((int *) extra))
		DirectFunctionCall1(setseed, Float8GetDatum(newval));
	*((int *) extra) = 0;
}

const char *
show_random_seed(void)
{
	return "unavailable";
}


/*
 * SET CLIENT_ENCODING
 */

bool
check_client_encoding(char **newval, void **extra, GucSource source)
{
	int			encoding;
	const char *canonical_name;

	/* 按名称查找编码 */
	encoding = pg_valid_client_encoding(*newval);
	if (encoding < 0)
		return false;

	/* 获取规范名称（无别名，统一大小写） */
	canonical_name = pg_encoding_to_char(encoding);

	/*
	 * 如果不在事务中，PrepareClientEncoding 将无法查找必要的
	 * 转换过程。如果仍在启动中，它会返回 "OK"，
	 * InitializeClientEncoding 将在初始化足够深入后修复问题。
	 * 启动后，我们将失败。这只会发生在有人尝试在 postgresql.conf
	 * 中更改 client_encoding 然后 SIGHUP 现有会话的情况下。
	 * 无论如何，以这种方式更改 client_encoding
	 * 似乎是个坏主意，
	 * 因此我们不会特意支持它。
	 *
	 * 注意：在 postmaster 或任何从不调用 InitializeClientEncoding
	 * 的其他进程中，PrepareClientEncoding 将始终成功，
	 * SetClientEncoding 也将成功；但它们不会做任何事情，这没问题。
	 */
	if (PrepareClientEncoding(encoding) < 0)
	{
		if (IsTransactionState())
		{
			/* 必须是真正的无此类转换问题 */
			GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
			GUC_check_errdetail("Conversion between %s and %s is not supported.",
								canonical_name,
								GetDatabaseEncodingName());
		}
		else
		{
			/* 提供有用的投诉信息 */
			GUC_check_errdetail("Cannot change \"client_encoding\" now.");
		}
		return false;
	}

	/*
	 * 用编码的规范名称替换用户提供的字符串。
	 * 这消除了别名和大小写变化。
	 *
	 * XXX 尽管规范化在抽象上是好主意，但它会破坏 pre-9.1
	 * JDBC 驱动程序，这些驱动程序期望如果发送 "UNICODE"
	 * 作为 client_encoding 设置，则读回的值也相同。
	 * 作为变通方案，如果字符串是 "UNICODE" 则不替换。
	 * 当 pre-9.1 JDBC 驱动程序不再使用时移除此 hack。
	 */
	if (strcmp(*newval, canonical_name) != 0 &&
		strcmp(*newval, "UNICODE") != 0)
	{
		guc_free(*newval);
		*newval = guc_strdup(LOG, canonical_name);
		if (!*newval)
			return false;
	}

	/*
	 * 将编码 ID 保存在 *extra 中，供 assign_client_encoding 使用。
	 */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = encoding;

	return true;
}

void
assign_client_encoding(const char *newval, void *extra)
{
	int			encoding = *((int *) extra);

	/*
	 * 并行工作进程向领导者发送数据，而不是客户端。
	 * 它们始终使用数据库编码发送数据。
	 */
	if (IsParallelWorker())
	{
		/*
		 * 在并行工作进程启动期间，我们要接受领导者的
		 * client_encoding 设置，以便在工作进程中查看该值的
		 * 任何人看到的值与在领导者中看到的相同。
		 */
		if (InitializingParallelWorker)
			return;

		/*
		 * 在启动期间以外的更改，例如附加到函数定义中的 SET 子句，
		 * 应被拒绝，
		 * 因为我们在工作进程中无法使其生效。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot change client_encoding during a parallel operation")));
	}

	/* 如果 PrepareClientEncoding 成功，我们不期望出现错误 */
	if (SetClientEncoding(encoding) < 0)
		elog(LOG, "SetClientEncoding(%d) failed", encoding);
}


/*
 * SET SESSION AUTHORIZATION
 */

typedef struct
{
	/* 这是 SESSION AUTHORIZATION 和 ROLE 共用的 "extra" 状态 */
	Oid			roleid;
	bool		is_superuser;
} role_auth_extra;

bool
check_session_authorization(char **newval, void **extra, GucSource source)
{
	HeapTuple	roleTup;
	Form_pg_authid roleform;
	Oid			roleid;
	bool		is_superuser;
	role_auth_extra *myextra;

	/* 对于 boot_val 默认值 NULL 不执行任何操作 */
	if (*newval == NULL)
		return true;

	if (InitializingParallelWorker)
	{
		/*
		 * 在并行工作进程初始化中，我们要复制领导者的状态，
		 * 即使它不再匹配目录。ParallelWorkerMain 已经安装了
		 * 正确的角色 OID 和超级用户状态。
		 */
		roleid = GetSessionUserId();
		is_superuser = GetSessionUserIsSuperuser();
	}
	else
	{
		if (!IsTransactionState())
		{
			/*
			 * 无法进行目录查找，因此失败。
			 * 这样做的结果是 session_authorization 不能在 postgresql.conf
			 * 中设置，这似乎无论如何都是件好事，
			 * 所以我们不努力避免它。
			 */
			return false;
		}

		/*
		 * 当 source == PGC_S_TEST 时，对于不存在的用户名或
		 * 权限不足，我们不会抛出硬错误，只发出 NOTICE。
		 * 参见 guc.h 中的注释。
		 */

		/* 查找用户名 */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*newval));
		if (!HeapTupleIsValid(roleTup))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", *newval)));
				return true;
			}
			GUC_check_errmsg("role \"%s\" does not exist", *newval);
			return false;
		}

		roleform = (Form_pg_authid) GETSTRUCT(roleTup);
		roleid = roleform->oid;
		is_superuser = roleform->rolsuper;

		ReleaseSysCache(roleTup);

		/*
		 * 只有超级用户可以 SET SESSION AUTHORIZATION 为自身以外的
		 * 角色。注意，在单个会话中多次 SET 的情况下，
		 * 原始认证用户的超级用户状态才是重要的。
		 */
		if (roleid != GetAuthenticatedUserId() &&
			!GetAuthenticatedUserIsSuperuser())
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission will be denied to set session authorization \"%s\"",
								*newval)));
				return true;
			}
			GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
			GUC_check_errmsg("permission denied to set session authorization \"%s\"",
							 *newval);
			return false;
		}
	}

	/* 设置 "extra" 结构供 assign_session_authorization 使用 */
	myextra = (role_auth_extra *) guc_malloc(LOG, sizeof(role_auth_extra));
	if (!myextra)
		return false;
	myextra->roleid = roleid;
	myextra->is_superuser = is_superuser;
	*extra = (void *) myextra;

	return true;
}

void
assign_session_authorization(const char *newval, void *extra)
{
	role_auth_extra *myextra = (role_auth_extra *) extra;

	/* 对于 boot_val 默认值 NULL 不执行任何操作 */
	if (!myextra)
		return;

	SetSessionAuthorization(myextra->roleid, myextra->is_superuser);
}


/*
 * SET ROLE
 *
 * SQL 规范要求 "SET ROLE NONE" 来取消角色，因此我们将
 * "none" 硬编码翻译为 InvalidOid。除此之外，
 * 这与 SET SESSION AUTHORIZATION 非常相似。
 */
extern char *role_string;		/* 在 guc_tables.c 中 */

bool
check_role(char **newval, void **extra, GucSource source)
{
	HeapTuple	roleTup;
	Oid			roleid;
	bool		is_superuser;
	role_auth_extra *myextra;
	Form_pg_authid roleform;

	if (strcmp(*newval, "none") == 0)
	{
		/* 硬编码翻译 */
		roleid = InvalidOid;
		is_superuser = false;
	}
	else if (InitializingParallelWorker)
	{
		/*
		 * 在并行工作进程初始化中，我们要复制领导者的状态，
		 * 即使它不再匹配目录。ParallelWorkerMain 已经安装了
		 * 正确的角色 OID 和超级用户状态。
		 */
		roleid = GetCurrentRoleId();
		is_superuser = session_auth_is_superuser;
	}
	else
	{
		if (!IsTransactionState())
		{
			/*
			 * 无法进行目录查找，因此失败。这样做的结果是
			 * role 不能在 postgresql.conf 中设置，
			 * 这似乎无论如何都是件好事，所以我们不努力避免它。
			 */
			return false;
		}

		/*
		 * 当 source == PGC_S_TEST 时，对于不存在的用户名或
		 * 权限不足，我们不会抛出硬错误，只发出 NOTICE。
		 * 参见 guc.h 中的注释。
		 */

		/* 查找用户名 */
		roleTup = SearchSysCache1(AUTHNAME, PointerGetDatum(*newval));
		if (!HeapTupleIsValid(roleTup))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("role \"%s\" does not exist", *newval)));
				return true;
			}
			GUC_check_errmsg("role \"%s\" does not exist", *newval);
			return false;
		}

		roleform = (Form_pg_authid) GETSTRUCT(roleTup);
		roleid = roleform->oid;
		is_superuser = roleform->rolsuper;

		ReleaseSysCache(roleTup);

		/* 验证会话用户是否被允许成为此角色 */
		if (!member_can_set_role(GetSessionUserId(), roleid))
		{
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission will be denied to set role \"%s\"",
								*newval)));
				return true;
			}
			GUC_check_errcode(ERRCODE_INSUFFICIENT_PRIVILEGE);
			GUC_check_errmsg("permission denied to set role \"%s\"",
							 *newval);
			return false;
		}
	}

	/* 设置 "extra" 结构供 assign_role 使用 */
	myextra = (role_auth_extra *) guc_malloc(LOG, sizeof(role_auth_extra));
	if (!myextra)
		return false;
	myextra->roleid = roleid;
	myextra->is_superuser = is_superuser;
	*extra = (void *) myextra;

	return true;
}

void
assign_role(const char *newval, void *extra)
{
	role_auth_extra *myextra = (role_auth_extra *) extra;

	SetCurrentRoleId(myextra->roleid, myextra->is_superuser);
}

const char *
show_role(void)
{
	/*
	 * 检查 SET ROLE 是否活跃；如果不活跃则返回 "none"。
	 * 这是一个 kluge，用于处理 SET SESSION AUTHORIZATION 逻辑上
	 * 将 SET ROLE 重置为 NONE，但我们不能从
	 * assign_session_authorization 设置 GUC role 变量
	 * （因为我们没有足够的信息来调用 set_config_option）的事实。
	 */
	if (!OidIsValid(GetCurrentRoleId()))
		return "none";

	/* 否则我们可以直接使用 GUC 字符串 */
	return role_string ? role_string : "none";
}


/*
 * 路径变量
 *
 * check_canonical_path 用于 log_directory 和其他一些 GUC，
 * 我们只需要规范化表示的路径名。
 */

bool
check_canonical_path(char **newval, void **extra, GucSource source)
{
	/*
	 * 由于 canonicalize_path 从不放大字符串，我们可以直接就地
	 * 修改 newval。但要注意 NULL，它是 external_pid_file 的
	 * 默认值。
	 */
	if (*newval)
		canonicalize_path(*newval);
	return true;
}


/*
 * 杂项
 */

/*
 * application_name 的 GUC check_hook
 */
bool
check_application_name(char **newval, void **extra, GucSource source)
{
	char	   *clean;
	char	   *ret;

	/* 只允许应用程序名称中包含干净的 ASCII 字符 */
	clean = pg_clean_ascii(*newval, MCXT_ALLOC_NO_OOM);
	if (!clean)
		return false;

	ret = guc_strdup(WARNING, clean);
	if (!ret)
	{
		pfree(clean);
		return false;
	}

	guc_free(*newval);

	pfree(clean);
	*newval = ret;
	return true;
}

/*
 * application_name 的 GUC assign_hook
 */
void
assign_application_name(const char *newval, void *extra)
{
	/* 更新 pg_stat_activity 视图 */
	pgstat_report_appname(newval);
}

/*
 * cluster_name 的 GUC check_hook
 */
bool
check_cluster_name(char **newval, void **extra, GucSource source)
{
	char	   *clean;
	char	   *ret;

	/* 只允许集簇名称中包含干净的 ASCII 字符 */
	clean = pg_clean_ascii(*newval, MCXT_ALLOC_NO_OOM);
	if (!clean)
		return false;

	ret = guc_strdup(WARNING, clean);
	if (!ret)
	{
		pfree(clean);
		return false;
	}

	guc_free(*newval);

	pfree(clean);
	*newval = ret;
	return true;
}

/*
 * maintenance_io_concurrency 的 GUC assign_hook
 */
void
assign_maintenance_io_concurrency(int newval, void *extra)
{
#ifdef USE_PREFETCH
	/*
	 * 重新配置恢复预取，因为其依赖的设置
	 * 发生了更改。
	 */
	maintenance_io_concurrency = newval;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
#endif
}


/*
 * 这些显示钩子存在只是因为要以八进制显示这些值。   
 */

/*
 * data_directory_mode 的 GUC show_hook
 */
const char *
show_data_directory_mode(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", data_directory_mode);
	return buf;
}

/*
 * log_file_mode 的 GUC show_hook
 */
const char *
show_log_file_mode(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", Log_file_mode);
	return buf;
}

/*
 * unix_socket_permissions 的 GUC show_hook
 */
const char *
show_unix_socket_permissions(void)
{
	static char buf[12];

	snprintf(buf, sizeof(buf), "%04o", Unix_socket_permissions);
	return buf;
}


/*
 * 这些检查钩子只是在不支持它们的构建中            
 * 拒绝非默认设置而已。    
 */

bool
check_bonjour(bool *newval, void **extra, GucSource source)
{
#ifndef USE_BONJOUR
	if (*newval)
	{
		GUC_check_errmsg("Bonjour is not supported by this build");
		return false;
	}
#endif
	return true;
}

bool
check_default_with_oids(bool *newval, void **extra, GucSource source)
{
	if (*newval)
	{
		/* 检查 GUC 的定义以获取解释 */
		GUC_check_errcode(ERRCODE_FEATURE_NOT_SUPPORTED);
		GUC_check_errmsg("tables declared WITH OIDS are not supported");

		return false;
	}

	return true;
}

bool
check_effective_io_concurrency(int *newval, void **extra, GucSource source)
{
#ifndef USE_PREFETCH
	if (*newval != 0)
	{
		GUC_check_errdetail("effective_io_concurrency must be set to 0 on platforms that lack posix_fadvise().");
		return false;
	}
#endif							/* USE_PREFETCH */
	return true;
}

bool
check_maintenance_io_concurrency(int *newval, void **extra, GucSource source)
{
#ifndef USE_PREFETCH
	if (*newval != 0)
	{
		GUC_check_errdetail("maintenance_io_concurrency must be set to 0 on platforms that lack posix_fadvise().");
		return false;
	}
#endif							/* USE_PREFETCH */
	return true;
}

bool
check_ssl(bool *newval, void **extra, GucSource source)
{
#ifndef USE_SSL
	if (*newval)
	{
		GUC_check_errmsg("SSL is not supported by this build");
		return false;
	}
#endif
	return true;
}
