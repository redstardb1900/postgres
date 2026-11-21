/*-------------------------------------------------------------------------
 *
 * openssl.h
 *	  前后端共享的 OpenSSL 支持功能
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/include/common/openssl.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMON_OPENSSL_H
#define COMMON_OPENSSL_H

#ifdef USE_OPENSSL
#include <openssl/ssl.h>

/*
 * OpenSSL 未提供很好的方式标识库支持的协议
 * 版本上下限，故尽力仿造。尤其未计入安装
 * openssl.cnf 中可能规定的限制。
 * 略
 *
 * 库设置中禁用 SSLv3 及更旧，故感兴趣的最旧
 * 协议版本为 TLSv1。
 */
#define MIN_OPENSSL_TLS_VERSION  "TLSv1"

#if defined(TLS1_3_VERSION)
#define MAX_OPENSSL_TLS_VERSION  "TLSv1.3"
#elif defined(TLS1_2_VERSION)
#define MAX_OPENSSL_TLS_VERSION  "TLSv1.2"
#elif defined(TLS1_1_VERSION)
#define MAX_OPENSSL_TLS_VERSION  "TLSv1.1"
#else
#define MAX_OPENSSL_TLS_VERSION  "TLSv1"
#endif

/* src/common/protocol_openssl.c */
#ifndef SSL_CTX_set_min_proto_version
extern int	SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version);
extern int	SSL_CTX_set_max_proto_version(SSL_CTX *ctx, int version);
#endif

#endif							/* USE_OPENSSL */

#endif							/* COMMON_OPENSSL_H */
