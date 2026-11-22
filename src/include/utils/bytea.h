/*-------------------------------------------------------------------------
 *
 * bytea.h
 *	  BYTEA 数据类型支持声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/bytea.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BYTEA_H
#define BYTEA_H



typedef enum
{
	BYTEA_OUTPUT_ESCAPE,
	BYTEA_OUTPUT_HEX
}			ByteaOutputType;

extern PGDLLIMPORT int bytea_output;	/*  ByteaOutputType，GUC 用 int
										 * enum */

#endif							/* BYTEA_H */
