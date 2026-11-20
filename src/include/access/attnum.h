/*-------------------------------------------------------------------------
 *
 * attnum.h
 *	  POSTGRES 属性号定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/attnum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATTNUM_H
#define ATTNUM_H


/*
 * 用户定义的属性号从 1 开始。   -ay 2/95
 */
typedef int16 AttrNumber;

#define InvalidAttrNumber		0
#define MaxAttrNumber			32767

/* ----------------
 *		辅助宏
 * ----------------
 */
/*
 * AttributeNumberIsValid
 *		属性号有效时才为真。
 */
#define AttributeNumberIsValid(attributeNumber) \
	((bool) ((attributeNumber) != InvalidAttrNumber))

/*
 * AttrNumberIsForUserDefinedAttr
 *		属性号对应于用户定义属性时才为真。
 */
#define AttrNumberIsForUserDefinedAttr(attributeNumber) \
	((bool) ((attributeNumber) > 0))

/*
 * AttrNumberGetAttrOffset
 *		返回属性号对应的属性偏移。
 *
 * Note:
 *		假定该属性号用于用户定义属性。
 */
#define AttrNumberGetAttrOffset(attNum) \
( \
	AssertMacro(AttrNumberIsForUserDefinedAttr(attNum)), \
	((attNum) - 1) \
)

/*
 * AttrOffsetGetAttrNumber
 *		返回属性偏移对应的属性号。
 */
#define AttrOffsetGetAttrNumber(attributeOffset) \
	 ((AttrNumber) (1 + (attributeOffset)))

#endif							/* ATTNUM_H */
