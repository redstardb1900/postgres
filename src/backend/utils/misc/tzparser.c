/*-------------------------------------------------------------------------
 *
 * tzparser.c
 *	  用于解析时区偏移文件的函数
 *
 * 注：本代码从 GUC 变量的 check_hook 中调用，
 * timezone_abbreviations。因此它应通过 GUC_check_errmsg()
 * 及其相关函数报告问题，并尽量避免抛出
 * elog(ERROR)。这在目前还不完全可靠 --- 特别是
 * 内存耗尽会抛出错误。必要时或许可以用 PG_TRY
 * 来修复。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/tzparser.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/tzparser.h"


#define WHITESPACE " \t\n\r"

static bool validateTzEntry(tzEntry *tzentry);
static bool splitTzLine(const char *filename, int lineno,
						char *line, tzEntry *tzentry);
static int	addToArray(tzEntry **base, int *arraysize, int n,
					   tzEntry *entry, bool override);
static int	ParseTzFile(const char *filename, int depth,
						tzEntry **base, int *arraysize, int n);


/*
 * 对一个 tzEntry 施加额外的验证检查
 *
 * OK 返回 true，否则 false
 */
static bool
validateTzEntry(tzEntry *tzentry)
{
	unsigned char *p;

	/*
	 * 检查 datetktbl 存储格式施加的限制（见 datetime.c）
	 */
	if (strlen(tzentry->abbrev) > TOKMAXLEN)
	{
		GUC_check_errmsg("time zone abbreviation \"%s\" is too long (maximum %d characters) in time zone file \"%s\", line %d",
						 tzentry->abbrev, TOKMAXLEN,
						 tzentry->filename, tzentry->lineno);
		return false;
	}

	/*
	 * 合理性检查偏移量：不得超过 14 小时
	 */
	if (tzentry->offset > 14 * 60 * 60 ||
		tzentry->offset < -14 * 60 * 60)
	{
		GUC_check_errmsg("time zone offset %d is out of range in time zone file \"%s\", line %d",
						 tzentry->offset,
						 tzentry->filename, tzentry->lineno);
		return false;
	}

	/*
	 * abbrev 转为小写（必须与 datetime.c 的转换匹配）
	 */
	for (p = (unsigned char *) tzentry->abbrev; *p; p++)
		*p = pg_tolower(*p);

	return true;
}

/*
 * 尝试把该行解析为时区缩写规格
 *
 * 有效格式为：
 *	name  zone
 *	name  offset  dst
 *
 * 成功返回 true，否则 false；数据存入 *tzentry
 */
static bool
splitTzLine(const char *filename, int lineno, char *line, tzEntry *tzentry)
{
	char	   *abbrev;
	char	   *offset;
	char	   *offset_endptr;
	char	   *remain;
	char	   *is_dst;

	tzentry->lineno = lineno;
	tzentry->filename = filename;

	abbrev = strtok(line, WHITESPACE);
	if (!abbrev)
	{
		GUC_check_errmsg("missing time zone abbreviation in time zone file \"%s\", line %d",
						 filename, lineno);
		return false;
	}
	tzentry->abbrev = pstrdup(abbrev);

	offset = strtok(NULL, WHITESPACE);
	if (!offset)
	{
		GUC_check_errmsg("missing time zone offset in time zone file \"%s\", line %d",
						 filename, lineno);
		return false;
	}

	/* 我们假设时区名不以数字或符号开头 */
	if (isdigit((unsigned char) *offset) || *offset == '+' || *offset == '-')
	{
		tzentry->zone = NULL;
		tzentry->offset = strtol(offset, &offset_endptr, 10);
		if (offset_endptr == offset || *offset_endptr != '\0')
		{
			GUC_check_errmsg("invalid number for time zone offset in time zone file \"%s\", line %d",
							 filename, lineno);
			return false;
		}

		is_dst = strtok(NULL, WHITESPACE);
		if (is_dst && pg_strcasecmp(is_dst, "D") == 0)
		{
			tzentry->is_dst = true;
			remain = strtok(NULL, WHITESPACE);
		}
		else
		{
			/* 没有 'D' dst 限定符 */
			tzentry->is_dst = false;
			remain = is_dst;
		}
	}
	else
	{
		/*
		 * 假定条目是时区名。我们不试图通过查找
		 * 该时区来验证它，因为那样会强制加载大量
		 * 当前会话中可能永远用不到的时区。
		 */
		tzentry->zone = pstrdup(offset);
		tzentry->offset = 0;
		tzentry->is_dst = false;
		remain = strtok(NULL, WHITESPACE);
	}

	if (!remain)				/* 已无非空白字符 */
		return true;

	if (remain[0] != '#')		/* 必是注释 */
	{
		GUC_check_errmsg("invalid syntax in time zone file \"%s\", line %d",
						 filename, lineno);
		return false;
	}
	return true;
}

/*
 * 把条目插入有序数组
 *
 * *base: 数组基址（若需扩容数组则可更改）
 * *arraysize: 数组已分配长度（须扩容数组时可更改）
 * n: 数组中当前有效元素的个数
 * entry: 待插入新数据
 * override: 可覆盖时为 true
 *
 * 返回新数组长度（n 的新值），出错则 -1
 */
static int
addToArray(tzEntry **base, int *arraysize, int n,
		   tzEntry *entry, bool override)
{
	tzEntry    *arrayptr;
	int			low;
	int			high;

	/*
	 * 在数组中搜索重复项；一个有用的副作用是数组会
	 * 保持有序。我们用 strcmp() 来确保与 datetime.c
	 * 期望的排序一致。
	 */
	arrayptr = *base;
	low = 0;
	high = n - 1;
	while (low <= high)
	{
		int			mid = (low + high) >> 1;
		tzEntry    *midptr = arrayptr + mid;
		int			cmp;

		cmp = strcmp(entry->abbrev, midptr->abbrev);
		if (cmp < 0)
			high = mid - 1;
		else if (cmp > 0)
			low = mid + 1;
		else
		{
			/*
			 * 发现重复条目；除非相同否则报错。
			 */
			if ((midptr->zone == NULL && entry->zone == NULL &&
				 midptr->offset == entry->offset &&
				 midptr->is_dst == entry->is_dst) ||
				(midptr->zone != NULL && entry->zone != NULL &&
				 strcmp(midptr->zone, entry->zone) == 0))
			{
				/* 原样返回数组 */
				return n;
			}
			if (override)
			{
				/* 缩写相同但有不同之处，覆盖 */
				midptr->zone = entry->zone;
				midptr->offset = entry->offset;
				midptr->is_dst = entry->is_dst;
				return n;
			}
			/* 缩写相同但有不同之处，报错 */
			GUC_check_errmsg("time zone abbreviation \"%s\" is multiply defined",
							 entry->abbrev);
			GUC_check_errdetail("Entry in time zone file \"%s\", line %d, conflicts with entry in file \"%s\", line %d.",
								midptr->filename, midptr->lineno,
								entry->filename, entry->lineno);
			return -1;
		}
	}

	/*
	 * 无匹配，插入于 "low"。
	 */
	if (n >= *arraysize)
	{
		*arraysize *= 2;
		*base = (tzEntry *) repalloc(*base, *arraysize * sizeof(tzEntry));
	}

	arrayptr = *base + low;

	memmove(arrayptr + 1, arrayptr, (n - low) * sizeof(tzEntry));

	memcpy(arrayptr, entry, sizeof(tzEntry));

	return n + 1;
}

/*
 * 解析单个时区缩写文件 --- 可递归处理 @INCLUDE
 *
 * filename: 用户指定的文件名（不含路径）
 * depth: 当前递归深度
 * *base: 结果数组（若必须扩容则可更改）
 * *arraysize: 数组已分配长度（须扩容数组时可更改）
 * n: 数组中当前有效元素的个数
 *
 * 返回新数组长度（n 的新值），出错则 -1
 */
static int
ParseTzFile(const char *filename, int depth,
			tzEntry **base, int *arraysize, int n)
{
	char		share_path[MAXPGPATH];
	char		file_path[MAXPGPATH];
	FILE	   *tzFile;
	char		tzbuf[1024];
	char	   *line;
	tzEntry		tzentry;
	int			lineno = 0;
	bool		override = false;
	const char *p;

	/*
	 * 我们强制要求文件名全为字母字符。这可能
	 * 过于严格，但我们不想允许访问
	 * timezonesets 目录之外任何东西，比如 '/' *必须*
	 * 拒绝。
	 */
	for (p = filename; *p; p++)
	{
		if (!isalpha((unsigned char) *p))
		{
			/* 层 0 时就使用 guc.c 的常规 "无效值" 消息 */
			if (depth > 0)
				GUC_check_errmsg("invalid time zone file name \"%s\"",
								 filename);
			return -1;
		}
	}

	/*
	 * 最大递归深度是一个相当随意的设定。很难想象
	 * 有人会需要多于 3 层的深度，所以维持这个
	 * 保守设定，直到有人抱怨为止。
	 */
	if (depth > 3)
	{
		GUC_check_errmsg("time zone file recursion limit exceeded in file \"%s\"",
						 filename);
		return -1;
	}

	get_share_path(my_exec_path, share_path);
	snprintf(file_path, sizeof(file_path), "%s/timezonesets/%s",
			 share_path, filename);
	tzFile = AllocateFile(file_path, "r");
	if (!tzFile)
	{
		/*
		 * 检查问题是否不在文件名而在目录。
		 * 这值得费心，因为若安装的 share/ 目录不存在或
		 * 不可读，这很可能是 postmaster 启动期间
		 * 我们最先注意到问题的地方。
		 */
		int			save_errno = errno;
		DIR		   *tzdir;

		snprintf(file_path, sizeof(file_path), "%s/timezonesets",
				 share_path);
		tzdir = AllocateDir(file_path);
		if (tzdir == NULL)
		{
			GUC_check_errmsg("could not open directory \"%s\": %m",
							 file_path);
			GUC_check_errhint("This may indicate an incomplete PostgreSQL installation, or that the file \"%s\" has been moved away from its proper location.",
							  my_exec_path);
			return -1;
		}
		FreeDir(tzdir);
		errno = save_errno;

		/*
		 * 否则，若文件不存在且处于层 0，guc.c 的
		 * 报错就足够了
		 */
		if (errno != ENOENT || depth > 0)
			GUC_check_errmsg("could not read time zone file \"%s\": %m",
							 filename);

		return -1;
	}

	while (!feof(tzFile))
	{
		lineno++;
		if (fgets(tzbuf, sizeof(tzbuf), tzFile) == NULL)
		{
			if (ferror(tzFile))
			{
				GUC_check_errmsg("could not read time zone file \"%s\": %m",
								 filename);
				n = -1;
				break;
			}
			/* 否则终究已到 EOF */
			break;
		}
		if (strlen(tzbuf) == sizeof(tzbuf) - 1)
		{
			/* 该行对 tzbuf 来说太长 */
			GUC_check_errmsg("line is too long in time zone file \"%s\", line %d",
							 filename, lineno);
			n = -1;
			break;
		}

		/* 跳过空白 */
		line = tzbuf;
		while (*line && isspace((unsigned char) *line))
			line++;

		if (*line == '\0')		/* 空行 */
			continue;
		if (*line == '#')		/* 注释行 */
			continue;

		if (pg_strncasecmp(line, "@INCLUDE", strlen("@INCLUDE")) == 0)
		{
			/* pstrdup 以便在结果数据结构中使用 filename */
			char	   *includeFile = pstrdup(line + strlen("@INCLUDE"));

			includeFile = strtok(includeFile, WHITESPACE);
			if (!includeFile || !*includeFile)
			{
				GUC_check_errmsg("@INCLUDE without file name in time zone file \"%s\", line %d",
								 filename, lineno);
				n = -1;
				break;
			}
			n = ParseTzFile(includeFile, depth + 1,
							base, arraysize, n);
			if (n < 0)
				break;
			continue;
		}

		if (pg_strncasecmp(line, "@OVERRIDE", strlen("@OVERRIDE")) == 0)
		{
			override = true;
			continue;
		}

		if (!splitTzLine(filename, lineno, line, &tzentry))
		{
			n = -1;
			break;
		}
		if (!validateTzEntry(&tzentry))
		{
			n = -1;
			break;
		}
		n = addToArray(base, arraysize, n, &tzentry, override);
		if (n < 0)
			break;
	}

	FreeFile(tzFile);

	return n;
}

/*
 * load_tzoffsets --- 读取并解析指定的时区偏移文件
 *
 * 成功时返回一个填好的 TimeZoneAbbrevTable，它必须
 * 是 guc_malloc'd 而非 palloc'd 的。失败时返回 NULL，用
 * GUC_check_errmsg 等给出问题详情。
 */
TimeZoneAbbrevTable *
load_tzoffsets(const char *filename)
{
	TimeZoneAbbrevTable *result = NULL;
	MemoryContext tmpContext;
	MemoryContext oldContext;
	tzEntry    *array;
	int			arraysize;
	int			n;

	/*
	 * 创建一个临时内存上下文供工作使用。这样便于
	 * 清理。
	 */
	tmpContext = AllocSetContextCreate(CurrentMemoryContext,
									   "TZParserMemory",
									   ALLOCSET_SMALL_SIZES);
	oldContext = MemoryContextSwitchTo(tmpContext);

	/* 以合理大小初始化数组 */
	arraysize = 128;
	array = (tzEntry *) palloc(arraysize * sizeof(tzEntry));

	/* 解析 file(s) */
	n = ParseTzFile(filename, 0, &array, &arraysize, 0);

	/* 若至今无错，让 datetime.c 分配内存并转换格式 */
	if (n >= 0)
	{
		result = ConvertTimeZoneAbbrevs(array, n);
		if (!result)
			GUC_check_errmsg("out of memory");
	}

	/* 清理 */
	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(tmpContext);

	return result;
}
