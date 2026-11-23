/*-------------------------------------------------------------------------
 *
 * ts_utils.c
 *		various support functions
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/ts_utils.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "miscadmin.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_utils.h"


/*
 * 给定 tsearch 配置文件基名与扩展名,返回其
 * 完整路径名。基名视为用户供给,须检查以防
 * 路径攻击。扩展名假定
 * 安全。
 *
 * 结果为 palloc 的字符串。
 */
char *
get_tsearch_config_filename(const char *basename,
							const char *extension)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	/*
	 * 基名限含 a-z、0-9 与下划线。或过严,但我们不想
	 * 允许访问 tsearch_data 目录外任何内容,
	 * 故例如 '/' *必须* 拒绝,且某些平台上 '\' 与 ':'
	 * 也有风险。允许大写可能使大小写敏感与不敏感
	 * 文件系统行为不一,非 ASCII 字符带来其他有趣
	 * 风险,故整体以
	 * 紧策略为妥。
	 */
	if (strspn(basename, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(basename))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid text search configuration file name \"%s\"",
						basename)));

	get_share_path(my_exec_path, sharepath);
	result = palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/tsearch_data/%s.%s",
			 sharepath, basename, extension);

	return result;
}

/*
 * 读停用词文件。每个词若给了 wordop 则经其
 * 处理。wordop 可原地改输入,或
 * palloc 新版。
 */
void
readstoplist(const char *fname, StopList *s, char *(*wordop) (const char *))
{
	char	  **stop = NULL;

	s->len = 0;
	if (fname && *fname)
	{
		char	   *filename = get_tsearch_config_filename(fname, "stop");
		tsearch_readline_state trst;
		char	   *line;
		int			reallen = 0;

		if (!tsearch_readline_begin(&trst, filename))
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not open stop-word file \"%s\": %m",
							filename)));

		while ((line = tsearch_readline(&trst)) != NULL)
		{
			char	   *pbuf = line;

			/* 去掉尾部空白 */
			while (*pbuf && !t_isspace(pbuf))
				pbuf += pg_mblen(pbuf);
			*pbuf = '\0';

			/* 跳过空行 */
			if (*line == '\0')
			{
				pfree(line);
				continue;
			}

			if (s->len >= reallen)
			{
				if (reallen == 0)
				{
					reallen = 64;
					stop = (char **) palloc(sizeof(char *) * reallen);
				}
				else
				{
					reallen *= 2;
					stop = (char **) repalloc(stop, sizeof(char *) * reallen);
				}
			}

			if (wordop)
			{
				stop[s->len] = wordop(line);
				if (stop[s->len] != line)
					pfree(line);
			}
			else
				stop[s->len] = line;

			(s->len)++;
		}

		tsearch_readline_end(&trst);
		pfree(filename);
	}

	s->stop = stop;

	/* 排序以便二分查找 */
	if (s->stop && s->len > 0)
		qsort(s->stop, s->len, sizeof(char *), pg_qsort_strcmp);
}

bool
searchstoplist(StopList *s, char *key)
{
	return (s->stop && s->len > 0 &&
			bsearch(&key, s->stop, s->len,
					sizeof(char *), pg_qsort_strcmp));
}
