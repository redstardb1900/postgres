/*-------------------------------------------------------------------------
 *
 * stringinfo.h
 *	  "StringInfo" 函数的声明与定义。
 *
 * StringInfo 提供一种可扩展字符串类型（当前上限
 * 1GB）。可缓冲普通 C 字符串（空字节结尾文本）
 * 或任意二进制数据。全部存储用 palloc() 分配
 * （前端代码回退到 malloc）。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/lib/stringinfo.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STRINGINFO_H
#define STRINGINFO_H

/*-------------------------
 * StringInfoData 保存可扩展字符串的信息。
 *		data	当前字符串缓冲区（用 palloc 分配）。
 *		len		当前字符串长度。保证 data[len] 处有
 *				终止 '\0'，但字符串存二进制而非文本时
 *				这一点不大有用。
 *		maxlen	为 'data' 已分配字节数，即当前可存入
 *				'data'、不必再分配即可容纳的最大字符串
 *				大小（含终止 '\0'）。必须始终 
 *				maxlen > len。
 *		cursor	由 makeStringInfo 或 initStringInfo 置零，
 *				stringinfo.c 例程此外不再改动。
 *				有些例程用它扫描 StringInfo。
 *-------------------------
 */
typedef struct StringInfoData
{
	char	   *data;
	int			len;
	int			maxlen;
	int			cursor;
} StringInfoData;

typedef StringInfoData *StringInfo;


/*------------------------
 * 最初有两种方式创建 StringInfo 对象：
 *
 * StringInfo stringptr = makeStringInfo();
 *		StringInfoData 与数据缓冲区都用 palloc。
 *
 * StringInfoData string;
 * initStringInfo(&string);
 *		数据缓冲用 palloc，StringInfoData 只是局部变量。
 *		若对象只活在当前例程期间，这是最省事的
 *		做法。
 *
 * 销毁 StringInfo 时先 pfree() 数据缓冲，再 pfree()
 * palloc 出来的 StringInfoData。没有专门接口。
 *
 * 注意：有些例程用 StringInfo 拼出字符串，然后
 * 释放 StringInfoData，却把数据字符串本身返回给
 * 调用方。此时该数据串看起来就像普通 palloc 的
 * 串。
 *-------------------------
 */

/*------------------------
 * makeStringInfo
 * 创建空的 'StringInfoData' 并返回其指针。
 */
extern StringInfo makeStringInfo(void);

/*------------------------
 * initStringInfo
 * 初始化内容未定义的 StringInfoData 结构，使其
 * 描述空串。
 */
extern void initStringInfo(StringInfo str);

/*------------------------
 * resetStringInfo
 * 若有内容则清除 StringInfo 当前内容。
 * StringInfo 仍有效。
 */
extern void resetStringInfo(StringInfo str);

/*------------------------
 * appendStringInfo
 * 按 fmt（sprintf 风格格式串）格式化文本数据，
 * 追加到 str 已有内容。必要时为 str 分配更多
 * 空间。有点像 sprintf 与 
 * strcat.
 */
extern void appendStringInfo(StringInfo str, const char *fmt,...) pg_attribute_printf(2, 3);

/*------------------------
 * appendStringInfoVA
 * 尝试按 fmt（sprintf 风格格式串）格式化文本
 * 数据并追加到 str。成功返回零；若空间不足则
 * 返回所需空间估计且不改 str。调用方通常应把
 * 返回值交给 enlargeStringInfo() 后再试；见
 * appendStringInfo 的标准
 * 用法。
 */
extern int	appendStringInfoVA(StringInfo str, const char *fmt, va_list args) pg_attribute_printf(2, 0);

/*------------------------
 * appendStringInfoString
 * 向 str 追加空终止字符串。
 * 同 appendStringInfo(str, "%s", s) 但更快。
 */
extern void appendStringInfoString(StringInfo str, const char *s);

/*------------------------
 * appendStringInfoChar
 * 向 str 追加一字节。
 * 同 appendStringInfo(str, "%c", ch) 但更快。
 */
extern void appendStringInfoChar(StringInfo str, char ch);

/*------------------------
 * appendStringInfoCharMacro
 * 同上，但是宏，速度要紧处更快。
 * 注意：str 实参会被求值多次。
 */
#define appendStringInfoCharMacro(str,ch) \
	(((str)->len + 1 >= (str)->maxlen) ? \
	 appendStringInfoChar(str, ch) : \
	 (void)((str)->data[(str)->len] = (ch), (str)->data[++(str)->len] = '\0'))

/*------------------------
 * appendStringInfoSpaces
 * 向 str 追加给定个数的空格。
 */
extern void appendStringInfoSpaces(StringInfo str, int count);

/*------------------------
 * appendBinaryStringInfo
 * 向 StringInfo 追加任意二进制数据，必要时
 * 再分配。
 */
extern void appendBinaryStringInfo(StringInfo str,
								   const void *data, int datalen);

/*------------------------
 * appendBinaryStringInfoNT
 * 向 StringInfo 追加任意二进制数据，必要时
 * 分配更多空间。不保证存在尾随空字节。
 */
extern void appendBinaryStringInfoNT(StringInfo str,
									 const void *data, int datalen);

/*------------------------
 * enlargeStringInfo
 * 确保 StringInfo 缓冲还能再放下至少 'needed' 字节。
 */
extern void enlargeStringInfo(StringInfo str, int needed);

#endif							/* STRINGINFO_H */
