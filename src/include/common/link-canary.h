/*-------------------------------------------------------------------------
 *
 * link-canary.h
 *	  检测 src/common 函数来自前端还是后端。
 *
 * Copyright (c) 2018-2023, PostgreSQL Global Development Group
 *
 * src/include/common/link-canary.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LINK_CANARY_H
#define LINK_CANARY_H

extern bool pg_link_canary_is_frontend(void);

#endif							/* LINK_CANARY_H */
