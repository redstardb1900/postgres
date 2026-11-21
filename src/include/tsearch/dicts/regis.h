/*-------------------------------------------------------------------------
 *
 * regis.h
 *
 * ISpell 所用快速正则子集的声明
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/tsearch/dicts/regis.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __REGIS_H__
#define __REGIS_H__

typedef struct RegisNode
{
	uint32
				type:2,
				len:16,
				unused:14;
	struct RegisNode *next;
	unsigned char data[FLEXIBLE_ARRAY_MEMBER];
} RegisNode;

#define  RNHDRSZ	(offsetof(RegisNode,data))

#define RSF_ONEOF	1
#define RSF_NONEOF	2

typedef struct Regis
{
	RegisNode  *node;
	uint32
				issuffix:1,
				nchar:16,
				unused:15;
} Regis;

extern bool RS_isRegis(const char *str);

extern void RS_compile(Regis *r, bool issuffix, const char *str);
extern void RS_free(Regis *r);

/*匹配则返回 true  */
extern bool RS_execute(Regis *r, char *str);

#endif
