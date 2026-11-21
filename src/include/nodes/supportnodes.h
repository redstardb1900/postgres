/*-------------------------------------------------------------------------
 *
 * supportnodes.h
 *	规划器支持函数的定义。
 *
 * 本文件定义规划器支持函数API:可挂到另一目标
 * 函数上 SQL 函数(通常用 C 写),向系统提供关于目标
 * 函数额外知识.当前能力都与规划使用目标函数的
 * 查询有关,将来扩展也可能增加由解析器或执行器
 * 调用的功能。
 * 见上。
 * 见上。
 *
 * 支持函数必须具有 SQL 签名
 *		supportfn(internal) returns internal
 * 参数是指向本文件所定义某 Node 类型的指针。
 * 结果通常也是 Node 指针,其类型取决于所调用能力。
 * 任何情形下,返回 NULL 指针
 * (即 PG_RETURN_POINTER(NULL),而非 PG_RETURN_NULL())
 * 表示支持函数对该请求帮不上忙。支持函数若不识别
 * 请求节点类型或不能处理给定情形,必须返回 NULL 指针
 * 而非失败;这便于将来扩展请求情形集合。
 * 见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/supportnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUPPORTNODES_H
#define SUPPORTNODES_H

#include "nodes/plannodes.h"

struct PlannerInfo;				/* 避免在此包含 pathnodes.h */
struct IndexOptInfo;
struct SpecialJoinInfo;
struct WindowClause;

/*
 * Simplify 请求允许支持函数在计划期简化对其目标函数
 * 的调用。例如未缩短参数允许长度的 varchar 长度强制
 * 可换成 RelabelType 节点,或 "x + 0" 可换成 "x"。
 * 在规划器常量折叠遍中调用,故可假定参数已简化。
 * 见上。
 * 见上。
 *
 * 规划器的 PlannerInfo "root" 通常不需要,但若必须取得
 * 给定节点树中 Vars 信息则可查阅.注意某些用法中
 * root 可能为 NULL。
 *
 * "fcall" 将是调用支持函数目标函数的 FuncExpr。
 * (即使原解析树节点是运算符调用也如此;为此合成
 * FuncExpr。)
 *
 * 结果应是语义等价的变换后节点树,若无法简化则为
 * NULL。切勿返回或修改 *fcall,因其并非真正单独分配
 * 的 Node。但可在结果树中使用 fcall->args 或其部分。
 * 见上。
 */
typedef struct SupportRequestSimplify
{
	NodeTag		type;

	struct PlannerInfo *root;	/* 规划器基础设施 */
	FuncExpr   *fcall;			/* 待简化的函数调用 */
} SupportRequestSimplify;

/*
 * Selectivity请求允许支持函数为出现在WHERE子句顶层
 * 函数提供选择率估计(故仅适用于返回布尔的函数)。
 * 见上。
 *
 * 输入参数与运算符限制/连接估计器相同,只是把两套 API
 * 统一成一种请求类型。细节见 clause_selectivity()。
 * 见上。
 *
 * 若能估计,把值写入 "selectivity" 字段并返回
 * SupportRequestSelectivity 节点地址;估计值须在 0 与 1
 * 之间(含)。无法估计则返回 NULL(规划器将回退到默认
 * 估计,传统上为 1/3)。
 * 见上。
 *
 * 若目标函数用作运算符实现,则本目的不用支持函数;
 * 改为查阅运算符的限制或连接估计器。
 * 见上。
 */
typedef struct SupportRequestSelectivity
{
	NodeTag		type;

	/* 输入字段: */
	struct PlannerInfo *root;	/* 规划器基础设施 */
	Oid			funcid;			/* 正在询问的函数 */
	List	   *args;			/* 预简化后的函数参数 */
	Oid			inputcollid;	/* 函数输入排序规则 */
	bool		is_join;		/* 是连接还是限制情形? */
	int			varRelid;		/* 若为限制,目标关系的 RTI */
	JoinType	jointype;		/* 若连接,外连类型 */
	struct SpecialJoinInfo *sjinfo; /* 若为外连接,连接信息 */

	/* 输出字段: */
	Selectivity selectivity;	/* 返回的选择率估计 */
} SupportRequestSelectivity;

/*
 * Cost请求允许支持函数提供目标函数执行代价估计.
 * 估计可含一次性(查询启动)分量与每次执行分量。
 * 估计*不应*包含求值目标函数参数的代价,仅目标函数
 * 本身。
 * 见上。
 *
 * "node" 参数通常是调用目标函数的解析节点。最简单
 * 情形是 FuncExpr,也可能是 OpExpr、DistinctExpr、
 * NullIfExpr 或 WindowFunc,将来还可能有其他。若函数
 * 不能假定其参数等价于调用节点所呈参数,则传 NULL;
 * 例如聚合支持函数等会发生。
 * 见上。
 * RowExpr 所用的逐列比较运算符。
 *
 * 若能估计,写入代价字段并返回 SupportRequestCost
 * 节点地址。无法估计则返回 NULL,规划器将依赖目标
 * 函数的 procost 字段。(注意:procost 会自动按
 * cpu_operator_cost 缩放,但 Cost 请求的输出不会;
 * 支持函数须自行适当缩放结果。)
 * 见上。
 */
typedef struct SupportRequestCost
{
	NodeTag		type;

	/* 输入字段: */
	struct PlannerInfo *root;	/* 规划器基础设施(可为 NULL) */
	Oid			funcid;			/* 正在询问的函数 */
	Node	   *node;			/* 调用函数的解析节点,或 NULL */

	/* 输出字段: */
	Cost		startup;		/* 启动代价 */
	Cost		per_tuple;		/* 每次求值代价 */
} SupportRequestCost;

/*
 * Rows 请求允许支持函数为其目标函数提供输出行数估计
 * (故仅适用于集合返回函数)。
 * 见上。
 *
 * "node" 参数是调用目标函数的解析节点;目前总是
 * FuncExpr 或 OpExpr。
 *
 * 若能估计,写入 rows 字段并返回 SupportRequestRows
 * 节点地址。无法估计则返回 NULL,规划器将依赖目标
 * 函数的 prorows
 * 字段
 */
typedef struct SupportRequestRows
{
	NodeTag		type;

	/* 输入字段: */
	struct PlannerInfo *root;	/* 规划器基础设施(可为 NULL) */
	Oid			funcid;			/* 正在询问的函数 */
	Node	   *node;			/* 调用函数的解析节点 */

	/* 输出字段: */
	double		rows;			/* 预期返回的行数 */
} SupportRequestRows;

/*
 * IndexCondition 请求允许支持函数基于本身不可索引的
 * 目标函数调用生成可直接索引条件。目标函数调用须
 * 出现在 WHERE 或 JOIN/ON 顶层,故仅适用于返回布尔的
 * 函数。
 * 见上.
 *
 * "node" 参数是调用目标函数的解析节点;目前总是
 * FuncExpr 或 OpExpr。仅当至少一个函数参数匹配索引
 * 列的变量或表达式时才调用。"indexarg" 标识匹配参数
 * (在节点 args 列表中的从零下标)。
 * 见上。
 *
 * 若可变换,返回可直接索引的条件表达式 List,否则
 * 返回 NULL。(用 List 是因有时生成多条可索引条件有用,
 * 例如带常量前缀的 LIKE 同时产生 >= 与 < 条件。)
 * 见上。
 *
 * "可直接索引"意指条件须能由索引机制直接执行。通常
 * 这意味着它是二元 OpExpr,左侧为索引列值、右侧为
 * 伪常量,且运算符属于索引列的操作符族。视索引类型
 * 还可能是 RowCompareExpr、ScalarArrayOpExpr、NullTest;
 * 还可能含RowCompareExpr,ScalarArrayOpExpr,NullTest,
 * 但对派生索引条件似乎较少用."伪常量"意指右侧表达式
 * 不得含易变函数,也不得含该索引所属表的 Var;用
 * is_pseudo_constant_for_index() 检查。(注意:若传入的
 * "node" 是 OpExpr,核心规划器已验证非索引键操作数是
 * 伪常量;但当 "node" 是 FuncExpr 时不检查,因其不知
 * 你会用函数的哪个参数作索引比较值。)
 * 见上。
 * 见上。
 *
 * 许多情形下可生成索引条件,但弱于函数条件本身;例如
 * 带常量前缀的 LIKE 可产生基于前缀的索引范围检查,但仍
 * 需执行 LIKE 验证模式其余部分.我们称这种索引条件为
 * "有损"。返回索引条件时,若有损应将 "lossy" 请求字段
 * 置真,若与函数结果精确等价则置假。核心代码会把该字段
 * 初始化为真,此为常见情形。
 * 见上。
 * 见上。
 *
 * 务必验证索引操作符族对你要生成条件是正确的.核心
 * 支持函数倾向用已知内置 opfamily 的 OID;扩展须更费力,
 * 因其 OID 不固定。扩展数据类型上索引一种可行办法是
 * 改验证索引 AM 的 OID,再假定你的类型只有一个相关
 * opclass,故 opfamily 必对。生成 OpExpr 节点也可能需要
 * 知道扩展数据类型 OID(常可对函数参数用 exprType())和
 * 运算符 OID(可用 get_opfamily_member 查找)。
 * 见上。
 * 见上。
 * 见上。
 */
typedef struct SupportRequestIndexCondition
{
	NodeTag		type;

	/* 输入字段: */
	struct PlannerInfo *root;	/* 规划器基础设施 */
	Oid			funcid;			/* 正在询问的函数 */
	Node	   *node;			/* 调用函数的解析节点 */
	int			indexarg;		/* 匹配 indexcol 的函数参数下标 */
	struct IndexOptInfo *index; /* 规划器关于目标索引信息 */
	int			indexcol;		/* 目标索引列下标(从 0 起) */
	Oid			opfamily;		/* 索引列的操作符族 */
	Oid			indexcollation; /* 索引列的排序规则 */

	/* 输出字段: */
	bool		lossy;			/* 若条件与函数调用精确等价则
								 * 置为假 */
} SupportRequestIndexCondition;

/* ----------
 * 为更高效执行任意单调递增和/或单调递减窗口函数,当
 * 规划器看到 OpExpr 限定在子查询中直接引用窗口函数时,
 * 支持把本结构传给窗口函数的 prosupport 函数。规划器
 * 遇到此情形时填充本结构并传给窗口函数的 prosupport,
 * 以便它评估给定 WindowFunc 是否:
 * 见上。
 * 见上。
 *
 * a) 单调递增,或
 * b) 单调递减,或
 * c) 既单调递增又单调递减,或
 * d) 以上皆非。
 *
 * 单调递增函数永不返回低于"先前调用"返回值值。单调
 * 递减永不返回高于先前调用的值.两者兼备的
 * 函数每次必须返回相同值。
 * 见上。
 * 见上。
 *
 * "先前调用"指对同分区中同一WindowFunc
 * 结构的先前调用。
 *
 * row_number() 是单调递增函数的例子。返回值在每个新
 * 分区会复位为 1。既增又减的例子是 COUNT(*) OVER ()。
 * 本例无 ORDER BY,分区内所有行是对等的且都在帧界内。
 * COUNT(*) OVER(ORDER BY a ROWS BETWEEN UNBOUNDED
 * PRECEDING AND UNBOUNDED FOLLOWING) 亦然。
 * 见上。
 * 见上。
 *
 * COUNT(*) OVER (ORDER BY a ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
 * 是单调递减函数的例子。
 *
 * 实现只需关心给定 WindowFunc 在单个分区内的单调性。
 * 见上。
 *
 * 输入:
 *	window_func 是被调用窗口函数的指针。
 *
 *	window_clause 指向 WindowClause 数据。支持函数可用它
 *	检查帧界等。
 *
 * 输出:
 *	monotonic 为给定输入窗口函数与窗口子句的
 *	MonotonicFunction 值。
 * ----------
 */
typedef struct SupportRequestWFuncMonotonic
{
	NodeTag		type;

	/* 输入字段: */
	WindowFunc *window_func;	/* 指向窗口函数数据的指针 */
	struct WindowClause *window_clause; /* 指向窗口子句数据的指针 */

	/* 输出字段: */
	MonotonicFunction monotonic;
} SupportRequestWFuncMonotonic;

/*
 * 某些 WindowFunc 行为可能不受 WindowClause.frameOptions
 * 某些变化影响。例如 row_number() 的实现使帧选项不改变
 * 返回的行号。若用 ROWS 而非 RANGE,nodeWindowAgg.c 工作
 * 更少,因无需检查对等行。由于默认帧选项含 RANGE,像
 * row_number() 这样的窗口函数可能想改成 ROW。
 * 见上。
 * 见上。
 *
 * 这里允许 WindowFunc 的支持函数决定可对所属
 * WindowClause 改动什么。目前只能改 frameOptions。
 * 将来或允许更多优化。
 * 见上。
 *
 * 支持函数须确保 frameOptions 的优化版不影响窗口函数
 * 结果。规划器负责仅在使用该 WindowClause 的所有
 * WindowFuncs 对优化后的 frameOptions 达成一致时才改。
 * 若某 WindowFunc 无支持函数,规划器不会改
 * WindowClause 的 frameOptions。
 * 见上。
 * 见上。
 *
 * 规划器在调用支持函数前设置 window_func 与
 * window_clause,以便支持函数可用这些字段。可能需要它们
 * 来判断哪些优化可行。
 * 见上。
 *
 * frameOptions 由规划器设为 WindowClause.frameOptions。
 * 仅当给定 WindowFunc 可能优化时,支持函数才应调整它。
 * 见上。
 */
typedef struct SupportRequestOptimizeWindowClause
{
	NodeTag		type;

	/* 输入字段: */
	WindowFunc *window_func;	/* 指向窗口函数数据的指针 */
	struct WindowClause *window_clause; /* 指向窗口子句数据的指针 */

	/* 输入/输出字段: */
	int			frameOptions;	/* 新的 frameOptions,若无可优化则
								 * 保持不动。 */
} SupportRequestOptimizeWindowClause;

#endif							/* SUPPORTNODES_H */
