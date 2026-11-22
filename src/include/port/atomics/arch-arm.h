/*-------------------------------------------------------------------------
 *
 * arch-arm.h
 *	  Atomic operations considerations specific to ARM
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * src/include/port/atomics/arch-arm.h
 *
 *-------------------------------------------------------------------------
 */

/*  故意无头文件卫士,仅应由 atomics.h 包含  */
#ifndef INSIDE_ATOMICS_H
#error "should be included via atomics.h"
#endif

/*
 * ARM32 上 64 位原子用内核回退实现,因而
 * 可能很慢,故完全禁用。ARM64 无此问题。
 */
#if !defined(__aarch64__)
#define PG_DISABLE_64_BIT_ATOMICS
#else
/*
 * ARMv8 架构参考手册称:对齐的通用寄存器
 * 读/写是原子的。
 */
#define PG_HAVE_8BYTE_SINGLE_COPY_ATOMICITY
#endif  /* __aarch64__ */
