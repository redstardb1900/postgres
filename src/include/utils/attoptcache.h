/*-------------------------------------------------------------------------
 *
 * attoptcache.h
 *	  属性选项缓存。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/attoptcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATTOPTCACHE_H
#define ATTOPTCACHE_H

/*
 * 属性选项。
 */
typedef struct AttributeOpts
{
	int32		vl_len_;		/*  varlena 头（勿直接碰！）  */
	float8		n_distinct;
	float8		n_distinct_inherited;
} AttributeOpts;

extern AttributeOpts *get_attribute_options(Oid attrelid, int attnum);

#endif							/* ATTOPTCACHE_H */
