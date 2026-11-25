/*-------------------------------------------------------------------------
 *
 * pqexpbuffer.h
 *	  PQExpBuffer 函数的声明与定义。
 *
 * PQExpBuffer 提供长度不限的可扩展的串数据类型。
 * 它可用来缓冲普通 C 字符串（空字节结尾的文本），
 * 或任意二进制数据。全部存储用 malloc() 分配。
 *
 * 本模块与后端的 StringInfo 数据类型基本相同，
 * 但它旨在供前端 libpq 与客户端应用程序使用。
 * 因此它不依赖 palloc() 或 elog()。
 *
 * 它确实依赖 vsnprintf()；若 configure 发现 libc 未提供
 * 可用的 vsnprintf()，就把我们自己实现的一份副本
 * 链入 libpq。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/pqexpbuffer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQEXPBUFFER_H
#define PQEXPBUFFER_H

/*-------------------------
 * PQExpBufferData 保存关于可扩展的串的信息。
 *		data	是该串的当前缓冲区（用 malloc 分配）。
 *		len		是当前字符串长度。保证 data[len] 处有
 *				终止 '\0'，但该串存二进制数据而非文本时
 *				这一点并不很有用。
 *		maxlen	是 'data' 缓冲区已分配大小，即当前无需
 *				重新分配就能存入 'data' 的最大字符串
 *				大小（含终止 '\0' 字符）。我们
 *				必须始终满足 maxlen > len。
 *
 * 若我们未能为字符串缓冲区分配够内存，就出现例外。
 * 此时 data 指向一块静态分配的空字符串，
 * 且 len = maxlen = 0.
 *-------------------------
 */
typedef struct PQExpBufferData
{
	char	   *data;
	size_t		len;
	size_t		maxlen;
} PQExpBufferData;

typedef PQExpBufferData *PQExpBuffer;

/*------------------------
 * 检测内存耗尽而损坏的 PQExpBuffer。
 * 当缓冲区"损坏"时，除重置或删除它外，其余全为
 * 空操作.
 *------------------------
 */
#define PQExpBufferBroken(str)	\
	((str) == NULL || (str)->maxlen == 0)

/*------------------------
 * 同上，但用于静态或局部的 PQExpBufferData 结构时。
 * 对此，空指针测试无用，且可能引发编译器警告。
 *------------------------
 */
#define PQExpBufferDataBroken(buf)	\
	((buf).maxlen == 0)

/*------------------------
 * PQExpBuffer 里数据缓冲区的初始大小。
 * 注意：它必须足以容纳可能返回的错误消息，由
 * PQrequestCancel() 所返回。
 *------------------------
 */
#define INITIAL_EXPBUFFER_SIZE	256

/*------------------------
 * 起初创建 PQExpBuffer 对象有两种方式：
 *
 * PQExpBuffer stringptr = createPQExpBuffer();
 *		PQExpBufferData 与数据缓冲区都由 malloc 分配。
 *
 * PQExpBufferData string;
 * initPQExpBuffer(&string);
 *		数据缓冲区经 malloc，而 PQExpBufferData 预先备好。
 *		若 PQExpBufferData 是另一个结构的字段，则此法
 *		合适.
 *-------------------------
 */

/*------------------------
 * createPQExpBuffer
 * 创建空 PQExpBufferData，并返回其指针。
 */
extern PQExpBuffer createPQExpBuffer(void);

/*------------------------
 * initPQExpBuffer
 * 初始化 PQExpBufferData 结构（内容原先未经定义），
 * 用来描述一个空串。
 */
extern void initPQExpBuffer(PQExpBuffer str);

/*------------------------
 * 销毁 PQExpBuffer 可用其一：
 *
 * destroyPQExpBuffer(str);
 *		free() 数据缓冲与 PQExpBufferData。
 *		这是 createPQExpBuffer() 的逆操作。
 *
 * termPQExpBuffer(str)
 *		释放数据缓冲区，不释放 PQExpBufferData 本身。
 *		这是 initPQExpBuffer() 的逆操作。
 *
 * 注意：有些例程用 PQExpBuffer 拼出字符串，然后
 * 释放 PQExpBufferData，但把数据串本身返回给其
 * 调用方。此时数据串看起来像经 malloc 分配的普通
 * 串。
 */
extern void destroyPQExpBuffer(PQExpBuffer str);
extern void termPQExpBuffer(PQExpBuffer str);

/*------------------------
 * resetPQExpBuffer
 *		重置 PQExpBuffer 为空
 *
 * 注意：若有可能，损坏的 PQExpBuffer 恢复正常。
 */
extern void resetPQExpBuffer(PQExpBuffer str);

/*------------------------
 * enlargePQExpBuffer
 * 确保缓冲区有足够空间再放 'needed' 个字节
 * 其中 'needed' 不含末尾的终止空字节。
 *
 * 成功返回 1，扩大缓冲区失败则返回 0。（后一情况
 * 下，缓冲区留于损坏态。）
 */
extern int	enlargePQExpBuffer(PQExpBuffer str, size_t needed);

/*------------------------
 * printfPQExpBuffer
 * 按 fmt（类似 sprintf 的格式串）控制来格式化文本数据
 * 并把它插入 str。必要时为 str 再分配空间。
 * 这是便捷例程，作用等同于先调用
 * resetPQExpBuffer() 再接 appendPQExpBuffer()。
 */
extern void printfPQExpBuffer(PQExpBuffer str, const char *fmt,...) pg_attribute_printf(2, 3);

/*------------------------
 * appendPQExpBuffer
 * 按 fmt（类似 sprintf 的格式串）控制来格式化文本数据
 * 并把它追加到 str 已有的任何内容后。更多空间则
 * 分配给 str，必要时。这有点像组合使用 sprintf 与
 * strcat.
 */
extern void appendPQExpBuffer(PQExpBuffer str, const char *fmt,...) pg_attribute_printf(2, 3);

/*------------------------
 * appendPQExpBufferVA
 * 尝试格式化并追加数据到 str。完成则返回 true，
 * 即成功或彻底失败；若需重试则返回 false。
 *
 * 注意：调用方必须保住其进入时的 errno，
 * 于循环中，以防 fmt 含 "%m"。
 */
extern bool appendPQExpBufferVA(PQExpBuffer str, const char *fmt, va_list args) pg_attribute_printf(2, 0);

/*------------------------
 * appendPQExpBufferStr
 * 向 PQExpBuffer 追加给定字符串，更多空间分配在
 * 必要时。
 */
extern void appendPQExpBufferStr(PQExpBuffer str, const char *data);

/*------------------------
 * appendPQExpBufferChar
 * 向 str 追加一字节。
 * 像 appendPQExpBuffer(str, "%c", ch) 但快得多。
 */
extern void appendPQExpBufferChar(PQExpBuffer str, char ch);

/*------------------------
 * appendBinaryPQExpBuffer
 * 向 PQExpBuffer 追加任意二进制数据，更多空间分配在
 * 必要时。
 */
extern void appendBinaryPQExpBuffer(PQExpBuffer str,
									const char *data, size_t datalen);

#endif							/* PQEXPBUFFER_H */
