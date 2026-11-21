/*-------------------------------------------------------------------------
 *
 * copyfrom_internal.h
 *	COPY FROM 命令的内部定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/copyfrom_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPYFROM_INTERNAL_H
#define COPYFROM_INTERNAL_H

#include "commands/copy.h"
#include "commands/trigger.h"

/*
 * 表示需处理的不同源情形，位于
 * 底端
 */
typedef enum CopySource
{
	COPY_FILE,					/* 来自文件（或管道） */
	COPY_FRONTEND,				/* 来自前端 */
	COPY_CALLBACK				/* 来自回调函数 */
} CopySource;

/*
 *	表示输入的行终止符类型
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL
} EolType;

/*
 * 表示 COPY FROM 期间所用的插入方法。
 */
typedef enum CopyInsertMethod
{
	CIM_SINGLE,					/* 用 tuple_insert 或 ExecForeignInsert */
	CIM_MULTI,					/* 总是用 table_multi_insert 或
								 * ExecForeignBatchInsert */
	CIM_MULTI_CONDITIONAL		/* 用批量插入或
								 * ExecForeignBatchInsert 仅当有效 */
} CopyInsertMethod;

/*
 * 此结构含 COPY FROM 操作全程所用的全部
 * 状态。
 */
typedef struct CopyFromStateData
{
	/* 低层状态数据 */
	CopySource	copy_src;		/* copy 源类型 */
	FILE	   *copy_file;		/* copy_src 为 COPY_FILE 时用 */
	StringInfo	fe_msgbuf;		/* copy_src 为 COPY_FRONTEND 时用 */

	EolType		eol_type;		/* 输入 EOL 类型 */
	int			file_encoding;	/* 文件或远端的字符编码 */
	bool		need_transcoding;	/* 文件编码异于服务器？ */
	Oid			conversion_proc;	/* 编码转换函数 */

	/* 来自 COPY 命令的参数 */
	Relation	rel;			/* 要装入的关系 */
	List	   *attnumlist;		/* 待复制属性号列表 */
	char	   *filename;		/* 文件名；STDIN 则 NULL */
	bool		is_program;		/* filename 是否待 popen？ */
	copy_data_source_cb data_source_cb; /* 读数据的函数 */

	CopyFormatOptions opts;
	bool	   *convert_select_flags;	/* 每列 CSV/TEXT CS 标志 */
	Node	   *whereClause;	/* WHERE 条件(或 NULL) */

	/* 仅供报错用，见 CopyFromErrorCallback */
	const char *cur_relname;	/* 报错用的表名 */
	uint64		cur_lineno;		/* 报错用的行号 */
	const char *cur_attname;	/* 报错用的当前属性 */
	const char *cur_attval;		/* 报错用的当前属性值 */
	bool		relname_only;	/* 不输出行号、属性等 */

	/*
	 * 工作状态
	 */
	MemoryContext copycontext;	/* 每 copy 执行上下文 */

	AttrNumber	num_defaults;	/* 缺失且有默认值的属性
								 * 个数 */
	FmgrInfo   *in_functions;	/* 各属性输入函数数组 */
	Oid		   *typioparams;	/* in_functions 的元素类型数组 */
	int		   *defmap;			/* 与缺失属性相关的默认属性
								 * 号数组 */
	ExprState **defexprs;		/* 全部属性的默认表达式
								 * 列 */
	bool	   *defaults;		/* 若对应属性发现 DEFAULT
								 * 标记 */
	bool		volatile_defexprs;	/* defexprs 有易变？ */
	List	   *range_table;	/* 仅含一个 RangeTblEntry 的列表 */
	List	   *rteperminfos;	/* 仅含一个 RTEPermissionInfo 的列表 */
	ExprState  *qualexpr;

	TransitionCaptureState *transition_capture;

	/*
	 * 这些变量用于降低 COPY FROM 开销。
	 *
	 * attribute_buf 保存当前行各字段经分隔、去转义后的
	 * 文本。CopyReadAttributes 函数返回指向本缓冲的
	 * 指针数组。通过每轮复用缓冲避免
	 * palloc/pfree 开销。
	 *
	 * 在二进制 COPY FROM 中，attribute_buf 保存当前字段的
	 * 二进制数据，其余用法类似。
	 */
	StringInfoData attribute_buf;

	/* COPY FROM 所得字段原始指针 */

	int			max_fields;
	char	  **raw_fields;

	/*
	 * 同理，line_buf 保存正在处理的整行输入。输入周期
	 * 先是把整行读入 line_buf，再把各属性字段
	 * 抽到 attribute_buf。line_buf 保持原样以便在适当时
	 * 于报错信息中显示。（二进制模式下
	 * 不用 line_buf。）
	 */
	StringInfoData line_buf;
	bool		line_buf_valid; /* 是否含正在处理的行？ */

	/*
	 * input_buf 保存已转为数据库编码的输入数据。
	 *
	 * 文本模式下，CopyReadLine 解析这些数据以定位行
	 * 边界，再转入 line_buf。我们保证任何时候
	 * input_buf[input_buf_len] 处都有 \0。（二进制
	 * 模式不用 input_buf。）
	 *
	 * 若不需编码转换，input_buf 不是独立缓冲，而是直接
	 * 指向 raw_buf。此时 input_buf_len 跟踪已校验为
	 * 数据库编码合法的字节数，而 raw_buf_len 是
	 * 缓冲中存放的总字节数。
	 * 见上。
	 */
#define INPUT_BUF_SIZE 65536	/* palloc INPUT_BUF_SIZE+1 字节 */
	char	   *input_buf;
	int			input_buf_index;	/* 下一字节 */
	int			input_buf_len;	/* 已存总字节数 */
	bool		input_reached_eof;	/* 达 EOF 则为真 */
	bool		input_reached_error;	/* 若发生转换错误则为真 */
	/* input_buf 中未消费字节数的简写 */
#define INPUT_BUF_BYTES(cstate) ((cstate)->input_buf_len - (cstate)->input_buf_index)

	/*
	 * raw_buf 保存从数据源（文件或客户端连接）读入、
	 * 尚未转为数据库编码的原始输入。与
	 * input_buf 一样，保证 raw_buf[raw_buf_len] 处有 \0。
	 */
#define RAW_BUF_SIZE 65536		/* palloc RAW_BUF_SIZE+1 字节 */
	char	   *raw_buf;
	int			raw_buf_index;	/* 下一字节 */
	int			raw_buf_len;	/* 已存总字节数 */
	bool		raw_reached_eof;	/* 达 EOF 则为真 */

	/* raw_buf 中未消费字节数的简写 */
#define RAW_BUF_BYTES(cstate) ((cstate)->raw_buf_len - (cstate)->raw_buf_index)

	uint64		bytes_processed;	/* 迄今已处理字节数 */
} CopyFromStateData;

extern void ReceiveCopyBegin(CopyFromState cstate);
extern void ReceiveCopyBinaryHeader(CopyFromState cstate);

#endif							/* COPYFROM_INTERNAL_H */
