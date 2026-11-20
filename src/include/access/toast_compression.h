/*-------------------------------------------------------------------------
 *
 * toast_compression.h
 *	  TOAST 压缩函数。
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 *
 * src/include/access/toast_compression.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_COMPRESSION_H
#define TOAST_COMPRESSION_H

/*
 * GUC 支持.
 *
 * default_toast_compression 对 GUC 机制而言是整数，
 * 但其值是下文所定义的 char 值之一，即出现在 
 * pg_attribute.attcompression,如 TOAST_PGLZ_COMPRESSION。
 */
extern PGDLLIMPORT int default_toast_compression;

/*
 * 内建压缩方法 ID。TOAST 压缩头会把
 * 此项存入原始长度的前 2 位。这些内建压缩
 * 方法 ID 直接映射到内建压缩方法。
 *
 * 勿把这些值用于理解 varlena 原始比特之含义以外的
 * 任何用途；特别是，若目的是识别
 * 压缩方法，则用常量 TOAST_PGLZ_COMPRESSION 等
 * 下文。我们或将支持多于 4 种压缩方法，但
 * 我们在此枚举中永远不能有超过 4 个值，因为
 * 存放该值之处仅有 2 位可用。
 */
typedef enum ToastCompressionId
{
	TOAST_PGLZ_COMPRESSION_ID = 0,
	TOAST_LZ4_COMPRESSION_ID = 1,
	TOAST_INVALID_COMPRESSION_ID = 2
} ToastCompressionId;

/*
 * 内建压缩方法。pg_attribute 会把它们存入 
 * attcompression 列；attcompression 中 InvalidCompressionMethod 
 * 表示默认行为。
 */
#define TOAST_PGLZ_COMPRESSION			'p'
#define TOAST_LZ4_COMPRESSION			'l'
#define InvalidCompressionMethod		'\0'

#define CompressionMethodIsValid(cm)  ((cm) != InvalidCompressionMethod)


/* pglz 压缩/解压例程 */
extern struct varlena *pglz_compress_datum(const struct varlena *value);
extern struct varlena *pglz_decompress_datum(const struct varlena *value);
extern struct varlena *pglz_decompress_datum_slice(const struct varlena *value,
												   int32 slicelength);

/* lz4 压缩/解压例程 */
extern struct varlena *lz4_compress_datum(const struct varlena *value);
extern struct varlena *lz4_decompress_datum(const struct varlena *value);
extern struct varlena *lz4_decompress_datum_slice(const struct varlena *value,
												  int32 slicelength);

/* 其他项 */
extern ToastCompressionId toast_get_compression_id(struct varlena *attr);
extern char CompressionNameToMethod(const char *compression);
extern const char *GetCompressionMethodName(char method);

#endif							/* TOAST_COMPRESSION_H */
