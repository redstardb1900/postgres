/*-------------------------------------------------------------------------
 *
 * fe-print.c
 *	  functions for pretty-printing query results
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * These functions were formerly part of fe-exec.c, but they
 * didn't really belong there.
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-print.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <signal.h>

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#else
#ifndef WIN32
#include <sys/termios.h>
#endif
#endif

#include "libpq-fe.h"
#include "libpq-int.h"


static bool do_field(const PQprintOpt *po, const PGresult *res,
					 const int i, const int j, const int fs_len,
					 char **fields,
					 const int nFields, const char **fieldNames,
					 unsigned char *fieldNotNum, int *fieldMax,
					 const int fieldMaxLen, FILE *fout);
static char *do_header(FILE *fout, const PQprintOpt *po, const int nFields,
					   int *fieldMax, const char **fieldNames, unsigned char *fieldNotNum,
					   const int fs_len, const PGresult *res);
static void output_row(FILE *fout, const PQprintOpt *po, const int nFields, char **fields,
					   unsigned char *fieldNotNum, int *fieldMax, char *border,
					   const int row_index);
static void fill(int length, int max, char filler, FILE *fp);

/*PQprint()
 * 
 *格式化查询结果以供打印。
 * 
 *PQprintOpt 是一个 typedef（结构体），包含各种标志
 * 和选项。详情请参阅 libpq-fe.h。
 * 
 * 此函数可能应该在某时移除，因为 psql 不再使用它。
 *然而，外部客户端在多大程度上使用它尚不清楚。
 * 
 * 
 * 
                       */
void
PQprint(FILE *fout, const PGresult *res, const PQprintOpt *po)
{
	int			nFields;

	nFields = PQnfields(res);

	if (nFields > 0)
	{							/* only print rows with at least 1 field.  */
		int			i,
					j;
		int			nTups;
		int		   *fieldMax = NULL;	/* in case we don't use them */
		unsigned char *fieldNotNum = NULL;
		char	   *border = NULL;
		char	  **fields = NULL;
		const char **fieldNames = NULL;
		int			fieldMaxLen = 0;
		int			numFieldName;
		int			fs_len = strlen(po->fieldSep);
		int			total_line_length = 0;
		bool		usePipe = false;
		char	   *pagerenv;

#if defined(ENABLE_THREAD_SAFETY) && !defined(WIN32)
		sigset_t	osigset;
		bool		sigpipe_masked = false;
		bool		sigpipe_pending;
#endif
#if !defined(ENABLE_THREAD_SAFETY) && !defined(WIN32)
		pqsigfunc	oldsigpipehandler = NULL;
#endif

#ifdef TIOCGWINSZ
		struct winsize screen_size;
#else
		struct winsize
		{
			int			ws_row;
			int			ws_col;
		}			screen_size;
#endif

		/*快速检查 po->fieldSep 的合理性，因为我们大量使用整数运算。
	
	
	            */
		if (fs_len < strlen(po->fieldSep))
		{
			fprintf(stderr, libpq_gettext("overlong field separator\n"));
			goto exit;
		}

		nTups = PQntuples(res);
		fieldNames = (const char **) calloc(nFields, sizeof(char *));
		fieldNotNum = (unsigned char *) calloc(nFields, 1);
		fieldMax = (int *) calloc(nFields, sizeof(int));
		if (!fieldNames || !fieldNotNum || !fieldMax)
		{
			fprintf(stderr, libpq_gettext("out of memory\n"));
			goto exit;
		}
		for (numFieldName = 0;
			 po->fieldName && po->fieldName[numFieldName];
			 numFieldName++)
			;
		for (j = 0; j < nFields; j++)
		{
			int			len;
			const char *s = (j < numFieldName && po->fieldName[j][0]) ?
				po->fieldName[j] : PQfname(res, j);

			fieldNames[j] = s;
			len = s ? strlen(s) : 0;
			fieldMax[j] = len;
			len += fs_len;
			if (len > fieldMaxLen)
				fieldMaxLen = len;
			total_line_length += len;
		}

		total_line_length += nFields * strlen(po->fieldSep) + 1;

		if (fout == NULL)
			fout = stdout;
		if (po->pager && fout == stdout && isatty(fileno(stdin)) &&
			isatty(fileno(stdout)))
		{
			/*如果认为输出会超过一屏，尝试管道到分页程序。
	
	
	                                   */
#ifdef TIOCGWINSZ
			if (ioctl(fileno(stdout), TIOCGWINSZ, &screen_size) == -1 ||
				screen_size.ws_col == 0 ||
				screen_size.ws_row == 0)
			{
				screen_size.ws_row = 24;
				screen_size.ws_col = 80;
			}
#else
			screen_size.ws_row = 24;
			screen_size.ws_col = 80;
#endif

			/*由于此函数不再被 psql 使用，我们不检查 PSQL_PAGER。
	假设的外部用户可能希望这样做，但为了向后兼容，我们仅
	检查 PAGER。
	
	
	                                                                                                              */
			pagerenv = getenv("PAGER");
			/* if PAGER is unset, empty or all-white-space, don't use pager */
			if (pagerenv != NULL &&
				strspn(pagerenv, " \t\r\n") != strlen(pagerenv) &&
				!po->html3 &&
				((po->expanded &&
				  nTups * (nFields + 1) >= screen_size.ws_row) ||
				 (!po->expanded &&
				  nTups * (total_line_length / screen_size.ws_col + 1) *
				  (1 + (po->standard != 0)) >= screen_size.ws_row -
				  (po->header != 0) *
				  (total_line_length / screen_size.ws_col + 1) * 2
				  - (po->header != 0) * 2	/* row count and newline */
				  )))
			{
				fflush(NULL);
				fout = popen(pagerenv, "w");
				if (fout)
				{
					usePipe = true;
#ifndef WIN32
#ifdef ENABLE_THREAD_SAFETY
					if (pq_block_sigpipe(&osigset, &sigpipe_pending) == 0)
						sigpipe_masked = true;
#else
					oldsigpipehandler = pqsignal(SIGPIPE, SIG_IGN);
#endif							/* ENABLE_THREAD_SAFETY */
#endif							/* WIN32 */
				}
				else
					fout = stdout;
			}
		}

		if (!po->expanded && (po->align || po->html3))
		{
			fields = (char **) calloc((size_t) nTups + 1,
									  nFields * sizeof(char *));
			if (!fields)
			{
				fprintf(stderr, libpq_gettext("out of memory\n"));
				goto exit;
			}
		}
		else if (po->header && !po->html3)
		{
			if (po->expanded)
			{
				if (po->align)
					fprintf(fout, libpq_gettext("%-*s%s Value\n"),
							fieldMaxLen - fs_len, libpq_gettext("Field"), po->fieldSep);
				else
					fprintf(fout, libpq_gettext("%s%sValue\n"), libpq_gettext("Field"), po->fieldSep);
			}
			else
			{
				int			len = 0;

				for (j = 0; j < nFields; j++)
				{
					const char *s = fieldNames[j];

					fputs(s, fout);
					len += strlen(s) + fs_len;
					if ((j + 1) < nFields)
						fputs(po->fieldSep, fout);
				}
				fputc('\n', fout);
				for (len -= fs_len; len--; fputc('-', fout));
				fputc('\n', fout);
			}
		}
		if (po->expanded && po->html3)
		{
			if (po->caption)
				fprintf(fout, "<center><h2>%s</h2></center>\n", po->caption);
			else
				fprintf(fout,
						"<center><h2>"
						"Query retrieved %d rows * %d fields"
						"</h2></center>\n",
						nTups, nFields);
		}
		for (i = 0; i < nTups; i++)
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout,
							"<table %s><caption align=\"top\">%d</caption>\n",
							po->tableOpt ? po->tableOpt : "", i);
				else
					fprintf(fout, libpq_gettext("-- RECORD %d --\n"), i);
			}
			for (j = 0; j < nFields; j++)
			{
				if (!do_field(po, res, i, j, fs_len, fields, nFields,
							  fieldNames, fieldNotNum,
							  fieldMax, fieldMaxLen, fout))
					goto exit;
			}
			if (po->html3 && po->expanded)
				fputs("</table>\n", fout);
		}
		if (!po->expanded && (po->align || po->html3))
		{
			if (po->html3)
			{
				if (po->header)
				{
					if (po->caption)
						fprintf(fout,
								"<table %s><caption align=\"top\">%s</caption>\n",
								po->tableOpt ? po->tableOpt : "",
								po->caption);
					else
						fprintf(fout,
								"<table %s><caption align=\"top\">"
								"Retrieved %d rows * %d fields"
								"</caption>\n",
								po->tableOpt ? po->tableOpt : "", nTups, nFields);
				}
				else
					fprintf(fout, "<table %s>", po->tableOpt ? po->tableOpt : "");
			}
			if (po->header)
				border = do_header(fout, po, nFields, fieldMax, fieldNames,
								   fieldNotNum, fs_len, res);
			for (i = 0; i < nTups; i++)
				output_row(fout, po, nFields, fields,
						   fieldNotNum, fieldMax, border, i);
		}
		if (po->header && !po->html3)
			fprintf(fout, "(%d row%s)\n\n", PQntuples(res),
					(PQntuples(res) == 1) ? "" : "s");
		if (po->html3 && !po->expanded)
			fputs("</table>\n", fout);

exit:
		free(fieldMax);
		free(fieldNotNum);
		free(border);
		if (fields)
		{
			/* if calloc succeeded, this shouldn't overflow size_t */
			size_t		numfields = ((size_t) nTups + 1) * (size_t) nFields;

			while (numfields-- > 0)
				free(fields[numfields]);
			free(fields);
		}
		free(fieldNames);
		if (usePipe)
		{
#ifdef WIN32
			_pclose(fout);
#else
			pclose(fout);

#ifdef ENABLE_THREAD_SAFETY
			/* we can't easily verify if EPIPE occurred, so say it did */
			if (sigpipe_masked)
				pq_reset_sigpipe(&osigset, sigpipe_pending, true);
#else
			pqsignal(SIGPIPE, oldsigpipehandler);
#endif							/* ENABLE_THREAD_SAFETY */
#endif							/* WIN32 */
		}
	}
}


static bool
do_field(const PQprintOpt *po, const PGresult *res,
		 const int i, const int j, const int fs_len,
		 char **fields,
		 const int nFields, char const **fieldNames,
		 unsigned char *fieldNotNum, int *fieldMax,
		 const int fieldMaxLen, FILE *fout)
{
	const char *pval,
			   *p;
	int			plen;
	bool		skipit;

	plen = PQgetlength(res, i, j);
	pval = PQgetvalue(res, i, j);

	if (plen < 1 || !pval || !*pval)
	{
		if (po->align || po->expanded)
			skipit = true;
		else
		{
			skipit = false;
			goto efield;
		}
	}
	else
		skipit = false;

	if (!skipit)
	{
		if (po->align && !fieldNotNum[j])
		{
			/* 检测字段是否包含非数值数据        */
			char		ch = '0';

			for (p = pval; *p; p += PQmblenBounded(p, res->client_encoding))
			{
				ch = *p;
				if (!((ch >= '0' && ch <= '9') ||
					  ch == '.' ||
					  ch == 'E' ||
					  ch == 'e' ||
					  ch == ' ' ||
					  ch == '-'))
				{
					fieldNotNum[j] = 1;
					break;
				}
			}

			/*上述循环会认为第一列的 E 是数值；此外，我们坚持要求
	数值的最后一列是数字。此测试仍非万无一失，但能处理大多数
	情况。
	
	                        */
			if (*pval == 'E' || *pval == 'e' ||
				!(ch >= '0' && ch <= '9'))
				fieldNotNum[j] = 1;
		}

		if (!po->expanded && (po->align || po->html3))
		{
			if (plen > fieldMax[j])
				fieldMax[j] = plen;
			if (!(fields[i * nFields + j] = (char *) malloc((size_t) plen + 1)))
			{
				fprintf(stderr, libpq_gettext("out of memory\n"));
				return false;
			}
			strcpy(fields[i * nFields + j], pval);
		}
		else
		{
			if (po->expanded)
			{
				if (po->html3)
					fprintf(fout,
							"<tr><td align=\"left\"><b>%s</b></td>"
							"<td align=\"%s\">%s</td></tr>\n",
							fieldNames[j],
							fieldNotNum[j] ? "left" : "right",
							pval);
				else
				{
					if (po->align)
						fprintf(fout,
								"%-*s%s %s\n",
								fieldMaxLen - fs_len, fieldNames[j],
								po->fieldSep,
								pval);
					else
						fprintf(fout,
								"%s%s%s\n",
								fieldNames[j], po->fieldSep, pval);
				}
			}
			else
			{
				if (!po->html3)
				{
					fputs(pval, fout);
			efield:
					if ((j + 1) < nFields)
						fputs(po->fieldSep, fout);
					else
						fputc('\n', fout);
				}
			}
		}
	}
	return true;
}


/*后端 add_size() 的前端版本，旨在与 pg_add_*_overflow()
 * 辅助函数保持 API 兼容。成功时将结果存储到 *dst 中。
 * 如果加法溢出则返回 true。
 * 
 *TODO: 移到 common/int.h
 * 
                              */
static bool
add_size_overflow(size_t s1, size_t s2, size_t *dst)
{
	size_t		result;

	result = s1 + s2;
	if (result < s1 || result < s2)
		return true;

	*dst = result;
	return false;
}


static char *
do_header(FILE *fout, const PQprintOpt *po, const int nFields, int *fieldMax,
		  const char **fieldNames, unsigned char *fieldNotNum,
		  const int fs_len, const PGresult *res)
{
	int			j;				/* 循环索引   */
	char	   *border = NULL;

	if (po->html3)
		fputs("<tr>", fout);
	else
	{
		size_t		tot = 0;
		int			n = 0;
		char	   *p = NULL;

		/* 计算边框大小，检查溢出。              */
		for (; n < nFields; n++)
		{
			/* Field plus separator, plus 2 extra '-' in standard format. */
			if (add_size_overflow(tot, fieldMax[n], &tot) ||
				add_size_overflow(tot, fs_len, &tot) ||
				(po->standard && add_size_overflow(tot, 2, &tot)))
				goto overflow;
		}
		if (po->standard)
		{
			/* 前后各加一个额外的分隔符。   */
			if (add_size_overflow(tot, fs_len, &tot) ||
				add_size_overflow(tot, fs_len, &tot) ||
				add_size_overflow(tot, 2, &tot))
				goto overflow;
		}
		if (add_size_overflow(tot, 1, &tot))	/* 终止符  */
			goto overflow;

		border = malloc(tot);
		if (!border)
		{
			fprintf(stderr, libpq_gettext("out of memory\n"));
			return NULL;
		}
		p = border;
		if (po->standard)
		{
			char	   *fs = po->fieldSep;

			while (*fs++)
				*p++ = '+';
		}
		for (j = 0; j < nFields; j++)
		{
			int			len;

			for (len = fieldMax[j] + (po->standard ? 2 : 0); len--; *p++ = '-');
			if (po->standard || (j + 1) < nFields)
			{
				char	   *fs = po->fieldSep;

				while (*fs++)
					*p++ = '+';
			}
		}
		*p = '\0';
		if (po->standard)
			fprintf(fout, "%s\n", border);
	}
	if (po->standard)
		fputs(po->fieldSep, fout);
	for (j = 0; j < nFields; j++)
	{
		const char *s = PQfname(res, j);

		if (po->html3)
		{
			fprintf(fout, "<th align=\"%s\">%s</th>",
					fieldNotNum[j] ? "left" : "right", fieldNames[j]);
		}
		else
		{
			int			n = strlen(s);

			if (n > fieldMax[j])
				fieldMax[j] = n;
			if (po->standard)
				fprintf(fout,
						fieldNotNum[j] ? " %-*s " : " %*s ",
						fieldMax[j], s);
			else
				fprintf(fout, fieldNotNum[j] ? "%-*s" : "%*s", fieldMax[j], s);
			if (po->standard || (j + 1) < nFields)
				fputs(po->fieldSep, fout);
		}
	}
	if (po->html3)
		fputs("</tr>\n", fout);
	else
		fprintf(fout, "\n%s\n", border);
	return border;

overflow:
	fprintf(stderr, libpq_gettext("header size exceeds the maximum allowed\n"));
	return NULL;
}


static void
output_row(FILE *fout, const PQprintOpt *po, const int nFields, char **fields,
		   unsigned char *fieldNotNum, int *fieldMax, char *border,
		   const int row_index)
{
	int			field_index;	/* 循环索引   */

	if (po->html3)
		fputs("<tr>", fout);
	else if (po->standard)
		fputs(po->fieldSep, fout);
	for (field_index = 0; field_index < nFields; field_index++)
	{
		char	   *p = fields[row_index * nFields + field_index];

		if (po->html3)
			fprintf(fout, "<td align=\"%s\">%s</td>",
					fieldNotNum[field_index] ? "left" : "right", p ? p : "");
		else
		{
			fprintf(fout,
					fieldNotNum[field_index] ?
					(po->standard ? " %-*s " : "%-*s") :
					(po->standard ? " %*s " : "%*s"),
					fieldMax[field_index],
					p ? p : "");
			if (po->standard || field_index + 1 < nFields)
				fputs(po->fieldSep, fout);
		}
	}
	if (po->html3)
		fputs("</tr>", fout);
	else if (po->standard)
		fprintf(fout, "\n%s", border);
	fputc('\n', fout);
}



/*非常旧的打印例程
 * 
     */

void
PQdisplayTuples(const PGresult *res,
				FILE *fp,		/* 输出发送到哪里    */
				int fillAlign,	/* 用空格填充字段      */
				const char *fieldSep,	/* 字段分隔符 */
				int printHeader,	/* 显示表头？  */
				int quiet
)
{
#define DEFAULT_FIELD_SEP " "

	int			i,
				j;
	int			nFields;
	int			nTuples;
	int		   *fLength = NULL;

	if (fieldSep == NULL)
		fieldSep = DEFAULT_FIELD_SEP;

	/* Get some useful info about the results */
	nFields = PQnfields(res);
	nTuples = PQntuples(res);

	if (fp == NULL)
		fp = stdout;

	/* Figure the field lengths to align to */
	/* will be somewhat time consuming for very large results */
	if (fillAlign)
	{
		fLength = (int *) malloc(nFields * sizeof(int));
		if (!fLength)
		{
			fprintf(stderr, libpq_gettext("out of memory\n"));
			return;
		}

		for (j = 0; j < nFields; j++)
		{
			fLength[j] = strlen(PQfname(res, j));
			for (i = 0; i < nTuples; i++)
			{
				int			flen = PQgetlength(res, i, j);

				if (flen > fLength[j])
					fLength[j] = flen;
			}
		}
	}

	if (printHeader)
	{
		/* 首先，打印属性名称          */
		for (i = 0; i < nFields; i++)
		{
			fputs(PQfname(res, i), fp);
			if (fillAlign)
				fill(strlen(PQfname(res, i)), fLength[i], ' ', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");

		/* 在属性名称下划线      */
		for (i = 0; i < nFields; i++)
		{
			if (fillAlign)
				fill(0, fLength[i], '-', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");
	}

	/* 接下来，打印实例      */
	for (i = 0; i < nTuples; i++)
	{
		for (j = 0; j < nFields; j++)
		{
			fprintf(fp, "%s", PQgetvalue(res, i, j));
			if (fillAlign)
				fill(strlen(PQgetvalue(res, i, j)), fLength[j], ' ', fp);
			fputs(fieldSep, fp);
		}
		fprintf(fp, "\n");
	}

	if (!quiet)
		fprintf(fp, "\nQuery returned %d row%s.\n", PQntuples(res),
				(PQntuples(res) == 1) ? "" : "s");

	fflush(fp);

	free(fLength);
}



void
PQprintTuples(const PGresult *res,
			  FILE *fout,		/* 输出流     */
			  int PrintAttNames,	/* 是否打印属性名称     */
			  int TerseOutput,	/* delimiter bars or not? */
			  int colWidth		/* width of column, if 0, use variable width */
)
{
	int			nFields;
	int			nTups;
	int			i,
				j;
	char		formatString[80];
	char	   *tborder = NULL;

	nFields = PQnfields(res);
	nTups = PQntuples(res);

	if (colWidth > 0)
		sprintf(formatString, "%%s %%-%ds", colWidth);
	else
		sprintf(formatString, "%%s %%s");

	if (nFields > 0)
	{							/* only print rows with at least 1 field.  */

		if (!TerseOutput)
		{
			int			width;

			width = nFields * 14;
			tborder = (char *) malloc(width + 1);
			if (!tborder)
			{
				fprintf(stderr, libpq_gettext("out of memory\n"));
				return;
			}
			for (i = 0; i < width; i++)
				tborder[i] = '-';
			tborder[width] = '\0';
			fprintf(fout, "%s\n", tborder);
		}

		for (i = 0; i < nFields; i++)
		{
			if (PrintAttNames)
			{
				fprintf(fout, formatString,
						TerseOutput ? "" : "|",
						PQfname(res, i));
			}
		}

		if (PrintAttNames)
		{
			if (TerseOutput)
				fprintf(fout, "\n");
			else
				fprintf(fout, "|\n%s\n", tborder);
		}

		for (i = 0; i < nTups; i++)
		{
			for (j = 0; j < nFields; j++)
			{
				const char *pval = PQgetvalue(res, i, j);

				fprintf(fout, formatString,
						TerseOutput ? "" : "|",
						pval ? pval : "");
			}
			if (TerseOutput)
				fprintf(fout, "\n");
			else
				fprintf(fout, "|\n%s\n", tborder);
		}
	}

	free(tborder);
}


/* 简单地向 fp 发送最大长度数量的填充字符      */

static void
fill(int length, int max, char filler, FILE *fp)
{
	int			count;

	count = max - length;
	while (count-- >= 0)
		putc(filler, fp);
}
