/*--------------------------------------------------------------------
 * conffiles.c
 *
 * 与配置文件处理相关的工具函数。
 *
 * 本文件包含一些处理 PostgreSQL 所用配置文件的
 * 通用工具，无论它们与 GUCs 还是认证相关。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/conffiles.c
 *
 *--------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>

#include "common/file_utils.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/conffiles.h"

/*
 * AbsoluteConfigLocation
 *
 * 给定一个可能是相对路径的配置文件或目录位置，
 * 返回一个绝对路径。我们认为该位置相对于持有调
 * 用文件的目录，若无调用文件则相对于 DataDir。
 */
char *
AbsoluteConfigLocation(const char *location, const char *calling_file)
{
	if (is_absolute_path(location))
		return pstrdup(location);
	else
	{
		char		abs_path[MAXPGPATH];

		if (calling_file != NULL)
		{
			strlcpy(abs_path, calling_file, sizeof(abs_path));
			get_parent_directory(abs_path);
			join_path_components(abs_path, abs_path, location);
			canonicalize_path(abs_path);
		}
		else
		{
			Assert(DataDir);
			join_path_components(abs_path, DataDir, location);
			canonicalize_path(abs_path);
		}
		return pstrdup(abs_path);
	}
}


/*
 * GetConfFilesInDir
 *
 * 返回位于某目录中的配置文件列表，按字母顺序排
 * 列。出错时返回 NULL，并把错误详情存入
 * "err_msg"
 */
char	  **
GetConfFilesInDir(const char *includedir, const char *calling_file,
				  int elevel, int *num_filenames, char **err_msg)
{
	char	   *directory;
	DIR		   *d;
	struct dirent *de;
	char	  **filenames = NULL;
	int			size_filenames;

	/*
	 * 拒绝全为空白（含空串）的目录名，因为这会导
	 * 致混淆 --- 我们会读取其所在目录，通常导致同样
	 * 的 file(s) 被递归包含。
	 */
	if (strspn(includedir, " \t\r\n") == strlen(includedir))
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty configuration directory name: \"%s\"",
						includedir)));
		*err_msg = "empty configuration directory name";
		return NULL;
	}

	directory = AbsoluteConfigLocation(includedir, calling_file);
	d = AllocateDir(directory);
	if (d == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open configuration directory \"%s\": %m",
						directory)));
		*err_msg = psprintf("could not open directory \"%s\"", directory);
		goto cleanup;
	}

	/*
	 * 读取目录并把文件名放入数组，以便调用者在处理
	 * 其内容之前先对它们排序。
	 */
	size_filenames = 32;
	filenames = (char **) palloc(size_filenames * sizeof(char *));
	*num_filenames = 0;

	while ((de = ReadDir(d, directory)) != NULL)
	{
		PGFileType	de_type;
		char		filename[MAXPGPATH];

		/*
		 * 只解析文件名以 ".conf" 结尾的文件。显式拒绝
		 * 以 "." 开头的文件。这排除了 "." 和 ".." 之类，
		 * 以及典型的隐藏文件、备份文件和编辑器残渣。
		 */
		if (strlen(de->d_name) < 6)
			continue;
		if (de->d_name[0] == '.')
			continue;
		if (strcmp(de->d_name + strlen(de->d_name) - 5, ".conf") != 0)
			continue;

		join_path_components(filename, directory, de->d_name);
		canonicalize_path(filename);
		de_type = get_dirent_type(filename, de, true, elevel);
		if (de_type == PGFILETYPE_ERROR)
		{
			*err_msg = psprintf("could not stat file \"%s\"", filename);
			pfree(filenames);
			filenames = NULL;
			goto cleanup;
		}
		else if (de_type != PGFILETYPE_DIR)
		{
			/* 把文件加入数组，数组以 32 为增量扩容 */
			if (*num_filenames >= size_filenames)
			{
				size_filenames += 32;
				filenames = (char **) repalloc(filenames,
											   size_filenames * sizeof(char *));
			}
			filenames[*num_filenames] = pstrdup(filename);
			(*num_filenames)++;
		}
	}

	/* 返回前按名称对文件排序 */
	if (*num_filenames > 0)
		qsort(filenames, *num_filenames, sizeof(char *), pg_qsort_strcmp);

cleanup:
	if (d)
		FreeDir(d);
	pfree(directory);
	return filenames;
}
