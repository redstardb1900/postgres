/*-------------------------------------------------------------------------
 *
 * compression.h
 *
 * 压缩方法与规格的共享定义。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/include/common/compression.h
 *-------------------------------------------------------------------------
 */

#ifndef PG_COMPRESSION_H
#define PG_COMPRESSION_H

/*
 * 这些值存于磁盘，例如 pg_dump 生成的文件。
 * 若顺序改变须做向后兼容层。
 */
typedef enum pg_compress_algorithm
{
	PG_COMPRESSION_NONE,
	PG_COMPRESSION_GZIP,
	PG_COMPRESSION_LZ4,
	PG_COMPRESSION_ZSTD
} pg_compress_algorithm;

#define PG_COMPRESSION_OPTION_WORKERS		(1 << 0)
#define PG_COMPRESSION_OPTION_LONG_DISTANCE	(1 << 1)

typedef struct pg_compress_specification
{
	pg_compress_algorithm algorithm;
	unsigned	options;		/*  PG_COMPRESSION_OPTION 常量的 OR  */
	int			level;
	int			workers;
	bool		long_distance;
	char	   *parse_error;	/* 略 */
} pg_compress_specification;

extern void parse_compress_options(const char *option, char **algorithm,
								   char **detail);
extern bool parse_compress_algorithm(char *name, pg_compress_algorithm *algorithm);
extern const char *get_compress_algorithm_name(pg_compress_algorithm algorithm);

extern void parse_compress_specification(pg_compress_algorithm algorithm,
										 char *specification,
										 pg_compress_specification *result);

extern char *validate_compress_specification(pg_compress_specification *);

#endif
