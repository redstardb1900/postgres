/*-------------------------------------------------------------------------
 *
 * geo_decls.h - 各种 2D 构造的声明。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/geo_decls.h
 *
 *	  XXX 这些例程并非数值分析师所写。
 *
 *	  XXX 我已尽量充实运算符与数据类型。还有
 *		一些要做。- tgl 97/04/19
 *
 *-------------------------------------------------------------------------
 */
#ifndef GEO_DECLS_H
#define GEO_DECLS_H

#include <math.h>

#include "fmgr.h"

/*--------------------------------------------------------------------
 * 有用的浮点工具与常量。
 *--------------------------------------------------------------------
 *
 * "模糊"浮点比较：彼此相差在 EPSILON 内视为相等。
 * 慎用对此类比较的常规定理，例如 FPeq 不满足传递
 * 性。
 *
 * 注意这些函数不知 NaN，凡含 NaN 输入均给 FALSE。
 * 见上。
 *
 * 对无穷输入也会给出合理答案；关键是避免算
 * Inf 减 Inf，做法是先排除相等情形再相减。
 * 见上。
 */

#define EPSILON					1.0E-06

#ifdef EPSILON
#define FPzero(A)				(fabs(A) <= EPSILON)

static inline bool
FPeq(double A, double B)
{
	return A == B || fabs(A - B) <= EPSILON;
}

static inline bool
FPne(double A, double B)
{
	return A != B && fabs(A - B) > EPSILON;
}

static inline bool
FPlt(double A, double B)
{
	return A + EPSILON < B;
}

static inline bool
FPle(double A, double B)
{
	return A <= B + EPSILON;
}

static inline bool
FPgt(double A, double B)
{
	return A > B + EPSILON;
}

static inline bool
FPge(double A, double B)
{
	return A + EPSILON >= B;
}
#else
#define FPzero(A)				((A) == 0)
#define FPeq(A,B)				((A) == (B))
#define FPne(A,B)				((A) != (B))
#define FPlt(A,B)				((A) < (B))
#define FPle(A,B)				((A) <= (B))
#define FPgt(A,B)				((A) > (B))
#define FPge(A,B)				((A) >= (B))
#endif

#define HYPOT(A, B)				pg_hypot(A, B)

/*---------------------------------------------------------------------
 * Point - (x,y)
 *-------------------------------------------------------------------*/
typedef struct
{
	float8		x,
				y;
} Point;


/*---------------------------------------------------------------------
 * LSEG - 由端点指定的直线段。
 *-------------------------------------------------------------------*/
typedef struct
{
	Point		p[2];
} LSEG;


/*---------------------------------------------------------------------
 * PATH - 由顶点指定。
 *-------------------------------------------------------------------*/
typedef struct
{
	int32		vl_len_;		/*  varlena 头（勿直接触碰！）  */
	int32		npts;
	int32		closed;			/* 是否闭合多边形? */
	int32		dummy;			/*  填充以使 double 对齐  */
	Point		p[FLEXIBLE_ARRAY_MEMBER];
} PATH;


/*---------------------------------------------------------------------
 * LINE - 由其一般方程 (Ax+By+C=0) 指定。
 *-------------------------------------------------------------------*/
typedef struct
{
	float8		A,
				B,
				C;
} LINE;


/*---------------------------------------------------------------------
 * BOX - 由两角点指定，并已排序
 *	以省后续计算。
 *-------------------------------------------------------------------*/
typedef struct
{
	Point		high,
				low;			/* 角点 POINTs */
} BOX;

/*---------------------------------------------------------------------
 * POLYGON - 由定义各点的 double 数组指定，并保
 *		留点数与包围盒以加速。
 *	见上。
 *-------------------------------------------------------------------*/
typedef struct
{
	int32		vl_len_;		/*  varlena 头（勿直接触碰！）  */
	int32		npts;
	BOX			boundbox;
	Point		p[FLEXIBLE_ARRAY_MEMBER];
} POLYGON;

/*---------------------------------------------------------------------
 * CIRCLE - 由中心点与半径指定。
 *-------------------------------------------------------------------*/
typedef struct
{
	Point		center;
	float8		radius;
} CIRCLE;

/*
 * fmgr 接口函数
 *
 * Path 与 Polygon 是可 toast 的 varlena 类型，其余
 * 只是定长按引用类型。
 */

static inline Point *
DatumGetPointP(Datum X)
{
	return (Point *) DatumGetPointer(X);
}
static inline Datum
PointPGetDatum(const Point *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_POINT_P(n) DatumGetPointP(PG_GETARG_DATUM(n))
#define PG_RETURN_POINT_P(x) return PointPGetDatum(x)

static inline LSEG *
DatumGetLsegP(Datum X)
{
	return (LSEG *) DatumGetPointer(X);
}
static inline Datum
LsegPGetDatum(const LSEG *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_LSEG_P(n) DatumGetLsegP(PG_GETARG_DATUM(n))
#define PG_RETURN_LSEG_P(x) return LsegPGetDatum(x)

static inline PATH *
DatumGetPathP(Datum X)
{
	return (PATH *) PG_DETOAST_DATUM(X);
}
static inline PATH *
DatumGetPathPCopy(Datum X)
{
	return (PATH *) PG_DETOAST_DATUM_COPY(X);
}
static inline Datum
PathPGetDatum(const PATH *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_PATH_P(n)		 DatumGetPathP(PG_GETARG_DATUM(n))
#define PG_GETARG_PATH_P_COPY(n) DatumGetPathPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_PATH_P(x)		 return PathPGetDatum(x)

static inline LINE *
DatumGetLineP(Datum X)
{
	return (LINE *) DatumGetPointer(X);
}
static inline Datum
LinePGetDatum(const LINE *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_LINE_P(n) DatumGetLineP(PG_GETARG_DATUM(n))
#define PG_RETURN_LINE_P(x) return LinePGetDatum(x)

static inline BOX *
DatumGetBoxP(Datum X)
{
	return (BOX *) DatumGetPointer(X);
}
static inline Datum
BoxPGetDatum(const BOX *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_BOX_P(n) DatumGetBoxP(PG_GETARG_DATUM(n))
#define PG_RETURN_BOX_P(x) return BoxPGetDatum(x)

static inline POLYGON *
DatumGetPolygonP(Datum X)
{
	return (POLYGON *) PG_DETOAST_DATUM(X);
}
static inline POLYGON *
DatumGetPolygonPCopy(Datum X)
{
	return (POLYGON *) PG_DETOAST_DATUM_COPY(X);
}
static inline Datum
PolygonPGetDatum(const POLYGON *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_POLYGON_P(n)		DatumGetPolygonP(PG_GETARG_DATUM(n))
#define PG_GETARG_POLYGON_P_COPY(n) DatumGetPolygonPCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_POLYGON_P(x)		return PolygonPGetDatum(x)

static inline CIRCLE *
DatumGetCircleP(Datum X)
{
	return (CIRCLE *) DatumGetPointer(X);
}
static inline Datum
CirclePGetDatum(const CIRCLE *X)
{
	return PointerGetDatum(X);
}
#define PG_GETARG_CIRCLE_P(n) DatumGetCircleP(PG_GETARG_DATUM(n))
#define PG_RETURN_CIRCLE_P(x) return CirclePGetDatum(x)


/*
 * geo_ops.c
 */

extern float8 pg_hypot(float8 x, float8 y);

#endif							/* GEO_DECLS_H */
