/*-------------------------------------------------------------------------
 *
 * geo_ops.c
 *	  二维几何运算
 *
 * 本模块实现几何函数与算子。
 * 几何类型（由简单到较复杂）为：
 *
 * 点
 * 直线
 *线段、矩形
 * 圆
 *
 * 多边形
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/geo_ops.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <limits.h>
#include <float.h>
#include <ctype.h>

#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/geo_decls.h"
#include "varatt.h"

/*
 * 类型构造器形式如下：
 *   void type_construct(Type *result, ...);
 *
 * 算子通常有类似如下的签名
 *   void type1_operator_type2(Type *result, Type1 *obj1, Type2 *obj2);
 *
 * 常见算子有：
 * * 交点：
 *   bool type1_interpt_type2(Point *result, Type1 *obj1, Type2 *obj2);
 *		返回两对象是否相交。若 *result 非 NULL，
 *		则将其设置为交点。
 *
 * * 包含：
 *   bool type1_contain_type2(Type1 *obj1, Type2 *obj2);
 *		返回 obj1 是否包含 obj2。
 *   bool type1_contain_type2(Type1 *contains_obj, Type1 *contained_obj);
 *		返回 obj1 是否包含 obj2（两类型相同时使用）
 *
 * * obj1 内或其上最近点到 obj2 的距离：
 *   float8 type1_closept_type2(Point *result, Type1 *obj1, Type2 *obj2);
 *		返回两对象间的最短距离。若 *result 非 NULL，
 *		则设为 obj1 内或其上距 obj2 最近的点。
 *
 * 这些函数可用于实现多个 SQL 层面的算子。
 * 例如，判断两条直线是否平行的方式是检查它们
 * 是否不相交。
 */

/*
 * 内部例程
 */

enum path_delim
{
	PATH_NONE, PATH_OPEN, PATH_CLOSED
};

/* 点相关例程 */
static inline void point_construct(Point *result, float8 x, float8 y);
static inline void point_add_point(Point *result, Point *pt1, Point *pt2);
static inline void point_sub_point(Point *result, Point *pt1, Point *pt2);
static inline void point_mul_point(Point *result, Point *pt1, Point *pt2);
static inline void point_div_point(Point *result, Point *pt1, Point *pt2);
static inline bool point_eq_point(Point *pt1, Point *pt2);
static inline float8 point_dt(Point *pt1, Point *pt2);
static inline float8 point_sl(Point *pt1, Point *pt2);
static int	point_inside(Point *p, int npts, Point *plist);

/* 直线相关例程 */
static inline void line_construct(LINE *result, Point *pt, float8 m);
static inline float8 line_sl(LINE *line);
static inline float8 line_invsl(LINE *line);
static bool line_interpt_line(Point *result, LINE *l1, LINE *l2);
static bool line_contain_point(LINE *line, Point *point);
static float8 line_closept_point(Point *result, LINE *line, Point *point);

/* 线段相关例程 */
static inline void statlseg_construct(LSEG *lseg, Point *pt1, Point *pt2);
static inline float8 lseg_sl(LSEG *lseg);
static inline float8 lseg_invsl(LSEG *lseg);
static bool lseg_interpt_line(Point *result, LSEG *lseg, LINE *line);
static bool lseg_interpt_lseg(Point *result, LSEG *l1, LSEG *l2);
static int	lseg_crossing(float8 x, float8 y, float8 prev_x, float8 prev_y);
static bool lseg_contain_point(LSEG *lseg, Point *pt);
static float8 lseg_closept_point(Point *result, LSEG *lseg, Point *pt);
static float8 lseg_closept_line(Point *result, LSEG *lseg, LINE *line);
static float8 lseg_closept_lseg(Point *result, LSEG *on_lseg, LSEG *to_lseg);

/* 矩形相关例程 */
static inline void box_construct(BOX *result, Point *pt1, Point *pt2);
static void box_cn(Point *center, BOX *box);
static bool box_ov(BOX *box1, BOX *box2);
static float8 box_ar(BOX *box);
static float8 box_ht(BOX *box);
static float8 box_wd(BOX *box);
static bool box_contain_point(BOX *box, Point *point);
static bool box_contain_box(BOX *contains_box, BOX *contained_box);
static bool box_contain_lseg(BOX *box, LSEG *lseg);
static bool box_interpt_lseg(Point *result, BOX *box, LSEG *lseg);
static float8 box_closept_point(Point *result, BOX *box, Point *pt);
static float8 box_closept_lseg(Point *result, BOX *box, LSEG *lseg);

/* 圆相关例程 */
static float8 circle_ar(CIRCLE *circle);

/* 多边形相关例程 */
static void make_bound_box(POLYGON *poly);
static void poly_to_circle(CIRCLE *result, POLYGON *poly);
static bool lseg_inside_poly(Point *a, Point *b, POLYGON *poly, int start);
static bool poly_contain_poly(POLYGON *contains_poly, POLYGON *contained_poly);
static bool plist_same(int npts, Point *p1, Point *p2);
static float8 dist_ppoly_internal(Point *pt, POLYGON *poly);

/* 编码与解码例程 */
static bool single_decode(char *num, float8 *x, char **endptr_p,
						  const char *type_name, const char *orig_string,
						  Node *escontext);
static void single_encode(float8 x, StringInfo str);
static bool pair_decode(char *str, float8 *x, float8 *y, char **endptr_p,
						const char *type_name, const char *orig_string,
						Node *escontext);
static void pair_encode(float8 x, float8 y, StringInfo str);
static int	pair_count(char *s, char delim);
static bool path_decode(char *str, bool opentype, int npts, Point *p,
						bool *isopen, char **endptr_p,
						const char *type_name, const char *orig_string,
						Node *escontext);
static char *path_encode(enum path_delim path_delim, int npts, Point *pt);


/*
 * 输入/输出字符串的分隔符。
 * LDELIM、RDELIM 和 DELIM 分别为左定界符、右定界符和分隔符。
 * LDELIM_EP、RDELIM_EP 为带端点路径的左、右定界符。
 */

#define LDELIM			'('
#define RDELIM			')'
#define DELIM			','
#define LDELIM_EP		'['
#define RDELIM_EP		']'
#define LDELIM_C		'<'
#define RDELIM_C		'>'
#define LDELIM_L		'{'
#define RDELIM_L		'}'


/*
 * 几何数据类型由点构成。
 * 这段代码力图在各数据类型间支持统一格式，
 *	以获得更可预期的用法与数据类型转换。
 * 基本单位是点。其他单位有线段、
 *	开放路径、矩形、闭合路径与多边形（应视为
 *	非相交的闭合路径）。
 *
 * 数据表示形式如下：
 *	点:				(x,y)
 *	线段:		[(x1,y1),(x2,y2)]
 *	矩形:	(x1,y1),(x2,y2)
 *	开放路径:[(x1,y1),...,(xn,yn)]
 *	闭合路径:	((x1,y1),...,(xn,yn))
 *	多边形:	((x1,y1),...,(xn,yn))
 *
 * 对矩形而言，各点为相对的角点，第一个点位于右上角。
 * 对闭合路径与多边形而言，各点应重新排序，
 *	以便快速且正确比较等值。
 *
 * XXX 或许应对复杂形状内部的点重排，
 *	以便加快内部运算，但应记录输入顺序，
 *	文本输出时再恢复该顺序 - tgl 97/01/16
 */

static bool
single_decode(char *num, float8 *x, char **endptr_p,
			  const char *type_name, const char *orig_string,
			  Node *escontext)
{
	*x = float8in_internal(num, endptr_p, type_name, orig_string, escontext);
	return (!SOFT_ERROR_OCCURRED(escontext));
}								/* single_decode() */

static void
single_encode(float8 x, StringInfo str)
{
	char	   *xstr = float8out_internal(x);

	appendStringInfoString(str, xstr);
	pfree(xstr);
}								/* single_encode() */

static bool
pair_decode(char *str, float8 *x, float8 *y, char **endptr_p,
			const char *type_name, const char *orig_string,
			Node *escontext)
{
	bool		has_delim;

	while (isspace((unsigned char) *str))
		str++;
	if ((has_delim = (*str == LDELIM)))
		str++;

	if (!single_decode(str, x, &str, type_name, orig_string, escontext))
		return false;

	if (*str++ != DELIM)
		goto fail;

	if (!single_decode(str, y, &str, type_name, orig_string, escontext))
		return false;

	if (has_delim)
	{
		if (*str++ != RDELIM)
			goto fail;
		while (isspace((unsigned char) *str))
			str++;
	}

	/* 需要时报告停止点，否则若字符串未结束则报错 */
	if (endptr_p)
		*endptr_p = str;
	else if (*str != '\0')
		goto fail;
	return true;

fail:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					type_name, orig_string)));
}

static void
pair_encode(float8 x, float8 y, StringInfo str)
{
	char	   *xstr = float8out_internal(x);
	char	   *ystr = float8out_internal(y);

	appendStringInfo(str, "%s,%s", xstr, ystr);
	pfree(xstr);
	pfree(ystr);
}

static bool
path_decode(char *str, bool opentype, int npts, Point *p,
			bool *isopen, char **endptr_p,
			const char *type_name, const char *orig_string,
			Node *escontext)
{
	int			depth = 0;
	char	   *cp;
	int			i;

	while (isspace((unsigned char) *str))
		str++;
	if ((*isopen = (*str == LDELIM_EP)))
	{
		/* 不允许左定界符？ */
		if (!opentype)
			goto fail;
		depth++;
		str++;
	}
	else if (*str == LDELIM)
	{
		cp = (str + 1);
		while (isspace((unsigned char) *cp))
			cp++;
		if (*cp == LDELIM)
		{
			depth++;
			str = cp;
		}
		else if (strrchr(str, LDELIM) == str)
		{
			depth++;
			str = cp;
		}
	}

	for (i = 0; i < npts; i++)
	{
		if (!pair_decode(str, &(p->x), &(p->y), &str, type_name, orig_string,
						 escontext))
			return false;
		if (*str == DELIM)
			str++;
		p++;
	}

	while (depth > 0)
	{
		if (*str == RDELIM || (*str == RDELIM_EP && *isopen && depth == 1))
		{
			depth--;
			str++;
			while (isspace((unsigned char) *str))
				str++;
		}
		else
			goto fail;
	}

	/* 需要时报告停止点，否则若字符串未结束则报错 */
	if (endptr_p)
		*endptr_p = str;
	else if (*str != '\0')
		goto fail;
	return true;

fail:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					type_name, orig_string)));
}								/* path_decode() */

static char *
path_encode(enum path_delim path_delim, int npts, Point *pt)
{
	StringInfoData str;
	int			i;

	initStringInfo(&str);

	switch (path_delim)
	{
		case PATH_CLOSED:
			appendStringInfoChar(&str, LDELIM);
			break;
		case PATH_OPEN:
			appendStringInfoChar(&str, LDELIM_EP);
			break;
		case PATH_NONE:
			break;
	}

	for (i = 0; i < npts; i++)
	{
		if (i > 0)
			appendStringInfoChar(&str, DELIM);
		appendStringInfoChar(&str, LDELIM);
		pair_encode(pt->x, pt->y, &str);
		appendStringInfoChar(&str, RDELIM);
		pt++;
	}

	switch (path_delim)
	{
		case PATH_CLOSED:
			appendStringInfoChar(&str, RDELIM);
			break;
		case PATH_OPEN:
			appendStringInfoChar(&str, RDELIM_EP);
			break;
		case PATH_NONE:
			break;
	}

	return str.data;
}								/* path_encode() */

/*-------------------------------------------------------------
 * pair_count - 统计点的数目
 * 允许如下记法：
 * '((1,2),(3,4))'
 * '(1,3,2,4)'
 * 要求字符串中分隔符数目为奇数
 *-------------------------------------------------------------*/
static int
pair_count(char *s, char delim)
{
	int			ndelim = 0;

	while ((s = strchr(s, delim)) != NULL)
	{
		ndelim++;
		s++;
	}
	return (ndelim % 2) ? ((ndelim + 1) / 2) : -1;
}


/***********************************************************************
 **
 **		二维矩形例程。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 格式化与转换例程。
 *---------------------------------------------------------*/

/*		box_in	-		将字符串转为内部形式。
 *
 *		外部格式：（矩形两角）
 *				"(f8, f8), (f8, f8)"
 *			也支持旧式 "(f8, f8, f8, f8)"
 */
Datum
box_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	BOX		   *box = (BOX *) palloc(sizeof(BOX));
	bool		isopen;
	float8		x,
				y;

	if (!path_decode(str, false, 2, &(box->high), &isopen, NULL, "box", str,
					 escontext))
		PG_RETURN_NULL();

	/* 必要时重排角点顺序… */
	if (float8_lt(box->high.x, box->low.x))
	{
		x = box->high.x;
		box->high.x = box->low.x;
		box->low.x = x;
	}
	if (float8_lt(box->high.y, box->low.y))
	{
		y = box->high.y;
		box->high.y = box->low.y;
		box->low.y = y;
	}

	PG_RETURN_BOX_P(box);
}

/*	box_out -	将矩形转为外部形式。
 */
Datum
box_out(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);

	PG_RETURN_CSTRING(path_encode(PATH_NONE, 2, &(box->high)));
}

/*
 *	box_recv	- 将外部二进制格式转为矩形
 */
Datum
box_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	BOX		   *box;
	float8		x,
				y;

	box = (BOX *) palloc(sizeof(BOX));

	box->high.x = pq_getmsgfloat8(buf);
	box->high.y = pq_getmsgfloat8(buf);
	box->low.x = pq_getmsgfloat8(buf);
	box->low.y = pq_getmsgfloat8(buf);

	/* 必要时重排角点顺序… */
	if (float8_lt(box->high.x, box->low.x))
	{
		x = box->high.x;
		box->high.x = box->low.x;
		box->low.x = x;
	}
	if (float8_lt(box->high.y, box->low.y))
	{
		y = box->high.y;
		box->high.y = box->low.y;
		box->low.y = y;
	}

	PG_RETURN_BOX_P(box);
}

/*
 *	box_send	- 将矩形转为二进制格式
 */
Datum
box_send(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, box->high.x);
	pq_sendfloat8(&buf, box->high.y);
	pq_sendfloat8(&buf, box->low.x);
	pq_sendfloat8(&buf, box->low.y);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*	box_construct	- 填充新矩形。
 */
static inline void
box_construct(BOX *result, Point *pt1, Point *pt2)
{
	if (float8_gt(pt1->x, pt2->x))
	{
		result->high.x = pt1->x;
		result->low.x = pt2->x;
	}
	else
	{
		result->high.x = pt2->x;
		result->low.x = pt1->x;
	}
	if (float8_gt(pt1->y, pt2->y))
	{
		result->high.y = pt1->y;
		result->low.y = pt2->y;
	}
	else
	{
		result->high.y = pt2->y;
		result->low.y = pt1->y;
	}
}


/*----------------------------------------------------------
 * 矩形的关系算子。
 *		<, >, <=, >= 和 == 基于矩形面积。
 *---------------------------------------------------------*/

/*		box_same	-		两矩形是否相同？
 */
Datum
box_same(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(point_eq_point(&box1->high, &box2->high) &&
				   point_eq_point(&box1->low, &box2->low));
}

/*		box_overlap	-		box1 与 box2 重叠？
 */
Datum
box_overlap(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(box_ov(box1, box2));
}

static bool
box_ov(BOX *box1, BOX *box2)
{
	return (FPle(box1->low.x, box2->high.x) &&
			FPle(box2->low.x, box1->high.x) &&
			FPle(box1->low.y, box2->high.y) &&
			FPle(box2->low.y, box1->high.y));
}

/*		box_left	-		box1 严格在 box2 左侧？
 */
Datum
box_left(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPlt(box1->high.x, box2->low.x));
}

/*		box_overleft	-	box1 的右边缘是否在 box2
 *						右边缘或其左侧？
 *
 *		这相当于时间范围末端的"小于等于"，
 *		用于以矩形存储的时间范围。
 */
Datum
box_overleft(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPle(box1->high.x, box2->high.x));
}

/*		box_right	-		box1 严格在 box2 右侧？
 */
Datum
box_right(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPgt(box1->low.x, box2->high.x));
}

/*		box_overright	-	box1 的左边缘是否在 box2
 *						左边缘或其右侧？
 *
 *		当时间范围以矩形存储时，这相当于时间范围的
 *		"大于等于"。
 */
Datum
box_overright(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPge(box1->low.x, box2->low.x));
}

/*		box_below	-		box1 严格在 box2 下方？
 */
Datum
box_below(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPlt(box1->high.y, box2->low.y));
}

/*		box_overbelow	-	box1 的上边缘是否在 box2
 *						上边缘或其下方？
 */
Datum
box_overbelow(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPle(box1->high.y, box2->high.y));
}

/*		box_above	-		box1 严格在 box2 上方？
 */
Datum
box_above(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPgt(box1->low.y, box2->high.y));
}

/*		box_overabove	-	box1 的下边缘是否在 box2
 *						下边缘或其上方？
 */
Datum
box_overabove(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPge(box1->low.y, box2->low.y));
}

/*		box_contained	-		box1是否被box2包含？
 */
Datum
box_contained(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(box_contain_box(box2, box1));
}

/*		box_contain		-		box1是否包含box2？
 */
Datum
box_contain(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(box_contain_box(box1, box2));
}

/*
 * 检查第二矩形是否在第一矩形内部或其边界上
 */
static bool
box_contain_box(BOX *contains_box, BOX *contained_box)
{
	return FPge(contains_box->high.x, contained_box->high.x) &&
		FPle(contains_box->low.x, contained_box->low.x) &&
		FPge(contains_box->high.y, contained_box->high.y) &&
		FPle(contains_box->low.y, contained_box->low.y);
}


/*		box_positionop	-
 *			box1是否整体在box2上/下方？
 *
 * box_below_eq 和 box_above_eq 是旧版本，其（可能
 * 错误地）接受边界相等的情形。由于这些
 * 代码与 box_left 和 box_right 不一致，已被弃用，
 * 且不受 PG 8.1 rtree 操作符类扩展的支持。
 */
Datum
box_below_eq(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPle(box1->high.y, box2->low.y));
}

Datum
box_above_eq(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPge(box1->low.y, box2->high.y));
}


/*		box_relop		-		area(box1) relop area(box2)在精度
 *						约束内是否成立？
 */
Datum
box_lt(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPlt(box_ar(box1), box_ar(box2)));
}

Datum
box_gt(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPgt(box_ar(box1), box_ar(box2)));
}

Datum
box_eq(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPeq(box_ar(box1), box_ar(box2)));
}

Datum
box_le(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPle(box_ar(box1), box_ar(box2)));
}

Datum
box_ge(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(FPge(box_ar(box1), box_ar(box2)));
}


/*----------------------------------------------------------
 * 矩形上的"算术"算子。
 *---------------------------------------------------------*/

/*		box_area		-		返回矩形的面积。
 */
Datum
box_area(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);

	PG_RETURN_FLOAT8(box_ar(box));
}


/*		box_width		-		返回矩形的宽度
 *						  （水平量）。
 */
Datum
box_width(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);

	PG_RETURN_FLOAT8(box_wd(box));
}


/*		box_height		-		返回矩形的高度
 *						  （垂直量）。
 */
Datum
box_height(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);

	PG_RETURN_FLOAT8(box_ht(box));
}


/*		box_distance	-		返回两个矩形的中心点
 *						  之间的距离。
 */
Datum
box_distance(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);
	Point		a,
				b;

	box_cn(&a, box1);
	box_cn(&b, box2);

	PG_RETURN_FLOAT8(point_dt(&a, &b));
}


/*		box_center		-		返回矩形的中心点。
 */
Datum
box_center(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *result = (Point *) palloc(sizeof(Point));

	box_cn(result, box);

	PG_RETURN_POINT_P(result);
}


/*		box_ar	-		返回矩形的面积。
 */
static float8
box_ar(BOX *box)
{
	return float8_mul(box_wd(box), box_ht(box));
}


/*		box_cn	-		把矩形的中心点存入 *center。
 */
static void
box_cn(Point *center, BOX *box)
{
	center->x = float8_div(float8_pl(box->high.x, box->low.x), 2.0);
	center->y = float8_div(float8_pl(box->high.y, box->low.y), 2.0);
}


/*		box_wd	-		返回矩形的宽度（长度）
 *						  （水平量）。
 */
static float8
box_wd(BOX *box)
{
	return float8_mi(box->high.x, box->low.x);
}


/*		box_ht	-		返回矩形的高度
 *						  （垂直量）。
 */
static float8
box_ht(BOX *box)
{
	return float8_mi(box->high.y, box->low.y);
}


/*----------------------------------------------------------
 * 特殊操作。
 *---------------------------------------------------------*/

/*		box_intersect	-
 *				返回两矩形相互重叠的部分，
 *				  不相交时为 NULL。
 */
Datum
box_intersect(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0);
	BOX		   *box2 = PG_GETARG_BOX_P(1);
	BOX		   *result;

	if (!box_ov(box1, box2))
		PG_RETURN_NULL();

	result = (BOX *) palloc(sizeof(BOX));

	result->high.x = float8_min(box1->high.x, box2->high.x);
	result->low.x = float8_max(box1->low.x, box2->low.x);
	result->high.y = float8_min(box1->high.y, box2->high.y);
	result->low.y = float8_max(box1->low.y, box2->low.y);

	PG_RETURN_BOX_P(result);
}


/*		box_diagonal	-
 *				返回一条恰好为"box"的
 *				  正斜率对角线。
 */
Datum
box_diagonal(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	LSEG	   *result = (LSEG *) palloc(sizeof(LSEG));

	statlseg_construct(result, &box->high, &box->low);

	PG_RETURN_LSEG_P(result);
}

/***********************************************************************
 **
 **		二维直线例程。
 **
 ***********************************************************************/

static bool
line_decode(char *s, const char *str, LINE *line, Node *escontext)
{
	/* s 已越过前导的 '{' */
	if (!single_decode(s, &line->A, &s, "line", str, escontext))
		return false;
	if (*s++ != DELIM)
		goto fail;
	if (!single_decode(s, &line->B, &s, "line", str, escontext))
		return false;
	if (*s++ != DELIM)
		goto fail;
	if (!single_decode(s, &line->C, &s, "line", str, escontext))
		return false;
	if (*s++ != RDELIM_L)
		goto fail;
	while (isspace((unsigned char) *s))
		s++;
	if (*s != '\0')
		goto fail;
	return true;

fail:
	ereturn(escontext, false,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"line", str)));
}

Datum
line_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	LINE	   *line = (LINE *) palloc(sizeof(LINE));
	LSEG		lseg;
	bool		isopen;
	char	   *s;

	s = str;
	while (isspace((unsigned char) *s))
		s++;
	if (*s == LDELIM_L)
	{
		if (!line_decode(s + 1, str, line, escontext))
			PG_RETURN_NULL();
		if (FPzero(line->A) && FPzero(line->B))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid line specification: A and B cannot both be zero")));
	}
	else
	{
		if (!path_decode(s, true, 2, &lseg.p[0], &isopen, NULL, "line", str,
						 escontext))
			PG_RETURN_NULL();
		if (point_eq_point(&lseg.p[0], &lseg.p[1]))
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid line specification: must be two distinct points")));

		/*
 *		 XXX lseg_sl() 和 line_construct() 可能抛出溢出/下溢
 *		 错误。最终我们应允许其软性出现，但
 *		 这种记法之痛目前大于其价值。
		 */
		line_construct(line, &lseg.p[0], lseg_sl(&lseg));
	}

	PG_RETURN_LINE_P(line);
}


Datum
line_out(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);
	char	   *astr = float8out_internal(line->A);
	char	   *bstr = float8out_internal(line->B);
	char	   *cstr = float8out_internal(line->C);

	PG_RETURN_CSTRING(psprintf("%c%s%c%s%c%s%c", LDELIM_L, astr, DELIM, bstr,
							   DELIM, cstr, RDELIM_L));
}

/*
 *	line_recv	- 将外部二进制格式转为直线
 */
Datum
line_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	LINE	   *line;

	line = (LINE *) palloc(sizeof(LINE));

	line->A = pq_getmsgfloat8(buf);
	line->B = pq_getmsgfloat8(buf);
	line->C = pq_getmsgfloat8(buf);

	if (FPzero(line->A) && FPzero(line->B))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid line specification: A and B cannot both be zero")));

	PG_RETURN_LINE_P(line);
}

/*
 *	line_send	- 将直线转为二进制格式
 */
Datum
line_send(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, line->A);
	pq_sendfloat8(&buf, line->B);
	pq_sendfloat8(&buf, line->C);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 * 直线方程到内部形式的转换例程。
 *		内部形式：Ax+By+C=0
 *---------------------------------------------------------*/

/*
 * 由点和斜率填充已分配的 LINE 结构
 */
static inline void
line_construct(LINE *result, Point *pt, float8 m)
{
	if (isinf(m))
	{
		/* 垂直：用 "x = C" */
		result->A = -1.0;
		result->B = 0.0;
		result->C = pt->x;
	}
	else if (m == 0)
	{
		/* 水平：用 "y = C" */
		result->A = 0.0;
		result->B = -1.0;
		result->C = pt->y;
	}
	else
	{
		/* 用 "mx - y + yinter = 0" */
		result->A = m;
		result->B = -1.0;
		result->C = float8_mi(pt->y, float8_mul(m, pt->x));
		/* 在某些平台上，前面的表达式可能产生 -0 */
		if (result->C == 0.0)
			result->C = 0.0;
	}
}

/* line_construct_pp()
 * 两点
 */
Datum
line_construct_pp(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);
	LINE	   *result = (LINE *) palloc(sizeof(LINE));

	if (point_eq_point(pt1, pt2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid line specification: must be two distinct points")));

	line_construct(result, pt1, point_sl(pt1, pt2));

	PG_RETURN_LINE_P(result);
}


/*----------------------------------------------------------
 * 相对位置例程。
 *---------------------------------------------------------*/

Datum
line_intersect(PG_FUNCTION_ARGS)
{
	LINE	   *l1 = PG_GETARG_LINE_P(0);
	LINE	   *l2 = PG_GETARG_LINE_P(1);

	PG_RETURN_BOOL(line_interpt_line(NULL, l1, l2));
}

Datum
line_parallel(PG_FUNCTION_ARGS)
{
	LINE	   *l1 = PG_GETARG_LINE_P(0);
	LINE	   *l2 = PG_GETARG_LINE_P(1);

	PG_RETURN_BOOL(!line_interpt_line(NULL, l1, l2));
}

Datum
line_perp(PG_FUNCTION_ARGS)
{
	LINE	   *l1 = PG_GETARG_LINE_P(0);
	LINE	   *l2 = PG_GETARG_LINE_P(1);

	if (FPzero(l1->A))
		PG_RETURN_BOOL(FPzero(l2->B));
	if (FPzero(l2->A))
		PG_RETURN_BOOL(FPzero(l1->B));
	if (FPzero(l1->B))
		PG_RETURN_BOOL(FPzero(l2->A));
	if (FPzero(l2->B))
		PG_RETURN_BOOL(FPzero(l1->A));

	PG_RETURN_BOOL(FPeq(float8_div(float8_mul(l1->A, l2->A),
								   float8_mul(l1->B, l2->B)), -1.0));
}

Datum
line_vertical(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);

	PG_RETURN_BOOL(FPzero(line->B));
}

Datum
line_horizontal(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);

	PG_RETURN_BOOL(FPzero(line->A));
}


/*
 * 检查两条直线是否相同
 */
Datum
line_eq(PG_FUNCTION_ARGS)
{
	LINE	   *l1 = PG_GETARG_LINE_P(0);
	LINE	   *l2 = PG_GETARG_LINE_P(1);
	float8		ratio;

	/* 若涉及任何 NaN，则坚持精确相等 */
	if (unlikely(isnan(l1->A) || isnan(l1->B) || isnan(l1->C) ||
				 isnan(l2->A) || isnan(l2->B) || isnan(l2->C)))
	{
		PG_RETURN_BOOL(float8_eq(l1->A, l2->A) &&
					   float8_eq(l1->B, l2->B) &&
					   float8_eq(l1->C, l2->C));
	}

	/* 否则，参数成比例的直线相同 */
	if (!FPzero(l2->A))
		ratio = float8_div(l1->A, l2->A);
	else if (!FPzero(l2->B))
		ratio = float8_div(l1->B, l2->B);
	else if (!FPzero(l2->C))
		ratio = float8_div(l1->C, l2->C);
	else
		ratio = 1.0;

	PG_RETURN_BOOL(FPeq(l1->A, float8_mul(ratio, l2->A)) &&
				   FPeq(l1->B, float8_mul(ratio, l2->B)) &&
				   FPeq(l1->C, float8_mul(ratio, l2->C)));
}


/*----------------------------------------------------------
 * 直线算术例程。
 *---------------------------------------------------------*/

/*
 * 返回直线的斜率
 */
static inline float8
line_sl(LINE *line)
{
	if (FPzero(line->A))
		return 0.0;
	if (FPzero(line->B))
		return get_float8_infinity();
	return float8_div(line->A, -line->B);
}


/*
 * 返回直线的反向斜率
 */
static inline float8
line_invsl(LINE *line)
{
	if (FPzero(line->A))
		return get_float8_infinity();
	if (FPzero(line->B))
		return 0.0;
	return float8_div(line->B, line->A);
}


/* line_distance()
 * 两直线间的距离。
 */
Datum
line_distance(PG_FUNCTION_ARGS)
{
	LINE	   *l1 = PG_GETARG_LINE_P(0);
	LINE	   *l2 = PG_GETARG_LINE_P(1);
	float8		ratio;

	if (line_interpt_line(NULL, l1, l2))	/* 相交？ */
		PG_RETURN_FLOAT8(0.0);

	if (!FPzero(l1->A) && !isnan(l1->A) && !FPzero(l2->A) && !isnan(l2->A))
		ratio = float8_div(l1->A, l2->A);
	else if (!FPzero(l1->B) && !isnan(l1->B) && !FPzero(l2->B) && !isnan(l2->B))
		ratio = float8_div(l1->B, l2->B);
	else
		ratio = 1.0;

	PG_RETURN_FLOAT8(float8_div(fabs(float8_mi(l1->C,
											   float8_mul(ratio, l2->C))),
								HYPOT(l1->A, l1->B)));
}

/* line_interpt()
 * 两直线 l1、l2 的交点（若有）
 */
Datum
line_interpt(PG_FUNCTION_ARGS)
{
	LINE	   *l1 = PG_GETARG_LINE_P(0);
	LINE	   *l2 = PG_GETARG_LINE_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	if (!line_interpt_line(result, l1, l2))
		PG_RETURN_NULL();
	PG_RETURN_POINT_P(result);
}

/*
 * line_interpt 的内部版本
 *
 * 返回两直线是否相交。若 *result 非 NULL，则将其
 * 设置为交点。
 *
 * 注意：若两直线相同，则会发现它们平行，
 * 并报告"无交点"。这有点怪，但由于
 * 不存在*唯一*交点，或许是恰当的行为。
 *
 * 若直线含 NaN 常量，我们将返回 true，且交点
 * 将会具有 NaN 坐标。此时我们不应返回 false，
 * 因为那意味着两直线平行。
 */
static bool
line_interpt_line(Point *result, LINE *l1, LINE *l2)
{
	float8		x,
				y;

	if (!FPzero(l1->B))
	{
		if (FPeq(l2->A, float8_mul(l1->A, float8_div(l2->B, l1->B))))
			return false;

		x = float8_div(float8_mi(float8_mul(l1->B, l2->C),
								 float8_mul(l2->B, l1->C)),
					   float8_mi(float8_mul(l1->A, l2->B),
								 float8_mul(l2->A, l1->B)));
		y = float8_div(-float8_pl(float8_mul(l1->A, x), l1->C), l1->B);
	}
	else if (!FPzero(l2->B))
	{
		if (FPeq(l1->A, float8_mul(l2->A, float8_div(l1->B, l2->B))))
			return false;

		x = float8_div(float8_mi(float8_mul(l2->B, l1->C),
								 float8_mul(l1->B, l2->C)),
					   float8_mi(float8_mul(l2->A, l1->B),
								 float8_mul(l1->A, l2->B)));
		y = float8_div(-float8_pl(float8_mul(l2->A, x), l2->C), l2->B);
	}
	else
		return false;

	/* 在某些平台上，前面的表达式可能产生 -0。 */
	if (x == 0.0)
		x = 0.0;
	if (y == 0.0)
		y = 0.0;

	if (result != NULL)
		point_construct(result, x, y);

	return true;
}


/***********************************************************************
 **
 **		二维路径（线段序列，也
 **				称为`折线'）。
 **
 **				这并非通用的几何路径程序包，
 **				当然也包含多边形；此处的重点是
 **				（例如）在布线中的实用性。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 字符串与路径的相互转换。
 *		外部格式：
 *				"((xcoord, ycoord),... )"
 *				"[(xcoord, ycoord),... ]"
 *				"(xcoord, ycoord),... "
 *				"[xcoord, ycoord,... ]"
 *		同时支持旧格式：
 *				"(closed, npts, xcoord, ycoord,... )"
 *---------------------------------------------------------*/

Datum
path_area(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);
	float8		area = 0.0;
	int			i,
				j;

	if (!path->closed)
		PG_RETURN_NULL();

	for (i = 0; i < path->npts; i++)
	{
		j = (i + 1) % path->npts;
		area = float8_pl(area, float8_mul(path->p[i].x, path->p[j].y));
		area = float8_mi(area, float8_mul(path->p[i].y, path->p[j].x));
	}

	PG_RETURN_FLOAT8(float8_div(fabs(area), 2.0));
}


Datum
path_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	PATH	   *path;
	bool		isopen;
	char	   *s;
	int			npts;
	int			size;
	int			base_size;
	int			depth = 0;

	if ((npts = pair_count(str, ',')) <= 0)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"path", str)));

	s = str;
	while (isspace((unsigned char) *s))
		s++;

	/* 跳过单个前导括号 */
	if ((*s == LDELIM) && (strrchr(s, LDELIM) == s))
	{
		s++;
		depth++;
	}

	base_size = sizeof(path->p[0]) * npts;
	size = offsetof(PATH, p) + base_size;

	/* 检查整数溢出 */
	if (base_size / npts != sizeof(path->p[0]) || size <= base_size)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many points requested")));

	path = (PATH *) palloc(size);

	SET_VARSIZE(path, size);
	path->npts = npts;

	if (!path_decode(s, true, npts, &(path->p[0]), &isopen, &s, "path", str,
					 escontext))
		PG_RETURN_NULL();

	if (depth >= 1)
	{
		if (*s++ != RDELIM)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"path", str)));
		while (isspace((unsigned char) *s))
			s++;
	}
	if (*s != '\0')
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"path", str)));

	path->closed = (!isopen);
	/* 防止未用填充字节的不稳定性 */
	path->dummy = 0;

	PG_RETURN_PATH_P(path);
}


Datum
path_out(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);

	PG_RETURN_CSTRING(path_encode(path->closed ? PATH_CLOSED : PATH_OPEN, path->npts, path->p));
}

/*
 *	path_recv	- 将外部二进制格式转为路径
 *
 * 外部表示为闭合标志（一个布尔字节）、int32 的
 * 点数，以及各点。
 */
Datum
path_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	PATH	   *path;
	int			closed;
	int32		npts;
	int32		i;
	int			size;

	closed = pq_getmsgbyte(buf);
	npts = pq_getmsgint(buf, sizeof(int32));
	if (npts <= 0 || npts >= (int32) ((INT_MAX - offsetof(PATH, p)) / sizeof(Point)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid number of points in external \"path\" value")));

	size = offsetof(PATH, p) + sizeof(path->p[0]) * npts;
	path = (PATH *) palloc(size);

	SET_VARSIZE(path, size);
	path->npts = npts;
	path->closed = (closed ? 1 : 0);
	/* 防止未用填充字节的不稳定性 */
	path->dummy = 0;

	for (i = 0; i < npts; i++)
	{
		path->p[i].x = pq_getmsgfloat8(buf);
		path->p[i].y = pq_getmsgfloat8(buf);
	}

	PG_RETURN_PATH_P(path);
}

/*
 *	path_send	- 将路径转为二进制格式
 */
Datum
path_send(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);
	StringInfoData buf;
	int32		i;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, path->closed ? 1 : 0);
	pq_sendint32(&buf, path->npts);
	for (i = 0; i < path->npts; i++)
	{
		pq_sendfloat8(&buf, path->p[i].x);
		pq_sendfloat8(&buf, path->p[i].y);
	}
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 * 关系算子。
 *		这些基于路径的基数，
 *		虽然听着蠢。
 *
 *		更佳关系算子与访问方法将至。
 *---------------------------------------------------------*/

Datum
path_n_lt(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);

	PG_RETURN_BOOL(p1->npts < p2->npts);
}

Datum
path_n_gt(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);

	PG_RETURN_BOOL(p1->npts > p2->npts);
}

Datum
path_n_eq(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);

	PG_RETURN_BOOL(p1->npts == p2->npts);
}

Datum
path_n_le(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);

	PG_RETURN_BOOL(p1->npts <= p2->npts);
}

Datum
path_n_ge(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);

	PG_RETURN_BOOL(p1->npts >= p2->npts);
}

/*----------------------------------------------------------
 * 转换算子。
 *---------------------------------------------------------*/

Datum
path_isclosed(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);

	PG_RETURN_BOOL(path->closed);
}

Datum
path_isopen(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);

	PG_RETURN_BOOL(!path->closed);
}

Datum
path_npoints(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);

	PG_RETURN_INT32(path->npts);
}


Datum
path_close(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P_COPY(0);

	path->closed = true;

	PG_RETURN_PATH_P(path);
}

Datum
path_open(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P_COPY(0);

	path->closed = false;

	PG_RETURN_PATH_P(path);
}


/* path_inter -
 *		p1 与 p2 是否在某点相交？
 *		用外接框做快速 (O(n)) 检查，然后再做
 *		O(n^2) 逐边迭代检查。
 */
Datum
path_inter(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);
	BOX			b1,
				b2;
	int			i,
				j;
	LSEG		seg1,
				seg2;

	Assert(p1->npts > 0 && p2->npts > 0);

	b1.high.x = b1.low.x = p1->p[0].x;
	b1.high.y = b1.low.y = p1->p[0].y;
	for (i = 1; i < p1->npts; i++)
	{
		b1.high.x = float8_max(p1->p[i].x, b1.high.x);
		b1.high.y = float8_max(p1->p[i].y, b1.high.y);
		b1.low.x = float8_min(p1->p[i].x, b1.low.x);
		b1.low.y = float8_min(p1->p[i].y, b1.low.y);
	}
	b2.high.x = b2.low.x = p2->p[0].x;
	b2.high.y = b2.low.y = p2->p[0].y;
	for (i = 1; i < p2->npts; i++)
	{
		b2.high.x = float8_max(p2->p[i].x, b2.high.x);
		b2.high.y = float8_max(p2->p[i].y, b2.high.y);
		b2.low.x = float8_min(p2->p[i].x, b2.low.x);
		b2.low.y = float8_min(p2->p[i].y, b2.low.y);
	}
	if (!box_ov(&b1, &b2))
		PG_RETURN_BOOL(false);

	/* 逐对检查线段相交 */
	for (i = 0; i < p1->npts; i++)
	{
		int			iprev;

		if (i > 0)
			iprev = i - 1;
		else
		{
			if (!p1->closed)
				continue;
			iprev = p1->npts - 1;	/* 包含闭合段 */
		}

		for (j = 0; j < p2->npts; j++)
		{
			int			jprev;

			if (j > 0)
				jprev = j - 1;
			else
			{
				if (!p2->closed)
					continue;
				jprev = p2->npts - 1;	/* 包含闭合段 */
			}

			statlseg_construct(&seg1, &p1->p[iprev], &p1->p[i]);
			statlseg_construct(&seg2, &p2->p[jprev], &p2->p[j]);
			if (lseg_interpt_lseg(NULL, &seg1, &seg2))
				PG_RETURN_BOOL(true);
		}
	}

	/* 落到此处即无任何两线段相交 */
	PG_RETURN_BOOL(false);
}

/* path_distance()
 * 这实质上对两条路径中的线段做笛卡尔积，
 *	并求任意两线段间的最小距离
 */
Datum
path_distance(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);
	float8		min = 0.0;		/* 初始化以让编译器安静 */
	bool		have_min = false;
	float8		tmp;
	int			i,
				j;
	LSEG		seg1,
				seg2;

	for (i = 0; i < p1->npts; i++)
	{
		int			iprev;

		if (i > 0)
			iprev = i - 1;
		else
		{
			if (!p1->closed)
				continue;
			iprev = p1->npts - 1;	/* 包含闭合段 */
		}

		for (j = 0; j < p2->npts; j++)
		{
			int			jprev;

			if (j > 0)
				jprev = j - 1;
			else
			{
				if (!p2->closed)
					continue;
				jprev = p2->npts - 1;	/* 包含闭合段 */
			}

			statlseg_construct(&seg1, &p1->p[iprev], &p1->p[i]);
			statlseg_construct(&seg2, &p2->p[jprev], &p2->p[j]);

			tmp = lseg_closept_lseg(NULL, &seg1, &seg2);
			if (!have_min || float8_lt(tmp, min))
			{
				min = tmp;
				have_min = true;
			}
		}
	}

	if (!have_min)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(min);
}


/*----------------------------------------------------------
 * "算术"操作。
 *---------------------------------------------------------*/

Datum
path_length(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);
	float8		result = 0.0;
	int			i;

	for (i = 0; i < path->npts; i++)
	{
		int			iprev;

		if (i > 0)
			iprev = i - 1;
		else
		{
			if (!path->closed)
				continue;
			iprev = path->npts - 1; /* 包含闭合段 */
		}

		result = float8_pl(result, point_dt(&path->p[iprev], &path->p[i]));
	}

	PG_RETURN_FLOAT8(result);
}

/***********************************************************************
 **
 **		二维点例程。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 字符串与点的相互转换。
 *		外部格式：
 *				"(x,y)"
 *				"x,y"
 *---------------------------------------------------------*/

Datum
point_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Point	   *point = (Point *) palloc(sizeof(Point));

	/* 忽略 pair_decode 失败，因为我们的返回值无关紧要 */
	pair_decode(str, &point->x, &point->y, NULL, "point", str, fcinfo->context);
	PG_RETURN_POINT_P(point);
}

Datum
point_out(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);

	PG_RETURN_CSTRING(path_encode(PATH_NONE, 1, pt));
}

/*
 *		point_recv			- 将外部二进制格式转为点
 */
Datum
point_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Point	   *point;

	point = (Point *) palloc(sizeof(Point));
	point->x = pq_getmsgfloat8(buf);
	point->y = pq_getmsgfloat8(buf);
	PG_RETURN_POINT_P(point);
}

/*
 *		point_send			- 将点转为二进制格式
 */
Datum
point_send(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, pt->x);
	pq_sendfloat8(&buf, pt->y);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * 初始化一个点
 */
static inline void
point_construct(Point *result, float8 x, float8 y)
{
	result->x = x;
	result->y = y;
}


/*----------------------------------------------------------
 * 点的关系算子。
 *		由于我们确有坐标"相等"的概念，
 *		即按给定精度 (point_vert, point_horiz)，
 *		其他算子必须保持这一概念。这意味着
 *		严格来说，除非 EPSILON=0.0，结果可能
 *		是个谎言。
 *---------------------------------------------------------*/

Datum
point_left(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(FPlt(pt1->x, pt2->x));
}

Datum
point_right(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(FPgt(pt1->x, pt2->x));
}

Datum
point_above(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(FPgt(pt1->y, pt2->y));
}

Datum
point_below(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(FPlt(pt1->y, pt2->y));
}

Datum
point_vert(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(FPeq(pt1->x, pt2->x));
}

Datum
point_horiz(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(FPeq(pt1->y, pt2->y));
}

Datum
point_eq(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(point_eq_point(pt1, pt2));
}

Datum
point_ne(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(!point_eq_point(pt1, pt2));
}


/*
 * 检查两个点是否相同
 */
static inline bool
point_eq_point(Point *pt1, Point *pt2)
{
	/* 若涉及任何 NaN，则坚持精确相等 */
	if (unlikely(isnan(pt1->x) || isnan(pt1->y) ||
				 isnan(pt2->x) || isnan(pt2->y)))
		return (float8_eq(pt1->x, pt2->x) && float8_eq(pt1->y, pt2->y));

	return (FPeq(pt1->x, pt2->x) && FPeq(pt1->y, pt2->y));
}


/*----------------------------------------------------------
 * 点上的"算术"算子。
 *---------------------------------------------------------*/

Datum
point_distance(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(point_dt(pt1, pt2));
}

static inline float8
point_dt(Point *pt1, Point *pt2)
{
	return HYPOT(float8_mi(pt1->x, pt2->x), float8_mi(pt1->y, pt2->y));
}

Datum
point_slope(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(point_sl(pt1, pt2));
}


/*
 * 返回两点的斜率
 *
 * 注意当两点相同时此函数返回 Inf。
 */
static inline float8
point_sl(Point *pt1, Point *pt2)
{
	if (FPeq(pt1->x, pt2->x))
		return get_float8_infinity();
	if (FPeq(pt1->y, pt2->y))
		return 0.0;
	return float8_div(float8_mi(pt1->y, pt2->y), float8_mi(pt1->x, pt2->x));
}


/*
 * 返回两点的反向斜率
 *
 * 注意当两点相同时此函数返回 0.0。
 */
static inline float8
point_invsl(Point *pt1, Point *pt2)
{
	if (FPeq(pt1->x, pt2->x))
		return 0.0;
	if (FPeq(pt1->y, pt2->y))
		return get_float8_infinity();
	return float8_div(float8_mi(pt1->x, pt2->x), float8_mi(pt2->y, pt1->y));
}


/***********************************************************************
 **
 **		二维线段例程。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 字符串与线段的相互转换。
 *		外部形式: "[(x1, y1), (x2, y2)]"
 *						"(x1, y1), (x2, y2)"
 *						"x1, y1, x2, y2"
 *		闭合形式	"((x1, y1), (x2, y2))"
 *		旧形式	"(x1, y1, x2, y2)"
 *---------------------------------------------------------*/

Datum
lseg_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	LSEG	   *lseg = (LSEG *) palloc(sizeof(LSEG));
	bool		isopen;

	if (!path_decode(str, true, 2, &lseg->p[0], &isopen, NULL, "lseg", str,
					 escontext))
		PG_RETURN_NULL();

	PG_RETURN_LSEG_P(lseg);
}


Datum
lseg_out(PG_FUNCTION_ARGS)
{
	LSEG	   *ls = PG_GETARG_LSEG_P(0);

	PG_RETURN_CSTRING(path_encode(PATH_OPEN, 2, &ls->p[0]));
}

/*
 *	lseg_recv	- 将外部二进制格式转为线段
 */
Datum
lseg_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	LSEG	   *lseg;

	lseg = (LSEG *) palloc(sizeof(LSEG));

	lseg->p[0].x = pq_getmsgfloat8(buf);
	lseg->p[0].y = pq_getmsgfloat8(buf);
	lseg->p[1].x = pq_getmsgfloat8(buf);
	lseg->p[1].y = pq_getmsgfloat8(buf);

	PG_RETURN_LSEG_P(lseg);
}

/*
 *	lseg_send	- 将线段转为二进制格式
 */
Datum
lseg_send(PG_FUNCTION_ARGS)
{
	LSEG	   *ls = PG_GETARG_LSEG_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, ls->p[0].x);
	pq_sendfloat8(&buf, ls->p[0].y);
	pq_sendfloat8(&buf, ls->p[1].x);
	pq_sendfloat8(&buf, ls->p[1].y);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/* lseg_construct -
 *		由两点构成 LSEG。
 */
Datum
lseg_construct(PG_FUNCTION_ARGS)
{
	Point	   *pt1 = PG_GETARG_POINT_P(0);
	Point	   *pt2 = PG_GETARG_POINT_P(1);
	LSEG	   *result = (LSEG *) palloc(sizeof(LSEG));

	statlseg_construct(result, pt1, pt2);

	PG_RETURN_LSEG_P(result);
}

/* 类似 lseg_construct，但假定空间已分配 */
static inline void
statlseg_construct(LSEG *lseg, Point *pt1, Point *pt2)
{
	lseg->p[0].x = pt1->x;
	lseg->p[0].y = pt1->y;
	lseg->p[1].x = pt2->x;
	lseg->p[1].y = pt2->y;
}


/*
 * 返回线段的斜率
 */
static inline float8
lseg_sl(LSEG *lseg)
{
	return point_sl(&lseg->p[0], &lseg->p[1]);
}


/*
 * 返回线段的反向斜率
 */
static inline float8
lseg_invsl(LSEG *lseg)
{
	return point_invsl(&lseg->p[0], &lseg->p[1]);
}


Datum
lseg_length(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);

	PG_RETURN_FLOAT8(point_dt(&lseg->p[0], &lseg->p[1]));
}

/*----------------------------------------------------------
 * 相对位置例程。
 *---------------------------------------------------------*/

/*
 **  求两直线交点，看它是否落在两线段上。
 **
 */
Datum
lseg_intersect(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(lseg_interpt_lseg(NULL, l1, l2));
}


Datum
lseg_parallel(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(FPeq(lseg_sl(l1), lseg_sl(l2)));
}

/*
 * 判断两条线段是否垂直。
 */
Datum
lseg_perp(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(FPeq(lseg_sl(l1), lseg_invsl(l2)));
}

Datum
lseg_vertical(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);

	PG_RETURN_BOOL(FPeq(lseg->p[0].x, lseg->p[1].x));
}

Datum
lseg_horizontal(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);

	PG_RETURN_BOOL(FPeq(lseg->p[0].y, lseg->p[1].y));
}


Datum
lseg_eq(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(point_eq_point(&l1->p[0], &l2->p[0]) &&
				   point_eq_point(&l1->p[1], &l2->p[1]));
}

Datum
lseg_ne(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(!point_eq_point(&l1->p[0], &l2->p[0]) ||
				   !point_eq_point(&l1->p[1], &l2->p[1]));
}

Datum
lseg_lt(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(FPlt(point_dt(&l1->p[0], &l1->p[1]),
						point_dt(&l2->p[0], &l2->p[1])));
}

Datum
lseg_le(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(FPle(point_dt(&l1->p[0], &l1->p[1]),
						point_dt(&l2->p[0], &l2->p[1])));
}

Datum
lseg_gt(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(FPgt(point_dt(&l1->p[0], &l1->p[1]),
						point_dt(&l2->p[0], &l2->p[1])));
}

Datum
lseg_ge(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(FPge(point_dt(&l1->p[0], &l1->p[1]),
						point_dt(&l2->p[0], &l2->p[1])));
}


/*----------------------------------------------------------
 * 直线算术例程。
 *---------------------------------------------------------*/

/* lseg_distance -
 *		若两线段不相交，则最近的点
 *		将是从某一线段的端点到另一
 *		段。
 */
Datum
lseg_distance(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);

	PG_RETURN_FLOAT8(lseg_closept_lseg(NULL, l1, l2));
}


Datum
lseg_center(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	result->x = float8_div(float8_pl(lseg->p[0].x, lseg->p[1].x), 2.0);
	result->y = float8_div(float8_pl(lseg->p[0].y, lseg->p[1].y), 2.0);

	PG_RETURN_POINT_P(result);
}


/*
 * 返回两线段是否相交。若 *result 非 NULL，
 * 则将其设置为交点。
 *
 * 此函数几乎完全对称，尽管看起来并不像。
 * 另一半参见 lseg_interpt_line()。
 */
static bool
lseg_interpt_lseg(Point *result, LSEG *l1, LSEG *l2)
{
	Point		interpt;
	LINE		tmp;

	line_construct(&tmp, &l2->p[0], lseg_sl(l2));
	if (!lseg_interpt_line(&interpt, l1, &tmp))
		return false;

	/*
	 * 若直线交点不在 l2 内，则完全不存在有效的
	 * 线段交点。
	 */
	if (!lseg_contain_point(l2, &interpt))
		return false;

	if (result != NULL)
		*result = interpt;

	return true;
}

Datum
lseg_interpt(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	if (!lseg_interpt_lseg(result, l1, l2))
		PG_RETURN_NULL();
	PG_RETURN_POINT_P(result);
}

/***********************************************************************
 **
 **		不同类型二维对象的位置比较例程。
 **
 **
 ***********************************************************************/

/*---------------------------------------------------------------------
 *		dist_
 *				一个对象到另一对象最小距离。
 *-------------------------------------------------------------------*/

/*
 * 点到直线的距离
 */
Datum
dist_pl(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	LINE	   *line = PG_GETARG_LINE_P(1);

	PG_RETURN_FLOAT8(line_closept_point(NULL, line, pt));
}

/*
 * 直线到点的距离
 */
Datum
dist_lp(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);
	Point	   *pt = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(line_closept_point(NULL, line, pt));
}

/*
 * 点到线段的距离
 */
Datum
dist_ps(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	LSEG	   *lseg = PG_GETARG_LSEG_P(1);

	PG_RETURN_FLOAT8(lseg_closept_point(NULL, lseg, pt));
}

/*
 * 线段到点的距离
 */
Datum
dist_sp(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	Point	   *pt = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(lseg_closept_point(NULL, lseg, pt));
}

static float8
dist_ppath_internal(Point *pt, PATH *path)
{
	float8		result = 0.0;	/* 让编译器安静 */
	bool		have_min = false;
	float8		tmp;
	int			i;
	LSEG		lseg;

	Assert(path->npts > 0);

	/*
	 * 点到路径的距离是从该点到其任一
	 * 组成线段的最小距离。
	 */
	for (i = 0; i < path->npts; i++)
	{
		int			iprev;

		if (i > 0)
			iprev = i - 1;
		else
		{
			if (!path->closed)
				continue;
			iprev = path->npts - 1; /* 包含闭合线段 */
		}

		statlseg_construct(&lseg, &path->p[iprev], &path->p[i]);
		tmp = lseg_closept_point(NULL, &lseg, pt);
		if (!have_min || float8_lt(tmp, result))
		{
			result = tmp;
			have_min = true;
		}
	}

	return result;
}

/*
 * 点到路径的距离
 */
Datum
dist_ppath(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	PATH	   *path = PG_GETARG_PATH_P(1);

	PG_RETURN_FLOAT8(dist_ppath_internal(pt, path));
}

/*
 * 路径到点的距离
 */
Datum
dist_pathp(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);
	Point	   *pt = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(dist_ppath_internal(pt, path));
}

/*
 * 点到矩形的距离
 */
Datum
dist_pb(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);

	PG_RETURN_FLOAT8(box_closept_point(NULL, box, pt));
}

/*
 * 矩形到点的距离
 */
Datum
dist_bp(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *pt = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(box_closept_point(NULL, box, pt));
}

/*
 * 线段到直线的距离
 */
Datum
dist_sl(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	LINE	   *line = PG_GETARG_LINE_P(1);

	PG_RETURN_FLOAT8(lseg_closept_line(NULL, lseg, line));
}

/*
 * 直线到线段的距离
 */
Datum
dist_ls(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);
	LSEG	   *lseg = PG_GETARG_LSEG_P(1);

	PG_RETURN_FLOAT8(lseg_closept_line(NULL, lseg, line));
}

/*
 * 线段到矩形的距离
 */
Datum
dist_sb(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);

	PG_RETURN_FLOAT8(box_closept_lseg(NULL, box, lseg));
}

/*
 * 矩形到线段的距离
 */
Datum
dist_bs(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	LSEG	   *lseg = PG_GETARG_LSEG_P(1);

	PG_RETURN_FLOAT8(box_closept_lseg(NULL, box, lseg));
}

static float8
dist_cpoly_internal(CIRCLE *circle, POLYGON *poly)
{
	float8		result;

	/* 计算到圆心的距离，再减去半径 */
	result = float8_mi(dist_ppoly_internal(&circle->center, poly),
					   circle->radius);
	if (result < 0.0)
		result = 0.0;

	return result;
}

/*
 * 圆到多边形的距离
 */
Datum
dist_cpoly(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	POLYGON    *poly = PG_GETARG_POLYGON_P(1);

	PG_RETURN_FLOAT8(dist_cpoly_internal(circle, poly));
}

/*
 * 多边形到圆的距离
 */
Datum
dist_polyc(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_FLOAT8(dist_cpoly_internal(circle, poly));
}

/*
 * 点到多边形的距离
 */
Datum
dist_ppoly(PG_FUNCTION_ARGS)
{
	Point	   *point = PG_GETARG_POINT_P(0);
	POLYGON    *poly = PG_GETARG_POLYGON_P(1);

	PG_RETURN_FLOAT8(dist_ppoly_internal(point, poly));
}

Datum
dist_polyp(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);

	PG_RETURN_FLOAT8(dist_ppoly_internal(point, poly));
}

static float8
dist_ppoly_internal(Point *pt, POLYGON *poly)
{
	float8		result;
	float8		d;
	int			i;
	LSEG		seg;

	if (point_inside(pt, poly->npts, poly->p) != 0)
		return 0.0;

	/* 用首尾两点间的线段初始化距离 */
	seg.p[0].x = poly->p[0].x;
	seg.p[0].y = poly->p[0].y;
	seg.p[1].x = poly->p[poly->npts - 1].x;
	seg.p[1].y = poly->p[poly->npts - 1].y;
	result = lseg_closept_point(NULL, &seg, pt);

	/* 检查其余线段的距离 */
	for (i = 0; i < poly->npts - 1; i++)
	{
		seg.p[0].x = poly->p[i].x;
		seg.p[0].y = poly->p[i].y;
		seg.p[1].x = poly->p[i + 1].x;
		seg.p[1].y = poly->p[i + 1].y;
		d = lseg_closept_point(NULL, &seg, pt);
		if (float8_lt(d, result))
			result = d;
	}

	return result;
}


/*---------------------------------------------------------------------
 *		interpt_
 *				对象的交点。
 *				我们选择忽略直线与矩形之间
 *				  的"交点"，因为通常有两个。
 *-------------------------------------------------------------------*/

/*
 * 返回线段与直线是否相交。若 *result
 * 非 NULL，则将其设置为交点。
 */
static bool
lseg_interpt_line(Point *result, LSEG *lseg, LINE *line)
{
	Point		interpt;
	LINE		tmp;

	/*
	 * 首先，我们把线段提升为直线，因为我们知道
	 * 如何求两直线的交点。若它们没有
	 * 交点，就完成了。
	 */
	line_construct(&tmp, &lseg->p[0], lseg_sl(lseg));
	if (!line_interpt_line(&interpt, &tmp, line))
		return false;

	/*
	 * 然后，我们检查交点是否确实在该线
	 * 上。
	 */
	if (!lseg_contain_point(lseg, &interpt))
		return false;
	if (result != NULL)
	{
		/*
		 * 若存在交点，则显式检查端点是否匹配，
		 * 因为可能存在恼人的 LSB 舍入残留
		 *效应。
		 */
		if (point_eq_point(&lseg->p[0], &interpt))
			*result = lseg->p[0];
		else if (point_eq_point(&lseg->p[1], &interpt))
			*result = lseg->p[1];
		else
			*result = interpt;
	}

	return true;
}

/*---------------------------------------------------------------------
 *		close_
 *				对象间最接近的点。
 *-------------------------------------------------------------------*/

/*
 * 若 *result 非 NULL，则将其设置为
 * 从该点向直线所作垂线的交点。返回这两
 * 个点的距离。
 */
static float8
line_closept_point(Point *result, LINE *line, Point *point)
{
	Point		closept;
	LINE		tmp;

	/*
	 * 我们作一条垂线来求交点。通常我们
	 * 总能找到它，但在存在 NaN 坐标时可能失败，
	 * 甚至可能仅因简单的舍入问题。
	 */
	line_construct(&tmp, point, line_invsl(line));
	if (!line_interpt_line(&closept, &tmp, line))
	{
		if (result != NULL)
			*result = *point;

		return get_float8_nan();
	}

	if (result != NULL)
		*result = closept;

	return point_dt(&closept, point);
}

Datum
close_pl(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	LINE	   *line = PG_GETARG_LINE_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	if (isnan(line_closept_point(result, line, pt)))
		PG_RETURN_NULL();

	PG_RETURN_POINT_P(result);
}


/*
 * 线段上到指定点的最近点。
 *
 * 若 *result 非 NULL，则将其设置为线段
 * 上距该点最近的点。返回这两点的距离。
 */
static float8
lseg_closept_point(Point *result, LSEG *lseg, Point *pt)
{
	Point		closept;
	LINE		tmp;

	/*
	 * 为求最近点，我们从该点作一条垂线
	 * 到线段。
	 */
	line_construct(&tmp, pt, point_invsl(&lseg->p[0], &lseg->p[1]));
	lseg_closept_line(&closept, lseg, &tmp);

	if (result != NULL)
		*result = closept;

	return point_dt(&closept, pt);
}

Datum
close_ps(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	LSEG	   *lseg = PG_GETARG_LSEG_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	if (isnan(lseg_closept_point(result, lseg, pt)))
		PG_RETURN_NULL();

	PG_RETURN_POINT_P(result);
}


/*
 * 线段上到线段的最近点
 */
static float8
lseg_closept_lseg(Point *result, LSEG *on_lseg, LSEG *to_lseg)
{
	Point		point;
	float8		dist,
				d;

	/* 首先，我们处理两线段相交的情形。 */
	if (lseg_interpt_lseg(result, on_lseg, to_lseg))
		return 0.0;

	/*
	 * 然后，我们从第二条线段的端点求最近点，
	 * 并保留最近的一个。
	 */
	dist = lseg_closept_point(result, on_lseg, &to_lseg->p[0]);
	d = lseg_closept_point(&point, on_lseg, &to_lseg->p[1]);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = point;
	}

	/* 最近点仍可能是端点之一，所以我们测试它们。 */
	d = lseg_closept_point(NULL, to_lseg, &on_lseg->p[0]);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = on_lseg->p[0];
	}
	d = lseg_closept_point(NULL, to_lseg, &on_lseg->p[1]);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = on_lseg->p[1];
	}

	return dist;
}

Datum
close_lseg(PG_FUNCTION_ARGS)
{
	LSEG	   *l1 = PG_GETARG_LSEG_P(0);
	LSEG	   *l2 = PG_GETARG_LSEG_P(1);
	Point	   *result;

	if (lseg_sl(l1) == lseg_sl(l2))
		PG_RETURN_NULL();

	result = (Point *) palloc(sizeof(Point));

	if (isnan(lseg_closept_lseg(result, l2, l1)))
		PG_RETURN_NULL();

	PG_RETURN_POINT_P(result);
}


/*
 * 矩形上或内部到指定点的最近点。
 *
 * 若 *result 非 NULL，则将其设置为矩形
 * 上距给定点最近的点，并返回两点距离。
 */
static float8
box_closept_point(Point *result, BOX *box, Point *pt)
{
	float8		dist,
				d;
	Point		point,
				closept;
	LSEG		lseg;

	if (box_contain_point(box, pt))
	{
		if (result != NULL)
			*result = *pt;

		return 0.0;
	}

	/* 逐对检查线段距离 */
	point.x = box->low.x;
	point.y = box->high.y;
	statlseg_construct(&lseg, &box->low, &point);
	dist = lseg_closept_point(result, &lseg, pt);

	statlseg_construct(&lseg, &box->high, &point);
	d = lseg_closept_point(&closept, &lseg, pt);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = closept;
	}

	point.x = box->high.x;
	point.y = box->low.y;
	statlseg_construct(&lseg, &box->low, &point);
	d = lseg_closept_point(&closept, &lseg, pt);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = closept;
	}

	statlseg_construct(&lseg, &box->high, &point);
	d = lseg_closept_point(&closept, &lseg, pt);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = closept;
	}

	return dist;
}

Datum
close_pb(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	if (isnan(box_closept_point(result, box, pt)))
		PG_RETURN_NULL();

	PG_RETURN_POINT_P(result);
}

/*
 * 线段上到直线的最近点。
 *
 * 返回直线与线段上到该直线最近点
 * 之间的距离。若 *result 非 NULL，则将其设为该点。
 *
 * 注意：当两直线平行时，若线段的端点
 * 为 FPeq()，或在存在 NaN 或 Infinite 坐标时，
 * 或仅因简单的舍入问题，可能不存在唯一最近点。
 * 我们很可能在这些情形下把结果设为第二个端点。
 *
 */
static float8
lseg_closept_line(Point *result, LSEG *lseg, LINE *line)
{
	float8		dist1,
				dist2;

	if (lseg_interpt_line(result, lseg, line))
		return 0.0;

	dist1 = line_closept_point(NULL, line, &lseg->p[0]);
	dist2 = line_closept_point(NULL, line, &lseg->p[1]);

	if (dist1 < dist2)
	{
		if (result != NULL)
			*result = lseg->p[0];

		return dist1;
	}
	else
	{
		if (result != NULL)
			*result = lseg->p[1];

		return dist2;
	}
}

Datum
close_ls(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);
	LSEG	   *lseg = PG_GETARG_LSEG_P(1);
	Point	   *result;

	if (lseg_sl(lseg) == line_sl(line))
		PG_RETURN_NULL();

	result = (Point *) palloc(sizeof(Point));

	if (isnan(lseg_closept_line(result, lseg, line)))
		PG_RETURN_NULL();

	PG_RETURN_POINT_P(result);
}


/*
 * 矩形上或内部到线段的最近点。
 *
 * 返回矩形上或内部到线段最近点之间
 * 的距离。若 *result 非 NULL，则设置为该点。
 */
static float8
box_closept_lseg(Point *result, BOX *box, LSEG *lseg)
{
	float8		dist,
				d;
	Point		point,
				closept;
	LSEG		bseg;

	if (box_interpt_lseg(result, box, lseg))
		return 0.0;

	/* 逐对检查线段距离 */
	point.x = box->low.x;
	point.y = box->high.y;
	statlseg_construct(&bseg, &box->low, &point);
	dist = lseg_closept_lseg(result, &bseg, lseg);

	statlseg_construct(&bseg, &box->high, &point);
	d = lseg_closept_lseg(&closept, &bseg, lseg);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = closept;
	}

	point.x = box->high.x;
	point.y = box->low.y;
	statlseg_construct(&bseg, &box->low, &point);
	d = lseg_closept_lseg(&closept, &bseg, lseg);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = closept;
	}

	statlseg_construct(&bseg, &box->high, &point);
	d = lseg_closept_lseg(&closept, &bseg, lseg);
	if (float8_lt(d, dist))
	{
		dist = d;
		if (result != NULL)
			*result = closept;
	}

	return dist;
}

Datum
close_sb(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	if (isnan(box_closept_lseg(result, box, lseg)))
		PG_RETURN_NULL();

	PG_RETURN_POINT_P(result);
}


/*---------------------------------------------------------------------
 *		on_
 *				一个对象是否完全位于另一对象内。
 *-------------------------------------------------------------------*/

/*
 *		该点是否满足方程？
 */
static bool
line_contain_point(LINE *line, Point *point)
{
	return FPzero(float8_pl(float8_pl(float8_mul(line->A, point->x),
									  float8_mul(line->B, point->y)),
							line->C));
}

Datum
on_pl(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	LINE	   *line = PG_GETARG_LINE_P(1);

	PG_RETURN_BOOL(line_contain_point(line, pt));
}


/*
 *		通过检测三角不等式来判断共线性。
 * 该算法即使在 lsb 残留下表现良好 - tgl 1997-07-09
 */
static bool
lseg_contain_point(LSEG *lseg, Point *pt)
{
	return FPeq(point_dt(pt, &lseg->p[0]) +
				point_dt(pt, &lseg->p[1]),
				point_dt(&lseg->p[0], &lseg->p[1]));
}

Datum
on_ps(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	LSEG	   *lseg = PG_GETARG_LSEG_P(1);

	PG_RETURN_BOOL(lseg_contain_point(lseg, pt));
}


/*
 * 检查该点是否在矩形内部或其边界上
 */
static bool
box_contain_point(BOX *box, Point *point)
{
	return box->high.x >= point->x && box->low.x <= point->x &&
		box->high.y >= point->y && box->low.y <= point->y;
}

Datum
on_pb(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(box_contain_point(box, pt));
}

Datum
box_contain_pt(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *pt = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(box_contain_point(box, pt));
}

/* on_ppath -
 *		点是否位于折线内（上）。
 *		若开放，则须（叹气）逐段检查。
 * (使用与点相交线段相同的算法 - tgl 1997-07-09)
 *		若闭合，则用旧 O(n) 射线法求点在多边形内。
 *				射线为水平，从 pt 向右延伸。
 *				每条跨过射线的线段计为一次
 *				相交；注意端点或边可能接触
 *			但未跨过。
 *				(p-in-p 可用 lg(n) 完成，但需预处理)
 */
Datum
on_ppath(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	PATH	   *path = PG_GETARG_PATH_P(1);
	int			i,
				n;
	float8		a,
				b;

	/*-- OPEN --*/
	if (!path->closed)
	{
		n = path->npts - 1;
		a = point_dt(pt, &path->p[0]);
		for (i = 0; i < n; i++)
		{
			b = point_dt(pt, &path->p[i + 1]);
			if (FPeq(float8_pl(a, b), point_dt(&path->p[i], &path->p[i + 1])))
				PG_RETURN_BOOL(true);
			a = b;
		}
		PG_RETURN_BOOL(false);
	}

	/*-- CLOSED --*/
	PG_RETURN_BOOL(point_inside(pt, path->npts, path->p) != 0);
}


/*
 * 检查线段是否在该直线上或足够接近
 *
 * 是的，若两端点都在该直线上或足够接近。
 */
Datum
on_sl(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	LINE	   *line = PG_GETARG_LINE_P(1);

	PG_RETURN_BOOL(line_contain_point(line, &lseg->p[0]) &&
				   line_contain_point(line, &lseg->p[1]));
}


/*
 * 检查线段是否在矩形内部或其边界上
 *
 * 是的，若两端点都在矩形内部或其边界上。
 */
static bool
box_contain_lseg(BOX *box, LSEG *lseg)
{
	return box_contain_point(box, &lseg->p[0]) &&
		box_contain_point(box, &lseg->p[1]);
}

Datum
on_sb(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(box_contain_lseg(box, lseg));
}

/*---------------------------------------------------------------------
 *		inter_
 *				对象是否与另一对象相交。
 *-------------------------------------------------------------------*/

Datum
inter_sl(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	LINE	   *line = PG_GETARG_LINE_P(1);

	PG_RETURN_BOOL(lseg_interpt_line(NULL, lseg, line));
}


/*
 * 线段与矩形是否相交？
 *
 * 线段完全位于矩形内也算相交。
 * 若只想知道跨过矩形边界的线段，
 *	请先把矩形转为路径。
 *
 * 当两者重叠且 *result 非 NULL 时，本函数还会将
 * *result 设置为线段上到矩形中心的最近点。
 * 这有些随意，但也许已是我们能做到的最佳结果，
 * 因为它们通常交于两点。
 *
 * 通过先检查矩形是否相交来为不相交优化。
 * - thomas 1998-01-30
 */
static bool
box_interpt_lseg(Point *result, BOX *box, LSEG *lseg)
{
	BOX			lbox;
	LSEG		bseg;
	Point		point;

	lbox.low.x = float8_min(lseg->p[0].x, lseg->p[1].x);
	lbox.low.y = float8_min(lseg->p[0].y, lseg->p[1].y);
	lbox.high.x = float8_max(lseg->p[0].x, lseg->p[1].x);
	lbox.high.y = float8_max(lseg->p[0].y, lseg->p[1].y);

	/* 没有接近重叠？那么不会相交 */
	if (!box_ov(&lbox, box))
		return false;

	if (result != NULL)
	{
		box_cn(&point, box);
		lseg_closept_point(result, lseg, &point);
	}

	/* 线段端点在矩形内？那么显然相交 */
	if (box_contain_point(box, &lseg->p[0]) ||
		box_contain_point(box, &lseg->p[1]))
		return true;

	/* 逐对检查线段相交 */
	point.x = box->low.x;
	point.y = box->high.y;
	statlseg_construct(&bseg, &box->low, &point);
	if (lseg_interpt_lseg(NULL, &bseg, lseg))
		return true;

	statlseg_construct(&bseg, &box->high, &point);
	if (lseg_interpt_lseg(NULL, &bseg, lseg))
		return true;

	point.x = box->high.x;
	point.y = box->low.y;
	statlseg_construct(&bseg, &box->low, &point);
	if (lseg_interpt_lseg(NULL, &bseg, lseg))
		return true;

	statlseg_construct(&bseg, &box->high, &point);
	if (lseg_interpt_lseg(NULL, &bseg, lseg))
		return true;

	/* 落到此处即无任何两线段相交 */
	return false;
}

Datum
inter_sb(PG_FUNCTION_ARGS)
{
	LSEG	   *lseg = PG_GETARG_LSEG_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);

	PG_RETURN_BOOL(box_interpt_lseg(NULL, box, lseg));
}


/* inter_lb()
 * 直线与矩形相交？
 */
Datum
inter_lb(PG_FUNCTION_ARGS)
{
	LINE	   *line = PG_GETARG_LINE_P(0);
	BOX		   *box = PG_GETARG_BOX_P(1);
	LSEG		bseg;
	Point		p1,
				p2;

	/* 逐对检查线段相交 */
	p1.x = box->low.x;
	p1.y = box->low.y;
	p2.x = box->low.x;
	p2.y = box->high.y;
	statlseg_construct(&bseg, &p1, &p2);
	if (lseg_interpt_line(NULL, &bseg, line))
		PG_RETURN_BOOL(true);
	p1.x = box->high.x;
	p1.y = box->high.y;
	statlseg_construct(&bseg, &p1, &p2);
	if (lseg_interpt_line(NULL, &bseg, line))
		PG_RETURN_BOOL(true);
	p2.x = box->high.x;
	p2.y = box->low.y;
	statlseg_construct(&bseg, &p1, &p2);
	if (lseg_interpt_line(NULL, &bseg, line))
		PG_RETURN_BOOL(true);
	p1.x = box->low.x;
	p1.y = box->low.y;
	statlseg_construct(&bseg, &p1, &p2);
	if (lseg_interpt_line(NULL, &bseg, line))
		PG_RETURN_BOOL(true);

	/* 落到此处即无交点 */
	PG_RETURN_BOOL(false);
}

/*------------------------------------------------------------------
 * 以下例程为多边形定义数据类型与操作符类，
 * 其中一部分（多边形的外接框）构建于
 * BOX 数据类型之上。
 *
 * make_bound_box - 为输入多边形创建外接框
 *------------------------------------------------------------------*/

/*---------------------------------------------------------------------
 * 为给定多边形制作最小外接框。
 *---------------------------------------------------------------------*/
static void
make_bound_box(POLYGON *poly)
{
	int			i;
	float8		x1,
				y1,
				x2,
				y2;

	Assert(poly->npts > 0);

	x1 = x2 = poly->p[0].x;
	y2 = y1 = poly->p[0].y;
	for (i = 1; i < poly->npts; i++)
	{
		if (float8_lt(poly->p[i].x, x1))
			x1 = poly->p[i].x;
		if (float8_gt(poly->p[i].x, x2))
			x2 = poly->p[i].x;
		if (float8_lt(poly->p[i].y, y1))
			y1 = poly->p[i].y;
		if (float8_gt(poly->p[i].y, y2))
			y2 = poly->p[i].y;
	}

	poly->boundbox.low.x = x1;
	poly->boundbox.high.x = x2;
	poly->boundbox.low.y = y1;
	poly->boundbox.high.y = y2;
}

/*------------------------------------------------------------------
 * poly_in - 从字符串描述读入多边形
 *
 *		外部格式：
 *				"((x0,y0),...,(xn,yn))"
 *				"x0,y0,...,xn,yn"
 *			也支持旧式 "(x1,...,xn,y1,...yn)"
 *------------------------------------------------------------------*/
Datum
poly_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	POLYGON    *poly;
	int			npts;
	int			size;
	int			base_size;
	bool		isopen;

	if ((npts = pair_count(str, ',')) <= 0)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"polygon", str)));

	base_size = sizeof(poly->p[0]) * npts;
	size = offsetof(POLYGON, p) + base_size;

	/* 检查整数溢出 */
	if (base_size / npts != sizeof(poly->p[0]) || size <= base_size)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many points requested")));

	poly = (POLYGON *) palloc0(size);	/* 空洞置零 */

	SET_VARSIZE(poly, size);
	poly->npts = npts;

	if (!path_decode(str, false, npts, &(poly->p[0]), &isopen, NULL, "polygon",
					 str, escontext))
		PG_RETURN_NULL();

	make_bound_box(poly);

	PG_RETURN_POLYGON_P(poly);
}

/*---------------------------------------------------------------
 * poly_out - 将内部 POLYGON 表示转换为
 *		  字符串格式 "((f8,f8),...,(f8,f8))"
 *---------------------------------------------------------------*/
Datum
poly_out(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);

	PG_RETURN_CSTRING(path_encode(PATH_CLOSED, poly->npts, poly->p));
}

/*
 *		poly_recv			- 将外部二进制格式转为多边形
 *
 * 外部表示为 int32 点数以及各点。
 * 我们在读取时重算外接框，而非轻信其有效。
 * (检查它同样耗时，所以不妨
 * 从外部表示中省略它。)
 */
Datum
poly_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	POLYGON    *poly;
	int32		npts;
	int32		i;
	int			size;

	npts = pq_getmsgint(buf, sizeof(int32));
	if (npts <= 0 || npts >= (int32) ((INT_MAX - offsetof(POLYGON, p)) / sizeof(Point)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid number of points in external \"polygon\" value")));

	size = offsetof(POLYGON, p) + sizeof(poly->p[0]) * npts;
	poly = (POLYGON *) palloc0(size);	/* 空洞置零 */

	SET_VARSIZE(poly, size);
	poly->npts = npts;

	for (i = 0; i < npts; i++)
	{
		poly->p[i].x = pq_getmsgfloat8(buf);
		poly->p[i].y = pq_getmsgfloat8(buf);
	}

	make_bound_box(poly);

	PG_RETURN_POLYGON_P(poly);
}

/*
 *		poly_send			-将多边形转为二进制格式
 */
Datum
poly_send(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	StringInfoData buf;
	int32		i;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, poly->npts);
	for (i = 0; i < poly->npts; i++)
	{
		pq_sendfloat8(&buf, poly->p[i].x);
		pq_sendfloat8(&buf, poly->p[i].y);
	}
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*-------------------------------------------------------
 * 多边形A是否严格在多边形B左侧？即
 * A 最右点是否在 B 最左点左侧？
 *
 *-------------------------------------------------------*/
Datum
poly_left(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.high.x < polyb->boundbox.low.x;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否重叠或在多边形B左侧？即
 * A 的最右点是否在 B 的最右点或其左侧？
 *
 *-------------------------------------------------------*/
Datum
poly_overleft(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.high.x <= polyb->boundbox.high.x;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否严格在多边形B右侧？即
 * A 的最左点是否在 B 的最右点右侧？
 *
 *-------------------------------------------------------*/
Datum
poly_right(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.low.x > polyb->boundbox.high.x;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否重叠或在多边形B右侧？即
 * A 的最左点是否在 B 的最左点或其右侧？
 *
 *-------------------------------------------------------*/
Datum
poly_overright(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.low.x >= polyb->boundbox.low.x;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否严格在B下方？即
 * A 的最高点是否在 B 的最低点下方？
 *
 *-------------------------------------------------------*/
Datum
poly_below(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.high.y < polyb->boundbox.low.y;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否重叠或在多边形B下方？即
 * A 的最高点是否在 B 的最高点或其下方？
 *
 *-------------------------------------------------------*/
Datum
poly_overbelow(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.high.y <= polyb->boundbox.high.y;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否严格在B上方？即
 * A 的最低点是否在 B 的最高点上方？
 *
 *-------------------------------------------------------*/
Datum
poly_above(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.low.y > polyb->boundbox.high.y;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-------------------------------------------------------
 * 多边形A是否重叠或在多边形B上方？即
 * A 的最低点是否在 B 的最低点或其上方？
 *
 *-------------------------------------------------------*/
Datum
poly_overabove(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = polya->boundbox.low.y >= polyb->boundbox.low.y;

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}


/*-------------------------------------------------------
 * 多边形A与多边形B是否相同？即全部
 * 点都相同？
 * 正向与反向都检查所有点是否匹配，
 *	因为多边形无方向且为闭合形状。
 *
 *-------------------------------------------------------*/
Datum
poly_same(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	if (polya->npts != polyb->npts)
		result = false;
	else
		result = plist_same(polya->npts, polya->p, polyb->p);

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*-----------------------------------------------------------------
 * 判断多边形A是否与多边形B重叠
 *-----------------------------------------------------------------*/
static bool
poly_overlap_internal(POLYGON *polya, POLYGON *polyb)
{
	bool		result;

	Assert(polya->npts > 0 && polyb->npts > 0);

	/* 用外接框快速检查 */
	result = box_ov(&polya->boundbox, &polyb->boundbox);

	/*
	 * 暴力算法 - 尝试找出相交的边，若找到，
	 * 则两多边形重叠，否则检查一个多边形
	 * 是否位于另一个之内。
	 */
	if (result)
	{
		int			ia,
					ib;
		LSEG		sa,
					sb;

		/* 用末点初始化 polya 边第一点 */
		sa.p[0] = polya->p[polya->npts - 1];
		result = false;

		for (ia = 0; ia < polya->npts && !result; ia++)
		{
			/* polya 边的第二点为当前点 */
			sa.p[1] = polya->p[ia];

			/* 用末点初始化 polyb 边第一点 */
			sb.p[0] = polyb->p[polyb->npts - 1];

			for (ib = 0; ib < polyb->npts && !result; ib++)
			{
				sb.p[1] = polyb->p[ib];
				result = lseg_interpt_lseg(NULL, &sa, &sb);
				sb.p[0] = sb.p[1];
			}

			/*
			 * 将当前端点移到下一条边的第一点
			 */
			sa.p[0] = sa.p[1];
		}

		if (!result)
		{
			result = (point_inside(polya->p, polyb->npts, polyb->p) ||
					  point_inside(polyb->p, polya->npts, polya->p));
		}
	}

	return result;
}

Datum
poly_overlap(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = poly_overlap_internal(polya, polyb);

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}

/*
 * 测试特殊线段相对多边形的进出情况。
 * 特殊意味着：
 *	- 点 a 应在线段 s 上
 *	- 线段 (a,b) 应不被 s 包含
 * true 条件：
 *	- 线段 (a,b) 与 s 共线且 (a,b) 在多边形内
 *	-线段(a,b)不与s共线。注意：这并不代表
 *	  线段在多边形内！
 */

static bool
touched_lseg_inside_poly(Point *a, Point *b, LSEG *s, POLYGON *poly, int start)
{
	/* a 在 s 上，b 不在 */
	LSEG		t;

	t.p[0] = *a;
	t.p[1] = *b;

	if (point_eq_point(a, s->p))
	{
		if (lseg_contain_point(&t, s->p + 1))
			return lseg_inside_poly(b, s->p + 1, poly, start);
	}
	else if (point_eq_point(a, s->p + 1))
	{
		if (lseg_contain_point(&t, s->p))
			return lseg_inside_poly(b, s->p, poly, start);
	}
	else if (lseg_contain_point(&t, s->p))
	{
		return lseg_inside_poly(b, s->p, poly, start);
	}
	else if (lseg_contain_point(&t, s->p + 1))
	{
		return lseg_inside_poly(b, s->p + 1, poly, start);
	}

	return true;				/* 可能并非如此，但稍后会检查 */
}

/*
 * 若线段 (a,b) 在多边形内则返回 true，
 * start 用于优化 - 函数
 *	从 start 检查多边形各边。
 */
static bool
lseg_inside_poly(Point *a, Point *b, POLYGON *poly, int start)
{
	LSEG		s,
				t;
	int			i;
	bool		res = true,
				intersection = false;

	/* 由于本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	t.p[0] = *a;
	t.p[1] = *b;
	s.p[0] = poly->p[(start == 0) ? (poly->npts - 1) : (start - 1)];

	for (i = start; i < poly->npts && res; i++)
	{
		Point		interpt;

		CHECK_FOR_INTERRUPTS();

		s.p[1] = poly->p[i];

		if (lseg_contain_point(&s, t.p))
		{
			if (lseg_contain_point(&s, t.p + 1))
				return true;	/* t 被 s 包含 */

			/* Y-cross */
			res = touched_lseg_inside_poly(t.p, t.p + 1, &s, poly, i + 1);
		}
		else if (lseg_contain_point(&s, t.p + 1))
		{
			/* Y-cross */
			res = touched_lseg_inside_poly(t.p + 1, t.p, &s, poly, i + 1);
		}
		else if (lseg_interpt_lseg(&interpt, &t, &s))
		{
			/*
			 * 线段 X 相交，继续检查每个子线段
			 */

			intersection = true;
			res = lseg_inside_poly(t.p, &interpt, poly, i + 1);
			if (res)
				res = lseg_inside_poly(t.p + 1, &interpt, poly, i + 1);
		}

		s.p[0] = s.p[1];
	}

	if (res && !intersection)
	{
		Point		p;

		/*
		 * 若未找到 X 相交，则检查被测线段的
		 * 中点。否则我们已检查过全部子线段
		 */
		p.x = float8_div(float8_pl(t.p[0].x, t.p[1].x), 2.0);
		p.y = float8_div(float8_pl(t.p[0].y, t.p[1].y), 2.0);

		res = point_inside(&p, poly->npts, poly->p);
	}

	return res;
}

/*
 * 检查第一个多边形是否包含第二个
 */
static bool
poly_contain_poly(POLYGON *contains_poly, POLYGON *contained_poly)
{
	int			i;
	LSEG		s;

	Assert(contains_poly->npts > 0 && contained_poly->npts > 0);

	/*
	 * 快速检查被包含者的外接框是否包含于包含者
	 * 外接框。
	 */
	if (!box_contain_box(&contains_poly->boundbox, &contained_poly->boundbox))
		return false;

	s.p[0] = contained_poly->p[contained_poly->npts - 1];

	for (i = 0; i < contained_poly->npts; i++)
	{
		s.p[1] = contained_poly->p[i];
		if (!lseg_inside_poly(s.p, s.p + 1, contains_poly, 0))
			return false;
		s.p[0] = s.p[1];
	}

	return true;
}

Datum
poly_contain(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	result = poly_contain_poly(polya, polyb);

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}


/*-----------------------------------------------------------------
 * 判断多边形A是否被多边形B包含
 *-----------------------------------------------------------------*/
Datum
poly_contained(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	bool		result;

	/* 只是交换参数并转交给 poly_contain */
	result = poly_contain_poly(polyb, polya);

	/*
	 * 避免 toast 输入泄漏内存……rtree 索引需要
	 */
	PG_FREE_IF_COPY(polya, 0);
	PG_FREE_IF_COPY(polyb, 1);

	PG_RETURN_BOOL(result);
}


Datum
poly_contain_pt(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	Point	   *p = PG_GETARG_POINT_P(1);

	PG_RETURN_BOOL(point_inside(p, poly->npts, poly->p) != 0);
}

Datum
pt_contained_poly(PG_FUNCTION_ARGS)
{
	Point	   *p = PG_GETARG_POINT_P(0);
	POLYGON    *poly = PG_GETARG_POLYGON_P(1);

	PG_RETURN_BOOL(point_inside(p, poly->npts, poly->p) != 0);
}


Datum
poly_distance(PG_FUNCTION_ARGS)
{
	POLYGON    *polya = PG_GETARG_POLYGON_P(0);
	POLYGON    *polyb = PG_GETARG_POLYGON_P(1);
	float8		min = 0.0;		/* 初始化以让编译器安静 */
	bool		have_min = false;
	float8		tmp;
	int			i,
				j;
	LSEG		seg1,
				seg2;

	/*
	 * 若多边形重叠则距离为零。我们必须检查这一点，
	 * 因为若一个多边形完全位于另一个内，路径
	 * 距离会错。
	 */
	if (poly_overlap_internal(polya, polyb))
		PG_RETURN_FLOAT8(0.0);

	/*
	 * 当它们不重叠时，距离计算与闭合路径的
 * 计算完全相同（即无需关心多边形包含其
 * 内部区域这一事实。）见 path_distance()。
	 */
	for (i = 0; i < polya->npts; i++)
	{
		int			iprev;

		if (i > 0)
			iprev = i - 1;
		else
			iprev = polya->npts - 1;

		for (j = 0; j < polyb->npts; j++)
		{
			int			jprev;

			if (j > 0)
				jprev = j - 1;
			else
				jprev = polyb->npts - 1;

			statlseg_construct(&seg1, &polya->p[iprev], &polya->p[i]);
			statlseg_construct(&seg2, &polyb->p[jprev], &polyb->p[j]);

			tmp = lseg_closept_lseg(NULL, &seg1, &seg2);
			if (!have_min || float8_lt(tmp, min))
			{
				min = tmp;
				have_min = true;
			}
		}
	}

	if (!have_min)
		PG_RETURN_NULL();

	PG_RETURN_FLOAT8(min);
}


/***********************************************************************
 **
 **		二维点例程。
 **
 ***********************************************************************/

Datum
construct_point(PG_FUNCTION_ARGS)
{
	float8		x = PG_GETARG_FLOAT8(0);
	float8		y = PG_GETARG_FLOAT8(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	point_construct(result, x, y);

	PG_RETURN_POINT_P(result);
}


static inline void
point_add_point(Point *result, Point *pt1, Point *pt2)
{
	point_construct(result,
					float8_pl(pt1->x, pt2->x),
					float8_pl(pt1->y, pt2->y));
}

Datum
point_add(PG_FUNCTION_ARGS)
{
	Point	   *p1 = PG_GETARG_POINT_P(0);
	Point	   *p2 = PG_GETARG_POINT_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	point_add_point(result, p1, p2);

	PG_RETURN_POINT_P(result);
}


static inline void
point_sub_point(Point *result, Point *pt1, Point *pt2)
{
	point_construct(result,
					float8_mi(pt1->x, pt2->x),
					float8_mi(pt1->y, pt2->y));
}

Datum
point_sub(PG_FUNCTION_ARGS)
{
	Point	   *p1 = PG_GETARG_POINT_P(0);
	Point	   *p2 = PG_GETARG_POINT_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	point_sub_point(result, p1, p2);

	PG_RETURN_POINT_P(result);
}


static inline void
point_mul_point(Point *result, Point *pt1, Point *pt2)
{
	point_construct(result,
					float8_mi(float8_mul(pt1->x, pt2->x),
							  float8_mul(pt1->y, pt2->y)),
					float8_pl(float8_mul(pt1->x, pt2->y),
							  float8_mul(pt1->y, pt2->x)));
}

Datum
point_mul(PG_FUNCTION_ARGS)
{
	Point	   *p1 = PG_GETARG_POINT_P(0);
	Point	   *p2 = PG_GETARG_POINT_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	point_mul_point(result, p1, p2);

	PG_RETURN_POINT_P(result);
}


static inline void
point_div_point(Point *result, Point *pt1, Point *pt2)
{
	float8		div;

	div = float8_pl(float8_mul(pt2->x, pt2->x), float8_mul(pt2->y, pt2->y));

	point_construct(result,
					float8_div(float8_pl(float8_mul(pt1->x, pt2->x),
										 float8_mul(pt1->y, pt2->y)), div),
					float8_div(float8_mi(float8_mul(pt1->y, pt2->x),
										 float8_mul(pt1->x, pt2->y)), div));
}

Datum
point_div(PG_FUNCTION_ARGS)
{
	Point	   *p1 = PG_GETARG_POINT_P(0);
	Point	   *p2 = PG_GETARG_POINT_P(1);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));

	point_div_point(result, p1, p2);

	PG_RETURN_POINT_P(result);
}


/***********************************************************************
 **
 **		二维矩形例程。
 **
 ***********************************************************************/

Datum
points_box(PG_FUNCTION_ARGS)
{
	Point	   *p1 = PG_GETARG_POINT_P(0);
	Point	   *p2 = PG_GETARG_POINT_P(1);
	BOX		   *result;

	result = (BOX *) palloc(sizeof(BOX));

	box_construct(result, p1, p2);

	PG_RETURN_BOX_P(result);
}

Datum
box_add(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *p = PG_GETARG_POINT_P(1);
	BOX		   *result;

	result = (BOX *) palloc(sizeof(BOX));

	point_add_point(&result->high, &box->high, p);
	point_add_point(&result->low, &box->low, p);

	PG_RETURN_BOX_P(result);
}

Datum
box_sub(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *p = PG_GETARG_POINT_P(1);
	BOX		   *result;

	result = (BOX *) palloc(sizeof(BOX));

	point_sub_point(&result->high, &box->high, p);
	point_sub_point(&result->low, &box->low, p);

	PG_RETURN_BOX_P(result);
}

Datum
box_mul(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *p = PG_GETARG_POINT_P(1);
	BOX		   *result;
	Point		high,
				low;

	result = (BOX *) palloc(sizeof(BOX));

	point_mul_point(&high, &box->high, p);
	point_mul_point(&low, &box->low, p);

	box_construct(result, &high, &low);

	PG_RETURN_BOX_P(result);
}

Datum
box_div(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	Point	   *p = PG_GETARG_POINT_P(1);
	BOX		   *result;
	Point		high,
				low;

	result = (BOX *) palloc(sizeof(BOX));

	point_div_point(&high, &box->high, p);
	point_div_point(&low, &box->low, p);

	box_construct(result, &high, &low);

	PG_RETURN_BOX_P(result);
}

/*
 * 将点转换为空矩形
 */
Datum
point_box(PG_FUNCTION_ARGS)
{
	Point	   *pt = PG_GETARG_POINT_P(0);
	BOX		   *box;

	box = (BOX *) palloc(sizeof(BOX));

	box->high.x = pt->x;
	box->low.x = pt->x;
	box->high.y = pt->y;
	box->low.y = pt->y;

	PG_RETURN_BOX_P(box);
}

/*
 * 包含两个给定矩形的最小外接框
 */
Datum
boxes_bound_box(PG_FUNCTION_ARGS)
{
	BOX		   *box1 = PG_GETARG_BOX_P(0),
			   *box2 = PG_GETARG_BOX_P(1),
			   *container;

	container = (BOX *) palloc(sizeof(BOX));

	container->high.x = float8_max(box1->high.x, box2->high.x);
	container->low.x = float8_min(box1->low.x, box2->low.x);
	container->high.y = float8_max(box1->high.y, box2->high.y);
	container->low.y = float8_min(box1->low.y, box2->low.y);

	PG_RETURN_BOX_P(container);
}


/***********************************************************************
 **
 **		二维路径例程。
 **
 ***********************************************************************/

/* path_add()
 * 连接两条路径（仅当两者均开放）。
 */
Datum
path_add(PG_FUNCTION_ARGS)
{
	PATH	   *p1 = PG_GETARG_PATH_P(0);
	PATH	   *p2 = PG_GETARG_PATH_P(1);
	PATH	   *result;
	int			size,
				base_size;
	int			i;

	if (p1->closed || p2->closed)
		PG_RETURN_NULL();

	base_size = sizeof(p1->p[0]) * (p1->npts + p2->npts);
	size = offsetof(PATH, p) + base_size;

	/* 检查整数溢出 */
	if (base_size / sizeof(p1->p[0]) != (p1->npts + p2->npts) ||
		size <= base_size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many points requested")));

	result = (PATH *) palloc(size);

	SET_VARSIZE(result, size);
	result->npts = (p1->npts + p2->npts);
	result->closed = p1->closed;
	/* 防止未用填充字节的不稳定性 */
	result->dummy = 0;

	for (i = 0; i < p1->npts; i++)
	{
		result->p[i].x = p1->p[i].x;
		result->p[i].y = p1->p[i].y;
	}
	for (i = 0; i < p2->npts; i++)
	{
		result->p[i + p1->npts].x = p2->p[i].x;
		result->p[i + p1->npts].y = p2->p[i].y;
	}

	PG_RETURN_PATH_P(result);
}

/* path_add_pt()
 * 平移算子。
 */
Datum
path_add_pt(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P_COPY(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	int			i;

	for (i = 0; i < path->npts; i++)
		point_add_point(&path->p[i], &path->p[i], point);

	PG_RETURN_PATH_P(path);
}

Datum
path_sub_pt(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P_COPY(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	int			i;

	for (i = 0; i < path->npts; i++)
		point_sub_point(&path->p[i], &path->p[i], point);

	PG_RETURN_PATH_P(path);
}

/* path_mul_pt()
 * 旋转与缩放算子。
 */
Datum
path_mul_pt(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P_COPY(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	int			i;

	for (i = 0; i < path->npts; i++)
		point_mul_point(&path->p[i], &path->p[i], point);

	PG_RETURN_PATH_P(path);
}

Datum
path_div_pt(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P_COPY(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	int			i;

	for (i = 0; i < path->npts; i++)
		point_div_point(&path->p[i], &path->p[i], point);

	PG_RETURN_PATH_P(path);
}


Datum
path_poly(PG_FUNCTION_ARGS)
{
	PATH	   *path = PG_GETARG_PATH_P(0);
	POLYGON    *poly;
	int			size;
	int			i;

	/* 这不太一致 --- 其他类似情形返回 NULL ... */
	if (!path->closed)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("open path cannot be converted to polygon")));

	/*
	 * 绝不溢出：旧大小能放入 MaxAllocSize，而新大小只
	 * 比它大一个小常量。
	 */
	size = offsetof(POLYGON, p) + sizeof(poly->p[0]) * path->npts;
	poly = (POLYGON *) palloc(size);

	SET_VARSIZE(poly, size);
	poly->npts = path->npts;

	for (i = 0; i < path->npts; i++)
	{
		poly->p[i].x = path->p[i].x;
		poly->p[i].y = path->p[i].y;
	}

	make_bound_box(poly);

	PG_RETURN_POLYGON_P(poly);
}


/***********************************************************************
 **
 **		二维多边形例程。
 **
 ***********************************************************************/

Datum
poly_npoints(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);

	PG_RETURN_INT32(poly->npts);
}


Datum
poly_center(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	Point	   *result;
	CIRCLE		circle;

	result = (Point *) palloc(sizeof(Point));

	poly_to_circle(&circle, poly);
	*result = circle.center;

	PG_RETURN_POINT_P(result);
}


Datum
poly_box(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	BOX		   *box;

	box = (BOX *) palloc(sizeof(BOX));
	*box = poly->boundbox;

	PG_RETURN_BOX_P(box);
}


/* box_poly()
 * 把矩形转为多边形。
 */
Datum
box_poly(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	POLYGON    *poly;
	int			size;

	/* 把矩形的四个角映射为多边形 */
	size = offsetof(POLYGON, p) + sizeof(poly->p[0]) * 4;
	poly = (POLYGON *) palloc(size);

	SET_VARSIZE(poly, size);
	poly->npts = 4;

	poly->p[0].x = box->low.x;
	poly->p[0].y = box->low.y;
	poly->p[1].x = box->low.x;
	poly->p[1].y = box->high.y;
	poly->p[2].x = box->high.x;
	poly->p[2].y = box->high.y;
	poly->p[3].x = box->high.x;
	poly->p[3].y = box->low.y;

	box_construct(&poly->boundbox, &box->high, &box->low);

	PG_RETURN_POLYGON_P(poly);
}


Datum
poly_path(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	PATH	   *path;
	int			size;
	int			i;

	/*
	 * 绝不溢出：旧大小能放入 MaxAllocSize，而新大小只
	 * 比它小一个小常量。
	 */
	size = offsetof(PATH, p) + sizeof(path->p[0]) * poly->npts;
	path = (PATH *) palloc(size);

	SET_VARSIZE(path, size);
	path->npts = poly->npts;
	path->closed = true;
	/* 防止未用填充字节的不稳定性 */
	path->dummy = 0;

	for (i = 0; i < poly->npts; i++)
	{
		path->p[i].x = poly->p[i].x;
		path->p[i].y = poly->p[i].y;
	}

	PG_RETURN_PATH_P(path);
}


/***********************************************************************
 **
 **		圆例程。
 **
 ***********************************************************************/

/*----------------------------------------------------------
 * 格式化与转换例程。
 *---------------------------------------------------------*/

/*		circle_in		-		将字符串转为内部形式。
 *
 *		外部格式：（圆的圆心与半径）
 *				"<(f8,f8),f8>"
 *			也支持快速输入样式 "f8,f8,f8"
 */
Datum
circle_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	CIRCLE	   *circle = (CIRCLE *) palloc(sizeof(CIRCLE));
	char	   *s,
			   *cp;
	int			depth = 0;

	s = str;
	while (isspace((unsigned char) *s))
		s++;
	if (*s == LDELIM_C)
		depth++, s++;
	else if (*s == LDELIM)
	{
		/* 若有两个左括号，则消费第一个 */
		cp = (s + 1);
		while (isspace((unsigned char) *cp))
			cp++;
		if (*cp == LDELIM)
			depth++, s = cp;
	}

	/* 若有的话 pair_decode 会消费数对周围括号 */
	if (!pair_decode(s, &circle->center.x, &circle->center.y, &s, "circle", str,
					 escontext))
		PG_RETURN_NULL();

	if (*s == DELIM)
		s++;

	if (!single_decode(s, &circle->radius, &s, "circle", str, escontext))
		PG_RETURN_NULL();

	/* 必须接受 NaN。 */
	if (circle->radius < 0.0)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"circle", str)));

	while (depth > 0)
	{
		if ((*s == RDELIM) || ((*s == RDELIM_C) && (depth == 1)))
		{
			depth--;
			s++;
			while (isspace((unsigned char) *s))
				s++;
		}
		else
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid input syntax for type %s: \"%s\"",
							"circle", str)));
	}

	if (*s != '\0')
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"circle", str)));

	PG_RETURN_CIRCLE_P(circle);
}

/*		circle_out		-		将圆转为外部形式。
 */
Datum
circle_out(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	StringInfoData str;

	initStringInfo(&str);

	appendStringInfoChar(&str, LDELIM_C);
	appendStringInfoChar(&str, LDELIM);
	pair_encode(circle->center.x, circle->center.y, &str);
	appendStringInfoChar(&str, RDELIM);
	appendStringInfoChar(&str, DELIM);
	single_encode(circle->radius, &str);
	appendStringInfoChar(&str, RDELIM_C);

	PG_RETURN_CSTRING(str.data);
}

/*
 *		circle_recv			- 将外部二进制格式转为圆
 */
Datum
circle_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	CIRCLE	   *circle;

	circle = (CIRCLE *) palloc(sizeof(CIRCLE));

	circle->center.x = pq_getmsgfloat8(buf);
	circle->center.y = pq_getmsgfloat8(buf);
	circle->radius = pq_getmsgfloat8(buf);

	/* 必须接受 NaN。 */
	if (circle->radius < 0.0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid radius in external \"circle\" value")));

	PG_RETURN_CIRCLE_P(circle);
}

/*
 *		circle_send			- 将圆转为二进制格式
 */
Datum
circle_send(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendfloat8(&buf, circle->center.x);
	pq_sendfloat8(&buf, circle->center.y);
	pq_sendfloat8(&buf, circle->radius);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*----------------------------------------------------------
 * 圆的关系算子。
 *		<, >, <=, >= 和 == 基于圆面积。
 *---------------------------------------------------------*/

/*		圆是否相同？
 *
 * 我们将 NaN 值视为彼此相等，以便能找到这些圆。
 *
 */
Datum
circle_same(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(((isnan(circle1->radius) && isnan(circle2->radius)) ||
					FPeq(circle1->radius, circle2->radius)) &&
				   point_eq_point(&circle1->center, &circle2->center));
}

/*		circle_overlap	-		circle1 与 circle2 重叠？
 */
Datum
circle_overlap(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPle(point_dt(&circle1->center, &circle2->center),
						float8_pl(circle1->radius, circle2->radius)));
}

/*		circle_overleft -		circle1 的右边缘是否在 circle2
 *						右边缘或其左侧？
 */
Datum
circle_overleft(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPle(float8_pl(circle1->center.x, circle1->radius),
						float8_pl(circle2->center.x, circle2->radius)));
}

/*		circle_left		-		circle1 严格在 circle2 左侧？
 */
Datum
circle_left(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPlt(float8_pl(circle1->center.x, circle1->radius),
						float8_mi(circle2->center.x, circle2->radius)));
}

/*		circle_right	-		circle1 严格在 circle2 右侧？
 */
Datum
circle_right(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPgt(float8_mi(circle1->center.x, circle1->radius),
						float8_pl(circle2->center.x, circle2->radius)));
}

/*		circle_overright	-	circle1 的左边缘是否在 circle2
 *						左边缘或其右侧？
 */
Datum
circle_overright(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPge(float8_mi(circle1->center.x, circle1->radius),
						float8_mi(circle2->center.x, circle2->radius)));
}

/*		circle_contained		-		circle1是否被circle2包含？
 */
Datum
circle_contained(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPle(point_dt(&circle1->center, &circle2->center),
						float8_mi(circle2->radius, circle1->radius)));
}

/*		circle_contain	-		circle1是否包含circle2？
 */
Datum
circle_contain(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPle(point_dt(&circle1->center, &circle2->center),
						float8_mi(circle1->radius, circle2->radius)));
}


/*		circle_below		-		circle1 严格在 circle2下方？
 */
Datum
circle_below(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPlt(float8_pl(circle1->center.y, circle1->radius),
						float8_mi(circle2->center.y, circle2->radius)));
}

/*		circle_above	-		circle1 严格在 circle2上方？
 */
Datum
circle_above(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPgt(float8_mi(circle1->center.y, circle1->radius),
						float8_pl(circle2->center.y, circle2->radius)));
}

/*		circle_overbelow -		circle1 的上边缘是否在 circle2
 *						上边缘或其下方？
 */
Datum
circle_overbelow(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPle(float8_pl(circle1->center.y, circle1->radius),
						float8_pl(circle2->center.y, circle2->radius)));
}

/*		circle_overabove	-	circle1 的下边缘是否在 circle2
 *						下边缘或其上方？
 */
Datum
circle_overabove(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPge(float8_mi(circle1->center.y, circle1->radius),
						float8_mi(circle2->center.y, circle2->radius)));
}


/*		circle_relop	-		area(circle1) relop area(circle2)在精度
 *						约束内是否成立？
 */
Datum
circle_eq(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPeq(circle_ar(circle1), circle_ar(circle2)));
}

Datum
circle_ne(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPne(circle_ar(circle1), circle_ar(circle2)));
}

Datum
circle_lt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPlt(circle_ar(circle1), circle_ar(circle2)));
}

Datum
circle_gt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPgt(circle_ar(circle1), circle_ar(circle2)));
}

Datum
circle_le(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPle(circle_ar(circle1), circle_ar(circle2)));
}

Datum
circle_ge(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);

	PG_RETURN_BOOL(FPge(circle_ar(circle1), circle_ar(circle2)));
}


/*----------------------------------------------------------
 * 圆上的"算术"算子。
 *---------------------------------------------------------*/

/* circle_add_pt()
 * 平移算子。
 */
Datum
circle_add_pt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	CIRCLE	   *result;

	result = (CIRCLE *) palloc(sizeof(CIRCLE));

	point_add_point(&result->center, &circle->center, point);
	result->radius = circle->radius;

	PG_RETURN_CIRCLE_P(result);
}

Datum
circle_sub_pt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	CIRCLE	   *result;

	result = (CIRCLE *) palloc(sizeof(CIRCLE));

	point_sub_point(&result->center, &circle->center, point);
	result->radius = circle->radius;

	PG_RETURN_CIRCLE_P(result);
}


/* circle_mul_pt()
 * 旋转与缩放算子。
 */
Datum
circle_mul_pt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	CIRCLE	   *result;

	result = (CIRCLE *) palloc(sizeof(CIRCLE));

	point_mul_point(&result->center, &circle->center, point);
	result->radius = float8_mul(circle->radius, HYPOT(point->x, point->y));

	PG_RETURN_CIRCLE_P(result);
}

Datum
circle_div_pt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	CIRCLE	   *result;

	result = (CIRCLE *) palloc(sizeof(CIRCLE));

	point_div_point(&result->center, &circle->center, point);
	result->radius = float8_div(circle->radius, HYPOT(point->x, point->y));

	PG_RETURN_CIRCLE_P(result);
}


/*		circle_area		-		返回圆的面积。
 */
Datum
circle_area(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);

	PG_RETURN_FLOAT8(circle_ar(circle));
}


/*		circle_diameter -		返回圆的直径。
 */
Datum
circle_diameter(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);

	PG_RETURN_FLOAT8(float8_mul(circle->radius, 2.0));
}


/*		circle_radius	-		返回圆的半径。
 */
Datum
circle_radius(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);

	PG_RETURN_FLOAT8(circle->radius);
}


/*		circle_distance -		返回两个圆之间
 *						  的距离。
 */
Datum
circle_distance(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle1 = PG_GETARG_CIRCLE_P(0);
	CIRCLE	   *circle2 = PG_GETARG_CIRCLE_P(1);
	float8		result;

	result = float8_mi(point_dt(&circle1->center, &circle2->center),
					   float8_pl(circle1->radius, circle2->radius));
	if (result < 0.0)
		result = 0.0;

	PG_RETURN_FLOAT8(result);
}


Datum
circle_contain_pt(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	float8		d;

	d = point_dt(&circle->center, point);
	PG_RETURN_BOOL(d <= circle->radius);
}


Datum
pt_contained_circle(PG_FUNCTION_ARGS)
{
	Point	   *point = PG_GETARG_POINT_P(0);
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(1);
	float8		d;

	d = point_dt(&circle->center, point);
	PG_RETURN_BOOL(d <= circle->radius);
}


/*		dist_pc	-		返回一个点与
 *				  一个圆的距离。
 */
Datum
dist_pc(PG_FUNCTION_ARGS)
{
	Point	   *point = PG_GETARG_POINT_P(0);
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(1);
	float8		result;

	result = float8_mi(point_dt(point, &circle->center),
					   circle->radius);
	if (result < 0.0)
		result = 0.0;

	PG_RETURN_FLOAT8(result);
}

/*
 * 圆到点的距离
 */
Datum
dist_cpoint(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *point = PG_GETARG_POINT_P(1);
	float8		result;

	result = float8_mi(point_dt(point, &circle->center), circle->radius);
	if (result < 0.0)
		result = 0.0;

	PG_RETURN_FLOAT8(result);
}

/*		circle_center	-		返回圆的圆心。
 */
Datum
circle_center(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	Point	   *result;

	result = (Point *) palloc(sizeof(Point));
	result->x = circle->center.x;
	result->y = circle->center.y;

	PG_RETURN_POINT_P(result);
}


/*		circle_ar		-		返回圆的面积。
 */
static float8
circle_ar(CIRCLE *circle)
{
	return float8_mul(float8_mul(circle->radius, circle->radius), M_PI);
}


/*----------------------------------------------------------
 * 转换算子。
 *---------------------------------------------------------*/

Datum
cr_circle(PG_FUNCTION_ARGS)
{
	Point	   *center = PG_GETARG_POINT_P(0);
	float8		radius = PG_GETARG_FLOAT8(1);
	CIRCLE	   *result;

	result = (CIRCLE *) palloc(sizeof(CIRCLE));

	result->center.x = center->x;
	result->center.y = center->y;
	result->radius = radius;

	PG_RETURN_CIRCLE_P(result);
}

Datum
circle_box(PG_FUNCTION_ARGS)
{
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(0);
	BOX		   *box;
	float8		delta;

	box = (BOX *) palloc(sizeof(BOX));

	delta = float8_div(circle->radius, sqrt(2.0));

	box->high.x = float8_pl(circle->center.x, delta);
	box->low.x = float8_mi(circle->center.x, delta);
	box->high.y = float8_pl(circle->center.y, delta);
	box->low.y = float8_mi(circle->center.y, delta);

	PG_RETURN_BOX_P(box);
}

/* box_circle()
 * 将矩形转换为圆。
 */
Datum
box_circle(PG_FUNCTION_ARGS)
{
	BOX		   *box = PG_GETARG_BOX_P(0);
	CIRCLE	   *circle;

	circle = (CIRCLE *) palloc(sizeof(CIRCLE));

	circle->center.x = float8_div(float8_pl(box->high.x, box->low.x), 2.0);
	circle->center.y = float8_div(float8_pl(box->high.y, box->low.y), 2.0);

	circle->radius = point_dt(&circle->center, &box->high);

	PG_RETURN_CIRCLE_P(circle);
}


Datum
circle_poly(PG_FUNCTION_ARGS)
{
	int32		npts = PG_GETARG_INT32(0);
	CIRCLE	   *circle = PG_GETARG_CIRCLE_P(1);
	POLYGON    *poly;
	int			base_size,
				size;
	int			i;
	float8		angle;
	float8		anglestep;

	if (FPzero(circle->radius))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot convert circle with radius zero to polygon")));

	if (npts < 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("must request at least 2 points")));

	base_size = sizeof(poly->p[0]) * npts;
	size = offsetof(POLYGON, p) + base_size;

	/* 检查整数溢出 */
	if (base_size / npts != sizeof(poly->p[0]) || size <= base_size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many points requested")));

	poly = (POLYGON *) palloc0(size);	/* 空洞置零 */
	SET_VARSIZE(poly, size);
	poly->npts = npts;

	anglestep = float8_div(2.0 * M_PI, npts);

	for (i = 0; i < npts; i++)
	{
		angle = float8_mul(anglestep, i);

		poly->p[i].x = float8_mi(circle->center.x,
								 float8_mul(circle->radius, cos(angle)));
		poly->p[i].y = float8_pl(circle->center.y,
								 float8_mul(circle->radius, sin(angle)));
	}

	make_bound_box(poly);

	PG_RETURN_POLYGON_P(poly);
}

/*
 * 将多边形转换为圆
 *
 * 结果必须预先分配。
 *
 * XXX 本算法应使用线段的加权均值，
 *	而非点的简单平均值 - tgl 97/01/21。
 */
static void
poly_to_circle(CIRCLE *result, POLYGON *poly)
{
	int			i;

	Assert(poly->npts > 0);

	result->center.x = 0;
	result->center.y = 0;
	result->radius = 0;

	for (i = 0; i < poly->npts; i++)
		point_add_point(&result->center, &result->center, &poly->p[i]);
	result->center.x = float8_div(result->center.x, poly->npts);
	result->center.y = float8_div(result->center.y, poly->npts);

	for (i = 0; i < poly->npts; i++)
		result->radius = float8_pl(result->radius,
								   point_dt(&poly->p[i], &result->center));
	result->radius = float8_div(result->radius, poly->npts);
}

Datum
poly_circle(PG_FUNCTION_ARGS)
{
	POLYGON    *poly = PG_GETARG_POLYGON_P(0);
	CIRCLE	   *result;

	result = (CIRCLE *) palloc(sizeof(CIRCLE));

	poly_to_circle(result, poly);

	PG_RETURN_CIRCLE_P(result);
}


/***********************************************************************
 **
 **		多类型私有例程。
 **
 ***********************************************************************/

/*
 * 测试点是否在多边形内部，返回 1/0，
 * 在多边形上则为2。
 * 代码改编但未照抄自 WN: A
 *	Server for the HTTP
 *	version 1.15.1, file wn/image.c
 *	http://hopf.math.northwestern.edu/index.html
 *	Description of algorithm:  http://www.linuxjournal.com/article/2197
 *							   http://www.linuxjournal.com/article/2029
 */

#define POINT_ON_POLYGON INT_MAX

static int
point_inside(Point *p, int npts, Point *plist)
{
	float8		x0,
				y0;
	float8		prev_x,
				prev_y;
	int			i = 0;
	float8		x,
				y;
	int			cross,
				total_cross = 0;

	Assert(npts > 0);

	/* 计算第一个多边形点相对于单点 */
	x0 = float8_mi(plist[0].x, p->x);
	y0 = float8_mi(plist[0].y, p->y);

	prev_x = x0;
	prev_y = y0;
	/* 遍历多边形各点并累计 total_cross */
	for (i = 1; i < npts; i++)
	{
		/* 计算下一个多边形点相对于单点 */
		x = float8_mi(plist[i].x, p->x);
		y = float8_mi(plist[i].y, p->y);

		/* 计算前一点到当前点的跨越 */
		if ((cross = lseg_crossing(x, y, prev_x, prev_y)) == POINT_ON_POLYGON)
			return 2;
		total_cross += cross;

		prev_x = x;
		prev_y = y;
	}

	/* 现在处理首点 */
	if ((cross = lseg_crossing(x0, y0, prev_x, prev_y)) == POINT_ON_POLYGON)
		return 2;
	total_cross += cross;

	if (total_cross != 0)
		return 1;
	return 0;
}


/* lseg_crossing()
 * 线段以 +/- 方向跨过正 X 轴时返回 +/-2。
 * 一个点位于正 X 轴上时返回 +/-1。
 * 两点都在正 X 轴上或没有跨越时返回 0。
 * 线段包含 (0,0) 时返回 POINT_ON_POLYGON。
 * 哇，这真是一个令人困惑的 API，但上面用到了它，
 * 求和即可判断点在多边形内。
 */

static int
lseg_crossing(float8 x, float8 y, float8 prev_x, float8 prev_y)
{
	float8		z;
	int			y_sign;

	if (FPzero(y))
	{							/* y==0 在 X 轴上 */
		if (FPzero(x))			/* (x,y)=(0,0)? */
			return POINT_ON_POLYGON;
		else if (FPgt(x, 0))
		{						/* x > 0 */
			if (FPzero(prev_y)) /* y、prev_y 均为零 */
				/* prev_x > 0? */
				return FPgt(prev_x, 0.0) ? 0 : POINT_ON_POLYGON;
			return FPlt(prev_y, 0.0) ? 1 : -1;
		}
		else
		{						/* x < 0，x 不在正 X 轴上 */
			if (FPzero(prev_y))
				/* prev_x < 0? */
				return FPlt(prev_x, 0.0) ? 0 : POINT_ON_POLYGON;
			return 0;
		}
	}
	else
	{							/* y != 0 */
		/* 计算 y 相对前一点的跨越方向 */
		y_sign = FPgt(y, 0.0) ? 1 : -1;

		if (FPzero(prev_y))
			/* 前一点在 X 轴上，故新点要么离开要么在其上 */
			return FPlt(prev_x, 0.0) ? 0 : y_sign;
		else if ((y_sign < 0 && FPlt(prev_y, 0.0)) ||
				 (y_sign > 0 && FPgt(prev_y, 0.0)))
			/* 同在 X 轴上或下方 */
			return 0;			/* 同号 */
		else
		{						/* y 与 prev_y 跨越 X 轴 */
			if (FPge(x, 0.0) && FPgt(prev_x, 0.0))
				/* 均为非负，故跨越正 X 轴 */
				return 2 * y_sign;
			if (FPlt(x, 0.0) && FPle(prev_x, 0.0))
				/* 均为非正，故不跨越正 X 轴 */
				return 0;

			/* x 与 y 跨轴，见上方 point_inside() 的URL */
			z = float8_mi(float8_mul(float8_mi(x, prev_x), y),
						  float8_mul(float8_mi(y, prev_y), x));
			if (FPzero(z))
				return POINT_ON_POLYGON;
			if ((y_sign < 0 && FPlt(z, 0.0)) ||
				(y_sign > 0 && FPgt(z, 0.0)))
				return 0;
			return 2 * y_sign;
		}
	}
}


static bool
plist_same(int npts, Point *p1, Point *p2)
{
	int			i,
				ii,
				j;

	/* 查找首点的匹配 */
	for (i = 0; i < npts; i++)
	{
		if (point_eq_point(&p2[i], &p1[0]))
		{

			/* 找到匹配？则向前查看其余各点 */
			for (ii = 1, j = i + 1; ii < npts; ii++, j++)
			{
				if (j >= npts)
					j = 0;
				if (!point_eq_point(&p2[j], &p1[ii]))
					break;
			}
			if (ii == npts)
				return true;

			/* 向前未找到匹配？则向后查看 */
			for (ii = 1, j = i - 1; ii < npts; ii++, j--)
			{
				if (j < 0)
					j = (npts - 1);
				if (!point_eq_point(&p2[j], &p1[ii]))
					break;
			}
			if (ii == npts)
				return true;
		}
	}

	return false;
}


/*-------------------------------------------------------------------------
 * 求斜边。
 *
 * 如有需要，则交换 x 与 y 使 x 成为较大数。
 * 传统的 x^2+y^2 公式经重排把 x 因子提到
 * sqrt 之外。故能对显著更大的值计算斜边，
 * 且比使用朴素公式精度更高。特别是，
 * 除非最终结果越界，否则这不会溢出。
 *
 *
 * sqrt( x^2 + y^2 ) = sqrt( x^2( 1 + y^2/x^2) )
 *					 = x * sqrt( 1 + y^2/x^2 )
 *					 = x * sqrt( 1 + y/x * y/x )
 *
 * 预期本例程最终将被 C99 hypot()
 * 函数取代。
 *
 * 本实现遵循 IEEE Std 1003.1 与 GLIBC：即
 * hypot(inf,nan) 的结果是 INF 而非 NAN。
 *-----------------------------------------------------------------------
 */
float8
pg_hypot(float8 x, float8 y)
{
	float8		yx,
				result;

	/* 正确处理 INF 与 NaN */
	if (isinf(x) || isinf(y))
		return get_float8_infinity();

	if (isnan(x) || isnan(y))
		return get_float8_nan();

	/* 否则去掉负号 */
	x = fabs(x);
	y = fabs(y);

	/* 如有需要则交换 x 与 y 使 x 较大 */
	if (x < y)
	{
		float8		temp = x;

		x = y;
		y = temp;
	}

	/*
	 * 若 y 为零则斜边为 x。这一测试在此类情形
	 * 下节省几个周期，但更重要的是它还能
	 * 防止除零错误，因 x >= y。
	 */
	if (y == 0.0)
		return x;

	/* 求斜边 */
	yx = y / x;
	result = x * sqrt(1.0 + (yx * yx));

	if (unlikely(isinf(result)))
		float_overflow_error();
	if (unlikely(result == 0.0))
		float_underflow_error();

	return result;
}
