/*-------------------------------------------------------------------------
 *
 * portal.h
 *	  POSTGRES 门户定义。
 *
 * 门户是抽象，表示正在运行或可运行查询的执行状
 * 态。门户既支持 SQL 级 CURSORs，也支持协议级门户。
 * 见上。
 *
 * 滚动（非顺序访问）与挂起执行仅允许含单个 SELECT
 * 类查询的门户。我们不希望客户端中途挂起更新类查
 * 询！因查询重写器不允许任意 ON SELECT 重写规则，
 * 只有原本为更新类的查询才可能产生多计划树；故
 * 限制为单查询在实践中不成问题。
 * 见上。
 * 见上。
 *
 * 对 SQL 游标，我们支持三种滚动行为：
 *
 * (1) 既未指定 NO SCROLL 也未指定 SCROLL：为保持
 *	   向后兼容，此处允许向后取数，除非会带来
 *	   额外运行时开销。
 *
 * (2) 指定了 NO SCROLL：不允许任何向后取数。
 *
 * (3) 指定了 SCROLL：允许各种向后取数，即使需付出
 *	   性能代价。（规划器必要时会在计划顶加
 *	   Materialize 节点。）
 *
 * 情形 #1 通过查看查询本身并判定能否无额外开销地
 * 支持滚动，转换为 #2 或 #3。
 * 见上。
 *
 * 协议级门户无非顺序取数 API，故区分对其无关。
 * 它们总是初始化为看起来像 NO SCROLL 游标。
 * 见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/portal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PORTAL_H
#define PORTAL_H

#include "datatype/timestamp.h"
#include "executor/execdesc.h"
#include "tcop/cmdtag.h"
#include "utils/plancache.h"
#include "utils/resowner.h"

/*
 * 按要执行的查询，门户有几种执行策略。（注意：
 * 各情形下门户只执行单条源 SQL，因而从用户视角
 * 只产生一个结果。但规则重写器可能把该源查询
 * 展开为零或多条实际查询。）
 * 见上。
 *
 * PORTAL_ONE_SELECT：门户含单个 SELECT。我们按需
 * 增量运行执行器。此策略也支持可保持游标（执行器
 * 结果可倾入 tuplestore，供事务结束后访问）。
 * 见上。
 *
 * PORTAL_ONE_RETURNING：门户含带 RETURNING 的单个
 * INSERT/UPDATE/DELETE（加规则重写可能加的辅助查询）。
 * 首次执行时跑完门户，把主查询结果倾入门户
 * tuplestore；再按需返回给客户端。（勿支持中途
 * 挂起，因 AFTER TRIGGER 代码应付不了，也不愿冒
 * 未执行全部辅助查询的风险。）
 * 见上。
 * 见上。
 *
 * PORTAL_ONE_MOD_WITH：门户含单个 SELECT，但有改
 * 数据的 CTEs。因可能需触发器，当前与
 * PORTAL_ONE_RETURNING 同样处理。将来或更像
 * PORTAL_ONE_SELECT。
 *
 * PORTAL_UTIL_SELECT：门户含返回类 SELECT 结果的
 * 实用语句（如 EXPLAIN 或 SHOW）。首次执行时跑该
 * 语句并把结果倾入门户 tuplestore；再按需返回给
 * 客户端。
 *
 * PORTAL_MULTI_QUERY：其余情形。此处不支持部分执
 * 行：门户查询将在首次调用时跑完。
 */
typedef enum PortalStrategy
{
	PORTAL_ONE_SELECT,
	PORTAL_ONE_RETURNING,
	PORTAL_ONE_MOD_WITH,
	PORTAL_UTIL_SELECT,
	PORTAL_MULTI_QUERY
} PortalStrategy;

/*
 * 门户总处于下列状态之一。若查询未跑完，可从
 * ACTIVE 回到 READY；否则状态从不回退。
 * 见上。
 */
typedef enum PortalStatus
{
	PORTAL_NEW,					/* 刚创建 */
	PORTAL_DEFINED,				/* DefineQuery 完成 */
	PORTAL_READY,				/* PortalStart 完成，可运行 */
	PORTAL_ACTIVE,				/* 门户正在运行（勿删除） */
	PORTAL_DONE,				/* 门户已结束（勿再运行） */
	PORTAL_FAILED				/* 门户出错（勿再运行） */
} PortalStatus;

typedef struct PortalData *Portal;

typedef struct PortalData
{
	/* 簿记数据 */
	const char *name;			/* 门户名 */
	const char *prepStmtName;	/* 源预备语句（无则为 NULL） */
	MemoryContext portalContext;	/* 门户的附属内存 */
	ResourceOwner resowner;		/* 门户拥有的资源 */
	void		(*cleanup) (Portal portal); /* 清理钩子 */

	/*
	 * 记住门户创建或使用于哪些 subtransaction 的
	 * 状态数据。若门户从上笔事务保持过来，两
	 * InvalidSubTransactionId。否则 createSubid 为创
	 * 建子事务，activeSubid 为最近运行该门户的子事务。
	 * 见上。
	 */
	SubTransactionId createSubid;	/* 创建子事务 */
	SubTransactionId activeSubid;	/* 最近有活动的子事务 */
	int			createLevel;	/* 创建子事务的嵌套层级 */

	/* 门户将执行的查询 */
	const char *sourceText;		/* 查询文本（自 8.4 起非 NULL） */
	CommandTag	commandTag;		/* 原查询的命令标签 */
	QueryCompletion qc;			/* 已执行查询的命令完成数据 */
	List	   *stmts;			/* PlannedStmts 列表 */
	CachedPlan *cplan;			/* CachedPlan(stmts 来自其一) */

	ParamListInfo portalParams; /* 传给查询的参数 */
	QueryEnvironment *queryEnv; /* 查询环境 */

	/* 特性/选项 */
	PortalStrategy strategy;	/* 见上 */
	int			cursorOptions;	/* DECLARE CURSOR 选项位 */
	bool		run_once;		/* 未用 */

	/* 状态 */
	PortalStatus status;		/* 见上 */
	bool		portalPinned;	/* 钉住的门户勿丢弃 */
	bool		autoHeld;		/* 曾自动从钉住转为保持
								 * 见 HoldPinnedPortals() */

	/* 若非 NULL，执行器活跃；最终须调 ExecutorEnd： */
	QueryDesc  *queryDesc;		/* 调用执行器所需信息 */

	/* 若门户返回元组，这是其 tupdesc： */
	TupleDesc	tupDesc;		/* 结果元组的描述符 */
	/* 以及各列要用的格式码： */
	int16	   *formats;		/* 每列一个格式码 */

	/*
	 * 执行门户查询时最外层 ActiveSnapshot。除少数
	 * 实用命令外，我们需要存在该快照。这确保结果中
	 * 的 TOAST 引用可 detoast，并有助于减少进程暴露
	 * xmin 的抖动。
	 */
	Snapshot	portalSnapshot; /* 活动快照，无则为 NULL */

	/*
	 * 对保持游标或 PORTAL_ONE_RETURNING、
	 * PORTAL_ONE_MOD_WITH、PORTAL_UTIL_SELECT 存放
	 * 元组处。（跨事务保持游标无活跃执行器状态。）
	 */
	Tuplestorestate *holdStore; /* 可保持游标的存储 */
	MemoryContext holdContext;	/* 含 holdStore 的内存 */

	/*
	 * 读取 holdStore 中元组时所用快照。若元组可能含
	 * TOAST 引用，必须保持对该快照的引用，因释放快照
	 * 可能让刚死行被 vacuum，连同其 toast 数据。对保
	 * 持游标，我们强制 detoast 以免需保持该快照。
	 * 见上。
	 * 见上。
	 */
	Snapshot	holdSnapshot;	/* 已注册快照，无则为 NULL */

	/*
	 * atStart、atEnd 与 portalPos 指示当前游标位置。
	 * portalPos 在首行前为零，取到第 N 行后为 N。跑过
	 * 末尾后 portalPos 等于查询行数，且 atEnd 为真。注意
	 * atStart 蕴含 portalPos == 0，反之不然：可能只回退
	 * 到首行而非起点。另注意各处代码会检查 atStart 与
	 * atEnd，但只有门户移动例程应触碰 portalPos。
	 * 见上。
	 */
	bool		atStart;
	bool		atEnd;
	uint64		portalPos;

	/* 展示数据，主要由 pg_cursors 系统视图使用 */
	TimestampTz creation_time;	/* 定义此门户的时间 */
	bool		visible;		/* 是否列入 pg_cursors？ */
}			PortalData;

/*
 * PortalIsValid
 *		门户有效则为真。
 */
#define PortalIsValid(p) PointerIsValid(p)


/* utils/mmgr/portalmem.c 中函数的原型 */
extern void EnablePortalManager(void);
extern bool PreCommit_Portals(bool isPrepare);
extern void AtAbort_Portals(void);
extern void AtCleanup_Portals(void);
extern void PortalErrorCleanup(void);
extern void AtSubCommit_Portals(SubTransactionId mySubid,
								SubTransactionId parentSubid,
								int parentLevel,
								ResourceOwner parentXactOwner);
extern void AtSubAbort_Portals(SubTransactionId mySubid,
							   SubTransactionId parentSubid,
							   ResourceOwner myXactOwner,
							   ResourceOwner parentXactOwner);
extern void AtSubCleanup_Portals(SubTransactionId mySubid);
extern Portal CreatePortal(const char *name, bool allowDup, bool dupSilent);
extern Portal CreateNewPortal(void);
extern void PinPortal(Portal portal);
extern void UnpinPortal(Portal portal);
extern void MarkPortalActive(Portal portal);
extern void MarkPortalDone(Portal portal);
extern void MarkPortalFailed(Portal portal);
extern void PortalDrop(Portal portal, bool isTopCommit);
extern Portal GetPortalByName(const char *name);
extern void PortalDefineQuery(Portal portal,
							  const char *prepStmtName,
							  const char *sourceText,
							  CommandTag commandTag,
							  List *stmts,
							  CachedPlan *cplan);
extern PlannedStmt *PortalGetPrimaryStmt(Portal portal);
extern void PortalCreateHoldStore(Portal portal);
extern void PortalHashTableDeleteAll(void);
extern bool ThereAreNoReadyPortals(void);
extern void HoldPinnedPortals(void);
extern void ForgetPortalSnapshots(void);

#endif							/* PORTAL_H */
