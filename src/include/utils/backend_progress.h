/* ----------
 * backend_progress.h
 *	  命令进度报告定义。
 *
 * 注意：本文件提供存储单个后端命令进度计数器的
 * 基础设施，不为各字段赋予含义。含义见
 * commands/progress.h 与 system_views.sql。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/backend_progress.h
 * ----------
 */
#ifndef BACKEND_PROGRESS_H
#define BACKEND_PROGRESS_H


/* ----------
 * 用于进度报告的命令类型
 * ----------
 */
typedef enum ProgressCommandType
{
	PROGRESS_COMMAND_INVALID,
	PROGRESS_COMMAND_VACUUM,
	PROGRESS_COMMAND_ANALYZE,
	PROGRESS_COMMAND_CLUSTER,
	PROGRESS_COMMAND_CREATE_INDEX,
	PROGRESS_COMMAND_BASEBACKUP,
	PROGRESS_COMMAND_COPY
} ProgressCommandType;

#define PGSTAT_NUM_PROGRESS_PARAM	20


extern void pgstat_progress_start_command(ProgressCommandType cmdtype,
										  Oid relid);
extern void pgstat_progress_update_param(int index, int64 val);
extern void pgstat_progress_incr_param(int index, int64 incr);
extern void pgstat_progress_update_multi_param(int nparam, const int *index,
											   const int64 *val);
extern void pgstat_progress_end_command(void);


#endif							/* BACKEND_PROGRESS_H */
