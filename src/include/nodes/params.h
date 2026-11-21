/*-------------------------------------------------------------------------
 *
 * params.h
 *	查找 Param 节点所关联值的支持。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/params.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARAMS_H
#define PARAMS_H

/* 前向声明,以免包含其他头文件 */
struct Bitmapset;
struct ExprState;
struct Param;
struct ParseState;


/*
 *	ParamListInfo
 *
 *	ParamListInfo 结构用于把参数传入执行器
 *	供参数化计划使用。支持两种基本供值方式,
 *	即静态方式与动态方式。
 *
 *	静态方式下,逐参数数据存放在一个
 *	ParamExternData 数组中,附于 ParamListInfo 结构。
 *	数组每项给出要代入的值,对应一个
 *	PARAM_EXTERN 参数。其 paramid 取值
 *	从 1 到 numParams。
 *
 *	参数号通常连续,但仍允许
 *	ptype == InvalidOid 表示未用的数组项。
 *
 *	pflags 是标志字段。目前唯一使用的位是:
 *	PARAM_FLAG_CONST 告知规划器可把该参数
 *	当作常量(即只生成适用于该
 *	参数值的计划)
 *
 *	动态方式下,对参数值的一切访问都经由
 *	ParamListInfo 结构中的钩子函数。此时
 *	ParamExternData 数组通常不用且不分配;
 *	但 paramid 的合法范围仍是 1 到 numParams。
 *
 *	此结构其实是数组而非列表,仍保留
 *	旧 typedef 名,以免无谓改动代码。
 *
 *	有 3 个钩子函数可关联到 ParamListInfo
 *	结构:
 *
 *	若 paramFetch 非空,则调它取某 param ID 的
 *	ParamExternData,而不访问相应元素
 *	所在的 ParamExternData 数组。这也支持数组
 *	根本不存在的情形,以及数组中数据
 *	可能过时或惰性求值的情形。paramFetch 必须返回
 *	描述该 param ID 的 ParamExternData 结构地址;
 *	上文 ptype == InvalidOid 表示无效
 *	param ID 的约定仍适用。返回的结构可放在
 *	调用方提供的 workspace 中,或放在由
 *	paramFetch 钩子控制的存储里,若那样更方便。
 *	两种情形下该结构都不预期长期存活。
 *	若 speculative 为真,paramFetch 钩子取参数值时
 *	不应冒险出错,而应报告无效
 *	参数。
 *
 *	若 paramCompile 非空,则它控制 execExpr.c 为
 *	PARAM_EXTERN Param 节点编译什么;通常该钩子发出
 *	EEOP_PARAM_CALLBACK 步。从而可把不必要工作
 *	从已编译表达式中优化掉。
 *
 *	若 parserSetup 非空,则调用它重建
 *	原解析钩子,当查询需要重新解析或规划时。
 *	参数类型若会不时变化,这尤其有用,
 *	因为它可免去向解析器提供一份固定
 *	参数类型列表的需要。
 *
 *	注意 paramFetch 与 paramCompile 钩子实际收到的是
 *	ParamListInfo 结构的地址;因此它们能访问全部
 *	三个 arg 字段,而 paramFetchArg
 *	与 paramCompileArg 之别颇随意。
 */

#define PARAM_FLAG_CONST	0x0001	/* 参数是常量 */

typedef struct ParamExternData
{
	Datum		value;			/* 参数值 */
	bool		isnull;			/* 是否 NULL */
	uint16		pflags;			/* 标志位,见上 */
	Oid			ptype;			/* 参数数据类型,或 0 */
} ParamExternData;

typedef struct ParamListInfoData *ParamListInfo;

typedef ParamExternData *(*ParamFetchHook) (ParamListInfo params,
											int paramid, bool speculative,
											ParamExternData *workspace);

typedef void (*ParamCompileHook) (ParamListInfo params, struct Param *param,
								  struct ExprState *state,
								  Datum *resv, bool *resnull);

typedef void (*ParserSetupHook) (struct ParseState *pstate, void *arg);

typedef struct ParamListInfoData
{
	ParamFetchHook paramFetch;	/* 参数获取钩子 */
	void	   *paramFetchArg;
	ParamCompileHook paramCompile;	/* 参数编译钩子 */
	void	   *paramCompileArg;
	ParserSetupHook parserSetup;	/* 解析器钩子 */
	void	   *parserSetupArg;
	char	   *paramValuesStr; /* 出错用的参数单字符串 */
	int			numParams;		/* 表示的 Params 名义或最大个数 */

	/*
	 * 若提供了 paramFetch,params[] 长度可为 0;否则它
	 * 长度须为 numParams。
	 */
	ParamExternData params[FLEXIBLE_ARRAY_MEMBER];
}			ParamListInfoData;


/* ----------------
 *	ParamExecData
 *
 *	ParamExecData 项用于执行器内部参数
 *	即传入或传出子查询的值。其
 *	PARAM_EXEC Param 的 paramid 是从 0 起的下标,指向
 *	一组 ParamExecData 记录,由下列引用:
 *	es_param_exec_vals 或 ecxt_param_exec_vals
 *
 *	若 execPlan 不是 NULL,它指向需执行的 SubPlanState 节点
 *	以产生该值。这样做是为了能
 *	对 InitPlans 惰性求值(它们直到
 *	需要结果值才执行)。否则视为需要时
 *	有效。
 * ----------------
 */

typedef struct ParamExecData
{
	void	   *execPlan;		/* 应为 SubPlanState * */
	Datum		value;
	bool		isnull;
} ParamExecData;

/* ParamsErrorCallback 的参数类型 */
typedef struct ParamsErrorCbData
{
	const char *portalName;
	ParamListInfo params;
} ParamsErrorCbData;

/* 函数位于 src/backend/nodes/params.c */
extern ParamListInfo makeParamList(int numParams);
extern ParamListInfo copyParamList(ParamListInfo from);
extern Size EstimateParamListSpace(ParamListInfo paramLI);
extern void SerializeParamList(ParamListInfo paramLI, char **start_address);
extern ParamListInfo RestoreParamList(char **start_address);
extern char *BuildParamLogString(ParamListInfo params, char **knownTextValues,
								 int maxlen);
extern void ParamsErrorCallback(void *arg);

#endif							/* PARAMS_H */
