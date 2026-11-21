/*
 * seclabel.h
 *
 * commands/seclabel.c 中函数的原型
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */
#ifndef SECLABEL_H
#define SECLABEL_H

#include "catalog/objectaddress.h"

/*
 * 内部 API
 */
extern char *GetSecurityLabel(const ObjectAddress *object,
							  const char *provider);
extern void SetSecurityLabel(const ObjectAddress *object,
							 const char *provider, const char *label);
extern void DeleteSecurityLabel(const ObjectAddress *object);
extern void DeleteSharedSecurityLabel(Oid objectId, Oid classId);

/*
 * 语句与 ESP 钩子支持
 */
extern ObjectAddress ExecSecLabelStmt(SecLabelStmt *stmt);

typedef void (*check_object_relabel_type) (const ObjectAddress *object,
										   const char *seclabel);
extern void register_label_provider(const char *provider_name,
									check_object_relabel_type hook);

#endif							/* SECLABEL_H */
