/*-------------------------------------------------------------------------
 *
 * value.c
 *	  implementation of value nodes
 *
 *
 * Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/value.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/value.h"

/*
 *	makeInteger
 */
Integer *
makeInteger(int i)
{
	Integer    *v = makeNode(Integer);

	v->ival = i;
	return v;
}

/*
 *	makeFloat
 *
 * 调用方负责传入一个 palloc 出来的字符串。
 */
Float *
makeFloat(char *numericStr)
{
	Float	   *v = makeNode(Float);

	v->fval = numericStr;
	return v;
}

/*
 *	makeBoolean
 */
Boolean *
makeBoolean(bool val)
{
	Boolean    *v = makeNode(Boolean);

	v->boolval = val;
	return v;
}

/*
 *	makeString
 *
 * 调用方负责传入一个 palloc 出来的字符串。
 */
String *
makeString(char *str)
{
	String	   *v = makeNode(String);

	v->sval = str;
	return v;
}

/*
 *	makeBitString
 *
 * 调用方负责传入一个 palloc 出来的字符串。
 */
BitString *
makeBitString(char *str)
{
	BitString  *v = makeNode(BitString);

	v->bsval = str;
	return v;
}
