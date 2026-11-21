/*-------------------------------------------------------------------------
 *
 * windowapi.h
 *	  窗口函数从其窗口提取数据的 API
 *
 * 窗口函数不以常规方式接收参数（因此严格性
 * 概念无关）。而是接收可用 PG_WINDOW_OBJECT()
 * 取得的 "WindowObject"（须用 V1 调用约定）。
 * 可用 WindowObjectIsValid() 测试调用上下文是否
 * 正确。虽不传参数值，调用仍正确设置，故可用
 * PG_NARGS()，并用 get_fn_expr_argtype()、
 * get_fn_expr_arg_stable() 等取得参数类型信息。
 * 略
 *
 * 对 WindowObject 的操作使窗口函数可获知当前行
 * 号、分区总行数等，并在窗口分区各行求值其参数
 * expression。各 WindowObject API 函数的头注释见
 * nodeWindowAgg.c。
 * 略
 *
 *
 * Portions Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/include/windowapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WINDOWAPI_H
#define WINDOWAPI_H

/* 略 */
#define WINDOW_SEEK_CURRENT 0
#define WINDOW_SEEK_HEAD 1
#define WINDOW_SEEK_TAIL 2

/*  本结构在 nodeWindowAgg.c 中私有  */
typedef struct WindowObjectData *WindowObject;

#define PG_WINDOW_OBJECT() ((WindowObject) fcinfo->context)

#define WindowObjectIsValid(winobj) \
	((winobj) != NULL && IsA(winobj, WindowObjectData))

extern void *WinGetPartitionLocalMemory(WindowObject winobj, Size sz);

extern int64 WinGetCurrentPosition(WindowObject winobj);
extern int64 WinGetPartitionRowCount(WindowObject winobj);

extern void WinSetMarkPosition(WindowObject winobj, int64 markpos);

extern bool WinRowsArePeers(WindowObject winobj, int64 pos1, int64 pos2);

extern Datum WinGetFuncArgInPartition(WindowObject winobj, int argno,
									  int relpos, int seektype, bool set_mark,
									  bool *isnull, bool *isout);

extern Datum WinGetFuncArgInFrame(WindowObject winobj, int argno,
								  int relpos, int seektype, bool set_mark,
								  bool *isnull, bool *isout);

extern Datum WinGetFuncArgCurrent(WindowObject winobj, int argno,
								  bool *isnull);

#endif							/* WINDOWAPI_H */
