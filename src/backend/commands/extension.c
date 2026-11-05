/*-------------------------------------------------------------------------
 *
 * extension.c
 *	  扩展操作命令。
 *
 * PostgreSQL 扩展允许管理 SQL 对象集合。
 *
 * 内部管理扩展只需一个 OID，依赖对象可与之关联。
 * 扩展通过将"控制"文件的内容填入 pg_extension 系统目录来创建。
 * 扩展控制文件使用与 postgresql.conf 相同的解析器解析。
 * 扩展还有一个安装脚本文件，包含创建扩展对象的 SQL 命令。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/extension.c
 *
 *-------------------------------------------------------------------------
*
*
 */
#include "postgres.h"

#include <dirent.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "commands/schemacmds.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/conffiles.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/varlena.h"


/* 全局可见的状态变量 */
bool		creating_extension = false;
Oid			CurrentExtensionObject = InvalidOid;

/*
 * 存储控制文件解析结果的内部数据结构
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* 扩展名称 */
	char	   *directory;		/* 脚本文件所在目录 */
	char	   *default_version;	/* 默认安装目标版本 */
	char	   *module_pathname;	/* 用于替换 MODULE_PATHNAME 的
									 * 字符串 */
	char	   *comment;		/* 注释（如有） */
	char	   *schema;			/* 目标模式（非可重定位时允许） */
	bool		relocatable;	/* 是否支持 ALTER EXTENSION SET SCHEMA？ */
	bool		superuser;		/* 安装是否需要超级用户？ */
	bool		trusted;		/* 是否允许临时切换为超级用户？ */
	int			encoding;		/* 脚本文件编码，-1 表示未指定 */
	List	   *requires;		/* 前置扩展名称 */
	List	   *no_relocate;	/* 不应被重定位的
								 * 前置扩展名称 */
} ExtensionControlFile;

/*
 * 更新路径信息的内部数据结构
 */
typedef struct ExtensionVersionInfo
{
	char	   *name;			/* 起始版本名称 */
	List	   *reachable;		/* ExtensionVersionInfo 链表 */
	bool		installable;	/* 此版本是否有安装脚本？ */
	/* Dijkstra 算法工作状态： */
	bool		distance_known; /* 距起点的距离是否已知？ */
	int			distance;		/* 当前最坏情况距离估计 */
	struct ExtensionVersionInfo *previous;	/* 当前最佳前驱 */
} ExtensionVersionInfo;

/* 本地函数 */
static List *find_update_path(List *evi_list,
							  ExtensionVersionInfo *evi_start,
							  ExtensionVersionInfo *evi_target,
							  bool reject_indirect,
							  bool reinitialize);
static Oid	get_required_extension(char *reqExtensionName,
								   char *extensionName,
								   char *origSchemaName,
								   bool cascade,
								   List *parents,
								   bool is_create);
static void get_available_versions_for_extension(ExtensionControlFile *pcontrol,
												 Tuplestorestate *tupstore,
												 TupleDesc tupdesc);
static Datum convert_requires_to_datum(List *requires);
static void ApplyExtensionUpdates(Oid extensionOid,
								  ExtensionControlFile *pcontrol,
								  const char *initialVersion,
								  List *updateVersions,
								  char *origSchemaName,
								  bool cascade,
								  bool is_create);
static char *read_whole_file(const char *filename, int *length);


/*
 * get_extension_oid - 给定扩展名称，查找其 OID
 *
 * missing_ok 为 false 时，未找到则报错。为 true 则仅返回 InvalidOid。
*
 */
Oid
get_extension_oid(const char *extname, bool missing_ok)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	scandesc = systable_beginscan(rel, ExtensionNameIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* 假定最多有一条匹配元组 */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->oid;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	table_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						extname)));

	return result;
}

/*
 * get_extension_name - 给定扩展 OID，查找其名称
 *
 * 返回 palloc 分配的字符串，扩展不存在则返回 NULL。
 */
char *
get_extension_name(Oid ext_oid)
{
	char	   *result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* 假定最多有一条匹配元组 */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_extension) GETSTRUCT(tuple))->extname));
	else
		result = NULL;

	systable_endscan(scandesc);

	table_close(rel, AccessShareLock);

	return result;
}

/*
 * get_extension_schema - 给定扩展 OID，获取其 extnamespace
 *
 * 扩展不存在则返回 InvalidOid。
 */
Oid
get_extension_schema(Oid ext_oid)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* 假定最多有一条匹配元组 */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	table_close(rel, AccessShareLock);

	return result;
}

/*
 * 检查扩展名和版本名有效性的工具函数
 */
static void
check_valid_extension_name(const char *extensionname)
{
	int			namelen = strlen(extensionname);

	/*
	 * 不允许空名称（解析器已拒绝空标识符，但仍需检查）。
	*
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not be empty.")));

	/*
	 * 不允许双破折号，否则脚本文件名会有歧义。
	 */
	if (strstr(extensionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain \"--\".")));

	/*
	 * 也不允许开头或结尾的破折号。（可能可以允许，但需
	 * 在文件名解析中格外小心，且会使文件名看起来有歧义。
	 * 由于无实际用例，直接禁止。）
	*
	 */
	if (extensionname[0] == '-' || extensionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not begin or end with \"-\".")));

	/*
	 * 也不允许目录分隔符（足以防止 ".." 式攻击）。
	*
	 */
	if (first_dir_separator(extensionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension name: \"%s\"", extensionname),
				 errdetail("Extension names must not contain directory separator characters.")));
}

static void
check_valid_version_name(const char *versionname)
{
	int			namelen = strlen(versionname);

	/*
	 * 不允许空名称（或许可以允许，但似乎没有意义）。
	*
	 */
	if (namelen == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not be empty.")));

	/*
	 * 不允许双破折号，否则脚本文件名会有歧义。
	 */
	if (strstr(versionname, "--"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain \"--\".")));

	/*
	 * 也不允许开头或结尾的破折号。
	 */
	if (versionname[0] == '-' || versionname[namelen - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not begin or end with \"-\".")));

	/*
	 * 也不允许目录分隔符（足以防止 ".." 式攻击）。
	*
	 */
	if (first_dir_separator(versionname) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid extension version name: \"%s\"", versionname),
				 errdetail("Version names must not contain directory separator characters.")));
}

/*
 * 处理扩展相关路径名的工具函数
 */
static bool
is_extension_control_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".control") == 0);
}

static bool
is_extension_script_filename(const char *filename)
{
	const char *extension = strrchr(filename, '.');

	return (extension != NULL) && (strcmp(extension, ".sql") == 0);
}

static char *
get_extension_control_directory(void)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/extension", sharepath);

	return result;
}

static char *
get_extension_control_filename(const char *extname)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/extension/%s.control",
			 sharepath, extname);

	return result;
}

static char *
get_extension_script_directory(ExtensionControlFile *control)
{
	char		sharepath[MAXPGPATH];
	char	   *result;

	/*
	 * directory 参数可省略、为绝对路径或相对于安装 share 目录。
	*
	 */
	if (!control->directory)
		return get_extension_control_directory();

	if (is_absolute_path(control->directory))
		return pstrdup(control->directory);

	get_share_path(my_exec_path, sharepath);
	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/%s", sharepath, control->directory);

	return result;
}

static char *
get_extension_aux_control_filename(ExtensionControlFile *control,
								   const char *version)
{
	char	   *result;
	char	   *scriptdir;

	scriptdir = get_extension_script_directory(control);

	result = (char *) palloc(MAXPGPATH);
	snprintf(result, MAXPGPATH, "%s/%s--%s.control",
			 scriptdir, control->name, version);

	pfree(scriptdir);

	return result;
}

static char *
get_extension_script_filename(ExtensionControlFile *control,
							  const char *from_version, const char *version)
{
	char	   *result;
	char	   *scriptdir;

	scriptdir = get_extension_script_directory(control);

	result = (char *) palloc(MAXPGPATH);
	if (from_version)
		snprintf(result, MAXPGPATH, "%s/%s--%s--%s.sql",
				 scriptdir, control->name, from_version, version);
	else
		snprintf(result, MAXPGPATH, "%s/%s--%s.sql",
				 scriptdir, control->name, version);

	pfree(scriptdir);

	return result;
}


/*
 * 解析主控制文件或辅助控制文件的内容，并填入 
 *  *control 的字段。version == NULL 时解析主文件，   
 * 否则解析该版本的辅助文件。           
 *
 * 控制文件应当非常短，只有几行，因此我们无需 
 * 担心此处的内存分配风险。同时我们也不必担心     
 * 它的编码；所有值都预期为 ASCII。                          
 */
static void
parse_extension_control_file(ExtensionControlFile *control,
							 const char *version)
{
	char	   *filename;
	FILE	   *file;
	ConfigVariable *item,
			   *head = NULL,
			   *tail = NULL;

	/*
	 * 定位待读取文件。辅助文件是可选的。
	 */
	if (version)
		filename = get_extension_aux_control_filename(control, version);
	else
		filename = get_extension_control_filename(control->name);

	if ((file = AllocateFile(filename, "r")) == NULL)
	{
		if (errno == ENOENT)
		{
			/* 缺少辅助文件不报错 */
			if (version)
			{
				pfree(filename);
				return;
			}

			/* 缺少控制文件表示扩展未安装 */
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" is not available", control->name),
					 errdetail("Could not open extension control file \"%s\": %m.",
							   filename),
					 errhint("The extension must first be installed on the system where PostgreSQL is running.")));
		}
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open extension control file \"%s\": %m",
						filename)));
	}

	/*
	 * 使用 GUC 文件解析代码解析文件内容。无需检查返回值，
	 * 因为错误会以 ERROR 级别抛出。
	 */
	(void) ParseConfigFp(file, filename, CONF_FILE_START_DEPTH, ERROR,
						 &head, &tail);

	FreeFile(file);

	/*
	 * 将 ConfigVariable 列表转换为 ExtensionControlFile 条目。
	 */
	for (item = head; item != NULL; item = item->next)
	{
		if (strcmp(item->name, "directory") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->directory = pstrdup(item->value);
		}
		else if (strcmp(item->name, "default_version") == 0)
		{
			if (version)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("parameter \"%s\" cannot be set in a secondary extension control file",
								item->name)));

			control->default_version = pstrdup(item->value);
		}
		else if (strcmp(item->name, "module_pathname") == 0)
		{
			control->module_pathname = pstrdup(item->value);
		}
		else if (strcmp(item->name, "comment") == 0)
		{
			control->comment = pstrdup(item->value);
		}
		else if (strcmp(item->name, "schema") == 0)
		{
			control->schema = pstrdup(item->value);
		}
		else if (strcmp(item->name, "relocatable") == 0)
		{
			if (!parse_bool(item->value, &control->relocatable))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "superuser") == 0)
		{
			if (!parse_bool(item->value, &control->superuser))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "trusted") == 0)
		{
			if (!parse_bool(item->value, &control->trusted))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" requires a Boolean value",
								item->name)));
		}
		else if (strcmp(item->name, "encoding") == 0)
		{
			control->encoding = pg_valid_server_encoding(item->value);
			if (control->encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("\"%s\" is not a valid encoding name",
								item->value)));
		}
		else if (strcmp(item->name, "requires") == 0)
		{
			/* 需要字符串的可修改副本 */
			char	   *rawnames = pstrdup(item->value);

			/* 将字符串解析为标识符列表 */
			if (!SplitIdentifierString(rawnames, ',', &control->requires))
			{
				/* 名称列表语法错误 */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else if (strcmp(item->name, "no_relocate") == 0)
		{
			/* 需要字符串的可修改副本 */
			char	   *rawnames = pstrdup(item->value);

			/* 将字符串解析为标识符列表 */
			if (!SplitIdentifierString(rawnames, ',', &control->no_relocate))
			{
				/* 名称列表语法错误 */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"%s\" must be a list of extension names",
								item->name)));
			}
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized parameter \"%s\" in file \"%s\"",
							item->name, filename)));
	}

	FreeConfigVariables(head);

	if (control->relocatable && control->schema != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("parameter \"schema\" cannot be specified when \"relocatable\" is true")));

	pfree(filename);
}

/*
 * 读取指定扩展的主控制文件。
 */
static ExtensionControlFile *
read_extension_control_file(const char *extname)
{
	ExtensionControlFile *control;

	/*
	 * 设置默认值。指针字段初始为 null。
	 */
	control = (ExtensionControlFile *) palloc0(sizeof(ExtensionControlFile));
	control->name = pstrdup(extname);
	control->relocatable = false;
	control->superuser = true;
	control->trusted = false;
	control->encoding = -1;

	/*
	 * 解析主控制文件。
	 */
	parse_extension_control_file(control, NULL);

	return control;
}

/*
 * 读取指定扩展和版本的辅助控制文件。
 *
 * 返回新的修改后的 ExtensionControlFile 结构；
 * 原始结构（仅反映主控制文件）不被修改。
 */
static ExtensionControlFile *
read_extension_aux_control_file(const ExtensionControlFile *pcontrol,
								const char *version)
{
	ExtensionControlFile *acontrol;

	/*
	 * 浅拷贝结构。指针字段与原结构共享值。
	 */
	acontrol = (ExtensionControlFile *) palloc(sizeof(ExtensionControlFile));
	memcpy(acontrol, pcontrol, sizeof(ExtensionControlFile));

	/*
	 * 解析辅助控制文件，覆盖结构字段
	 */
	parse_extension_control_file(acontrol, version);

	return acontrol;
}

/*
 * 将 SQL 脚本文件读入字符串并转换为数据库编码
 */
static char *
read_extension_script_file(const ExtensionControlFile *control,
						   const char *filename)
{
	int			src_encoding;
	char	   *src_str;
	char	   *dest_str;
	int			len;

	src_str = read_whole_file(filename, &len);

	/* 未给定时使用数据库编码 */
	if (control->encoding < 0)
		src_encoding = GetDatabaseEncoding();
	else
		src_encoding = control->encoding;

	/* 确保源字符串在预期编码中有效 */
	(void) pg_verify_mbstr(src_encoding, src_str, len, false);

	/*
	 * 将编码转换为数据库编码。read_whole_file 已将    
	 * 字符串以 null 结尾，因此若无转换发生，则原样      
	 * 可用。   
	 */
	dest_str = pg_any_to_server(src_str, len, src_encoding);

	return dest_str;
}

/*
 * 执行给定的 SQL 字符串。
 *
 * 注意：直接使用 SPI 执行字符串看似方便，但并不理想。
 * 关键问题是 SPI 会在执行任何语句前解析、分析并计划
 * 整个字符串；若脚本中存在引用更早创建对象的可计划语句
 * 则会失败。另一点是 SPI 会在出错时将整个字符串打印
 * 为 errcontext，可能非常长。
*
*
 */
static void
execute_sql_string(const char *sql)
{
	List	   *raw_parsetree_list;
	DestReceiver *dest;
	ListCell   *lc1;

	/*
	 * 将 SQL 字符串解析为原始解析树列表。
	 */
	raw_parsetree_list = pg_parse_query(sql);

	/* SELECT 的所有输出被丢弃 */
	dest = CreateDestReceiver(DestNone);

	/*
	 * 对每个原始解析树执行解析分析、规则重写、计划和执行。
	 * 必须先完整执行每条查询再开始下一条的解析分析，
	 * 因为可能存在相互依赖。
	 */
	foreach(lc1, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
		MemoryContext per_parsetree_context,
					oldcontext;
		List	   *stmt_list;
		ListCell   *lc2;

		/*
		 * 在短期上下文中处理每个解析树，
		 * 限制字符串包含多命令时的内存使用。
		 */
		per_parsetree_context =
			AllocSetContextCreate(CurrentMemoryContext,
								  "execute_sql_string per-statement context",
								  ALLOCSET_DEFAULT_SIZES);
		oldcontext = MemoryContextSwitchTo(per_parsetree_context);

		/* 确保解析器能看到已完成的所有 DDL */
		CommandCounterIncrement();

		stmt_list = pg_analyze_and_rewrite_fixedparams(parsetree,
													   sql,
													   NULL,
													   0,
													   NULL);
		stmt_list = pg_plan_queries(stmt_list, sql, CURSOR_OPT_PARALLEL_OK, NULL);

		foreach(lc2, stmt_list)
		{
			PlannedStmt *stmt = lfirst_node(PlannedStmt, lc2);

			CommandCounterIncrement();

			PushActiveSnapshot(GetTransactionSnapshot());

			if (stmt->utilityStmt == NULL)
			{
				QueryDesc  *qdesc;

				qdesc = CreateQueryDesc(stmt,
										sql,
										GetActiveSnapshot(), NULL,
										dest, NULL, NULL, 0);

				ExecutorStart(qdesc, 0);
				ExecutorRun(qdesc, ForwardScanDirection, 0, true);
				ExecutorFinish(qdesc);
				ExecutorEnd(qdesc);

				FreeQueryDesc(qdesc);
			}
			else
			{
				if (IsA(stmt->utilityStmt, TransactionStmt))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("transaction control statements are not allowed within an extension script")));

				ProcessUtility(stmt,
							   sql,
							   false,
							   PROCESS_UTILITY_QUERY,
							   NULL,
							   NULL,
							   dest,
							   NULL);
			}

			PopActiveSnapshot();
		}

		/* 清理每个解析树的上下文。 */
		MemoryContextSwitchTo(oldcontext);
		MemoryContextDelete(per_parsetree_context);
	}

	/* 确保最后一条脚本命令后推进命令计数器 */
	CommandCounterIncrement();
}

/*
 * 策略函数：给定的扩展是否受信任，可由非超级用户
 * 安装？     
 *
 * （若改动此处，请同步更新 errhint。）  
 */
static bool
extension_is_trusted(ExtensionControlFile *control)
{
	AclResult	aclresult;

	/* 除非扩展控制文件声明可信，否则一律不信 */
	if (!control->trusted)
		return false;
	/* 如果用户对当前数据库有 CREATE 权限则允许 */
	aclresult = object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(), ACL_CREATE);
	if (aclresult == ACLCHECK_OK)
		return true;
	return false;
}

/*
 * 执行用于安装或更新扩展的脚本文件
 *
 * 如果 from_version 非 NULL 则为更新操作
 *
 * 注意：requiredSchemas 必须与 control->requires 列表一一对应
 */
static void
execute_extension_script(Oid extensionOid, ExtensionControlFile *control,
						 const char *from_version,
						 const char *version,
						 List *requiredSchemas,
						 const char *schemaName, Oid schemaOid)
{
	bool		switch_to_superuser = false;
	char	   *filename;
	Oid			save_userid = 0;
	int			save_sec_context = 0;
	int			save_nestlevel;
	StringInfoData pathbuf;
	ListCell   *lc;
	ListCell   *lc2;

	/*
	 * 必要时强制执行超级用户要求。延迟检查至此，确保
	 * 控制标志与正确的脚本关联（可能在辅助控制文件中设置）。
	*
	 */
	if (control->superuser && !superuser())
	{
		if (extension_is_trusted(control))
			switch_to_superuser = true;
		else if (from_version == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to create this extension.")
					 : errhint("Must be superuser to create this extension.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to update extension \"%s\"",
							control->name),
					 control->trusted
					 ? errhint("Must have CREATE privilege on current database to update this extension.")
					 : errhint("Must be superuser to update this extension.")));
	}

	filename = get_extension_script_filename(control, from_version, version);

	if (from_version == NULL)
		elog(DEBUG1, "executing extension script for \"%s\" version '%s'", control->name, version);
	else
		elog(DEBUG1, "executing extension script for \"%s\" update from version '%s' to '%s'", control->name, from_version, version);

	/*
	 * 如为非超级用户安装受信扩展，临时成为 bootstrap 超级用户。
	 * （若事务中止，此切换及下方 GUC 更改将自动清理。）
	*
	 */
	if (switch_to_superuser)
	{
		GetUserIdAndSecContext(&save_userid, &save_sec_context);
		SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
							   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	}

	/*
	 * 强制 client_min_messages 和 log_min_messages 至少为 WARNING，
	 * 避免常见脚本操作（如创建 shell 类型）产生无用的 NOTICE
	 * 消息干扰用户。
	 *
	 * 使用等效于函数 SET 选项的方式，使设置在脚本执行期间
	 * 持续有效。guc.c 负责在出错时撤销设置。
	 *
	 * log_min_messages 普通用户无法设置，因此假装为超级用户。
	*
	*
	 */
	save_nestlevel = NewGUCNestLevel();

	if (client_min_messages < WARNING)
		(void) set_config_option("client_min_messages", "warning",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (log_min_messages < WARNING)
		(void) set_config_option_ext("log_min_messages", "warning",
									 PGC_SUSET, PGC_S_SESSION,
									 BOOTSTRAP_SUPERUSERID,
									 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * 同样禁用 check_function_bodies，确保创建期间
	 * SQL 函数不会被解析。
	 */
	if (check_function_bodies)
		(void) set_config_option("check_function_bodies", "off",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * 设置搜索路径，将目标模式放在首位使其成为默认
	 * 创建目标命名空间。然后添加前置扩展的模式（已在
	 * pg_catalog 中者除外，因其始终可搜索）。
	 * （在非首位显式列出 pg_catalog 有安全隐患。）
	 * 最后添加 pg_temp 确保临时对象不会优先于其他对象。
	 *
	 * 注意：使用 PushOverrideSearchPath 看似方便，但不可行。
	 * 必须实际设置 search_path GUC，因为扩展脚本可能检查
	 * 或修改它。GUC_ACTION_SAVE 方法同样方便。
	*
	*
	 */
	initStringInfo(&pathbuf);
	appendStringInfoString(&pathbuf, quote_identifier(schemaName));
	foreach(lc, requiredSchemas)
	{
		Oid			reqschema = lfirst_oid(lc);
		char	   *reqname = get_namespace_name(reqschema);

		if (reqname && strcmp(reqname, "pg_catalog") != 0)
			appendStringInfo(&pathbuf, ", %s", quote_identifier(reqname));
	}
	appendStringInfoString(&pathbuf, ", pg_temp");

	(void) set_config_option("search_path", pathbuf.data,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * 设置 creating_extension 及相关变量，
	 * 使 recordDependencyOnCurrentExtension 等函数正确执行。
	 * 失败时务必重置这些变量。
	 */
	creating_extension = true;
	CurrentExtensionObject = extensionOid;
	PG_TRY();
	{
		char	   *c_sql = read_extension_script_file(control, filename);
		Datum		t_sql;

		/*
		 * 通过 quote_identifier() 过滤每个替换。若参数包含
		 * 以下字符之一，则无法同时在 $$dollor-quoted$$、
		 * 'single-quoted' 字面量内外安全引号。
		 * 为保护扩展作者，含这些字符的参数替换时报错。
		*
		*
		 */
		const char *quoting_relevant_chars = "\"$'\\";

		/* 使用各种操作 text datum 的函数 */
		t_sql = CStringGetTextDatum(c_sql);

		/*
		 * 将以 "\echo" 开头的行置空，使脚本可包含提醒
		 * 不要通过 psql 运行的消息（因旧习惯而必要）。
		*
		 */
		t_sql = DirectFunctionCall4Coll(textregexreplace,
										C_COLLATION_OID,
										t_sql,
										CStringGetTextDatum("^\\\\echo.*$"),
										CStringGetTextDatum(""),
										CStringGetTextDatum("ng"));

		/*
		 * 若脚本使用 @extowner@，替换为调用用户名。
		 */
		if (strstr(c_sql, "@extowner@"))
		{
			Oid			uid = switch_to_superuser ? save_userid : GetUserId();
			const char *userName = GetUserNameFromId(uid, false);
			const char *qUserName = quote_identifier(userName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extowner@"),
											CStringGetTextDatum(qUserName));
			if (strpbrk(userName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension owner: must not contain any of \"%s\"",
								quoting_relevant_chars)));
		}

		/*
		 * 若扩展不可重定位，则将 @extschema@ 的出现替换 
		 * 为目标模式名。      
		 *
		 * 对于可重定位的扩展，无需这样做。不可能存在  
		 *  @extschema@ 需求，否则就不是可重定位的。   
		 */
		if (!control->relocatable)
		{
			Datum		old = t_sql;
			const char *qSchemaName = quote_identifier(schemaName);

			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("@extschema@"),
											CStringGetTextDatum(qSchemaName));
			if (t_sql != old && strpbrk(schemaName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension \"%s\" schema: must not contain any of \"%s\"",
								control->name, quoting_relevant_chars)));
		}

		/*
		 * 同样，将 @extschema:extension_name@ 替换为所需扩展的模式名。
		*
		 */
		Assert(list_length(control->requires) == list_length(requiredSchemas));
		forboth(lc, control->requires, lc2, requiredSchemas)
		{
			Datum		old = t_sql;
			char	   *reqextname = (char *) lfirst(lc);
			Oid			reqschema = lfirst_oid(lc2);
			char	   *schemaName = get_namespace_name(reqschema);
			const char *qSchemaName = quote_identifier(schemaName);
			char	   *repltoken;

			repltoken = psprintf("@extschema:%s@", reqextname);
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum(repltoken),
											CStringGetTextDatum(qSchemaName));
			if (t_sql != old && strpbrk(schemaName, quoting_relevant_chars))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("invalid character in extension \"%s\" schema: must not contain any of \"%s\"",
								reqextname, quoting_relevant_chars)));
		}

		/*
		 * 若控制文件中设置了 module_pathname，
		 * 将其值替换 MODULE_PATHNAME。
		 */
		if (control->module_pathname)
		{
			t_sql = DirectFunctionCall3Coll(replace_text,
											C_COLLATION_OID,
											t_sql,
											CStringGetTextDatum("MODULE_PATHNAME"),
											CStringGetTextDatum(control->module_pathname));
		}

		/* 回到 C 字符串 */
		c_sql = text_to_cstring(DatumGetTextPP(t_sql));

		execute_sql_string(c_sql);
	}
	PG_FINALLY();
	{
		creating_extension = false;
		CurrentExtensionObject = InvalidOid;
	}
	PG_END_TRY();

	/*
	 * 恢复上面设置的 GUC 变量。
	 */
	AtEOXact_GUC(true, save_nestlevel);

	/*
	 * 必要时恢复认证状态。
	 */
	if (switch_to_superuser)
		SetUserIdAndSecContext(save_userid, save_sec_context);
}

/*
 * 为指定的版本名查找或创建 ExtensionVersionInfo。         
 *
 * 目前我们只是使用 ExtensionVersionInfo 的链表。因此       
 * 当扩展有 N 个版本时，搜索耗时约 O(N^2)。如果         
 * 这成为瓶颈，我们可以把数据结构改为哈希表。      
 *                                
 */
static ExtensionVersionInfo *
get_ext_ver_info(const char *versionname, List **evi_list)
{
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	foreach(lc, *evi_list)
	{
		evi = (ExtensionVersionInfo *) lfirst(lc);
		if (strcmp(evi->name, versionname) == 0)
			return evi;
	}

	evi = (ExtensionVersionInfo *) palloc(sizeof(ExtensionVersionInfo));
	evi->name = pstrdup(versionname);
	evi->reachable = NIL;
	evi->installable = false;
	/* 初始化以供后续 Dijkstra 算法使用 */
	evi->distance_known = false;
	evi->distance = INT_MAX;
	evi->previous = NULL;

	*evi_list = lappend(*evi_list, evi);

	return evi;
}

/*
 * 定位最近未处理的 ExtensionVersionInfo
 *
 * 算法此部分也约为 O(N^2)。优先队列可大幅加速，但目前无需。
*
 */
static ExtensionVersionInfo *
get_nearest_unprocessed_vertex(List *evi_list)
{
	ExtensionVersionInfo *evi = NULL;
	ListCell   *lc;

	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);

		/* 仅距离仍不确定的顶点是候选 */
		if (evi2->distance_known)
			continue;
		/* 记住最近的此类顶点 */
		if (evi == NULL ||
			evi->distance > evi2->distance)
			evi = evi2;
	}

	return evi;
}

/*
 * 获取指定扩展可用更新脚本集的信息。
 * 返回 ExtensionVersionInfo 结构链表，每个包含可从该版本
 * 一步到达的版本的 ExtensionVersionInfo 子链表。
*
 */
static List *
get_ext_ver_list(ExtensionControlFile *control)
{
	List	   *evi_list = NIL;
	int			extnamelen = strlen(control->name);
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	location = get_extension_script_directory(control);
	dir = AllocateDir(location);
	while ((de = ReadDir(dir, location)) != NULL)
	{
		char	   *vername;
		char	   *vername2;
		ExtensionVersionInfo *evi;
		ExtensionVersionInfo *evi2;

		/* 必须是 .sql 文件 ... */
		if (!is_extension_script_filename(de->d_name))
			continue;

		/* ... 匹配扩展名后跟分隔符 */
		if (strncmp(de->d_name, control->name, extnamelen) != 0 ||
			de->d_name[extnamelen] != '-' ||
			de->d_name[extnamelen + 1] != '-')
			continue;

		/* 从 'extname--something.sql' 文件名中提取版本名 */
		vername = pstrdup(de->d_name + extnamelen + 2);
		*strrchr(vername, '.') = '\0';
		vername2 = strstr(vername, "--");
		if (!vername2)
		{
			/* 是安装脚本而非更新脚本；记录其版本名 */
			evi = get_ext_ver_info(vername, &evi_list);
			evi->installable = true;
			continue;
		}
		*vername2 = '\0';		/* 结束第一个版本 */
		vername2 += 2;			/* 指向第二个 */

		/* 若有第三个 --，为非法的，忽略 */
		if (strstr(vername2, "--"))
			continue;

		/* 创建 ExtensionVersionInfo 并链接它们 */
		evi = get_ext_ver_info(vername, &evi_list);
		evi2 = get_ext_ver_info(vername2, &evi_list);
		evi->reachable = lappend(evi->reachable, evi2);
	}
	FreeDir(dir);

	return evi_list;
}

/*
 * 给定初始版本名和最终版本名，找出为执行该更新      
 * 而必须应用的一系列更新脚本。             
 *
 * 结果是需经过的各版本名组成的链表（初始版本         
 * *不*包含在内）。    
 */
static List *
identify_update_path(ExtensionControlFile *control,
					 const char *oldVersion, const char *newVersion)
{
	List	   *result;
	List	   *evi_list;
	ExtensionVersionInfo *evi_start;
	ExtensionVersionInfo *evi_target;

	/* 从脚本目录提取版本更新图 */
	evi_list = get_ext_ver_list(control);

	/* 初始化起点和终点 */
	evi_start = get_ext_ver_info(oldVersion, &evi_list);
	evi_target = get_ext_ver_info(newVersion, &evi_list);

	/* 查找最短路径 */
	result = find_update_path(evi_list, evi_start, evi_target, false, false);

	if (result == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("extension \"%s\" has no update path from version \"%s\" to version \"%s\"",
						control->name, oldVersion, newVersion)));

	return result;
}

/*
 * 应用 Dijkstra 算法求 evi_start 到 evi_target 的最短路径。 
 *            
 *
 * 若 reject_indirect 为 true，则忽略经过可安装版本       
 * 的路径。调用者无论如何都要从所有可安装版本       
 * 出发，可省去工作量。  
 *
 * 若 reinitialize 为 false，则假定 ExtensionVersionInfo           
 *  链表此前未曾这样使用过，且 get_ext_ver_info 完成          
 * 的初始化仍然有效。否则，重新初始化这里用到的     
 *
 * 结果是需经过的各版本名组成的链表（初始版本         
 * *不*包含在内）。无此路径则返回 NIL。      
 */
static List *
find_update_path(List *evi_list,
				 ExtensionVersionInfo *evi_start,
				 ExtensionVersionInfo *evi_target,
				 bool reject_indirect,
				 bool reinitialize)
{
	List	   *result;
	ExtensionVersionInfo *evi;
	ListCell   *lc;

	/* start == target 为调用者错误 */
	Assert(evi_start != evi_target);
	/* reject_indirect 且 target 可安装为调用者错误 */
	Assert(!(reject_indirect && evi_target->installable));

	if (reinitialize)
	{
		foreach(lc, evi_list)
		{
			evi = (ExtensionVersionInfo *) lfirst(lc);
			evi->distance_known = false;
			evi->distance = INT_MAX;
			evi->previous = NULL;
		}
	}

	evi_start->distance = 0;

	while ((evi = get_nearest_unprocessed_vertex(evi_list)) != NULL)
	{
		if (evi->distance == INT_MAX)
			break;				/* 所有剩余顶点不可达 */
		evi->distance_known = true;
		if (evi == evi_target)
			break;				/* 找到到目标的最短路径 */
		foreach(lc, evi->reachable)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc);
			int			newdist;

			/* reject_indirect 时，将可安装版本视为不可达 */
			if (reject_indirect && evi2->installable)
				continue;
			newdist = evi->distance + 1;
			if (newdist < evi2->distance)
			{
				evi2->distance = newdist;
				evi2->previous = evi;
			}
			else if (newdist == evi2->distance &&
					 evi2->previous != NULL &&
					 strcmp(evi->name, evi2->previous->name) < 0)
			{
				/*
				 * 平局时倾向于 strcmp() 顺序中靠前的版本名。
				 * 此行为未文档化且用户不应依赖。
				 * 仅为确保平局时选择的更新路径不依赖于
				 * 目录条目访问顺序等随机因素。
				*
				*
				 */
				evi2->previous = evi;
			}
		}
	}

	/* 目标从起点不可达则返回 NIL */
	if (!evi_target->distance_known)
		return NIL;

	/* 构建并返回表示更新路径的版本名链表 */
	result = NIL;
	for (evi = evi_target; evi != evi_start; evi = evi->previous)
		result = lcons(evi->name, result);

	return result;
}

/*
 * 给定不直接可安装的目标版本，找到从直接可安装版本
 * 出发的最佳安装序列。
 *
 * evi_list：先前收集的版本更新图
 * evi_target：链表中我们要到达的目标成员
 *
 * 返回最佳起始版本，无则返回 NULL。成功时 *best_path
 * 设为从起始点的路径。
 *
 * 多个起始点时优先较短更新路径，平局基于起始版本名
 * strcmp 任意决定。
*
 */
static ExtensionVersionInfo *
find_install_path(List *evi_list, ExtensionVersionInfo *evi_target,
				  List **best_path)
{
	ExtensionVersionInfo *evi_start = NULL;
	ListCell   *lc;

	*best_path = NIL;

	/*
	 * 不应为可安装目标调用此处，但若调用了，
	 * 答案很简单：从该版本开始，更新路径为空。
	 */
	if (evi_target->installable)
		return evi_target;

	/* 将所有可安装版本视为起点 */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc);
		List	   *path;

		if (!evi1->installable)
			continue;

		/*
		 * 查找 evi1 到 evi_target 的最短路径；
		 * 无需考虑经过其他可安装版本的路径。
		 */
		path = find_update_path(evi_list, evi1, evi_target, true, true);
		if (path == NIL)
			continue;

		/* 记住最佳路径 */
		if (evi_start == NULL ||
			list_length(path) < list_length(*best_path) ||
			(list_length(path) == list_length(*best_path) &&
			 strcmp(evi_start->name, evi1->name) < 0))
		{
			evi_start = evi1;
			*best_path = path;
		}
	}

	return evi_start;
}

/*
 * CREATE EXTENSION 内部实现
 *
 * 指定 CASCADE 时，如需安装前置扩展则递归。为安全处理
 * 循环依赖，"parents" 链表包含已在安装中的扩展名，
 * 递归到其中某个时即可报错。
*
 */
static ObjectAddress
CreateExtensionInternal(char *extensionName,
						char *schemaName,
						const char *versionName,
						bool cascade,
						List *parents,
						bool is_create)
{
	char	   *origSchemaName = schemaName;
	Oid			schemaOid = InvalidOid;
	Oid			extowner = GetUserId();
	ExtensionControlFile *pcontrol;
	ExtensionControlFile *control;
	char	   *filename;
	struct stat fst;
	List	   *updateVersions;
	List	   *requiredExtensions;
	List	   *requiredSchemas;
	Oid			extensionOid;
	ObjectAddress address;
	ListCell   *lc;

	/*
	 * 读取主控制文件。注意，我们假定其中不含任何        
	 * 非 ASCII 数据，因此此时无需担心编码问题。             
	 *       
	 */
	pcontrol = read_extension_control_file(extensionName);

	/*
	 * 确定要安装的版本
	 */
	if (versionName == NULL)
	{
		if (pcontrol->default_version)
			versionName = pcontrol->default_version;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("version to install must be specified")));
	}
	check_valid_version_name(versionName);

	/*
	 * 弄清安装所需扩展版本需要执行哪些脚本。       
	 * 如果我们没有能直接完成所需工作的脚本，          
	 * 就尝试找到一串能达成目标的更新脚本。               
	 *                   
	 */
	filename = get_extension_script_filename(pcontrol, NULL, versionName);
	if (stat(filename, &fst) == 0)
	{
		/* 简单情况，无需额外脚本 */
		updateVersions = NIL;
	}
	else
	{
		/* 寻找安装此版本的最佳方式 */
		List	   *evi_list;
		ExtensionVersionInfo *evi_start;
		ExtensionVersionInfo *evi_target;

		/* 从脚本目录提取版本更新图 */
		evi_list = get_ext_ver_list(pcontrol);

		/* 识别目标版本 */
		evi_target = get_ext_ver_info(versionName, &evi_list);

		/* 识别到达目标的最佳路径 */
		evi_start = find_install_path(evi_list, evi_target,
									  &updateVersions);

		/* 无路径则失败 ... */
		if (evi_start == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("extension \"%s\" has no installation script nor update path for version \"%s\"",
							pcontrol->name, versionName)));

		/* 否则先安装最佳起点再升级 */
		versionName = evi_start->name;
	}

	/*
	 * 获取安装目标版本的控制参数
	 */
	control = read_extension_aux_control_file(pcontrol, versionName);

	/*
	 * 确定安装扩展的目标模式
	 */
	if (schemaName)
	{
		/* 若用户指定了模式名，该模式必须已存在。 */
		schemaOid = get_namespace_oid(schemaName, false);
	}

	if (control->schema != NULL)
	{
		/*
		 * 扩展不可重定位，且作者为它指定了一个模式。 
		 *        
		 *
		 * 除非指定了 CASCADE 参数，否则若 control->schema        
		 *  已指定，却又给出不同的模式，就是错误。     
		 */
		if (schemaName && strcmp(control->schema, schemaName) != 0 &&
			!cascade)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" must be installed in schema \"%s\"",
							control->name,
							control->schema)));

		/* 始终使用控制文件中的模式。 */
		schemaName = control->schema;

		/* 查找或创建模式。 */
		schemaOid = get_namespace_oid(schemaName, true);

		if (!OidIsValid(schemaOid))
		{
			CreateSchemaStmt *csstmt = makeNode(CreateSchemaStmt);

			csstmt->schemaname = schemaName;
			csstmt->authrole = NULL;	/* 将由当前用户创建 */
			csstmt->schemaElts = NIL;
			csstmt->if_not_exists = false;
			CreateSchemaCommand(csstmt, "(generated CREATE SCHEMA command)",
								-1, -1);

			/*
			 * CreateSchemaCommand 包含 CommandCounterIncrement，
			 * 新模式现已可见。
			 */
			schemaOid = get_namespace_oid(schemaName, false);
		}
	}
	else if (!OidIsValid(schemaOid))
	{
		/*
		 * 用户和扩展作者均未指定模式；使用当前默认
		 * 创建命名空间（search_path 的首个显式条目）。
		*
		 */
		List	   *search_path = fetch_search_path(false);

		if (search_path == NIL) /* search_path 中无有效项？ */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
		schemaOid = linitial_oid(search_path);
		schemaName = get_namespace_name(schemaOid);
		if (schemaName == NULL) /* 最近删除的命名空间？ */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));

		list_free(search_path);
	}

	/*
	 * 在此事务中访问了临时命名空间则做记录。
	*
	 */
	if (isTempNamespace(schemaOid))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/*
	 * 此处不检查目标命名空间的创建权限。若扩展脚本
	 * 实际在其中创建对象，用户无相应权限时会失败。
	 * 但如过程语言等场景，设置为 schema = pg_catalog
	 * 方便使用，不应限制为需有 pg_catalog ACL_CREATE。
	*
	*
	 */

	/*
	 * 查找前置扩展，必要时安装，构建其 OID 和
	 * 目标模式 OID 的列表。
	 */
	requiredExtensions = NIL;
	requiredSchemas = NIL;
	foreach(lc, control->requires)
	{
		char	   *curreq = (char *) lfirst(lc);
		Oid			reqext;
		Oid			reqschema;

		reqext = get_required_extension(curreq,
										extensionName,
										origSchemaName,
										cascade,
										parents,
										is_create);
		reqschema = get_extension_schema(reqext);
		requiredExtensions = lappend_oid(requiredExtensions, reqext);
		requiredSchemas = lappend_oid(requiredSchemas, reqschema);
	}

	/*
	 * 向 pg_extension 插入新元组并创建依赖条目。
	 */
	address = InsertExtensionTuple(control->name, extowner,
								   schemaOid, control->relocatable,
								   versionName,
								   PointerGetDatum(NULL),
								   PointerGetDatum(NULL),
								   requiredExtensions);
	extensionOid = address.objectId;

	/*
	 * 应用控制文件中对扩展的描述
	 */
	if (control->comment != NULL)
		CreateComments(extensionOid, ExtensionRelationId, 0, control->comment);

	/*
	 * 执行安装脚本文件
	 */
	execute_extension_script(extensionOid, control,
							 NULL, versionName,
							 requiredSchemas,
							 schemaName, schemaOid);

	/*
	 * 若有额外更新脚本需执行，按一系列
	 * ALTER EXTENSION UPDATE 命令的方式应用更新
	 */
	ApplyExtensionUpdates(extensionOid, pcontrol,
						  versionName, updateVersions,
						  origSchemaName, cascade, is_create);

	return address;
}

/*
 * 获取 "requires" 列表中扩展的 OID，可能需要创建它。
 */
static Oid
get_required_extension(char *reqExtensionName,
					   char *extensionName,
					   char *origSchemaName,
					   bool cascade,
					   List *parents,
					   bool is_create)
{
	Oid			reqExtensionOid;

	reqExtensionOid = get_extension_oid(reqExtensionName, true);
	if (!OidIsValid(reqExtensionOid))
	{
		if (cascade)
		{
			/* 必须安装它。 */
			ObjectAddress addr;
			List	   *cascade_parents;
			ListCell   *lc;

			/* 级联前检查扩展名有效性。 */
			check_valid_extension_name(reqExtensionName);

			/* 检查扩展间循环依赖。 */
			foreach(lc, parents)
			{
				char	   *pname = (char *) lfirst(lc);

				if (strcmp(pname, reqExtensionName) == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_RECURSION),
							 errmsg("cyclic dependency detected between extensions \"%s\" and \"%s\"",
									reqExtensionName, extensionName)));
			}

			ereport(NOTICE,
					(errmsg("installing required extension \"%s\"",
							reqExtensionName)));

			/* 将当前扩展添加到要传递的父级列表。 */
			cascade_parents = lappend(list_copy(parents), extensionName);

			/*
			 * 创建所需扩展。传播 SCHEMA 选项（如有）和 CASCADE，
			 * 不传播其他选项。
			 */
			addr = CreateExtensionInternal(reqExtensionName,
										   origSchemaName,
										   NULL,
										   cascade,
										   cascade_parents,
										   is_create);

			/* 获取其新分配的 OID。 */
			reqExtensionOid = addr.objectId;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("required extension \"%s\" is not installed",
							reqExtensionName),
					 is_create ?
					 errhint("Use CREATE EXTENSION ... CASCADE to install required extensions too.") : 0));
	}

	return reqExtensionOid;
}

/*
 * CREATE EXTENSION
 */
ObjectAddress
CreateExtension(ParseState *pstate, CreateExtensionStmt *stmt)
{
	DefElem    *d_schema = NULL;
	DefElem    *d_new_version = NULL;
	DefElem    *d_cascade = NULL;
	char	   *schemaName = NULL;
	char	   *versionName = NULL;
	bool		cascade = false;
	ListCell   *lc;

	/* 访问文件系统前检查扩展名有效性 */
	check_valid_extension_name(stmt->extname);

	/*
	 * 检查扩展名重复。pg_extension.extname 上的唯一索引
	 * 最终也会捕获，并作为竞态条件的最后防线；
	 * 但此处的错误消息更友好，且需要检查以支持 IF NOT EXISTS。
	*
	 */
	if (get_extension_oid(stmt->extname, true) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists, skipping",
							stmt->extname)));
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already exists",
							stmt->extname)));
	}

	/*
	 * 使用全局变量跟踪正在创建的扩展，同时只能创建一个。
	*
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested CREATE EXTENSION is not supported")));

	/* 解析语句选项列表 */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "schema") == 0)
		{
			if (d_schema)
				errorConflictingDefElem(defel, pstate);
			d_schema = defel;
			schemaName = defGetString(d_schema);
		}
		else if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				errorConflictingDefElem(defel, pstate);
			d_new_version = defel;
			versionName = defGetString(d_new_version);
		}
		else if (strcmp(defel->defname, "cascade") == 0)
		{
			if (d_cascade)
				errorConflictingDefElem(defel, pstate);
			d_cascade = defel;
			cascade = defGetBoolean(d_cascade);
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/* 调用 CreateExtensionInternal 执行实际工作。 */
	return CreateExtensionInternal(stmt->extname,
								   schemaName,
								   versionName,
								   cascade,
								   NIL,
								   true);
}

/*
 * InsertExtensionTuple
 *
 * 插入新 pg_extension 行并创建扩展的依赖条目。
 * 返回分配给新行的 OID。
 *
 * 导出此函数供 pg_upgrade 使用，需在不实际运行扩展脚本
 * 的情况下创建 pg_extension 条目（及扩展级依赖）。
 *
 * extConfig 和 extCondition 应为数组或 PointerGetDatum(NULL)。
 * 声明为普通 Datum 以避免 extension.h 需要 array.h。
*
 */
ObjectAddress
InsertExtensionTuple(const char *extName, Oid extOwner,
					 Oid schemaOid, bool relocatable, const char *extVersion,
					 Datum extConfig, Datum extCondition,
					 List *requiredExtensions)
{
	Oid			extensionOid;
	Relation	rel;
	Datum		values[Natts_pg_extension];
	bool		nulls[Natts_pg_extension];
	HeapTuple	tuple;
	ObjectAddress myself;
	ObjectAddress nsp;
	ObjectAddresses *refobjs;
	ListCell   *lc;

	/*
	 * 构建并插入 pg_extension 元组
	 */
	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	extensionOid = GetNewOidWithIndex(rel, ExtensionOidIndexId,
									  Anum_pg_extension_oid);
	values[Anum_pg_extension_oid - 1] = ObjectIdGetDatum(extensionOid);
	values[Anum_pg_extension_extname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(extName));
	values[Anum_pg_extension_extowner - 1] = ObjectIdGetDatum(extOwner);
	values[Anum_pg_extension_extnamespace - 1] = ObjectIdGetDatum(schemaOid);
	values[Anum_pg_extension_extrelocatable - 1] = BoolGetDatum(relocatable);
	values[Anum_pg_extension_extversion - 1] = CStringGetTextDatum(extVersion);

	if (extConfig == PointerGetDatum(NULL))
		nulls[Anum_pg_extension_extconfig - 1] = true;
	else
		values[Anum_pg_extension_extconfig - 1] = extConfig;

	if (extCondition == PointerGetDatum(NULL))
		nulls[Anum_pg_extension_extcondition - 1] = true;
	else
		values[Anum_pg_extension_extcondition - 1] = extCondition;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);
	table_close(rel, RowExclusiveLock);

	/*
	 * 记录对所有者、模式和前置扩展的依赖
	 */
	recordDependencyOnOwner(ExtensionRelationId, extensionOid, extOwner);

	refobjs = new_object_addresses();

	ObjectAddressSet(myself, ExtensionRelationId, extensionOid);

	ObjectAddressSet(nsp, NamespaceRelationId, schemaOid);
	add_exact_object_address(&nsp, refobjs);

	foreach(lc, requiredExtensions)
	{
		Oid			reqext = lfirst_oid(lc);
		ObjectAddress otherext;

		ObjectAddressSet(otherext, ExtensionRelationId, reqext);
		add_exact_object_address(&otherext, refobjs);
	}

	/* 记录所有依赖（含去重） */
	record_object_address_dependencies(&myself, refobjs, DEPENDENCY_NORMAL);
	free_object_addresses(refobjs);

	/* 新扩展的创建后钩子 */
	InvokeObjectPostCreateHook(ExtensionRelationId, extensionOid, 0);

	return myself;
}

/*
 * 扩展删除的内部实现。
 *
 * 只需删除 pg_extension 元组本身，其余由依赖基础设施处理。
*
 */
void
RemoveExtensionById(Oid extId)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * 禁止删除当前开放插入的扩展；否则后续
	 * recordDependencyOnCurrentExtension() 可能创建引用
	 * 不再有效的 pg_extension OID 的悬空 pg_depend 记录。
	 * 这不仅因为可能有人在脚本中写了 "DROP EXTENSION foo"，
	 * 更因为扩展脚本中的依赖管理错误可能导致从内部对象
	 * 递归删除扩展。因此必须在此处而非更上层的
	 * DROP EXTENSION 命令中检查。
	*
	*
	 */
	if (extId == CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot drop extension \"%s\" because it is being modified",
						get_extension_name(extId))));

	rel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extId));
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* 假定最多有一条匹配元组 */
	if (HeapTupleIsValid(tuple))
		CatalogTupleDelete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	table_close(rel, RowExclusiveLock);
}

/*
 * 此函数列出可用的扩展（控制目录中每个主控制文件    
 * 一行）。我们解析每个控制文件并报告有价值的字段。  
 *                    
 *
 * 系统视图 pg_available_extensions 为此 SRF 提供                   
 * 用户接口，增加扩展是否已安装在当前数据库的          
 * 信息。  
 */
Datum
pg_available_extensions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	/* 构建 tuplestore 存放结果行 */
	InitMaterializedSRF(fcinfo, 0);

	location = get_extension_control_directory();
	dir = AllocateDir(location);

	/*
	 * 若控制目录不存在则静默返回空集。
	 * 其他错误由 ReadDir 报告。
	 */
	if (dir == NULL && errno == ENOENT)
	{
		/* 不做任何事 */
	}
	else
	{
		while ((de = ReadDir(dir, location)) != NULL)
		{
			ExtensionControlFile *control;
			char	   *extname;
			Datum		values[3];
			bool		nulls[3];

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* 从 'name.control' 文件名提取扩展名 */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

			/* 辅助控制文件则忽略 */
			if (strstr(extname, "--"))
				continue;

			control = read_extension_control_file(extname);

			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* 名称 */
			values[0] = DirectFunctionCall1(namein,
											CStringGetDatum(control->name));
			/* default_version */
			if (control->default_version == NULL)
				nulls[1] = true;
			else
				values[1] = CStringGetTextDatum(control->default_version);
			/* 注解 */
			if (control->comment == NULL)
				nulls[2] = true;
			else
				values[2] = CStringGetTextDatum(control->comment);

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}

		FreeDir(dir);
	}

	return (Datum) 0;
}

/*
 * 此函数列出可用的扩展版本（每个扩展安装脚本  
 * 一行）。对于每个版本，我们解析相关的控制文件     
 * 并报告有价值的字段。                    
 *
 * 系统视图 pg_available_extension_versions 为此                      
 *  SRF 提供用户接口，增加哪些版本安装在当前数据库的  
 * 信息。  
 */
Datum
pg_available_extension_versions(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	/* 构建 tuplestore 存放结果行 */
	InitMaterializedSRF(fcinfo, 0);

	location = get_extension_control_directory();
	dir = AllocateDir(location);

	/*
	 * 若控制目录不存在则静默返回空集。
	 * 其他错误由 ReadDir 报告。
	 */
	if (dir == NULL && errno == ENOENT)
	{
		/* 不做任何事 */
	}
	else
	{
		while ((de = ReadDir(dir, location)) != NULL)
		{
			ExtensionControlFile *control;
			char	   *extname;

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* 从 'name.control' 文件名提取扩展名 */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

			/* 辅助控制文件则忽略 */
			if (strstr(extname, "--"))
				continue;

			/* 读取控制文件 */
			control = read_extension_control_file(extname);

			/* 扫描扩展脚本目录查找安装脚本 */
			get_available_versions_for_extension(control, rsinfo->setResult,
												 rsinfo->setDesc);
		}

		FreeDir(dir);
	}

	return (Datum) 0;
}

/*
 * pg_available_extension_versions 的内部循环：
 *		读取一个扩展的版本，向 tupstore 添加行
 */
static void
get_available_versions_for_extension(ExtensionControlFile *pcontrol,
									 Tuplestorestate *tupstore,
									 TupleDesc tupdesc)
{
	List	   *evi_list;
	ListCell   *lc;

	/* 从脚本目录提取版本更新图 */
	evi_list = get_ext_ver_list(pcontrol);

	/* 对每个可安装版本 ... */
	foreach(lc, evi_list)
	{
		ExtensionVersionInfo *evi = (ExtensionVersionInfo *) lfirst(lc);
		ExtensionControlFile *control;
		Datum		values[8];
		bool		nulls[8];
		ListCell   *lc2;

		if (!evi->installable)
			continue;

		/*
		 * 获取特定版本的参数（pcontrol 不变）
		 */
		control = read_extension_aux_control_file(pcontrol, evi->name);

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* 名称 */
		values[0] = DirectFunctionCall1(namein,
										CStringGetDatum(control->name));
		/* 版本 */
		values[1] = CStringGetTextDatum(evi->name);
		/* superuser */
		values[2] = BoolGetDatum(control->superuser);
		/* 受信 */
		values[3] = BoolGetDatum(control->trusted);
		/* 可重定位 */
		values[4] = BoolGetDatum(control->relocatable);
		/* 模式 */
		if (control->schema == NULL)
			nulls[5] = true;
		else
			values[5] = DirectFunctionCall1(namein,
											CStringGetDatum(control->schema));
		/* requires */
		if (control->requires == NIL)
			nulls[6] = true;
		else
			values[6] = convert_requires_to_datum(control->requires);
		/* 注解 */
		if (control->comment == NULL)
			nulls[7] = true;
		else
			values[7] = CStringGetTextDatum(control->comment);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/*
		 * 查找以此版本为起点可安装的所有非直接可安装版本，
		 * 报告它们，继承此版本更新中未更改的参数。
		*
		 */
		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *best_path;

			if (evi2->installable)
				continue;
			if (find_install_path(evi_list, evi2, &best_path) == evi)
			{
				/*
				 * 获取此版本的参数（pcontrol 不变）
				 */
				control = read_extension_aux_control_file(pcontrol, evi2->name);

				/* 名称保持不变 */
				/* 版本 */
				values[1] = CStringGetTextDatum(evi2->name);
				/* superuser */
				values[2] = BoolGetDatum(control->superuser);
				/* 受信 */
				values[3] = BoolGetDatum(control->trusted);
				/* 可重定位 */
				values[4] = BoolGetDatum(control->relocatable);
				/* 模式保持不变 */
				/* requires */
				if (control->requires == NIL)
					nulls[6] = true;
				else
				{
					values[6] = convert_requires_to_datum(control->requires);
					nulls[6] = false;
				}
				/* 注解保持不变 */

				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
		}
	}
}

/*
 * 测试给定扩展是否存在（非是否已安装）
 *
 * 检查扩展目录中是否存在匹配的控制文件。
 * 并非绝对可靠的检查（文件可能无效），
 * 但仅用于提示，无需 100% 准确。
*
 */
bool
extension_file_exists(const char *extensionName)
{
	bool		result = false;
	char	   *location;
	DIR		   *dir;
	struct dirent *de;

	location = get_extension_control_directory();
	dir = AllocateDir(location);

	/*
	 * 若控制目录不存在则静默返回 false。
	 * 其他错误由 ReadDir 报告。
	 */
	if (dir == NULL && errno == ENOENT)
	{
		/* 不做任何事 */
	}
	else
	{
		while ((de = ReadDir(dir, location)) != NULL)
		{
			char	   *extname;

			if (!is_extension_control_filename(de->d_name))
				continue;

			/* 从 'name.control' 文件名提取扩展名 */
			extname = pstrdup(de->d_name);
			*strrchr(extname, '.') = '\0';

			/* 辅助控制文件则忽略 */
			if (strstr(extname, "--"))
				continue;

			/* 匹配请求则完成 */
			if (strcmp(extname, extensionName) == 0)
			{
				result = true;
				break;
			}
		}

		FreeDir(dir);
	}

	return result;
}

/*
 * 将扩展名列表转换为 name[] Datum
 */
static Datum
convert_requires_to_datum(List *requires)
{
	Datum	   *datums;
	int			ndatums;
	ArrayType  *a;
	ListCell   *lc;

	ndatums = list_length(requires);
	datums = (Datum *) palloc(ndatums * sizeof(Datum));
	ndatums = 0;
	foreach(lc, requires)
	{
		char	   *curreq = (char *) lfirst(lc);

		datums[ndatums++] =
			DirectFunctionCall1(namein, CStringGetDatum(curreq));
	}
	a = construct_array_builtin(datums, ndatums, NAMEOID);
	return PointerGetDatum(a);
}

/*
 * 本函数报告指定扩展现存的各版本                    
 * 更新路径。     
 */
Datum
pg_extension_update_paths(PG_FUNCTION_ARGS)
{
	Name		extname = PG_GETARG_NAME(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	List	   *evi_list;
	ExtensionControlFile *control;
	ListCell   *lc1;

	/* 访问文件系统前检查扩展名有效性 */
	check_valid_extension_name(NameStr(*extname));

	/* 构建 tuplestore 存放结果行 */
	InitMaterializedSRF(fcinfo, 0);

	/* 读取扩展的控制文件 */
	control = read_extension_control_file(NameStr(*extname));

	/* 从脚本目录提取版本更新图 */
	evi_list = get_ext_ver_list(control);

	/* 遍历所有版本对 */
	foreach(lc1, evi_list)
	{
		ExtensionVersionInfo *evi1 = (ExtensionVersionInfo *) lfirst(lc1);
		ListCell   *lc2;

		foreach(lc2, evi_list)
		{
			ExtensionVersionInfo *evi2 = (ExtensionVersionInfo *) lfirst(lc2);
			List	   *path;
			Datum		values[3];
			bool		nulls[3];

			if (evi1 == evi2)
				continue;

			/* 查找 evi1 到 evi2 的最短路径 */
			path = find_update_path(evi_list, evi1, evi2, false, true);

			/* 输出结果行 */
			memset(values, 0, sizeof(values));
			memset(nulls, 0, sizeof(nulls));

			/* 来源 */
			values[0] = CStringGetTextDatum(evi1->name);
			/* 目标 */
			values[1] = CStringGetTextDatum(evi2->name);
			/* 路径 */
			if (path == NIL)
				nulls[2] = true;
			else
			{
				StringInfoData pathbuf;
				ListCell   *lcv;

				initStringInfo(&pathbuf);
				/* 路径不包含起点，但我们展示它 */
				appendStringInfoString(&pathbuf, evi1->name);
				foreach(lcv, path)
				{
					char	   *versionName = (char *) lfirst(lcv);

					appendStringInfoString(&pathbuf, "--");
					appendStringInfoString(&pathbuf, versionName);
				}
				values[2] = CStringGetTextDatum(pathbuf.data);
				pfree(pathbuf.data);
			}

			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
								 values, nulls);
		}
	}

	return (Datum) 0;
}

/*
 * pg_extension_config_dump
 *
 * 记录属于正在创建的扩展的配置表信息，
 * 该表内容应在 pg_dump 时全部或部分导出。
*
 */
Datum
pg_extension_config_dump(PG_FUNCTION_ARGS)
{
	Oid			tableoid = PG_GETARG_OID(0);
	text	   *wherecond = PG_GETARG_TEXT_PP(1);
	char	   *tablename;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	Datum		elementDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/*
	 * 仅允许从扩展 SQL 脚本中调用。除此之外无需权限检查。
	*
	 */
	if (!creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s can only be called from an SQL script executed by CREATE EXTENSION",
						"pg_extension_config_dump()")));

	/*
	 * 检查表是否存在且是正在创建的扩展的成员。
	 * 这样无需注册额外依赖来保护 extconfig 条目。
	*
	 */
	tablename = get_rel_name(tableoid);
	if (tablename == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("OID %u does not refer to a table", tableoid)));
	if (getExtensionOfObject(RelationRelationId, tableoid) !=
		CurrentExtensionObject)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("table \"%s\" is not a member of the extension being created",
						tablename)));

	/*
	 * 将表 OID 和 WHERE 条件添加到扩展的 extconfig 和
	 * extcondition 数组。
	 *
	 * 若表已在 extconfig 中，视为对 WHERE 条件的更新。
	*
	 */

	/* 查找 pg_extension 元组 */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(CurrentExtensionObject));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* 不应发生 */
		elog(ERROR, "could not find tuple for extension %u",
			 CurrentExtensionObject);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* 构建或修改 extconfig 值 */
	elementDatum = ObjectIdGetDatum(tableoid);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* extconfig 之前为空，构建单元素数组 */
		arrayLength = 0;
		arrayIndex = 1;

		a = construct_array_builtin(&elementDatum, 1, OIDOID);
	}
	else
	{
		/* 修改或扩展现有 extconfig 数组 */
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = arrayLength + 1;	/* 准备追加到末尾 */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i + 1; /* 替换此元素 */
				break;
			}
		}

		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena 数组 */ ,
					  sizeof(Oid) /* OID 的 typlen */ ,
					  true /* OID 的 typbyval */ ,
					  TYPALIGN_INT /* OID 的 typalign */ );
	}
	repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* 构建或修改 extcondition 值 */
	elementDatum = PointerGetDatum(wherecond);

	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		if (arrayLength != 0)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		a = construct_array_builtin(&elementDatum, 1, TEXTOID);
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");

		/* 在与 extconfig 相同的索引处添加或替换 */
		a = array_set(a, 1, &arrayIndex,
					  elementDatum,
					  false,
					  -1 /* varlena 数组 */ ,
					  -1 /* TEXT 的 typlen */ ,
					  false /* TEXT 的 typbyval */ ,
					  TYPALIGN_INT /* TEXT 的 typalign */ );
	}
	repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * extension_config_remove
 *
 * 从扩展的 extconfig 中移除指定表 OID（如存在）。
 * 当前未作为函数暴露，但可；目前仅从 ALTER EXTENSION DROP 调用。
*
 */
static void
extension_config_remove(Oid extensionoid, Oid tableoid)
{
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Datum		arrayDatum;
	int			arrayLength;
	int			arrayIndex;
	bool		isnull;
	Datum		repl_val[Natts_pg_extension];
	bool		repl_null[Natts_pg_extension];
	bool		repl_repl[Natts_pg_extension];
	ArrayType  *a;

	/* 查找 pg_extension 元组 */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionoid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* 不应发生 */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionoid);

	/* 在 extconfig 中搜索 tableoid */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extconfig,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		/* 无需操作 */
		a = NULL;
		arrayLength = 0;
		arrayIndex = -1;
	}
	else
	{
		Oid		   *arrayData;
		int			i;

		a = DatumGetArrayTypeP(arrayDatum);

		arrayLength = ARR_DIMS(a)[0];
		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			arrayLength < 0 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != OIDOID)
			elog(ERROR, "extconfig is not a 1-D Oid array");
		arrayData = (Oid *) ARR_DATA_PTR(a);

		arrayIndex = -1;		/* 无需删除的标志 */

		for (i = 0; i < arrayLength; i++)
		{
			if (arrayData[i] == tableoid)
			{
				arrayIndex = i; /* 要移除的索引 */
				break;
			}
		}
	}

	/* tableoid 不在 extconfig 中则无需操作 */
	if (arrayIndex < 0)
	{
		systable_endscan(extScan);
		table_close(extRel, RowExclusiveLock);
		return;
	}

	/* 修改或删除 extconfig 值 */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (arrayLength <= 1)
	{
		/* 仅剩一个元素，直接将数组设为 null */
		repl_null[Anum_pg_extension_extconfig - 1] = true;
	}
	else
	{
		/* 挤出目标元素 */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* 已检查无 null */
		deconstruct_array_builtin(a, OIDOID, &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array_builtin(dvalues, arrayLength - 1, OIDOID);

		repl_val[Anum_pg_extension_extconfig - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extconfig - 1] = true;

	/* 修改或删除 extcondition 值 */
	arrayDatum = heap_getattr(extTup, Anum_pg_extension_extcondition,
							  RelationGetDescr(extRel), &isnull);
	if (isnull)
	{
		elog(ERROR, "extconfig and extcondition arrays do not match");
	}
	else
	{
		a = DatumGetArrayTypeP(arrayDatum);

		if (ARR_NDIM(a) != 1 ||
			ARR_LBOUND(a)[0] != 1 ||
			ARR_HASNULL(a) ||
			ARR_ELEMTYPE(a) != TEXTOID)
			elog(ERROR, "extcondition is not a 1-D text array");
		if (ARR_DIMS(a)[0] != arrayLength)
			elog(ERROR, "extconfig and extcondition arrays do not match");
	}

	if (arrayLength <= 1)
	{
		/* 仅剩一个元素，直接将数组设为 null */
		repl_null[Anum_pg_extension_extcondition - 1] = true;
	}
	else
	{
		/* 挤出目标元素 */
		Datum	   *dvalues;
		int			nelems;
		int			i;

		/* 已检查无 null */
		deconstruct_array_builtin(a, TEXTOID, &dvalues, NULL, &nelems);

		for (i = arrayIndex; i < arrayLength - 1; i++)
			dvalues[i] = dvalues[i + 1];

		a = construct_array_builtin(dvalues, arrayLength - 1, TEXTOID);

		repl_val[Anum_pg_extension_extcondition - 1] = PointerGetDatum(a);
	}
	repl_repl[Anum_pg_extension_extcondition - 1] = true;

	extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
							   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	systable_endscan(extScan);

	table_close(extRel, RowExclusiveLock);
}

/*
 * 执行 ALTER EXTENSION SET SCHEMA
 */
ObjectAddress
AlterExtensionNamespace(const char *extensionName, const char *newschema, Oid *oldschema)
{
	Oid			extensionOid;
	Oid			nspOid;
	Oid			oldNspOid;
	AclResult	aclresult;
	Relation	extRel;
	ScanKeyData key[2];
	SysScanDesc extScan;
	HeapTuple	extTup;
	Form_pg_extension extForm;
	Relation	depRel;
	SysScanDesc depScan;
	HeapTuple	depTup;
	ObjectAddresses *objsMoved;
	ObjectAddress extAddr;

	extensionOid = get_extension_oid(extensionName, false);

	nspOid = LookupCreationNamespace(newschema);

	/*
	 * 权限检查：必须拥有扩展。注意不检查各个成员对象
	 * 的所有权 ...
	 */
	if (!object_ownercheck(ExtensionRelationId, extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   extensionName);

	/* 权限检查：必须有目标命名空间中的创建权限 */
	aclresult = object_aclcheck(NamespaceRelationId, nspOid, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, newschema);

	/*
	 * 若模式当前是扩展成员，禁止将扩展移入该模式，
	 * 否则会创建依赖循环。
	 */
	if (getExtensionOfObject(NamespaceRelationId, nspOid) == extensionOid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot move extension \"%s\" into schema \"%s\" "
						"because the extension contains the schema",
						extensionName, newschema)));

	/* 定位 pg_extension 元组 */
	extRel = table_open(ExtensionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))	/* 不应发生 */
		elog(ERROR, "could not find tuple for extension %u",
			 extensionOid);

	/* 复制元组以便下方修改 */
	extTup = heap_copytuple(extTup);
	extForm = (Form_pg_extension) GETSTRUCT(extTup);

	systable_endscan(extScan);

	/*
	 * 若扩展已在目标模式中，静默不做任何操作。
	*
	 */
	if (extForm->extnamespace == nspOid)
	{
		table_close(extRel, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/* 检查扩展是否应为可重定位 */
	if (!extForm->extrelocatable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("extension \"%s\" does not support SET SCHEMA",
						NameStr(extForm->extname))));

	objsMoved = new_object_addresses();

	/* 存储待更改的命名空间 OID */
	oldNspOid = extForm->extnamespace;

	/*
	 * 扫描 pg_depend 查找直接依赖扩展的对象，
	 * 更改每个对象的模式。
	 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ExtensionRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extensionOid));

	depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
								 NULL, 2, key);

	while (HeapTupleIsValid(depTup = systable_getnext(depScan)))
	{
		Form_pg_depend pg_depend = (Form_pg_depend) GETSTRUCT(depTup);
		ObjectAddress dep;
		Oid			dep_oldNspOid;

		/*
		 * 若依赖扩展对此扩展有 no_relocate 请求，
		 * 禁止 SET SCHEMA。（XXX 在执行重命名的同一
		 * 循环中进行检查不太优雅：可能在耗费大量工作后
		 * 才发现错误。但替代方案是两次扫描 pg_depend，
		 * 似乎是为失败情况优化。若此处失败，
		 * 重命名工作可干净回滚。）
		*
		 */
		if (pg_depend->deptype == DEPENDENCY_NORMAL &&
			pg_depend->classid == ExtensionRelationId)
		{
			char	   *depextname = get_extension_name(pg_depend->objid);
			ExtensionControlFile *dcontrol;
			ListCell   *lc;

			dcontrol = read_extension_control_file(depextname);
			foreach(lc, dcontrol->no_relocate)
			{
				char	   *nrextname = (char *) lfirst(lc);

				if (strcmp(nrextname, NameStr(extForm->extname)) == 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot SET SCHEMA of extension \"%s\" because other extensions prevent it",
									NameStr(extForm->extname)),
							 errdetail("Extension \"%s\" requests no relocation of extension \"%s\".",
									   depextname,
									   NameStr(extForm->extname))));
				}
			}
		}

		/*
		 * 否则忽略非成员依赖。
		 * （目前唯一其他情况是来自另一个扩展的普通依赖。）
		*
		 */
		if (pg_depend->deptype != DEPENDENCY_EXTENSION)
			continue;

		dep.classId = pg_depend->classid;
		dep.objectId = pg_depend->objid;
		dep.objectSubId = pg_depend->objsubid;

		if (dep.objectSubId != 0)	/* 不应发生 */
			elog(ERROR, "extension should not have a sub-object dependency");

		/* 重定位对象 */
		dep_oldNspOid = AlterObjectNamespace_oid(dep.classId,
												 dep.objectId,
												 nspOid,
												 objsMoved);

		/*
		 * 若非所有对象具有相同的旧命名空间
		 * （忽略不在命名空间中的），则报错。
		 */
		if (dep_oldNspOid != InvalidOid && dep_oldNspOid != oldNspOid)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("extension \"%s\" does not support SET SCHEMA",
							NameStr(extForm->extname)),
					 errdetail("%s is not in the extension's schema \"%s\"",
							   getObjectDescription(&dep, false),
							   get_namespace_name(oldNspOid))));
	}

	/* 若调用者需要，报告旧模式 */
	if (oldschema)
		*oldschema = oldNspOid;

	systable_endscan(depScan);

	relation_close(depRel, AccessShareLock);

	/* 现在调整 pg_extension.extnamespace */
	extForm->extnamespace = nspOid;

	CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

	table_close(extRel, RowExclusiveLock);

	/* 更新依赖指向新模式 */
	changeDependencyFor(ExtensionRelationId, extensionOid,
						NamespaceRelationId, oldNspOid, nspOid);

	InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

	ObjectAddressSet(extAddr, ExtensionRelationId, extensionOid);

	return extAddr;
}

/*
 * 执行 ALTER EXTENSION UPDATE
 */
ObjectAddress
ExecAlterExtensionStmt(ParseState *pstate, AlterExtensionStmt *stmt)
{
	DefElem    *d_new_version = NULL;
	char	   *versionName;
	char	   *oldVersionName;
	ExtensionControlFile *control;
	Oid			extensionOid;
	Relation	extRel;
	ScanKeyData key[1];
	SysScanDesc extScan;
	HeapTuple	extTup;
	List	   *updateVersions;
	Datum		datum;
	bool		isnull;
	ListCell   *lc;
	ObjectAddress address;

	/*
	 * 使用全局变量跟踪正在创建/更新的扩展，同时只能一个。
	*
	 */
	if (creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nested ALTER EXTENSION is not supported")));

	/*
	 * 查找扩展 --- 必须已存在于 pg_extension
	 */
	extRel = table_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_extension_extname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->extname));

	extScan = systable_beginscan(extRel, ExtensionNameIndexId, true,
								 NULL, 1, key);

	extTup = systable_getnext(extScan);

	if (!HeapTupleIsValid(extTup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" does not exist",
						stmt->extname)));

	extensionOid = ((Form_pg_extension) GETSTRUCT(extTup))->oid;

	/*
	 * 确定当前从哪个版本更新
	 */
	datum = heap_getattr(extTup, Anum_pg_extension_extversion,
						 RelationGetDescr(extRel), &isnull);
	if (isnull)
		elog(ERROR, "extversion is null");
	oldVersionName = text_to_cstring(DatumGetTextPP(datum));

	systable_endscan(extScan);

	table_close(extRel, AccessShareLock);

	/* 权限检查：必须拥有扩展 */
	if (!object_ownercheck(ExtensionRelationId, extensionOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * 读取主控制文件。注意，我们假定其中不含任何        
	 * 非 ASCII 数据，因此此时无需担心编码问题。             
	 *       
	 */
	control = read_extension_control_file(stmt->extname);

	/*
	 * 读取语句选项列表
	 */
	foreach(lc, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "new_version") == 0)
		{
			if (d_new_version)
				errorConflictingDefElem(defel, pstate);
			d_new_version = defel;
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	/*
	 * 确定要更新到的目标版本
	 */
	if (d_new_version && d_new_version->arg)
		versionName = strVal(d_new_version->arg);
	else if (control->default_version)
		versionName = control->default_version;
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("version to install must be specified")));
		versionName = NULL;		/* 让编译器安静 */
	}
	check_valid_version_name(versionName);

	/*
	 * 若已在该版本，直接告知
	 */
	if (strcmp(oldVersionName, versionName) == 0)
	{
		ereport(NOTICE,
				(errmsg("version \"%s\" of extension \"%s\" is already installed",
						versionName, stmt->extname)));
		return InvalidObjectAddress;
	}

	/*
	 * 确定需要执行的更新脚本文件序列
	 */
	updateVersions = identify_update_path(control,
										  oldVersionName,
										  versionName);

	/*
	 * 更新 pg_extension 行并逐个执行更新脚本
	*
	 */
	ApplyExtensionUpdates(extensionOid, control,
						  oldVersionName, updateVersions,
						  NULL, false, false);

	ObjectAddressSet(address, ExtensionRelationId, extensionOid);

	return address;
}

/*
 * 应用一系列更新脚本，就像单独给出了一个个         
 *  ALTER EXTENSION UPDATE 命令那样，包括每次修改 pg_extension 
 *  行与依赖项。        
 *
 * 这样做的工作量可能超出所需，但它能确保旧的       
 * 更新脚本在新版本拥有不同控制参数时也不会失效。   
 */
static void
ApplyExtensionUpdates(Oid extensionOid,
					  ExtensionControlFile *pcontrol,
					  const char *initialVersion,
					  List *updateVersions,
					  char *origSchemaName,
					  bool cascade,
					  bool is_create)
{
	const char *oldVersionName = initialVersion;
	ListCell   *lcv;

	foreach(lcv, updateVersions)
	{
		char	   *versionName = (char *) lfirst(lcv);
		ExtensionControlFile *control;
		char	   *schemaName;
		Oid			schemaOid;
		List	   *requiredExtensions;
		List	   *requiredSchemas;
		Relation	extRel;
		ScanKeyData key[1];
		SysScanDesc extScan;
		HeapTuple	extTup;
		Form_pg_extension extForm;
		Datum		values[Natts_pg_extension];
		bool		nulls[Natts_pg_extension];
		bool		repl[Natts_pg_extension];
		ObjectAddress myself;
		ListCell   *lc;

		/*
		 * 获取特定版本的参数（pcontrol 不变）
		 */
		control = read_extension_aux_control_file(pcontrol, versionName);

		/* 查找 pg_extension 元组 */
		extRel = table_open(ExtensionRelationId, RowExclusiveLock);

		ScanKeyInit(&key[0],
					Anum_pg_extension_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(extensionOid));

		extScan = systable_beginscan(extRel, ExtensionOidIndexId, true,
									 NULL, 1, key);

		extTup = systable_getnext(extScan);

		if (!HeapTupleIsValid(extTup))	/* 不应发生 */
			elog(ERROR, "could not find tuple for extension %u",
				 extensionOid);

		extForm = (Form_pg_extension) GETSTRUCT(extTup);

		/*
		 * 确定目标模式（由初始安装设定）
		 */
		schemaOid = extForm->extnamespace;
		schemaName = get_namespace_name(schemaOid);

		/*
		 * 修改 pg_extension 元组中的 extrelocatable 和 extversion
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));
		memset(repl, 0, sizeof(repl));

		values[Anum_pg_extension_extrelocatable - 1] =
			BoolGetDatum(control->relocatable);
		repl[Anum_pg_extension_extrelocatable - 1] = true;
		values[Anum_pg_extension_extversion - 1] =
			CStringGetTextDatum(versionName);
		repl[Anum_pg_extension_extversion - 1] = true;

		extTup = heap_modify_tuple(extTup, RelationGetDescr(extRel),
								   values, nulls, repl);

		CatalogTupleUpdate(extRel, &extTup->t_self, extTup);

		systable_endscan(extScan);

		table_close(extRel, RowExclusiveLock);

		/*
		 * 查找此版本的前置扩展，必要时安装，
		 * 构建其 OID 和目标模式 OID 的列表。
		*
		 */
		requiredExtensions = NIL;
		requiredSchemas = NIL;
		foreach(lc, control->requires)
		{
			char	   *curreq = (char *) lfirst(lc);
			Oid			reqext;
			Oid			reqschema;

			reqext = get_required_extension(curreq,
											control->name,
											origSchemaName,
											cascade,
											NIL,
											is_create);
			reqschema = get_extension_schema(reqext);
			requiredExtensions = lappend_oid(requiredExtensions, reqext);
			requiredSchemas = lappend_oid(requiredSchemas, reqschema);
		}

		/*
		 * 移除并重新创建对前置扩展的依赖
		 */
		deleteDependencyRecordsForClass(ExtensionRelationId, extensionOid,
										ExtensionRelationId,
										DEPENDENCY_NORMAL);

		myself.classId = ExtensionRelationId;
		myself.objectId = extensionOid;
		myself.objectSubId = 0;

		foreach(lc, requiredExtensions)
		{
			Oid			reqext = lfirst_oid(lc);
			ObjectAddress otherext;

			otherext.classId = ExtensionRelationId;
			otherext.objectId = reqext;
			otherext.objectSubId = 0;

			recordDependencyOn(&myself, &otherext, DEPENDENCY_NORMAL);
		}

		InvokeObjectPostAlterHook(ExtensionRelationId, extensionOid, 0);

		/*
		 * 最后，执行更新脚本文件
		 */
		execute_extension_script(extensionOid, control,
								 oldVersionName, versionName,
								 requiredSchemas,
								 schemaName, schemaOid);

		/*
		 * 更新前一版本名并循环。
		 * execute_sql_string 已完成 CommandCounterIncrement，
		 * 可再次更新 pg_extension 行。
		 */
		oldVersionName = versionName;
	}
}

/*
 * 执行 ALTER EXTENSION ADD/DROP
 *
 * 返回值为被修改的扩展的地址。
 *
 * objAddr 是输出参数，非 NULL 时设置为添加/删除对象的地址。
*
 */
ObjectAddress
ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
							   ObjectAddress *objAddr)
{
	ObjectAddress extension;
	ObjectAddress object;
	Relation	relation;
	Oid			oldExtension;

	switch (stmt->objtype)
	{
		case OBJECT_DATABASE:
		case OBJECT_EXTENSION:
		case OBJECT_INDEX:
		case OBJECT_PUBLICATION:
		case OBJECT_ROLE:
		case OBJECT_STATISTIC_EXT:
		case OBJECT_SUBSCRIPTION:
		case OBJECT_TABLESPACE:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot add an object of this type to an extension")));
			break;
		default:
			/* OK */
			break;
	}

	/*
	 * 查找扩展并获取锁，防止并发删除。
	 * 共享锁应足够：允许其他操作（如其他对象的 add/drop）并发。
	 * 并发添加/删除*同一*对象会造成问题，
	 * 但下方通过对个别对象使用非共享锁来防止。
	*
	*
	 */
	extension = get_object_address(OBJECT_EXTENSION,
								   (Node *) makeString(stmt->extname),
								   &relation, AccessShareLock, false);

	/* 权限检查：必须拥有扩展 */
	if (!object_ownercheck(ExtensionRelationId, extension.objectId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_EXTENSION,
					   stmt->extname);

	/*
	 * 将标识对象的解析器表示转换为 ObjectAddress。
	 * get_object_address() 在对象不存在时抛出错误，
	 * 并获取对象锁以防御并发 DROP 和 ALTER EXTENSION ADD/DROP。
	*
	 */
	object = get_object_address(stmt->objtype, stmt->object,
								&relation, ShareUpdateExclusiveLock, false);

	Assert(object.objectSubId == 0);
	if (objAddr)
		*objAddr = object;

	/* 权限检查：还必须拥有目标对象 */
	check_object_ownership(GetUserId(), stmt->objtype, object,
						   stmt->object, relation);

	/*
	 * 检查现有扩展成员关系。
	 */
	oldExtension = getExtensionOfObject(object.classId, object.objectId);

	if (stmt->action > 0)
	{
		/*
		 * ADD，若对象已附加到某个扩展则报错。
		 */
		if (OidIsValid(oldExtension))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is already a member of extension \"%s\"",
							getObjectDescription(&object, false),
							get_extension_name(oldExtension))));

		/*
		 * 若模式包含扩展，禁止将模式添加到扩展，
		 * 否则会创建依赖循环。
		 */
		if (object.classId == NamespaceRelationId &&
			object.objectId == get_extension_schema(extension.objectId))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot add schema \"%s\" to extension \"%s\" "
							"because the schema contains the extension",
							get_namespace_name(object.objectId),
							stmt->extname)));

		/*
		 * OK，添加依赖。
		 */
		recordDependencyOn(&object, &extension, DEPENDENCY_EXTENSION);

		/*
		 * 同时记录对象上的初始 ACL（如有）。 
		 *
		 * 注意，这会同时处理对象自身的 ACL 以及对象      
		 * 子 ID 上的 ACL。（换言之，当对象是一张表时， 
		 * 这会记录表的 ACL 以及表中各列的 ACL（如          
		 * 有的话）。    
		 */
		recordExtObjInitPriv(object.objectId, object.classId);
	}
	else
	{
		/*
		 * DROP，若对象非成员则报错。
		 */
		if (oldExtension != extension.objectId)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("%s is not a member of extension \"%s\"",
							getObjectDescription(&object, false),
							stmt->extname)));

		/*
		 * OK，删除依赖。
		 */
		if (deleteDependencyRecordsForClass(object.classId, object.objectId,
											ExtensionRelationId,
											DEPENDENCY_EXTENSION) != 1)
			elog(ERROR, "unexpected number of extension dependency records");

		/*
		 * 若为关系，可能在扩展的 extconfig 数组中有条目，
		 * 必须移除。
		 */
		if (object.classId == RelationRelationId)
			extension_config_remove(extension.objectId, object.objectId);

		/*
		 * 移除所有初始 ACL，如有。  
		 *
		 * 注意，这会同时移除对象自身的 ACL 以及对象      
		 * 子 ID 上的 ACL。（换言之，当对象是一张表时， 
		 * 这会移除表的 ACL 以及表中各列的 ACL（如          
		 * 有的话）。    
		 */
		removeExtObjInitPriv(object.objectId, object.classId);
	}

	InvokeObjectPostAlterHook(ExtensionRelationId, extension.objectId, 0);

	/*
	 * 若 get_object_address() 打开了关系，则关闭以保持
	 * 引用计数正确——但保留其获取的锁直到提交时刻，
	 * 以防御并发活动。
	*
	 */
	if (relation != NULL)
		relation_close(relation, NoLock);

	return extension;
}

/*
 * 将整个文件读入内存。
 *
 * 文件内容以单个 palloc 分配块返回。为方便调用者，
 * 末尾附加一个 \0 字节。
 */
static char *
read_whole_file(const char *filename, int *length)
{
	char	   *buf;
	FILE	   *file;
	size_t		bytes_to_read;
	struct stat fst;

	if (stat(filename, &fst) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", filename)));

	if (fst.st_size > (MaxAllocSize - 1))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("file \"%s\" is too large", filename)));
	bytes_to_read = (size_t) fst.st_size;

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for reading: %m",
						filename)));

	buf = (char *) palloc(bytes_to_read + 1);

	*length = fread(buf, 1, bytes_to_read, file);

	if (ferror(file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));

	FreeFile(file);

	buf[*length] = '\0';
	return buf;
}
