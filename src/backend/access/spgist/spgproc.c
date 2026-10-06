/*-------------------------------------------------------------------------
 *
 * spgproc.c
 *	  SP-GiST 操作符类的公共支持过程。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgproc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <math.h>

#include "access/spgist_private.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/geo_decls.h"

#define point_point_distance(p1,p2) \
	DatumGetFloat8(DirectFunctionCall2(point_distance, \
									   PointPGetDatum(p1), PointPGetDatum(p2)))

/* 点到框的距离，假定框与坐标轴对齐 */
static double
point_box_distance(Point *point, BOX *box)
{
	double		dx,
				dy;

	if (isnan(point->x) || isnan(box->low.x) ||
		isnan(point->y) || isnan(box->low.y))
		return get_float8_nan();

	if (point->x < box->low.x)
		dx = box->low.x - point->x;
	else if (point->x > box->high.x)
		dx = point->x - box->high.x;
	else
		dx = 0.0;

	if (point->y < box->low.y)
		dy = box->low.y - point->y;
	else if (point->y > box->high.y)
		dy = point->y - box->high.y;
	else
		dy = 0.0;

	return HYPOT(dx, dy);
}

/*
 * 返回给定键到排序扫描键数组的距离。叶子键
 * 预期为点，非叶子键预期为框。扫描键
 * 参数预期为点。
 */
double *
spg_key_orderbys_distances(Datum key, bool isLeaf,
						   ScanKey orderbys, int norderbys)
{
	int			sk_num;
	double	   *distances = (double *) palloc(norderbys * sizeof(double)),
			   *distance = distances;

	for (sk_num = 0; sk_num < norderbys; ++sk_num, ++orderbys, ++distance)
	{
		Point	   *point = DatumGetPointP(orderbys->sk_argument);

		*distance = isLeaf ? point_point_distance(point, DatumGetPointP(key))
			: point_box_distance(point, DatumGetBoxP(key));
	}

	return distances;
}

BOX *
box_copy(BOX *orig)
{
	BOX		   *result = palloc(sizeof(BOX));

	*result = *orig;
	return result;
}
