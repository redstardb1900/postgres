/*-------------------------------------------------------------------------
 *
 * miscnodes.h
 *	难归类节点类型的定义。
 *
 * 此处声明的节点类型不属于解析树、计划树或
 * 执行状态树。赋 NodeTag 只因 IsA() 测试便于在各类
 * API（如函数 "context" 指针）中辨析所传结构
 * 种类。
 * 指针。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/miscnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MISCNODES_H
#define MISCNODES_H

#include "nodes/nodes.h"

/*
 * ErrorSaveContext -
 *	处理"软"错误的函数调用上下文节点
 *
 * 欲捕获软错误的调用方须初始化此类结构，除 NodeTag
 * 外字段皆为零/NULL。若需要比"发生了软错误"更多
 * 信息，可选用 details_wanted = true。随后经
 * FunctionCallInfo.context 传给 SQL 可调用函数；或在
 * SQL 调用层之下直接传给子程序。
 * 直接传入。
 *
 * 调用可能以此方式报错代码后,检查 error_occurred
 * 看是否出错。若出错且 details_wanted 为真，则
 * error_data 已填入错误细节（存于被调方的
 * 内存上下文！）。可调用 FreeErrorData() 释放
 * error_data，若被调代码跑在短寿上下文中则通常
 * 不必。
 */
typedef struct ErrorSaveContext
{
	NodeTag		type;
	bool		error_occurred; /* 若检测到软错误则置真 */
	bool		details_wanted; /* 调用方是否要更多信息？ */
	ErrorData  *error_data;		/* 若是,含错误细节 */
} ErrorSaveContext;

/* 常用于检查是否报告了软错误的宏 */
#define SOFT_ERROR_OCCURRED(escontext) \
	((escontext) != NULL && IsA(escontext, ErrorSaveContext) && \
	 ((ErrorSaveContext *) (escontext))->error_occurred)

#endif							/* MISCNODES_H */
