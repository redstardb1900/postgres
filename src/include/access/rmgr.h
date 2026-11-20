/*
 * rmgr.h
 *
 * 资源管理器定义
 *
 * src/include/access/rmgr.h
 */
#ifndef RMGR_H
#define RMGR_H

typedef uint8 RmgrId;

/*
 * 内建资源管理器
 *
 * 各 rmgr ID 的实际数值由 rmgrlist.h 中
 * 条目的顺序定义。
 *
 * 注意：RM_MAX_ID 必须能放入 RmgrId；加宽该类型将影响 XLOG 
 * 文件格式
 */
#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode) \
	symname,

typedef enum RmgrIds
{
#include "access/rmgrlist.h"
	RM_NEXT_ID
}			RmgrIds;

#undef PG_RMGR

#define RM_MAX_ID			UINT8_MAX
#define RM_MAX_BUILTIN_ID	(RM_NEXT_ID - 1)
#define RM_MIN_CUSTOM_ID	128
#define RM_MAX_CUSTOM_ID	UINT8_MAX
#define RM_N_IDS			(UINT8_MAX + 1)
#define RM_N_BUILTIN_IDS	(RM_MAX_BUILTIN_ID + 1)
#define RM_N_CUSTOM_IDS		(RM_MAX_CUSTOM_ID - RM_MIN_CUSTOM_ID + 1)

static inline bool
RmgrIdIsBuiltin(int rmid)
{
	return rmid <= RM_MAX_BUILTIN_ID;
}

static inline bool
RmgrIdIsCustom(int rmid)
{
	return rmid >= RM_MIN_CUSTOM_ID && rmid <= RM_MAX_CUSTOM_ID;
}

#define RmgrIdIsValid(rmid) (RmgrIdIsBuiltin((rmid)) || RmgrIdIsCustom((rmid)))

/*
 * 供需要 RmgrId 但仍在开发中、尚未预留自身唯一 
 * RmgrId 的扩展使用的 RmgrId。见：
 * https://wiki.postgresql.org/wiki/CustomWALResourceManagers
 */
#define RM_EXPERIMENTAL_ID		128

#endif							/* RMGR_H */
