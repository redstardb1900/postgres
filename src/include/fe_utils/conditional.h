/*-------------------------------------------------------------------------
 * 处理嵌套条件的自动机状态栈。
 *
 * 本文件描述自动机状态栈，用于管理
 * 嵌套条件。
 *
 * 用于：
 * - "psql" 解释器处理 \if ... \endif
 * - "pgbench" 解释器处理 \if ... \endif
 * - "pgbench" 语法检查器测试嵌套是否正确
 *
 * 略
 * 是否已遇过真分支？），以便解释器知道是否
 * 执行代码以及是否求值条件。
 * 略
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/include/fe_utils/conditional.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONDITIONAL_H
#define CONDITIONAL_H

/*
 * 单层 \if 块的可能状态。
 */
typedef enum ifState
{
	IFSTATE_NONE = 0,			/*  当前不在 \if 块中  */
	IFSTATE_TRUE,				/*  当前在为真的 \if 或 \elif 中，
								 * 且所有父分支（若有）为真 */
	IFSTATE_FALSE,				/*  当前在为假的 \if 或 \elif 中，
								 * 但尚未见真分支，且
								 * 所有父分支（若有）为真 */
	IFSTATE_IGNORED,			/*  当前在真分支之后的 \elif 中，
								 * 或整个 \if 是假父分支的
								 * 子分支 */
	IFSTATE_ELSE_TRUE,			/*  当前在为真的 \else 中，且所有
								 * 父分支（若有）为真 */
	IFSTATE_ELSE_FALSE			/*  当前在为假或被忽略的
								 * 略 */
} ifState;

/*
 * 嵌套 \if 的状态存在栈中。
 *
 * query_len 用于决定在非活动分支末丢弃多少
 * 已累积文本。（也可教词法器在非活动分支内
 * 起初就不往查询缓冲加内容；但改动会很大。）
 * 丢弃文本时还须保存并恢复词法器的括号嵌套
 * 深度。（其他状态如注释嵌套深度不必保存，
 * 因反斜杠命令绝不会出现在注释或 SQL
 * 字面量中。）
 * 略
 */
typedef struct IfStackElem
{
	ifState		if_state;		/* 略 */
	int			query_len;		/*  上次分支起点处 query_buf 长度  */
	int			paren_depth;	/*  上次分支起点处括号深度  */
	struct IfStackElem *next;	/* 略 */
} IfStackElem;

typedef struct ConditionalStackData
{
	IfStackElem *head;
}			ConditionalStackData;

typedef struct ConditionalStackData *ConditionalStack;


extern ConditionalStack conditional_stack_create(void);

extern void conditional_stack_reset(ConditionalStack cstack);

extern void conditional_stack_destroy(ConditionalStack cstack);

extern int	conditional_stack_depth(ConditionalStack cstack);

extern void conditional_stack_push(ConditionalStack cstack, ifState new_state);

extern bool conditional_stack_pop(ConditionalStack cstack);

extern ifState conditional_stack_peek(ConditionalStack cstack);

extern bool conditional_stack_poke(ConditionalStack cstack, ifState new_state);

extern bool conditional_stack_empty(ConditionalStack cstack);

extern bool conditional_active(ConditionalStack cstack);

extern void conditional_stack_set_query_len(ConditionalStack cstack, int len);

extern int	conditional_stack_get_query_len(ConditionalStack cstack);

extern void conditional_stack_set_paren_depth(ConditionalStack cstack, int depth);

extern int	conditional_stack_get_paren_depth(ConditionalStack cstack);

#endif							/* CONDITIONAL_H */
