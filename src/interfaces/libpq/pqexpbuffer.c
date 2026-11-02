/*-------------------------------------------------------------------------
 *
 * pqexpbuffer.c
 *
 * PQExpBuffer provides an indefinitely-extensible string data type.
 * It can be used to buffer either ordinary C strings (null-terminated text)
 * or arbitrary binary data.  All storage is allocated with malloc().
 *
 * This module is essentially the same as the backend's StringInfo data type,
 * but it is intended for use in frontend libpq and client applications.
 * Thus, it does not rely on palloc() nor elog(), nor psprintf.c which
 * will exit() on error.
 *
 * It does rely on vsnprintf(); if configure finds that libc doesn't provide
 * a usable vsnprintf(), then a copy of our own implementation of it will
 * be linked into libpq.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq/pqexpbuffer.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <limits.h>

#include "pqexpbuffer.h"

#ifdef WIN32
#include "win32.h"
#endif


/* All "broken" PQExpBuffers point to this string. */
static const char oom_buffer[1] = "";

/* 需要一个 char * 以兼容 unconstify()   */
static const char *oom_buffer_ptr = oom_buffer;


/*
 * markPQExpBufferBroken
 *
 * Put a PQExpBuffer in "broken" state if it isn't already.
 */
static void
markPQExpBufferBroken(PQExpBuffer str)
{
	if (str->data != oom_buffer)
		free(str->data);

	/*此处去除 const 有点丑陋，但似乎比不将 oom_buffer 标记为
	 * const 更好。我们想这样做以鼓励编译器将 oom_buffer 放在
	 * 只读存储中，这样任何试图在损坏的 PQExpBuffer 上乱写的人
	 * 都会得到失败。
	 * 
	                  */
	str->data = unconstify(char *, oom_buffer_ptr);
	str->len = 0;
	str->maxlen = 0;
}

/*
 * createPQExpBuffer
 *
 * Create an empty 'PQExpBufferData' & return a pointer to it.
 */
PQExpBuffer
createPQExpBuffer(void)
{
	PQExpBuffer res;

	res = (PQExpBuffer) malloc(sizeof(PQExpBufferData));
	if (res != NULL)
		initPQExpBuffer(res);

	return res;
}

/*initPQExpBuffer
 * 
 *初始化一个 PQExpBufferData 结构体（先前内容未定义）
 * 以描述一个空字符串。
 * 
   */
void
initPQExpBuffer(PQExpBuffer str)
{
	str->data = (char *) malloc(INITIAL_EXPBUFFER_SIZE);
	if (str->data == NULL)
	{
		str->data = unconstify(char *, oom_buffer_ptr); /* see comment above */
		str->maxlen = 0;
		str->len = 0;
	}
	else
	{
		str->maxlen = INITIAL_EXPBUFFER_SIZE;
		str->len = 0;
		str->data[0] = '\0';
	}
}

/*destroyPQExpBuffer(str);
 * 
 *		释放数据缓冲区和 PQExpBufferData。
 *		这是 createPQExpBuffer() 的逆操作。
 *	
           */
void
destroyPQExpBuffer(PQExpBuffer str)
{
	if (str)
	{
		termPQExpBuffer(str);
		free(str);
	}
}

/*termPQExpBuffer(str)
 * 		释放数据缓冲区但不释放 PQExpBufferData 本身。
 *		这是 initPQExpBuffer() 的逆操作。
 *	
 */
void
termPQExpBuffer(PQExpBuffer str)
{
	if (str->data != oom_buffer)
		free(str->data);
	/* 为保险起见，使缓冲区合法为空。 */
	str->data = unconstify(char *, oom_buffer_ptr); /* see comment above */
	str->maxlen = 0;
	str->len = 0;
}

/*
 * resetPQExpBuffer
 *		Reset a PQExpBuffer to empty
 *
 * Note: if possible, a "broken" PQExpBuffer is returned to normal.
 */
void
resetPQExpBuffer(PQExpBuffer str)
{
	if (str)
	{
		if (str->data != oom_buffer)
		{
			str->len = 0;
			str->data[0] = '\0';
		}
		else
		{
			/* try to reinitialize to valid state */
			initPQExpBuffer(str);
		}
	}
}

/*enlargePQExpBuffer
 * 确保缓冲区中有足够空间容纳 'needed' 个以上的字节
 * （'needed' 不包括终止 null）。
 * 
 *成功则返回 1，扩大缓冲区失败则返回 0。（后一种情况
 * 缓冲区将处于"损坏"状态。）
 * 
         */
int
enlargePQExpBuffer(PQExpBuffer str, size_t needed)
{
	size_t		newlen;
	char	   *newdata;

	if (PQExpBufferBroken(str))
		return 0;				/* 已经失败   */

	/*防止荒谬的 "needed" 值，这可能在输入虚假数据时发生。
	 * 没有此防护，后续可能出现溢出或无限循环。
	 * 
	 * 
	                     */
	if (needed >= ((size_t) INT_MAX - str->len))
	{
		markPQExpBufferBroken(str);
		return 0;
	}

	needed += str->len + 1;		/* 现在所需的总空间 */

	/* 由于上述测试，现在 needed <= INT_MAX            */

	if (needed <= str->maxlen)
		return 1;				/* 已有足够空间       */

	/*我们不想每次追加时只分配一点点额外空间；为效率起见，
	 * 每次溢出时将缓冲区大小加倍。实际上，如果 'needed' 很大，
	 * 可能需要加倍以上……
	 * 
	            */
	newlen = (str->maxlen > 0) ? (2 * str->maxlen) : 64;
	while (needed > newlen)
		newlen = 2 * newlen;

	/*如果超过 INT_MAX 则限制到 INT_MAX。注意这里假设
	 * INT_MAX <= UINT_MAX/2，否则上述循环可能溢出。我们仍然会
	 * 有 newlen >= needed。
	 * 
	            */
	if (newlen > (size_t) INT_MAX)
		newlen = (size_t) INT_MAX;

	newdata = (char *) realloc(str->data, newlen);
	if (newdata != NULL)
	{
		str->data = newdata;
		str->maxlen = newlen;
		return 1;
	}

	markPQExpBufferBroken(str);
	return 0;
}

/*printfPQExpBuffer
 * 在 fmt（类似 sprintf 的格式字符串）控制下格式化文本数据
 * 并将其插入 str。必要时为 str 分配更多空间。
 * 这是一个便捷例程，等效于 resetPQExpBuffer() 后再调用
 * appendPQExpBuffer()。
 * 
                         */
void
printfPQExpBuffer(PQExpBuffer str, const char *fmt,...)
{
	int			save_errno = errno;
	va_list		args;
	bool		done;

	resetPQExpBuffer(str);

	if (PQExpBufferBroken(str))
		return;					/* 已经失败   */

	/* 循环，以防扩大缓冲区后需要重试。          */
	do
	{
		errno = save_errno;
		va_start(args, fmt);
		done = appendPQExpBufferVA(str, fmt, args);
		va_end(args);
	} while (!done);
}

/*appendPQExpBuffer
 * 
 *在 fmt（类似 sprintf 的格式字符串）控制下格式化文本数据
 * 并将其追加到 str 中已有的内容。必要时为 str 分配更多空间。
 * 这有点像是 sprintf 和 strcat 的组合。
 * 
 * 
                */
void
appendPQExpBuffer(PQExpBuffer str, const char *fmt,...)
{
	int			save_errno = errno;
	va_list		args;
	bool		done;

	if (PQExpBufferBroken(str))
		return;					/* 已经失败   */

	/* 循环，以防扩大缓冲区后需要重试。          */
	do
	{
		errno = save_errno;
		va_start(args, fmt);
		done = appendPQExpBufferVA(str, fmt, args);
		va_end(args);
	} while (!done);
}

/*appendPQExpBufferVA
 * printfPQExpBuffer/appendPQExpBuffer 的共享核心。
 * 尝试格式化数据并将其追加到 str。完成时返回 true（成功或
 * 硬失败），需要重试时返回 false。
 * 
 *注意：调用者必须确保在循环时保留其进入时的 errno，以防
 * fmt 包含 "%m"。
 * 
             */
bool
appendPQExpBufferVA(PQExpBuffer str, const char *fmt, va_list args)
{
	size_t		avail;
	size_t		needed;
	int			nprinted;

	/*尝试将给定字符串格式化到可用空间中；但如果空间很少，
	 * 则不必尝试，先扩大缓冲区。
	 * 
	                        */
	if (str->maxlen > str->len + 16)
	{
		avail = str->maxlen - str->len;

		nprinted = vsnprintf(str->data + str->len, avail, fmt, args);

		/*如果 vsnprintf 报告错误，则失败（我们假定这意味着格式
	字符串有问题）。
	
	                 */
		if (unlikely(nprinted < 0))
		{
			markPQExpBufferBroken(str);
			return true;
		}

		if ((size_t) nprinted < avail)
		{
			/* 成功。注意 nprinted 不包括结尾的 null。     */
			str->len += nprinted;
			return true;
		}

		/*我们假定 vsnprintf 符合 C99 标准，因此相信其对所需空间的
	估计，并为结尾 null 加一。（如果错了，逻辑仍然可以工作，
	但可能会循环多次。）
	
	如果所需空间超过 INT_MAX 则阻塞，因为 str->maxlen 不能
	表示超过此值。
	
	                                      */
		if (unlikely(nprinted > INT_MAX - 1))
		{
			markPQExpBufferBroken(str);
			return true;
		}
		needed = nprinted + 1;
	}
	else
	{
		/*由于跳过了格式化工作，我们必须猜测扩大多少。幸运的是，
	由于 enlargePQExpBuffer 偏好 2 的幂次大小，此数字不太敏感；
	最终效果是在尝试运行 vsnprintf 之前将缓冲区大小加倍，
	这似乎是合理的。
	
	
	                                                               */
		needed = 32;
	}

	/* 增加缓冲区大小并重试。       */
	if (!enlargePQExpBuffer(str, needed))
		return true;			/* oops, out of memory */

	return false;
}

/*appendPQExpBufferStr
 * 将给定字符串追加到 PQExpBuffer，必要时分配更多空间。
 * 
 * 
     */
void
appendPQExpBufferStr(PQExpBuffer str, const char *data)
{
	appendBinaryPQExpBuffer(str, data, strlen(data));
}

/*
 * appendPQExpBufferChar
 * Append a single byte to str.
 * Like appendPQExpBuffer(str, "%c", ch) but much faster.
 */
void
appendPQExpBufferChar(PQExpBuffer str, char ch)
{
	/* Make more room if needed */
	if (!enlargePQExpBuffer(str, 1))
		return;

	/* 好的，追加字符    */
	str->data[str->len] = ch;
	str->len++;
	str->data[str->len] = '\0';
}

/*appendBinaryPQExpBuffer
 * 
 *将任意二进制数据追加到 PQExpBuffer，必要时分配更多空间。
 * 
 * 
    */
void
appendBinaryPQExpBuffer(PQExpBuffer str, const char *data, size_t datalen)
{
	/* Make more room if needed */
	if (!enlargePQExpBuffer(str, datalen))
		return;

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/*
	 * Keep a trailing null in place, even though it's probably useless for
	 * binary data...
	 */
	str->data[str->len] = '\0';
}
