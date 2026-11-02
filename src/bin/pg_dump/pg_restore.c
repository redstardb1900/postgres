/*-------------------------------------------------------------------------
 *
 * pg_restore.c
 *	pg_restore is an utility extracting postgres database definitions
 *	from a backup archive created by pg_dump using the archiver
 *	interface.
 *
 *	pg_restore will read the backup archive and
 *	dump out a script that reproduces
 *	the schema of the database in terms of
 *		  user-defined types
 *		  user-defined functions
 *		  tables
 *		  indexes
 *		  aggregates
 *		  operators
 *		  ACL - grant/revoke
 *
 * the output script is SQL that is understood by PostgreSQL
 *
 * Basic process in a restore operation is:
 *
 *	Open the Archive and read the TOC.
 *	Set flags in TOC entries, and *maybe* reorder them.
 *	Generate script to stdout
 *	Exit
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_restore.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "dumputils.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"
#include "parallel.h"
#include "pg_backup_utils.h"

static void usage(const char *progname);

int
main(int argc, char **argv)
{
	RestoreOptions *opts;
	int			c;
	int			exit_code;
	int			numWorkers = 1;
	Archive    *AH;
	char	   *inputFileSpec;
	static int	disable_triggers = 0;
	static int	enable_row_security = 0;
	static int	if_exists = 0;
	static int	no_data_for_failed_tables = 0;
	static int	outputNoTableAm = 0;
	static int	outputNoTablespaces = 0;
	static int	use_setsessauth = 0;
	static int	no_comments = 0;
	static int	no_publications = 0;
	static int	no_security_labels = 0;
	static int	no_subscriptions = 0;
	static int	strict_names = 0;

	struct option cmdopts[] = {
		{"clean", 0, NULL, 'c'},
		{"create", 0, NULL, 'C'},
		{"data-only", 0, NULL, 'a'},
		{"dbname", 1, NULL, 'd'},
		{"exit-on-error", 0, NULL, 'e'},
		{"exclude-schema", 1, NULL, 'N'},
		{"file", 1, NULL, 'f'},
		{"format", 1, NULL, 'F'},
		{"function", 1, NULL, 'P'},
		{"host", 1, NULL, 'h'},
		{"index", 1, NULL, 'I'},
		{"jobs", 1, NULL, 'j'},
		{"list", 0, NULL, 'l'},
		{"no-privileges", 0, NULL, 'x'},
		{"no-acl", 0, NULL, 'x'},
		{"no-owner", 0, NULL, 'O'},
		{"no-reconnect", 0, NULL, 'R'},
		{"port", 1, NULL, 'p'},
		{"no-password", 0, NULL, 'w'},
		{"password", 0, NULL, 'W'},
		{"schema", 1, NULL, 'n'},
		{"schema-only", 0, NULL, 's'},
		{"superuser", 1, NULL, 'S'},
		{"table", 1, NULL, 't'},
		{"trigger", 1, NULL, 'T'},
		{"use-list", 1, NULL, 'L'},
		{"username", 1, NULL, 'U'},
		{"verbose", 0, NULL, 'v'},
		{"single-transaction", 0, NULL, '1'},

		/*
		 * 以下选项没有对应的短选项字母
		 */
		{"disable-triggers", no_argument, &disable_triggers, 1},
		{"enable-row-security", no_argument, &enable_row_security, 1},
		{"if-exists", no_argument, &if_exists, 1},
		{"no-data-for-failed-tables", no_argument, &no_data_for_failed_tables, 1},
		{"no-table-access-method", no_argument, &outputNoTableAm, 1},
		{"no-tablespaces", no_argument, &outputNoTablespaces, 1},
		{"role", required_argument, NULL, 2},
		{"section", required_argument, NULL, 3},
		{"strict-names", no_argument, &strict_names, 1},
		{"use-set-session-authorization", no_argument, &use_setsessauth, 1},
		{"no-comments", no_argument, &no_comments, 1},
		{"no-publications", no_argument, &no_publications, 1},
		{"no-security-labels", no_argument, &no_security_labels, 1},
		{"no-subscriptions", no_argument, &no_subscriptions, 1},
		{"restrict-key", required_argument, NULL, 6},

		{NULL, 0, NULL, 0}
	};

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_dump"));

	init_parallel_dump_utils();

	opts = NewRestoreOptions();

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage(progname);
			exit_nicely(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_restore (PostgreSQL) " PG_VERSION);
			exit_nicely(0);
		}
	}

	while ((c = getopt_long(argc, argv, "acCd:ef:F:h:I:j:lL:n:N:Op:P:RsS:t:T:U:vwWx1",
							cmdopts, NULL)) != -1)
	{
		switch (c)
		{
			case 'a':			/* 仅导出数据 */
				opts->dataOnly = 1;
				break;
			case 'c':			/* 在创建之前清理（即删除）模式 */
				opts->dropSchema = 1;
				break;
			case 'C':
				opts->createDB = 1;
				break;
			case 'd':
				opts->cparams.dbname = pg_strdup(optarg);
				break;
			case 'e':
				opts->exit_on_error = true;
				break;
			case 'f':			/* 输出文件名称 */
				opts->filename = pg_strdup(optarg);
				break;
			case 'F':
				if (strlen(optarg) != 0)
					opts->formatName = pg_strdup(optarg);
				break;
			case 'h':
				if (strlen(optarg) != 0)
					opts->cparams.pghost = pg_strdup(optarg);
				break;

			case 'j':			/* 恢复作业数量 */
				if (!option_parse_int(optarg, "-j/--jobs", 1,
									  PG_MAX_JOBS,
									  &numWorkers))
					exit(1);
				break;

			case 'l':			/* 导出 TOC 摘要 */
				opts->tocSummary = 1;
				break;

			case 'L':			/* 输入 TOC 摘要文件名称 */
				opts->tocFile = pg_strdup(optarg);
				break;

			case 'n':			/* 仅导出此模式的数据 */
				simple_string_list_append(&opts->schemaNames, optarg);
				break;

			case 'N':			/* 不导出此模式的数据 */
				simple_string_list_append(&opts->schemaExcludeNames, optarg);
				break;

			case 'O':
				opts->noOwner = 1;
				break;

			case 'p':
				if (strlen(optarg) != 0)
					opts->cparams.pgport = pg_strdup(optarg);
				break;
			case 'R':
				/* 空操作，为向后兼容仍可接受 */
				break;
			case 'P':			/* 函数 */
				opts->selTypes = 1;
				opts->selFunction = 1;
				simple_string_list_append(&opts->functionNames, optarg);
				break;
			case 'I':			/* 索引 */
				opts->selTypes = 1;
				opts->selIndex = 1;
				simple_string_list_append(&opts->indexNames, optarg);
				break;
			case 'T':			/* 触发器 */
				opts->selTypes = 1;
				opts->selTrigger = 1;
				simple_string_list_append(&opts->triggerNames, optarg);
				break;
			case 's':			/* 仅导出模式 */
				opts->schemaOnly = 1;
				break;
			case 'S':			/* 超级用户名称 */
				if (strlen(optarg) != 0)
					opts->superuser = pg_strdup(optarg);
				break;
			case 't':			/* 仅导出指定的表 */
				opts->selTypes = 1;
				opts->selTable = 1;
				simple_string_list_append(&opts->tableNames, optarg);
				break;

			case 'U':
				opts->cparams.username = pg_strdup(optarg);
				break;

			case 'v':			/* 详细 */
				opts->verbose = 1;
				pg_logging_increase_verbosity();
				break;

			case 'w':
				opts->cparams.promptPassword = TRI_NO;
				break;

			case 'W':
				opts->cparams.promptPassword = TRI_YES;
				break;

			case 'x':			/* 跳过 ACL 导出 */
				opts->aclsSkip = 1;
				break;

			case '1':			/* 在单事务中恢复数据 */
				opts->single_txn = true;
				opts->exit_on_error = true;
				break;

			case 0:

				/*
				 * 这涵盖了没有对应短选项的长选项。
				 */
				break;

			case 2:				/* SET ROLE */
				opts->use_role = pg_strdup(optarg);
				break;

			case 3:				/* 节 */
				set_dump_section(optarg, &(opts->dumpSections));
				break;

			case 6:
				opts->restrict_key = pg_strdup(optarg);
				break;

			default:
				/* getopt_long already emitted a complaint */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit_nicely(1);
		}
	}

	/* 从命令行获取文件名 */
	if (optind < argc)
		inputFileSpec = argv[optind++];
	else
		inputFileSpec = NULL;

	/* 如果有剩余参数则报错 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/* 如果既未指定 -f 也未指定 -d 则报错（导出 TOC 时除外） */
	if (!opts->cparams.dbname && !opts->filename && !opts->tocSummary)
		pg_fatal("one of -d/--dbname and -f/--file must be specified");

	/* -d 和 -f 最多只能指定一个，否则用户会困惑 */
	if (opts->cparams.dbname)
	{
		if (opts->filename)
		{
			pg_log_error("options -d/--dbname and -f/--file cannot be used together");
			pg_log_error_hint("Try \"%s --help\" for more information.", progname);
			exit_nicely(1);
		}

		if (opts->restrict_key)
			pg_fatal("options -d/--dbname and --restrict-key cannot be used together");

		opts->useDB = 1;
	}
	else
	{
		/*
		 * 如果不提供限制键，系统将为您指定一个。
		 */
		if (!opts->restrict_key)
			opts->restrict_key = generate_restrict_key();
		if (!opts->restrict_key)
			pg_fatal("could not generate restrict key");
		if (!valid_restrict_key(opts->restrict_key))
			pg_fatal("invalid restrict key");
	}

	if (opts->dataOnly && opts->schemaOnly)
		pg_fatal("options -s/--schema-only and -a/--data-only cannot be used together");

	if (opts->dataOnly && opts->dropSchema)
		pg_fatal("options -c/--clean and -a/--data-only cannot be used together");

	/*
	 * -C 与 -1 不兼容，因为我们无法在事务块中
	 * 创建数据库。
	 */
	if (opts->createDB && opts->single_txn)
		pg_fatal("options -C/--create and -1/--single-transaction cannot be used together");

	/* 单事务模式不能与多个连接同时使用 */
	if (opts->single_txn && numWorkers > 1)
		pg_fatal("cannot specify both --single-transaction and multiple jobs");

	opts->disable_triggers = disable_triggers;
	opts->enable_row_security = enable_row_security;
	opts->noDataForFailedTables = no_data_for_failed_tables;
	opts->noTableAm = outputNoTableAm;
	opts->noTablespace = outputNoTablespaces;
	opts->use_setsessauth = use_setsessauth;
	opts->no_comments = no_comments;
	opts->no_publications = no_publications;
	opts->no_security_labels = no_security_labels;
	opts->no_subscriptions = no_subscriptions;

	if (if_exists && !opts->dropSchema)
		pg_fatal("option --if-exists requires option -c/--clean");
	opts->if_exists = if_exists;
	opts->strict_names = strict_names;

	if (opts->formatName)
	{
		switch (opts->formatName[0])
		{
			case 'c':
			case 'C':
				opts->format = archCustom;
				break;

			case 'd':
			case 'D':
				opts->format = archDirectory;
				break;

			case 't':
			case 'T':
				opts->format = archTar;
				break;

			default:
				pg_fatal("unrecognized archive format \"%s\"; please specify \"c\", \"d\", or \"t\"",
						 opts->formatName);
		}
	}

	AH = OpenArchive(inputFileSpec, opts->format);

	SetArchiveOptions(AH, NULL, opts);

	/*
	 * 我们尚未建立连接，但这并不重要。连接初始化为 NULL，
	 * 如果在它仍为 NULL 时通过 exit_nicely() 终止，
	 * 清理函数将仅是一个空操作。
	 */
	on_exit_close_archive(AH);

	/* 告知归档器输出详细程度 */
	AH->verbose = opts->verbose;

	/*
	 * 是否保持将 SQL 命令以 "pg_restore ... | psql ... " 方式提交
	 */
	AH->exit_on_error = opts->exit_on_error;

	if (opts->tocFile)
		SortTocFromFile(AH);

	AH->numWorkers = numWorkers;

	if (opts->tocSummary)
		PrintTOCSummary(AH);
	else
	{
		ProcessArchiveRestoreOptions(AH);
		RestoreArchive(AH);
	}

	/* 完成，打印被忽略的错误的摘要 */
	if (AH->n_errors)
		pg_log_warning("errors ignored on restore: %d", AH->n_errors);

	/* AH 可能在 CloseArchive 中被释放？ */
	exit_code = AH->n_errors ? 1 : 0;

	CloseArchive(AH);

	return exit_code;
}

static void
usage(const char *progname)
{
	printf(_("%s restores a PostgreSQL database from an archive created by pg_dump.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [FILE]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -d, --dbname=NAME        connect to database name\n"));
	printf(_("  -f, --file=FILENAME      output file name (- for stdout)\n"));
	printf(_("  -F, --format=c|d|t       backup file format (should be automatic)\n"));
	printf(_("  -l, --list               print summarized TOC of the archive\n"));
	printf(_("  -v, --verbose            verbose mode\n"));
	printf(_("  -V, --version            output version information, then exit\n"));
	printf(_("  -?, --help               show this help, then exit\n"));

	printf(_("\nOptions controlling the restore:\n"));
	printf(_("  -a, --data-only              restore only the data, no schema\n"));
	printf(_("  -c, --clean                  clean (drop) database objects before recreating\n"));
	printf(_("  -C, --create                 create the target database\n"));
	printf(_("  -e, --exit-on-error          exit on error, default is to continue\n"));
	printf(_("  -I, --index=NAME             restore named index\n"));
	printf(_("  -j, --jobs=NUM               use this many parallel jobs to restore\n"));
	printf(_("  -L, --use-list=FILENAME      use table of contents from this file for\n"
			 "                               selecting/ordering output\n"));
	printf(_("  -n, --schema=NAME            restore only objects in this schema\n"));
	printf(_("  -N, --exclude-schema=NAME    do not restore objects in this schema\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership\n"));
	printf(_("  -P, --function=NAME(args)    restore named function\n"));
	printf(_("  -s, --schema-only            restore only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use for disabling triggers\n"));
	printf(_("  -t, --table=NAME             restore named relation (table, view, etc.)\n"));
	printf(_("  -T, --trigger=NAME           restore named trigger\n"));
	printf(_("  -x, --no-privileges          skip restoration of access privileges (grant/revoke)\n"));
	printf(_("  -1, --single-transaction     restore as a single transaction\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --enable-row-security        enable row security\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --no-comments                do not restore comments\n"));
	printf(_("  --no-data-for-failed-tables  do not restore data of tables that could not be\n"
			 "                               created\n"));
	printf(_("  --no-publications            do not restore publications\n"));
	printf(_("  --no-security-labels         do not restore security labels\n"));
	printf(_("  --no-subscriptions           do not restore subscriptions\n"));
	printf(_("  --no-table-access-method     do not restore table access methods\n"));
	printf(_("  --no-tablespaces             do not restore tablespace assignments\n"));
	printf(_("  --restrict-key=RESTRICT_KEY  use provided string as psql \\restrict key\n"));
	printf(_("  --section=SECTION            restore named section (pre-data, data, or post-data)\n"));
	printf(_("  --strict-names               require table and/or schema include patterns to\n"
			 "                               match at least one entity each\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                               use SET SESSION AUTHORIZATION commands instead of\n"
			 "                               ALTER OWNER commands to set ownership\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  --role=ROLENAME          do SET ROLE before restore\n"));

	printf(_("\n"
			 "The options -I, -n, -N, -P, -t, -T, and --section can be combined and specified\n"
			 "multiple times to select multiple objects.\n"));
	printf(_("\nIf no input file name is supplied, then standard input is used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}
