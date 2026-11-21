/*-------------------------------------------------------------------------
 * auxprocess.h
 *	  与辅助进程相关函数的头文件。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/postmaster/auxprocess.h
 *-------------------------------------------------------------------------
 */
#ifndef AUXPROCESS_H
#define AUXPROCESS_H

#include "miscadmin.h"

extern void AuxiliaryProcessMain(AuxProcType auxtype) pg_attribute_noreturn();

#endif							/* AUXPROCESS_H */
