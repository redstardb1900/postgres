/*-------------------------------------------------------------------------
 *
 * checksum.c
 *	  数据页的校验和实现。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/checksum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/checksum.h"
/*
 * 实际代码在 storage/checksum_impl.h 里。这样做是为了让外部程序
 * 能够通过 #include 从导出的 Postgres 头文件纳入这段校验和代码。
 * （对照我们的 CRC 代码。）
 */
#include "storage/checksum_impl.h"
