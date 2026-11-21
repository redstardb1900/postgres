/*-------------------------------------------------------------------------
 *
 * 恢复配置生成器
 *
 * Portions Copyright (c) 2011-2023, PostgreSQL Global Development Group
 *
 * src/include/fe_utils/recovery_gen.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECOVERY_GEN_H
#define RECOVERY_GEN_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

/*
 * 恢复配置在 12 及以上属于 postgresql.conf，
 * 此前在 recovery.conf。
 */
#define MINIMUM_VERSION_FOR_RECOVERY_GUC 120000

extern PQExpBuffer GenerateRecoveryConfig(PGconn *pgconn,
										  char *replication_slot);
extern void WriteRecoveryConfig(PGconn *pgconn, char *target_dir,
								PQExpBuffer contents);

#endif							/* RECOVERY_GEN_H */
