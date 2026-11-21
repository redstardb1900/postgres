/*-------------------------------------------------------------------------
 * 前端程序的日志框架
 *
 * Copyright (c) 2018-2023, PostgreSQL Global Development Group
 *
 * src/include/common/logging.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_LOGGING_H
#define COMMON_LOGGING_H

/*
 * 日志级别仅供信息，不影响程序流。
 */
enum pg_log_level
{
	/*
	 * 尚未初始化（勿用作实际消息级别）。
	 */
	PG_LOG_NOTSET = 0,

	/*
	 * 通常默认关闭的低级消息。
	 */
	PG_LOG_DEBUG,

	/*
	 * 略
	 * 正常输出应走 stdout，不用日志系统。）
	 * 略
	 */
	PG_LOG_INFO,

	/*
	 * 警告与"几乎"错误，依程序而定
	 */
	PG_LOG_WARNING,

	/*
	 * 错误
	 */
	PG_LOG_ERROR,

	/*
	 * 关闭全部日志（勿用作实际消息级别）。
	 */
	PG_LOG_OFF,
};

/*
 * __pg_log_level 是实际会显示的最低日志级别。
 */
extern enum pg_log_level __pg_log_level;

/*
 * 日志消息可有多部分。主消息必需，其余可选。
 * 发多部分时按本枚举顺序，以求一致。
 * 略
 */
enum pg_log_part
{
	/*
	 * 主消息。尽量一行；遵循后端主消息风格指南。
	 * 略
	 */
	PG_LOG_PRIMARY,

	/*
	 * 附加细节。遵循后端细节消息风格指南。
	 * 略
	 */
	PG_LOG_DETAIL,

	/*
	 * 关于如何修复的提示（不保证正确）。遵循后端
	 * 提示消息风格指南。
	 */
	PG_LOG_HINT,
};

/*
 * 小技巧：能按回归测试要求精确产出 psql 输出。
 * 略
 */
#define PG_LOG_FLAG_TERSE	1

void		pg_logging_init(const char *argv0);
void		pg_logging_config(int new_flags);
void		pg_logging_set_level(enum pg_log_level new_level);
void		pg_logging_increase_verbosity(void);
void		pg_logging_set_pre_callback(void (*cb) (void));
void		pg_logging_set_locus_callback(void (*cb) (const char **filename, uint64 *lineno));

void		pg_log_generic(enum pg_log_level level, enum pg_log_part part,
						   const char *pg_restrict fmt,...)
			pg_attribute_printf(3, 4);
void		pg_log_generic_v(enum pg_log_level level, enum pg_log_part part,
							 const char *pg_restrict fmt, va_list ap)
			pg_attribute_printf(3, 0);

/*
 * 首选用这些宏记日志；勿直接调 pg_log_generic[_v]，
 * 除非在错误接口代码中。
 */
#define pg_log_error(...) \
	pg_log_generic(PG_LOG_ERROR, PG_LOG_PRIMARY, __VA_ARGS__)

#define pg_log_error_detail(...) \
	pg_log_generic(PG_LOG_ERROR, PG_LOG_DETAIL, __VA_ARGS__)

#define pg_log_error_hint(...) \
	pg_log_generic(PG_LOG_ERROR, PG_LOG_HINT, __VA_ARGS__)

#define pg_log_warning(...) \
	pg_log_generic(PG_LOG_WARNING, PG_LOG_PRIMARY, __VA_ARGS__)

#define pg_log_warning_detail(...) \
	pg_log_generic(PG_LOG_WARNING, PG_LOG_DETAIL, __VA_ARGS__)

#define pg_log_warning_hint(...) \
	pg_log_generic(PG_LOG_WARNING, PG_LOG_HINT, __VA_ARGS__)

#define pg_log_info(...) \
	pg_log_generic(PG_LOG_INFO, PG_LOG_PRIMARY, __VA_ARGS__)

#define pg_log_info_detail(...) \
	pg_log_generic(PG_LOG_INFO, PG_LOG_DETAIL, __VA_ARGS__)

#define pg_log_info_hint(...) \
	pg_log_generic(PG_LOG_INFO, PG_LOG_HINT, __VA_ARGS__)

#define pg_log_debug(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) \
			pg_log_generic(PG_LOG_DEBUG, PG_LOG_PRIMARY, __VA_ARGS__); \
	} while(0)

#define pg_log_debug_detail(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) \
			pg_log_generic(PG_LOG_DEBUG, PG_LOG_DETAIL, __VA_ARGS__); \
	} while(0)

#define pg_log_debug_hint(...) do { \
		if (unlikely(__pg_log_level <= PG_LOG_DEBUG)) \
			pg_log_generic(PG_LOG_DEBUG, PG_LOG_HINT, __VA_ARGS__); \
	} while(0)

/*
 * 常用捷径：pg_log_error() 并立即 exit(1)。
 */
#define pg_fatal(...) do { \
		pg_log_generic(PG_LOG_ERROR, PG_LOG_PRIMARY, __VA_ARGS__); \
		exit(1); \
	} while(0)

#endif							/* COMMON_LOGGING_H */
