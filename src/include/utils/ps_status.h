/*-------------------------------------------------------------------------
 *
 * ps_status.h
 *
 * backend/utils/misc/ps_status.c 的声明
 *
 * src/include/utils/ps_status.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PS_STATUS_H
#define PS_STATUS_H

/*  Windows 上禁用，因性能开销可能很大  */
#ifdef WIN32
#define DEFAULT_UPDATE_PROCESS_TITLE false
#else
#define DEFAULT_UPDATE_PROCESS_TITLE true
#endif

extern PGDLLIMPORT bool update_process_title;

extern char **save_ps_display_args(int argc, char **argv);

extern void init_ps_display(const char *fixed_part);

extern void set_ps_display_suffix(const char *suffix);

extern void set_ps_display_remove_suffix(void);

extern void set_ps_display_with_len(const char *activity, size_t len);

/*
 * set_ps_display
 *		内联以便在传入字符串常量时于编译期
 *		求值 strlen。
 */
static inline void
set_ps_display(const char *activity)
{
	set_ps_display_with_len(activity, strlen(activity));
}

extern const char *get_ps_display(int *displen);

#endif							/* PS_STATUS_H */
