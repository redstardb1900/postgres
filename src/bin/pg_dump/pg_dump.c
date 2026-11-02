/*-------------------------------------------------------------------------
 *
 * pg_dump.c
 *	  pg_dump is a utility for dumping out a postgres database
 *	  into a script file.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	pg_dump will read the system catalogs in a database and dump out a
 *	script that reproduces the schema in terms of SQL that is understood
 *	by PostgreSQL
 *
 *	Note that pg_dump runs in a transaction-snapshot mode transaction,
 *	so it sees a consistent snapshot of the database including system
 *	catalogs. However, it relies in part on various specialized backend
 *	functions like pg_get_indexdef(), and those things tend to look at
 *	the currently committed state.  So it is possible to get 'cache
 *	lookup failed' error if someone performs DDL changes while a dump is
 *	happening. The window for this sort of thing is from the acquisition
 *	of the transaction snapshot to getSchemaData() (when pg_dump acquires
 *	AccessShareLock on every table it intends to dump). It isn't very large,
 *	but it can happen.
 *
 *	http://archives.postgresql.org/pgsql-bugs/2010-02/msg00187.php
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/pg_dump.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "access/attnum.h"
#include "access/sysattr.h"
#include "access/transam.h"
#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_attribute_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_default_acl_d.h"
#include "catalog/pg_largeobject_d.h"
#include "catalog/pg_largeobject_metadata_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_trigger_d.h"
#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "common/relpath.h"
#include "compress_io.h"
#include "dumputils.h"
#include "fe_utils/option_utils.h"
#include "fe_utils/string_utils.h"
#include "getopt_long.h"
#include "libpq/libpq-fs.h"
#include "parallel.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"
#include "pg_dump.h"
#include "storage/block.h"

typedef struct
{
	Oid			roleoid;		/* 角色的 OID */
	const char *rolename;		/* 角色名称 */
} RoleNameItem;

typedef struct
{
	const char *descr;			/* 对象的注释 */
	Oid			classoid;		/* 对象类别（目录 OID） */
	Oid			objoid;			/* 对象 OID */
	int			objsubid;		/* 子对象（表列编号） */
} CommentItem;

typedef struct
{
	const char *provider;		/* 此安全标签的标签提供者 */
	const char *label;			/* 对象的安全标签 */
	Oid			classoid;		/* 对象类别（目录 OID） */
	Oid			objoid;			/* 对象 OID */
	int			objsubid;		/* 子对象（表列编号） */
} SecLabelItem;

typedef enum OidOptions
{
	zeroIsError = 1,
	zeroAsStar = 2,
	zeroAsNone = 4
} OidOptions;

/* 全局声明 */
static bool dosync = true;		/* 执行 fsync() 以使导出持久化到磁盘。 */

static Oid	g_last_builtin_oid; /* 最后内置 OID 的值 */

/* 指定的名称/模式至少应匹配一个实体 */
static int	strict_names = 0;

static pg_compress_algorithm compression_algorithm = PG_COMPRESSION_NONE;

/*
 * 对象包含/排除列表
 * 
 * 字符串列表记录命令行开关给出的模式，
 * 我们随后将其转换为匹配对象的 OID 列表。
 */
static SimpleStringList schema_include_patterns = {NULL, NULL};
static SimpleOidList schema_include_oids = {NULL, NULL};
static SimpleStringList schema_exclude_patterns = {NULL, NULL};
static SimpleOidList schema_exclude_oids = {NULL, NULL};

static SimpleStringList table_include_patterns = {NULL, NULL};
static SimpleStringList table_include_patterns_and_children = {NULL, NULL};
static SimpleOidList table_include_oids = {NULL, NULL};
static SimpleStringList table_exclude_patterns = {NULL, NULL};
static SimpleStringList table_exclude_patterns_and_children = {NULL, NULL};
static SimpleOidList table_exclude_oids = {NULL, NULL};
static SimpleStringList tabledata_exclude_patterns = {NULL, NULL};
static SimpleStringList tabledata_exclude_patterns_and_children = {NULL, NULL};
static SimpleOidList tabledata_exclude_oids = {NULL, NULL};

static SimpleStringList foreign_servers_include_patterns = {NULL, NULL};
static SimpleOidList foreign_servers_include_oids = {NULL, NULL};

static SimpleStringList extension_include_patterns = {NULL, NULL};
static SimpleOidList extension_include_oids = {NULL, NULL};

static const CatalogId nilCatalogId = {0, 0};

/* 覆盖标准的 extra_float_digits 设置 */
static bool have_extra_float_digits = false;
static int	extra_float_digits;

/* 排序的角色名称表 */
static RoleNameItem *rolenames = NULL;
static int	nrolenames = 0;

/* 排序的注释表 */
static CommentItem *comments = NULL;
static int	ncomments = 0;

/* 排序的安全标签表 */
static SecLabelItem *seclabels = NULL;
static int	nseclabels = 0;

/*
 * 指定 --inserts 而未指定 --rows-per-insert 时
 * 每个 INSERT 的默认行数
 */
#define DUMP_DEFAULT_ROWS_PER_INSERT 1

/*
 * 用于生成可导出对象的带引号、模式限定名称的宏。
 */
#define fmtQualifiedDumpable(obj) \
	fmtQualifiedId((obj)->dobj.namespace->dobj.name, \
				   (obj)->dobj.name)

static void help(const char *progname);
static void setup_connection(Archive *AH,
							 const char *dumpencoding, const char *dumpsnapshot,
							 char *use_role);
static ArchiveFormat parseArchiveFormat(const char *format, ArchiveMode *mode);
static void expand_schema_name_patterns(Archive *fout,
										SimpleStringList *patterns,
										SimpleOidList *oids,
										bool strict_names);
static void expand_extension_name_patterns(Archive *fout,
										   SimpleStringList *patterns,
										   SimpleOidList *oids,
										   bool strict_names);
static void expand_foreign_server_name_patterns(Archive *fout,
												SimpleStringList *patterns,
												SimpleOidList *oids);
static void expand_table_name_patterns(Archive *fout,
									   SimpleStringList *patterns,
									   SimpleOidList *oids,
									   bool strict_names,
									   bool with_child_tables);
static void prohibit_crossdb_refs(PGconn *conn, const char *dbname,
								  const char *pattern);

static NamespaceInfo *findNamespace(Oid nsoid);
static void dumpTableData(Archive *fout, const TableDataInfo *tdinfo);
static void refreshMatViewData(Archive *fout, const TableDataInfo *tdinfo);
static const char *getRoleName(const char *roleoid_str);
static void collectRoleNames(Archive *fout);
static void getAdditionalACLs(Archive *fout);
static void dumpCommentExtended(Archive *fout, const char *type,
								const char *name, const char *namespace,
								const char *owner, CatalogId catalogId,
								int subid, DumpId dumpId,
								const char *initdb_comment);
static inline void dumpComment(Archive *fout, const char *type,
							   const char *name, const char *namespace,
							   const char *owner, CatalogId catalogId,
							   int subid, DumpId dumpId);
static int	findComments(Oid classoid, Oid objoid, CommentItem **items);
static void collectComments(Archive *fout);
static void dumpSecLabel(Archive *fout, const char *type, const char *name,
						 const char *namespace, const char *owner,
						 CatalogId catalogId, int subid, DumpId dumpId);
static int	findSecLabels(Oid classoid, Oid objoid, SecLabelItem **items);
static void collectSecLabels(Archive *fout);
static void dumpDumpableObject(Archive *fout, DumpableObject *dobj);
static void dumpNamespace(Archive *fout, const NamespaceInfo *nspinfo);
static void dumpExtension(Archive *fout, const ExtensionInfo *extinfo);
static void dumpType(Archive *fout, const TypeInfo *tyinfo);
static void dumpBaseType(Archive *fout, const TypeInfo *tyinfo);
static void dumpEnumType(Archive *fout, const TypeInfo *tyinfo);
static void dumpRangeType(Archive *fout, const TypeInfo *tyinfo);
static void dumpUndefinedType(Archive *fout, const TypeInfo *tyinfo);
static void dumpDomain(Archive *fout, const TypeInfo *tyinfo);
static void dumpCompositeType(Archive *fout, const TypeInfo *tyinfo);
static void dumpCompositeTypeColComments(Archive *fout, const TypeInfo *tyinfo,
										 PGresult *res);
static void dumpShellType(Archive *fout, const ShellTypeInfo *stinfo);
static void dumpProcLang(Archive *fout, const ProcLangInfo *plang);
static void dumpFunc(Archive *fout, const FuncInfo *finfo);
static void dumpCast(Archive *fout, const CastInfo *cast);
static void dumpTransform(Archive *fout, const TransformInfo *transform);
static void dumpOpr(Archive *fout, const OprInfo *oprinfo);
static void dumpAccessMethod(Archive *fout, const AccessMethodInfo *aminfo);
static void dumpOpclass(Archive *fout, const OpclassInfo *opcinfo);
static void dumpOpfamily(Archive *fout, const OpfamilyInfo *opfinfo);
static void dumpCollation(Archive *fout, const CollInfo *collinfo);
static void dumpConversion(Archive *fout, const ConvInfo *convinfo);
static void dumpRule(Archive *fout, const RuleInfo *rinfo);
static void dumpAgg(Archive *fout, const AggInfo *agginfo);
static void dumpTrigger(Archive *fout, const TriggerInfo *tginfo);
static void dumpEventTrigger(Archive *fout, const EventTriggerInfo *evtinfo);
static void dumpTable(Archive *fout, const TableInfo *tbinfo);
static void dumpTableSchema(Archive *fout, const TableInfo *tbinfo);
static void dumpTableAttach(Archive *fout, const TableAttachInfo *attachinfo);
static void dumpAttrDef(Archive *fout, const AttrDefInfo *adinfo);
static void dumpSequence(Archive *fout, const TableInfo *tbinfo);
static void dumpSequenceData(Archive *fout, const TableDataInfo *tdinfo);
static void dumpIndex(Archive *fout, const IndxInfo *indxinfo);
static void dumpIndexAttach(Archive *fout, const IndexAttachInfo *attachinfo);
static void dumpStatisticsExt(Archive *fout, const StatsExtInfo *statsextinfo);
static void dumpConstraint(Archive *fout, const ConstraintInfo *coninfo);
static void dumpTableConstraintComment(Archive *fout, const ConstraintInfo *coninfo);
static void dumpTSParser(Archive *fout, const TSParserInfo *prsinfo);
static void dumpTSDictionary(Archive *fout, const TSDictInfo *dictinfo);
static void dumpTSTemplate(Archive *fout, const TSTemplateInfo *tmplinfo);
static void dumpTSConfig(Archive *fout, const TSConfigInfo *cfginfo);
static void dumpForeignDataWrapper(Archive *fout, const FdwInfo *fdwinfo);
static void dumpForeignServer(Archive *fout, const ForeignServerInfo *srvinfo);
static void dumpUserMappings(Archive *fout,
							 const char *servername, const char *namespace,
							 const char *owner, CatalogId catalogId, DumpId dumpId);
static void dumpDefaultACL(Archive *fout, const DefaultACLInfo *daclinfo);

static DumpId dumpACL(Archive *fout, DumpId objDumpId, DumpId altDumpId,
					  const char *type, const char *name, const char *subname,
					  const char *nspname, const char *owner,
					  const DumpableAcl *dacl);

static void getDependencies(Archive *fout);
static void BuildArchiveDependencies(Archive *fout);
static void findDumpableDependencies(ArchiveHandle *AH, const DumpableObject *dobj,
									 DumpId **dependencies, int *nDeps, int *allocDeps);

static DumpableObject *createBoundaryObjects(void);
static void addBoundaryDependencies(DumpableObject **dobjs, int numObjs,
									DumpableObject *boundaryObjs);

static void addConstrChildIdxDeps(DumpableObject *dobj, const IndxInfo *refidx);
static void getDomainConstraints(Archive *fout, TypeInfo *tyinfo);
static void getTableData(DumpOptions *dopt, TableInfo *tblinfo, int numTables, char relkind);
static void makeTableDataInfo(DumpOptions *dopt, TableInfo *tbinfo);
static void buildMatViewRefreshDependencies(Archive *fout);
static void getTableDataFKConstraints(void);
static char *format_function_arguments(const FuncInfo *finfo, const char *funcargs,
									   bool is_agg);
static char *format_function_signature(Archive *fout,
									   const FuncInfo *finfo, bool honor_quotes);
static char *convertRegProcReference(const char *proc);
static char *getFormattedOperatorName(const char *oproid);
static char *convertTSFunction(Archive *fout, Oid funcOid);
static const char *getFormattedTypeName(Archive *fout, Oid oid, OidOptions opts);
static void getLOs(Archive *fout);
static void dumpLO(Archive *fout, const LoInfo *loinfo);
static int	dumpLOs(Archive *fout, const void *arg);
static void dumpPolicy(Archive *fout, const PolicyInfo *polinfo);
static void dumpPublication(Archive *fout, const PublicationInfo *pubinfo);
static void dumpPublicationTable(Archive *fout, const PublicationRelInfo *pubrinfo);
static void dumpSubscription(Archive *fout, const SubscriptionInfo *subinfo);
static void dumpDatabase(Archive *fout);
static void dumpDatabaseConfig(Archive *AH, PQExpBuffer outbuf,
							   const char *dbname, Oid dboid);
static void dumpEncoding(Archive *AH);
static void dumpStdStrings(Archive *AH);
static void dumpSearchPath(Archive *AH);
static void binary_upgrade_set_type_oids_by_type_oid(Archive *fout,
													 PQExpBuffer upgrade_buffer,
													 Oid pg_type_oid,
													 bool force_array_type,
													 bool include_multirange_type);
static void binary_upgrade_set_type_oids_by_rel(Archive *fout,
												PQExpBuffer upgrade_buffer,
												const TableInfo *tbinfo);
static void binary_upgrade_set_pg_class_oids(Archive *fout,
											 PQExpBuffer upgrade_buffer,
											 Oid pg_class_oid, bool is_index);
static void binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
											const DumpableObject *dobj,
											const char *objtype,
											const char *objname,
											const char *objnamespace);
static const char *getAttrName(int attrnum, const TableInfo *tblInfo);
static const char *fmtCopyColumnList(const TableInfo *ti, PQExpBuffer buffer);
static bool nonemptyReloptions(const char *reloptions);
static void appendReloptionsArrayAH(PQExpBuffer buffer, const char *reloptions,
									const char *prefix, Archive *fout);
static char *get_synchronized_snapshot(Archive *fout);
static void set_restrict_relation_kind(Archive *AH, const char *value);
static void setupDumpWorker(Archive *AH);
static TableInfo *getRootTableInfo(const TableInfo *tbinfo);
static bool forcePartitionRootLoad(const TableInfo *tbinfo);


int
main(int argc, char **argv)
{
	int			c;
	const char *filename = NULL;
	const char *format = "p";
	TableInfo  *tblinfo;
	int			numTables;
	DumpableObject **dobjs;
	int			numObjs;
	DumpableObject *boundaryObjs;
	int			i;
	int			optindex;
	RestoreOptions *ropt;
	Archive    *fout;			/* 脚本文件 */
	bool		g_verbose = false;
	const char *dumpencoding = NULL;
	const char *dumpsnapshot = NULL;
	char	   *use_role = NULL;
	int			numWorkers = 1;
	int			plainText = 0;
	ArchiveFormat archiveFormat = archUnknown;
	ArchiveMode archiveMode;
	pg_compress_specification compression_spec = {0};
	char	   *compression_detail = NULL;
	char	   *compression_algorithm_str = "none";
	char	   *error_detail = NULL;
	bool		user_compression_defined = false;

	static DumpOptions dopt;

	static struct option long_options[] = {
		{"data-only", no_argument, NULL, 'a'},
		{"blobs", no_argument, NULL, 'b'},
		{"large-objects", no_argument, NULL, 'b'},
		{"no-blobs", no_argument, NULL, 'B'},
		{"no-large-objects", no_argument, NULL, 'B'},
		{"clean", no_argument, NULL, 'c'},
		{"create", no_argument, NULL, 'C'},
		{"dbname", required_argument, NULL, 'd'},
		{"extension", required_argument, NULL, 'e'},
		{"file", required_argument, NULL, 'f'},
		{"format", required_argument, NULL, 'F'},
		{"host", required_argument, NULL, 'h'},
		{"jobs", 1, NULL, 'j'},
		{"no-reconnect", no_argument, NULL, 'R'},
		{"no-owner", no_argument, NULL, 'O'},
		{"port", required_argument, NULL, 'p'},
		{"schema", required_argument, NULL, 'n'},
		{"exclude-schema", required_argument, NULL, 'N'},
		{"schema-only", no_argument, NULL, 's'},
		{"superuser", required_argument, NULL, 'S'},
		{"table", required_argument, NULL, 't'},
		{"exclude-table", required_argument, NULL, 'T'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
		{"username", required_argument, NULL, 'U'},
		{"verbose", no_argument, NULL, 'v'},
		{"no-privileges", no_argument, NULL, 'x'},
		{"no-acl", no_argument, NULL, 'x'},
		{"compress", required_argument, NULL, 'Z'},
		{"encoding", required_argument, NULL, 'E'},
		{"help", no_argument, NULL, '?'},
		{"version", no_argument, NULL, 'V'},

		/*
		 * 以下选项没有对应的短选项字母
		 */
		{"attribute-inserts", no_argument, &dopt.column_inserts, 1},
		{"binary-upgrade", no_argument, &dopt.binary_upgrade, 1},
		{"column-inserts", no_argument, &dopt.column_inserts, 1},
		{"disable-dollar-quoting", no_argument, &dopt.disable_dollar_quoting, 1},
		{"disable-triggers", no_argument, &dopt.disable_triggers, 1},
		{"enable-row-security", no_argument, &dopt.enable_row_security, 1},
		{"exclude-table-data", required_argument, NULL, 4},
		{"extra-float-digits", required_argument, NULL, 8},
		{"if-exists", no_argument, &dopt.if_exists, 1},
		{"inserts", no_argument, NULL, 9},
		{"lock-wait-timeout", required_argument, NULL, 2},
		{"no-table-access-method", no_argument, &dopt.outputNoTableAm, 1},
		{"no-tablespaces", no_argument, &dopt.outputNoTablespaces, 1},
		{"quote-all-identifiers", no_argument, &quote_all_identifiers, 1},
		{"load-via-partition-root", no_argument, &dopt.load_via_partition_root, 1},
		{"role", required_argument, NULL, 3},
		{"section", required_argument, NULL, 5},
		{"serializable-deferrable", no_argument, &dopt.serializable_deferrable, 1},
		{"snapshot", required_argument, NULL, 6},
		{"strict-names", no_argument, &strict_names, 1},
		{"use-set-session-authorization", no_argument, &dopt.use_setsessauth, 1},
		{"no-comments", no_argument, &dopt.no_comments, 1},
		{"no-publications", no_argument, &dopt.no_publications, 1},
		{"no-security-labels", no_argument, &dopt.no_security_labels, 1},
		{"no-subscriptions", no_argument, &dopt.no_subscriptions, 1},
		{"no-toast-compression", no_argument, &dopt.no_toast_compression, 1},
		{"no-unlogged-table-data", no_argument, &dopt.no_unlogged_table_data, 1},
		{"no-sync", no_argument, NULL, 7},
		{"on-conflict-do-nothing", no_argument, &dopt.do_nothing, 1},
		{"rows-per-insert", required_argument, NULL, 10},
		{"include-foreign-data", required_argument, NULL, 11},
		{"table-and-children", required_argument, NULL, 12},
		{"exclude-table-and-children", required_argument, NULL, 13},
		{"exclude-table-data-and-children", required_argument, NULL, 14},
		{"restrict-key", required_argument, NULL, 25},

		{NULL, 0, NULL, 0}
	};

	pg_logging_init(argv[0]);
	pg_logging_set_level(PG_LOG_WARNING);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_dump"));

	/*
	 * 初始化并行执行所需的内容，特别是 Windows 上的线程支持。
	 * 
	 */
	init_parallel_dump_utils();

	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			help(progname);
			exit_nicely(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_dump (PostgreSQL) " PG_VERSION);
			exit_nicely(0);
		}
	}

	InitDumpOptions(&dopt);

	while ((c = getopt_long(argc, argv, "abBcCd:e:E:f:F:h:j:n:N:Op:RsS:t:T:U:vwWxZ:",
							long_options, &optindex)) != -1)
	{
		switch (c)
		{
			case 'a':			/* 仅导出数据 */
				dopt.dataOnly = true;
				break;

			case 'b':			/* 导出大对象 */
				dopt.outputLOs = true;
				break;

			case 'B':			/* 不导出大对象 */
				dopt.dontOutputLOs = true;
				break;

			case 'c':			/* 创建前清理（即删除）模式 */
				dopt.outputClean = 1;
				break;

			case 'C':			/* 创建数据库 */
				dopt.outputCreateDB = 1;
				break;

			case 'd':			/* 数据库名称 */
				dopt.cparams.dbname = pg_strdup(optarg);
				break;

			case 'e':			/* 包含扩展 */
				simple_string_list_append(&extension_include_patterns, optarg);
				dopt.include_everything = false;
				break;

			case 'E':			/* 导出编码 */
				dumpencoding = pg_strdup(optarg);
				break;

			case 'f':
				filename = pg_strdup(optarg);
				break;

			case 'F':
				format = pg_strdup(optarg);
				break;

			case 'h':			/* 服务器主机 */
				dopt.cparams.pghost = pg_strdup(optarg);
				break;

			case 'j':			/* 导出任务数 */
				if (!option_parse_int(optarg, "-j/--jobs", 1,
									  PG_MAX_JOBS,
									  &numWorkers))
					exit_nicely(1);
				break;

			case 'n':			/* 包含模式 */
				simple_string_list_append(&schema_include_patterns, optarg);
				dopt.include_everything = false;
				break;

			case 'N':			/* 排除模式 */
				simple_string_list_append(&schema_exclude_patterns, optarg);
				break;

			case 'O':			/* 不重新连接以匹配所有者 */
				dopt.outputNoOwner = 1;
				break;

			case 'p':			/* 服务器端口 */
				dopt.cparams.pgport = pg_strdup(optarg);
				break;

			case 'R':
				/* 无操作，为了向后兼容仍接受该选项 */
				break;

			case 's':			/* 仅导出模式 */
				dopt.schemaOnly = true;
				break;

			case 'S':			/* 纯文本输出中超级用户的用户名 */
				dopt.outputSuperuser = pg_strdup(optarg);
				break;

			case 't':			/* 包含表 */
				simple_string_list_append(&table_include_patterns, optarg);
				dopt.include_everything = false;
				break;

			case 'T':			/* 排除表 */
				simple_string_list_append(&table_exclude_patterns, optarg);
				break;

			case 'U':
				dopt.cparams.username = pg_strdup(optarg);
				break;

			case 'v':			/* 详细模式 */
				g_verbose = true;
				pg_logging_increase_verbosity();
				break;

			case 'w':
				dopt.cparams.promptPassword = TRI_NO;
				break;

			case 'W':
				dopt.cparams.promptPassword = TRI_YES;
				break;

			case 'x':			/* 跳过 ACL 导出 */
				dopt.aclsSkip = true;
				break;

			case 'Z':			/* 压缩 */
				parse_compress_options(optarg, &compression_algorithm_str,
									   &compression_detail);
				user_compression_defined = true;
				break;

			case 0:
				/* 涵盖长选项。 */
				break;

			case 2:				/* 锁等待超时 */
				dopt.lockWaitTimeout = pg_strdup(optarg);
				break;

			case 3:				/* SET ROLE */
				use_role = pg_strdup(optarg);
				break;

			case 4:				/* 排除表数据 */
				simple_string_list_append(&tabledata_exclude_patterns, optarg);
				break;

			case 5:				/* 节 */
				set_dump_section(optarg, &dopt.dumpSections);
				break;

			case 6:				/* 快照 */
				dumpsnapshot = pg_strdup(optarg);
				break;

			case 7:				/* 不同步 */
				dosync = false;
				break;

			case 8:
				have_extra_float_digits = true;
				if (!option_parse_int(optarg, "--extra-float-digits", -15, 3,
									  &extra_float_digits))
					exit_nicely(1);
				break;

			case 9:				/* INSERT 格式 */

				/*
				 * dump_inserts 也存储 --rows-per-insert，注意不要覆盖它。
				 * 
				 */
				if (dopt.dump_inserts == 0)
					dopt.dump_inserts = DUMP_DEFAULT_ROWS_PER_INSERT;
				break;

			case 10:			/* 每次插入行数 */
				if (!option_parse_int(optarg, "--rows-per-insert", 1, INT_MAX,
									  &dopt.dump_inserts))
					exit_nicely(1);
				break;

			case 11:			/* 包含外部数据 */
				simple_string_list_append(&foreign_servers_include_patterns,
										  optarg);
				break;

			case 12:			/* 包含表及其子表 */
				simple_string_list_append(&table_include_patterns_and_children,
										  optarg);
				dopt.include_everything = false;
				break;

			case 13:			/* 排除表及其子表 */
				simple_string_list_append(&table_exclude_patterns_and_children,
										  optarg);
				break;

			case 14:			/* 排除表及其子表的数据 */
				simple_string_list_append(&tabledata_exclude_patterns_and_children,
										  optarg);
				break;

			case 25:
				dopt.restrict_key = pg_strdup(optarg);
				break;

			default:
				/* getopt_long 已发出警告 */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit_nicely(1);
		}
	}

	/*
	 * 非选项参数指定数据库名称，除非已通过 -d / --dbname 指定。
	 * 
	 */
	if (optind < argc && dopt.cparams.dbname == NULL)
		dopt.cparams.dbname = argv[optind++];

	/* 如果还有剩余参数则报错 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit_nicely(1);
	}

	/* --column-inserts 暗含 --inserts */
	if (dopt.column_inserts && dopt.dump_inserts == 0)
		dopt.dump_inserts = DUMP_DEFAULT_ROWS_PER_INSERT;

	/*
	 * 二进制升级模式暗含即使在仅模式模式下也导出序列数据。
	 * 这不会作为单独选项暴露，但为了清晰内部保持独立。
	 * 
	 */
	if (dopt.binary_upgrade)
		dopt.sequence_data = 1;

	if (dopt.dataOnly && dopt.schemaOnly)
		pg_fatal("options -s/--schema-only and -a/--data-only cannot be used together");

	if (dopt.schemaOnly && foreign_servers_include_patterns.head != NULL)
		pg_fatal("options -s/--schema-only and --include-foreign-data cannot be used together");

	if (numWorkers > 1 && foreign_servers_include_patterns.head != NULL)
		pg_fatal("option --include-foreign-data is not supported with parallel backup");

	if (dopt.dataOnly && dopt.outputClean)
		pg_fatal("options -c/--clean and -a/--data-only cannot be used together");

	if (dopt.if_exists && !dopt.outputClean)
		pg_fatal("option --if-exists requires option -c/--clean");

	/*
	 * 如果已指定 --column-inserts 或 --rows-per-insert，
	 * 则 --inserts 已在上方隐式启用。
	 */
	if (dopt.do_nothing && dopt.dump_inserts == 0)
		pg_fatal("option --on-conflict-do-nothing requires option --inserts, --rows-per-insert, or --column-inserts");

	/* 识别要输出的归档格式 */
	archiveFormat = parseArchiveFormat(format, &archiveMode);

	/* 归档格式特定设置 */
	if (archiveFormat == archNull)
	{
		plainText = 1;

		/*
		 * 如果你不提供限制键，系统会为你指定一个。
		 */
		if (!dopt.restrict_key)
			dopt.restrict_key = generate_restrict_key();
		if (!dopt.restrict_key)
			pg_fatal("could not generate restrict key");
		if (!valid_restrict_key(dopt.restrict_key))
			pg_fatal("invalid restrict key");
	}
	else if (dopt.restrict_key)
		pg_fatal("option --restrict-key can only be used with --format=plain");

	/*
	 * 自定义和目录格式在可用时默认使用 gzip 压缩，
	 * 其他格式则不会。如果 gzip 不可用，
	 * 默认不进行压缩。
	 */
	if ((archiveFormat == archCustom || archiveFormat == archDirectory) &&
		!user_compression_defined)
	{
#ifdef HAVE_LIBZ
		compression_algorithm_str = "gzip";
#else
		compression_algorithm_str = "none";
#endif
	}

	/*
	 * 压缩选项
	 */
	if (!parse_compress_algorithm(compression_algorithm_str,
								  &compression_algorithm))
		pg_fatal("unrecognized compression algorithm: \"%s\"",
				 compression_algorithm_str);

	parse_compress_specification(compression_algorithm, compression_detail,
								 &compression_spec);
	error_detail = validate_compress_specification(&compression_spec);
	if (error_detail != NULL)
		pg_fatal("invalid compression specification: %s",
				 error_detail);

	error_detail = supports_compression(compression_spec);
	if (error_detail != NULL)
		pg_fatal("%s", error_detail);

	/*
	 * 暂时禁用 zstd workers 支持 —— 这些基于线程，
	 * 目前尚不清楚它与同样依赖线程的并行导出（例如 Windows 上）
	 * 如何交互。
	 */
	if (compression_spec.options & PG_COMPRESSION_OPTION_WORKERS)
		pg_log_warning("compression option \"%s\" is not currently supported by pg_dump",
					   "workers");

	/*
	 * 如果输出归档格式，我们总是要输出 DATABASE 条目，
	 * 以备在 pg_restore 时指定 --create。
	 */
	if (!plainText)
		dopt.outputCreateDB = 1;

	/* 目前仅目录归档格式支持并行备份 */
	if (archiveFormat != archDirectory && numWorkers > 1)
		pg_fatal("parallel backup only supported by the directory format");

	/* 打开输出文件 */
	fout = CreateArchive(filename, archiveFormat, compression_spec,
						 dosync, archiveMode, setupDumpWorker);

	/* 使导出选项立即可用 */
	SetArchiveOptions(fout, &dopt, NULL);

	/* 注册清理钩子 */
	on_exit_close_archive(fout);

	/* 告知归档器输出详细程度 */
	fout->verbose = g_verbose;


	/*
	 * 我们允许服务器版本回溯至 9.2，以及我们自身主版本的
	 * 任何次版本。（另见 pg_dumpall.c 中的版本检查。）
	 */
	fout->minRemoteVersion = 90200;
	fout->maxRemoteVersion = (PG_VERSION_NUM / 100) * 100 + 99;

	fout->numWorkers = numWorkers;

	/*
	 * 使用归档器打开数据库，以便它知道数据库信息。错误意味着终止。
	 *
	 */
	ConnectDatabase(fout, &dopt.cparams, false);
	setup_connection(fout, dumpencoding, dumpsnapshot, use_role);

	/*
	 * 在热备上，永远不要尝试导出未记录日志的表数据，
	 * 因为这只会抛出错误。
	 */
	if (fout->isStandby)
		dopt.no_unlogged_table_data = true;

	/*
	 * 查找最后的内置 OID（8.1 之前需要）
	 * 
	 * 8.1 及以上版本可以直接使用 FirstNormalObjectId - 1。
	 */
	g_last_builtin_oid = FirstNormalObjectId - 1;

	pg_log_info("last built-in OID is %u", g_last_builtin_oid);

	/* 将模式选择模式展开为 OID 列表 */
	if (schema_include_patterns.head != NULL)
	{
		expand_schema_name_patterns(fout, &schema_include_patterns,
									&schema_include_oids,
									strict_names);
		if (schema_include_oids.head == NULL)
			pg_fatal("no matching schemas were found");
	}
	expand_schema_name_patterns(fout, &schema_exclude_patterns,
								&schema_exclude_oids,
								false);
	/* 不匹配的排除模式不算错误 */

	/* 将表选择模式展开为 OID 列表 */
	expand_table_name_patterns(fout, &table_include_patterns,
							   &table_include_oids,
							   strict_names, false);
	expand_table_name_patterns(fout, &table_include_patterns_and_children,
							   &table_include_oids,
							   strict_names, true);
	if ((table_include_patterns.head != NULL ||
		 table_include_patterns_and_children.head != NULL) &&
		table_include_oids.head == NULL)
		pg_fatal("no matching tables were found");

	expand_table_name_patterns(fout, &table_exclude_patterns,
							   &table_exclude_oids,
							   false, false);
	expand_table_name_patterns(fout, &table_exclude_patterns_and_children,
							   &table_exclude_oids,
							   false, true);

	expand_table_name_patterns(fout, &tabledata_exclude_patterns,
							   &tabledata_exclude_oids,
							   false, false);
	expand_table_name_patterns(fout, &tabledata_exclude_patterns_and_children,
							   &tabledata_exclude_oids,
							   false, true);

	expand_foreign_server_name_patterns(fout, &foreign_servers_include_patterns,
										&foreign_servers_include_oids);

	/* 不匹配的排除模式不算错误 */

	/* 将扩展选择模式展开为 OID 列表 */
	if (extension_include_patterns.head != NULL)
	{
		expand_extension_name_patterns(fout, &extension_include_patterns,
									   &extension_include_oids,
									   strict_names);
		if (extension_include_oids.head == NULL)
			pg_fatal("no matching extensions were found");
	}

	/*
	 * 对于未使用包含开关的导出（"全部包含"导出），默认导出大对象。
	 * -B 可用于从这些导出中排除大对象。-b 可用于即使使用了
	 * 包含开关也包含大对象。
	 * 
	 * -s 表示"仅模式"，而大对象是数据而不是模式，
	 * 因此使用 -s 时绝不包含大对象。
	 *
	 */
	if (dopt.include_everything && !dopt.schemaOnly && !dopt.dontOutputLOs)
		dopt.outputLOs = true;

	/*
	 * 收集角色名称，以便将对象所有者 OID 映射到名称。
	 */
	collectRoleNames(fout);

	/*
	 * 现在扫描数据库并为所有我们打算导出的对象
	 * 创建 DumpableObject 结构。
	 */
	tblinfo = getSchemaData(fout, &numTables);

	if (!dopt.schemaOnly)
	{
		getTableData(&dopt, tblinfo, numTables, 0);
		buildMatViewRefreshDependencies(fout);
		if (dopt.dataOnly)
			getTableDataFKConstraints();
	}

	if (dopt.schemaOnly && dopt.sequence_data)
		getTableData(&dopt, tblinfo, numTables, RELKIND_SEQUENCE);

	/*
	 * 在二进制升级模式下，我们不必担心实际的大对象数据
	 * 或驻留在 pg_largeobject 和 pg_largeobject_metadata 表中
	 * 的关联元数据。
	 * 
	 * 但是，我们确实需要收集大对象信息，因为可能存在需要导出的
	 * 大对象上的注释或其他信息。
	 */
	if (dopt.outputLOs || dopt.binary_upgrade)
		getLOs(fout);

	/*
	 * 收集依赖数据以协助对象排序。
	 */
	getDependencies(fout);

	/*
	 * 如果需要，收集 ACL、注释和安全标签。
	 */
	if (!dopt.aclsSkip)
		getAdditionalACLs(fout);
	if (!dopt.no_comments)
		collectComments(fout);
	if (!dopt.no_security_labels)
		collectSecLabels(fout);

	/* 最后，创建虚拟对象来表示节边界 */
	boundaryObjs = createBoundaryObjects();

	/* 获取指向所有已知 DumpableObject 的指针 */
	getDumpableObjects(&dobjs, &numObjs);

	/*
	 * 添加虚拟依赖以强制执行导出节排序。
	 */
	addBoundaryDependencies(dobjs, numObjs, boundaryObjs);

	/*
	 * 将对象排序为安全的导出顺序（无前向引用）。
	 * 
	 * 我们依赖依赖信息来帮助确定安全顺序，因此初始排序主要用于
	 * 美观目的：我们按名称排序以确保逻辑上相同的模式将以相同方式导出。
	 *
	 */
	sortDumpableObjectsByTypeName(dobjs, numObjs);

	sortDumpableObjects(dobjs, numObjs,
						boundaryObjs[0].dumpId, boundaryObjs[1].dumpId);

	/*
	 * 为所有要导出的对象按安全顺序创建归档 TOC 条目。
	 *
	 */

	/*
	 * 首先是 ENCODING、STDSTRINGS 和 SEARCHPATH 的特殊条目。
	 */
	dumpEncoding(fout);
	dumpStdStrings(fout);
	dumpSearchPath(fout);

	/* 数据库条目总是紧随其后，除非我们完全不想要它们 */
	if (dopt.outputCreateDB)
		dumpDatabase(fout);

	/* 现在是可重新排列的对象。 */
	for (i = 0; i < numObjs; i++)
		dumpDumpableObject(fout, dobjs[i]);

	/*
	 * 设置选项信息以确保我们导出所需内容。
	 */
	ropt = NewRestoreOptions();
	ropt->filename = filename;

	/* 如果你更改此列表，请参见 dumpOptionsFromRestoreOptions */
	ropt->cparams.dbname = dopt.cparams.dbname ? pg_strdup(dopt.cparams.dbname) : NULL;
	ropt->cparams.pgport = dopt.cparams.pgport ? pg_strdup(dopt.cparams.pgport) : NULL;
	ropt->cparams.pghost = dopt.cparams.pghost ? pg_strdup(dopt.cparams.pghost) : NULL;
	ropt->cparams.username = dopt.cparams.username ? pg_strdup(dopt.cparams.username) : NULL;
	ropt->cparams.promptPassword = dopt.cparams.promptPassword;
	ropt->dropSchema = dopt.outputClean;
	ropt->dataOnly = dopt.dataOnly;
	ropt->schemaOnly = dopt.schemaOnly;
	ropt->if_exists = dopt.if_exists;
	ropt->column_inserts = dopt.column_inserts;
	ropt->dumpSections = dopt.dumpSections;
	ropt->aclsSkip = dopt.aclsSkip;
	ropt->superuser = dopt.outputSuperuser;
	ropt->createDB = dopt.outputCreateDB;
	ropt->noOwner = dopt.outputNoOwner;
	ropt->noTableAm = dopt.outputNoTableAm;
	ropt->noTablespace = dopt.outputNoTablespaces;
	ropt->disable_triggers = dopt.disable_triggers;
	ropt->use_setsessauth = dopt.use_setsessauth;
	ropt->disable_dollar_quoting = dopt.disable_dollar_quoting;
	ropt->dump_inserts = dopt.dump_inserts;
	ropt->no_comments = dopt.no_comments;
	ropt->no_publications = dopt.no_publications;
	ropt->no_security_labels = dopt.no_security_labels;
	ropt->no_subscriptions = dopt.no_subscriptions;
	ropt->lockWaitTimeout = dopt.lockWaitTimeout;
	ropt->include_everything = dopt.include_everything;
	ropt->enable_row_security = dopt.enable_row_security;
	ropt->sequence_data = dopt.sequence_data;
	ropt->binary_upgrade = dopt.binary_upgrade;
	ropt->restrict_key = dopt.restrict_key ? pg_strdup(dopt.restrict_key) : NULL;

	ropt->compression_spec = compression_spec;

	ropt->suppressDumpWarnings = true;	/* 我们已经显示过它们了 */

	SetArchiveOptions(fout, &dopt, ropt);

	/* 标记应输出哪些条目 */
	ProcessArchiveRestoreOptions(fout);

	/*
	 * 归档的 TOC 条目现在已标记哪些将实际输出，
	 * 因此我们可以正确设置它们的依赖列表。
	 * 不过，对于纯文本输出，这不是必需的。
	 */
	if (!plainText)
		BuildArchiveDependencies(fout);

	/*
	 * 最后我们可以进行实际输出了。
	 * 
	 * 注意：对于非纯文本输出格式，输出文件在 CloseArchive()
	 * 内部写入。这，嗯，有点奇怪；但现在不值得改变。
	 *
	 */
	if (plainText)
		RestoreArchive(fout);

	CloseArchive(fout);

	exit_nicely(0);
}


static void
help(const char *progname)
{
	printf(_("%s dumps a database as a text file or to other formats.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]... [DBNAME]\n"), progname);

	printf(_("\nGeneral options:\n"));
	printf(_("  -f, --file=FILENAME          output file or directory name\n"));
	printf(_("  -F, --format=c|d|t|p         output file format (custom, directory, tar,\n"
			 "                               plain text (default))\n"));
	printf(_("  -j, --jobs=NUM               use this many parallel jobs to dump\n"));
	printf(_("  -v, --verbose                verbose mode\n"));
	printf(_("  -V, --version                output version information, then exit\n"));
	printf(_("  -Z, --compress=METHOD[:DETAIL]\n"
			 "                               compress as specified\n"));
	printf(_("  --lock-wait-timeout=TIMEOUT  fail after waiting TIMEOUT for a table lock\n"));
	printf(_("  --no-sync                    do not wait for changes to be written safely to disk\n"));
	printf(_("  -?, --help                   show this help, then exit\n"));

	printf(_("\nOptions controlling the output content:\n"));
	printf(_("  -a, --data-only              dump only the data, not the schema\n"));
	printf(_("  -b, --large-objects          include large objects in dump\n"));
	printf(_("  --blobs                      (same as --large-objects, deprecated)\n"));
	printf(_("  -B, --no-large-objects       exclude large objects in dump\n"));
	printf(_("  --no-blobs                   (same as --no-large-objects, deprecated)\n"));
	printf(_("  -c, --clean                  clean (drop) database objects before recreating\n"));
	printf(_("  -C, --create                 include commands to create database in dump\n"));
	printf(_("  -e, --extension=PATTERN      dump the specified extension(s) only\n"));
	printf(_("  -E, --encoding=ENCODING      dump the data in encoding ENCODING\n"));
	printf(_("  -n, --schema=PATTERN         dump the specified schema(s) only\n"));
	printf(_("  -N, --exclude-schema=PATTERN do NOT dump the specified schema(s)\n"));
	printf(_("  -O, --no-owner               skip restoration of object ownership in\n"
			 "                               plain-text format\n"));
	printf(_("  -s, --schema-only            dump only the schema, no data\n"));
	printf(_("  -S, --superuser=NAME         superuser user name to use in plain-text format\n"));
	printf(_("  -t, --table=PATTERN          dump only the specified table(s)\n"));
	printf(_("  -T, --exclude-table=PATTERN  do NOT dump the specified table(s)\n"));
	printf(_("  -x, --no-privileges          do not dump privileges (grant/revoke)\n"));
	printf(_("  --binary-upgrade             for use by upgrade utilities only\n"));
	printf(_("  --column-inserts             dump data as INSERT commands with column names\n"));
	printf(_("  --disable-dollar-quoting     disable dollar quoting, use SQL standard quoting\n"));
	printf(_("  --disable-triggers           disable triggers during data-only restore\n"));
	printf(_("  --enable-row-security        enable row security (dump only content user has\n"
			 "                               access to)\n"));
	printf(_("  --exclude-table-and-children=PATTERN\n"
			 "                               do NOT dump the specified table(s), including\n"
			 "                               child and partition tables\n"));
	printf(_("  --exclude-table-data=PATTERN do NOT dump data for the specified table(s)\n"));
	printf(_("  --exclude-table-data-and-children=PATTERN\n"
			 "                               do NOT dump data for the specified table(s),\n"
			 "                               including child and partition tables\n"));
	printf(_("  --extra-float-digits=NUM     override default setting for extra_float_digits\n"));
	printf(_("  --if-exists                  use IF EXISTS when dropping objects\n"));
	printf(_("  --include-foreign-data=PATTERN\n"
			 "                               include data of foreign tables on foreign\n"
			 "                               servers matching PATTERN\n"));
	printf(_("  --inserts                    dump data as INSERT commands, rather than COPY\n"));
	printf(_("  --load-via-partition-root    load partitions via the root table\n"));
	printf(_("  --no-comments                do not dump comments\n"));
	printf(_("  --no-publications            do not dump publications\n"));
	printf(_("  --no-security-labels         do not dump security label assignments\n"));
	printf(_("  --no-subscriptions           do not dump subscriptions\n"));
	printf(_("  --no-table-access-method     do not dump table access methods\n"));
	printf(_("  --no-tablespaces             do not dump tablespace assignments\n"));
	printf(_("  --no-toast-compression       do not dump TOAST compression methods\n"));
	printf(_("  --no-unlogged-table-data     do not dump unlogged table data\n"));
	printf(_("  --on-conflict-do-nothing     add ON CONFLICT DO NOTHING to INSERT commands\n"));
	printf(_("  --quote-all-identifiers      quote all identifiers, even if not key words\n"));
	printf(_("  --restrict-key=RESTRICT_KEY  use provided string as psql \\restrict key\n"));
	printf(_("  --rows-per-insert=NROWS      number of rows per INSERT; implies --inserts\n"));
	printf(_("  --section=SECTION            dump named section (pre-data, data, or post-data)\n"));
	printf(_("  --serializable-deferrable    wait until the dump can run without anomalies\n"));
	printf(_("  --snapshot=SNAPSHOT          use given snapshot for the dump\n"));
	printf(_("  --strict-names               require table and/or schema include patterns to\n"
			 "                               match at least one entity each\n"));
	printf(_("  --table-and-children=PATTERN dump only the specified table(s), including\n"
			 "                               child and partition tables\n"));
	printf(_("  --use-set-session-authorization\n"
			 "                               use SET SESSION AUTHORIZATION commands instead of\n"
			 "                               ALTER OWNER commands to set ownership\n"));

	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=DBNAME      database to dump\n"));
	printf(_("  -h, --host=HOSTNAME      database server host or socket directory\n"));
	printf(_("  -p, --port=PORT          database server port number\n"));
	printf(_("  -U, --username=NAME      connect as specified database user\n"));
	printf(_("  -w, --no-password        never prompt for password\n"));
	printf(_("  -W, --password           force password prompt (should happen automatically)\n"));
	printf(_("  --role=ROLENAME          do SET ROLE before dump\n"));

	printf(_("\nIf no database name is supplied, then the PGDATABASE environment\n"
			 "variable value is used.\n\n"));
	printf(_("Report bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

static void
setup_connection(Archive *AH, const char *dumpencoding,
				 const char *dumpsnapshot, char *use_role)
{
	DumpOptions *dopt = AH->dopt;
	PGconn	   *conn = GetConnection(AH);
	const char *std_strings;

	PQclear(ExecuteSqlQueryForSingleRow(AH, ALWAYS_SECURE_SEARCH_PATH_SQL));

	/*
	 * 如果请求，则设置客户端编码。
	 */
	if (dumpencoding)
	{
		if (PQsetClientEncoding(conn, dumpencoding) < 0)
			pg_fatal("invalid client encoding \"%s\" specified",
					 dumpencoding);
	}

	/*
	 * 获取活动编码和 standard_conforming_strings 设置，
	 * 以便我们知道如何转义字符串。
	 */
	AH->encoding = PQclientEncoding(conn);
	setFmtEncoding(AH->encoding);

	std_strings = PQparameterStatus(conn, "standard_conforming_strings");
	AH->std_strings = (std_strings && strcmp(std_strings, "on") == 0);

	/*
	 * 如果请求，则设置角色。在并行导出工作进程中，
	 * 我们会收到 use_role == NULL，但 AH->use_role 已设置
	 * （如果用户最初指定了），我们应该使用它。
	 */
	if (!use_role && AH->use_role)
		use_role = AH->use_role;

	/* 如果请求，则设置角色 */
	if (use_role)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBuffer(query, "SET ROLE %s", fmtId(use_role));
		ExecuteSqlStatement(AH, query->data);
		destroyPQExpBuffer(query);

		/* 保存以备并行工作进程后续使用 */
		if (!AH->use_role)
			AH->use_role = pg_strdup(use_role);
	}

	/* 将日期样式设为 ISO 以确保导出的可移植性 */
	ExecuteSqlStatement(AH, "SET DATESTYLE = ISO");

	/* 同样，避免使用 sql_standard 间隔样式 */
	ExecuteSqlStatement(AH, "SET INTERVALSTYLE = POSTGRES");

	/*
	 * 如果提供了显式指定的 extra_float_digits，则使用它。
	 * 否则，设置 extra_float_digits 以便我们精确导出浮点数据
	 * （前提是正确实现了浮点 I/O 代码，无论如何）。
	 */
	if (have_extra_float_digits)
	{
		PQExpBuffer q = createPQExpBuffer();

		appendPQExpBuffer(q, "SET extra_float_digits TO %d",
						  extra_float_digits);
		ExecuteSqlStatement(AH, q->data);
		destroyPQExpBuffer(q);
	}
	else
		ExecuteSqlStatement(AH, "SET extra_float_digits TO 3");

	/*
	 * 禁用同步扫描，以防止转储和重新加载之间
	 * 行顺序发生不可预测的变化。
	 */
	ExecuteSqlStatement(AH, "SET synchronize_seqscans TO off");

	/*
	 * 如果支持，禁用超时。
	 */
	ExecuteSqlStatement(AH, "SET statement_timeout = 0");
	if (AH->remoteVersion >= 90300)
		ExecuteSqlStatement(AH, "SET lock_timeout = 0");
	if (AH->remoteVersion >= 90600)
		ExecuteSqlStatement(AH, "SET idle_in_transaction_session_timeout = 0");

	/*
	 * 如果请求，引用所有标识符。
	 */
	if (quote_all_identifiers)
		ExecuteSqlStatement(AH, "SET quote_all_identifiers = true");

	/*
	 * 如果支持，调整行安全性模式。
	 */
	if (AH->remoteVersion >= 90500)
	{
		if (dopt->enable_row_security)
			ExecuteSqlStatement(AH, "SET row_security = on");
		else
			ExecuteSqlStatement(AH, "SET row_security = off");
	}

	/*
	 * 出于安全原因，我们在 pg_dump 过程中限制非系统视图的展开
	 * 和对外部表的访问。导出外部表数据时会调整此限制。
	 *
	 */
	set_restrict_relation_kind(AH, "view, foreign-table");

	/*
	 * 将预编译查询状态初始化为"无预编译"。我们在此处执行此操作
	 * 以便并行导出工作进程拥有自己的状态。
	 */
	AH->is_prepared = (bool *) pg_malloc0(NUM_PREP_QUERIES * sizeof(bool));

	/*
	 * 启动事务快照模式事务以导出一致数据。
	 */
	ExecuteSqlStatement(AH, "BEGIN");

	/*
	 * 为了支持 serializable_deferrable 与 jobs 选项的组合，
	 * 我们对传入快照的工作进程连接使用 REPEATABLE READ。
	 * 只要快照在 SERIALIZABLE, READ ONLY, DEFERRABLE 事务中获取，
	 * 在 REPEATABLE READ 事务中使用它就能提供适当的完整性保证。
	 * 这是一个权宜之计，但对于向后移植是安全的。
	 *
	 */
	if (dopt->serializable_deferrable && AH->sync_snapshot_id == NULL)
		ExecuteSqlStatement(AH,
							"SET TRANSACTION ISOLATION LEVEL "
							"SERIALIZABLE, READ ONLY, DEFERRABLE");
	else
		ExecuteSqlStatement(AH,
							"SET TRANSACTION ISOLATION LEVEL "
							"REPEATABLE READ, READ ONLY");

	/*
	 * 如果用户指定了要使用的快照，则选择它。在并行导出
	 * 工作进程中，我们会收到 dumpsnapshot == NULL，
	 * 但 AH->sync_snapshot_id 已设置（如果服务器可以处理），
	 */
	if (dumpsnapshot)
		AH->sync_snapshot_id = pg_strdup(dumpsnapshot);

	if (AH->sync_snapshot_id)
	{
		PQExpBuffer query = createPQExpBuffer();

		appendPQExpBufferStr(query, "SET TRANSACTION SNAPSHOT ");
		appendStringLiteralConn(query, AH->sync_snapshot_id, conn);
		ExecuteSqlStatement(AH, query->data);
		destroyPQExpBuffer(query);
	}
	else if (AH->numWorkers > 1)
	{
		if (AH->isStandby && AH->remoteVersion < 100000)
			pg_fatal("parallel dumps from standby servers are not supported by this server version");
		AH->sync_snapshot_id = get_synchronized_snapshot(AH);
	}
}

/* 为并行工作进程设置连接 */
static void
setupDumpWorker(Archive *AH)
{
	/*
	 * 我们希望重新选择主连接使用的所有相同的值。
	 * 我们将直接继承 AH->sync_snapshot_id 和 AH->use_role 中的值，
	 * 但需要将继承的编码值转换回字符串以传递给 setup_connection。
	 * 
	 */
	setup_connection(AH,
					 pg_encoding_to_char(AH->encoding),
					 NULL,
					 NULL);
}

static char *
get_synchronized_snapshot(Archive *fout)
{
	char	   *query = "SELECT pg_catalog.pg_export_snapshot()";
	char	   *result;
	PGresult   *res;

	res = ExecuteSqlQueryForSingleRow(fout, query);
	result = pg_strdup(PQgetvalue(res, 0, 0));
	PQclear(res);

	return result;
}

static ArchiveFormat
parseArchiveFormat(const char *format, ArchiveMode *mode)
{
	ArchiveFormat archiveFormat;

	*mode = archModeWrite;

	if (pg_strcasecmp(format, "a") == 0 || pg_strcasecmp(format, "append") == 0)
	{
		/* 由 pg_dumpall 使用，未在文档中记录 */
		archiveFormat = archNull;
		*mode = archModeAppend;
	}
	else if (pg_strcasecmp(format, "c") == 0)
		archiveFormat = archCustom;
	else if (pg_strcasecmp(format, "custom") == 0)
		archiveFormat = archCustom;
	else if (pg_strcasecmp(format, "d") == 0)
		archiveFormat = archDirectory;
	else if (pg_strcasecmp(format, "directory") == 0)
		archiveFormat = archDirectory;
	else if (pg_strcasecmp(format, "p") == 0)
		archiveFormat = archNull;
	else if (pg_strcasecmp(format, "plain") == 0)
		archiveFormat = archNull;
	else if (pg_strcasecmp(format, "t") == 0)
		archiveFormat = archTar;
	else if (pg_strcasecmp(format, "tar") == 0)
		archiveFormat = archTar;
	else
		pg_fatal("invalid output format \"%s\" specified", format);
	return archiveFormat;
}

/*
 * 查找匹配给定模式列表的所有模式的 OID，
 * 并将它们追加到给定的 OID 列表中。
 */
static void
expand_schema_name_patterns(Archive *fout,
							SimpleStringList *patterns,
							SimpleOidList *oids,
							bool strict_names)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* 无事可做 */

	query = createPQExpBuffer();

	/*
	 * 下面的循环运行多个 SELECT，有时可能导致 OID 列表中出现重复条目，
	 * 但我们事后会去重。
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		PQExpBufferData dbbuf;
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_namespace n\n");
		initPQExpBuffer(&dbbuf);
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "n.nspname", NULL, NULL, &dbbuf,
							  &dotcnt);
		if (dotcnt > 1)
			pg_fatal("improper qualified name (too many dotted names): %s",
					 cell->val);
		else if (dotcnt == 1)
			prohibit_crossdb_refs(GetConnection(fout), dbbuf.data, cell->val);
		termPQExpBuffer(&dbbuf);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (strict_names && PQntuples(res) == 0)
			pg_fatal("no matching schemas were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
		{
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * 查找匹配给定模式列表的所有扩展的 OID，
 * 并将它们追加到给定的 OID 列表中。
 */
static void
expand_extension_name_patterns(Archive *fout,
							   SimpleStringList *patterns,
							   SimpleOidList *oids,
							   bool strict_names)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* 无事可做 */

	query = createPQExpBuffer();

	/*
	 * 下面的循环运行多个 SELECT，有时可能导致 OID 列表中出现重复条目，
	 * 但我们事后会去重。
	 */
	for (cell = patterns->head; cell; cell = cell->next)
	{
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_extension e\n");
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "e.extname", NULL, NULL, NULL,
							  &dotcnt);
		if (dotcnt > 0)
			pg_fatal("improper qualified name (too many dotted names): %s",
					 cell->val);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (strict_names && PQntuples(res) == 0)
			pg_fatal("no matching extensions were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
		{
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * 查找匹配给定模式列表的所有外部服务器的 OID，
 * 并将它们追加到给定的 OID 列表中。
 */
static void
expand_foreign_server_name_patterns(Archive *fout,
									SimpleStringList *patterns,
									SimpleOidList *oids)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* 无事可做 */

	query = createPQExpBuffer();

	/*
	 * 下面的循环运行多个 SELECT，有时可能导致 OID 列表中
	 * 出现重复条目，但我们不在乎。
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		int			dotcnt;

		appendPQExpBufferStr(query,
							 "SELECT oid FROM pg_catalog.pg_foreign_server s\n");
		processSQLNamePattern(GetConnection(fout), query, cell->val, false,
							  false, NULL, "s.srvname", NULL, NULL, NULL,
							  &dotcnt);
		if (dotcnt > 0)
			pg_fatal("improper qualified name (too many dotted names): %s",
					 cell->val);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		if (PQntuples(res) == 0)
			pg_fatal("no matching foreign servers were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * 查找匹配给定模式列表的所有表的 OID，
 * 并将它们追加到给定的 OID 列表中。
 * 另见 pg_dumpall.c 中的 expand_dbname_patterns()。
 */
static void
expand_table_name_patterns(Archive *fout,
						   SimpleStringList *patterns, SimpleOidList *oids,
						   bool strict_names, bool with_child_tables)
{
	PQExpBuffer query;
	PGresult   *res;
	SimpleStringListCell *cell;
	int			i;

	if (patterns->head == NULL)
		return;					/* 无事可做 */

	query = createPQExpBuffer();

	/*
	 * 有时可能导致 OID 列表中出现重复条目，
	 * 但我们不在乎。
	 */

	for (cell = patterns->head; cell; cell = cell->next)
	{
		PQExpBufferData dbbuf;
		int			dotcnt;

		/*
		 * 查询必须绝对不包含非限定名称。如果有一个接受
		 * search_path 参数的 pg_table_is_visible() 变体，
		 * 这就不必要了。
		 * 
		 * 对于 with_child_tables，我们从基本查询的结果开始，
		 * 递归搜索继承树以添加子表。
		 */
		if (with_child_tables)
		{
			appendPQExpBuffer(query, "WITH RECURSIVE partition_tree (relid) AS (\n");
		}

		appendPQExpBuffer(query,
						  "SELECT c.oid"
						  "\nFROM pg_catalog.pg_class c"
						  "\n     LEFT JOIN pg_catalog.pg_namespace n"
						  "\n     ON n.oid OPERATOR(pg_catalog.=) c.relnamespace"
						  "\nWHERE c.relkind OPERATOR(pg_catalog.=) ANY"
						  "\n    (array['%c', '%c', '%c', '%c', '%c', '%c'])\n",
						  RELKIND_RELATION, RELKIND_SEQUENCE, RELKIND_VIEW,
						  RELKIND_MATVIEW, RELKIND_FOREIGN_TABLE,
						  RELKIND_PARTITIONED_TABLE);
		initPQExpBuffer(&dbbuf);
		processSQLNamePattern(GetConnection(fout), query, cell->val, true,
							  false, "n.nspname", "c.relname", NULL,
							  "pg_catalog.pg_table_is_visible(c.oid)", &dbbuf,
							  &dotcnt);
		if (dotcnt > 2)
			pg_fatal("improper relation name (too many dotted names): %s",
					 cell->val);
		else if (dotcnt == 2)
			prohibit_crossdb_refs(GetConnection(fout), dbbuf.data, cell->val);
		termPQExpBuffer(&dbbuf);

		if (with_child_tables)
		{
			appendPQExpBuffer(query, "UNION"
							  "\nSELECT i.inhrelid"
							  "\nFROM partition_tree p"
							  "\n     JOIN pg_catalog.pg_inherits i"
							  "\n     ON p.relid OPERATOR(pg_catalog.=) i.inhparent"
							  "\n)"
							  "\nSELECT relid FROM partition_tree");
		}

		ExecuteSqlStatement(fout, "RESET search_path");
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		PQclear(ExecuteSqlQueryForSingleRow(fout,
											ALWAYS_SECURE_SEARCH_PATH_SQL));
		if (strict_names && PQntuples(res) == 0)
			pg_fatal("no matching tables were found for pattern \"%s\"", cell->val);

		for (i = 0; i < PQntuples(res); i++)
		{
			simple_oid_list_append(oids, atooid(PQgetvalue(res, i, 0)));
		}

		PQclear(res);
		resetPQExpBuffer(query);
	}

	destroyPQExpBuffer(query);
}

/*
 * 验证连接的数据库名称是否匹配给定的数据库名称，
 * 如果不匹配，则因关于给定模式的错误而终止。
 * 
 * 'dbname' 参数应是从 'pattern' 解析出的字面名称。
 */
static void
prohibit_crossdb_refs(PGconn *conn, const char *dbname, const char *pattern)
{
	const char *db;

	db = PQdb(conn);
	if (db == NULL)
		pg_fatal("You are currently not connected to a database.");

	if (strcmp(db, dbname) != 0)
		pg_fatal("cross-database references are not implemented: %s",
				 pattern);
}

/*
 * checkExtensionMembership
 *   确定对象是否为扩展成员，如果是，
 *   则记录适当的依赖并设置对象的导出标志。
 * 
 * 对每个可能是扩展成员的对象调用此函数非常重要。
 * 通常，我们将其与确定对象是否应被导出集成在一起，
 * 因为扩展成员身份会覆盖其他规则。
 * 
 * 如果对象是扩展成员则返回 true，否则返回 false。
 */
static bool
checkExtensionMembership(DumpableObject *dobj, Archive *fout)
{
	ExtensionInfo *ext = findOwningExtension(dobj->catId);

	if (ext == NULL)
		return false;

	dobj->ext_member = true;

	/* 记录依赖，以便 getDependencies 无需处理它 */
	addObjectDependency(dobj, ext->dobj.dumpId);

	/*
	 * 在 9.6 及以上版本中，标记成员对象以导出所有非初始 ACL。
	 * （所有初始 ACL 将稍后使用 pg_init_privs 的数据被删除，
	 * 以便我们只导出与扩展初始设置的差异部分。）
	 * 
	 * 在 9.6 之前，我们不包含任何扩展成员组件。
	 * 
	 * 在二进制升级中，我们仍然分别导出成员的所有组件，
	 * 因为其理念是精确复制数据库内容，
	 * 而不是用不同的内容替换扩展内容。
	 * 
	 * 注意：将来实现扩展成员 RLS 策略和/或安全标签的
	 * 存储和增量导出可能会很有趣。但 RLS 策略存在一个陷阱：
	 * 尝试导出它们需要锁定其表，而调用用户可能没有相应的权限。
	 * 我们检查表的 ACL 不需要锁定，
	 * 因此当前功能不存在此类问题。
	 *
	 *
	 *
	 */
	if (fout->dopt->binary_upgrade)
		dobj->dump = ext->dobj.dump;
	else
	{
		if (fout->remoteVersion < 90600)
			dobj->dump = DUMP_COMPONENT_NONE;
		else
			dobj->dump = ext->dobj.dump_contains & (DUMP_COMPONENT_ACL);
	}

	return true;
}

/*
 * selectDumpableNamespace：策略设置子程序
 *   标记一个模式是否应被导出
 */
static void
selectDumpableNamespace(NamespaceInfo *nsinfo, Archive *fout)
{
	/*
	 * DUMP_COMPONENT_DEFINITION 通常意味着 CREATE SCHEMA 语句
	 * 和（对于 --clean）DROP SCHEMA 语句。
	 * （在没有 DUMP_COMPONENT_DEFINITION 的情况下，此值无关紧要。）
	 */
	nsinfo->create = true;

	/*
	 * 如果正在导出特定表，则不导出任何完整的模式。
	 * 如果正在导出特定模式，则仅导出这些模式。
	 * 否则，导出所有非系统模式。
	 */
	if (table_include_oids.head != NULL)
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_NONE;
	else if (schema_include_oids.head != NULL)
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump =
			simple_oid_list_member(&schema_include_oids,
								   nsinfo->dobj.catId.oid) ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
	else if (fout->remoteVersion >= 90600 &&
			 strcmp(nsinfo->dobj.name, "pg_catalog") == 0)
	{
		/*
		 * 在 9.6 及以上版本中，我们导出 pg_catalog 中定义的所有 ACL，
		 * 只要它们是有意义的（而不是在 initdb 时设置的原始 ACL，
		 * 参见 pg_init_privs）。
		 */
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_ACL;
	}
	else if (strncmp(nsinfo->dobj.name, "pg_", 3) == 0 ||
			 strcmp(nsinfo->dobj.name, "information_schema") == 0)
	{
		/* 其他系统模式不被导出 */
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_NONE;
	}
	else if (strcmp(nsinfo->dobj.name, "public") == 0)
	{
		/*
		 * public 模式是一个特殊的实体，
		 * 介于系统对象和用户对象之间。
		 * CREATE SCHEMA 会失败，因此其 DUMP_COMPONENT_DEFINITION
		 * 只是一个注释和所有权指示。
		 * 如果所有者是默认的，则省略多余的 DUMP_COMPONENT_DEFINITION。
		 * 在 v15 之前，默认所有者是 BOOTSTRAP_SUPERUSERID。
		 */
		nsinfo->create = false;
		nsinfo->dobj.dump = DUMP_COMPONENT_ALL;
		if (nsinfo->nspowner == ROLE_PG_DATABASE_OWNER)
			nsinfo->dobj.dump &= ~DUMP_COMPONENT_DEFINITION;
		nsinfo->dobj.dump_contains = DUMP_COMPONENT_ALL;

		/*
		 * 另外，即使它没有注释，也要表现得好像它有注释；
		 * 这样我们会在适当的时候发出删除注释的命令。
		 * （没有这个，我们不会为它调用 dumpCommentExtended。）
		 */
		nsinfo->dobj.components |= DUMP_COMPONENT_COMMENT;
	}
	else
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_ALL;

	/*
	 * 在任何情况下，模式都可以被排除开关排除
	 */
	if (nsinfo->dobj.dump_contains &&
		simple_oid_list_member(&schema_exclude_oids,
							   nsinfo->dobj.catId.oid))
		nsinfo->dobj.dump_contains = nsinfo->dobj.dump = DUMP_COMPONENT_NONE;

	/*
	 * 如果模式属于扩展，允许扩展成员身份覆盖
	 * 模式本身的导出决策。然而，这不会改变 dump_contains，
	 * 因此这不会改变我们对模式内对象的处理方式。
	 * （如果它们属于扩展，将被扩展抑制，否则不会。）
	 *
	 */
	(void) checkExtensionMembership(&nsinfo->dobj, fout);
}

/*
 * selectDumpableTable：策略设置子程序
 *   标记一个表是否应被导出
 */
static void
selectDumpableTable(TableInfo *tbinfo, Archive *fout)
{
	if (checkExtensionMembership(&tbinfo->dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	/*
	 * 如果正在导出特定表，则仅导出这些表；否则，
	 * 根据父模式的导出标志导出。
	 */
	if (table_include_oids.head != NULL)
		tbinfo->dobj.dump = simple_oid_list_member(&table_include_oids,
												   tbinfo->dobj.catId.oid) ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
	else
		tbinfo->dobj.dump = tbinfo->dobj.namespace->dobj.dump_contains;

	/*
	 * 在任何情况下，表都可以被排除开关排除
	 */
	if (tbinfo->dobj.dump &&
		simple_oid_list_member(&table_exclude_oids,
							   tbinfo->dobj.catId.oid))
		tbinfo->dobj.dump = DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableType：策略设置子程序
 *   标记一个类型是否应被导出
 * 
 * 如果它是表的行类型或自动生成的数组类型，我们还应用一个
 * 特殊类型代码以方便按所需顺序排序。（我们不想将它们
 * 视为普通类型，因为那会将表提升到导出顺序的数据类型部分。）
 * 我们仍然设置对象的导出标志；这不会导致虚拟类型被导出，
 * 但我们需要它以便涉及此类类型的转换能被正确导出——参见 dumpCast。
 * 这意味着标志的设置应与底层对象（表或基础类型）的设置相同。
 *
 *
 */
static void
selectDumpableType(TypeInfo *tyinfo, Archive *fout)
{
	/* 跳过复杂类型，独立组合类型除外 */
	if (OidIsValid(tyinfo->typrelid) &&
		tyinfo->typrelkind != RELKIND_COMPOSITE_TYPE)
	{
		TableInfo  *tytable = findTableByOid(tyinfo->typrelid);

		tyinfo->dobj.objType = DO_DUMMY_TYPE;
		if (tytable != NULL)
			tyinfo->dobj.dump = tytable->dobj.dump;
		else
			tyinfo->dobj.dump = DUMP_COMPONENT_NONE;
		return;
	}

	/* 跳过自动生成的数组类型 */
	if (tyinfo->isArray || tyinfo->isMultirange)
	{
		tyinfo->dobj.objType = DO_DUMMY_TYPE;

		/*
		 * Fall through to set the dump flag; we assume that the subsequent
		 * rules will do the same thing as they would for the array's base
		 * type.  (We cannot reliably look up the base type here, since
		 * getTypes may not have processed it yet.)
		 */
	}

	if (checkExtensionMembership(&tyinfo->dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	/* 根据命名空间的内容是否被导出决定导出 */
	tyinfo->dobj.dump = tyinfo->dobj.namespace->dobj.dump_contains;
}

/*
 * selectDumpableDefaultACL：策略设置子程序
 *   标记一个默认 ACL 是否应被导出
 * 
 * 对于按模式划分的默认 ACL，我们在对象意向的上下文中使用它们。
 *
 *
 */
static void
selectDumpableDefaultACL(DefaultACLInfo *dinfo, DumpOptions *dopt)
{
	/* 默认 ACL 不能是扩展成员 */

	if (dinfo->dobj.namespace)
		/* 默认 ACL 被视为命名空间的一部分 */
		dinfo->dobj.dump = dinfo->dobj.namespace->dobj.dump_contains;
	else
		dinfo->dobj.dump = dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableCast：策略设置子程序
 *   标记一个转换是否应被导出
 * 
 * 转换不属于任何命名空间（因为它们没有名称），
 * 也没有可识别的所有者。为了区分子定义转换和内置转换，
 * 我们必须检查转换的 OID 是否在 initdb 保留的范围内。
 *
 */
static void
selectDumpableCast(CastInfo *cast, Archive *fout)
{
	if (checkExtensionMembership(&cast->dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	/*
	 * 对于来自 initdb 的转换，这将是 DUMP_COMPONENT_ACL，
	 * 但它们目前不支持 ACL。
	 */
	if (cast->dobj.catId.oid <= (Oid) g_last_builtin_oid)
		cast->dobj.dump = DUMP_COMPONENT_NONE;
	else
		cast->dobj.dump = fout->dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableProcLang：策略设置子程序
 *   标记一个过程语言是否应被导出
 * 
 * 过程语言不属于任何命名空间。
 * 为了识别内置语言，我们必须检查该语言的 OID
 * 是否在 initdb 保留的范围内。
 */
static void
selectDumpableProcLang(ProcLangInfo *plang, Archive *fout)
{
	if (checkExtensionMembership(&plang->dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	/*
	 * 仅在导出所有内容时才包含过程语言。
	 * 
	 * 对于来自 initdb 的过程语言，我们仍然仅在执行包含所有内容的导出时才包含它们。
	 * 我们还标记它们以便 dumpProcLang 可以发出 ALTER EXTENSION ADD。
	 *
	 */
	if (!fout->dopt->include_everything)
		plang->dobj.dump = DUMP_COMPONENT_NONE;
	else
	{
		if (plang->dobj.catId.oid <= (Oid) g_last_builtin_oid)
			plang->dobj.dump = fout->remoteVersion < 90600 ?
				DUMP_COMPONENT_NONE : DUMP_COMPONENT_ACL;
		else
			plang->dobj.dump = DUMP_COMPONENT_ALL;
	}
}

/*
 * selectDumpableAccessMethod：策略设置子程序
 *   标记一个访问方法是否应被导出
 * 
 * 访问方法不属于任何命名空间。
 * 为了识别内置访问方法，我们必须检查该方法的 OID
 * 是否在 initdb 保留的范围内。
 */
static void
selectDumpableAccessMethod(AccessMethodInfo *method, Archive *fout)
{
	/* 参见 getAccessMethods() 中关于 v9.6 的注释。 */
	if (fout->remoteVersion < 90600)
	{
		method->dobj.dump = DUMP_COMPONENT_NONE;
		return;
	}

	if (checkExtensionMembership(&method->dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	/*
	 * 对于来自 initdb 的访问方法，这将是 DUMP_COMPONENT_ACL，
	 * 但它们目前不支持 ACL。
	 */
	if (method->dobj.catId.oid <= (Oid) g_last_builtin_oid)
		method->dobj.dump = DUMP_COMPONENT_NONE;
	else
		method->dobj.dump = fout->dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableExtension：策略设置子程序
 *   标记一个扩展是否应被导出
 * 
 * 内置扩展始终存在，在 pg_dump 运行期间无法被删除。
 * 我们仍然希望导出它们的内容（成员对象），
 * 即使不能按名称包含/排除它们。
 * 
 * 注意：getExtension 已应用了用户指定的模式匹配。
 *
 */
static void
selectDumpableExtension(ExtensionInfo *extinfo, DumpOptions *dopt)
{
	/*
	 * 对内置扩展使用 DUMP_COMPONENT_ACL，
	 * 允许用户通过其成员对象更改权限。
	 *
	 */
	if (extinfo->dobj.catId.oid <= (Oid) g_last_builtin_oid)
		extinfo->dobj.dump = extinfo->dobj.dump_contains = DUMP_COMPONENT_ACL;
	else
	{
		/* 检查是否有要导出的扩展列表 */
		if (extension_include_oids.head != NULL)
			extinfo->dobj.dump = extinfo->dobj.dump_contains =
				simple_oid_list_member(&extension_include_oids,
									   extinfo->dobj.catId.oid) ?
				DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
		else
			extinfo->dobj.dump = extinfo->dobj.dump_contains =
				dopt->include_everything ?
				DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
	}
}

/*
 * selectDumpablePublicationObject：策略设置子程序
 *   标记一个发布对象是否应被导出
 * 
 * 发布不属于任何命名空间。
 *
 *
 */
static void
selectDumpablePublicationObject(DumpableObject *dobj, Archive *fout)
{
	if (checkExtensionMembership(dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	dobj->dump = fout->dopt->include_everything ?
		DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableStatisticsObject：策略设置子程序
 *   标记一个扩展统计对象是否应被导出
 * 
 * 扩展统计对象位于包含它们的模式中。
 *
 *
 */
static void
selectDumpableStatisticsObject(StatsExtInfo *sobj, Archive *fout)
{
	if (checkExtensionMembership(&sobj->dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	sobj->dobj.dump = sobj->dobj.namespace->dobj.dump_contains;
	if (sobj->stattable == NULL ||
		!(sobj->stattable->dobj.dump & DUMP_COMPONENT_DEFINITION))
		sobj->dobj.dump = DUMP_COMPONENT_NONE;
}

/*
 * selectDumpableObject：策略设置子程序
 *   标记一个通用可导出对象是否应被导出
 * 
 * 用于没有更具体策略函数的对象类型。
 */
static void
selectDumpableObject(DumpableObject *dobj, Archive *fout)
{
	if (checkExtensionMembership(dobj, fout))
		return;					/* 扩展成员身份覆盖所有其他规则 */

	/*
	 * 默认策略是：如果父命名空间可导出则导出，
	 * 或者对于非命名空间关联的项目，如果我们在"包含所有内容"模式下则导出。
	 */
	if (dobj->namespace)
		dobj->dump = dobj->namespace->dobj.dump_contains;
	else
		dobj->dump = fout->dopt->include_everything ?
			DUMP_COMPONENT_ALL : DUMP_COMPONENT_NONE;
}

/*
 * 使用 COPY 命令导出表内容以供加载
 * - 当归档器需要时由归档器调用此例程。
 *
 */
static int
dumpTableData_copy(Archive *fout, const void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	const char *classname = tbinfo->dobj.name;
	PQExpBuffer q = createPQExpBuffer();

	/*
	 * 注意：此处不能使用 getThreadLocalPQExpBuffer()，
	 * 我们正在调用 fmtId，它已经使用了该缓冲区。
	 */
	PQExpBuffer clistBuf = createPQExpBuffer();
	PGconn	   *conn = GetConnection(fout);
	PGresult   *res;
	int			ret;
	char	   *copybuf;
	const char *column_list;

	pg_log_info("dumping contents of table \"%s.%s\"",
				tbinfo->dobj.namespace->dobj.name, classname);

	/*
	 * 显式指定列列表，以便我们不会以错误的列顺序
	 * 检索数据，等等。这还会确保在转储和重新加载之间
	 * 添加或删除列时不会出现问题。
	 *
	 */
	column_list = fmtCopyColumnList(tbinfo, clistBuf);

	/*
	 * 在导出外部表数据时以及指定了过滤条件时
	 * 使用 COPY (SELECT ...) TO。对于其他所有情况，我们在这里使用 COPY table TO。
	 *
	 */
	if (tdinfo->filtercond || tbinfo->relkind == RELKIND_FOREIGN_TABLE)
	{
		/* 临时允许访问外部表以导出数据 */
		if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
			set_restrict_relation_kind(fout, "view");

		appendPQExpBufferStr(q, "COPY (SELECT ");
		/* 去除列列表中括号的权宜之计 */
		if (strlen(column_list) > 2)
		{
			appendPQExpBufferStr(q, column_list + 1);
			q->data[q->len - 1] = ' ';
		}
		else
			appendPQExpBufferStr(q, "* ");

		appendPQExpBuffer(q, "FROM %s %s) TO stdout;",
						  fmtQualifiedDumpable(tbinfo),
						  tdinfo->filtercond ? tdinfo->filtercond : "");
	}
	else
	{
		appendPQExpBuffer(q, "COPY %s %s TO stdout;",
						  fmtQualifiedDumpable(tbinfo),
						  column_list);
	}
	res = ExecuteSqlQuery(fout, q->data, PGRES_COPY_OUT);
	PQclear(res);
	destroyPQExpBuffer(clistBuf);

	for (;;)
	{
		ret = PQgetCopyData(conn, &copybuf, 0);

		if (ret < 0)
			break;				/* 完成或出错 */

		if (copybuf)
		{
			WriteData(fout, copybuf, ret);
			PQfreemem(copybuf);
		}

		/* ----------
		 * THROTTLE:
		 *
		 * There was considerable discussion in late July, 2000 regarding
		 * slowing down pg_dump when backing up large tables. Users with both
		 * slow & fast (multi-processor) machines experienced performance
		 * degradation when doing a backup.
		 *
		 * Initial attempts based on sleeping for a number of ms for each ms
		 * of work were deemed too complex, then a simple 'sleep in each loop'
		 * implementation was suggested. The latter failed because the loop
		 * was too tight. Finally, the following was implemented:
		 *
		 * If throttle is non-zero, then
		 *		See how long since the last sleep.
		 *		Work out how long to sleep (based on ratio).
		 *		If sleep is more than 100ms, then
		 *			sleep
		 *			reset timer
		 *		EndIf
		 * EndIf
		 *
		 * where the throttle value was the number of ms to sleep per ms of
		 * work. The calculation was done in each loop.
		 *
		 * Most of the hard work is done in the backend, and this solution
		 * still did not work particularly well: on slow machines, the ratio
		 * was 50:1, and on medium paced machines, 1:1, and on fast
		 * multi-processor machines, it had little or no effect, for reasons
		 * that were unclear.
		 *
		 * Further discussion ensued, and the proposal was dropped.
		 *
		 * For those people who want this feature, it can be implemented using
		 * gettimeofday in each loop, calculating the time since last sleep,
		 * multiplying that by the sleep ratio, then if the result is more
		 * than a preset 'minimum sleep time' (say 100ms), call the 'select'
		 * function to sleep for a subsecond period ie.
		 *
		 * select(0, NULL, NULL, NULL, &tvi);
		 *
		 * This will return after the interval specified in the structure tvi.
		 * Finally, call gettimeofday again to save the 'last sleep time'.
		 * ----------
		 */
	}
	archprintf(fout, "\\.\n\n\n");

	if (ret == -2)
	{
		/* 复制数据传输失败 */
		pg_log_error("Dumping the contents of table \"%s\" failed: PQgetCopyData() failed.", classname);
		pg_log_error_detail("Error message from server: %s", PQerrorMessage(conn));
		pg_log_error_detail("Command was: %s", q->data);
		exit_nicely(1);
	}

	/* 检查命令状态并返回到正常的 libpq 状态 */
	res = PQgetResult(conn);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("Dumping the contents of table \"%s\" failed: PQgetResult() failed.", classname);
		pg_log_error_detail("Error message from server: %s", PQerrorMessage(conn));
		pg_log_error_detail("Command was: %s", q->data);
		exit_nicely(1);
	}
	PQclear(res);

	/* 执行此操作以确保我们已将 libpq 泵送回空闲状态 */
	if (PQgetResult(conn) != NULL)
		pg_log_warning("unexpected extra results during COPY of table \"%s\"",
					   classname);

	destroyPQExpBuffer(q);

	/* 恢复设置 */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
		set_restrict_relation_kind(fout, "view, foreign-table");

	return 1;
}

/*
 * 使用 INSERT 命令导出表数据。
 * 
 * 注意：当我们从归档文件直接恢复到数据库时，INSERT 命令
 * 必须被格式化为一个字符串，然后可以传递给 PQexec。
 * 让调用者担心这一点——我们只返回一个预先构建的 INSERT 字符串。
 *
 */
static int
dumpTableData_insert(Archive *fout, const void *dcontext)
{
	TableDataInfo *tdinfo = (TableDataInfo *) dcontext;
	TableInfo  *tbinfo = tdinfo->tdtable;
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer insertStmt = NULL;
	char	   *attgenerated;
	PGresult   *res;
	int			nfields,
				i;
	int			rows_per_statement = dopt->dump_inserts;
	int			rows_this_statement = 0;

	/* 临时允许访问外部表以导出数据 */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
		set_restrict_relation_kind(fout, "view");

	/*
	 * 如果我们要发出带列名的 INSERT，处理生成列最有效的
	 * 方式是将其放在列列表中的默认位置。但如果我们没有列名，
	 * 在列列表中使用 DEFAULT 是无效的，因此我们必须检索生成列的值。
	 *
	 *
	 */
	attgenerated = (char *) pg_malloc(tbinfo->numatts * sizeof(char));
	appendPQExpBufferStr(q, "DECLARE _pg_dump_cursor CURSOR FOR SELECT ");
	nfields = 0;
	for (i = 0; i < tbinfo->numatts; i++)
	{
		if (tbinfo->attisdropped[i])
			continue;
		if (tbinfo->attgenerated[i] && dopt->column_inserts)
			continue;
		if (nfields > 0)
			appendPQExpBufferStr(q, ", ");
		if (tbinfo->attgenerated[i])
			appendPQExpBufferStr(q, "NULL");
		else
			appendPQExpBufferStr(q, fmtId(tbinfo->attnames[i]));
		attgenerated[nfields] = tbinfo->attgenerated[i];
		nfields++;
	}
	/* 9.4 之前的服务器会对零列 SELECT 报错 */
	if (nfields == 0)
		appendPQExpBufferStr(q, "NULL");
	appendPQExpBuffer(q, " FROM ONLY %s",
					  fmtQualifiedDumpable(tbinfo));
	if (tdinfo->filtercond)
		appendPQExpBuffer(q, " %s", tdinfo->filtercond);

	ExecuteSqlStatement(fout, q->data);

	while (1)
	{
		res = ExecuteSqlQuery(fout, "FETCH 100 FROM _pg_dump_cursor",
							  PGRES_TUPLES_OK);

		/* 交叉检查字段计数，如有虚拟 NULL 则予以考虑 */
		if (nfields != PQnfields(res) &&
			!(nfields == 0 && PQnfields(res) == 1))
			pg_fatal("wrong number of fields retrieved from table \"%s\"",
					 tbinfo->dobj.name);

		/*
		 * 第一次通过时，我们尽可能构建 INSERT 语句的
		 * 完整内容到 insertStmt 中，然后可以重复使用。
		 * 如果表中有生成列，那么对于不同的行，
		 * 列列表会有所不同，因此我们将列列表构建推迟到当前行。
		 *
		 */
		if (insertStmt == NULL)
		{
			TableInfo  *targettab;

			insertStmt = createPQExpBuffer();

			/*
			 * 当设置 load-via-partition-root 或强制使用时，
			 * 获取分区表的根表名称，以便数据被加载到分区根中。
			 *
			 */
			if (tbinfo->ispartition &&
				(dopt->load_via_partition_root ||
				 forcePartitionRootLoad(tbinfo)))
				targettab = getRootTableInfo(tbinfo);
			else
				targettab = tbinfo;

			appendPQExpBuffer(insertStmt, "INSERT INTO %s ",
							  fmtQualifiedDumpable(targettab));

			/* 零列表的边界情况 */
			if (nfields == 0)
			{
				appendPQExpBufferStr(insertStmt, "DEFAULT VALUES;\n");
			}
			else
			{
				/* 如果需要，追加列名列表 */
				if (dopt->column_inserts)
				{
					appendPQExpBufferChar(insertStmt, '(');
					for (int field = 0; field < nfields; field++)
					{
						if (field > 0)
							appendPQExpBufferStr(insertStmt, ", ");
						appendPQExpBufferStr(insertStmt,
											 fmtId(PQfname(res, field)));
					}
					appendPQExpBufferStr(insertStmt, ") ");
				}

				if (tbinfo->needs_override)
					appendPQExpBufferStr(insertStmt, "OVERRIDING SYSTEM VALUE ");

				appendPQExpBufferStr(insertStmt, "VALUES");
			}
		}

		for (int tuple = 0; tuple < PQntuples(res); tuple++)
		{
			/* 如果不处于多行 INSERT 的中间，则写入 INSERT */
			if (rows_this_statement == 0)
				archputs(insertStmt->data, fout);

			/*
			 * 如果是零列表，那么我们已经写出了完整的语句，
			 * 这意味着我们已经输出了足够的数据作为一行。
			 * 如果我们不在多行 INSERT 块的中间，则跳过这一行。
			 * 
			 * 如果我们完全处于多行 INSERT 块的中间，
			 * 我们就把它展开成一组单行 INSERT 语句。
			 *
			 */
			if (nfields == 0)
				continue;

			/* 输出行标题 */
			if (rows_per_statement == 1)
				archputs(" (", fout);
			else if (rows_this_statement > 0)
				archputs(",\n\t(", fout);
			else
				archputs("\n\t(", fout);

			for (int field = 0; field < nfields; field++)
			{
				if (field > 0)
					archputs(", ", fout);
				if (attgenerated[field])
				{
					archputs("DEFAULT", fout);
					continue;
				}
				if (PQgetisnull(res, tuple, field))
				{
					archputs("NULL", fout);
					continue;
				}

				/* XXX 此代码在 ruleutils.c 中有部分重复 */
				switch (PQftype(res, field))
				{
					case INT2OID:
					case INT4OID:
					case INT8OID:
					case OIDOID:
					case FLOAT4OID:
					case FLOAT8OID:
					case NUMERICOID:
						{
							/*
							 * 这些类型不带引号打印，除非它们包含不被类型输入函数
							 * 接受的值（数据类型的输入函数将正确处理），
							 * 或者如果它们是数值类型并带有 Infinity、-Infinity 或 NaN。
							 * 
							 * 这里我们以引号形式输出这些特定的引用值。
							 * 
							 * 然而，为了可移植性，其他平台特定的无限大和 NaN 表示
							 * 仍然以不带引号的形式输出。这包括 inf 和 -inf 用于 float4/float8。
							 *
							 */
							const char *s = PQgetvalue(res, tuple, field);

							if (strspn(s, "0123456789 +-eE.") == strlen(s))
								archputs(s, fout);
							else
								archprintf(fout, "'%s'", s);
						}
						break;

					case BITOID:
					case VARBITOID:
						archprintf(fout, "B'%s'",
								   PQgetvalue(res, tuple, field));
						break;

					case BOOLOID:
						if (strcmp(PQgetvalue(res, tuple, field), "t") == 0)
							archputs("true", fout);
						else
							archputs("false", fout);
						break;

					default:
						/* 所有其他类型都以字符串字面量打印。 */
						resetPQExpBuffer(q);
						appendStringLiteralAH(q,
											  PQgetvalue(res, tuple, field),
											  fout);
						archputs(q->data, fout);
						break;
				}
			}

			/* 终止该行... */
			archputs(")", fout);

			/* ...并在达到目标行数时终止语句 */
			if (++rows_this_statement >= rows_per_statement)
			{
				if (dopt->do_nothing)
					archputs(" ON CONFLICT DO NOTHING;\n", fout);
				else
					archputs(";\n", fout);
				/* 重置行计数器 */
				rows_this_statement = 0;
			}
		}

		if (PQntuples(res) <= 0)
		{
			PQclear(res);
			break;
		}
		PQclear(res);
	}

	/* 终止所有未达到行数的语句。 */
	if (rows_this_statement > 0)
	{
		if (dopt->do_nothing)
			archputs(" ON CONFLICT DO NOTHING;\n", fout);
		else
			archputs(";\n", fout);
	}

	archputs("\n\n", fout);

	ExecuteSqlStatement(fout, "CLOSE _pg_dump_cursor");

	destroyPQExpBuffer(q);
	if (insertStmt != NULL)
		destroyPQExpBuffer(insertStmt);
	free(attgenerated);

	/* 恢复设置 */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
		set_restrict_relation_kind(fout, "view, foreign-table");

	return 1;
}

/*
 * getRootTableInfo：
 *     获取给定分区表的根 TableInfo。
 */
static TableInfo *
getRootTableInfo(const TableInfo *tbinfo)
{
	TableInfo  *parentTbinfo;

	Assert(tbinfo->ispartition);
	Assert(tbinfo->numParents == 1);

	parentTbinfo = tbinfo->parents[0];
	while (parentTbinfo->ispartition)
	{
		Assert(parentTbinfo->numParents == 1);
		parentTbinfo = parentTbinfo->parents[0];
	}

	return parentTbinfo;
}

/*
 * forcePartitionRootLoad
 *     检查我们是否必须为此表强制 load_via_partition_root。
 * 
 * 我们必须为分区表强制使用此选项，
 * 因为其父表可能不存在于归档文件中。
 */
static bool
forcePartitionRootLoad(const TableInfo *tbinfo)
{
	TableInfo  *parentTbinfo;

	Assert(tbinfo->ispartition);
	Assert(tbinfo->numParents == 1);

	parentTbinfo = tbinfo->parents[0];
	if (parentTbinfo->unsafe_partitions)
		return true;
	while (parentTbinfo->ispartition)
	{
		Assert(parentTbinfo->numParents == 1);
		parentTbinfo = parentTbinfo->parents[0];
		if (parentTbinfo->unsafe_partitions)
			return true;
	}

	return false;
}

/*
 * dumpTableData -
 *   导出单个表的内容
 * 
 * 实际上，这只是通过归档器调度转储。
 */
static void
dumpTableData(Archive *fout, const TableDataInfo *tdinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer copyBuf = createPQExpBuffer();
	PQExpBuffer clistBuf = createPQExpBuffer();
	DataDumperPtr dumpFn;
	char	   *tdDefn = NULL;
	char	   *copyStmt;
	const char *copyFrom;

	/* 我们最好已经加载了此表的逐列详细信息 */
	Assert(tbinfo->interesting);

	/*
	 * 当设置 load-via-partition-root 或强制使用时，
	 * 获取分区表的根表名称，以便数据被加载到分区根中。
	 * 当使用 load-via-partition-root 时，
	 * 数据会被加载到分区根表中。
	 */
	if (tbinfo->ispartition &&
		(dopt->load_via_partition_root ||
		 forcePartitionRootLoad(tbinfo)))
	{
		TableInfo  *parentTbinfo;
		char	   *sanitized;

		parentTbinfo = getRootTableInfo(tbinfo);
		copyFrom = fmtQualifiedDumpable(parentTbinfo);
		sanitized = sanitize_line(copyFrom, true);
		printfPQExpBuffer(copyBuf, "-- load via partition root %s",
						  sanitized);
		free(sanitized);
		tdDefn = pg_strdup(copyBuf->data);
	}
	else
		copyFrom = fmtQualifiedDumpable(tbinfo);

	if (dopt->dump_inserts == 0)
	{
		/* 使用 COPY 导出/恢复 */
		dumpFn = dumpTableData_copy;
		/* 必须使用两个步骤，因为 fmtId 不可重入 */
		printfPQExpBuffer(copyBuf, "COPY %s ",
						  copyFrom);
		appendPQExpBuffer(copyBuf, "%s FROM stdin;\n",
						  fmtCopyColumnList(tbinfo, clistBuf));
		copyStmt = copyBuf->data;
	}
	else
	{
		/* 使用 INSERT 恢复 */
		dumpFn = dumpTableData_insert;
		copyStmt = NULL;
	}

	/*
	 * 注意：虽然 TableDataInfo 是一个完整的 DumpableObject，
	 * 我们将其依赖关系视为与表的数据对象的依赖关系一样精细。
	 * 这样做是为了帮助我们正确排序。
	 */
	if (tdinfo->dobj.dump & DUMP_COMPONENT_DATA)
	{
		TocEntry   *te;

		te = ArchiveEntry(fout, tdinfo->dobj.catId, tdinfo->dobj.dumpId,
						  ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
									   .namespace = tbinfo->dobj.namespace->dobj.name,
									   .owner = tbinfo->rolname,
									   .description = "TABLE DATA",
									   .section = SECTION_DATA,
									   .createStmt = tdDefn,
									   .copyStmt = copyStmt,
									   .deps = &(tbinfo->dobj.dumpId),
									   .nDeps = 1,
									   .dumpFn = dumpFn,
									   .dumpArg = tdinfo));

		/*
		 * 设置 TocEntry 的 dataLength，以便我们进行并行导出并按表大小
		 * 排序导出作业。我们选择在导出期间以表页（包括 TOAST 页）
		 * 为单位度量 dataLength，因此不需要缩放。
		 * 
		 * 但是，relpages 在 pg_class 中声明为 "integer"，
		 * 因此在 TableInfo 中也是如此，但它实际上是 BlockNumber，
		 * 即 unsigned int。进行强制转换，以便对超过 INT_MAX 页的
		 * 表大小获得正确的解释。
		 *
		 */
		te->dataLength = (BlockNumber) tbinfo->relpages;
		te->dataLength += (BlockNumber) tbinfo->toastpages;

		/*
		 * 如果 pgoff_t 只有 32 位宽，上述细化是无用的，
		 * 相反，我们使用估算的文件系统页面大小以避免溢出。
		 *
		 */
		if (sizeof(te->dataLength) == 4 &&
			(tbinfo->relpages < 0 || tbinfo->toastpages < 0 ||
			 te->dataLength < 0))
			te->dataLength = INT_MAX;
	}

	destroyPQExpBuffer(copyBuf);
	destroyPQExpBuffer(clistBuf);
}

/*
 * refreshMatViewData -
 *   加载或刷新单个物化视图的内容
 * 
 * 这实际上只是通过归档器调度刷新。
 *
 */
static void
refreshMatViewData(Archive *fout, const TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	PQExpBuffer q;

	/* 如果物化视图未标记为已填充，则跳过。 */
	if (!tbinfo->relispopulated)
		return;

	q = createPQExpBuffer();

	appendPQExpBuffer(q, "REFRESH MATERIALIZED VIEW %s;\n",
					  fmtQualifiedDumpable(tbinfo));

	if (tdinfo->dobj.dump & DUMP_COMPONENT_DATA)
		ArchiveEntry(fout,
					 tdinfo->dobj.catId,	/* 目录 ID */
					 tdinfo->dobj.dumpId,	/* 导出 ID */
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "MATERIALIZED VIEW DATA",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data,
								  .deps = tdinfo->dobj.dependencies,
								  .nDeps = tdinfo->dobj.nDeps));

	destroyPQExpBuffer(q);
}

/*
 * getTableData -
 *   设置表示表内容的可导出对象
 */
static void
getTableData(DumpOptions *dopt, TableInfo *tblinfo, int numTables, char relkind)
{
	int			i;

	for (i = 0; i < numTables; i++)
	{
		if (tblinfo[i].dobj.dump & DUMP_COMPONENT_DATA &&
			(!relkind || tblinfo[i].relkind == relkind))
			makeTableDataInfo(dopt, &(tblinfo[i]));
	}
}

/*
 * 为此特定表的数据创建可导出对象
 * 
 * 注意：我们为每个要导出数据的表创建一个 TableDataInfo。
 *
 */
static void
makeTableDataInfo(DumpOptions *dopt, TableInfo *tbinfo)
{
	TableDataInfo *tdinfo;

	/*
	 * 如果我们已经决定导出该表，则无事可做。
	 * 如果为表指定了 --data-only，就会发生这种情况。
	 */
	if (tbinfo->dataObj != NULL)
		return;

	/* 跳过视图（没有数据可导出） */
	if (tbinfo->relkind == RELKIND_VIEW)
		return;
	/* 跳过外部表（没有数据可导出），除非显式请求 */
	if (tbinfo->relkind == RELKIND_FOREIGN_TABLE &&
		(foreign_servers_include_oids.head == NULL ||
		 !simple_oid_list_member(&foreign_servers_include_oids,
								 tbinfo->foreign_server)))
		return;
	/* 跳过分区表（数据在分区中） */
	if (tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* 如果请求，不要导出未记录日志的表中的数据 */
	if (tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED &&
		dopt->no_unlogged_table_data)
		return;

	/* 检查数据是否被显式排除 */
	if (simple_oid_list_member(&tabledata_exclude_oids,
							   tbinfo->dobj.catId.oid))
		return;

	/* 好的，让我们导出它 */
	tdinfo = (TableDataInfo *) pg_malloc(sizeof(TableDataInfo));

	if (tbinfo->relkind == RELKIND_MATVIEW)
		tdinfo->dobj.objType = DO_REFRESH_MATVIEW;
	else if (tbinfo->relkind == RELKIND_SEQUENCE)
		tdinfo->dobj.objType = DO_SEQUENCE_SET;
	else
		tdinfo->dobj.objType = DO_TABLE_DATA;

	/*
	 * 注意：使用 tableoid 0，这样此对象不会被误认为
	 * 可能与普通表有依赖关系的东西。
	 */
	tdinfo->dobj.catId.tableoid = 0;
	tdinfo->dobj.catId.oid = tbinfo->dobj.catId.oid;
	AssignDumpId(&tdinfo->dobj);
	tdinfo->dobj.name = tbinfo->dobj.name;
	tdinfo->dobj.namespace = tbinfo->dobj.namespace;
	tdinfo->tdtable = tbinfo;
	tdinfo->filtercond = NULL;	/* 稍后可能会设置 */
	addObjectDependency(&tdinfo->dobj, tbinfo->dobj.dumpId);

	/* TableDataInfo 当然包含数据 */
	tdinfo->dobj.components |= DUMP_COMPONENT_DATA;

	tbinfo->dataObj = tdinfo;

	/* 确保我们将收集此表的逐列信息。 */
	tbinfo->interesting = true;
}

/*
 * 物化视图的刷新必须依赖于该视图依赖的任何
 * 物化视图的刷新，以便在依赖视图的数据之后
 * 自动刷新视图。
 * 
 * 原始 SQL 视图的转储中没有物化视图，因此仅当
 */
static void
buildMatViewRefreshDependencies(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_classid,
				i_objid,
				i_refobjid;

	/* 9.3 之前没有物化视图。 */
	if (fout->remoteVersion < 90300)
		return;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "WITH RECURSIVE w AS "
						 "( "
						 "SELECT d1.objid, d2.refobjid, c2.relkind AS refrelkind "
						 "FROM pg_depend d1 "
						 "JOIN pg_class c1 ON c1.oid = d1.objid "
						 "AND c1.relkind = " CppAsString2(RELKIND_MATVIEW)
						 " JOIN pg_rewrite r1 ON r1.ev_class = d1.objid "
						 "JOIN pg_depend d2 ON d2.classid = 'pg_rewrite'::regclass "
						 "AND d2.objid = r1.oid "
						 "AND d2.refobjid <> d1.objid "
						 "JOIN pg_class c2 ON c2.oid = d2.refobjid "
						 "AND c2.relkind IN (" CppAsString2(RELKIND_MATVIEW) ","
						 CppAsString2(RELKIND_VIEW) ") "
						 "WHERE d1.classid = 'pg_class'::regclass "
						 "UNION "
						 "SELECT w.objid, d3.refobjid, c3.relkind "
						 "FROM w "
						 "JOIN pg_rewrite r3 ON r3.ev_class = w.refobjid "
						 "JOIN pg_depend d3 ON d3.classid = 'pg_rewrite'::regclass "
						 "AND d3.objid = r3.oid "
						 "AND d3.refobjid <> w.refobjid "
						 "JOIN pg_class c3 ON c3.oid = d3.refobjid "
						 "AND c3.relkind IN (" CppAsString2(RELKIND_MATVIEW) ","
						 CppAsString2(RELKIND_VIEW) ") "
						 ") "
						 "SELECT 'pg_class'::regclass::oid AS classid, objid, refobjid "
						 "FROM w "
						 "WHERE refrelkind = " CppAsString2(RELKIND_MATVIEW));

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refobjid = PQfnumber(res, "refobjid");

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		CatalogId	refobjId;
		DumpableObject *dobj;
		DumpableObject *refdobj;
		TableInfo  *tbinfo;
		TableInfo  *reftbinfo;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		refobjId.tableoid = objId.tableoid;
		refobjId.oid = atooid(PQgetvalue(res, i, i_refobjid));

		dobj = findObjectByCatalogId(objId);
		if (dobj == NULL)
			continue;

		Assert(dobj->objType == DO_TABLE);
		tbinfo = (TableInfo *) dobj;
		Assert(tbinfo->relkind == RELKIND_MATVIEW);
		dobj = (DumpableObject *) tbinfo->dataObj;
		if (dobj == NULL)
			continue;
		Assert(dobj->objType == DO_REFRESH_MATVIEW);

		refdobj = findObjectByCatalogId(refobjId);
		if (refdobj == NULL)
			continue;

		Assert(refdobj->objType == DO_TABLE);
		reftbinfo = (TableInfo *) refdobj;
		Assert(reftbinfo->relkind == RELKIND_MATVIEW);
		refdobj = (DumpableObject *) reftbinfo->dataObj;
		if (refdobj == NULL)
			continue;
		Assert(refdobj->objType == DO_REFRESH_MATVIEW);

		addObjectDependency(dobj, refdobj->dumpId);

		if (!reftbinfo->relispopulated)
			tbinfo->relispopulated = false;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getTableDataFKConstraints -
 *   添加反映外键约束的导出顺序依赖
 * 
 * 这些依赖确保外键所引用的表的表数据在外键
 * 所属表的表数据之前被导出。
 * 
 * 对于 --data-only，我们通过确保约束在数据之后而不是
 * 之前创建来避免外键检查，因此不需要额外的依赖。
 *
 *
 */
static void
getTableDataFKConstraints(void)
{
	DumpableObject **dobjs;
	int			numObjs;
	int			i;

	/* 在所有可导出对象中搜索 FK 约束 */
	getDumpableObjects(&dobjs, &numObjs);
	for (i = 0; i < numObjs; i++)
	{
		if (dobjs[i]->objType == DO_FK_CONSTRAINT)
		{
			ConstraintInfo *cinfo = (ConstraintInfo *) dobjs[i];
			TableInfo  *ftable;

			/* 除非两个表都要导出，否则不感兴趣 */
			if (cinfo->contable == NULL ||
				cinfo->contable->dataObj == NULL)
				continue;
			ftable = findTableByOid(cinfo->confrelid);
			if (ftable == NULL ||
				ftable->dataObj == NULL)
				continue;

			/*
			 * 好的，让引用表的 TABLE_DATA 对象依赖于被引用表的 TABLE_DATA。
			 * 
			 */
			addObjectDependency(&cinfo->contable->dataObj->dobj,
								ftable->dataObj->dobj.dumpId);
		}
	}
	free(dobjs);
}


/*
 * dumpDatabase:
 *   导出数据库定义
 */
static void
dumpDatabase(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer dbQry = createPQExpBuffer();
	PQExpBuffer delQry = createPQExpBuffer();
	PQExpBuffer creaQry = createPQExpBuffer();
	PQExpBuffer labelq = createPQExpBuffer();
	PGconn	   *conn = GetConnection(fout);
	PGresult   *res;
	int			i_tableoid,
				i_oid,
				i_datname,
				i_datdba,
				i_encoding,
				i_datlocprovider,
				i_collate,
				i_ctype,
				i_daticulocale,
				i_daticurules,
				i_frozenxid,
				i_minmxid,
				i_datacl,
				i_acldefault,
				i_datistemplate,
				i_datconnlimit,
				i_datcollversion,
				i_tablespace;
	CatalogId	dbCatId;
	DumpId		dbDumpId;
	DumpableAcl dbdacl;
	const char *datname,
			   *dba,
			   *encoding,
			   *datlocprovider,
			   *collate,
			   *ctype,
			   *iculocale,
			   *icurules,
			   *datistemplate,
			   *datconnlimit,
			   *tablespace;
	uint32		frozenxid,
				minmxid;
	char	   *qdatname;

	pg_log_info("saving database definition");

	/*
	 * 获取此数据库的数据库级别属性。
	 */
	appendPQExpBufferStr(dbQry, "SELECT tableoid, oid, datname, "
						 "datdba, "
						 "pg_encoding_to_char(encoding) AS encoding, "
						 "datcollate, datctype, datfrozenxid, "
						 "datacl, acldefault('d', datdba) AS acldefault, "
						 "datistemplate, datconnlimit, ");
	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(dbQry, "datminmxid, ");
	else
		appendPQExpBufferStr(dbQry, "0 AS datminmxid, ");
	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(dbQry, "datlocprovider, daticulocale, datcollversion, ");
	else
		appendPQExpBufferStr(dbQry, "'c' AS datlocprovider, NULL AS daticulocale, NULL AS datcollversion, ");
	if (fout->remoteVersion >= 160000)
		appendPQExpBufferStr(dbQry, "daticurules, ");
	else
		appendPQExpBufferStr(dbQry, "NULL AS daticurules, ");
	appendPQExpBufferStr(dbQry,
						 "(SELECT spcname FROM pg_tablespace t WHERE t.oid = dattablespace) AS tablespace, "
						 "shobj_description(oid, 'pg_database') AS description "
						 "FROM pg_database "
						 "WHERE datname = current_database()");

	res = ExecuteSqlQueryForSingleRow(fout, dbQry->data);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_datname = PQfnumber(res, "datname");
	i_datdba = PQfnumber(res, "datdba");
	i_encoding = PQfnumber(res, "encoding");
	i_datlocprovider = PQfnumber(res, "datlocprovider");
	i_collate = PQfnumber(res, "datcollate");
	i_ctype = PQfnumber(res, "datctype");
	i_daticulocale = PQfnumber(res, "daticulocale");
	i_daticurules = PQfnumber(res, "daticurules");
	i_frozenxid = PQfnumber(res, "datfrozenxid");
	i_minmxid = PQfnumber(res, "datminmxid");
	i_datacl = PQfnumber(res, "datacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_datistemplate = PQfnumber(res, "datistemplate");
	i_datconnlimit = PQfnumber(res, "datconnlimit");
	i_datcollversion = PQfnumber(res, "datcollversion");
	i_tablespace = PQfnumber(res, "tablespace");

	dbCatId.tableoid = atooid(PQgetvalue(res, 0, i_tableoid));
	dbCatId.oid = atooid(PQgetvalue(res, 0, i_oid));
	datname = PQgetvalue(res, 0, i_datname);
	dba = getRoleName(PQgetvalue(res, 0, i_datdba));
	encoding = PQgetvalue(res, 0, i_encoding);
	datlocprovider = PQgetvalue(res, 0, i_datlocprovider);
	collate = PQgetvalue(res, 0, i_collate);
	ctype = PQgetvalue(res, 0, i_ctype);
	if (!PQgetisnull(res, 0, i_daticulocale))
		iculocale = PQgetvalue(res, 0, i_daticulocale);
	else
		iculocale = NULL;
	if (!PQgetisnull(res, 0, i_daticurules))
		icurules = PQgetvalue(res, 0, i_daticurules);
	else
		icurules = NULL;
	frozenxid = atooid(PQgetvalue(res, 0, i_frozenxid));
	minmxid = atooid(PQgetvalue(res, 0, i_minmxid));
	dbdacl.acl = PQgetvalue(res, 0, i_datacl);
	dbdacl.acldefault = PQgetvalue(res, 0, i_acldefault);
	datistemplate = PQgetvalue(res, 0, i_datistemplate);
	datconnlimit = PQgetvalue(res, 0, i_datconnlimit);
	tablespace = PQgetvalue(res, 0, i_tablespace);

	qdatname = pg_strdup(fmtId(datname));

	/*
	 * 准备 CREATE DATABASE 命令。如果需要保留 OID，必须指定它，
	 * 并且还需要设置其他一些选项。
	 * 注意：我们无法使用 fmtQualifiedId，因为它会生成多余的引号。
	 * 
	 *
	 */
	if (dopt->binary_upgrade)
	{
		appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0 OID = %u",
						  qdatname, dbCatId.oid);
	}
	else
	{
		appendPQExpBuffer(creaQry, "CREATE DATABASE %s WITH TEMPLATE = template0",
						  qdatname);
	}
	if (strlen(encoding) > 0)
	{
		appendPQExpBufferStr(creaQry, " ENCODING = ");
		appendStringLiteralAH(creaQry, encoding, fout);
	}

	appendPQExpBufferStr(creaQry, " LOCALE_PROVIDER = ");
	if (datlocprovider[0] == 'c')
		appendPQExpBufferStr(creaQry, "libc");
	else if (datlocprovider[0] == 'i')
		appendPQExpBufferStr(creaQry, "icu");
	else
		pg_fatal("unrecognized locale provider: %s",
				 datlocprovider);

	if (strlen(collate) > 0 && strcmp(collate, ctype) == 0)
	{
		appendPQExpBufferStr(creaQry, " LOCALE = ");
		appendStringLiteralAH(creaQry, collate, fout);
	}
	else
	{
		if (strlen(collate) > 0)
		{
			appendPQExpBufferStr(creaQry, " LC_COLLATE = ");
			appendStringLiteralAH(creaQry, collate, fout);
		}
		if (strlen(ctype) > 0)
		{
			appendPQExpBufferStr(creaQry, " LC_CTYPE = ");
			appendStringLiteralAH(creaQry, ctype, fout);
		}
	}
	if (iculocale)
	{
		appendPQExpBufferStr(creaQry, " ICU_LOCALE = ");
		appendStringLiteralAH(creaQry, iculocale, fout);
	}
	if (icurules)
	{
		appendPQExpBufferStr(creaQry, " ICU_RULES = ");
		appendStringLiteralAH(creaQry, icurules, fout);
	}

	/*
	 * 对于二进制升级，继承排序规则版本。
	 * 对于正常的导出/恢复，省略该设置。
	 */
	if (dopt->binary_upgrade)
	{
		if (!PQgetisnull(res, 0, i_datcollversion))
		{
			appendPQExpBufferStr(creaQry, " COLLATION_VERSION = ");
			appendStringLiteralAH(creaQry,
								  PQgetvalue(res, 0, i_datcollversion),
								  fout);
		}
	}

	/*
	 * 注意：此处查看 dopt->outputNoTablespaces 是完全错误的；
	 * 决定在 pg_restore 中做出。但是它提供了恢复选项的
	 * 非常有用的使用方式。这可能应该改变。
	 * 
	 * 为清楚起见，在二进制升级模式下始终设置表空间。
	 *
	 */
	if (strlen(tablespace) > 0 && strcmp(tablespace, "pg_default") != 0 &&
		!dopt->outputNoTablespaces)
		appendPQExpBuffer(creaQry, " TABLESPACE = %s",
						  fmtId(tablespace));
	appendPQExpBufferStr(creaQry, ";\n");

	appendPQExpBuffer(delQry, "DROP DATABASE %s;\n",
					  qdatname);

	dbDumpId = createDumpId();

	ArchiveEntry(fout,
				 dbCatId,		/* 目录 ID */
				 dbDumpId,		/* 导出 ID */
				 ARCHIVE_OPTS(.tag = datname,
							  .owner = dba,
							  .description = "DATABASE",
							  .section = SECTION_PRE_DATA,
							  .createStmt = creaQry->data,
							  .dropStmt = delQry->data));

	/* 为归档条目计算正确的标签 */
	appendPQExpBuffer(labelq, "DATABASE %s", qdatname);

	/* 导出数据库注释（如果有） */
	{
		/*
		 * 8.2 及以上版本将共享对象上的注释保存在共享表中，
		 * 因此我们不能使用 dumpComment。
		 * 
		 */
		char	   *comment = PQgetvalue(res, 0, PQfnumber(res, "description"));

		if (comment && *comment && !dopt->no_comments)
		{
			resetPQExpBuffer(dbQry);

			/*
			 * 加载到不同名称的数据库时生成警告。
			 * 
			 */
			appendPQExpBuffer(dbQry, "COMMENT ON DATABASE %s IS ", qdatname);
			appendStringLiteralAH(dbQry, comment, fout);
			appendPQExpBufferStr(dbQry, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = labelq->data,
									  .owner = dba,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = dbQry->data,
									  .deps = &dbDumpId,
									  .nDeps = 1));
		}
	}

	/* 导出数据库安全标签（如果启用） */
	if (!dopt->no_security_labels)
	{
		PGresult   *shres;
		PQExpBuffer seclabelQry;

		seclabelQry = createPQExpBuffer();

		buildShSecLabelQuery("pg_database", dbCatId.oid, seclabelQry);
		shres = ExecuteSqlQuery(fout, seclabelQry->data, PGRES_TUPLES_OK);
		resetPQExpBuffer(seclabelQry);
		emitShSecLabels(conn, shres, seclabelQry, "DATABASE", datname);
		if (seclabelQry->len > 0)
			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = labelq->data,
									  .owner = dba,
									  .description = "SECURITY LABEL",
									  .section = SECTION_NONE,
									  .createStmt = seclabelQry->data,
									  .deps = &dbDumpId,
									  .nDeps = 1));
		destroyPQExpBuffer(seclabelQry);
		PQclear(shres);
	}

	/*
	 * 导出 ACL（如果有）。注意，我们不支持数据库上
	 * 的初始特权（pg_init_privs），因为它们始终是默认的。
	 */
	dbdacl.privtype = 0;
	dbdacl.initprivs = NULL;

	dumpACL(fout, dbDumpId, InvalidDumpId, "DATABASE",
			qdatname, NULL, NULL,
			dba, &dbdacl);

	/*
	 * 现在构造一个 DATABASE PROPERTIES 归档条目，
	 * 以恢复可能已更改的非默认数据库级 GUC 变量。
	 * 此外，在二进制升级模式下，我们向此条目添加查询，
	 * 以重置某些关键系统目录的 relfrozenxid 和相关字段。
	 * 
	 * 这必须在加载其他内容之前发生。
	 */
	resetPQExpBuffer(creaQry);
	resetPQExpBuffer(delQry);

	if (strlen(datconnlimit) > 0 && strcmp(datconnlimit, "-1") != 0)
		appendPQExpBuffer(creaQry, "ALTER DATABASE %s CONNECTION LIMIT = %s;\n",
						  qdatname, datconnlimit);

	if (strcmp(datistemplate, "t") == 0)
	{
		appendPQExpBuffer(creaQry, "ALTER DATABASE %s IS_TEMPLATE = true;\n",
						  qdatname);

		/*
		 * 后端不会接受对模板数据库执行 DROP DATABASE。
		 * 我们可以将 ALTER 用于此处感兴趣的变量
		 * （它们不会被 CREATE DATABASE 设置），
		 * 因此输出用于模板数据库和普通数据库的
		 * ALTER DATABASE SET 命令。
		 */
		appendPQExpBufferStr(delQry, "UPDATE pg_catalog.pg_database "
							 "SET datistemplate = false WHERE datname = ");
		appendStringLiteralAH(delQry, datname, fout);
		appendPQExpBufferStr(delQry, ";\n");
	}

	/* 添加特定于数据库的 SET 选项 */
	dumpDatabaseConfig(fout, creaQry, datname, dbCatId.oid);

	/*
	 * 我们将此二进制升级查询也放入 DATABASE PROPERTIES 归档条目中，
	 * 以便在恢复期间尽早执行。
	 */
	if (dopt->binary_upgrade)
	{
		appendPQExpBufferStr(creaQry, "\n-- For binary upgrade, set datfrozenxid and datminmxid.\n");
		appendPQExpBuffer(creaQry, "UPDATE pg_catalog.pg_database\n"
						  "SET datfrozenxid = '%u', datminmxid = '%u'\n"
						  "WHERE datname = ",
						  frozenxid, minmxid);
		appendStringLiteralAH(creaQry, datname, fout);
		appendPQExpBufferStr(creaQry, ";\n");
	}

	if (creaQry->len > 0)
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = datname,
								  .owner = dba,
								  .description = "DATABASE PROPERTIES",
								  .section = SECTION_PRE_DATA,
								  .createStmt = creaQry->data,
								  .dropStmt = delQry->data,
								  .deps = &dbDumpId));

	/*
	 * pg_largeobject 来自旧系统且保持完整，
	 * 因此设置其 relfrozenxids、relminmxids 和相关字段。
	 */
	if (dopt->binary_upgrade)
	{
		PGresult   *lo_res;
		PQExpBuffer loFrozenQry = createPQExpBuffer();
		PQExpBuffer loOutQry = createPQExpBuffer();
		PQExpBuffer loHorizonQry = createPQExpBuffer();
		int			ii_relfrozenxid,
					ii_relfilenode,
					ii_oid,
					ii_relminmxid;

		/*
		 * pg_largeobject
		 */
		if (fout->remoteVersion >= 90300)
			appendPQExpBuffer(loFrozenQry, "SELECT relfrozenxid, relminmxid, relfilenode, oid\n"
							  "FROM pg_catalog.pg_class\n"
							  "WHERE oid IN (%u, %u);\n",
							  LargeObjectRelationId, LargeObjectLOidPNIndexId);
		else
			appendPQExpBuffer(loFrozenQry, "SELECT relfrozenxid, 0 AS relminmxid, relfilenode, oid\n"
							  "FROM pg_catalog.pg_class\n"
							  "WHERE oid IN (%u, %u);\n",
							  LargeObjectRelationId, LargeObjectLOidPNIndexId);

		lo_res = ExecuteSqlQuery(fout, loFrozenQry->data, PGRES_TUPLES_OK);

		ii_relfrozenxid = PQfnumber(lo_res, "relfrozenxid");
		ii_relminmxid = PQfnumber(lo_res, "relminmxid");
		ii_relfilenode = PQfnumber(lo_res, "relfilenode");
		ii_oid = PQfnumber(lo_res, "oid");

		appendPQExpBufferStr(loHorizonQry, "\n-- For binary upgrade, set pg_largeobject relfrozenxid and relminmxid\n");
		appendPQExpBufferStr(loOutQry, "\n-- For binary upgrade, preserve pg_largeobject and index relfilenodes\n");
		for (int i = 0; i < PQntuples(lo_res); ++i)
		{
			Oid			oid;
			RelFileNumber relfilenumber;

			appendPQExpBuffer(loHorizonQry, "UPDATE pg_catalog.pg_class\n"
							  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
							  "WHERE oid = %u;\n",
							  atooid(PQgetvalue(lo_res, i, ii_relfrozenxid)),
							  atooid(PQgetvalue(lo_res, i, ii_relminmxid)),
							  atooid(PQgetvalue(lo_res, i, ii_oid)));

			oid = atooid(PQgetvalue(lo_res, i, ii_oid));
			relfilenumber = atooid(PQgetvalue(lo_res, i, ii_relfilenode));

			if (oid == LargeObjectRelationId)
				appendPQExpBuffer(loOutQry,
								  "SELECT pg_catalog.binary_upgrade_set_next_heap_relfilenode('%u'::pg_catalog.oid);\n",
								  relfilenumber);
			else if (oid == LargeObjectLOidPNIndexId)
				appendPQExpBuffer(loOutQry,
								  "SELECT pg_catalog.binary_upgrade_set_next_index_relfilenode('%u'::pg_catalog.oid);\n",
								  relfilenumber);
		}

		appendPQExpBufferStr(loOutQry,
							 "TRUNCATE pg_catalog.pg_largeobject;\n");
		appendPQExpBufferStr(loOutQry, loHorizonQry->data);

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = "pg_largeobject",
								  .description = "pg_largeobject",
								  .section = SECTION_PRE_DATA,
								  .createStmt = loOutQry->data));

		PQclear(lo_res);

		destroyPQExpBuffer(loFrozenQry);
		destroyPQExpBuffer(loHorizonQry);
		destroyPQExpBuffer(loOutQry);
	}

	PQclear(res);

	free(qdatname);
	destroyPQExpBuffer(dbQry);
	destroyPQExpBuffer(delQry);
	destroyPQExpBuffer(creaQry);
	destroyPQExpBuffer(labelq);
}

/*
 * 为此数据库收集任何数据库特定或角色及数据库特定的
 * SET 选项，并返回它们以便构建 DATABASE PROPERTIES 条目。
 */
static void
dumpDatabaseConfig(Archive *AH, PQExpBuffer outbuf,
				   const char *dbname, Oid dboid)
{
	PGconn	   *conn = GetConnection(AH);
	PQExpBuffer buf = createPQExpBuffer();
	PGresult   *res;

	/* 首先收集数据库特定的选项 */
	printfPQExpBuffer(buf, "SELECT unnest(setconfig) FROM pg_db_role_setting "
					  "WHERE setrole = 0 AND setdatabase = '%u'::oid",
					  dboid);

	res = ExecuteSqlQuery(AH, buf->data, PGRES_TUPLES_OK);

	for (int i = 0; i < PQntuples(res); i++)
		makeAlterConfigCommand(conn, PQgetvalue(res, i, 0),
							   "DATABASE", dbname, NULL, NULL,
							   outbuf);

	PQclear(res);

	/* 现在查找角色及数据库特定的选项 */
	printfPQExpBuffer(buf, "SELECT rolname, unnest(setconfig) "
					  "FROM pg_db_role_setting s, pg_roles r "
					  "WHERE setrole = r.oid AND setdatabase = '%u'::oid",
					  dboid);

	res = ExecuteSqlQuery(AH, buf->data, PGRES_TUPLES_OK);

	for (int i = 0; i < PQntuples(res); i++)
		makeAlterConfigCommand(conn, PQgetvalue(res, i, 1),
							   "ROLE", PQgetvalue(res, i, 0),
							   "DATABASE", dbname,
							   outbuf);

	PQclear(res);

	destroyPQExpBuffer(buf);
}

/*
 * dumpEncoding：将正确的编码放入归档
 */
static void
dumpEncoding(Archive *AH)
{
	const char *encname = pg_encoding_to_char(AH->encoding);
	PQExpBuffer qry = createPQExpBuffer();

	pg_log_info("saving encoding = %s", encname);

	appendPQExpBufferStr(qry, "SET client_encoding = ");
	appendStringLiteralAH(qry, encname, AH);
	appendPQExpBufferStr(qry, ";\n");

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "ENCODING",
							  .description = "ENCODING",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

	destroyPQExpBuffer(qry);
}


/*
 * dumpStdStrings：将正确的转义字符串行为放入归档
 */
static void
dumpStdStrings(Archive *AH)
{
	const char *stdstrings = AH->std_strings ? "on" : "off";
	PQExpBuffer qry = createPQExpBuffer();

	pg_log_info("saving standard_conforming_strings = %s",
				stdstrings);

	appendPQExpBuffer(qry, "SET standard_conforming_strings = '%s';\n",
					  stdstrings);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "STDSTRINGS",
							  .description = "STDSTRINGS",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

	destroyPQExpBuffer(qry);
}

/*
 * dumpSearchPath：在归档中记录活动的 search_path
 */
static void
dumpSearchPath(Archive *AH)
{
	PQExpBuffer qry = createPQExpBuffer();
	PQExpBuffer path = createPQExpBuffer();
	PGresult   *res;
	char	  **schemanames = NULL;
	int			nschemanames = 0;
	int			i;

	/*
	 * 我们使用 current_schemas() 的结果，而不是 search_path GUC，
	 * 因为后者可能包含我们不关心的模式名，
	 * 例如在自引用视图中。
	 *
	 *
	 */
	res = ExecuteSqlQueryForSingleRow(AH,
									  "SELECT pg_catalog.current_schemas(false)");

	if (!parsePGArray(PQgetvalue(res, 0, 0), &schemanames, &nschemanames))
		pg_fatal("could not parse result of current_schemas()");

	/*
	 * 我们使用 set_config()，而不是简单的 "SET search_path"，
	 * 因为后者在解析 search_path 值的方式上灵活性较差。
	 * 具体来说，它不处理嵌入的空格或引号。
	 *
	 */
	for (i = 0; i < nschemanames; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(path, ", ");
		appendPQExpBufferStr(path, fmtId(schemanames[i]));
	}

	appendPQExpBufferStr(qry, "SELECT pg_catalog.set_config('search_path', ");
	appendStringLiteralAH(qry, path->data, AH);
	appendPQExpBufferStr(qry, ", false);\n");

	pg_log_info("saving search_path = %s", path->data);

	ArchiveEntry(AH, nilCatalogId, createDumpId(),
				 ARCHIVE_OPTS(.tag = "SEARCHPATH",
							  .description = "SEARCHPATH",
							  .section = SECTION_PRE_DATA,
							  .createStmt = qry->data));

	/* 同时保存到 AH->searchpath，以备进行纯文本导出 */
	AH->searchpath = pg_strdup(qry->data);

	free(schemanames);
	PQclear(res);
	destroyPQExpBuffer(qry);
	destroyPQExpBuffer(path);
}


/*
 * getLOs:
 *   收集大对象的模式级别数据
 */
static void
getLOs(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer loQry = createPQExpBuffer();
	LoInfo	   *loinfo;
	DumpableObject *lodata;
	PGresult   *res;
	int			ntups;
	int			i;
	int			i_oid;
	int			i_lomowner;
	int			i_lomacl;
	int			i_acldefault;

	pg_log_info("reading large objects");

	/* 获取大对象 OID 以及所有者/ACL 数据 */
	appendPQExpBufferStr(loQry,
						 "SELECT oid, lomowner, lomacl, "
						 "acldefault('L', lomowner) AS acldefault "
						 "FROM pg_largeobject_metadata");

	res = ExecuteSqlQuery(fout, loQry->data, PGRES_TUPLES_OK);

	i_oid = PQfnumber(res, "oid");
	i_lomowner = PQfnumber(res, "lomowner");
	i_lomacl = PQfnumber(res, "lomacl");
	i_acldefault = PQfnumber(res, "acldefault");

	ntups = PQntuples(res);

	/*
	 * 每个大对象都有自己的 "BLOB" 归档条目。
	 */
	loinfo = (LoInfo *) pg_malloc(ntups * sizeof(LoInfo));

	for (i = 0; i < ntups; i++)
	{
		loinfo[i].dobj.objType = DO_LARGE_OBJECT;
		loinfo[i].dobj.catId.tableoid = LargeObjectRelationId;
		loinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&loinfo[i].dobj);

		loinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_oid));
		loinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_lomacl));
		loinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		loinfo[i].dacl.privtype = 0;
		loinfo[i].dacl.initprivs = NULL;
		loinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_lomowner));

		/* 大对象有数据 */
		loinfo[i].dobj.components |= DUMP_COMPONENT_DATA;

		/* 标记大对象是否有 ACL */
		if (!PQgetisnull(res, i, i_lomacl))
			loinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

		/*
		 * 在大对象的二进制升级模式中，我们*不*导出大对象数据，
		 * 因为它将*原样*从旧集群复制过来。
		 * 但是，我们仍然会导出元数据（注释/安全标签），
		 * 因为 pg_largeobject_metadata 不会被复制。
		 *
		 */
		if (dopt->binary_upgrade)
			loinfo[i].dobj.dump &= ~DUMP_COMPONENT_DATA;
	}

	/*
	 * 如果我们有任何大对象，则需要 "BLOBS" 归档条目。
	 * 这只是一个占位符，用于决定是否在恢复时执行
	 */
	if (ntups > 0)
	{
		lodata = (DumpableObject *) pg_malloc(sizeof(DumpableObject));
		lodata->objType = DO_LARGE_OBJECT_DATA;
		lodata->catId = nilCatalogId;
		AssignDumpId(lodata);
		lodata->name = pg_strdup("BLOBS");
		lodata->components |= DUMP_COMPONENT_DATA;
	}

	PQclear(res);
	destroyPQExpBuffer(loQry);
}

/*
 * dumpLO
 * 
 *   导出给定大对象的定义（元数据）
 */
static void
dumpLO(Archive *fout, const LoInfo *loinfo)
{
	PQExpBuffer cquery = createPQExpBuffer();
	PQExpBuffer dquery = createPQExpBuffer();

	appendPQExpBuffer(cquery,
					  "SELECT pg_catalog.lo_create('%s');\n",
					  loinfo->dobj.name);

	appendPQExpBuffer(dquery,
					  "SELECT pg_catalog.lo_unlink('%s');\n",
					  loinfo->dobj.name);

	if (loinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, loinfo->dobj.catId, loinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = loinfo->dobj.name,
								  .owner = loinfo->rolname,
								  .description = "BLOB",
								  .section = SECTION_PRE_DATA,
								  .createStmt = cquery->data,
								  .dropStmt = dquery->data));

	/* 导出注释（如果有） */
	if (loinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "LARGE OBJECT", loinfo->dobj.name,
					NULL, loinfo->rolname,
					loinfo->dobj.catId, 0, loinfo->dobj.dumpId);

	/* 导出安全标签（如果有） */
	if (loinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "LARGE OBJECT", loinfo->dobj.name,
					 NULL, loinfo->rolname,
					 loinfo->dobj.catId, 0, loinfo->dobj.dumpId);

	/* 导出 ACL（如果有） */
	if (loinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, loinfo->dobj.dumpId, InvalidDumpId, "LARGE OBJECT",
				loinfo->dobj.name, NULL,
				NULL, loinfo->rolname, &loinfo->dacl);

	destroyPQExpBuffer(cquery);
	destroyPQExpBuffer(dquery);
}

/*
 * dumpLOs:
 *   导出所有大对象的数据内容
 */
static int
dumpLOs(Archive *fout, const void *arg)
{
	const char *loQry;
	const char *loFetchQry;
	PGconn	   *conn = GetConnection(fout);
	PGresult   *res;
	char		buf[LOBBUFSIZE];
	int			ntups;
	int			i;
	int			cnt;

	pg_log_info("saving large objects");

	/*
	 * 目前，我们使用游标重新获取所有大对象 OID。
	 * 考虑改为扫描已加载到内存中的 LoInfo 数组。
	 */
	loQry =
		"DECLARE looid CURSOR FOR "
		"SELECT oid FROM pg_largeobject_metadata ORDER BY 1";

	ExecuteSqlStatement(fout, loQry);

	/* 从游标获取的命令 */
	loFetchQry = "FETCH 1000 IN looid";

	do
	{
		/* 执行获取 */
		res = ExecuteSqlQuery(fout, loFetchQry, PGRES_TUPLES_OK);

		/* 处理元组（如果有） */
		ntups = PQntuples(res);
		for (i = 0; i < ntups; i++)
		{
			Oid			loOid;
			int			loFd;

			loOid = atooid(PQgetvalue(res, i, 0));
			/* 打开大对象 */
			loFd = lo_open(conn, loOid, INV_READ);
			if (loFd == -1)
				pg_fatal("could not open large object %u: %s",
						 loOid, PQerrorMessage(conn));

			StartLO(fout, loOid);

			/* 现在分块读取它，将数据发送到归档 */
			do
			{
				cnt = lo_read(conn, loFd, buf, LOBBUFSIZE);
				if (cnt < 0)
					pg_fatal("error reading large object %u: %s",
							 loOid, PQerrorMessage(conn));

				WriteData(fout, buf, cnt);
			} while (cnt > 0);

			lo_close(conn, loFd);

			EndLO(fout, loOid);
		}

		PQclear(res);
	} while (ntups > 0);

	return 1;
}

/*
 * getPolicies
 *   获取可导出表上所有 RLS 策略的信息。
 */
void
getPolicies(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query;
	PQExpBuffer tbloids;
	PGresult   *res;
	PolicyInfo *polinfo;
	int			i_oid;
	int			i_tableoid;
	int			i_polrelid;
	int			i_polname;
	int			i_polcmd;
	int			i_polpermissive;
	int			i_polroles;
	int			i_polqual;
	int			i_polwithcheck;
	int			i,
				j,
				ntups;

	/* 9.5 之前没有策略 */
	if (fout->remoteVersion < 90500)
		return;

	query = createPQExpBuffer();
	tbloids = createPQExpBuffer();

	/*
	 * 识别感兴趣的表，并检查哪些启用了 RLS。
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* 忽略不导出的表上的行安全性 */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_POLICY))
			continue;

		/* 如果不是表，则不能有 RLS 或策略 */
		if (tbinfo->relkind != RELKIND_RELATION &&
			tbinfo->relkind != RELKIND_PARTITIONED_TABLE)
			continue;

		/* 将其添加到下面要探测的表 OID 列表中 */
		if (tbloids->len > 1)	/* 我们是否有超过 '{' 的内容？ */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);

		/* RLS 是否已启用？（这与是否有策略是分开的） */
		if (tbinfo->rowsec)
		{
			tbinfo->dobj.components |= DUMP_COMPONENT_POLICY;

			/*
			 * 我们通过在表上创建一个带有空 polname 的 PolicyInfo 对象
			 * 来表示表上启用了 RLS。
			 * 
			 * 然后 dumpPolicy 写出启用 ROW LEVEL SECURITY 所需的命令。
			 *
			 */
			polinfo = pg_malloc(sizeof(PolicyInfo));
			polinfo->dobj.objType = DO_POLICY;
			polinfo->dobj.catId.tableoid = 0;
			polinfo->dobj.catId.oid = tbinfo->dobj.catId.oid;
			AssignDumpId(&polinfo->dobj);
			polinfo->dobj.namespace = tbinfo->dobj.namespace;
			polinfo->dobj.name = pg_strdup(tbinfo->dobj.name);
			polinfo->poltable = tbinfo;
			polinfo->polname = NULL;
			polinfo->polcmd = '\0';
			polinfo->polpermissive = 0;
			polinfo->polroles = NULL;
			polinfo->polqual = NULL;
			polinfo->polwithcheck = NULL;
		}
	}
	appendPQExpBufferChar(tbloids, '}');

	/*
	 * 现在，读取属于感兴趣表的所有 RLS 策略，
	 * 并为每个策略创建 PolicyInfo 对象。
	 * 
	 * 这包括一切 —— 甚至是默认策略。
	 */
	pg_log_info("reading row-level security policies");

	printfPQExpBuffer(query,
					  "SELECT pol.oid, pol.tableoid, pol.polrelid, pol.polname, pol.polcmd, ");
	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query, "pol.polpermissive, ");
	else
		appendPQExpBufferStr(query, "'t' as polpermissive, ");
	appendPQExpBuffer(query,
					  "CASE WHEN pol.polroles = '{0}' THEN NULL ELSE "
					  "   pg_catalog.array_to_string(ARRAY(SELECT pg_catalog.quote_ident(rolname) from pg_catalog.pg_roles WHERE oid = ANY(pol.polroles)), ', ') END AS polroles, "
					  "pg_catalog.pg_get_expr(pol.polqual, pol.polrelid) AS polqual, "
					  "pg_catalog.pg_get_expr(pol.polwithcheck, pol.polrelid) AS polwithcheck "
					  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
					  "JOIN pg_catalog.pg_policy pol ON (src.tbloid = pol.polrelid)",
					  tbloids->data);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	if (ntups > 0)
	{
		i_oid = PQfnumber(res, "oid");
		i_tableoid = PQfnumber(res, "tableoid");
		i_polrelid = PQfnumber(res, "polrelid");
		i_polname = PQfnumber(res, "polname");
		i_polcmd = PQfnumber(res, "polcmd");
		i_polpermissive = PQfnumber(res, "polpermissive");
		i_polroles = PQfnumber(res, "polroles");
		i_polqual = PQfnumber(res, "polqual");
		i_polwithcheck = PQfnumber(res, "polwithcheck");

		polinfo = pg_malloc(ntups * sizeof(PolicyInfo));

		for (j = 0; j < ntups; j++)
		{
			Oid			polrelid = atooid(PQgetvalue(res, j, i_polrelid));
			TableInfo  *tbinfo = findTableByOid(polrelid);

			tbinfo->dobj.components |= DUMP_COMPONENT_POLICY;

			polinfo[j].dobj.objType = DO_POLICY;
			polinfo[j].dobj.catId.tableoid =
				atooid(PQgetvalue(res, j, i_tableoid));
			polinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&polinfo[j].dobj);
			polinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			polinfo[j].poltable = tbinfo;
			polinfo[j].polname = pg_strdup(PQgetvalue(res, j, i_polname));
			polinfo[j].dobj.name = pg_strdup(polinfo[j].polname);

			polinfo[j].polcmd = *(PQgetvalue(res, j, i_polcmd));
			polinfo[j].polpermissive = *(PQgetvalue(res, j, i_polpermissive)) == 't';

			if (PQgetisnull(res, j, i_polroles))
				polinfo[j].polroles = NULL;
			else
				polinfo[j].polroles = pg_strdup(PQgetvalue(res, j, i_polroles));

			if (PQgetisnull(res, j, i_polqual))
				polinfo[j].polqual = NULL;
			else
				polinfo[j].polqual = pg_strdup(PQgetvalue(res, j, i_polqual));

			if (PQgetisnull(res, j, i_polwithcheck))
				polinfo[j].polwithcheck = NULL;
			else
				polinfo[j].polwithcheck
					= pg_strdup(PQgetvalue(res, j, i_polwithcheck));
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
}

/*
 * dumpPolicy
 *   导出给定策略的定义
 */
static void
dumpPolicy(Archive *fout, const PolicyInfo *polinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = polinfo->poltable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PQExpBuffer polprefix;
	char	   *qtabname;
	const char *cmd;
	char	   *tag;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/*
	 * 如果 polname 为 NULL，则此记录仅指示 ROW LEVEL SECURITY 已启用（见上文）。
	 *
	 *
	 */
	if (polinfo->polname == NULL)
	{
		query = createPQExpBuffer();

		appendPQExpBuffer(query, "ALTER TABLE %s ENABLE ROW LEVEL SECURITY;",
						  fmtQualifiedDumpable(tbinfo));

		/*
		 * 我们必须显式发出 ROW SECURITY 对象对其表的依赖，
		 * 因为它将引用一个不存在的对象。
		 *
		 */
		if (polinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, polinfo->dobj.catId, polinfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = polinfo->dobj.name,
									  .namespace = polinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "ROW SECURITY",
									  .section = SECTION_POST_DATA,
									  .createStmt = query->data,
									  .deps = &(tbinfo->dobj.dumpId),
									  .nDeps = 1));

		destroyPQExpBuffer(query);
		return;
	}

	if (polinfo->polcmd == '*')
		cmd = "";
	else if (polinfo->polcmd == 'r')
		cmd = " FOR SELECT";
	else if (polinfo->polcmd == 'a')
		cmd = " FOR INSERT";
	else if (polinfo->polcmd == 'w')
		cmd = " FOR UPDATE";
	else if (polinfo->polcmd == 'd')
		cmd = " FOR DELETE";
	else
		pg_fatal("unexpected policy command type: %c",
				 polinfo->polcmd);

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();
	polprefix = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(query, "CREATE POLICY %s", fmtId(polinfo->polname));

	appendPQExpBuffer(query, " ON %s%s%s", fmtQualifiedDumpable(tbinfo),
					  !polinfo->polpermissive ? " AS RESTRICTIVE" : "", cmd);

	if (polinfo->polroles != NULL)
		appendPQExpBuffer(query, " TO %s", polinfo->polroles);

	if (polinfo->polqual != NULL)
		appendPQExpBuffer(query, " USING (%s)", polinfo->polqual);

	if (polinfo->polwithcheck != NULL)
		appendPQExpBuffer(query, " WITH CHECK (%s)", polinfo->polwithcheck);

	appendPQExpBufferStr(query, ";\n");

	appendPQExpBuffer(delqry, "DROP POLICY %s", fmtId(polinfo->polname));
	appendPQExpBuffer(delqry, " ON %s;\n", fmtQualifiedDumpable(tbinfo));

	appendPQExpBuffer(polprefix, "POLICY %s ON",
					  fmtId(polinfo->polname));

	tag = psprintf("%s %s", tbinfo->dobj.name, polinfo->dobj.name);

	if (polinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, polinfo->dobj.catId, polinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = polinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "POLICY",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	if (polinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, polprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					polinfo->dobj.catId, 0, polinfo->dobj.dumpId);

	free(tag);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(polprefix);
	free(qtabname);
}

/*
 * getPublications
 *   获取有关发布的信息
 */
PublicationInfo *
getPublications(Archive *fout, int *numPublications)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PGresult   *res;
	PublicationInfo *pubinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_pubname;
	int			i_pubowner;
	int			i_puballtables;
	int			i_pubinsert;
	int			i_pubupdate;
	int			i_pubdelete;
	int			i_pubtruncate;
	int			i_pubviaroot;
	int			i,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 100000)
	{
		*numPublications = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	resetPQExpBuffer(query);

	/* 获取发布。 */
	if (fout->remoteVersion >= 130000)
		appendPQExpBufferStr(query,
							 "SELECT p.tableoid, p.oid, p.pubname, "
							 "p.pubowner, "
							 "p.puballtables, p.pubinsert, p.pubupdate, p.pubdelete, p.pubtruncate, p.pubviaroot "
							 "FROM pg_publication p");
	else if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "SELECT p.tableoid, p.oid, p.pubname, "
							 "p.pubowner, "
							 "p.puballtables, p.pubinsert, p.pubupdate, p.pubdelete, p.pubtruncate, false AS pubviaroot "
							 "FROM pg_publication p");
	else
		appendPQExpBufferStr(query,
							 "SELECT p.tableoid, p.oid, p.pubname, "
							 "p.pubowner, "
							 "p.puballtables, p.pubinsert, p.pubupdate, p.pubdelete, false AS pubtruncate, false AS pubviaroot "
							 "FROM pg_publication p");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_pubname = PQfnumber(res, "pubname");
	i_pubowner = PQfnumber(res, "pubowner");
	i_puballtables = PQfnumber(res, "puballtables");
	i_pubinsert = PQfnumber(res, "pubinsert");
	i_pubupdate = PQfnumber(res, "pubupdate");
	i_pubdelete = PQfnumber(res, "pubdelete");
	i_pubtruncate = PQfnumber(res, "pubtruncate");
	i_pubviaroot = PQfnumber(res, "pubviaroot");

	pubinfo = pg_malloc(ntups * sizeof(PublicationInfo));

	for (i = 0; i < ntups; i++)
	{
		pubinfo[i].dobj.objType = DO_PUBLICATION;
		pubinfo[i].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubinfo[i].dobj);
		pubinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_pubname));
		pubinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_pubowner));
		pubinfo[i].puballtables =
			(strcmp(PQgetvalue(res, i, i_puballtables), "t") == 0);
		pubinfo[i].pubinsert =
			(strcmp(PQgetvalue(res, i, i_pubinsert), "t") == 0);
		pubinfo[i].pubupdate =
			(strcmp(PQgetvalue(res, i, i_pubupdate), "t") == 0);
		pubinfo[i].pubdelete =
			(strcmp(PQgetvalue(res, i, i_pubdelete), "t") == 0);
		pubinfo[i].pubtruncate =
			(strcmp(PQgetvalue(res, i, i_pubtruncate), "t") == 0);
		pubinfo[i].pubviaroot =
			(strcmp(PQgetvalue(res, i, i_pubviaroot), "t") == 0);

		/* 决定是否要导出它 */
		selectDumpableObject(&(pubinfo[i].dobj), fout);
	}
	PQclear(res);

	destroyPQExpBuffer(query);

	*numPublications = ntups;
	return pubinfo;
}

/*
 * dumpPublication
 *   导出给定发布的定义
 */
static void
dumpPublication(Archive *fout, const PublicationInfo *pubinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qpubname;
	bool		first = true;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qpubname = pg_strdup(fmtId(pubinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP PUBLICATION %s;\n",
					  qpubname);

	appendPQExpBuffer(query, "CREATE PUBLICATION %s",
					  qpubname);

	if (pubinfo->puballtables)
		appendPQExpBufferStr(query, " FOR ALL TABLES");

	appendPQExpBufferStr(query, " WITH (publish = '");
	if (pubinfo->pubinsert)
	{
		appendPQExpBufferStr(query, "insert");
		first = false;
	}

	if (pubinfo->pubupdate)
	{
		if (!first)
			appendPQExpBufferStr(query, ", ");

		appendPQExpBufferStr(query, "update");
		first = false;
	}

	if (pubinfo->pubdelete)
	{
		if (!first)
			appendPQExpBufferStr(query, ", ");

		appendPQExpBufferStr(query, "delete");
		first = false;
	}

	if (pubinfo->pubtruncate)
	{
		if (!first)
			appendPQExpBufferStr(query, ", ");

		appendPQExpBufferStr(query, "truncate");
		first = false;
	}

	appendPQExpBufferChar(query, '\'');

	if (pubinfo->pubviaroot)
		appendPQExpBufferStr(query, ", publish_via_partition_root = true");

	appendPQExpBufferStr(query, ");\n");

	if (pubinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, pubinfo->dobj.catId, pubinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = pubinfo->dobj.name,
								  .owner = pubinfo->rolname,
								  .description = "PUBLICATION",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delq->data));

	if (pubinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "PUBLICATION", qpubname,
					NULL, pubinfo->rolname,
					pubinfo->dobj.catId, 0, pubinfo->dobj.dumpId);

	if (pubinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "PUBLICATION", qpubname,
					 NULL, pubinfo->rolname,
					 pubinfo->dobj.catId, 0, pubinfo->dobj.dumpId);

	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qpubname);
}

/*
 * getPublicationNamespaces
 *   获取可导出模式的发布成员资格信息。
 */
void
getPublicationNamespaces(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	PublicationSchemaInfo *pubsinfo;
	DumpOptions *dopt = fout->dopt;
	int			i_tableoid;
	int			i_oid;
	int			i_pnpubid;
	int			i_pnnspid;
	int			i,
				j,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 150000)
		return;

	query = createPQExpBuffer();

	/* 收集所有发布成员资格信息。 */
	appendPQExpBufferStr(query,
						 "SELECT tableoid, oid, pnpubid, pnnspid "
						 "FROM pg_catalog.pg_publication_namespace");
	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_pnpubid = PQfnumber(res, "pnpubid");
	i_pnnspid = PQfnumber(res, "pnnspid");

	/* 此分配可能超过所需 */
	pubsinfo = pg_malloc(ntups * sizeof(PublicationSchemaInfo));
	j = 0;

	for (i = 0; i < ntups; i++)
	{
		Oid			pnpubid = atooid(PQgetvalue(res, i, i_pnpubid));
		Oid			pnnspid = atooid(PQgetvalue(res, i, i_pnnspid));
		PublicationInfo *pubinfo;
		NamespaceInfo *nspinfo;

		/*
		 * 忽略我们不关心的发布或引用模式的所有条目。
		 *
		 */
		pubinfo = findPublicationByOid(pnpubid);
		if (pubinfo == NULL)
			continue;
		nspinfo = findNamespaceByOid(pnnspid);
		if (nspinfo == NULL)
			continue;

		/*
		 * 我们总是导出发布命名空间，除非相应的命名空间被排除在导出之外。
		 *
		 */
		if (nspinfo->dobj.dump == DUMP_COMPONENT_NONE)
			continue;

		/* 好的，为此关系创建 DumpableObject */
		pubsinfo[j].dobj.objType = DO_PUBLICATION_TABLE_IN_SCHEMA;
		pubsinfo[j].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubsinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubsinfo[j].dobj);
		pubsinfo[j].dobj.namespace = nspinfo->dobj.namespace;
		pubsinfo[j].dobj.name = nspinfo->dobj.name;
		pubsinfo[j].publication = pubinfo;
		pubsinfo[j].pubschema = nspinfo;

		/* 决定是否要导出它 */
		selectDumpablePublicationObject(&(pubsinfo[j].dobj), fout);

		j++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * getPublicationTables
 *   获取可导出表的发布成员资格信息。
 */
void
getPublicationTables(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query;
	PGresult   *res;
	PublicationRelInfo *pubrinfo;
	DumpOptions *dopt = fout->dopt;
	int			i_tableoid;
	int			i_oid;
	int			i_prpubid;
	int			i_prrelid;
	int			i_prrelqual;
	int			i_prattrs;
	int			i,
				j,
				ntups;

	if (dopt->no_publications || fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	/* 收集所有发布成员资格信息。 */
	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 "SELECT tableoid, oid, prpubid, prrelid, "
							 "pg_catalog.pg_get_expr(prqual, prrelid) AS prrelqual, "
							 "(CASE\n"
							 "  WHEN pr.prattrs IS NOT NULL THEN\n"
							 "    (SELECT array_agg(attname)\n"
							 "       FROM\n"
							 "         pg_catalog.generate_series(0, pg_catalog.array_upper(pr.prattrs::pg_catalog.int2[], 1)) s,\n"
							 "         pg_catalog.pg_attribute\n"
							 "      WHERE attrelid = pr.prrelid AND attnum = prattrs[s])\n"
							 "  ELSE NULL END) prattrs "
							 "FROM pg_catalog.pg_publication_rel pr");
	else
		appendPQExpBufferStr(query,
							 "SELECT tableoid, oid, prpubid, prrelid, "
							 "NULL AS prrelqual, NULL AS prattrs "
							 "FROM pg_catalog.pg_publication_rel");
	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_prpubid = PQfnumber(res, "prpubid");
	i_prrelid = PQfnumber(res, "prrelid");
	i_prrelqual = PQfnumber(res, "prrelqual");
	i_prattrs = PQfnumber(res, "prattrs");

	/* 此分配可能超过所需 */
	pubrinfo = pg_malloc(ntups * sizeof(PublicationRelInfo));
	j = 0;

	for (i = 0; i < ntups; i++)
	{
		Oid			prpubid = atooid(PQgetvalue(res, i, i_prpubid));
		Oid			prrelid = atooid(PQgetvalue(res, i, i_prrelid));
		PublicationInfo *pubinfo;
		TableInfo  *tbinfo;

		/*
		 * 忽略我们不关心的发布或引用表的所有条目。
		 *
		 */
		pubinfo = findPublicationByOid(prpubid);
		if (pubinfo == NULL)
			continue;
		tbinfo = findTableByOid(prrelid);
		if (tbinfo == NULL)
			continue;

		/*
		 * 忽略定义不导出的表的发布成员资格。
		 *
		 */
		if (!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		/* 好的，为此关系创建 DumpableObject */
		pubrinfo[j].dobj.objType = DO_PUBLICATION_REL;
		pubrinfo[j].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		pubrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&pubrinfo[j].dobj);
		pubrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
		pubrinfo[j].dobj.name = tbinfo->dobj.name;
		pubrinfo[j].publication = pubinfo;
		pubrinfo[j].pubtable = tbinfo;
		if (PQgetisnull(res, i, i_prrelqual))
			pubrinfo[j].pubrelqual = NULL;
		else
			pubrinfo[j].pubrelqual = pg_strdup(PQgetvalue(res, i, i_prrelqual));

		if (!PQgetisnull(res, i, i_prattrs))
		{
			char	  **attnames;
			int			nattnames;
			PQExpBuffer attribs;

			if (!parsePGArray(PQgetvalue(res, i, i_prattrs),
							  &attnames, &nattnames))
				pg_fatal("could not parse %s array", "prattrs");
			attribs = createPQExpBuffer();
			for (int k = 0; k < nattnames; k++)
			{
				if (k > 0)
					appendPQExpBufferStr(attribs, ", ");

				appendPQExpBufferStr(attribs, fmtId(attnames[k]));
			}
			pubrinfo[j].pubrattrs = attribs->data;
		}
		else
			pubrinfo[j].pubrattrs = NULL;

		/* 决定是否要导出它 */
		selectDumpablePublicationObject(&(pubrinfo[j].dobj), fout);

		j++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpPublicationNamespace
 *   导出给定发布模式映射的定义。
 */
static void
dumpPublicationNamespace(Archive *fout, const PublicationSchemaInfo *pubsinfo)
{
	DumpOptions *dopt = fout->dopt;
	NamespaceInfo *schemainfo = pubsinfo->pubschema;
	PublicationInfo *pubinfo = pubsinfo->publication;
	PQExpBuffer query;
	char	   *tag;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	tag = psprintf("%s %s", pubinfo->dobj.name, schemainfo->dobj.name);

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "ALTER PUBLICATION %s ", fmtId(pubinfo->dobj.name));
	appendPQExpBuffer(query, "ADD TABLES IN SCHEMA %s;\n", fmtId(schemainfo->dobj.name));

	/*
	 * 创建删除查询毫无意义，因为删除是通过模式删除完成的。
	 *
	 */
	if (pubsinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, pubsinfo->dobj.catId, pubsinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = schemainfo->dobj.name,
								  .owner = pubinfo->rolname,
								  .description = "PUBLICATION TABLES IN SCHEMA",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data));

	/* 这些对象目前不能有注释或安全标签 */

	free(tag);
	destroyPQExpBuffer(query);
}

/*
 * dumpPublicationTable
 *   导出给定发布表映射的定义
 */
static void
dumpPublicationTable(Archive *fout, const PublicationRelInfo *pubrinfo)
{
	DumpOptions *dopt = fout->dopt;
	PublicationInfo *pubinfo = pubrinfo->publication;
	TableInfo  *tbinfo = pubrinfo->pubtable;
	PQExpBuffer query;
	char	   *tag;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	tag = psprintf("%s %s", pubinfo->dobj.name, tbinfo->dobj.name);

	query = createPQExpBuffer();

	appendPQExpBuffer(query, "ALTER PUBLICATION %s ADD TABLE ONLY",
					  fmtId(pubinfo->dobj.name));
	appendPQExpBuffer(query, " %s",
					  fmtQualifiedDumpable(tbinfo));

	if (pubrinfo->pubrattrs)
		appendPQExpBuffer(query, " (%s)", pubrinfo->pubrattrs);

	if (pubrinfo->pubrelqual)
	{
		/*
		 * 需要在表达式周围添加括号，因为 pg_get_expr 不会为顶层表达式提供括号。
		 *
		 *
		 */
		appendPQExpBuffer(query, " WHERE (%s)", pubrinfo->pubrelqual);
	}
	appendPQExpBufferStr(query, ";\n");

	/*
	 * 创建删除查询毫无意义，因为删除是通过表删除完成的。
	 * （如果你想实现它，请确保在发布或其订阅者被删除时处理级联情况。）
	 *
	 *
	 *
	 */
	if (pubrinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, pubrinfo->dobj.catId, pubrinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = pubinfo->rolname,
								  .description = "PUBLICATION TABLE",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data));

	/* 这些对象目前不能有注释或安全标签 */

	free(tag);
	destroyPQExpBuffer(query);
}

/*
 * 当前连接的用户是超级用户吗？
 */
static bool
is_superuser(Archive *fout)
{
	ArchiveHandle *AH = (ArchiveHandle *) fout;
	const char *val;

	val = PQparameterStatus(AH->connection, "is_superuser");

	if (val && strcmp(val, "on") == 0)
		return true;

	return false;
}

/*
 * 将 restrict_nonsystem_relation_kind 设置为给定的值。
 * 由于它是 SUSET，只有超级用户可以无错误地更改它。
 * 
 */
static void
set_restrict_relation_kind(Archive *AH, const char *value)
{
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;

	appendPQExpBuffer(query,
					  "SELECT set_config(name, '%s', false) "
					  "FROM pg_settings "
					  "WHERE name = 'restrict_nonsystem_relation_kind'",
					  value);
	res = ExecuteSqlQuery(AH, query->data, PGRES_TUPLES_OK);

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * getSubscriptions
 *   获取有关订阅的信息
 */
void
getSubscriptions(Archive *fout)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PGresult   *res;
	SubscriptionInfo *subinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_subname;
	int			i_subowner;
	int			i_subbinary;
	int			i_substream;
	int			i_subtwophasestate;
	int			i_subdisableonerr;
	int			i_subpasswordrequired;
	int			i_subrunasowner;
	int			i_subconninfo;
	int			i_subslotname;
	int			i_subsynccommit;
	int			i_subpublications;
	int			i_suborigin;
	int			i,
				ntups;

	if (dopt->no_subscriptions || fout->remoteVersion < 100000)
		return;

	if (!is_superuser(fout))
	{
		int			n;

		res = ExecuteSqlQuery(fout,
							  "SELECT count(*) FROM pg_subscription "
							  "WHERE subdbid = (SELECT oid FROM pg_database"
							  "                 WHERE datname = current_database())",
							  PGRES_TUPLES_OK);
		n = atoi(PQgetvalue(res, 0, 0));
		if (n > 0)
			pg_log_warning("subscriptions not dumped because current user is not a superuser");
		PQclear(res);
		return;
	}

	query = createPQExpBuffer();

	/* 获取当前数据库中的订阅。 */
	appendPQExpBufferStr(query,
						 "SELECT s.tableoid, s.oid, s.subname,\n"
						 " s.subowner,\n"
						 " s.subconninfo, s.subslotname, s.subsynccommit,\n"
						 " s.subpublications,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(query, " s.subbinary,\n");
	else
		appendPQExpBufferStr(query, " false AS subbinary,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(query, " s.substream,\n");
	else
		appendPQExpBufferStr(query, " 'f' AS substream,\n");

	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 " s.subtwophasestate,\n"
							 " s.subdisableonerr,\n");
	else
		appendPQExpBuffer(query,
						  " '%c' AS subtwophasestate,\n"
						  " false AS subdisableonerr,\n",
						  LOGICALREP_TWOPHASE_STATE_DISABLED);

	if (fout->remoteVersion >= 160000)
		appendPQExpBufferStr(query,
							 " s.subpasswordrequired,\n"
							 " s.subrunasowner,\n"
							 " s.suborigin\n");
	else
		appendPQExpBuffer(query,
						  " 't' AS subpasswordrequired,\n"
						  " 't' AS subrunasowner,\n"
						  " '%s' AS suborigin\n",
						  LOGICALREP_ORIGIN_ANY);

	appendPQExpBufferStr(query,
						 "FROM pg_subscription s\n"
						 "WHERE s.subdbid = (SELECT oid FROM pg_database\n"
						 "                   WHERE datname = current_database())");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	/*
	 * 获取订阅字段。我们不在导出中包含 subskiplsn，
	 * 因为在恢复订阅后 LSN 将无效。改为由用户手动设置它。
	 */
	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_subname = PQfnumber(res, "subname");
	i_subowner = PQfnumber(res, "subowner");
	i_subbinary = PQfnumber(res, "subbinary");
	i_substream = PQfnumber(res, "substream");
	i_subtwophasestate = PQfnumber(res, "subtwophasestate");
	i_subdisableonerr = PQfnumber(res, "subdisableonerr");
	i_subpasswordrequired = PQfnumber(res, "subpasswordrequired");
	i_subrunasowner = PQfnumber(res, "subrunasowner");
	i_subconninfo = PQfnumber(res, "subconninfo");
	i_subslotname = PQfnumber(res, "subslotname");
	i_subsynccommit = PQfnumber(res, "subsynccommit");
	i_subpublications = PQfnumber(res, "subpublications");
	i_suborigin = PQfnumber(res, "suborigin");

	subinfo = pg_malloc(ntups * sizeof(SubscriptionInfo));

	for (i = 0; i < ntups; i++)
	{
		subinfo[i].dobj.objType = DO_SUBSCRIPTION;
		subinfo[i].dobj.catId.tableoid =
			atooid(PQgetvalue(res, i, i_tableoid));
		subinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&subinfo[i].dobj);
		subinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_subname));
		subinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_subowner));

		subinfo[i].subbinary =
			pg_strdup(PQgetvalue(res, i, i_subbinary));
		subinfo[i].substream =
			pg_strdup(PQgetvalue(res, i, i_substream));
		subinfo[i].subtwophasestate =
			pg_strdup(PQgetvalue(res, i, i_subtwophasestate));
		subinfo[i].subdisableonerr =
			pg_strdup(PQgetvalue(res, i, i_subdisableonerr));
		subinfo[i].subpasswordrequired =
			pg_strdup(PQgetvalue(res, i, i_subpasswordrequired));
		subinfo[i].subrunasowner =
			pg_strdup(PQgetvalue(res, i, i_subrunasowner));
		subinfo[i].subconninfo =
			pg_strdup(PQgetvalue(res, i, i_subconninfo));
		if (PQgetisnull(res, i, i_subslotname))
			subinfo[i].subslotname = NULL;
		else
			subinfo[i].subslotname =
				pg_strdup(PQgetvalue(res, i, i_subslotname));
		subinfo[i].subsynccommit =
			pg_strdup(PQgetvalue(res, i, i_subsynccommit));
		subinfo[i].subpublications =
			pg_strdup(PQgetvalue(res, i, i_subpublications));
		subinfo[i].suborigin = pg_strdup(PQgetvalue(res, i, i_suborigin));

		/* 决定是否要导出它 */
		selectDumpableObject(&(subinfo[i].dobj), fout);
	}
	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * dumpSubscription
 *   导出给定订阅的定义
 */
static void
dumpSubscription(Archive *fout, const SubscriptionInfo *subinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer delq;
	PQExpBuffer query;
	PQExpBuffer publications;
	char	   *qsubname;
	char	  **pubnames = NULL;
	int			npubnames = 0;
	int			i;
	char		two_phase_disabled[] = {LOGICALREP_TWOPHASE_STATE_DISABLED, '\0'};

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qsubname = pg_strdup(fmtId(subinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP SUBSCRIPTION %s;\n",
					  qsubname);

	appendPQExpBuffer(query, "CREATE SUBSCRIPTION %s CONNECTION ",
					  qsubname);
	appendStringLiteralAH(query, subinfo->subconninfo, fout);

	/* 构建带引号的发布列表并将其附加到查询中。 */
	if (!parsePGArray(subinfo->subpublications, &pubnames, &npubnames))
		pg_fatal("could not parse %s array", "subpublications");

	publications = createPQExpBuffer();
	for (i = 0; i < npubnames; i++)
	{
		if (i > 0)
			appendPQExpBufferStr(publications, ", ");

		appendPQExpBufferStr(publications, fmtId(pubnames[i]));
	}

	appendPQExpBuffer(query, " PUBLICATION %s WITH (connect = false, slot_name = ", publications->data);
	if (subinfo->subslotname)
		appendStringLiteralAH(query, subinfo->subslotname, fout);
	else
		appendPQExpBufferStr(query, "NONE");

	if (strcmp(subinfo->subbinary, "t") == 0)
		appendPQExpBufferStr(query, ", binary = true");

	if (strcmp(subinfo->substream, "t") == 0)
		appendPQExpBufferStr(query, ", streaming = on");
	else if (strcmp(subinfo->substream, "p") == 0)
		appendPQExpBufferStr(query, ", streaming = parallel");

	if (strcmp(subinfo->subtwophasestate, two_phase_disabled) != 0)
		appendPQExpBufferStr(query, ", two_phase = on");

	if (strcmp(subinfo->subdisableonerr, "t") == 0)
		appendPQExpBufferStr(query, ", disable_on_error = true");

	if (strcmp(subinfo->subpasswordrequired, "t") != 0)
		appendPQExpBuffer(query, ", password_required = false");

	if (strcmp(subinfo->subrunasowner, "t") == 0)
		appendPQExpBufferStr(query, ", run_as_owner = true");

	if (strcmp(subinfo->subsynccommit, "off") != 0)
		appendPQExpBuffer(query, ", synchronous_commit = %s", fmtId(subinfo->subsynccommit));

	if (pg_strcasecmp(subinfo->suborigin, LOGICALREP_ORIGIN_ANY) != 0)
		appendPQExpBuffer(query, ", origin = %s", subinfo->suborigin);

	appendPQExpBufferStr(query, ");\n");

	if (subinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, subinfo->dobj.catId, subinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = subinfo->dobj.name,
								  .owner = subinfo->rolname,
								  .description = "SUBSCRIPTION",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delq->data));

	if (subinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SUBSCRIPTION", qsubname,
					NULL, subinfo->rolname,
					subinfo->dobj.catId, 0, subinfo->dobj.dumpId);

	if (subinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SUBSCRIPTION", qsubname,
					 NULL, subinfo->rolname,
					 subinfo->dobj.catId, 0, subinfo->dobj.dumpId);

	destroyPQExpBuffer(publications);
	free(pubnames);

	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qsubname);
}

/*
 * 给定"创建查询"，追加对象所需的多个 ALTER ... DEPENDS ON EXTENSION。
 *
 */
static void
append_depends_on_extension(Archive *fout,
							PQExpBuffer create,
							const DumpableObject *dobj,
							const char *catalog,
							const char *keyword,
							const char *objname)
{
	if (dobj->depends_on_ext)
	{
		char	   *nm;
		PGresult   *res;
		PQExpBuffer query;
		int			ntups;
		int			i_extname;
		int			i;

		/* 避免 fmtId() 不可重入 */
		nm = pg_strdup(objname);

		query = createPQExpBuffer();
		appendPQExpBuffer(query,
						  "SELECT e.extname "
						  "FROM pg_catalog.pg_depend d, pg_catalog.pg_extension e "
						  "WHERE d.refobjid = e.oid AND classid = '%s'::pg_catalog.regclass "
						  "AND objid = '%u'::pg_catalog.oid AND deptype = 'x' "
						  "AND refclassid = 'pg_catalog.pg_extension'::pg_catalog.regclass",
						  catalog,
						  dobj->catId.oid);
		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
		ntups = PQntuples(res);
		i_extname = PQfnumber(res, "extname");
		for (i = 0; i < ntups; i++)
		{
			appendPQExpBuffer(create, "ALTER %s %s DEPENDS ON EXTENSION %s;\n",
							  keyword, nm,
							  fmtId(PQgetvalue(res, i, i_extname)));
		}

		PQclear(res);
		destroyPQExpBuffer(query);
		pg_free(nm);
	}
}

static Oid
get_next_possible_free_pg_type_oid(Archive *fout, PQExpBuffer upgrade_query)
{
	/*
	 * 如果旧版本未分配数组类型，但新版本分配了，
	 * 我们必须选择一个未使用的 OID 来分配。
	 * 
	 * 数组类型的 OID 由其基本类型确定，因此我们必须确保
	 * 我们选择的基本类型的 OID 是自由的。
	 * 我们还需要确保我们为数组类型选择的 OID 是自由的。
	 */
	static Oid	next_possible_free_oid = FirstNormalObjectId;
	PGresult   *res;
	bool		is_dup;

	do
	{
		++next_possible_free_oid;
		printfPQExpBuffer(upgrade_query,
						  "SELECT EXISTS(SELECT 1 "
						  "FROM pg_catalog.pg_type "
						  "WHERE oid = '%u'::pg_catalog.oid);",
						  next_possible_free_oid);
		res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);
		is_dup = (PQgetvalue(res, 0, 0)[0] == 't');
		PQclear(res);
	} while (is_dup);

	return next_possible_free_oid;
}

static void
binary_upgrade_set_type_oids_by_type_oid(Archive *fout,
										 PQExpBuffer upgrade_buffer,
										 Oid pg_type_oid,
										 bool force_array_type,
										 bool include_multirange_type)
{
	PQExpBuffer upgrade_query = createPQExpBuffer();
	PGresult   *res;
	Oid			pg_type_array_oid;
	Oid			pg_type_multirange_oid;
	Oid			pg_type_multirange_array_oid;

	appendPQExpBufferStr(upgrade_buffer, "\n-- For binary upgrade, must preserve pg_type oid\n");
	appendPQExpBuffer(upgrade_buffer,
					  "SELECT pg_catalog.binary_upgrade_set_next_pg_type_oid('%u'::pg_catalog.oid);\n\n",
					  pg_type_oid);

	appendPQExpBuffer(upgrade_query,
					  "SELECT typarray "
					  "FROM pg_catalog.pg_type "
					  "WHERE oid = '%u'::pg_catalog.oid;",
					  pg_type_oid);

	res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

	pg_type_array_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typarray")));

	PQclear(res);

	if (!OidIsValid(pg_type_array_oid) && force_array_type)
		pg_type_array_oid = get_next_possible_free_pg_type_oid(fout, upgrade_query);

	if (OidIsValid(pg_type_array_oid))
	{
		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve pg_type array oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_array_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_array_oid);
	}

	/*
	 * 预设 multirange 类型 OID 及其自身的数组类型 OID。
	 */
	if (include_multirange_type)
	{
		if (fout->remoteVersion >= 140000)
		{
			printfPQExpBuffer(upgrade_query,
							  "SELECT t.oid, t.typarray "
							  "FROM pg_catalog.pg_type t "
							  "JOIN pg_catalog.pg_range r "
							  "ON t.oid = r.rngmultitypid "
							  "WHERE r.rngtypid = '%u'::pg_catalog.oid;",
							  pg_type_oid);

			res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

			pg_type_multirange_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "oid")));
			pg_type_multirange_array_oid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typarray")));

			PQclear(res);
		}
		else
		{
			pg_type_multirange_oid = get_next_possible_free_pg_type_oid(fout, upgrade_query);
			pg_type_multirange_array_oid = get_next_possible_free_pg_type_oid(fout, upgrade_query);
		}

		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve multirange pg_type oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_multirange_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_multirange_oid);
		appendPQExpBufferStr(upgrade_buffer,
							 "\n-- For binary upgrade, must preserve multirange pg_type array oid\n");
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_multirange_array_pg_type_oid('%u'::pg_catalog.oid);\n\n",
						  pg_type_multirange_array_oid);
	}

	destroyPQExpBuffer(upgrade_query);
}

static void
binary_upgrade_set_type_oids_by_rel(Archive *fout,
									PQExpBuffer upgrade_buffer,
									const TableInfo *tbinfo)
{
	Oid			pg_type_oid = tbinfo->reltype;

	if (OidIsValid(pg_type_oid))
		binary_upgrade_set_type_oids_by_type_oid(fout, upgrade_buffer,
												 pg_type_oid, false, false);
}

static void
binary_upgrade_set_pg_class_oids(Archive *fout,
								 PQExpBuffer upgrade_buffer, Oid pg_class_oid,
								 bool is_index)
{
	PQExpBuffer upgrade_query = createPQExpBuffer();
	PGresult   *upgrade_res;
	RelFileNumber relfilenumber;
	Oid			toast_oid;
	RelFileNumber toast_relfilenumber;
	char		relkind;
	Oid			toast_index_oid;
	RelFileNumber toast_index_relfilenumber;

	/*
	 * 保留表的 OID 和 relfilenumber、表的索引、
	 * 表的 TOAST 表和 TOAST 表的索引。
	 * 还需要为其 TOAST 表保留 OID，
	 * 并且如果它是 ACL 条目的目标，我们可能还需要
	 * 为表的 TOAST 表插入一个 ACL 条目。
	 * 
	 * 但请注意：仅在二进制升级模式下执行此操作。
	 *
	 *
	 */
	appendPQExpBuffer(upgrade_query,
					  "SELECT c.relkind, c.relfilenode, c.reltoastrelid, ct.relfilenode AS toast_relfilenode, i.indexrelid, cti.relfilenode AS toast_index_relfilenode "
					  "FROM pg_catalog.pg_class c LEFT JOIN "
					  "pg_catalog.pg_index i ON (c.reltoastrelid = i.indrelid AND i.indisvalid) "
					  "LEFT JOIN pg_catalog.pg_class ct ON (c.reltoastrelid = ct.oid) "
					  "LEFT JOIN pg_catalog.pg_class AS cti ON (i.indexrelid = cti.oid) "
					  "WHERE c.oid = '%u'::pg_catalog.oid;",
					  pg_class_oid);

	upgrade_res = ExecuteSqlQueryForSingleRow(fout, upgrade_query->data);

	relkind = *PQgetvalue(upgrade_res, 0, PQfnumber(upgrade_res, "relkind"));

	relfilenumber = atooid(PQgetvalue(upgrade_res, 0,
									  PQfnumber(upgrade_res, "relfilenode")));
	toast_oid = atooid(PQgetvalue(upgrade_res, 0,
								  PQfnumber(upgrade_res, "reltoastrelid")));
	toast_relfilenumber = atooid(PQgetvalue(upgrade_res, 0,
											PQfnumber(upgrade_res, "toast_relfilenode")));
	toast_index_oid = atooid(PQgetvalue(upgrade_res, 0,
										PQfnumber(upgrade_res, "indexrelid")));
	toast_index_relfilenumber = atooid(PQgetvalue(upgrade_res, 0,
												  PQfnumber(upgrade_res, "toast_index_relfilenode")));

	appendPQExpBufferStr(upgrade_buffer,
						 "\n-- For binary upgrade, must preserve pg_class oids and relfilenodes\n");

	if (!is_index)
	{
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_heap_pg_class_oid('%u'::pg_catalog.oid);\n",
						  pg_class_oid);

		/*
		 * 并非每个关系都有存储。此外，在 v12 之前的数据库中，
		 * 分区表有 relfilenode，但不应设置它。
		 *
		 */
		if (RelFileNumberIsValid(relfilenumber) && relkind != RELKIND_PARTITIONED_TABLE)
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_heap_relfilenode('%u'::pg_catalog.oid);\n",
							  relfilenumber);

		/*
		 * 在 v12 之前的数据库中，分区表可能被标记为有 TOAST 表，
		 * 但我们不应该为它们生成 TOAST 表创建。
		 */
		if (OidIsValid(toast_oid) &&
			relkind != RELKIND_PARTITIONED_TABLE)
		{
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_toast_pg_class_oid('%u'::pg_catalog.oid);\n",
							  toast_oid);
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_toast_relfilenode('%u'::pg_catalog.oid);\n",
							  toast_relfilenumber);

			/* 每个 TOAST 表都有一个索引 */
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_index_pg_class_oid('%u'::pg_catalog.oid);\n",
							  toast_index_oid);
			appendPQExpBuffer(upgrade_buffer,
							  "SELECT pg_catalog.binary_upgrade_set_next_index_relfilenode('%u'::pg_catalog.oid);\n",
							  toast_index_relfilenumber);
		}
	}
	else
	{
		/* 保留索引的 OID 和 relfilenumber */
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_index_pg_class_oid('%u'::pg_catalog.oid);\n",
						  pg_class_oid);
		appendPQExpBuffer(upgrade_buffer,
						  "SELECT pg_catalog.binary_upgrade_set_next_index_relfilenode('%u'::pg_catalog.oid);\n",
						  relfilenumber);
	}

	PQclear(upgrade_res);

	appendPQExpBufferChar(upgrade_buffer, '\n');

	destroyPQExpBuffer(upgrade_query);
}

/*
 * 如果 DumpableObject 是扩展的成员，
 * 向创建查询中添加适当的 ALTER EXTENSION ADD 命令。
 * 
 * 目前这总是输出。但我们可能希望有一天实现
 * 一些巧妙的过滤。
 */
static void
binary_upgrade_extension_member(PQExpBuffer upgrade_buffer,
								const DumpableObject *dobj,
								const char *objtype,
								const char *objname,
								const char *objnamespace)
{
	DumpableObject *extobj = NULL;
	int			i;

	if (!dobj->ext_member)
		return;

	/*
	 * 查找父扩展。如果我们想向 DumpableObject 添加链接字段，
	 * 可以避免此搜索，但这样做不值得。
	 *
	 *
	 */
	for (i = 0; i < dobj->nDeps; i++)
	{
		extobj = findObjectByDumpId(dobj->dependencies[i]);
		if (extobj && extobj->objType == DO_EXTENSION)
			break;
		extobj = NULL;
	}
	if (extobj == NULL)
		pg_fatal("could not find parent extension for %s %s",
				 objtype, objname);

	appendPQExpBufferStr(upgrade_buffer,
						 "\n-- For binary upgrade, handle extension membership the hard way\n");
	appendPQExpBuffer(upgrade_buffer, "ALTER EXTENSION %s ADD %s ",
					  fmtId(extobj->name),
					  objtype);
	if (objnamespace && *objnamespace)
		appendPQExpBuffer(upgrade_buffer, "%s.", fmtId(objnamespace));
	appendPQExpBuffer(upgrade_buffer, "%s;\n", objname);
}

/*
 * getNamespaces:
 *   读取系统目录中的所有命名空间，并将它们返回到 NamespaceInfo* 结构中
 *
 *
 *
 */
NamespaceInfo *
getNamespaces(Archive *fout, int *numNamespaces)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	NamespaceInfo *nsinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_nspname;
	int			i_nspowner;
	int			i_nspacl;
	int			i_acldefault;

	query = createPQExpBuffer();

	/*
	 * 我们获取所有命名空间，包括系统命名空间，
	 * 以便我们读取的每个对象都可以链接到包含它的命名空间。
	 */
	appendPQExpBufferStr(query, "SELECT n.tableoid, n.oid, n.nspname, "
						 "n.nspowner, "
						 "n.nspacl, "
						 "acldefault('n', n.nspowner) AS acldefault "
						 "FROM pg_namespace n");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	nsinfo = (NamespaceInfo *) pg_malloc(ntups * sizeof(NamespaceInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_nspname = PQfnumber(res, "nspname");
	i_nspowner = PQfnumber(res, "nspowner");
	i_nspacl = PQfnumber(res, "nspacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		const char *nspowner;

		nsinfo[i].dobj.objType = DO_NAMESPACE;
		nsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		nsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&nsinfo[i].dobj);
		nsinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_nspname));
		nsinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_nspacl));
		nsinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		nsinfo[i].dacl.privtype = 0;
		nsinfo[i].dacl.initprivs = NULL;
		nspowner = PQgetvalue(res, i, i_nspowner);
		nsinfo[i].nspowner = atooid(nspowner);
		nsinfo[i].rolname = getRoleName(nspowner);

		/* 决定是否导出此命名空间 */
		selectDumpableNamespace(&nsinfo[i], fout);

		/* 标记命名空间是否有 ACL */
		if (!PQgetisnull(res, i, i_nspacl))
			nsinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

		/*
		 * 我们忽略 public 模式的任何 pg_init_privs.initprivs 条目，
		 * 并假设使用预先确定的默认值。
		 * 这些在 v15 中已更改，因此逻辑取决于服务器版本。
		 * 
		 * 此外，对于每个现有模式，我们检查 initdb 是否设置了任何
		 * initprivs，并将它们与我们期望的值进行比较。
		 * 如果一致，则对象报告其默认 ACL，我们什么都不做。
		 * 如果不一致，则对象报告其非默认 ACL 包含在转储中。
		 * 
		 * 对于 pg_catalog，我们始终包含其 ACL，否则在转储 pg_catalog 的
		 * ACL 时，我们将无法区分已撤销和未撤销的 ACL。
		 *
		 *
		 *
		 */
		if (strcmp(nsinfo[i].dobj.name, "public") == 0)
		{
			PQExpBuffer aclarray = createPQExpBuffer();
			PQExpBuffer aclitem = createPQExpBuffer();

			/* v15 的标准 ACL 是 {owner=UC/owner,=U/owner} */
			appendPQExpBufferChar(aclarray, '{');
			quoteAclUserName(aclitem, nsinfo[i].rolname);
			appendPQExpBufferStr(aclitem, "=UC/");
			quoteAclUserName(aclitem, nsinfo[i].rolname);
			appendPGArray(aclarray, aclitem->data);
			resetPQExpBuffer(aclitem);
			appendPQExpBufferStr(aclitem, "=U/");
			quoteAclUserName(aclitem, nsinfo[i].rolname);
			appendPGArray(aclarray, aclitem->data);
			appendPQExpBufferChar(aclarray, '}');

			nsinfo[i].dacl.privtype = 'i';
			nsinfo[i].dacl.initprivs = pstrdup(aclarray->data);
			nsinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

			destroyPQExpBuffer(aclarray);
			destroyPQExpBuffer(aclitem);
		}
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	*numNamespaces = ntups;

	return nsinfo;
}

/*
 * findNamespace:
 *   给定命名空间 OID，查找 getNamespaces 读取的信息
 */
static NamespaceInfo *
findNamespace(Oid nsoid)
{
	NamespaceInfo *nsinfo;

	nsinfo = findNamespaceByOid(nsoid);
	if (nsinfo == NULL)
		pg_fatal("schema with OID %u does not exist", nsoid);
	return nsinfo;
}

/*
 * getExtensions:
 *   读取系统目录中的所有扩展，并将它们返回到 ExtensionInfo* 结构中
 *
 *
 *
 */
ExtensionInfo *
getExtensions(Archive *fout, int *numExtensions)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ExtensionInfo *extinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_extname;
	int			i_nspname;
	int			i_extrelocatable;
	int			i_extversion;
	int			i_extconfig;
	int			i_extcondition;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT x.tableoid, x.oid, "
						 "x.extname, n.nspname, x.extrelocatable, x.extversion, x.extconfig, x.extcondition "
						 "FROM pg_extension x "
						 "JOIN pg_namespace n ON n.oid = x.extnamespace");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	extinfo = (ExtensionInfo *) pg_malloc(ntups * sizeof(ExtensionInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_extname = PQfnumber(res, "extname");
	i_nspname = PQfnumber(res, "nspname");
	i_extrelocatable = PQfnumber(res, "extrelocatable");
	i_extversion = PQfnumber(res, "extversion");
	i_extconfig = PQfnumber(res, "extconfig");
	i_extcondition = PQfnumber(res, "extcondition");

	for (i = 0; i < ntups; i++)
	{
		extinfo[i].dobj.objType = DO_EXTENSION;
		extinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		extinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&extinfo[i].dobj);
		extinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_extname));
		extinfo[i].namespace = pg_strdup(PQgetvalue(res, i, i_nspname));
		extinfo[i].relocatable = *(PQgetvalue(res, i, i_extrelocatable)) == 't';
		extinfo[i].extversion = pg_strdup(PQgetvalue(res, i, i_extversion));
		extinfo[i].extconfig = pg_strdup(PQgetvalue(res, i, i_extconfig));
		extinfo[i].extcondition = pg_strdup(PQgetvalue(res, i, i_extcondition));

		/* 决定是否要导出它 */
		selectDumpableExtension(&(extinfo[i]), dopt);
	}

	PQclear(res);
	destroyPQExpBuffer(query);

	*numExtensions = ntups;

	return extinfo;
}

/*
 * getTypes:
 *   读取系统目录中的所有类型，并将它们返回到 TypeInfo* 结构中
 * 
 * 注意：对于可以隐式创建的数组类型，如果基本类型被导出，则隐式创建的数组类型也会被导出。
 * 它们不会被显式创建，但我们需要它们以便数组引用的类型能够正确识别别名。
 *
 *
 *
 */
TypeInfo *
getTypes(Archive *fout, int *numTypes)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TypeInfo   *tyinfo;
	ShellTypeInfo *stinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_typname;
	int			i_typnamespace;
	int			i_typacl;
	int			i_acldefault;
	int			i_typowner;
	int			i_typelem;
	int			i_typrelid;
	int			i_typrelkind;
	int			i_typtype;
	int			i_typisdefined;
	int			i_isarray;

	/*
	 * 我们甚至包括内置类型，因为用户定义类型可能将它们用作数组元素。
	 * 我们过滤掉系统定义的类型（在 dump-out 时），但我们仍然需要它们的
	 * 信息以正确排序。
	 * 
	 * 但是，对于关系复合类型（pg_class.reltype），我们只包括那些
	 * 我们没有相关表的类型。
	 * 我们通过检查 typrelid 是否与 pg_class 中的任何 OID 匹配来实现这一点。
	 * 这避免了我们获取两次相同信息的可能性。
	 * 尽管 pg_class 和 pg_type 之间存在一对一的关系，
	 * 但这无法通过简单的连接完成。（为什么？想不出来……）
	 *
	 *
	 *
	 */
	appendPQExpBufferStr(query, "SELECT tableoid, oid, typname, "
						 "typnamespace, typacl, "
						 "acldefault('T', typowner) AS acldefault, "
						 "typowner, "
						 "typelem, typrelid, "
						 "CASE WHEN typrelid = 0 THEN ' '::\"char\" "
						 "ELSE (SELECT relkind FROM pg_class WHERE oid = typrelid) END AS typrelkind, "
						 "typtype, typisdefined, "
						 "typname[0] = '_' AND typelem != 0 AND "
						 "(SELECT typarray FROM pg_type te WHERE oid = pg_type.typelem) = oid AS isarray "
						 "FROM pg_type");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	tyinfo = (TypeInfo *) pg_malloc(ntups * sizeof(TypeInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_typname = PQfnumber(res, "typname");
	i_typnamespace = PQfnumber(res, "typnamespace");
	i_typacl = PQfnumber(res, "typacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_typowner = PQfnumber(res, "typowner");
	i_typelem = PQfnumber(res, "typelem");
	i_typrelid = PQfnumber(res, "typrelid");
	i_typrelkind = PQfnumber(res, "typrelkind");
	i_typtype = PQfnumber(res, "typtype");
	i_typisdefined = PQfnumber(res, "typisdefined");
	i_isarray = PQfnumber(res, "isarray");

	for (i = 0; i < ntups; i++)
	{
		tyinfo[i].dobj.objType = DO_TYPE;
		tyinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tyinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tyinfo[i].dobj);
		tyinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_typname));
		tyinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_typnamespace)));
		tyinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_typacl));
		tyinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		tyinfo[i].dacl.privtype = 0;
		tyinfo[i].dacl.initprivs = NULL;
		tyinfo[i].ftypname = NULL;	/* 稍后可能会填充 */
		tyinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_typowner));
		tyinfo[i].typelem = atooid(PQgetvalue(res, i, i_typelem));
		tyinfo[i].typrelid = atooid(PQgetvalue(res, i, i_typrelid));
		tyinfo[i].typrelkind = *PQgetvalue(res, i, i_typrelkind);
		tyinfo[i].typtype = *PQgetvalue(res, i, i_typtype);
		tyinfo[i].shellType = NULL;

		if (strcmp(PQgetvalue(res, i, i_typisdefined), "t") == 0)
			tyinfo[i].isDefined = true;
		else
			tyinfo[i].isDefined = false;

		if (strcmp(PQgetvalue(res, i, i_isarray), "t") == 0)
			tyinfo[i].isArray = true;
		else
			tyinfo[i].isArray = false;

		if (tyinfo[i].typtype == 'm')
			tyinfo[i].isMultirange = true;
		else
			tyinfo[i].isMultirange = false;

		/* 决定是否要导出它 */
		selectDumpableType(&tyinfo[i], fout);

		/* 标记类型是否有 ACL */
		if (!PQgetisnull(res, i, i_typacl))
			tyinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

		/*
		 * 如果是域，则获取其约束信息（如果有）
		 */
		tyinfo[i].nDomChecks = 0;
		tyinfo[i].domChecks = NULL;
		if ((tyinfo[i].dobj.dump & DUMP_COMPONENT_DEFINITION) &&
			tyinfo[i].typtype == TYPTYPE_DOMAIN)
			getDomainConstraints(fout, &(tyinfo[i]));

		/*
		 * 如果是基本类型，则创建一个表示该类型的 shell 定义的 DumpableObject。
		 * 我们将仅在实际的 I/O 函数和/或规范化分析函数
		 * 不在 initdb 中的情况下导出 shell 类型。
		 * 
		 * 注意：在转储中，shell 类型总是出现在基本类型之前，
		 * 因为它们的排序类别（DO_SHELL_TYPE）低于 DO_TYPE。
		 * 这很重要，因为类型必须先作为 shell 类型存在，
		 * 然后才能被定义为完整类型。
		 */
		if ((tyinfo[i].dobj.dump & DUMP_COMPONENT_DEFINITION) &&
			(tyinfo[i].typtype == TYPTYPE_BASE ||
			 tyinfo[i].typtype == TYPTYPE_RANGE))
		{
			stinfo = (ShellTypeInfo *) pg_malloc(sizeof(ShellTypeInfo));
			stinfo->dobj.objType = DO_SHELL_TYPE;
			stinfo->dobj.catId = nilCatalogId;
			AssignDumpId(&stinfo->dobj);
			stinfo->dobj.name = pg_strdup(tyinfo[i].dobj.name);
			stinfo->dobj.namespace = tyinfo[i].dobj.namespace;
			stinfo->baseType = &(tyinfo[i]);
			tyinfo[i].shellType = stinfo;

			/*
			 * 最初将 shell 类型标记为不导出。
			 * 仅当 I/O 或规范化分析函数不是 initdb 值时才导出它。
			 *
			 */
			stinfo->dobj.dump = DUMP_COMPONENT_NONE;
		}
	}

	*numTypes = ntups;

	PQclear(res);

	destroyPQExpBuffer(query);

	return tyinfo;
}

/*
 * getOperators:
 *   读取系统目录中的所有操作符，并将它们返回到 OprInfo* 结构中
 *
 *
 *
 */
OprInfo *
getOperators(Archive *fout, int *numOprs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	OprInfo    *oprinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_oprname;
	int			i_oprnamespace;
	int			i_oprowner;
	int			i_oprkind;
	int			i_oprleft;
	int			i_oprright;
	int			i_oprcode;

	/*
	 * 查找所有操作符，包括内置操作符；
	 * 我们在导出输出时过滤掉系统定义的操作符。
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, oprname, "
						 "oprnamespace, "
						 "oprowner, "
						 "oprkind, "
						 "oprleft, "
						 "oprright, "
						 "oprcode::oid AS oprcode "
						 "FROM pg_operator");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOprs = ntups;

	oprinfo = (OprInfo *) pg_malloc(ntups * sizeof(OprInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_oprname = PQfnumber(res, "oprname");
	i_oprnamespace = PQfnumber(res, "oprnamespace");
	i_oprowner = PQfnumber(res, "oprowner");
	i_oprkind = PQfnumber(res, "oprkind");
	i_oprleft = PQfnumber(res, "oprleft");
	i_oprright = PQfnumber(res, "oprright");
	i_oprcode = PQfnumber(res, "oprcode");

	for (i = 0; i < ntups; i++)
	{
		oprinfo[i].dobj.objType = DO_OPERATOR;
		oprinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		oprinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&oprinfo[i].dobj);
		oprinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_oprname));
		oprinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_oprnamespace)));
		oprinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_oprowner));
		oprinfo[i].oprkind = (PQgetvalue(res, i, i_oprkind))[0];
		oprinfo[i].oprleft = atooid(PQgetvalue(res, i, i_oprleft));
		oprinfo[i].oprright = atooid(PQgetvalue(res, i, i_oprright));
		oprinfo[i].oprcode = atooid(PQgetvalue(res, i, i_oprcode));

		/* 决定是否要导出它 */
		selectDumpableObject(&(oprinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return oprinfo;
}

/*
 * getCollations:
 *   读取系统目录中的所有排序规则，并将它们返回到 CollInfo* 结构中
 *
 *
 *
 */
CollInfo *
getCollations(Archive *fout, int *numCollations)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	CollInfo   *collinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_collname;
	int			i_collnamespace;
	int			i_collowner;
	int			i_collencoding;

	query = createPQExpBuffer();

	/*
	 * 查找所有排序规则，包括内置排序规则；
	 * 我们在导出输出时过滤掉系统定义的排序规则。
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, collname, "
						 "collnamespace, "
						 "collowner, "
						 "collencoding "
						 "FROM pg_collation");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numCollations = ntups;

	collinfo = (CollInfo *) pg_malloc(ntups * sizeof(CollInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_collname = PQfnumber(res, "collname");
	i_collnamespace = PQfnumber(res, "collnamespace");
	i_collowner = PQfnumber(res, "collowner");
	i_collencoding = PQfnumber(res, "collencoding");

	for (i = 0; i < ntups; i++)
	{
		collinfo[i].dobj.objType = DO_COLLATION;
		collinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		collinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&collinfo[i].dobj);
		collinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_collname));
		collinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_collnamespace)));
		collinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_collowner));
		collinfo[i].collencoding = atoi(PQgetvalue(res, i, i_collencoding));

		/* 决定是否要导出它 */
		selectDumpableObject(&(collinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return collinfo;
}

/*
 * getConversions:
 *   读取系统目录中的所有转换，并将它们返回到 ConvInfo* 结构中
 *
 *
 *
 */
ConvInfo *
getConversions(Archive *fout, int *numConversions)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ConvInfo   *convinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_conname;
	int			i_connamespace;
	int			i_conowner;

	query = createPQExpBuffer();

	/*
	 * 查找所有转换，包括内置转换；
	 * 我们在导出输出时过滤掉系统定义的转换。
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, conname, "
						 "connamespace, "
						 "conowner "
						 "FROM pg_conversion");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numConversions = ntups;

	convinfo = (ConvInfo *) pg_malloc(ntups * sizeof(ConvInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_connamespace = PQfnumber(res, "connamespace");
	i_conowner = PQfnumber(res, "conowner");

	for (i = 0; i < ntups; i++)
	{
		convinfo[i].dobj.objType = DO_CONVERSION;
		convinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		convinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&convinfo[i].dobj);
		convinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_conname));
		convinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_connamespace)));
		convinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_conowner));

		/* 决定是否要导出它 */
		selectDumpableObject(&(convinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return convinfo;
}

/*
 * getAccessMethods:
 *   读取系统目录中所有用户定义的访问方法，并将它们返回到 AccessMethodInfo* 结构中
 *
 *
 *
 */
AccessMethodInfo *
getAccessMethods(Archive *fout, int *numAccessMethods)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	AccessMethodInfo *aminfo;
	int			i_tableoid;
	int			i_oid;
	int			i_amname;
	int			i_amhandler;
	int			i_amtype;

	query = createPQExpBuffer();

	/*
	 * 从 pg_am 表中选择所有访问方法。v9.6 引入了 CREATE ACCESS METHOD，
	 * 因此早期版本通常只有内置访问方法。v9.6 还更改了访问方法 API，
	 * 用 amhandler 替换了数十个 pg_am 列。即使用户通过
	 * "INSERT INTO pg_am" 创建了访问方法，我们也无法将 v9.6 之前的
	 * pg_am 列转换为 v9.6+ 的 CREATE ACCESS METHOD。因此，
	 * 在 v9.6 之前，读取 pg_am 仅是为了方便 findAccessMethodByOid()
	 * 提供 OID 到名称的映射。
	 *
	 */
	appendPQExpBufferStr(query, "SELECT tableoid, oid, amname, ");
	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "amtype, "
							 "amhandler::pg_catalog.regproc AS amhandler ");
	else
		appendPQExpBufferStr(query,
							 "'i'::pg_catalog.\"char\" AS amtype, "
							 "'-'::pg_catalog.regproc AS amhandler ");
	appendPQExpBufferStr(query, "FROM pg_am");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numAccessMethods = ntups;

	aminfo = (AccessMethodInfo *) pg_malloc(ntups * sizeof(AccessMethodInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_amname = PQfnumber(res, "amname");
	i_amhandler = PQfnumber(res, "amhandler");
	i_amtype = PQfnumber(res, "amtype");

	for (i = 0; i < ntups; i++)
	{
		aminfo[i].dobj.objType = DO_ACCESS_METHOD;
		aminfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		aminfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&aminfo[i].dobj);
		aminfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_amname));
		aminfo[i].dobj.namespace = NULL;
		aminfo[i].amhandler = pg_strdup(PQgetvalue(res, i, i_amhandler));
		aminfo[i].amtype = *(PQgetvalue(res, i, i_amtype));

		/* 决定是否要导出它 */
		selectDumpableAccessMethod(&(aminfo[i]), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return aminfo;
}


/*
 * getOpclasses:
 *   读取系统目录中的所有操作符类，并将它们返回到 OpclassInfo* 结构中
 *
 *
 *
 */
OpclassInfo *
getOpclasses(Archive *fout, int *numOpclasses)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	OpclassInfo *opcinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_opcmethod;
	int			i_opcname;
	int			i_opcnamespace;
	int			i_opcowner;

	/*
	 * 查找所有操作符类，包括内置操作符类；
	 * 我们在导出输出时过滤掉系统定义的操作符类。
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, opcmethod, opcname, "
						 "opcnamespace, "
						 "opcowner "
						 "FROM pg_opclass");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpclasses = ntups;

	opcinfo = (OpclassInfo *) pg_malloc(ntups * sizeof(OpclassInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opcmethod = PQfnumber(res, "opcmethod");
	i_opcname = PQfnumber(res, "opcname");
	i_opcnamespace = PQfnumber(res, "opcnamespace");
	i_opcowner = PQfnumber(res, "opcowner");

	for (i = 0; i < ntups; i++)
	{
		opcinfo[i].dobj.objType = DO_OPCLASS;
		opcinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opcinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opcinfo[i].dobj);
		opcinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_opcname));
		opcinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_opcnamespace)));
		opcinfo[i].opcmethod = atooid(PQgetvalue(res, i, i_opcmethod));
		opcinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_opcowner));

		/* 决定是否要导出它 */
		selectDumpableObject(&(opcinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opcinfo;
}

/*
 * getOpfamilies:
 *   读取系统目录中的所有操作符族，并将它们返回到 OpfamilyInfo* 结构中
 *
 *
 *
 */
OpfamilyInfo *
getOpfamilies(Archive *fout, int *numOpfamilies)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	OpfamilyInfo *opfinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_opfmethod;
	int			i_opfname;
	int			i_opfnamespace;
	int			i_opfowner;

	query = createPQExpBuffer();

	/*
	 * 查找所有操作符族，包括内置操作符族；
	 * 我们在导出输出时过滤掉系统定义的操作符族。
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, opfmethod, opfname, "
						 "opfnamespace, "
						 "opfowner "
						 "FROM pg_opfamily");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numOpfamilies = ntups;

	opfinfo = (OpfamilyInfo *) pg_malloc(ntups * sizeof(OpfamilyInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_opfname = PQfnumber(res, "opfname");
	i_opfmethod = PQfnumber(res, "opfmethod");
	i_opfnamespace = PQfnumber(res, "opfnamespace");
	i_opfowner = PQfnumber(res, "opfowner");

	for (i = 0; i < ntups; i++)
	{
		opfinfo[i].dobj.objType = DO_OPFAMILY;
		opfinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		opfinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&opfinfo[i].dobj);
		opfinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_opfname));
		opfinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_opfnamespace)));
		opfinfo[i].opfmethod = atooid(PQgetvalue(res, i, i_opfmethod));
		opfinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_opfowner));

		/* 决定是否要导出它 */
		selectDumpableObject(&(opfinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return opfinfo;
}

/*
 * getAggregates:
 *   读取系统目录中所有用户定义的聚合，并将它们返回到 AggInfo* 结构中
 *
 *
 *
 */
AggInfo *
getAggregates(Archive *fout, int *numAggs)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	AggInfo    *agginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_aggname;
	int			i_aggnamespace;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_proowner;
	int			i_aggacl;
	int			i_acldefault;

	/*
	 * 查找所有有趣的聚合。有关过滤逻辑的基本原理，
	 * 请参阅 getFuncs() 中的注释。
	 */
	if (fout->remoteVersion >= 90600)
	{
		const char *agg_check;

		agg_check = (fout->remoteVersion >= 110000 ? "p.prokind = 'a'"
					 : "p.proisagg");

		appendPQExpBuffer(query, "SELECT p.tableoid, p.oid, "
						  "p.proname AS aggname, "
						  "p.pronamespace AS aggnamespace, "
						  "p.pronargs, p.proargtypes, "
						  "p.proowner, "
						  "p.proacl AS aggacl, "
						  "acldefault('f', p.proowner) AS acldefault "
						  "FROM pg_proc p "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(p.oid = pip.objoid "
						  "AND pip.classoid = 'pg_proc'::regclass "
						  "AND pip.objsubid = 0) "
						  "WHERE %s AND ("
						  "p.pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog') OR "
						  "p.proacl IS DISTINCT FROM pip.initprivs",
						  agg_check);
		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 " OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');
	}
	else
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, proname AS aggname, "
							 "pronamespace AS aggnamespace, "
							 "pronargs, proargtypes, "
							 "proowner, "
							 "proacl AS aggacl, "
							 "acldefault('f', proowner) AS acldefault "
							 "FROM pg_proc p "
							 "WHERE proisagg AND ("
							 "pronamespace != "
							 "(SELECT oid FROM pg_namespace "
							 "WHERE nspname = 'pg_catalog')");
		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 " OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numAggs = ntups;

	agginfo = (AggInfo *) pg_malloc(ntups * sizeof(AggInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_aggname = PQfnumber(res, "aggname");
	i_aggnamespace = PQfnumber(res, "aggnamespace");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_proowner = PQfnumber(res, "proowner");
	i_aggacl = PQfnumber(res, "aggacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		agginfo[i].aggfn.dobj.objType = DO_AGG;
		agginfo[i].aggfn.dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		agginfo[i].aggfn.dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&agginfo[i].aggfn.dobj);
		agginfo[i].aggfn.dobj.name = pg_strdup(PQgetvalue(res, i, i_aggname));
		agginfo[i].aggfn.dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_aggnamespace)));
		agginfo[i].aggfn.dacl.acl = pg_strdup(PQgetvalue(res, i, i_aggacl));
		agginfo[i].aggfn.dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		agginfo[i].aggfn.dacl.privtype = 0;
		agginfo[i].aggfn.dacl.initprivs = NULL;
		agginfo[i].aggfn.rolname = getRoleName(PQgetvalue(res, i, i_proowner));
		agginfo[i].aggfn.lang = InvalidOid; /* 目前不感兴趣 */
		agginfo[i].aggfn.prorettype = InvalidOid;	/* 未保存 */
		agginfo[i].aggfn.nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (agginfo[i].aggfn.nargs == 0)
			agginfo[i].aggfn.argtypes = NULL;
		else
		{
			agginfo[i].aggfn.argtypes = (Oid *) pg_malloc(agginfo[i].aggfn.nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  agginfo[i].aggfn.argtypes,
						  agginfo[i].aggfn.nargs);
		}
		agginfo[i].aggfn.postponed_def = false; /* 在排序期间可能会设置 */

		/* 决定是否要导出它 */
		selectDumpableObject(&(agginfo[i].aggfn.dobj), fout);

		/* 标记聚合是否有 ACL */
		if (!PQgetisnull(res, i, i_aggacl))
			agginfo[i].aggfn.dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return agginfo;
}

/*
 * getFuncs:
 *   读取系统目录中所有用户定义的函数，并将它们返回到 FuncInfo* 结构中
 *
 *
 *
 */
FuncInfo *
getFuncs(Archive *fout, int *numFuncs)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	FuncInfo   *finfo;
	int			i_tableoid;
	int			i_oid;
	int			i_proname;
	int			i_pronamespace;
	int			i_proowner;
	int			i_prolang;
	int			i_pronargs;
	int			i_proargtypes;
	int			i_prorettype;
	int			i_proacl;
	int			i_acldefault;

	/*
	 * 查找所有有趣的函数。这有点复杂：
	 * 
	 * 1. 始终排除聚合；这些由 getAggregates 处理。
	 * 2. 始终排除转换函数；这些由 getConversions 处理。
	 * 3. 始终排除扩展的成员；getExtensions 仅转储扩展本身。
	 * 4. 排除属于扩展操作符类/操作符族的支持函数；
	 *    这些是扩展的一部分，应单独处理。
	 * 5. 通过检查函数的所有者是否在 pg_authid 中来确认函数未被孤立。
	 * 6. 排除不属于扩展的 SQL 语言函数，
	 *    我们可以通过重新执行其查询将其内联。
	 * 7. 始终包含不属于任何命名空间的函数（因为对象没有命名空间）。
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 *
	 */
	if (fout->remoteVersion >= 90600)
	{
		const char *not_agg_check;

		not_agg_check = (fout->remoteVersion >= 110000 ? "p.prokind <> 'a'"
						 : "NOT p.proisagg");

		appendPQExpBuffer(query,
						  "SELECT p.tableoid, p.oid, p.proname, p.prolang, "
						  "p.pronargs, p.proargtypes, p.prorettype, "
						  "p.proacl, "
						  "acldefault('f', p.proowner) AS acldefault, "
						  "p.pronamespace, "
						  "p.proowner "
						  "FROM pg_proc p "
						  "LEFT JOIN pg_init_privs pip ON "
						  "(p.oid = pip.objoid "
						  "AND pip.classoid = 'pg_proc'::regclass "
						  "AND pip.objsubid = 0) "
						  "WHERE %s"
						  "\n  AND NOT EXISTS (SELECT 1 FROM pg_depend "
						  "WHERE classid = 'pg_proc'::regclass AND "
						  "objid = p.oid AND deptype = 'i')"
						  "\n  AND ("
						  "\n  pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog')"
						  "\n  OR EXISTS (SELECT 1 FROM pg_cast"
						  "\n  WHERE pg_cast.oid > %u "
						  "\n  AND p.oid = pg_cast.castfunc)"
						  "\n  OR EXISTS (SELECT 1 FROM pg_transform"
						  "\n  WHERE pg_transform.oid > %u AND "
						  "\n  (p.oid = pg_transform.trffromsql"
						  "\n  OR p.oid = pg_transform.trftosql))",
						  not_agg_check,
						  g_last_builtin_oid,
						  g_last_builtin_oid);
		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 "\n  OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferStr(query,
							 "\n  OR p.proacl IS DISTINCT FROM pip.initprivs");
		appendPQExpBufferChar(query, ')');
	}
	else
	{
		appendPQExpBuffer(query,
						  "SELECT tableoid, oid, proname, prolang, "
						  "pronargs, proargtypes, prorettype, proacl, "
						  "acldefault('f', proowner) AS acldefault, "
						  "pronamespace, "
						  "proowner "
						  "FROM pg_proc p "
						  "WHERE NOT proisagg"
						  "\n  AND NOT EXISTS (SELECT 1 FROM pg_depend "
						  "WHERE classid = 'pg_proc'::regclass AND "
						  "objid = p.oid AND deptype = 'i')"
						  "\n  AND ("
						  "\n  pronamespace != "
						  "(SELECT oid FROM pg_namespace "
						  "WHERE nspname = 'pg_catalog')"
						  "\n  OR EXISTS (SELECT 1 FROM pg_cast"
						  "\n  WHERE pg_cast.oid > '%u'::oid"
						  "\n  AND p.oid = pg_cast.castfunc)",
						  g_last_builtin_oid);

		if (fout->remoteVersion >= 90500)
			appendPQExpBuffer(query,
							  "\n  OR EXISTS (SELECT 1 FROM pg_transform"
							  "\n  WHERE pg_transform.oid > '%u'::oid"
							  "\n  AND (p.oid = pg_transform.trffromsql"
							  "\n  OR p.oid = pg_transform.trftosql))",
							  g_last_builtin_oid);

		if (dopt->binary_upgrade)
			appendPQExpBufferStr(query,
								 "\n  OR EXISTS(SELECT 1 FROM pg_depend WHERE "
								 "classid = 'pg_proc'::regclass AND "
								 "objid = p.oid AND "
								 "refclassid = 'pg_extension'::regclass AND "
								 "deptype = 'e')");
		appendPQExpBufferChar(query, ')');
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numFuncs = ntups;

	finfo = (FuncInfo *) pg_malloc0(ntups * sizeof(FuncInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_proname = PQfnumber(res, "proname");
	i_pronamespace = PQfnumber(res, "pronamespace");
	i_proowner = PQfnumber(res, "proowner");
	i_prolang = PQfnumber(res, "prolang");
	i_pronargs = PQfnumber(res, "pronargs");
	i_proargtypes = PQfnumber(res, "proargtypes");
	i_prorettype = PQfnumber(res, "prorettype");
	i_proacl = PQfnumber(res, "proacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		finfo[i].dobj.objType = DO_FUNC;
		finfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		finfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&finfo[i].dobj);
		finfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_proname));
		finfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_pronamespace)));
		finfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_proacl));
		finfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		finfo[i].dacl.privtype = 0;
		finfo[i].dacl.initprivs = NULL;
		finfo[i].rolname = getRoleName(PQgetvalue(res, i, i_proowner));
		finfo[i].lang = atooid(PQgetvalue(res, i, i_prolang));
		finfo[i].prorettype = atooid(PQgetvalue(res, i, i_prorettype));
		finfo[i].nargs = atoi(PQgetvalue(res, i, i_pronargs));
		if (finfo[i].nargs == 0)
			finfo[i].argtypes = NULL;
		else
		{
			finfo[i].argtypes = (Oid *) pg_malloc(finfo[i].nargs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, i, i_proargtypes),
						  finfo[i].argtypes, finfo[i].nargs);
		}
		finfo[i].postponed_def = false; /* 在排序期间可能会设置 */

		/* 决定是否要导出它 */
		selectDumpableObject(&(finfo[i].dobj), fout);

		/* 标记函数是否有 ACL */
		if (!PQgetisnull(res, i, i_proacl))
			finfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return finfo;
}

/*
 * getTables
 *   读取系统目录中的所有表（不包括索引），
 *   并将它们作为 TableInfo* 结构的数组返回
 *
 *
 */
TableInfo *
getTables(Archive *fout, int *numTables)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	TableInfo  *tblinfo;
	int			i_reltableoid;
	int			i_reloid;
	int			i_relname;
	int			i_relnamespace;
	int			i_relkind;
	int			i_reltype;
	int			i_relowner;
	int			i_relchecks;
	int			i_relhasindex;
	int			i_relhasrules;
	int			i_relpages;
	int			i_toastpages;
	int			i_owning_tab;
	int			i_owning_col;
	int			i_reltablespace;
	int			i_relhasoids;
	int			i_relhastriggers;
	int			i_relpersistence;
	int			i_relispopulated;
	int			i_relreplident;
	int			i_relrowsec;
	int			i_relforcerowsec;
	int			i_relfrozenxid;
	int			i_toastfrozenxid;
	int			i_toastoid;
	int			i_relminmxid;
	int			i_toastminmxid;
	int			i_reloptions;
	int			i_checkoption;
	int			i_toastreloptions;
	int			i_reloftype;
	int			i_foreignserver;
	int			i_amname;
	int			i_is_identity_sequence;
	int			i_relacl;
	int			i_acldefault;
	int			i_ispartition;

	/*
	 * 查找所有表和类似表的对象。
	 * 
	 * 我们必须在此阶段获取所有表，因为其他读取过程需要
	 * 每个表的完整信息。
	 * 
	 * 我们获取：表（常规和分区）、视图、物化视图、序列、外部表、
	 * 复合类型和 TOAST 表。
	 * 
	 * 请注意，我们获取索引和统计信息，但在另一个查询中，
	 * 因此我们将其排除在 relkind 列表之外。
	 *
	 *
	 *
	 *
	 */

	appendPQExpBufferStr(query,
						 "SELECT c.tableoid, c.oid, c.relname, "
						 "c.relnamespace, c.relkind, c.reltype, "
						 "c.relowner, "
						 "c.relchecks, "
						 "c.relhasindex, c.relhasrules, c.relpages, "
						 "c.relhastriggers, "
						 "c.relpersistence, "
						 "c.reloftype, "
						 "c.relacl, "
						 "acldefault(CASE WHEN c.relkind = " CppAsString2(RELKIND_SEQUENCE)
						 " THEN 's'::\"char\" ELSE 'r'::\"char\" END, c.relowner) AS acldefault, "
						 "CASE WHEN c.relkind = " CppAsString2(RELKIND_FOREIGN_TABLE) " THEN "
						 "(SELECT ftserver FROM pg_catalog.pg_foreign_table WHERE ftrelid = c.oid) "
						 "ELSE 0 END AS foreignserver, "
						 "c.relfrozenxid, tc.relfrozenxid AS tfrozenxid, "
						 "tc.oid AS toid, "
						 "tc.relpages AS toastpages, "
						 "tc.reloptions AS toast_reloptions, "
						 "d.refobjid AS owning_tab, "
						 "d.refobjsubid AS owning_col, "
						 "tsp.spcname AS reltablespace, ");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(query,
							 "false AS relhasoids, ");
	else
		appendPQExpBufferStr(query,
							 "c.relhasoids, ");

	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(query,
							 "c.relispopulated, ");
	else
		appendPQExpBufferStr(query,
							 "'t' as relispopulated, ");

	if (fout->remoteVersion >= 90400)
		appendPQExpBufferStr(query,
							 "c.relreplident, ");
	else
		appendPQExpBufferStr(query,
							 "'d' AS relreplident, ");

	if (fout->remoteVersion >= 90500)
		appendPQExpBufferStr(query,
							 "c.relrowsecurity, c.relforcerowsecurity, ");
	else
		appendPQExpBufferStr(query,
							 "false AS relrowsecurity, "
							 "false AS relforcerowsecurity, ");

	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(query,
							 "c.relminmxid, tc.relminmxid AS tminmxid, ");
	else
		appendPQExpBufferStr(query,
							 "0 AS relminmxid, 0 AS tminmxid, ");

	if (fout->remoteVersion >= 90300)
		appendPQExpBufferStr(query,
							 "array_remove(array_remove(c.reloptions,'check_option=local'),'check_option=cascaded') AS reloptions, "
							 "CASE WHEN 'check_option=local' = ANY (c.reloptions) THEN 'LOCAL'::text "
							 "WHEN 'check_option=cascaded' = ANY (c.reloptions) THEN 'CASCADED'::text ELSE NULL END AS checkoption, ");
	else
		appendPQExpBufferStr(query,
							 "c.reloptions, NULL AS checkoption, ");

	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "am.amname, ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS amname, ");

	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "(d.deptype = 'i') IS TRUE AS is_identity_sequence, ");
	else
		appendPQExpBufferStr(query,
							 "false AS is_identity_sequence, ");

	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query,
							 "c.relispartition AS ispartition ");
	else
		appendPQExpBufferStr(query,
							 "false AS ispartition ");

	/*
	 * 左连接 pg_depend 以获取将序列链接到其所属列的依赖信息（如果有）。
	 *
	 *
	 *
	 */
	appendPQExpBufferStr(query,
						 "\nFROM pg_class c\n"
						 "LEFT JOIN pg_depend d ON "
						 "(c.relkind = " CppAsString2(RELKIND_SEQUENCE) " AND "
						 "d.classid = 'pg_class'::regclass AND d.objid = c.oid AND "
						 "d.objsubid = 0 AND "
						 "d.refclassid = 'pg_class'::regclass AND d.deptype IN ('a', 'i'))\n"
						 "LEFT JOIN pg_tablespace tsp ON (tsp.oid = c.reltablespace)\n");

	/*
	 * 在 9.6 及以上版本中，左连接 pg_am 以获取 amname。
	 */
	if (fout->remoteVersion >= 90600)
		appendPQExpBufferStr(query,
							 "LEFT JOIN pg_am am ON (c.relam = am.oid)\n");

	/*
	 * 我们故意忽略分区表的 TOAST OID；原因是版本 10 和 11
	 * 对分区表的 reltoastrelid 使用了无效值。
	 *
	 */
	appendPQExpBufferStr(query,
						 "LEFT JOIN pg_class tc ON (c.reltoastrelid = tc.oid"
						 " AND tc.relkind = " CppAsString2(RELKIND_TOASTVALUE)
						 " AND c.relkind <> " CppAsString2(RELKIND_PARTITIONED_TABLE) ")\n");

	/*
	 * 限制为有趣的 relkind（特别是，不包含索引）。
	 * 并非所有 relkind 在所有服务器中都可能，
	 * 但查询会忽略缺失的 relkind。
	 * 
	 * 注意：在 v16 及更高版本中，pg_class 可能包含我们不关心的
	 * 其他 relkind，因此最好明确列出我们关心的 relkind。
	 *
	 *
	 */
	appendPQExpBufferStr(query,
						 "WHERE c.relkind IN ("
						 CppAsString2(RELKIND_RELATION) ", "
						 CppAsString2(RELKIND_SEQUENCE) ", "
						 CppAsString2(RELKIND_VIEW) ", "
						 CppAsString2(RELKIND_COMPOSITE_TYPE) ", "
						 CppAsString2(RELKIND_MATVIEW) ", "
						 CppAsString2(RELKIND_FOREIGN_TABLE) ", "
						 CppAsString2(RELKIND_PARTITIONED_TABLE) ")\n"
						 "ORDER BY c.oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numTables = ntups;

	/*
	 * 从结果中提取数据并锁定可导出表。
	 * 我们在其他任何事情之前进行锁定，以最小化死锁的机会
	 * 并避免在尝试锁定一系列表时出现模式更改。
	 *
	 *
	 *
	 *
	 */
	tblinfo = (TableInfo *) pg_malloc0(ntups * sizeof(TableInfo));

	i_reltableoid = PQfnumber(res, "tableoid");
	i_reloid = PQfnumber(res, "oid");
	i_relname = PQfnumber(res, "relname");
	i_relnamespace = PQfnumber(res, "relnamespace");
	i_relkind = PQfnumber(res, "relkind");
	i_reltype = PQfnumber(res, "reltype");
	i_relowner = PQfnumber(res, "relowner");
	i_relchecks = PQfnumber(res, "relchecks");
	i_relhasindex = PQfnumber(res, "relhasindex");
	i_relhasrules = PQfnumber(res, "relhasrules");
	i_relpages = PQfnumber(res, "relpages");
	i_toastpages = PQfnumber(res, "toastpages");
	i_owning_tab = PQfnumber(res, "owning_tab");
	i_owning_col = PQfnumber(res, "owning_col");
	i_reltablespace = PQfnumber(res, "reltablespace");
	i_relhasoids = PQfnumber(res, "relhasoids");
	i_relhastriggers = PQfnumber(res, "relhastriggers");
	i_relpersistence = PQfnumber(res, "relpersistence");
	i_relispopulated = PQfnumber(res, "relispopulated");
	i_relreplident = PQfnumber(res, "relreplident");
	i_relrowsec = PQfnumber(res, "relrowsecurity");
	i_relforcerowsec = PQfnumber(res, "relforcerowsecurity");
	i_relfrozenxid = PQfnumber(res, "relfrozenxid");
	i_toastfrozenxid = PQfnumber(res, "tfrozenxid");
	i_toastoid = PQfnumber(res, "toid");
	i_relminmxid = PQfnumber(res, "relminmxid");
	i_toastminmxid = PQfnumber(res, "tminmxid");
	i_reloptions = PQfnumber(res, "reloptions");
	i_checkoption = PQfnumber(res, "checkoption");
	i_toastreloptions = PQfnumber(res, "toast_reloptions");
	i_reloftype = PQfnumber(res, "reloftype");
	i_foreignserver = PQfnumber(res, "foreignserver");
	i_amname = PQfnumber(res, "amname");
	i_is_identity_sequence = PQfnumber(res, "is_identity_sequence");
	i_relacl = PQfnumber(res, "relacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_ispartition = PQfnumber(res, "ispartition");

	if (dopt->lockWaitTimeout)
	{
		/*
		 * 安排在等待表锁时失败而不是永远等待。
		 * 
		 * 注意：此编码假设初始设置对所有连接
		 * 都有效。可能理想情况下我们不会重复执行此操作。
		 *
		 */
		resetPQExpBuffer(query);
		appendPQExpBufferStr(query, "SET statement_timeout = ");
		appendStringLiteralConn(query, dopt->lockWaitTimeout, GetConnection(fout));
		ExecuteSqlStatement(fout, query->data);
	}

	resetPQExpBuffer(query);

	for (i = 0; i < ntups; i++)
	{
		tblinfo[i].dobj.objType = DO_TABLE;
		tblinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_reltableoid));
		tblinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_reloid));
		AssignDumpId(&tblinfo[i].dobj);
		tblinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_relname));
		tblinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_relnamespace)));
		tblinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_relacl));
		tblinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		tblinfo[i].dacl.privtype = 0;
		tblinfo[i].dacl.initprivs = NULL;
		tblinfo[i].relkind = *(PQgetvalue(res, i, i_relkind));
		tblinfo[i].reltype = atooid(PQgetvalue(res, i, i_reltype));
		tblinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_relowner));
		tblinfo[i].ncheck = atoi(PQgetvalue(res, i, i_relchecks));
		tblinfo[i].hasindex = (strcmp(PQgetvalue(res, i, i_relhasindex), "t") == 0);
		tblinfo[i].hasrules = (strcmp(PQgetvalue(res, i, i_relhasrules), "t") == 0);
		tblinfo[i].relpages = atoi(PQgetvalue(res, i, i_relpages));
		if (PQgetisnull(res, i, i_toastpages))
			tblinfo[i].toastpages = 0;
		else
			tblinfo[i].toastpages = atoi(PQgetvalue(res, i, i_toastpages));
		if (PQgetisnull(res, i, i_owning_tab))
		{
			tblinfo[i].owning_tab = InvalidOid;
			tblinfo[i].owning_col = 0;
		}
		else
		{
			tblinfo[i].owning_tab = atooid(PQgetvalue(res, i, i_owning_tab));
			tblinfo[i].owning_col = atoi(PQgetvalue(res, i, i_owning_col));
		}
		tblinfo[i].reltablespace = pg_strdup(PQgetvalue(res, i, i_reltablespace));
		tblinfo[i].hasoids = (strcmp(PQgetvalue(res, i, i_relhasoids), "t") == 0);
		tblinfo[i].hastriggers = (strcmp(PQgetvalue(res, i, i_relhastriggers), "t") == 0);
		tblinfo[i].relpersistence = *(PQgetvalue(res, i, i_relpersistence));
		tblinfo[i].relispopulated = (strcmp(PQgetvalue(res, i, i_relispopulated), "t") == 0);
		tblinfo[i].relreplident = *(PQgetvalue(res, i, i_relreplident));
		tblinfo[i].rowsec = (strcmp(PQgetvalue(res, i, i_relrowsec), "t") == 0);
		tblinfo[i].forcerowsec = (strcmp(PQgetvalue(res, i, i_relforcerowsec), "t") == 0);
		tblinfo[i].frozenxid = atooid(PQgetvalue(res, i, i_relfrozenxid));
		tblinfo[i].toast_frozenxid = atooid(PQgetvalue(res, i, i_toastfrozenxid));
		tblinfo[i].toast_oid = atooid(PQgetvalue(res, i, i_toastoid));
		tblinfo[i].minmxid = atooid(PQgetvalue(res, i, i_relminmxid));
		tblinfo[i].toast_minmxid = atooid(PQgetvalue(res, i, i_toastminmxid));
		tblinfo[i].reloptions = pg_strdup(PQgetvalue(res, i, i_reloptions));
		if (PQgetisnull(res, i, i_checkoption))
			tblinfo[i].checkoption = NULL;
		else
			tblinfo[i].checkoption = pg_strdup(PQgetvalue(res, i, i_checkoption));
		tblinfo[i].toast_reloptions = pg_strdup(PQgetvalue(res, i, i_toastreloptions));
		tblinfo[i].reloftype = atooid(PQgetvalue(res, i, i_reloftype));
		tblinfo[i].foreign_server = atooid(PQgetvalue(res, i, i_foreignserver));
		if (PQgetisnull(res, i, i_amname))
			tblinfo[i].amname = NULL;
		else
			tblinfo[i].amname = pg_strdup(PQgetvalue(res, i, i_amname));
		tblinfo[i].is_identity_sequence = (strcmp(PQgetvalue(res, i, i_is_identity_sequence), "t") == 0);
		tblinfo[i].ispartition = (strcmp(PQgetvalue(res, i, i_ispartition), "t") == 0);

		/* 其他字段已在上面清零 */

		/*
		 * 决定是否要导出此表。
		 */
		if (tblinfo[i].relkind == RELKIND_COMPOSITE_TYPE)
			tblinfo[i].dobj.dump = DUMP_COMPONENT_NONE;
		else
			selectDumpableTable(&tblinfo[i], fout);

		/*
		 * 现在，如果我们需要导出其定义或数据，
		 * 就将表视为"有趣的"。稍后，当我们扫描索引时，
		 * 我们将标记需要导出的索引。
		 * 
		 * 但要注意：这可能会被扩展成员身份覆盖。
		 * 
		 * 我们还确定表是否包含任何列级 ACL，
		 * 或者是否包含任何生成列。
		 *
		 */
		tblinfo[i].interesting = (tblinfo[i].dobj.dump &
								  (DUMP_COMPONENT_DEFINITION |
								   DUMP_COMPONENT_DATA)) != 0;

		tblinfo[i].dummy_view = false;	/* 在排序期间可能会设置 */
		tblinfo[i].postponed_def = false;	/* 在排序期间可能会设置 */

		/* 表有数据 */
		tblinfo[i].dobj.components |= DUMP_COMPONENT_DATA;

		/* 标记表是否有 ACL */
		if (!PQgetisnull(res, i, i_relacl))
			tblinfo[i].dobj.components |= DUMP_COMPONENT_ACL;
		tblinfo[i].hascolumnACLs = false;	/* 稍后可能会设置 */

		/*
		 * 对目标表加读锁，以确保在我们进行 DDL 提取之前
		 * 它们不会被 DROP 或修改模式。
		 * 请注意，我们不锁定父表（通过 pg_inherits 找到），
		 * 因此我们必须假定在转储期间不会更改子表的任何父表。
		 * 
		 * 注意：我们使用 MySQL 样式的 ORDER BY 限制来确保
		 * 我们不会一次锁定太多表，因为这会消耗大量
		 * 共享内存并获得不可预测的锁定顺序。
		 * 
		 * 同样，在远程服务器上，批处理很有用，因为它减少了
		 * 网络往返次数。
		 *
		 *
		 */
		if ((tblinfo[i].dobj.dump & DUMP_COMPONENTS_REQUIRING_LOCK) &&
			(tblinfo[i].relkind == RELKIND_RELATION ||
			 tblinfo[i].relkind == RELKIND_PARTITIONED_TABLE))
		{
			/*
			 * 表是批量锁定的。从远程服务器转储时，
			 * 这可以节省大量的网络开销。
			 *
			 */
			if (query->len == 0)
				appendPQExpBuffer(query, "LOCK TABLE %s",
								  fmtQualifiedDumpable(&tblinfo[i]));
			else
			{
				appendPQExpBuffer(query, ", %s",
								  fmtQualifiedDumpable(&tblinfo[i]));

				/* 当查询长度达到 100K 时任意结束批处理。 */
				if (query->len >= 100000)
				{
					/* 锁定另一批表。 */
					appendPQExpBufferStr(query, " IN ACCESS SHARE MODE");
					ExecuteSqlStatement(fout, query->data);
					resetPQExpBuffer(query);
				}
			}
		}
	}

	if (query->len != 0)
	{
		/* 锁定最后一批中的表。 */
		appendPQExpBufferStr(query, " IN ACCESS SHARE MODE");
		ExecuteSqlStatement(fout, query->data);
	}

	if (dopt->lockWaitTimeout)
	{
		ExecuteSqlStatement(fout, "SET statement_timeout = 0");
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return tblinfo;
}

/*
 * getOwnedSeqs
 *   识别拥有的序列，如果拥有表被导出，则将其标记为可导出。
 * 
 * 我们过去常常在 getTables 中处理拥有的序列，但将其移出
 * 使分区处理的逻辑更简单，并确保我们不会意外地
 */
void
getOwnedSeqs(Archive *fout, TableInfo tblinfo[], int numTables)
{
	int			i;

	/*
	 * 强制被表列"拥有"的序列在其拥有表被导出时导出。
	 *
	 */
	for (i = 0; i < numTables; i++)
	{
		TableInfo  *seqinfo = &tblinfo[i];
		TableInfo  *owning_tab;

		if (!OidIsValid(seqinfo->owning_tab))
			continue;			/* 不是拥有的序列 */

		owning_tab = findTableByOid(seqinfo->owning_tab);
		if (owning_tab == NULL)
			pg_fatal("failed sanity check, parent table with OID %u of sequence with OID %u not found",
					 seqinfo->owning_tab, seqinfo->dobj.catId.oid);

		/*
		 * 对于标识序列，导出与拥有表的序列完全相同的组件。
		 * 此外，如果拥有表将被导出，则始终导出序列。
		 * 如果没有，则可能仍需要序列的 ACL，
		 * 因此我们根据通常的规则进行决定。
		 * 
		 * 在二进制升级模式下，我们确实需要序列定义
		 * （以设置 pg_sequence 的值）和数据（以调用 pg_catalog.setval）。
		 * 
		 * 不要将序列数据设置为无条件导出；
		 * 如果我们正在导出表数据，则只需要序列数据。
		 *
		 *
		 *
		 *
		 *
		 *
		 *
		 *
		 *
		 *
		 */
		if (seqinfo->is_identity_sequence)
			seqinfo->dobj.dump = owning_tab->dobj.dump;
		else
			seqinfo->dobj.dump |= owning_tab->dobj.dump;

		/* 确保在我们导出它时必要的数椐可用 */
		if (seqinfo->dobj.dump != DUMP_COMPONENT_NONE)
		{
			seqinfo->interesting = true;
			owning_tab->interesting = true;
		}
	}
}

/*
 * getInherits
 *   读取系统目录中的所有继承信息，
 *   并将它们返回到 InhInfo* 结构的数组中
 *
 *
 */
InhInfo *
getInherits(Archive *fout, int *numInherits)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	InhInfo    *inhinfo;

	int			i_inhrelid;
	int			i_inhparent;

	/* 查找所有继承信息 */
	appendPQExpBufferStr(query, "SELECT inhrelid, inhparent FROM pg_inherits");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numInherits = ntups;

	inhinfo = (InhInfo *) pg_malloc(ntups * sizeof(InhInfo));

	i_inhrelid = PQfnumber(res, "inhrelid");
	i_inhparent = PQfnumber(res, "inhparent");

	for (i = 0; i < ntups; i++)
	{
		inhinfo[i].inhrelid = atooid(PQgetvalue(res, i, i_inhrelid));
		inhinfo[i].inhparent = atooid(PQgetvalue(res, i, i_inhparent));
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return inhinfo;
}

/*
 * getPartitioningInfo
 *   获取有关分区的信息
 * 
 * 在大多数情况下，我们仅为我们要转储的表收集分区信息，
 * 以减少要处理的数据量。但是，对于通过分区根加载，
 * 我们需要目标表的所有分区的完整列表。
 * 
 * 因为我们允许用户指定分区层次结构中任何级别的表，
 * 我们需要从 pg_inherits 中读取以获取子分区。
 * 我们不获取完整的层次结构，仅获取最近的子分区。
 *
 *
 *
 *
 *
 */
void
getPartitioningInfo(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups;

	/* 哈希分区在 v11 之前不存在 */
	if (fout->remoteVersion < 110000)
		return;
	/* 如果仅模式导出，则无需费心 */
	if (fout->dopt->schemaOnly)
		return;

	query = createPQExpBuffer();

	/*
	 * 不安全的划分方案正是那些在分区边界中
	 * 出现 hash enum_ops 的方案。这些方案中，
	 * 哈希分区函数的语义可能因平台而异。
	 * 我们无法安全地转储不安全的划分方案。
	 *
	 *
	 */
	appendPQExpBufferStr(query,
						 "SELECT partrelid FROM pg_partitioned_table WHERE\n"
						 "(SELECT c.oid FROM pg_opclass c JOIN pg_am a "
						 "ON c.opcmethod = a.oid\n"
						 "WHERE opcname = 'enum_ops' "
						 "AND opcnamespace = 'pg_catalog'::regnamespace "
						 "AND amname = 'hash') = ANY(partclass)");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	for (int i = 0; i < ntups; i++)
	{
		Oid			tabrelid = atooid(PQgetvalue(res, i, 0));
		TableInfo  *tbinfo;

		tbinfo = findTableByOid(tabrelid);
		if (tbinfo == NULL)
			pg_fatal("failed sanity check, table OID %u appearing in pg_partitioned_table not found",
					 tabrelid);
		tbinfo->unsafe_partitions = true;
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getIndexes
 *   获取可导出表上每个索引的信息
 * 
 * 注意：索引数据不作为此结构的一部分返回；
 * 它在 createDummyIndexObjects() 之后单独获取。
 */
void
getIndexes(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	IndxInfo   *indxinfo;
	int			i_tableoid,
				i_oid,
				i_indrelid,
				i_indexname,
				i_parentidx,
				i_indexdef,
				i_indnkeyatts,
				i_indnatts,
				i_indkey,
				i_indisclustered,
				i_indisreplident,
				i_indnullsnotdistinct,
				i_contype,
				i_conname,
				i_condeferrable,
				i_condeferred,
				i_contableoid,
				i_conoid,
				i_condef,
				i_tablespace,
				i_indreloptions,
				i_indstatcols,
				i_indstatvals;

	/*
	 * 我们希望只对 pg_index 执行一次查询。但是，我们不能尝试选择
	 * 目录的每一行然后在客户端进行排序，因为某些我们需要的服务器端函数
	 * 应用于我们未加锁的表上是不安全的。因此，我们构建一个包含我们关心
	 * （现已加锁）的表的 OID 数组，并使用 WHERE 子句来限制选择哪些行。
	 *
	 *
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (!tbinfo->hasindex)
			continue;

		/*
		 * 我们可以忽略不感兴趣表的索引。
		 */
		if (!tbinfo->interesting)
			continue;

		/* 好的，我们需要此表的信息 */
		if (tbloids->len > 1)	/* 检查是否有多于 '{' 的内容 */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);
	}
	appendPQExpBufferChar(tbloids, '}');

	appendPQExpBufferStr(query,
						 "SELECT t.tableoid, t.oid, i.indrelid, "
						 "t.relname AS indexname, "
						 "pg_catalog.pg_get_indexdef(i.indexrelid) AS indexdef, "
						 "i.indkey, i.indisclustered, "
						 "c.contype, c.conname, "
						 "c.condeferrable, c.condeferred, "
						 "c.tableoid AS contableoid, "
						 "c.oid AS conoid, "
						 "pg_catalog.pg_get_constraintdef(c.oid, false) AS condef, "
						 "(SELECT spcname FROM pg_catalog.pg_tablespace s WHERE s.oid = t.reltablespace) AS tablespace, "
						 "t.reloptions AS indreloptions, ");


	if (fout->remoteVersion >= 90400)
		appendPQExpBufferStr(query,
							 "i.indisreplident, ");
	else
		appendPQExpBufferStr(query,
							 "false AS indisreplident, ");

	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "inh.inhparent AS parentidx, "
							 "i.indnkeyatts AS indnkeyatts, "
							 "i.indnatts AS indnatts, "
							 "(SELECT pg_catalog.array_agg(attnum ORDER BY attnum) "
							 "  FROM pg_catalog.pg_attribute "
							 "  WHERE attrelid = i.indexrelid AND "
							 "    attstattarget >= 0) AS indstatcols, "
							 "(SELECT pg_catalog.array_agg(attstattarget ORDER BY attnum) "
							 "  FROM pg_catalog.pg_attribute "
							 "  WHERE attrelid = i.indexrelid AND "
							 "    attstattarget >= 0) AS indstatvals, ");
	else
		appendPQExpBufferStr(query,
							 "0 AS parentidx, "
							 "i.indnatts AS indnkeyatts, "
							 "i.indnatts AS indnatts, "
							 "'' AS indstatcols, "
							 "'' AS indstatvals, ");

	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 "i.indnullsnotdistinct ");
	else
		appendPQExpBufferStr(query,
							 "false AS indnullsnotdistinct ");

	/*
	 * 看起来凌乱的外连接的目的是找到一个通过内部依赖链接
	 * 与索引关联的约束。如果找到，则创建一个 CONSTRAINT 条目
	 * 链接到 INDEX 条目。我们假设一个索引不会有多个内部依赖。
	 * 
	 * 注意：对 conrelid 的检查是冗余的，但很有用，因为
	 * 该列有索引而 conindid 没有。
	 *
	 */
	if (fout->remoteVersion >= 110000)
	{
		appendPQExpBuffer(query,
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_index i ON (src.tbloid = i.indrelid) "
						  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
						  "JOIN pg_catalog.pg_class t2 ON (t2.oid = i.indrelid) "
						  "LEFT JOIN pg_catalog.pg_constraint c "
						  "ON (i.indrelid = c.conrelid AND "
						  "i.indexrelid = c.conindid AND "
						  "c.contype IN ('p','u','x')) "
						  "LEFT JOIN pg_catalog.pg_inherits inh "
						  "ON (inh.inhrelid = indexrelid) "
						  "WHERE (i.indisvalid OR t2.relkind = 'p') "
						  "AND i.indisready "
						  "ORDER BY i.indrelid, indexname",
						  tbloids->data);
	}
	else
	{
		/*
		 * 对 indisready 的测试在 9.2 中是必要的，
		 * 在更早/更晚的版本中无害
		 */
		appendPQExpBuffer(query,
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_index i ON (src.tbloid = i.indrelid) "
						  "JOIN pg_catalog.pg_class t ON (t.oid = i.indexrelid) "
						  "LEFT JOIN pg_catalog.pg_constraint c "
						  "ON (i.indrelid = c.conrelid AND "
						  "i.indexrelid = c.conindid AND "
						  "c.contype IN ('p','u','x')) "
						  "WHERE i.indisvalid AND i.indisready "
						  "ORDER BY i.indrelid, indexname",
						  tbloids->data);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_indrelid = PQfnumber(res, "indrelid");
	i_indexname = PQfnumber(res, "indexname");
	i_parentidx = PQfnumber(res, "parentidx");
	i_indexdef = PQfnumber(res, "indexdef");
	i_indnkeyatts = PQfnumber(res, "indnkeyatts");
	i_indnatts = PQfnumber(res, "indnatts");
	i_indkey = PQfnumber(res, "indkey");
	i_indisclustered = PQfnumber(res, "indisclustered");
	i_indisreplident = PQfnumber(res, "indisreplident");
	i_indnullsnotdistinct = PQfnumber(res, "indnullsnotdistinct");
	i_contype = PQfnumber(res, "contype");
	i_conname = PQfnumber(res, "conname");
	i_condeferrable = PQfnumber(res, "condeferrable");
	i_condeferred = PQfnumber(res, "condeferred");
	i_contableoid = PQfnumber(res, "contableoid");
	i_conoid = PQfnumber(res, "conoid");
	i_condef = PQfnumber(res, "condef");
	i_tablespace = PQfnumber(res, "tablespace");
	i_indreloptions = PQfnumber(res, "indreloptions");
	i_indstatcols = PQfnumber(res, "indstatcols");
	i_indstatvals = PQfnumber(res, "indstatvals");

	indxinfo = (IndxInfo *) pg_malloc(ntups * sizeof(IndxInfo));

	/*
	 * 外循环对每个表迭代一次，而不是每行一次。
	 * j 的递增由内循环处理。
	 */
	curtblindx = -1;
	for (int j = 0; j < ntups;)
	{
		Oid			indrelid = atooid(PQgetvalue(res, j, i_indrelid));
		TableInfo  *tbinfo = NULL;
		int			numinds;

		/* 计算此表的行数 */
		for (numinds = 1; numinds < ntups - j; numinds++)
			if (atooid(PQgetvalue(res, j + numinds, i_indrelid)) != indrelid)
				break;

		/*
		 * 定位关联的 TableInfo；我们依赖 tblinfo[] 按 OID 排序。
		 *
		 */
		while (++curtblindx < numTables)
		{
			tbinfo = &tblinfo[curtblindx];
			if (tbinfo->dobj.catId.oid == indrelid)
				break;
		}
		if (curtblindx >= numTables)
			pg_fatal("unrecognized table OID %u", indrelid);
		/* 交叉检查我们是否只获取了请求的表 */
		if (!tbinfo->hasindex ||
			!tbinfo->interesting)
			pg_fatal("unexpected index data for table \"%s\"",
					 tbinfo->dobj.name);

		/* 保存此表的数据 */
		tbinfo->indexes = indxinfo + j;
		tbinfo->numIndexes = numinds;

		for (int c = 0; c < numinds; c++, j++)
		{
			char		contype;

			indxinfo[j].dobj.objType = DO_INDEX;
			indxinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			indxinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&indxinfo[j].dobj);
			indxinfo[j].dobj.dump = tbinfo->dobj.dump;
			indxinfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_indexname));
			indxinfo[j].dobj.namespace = tbinfo->dobj.namespace;
			indxinfo[j].indextable = tbinfo;
			indxinfo[j].indexdef = pg_strdup(PQgetvalue(res, j, i_indexdef));
			indxinfo[j].indnkeyattrs = atoi(PQgetvalue(res, j, i_indnkeyatts));
			indxinfo[j].indnattrs = atoi(PQgetvalue(res, j, i_indnatts));
			indxinfo[j].tablespace = pg_strdup(PQgetvalue(res, j, i_tablespace));
			indxinfo[j].indreloptions = pg_strdup(PQgetvalue(res, j, i_indreloptions));
			indxinfo[j].indstatcols = pg_strdup(PQgetvalue(res, j, i_indstatcols));
			indxinfo[j].indstatvals = pg_strdup(PQgetvalue(res, j, i_indstatvals));
			indxinfo[j].indkeys = (Oid *) pg_malloc(indxinfo[j].indnattrs * sizeof(Oid));
			parseOidArray(PQgetvalue(res, j, i_indkey),
						  indxinfo[j].indkeys, indxinfo[j].indnattrs);
			indxinfo[j].indisclustered = (PQgetvalue(res, j, i_indisclustered)[0] == 't');
			indxinfo[j].indisreplident = (PQgetvalue(res, j, i_indisreplident)[0] == 't');
			indxinfo[j].indnullsnotdistinct = (PQgetvalue(res, j, i_indnullsnotdistinct)[0] == 't');
			indxinfo[j].parentidx = atooid(PQgetvalue(res, j, i_parentidx));
			indxinfo[j].partattaches = (SimplePtrList)
			{
				NULL, NULL
			};
			contype = *(PQgetvalue(res, j, i_contype));

			if (contype == 'p' || contype == 'u' || contype == 'x')
			{
				/*
				 * 如果我们找到与索引匹配的约束，为其创建一个条目。
				 *
				 */
				ConstraintInfo *constrinfo;

				constrinfo = (ConstraintInfo *) pg_malloc(sizeof(ConstraintInfo));
				constrinfo->dobj.objType = DO_CONSTRAINT;
				constrinfo->dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
				constrinfo->dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
				AssignDumpId(&constrinfo->dobj);
				constrinfo->dobj.dump = tbinfo->dobj.dump;
				constrinfo->dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
				constrinfo->dobj.namespace = tbinfo->dobj.namespace;
				constrinfo->contable = tbinfo;
				constrinfo->condomain = NULL;
				constrinfo->contype = contype;
				if (contype == 'x')
					constrinfo->condef = pg_strdup(PQgetvalue(res, j, i_condef));
				else
					constrinfo->condef = NULL;
				constrinfo->confrelid = InvalidOid;
				constrinfo->conindex = indxinfo[j].dobj.dumpId;
				constrinfo->condeferrable = *(PQgetvalue(res, j, i_condeferrable)) == 't';
				constrinfo->condeferred = *(PQgetvalue(res, j, i_condeferred)) == 't';
				constrinfo->conislocal = true;
				constrinfo->separate = true;

				indxinfo[j].indexconstraint = constrinfo->dobj.dumpId;
			}
			else
			{
				/* 普通次级索引 */
				indxinfo[j].indexconstraint = 0;
			}
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
}

/*
 * getExtendedStatistics
 *   获取扩展统计对象的信息。
 * 
 * 注意：扩展统计数据不直接返回给调用者，
 * 而是记录到 DumpableObject 表中。
 */
void
getExtendedStatistics(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	StatsExtInfo *statsextinfo;
	int			ntups;
	int			i_tableoid;
	int			i_oid;
	int			i_stxname;
	int			i_stxnamespace;
	int			i_stxowner;
	int			i_stxrelid;
	int			i_stattarget;
	int			i;

	/* 扩展统计在 v10 中新增 */
	if (fout->remoteVersion < 100000)
		return;

	query = createPQExpBuffer();

	if (fout->remoteVersion < 130000)
		appendPQExpBufferStr(query, "SELECT tableoid, oid, stxname, "
							 "stxnamespace, stxowner, stxrelid, (-1) AS stxstattarget "
							 "FROM pg_catalog.pg_statistic_ext");
	else
		appendPQExpBufferStr(query, "SELECT tableoid, oid, stxname, "
							 "stxnamespace, stxowner, stxrelid, stxstattarget "
							 "FROM pg_catalog.pg_statistic_ext");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_stxname = PQfnumber(res, "stxname");
	i_stxnamespace = PQfnumber(res, "stxnamespace");
	i_stxowner = PQfnumber(res, "stxowner");
	i_stxrelid = PQfnumber(res, "stxrelid");
	i_stattarget = PQfnumber(res, "stxstattarget");

	statsextinfo = (StatsExtInfo *) pg_malloc(ntups * sizeof(StatsExtInfo));

	for (i = 0; i < ntups; i++)
	{
		statsextinfo[i].dobj.objType = DO_STATSEXT;
		statsextinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		statsextinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&statsextinfo[i].dobj);
		statsextinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_stxname));
		statsextinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_stxnamespace)));
		statsextinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_stxowner));
		statsextinfo[i].stattable =
			findTableByOid(atooid(PQgetvalue(res, i, i_stxrelid)));
		statsextinfo[i].stattarget = atoi(PQgetvalue(res, i, i_stattarget));

		/* 决定是否要导出它 */
		selectDumpableStatisticsObject(&(statsextinfo[i]), fout);
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * getConstraints
 * 
 * 获取可导出表上约束的信息。
 * 
 * 当前处理外键。
 * 唯一键和主键约束与索引一起处理，
 * 而检查约束在 getTableAttrs() 中处理。
 */
void
getConstraints(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	TableInfo  *tbinfo = NULL;
	ConstraintInfo *constrinfo;
	int			i_contableoid,
				i_conoid,
				i_conrelid,
				i_conname,
				i_confrelid,
				i_conindid,
				i_condef;

	/*
	 * 我们希望只对 pg_constraint 执行一次查询。但是，我们不能尝试选择
	 * 目录的每一行然后在客户端进行排序，因为某些我们需要的服务器端函数
	 * 应用于我们未加锁的表上是不安全的。因此，我们构建一个包含我们关心
	 * （现已加锁）的表的 OID 数组，并使用 WHERE 子句来限制选择哪些行。
	 *
	 *
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tinfo = &tblinfo[i];

		/*
		 * 对于分区表，外键没有触发器，因此无论如何都必须
		 * 包含它们，以防定义了某些外键。
		 */
		if ((!tinfo->hastriggers &&
			 tinfo->relkind != RELKIND_PARTITIONED_TABLE) ||
			!(tinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		/* 好的，我们需要此表的信息 */
		if (tbloids->len > 1)	/* 检查是否有多于 '{' 的内容 */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tinfo->dobj.catId.oid);
	}
	appendPQExpBufferChar(tbloids, '}');

	appendPQExpBufferStr(query,
						 "SELECT c.tableoid, c.oid, "
						 "conrelid, conname, confrelid, ");
	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query, "conindid, ");
	else
		appendPQExpBufferStr(query, "0 AS conindid, ");
	appendPQExpBuffer(query,
					  "pg_catalog.pg_get_constraintdef(c.oid) AS condef\n"
					  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
					  "JOIN pg_catalog.pg_constraint c ON (src.tbloid = c.conrelid)\n"
					  "WHERE contype = 'f' ",
					  tbloids->data);
	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(query,
							 "AND conparentid = 0 ");
	appendPQExpBufferStr(query,
						 "ORDER BY conrelid, conname");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_contableoid = PQfnumber(res, "tableoid");
	i_conoid = PQfnumber(res, "oid");
	i_conrelid = PQfnumber(res, "conrelid");
	i_conname = PQfnumber(res, "conname");
	i_confrelid = PQfnumber(res, "confrelid");
	i_conindid = PQfnumber(res, "conindid");
	i_condef = PQfnumber(res, "condef");

	constrinfo = (ConstraintInfo *) pg_malloc(ntups * sizeof(ConstraintInfo));

	curtblindx = -1;
	for (int j = 0; j < ntups; j++)
	{
		Oid			conrelid = atooid(PQgetvalue(res, j, i_conrelid));
		TableInfo  *reftable;

		/*
		 * 定位关联的 TableInfo；我们依赖 tblinfo[] 按 OID 排序。
		 *
		 */
		if (tbinfo == NULL || tbinfo->dobj.catId.oid != conrelid)
		{
			while (++curtblindx < numTables)
			{
				tbinfo = &tblinfo[curtblindx];
				if (tbinfo->dobj.catId.oid == conrelid)
					break;
			}
			if (curtblindx >= numTables)
				pg_fatal("unrecognized table OID %u", conrelid);
		}

		constrinfo[j].dobj.objType = DO_FK_CONSTRAINT;
		constrinfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_contableoid));
		constrinfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_conoid));
		AssignDumpId(&constrinfo[j].dobj);
		constrinfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
		constrinfo[j].dobj.namespace = tbinfo->dobj.namespace;
		constrinfo[j].contable = tbinfo;
		constrinfo[j].condomain = NULL;
		constrinfo[j].contype = 'f';
		constrinfo[j].condef = pg_strdup(PQgetvalue(res, j, i_condef));
		constrinfo[j].confrelid = atooid(PQgetvalue(res, j, i_confrelid));
		constrinfo[j].conindex = 0;
		constrinfo[j].condeferrable = false;
		constrinfo[j].condeferred = false;
		constrinfo[j].conislocal = true;
		constrinfo[j].separate = true;

		/*
		 * 恢复指向分区表的 FK 要求所有分区索引已事先附加。
		 * 通过使约束依赖于每个索引分区附加对象来确保这一点。
		 *
		 *
		 */
		reftable = findTableByOid(constrinfo[j].confrelid);
		if (reftable && reftable->relkind == RELKIND_PARTITIONED_TABLE)
		{
			Oid			indexOid = atooid(PQgetvalue(res, j, i_conindid));

			if (indexOid != InvalidOid)
			{
				for (int k = 0; k < reftable->numIndexes; k++)
				{
					IndxInfo   *refidx;

					/* 不是我们的索引？ */
					if (reftable->indexes[k].dobj.catId.oid != indexOid)
						continue;

					refidx = &reftable->indexes[k];
					addConstrChildIdxDeps(&constrinfo[j].dobj, refidx);
					break;
				}
			}
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
}

/*
 * addConstrChildIdxDeps
 * 
 * getConstraints 的递归子程序
 * 
 * 给定一个表示外键约束的对象和一个它引用的分区表上的索引，
 * 将约束对象标记为依赖于每个索引分区的 DO_INDEX_ATTACH 对象，
 * 递归地向下钻取到它们的分区（如果有）。这确保在索引完全标记为
 * 有效之前不会恢复 FK。
 *
 */
static void
addConstrChildIdxDeps(DumpableObject *dobj, const IndxInfo *refidx)
{
	SimplePtrListCell *cell;

	Assert(dobj->objType == DO_FK_CONSTRAINT);

	for (cell = refidx->partattaches.head; cell; cell = cell->next)
	{
		IndexAttachInfo *attach = (IndexAttachInfo *) cell->ptr;

		addObjectDependency(dobj, attach->dobj.dumpId);

		if (attach->partitionIdx->partattaches.head != NULL)
			addConstrChildIdxDeps(dobj, attach->partitionIdx);
	}
}

/*
 * getDomainConstraints
 * 
 * 获取域上约束的信息。
 */
static void
getDomainConstraints(Archive *fout, TypeInfo *tyinfo)
{
	int			i;
	ConstraintInfo *constrinfo;
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			i_tableoid,
				i_oid,
				i_conname,
				i_consrc;
	int			ntups;

	if (!fout->is_prepared[PREPQUERY_GETDOMAINCONSTRAINTS])
	{
		/* 设置查询以获取特定于约束的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE getDomainConstraints(pg_catalog.oid) AS\n"
							 "SELECT tableoid, oid, conname, "
							 "pg_catalog.pg_get_constraintdef(oid) AS consrc, "
							 "convalidated "
							 "FROM pg_catalog.pg_constraint "
							 "WHERE contypid = $1 "
							 "ORDER BY conname");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_GETDOMAINCONSTRAINTS] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE getDomainConstraints('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_conname = PQfnumber(res, "conname");
	i_consrc = PQfnumber(res, "consrc");

	constrinfo = (ConstraintInfo *) pg_malloc(ntups * sizeof(ConstraintInfo));

	tyinfo->nDomChecks = ntups;
	tyinfo->domChecks = constrinfo;

	for (i = 0; i < ntups; i++)
	{
		bool		validated = PQgetvalue(res, i, 4)[0] == 't';

		constrinfo[i].dobj.objType = DO_CONSTRAINT;
		constrinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		constrinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&constrinfo[i].dobj);
		constrinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_conname));
		constrinfo[i].dobj.namespace = tyinfo->dobj.namespace;
		constrinfo[i].contable = NULL;
		constrinfo[i].condomain = tyinfo;
		constrinfo[i].contype = 'c';
		constrinfo[i].condef = pg_strdup(PQgetvalue(res, i, i_consrc));
		constrinfo[i].confrelid = InvalidOid;
		constrinfo[i].conindex = 0;
		constrinfo[i].condeferrable = false;
		constrinfo[i].condeferred = false;
		constrinfo[i].conislocal = true;

		constrinfo[i].separate = !validated;

		/*
		 * 使域依赖于约束，确保在约束依赖项
		 * 都 OK 之前不会输出它。如果约束未经验证，
		 * 它无论如何都会在域之后导出，因此这无关紧要。
		 *
		 */
		if (validated)
			addObjectDependency(&tyinfo->dobj,
								constrinfo[i].dobj.dumpId);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * getRules
 *   获取系统中每个规则的基本信息
 * 
 * numRules 设置为读取到的规则数
 */
RuleInfo *
getRules(Archive *fout, int *numRules)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	RuleInfo   *ruleinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_rulename;
	int			i_ruletable;
	int			i_ev_type;
	int			i_is_instead;
	int			i_ev_enabled;

	appendPQExpBufferStr(query, "SELECT "
						 "tableoid, oid, rulename, "
						 "ev_class AS ruletable, ev_type, is_instead, "
						 "ev_enabled "
						 "FROM pg_rewrite "
						 "ORDER BY oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numRules = ntups;

	ruleinfo = (RuleInfo *) pg_malloc(ntups * sizeof(RuleInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_rulename = PQfnumber(res, "rulename");
	i_ruletable = PQfnumber(res, "ruletable");
	i_ev_type = PQfnumber(res, "ev_type");
	i_is_instead = PQfnumber(res, "is_instead");
	i_ev_enabled = PQfnumber(res, "ev_enabled");

	for (i = 0; i < ntups; i++)
	{
		Oid			ruletableoid;

		ruleinfo[i].dobj.objType = DO_RULE;
		ruleinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		ruleinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&ruleinfo[i].dobj);
		ruleinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_rulename));
		ruletableoid = atooid(PQgetvalue(res, i, i_ruletable));
		ruleinfo[i].ruletable = findTableByOid(ruletableoid);
		if (ruleinfo[i].ruletable == NULL)
			pg_fatal("failed sanity check, parent table with OID %u of pg_rewrite entry with OID %u not found",
					 ruletableoid, ruleinfo[i].dobj.catId.oid);
		ruleinfo[i].dobj.namespace = ruleinfo[i].ruletable->dobj.namespace;
		ruleinfo[i].dobj.dump = ruleinfo[i].ruletable->dobj.dump;
		ruleinfo[i].ev_type = *(PQgetvalue(res, i, i_ev_type));
		ruleinfo[i].is_instead = *(PQgetvalue(res, i, i_is_instead)) == 't';
		ruleinfo[i].ev_enabled = *(PQgetvalue(res, i, i_ev_enabled));
		if (ruleinfo[i].ruletable)
		{
			/*
			 * 如果表是视图或物化视图，强制其 ON SELECT 规则
			 * 在视图本身之前排序 —— 这确保规则的任何依赖项
			 * 影响表的定位。其他规则强制出现在其表之后。
			 *
			 *
			 */
			if ((ruleinfo[i].ruletable->relkind == RELKIND_VIEW ||
				 ruleinfo[i].ruletable->relkind == RELKIND_MATVIEW) &&
				ruleinfo[i].ev_type == '1' && ruleinfo[i].is_instead)
			{
				addObjectDependency(&ruleinfo[i].ruletable->dobj,
									ruleinfo[i].dobj.dumpId);
				/* 如果可能，我们会将规则合并到 CREATE VIEW 中 */
				ruleinfo[i].separate = false;
			}
			else
			{
				addObjectDependency(&ruleinfo[i].dobj,
									ruleinfo[i].ruletable->dobj.dumpId);
				ruleinfo[i].separate = true;
			}
		}
		else
			ruleinfo[i].separate = true;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return ruleinfo;
}

/*
 * getTriggers
 *   获取可导出表上每个触发器的信息
 * 
 * 注意：触发器数据不直接返回给调用者，
 * 而是记录到 DumpableObject 表中。
 */
void
getTriggers(Archive *fout, TableInfo tblinfo[], int numTables)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	TriggerInfo *tginfo;
	int			i_tableoid,
				i_oid,
				i_tgrelid,
				i_tgname,
				i_tgfname,
				i_tgtype,
				i_tgnargs,
				i_tgargs,
				i_tgisconstraint,
				i_tgconstrname,
				i_tgconstrrelid,
				i_tgconstrrelname,
				i_tgenabled,
				i_tgispartition,
				i_tgdeferrable,
				i_tginitdeferred,
				i_tgdef;

	/*
	 * 我们希望只对 pg_trigger 执行一次查询。但是，我们不能尝试选择
	 * 目录的每一行然后在客户端进行排序，因为某些我们需要的服务器端函数
	 * 应用于我们未加锁的表上是不安全的。因此，我们构建一个包含我们关心
	 * （现已加锁）的表的 OID 数组，并使用 WHERE 子句来限制选择哪些行。
	 *
	 *
	 */
	appendPQExpBufferChar(tbloids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		if (!tbinfo->hastriggers ||
			!(tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION))
			continue;

		/* 好的，我们需要此表的信息 */
		if (tbloids->len > 1)	/* 检查是否有多于 '{' 的内容 */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);
	}
	appendPQExpBufferChar(tbloids, '}');

	if (fout->remoteVersion >= 150000)
	{
		/*
		 * 注意：不要在 pg_get_triggerdef 中使用 pretty=true。
		 * 由于括号不足，它可能导致 WHEN 子句的导出结果
		 * 不向前兼容。
		 * 
		 * 注意：我们需要查看分区触发器，因为 tgenabled 标志
		 * 可能已从父表更改。
		 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "t.tgfoid::pg_catalog.regproc AS tgfname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, t.tableoid, t.oid, "
						  "t.tgparentid <> 0 AS tgispartition\n"
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "LEFT JOIN pg_catalog.pg_trigger u ON (u.oid = t.tgparentid) "
						  "WHERE ((NOT t.tgisinternal AND t.tgparentid = 0) "
						  "OR t.tgenabled != u.tgenabled) "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}
	else if (fout->remoteVersion >= 130000)
	{
		/*
		 * 注意：不要在 pg_get_triggerdef 中使用 pretty=true。
		 * 由于括号不足，它可能导致 WHEN 子句的导出结果
		 * 不向前兼容。
		 * 
		 * 注意：我们需要在分区中查看 tgisinternal 触发器，
		 * 因为 tgenabled 标志可能已从父表更改。
		 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "t.tgfoid::pg_catalog.regproc AS tgfname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, t.tableoid, t.oid, t.tgisinternal as tgispartition\n"
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "LEFT JOIN pg_catalog.pg_trigger u ON (u.oid = t.tgparentid) "
						  "WHERE (NOT t.tgisinternal OR t.tgenabled != u.tgenabled) "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}
	else if (fout->remoteVersion >= 110000)
	{
		/*
		 * 注意：我们需要查看分区中的 tgisinternal 触发器，
		 * 因为 tgenabled 标志可能已从父表更改。
		 * 版本 11-12 中没有 tgparentid，因此我们必须通过
		 * pg_depend 来匹配它们。
		 * 
		 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "t.tgfoid::pg_catalog.regproc AS tgfname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, t.tableoid, t.oid, t.tgisinternal as tgispartition "
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "LEFT JOIN pg_catalog.pg_depend AS d ON "
						  " d.classid = 'pg_catalog.pg_trigger'::pg_catalog.regclass AND "
						  " d.refclassid = 'pg_catalog.pg_trigger'::pg_catalog.regclass AND "
						  " d.objid = t.oid "
						  "LEFT JOIN pg_catalog.pg_trigger AS pt ON pt.oid = refobjid "
						  "WHERE (NOT t.tgisinternal OR t.tgenabled != pt.tgenabled) "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}
	else
	{
		/* 关于 pg_get_triggerdef 中 pretty=true 的说明同上 */
		appendPQExpBuffer(query,
						  "SELECT t.tgrelid, t.tgname, "
						  "t.tgfoid::pg_catalog.regproc AS tgfname, "
						  "pg_catalog.pg_get_triggerdef(t.oid, false) AS tgdef, "
						  "t.tgenabled, false as tgispartition, "
						  "t.tableoid, t.oid "
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_trigger t ON (src.tbloid = t.tgrelid) "
						  "WHERE NOT tgisinternal "
						  "ORDER BY t.tgrelid, t.tgname",
						  tbloids->data);
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_tgrelid = PQfnumber(res, "tgrelid");
	i_tgname = PQfnumber(res, "tgname");
	i_tgfname = PQfnumber(res, "tgfname");
	i_tgtype = PQfnumber(res, "tgtype");
	i_tgnargs = PQfnumber(res, "tgnargs");
	i_tgargs = PQfnumber(res, "tgargs");
	i_tgisconstraint = PQfnumber(res, "tgisconstraint");
	i_tgconstrname = PQfnumber(res, "tgconstrname");
	i_tgconstrrelid = PQfnumber(res, "tgconstrrelid");
	i_tgconstrrelname = PQfnumber(res, "tgconstrrelname");
	i_tgenabled = PQfnumber(res, "tgenabled");
	i_tgispartition = PQfnumber(res, "tgispartition");
	i_tgdeferrable = PQfnumber(res, "tgdeferrable");
	i_tginitdeferred = PQfnumber(res, "tginitdeferred");
	i_tgdef = PQfnumber(res, "tgdef");

	tginfo = (TriggerInfo *) pg_malloc(ntups * sizeof(TriggerInfo));

	/*
	 * 外循环对每个表迭代一次，而不是每行一次。
	 * j 的递增由内循环处理。
	 */
	curtblindx = -1;
	for (int j = 0; j < ntups;)
	{
		Oid			tgrelid = atooid(PQgetvalue(res, j, i_tgrelid));
		TableInfo  *tbinfo = NULL;
		int			numtrigs;

		/* 计算此表的行数 */
		for (numtrigs = 1; numtrigs < ntups - j; numtrigs++)
			if (atooid(PQgetvalue(res, j + numtrigs, i_tgrelid)) != tgrelid)
				break;

		/*
		 * 定位关联的 TableInfo；我们依赖 tblinfo[] 按 OID 排序。
		 *
		 */
		while (++curtblindx < numTables)
		{
			tbinfo = &tblinfo[curtblindx];
			if (tbinfo->dobj.catId.oid == tgrelid)
				break;
		}
		if (curtblindx >= numTables)
			pg_fatal("unrecognized table OID %u", tgrelid);

		/* 保存此表的数据 */
		tbinfo->triggers = tginfo + j;
		tbinfo->numTriggers = numtrigs;

		for (int c = 0; c < numtrigs; c++, j++)
		{
			tginfo[j].dobj.objType = DO_TRIGGER;
			tginfo[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
			tginfo[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
			AssignDumpId(&tginfo[j].dobj);
			tginfo[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_tgname));
			tginfo[j].dobj.namespace = tbinfo->dobj.namespace;
			tginfo[j].tgtable = tbinfo;
			tginfo[j].tgenabled = *(PQgetvalue(res, j, i_tgenabled));
			tginfo[j].tgispartition = *(PQgetvalue(res, j, i_tgispartition)) == 't';
			if (i_tgdef >= 0)
			{
				tginfo[j].tgdef = pg_strdup(PQgetvalue(res, j, i_tgdef));

				/* 如果有 tgdef，其余字段无效 */
				tginfo[j].tgfname = NULL;
				tginfo[j].tgtype = 0;
				tginfo[j].tgnargs = 0;
				tginfo[j].tgargs = NULL;
				tginfo[j].tgisconstraint = false;
				tginfo[j].tgdeferrable = false;
				tginfo[j].tginitdeferred = false;
				tginfo[j].tgconstrname = NULL;
				tginfo[j].tgconstrrelid = InvalidOid;
				tginfo[j].tgconstrrelname = NULL;
			}
			else
			{
				tginfo[j].tgdef = NULL;

				tginfo[j].tgfname = pg_strdup(PQgetvalue(res, j, i_tgfname));
				tginfo[j].tgtype = atoi(PQgetvalue(res, j, i_tgtype));
				tginfo[j].tgnargs = atoi(PQgetvalue(res, j, i_tgnargs));
				tginfo[j].tgargs = pg_strdup(PQgetvalue(res, j, i_tgargs));
				tginfo[j].tgisconstraint = *(PQgetvalue(res, j, i_tgisconstraint)) == 't';
				tginfo[j].tgdeferrable = *(PQgetvalue(res, j, i_tgdeferrable)) == 't';
				tginfo[j].tginitdeferred = *(PQgetvalue(res, j, i_tginitdeferred)) == 't';

				if (tginfo[j].tgisconstraint)
				{
					tginfo[j].tgconstrname = pg_strdup(PQgetvalue(res, j, i_tgconstrname));
					tginfo[j].tgconstrrelid = atooid(PQgetvalue(res, j, i_tgconstrrelid));
					if (OidIsValid(tginfo[j].tgconstrrelid))
					{
						if (PQgetisnull(res, j, i_tgconstrrelname))
							pg_fatal("query produced null referenced table name for foreign key trigger \"%s\" on table \"%s\" (OID of table: %u)",
									 tginfo[j].dobj.name,
									 tbinfo->dobj.name,
									 tginfo[j].tgconstrrelid);
						tginfo[j].tgconstrrelname = pg_strdup(PQgetvalue(res, j, i_tgconstrrelname));
					}
					else
						tginfo[j].tgconstrrelname = NULL;
				}
				else
				{
					tginfo[j].tgconstrname = NULL;
					tginfo[j].tgconstrrelid = InvalidOid;
					tginfo[j].tgconstrrelname = NULL;
				}
			}
		}
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tbloids);
}

/*
 * getEventTriggers
 *   获取事件触发器的信息
 */
EventTriggerInfo *
getEventTriggers(Archive *fout, int *numEventTriggers)
{
	int			i;
	PQExpBuffer query;
	PGresult   *res;
	EventTriggerInfo *evtinfo;
	int			i_tableoid,
				i_oid,
				i_evtname,
				i_evtevent,
				i_evtowner,
				i_evttags,
				i_evtfname,
				i_evtenabled;
	int			ntups;

	/* 9.3 之前没有事件触发器 */
	if (fout->remoteVersion < 90300)
	{
		*numEventTriggers = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBufferStr(query,
						 "SELECT e.tableoid, e.oid, evtname, evtenabled, "
						 "evtevent, evtowner, "
						 "array_to_string(array("
						 "select quote_literal(x) "
						 " from unnest(evttags) as t(x)), ', ') as evttags, "
						 "e.evtfoid::regproc as evtfname "
						 "FROM pg_event_trigger e "
						 "ORDER BY e.oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numEventTriggers = ntups;

	evtinfo = (EventTriggerInfo *) pg_malloc(ntups * sizeof(EventTriggerInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_evtname = PQfnumber(res, "evtname");
	i_evtevent = PQfnumber(res, "evtevent");
	i_evtowner = PQfnumber(res, "evtowner");
	i_evttags = PQfnumber(res, "evttags");
	i_evtfname = PQfnumber(res, "evtfname");
	i_evtenabled = PQfnumber(res, "evtenabled");

	for (i = 0; i < ntups; i++)
	{
		evtinfo[i].dobj.objType = DO_EVENT_TRIGGER;
		evtinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		evtinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&evtinfo[i].dobj);
		evtinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_evtname));
		evtinfo[i].evtname = pg_strdup(PQgetvalue(res, i, i_evtname));
		evtinfo[i].evtevent = pg_strdup(PQgetvalue(res, i, i_evtevent));
		evtinfo[i].evtowner = getRoleName(PQgetvalue(res, i, i_evtowner));
		evtinfo[i].evttags = pg_strdup(PQgetvalue(res, i, i_evttags));
		evtinfo[i].evtfname = pg_strdup(PQgetvalue(res, i, i_evtfname));
		evtinfo[i].evtenabled = *(PQgetvalue(res, i, i_evtenabled));

		/* 决定是否要导出它 */
		selectDumpableObject(&(evtinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return evtinfo;
}

/*
 * getProcLangs
 *   获取系统中每个过程语言的基本信息
 * 
 * numProcLangs 设置为读取到的语言数
 * 
 * 注意：这必须在 getFuncs() 之后运行，因为我们假设
 * 可以使用 findFuncByOid()。
 */
ProcLangInfo *
getProcLangs(Archive *fout, int *numProcLangs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	ProcLangInfo *planginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_lanname;
	int			i_lanpltrusted;
	int			i_lanplcallfoid;
	int			i_laninline;
	int			i_lanvalidator;
	int			i_lanacl;
	int			i_acldefault;
	int			i_lanowner;

	appendPQExpBufferStr(query, "SELECT tableoid, oid, "
						 "lanname, lanpltrusted, lanplcallfoid, "
						 "laninline, lanvalidator, "
						 "lanacl, "
						 "acldefault('l', lanowner) AS acldefault, "
						 "lanowner "
						 "FROM pg_language "
						 "WHERE lanispl "
						 "ORDER BY oid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numProcLangs = ntups;

	planginfo = (ProcLangInfo *) pg_malloc(ntups * sizeof(ProcLangInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_lanname = PQfnumber(res, "lanname");
	i_lanpltrusted = PQfnumber(res, "lanpltrusted");
	i_lanplcallfoid = PQfnumber(res, "lanplcallfoid");
	i_laninline = PQfnumber(res, "laninline");
	i_lanvalidator = PQfnumber(res, "lanvalidator");
	i_lanacl = PQfnumber(res, "lanacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_lanowner = PQfnumber(res, "lanowner");

	for (i = 0; i < ntups; i++)
	{
		planginfo[i].dobj.objType = DO_PROCLANG;
		planginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		planginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&planginfo[i].dobj);

		planginfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_lanname));
		planginfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_lanacl));
		planginfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		planginfo[i].dacl.privtype = 0;
		planginfo[i].dacl.initprivs = NULL;
		planginfo[i].lanpltrusted = *(PQgetvalue(res, i, i_lanpltrusted)) == 't';
		planginfo[i].lanplcallfoid = atooid(PQgetvalue(res, i, i_lanplcallfoid));
		planginfo[i].laninline = atooid(PQgetvalue(res, i, i_laninline));
		planginfo[i].lanvalidator = atooid(PQgetvalue(res, i, i_lanvalidator));
		planginfo[i].lanowner = getRoleName(PQgetvalue(res, i, i_lanowner));

		/* 决定是否要导出它 */
		selectDumpableProcLang(&(planginfo[i]), fout);

		/* 标记语言是否有 ACL */
		if (!PQgetisnull(res, i, i_lanacl))
			planginfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return planginfo;
}

/*
 * getCasts
 *   获取系统中大多数类型转换的基本信息
 * 
 * numCasts 设置为读取到的类型转换数
 * 
 * 跳过从 range 到其 multirange 的类型转换，
 * 因为我们会自动创建这些。
 */
CastInfo *
getCasts(Archive *fout, int *numCasts)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query = createPQExpBuffer();
	CastInfo   *castinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_castsource;
	int			i_casttarget;
	int			i_castfunc;
	int			i_castcontext;
	int			i_castmethod;

	if (fout->remoteVersion >= 140000)
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, "
							 "castsource, casttarget, castfunc, castcontext, "
							 "castmethod "
							 "FROM pg_cast c "
							 "WHERE NOT EXISTS ( "
							 "SELECT 1 FROM pg_range r "
							 "WHERE c.castsource = r.rngtypid "
							 "AND c.casttarget = r.rngmultitypid "
							 ") "
							 "ORDER BY 3,4");
	}
	else
	{
		appendPQExpBufferStr(query, "SELECT tableoid, oid, "
							 "castsource, casttarget, castfunc, castcontext, "
							 "castmethod "
							 "FROM pg_cast ORDER BY 3,4");
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numCasts = ntups;

	castinfo = (CastInfo *) pg_malloc(ntups * sizeof(CastInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_castsource = PQfnumber(res, "castsource");
	i_casttarget = PQfnumber(res, "casttarget");
	i_castfunc = PQfnumber(res, "castfunc");
	i_castcontext = PQfnumber(res, "castcontext");
	i_castmethod = PQfnumber(res, "castmethod");

	for (i = 0; i < ntups; i++)
	{
		PQExpBufferData namebuf;
		TypeInfo   *sTypeInfo;
		TypeInfo   *tTypeInfo;

		castinfo[i].dobj.objType = DO_CAST;
		castinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		castinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&castinfo[i].dobj);
		castinfo[i].castsource = atooid(PQgetvalue(res, i, i_castsource));
		castinfo[i].casttarget = atooid(PQgetvalue(res, i, i_casttarget));
		castinfo[i].castfunc = atooid(PQgetvalue(res, i, i_castfunc));
		castinfo[i].castcontext = *(PQgetvalue(res, i, i_castcontext));
		castinfo[i].castmethod = *(PQgetvalue(res, i, i_castmethod));

		/*
		 * 尝试将类型转换命名为 typname 的连接。这仅用于
		 * 排序目的。如果找不到任一类型，名称将为空字符串。
		 *
		 */
		initPQExpBuffer(&namebuf);
		sTypeInfo = findTypeByOid(castinfo[i].castsource);
		tTypeInfo = findTypeByOid(castinfo[i].casttarget);
		if (sTypeInfo && tTypeInfo)
			appendPQExpBuffer(&namebuf, "%s %s",
							  sTypeInfo->dobj.name, tTypeInfo->dobj.name);
		castinfo[i].dobj.name = namebuf.data;

		/* 决定是否要导出它 */
		selectDumpableCast(&(castinfo[i]), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return castinfo;
}

static char *
get_language_name(Archive *fout, Oid langid)
{
	PQExpBuffer query;
	PGresult   *res;
	char	   *lanname;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT lanname FROM pg_language WHERE oid = %u", langid);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	lanname = pg_strdup(fmtId(PQgetvalue(res, 0, 0)));
	destroyPQExpBuffer(query);
	PQclear(res);

	return lanname;
}

/*
 * getTransforms
 *   获取系统中每个转换的基本信息
 * 
 * numTransforms 设置为读取到的转换数
 */
TransformInfo *
getTransforms(Archive *fout, int *numTransforms)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TransformInfo *transforminfo;
	int			i_tableoid;
	int			i_oid;
	int			i_trftype;
	int			i_trflang;
	int			i_trffromsql;
	int			i_trftosql;

	/* 转换在 9.5 之前不存在 */
	if (fout->remoteVersion < 90500)
	{
		*numTransforms = 0;
		return NULL;
	}

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, "
						 "trftype, trflang, trffromsql::oid, trftosql::oid "
						 "FROM pg_transform "
						 "ORDER BY 3,4");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	*numTransforms = ntups;

	transforminfo = (TransformInfo *) pg_malloc(ntups * sizeof(TransformInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_trftype = PQfnumber(res, "trftype");
	i_trflang = PQfnumber(res, "trflang");
	i_trffromsql = PQfnumber(res, "trffromsql");
	i_trftosql = PQfnumber(res, "trftosql");

	for (i = 0; i < ntups; i++)
	{
		PQExpBufferData namebuf;
		TypeInfo   *typeInfo;
		char	   *lanname;

		transforminfo[i].dobj.objType = DO_TRANSFORM;
		transforminfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		transforminfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&transforminfo[i].dobj);
		transforminfo[i].trftype = atooid(PQgetvalue(res, i, i_trftype));
		transforminfo[i].trflang = atooid(PQgetvalue(res, i, i_trflang));
		transforminfo[i].trffromsql = atooid(PQgetvalue(res, i, i_trffromsql));
		transforminfo[i].trftosql = atooid(PQgetvalue(res, i, i_trftosql));

		/*
		 * 尝试将转换命名为类型和语言名称的连接。这仅用于
		 * 排序目的。如果找不到任一，名称将为空字符串。
		 *
		 */
		initPQExpBuffer(&namebuf);
		typeInfo = findTypeByOid(transforminfo[i].trftype);
		lanname = get_language_name(fout, transforminfo[i].trflang);
		if (typeInfo && lanname)
			appendPQExpBuffer(&namebuf, "%s %s",
							  typeInfo->dobj.name, lanname);
		transforminfo[i].dobj.name = namebuf.data;
		free(lanname);

		/* 决定是否要导出它 */
		selectDumpableObject(&(transforminfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return transforminfo;
}

/*
 * getTableAttrs -
 *   对于每个感兴趣的表，读取其属性信息
 *   （名称、类型、默认值、CHECK 约束等）
 * 
 *   修改 tblinfo
 */
void
getTableAttrs(Archive *fout, TableInfo *tblinfo, int numTables)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer tbloids = createPQExpBuffer();
	PQExpBuffer checkoids = createPQExpBuffer();
	PGresult   *res;
	int			ntups;
	int			curtblindx;
	int			i_attrelid;
	int			i_attnum;
	int			i_attname;
	int			i_atttypname;
	int			i_attstattarget;
	int			i_attstorage;
	int			i_typstorage;
	int			i_attidentity;
	int			i_attgenerated;
	int			i_attisdropped;
	int			i_attlen;
	int			i_attalign;
	int			i_attislocal;
	int			i_attnotnull;
	int			i_attoptions;
	int			i_attcollation;
	int			i_attcompression;
	int			i_attfdwoptions;
	int			i_attmissingval;
	int			i_atthasdef;

	/*
	 * 我们希望只对 pg_attribute 执行一次查询，然后只对
	 * pg_attrdef（用于 DEFAULT）和 pg_constraint（用于 CHECK 约束）
	 * 各执行一次查询。但是，我们不能尝试选择这些目录的每一行然后
	 * 在客户端进行排序，因为某些我们需要的服务器端函数应用于
	 * 我们未加锁的表上是不安全的。因此，我们构建一个包含我们关心
	 * （现已加锁）的表的 OID 数组，并使用 WHERE 子句来限制选择哪些行。
	 *
	 *
	 */
	appendPQExpBufferChar(tbloids, '{');
	appendPQExpBufferChar(checkoids, '{');
	for (int i = 0; i < numTables; i++)
	{
		TableInfo  *tbinfo = &tblinfo[i];

		/* 不费心为序列收集信息 */
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			continue;

		/* 也不费心处理不感兴趣的表 */
		if (!tbinfo->interesting)
			continue;

		/* 好的，我们需要此表的信息 */
		if (tbloids->len > 1)	/* 检查是否有多于 '{' 的内容 */
			appendPQExpBufferChar(tbloids, ',');
		appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);

		if (tbinfo->ncheck > 0)
		{
			/* 还为具有检查约束的表制作一个列表 */
			if (checkoids->len > 1) /* 检查是否有多于 '{' 的内容 */
				appendPQExpBufferChar(checkoids, ',');
			appendPQExpBuffer(checkoids, "%u", tbinfo->dobj.catId.oid);
		}
	}
	appendPQExpBufferChar(tbloids, '}');
	appendPQExpBufferChar(checkoids, '}');

	/*
	 * 查找所有用户属性及其类型。
	 * 
	 * 由于我们只想为排序规则与其类型默认值不同的属性
	 * 导出 COLLATE 子句，我们在此使用 CASE 来廉价地抑制
	 * 不感兴趣的 attcollation。
	 */
	appendPQExpBufferStr(q,
						 "SELECT\n"
						 "a.attrelid,\n"
						 "a.attnum,\n"
						 "a.attname,\n"
						 "a.attstattarget,\n"
						 "a.attstorage,\n"
						 "t.typstorage,\n"
						 "a.attnotnull,\n"
						 "a.atthasdef,\n"
						 "a.attisdropped,\n"
						 "a.attlen,\n"
						 "a.attalign,\n"
						 "a.attislocal,\n"
						 "pg_catalog.format_type(t.oid, a.atttypmod) AS atttypname,\n"
						 "array_to_string(a.attoptions, ', ') AS attoptions,\n"
						 "CASE WHEN a.attcollation <> t.typcollation "
						 "THEN a.attcollation ELSE 0 END AS attcollation,\n"
						 "pg_catalog.array_to_string(ARRAY("
						 "SELECT pg_catalog.quote_ident(option_name) || "
						 "' ' || pg_catalog.quote_literal(option_value) "
						 "FROM pg_catalog.pg_options_to_table(attfdwoptions) "
						 "ORDER BY option_name"
						 "), E',\n    ') AS attfdwoptions,\n");

	if (fout->remoteVersion >= 140000)
		appendPQExpBufferStr(q,
							 "a.attcompression AS attcompression,\n");
	else
		appendPQExpBufferStr(q,
							 "'' AS attcompression,\n");

	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(q,
							 "a.attidentity,\n");
	else
		appendPQExpBufferStr(q,
							 "'' AS attidentity,\n");

	if (fout->remoteVersion >= 110000)
		appendPQExpBufferStr(q,
							 "CASE WHEN a.atthasmissing AND NOT a.attisdropped "
							 "THEN a.attmissingval ELSE null END AS attmissingval,\n");
	else
		appendPQExpBufferStr(q,
							 "NULL AS attmissingval,\n");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(q,
							 "a.attgenerated\n");
	else
		appendPQExpBufferStr(q,
							 "'' AS attgenerated\n");

	/* 需要对 pg_type 的左连接以避免删除列时失败 ... */
	appendPQExpBuffer(q,
					  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
					  "JOIN pg_catalog.pg_attribute a ON (src.tbloid = a.attrelid) "
					  "LEFT JOIN pg_catalog.pg_type t "
					  "ON (a.atttypid = t.oid)\n"
					  "WHERE a.attnum > 0::pg_catalog.int2\n"
					  "ORDER BY a.attrelid, a.attnum",
					  tbloids->data);

	res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_attrelid = PQfnumber(res, "attrelid");
	i_attnum = PQfnumber(res, "attnum");
	i_attname = PQfnumber(res, "attname");
	i_atttypname = PQfnumber(res, "atttypname");
	i_attstattarget = PQfnumber(res, "attstattarget");
	i_attstorage = PQfnumber(res, "attstorage");
	i_typstorage = PQfnumber(res, "typstorage");
	i_attidentity = PQfnumber(res, "attidentity");
	i_attgenerated = PQfnumber(res, "attgenerated");
	i_attisdropped = PQfnumber(res, "attisdropped");
	i_attlen = PQfnumber(res, "attlen");
	i_attalign = PQfnumber(res, "attalign");
	i_attislocal = PQfnumber(res, "attislocal");
	i_attnotnull = PQfnumber(res, "attnotnull");
	i_attoptions = PQfnumber(res, "attoptions");
	i_attcollation = PQfnumber(res, "attcollation");
	i_attcompression = PQfnumber(res, "attcompression");
	i_attfdwoptions = PQfnumber(res, "attfdwoptions");
	i_attmissingval = PQfnumber(res, "attmissingval");
	i_atthasdef = PQfnumber(res, "atthasdef");

	/* 在下一个循环中，我们将累积具有默认值的表的 OID */
	resetPQExpBuffer(tbloids);
	appendPQExpBufferChar(tbloids, '{');

	/*
	 * 外循环对每个表迭代一次，而不是每行一次。
	 * r 的递增由内循环处理。
	 */
	curtblindx = -1;
	for (int r = 0; r < ntups;)
	{
		Oid			attrelid = atooid(PQgetvalue(res, r, i_attrelid));
		TableInfo  *tbinfo = NULL;
		int			numatts;
		bool		hasdefaults;

		/* 计算此表的行数 */
		for (numatts = 1; numatts < ntups - r; numatts++)
			if (atooid(PQgetvalue(res, r + numatts, i_attrelid)) != attrelid)
				break;

		/*
		 * 定位关联的 TableInfo；我们依赖 tblinfo[] 按 OID 排序。
		 *
		 */
		while (++curtblindx < numTables)
		{
			tbinfo = &tblinfo[curtblindx];
			if (tbinfo->dobj.catId.oid == attrelid)
				break;
		}
		if (curtblindx >= numTables)
			pg_fatal("unrecognized table OID %u", attrelid);
		/* 交叉检查我们是否只获取了请求的表 */
		if (tbinfo->relkind == RELKIND_SEQUENCE ||
			!tbinfo->interesting)
			pg_fatal("unexpected column data for table \"%s\"",
					 tbinfo->dobj.name);

		/* 保存此表的数据 */
		tbinfo->numatts = numatts;
		tbinfo->attnames = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->atttypnames = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->attstattarget = (int *) pg_malloc(numatts * sizeof(int));
		tbinfo->attstorage = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->typstorage = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attidentity = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attgenerated = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attisdropped = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->attlen = (int *) pg_malloc(numatts * sizeof(int));
		tbinfo->attalign = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attislocal = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->attoptions = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->attcollation = (Oid *) pg_malloc(numatts * sizeof(Oid));
		tbinfo->attcompression = (char *) pg_malloc(numatts * sizeof(char));
		tbinfo->attfdwoptions = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->attmissingval = (char **) pg_malloc(numatts * sizeof(char *));
		tbinfo->notnull = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->inhNotNull = (bool *) pg_malloc(numatts * sizeof(bool));
		tbinfo->attrdefs = (AttrDefInfo **) pg_malloc(numatts * sizeof(AttrDefInfo *));
		hasdefaults = false;

		for (int j = 0; j < numatts; j++, r++)
		{
			if (j + 1 != atoi(PQgetvalue(res, r, i_attnum)))
				pg_fatal("invalid column numbering in table \"%s\"",
						 tbinfo->dobj.name);
			tbinfo->attnames[j] = pg_strdup(PQgetvalue(res, r, i_attname));
			tbinfo->atttypnames[j] = pg_strdup(PQgetvalue(res, r, i_atttypname));
			tbinfo->attstattarget[j] = atoi(PQgetvalue(res, r, i_attstattarget));
			tbinfo->attstorage[j] = *(PQgetvalue(res, r, i_attstorage));
			tbinfo->typstorage[j] = *(PQgetvalue(res, r, i_typstorage));
			tbinfo->attidentity[j] = *(PQgetvalue(res, r, i_attidentity));
			tbinfo->attgenerated[j] = *(PQgetvalue(res, r, i_attgenerated));
			tbinfo->needs_override = tbinfo->needs_override || (tbinfo->attidentity[j] == ATTRIBUTE_IDENTITY_ALWAYS);
			tbinfo->attisdropped[j] = (PQgetvalue(res, r, i_attisdropped)[0] == 't');
			tbinfo->attlen[j] = atoi(PQgetvalue(res, r, i_attlen));
			tbinfo->attalign[j] = *(PQgetvalue(res, r, i_attalign));
			tbinfo->attislocal[j] = (PQgetvalue(res, r, i_attislocal)[0] == 't');
			tbinfo->notnull[j] = (PQgetvalue(res, r, i_attnotnull)[0] == 't');
			tbinfo->attoptions[j] = pg_strdup(PQgetvalue(res, r, i_attoptions));
			tbinfo->attcollation[j] = atooid(PQgetvalue(res, r, i_attcollation));
			tbinfo->attcompression[j] = *(PQgetvalue(res, r, i_attcompression));
			tbinfo->attfdwoptions[j] = pg_strdup(PQgetvalue(res, r, i_attfdwoptions));
			tbinfo->attmissingval[j] = pg_strdup(PQgetvalue(res, r, i_attmissingval));
			tbinfo->attrdefs[j] = NULL; /* 下面修正 */
			if (PQgetvalue(res, r, i_atthasdef)[0] == 't')
				hasdefaults = true;
			/* 这些标志将在 flagInhAttrs() 中设置 */
			tbinfo->inhNotNull[j] = false;
		}

		if (hasdefaults)
		{
			/* 收集具有默认值的感兴趣表的 OID */
			if (tbloids->len > 1)	/* 检查是否有多于 '{' 的内容 */
				appendPQExpBufferChar(tbloids, ',');
			appendPQExpBuffer(tbloids, "%u", tbinfo->dobj.catId.oid);
		}
	}

	PQclear(res);

	/*
	 * 现在获取列默认值的信息。仅数据导出时跳过此步骤，
	 * 因为仅表模式需要它。
	 */
	if (!dopt->dataOnly && tbloids->len > 1)
	{
		AttrDefInfo *attrdefs;
		int			numDefaults;
		TableInfo  *tbinfo = NULL;

		pg_log_info("finding table default expressions");

		appendPQExpBufferChar(tbloids, '}');

		printfPQExpBuffer(q, "SELECT a.tableoid, a.oid, adrelid, adnum, "
						  "pg_catalog.pg_get_expr(adbin, adrelid) AS adsrc\n"
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_attrdef a ON (src.tbloid = a.adrelid)\n"
						  "ORDER BY a.adrelid, a.adnum",
						  tbloids->data);

		res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

		numDefaults = PQntuples(res);
		attrdefs = (AttrDefInfo *) pg_malloc(numDefaults * sizeof(AttrDefInfo));

		curtblindx = -1;
		for (int j = 0; j < numDefaults; j++)
		{
			Oid			adtableoid = atooid(PQgetvalue(res, j, 0));
			Oid			adoid = atooid(PQgetvalue(res, j, 1));
			Oid			adrelid = atooid(PQgetvalue(res, j, 2));
			int			adnum = atoi(PQgetvalue(res, j, 3));
			char	   *adsrc = PQgetvalue(res, j, 4);

			/*
			 * 定位关联的 TableInfo；我们依赖 tblinfo[] 按 OID 排序。
			 *
			 */
			if (tbinfo == NULL || tbinfo->dobj.catId.oid != adrelid)
			{
				while (++curtblindx < numTables)
				{
					tbinfo = &tblinfo[curtblindx];
					if (tbinfo->dobj.catId.oid == adrelid)
						break;
				}
				if (curtblindx >= numTables)
					pg_fatal("unrecognized table OID %u", adrelid);
			}

			if (adnum <= 0 || adnum > tbinfo->numatts)
				pg_fatal("invalid adnum value %d for table \"%s\"",
						 adnum, tbinfo->dobj.name);

			/*
			 * 已删除的列不应该有默认值，但为安全起见，忽略它们
			 *
			 */
			if (tbinfo->attisdropped[adnum - 1])
				continue;

			attrdefs[j].dobj.objType = DO_ATTRDEF;
			attrdefs[j].dobj.catId.tableoid = adtableoid;
			attrdefs[j].dobj.catId.oid = adoid;
			AssignDumpId(&attrdefs[j].dobj);
			attrdefs[j].adtable = tbinfo;
			attrdefs[j].adnum = adnum;
			attrdefs[j].adef_expr = pg_strdup(adsrc);

			attrdefs[j].dobj.name = pg_strdup(tbinfo->dobj.name);
			attrdefs[j].dobj.namespace = tbinfo->dobj.namespace;

			attrdefs[j].dobj.dump = tbinfo->dobj.dump;

			/*
			 * 确定默认值/生成表达式是作为主 CREATE TABLE
			 * （或类似）命令的一部分导出，还是作为单独的
			 * ALTER TABLE（或类似）命令导出。优先方案是放入
			 * CREATE 命令中，但在某些情况下这不可行。
			 *
			 */
			if (tbinfo->attgenerated[adnum - 1])
			{
				/*
				 * 列生成表达式不能单独导出，因为没有相应的语法。
				 * 通过在此处将 separate 设置为 false，我们防止
				 * "default" 被作为自己的可导出对象处理。稍后，
				 * flagInhAttrs() 会将其标记为完全不要导出，
				 * 如果可能的话（即如果可以从父表继承）。
				 *
				 */
				attrdefs[j].separate = false;
			}
			else if (tbinfo->relkind == RELKIND_VIEW)
			{
				/*
				 * VIEW 上的默认值必须始终作为单独的 ALTER TABLE
				 * 命令导出。
				 */
				attrdefs[j].separate = true;
			}
			else if (!shouldPrintColumn(dopt, tbinfo, adnum - 1))
			{
				/* 列将被抑制，单独输出默认值 */
				attrdefs[j].separate = true;
			}
			else
			{
				attrdefs[j].separate = false;
			}

			if (!attrdefs[j].separate)
			{
				/*
				 * 将默认值标记为需要出现在表之前，
				 * 以便它的任何依赖项必须在 CREATE TABLE 之前发出。
				 * 如果这不可能，我们将在排序依赖项时切换到
				 * "separate" 模式。
				 */
				addObjectDependency(&tbinfo->dobj,
									attrdefs[j].dobj.dumpId);
			}

			tbinfo->attrdefs[adnum - 1] = &attrdefs[j];
		}

		PQclear(res);
	}

	/*
	 * 获取表 CHECK 约束的信息。仅数据导出时跳过此步骤，
	 * 因为仅表模式需要它。
	 */
	if (!dopt->dataOnly && checkoids->len > 2)
	{
		ConstraintInfo *constrs;
		int			numConstrs;
		int			i_tableoid;
		int			i_oid;
		int			i_conrelid;
		int			i_conname;
		int			i_consrc;
		int			i_conislocal;
		int			i_convalidated;

		pg_log_info("finding table check constraints");

		resetPQExpBuffer(q);
		appendPQExpBuffer(q,
						  "SELECT c.tableoid, c.oid, conrelid, conname, "
						  "pg_catalog.pg_get_constraintdef(c.oid) AS consrc, "
						  "conislocal, convalidated "
						  "FROM unnest('%s'::pg_catalog.oid[]) AS src(tbloid)\n"
						  "JOIN pg_catalog.pg_constraint c ON (src.tbloid = c.conrelid)\n"
						  "WHERE contype = 'c' "
						  "ORDER BY c.conrelid, c.conname",
						  checkoids->data);

		res = ExecuteSqlQuery(fout, q->data, PGRES_TUPLES_OK);

		numConstrs = PQntuples(res);
		constrs = (ConstraintInfo *) pg_malloc(numConstrs * sizeof(ConstraintInfo));

		i_tableoid = PQfnumber(res, "tableoid");
		i_oid = PQfnumber(res, "oid");
		i_conrelid = PQfnumber(res, "conrelid");
		i_conname = PQfnumber(res, "conname");
		i_consrc = PQfnumber(res, "consrc");
		i_conislocal = PQfnumber(res, "conislocal");
		i_convalidated = PQfnumber(res, "convalidated");

		/* 如上所述，此循环对每个表迭代一次，而不是每行一次 */
		curtblindx = -1;
		for (int j = 0; j < numConstrs;)
		{
			Oid			conrelid = atooid(PQgetvalue(res, j, i_conrelid));
			TableInfo  *tbinfo = NULL;
			int			numcons;

			/* 计算此表的行数 */
			for (numcons = 1; numcons < numConstrs - j; numcons++)
				if (atooid(PQgetvalue(res, j + numcons, i_conrelid)) != conrelid)
					break;

			/*
			 * 定位关联的 TableInfo；我们依赖 tblinfo[] 按 OID 排序。
			 *
			 */
			while (++curtblindx < numTables)
			{
				tbinfo = &tblinfo[curtblindx];
				if (tbinfo->dobj.catId.oid == conrelid)
					break;
			}
			if (curtblindx >= numTables)
				pg_fatal("unrecognized table OID %u", conrelid);

			if (numcons != tbinfo->ncheck)
			{
				pg_log_error(ngettext("expected %d check constraint on table \"%s\" but found %d",
									  "expected %d check constraints on table \"%s\" but found %d",
									  tbinfo->ncheck),
							 tbinfo->ncheck, tbinfo->dobj.name, numcons);
				pg_log_error_hint("The system catalogs might be corrupted.");
				exit_nicely(1);
			}

			tbinfo->checkexprs = constrs + j;

			for (int c = 0; c < numcons; c++, j++)
			{
				bool		validated = PQgetvalue(res, j, i_convalidated)[0] == 't';

				constrs[j].dobj.objType = DO_CONSTRAINT;
				constrs[j].dobj.catId.tableoid = atooid(PQgetvalue(res, j, i_tableoid));
				constrs[j].dobj.catId.oid = atooid(PQgetvalue(res, j, i_oid));
				AssignDumpId(&constrs[j].dobj);
				constrs[j].dobj.name = pg_strdup(PQgetvalue(res, j, i_conname));
				constrs[j].dobj.namespace = tbinfo->dobj.namespace;
				constrs[j].contable = tbinfo;
				constrs[j].condomain = NULL;
				constrs[j].contype = 'c';
				constrs[j].condef = pg_strdup(PQgetvalue(res, j, i_consrc));
				constrs[j].confrelid = InvalidOid;
				constrs[j].conindex = 0;
				constrs[j].condeferrable = false;
				constrs[j].condeferred = false;
				constrs[j].conislocal = (PQgetvalue(res, j, i_conislocal)[0] == 't');

				/*
				 * 未经验证的约束需要单独导出，
				 * 以便可能违规的现有数据在约束之前加载。
				 *
				 */
				constrs[j].separate = !validated;

				constrs[j].dobj.dump = tbinfo->dobj.dump;

				/*
				 * 将约束标记为需要出现在表之前 —— 这样约束的任何
				 * 其他依赖项将在我们尝试创建表之前发出。如果约束
				 * 要单独导出，它将在数据加载后导出，因此不需要。
				 * （无论如何都有一个相反方向的自动依赖，
				 * 因此不需要在此手动添加。）
				 *
				 *
				 */
				if (!constrs[j].separate)
					addObjectDependency(&tbinfo->dobj,
										constrs[j].dobj.dumpId);

				/*
				 * 我们稍后将检测约束是否必须从表定义中分离出来。
				 *
				 */
			}
		}

		PQclear(res);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(tbloids);
	destroyPQExpBuffer(checkoids);
}

/*
 * 测试列是否应作为表的 CREATE TABLE 的一部分输出。
 * 列号从零开始。
 * 
 * 通常这始终为 true，但对于已删除的列以及那些没有任何
 * 本地定义的继承列为 false。（如果我们输出这样的列，
 * 它会错误地将 pg_attribute.attislocal 设置为 true。）
 * 对于分区，它始终为 true，因为我们希望分区独立创建
 * 并随后使用 ATTACH PARTITION。
 * 
 * 在二进制升级模式下，我们必须输出所有列并在之后
 * 修正 attislocal/attisdropped 状态，以便控制物理列顺序。
 * 
 * 此函数存在是因为有许多分散且不明显的地方
 * 必须与此决策保持同步。
 *
 */
bool
shouldPrintColumn(const DumpOptions *dopt, const TableInfo *tbinfo, int colno)
{
	if (dopt->binary_upgrade)
		return true;
	if (tbinfo->attisdropped[colno])
		return false;
	return (tbinfo->attislocal[colno] || tbinfo->ispartition);
}


/*
 * getTSParsers:
 *   读取系统目录中的所有文本搜索解析器，并将它们返回到
 *   TSParserInfo* 结构中
 * 
 *   numTSParsers 设置为读取到的解析器数
 */
TSParserInfo *
getTSParsers(Archive *fout, int *numTSParsers)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSParserInfo *prsinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_prsname;
	int			i_prsnamespace;
	int			i_prsstart;
	int			i_prstoken;
	int			i_prsend;
	int			i_prsheadline;
	int			i_prslextype;

	query = createPQExpBuffer();

	/*
	 * 查找所有文本搜索对象，包括内置对象；
	 * 我们在导出时过滤掉系统定义的对象。
	 */

	appendPQExpBufferStr(query, "SELECT tableoid, oid, prsname, prsnamespace, "
						 "prsstart::oid, prstoken::oid, "
						 "prsend::oid, prsheadline::oid, prslextype::oid "
						 "FROM pg_ts_parser");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSParsers = ntups;

	prsinfo = (TSParserInfo *) pg_malloc(ntups * sizeof(TSParserInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_prsname = PQfnumber(res, "prsname");
	i_prsnamespace = PQfnumber(res, "prsnamespace");
	i_prsstart = PQfnumber(res, "prsstart");
	i_prstoken = PQfnumber(res, "prstoken");
	i_prsend = PQfnumber(res, "prsend");
	i_prsheadline = PQfnumber(res, "prsheadline");
	i_prslextype = PQfnumber(res, "prslextype");

	for (i = 0; i < ntups; i++)
	{
		prsinfo[i].dobj.objType = DO_TSPARSER;
		prsinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		prsinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&prsinfo[i].dobj);
		prsinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_prsname));
		prsinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_prsnamespace)));
		prsinfo[i].prsstart = atooid(PQgetvalue(res, i, i_prsstart));
		prsinfo[i].prstoken = atooid(PQgetvalue(res, i, i_prstoken));
		prsinfo[i].prsend = atooid(PQgetvalue(res, i, i_prsend));
		prsinfo[i].prsheadline = atooid(PQgetvalue(res, i, i_prsheadline));
		prsinfo[i].prslextype = atooid(PQgetvalue(res, i, i_prslextype));

		/* 决定是否要导出它 */
		selectDumpableObject(&(prsinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return prsinfo;
}

/*
 * getTSDictionaries:
 *   读取系统目录中的所有文本搜索字典，并将它们返回到
 *   TSDictInfo* 结构中
 * 
 *   numTSDicts 设置为读取到的字典数
 */
TSDictInfo *
getTSDictionaries(Archive *fout, int *numTSDicts)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSDictInfo *dictinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_dictname;
	int			i_dictnamespace;
	int			i_dictowner;
	int			i_dicttemplate;
	int			i_dictinitoption;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, dictname, "
						 "dictnamespace, dictowner, "
						 "dicttemplate, dictinitoption "
						 "FROM pg_ts_dict");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSDicts = ntups;

	dictinfo = (TSDictInfo *) pg_malloc(ntups * sizeof(TSDictInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_dictname = PQfnumber(res, "dictname");
	i_dictnamespace = PQfnumber(res, "dictnamespace");
	i_dictowner = PQfnumber(res, "dictowner");
	i_dictinitoption = PQfnumber(res, "dictinitoption");
	i_dicttemplate = PQfnumber(res, "dicttemplate");

	for (i = 0; i < ntups; i++)
	{
		dictinfo[i].dobj.objType = DO_TSDICT;
		dictinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		dictinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&dictinfo[i].dobj);
		dictinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_dictname));
		dictinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_dictnamespace)));
		dictinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_dictowner));
		dictinfo[i].dicttemplate = atooid(PQgetvalue(res, i, i_dicttemplate));
		if (PQgetisnull(res, i, i_dictinitoption))
			dictinfo[i].dictinitoption = NULL;
		else
			dictinfo[i].dictinitoption = pg_strdup(PQgetvalue(res, i, i_dictinitoption));

		/* 决定是否要导出它 */
		selectDumpableObject(&(dictinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return dictinfo;
}

/*
 * getTSTemplates:
 *   读取系统目录中的所有文本搜索模板，并将它们返回到
 *   TSTemplateInfo* 结构中
 * 
 *   numTSTemplates 设置为读取到的模板数
 */
TSTemplateInfo *
getTSTemplates(Archive *fout, int *numTSTemplates)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSTemplateInfo *tmplinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_tmplname;
	int			i_tmplnamespace;
	int			i_tmplinit;
	int			i_tmpllexize;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, tmplname, "
						 "tmplnamespace, tmplinit::oid, tmpllexize::oid "
						 "FROM pg_ts_template");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSTemplates = ntups;

	tmplinfo = (TSTemplateInfo *) pg_malloc(ntups * sizeof(TSTemplateInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_tmplname = PQfnumber(res, "tmplname");
	i_tmplnamespace = PQfnumber(res, "tmplnamespace");
	i_tmplinit = PQfnumber(res, "tmplinit");
	i_tmpllexize = PQfnumber(res, "tmpllexize");

	for (i = 0; i < ntups; i++)
	{
		tmplinfo[i].dobj.objType = DO_TSTEMPLATE;
		tmplinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		tmplinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&tmplinfo[i].dobj);
		tmplinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_tmplname));
		tmplinfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_tmplnamespace)));
		tmplinfo[i].tmplinit = atooid(PQgetvalue(res, i, i_tmplinit));
		tmplinfo[i].tmpllexize = atooid(PQgetvalue(res, i, i_tmpllexize));

		/* 决定是否要导出它 */
		selectDumpableObject(&(tmplinfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return tmplinfo;
}

/*
 * getTSConfigurations:
 *   读取系统目录中的所有文本搜索配置，并将它们返回到
 *   TSConfigInfo* 结构中
 * 
 *   numTSConfigs 设置为读取到的配置数
 */
TSConfigInfo *
getTSConfigurations(Archive *fout, int *numTSConfigs)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	TSConfigInfo *cfginfo;
	int			i_tableoid;
	int			i_oid;
	int			i_cfgname;
	int			i_cfgnamespace;
	int			i_cfgowner;
	int			i_cfgparser;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, cfgname, "
						 "cfgnamespace, cfgowner, cfgparser "
						 "FROM pg_ts_config");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numTSConfigs = ntups;

	cfginfo = (TSConfigInfo *) pg_malloc(ntups * sizeof(TSConfigInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_cfgname = PQfnumber(res, "cfgname");
	i_cfgnamespace = PQfnumber(res, "cfgnamespace");
	i_cfgowner = PQfnumber(res, "cfgowner");
	i_cfgparser = PQfnumber(res, "cfgparser");

	for (i = 0; i < ntups; i++)
	{
		cfginfo[i].dobj.objType = DO_TSCONFIG;
		cfginfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		cfginfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&cfginfo[i].dobj);
		cfginfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_cfgname));
		cfginfo[i].dobj.namespace =
			findNamespace(atooid(PQgetvalue(res, i, i_cfgnamespace)));
		cfginfo[i].rolname = getRoleName(PQgetvalue(res, i, i_cfgowner));
		cfginfo[i].cfgparser = atooid(PQgetvalue(res, i, i_cfgparser));

		/* 决定是否要导出它 */
		selectDumpableObject(&(cfginfo[i].dobj), fout);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return cfginfo;
}

/*
 * getForeignDataWrappers:
 *   读取系统目录中的所有 FDW，并将它们返回到
 *   FdwInfo* 结构中
 * 
 *   numForeignDataWrappers 设置为读取到的 FDW 数
 */
FdwInfo *
getForeignDataWrappers(Archive *fout, int *numForeignDataWrappers)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	FdwInfo    *fdwinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_fdwname;
	int			i_fdwowner;
	int			i_fdwhandler;
	int			i_fdwvalidator;
	int			i_fdwacl;
	int			i_acldefault;
	int			i_fdwoptions;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, fdwname, "
						 "fdwowner, "
						 "fdwhandler::pg_catalog.regproc, "
						 "fdwvalidator::pg_catalog.regproc, "
						 "fdwacl, "
						 "acldefault('F', fdwowner) AS acldefault, "
						 "array_to_string(ARRAY("
						 "SELECT quote_ident(option_name) || ' ' || "
						 "quote_literal(option_value) "
						 "FROM pg_options_to_table(fdwoptions) "
						 "ORDER BY option_name"
						 "), E',\n    ') AS fdwoptions "
						 "FROM pg_foreign_data_wrapper");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numForeignDataWrappers = ntups;

	fdwinfo = (FdwInfo *) pg_malloc(ntups * sizeof(FdwInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_fdwname = PQfnumber(res, "fdwname");
	i_fdwowner = PQfnumber(res, "fdwowner");
	i_fdwhandler = PQfnumber(res, "fdwhandler");
	i_fdwvalidator = PQfnumber(res, "fdwvalidator");
	i_fdwacl = PQfnumber(res, "fdwacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_fdwoptions = PQfnumber(res, "fdwoptions");

	for (i = 0; i < ntups; i++)
	{
		fdwinfo[i].dobj.objType = DO_FDW;
		fdwinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		fdwinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&fdwinfo[i].dobj);
		fdwinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_fdwname));
		fdwinfo[i].dobj.namespace = NULL;
		fdwinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_fdwacl));
		fdwinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		fdwinfo[i].dacl.privtype = 0;
		fdwinfo[i].dacl.initprivs = NULL;
		fdwinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_fdwowner));
		fdwinfo[i].fdwhandler = pg_strdup(PQgetvalue(res, i, i_fdwhandler));
		fdwinfo[i].fdwvalidator = pg_strdup(PQgetvalue(res, i, i_fdwvalidator));
		fdwinfo[i].fdwoptions = pg_strdup(PQgetvalue(res, i, i_fdwoptions));

		/* 决定是否要导出它 */
		selectDumpableObject(&(fdwinfo[i].dobj), fout);

		/* 标记 FDW 是否有 ACL */
		if (!PQgetisnull(res, i, i_fdwacl))
			fdwinfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return fdwinfo;
}

/*
 * getForeignServers:
 *   读取系统目录中的所有外部服务器，并将它们返回到
 *   ForeignServerInfo * 结构中
 * 
 *   numForeignServers 设置为读取到的服务器数
 */
ForeignServerInfo *
getForeignServers(Archive *fout, int *numForeignServers)
{
	PGresult   *res;
	int			ntups;
	int			i;
	PQExpBuffer query;
	ForeignServerInfo *srvinfo;
	int			i_tableoid;
	int			i_oid;
	int			i_srvname;
	int			i_srvowner;
	int			i_srvfdw;
	int			i_srvtype;
	int			i_srvversion;
	int			i_srvacl;
	int			i_acldefault;
	int			i_srvoptions;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT tableoid, oid, srvname, "
						 "srvowner, "
						 "srvfdw, srvtype, srvversion, srvacl, "
						 "acldefault('S', srvowner) AS acldefault, "
						 "array_to_string(ARRAY("
						 "SELECT quote_ident(option_name) || ' ' || "
						 "quote_literal(option_value) "
						 "FROM pg_options_to_table(srvoptions) "
						 "ORDER BY option_name"
						 "), E',\n    ') AS srvoptions "
						 "FROM pg_foreign_server");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numForeignServers = ntups;

	srvinfo = (ForeignServerInfo *) pg_malloc(ntups * sizeof(ForeignServerInfo));

	i_tableoid = PQfnumber(res, "tableoid");
	i_oid = PQfnumber(res, "oid");
	i_srvname = PQfnumber(res, "srvname");
	i_srvowner = PQfnumber(res, "srvowner");
	i_srvfdw = PQfnumber(res, "srvfdw");
	i_srvtype = PQfnumber(res, "srvtype");
	i_srvversion = PQfnumber(res, "srvversion");
	i_srvacl = PQfnumber(res, "srvacl");
	i_acldefault = PQfnumber(res, "acldefault");
	i_srvoptions = PQfnumber(res, "srvoptions");

	for (i = 0; i < ntups; i++)
	{
		srvinfo[i].dobj.objType = DO_FOREIGN_SERVER;
		srvinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		srvinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&srvinfo[i].dobj);
		srvinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_srvname));
		srvinfo[i].dobj.namespace = NULL;
		srvinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_srvacl));
		srvinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		srvinfo[i].dacl.privtype = 0;
		srvinfo[i].dacl.initprivs = NULL;
		srvinfo[i].rolname = getRoleName(PQgetvalue(res, i, i_srvowner));
		srvinfo[i].srvfdw = atooid(PQgetvalue(res, i, i_srvfdw));
		srvinfo[i].srvtype = pg_strdup(PQgetvalue(res, i, i_srvtype));
		srvinfo[i].srvversion = pg_strdup(PQgetvalue(res, i, i_srvversion));
		srvinfo[i].srvoptions = pg_strdup(PQgetvalue(res, i, i_srvoptions));

		/* 决定是否要导出它 */
		selectDumpableObject(&(srvinfo[i].dobj), fout);

		/* 服务器有用户映射 */
		srvinfo[i].dobj.components |= DUMP_COMPONENT_USERMAP;

		/* 标记服务器是否有 ACL */
		if (!PQgetisnull(res, i, i_srvacl))
			srvinfo[i].dobj.components |= DUMP_COMPONENT_ACL;
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return srvinfo;
}

/*
 * getDefaultACLs:
 *   读取系统目录中的所有默认 ACL 信息，并将它们返回到
 *   DefaultACLInfo 结构中
 * 
 *   numDefaultACLs 设置为读取到的 ACL 数
 */
DefaultACLInfo *
getDefaultACLs(Archive *fout, int *numDefaultACLs)
{
	DumpOptions *dopt = fout->dopt;
	DefaultACLInfo *daclinfo;
	PQExpBuffer query;
	PGresult   *res;
	int			i_oid;
	int			i_tableoid;
	int			i_defaclrole;
	int			i_defaclnamespace;
	int			i_defaclobjtype;
	int			i_defaclacl;
	int			i_acldefault;
	int			i,
				ntups;

	query = createPQExpBuffer();

	/*
	 * 全局条目（defaclnamespace=0）替代其对象类型的硬编码默认
	 * ACL。我们应该将它们作为相对于默认 ACL 的增量来导出，
	 * 因为那将被用作解释 ALTER DEFAULT PRIVILEGES 命令的起点。
	 * 另一方面，非全局条目只能添加权限而不能撤销它们。
	 * 我们必须按原样导出这些条目（即作为空 ACL 的增量）。
	 * 
	 * 我们可以使用 defaclobjtype 作为 acldefault() 的对象类型，
	 * 除了 'S'（DEFACLOBJ_SEQUENCE）的情况，它必须转换为 's'。
	 *
	 *
	 */
	appendPQExpBufferStr(query,
						 "SELECT oid, tableoid, "
						 "defaclrole, "
						 "defaclnamespace, "
						 "defaclobjtype, "
						 "defaclacl, "
						 "CASE WHEN defaclnamespace = 0 THEN "
						 "acldefault(CASE WHEN defaclobjtype = 'S' "
						 "THEN 's'::\"char\" ELSE defaclobjtype END, "
						 "defaclrole) ELSE '{}' END AS acldefault "
						 "FROM pg_default_acl");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	*numDefaultACLs = ntups;

	daclinfo = (DefaultACLInfo *) pg_malloc(ntups * sizeof(DefaultACLInfo));

	i_oid = PQfnumber(res, "oid");
	i_tableoid = PQfnumber(res, "tableoid");
	i_defaclrole = PQfnumber(res, "defaclrole");
	i_defaclnamespace = PQfnumber(res, "defaclnamespace");
	i_defaclobjtype = PQfnumber(res, "defaclobjtype");
	i_defaclacl = PQfnumber(res, "defaclacl");
	i_acldefault = PQfnumber(res, "acldefault");

	for (i = 0; i < ntups; i++)
	{
		Oid			nspid = atooid(PQgetvalue(res, i, i_defaclnamespace));

		daclinfo[i].dobj.objType = DO_DEFAULT_ACL;
		daclinfo[i].dobj.catId.tableoid = atooid(PQgetvalue(res, i, i_tableoid));
		daclinfo[i].dobj.catId.oid = atooid(PQgetvalue(res, i, i_oid));
		AssignDumpId(&daclinfo[i].dobj);
		/* 有点取巧 ... 值得想一个更好的对象名称吗？ */
		daclinfo[i].dobj.name = pg_strdup(PQgetvalue(res, i, i_defaclobjtype));

		if (nspid != InvalidOid)
			daclinfo[i].dobj.namespace = findNamespace(nspid);
		else
			daclinfo[i].dobj.namespace = NULL;

		daclinfo[i].dacl.acl = pg_strdup(PQgetvalue(res, i, i_defaclacl));
		daclinfo[i].dacl.acldefault = pg_strdup(PQgetvalue(res, i, i_acldefault));
		daclinfo[i].dacl.privtype = 0;
		daclinfo[i].dacl.initprivs = NULL;
		daclinfo[i].defaclrole = getRoleName(PQgetvalue(res, i, i_defaclrole));
		daclinfo[i].defaclobjtype = *(PQgetvalue(res, i, i_defaclobjtype));

		/* 默认 ACL 当然是 ACL */
		daclinfo[i].dobj.components |= DUMP_COMPONENT_ACL;

		/* 决定是否要导出它 */
		selectDumpableDefaultACL(&(daclinfo[i]), dopt);
	}

	PQclear(res);

	destroyPQExpBuffer(query);

	return daclinfo;
}

/*
 * getRoleName -- 根据 OID 查找角色的名称
 * 
 * 在当前用法中，我们不期望失败，因此对于错误的 OID 直接报错。
 */
static const char *
getRoleName(const char *roleoid_str)
{
	Oid			roleoid = atooid(roleoid_str);

	/*
	 * 执行二分查找以找到适当的条目。
	 */
	if (nrolenames > 0)
	{
		RoleNameItem *low = &rolenames[0];
		RoleNameItem *high = &rolenames[nrolenames - 1];

		while (low <= high)
		{
			RoleNameItem *middle = low + (high - low) / 2;

			if (roleoid < middle->roleoid)
				high = middle - 1;
			else if (roleoid > middle->roleoid)
				low = middle + 1;
			else
				return middle->rolename;	/* 找到匹配 */
		}
	}

	pg_fatal("role with OID %u does not exist", roleoid);
	return NULL;				/* 使编译器保持安静 */
}

/*
 * collectRoleNames --
 * 
 * 构建一个包含所有已知角色的表。
 * 该表按 OID 排序以提高查找速度。
 */
static void
collectRoleNames(Archive *fout)
{
	PGresult   *res;
	const char *query;
	int			i;

	query = "SELECT oid, rolname FROM pg_catalog.pg_roles ORDER BY 1";

	res = ExecuteSqlQuery(fout, query, PGRES_TUPLES_OK);

	nrolenames = PQntuples(res);

	rolenames = (RoleNameItem *) pg_malloc(nrolenames * sizeof(RoleNameItem));

	for (i = 0; i < nrolenames; i++)
	{
		rolenames[i].roleoid = atooid(PQgetvalue(res, i, 0));
		rolenames[i].rolename = pg_strdup(PQgetvalue(res, i, 1));
	}

	PQclear(res);
}

/*
 * getAdditionalACLs
 * 
 * 我们现在已经创建了所有 DumpableObject，并收集了出现在
 * 直接关联的目录条目中的 ACL 数据。但是，还有更多与 ACL
 * 相关的信息需要收集。如果表的任何列有 ACL，我们必须设置
 * TableInfo 的 DUMP_COMPONENT_ACL 组件标志以及其
 * hascolumnACLs 标志（但我们不会在此存储 ACL 本身）。
 * 此外，在具有 pg_init_privs 目录的版本中，读取该目录并
 * 将信息加载到相关的 DumpableObject 中。
 */
static void
getAdditionalACLs(Archive *fout)
{
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			ntups,
				i;

	/* 检查每列 ACL */
	appendPQExpBufferStr(query,
						 "SELECT DISTINCT attrelid FROM pg_attribute "
						 "WHERE attacl IS NOT NULL");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	for (i = 0; i < ntups; i++)
	{
		Oid			relid = atooid(PQgetvalue(res, i, 0));
		TableInfo  *tblinfo;

		tblinfo = findTableByOid(relid);
		/* 忽略我们没有 DumpableObject 的表是可以的 */
		if (tblinfo)
		{
			tblinfo->dobj.components |= DUMP_COMPONENT_ACL;
			tblinfo->hascolumnACLs = true;
		}
	}
	PQclear(res);

	/* 获取初始权限数据 */
	if (fout->remoteVersion >= 90600)
	{
		printfPQExpBuffer(query,
						  "SELECT objoid, classoid, objsubid, privtype, initprivs "
						  "FROM pg_init_privs");

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		ntups = PQntuples(res);
		for (i = 0; i < ntups; i++)
		{
			Oid			objoid = atooid(PQgetvalue(res, i, 0));
			Oid			classoid = atooid(PQgetvalue(res, i, 1));
			int			objsubid = atoi(PQgetvalue(res, i, 2));
			char		privtype = *(PQgetvalue(res, i, 3));
			char	   *initprivs = PQgetvalue(res, i, 4);
			CatalogId	objId;
			DumpableObject *dobj;

			objId.tableoid = classoid;
			objId.oid = objoid;
			dobj = findObjectByCatalogId(objId);
			/* 忽略我们没有 DumpableObject 的条目是可以的 */
			if (dobj)
			{
				/* 处理子对象 initprivs */
				if (objsubid != 0)
				{
					if (dobj->objType == DO_TABLE)
					{
						/* 对于列 initprivs，设置表的 ACL 标志 */
						dobj->components |= DUMP_COMPONENT_ACL;
						((TableInfo *) dobj)->hascolumnACLs = true;
					}
					else
						pg_log_warning("unsupported pg_init_privs entry: %u %u %d",
									   classoid, objoid, objsubid);
					continue;
				}

				/*
				 * 我们忽略 public 模式的 pg_init_privs.initprivs 条目，
				 * 如 getNamespaces() 中所述。
				 */
				if (dobj->objType == DO_NAMESPACE &&
					strcmp(dobj->name, "public") == 0)
					continue;

				/* 否则，它最好是我们认为具有 ACL 的类型 */
				if (dobj->objType == DO_NAMESPACE ||
					dobj->objType == DO_TYPE ||
					dobj->objType == DO_FUNC ||
					dobj->objType == DO_AGG ||
					dobj->objType == DO_TABLE ||
					dobj->objType == DO_PROCLANG ||
					dobj->objType == DO_FDW ||
					dobj->objType == DO_FOREIGN_SERVER)
				{
					DumpableObjectWithAcl *daobj = (DumpableObjectWithAcl *) dobj;

					daobj->dacl.privtype = privtype;
					daobj->dacl.initprivs = pstrdup(initprivs);
				}
				else
					pg_log_warning("unsupported pg_init_privs entry: %u %u %d",
								   classoid, objoid, objsubid);
			}
		}
		PQclear(res);
	}

	destroyPQExpBuffer(query);
}

/*
 * dumpCommentExtended --
 * 
 * 此例程用于导出与传递给此例程的对象关联的任何注释。
 * 该例程接受对象类型和对象名称（可直接输出，
 * 仅需要装饰模式名），加上对象的命名空间和所有者
 * （用于标记 ArchiveEntry），加上目录 ID 和 subid
 * （作为 pg_description 的查找键），加上对象的导出 ID
 * （用于设置依赖）。如果找到匹配的 pg_description 条目，
 * 则导出。
 * 
 * 注意：在某些情况下，例如触发器和规则的注释，
 * "类型"字符串实际上类似于 "TRIGGER name ON"。
 * 这有点取巧，但为了使其更清晰而使所有调用者的
 * API 复杂化似乎不值得。
 * 
 * 注意：虽然此例程接受一个 dumpId 用于依赖目的，
 * 但该目的仅是在发出的导出文件中标记依赖，
 * 以供 pg_restore 将来使用。我们不会将其用于确定
 * 注释在导出文件中的排序，因为此例程在依赖排序
 * 之后调用。此例程应在为指定对象调用
 * ArchiveEntry() 之后立即调用。
 */
static void
dumpCommentExtended(Archive *fout, const char *type,
					const char *name, const char *namespace,
					const char *owner, CatalogId catalogId,
					int subid, DumpId dumpId,
					const char *initdb_comment)
{
	DumpOptions *dopt = fout->dopt;
	CommentItem *comments;
	int			ncomments;

	/* 如果提供了 --no-comments，则不执行任何操作 */
	if (dopt->no_comments)
		return;

	/* 注释是模式而非数据 ... 但大对象注释是数据 */
	if (strcmp(type, "LARGE OBJECT") != 0)
	{
		if (dopt->dataOnly)
			return;
	}
	else
	{
		/* 我们在二进制升级模式下确实会导出大对象注释 */
		if (dopt->schemaOnly && !dopt->binary_upgrade)
			return;
	}

	/* 使用表搜索与 catalogId 关联的注释 */
	ncomments = findComments(catalogId.tableoid, catalogId.oid,
							 &comments);

	/* 是否有匹配 subid 的条目？ */
	while (ncomments > 0)
	{
		if (comments->objsubid == subid)
			break;
		comments++;
		ncomments--;
	}

	if (initdb_comment != NULL)
	{
		static CommentItem empty_comment = {.descr = ""};

		/*
		 * initdb 创建此对象时附带了注释。跳过导出
		 * initdb 提供的注释，这会为非超级用户使用
		 * pg_dump 带来复杂性。当 DBA 已删除 initdb 的
		 * 注释时，复制该行为。
		 */
		if (ncomments == 0)
		{
			comments = &empty_comment;
			ncomments = 1;
		}
		else if (strcmp(comments->descr, initdb_comment) == 0)
			ncomments = 0;
	}

	/* 如果存在注释，构建 COMMENT ON 语句 */
	if (ncomments > 0)
	{
		PQExpBuffer query = createPQExpBuffer();
		PQExpBuffer tag = createPQExpBuffer();

		appendPQExpBuffer(query, "COMMENT ON %s ", type);
		if (namespace && *namespace)
			appendPQExpBuffer(query, "%s.", fmtId(namespace));
		appendPQExpBuffer(query, "%s IS ", name);
		appendStringLiteralAH(query, comments->descr, fout);
		appendPQExpBufferStr(query, ";\n");

		appendPQExpBuffer(tag, "%s %s", type, name);

		/*
		 * 我们将注释标记为 SECTION_NONE，因为它们实际上
		 * 与父对象属于相同的部分，无论是 pre-data 还是
		 * post-data。
		 */
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = namespace,
								  .owner = owner,
								  .description = "COMMENT",
								  .section = SECTION_NONE,
								  .createStmt = query->data,
								  .deps = &dumpId,
								  .nDeps = 1));

		destroyPQExpBuffer(query);
		destroyPQExpBuffer(tag);
	}
}

/*
 * dumpComment --
 * 
 * 上述函数的典型简化。
 */
static inline void
dumpComment(Archive *fout, const char *type,
			const char *name, const char *namespace,
			const char *owner, CatalogId catalogId,
			int subid, DumpId dumpId)
{
	dumpCommentExtended(fout, type, name, namespace, owner,
						catalogId, subid, dumpId, NULL);
}

/*
 * dumpTableComment --
 * 
 * 如上所述，但导出指定表（或视图）及其列的注释。
 *
 */
static void
dumpTableComment(Archive *fout, const TableInfo *tbinfo,
				 const char *reltypename)
{
	DumpOptions *dopt = fout->dopt;
	CommentItem *comments;
	int			ncomments;
	PQExpBuffer query;
	PQExpBuffer tag;

	/* 如果提供了 --no-comments，则不执行任何操作 */
	if (dopt->no_comments)
		return;

	/* 注释是模式而非数据 */
	if (dopt->dataOnly)
		return;

	/* 使用表搜索与关系关联的注释 */
	ncomments = findComments(tbinfo->dobj.catId.tableoid,
							 tbinfo->dobj.catId.oid,
							 &comments);

	/* 如果存在注释，构建 COMMENT ON 语句 */
	if (ncomments <= 0)
		return;

	query = createPQExpBuffer();
	tag = createPQExpBuffer();

	while (ncomments > 0)
	{
		const char *descr = comments->descr;
		int			objsubid = comments->objsubid;

		if (objsubid == 0)
		{
			resetPQExpBuffer(tag);
			appendPQExpBuffer(tag, "%s %s", reltypename,
							  fmtId(tbinfo->dobj.name));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON %s %s IS ", reltypename,
							  fmtQualifiedDumpable(tbinfo));
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBufferStr(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = tag->data,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = query->data,
									  .deps = &(tbinfo->dobj.dumpId),
									  .nDeps = 1));
		}
		else if (objsubid > 0 && objsubid <= tbinfo->numatts)
		{
			resetPQExpBuffer(tag);
			appendPQExpBuffer(tag, "COLUMN %s.",
							  fmtId(tbinfo->dobj.name));
			appendPQExpBufferStr(tag, fmtId(tbinfo->attnames[objsubid - 1]));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON COLUMN %s.",
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(query, "%s IS ",
							  fmtId(tbinfo->attnames[objsubid - 1]));
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBufferStr(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = tag->data,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = query->data,
									  .deps = &(tbinfo->dobj.dumpId),
									  .nDeps = 1));
		}

		comments++;
		ncomments--;
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(tag);
}

/*
 * findComments --
 * 
 * 查找与给定对象关联的注释（如果有）。与给定
 * classoid/objoid 关联的所有 objsubid 值通过
 * 一次搜索找到。
 */
static int
findComments(Oid classoid, Oid objoid, CommentItem **items)
{
	CommentItem *middle = NULL;
	CommentItem *low;
	CommentItem *high;
	int			nmatch;

	/*
	 * 执行二分查找以找到与对象匹配的某个条目。
	 */
	low = &comments[0];
	high = &comments[ncomments - 1];
	while (low <= high)
	{
		middle = low + (high - low) / 2;

		if (classoid < middle->classoid)
			high = middle - 1;
		else if (classoid > middle->classoid)
			low = middle + 1;
		else if (objoid < middle->objoid)
			high = middle - 1;
		else if (objoid > middle->objoid)
			low = middle + 1;
		else
			break;				/* 找到匹配 */
	}

	if (low > high)				/* 无匹配 */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * 现在确定有多少条目与对象匹配。搜索循环不变式
	 * 仍然成立：只有 low 和 high 之间（含）的条目可能匹配。
	 *
	 */
	nmatch = 1;
	while (middle > low)
	{
		if (classoid != middle[-1].classoid ||
			objoid != middle[-1].objoid)
			break;
		middle--;
		nmatch++;
	}

	*items = middle;

	middle += nmatch;
	while (middle <= high)
	{
		if (classoid != middle->classoid ||
			objoid != middle->objoid)
			break;
		middle++;
		nmatch++;
	}

	return nmatch;
}

/*
 * collectComments --
 * 
 * 构建一个包含所有可用于数据库对象的注释的表；
 * 还为每个相关对象设置 has-comment 组件标志。
 * 
 * 我们过去常常为注释执行逐对象查询，但一次性
 * 全部拉取要快得多，而且在大多数数据库上内存成本
 * 不高。
 * 
 * 该表按 classoid/objid/objsubid 排序以提高查找速度。
 */
static void
collectComments(Archive *fout)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_description;
	int			i_classoid;
	int			i_objoid;
	int			i_objsubid;
	int			ntups;
	int			i;
	DumpableObject *dobj;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query, "SELECT description, classoid, objoid, objsubid "
						 "FROM pg_catalog.pg_description "
						 "ORDER BY classoid, objoid, objsubid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	/* 构建包含数值形式 OID 的查找表 */

	i_description = PQfnumber(res, "description");
	i_classoid = PQfnumber(res, "classoid");
	i_objoid = PQfnumber(res, "objoid");
	i_objsubid = PQfnumber(res, "objsubid");

	ntups = PQntuples(res);

	comments = (CommentItem *) pg_malloc(ntups * sizeof(CommentItem));
	ncomments = 0;
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		int			subid;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classoid));
		objId.oid = atooid(PQgetvalue(res, i, i_objoid));
		subid = atoi(PQgetvalue(res, i, i_objsubid));

		/* 我们不需要记住不匹配任何可导出对象的注释 */
		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);
		if (dobj == NULL)
			continue;

		/*
		 * 复合类型的列的注释链接到该类型的 pg_class 条目，
		 * 但我们需要在该类型自身的 DumpableObject 中设置
		 * DUMP_COMPONENT_COMMENT 标志。
		 */
		if (subid != 0 && dobj->objType == DO_TABLE &&
			((TableInfo *) dobj)->relkind == RELKIND_COMPOSITE_TYPE)
		{
			TypeInfo   *cTypeInfo;

			cTypeInfo = findTypeByOid(((TableInfo *) dobj)->reltype);
			if (cTypeInfo)
				cTypeInfo->dobj.components |= DUMP_COMPONENT_COMMENT;
		}
		else
			dobj->components |= DUMP_COMPONENT_COMMENT;

		comments[ncomments].descr = pg_strdup(PQgetvalue(res, i, i_description));
		comments[ncomments].classoid = objId.tableoid;
		comments[ncomments].objoid = objId.oid;
		comments[ncomments].objsubid = subid;
		ncomments++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpDumpableObject
 * 
 * 此例程及其子例程负责为每个要导出的对象创建
 * ArchiveEntry（TOC 对象）。
 */
static void
dumpDumpableObject(Archive *fout, DumpableObject *dobj)
{
	/*
	 * 清除此对象不存在的组件的任何导出请求位。
	 * （这使得最初对所有对象类型使用 DUMP_COMPONENT_ALL
	 * 作为请求是安全的。）
	 */
	dobj->dump &= dobj->components;

	/* 现在，如果此处无事可做，则短路返回。 */
	if (dobj->dump == 0)
		return;

	switch (dobj->objType)
	{
		case DO_NAMESPACE:
			dumpNamespace(fout, (const NamespaceInfo *) dobj);
			break;
		case DO_EXTENSION:
			dumpExtension(fout, (const ExtensionInfo *) dobj);
			break;
		case DO_TYPE:
			dumpType(fout, (const TypeInfo *) dobj);
			break;
		case DO_SHELL_TYPE:
			dumpShellType(fout, (const ShellTypeInfo *) dobj);
			break;
		case DO_FUNC:
			dumpFunc(fout, (const FuncInfo *) dobj);
			break;
		case DO_AGG:
			dumpAgg(fout, (const AggInfo *) dobj);
			break;
		case DO_OPERATOR:
			dumpOpr(fout, (const OprInfo *) dobj);
			break;
		case DO_ACCESS_METHOD:
			dumpAccessMethod(fout, (const AccessMethodInfo *) dobj);
			break;
		case DO_OPCLASS:
			dumpOpclass(fout, (const OpclassInfo *) dobj);
			break;
		case DO_OPFAMILY:
			dumpOpfamily(fout, (const OpfamilyInfo *) dobj);
			break;
		case DO_COLLATION:
			dumpCollation(fout, (const CollInfo *) dobj);
			break;
		case DO_CONVERSION:
			dumpConversion(fout, (const ConvInfo *) dobj);
			break;
		case DO_TABLE:
			dumpTable(fout, (const TableInfo *) dobj);
			break;
		case DO_TABLE_ATTACH:
			dumpTableAttach(fout, (const TableAttachInfo *) dobj);
			break;
		case DO_ATTRDEF:
			dumpAttrDef(fout, (const AttrDefInfo *) dobj);
			break;
		case DO_INDEX:
			dumpIndex(fout, (const IndxInfo *) dobj);
			break;
		case DO_INDEX_ATTACH:
			dumpIndexAttach(fout, (const IndexAttachInfo *) dobj);
			break;
		case DO_STATSEXT:
			dumpStatisticsExt(fout, (const StatsExtInfo *) dobj);
			break;
		case DO_REFRESH_MATVIEW:
			refreshMatViewData(fout, (const TableDataInfo *) dobj);
			break;
		case DO_RULE:
			dumpRule(fout, (const RuleInfo *) dobj);
			break;
		case DO_TRIGGER:
			dumpTrigger(fout, (const TriggerInfo *) dobj);
			break;
		case DO_EVENT_TRIGGER:
			dumpEventTrigger(fout, (const EventTriggerInfo *) dobj);
			break;
		case DO_CONSTRAINT:
			dumpConstraint(fout, (const ConstraintInfo *) dobj);
			break;
		case DO_FK_CONSTRAINT:
			dumpConstraint(fout, (const ConstraintInfo *) dobj);
			break;
		case DO_PROCLANG:
			dumpProcLang(fout, (const ProcLangInfo *) dobj);
			break;
		case DO_CAST:
			dumpCast(fout, (const CastInfo *) dobj);
			break;
		case DO_TRANSFORM:
			dumpTransform(fout, (const TransformInfo *) dobj);
			break;
		case DO_SEQUENCE_SET:
			dumpSequenceData(fout, (const TableDataInfo *) dobj);
			break;
		case DO_TABLE_DATA:
			dumpTableData(fout, (const TableDataInfo *) dobj);
			break;
		case DO_DUMMY_TYPE:
			/* 表行类型和数组类型从不单独导出 */
			break;
		case DO_TSPARSER:
			dumpTSParser(fout, (const TSParserInfo *) dobj);
			break;
		case DO_TSDICT:
			dumpTSDictionary(fout, (const TSDictInfo *) dobj);
			break;
		case DO_TSTEMPLATE:
			dumpTSTemplate(fout, (const TSTemplateInfo *) dobj);
			break;
		case DO_TSCONFIG:
			dumpTSConfig(fout, (const TSConfigInfo *) dobj);
			break;
		case DO_FDW:
			dumpForeignDataWrapper(fout, (const FdwInfo *) dobj);
			break;
		case DO_FOREIGN_SERVER:
			dumpForeignServer(fout, (const ForeignServerInfo *) dobj);
			break;
		case DO_DEFAULT_ACL:
			dumpDefaultACL(fout, (const DefaultACLInfo *) dobj);
			break;
		case DO_LARGE_OBJECT:
			dumpLO(fout, (const LoInfo *) dobj);
			break;
		case DO_LARGE_OBJECT_DATA:
			if (dobj->dump & DUMP_COMPONENT_DATA)
			{
				TocEntry   *te;

				te = ArchiveEntry(fout, dobj->catId, dobj->dumpId,
								  ARCHIVE_OPTS(.tag = dobj->name,
											   .description = "BLOBS",
											   .section = SECTION_DATA,
											   .dumpFn = dumpLOs));

				/*
				 * 设置 TocEntry 的 dataLength，以防我们正在进行
				 * 并行导出并希望按表大小排序导出作业。
				 * （我们需要为每个具有 DataDumper 函数的 TocEntry
				 * 提供某种大小估计。）我们目前没有任何廉价的方法来
				 * 估计大对象的大小，但这无关紧要；让我们将大小设置为
				 * 一个较大的值，以便并行导出会首先启动此作业。
				 * 如果有很多大对象，我们赚了；如果没有，
				 * 我们也没损失多少。（如果你想改进这一点，
				 * 你真正应该考虑的是允许大对象导出并行化，
				 * 而不仅仅是为单个 TOC 条目获取更智能的估计。）
				 *
				 */
				te->dataLength = INT_MAX;
			}
			break;
		case DO_POLICY:
			dumpPolicy(fout, (const PolicyInfo *) dobj);
			break;
		case DO_PUBLICATION:
			dumpPublication(fout, (const PublicationInfo *) dobj);
			break;
		case DO_PUBLICATION_REL:
			dumpPublicationTable(fout, (const PublicationRelInfo *) dobj);
			break;
		case DO_PUBLICATION_TABLE_IN_SCHEMA:
			dumpPublicationNamespace(fout,
									 (const PublicationSchemaInfo *) dobj);
			break;
		case DO_SUBSCRIPTION:
			dumpSubscription(fout, (const SubscriptionInfo *) dobj);
			break;
		case DO_PRE_DATA_BOUNDARY:
		case DO_POST_DATA_BOUNDARY:
			/* 从不导出，无事可做 */
			break;
	}
}

/*
 * dumpNamespace
 *   输出到 fout 以重建用户定义命名空间的查询
 */
static void
dumpNamespace(Archive *fout, const NamespaceInfo *nspinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qnspname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qnspname = pg_strdup(fmtId(nspinfo->dobj.name));

	if (nspinfo->create)
	{
		appendPQExpBuffer(delq, "DROP SCHEMA %s;\n", qnspname);
		appendPQExpBuffer(q, "CREATE SCHEMA %s;\n", qnspname);
	}
	else
	{
		/* 参见 selectDumpableNamespace() */
		appendPQExpBufferStr(delq,
							 "-- *not* dropping schema, since initdb creates it\n");
		appendPQExpBufferStr(q,
							 "-- *not* creating schema, since initdb creates it\n");
	}

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &nspinfo->dobj,
										"SCHEMA", qnspname, NULL);

	if (nspinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, nspinfo->dobj.catId, nspinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = nspinfo->dobj.name,
								  .owner = nspinfo->rolname,
								  .description = "SCHEMA",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出模式注释和安全标签 */
	if (nspinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
	{
		const char *initdb_comment = NULL;

		if (!nspinfo->create && strcmp(qnspname, "public") == 0)
			initdb_comment = "standard public schema";
		dumpCommentExtended(fout, "SCHEMA", qnspname,
							NULL, nspinfo->rolname,
							nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId,
							initdb_comment);
	}

	if (nspinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SCHEMA", qnspname,
					 NULL, nspinfo->rolname,
					 nspinfo->dobj.catId, 0, nspinfo->dobj.dumpId);

	if (nspinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, nspinfo->dobj.dumpId, InvalidDumpId, "SCHEMA",
				qnspname, NULL, NULL,
				nspinfo->rolname, &nspinfo->dacl);

	free(qnspname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpExtension
 *   输出到 fout 以重建扩展的查询
 */
static void
dumpExtension(Archive *fout, const ExtensionInfo *extinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qextname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qextname = pg_strdup(fmtId(extinfo->dobj.name));

	appendPQExpBuffer(delq, "DROP EXTENSION %s;\n", qextname);

	if (!dopt->binary_upgrade)
	{
		/*
		 * 在常规导出中，我们简单地创建扩展，有意不指定版本，
		 * 以便使用目标安装的默认版本。
		 * 
		 * 在此使用 IF NOT EXISTS 与我们对其他对象类型的行为不同；
		 * 但存在多种场景，在恢复之前手动创建所需的扩展很方便，
		 * 因此我们更愿意允许它已经存在。
		 *
		 *
		 */
		appendPQExpBuffer(q, "CREATE EXTENSION IF NOT EXISTS %s WITH SCHEMA %s;\n",
						  qextname, fmtId(extinfo->namespace));
	}
	else
	{
		/*
		 * 在二进制升级模式中，精确复制数据库的状态至关重要，
		 * 因此我们的过程是创建一个空扩展，正常恢复所有包含的
		 * 对象，然后逐个将它们添加到扩展中。此函数仅执行这些
		 * 步骤中的第一步。binary_upgrade_extension_member()
		 * 负责在创建成员对象时添加它们。
		 *
		 */
		int			i;
		int			n;

		appendPQExpBufferStr(q, "-- For binary upgrade, create an empty extension and insert objects into it\n");

		/*
		 * 我们无条件创建扩展，因此如果它已存在则必须删除它。
		 * 如果用户删除了 'plpgsql' 然后重新添加，导致其 OID
		 * 大于 g_last_builtin_oid，就可能发生这种情况。
		 */
		appendPQExpBuffer(q, "DROP EXTENSION IF EXISTS %s;\n", qextname);

		appendPQExpBufferStr(q,
							 "SELECT pg_catalog.binary_upgrade_create_empty_extension(");
		appendStringLiteralAH(q, extinfo->dobj.name, fout);
		appendPQExpBufferStr(q, ", ");
		appendStringLiteralAH(q, extinfo->namespace, fout);
		appendPQExpBufferStr(q, ", ");
		appendPQExpBuffer(q, "%s, ", extinfo->relocatable ? "true" : "false");
		appendStringLiteralAH(q, extinfo->extversion, fout);
		appendPQExpBufferStr(q, ", ");

		/*
		 * 注意，我们将 extconfig（一个 OID 数组）原样推回
		 * pg_extension。这是可以的，因为 pg_class OID 在
		 * 二进制升级中得以保留。
		 */
		if (strlen(extinfo->extconfig) > 2)
			appendStringLiteralAH(q, extinfo->extconfig, fout);
		else
			appendPQExpBufferStr(q, "NULL");
		appendPQExpBufferStr(q, ", ");
		if (strlen(extinfo->extcondition) > 2)
			appendStringLiteralAH(q, extinfo->extcondition, fout);
		else
			appendPQExpBufferStr(q, "NULL");
		appendPQExpBufferStr(q, ", ");
		appendPQExpBufferStr(q, "ARRAY[");
		n = 0;
		for (i = 0; i < extinfo->dobj.nDeps; i++)
		{
			DumpableObject *extobj;

			extobj = findObjectByDumpId(extinfo->dobj.dependencies[i]);
			if (extobj && extobj->objType == DO_EXTENSION)
			{
				if (n++ > 0)
					appendPQExpBufferChar(q, ',');
				appendStringLiteralAH(q, extobj->name, fout);
			}
		}
		appendPQExpBufferStr(q, "]::pg_catalog.text[]");
		appendPQExpBufferStr(q, ");\n");
	}

	if (extinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, extinfo->dobj.catId, extinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = extinfo->dobj.name,
								  .description = "EXTENSION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出扩展注释和安全标签 */
	if (extinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "EXTENSION", qextname,
					NULL, "",
					extinfo->dobj.catId, 0, extinfo->dobj.dumpId);

	if (extinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "EXTENSION", qextname,
					 NULL, "",
					 extinfo->dobj.catId, 0, extinfo->dobj.dumpId);

	free(qextname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpType
 *   输出到 fout 以重建用户定义类型的查询
 */
static void
dumpType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/* 以适当的格式导出 */
	if (tyinfo->typtype == TYPTYPE_BASE)
		dumpBaseType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_DOMAIN)
		dumpDomain(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_COMPOSITE)
		dumpCompositeType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_ENUM)
		dumpEnumType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_RANGE)
		dumpRangeType(fout, tyinfo);
	else if (tyinfo->typtype == TYPTYPE_PSEUDO && !tyinfo->isDefined)
		dumpUndefinedType(fout, tyinfo);
	else
		pg_log_warning("typtype of data type \"%s\" appears to be invalid",
					   tyinfo->dobj.name);
}

/*
 * dumpEnumType
 *   输出到 fout 以重建用户定义枚举类型的查询
 */
static void
dumpEnumType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			num,
				i;
	Oid			enum_oid;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *label;
	int			i_enumlabel;
	int			i_oid;

	if (!fout->is_prepared[PREPQUERY_DUMPENUMTYPE])
	{
		/* 设置查询以获取特定于枚举的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpEnumType(pg_catalog.oid) AS\n"
							 "SELECT oid, enumlabel "
							 "FROM pg_catalog.pg_enum "
							 "WHERE enumtypid = $1 "
							 "ORDER BY enumsortorder");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPENUMTYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpEnumType('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	num = PQntuples(res);

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	/*
	 * 与普通类型不同，这里不应需要 CASCADE，因为 I/O
	 * 函数是通用的并且不会被删除。
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false, false);

	appendPQExpBuffer(q, "CREATE TYPE %s AS ENUM (",
					  qualtypname);

	if (!dopt->binary_upgrade)
	{
		i_enumlabel = PQfnumber(res, "enumlabel");

		/* 具有服务器分配 OID 的标签 */
		for (i = 0; i < num; i++)
		{
			label = PQgetvalue(res, i, i_enumlabel);
			if (i > 0)
				appendPQExpBufferChar(q, ',');
			appendPQExpBufferStr(q, "\n    ");
			appendStringLiteralAH(q, label, fout);
		}
	}

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
	{
		i_oid = PQfnumber(res, "oid");
		i_enumlabel = PQfnumber(res, "enumlabel");

		/* 具有导出分配（保留）OID 的标签 */
		for (i = 0; i < num; i++)
		{
			enum_oid = atooid(PQgetvalue(res, i, i_oid));
			label = PQgetvalue(res, i, i_enumlabel);

			if (i == 0)
				appendPQExpBufferStr(q, "\n-- For binary upgrade, must preserve pg_enum oids\n");
			appendPQExpBuffer(q,
							  "SELECT pg_catalog.binary_upgrade_set_next_pg_enum_oid('%u'::pg_catalog.oid);\n",
							  enum_oid);
			appendPQExpBuffer(q, "ALTER TYPE %s ADD VALUE ", qualtypname);
			appendStringLiteralAH(q, label, fout);
			appendPQExpBufferStr(q, ";\n\n");
		}
	}

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出类型注释和安全标签 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, &tyinfo->dacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpRangeType
 *   输出到 fout 以重建用户定义范围类型的查询
 */
static void
dumpRangeType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	Oid			collationOid;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *procname;

	if (!fout->is_prepared[PREPQUERY_DUMPRANGETYPE])
	{
		/* 设置查询以获取特定于范围的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpRangeType(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query,
							 "SELECT ");

		if (fout->remoteVersion >= 140000)
			appendPQExpBufferStr(query,
								 "pg_catalog.format_type(rngmultitypid, NULL) AS rngmultitype, ");
		else
			appendPQExpBufferStr(query,
								 "NULL AS rngmultitype, ");

		appendPQExpBufferStr(query,
							 "pg_catalog.format_type(rngsubtype, NULL) AS rngsubtype, "
							 "opc.opcname AS opcname, "
							 "(SELECT nspname FROM pg_catalog.pg_namespace nsp "
							 "  WHERE nsp.oid = opc.opcnamespace) AS opcnsp, "
							 "opc.opcdefault, "
							 "CASE WHEN rngcollation = st.typcollation THEN 0 "
							 "     ELSE rngcollation END AS collation, "
							 "rngcanonical, rngsubdiff "
							 "FROM pg_catalog.pg_range r, pg_catalog.pg_type st, "
							 "     pg_catalog.pg_opclass opc "
							 "WHERE st.oid = rngsubtype AND opc.oid = rngsubopc AND "
							 "rngtypid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPRANGETYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpRangeType('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	/*
	 * 与普通类型不同，这里不应需要 CASCADE，因为 I/O
	 * 函数是通用的并且不会被删除。
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false, true);

	appendPQExpBuffer(q, "CREATE TYPE %s AS RANGE (",
					  qualtypname);

	appendPQExpBuffer(q, "\n    subtype = %s",
					  PQgetvalue(res, 0, PQfnumber(res, "rngsubtype")));

	if (!PQgetisnull(res, 0, PQfnumber(res, "rngmultitype")))
		appendPQExpBuffer(q, ",\n    multirange_type_name = %s",
						  PQgetvalue(res, 0, PQfnumber(res, "rngmultitype")));

	/* 仅在不是子类型的默认值时输出 subtype_opclass */
	if (PQgetvalue(res, 0, PQfnumber(res, "opcdefault"))[0] != 't')
	{
		char	   *opcname = PQgetvalue(res, 0, PQfnumber(res, "opcname"));
		char	   *nspname = PQgetvalue(res, 0, PQfnumber(res, "opcnsp"));

		appendPQExpBuffer(q, ",\n    subtype_opclass = %s.",
						  fmtId(nspname));
		appendPQExpBufferStr(q, fmtId(opcname));
	}

	collationOid = atooid(PQgetvalue(res, 0, PQfnumber(res, "collation")));
	if (OidIsValid(collationOid))
	{
		CollInfo   *coll = findCollationByOid(collationOid);

		if (coll)
			appendPQExpBuffer(q, ",\n    collation = %s",
							  fmtQualifiedDumpable(coll));
	}

	procname = PQgetvalue(res, 0, PQfnumber(res, "rngcanonical"));
	if (strcmp(procname, "-") != 0)
		appendPQExpBuffer(q, ",\n    canonical = %s", procname);

	procname = PQgetvalue(res, 0, PQfnumber(res, "rngsubdiff"));
	if (strcmp(procname, "-") != 0)
		appendPQExpBuffer(q, ",\n    subtype_diff = %s", procname);

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出类型注释和安全标签 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, &tyinfo->dacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpUndefinedType
 *   输出到 fout 以重建 !typisdefined 类型的查询
 * 
 * 这是一个壳类型，但我们使用不同的术语来区分
 * 这种情况与我们不得不发出壳类型定义以打破
 * 循环依赖的情况。未定义类型不应该有任何
 * 依赖它的东西。
 */
static void
dumpUndefinedType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	char	   *qtypname;
	char	   *qualtypname;

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false, false);

	appendPQExpBuffer(q, "CREATE TYPE %s;\n",
					  qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出类型注释和安全标签 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, &tyinfo->dacl);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpBaseType
 *   输出到 fout 以重建用户定义基础类型的查询
 */
static void
dumpBaseType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *typlen;
	char	   *typinput;
	char	   *typoutput;
	char	   *typreceive;
	char	   *typsend;
	char	   *typmodin;
	char	   *typmodout;
	char	   *typanalyze;
	char	   *typsubscript;
	Oid			typreceiveoid;
	Oid			typsendoid;
	Oid			typmodinoid;
	Oid			typmodoutoid;
	Oid			typanalyzeoid;
	Oid			typsubscriptoid;
	char	   *typcategory;
	char	   *typispreferred;
	char	   *typdelim;
	char	   *typbyval;
	char	   *typalign;
	char	   *typstorage;
	char	   *typcollatable;
	char	   *typdefault;
	bool		typdefault_is_literal = false;

	if (!fout->is_prepared[PREPQUERY_DUMPBASETYPE])
	{
		/* 设置查询以获取特定于类型的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpBaseType(pg_catalog.oid) AS\n"
							 "SELECT typlen, "
							 "typinput, typoutput, typreceive, typsend, "
							 "typreceive::pg_catalog.oid AS typreceiveoid, "
							 "typsend::pg_catalog.oid AS typsendoid, "
							 "typanalyze, "
							 "typanalyze::pg_catalog.oid AS typanalyzeoid, "
							 "typdelim, typbyval, typalign, typstorage, "
							 "typmodin, typmodout, "
							 "typmodin::pg_catalog.oid AS typmodinoid, "
							 "typmodout::pg_catalog.oid AS typmodoutoid, "
							 "typcategory, typispreferred, "
							 "(typcollation <> 0) AS typcollatable, "
							 "pg_catalog.pg_get_expr(typdefaultbin, 0) AS typdefaultbin, typdefault, ");

		if (fout->remoteVersion >= 140000)
			appendPQExpBufferStr(query,
								 "typsubscript, "
								 "typsubscript::pg_catalog.oid AS typsubscriptoid ");
		else
			appendPQExpBufferStr(query,
								 "'-' AS typsubscript, 0 AS typsubscriptoid ");

		appendPQExpBufferStr(query, "FROM pg_catalog.pg_type "
							 "WHERE oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPBASETYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpBaseType('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	typlen = PQgetvalue(res, 0, PQfnumber(res, "typlen"));
	typinput = PQgetvalue(res, 0, PQfnumber(res, "typinput"));
	typoutput = PQgetvalue(res, 0, PQfnumber(res, "typoutput"));
	typreceive = PQgetvalue(res, 0, PQfnumber(res, "typreceive"));
	typsend = PQgetvalue(res, 0, PQfnumber(res, "typsend"));
	typmodin = PQgetvalue(res, 0, PQfnumber(res, "typmodin"));
	typmodout = PQgetvalue(res, 0, PQfnumber(res, "typmodout"));
	typanalyze = PQgetvalue(res, 0, PQfnumber(res, "typanalyze"));
	typsubscript = PQgetvalue(res, 0, PQfnumber(res, "typsubscript"));
	typreceiveoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typreceiveoid")));
	typsendoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsendoid")));
	typmodinoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodinoid")));
	typmodoutoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typmodoutoid")));
	typanalyzeoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typanalyzeoid")));
	typsubscriptoid = atooid(PQgetvalue(res, 0, PQfnumber(res, "typsubscriptoid")));
	typcategory = PQgetvalue(res, 0, PQfnumber(res, "typcategory"));
	typispreferred = PQgetvalue(res, 0, PQfnumber(res, "typispreferred"));
	typdelim = PQgetvalue(res, 0, PQfnumber(res, "typdelim"));
	typbyval = PQgetvalue(res, 0, PQfnumber(res, "typbyval"));
	typalign = PQgetvalue(res, 0, PQfnumber(res, "typalign"));
	typstorage = PQgetvalue(res, 0, PQfnumber(res, "typstorage"));
	typcollatable = PQgetvalue(res, 0, PQfnumber(res, "typcollatable"));
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* 需要引号 */
	}
	else
		typdefault = NULL;

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	/*
	 * 我们包含 CASCADE 的原因是类型与其 I/O 函数之间的
	 * 循环依赖使得无法以任何其他方式删除该类型。
	 *
	 */
	appendPQExpBuffer(delq, "DROP TYPE %s CASCADE;\n", qualtypname);

	/*
	 * 我们可能已经有壳类型，但设置 pg_type_oid 是无害的，
	 * 无论如何我们最好设置数组类型 OID。
	 */
	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false, false);

	appendPQExpBuffer(q,
					  "CREATE TYPE %s (\n"
					  "    INTERNALLENGTH = %s",
					  qualtypname,
					  (strcmp(typlen, "-1") == 0) ? "variable" : typlen);

	/* regproc 结果已充分加引号 */
	appendPQExpBuffer(q, ",\n    INPUT = %s", typinput);
	appendPQExpBuffer(q, ",\n    OUTPUT = %s", typoutput);
	if (OidIsValid(typreceiveoid))
		appendPQExpBuffer(q, ",\n    RECEIVE = %s", typreceive);
	if (OidIsValid(typsendoid))
		appendPQExpBuffer(q, ",\n    SEND = %s", typsend);
	if (OidIsValid(typmodinoid))
		appendPQExpBuffer(q, ",\n    TYPMOD_IN = %s", typmodin);
	if (OidIsValid(typmodoutoid))
		appendPQExpBuffer(q, ",\n    TYPMOD_OUT = %s", typmodout);
	if (OidIsValid(typanalyzeoid))
		appendPQExpBuffer(q, ",\n    ANALYZE = %s", typanalyze);

	if (strcmp(typcollatable, "t") == 0)
		appendPQExpBufferStr(q, ",\n    COLLATABLE = true");

	if (typdefault != NULL)
	{
		appendPQExpBufferStr(q, ",\n    DEFAULT = ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	if (OidIsValid(typsubscriptoid))
		appendPQExpBuffer(q, ",\n    SUBSCRIPT = %s", typsubscript);

	if (OidIsValid(tyinfo->typelem))
		appendPQExpBuffer(q, ",\n    ELEMENT = %s",
						  getFormattedTypeName(fout, tyinfo->typelem,
											   zeroIsError));

	if (strcmp(typcategory, "U") != 0)
	{
		appendPQExpBufferStr(q, ",\n    CATEGORY = ");
		appendStringLiteralAH(q, typcategory, fout);
	}

	if (strcmp(typispreferred, "t") == 0)
		appendPQExpBufferStr(q, ",\n    PREFERRED = true");

	if (typdelim && strcmp(typdelim, ",") != 0)
	{
		appendPQExpBufferStr(q, ",\n    DELIMITER = ");
		appendStringLiteralAH(q, typdelim, fout);
	}

	if (*typalign == TYPALIGN_CHAR)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = char");
	else if (*typalign == TYPALIGN_SHORT)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = int2");
	else if (*typalign == TYPALIGN_INT)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = int4");
	else if (*typalign == TYPALIGN_DOUBLE)
		appendPQExpBufferStr(q, ",\n    ALIGNMENT = double");

	if (*typstorage == TYPSTORAGE_PLAIN)
		appendPQExpBufferStr(q, ",\n    STORAGE = plain");
	else if (*typstorage == TYPSTORAGE_EXTERNAL)
		appendPQExpBufferStr(q, ",\n    STORAGE = external");
	else if (*typstorage == TYPSTORAGE_EXTENDED)
		appendPQExpBufferStr(q, ",\n    STORAGE = extended");
	else if (*typstorage == TYPSTORAGE_MAIN)
		appendPQExpBufferStr(q, ",\n    STORAGE = main");

	if (strcmp(typbyval, "t") == 0)
		appendPQExpBufferStr(q, ",\n    PASSEDBYVALUE");

	appendPQExpBufferStr(q, "\n);\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出类型注释和安全标签 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, &tyinfo->dacl);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpDomain
 *   输出到 fout 以重建用户定义域的查询
 */
static void
dumpDomain(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	int			i;
	char	   *qtypname;
	char	   *qualtypname;
	char	   *typnotnull;
	char	   *typdefn;
	char	   *typdefault;
	Oid			typcollation;
	bool		typdefault_is_literal = false;

	if (!fout->is_prepared[PREPQUERY_DUMPDOMAIN])
	{
		/* 设置查询以获取特定于域的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpDomain(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query, "SELECT t.typnotnull, "
							 "pg_catalog.format_type(t.typbasetype, t.typtypmod) AS typdefn, "
							 "pg_catalog.pg_get_expr(t.typdefaultbin, 'pg_catalog.pg_type'::pg_catalog.regclass) AS typdefaultbin, "
							 "t.typdefault, "
							 "CASE WHEN t.typcollation <> u.typcollation "
							 "THEN t.typcollation ELSE 0 END AS typcollation "
							 "FROM pg_catalog.pg_type t "
							 "LEFT JOIN pg_catalog.pg_type u ON (t.typbasetype = u.oid) "
							 "WHERE t.oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPDOMAIN] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpDomain('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	typnotnull = PQgetvalue(res, 0, PQfnumber(res, "typnotnull"));
	typdefn = PQgetvalue(res, 0, PQfnumber(res, "typdefn"));
	if (!PQgetisnull(res, 0, PQfnumber(res, "typdefaultbin")))
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefaultbin"));
	else if (!PQgetisnull(res, 0, PQfnumber(res, "typdefault")))
	{
		typdefault = PQgetvalue(res, 0, PQfnumber(res, "typdefault"));
		typdefault_is_literal = true;	/* 需要引号 */
	}
	else
		typdefault = NULL;
	typcollation = atooid(PQgetvalue(res, 0, PQfnumber(res, "typcollation")));

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 true,	/* 强制数组类型 */
												 false);	/* 强制 multirange 类型 */

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	appendPQExpBuffer(q,
					  "CREATE DOMAIN %s AS %s",
					  qualtypname,
					  typdefn);

	/* 仅在排序规则与基本类型的排序规则不同时输出 */
	if (OidIsValid(typcollation))
	{
		CollInfo   *coll;

		coll = findCollationByOid(typcollation);
		if (coll)
			appendPQExpBuffer(q, " COLLATE %s", fmtQualifiedDumpable(coll));
	}

	if (typnotnull[0] == 't')
		appendPQExpBufferStr(q, " NOT NULL");

	if (typdefault != NULL)
	{
		appendPQExpBufferStr(q, " DEFAULT ");
		if (typdefault_is_literal)
			appendStringLiteralAH(q, typdefault, fout);
		else
			appendPQExpBufferStr(q, typdefault);
	}

	PQclear(res);

	/*
	 * 为域添加任何 CHECK 约束
	 */
	for (i = 0; i < tyinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tyinfo->domChecks[i]);

		if (!domcheck->separate)
			appendPQExpBuffer(q, "\n\tCONSTRAINT %s %s",
							  fmtId(domcheck->dobj.name), domcheck->condef);
	}

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBuffer(delq, "DROP DOMAIN %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"DOMAIN", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "DOMAIN",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出域注释和安全标签 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "DOMAIN", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "DOMAIN", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, &tyinfo->dacl);

	/* 导出每个约束的注释 */
	for (i = 0; i < tyinfo->nDomChecks; i++)
	{
		ConstraintInfo *domcheck = &(tyinfo->domChecks[i]);
		PQExpBuffer conprefix;

		/* 但仅在约束本身在此处被导出时 */
		if (domcheck->separate)
			continue;

		conprefix = createPQExpBuffer();
		appendPQExpBuffer(conprefix, "CONSTRAINT %s ON DOMAIN",
						  fmtId(domcheck->dobj.name));

		if (domcheck->dobj.dump & DUMP_COMPONENT_COMMENT)
			dumpComment(fout, conprefix->data, qtypname,
						tyinfo->dobj.namespace->dobj.name,
						tyinfo->rolname,
						domcheck->dobj.catId, 0, tyinfo->dobj.dumpId);

		destroyPQExpBuffer(conprefix);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpCompositeType
 *   输出到 fout 以重建用户定义独立复合类型的查询
 *
 */
static void
dumpCompositeType(Archive *fout, const TypeInfo *tyinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer dropped = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	PQExpBuffer query = createPQExpBuffer();
	PGresult   *res;
	char	   *qtypname;
	char	   *qualtypname;
	int			ntups;
	int			i_attname;
	int			i_atttypdefn;
	int			i_attlen;
	int			i_attalign;
	int			i_attisdropped;
	int			i_attcollation;
	int			i;
	int			actual_atts;

	if (!fout->is_prepared[PREPQUERY_DUMPCOMPOSITETYPE])
	{
		/*
		 * 设置查询以获取特定于类型的详细信息。
		 * 
		 * 由于我们只想为排序规则与其类型默认值不同的属性
		 * 导出 COLLATE 子句，我们在此使用 CASE 来廉价地抑制
		 * 不感兴趣的 attcollation。对于已删除的列，
		 * atttypid 将为 0；排序规则对这些列无关紧要。
		 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpCompositeType(pg_catalog.oid) AS\n"
							 "SELECT a.attname, a.attnum, "
							 "pg_catalog.format_type(a.atttypid, a.atttypmod) AS atttypdefn, "
							 "a.attlen, a.attalign, a.attisdropped, "
							 "CASE WHEN a.attcollation <> at.typcollation "
							 "THEN a.attcollation ELSE 0 END AS attcollation "
							 "FROM pg_catalog.pg_type ct "
							 "JOIN pg_catalog.pg_attribute a ON a.attrelid = ct.typrelid "
							 "LEFT JOIN pg_catalog.pg_type at ON at.oid = a.atttypid "
							 "WHERE ct.oid = $1 "
							 "ORDER BY a.attnum");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPCOMPOSITETYPE] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpCompositeType('%u')",
					  tyinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_attname = PQfnumber(res, "attname");
	i_atttypdefn = PQfnumber(res, "atttypdefn");
	i_attlen = PQfnumber(res, "attlen");
	i_attalign = PQfnumber(res, "attalign");
	i_attisdropped = PQfnumber(res, "attisdropped");
	i_attcollation = PQfnumber(res, "attcollation");

	if (dopt->binary_upgrade)
	{
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 tyinfo->dobj.catId.oid,
												 false, false);
		binary_upgrade_set_pg_class_oids(fout, q, tyinfo->typrelid, false);
	}

	qtypname = pg_strdup(fmtId(tyinfo->dobj.name));
	qualtypname = pg_strdup(fmtQualifiedDumpable(tyinfo));

	appendPQExpBuffer(q, "CREATE TYPE %s AS (",
					  qualtypname);

	actual_atts = 0;
	for (i = 0; i < ntups; i++)
	{
		char	   *attname;
		char	   *atttypdefn;
		char	   *attlen;
		char	   *attalign;
		bool		attisdropped;
		Oid			attcollation;

		attname = PQgetvalue(res, i, i_attname);
		atttypdefn = PQgetvalue(res, i, i_atttypdefn);
		attlen = PQgetvalue(res, i, i_attlen);
		attalign = PQgetvalue(res, i, i_attalign);
		attisdropped = (PQgetvalue(res, i, i_attisdropped)[0] == 't');
		attcollation = atooid(PQgetvalue(res, i, i_attcollation));

		if (attisdropped && !dopt->binary_upgrade)
			continue;

		/* 如果不是第一个属性则正确格式化 */
		if (actual_atts++ > 0)
			appendPQExpBufferChar(q, ',');
		appendPQExpBufferStr(q, "\n\t");

		if (!attisdropped)
		{
			appendPQExpBuffer(q, "%s %s", fmtId(attname), atttypdefn);

			/* 如果排序规则不是列类型默认值则添加 */
			if (OidIsValid(attcollation))
			{
				CollInfo   *coll;

				coll = findCollationByOid(attcollation);
				if (coll)
					appendPQExpBuffer(q, " COLLATE %s",
									  fmtQualifiedDumpable(coll));
			}
		}
		else
		{
			/*
			 * 这是一个已删除的属性并且我们处于二进制升级模式。
			 * 在 CREATE TYPE 命令中为其插入一个占位符，
			 * 然后在之后通过直接 UPDATE 到目录来设置长度和对齐。
			 * 参见 dumpTableSchema() 中的类似代码。
			 */
			appendPQExpBuffer(q, "%s INTEGER /* dummy */", fmtId(attname));

			/* 单独存储以在 CREATE TYPE 之后插入 */
			appendPQExpBufferStr(dropped,
								 "\n-- For binary upgrade, recreate dropped column.\n");
			appendPQExpBuffer(dropped, "UPDATE pg_catalog.pg_attribute\n"
							  "SET attlen = %s, "
							  "attalign = '%s', attbyval = false\n"
							  "WHERE attname = ", attlen, attalign);
			appendStringLiteralAH(dropped, attname, fout);
			appendPQExpBufferStr(dropped, "\n  AND attrelid = ");
			appendStringLiteralAH(dropped, qualtypname, fout);
			appendPQExpBufferStr(dropped, "::pg_catalog.regclass;\n");

			appendPQExpBuffer(dropped, "ALTER TYPE %s ",
							  qualtypname);
			appendPQExpBuffer(dropped, "DROP ATTRIBUTE %s;\n",
							  fmtId(attname));
		}
	}
	appendPQExpBufferStr(q, "\n);\n");
	appendPQExpBufferStr(q, dropped->data);

	appendPQExpBuffer(delq, "DROP TYPE %s;\n", qualtypname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tyinfo->dobj,
										"TYPE", qtypname,
										tyinfo->dobj.namespace->dobj.name);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tyinfo->dobj.catId, tyinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tyinfo->dobj.name,
								  .namespace = tyinfo->dobj.namespace->dobj.name,
								  .owner = tyinfo->rolname,
								  .description = "TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));


	/* 导出类型注释和安全标签 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TYPE", qtypname,
					tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "TYPE", qtypname,
					 tyinfo->dobj.namespace->dobj.name, tyinfo->rolname,
					 tyinfo->dobj.catId, 0, tyinfo->dobj.dumpId);

	if (tyinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, tyinfo->dobj.dumpId, InvalidDumpId, "TYPE",
				qtypname, NULL,
				tyinfo->dobj.namespace->dobj.name,
				tyinfo->rolname, &tyinfo->dacl);

	/* 导出每列注释 */
	if (tyinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpCompositeTypeColComments(fout, tyinfo, res);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(dropped);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qtypname);
	free(qualtypname);
}

/*
 * dumpCompositeTypeColComments
 *   输出到 fout 以重建用户定义独立复合类型列的注释。
 * 
 * 调用者已执行查询来收集类型列的名称和 attnum，
 * 因此我们仅将该结果传递到这里，而不是重新读取。
 *
 *
 */
static void
dumpCompositeTypeColComments(Archive *fout, const TypeInfo *tyinfo,
							 PGresult *res)
{
	CommentItem *comments;
	int			ncomments;
	PQExpBuffer query;
	PQExpBuffer target;
	int			i;
	int			ntups;
	int			i_attname;
	int			i_attnum;
	int			i_attisdropped;

	/* 如果提供了 --no-comments，则不执行任何操作 */
	if (fout->dopt->no_comments)
		return;

	/* 搜索与类型 pg_class OID 关联的注释 */
	ncomments = findComments(RelationRelationId, tyinfo->typrelid,
							 &comments);

	/* 如果没有注释存在，则完成 */
	if (ncomments <= 0)
		return;

	/* 构建 COMMENT ON 语句 */
	query = createPQExpBuffer();
	target = createPQExpBuffer();

	ntups = PQntuples(res);
	i_attnum = PQfnumber(res, "attnum");
	i_attname = PQfnumber(res, "attname");
	i_attisdropped = PQfnumber(res, "attisdropped");
	while (ncomments > 0)
	{
		const char *attname;

		attname = NULL;
		for (i = 0; i < ntups; i++)
		{
			if (atoi(PQgetvalue(res, i, i_attnum)) == comments->objsubid &&
				PQgetvalue(res, i, i_attisdropped)[0] != 't')
			{
				attname = PQgetvalue(res, i, i_attname);
				break;
			}
		}
		if (attname)			/* 以防我们找不到它 */
		{
			const char *descr = comments->descr;

			resetPQExpBuffer(target);
			appendPQExpBuffer(target, "COLUMN %s.",
							  fmtId(tyinfo->dobj.name));
			appendPQExpBufferStr(target, fmtId(attname));

			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "COMMENT ON COLUMN %s.",
							  fmtQualifiedDumpable(tyinfo));
			appendPQExpBuffer(query, "%s IS ", fmtId(attname));
			appendStringLiteralAH(query, descr, fout);
			appendPQExpBufferStr(query, ";\n");

			ArchiveEntry(fout, nilCatalogId, createDumpId(),
						 ARCHIVE_OPTS(.tag = target->data,
									  .namespace = tyinfo->dobj.namespace->dobj.name,
									  .owner = tyinfo->rolname,
									  .description = "COMMENT",
									  .section = SECTION_NONE,
									  .createStmt = query->data,
									  .deps = &(tyinfo->dobj.dumpId),
									  .nDeps = 1));
		}

		comments++;
		ncomments--;
	}

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
}

/*
 * dumpShellType
 *   输出到 fout 以创建壳类型的查询
 * 
 * 我们在类型的 I/O 函数之前导出一个壳定义。
 */
static void
dumpShellType(Archive *fout, const ShellTypeInfo *stinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();

	/*
	 * 注意壳类型没有 DROP 命令；任何必需的 DROP
	 * 都由基础类型条目驱动。这与 _printTocEntry()
	 * 使用 DROP 命令的存在来决定条目是否需要
	 * ALTER OWNER 命令的方式交互。我们不想在创建时
	 * 立即更改壳类型的所有者；这应该只在它被填充后
	 * 发生，否则后端会报错。
	 */

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_type_oid(fout, q,
												 stinfo->baseType->dobj.catId.oid,
												 false, false);

	appendPQExpBuffer(q, "CREATE TYPE %s;\n",
					  fmtQualifiedDumpable(stinfo));

	if (stinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, stinfo->dobj.catId, stinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = stinfo->dobj.name,
								  .namespace = stinfo->dobj.namespace->dobj.name,
								  .owner = stinfo->baseType->rolname,
								  .description = "SHELL TYPE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data));

	destroyPQExpBuffer(q);
}

/*
 * dumpProcLang
 *   输出到 fout 以重建用户定义过程语言的查询
 *
 */
static void
dumpProcLang(Archive *fout, const ProcLangInfo *plang)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	bool		useParams;
	char	   *qlanname;
	FuncInfo   *funcInfo;
	FuncInfo   *inlineInfo = NULL;
	FuncInfo   *validatorInfo = NULL;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/*
	 * 尝试查找支持函数。找不到它们不是错误 ——
	 * 如果函数在 pg_catalog 模式中（如 8.1 及以上版本的标准），
	 * 则我们不会加载它们。（在这种情况下，我们将发出
	 * 无参数的 CREATE LANGUAGE 命令，这需要后端的 PL
	 * 模板知识来重新加载。）
	 */

	funcInfo = findFuncByOid(plang->lanplcallfoid);
	if (funcInfo != NULL && !funcInfo->dobj.dump)
		funcInfo = NULL;		/* 将未导出的视为与未找到相同 */

	if (OidIsValid(plang->laninline))
	{
		inlineInfo = findFuncByOid(plang->laninline);
		if (inlineInfo != NULL && !inlineInfo->dobj.dump)
			inlineInfo = NULL;
	}

	if (OidIsValid(plang->lanvalidator))
	{
		validatorInfo = findFuncByOid(plang->lanvalidator);
		if (validatorInfo != NULL && !validatorInfo->dobj.dump)
			validatorInfo = NULL;
	}

	/*
	 * 如果函数是可导出的，则发出带有参数的完整
	 * CREATE LANGUAGE。否则，我们将编写无参数命令，
	 * 它将被解释为 CREATE EXTENSION。
	 */
	useParams = (funcInfo != NULL &&
				 (inlineInfo != NULL || !OidIsValid(plang->laninline)) &&
				 (validatorInfo != NULL || !OidIsValid(plang->lanvalidator)));

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();

	qlanname = pg_strdup(fmtId(plang->dobj.name));

	appendPQExpBuffer(delqry, "DROP PROCEDURAL LANGUAGE %s;\n",
					  qlanname);

	if (useParams)
	{
		appendPQExpBuffer(defqry, "CREATE %sPROCEDURAL LANGUAGE %s",
						  plang->lanpltrusted ? "TRUSTED " : "",
						  qlanname);
		appendPQExpBuffer(defqry, " HANDLER %s",
						  fmtQualifiedDumpable(funcInfo));
		if (OidIsValid(plang->laninline))
			appendPQExpBuffer(defqry, " INLINE %s",
							  fmtQualifiedDumpable(inlineInfo));
		if (OidIsValid(plang->lanvalidator))
			appendPQExpBuffer(defqry, " VALIDATOR %s",
							  fmtQualifiedDumpable(validatorInfo));
	}
	else
	{
		/*
		 * 如果不导出参数，则使用 CREATE OR REPLACE，
		 * 以便在目标数据库中预安装了该语言时命令不会失败。
		 * 
		 * 现代服务器会将其解释为 CREATE EXTENSION IF NOT EXISTS；
		 * 也许我们应该改为发出那个？但这可能只会增加混乱。
		 *
		 *
		 */
		appendPQExpBuffer(defqry, "CREATE OR REPLACE PROCEDURAL LANGUAGE %s",
						  qlanname);
	}
	appendPQExpBufferStr(defqry, ";\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(defqry, &plang->dobj,
										"LANGUAGE", qlanname, NULL);

	if (plang->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, plang->dobj.catId, plang->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = plang->dobj.name,
								  .owner = plang->lanowner,
								  .description = "PROCEDURAL LANGUAGE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = defqry->data,
								  .dropStmt = delqry->data,
								  ));

	/* 导出过程语言注释和安全标签 */
	if (plang->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "LANGUAGE", qlanname,
					NULL, plang->lanowner,
					plang->dobj.catId, 0, plang->dobj.dumpId);

	if (plang->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "LANGUAGE", qlanname,
					 NULL, plang->lanowner,
					 plang->dobj.catId, 0, plang->dobj.dumpId);

	if (plang->lanpltrusted && plang->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, plang->dobj.dumpId, InvalidDumpId, "LANGUAGE",
				qlanname, NULL, NULL,
				plang->lanowner, &plang->dacl);

	free(qlanname);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
}

/*
 * format_function_arguments: 生成函数名称和参数列表
 * 
 * 当我们依赖 pg_get_function_arguments 来格式化参数列表时使用。
 * 但请注意，pg_get_function_arguments 不会特殊处理零参数聚合。
 *
 */
static char *
format_function_arguments(const FuncInfo *finfo, const char *funcargs, bool is_agg)
{
	PQExpBufferData fn;

	initPQExpBuffer(&fn);
	appendPQExpBufferStr(&fn, fmtId(finfo->dobj.name));
	if (is_agg && finfo->nargs == 0)
		appendPQExpBufferStr(&fn, "(*)");
	else
		appendPQExpBuffer(&fn, "(%s)", funcargs);
	return fn.data;
}

/*
 * format_function_signature: 生成函数名称和参数列表
 * 
 * 仅生成最小列表的输入参数类型；这足以引用函数，
 * 但不足以定义它。
 * 
 * 如果 honor_quotes 为 false，则函数名从不加引号。
 * 这适用于 TOC 标签，但不适用于 SQL 命令。
 */
static char *
format_function_signature(Archive *fout, const FuncInfo *finfo, bool honor_quotes)
{
	PQExpBufferData fn;
	int			j;

	initPQExpBuffer(&fn);
	if (honor_quotes)
		appendPQExpBuffer(&fn, "%s(", fmtId(finfo->dobj.name));
	else
		appendPQExpBuffer(&fn, "%s(", finfo->dobj.name);
	for (j = 0; j < finfo->nargs; j++)
	{
		if (j > 0)
			appendPQExpBufferStr(&fn, ", ");

		appendPQExpBufferStr(&fn,
							 getFormattedTypeName(fout, finfo->argtypes[j],
												  zeroIsError));
	}
	appendPQExpBufferChar(&fn, ')');
	return fn.data;
}


/*
 * dumpFunc:
 *   导出一个函数
 */
static void
dumpFunc(Archive *fout, const FuncInfo *finfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delqry;
	PQExpBuffer asPart;
	PGresult   *res;
	char	   *funcsig;		/* 身份签名 */
	char	   *funcfullsig = NULL; /* 完整签名 */
	char	   *funcsig_tag;
	char	   *qual_funcsig;
	char	   *proretset;
	char	   *prosrc;
	char	   *probin;
	char	   *prosqlbody;
	char	   *funcargs;
	char	   *funciargs;
	char	   *funcresult;
	char	   *protrftypes;
	char	   *prokind;
	char	   *provolatile;
	char	   *proisstrict;
	char	   *prosecdef;
	char	   *proleakproof;
	char	   *proconfig;
	char	   *procost;
	char	   *prorows;
	char	   *prosupport;
	char	   *proparallel;
	char	   *lanname;
	char	  **configitems = NULL;
	int			nconfigitems = 0;
	const char *keyword;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delqry = createPQExpBuffer();
	asPart = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPFUNC])
	{
		/* 设置查询以获取特定于函数的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpFunc(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query,
							 "SELECT\n"
							 "proretset,\n"
							 "prosrc,\n"
							 "probin,\n"
							 "provolatile,\n"
							 "proisstrict,\n"
							 "prosecdef,\n"
							 "lanname,\n"
							 "proconfig,\n"
							 "procost,\n"
							 "prorows,\n"
							 "pg_catalog.pg_get_function_arguments(p.oid) AS funcargs,\n"
							 "pg_catalog.pg_get_function_identity_arguments(p.oid) AS funciargs,\n"
							 "pg_catalog.pg_get_function_result(p.oid) AS funcresult,\n"
							 "proleakproof,\n");

		if (fout->remoteVersion >= 90500)
			appendPQExpBufferStr(query,
								 "array_to_string(protrftypes, ' ') AS protrftypes,\n");
		else
			appendPQExpBufferStr(query,
								 "NULL AS protrftypes,\n");

		if (fout->remoteVersion >= 90600)
			appendPQExpBufferStr(query,
								 "proparallel,\n");
		else
			appendPQExpBufferStr(query,
								 "'u' AS proparallel,\n");

		if (fout->remoteVersion >= 110000)
			appendPQExpBufferStr(query,
								 "prokind,\n");
		else
			appendPQExpBufferStr(query,
								 "CASE WHEN proiswindow THEN 'w' ELSE 'f' END AS prokind,\n");

		if (fout->remoteVersion >= 120000)
			appendPQExpBufferStr(query,
								 "prosupport,\n");
		else
			appendPQExpBufferStr(query,
								 "'-' AS prosupport,\n");

		if (fout->remoteVersion >= 140000)
			appendPQExpBufferStr(query,
								 "pg_get_function_sqlbody(p.oid) AS prosqlbody\n");
		else
			appendPQExpBufferStr(query,
								 "NULL AS prosqlbody\n");

		appendPQExpBufferStr(query,
							 "FROM pg_catalog.pg_proc p, pg_catalog.pg_language l\n"
							 "WHERE p.oid = $1 "
							 "AND l.oid = p.prolang");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPFUNC] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpFunc('%u')",
					  finfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	proretset = PQgetvalue(res, 0, PQfnumber(res, "proretset"));
	if (PQgetisnull(res, 0, PQfnumber(res, "prosqlbody")))
	{
		prosrc = PQgetvalue(res, 0, PQfnumber(res, "prosrc"));
		probin = PQgetvalue(res, 0, PQfnumber(res, "probin"));
		prosqlbody = NULL;
	}
	else
	{
		prosrc = NULL;
		probin = NULL;
		prosqlbody = PQgetvalue(res, 0, PQfnumber(res, "prosqlbody"));
	}
	funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
	funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
	funcresult = PQgetvalue(res, 0, PQfnumber(res, "funcresult"));
	protrftypes = PQgetvalue(res, 0, PQfnumber(res, "protrftypes"));
	prokind = PQgetvalue(res, 0, PQfnumber(res, "prokind"));
	provolatile = PQgetvalue(res, 0, PQfnumber(res, "provolatile"));
	proisstrict = PQgetvalue(res, 0, PQfnumber(res, "proisstrict"));
	prosecdef = PQgetvalue(res, 0, PQfnumber(res, "prosecdef"));
	proleakproof = PQgetvalue(res, 0, PQfnumber(res, "proleakproof"));
	proconfig = PQgetvalue(res, 0, PQfnumber(res, "proconfig"));
	procost = PQgetvalue(res, 0, PQfnumber(res, "procost"));
	prorows = PQgetvalue(res, 0, PQfnumber(res, "prorows"));
	prosupport = PQgetvalue(res, 0, PQfnumber(res, "prosupport"));
	proparallel = PQgetvalue(res, 0, PQfnumber(res, "proparallel"));
	lanname = PQgetvalue(res, 0, PQfnumber(res, "lanname"));

	/*
	 * 有关 'AS' 子句如何使用的详细信息，
	 * 请参见 backend/commands/functioncmds.c。
	 */
	if (prosqlbody)
	{
		appendPQExpBufferStr(asPart, prosqlbody);
	}
	else if (probin[0] != '\0')
	{
		appendPQExpBufferStr(asPart, "AS ");
		appendStringLiteralAH(asPart, probin, fout);
		if (prosrc[0] != '\0')
		{
			appendPQExpBufferStr(asPart, ", ");

			/*
			 * 在有 bin 的情况下，如果允许且 src 包含引号或反斜杠，
			 * 则使用美元引号；否则使用常规引号。
			 */
			if (dopt->disable_dollar_quoting ||
				(strchr(prosrc, '\'') == NULL && strchr(prosrc, '\\') == NULL))
				appendStringLiteralAH(asPart, prosrc, fout);
			else
				appendStringLiteralDQ(asPart, prosrc, NULL);
		}
	}
	else
	{
		appendPQExpBufferStr(asPart, "AS ");
		/* 没有 bin 时，如果允许则无条件使用美元引号 */
		if (dopt->disable_dollar_quoting)
			appendStringLiteralAH(asPart, prosrc, fout);
		else
			appendStringLiteralDQ(asPart, prosrc, NULL);
	}

	if (*proconfig)
	{
		if (!parsePGArray(proconfig, &configitems, &nconfigitems))
			pg_fatal("could not parse %s array", "proconfig");
	}
	else
	{
		configitems = NULL;
		nconfigitems = 0;
	}

	funcfullsig = format_function_arguments(finfo, funcargs, false);
	funcsig = format_function_arguments(finfo, funciargs, false);

	funcsig_tag = format_function_signature(fout, finfo, false);

	qual_funcsig = psprintf("%s.%s",
							fmtId(finfo->dobj.namespace->dobj.name),
							funcsig);

	if (prokind[0] == PROKIND_PROCEDURE)
		keyword = "PROCEDURE";
	else
		keyword = "FUNCTION";	/* 也适用于窗口函数 */

	appendPQExpBuffer(delqry, "DROP %s %s;\n",
					  keyword, qual_funcsig);

	appendPQExpBuffer(q, "CREATE %s %s.%s",
					  keyword,
					  fmtId(finfo->dobj.namespace->dobj.name),
					  funcfullsig ? funcfullsig :
					  funcsig);

	if (prokind[0] == PROKIND_PROCEDURE)
		 /* 没有结果类型可输出 */ ;
	else if (funcresult)
		appendPQExpBuffer(q, " RETURNS %s", funcresult);
	else
		appendPQExpBuffer(q, " RETURNS %s%s",
						  (proretset[0] == 't') ? "SETOF " : "",
						  getFormattedTypeName(fout, finfo->prorettype,
											   zeroIsError));

	appendPQExpBuffer(q, "\n    LANGUAGE %s", fmtId(lanname));

	if (*protrftypes)
	{
		Oid		   *typeids = palloc(FUNC_MAX_ARGS * sizeof(Oid));
		int			i;

		appendPQExpBufferStr(q, " TRANSFORM ");
		parseOidArray(protrftypes, typeids, FUNC_MAX_ARGS);
		for (i = 0; typeids[i]; i++)
		{
			if (i != 0)
				appendPQExpBufferStr(q, ", ");
			appendPQExpBuffer(q, "FOR TYPE %s",
							  getFormattedTypeName(fout, typeids[i], zeroAsNone));
		}
	}

	if (prokind[0] == PROKIND_WINDOW)
		appendPQExpBufferStr(q, " WINDOW");

	if (provolatile[0] != PROVOLATILE_VOLATILE)
	{
		if (provolatile[0] == PROVOLATILE_IMMUTABLE)
			appendPQExpBufferStr(q, " IMMUTABLE");
		else if (provolatile[0] == PROVOLATILE_STABLE)
			appendPQExpBufferStr(q, " STABLE");
		else if (provolatile[0] != PROVOLATILE_VOLATILE)
			pg_fatal("unrecognized provolatile value for function \"%s\"",
					 finfo->dobj.name);
	}

	if (proisstrict[0] == 't')
		appendPQExpBufferStr(q, " STRICT");

	if (prosecdef[0] == 't')
		appendPQExpBufferStr(q, " SECURITY DEFINER");

	if (proleakproof[0] == 't')
		appendPQExpBufferStr(q, " LEAKPROOF");

	/*
	 * COST 和 ROWS 仅在存在且非默认值时才发出，
	 * 以避免不必要地破坏导出的向后兼容性。
	 * 请将此代码与 functioncmds.c 中的默认值保持同步。
	 */
	if (strcmp(procost, "0") != 0)
	{
		if (strcmp(lanname, "internal") == 0 || strcmp(lanname, "c") == 0)
		{
			/* 默认成本为 1 */
			if (strcmp(procost, "1") != 0)
				appendPQExpBuffer(q, " COST %s", procost);
		}
		else
		{
			/* 默认成本为 100 */
			if (strcmp(procost, "100") != 0)
				appendPQExpBuffer(q, " COST %s", procost);
		}
	}
	if (proretset[0] == 't' &&
		strcmp(prorows, "0") != 0 && strcmp(prorows, "1000") != 0)
		appendPQExpBuffer(q, " ROWS %s", prorows);

	if (strcmp(prosupport, "-") != 0)
	{
		/* 我们依赖 regprocout 来提供引号和限定 */
		appendPQExpBuffer(q, " SUPPORT %s", prosupport);
	}

	if (proparallel[0] != PROPARALLEL_UNSAFE)
	{
		if (proparallel[0] == PROPARALLEL_SAFE)
			appendPQExpBufferStr(q, " PARALLEL SAFE");
		else if (proparallel[0] == PROPARALLEL_RESTRICTED)
			appendPQExpBufferStr(q, " PARALLEL RESTRICTED");
		else if (proparallel[0] != PROPARALLEL_UNSAFE)
			pg_fatal("unrecognized proparallel value for function \"%s\"",
					 finfo->dobj.name);
	}

	for (int i = 0; i < nconfigitems; i++)
	{
		/* 我们可以在此随意修改 configitems[] */
		char	   *configitem = configitems[i];
		char	   *pos;

		pos = strchr(configitem, '=');
		if (pos == NULL)
			continue;
		*pos++ = '\0';
		appendPQExpBuffer(q, "\n    SET %s TO ", fmtId(configitem));

		/*
		 * 标记为 GUC_LIST_QUOTE 的变量在被放入 proconfig 数组之前
		 * 已由 flatten_set_variable_args() 完全引用。但是，由于
		 * 那里使用的引用规则与 SQL 不完全相同，我们必须将列表值
		 * 拆分开，然后将元素作为字符串字面量引用。（元素可以
		 * 原样双引号引用，但我们不能直接将其输入 SQL 解析器；
		 * 对于长度为零或超过 NAMEDATALEN 的元素，它会做错误的事情。）
		 * 
		 * 未标记的变量应该作为简单的字符串字面量发出。如果变量
		 * 不被 variable_is_guc_list_quote() 所知，我们会这样做；
		 * 这使得对扩展变量使用 GUC_LIST_QUOTE 变得不安全。
		 *
		 *
		 *
		 */
		if (variable_is_guc_list_quote(configitem))
		{
			char	  **namelist;
			char	  **nameptr;

			/* 将字符串解析为标识符列表 */
			/* 这实际上不应该失败 */
			if (SplitGUCList(pos, ',', &namelist))
			{
				for (nameptr = namelist; *nameptr; nameptr++)
				{
					if (nameptr != namelist)
						appendPQExpBufferStr(q, ", ");
					appendStringLiteralAH(q, *nameptr, fout);
				}
			}
			pg_free(namelist);
		}
		else
			appendStringLiteralAH(q, pos, fout);
	}

	appendPQExpBuffer(q, "\n    %s;\n", asPart->data);

	append_depends_on_extension(fout, q, &finfo->dobj,
								"pg_catalog.pg_proc", keyword,
								qual_funcsig);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &finfo->dobj,
										keyword, funcsig,
										finfo->dobj.namespace->dobj.name);

	if (finfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, finfo->dobj.catId, finfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = funcsig_tag,
								  .namespace = finfo->dobj.namespace->dobj.name,
								  .owner = finfo->rolname,
								  .description = keyword,
								  .section = finfo->postponed_def ?
								  SECTION_POST_DATA : SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delqry->data));

	/* 导出函数注释和安全标签 */
	if (finfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, keyword, funcsig,
					finfo->dobj.namespace->dobj.name, finfo->rolname,
					finfo->dobj.catId, 0, finfo->dobj.dumpId);

	if (finfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, keyword, funcsig,
					 finfo->dobj.namespace->dobj.name, finfo->rolname,
					 finfo->dobj.catId, 0, finfo->dobj.dumpId);

	if (finfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, finfo->dobj.dumpId, InvalidDumpId, keyword,
				funcsig, NULL,
				finfo->dobj.namespace->dobj.name,
				finfo->rolname, &finfo->dacl);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(asPart);
	free(funcsig);
	free(funcfullsig);
	free(funcsig_tag);
	free(qual_funcsig);
	free(configitems);
}


/*
 * 导出一个用户定义的类型转换
 */
static void
dumpCast(Archive *fout, const CastInfo *cast)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer castargs;
	FuncInfo   *funcInfo = NULL;
	const char *sourceType;
	const char *targetType;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/* 如果没有类型转换函数的信息则无法导出 */
	if (OidIsValid(cast->castfunc))
	{
		funcInfo = findFuncByOid(cast->castfunc);
		if (funcInfo == NULL)
			pg_fatal("could not find function definition for function with OID %u",
					 cast->castfunc);
	}

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();
	castargs = createPQExpBuffer();

	sourceType = getFormattedTypeName(fout, cast->castsource, zeroAsNone);
	targetType = getFormattedTypeName(fout, cast->casttarget, zeroAsNone);
	appendPQExpBuffer(delqry, "DROP CAST (%s AS %s);\n",
					  sourceType, targetType);

	appendPQExpBuffer(defqry, "CREATE CAST (%s AS %s) ",
					  sourceType, targetType);

	switch (cast->castmethod)
	{
		case COERCION_METHOD_BINARY:
			appendPQExpBufferStr(defqry, "WITHOUT FUNCTION");
			break;
		case COERCION_METHOD_INOUT:
			appendPQExpBufferStr(defqry, "WITH INOUT");
			break;
		case COERCION_METHOD_FUNCTION:
			if (funcInfo)
			{
				char	   *fsig = format_function_signature(fout, funcInfo, true);

				/*
				 * 始终限定函数名（format_function_signature 不会限定它）。
				 *
				 */
				appendPQExpBuffer(defqry, "WITH FUNCTION %s.%s",
								  fmtId(funcInfo->dobj.namespace->dobj.name), fsig);
				free(fsig);
			}
			else
				pg_log_warning("bogus value in pg_cast.castfunc or pg_cast.castmethod field");
			break;
		default:
			pg_log_warning("bogus value in pg_cast.castmethod field");
	}

	if (cast->castcontext == 'a')
		appendPQExpBufferStr(defqry, " AS ASSIGNMENT");
	else if (cast->castcontext == 'i')
		appendPQExpBufferStr(defqry, " AS IMPLICIT");
	appendPQExpBufferStr(defqry, ";\n");

	appendPQExpBuffer(labelq, "CAST (%s AS %s)",
					  sourceType, targetType);

	appendPQExpBuffer(castargs, "(%s AS %s)",
					  sourceType, targetType);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(defqry, &cast->dobj,
										"CAST", castargs->data, NULL);

	if (cast->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, cast->dobj.catId, cast->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = labelq->data,
								  .description = "CAST",
								  .section = SECTION_PRE_DATA,
								  .createStmt = defqry->data,
								  .dropStmt = delqry->data));

	/* 导出类型转换注释 */
	if (cast->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "CAST", castargs->data,
					NULL, "",
					cast->dobj.catId, 0, cast->dobj.dumpId);

	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(castargs);
}

/*
 * 导出一个转换
 */
static void
dumpTransform(Archive *fout, const TransformInfo *transform)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer defqry;
	PQExpBuffer delqry;
	PQExpBuffer labelq;
	PQExpBuffer transformargs;
	FuncInfo   *fromsqlFuncInfo = NULL;
	FuncInfo   *tosqlFuncInfo = NULL;
	char	   *lanname;
	const char *transformType;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/* 如果没有转换函数的信息则无法导出 */
	if (OidIsValid(transform->trffromsql))
	{
		fromsqlFuncInfo = findFuncByOid(transform->trffromsql);
		if (fromsqlFuncInfo == NULL)
			pg_fatal("could not find function definition for function with OID %u",
					 transform->trffromsql);
	}
	if (OidIsValid(transform->trftosql))
	{
		tosqlFuncInfo = findFuncByOid(transform->trftosql);
		if (tosqlFuncInfo == NULL)
			pg_fatal("could not find function definition for function with OID %u",
					 transform->trftosql);
	}

	defqry = createPQExpBuffer();
	delqry = createPQExpBuffer();
	labelq = createPQExpBuffer();
	transformargs = createPQExpBuffer();

	lanname = get_language_name(fout, transform->trflang);
	transformType = getFormattedTypeName(fout, transform->trftype, zeroAsNone);

	appendPQExpBuffer(delqry, "DROP TRANSFORM FOR %s LANGUAGE %s;\n",
					  transformType, lanname);

	appendPQExpBuffer(defqry, "CREATE TRANSFORM FOR %s LANGUAGE %s (",
					  transformType, lanname);

	if (!transform->trffromsql && !transform->trftosql)
		pg_log_warning("bogus transform definition, at least one of trffromsql and trftosql should be nonzero");

	if (transform->trffromsql)
	{
		if (fromsqlFuncInfo)
		{
			char	   *fsig = format_function_signature(fout, fromsqlFuncInfo, true);

			/*
			 * 始终限定函数名（format_function_signature 不会限定它）。
			 *
			 */
			appendPQExpBuffer(defqry, "FROM SQL WITH FUNCTION %s.%s",
							  fmtId(fromsqlFuncInfo->dobj.namespace->dobj.name), fsig);
			free(fsig);
		}
		else
			pg_log_warning("bogus value in pg_transform.trffromsql field");
	}

	if (transform->trftosql)
	{
		if (transform->trffromsql)
			appendPQExpBufferStr(defqry, ", ");

		if (tosqlFuncInfo)
		{
			char	   *fsig = format_function_signature(fout, tosqlFuncInfo, true);

			/*
			 * 始终限定函数名（format_function_signature 不会限定它）。
			 *
			 */
			appendPQExpBuffer(defqry, "TO SQL WITH FUNCTION %s.%s",
							  fmtId(tosqlFuncInfo->dobj.namespace->dobj.name), fsig);
			free(fsig);
		}
		else
			pg_log_warning("bogus value in pg_transform.trftosql field");
	}

	appendPQExpBufferStr(defqry, ");\n");

	appendPQExpBuffer(labelq, "TRANSFORM FOR %s LANGUAGE %s",
					  transformType, lanname);

	appendPQExpBuffer(transformargs, "FOR %s LANGUAGE %s",
					  transformType, lanname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(defqry, &transform->dobj,
										"TRANSFORM", transformargs->data, NULL);

	if (transform->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, transform->dobj.catId, transform->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = labelq->data,
								  .description = "TRANSFORM",
								  .section = SECTION_PRE_DATA,
								  .createStmt = defqry->data,
								  .dropStmt = delqry->data,
								  .deps = transform->dobj.dependencies,
								  .nDeps = transform->dobj.nDeps));

	/* 导出转换注释 */
	if (transform->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TRANSFORM", transformargs->data,
					NULL, "",
					transform->dobj.catId, 0, transform->dobj.dumpId);

	free(lanname);
	destroyPQExpBuffer(defqry);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(labelq);
	destroyPQExpBuffer(transformargs);
}


/*
 * dumpOpr
 *   写出单个操作符定义
 */
static void
dumpOpr(Archive *fout, const OprInfo *oprinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer oprid;
	PQExpBuffer details;
	PGresult   *res;
	int			i_oprkind;
	int			i_oprcode;
	int			i_oprleft;
	int			i_oprright;
	int			i_oprcom;
	int			i_oprnegate;
	int			i_oprrest;
	int			i_oprjoin;
	int			i_oprcanmerge;
	int			i_oprcanhash;
	char	   *oprkind;
	char	   *oprcode;
	char	   *oprleft;
	char	   *oprright;
	char	   *oprcom;
	char	   *oprnegate;
	char	   *oprrest;
	char	   *oprjoin;
	char	   *oprcanmerge;
	char	   *oprcanhash;
	char	   *oprregproc;
	char	   *oprref;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/*
	 * 某些操作符无效，因为它们是用户在换向器存在之前
	 * 定义操作符的结果
	 */
	if (!OidIsValid(oprinfo->oprcode))
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	oprid = createPQExpBuffer();
	details = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPOPR])
	{
		/* 设置查询以获取特定于操作符的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpOpr(pg_catalog.oid) AS\n"
							 "SELECT oprkind, "
							 "oprcode::pg_catalog.regprocedure, "
							 "oprleft::pg_catalog.regtype, "
							 "oprright::pg_catalog.regtype, "
							 "oprcom, "
							 "oprnegate, "
							 "oprrest::pg_catalog.regprocedure, "
							 "oprjoin::pg_catalog.regprocedure, "
							 "oprcanmerge, oprcanhash "
							 "FROM pg_catalog.pg_operator "
							 "WHERE oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPOPR] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpOpr('%u')",
					  oprinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_oprkind = PQfnumber(res, "oprkind");
	i_oprcode = PQfnumber(res, "oprcode");
	i_oprleft = PQfnumber(res, "oprleft");
	i_oprright = PQfnumber(res, "oprright");
	i_oprcom = PQfnumber(res, "oprcom");
	i_oprnegate = PQfnumber(res, "oprnegate");
	i_oprrest = PQfnumber(res, "oprrest");
	i_oprjoin = PQfnumber(res, "oprjoin");
	i_oprcanmerge = PQfnumber(res, "oprcanmerge");
	i_oprcanhash = PQfnumber(res, "oprcanhash");

	oprkind = PQgetvalue(res, 0, i_oprkind);
	oprcode = PQgetvalue(res, 0, i_oprcode);
	oprleft = PQgetvalue(res, 0, i_oprleft);
	oprright = PQgetvalue(res, 0, i_oprright);
	oprcom = PQgetvalue(res, 0, i_oprcom);
	oprnegate = PQgetvalue(res, 0, i_oprnegate);
	oprrest = PQgetvalue(res, 0, i_oprrest);
	oprjoin = PQgetvalue(res, 0, i_oprjoin);
	oprcanmerge = PQgetvalue(res, 0, i_oprcanmerge);
	oprcanhash = PQgetvalue(res, 0, i_oprcanhash);

	/* 在 PG14 及以上版本中，后缀操作符支持不再存在。 */
	if (strcmp(oprkind, "r") == 0)
		pg_log_warning("postfix operators are not supported anymore (operator \"%s\")",
					   oprcode);

	oprregproc = convertRegProcReference(oprcode);
	if (oprregproc)
	{
		appendPQExpBuffer(details, "    FUNCTION = %s", oprregproc);
		free(oprregproc);
	}

	appendPQExpBuffer(oprid, "%s (",
					  oprinfo->dobj.name);

	/*
	 * 右一元意味着有左参数，左一元意味着有右参数。
	 * （尽管 "r" 情况对于 PG14 及更高版本是死代码，
	 * 但继续支持它，以防我们从旧服务器导出。）
	 */
	if (strcmp(oprkind, "r") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		appendPQExpBuffer(details, ",\n    LEFTARG = %s", oprleft);
		appendPQExpBufferStr(oprid, oprleft);
	}
	else
		appendPQExpBufferStr(oprid, "NONE");

	if (strcmp(oprkind, "l") == 0 ||
		strcmp(oprkind, "b") == 0)
	{
		appendPQExpBuffer(details, ",\n    RIGHTARG = %s", oprright);
		appendPQExpBuffer(oprid, ", %s)", oprright);
	}
	else
		appendPQExpBufferStr(oprid, ", NONE)");

	oprref = getFormattedOperatorName(oprcom);
	if (oprref)
	{
		appendPQExpBuffer(details, ",\n    COMMUTATOR = %s", oprref);
		free(oprref);
	}

	oprref = getFormattedOperatorName(oprnegate);
	if (oprref)
	{
		appendPQExpBuffer(details, ",\n    NEGATOR = %s", oprref);
		free(oprref);
	}

	if (strcmp(oprcanmerge, "t") == 0)
		appendPQExpBufferStr(details, ",\n    MERGES");

	if (strcmp(oprcanhash, "t") == 0)
		appendPQExpBufferStr(details, ",\n    HASHES");

	oprregproc = convertRegProcReference(oprrest);
	if (oprregproc)
	{
		appendPQExpBuffer(details, ",\n    RESTRICT = %s", oprregproc);
		free(oprregproc);
	}

	oprregproc = convertRegProcReference(oprjoin);
	if (oprregproc)
	{
		appendPQExpBuffer(details, ",\n    JOIN = %s", oprregproc);
		free(oprregproc);
	}

	appendPQExpBuffer(delq, "DROP OPERATOR %s.%s;\n",
					  fmtId(oprinfo->dobj.namespace->dobj.name),
					  oprid->data);

	appendPQExpBuffer(q, "CREATE OPERATOR %s.%s (\n%s\n);\n",
					  fmtId(oprinfo->dobj.namespace->dobj.name),
					  oprinfo->dobj.name, details->data);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &oprinfo->dobj,
										"OPERATOR", oprid->data,
										oprinfo->dobj.namespace->dobj.name);

	if (oprinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, oprinfo->dobj.catId, oprinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = oprinfo->dobj.name,
								  .namespace = oprinfo->dobj.namespace->dobj.name,
								  .owner = oprinfo->rolname,
								  .description = "OPERATOR",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出操作符注释 */
	if (oprinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "OPERATOR", oprid->data,
					oprinfo->dobj.namespace->dobj.name, oprinfo->rolname,
					oprinfo->dobj.catId, 0, oprinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(oprid);
	destroyPQExpBuffer(details);
}

/*
 * 转换从 pg_operator 获取的函数引用
 * 
 * 返回分配的要输出内容的字符串，如果函数引用
 * 是 InvalidOid 则返回 NULL。返回的字符串应由调用者释放。
 * 
 * 输入是 REGPROCEDURE 显示；我们必须去除参数类型部分。
 *
 */
static char *
convertRegProcReference(const char *proc)
{
	char	   *name;
	char	   *paren;
	bool		inquote;

	/* 在所有情况下 "-" 都表示空引用 */
	if (strcmp(proc, "-") == 0)
		return NULL;

	name = pg_strdup(proc);
	/* 查找非双引号的左括号 */
	inquote = false;
	for (paren = name; *paren; paren++)
	{
		if (*paren == '(' && !inquote)
		{
			*paren = '\0';
			break;
		}
		if (*paren == '"')
			inquote = !inquote;
	}
	return name;
}

/*
 * getFormattedOperatorName - 为给定的操作符 OID
 * （以字符串形式表示）检索操作符名称。
 * 
 * 返回分配的字符串，如果给定的 OID 无效则返回 NULL。
 * 调用者负责释放结果字符串。
 * 
 * 我们生成的内容具有格式 "OPERATOR(schema.oprname)"。
 * 这仅在可以从上下文推断操作符参数类型的命令中有用。
 * 我们始终对名称进行模式限定。此代码的前身尝试
 * 在可能时跳过模式限定，但这在边界情况下导致错误结果，
 * 例如操作符和其否定器位于不同的模式中。
 *
 */
static char *
getFormattedOperatorName(const char *oproid)
{
	OprInfo    *oprInfo;

	/* 在所有情况下 "0" 都表示空引用 */
	if (strcmp(oproid, "0") == 0)
		return NULL;

	oprInfo = findOprByOid(atooid(oproid));
	if (oprInfo == NULL)
	{
		pg_log_warning("could not find operator with OID %s",
					   oproid);
		return NULL;
	}

	return psprintf("OPERATOR(%s.%s)",
					fmtId(oprInfo->dobj.namespace->dobj.name),
					oprInfo->dobj.name);
}

/*
 * 转换从 pg_ts_parser 或 pg_ts_template 获取的函数 OID
 * 
 * 使用 REGPROC 而不是 REGPROCEDURE 就足够了，因为这些
 * 函数的参数列表是预定的。请注意，调用者应确保
 * 我们在正确的模式中，因为结果是依赖于搜索路径的！
 *
 */
static char *
convertTSFunction(Archive *fout, Oid funcOid)
{
	char	   *result;
	char		query[128];
	PGresult   *res;

	snprintf(query, sizeof(query),
			 "SELECT '%u'::pg_catalog.regproc", funcOid);
	res = ExecuteSqlQueryForSingleRow(fout, query);

	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);

	return result;
}

/*
 * dumpAccessMethod
 *   写出单个访问方法定义
 */
static void
dumpAccessMethod(Archive *fout, const AccessMethodInfo *aminfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qamname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qamname = pg_strdup(fmtId(aminfo->dobj.name));

	appendPQExpBuffer(q, "CREATE ACCESS METHOD %s ", qamname);

	switch (aminfo->amtype)
	{
		case AMTYPE_INDEX:
			appendPQExpBufferStr(q, "TYPE INDEX ");
			break;
		case AMTYPE_TABLE:
			appendPQExpBufferStr(q, "TYPE TABLE ");
			break;
		default:
			pg_log_warning("invalid type \"%c\" of access method \"%s\"",
						   aminfo->amtype, qamname);
			destroyPQExpBuffer(q);
			destroyPQExpBuffer(delq);
			free(qamname);
			return;
	}

	appendPQExpBuffer(q, "HANDLER %s;\n", aminfo->amhandler);

	appendPQExpBuffer(delq, "DROP ACCESS METHOD %s;\n",
					  qamname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &aminfo->dobj,
										"ACCESS METHOD", qamname, NULL);

	if (aminfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, aminfo->dobj.catId, aminfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = aminfo->dobj.name,
								  .description = "ACCESS METHOD",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出访问方法注释 */
	if (aminfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "ACCESS METHOD", qamname,
					NULL, "",
					aminfo->dobj.catId, 0, aminfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qamname);
}

/*
 * dumpOpclass
 *   写出单个操作符类定义
 */
static void
dumpOpclass(Archive *fout, const OpclassInfo *opcinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer nameusing;
	PGresult   *res;
	int			ntups;
	int			i_opcintype;
	int			i_opckeytype;
	int			i_opcdefault;
	int			i_opcfamily;
	int			i_opcfamilyname;
	int			i_opcfamilynsp;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopopr;
	int			i_sortfamily;
	int			i_sortfamilynsp;
	int			i_amprocnum;
	int			i_amproc;
	int			i_amproclefttype;
	int			i_amprocrighttype;
	char	   *opcintype;
	char	   *opckeytype;
	char	   *opcdefault;
	char	   *opcfamily;
	char	   *opcfamilyname;
	char	   *opcfamilynsp;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopopr;
	char	   *sortfamily;
	char	   *sortfamilynsp;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	nameusing = createPQExpBuffer();

	/* 从 pg_opclass 行获取附加字段 */
	appendPQExpBuffer(query, "SELECT opcintype::pg_catalog.regtype, "
					  "opckeytype::pg_catalog.regtype, "
					  "opcdefault, opcfamily, "
					  "opfname AS opcfamilyname, "
					  "nspname AS opcfamilynsp, "
					  "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opcmethod) AS amname "
					  "FROM pg_catalog.pg_opclass c "
					  "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = opcfamily "
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  opcinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_opcintype = PQfnumber(res, "opcintype");
	i_opckeytype = PQfnumber(res, "opckeytype");
	i_opcdefault = PQfnumber(res, "opcdefault");
	i_opcfamily = PQfnumber(res, "opcfamily");
	i_opcfamilyname = PQfnumber(res, "opcfamilyname");
	i_opcfamilynsp = PQfnumber(res, "opcfamilynsp");
	i_amname = PQfnumber(res, "amname");

	/* 在 PQclear res 之后可能仍需要 opcintype */
	opcintype = pg_strdup(PQgetvalue(res, 0, i_opcintype));
	opckeytype = PQgetvalue(res, 0, i_opckeytype);
	opcdefault = PQgetvalue(res, 0, i_opcdefault);
	/* 在 PQclear res 之后仍需要 opcfamily */
	opcfamily = pg_strdup(PQgetvalue(res, 0, i_opcfamily));
	opcfamilyname = PQgetvalue(res, 0, i_opcfamilyname);
	opcfamilynsp = PQgetvalue(res, 0, i_opcfamilynsp);
	/* 在 PQclear res 之后仍需要 amname */
	amname = pg_strdup(PQgetvalue(res, 0, i_amname));

	appendPQExpBuffer(delq, "DROP OPERATOR CLASS %s",
					  fmtQualifiedDumpable(opcinfo));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* 构建 CREATE 命令的固定部分 */
	appendPQExpBuffer(q, "CREATE OPERATOR CLASS %s\n    ",
					  fmtQualifiedDumpable(opcinfo));
	if (strcmp(opcdefault, "t") == 0)
		appendPQExpBufferStr(q, "DEFAULT ");
	appendPQExpBuffer(q, "FOR TYPE %s USING %s",
					  opcintype,
					  fmtId(amname));
	if (strlen(opcfamilyname) > 0)
	{
		appendPQExpBufferStr(q, " FAMILY ");
		appendPQExpBuffer(q, "%s.", fmtId(opcfamilynsp));
		appendPQExpBufferStr(q, fmtId(opcfamilyname));
	}
	appendPQExpBufferStr(q, " AS\n    ");

	needComma = false;

	if (strcmp(opckeytype, "-") != 0)
	{
		appendPQExpBuffer(q, "STORAGE %s",
						  opckeytype);
		needComma = true;
	}

	PQclear(res);

	/*
	 * 现在获取并输出 OPERATOR 条目（pg_amop 行）。
	 * 
	 * 仅输出那些通过 pg_depend 条目绑定到操作符类的
	 * 操作符族成员。
	 */
	resetPQExpBuffer(query);
	appendPQExpBuffer(query, "SELECT amopstrategy, "
					  "amopopr::pg_catalog.regoperator, "
					  "opfname AS sortfamily, "
					  "nspname AS sortfamilynsp "
					  "FROM pg_catalog.pg_amop ao JOIN pg_catalog.pg_depend ON "
					  "(classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND objid = ao.oid) "
					  "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = amopsortfamily "
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
					  "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
					  "AND amopfamily = '%s'::pg_catalog.oid "
					  "ORDER BY amopstrategy",
					  opcinfo->dobj.catId.oid,
					  opcfamily);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amopstrategy = PQfnumber(res, "amopstrategy");
	i_amopopr = PQfnumber(res, "amopopr");
	i_sortfamily = PQfnumber(res, "sortfamily");
	i_sortfamilynsp = PQfnumber(res, "sortfamilynsp");

	for (i = 0; i < ntups; i++)
	{
		amopstrategy = PQgetvalue(res, i, i_amopstrategy);
		amopopr = PQgetvalue(res, i, i_amopopr);
		sortfamily = PQgetvalue(res, i, i_sortfamily);
		sortfamilynsp = PQgetvalue(res, i, i_sortfamilynsp);

		if (needComma)
			appendPQExpBufferStr(q, " ,\n    ");

		appendPQExpBuffer(q, "OPERATOR %s %s",
						  amopstrategy, amopopr);

		if (strlen(sortfamily) > 0)
		{
			appendPQExpBufferStr(q, " FOR ORDER BY ");
			appendPQExpBuffer(q, "%s.", fmtId(sortfamilynsp));
			appendPQExpBufferStr(q, fmtId(sortfamily));
		}

		needComma = true;
	}

	PQclear(res);

	/*
	 * 现在获取并输出 FUNCTION 条目（pg_amproc 行）。
	 * 
	 * 仅输出那些通过 pg_depend 条目绑定到操作符类的
	 * 操作符族成员。
	 * 
	 * 我们输出 amproclefttype/amprocrighttype，即使在大多数情况下
	 * 后端可以推导出正确的值，因为存在跨类型比较的 btree
	 * 排序支持函数的边界情况。
	 */
	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT amprocnum, "
					  "amproc::pg_catalog.regprocedure, "
					  "amproclefttype::pg_catalog.regtype, "
					  "amprocrighttype::pg_catalog.regtype "
					  "FROM pg_catalog.pg_amproc ap, pg_catalog.pg_depend "
					  "WHERE refclassid = 'pg_catalog.pg_opclass'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
					  "AND classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass "
					  "AND objid = ap.oid "
					  "ORDER BY amprocnum",
					  opcinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_amprocnum = PQfnumber(res, "amprocnum");
	i_amproc = PQfnumber(res, "amproc");
	i_amproclefttype = PQfnumber(res, "amproclefttype");
	i_amprocrighttype = PQfnumber(res, "amprocrighttype");

	for (i = 0; i < ntups; i++)
	{
		amprocnum = PQgetvalue(res, i, i_amprocnum);
		amproc = PQgetvalue(res, i, i_amproc);
		amproclefttype = PQgetvalue(res, i, i_amproclefttype);
		amprocrighttype = PQgetvalue(res, i, i_amprocrighttype);

		if (needComma)
			appendPQExpBufferStr(q, " ,\n    ");

		appendPQExpBuffer(q, "FUNCTION %s", amprocnum);

		if (*amproclefttype && *amprocrighttype)
			appendPQExpBuffer(q, " (%s, %s)", amproclefttype, amprocrighttype);

		appendPQExpBuffer(q, " %s", amproc);

		needComma = true;
	}

	PQclear(res);

	/*
	 * 如果 needComma 仍为 false，意味着我们在 AS 关键字之后
	 * 没有添加任何内容。为避免输出损坏的 SQL，添加一个具有
	 * 相同数据类型的虚拟 STORAGE 子句。文档中没有规定这一点，
	 * 但实际上 DefineOpClass 会将其视为无操作。
	 */
	if (!needComma)
		appendPQExpBuffer(q, "STORAGE %s", opcintype);

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBufferStr(nameusing, fmtId(opcinfo->dobj.name));
	appendPQExpBuffer(nameusing, " USING %s",
					  fmtId(amname));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &opcinfo->dobj,
										"OPERATOR CLASS", nameusing->data,
										opcinfo->dobj.namespace->dobj.name);

	if (opcinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, opcinfo->dobj.catId, opcinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = opcinfo->dobj.name,
								  .namespace = opcinfo->dobj.namespace->dobj.name,
								  .owner = opcinfo->rolname,
								  .description = "OPERATOR CLASS",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出操作符类注释 */
	if (opcinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "OPERATOR CLASS", nameusing->data,
					opcinfo->dobj.namespace->dobj.name, opcinfo->rolname,
					opcinfo->dobj.catId, 0, opcinfo->dobj.dumpId);

	free(opcintype);
	free(opcfamily);
	free(amname);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(nameusing);
}

/*
 * dumpOpfamily
 *   写出单个操作符族定义
 * 
 * 注意：这还会导出那些未绑定到操作符族中特定
 * 操作符类的"松散"操作符成员。
 */
static void
dumpOpfamily(Archive *fout, const OpfamilyInfo *opfinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer nameusing;
	PGresult   *res;
	PGresult   *res_ops;
	PGresult   *res_procs;
	int			ntups;
	int			i_amname;
	int			i_amopstrategy;
	int			i_amopopr;
	int			i_sortfamily;
	int			i_sortfamilynsp;
	int			i_amprocnum;
	int			i_amproc;
	int			i_amproclefttype;
	int			i_amprocrighttype;
	char	   *amname;
	char	   *amopstrategy;
	char	   *amopopr;
	char	   *sortfamily;
	char	   *sortfamilynsp;
	char	   *amprocnum;
	char	   *amproc;
	char	   *amproclefttype;
	char	   *amprocrighttype;
	bool		needComma;
	int			i;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	nameusing = createPQExpBuffer();

	/*
	 * 仅获取那些通过 pg_depend 条目直接绑定到
	 * 操作符族的操作符族成员。
	 */
	appendPQExpBuffer(query, "SELECT amopstrategy, "
					  "amopopr::pg_catalog.regoperator, "
					  "opfname AS sortfamily, "
					  "nspname AS sortfamilynsp "
					  "FROM pg_catalog.pg_amop ao JOIN pg_catalog.pg_depend ON "
					  "(classid = 'pg_catalog.pg_amop'::pg_catalog.regclass AND objid = ao.oid) "
					  "LEFT JOIN pg_catalog.pg_opfamily f ON f.oid = amopsortfamily "
					  "LEFT JOIN pg_catalog.pg_namespace n ON n.oid = opfnamespace "
					  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
					  "AND amopfamily = '%u'::pg_catalog.oid "
					  "ORDER BY amopstrategy",
					  opfinfo->dobj.catId.oid,
					  opfinfo->dobj.catId.oid);

	res_ops = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT amprocnum, "
					  "amproc::pg_catalog.regprocedure, "
					  "amproclefttype::pg_catalog.regtype, "
					  "amprocrighttype::pg_catalog.regtype "
					  "FROM pg_catalog.pg_amproc ap, pg_catalog.pg_depend "
					  "WHERE refclassid = 'pg_catalog.pg_opfamily'::pg_catalog.regclass "
					  "AND refobjid = '%u'::pg_catalog.oid "
					  "AND classid = 'pg_catalog.pg_amproc'::pg_catalog.regclass "
					  "AND objid = ap.oid "
					  "ORDER BY amprocnum",
					  opfinfo->dobj.catId.oid);

	res_procs = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	/* 从 pg_opfamily 行获取附加字段 */
	resetPQExpBuffer(query);

	appendPQExpBuffer(query, "SELECT "
					  "(SELECT amname FROM pg_catalog.pg_am WHERE oid = opfmethod) AS amname "
					  "FROM pg_catalog.pg_opfamily "
					  "WHERE oid = '%u'::pg_catalog.oid",
					  opfinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_amname = PQfnumber(res, "amname");

	/* 在 PQclear res 之后仍需要 amname */
	amname = pg_strdup(PQgetvalue(res, 0, i_amname));

	appendPQExpBuffer(delq, "DROP OPERATOR FAMILY %s",
					  fmtQualifiedDumpable(opfinfo));
	appendPQExpBuffer(delq, " USING %s;\n",
					  fmtId(amname));

	/* 构建 CREATE 命令的固定部分 */
	appendPQExpBuffer(q, "CREATE OPERATOR FAMILY %s",
					  fmtQualifiedDumpable(opfinfo));
	appendPQExpBuffer(q, " USING %s;\n",
					  fmtId(amname));

	PQclear(res);

	/* 是否需要 ALTER 来添加松散成员？ */
	if (PQntuples(res_ops) > 0 || PQntuples(res_procs) > 0)
	{
		appendPQExpBuffer(q, "ALTER OPERATOR FAMILY %s",
						  fmtQualifiedDumpable(opfinfo));
		appendPQExpBuffer(q, " USING %s ADD\n    ",
						  fmtId(amname));

		needComma = false;

		/*
		 * 现在获取并输出 OPERATOR 条目（pg_amop 行）。
		 */
		ntups = PQntuples(res_ops);

		i_amopstrategy = PQfnumber(res_ops, "amopstrategy");
		i_amopopr = PQfnumber(res_ops, "amopopr");
		i_sortfamily = PQfnumber(res_ops, "sortfamily");
		i_sortfamilynsp = PQfnumber(res_ops, "sortfamilynsp");

		for (i = 0; i < ntups; i++)
		{
			amopstrategy = PQgetvalue(res_ops, i, i_amopstrategy);
			amopopr = PQgetvalue(res_ops, i, i_amopopr);
			sortfamily = PQgetvalue(res_ops, i, i_sortfamily);
			sortfamilynsp = PQgetvalue(res_ops, i, i_sortfamilynsp);

			if (needComma)
				appendPQExpBufferStr(q, " ,\n    ");

			appendPQExpBuffer(q, "OPERATOR %s %s",
							  amopstrategy, amopopr);

			if (strlen(sortfamily) > 0)
			{
				appendPQExpBufferStr(q, " FOR ORDER BY ");
				appendPQExpBuffer(q, "%s.", fmtId(sortfamilynsp));
				appendPQExpBufferStr(q, fmtId(sortfamily));
			}

			needComma = true;
		}

		/*
		 * 现在获取并输出 FUNCTION 条目（pg_amproc 行）。
		 */
		ntups = PQntuples(res_procs);

		i_amprocnum = PQfnumber(res_procs, "amprocnum");
		i_amproc = PQfnumber(res_procs, "amproc");
		i_amproclefttype = PQfnumber(res_procs, "amproclefttype");
		i_amprocrighttype = PQfnumber(res_procs, "amprocrighttype");

		for (i = 0; i < ntups; i++)
		{
			amprocnum = PQgetvalue(res_procs, i, i_amprocnum);
			amproc = PQgetvalue(res_procs, i, i_amproc);
			amproclefttype = PQgetvalue(res_procs, i, i_amproclefttype);
			amprocrighttype = PQgetvalue(res_procs, i, i_amprocrighttype);

			if (needComma)
				appendPQExpBufferStr(q, " ,\n    ");

			appendPQExpBuffer(q, "FUNCTION %s (%s, %s) %s",
							  amprocnum, amproclefttype, amprocrighttype,
							  amproc);

			needComma = true;
		}

		appendPQExpBufferStr(q, ";\n");
	}

	appendPQExpBufferStr(nameusing, fmtId(opfinfo->dobj.name));
	appendPQExpBuffer(nameusing, " USING %s",
					  fmtId(amname));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &opfinfo->dobj,
										"OPERATOR FAMILY", nameusing->data,
										opfinfo->dobj.namespace->dobj.name);

	if (opfinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, opfinfo->dobj.catId, opfinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = opfinfo->dobj.name,
								  .namespace = opfinfo->dobj.namespace->dobj.name,
								  .owner = opfinfo->rolname,
								  .description = "OPERATOR FAMILY",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出操作符族注释 */
	if (opfinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "OPERATOR FAMILY", nameusing->data,
					opfinfo->dobj.namespace->dobj.name, opfinfo->rolname,
					opfinfo->dobj.catId, 0, opfinfo->dobj.dumpId);

	free(amname);
	PQclear(res_ops);
	PQclear(res_procs);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(nameusing);
}

/*
 * dumpCollation
 *   写出单个排序规则定义
 */
static void
dumpCollation(Archive *fout, const CollInfo *collinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qcollname;
	PGresult   *res;
	int			i_collprovider;
	int			i_collisdeterministic;
	int			i_collcollate;
	int			i_collctype;
	int			i_colliculocale;
	int			i_collicurules;
	const char *collprovider;
	const char *collcollate;
	const char *collctype;
	const char *colliculocale;
	const char *collicurules;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qcollname = pg_strdup(fmtId(collinfo->dobj.name));

	/* 获取特定于排序规则的详细信息 */
	appendPQExpBufferStr(query, "SELECT ");

	if (fout->remoteVersion >= 100000)
		appendPQExpBufferStr(query,
							 "collprovider, "
							 "collversion, ");
	else
		appendPQExpBufferStr(query,
							 "'c' AS collprovider, "
							 "NULL AS collversion, ");

	if (fout->remoteVersion >= 120000)
		appendPQExpBufferStr(query,
							 "collisdeterministic, ");
	else
		appendPQExpBufferStr(query,
							 "true AS collisdeterministic, ");

	if (fout->remoteVersion >= 150000)
		appendPQExpBufferStr(query,
							 "colliculocale, ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS colliculocale, ");

	if (fout->remoteVersion >= 160000)
		appendPQExpBufferStr(query,
							 "collicurules, ");
	else
		appendPQExpBufferStr(query,
							 "NULL AS collicurules, ");

	appendPQExpBuffer(query,
					  "collcollate, "
					  "collctype "
					  "FROM pg_catalog.pg_collation c "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  collinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_collprovider = PQfnumber(res, "collprovider");
	i_collisdeterministic = PQfnumber(res, "collisdeterministic");
	i_collcollate = PQfnumber(res, "collcollate");
	i_collctype = PQfnumber(res, "collctype");
	i_colliculocale = PQfnumber(res, "colliculocale");
	i_collicurules = PQfnumber(res, "collicurules");

	collprovider = PQgetvalue(res, 0, i_collprovider);

	if (!PQgetisnull(res, 0, i_collcollate))
		collcollate = PQgetvalue(res, 0, i_collcollate);
	else
		collcollate = NULL;

	if (!PQgetisnull(res, 0, i_collctype))
		collctype = PQgetvalue(res, 0, i_collctype);
	else
		collctype = NULL;

	/*
	 * 在版本 15 之前，collcollate 和 collctype 是 NAME 类型且
	 * 不可为空。为了一致性，将空字符串视为 NULL。
	 */
	if (fout->remoteVersion < 150000)
	{
		if (collcollate[0] == '\0')
			collcollate = NULL;
		if (collctype[0] == '\0')
			collctype = NULL;
	}

	if (!PQgetisnull(res, 0, i_colliculocale))
		colliculocale = PQgetvalue(res, 0, i_colliculocale);
	else
		colliculocale = NULL;

	if (!PQgetisnull(res, 0, i_collicurules))
		collicurules = PQgetvalue(res, 0, i_collicurules);
	else
		collicurules = NULL;

	appendPQExpBuffer(delq, "DROP COLLATION %s;\n",
					  fmtQualifiedDumpable(collinfo));

	appendPQExpBuffer(q, "CREATE COLLATION %s (",
					  fmtQualifiedDumpable(collinfo));

	appendPQExpBufferStr(q, "provider = ");
	if (collprovider[0] == 'c')
		appendPQExpBufferStr(q, "libc");
	else if (collprovider[0] == 'i')
		appendPQExpBufferStr(q, "icu");
	else if (collprovider[0] == 'd')
		/* 允许导出 pg_catalog；输入时不接受 */
		appendPQExpBufferStr(q, "default");
	else
		pg_fatal("unrecognized collation provider: %s",
				 collprovider);

	if (strcmp(PQgetvalue(res, 0, i_collisdeterministic), "f") == 0)
		appendPQExpBufferStr(q, ", deterministic = false");

	if (collprovider[0] == 'd')
	{
		if (collcollate || collctype || colliculocale || collicurules)
			pg_log_warning("invalid collation \"%s\"", qcollname);

		/* 无 locale -- 默认排序规则无论如何都无法重新加载 */
	}
	else if (collprovider[0] == 'i')
	{
		if (fout->remoteVersion >= 150000)
		{
			if (collcollate || collctype || !colliculocale)
				pg_log_warning("invalid collation \"%s\"", qcollname);

			appendPQExpBufferStr(q, ", locale = ");
			appendStringLiteralAH(q, colliculocale ? colliculocale : "",
								  fout);
		}
		else
		{
			if (!collcollate || !collctype || colliculocale ||
				strcmp(collcollate, collctype) != 0)
				pg_log_warning("invalid collation \"%s\"", qcollname);

			appendPQExpBufferStr(q, ", locale = ");
			appendStringLiteralAH(q, collcollate ? collcollate : "", fout);
		}

		if (collicurules)
		{
			appendPQExpBufferStr(q, ", rules = ");
			appendStringLiteralAH(q, collicurules ? collicurules : "", fout);
		}
	}
	else if (collprovider[0] == 'c')
	{
		if (colliculocale || collicurules || !collcollate || !collctype)
			pg_log_warning("invalid collation \"%s\"", qcollname);

		if (collcollate && collctype && strcmp(collcollate, collctype) == 0)
		{
			appendPQExpBufferStr(q, ", locale = ");
			appendStringLiteralAH(q, collcollate ? collcollate : "", fout);
		}
		else
		{
			appendPQExpBufferStr(q, ", lc_collate = ");
			appendStringLiteralAH(q, collcollate ? collcollate : "", fout);
			appendPQExpBufferStr(q, ", lc_ctype = ");
			appendStringLiteralAH(q, collctype ? collctype : "", fout);
		}
	}
	else
		pg_fatal("unrecognized collation provider: %s", collprovider);

	/*
	 * 对于二进制升级，携带排序规则版本。对于正常的
	 * 导出/恢复，省略版本，以便在恢复时计算。
	 */
	if (dopt->binary_upgrade)
	{
		int			i_collversion;

		i_collversion = PQfnumber(res, "collversion");
		if (!PQgetisnull(res, 0, i_collversion))
		{
			appendPQExpBufferStr(q, ", version = ");
			appendStringLiteralAH(q,
								  PQgetvalue(res, 0, i_collversion),
								  fout);
		}
	}

	appendPQExpBufferStr(q, ");\n");

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &collinfo->dobj,
										"COLLATION", qcollname,
										collinfo->dobj.namespace->dobj.name);

	if (collinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, collinfo->dobj.catId, collinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = collinfo->dobj.name,
								  .namespace = collinfo->dobj.namespace->dobj.name,
								  .owner = collinfo->rolname,
								  .description = "COLLATION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出排序规则注释 */
	if (collinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "COLLATION", qcollname,
					collinfo->dobj.namespace->dobj.name, collinfo->rolname,
					collinfo->dobj.catId, 0, collinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qcollname);
}

/*
 * dumpConversion
 *   写出单个转换定义
 */
static void
dumpConversion(Archive *fout, const ConvInfo *convinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qconvname;
	PGresult   *res;
	int			i_conforencoding;
	int			i_contoencoding;
	int			i_conproc;
	int			i_condefault;
	const char *conforencoding;
	const char *contoencoding;
	const char *conproc;
	bool		condefault;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qconvname = pg_strdup(fmtId(convinfo->dobj.name));

	/* 获取特定于转换的详细信息 */
	appendPQExpBuffer(query, "SELECT "
					  "pg_catalog.pg_encoding_to_char(conforencoding) AS conforencoding, "
					  "pg_catalog.pg_encoding_to_char(contoencoding) AS contoencoding, "
					  "conproc, condefault "
					  "FROM pg_catalog.pg_conversion c "
					  "WHERE c.oid = '%u'::pg_catalog.oid",
					  convinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_conforencoding = PQfnumber(res, "conforencoding");
	i_contoencoding = PQfnumber(res, "contoencoding");
	i_conproc = PQfnumber(res, "conproc");
	i_condefault = PQfnumber(res, "condefault");

	conforencoding = PQgetvalue(res, 0, i_conforencoding);
	contoencoding = PQgetvalue(res, 0, i_contoencoding);
	conproc = PQgetvalue(res, 0, i_conproc);
	condefault = (PQgetvalue(res, 0, i_condefault)[0] == 't');

	appendPQExpBuffer(delq, "DROP CONVERSION %s;\n",
					  fmtQualifiedDumpable(convinfo));

	appendPQExpBuffer(q, "CREATE %sCONVERSION %s FOR ",
					  (condefault) ? "DEFAULT " : "",
					  fmtQualifiedDumpable(convinfo));
	appendStringLiteralAH(q, conforencoding, fout);
	appendPQExpBufferStr(q, " TO ");
	appendStringLiteralAH(q, contoencoding, fout);
	/* regproc 输出已充分加引号 */
	appendPQExpBuffer(q, " FROM %s;\n", conproc);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &convinfo->dobj,
										"CONVERSION", qconvname,
										convinfo->dobj.namespace->dobj.name);

	if (convinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, convinfo->dobj.catId, convinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = convinfo->dobj.name,
								  .namespace = convinfo->dobj.namespace->dobj.name,
								  .owner = convinfo->rolname,
								  .description = "CONVERSION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出转换注释 */
	if (convinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "CONVERSION", qconvname,
					convinfo->dobj.namespace->dobj.name, convinfo->rolname,
					convinfo->dobj.catId, 0, convinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qconvname);
}

/*
 * format_aggregate_signature: 生成聚合名称和参数列表
 * 
 * 如果需要，参数类型名称会进行限定。聚合名称从不限定。
 *
 */
static char *
format_aggregate_signature(const AggInfo *agginfo, Archive *fout, bool honor_quotes)
{
	PQExpBufferData buf;
	int			j;

	initPQExpBuffer(&buf);
	if (honor_quotes)
		appendPQExpBufferStr(&buf, fmtId(agginfo->aggfn.dobj.name));
	else
		appendPQExpBufferStr(&buf, agginfo->aggfn.dobj.name);

	if (agginfo->aggfn.nargs == 0)
		appendPQExpBufferStr(&buf, "(*)");
	else
	{
		appendPQExpBufferChar(&buf, '(');
		for (j = 0; j < agginfo->aggfn.nargs; j++)
			appendPQExpBuffer(&buf, "%s%s",
							  (j > 0) ? ", " : "",
							  getFormattedTypeName(fout,
												   agginfo->aggfn.argtypes[j],
												   zeroIsError));
		appendPQExpBufferChar(&buf, ')');
	}
	return buf.data;
}

/*
 * dumpAgg
 *   写出单个聚合定义
 */
static void
dumpAgg(Archive *fout, const AggInfo *agginfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer details;
	char	   *aggsig;			/* 身份签名 */
	char	   *aggfullsig = NULL;	/* 完整签名 */
	char	   *aggsig_tag;
	PGresult   *res;
	int			i_agginitval;
	int			i_aggminitval;
	const char *aggtransfn;
	const char *aggfinalfn;
	const char *aggcombinefn;
	const char *aggserialfn;
	const char *aggdeserialfn;
	const char *aggmtransfn;
	const char *aggminvtransfn;
	const char *aggmfinalfn;
	bool		aggfinalextra;
	bool		aggmfinalextra;
	char		aggfinalmodify;
	char		aggmfinalmodify;
	const char *aggsortop;
	char	   *aggsortconvop;
	char		aggkind;
	const char *aggtranstype;
	const char *aggtransspace;
	const char *aggmtranstype;
	const char *aggmtransspace;
	const char *agginitval;
	const char *aggminitval;
	const char *proparallel;
	char		defaultfinalmodify;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	details = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPAGG])
	{
		/* 设置查询以获取特定于聚合的详细信息 */
		appendPQExpBufferStr(query,
							 "PREPARE dumpAgg(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(query,
							 "SELECT "
							 "aggtransfn,\n"
							 "aggfinalfn,\n"
							 "aggtranstype::pg_catalog.regtype,\n"
							 "agginitval,\n"
							 "aggsortop,\n"
							 "pg_catalog.pg_get_function_arguments(p.oid) AS funcargs,\n"
							 "pg_catalog.pg_get_function_identity_arguments(p.oid) AS funciargs,\n");

		if (fout->remoteVersion >= 90400)
			appendPQExpBufferStr(query,
								 "aggkind,\n"
								 "aggmtransfn,\n"
								 "aggminvtransfn,\n"
								 "aggmfinalfn,\n"
								 "aggmtranstype::pg_catalog.regtype,\n"
								 "aggfinalextra,\n"
								 "aggmfinalextra,\n"
								 "aggtransspace,\n"
								 "aggmtransspace,\n"
								 "aggminitval,\n");
		else
			appendPQExpBufferStr(query,
								 "'n' AS aggkind,\n"
								 "'-' AS aggmtransfn,\n"
								 "'-' AS aggminvtransfn,\n"
								 "'-' AS aggmfinalfn,\n"
								 "0 AS aggmtranstype,\n"
								 "false AS aggfinalextra,\n"
								 "false AS aggmfinalextra,\n"
								 "0 AS aggtransspace,\n"
								 "0 AS aggmtransspace,\n"
								 "NULL AS aggminitval,\n");

		if (fout->remoteVersion >= 90600)
			appendPQExpBufferStr(query,
								 "aggcombinefn,\n"
								 "aggserialfn,\n"
								 "aggdeserialfn,\n"
								 "proparallel,\n");
		else
			appendPQExpBufferStr(query,
								 "'-' AS aggcombinefn,\n"
								 "'-' AS aggserialfn,\n"
								 "'-' AS aggdeserialfn,\n"
								 "'u' AS proparallel,\n");

		if (fout->remoteVersion >= 110000)
			appendPQExpBufferStr(query,
								 "aggfinalmodify,\n"
								 "aggmfinalmodify\n");
		else
			appendPQExpBufferStr(query,
								 "'0' AS aggfinalmodify,\n"
								 "'0' AS aggmfinalmodify\n");

		appendPQExpBufferStr(query,
							 "FROM pg_catalog.pg_aggregate a, pg_catalog.pg_proc p "
							 "WHERE a.aggfnoid = p.oid "
							 "AND p.oid = $1");

		ExecuteSqlStatement(fout, query->data);

		fout->is_prepared[PREPQUERY_DUMPAGG] = true;
	}

	printfPQExpBuffer(query,
					  "EXECUTE dumpAgg('%u')",
					  agginfo->aggfn.dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	i_agginitval = PQfnumber(res, "agginitval");
	i_aggminitval = PQfnumber(res, "aggminitval");

	aggtransfn = PQgetvalue(res, 0, PQfnumber(res, "aggtransfn"));
	aggfinalfn = PQgetvalue(res, 0, PQfnumber(res, "aggfinalfn"));
	aggcombinefn = PQgetvalue(res, 0, PQfnumber(res, "aggcombinefn"));
	aggserialfn = PQgetvalue(res, 0, PQfnumber(res, "aggserialfn"));
	aggdeserialfn = PQgetvalue(res, 0, PQfnumber(res, "aggdeserialfn"));
	aggmtransfn = PQgetvalue(res, 0, PQfnumber(res, "aggmtransfn"));
	aggminvtransfn = PQgetvalue(res, 0, PQfnumber(res, "aggminvtransfn"));
	aggmfinalfn = PQgetvalue(res, 0, PQfnumber(res, "aggmfinalfn"));
	aggfinalextra = (PQgetvalue(res, 0, PQfnumber(res, "aggfinalextra"))[0] == 't');
	aggmfinalextra = (PQgetvalue(res, 0, PQfnumber(res, "aggmfinalextra"))[0] == 't');
	aggfinalmodify = PQgetvalue(res, 0, PQfnumber(res, "aggfinalmodify"))[0];
	aggmfinalmodify = PQgetvalue(res, 0, PQfnumber(res, "aggmfinalmodify"))[0];
	aggsortop = PQgetvalue(res, 0, PQfnumber(res, "aggsortop"));
	aggkind = PQgetvalue(res, 0, PQfnumber(res, "aggkind"))[0];
	aggtranstype = PQgetvalue(res, 0, PQfnumber(res, "aggtranstype"));
	aggtransspace = PQgetvalue(res, 0, PQfnumber(res, "aggtransspace"));
	aggmtranstype = PQgetvalue(res, 0, PQfnumber(res, "aggmtranstype"));
	aggmtransspace = PQgetvalue(res, 0, PQfnumber(res, "aggmtransspace"));
	agginitval = PQgetvalue(res, 0, i_agginitval);
	aggminitval = PQgetvalue(res, 0, i_aggminitval);
	proparallel = PQgetvalue(res, 0, PQfnumber(res, "proparallel"));

	{
		char	   *funcargs;
		char	   *funciargs;

		funcargs = PQgetvalue(res, 0, PQfnumber(res, "funcargs"));
		funciargs = PQgetvalue(res, 0, PQfnumber(res, "funciargs"));
		aggfullsig = format_function_arguments(&agginfo->aggfn, funcargs, true);
		aggsig = format_function_arguments(&agginfo->aggfn, funciargs, true);
	}

	aggsig_tag = format_aggregate_signature(agginfo, fout, false);

	/* 识别 aggkind 的默认 modify 标志（必须匹配 DefineAggregate） */
	defaultfinalmodify = (aggkind == AGGKIND_NORMAL) ? AGGMODIFY_READ_ONLY : AGGMODIFY_READ_WRITE;
	/* 替换旧版本中省略的标志 */
	if (aggfinalmodify == '0')
		aggfinalmodify = defaultfinalmodify;
	if (aggmfinalmodify == '0')
		aggmfinalmodify = defaultfinalmodify;

	/* regproc 和 regtype 输出已充分加引号 */
	appendPQExpBuffer(details, "    SFUNC = %s,\n    STYPE = %s",
					  aggtransfn, aggtranstype);

	if (strcmp(aggtransspace, "0") != 0)
	{
		appendPQExpBuffer(details, ",\n    SSPACE = %s",
						  aggtransspace);
	}

	if (!PQgetisnull(res, 0, i_agginitval))
	{
		appendPQExpBufferStr(details, ",\n    INITCOND = ");
		appendStringLiteralAH(details, agginitval, fout);
	}

	if (strcmp(aggfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    FINALFUNC = %s",
						  aggfinalfn);
		if (aggfinalextra)
			appendPQExpBufferStr(details, ",\n    FINALFUNC_EXTRA");
		if (aggfinalmodify != defaultfinalmodify)
		{
			switch (aggfinalmodify)
			{
				case AGGMODIFY_READ_ONLY:
					appendPQExpBufferStr(details, ",\n    FINALFUNC_MODIFY = READ_ONLY");
					break;
				case AGGMODIFY_SHAREABLE:
					appendPQExpBufferStr(details, ",\n    FINALFUNC_MODIFY = SHAREABLE");
					break;
				case AGGMODIFY_READ_WRITE:
					appendPQExpBufferStr(details, ",\n    FINALFUNC_MODIFY = READ_WRITE");
					break;
				default:
					pg_fatal("unrecognized aggfinalmodify value for aggregate \"%s\"",
							 agginfo->aggfn.dobj.name);
					break;
			}
		}
	}

	if (strcmp(aggcombinefn, "-") != 0)
		appendPQExpBuffer(details, ",\n    COMBINEFUNC = %s", aggcombinefn);

	if (strcmp(aggserialfn, "-") != 0)
		appendPQExpBuffer(details, ",\n    SERIALFUNC = %s", aggserialfn);

	if (strcmp(aggdeserialfn, "-") != 0)
		appendPQExpBuffer(details, ",\n    DESERIALFUNC = %s", aggdeserialfn);

	if (strcmp(aggmtransfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    MSFUNC = %s,\n    MINVFUNC = %s,\n    MSTYPE = %s",
						  aggmtransfn,
						  aggminvtransfn,
						  aggmtranstype);
	}

	if (strcmp(aggmtransspace, "0") != 0)
	{
		appendPQExpBuffer(details, ",\n    MSSPACE = %s",
						  aggmtransspace);
	}

	if (!PQgetisnull(res, 0, i_aggminitval))
	{
		appendPQExpBufferStr(details, ",\n    MINITCOND = ");
		appendStringLiteralAH(details, aggminitval, fout);
	}

	if (strcmp(aggmfinalfn, "-") != 0)
	{
		appendPQExpBuffer(details, ",\n    MFINALFUNC = %s",
						  aggmfinalfn);
		if (aggmfinalextra)
			appendPQExpBufferStr(details, ",\n    MFINALFUNC_EXTRA");
		if (aggmfinalmodify != defaultfinalmodify)
		{
			switch (aggmfinalmodify)
			{
				case AGGMODIFY_READ_ONLY:
					appendPQExpBufferStr(details, ",\n    MFINALFUNC_MODIFY = READ_ONLY");
					break;
				case AGGMODIFY_SHAREABLE:
					appendPQExpBufferStr(details, ",\n    MFINALFUNC_MODIFY = SHAREABLE");
					break;
				case AGGMODIFY_READ_WRITE:
					appendPQExpBufferStr(details, ",\n    MFINALFUNC_MODIFY = READ_WRITE");
					break;
				default:
					pg_fatal("unrecognized aggmfinalmodify value for aggregate \"%s\"",
							 agginfo->aggfn.dobj.name);
					break;
			}
		}
	}

	aggsortconvop = getFormattedOperatorName(aggsortop);
	if (aggsortconvop)
	{
		appendPQExpBuffer(details, ",\n    SORTOP = %s",
						  aggsortconvop);
		free(aggsortconvop);
	}

	if (aggkind == AGGKIND_HYPOTHETICAL)
		appendPQExpBufferStr(details, ",\n    HYPOTHETICAL");

	if (proparallel[0] != PROPARALLEL_UNSAFE)
	{
		if (proparallel[0] == PROPARALLEL_SAFE)
			appendPQExpBufferStr(details, ",\n    PARALLEL = safe");
		else if (proparallel[0] == PROPARALLEL_RESTRICTED)
			appendPQExpBufferStr(details, ",\n    PARALLEL = restricted");
		else if (proparallel[0] != PROPARALLEL_UNSAFE)
			pg_fatal("unrecognized proparallel value for function \"%s\"",
					 agginfo->aggfn.dobj.name);
	}

	appendPQExpBuffer(delq, "DROP AGGREGATE %s.%s;\n",
					  fmtId(agginfo->aggfn.dobj.namespace->dobj.name),
					  aggsig);

	appendPQExpBuffer(q, "CREATE AGGREGATE %s.%s (\n%s\n);\n",
					  fmtId(agginfo->aggfn.dobj.namespace->dobj.name),
					  aggfullsig ? aggfullsig : aggsig, details->data);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &agginfo->aggfn.dobj,
										"AGGREGATE", aggsig,
										agginfo->aggfn.dobj.namespace->dobj.name);

	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, agginfo->aggfn.dobj.catId,
					 agginfo->aggfn.dobj.dumpId,
					 ARCHIVE_OPTS(.tag = aggsig_tag,
								  .namespace = agginfo->aggfn.dobj.namespace->dobj.name,
								  .owner = agginfo->aggfn.rolname,
								  .description = "AGGREGATE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出聚合注释 */
	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "AGGREGATE", aggsig,
					agginfo->aggfn.dobj.namespace->dobj.name,
					agginfo->aggfn.rolname,
					agginfo->aggfn.dobj.catId, 0, agginfo->aggfn.dobj.dumpId);

	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "AGGREGATE", aggsig,
					 agginfo->aggfn.dobj.namespace->dobj.name,
					 agginfo->aggfn.rolname,
					 agginfo->aggfn.dobj.catId, 0, agginfo->aggfn.dobj.dumpId);

	/*
	 * 由于没有 GRANT ON AGGREGATE 语法，我们必须使 ACL
	 * 命令看起来像函数的 GRANT；特别地，这影响零参数
	 * 聚合和有序集聚合的语法。
	 */
	free(aggsig);

	aggsig = format_function_signature(fout, &agginfo->aggfn, true);

	if (agginfo->aggfn.dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, agginfo->aggfn.dobj.dumpId, InvalidDumpId,
				"FUNCTION", aggsig, NULL,
				agginfo->aggfn.dobj.namespace->dobj.name,
				agginfo->aggfn.rolname, &agginfo->aggfn.dacl);

	free(aggsig);
	free(aggfullsig);
	free(aggsig_tag);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(details);
}

/*
 * dumpTSParser
 *   写出单个文本搜索解析器
 */
static void
dumpTSParser(Archive *fout, const TSParserInfo *prsinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qprsname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qprsname = pg_strdup(fmtId(prsinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE TEXT SEARCH PARSER %s (\n",
					  fmtQualifiedDumpable(prsinfo));

	appendPQExpBuffer(q, "    START = %s,\n",
					  convertTSFunction(fout, prsinfo->prsstart));
	appendPQExpBuffer(q, "    GETTOKEN = %s,\n",
					  convertTSFunction(fout, prsinfo->prstoken));
	appendPQExpBuffer(q, "    END = %s,\n",
					  convertTSFunction(fout, prsinfo->prsend));
	if (prsinfo->prsheadline != InvalidOid)
		appendPQExpBuffer(q, "    HEADLINE = %s,\n",
						  convertTSFunction(fout, prsinfo->prsheadline));
	appendPQExpBuffer(q, "    LEXTYPES = %s );\n",
					  convertTSFunction(fout, prsinfo->prslextype));

	appendPQExpBuffer(delq, "DROP TEXT SEARCH PARSER %s;\n",
					  fmtQualifiedDumpable(prsinfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &prsinfo->dobj,
										"TEXT SEARCH PARSER", qprsname,
										prsinfo->dobj.namespace->dobj.name);

	if (prsinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, prsinfo->dobj.catId, prsinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = prsinfo->dobj.name,
								  .namespace = prsinfo->dobj.namespace->dobj.name,
								  .description = "TEXT SEARCH PARSER",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出解析器注释 */
	if (prsinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH PARSER", qprsname,
					prsinfo->dobj.namespace->dobj.name, "",
					prsinfo->dobj.catId, 0, prsinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qprsname);
}

/*
 * dumpTSDictionary
 *   写出单个文本搜索字典
 */
static void
dumpTSDictionary(Archive *fout, const TSDictInfo *dictinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qdictname;
	PGresult   *res;
	char	   *nspname;
	char	   *tmplname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qdictname = pg_strdup(fmtId(dictinfo->dobj.name));

	/* 获取字典模板的名称和命名空间 */
	appendPQExpBuffer(query, "SELECT nspname, tmplname "
					  "FROM pg_ts_template p, pg_namespace n "
					  "WHERE p.oid = '%u' AND n.oid = tmplnamespace",
					  dictinfo->dicttemplate);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	nspname = PQgetvalue(res, 0, 0);
	tmplname = PQgetvalue(res, 0, 1);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH DICTIONARY %s (\n",
					  fmtQualifiedDumpable(dictinfo));

	appendPQExpBufferStr(q, "    TEMPLATE = ");
	appendPQExpBuffer(q, "%s.", fmtId(nspname));
	appendPQExpBufferStr(q, fmtId(tmplname));

	PQclear(res);

	/* dictinitoption 可以直接导出到命令中 */
	if (dictinfo->dictinitoption)
		appendPQExpBuffer(q, ",\n    %s", dictinfo->dictinitoption);

	appendPQExpBufferStr(q, " );\n");

	appendPQExpBuffer(delq, "DROP TEXT SEARCH DICTIONARY %s;\n",
					  fmtQualifiedDumpable(dictinfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &dictinfo->dobj,
										"TEXT SEARCH DICTIONARY", qdictname,
										dictinfo->dobj.namespace->dobj.name);

	if (dictinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, dictinfo->dobj.catId, dictinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = dictinfo->dobj.name,
								  .namespace = dictinfo->dobj.namespace->dobj.name,
								  .owner = dictinfo->rolname,
								  .description = "TEXT SEARCH DICTIONARY",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出字典注释 */
	if (dictinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH DICTIONARY", qdictname,
					dictinfo->dobj.namespace->dobj.name, dictinfo->rolname,
					dictinfo->dobj.catId, 0, dictinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qdictname);
}

/*
 * dumpTSTemplate
 *   写出单个文本搜索模板
 */
static void
dumpTSTemplate(Archive *fout, const TSTemplateInfo *tmplinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qtmplname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qtmplname = pg_strdup(fmtId(tmplinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE TEXT SEARCH TEMPLATE %s (\n",
					  fmtQualifiedDumpable(tmplinfo));

	if (tmplinfo->tmplinit != InvalidOid)
		appendPQExpBuffer(q, "    INIT = %s,\n",
						  convertTSFunction(fout, tmplinfo->tmplinit));
	appendPQExpBuffer(q, "    LEXIZE = %s );\n",
					  convertTSFunction(fout, tmplinfo->tmpllexize));

	appendPQExpBuffer(delq, "DROP TEXT SEARCH TEMPLATE %s;\n",
					  fmtQualifiedDumpable(tmplinfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tmplinfo->dobj,
										"TEXT SEARCH TEMPLATE", qtmplname,
										tmplinfo->dobj.namespace->dobj.name);

	if (tmplinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tmplinfo->dobj.catId, tmplinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tmplinfo->dobj.name,
								  .namespace = tmplinfo->dobj.namespace->dobj.name,
								  .description = "TEXT SEARCH TEMPLATE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出模板注释 */
	if (tmplinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH TEMPLATE", qtmplname,
					tmplinfo->dobj.namespace->dobj.name, "",
					tmplinfo->dobj.catId, 0, tmplinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qtmplname);
}

/*
 * dumpTSConfig
 *   写出单个文本搜索配置
 */
static void
dumpTSConfig(Archive *fout, const TSConfigInfo *cfginfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qcfgname;
	PGresult   *res;
	char	   *nspname;
	char	   *prsname;
	int			ntups,
				i;
	int			i_tokenname;
	int			i_dictname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qcfgname = pg_strdup(fmtId(cfginfo->dobj.name));

	/* 获取配置解析器的名称和命名空间 */
	appendPQExpBuffer(query, "SELECT nspname, prsname "
					  "FROM pg_ts_parser p, pg_namespace n "
					  "WHERE p.oid = '%u' AND n.oid = prsnamespace",
					  cfginfo->cfgparser);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	nspname = PQgetvalue(res, 0, 0);
	prsname = PQgetvalue(res, 0, 1);

	appendPQExpBuffer(q, "CREATE TEXT SEARCH CONFIGURATION %s (\n",
					  fmtQualifiedDumpable(cfginfo));

	appendPQExpBuffer(q, "    PARSER = %s.", fmtId(nspname));
	appendPQExpBuffer(q, "%s );\n", fmtId(prsname));

	PQclear(res);

	resetPQExpBuffer(query);
	appendPQExpBuffer(query,
					  "SELECT\n"
					  "  ( SELECT alias FROM pg_catalog.ts_token_type('%u'::pg_catalog.oid) AS t\n"
					  "    WHERE t.tokid = m.maptokentype ) AS tokenname,\n"
					  "  m.mapdict::pg_catalog.regdictionary AS dictname\n"
					  "FROM pg_catalog.pg_ts_config_map AS m\n"
					  "WHERE m.mapcfg = '%u'\n"
					  "ORDER BY m.mapcfg, m.maptokentype, m.mapseqno",
					  cfginfo->cfgparser, cfginfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);

	i_tokenname = PQfnumber(res, "tokenname");
	i_dictname = PQfnumber(res, "dictname");

	for (i = 0; i < ntups; i++)
	{
		char	   *tokenname = PQgetvalue(res, i, i_tokenname);
		char	   *dictname = PQgetvalue(res, i, i_dictname);

		if (i == 0 ||
			strcmp(tokenname, PQgetvalue(res, i - 1, i_tokenname)) != 0)
		{
			/* 开始一个新的标记类型，因此开始一个新的命令 */
			if (i > 0)
				appendPQExpBufferStr(q, ";\n");
			appendPQExpBuffer(q, "\nALTER TEXT SEARCH CONFIGURATION %s\n",
							  fmtQualifiedDumpable(cfginfo));
			/* tokenname 需要加引号，dictname 不需要 */
			appendPQExpBuffer(q, "    ADD MAPPING FOR %s WITH %s",
							  fmtId(tokenname), dictname);
		}
		else
			appendPQExpBuffer(q, ", %s", dictname);
	}

	if (ntups > 0)
		appendPQExpBufferStr(q, ";\n");

	PQclear(res);

	appendPQExpBuffer(delq, "DROP TEXT SEARCH CONFIGURATION %s;\n",
					  fmtQualifiedDumpable(cfginfo));

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &cfginfo->dobj,
										"TEXT SEARCH CONFIGURATION", qcfgname,
										cfginfo->dobj.namespace->dobj.name);

	if (cfginfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, cfginfo->dobj.catId, cfginfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = cfginfo->dobj.name,
								  .namespace = cfginfo->dobj.namespace->dobj.name,
								  .owner = cfginfo->rolname,
								  .description = "TEXT SEARCH CONFIGURATION",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出配置注释 */
	if (cfginfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "TEXT SEARCH CONFIGURATION", qcfgname,
					cfginfo->dobj.namespace->dobj.name, cfginfo->rolname,
					cfginfo->dobj.catId, 0, cfginfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qcfgname);
}

/*
 * dumpForeignDataWrapper
 *   写出单个 FDW 定义
 */
static void
dumpForeignDataWrapper(Archive *fout, const FdwInfo *fdwinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qfdwname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qfdwname = pg_strdup(fmtId(fdwinfo->dobj.name));

	appendPQExpBuffer(q, "CREATE FOREIGN DATA WRAPPER %s",
					  qfdwname);

	if (strcmp(fdwinfo->fdwhandler, "-") != 0)
		appendPQExpBuffer(q, " HANDLER %s", fdwinfo->fdwhandler);

	if (strcmp(fdwinfo->fdwvalidator, "-") != 0)
		appendPQExpBuffer(q, " VALIDATOR %s", fdwinfo->fdwvalidator);

	if (strlen(fdwinfo->fdwoptions) > 0)
		appendPQExpBuffer(q, " OPTIONS (\n    %s\n)", fdwinfo->fdwoptions);

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBuffer(delq, "DROP FOREIGN DATA WRAPPER %s;\n",
					  qfdwname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &fdwinfo->dobj,
										"FOREIGN DATA WRAPPER", qfdwname,
										NULL);

	if (fdwinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, fdwinfo->dobj.catId, fdwinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = fdwinfo->dobj.name,
								  .owner = fdwinfo->rolname,
								  .description = "FOREIGN DATA WRAPPER",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出 FDW 注释 */
	if (fdwinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "FOREIGN DATA WRAPPER", qfdwname,
					NULL, fdwinfo->rolname,
					fdwinfo->dobj.catId, 0, fdwinfo->dobj.dumpId);

	/* 处理 ACL */
	if (fdwinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, fdwinfo->dobj.dumpId, InvalidDumpId,
				"FOREIGN DATA WRAPPER", qfdwname, NULL,
				NULL, fdwinfo->rolname, &fdwinfo->dacl);

	free(qfdwname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpForeignServer
 *   写出一个外部服务器定义
 */
static void
dumpForeignServer(Archive *fout, const ForeignServerInfo *srvinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	PGresult   *res;
	char	   *qsrvname;
	char	   *fdwname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qsrvname = pg_strdup(fmtId(srvinfo->dobj.name));

	/* 查找 FDW */
	appendPQExpBuffer(query, "SELECT fdwname "
					  "FROM pg_foreign_data_wrapper w "
					  "WHERE w.oid = '%u'",
					  srvinfo->srvfdw);
	res = ExecuteSqlQueryForSingleRow(fout, query->data);
	fdwname = PQgetvalue(res, 0, 0);

	appendPQExpBuffer(q, "CREATE SERVER %s", qsrvname);
	if (srvinfo->srvtype && strlen(srvinfo->srvtype) > 0)
	{
		appendPQExpBufferStr(q, " TYPE ");
		appendStringLiteralAH(q, srvinfo->srvtype, fout);
	}
	if (srvinfo->srvversion && strlen(srvinfo->srvversion) > 0)
	{
		appendPQExpBufferStr(q, " VERSION ");
		appendStringLiteralAH(q, srvinfo->srvversion, fout);
	}

	appendPQExpBufferStr(q, " FOREIGN DATA WRAPPER ");
	appendPQExpBufferStr(q, fmtId(fdwname));

	if (srvinfo->srvoptions && strlen(srvinfo->srvoptions) > 0)
		appendPQExpBuffer(q, " OPTIONS (\n    %s\n)", srvinfo->srvoptions);

	appendPQExpBufferStr(q, ";\n");

	appendPQExpBuffer(delq, "DROP SERVER %s;\n",
					  qsrvname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &srvinfo->dobj,
										"SERVER", qsrvname, NULL);

	if (srvinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, srvinfo->dobj.catId, srvinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = srvinfo->dobj.name,
								  .owner = srvinfo->rolname,
								  .description = "SERVER",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出外部服务器注释 */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SERVER", qsrvname,
					NULL, srvinfo->rolname,
					srvinfo->dobj.catId, 0, srvinfo->dobj.dumpId);

	/* 处理 ACL */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_ACL)
		dumpACL(fout, srvinfo->dobj.dumpId, InvalidDumpId,
				"FOREIGN SERVER", qsrvname, NULL,
				NULL, srvinfo->rolname, &srvinfo->dacl);

	/* 导出用户映射 */
	if (srvinfo->dobj.dump & DUMP_COMPONENT_USERMAP)
		dumpUserMappings(fout,
						 srvinfo->dobj.name, NULL,
						 srvinfo->rolname,
						 srvinfo->dobj.catId, srvinfo->dobj.dumpId);

	PQclear(res);

	free(qsrvname);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
}

/*
 * dumpUserMappings
 * 
 * 此例程用于导出与传递给此例程的服务器关联的任何
 * 用户映射。应在服务器的 ArchiveEntry() 之后调用。
 *
 */
static void
dumpUserMappings(Archive *fout,
				 const char *servername, const char *namespace,
				 const char *owner,
				 CatalogId catalogId, DumpId dumpId)
{
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	PQExpBuffer tag;
	PGresult   *res;
	int			ntups;
	int			i_usename;
	int			i_umoptions;
	int			i;

	q = createPQExpBuffer();
	tag = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	/*
	 * 我们从公开可访问的视图 pg_user_mappings 读取，
	 * 以免在由非超级用户运行时失败。请注意，如果用户
	 * 对关联的服务器没有权限，该视图将显示 umoptions
	 * 为 null；这意味着 pg_dump 将导出这样的映射，但
	 * 没有 OPTIONS 子句。一个可能的替代方案是完全跳过
	 * 这样的映射，但不清楚那是否是一种改进。
	 */
	appendPQExpBuffer(query,
					  "SELECT usename, "
					  "array_to_string(ARRAY("
					  "SELECT quote_ident(option_name) || ' ' || "
					  "quote_literal(option_value) "
					  "FROM pg_options_to_table(umoptions) "
					  "ORDER BY option_name"
					  "), E',\n    ') AS umoptions "
					  "FROM pg_user_mappings "
					  "WHERE srvid = '%u' "
					  "ORDER BY usename",
					  catalogId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);
	i_usename = PQfnumber(res, "usename");
	i_umoptions = PQfnumber(res, "umoptions");

	for (i = 0; i < ntups; i++)
	{
		char	   *usename;
		char	   *umoptions;

		usename = PQgetvalue(res, i, i_usename);
		umoptions = PQgetvalue(res, i, i_umoptions);

		resetPQExpBuffer(q);
		appendPQExpBuffer(q, "CREATE USER MAPPING FOR %s", fmtId(usename));
		appendPQExpBuffer(q, " SERVER %s", fmtId(servername));

		if (umoptions && strlen(umoptions) > 0)
			appendPQExpBuffer(q, " OPTIONS (\n    %s\n)", umoptions);

		appendPQExpBufferStr(q, ";\n");

		resetPQExpBuffer(delq);
		appendPQExpBuffer(delq, "DROP USER MAPPING FOR %s", fmtId(usename));
		appendPQExpBuffer(delq, " SERVER %s;\n", fmtId(servername));

		resetPQExpBuffer(tag);
		appendPQExpBuffer(tag, "USER MAPPING %s SERVER %s",
						  usename, servername);

		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = namespace,
								  .owner = owner,
								  .description = "USER MAPPING",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));
	}

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(tag);
	destroyPQExpBuffer(q);
}

/*
 * 输出默认权限信息
 */
static void
dumpDefaultACL(Archive *fout, const DefaultACLInfo *daclinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer tag;
	const char *type;

	/* 在仅数据导出中不执行任何操作，或者如果我们正在跳过 ACL */
	if (dopt->dataOnly || dopt->aclsSkip)
		return;

	q = createPQExpBuffer();
	tag = createPQExpBuffer();

	switch (daclinfo->defaclobjtype)
	{
		case DEFACLOBJ_RELATION:
			type = "TABLES";
			break;
		case DEFACLOBJ_SEQUENCE:
			type = "SEQUENCES";
			break;
		case DEFACLOBJ_FUNCTION:
			type = "FUNCTIONS";
			break;
		case DEFACLOBJ_TYPE:
			type = "TYPES";
			break;
		case DEFACLOBJ_NAMESPACE:
			type = "SCHEMAS";
			break;
		default:
			/* 不应该到达这里 */
			pg_fatal("unrecognized object type in default privileges: %d",
					 (int) daclinfo->defaclobjtype);
			type = "";			/* 使编译器保持安静 */
	}

	appendPQExpBuffer(tag, "DEFAULT PRIVILEGES FOR %s", type);

	/* 为此元组构建实际的命令 */
	if (!buildDefaultACLCommands(type,
								 daclinfo->dobj.namespace != NULL ?
								 daclinfo->dobj.namespace->dobj.name : NULL,
								 daclinfo->dacl.acl,
								 daclinfo->dacl.acldefault,
								 daclinfo->defaclrole,
								 fout->remoteVersion,
								 q))
		pg_fatal("could not parse default ACL list (%s)",
				 daclinfo->dacl.acl);

	if (daclinfo->dobj.dump & DUMP_COMPONENT_ACL)
		ArchiveEntry(fout, daclinfo->dobj.catId, daclinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = daclinfo->dobj.namespace ?
								  daclinfo->dobj.namespace->dobj.name : NULL,
								  .owner = daclinfo->defaclrole,
								  .description = "DEFAULT ACL",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data));

	destroyPQExpBuffer(tag);
	destroyPQExpBuffer(q);
}

/*
 * ----------
 *  输出授予/撤销信息
 * 
 *  'objDumpId' 是底层对象的导出 ID。
 *  'altDumpId' 可以是 ACL 条目还必须依赖的第二个 dumpId，
 *      或者如果没有第二个依赖的需要则为 InvalidDumpId。
 *  'type' 必须是以下之一
 *      TABLE、SEQUENCE、FUNCTION、LANGUAGE、SCHEMA、DATABASE、
 *      TABLESPACE、FOREIGN DATA WRAPPER、SERVER 或 LARGE OBJECT。
 *  'name' 是对象的格式化名称。必须已加引号等。
 *  'subname' 是子对象的格式化名称（如果有）。必须加引号。
 *      （当前我们假设 subname 仅为表列提供。）
 *  'nspname' 是对象所在的命名空间（如果没有则为 NULL）。
 *  'owner' 是所有者，如果没有所有者则为 NULL（对于语言）。
 *  'dacl' 是对象的 DumpableAcl 结构。
 * 
 *  返回分配给 ACL TocEntry 的导出 ID，如果未创建 ACL 条目
 *  则返回 InvalidDumpId。
 */
static DumpId
dumpACL(Archive *fout, DumpId objDumpId, DumpId altDumpId,
		const char *type, const char *name, const char *subname,
		const char *nspname, const char *owner,
		const DumpableAcl *dacl)
{
	DumpId		aclDumpId = InvalidDumpId;
	DumpOptions *dopt = fout->dopt;
	const char *acls = dacl->acl;
	const char *acldefault = dacl->acldefault;
	char		privtype = dacl->privtype;
	const char *initprivs = dacl->initprivs;
	const char *baseacls;
	PQExpBuffer sql;

	/* 如果未启用 ACL 导出则不执行任何操作 */
	if (dopt->aclsSkip)
		return InvalidDumpId;

	/* --data-only 跳过 ACL，但大对象 ACL 除外 */
	if (dopt->dataOnly && strcmp(type, "LARGE OBJECT") != 0)
		return InvalidDumpId;

	sql = createPQExpBuffer();

	/*
	 * 在二进制升级模式下，我们不运行扩展的脚本，而是
	 * 独立导出对象然后重新创建它们。为了保留扩展对象上
	 * 设置的任何初始权限，我们需要计算从对象的默认权限
	 * 到其 pg_init_privs 中记录的初始权限所需的 GRANT 和
	 * REVOKE 命令集。
	 * 
	 * 在恢复时，我们在调用
	 * binary_upgrade_set_record_init_privs(true) 之后应用
	 * 这些命令。这告诉后端将结果复制到 pg_init_privs 中。
	 * 这就是我们跨二进制升级保留该目录内容的方式。
	 *
	 */
	if (dopt->binary_upgrade && privtype == 'e' &&
		initprivs && *initprivs != '\0')
	{
		appendPQExpBufferStr(sql, "SELECT pg_catalog.binary_upgrade_set_record_init_privs(true);\n");
		if (!buildACLCommands(name, subname, nspname, type,
							  initprivs, acldefault, owner,
							  "", fout->remoteVersion, sql))
			pg_fatal("could not parse initial ACL list (%s) or default (%s) for object \"%s\" (%s)",
					 initprivs, acldefault, name, type);
		appendPQExpBufferStr(sql, "SELECT pg_catalog.binary_upgrade_set_record_init_privs(false);\n");
	}

	/*
	 * 现在计算从 initprivs（如果提供）或从对象类型特定的
	 * 默认值开始，到达对象的实际当前 ACL 所需的 GRANT 和
	 * REVOKE 命令。此外，虽然 buildACLCommands 会假定
	 * NULL/空 acls 字符串意味着不需要执行任何操作，但实际
	 * 表示的是对象类型特定的默认值；因此我们需要在这种情况下
	 * 替换 acldefault 字符串以获得正确的结果。
	 */
	if (initprivs && *initprivs != '\0')
	{
		baseacls = initprivs;
		if (acls == NULL || *acls == '\0')
			acls = acldefault;
	}
	else
		baseacls = acldefault;

	if (!buildACLCommands(name, subname, nspname, type,
						  acls, baseacls, owner,
						  "", fout->remoteVersion, sql))
		pg_fatal("could not parse ACL list (%s) or default (%s) for object \"%s\" (%s)",
				 acls, baseacls, name, type);

	if (sql->len > 0)
	{
		PQExpBuffer tag = createPQExpBuffer();
		DumpId		aclDeps[2];
		int			nDeps = 0;

		if (subname)
			appendPQExpBuffer(tag, "COLUMN %s.%s", name, subname);
		else
			appendPQExpBuffer(tag, "%s %s", type, name);

		aclDeps[nDeps++] = objDumpId;
		if (altDumpId != InvalidDumpId)
			aclDeps[nDeps++] = altDumpId;

		aclDumpId = createDumpId();

		ArchiveEntry(fout, nilCatalogId, aclDumpId,
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = nspname,
								  .owner = owner,
								  .description = "ACL",
								  .section = SECTION_NONE,
								  .createStmt = sql->data,
								  .deps = aclDeps,
								  .nDeps = nDeps));

		destroyPQExpBuffer(tag);
	}

	destroyPQExpBuffer(sql);

	return aclDumpId;
}

/*
 * dumpSecLabel
 * 
 * 此例程用于导出与传递给此例程的对象关联的任何
 * 安全标签。该例程接受对象类型和对象名称（可直接输出，
 * 仅需要装饰模式名），加上对象的命名空间和所有者
 * （用于标记 ArchiveEntry），加上目录 ID 和 subid
 * （作为 pg_seclabel 的查找键），加上对象的导出 ID
 * （用于设置依赖）。如果找到匹配的 pg_seclabel 条目，
 * 则导出。
 * 
 * 注意：虽然此例程接受一个 dumpId 用于依赖目的，
 * 但该目的仅是在发出的导出文件中标记依赖，
 * 以供 pg_restore 将来使用。我们不会将其用于确定
 * 标签在导出文件中的排序，因为此例程在依赖排序
 * 之后调用。此例程应在为指定对象调用
 * ArchiveEntry() 之后立即调用。
 */
static void
dumpSecLabel(Archive *fout, const char *type, const char *name,
			 const char *namespace, const char *owner,
			 CatalogId catalogId, int subid, DumpId dumpId)
{
	DumpOptions *dopt = fout->dopt;
	SecLabelItem *labels;
	int			nlabels;
	int			i;
	PQExpBuffer query;

	/* 如果提供了 --no-security-labels，则不执行任何操作 */
	if (dopt->no_security_labels)
		return;

	/*
	 * 安全标签是模式而非数据 ... 但大对象标签是数据
	 *
	 */
	if (strcmp(type, "LARGE OBJECT") != 0)
	{
		if (dopt->dataOnly)
			return;
	}
	else
	{
		/* 我们在二进制升级模式下确实会导出大对象安全标签 */
		if (dopt->schemaOnly && !dopt->binary_upgrade)
			return;
	}

	/* 使用表搜索与 catalogId 关联的安全标签 */
	nlabels = findSecLabels(catalogId.tableoid, catalogId.oid, &labels);

	query = createPQExpBuffer();

	for (i = 0; i < nlabels; i++)
	{
		/*
		 * 忽略 subid 不匹配的标签条目。
		 */
		if (labels[i].objsubid != subid)
			continue;

		appendPQExpBuffer(query,
						  "SECURITY LABEL FOR %s ON %s ",
						  fmtId(labels[i].provider), type);
		if (namespace && *namespace)
			appendPQExpBuffer(query, "%s.", fmtId(namespace));
		appendPQExpBuffer(query, "%s IS ", name);
		appendStringLiteralAH(query, labels[i].label, fout);
		appendPQExpBufferStr(query, ";\n");
	}

	if (query->len > 0)
	{
		PQExpBuffer tag = createPQExpBuffer();

		appendPQExpBuffer(tag, "%s %s", type, name);
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tag->data,
								  .namespace = namespace,
								  .owner = owner,
								  .description = "SECURITY LABEL",
								  .section = SECTION_NONE,
								  .createStmt = query->data,
								  .deps = &dumpId,
								  .nDeps = 1));
		destroyPQExpBuffer(tag);
	}

	destroyPQExpBuffer(query);
}

/*
 * dumpTableSecLabel
 * 
 * 如上所述，但为指定表（或视图）及其列导出安全标签。
 *
 */
static void
dumpTableSecLabel(Archive *fout, const TableInfo *tbinfo, const char *reltypename)
{
	DumpOptions *dopt = fout->dopt;
	SecLabelItem *labels;
	int			nlabels;
	int			i;
	PQExpBuffer query;
	PQExpBuffer target;

	/* 如果提供了 --no-security-labels，则不执行任何操作 */
	if (dopt->no_security_labels)
		return;

	/* 安全标签是模式而非数据 */
	if (dopt->dataOnly)
		return;

	/* 使用表搜索与关系关联的注释 */
	nlabels = findSecLabels(tbinfo->dobj.catId.tableoid,
							tbinfo->dobj.catId.oid,
							&labels);

	/* 如果安全标签存在，构建 SECURITY LABEL 语句 */
	if (nlabels <= 0)
		return;

	query = createPQExpBuffer();
	target = createPQExpBuffer();

	for (i = 0; i < nlabels; i++)
	{
		const char *colname;
		const char *provider = labels[i].provider;
		const char *label = labels[i].label;
		int			objsubid = labels[i].objsubid;

		resetPQExpBuffer(target);
		if (objsubid == 0)
		{
			appendPQExpBuffer(target, "%s %s", reltypename,
							  fmtQualifiedDumpable(tbinfo));
		}
		else
		{
			colname = getAttrName(objsubid, tbinfo);
			/* 在再次调用之前必须先消耗第一次 fmtXXX 结果 */
			appendPQExpBuffer(target, "COLUMN %s",
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(target, ".%s", fmtId(colname));
		}
		appendPQExpBuffer(query, "SECURITY LABEL FOR %s ON %s IS ",
						  fmtId(provider), target->data);
		appendStringLiteralAH(query, label, fout);
		appendPQExpBufferStr(query, ";\n");
	}
	if (query->len > 0)
	{
		resetPQExpBuffer(target);
		appendPQExpBuffer(target, "%s %s", reltypename,
						  fmtId(tbinfo->dobj.name));
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = target->data,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "SECURITY LABEL",
								  .section = SECTION_NONE,
								  .createStmt = query->data,
								  .deps = &(tbinfo->dobj.dumpId),
								  .nDeps = 1));
	}
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(target);
}

/*
 * findSecLabels
 * 
 * 查找与给定对象关联的安全标签（如果有）。与给定
 * classoid/objoid 关联的所有 objsubid 值通过一次
 * 搜索找到。
 */
static int
findSecLabels(Oid classoid, Oid objoid, SecLabelItem **items)
{
	SecLabelItem *middle = NULL;
	SecLabelItem *low;
	SecLabelItem *high;
	int			nmatch;

	if (nseclabels <= 0)		/* 没有标签，因此不可能匹配 */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * 执行二分查找以找到与对象匹配的某个条目。
	 */
	low = &seclabels[0];
	high = &seclabels[nseclabels - 1];
	while (low <= high)
	{
		middle = low + (high - low) / 2;

		if (classoid < middle->classoid)
			high = middle - 1;
		else if (classoid > middle->classoid)
			low = middle + 1;
		else if (objoid < middle->objoid)
			high = middle - 1;
		else if (objoid > middle->objoid)
			low = middle + 1;
		else
			break;				/* 找到匹配 */
	}

	if (low > high)				/* 无匹配 */
	{
		*items = NULL;
		return 0;
	}

	/*
	 * 现在确定有多少条目与对象匹配。搜索循环不变式
	 * 仍然成立：只有 low 和 high 之间（含）的条目可能匹配。
	 *
	 */
	nmatch = 1;
	while (middle > low)
	{
		if (classoid != middle[-1].classoid ||
			objoid != middle[-1].objoid)
			break;
		middle--;
		nmatch++;
	}

	*items = middle;

	middle += nmatch;
	while (middle <= high)
	{
		if (classoid != middle->classoid ||
			objoid != middle->objoid)
			break;
		middle++;
		nmatch++;
	}

	return nmatch;
}

/*
 * collectSecLabels
 * 
 * 构建一个包含所有可用于数据库对象的安全标签的表；
 * 还为每个相关对象设置 has-seclabel 组件标志。
 * 
 * 该表按 classoid/objid/objsubid 排序以提高查找速度。
 */
static void
collectSecLabels(Archive *fout)
{
	PGresult   *res;
	PQExpBuffer query;
	int			i_label;
	int			i_provider;
	int			i_classoid;
	int			i_objoid;
	int			i_objsubid;
	int			ntups;
	int			i;
	DumpableObject *dobj;

	query = createPQExpBuffer();

	appendPQExpBufferStr(query,
						 "SELECT label, provider, classoid, objoid, objsubid "
						 "FROM pg_catalog.pg_seclabels "
						 "ORDER BY classoid, objoid, objsubid");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	/* 构建包含数值形式 OID 的查找表 */
	i_label = PQfnumber(res, "label");
	i_provider = PQfnumber(res, "provider");
	i_classoid = PQfnumber(res, "classoid");
	i_objoid = PQfnumber(res, "objoid");
	i_objsubid = PQfnumber(res, "objsubid");

	ntups = PQntuples(res);

	seclabels = (SecLabelItem *) pg_malloc(ntups * sizeof(SecLabelItem));
	nseclabels = 0;
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		int			subid;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classoid));
		objId.oid = atooid(PQgetvalue(res, i, i_objoid));
		subid = atoi(PQgetvalue(res, i, i_objsubid));

		/* 我们不需要记住不匹配任何可导出对象的标签 */
		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);
		if (dobj == NULL)
			continue;

		/*
		 * 复合类型的列上的标签链接到该类型的 pg_class 条目，
		 * 但我们需要在该类型自身的 DumpableObject 中设置
		 * DUMP_COMPONENT_SECLABEL 标志。
		 */
		if (subid != 0 && dobj->objType == DO_TABLE &&
			((TableInfo *) dobj)->relkind == RELKIND_COMPOSITE_TYPE)
		{
			TypeInfo   *cTypeInfo;

			cTypeInfo = findTypeByOid(((TableInfo *) dobj)->reltype);
			if (cTypeInfo)
				cTypeInfo->dobj.components |= DUMP_COMPONENT_SECLABEL;
		}
		else
			dobj->components |= DUMP_COMPONENT_SECLABEL;

		seclabels[nseclabels].label = pg_strdup(PQgetvalue(res, i, i_label));
		seclabels[nseclabels].provider = pg_strdup(PQgetvalue(res, i, i_provider));
		seclabels[nseclabels].classoid = objId.tableoid;
		seclabels[nseclabels].objoid = objId.oid;
		seclabels[nseclabels].objsubid = subid;
		nseclabels++;
	}

	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * dumpTable
 *   输出到 fout 用户定义表的声明（非数据）
 */
static void
dumpTable(Archive *fout, const TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	DumpId		tableAclDumpId = InvalidDumpId;
	char	   *namecopy;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		if (tbinfo->relkind == RELKIND_SEQUENCE)
			dumpSequence(fout, tbinfo);
		else
			dumpTableSchema(fout, tbinfo);
	}

	/* 在此处理 ACL */
	namecopy = pg_strdup(fmtId(tbinfo->dobj.name));
	if (tbinfo->dobj.dump & DUMP_COMPONENT_ACL)
	{
		const char *objtype =
			(tbinfo->relkind == RELKIND_SEQUENCE) ? "SEQUENCE" : "TABLE";

		tableAclDumpId =
			dumpACL(fout, tbinfo->dobj.dumpId, InvalidDumpId,
					objtype, namecopy, NULL,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					&tbinfo->dacl);
	}

	/*
	 * 处理列 ACL（如果有）。注意：我们通过单独的查询拉取
	 * 这些，而不是在 getTableAttrs 期间获取它们，以便不会
	 * 遗漏系统列上的 ACL。这种方式还允许我们为在 getTables
	 * 中没有标记为"感兴趣"的目录导出 ACL。
	 */
	if ((tbinfo->dobj.dump & DUMP_COMPONENT_ACL) && tbinfo->hascolumnACLs)
	{
		PQExpBuffer query = createPQExpBuffer();
		PGresult   *res;
		int			i;

		if (!fout->is_prepared[PREPQUERY_GETCOLUMNACLS])
		{
			/* 设置查询以获取列 ACL */
			appendPQExpBufferStr(query,
								 "PREPARE getColumnACLs(pg_catalog.oid) AS\n");

			if (fout->remoteVersion >= 90600)
			{
				/*
				 * 原则上我们应该调用 acldefault('c', relowner) 来获取
				 * 列的默认 ACL。但是，我们目前不在 TableInfo 中存储
				 * relowner 的数值 OID。我们可以使用 regrole 转换所有者
				 * 名称，但这会因并发的角色重命名而产生失败风险。
				 * 鉴于列的默认 ACL 为空且可能保持这种状态，
				 * 在这里硬编码这些知识比额外的周期和风险更值得。
				 *
				 *
				 */
				appendPQExpBufferStr(query,
									 "SELECT at.attname, "
									 "at.attacl, "
									 "'{}' AS acldefault, "
									 "pip.privtype, pip.initprivs "
									 "FROM pg_catalog.pg_attribute at "
									 "LEFT JOIN pg_catalog.pg_init_privs pip ON "
									 "(at.attrelid = pip.objoid "
									 "AND pip.classoid = 'pg_catalog.pg_class'::pg_catalog.regclass "
									 "AND at.attnum = pip.objsubid) "
									 "WHERE at.attrelid = $1 AND "
									 "NOT at.attisdropped "
									 "AND (at.attacl IS NOT NULL OR pip.initprivs IS NOT NULL) "
									 "ORDER BY at.attnum");
			}
			else
			{
				appendPQExpBufferStr(query,
									 "SELECT attname, attacl, '{}' AS acldefault, "
									 "NULL AS privtype, NULL AS initprivs "
									 "FROM pg_catalog.pg_attribute "
									 "WHERE attrelid = $1 AND NOT attisdropped "
									 "AND attacl IS NOT NULL "
									 "ORDER BY attnum");
			}

			ExecuteSqlStatement(fout, query->data);

			fout->is_prepared[PREPQUERY_GETCOLUMNACLS] = true;
		}

		printfPQExpBuffer(query,
						  "EXECUTE getColumnACLs('%u')",
						  tbinfo->dobj.catId.oid);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		for (i = 0; i < PQntuples(res); i++)
		{
			char	   *attname = PQgetvalue(res, i, 0);
			char	   *attacl = PQgetvalue(res, i, 1);
			char	   *acldefault = PQgetvalue(res, i, 2);
			char		privtype = *(PQgetvalue(res, i, 3));
			char	   *initprivs = PQgetvalue(res, i, 4);
			DumpableAcl coldacl;
			char	   *attnamecopy;

			coldacl.acl = attacl;
			coldacl.acldefault = acldefault;
			coldacl.privtype = privtype;
			coldacl.initprivs = initprivs;
			attnamecopy = pg_strdup(fmtId(attname));

			/*
			 * 列的 GRANT 类型始终是 TABLE。每个列 ACL 依赖于
			 * 表级 ACL，因为我们可以并行恢复列 ACL，
			 * 但表级 ACL 必须先完成。
			 */
			dumpACL(fout, tbinfo->dobj.dumpId, tableAclDumpId,
					"TABLE", namecopy, attnamecopy,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					&coldacl);
			free(attnamecopy);
		}
		PQclear(res);
		destroyPQExpBuffer(query);
	}

	free(namecopy);
}

/*
 * 为视图或物化视图创建 AS 子句。去掉分号，
 * 因为物化视图必须添加 WITH NO DATA 子句。
 * 
 * 返回一个新的缓冲区，调用者必须释放。
 */
static PQExpBuffer
createViewAsClause(Archive *fout, const TableInfo *tbinfo)
{
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer result = createPQExpBuffer();
	PGresult   *res;
	int			len;

	/* 获取视图定义 */
	appendPQExpBuffer(query,
					  "SELECT pg_catalog.pg_get_viewdef('%u'::pg_catalog.oid) AS viewdef",
					  tbinfo->dobj.catId.oid);

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
	{
		if (PQntuples(res) < 1)
			pg_fatal("query to obtain definition of view \"%s\" returned no data",
					 tbinfo->dobj.name);
		else
			pg_fatal("query to obtain definition of view \"%s\" returned more than one definition",
					 tbinfo->dobj.name);
	}

	len = PQgetlength(res, 0, 0);

	if (len == 0)
		pg_fatal("definition of view \"%s\" appears to be empty (length zero)",
				 tbinfo->dobj.name);

	/* 去掉尾部分号，以便其他内容可以跟随。 */
	Assert(PQgetvalue(res, 0, 0)[len - 1] == ';');
	appendBinaryPQExpBuffer(result, PQgetvalue(res, 0, 0), len - 1);

	PQclear(res);
	destroyPQExpBuffer(query);

	return result;
}

/*
 * 为视图创建一个虚拟 AS 子句。当真实的视图定义因
 * 循环依赖而必须推迟时使用。我们必须复制视图的外部
 * 属性 —— 列名和类型（包括排序规则）—— 以便它能
 * 用于后续引用。
 * 
 * 返回一个新的缓冲区，调用者必须释放。
 */
static PQExpBuffer
createDummyViewAsClause(Archive *fout, const TableInfo *tbinfo)
{
	PQExpBuffer result = createPQExpBuffer();
	int			j;

	appendPQExpBufferStr(result, "SELECT");

	for (j = 0; j < tbinfo->numatts; j++)
	{
		if (j > 0)
			appendPQExpBufferChar(result, ',');
		appendPQExpBufferStr(result, "\n    ");

		appendPQExpBuffer(result, "NULL::%s", tbinfo->atttypnames[j]);

		/*
		 * 如果排序规则不是类型的默认值，则必须添加，
		 * 因为 CREATE OR REPLACE VIEW 不会更改它
		 */
		if (OidIsValid(tbinfo->attcollation[j]))
		{
			CollInfo   *coll;

			coll = findCollationByOid(tbinfo->attcollation[j]);
			if (coll)
				appendPQExpBuffer(result, " COLLATE %s",
								  fmtQualifiedDumpable(coll));
		}

		appendPQExpBuffer(result, " AS %s", fmtId(tbinfo->attnames[j]));
	}

	return result;
}

/*
 * dumpTableSchema
 *   写出一个用户定义表或视图的声明（非数据）
 */
static void
dumpTableSchema(Archive *fout, const TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q = createPQExpBuffer();
	PQExpBuffer delq = createPQExpBuffer();
	char	   *qrelname;
	char	   *qualrelname;
	int			numParents;
	TableInfo **parents;
	int			actual_atts;	/* 此 CREATE 语句中的属性数 */
	const char *reltypename;
	char	   *storage;
	int			j,
				k;

	/* 我们最好已经加载了此表的每列详细信息 */
	Assert(tbinfo->interesting);

	qrelname = pg_strdup(fmtId(tbinfo->dobj.name));
	qualrelname = pg_strdup(fmtQualifiedDumpable(tbinfo));

	if (tbinfo->hasoids)
		pg_log_warning("WITH OIDS is not supported anymore (table \"%s\")",
					   qrelname);

	if (dopt->binary_upgrade)
		binary_upgrade_set_type_oids_by_rel(fout, q, tbinfo);

	/* 是表还是视图？ */
	if (tbinfo->relkind == RELKIND_VIEW)
	{
		PQExpBuffer result;

		/*
		 * 注意：使此代码与 dumpRule() 中的 is_view 情况保持同步
		 */

		reltypename = "VIEW";

		appendPQExpBuffer(delq, "DROP VIEW %s;\n", qualrelname);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 tbinfo->dobj.catId.oid, false);

		appendPQExpBuffer(q, "CREATE VIEW %s", qualrelname);

		if (tbinfo->dummy_view)
			result = createDummyViewAsClause(fout, tbinfo);
		else
		{
			if (nonemptyReloptions(tbinfo->reloptions))
			{
				appendPQExpBufferStr(q, " WITH (");
				appendReloptionsArrayAH(q, tbinfo->reloptions, "", fout);
				appendPQExpBufferChar(q, ')');
			}
			result = createViewAsClause(fout, tbinfo);
		}
		appendPQExpBuffer(q, " AS\n%s", result->data);
		destroyPQExpBuffer(result);

		if (tbinfo->checkoption != NULL && !tbinfo->dummy_view)
			appendPQExpBuffer(q, "\n  WITH %s CHECK OPTION", tbinfo->checkoption);
		appendPQExpBufferStr(q, ";\n");
	}
	else
	{
		char	   *partkeydef = NULL;
		char	   *ftoptions = NULL;
		char	   *srvname = NULL;
		char	   *foreign = "";

		/*
		 * 设置 reltypename，并收集我们在 getTables() 期间
		 * 未获取的任何 relkind 特定数据。
		 */
		switch (tbinfo->relkind)
		{
			case RELKIND_PARTITIONED_TABLE:
				{
					PQExpBuffer query = createPQExpBuffer();
					PGresult   *res;

					reltypename = "TABLE";

					/* 检索分区键定义 */
					appendPQExpBuffer(query,
									  "SELECT pg_get_partkeydef('%u')",
									  tbinfo->dobj.catId.oid);
					res = ExecuteSqlQueryForSingleRow(fout, query->data);
					partkeydef = pg_strdup(PQgetvalue(res, 0, 0));
					PQclear(res);
					destroyPQExpBuffer(query);
					break;
				}
			case RELKIND_FOREIGN_TABLE:
				{
					PQExpBuffer query = createPQExpBuffer();
					PGresult   *res;
					int			i_srvname;
					int			i_ftoptions;

					reltypename = "FOREIGN TABLE";

					/* 检索外部服务器名称和通用选项 */
					appendPQExpBuffer(query,
									  "SELECT fs.srvname, "
									  "pg_catalog.array_to_string(ARRAY("
									  "SELECT pg_catalog.quote_ident(option_name) || "
									  "' ' || pg_catalog.quote_literal(option_value) "
									  "FROM pg_catalog.pg_options_to_table(ftoptions) "
									  "ORDER BY option_name"
									  "), E',\n    ') AS ftoptions "
									  "FROM pg_catalog.pg_foreign_table ft "
									  "JOIN pg_catalog.pg_foreign_server fs "
									  "ON (fs.oid = ft.ftserver) "
									  "WHERE ft.ftrelid = '%u'",
									  tbinfo->dobj.catId.oid);
					res = ExecuteSqlQueryForSingleRow(fout, query->data);
					i_srvname = PQfnumber(res, "srvname");
					i_ftoptions = PQfnumber(res, "ftoptions");
					srvname = pg_strdup(PQgetvalue(res, 0, i_srvname));
					ftoptions = pg_strdup(PQgetvalue(res, 0, i_ftoptions));
					PQclear(res);
					destroyPQExpBuffer(query);

					foreign = "FOREIGN ";
					break;
				}
			case RELKIND_MATVIEW:
				reltypename = "MATERIALIZED VIEW";
				break;
			default:
				reltypename = "TABLE";
				break;
		}

		numParents = tbinfo->numParents;
		parents = tbinfo->parents;

		appendPQExpBuffer(delq, "DROP %s %s;\n", reltypename, qualrelname);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 tbinfo->dobj.catId.oid, false);

		appendPQExpBuffer(q, "CREATE %s%s %s",
						  tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED ?
						  "UNLOGGED " : "",
						  reltypename,
						  qualrelname);

		/*
		 * 如果是 reloftype，则附加到类型；但在二进制升级情况下，
		 * 我们正常导出表，之后将其附加到类型。
		 */
		if (OidIsValid(tbinfo->reloftype) && !dopt->binary_upgrade)
			appendPQExpBuffer(q, " OF %s",
							  getFormattedTypeName(fout, tbinfo->reloftype,
												   zeroIsError));

		if (tbinfo->relkind != RELKIND_MATVIEW)
		{
			/* 导出属性 */
			actual_atts = 0;
			for (j = 0; j < tbinfo->numatts; j++)
			{
				/*
				 * 通常，如果在本表中本地定义且未删除则导出。
				 * 但对于二进制升级，我们将导出所有列，然后在下面
				 * 修正已删除和非本地的情况。
				 *
				 */
				if (shouldPrintColumn(dopt, tbinfo, j))
				{
					bool		print_default;
					bool		print_notnull;

					/*
					 * 默认值 —— 如果要单独输出或根本不输出则抑制。
					 *
					 */
					print_default = (tbinfo->attrdefs[j] != NULL &&
									 tbinfo->attrdefs[j]->dobj.dump &&
									 !tbinfo->attrdefs[j]->separate);

					/*
					 * Not Null 约束 —— 如果是继承的则抑制，
					 * 除非是分区，或在不起作用的二进制升级情况下。
					 *
					 */
					print_notnull = (tbinfo->notnull[j] &&
									 (!tbinfo->inhNotNull[j] ||
									  tbinfo->ispartition || dopt->binary_upgrade));

					/*
					 * 如果完全由 reloftype 定义则跳过列，
					 * 但在二进制升级中除外
					 */
					if (OidIsValid(tbinfo->reloftype) &&
						!print_default && !print_notnull &&
						!dopt->binary_upgrade)
						continue;

					/* 如果不是第一个属性则正确格式化 */
					if (actual_atts == 0)
						appendPQExpBufferStr(q, " (");
					else
						appendPQExpBufferChar(q, ',');
					appendPQExpBufferStr(q, "\n    ");
					actual_atts++;

					/* 属性名称 */
					appendPQExpBufferStr(q, fmtId(tbinfo->attnames[j]));

					if (tbinfo->attisdropped[j])
					{
						/*
						 * ALTER TABLE DROP COLUMN 清除 pg_attribute.atttypid，
						 * 因此我们不会获得有效的类型名称；插入 INTEGER 作为
						 * 权宜之计。我们稍后会清理。
						 *
						 */
						appendPQExpBufferStr(q, " INTEGER /* dummy */");
						/* 然后跳到下一列 */
						continue;
					}

					/*
					 * 属性类型；在创建类型化表（'OF type_name'）时不输出，
					 * 但在二进制升级模式下也在该情况下输出。
					 *
					 */
					if (dopt->binary_upgrade || !OidIsValid(tbinfo->reloftype))
					{
						appendPQExpBuffer(q, " %s",
										  tbinfo->atttypnames[j]);
					}

					if (print_default)
					{
						if (tbinfo->attgenerated[j] == ATTRIBUTE_GENERATED_STORED)
							appendPQExpBuffer(q, " GENERATED ALWAYS AS (%s) STORED",
											  tbinfo->attrdefs[j]->adef_expr);
						else
							appendPQExpBuffer(q, " DEFAULT %s",
											  tbinfo->attrdefs[j]->adef_expr);
					}


					if (print_notnull)
						appendPQExpBufferStr(q, " NOT NULL");

					/* 如果排序规则不是类型默认值则添加 */
					if (OidIsValid(tbinfo->attcollation[j]))
					{
						CollInfo   *coll;

						coll = findCollationByOid(tbinfo->attcollation[j]);
						if (coll)
							appendPQExpBuffer(q, " COLLATE %s",
											  fmtQualifiedDumpable(coll));
					}
				}
			}

			/*
			 * 添加非继承的 CHECK 约束（如果有）。
			 * 
			 * 对于分区，即使检查约束不是在本地定义的，也需要
			 * 包含它们，因为我们稍后将发出的 ALTER TABLE ATTACH
			 * PARTITION 期望约束存在。（不需要修正 conislocal：
			 * ATTACH PARTITION 会处理）
			 */
			for (j = 0; j < tbinfo->ncheck; j++)
			{
				ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

				if (constr->separate ||
					(!constr->conislocal && !tbinfo->ispartition))
					continue;

				if (actual_atts == 0)
					appendPQExpBufferStr(q, " (\n    ");
				else
					appendPQExpBufferStr(q, ",\n    ");

				appendPQExpBuffer(q, "CONSTRAINT %s ",
								  fmtId(constr->dobj.name));
				appendPQExpBufferStr(q, constr->condef);

				actual_atts++;
			}

			if (actual_atts)
				appendPQExpBufferStr(q, "\n)");
			else if (!(OidIsValid(tbinfo->reloftype) && !dopt->binary_upgrade))
			{
				/*
				 * 没有属性？我们必须有一个带括号的属性列表，
				 * 即使是空的，当不使用 OF TYPE 语法时。
				 */
				appendPQExpBufferStr(q, " (\n)");
			}

			/*
			 * 发出 INHERITS 子句（分区不适用），
			 * 但在二进制升级模式下除外。
			 */
			if (numParents > 0 && !tbinfo->ispartition &&
				!dopt->binary_upgrade)
			{
				appendPQExpBufferStr(q, "\nINHERITS (");
				for (k = 0; k < numParents; k++)
				{
					TableInfo  *parentRel = parents[k];

					if (k > 0)
						appendPQExpBufferStr(q, ", ");
					appendPQExpBufferStr(q, fmtQualifiedDumpable(parentRel));
				}
				appendPQExpBufferChar(q, ')');
			}

			if (tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
				appendPQExpBuffer(q, "\nPARTITION BY %s", partkeydef);

			if (tbinfo->relkind == RELKIND_FOREIGN_TABLE)
				appendPQExpBuffer(q, "\nSERVER %s", fmtId(srvname));
		}

		if (nonemptyReloptions(tbinfo->reloptions) ||
			nonemptyReloptions(tbinfo->toast_reloptions))
		{
			bool		addcomma = false;

			appendPQExpBufferStr(q, "\nWITH (");
			if (nonemptyReloptions(tbinfo->reloptions))
			{
				addcomma = true;
				appendReloptionsArrayAH(q, tbinfo->reloptions, "", fout);
			}
			if (nonemptyReloptions(tbinfo->toast_reloptions))
			{
				if (addcomma)
					appendPQExpBufferStr(q, ", ");
				appendReloptionsArrayAH(q, tbinfo->toast_reloptions, "toast.",
										fout);
			}
			appendPQExpBufferChar(q, ')');
		}

		/* 导出通用选项（如果有） */
		if (ftoptions && ftoptions[0])
			appendPQExpBuffer(q, "\nOPTIONS (\n    %s\n)", ftoptions);

		/*
		 * 对于物化视图，创建像视图一样的 AS 子句。
		 * 此时，我们始终将视图标记为未填充。
		 */
		if (tbinfo->relkind == RELKIND_MATVIEW)
		{
			PQExpBuffer result;

			result = createViewAsClause(fout, tbinfo);
			appendPQExpBuffer(q, " AS\n%s\n  WITH NO DATA;\n",
							  result->data);
			destroyPQExpBuffer(result);
		}
		else
			appendPQExpBufferStr(q, ";\n");

		/* 物化视图可以依赖于扩展 */
		if (tbinfo->relkind == RELKIND_MATVIEW)
			append_depends_on_extension(fout, q, &tbinfo->dobj,
										"pg_catalog.pg_class",
										"MATERIALIZED VIEW",
										qualrelname);

		/*
		 * 在二进制升级模式下，使用可能存在的任何缺失值
		 * 更新目录。
		 */
		if (dopt->binary_upgrade)
		{
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (tbinfo->attmissingval[j][0] != '\0')
				{
					appendPQExpBufferStr(q, "\n-- set missing value.\n");
					appendPQExpBufferStr(q,
										 "SELECT pg_catalog.binary_upgrade_set_missing_value(");
					appendStringLiteralAH(q, qualrelname, fout);
					appendPQExpBufferStr(q, "::pg_catalog.regclass,");
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBufferChar(q, ',');
					appendStringLiteralAH(q, tbinfo->attmissingval[j], fout);
					appendPQExpBufferStr(q, ");\n\n");
				}
			}
		}

		/*
		 * 要创建二进制兼容的堆文件，我们必须确保相同的
		 * 物理列顺序，包括已删除的列，与原始相同。因此，
		 * 我们在上面创建已删除的列并在此处删除它们，
		 * 同时更新它们的 attlen/attalign 值，以便已删除的
		 * 列可以被正确跳过。（我们不费心恢复原始的
		 * attbyval 设置。）此外，继承关系是通过执行
		 * ALTER TABLE INHERIT 而不是使用 INHERITS 子句来建立的
		 * —— 后者可能会搞乱列顺序。这也意味着我们必须注意
		 * 正确设置 attislocal，并修正任何继承的 CHECK 约束。
		 * 类似地，我们使用 ALTER TABLE / OF 在此处设置类型化表。
		 * 
		 * 我们在此处理外部表和分区表，尽管它们缺乏堆存储，
		 * 因为它们可以参与继承关系，并且我们希望这些
		 * 在继承树中保持一致。我们可以排除索引、TOAST 表、
		 * 序列和物化视图，即使它们有存储，因为我们不支持
		 * 在其中更改或删除列，也不能成为继承树的一部分。
		 *
		 *
		 *
		 */
		if (dopt->binary_upgrade &&
			(tbinfo->relkind == RELKIND_RELATION ||
			 tbinfo->relkind == RELKIND_FOREIGN_TABLE ||
			 tbinfo->relkind == RELKIND_PARTITIONED_TABLE))
		{
			for (j = 0; j < tbinfo->numatts; j++)
			{
				if (tbinfo->attisdropped[j])
				{
					appendPQExpBufferStr(q, "\n-- For binary upgrade, recreate dropped column.\n");
					appendPQExpBuffer(q, "UPDATE pg_catalog.pg_attribute\n"
									  "SET attlen = %d, "
									  "attalign = '%c', attbyval = false\n"
									  "WHERE attname = ",
									  tbinfo->attlen[j],
									  tbinfo->attalign[j]);
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBufferStr(q, "\n  AND attrelid = ");
					appendStringLiteralAH(q, qualrelname, fout);
					appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");

					if (tbinfo->relkind == RELKIND_RELATION ||
						tbinfo->relkind == RELKIND_PARTITIONED_TABLE)
						appendPQExpBuffer(q, "ALTER TABLE ONLY %s ",
										  qualrelname);
					else
						appendPQExpBuffer(q, "ALTER FOREIGN TABLE ONLY %s ",
										  qualrelname);
					appendPQExpBuffer(q, "DROP COLUMN %s;\n",
									  fmtId(tbinfo->attnames[j]));
				}
				else if (!tbinfo->attislocal[j])
				{
					appendPQExpBufferStr(q, "\n-- For binary upgrade, recreate inherited column.\n");
					appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_attribute\n"
										 "SET attislocal = false\n"
										 "WHERE attname = ");
					appendStringLiteralAH(q, tbinfo->attnames[j], fout);
					appendPQExpBufferStr(q, "\n  AND attrelid = ");
					appendStringLiteralAH(q, qualrelname, fout);
					appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");
				}
			}

			/*
			 * 添加继承的 CHECK 约束（如果有）。
			 * 
			 * 对于分区，它们已经导出，且 conislocal 不需要修正。
			 *
			 */
			for (k = 0; k < tbinfo->ncheck; k++)
			{
				ConstraintInfo *constr = &(tbinfo->checkexprs[k]);

				if (constr->separate || constr->conislocal || tbinfo->ispartition)
					continue;

				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up inherited constraint.\n");
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ADD CONSTRAINT %s %s;\n",
								  foreign, qualrelname,
								  fmtId(constr->dobj.name),
								  constr->condef);
				appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_constraint\n"
									 "SET conislocal = false\n"
									 "WHERE contype = 'c' AND conname = ");
				appendStringLiteralAH(q, constr->dobj.name, fout);
				appendPQExpBufferStr(q, "\n  AND conrelid = ");
				appendStringLiteralAH(q, qualrelname, fout);
				appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");
			}

			if (numParents > 0 && !tbinfo->ispartition)
			{
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up inheritance this way.\n");
				for (k = 0; k < numParents; k++)
				{
					TableInfo  *parentRel = parents[k];

					appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s INHERIT %s;\n", foreign,
									  qualrelname,
									  fmtQualifiedDumpable(parentRel));
				}
			}

			if (OidIsValid(tbinfo->reloftype))
			{
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set up typed tables this way.\n");
				appendPQExpBuffer(q, "ALTER TABLE ONLY %s OF %s;\n",
								  qualrelname,
								  getFormattedTypeName(fout, tbinfo->reloftype,
													   zeroIsError));
			}
		}

		/*
		 * 在二进制升级模式下，安排恢复所有可 vacuum 的关系
		 * 的旧 relfrozenxid 和 relminmxid。（虽然 vacuum.c
		 * 半独立地处理 TOAST 表，但这里我们只将它们视为
		 * 其他关系的子表；因此此 "if" 缺少 RELKIND_TOASTVALUE，
		 * 并且子 TOAST 表在下面处理。）
		 */
		if (dopt->binary_upgrade &&
			(tbinfo->relkind == RELKIND_RELATION ||
			 tbinfo->relkind == RELKIND_MATVIEW))
		{
			appendPQExpBufferStr(q, "\n-- For binary upgrade, set heap's relfrozenxid and relminmxid\n");
			appendPQExpBuffer(q, "UPDATE pg_catalog.pg_class\n"
							  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
							  "WHERE oid = ",
							  tbinfo->frozenxid, tbinfo->minmxid);
			appendStringLiteralAH(q, qualrelname, fout);
			appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");

			if (tbinfo->toast_oid)
			{
				/*
				 * TOAST 表在恢复时具有相同的 OID，因此我们可以
				 * 安全地按 OID 定位它。
				 */
				appendPQExpBufferStr(q, "\n-- For binary upgrade, set toast's relfrozenxid and relminmxid\n");
				appendPQExpBuffer(q, "UPDATE pg_catalog.pg_class\n"
								  "SET relfrozenxid = '%u', relminmxid = '%u'\n"
								  "WHERE oid = '%u';\n",
								  tbinfo->toast_frozenxid,
								  tbinfo->toast_minmxid, tbinfo->toast_oid);
			}
		}

		/*
		 * 在二进制升级模式下，通过直接修改 pg_class 来恢复
		 * 物化视图的填充状态。这相当丑陋，但我们不能使用
		 * REFRESH MATERIALIZED VIEW，因为即使此物化视图已填充，
		 * 某些底层物化视图可能未填充；无论如何，我们想传输
		 * 物化视图的堆存储，而不是运行 REFRESH。
		 */
		if (dopt->binary_upgrade && tbinfo->relkind == RELKIND_MATVIEW &&
			tbinfo->relispopulated)
		{
			appendPQExpBufferStr(q, "\n-- For binary upgrade, mark materialized view as populated\n");
			appendPQExpBufferStr(q, "UPDATE pg_catalog.pg_class\n"
								 "SET relispopulated = 't'\n"
								 "WHERE oid = ");
			appendStringLiteralAH(q, qualrelname, fout);
			appendPQExpBufferStr(q, "::pg_catalog.regclass;\n");
		}

		/*
		 * 导出在主 CREATE TABLE 命令中无法处理的附加每列属性。
		 *
		 */
		for (j = 0; j < tbinfo->numatts; j++)
		{
			/* 这些都不适用于已删除的列 */
			if (tbinfo->attisdropped[j])
				continue;

			/*
			 * 如果我们在上面没有显式导出列定义，并且它是
			 * NOT NULL 且未从父表继承该属性，则必须单独标记。
			 *
			 */
			if (!shouldPrintColumn(dopt, tbinfo, j) &&
				tbinfo->notnull[j] && !tbinfo->inhNotNull[j])
				appendPQExpBuffer(q,
								  "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET NOT NULL;\n",
								  foreign, qualrelname,
								  fmtId(tbinfo->attnames[j]));

			/*
			 * 导出每列统计信息。我们仅在列的 attstattarget
			 * 条目为非负值时发出 ALTER TABLE 语句
			 * （即它不是默认值）
			 */
			if (tbinfo->attstattarget[j] >= 0)
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET STATISTICS %d;\n",
								  foreign, qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attstattarget[j]);

			/*
			 * 导出每列存储信息。仅在存储已从类型默认值
			 * 更改时才导出该语句。
			 */
			if (tbinfo->attstorage[j] != tbinfo->typstorage[j])
			{
				switch (tbinfo->attstorage[j])
				{
					case TYPSTORAGE_PLAIN:
						storage = "PLAIN";
						break;
					case TYPSTORAGE_EXTERNAL:
						storage = "EXTERNAL";
						break;
					case TYPSTORAGE_EXTENDED:
						storage = "EXTENDED";
						break;
					case TYPSTORAGE_MAIN:
						storage = "MAIN";
						break;
					default:
						storage = NULL;
				}

				/*
				 * 仅在我们识别的存储类型时才导出该语句
				 */
				if (storage != NULL)
					appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET STORAGE %s;\n",
									  foreign, qualrelname,
									  fmtId(tbinfo->attnames[j]),
									  storage);
			}

			/*
			 * 导出每列压缩设置（如果有）。
			 */
			if (!dopt->no_toast_compression)
			{
				const char *cmname;

				switch (tbinfo->attcompression[j])
				{
					case 'p':
						cmname = "pglz";
						break;
					case 'l':
						cmname = "lz4";
						break;
					default:
						cmname = NULL;
						break;
				}

				if (cmname != NULL)
					appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET COMPRESSION %s;\n",
									  foreign, qualrelname,
									  fmtId(tbinfo->attnames[j]),
									  cmname);
			}

			/*
			 * 导出每列属性。
			 */
			if (tbinfo->attoptions[j][0] != '\0')
				appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET (%s);\n",
								  foreign, qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attoptions[j]);

			/*
			 * 导出每列 FDW 选项。
			 */
			if (tbinfo->relkind == RELKIND_FOREIGN_TABLE &&
				tbinfo->attfdwoptions[j][0] != '\0')
				appendPQExpBuffer(q,
								  "ALTER FOREIGN TABLE %s ALTER COLUMN %s OPTIONS (\n"
								  "    %s\n"
								  ");\n",
								  qualrelname,
								  fmtId(tbinfo->attnames[j]),
								  tbinfo->attfdwoptions[j]);
		}						/* 结束列循环 */

		free(partkeydef);
		free(ftoptions);
		free(srvname);
	}

	/*
	 * 导出我们只有 ALTER TABLE 语法的属性
	 */
	if ((tbinfo->relkind == RELKIND_RELATION ||
		 tbinfo->relkind == RELKIND_PARTITIONED_TABLE ||
		 tbinfo->relkind == RELKIND_MATVIEW) &&
		tbinfo->relreplident != REPLICA_IDENTITY_DEFAULT)
	{
		if (tbinfo->relreplident == REPLICA_IDENTITY_INDEX)
		{
			/* 无需处理，将在导出索引时设置 */
		}
		else if (tbinfo->relreplident == REPLICA_IDENTITY_NOTHING)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY NOTHING;\n",
							  qualrelname);
		}
		else if (tbinfo->relreplident == REPLICA_IDENTITY_FULL)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY FULL;\n",
							  qualrelname);
		}
	}

	if (tbinfo->forcerowsec)
		appendPQExpBuffer(q, "\nALTER TABLE ONLY %s FORCE ROW LEVEL SECURITY;\n",
						  qualrelname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(q, &tbinfo->dobj,
										reltypename, qrelname,
										tbinfo->dobj.namespace->dobj.name);

	if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		char	   *tablespace = NULL;
		char	   *tableam = NULL;

		/*
		 * _selectTablespace() 依赖默认表空间中的表空间启用对象
		 * 具有 ""（空字符串）的表空间，而非表空间启用对象具有
		 * NULL 的表空间。getTables() 将默认表空间的
		 * tbinfo->reltablespace 设置为 ""（而非 NULL）。
		 *
		 */
		if (RELKIND_HAS_TABLESPACE(tbinfo->relkind))
			tablespace = tbinfo->reltablespace;

		if (RELKIND_HAS_TABLE_AM(tbinfo->relkind))
			tableam = tbinfo->amname;

		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .tablespace = tablespace,
								  .tableam = tableam,
								  .owner = tbinfo->rolname,
								  .description = reltypename,
								  .section = tbinfo->postponed_def ?
								  SECTION_POST_DATA : SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));
	}

	/* 导出表注释 */
	if (tbinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpTableComment(fout, tbinfo, reltypename);

	/* 导出表安全标签 */
	if (tbinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpTableSecLabel(fout, tbinfo, reltypename);

	/* 导出内联表约束的注释 */
	for (j = 0; j < tbinfo->ncheck; j++)
	{
		ConstraintInfo *constr = &(tbinfo->checkexprs[j]);

		if (constr->separate || !constr->conislocal)
			continue;

		if (constr->dobj.dump & DUMP_COMPONENT_COMMENT)
			dumpTableConstraintComment(fout, constr);
	}

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qrelname);
	free(qualrelname);
}

/*
 * dumpTableAttach
 *   向 fout 写入附加子分区的命令
 * 
 * 子分区总是通过单独创建然后使用 ATTACH PARTITION 来创建，
 * 而不是使用 CREATE TABLE ... PARTITION OF。这对于保留列布局
 * 中任何可能的差异很重要，允许在表空间不同时分配正确的
 * 表空间，并且可以在不恢复父表的情况下恢复分区。
 * （你会从 ATTACH PARTITION 命令收到错误，但可以忽略，
 * 或者如果你愿意，可以使用 "pg_restore -L" 跳过。）
 * 最后一点促使将 ATTACH PARTITION 视为完全独立的
 * ArchiveEntry，而不是在子分区的 ArchiveEntry 中发出。
 *
 *
 */
static void
dumpTableAttach(Archive *fout, const TableAttachInfo *attachinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PGresult   *res;
	char	   *partbound;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();

	if (!fout->is_prepared[PREPQUERY_DUMPTABLEATTACH])
	{
		/* 设置查询以获取 partbound 详细信息 */
		appendPQExpBufferStr(q,
							 "PREPARE dumpTableAttach(pg_catalog.oid) AS\n");

		appendPQExpBufferStr(q,
							 "SELECT pg_get_expr(c.relpartbound, c.oid) "
							 "FROM pg_class c "
							 "WHERE c.oid = $1");

		ExecuteSqlStatement(fout, q->data);

		fout->is_prepared[PREPQUERY_DUMPTABLEATTACH] = true;
	}

	printfPQExpBuffer(q,
					  "EXECUTE dumpTableAttach('%u')",
					  attachinfo->partitionTbl->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, q->data);
	partbound = PQgetvalue(res, 0, 0);

	/* 对父表执行 ALTER TABLE */
	printfPQExpBuffer(q,
					  "ALTER TABLE ONLY %s ",
					  fmtQualifiedDumpable(attachinfo->parentTbl));
	appendPQExpBuffer(q,
					  "ATTACH PARTITION %s %s;\n",
					  fmtQualifiedDumpable(attachinfo->partitionTbl),
					  partbound);

	/*
	 * 创建 drop 查询没有意义，因为 drop 是由表 drop 完成的。
	 * （如果你考虑更改这一点，也请参见 _printTocEntry()。）
	 * 虽然此对象实际上没有这样的所有权，但还是设置 owner 字段
	 * 以确保命令在恢复时由正确的角色运行。
	 *
	 */
	ArchiveEntry(fout, attachinfo->dobj.catId, attachinfo->dobj.dumpId,
				 ARCHIVE_OPTS(.tag = attachinfo->dobj.name,
							  .namespace = attachinfo->dobj.namespace->dobj.name,
							  .owner = attachinfo->partitionTbl->rolname,
							  .description = "TABLE ATTACH",
							  .section = SECTION_PRE_DATA,
							  .createStmt = q->data));

	PQclear(res);
	destroyPQExpBuffer(q);
}

/*
 * dumpAttrDef --- 导出属性的默认值声明
 */
static void
dumpAttrDef(Archive *fout, const AttrDefInfo *adinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = adinfo->adtable;
	int			adnum = adinfo->adnum;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qualrelname;
	char	   *tag;
	char	   *foreign;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/* 如果不是 "separate" 则跳过；它已在表定义中导出 */
	if (!adinfo->separate)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qualrelname = pg_strdup(fmtQualifiedDumpable(tbinfo));

	foreign = tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "";

	appendPQExpBuffer(q,
					  "ALTER %sTABLE ONLY %s ALTER COLUMN %s SET DEFAULT %s;\n",
					  foreign, qualrelname, fmtId(tbinfo->attnames[adnum - 1]),
					  adinfo->adef_expr);

	appendPQExpBuffer(delq, "ALTER %sTABLE %s ALTER COLUMN %s DROP DEFAULT;\n",
					  foreign, qualrelname,
					  fmtId(tbinfo->attnames[adnum - 1]));

	tag = psprintf("%s %s", tbinfo->dobj.name, tbinfo->attnames[adnum - 1]);

	if (adinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, adinfo->dobj.catId, adinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "DEFAULT",
								  .section = SECTION_PRE_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	free(tag);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qualrelname);
}

/*
 * getAttrName: 提取属性的正确名称
 * 
 * 数组 tblInfo->attnames[] 仅提供用户属性的名称；
 * 如果提供了系统属性号，我们必须伪造它。
 * 我们还做了一点边界检查以确保安全。
 */
static const char *
getAttrName(int attrnum, const TableInfo *tblInfo)
{
	if (attrnum > 0 && attrnum <= tblInfo->numatts)
		return tblInfo->attnames[attrnum - 1];
	switch (attrnum)
	{
		case SelfItemPointerAttributeNumber:
			return "ctid";
		case MinTransactionIdAttributeNumber:
			return "xmin";
		case MinCommandIdAttributeNumber:
			return "cmin";
		case MaxTransactionIdAttributeNumber:
			return "xmax";
		case MaxCommandIdAttributeNumber:
			return "cmax";
		case TableOidAttributeNumber:
			return "tableoid";
	}
	pg_fatal("invalid column number %d for table \"%s\"",
			 attrnum, tblInfo->dobj.name);
	return NULL;				/* 使编译器保持安静 */
}

/*
 * dumpIndex
 *   向 fout 写出用户定义的索引
 */
static void
dumpIndex(Archive *fout, const IndxInfo *indxinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = indxinfo->indextable;
	bool		is_constraint = (indxinfo->indexconstraint != 0);
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *qindxname;
	char	   *qqindxname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	qindxname = pg_strdup(fmtId(indxinfo->dobj.name));
	qqindxname = pg_strdup(fmtQualifiedDumpable(indxinfo));

	/*
	 * 如果有关联的约束，则不导出索引本身，
	 * 但导出其任何注释。（这是安全的，因为依赖排序
	 * 将确保约束先被发出。）注意，在这种情况下，
	 * 发出的注释必须显示为依赖于约束，而非索引。
	 *
	 */
	if (!is_constraint)
	{
		char	   *indstatcols = indxinfo->indstatcols;
		char	   *indstatvals = indxinfo->indstatvals;
		char	  **indstatcolsarray = NULL;
		char	  **indstatvalsarray = NULL;
		int			nstatcols = 0;
		int			nstatvals = 0;

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 indxinfo->dobj.catId.oid, true);

		/* 普通次级索引 */
		appendPQExpBuffer(q, "%s;\n", indxinfo->indexdef);

		/*
		 * 根据需要附加 ALTER TABLE 命令以设置我们只有
		 * ALTER TABLE 语法的属性。使此代码与 dumpConstraint
		 * 中的类似代码保持同步！
		 */

		/* 如果索引是聚集的，我们需要记录这一点。 */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtQualifiedDumpable(tbinfo));
			/* 索引名在此语法中不限定 */
			appendPQExpBuffer(q, " ON %s;\n",
							  qindxname);
		}

		/*
		 * 如果索引在某些列上有统计信息，
		 * 生成关联的 ALTER INDEX 查询。
		 */
		if (strlen(indstatcols) != 0 || strlen(indstatvals) != 0)
		{
			int			j;

			if (!parsePGArray(indstatcols, &indstatcolsarray, &nstatcols))
				pg_fatal("could not parse index statistic columns");
			if (!parsePGArray(indstatvals, &indstatvalsarray, &nstatvals))
				pg_fatal("could not parse index statistic values");
			if (nstatcols != nstatvals)
				pg_fatal("mismatched number of columns and values for index statistics");

			for (j = 0; j < nstatcols; j++)
			{
				appendPQExpBuffer(q, "ALTER INDEX %s ", qqindxname);

				/*
				 * 注意，这是列号，因此不应使用引号。
				 *
				 */
				appendPQExpBuffer(q, "ALTER COLUMN %s ",
								  indstatcolsarray[j]);
				appendPQExpBuffer(q, "SET STATISTICS %s;\n",
								  indstatvalsarray[j]);
			}
		}

		/* 索引可以依赖于扩展 */
		append_depends_on_extension(fout, q, &indxinfo->dobj,
									"pg_catalog.pg_class",
									"INDEX", qqindxname);

		/* 如果索引定义身份，我们需要记录这一点。 */
		if (indxinfo->indisreplident)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY USING",
							  fmtQualifiedDumpable(tbinfo));
			/* 索引名在此语法中不限定 */
			appendPQExpBuffer(q, " INDEX %s;\n",
							  qindxname);
		}

		/*
		 * 如果此索引是分区索引的成员，后端将不允许我们
		 * 单独删除它，因此不要尝试。当我们删除索引的表或
		 * 分区索引时，它会自动消失。（如果在使用 --clean 的
		 * 选择性恢复中，我们都不删除这两者，那么此索引也不会
		 * 被删除。但这没关系，即使你认为不好，后端也不会
		 * 允许我们以其他方式操作。）
		 *
		 */
		if (indxinfo->parentidx == 0)
			appendPQExpBuffer(delq, "DROP INDEX %s;\n", qqindxname);

		if (indxinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, indxinfo->dobj.catId, indxinfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = indxinfo->dobj.name,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .tablespace = indxinfo->tablespace,
									  .owner = tbinfo->rolname,
									  .description = "INDEX",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data,
									  .dropStmt = delq->data));

		free(indstatcolsarray);
		free(indstatvalsarray);
	}

	/* 导出索引注释 */
	if (indxinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "INDEX", qindxname,
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->rolname,
					indxinfo->dobj.catId, 0,
					is_constraint ? indxinfo->indexconstraint :
					indxinfo->dobj.dumpId);

	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	free(qindxname);
	free(qqindxname);
}

/*
 * dumpIndexAttach
 *   向 fout 写出分区索引附加子句
 */
static void
dumpIndexAttach(Archive *fout, const IndexAttachInfo *attachinfo)
{
	/* 在仅数据导出中不执行任何操作 */
	if (fout->dopt->dataOnly)
		return;

	if (attachinfo->partitionIdx->dobj.dump & DUMP_COMPONENT_DEFINITION)
	{
		PQExpBuffer q = createPQExpBuffer();

		appendPQExpBuffer(q, "ALTER INDEX %s ",
						  fmtQualifiedDumpable(attachinfo->parentIdx));
		appendPQExpBuffer(q, "ATTACH PARTITION %s;\n",
						  fmtQualifiedDumpable(attachinfo->partitionIdx));

		/*
		 * 不需要 dropStmt，因为当我们删除索引的表或分区索引时
		 * drop 是隐式完成的。此外，由于没有
		 * ALTER INDEX DETACH PARTITION 命令，无论如何也无法执行。
		 * （如果你考虑更改这一点，也请考虑如何处理 --if-exists。）
		 * 
		 * 虽然此对象实际上没有这样的所有权，但还是设置 owner 字段
		 * 以确保命令在恢复时由正确的角色运行。
		 *
		 *
		 */
		ArchiveEntry(fout, attachinfo->dobj.catId, attachinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = attachinfo->dobj.name,
								  .namespace = attachinfo->dobj.namespace->dobj.name,
								  .owner = attachinfo->parentIdx->indextable->rolname,
								  .description = "INDEX ATTACH",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data));

		destroyPQExpBuffer(q);
	}
}

/*
 * dumpStatisticsExt
 *   向 fout 写出扩展统计对象
 */
static void
dumpStatisticsExt(Archive *fout, const StatsExtInfo *statsextinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer q;
	PQExpBuffer delq;
	PQExpBuffer query;
	char	   *qstatsextname;
	PGresult   *res;
	char	   *stxdef;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();
	query = createPQExpBuffer();

	qstatsextname = pg_strdup(fmtId(statsextinfo->dobj.name));

	appendPQExpBuffer(query, "SELECT "
					  "pg_catalog.pg_get_statisticsobjdef('%u'::pg_catalog.oid)",
					  statsextinfo->dobj.catId.oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	stxdef = PQgetvalue(res, 0, 0);

	/* pg_get_statisticsobjdef 的结果除分号外是完整的 */
	appendPQExpBuffer(q, "%s;\n", stxdef);

	/*
	 * 我们仅在此统计对象的 stxstattarget 条目为非负时
	 * 发出 ALTER STATISTICS 语句（即它不是默认值）。
	 *
	 */
	if (statsextinfo->stattarget >= 0)
	{
		appendPQExpBuffer(q, "ALTER STATISTICS %s ",
						  fmtQualifiedDumpable(statsextinfo));
		appendPQExpBuffer(q, "SET STATISTICS %d;\n",
						  statsextinfo->stattarget);
	}

	appendPQExpBuffer(delq, "DROP STATISTICS %s;\n",
					  fmtQualifiedDumpable(statsextinfo));

	if (statsextinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, statsextinfo->dobj.catId,
					 statsextinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = statsextinfo->dobj.name,
								  .namespace = statsextinfo->dobj.namespace->dobj.name,
								  .owner = statsextinfo->rolname,
								  .description = "STATISTICS",
								  .section = SECTION_POST_DATA,
								  .createStmt = q->data,
								  .dropStmt = delq->data));

	/* 导出统计注释 */
	if (statsextinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "STATISTICS", qstatsextname,
					statsextinfo->dobj.namespace->dobj.name,
					statsextinfo->rolname,
					statsextinfo->dobj.catId, 0,
					statsextinfo->dobj.dumpId);

	PQclear(res);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
	destroyPQExpBuffer(query);
	free(qstatsextname);
}

/*
 * dumpConstraint
 *   向 fout 写出用户定义的约束
 */
static void
dumpConstraint(Archive *fout, const ConstraintInfo *coninfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer q;
	PQExpBuffer delq;
	char	   *tag = NULL;
	char	   *foreign;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	q = createPQExpBuffer();
	delq = createPQExpBuffer();

	foreign = tbinfo &&
		tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "";

	if (coninfo->contype == 'p' ||
		coninfo->contype == 'u' ||
		coninfo->contype == 'x')
	{
		/* 与索引相关的约束 */
		IndxInfo   *indxinfo;
		int			k;

		indxinfo = (IndxInfo *) findObjectByDumpId(coninfo->conindex);

		if (indxinfo == NULL)
			pg_fatal("missing index for constraint \"%s\"",
					 coninfo->dobj.name);

		if (dopt->binary_upgrade)
			binary_upgrade_set_pg_class_oids(fout, q,
											 indxinfo->dobj.catId.oid, true);

		appendPQExpBuffer(q, "ALTER %sTABLE ONLY %s\n", foreign,
						  fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s ",
						  fmtId(coninfo->dobj.name));

		if (coninfo->condef)
		{
			/* pg_get_constraintdef 应该提供了所有内容 */
			appendPQExpBuffer(q, "%s;\n", coninfo->condef);
		}
		else
		{
			appendPQExpBufferStr(q,
								 coninfo->contype == 'p' ? "PRIMARY KEY" : "UNIQUE");

			/*
			 * PRIMARY KEY 约束不应使用 NULLS NOT DISTINCT 索引。
			 * 能够创建这一点已得到修复，但我们需要使索引为
			 * DISTINCT 才能恢复导出。
			 *
			 */
			if (indxinfo->indnullsnotdistinct && coninfo->contype != 'p')
				appendPQExpBufferStr(q, " NULLS NOT DISTINCT");
			appendPQExpBufferStr(q, " (");
			for (k = 0; k < indxinfo->indnkeyattrs; k++)
			{
				int			indkey = (int) indxinfo->indkeys[k];
				const char *attname;

				if (indkey == InvalidAttrNumber)
					break;
				attname = getAttrName(indkey, tbinfo);

				appendPQExpBuffer(q, "%s%s",
								  (k == 0) ? "" : ", ",
								  fmtId(attname));
			}

			if (indxinfo->indnkeyattrs < indxinfo->indnattrs)
				appendPQExpBufferStr(q, ") INCLUDE (");

			for (k = indxinfo->indnkeyattrs; k < indxinfo->indnattrs; k++)
			{
				int			indkey = (int) indxinfo->indkeys[k];
				const char *attname;

				if (indkey == InvalidAttrNumber)
					break;
				attname = getAttrName(indkey, tbinfo);

				appendPQExpBuffer(q, "%s%s",
								  (k == indxinfo->indnkeyattrs) ? "" : ", ",
								  fmtId(attname));
			}

			appendPQExpBufferChar(q, ')');

			if (nonemptyReloptions(indxinfo->indreloptions))
			{
				appendPQExpBufferStr(q, " WITH (");
				appendReloptionsArrayAH(q, indxinfo->indreloptions, "", fout);
				appendPQExpBufferChar(q, ')');
			}

			if (coninfo->condeferrable)
			{
				appendPQExpBufferStr(q, " DEFERRABLE");
				if (coninfo->condeferred)
					appendPQExpBufferStr(q, " INITIALLY DEFERRED");
			}

			appendPQExpBufferStr(q, ";\n");
		}

		/*
		 * 根据需要附加 ALTER TABLE 命令以设置我们只有
		 * ALTER TABLE 语法的属性。使此代码与 dumpIndex
		 * 中的类似代码保持同步！
		 */

		/* 如果索引是聚集的，我们需要记录这一点。 */
		if (indxinfo->indisclustered)
		{
			appendPQExpBuffer(q, "\nALTER TABLE %s CLUSTER",
							  fmtQualifiedDumpable(tbinfo));
			/* 索引名在此语法中不限定 */
			appendPQExpBuffer(q, " ON %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/* 如果索引定义身份，我们需要记录这一点。 */
		if (indxinfo->indisreplident)
		{
			appendPQExpBuffer(q, "\nALTER TABLE ONLY %s REPLICA IDENTITY USING",
							  fmtQualifiedDumpable(tbinfo));
			/* 索引名在此语法中不限定 */
			appendPQExpBuffer(q, " INDEX %s;\n",
							  fmtId(indxinfo->dobj.name));
		}

		/* 索引可以依赖于扩展 */
		append_depends_on_extension(fout, q, &indxinfo->dobj,
									"pg_catalog.pg_class", "INDEX",
									fmtQualifiedDumpable(indxinfo));

		appendPQExpBuffer(delq, "ALTER %sTABLE ONLY %s ", foreign,
						  fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		tag = psprintf("%s %s", tbinfo->dobj.name, coninfo->dobj.name);

		if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = tag,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .tablespace = indxinfo->tablespace,
									  .owner = tbinfo->rolname,
									  .description = "CONSTRAINT",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data,
									  .dropStmt = delq->data));
	}
	else if (coninfo->contype == 'f')
	{
		char	   *only;

		/*
		 * 分区表上的外键始终声明为继承到分区；
		 * 对于所有其他情况，将它们发出为仅直接应用于
		 * 指定表，因为这就是它们对常规继承表的工作方式。
		 *
		 */
		only = tbinfo->relkind == RELKIND_PARTITIONED_TABLE ? "" : "ONLY ";

		/*
		 * XXX 可能包装在 'SET CONSTRAINTS OFF' 块中，
		 * 以便不处理当前表数据
		 */
		appendPQExpBuffer(q, "ALTER %sTABLE %s%s\n", foreign,
						  only, fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
						  fmtId(coninfo->dobj.name),
						  coninfo->condef);

		appendPQExpBuffer(delq, "ALTER %sTABLE %s%s ", foreign,
						  only, fmtQualifiedDumpable(tbinfo));
		appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
						  fmtId(coninfo->dobj.name));

		tag = psprintf("%s %s", tbinfo->dobj.name, coninfo->dobj.name);

		if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
			ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
						 ARCHIVE_OPTS(.tag = tag,
									  .namespace = tbinfo->dobj.namespace->dobj.name,
									  .owner = tbinfo->rolname,
									  .description = "FK CONSTRAINT",
									  .section = SECTION_POST_DATA,
									  .createStmt = q->data,
									  .dropStmt = delq->data));
	}
	else if (coninfo->contype == 'c' && tbinfo)
	{
		/* 表上的 CHECK 约束 */

		/* 如果不是单独导出的或如果是继承的则忽略 */
		if (coninfo->separate && coninfo->conislocal)
		{
			/* 不是 ONLY，因为我们希望它传播到子表 */
			appendPQExpBuffer(q, "ALTER %sTABLE %s\n", foreign,
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			appendPQExpBuffer(delq, "ALTER %sTABLE %s ", foreign,
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			tag = psprintf("%s %s", tbinfo->dobj.name, coninfo->dobj.name);

			if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
				ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
							 ARCHIVE_OPTS(.tag = tag,
										  .namespace = tbinfo->dobj.namespace->dobj.name,
										  .owner = tbinfo->rolname,
										  .description = "CHECK CONSTRAINT",
										  .section = SECTION_POST_DATA,
										  .createStmt = q->data,
										  .dropStmt = delq->data));
		}
	}
	else if (coninfo->contype == 'c' && tbinfo == NULL)
	{
		/* 域上的 CHECK 约束 */
		TypeInfo   *tyinfo = coninfo->condomain;

		/* 如果不是单独导出的则忽略 */
		if (coninfo->separate)
		{
			appendPQExpBuffer(q, "ALTER DOMAIN %s\n",
							  fmtQualifiedDumpable(tyinfo));
			appendPQExpBuffer(q, "    ADD CONSTRAINT %s %s;\n",
							  fmtId(coninfo->dobj.name),
							  coninfo->condef);

			appendPQExpBuffer(delq, "ALTER DOMAIN %s ",
							  fmtQualifiedDumpable(tyinfo));
			appendPQExpBuffer(delq, "DROP CONSTRAINT %s;\n",
							  fmtId(coninfo->dobj.name));

			tag = psprintf("%s %s", tyinfo->dobj.name, coninfo->dobj.name);

			if (coninfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
				ArchiveEntry(fout, coninfo->dobj.catId, coninfo->dobj.dumpId,
							 ARCHIVE_OPTS(.tag = tag,
										  .namespace = tyinfo->dobj.namespace->dobj.name,
										  .owner = tyinfo->rolname,
										  .description = "CHECK CONSTRAINT",
										  .section = SECTION_POST_DATA,
										  .createStmt = q->data,
										  .dropStmt = delq->data));

			if (coninfo->dobj.dump & DUMP_COMPONENT_COMMENT)
			{
				PQExpBuffer conprefix = createPQExpBuffer();
				char	   *qtypname = pg_strdup(fmtId(tyinfo->dobj.name));

				appendPQExpBuffer(conprefix, "CONSTRAINT %s ON DOMAIN",
								  fmtId(coninfo->dobj.name));

				dumpComment(fout, conprefix->data, qtypname,
							tyinfo->dobj.namespace->dobj.name,
							tyinfo->rolname,
							coninfo->dobj.catId, 0, coninfo->dobj.dumpId);
				destroyPQExpBuffer(conprefix);
				free(qtypname);
			}
		}
	}
	else
	{
		pg_fatal("unrecognized constraint type: %c",
				 coninfo->contype);
	}

	/* 导出约束注释 --- 仅适用于表约束 */
	if (tbinfo && coninfo->separate &&
		coninfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpTableConstraintComment(fout, coninfo);

	free(tag);
	destroyPQExpBuffer(q);
	destroyPQExpBuffer(delq);
}

/*
 * dumpTableConstraintComment --- 导出约束的注释（如果有）
 * 
 * 将其分离出来是因为我们在两个不同的地方需要该函数，
 * 具体取决于约束是作为 CREATE TABLE 的一部分导出
 * 还是作为单独的 ALTER 命令导出。
 */
static void
dumpTableConstraintComment(Archive *fout, const ConstraintInfo *coninfo)
{
	TableInfo  *tbinfo = coninfo->contable;
	PQExpBuffer conprefix = createPQExpBuffer();
	char	   *qtabname;

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(conprefix, "CONSTRAINT %s ON",
					  fmtId(coninfo->dobj.name));

	if (coninfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, conprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->rolname,
					coninfo->dobj.catId, 0,
					coninfo->separate ? coninfo->dobj.dumpId : tbinfo->dobj.dumpId);

	destroyPQExpBuffer(conprefix);
	free(qtabname);
}

/*
 * dumpSequence
 *   写出一个用户定义序列的声明（非数据）
 */
static void
dumpSequence(Archive *fout, const TableInfo *tbinfo)
{
	DumpOptions *dopt = fout->dopt;
	PGresult   *res;
	char	   *startv,
			   *incby,
			   *maxv,
			   *minv,
			   *cache,
			   *seqtype;
	bool		cycled;
	bool		is_ascending;
	int64		default_minv,
				default_maxv;
	char		bufm[32],
				bufx[32];
	PQExpBuffer query = createPQExpBuffer();
	PQExpBuffer delqry = createPQExpBuffer();
	char	   *qseqname;
	TableInfo  *owning_tab = NULL;

	qseqname = pg_strdup(fmtId(tbinfo->dobj.name));

	if (fout->remoteVersion >= 100000)
	{
		appendPQExpBuffer(query,
						  "SELECT format_type(seqtypid, NULL), "
						  "seqstart, seqincrement, "
						  "seqmax, seqmin, "
						  "seqcache, seqcycle "
						  "FROM pg_catalog.pg_sequence "
						  "WHERE seqrelid = '%u'::oid",
						  tbinfo->dobj.catId.oid);
	}
	else
	{
		/*
		 * 在 PostgreSQL 10 之前，序列元数据位于序列自身中。
		 * 
		 * 注意：可能看起来 'bigint' 可能需要模式限定，
		 * 但实际上它是一个关键字。
		 */
		appendPQExpBuffer(query,
						  "SELECT 'bigint' AS sequence_type, "
						  "start_value, increment_by, max_value, min_value, "
						  "cache_value, is_cycled FROM %s",
						  fmtQualifiedDumpable(tbinfo));
	}

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
		pg_fatal(ngettext("query to get data of sequence \"%s\" returned %d row (expected 1)",
						  "query to get data of sequence \"%s\" returned %d rows (expected 1)",
						  PQntuples(res)),
				 tbinfo->dobj.name, PQntuples(res));

	seqtype = PQgetvalue(res, 0, 0);
	startv = PQgetvalue(res, 0, 1);
	incby = PQgetvalue(res, 0, 2);
	maxv = PQgetvalue(res, 0, 3);
	minv = PQgetvalue(res, 0, 4);
	cache = PQgetvalue(res, 0, 5);
	cycled = (strcmp(PQgetvalue(res, 0, 6), "t") == 0);

	/* 计算此类型序列的默认限制 */
	is_ascending = (incby[0] != '-');
	if (strcmp(seqtype, "smallint") == 0)
	{
		default_minv = is_ascending ? 1 : PG_INT16_MIN;
		default_maxv = is_ascending ? PG_INT16_MAX : -1;
	}
	else if (strcmp(seqtype, "integer") == 0)
	{
		default_minv = is_ascending ? 1 : PG_INT32_MIN;
		default_maxv = is_ascending ? PG_INT32_MAX : -1;
	}
	else if (strcmp(seqtype, "bigint") == 0)
	{
		default_minv = is_ascending ? 1 : PG_INT64_MIN;
		default_maxv = is_ascending ? PG_INT64_MAX : -1;
	}
	else
	{
		pg_fatal("unrecognized sequence type: %s", seqtype);
		default_minv = default_maxv = 0;	/* 使编译器保持安静 */
	}

	/*
	 * 64 位 strtol() 的可移植性不太好，因此将限制转换为
	 * 字符串并以此方式进行比较。
	 */
	snprintf(bufm, sizeof(bufm), INT64_FORMAT, default_minv);
	snprintf(bufx, sizeof(bufx), INT64_FORMAT, default_maxv);

	/* 如果 minv/maxv 匹配相应的默认限制则不输出 */
	if (strcmp(minv, bufm) == 0)
		minv = NULL;
	if (strcmp(maxv, bufx) == 0)
		maxv = NULL;

	/*
	 * 标识序列不应单独删除。
	 */
	if (!tbinfo->is_identity_sequence)
	{
		appendPQExpBuffer(delqry, "DROP SEQUENCE %s;\n",
						  fmtQualifiedDumpable(tbinfo));
	}

	resetPQExpBuffer(query);

	if (dopt->binary_upgrade)
	{
		binary_upgrade_set_pg_class_oids(fout, query,
										 tbinfo->dobj.catId.oid, false);

		/*
		 * 在较旧的 PG 版本中，序列将有一个 pg_type 条目，
		 * 但 v14 及以上版本不使用它，因此不要尝试保留类型 OID。
		 */
	}

	if (tbinfo->is_identity_sequence)
	{
		owning_tab = findTableByOid(tbinfo->owning_tab);

		appendPQExpBuffer(query,
						  "ALTER TABLE %s ",
						  fmtQualifiedDumpable(owning_tab));
		appendPQExpBuffer(query,
						  "ALTER COLUMN %s ADD GENERATED ",
						  fmtId(owning_tab->attnames[tbinfo->owning_col - 1]));
		if (owning_tab->attidentity[tbinfo->owning_col - 1] == ATTRIBUTE_IDENTITY_ALWAYS)
			appendPQExpBufferStr(query, "ALWAYS");
		else if (owning_tab->attidentity[tbinfo->owning_col - 1] == ATTRIBUTE_IDENTITY_BY_DEFAULT)
			appendPQExpBufferStr(query, "BY DEFAULT");
		appendPQExpBuffer(query, " AS IDENTITY (\n    SEQUENCE NAME %s\n",
						  fmtQualifiedDumpable(tbinfo));

		/*
		 * 仅在持久性选项与拥有表不同时才发出。
		 * 这避免了不必要地使用此新语法。
		 */
		if (tbinfo->relpersistence != owning_tab->relpersistence)
			appendPQExpBuffer(query, "    %s\n",
							  tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED ?
							  "UNLOGGED" : "LOGGED");
	}
	else
	{
		appendPQExpBuffer(query,
						  "CREATE %sSEQUENCE %s\n",
						  tbinfo->relpersistence == RELPERSISTENCE_UNLOGGED ?
						  "UNLOGGED " : "",
						  fmtQualifiedDumpable(tbinfo));

		if (strcmp(seqtype, "bigint") != 0)
			appendPQExpBuffer(query, "    AS %s\n", seqtype);
	}

	appendPQExpBuffer(query, "    START WITH %s\n", startv);

	appendPQExpBuffer(query, "    INCREMENT BY %s\n", incby);

	if (minv)
		appendPQExpBuffer(query, "    MINVALUE %s\n", minv);
	else
		appendPQExpBufferStr(query, "    NO MINVALUE\n");

	if (maxv)
		appendPQExpBuffer(query, "    MAXVALUE %s\n", maxv);
	else
		appendPQExpBufferStr(query, "    NO MAXVALUE\n");

	appendPQExpBuffer(query,
					  "    CACHE %s%s",
					  cache, (cycled ? "\n    CYCLE" : ""));

	if (tbinfo->is_identity_sequence)
		appendPQExpBufferStr(query, "\n);\n");
	else
		appendPQExpBufferStr(query, ";\n");

	/* 二进制升级：不需要清除 TOAST 表 OID */

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(query, &tbinfo->dobj,
										"SEQUENCE", qseqname,
										tbinfo->dobj.namespace->dobj.name);

	if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tbinfo->dobj.catId, tbinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "SEQUENCE",
								  .section = SECTION_PRE_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	/*
	 * 如果序列由表列拥有，则为其发出 ALTER 作为单独的
	 * TOC 条目，紧跟在序列自己的条目之后。这样做而不是
	 * 使用完全排序逻辑是可以的，因为告诉我们它被拥有的
	 * 依赖关系将强制先创建表。我们不能只是将 ALTER 包含
	 * 在 TOC 条目中，因为如果我们尚未重新分配序列所有者
	 * 以匹配表的所有者，它会失败。
	 * 
	 * 我们不需要对表引用进行模式限定，因为序列和表
	 * 必须在同一模式中。
	 *
	 */
	if (OidIsValid(tbinfo->owning_tab) && !tbinfo->is_identity_sequence)
	{
		owning_tab = findTableByOid(tbinfo->owning_tab);

		if (owning_tab == NULL)
			pg_fatal("failed sanity check, parent table with OID %u of sequence with OID %u not found",
					 tbinfo->owning_tab, tbinfo->dobj.catId.oid);

		if (owning_tab->dobj.dump & DUMP_COMPONENT_DEFINITION)
		{
			resetPQExpBuffer(query);
			appendPQExpBuffer(query, "ALTER SEQUENCE %s",
							  fmtQualifiedDumpable(tbinfo));
			appendPQExpBuffer(query, " OWNED BY %s",
							  fmtQualifiedDumpable(owning_tab));
			appendPQExpBuffer(query, ".%s;\n",
							  fmtId(owning_tab->attnames[tbinfo->owning_col - 1]));

			if (tbinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
				ArchiveEntry(fout, nilCatalogId, createDumpId(),
							 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
										  .namespace = tbinfo->dobj.namespace->dobj.name,
										  .owner = tbinfo->rolname,
										  .description = "SEQUENCE OWNED BY",
										  .section = SECTION_PRE_DATA,
										  .createStmt = query->data,
										  .deps = &(tbinfo->dobj.dumpId),
										  .nDeps = 1));
		}
	}

	/* 导出序列注释和安全标签 */
	if (tbinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "SEQUENCE", qseqname,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tbinfo->dobj.catId, 0, tbinfo->dobj.dumpId);

	if (tbinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "SEQUENCE", qseqname,
					 tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					 tbinfo->dobj.catId, 0, tbinfo->dobj.dumpId);

	PQclear(res);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	free(qseqname);
}

/*
 * dumpSequenceData
 *   写出一个用户定义序列的数据
 */
static void
dumpSequenceData(Archive *fout, const TableDataInfo *tdinfo)
{
	TableInfo  *tbinfo = tdinfo->tdtable;
	PGresult   *res;
	char	   *last;
	bool		called;
	PQExpBuffer query = createPQExpBuffer();

	appendPQExpBuffer(query,
					  "SELECT last_value, is_called FROM %s",
					  fmtQualifiedDumpable(tbinfo));

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	if (PQntuples(res) != 1)
		pg_fatal(ngettext("query to get data of sequence \"%s\" returned %d row (expected 1)",
						  "query to get data of sequence \"%s\" returned %d rows (expected 1)",
						  PQntuples(res)),
				 tbinfo->dobj.name, PQntuples(res));

	last = PQgetvalue(res, 0, 0);
	called = (strcmp(PQgetvalue(res, 0, 1), "t") == 0);

	resetPQExpBuffer(query);
	appendPQExpBufferStr(query, "SELECT pg_catalog.setval(");
	appendStringLiteralAH(query, fmtQualifiedDumpable(tbinfo), fout);
	appendPQExpBuffer(query, ", %s, %s);\n",
					  last, (called ? "true" : "false"));

	if (tdinfo->dobj.dump & DUMP_COMPONENT_DATA)
		ArchiveEntry(fout, nilCatalogId, createDumpId(),
					 ARCHIVE_OPTS(.tag = tbinfo->dobj.name,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "SEQUENCE SET",
								  .section = SECTION_DATA,
								  .createStmt = query->data,
								  .deps = &(tbinfo->dobj.dumpId),
								  .nDeps = 1));

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * dumpTrigger
 *   写出一个用户定义表触发器的声明
 */
static void
dumpTrigger(Archive *fout, const TriggerInfo *tginfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = tginfo->tgtable;
	PQExpBuffer query;
	PQExpBuffer delqry;
	PQExpBuffer trigprefix;
	PQExpBuffer trigidentity;
	char	   *qtabname;
	char	   *tgargs;
	size_t		lentgargs;
	const char *p;
	int			findx;
	char	   *tag;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();
	trigprefix = createPQExpBuffer();
	trigidentity = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	appendPQExpBuffer(trigidentity, "%s ", fmtId(tginfo->dobj.name));
	appendPQExpBuffer(trigidentity, "ON %s", fmtQualifiedDumpable(tbinfo));

	appendPQExpBuffer(delqry, "DROP TRIGGER %s;\n", trigidentity->data);

	if (tginfo->tgdef)
	{
		appendPQExpBuffer(query, "%s;\n", tginfo->tgdef);
	}
	else
	{
		if (tginfo->tgisconstraint)
		{
			appendPQExpBufferStr(query, "CREATE CONSTRAINT TRIGGER ");
			appendPQExpBufferStr(query, fmtId(tginfo->tgconstrname));
		}
		else
		{
			appendPQExpBufferStr(query, "CREATE TRIGGER ");
			appendPQExpBufferStr(query, fmtId(tginfo->dobj.name));
		}
		appendPQExpBufferStr(query, "\n    ");

		/* 触发器类型 */
		if (TRIGGER_FOR_BEFORE(tginfo->tgtype))
			appendPQExpBufferStr(query, "BEFORE");
		else if (TRIGGER_FOR_AFTER(tginfo->tgtype))
			appendPQExpBufferStr(query, "AFTER");
		else if (TRIGGER_FOR_INSTEAD(tginfo->tgtype))
			appendPQExpBufferStr(query, "INSTEAD OF");
		else
			pg_fatal("unexpected tgtype value: %d", tginfo->tgtype);

		findx = 0;
		if (TRIGGER_FOR_INSERT(tginfo->tgtype))
		{
			appendPQExpBufferStr(query, " INSERT");
			findx++;
		}
		if (TRIGGER_FOR_DELETE(tginfo->tgtype))
		{
			if (findx > 0)
				appendPQExpBufferStr(query, " OR DELETE");
			else
				appendPQExpBufferStr(query, " DELETE");
			findx++;
		}
		if (TRIGGER_FOR_UPDATE(tginfo->tgtype))
		{
			if (findx > 0)
				appendPQExpBufferStr(query, " OR UPDATE");
			else
				appendPQExpBufferStr(query, " UPDATE");
			findx++;
		}
		if (TRIGGER_FOR_TRUNCATE(tginfo->tgtype))
		{
			if (findx > 0)
				appendPQExpBufferStr(query, " OR TRUNCATE");
			else
				appendPQExpBufferStr(query, " TRUNCATE");
			findx++;
		}
		appendPQExpBuffer(query, " ON %s\n",
						  fmtQualifiedDumpable(tbinfo));

		if (tginfo->tgisconstraint)
		{
			if (OidIsValid(tginfo->tgconstrrelid))
			{
				/* regclass 输出已加引号 */
				appendPQExpBuffer(query, "    FROM %s\n    ",
								  tginfo->tgconstrrelname);
			}
			if (!tginfo->tgdeferrable)
				appendPQExpBufferStr(query, "NOT ");
			appendPQExpBufferStr(query, "DEFERRABLE INITIALLY ");
			if (tginfo->tginitdeferred)
				appendPQExpBufferStr(query, "DEFERRED\n");
			else
				appendPQExpBufferStr(query, "IMMEDIATE\n");
		}

		if (TRIGGER_FOR_ROW(tginfo->tgtype))
			appendPQExpBufferStr(query, "    FOR EACH ROW\n    ");
		else
			appendPQExpBufferStr(query, "    FOR EACH STATEMENT\n    ");

		/* regproc 输出已充分加引号 */
		appendPQExpBuffer(query, "EXECUTE FUNCTION %s(",
						  tginfo->tgfname);

		tgargs = (char *) PQunescapeBytea((unsigned char *) tginfo->tgargs,
										  &lentgargs);
		p = tgargs;
		for (findx = 0; findx < tginfo->tgnargs; findx++)
		{
			/* 查找终止此触发器参数的嵌入 null */
			size_t		tlen = strlen(p);

			if (p + tlen >= tgargs + lentgargs)
			{
				/* 嗯，在 bytea 值结束前未找到... */
				pg_fatal("invalid argument string (%s) for trigger \"%s\" on table \"%s\"",
						 tginfo->tgargs,
						 tginfo->dobj.name,
						 tbinfo->dobj.name);
			}

			if (findx > 0)
				appendPQExpBufferStr(query, ", ");
			appendStringLiteralAH(query, p, fout);
			p += tlen + 1;
		}
		free(tgargs);
		appendPQExpBufferStr(query, ");\n");
	}

	/* 触发器可以依赖于扩展 */
	append_depends_on_extension(fout, query, &tginfo->dobj,
								"pg_catalog.pg_trigger", "TRIGGER",
								trigidentity->data);

	if (tginfo->tgispartition)
	{
		Assert(tbinfo->ispartition);

		/*
		 * 分区触发器仅在此处出现，因为它们的 'tgenabled' 标志
		 * 与其父表不同。触发器已创建，因此移除 CREATE 并用
		 * ALTER 替换。（同时清除 DROP 查询，以便 pg_dump --create
		 * 不会导致错误。）
		 */
		resetPQExpBuffer(query);
		resetPQExpBuffer(delqry);
		appendPQExpBuffer(query, "\nALTER %sTABLE %s ",
						  tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "",
						  fmtQualifiedDumpable(tbinfo));
		switch (tginfo->tgenabled)
		{
			case 'f':
			case 'D':
				appendPQExpBufferStr(query, "DISABLE");
				break;
			case 't':
			case 'O':
				appendPQExpBufferStr(query, "ENABLE");
				break;
			case 'R':
				appendPQExpBufferStr(query, "ENABLE REPLICA");
				break;
			case 'A':
				appendPQExpBufferStr(query, "ENABLE ALWAYS");
				break;
		}
		appendPQExpBuffer(query, " TRIGGER %s;\n",
						  fmtId(tginfo->dobj.name));
	}
	else if (tginfo->tgenabled != 't' && tginfo->tgenabled != 'O')
	{
		appendPQExpBuffer(query, "\nALTER %sTABLE %s ",
						  tbinfo->relkind == RELKIND_FOREIGN_TABLE ? "FOREIGN " : "",
						  fmtQualifiedDumpable(tbinfo));
		switch (tginfo->tgenabled)
		{
			case 'D':
			case 'f':
				appendPQExpBufferStr(query, "DISABLE");
				break;
			case 'A':
				appendPQExpBufferStr(query, "ENABLE ALWAYS");
				break;
			case 'R':
				appendPQExpBufferStr(query, "ENABLE REPLICA");
				break;
			default:
				appendPQExpBufferStr(query, "ENABLE");
				break;
		}
		appendPQExpBuffer(query, " TRIGGER %s;\n",
						  fmtId(tginfo->dobj.name));
	}

	appendPQExpBuffer(trigprefix, "TRIGGER %s ON",
					  fmtId(tginfo->dobj.name));

	tag = psprintf("%s %s", tbinfo->dobj.name, tginfo->dobj.name);

	if (tginfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, tginfo->dobj.catId, tginfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "TRIGGER",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	if (tginfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, trigprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name, tbinfo->rolname,
					tginfo->dobj.catId, 0, tginfo->dobj.dumpId);

	free(tag);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	destroyPQExpBuffer(trigprefix);
	destroyPQExpBuffer(trigidentity);
	free(qtabname);
}

/*
 * dumpEventTrigger
 *   写出一个用户定义事件触发器的声明
 */
static void
dumpEventTrigger(Archive *fout, const EventTriggerInfo *evtinfo)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PQExpBuffer delqry;
	char	   *qevtname;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	query = createPQExpBuffer();
	delqry = createPQExpBuffer();

	qevtname = pg_strdup(fmtId(evtinfo->dobj.name));

	appendPQExpBufferStr(query, "CREATE EVENT TRIGGER ");
	appendPQExpBufferStr(query, qevtname);
	appendPQExpBufferStr(query, " ON ");
	appendPQExpBufferStr(query, fmtId(evtinfo->evtevent));

	if (strcmp("", evtinfo->evttags) != 0)
	{
		appendPQExpBufferStr(query, "\n         WHEN TAG IN (");
		appendPQExpBufferStr(query, evtinfo->evttags);
		appendPQExpBufferChar(query, ')');
	}

	appendPQExpBufferStr(query, "\n   EXECUTE FUNCTION ");
	appendPQExpBufferStr(query, evtinfo->evtfname);
	appendPQExpBufferStr(query, "();\n");

	if (evtinfo->evtenabled != 'O')
	{
		appendPQExpBuffer(query, "\nALTER EVENT TRIGGER %s ",
						  qevtname);
		switch (evtinfo->evtenabled)
		{
			case 'D':
				appendPQExpBufferStr(query, "DISABLE");
				break;
			case 'A':
				appendPQExpBufferStr(query, "ENABLE ALWAYS");
				break;
			case 'R':
				appendPQExpBufferStr(query, "ENABLE REPLICA");
				break;
			default:
				appendPQExpBufferStr(query, "ENABLE");
				break;
		}
		appendPQExpBufferStr(query, ";\n");
	}

	appendPQExpBuffer(delqry, "DROP EVENT TRIGGER %s;\n",
					  qevtname);

	if (dopt->binary_upgrade)
		binary_upgrade_extension_member(query, &evtinfo->dobj,
										"EVENT TRIGGER", qevtname, NULL);

	if (evtinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, evtinfo->dobj.catId, evtinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = evtinfo->dobj.name,
								  .owner = evtinfo->evtowner,
								  .description = "EVENT TRIGGER",
								  .section = SECTION_POST_DATA,
								  .createStmt = query->data,
								  .dropStmt = delqry->data));

	if (evtinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, "EVENT TRIGGER", qevtname,
					NULL, evtinfo->evtowner,
					evtinfo->dobj.catId, 0, evtinfo->dobj.dumpId);

	if (evtinfo->dobj.dump & DUMP_COMPONENT_SECLABEL)
		dumpSecLabel(fout, "EVENT TRIGGER", qevtname,
					 NULL, evtinfo->evtowner,
					 evtinfo->dobj.catId, 0, evtinfo->dobj.dumpId);

	destroyPQExpBuffer(query);
	destroyPQExpBuffer(delqry);
	free(qevtname);
}

/*
 * dumpRule
 *   导出一个规则
 */
static void
dumpRule(Archive *fout, const RuleInfo *rinfo)
{
	DumpOptions *dopt = fout->dopt;
	TableInfo  *tbinfo = rinfo->ruletable;
	bool		is_view;
	PQExpBuffer query;
	PQExpBuffer cmd;
	PQExpBuffer delcmd;
	PQExpBuffer ruleprefix;
	char	   *qtabname;
	PGresult   *res;
	char	   *tag;

	/* 在仅数据导出中不执行任何操作 */
	if (dopt->dataOnly)
		return;

	/*
	 * 如果是由 CREATE VIEW 隐式创建的 ON SELECT 规则，
	 * 我们不希望将其作为单独的对象导出。
	 */
	if (!rinfo->separate)
		return;

	/*
	 * 如果是 ON SELECT 规则，我们希望将其输出为视图定义，
	 * 而不是规则。
	 */
	is_view = (rinfo->ev_type == '1' && rinfo->is_instead);

	query = createPQExpBuffer();
	cmd = createPQExpBuffer();
	delcmd = createPQExpBuffer();
	ruleprefix = createPQExpBuffer();

	qtabname = pg_strdup(fmtId(tbinfo->dobj.name));

	if (is_view)
	{
		PQExpBuffer result;

		/*
		 * 我们在此需要 OR REPLACE，因为我们将替换一个虚拟视图。
		 * 除此之外，这应该与常规视图导出代码基本相似。
		 */
		appendPQExpBuffer(cmd, "CREATE OR REPLACE VIEW %s",
						  fmtQualifiedDumpable(tbinfo));
		if (nonemptyReloptions(tbinfo->reloptions))
		{
			appendPQExpBufferStr(cmd, " WITH (");
			appendReloptionsArrayAH(cmd, tbinfo->reloptions, "", fout);
			appendPQExpBufferChar(cmd, ')');
		}
		result = createViewAsClause(fout, tbinfo);
		appendPQExpBuffer(cmd, " AS\n%s", result->data);
		destroyPQExpBuffer(result);
		if (tbinfo->checkoption != NULL)
			appendPQExpBuffer(cmd, "\n  WITH %s CHECK OPTION",
							  tbinfo->checkoption);
		appendPQExpBufferStr(cmd, ";\n");
	}
	else
	{
		/* 在规则情况下，只需按原样输出 pg_get_ruledef 的结果 */
		appendPQExpBuffer(query,
						  "SELECT pg_catalog.pg_get_ruledef('%u'::pg_catalog.oid)",
						  rinfo->dobj.catId.oid);

		res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

		if (PQntuples(res) != 1)
			pg_fatal("query to get rule \"%s\" for table \"%s\" failed: wrong number of rows returned",
					 rinfo->dobj.name, tbinfo->dobj.name);

		printfPQExpBuffer(cmd, "%s\n", PQgetvalue(res, 0, 0));

		PQclear(res);
	}

	/*
	 * 添加更改规则复制触发语义的命令，如果它与默认值不同。
	 *
	 */
	if (rinfo->ev_enabled != 'O')
	{
		appendPQExpBuffer(cmd, "ALTER TABLE %s ", fmtQualifiedDumpable(tbinfo));
		switch (rinfo->ev_enabled)
		{
			case 'A':
				appendPQExpBuffer(cmd, "ENABLE ALWAYS RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
			case 'R':
				appendPQExpBuffer(cmd, "ENABLE REPLICA RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
			case 'D':
				appendPQExpBuffer(cmd, "DISABLE RULE %s;\n",
								  fmtId(rinfo->dobj.name));
				break;
		}
	}

	if (is_view)
	{
		/*
		 * 我们不能 DROP 视图的 ON SELECT 规则。相反，使用
		 * CREATE OR REPLACE VIEW 将规则替换为具有最小依赖的内容。
		 *
		 */
		PQExpBuffer result;

		appendPQExpBuffer(delcmd, "CREATE OR REPLACE VIEW %s",
						  fmtQualifiedDumpable(tbinfo));
		result = createDummyViewAsClause(fout, tbinfo);
		appendPQExpBuffer(delcmd, " AS\n%s;\n", result->data);
		destroyPQExpBuffer(result);
	}
	else
	{
		appendPQExpBuffer(delcmd, "DROP RULE %s ",
						  fmtId(rinfo->dobj.name));
		appendPQExpBuffer(delcmd, "ON %s;\n",
						  fmtQualifiedDumpable(tbinfo));
	}

	appendPQExpBuffer(ruleprefix, "RULE %s ON",
					  fmtId(rinfo->dobj.name));

	tag = psprintf("%s %s", tbinfo->dobj.name, rinfo->dobj.name);

	if (rinfo->dobj.dump & DUMP_COMPONENT_DEFINITION)
		ArchiveEntry(fout, rinfo->dobj.catId, rinfo->dobj.dumpId,
					 ARCHIVE_OPTS(.tag = tag,
								  .namespace = tbinfo->dobj.namespace->dobj.name,
								  .owner = tbinfo->rolname,
								  .description = "RULE",
								  .section = SECTION_POST_DATA,
								  .createStmt = cmd->data,
								  .dropStmt = delcmd->data));

	/* 导出规则注释 */
	if (rinfo->dobj.dump & DUMP_COMPONENT_COMMENT)
		dumpComment(fout, ruleprefix->data, qtabname,
					tbinfo->dobj.namespace->dobj.name,
					tbinfo->rolname,
					rinfo->dobj.catId, 0, rinfo->dobj.dumpId);

	free(tag);
	destroyPQExpBuffer(query);
	destroyPQExpBuffer(cmd);
	destroyPQExpBuffer(delcmd);
	destroyPQExpBuffer(ruleprefix);
	free(qtabname);
}

/*
 * getExtensionMembership --- 获取扩展成员资格数据
 * 
 * 我们需要在加载对象时尽快识别扩展成员对象，以便正确确定
 * 它们是否需要导出。一般来说，扩展成员对象将被标记为*不*
 * 导出，因为它们将由单个 CREATE EXTENSION 命令重新创建。
 * 但是，在二进制升级模式下，我们仍然需要单独导出成员。
 *
 *
 */
void
getExtensionMembership(Archive *fout, ExtensionInfo extinfo[],
					   int numExtensions)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_classid,
				i_objid,
				i_refobjid;
	ExtensionInfo *ext;

	/* 如果没有扩展则无需处理 */
	if (numExtensions == 0)
		return;

	query = createPQExpBuffer();

	/* refclassid 约束是冗余的但可能加快搜索 */
	appendPQExpBufferStr(query, "SELECT "
						 "classid, objid, refobjid "
						 "FROM pg_depend "
						 "WHERE refclassid = 'pg_extension'::regclass "
						 "AND deptype = 'e' "
						 "ORDER BY 3");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refobjid = PQfnumber(res, "refobjid");

	/*
	 * 由于我们按引用 ID 排序 SELECT，我们可以预期同一扩展的
	 * 多个条目将一起出现；这节省了搜索。
	 *
	 */
	ext = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		Oid			extId;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		extId = atooid(PQgetvalue(res, i, i_refobjid));

		if (ext == NULL ||
			ext->dobj.catId.oid != extId)
			ext = findExtensionByOid(extId);

		if (ext == NULL)
		{
			/* 不应该发生 */
			pg_log_warning("could not find referenced extension %u", extId);
			continue;
		}

		recordExtensionMembership(objId, ext);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}

/*
 * processExtensionTables --- 处理扩展配置表
 * 
 * 此过程有两部分：
 * 
 * 1. 识别并创建扩展配置表的导出记录。
 * 
 *   扩展可以将表标记为"配置"，这意味着用户能够且期望
 *   在扩展加载后修改这些表。对于这些表，我们仅导出
 *   数据 —— 结构预期在 CREATE EXTENSION 时处理，包括
 *   任何索引或外键，这就引出了 ——
 * 
 * 2. 记录配置表之间的 FK 依赖。
 * 
 *   由于 FK 是在 CREATE EXTENSION 时创建的，因此在数据
 *   加载之前创建，我们必须确定重新加载数据的最佳顺序，
 *   以避免在恢复表时出现 FK 违规。这并不完美 —— 我们
 *   无法处理循环依赖，如果存在循环依赖，将导致生成
 *   无效的导出（尽管至少所有数据都包含在内供用户手动
 *   恢复）。这目前已有文档记录，但也许我们将来可以提供
 *   更好的解决方案。
 *
 */
void
processExtensionTables(Archive *fout, ExtensionInfo extinfo[],
					   int numExtensions)
{
	DumpOptions *dopt = fout->dopt;
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_conrelid,
				i_confrelid;

	/* 如果没有扩展则无需处理 */
	if (numExtensions == 0)
		return;

	/*
	 * 识别扩展配置表并为其创建 TableDataInfo 对象，
	 * 确保即使表本身不会被导出，它们的数据也会被导出。
	 * 
	 * 注意，即使在 schemaOnly 模式下，我们也会创建
	 * TableDataInfo 对象，即配置表中的用户数据被视为模式数据。
	 * 这似乎是合适的，因为配置表中的系统数据将由
	 * CREATE EXTENSION 重新加载。如果扩展不在要包含的扩展
	 * 列表中，则不会导出其任何数据。
	 *
	 */
	for (i = 0; i < numExtensions; i++)
	{
		ExtensionInfo *curext = &(extinfo[i]);
		char	   *extconfig = curext->extconfig;
		char	   *extcondition = curext->extcondition;
		char	  **extconfigarray = NULL;
		char	  **extconditionarray = NULL;
		int			nconfigitems = 0;
		int			nconditionitems = 0;

		/*
		 * 检查此扩展是否在导出中列为要包含的。如果不是，
		 * 则丢弃与之关联的任何表数据。
		 */
		if (extension_include_oids.head != NULL &&
			!simple_oid_list_member(&extension_include_oids,
									curext->dobj.catId.oid))
			continue;

		if (strlen(extconfig) != 0 || strlen(extcondition) != 0)
		{
			int			j;

			if (!parsePGArray(extconfig, &extconfigarray, &nconfigitems))
				pg_fatal("could not parse %s array", "extconfig");
			if (!parsePGArray(extcondition, &extconditionarray, &nconditionitems))
				pg_fatal("could not parse %s array", "extcondition");
			if (nconfigitems != nconditionitems)
				pg_fatal("mismatched number of configurations and conditions for extension");

			for (j = 0; j < nconfigitems; j++)
			{
				TableInfo  *configtbl;
				Oid			configtbloid = atooid(extconfigarray[j]);
				bool		dumpobj =
					curext->dobj.dump & DUMP_COMPONENT_DEFINITION;

				configtbl = findTableByOid(configtbloid);
				if (configtbl == NULL)
					continue;

				/*
				 * 不应导出的扩展的表不应导出，
				 * 除非表或其模式被显式包含
				 */
				if (!(curext->dobj.dump & DUMP_COMPONENT_DEFINITION))
				{
					/* 检查表是否被显式请求 */
					if (table_include_oids.head != NULL &&
						simple_oid_list_member(&table_include_oids,
											   configtbloid))
						dumpobj = true;

					/* 检查表的模式是否被显式请求 */
					if (configtbl->dobj.namespace->dobj.dump &
						DUMP_COMPONENT_DATA)
						dumpobj = true;
				}

				/* 检查表是否被排除开关排除 */
				if (table_exclude_oids.head != NULL &&
					simple_oid_list_member(&table_exclude_oids,
										   configtbloid))
					dumpobj = false;

				/* 检查模式是否被排除开关排除 */
				if (simple_oid_list_member(&schema_exclude_oids,
										   configtbl->dobj.namespace->dobj.catId.oid))
					dumpobj = false;

				if (dumpobj)
				{
					makeTableDataInfo(dopt, configtbl);
					if (configtbl->dataObj != NULL)
					{
						if (strlen(extconditionarray[j]) > 0)
							configtbl->dataObj->filtercond = pg_strdup(extconditionarray[j]);
					}
				}
			}
		}
		if (extconfigarray)
			free(extconfigarray);
		if (extconditionarray)
			free(extconditionarray);
	}

	/*
	 * 现在所有扩展的所有 TableDataInfo 对象都已创建，
	 * 检查它们的 FK 依赖并注册它们，以尝试按可恢复的
	 * 顺序导出数据。
	 * 
	 * 注意，这对于用户表不是问题，因为它们的 FK 在
	 * 数据加载后重新创建。
	 */

	query = createPQExpBuffer();

	printfPQExpBuffer(query,
					  "SELECT conrelid, confrelid "
					  "FROM pg_constraint "
					  "JOIN pg_depend ON (objid = confrelid) "
					  "WHERE contype = 'f' "
					  "AND refclassid = 'pg_extension'::regclass "
					  "AND classid = 'pg_class'::regclass;");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);
	ntups = PQntuples(res);

	i_conrelid = PQfnumber(res, "conrelid");
	i_confrelid = PQfnumber(res, "confrelid");

	/* 现在获取依赖并注册它们 */
	for (i = 0; i < ntups; i++)
	{
		Oid			conrelid,
					confrelid;
		TableInfo  *reftable,
				   *contable;

		conrelid = atooid(PQgetvalue(res, i, i_conrelid));
		confrelid = atooid(PQgetvalue(res, i, i_confrelid));
		contable = findTableByOid(conrelid);
		reftable = findTableByOid(confrelid);

		if (reftable == NULL ||
			reftable->dataObj == NULL ||
			contable == NULL ||
			contable->dataObj == NULL)
			continue;

		/*
		 * 使引用 TABLE_DATA 对象依赖于被引用表的 TABLE_DATA 对象。
		 *
		 */
		addObjectDependency(&contable->dataObj->dobj,
							reftable->dataObj->dobj.dumpId);
	}
	PQclear(res);
	destroyPQExpBuffer(query);
}

/*
 * getDependencies --- 获取可用的依赖数据
 */
static void
getDependencies(Archive *fout)
{
	PQExpBuffer query;
	PGresult   *res;
	int			ntups,
				i;
	int			i_classid,
				i_objid,
				i_refclassid,
				i_refobjid,
				i_deptype;
	DumpableObject *dobj,
			   *refdobj;

	pg_log_info("reading dependency data");

	query = createPQExpBuffer();

	/*
	 * 收集我们需要的依赖数据的凌乱查询。注意，我们忽略
	 * 子对象列，因此列上的依赖或依赖于列的依赖看起来
	 * 与整个表上的依赖相同。
	 * 
	 * PIN 依赖不感兴趣，EXTENSION 依赖已由
	 * getExtensionMembership 处理。
	 */
	appendPQExpBufferStr(query, "SELECT "
						 "classid, objid, refclassid, refobjid, deptype "
						 "FROM pg_depend "
						 "WHERE deptype != 'p' AND deptype != 'e'\n");

	/*
	 * 由于我们不将 pg_amop 条目视为单独的 DumpableObject，
	 * 我们必须将它们的依赖转换为其父操作符族的依赖。
	 * 不过忽略内部依赖，因为它们会指向其父操作符类，
	 * 我们在此不需要考虑（如果我们考虑，只会导致循环依赖）。
	 * 此外，"松散"操作符族条目将依赖于其父操作符族，
	 * 我们应该删除这些依赖，因为它们同样会变成无用的
	 * 自依赖。（但一定要保留对*其他*操作符族的依赖；
	 * 参见 amopsortfamily。）
	 */
	appendPQExpBufferStr(query, "UNION ALL\n"
						 "SELECT 'pg_opfamily'::regclass AS classid, amopfamily AS objid, refclassid, refobjid, deptype "
						 "FROM pg_depend d, pg_amop o "
						 "WHERE deptype NOT IN ('p', 'e', 'i') AND "
						 "classid = 'pg_amop'::regclass AND objid = o.oid "
						 "AND NOT (refclassid = 'pg_opfamily'::regclass AND amopfamily = refobjid)\n");

	/* 同样适用于 pg_amproc 条目 */
	appendPQExpBufferStr(query, "UNION ALL\n"
						 "SELECT 'pg_opfamily'::regclass AS classid, amprocfamily AS objid, refclassid, refobjid, deptype "
						 "FROM pg_depend d, pg_amproc p "
						 "WHERE deptype NOT IN ('p', 'e', 'i') AND "
						 "classid = 'pg_amproc'::regclass AND objid = p.oid "
						 "AND NOT (refclassid = 'pg_opfamily'::regclass AND amprocfamily = refobjid)\n");

	/* 为后续效率对输出进行排序 */
	appendPQExpBufferStr(query, "ORDER BY 1,2");

	res = ExecuteSqlQuery(fout, query->data, PGRES_TUPLES_OK);

	ntups = PQntuples(res);

	i_classid = PQfnumber(res, "classid");
	i_objid = PQfnumber(res, "objid");
	i_refclassid = PQfnumber(res, "refclassid");
	i_refobjid = PQfnumber(res, "refobjid");
	i_deptype = PQfnumber(res, "deptype");

	/*
	 * 由于我们按引用 ID 排序 SELECT，我们可以预期同一对象的
	 * 多个条目将一起出现；这节省了搜索。
	 *
	 */
	dobj = NULL;

	for (i = 0; i < ntups; i++)
	{
		CatalogId	objId;
		CatalogId	refobjId;
		char		deptype;

		objId.tableoid = atooid(PQgetvalue(res, i, i_classid));
		objId.oid = atooid(PQgetvalue(res, i, i_objid));
		refobjId.tableoid = atooid(PQgetvalue(res, i, i_refclassid));
		refobjId.oid = atooid(PQgetvalue(res, i, i_refobjid));
		deptype = *(PQgetvalue(res, i, i_deptype));

		if (dobj == NULL ||
			dobj->catId.tableoid != objId.tableoid ||
			dobj->catId.oid != objId.oid)
			dobj = findObjectByCatalogId(objId);

		/*
		 * 找不到 pg_depend 中提到的对象并不意外，
		 * 因为例如我们不收集关于 TOAST 表的信息。
		 */
		if (dobj == NULL)
		{
#ifdef NOT_USED
			pg_log_warning("no referencing object %u %u",
						   objId.tableoid, objId.oid);
#endif
			continue;
		}

		refdobj = findObjectByCatalogId(refobjId);

		if (refdobj == NULL)
		{
#ifdef NOT_USED
			pg_log_warning("no referenced object %u %u",
						   refobjId.tableoid, refobjId.oid);
#endif
			continue;
		}

		/*
		 * 对于 'x' 依赖，标记对象以备后用；我们仍然添加
		 * 正常的依赖，用于可能的排序目的。目前
		 * pg_dump_sort.c 知道将扩展放在可能依赖它们的
		 * 所有对象类型之前，但这更安全。
		 */
		if (deptype == 'x')
			dobj->depends_on_ext = true;

		/*
		 * 通常，表行类型具有对其表的隐式依赖。然而，对于复合类型，
		 * 隐式依赖在 pg_depend 中是相反方向的；这对 DROP 是正确的，
		 * 但它不会产生我们需要的依赖排序。因此，在这种情况下，
		 * 我们反转依赖的方向。
		 *
		 */
		if (deptype == 'i' &&
			dobj->objType == DO_TABLE &&
			refdobj->objType == DO_TYPE)
			addObjectDependency(refdobj, dobj->dumpId);
		else
			/* 正常情况 */
			addObjectDependency(dobj, refdobj->dumpId);
	}

	PQclear(res);

	destroyPQExpBuffer(query);
}


/*
 * createBoundaryObjects - 创建虚拟 DumpableObject
 * 以表示导出节边界。
 */
static DumpableObject *
createBoundaryObjects(void)
{
	DumpableObject *dobjs;

	dobjs = (DumpableObject *) pg_malloc(2 * sizeof(DumpableObject));

	dobjs[0].objType = DO_PRE_DATA_BOUNDARY;
	dobjs[0].catId = nilCatalogId;
	AssignDumpId(dobjs + 0);
	dobjs[0].name = pg_strdup("PRE-DATA BOUNDARY");

	dobjs[1].objType = DO_POST_DATA_BOUNDARY;
	dobjs[1].catId = nilCatalogId;
	AssignDumpId(dobjs + 1);
	dobjs[1].name = pg_strdup("POST-DATA BOUNDARY");

	return dobjs;
}

/*
 * addBoundaryDependencies - 根据需要添加依赖
 * 以强制执行导出节边界。
 */
static void
addBoundaryDependencies(DumpableObject **dobjs, int numObjs,
						DumpableObject *boundaryObjs)
{
	DumpableObject *preDataBound = boundaryObjs + 0;
	DumpableObject *postDataBound = boundaryObjs + 1;
	int			i;

	for (i = 0; i < numObjs; i++)
	{
		DumpableObject *dobj = dobjs[i];

		/*
		 * 此处的对象类型分类必须与后续 ArchiveEntry 调用
		 * 中分配的 SECTION_xxx 值匹配！
		 */
		switch (dobj->objType)
		{
			case DO_NAMESPACE:
			case DO_EXTENSION:
			case DO_TYPE:
			case DO_SHELL_TYPE:
			case DO_FUNC:
			case DO_AGG:
			case DO_OPERATOR:
			case DO_ACCESS_METHOD:
			case DO_OPCLASS:
			case DO_OPFAMILY:
			case DO_COLLATION:
			case DO_CONVERSION:
			case DO_TABLE:
			case DO_TABLE_ATTACH:
			case DO_ATTRDEF:
			case DO_PROCLANG:
			case DO_CAST:
			case DO_DUMMY_TYPE:
			case DO_TSPARSER:
			case DO_TSDICT:
			case DO_TSTEMPLATE:
			case DO_TSCONFIG:
			case DO_FDW:
			case DO_FOREIGN_SERVER:
			case DO_TRANSFORM:
			case DO_LARGE_OBJECT:
				/* Pre-data 对象：必须位于 pre-data 边界之前 */
				addObjectDependency(preDataBound, dobj->dumpId);
				break;
			case DO_TABLE_DATA:
			case DO_SEQUENCE_SET:
			case DO_LARGE_OBJECT_DATA:
				/* Data 对象：必须位于边界之间 */
				addObjectDependency(dobj, preDataBound->dumpId);
				addObjectDependency(postDataBound, dobj->dumpId);
				break;
			case DO_INDEX:
			case DO_INDEX_ATTACH:
			case DO_STATSEXT:
			case DO_REFRESH_MATVIEW:
			case DO_TRIGGER:
			case DO_EVENT_TRIGGER:
			case DO_DEFAULT_ACL:
			case DO_POLICY:
			case DO_PUBLICATION:
			case DO_PUBLICATION_REL:
			case DO_PUBLICATION_TABLE_IN_SCHEMA:
			case DO_SUBSCRIPTION:
				/* Post-data 对象：必须位于 post-data 边界之后 */
				addObjectDependency(dobj, postDataBound->dumpId);
				break;
			case DO_RULE:
				/* 规则是 post-data，但仅在单独导出时 */
				if (((RuleInfo *) dobj)->separate)
					addObjectDependency(dobj, postDataBound->dumpId);
				break;
			case DO_CONSTRAINT:
			case DO_FK_CONSTRAINT:
				/* 约束是 post-data，但仅在单独导出时 */
				if (((ConstraintInfo *) dobj)->separate)
					addObjectDependency(dobj, postDataBound->dumpId);
				break;
			case DO_PRE_DATA_BOUNDARY:
				/* 无事可做 */
				break;
			case DO_POST_DATA_BOUNDARY:
				/* 必须位于 pre-data 边界之后 */
				addObjectDependency(dobj, preDataBound->dumpId);
				break;
		}
	}
}


/*
 * BuildArchiveDependencies - 为归档 TOC 条目创建依赖数据
 * 
 * getDependencies() 获取的原始依赖数据在归档导出中不太有用，
 * 因为在许多情况下，依赖链通过不在导出中显式出现的对象。
 * 例如，视图将依赖于其 _RETURN 规则，而 _RETURN 规则
 * 将依赖于其他对象 —— 但规则不会在导出中作为单独的对象
 * 出现。我们需要调整视图的依赖以包含规则依赖的任何
 * 包含在导出中的内容。
 * 
 * 更复杂的是，还存在"特殊"依赖，例如 TABLE DATA 项
 * 对其 TABLE 的依赖，我们不能重新排列这些依赖，因为
 * pg_restore 知道 TABLE DATA 只依赖于其表。在这些情况下，
 * 我们必须严格保持依赖原样，即使它们引用不导出的对象。
 * 
 * 为处理这一点，约定是"特殊"依赖在 ArchiveEntry 调用
 * 期间创建，并且具有任何此类条目的归档 TOC 项在此处不会
 * 被修改。否则，我们递归搜索 DumpableObject 数据结构
 * 以为每个归档 TOC 项构建正确的依赖。
 *
 *
 *
 */
static void
BuildArchiveDependencies(Archive *fout)
{
	ArchiveHandle *AH = (ArchiveHandle *) fout;
	TocEntry   *te;

	/* 扫描归档中的所有 TOC 条目 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		DumpableObject *dobj;
		DumpId	   *dependencies;
		int			nDeps;
		int			allocDeps;

		/* 无需处理不会被导出的条目 */
		if (te->reqs == 0)
			continue;
		/* 忽略已经具有"特殊"依赖的条目 */
		if (te->nDeps > 0)
			continue;
		/* 否则，查找条目的原始 DumpableObject（如果有） */
		dobj = findObjectByDumpId(te->dumpId);
		if (dobj == NULL)
			continue;
		/* 如果没有依赖则无需工作 */
		if (dobj->nDeps <= 0)
			continue;
		/* 设置工作数组 */
		allocDeps = 64;
		dependencies = (DumpId *) pg_malloc(allocDeps * sizeof(DumpId));
		nDeps = 0;
		/* 递归查找所有可导出的依赖 */
		findDumpableDependencies(AH, dobj,
								 &dependencies, &nDeps, &allocDeps);
		/* 并保存它们 ... */
		if (nDeps > 0)
		{
			dependencies = (DumpId *) pg_realloc(dependencies,
												 nDeps * sizeof(DumpId));
			te->dependencies = dependencies;
			te->nDeps = nDeps;
		}
		else
			free(dependencies);
	}
}

/* BuildArchiveDependencies 的递归搜索子程序 */
static void
findDumpableDependencies(ArchiveHandle *AH, const DumpableObject *dobj,
						 DumpId **dependencies, int *nDeps, int *allocDeps)
{
	int			i;

	/*
	 * 忽略节边界对象：如果我们搜索它们，我们将报告
	 * 许多虚假的依赖。
	 */
	if (dobj->objType == DO_PRE_DATA_BOUNDARY ||
		dobj->objType == DO_POST_DATA_BOUNDARY)
		return;

	for (i = 0; i < dobj->nDeps; i++)
	{
		DumpId		depid = dobj->dependencies[i];

		if (TocIDRequired(AH, depid) != 0)
		{
			/* 对象将被导出，因此仅将其引用为依赖 */
			if (*nDeps >= *allocDeps)
			{
				*allocDeps *= 2;
				*dependencies = (DumpId *) pg_realloc(*dependencies,
													  *allocDeps * sizeof(DumpId));
			}
			(*dependencies)[*nDeps] = depid;
			(*nDeps)++;
		}
		else
		{
			/*
			 * 对象不会被导出，因此递归考虑其依赖。我们依赖
			 * sortDumpableObjects 已打破任何依赖循环的假设，
			 * 否则我们可能会无限递归。
			 */
			DumpableObject *otherdobj = findObjectByDumpId(depid);

			if (otherdobj)
				findDumpableDependencies(AH, otherdobj,
										 dependencies, nDeps, allocDeps);
		}
	}
}


/*
 * getFormattedTypeName - 为给定类型 OID 检索
 * 格式良好的类型名称。
 * 
 * 这不保证对输出进行模式限定，因此不应用于创建
 * CREATE 或 ALTER 命令的目标对象名称。
 * 
 * 注意，结果被缓存，调用者不得释放。
 */
static const char *
getFormattedTypeName(Archive *fout, Oid oid, OidOptions opts)
{
	TypeInfo   *typeInfo;
	char	   *result;
	PQExpBuffer query;
	PGresult   *res;

	if (oid == 0)
	{
		if ((opts & zeroAsStar) != 0)
			return "*";
		else if ((opts & zeroAsNone) != 0)
			return "NONE";
	}

	/* 查看我们是否已将结果缓存在类型的 TypeInfo 记录中 */
	typeInfo = findTypeByOid(oid);
	if (typeInfo && typeInfo->ftypname)
		return typeInfo->ftypname;

	query = createPQExpBuffer();
	appendPQExpBuffer(query, "SELECT pg_catalog.format_type('%u'::pg_catalog.oid, NULL)",
					  oid);

	res = ExecuteSqlQueryForSingleRow(fout, query->data);

	/* format_type 的结果已加引号 */
	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	destroyPQExpBuffer(query);

	/*
	 * 如果可能，缓存结果以供以后请求重用。如果我们没有
	 * 类型的 TypeInfo，字符串将在调用者使用完后泄漏 ...
	 * 但这种情况确实不应该发生，因此如果发生，泄漏似乎
	 * 可以接受。
	 */
	if (typeInfo)
		typeInfo->ftypname = result;

	return result;
}

/*
 * 返回给定关系的列列表子句。
 * 
 * 特殊情况：如果关系中没有未删除的列，返回 ""，
 * 而不是无效的 "()" 列列表。
 */
static const char *
fmtCopyColumnList(const TableInfo *ti, PQExpBuffer buffer)
{
	int			numatts = ti->numatts;
	char	  **attnames = ti->attnames;
	bool	   *attisdropped = ti->attisdropped;
	char	   *attgenerated = ti->attgenerated;
	bool		needComma;
	int			i;

	appendPQExpBufferChar(buffer, '(');
	needComma = false;
	for (i = 0; i < numatts; i++)
	{
		if (attisdropped[i])
			continue;
		if (attgenerated[i])
			continue;
		if (needComma)
			appendPQExpBufferStr(buffer, ", ");
		appendPQExpBufferStr(buffer, fmtId(attnames[i]));
		needComma = true;
	}

	if (!needComma)
		return "";				/* 没有未删除的列 */

	appendPQExpBufferChar(buffer, ')');
	return buffer->data;
}

/*
 * 检查 reloptions 数组是否非空。
 */
static bool
nonemptyReloptions(const char *reloptions)
{
	/* 如果只是 "{}" 则不想输出 */
	return (reloptions != NULL && strlen(reloptions) > 2);
}

/*
 * 格式化 reloptions 数组并将其附加到给定缓冲区。
 * 
 * "prefix" 被添加到选项名称前面；通常是 "" 或 "toast."。
 */
static void
appendReloptionsArrayAH(PQExpBuffer buffer, const char *reloptions,
						const char *prefix, Archive *fout)
{
	bool		res;

	res = appendReloptionsArray(buffer, reloptions, prefix, fout->encoding,
								fout->std_strings);
	if (!res)
		pg_log_warning("could not parse %s array", "reloptions");
}
