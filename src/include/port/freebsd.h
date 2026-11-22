/* src/include/port/freebsd.h */

/*
 * 默认 wal_sync_method 用 fdatasync。xlogdefs.h 常规规则在
 * FreeBSD 13+ 会偏好 open_datasync,但许多系统上并非
 * 好选择。
 */
#define PLATFORM_DEFAULT_SYNC_METHOD	SYNC_METHOD_FDATASYNC
