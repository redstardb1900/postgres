/*-------------------------------------------------------------------------
 *
 * ts_type.h
 *	  tsvector 与 tsquery 类型的定义
 *
 * Copyright (c) 1998-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_type.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_TSTYPE_H_
#define _PG_TSTYPE_H_

#include "fmgr.h"
#include "utils/memutils.h"


/*
 * TSVector
 *
 * tsvector 数据类型结构：
 * 1) 标准 varlena 头
 * 2) int32		词元数（WordEntry 数组项数）
 * 3) WordEntry 数组 - 每词元一项；须按
 *				tsCompareString()（词元 memcmp）排序。
 *				WordEntry->pos 给出从 WordEntry 数组末
 *				到词元串起点的字节数，串长为 len。
 * 4) 每词元数据存储：
 *	  词元串（非空终止）
 *	  若 haspos：
 *		必要时填充字节使位置数据 2 字节对齐
 *		uint16			后续位置个数
 *		WordEntryPos[]	位置
 *
 * 各词元的位置须已排序。
 *
 * 注意 tsvectorsend/recv 假定 sizeof(WordEntry)==4
 */

typedef struct
{
	uint32
				haspos:1,
				len:11,			/*  2Kb  */
				pos:20;			/*  1Mb  */
} WordEntry;

#define MAXSTRLEN ( (1<<11) - 1)
#define MAXSTRPOS ( (1<<20) - 1)

extern int	compareWordEntryPos(const void *a, const void *b);

/*
 * Equivalent to
 * typedef struct {
 *		uint16
 *			weight:2,
 *			pos:14;
 * }
 */

typedef uint16 WordEntryPos;

typedef struct
{
	uint16		npos;
	WordEntryPos pos[FLEXIBLE_ARRAY_MEMBER];
} WordEntryPosVector;

/*  恰含 1 项的 WordEntryPosVector  */
typedef struct
{
	uint16		npos;
	WordEntryPos pos[1];
} WordEntryPosVector1;


#define WEP_GETWEIGHT(x)	( (x) >> 14 )
#define WEP_GETPOS(x)		( (x) & 0x3fff )

#define WEP_SETWEIGHT(x,v)	( (x) = ( (v) << 14 ) | ( (x) & 0x3fff ) )
#define WEP_SETPOS(x,v)		( (x) = ( (x) & 0xc000 ) | ( (v) & 0x3fff ) )

#define MAXENTRYPOS (1<<14)
#define MAXNUMPOS	(256)
#define LIMITPOS(x) ( ( (x) >= MAXENTRYPOS ) ? (MAXENTRYPOS-1) : (x) )

/*  本结构表示完整 tsvector 数据  */
typedef struct
{
	int32		vl_len_;		/*  varlena 头（勿直接触碰！）  */
	int32		size;
	WordEntry	entries[FLEXIBLE_ARRAY_MEMBER];
	/* 。 */
} TSVectorData;

typedef TSVectorData *TSVector;

#define DATAHDRSIZE (offsetof(TSVectorData, entries))
#define CALCDATASIZE(nentries, lenstr) (DATAHDRSIZE + (nentries) * sizeof(WordEntry) + (lenstr) )

/*  指向 tsvector 的 WordEntry 数组起点  */
#define ARRPTR(x)	( (x)->entries )

/*  指向 tsvector 的词元存储起点  */
#define STRPTR(x)	( (char *) &(x)->entries[(x)->size] )

#define _POSVECPTR(x, e)	((WordEntryPosVector *)(STRPTR(x) + SHORTALIGN((e)->pos + (e)->len)))
#define POSDATALEN(x,e) ( ( (e)->haspos ) ? (_POSVECPTR(x,e)->npos) : 0 )
#define POSDATAPTR(x,e) (_POSVECPTR(x,e)->pos)

/*
 * fmgr 接口函数
 */

static inline TSVector
DatumGetTSVector(Datum X)
{
	return (TSVector) PG_DETOAST_DATUM(X);
}

static inline TSVector
DatumGetTSVectorCopy(Datum X)
{
	return (TSVector) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
TSVectorGetDatum(const TSVectorData *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_TSVECTOR(n)		DatumGetTSVector(PG_GETARG_DATUM(n))
#define PG_GETARG_TSVECTOR_COPY(n)	DatumGetTSVectorCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_TSVECTOR(x)		return TSVectorGetDatum(x)


/*
 * TSQuery
 *
 *
 */

typedef int8 QueryItemType;

/*  QueryItemType 的合法值：  */
#define QI_VAL 1
#define QI_OPR 2
#define QI_VALSTOP 3			/*  仅用于 parse_tsquery 中的中间栈
								 * 表示。别处不是合法
								 * 类型。 */

/*
 * 。
 */
typedef struct
{
	QueryItemType type;			/* 。 */
	uint8		weight;			/* 。
								 * 位掩码。若为 0 则允许
								 * 任意权重。权重与位图：
								 * A:1<<3 B:1<<2 C:1<<1 D:1<<0 */
	bool		prefix;			/*  为真表示前缀搜索  */
	int32		valcrc;			/*  XXX: pg_crc32 更合适，但代码中
								 * 与有符号整数比较。那些
								 * 比较也须一并
								 * 改掉。 */

	/*  指向操作数文本值的指针，须与 WordEntry 对应  */
	uint32
				length:12,
				distance:20;
} QueryOperand;


/*
 * QueryOperator.operator 的合法值。
 */
#define OP_NOT			1
#define OP_AND			2
#define OP_OR			3
#define OP_PHRASE		4		/* 。 */
#define OP_COUNT		4

extern PGDLLIMPORT const int tsearch_op_priority[OP_COUNT];

/*  按运算码取运算优先级  */
#define OP_PRIORITY(x)	( tsearch_op_priority[(x) - 1] )
/* 。 */
#define QO_PRIORITY(x)	OP_PRIORITY(((QueryOperator *) (x))->oper)

typedef struct
{
	QueryItemType type;
	int8		oper;			/* 。 */
	int16		distance;		/*  OP_PHRASE 的参数间距  */
	uint32		left;			/*  指向左操作数。右操作数为
								 * item+1，左操作数在
								 * item+item->left */
} QueryOperator;

/*
 * 注意：TSQuery 按 4 字节对齐，故确保 QueryItem
 * 内无需要 8 字节对齐的字段（如 int64）。
 */
typedef union
{
	QueryItemType type;
	QueryOperator qoperator;
	QueryOperand qoperand;
} QueryItem;

/*
 * 。
 *	(len)(size)(QueryItem 数组)(以 '\0' 终止的操作数 c 串)
 */

typedef struct
{
	int32		vl_len_;		/*  varlena 头（勿直接触碰！）  */
	int32		size;			/*  QueryItem 个数  */
	char		data[FLEXIBLE_ARRAY_MEMBER];	/* 。 */
} TSQueryData;

typedef TSQueryData *TSQuery;

#define HDRSIZETQ	( VARHDRSZ + sizeof(int32) )

/*  计算头与全部 QueryItem 的大小。size 为 QueryItem
 * 个数，lenofoperand 为全部操作数总长
 */
#define COMPUTESIZE(size, lenofoperand) ( HDRSIZETQ + (size) * sizeof(QueryItem) + (lenofoperand) )
#define TSQUERY_TOO_BIG(size, lenofoperand) \
	((size) > (MaxAllocSize - HDRSIZETQ - (lenofoperand)) / sizeof(QueryItem))

/*  返回指向 TSQuery 中首个 QueryItem 的指针  */
#define GETQUERY(x)  ((QueryItem*)( (char*)(x)+HDRSIZETQ ))

/*  返回指向 TSQuery 中操作数起始的指针  */
#define GETOPERAND(x)	( (char*)GETQUERY(x) + ((TSQuery)(x))->size * sizeof(QueryItem) )

/*
 * fmgr 接口函数
 * 注意：TSQuery 标为 plain 存储，故不能 toast，
 * 但为简便仍用 PG_DETOAST_DATUM_COPY
 */

static inline TSQuery
DatumGetTSQuery(Datum X)
{
	return (TSQuery) DatumGetPointer(X);
}

static inline TSQuery
DatumGetTSQueryCopy(Datum X)
{
	return (TSQuery) PG_DETOAST_DATUM_COPY(X);
}

static inline Datum
TSQueryGetDatum(const TSQueryData *X)
{
	return PointerGetDatum(X);
}

#define PG_GETARG_TSQUERY(n)		DatumGetTSQuery(PG_GETARG_DATUM(n))
#define PG_GETARG_TSQUERY_COPY(n)	DatumGetTSQueryCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_TSQUERY(x)		return TSQueryGetDatum(x)

#endif							/* _PG_TSTYPE_H_ */
