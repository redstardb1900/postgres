/*-------------------------------------------------------------------------
 *
 * nodes.h
 *	带标签节点的定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/nodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODES_H
#define NODES_H

/*
 * 每个节点的第一字段是 NodeTag。用 makeNode 创建的节点
 * 其第一字段必为下列标签之一。
 *
 * 注插入或删除节点类型会改变列表后面其他节点类型
 * 编号。开发期无妨,因节点号从不落盘。但勿在已发布
 * 分支上这么做,那会对扩展造成 ABI
 * 破坏。
 */
typedef enum NodeTag
{
	T_Invalid = 0,

#include "nodes/nodetags.h"
} NodeTag;

/*
 * pg_node_attr() - 在节点定义中为 gen_node_support.pl
 * 设置额外信息
 *
 * 属性可挂在整个节点上(写在结构开括号后的第一行)
 * 或挂在特定字段上(写在该字段行末)。参数是
 * 逗号分隔的属性列表。无法识别的属性会
 * 报错。
 * 见上。
 *
 * 合法的节点属性:
 *
 * - abstract: 抽象类型不可实例化,但可作为其他类型的
 *   超类型。我们跟踪其字段供子类型使用,但不发出
 *   节点标签,故不能实例化它们。
 * 见上。
 *
 * - custom_copy_equal: 在 copyfuncs.c 与 equalfuncs.c 中有
 *   定制实现.
 *
 * - custom_read_write: 在 outfuncs.c 与 readfuncs.c 中有
 * 定制实现.
 *
 * - custom_query_jumble: 在 queryjumblefuncs.c 中有定制实现。
 *
 * - no_copy: 完全不支持 copyObject()。
 *
 * - no_equal: 完全不支持 equal()。
 *
 * - no_copy_equal: no_copy 与 no_equal 的简写。
 *
 * - no_query_jumble: 完全不支持 JumbleQuery()。
 *
 * - no_read: 完全不支持 nodeRead()。
 *
 * - nodetag_only: 不支持 copyObject()、equal()、jumbleQuery()、
 * outNode()/nodeRead().
 *
 * - special_read_write: 在 outNode() 与 nodeRead() 中有特殊处理。
 *
 * - nodetag_number(VALUE): 指定 nodetag 编号而非自动生成。
 *   通常仅用于稳定分支,给新节点类型编号且不因改变
 *   既有节点类型编号而破坏 ABI。
 * 见上。
 *
 * 无论是否标 abstract,节点类型都可作其他类型的超类型:
 * 若某节点结构作为另一结构类型第一字段出现,则它是
 * 该类型的超类型。no_copy、no_equal、no_query_jumble 与
 * no_read 属性自动从超类型继承。(注意 nodetag_only
 * 不继承,故不完全等价于其他属性的组合。)
 * 见上。
 *
 * 合法的节点字段属性:
 *
 * - array_size(OTHERFIELD): 本字段是动态分配数组,大小由
 *   所述另一字段指示。另一字段是标量或列表,
 *   后者则用列表长度。
 *
 * - copy_as(VALUE): 在 copyObject() 中用 VALUE 替换字段值。
 *
 * - copy_as_scalar: 在 copyObject() 中把字段当标量拷贝
 *   (如指针),即使它是节点类型指针。
 *
 * - equal_as_scalar: 在 equal() 中把字段当标量比较,
 *   即使它是节点类型指针。
 *
 * - equal_ignore: 相等时忽略字段.
 *
 * - equal_ignore_if_zero: 字段为零时忽略相等性比较。
 *   (否则正常比较。)
 *
 * - query_jumble_ignore: 查询混淆时忽略该字段。注意
 *   typmod 与排序规则信息通常与查询混淆
 *   无关。
 *
 * - query_jumble_location: 标为要跟踪的位置字段。仅允许
 *   名称含 location 的整型字段。
 *
 * - read_as(VALUE): 在 nodeRead() 中用 VALUE 替换字段值。
 *
 * - read_write_ignore: 读写时忽略该字段。仅当节点类型
 *   标了 no_read 或同时指定了 read_as() 时允许。
 *
 * - write_only_relids, write_only_nondefault_pathtarget, write_only_req_outer:
 *   Path 结构的特殊处理;见该处。
 *
 */
#define pg_node_attr(...)

/*
 * 任意类型节点的第一字段保证是 NodeTag。
 * 故可通过转成 Node 取得任意节点的类型。把变量声明为
 * Node *(而非 void *) 也便于调试。
 * 见上。
 */
typedef struct Node
{
	NodeTag		type;
} Node;

#define nodeTag(nodeptr)		(((const Node*)(nodeptr))->type)

/*
 * newNode -
 *	  按指定大小创建新节点并用指定标签
 *	标记该节点.
 *
 * !警告!: 勿直接用 newNode。应使用宏 makeNode。
 *	  例如创建 Query 节点用 makeNode(Query)
 *
 * 注意: size 参数应恒为编译期常量,以便
 * 多重求值的表面风险在实践中无妨。
 */
#ifdef __GNUC__

/* 用 GCC 时可在表达式中使用复合语句 */
#define newNode(size, tag) \
({	Node   *_result; \
	AssertMacro((size) >= sizeof(Node));		/* 至少需要标签 */ \
	_result = (Node *) palloc0fast(size); \
	_result->type = (tag); \
	_result; \
})
#else

/*
 *	无法解引用 palloc 所得指针既赋值标签又返回指针
 *	本身,故需要一个持有变量。
 *	幸好此宏不递归,我们只为此定义一个
 *	全局变量。
 */
extern PGDLLIMPORT Node *newNodeMacroHolder;

#define newNode(size, tag) \
( \
	AssertMacro((size) >= sizeof(Node)),		/* 至少需要标签 */ \
	newNodeMacroHolder = (Node *) palloc0fast(size), \
	newNodeMacroHolder->type = (tag), \
	newNodeMacroHolder \
)
#endif							/* __GNUC__ */


#define makeNode(_type_)		((_type_ *) newNode(sizeof(_type_),T_##_type_))
#define NodeSetTag(nodeptr,t)	(((Node*)(nodeptr))->type = (t))

#define IsA(nodeptr,_type_)		(nodeTag(nodeptr) == T_##_type_)

/*
 * castNode(type, ptr) casts ptr to "type *", and if assertions are enabled,
 * verifies that the node has the appropriate type (using its nodeTag()).
 *
 * Use an inline function when assertions are enabled, to avoid multiple
 * evaluations of the ptr argument (which could e.g. be a function call).
 */
#ifdef USE_ASSERT_CHECKING
static inline Node *
castNodeImpl(NodeTag type, void *ptr)
{
	Assert(ptr == NULL || nodeTag(ptr) == type);
	return (Node *) ptr;
}
#define castNode(_type_, nodeptr) ((_type_ *) castNodeImpl(T_##_type_, nodeptr))
#else
#define castNode(_type_, nodeptr) ((_type_ *) (nodeptr))
#endif							/* USE_ASSERT_CHECKING */


/* ----------------------------------------------------------------
 *	随后为外部声明
 * ----------------------------------------------------------------
 */

/*
 * nodes/{outfuncs.c,print.c}
 */
struct Bitmapset;				/* 此处不包含 bitmapset.h */
struct StringInfoData;			/* 此处不包含 stringinfo.h */

extern void outNode(struct StringInfoData *str, const void *obj);
extern void outToken(struct StringInfoData *str, const char *s);
extern void outBitmapset(struct StringInfoData *str,
						 const struct Bitmapset *bms);
extern void outDatum(struct StringInfoData *str, uintptr_t value,
					 int typlen, bool typbyval);
extern char *nodeToString(const void *obj);
extern char *bmsToString(const struct Bitmapset *bms);

/*
 * nodes/{readfuncs.c,read.c}
 */
extern void *stringToNode(const char *str);
#ifdef WRITE_READ_PARSE_PLAN_TREES
extern void *stringToNodeWithLocations(const char *str);
#endif
extern struct Bitmapset *readBitmapset(void);
extern uintptr_t readDatum(bool typbyval);
extern bool *readBoolCols(int numCols);
extern int *readIntCols(int numCols);
extern Oid *readOidCols(int numCols);
extern int16 *readAttrNumberCols(int numCols);

/*
 * nodes/copyfuncs.c
 */
extern void *copyObjectImpl(const void *from);

/* 若编译器支持,把结果转回实参类型 */
#ifdef HAVE_TYPEOF
#define copyObject(obj) ((typeof(obj)) copyObjectImpl(obj))
#else
#define copyObject(obj) copyObjectImpl(obj)
#endif

/*
 * nodes/equalfuncs.c
 */
extern bool equal(const void *a, const void *b);


/*
 * 用于标识限定词选择率与计划代价的类型定义。
 * 其实就是普通 double,但把变量声明为 Selectivity
 * 或 Cost 可使意图更明显。
 *
 * 本可放进 plannodes.h 之类,但许多文件依赖它们...
 * 见上。
 */
typedef double Selectivity;		/* 限定词将放过的元组比例 */
typedef double Cost;			/* 执行代价(以页访问为单位) */
typedef double Cardinality;		/* (估计)行数或其他整数
								 * 数 */


/*
 * CmdType -
 *	  Query 或 PlannedStmt 所表示操作类型的枚举
 *
 * pathnodes.h 与 plannodes.h 都需要,故放此处...
 */
typedef enum CmdType
{
	CMD_UNKNOWN,
	CMD_SELECT,					/* select stmt */
	CMD_UPDATE,					/* update stmt */
	CMD_INSERT,					/* insert stmt */
	CMD_DELETE,					/* delete stmt */
	CMD_MERGE,					/* merge stmt */
	CMD_UTILITY,				/* create/destroy/copy/vacuum等命令,
								 * 等 */
	CMD_NOTHING					/* 带qual的空操作规则之
								 * 哑命令 */
} CmdType;


/*
 * JoinType -
 *	  关系连接类型的枚举
 *
 * JoinType 决定用匹配限定连接两关系时的精确语义。
 * 例如说明对在另一关系中无匹配的元组做何处理。
 * 见上。
 *
 * pathnodes.h 与 plannodes.h 都需要,故放此处...
 */
typedef enum JoinType
{
	/*
	 * 按 SQL JOIN 语法的规范连接种类。解析器输出中只能
	 * 出现这些码(如 JoinExpr 节点)。
	 */
	JOIN_INNER,					/* 仅匹配的元组对 */
	JOIN_LEFT,					/* 匹配对+未匹配LHS元组 */
	JOIN_FULL,					/* 匹配对+未匹配LHS+未匹配RHS */
	JOIN_RIGHT,					/* 匹配对+未匹配RHS元组 */

	/*
	 * 半连接与反半连接(按关系理论定义)不出现在 SQL JOIN
	 * 语法中,但有标准惯用法为它们(如 EXISTS).规划器
	 * 识别这些情形并转为连接。故规划器与执行器必须
	 * 支持这些码。注意: JOIN_SEMI 输出中未指定与哪行
	 * 匹配的 RHS 行连接。JOIN_ANTI 输出中该行保证
	 * 被空扩展。
	 * 见上。
	 */
	JOIN_SEMI,					/* 每有匹配的 LHS 行各 1 份 */
	JOIN_ANTI,					/* 每无匹配的 LHS 行各 1 份 */
	JOIN_RIGHT_ANTI,			/* 每无匹配的 RHS 行各 1 份 */

	/*
	 * 这些码供规划器内部使用,执行器不支持
	 * (规划器大部分也不支持)。
	 */
	JOIN_UNIQUE_OUTER,			/* LHS 路径必须做成唯一 */
	JOIN_UNIQUE_INNER			/* RHS 路径必须做成唯一 */

	/*
	 * 将来可能需要更多连接类型。
	 */
} JoinType;

/*
 * 外连接是那些下推限定须与连接自身限定行为不同的
 * 连接。实际上除 INNER 与 SEMI 外皆是。但本宏也须
 * 排除 JOIN_UNIQUE 符号,因其是最终将成为 INNER
 * 连接的临时代理。
 * join.
 *
 * 注意: 半连接是混合情形,我们选择不视其为外连接。
 * 之所以可行,主要因 SQL 语法使下推限定无法引用
 * 半连接的内关系;故无需强分连接限定与下推限定。
 * 这很方便,因对几乎所有目的,挂在半连接上的限定
 * 都可与内连接限定同样处理。
 * 见上。
 */
#define IS_OUTER_JOIN(jointype) \
	(((1 << (jointype)) & \
	  ((1 << JOIN_LEFT) | \
	   (1 << JOIN_FULL) | \
	   (1 << JOIN_RIGHT) | \
	   (1 << JOIN_ANTI) | \
	   (1 << JOIN_RIGHT_ANTI))) != 0)

/*
 * AggStrategy -
 *	  Agg 计划节点的总体执行策略
 *
 * pathnodes.h 与 plannodes.h 都需要,故放此处...
 */
typedef enum AggStrategy
{
	AGG_PLAIN,					/* 跨全部输入行简单聚合 */
	AGG_SORTED,					/* 分组聚合,输入须已排序 */
	AGG_HASHED,					/* 分组聚合,用内部哈希表 */
	AGG_MIXED					/* 分组聚合,哈希与排序都用 */
} AggStrategy;

/*
 * AggSplit -
 *	  Agg 计划节点的拆分(部分聚合)模式
 *
 * pathnodes.h 与 plannodes.h 都需要,故放此处...
 */

/* nodeAgg.c 支持的原始选项: */
#define AGGSPLITOP_COMBINE		0x01	/* 用 combinefn 替代 transfn */
#define AGGSPLITOP_SKIPFINAL	0x02	/* 跳过finalfn,原样返回状态 */
#define AGGSPLITOP_SERIALIZE	0x04	/* 对输出应用 serialfn */
#define AGGSPLITOP_DESERIALIZE	0x08	/* 对输入应用deserialfn */

/* 支持的运行模式(即这些选项的有用组合): */
typedef enum AggSplit
{
	/* 基本、非拆分聚合: */
	AGGSPLIT_SIMPLE = 0,
	/* 部分聚合初始阶段,带序列化: */
	AGGSPLIT_INITIAL_SERIAL = AGGSPLITOP_SKIPFINAL | AGGSPLITOP_SERIALIZE,
	/* 部分聚合最终阶段,带反序列化: */
	AGGSPLIT_FINAL_DESERIAL = AGGSPLITOP_COMBINE | AGGSPLITOP_DESERIALIZE
} AggSplit;

/* 测试 AggSplit 值是否选择各原始选项: */
#define DO_AGGSPLIT_COMBINE(as)		(((as) & AGGSPLITOP_COMBINE) != 0)
#define DO_AGGSPLIT_SKIPFINAL(as)	(((as) & AGGSPLITOP_SKIPFINAL) != 0)
#define DO_AGGSPLIT_SERIALIZE(as)	(((as) & AGGSPLITOP_SERIALIZE) != 0)
#define DO_AGGSPLIT_DESERIALIZE(as) (((as) & AGGSPLITOP_DESERIALIZE) != 0)

/*
 * SetOpCmd 与 SetOpStrategy -
 *	  SetOp 计划节点的总体语义与执行策略
 *
 * pathnodes.h 与 plannodes.h 都需要,故放此处...
 */
typedef enum SetOpCmd
{
	SETOPCMD_INTERSECT,
	SETOPCMD_INTERSECT_ALL,
	SETOPCMD_EXCEPT,
	SETOPCMD_EXCEPT_ALL
} SetOpCmd;

typedef enum SetOpStrategy
{
	SETOP_SORTED,				/* 输入须已排序 */
	SETOP_HASHED				/* 用内部哈希表 */
} SetOpStrategy;

/*
 * OnConflictAction -
 *	  查询 "ON CONFLICT" 子句类型
 *
 * parsenodes.h 与 plannodes.h 都需要,故放此处...
 */
typedef enum OnConflictAction
{
	ONCONFLICT_NONE,			/* 无ON CONFLICT子句 */
	ONCONFLICT_NOTHING,			/* ON CONFLICT ... DO NOTHING */
	ONCONFLICT_UPDATE			/* ON CONFLICT ... DO UPDATE */
} OnConflictAction;

/*
 * LimitOption -
 *	查询 LIMIT 选项
 *
 * parsenodes.h 与 plannodes.h 都需要,故放此处...
 */
typedef enum LimitOption
{
	LIMIT_OPTION_COUNT,			/* FETCH FIRST... ONLY */
	LIMIT_OPTION_WITH_TIES,		/* FETCH FIRST... WITH TIES */
	LIMIT_OPTION_DEFAULT,		/* 无线索限制 */
} LimitOption;

#endif							/* NODES_H */
