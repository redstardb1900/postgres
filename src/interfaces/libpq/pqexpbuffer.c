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


/* 损坏的 PQExpBuffers 都指向此字符串    */
static const char oom_buffer[1] = "";

/* 需要一个 char * 以兼容 unconstify()   */
static const char *oom_buffer_ptr = oom_buffer;


/*
 * markPQExpBufferBroken
 *
 * 将未损坏的 PQExpBuffer 置为 "损坏" 状态。   
 */
static void
markPQExpBufferBroken(PQExpBuffer str)
{
	if (str->data != oom_buffer)
		free(str->data);

	/*
	 * 此处去掉 const 有点丑，但似乎胜过不将 oom_buffer 标  
	 * 记为 const。我们想这样做是为了促使编译器把 oom_buffer
	 * 放入只读存储，这样任何试图乱写损坏的 PQExpBuffer 
	 * 的人都会得到失败。                         
	 */
	str->data = unconstify(char *, oom_buffer_ptr);
	str->len = 0;
	str->maxlen = 0;
}

/*
 * createPQExpBuffer
 *
 * 创建一个空 'PQExpBufferData' 并返回其指针。    
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

/*
 * initPQExpBuffer
 *
 * 初始化一个 PQExpBufferData 结构体（先前内容未定义）， 
 * 来描述空字符串。    
 */
void
initPQExpBuffer(PQExpBuffer str)
{
	str->data = (char *) malloc(INITIAL_EXPBUFFER_SIZE);
	if (str->data == NULL)
	{
		str->data = unconstify(char *, oom_buffer_ptr); /* 见上文注释   */
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

/*
 * destroyPQExpBuffer(str);
 *
 *		释放数据缓冲区和 PQExpBufferData。          
 *		这是 createPQExpBuffer() 的逆操作。 
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

/*
 * termPQExpBuffer(str)
 *		释放数据缓冲区但不释放 PQExpBufferData 本身。
 *		这是 initPQExpBuffer() 的逆操作。 
 */
void
termPQExpBuffer(PQExpBuffer str)
{
	if (str->data != oom_buffer)
		free(str->data);
	/* 为保险起见，使缓冲区合法为空。 */
	str->data = unconstify(char *, oom_buffer_ptr); /* 见上文注释   */
	str->maxlen = 0;
	str->len = 0;
}

/*
 * resetPQExpBuffer
 *		将 PQExpBuffer 重置为空
 *
 * 注意：如有可能，损坏的 PQExpBuffer 会恢复正常。
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
			/* 尝试重新初始化为有效态  */
			initPQExpBuffer(str);
		}
	}
}

/*
 * enlargePQExpBuffer
 * 确保缓冲区中有足够空间容纳 'needed' 个以上的字节  
 * （'needed' 不包括终止 null）。           
 *
 * 成功返回 1，扩大缓冲区失败返回 0。（此时缓冲区 
 * 将处于"损坏"状态。）         
 */
int
enlargePQExpBuffer(PQExpBuffer str, size_t needed)
{
	size_t		newlen;
	char	   *newdata;

	if (PQExpBufferBroken(str))
		return 0;				/* 已经失败   */

	/*
	 * 防止荒谬的 "needed" 值，输入虚假数据时可能出现，   
	 * 若无此防护，后续可能出现溢出或无限循环。         
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

	/*
	 * 我们不想每次追加只分配一点额外空间；为效率，  
	 * 每次溢出时将缓冲区加倍。实际上若 'needed'     
	 * 很大，可能需要加倍以上……                             
	 */
	newlen = (str->maxlen > 0) ? (2 * str->maxlen) : 64;
	while (needed > newlen)
		newlen = 2 * newlen;

	/*
	 * 若已越过 INT_MAX 则限制到 INT_MAX。注意这里我们假定
	 * INT_MAX <= UINT_MAX/2，否则上述循环可能溢出。我们仍 
	 * 会有 newlen >= needed。       
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

/*
 * printfPQExpBuffer
 * 在 fmt（sprintf 式的格式字符串）控制下格式化文本数据 
 * 据，将其插入 str。必要时为 str 分配更多空间。这是
 * 一个便捷例程，等效于先 resetPQExpBuffer()     
 * 再 appendPQExpBuffer()。                         
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

/*
 * appendPQExpBuffer
 *
 * 在 fmt（sprintf 式格式串）的控制下格式化文本数据，    
 * 将其追加到 str 已有内容上。必要时为 str 分配更多  
 * 空间。这有点像是 sprintf 与 strcat 的组合。               
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

/*
 * appendPQExpBufferVA
 * printfPQExpBuffer/appendPQExpBuffer 共享核心。
 * 尝试格式化数据并追加到 str。完成时返回 true      
 * （成功或硬失败均可），需重试时返回 false。
 *
 * 注意：调用者循环时须保留进入时的 errno，以防 
 * fmt 中含有 "%m"。                       
 */
bool
appendPQExpBufferVA(PQExpBuffer str, const char *fmt, va_list args)
{
	size_t		avail;
	size_t		needed;
	int			nprinted;

	/*
	 * 尝试将给定字符串格式化进可用空间；但若几乎没有  
	 * 多少空间，就不必尝试，直接先扩大缓冲区。         
	 */
	if (str->maxlen > str->len + 16)
	{
		avail = str->maxlen - str->len;

		nprinted = vsnprintf(str->data + str->len, avail, fmt, args);

		/*
		 * 若 vsnprintf 报告错误则失败（我们假定这是格式串
		 * 有问题）。                         
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

		/*
		 * 我们假定 vsnprintf 符合 C99 标准，并相信其对所需   
		 * 空间的估计，并为结尾 null 加一。（若有误，逻辑 
		 * 仍会正常工作，但可能循环多次。              
		 *
		 * 若所需空间超过 INT_MAX 则中止，因为 str->maxlen 不   
		 * 能表示更大的数值。    
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
		/*
		 * 由于跳过了格式化工作，必须猜测要扩大多少。  
		 * 幸运的是，enlargePQExpBuffer 偏好 2 的幂次大小， 
		 * 此数字不太敏感；净效果是在运行 vsnprintf 之前将 
		 * 缓冲区大小加倍，这似乎是合理的。                 
		 *                                        
		 */
		needed = 32;
	}

	/* 增加缓冲区大小并重试。       */
	if (!enlargePQExpBuffer(str, needed))
		return true;			/* 内存不足        */

	return false;
}

/*
 * appendPQExpBufferStr
 * 将给定字符串追加到 PQExpBuffer，必要时扩容。   
 *              
 */
void
appendPQExpBufferStr(PQExpBuffer str, const char *data)
{
	appendBinaryPQExpBuffer(str, data, strlen(data));
}

/*
 * appendPQExpBufferChar
 * 向 str 追加单字节。  
 * 类似 appendPQExpBuffer(str, "%c", ch) 但更快。  
 */
void
appendPQExpBufferChar(PQExpBuffer str, char ch)
{
	/* 需要时再扩展空间 */
	if (!enlargePQExpBuffer(str, 1))
		return;

	/* 好的，追加字符    */
	str->data[str->len] = ch;
	str->len++;
	str->data[str->len] = '\0';
}

/*
 * appendBinaryPQExpBuffer
 *
 * 将任意二进制数据追加到 PQExpBuffer，必要时扩容。  
 *              
 */
void
appendBinaryPQExpBuffer(PQExpBuffer str, const char *data, size_t datalen)
{
	/* 需要时再扩展空间 */
	if (!enlargePQExpBuffer(str, datalen))
		return;

	/* 好，追加数据  */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/*
	 * 保留结尾的空字符，尽管它对二进制数据来说多半  
	 * 用不上...  
	 */
	str->data[str->len] = '\0';
}
