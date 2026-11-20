/*-------------------------------------------------------------------------
 *
 * tupdesc_details.h
 *	  POSTGRES 元组描述符定义，我们无法处处包含
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc_details.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TUPDESC_DETAILS_H
#define TUPDESC_DETAILS_H

/*
 * 用于表示当属性在元组中根本不存在时
 * 所用之值的结构，即当该列在该元组之后创建
 */
typedef struct AttrMissing
{
	bool		am_present;		/* 若存在非 NULL 缺失值则为真 */
	Datum		am_value;		/* 属性缺失时的值 */
} AttrMissing;

#endif							/* TUPDESC_DETAILS_H */
