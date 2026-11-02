/*-------------------------------------------------------------------------
 *
 * pg_dumpall.c
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * pg_dumpall forces all pg_dump output to be text, since it also outputs
 * text into the same output stream.
 *
 * src/bin/pg_dump/pg_dumpall.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <time.h>
#include <unistd.h>

#include "catalog/pg_authid_d.h"
#include "common/connect.h"
#include "common/file_utils.h"
#include "common/hashfn.h"
#include "common/logging.h"
#include "common/string.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "pg_backup.h"

/* 期望从 pg_dump 收到的版本字符串 */
#define PGDUMP_VERSIONSTR "pg_dump (PostgreSQL) " PG_VERSION "\n"

static uint32 hash_string_pointer(char *s);

typedef struct
{
	uint32		status;
	uint32		hashval;
	char	   *rolename;
} RoleNameEntry;

#define SH_PREFIX	rolename
#define SH_ELEMENT_TYPE	RoleNameEntry
#define SH_KEY_TYPE	char *
#define SH_KEY		rolename
#define SH_HASH_KEY(tb, key)	hash_string_pointer(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a)		(a)->hashval
#define SH_SCOPE	static inline
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static void help(void);

static void dropRoles(PGconn *conn);
static void dumpRoles(PGconn *conn);
static void dumpRoleMembership(PGconn *conn);
static void dumpRoleGUCPrivs(PGconn *conn);
static void dropTablespaces(PGconn *conn);
static void dumpTablespaces(PGconn *conn);
static void dropDBs(PGconn *conn);
static void dumpUserConfig(PGconn *conn, const char *username);
static void dumpDatabases(PGconn *conn);
static void dumpTimestamp(const char *msg);
static int	runPgDump(const char *dbname, const char *create_opts);
static void buildShSecLabels(PGconn *conn,
							 const char *catalog_name, Oid objectId,
							 const char *objtype, const char *objname,
							 PQExpBuffer buffer);
static PGconn *connectDatabase(const char *dbname,
							   const char *connection_string, const char *pghost,
							   const char *pgport, const char *pguser,
							   trivalue prompt_password, bool fail_on_error);
static char *constructConnStr(const char **keywords, const char **values);
static PGresult *executeQuery(PGconn *conn, const char *query);
static void executeCommand(PGconn *conn, const char *query);
static void expand_dbname_patterns(PGconn *conn, SimpleStringList *patterns,
								   SimpleStringList *names);

static char pg_dump_bin[MAXPGPATH];
static const char *progname;
static PQExpBuffer pgdumpopts;
static char *connstr = "";
static bool output_clean = false;
static bool skip_acls = false;
static bool verbose = false;
static bool dosync = true;

static int	binary_upgrade = 0;
static int	column_inserts = 0;
static int	disable_dollar_quoting = 0;
static int	disable_triggers = 0;
static int	if_exists = 0;
static int	inserts = 0;
static int	no_table_access_method = 0;
static int	no_tablespaces = 0;
static int	use_setsessauth = 0;
static int	no_comments = 0;
static int	no_publications = 0;
static int	no_security_labels = 0;
static int	no_subscriptions = 0;
static int	no_toast_compression = 0;
static int	no_unlogged_table_data = 0;
static int	no_role_passwords = 0;
static int	server_version;
static int	load_via_partition_root = 0;
static int	on_conflict_do_nothing = 0;

static char role_catalog[10];
#define PG_AUTHID "pg_authid"
#define PG_ROLES  "pg_roles "

static FILE *OPF;
static char *filename = NULL;

static SimpleStringList database_exclude_patterns = {NULL, NULL};
static SimpleStringList database_exclude_names = {NULL, NULL};

static char *restrict_key;

#define exit_nicely(code) exit(code)

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"clean", no_argument, NULL, 'c'},
		{"encoding", required_argument, NULL, 'E'},
		{"file", required_argument, NULL, 'f'},
		{"globals-only", no_argument, NULL, 'g'},
		{"host", required_argument, NULL, 'h'},
		{"dbname", required_argument, NULL, 'd'},
		{"database", required_argument, NULL, 'l'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"roles-only", no_argument, NULL, 'r'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"tablespaces-only", no_argument, NULL, 't'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},

		/*
			 * 以下选项没有对应的短选项字母
			 */
		{"attribute-inserts", no_argument, &column_inserts, 1},
		{"binary-upgrade", no_argument, &binary_upgrade, 1},
		{"column-inserts", no_argument, &column_inserts, 1},
		{"disable-dollar-quoting", no_argument, &disable_dollar_quoting, 1},
		{"disable-triggers", no_argument, &disable_triggers, 1},
		{"exclude-database", required_argument, NULL, 6},
		{"extra-float-digits", required_argument, NULL, 5},
		{"if-exists", no_argument, &if_exists, 1},
		{"inserts", no_argument, &inserts, 1},
		{"lock-wait-timeout", required_argument, NULL, 2},
		{"no-table-access-method", no_argument, &no_table_access_method, 1},
		{"no-tablespaces", no_argument, &no_tablespaces, 1},
		{"quote-all-identifiers", no_argument, &quote_all_identifiers, 1},
		{"load-via-partition-root", no_argument, &load_via_partition_root, 1},
		{"role", required_argument, NULL, 3},
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"no-comments", no_argument, &no_comments, 1},
		{"no-publications", no_argument, &no_publications, 1},
		{"no-role-passwords", no_argument, &no_role_passwords, 1},
		{"no-security-labels", no_argument, &no_security_labels, 1},
		{"no-subscriptions", no_argument, &no_subscriptions, 1},
		{"no-sync", no_argument, NULL, 4},
		{"no-toast-compression", no_argument, &no_toast_compression, 1},
		{"no-unlogged-table-data", no_argument, &no_unlogged_table_data, 1},
		{"on-conflict-do-nothing", no_argument, &on_conflict_do_nothing, 1},
		{"rows-per-insert", required_argument, NULL, 7},
		{"restrict-key", required_argument, NULL, 9},

		{NULL, 0, NULL, 0}
	};

	char	   *pghost = NULL;
	char	   *pgport = NULL;
	char	   *pguser = NULL;
	char	   *pgdb = NULL;
	char	   *use_role = NULL;
	const char *dumpencoding = NULL;
	trivalue	prompt_password = TRI_DEFAULT;
	bool		data_only = false;
	bool		globals_only = false;
	bool		roles_only = false;
	bool		tablespaces_only = false;
	PGconn	   *conn;
	int			encoding;
	const char *std_strings;
	int			c,
				ret;
	int			optindex;

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_dump"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help();
			exit_nicely(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dumpall (PostgreSQL) " PG_VERSION);
			exit_nicely(0);
		}
	}

	if ((ret = find_other_exec(argv[0], "pg_dump", PGDUMP_VERSIONSTR,
							   pg_dump_bin)) < 0)
	{
		char		full_path[MAXPGPATH];

		if (find_my_exec(argv[0], full_path) < 0)
			strlcpy(full_path, progname, sizeof(full_path));

		if (ret == -1)
			pg_fatal("program \"%s\" is needed by %s but was not found in the same directory as \"%s\"",
					 "pg_dump", progname, full_path);
		else
			pg_fatal("program \"%s\" was found by \"%s\" but was not the same version as %s",
					 "pg_dump", full_path, progname);
	}

	pgdumpopts = createPQExpBuffer();

	while ((c = getopt_long(argc, argv, "acd:E:f:gh:l:Op:rsS:tU:vwWx", long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':
				data_only = true;
				appendPQExpBufferStr(pgdumpopts, " -a");
				break;

			case 'c':
				output_clean = true;
				break;

			case 'd':
				connstr = pg_strdup(optarg);
				break;

			case 'E':
				dumpencoding = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " -E ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 'f':
				filename = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " -f ");
				appendShellString(pgdumpopts, filename);
				break;

			case 'g':
				globals_only = true;
				break;

			case 'h':
				pghost = pg_strdup(optarg);
				break;

			case 'l':
				pgdb = pg_strdup(optarg);
				break;

			case 'O':
				appendPQExpBufferStr(pgdumpopts, " -O");
				break;

			case 'p':
				pgport = pg_strdup(optarg);
				break;

			case 'r':
				roles_only = true;
				break;

			case 's':
				appendPQExpBufferStr(pgdumpopts, " -s");
				break;

			case 'S':
				appendPQExpBufferStr(pgdumpopts, " -S ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 't':
				tablespaces_only = true;
				break;

			case 'U':
				pguser = pg_strdup(optarg);
				break;

			case 'v':
				verbose = true;
				pg_logging_increase_verbosity();
				appendPQExpBufferStr(pgdumpopts, " -v");
				break;

			case 'w':
				prompt_password = TRI_NO;
				appendPQExpBufferStr(pgdumpopts, " -w");
				break;

			case 'W':
				prompt_password = TRI_YES;
				appendPQExpBufferStr(pgdumpopts, " -W");
				break;

			case 'x':
				skip_acls = true;
				appendPQExpBufferStr(pgdumpopts, " -x");
				break;

			case 0:
				break;

			case 2:
				appendPQExpBufferStr(pgdumpopts, " --lock-wait-timeout ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 3:
				use_role = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " --role ");
				appendShellString(pgdumpopts, use_role);
				break;

			case 4:
				dosync = false;
				appendPQExpBufferStr(pgdumpopts, " --no-sync");
				break;

			case 5:
				appendPQExpBufferStr(pgdumpopts, " --extra-float-digits ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 6:
				simple_string_list_append(&database_exclude_patterns, optarg);
				break;

			case 7:
				appendPQExpBufferStr(pgdumpopts, " --rows-per-insert ");
				appendShellString(pgdumpopts, optarg);
				break;

			case 9:
				restrict_key = pg_strdup(optarg);
				appendPQExpBufferStr(pgdumpopts, " --restrict-key ");
				appendShellString(pgdumpopts, optarg);
				break;

			default:
				/* getopt_long 已经输出了错误信息 */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit_nicely(1);
		}
	}

	/* 如果有剩余参数则报错 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	if (database_exclude_patterns.head != NULL &&
		(globals_only || roles_only || tablespaces_only))
	{
		pg_log_error("option --exclude-database cannot be used together with -g/--globals-only, -r/--roles-only, or -t/--tablespaces-only");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/* 确保用户没有混用 globals-only 相关的选项 */
	if (globals_only && roles_only)
	{
		pg_log_error("options -g/--globals-only and -r/--roles-only cannot be used together");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	if (globals_only && tablespaces_only)
	{
		pg_log_error("options -g/--globals-only and -t/--tablespaces-only cannot be used together");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	if (if_exists && !output_clean)
		pg_fatal("option --if-exists requires option -c/--clean");

	if (roles_only && tablespaces_only)
	{
		pg_log_error("options -r/--roles-only and -t/--tablespaces-only cannot be used together");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/*
	 * 如果导出中不需要密码值，则切换为使用 pg_roles，
	 * 它同样有用，且比 pg_authid 更可能具有不受限制的
	 * 访问权限。
	 */
	if (no_role_passwords)
		sprintf(role_catalog, "%s", PG_ROLES);
	else
		sprintf(role_catalog, "%s", PG_AUTHID);

	/* 将长选项添加到 pg_dump 参数列表 */
	if (binary_upgrade)
		appendPQExpBufferStr(pgdumpopts, " --binary-upgrade");
	if (column_inserts)
		appendPQExpBufferStr(pgdumpopts, " --column-inserts");
	if (disable_dollar_quoting)
		appendPQExpBufferStr(pgdumpopts, " --disable-dollar-quoting");
	if (disable_triggers)
		appendPQExpBufferStr(pgdumpopts, " --disable-triggers");
	if (inserts)
		appendPQExpBufferStr(pgdumpopts, " --inserts");
	if (no_table_access_method)
		appendPQExpBufferStr(pgdumpopts, " --no-table-access-method");
	if (no_tablespaces)
		appendPQExpBufferStr(pgdumpopts, " --no-tablespaces");
	if (quote_all_identifiers)
		appendPQExpBufferStr(pgdumpopts, " --quote-all-identifiers");
	if (load_via_partition_root)
		appendPQExpBufferStr(pgdumpopts, " --load-via-partition-root");
	if (use_setsessauth)
		appendPQExpBufferStr(pgdumpopts, " --use-set-session-authorization");
	if (no_comments)
		appendPQExpBufferStr(pgdumpopts, " --no-comments");
	if (no_publications)
		appendPQExpBufferStr(pgdumpopts, " --no-publications");
	if (no_security_labels)
		appendPQExpBufferStr(pgdumpopts, " --no-security-labels");
	if (no_subscriptions)
		appendPQExpBufferStr(pgdumpopts, " --no-subscriptions");
	if (no_toast_compression)
		appendPQExpBufferStr(pgdumpopts, " --no-toast-compression");
	if (no_unlogged_table_data)
		appendPQExpBufferStr(pgdumpopts, " --no-unlogged-table-data");
	if (on_conflict_do_nothing)
		appendPQExpBufferStr(pgdumpopts, " --on-conflict-do-nothing");

	/*
	 * 如果不提供限制键，系统会自动生成一个。
	 */
	if (!restrict_key)
		restrict_key = generate_restrict_key();
	if (!restrict_key)
		pg_fatal("could not generate restrict key");
	if (!valid_restrict_key(restrict_key))
		pg_fatal("invalid restrict key");

	/*
	 * 如果在命令行指定了数据库，则使用该数据库，
	 * 否则尝试连接数据库 "postgres"，如果失败则连接
	 * "template1"。
	 */
	if (pgdb)
	{
		conn = connectDatabase(pgdb, connstr, pghost, pgport, pguser,
							   prompt_password, false);

		if (!conn)
			pg_fatal("could not connect to database \"%s\"", pgdb);
	}
	else
	{
		conn = connectDatabase("postgres", connstr, pghost, pgport, pguser,
							   prompt_password, false);
		if (!conn)
			conn = connectDatabase("template1", connstr, pghost, pgport, pguser,
								   prompt_password, true);

		if (!conn)
		{
			pg_log_error("could not connect to databases \"postgres\" or \"template1\"\n"
						 "Please specify an alternative database.");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit_nicely(1);
		}
	}

	/*
	 * 获取匹配排除模式的数据库名称列表
	 */
	expand_dbname_patterns(conn, &database_exclude_patterns,
						   &database_exclude_names);

	/*
	 * 如果需要则打开输出文件，否则使用 stdout
	 */
	if (filename)
	{
		OPF = fopen(filename, PG_BINARY_W);
		if (!OPF)
			pg_fatal("could not open output file \"%s\": %m",
					 filename);
	}
	else
		OPF = stdout;

	/*
	 * 如果请求了则设置客户端编码。
	 */
	if (dumpencoding)
	{
		if (PQsetClientEncoding(conn, dumpencoding) < 0)
			pg_fatal("invalid client encoding \"%s\" specified",
					 dumpencoding);
	}

	/*
	 * 获取活动编码和 standard_conforming_strings 设置，以便
	 * 我们知道如何转义字符串。
	 */
	encoding = PQclientEncoding(conn);
	setFmtEncoding(encoding);
	std_strings = PQparameterStatus(conn, "standard_conforming_strings");
	if (!std_strings)
		std_strings = "off";

	/* 如果请求了则设置角色 */
	if (use_role)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBuffer(query, "SET ROLE %s", fmtId(use_role));
		executeCommand(conn, query->data);
		destroyPQExpBuffer(query);
	}

	/* 如果请求了则强制引用所有标识符。 */
	if (quote_all_identifiers)
		executeCommand(conn, "SET quote_all_identifiers = true");

	fprintf(OPF, "--\n-- PostgreSQL database cluster dump\n--\n\n");
	if (verbose)
		dumpTimestamp("Started on");

	/*
	 * 进入受限模式以阻止任何意外的 psql 元命令。恶意数据源可能
	 * 试图通过伪造的查询响应注入各种内容。虽然我们无法阻止此类
	 * 数据源在恢复时影响目标数据库，但我们可以阻止 psql 元命令，
	 * 以便运行 psql 并处理导出输出的客户端机器
	 * 不受影响。
	 *
	 */
	fprintf(OPF, "\\restrict %s\n\n", restrict_key);

	/*
	 * 我们过去在这里输出 \connect postgres，但这除了破坏
	 * 没有 postgres 数据库的安装之外没有任何作用。
	 * 我们在这里恢复的所有内容都是全局的，因此当前连接到
	 * 哪个数据库都可以。
	 */

	/* 恢复将需要写入目标集簇 */
	fprintf(OPF, "SET default_transaction_read_only = off;\n\n");

	/* 在输出中复制编码和 std_strings */
	fprintf(OPF, "SET client_encoding = '%s';\n",
			pg_encoding_to_char(encoding));
	fprintf(OPF, "SET standard_conforming_strings = %s;\n", std_strings);
	if (strcmp(std_strings, "off") == 0)
		fprintf(OPF, "SET escape_string_warning = off;\n");
	fprintf(OPF, "\n");

	if (!data_only)
	{
		/*
			 * 如果指定了 --clean，则先执行清理。我们可以避免详细的
			 * 依赖分析，因为数据库之间从不相互依赖，
			 * 表空间之间也从不相互依赖。角色之间可能存在
			 * 授权关系，但 DROP ROLE 会静默清理这些。
			 */
		if (output_clean)
		{
			if (!globals_only && !roles_only && !tablespaces_only)
				dropDBs(conn);

			if (!roles_only && !no_tablespaces)
				dropTablespaces(conn);

			if (!tablespaces_only)
				dropRoles(conn);
		}

		/*
			 * 现在按要求创建对象。注意此处的选项逻辑
			 * 必须与上面的删除逻辑一致。
			 */
		if (!tablespaces_only)
		{
			/* 导出角色（用户） */
			dumpRoles(conn);

			/* 导出角色成员关系 */
			dumpRoleMembership(conn);

			/* 导出角色 GUC 权限 */
			if (server_version >= 150000 && !skip_acls)
				dumpRoleGUCPrivs(conn);
		}

		/* 导出表空间 */
		if (!roles_only && !no_tablespaces)
			dumpTablespaces(conn);
	}

	/*
	 * 在导出数据库之前退出受限模式。pg_dump 将
	 * 根据需要再次进入受限模式。
	 */
	fprintf(OPF, "\\unrestrict %s\n\n", restrict_key);

	if (!globals_only && !roles_only && !tablespaces_only)
		dumpDatabases(conn);

	PQfinish(conn);

	if (verbose)
		dumpTimestamp("Completed on");
	fprintf(OPF, "--\n-- PostgreSQL database cluster dump complete\n--\n\n");

	if (filename)
	{
		fclose(OPF);

		/* 同步输出文件，错误不致命 */
		if (dosync)
			(void) fsync_fname(filename, false);
	}

	exit_nicely(0);
}


static void
help(void)
{
	printf(_("%s extracts a PostgreSQL database cluster into an SQL script file.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -f, --file=FILENAME          output file name\n"));
	printf(_("  -v, --verbose                verbose mode\n"));
	printf(_("  -V, --version                output version information, then exit\n"));
	printf(_("  --lock-wait-timeout=TIMEOUT  fail after waiting TIMEOUT for a table lock\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));
	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only              dump only the data, not the schema\n"));
	printf(_("  -c, --clean                  clean (drop) databases before recreating\n"));
	printf(_("  -E, --encoding=ENCODING      dump the data in encoding ENCODING\n"));
	printf(_("  -g, --globals-only           dump only global objects, no databases\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership\n"));
	printf(_("  -r, --roles-only             dump only roles, no databases or tablespaces\n"));
	printf(_("  -s, --schema-only            dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use in the dump\n"));
	printf(_("  -t, --tablespaces-only       dump only tablespaces, no databases or roles\n"));
	printf(_("  -x, --no-privileges          do not dump privileges (grant/revoke)\n"));
	printf(_("  --binary-upgrade             for use by upgrade utilities only\n"));
	printf(_("  --column-inserts             dump data as INSERT commands with column names\n"));
	printf(_("  --disable-dollar-quoting     disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --exclude-database=PATTERN   exclude databases whose name matches PATTERN\n"));
	printf(_("  --extra-float-digits=NUM     override default setting for extra_float_digits\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --inserts                    dump data as INSERT commands, rather than COPY\n"));
	printf(_("  --load-via-partition-root    load partitions via the root table\n"));
	printf(_("  --no-comments                do not dump comments\n"));
	printf(_("  --no-publications            do not dump publications\n"));
	printf(_("  --no-role-passwords          do not dump passwords for roles\n"));
	printf(_("  --no-security-labels         do not dump security label assignments\n"));
	printf(_("  --no-subscriptions           do not dump subscriptions\n"));
	printf(_("  --no-sync                    do not wait for changes to be written safely to disk\n"));
	printf(_("  --no-table-access-method     do not dump table access methods\n"));
	printf(_("  --no-tablespaces             do not dump tablespace assignments\n"));
	printf(_("  --no-toast-compression       do not dump TOAST compression methods\n"));
	printf(_("  --no-unlogged-table-data     do not dump unlogged table data\n"));
	printf(_("  --on-conflict-do-nothing     add ON CONFLICT DO NOTHING to INSERT commands\n"));
	printf(_("  --quote-all-identifiers      quote all identifiers, even if not key words\n"));
	printf(_("  --restrict-key=RESTRICT_KEY  use provided string as psql \\restrict key\n"));
	printf(_("  --rows-per-insert=NROWS      number of rows per INSERT; implies --inserts\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                               use SET SESSION AUTHORIZATION commands instead of\n"
			 "                               ALTER OWNER commands to set ownership\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=CONNSTR     connect using connection string\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -l, --database=DBNAME    alternative default database\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  --role=ROLENAME          do SET ROLE before dump\n"));

	printf(_("\nIf -f/--file is not used, then the SQL script will be written to the standard\n"
			 "output.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}


/*
 * 删除角色
 */
static void
dropRoles(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i_rolname;
	int			i;

	if (server_version >= 90600)
		printfPQExpBuffer(buf,
						  "SELECT rolname "
						  "FROM %s "
						  "WHERE rolname !~ '^pg_' "
						  "ORDER BY 1", role_catalog);
	else
		printfPQExpBuffer(buf,
						  "SELECT rolname "
						  "FROM %s "
						  "ORDER BY 1", role_catalog);

	res = executeQuery(conn, buf->data);

	i_rolname = PQfnumber(res, "rolname");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Drop roles\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *rolename;

		rolename = PQgetvalue(res, i, i_rolname);

		fprintf(OPF, "DROP ROLE %s%s;\n",
				if_exists ? "IF EXISTS " : "",
				fmtId(rolename));
	}

	PQclear(res);
	destroyPQExpBuffer(buf);

	fprintf(OPF, "\n\n");
}

/*
 * 导出角色
 */
static void
dumpRoles(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;
	int			i_oid,
				i_rolname,
				i_rolsuper,
				i_rolinherit,
				i_rolcreaterole,
				i_rolcreatedb,
				i_rolcanlogin,
				i_rolconnlimit,
				i_rolpassword,
				i_rolvaliduntil,
				i_rolreplication,
				i_rolbypassrls,
				i_rolcomment,
				i_is_current_user;
	int			i;

	/*
		 * 注意：rolconfig 稍后导出，且无论 role_catalog 如何，
		 * 都必须使用 pg_authid 提取 rolcomment。
		 */
	if (server_version >= 90600)
		printfPQExpBuffer(buf,
						  "SELECT oid, rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, rolreplication, rolbypassrls, "
						  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment, "
						  "rolname = current_user AS is_current_user "
						  "FROM %s "
						  "WHERE rolname !~ '^pg_' "
						  "ORDER BY 2", role_catalog);
	else if (server_version >= 90500)
		printfPQExpBuffer(buf,
						  "SELECT oid, rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, rolreplication, rolbypassrls, "
						  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment, "
						  "rolname = current_user AS is_current_user "
						  "FROM %s "
						  "ORDER BY 2", role_catalog);
	else
		printfPQExpBuffer(buf,
						  "SELECT oid, rolname, rolsuper, rolinherit, "
						  "rolcreaterole, rolcreatedb, "
						  "rolcanlogin, rolconnlimit, rolpassword, "
						  "rolvaliduntil, rolreplication, "
						  "false as rolbypassrls, "
						  "pg_catalog.shobj_description(oid, 'pg_authid') as rolcomment, "
						  "rolname = current_user AS is_current_user "
						  "FROM %s "
						  "ORDER BY 2", role_catalog);

	res = executeQuery(conn, buf->data);

	i_oid = PQfnumber(res, "oid");
	i_rolname = PQfnumber(res, "rolname");
	i_rolsuper = PQfnumber(res, "rolsuper");
	i_rolinherit = PQfnumber(res, "rolinherit");
	i_rolcreaterole = PQfnumber(res, "rolcreaterole");
	i_rolcreatedb = PQfnumber(res, "rolcreatedb");
	i_rolcanlogin = PQfnumber(res, "rolcanlogin");
	i_rolconnlimit = PQfnumber(res, "rolconnlimit");
	i_rolpassword = PQfnumber(res, "rolpassword");
	i_rolvaliduntil = PQfnumber(res, "rolvaliduntil");
	i_rolreplication = PQfnumber(res, "rolreplication");
	i_rolbypassrls = PQfnumber(res, "rolbypassrls");
	i_rolcomment = PQfnumber(res, "rolcomment");
	i_is_current_user = PQfnumber(res, "is_current_user");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Roles\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		const char *rolename;
		Oid			auth_oid;

		auth_oid = atooid(PQgetvalue(res, i, i_oid));
		rolename = PQgetvalue(res, i, i_rolname);

		if (strncmp(rolename, "pg_", 3) == 0)
		{
			pg_log_warning("role name starting with \"pg_\" skipped (%s)",
						   rolename);
			continue;
		}

		resetPQExpBuffer(buf);

		if (binary_upgrade)
		{
			appendPQExpBufferStr(buf, "\n-- For binary upgrade, must preserve pg_authid.oid\n");
			appendPQExpBuffer(buf,
							  "SELECT pg_catalog.binary_upgrade_set_next_pg_authid_oid('%u'::pg_catalog.oid);\n\n",
							  auth_oid);
		}

		/*
			 * 我们导出 CREATE ROLE 后跟 ALTER ROLE，以确保即使角色
			 * 已存在也能获得正确的属性（即 CREATE 失败也无妨）。
			 * 这对我们连接所用的角色尤为重要，因为即使使用 --clean，
			 * 我们也无法删除它。binary_upgrade 不能产生任何错误，
			 * 因此我们假设当前角色已经创建。
			 *
			 */
		if (!binary_upgrade ||
			strcmp(PQgetvalue(res, i, i_is_current_user), "f") == 0)
			appendPQExpBuffer(buf, "CREATE ROLE %s;\n", fmtId(rolename));
		appendPQExpBuffer(buf, "ALTER ROLE %s WITH", fmtId(rolename));

		if (strcmp(PQgetvalue(res, i, i_rolsuper), "t") == 0)
			appendPQExpBufferStr(buf, " SUPERUSER");
		else
			appendPQExpBufferStr(buf, " NOSUPERUSER");

		if (strcmp(PQgetvalue(res, i, i_rolinherit), "t") == 0)
			appendPQExpBufferStr(buf, " INHERIT");
		else
			appendPQExpBufferStr(buf, " NOINHERIT");

		if (strcmp(PQgetvalue(res, i, i_rolcreaterole), "t") == 0)
			appendPQExpBufferStr(buf, " CREATEROLE");
		else
			appendPQExpBufferStr(buf, " NOCREATEROLE");

		if (strcmp(PQgetvalue(res, i, i_rolcreatedb), "t") == 0)
			appendPQExpBufferStr(buf, " CREATEDB");
		else
			appendPQExpBufferStr(buf, " NOCREATEDB");

		if (strcmp(PQgetvalue(res, i, i_rolcanlogin), "t") == 0)
			appendPQExpBufferStr(buf, " LOGIN");
		else
			appendPQExpBufferStr(buf, " NOLOGIN");

		if (strcmp(PQgetvalue(res, i, i_rolreplication), "t") == 0)
			appendPQExpBufferStr(buf, " REPLICATION");
		else
			appendPQExpBufferStr(buf, " NOREPLICATION");

		if (strcmp(PQgetvalue(res, i, i_rolbypassrls), "t") == 0)
			appendPQExpBufferStr(buf, " BYPASSRLS");
		else
			appendPQExpBufferStr(buf, " NOBYPASSRLS");

		if (strcmp(PQgetvalue(res, i, i_rolconnlimit), "-1") != 0)
			appendPQExpBuffer(buf, " CONNECTION LIMIT %s",
							  PQgetvalue(res, i, i_rolconnlimit));


		if (!PQgetisnull(res, i, i_rolpassword) && !no_role_passwords)
		{
			appendPQExpBufferStr(buf, " PASSWORD ");
			appendStringLiteralConn(buf, PQgetvalue(res, i, i_rolpassword), conn);
		}

		if (!PQgetisnull(res, i, i_rolvaliduntil))
			appendPQExpBuffer(buf, " VALID UNTIL '%s'",
							  PQgetvalue(res, i, i_rolvaliduntil));

		appendPQExpBufferStr(buf, ";\n");

		if (!no_comments && !PQgetisnull(res, i, i_rolcomment))
		{
			appendPQExpBuffer(buf, "COMMENT ON ROLE %s IS ", fmtId(rolename));
			appendStringLiteralConn(buf, PQgetvalue(res, i, i_rolcomment), conn);
			appendPQExpBufferStr(buf, ";\n");
		}

		if (!no_security_labels)
			buildShSecLabels(conn, "pg_authid", auth_oid,
							 "ROLE", rolename,
							 buf);

		fprintf(OPF, "%s", buf->data);
	}

	/*
		 * 在所有角色导出后，再导出角色的配置设置。
		 * 我们这样做是因为角色的配置设置可能引用
		 * 其他角色的名称。
		 */
	if (PQntuples(res) > 0)
		fprintf(OPF, "\n--\n-- User Configurations\n--\n");

	for (i = 0; i < PQntuples(res); i++)
		dumpUserConfig(conn, PQgetvalue(res, i, i_rolname));

	PQclear(res);

	fprintf(OPF, "\n\n");

	destroyPQExpBuffer(buf);
}


/*
	 * 导出角色成员关系。
	 *
	 * 注意：我们期望 dumpRoles 已经创建了所有角色，但
	 * 尚未建立成员关系。
	 */
static void
dumpRoleMembership(PGconn *conn)
{
	PQExpBuffer buf = createPQExpBuffer();
	PQExpBuffer optbuf = createPQExpBuffer();
	PGresult   *res;
	int			start = 0,
				end,
				total;
	bool		dump_grantors;
	bool		dump_grant_options;
	int			i_role;
	int			i_member;
	int			i_grantor;
	int			i_roleid;
	int			i_memberid;
	int			i_grantorid;
	int			i_admin_option;
	int			i_inherit_option;
	int			i_set_option;

	/*
		 * 旧版 PostgreSQL 在后台对授予者的追踪不够仔细，
		 * 授予者可能是任何用户，即使他们没有角色的 ADMIN OPTION，
		 * 或者是不再存在的用户。为避免导出和恢复失败，
		 * 在连接旧版服务器时不导出授予者。
		 *
		 * 此外，在旧版本中 roleid 和/或 member 可能是不再存在的
		 * 角色 OID。如果发现这种情况，打印警告并跳过该条目。
		 *
		 *
		 */
	dump_grantors = (PQserverVersion(conn) >= 160000);

	/*
		 * 旧版 PostgreSQL 也没有授予级别的选项。
		 */
	dump_grant_options = (server_version >= 160000);

	/* 生成并执行查询。 */
	printfPQExpBuffer(buf, "SELECT ur.rolname AS role, "
					  "um.rolname AS member, "
					  "ug.rolname AS grantor, "
					  "a.roleid AS roleid, "
					  "a.member AS memberid, "
					  "a.grantor AS grantorid, "
					  "a.admin_option");
	if (dump_grant_options)
		appendPQExpBufferStr(buf, ", a.inherit_option, a.set_option");
	appendPQExpBuffer(buf, " FROM pg_auth_members a "
					  "LEFT JOIN %s ur on ur.oid = a.roleid "
					  "LEFT JOIN %s um on um.oid = a.member "
					  "LEFT JOIN %s ug on ug.oid = a.grantor "
					  "WHERE NOT (ur.rolname ~ '^pg_' AND um.rolname ~ '^pg_')"
					  "ORDER BY 1,2,3", role_catalog, role_catalog, role_catalog);
	res = executeQuery(conn, buf->data);
	i_role = PQfnumber(res, "role");
	i_member = PQfnumber(res, "member");
	i_grantor = PQfnumber(res, "grantor");
	i_roleid = PQfnumber(res, "roleid");
	i_memberid = PQfnumber(res, "memberid");
	i_grantorid = PQfnumber(res, "grantorid");
	i_admin_option = PQfnumber(res, "admin_option");
	i_inherit_option = PQfnumber(res, "inherit_option");
	i_set_option = PQfnumber(res, "set_option");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Role memberships\n--\n\n");

	/*
		 * 我们不能按任意顺序导出这些 GRANT 命令，因为被指定为
		 * 授予者的角色必须已经对其授予权限的角色拥有 ADMIN OPTION，
		 * 唯一的例外是引导超级用户，它始终可以作为授予者。
		 *
		 * 我们通过逐个角色处理这些授权来解决。对于每个角色，
		 * 我们最初只允许引导超级用户作为授予者。每次我们向某个用户
		 * 授予角色的 ADMIN OPTION 后，该用户也变成允许的授予者。
		 * 我们对角色的授权进行多次遍历，每次导出那些授予者被允许
		 * 且尚未处理过的授权。最终这应该能让我们
		 * 导出所有授权。
		 *
		 *
		 */
	total = PQntuples(res);
	while (start < total)
	{
		char	   *role = PQgetvalue(res, start, i_role);
		int			i;
		bool	   *done;
		int			remaining;
		int			prev_remaining = 0;
		rolename_hash *ht;

		/* 如果遇到空的 roleid，表示已完成（空值排在末尾）。 */
		if (PQgetisnull(res, start, i_role))
		{
			/* translator: %s represents a numeric role OID */
			pg_log_warning("found orphaned pg_auth_members entry for role %s",
						   PQgetvalue(res, start, i_roleid));
			break;
		}

		/* 单个角色的所有成员关系应该是相邻的。 */
		for (end = start; end < total; ++end)
		{
			char	   *otherrole;

			otherrole = PQgetvalue(res, end, i_role);
			if (strcmp(role, otherrole) != 0)
				break;
		}

		remaining = end - start;
		done = pg_malloc0(remaining * sizeof(bool));
		ht = rolename_create(remaining, NULL);

		/*
			 * 对该角色的授权进行多次遍历，直到所有授权
			 * 都已导出。
			 */
		while (remaining > 0)
		{
			/*
				 * 每次迭代都应该有所进展，因为一个以授权为顶点、
				 * 以授予者到成员为边的概念图应该是
				 * 连通且无环的。如果无法取得进展，
				 * 说明我们或服务器出了问题。
				 */
			if (remaining == prev_remaining)
			{
				pg_log_error("could not find a legal dump ordering for memberships in role \"%s\"",
							 role);
				PQfinish(conn);
				exit_nicely(1);
			}
			prev_remaining = remaining;

			/* 对该角色的授权进行一次遍历。 */
			for (i = start; i < end; ++i)
			{
				char	   *member;
				char	   *admin_option;
				char	   *grantorid;
				char	   *grantor;
				char	   *set_option = "true";
				bool		found;

				/* 如果已处理过此授权，则不再重复。 */
				if (done[i - start])
					continue;

				/* 对存在孤立 OID 的条目报错，然后忽略。 */
				if (PQgetisnull(res, i, i_member))
				{
					/* translator: %s represents a numeric role OID */
					pg_log_warning("found orphaned pg_auth_members entry for role %s",
								   PQgetvalue(res, i, i_memberid));
					done[i - start] = true;
					--remaining;
					continue;
				}
				if (PQgetisnull(res, i, i_grantor))
				{
					/* translator: %s represents a numeric role OID */
					pg_log_warning("found orphaned pg_auth_members entry for role %s",
								   PQgetvalue(res, i, i_grantorid));
					done[i - start] = true;
					--remaining;
					continue;
				}

				member = PQgetvalue(res, i, i_member);
				grantor = PQgetvalue(res, i, i_grantor);
				grantorid = PQgetvalue(res, i, i_grantorid);
				admin_option = PQgetvalue(res, i, i_admin_option);
				if (dump_grant_options)
					set_option = PQgetvalue(res, i, i_set_option);

				/*
					 * 如果不导出授予者，或者授予者是
					 * 引导超级用户，现在就可以导出。否则，
					 * 必须是已被授予 ADMIN OPTION 的
					 * 用户。
					 */
				if (dump_grantors &&
					atooid(grantorid) != BOOTSTRAP_SUPERUSERID &&
					rolename_lookup(ht, grantor) == NULL)
					continue;

				/* 记录已完成，以免重复处理。 */
				done[i - start] = true;
				--remaining;

				/*
					 * 如果正在授予 ADMIN OPTION，记住现在可以导出
					 * 以此成员为授予者的授权。
					 */
				if (*admin_option == 't')
					rolename_insert(ht, member, &found);

				/* 生成实际的 GRANT 语句。 */
				resetPQExpBuffer(optbuf);
				fprintf(OPF, "GRANT %s", fmtId(role));
				fprintf(OPF, " TO %s", fmtId(member));
				if (*admin_option == 't')
					appendPQExpBufferStr(optbuf, "ADMIN OPTION");
				if (dump_grant_options)
				{
					char	   *inherit_option;

					if (optbuf->data[0] != '\0')
						appendPQExpBufferStr(optbuf, ", ");
					inherit_option = PQgetvalue(res, i, i_inherit_option);
					appendPQExpBuffer(optbuf, "INHERIT %s",
									  *inherit_option == 't' ?
									  "TRUE" : "FALSE");
				}
				if (*set_option != 't')
				{
					if (optbuf->data[0] != '\0')
						appendPQExpBufferStr(optbuf, ", ");
					appendPQExpBuffer(optbuf, "SET FALSE");
				}
				if (optbuf->data[0] != '\0')
					fprintf(OPF, " WITH %s", optbuf->data);
				if (dump_grantors)
					fprintf(OPF, " GRANTED BY %s", fmtId(grantor));
				fprintf(OPF, ";\n");
			}
		}

		rolename_destroy(ht);
		pg_free(done);
		start = end;
	}

	PQclear(res);
	destroyPQExpBuffer(buf);

	fprintf(OPF, "\n\n");
}


/*
	 * 导出角色配置参数权限。此代码用于 15.0
	 * 及更高版本的服务器。
	 *
	 * 注意：我们期望 dumpRoles 已经创建了所有角色，但
	 * 尚未有按角色的配置参数权限。
	 */
static void
dumpRoleGUCPrivs(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
		 * 获取所有定义了非默认 ACL 的参数。
		 */
	res = executeQuery(conn, "SELECT parname, "
					   "pg_catalog.pg_get_userbyid(" CppAsString2(BOOTSTRAP_SUPERUSERID) ") AS parowner, "
					   "paracl, "
					   "pg_catalog.acldefault('p', " CppAsString2(BOOTSTRAP_SUPERUSERID) ") AS acldefault "
					   "FROM pg_catalog.pg_parameter_acl "
					   "ORDER BY 1");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Role privileges on configuration parameters\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		char	   *parname = PQgetvalue(res, i, 0);
		char	   *parowner = PQgetvalue(res, i, 1);
		char	   *paracl = PQgetvalue(res, i, 2);
		char	   *acldefault = PQgetvalue(res, i, 3);
		char	   *fparname;

		/* buildACLCommands() 所需 */
		fparname = pg_strdup(fmtId(parname));

		if (!buildACLCommands(fparname, NULL, NULL, "PARAMETER",
							  paracl, acldefault,
							  parowner, "", server_version, buf))
		{
			pg_log_error("could not parse ACL list (%s) for parameter \"%s\"",
						 paracl, parname);
			PQfinish(conn);
			exit_nicely(1);
		}

		fprintf(OPF, "%s", buf->data);

		free(fparname);
		destroyPQExpBuffer(buf);
	}

	PQclear(res);
	fprintf(OPF, "\n\n");
}


/*
	 * 删除表空间。
	 */
static void
dropTablespaces(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
		 * 获取所有表空间，内置的除外（假设内置表空间命名为
		 * pg_xxx）
		 */
	res = executeQuery(conn, "SELECT spcname "
					   "FROM pg_catalog.pg_tablespace "
					   "WHERE spcname !~ '^pg_' "
					   "ORDER BY 1");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Drop tablespaces\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *spcname = PQgetvalue(res, i, 0);

		fprintf(OPF, "DROP TABLESPACE %s%s;\n",
				if_exists ? "IF EXISTS " : "",
				fmtId(spcname));
	}

	PQclear(res);

	fprintf(OPF, "\n\n");
}

/*
	 * 导出表空间。
	 */
static void
dumpTablespaces(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
		 * 获取所有表空间，内置的除外（假设内置表空间命名为
		 * pg_xxx）
		 */
	res = executeQuery(conn, "SELECT oid, spcname, "
					   "pg_catalog.pg_get_userbyid(spcowner) AS spcowner, "
					   "pg_catalog.pg_tablespace_location(oid), "
					   "spcacl, acldefault('t', spcowner) AS acldefault, "
					   "array_to_string(spcoptions, ', '),"
					   "pg_catalog.shobj_description(oid, 'pg_tablespace') "
					   "FROM pg_catalog.pg_tablespace "
					   "WHERE spcname !~ '^pg_' "
					   "ORDER BY 1");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Tablespaces\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		PQExpBuffer buf = createPQExpBuffer();
		Oid			spcoid = atooid(PQgetvalue(res, i, 0));
		char	   *spcname = PQgetvalue(res, i, 1);
		char	   *spcowner = PQgetvalue(res, i, 2);
		char	   *spclocation = PQgetvalue(res, i, 3);
		char	   *spcacl = PQgetvalue(res, i, 4);
		char	   *acldefault = PQgetvalue(res, i, 5);
		char	   *spcoptions = PQgetvalue(res, i, 6);
		char	   *spccomment = PQgetvalue(res, i, 7);
		char	   *fspcname;

		/* buildACLCommands() 所需 */
		fspcname = pg_strdup(fmtId(spcname));

		if (binary_upgrade)
		{
			appendPQExpBufferStr(buf, "\n-- For binary upgrade, must preserve pg_tablespace oid\n");
			appendPQExpBuffer(buf, "SELECT pg_catalog.binary_upgrade_set_next_pg_tablespace_oid('%u'::pg_catalog.oid);\n", spcoid);
		}

		appendPQExpBuffer(buf, "CREATE TABLESPACE %s", fspcname);
		appendPQExpBuffer(buf, " OWNER %s", fmtId(spcowner));

		appendPQExpBufferStr(buf, " LOCATION ");
		appendStringLiteralConn(buf, spclocation, conn);
		appendPQExpBufferStr(buf, ";\n");

		if (spcoptions && spcoptions[0] != '\0')
			appendPQExpBuffer(buf, "ALTER TABLESPACE %s SET (%s);\n",
							  fspcname, spcoptions);

		/* 表空间不能有 initprivs */

		if (!skip_acls &&
			!buildACLCommands(fspcname, NULL, NULL, "TABLESPACE",
							  spcacl, acldefault,
							  spcowner, "", server_version, buf))
		{
			pg_log_error("could not parse ACL list (%s) for tablespace \"%s\"",
						 spcacl, spcname);
			PQfinish(conn);
			exit_nicely(1);
		}

		if (!no_comments && spccomment && spccomment[0] != '\0')
		{
			appendPQExpBuffer(buf, "COMMENT ON TABLESPACE %s IS ", fspcname);
			appendStringLiteralConn(buf, spccomment, conn);
			appendPQExpBufferStr(buf, ";\n");
		}

		if (!no_security_labels)
			buildShSecLabels(conn, "pg_tablespace", spcoid,
							 "TABLESPACE", spcname,
							 buf);

		fprintf(OPF, "%s", buf->data);

		free(fspcname);
		destroyPQExpBuffer(buf);
	}

	PQclear(res);
	fprintf(OPF, "\n\n");
}


/*
	 * 导出用于删除每个数据库的命令。
	 */
static void
dropDBs(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
		 * 跳过标记为 datallowconn 为 false 的数据库，因为我们无论如何
		 * 也无法连接它们。这必须与 dumpDatabases() 保持一致。
		 */
	res = executeQuery(conn,
					   "SELECT datname "
					   "FROM pg_database d "
					   "WHERE datallowconn AND datconnlimit != -2 "
					   "ORDER BY datname");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Drop databases (except postgres and template1)\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *dbname = PQgetvalue(res, i, 0);

		/*
			 * 跳过 "postgres" 和 "template1"；dumpDatabases() 将
			 * 特殊处理它们。此外，务必跳过 "template0"，即使
			 * 出于某种原因它没有被标记为 !datallowconn。
			 */
		if (strcmp(dbname, "template1") != 0 &&
			strcmp(dbname, "template0") != 0 &&
			strcmp(dbname, "postgres") != 0)
		{
			fprintf(OPF, "DROP DATABASE %s%s;\n",
					if_exists ? "IF EXISTS " : "",
					fmtId(dbname));
		}
	}

	PQclear(res);

	fprintf(OPF, "\n\n");
}


/*
	 * 导出用户特定的配置
	 */
static void
dumpUserConfig(PGconn *conn, const char *username)
{
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;

	printfPQExpBuffer(buf, "SELECT unnest(setconfig) FROM pg_db_role_setting "
					  "WHERE setdatabase = 0 AND setrole = "
					  "(SELECT oid FROM %s WHERE rolname = ",
					  role_catalog);
	appendStringLiteralConn(buf, username, conn);
	appendPQExpBufferChar(buf, ')');

	res = executeQuery(conn, buf->data);

	if (PQntuples(res) > 0)
	{
		char	   *sanitized;

		sanitized = sanitize_line(username, true);
		fprintf(OPF, "\n--\n-- User Config \"%s\"\n--\n\n", sanitized);
		free(sanitized);
	}

	for (int i = 0; i < PQntuples(res); i++)
	{
		resetPQExpBuffer(buf);
		makeAlterConfigCommand(conn, PQgetvalue(res, i, 0),
							   "ROLE", username, NULL, NULL,
							   buf);
		fprintf(OPF, "%s", buf->data);
	}

	PQclear(res);

	destroyPQExpBuffer(buf);
}

/*
	 * 查找匹配给定模式的数据库名称列表。
	 * 另见 pg_dump.c 中的 expand_table_name_patterns()
	 */
static void
expand_dbname_patterns(PGconn *conn,
					   SimpleStringList *patterns,
					   SimpleStringList *names)
{
	PQExpBuffer query;
	PGresult   *res;

	if (patterns->head == NULL)
		return;					/* 无事可做 */

	query = createPQExpBuffer();

	/*
		 * 下面的循环运行多个 SELECT，有时可能导致名称列表中出现
		 * 重复条目，但我们不关心，因为我们只是用来
		 * 测试列表成员关系。
		 */

	for (SimpleStringListCell *cell = patterns->head; cell; cell = cell->next)
	{
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT datname FROM pg_catalog.pg_database n\n");
		processSQLNamePattern(conn, query, cell->val, false,
							  false, NULL, "datname", NULL, NULL, NULL,
							  &dotcnt);

		if (dotcnt > 0)
		{
			pg_log_error("improper qualified name (too many dotted names): %s",
						 cell->val);
			PQfinish(conn);
			exit_nicely(1);
		}

		res = executeQuery(conn, query->data);
		for (int i = 0; i < PQntuples(res); i++)
		{
			simple_string_list_append(names, PQgetvalue(res, i, 0));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
	 * 导出数据库内容。
	 */
static void
dumpDatabases(PGconn *conn)
{
	PGresult   *res;
	int			i;

	/*
		 * 跳过标记为 datallowconn 为 false 的数据库，因为我们无论如何
		 * 也无法连接它们。这必须与 dropDBs() 保持一致。
		 *
		 * 我们安排先处理 template1，然后按字母顺序处理其他数据库。
		 * 如果全部按字母顺序处理，可能会在仍连接着 "postgres" 时
		 * 尝试删除它。这使得在连接 "template1" 时运行恢复脚本
		 * 是个坏主意，但在 --clean 模式下，没有哪种固定顺序
		 * 能避免所有失败模式。
		 *
		 */
	res = executeQuery(conn,
					   "SELECT datname "
					   "FROM pg_database d "
					   "WHERE datallowconn AND datconnlimit != -2 "
					   "ORDER BY (datname <> 'template1'), datname");

	if (PQntuples(res) > 0)
		fprintf(OPF, "--\n-- Databases\n--\n\n");

	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *dbname = PQgetvalue(res, i, 0);
		char	   *sanitized;
		const char *create_opts;
		int			ret;

		/* 跳过 template0，即使它没有被标记为 !datallowconn。 */
		if (strcmp(dbname, "template0") == 0)
			continue;

		/* 跳过任何被明确排除的数据库 */
		if (simple_string_list_member(&database_exclude_names, dbname))
		{
			pg_log_info("excluding database \"%s\"", dbname);
			continue;
		}

		pg_log_info("dumping database \"%s\"", dbname);

		sanitized = sanitize_line(dbname, true);
		fprintf(OPF, "--\n-- Database \"%s\" dump\n--\n\n", sanitized);
		free(sanitized);

		/*
			 * 我们假设 "template1" 和 "postgres" 已存在于
			 * 目标安装中。dropDBs() 不会删除它们，因为担心
			 * 会删除恢复脚本最初连接的数据库。如果指定了
			 * --clean，则告诉 pg_dump 删除并重建它们；
			 * 否则只恢复它们的内容。其他数据库
			 * 应该直接创建。
			 */
		if (strcmp(dbname, "template1") == 0 || strcmp(dbname, "postgres") == 0)
		{
			if (output_clean)
				create_opts = "--clean --create";
			else
			{
				create_opts = "";
				/* pg_dump 不会输出 \connect 命令，我们必须 */
				fprintf(OPF, "\\connect %s\n\n", dbname);
			}
		}
		else
			create_opts = "--create";

		if (filename)
			fclose(OPF);

		ret = runPgDump(dbname, create_opts);
		if (ret != 0)
			pg_fatal("pg_dump failed on database \"%s\", exiting", dbname);

		if (filename)
		{
			OPF = fopen(filename, PG_BINARY_A);
			if (!OPF)
				pg_fatal("could not re-open the output file \"%s\": %m",
						 filename);
		}
	}

	PQclear(res);
}



/*
	 * 在 dbname 上运行 pg_dump，使用指定的选项。
	 */
static int
runPgDump(const char *dbname, const char *create_opts)
{
	PQExpBuffer connstrbuf = createPQExpBuffer();
	PQExpBuffer cmd = createPQExpBuffer();
	int			ret;

	appendPQExpBuffer(cmd, "\"%s\" %s %s", pg_dump_bin,
					  pgdumpopts->data, create_opts);

	/*
		 * 如果有文件名，使用未公开的 plain-append pg_dump
		 * 格式。
		 */
	if (filename)
		appendPQExpBufferStr(cmd, " -Fa ");
	else
		appendPQExpBufferStr(cmd, " -Fp ");

	/*
		 * 将数据库名称追加到已构造的连接字符串
		 * 主干。
		 */
	appendPQExpBuffer(connstrbuf, "%s dbname=", connstr);
	appendConnStrVal(connstrbuf, dbname);

	appendShellString(cmd, connstrbuf->data);

	pg_log_info("running \"%s\"", cmd->data);

	fflush(NULL);

	ret = system(cmd->data);

	destroyPQExpBuffer(cmd);
	destroyPQExpBuffer(connstrbuf);

	return ret;
}

/*
	 * buildShSecLabels
	 *
	 * 为共享对象构建 SECURITY LABEL 命令
	 *
	 * 调用者必须以两种不同格式提供对象类型和标识：
	 * catalog_name（例如 "pg_database"）和对象 OID，以及
	 * 类型名称（例如 "DATABASE"）和对象名称（未预引用）。
	 *
	 * 命令被追加到 "buffer"。
	 */
static void
buildShSecLabels(PGconn *conn, const char *catalog_name, Oid objectId,
				 const char *objtype, const char *objname,
				 PQExpBuffer buffer)
{
	PQExpBuffer sql = createPQExpBuffer();
	PGresult   *res;

	buildShSecLabelQuery(catalog_name, objectId, sql);
	res = executeQuery(conn, sql->data);
	emitShSecLabels(conn, res, buffer, objtype, objname);

	PQclear(res);
	destroyPQExpBuffer(sql);
}

/*
	 * 使用给定的参数建立数据库连接。如果需要，
	 * 会自动发出交互式密码提示。
	 *
	 * 如果 fail_on_error 为 false，失败时返回 NULL 且不打印任何消息，
	 * 但保留已提示的密码供下次尝试。
	 *
	 * 成功时，全局变量 'connstr' 被设置为包含所用选项的
	 * 连接字符串。
	 */
static PGconn *
connectDatabase(const char *dbname, const char *connection_string,
				const char *pghost, const char *pgport, const char *pguser,
				trivalue prompt_password, bool fail_on_error)
{
	PGconn	   *conn;
	bool		new_pass;
	const char *remoteversion_str;
	int			my_version;
	const char **keywords = NULL;
	const char **values = NULL;
	PQconninfoOption *conn_opts = NULL;
	static char *password = NULL;

	if (prompt_password == TRI_YES && !password)
		password = simple_prompt("Password: ", false);

	/*
		 * 开始连接。如果后端要求密码，则循环直到
		 * 获得密码。
		 */
	do
	{
		int			argcount = 6;
		PQconninfoOption *conn_opt;
		char	   *err_msg = NULL;
		int			i = 0;

		free(keywords);
		free(values);
		PQconninfoFree(conn_opts);

		/*
			 * 合并以连接字符串形式和其他选项形式给定的
			 * 连接信息输入。显式丢弃连接字符串中的任何 dbname 值，
			 * 否则 PQconnectdbParams() 会将该值
			 * 解释为连接字符串本身。
			 */
		if (connection_string)
		{
			conn_opts = PQconninfoParse(connection_string, &err_msg);
			if (conn_opts == NULL)
				pg_fatal("%s", err_msg);

			for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
			{
				if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
					strcmp(conn_opt->keyword, "dbname") != 0)
					argcount++;
			}

			keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
			values = pg_malloc0((argcount + 1) * sizeof(*values));

			for (conn_opt = conn_opts; conn_opt->keyword != NULL; conn_opt++)
			{
				if (conn_opt->val != NULL && conn_opt->val[0] != '\0' &&
					strcmp(conn_opt->keyword, "dbname") != 0)
				{
					keywords[i] = conn_opt->keyword;
					values[i] = conn_opt->val;
					i++;
				}
			}
		}
		else
		{
			keywords = pg_malloc0((argcount + 1) * sizeof(*keywords));
			values = pg_malloc0((argcount + 1) * sizeof(*values));
		}

		if (pghost)
		{
			keywords[i] = "host";
			values[i] = pghost;
			i++;
		}
		if (pgport)
		{
			keywords[i] = "port";
			values[i] = pgport;
			i++;
		}
		if (pguser)
		{
			keywords[i] = "user";
			values[i] = pguser;
			i++;
		}
		if (password)
		{
			keywords[i] = "password";
			values[i] = password;
			i++;
		}
		if (dbname)
		{
			keywords[i] = "dbname";
			values[i] = dbname;
			i++;
		}
		keywords[i] = "fallback_application_name";
		values[i] = progname;
		i++;

		new_pass = false;
		conn = PQconnectdbParams(keywords, values, true);

		if (!conn)
			pg_fatal("could not connect to database \"%s\"", dbname);

		if (PQstatus(conn) == CONNECTION_BAD &&
			PQconnectionNeedsPassword(conn) &&
			!password &&
			prompt_password != TRI_NO)
		{
			PQfinish(conn);
			password = simple_prompt("Password: ", false);
			new_pass = true;
		}
	} while (new_pass);

	/* 检查后端连接是否成功建立 */
	if (PQstatus(conn) == CONNECTION_BAD)
	{
		if (fail_on_error)
			pg_fatal("%s", PQerrorMessage(conn));
		else
		{
			PQfinish(conn);

			free(keywords);
			free(values);
			PQconninfoFree(conn_opts);

			return NULL;
		}
	}

	/*
		 * 好，连接成功。以连接字符串的形式
		 * 记住使用的选项。
		 */
	connstr = constructConnStr(keywords, values);

	free(keywords);
	free(values);
	PQconninfoFree(conn_opts);

	/* 检查版本 */
	remoteversion_str = PQparameterStatus(conn, "server_version");
	if (!remoteversion_str)
		pg_fatal("could not get server version");
	server_version = PQserverVersion(conn);
	if (server_version == 0)
		pg_fatal("could not parse server version \"%s\"",
				 remoteversion_str);

	my_version = PG_VERSION_NUM;

	/*
		 * 我们允许服务器版本回溯到 9.2，以及我们自己主版本的
		 * 任何小版本。（另见 pg_dump.c 中的版本检查。）
		 */
	if (my_version != server_version
		&& (server_version < 90200 ||
			(server_version / 100) > (my_version / 100)))
	{
		pg_log_error("aborting because of server version mismatch");
		pg_log_error_detail("server version: %s; %s version: %s",
							remoteversion_str, progname, PG_VERSION);
		exit_nicely(1);
	}

	PQclear(executeQuery(conn, ALWAYS_SECURE_SEARCH_PATH_SQL));

	return conn;
}

/* ----------
	 * 从给定的关键字/值对构造连接字符串。
	 * 它用于将连接选项传递给 pg_dump 子进程。
	 *
	 * 以下参数被排除：
	 *	dbname		- 每次 pg_dump 调用都不同
	 *	password	- 在命令行传递密码不安全
	 *	fallback_application_name - 让 pg_dump 自行设置
	 * ----------
	 */
static char *
constructConnStr(const char **keywords, const char **values)
{
	PQExpBuffer buf = createPQExpBuffer();
	char	   *connstr;
	int			i;
	bool		firstkeyword = true;

	/* 以 key='value' 格式构造新的连接字符串。 */
	for (i = 0; keywords[i] != NULL; i++)
	{
		if (strcmp(keywords[i], "dbname") == 0 ||
			strcmp(keywords[i], "password") == 0 ||
			strcmp(keywords[i], "fallback_application_name") == 0)
			continue;

		if (!firstkeyword)
			appendPQExpBufferChar(buf, ' ');
		firstkeyword = false;
		appendPQExpBuffer(buf, "%s=", keywords[i]);
		appendConnStrVal(buf, values[i]);
	}

	connstr = pg_strdup(buf->data);
	destroyPQExpBuffer(buf);
	return connstr;
}

/*
	 * 运行查询，返回结果，失败时退出程序。
	 */
static PGresult *
executeQuery(PGconn *conn, const char *query)
{
	PGresult   *res;

	pg_log_info("executing %s", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", query);
		PQfinish(conn);
		exit_nicely(1);
	}

	return res;
}

/*
	 * 同上，但用于 SQL 命令（不返回结果）。
	 */
static void
executeCommand(PGconn *conn, const char *query)
{
	PGresult   *res;

	pg_log_info("executing %s", query);

	res = PQexec(conn, query);
	if (!res ||
		PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("query failed: %s", PQerrorMessage(conn));
		pg_log_error_detail("Query was: %s", query);
		PQfinish(conn);
		exit_nicely(1);
	}

	PQclear(res);
}


/*
	 * dumpTimestamp — 输出时间戳
	 */
static void
dumpTimestamp(const char *msg)
{
	char		buf[64];
	time_t		now = time(NULL);

	if (strftime(buf, sizeof(buf), PGDUMP_STRFTIME_FMT, localtime(&now)) != 0)
		fprintf(OPF, "-- %s %s\n\n", msg, buf);
}

/*
	 * rolename_hash 哈希表的辅助函数。
	 */
static uint32
hash_string_pointer(char *s)
{
	unsigned char *ss = (unsigned char *) s;

	return hash_bytes(ss, strlen(s));
}
