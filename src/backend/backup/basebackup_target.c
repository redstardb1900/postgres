/*-------------------------------------------------------------------------
 *
 * basebackup_target.c
 *	  基备份可被"定向"，即发往请求备份的客户端
 *	  以外之处。扩展还可定义新 target。本文件
 *	  含支持该功能的代码。
 *	  代码见下。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_target.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "backup/basebackup_target.h"
#include "utils/memutils.h"

typedef struct BaseBackupTargetType
{
	char	   *name;
	void	   *(*check_detail) (char *, char *);
	bbsink	   *(*get_sink) (bbsink *, void *);
} BaseBackupTargetType;

struct BaseBackupTargetHandle
{
	BaseBackupTargetType *type;
	void	   *detail_arg;
};

static void initialize_target_list(void);
static bbsink *blackhole_get_sink(bbsink *next_sink, void *detail_arg);
static bbsink *server_get_sink(bbsink *next_sink, void *detail_arg);
static void *reject_target_detail(char *target, char *target_detail);
static void *server_check_detail(char *target, char *target_detail);

static BaseBackupTargetType builtin_backup_targets[] =
{
	{
		"blackhole", reject_target_detail, blackhole_get_sink
	},
	{
		"server", server_check_detail, server_get_sink
	},
	{
		NULL
	}
};

static List *BaseBackupTargetTypeList = NIL;

/*
 * 新增基备份 target 类型。
 *
 * 供服务器扩展使用。
 */
void
BaseBackupAddTarget(char *name,
					void *(*check_detail) (char *, char *),
					bbsink *(*get_sink) (bbsink *, void *))
{
	BaseBackupTargetType *newtype;
	MemoryContext oldcontext;
	ListCell   *lc;

	/* 若 target 列表尚未初始化，先初始化。 */
	if (BaseBackupTargetTypeList == NIL)
		initialize_target_list();

	/* 在 target 类型列表中查找同名已有条目。 */
	foreach(lc, BaseBackupTargetTypeList)
	{
		BaseBackupTargetType *ttype = lfirst(lc);

		if (strcmp(ttype->name, name) == 0)
		{
			/*
			 * 找到了，故更新它。
			 *
			 * 对同一名字多次调用 BaseBackupAddTarget 多半
			 * 不是好主意；若发生了，这样处理似乎最
			 * 稳妥。
			 */
			ttype->check_detail = check_detail;
			ttype->get_sink = get_sink;
			return;
		}
	}

	/*
	 * 此处分配用 TopMemoryContext，确保所需数据不会
	 * 在脚下消失；这也是把 target 名拷到新分配内存块
	 * 的原因。
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	newtype = palloc(sizeof(BaseBackupTargetType));
	newtype->name = pstrdup(name);
	newtype->check_detail = check_detail;
	newtype->get_sink = get_sink;
	BaseBackupTargetTypeList = lappend(BaseBackupTargetTypeList, newtype);
	MemoryContextSwitchTo(oldcontext);
}

/*
 * 查找基备份 target 并校验 target_detail。
 *
 * 定义新备份 target 的扩展多半会定义匹配的新
 * bbsink 类型。target_detail 的校验可在此调用的
 * check_detail 例程中做，也可在 bbsink 构造函数中
 * 做（由 BaseBackupGetSink 调用）。多半是品味问题，
 * 但 check_detail 运行得稍早些。
 */
BaseBackupTargetHandle *
BaseBackupGetTargetHandle(char *target, char *target_detail)
{
	ListCell   *lc;

	/* 若 target 列表尚未初始化，先初始化。 */
	if (BaseBackupTargetTypeList == NIL)
		initialize_target_list();

	/* 在 target 类型列表中找匹配。 */
	foreach(lc, BaseBackupTargetTypeList)
	{
		BaseBackupTargetType *ttype = lfirst(lc);

		if (strcmp(ttype->name, target) == 0)
		{
			BaseBackupTargetHandle *handle;

			/* 找到 target。 */
			handle = palloc(sizeof(BaseBackupTargetHandle));
			handle->type = ttype;
			handle->detail_arg = ttype->check_detail(target, target_detail);

			return handle;
		}
	}

	/* 未找到该 target。 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("unrecognized target: \"%s\"", target)));

	/* 让编译器安静 */
	return NULL;
}

/*
 * 构造实现该备份 target 的 bbsink。
 *
 * 真正工作由 get_sink 完成，此处只需用正确参数
 * 调用它。check_detail 返回值原样传给 get_sink，
 * 以便两函数可通信；若不需要，check_detail 可
 * 直接返回 target_detail，交由 get_sink
 * 接着处理即可。
 *
 */
bbsink *
BaseBackupGetSink(BaseBackupTargetHandle *handle, bbsink *next_sink)
{
	return handle->type->get_sink(next_sink, handle->detail_arg);
}

/*
 * 预定义 target 类型载入 BaseBackupTargetTypeList。
 */
static void
initialize_target_list(void)
{
	BaseBackupTargetType *ttype = builtin_backup_targets;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	while (ttype->name != NULL)
	{
		BaseBackupTargetTypeList = lappend(BaseBackupTargetTypeList, ttype);
		++ttype;
	}
	MemoryContextSwitchTo(oldcontext);
}

/*
 * 通常 get_sink 应构造并返回实现该 target 的新
 * bbsink，但 blackhole target 只丢弃数据。虽可
 * 用只转发的 bbsink，但不添加 bbsink 更省事。
 *
 *
 */
static bbsink *
blackhole_get_sink(bbsink *next_sink, void *detail_arg)
{
	return next_sink;
}

/*
 * 创建实现服务器端备份的 bbsink。
 */
static bbsink *
server_get_sink(bbsink *next_sink, void *detail_arg)
{
	return bbsink_server_new(next_sink, detail_arg);
}

/*
 * 为不接受 detail 的 target 实现 target-detail
 * 检查.
 */
static void *
reject_target_detail(char *target, char *target_detail)
{
	if (target_detail != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("target \"%s\" does not accept a target detail",
						target)));

	return NULL;
}

/*
 * 为服务器端备份实现 target-detail 检查。
 *
 * target_detail 应为写入备份的目录名，但此处不
 * 检查。该检查以及必要的权限检查在
 * bbsink_server_new 中进行。
 */
static void *
server_check_detail(char *target, char *target_detail)
{
	if (target_detail == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("target \"%s\" requires a target detail",
						target)));

	return target_detail;
}
