/*-----------------------------------------------------------------------
 *
 * PostgreSQL 区域设置
 *
 * Portions Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale.c
 *
 *-----------------------------------------------------------------------
 */

/*----------
 * 区域设置的处理方式如下：LC_COLLATE 与 LC_CTYPE
 * 在 CREATE DATABASE 时固定并存于 pg_database，不可更改。
 * 因此，strcoll()、strxfrm()、isupper()、
 * toupper() 等总在同一固定区域设置生效。
 *
 * LC_MESSAGES 可在运行时设置，并将立即
 * 生效。
 *
 * 其他类别 LC_MONETARY、LC_NUMERIC 与 LC_TIME 也可在
 * 运行时设置。但我们实际上并不永久设置这些区域
 * 设置类别。因为这会导致奇怪的效果，例如
 * 某些区域设置不再接受标准浮点文字。
 * 相反，只在需要时短暂设置这些类别，
 * 缓存 localeconv() 或 strftime() 取得的信息，
 * 然后把区域设置类别设回 "C"。
 * 缓存的信息仅供格式化函数（to_char 等）与货币
 * 类型使用。对用户而言，这一切都应当是透明的。
 * transparent.
 *
 * !!! 听好了 !!!
 *
 * 我们已被这个 bug 反复坑过，所以今后要把它牢
 * 记在心间：在某些平台上，区域设置函数返回的
 * 是指向静态数据的指针，会被之后任何区域设置
 * 函数覆盖。例如看似显然的序列
 *			save = setlocale(category, NULL);
 *			if (!setlocale(category, value))
 *				fail = true;
 *			setlocale(category, save);
 * 并不能可靠地工作：在某些平台上第二次
 * setlocale() 调用会改变 save 所指向的内存。安全
 * 做法：你 *必须* 对 setlocale 首次返回值 pstrdup
 *
 * POSIX 区域设置标准见此：
 *
 *	http://www.opengroup.org/onlinepubs/009695399/basedefs/xbd_chap07.html
 *----------
 */


#include "postgres.h"

#include <time.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_control.h"
#include "common/string.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/formatting.h"
#include "utils/guc_hooks.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

#ifdef USE_ICU
#include <unicode/ucnv.h>
#include <unicode/ustring.h>
#endif

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifdef WIN32
#include <shlwapi.h>
#endif

/* 区域设置敏感子例程触发的错误 */
#define		PGLOCALE_SUPPORT_ERROR(provider) \
	elog(ERROR, "unsupported collprovider for %s: %c", __func__, provider)

/*
 * 它应当足够大以容纳大多数字符串，但又足够小，
 * 让我们可以放心地把它放在栈上
 */
#define		TEXTBUFLEN			1024

#define		MAX_L10N_DATA		80


/* GUC 设置 */
char	   *locale_messages;
char	   *locale_monetary;
char	   *locale_numeric;
char	   *locale_time;

int			icu_validation_level = WARNING;

/*
 * lc_time 本地化缓存。
 *
 * 我们只用这些数组的前 7 或 12 项。末尾数组
 * 元素留为 NULL，以便外部代码可以顺序扫描这些
 * 数组。
 */
char	   *localized_abbrev_days[7 + 1];
char	   *localized_full_days[7 + 1];
char	   *localized_abbrev_months[12 + 1];
char	   *localized_full_months[12 + 1];

/* 数据库 LC_CTYPE 是否为 C locale？ */
bool		database_ctype_is_c = false;

/* 指示区域设置信息缓存是否有效 */
static bool CurrentLocaleConvValid = false;
static bool CurrentLCTimeValid = false;

/* 排序规则相关知识的缓存 */

typedef struct
{
	Oid			collid;			/* 哈希键:pg_collation OID */
	bool		collate_is_c;	/* LC_COLLATE 是否为 C？ */
	bool		ctype_is_c;		/* LC_CTYPE 是否为 C？ */
	bool		flags_valid;	/* 以上标志是否有效 */
	pg_locale_t locale;			/* locale_t 结构，无效时为 0 */
} collation_cache_entry;

static HTAB *collation_cache = NULL;


#if defined(WIN32) && defined(LC_MESSAGES)
static char *IsoLocaleName(const char *);
#endif

#ifdef USE_ICU
/*
 * 用于在 ICU 的 UChar 字符串与数据库编码的 C 字符串
 * 之间转换的转换器对象。数据库编码不会变化，
 * 每个会话只需一个。
 */
static UConverter *icu_converter = NULL;

static UCollator *pg_ucol_open(const char *loc_str);
static void init_icu_converter(void);
static size_t uchar_length(UConverter *converter,
						   const char *str, int32_t len);
static int32_t uchar_convert(UConverter *converter,
							 UChar *dest, int32_t destlen,
							 const char *src, int32_t srclen);
static void icu_set_collation_attributes(UCollator *collator, const char *loc,
										 UErrorCode *status);
#endif

/*
 * pg_perm_setlocale
 *
 * 这是 libc 函数 setlocale() 的封装，有两点补充。其一，
 * 当更改 LC_CTYPE 时，更新当前消息域的 gettext 编码。
 * GNU gettext 在大多数平台上会自动跟踪 LC_CTYPE，但
 * Windows 上则不会。第二，如果操作成功，相应的
 * LC_XXX 环境变量会被设为匹配的值。通过设置环境
 * 变量，我们确保之后任何对 setlocale(..., "") 的使用
 * 都会保留通过本例程所做的设置。当然，LC_ALL 也
 * 必须被取消设置才能完全确保这一点，但这必须在
 * 所有单个 LC_XXX 变量都正确设置之后在其他地方完
 * 成（拜 Perl 所赐。）
 */
char *
pg_perm_setlocale(int category, const char *locale)
{
	char	   *result;
	const char *envvar;

#ifndef WIN32
	result = setlocale(category, locale);
#else

	/*
	 * 在 Windows 上，setlocale(LC_MESSAGES) 不生效，所以只
	 * 需假定给定值是好的，并将其设置到环境变量中。
	 * 必须忽略设为 "" 的尝试，那意味着 "继续使用旧
	 * 的环境值"。
	 */
#ifdef LC_MESSAGES
	if (category == LC_MESSAGES)
	{
		result = (char *) locale;
		if (locale == NULL || locale[0] == '\0')
			return result;
	}
	else
#endif
		result = setlocale(category, locale);
#endif							/* WIN32 */

	if (result == NULL)
		return result;			/* 失败时立即跳出 */

	/*
	 * 在翻译消息中使用正确的编码。在 ENABLE_NLS 下，让
	 * pg_bind_textdomain_codeset() 自行判断。在 !ENABLE_NLS
	 * 下，消息格式字符串都是 ASCII，但数据库编码的字
	 * 符串可能经由 %s 进入消息。这使整体消息编码等于
	 * 数据库编码。
	 */
	if (category == LC_CTYPE)
	{
		static char save_lc_ctype[LOCALE_NAME_BUFLEN];

		/* 在被调方再次调用前复制 setlocale() 返回值 */
		strlcpy(save_lc_ctype, result, sizeof(save_lc_ctype));
		result = save_lc_ctype;

#ifdef ENABLE_NLS
		SetMessageEncoding(pg_bind_textdomain_codeset(textdomain(NULL)));
#else
		SetMessageEncoding(GetDatabaseEncoding());
#endif
	}

	switch (category)
	{
		case LC_COLLATE:
			envvar = "LC_COLLATE";
			break;
		case LC_CTYPE:
			envvar = "LC_CTYPE";
			break;
#ifdef LC_MESSAGES
		case LC_MESSAGES:
			envvar = "LC_MESSAGES";
#ifdef WIN32
			result = IsoLocaleName(locale);
			if (result == NULL)
				result = (char *) locale;
			elog(DEBUG3, "IsoLocaleName() executed; locale: \"%s\"", result);
#endif							/* WIN32 */
			break;
#endif							/* LC_MESSAGES */
		case LC_MONETARY:
			envvar = "LC_MONETARY";
			break;
		case LC_NUMERIC:
			envvar = "LC_NUMERIC";
			break;
		case LC_TIME:
			envvar = "LC_TIME";
			break;
		default:
			elog(FATAL, "unrecognized LC category: %d", category);
			return NULL;		/* 让编译器安静 */
	}

	if (setenv(envvar, result, 1) != 0)
		return NULL;

	return result;
}


/*
 * 区域设置名对该类别是否有效？
 *
 * 如果成功且 canonname 非 NULL，则该区域设置规范名
 * 的一份 palloc 副本将存于其中。这对弄清区域设置名
 * "" 的含义特别有用（即服务器环境值）。（实际上，
 * 在大多数实现上这似乎是它唯一有用的地方；我们
 * 可能希望 setlocale 返回规范拼写的区域设置名称，
 * 但通常它并不这么做。）
 */
bool
check_locale(int category, const char *locale, char **canonname)
{
	char	   *save;
	char	   *res;

	/* 不放行 Windows 非 ASCII 区域设置名 */
	if (!pg_is_ascii(locale))
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("locale name \"%s\" contains non-ASCII characters",
						locale)));
		return false;
	}

	if (canonname)
		*canonname = NULL;		/* 以防失败 */

	save = setlocale(category, NULL);
	if (!save)
		return false;			/* 但愿不会发生 */

	/* save 可能指向一个可修改的暂存变量，见上文。 */
	save = pstrdup(save);

	/* 用 setlocale 设置区域设置，看是否接受 */
	res = setlocale(category, locale);

	/* 如要求则保存规范名 */
	if (res && canonname)
		*canonname = pstrdup(res);

	/* 恢复旧值 */
	if (!setlocale(category, save))
		elog(WARNING, "failed to restore old locale \"%s\"", save);
	pfree(save);

	/* 不放出 Windows 非 ASCII 区域设置名。 */
	if (canonname && *canonname && !pg_is_ascii(*canonname))
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("locale name \"%s\" contains non-ASCII characters",
						*canonname)));
		pfree(*canonname);
		*canonname = NULL;
		return false;
	}

	return (res != NULL);
}


/*
 * GUC 检查赋值钩子
 *
 * 对大多数区域设置类别而言，赋值钩子实际上并不
 * 永久设置区域设置，只是重置标志，以便下次
 * 使用时缓存适当的值。（见文件顶部说明。）
 *
 * 注意：我们接受 value = "" 作为选择 postmaster 的
 * 环境值，无论它是什么（只要环境设置合法）。
 * 这应当已被先前对 pg_perm_setlocale 的调用锁定。
 */
bool
check_locale_monetary(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_MONETARY, *newval, NULL);
}

void
assign_locale_monetary(const char *newval, void *extra)
{
	CurrentLocaleConvValid = false;
}

bool
check_locale_numeric(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_NUMERIC, *newval, NULL);
}

void
assign_locale_numeric(const char *newval, void *extra)
{
	CurrentLocaleConvValid = false;
}

bool
check_locale_time(char **newval, void **extra, GucSource source)
{
	return check_locale(LC_TIME, *newval, NULL);
}

void
assign_locale_time(const char *newval, void *extra)
{
	CurrentLCTimeValid = false;
}

/*
 * 我们允许 LC_MESSAGES 真正被全局设置。
 *
 * 注意：我们通常拒绝 value = ""，因为它的语义会不
 * 一致（它实际上只会沿用先前的值）。但是，这是
 * PGC_S_DEFAULT 传入的值，所以那种情况下不要抱
 * 怨，即使尝试的设置因环境值非法而失败也不要。
 * 那里的想法只是在启动期间 *尽可能* 接受环境设
 * 置，直到我们能从 postgresql.conf 读到正确的值。
 */
bool
check_locale_messages(char **newval, void **extra, GucSource source)
{
	if (**newval == '\0')
	{
		if (source == PGC_S_DEFAULT)
			return true;
		else
			return false;
	}

	/*
	 * LC_MESSAGES 类别并非处处存在，但还是接受它
	 *
	 * Windows 上甚至无法检查该值，只能盲目接受
	 */
#if defined(LC_MESSAGES) && !defined(WIN32)
	return check_locale(LC_MESSAGES, *newval, NULL);
#else
	return true;
#endif
}

void
assign_locale_messages(const char *newval, void *extra)
{
	/*
	 * LC_MESSAGES 类别并非处处存在，但还是接受它。
	 * 失败则忽略，理由见上。
	 */
#ifdef LC_MESSAGES
	(void) pg_perm_setlocale(LC_MESSAGES, newval);
#endif
}


/*
 * 释放 struct lconv 中 malloc 的内容。（但不含该结构
 * 本身。）重要的是它不得抛出 elog(ERROR)。
 */
static void
free_struct_lconv(struct lconv *s)
{
	free(s->decimal_point);
	free(s->thousands_sep);
	free(s->grouping);
	free(s->int_curr_symbol);
	free(s->currency_symbol);
	free(s->mon_decimal_point);
	free(s->mon_thousands_sep);
	free(s->mon_grouping);
	free(s->positive_sign);
	free(s->negative_sign);
}

/*
 * 检查 struct lconv 的所有字段（至少是我们关心的
 * 那些）非 NULL。字段列表须与 free_struct_lconv() 一致
 */
static bool
struct_lconv_is_valid(struct lconv *s)
{
	if (s->decimal_point == NULL)
		return false;
	if (s->thousands_sep == NULL)
		return false;
	if (s->grouping == NULL)
		return false;
	if (s->int_curr_symbol == NULL)
		return false;
	if (s->currency_symbol == NULL)
		return false;
	if (s->mon_decimal_point == NULL)
		return false;
	if (s->mon_thousands_sep == NULL)
		return false;
	if (s->mon_grouping == NULL)
		return false;
	if (s->positive_sign == NULL)
		return false;
	if (s->negative_sign == NULL)
		return false;
	return true;
}


/*
 * 将 *str 处以指定编码保存的 strdup 字符串转换为
 * 数据库编码。
 */
static void
db_encoding_convert(int encoding, char **str)
{
	char	   *pstr;
	char	   *mstr;

	/* 把字符串转换为数据库编码 */
	pstr = pg_any_to_server(*str, strlen(*str), encoding);
	if (pstr == *str)
		return;					/* 未发生转换 */

	/* 需要 malloc 而非 palloc */
	mstr = strdup(pstr);
	if (mstr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* 替换旧字符串 */
	free(*str);
	*str = mstr;

	pfree(pstr);
}


/*
 * 返回 POSIX lconv 结构（含数字/货币格式信息），
 * 并带有所有类别的区域设置信息。
 */
struct lconv *
PGLC_localeconv(void)
{
	static struct lconv CurrentLocaleConv;
	static bool CurrentLocaleConvAllocated = false;
	struct lconv *extlconv;
	struct lconv worklconv;
	char	   *save_lc_monetary;
	char	   *save_lc_numeric;
#ifdef WIN32
	char	   *save_lc_ctype;
#endif

	/* 已做过吗？ */
	if (CurrentLocaleConvValid)
		return &CurrentLocaleConv;

	/* 释放任何已分配的存储 */
	if (CurrentLocaleConvAllocated)
	{
		free_struct_lconv(&CurrentLocaleConv);
		CurrentLocaleConvAllocated = false;
	}

	/*
	 * 这很棘手，因为我们实在不想冒在区域设置处于非
	 * 常规设置时抛出错误的风险。因此，流程是：收集
	 * 常规设置，将区域设为特殊值，用 strdup() 把相
	 * 关数据复制到 worklconv，恢复常规设置，转换数据
	 * 为所需编码，最后把收集到的数据存入
	 * CurrentLocaleConv。这样即使在编码转换期间抛错
	 * 或过程中任何地方内存耗尽也是安全的。struct lconv
	 * 成员指向的数据都用 strdup 分配，以免过早的
	 * elog(ERROR)，并允许使用
	 * 单一清理例程。
	 */
	memset(&worklconv, 0, sizeof(worklconv));

	/* 保存货币与数字区域设置的现行值 */
	save_lc_monetary = setlocale(LC_MONETARY, NULL);
	if (!save_lc_monetary)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_monetary = pstrdup(save_lc_monetary);

	save_lc_numeric = setlocale(LC_NUMERIC, NULL);
	if (!save_lc_numeric)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_numeric = pstrdup(save_lc_numeric);

#ifdef WIN32

	/*
	 * POSIX 标准明确指出，如果 LC_MONETARY 或 LC_NUMERIC
	 * 暗示的编码（codeset）不同于 LC_CTYPE 的情况，
	 * 其行为是未定义的。实践中，所有类 Unix 平台似乎
	 * 都相信 localeconv() 应返回按 LC_MONETARY 或
	 * LC_NUMERIC 区域设置名所暗示编码的字符串。因此，
	 * 一旦我们成功收集到 localeconv() 的结果，就会把它
	 * 从该编码转换为所需的服务器编码。
	 *
	 * Windows 则坚持自行其是；在该平台上，
	 * LC_CTYPE 必须与 LC_MONETARY/LC_NUMERIC 匹配才能得
	 * 到合理结果。因此必须临时设置该类别。
	 */

	/* 保存 ctype 区域设置现行值 */
	save_lc_ctype = setlocale(LC_CTYPE, NULL);
	if (!save_lc_ctype)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_ctype = pstrdup(save_lc_ctype);

	/* 从这里开始是不得抛错的关键区段 */

	/* 用 numeric 来设置 ctype */
	setlocale(LC_CTYPE, locale_numeric);
#endif

	/* 获取 numeric 的格式化信息 */
	setlocale(LC_NUMERIC, locale_numeric);
	extlconv = localeconv();

	/* 现在须复制数据，以防 setlocale() 覆盖 */
	worklconv.decimal_point = strdup(extlconv->decimal_point);
	worklconv.thousands_sep = strdup(extlconv->thousands_sep);
	worklconv.grouping = strdup(extlconv->grouping);

#ifdef WIN32
	/* 用 monetary 来设置 ctype */
	setlocale(LC_CTYPE, locale_monetary);
#endif

	/* 获取 monetary 格式化信息 */
	setlocale(LC_MONETARY, locale_monetary);
	extlconv = localeconv();

	/* 现在须复制数据，以防 setlocale() 覆盖 */
	worklconv.int_curr_symbol = strdup(extlconv->int_curr_symbol);
	worklconv.currency_symbol = strdup(extlconv->currency_symbol);
	worklconv.mon_decimal_point = strdup(extlconv->mon_decimal_point);
	worklconv.mon_thousands_sep = strdup(extlconv->mon_thousands_sep);
	worklconv.mon_grouping = strdup(extlconv->mon_grouping);
	worklconv.positive_sign = strdup(extlconv->positive_sign);
	worklconv.negative_sign = strdup(extlconv->negative_sign);
	/* 同时复制标量字段 */
	worklconv.int_frac_digits = extlconv->int_frac_digits;
	worklconv.frac_digits = extlconv->frac_digits;
	worklconv.p_cs_precedes = extlconv->p_cs_precedes;
	worklconv.p_sep_by_space = extlconv->p_sep_by_space;
	worklconv.n_cs_precedes = extlconv->n_cs_precedes;
	worklconv.n_sep_by_space = extlconv->n_sep_by_space;
	worklconv.p_sign_posn = extlconv->p_sign_posn;
	worklconv.n_sign_posn = extlconv->n_sign_posn;

	/*
	 * 恢复现行区域设置；恢复失败是致命的。
	 * 也许我们可以在非默认 LC_MONETARY 或 LC_NUMERIC 下
	 * 勉强维持，但带着错误的 LC_CTYPE 值继续下去肯定
	 * 是坏消息；而且考虑到现行的 LC_MONETARY 与
	 * LC_NUMERIC 几乎可以肯定是 "C"，实在没有理由让恢复
	 * 失败。
	 */
#ifdef WIN32
	if (!setlocale(LC_CTYPE, save_lc_ctype))
		elog(FATAL, "failed to restore LC_CTYPE to \"%s\"", save_lc_ctype);
#endif
	if (!setlocale(LC_MONETARY, save_lc_monetary))
		elog(FATAL, "failed to restore LC_MONETARY to \"%s\"", save_lc_monetary);
	if (!setlocale(LC_NUMERIC, save_lc_numeric))
		elog(FATAL, "failed to restore LC_NUMERIC to \"%s\"", save_lc_numeric);

	/*
	 * 至此我们已尽力清理，可以问心无愧地调用可能
	 * 抛错的函数了。但让我们确保不泄漏 worklconv 中
	 * 任何已 strdup 的字段。
	 */
	PG_TRY();
	{
		int			encoding;

		/* 释放 pstrdup 区域设置名 */
		pfree(save_lc_monetary);
		pfree(save_lc_numeric);
#ifdef WIN32
		pfree(save_lc_ctype);
#endif

		/* 若前面任何 strdup 调用失败，现在报错。 */
		if (!struct_lconv_is_valid(&worklconv))
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		/*
		 * 现在我们必须把与区域设置相关联的编码转换为
		 * 数据库编码。如果我们无法识别 LC_NUMERIC 或
		 * LC_MONETARY 暗示的编码（即得到 -1），则使用
		 * PG_SQL_ASCII，这样只会验证字符串在
		 * 数据库编码下是否正常。
		 */
		encoding = pg_get_encoding_from_locale(locale_numeric, true);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;

		db_encoding_convert(encoding, &worklconv.decimal_point);
		db_encoding_convert(encoding, &worklconv.thousands_sep);
		/* grouping 不是文本，无需转换 */

		encoding = pg_get_encoding_from_locale(locale_monetary, true);
		if (encoding < 0)
			encoding = PG_SQL_ASCII;

		db_encoding_convert(encoding, &worklconv.int_curr_symbol);
		db_encoding_convert(encoding, &worklconv.currency_symbol);
		db_encoding_convert(encoding, &worklconv.mon_decimal_point);
		db_encoding_convert(encoding, &worklconv.mon_thousands_sep);
		/* mon_grouping 不是文本，无需转换 */
		db_encoding_convert(encoding, &worklconv.positive_sign);
		db_encoding_convert(encoding, &worklconv.negative_sign);
	}
	PG_CATCH();
	{
		free_struct_lconv(&worklconv);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * 一切正常，保存结果。
	 */
	CurrentLocaleConv = worklconv;
	CurrentLocaleConvAllocated = true;
	CurrentLocaleConvValid = true;
	return &CurrentLocaleConv;
}

#ifdef WIN32
/*
 * 在 Windows 上，strftime() 返回以 CP_ACP 编码（计算机
 * 默认操作系统代码页）的输出，这很可能与
 * SERVER_ENCODING 不同。这在日语版 Windows 上尤其
 * 重要，因其使用 SJIS 编码，而这并非我们
 * 支持的。
 *
 * 所以，我们不使用 strftime()，而是用 wcsftime() 以宽
 * 字符（内部为 UTF16）返回值，再转换为 UTF8，这一
 * 点我们可直接处理。
 *
 * 注意这只影响本文件中对 strftime() 的调用，这些调
 * 用用于获取区域设置感知字符串。后端的其他部
 * 分使用 pg_strftime()，它不感知区域设置，无需替换。
 */
static size_t
strftime_win32(char *dst, size_t dstlen,
			   const char *format, const struct tm *tm)
{
	size_t		len;
	wchar_t		wformat[8];		/* 下方格式需要 3 字符 */
	wchar_t		wbuf[MAX_L10N_DATA];

	/*
	 * 获取格式串的 wchar_t 版本。实际只使用
	 * 纯 ASCII 格式，因此可以说它们是 UTF8。
	 */
	len = MultiByteToWideChar(CP_UTF8, 0, format, -1,
							  wformat, lengthof(wformat));
	if (len == 0)
		elog(ERROR, "could not convert format string from UTF-8: error code %lu",
			 GetLastError());

	len = wcsftime(wbuf, MAX_L10N_DATA, wformat, tm);
	if (len == 0)
	{
		/*
		 * wcsftime 失败，可能因结果装不下 MAX_L10N_DATA。
		 * 返回 0 且 dst 内容未指定。
		 */
		return 0;
	}

	len = WideCharToMultiByte(CP_UTF8, 0, wbuf, len, dst, dstlen - 1,
							  NULL, NULL);
	if (len == 0)
		elog(ERROR, "could not convert string to UTF-8: error code %lu",
			 GetLastError());

	dst[len] = '\0';

	return len;
}

/*重定义 strftime()*/
#define strftime(a,b,c,d) strftime_win32(a,b,c,d)
#endif							/* WIN32 */

/*
 * cache_locale_time() 子例程。
 * 把给定串从 "encoding" 编码转换到数据库编码，
 * 并把结果存到 *dst，替换任何先前值。
 */
static void
cache_single_string(char **dst, const char *src, int encoding)
{
	char	   *ptr;
	char	   *olddst;

	/* 把字符串转换为数据库编码，或验证其有效 */
	ptr = pg_any_to_server(src, strlen(src), encoding);

	/* 把字符串存入长效存储，替换任何先前值 */
	olddst = *dst;
	*dst = MemoryContextStrdup(TopMemoryContext, ptr);
	if (olddst)
		pfree(olddst);

	/* 不妨也清理任何 palloc 的转换结果 */
	if (ptr != src)
		pfree(ptr);
}

/*
 * 如需要，更新 lc_time 本地化缓存变量。
 */
void
cache_locale_time(void)
{
	char		buf[(2 * 7 + 2 * 12) * MAX_L10N_DATA];
	char	   *bufptr;
	time_t		timenow;
	struct tm  *timeinfo;
	bool		strftimefail = false;
	int			encoding;
	int			i;
	char	   *save_lc_time;
#ifdef WIN32
	char	   *save_lc_ctype;
#endif

	/* 已做过此事吗？ */
	if (CurrentLCTimeValid)
		return;

	elog(DEBUG3, "cache_locale_time() executed; locale: \"%s\"", locale_time);

	/*
	 * 与 PGLC_localeconv() 一样，libc 区域设置为非默认
	 * 值时绝不能抛错。因此，我们只在关键区段内调用
	 * strftime()，然后再转换并保存
	 * 其结果。
	 */

	/* 保存 time 区域设置现行值 */
	save_lc_time = setlocale(LC_TIME, NULL);
	if (!save_lc_time)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_time = pstrdup(save_lc_time);

#ifdef WIN32

	/*
	 * 在 Windows 上，wcsftime() 内部似乎会使用 LC_CTYPE，
	 * 所以必须在这里设置它。这段代码看上去与
	 * PGLC_localeconv() 所做的相同，但根本原因不同：这并
	 * 不决定 strftime_win32() 将返回的编码。
	 */

	/* 保存 ctype 区域设置现行值 */
	save_lc_ctype = setlocale(LC_CTYPE, NULL);
	if (!save_lc_ctype)
		elog(ERROR, "setlocale(NULL) failed");
	save_lc_ctype = pstrdup(save_lc_ctype);

	/* 用 lc_time 来设置 ctype */
	setlocale(LC_CTYPE, locale_time);
#endif

	setlocale(LC_TIME, locale_time);

	/* 用接近当前的时间作为 strftime() 数据 */
	timenow = time(NULL);
	timeinfo = localtime(&timenow);

	/* 把 strftime 结果存入 buf[] 中 MAX_L10N_DATA 大小的片段 */
	bufptr = buf;

	/*
	 * MAX_L10N_DATA 对于每个已知区域设置都是足够的缓冲
	 * 空间，且 POSIX 未定义任何 strftime() 错误。（缓冲空间
	 * 耗尽不算错误。）实现可能返回 0（或不太可
	 * 能的负值）并设 errno 来报告错误。万一真如此，
	 * 做了，就报告 errno，但要提前清除它，这样我们就
	 * 不会发出陈旧且无关的 errno。
	 */
	errno = 0;

	/* 本地化日 */
	for (i = 0; i < 7; i++)
	{
		timeinfo->tm_wday = i;
		if (strftime(bufptr, MAX_L10N_DATA, "%a", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
		if (strftime(bufptr, MAX_L10N_DATA, "%A", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
	}

	/* 本地化月份 */
	for (i = 0; i < 12; i++)
	{
		timeinfo->tm_mon = i;
		timeinfo->tm_mday = 1;	/* 确保没有无效日期 */
		if (strftime(bufptr, MAX_L10N_DATA, "%b", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
		if (strftime(bufptr, MAX_L10N_DATA, "%B", timeinfo) <= 0)
			strftimefail = true;
		bufptr += MAX_L10N_DATA;
	}

	/*
	 * 恢复现行区域设置；与 PGLC_localeconv() 中一样，
	 * 失败是致命的。
	 */
#ifdef WIN32
	if (!setlocale(LC_CTYPE, save_lc_ctype))
		elog(FATAL, "failed to restore LC_CTYPE to \"%s\"", save_lc_ctype);
#endif
	if (!setlocale(LC_TIME, save_lc_time))
		elog(FATAL, "failed to restore LC_TIME to \"%s\"", save_lc_time);

	/*
	 * 至此我们已尽力清理，可以问心无愧地抛错，或调
	 * 用可能抛错的函数。
	 */
	if (strftimefail)
		elog(ERROR, "strftime() failed: %m");

	/* 释放 pstrdup 区域设置名 */
	pfree(save_lc_time);
#ifdef WIN32
	pfree(save_lc_ctype);
#endif

#ifndef WIN32

	/*
	 * 与 PGLC_localeconv() 一样，必须把 strftime() 输出从
	 * LC_TIME 暗示的编码转换为数据库编码。如果我们无
	 * 法识别 LC_TIME 编码，就只做编码验证。
	 */
	encoding = pg_get_encoding_from_locale(locale_time, true);
	if (encoding < 0)
		encoding = PG_SQL_ASCII;

#else

	/*
	 * 在 Windows 上，strftime_win32() 总是返回 UTF8 数据，
	 * 必要时转换。
	 */
	encoding = PG_UTF8;

#endif							/* WIN32 */

	bufptr = buf;

	/* 本地化日 */
	for (i = 0; i < 7; i++)
	{
		cache_single_string(&localized_abbrev_days[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
		cache_single_string(&localized_full_days[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
	}
	localized_abbrev_days[7] = NULL;
	localized_full_days[7] = NULL;

	/* 本地化月份 */
	for (i = 0; i < 12; i++)
	{
		cache_single_string(&localized_abbrev_months[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
		cache_single_string(&localized_full_months[i], bufptr, encoding);
		bufptr += MAX_L10N_DATA;
	}
	localized_abbrev_months[12] = NULL;
	localized_full_months[12] = NULL;

	CurrentLCTimeValid = true;
}


#if defined(WIN32) && defined(LC_MESSAGES)
/*
 * 将 Windows setlocale() 参数转为 Unix 风格。
 *
 * 无论平台如何，我们都以 Unix 风格的
 * LL[_CC][.ENCODING][@VARIANT] 命名约定安装消息目录。
 * 该风格 LC_MESSAGES 设置方能引出本地化接口串
 *
 * 在 Visual Studio 2012（msvcr110.dll）前，Windows 的
 * setlocale() 接受 "C"（但不接受 "c"）以及形如
 * <Language>[_<Country>][.<CodePage>] 的字符串，不区分
 * 大小写。setlocale() 返回完全限定的形式；例如，
 * setlocale("thaI") 返回 "Thai_Thailand.874"。在内部，
 * setlocale() 与 _create_locale() 会选择一个"区域设置标识符"[1]
 * 并把它存入未公开文档的 _locale_t 字段。从该 LCID，
 * 符编码无关紧要，由两端编码决定。
 *
 * Windows Vista 引入了"区域设置名称"概念[2]，紧密遵循
 * RFC 4646。区域设置标识符现已弃用。从 Visual
 * Studio 2012 起，setlocale() 除历史接受的字符串外还接
 * 受区域设置名称。它不将其标准化；setlocale("Th-tH")
 * 返回 "Th-tH"。setlocale(category, "") 仍返回
 * 传统的字符串。此外，msvcr110.dll 把未公开文档的
 * _locale_t 内容改为携带名称而非标识符。
 *
 * Visual Studio 2015 应仍能做到与 Visual Studio 2012 相
 * 同的事情，但 _locale_t 中缺少 locale_name 声明，导致
 * 这段代码编译失败，因此这里转而回退到
 * 用 EnumSystemLocalesEx 枚举所有系统区域设置，以找出
 * 所需的区域设置名称。如果输入参数是 Unix 风格，则
 * 可以用 GetLocaleInfoEx() 以 LCType 直接取得 ISO 区域
 * 设置名。
 *
 * MinGW 头文件声明了 _create_locale()，但 Windows 8 之前
 * 的发布版 msvcrt.dll 缺少该符号。IsoLocaleName() 在
 * MinGW 构建的 postgres.exe 中总是失败，所以只有 Unix
 * 风格的 lc_messages GUC 值才能引出本地化消息。特别
 * 是 initdb 能自动选择的每个 lc_messages 设置都只会得
 * 到 C 区域设置的消息。XXX 可让完全限定名查表解决。
 *
 * 本函数返回指向静态缓冲区的指针，其中承载转换后
 * 的名称，否则为 NULL。
 *
 * [1] https://docs.microsoft.com/en-us/windows/win32/intl/locale-identifiers
 * [2] https://docs.microsoft.com/en-us/windows/win32/intl/locale-names
 */

#if defined(_MSC_VER)

/*
 * 供 get_iso_localename() 中 EnumSystemLocalesEx() 使用的
 *
 * 回调函数。本函数枚举所有系统区域设置，搜索与以
 * <Language>[_<Country>] 格式输入的匹配项，如
 * English[_United States]
 *
 * 输入是一个作为 LPARAM 传入的三元素 wchar_t 数组。第
 * 一个元素是我们想匹配的 locale_name，第二个元素是
 * 一块已分配的缓冲区，匹配找到时会把 Unix 风格区域
 * 设置复制进去，第三个是搜索状态（1=找到）。
 */
static BOOL CALLBACK
search_locale_enum(LPWSTR pStr, DWORD dwFlags, LPARAM lparam)
{
	wchar_t		test_locale[LOCALE_NAME_MAX_LENGTH];
	wchar_t   **argv;

	(void) (dwFlags);

	argv = (wchar_t **) lparam;
	*argv[2] = (wchar_t) 0;

	memset(test_locale, 0, sizeof(test_locale));

	/* 以英文获取 <Language> 的名称 */
	if (GetLocaleInfoEx(pStr, LOCALE_SENGLISHLANGUAGENAME,
						test_locale, LOCALE_NAME_MAX_LENGTH))
	{
		/*
		 * 若枚举到的区域设置无连字符（"en"），或
		 * locale_name 输入没有下划线（"English"），那么我们只
		 * 需比较 <Language> 标签。
		 */
		if (wcsrchr(pStr, '-') == NULL || wcsrchr(argv[0], '_') == NULL)
		{
			if (_wcsicmp(argv[0], test_locale) == 0)
			{
				wcscpy(argv[1], pStr);
				*argv[2] = (wchar_t) 1;
				return FALSE;
			}
		}

		/*
		 * 我们必须比较完整的 <Language>_<Country> 标签，因此
		 * 要追加下划线与国家/地区的英文名称，例如
		 * "English_United States".
		 */
		else
		{
			size_t		len;

			wcscat(test_locale, L"_");
			len = wcslen(test_locale);
			if (GetLocaleInfoEx(pStr, LOCALE_SENGLISHCOUNTRYNAME,
								test_locale + len,
								LOCALE_NAME_MAX_LENGTH - len))
			{
				if (_wcsicmp(argv[0], test_locale) == 0)
				{
					wcscpy(argv[1], pStr);
					*argv[2] = (wchar_t) 1;
					return FALSE;
				}
			}
		}
	}

	return TRUE;
}

/*
 * 本函数将 Windows 区域设置名称转换为 ISO 格式版本，
 * 供 Visual Studio 2015+ 用。
 *
 * 如果找不到有效转换则返回 NULL。
 */
static char *
get_iso_localename(const char *winlocname)
{
	wchar_t		wc_locale_name[LOCALE_NAME_MAX_LENGTH];
	wchar_t		buffer[LOCALE_NAME_MAX_LENGTH];
	static char iso_lc_messages[LOCALE_NAME_MAX_LENGTH];
	char	   *period;
	int			len;
	int			ret_val;

	/*
	 * 有效区域设置具有以下语法：
	 * <Language>[_<Country>[.<CodePage>]]
	 *
	 * GetLocaleInfoEx 只能接受不带代码页的区域设置名称，
	 * 且对该 API 而言代码页无关紧要。
	 */
	period = strchr(winlocname, '.');
	if (period != NULL)
		len = period - winlocname;
	else
		len = pg_mbstrlen(winlocname);

	memset(wc_locale_name, 0, sizeof(wc_locale_name));
	memset(buffer, 0, sizeof(buffer));
	MultiByteToWideChar(CP_ACP, 0, winlocname, len, wc_locale_name,
						LOCALE_NAME_MAX_LENGTH);

	/*
	 * 如果 lc_messages 已经是 Unix 风格字符串，我们会与
	 * LOCALE_SNAME 直接匹配，如 en-US。
	 */
	ret_val = GetLocaleInfoEx(wc_locale_name, LOCALE_SNAME, (LPWSTR) &buffer,
							  LOCALE_NAME_MAX_LENGTH);
	if (!ret_val)
	{
		/*
		 * 在系统中搜索语言与国家/地区名匹配的区域设置。
		 * name.
		 */
		wchar_t    *argv[3];

		argv[0] = wc_locale_name;
		argv[1] = buffer;
		argv[2] = (wchar_t *) &ret_val;
		EnumSystemLocalesEx(search_locale_enum, LOCALE_WINDOWS, (LPARAM) argv,
							NULL);
	}

	if (ret_val)
	{
		size_t		rc;
		char	   *hyphen;

		/* 区域设置名仅用 ASCII，任何转换编码皆可 */
		rc = wchar2char(iso_lc_messages, buffer, sizeof(iso_lc_messages), NULL);
		if (rc == -1 || rc == sizeof(iso_lc_messages))
			return NULL;

		/*
		 * 由于消息目录位于大小写不敏感的文件系统上，我
		 * 们无需在此规范字母大小写。只要我们不发布会
		 * 有影响目录，也无需翻译变体部分，如把
		 * uz-Cyrl-UZ 换成 uz_UZ@cyrillic。只需把连字符
		 * 替换为下划线即可。
		 */
		hyphen = strchr(iso_lc_messages, '-');
		if (hyphen)
			*hyphen = '_';
		return iso_lc_messages;
	}

	return NULL;
}

static char *
IsoLocaleName(const char *winlocname)
{
	static char iso_lc_messages[LOCALE_NAME_MAX_LENGTH];

	if (pg_strcasecmp("c", winlocname) == 0 ||
		pg_strcasecmp("posix", winlocname) == 0)
	{
		strcpy(iso_lc_messages, "C");
		return iso_lc_messages;
	}
	else
		return get_iso_localename(winlocname);
}

#else							/* !defined(_MSC_VER) */

static char *
IsoLocaleName(const char *winlocname)
{
	return NULL;				/* MinGW 不支持 */
}

#endif							/* defined(_MSC_VER) */

#endif							/* WIN32 && LC_MESSAGES */


/*
 * 排序规则信息的缓存机制。
 *
 * 我们缓存两个标志：排序规则的 LC_COLLATE 或 LC_CTYPE
 * 是否为 C/POSIX，以便在多处优化少量代码路径。
 * 对于内置的 C 与 POSIX 排序规则，我们甚至无需缓存
 * 查找就能知道这一点，但我们也想支持 C/POSIX 的别
 * 名。对"default"排序规则，另有单独的静态缓存变量，
 * 因为查询 pg_collation 目录并不能告诉我们所需信息。
 *
 * 此外，若某排序规则已请求 pg_locale_t，我们会将其
 * 缓存至后端结束。
 *
 * 注意有些代码依赖这些标志不报假阴性（即在确实
 * 为 C 时却说不为 C）。例如，若区域设置为 C，
 * char2wchar() 可能失败，所以 str_tolower() 不应
 * 调用它。
 *
 * 注意我们目前缺乏任何刷新缓存的手段。因为我们不
 * 支持 ALTER COLLATION，这没有问题。最坏的情况是
 * 有人删除了一个排序规则，而一个无用的缓存条目
 * 滞留。
 */

static collation_cache_entry *
lookup_collation_cache(Oid collation, bool set_flags)
{
	collation_cache_entry *cache_entry;
	bool		found;

	Assert(OidIsValid(collation));
	Assert(collation != DEFAULT_COLLATION_OID);

	if (collation_cache == NULL)
	{
		/* 首次进入时初始化哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(collation_cache_entry);
		collation_cache = hash_create("Collation cache", 100, &ctl,
									  HASH_ELEM | HASH_BLOBS);
	}

	cache_entry = hash_search(collation_cache, &collation, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * 确保缓存条目标记为无效，以防我们在设置
		 * 之前失败。
		 */
		cache_entry->flags_valid = false;
		cache_entry->locale = 0;
	}

	if (set_flags && !cache_entry->flags_valid)
	{
		/* 尝试设置标志 */
		HeapTuple	tp;
		Form_pg_collation collform;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collation);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		if (collform->collprovider == COLLPROVIDER_LIBC)
		{
			Datum		datum;
			const char *collcollate;
			const char *collctype;

			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collcollate);
			collcollate = TextDatumGetCString(datum);
			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collctype);
			collctype = TextDatumGetCString(datum);

			cache_entry->collate_is_c = ((strcmp(collcollate, "C") == 0) ||
										 (strcmp(collcollate, "POSIX") == 0));
			cache_entry->ctype_is_c = ((strcmp(collctype, "C") == 0) ||
									   (strcmp(collctype, "POSIX") == 0));
		}
		else
		{
			cache_entry->collate_is_c = false;
			cache_entry->ctype_is_c = false;
		}

		cache_entry->flags_valid = true;

		ReleaseSysCache(tp);
	}

	return cache_entry;
}


/*
 * 检测排序规则的 LC_COLLATE 属性是否为 C
 */
bool
lc_collate_is_c(Oid collation)
{
	/*
	 * 若被问及"排序规则 0"则返回假，让代码走入
	 * 非 C 路径并报告该排序规则是伪造的。
	 */
	if (!OidIsValid(collation))
		return false;

	/*
	 * 如果我们被问及默认排序规则，就必须向 C 库查
	 * 询。缓存结果，这样只需计算一次。
	 */
	if (collation == DEFAULT_COLLATION_OID)
	{
		static int	result = -1;
		char	   *localeptr;

		if (default_locale.provider == COLLPROVIDER_ICU)
			return false;

		if (result >= 0)
			return (bool) result;
		localeptr = setlocale(LC_COLLATE, NULL);
		if (!localeptr)
			elog(ERROR, "invalid LC_COLLATE setting");

		if (strcmp(localeptr, "C") == 0)
			result = true;
		else if (strcmp(localeptr, "POSIX") == 0)
			result = true;
		else
			result = false;
		return (bool) result;
	}

	/*
	 * 若被问及内置的 C/POSIX 排序规则，我们是知道的。
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * 否则我们必须查询 pg_collation，但会缓存之。
	 */
	return (lookup_collation_cache(collation, true))->collate_is_c;
}

/*
 * 检测排序规则的 LC_CTYPE 属性是否为 C
 */
bool
lc_ctype_is_c(Oid collation)
{
	/*
	 * 若被问及"排序规则 0"则返回假，让代码走入
	 * 非 C 路径并报告该排序规则是伪造的。
	 */
	if (!OidIsValid(collation))
		return false;

	/*
	 * 如果我们被问及默认排序规则，就必须向 C 库查
	 * 询。缓存结果，这样只需计算一次。
	 */
	if (collation == DEFAULT_COLLATION_OID)
	{
		static int	result = -1;
		char	   *localeptr;

		if (default_locale.provider == COLLPROVIDER_ICU)
			return false;

		if (result >= 0)
			return (bool) result;
		localeptr = setlocale(LC_CTYPE, NULL);
		if (!localeptr)
			elog(ERROR, "invalid LC_CTYPE setting");

		if (strcmp(localeptr, "C") == 0)
			result = true;
		else if (strcmp(localeptr, "POSIX") == 0)
			result = true;
		else
			result = false;
		return (bool) result;
	}

	/*
	 * 若被问及内置的 C/POSIX 排序规则，我们是知道的。
	 */
	if (collation == C_COLLATION_OID ||
		collation == POSIX_COLLATION_OID)
		return true;

	/*
	 * 否则我们必须查询 pg_collation，但会缓存之。
	 */
	return (lookup_collation_cache(collation, true))->ctype_is_c;
}

struct pg_locale_struct default_locale;

void
make_icu_collator(const char *iculocstr,
				  const char *icurules,
				  struct pg_locale_struct *resultp)
{
#ifdef USE_ICU
	UCollator  *collator;

	collator = pg_ucol_open(iculocstr);

	/*
	 * 如果指定了规则，我们就提取标准排序规则的规则，
	 * 加上自己的规则，用合并后的规则新建排序器
	 */
	if (icurules)
	{
		const UChar *default_rules;
		UChar	   *agg_rules;
		UChar	   *my_rules;
		UErrorCode	status;
		int32_t		length;

		default_rules = ucol_getRules(collator, &length);
		icu_to_uchar(&my_rules, icurules, strlen(icurules));

		agg_rules = palloc_array(UChar, u_strlen(default_rules) + u_strlen(my_rules) + 1);
		u_strcpy(agg_rules, default_rules);
		u_strcat(agg_rules, my_rules);

		ucol_close(collator);

		status = U_ZERO_ERROR;
		collator = ucol_openRules(agg_rules, u_strlen(agg_rules),
								  UCOL_DEFAULT, UCOL_DEFAULT_STRENGTH, NULL, &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errmsg("could not open collator for locale \"%s\" with rules \"%s\": %s",
							iculocstr, icurules, u_errorName(status))));
	}

	/* 调用者之后若报错，此串将泄漏 :-( */
	resultp->info.icu.locale = MemoryContextStrdup(TopMemoryContext, iculocstr);
	resultp->info.icu.ucol = collator;
#else							/* not USE_ICU */
	/* 排序规则若由带 ICU 的构建创建则会到这 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ICU is not supported in this build")));
#endif							/* not USE_ICU */
}


/* 供报告 newlocale() 错误的简单子例程 */
#ifdef HAVE_LOCALE_T
static void
report_newlocale_failure(const char *localename)
{
	int			save_errno;

	/*
	 * Windows 对 _create_locale() 无有用错误指示，
	 * 而 BSD 系平台似乎也认为无需设置 errno（尽管
	 * POSIX 明确要求 newlocale 应这么做）。所以，若
	 * errno 尚未设置，就假定要报告的错误
	 * 是 ENOENT。
	 */
	if (errno == 0)
		errno = ENOENT;

	/*
	 * ENOENT 意为"无此区域设置"而非"无此文件"，
	 * 故用 errdetail 澄清之。
	 */
	save_errno = errno;			/* 辅助函数可能改变 errno */
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("could not create locale \"%s\": %m",
					localename),
			 (save_errno == ENOENT ?
			  errdetail("The operating system could not find any locale data for the locale name \"%s\".",
						localename) : 0)));
}
#endif							/* HAVE_LOCALE_T */

bool
pg_locale_deterministic(pg_locale_t locale)
{
	/* 默认区域设置必须为确定性 */
	if (locale == NULL)
		return true;
	else
		return locale->deterministic;
}

/*
 * 由排序规则 OID 创建 locale_t。结果为后端生命周期
 * 内缓存。因此不要用 freelocale() 释放结果。
 *
 * 作为特别优化，默认/数据库排序规则返回 0。
 * 调用者此时应回退到未启用 locale_t 的代码路径。
 * 此外，调用者应避免在走 C/POSIX 快路径前调用
 * 本函数，因为这样的快速路径即使在 C 库不支持
 * locale_t 的平台也工作。
 *
 * 简单起见，我们总是同时生成 COLLATE 与 CTYPE，尽
 * 管可能只需要其中之一。由于本函数每个会话只调用
 * 一次，无碍。
 */
pg_locale_t
pg_newlocale_from_collation(Oid collid)
{
	collation_cache_entry *cache_entry;

	/* 调用者须传有效 OID */
	Assert(OidIsValid(collid));

	if (collid == DEFAULT_COLLATION_OID)
	{
		if (default_locale.provider == COLLPROVIDER_ICU)
			return &default_locale;
		else
			return (pg_locale_t) 0;
	}

	cache_entry = lookup_collation_cache(collid, false);

	if (cache_entry->locale == 0)
	{
		/* 本会话尚未计算过它，所以现在算 */
		HeapTuple	tp;
		Form_pg_collation collform;
		struct pg_locale_struct result;
		pg_locale_t resultp;
		Datum		datum;
		bool		isnull;

		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", collid);
		collform = (Form_pg_collation) GETSTRUCT(tp);

		/* 在分配内存前先在本地填写结果结构体 */
		memset(&result, 0, sizeof(result));
		result.provider = collform->collprovider;
		result.deterministic = collform->collisdeterministic;

		if (collform->collprovider == COLLPROVIDER_LIBC)
		{
#ifdef HAVE_LOCALE_T
			const char *collcollate;
			const char *collctype pg_attribute_unused();
			locale_t	loc;

			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collcollate);
			collcollate = TextDatumGetCString(datum);
			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_collctype);
			collctype = TextDatumGetCString(datum);

			if (strcmp(collcollate, collctype) == 0)
			{
				/* 两者相同的正常情形 */
				errno = 0;
#ifndef WIN32
				loc = newlocale(LC_COLLATE_MASK | LC_CTYPE_MASK, collcollate,
								NULL);
#else
				loc = _create_locale(LC_ALL, collcollate);
#endif
				if (!loc)
					report_newlocale_failure(collcollate);
			}
			else
			{
#ifndef WIN32
				/* 需两步 newlocale() */
				locale_t	loc1;

				errno = 0;
				loc1 = newlocale(LC_COLLATE_MASK, collcollate, NULL);
				if (!loc1)
					report_newlocale_failure(collcollate);
				errno = 0;
				loc = newlocale(LC_CTYPE_MASK, collctype, loc1);
				if (!loc)
					report_newlocale_failure(collctype);
#else

				/*
				 * XXX _create_locale() API 似乎不支持这一点。
				 * 或许可以变通：改用含两个独立
				 * 字段的 pg_locale_t。
				 */
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("collations with different collate and ctype values are not supported on this platform")));
#endif
			}

			result.info.lt = loc;
#else							/* not HAVE_LOCALE_T */
			/* 不支持 locale_t 的平台 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("collation provider LIBC is not supported on this platform")));
#endif							/* not HAVE_LOCALE_T */
		}
		else if (collform->collprovider == COLLPROVIDER_ICU)
		{
			const char *iculocstr;
			const char *icurules;

			datum = SysCacheGetAttrNotNull(COLLOID, tp, Anum_pg_collation_colliculocale);
			iculocstr = TextDatumGetCString(datum);

			datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collicurules, &isnull);
			if (!isnull)
				icurules = TextDatumGetCString(datum);
			else
				icurules = NULL;

			make_icu_collator(iculocstr, icurules, &result);
		}

		datum = SysCacheGetAttr(COLLOID, tp, Anum_pg_collation_collversion,
								&isnull);
		if (!isnull)
		{
			char	   *actual_versionstr;
			char	   *collversionstr;

			collversionstr = TextDatumGetCString(datum);

			datum = SysCacheGetAttrNotNull(COLLOID, tp, collform->collprovider == COLLPROVIDER_ICU ? Anum_pg_collation_colliculocale : Anum_pg_collation_collcollate);

			actual_versionstr = get_collation_actual_version(collform->collprovider,
															 TextDatumGetCString(datum));
			if (!actual_versionstr)
			{
				/*
				 * 在 CREATE COLLATION 中指定版本而提供者
				 * 不支持版本功能时，或有人手动在目录里
				 * 制造混乱时，就可能发生。
				 */
				ereport(ERROR,
						(errmsg("collation \"%s\" has no actual version, but a version was recorded",
								NameStr(collform->collname))));
			}

			if (strcmp(actual_versionstr, collversionstr) != 0)
				ereport(WARNING,
						(errmsg("collation \"%s\" has version mismatch",
								NameStr(collform->collname)),
						 errdetail("The collation in the database was created using version %s, "
								   "but the operating system provides version %s.",
								   collversionstr, actual_versionstr),
						 errhint("Rebuild all objects affected by this collation and run "
								 "ALTER COLLATION %s REFRESH VERSION, "
								 "or build PostgreSQL with the right library version.",
								 quote_qualified_identifier(get_namespace_name(collform->collnamespace),
															NameStr(collform->collname)))));
		}

		ReleaseSysCache(tp);

		/* 我们会把 pg_locale_t 结构保存在 TopMemoryContext */
		resultp = MemoryContextAlloc(TopMemoryContext, sizeof(*resultp));
		*resultp = result;

		cache_entry->locale = resultp;
	}

	return cache_entry->locale;
}

/*
 * 从操作系统/库获取给定排序规则的提供者特定排序规
 * 则版本字符串。
 */
char *
get_collation_actual_version(char collprovider, const char *collcollate)
{
	char	   *collversion = NULL;

#ifdef USE_ICU
	if (collprovider == COLLPROVIDER_ICU)
	{
		UCollator  *collator;
		UVersionInfo versioninfo;
		char		buf[U_MAX_VERSION_STRING_LENGTH];

		collator = pg_ucol_open(collcollate);

		ucol_getVersion(collator, versioninfo);
		ucol_close(collator);

		u_versionToString(versioninfo, buf);
		collversion = pstrdup(buf);
	}
	else
#endif
		if (collprovider == COLLPROVIDER_LIBC &&
			pg_strcasecmp("C", collcollate) != 0 &&
			pg_strncasecmp("C.", collcollate, 2) != 0 &&
			pg_strcasecmp("POSIX", collcollate) != 0)
	{
#if defined(__GLIBC__)
		/* 没有更好的选择，故使用 glibc 版本 */
		collversion = pstrdup(gnu_get_libc_version());
#elif defined(LC_VERSION_MASK)
		locale_t	loc;

		/* 查 FreeBSD 排序规则版本 */
		loc = newlocale(LC_COLLATE_MASK, collcollate, NULL);
		if (loc)
		{
			collversion =
				pstrdup(querylocale(LC_COLLATE_MASK | LC_VERSION_MASK, loc));
			freelocale(loc);
		}
		else
			ereport(ERROR,
					(errmsg("could not load locale \"%s\"", collcollate)));
#elif defined(WIN32)
		/*
		 * 如果我们的目标是 Windows Vista 及以上，就可以给定
		 * 排序规则名来请求版本（更早版本要求的
		 * 位置代码）。
		 */
		NLSVERSIONINFOEX version = {sizeof(NLSVERSIONINFOEX)};
		WCHAR		wide_collcollate[LOCALE_NAME_MAX_LENGTH];

		MultiByteToWideChar(CP_ACP, 0, collcollate, -1, wide_collcollate,
							LOCALE_NAME_MAX_LENGTH);
		if (!GetNLSVersionEx(COMPARE_STRING, wide_collcollate, &version))
		{
			/*
			 * GetNLSVersionEx() 要 "en-US" 这类语言标签，而非
			 * "English_United States.1252" 这类区域设置名。在
			 * 能阻止这些值进入系统或把它们完全可靠地
			 * 换为更有用的标签格式前，只能容忍产生的
			 * 错误并报告我们没有版本数据。
			 */
			if (GetLastError() == ERROR_INVALID_PARAMETER)
				return NULL;

			ereport(ERROR,
					(errmsg("could not get collation version for locale \"%s\": error code %lu",
							collcollate,
							GetLastError())));
		}
		collversion = psprintf("%lu.%lu,%lu.%lu",
							   (version.dwNLSVersion >> 8) & 0xFFFF,
							   version.dwNLSVersion & 0xFF,
							   (version.dwDefinedVersion >> 8) & 0xFFFF,
							   version.dwDefinedVersion & 0xFF);
#endif
	}

	return collversion;
}

/*
 * pg_strncoll_libc_win32_utf8
 *
 * Win32 没有 UTF-8。把 UTF8 参数转换为宽字符并调用
 * wcscoll() 或 wcscoll_l()。
 */
#ifdef WIN32
static int
pg_strncoll_libc_win32_utf8(const char *arg1, size_t len1, const char *arg2,
							size_t len2, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	char	   *a1p,
			   *a2p;
	int			a1len = len1 * 2 + 2;
	int			a2len = len2 * 2 + 2;
	int			r;
	int			result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);
	Assert(GetDatabaseEncoding() == PG_UTF8);
#ifndef WIN32
	Assert(false);
#endif

	if (a1len + a2len > TEXTBUFLEN)
		buf = palloc(a1len + a2len);

	a1p = buf;
	a2p = buf + a1len;

	/* API 对零长度输入不工作 */
	if (len1 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg1, len1,
								(LPWSTR) a1p, a1len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a1p)[r] = 0;

	if (len2 == 0)
		r = 0;
	else
	{
		r = MultiByteToWideChar(CP_UTF8, 0, arg2, len2,
								(LPWSTR) a2p, a2len / 2);
		if (!r)
			ereport(ERROR,
					(errmsg("could not convert string to UTF-16: error code %lu",
							GetLastError())));
	}
	((LPWSTR) a2p)[r] = 0;

	errno = 0;
#ifdef HAVE_LOCALE_T
	if (locale)
		result = wcscoll_l((LPWSTR) a1p, (LPWSTR) a2p, locale->info.lt);
	else
#endif
		result = wcscoll((LPWSTR) a1p, (LPWSTR) a2p);
	if (result == 2147483647)	/* _NLSCMPERROR；mingw 头文件缺失 */
		ereport(ERROR,
				(errmsg("could not compare Unicode strings: %m")));

	if (buf != sbuf)
		pfree(buf);

	return result;
}
#endif							/* WIN32 */

/*
 * pg_strcoll_libc
 *
 * 按给定区域设置、平台与数据库编码酌情调用
 * strcoll() 等函数。若 locale 为 NULL，
 * 用数据库排序规则。
 *
 * 参数必须以数据库编码编码并以 nul 结尾。
 */
static int
pg_strcoll_libc(const char *arg1, const char *arg2, pg_locale_t locale)
{
	int			result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);
#ifdef WIN32
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		size_t		len1 = strlen(arg1);
		size_t		len2 = strlen(arg2);

		result = pg_strncoll_libc_win32_utf8(arg1, len1, arg2, len2, locale);
	}
	else
#endif							/* WIN32 */
	if (locale)
	{
#ifdef HAVE_LOCALE_T
		result = strcoll_l(arg1, arg2, locale->info.lt);
#else
		/* 不应发生 */
		elog(ERROR, "unsupported collprovider: %c", locale->provider);
#endif
	}
	else
		result = strcoll(arg1, arg2);

	return result;
}

/*
 * pg_strncoll_libc
 *
 * 以 nul 终结参数并调用 pg_strcoll_libc()。
 */
static int
pg_strncoll_libc(const char *arg1, size_t len1, const char *arg2, size_t len2,
				 pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize1 = len1 + 1;
	size_t		bufsize2 = len2 + 1;
	char	   *arg1n;
	char	   *arg2n;
	int			result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

#ifdef WIN32
	/* 在为 nul 终结做工作之前先检查这种情况 */
	if (GetDatabaseEncoding() == PG_UTF8)
		return pg_strncoll_libc_win32_utf8(arg1, len1, arg2, len2, locale);
#endif							/* WIN32 */

	if (bufsize1 + bufsize2 > TEXTBUFLEN)
		buf = palloc(bufsize1 + bufsize2);

	arg1n = buf;
	arg2n = buf + bufsize1;

	/* 以 nul 终结参数 */
	memcpy(arg1n, arg1, len1);
	arg1n[len1] = '\0';
	memcpy(arg2n, arg2, len2);
	arg2n[len2] = '\0';

	result = pg_strcoll_libc(arg1n, arg2n, locale);

	if (buf != sbuf)
		pfree(buf);

	return result;
}

#ifdef USE_ICU

/*
 * pg_strncoll_icu_no_utf8
 *
 * 把参数从数据库编码转换为 UChar 字符串，然后调用
 * ucol_strcoll()。参数长度为 -1 表示字符串以 NUL 终
 * 止。
 *
 * 当数据库编码为 UTF-8 且 ICU 支持 ucol_strcollUTF8()
 * 时调用者应调用后者。
 */
static int
pg_strncoll_icu_no_utf8(const char *arg1, int32_t len1,
						const char *arg2, int32_t len2, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	int32_t		ulen1;
	int32_t		ulen2;
	size_t		bufsize1;
	size_t		bufsize2;
	UChar	   *uchar1,
			   *uchar2;
	int			result;

	Assert(locale->provider == COLLPROVIDER_ICU);
#ifdef HAVE_UCOL_STRCOLLUTF8
	Assert(GetDatabaseEncoding() != PG_UTF8);
#endif

	init_icu_converter();

	ulen1 = uchar_length(icu_converter, arg1, len1);
	ulen2 = uchar_length(icu_converter, arg2, len2);

	bufsize1 = (ulen1 + 1) * sizeof(UChar);
	bufsize2 = (ulen2 + 1) * sizeof(UChar);

	if (bufsize1 + bufsize2 > TEXTBUFLEN)
		buf = palloc(bufsize1 + bufsize2);

	uchar1 = (UChar *) buf;
	uchar2 = (UChar *) (buf + bufsize1);

	ulen1 = uchar_convert(icu_converter, uchar1, ulen1 + 1, arg1, len1);
	ulen2 = uchar_convert(icu_converter, uchar2, ulen2 + 1, arg2, len2);

	result = ucol_strcoll(locale->info.icu.ucol,
						  uchar1, ulen1,
						  uchar2, ulen2);

	if (buf != sbuf)
		pfree(buf);

	return result;
}

/*
 * pg_strncoll_icu
 *
 * 按给定数据库编码酌情调用 ucol_strcollUTF8() 或
 * ucol_strcoll()。参数长度 -1 表示字符串已以 NUL
 * 终结。
 *
 * 参数必须以数据库编码编码。
 */
static int
pg_strncoll_icu(const char *arg1, int32_t len1, const char *arg2, int32_t len2,
				pg_locale_t locale)
{
	int			result;

	Assert(locale->provider == COLLPROVIDER_ICU);

#ifdef HAVE_UCOL_STRCOLLUTF8
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		UErrorCode	status;

		status = U_ZERO_ERROR;
		result = ucol_strcollUTF8(locale->info.icu.ucol,
								  arg1, len1,
								  arg2, len2,
								  &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errmsg("collation failed: %s", u_errorName(status))));
	}
	else
#endif
	{
		result = pg_strncoll_icu_no_utf8(arg1, len1, arg2, len2, locale);
	}

	return result;
}

#endif							/* USE_ICU */

/*
 * pg_strcoll
 *
 * 按给定区域设置、平台与数据库编码，酌情调用
 * ucol_strcollUTF8()、ucol_strcoll()、strcoll()、
 * strcoll_l()、wcscoll() 或 wcscoll_l()。若未指定区域
 *
 * 设置，则使用数据库排序规则。
 *
 * 若排序规则为确定性，调用者负责打破平局；
 * 这与 pg_strxfrm() 保持一致，后者难以顾及确定性排
 * 序规则。
 */
int
pg_strcoll(const char *arg1, const char *arg2, pg_locale_t locale)
{
	int			result;

	if (!locale || locale->provider == COLLPROVIDER_LIBC)
		result = pg_strcoll_libc(arg1, arg2, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strncoll_icu(arg1, -1, arg2, -1, locale);
#endif
	else
		/* 不应发生 */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * pg_strncoll
 *
 * ucol_strcollUTF8()、ucol_strcoll()、strcoll()、
 * strcoll_l()、wcscoll() 或 wcscoll_l()，按给定区域设置、
 * 平台与数据库编码。未指定时用数据库排序规则。
 *
 * 参数必须以数据库编码编码。
 *
 * 本函数可能需要为 libc 函数以 nul 终结参数；所以如
 * 果调用者已有以 nul 终结的字符串，则应改为调用
 * pg_strcoll()。
 *
 * 若排序规则为确定性，调用者负责打破平局；
 * 这与 pg_strnxfrm() 保持一致，后者难以顾及确定性排
 * 序规则。
 */
int
pg_strncoll(const char *arg1, size_t len1, const char *arg2, size_t len2,
			pg_locale_t locale)
{
	int			result;

	if (!locale || locale->provider == COLLPROVIDER_LIBC)
		result = pg_strncoll_libc(arg1, len1, arg2, len2, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strncoll_icu(arg1, len1, arg2, len2, locale);
#endif
	else
		/* 不应发生 */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}


static size_t
pg_strxfrm_libc(char *dest, const char *src, size_t destsize,
				pg_locale_t locale)
{
	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

#ifdef TRUST_STRXFRM
#ifdef HAVE_LOCALE_T
	if (locale)
		return strxfrm_l(dest, src, destsize, locale->info.lt);
	else
#endif
		return strxfrm(dest, src, destsize);
#else
	/* 不应发生 */
	PGLOCALE_SUPPORT_ERROR(locale->provider);
	return 0;					/* 让编译器安静 */
#endif
}

static size_t
pg_strnxfrm_libc(char *dest, const char *src, size_t srclen, size_t destsize,
				 pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	size_t		bufsize = srclen + 1;
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (bufsize > TEXTBUFLEN)
		buf = palloc(bufsize);

	/* 以 nul 终结参数 */
	memcpy(buf, src, srclen);
	buf[srclen] = '\0';

	result = pg_strxfrm_libc(dest, buf, destsize, locale);

	if (buf != sbuf)
		pfree(buf);

	/* 若定义了 dest，它就应以 nul 终结 */
	Assert(result >= destsize || dest[result] == '\0');

	return result;
}

#ifdef USE_ICU

/* 'srclen' 为 -1 表示字符串以 NUL 终结 */
static size_t
pg_strnxfrm_icu(char *dest, const char *src, int32_t srclen, int32_t destsize,
				pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	UChar	   *uchar;
	int32_t		ulen;
	size_t		uchar_bsize;
	Size		result_bsize;

	Assert(locale->provider == COLLPROVIDER_ICU);

	init_icu_converter();

	ulen = uchar_length(icu_converter, src, srclen);

	uchar_bsize = (ulen + 1) * sizeof(UChar);

	if (uchar_bsize > TEXTBUFLEN)
		buf = palloc(uchar_bsize);

	uchar = (UChar *) buf;

	ulen = uchar_convert(icu_converter, uchar, ulen + 1, src, srclen);

	result_bsize = ucol_getSortKey(locale->info.icu.ucol,
								   uchar, ulen,
								   (uint8_t *) dest, destsize);

	/*
	 * ucol_getSortKey() 把 nul 终结符计入结果长度，但本函
	 * 数不应这样。
	 */
	Assert(result_bsize > 0);
	result_bsize--;

	if (buf != sbuf)
		pfree(buf);

	/* 若定义了 dest，它就应以 nul 终结 */
	Assert(result_bsize >= destsize || dest[result_bsize] == '\0');

	return result_bsize;
}

/* 'srclen' 为 -1 表示字符串以 NUL 终结 */
static size_t
pg_strnxfrm_prefix_icu_no_utf8(char *dest, const char *src, int32_t srclen,
							   int32_t destsize, pg_locale_t locale)
{
	char		sbuf[TEXTBUFLEN];
	char	   *buf = sbuf;
	UCharIterator iter;
	uint32_t	state[2];
	UErrorCode	status;
	int32_t		ulen = -1;
	UChar	   *uchar = NULL;
	size_t		uchar_bsize;
	Size		result_bsize;

	Assert(locale->provider == COLLPROVIDER_ICU);
	Assert(GetDatabaseEncoding() != PG_UTF8);

	init_icu_converter();

	ulen = uchar_length(icu_converter, src, srclen);

	uchar_bsize = (ulen + 1) * sizeof(UChar);

	if (uchar_bsize > TEXTBUFLEN)
		buf = palloc(uchar_bsize);

	uchar = (UChar *) buf;

	ulen = uchar_convert(icu_converter, uchar, ulen + 1, src, srclen);

	uiter_setString(&iter, uchar, ulen);
	state[0] = state[1] = 0;	/* 之后不再需要 */
	status = U_ZERO_ERROR;
	result_bsize = ucol_nextSortKeyPart(locale->info.icu.ucol,
										&iter,
										state,
										(uint8_t *) dest,
										destsize,
										&status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("sort key generation failed: %s",
						u_errorName(status))));

	return result_bsize;
}

/* 'srclen' 为 -1 表示字符串以 NUL 终结 */
static size_t
pg_strnxfrm_prefix_icu(char *dest, const char *src, int32_t srclen,
					   int32_t destsize, pg_locale_t locale)
{
	size_t		result;

	Assert(locale->provider == COLLPROVIDER_ICU);

	if (GetDatabaseEncoding() == PG_UTF8)
	{
		UCharIterator iter;
		uint32_t	state[2];
		UErrorCode	status;

		uiter_setUTF8(&iter, src, srclen);
		state[0] = state[1] = 0;	/* 之后不再需要 */
		status = U_ZERO_ERROR;
		result = ucol_nextSortKeyPart(locale->info.icu.ucol,
									  &iter,
									  state,
									  (uint8_t *) dest,
									  destsize,
									  &status);
		if (U_FAILURE(status))
			ereport(ERROR,
					(errmsg("sort key generation failed: %s",
							u_errorName(status))));
	}
	else
		result = pg_strnxfrm_prefix_icu_no_utf8(dest, src, srclen, destsize,
												locale);

	return result;
}

#endif

/*
 * 返回排序规则提供者是否支持 pg_strxfrm() 与
 * pg_strnxfrm(); 否则假。
 *
 * 不幸的是，非 C 排序规则的 strxfrm() 在许多常见平台
 * 上都有问题；对多个 glibc 版本的测试表明，对许多区
 * 域设置而言 strcoll() 与 strxfrm() 结果不一致。虽
 * 然除 Cygwin 之外尚无其他 libc 被证明有问题，但我们
 * 现在采取保守做法，将这一律禁用。（
 * 确信其系统上没有此问题的用户可以定义
 *  TRUST_STRXFRM。）
 *
 * ICU 提供者没有已知类似问题。
 */
bool
pg_strxfrm_enabled(pg_locale_t locale)
{
	if (!locale || locale->provider == COLLPROVIDER_LIBC)
#ifdef TRUST_STRXFRM
		return true;
#else
		return false;
#endif
	else if (locale->provider == COLLPROVIDER_ICU)
		return true;
	else
		/* 不应发生 */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return false;				/* 让编译器安静 */
}

/*
 * pg_strxfrm
 *
 * 把 'src' 转换为以 nul 终结、存于 'dest' 的字符串，使
 * 得对转换后字符串的 strcmp() 等价于对未转换串
 * 的 pg_strcoll()。
 *
 * 提供的 src 必须以 nul 终结。destsize 为零时 dest
 * 可为 NULL
 *
 * 返回存储转换后字符串所需（或更多）的字节数，不
 * 含结尾的 nul 字节。若返回值大于等于 destsize，
 * 则 'dest' 最终内容未定义。
 */
size_t
pg_strxfrm(char *dest, const char *src, size_t destsize, pg_locale_t locale)
{
	size_t		result = 0;		/* 让编译器安静 */

	if (!locale || locale->provider == COLLPROVIDER_LIBC)
		result = pg_strxfrm_libc(dest, src, destsize, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_icu(dest, src, -1, destsize, locale);
#endif
	else
		/* 不应发生 */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * pg_strnxfrm
 *
 * 把 'src' 转换为以 nul 终结、存于 'dest' 的字符串，使
 * 得对转换后字符串的 strcmp() 等价于对未转换串
 * 的 pg_strcoll()。
 *
 * 'src' 无需以 nul 终结。destsize 为零时 dest 可
 * 为空
 *
 * 返回存储转换后字符串所需（或更多）的字节数，不
 * 含结尾的 nul 字节。若返回值大于等于 destsize，
 * 则 'dest' 最终内容未定义。
 *
 * 本函数可能需要为 libc 函数以 nul 终结参数，所以如
 * 果调用者已有以 nul 终结的字符串，则应改为调用
 * pg_strxfrm()。
 */
size_t
pg_strnxfrm(char *dest, size_t destsize, const char *src, size_t srclen,
			pg_locale_t locale)
{
	size_t		result = 0;		/* 让编译器安静 */

	if (!locale || locale->provider == COLLPROVIDER_LIBC)
		result = pg_strnxfrm_libc(dest, src, srclen, destsize, locale);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_icu(dest, src, srclen, destsize, locale);
#endif
	else
		/* 不应发生 */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * 返回排序规则提供者是否支持 pg_strxfrm_prefix()
 * 与 pg_strnxfrm_prefix()；否则假
 */
bool
pg_strxfrm_prefix_enabled(pg_locale_t locale)
{
	if (!locale || locale->provider == COLLPROVIDER_LIBC)
		return false;
	else if (locale->provider == COLLPROVIDER_ICU)
		return true;
	else
		/* 不应发生 */
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return false;				/* 让编译器安静 */
}

/*
 * pg_strxfrm_prefix
 *
 * 把 'src' 转换为存于 'dest' 的字节序列，使对字节序
 * 列做普通 memcmp() 等价于对未转换字符串做
 * pg_strcoll()。结果不以 nul 终结。
 *
 * 提供的 'src' 必须以 nul 终结。
 *
 * 如果 destsize 不足以容纳得到的字节序列，则只在
 * 'dest' 中存储前 destsize 个字节。返回实际复制到
 * 'dest' 的字节数。
 */
size_t
pg_strxfrm_prefix(char *dest, const char *src, size_t destsize,
				  pg_locale_t locale)
{
	size_t		result = 0;		/* 让编译器安静 */

	if (!locale)
		PGLOCALE_SUPPORT_ERROR(COLLPROVIDER_LIBC);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_prefix_icu(dest, src, -1, destsize, locale);
#endif
	else
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

/*
 * pg_strnxfrm_prefix
 *
 * 把 'src' 转换为存于 'dest' 的字节序列，使对字节序
 * 列做普通 memcmp() 等价于对未转换字符串做
 * pg_strcoll()。结果不以 nul 终结。
 *
 * 提供的 'src' 必须以 nul 终结。
 *
 * 如果 destsize 不足以容纳得到的字节序列，则只在
 * 'dest' 中存储前 destsize 个字节。返回实际复制到
 * 'dest' 的字节数。
 *
 * 本函数可能需要为 libc 函数以 nul 终结参数，所以如
 * 果调用者已有以 nul 终结的字符串，则应改为调用
 * pg_strxfrm_prefix()。
 */
size_t
pg_strnxfrm_prefix(char *dest, size_t destsize, const char *src,
				   size_t srclen, pg_locale_t locale)
{
	size_t		result = 0;		/* 让编译器安静 */

	if (!locale)
		PGLOCALE_SUPPORT_ERROR(COLLPROVIDER_LIBC);
#ifdef USE_ICU
	else if (locale->provider == COLLPROVIDER_ICU)
		result = pg_strnxfrm_prefix_icu(dest, src, -1, destsize, locale);
#endif
	else
		PGLOCALE_SUPPORT_ERROR(locale->provider);

	return result;
}

#ifdef USE_ICU

/*
 * 包装 ucol_open() 以处理旧版 ICU 的 API 差异。
 * versions.
 */
static UCollator *
pg_ucol_open(const char *loc_str)
{
	UCollator  *collator;
	UErrorCode	status;
	const char *orig_str = loc_str;
	char	   *fixed_str = NULL;

	/*
	 * 绝不能打开默认排序器，因为它依赖环境且可能随时
	 * 改变。本不应发生，但在此检查以捕获那些原本难以
	 * 捕获的 bug。
	 *
	 * 注意：默认排序器不等于根区域设置的排序器。根区
	 * 域设置可以用空字符串、"und" 或 "root" 指定。默认排
	 * 序器通过向 ucol_open() 传递 NULL 来打开。
	 */
	if (loc_str == NULL)
		elog(ERROR, "opening default collator is not supported");

	/*
	 * 在 ICU 54 及更早版本中，"und" 不是根区域设置的公认
	 * 拼法。若区域设置首个成分是 "und"，则在打开前
	 * 替换为 "root"。
	 */
	if (U_ICU_VERSION_MAJOR_NUM < 55)
	{
		char		lang[ULOC_LANG_CAPACITY];

		status = U_ZERO_ERROR;
		uloc_getLanguage(loc_str, lang, ULOC_LANG_CAPACITY, &status);
		if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
		{
			ereport(ERROR,
					(errmsg("could not get language from locale \"%s\": %s",
							loc_str, u_errorName(status))));
		}

		if (strcmp(lang, "und") == 0)
		{
			const char *remainder = loc_str + strlen("und");

			fixed_str = palloc(strlen("root") + strlen(remainder) + 1);
			strcpy(fixed_str, "root");
			strcat(fixed_str, remainder);

			loc_str = fixed_str;
		}
	}

	status = U_ZERO_ERROR;
	collator = ucol_open(loc_str, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
		/* 用原字符串报告错误 */
				(errmsg("could not open collator for locale \"%s\": %s",
						orig_str, u_errorName(status))));

	if (U_ICU_VERSION_MAJOR_NUM < 54)
	{
		status = U_ZERO_ERROR;
		icu_set_collation_attributes(collator, loc_str, &status);

		/*
		 * 假称错误来自 ucol_open()，使各 ICU 版本的错误
		 * 消息保持一致。
		 */
		if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
		{
			ucol_close(collator);
			ereport(ERROR,
					(errmsg("could not open collator for locale \"%s\": %s",
							orig_str, u_errorName(status))));
		}
	}

	if (fixed_str != NULL)
		pfree(fixed_str);

	return collator;
}

static void
init_icu_converter(void)
{
	const char *icu_encoding_name;
	UErrorCode	status;
	UConverter *conv;

	if (icu_converter)
		return;					/* 已做过 */

	icu_encoding_name = get_encoding_name_for_icu(GetDatabaseEncoding());
	if (!icu_encoding_name)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("encoding \"%s\" not supported by ICU",
						pg_encoding_to_char(GetDatabaseEncoding()))));

	status = U_ZERO_ERROR;
	conv = ucnv_open(icu_encoding_name, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("could not open ICU converter for encoding \"%s\": %s",
						icu_encoding_name, u_errorName(status))));

	icu_converter = conv;
}

/*
 * 求给定串转为 UChar 字符串后的 UChar 长度。
 */
static size_t
uchar_length(UConverter *converter, const char *str, int32_t len)
{
	UErrorCode	status = U_ZERO_ERROR;
	int32_t		ulen;

	ulen = ucnv_toUChars(converter, NULL, 0, str, len, &status);
	if (U_FAILURE(status) && status != U_BUFFER_OVERFLOW_ERROR)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_toUChars", u_errorName(status))));
	return ulen;
}

/*
 * 把给定源字符串转换为一个 UChar 字符串，存入 dest，
 * 并返回 UChar 数。
 */
static int32_t
uchar_convert(UConverter *converter, UChar *dest, int32_t destlen,
			  const char *src, int32_t srclen)
{
	UErrorCode	status = U_ZERO_ERROR;
	int32_t		ulen;

	status = U_ZERO_ERROR;
	ulen = ucnv_toUChars(converter, dest, destlen, src, srclen, &status);
	if (U_FAILURE(status))
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_toUChars", u_errorName(status))));
	return ulen;
}

/*
 * 把数据库编码的字符串转换为 UChar 字符串。
 *
 * 位于 buff 的源字符串长度为 nbytes
 * （不必 nul 终结）
 *
 * *buff_uchar 收到 palloc 结果串的指针，本函数
 * 的结果为生成的 UChar 数。
 *
 * 结果字符串以 nul 终结，但多数调用者依赖的是
 * 长度本身。
 */
int32_t
icu_to_uchar(UChar **buff_uchar, const char *buff, size_t nbytes)
{
	int32_t		len_uchar;

	init_icu_converter();

	len_uchar = uchar_length(icu_converter, buff, nbytes);

	*buff_uchar = palloc((len_uchar + 1) * sizeof(**buff_uchar));
	len_uchar = uchar_convert(icu_converter,
							  *buff_uchar, len_uchar + 1, buff, nbytes);

	return len_uchar;
}

/*
 * 把 UChar 字符串转换为数据库编码。
 *
 * 位于 buff_uchar 的源字符串长度为 len_uchar
 * （不必 nul 终结）
 *
 * *result 收到 palloc 结果串的指针，本函数
 * 的结果为生成的字节数（不计 nul）。
 *
 * 结果串以 nul 终结。
 */
int32_t
icu_from_uchar(char **result, const UChar *buff_uchar, int32_t len_uchar)
{
	UErrorCode	status;
	int32_t		len_result;

	init_icu_converter();

	status = U_ZERO_ERROR;
	len_result = ucnv_fromUChars(icu_converter, NULL, 0,
								 buff_uchar, len_uchar, &status);
	if (U_FAILURE(status) && status != U_BUFFER_OVERFLOW_ERROR)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_fromUChars",
						u_errorName(status))));

	*result = palloc(len_result + 1);

	status = U_ZERO_ERROR;
	len_result = ucnv_fromUChars(icu_converter, *result, len_result + 1,
								 buff_uchar, len_uchar, &status);
	if (U_FAILURE(status) ||
		status == U_STRING_NOT_TERMINATED_WARNING)
		ereport(ERROR,
				(errmsg("%s failed: %s", "ucnv_fromUChars",
						u_errorName(status))));

	return len_result;
}

/*
 * 从给定的区域设置字符串解析排序规则属性并应用到
 * 排序器上。
 *
 * 首先，把区域设置字符串规范化为 ICU 格式区域设
 * 置 ID（如 "und@colStrength=primary;colCaseLevel=yes"），
 * 解析并应用参数。
 *
 * 从 ICU 54 版开始，属性由 ucol_open() 自动处理，因此
 * 只有在旧版本上模拟这种行为时才需要这样做。
 * versions.
 */
pg_attribute_unused()
static void
icu_set_collation_attributes(UCollator *collator, const char *loc,
							 UErrorCode *status)
{
	int32_t		len;
	char	   *icu_locale_id;
	char	   *lower_str;
	char	   *str;

	/*
	 * 输入可能是 BCP 47 语言标签，例如
	 * "und-u-kc-ks-level1"，它以不同形式表达相同的属
	 * 性。它会被转换为等价的 ICU 格式区域设置 ID，如
	 * "und@colcaselevel=yes;colstrength=primary"，经
	 * uloc_canonicalize()
	 */
	*status = U_ZERO_ERROR;
	len = uloc_canonicalize(loc, NULL, 0, status);
	icu_locale_id = palloc(len + 1);
	*status = U_ZERO_ERROR;
	len = uloc_canonicalize(loc, icu_locale_id, len + 1, status);
	if (U_FAILURE(*status) || *status == U_STRING_NOT_TERMINATED_WARNING)
		return;

	lower_str = asc_tolower(icu_locale_id, strlen(icu_locale_id));

	pfree(icu_locale_id);

	str = strchr(lower_str, '@');
	if (!str)
		return;
	str++;

	for (char *token = strtok(str, ";"); token; token = strtok(NULL, ";"))
	{
		char	   *e = strchr(token, '=');

		if (e)
		{
			char	   *name;
			char	   *value;
			UColAttribute uattr;
			UColAttributeValue uvalue;

			*status = U_ZERO_ERROR;

			*e = '\0';
			name = token;
			value = e + 1;

			/*
			 * 属性名与值的清单见 ICU i18n/coll.cpp
			 */
			if (strcmp(name, "colstrength") == 0)
				uattr = UCOL_STRENGTH;
			else if (strcmp(name, "colbackwards") == 0)
				uattr = UCOL_FRENCH_COLLATION;
			else if (strcmp(name, "colcaselevel") == 0)
				uattr = UCOL_CASE_LEVEL;
			else if (strcmp(name, "colcasefirst") == 0)
				uattr = UCOL_CASE_FIRST;
			else if (strcmp(name, "colalternate") == 0)
				uattr = UCOL_ALTERNATE_HANDLING;
			else if (strcmp(name, "colnormalization") == 0)
				uattr = UCOL_NORMALIZATION_MODE;
			else if (strcmp(name, "colnumeric") == 0)
				uattr = UCOL_NUMERIC_COLLATION;
			else
				/* 未知则忽略 */
				continue;

			if (strcmp(value, "primary") == 0)
				uvalue = UCOL_PRIMARY;
			else if (strcmp(value, "secondary") == 0)
				uvalue = UCOL_SECONDARY;
			else if (strcmp(value, "tertiary") == 0)
				uvalue = UCOL_TERTIARY;
			else if (strcmp(value, "quaternary") == 0)
				uvalue = UCOL_QUATERNARY;
			else if (strcmp(value, "identical") == 0)
				uvalue = UCOL_IDENTICAL;
			else if (strcmp(value, "no") == 0)
				uvalue = UCOL_OFF;
			else if (strcmp(value, "yes") == 0)
				uvalue = UCOL_ON;
			else if (strcmp(value, "shifted") == 0)
				uvalue = UCOL_SHIFTED;
			else if (strcmp(value, "non-ignorable") == 0)
				uvalue = UCOL_NON_IGNORABLE;
			else if (strcmp(value, "lower") == 0)
				uvalue = UCOL_LOWER_FIRST;
			else if (strcmp(value, "upper") == 0)
				uvalue = UCOL_UPPER_FIRST;
			else
			{
				*status = U_ILLEGAL_ARGUMENT_ERROR;
				break;
			}

			ucol_setAttribute(collator, uattr, uvalue, status);
		}
	}

	pfree(lower_str);
}
#endif

/*
 * 返回所请求区域设置的 BCP47 语言标签表示。
 *
 * 本函数应在把字符串传给 ucol_open() 之前调用，因为
 * 转换成语言标签会顺带做"二级规范化"，
 * 化"。这不只能产生一致的格式，还能更准
 * 确地解释各种输入的区域设置字符串格
 * 式，如 POSIX 与 .NET ID。
 */
char *
icu_language_tag(const char *loc_str, int elevel)
{
#ifdef USE_ICU
	UErrorCode	status;
	char	   *langtag;
	size_t		buflen = 32;	/* 任意起始缓冲大小 */
	const bool	strict = true;

	/*
	 * BCP47 语言标签没有明确的上限（参见 RFC5646 第 4.4
	 * 节）。另外，旧版 ICU 的 uloc_toLanguageTag()
	 * 首次调用未必返回最终长度，
	 * 故需要循环。
	 */
	langtag = palloc(buflen);
	while (true)
	{
		status = U_ZERO_ERROR;
		uloc_toLanguageTag(loc_str, langtag, buflen, strict, &status);

		/* 若缓冲区不够大则重试 */
		if ((status == U_BUFFER_OVERFLOW_ERROR ||
			 status == U_STRING_NOT_TERMINATED_WARNING) &&
			buflen < MaxAllocSize)
		{
			buflen = Min(buflen * 2, MaxAllocSize);
			langtag = repalloc(langtag, buflen);
			continue;
		}

		break;
	}

	if (U_FAILURE(status))
	{
		pfree(langtag);

		if (elevel > 0)
			ereport(elevel,
					(errmsg("could not convert locale name \"%s\" to language tag: %s",
							loc_str, u_errorName(status))));
		return NULL;
	}

	return langtag;
#else							/* not USE_ICU */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ICU is not supported in this build")));
	return NULL;				/* 让编译器安静 */
#endif							/* not USE_ICU */
}

/*
 * 尽力检查该区域设置是否有效。
 */
void
icu_validate_locale(const char *loc_str)
{
#ifdef USE_ICU
	UCollator  *collator;
	UErrorCode	status;
	char		lang[ULOC_LANG_CAPACITY];
	bool		found = false;
	int			elevel = icu_validation_level;

	/* 无验证 */
	if (elevel < 0)
		return;

	/* pg_upgrade 时降为 WARNING */
	if (IsBinaryUpgrade && elevel > WARNING)
		elevel = WARNING;

	/* 验证我们能提取语言 */
	status = U_ZERO_ERROR;
	uloc_getLanguage(loc_str, lang, ULOC_LANG_CAPACITY, &status);
	if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
	{
		ereport(elevel,
				(errmsg("could not get language from ICU locale \"%s\": %s",
						loc_str, u_errorName(status)),
				 errhint("To disable ICU locale validation, set the parameter \"%s\" to \"%s\".",
						 "icu_validation_level", "disabled")));
		return;
	}

	/* 检查特殊语言名 */
	if (strcmp(lang, "") == 0 ||
		strcmp(lang, "root") == 0 || strcmp(lang, "und") == 0)
		found = true;

	/* 在 ICU 内搜索匹配的语言 */
	for (int32_t i = 0; !found && i < uloc_countAvailable(); i++)
	{
		const char *otherloc = uloc_getAvailable(i);
		char		otherlang[ULOC_LANG_CAPACITY];

		status = U_ZERO_ERROR;
		uloc_getLanguage(otherloc, otherlang, ULOC_LANG_CAPACITY, &status);
		if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING)
			continue;

		if (strcmp(lang, otherlang) == 0)
			found = true;
	}

	if (!found)
		ereport(elevel,
				(errmsg("ICU locale \"%s\" has unknown language \"%s\"",
						loc_str, lang),
				 errhint("To disable ICU locale validation, set the parameter \"%s\" to \"%s\".",
						 "icu_validation_level", "disabled")));

	/* 检查它能被打开 */
	collator = pg_ucol_open(loc_str);
	ucol_close(collator);
#else							/* not USE_ICU */
	/* 排序规则若由带 ICU 的构建创建则会到这 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ICU is not supported in this build")));
#endif							/* not USE_ICU */
}

/*
 * 这些函数与 libc 的 wchar_t 相互转换，*而非*
 * pg_wchar_t。故放于此处，而非 mbutils 代码
 */

/*
 * wchar2char --- 把宽字符转换为多字节格式
 *
 * 它与标准 wcstombs_l() 函数的 API 相同；特别地，
 * tolen 是存入 *to 的最大字节数，且 *from 必须
 * 以零终结。输出只在有空间时才会以零终结。
 */
size_t
wchar2char(char *to, const wchar_t *from, size_t tolen, pg_locale_t locale)
{
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (tolen == 0)
		return 0;

#ifdef WIN32

	/*
	 * 在 Windows 上，"Unicode" 区域设置假定编码为
	 * UTF16 而非 UTF8；mbstowcs/wcstombs 不代劳，故改用
	 * MultiByteToWideChar().
	 */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		result = WideCharToMultiByte(CP_UTF8, 0, from, -1, to, tolen,
									 NULL, NULL);
		/* 返回零即失败 */
		if (result <= 0)
			result = -1;
		else
		{
			Assert(result <= tolen);
			/* 微软把零终结符计入结果 */
			result--;
		}
	}
	else
#endif							/* WIN32 */
	if (locale == (pg_locale_t) 0)
	{
		/* 默认区域设置直接用 wcstombs */
		result = wcstombs(to, from, tolen);
	}
	else
	{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
		/* 非默认区域设置用 wcstombs_l */
		result = wcstombs_l(to, from, tolen, locale->info.lt);
#else							/* !HAVE_WCSTOMBS_L */
		/* 只得临时把该区域设置设为当前... 唉 */
		locale_t	save_locale = uselocale(locale->info.lt);

		result = wcstombs(to, from, tolen);

		uselocale(save_locale);
#endif							/* HAVE_WCSTOMBS_L */
#else							/* !HAVE_LOCALE_T */
		/* 无 HAVE_LOCALE_T 时不能有 locale != 0 */
		elog(ERROR, "wcstombs_l is not available");
		result = 0;				/* 让编译器安静 */
#endif							/* HAVE_LOCALE_T */
	}

	return result;
}

/*
 * char2wchar --- 把多字节字符转换为宽字符
 *
 * 它的 API 与 mbstowcs_l() 几乎相同，区别是 *from
 * 无需以 null 终结；而是把输入字节数指定为
 * fromlen。且对非法输入编码我们 ereport() 而非
 * 返回 -1。tolen 是要存到 *to 的 wchar_t 最大个数。
 * 输出只在有空间时才以零终结。
 */
size_t
char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen,
		   pg_locale_t locale)
{
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (tolen == 0)
		return 0;

#ifdef WIN32
	/* 见上 WIN32 "Unicode" 注释 */
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		/* Win32 API 对零长度输入不工作 */
		if (fromlen == 0)
			result = 0;
		else
		{
			result = MultiByteToWideChar(CP_UTF8, 0, from, fromlen, to, tolen - 1);
			/* 返回零即失败 */
			if (result == 0)
				result = -1;
		}

		if (result != -1)
		{
			Assert(result < tolen);
			/* 追加结尾 null wchar（MultiByteToWideChar() 不加） */
			to[result] = 0;
		}
	}
	else
#endif							/* WIN32 */
	{
		/* mbstowcs 需结尾 '\0' */
		char	   *str = pnstrdup(from, fromlen);

		if (locale == (pg_locale_t) 0)
		{
			/* 默认区域设置直接用 mbstowcs */
			result = mbstowcs(to, str, tolen);
		}
		else
		{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_MBSTOWCS_L
			/* 非默认区域设置用 mbstowcs_l */
			result = mbstowcs_l(to, str, tolen, locale->info.lt);
#else							/* !HAVE_MBSTOWCS_L */
			/* 只得临时把该区域设置设为当前... 唉 */
			locale_t	save_locale = uselocale(locale->info.lt);

			result = mbstowcs(to, str, tolen);

			uselocale(save_locale);
#endif							/* HAVE_MBSTOWCS_L */
#else							/* !HAVE_LOCALE_T */
			/* 无 HAVE_LOCALE_T 时不能有 locale != 0 */
			elog(ERROR, "mbstowcs_l is not available");
			result = 0;			/* 让编译器安静 */
#endif							/* HAVE_LOCALE_T */
		}

		pfree(str);
	}

	if (result == -1)
	{
		/*
		 * 碰到非法多字节字符。我们试图借
		 * pg_verifymbstr 检查该字符串来给出有用的错误消息。
		 * 但也可能此串对我们没问题，mbstowcs 却认为
		 * 行 --- 这暗示 LC_CTYPE 与数据库编码不同。
		 * 若 pg_verifymbstr 找不出问题，就给出一条通用
		 * 错误消息。
		 */
		pg_verifymbstr(from, fromlen, false);	/* 可能不返回 */
		/* 若返回了... */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	return result;
}
