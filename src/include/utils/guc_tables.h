/*-------------------------------------------------------------------------
 *
 * guc_tables.h
 *		GUC 所用表的声明。
 *
 * 设计说明见 src/backend/utils/misc/README。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *	  src/include/utils/guc_tables.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GUC_TABLES_H
#define GUC_TABLES_H 1

#include "lib/ilist.h"
#include "utils/guc.h"

/*
 * GUC 支持下列类型的变量：
 */
enum config_type
{
	PGC_BOOL,
	PGC_INT,
	PGC_REAL,
	PGC_STRING,
	PGC_ENUM
};

union config_var_val
{
	bool		boolval;
	int			intval;
	double		realval;
	char	   *stringval;
	int			enumval;
};

/*
 * GUC 变量的实际值可含一块 malloc 的不透明结构
 * extra，由其 check_hook 创建、assign_hook 使用。
 */
typedef struct config_var_value
{
	union config_var_val val;
	void	   *extra;
} config_var_value;

/*
 * 用于在展示时组织全部运行时选项的分组。
 * 须与 config.sgml 中选项分类方式一致！
 */
enum config_group
{
	UNGROUPED,					/*  不在 pg_settings 中显示的选项  */
	FILE_LOCATIONS,
	CONN_AUTH_SETTINGS,
	CONN_AUTH_TCP,
	CONN_AUTH_AUTH,
	CONN_AUTH_SSL,
	RESOURCES_MEM,
	RESOURCES_DISK,
	RESOURCES_KERNEL,
	RESOURCES_VACUUM_DELAY,
	RESOURCES_BGWRITER,
	RESOURCES_ASYNCHRONOUS,
	WAL_SETTINGS,
	WAL_CHECKPOINTS,
	WAL_ARCHIVING,
	WAL_RECOVERY,
	WAL_ARCHIVE_RECOVERY,
	WAL_RECOVERY_TARGET,
	REPLICATION_SENDING,
	REPLICATION_PRIMARY,
	REPLICATION_STANDBY,
	REPLICATION_SUBSCRIBERS,
	QUERY_TUNING_METHOD,
	QUERY_TUNING_COST,
	QUERY_TUNING_GEQO,
	QUERY_TUNING_OTHER,
	LOGGING_WHERE,
	LOGGING_WHEN,
	LOGGING_WHAT,
	PROCESS_TITLE,
	STATS_MONITORING,
	STATS_CUMULATIVE,
	AUTOVACUUM,
	CLIENT_CONN_STATEMENT,
	CLIENT_CONN_LOCALE,
	CLIENT_CONN_PRELOAD,
	CLIENT_CONN_OTHER,
	LOCK_MANAGEMENT,
	COMPAT_OPTIONS_PREVIOUS,
	COMPAT_OPTIONS_CLIENT,
	ERROR_HANDLING_OPTIONS,
	PRESET_OPTIONS,
	CUSTOM_OPTIONS,
	DEVELOPER_OPTIONS
};

/*
 * 在未提交的事务性变更之前，保存变量状态的栈项
 * 见上。
 */
typedef enum
{
	/*  几乎就是 GucAction，但 SET+LOCAL 还需第四态  */
	GUC_SAVE,					/*  由函数 SET 选项引起的项  */
	GUC_SET,					/*  由普通 SET 引起的项  */
	GUC_LOCAL,					/*  由 SET LOCAL 引起的项  */
	GUC_SET_LOCAL				/*  由 SET 再 SET LOCAL 的项  */
} GucStackState;

typedef struct guc_stack
{
	struct guc_stack *prev;		/*  前一栈项，若有  */
	int			nest_level;		/*  我们入栈时的嵌套深度  */
	GucStackState state;		/*  见上枚举  */
	GucSource	source;			/*  先前值的来源  */
	/*  掩码值来源必为 PGC_S_SESSION，无需再存  */
	GucContext	scontext;		/*  设定先前值的上下文  */
	GucContext	masked_scontext;	/*  设定掩码值的上下文  */
	Oid			srole;			/*  设定先前值的角色  */
	Oid			masked_srole;	/*  设定掩码值的角色  */
	config_var_value prior;		/*  变量的先前值  */
	config_var_value masked;	/*  GUC_SET_LOCAL 项中的 SET 值  */
} GucStack;

/*
 * 适用于所有变量类型的通用字段
 *
 * 短描述应少于 80 字符。有些应用也会用长描述，
 * 并把它追加到短描述之后（以换行或 ". " 分隔）。
 * 见上。
 *
 * srole 是设定当前值的角色；值来自内部源或配置文件
 * 则为 BOOTSTRAP_SUPERUSERID。reset_srole 同理（通常
 * 为 BOOTSTRAP_SUPERUSERID，但不总是）。
 *
 * 当前因维护操作而受关注的变量，经 xxx_link
 * 链入各类列表。不具备所述属性的变量中这些链接
 * 字段未用/为垃圾。
 * 见上。
 *
 * 注意：sourcefile/sourceline 保存在此，不推入栈值，
 * 虽原则上活动值为会话或事务局部时它们属于某栈值。
 * 这是为免膨胀栈项。我们知道仅当 source == PGC_S_FILE
 * 时它们才相关。
 */
struct config_generic
{
	/*  常量字段，须在初值中正确设定：  */
	const char *name;			/*  变量名 - 必须 FIRST  */
	GucContext	context;		/*  设定该变量所需上下文  */
	enum config_group group;	/*  按功能组织变量  */
	const char *short_desc;		/*  本变量用途的短描述  */
	const char *long_desc;		/*  本变量用途的长描述  */
	int			flags;			/* flag bits, see guc.h */
	/*  变量字段，运行时初始化：  */
	enum config_type vartype;	/*  变量类型（仅启动时设定）  */
	int			status;			/*  状态位，见下  */
	GucSource	source;			/*  当前实际值的来源  */
	GucSource	reset_source;	/*  reset_value 的来源  */
	GucContext	scontext;		/*  设定当前值的上下文  */
	GucContext	reset_scontext; /*  设定重置值的上下文  */
	Oid			srole;			/*  设定当前值的角色  */
	Oid			reset_srole;	/*  设定重置值的角色  */
	GucStack   *stack;			/*  栈上先前值  */
	void	   *extra;			/*  当前实际值的 "extra" 指针  */
	dlist_node	nondef_link;	/*  来源异于 PGC_S_DEFAULT 的
								 * 变量列表链接  */
	slist_node	stack_link;		/*  具非空 stack 的变量之
								 * 链  */
	slist_node	report_link;	/*  status 置了
								 * GUC_NEEDS_REPORT 的变量链  */
	char	   *last_reported;	/*  若为 GUC_REPORT，上次发给
								 * 客户端的值(未发则 NULL)  */
	char	   *sourcefile;		/*  当前设定所来自文件(非配置
								 * 文件则 NULL)  */
	int			sourceline;		/*  源文件行号  */
};

/*  status 中的位值  */
#define GUC_IS_IN_FILE		0x0001	/* found it in config file */
/*
 * 注意：GUC_IS_IN_FILE 位是 ProcessConfigFile 的瞬时状态。
 * 勿假定其值在别处表示有用信息。
 */
#define GUC_PENDING_RESTART 0x0002	/*  变更值尚无法应用  */
#define GUC_NEEDS_REPORT	0x0004	/*  新值必须报告给客户端  */


/*  特定变量类型的 GUC 记录  */

struct config_bool
{
	struct config_generic gen;
	/*  常量字段，须在初值中正确设定：  */
	bool	   *variable;
	bool		boot_val;
	GucBoolCheckHook check_hook;
	GucBoolAssignHook assign_hook;
	GucShowHook show_hook;
	/*  变量字段，运行时初始化：  */
	bool		reset_val;
	void	   *reset_extra;
};

struct config_int
{
	struct config_generic gen;
	/*  常量字段，须在初值中正确设定：  */
	int		   *variable;
	int			boot_val;
	int			min;
	int			max;
	GucIntCheckHook check_hook;
	GucIntAssignHook assign_hook;
	GucShowHook show_hook;
	/*  变量字段，运行时初始化：  */
	int			reset_val;
	void	   *reset_extra;
};

struct config_real
{
	struct config_generic gen;
	/*  常量字段，须在初值中正确设定：  */
	double	   *variable;
	double		boot_val;
	double		min;
	double		max;
	GucRealCheckHook check_hook;
	GucRealAssignHook assign_hook;
	GucShowHook show_hook;
	/*  变量字段，运行时初始化：  */
	double		reset_val;
	void	   *reset_extra;
};

/*
 * 关于字符串 GUCs 的说明：允许 boot_val 为 NULL，这会使
 * reset_val 与实际变量值 (*variable) 也为 NULL。但之后无法
 * 用 set_config_option 或任何其他 GUC API 再设为 NULL。
 * 此外 SHOW 等 GUC APIs 会把 NULL 显示为空串。选用 NULL
 * boot_val 的调用方应在启动稍后覆盖该设定，或小心确保
 * NULL 与空串在语义上无明显差别。
 * 见上。
 * 见上.
 */
struct config_string
{
	struct config_generic gen;
	/*  常量字段，须在初值中正确设定：  */
	char	  **variable;
	const char *boot_val;
	GucStringCheckHook check_hook;
	GucStringAssignHook assign_hook;
	GucShowHook show_hook;
	/*  变量字段，运行时初始化：  */
	char	   *reset_val;
	void	   *reset_extra;
};

struct config_enum
{
	struct config_generic gen;
	/*  常量字段，须在初值中正确设定：  */
	int		   *variable;
	int			boot_val;
	const struct config_enum_entry *options;
	GucEnumCheckHook check_hook;
	GucEnumAssignHook assign_hook;
	GucShowHook show_hook;
	/*  变量字段，运行时初始化：  */
	int			reset_val;
	void	   *reset_extra;
};

/*  对应上方及 guc.h 中枚举的常量表  */
extern PGDLLIMPORT const char *const config_group_names[];
extern PGDLLIMPORT const char *const config_type_names[];
extern PGDLLIMPORT const char *const GucContext_Names[];
extern PGDLLIMPORT const char *const GucSource_Names[];

/*  定义全部内建 GUC 变量的数据数组  */
extern PGDLLIMPORT struct config_bool ConfigureNamesBool[];
extern PGDLLIMPORT struct config_int ConfigureNamesInt[];
extern PGDLLIMPORT struct config_real ConfigureNamesReal[];
extern PGDLLIMPORT struct config_string ConfigureNamesString[];
extern PGDLLIMPORT struct config_enum ConfigureNamesEnum[];

/*  查找 GUC 变量，返回 config_generic 指针  */
extern struct config_generic *find_option(const char *name,
										  bool create_placeholders,
										  bool skip_errors,
										  int elevel);
extern struct config_generic **get_explain_guc_options(int *num);

/*  取得变量的字符串值 */
extern char *ShowGUCOption(struct config_generic *record, bool use_units);

/*  取得当前用户是否可见该 GUC 变量  */
extern bool ConfigOptionIsVisible(struct config_generic *conf);

/*  取得当前变量集合  */
extern struct config_generic **get_guc_variables(int *num_vars);

extern void build_guc_variables(void);

/* search in enum options */
extern const char *config_enum_lookup_by_value(struct config_enum *record, int val);
extern bool config_enum_lookup_by_name(struct config_enum *record,
									   const char *value, int *retval);
extern char *config_enum_get_options(struct config_enum *record,
									 const char *prefix,
									 const char *suffix,
									 const char *separator);

#endif							/* GUC_TABLES_H */
