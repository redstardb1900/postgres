/*-------------------------------------------------------------------------
 *
 * fe-auth-sasl.h
 *	为 libpq 定义 SASL 机制接口。
 *
 * 每种 SASL 机制定义前端与后端各自的回调结构。
 * 这不是供应用程序使用的公开 API。
 *
 * 见 src/include/libpq/sasl.h 的后端对应部分。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/fe-auth-sasl.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FE_AUTH_SASL_H
#define FE_AUTH_SASL_H

#include "libpq-fe.h"

/*
 * 前端 SASL 机制回调。
 *
 * 要实现前端机制，声明带有
 * 合适回调实现的 pg_be_sasl_mech 结构，再挂入 conn->sasl，于
 * pg_SASL_init() 的机制协商期间。
 */
typedef struct pg_fe_sasl_mech
{
	/*-------
	 * init()
	 *
	 * 为连接初始化机制专属状态。此
	 * 回调必须返回指向其所分配状态的指针，并将
	 * 它原样作为第一个参数传给其他回调。
	 * free() 回调被调用来释放任何状态资源。
	 *
	 * 若状态分配失败，实现应返回 NULL 以
	 * 使认证交换失败。
	 *
	 * 输入参数：
	 *
	 * conn：到服务器的连接
	 *
	 * password：用户为当前连接提供的密码
	 *
	 * mech：在用的机制名，供那些可能
	 *	公布不止一个名称的实现（如 *-PLUS 变体）。
	 *-------
	 */
	void	   *(*init) (PGconn *conn, const char *password, const char *mech);

	/*--------
	 * exchange()
	 *
	 * 生成对服务器质询的客户端响应。特例：
	 * 对客户端先发 SASL 机制，在认证交换开始时
	 * 用 NULL 服务器响应调用一次 exchange()，以
	 * 生成初始响应。
	 *
	 * 输入参数：
	 *
	 *	state：init() 返回的不透明机制状态
	 *
	 *	input：服务器发来的质询数据，或为 NULL，当
	 *	生成客户端先发初始响应时（亦即
	 *	服务器期望客户端发送消息来启动
	 *	该交换）。它保证以空字符结尾，
	 *	目的是安全，但 SASL 允许质询内嵌空字符，
	 *	故机制必须小心检查 inputlen。
	 *
	 *	inputlen：服务器所发质询数据的长度，或在
	 * 生成客户端先发初始响应时为 -1。
	 *
	 * 输出参数，由回调函数设置：
	 *
	 *	output：一块 malloc 的缓冲，含客户端给
	 *	服务器的响应（可为空），或在交换应
	 *	被中止时为 NULL。（*success 应设为 false，于
	 *	后一情形。）
	 *
	 *	outputlen：客户端响应缓冲的长度（0 或更高），
	 *	output 为 NULL 时忽略。
	 *
	 *	done：若 SASL 交换不应继续则设为 true，
	 *	因该交换已完成或已失败。
	 *
	 *	success：若 SASL 交换成功完成则设为 true。
	 *	若*done 为 false 则忽略。
	 *--------
	 */
	void		(*exchange) (void *state, char *input, int inputlen,
							 char **output, int *outputlen,
							 bool *done, bool *success);

	/*--------
	 * channel_bound()
	 *
	 * 若连接已建立通道绑定则返回 true。
	 * 机制实现必须确保 SASL 交换确已
	 * 完成，此外还要检查通道绑定正在使用。
	 *
	 * 未实现通道绑定的机制可能直接返回
	 * false.
	 *
	 * 输入参数：
	 *
	 *	state：init() 返回的不透明机制状态
	 *--------
	 */
	bool		(*channel_bound) (void *state);

	/*--------
	 * free()
	 *
	 * 释放 init() 所分配的状态。本回调在连接
	 * 断开时调用，不是交换完成之时。
	 *
	 * 输入参数：
	 *
	 * state：init() 返回的不透明机制状态
	 *--------
	 */
	void		(*free) (void *state);

} pg_fe_sasl_mech;

#endif							/* FE_AUTH_SASL_H */
