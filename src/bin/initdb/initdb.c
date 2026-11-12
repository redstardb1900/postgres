/*-------------------------------------------------------------------------
 *
 * initdb --- initialize a PostgreSQL installation
 *
 * initdb creates (initializes) a PostgreSQL database cluster (site,
 * instance, installation, whatever).  A database cluster is a
 * collection of PostgreSQL databases all managed by the same server.
 *
 * To create the database cluster, we create the directory that contains
 * all its data, create the files that hold the global tables, create
 * a few other control files for it, and create three databases: the
 * template databases "template0" and "template1", and a default user
 * database "postgres".
 *
 * The template databases are ordinary PostgreSQL databases.  template0
 * is never supposed to change after initdb, whereas template1 can be
 * changed to add site-local standard data.  Either one can be copied
 * to produce a new database.
 *
 * For largely-historical reasons, the template1 database is the one built
 * by the basic bootstrap process.  After it is complete, template0 and
 * the default database, postgres, are made just by copying template1.
 *
 * To create template1, we run the postgres (backend) program in bootstrap
 * mode and feed it data from the postgres.bki library file.  After this
 * initial bootstrap phase, some additional stuff is created by normal
 * SQL commands fed to a standalone backend.  Some of those commands are
 * just embedded into this program (yeah, it's ugly), but larger chunks
 * are taken from script files.
 *
 *
 * Note:
 *	 The program has some memory leakage - it isn't worth cleaning it up.
 *
 * This is a C implementation of the previous shell script for setting up a
 * PostgreSQL cluster location, and should be highly compatible with it.
 * author of C translation: Andrew Dunstan	   mailto:andrew@dunslane.net
 *
 * This code is released under the terms of the PostgreSQL License.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/initdb/initdb.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef USE_ICU
#include <unicode/ucol.h>
#endif
#include <unistd.h>
#include <signal.h>
#include <time.h>

#ifdef HAVE_SHM_OPEN
#include "sys/mman.h"
#endif

#include "access/xlog_internal.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_class_d.h" /* pgrminclude 忽略 */
#include "catalog/pg_collation_d.h"
#include "catalog/pg_database_d.h"	/* pgrminclude 忽略 */
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "common/pg_prng.h"
#include "common/restricted_token.h"
#include "common/string.h"
#include "common/username.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"


/* 理想情况下它本应放在.h 文件中，但这几乎不值得麻烦  */
extern const char *select_default_timezone(const char *share_path);

/* 简单字符串列表  */
typedef struct _stringlist
{
	char	   *str;
	struct _stringlist *next;
} _stringlist;

static const char *const auth_methods_host[] = {
	"trust", "reject", "scram-sha-256", "md5", "password", "ident", "radius",
#ifdef ENABLE_GSS
	"gss",
#endif
#ifdef ENABLE_SSPI
	"sspi",
#endif
#ifdef USE_PAM
	"pam", "pam ",
#endif
#ifdef USE_BSD_AUTH
	"bsd",
#endif
#ifdef USE_LDAP
	"ldap",
#endif
#ifdef USE_SSL
	"cert",
#endif
	NULL
};
static const char *const auth_methods_local[] = {
	"trust", "reject", "scram-sha-256", "md5", "password", "peer", "radius",
#ifdef USE_PAM
	"pam", "pam ",
#endif
#ifdef USE_BSD_AUTH
	"bsd",
#endif
#ifdef USE_LDAP
	"ldap",
#endif
	NULL
};

/*
 * 这些值通过 makefile 定义传入
          */
static char *share_path = NULL;

/* 从参数中获取的值             */
static char *pg_data = NULL;
static char *encoding = NULL;
static char *locale = NULL;
static char *lc_collate = NULL;
static char *lc_ctype = NULL;
static char *lc_monetary = NULL;
static char *lc_numeric = NULL;
static char *lc_time = NULL;
static char *lc_messages = NULL;
static char locale_provider = COLLPROVIDER_LIBC;
static char *icu_locale = NULL;
static char *icu_rules = NULL;
static const char *default_text_search_config = NULL;
static char *username = NULL;
static bool pwprompt = false;
static char *pwfilename = NULL;
static char *superuser_password = NULL;
static const char *authmethodhost = NULL;
static const char *authmethodlocal = NULL;
static _stringlist *extra_guc_names = NULL;
static _stringlist *extra_guc_values = NULL;
static bool debug = false;
static bool noclean = false;
static bool noinstructions = false;
static bool do_sync = true;
static bool sync_only = false;
static bool show_setting = false;
static bool data_checksums = false;
static char *xlog_dir = NULL;
static char *str_wal_segment_size_mb = NULL;
static int	wal_segment_size_mb;


/* 内部变量  */
static const char *progname;
static int	encodingid;
static char *bki_file;
static char *hba_file;
static char *ident_file;
static char *conf_file;
static char *dictionary_file;
static char *info_schema_file;
static char *features_file;
static char *system_constraints_file;
static char *system_functions_file;
static char *system_views_file;
static bool success = false;
static bool made_new_pgdata = false;
static bool found_existing_pgdata = false;
static bool made_new_xlogdir = false;
static bool found_existing_xlogdir = false;
static char infoversion[100];
static bool caught_signal = false;
static bool output_failed = false;
static int	output_errno = 0;
static char *pgdata_native;

/* 默认   */
static int	n_connections = 10;
static int	n_buffers = 50;
static const char *dynamic_shared_memory_type = NULL;
static const char *default_timezone = NULL;

/*
 * 认证方法的警告消息
                 */
#define AUTHTRUST_WARNING \
"# CAUTION: Configuring the system for local \"trust\" authentication\n" \
"# allows any local user to connect as any PostgreSQL user, including\n" \
"# the database superuser.  If you do not trust all your local users,\n" \
"# use another authentication method.\n"
static bool authwarning = false;

/*
 * 传递给后端的开关的集中管理
 *
 * 注意：我们使用 -F（禁用 fsync）运行后端，然后在最后进行一次
 * fsync 遍历。这比每一步都进行 fsync 更快。
 *
 * 注意：在 shell 脚本版本中，我们还通过 -D 开关传递 PGDATA，
 * 但在这里，将其作为环境变量传递更方便
 * （无需担心引号问题）。
                    */
static const char *boot_options = "-F -c log_checkpoints=false";
static const char *backend_options = "--single -F -O -j -c search_path=pg_catalog -c exit_on_error=true -c log_checkpoints=false";

/* 传递给后端的额外开关（引导或独立模式）          */
static char *extra_options = "";

static const char *const subdirs[] = {
	"global",
	"pg_wal/archive_status",
	"pg_commit_ts",
	"pg_dynshmem",
	"pg_notify",
	"pg_serial",
	"pg_snapshots",
	"pg_subtrans",
	"pg_twophase",
	"pg_multixact",
	"pg_multixact/members",
	"pg_multixact/offsets",
	"base",
	"base/1",
	"pg_replslot",
	"pg_tblspc",
	"pg_stat",
	"pg_stat_tmp",
	"pg_xact",
	"pg_logical",
	"pg_logical/snapshots",
	"pg_logical/mappings"
};


/* 'initdb' 二进制目录的路径 */
static char bin_path[MAXPGPATH];
static char backend_exec[MAXPGPATH];

static char **replace_token(char **lines,
							const char *token, const char *replacement);
static char **replace_guc_value(char **lines,
								const char *guc_name, const char *guc_value,
								bool mark_as_comment);
static bool guc_value_requires_quotes(const char *guc_value);
static char **readfile(const char *path);
static void writefile(char *path, char **lines);
static FILE *popen_check(const char *command, const char *mode);
static char *get_id(void);
static int	get_encoding_id(const char *encoding_name);
static void set_input(char **dest, const char *filename);
static void check_input(char *path);
static void write_version_file(const char *extrapath);
static void set_null_conf(void);
static void test_config_settings(void);
static bool test_specific_config_settings(int test_conns, int test_buffs);
static void setup_config(void);
static void bootstrap_template1(void);
static void setup_auth(FILE *cmdfd);
static void get_su_pwd(void);
static void setup_depend(FILE *cmdfd);
static void setup_run_file(FILE *cmdfd, const char *filename);
static void setup_description(FILE *cmdfd);
static void setup_collation(FILE *cmdfd);
static void setup_privileges(FILE *cmdfd);
static void set_info_version(void);
static void setup_schema(FILE *cmdfd);
static void load_plpgsql(FILE *cmdfd);
static void vacuum_db(FILE *cmdfd);
static void make_template0(FILE *cmdfd);
static void make_postgres(FILE *cmdfd);
static void trapsig(SIGNAL_ARGS);
static void check_ok(void);
static char *escape_quotes(const char *src);
static char *escape_quotes_bki(const char *src);
static int	locale_date_order(const char *locale);
static void check_locale_name(int category, const char *locale,
							  char **canonname);
static bool check_locale_encoding(const char *locale, int user_enc);
static void setlocales(void);
static void usage(const char *progname);
void		setup_pgdata(void);
void		setup_bin_paths(const char *argv0);
void		setup_data_file_paths(void);
void		setup_locale_encoding(void);
void		setup_signals(void);
void		setup_text_search(void);
void		create_data_directory(void);
void		create_xlog_or_symlink(void);
void		warn_on_mount_point(int error);
void		initialize_data_directory(void);

/*
 * 用于建立到postgres管道的宏 
 */
#define PG_CMD_DECL		char cmd[MAXPGPATH]; FILE *cmdfd

#define PG_CMD_OPEN \
do { \
	cmdfd = popen_check(cmd, "w"); \
	if (cmdfd == NULL) \
		exit(1); /* 消息已由 popen_check 打印        */ \
} while (0)

#define PG_CMD_CLOSE \
do { \
	if (pclose_check(cmdfd)) \
		exit(1); /* 消息已由 pclose_check 打印        */ \
} while (0)

#define PG_CMD_PUTS(line) \
do { \
	if (fputs(line, cmdfd) < 0 || fflush(cmdfd) < 0) \
		output_failed = true, output_errno = errno; \
} while (0)

#define PG_CMD_PRINTF(fmt, ...) \
do { \
	if (fprintf(cmdfd, fmt, __VA_ARGS__) < 0 || fflush(cmdfd) < 0) \
		output_failed = true, output_errno = errno; \
} while (0)

#ifdef WIN32
typedef wchar_t *save_locale_t;
#else
typedef char *save_locale_t;
#endif

/*
 * 保存当前全局区域设置名称的副本，用于给定类别。
 * 返回值必须传递给 restore_global_locale()。
 *
 * 因为环境中的名称未经过非 ASCII 字符验证，
 * 我们在 Windows 上使用 setlocale() 的 wchar_t 变体。否则，
 * 在保存-恢复往返中可能无法存活：恢复时，名称本身
 * 可能会被普通的 setlocale() 以不同的编码解释，
 * 当我们在其间切换到另一个区域设置时。（这只是 initdb 中的问题，
 * 在类似的后端代码中不会出现，因为全局区域设置的名称应该
 * 已经被验证为仅含 ASCII。）
                            */
static save_locale_t
save_global_locale(int category)
{
	save_locale_t save;

#ifdef WIN32
	save = _wsetlocale(category, NULL);
	if (!save)
		pg_fatal("_wsetlocale() failed");
	save = wcsdup(save);
	if (!save)
		pg_fatal("out of memory");
#else
	save = setlocale(category, NULL);
	if (!save)
		pg_fatal("setlocale() failed");
	save = pg_strdup(save);
#endif
	return save;
}

/*
 * 恢复 save_global_locale()返回的全局区域设置     
 */
static void
restore_global_locale(int category, save_locale_t save)
{
#ifdef WIN32
	if (!_wsetlocale(category, save))
		pg_fatal("failed to restore old locale");
#else
	if (!setlocale(category, save))
		pg_fatal("failed to restore old locale \"%s\"", save);
#endif
	free(save);
}

/*
 * 转义单引号和反斜杠，适用于插入到
 * 配置文件或 SQL E'' 字符串中。
                   */
static char *
escape_quotes(const char *src)
{
	char	   *result = escape_single_quotes_ascii(src);

	if (!result)
		pg_fatal("out of memory");
	return result;
}

/*
 * 转义要插入到 BKI 数据中的字段值。
 * 通过 escape_quotes 运行值（将由后端
 * 的 DeescapeQuotedString() 函数反转），然后将值
 * 用单引号括起来，即使这不是严格必需的。
                                  */
static char *
escape_quotes_bki(const char *src)
{
	char	   *result;
	char	   *data = escape_quotes(src);
	char	   *resultp;
	char	   *datap;

	result = (char *) pg_malloc(strlen(data) + 3);
	resultp = result;
	*resultp++ = '\'';
	for (datap = data; *datap; datap++)
		*resultp++ = *datap;
	*resultp++ = '\'';
	*resultp = '\0';

	free(data);
	return result;
}

/*
 * 在字符串列表末尾添加条目   
 */
static void
add_stringlist_item(_stringlist **listhead, const char *str)
{
	_stringlist *newentry = pg_malloc(sizeof(_stringlist));
	_stringlist *oldentry;

	newentry->str = pg_strdup(str);
	newentry->next = NULL;
	if (*listhead == NULL)
		*listhead = newentry;
	else
	{
		for (oldentry = *listhead; oldentry->next; oldentry = oldentry->next)
			 /* 略  */ ;
		oldentry->next = newentry;
	}
}

/*
 * 修改行数组，将"token"替换为"replacement"           
 * 每行仅在首次出现处替换。  
 *
 * 该数组必须是 malloc'd 数组，元素各自单独 malloc'd。 
 * 释放被丢弃的字符串。
 *
 * 它完成了 shell 脚本中 sed 所做的大部分工作，但 
 * 无需任何 regexp 处理。 
 */
static char **
replace_token(char **lines, const char *token, const char *replacement)
{
	int			toklen,
				replen,
				diff;

	toklen = strlen(token);
	replen = strlen(replacement);
	diff = replen - toklen;

	for (int i = 0; lines[i]; i++)
	{
		char	   *where;
		char	   *newline;
		int			pre;

		/* 若无需修改则无事可做    */
		if ((where = strstr(lines[i], token)) == NULL)
			continue;

		/* 若到达此处则需修改——构造新行       */

		newline = (char *) pg_malloc(strlen(lines[i]) + diff + 1);

		pre = where - lines[i];

		memcpy(newline, lines[i], pre);

		memcpy(newline + pre, replacement, replen);

		strcpy(newline + pre + replen, lines[i] + pre + toklen);

		free(lines[i]);
		lines[i] = newline;
	}

	return lines;
}

/*
 * 修改行数组，将可能被注释掉的参数 guc_name 的
 * 赋值替换为 guc_value 的实时赋值。
 * 值将被适当地加引号。
 *
 * 如果 mark_as_comment 为 true，替换行会以 '#' 为前缀。
 * 这用于修复有效默认值可能与
 * postgresql.conf.sample 中内容不匹配的情况。
 *
 * 我们假设最多只有一个匹配的赋值。如果没有找到匹配，
 * 则追加一行包含所需赋值的新行。
 *
 * 数组必须是 malloc 分配的数组，包含各自 malloc 分配的字符串。
 * 我们释放所有被丢弃的字符串。
                          */
static char **
replace_guc_value(char **lines, const char *guc_name, const char *guc_value,
				  bool mark_as_comment)
{
	int			namelen = strlen(guc_name);
	PQExpBuffer newline = createPQExpBuffer();
	int			i;

	/* 准备替换行，除了可能的注释和换行                      */
	if (mark_as_comment)
		appendPQExpBufferChar(newline, '#');
	appendPQExpBuffer(newline, "%s = ", guc_name);
	if (guc_value_requires_quotes(guc_value))
		appendPQExpBuffer(newline, "'%s'", escape_quotes(guc_value));
	else
		appendPQExpBufferStr(newline, guc_value);

	for (i = 0; lines[i]; i++)
	{
		const char *where;
		const char *namestart;

		/*
		 * 查找为guc_name赋值的行。通常其前会有'#'，但 
		 * 如果-c 开关覆盖了先前的赋值，则可能没有。  
		 * 我们也允许存在前导空白，                              
		 * 尽管通常不会有。                
		 */
		where = lines[i];
		while (*where == '#' || isspace((unsigned char) *where))
			where++;
		if (pg_strncasecmp(where, guc_name, namelen) != 0)
			continue;
		namestart = where;
		where += namelen;
		while (isspace((unsigned char) *where))
			where++;
		if (*where != '=')
			continue;

		/* 找到了——采用文件中显示的规范大小写       */
		memcpy(&newline->data[mark_as_comment ? 1 : 0], namestart, namelen);

		/* 接着追加原有注释（若有）   */
		where = strrchr(where, '#');
		if (where)
		{
			/*
			 * 我们尝试保留原始缩进，这很繁琐。
			 * oldindent 和 newindent 以去制表位列衡量。
			                     */
			const char *ptr;
			int			oldindent = 0;
			int			newindent;

			for (ptr = lines[i]; ptr < where; ptr++)
			{
				if (*ptr == '\t')
					oldindent += 8 - (oldindent % 8);
				else
					oldindent++;
			}
			/* 不考虑 guc_value 中含制表符的情况*/
			newindent = newline->len;
			/* 追加适当的制表符和空格，至少强制一个   */
			oldindent = Max(oldindent, newindent + 1);
			while (newindent < oldindent)
			{
				int			newindent_if_tab = newindent + 8 - (newindent % 8);

				if (newindent_if_tab <= oldindent)
				{
					appendPQExpBufferChar(newline, '\t');
					newindent = newindent_if_tab;
				}
				else
				{
					appendPQExpBufferChar(newline, ' ');
					newindent++;
				}
			}
			/* 最后追加旧注释              */
			appendPQExpBufferStr(newline, where);
			/* 我们已经追加了原始换行；不要再添加一个   */
		}
		else
			appendPQExpBufferChar(newline, '\n');

		free(lines[i]);
		lines[i] = newline->data;

		break;					/* 假设只有一个匹配      */
	}

	if (lines[i] == NULL)
	{
		/*
		 * 未找到匹配，故追加新条目。（依靠引导服务器   
		 * 在其非有效 GUC 名称时报错。） 
		 */
		appendPQExpBufferChar(newline, '\n');
		lines = pg_realloc_array(lines, char *, i + 2);
		lines[i++] = newline->data;
		lines[i] = NULL;		/* 保持数组以 null 结尾    */
	}

	free(newline);				/* 但不释放 newline->data   */

	return lines;
}

/*
 * 判断是否应为替换的 GUC 值加引号。这里我们并不      
 * 严格，但希望避免为简单标识符和带单位的数字    
 * 加引号，皆为常见情形。  
 */
static bool
guc_value_requires_quotes(const char *guc_value)
{
	/* 此处勿用<ctype.h>宏，它们可能接受过多         */
#define LETTERS	"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DIGITS	"0123456789"

	if (*guc_value == '\0')
		return true;			/* 空字符串必须加引号 */
	if (strchr(LETTERS, *guc_value))
	{
		if (strspn(guc_value, LETTERS DIGITS) == strlen(guc_value))
			return false;		/* 是一个标识符 */
		return true;			/* 不  */
	}
	if (strchr(DIGITS, *guc_value))
	{
		/* 跳过数字     */
		guc_value += strspn(guc_value, DIGITS);
		/* 数字后面可以有零个或多个单位字母        */
		if (strspn(guc_value, LETTERS) == strlen(guc_value))
			return false;		/* 是数字，可能带单位        */
		return true;			/* 不  */
	}
	return true;				/* 其余一律加引号   */
}

/*
 * 从文本文件中读取各行
 *
 * 结果为 malloc'd 数组，元素各自单独 malloc'd。      
 */
static char **
readfile(const char *path)
{
	char	  **result;
	FILE	   *infile;
	StringInfoData line;
	int			maxlines;
	int			n;

	if ((infile = fopen(path, "r")) == NULL)
		pg_fatal("could not open file \"%s\" for reading: %m", path);

	initStringInfo(&line);

	maxlines = 1024;
	result = (char **) pg_malloc(maxlines * sizeof(char *));

	n = 0;
	while (pg_get_line_buf(infile, &line))
	{
		/* 确保有空间容纳尾部的 NULL 指针               */
		if (n >= maxlines - 1)
		{
			maxlines *= 2;
			result = (char **) pg_realloc(result, maxlines * sizeof(char *));
		}

		result[n++] = pg_strdup(line.data);
	}
	result[n] = NULL;

	pfree(line.data);

	fclose(infile);

	return result;
}

/*
 * 将行数组写入文件         
 *
 * "lines"必须是 malloc'd 数组，元素各自单独 malloc'd。 
 * 数据均在此释放。    
 *
 * 此处仅用于写文本文件。用 fopen "w" 而非 PG_BINARY_W， 
 * 这样生成的配置文件在 Windows 上可以方便地编辑。       
 */
static void
writefile(char *path, char **lines)
{
	FILE	   *out_file;
	char	  **line;

	if ((out_file = fopen(path, "w")) == NULL)
		pg_fatal("could not open file \"%s\" for writing: %m", path);
	for (line = lines; *line != NULL; line++)
	{
		if (fputs(*line, out_file) < 0)
			pg_fatal("could not write file \"%s\": %m", path);
		free(*line);
	}
	if (fclose(out_file))
		pg_fatal("could not close file \"%s\": %m", path);
	free(lines);
}

/*
 * 打开子命令并提供适当的错误消息
   */
static FILE *
popen_check(const char *command, const char *mode)
{
	FILE	   *cmdfd;

	fflush(NULL);
	errno = 0;
	cmdfd = popen(command, mode);
	if (cmdfd == NULL)
		pg_log_error("could not execute command \"%s\": %m", command);
	return cmdfd;
}

/*
 * 失败时清理所创建的全部文件 
 * 若创建了数据目录则将其一并删除 
 */
static void
cleanup_directories_atexit(void)
{
	if (success)
		return;

	if (!noclean)
	{
		if (made_new_pgdata)
		{
			pg_log_info("removing data directory \"%s\"", pg_data);
			if (!rmtree(pg_data, true))
				pg_log_error("failed to remove data directory");
		}
		else if (found_existing_pgdata)
		{
			pg_log_info("removing contents of data directory \"%s\"",
						pg_data);
			if (!rmtree(pg_data, false))
				pg_log_error("failed to remove contents of data directory");
		}

		if (made_new_xlogdir)
		{
			pg_log_info("removing WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, true))
				pg_log_error("failed to remove WAL directory");
		}
		else if (found_existing_xlogdir)
		{
			pg_log_info("removing contents of WAL directory \"%s\"", xlog_dir);
			if (!rmtree(xlog_dir, false))
				pg_log_error("failed to remove contents of WAL directory");
		}
		/* 若启动期间夭折则不做任何事！ */
	}
	else
	{
		if (made_new_pgdata || found_existing_pgdata)
			pg_log_info("data directory \"%s\" not removed at user's request",
						pg_data);

		if (made_new_xlogdir || found_existing_xlogdir)
			pg_log_info("WAL directory \"%s\" not removed at user's request",
						xlog_dir);
	}
}

/*
 * 查找当前用户
 *
 * 在 unix 上确保不是 root
      */
static char *
get_id(void)
{
	const char *username;

#ifndef WIN32
	if (geteuid() == 0)			/* 0即root的uid  */
	{
		pg_log_error("cannot be run as root");
		pg_log_error_hint("Please log in (using, e.g., \"su\") as the (unprivileged) user that will own the server process.");
		exit(1);
	}
#endif

	username = get_user_name_or_exit(progname);

	return pg_strdup(username);
}

static char *
encodingid_to_string(int enc)
{
	char		result[20];

	sprintf(result, "%d", enc);
	return pg_strdup(result);
}

/*
 * 获取给定编码名称的编码 id
          */
static int
get_encoding_id(const char *encoding_name)
{
	int			enc;

	if (encoding_name && *encoding_name)
	{
		if ((enc = pg_valid_server_encoding(encoding_name)) >= 0)
			return enc;
	}
	pg_fatal("\"%s\" is not a valid server encoding name",
			 encoding_name ? encoding_name : "(null)");
}

/*
 * 用于支持确定最佳的默认文本搜索配置。             
 * 以 LC_CTYPE 的第一部分（即语言名称）为依据。      
 */
struct tsearch_config_match
{
	const char *tsconfname;
	const char *langname;
};

static const struct tsearch_config_match tsearch_config_languages[] =
{
	{"arabic", "ar"},
	{"arabic", "Arabic"},
	{"armenian", "hy"},
	{"armenian", "Armenian"},
	{"basque", "eu"},
	{"basque", "Basque"},
	{"catalan", "ca"},
	{"catalan", "Catalan"},
	{"danish", "da"},
	{"danish", "Danish"},
	{"dutch", "nl"},
	{"dutch", "Dutch"},
	{"english", "C"},
	{"english", "POSIX"},
	{"english", "en"},
	{"english", "English"},
	{"finnish", "fi"},
	{"finnish", "Finnish"},
	{"french", "fr"},
	{"french", "French"},
	{"german", "de"},
	{"german", "German"},
	{"greek", "el"},
	{"greek", "Greek"},
	{"hindi", "hi"},
	{"hindi", "Hindi"},
	{"hungarian", "hu"},
	{"hungarian", "Hungarian"},
	{"indonesian", "id"},
	{"indonesian", "Indonesian"},
	{"irish", "ga"},
	{"irish", "Irish"},
	{"italian", "it"},
	{"italian", "Italian"},
	{"lithuanian", "lt"},
	{"lithuanian", "Lithuanian"},
	{"nepali", "ne"},
	{"nepali", "Nepali"},
	{"norwegian", "no"},
	{"norwegian", "Norwegian"},
	{"portuguese", "pt"},
	{"portuguese", "Portuguese"},
	{"romanian", "ro"},
	{"russian", "ru"},
	{"russian", "Russian"},
	{"serbian", "sr"},
	{"serbian", "Serbian"},
	{"spanish", "es"},
	{"spanish", "Spanish"},
	{"swedish", "sv"},
	{"swedish", "Swedish"},
	{"tamil", "ta"},
	{"tamil", "Tamil"},
	{"turkish", "tr"},
	{"turkish", "Turkish"},
	{"yiddish", "yi"},
	{"yiddish", "Yiddish"},
	{NULL, NULL}				/* 结束符  */
};

/*
 * 查找与 lc_ctype 匹配的文本搜索配置，并返回其         
 * 名称，无则返回 NULL    
 */
static const char *
find_matching_ts_config(const char *lc_type)
{
	int			i;
	char	   *langname,
			   *ptr;

	/*
	 * 把lc_ctype转换为语言名称：去掉下划线（常见情况）
	 * 或连字符（Windows"区域设置名"；参见IsoLocaleName()
	 * 注释）之后的内容。  
	 *
	 * XXX 空格' '是否应为终止字符？这会为Windows区域设置 
	 * "Norwegian (Nynorsk)_Norway.1252"选中"norwegian"。如果我们     
	 * 这样还应接受"nn"与"nb" Unix 区域设置。
	 *
	 * 保险起见，也在'.'或'@'处停止。    
	 */
	if (lc_type == NULL)
		langname = pg_strdup("");
	else
	{
		ptr = langname = pg_strdup(lc_type);
		while (*ptr &&
			   *ptr != '_' && *ptr != '-' && *ptr != '.' && *ptr != '@')
			ptr++;
		*ptr = '\0';
	}

	for (i = 0; tsearch_config_languages[i].tsconfname; i++)
	{
		if (pg_strcasecmp(tsearch_config_languages[i].langname, langname) == 0)
		{
			free(langname);
			return tsearch_config_languages[i].tsconfname;
		}
	}

	free(langname);
	return NULL;
}


/*
 * 设置给定输入文件变量在数据目录下的名称
  */
static void
set_input(char **dest, const char *filename)
{
	*dest = psprintf("%s/%s", share_path, filename);
}

/*
 * 检查输入文件是否存在    
 */
static void
check_input(char *path)
{
	struct stat statbuf;

	if (stat(path, &statbuf) != 0)
	{
		if (errno == ENOENT)
		{
			pg_log_error("file \"%s\" does not exist", path);
			pg_log_error_hint("This might mean you have a corrupted installation or identified the wrong directory with the invocation option -L.");
		}
		else
		{
			pg_log_error("could not access file \"%s\": %m", path);
			pg_log_error_hint("This might mean you have a corrupted installation or identified the wrong directory with the invocation option -L.");
		}
		exit(1);
	}
	if (!S_ISREG(statbuf.st_mode))
	{
		pg_log_error("file \"%s\" is not a regular file", path);
		pg_log_error_hint("This might mean you have a corrupted installation or identified the wrong directory with the invocation option -L.");
		exit(1);
	}
}

/*
 * 将 PG_VERSION 文件写入数据目录，或其子目录         
 * 若extrapath非NULL。  
 */
static void
write_version_file(const char *extrapath)
{
	FILE	   *version_file;
	char	   *path;

	if (extrapath == NULL)
		path = psprintf("%s/PG_VERSION", pg_data);
	else
		path = psprintf("%s/%s/PG_VERSION", pg_data, extrapath);

	if ((version_file = fopen(path, PG_BINARY_W)) == NULL)
		pg_fatal("could not open file \"%s\" for writing: %m", path);
	if (fprintf(version_file, "%s\n", PG_MAJORVERSION) < 0 ||
		fclose(version_file))
		pg_fatal("could not write file \"%s\": %m", path);
	free(path);
}

/*
 * 设置空配置文件，以便通过启动测试后端来检查配置   
 * 设置。     
 */
static void
set_null_conf(void)
{
	FILE	   *conf_file;
	char	   *path;

	path = psprintf("%s/postgresql.conf", pg_data);
	conf_file = fopen(path, PG_BINARY_W);
	if (conf_file == NULL)
		pg_fatal("could not open file \"%s\" for writing: %m", path);
	if (fclose(conf_file))
		pg_fatal("could not write file \"%s\": %m", path);
	free(path);
}

/*
 * 确定在此平台上应使用哪种动态共享内存实现。       
 * POSIX 共享内存更可取，因为在大多数同时支持两者   
 * 的系统上，其默认分配上限远高于 System V 的上限；   
 * 但平台提供 shm_open 并不意味着该调用在尝试时       
 * 一定成功。因此，我们尝试重现 postmaster 在            
 * dsm_impl.c 中分配 POSIX 段时的做法；如果行不通，         
 * 就假定 postmaster 同样无法使用，转而将集簇             
 * 配置为使用 System V 共享内存。                            
 *                
 *
 * 我们避免默认选择 Solaris 的 shm_open()实现。                
 * 它在争用下可能休眠并偶发失败。  
 */
static const char *
choose_dsm_implementation(void)
{
#if defined(HAVE_SHM_OPEN) && !defined(__sun__)
	int			ntries = 10;
	pg_prng_state prng_state;

	/* 初始化 prng；本程序中只有此函数使用它。         */
	pg_prng_seed(&prng_state, (uint64) (getpid() ^ time(NULL)));

	while (ntries > 0)
	{
		uint32		handle;
		char		name[64];
		int			fd;

		handle = pg_prng_uint32(&prng_state);
		snprintf(name, 64, "/PostgreSQL.%u", handle);
		if ((fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600)) != -1)
		{
			close(fd);
			shm_unlink(name);
			return "posix";
		}
		if (errno != EEXIST)
			break;
		--ntries;
	}
#endif

#ifdef WIN32
	return "windows";
#else
	return "sysv";
#endif
}

/*
 * 确定平台相关的配置设置          
 *
 * 若内核允许则使用合理的值，否则调低。       
 */
static void
test_config_settings(void)
{
	/*
	 * 该宏定义了给定 max_connections 值下我们所需的      
	 * 最小 shared_buffers。数组给出可尝试的设置。  
	 */
#define MIN_BUFS_FOR_CONNS(nconns)	((nconns) * 10)

	static const int trial_conns[] = {
		100, 50, 40, 30, 20
	};
	static const int trial_bufs[] = {
		16384, 8192, 4096, 3584, 3072, 2560, 2048, 1536,
		1000, 900, 800, 700, 600, 500,
		400, 300, 200, 100, 50
	};

	const int	connslen = sizeof(trial_conns) / sizeof(int);
	const int	bufslen = sizeof(trial_bufs) / sizeof(int);
	int			i,
				test_conns,
				test_buffs,
				ok_buffers = 0;

	/*
	 * 需要首先确定可用的 DSM 实现，以便后续
	 * 测试不会因此失败。
	                                        */
	printf(_("selecting dynamic shared memory implementation ... "));
	fflush(stdout);
	dynamic_shared_memory_type = choose_dsm_implementation();
	printf("%s\n", dynamic_shared_memory_type);

	/*
	 * 先探测 max_connections 再探测 shared_buffers，因为它受到的
	 * 限制比 shared_buffers 更多。   
	 */
	printf(_("selecting default max_connections ... "));
	fflush(stdout);

	for (i = 0; i < connslen; i++)
	{
		test_conns = trial_conns[i];
		test_buffs = MIN_BUFS_FOR_CONNS(test_conns);

		if (test_specific_config_settings(test_conns, test_buffs))
		{
			ok_buffers = test_buffs;
			break;
		}
	}
	if (i >= connslen)
		i = connslen - 1;
	n_connections = trial_conns[i];

	printf("%d\n", n_connections);

	printf(_("selecting default shared_buffers ... "));
	fflush(stdout);

	for (i = 0; i < bufslen; i++)
	{
		/* 使用相同的内存量，与 BLCKSZ 无关     */
		test_buffs = (trial_bufs[i] * 8192) / BLCKSZ;
		if (test_buffs <= ok_buffers)
		{
			test_buffs = ok_buffers;
			break;
		}

		if (test_specific_config_settings(n_connections, test_buffs))
			break;
	}
	n_buffers = test_buffs;

	if ((n_buffers * (BLCKSZ / 1024)) % 1024 == 0)
		printf("%dMB\n", (n_buffers * (BLCKSZ / 1024)) / 1024);
	else
		printf("%dkB\n", n_buffers * (BLCKSZ / 1024));

	printf(_("selecting default time zone ... "));
	fflush(stdout);
	default_timezone = select_default_timezone(share_path);
	printf("%s\n", default_timezone ? default_timezone : "GMT");
}

/*
 * 测试特定的配置设置组合。
                   */
static bool
test_specific_config_settings(int test_conns, int test_buffs)
{
	PQExpBuffer cmd = createPQExpBuffer();
	_stringlist *gnames,
			   *gvalues;
	int			status;

	/* 设置测试 postmaster 调用        */
	printfPQExpBuffer(cmd,
					  "\"%s\" --check %s %s "
					  "-c max_connections=%d "
					  "-c shared_buffers=%d "
					  "-c dynamic_shared_memory_type=%s",
					  backend_exec, boot_options, extra_options,
					  test_conns, test_buffs,
					  dynamic_shared_memory_type);

	/* 加上用户给定的设置覆盖项 */
	for (gnames = extra_guc_names, gvalues = extra_guc_values;
		 gnames != NULL;		/* 假设列表具有相同的长度 */
		 gnames = gnames->next, gvalues = gvalues->next)
	{
		appendPQExpBuffer(cmd, " -c %s=", gnames->str);
		appendShellString(cmd, gvalues->str);
	}

	appendPQExpBuffer(cmd,
					  " < \"%s\" > \"%s\" 2>&1",
					  DEVNULL, DEVNULL);

	fflush(NULL);
	status = system(cmd->data);

	destroyPQExpBuffer(cmd);

	return (status == 0);
}

/*
 * 使用"漂亮"的单位计算默认的 wal_size。
   */
static char *
pretty_wal_size(int segment_count)
{
	int			sz = wal_segment_size_mb * segment_count;
	char	   *result = pg_malloc(14);

	if ((sz % 1024) == 0)
		snprintf(result, 14, "%dGB", sz / 1024);
	else
		snprintf(result, 14, "%dMB", sz);

	return result;
}

/*
 * 设置所有配置文件
    */
static void
setup_config(void)
{
	char	  **conflines;
	char		repltok[MAXPGPATH];
	char		path[MAXPGPATH];
	_stringlist *gnames,
			   *gvalues;

	fputs(_("creating configuration files ... "), stdout);
	fflush(stdout);

	/* postgresql.conf */

	conflines = readfile(conf_file);

	snprintf(repltok, sizeof(repltok), "%d", n_connections);
	conflines = replace_guc_value(conflines, "max_connections",
								  repltok, false);

	if ((n_buffers * (BLCKSZ / 1024)) % 1024 == 0)
		snprintf(repltok, sizeof(repltok), "%dMB",
				 (n_buffers * (BLCKSZ / 1024)) / 1024);
	else
		snprintf(repltok, sizeof(repltok), "%dkB",
				 n_buffers * (BLCKSZ / 1024));
	conflines = replace_guc_value(conflines, "shared_buffers",
								  repltok, false);

	conflines = replace_guc_value(conflines, "lc_messages",
								  lc_messages, false);

	conflines = replace_guc_value(conflines, "lc_monetary",
								  lc_monetary, false);

	conflines = replace_guc_value(conflines, "lc_numeric",
								  lc_numeric, false);

	conflines = replace_guc_value(conflines, "lc_time",
								  lc_time, false);

	switch (locale_date_order(lc_time))
	{
		case DATEORDER_YMD:
			strcpy(repltok, "iso, ymd");
			break;
		case DATEORDER_DMY:
			strcpy(repltok, "iso, dmy");
			break;
		case DATEORDER_MDY:
		default:
			strcpy(repltok, "iso, mdy");
			break;
	}
	conflines = replace_guc_value(conflines, "datestyle",
								  repltok, false);

	snprintf(repltok, sizeof(repltok), "pg_catalog.%s",
			 default_text_search_config);
	conflines = replace_guc_value(conflines, "default_text_search_config",
								  repltok, false);

	if (default_timezone)
	{
		conflines = replace_guc_value(conflines, "timezone",
									  default_timezone, false);
		conflines = replace_guc_value(conflines, "log_timezone",
									  default_timezone, false);
	}

	conflines = replace_guc_value(conflines, "dynamic_shared_memory_type",
								  dynamic_shared_memory_type, false);

	/* 注意：这些值依赖于 wal_segment_size_mb，并非常量      */
	conflines = replace_guc_value(conflines, "min_wal_size",
								  pretty_wal_size(DEFAULT_MIN_WAL_SEGS), false);

	conflines = replace_guc_value(conflines, "max_wal_size",
								  pretty_wal_size(DEFAULT_MAX_WAL_SEGS), false);

	/*
	 * 修正各种条目以匹配真实的编译期默认值。既然       
	 * 它们确为默认值，故将 postgresql.conf 行保持注释。   
	 */
	conflines = replace_guc_value(conflines, "unix_socket_directories",
								  DEFAULT_PGSOCKET_DIR, true);

	conflines = replace_guc_value(conflines, "port",
								  DEF_PGPORT_STR, true);

#if DEFAULT_BACKEND_FLUSH_AFTER > 0
	snprintf(repltok, sizeof(repltok), "%dkB",
			 DEFAULT_BACKEND_FLUSH_AFTER * (BLCKSZ / 1024));
	conflines = replace_guc_value(conflines, "backend_flush_after",
								  repltok, true);
#endif

#if DEFAULT_BGWRITER_FLUSH_AFTER > 0
	snprintf(repltok, sizeof(repltok), "%dkB",
			 DEFAULT_BGWRITER_FLUSH_AFTER * (BLCKSZ / 1024));
	conflines = replace_guc_value(conflines, "bgwriter_flush_after",
								  repltok, true);
#endif

#if DEFAULT_CHECKPOINT_FLUSH_AFTER > 0
	snprintf(repltok, sizeof(repltok), "%dkB",
			 DEFAULT_CHECKPOINT_FLUSH_AFTER * (BLCKSZ / 1024));
	conflines = replace_guc_value(conflines, "checkpoint_flush_after",
								  repltok, true);
#endif

#ifndef USE_PREFETCH
	conflines = replace_guc_value(conflines, "effective_io_concurrency",
								  "0", true);
#endif

#ifdef WIN32
	conflines = replace_guc_value(conflines, "update_process_title",
								  "off", true);
#endif

	/*
	 * 若选 md5 作为授权方法，则将 password_encryption 改为
	 * 为 md5，除非同时选择了 scram-sha-256。            
	 */
	if ((strcmp(authmethodlocal, "md5") == 0 &&
		 strcmp(authmethodhost, "scram-sha-256") != 0) ||
		(strcmp(authmethodhost, "md5") == 0 &&
		 strcmp(authmethodlocal, "scram-sha-256") != 0))
	{
		conflines = replace_guc_value(conflines, "password_encryption",
									  "md5", false);
	}

	/*
	 * 如果已为集簇启用组访问权限，那么确保日志文件也  
	 * 允许组访问才有意义。否则，如果日志文件未被       
	 * 迁移，组内用户进行的备份将会失败。          
	 *           
	 */
	if (pg_dir_create_mode == PG_DIR_MODE_GROUP)
	{
		conflines = replace_guc_value(conflines, "log_file_mode",
									  "0640", false);
	}

	/*
	 * 现在替换所有通过 -c 开关覆盖的内容。
	    */
	for (gnames = extra_guc_names, gvalues = extra_guc_values;
		 gnames != NULL;		/* 假设列表具有相同的长度 */
		 gnames = gnames->next, gvalues = gvalues->next)
	{
		conflines = replace_guc_value(conflines, gnames->str,
									  gvalues->str, false);
	}

	/* ... 然后写出完成的 postgresql.conf 文件    */
	snprintf(path, sizeof(path), "%s/postgresql.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, pg_file_create_mode) != 0)
		pg_fatal("could not change permissions of \"%s\": %m", path);


	/* postgresql.auto.conf */

	conflines = pg_malloc_array(char *, 3);
	conflines[0] = pg_strdup("# Do not edit this file manually!\n");
	conflines[1] = pg_strdup("# It will be overwritten by the ALTER SYSTEM command.\n");
	conflines[2] = NULL;

	sprintf(path, "%s/postgresql.auto.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, pg_file_create_mode) != 0)
		pg_fatal("could not change permissions of \"%s\": %m", path);


	/* pg_hba.conf */

	conflines = readfile(hba_file);

	conflines = replace_token(conflines, "@remove-line-for-nolocal@", "");


	/*
	 * 探测平台是否真正支持 IPv6，若不支持则注释掉    
	 * 相关的 pg_hba 行。这样可以避免当 getaddrinfo         
	 * 实际上无法处理 IPv6 时出现运行时警告。这在          
	 * Windows上尤其有用：带IPv6的机器上构建的可执行文件 
	 * 可能在无 IPv6 机器上运行
	 */
	{
		struct addrinfo *gai_result;
		struct addrinfo hints;
		int			err = 0;

#ifdef WIN32
		/* 调用 getaddrinfo 之前需先调用 WSAStartup   */
		WSADATA		wsaData;

		err = WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		/* 为最佳效果，此代码应与 parse_hba_line()一致  */
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = 0;
		hints.ai_protocol = 0;
		hints.ai_addrlen = 0;
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		if (err != 0 ||
			getaddrinfo("::1", NULL, &hints, &gai_result) != 0)
		{
			conflines = replace_token(conflines,
									  "host    all             all             ::1",
									  "#host    all             all             ::1");
			conflines = replace_token(conflines,
									  "host    replication     all             ::1",
									  "#host    replication     all             ::1");
		}
	}

	/* 替换默认的认证方法            */
	conflines = replace_token(conflines,
							  "@authmethodhost@",
							  authmethodhost);
	conflines = replace_token(conflines,
							  "@authmethodlocal@",
							  authmethodlocal);

	conflines = replace_token(conflines,
							  "@authcomment@",
							  (strcmp(authmethodlocal, "trust") == 0 || strcmp(authmethodhost, "trust") == 0) ? AUTHTRUST_WARNING : "");

	snprintf(path, sizeof(path), "%s/pg_hba.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, pg_file_create_mode) != 0)
		pg_fatal("could not change permissions of \"%s\": %m", path);


	/* pg_ident.conf */

	conflines = readfile(ident_file);

	snprintf(path, sizeof(path), "%s/pg_ident.conf", pg_data);

	writefile(path, conflines);
	if (chmod(path, pg_file_create_mode) != 0)
		pg_fatal("could not change permissions of \"%s\": %m", path);

	check_ok();
}


/*
 * 在引导模式下运行 BKI 脚本来创建 template1
   */
static void
bootstrap_template1(void)
{
	PG_CMD_DECL;
	char	  **line;
	char	  **bki_lines;
	char		headerline[MAXPGPATH];
	char		buf[64];

	printf(_("running bootstrap script ... "));
	fflush(stdout);

	bki_lines = readfile(bki_file);

	/* 检查 bki 文件是否看起来是正确的版本     */

	snprintf(headerline, sizeof(headerline), "# PostgreSQL %s\n",
			 PG_MAJORVERSION);

	if (strcmp(headerline, *bki_lines) != 0)
	{
		pg_log_error("input file \"%s\" does not belong to PostgreSQL %s",
					 bki_file, PG_VERSION);
		pg_log_error_hint("Specify the correct path using the option -L.");
		exit(1);
	}

	/* 替换 BKI 文件中使用的各种符号           */

	sprintf(buf, "%d", NAMEDATALEN);
	bki_lines = replace_token(bki_lines, "NAMEDATALEN", buf);

	sprintf(buf, "%d", (int) sizeof(Pointer));
	bki_lines = replace_token(bki_lines, "SIZEOF_POINTER", buf);

	bki_lines = replace_token(bki_lines, "ALIGNOF_POINTER",
							  (sizeof(Pointer) == 4) ? "i" : "d");

	bki_lines = replace_token(bki_lines, "FLOAT8PASSBYVAL",
							  FLOAT8PASSBYVAL ? "true" : "false");

	bki_lines = replace_token(bki_lines, "POSTGRES",
							  escape_quotes_bki(username));

	bki_lines = replace_token(bki_lines, "ENCODING",
							  encodingid_to_string(encodingid));

	bki_lines = replace_token(bki_lines, "LC_COLLATE",
							  escape_quotes_bki(lc_collate));

	bki_lines = replace_token(bki_lines, "LC_CTYPE",
							  escape_quotes_bki(lc_ctype));

	bki_lines = replace_token(bki_lines, "ICU_LOCALE",
							  icu_locale ? escape_quotes_bki(icu_locale) : "_null_");

	bki_lines = replace_token(bki_lines, "ICU_RULES",
							  icu_rules ? escape_quotes_bki(icu_rules) : "_null_");

	sprintf(buf, "%c", locale_provider);
	bki_lines = replace_token(bki_lines, "LOCALE_PROVIDER", buf);

	/* 还要确保后端不会被此环境变量混淆：         */
	unsetenv("PGCLIENTENCODING");

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" --boot -X %d %s %s %s %s",
			 backend_exec,
			 wal_segment_size_mb * (1024 * 1024),
			 data_checksums ? "-k" : "",
			 boot_options, extra_options,
			 debug ? "-d 5" : "");


	PG_CMD_OPEN;

	for (line = bki_lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	PG_CMD_CLOSE;

	free(bki_lines);

	check_ok();
}

/*
 * 设置影子密码表
            */
static void
setup_auth(FILE *cmdfd)
{
	/*
	 * authid 表不应该是可读的，除了通过视图，以确保
	 * 密码不会被公开访问。
	            */
	PG_CMD_PUTS("REVOKE ALL ON pg_authid FROM public;\n\n");

	if (superuser_password)
		PG_CMD_PRINTF("ALTER USER \"%s\" WITH PASSWORD E'%s';\n\n",
					  username, escape_quotes(superuser_password));
}

/*
 * 若需要则获取超级用户密码  
 */
static void
get_su_pwd(void)
{
	char	   *pwd1;

	if (pwprompt)
	{
		/*
		 * 从终端读取密码
		       */
		char	   *pwd2;

		printf("\n");
		fflush(stdout);
		pwd1 = simple_prompt("Enter new superuser password: ", false);
		pwd2 = simple_prompt("Enter it again: ", false);
		if (strcmp(pwd1, pwd2) != 0)
		{
			fprintf(stderr, _("Passwords didn't match.\n"));
			exit(1);
		}
		free(pwd2);
	}
	else
	{
		/*
		 * 从文件读取密码  
		 *
		 * 理想情况下应坚持该文件不得全局可读。         
		 * 然而，此选项主要用于 Windows，那里可能           
		 * 完全没有文件权限，所以我们暂时不去              
		 * 多虑  
		 */
		FILE	   *pwf = fopen(pwfilename, "r");

		if (!pwf)
			pg_fatal("could not open file \"%s\" for reading: %m",
					 pwfilename);
		pwd1 = pg_get_line(pwf, NULL);
		if (!pwd1)
		{
			if (ferror(pwf))
				pg_fatal("could not read password from file \"%s\": %m",
						 pwfilename);
			else
				pg_fatal("password file \"%s\" is empty",
						 pwfilename);
		}
		fclose(pwf);

		(void) pg_strip_crlf(pwd1);
	}

	superuser_password = pwd1;
}

/*
 * 设置 pg_depend
 */
static void
setup_depend(FILE *cmdfd)
{
	/*
	 * 推进 OID 计数器，以便后续创建的对象
	 * 不被固定。
	          */
	PG_CMD_PUTS("SELECT pg_stop_making_pinned_objects();\n\n");
}

/*
 * Run external file
 */
static void
setup_run_file(FILE *cmdfd, const char *filename)
{
	char	  **lines;

	lines = readfile(filename);

	for (char **line = lines; *line != NULL; line++)
	{
		PG_CMD_PUTS(*line);
		free(*line);
	}

	PG_CMD_PUTS("\n\n");

	free(lines);
}

/*
 * 填充额外的描述数据
    */
static void
setup_description(FILE *cmdfd)
{
	/* 为操作符实现函数创建默认描述                        */
	PG_CMD_PUTS("WITH funcdescs AS ( "
				"SELECT p.oid as p_oid, o.oid as o_oid, oprname "
				"FROM pg_proc p JOIN pg_operator o ON oprcode = p.oid ) "
				"INSERT INTO pg_description "
				"  SELECT p_oid, 'pg_proc'::regclass, 0, "
				"    'implementation of ' || oprname || ' operator' "
				"  FROM funcdescs "
				"  WHERE NOT EXISTS (SELECT 1 FROM pg_description "
				"   WHERE objoid = p_oid AND classoid = 'pg_proc'::regclass) "
				"  AND NOT EXISTS (SELECT 1 FROM pg_description "
				"   WHERE objoid = o_oid AND classoid = 'pg_operator'::regclass"
				"         AND description LIKE 'deprecated%');\n\n");
}

/*
 * 填充 pg_collation
   */
static void
setup_collation(FILE *cmdfd)
{
	/*
	 * 为 pg_collation.dat 中定义的排序规则设置排序规则      
	 * 版本，但排序行为已知永不改变的除外。               
	 *        
	 */
	PG_CMD_PUTS("UPDATE pg_collation SET collversion = pg_collation_actual_version(oid) WHERE collname = 'unicode';\n\n");

	/* 导入操作系统中我们能找到的所有排序规则 */
	PG_CMD_PUTS("SELECT pg_import_system_collations('pg_catalog');\n\n");
}

/*
 * 设置权限     
 *
 * 我们将大多数系统目录标记为全局可读。目前我们      
 * 无需触碰函数、语言或数据库，因为它们的默认   
 * 权限没问题。 
 *
 * 有些对象默认可能需要不同的权限，因此我们要 
 * 确保不会覆盖已设置好的权限集（NOT NULL）。       
 *                
 *
 * 同时填充 pg_init_privs 以保存初始化时的权限状态。 
 * pg_dump 会使用它，让用户能够修改系统目录对象上的    
 * 权限，并让这些权限修改在 dump/reload 和            
 * pg_upgrade 之后得以保留。  
 *
 * 注意 pg_init_privs 仅用于每数据库对象，因此我们       
 * 不包括数据库和表空间。         
 */
static void
setup_privileges(FILE *cmdfd)
{
	PG_CMD_PRINTF("UPDATE pg_class "
				  "  SET relacl = (SELECT array_agg(a.acl) FROM "
				  " (SELECT E'=r/\"%s\"' as acl "
				  "  UNION SELECT unnest(pg_catalog.acldefault("
				  "    CASE WHEN relkind = " CppAsString2(RELKIND_SEQUENCE) " THEN 's' "
				  "         ELSE 'r' END::\"char\"," CppAsString2(BOOTSTRAP_SUPERUSERID) "::oid))"
				  " ) as a) "
				  "  WHERE relkind IN (" CppAsString2(RELKIND_RELATION) ", "
				  CppAsString2(RELKIND_VIEW) ", " CppAsString2(RELKIND_MATVIEW) ", "
				  CppAsString2(RELKIND_SEQUENCE) ")"
				  "  AND relacl IS NULL;\n\n",
				  escape_quotes(username));
	PG_CMD_PUTS("GRANT USAGE ON SCHEMA pg_catalog, public TO PUBLIC;\n\n");
	PG_CMD_PUTS("REVOKE ALL ON pg_largeobject FROM PUBLIC;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE relname = 'pg_class'),"
				"        0,"
				"        relacl,"
				"        'i'"
				"    FROM"
				"        pg_class"
				"    WHERE"
				"        relacl IS NOT NULL"
				"        AND relkind IN (" CppAsString2(RELKIND_RELATION) ", "
				CppAsString2(RELKIND_VIEW) ", " CppAsString2(RELKIND_MATVIEW) ", "
				CppAsString2(RELKIND_SEQUENCE) ");\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        pg_class.oid,"
				"        (SELECT oid FROM pg_class WHERE relname = 'pg_class'),"
				"        pg_attribute.attnum,"
				"        pg_attribute.attacl,"
				"        'i'"
				"    FROM"
				"        pg_class"
				"        JOIN pg_attribute ON (pg_class.oid = pg_attribute.attrelid)"
				"    WHERE"
				"        pg_attribute.attacl IS NOT NULL"
				"        AND pg_class.relkind IN (" CppAsString2(RELKIND_RELATION) ", "
				CppAsString2(RELKIND_VIEW) ", " CppAsString2(RELKIND_MATVIEW) ", "
				CppAsString2(RELKIND_SEQUENCE) ");\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),"
				"        0,"
				"        proacl,"
				"        'i'"
				"    FROM"
				"        pg_proc"
				"    WHERE"
				"        proacl IS NOT NULL;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE relname = 'pg_type'),"
				"        0,"
				"        typacl,"
				"        'i'"
				"    FROM"
				"        pg_type"
				"    WHERE"
				"        typacl IS NOT NULL;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE relname = 'pg_language'),"
				"        0,"
				"        lanacl,"
				"        'i'"
				"    FROM"
				"        pg_language"
				"    WHERE"
				"        lanacl IS NOT NULL;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE "
				"         relname = 'pg_largeobject_metadata'),"
				"        0,"
				"        lomacl,"
				"        'i'"
				"    FROM"
				"        pg_largeobject_metadata"
				"    WHERE"
				"        lomacl IS NOT NULL;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE relname = 'pg_namespace'),"
				"        0,"
				"        nspacl,"
				"        'i'"
				"    FROM"
				"        pg_namespace"
				"    WHERE"
				"        nspacl IS NOT NULL;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class WHERE "
				"         relname = 'pg_foreign_data_wrapper'),"
				"        0,"
				"        fdwacl,"
				"        'i'"
				"    FROM"
				"        pg_foreign_data_wrapper"
				"    WHERE"
				"        fdwacl IS NOT NULL;\n\n");
	PG_CMD_PUTS("INSERT INTO pg_init_privs "
				"  (objoid, classoid, objsubid, initprivs, privtype)"
				"    SELECT"
				"        oid,"
				"        (SELECT oid FROM pg_class "
				"         WHERE relname = 'pg_foreign_server'),"
				"        0,"
				"        srvacl,"
				"        'i'"
				"    FROM"
				"        pg_foreign_server"
				"    WHERE"
				"        srvacl IS NOT NULL;\n\n");
}

/*
 * 提取信息模式所需的奇怪版本格式
 * (09.08.0007abc)
                          */
static void
set_info_version(void)
{
	char	   *letterversion;
	long		major = 0,
				minor = 0,
				micro = 0;
	char	   *endptr;
	char	   *vstr = pg_strdup(PG_VERSION);
	char	   *ptr;

	ptr = vstr + (strlen(vstr) - 1);
	while (ptr != vstr && (*ptr < '0' || *ptr > '9'))
		ptr--;
	letterversion = ptr + 1;
	major = strtol(vstr, &endptr, 10);
	if (*endptr)
		minor = strtol(endptr + 1, &endptr, 10);
	if (*endptr)
		micro = strtol(endptr + 1, &endptr, 10);
	snprintf(infoversion, sizeof(infoversion), "%02ld.%02ld.%04ld%s",
			 major, minor, micro, letterversion);
}

/*
 * 加载信息模式并从特性文件中填充
    */
static void
setup_schema(FILE *cmdfd)
{
	setup_run_file(cmdfd, info_schema_file);

	PG_CMD_PRINTF("UPDATE information_schema.sql_implementation_info "
				  "  SET character_value = '%s' "
				  "  WHERE implementation_info_name = 'DBMS VERSION';\n\n",
				  infoversion);

	PG_CMD_PRINTF("COPY information_schema.sql_features "
				  "  (feature_id, feature_name, sub_feature_id, "
				  "  sub_feature_name, is_supported, comments) "
				  " FROM E'%s';\n\n",
				  escape_quotes(features_file));
}

/*
 * 加载 PL/pgSQL 服务器端语言
 */
static void
load_plpgsql(FILE *cmdfd)
{
	PG_CMD_PUTS("CREATE EXTENSION plpgsql;\n\n");
}

/*
 * 清理 template1 中的一切   
 */
static void
vacuum_db(FILE *cmdfd)
{
	/* 先运行 analyze 再 VACUUM，以冻结统计信息。 */
	PG_CMD_PUTS("ANALYZE;\n\nVACUUM FREEZE;\n\n");
}

/*
 * template1复制到template0
 */
static void
make_template0(FILE *cmdfd)
{
	/*
	 * pg_upgrade 尝试在升级期间保留数据库 OID。它足够       
	 * 聪明，能把同名冲突数据库删除并重建，但             
	 * 若同一 OID 被旧集簇某系统创建数据库占用，         
	 * 而新集簇另一系统创建数据库也用此 OID，则会失败。 
	 * 为避免此问题，给 template0 指定固定 OID，而不是     
	 * 让服务器自行选择。        
	 *
	 * （注意，即使用户可能在旧集簇中删除并重建过  
	 * 这些对象，但问题场景只在以下情况存在：旧集簇     
	 * 正在使用的 OID 同时也被新集簇使用——且新集簇      
	 * 应是全新 initdb 的结果。）                     
	 *
	 * 我们在这里使用"STRATEGY = file_copy"，因为 initdb         
	 * 期间检查点很廉价。"STRATEGY = wal_log"会产生更多 WAL，  
	 * 会稍慢一些，并使新集簇稍大一些。                 
	 */
	PG_CMD_PUTS("CREATE DATABASE template0 IS_TEMPLATE = true ALLOW_CONNECTIONS = false"
				" OID = " CppAsString2(Template0DbOid)
				" STRATEGY = file_copy;\n\n");

	/*
	 * template0 不应有任何依赖排序规则的对象，因此应取消
	 * 其排序规则版本。从它创建新数据库时会禁用          
	 * 排序规则检查     
	 */
	PG_CMD_PUTS("UPDATE pg_database SET datcollversion = NULL WHERE datname = 'template0';\n\n");

	/*
	 * 顺便在这里为 template1 设置排序规则版本。     
	 */
	PG_CMD_PUTS("UPDATE pg_database SET datcollversion = pg_database_collation_actual_version(oid) WHERE datname = 'template1';\n\n");

	/*
	 * 显式撤销 template1 和 template0 中的 public create-schema
	 * 和 create-temp-table 权限。
	                                           */
	PG_CMD_PUTS("REVOKE CREATE,TEMPORARY ON DATABASE template1 FROM public;\n\n");
	PG_CMD_PUTS("REVOKE CREATE,TEMPORARY ON DATABASE template0 FROM public;\n\n");

	PG_CMD_PUTS("COMMENT ON DATABASE template0 IS 'unmodifiable empty database';\n\n");

	/*
	 * 最后对pg_database执行vacuum以清理死元组 
	 */
	PG_CMD_PUTS("VACUUM pg_database;\n\n");
}

/*
 * 复制template1到postgres
 */
static void
make_postgres(FILE *cmdfd)
{
	/*
	 * 与 template0 一样，出于同样原因，为 postgres 指定固定 
	 * 的 OID 并选择 file_copy 策略。             
	 */
	PG_CMD_PUTS("CREATE DATABASE postgres OID = " CppAsString2(PostgresDbOid)
				" STRATEGY = file_copy;\n\n");
	PG_CMD_PUTS("COMMENT ON DATABASE postgres IS 'default administrative connection database';\n\n");
}

/*
 * 被中断时的信号处理函数。      
 *
 * Windows 运行时文档在 
 * https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/signal
 * 指出，许多操作被明确禁止在信号处理函数内进行，   
 * 包括 IO、内存分配和系统调用，且仅在处理 SIGFPE 时   
 * 才允许使用 jmpbuf。  
 *
 * 我通过设置标志而不是直接调用 exit()来避免这些         
 * 禁止操作。 
 *
 * 另请注意 Windows 对 SIGINT 的行为，其中说道：     
 * "SIGINT 不受任何 Win32 应用程序支持。当发生 CTRL+C           
 * 中断时，Win32 操作系统会生成一个新线程来专门       
 * 处理该中断。这会使单线程应用程序（例如 UNIX 中         
 * 的程序）变成多线程，从而导致意外行为。           
 *
 * 我不知道该如何处理。（奇怪，他们竟称 UNIX 为应用      
 * 程序！）故需在 Windows 上测试   
 */
static void
trapsig(SIGNAL_ARGS)
{
	/* 处理会重置处理函数的系统（Windows，唉）   */
	pqsignal(postgres_signal_arg, trapsig);
	caught_signal = true;
}

/*
 * 收到信号则调用 exit()，否则输出"ok"。  
 */
static void
check_ok(void)
{
	if (caught_signal)
	{
		printf(_("caught signal\n"));
		fflush(stdout);
		exit(1);
	}
	else if (output_failed)
	{
		printf(_("could not write to child process: %s\n"),
			   strerror(output_errno));
		fflush(stdout);
		exit(1);
	}
	else
	{
		/* 一切正常   */
		printf(_("ok\n"));
		fflush(stdout);
	}
}

/* hack：抑制某些版本 gcc 关于 %x 的警告             */
static inline size_t
my_strftime(char *s, size_t max, const char *fmt, const struct tm *tm)
{
	return strftime(s, max, fmt, tm);
}

/*
 * 据区域设置判断可能日期顺序
 */
static int
locale_date_order(const char *locale)
{
	struct tm	testtime;
	char		buf[128];
	char	   *posD;
	char	   *posM;
	char	   *posY;
	save_locale_t save;
	size_t		res;
	int			result;

	result = DATEORDER_MDY;		/* 默认  */

	save = save_global_locale(LC_TIME);

	setlocale(LC_TIME, locale);

	memset(&testtime, 0, sizeof(testtime));
	testtime.tm_mday = 22;
	testtime.tm_mon = 10;		/* 十一月，应输出为 "11"     */
	testtime.tm_year = 133;		/* 2033 */

	res = my_strftime(buf, sizeof(buf), "%x", &testtime);

	restore_global_locale(LC_TIME, save);

	if (res == 0)
		return result;

	posM = strstr(buf, "11");
	posD = strstr(buf, "22");
	posY = strstr(buf, "33");

	if (!posM || !posD || !posY)
		return result;

	if (posY < posM && posM < posD)
		result = DATEORDER_YMD;
	else if (posD < posM)
		result = DATEORDER_DMY;
	else
		result = DATEORDER_MDY;

	return result;
}

/*
 * 验证区域设置名对该区域设置类别是否有效
 *
 * 如果成功且 canonname 非 NULL，则在其中存储一个           
 * malloc'd 的区域设置规范名称副本。这对于弄清               
 * 空字符串""作为区域设置名称是什么含义（即       
 * 环境变量的值）。（实际上，在大多数实现中它           
 * 似乎仅此一用；我们本希望 setlocale 能返回规范拼写   
 * 的名称，但通常并非如此。）    
 *
 * 此处应与后端的 check_locale()函数保持一致 
 */
static void
check_locale_name(int category, const char *locale, char **canonname)
{
	save_locale_t save;
	char	   *res;

	/* 不接受Windows的非ASCII区域设置名。 */
	if (locale && !pg_is_ascii(locale))
		pg_fatal("locale name \"%s\" contains non-ASCII characters", locale);

	if (canonname)
		*canonname = NULL;		/* 失败时使用    */

	save = save_global_locale(category);

	/* 供 setlocale()调用*/
	if (!locale)
		locale = "";

	/* 以 setlocale 设置区域设置，看其是否接受。*/
	res = setlocale(category, locale);

	/* 若请求则保存规范名称。 */
	if (res && canonname)
		*canonname = pg_strdup(res);

	/* 恢复旧值。    */
	restore_global_locale(category, save);

	/* 若区域设置无效则报错  */
	if (res == NULL)
	{
		if (*locale)
		{
			pg_log_error("invalid locale name \"%s\"", locale);
			pg_log_error_hint("If the locale name is specific to ICU, use --icu-locale.");
			exit(1);
		}
		else
		{
			/*
			 * 若命令行未给出相关开关，locale 便是一个空   
			 * 字符串，报告起来没有太大用处。大概是       
			 * setlocale()在环境中发现了它不喜欢的东西。       
			 * 理想情况下，应报告有问题的环境变量，     
			 * 但 setlocale 的行为随实现而异，很难确定它不  
			 * 喜欢的是什么。请输出稳妥的通用消息。     
			 */
			pg_fatal("invalid locale settings; check LANG and LC_* environment variables");
		}
	}

	/* 不输出Windows的非ASCII区域设置名。  */
	if (canonname && !pg_is_ascii(*canonname))
		pg_fatal("locale name \"%s\" contains non-ASCII characters",
				 *canonname);
}

/*
 * 检查所选编码是否与区域设置所需的编码匹配
 *
 * 这应与后端 createdb() 中的类似检查保持一致。          
             */
static bool
check_locale_encoding(const char *locale, int user_enc)
{
	int			locale_enc;

	locale_enc = pg_get_encoding_from_locale(locale, true);

	/* 参见 createdb()中的说明以理解这些测试*/
	if (!(locale_enc == user_enc ||
		  locale_enc == PG_SQL_ASCII ||
		  locale_enc == -1 ||
#ifdef WIN32
		  user_enc == PG_UTF8 ||
#endif
		  user_enc == PG_SQL_ASCII))
	{
		pg_log_error("encoding mismatch");
		pg_log_error_detail("The encoding you selected (%s) and the encoding that the "
							"selected locale uses (%s) do not match. This would lead to "
							"misbehavior in various character string processing functions.",
							pg_encoding_to_char(user_enc),
							pg_encoding_to_char(locale_enc));
		pg_log_error_hint("Rerun %s and either do not specify an encoding explicitly, "
						  "or choose a matching combination.",
						  progname);
		return false;
	}
	return true;
}

/*
 * 检查所选编码是否受 ICU 支持
 *
 * 这应与后端 createdb() 中的类似检查保持一致。                  
           */
static bool
check_icu_locale_encoding(int user_enc)
{
	if (!(is_encoding_supported_by_icu(user_enc)))
	{
		pg_log_error("encoding mismatch");
		pg_log_error_detail("The encoding you selected (%s) is not supported with the ICU provider.",
							pg_encoding_to_char(user_enc));
		pg_log_error_hint("Rerun %s and either do not specify an encoding explicitly, "
						  "or choose a matching combination.",
						  progname);
		return false;
	}
	return true;
}

/*
 * 转换为规范的 BCP47 语言标签。必须与
 * icu_language_tag() 一致。
       */
static char *
icu_language_tag(const char *loc_str)
{
#ifdef USE_ICU
	UErrorCode	status;
	char	   *langtag;
	size_t		buflen = 32;	/* 任意起始缓冲区大小    */
	const bool	strict = true;

	/*
	 * BCP47 语言标签没有明确界定的上限（参见 RFC5646      
	 * 第 4.4 节）。此外，在较旧的 ICU 版本中，   
	 * uloc_toLanguageTag()未必在第一次调用时就返回             
	 * 最终长度，故需循环。   
	 */
	langtag = pg_malloc(buflen);
	while (true)
	{
		status = U_ZERO_ERROR;
		uloc_toLanguageTag(loc_str, langtag, buflen, strict, &status);

		/* 如果缓冲区不够大，再试一次     */
		if (status == U_BUFFER_OVERFLOW_ERROR ||
			status == U_STRING_NOT_TERMINATED_WARNING)
		{
			buflen = buflen * 2;
			langtag = pg_realloc(langtag, buflen);
			continue;
		}

		break;
	}

	if (U_FAILURE(status))
	{
		pg_free(langtag);

		pg_fatal("could not convert locale name \"%s\" to language tag: %s",
				 loc_str, u_errorName(status));
	}

	return langtag;
#else
	pg_fatal("ICU is not supported in this build");
	return NULL;				/* 使编译器安静  */
#endif
}

/*
 * 尽力检查区域设置是否有效。应与 pg_locale.c 保持   
 * 一致，只是无需打开排序器（那将在引导后初始化  
 * 期间进行）。                                               
 */
static void
icu_validate_locale(const char *loc_str)
{
#ifdef USE_ICU
	UErrorCode	status;
	char		lang[ULOC_LANG_CAPACITY];
	bool		found = false;

	/* 验证我们能否提取语言            */
	status = U_ZERO_ERROR;
	uloc_getLanguage(loc_str, lang, ULOC_LANG_CAPACITY, &status);
	if (U_FAILURE(status))
	{
		pg_fatal("could not get language from locale \"%s\": %s",
				 loc_str, u_errorName(status));
		return;
	}

	/* 检查特殊语言名称        */
	if (strcmp(lang, "") == 0 ||
		strcmp(lang, "root") == 0 || strcmp(lang, "und") == 0)
		found = true;

	/* 在 ICU 中搜索匹配的语言        */
	for (int32_t i = 0; !found && i < uloc_countAvailable(); i++)
	{
		const char *otherloc = uloc_getAvailable(i);
		char		otherlang[ULOC_LANG_CAPACITY];

		status = U_ZERO_ERROR;
		uloc_getLanguage(otherloc, otherlang, ULOC_LANG_CAPACITY, &status);
		if (U_FAILURE(status))
			continue;

		if (strcmp(lang, otherlang) == 0)
			found = true;
	}

	if (!found)
		pg_fatal("locale \"%s\" has unknown language \"%s\"",
				 loc_str, lang);
#else
	pg_fatal("ICU is not supported in this build");
#endif
}

/*
 * 设置各区域设置变量
 *
 * 假定已调用 setlocale(LC_ALL, "")——参见 set_pglocale_pgservice  
 */
static void
setlocales(void)
{
	char	   *canonname;

	/* 若已设置，将空lc_*与iculocale值设为locale配置  */

	if (locale)
	{
		if (!lc_ctype)
			lc_ctype = locale;
		if (!lc_collate)
			lc_collate = locale;
		if (!lc_numeric)
			lc_numeric = locale;
		if (!lc_time)
			lc_time = locale;
		if (!lc_monetary)
			lc_monetary = locale;
		if (!lc_messages)
			lc_messages = locale;
		if (!icu_locale && locale_provider == COLLPROVIDER_ICU)
			icu_locale = locale;
	}

	/*
	 * 规范化区域设置名称，并从当前环境中
	 * 获取任何缺失的值。
	       */
	check_locale_name(LC_CTYPE, lc_ctype, &canonname);
	lc_ctype = canonname;
	check_locale_name(LC_COLLATE, lc_collate, &canonname);
	lc_collate = canonname;
	check_locale_name(LC_NUMERIC, lc_numeric, &canonname);
	lc_numeric = canonname;
	check_locale_name(LC_TIME, lc_time, &canonname);
	lc_time = canonname;
	check_locale_name(LC_MONETARY, lc_monetary, &canonname);
	lc_monetary = canonname;
#if defined(LC_MESSAGES) && !defined(WIN32)
	check_locale_name(LC_MESSAGES, lc_messages, &canonname);
	lc_messages = canonname;
#else
	/* 当 LC_MESSAGES 不可用时，使用 LC_CTYPE 设置       */
	check_locale_name(LC_CTYPE, lc_messages, &canonname);
	lc_messages = canonname;
#endif

	if (locale_provider == COLLPROVIDER_ICU)
	{
		char	   *langtag;

		/* 如果未指定，从环境中获取默认区域设置        */
		if (icu_locale == NULL)
			pg_fatal("ICU locale must be specified");

		/* 规范化为语言标签       */
		langtag = icu_language_tag(icu_locale);
		printf(_("Using language tag \"%s\" for ICU locale \"%s\".\n"),
			   langtag, icu_locale);
		pg_free(icu_locale);
		icu_locale = langtag;

		icu_validate_locale(icu_locale);

		/*
		 * 在受支持的构建中，ICU区域设置ID将在引导后 
		 * 初始化期间打开，届时会执行额外的检查。      
		 */
#ifndef USE_ICU
		pg_fatal("ICU is not supported in this build");
#endif
	}
}

/*
 * 打印帮助   
 */
static void
usage(const char *progname)
{
	printf(_("%s initializes a PostgreSQL database cluster.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DATADIR]\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -A, --auth=METHOD         default authentication method for local connections\n"));
	printf(_("      --auth-host=METHOD    default authentication method for local TCP/IP connections\n"));
	printf(_("      --auth-local=METHOD   default authentication method for local-socket connections\n"));
	printf(_(" [-D, --pgdata=]DATADIR     location for this database cluster\n"));
	printf(_("  -E, --encoding=ENCODING   set default encoding for new databases\n"));
	printf(_("  -g, --allow-group-access  allow group read/execute on data directory\n"));
	printf(_("      --icu-locale=LOCALE   set ICU locale ID for new databases\n"));
	printf(_("      --icu-rules=RULES     set additional ICU collation rules for new databases\n"));
	printf(_("  -k, --data-checksums      use data page checksums\n"));
	printf(_("      --locale=LOCALE       set default locale for new databases\n"));
	printf(_("      --lc-collate=, --lc-ctype=, --lc-messages=LOCALE\n"
			 "      --lc-monetary=, --lc-numeric=, --lc-time=LOCALE\n"
			 "                            set default locale in the respective category for\n"
			 "                            new databases (default taken from environment)\n"));
	printf(_("      --no-locale           equivalent to --locale=C\n"));
	printf(_("      --locale-provider={libc|icu}\n"
			 "                            set default locale provider for new databases\n"));
	printf(_("      --pwfile=FILE         read password for the new superuser from file\n"));
	printf(_("  -T, --text-search-config=CFG\n"
			 "                            default text search configuration\n"));
	printf(_("  -U, --username=NAME       database superuser name\n"));
	printf(_("  -W, --pwprompt            prompt for a password for the new superuser\n"));
	printf(_("  -X, --waldir=WALDIR       location for the write-ahead log directory\n"));
	printf(_("      --wal-segsize=SIZE    size of WAL segments, in megabytes\n"));
	printf(_("\nLess commonly used options:\n"));
	printf(_("  -c, --set NAME=VALUE      override default setting for server parameter\n"));
	printf(_("  -d, --debug               generate lots of debugging output\n"));
	printf(_("      --discard-caches      set debug_discard_caches=1\n"));
	printf(_("  -L DIRECTORY              where to find the input files\n"));
	printf(_("  -n, --no-clean            do not clean up after errors\n"));
	printf(_("  -N, --no-sync             do not wait for changes to be written safely to disk\n"));
	printf(_("      --no-instructions     do not print instructions for next steps\n"));
	printf(_("  -s, --show                show internal settings\n"));
	printf(_("  -S, --sync-only           only sync database files to disk, then exit\n"));
	printf(_("\nOther options:\n"));
	printf(_("  -V, --version             output version information, then exit\n"));
	printf(_("  -?, --help                show this help, then exit\n"));
	printf(_("\nIf the data directory is not specified, the environment variable PGDATA\n"
			 "is used.\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

static void
check_authmethod_unspecified(const char **authmethod)
{
	if (*authmethod == NULL)
	{
		authwarning = true;
		*authmethod = "trust";
	}
}

static void
check_authmethod_valid(const char *authmethod, const char *const *valid_methods, const char *conntype)
{
	const char *const *p;

	for (p = valid_methods; *p; p++)
	{
		if (strcmp(authmethod, *p) == 0)
			return;
		/* 带空格 = 参数 */
		if (strchr(authmethod, ' '))
			if (strncmp(authmethod, *p, (authmethod - strchr(authmethod, ' '))) == 0)
				return;
	}

	pg_fatal("invalid authentication method \"%s\" for \"%s\" connections",
			 authmethod, conntype);
}

static void
check_need_password(const char *authmethodlocal, const char *authmethodhost)
{
	if ((strcmp(authmethodlocal, "md5") == 0 ||
		 strcmp(authmethodlocal, "password") == 0 ||
		 strcmp(authmethodlocal, "scram-sha-256") == 0) &&
		(strcmp(authmethodhost, "md5") == 0 ||
		 strcmp(authmethodhost, "password") == 0 ||
		 strcmp(authmethodhost, "scram-sha-256") == 0) &&
		!(pwprompt || pwfilename))
		pg_fatal("must specify a password for the superuser to enable password authentication");
}


void
setup_pgdata(void)
{
	char	   *pgdata_get_env;

	if (!pg_data)
	{
		pgdata_get_env = getenv("PGDATA");
		if (pgdata_get_env && strlen(pgdata_get_env))
		{
			/* 找到 PGDATA*/
			pg_data = pg_strdup(pgdata_get_env);
		}
		else
		{
			pg_log_error("no data directory specified");
			pg_log_error_hint("You must identify the directory where the data for this database system "
							  "will reside.  Do this with either the invocation option -D or the "
							  "environment variable PGDATA.");
			exit(1);
		}
	}

	pgdata_native = pg_strdup(pg_data);
	canonicalize_path(pg_data);

	/*
	 * 我们必须为 postgres 设置 PGDATA，而不是在命令行上    
	 * 传递它，以避免 Windows 上愚蠢的引号问题；而且在     
	 * Windows 上本来也特别需要引号，因为其路径最有         
	 * 可能含内嵌空格
	 */
	if (setenv("PGDATA", pg_data, 1) != 0)
		pg_fatal("could not set environment");
}


void
setup_bin_paths(const char *argv0)
{
	int			ret;

	if ((ret = find_other_exec(argv0, "postgres", PG_BACKEND_VERSIONSTR,
							   backend_exec)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv0, full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "postgres", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "postgres", full_path, progname);
	}

	/* 存储二进制目录  */
	strcpy(bin_path, backend_exec);
	*last_dir_separator(bin_path) = '\0';
	canonicalize_path(bin_path);

	if (!share_path)
	{
		share_path = pg_malloc(MAXPGPATH);
		get_share_path(backend_exec, share_path);
	}
	else if (!is_absolute_path(share_path))
		pg_fatal("input file location must be an absolute path");

	canonicalize_path(share_path);
}

void
setup_locale_encoding(void)
{
	setlocales();

	if (locale_provider == COLLPROVIDER_LIBC &&
		strcmp(lc_ctype, lc_collate) == 0 &&
		strcmp(lc_ctype, lc_time) == 0 &&
		strcmp(lc_ctype, lc_numeric) == 0 &&
		strcmp(lc_ctype, lc_monetary) == 0 &&
		strcmp(lc_ctype, lc_messages) == 0 &&
		(!icu_locale || strcmp(lc_ctype, icu_locale) == 0))
		printf(_("The database cluster will be initialized with locale \"%s\".\n"), lc_ctype);
	else
	{
		printf(_("The database cluster will be initialized with this locale configuration:\n"));
		printf(_("  provider:    %s\n"), collprovider_name(locale_provider));
		if (icu_locale)
			printf(_("  ICU locale:  %s\n"), icu_locale);
		printf(_("  LC_COLLATE:  %s\n"
				 "  LC_CTYPE:    %s\n"
				 "  LC_MESSAGES: %s\n"
				 "  LC_MONETARY: %s\n"
				 "  LC_NUMERIC:  %s\n"
				 "  LC_TIME:     %s\n"),
			   lc_collate,
			   lc_ctype,
			   lc_messages,
			   lc_monetary,
			   lc_numeric,
			   lc_time);
	}

	if (!encoding)
	{
		int			ctype_enc;

		ctype_enc = pg_get_encoding_from_locale(lc_ctype, true);

		/*
		 * 若 ctype_enc=SQL_ASCII，则它与任何编码都兼容。ICU     
		 * 不支持 SQL_ASCII，故改用 UTF-8。       
		 */
		if (locale_provider == COLLPROVIDER_ICU && ctype_enc == PG_SQL_ASCII)
			ctype_enc = PG_UTF8;

		if (ctype_enc == -1)
		{
			/* 无法识别区域设置的代码集    */
			pg_log_error("could not find suitable encoding for locale \"%s\"",
						 lc_ctype);
			pg_log_error_hint("Rerun %s with the -E option.", progname);
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit(1);
		}
		else if (!pg_valid_server_encoding_id(ctype_enc))
		{
			/*
			 * 我们识别了它，但并非合法的服务器编码。 
			 * Windows上，UTF-8兼容任何区域设置，故可回退到
			 * UTF-8 
			 */
#ifdef WIN32
			encodingid = PG_UTF8;
			printf(_("Encoding \"%s\" implied by locale is not allowed as a server-side encoding.\n"
					 "The default database encoding will be set to \"%s\" instead.\n"),
				   pg_encoding_to_char(ctype_enc),
				   pg_encoding_to_char(encodingid));
#else
			pg_log_error("locale \"%s\" requires unsupported encoding \"%s\"",
						 lc_ctype, pg_encoding_to_char(ctype_enc));
			pg_log_error_detail("Encoding \"%s\" is not allowed as a server-side encoding.",
								pg_encoding_to_char(ctype_enc));
			pg_log_error_hint("Rerun %s with a different locale selection.",
							  progname);
			exit(1);
#endif
		}
		else
		{
			encodingid = ctype_enc;
			printf(_("The default database encoding has accordingly been set to \"%s\".\n"),
				   pg_encoding_to_char(encodingid));
		}
	}
	else
		encodingid = get_encoding_id(encoding);

	if (!check_locale_encoding(lc_ctype, encodingid) ||
		!check_locale_encoding(lc_collate, encodingid))
		exit(1);				/* check_locale_encoding 已打印错误   */

	if (locale_provider == COLLPROVIDER_ICU &&
		!check_icu_locale_encoding(encodingid))
		exit(1);
}


void
setup_data_file_paths(void)
{
	set_input(&bki_file, "postgres.bki");
	set_input(&hba_file, "pg_hba.conf.sample");
	set_input(&ident_file, "pg_ident.conf.sample");
	set_input(&conf_file, "postgresql.conf.sample");
	set_input(&dictionary_file, "snowball_create.sql");
	set_input(&info_schema_file, "information_schema.sql");
	set_input(&features_file, "sql_features.txt");
	set_input(&system_constraints_file, "system_constraints.sql");
	set_input(&system_functions_file, "system_functions.sql");
	set_input(&system_views_file, "system_views.sql");

	if (show_setting || debug)
	{
		fprintf(stderr,
				"VERSION=%s\n"
				"PGDATA=%s\nshare_path=%s\nPGPATH=%s\n"
				"POSTGRES_SUPERUSERNAME=%s\nPOSTGRES_BKI=%s\n"
				"POSTGRESQL_CONF_SAMPLE=%s\n"
				"PG_HBA_SAMPLE=%s\nPG_IDENT_SAMPLE=%s\n",
				PG_VERSION,
				pg_data, share_path, bin_path,
				username, bki_file,
				conf_file,
				hba_file, ident_file);
		if (show_setting)
			exit(0);
	}

	check_input(bki_file);
	check_input(hba_file);
	check_input(ident_file);
	check_input(conf_file);
	check_input(dictionary_file);
	check_input(info_schema_file);
	check_input(features_file);
	check_input(system_constraints_file);
	check_input(system_functions_file);
	check_input(system_views_file);
}


void
setup_text_search(void)
{
	if (!default_text_search_config)
	{
		default_text_search_config = find_matching_ts_config(lc_ctype);
		if (!default_text_search_config)
		{
			pg_log_info("could not find suitable text search configuration for locale \"%s\"",
						lc_ctype);
			default_text_search_config = "simple";
		}
	}
	else
	{
		const char *checkmatch = find_matching_ts_config(lc_ctype);

		if (checkmatch == NULL)
		{
			pg_log_warning("suitable text search configuration for locale \"%s\" is unknown",
						   lc_ctype);
		}
		else if (strcmp(checkmatch, default_text_search_config) != 0)
		{
			pg_log_warning("specified text search configuration \"%s\" might not match locale \"%s\"",
						   default_text_search_config, lc_ctype);
		}
	}

	printf(_("The default text search configuration will be set to \"%s\".\n"),
		   default_text_search_config);
}


void
setup_signals(void)
{
	/* 其中一些在 Windows 上无效      */
#ifdef SIGHUP
	pqsignal(SIGHUP, trapsig);
#endif
#ifdef SIGINT
	pqsignal(SIGINT, trapsig);
#endif
#ifdef SIGQUIT
	pqsignal(SIGQUIT, trapsig);
#endif
#ifdef SIGTERM
	pqsignal(SIGTERM, trapsig);
#endif

	/* 写入后端时忽略 SIGPIPE，以便我们能进行清理*/
#ifdef SIGPIPE
	pqsignal(SIGPIPE, SIG_IGN);
#endif

	/* 屏蔽 SIGSYS，以便探测可能无法工作的内核调用       */
#ifdef SIGSYS
	pqsignal(SIGSYS, SIG_IGN);
#endif
}


void
create_data_directory(void)
{
	int			ret;

	switch ((ret = pg_check_dir(pg_data)))
	{
		case 0:
			/* PGDATA 不存在，必须创建  */
			printf(_("creating directory %s ... "),
				   pg_data);
			fflush(stdout);

			if (pg_mkdir_p(pg_data, pg_dir_create_mode) != 0)
				pg_fatal("could not create directory \"%s\": %m", pg_data);
			else
				check_ok();

			made_new_pgdata = true;
			break;

		case 1:
			/* 存在但为空，修复权限并使用它    */
			printf(_("fixing permissions on existing directory %s ... "),
				   pg_data);
			fflush(stdout);

			if (chmod(pg_data, pg_dir_create_mode) != 0)
				pg_fatal("could not change permissions of directory \"%s\": %m",
						 pg_data);
			else
				check_ok();

			found_existing_pgdata = true;
			break;

		case 2:
		case 3:
		case 4:
			/* 存在且不为空    */
			pg_log_error("directory \"%s\" exists but is not empty", pg_data);
			if (ret != 4)
				warn_on_mount_point(ret);
			else
				pg_log_error_hint("If you want to create a new database system, either remove or empty "
								  "the directory \"%s\" or run %s "
								  "with an argument other than \"%s\".",
								  pg_data, progname, pg_data);
			exit(1);			/* 无需进一步的消息  */

		default:
			/* 访问目录时遇到问题 */
			pg_fatal("could not access directory \"%s\": %m", pg_data);
	}
}


/* 创建WAL目录，需要时建立符号链接 */
void
create_xlog_or_symlink(void)
{
	char	   *subdirloc;

	/* 形成子目录或符号链接位置的名称          */
	subdirloc = psprintf("%s/pg_wal", pg_data);

	if (xlog_dir)
	{
		int			ret;

		/* 清理xlog目录名，检查是否为绝对路径 */
		canonicalize_path(xlog_dir);
		if (!is_absolute_path(xlog_dir))
			pg_fatal("WAL directory location must be an absolute path");

		/* 检查指定的 xlog 目录是否存在/是否为空  */
		switch ((ret = pg_check_dir(xlog_dir)))
		{
			case 0:
				/* xlog 目录不存在，必须创建它   */
				printf(_("creating directory %s ... "),
					   xlog_dir);
				fflush(stdout);

				if (pg_mkdir_p(xlog_dir, pg_dir_create_mode) != 0)
					pg_fatal("could not create directory \"%s\": %m",
							 xlog_dir);
				else
					check_ok();

				made_new_xlogdir = true;
				break;

			case 1:
				/* 存在但为空，修复权限并使用它    */
				printf(_("fixing permissions on existing directory %s ... "),
					   xlog_dir);
				fflush(stdout);

				if (chmod(xlog_dir, pg_dir_create_mode) != 0)
					pg_fatal("could not change permissions of directory \"%s\": %m",
							 xlog_dir);
				else
					check_ok();

				found_existing_xlogdir = true;
				break;

			case 2:
			case 3:
			case 4:
				/* 存在且不为空    */
				pg_log_error("directory \"%s\" exists but is not empty", xlog_dir);
				if (ret != 4)
					warn_on_mount_point(ret);
				else
					pg_log_error_hint("If you want to store the WAL there, either remove or empty the directory \"%s\".",
									  xlog_dir);
				exit(1);

			default:
				/* 访问目录时遇到问题 */
				pg_fatal("could not access directory \"%s\": %m", xlog_dir);
		}

		if (symlink(xlog_dir, subdirloc) != 0)
			pg_fatal("could not create symbolic link \"%s\": %m",
					 subdirloc);
	}
	else
	{
		/* 没有 -X 选项，只需正常创建子目录         */
		if (mkdir(subdirloc, pg_dir_create_mode) < 0)
			pg_fatal("could not create directory \"%s\": %m",
					 subdirloc);
	}

	free(subdirloc);
}


void
warn_on_mount_point(int error)
{
	if (error == 2)
		pg_log_error_detail("It contains a dot-prefixed/invisible file, perhaps due to it being a mount point.");
	else if (error == 3)
		pg_log_error_detail("It contains a lost+found directory, perhaps due to it being a mount point.");

	pg_log_error_hint("Using a mount point directly as the data directory is not recommended.\n"
					  "Create a subdirectory under the mount point.");
}


void
initialize_data_directory(void)
{
	PG_CMD_DECL;
	int			i;

	setup_signals();

	/*
	 * 根据所要求的 PGDATA 权限设置掩码。pg_mode_mask       
	 * 及其同类（如 pg_dir_create_mode）默认被设为仅           
	 * 属主可访问；当解析选项（见上文）传入-g时，会调用
	 * SetDataDirectoryCreatePerm()更新。
	 */
	umask(pg_mode_mask);

	create_data_directory();

	create_xlog_or_symlink();

	/* 创建所需的子目录（pg_wal 除外）        */
	printf(_("creating subdirectories ... "));
	fflush(stdout);

	for (i = 0; i < lengthof(subdirs); i++)
	{
		char	   *path;

		path = psprintf("%s/%s", pg_data, subdirs[i]);

		/*
		 * 父目录已存在，因此这里只需 mkdir()而无需        
		 * pg_mkdir_p()，从而避免某些失败模式；见 bug #13853。 
		 */
		if (mkdir(path, pg_dir_create_mode) < 0)
			pg_fatal("could not create directory \"%s\": %m", path);

		free(path);
	}

	check_ok();

	/* 顶级 PG_VERSION 由引导程序检查，因此先创建它      */
	write_version_file(NULL);

	/* 选择合适的配置设置            */
	set_null_conf();
	test_config_settings();

	/* 现在创建所有文本配置文件 */
	setup_config();

	/* 引导 template1    */
	bootstrap_template1();

	/*
	 * 仅在初始化 template1 后才创建其每数据库的 PG_VERSION  
	 */
	write_version_file("base/1");

	/*
	 * 创建我们不需要使用引导模式的内容，
	 * 使用运行在简单独立模式下的后端。
	         */
	fputs(_("performing post-bootstrap initialization ... "), stdout);
	fflush(stdout);

	snprintf(cmd, sizeof(cmd),
			 "\"%s\" %s %s template1 >%s",
			 backend_exec, backend_options, extra_options,
			 DEVNULL);

	PG_CMD_OPEN;

	setup_auth(cmdfd);

	setup_run_file(cmdfd, system_constraints_file);

	setup_run_file(cmdfd, system_functions_file);

	setup_depend(cmdfd);

	/*
	 * 请注意，在 setup_depend() 之后创建的对象不会
	 * 被"固定"。它们都是可删除的。
	               */

	setup_run_file(cmdfd, system_views_file);

	setup_description(cmdfd);

	setup_collation(cmdfd);

	setup_run_file(cmdfd, dictionary_file);

	setup_privileges(cmdfd);

	setup_schema(cmdfd);

	load_plpgsql(cmdfd);

	vacuum_db(cmdfd);

	make_template0(cmdfd);

	make_postgres(cmdfd);

	PG_CMD_CLOSE;

	check_ok();
}


int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"encoding", required_argument, NULL, 'E'},
		{"locale", required_argument, NULL, 1},
		{"lc-collate", required_argument, NULL, 2},
		{"lc-ctype", required_argument, NULL, 3},
		{"lc-monetary", required_argument, NULL, 4},
		{"lc-numeric", required_argument, NULL, 5},
		{"lc-time", required_argument, NULL, 6},
		{"lc-messages", required_argument, NULL, 7},
		{"no-locale", no_argument, NULL, 8},
		{"text-search-config", required_argument, NULL, 'T'},
		{"auth", required_argument, NULL, 'A'},
		{"auth-local", required_argument, NULL, 10},
		{"auth-host", required_argument, NULL, 11},
		{"pwprompt", no_argument, NULL, 'W'},
		{"pwfile", required_argument, NULL, 9},
		{"username", required_argument, NULL, 'U'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},
		{"debug", no_argument, NULL, 'd'},
		{"show", no_argument, NULL, 's'},
		{"noclean", no_argument, NULL, 'n'},	/* 用于向后兼容          */
		{"no-clean", no_argument, NULL, 'n'},
		{"nosync", no_argument, NULL, 'N'}, /* 用于向后兼容          */
		{"no-sync", no_argument, NULL, 'N'},
		{"no-instructions", no_argument, NULL, 13},
		{"set", required_argument, NULL, 'c'},
		{"sync-only", no_argument, NULL, 'S'},
		{"waldir", required_argument, NULL, 'X'},
		{"wal-segsize", required_argument, NULL, 12},
		{"data-checksums", no_argument, NULL, 'k'},
		{"allow-group-access", no_argument, NULL, 'g'},
		{"discard-caches", no_argument, NULL, 14},
		{"locale-provider", required_argument, NULL, 15},
		{"icu-locale", required_argument, NULL, 16},
		{"icu-rules", required_argument, NULL, 17},
		{NULL, 0, NULL, 0}
	};

	/*
	 * 没有短版本的选项返回一个小的整数，其余的
	 * 返回它们的短版本值。
	   */
	int			c;
	int			option_index;
	char	   *effective_user;
	PQExpBuffer start_db_cmd;
	char		pg_ctl_path[MAXPGPATH];

	/*
	 * 确保 stdout 的缓冲行为与交互式使用时            
	 * 一致（至少在大多数平台上）。这可避免        
	 * 输出重定向到文件时出现意外的输出顺序，           
	 * POSIX 要求在对这些文件的其他使用前完成此操作  
	 */
	setvbuf(stdout, NULL, PG_IOLBF, 0);

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("initdb"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("initdb (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	/* 处理命令行选项        */

	while ((c = getopt_long(argc, argv, "A:c:dD:E:gkL:nNsST:U:WX:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'A':
				authmethodlocal = authmethodhost = pg_strdup(optarg);

				/*
				 * 指定 ident 时，本地连接用 peer；而指定 peer
				 * 时，则对 TCP/IP 连接使用 ident。             
				 *             
				 */
				if (strcmp(authmethodhost, "ident") == 0)
					authmethodlocal = "peer";
				else if (strcmp(authmethodlocal, "peer") == 0)
					authmethodhost = "ident";
				break;
			case 10:
				authmethodlocal = pg_strdup(optarg);
				break;
			case 11:
				authmethodhost = pg_strdup(optarg);
				break;
			case 'c':
				{
					char	   *buf = pg_strdup(optarg);
					char	   *equals = strchr(buf, '=');

					if (!equals)
					{
						pg_log_error("-c %s requires a value", buf);
						pg_log_error_hint("Try \"%s --help\" for more information.",
										  progname);
						exit(1);
					}
					*equals++ = '\0';	/* 终止变量名         */
					add_stringlist_item(&extra_guc_names, buf);
					add_stringlist_item(&extra_guc_values, equals);
					pfree(buf);
				}
				break;
			case 'D':
				pg_data = pg_strdup(optarg);
				break;
			case 'E':
				encoding = pg_strdup(optarg);
				break;
			case 'W':
				pwprompt = true;
				break;
			case 'U':
				username = pg_strdup(optarg);
				break;
			case 'd':
				debug = true;
				printf(_("Running in debug mode.\n"));
				break;
			case 'n':
				noclean = true;
				printf(_("Running in no-clean mode.  Mistakes will not be cleaned up.\n"));
				break;
			case 'N':
				do_sync = false;
				break;
			case 'S':
				sync_only = true;
				break;
			case 'k':
				data_checksums = true;
				break;
			case 'L':
				share_path = pg_strdup(optarg);
				break;
			case 1:
				locale = pg_strdup(optarg);
				break;
			case 2:
				lc_collate = pg_strdup(optarg);
				break;
			case 3:
				lc_ctype = pg_strdup(optarg);
				break;
			case 4:
				lc_monetary = pg_strdup(optarg);
				break;
			case 5:
				lc_numeric = pg_strdup(optarg);
				break;
			case 6:
				lc_time = pg_strdup(optarg);
				break;
			case 7:
				lc_messages = pg_strdup(optarg);
				break;
			case 8:
				locale = "C";
				break;
			case 9:
				pwfilename = pg_strdup(optarg);
				break;
			case 's':
				show_setting = true;
				break;
			case 'T':
				default_text_search_config = pg_strdup(optarg);
				break;
			case 'X':
				xlog_dir = pg_strdup(optarg);
				break;
			case 12:
				str_wal_segment_size_mb = pg_strdup(optarg);
				break;
			case 13:
				noinstructions = true;
				break;
			case 'g':
				SetDataDirectoryCreatePerm(PG_DIR_MODE_GROUP);
				break;
			case 14:
				extra_options = psprintf("%s %s",
										 extra_options,
										 "-c debug_discard_caches=1");
				break;
			case 15:
				if (strcmp(optarg, "icu") == 0)
					locale_provider = COLLPROVIDER_ICU;
				else if (strcmp(optarg, "libc") == 0)
					locale_provider = COLLPROVIDER_LIBC;
				else
					pg_fatal("unrecognized locale provider: %s", optarg);
				break;
			case 16:
				icu_locale = pg_strdup(optarg);
				break;
			case 17:
				icu_rules = pg_strdup(optarg);
				break;
			default:
				/* getopt_long 已经发出了警告       */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}


	/*
	 * 非选项参数指定数据目录，只要它尚未
	 * 通过 -D 指定。
	                                */
	if (optind < argc && !pg_data)
	{
		pg_data = pg_strdup(argv[optind]);
		optind++;
	}

	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (icu_locale && locale_provider != COLLPROVIDER_ICU)
		pg_fatal("%s cannot be specified unless locale provider \"%s\" is chosen",
				 "--icu-locale", "icu");

	if (icu_rules && locale_provider != COLLPROVIDER_ICU)
		pg_fatal("%s cannot be specified unless locale provider \"%s\" is chosen",
				 "--icu-rules", "icu");

	atexit(cleanup_directories_atexit);

	/* 如果只需要 fsync，直接执行并退出 */
	if (sync_only)
	{
		setup_pgdata();

		/* 必须检查目录是否可读        */
		if (pg_check_dir(pg_data) <= 0)
			pg_fatal("could not access directory \"%s\": %m", pg_data);

		fputs(_("syncing data to disk ... "), stdout);
		fflush(stdout);
		fsync_pgdata(pg_data, PG_VERSION_NUM);
		check_ok();
		return 0;
	}

	if (pwprompt && pwfilename)
		pg_fatal("password prompt and password file cannot be specified together");

	check_authmethod_unspecified(&authmethodlocal);
	check_authmethod_unspecified(&authmethodhost);

	check_authmethod_valid(authmethodlocal, auth_methods_local, "local");
	check_authmethod_valid(authmethodhost, auth_methods_host, "host");

	check_need_password(authmethodlocal, authmethodhost);

	/* 设置 wal 段大小 */
	if (str_wal_segment_size_mb == NULL)
		wal_segment_size_mb = (DEFAULT_XLOG_SEG_SIZE) / (1024 * 1024);
	else
	{
		char	   *endptr;

		/* 检查参数是否为数字         */
		wal_segment_size_mb = strtol(str_wal_segment_size_mb, &endptr, 10);

		/* 验证 WAL 段大小是否有效      */
		if (endptr == str_wal_segment_size_mb || *endptr != '\0')
			pg_fatal("argument of --wal-segsize must be a number");
		if (!IsValidWalSegSize(wal_segment_size_mb * 1024 * 1024))
			pg_fatal("argument of --wal-segsize must be a power of two between 1 and 1024");
	}

	get_restricted_token();

	setup_pgdata();

	setup_bin_paths(argv[0]);

	effective_user = get_id();
	if (!username)
		username = effective_user;

	if (strncmp(username, "pg_", 3) == 0)
		pg_fatal("superuser name \"%s\" is disallowed; role names cannot begin with \"pg_\"", username);

	printf(_("The files belonging to this database system will be owned "
			 "by user \"%s\".\n"
			 "This user must also own the server process.\n\n"),
		   effective_user);

	set_info_version();

	setup_data_file_paths();

	setup_locale_encoding();

	setup_text_search();

	printf("\n");

	if (data_checksums)
		printf(_("Data page checksums are enabled.\n"));
	else
		printf(_("Data page checksums are disabled.\n"));

	if (pwprompt || pwfilename)
		get_su_pwd();

	printf("\n");

	initialize_data_directory();

	if (do_sync)
	{
		fputs(_("syncing data to disk ... "), stdout);
		fflush(stdout);
		fsync_pgdata(pg_data, PG_VERSION_NUM);
		check_ok();
	}
	else
		printf(_("\nSync to disk skipped.\nThe data directory might become corrupt if the operating system crashes.\n"));

	if (authwarning)
	{
		printf("\n");
		pg_log_warning("enabling \"trust\" authentication for local connections");
		pg_log_warning_hint("You can change this by editing pg_hba.conf or using the option -A, or "
							"--auth-local and --auth-host, the next time you run initdb.");
	}

	if (!noinstructions)
	{
		/*
		 * 构建一个 shell 命令来告诉用户如何启动服务器
		     */
		start_db_cmd = createPQExpBuffer();

		/* 获取用于启动 initdb 的目录规范 ...        */
		strlcpy(pg_ctl_path, argv[0], sizeof(pg_ctl_path));
		canonicalize_path(pg_ctl_path);
		get_parent_directory(pg_ctl_path);
		/* ... 然后替换为 pg_ctl    */
		join_path_components(pg_ctl_path, pg_ctl_path, "pg_ctl");

		/* 将路径转换为使用本地分隔符   */
		make_native_path(pg_ctl_path);

		/* 已正确引用的 pg_ctl 路径*/
		appendShellString(start_db_cmd, pg_ctl_path);

		/* 添加 -D 开关，数据目录适当加引号     */
		appendPQExpBufferStr(start_db_cmd, " -D ");
		appendShellString(start_db_cmd, pgdata_native);

		/* 添加建议的 -l 开关和 "start" 命令 */
		/* translator: This is a placeholder in a shell command. */
		appendPQExpBuffer(start_db_cmd, " -l %s start", _("logfile"));

		printf(_("\nSuccess. You can now start the database server using:\n\n"
				 "    %s\n\n"),
			   start_db_cmd->data);

		destroyPQExpBuffer(start_db_cmd);
	}


	success = true;
	return 0;
}
