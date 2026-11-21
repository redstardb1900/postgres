/*-------------------------------------------------------------------------
 *
 * basebackup_target.h
 *	  添加基备份目标的可扩展框架。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup_target.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BASEBACKUP_TARGET_H
#define BASEBACKUP_TARGET_H

#include "backup/basebackup_sink.h"

struct BaseBackupTargetHandle;
typedef struct BaseBackupTargetHandle BaseBackupTargetHandle;

/*
 * 扩展可调用本函数创建新备份目标。
 *
 * 'name' 是新目标的名称。
 *
 * 'check_detail' 接受目标名与目标细节，
 * 若细节无效或发现其他问题（如权限）则报错，
 * 否则返回创建实现该目标的 bbsink 所需
 * 数据的指针。
 * 若 BASE_BACKUP 未指定 TARGET_DETAIL，第二
 * 参数为 NULL。
 *
 * 'get_sink' 创建 bbsink。第一参数是后继
 * 接收器；本函数创建的接收器应始终转发到它。
 * 第二参数是先前 'check_detail' 返回的指针。
 * 。
 *
 * 实践中用户可能指定目标形如 "foo:bar"
 * （经 pg_basebackup）。服务器查找名为 "foo" 的目标。
 * 若找到，对字符串 "bar" 调用 check_detail，
 * 其返回值作为 get_sink 的第二参数。
 * 。
 */
extern void BaseBackupAddTarget(char *name,
								void *(*check_detail) (char *, char *),
								bbsink *(*get_sink) (bbsink *, void *));

/*
 * 核心代码用这些函数访问经 BaseBackupAddTarget()
 * 添加的基备份目标。核心会把用户给的 TARGET 与
 * TARGET_DETAIL 传给 BaseBackupGetTargetHandle，
 * 若 TARGET 未识别或该 TARGET 的 check_detail
 * 不认可 TARGET_DETAIL 则报错，否则返回可稍后
 * 传给 BaseBackupGetSink 的 BaseBackupTargetHandle。
 * 。
 *
 * BaseBackupGetSink 用该句柄与后继 bbsink 构造
 * 实现所需目标的 bbsink，做法是调用扩展提供的
 * get_sink() 回调。
 * 。
 */
extern BaseBackupTargetHandle *BaseBackupGetTargetHandle(char *target,
														 char *target_detail);
extern bbsink *BaseBackupGetSink(BaseBackupTargetHandle *handle,
								 bbsink *next_sink);

#endif
