/*--------------------------------------------------------------------
 * guc.c
 *
 * 支持大统一配置(GUC)方案,包括 SET
 * 命令、配置文件以及命令行选项。
 *
 * 本文件包含通用的选项处理基础设施。
 * guc_funcs.c 包含 SQL 层面的功能,包括 SET/SHOW
 * 命令以及各种系统管理类的 SQL 函数。
 * guc_tables.c 包含定义全部内建 GUC 变量的数组。
 * 实现变量特有行为的代码,
 * 散布在系统各处的检查、赋值和显示钩子中。
 *
 * 更多信息参见 src/backend/utils/misc/README。
 *
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_parameter_acl.h"
#include "guc_internal.h"
#include "libpq/pqformat.h"
#include "parser/scansup.h"
#include "port/pg_bitutils.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/conffiles.h"
#include "utils/float.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"


#define CONFIG_FILENAME "postgresql.conf"
#define HBA_FILENAME	"pg_hba.conf"
#define IDENT_FILENAME	"pg_ident.conf"

#ifdef EXEC_BACKEND
#define CONFIG_EXEC_PARAMS "global/config_exec_params"
#define CONFIG_EXEC_PARAMS_NEW "global/config_exec_params.new"
#endif

/*
 * REAL 类型的 GUC 值在 GUC 序列化时打印所用的精度。
 *
 */
#define REALTYPE_PRECISION 17

static int	GUC_check_errcode_value;

static List *reserved_class_prefix = NIL;

/* 用于检查钩子支持的全局变量 */
char	   *GUC_check_errmsg_string;
char	   *GUC_check_errdetail_string;
char	   *GUC_check_errhint_string;

/* 权宜:为提速,我们直接读该 GUC 变量的值 */
extern bool in_hot_standby_guc;


/*
 * 单位换算表。
 *
 * 有两张表,一张用于内存单位,另一张用于时间单位。
 * 对每种受支持的单位间换算,表中都有一个对应表项。
 *
 *
 * 为了保持简单并避免可能的舍入误差,
 * 换算从不级联。同类型的所有单位之间都必须有
 * 直接换算。
 *
 * 每个基础单位的换算必须按人类友好单位从大到小
 * 的顺序排列;convert_xxx_from_base_unit() 依赖这一点。
 * (基础单位组的顺序则无关紧要。)
 */
#define MAX_UNIT_LEN		3	/* 最长可识别单位串的长度 */

typedef struct
{
	char		unit[MAX_UNIT_LEN + 1]; /* 单位,字符串,如 "kB"
										 * "min" */
	int			base_unit;		/* GUC_UNIT_XXX */
	double		multiplier;		/* unit 换算到 base_unit 的系数 */
} unit_conversion;

/* 确保表中常量不会溢出或下溢 */
#if BLCKSZ < 1024 || BLCKSZ > (1024*1024)
#error BLCKSZ must be between 1KB and 1MB
#endif
#if XLOG_BLCKSZ < 1024 || XLOG_BLCKSZ > (1024*1024)
#error XLOG_BLCKSZ must be between 1KB and 1MB
#endif

static const char *memory_units_hint = gettext_noop("Valid units for this parameter are \"B\", \"kB\", \"MB\", \"GB\", and \"TB\".");

static const unit_conversion memory_unit_conversion_table[] =
{
	{"TB", GUC_UNIT_BYTE, 1024.0 * 1024.0 * 1024.0 * 1024.0},
	{"GB", GUC_UNIT_BYTE, 1024.0 * 1024.0 * 1024.0},
	{"MB", GUC_UNIT_BYTE, 1024.0 * 1024.0},
	{"kB", GUC_UNIT_BYTE, 1024.0},
	{"B", GUC_UNIT_BYTE, 1.0},

	{"TB", GUC_UNIT_KB, 1024.0 * 1024.0 * 1024.0},
	{"GB", GUC_UNIT_KB, 1024.0 * 1024.0},
	{"MB", GUC_UNIT_KB, 1024.0},
	{"kB", GUC_UNIT_KB, 1.0},
	{"B", GUC_UNIT_KB, 1.0 / 1024.0},

	{"TB", GUC_UNIT_MB, 1024.0 * 1024.0},
	{"GB", GUC_UNIT_MB, 1024.0},
	{"MB", GUC_UNIT_MB, 1.0},
	{"kB", GUC_UNIT_MB, 1.0 / 1024.0},
	{"B", GUC_UNIT_MB, 1.0 / (1024.0 * 1024.0)},

	{"TB", GUC_UNIT_BLOCKS, (1024.0 * 1024.0 * 1024.0) / (BLCKSZ / 1024)},
	{"GB", GUC_UNIT_BLOCKS, (1024.0 * 1024.0) / (BLCKSZ / 1024)},
	{"MB", GUC_UNIT_BLOCKS, 1024.0 / (BLCKSZ / 1024)},
	{"kB", GUC_UNIT_BLOCKS, 1.0 / (BLCKSZ / 1024)},
	{"B", GUC_UNIT_BLOCKS, 1.0 / BLCKSZ},

	{"TB", GUC_UNIT_XBLOCKS, (1024.0 * 1024.0 * 1024.0) / (XLOG_BLCKSZ / 1024)},
	{"GB", GUC_UNIT_XBLOCKS, (1024.0 * 1024.0) / (XLOG_BLCKSZ / 1024)},
	{"MB", GUC_UNIT_XBLOCKS, 1024.0 / (XLOG_BLCKSZ / 1024)},
	{"kB", GUC_UNIT_XBLOCKS, 1.0 / (XLOG_BLCKSZ / 1024)},
	{"B", GUC_UNIT_XBLOCKS, 1.0 / XLOG_BLCKSZ},

	{""}						/* 表尾标记 */
};

static const char *time_units_hint = gettext_noop("Valid units for this parameter are \"us\", \"ms\", \"s\", \"min\", \"h\", and \"d\".");

static const unit_conversion time_unit_conversion_table[] =
{
	{"d", GUC_UNIT_MS, 1000 * 60 * 60 * 24},
	{"h", GUC_UNIT_MS, 1000 * 60 * 60},
	{"min", GUC_UNIT_MS, 1000 * 60},
	{"s", GUC_UNIT_MS, 1000},
	{"ms", GUC_UNIT_MS, 1},
	{"us", GUC_UNIT_MS, 1.0 / 1000},

	{"d", GUC_UNIT_S, 60 * 60 * 24},
	{"h", GUC_UNIT_S, 60 * 60},
	{"min", GUC_UNIT_S, 60},
	{"s", GUC_UNIT_S, 1},
	{"ms", GUC_UNIT_S, 1.0 / 1000},
	{"us", GUC_UNIT_S, 1.0 / (1000 * 1000)},

	{"d", GUC_UNIT_MIN, 60 * 24},
	{"h", GUC_UNIT_MIN, 60},
	{"min", GUC_UNIT_MIN, 1},
	{"s", GUC_UNIT_MIN, 1.0 / 60},
	{"ms", GUC_UNIT_MIN, 1.0 / (1000 * 60)},
	{"us", GUC_UNIT_MIN, 1.0 / (1000 * 1000 * 60)},

	{""}						/* 表尾标记 */
};

/*
 * 为继续支持过时的 GUC 变量名,对任何无法识别的名字
 * 都应用以下映射。注意:仅当新旧变量语义非常接近
 * 时才应把旧名字映射到新名字。
 *
 */
static const char *const map_old_guc_names[] = {
	"sort_mem", "work_mem",
	"vacuum_mem", "maintenance_work_mem",
	NULL
};


/* 保存 GUC 相关数据的内存上下文 */
static MemoryContext GUCMemoryContext;

/*
 * 我们用一个 dynahash 表按名称查找 GUC,或遍历全部
 * GUCs。gucname 字段与 gucvar->name 冗余,
 * 但 dynahash 若不单独存放哈希键会很痛苦。
 */
typedef struct
{
	const char *gucname;		/* 哈希键*/
	struct config_generic *gucvar;	/* -> GUC 的定义结构 */
} GUCHashEntry;

static HTAB *guc_hashtab;		/* 表项为 GUCHashEntrys */

/*
 * 除哈希表之外,具有某些特性的变量会被链接到
 * 这些链表,从而无需扫描整个哈希表即可找到它们。
 * 在多数应用中,任意时刻只有一小部分 GUCs 出现在
 * 这些链表中。栈链表和报告链表的用法相当固定,
 * 完全可以用 slist 实现;但 nondef 链表必须是 dlist,
 * 以避免常见情形下 O(N) 的删除开销。
 */
static dlist_head guc_nondef_list;	/* source 不同于 PGC_S_DEFAULT
									  * 的变量列表 */
static slist_head guc_stack_list;	/* stack 非 NULL 的变量列表
									  */
static slist_head guc_report_list;	/* status 置有 GUC_NEEDS_REPORT
									  * 位的变量列表 */

static bool reporting_enabled;	/* true 时启用 GUC_REPORT */

static int	GUCNestLevel = 0;	/* 主事务中为 1 */


static int	guc_var_compare(const void *a, const void *b);
static uint32 guc_name_hash(const void *key, Size keysize);
static int	guc_name_match(const void *key1, const void *key2, Size keysize);
static void InitializeGUCOptionsFromEnvironment(void);
static void InitializeOneGUCOption(struct config_generic *gconf);
static void RemoveGUCFromLists(struct config_generic *gconf);
static void set_guc_source(struct config_generic *gconf, GucSource newsource);
static void pg_timezone_abbrev_initialize(void);
static void push_old_value(struct config_generic *gconf, GucAction action);
static void ReportGUCOption(struct config_generic *record);
static void set_config_sourcefile(const char *name, char *sourcefile,
								  int sourceline);
static void reapply_stacked_values(struct config_generic *variable,
								   struct config_string *pHolder,
								   GucStack *stack,
								   const char *curvalue,
								   GucContext curscontext, GucSource cursource,
								   Oid cursrole);
static bool validate_option_array_item(const char *name, const char *value,
									   bool skipIfNoPermissions);
static void write_auto_conf_file(int fd, const char *filename, ConfigVariable *head);
static void replace_auto_config_value(ConfigVariable **head_p, ConfigVariable **tail_p,
									  const char *name, const char *value);
static bool valid_custom_variable_name(const char *name);
static void do_serialize(char **destptr, Size *maxbytes,
						 const char *fmt,...) pg_attribute_printf(3, 4);
static bool call_bool_check_hook(struct config_bool *conf, bool *newval,
								 void **extra, GucSource source, int elevel);
static bool call_int_check_hook(struct config_int *conf, int *newval,
								void **extra, GucSource source, int elevel);
static bool call_real_check_hook(struct config_real *conf, double *newval,
								 void **extra, GucSource source, int elevel);
static bool call_string_check_hook(struct config_string *conf, char **newval,
								   void **extra, GucSource source, int elevel);
static bool call_enum_check_hook(struct config_enum *conf, int *newval,
								 void **extra, GucSource source, int elevel);


/*
 * 本函数既处理实际的配置文件(重)加载,也处理
 * show_all_file_settings()(即 pg_file_settings 视图)。
 * 对于后者,我们不应用任何设置,但会执行所有常规的
 * 有效性检查,并返回 ConfigVariable 列表,以便
 * 打印输出。
 */
ConfigVariable *
ProcessConfigFileInternal(GucContext context, bool applySettings, int elevel)
{
	bool		error = false;
	bool		applying = false;
	const char *ConfFileWithError;
	ConfigVariable *item,
			   *head,
			   *tail;
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;

	/* 把主配置文件解析为选项名和值的列表 */
	ConfFileWithError = ConfigFileName;
	head = tail = NULL;

	if (!ParseConfigFile(ConfigFileName, true,
						 NULL, 0, CONF_FILE_START_DEPTH, elevel,
						 &head, &tail))
	{
		/* 文件中检测到语法错误,故中止 */
		error = true;
		goto bail_out;
	}

	/*
	 * 若存在,则在主文件之后解析 PG_AUTOCONF_FILENAME 文件,
	 * 以替换 ALTER SYSTEM 命令设置的参数。由于该文件
	 * 位于数据目录中,DataDir 设置之前无法读取它。
	 *
	 */
	if (DataDir)
	{
		if (!ParseConfigFile(PG_AUTOCONF_FILENAME, false,
							 NULL, 0, CONF_FILE_START_DEPTH, elevel,
							 &head, &tail))
		{
			/* 文件中检测到语法错误,故中止 */
			error = true;
			ConfFileWithError = PG_AUTOCONF_FILENAME;
			goto bail_out;
		}
	}
	else
	{
		/*
		 * 若 DataDir 未设置,则无法读取 PG_AUTOCONF_FILENAME
		 * 文件。此时我们不愿接受除 postgresql.conf 中
		 * data_directory 之外的任何设置,因为它们可能会
		 * 被 PG_AUTOCONF_FILENAME 文件中的设置所覆盖,而该
		 * 文件稍后会被读取。另一方面,data_directory 本身
		 * 不允许出现在其中,因此永远不会被覆盖。
		 */
		ConfigVariable *newlist = NULL;

		/*
		 * 剔除除最后一个 "data_directory" 外的所有条目。
		 */
		for (item = head; item; item = item->next)
		{
			if (!item->ignore &&
				strcmp(item->name, "data_directory") == 0)
				newlist = item;
		}

		if (newlist)
			newlist->next = NULL;
		head = tail = newlist;

		/*
		 * 文件中无 data_directory 时快速退出。
		 *
		 * 无需再做任何进一步处理,特别是我们不设置
		 * PgReloadTime;随后的完整加载很快就会设置它。
		 *
		 */
		if (head == NULL)
			goto bail_out;
	}

	/*
	 * 把所有现存 GUC 变量标记为不在配置文件中。我们
	 * 需要它来判断哪些变量自上次处理之后已从文件中
	 * 被移除。
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *gconf = hentry->gucvar;

		gconf->status &= ~GUC_IS_IN_FILE;
	}

	/*
	 * 检查所有给出的选项名是否有效,作为对配置文件
	 * 有效性的准语法检查。postmaster 与所有后端须就
	 * 本阶段结果达成一致,否则对于哪些进程接受配置
	 * 文件更新哪些不接受,就会出现奇怪的不一致。
	 * 因此,未知的自定义变量名必须毫无异议地接受。
	 * 出于同一原因,我们在此不尝试验证选项的值。
	 *
	 *
	 * 此外,文件中提到的每个现存 GUC 变量都会设置
	 * GUC_IS_IN_FILE 标志;并且我们检测文件中的重复
	 * 表项,并把较早的出现标记为可忽略。
	 */
	for (item = head; item; item = item->next)
	{
		struct config_generic *record;

		/* 忽略任何已标记为可忽略的东西 */
		if (item->ignore)
			continue;

		/*
		 * 尝试查找该变量;但如果它尚不存在,则不要创建
		 * 自定义占位符。
		 */
		record = find_option(item->name, false, true, elevel);

		if (record)
		{
			/* 若已标记,则这是重复的表项 */
			if (record->status & GUC_IS_IN_FILE)
			{
				/*
				 * 把较早的出现标记为失效/可忽略。本可以用
				 * 一些额外状态来避免这里的 O(N^2) 行为,但看
				 * 来不值得为此费这个麻烦。
				 */
				ConfigVariable *pitem;

				for (pitem = head; pitem != item; pitem = pitem->next)
				{
					if (!pitem->ignore &&
						strcmp(pitem->name, item->name) == 0)
						pitem->ignore = true;
				}
			}
			/* 现标记为已在文件中 */
			record->status |= GUC_IS_IN_FILE;
		}
		else if (!valid_custom_variable_name(item->name))
		{
			/* 非自定义的无效变量,故报错 */
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("unrecognized configuration parameter \"%s\" in file \"%s\" line %d",
							item->name,
							item->filename, item->sourceline)));
			item->errmsg = pstrdup("unrecognized configuration parameter");
			error = true;
			ConfFileWithError = item->filename;
		}
	}

	/*
	 * 如果迄今已检测到错误,我们不想冒险应用任何更
	 * 改。
	 */
	if (error)
		goto bail_out;

	/* 否则,设置我们即将开始应用更改的标志 */
	applying = true;

	/*
	 * 检查从配置文件中移除的变量,并把它们的重置值
	 * (可能还有生效值)回退到启动时的默认值。如果
	 * 这样的变量在启动后不能更改,则报告并继续。
	 *
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *gconf = hentry->gucvar;
		GucStack   *stack;

		if (gconf->reset_source != PGC_S_FILE ||
			(gconf->status & GUC_IS_IN_FILE))
			continue;
		if (gconf->context < PGC_SIGHUP)
		{
			/* 移除不重启就无法生效 */
			gconf->status |= GUC_PENDING_RESTART;
			ereport(elevel,
					(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					 errmsg("parameter \"%s\" cannot be changed without restarting the server",
							gconf->name)));
			record_config_file_error(psprintf("parameter \"%s\" cannot be changed without restarting the server",
											  gconf->name),
									 NULL, 0,
									 &head, &tail);
			error = true;
			continue;
		}

		/* 若只是执行 show_all_file_settings() 则无事可做 */
		if (!applySettings)
			continue;

		/*
		 * 把 "file" 来源重置为 "default",否则 set_config_option
		 * 无法覆盖相应设置。
		 */
		if (gconf->reset_source == PGC_S_FILE)
			gconf->reset_source = PGC_S_DEFAULT;
		if (gconf->source == PGC_S_FILE)
			set_guc_source(gconf, PGC_S_DEFAULT);
		for (stack = gconf->stack; stack; stack = stack->prev)
		{
			if (stack->source == PGC_S_FILE)
				stack->source = PGC_S_DEFAULT;
		}

		/* 现在可以重新应用内建默认值(即 boot_val) */
		if (set_config_option(gconf->name, NULL,
							  context, PGC_S_DEFAULT,
							  GUC_ACTION_SET, true, 0, false) > 0)
		{
			/* 适当时记录该更改 */
			if (context == PGC_SIGHUP)
				ereport(elevel,
						(errmsg("parameter \"%s\" removed from configuration file, reset to default",
								gconf->name)));
		}
	}

	/*
	 * 恢复任何由环境变量或动态计算默认值决定的
	 * 变量。除非该变量曾在配置文件中且现被移除,
	 * 本操作不会有任何效果。
	 *
	 * 特别地,在 postmaster 初次加载该文件期间,我们
	 * 绝不能这样做,因为时区函数尤其应当只在初始
	 * 化完成之后才运行。
	 *
	 * XXX 这是一段不可维护的拙劣代码,因为我们必须知道
	 * 如何设置(或至少调用什么来设置)每一种可能具有
	 * PGC_S_DYNAMIC_DEFAULT/PGC_S_ENV_VAR 来源的变量。
	 */
	if (context == PGC_SIGHUP && applySettings)
	{
		InitializeGUCOptionsFromEnvironment();
		pg_timezone_abbrev_initialize();
		/* 在未连接到数据库的进程中这会选择 SQL_ASCII */
		SetConfigOption("client_encoding", GetDatabaseEncodingName(),
						PGC_BACKEND, PGC_S_DYNAMIC_DEFAULT);
	}

	/*
	 * 现在应用来自配置文件的值。
	 */
	for (item = head; item; item = item->next)
	{
		char	   *pre_value = NULL;
		int			scres;

		/* 忽略标记为可忽略的项 */
		if (item->ignore)
			continue;

		/* postmaster 的 SIGHUP 情形需要报告更改 */
		if (context == PGC_SIGHUP && applySettings && !IsUnderPostmaster)
		{
			const char *preval = GetConfigOption(item->name, true, false);

			/* 若选项尚不存在或为 NULL,则按空字符串处理 */
			if (!preval)
				preval = "";
			/* 必须复制,否则下文会有悬空指针 */
			pre_value = pstrdup(preval);
		}

		scres = set_config_option(item->name, item->value,
								  context, PGC_S_FILE,
								  GUC_ACTION_SET, applySettings, 0, false);
		if (scres > 0)
		{
			/* 变量已更新,故适当时记录更改 */
			if (pre_value)
			{
				const char *post_value = GetConfigOption(item->name, true, false);

				if (!post_value)
					post_value = "";
				if (strcmp(pre_value, post_value) != 0)
					ereport(elevel,
							(errmsg("parameter \"%s\" changed to \"%s\"",
									item->name, item->value)));
			}
			item->applied = true;
		}
		else if (scres == 0)
		{
			error = true;
			item->errmsg = pstrdup("setting could not be applied");
			ConfFileWithError = item->filename;
		}
		else
		{
			/* 无错误,但变量的生效值未改变 */
			item->applied = true;
		}

		/*
		 * 除非发生了错误,否则我们应当更新来源位置,因为
		 * 即使生效值未变,重置值也可能已经变化。
		 * (在 postmaster 中不会有差异,但在后端中确实有
		 * 影响。)
		 */
		if (scres != 0 && applySettings)
			set_config_sourcefile(item->name, item->filename,
								  item->sourceline);

		if (pre_value)
			pfree(pre_value);
	}

	/* 记住上次成功加载配置文件的时间。 */
	if (applySettings)
		PgReloadTime = GetCurrentTimestamp();

bail_out:
	if (error && applySettings)
	{
		/* postmaster 启动期间错误即致命 */
		if (context == PGC_POSTMASTER)
			ereport(ERROR,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors",
							ConfFileWithError)));
		else if (applying)
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors; unaffected changes were applied",
							ConfFileWithError)));
		else
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("configuration file \"%s\" contains errors; no changes were applied",
							ConfFileWithError)));
	}

	/* 无论成功与否,都返回收集到的数据列表 */
	return head;
}


/*
 * 一些与 GUC 相关的内存分配基础设施
 *
 * 这些函数总体上仿照 libc 的 malloc/realloc 等,
 * 但任何 OOM 问题都以指定的 elevel 报告。
 * (因此,仅当其低于 ERROR 时才返回。)
 */
void *
guc_malloc(int elevel, size_t size)
{
	void	   *data;

	data = MemoryContextAllocExtended(GUCMemoryContext, size,
									  MCXT_ALLOC_NO_OOM);
	if (unlikely(data == NULL))
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}

void *
guc_realloc(int elevel, void *old, size_t size)
{
	void	   *data;

	if (old != NULL)
	{
		/* 便于捕获 malloc GUC 数据的旧代码。 */
		Assert(GetMemoryChunkContext(old) == GUCMemoryContext);
		data = repalloc_extended(old, size,
								 MCXT_ALLOC_NO_OOM);
	}
	else
	{
		/* 类似 realloc(3);repalloc() 不然,允许 old==NULL。 */
		data = MemoryContextAllocExtended(GUCMemoryContext, size,
										  MCXT_ALLOC_NO_OOM);
	}
	if (unlikely(data == NULL))
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	return data;
}

char *
guc_strdup(int elevel, const char *src)
{
	char	   *data;
	size_t		len = strlen(src) + 1;

	data = guc_malloc(elevel, len);
	if (likely(data != NULL))
		memcpy(data, src, len);
	return data;
}

void
guc_free(void *ptr)
{
	/*
	 * 历史上,与 GUC 相关的代码严重依赖于执行
	 * free(NULL),因此即使 pfree() 不允许我们也放行。
	 */
	if (ptr != NULL)
	{
		/* 便于捕获 malloc GUC 数据的旧代码。 */
		Assert(GetMemoryChunkContext(ptr) == GUCMemoryContext);
		pfree(ptr);
	}
}


/*
 * 检测 strval 是否在某个 GUC 字符串项中被引用
 */
static bool
string_field_used(struct config_string *conf, char *strval)
{
	GucStack   *stack;

	if (strval == *(conf->variable) ||
		strval == conf->reset_val ||
		strval == conf->boot_val)
		return true;
	for (stack = conf->gen.stack; stack; stack = stack->prev)
	{
		if (strval == stack->prior.val.stringval ||
			strval == stack->masked.val.stringval)
			return true;
	}
	return false;
}

/*
 * 支持对字符串 GUC 项的字段赋值。如果先前的值没有
 * 被该项中任何其他地方引用(包括栈中状态),则释
 * 放。
 */
static void
set_string_field(struct config_string *conf, char **field, char *newval)
{
	char	   *oldval = *field;

	/* 执行赋值 */
	*field = newval;

	/* 若旧值非 NULL 且不再被引用,则释放它 */
	if (oldval && !string_field_used(conf, oldval))
		guc_free(oldval);
}

/*
 * 检测 "extra" 结构是否在某个 GUC 项中被引用
 */
static bool
extra_field_used(struct config_generic *gconf, void *extra)
{
	GucStack   *stack;

	if (extra == gconf->extra)
		return true;
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			if (extra == ((struct config_bool *) gconf)->reset_extra)
				return true;
			break;
		case PGC_INT:
			if (extra == ((struct config_int *) gconf)->reset_extra)
				return true;
			break;
		case PGC_REAL:
			if (extra == ((struct config_real *) gconf)->reset_extra)
				return true;
			break;
		case PGC_STRING:
			if (extra == ((struct config_string *) gconf)->reset_extra)
				return true;
			break;
		case PGC_ENUM:
			if (extra == ((struct config_enum *) gconf)->reset_extra)
				return true;
			break;
	}
	for (stack = gconf->stack; stack; stack = stack->prev)
	{
		if (extra == stack->prior.extra ||
			extra == stack->masked.extra)
			return true;
	}

	return false;
}

/*
 * 支持对 GUC 项的 "extra" 字段赋值。如果先前的值没
 * 被该项中任何其他地方引用(包括栈中状态),则释
 * 放。
 */
static void
set_extra_field(struct config_generic *gconf, void **field, void *newval)
{
	void	   *oldval = *field;

	/* 执行赋值 */
	*field = newval;

	/* 若旧值非 NULL 且不再被引用,则释放它 */
	if (oldval && !extra_field_used(gconf, oldval))
		guc_free(oldval);
}

/*
 * 支持把变量的生效值复制到一个栈表项中。
 * 与生效值关联的 "extra" 字段也一并复制。
 *
 * NB:务必把新栈表项的 stringval 与 extra 字段
 * 使用前初始化为 NULL,否则会试图对它们调用 guc_free()。
 */
static void
set_stack_value(struct config_generic *gconf, config_var_value *val)
{
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			val->val.boolval =
				*((struct config_bool *) gconf)->variable;
			break;
		case PGC_INT:
			val->val.intval =
				*((struct config_int *) gconf)->variable;
			break;
		case PGC_REAL:
			val->val.realval =
				*((struct config_real *) gconf)->variable;
			break;
		case PGC_STRING:
			set_string_field((struct config_string *) gconf,
							 &(val->val.stringval),
							 *((struct config_string *) gconf)->variable);
			break;
		case PGC_ENUM:
			val->val.enumval =
				*((struct config_enum *) gconf)->variable;
			break;
	}
	set_extra_field(gconf, &(val->extra), gconf->extra);
}

/*
 * 支持丢弃栈表项中不再需要的值。
 * 与栈表项关联的 "extra" 字段也一并清除。
 */
static void
discard_stack_value(struct config_generic *gconf, config_var_value *val)
{
	switch (gconf->vartype)
	{
		case PGC_BOOL:
		case PGC_INT:
		case PGC_REAL:
		case PGC_ENUM:
			/* 无需做任何事 */
			break;
		case PGC_STRING:
			set_string_field((struct config_string *) gconf,
							 &(val->val.stringval),
							 NULL);
			break;
	}
	set_extra_field(gconf, &(val->extra), NULL);
}


/*
 * 取得 palloc'd 的已排序 GUC 指针数组
 *
 * 数组长度返回到 *num_vars 中。
 */
struct config_generic **
get_guc_variables(int *num_vars)
{
	struct config_generic **result;
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;
	int			i;

	*num_vars = hash_get_num_entries(guc_hashtab);
	result = palloc(sizeof(struct config_generic *) * *num_vars);

	/* 从哈希表中抽取指针 */
	i = 0;
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
		result[i++] = hentry->gucvar;
	Assert(i == *num_vars);

	/* 名称排序 */
	qsort(result, *num_vars,
		  sizeof(struct config_generic *), guc_var_compare);

	return result;
}


/*
 * 构建那份 GUC 哈希表。把它单独拆出来,是为了让
 * help_config.c 能避开 InitializeGUCOptions 整体运行
 * 而抽取全部变量,别无他用。
 */
void
build_guc_variables(void)
{
	int			size_vars;
	int			num_vars = 0;
	HASHCTL		hash_ctl;
	GUCHashEntry *hentry;
	bool		found;
	int			i;

	/*
	 * 创建容纳所有 GUC 相关数据的内存上下文。
	 */
	Assert(GUCMemoryContext == NULL);
	GUCMemoryContext = AllocSetContextCreate(TopMemoryContext,
											 "GUCMemoryContext",
											 ALLOCSET_DEFAULT_SIZES);

	/*
	 * 统计所有内建变量,并正确设置它们的 vartype。
	 */
	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
	{
		struct config_bool *conf = &ConfigureNamesBool[i];

		/* 与其要求手工填写 vartype,不如这样做: */
		conf->gen.vartype = PGC_BOOL;
		num_vars++;
	}

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
	{
		struct config_int *conf = &ConfigureNamesInt[i];

		conf->gen.vartype = PGC_INT;
		num_vars++;
	}

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
	{
		struct config_real *conf = &ConfigureNamesReal[i];

		conf->gen.vartype = PGC_REAL;
		num_vars++;
	}

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
	{
		struct config_string *conf = &ConfigureNamesString[i];

		conf->gen.vartype = PGC_STRING;
		num_vars++;
	}

	for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
	{
		struct config_enum *conf = &ConfigureNamesEnum[i];

		conf->gen.vartype = PGC_ENUM;
		num_vars++;
	}

	/*
	 * 创建留 20% 余量的哈希表
	 */
	size_vars = num_vars + num_vars / 4;

	hash_ctl.keysize = sizeof(char *);
	hash_ctl.entrysize = sizeof(GUCHashEntry);
	hash_ctl.hash = guc_name_hash;
	hash_ctl.match = guc_name_match;
	hash_ctl.hcxt = GUCMemoryContext;
	guc_hashtab = hash_create("GUC hash table",
							  size_vars,
							  &hash_ctl,
							  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	for (i = 0; ConfigureNamesBool[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesBool[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesInt[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesInt[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesReal[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesReal[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesString[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesString[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	for (i = 0; ConfigureNamesEnum[i].gen.name; i++)
	{
		struct config_generic *gucvar = &ConfigureNamesEnum[i].gen;

		hentry = (GUCHashEntry *) hash_search(guc_hashtab,
											  &gucvar->name,
											  HASH_ENTER,
											  &found);
		Assert(!found);
		hentry->gucvar = gucvar;
	}

	Assert(num_vars == hash_get_num_entries(guc_hashtab));
}

/*
 * 把新 GUC 变量加入已知变量哈希表。必要时
 * 会扩展该哈希表。
 */
static bool
add_guc_variable(struct config_generic *var, int elevel)
{
	GUCHashEntry *hentry;
	bool		found;

	hentry = (GUCHashEntry *) hash_search(guc_hashtab,
										  &var->name,
										  HASH_ENTER_NULL,
										  &found);
	if (unlikely(hentry == NULL))
	{
		ereport(elevel,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
		return false;			/* 内存不足 */
	}
	Assert(!found);
	hentry->gucvar = var;
	return true;
}

/*
 * 判定一个拟用的自定义变量名是否被允许。
 *
 * 必须是两个或更多由点号分隔的标识符,标识符的
 * 判定规则须与 scan.l 一致。(若改动此规则,errdetail
 * 的调整请见 find_option()。)
 */
static bool
valid_custom_variable_name(const char *name)
{
	bool		saw_sep = false;
	bool		name_start = true;

	for (const char *p = name; *p; p++)
	{
		if (*p == GUC_QUALIFIER_SEPARATOR)
		{
			if (name_start)
				return false;	/* 名字为空 */
			saw_sep = true;
			name_start = true;
		}
		else if (strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						"abcdefghijklmnopqrstuvwxyz_", *p) != NULL ||
				 IS_HIGHBIT_SET(*p))
		{
			/* 首字符或非首字符皆可 */
			name_start = false;
		}
		else if (!name_start && strchr("0123456789$", *p) != NULL)
			 /* 作为非首字符也可以 */ ;
		else
			return false;
	}
	if (name_start)
		return false;			/* 名字为空 */
	/* 找到至少一个分隔符即可 */
	return saw_sep;
}

/*
 * 创建并加入自定义变量名的占位符变量。
 */
static struct config_generic *
add_placeholder_variable(const char *name, int elevel)
{
	size_t		sz = sizeof(struct config_string) + sizeof(char *);
	struct config_string *var;
	struct config_generic *gen;

	var = (struct config_string *) guc_malloc(elevel, sz);
	if (var == NULL)
		return NULL;
	memset(var, 0, sz);
	gen = &var->gen;

	gen->name = guc_strdup(elevel, name);
	if (gen->name == NULL)
	{
		guc_free(var);
		return NULL;
	}

	gen->context = PGC_USERSET;
	gen->group = CUSTOM_OPTIONS;
	gen->short_desc = "GUC placeholder variable";
	gen->flags = GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE | GUC_CUSTOM_PLACEHOLDER;
	gen->vartype = PGC_STRING;

	/*
	 * char* 分配在结构末尾,因为没有静态处可指向。
	 * 注意:当前值以及 boot 值与 reset 值一开始都
	 * 为 NULL。
	 */
	var->variable = (char **) (var + 1);

	if (!add_guc_variable((struct config_generic *) var, elevel))
	{
		guc_free(unconstify(char *, gen->name));
		guc_free(var);
		return NULL;
	}

	return gen;
}

/*
 * 查找名为 "name" 的选项。如果存在,返回指向其
 * 记录的指针。否则,若 create_placeholders 为 true
 * 且 name 看起来是有效的自定义变量名,我们会创建
 * 并返回占位符记录。否则,若 skip_errors 为 true,
 * 则对无法识别或无效的名字静默返回 NULL。否则,
 * 错误以 elevel 级别报告(低于 ERROR 则返回 NULL)。
 *
 * 注意:内部错误(如内存耗尽)无论 skip_errors
 * 取值如何,都会报告并返回 NULL。因此,当 elevel
 * 低于 ERROR 时调用方必须处理 NULL,但无需再发
 * 出额外的错误消息。(实践中,内部错误只会在
 * create_placeholders 为 true 时发生,因此传入 false
 * 的调用方无需为此多虑。)
 */
struct config_generic *
find_option(const char *name, bool create_placeholders, bool skip_errors,
			int elevel)
{
	GUCHashEntry *hentry;
	int			i;

	Assert(name);

	/* 用哈希表查找它。 */
	hentry = (GUCHashEntry *) hash_search(guc_hashtab,
										  &name,
										  HASH_FIND,
										  NULL);
	if (hentry)
		return hentry->gucvar;

	/*
	 * 查看该名字是否是某个变量的过时名字。我们假定受
	 * 支持的旧名字集合足够小,暴力搜索是最好的办法。
	 *
	 */
	for (i = 0; map_old_guc_names[i] != NULL; i += 2)
	{
		if (guc_name_compare(name, map_old_guc_names[i]) == 0)
			return find_option(map_old_guc_names[i + 1], false,
							   skip_errors, elevel);
	}

	if (create_placeholders)
	{
		/*
		 * 检查名字是否有效,若有效则加占位符。如果其中
		 * 不含分隔符,就不要假定它想要用作占位符。
		 *
		 */
		const char *sep = strchr(name, GUC_QUALIFIER_SEPARATOR);

		if (sep != NULL)
		{
			size_t		classLen = sep - name;
			ListCell   *lc;

			/* 名字必须在语法上可接受 ... */
			if (!valid_custom_variable_name(name))
			{
				if (!skip_errors)
					ereport(elevel,
							(errcode(ERRCODE_INVALID_NAME),
							 errmsg("invalid configuration parameter name \"%s\"",
									name),
							 errdetail("Custom parameter names must be two or more simple identifiers separated by dots.")));
				return NULL;
			}
			/* ... 且不得匹配任何先前保留的前缀 */
			foreach(lc, reserved_class_prefix)
			{
				const char *rcprefix = lfirst(lc);

				if (strlen(rcprefix) == classLen &&
					strncmp(name, rcprefix, classLen) == 0)
				{
					if (!skip_errors)
						ereport(elevel,
								(errcode(ERRCODE_INVALID_NAME),
								 errmsg("invalid configuration parameter name \"%s\"",
										name),
								 errdetail("\"%s\" is a reserved prefix.",
										   rcprefix)));
					return NULL;
				}
			}
			/* 好,创建它 */
			return add_placeholder_variable(name, elevel);
		}
	}

	/* 未知名 */
	if (!skip_errors)
		ereport(elevel,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("unrecognized configuration parameter \"%s\"",
						name)));
	return NULL;
}


/*
 * 供 qsort 使用的 GUC 指针数组比较器
 */
static int
guc_var_compare(const void *a, const void *b)
{
	const char *namea = **(const char **const *) a;
	const char *nameb = **(const char **const *) b;

	return guc_name_compare(namea, nameb);
}

/*
 * GUC 名字比较的基础比较函数
 */
int
guc_name_compare(const char *namea, const char *nameb)
{
	/*
	 * 在这里使用 strcasecmp() 的诱惑必须抵制,因为哈希
	 * 映射必须在 setlocale() 调用之间保持稳定。因此,
	 * 以仅 ASCII 的简单小写自建一个。
	 */
	while (*namea && *nameb)
	{
		char		cha = *namea++;
		char		chb = *nameb++;

		if (cha >= 'A' && cha <= 'Z')
			cha += 'a' - 'A';
		if (chb >= 'A' && chb <= 'Z')
			chb += 'a' - 'A';
		if (cha != chb)
			return cha - chb;
	}
	if (*namea)
		return 1;				/* a 更长 */
	if (*nameb)
		return -1;				/* b 更长 */
	return 0;
}

/*
 * 与 guc_name_compare 兼容的哈希函数
 */
static uint32
guc_name_hash(const void *key, Size keysize)
{
	uint32		result = 0;
	const char *name = *(const char *const *) key;

	while (*name)
	{
		char		ch = *name++;

		/* 按 guc_name_compare 方式折叠大小写 */
		if (ch >= 'A' && ch <= 'Z')
			ch += 'a' - 'A';

		/* 并入哈希 ... 不太聪明,但没必要聪明 */
		result = pg_rotate_left32(result, 5);
		result ^= (uint32) ch;
	}
	return result;
}

/*
 * guc_hashtab 所用的 dynahash 匹配函数
 */
static int
guc_name_match(const void *key1, const void *key2, Size keysize)
{
	const char *name1 = *(const char *const *) key1;
	const char *name2 = *(const char *const *) key2;

	return guc_name_compare(name1, name2);
}


/*
 * 把 GUC 名字转换为在 pg_parameter_acl 中应使用的形式。
 *
 * 我们需要把表项规范化,例如大小写不应
 * 有影响。此外,我们还应用 map_old_guc_names[] 映射,
 * 以便任何过时名字在存入新 PG 版本时都会被转换。
 * 但请注意,本函数并不验证名字的合法性。
 *
 * 结果为 palloc'd 字符串。
 */
char *
convert_GUC_name_for_parameter_acl(const char *name)
{
	char	   *result;

	/* 用旧 GUC 名映射。*/
	for (int i = 0; map_old_guc_names[i] != NULL; i += 2)
	{
		if (guc_name_compare(name, map_old_guc_names[i]) == 0)
		{
			name = map_old_guc_names[i + 1];
			break;
		}
	}

	/* 按 guc_name_compare() 折叠大小写。 */
	result = pstrdup(name);
	for (char *ptr = result; *ptr != '\0'; ptr++)
	{
		char		ch = *ptr;

		if (ch >= 'A' && ch <= 'Z')
		{
			ch += 'a' - 'A';
			*ptr = ch;
		}
	}

	return result;
}

/*
 * 检查是否应允许为给定的名字创建 pg_parameter_acl
 * 表项。(在规范化之前或之后应用皆可。)
 *
 */
bool
check_GUC_name_for_parameter_acl(const char *name)
{
	/* GUC 存在即可。 */
	if (find_option(name, false, true, DEBUG1) != NULL)
		return true;
	/* 否则,须为有效的自定义 GUC 名。 */
	if (valid_custom_variable_name(name))
		return true;
	return false;
}

/*
 * 负责检查 GUC 各种状态的例程。
 *
 * 它执行两项健全性检查。第一,检查 GUC 的初始
 * 值在声明时与加载时相同,以免查看这些 GUCs 的
 * C 声明的人被不一致的值所迷惑。第二,它检查
 * 不正确的标志组合。
 *
 * 下列校验规则适用于这些值:
 * bool - 可为 false,否则必须与 boot_val 相同
 * int  - 可为 0,否则必须与 boot_val 相同
 * real - 可为 0.0,否则必须与 boot_val 相同
 * string - 可为 NULL,否则必须 strcmp 等于 boot_val
 * enum - 必须与 boot_val 相同
 */
#ifdef USE_ASSERT_CHECKING
static bool
check_GUC_init(struct config_generic *gconf)
{
	/* 对值的检查 */
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;

				if (*conf->variable && !conf->boot_val)
				{
					elog(LOG, "GUC (PGC_BOOL) %s, boot_val=%d, C-var=%d",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				if (*conf->variable != 0 && *conf->variable != conf->boot_val)
				{
					elog(LOG, "GUC (PGC_INT) %s, boot_val=%d, C-var=%d",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;

				if (*conf->variable != 0.0 && *conf->variable != conf->boot_val)
				{
					elog(LOG, "GUC (PGC_REAL) %s, boot_val=%g, C-var=%g",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				if (*conf->variable != NULL &&
					(conf->boot_val == NULL ||
					 strcmp(*conf->variable, conf->boot_val) != 0))
				{
					elog(LOG, "GUC (PGC_STRING) %s, boot_val=%s, C-var=%s",
						 conf->gen.name, conf->boot_val ? conf->boot_val : "<null>", *conf->variable);
					return false;
				}
				break;
			}
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				if (*conf->variable != conf->boot_val)
				{
					elog(LOG, "GUC (PGC_ENUM) %s, boot_val=%d, C-var=%d",
						 conf->gen.name, conf->boot_val, *conf->variable);
					return false;
				}
				break;
			}
	}

	/* 标志组合 */

	/*
	 * GUC_NO_SHOW_ALL 需要 GUC_NOT_IN_SAMPLE,不属于
	 * SHOW ALL 参数不应被 sample 文件隐藏。
	 */
	if ((gconf->flags & GUC_NO_SHOW_ALL) &&
		!(gconf->flags & GUC_NOT_IN_SAMPLE))
	{
		elog(LOG, "GUC %s flags: NO_SHOW_ALL and !NOT_IN_SAMPLE",
			 gconf->name);
		return false;
	}

	return true;
}
#endif

/*
 * 在程序启动期间初始化 GUC 选项。
 *
 * 注意此时还无法读取配置文件,因为我们尚未处理
 * 命令行开关。
 */
void
InitializeGUCOptions(void)
{
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;

	/*
	 * 在 log_line_prefix 可能收到非空设置之前,先确保
	 * 时区处理最低限度可用(参见 elog.c)。
	 */
	pg_timezone_initialize();

	/*
	 * 创建 GUCMemoryContext 并构建所有 GUC 变量的哈希表。
	 */
	build_guc_variables();

	/*
	 * 以编译进来的默认值加载所有变量,并按需初始化
	 * 状态字段。
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		/* 检查初始值与默认值之间的映射 */
		Assert(check_GUC_init(hentry->gucvar));

		InitializeOneGUCOption(hentry->gucvar);
	}

	reporting_enabled = false;

	/*
	 * 防止非交互来源覆盖事务模式的任何企图。
	 *
	 */
	SetConfigOption("transaction_isolation", "read committed",
					PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("transaction_read_only", "no",
					PGC_POSTMASTER, PGC_S_OVERRIDE);
	SetConfigOption("transaction_deferrable", "no",
					PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * 由于历史原因,某些 GUC 参数可以从环境变量接收
	 * 默认值。处理这些设置。
	 */
	InitializeGUCOptionsFromEnvironment();
}

/*
 * 设置任何可能来自服务器环境的 GUC 值。
 *
 * 本函数由 InitializeGUCOptions 调用,也由 ProcessConfigFile
 * 调用,以应对设置已从 postgresql.conf 移除、应当
 * 改从环境取得值的情形。
 * (后者是个权宜之计,或许某天该去掉;若如此,
 * 可把它折回 InitializeGUCOptions。)
 */
static void
InitializeGUCOptionsFromEnvironment(void)
{
	char	   *env;
	long		stack_rlimit;

	env = getenv("PGPORT");
	if (env != NULL)
		SetConfigOption("port", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGDATESTYLE");
	if (env != NULL)
		SetConfigOption("datestyle", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	env = getenv("PGCLIENTENCODING");
	if (env != NULL)
		SetConfigOption("client_encoding", env, PGC_POSTMASTER, PGC_S_ENV_VAR);

	/*
	 * rlimit 并不完全是"环境变量",但行为大致相同。如
	 * 果我们能识别平台的栈深度 rlimit,就把默认栈深度
	 * 设置增加到安全范围内(但至多 2MB)。如果值为
	 * 2MB,则把值的来源报告为 PGC_S_DYNAMIC_DEFAULT;
	 * 若它反映 rlimit 上限,则为 PGC_S_ENV_VAR。
	 */
	stack_rlimit = get_stack_depth_rlimit();
	if (stack_rlimit > 0)
	{
		long		new_limit = (stack_rlimit - STACK_DEPTH_SLOP) / 1024L;

		if (new_limit > 100)
		{
			GucSource	source;
			char		limbuf[16];

			if (new_limit < 2048)
				source = PGC_S_ENV_VAR;
			else
			{
				new_limit = 2048;
				source = PGC_S_DYNAMIC_DEFAULT;
			}
			snprintf(limbuf, sizeof(limbuf), "%ld", new_limit);
			SetConfigOption("max_stack_depth", limbuf,
							PGC_POSTMASTER, source);
		}
	}
}

/*
 * 将一个 GUC 选项变量初始化为编译内默认值。
 *
 * 注意:调用 check_hooks 并非因为认为 boot_val 可能
 * 不通过,而是因为钩子可能希望计算一个 "extra" 结构。
 */
static void
InitializeOneGUCOption(struct config_generic *gconf)
{
	gconf->status = 0;
	gconf->source = PGC_S_DEFAULT;
	gconf->reset_source = PGC_S_DEFAULT;
	gconf->scontext = PGC_INTERNAL;
	gconf->reset_scontext = PGC_INTERNAL;
	gconf->srole = BOOTSTRAP_SUPERUSERID;
	gconf->reset_srole = BOOTSTRAP_SUPERUSERID;
	gconf->stack = NULL;
	gconf->extra = NULL;
	gconf->last_reported = NULL;
	gconf->sourcefile = NULL;
	gconf->sourceline = 0;

	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;
				bool		newval = conf->boot_val;
				void	   *extra = NULL;

				if (!call_bool_check_hook(conf, &newval, &extra,
										  PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, (int) newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;
				int			newval = conf->boot_val;
				void	   *extra = NULL;

				Assert(newval >= conf->min);
				Assert(newval <= conf->max);
				if (!call_int_check_hook(conf, &newval, &extra,
										 PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;
				double		newval = conf->boot_val;
				void	   *extra = NULL;

				Assert(newval >= conf->min);
				Assert(newval <= conf->max);
				if (!call_real_check_hook(conf, &newval, &extra,
										  PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %g",
						 conf->gen.name, newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;
				char	   *newval;
				void	   *extra = NULL;

				/* 非 NULL 的 boot_val 必经 strdup 复制 */
				if (conf->boot_val != NULL)
					newval = guc_strdup(FATAL, conf->boot_val);
				else
					newval = NULL;

				if (!call_string_check_hook(conf, &newval, &extra,
											PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to \"%s\"",
						 conf->gen.name, newval ? newval : "");
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;
				int			newval = conf->boot_val;
				void	   *extra = NULL;

				if (!call_enum_check_hook(conf, &newval, &extra,
										  PGC_S_DEFAULT, LOG))
					elog(FATAL, "failed to initialize %s to %d",
						 conf->gen.name, newval);
				if (conf->assign_hook)
					conf->assign_hook(newval, extra);
				*conf->variable = conf->reset_val = newval;
				conf->gen.extra = conf->reset_extra = extra;
				break;
			}
	}
}

/*
 * 将 GUC 变量从其所在的所有链表中立即移除。
 *
 * 在变量即将被删除或重置的情形下我们会使用它。
 * 这些操作并不常见,所以它稍慢一点也无妨。
 */
static void
RemoveGUCFromLists(struct config_generic *gconf)
{
	if (gconf->source != PGC_S_DEFAULT)
		dlist_delete(&gconf->nondef_link);
	if (gconf->stack != NULL)
		slist_delete(&guc_stack_list, &gconf->stack_link);
	if (gconf->status & GUC_NEEDS_REPORT)
		slist_delete(&guc_report_list, &gconf->report_link);
}


/*
 * 选择要使用的配置文件与数据目录,并对
 * postgresql.conf 做初次读取。
 *
 * 本函数在处理完命令行开关之后调用。
 *		userDoption 是 -D 开关的值(若无则为 NULL)。
 *		progname 仅用于错误消息。
 *
 * 成功时返回 true;失败时向 stderr 打印合适的错误
 * 消息并返回 false。
 */
bool
SelectConfigFiles(const char *userDoption, const char *progname)
{
	char	   *configdir;
	char	   *fname;
	bool		fname_is_malloced;
	struct stat stat_buf;
	struct config_string *data_directory_rec;

	/* configdir 是 -D 选项,否则 $PGDATA */
	if (userDoption)
		configdir = make_absolute_path(userDoption);
	else
		configdir = make_absolute_path(getenv("PGDATA"));

	if (configdir && stat(configdir, &stat_buf) != 0)
	{
		write_stderr("%s: could not access directory \"%s\": %s\n",
					 progname,
					 configdir,
					 strerror(errno));
		if (errno == ENOENT)
			write_stderr("Run initdb or pg_basebackup to initialize a PostgreSQL data directory.\n");
		return false;
	}

	/*
	 * 寻找配置文件:若 config_file 在命令行上指定过
	 * 就使用它,否则使用 configdir/postgresql.conf。无
	 * 论如何都要确保结果是绝对路径,这样未来的后端
	 * 都以同样方式解释它。
	 */
	if (ConfigFileName)
	{
		fname = make_absolute_path(ConfigFileName);
		fname_is_malloced = true;
	}
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(CONFIG_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, CONFIG_FILENAME);
		fname_is_malloced = false;
	}
	else
	{
		write_stderr("%s does not know where to find the server configuration file.\n"
					 "You must specify the --config-file or -D invocation "
					 "option or set the PGDATA environment variable.\n",
					 progname);
		return false;
	}

	/*
	 * 把 ConfigFileName GUC 变量设为最终值,确保它以后
	 * 无法被覆盖。
	 */
	SetConfigOption("config_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);

	if (fname_is_malloced)
		free(fname);
	else
		guc_free(fname);

	/*
	 * 现在第一次读取配置文件。
	 */
	if (stat(ConfigFileName, &stat_buf) != 0)
	{
		write_stderr("%s: could not access the server configuration file \"%s\": %s\n",
					 progname, ConfigFileName, strerror(errno));
		free(configdir);
		return false;
	}

	/*
	 * 第一次读取配置文件。这一次只挑出 data_directory
	 * 参数以确定数据目录,以便下次能读取
	 * PG_AUTOCONF_FILENAME 文件。
	 */
	ProcessConfigFile(PGC_POSTMASTER);

	/*
	 * 如果 data_directory GUC 变量已被设置,就用它作为
	 * DataDir;否则 configdir;不然放弃。
	 *
	 * 注意:SetDataDir 会复制其参数并把它绝对化,所以
	 *不必。
	 */
	data_directory_rec = (struct config_string *)
		find_option("data_directory", false, false, PANIC);
	if (*data_directory_rec->variable)
		SetDataDir(*data_directory_rec->variable);
	else if (configdir)
		SetDataDir(configdir);
	else
	{
		write_stderr("%s does not know where to find the database system data.\n"
					 "This can be specified as \"data_directory\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}

	/*
	 * 把最终的 DataDir 值回写进 data_directory GUC 变量。
	 * (如果你奇怪为什么不干脆把它们做成单个变量,那
	 * 是因为 EXEC_BACKEND 情形需要把 DataDir 特殊地传递
	 * 给子后端。XXX 现在仍如此吗?鉴于我们现在
	 * chdir 到 DataDir,EXEC_BACKEND 不预知 DataDir 也
	 * 能读取配置。)
	 */
	SetConfigOption("data_directory", DataDir, PGC_POSTMASTER, PGC_S_OVERRIDE);

	/*
	 * 现在第二次读取配置文件,让 PG_AUTOCONF_FILENAME
	 * 文件中的设置生效。(这很丑,但因为我们
	 * 必须先确定 DataDir 才能找到 autoconf 文件,其他
	 * 做法似乎更糟。)
	 */
	ProcessConfigFile(PGC_POSTMASTER);

	/*
	 * 如果 timezone_abbreviations 未在配置文件中设置,
	 * 就安装默认值。这样做是因为在 my_exec_path 设置好
	 * 之前无法安全地安装"真实"值,而它可能尚未在
	 * InitializeGUCOptions 运行时发生,所以引导默认值不
	 * 会是所需的默认值。
	 */
	pg_timezone_abbrev_initialize();

	/*
	 * 弄清 pg_hba.conf 在哪里,并确保是绝对路径。
	 */
	if (HbaFileName)
	{
		fname = make_absolute_path(HbaFileName);
		fname_is_malloced = true;
	}
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(HBA_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, HBA_FILENAME);
		fname_is_malloced = false;
	}
	else
	{
		write_stderr("%s does not know where to find the \"hba\" configuration file.\n"
					 "This can be specified as \"hba_file\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}
	SetConfigOption("hba_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);

	if (fname_is_malloced)
		free(fname);
	else
		guc_free(fname);

	/*
	 * pg_ident.conf 同理。
	 */
	if (IdentFileName)
	{
		fname = make_absolute_path(IdentFileName);
		fname_is_malloced = true;
	}
	else if (configdir)
	{
		fname = guc_malloc(FATAL,
						   strlen(configdir) + strlen(IDENT_FILENAME) + 2);
		sprintf(fname, "%s/%s", configdir, IDENT_FILENAME);
		fname_is_malloced = false;
	}
	else
	{
		write_stderr("%s does not know where to find the \"ident\" configuration file.\n"
					 "This can be specified as \"ident_file\" in \"%s\", "
					 "or by the -D invocation option, or by the "
					 "PGDATA environment variable.\n",
					 progname, ConfigFileName);
		return false;
	}
	SetConfigOption("ident_file", fname, PGC_POSTMASTER, PGC_S_OVERRIDE);

	if (fname_is_malloced)
		free(fname);
	else
		guc_free(fname);

	free(configdir);

	return true;
}

/*
 * pg_timezone_abbrev_initialize --- 若尚未设置则设默认值
 *
 * 本函数在初次加载 postgresql.conf 后调用。如果其
 * 中没有 timezone_abbreviations 设置则选默认值。
 * 如果已经安装了非默认值,则什么都不会发生。
 *
 * 本函数也可以从 ProcessConfigFile 调用,以便在其
 * postgresql.conf 条目被移除后确立默认值。
 */
static void
pg_timezone_abbrev_initialize(void)
{
	SetConfigOption("timezone_abbreviations", "Default",
					PGC_POSTMASTER, PGC_S_DYNAMIC_DEFAULT);
}


/*
 * 把所有选项重置为保存的默认值(实现 RESET ALL)
 */
void
ResetAllOptions(void)
{
	dlist_mutable_iter iter;

	/* 只考虑尚未处于 PGC_S_DEFAULT 的 GUCs */
	dlist_foreach_modify(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		/* 不重置不可 SET 的值 */
		if (gconf->context != PGC_SUSET &&
			gconf->context != PGC_USERSET)
			continue;
		/* 因被 RESET ALL 特殊排除而不重置 */
		if (gconf->flags & GUC_NO_RESET_ALL)
			continue;
		/* 未 SET 过则无需重置 */
		if (gconf->source <= PGC_S_OVERRIDE)
			continue;

		/* 保存旧值以支持事务中止 */
		push_old_value(gconf, GUC_ACTION_SET);

		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					set_string_field(conf, conf->variable, conf->reset_val);
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
			case PGC_ENUM:
				{
					struct config_enum *conf = (struct config_enum *) gconf;

					if (conf->assign_hook)
						conf->assign_hook(conf->reset_val,
										  conf->reset_extra);
					*conf->variable = conf->reset_val;
					set_extra_field(&conf->gen, &conf->gen.extra,
									conf->reset_extra);
					break;
				}
		}

		set_guc_source(gconf, gconf->reset_source);
		gconf->scontext = gconf->reset_scontext;
		gconf->srole = gconf->reset_srole;

		if ((gconf->flags & GUC_REPORT) && !(gconf->status & GUC_NEEDS_REPORT))
		{
			gconf->status |= GUC_NEEDS_REPORT;
			slist_push_head(&guc_report_list, &gconf->report_link);
		}
	}
}


/*
 * 对 GUC 变量的 "source" 字段应用更改。
 *
 * 使用本函数而不是直接赋值,以确保变量在
 * guc_nondef_list 的成员关系正确更新。
 */
static void
set_guc_source(struct config_generic *gconf, GucSource newsource)
{
	/* 变更适当时调整 nondef 链表成员身份 */
	if (gconf->source == PGC_S_DEFAULT)
	{
		if (newsource != PGC_S_DEFAULT)
			dlist_push_tail(&guc_nondef_list, &gconf->nondef_link);
	}
	else
	{
		if (newsource == PGC_S_DEFAULT)
			dlist_delete(&gconf->nondef_link);
	}
	/* 现更新 source 字段 */
	gconf->source = newsource;
}


/*
 * push_old_value
 *		在对 GUC 变量做事务性赋值时压入先前的状态。
 */
static void
push_old_value(struct config_generic *gconf, GucAction action)
{
	GucStack   *stack;

	/* 若不在嵌套层级内,则不做任何事 */
	if (GUCNestLevel == 0)
		return;

	/* 是否已有当前嵌套层级的栈表项? */
	stack = gconf->stack;
	if (stack && stack->nest_level >= GUCNestLevel)
	{
		/* 有,必要时调整其状态 */
		Assert(stack->nest_level == GUCNestLevel);
		switch (action)
		{
			case GUC_ACTION_SET:
				/* SET 覆盖同层级上任何先前的动作 */
				if (stack->state == GUC_SET_LOCAL)
				{
					/* 须丢弃旧的被遮蔽值 */
					discard_stack_value(gconf, &stack->masked);
				}
				stack->state = GUC_SET;
				break;
			case GUC_ACTION_LOCAL:
				if (stack->state == GUC_SET)
				{
					/* SET 之后跟 SET LOCAL,记住 SET 的值 */
					stack->masked_scontext = gconf->scontext;
					stack->masked_srole = gconf->srole;
					set_stack_value(gconf, &stack->masked);
					stack->state = GUC_SET_LOCAL;
				}
				/* 在所有其他情况下,栈表项不变 */
				break;
			case GUC_ACTION_SAVE:
				/* 之前只可能有过同一变量的 SAVE */
				Assert(stack->state == GUC_SAVE);
				break;
		}
		return;
	}

	/*
	 * 压入新栈表项
	 *
	 * 为简单起见,栈表项都存于 TopTransactionContext 中。
	 */
	stack = (GucStack *) MemoryContextAllocZero(TopTransactionContext,
												sizeof(GucStack));

	stack->prev = gconf->stack;
	stack->nest_level = GUCNestLevel;
	switch (action)
	{
		case GUC_ACTION_SET:
			stack->state = GUC_SET;
			break;
		case GUC_ACTION_LOCAL:
			stack->state = GUC_LOCAL;
			break;
		case GUC_ACTION_SAVE:
			stack->state = GUC_SAVE;
			break;
	}
	stack->source = gconf->source;
	stack->scontext = gconf->scontext;
	stack->srole = gconf->srole;
	set_stack_value(gconf, &stack->prior);

	if (gconf->stack == NULL)
		slist_push_head(&guc_stack_list, &gconf->stack_link);
	gconf->stack = stack;
}


/*
 * 在主事务开始时做 GUC 处理。
 */
void
AtStart_GUC(void)
{
	/*
	 * 事务之间嵌套层级应当为 0;如果不是,说明有人没
	 * 有调用 AtEOXact_GUC,或者调用时传了错误的 nestLevel。
	 * 我们发出警告,但不做其他清理工作。
	 */
	if (GUCNestLevel != 0)
		elog(WARNING, "GUC nest level = %d at transaction start",
			 GUCNestLevel);
	GUCNestLevel = 1;
}

/*
 * 为 GUC 值进入一个新的嵌套层级。本函数在子事务开始
 * 时、在进入带有 proconfig 设置的函数时,以及在其
 * 他一些想要临时设置 GUC 变量的地方被调用。
 * 注意:此处绝不能冒险出错,否则子事务开始会不愉快。
 */
int
NewGUCNestLevel(void)
{
	return ++GUCNestLevel;
}

/*
 * 在事务或子事务提交或中止时、在退出带有 proconfig
 * 设置的函数时,或撤销对某些 GUC 变量的临时赋值
 * 时执行。(因此这个名字有点名不副实;或许它应当
 * 叫 ExitGUCNestLevel 之类的名字。)
 * 在中止时,我们会丢弃所有在嵌套层级 >= nestLevel
 * 时应用的 GUC 设置。nestLevel == 1 对应主事务。
 */
void
AtEOXact_GUC(bool isCommit, int nestLevel)
{
	slist_mutable_iter iter;

	/*
	 * 注意:在中止期间,如果事务开始过程中 AtStart_GUC
	 * 被调用之前发生失败,则可能以 GUCNestLevel 等于
	 * nestLevel-1 抵达。
	 */
	Assert(nestLevel > 0 &&
		   (nestLevel <= GUCNestLevel ||
			(nestLevel == GUCNestLevel + 1 && !isCommit)));

	/* 只处理具有非空栈的 GUCs */
	slist_foreach_modify(iter, &guc_stack_list)
	{
		struct config_generic *gconf = slist_container(struct config_generic,
													   stack_link, iter.cur);
		GucStack   *stack;

		/*
		 * 处理并弹出该嵌套层级内的每个栈表项。为了简化
		 * fmgr_security_definer() 以及其他使用 GUC_ACTION_SAVE
		 * 的地方,允许使用局部嵌套层级的代码以失败退
		 * 出,并在外围事务或子事务中止时恢复;因此可能
		 * 有不止一个栈表项需要弹出。
		 */
		while ((stack = gconf->stack) != NULL &&
			   stack->nest_level >= nestLevel)
		{
			GucStack   *prev = stack->prev;
			bool		restorePrior = false;
			bool		restoreMasked = false;
			bool		changed;

			/*
			 * 在接下来这段中,如果既不设置 restorePrior
			 * 也不设置 restoreMasked,则必须"丢弃"栈表项中
			 * 任何多余字段以免内存泄漏。若设置了这两
			 * 个标志之一,未用的字段会在恢复之后被清理。
			 */
			if (!isCommit)		/* 中止时总恢复先前值 */
				restorePrior = true;
			else if (stack->state == GUC_SAVE)
				restorePrior = true;
			else if (stack->nest_level == 1)
			{
				/* 事务提交 */
				if (stack->state == GUC_SET_LOCAL)
					restoreMasked = true;
				else if (stack->state == GUC_SET)
				{
					/* 我们保留当前生效值 */
					discard_stack_value(gconf, &stack->prior);
				}
				else			/* 必为 GUC_LOCAL */
					restorePrior = true;
			}
			else if (prev == NULL ||
					 prev->nest_level < stack->nest_level - 1)
			{
				/* 递减表项的层级且不弹出它 */
				stack->nest_level--;
				continue;
			}
			else
			{
				/*
				 * 须把此栈表项合并进 prev。讨论参见
				 * README 中的说明。
				 */
				switch (stack->state)
				{
					case GUC_SAVE:
						Assert(false);	/* 不会到此 */
						break;

					case GUC_SET:
						/* 下一层级总是变为 SET */
						discard_stack_value(gconf, &stack->prior);
						if (prev->state == GUC_SET_LOCAL)
							discard_stack_value(gconf, &prev->masked);
						prev->state = GUC_SET;
						break;

					case GUC_LOCAL:
						if (prev->state == GUC_SET)
						{
							/* LOCAL 向下迁移 */
							prev->masked_scontext = stack->scontext;
							prev->masked_srole = stack->srole;
							prev->masked = stack->prior;
							prev->state = GUC_SET_LOCAL;
						}
						else
						{
							/* 否则直接忘掉这一栈层级 */
							discard_stack_value(gconf, &stack->prior);
						}
						break;

					case GUC_SET_LOCAL:
						/* 该层级上先前的状态不再需要 */
						discard_stack_value(gconf, &stack->prior);
						/* 复制被遮蔽状态 */
						prev->masked_scontext = stack->masked_scontext;
						prev->masked_srole = stack->masked_srole;
						if (prev->state == GUC_SET_LOCAL)
							discard_stack_value(gconf, &prev->masked);
						prev->masked = stack->masked;
						prev->state = GUC_SET_LOCAL;
						break;
				}
			}

			changed = false;

			if (restorePrior || restoreMasked)
			{
				/* 对栈中的值执行适当的恢复 */
				config_var_value newvalue;
				GucSource	newsource;
				GucContext	newscontext;
				Oid			newsrole;

				if (restoreMasked)
				{
					newvalue = stack->masked;
					newsource = PGC_S_SESSION;
					newscontext = stack->masked_scontext;
					newsrole = stack->masked_srole;
				}
				else
				{
					newvalue = stack->prior;
					newsource = stack->source;
					newscontext = stack->scontext;
					newsrole = stack->srole;
				}

				switch (gconf->vartype)
				{
					case PGC_BOOL:
						{
							struct config_bool *conf = (struct config_bool *) gconf;
							bool		newval = newvalue.val.boolval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
					case PGC_INT:
						{
							struct config_int *conf = (struct config_int *) gconf;
							int			newval = newvalue.val.intval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
					case PGC_REAL:
						{
							struct config_real *conf = (struct config_real *) gconf;
							double		newval = newvalue.val.realval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
					case PGC_STRING:
						{
							struct config_string *conf = (struct config_string *) gconf;
							char	   *newval = newvalue.val.stringval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								set_string_field(conf, conf->variable, newval);
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}

							/*
							 * 栈值若不再使用则释放。本可用
							 * discard_stack_value(),但类型特定代
							 * 码反正都要写,不如把它内联。
							 *
							 */
							set_string_field(conf, &stack->prior.val.stringval, NULL);
							set_string_field(conf, &stack->masked.val.stringval, NULL);
							break;
						}
					case PGC_ENUM:
						{
							struct config_enum *conf = (struct config_enum *) gconf;
							int			newval = newvalue.val.enumval;
							void	   *newextra = newvalue.extra;

							if (*conf->variable != newval ||
								conf->gen.extra != newextra)
							{
								if (conf->assign_hook)
									conf->assign_hook(newval, newextra);
								*conf->variable = newval;
								set_extra_field(&conf->gen, &conf->gen.extra,
												newextra);
								changed = true;
							}
							break;
						}
				}

				/*
				 * 如果栈中的 extra 值不再使用则释放。
				 */
				set_extra_field(gconf, &(stack->prior.extra), NULL);
				set_extra_field(gconf, &(stack->masked.extra), NULL);

				/* 并恢复来源信息 */
				set_guc_source(gconf, newsource);
				gconf->scontext = newscontext;
				gconf->srole = newsrole;
			}

			/*
			 * 弹出该 GUC 的状态栈;若为空,同时把它移出
			 * guc_stack_list。
			 */
			gconf->stack = prev;
			if (prev == NULL)
				slist_delete_current(&iter);
			pfree(stack);

			/* 如果更改了它则报告新值 */
			if (changed && (gconf->flags & GUC_REPORT) &&
				!(gconf->status & GUC_NEEDS_REPORT))
			{
				gconf->status |= GUC_NEEDS_REPORT;
				slist_push_head(&guc_report_list, &gconf->report_link);
			}
		}						/* 弹出栈循环结束 */
	}

	/* 更新嵌套层级 */
	GUCNestLevel = nestLevel - 1;
}


/*
 * 启动对标记为 GUC_REPORT 的变量更改的自动报告。
 * 本函数在后端启动完成时执行。
 */
void
BeginReportingGUCOptions(void)
{
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;

	/*
	 * 除非在与交互式前端通信,否则什么也不做。
	 */
	if (whereToSendOutput != DestRemote)
		return;

	reporting_enabled = true;

	/*
	 * 针对 in_hot_standby 的权宜做法:如合适则把 GUC 值设
	 * 为 true。此处有点丑,但没有更好的选择了。
	 *
	 * (等到我们实际发送它时,这个值可能已过时,那种情
	 * 况下下一次 ReportChangedGUCOptions 调用会发送重复报
	 * 告。)
	 */
	if (RecoveryInProgress())
		SetConfigOption("in_hot_standby", "true",
						PGC_INTERNAL, PGC_S_OVERRIDE);

	/* 发送相关变量的初始值 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *conf = hentry->gucvar;

		if (conf->flags & GUC_REPORT)
			ReportGUCOption(conf);
	}
}

/*
 * ReportChangedGUCOptions:报告近期更改的 GUC_REPORT 变量
 *
 * 本函数在我们等待新的客户查询之前调用。
 *
 * 以这种方式处理,我们能确保 ParameterStatus 消息对每
 * 个变量每次查询至多一次,即使该变量在查询内
 * 改变多次的情形也完全可能,例如在使用函数 SET 子
 * 句这类特性时。函数 SET 子句还容易导致值在查询
 * 内改变但最终回归其主流值;ReportGUCOption 负责
 * 避免此时的冗余报告。
 *
 */
void
ReportChangedGUCOptions(void)
{
	slist_mutable_iter iter;

	/* 若尚未启用则快速退出 */
	if (!reporting_enabled)
		return;

	/*
	 * 由于 in_hot_standby 实际上不会因正常 GUC 动作而改
	 * 变,我们需要用一个权宜手段来检查是否需要向客户
	 * 端报告新值。为了速度,我们依赖这一假设:它
	 * 绝不会由 false 变 true。
	 */
	if (in_hot_standby_guc && !RecoveryInProgress())
		SetConfigOption("in_hot_standby", "false",
						PGC_INTERNAL, PGC_S_OVERRIDE);

	/* 发送相关变量的新值 */
	slist_foreach_modify(iter, &guc_report_list)
	{
		struct config_generic *conf = slist_container(struct config_generic,
													  report_link, iter.cur);

		Assert((conf->flags & GUC_REPORT) && (conf->status & GUC_NEEDS_REPORT));
		ReportGUCOption(conf);
		conf->status &= ~GUC_NEEDS_REPORT;
		slist_delete_current(&iter);
	}
}

/*
 * ReportGUCOption:如合适,则向前端发送选项值
 *
 * 如果该值与上次发送的值相同,则无需发送。
 *
 */
static void
ReportGUCOption(struct config_generic *record)
{
	char	   *val = ShowGUCOption(record, false);

	if (record->last_reported == NULL ||
		strcmp(val, record->last_reported) != 0)
	{
		StringInfoData msgbuf;

		pq_beginmessage(&msgbuf, 'S');
		pq_sendstring(&msgbuf, record->name);
		pq_sendstring(&msgbuf, val);
		pq_endmessage(&msgbuf);

		/*
		 * 需要长寿命副本。若 guc_strdup() 因 OOM 而失败,
		 * 我们会把 last_reported 设为 NULL,从而可能稍后
		 * 作出重复报告。
		 */
		guc_free(record->last_reported);
		record->last_reported = guc_strdup(LOG, val);
	}

	pfree(val);
}

/*
 * 把值从一种人类友好单位("kB"、"min" 等)换算为
 * 给定的基础单位。value 与 unit 是输入的值与单位
 * (单位字符串中可以有尾随空格)。结果
 * 存入 *base_value。
 * 调用者负责按需将转换后的值取整,并检查是
 * 否越界。
 *
 * 成功时返回 true,输入单位无法识别时返回 false。
 */
static bool
convert_to_base_unit(double value, const char *unit,
					 int base_unit, double *base_value)
{
	char		unitstr[MAX_UNIT_LEN + 1];
	int			unitlen;
	const unit_conversion *table;
	int			i;

	/* 抽出单位字符串以与表项比较 */
	unitlen = 0;
	while (*unit != '\0' && !isspace((unsigned char) *unit) &&
		   unitlen < MAX_UNIT_LEN)
		unitstr[unitlen++] = *(unit++);
	unitstr[unitlen] = '\0';
	/* 允许单位后有空白 */
	while (isspace((unsigned char) *unit))
		unit++;
	if (*unit != '\0')
		return false;			/* 单位过长或其后有垃圾 */

	/* 现在搜索相应的表 */
	if (base_unit & GUC_UNIT_MEMORY)
		table = memory_unit_conversion_table;
	else
		table = time_unit_conversion_table;

	for (i = 0; *table[i].unit; i++)
	{
		if (base_unit == table[i].base_unit &&
			strcmp(unitstr, table[i].unit) == 0)
		{
			double		cvalue = value * table[i].multiplier;

			/*
			 * 若用户给出 "30.1GB" 这类小数值,则把它舍入为
			 * 下一更小单位的最近倍数(如果存在的话)。
			 *
			 */
			if (*table[i + 1].unit &&
				base_unit == table[i + 1].base_unit)
				cvalue = rint(cvalue / table[i + 1].multiplier) *
					table[i + 1].multiplier;

			*base_value = cvalue;
			return true;
		}
	}
	return false;
}

/*
 * 把某基础单位的整数值换算为人类友好单位。
 *
 * 输出单位的选择标准是:它是能无损表示该值的最大
 * 单位。例如,若基础单位为 GUC_UNIT_KB,则 1024 会
 * 被换算为 1 MB,而 1025 表示为 1025 kB。
 */
static void
convert_int_from_base_unit(int64 base_value, int base_unit,
						   int64 *value, const char **unit)
{
	const unit_conversion *table;
	int			i;

	*unit = NULL;

	if (base_unit & GUC_UNIT_MEMORY)
		table = memory_unit_conversion_table;
	else
		table = time_unit_conversion_table;

	for (i = 0; *table[i].unit; i++)
	{
		if (base_unit == table[i].base_unit)
		{
			/*
			 * 接受第一个整除该值的换算。假定每个基础单
			 * 位的换算是按从最大单位到最小单位排序的!
			 *
			 */
			if (table[i].multiplier <= 1.0 ||
				base_value % (int64) table[i].multiplier == 0)
			{
				*value = (int64) rint(base_value / table[i].multiplier);
				*unit = table[i].unit;
				break;
			}
		}
	}

	Assert(*unit != NULL);
}

/*
 * 把某基础单位的浮点值换算为人类友好单位。
 *
 * 与上面相同,但计算方式稍异,且可能找不到任何
 * 精确的除数。
 */
static void
convert_real_from_base_unit(double base_value, int base_unit,
							double *value, const char **unit)
{
	const unit_conversion *table;
	int			i;

	*unit = NULL;

	if (base_unit & GUC_UNIT_MEMORY)
		table = memory_unit_conversion_table;
	else
		table = time_unit_conversion_table;

	for (i = 0; *table[i].unit; i++)
	{
		if (base_unit == table[i].base_unit)
		{
			/*
			 * 接受第一个整除该值的换算;若无,则用最小
			 * (最后)一个目标单位。
			 *
			 * 真正关心的是用 "%g" 的 snprintf 是否会把值
			 * 打印成整数,所以 "*value == rint(*value)" 这一直
			 * 观测试过严;舍入误差可能使我们选小得
			 * 离谱的小单位。折衷方案:接受相差在
			 * 1e-8 以内的除数。
			 */
			*value = base_value / table[i].multiplier;
			*unit = table[i].unit;
			if (*value > 0 &&
				fabs((rint(*value) / *value) - 1.0) <= 1e-8)
				break;
		}
	}

	Assert(*unit != NULL);
}

/*
 * 根据给定标志返回 GUC 基础单位的名称(如 "ms")。
 * 若 GUC 无单位则返回 NULL。
 */
const char *
get_config_unit_name(int flags)
{
	switch (flags & GUC_UNIT)
	{
		case 0:
			return NULL;		/* GUC 无单位 */
		case GUC_UNIT_BYTE:
			return "B";
		case GUC_UNIT_KB:
			return "kB";
		case GUC_UNIT_MB:
			return "MB";
		case GUC_UNIT_BLOCKS:
			{
				static char bbuf[8];

				/* 首次经过时初始化 */
				if (bbuf[0] == '\0')
					snprintf(bbuf, sizeof(bbuf), "%dkB", BLCKSZ / 1024);
				return bbuf;
			}
		case GUC_UNIT_XBLOCKS:
			{
				static char xbuf[8];

				/* 首次经过时初始化 */
				if (xbuf[0] == '\0')
					snprintf(xbuf, sizeof(xbuf), "%dkB", XLOG_BLCKSZ / 1024);
				return xbuf;
			}
		case GUC_UNIT_MS:
			return "ms";
		case GUC_UNIT_S:
			return "s";
		case GUC_UNIT_MIN:
			return "min";
		default:
			elog(ERROR, "unrecognized GUC units value: %d",
				 flags & GUC_UNIT);
			return NULL;
	}
}


/*
 * 尝试把 value 解析为整数。可接受格式有常见
 * 十进制、八进制或十六进制格式,以及浮点数格式(它
 * 会在单位换算后被取整为整数)。可选地,如果
 * 可选地,若 "flags" 表明允许单位,值后可跟单位名。
 *
 *
 * 解析正常则返回 true,否则 false。
 * 如果正常且 result 非 NULL,则把值存入 *result。
 * 如果不正常且 hintmsg 非 NULL,则把 *hintmsg 设为
 * 合适的 HINT 消息或 NULL。
 */
bool
parse_int(const char *value, int *result, int flags, const char **hintmsg)
{
	/*
	 * 我们在此假定 double 足够宽,能以足够的精度表示
	 * 任何整数值。
	 */
	double		val;
	char	   *endptr;

	/* 为避免编译器警告,总是设置输出参数 */
	if (result)
		*result = 0;
	if (hintmsg)
		*hintmsg = NULL;

	/*
	 * 尝试按整数解析(允许八进制或十六进制输入)。如
	 * 果换算在小数点或 'e' 处停止或溢出,则重新按浮点
	 * 数解析。只要没有以 'e' 开头的单位名,这就应当
	 * 该没有问题。如果将来有了,可以把测试扩展为检查
	 * 'e' 之后的正负号或数字,但目前没必要。
	 */
	errno = 0;
	val = strtol(value, &endptr, 0);
	if (*endptr == '.' || *endptr == 'e' || *endptr == 'E' ||
		errno == ERANGE)
	{
		errno = 0;
		val = strtod(value, &endptr);
	}

	if (endptr == value || errno == ERANGE)
		return false;			/* 这些情形不给 HINT */

	/* 拒绝 NaN(无穷将过不了范围检查) */
	if (isnan(val))
		return false;			/* 视同语法错误;无 HINT */

	/* 允许数字与单位之间有空白 */
	while (isspace((unsigned char) *endptr))
		endptr++;

	/* 可能处理单位 */
	if (*endptr != '\0')
	{
		if ((flags & GUC_UNIT) == 0)
			return false;		/* 该设置不接受单位 */

		if (!convert_to_base_unit(val,
								  endptr, (flags & GUC_UNIT),
								  &val))
		{
			/* 无效单位或单位后有垃圾;设置提示并失败。 */
			if (hintmsg)
			{
				if (flags & GUC_UNIT_MEMORY)
					*hintmsg = memory_units_hint;
				else
					*hintmsg = time_units_hint;
			}
			return false;
		}
	}

	/* 先取整为整数,再检查溢出 */
	val = rint(val);

	if (val > INT_MAX || val < INT_MIN)
	{
		if (hintmsg)
			*hintmsg = gettext_noop("Value exceeds integer range.");
		return false;
	}

	if (result)
		*result = (int) val;
	return true;
}

/*
 * 尝试按常规格式把 value 解析为浮点数。
 * 可选地,若 "flags" 表明允许单位,值后可跟单位名。
 *
 *
 * 解析正常则返回 true,否则 false。
 * 如果正常且 result 非 NULL,则把值存入 *result。
 * 如果不正常且 hintmsg 非 NULL,则把 *hintmsg 设为
 * 合适的 HINT 消息或 NULL。
 */
bool
parse_real(const char *value, double *result, int flags, const char **hintmsg)
{
	double		val;
	char	   *endptr;

	/* 为避免编译器警告,总是设置输出参数 */
	if (result)
		*result = 0;
	if (hintmsg)
		*hintmsg = NULL;

	errno = 0;
	val = strtod(value, &endptr);

	if (endptr == value || errno == ERANGE)
		return false;			/* 这些情形不给 HINT */

	/* 拒绝 NaN(无穷稍后过不了范围检查) */
	if (isnan(val))
		return false;			/* 视同语法错误;无 HINT */

	/* 允许数字与单位之间有空白 */
	while (isspace((unsigned char) *endptr))
		endptr++;

	/* 可能处理单位 */
	if (*endptr != '\0')
	{
		if ((flags & GUC_UNIT) == 0)
			return false;		/* 该设置不接受单位 */

		if (!convert_to_base_unit(val,
								  endptr, (flags & GUC_UNIT),
								  &val))
		{
			/* 无效单位或单位后有垃圾;设置提示并失败。 */
			if (hintmsg)
			{
				if (flags & GUC_UNIT_MEMORY)
					*hintmsg = memory_units_hint;
				else
					*hintmsg = time_units_hint;
			}
			return false;
		}
	}

	if (result)
		*result = val;
	return true;
}


/*
 * 按给定值查找枚举选项的名称。
 * 只应以已知有效值调用,找不到枚举选项即抛
 * 出 elog(ERROR)。
 *
 * 返回的字符串指向静态数据,且并非为修改
 * 而分配的。
 */
const char *
config_enum_lookup_by_value(struct config_enum *record, int val)
{
	const struct config_enum_entry *entry;

	for (entry = record->options; entry && entry->name; entry++)
	{
		if (entry->val == val)
			return entry->name;
	}

	elog(ERROR, "could not find enum option %d for %s",
		 val, record->gen.name);
	return NULL;				/* 哄编译器 */
}


/*
 * 按名称查找枚举选项值(不分大小写)。
 *
 * 如果找到了枚举选项,则设置 retval 值并返回
 * true;没找到则返回 false,retval 被设 0。
 */
bool
config_enum_lookup_by_name(struct config_enum *record, const char *value,
						   int *retval)
{
	const struct config_enum_entry *entry;

	for (entry = record->options; entry && entry->name; entry++)
	{
		if (pg_strcasecmp(value, entry->name) == 0)
		{
			*retval = entry->val;
			return true;
		}
	}

	*retval = 0;
	return false;
}


/*
 * 返回一个 palloc'd 字符串,列出某枚举 GUC 所有可用
 * 选项(不包括隐藏的),以给定分隔符隔开。
 * 若 prefix 非 NULL,则加在第一个枚举值之前。
 * 如果 suffix 非 NULL,则把它加到字符串末尾。
 */
char *
config_enum_get_options(struct config_enum *record, const char *prefix,
						const char *suffix, const char *separator)
{
	const struct config_enum_entry *entry;
	StringInfoData retstr;
	int			seplen;

	initStringInfo(&retstr);
	appendStringInfoString(&retstr, prefix);

	seplen = strlen(separator);
	for (entry = record->options; entry && entry->name; entry++)
	{
		if (!entry->hidden)
		{
			appendStringInfoString(&retstr, entry->name);
			appendBinaryStringInfo(&retstr, separator, seplen);
		}
	}

	/*
	 * 可能所有表项都被隐藏了;若未给出 prefix,字符串
	 * 会是空的。这表明 GUC 设置已损坏,因为没有任何值
	 * 的枚举毫无用处,所以我们只检查确保不会写入无效
	 * 内存,而不是真的试图对它做什么聪明处理。
	 *
	 */
	if (retstr.len >= seplen)
	{
		/* 替换末尾分隔符 */
		retstr.data[retstr.len - seplen] = '\0';
		retstr.len -= seplen;
	}

	appendStringInfoString(&retstr, suffix);

	return retstr.data;
}

/*
 * 解析并校验指定配置参数的拟定值。
 *
 *
 * 它执行内建检查(例如整数参数的范围限制),并且还
 * 会调用该参数可能有的任何检查钩子。
 *
 * record:GUC 变量的信息记录
 * name:变量名(当然应当与记录匹配)
 * value:拟定值(字符串形式)
 * source:标识值的来源(检查钩子可能需要它)
 * elevel:记录错误报告所用级别
 * newval:成功时,转换后的参数值在此返回
 * newextra:成功时,接收检查钩子返回的 "extra" 数据
 *	(调用方须先把 *newextra 置 NULL)
 *
 * 成功返回 true,否则 false(elevel >= ERROR 时抛错)
 */
static bool
parse_and_validate_value(struct config_generic *record,
						 const char *name, const char *value,
						 GucSource source, int elevel,
						 union config_var_val *newval, void **newextra)
{
	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

				if (!parse_bool(value, &newval->boolval))
				{
					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("parameter \"%s\" requires a Boolean value",
									name)));
					return false;
				}

				if (!call_bool_check_hook(conf, &newval->boolval, newextra,
										  source, elevel))
					return false;
			}
			break;
		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;
				const char *hintmsg;

				if (!parse_int(value, &newval->intval,
							   conf->gen.flags, &hintmsg))
				{
					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									name, value),
							 hintmsg ? errhint("%s", _(hintmsg)) : 0));
					return false;
				}

				if (newval->intval < conf->min || newval->intval > conf->max)
				{
					const char *unit = get_config_unit_name(conf->gen.flags);

					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("%d%s%s is outside the valid range for parameter \"%s\" (%d .. %d)",
									newval->intval,
									unit ? " " : "",
									unit ? unit : "",
									name,
									conf->min, conf->max)));
					return false;
				}

				if (!call_int_check_hook(conf, &newval->intval, newextra,
										 source, elevel))
					return false;
			}
			break;
		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;
				const char *hintmsg;

				if (!parse_real(value, &newval->realval,
								conf->gen.flags, &hintmsg))
				{
					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									name, value),
							 hintmsg ? errhint("%s", _(hintmsg)) : 0));
					return false;
				}

				if (newval->realval < conf->min || newval->realval > conf->max)
				{
					const char *unit = get_config_unit_name(conf->gen.flags);

					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("%g%s%s is outside the valid range for parameter \"%s\" (%g .. %g)",
									newval->realval,
									unit ? " " : "",
									unit ? unit : "",
									name,
									conf->min, conf->max)));
					return false;
				}

				if (!call_real_check_hook(conf, &newval->realval, newextra,
										  source, elevel))
					return false;
			}
			break;
		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;

				/*
				 * 调用者传入的值可能是暂态的,故我们总是
				 * strdup 它。
				 */
				newval->stringval = guc_strdup(elevel, value);
				if (newval->stringval == NULL)
					return false;

				/*
				 * 仅有的内建"解析"检查:若设 GUC_IS_NAME,
				 * 则应用截断。
				 */
				if (conf->gen.flags & GUC_IS_NAME)
					truncate_identifier(newval->stringval,
										strlen(newval->stringval),
										true);

				if (!call_string_check_hook(conf, &newval->stringval, newextra,
											source, elevel))
				{
					guc_free(newval->stringval);
					newval->stringval = NULL;
					return false;
				}
			}
			break;
		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

				if (!config_enum_lookup_by_name(conf, value, &newval->enumval))
				{
					char	   *hintmsg;

					hintmsg = config_enum_get_options(conf,
													  "Available values: ",
													  ".", ", ");

					ereport(elevel,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for parameter \"%s\": \"%s\"",
									name, value),
							 hintmsg ? errhint("%s", _(hintmsg)) : 0));

					if (hintmsg)
						pfree(hintmsg);
					return false;
				}

				if (!call_enum_check_hook(conf, &newval->enumval, newextra,
										  source, elevel))
					return false;
			}
			break;
	}

	return true;
}


/*
 * set_config_option:设选项 `name' 为给定值。
 *
 * 该值应为字符串,将被解析转换成相应数据类型。
 * context 与 source 参数指明本函数被调用时所处的
 * 上下文,以便它能正确地应用访问限制。
 *
 *
 * 如果 value 为 NULL,则把选项设为其默认值(通常是
 * reset_val,但若 source == PGC_S_DEFAULT 则改用 boot_val)。
 *
 * action 指示是把值设置为本会话全局,当前顶层事务局
 * 部,还是仅在函数调用期间有效。
 *
 * 如果 changeVal 为 false,则并不真正设置选项,而是
 * 执行检查看它能否生效。
 *
 * elevel 通常应传 0,让本函数自行选择标准 ereport 级
 * 别。不过某些调用者需要能够改写这一选择;它们应
 * 传入要使用的 ereport 级别。
 *
 * is_reload 仅当从 read_nondefault_variables() 或
 * RestoreGUCState() 调用时才应为 true;此时正把其
 * GUC 设置载入新进程。
 *
 * 返回值:
 *	+1:该值有效且已被成功应用。
 *	0:	名字或值无效,或此时尝试设置该 GUC 无效;
 *		但 elevel 低于 ERROR(见下文)。
 *	-1:未检测到错误,但值未被应用,原因是
 *		changeVal 为 false,或者存在某个优先设置。
 *
 * 如果有错误(选项不存在、值无效等),则会抛出
 * ereport(ERROR),*除非*调用来源属于我们不想要
 * ERROR 的来源(当前即默认值、配置文件以及按数据库
 * 或按用户的设置,还有指定低于 ERROR 的 elevel 的调
 * 用方)。在这些情况下,我们会通过 ereport() 写出合
 * 适的错误消息并返回 0。
 *
 * 另请参见 SetConfigOption 这一外部接口。
 */
int
set_config_option(const char *name, const char *value,
				  GucContext context, GucSource source,
				  GucAction action, bool changeVal, int elevel,
				  bool is_reload)
{
	Oid			srole;

	/*
	 * 非交互式来源应被视为拥有一切权限,但 PGC_S_CLIENT
	 * 除外。特别地,pg_db_role_setting 来源(PGC_S_GLOBAL
	 * 等)亦然:假定创建 pg_db_role_setting 条目时已经
	 * 做过合适的权限检查。
	 */
	if (source >= PGC_S_INTERACTIVE || source == PGC_S_CLIENT)
		srole = GetUserId();
	else
		srole = BOOTSTRAP_SUPERUSERID;

	return set_config_option_ext(name, value,
								 context, source, srole,
								 action, changeVal, elevel,
								 is_reload);
}

/*
 * set_config_option_ext:把选项 `name' 设为给定值。
 *
 * 本 API 可显式指定由哪个角色 OID 来设置该值。
 * 大多数外部调用者可以使用 set_config_option() 并让它
 * 根据 GucSource 自行判定;但有少数调用者提供的值
 * 以特殊方式确定,需要改写该判定。
 * 此外,在恢复先前赋的值时,提供最初设置该
 * 值的同一角色 OID 很重要;因此所有 guc.c 调用方
 * 都需要直接调用本函数。
 *
 *
 * 一般而言,当来源是 SQL 操作时 srole 应为 GetUserId(),
 * 而当来源是配置文件等时则为 BOOTSTRAP_SUPERUSERID。
 */
int
set_config_option_ext(const char *name, const char *value,
					  GucContext context, GucSource source, Oid srole,
					  GucAction action, bool changeVal, int elevel,
					  bool is_reload)
{
	struct config_generic *record;
	union config_var_val newval_union;
	void	   *newextra = NULL;
	bool		prohibitValueChange = false;
	bool		makeDefault;

	if (elevel == 0)
	{
		if (source == PGC_S_DEFAULT || source == PGC_S_FILE)
		{
			/*
			 * 为免日志被刷屏,只有 postmaster 会大声抱怨
			 * 配置文件的问题。
			 */
			elevel = IsUnderPostmaster ? DEBUG3 : LOG;
		}
		else if (source == PGC_S_GLOBAL ||
				 source == PGC_S_DATABASE ||
				 source == PGC_S_USER ||
				 source == PGC_S_DATABASE_USER)
			elevel = WARNING;
		else
			elevel = ERROR;
	}

	record = find_option(name, true, false, elevel);
	if (record == NULL)
		return 0;

	/*
	 * 在并行操作期间,GUC_ACTION_SAVE 更改是可以接受的,
	 * 因为当前 worker 也会弹出该更改。我们处理的很可能
	 * 是带有 proconfig 条目的函数。只有该函数的函数体应
	 * 观察到该更改,而且同侪 worker 不参与本 worker 启动
	 * 的函数调用的执行。
	 *
	 * 如果 GUC 标记了 GUC_ALLOW_IN_PARALLEL,也允许正常设置。
	 *
	 * 其他更改可能需要影响其他 worker,因此予以禁止。
	 */
	if (IsInParallelMode() && changeVal && action != GUC_ACTION_SAVE &&
		(record->flags & GUC_ALLOW_IN_PARALLEL) == 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("parameter \"%s\" cannot be set during a parallel operation",
						name)));
		return 0;
	}

	/*
	 * 检查该选项此时能否设置。精确规则参见 guc.h。
	 *
	 */
	switch (record->context)
	{
		case PGC_INTERNAL:
			if (context != PGC_INTERNAL)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed",
								name)));
				return 0;
			}
			break;
		case PGC_POSTMASTER:
			if (context == PGC_SIGHUP)
			{
				/*
				 * 我们正重新读取 PGC_POSTMASTER 变量,
				 * 它来自 postgresql.conf。该设置不可更改,所以
				 * 若 DBA 试图更改则给出警告。不过,由于
				 * 格式有变体、检查钩子会规范化等,
				 * 因此不能直接将给定字符串与所存储的
				 * 值进行比较。设置标志,待取得最终可存储
				 * 值后再检查。
				 */
				prohibitValueChange = true;
			}
			else if (context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed without restarting the server",
								name)));
				return 0;
			}
			break;
		case PGC_SIGHUP:
			if (context != PGC_SIGHUP && context != PGC_POSTMASTER)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be changed now",
								name)));
				return 0;
			}

			/*
			 * 呃,SIGHUP 上下文的理念是"应当是全局的,但
			 * 可在 postmaster 启动后更改"。但没有什么能
			 * 阻止狡猾的管理员只向个别后端发送 SIGHUP
			 * 信号。
			 */
			break;
		case PGC_SU_BACKEND:
			if (context == PGC_BACKEND)
			{
				/*
				 * 检查请求用户是否已被授予设置该
				 * GUC 的权限。
				 */
				AclResult	aclresult;

				aclresult = pg_parameter_aclcheck(name, srole, ACL_SET);
				if (aclresult != ACLCHECK_OK)
				{
					/* 未获授权 */
					ereport(elevel,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to set parameter \"%s\"",
									name)));
					return 0;
				}
			}
			/* 落到下方案例,按 PGC_BACKEND 同样处理 */
			/* FALLTHROUGH */
		case PGC_BACKEND:
			if (context == PGC_SIGHUP)
			{
				/*
				 * 如果在配置文件中更改了 PGC_BACKEND 或
				 * PGC_SU_BACKEND 参数,我们希望 postmaster
				 * 接受新值(它将随后传播到
				 * 随后启动的后端),但在既有后端中忽略它。
				 * 这有点笨拙,但却是必需的,因为我们
				 * 在启动后端时不会重新读配置文件。
				 *
				 * 不过,若 changeVal 为 false,则尽管继续推进,
				 * 因为我们只想查明该值是否潜在有效,而非
				 * 实际使用它。
				 *
				 * 在 EXEC_BACKEND 构建中,做法不同:我们会
				 * 启动后端期间加载 CONFIG_EXEC_PARAMS 文件
				 * 的全部非默认设置。此时必须接受
				 * PGC_SIGHUP 设置,以便取值如同直接
				 * fork 自 postmaster 那样。当在后台工作进程
				 * 中用 RestoreGUCState() 时也会出现:工作进程
				 * 需与启动它的用户后端有相同设置。上述
				 * 情形下 is_reload 都将为真。
				 *
				 */
				if (IsUnderPostmaster && changeVal && !is_reload)
					return -1;
			}
			else if (context != PGC_POSTMASTER &&
					 context != PGC_BACKEND &&
					 context != PGC_SU_BACKEND &&
					 source != PGC_S_CLIENT)
			{
				ereport(elevel,
						(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
						 errmsg("parameter \"%s\" cannot be set after connection start",
								name)));
				return 0;
			}
			break;
		case PGC_SUSET:
			if (context == PGC_USERSET || context == PGC_BACKEND)
			{
				/*
				 * 检查请求用户是否已被授予设置该
				 * GUC 的权限。
				 */
				AclResult	aclresult;

				aclresult = pg_parameter_aclcheck(name, srole, ACL_SET);
				if (aclresult != ACLCHECK_OK)
				{
					/* 未获授权 */
					ereport(elevel,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("permission denied to set parameter \"%s\"",
									name)));
					return 0;
				}
			}
			break;
		case PGC_USERSET:
			/*总是允许*/
			break;
	}

	/*
	 * 如果处于安全限制上下文之内,则禁止更改
	 * GUC_NOT_WHILE_SEC_REST 值。无论 GUC 上下文或来源如何,
	 * 这种更改都可以拒绝,主要原因是合理的覆盖来源
	 * 在函数内部是不会出现的。
	 *
	 * 注意:被标记为 GUC_NOT_WHILE_SEC_REST 的变量通常也
	 * 应标记为 GUC_NO_RESET_ALL,因为 ResetAllOptions()
	 * 不会检查此项。若重置值被假定"安全",则可例外。
	 *
	 * 注意:此标志目前用于 "session_authorization" 和
	 * "role"。必须禁止在局部用户 ID 上下文内更改,
	 * 因为退出它时 GUC 不会得到通知,致使状态脱节。
	 * (可以通过强制新的 GUC 嵌套层级来修复,
	 * 但这可能以不良方式改变行为。)此外,我们还
	 * 禁止在安全受限操作中更改它们,因为否则
	 * RESET 可能被用来重新取得会话用户的权限。
	 */
	if (record->flags & GUC_NOT_WHILE_SEC_REST)
	{
		if (InLocalUserIdChange())
		{
			/*
			 * 这条错误消息的措辞是历史遗留,但它是最
			 * 常见的。
			 */
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("cannot set parameter \"%s\" within security-definer function",
							name)));
			return 0;
		}
		if (InSecurityRestrictedOperation())
		{
			ereport(elevel,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("cannot set parameter \"%s\" within security-restricted operation",
							name)));
			return 0;
		}
	}

	/* 禁止重置并保存 GUC_NO_RESET 值 */
	if (record->flags & GUC_NO_RESET)
	{
		if (value == NULL)
		{
			ereport(elevel,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("parameter \"%s\" cannot be reset", name)));
			return 0;
		}
		if (action == GUC_ACTION_SAVE)
		{
			ereport(elevel,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("parameter \"%s\" cannot be set locally in functions",
							name)));
			return 0;
		}
	}

	/*
	 * 是否应设置 reset/stacked 值?(若是,其行为不是
	 * 事务性的。)做法是:当我们从数据库/用户/客户端
	 * 的默认设置中取得默认值,或者将一个值重置为
	 * 其默认值。
	 */
	makeDefault = changeVal && (source <= PGC_S_OVERRIDE) &&
		((value != NULL) || source == PGC_S_DEFAULT);

	/*
	 * 若被先前处理的设置覆盖,则忽略此次设置尝试。
	 * 不过,若 changeVal 为 false,则尽管继续推进,因为我们
	 * 只想查明该值是否潜在有效,而非实际使用它。同时
	 * 如果 makeDefault 为真也继续,因为我们可能想要设置
	 * reset/stacked 值,即使无法设置变量本身。
	 */
	if (record->source > source)
	{
		if (changeVal && !makeDefault)
		{
			elog(DEBUG3, "\"%s\": setting ignored because previous source is higher priority",
				 name);
			return -1;
		}
		changeVal = false;
	}

	/*
	 * 求值并设置变量。
	 */
	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

#define newval (newval_union.boolval)

				if (value)
				{
					if (!parse_and_validate_value(record, name, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_bool_check_hook(conf, &newval, &newextra,
											  source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* 释放 newextra,除非它是 reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* 保存旧值以支持事务中止 */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.boolval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* 可能未在任何位置安装 newextra */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;

#define newval (newval_union.intval)

				if (value)
				{
					if (!parse_and_validate_value(record, name, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_int_check_hook(conf, &newval, &newextra,
											 source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* 释放 newextra,除非它是 reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* 保存旧值以支持事务中止 */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.intval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* 可能未在任何位置安装 newextra */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;

#define newval (newval_union.realval)

				if (value)
				{
					if (!parse_and_validate_value(record, name, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_real_check_hook(conf, &newval, &newextra,
											  source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* 释放 newextra,除非它是 reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* 保存旧值以支持事务中止 */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.realval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* 可能未在任何位置安装 newextra */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;
				GucContext	orig_context = context;
				GucSource	orig_source = source;
				Oid			orig_srole = srole;

#define newval (newval_union.stringval)

				if (value)
				{
					if (!parse_and_validate_value(record, name, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					/* 非 NULL boot_val 必须始终被 strdup */
					if (conf->boot_val != NULL)
					{
						newval = guc_strdup(elevel, conf->boot_val);
						if (newval == NULL)
							return 0;
					}
					else
						newval = NULL;

					if (!call_string_check_hook(conf, &newval, &newextra,
												source, elevel))
					{
						guc_free(newval);
						return 0;
					}
				}
				else
				{
					/*
					 * 无需 strdup,因为 reset_val 已在
					 * guc.c 管辖
					 */
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					bool		newval_different;

					/* newval 不应为 NULL,故此处稍欠严谨 */
					newval_different = (*conf->variable == NULL ||
										newval == NULL ||
										strcmp(*conf->variable, newval) != 0);

					/* 释放 newval,除非它是 reset_val */
					if (newval && !string_field_used(conf, newval))
						guc_free(newval);
					/* 释放 newextra,除非它是 reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (newval_different)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* 保存旧值以支持事务中止 */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					set_string_field(conf, conf->variable, newval);
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;

					/*
					 * 丑陋的取巧做法:在 SET session_authorization
					 * 期间强制执行 SET ROLE NONE,并使用相同的
					 * 上下文/来源等,使两者的效果拥有相同的
					 * 生命周期。这是 SQL 规范所要求的,而且
					 * 无法在变量的检查钩子或赋值钩子内
					 * 完成,因为相关 API 传递的信息不够。
					 * 不过,若是 is_reload 则不要这样做:此时
					 * 我们预期,若 "role" 不应为默认值,它已经
					 * 或者将要由单独的重载动作来设置。
					 *
					 * 此外,对于来自 InitializeSessionUserId 且
					 * source == PGC_S_OVERRIDE 的调用,把 "role"
					 * 的来源设为 PGC_S_DYNAMIC_DEFAULT,以便
					 * 仍可从 pg_db_role_setting 条目设置 "role"。
					 * (改动前请先读该函数的说明。)
					 *
					 * 细节:对于 RESET session_authorization,我们
					 * 执行的是 "RESET role" 而非 "SET ROLE NONE"
					 * (向下传 NULL 而非 "none" 作为值)。这在
					 * 典型情形下效果相同,但若 "role" 的重置
					 * 值不是 "none",回退到该值似乎更好。
					 *
					 */
					if (!is_reload &&
						strcmp(conf->gen.name, "session_authorization") == 0)
						(void) set_config_option_ext("role",
													 value ? "none" : NULL,
													 orig_context,
													  (orig_source == PGC_S_OVERRIDE)
													  ? PGC_S_DYNAMIC_DEFAULT
													  : orig_source,
													 orig_srole,
													 action,
													 true,
													 elevel,
													 false);
				}

				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						set_string_field(conf, &conf->reset_val, newval);
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							set_string_field(conf, &stack->prior.val.stringval,
											 newval);
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* 可能未在任何位置安装 newval */
				if (newval && !string_field_used(conf, newval))
					guc_free(newval);
				/* 可能未在任何位置安装 newextra */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

#define newval (newval_union.enumval)

				if (value)
				{
					if (!parse_and_validate_value(record, name, value,
												  source, elevel,
												  &newval_union, &newextra))
						return 0;
				}
				else if (source == PGC_S_DEFAULT)
				{
					newval = conf->boot_val;
					if (!call_enum_check_hook(conf, &newval, &newextra,
											  source, elevel))
						return 0;
				}
				else
				{
					newval = conf->reset_val;
					newextra = conf->reset_extra;
					source = conf->gen.reset_source;
					context = conf->gen.reset_scontext;
					srole = conf->gen.reset_srole;
				}

				if (prohibitValueChange)
				{
					/* 释放 newextra,除非它是 reset_extra */
					if (newextra && !extra_field_used(&conf->gen, newextra))
						guc_free(newextra);

					if (*conf->variable != newval)
					{
						record->status |= GUC_PENDING_RESTART;
						ereport(elevel,
								(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
								 errmsg("parameter \"%s\" cannot be changed without restarting the server",
										name)));
						return 0;
					}
					record->status &= ~GUC_PENDING_RESTART;
					return -1;
				}

				if (changeVal)
				{
					/* 保存旧值以支持事务中止 */
					if (!makeDefault)
						push_old_value(&conf->gen, action);

					if (conf->assign_hook)
						conf->assign_hook(newval, newextra);
					*conf->variable = newval;
					set_extra_field(&conf->gen, &conf->gen.extra,
									newextra);
					set_guc_source(&conf->gen, source);
					conf->gen.scontext = context;
					conf->gen.srole = srole;
				}
				if (makeDefault)
				{
					GucStack   *stack;

					if (conf->gen.reset_source <= source)
					{
						conf->reset_val = newval;
						set_extra_field(&conf->gen, &conf->reset_extra,
										newextra);
						conf->gen.reset_source = source;
						conf->gen.reset_scontext = context;
						conf->gen.reset_srole = srole;
					}
					for (stack = conf->gen.stack; stack; stack = stack->prev)
					{
						if (stack->source <= source)
						{
							stack->prior.val.enumval = newval;
							set_extra_field(&conf->gen, &stack->prior.extra,
											newextra);
							stack->source = source;
							stack->scontext = context;
							stack->srole = srole;
						}
					}
				}

				/* 可能未在任何位置安装 newextra */
				if (newextra && !extra_field_used(&conf->gen, newextra))
					guc_free(newextra);
				break;

#undef newval
			}
	}

	if (changeVal && (record->flags & GUC_REPORT) &&
		!(record->status & GUC_NEEDS_REPORT))
	{
		record->status |= GUC_NEEDS_REPORT;
		slist_push_head(&guc_report_list, &record->report_link);
	}

	return changeVal ? 1 : -1;
}


/*
 * 设置该设置所来自的源文件与行号字段。
 */
static void
set_config_sourcefile(const char *name, char *sourcefile, int sourceline)
{
	struct config_generic *record;
	int			elevel;

	/*
	 * 为了避免弄乱日志,只有 postmaster 会大声报告
	 * 配置文件的问题。
	 */
	elevel = IsUnderPostmaster ? DEBUG3 : LOG;

	record = find_option(name, true, false, elevel);
	/* 不应发生 */
	if (record == NULL)
		return;

	sourcefile = guc_strdup(elevel, sourcefile);
	guc_free(record->sourcefile);
	record->sourcefile = sourcefile;
	record->sourceline = sourceline;
}

/*
 * 将配置选项设置为给定值。
 *
 * 另见 set_config_option;这只是供 GUC 之外
 * 调用的包装器。(应尽可能使用本函数,因为它的 API
 * 比 set_config_option 的更稳定。)
 *
 * 注意:这里不支持设置来源文件/行号,因为目前
 * 并不需要。
 */
void
SetConfigOption(const char *name, const char *value,
				GucContext context, GucSource source)
{
	(void) set_config_option(name, value, context, source,
							 GUC_ACTION_SET, true, 0, false);
}



/*
 * 以字符串形式获取选项 `name' 的当前值。
 *
 * 如果选项不存在,当 missing_ok 为真则返回 NULL(注意
 * 这无法与值为 NULL 的字符串变量区分开!),
 * 否则抛出 ereport 且不返回。
 *
 * 如果 restrict_privileged 为真,我们还强制只有超级
 * 用户和 pg_read_all_settings 角色的成员才能看见
 * GUC_SUPERUSER_ONLY 变量。仅用户驱动的调用传入真值。
 *
 * 该字符串并非为修改而分配,并且实际上仅在
 * 下一次调用配置相关函数之前有效。
 */
const char *
GetConfigOption(const char *name, bool missing_ok, bool restrict_privileged)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name, false, missing_ok, ERROR);
	if (record == NULL)
		return NULL;
	if (restrict_privileged &&
		!ConfigOptionIsVisible(record))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to examine \"%s\"", name),
				 errdetail("Only roles with privileges of the \"%s\" role may examine this parameter.",
						   "pg_read_all_settings")));

	switch (record->vartype)
	{
		case PGC_BOOL:
			return *((struct config_bool *) record)->variable ? "on" : "off";

		case PGC_INT:
			snprintf(buffer, sizeof(buffer), "%d",
					 *((struct config_int *) record)->variable);
			return buffer;

		case PGC_REAL:
			snprintf(buffer, sizeof(buffer), "%g",
					 *((struct config_real *) record)->variable);
			return buffer;

		case PGC_STRING:
			return *((struct config_string *) record)->variable;

		case PGC_ENUM:
			return config_enum_lookup_by_value((struct config_enum *) record,
											   *((struct config_enum *) record)->variable);
	}
	return NULL;
}

/*
 * 获取与给定选项关联的 RESET 值。
 *
 * 注意:由于使用静态结果缓冲区,本函数不可重入;
 * 更不用说字符串变量的 reset_val 还可能被改变。
 * 不要以为结果值能在很长时间内保持有效。
 */
const char *
GetConfigOptionResetString(const char *name)
{
	struct config_generic *record;
	static char buffer[256];

	record = find_option(name, false, false, ERROR);
	Assert(record != NULL);
	if (!ConfigOptionIsVisible(record))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to examine \"%s\"", name),
				 errdetail("Only roles with privileges of the \"%s\" role may examine this parameter.",
						   "pg_read_all_settings")));

	switch (record->vartype)
	{
		case PGC_BOOL:
			return ((struct config_bool *) record)->reset_val ? "on" : "off";

		case PGC_INT:
			snprintf(buffer, sizeof(buffer), "%d",
					 ((struct config_int *) record)->reset_val);
			return buffer;

		case PGC_REAL:
			snprintf(buffer, sizeof(buffer), "%g",
					 ((struct config_real *) record)->reset_val);
			return buffer;

		case PGC_STRING:
			return ((struct config_string *) record)->reset_val;

		case PGC_ENUM:
			return config_enum_lookup_by_value((struct config_enum *) record,
											   ((struct config_enum *) record)->reset_val);
	}
	return NULL;
}

/*
 * 获取与给定选项关联的 GUC 标志。
 *
 * 如果选项不存在,当 missing_ok 为真则返回 0,
 * 否则抛出 ereport 且不返回。
 */
int
GetConfigOptionFlags(const char *name, bool missing_ok)
{
	struct config_generic *record;

	record = find_option(name, false, missing_ok, ERROR);
	if (record == NULL)
		return 0;
	return record->flags;
}


/*
 * 将更新后的配置参数值写入一个临时文件。
 * 本函数遍历参数列表,并在写入之前为字符串
 * 值加上引号。
 */
static void
write_auto_conf_file(int fd, const char *filename, ConfigVariable *head)
{
	StringInfoData buf;
	ConfigVariable *item;

	initStringInfo(&buf);

	/* 输出包含警告注释的文件头 */
	appendStringInfoString(&buf, "# Do not edit this file manually!\n");
	appendStringInfoString(&buf, "# It will be overwritten by the ALTER SYSTEM command.\n");

	errno = 0;
	if (write(fd, buf.data, buf.len) != buf.len)
	{
		/* 若 write 未设 errno,视为磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", filename)));
	}

	/* 输出各参数,并正确为值加引号 */
	for (item = head; item != NULL; item = item->next)
	{
		char	   *escaped;

		resetStringInfo(&buf);

		appendStringInfoString(&buf, item->name);
		appendStringInfoString(&buf, " = '");

		escaped = escape_single_quotes_ascii(item->value);
		if (!escaped)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		appendStringInfoString(&buf, escaped);
		free(escaped);

		appendStringInfoString(&buf, "'\n");

		errno = 0;
		if (write(fd, buf.data, buf.len) != buf.len)
		{
			/* 若 write 未设 errno,视为磁盘空间不足 */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", filename)));
		}
	}

	/* 在认为写入成功之前先执行 fsync */
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", filename)));

	pfree(buf.data);
}

/*
 * 更新给定的配置参数列表:增加、替换或删除
 * "name" 项的条目(若 "value" == NULL 则删除)。
 */
static void
replace_auto_config_value(ConfigVariable **head_p, ConfigVariable **tail_p,
						  const char *name, const char *value)
{
	ConfigVariable *item,
			   *next,
			   *prev = NULL;

	/*
	 * 移除 "name" 的所有既有匹配项。通常至多只有
	 * 一个,但如果外部工具改过配置文件,则可能更多。
	 *
	 */
	for (item = *head_p; item != NULL; item = next)
	{
		next = item->next;
		if (guc_name_compare(item->name, name) == 0)
		{
			/* 找到匹配项,删除 */
			if (prev)
				prev->next = next;
			else
				*head_p = next;
			if (next == NULL)
				*tail_p = prev;

			pfree(item->name);
			pfree(item->value);
			pfree(item->filename);
			pfree(item);
		}
		else
			prev = item;
	}

	/* 若尝试的是删除,则完成 */
	if (value == NULL)
		return;

	/* 好,追加新条目 */
	item = palloc(sizeof *item);
	item->name = pstrdup(name);
	item->value = pstrdup(value);
	item->errmsg = NULL;
	item->filename = pstrdup("");	/* 新项无位置信息 */
	item->sourceline = 0;
	item->ignore = false;
	item->applied = false;
	item->next = NULL;

	if (*head_p == NULL)
		*head_p = item;
	else
		(*tail_p)->next = item;
	*tail_p = item;
}


/*
 * 执行 ALTER SYSTEM 语句。
 *
 * 读取旧 PG_AUTOCONF_FILENAME 文件,合并入新变量值,
 * 然后写出更新文件。若命令是 ALTER SYSTEM RESET ALL,
 * 则可以跳过读取旧文件,直接写出一个空文件。
 *
 * 使用一个 LWLock 来串行化对配置文件的更新。
 *
 * 如果发生错误,我们让原有的自动
 * 配置文件(PG_AUTOCONF_FILENAME)保持原样。
 */
void
AlterSystemSetConfigFile(AlterSystemStmt *altersysstmt)
{
	char	   *name;
	char	   *value;
	bool		resetall = false;
	ConfigVariable *head = NULL;
	ConfigVariable *tail = NULL;
	volatile int Tmpfd;
	char		AutoConfFileName[MAXPGPATH];
	char		AutoConfTmpFileName[MAXPGPATH];

	/*
	 * 提取语句参数
	 */
	name = altersysstmt->setstmt->name;

	switch (altersysstmt->setstmt->kind)
	{
		case VAR_SET_VALUE:
			value = ExtractSetVariableArgs(altersysstmt->setstmt);
			break;

		case VAR_SET_DEFAULT:
		case VAR_RESET:
			value = NULL;
			break;

		case VAR_RESET_ALL:
			value = NULL;
			resetall = true;
			break;

		default:
			elog(ERROR, "unrecognized alter system stmt type: %d",
				 altersysstmt->setstmt->kind);
			break;
	}

	/*
	 * 检查对目标变量运行 ALTER SYSTEM 的权限
	 */
	if (!superuser())
	{
		if (resetall)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to perform ALTER SYSTEM RESET ALL")));
		else
		{
			AclResult	aclresult;

			aclresult = pg_parameter_aclcheck(name, GetUserId(),
											  ACL_ALTER_SYSTEM);
			if (aclresult != ACLCHECK_OK)
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set parameter \"%s\"",
								name)));
		}
	}

	/*
	 * 除非是 RESET_ALL,否则校验目标变量与值
	 */
	if (!resetall)
	{
		struct config_generic *record;

		record = find_option(name, false, false, ERROR);
		Assert(record != NULL);

		/*
		 * 不允许将无法在配置文件中设置的参数设置到
		 * PG_AUTOCONF_FILENAME 文件中。
		 */
		if ((record->context == PGC_INTERNAL) ||
			(record->flags & GUC_DISALLOW_IN_FILE) ||
			(record->flags & GUC_DISALLOW_IN_AUTO_FILE))
			ereport(ERROR,
					(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					 errmsg("parameter \"%s\" cannot be changed",
							name)));

		/*
		 * 如果指定了值,则验证其健全。
		 */
		if (value)
		{
			union config_var_val newval;
			void	   *newextra = NULL;

			/* 检查它对于指定参数是否可接受 */
			if (!parse_and_validate_value(record, name, value,
										  PGC_S_FILE, ERROR,
										  &newval, &newextra))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid value for parameter \"%s\": \"%s\"",
								name, value)));

			if (record->vartype == PGC_STRING && newval.stringval != NULL)
				guc_free(newval.stringval);
			guc_free(newextra);

			/*
			 * 还必须拒绝含换行符的值,因为配置文件
			 * 的文法不支持字符串字面量中内嵌换行。
			 *
			 */
			if (strchr(value, '\n'))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter value for ALTER SYSTEM must not contain a newline")));
		}
	}

	/*
	 * PG_AUTOCONF_FILENAME 与其对应的临时文件总是位于
	 * 数据目录中,因此可用简单相对路径引用它们。
	 */
	snprintf(AutoConfFileName, sizeof(AutoConfFileName), "%s",
			 PG_AUTOCONF_FILENAME);
	snprintf(AutoConfTmpFileName, sizeof(AutoConfTmpFileName), "%s.%s",
			 AutoConfFileName,
			 "tmp");

	/*
	 * 同一时刻只允许一个后端操作 PG_AUTOCONF_FILENAME。
	 * 使用 AutoFileLock 来保证这一点。读取旧文件内容期间
	 * 必须持有该锁。
	 */
	LWLockAcquire(AutoFileLock, LW_EXCLUSIVE);

	/*
	 * 如果我们将要重置一切,则无需打开或解析旧文件。
	 * 我们只管写出一个空列表。
	 */
	if (!resetall)
	{
		struct stat st;

		if (stat(AutoConfFileName, &st) == 0)
		{
			/*打开旧文件 PG_AUTOCONF_FILENAME*/
			FILE	   *infile;

			infile = AllocateFile(AutoConfFileName, "r");
			if (infile == NULL)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								AutoConfFileName)));

			/* 解析 */
			if (!ParseConfigFp(infile, AutoConfFileName, CONF_FILE_START_DEPTH,
							   LOG, &head, &tail))
				ereport(ERROR,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not parse contents of file \"%s\"",
								AutoConfFileName)));

			FreeFile(infile);
		}

		/*
		 * 现在,用新值替换已有条目,若无则添加它。
		 *
		 */
		replace_auto_config_value(&head, &tail, name, value);
	}

	/*
	 * 为设置该 GUC 变量调用 post-alter 钩子。GUC
	 * 在 pg_parameter_acl 中通常没有对应条目,因此我们
	 * 用名称来调用钩子,而不是用可能不存在的 OID。
	 * 尽管如此,我们仍传递 ParameterAclRelationId,
	 * 以便此调用上下文可与其他调用区分。("name" 在
	 * RESET ALL 时为 NULL。)
	 *
	 * 我们在这里而非最后做这件事,因为 ALTER SYSTEM 不
	 * 是事务性的。若钩子将中止我们的事务,在触碰文件
	 * 之前这样做会更干净。
	 */
	InvokeObjectPostAlterHookArgStr(ParameterAclRelationId, name,
									ACL_ALTER_SYSTEM,
									altersysstmt->setstmt->kind,
									false);

	/*
	 * 为了确保崩溃安全,先把新文件数据写入临时文件,
	 * 再原子地将其重命名到位。
	 *
	 * 如果存在先前崩溃遗留的临时文件,可以对它进行
	 * 截断并复用。
	 */
	Tmpfd = BasicOpenFile(AutoConfTmpFileName,
						  O_CREAT | O_RDWR | O_TRUNC);
	if (Tmpfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						AutoConfTmpFileName)));

	/*
	 * 使用 TRY 块在失败时清理文件。既然反正都需要 TRY
	 * 块,那么用 BasicOpenFile 而非 OpenTransientFile 亦可。
	 */
	PG_TRY();
	{
		/* 将新内容写入并同步到临时文件 */
		write_auto_conf_file(Tmpfd, AutoConfTmpFileName, head);

		/* 重命名前先关闭;某些平台可能要求 */
		close(Tmpfd);
		Tmpfd = -1;

		/*
		 * 由于重命名是原子操作,如果此后发生任何问题,
		 * 最坏只会丢失上次 ALTER SYSTEM 设置的参数。
		 *
		 */
		durable_rename(AutoConfTmpFileName, AutoConfFileName, ERROR);
	}
	PG_CATCH();
	{
		/* 先关闭,否则部分平台 unlink 会失败 */
		if (Tmpfd >= 0)
			close(Tmpfd);

		/* 解除链接但忽略错误 */
		(void) unlink(AutoConfTmpFileName);

		PG_RE_THROW();
	}
	PG_END_TRY();

	FreeConfigVariables(head);

	LWLockRelease(AutoFileLock);
}


/*
 * DefineCustomXXXVariable 子例程的公共代码:分配新
 * 变量的配置结构体并填入通用字段。
 */
static struct config_generic *
init_custom_variable(const char *name,
					 const char *short_desc,
					 const char *long_desc,
					 GucContext context,
					 int flags,
					 enum config_type type,
					 size_t sz)
{
	struct config_generic *gen;

	/*
	 * 只允许在共享库预加载期间创建自定义的
	 * PGC_POSTMASTER 变量;再晚的话,我们无法确保
	 * 该值在启动后不再改变。若发生这种情况则是致命
	 * 的 elog;仅仅报错并不安全,因为我们不知道调用
	 * 的加载模块已挂接了什么。
	 */
	if (context == PGC_POSTMASTER &&
		!process_shared_preload_libraries_in_progress)
		elog(FATAL, "cannot create PGC_POSTMASTER variables after startup");

	/*
	 * 我们无法支持自定义的 GUC_LIST_QUOTE 变量,因为
	 * 当定义它的扩展未被加载时,这类变量若被设置或被
	 * pg_dump 都会出问题。同样,将此视为致命错误,因为
	 * 可加载模块可能已部分完成初始化。
	 */
	if (flags & GUC_LIST_QUOTE)
		elog(FATAL, "extensions cannot define GUC_LIST_QUOTE variables");

	/*
	 * 在 pljava 提交 398f3b876ed402bdaec8bc804f29e2be95c75139
	 * (2015-12-15)之前,该模块的两个 PGC_USERSET 变量
	 * 可轻易提权到超级用户权限。限制这些变量是为了
	 * 保护尚未升级 pljava 的站点。
	 */
	if (context == PGC_USERSET &&
		(strcmp(name, "pljava.classpath") == 0 ||
		 strcmp(name, "pljava.vmoptions") == 0))
		context = PGC_SUSET;

	gen = (struct config_generic *) guc_malloc(ERROR, sz);
	memset(gen, 0, sz);

	gen->name = guc_strdup(ERROR, name);
	gen->context = context;
	gen->group = CUSTOM_OPTIONS;
	gen->short_desc = short_desc;
	gen->long_desc = long_desc;
	gen->flags = flags;
	gen->vartype = type;

	return gen;
}

/*
 * DefineCustomXXXVariable 子例程的公共代码:将新
 * 变量插入 GUC 变量哈希表,替换任何占位符。
 */
static void
define_custom_variable(struct config_generic *variable)
{
	const char *name = variable->name;
	GUCHashEntry *hentry;
	struct config_string *pHolder;

	/* 检查初始值与默认值的映射 */
	Assert(check_GUC_init(variable));

	/*
	 * 查看是否存在同名的占位符。
	 */
	hentry = (GUCHashEntry *) hash_search(guc_hashtab,
										  &name,
										  HASH_FIND,
										  NULL);
	if (hentry == NULL)
	{
		/*
		 * 没有要替换的占位符,可直接添加它,但
		 * 先确保它已初始化为默认值。
		 */
		InitializeOneGUCOption(variable);
		add_guc_variable(variable, ERROR);
		return;
	}

	/*
	 * 最好是占位符
	 */
	if ((hentry->gucvar->flags & GUC_CUSTOM_PLACEHOLDER) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("attempt to redefine parameter \"%s\"", name)));

	Assert(hentry->gucvar->vartype == PGC_STRING);
	pHolder = (struct config_string *) hentry->gucvar;

	/*
	 * 首先,把变量设为它的默认值。即使我们打算立即
	 * 应用一个新值也必须这样做,因为新值有可能
	 * 是无效的。
	 */
	InitializeOneGUCOption(variable);

	/*
	 * 替换哈希表中的占位符。我们没有改变名称
	 * (至少按大小写折叠计),因此哈希值不变。
	 */
	hentry->gucname = name;
	hentry->gucvar = variable;

	/*
	 * 同时也把它从所在的所有列表中移除。
	 */
	RemoveGUCFromLists(&pHolder->gen);

	/*
	 * 把占位符中存储的各字符串值赋给实际变量。
	 * 本质上,我们需要复制全部生效值和栈中值,
	 * 但要用适当的校验和数据类型调整。
	 *
	 * 如果赋值失败,我们报告 WARNING 并继续。我们
	 * 不想为坏值抛出 ERROR,因为这会搞乱正在
	 * 加载到一半的附加模块。在这种情形下,
	 * 默认状态或先前状态将转为生效。
	 */

	/* 先应用重置值(若有) */
	if (pHolder->reset_val)
		(void) set_config_option_ext(name, pHolder->reset_val,
									 pHolder->gen.reset_scontext,
									 pHolder->gen.reset_source,
									 pHolder->gen.reset_srole,
									 GUC_ACTION_SET, true, WARNING, false);
	/* 这不应导致任何东西入栈 */
	Assert(variable->stack == NULL);

	/* 现在,按入栈时的顺序应用当前值和栈中值 */
	reapply_stacked_values(variable, pHolder, pHolder->gen.stack,
						   *(pHolder->variable),
						   pHolder->gen.scontext, pHolder->gen.source,
						   pHolder->gen.srole);

	/* 同时复制所有已保存的来源位置信息 */
	if (pHolder->gen.sourcefile)
		set_config_sourcefile(name, pHolder->gen.sourcefile,
							  pHolder->gen.sourceline);

	/*
	 * 尽可能方便地释放占位符结构中能释放的部分。
	 * (这忽略了栈中条目,因此某些内存有可能泄漏。
	 * 由于这对每个会话中每个变量只会发生一次,
	 * 似乎不值得为此花太多代码。)
	 */
	set_string_field(pHolder, pHolder->variable, NULL);
	set_string_field(pHolder, &pHolder->reset_val, NULL);

	guc_free(pHolder);
}

/*
 * define_custom_variable 的递归子例程:重新应用非重置值
 *
 * 我们进行递归,以便各值按最初的顺序被应用。在每
 * 个递归层级上,按栈条目所隐含的方式应用传入的
 * 上层值。
 */
static void
reapply_stacked_values(struct config_generic *variable,
					   struct config_string *pHolder,
					   GucStack *stack,
					   const char *curvalue,
					   GucContext curscontext, GucSource cursource,
					   Oid cursrole)
{
	const char *name = variable->name;
	GucStack   *oldvarstack = variable->stack;

	if (stack != NULL)
	{
		/* 首先递归,使栈条目自底向上被处理 */
		reapply_stacked_values(variable, pHolder, stack->prev,
							   stack->prior.val.stringval,
							   stack->scontext, stack->source, stack->srole);

		/* 查看如何应用传入的值 */
		switch (stack->state)
		{
			case GUC_SAVE:
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_SAVE, true,
											 WARNING, false);
				break;

			case GUC_SET:
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_SET, true,
											 WARNING, false);
				break;

			case GUC_LOCAL:
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_LOCAL, true,
											 WARNING, false);
				break;

			case GUC_SET_LOCAL:
				/* 先以 SET 应用被屏蔽的值 */
				(void) set_config_option_ext(name, stack->masked.val.stringval,
											 stack->masked_scontext,
											 PGC_S_SESSION,
											 stack->masked_srole,
											 GUC_ACTION_SET, true,
											 WARNING, false);
				/* 再以 LOCAL 应用当前值 */
				(void) set_config_option_ext(name, curvalue,
											 curscontext, cursource, cursrole,
											 GUC_ACTION_LOCAL, true,
											 WARNING, false);
				break;
		}

		/* 若成功创建了栈条目,则调整其嵌套层级 */
		if (variable->stack != oldvarstack)
			variable->stack->nest_level = stack->nest_level;
	}
	else
	{
		/*
		 * 我们已到达栈的末端。如果生效值/先前值与
		 * 重置值不同,那它必定代表先前已提交的
		 * 会话值。应用它,然后丢弃 set_config_option
		 * 出于"这只是事务性赋值"误解而创建的栈条目。
		 * (我们会泄漏该栈条目。)
		 *
		 */
		if (curvalue != pHolder->reset_val ||
			curscontext != pHolder->gen.reset_scontext ||
			cursource != pHolder->gen.reset_source ||
			cursrole != pHolder->gen.reset_srole)
		{
			(void) set_config_option_ext(name, curvalue,
										 curscontext, cursource, cursrole,
										 GUC_ACTION_SET, true, WARNING, false);
			if (variable->stack != NULL)
			{
				slist_delete(&guc_stack_list, &variable->stack_link);
				variable->stack = NULL;
			}
		}
	}
}

/*
 * 供扩展调用来定义其自定义 GUC 变量的函数。
 */
void
DefineCustomBoolVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 bool *valueAddr,
						 bool bootValue,
						 GucContext context,
						 int flags,
						 GucBoolCheckHook check_hook,
						 GucBoolAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_bool *var;

	var = (struct config_bool *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_BOOL, sizeof(struct config_bool));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomIntVariable(const char *name,
						const char *short_desc,
						const char *long_desc,
						int *valueAddr,
						int bootValue,
						int minValue,
						int maxValue,
						GucContext context,
						int flags,
						GucIntCheckHook check_hook,
						GucIntAssignHook assign_hook,
						GucShowHook show_hook)
{
	struct config_int *var;

	var = (struct config_int *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_INT, sizeof(struct config_int));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->min = minValue;
	var->max = maxValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomRealVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 double *valueAddr,
						 double bootValue,
						 double minValue,
						 double maxValue,
						 GucContext context,
						 int flags,
						 GucRealCheckHook check_hook,
						 GucRealAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_real *var;

	var = (struct config_real *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_REAL, sizeof(struct config_real));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->min = minValue;
	var->max = maxValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomStringVariable(const char *name,
						   const char *short_desc,
						   const char *long_desc,
						   char **valueAddr,
						   const char *bootValue,
						   GucContext context,
						   int flags,
						   GucStringCheckHook check_hook,
						   GucStringAssignHook assign_hook,
						   GucShowHook show_hook)
{
	struct config_string *var;

	var = (struct config_string *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_STRING, sizeof(struct config_string));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

void
DefineCustomEnumVariable(const char *name,
						 const char *short_desc,
						 const char *long_desc,
						 int *valueAddr,
						 int bootValue,
						 const struct config_enum_entry *options,
						 GucContext context,
						 int flags,
						 GucEnumCheckHook check_hook,
						 GucEnumAssignHook assign_hook,
						 GucShowHook show_hook)
{
	struct config_enum *var;

	var = (struct config_enum *)
		init_custom_variable(name, short_desc, long_desc, context, flags,
							 PGC_ENUM, sizeof(struct config_enum));
	var->variable = valueAddr;
	var->boot_val = bootValue;
	var->reset_val = bootValue;
	var->options = options;
	var->check_hook = check_hook;
	var->assign_hook = assign_hook;
	var->show_hook = show_hook;
	define_custom_variable(&var->gen);
}

/*
 * 将给定 GUC 前缀标记为"保留"。
 *
 * 这会删除与该前缀匹配的所有既有占位符,
 * 然后阻止创建新的占位符。
 * 扩展应在定义完自己的全部自定义 GUC 之后调用
 * 本函数,以便发现拼错的配置条目。
 */
void
MarkGUCPrefixReserved(const char *className)
{
	int			classLen = strlen(className);
	HASH_SEQ_STATUS status;
	GUCHashEntry *hentry;
	MemoryContext oldcontext;

	/*
	 * 检查既有的占位符。我们必须真正移除无效的
	 * 占位符,否则将来并行 worker 启动会失败。(我们
	 * 不打算费心释放相关内存,因为这不常发生。)
	 *
	 */
	hash_seq_init(&status, guc_hashtab);
	while ((hentry = (GUCHashEntry *) hash_seq_search(&status)) != NULL)
	{
		struct config_generic *var = hentry->gucvar;

		if ((var->flags & GUC_CUSTOM_PLACEHOLDER) != 0 &&
			strncmp(className, var->name, classLen) == 0 &&
			var->name[classLen] == GUC_QUALIFIER_SEPARATOR)
		{
			ereport(WARNING,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("invalid configuration parameter name \"%s\", removing it",
							var->name),
					 errdetail("\"%s\" is now a reserved prefix.",
							   className)));
			/* 将它从哈希表中移除 */
			hash_search(guc_hashtab,
						&var->name,
						HASH_REMOVE,
						NULL);
			/* 也从其所在列表中移除 */
			RemoveGUCFromLists(var);
		}
	}

	/* 并记住该名称,以便防止将来出现错误。 */
	oldcontext = MemoryContextSwitchTo(GUCMemoryContext);
	reserved_class_prefix = lappend(reserved_class_prefix, pstrdup(className));
	MemoryContextSwitchTo(oldcontext);
}


/*
 * 返回供 EXPLAIN 显示的已修改 GUC 选项数组。
 *
 * 我们只报告与查询规划相关的选项(以 GUC_EXPLAIN 标记),
 * 且取值不同于其内建默认值的选项。
 */
struct config_generic **
get_explain_guc_options(int *num)
{
	struct config_generic **result;
	dlist_iter	iter;

	*num = 0;

	/*
	 * 虽然所有 GUC 变量中只有一小部分被标记为 GUC_EXPLAIN,
	 * 但动态调整该数组大小似乎并不值得。
	 */
	result = palloc(sizeof(struct config_generic *) * hash_get_num_entries(guc_hashtab));

	/* 只需考虑来源不是 PGC_S_DEFAULT 的 GUC */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *conf = dlist_container(struct config_generic,
													  nondef_link, iter.cur);
		bool		modified;

		/* 只返回被标记为纳入 explain 的参数 */
		if (!(conf->flags & GUC_EXPLAIN))
			continue;

		/* 只返回当前用户可见的选项 */
		if (!ConfigOptionIsVisible(conf))
			continue;

		/* 只返回与其引导值不同的选项 */
		modified = false;

		switch (conf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *lconf = (struct config_bool *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			case PGC_INT:
				{
					struct config_int *lconf = (struct config_int *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			case PGC_REAL:
				{
					struct config_real *lconf = (struct config_real *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			case PGC_STRING:
				{
					struct config_string *lconf = (struct config_string *) conf;

					if (lconf->boot_val == NULL &&
						*lconf->variable == NULL)
						modified = false;
					else if (lconf->boot_val == NULL ||
							 *lconf->variable == NULL)
						modified = true;
					else
						modified = (strcmp(lconf->boot_val, *(lconf->variable)) != 0);
				}
				break;

			case PGC_ENUM:
				{
					struct config_enum *lconf = (struct config_enum *) conf;

					modified = (lconf->boot_val != *(lconf->variable));
				}
				break;

			default:
				elog(ERROR, "unexpected GUC type: %d", conf->vartype);
		}

		if (!modified)
			continue;

		/* 好,报告它 */
		result[*num] = conf;
		*num = *num + 1;
	}

	return result;
}

/*
 * 按名称返回 GUC 变量值;可选地返回名称的规范
 * 形式。如果 GUC 未设置,则抛出错误,除非 missing_ok
 * 为真,此时返回 NULL。返回值经 palloc 分配(*varname 除外)。
 */
char *
GetConfigOptionByName(const char *name, const char **varname, bool missing_ok)
{
	struct config_generic *record;

	record = find_option(name, false, missing_ok, ERROR);
	if (record == NULL)
	{
		if (varname)
			*varname = NULL;
		return NULL;
	}

	if (!ConfigOptionIsVisible(record))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to examine \"%s\"", name),
				 errdetail("Only roles with privileges of the \"%s\" role may examine this parameter.",
						   "pg_read_all_settings")));

	if (varname)
		*varname = record->name;

	return ShowGUCOption(record, true);
}

/*
 * ShowGUCOption:获取变量的字符串值
 *
 * 如果数值带有单位且 use_units 为真,则以合适的
 * 单位表示该值;否则只得到原始数字。
 * 结果串经 palloc 分配。
 */
char *
ShowGUCOption(struct config_generic *record, bool use_units)
{
	char		buffer[256];
	const char *val;

	switch (record->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
					val = *conf->variable ? "on" : "off";
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
				{
					/*
					 * 用 int64 算术避免单位换算溢出。
					 *
					 */
					int64		result = *conf->variable;
					const char *unit;

					if (use_units && result > 0 && (record->flags & GUC_UNIT))
						convert_int_from_base_unit(result,
												   record->flags & GUC_UNIT,
												   &result, &unit);
					else
						unit = "";

					snprintf(buffer, sizeof(buffer), INT64_FORMAT "%s",
							 result, unit);
					val = buffer;
				}
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
				{
					double		result = *conf->variable;
					const char *unit;

					if (use_units && result > 0 && (record->flags & GUC_UNIT))
						convert_real_from_base_unit(result,
													record->flags & GUC_UNIT,
													&result, &unit);
					else
						unit = "";

					snprintf(buffer, sizeof(buffer), "%g%s",
							 result, unit);
					val = buffer;
				}
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else if (*conf->variable && **conf->variable)
					val = *conf->variable;
				else
					val = "";
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) record;

				if (conf->show_hook)
					val = conf->show_hook();
				else
					val = config_enum_lookup_by_value(conf, *conf->variable);
			}
			break;

		default:
			/* 只为让编译器安静 */
			val = "???";
			break;
	}

	return pstrdup(val);
}


#ifdef EXEC_BACKEND

/*
 *	这些例程把所有非默认 GUC 选项转储到一个二进制
 *	文件,由所有 exec 出的后端读取。格式为:
 *
 *		变量名,字符串,以 NUL 结尾
 *		变量取值,字符串,NUL 结尾
 *		变量来源文件,字符串,NUL 结尾(无则空)
 *		变量来源行号,整数
 *		变量来源,整数
 *		安全上下文,整数
*		变量 srole,OID
 */
static void
write_one_nondefault_variable(FILE *fp, struct config_generic *gconf)
{
	Assert(gconf->source != PGC_S_DEFAULT);

	fprintf(fp, "%s", gconf->name);
	fputc(0, fp);

	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;

				if (*conf->variable)
					fprintf(fp, "true");
				else
					fprintf(fp, "false");
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				fprintf(fp, "%d", *conf->variable);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;

				fprintf(fp, "%.17g", *conf->variable);
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				if (*conf->variable)
					fprintf(fp, "%s", *conf->variable);
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				fprintf(fp, "%s",
						config_enum_lookup_by_value(conf, *conf->variable));
			}
			break;
	}

	fputc(0, fp);

	if (gconf->sourcefile)
		fprintf(fp, "%s", gconf->sourcefile);
	fputc(0, fp);

	fwrite(&gconf->sourceline, 1, sizeof(gconf->sourceline), fp);
	fwrite(&gconf->source, 1, sizeof(gconf->source), fp);
	fwrite(&gconf->scontext, 1, sizeof(gconf->scontext), fp);
	fwrite(&gconf->srole, 1, sizeof(gconf->srole), fp);
}

void
write_nondefault_variables(GucContext context)
{
	int			elevel;
	FILE	   *fp;
	dlist_iter	iter;

	Assert(context == PGC_POSTMASTER || context == PGC_SIGHUP);

	elevel = (context == PGC_SIGHUP) ? LOG : ERROR;

	/*
	 * Open file
	 */
	fp = AllocateFile(CONFIG_EXEC_PARAMS_NEW, "w");
	if (!fp)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						CONFIG_EXEC_PARAMS_NEW)));
		return;
	}

	/* 只需考虑来源不是 PGC_S_DEFAULT 的 GUC */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		write_one_nondefault_variable(fp, gconf);
	}

	if (FreeFile(fp))
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						CONFIG_EXEC_PARAMS_NEW)));
		return;
	}

	/*
	 * 把新文件放到位。这在 Win32 上可能有延迟,但我们
	 * 不持排他锁。
	 */
	rename(CONFIG_EXEC_PARAMS_NEW, CONFIG_EXEC_PARAMS);
}


/*
 *	从文件读字符串,含 NUL 字节
 *
 *	EOF 且无读取时返回 NULL
 */
static char *
read_string_with_null(FILE *fp)
{
	int			i = 0,
				ch,
				maxlen = 256;
	char	   *str = NULL;

	do
	{
		if ((ch = fgetc(fp)) == EOF)
		{
			if (i == 0)
				return NULL;
			else
				elog(FATAL, "invalid format of exec config params file");
		}
		if (i == 0)
			str = guc_malloc(FATAL, maxlen);
		else if (i == maxlen)
			str = guc_realloc(FATAL, str, maxlen *= 2);
		str[i++] = ch;
	} while (ch != 0);

	return str;
}


/*
 *	本例程加载先前 postmaster 转储出来的非默认
 *	设置。
 */
void
read_nondefault_variables(void)
{
	FILE	   *fp;
	char	   *varname,
			   *varvalue,
			   *varsourcefile;
	int			varsourceline;
	GucSource	varsource;
	GucContext	varscontext;
	Oid			varsrole;

	/*
	 * Open file
	 */
	fp = AllocateFile(CONFIG_EXEC_PARAMS, "r");
	if (!fp)
	{
		/* 文件未找到无妨 */
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": %m",
							CONFIG_EXEC_PARAMS)));
		return;
	}

	for (;;)
	{
		if ((varname = read_string_with_null(fp)) == NULL)
			break;

		if (find_option(varname, true, false, FATAL) == NULL)
			elog(FATAL, "failed to locate variable \"%s\" in exec config params file", varname);

		if ((varvalue = read_string_with_null(fp)) == NULL)
			elog(FATAL, "invalid format of exec config params file");
		if ((varsourcefile = read_string_with_null(fp)) == NULL)
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsourceline, 1, sizeof(varsourceline), fp) != sizeof(varsourceline))
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsource, 1, sizeof(varsource), fp) != sizeof(varsource))
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varscontext, 1, sizeof(varscontext), fp) != sizeof(varscontext))
			elog(FATAL, "invalid format of exec config params file");
		if (fread(&varsrole, 1, sizeof(varsrole), fp) != sizeof(varsrole))
			elog(FATAL, "invalid format of exec config params file");

		(void) set_config_option_ext(varname, varvalue,
									 varscontext, varsource, varsrole,
									 GUC_ACTION_SET, true, 0, true);
		if (varsourcefile[0])
			set_config_sourcefile(varname, varsourcefile, varsourceline);

		guc_free(varname);
		guc_free(varvalue);
		guc_free(varsourcefile);
	}

	FreeFile(fp);
}
#endif							/* EXEC_BACKEND */

/*
 * can_skip_gucvar:
 * 决定 SerializeGUCState 是否可以跳过发送该 GUC 变量,
 * 或 RestoreGUCState 是否可跳过把该 GUC 重置为默认。
 *
 * 同一个测试竟能适用于两种情形,这有点神奇且脆弱。
 * 尤其要意识到:在 leader 侧和 worker 侧,我们很可能
 * 选出的是不同的 GUC 集合!改动前请务必彻底
 * 理解此处以及 RestoreGUCState 中的注释。
 */
static bool
can_skip_gucvar(struct config_generic *gconf)
{
	/*
	 * 我们可以跳过那些保证在 leader 与 worker 中取值
	 * 相同的 GUC。(注意,leader 与 worker 对哪些 GUC 属
	 * 于这一类别必须看法一致。为此考虑上下文和
	 * 名称没有问题,因为它们是 GUC 的不变
	 * 属性。)
	 *
	 * PGC_POSTMASTER 变量在特定 postmaster 的每个子进程
	 * 中总是取相同值,因此 worker 必然已经持有正确
	 * 的值。同样,PGC_INTERNAL 变量由特殊机制设置
	 * (若它们确实不是编译期常量)。因此总是
	 * 可以跳过。
	 *
	 * 对于所有其他 GUC,若其取值为编译进去的默认值
	 * (即 source == PGC_S_DEFAULT),则跳过它。在 leader 侧,
	 * 这意味着我们不发送具有默认值的 GUC,这通常
	 * 能节省大量工作。在 worker 侧,这意味着我们无需
	 * 把该 GUC 重置为默认值,因其已有该值。详见
	 * RestoreGUCState 中的注释。
	 */
	return gconf->context == PGC_POSTMASTER ||
		gconf->context == PGC_INTERNAL ||
		gconf->source == PGC_S_DEFAULT;
}

/*
 * estimate_variable_size:
 *		计算转储给定 GUC 变量所需的空间。
 *
 * 可以高估,但不可以低估。
 */
static Size
estimate_variable_size(struct config_generic *gconf)
{
	Size		size;
	Size		valsize = 0;

	/* 可跳过 GUC 零占用。 */
	if (can_skip_gucvar(gconf))
		return 0;

	/* 名称加零字节。 */
	size = strlen(gconf->name) + 1;

	/* 获取 GUC 值的最大显示长度。 */
	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				valsize = 5;	/*max(strlen('true'),strlen('false'))*/
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				/*
				 * 与其取精确显示长度,不如使用最大
				 * 长度。小值典型范围的最大长度可再减。
				 * 最大值为 2147483647,即 10 个字符。
				 * 符号占一字节。
				 */
				if (abs(*conf->variable) < 1000)
					valsize = 3 + 1;
				else
					valsize = 10 + 1;
			}
			break;

		case PGC_REAL:
			{
				/*
				 * 我们将使用 %e 以 REALTYPE_PRECISION
				 * 位小数打印。需计入符号、首数字、
				 * 小数点以及最多 3 位的指数。例如
				 * -3.99329042340000021e+110
				 */
				valsize = 1 + 1 + 1 + REALTYPE_PRECISION + 5;
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				/*
				 * 如果值为 NULL,我们以空字符串传输它。
				 * 虽然物理上并非同一个值,但 GUC 通常
				 * 把 NULL 与空字符串同等对待。
				 */
				if (*conf->variable)
					valsize = strlen(*conf->variable);
				else
					valsize = 0;
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				valsize = strlen(config_enum_lookup_by_value(conf, *conf->variable));
			}
			break;
	}

	/* 为值的结尾零字节预留空间 */
	size = add_size(size, valsize + 1);

	if (gconf->sourcefile)
		size = add_size(size, strlen(gconf->sourcefile));

	/* 为来源文件的结尾零字节预留空间 */
	size = add_size(size, 1);

	/* 文件非空即含行号。 */
	if (gconf->sourcefile && gconf->sourcefile[0])
		size = add_size(size, sizeof(gconf->sourceline));

	size = add_size(size, sizeof(gconf->source));
	size = add_size(size, sizeof(gconf->scontext));
	size = add_size(size, sizeof(gconf->srole));

	return size;
}

/*
 * EstimateGUCStateSpace:
 * 返回为当前进程存储 GUC 状态所需的大小
 */
Size
EstimateGUCStateSpace(void)
{
	Size		size;
	dlist_iter	iter;

	/* 加上保存 guc 状态数据大小所需空间 */
	size = sizeof(Size);

	/*
	 * 把每个 GUC 变量所需空间加起来。
	 *
	 * 只需处理非默认的 GUC。
	 */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		size = add_size(size, estimate_variable_size(gconf));
	}

	return size;
}

/*
 * do_serialize:
 * 把格式化后的字符串复制到目标。向前移动
 * 目标指针,并按相应字节数递减 maxbytes。如果
 * maxbytes 不够复制字符串,则报错退出。
 */
static void
do_serialize(char **destptr, Size *maxbytes, const char *fmt,...)
{
	va_list		vargs;
	int			n;

	if (*maxbytes <= 0)
		elog(ERROR, "not enough space to serialize GUC state");

	va_start(vargs, fmt);
	n = vsnprintf(*destptr, *maxbytes, fmt, vargs);
	va_end(vargs);

	if (n < 0)
	{
		/* 不应发生。最好显示 errno 描述。 */
		elog(ERROR, "vsnprintf failed: %m with format string \"%s\"", fmt);
	}
	if (n >= *maxbytes)
	{
		/* 这个其实也不应发生。 */
		elog(ERROR, "not enough space to serialize GUC state");
	}

	/* 把 destptr 移到 NUL 终止符之后 */
	*destptr += n + 1;
	*maxbytes -= n + 1;
}

/* do_serialize() 的二进制复制版 */
static void
do_serialize_binary(char **destptr, Size *maxbytes, void *val, Size valsize)
{
	if (valsize > *maxbytes)
		elog(ERROR, "not enough space to serialize GUC state");

	memcpy(*destptr, val, valsize);
	*destptr += valsize;
	*maxbytes -= valsize;
}

/*
 * serialize_variable:
 * 把 GUC 变量的名称、取值及其他信息转储到 destptr。
 */
static void
serialize_variable(char **destptr, Size *maxbytes,
				   struct config_generic *gconf)
{
	/* 忽略可跳过 GUC。 */
	if (can_skip_gucvar(gconf))
		return;

	do_serialize(destptr, maxbytes, "%s", gconf->name);

	switch (gconf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *conf = (struct config_bool *) gconf;

				do_serialize(destptr, maxbytes,
							 (*conf->variable ? "true" : "false"));
			}
			break;

		case PGC_INT:
			{
				struct config_int *conf = (struct config_int *) gconf;

				do_serialize(destptr, maxbytes, "%d", *conf->variable);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *conf = (struct config_real *) gconf;

				do_serialize(destptr, maxbytes, "%.*e",
							 REALTYPE_PRECISION, *conf->variable);
			}
			break;

		case PGC_STRING:
			{
				struct config_string *conf = (struct config_string *) gconf;

				/* NULL 变为空字符串,见 estimate_variable_size() */
				do_serialize(destptr, maxbytes, "%s",
							 *conf->variable ? *conf->variable : "");
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *conf = (struct config_enum *) gconf;

				do_serialize(destptr, maxbytes, "%s",
							 config_enum_lookup_by_value(conf, *conf->variable));
			}
			break;
	}

	do_serialize(destptr, maxbytes, "%s",
				 (gconf->sourcefile ? gconf->sourcefile : ""));

	if (gconf->sourcefile && gconf->sourcefile[0])
		do_serialize_binary(destptr, maxbytes, &gconf->sourceline,
							sizeof(gconf->sourceline));

	do_serialize_binary(destptr, maxbytes, &gconf->source,
						sizeof(gconf->source));
	do_serialize_binary(destptr, maxbytes, &gconf->scontext,
						sizeof(gconf->scontext));
	do_serialize_binary(destptr, maxbytes, &gconf->srole,
						sizeof(gconf->srole));
}

/*
 * SerializeGUCState:
 * 把完整 GUC 状态转储到 start_address 处的内存位置。
 */
void
SerializeGUCState(Size maxsize, char *start_address)
{
	char	   *curptr;
	Size		actual_size;
	Size		bytes_left;
	dlist_iter	iter;

	/* 为保存 guc 状态的实际大小预留空间 */
	Assert(maxsize > sizeof(actual_size));
	curptr = start_address + sizeof(actual_size);
	bytes_left = maxsize - sizeof(actual_size);

	/* 只需考虑来源不是 PGC_S_DEFAULT 的 GUC */
	dlist_foreach(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		serialize_variable(&curptr, &bytes_left, gconf);
	}

	/* 存储实际大小,不假设 start_address 的对齐。 */
	actual_size = maxsize - bytes_left - sizeof(actual_size);
	memcpy(start_address, &actual_size, sizeof(actual_size));
}

/*
 * read_gucstate:
 * 实际上它并不读取任何内容,只是返回 srcptr。但它
 * 会把 srcptr 移过结尾零字节,使调用方准备好读取
 * 下一个字符串。
 */
static char *
read_gucstate(char **srcptr, char *srcend)
{
	char	   *retptr = *srcptr;
	char	   *ptr;

	if (*srcptr >= srcend)
		elog(ERROR, "incomplete GUC state");

	/* 字符串变量都以 NUL 结尾 */
	for (ptr = *srcptr; ptr < srcend && *ptr != '\0'; ptr++)
		;

	if (ptr >= srcend)
		elog(ERROR, "could not find null terminator in GUC state");

	/* 把新位置设为紧跟 NUL 终止符之后的字节 */
	*srcptr = ptr + 1;

	return retptr;
}

/* read_gucstate() 二进制读取版。复制进 dest */
static void
read_gucstate_binary(char **srcptr, char *srcend, void *dest, Size size)
{
	if (*srcptr + size > srcend)
		elog(ERROR, "incomplete GUC state");

	memcpy(dest, *srcptr, size);
	*srcptr += size;
}

/*
 * 本回调用于在报告并行 worker 中恢复 GUC 时
 * 发生的错误时附加一条上下文消息。
 */
static void
guc_restore_error_context_callback(void *arg)
{
	char	  **error_context_name_and_value = (char **) arg;

	if (error_context_name_and_value)
		errcontext("while setting parameter \"%s\" to \"%s\"",
				   error_context_name_and_value[0],
				   error_context_name_and_value[1]);
}

/*
 * RestoreGUCState:
 * 读取指定地址处的 GUC 状态,并使本进程 GUC
 * 匹配。
 *
 * 注意,这只会让 worker 看到 leader GUC 状态的
 * 非常浅的视图:我们将知道当前生效的值,但不知道
 * 栈中值或重置值。这没有问题,因为 worker 只是
 * 在执行查询的一部分,在此期间生效值不会改变,
 * 而栈中值不可见。
 */
void
RestoreGUCState(void *gucstate)
{
	char	   *varname,
			   *varvalue,
			   *varsourcefile;
	int			varsourceline;
	GucSource	varsource;
	GucContext	varscontext;
	Oid			varsrole;
	char	   *srcptr = (char *) gucstate;
	char	   *srcend;
	Size		len;
	dlist_mutable_iter iter;
	ErrorContextCallback error_context_callback;

	/*
	 * 首先,确保所有可传输的 GUC 都重置为
	 * 默认值。leader 永远不会传输的那些 GUC 我们
	 * 绝不能碰,而可传输但已取默认值的那些则无需
	 * 去碰。于是,这最终就变成了与
	 * SerializeGUCState 所用相同的测试,尽管涉及的
	 * 变量集合很可能不同,因为 leader 的"非默认值变量"
	 * 集合可能不同于这个刚启动的 worker 中
	 * 非默认值集合。
	 *
	 * 一旦我们把所有可传输的 GUC 都设为默认值,
	 * 恢复 leader 发来的那些 GUC(因为它们在那边
	 * 取的是非默认值)就会恰好得到与 leader 相同的
	 * 那组 GUC 值。即使 worker 最初可能已经吸收了
	 * leader 尚未见到的 postgresql.conf 设置,或者
	 * 在 leader 启动后才建立的 ALTER USER/DATABASE SET 设置。
	 *
	 *
	 * 注意,确保所有潜在目标 GUC 都处于 PGC_S_DEFAULT
	 * 也确保了 set_config_option 不会因来源优先级比较
	 * 拒绝设置它们。
	 */
	dlist_foreach_modify(iter, &guc_nondef_list)
	{
		struct config_generic *gconf = dlist_container(struct config_generic,
													   nondef_link, iter.cur);

		/* 不可传输或已在 PGC_S_DEFAULT 则不作为。 */
		if (can_skip_gucvar(gconf))
			continue;

		/*
		 * 可用 InitializeOneGUCOption 把 GUC 重置为
		 * 默认值,但先须释放所有已存在的附属数据,
		 * 以免泄漏内存。栈必须为空,但我们必须清理其余
		 * 的字段。注意可能有重复的值或 "extra" 指针。
		 * 还必须确保把它从所在的所有列表中取出。
		 *
		 */
		Assert(gconf->stack == NULL);
		guc_free(gconf->extra);
		guc_free(gconf->last_reported);
		guc_free(gconf->sourcefile);
		switch (gconf->vartype)
		{
			case PGC_BOOL:
				{
					struct config_bool *conf = (struct config_bool *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_INT:
				{
					struct config_int *conf = (struct config_int *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_REAL:
				{
					struct config_real *conf = (struct config_real *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_STRING:
				{
					struct config_string *conf = (struct config_string *) gconf;

					guc_free(*conf->variable);
					if (conf->reset_val && conf->reset_val != *conf->variable)
						guc_free(conf->reset_val);
					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
			case PGC_ENUM:
				{
					struct config_enum *conf = (struct config_enum *) gconf;

					if (conf->reset_extra && conf->reset_extra != gconf->extra)
						guc_free(conf->reset_extra);
					break;
				}
		}
		/* 从所在列表移除。 */
		RemoveGUCFromLists(gconf);
		/* 现在可把结构体重置回 PGS_S_DEFAULT。 */
		InitializeOneGUCOption(gconf);
	}

	/* 第一项是后续数据的长度 */
	memcpy(&len, gucstate, sizeof(len));

	srcptr += sizeof(len);
	srcend = srcptr + len;

	/* 如果 GUC 值检查失败,我们希望错误能显示有用的 */
	error_context_callback.callback = guc_restore_error_context_callback;
	error_context_callback.previous = error_context_stack;
	error_context_callback.arg = NULL;
	error_context_stack = &error_context_callback;

	/* 恢复列出的 GUC。 */
	while (srcptr < srcend)
	{
		int			result;
		char	   *error_context_name_and_value[2];

		varname = read_gucstate(&srcptr, srcend);
		varvalue = read_gucstate(&srcptr, srcend);
		varsourcefile = read_gucstate(&srcptr, srcend);
		if (varsourcefile[0])
			read_gucstate_binary(&srcptr, srcend,
								 &varsourceline, sizeof(varsourceline));
		else
			varsourceline = 0;
		read_gucstate_binary(&srcptr, srcend,
							 &varsource, sizeof(varsource));
		read_gucstate_binary(&srcptr, srcend,
							 &varscontext, sizeof(varscontext));
		read_gucstate_binary(&srcptr, srcend,
							 &varsrole, sizeof(varsrole));

		error_context_name_and_value[0] = varname;
		error_context_name_and_value[1] = varvalue;
		error_context_callback.arg = &error_context_name_and_value[0];
		result = set_config_option_ext(varname, varvalue,
									   varscontext, varsource, varsrole,
									   GUC_ACTION_SET, true, ERROR, true);
		if (result <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("parameter \"%s\" could not be set", varname)));
		if (varsourcefile[0])
			set_config_sourcefile(varname, varsourcefile, varsourceline);
		error_context_callback.arg = NULL;
	}

	error_context_stack = error_context_callback.previous;
}

/*
 * 一种"长参数"的简单模拟,不完全符合 GNU
 * 规范。它接受形如 "some-option=some value" 的字符串,
 * 并在 palloc 的存储中返回 name = "some_option"、
 * value = "some value"。注意选项名中的 '-' 会被
 * 转换为 '_'。若输入串无 '=',则 value 为 NULL。
 */
void
ParseLongOption(const char *string, char **name, char **value)
{
	size_t		equal_pos;
	char	   *cp;

	Assert(string);
	Assert(name);
	Assert(value);

	equal_pos = strcspn(string, "=");

	if (string[equal_pos] == '=')
	{
		*name = palloc(equal_pos + 1);
		strlcpy(*name, string, equal_pos + 1);

		*value = pstrdup(&string[equal_pos + 1]);
	}
	else
	{
		/* 字符串无等号 */
		*name = pstrdup(string);
		*value = NULL;
	}

	for (cp = *name; *cp; cp++)
		if (*cp == '-')
			*cp = '_';
}


/*
 * 处理取自 pg_db_role_setting.setconfig、
 * pg_proc.proconfig 等。调用方须指定正确上下文/来源/动作。
 *
 * 数组参数必须是 TEXT 数组(不得为 NULL)。
 */
void
ProcessGUCArray(ArrayType *array,
				GucContext context, GucSource source, GucAction action)
{
	int			i;

	Assert(array != NULL);
	Assert(ARR_ELEMTYPE(array) == TEXTOID);
	Assert(ARR_NDIM(array) == 1);
	Assert(ARR_LBOUND(array)[0] == 1);

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		bool		isnull;
		char	   *s;
		char	   *name;
		char	   *value;

		d = array_ref(array, 1, &i,
					  -1 /*变长数组*/ ,
					  -1 /*TEXT的typlen*/ ,
					  false /*TEXT的typbyval*/ ,
					  TYPALIGN_INT /*TEXT的typalign*/ ,
					  &isnull);

		if (isnull)
			continue;

		s = TextDatumGetCString(d);

		ParseLongOption(s, &name, &value);
		if (!value)
		{
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("could not parse setting for parameter \"%s\"",
							name)));
			pfree(name);
			continue;
		}

		(void) set_config_option(name, value,
								 context, source,
								 action, true, 0, false);

		pfree(name);
		pfree(value);
		pfree(s);
	}
}


/*
 * 向选项数组添加一个条目。数组参数可以为 NULL,
 * 以指示当前表条目为 NULL。
 */
ArrayType *
GUCArrayAdd(ArrayType *array, const char *name, const char *value)
{
	struct config_generic *record;
	Datum		datum;
	char	   *newval;
	ArrayType  *a;

	Assert(name);
	Assert(value);

	/* 测试选项是否有效、我们可否设置它 */
	(void) validate_option_array_item(name, value, false);

	/* 规范化名称(把过时 GUC 名转为现代拼写) */
	record = find_option(name, false, true, WARNING);
	if (record)
		name = record->name;

	/* 为数组建新条目 */
	newval = psprintf("%s=%s", name, value);
	datum = CStringGetTextDatum(newval);

	if (array)
	{
		int			index;
		bool		isnull;
		int			i;

		Assert(ARR_ELEMTYPE(array) == TEXTOID);
		Assert(ARR_NDIM(array) == 1);
		Assert(ARR_LBOUND(array)[0] == 1);

		index = ARR_DIMS(array)[0] + 1; /*添加到末尾*/

		for (i = 1; i <= ARR_DIMS(array)[0]; i++)
		{
			Datum		d;
			char	   *current;

			d = array_ref(array, 1, &i,
						  -1 /*变长数组*/ ,
						  -1 /*TEXT的typlen*/ ,
						  false /*TEXT的typbyval*/ ,
						  TYPALIGN_INT /*TEXT的typalign*/ ,
						  &isnull);
			if (isnull)
				continue;
			current = TextDatumGetCString(d);

			/* 检查匹配直至并包含 '=' */
			if (strncmp(current, newval, strlen(name) + 1) == 0)
			{
				index = i;
				break;
			}
		}

		a = array_set(array, 1, &index,
					  datum,
					  false,
					  -1 /* 变长数组 */ ,
					  -1 /*TEXT的typlen*/ ,
					  false /*TEXT的typbyval*/ ,
					  TYPALIGN_INT /*TEXT的typalign*/ );
	}
	else
		a = construct_array_builtin(&datum, 1, TEXTOID);

	return a;
}


/*
 * 从选项数组删除一个条目。数组参数可以为 NULL,
 * 以指示当前表条目为 NULL。此外,如果返回值为 NULL,
 * 则应存储一个空值。
 */
ArrayType *
GUCArrayDelete(ArrayType *array, const char *name)
{
	struct config_generic *record;
	ArrayType  *newarray;
	int			i;
	int			index;

	Assert(name);

	/* 测试选项是否有效、我们可否设置它 */
	(void) validate_option_array_item(name, NULL, false);

	/* 规范化名称(把过时 GUC 名转为现代拼写) */
	record = find_option(name, false, true, WARNING);
	if (record)
		name = record->name;

	/* 数组当前为 NULL 则肯定没有要删除项 */
	if (!array)
		return NULL;

	newarray = NULL;
	index = 1;

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		char	   *val;
		bool		isnull;

		d = array_ref(array, 1, &i,
					  -1 /*变长数组*/ ,
					  -1 /*TEXT的typlen*/ ,
					  false /*TEXT的typbyval*/ ,
					  TYPALIGN_INT /*TEXT的typalign*/ ,
					  &isnull);
		if (isnull)
			continue;
		val = TextDatumGetCString(d);

		/* 若条目正是要删的,则忽略 */
		if (strncmp(val, name, strlen(name)) == 0
			&& val[strlen(name)] == '=')
			continue;

		/* 否则加进输出数组 */
		if (newarray)
			newarray = array_set(newarray, 1, &index,
								 d,
								 false,
								 -1 /*变长数组*/ ,
								 -1 /*TEXT的typlen*/ ,
								 false /*TEXT的typbyval*/ ,
								 TYPALIGN_INT /*TEXT的typalign*/ );
		else
			newarray = construct_array_builtin(&d, 1, TEXTOID);

		index++;
	}

	return newarray;
}


/*
 * 给定一个 GUC 数组,删掉其中我们权限级别
 * 允许删除的全部设置:超级用户全删;普通用户
 * 只删 PGC_USERSET 的或有设置权限的
 */
ArrayType *
GUCArrayReset(ArrayType *array)
{
	ArrayType  *newarray;
	int			i;
	int			index;

	/* 数组当前为 NULL 则无事可做 */
	if (!array)
		return NULL;

	/* 若是超级用户则能删一切,直接删即可 */
	if (superuser())
		return NULL;

	newarray = NULL;
	index = 1;

	for (i = 1; i <= ARR_DIMS(array)[0]; i++)
	{
		Datum		d;
		char	   *val;
		char	   *eqsgn;
		bool		isnull;

		d = array_ref(array, 1, &i,
					  -1 /*变长数组*/ ,
					  -1 /*TEXT的typlen*/ ,
					  false /*TEXT的typbyval*/ ,
					  TYPALIGN_INT /*TEXT的typalign*/ ,
					  &isnull);
		if (isnull)
			continue;
		val = TextDatumGetCString(d);

		eqsgn = strchr(val, '=');
		*eqsgn = '\0';

		/* 若有删除权限,则跳过 */
		if (validate_option_array_item(val, NULL, true))
			continue;

		/* 否则加进输出数组 */
		if (newarray)
			newarray = array_set(newarray, 1, &index,
								 d,
								 false,
								 -1 /*变长数组*/ ,
								 -1 /*TEXT的typlen*/ ,
								 false /*TEXT的typbyval*/ ,
								 TYPALIGN_INT /*TEXT的typalign*/ );
		else
			newarray = construct_array_builtin(&d, 1, TEXTOID);

		index++;
		pfree(val);
	}

	return newarray;
}

/*
 * 为 GUCArrayAdd/Delete/Reset 校验提议的选项设置。
 *
 * name 是选项名。value 是 Add 情形下提议的值,
 * Delete/Reset 情形下则为 NULL。如果 skipIfNoPermissions
 * 为真,那么无权限设置该选项也不算错。
 *
 * 如果一切正常则返回真;如果 skipIfNoPermissions 为真且
 * 用户无权更改该选项则返回假(所有其他错误情形都会
 * 抛出错误)。
 */
static bool
validate_option_array_item(const char *name, const char *value,
						   bool skipIfNoPermissions)

{
	struct config_generic *gconf;
	bool		reset_custom;

	/*
	 * 有三种情形需要考虑:
	 *
	 * name 是已知 GUC 变量。正常检查值,正常检查
	 * 权限(即变量为 USERSET 时允许,或它是 SUSET
	 * 且用户为超级用户或持有 ACL_SET 权限)。
	 *
	 * name 未知,但已存在或可创建为占位符(即它
	 * 有有效的自定义名称)。若你是超级用户则允许,
	 * 否则不允许。假定超级用户知道自己
	 * 在做什么。不允许其他用户这样做,因为占位符
	 * 被解析时可能变成 SUSET 变量。(以目前可用的
	 * 基础设施,我们其实能在当前会话内处理
	 * 这类情形---但一旦在 pg_db_role_setting 中建立了
	 * 条目,即视为已完整校验。)
	 *
	 * name 未知且无法创建为占位符。抛出错误,除非
	 * skipIfNoPermissions 或 reset_custom 为真。如果
	 * reset_custom 为真,那么这是对带保留前缀的未知自
	 * 定义 GUC 的 RESET 或 RESET ALL 操作,此时我们想
	 * 继续按上一段描述的占位符情形处理(否则用户将
	 * 没有办法移除它们)。否则,返回假。
	 */
	reset_custom = (!value && valid_custom_variable_name(name));
	gconf = find_option(name, true, skipIfNoPermissions || reset_custom, ERROR);
	if (!gconf && !reset_custom)
	{
		/* 未知,创建占位符失败 */
		return false;
	}

	if (!gconf || gconf->flags & GUC_CUSTOM_PLACEHOLDER)
	{
		/*
		 * 我们无法对值做任何有意义的检查,因此只有权限
		 * 检查才有用。
		 */
		if (superuser() ||
			pg_parameter_aclcheck(name, GetUserId(), ACL_SET) == ACLCHECK_OK)
			return true;
		if (skipIfNoPermissions)
			return false;
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to set parameter \"%s\"", name)));
	}

	/* 手动做权限检查,从而避免抛出错误 */
	if (gconf->context == PGC_USERSET)
		 /*好*/ ;
	else if (gconf->context == PGC_SUSET &&
			 (superuser() ||
			  pg_parameter_aclcheck(name, GetUserId(), ACL_SET) == ACLCHECK_OK))
		 /*好*/ ;
	else if (skipIfNoPermissions)
		return false;
	/* 如果应当抛出权限错误,就让 set_config_option 来做 */

	/* 测试权限与有效的选项值 */
	(void) set_config_option(name, value,
							 superuser() ? PGC_SUSET : PGC_USERSET,
							 PGC_S_TEST, GUC_ACTION_SET, false, 0, false);

	return true;
}


/*
 * 由想要为检查钩子失败而覆盖常规
 * ERRCODE_INVALID_PARAMETER_VALUE SQLSTATE的check_hooks调用。
 *
 * 注意,GUC_check_errmsg() 等
 * 只是宏,其结果是对相关变量的直接赋值。这很丑陋,
 * 但受 C 宏机制限制所迫。
 */
void
GUC_check_errcode(int sqlerrcode)
{
	GUC_check_errcode_value = sqlerrcode;
}


/*
 * 用于管理调用变量 check_hook 的便利函数。
 * 它们主要负责让检查钩子在失败时提供部分
 * 错误报告内容的协议。
 */

static bool
call_bool_check_hook(struct config_bool *conf, bool *newval, void **extra,
					 GucSource source, int elevel)
{
	/* 无钩子时快速成功 */
	if (!conf->check_hook)
		return true;

	/* 重置可能被钩子设置的变量 */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": %d",
						conf->gen.name, (int) *newval),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* 清空 ErrorContext 中的字符串 */
		FlushErrorState();
		return false;
	}

	return true;
}

static bool
call_int_check_hook(struct config_int *conf, int *newval, void **extra,
					GucSource source, int elevel)
{
	/* 无钩子时快速成功 */
	if (!conf->check_hook)
		return true;

	/* 重置可能被钩子设置的变量 */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": %d",
						conf->gen.name, *newval),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* 清空 ErrorContext 中的字符串 */
		FlushErrorState();
		return false;
	}

	return true;
}

static bool
call_real_check_hook(struct config_real *conf, double *newval, void **extra,
					 GucSource source, int elevel)
{
	/* 无钩子时快速成功 */
	if (!conf->check_hook)
		return true;

	/* 重置可能被钩子设置的变量 */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": %g",
						conf->gen.name, *newval),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* 清空 ErrorContext 中的字符串 */
		FlushErrorState();
		return false;
	}

	return true;
}

static bool
call_string_check_hook(struct config_string *conf, char **newval, void **extra,
					   GucSource source, int elevel)
{
	volatile bool result = true;

	/* 无钩子时快速成功 */
	if (!conf->check_hook)
		return true;

	/*
	 * 如果 elevel 是 ERROR,或者 check_hook 自身抛出
	 * elog(不理想,但并不总能避免),必须确保我们不泄漏
	 * 已 malloc 的 newval。
	 */
	PG_TRY();
	{
		/* 重置可能被钩子设置的变量 */
		GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
		GUC_check_errmsg_string = NULL;
		GUC_check_errdetail_string = NULL;
		GUC_check_errhint_string = NULL;

		if (!conf->check_hook(newval, extra, source))
		{
			ereport(elevel,
					(errcode(GUC_check_errcode_value),
					 GUC_check_errmsg_string ?
					 errmsg_internal("%s", GUC_check_errmsg_string) :
					 errmsg("invalid value for parameter \"%s\": \"%s\"",
							conf->gen.name, *newval ? *newval : ""),
					 GUC_check_errdetail_string ?
					 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
					 GUC_check_errhint_string ?
					 errhint("%s", GUC_check_errhint_string) : 0));
			/* 清空 ErrorContext 中的字符串 */
			FlushErrorState();
			result = false;
		}
	}
	PG_CATCH();
	{
		guc_free(*newval);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}

static bool
call_enum_check_hook(struct config_enum *conf, int *newval, void **extra,
					 GucSource source, int elevel)
{
	/* 无钩子时快速成功 */
	if (!conf->check_hook)
		return true;

	/* 重置可能被钩子设置的变量 */
	GUC_check_errcode_value = ERRCODE_INVALID_PARAMETER_VALUE;
	GUC_check_errmsg_string = NULL;
	GUC_check_errdetail_string = NULL;
	GUC_check_errhint_string = NULL;

	if (!conf->check_hook(newval, extra, source))
	{
		ereport(elevel,
				(errcode(GUC_check_errcode_value),
				 GUC_check_errmsg_string ?
				 errmsg_internal("%s", GUC_check_errmsg_string) :
				 errmsg("invalid value for parameter \"%s\": \"%s\"",
						conf->gen.name,
						config_enum_lookup_by_value(conf, *newval)),
				 GUC_check_errdetail_string ?
				 errdetail_internal("%s", GUC_check_errdetail_string) : 0,
				 GUC_check_errhint_string ?
				 errhint("%s", GUC_check_errhint_string) : 0));
		/* 清空 ErrorContext 中的字符串 */
		FlushErrorState();
		return false;
	}

	return true;
}
