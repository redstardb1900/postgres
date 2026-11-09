/*-------------------------------------------------------------------------
 *
 * copyfromparse.c
 *		解析 COPY FROM 文本/CSV/二进制格式
 *
 * 本文件包含解析文本、CSV 与二进制格式的例程。   
 * 主入口是 NextCopyFrom()，它解析下一条输入行并将   
 * 其作为 Datum 返回。                
 *
 * 在文本/CSV 模式下，解析分多个阶段进行： 
 *
 * [data source] --> raw_buf --> input_buf --> line_buf --> attribute_buf
 *                1.          2.            3.           4.
 *
 * 1. CopyLoadRawBuf() 从输入文件或客户端读取原始数据， 
 *    放入 'raw_buf'。      
 *
 * 2. CopyConvertBuf() 调用编码转换函数，将 'raw_buf' 中     
 *    的数据从客户端编码转为服务器编码，结果放入  
 *    'input_buf'。                  
 *
 * 3. CopyReadLine() 逐行解析 'input_buf' 中的数据。           
 *    它负责找出下一个换行符标记，并根据 COPY 选项把     
 *    引号和转义字符考虑在内。                                   
 *    该行被复制进 'line_buf'，引号与转义字符原样保    
 *    留。 
 *
 * 4. CopyReadAttributesText/CSV() 函数从 'line_buf' 中取出    
 *    输入行，并按需拆分为字段，同时执行去转义。        
 *    各字段存入 'attribute_buf'，'raw_fields' 数组保存           
 *    各字段的指针。  
 *
 * 如果不需要进行编码转换，第 2 步会采用捷径来避      
 * 免不必要的数据复制。'input_buf' 指针会被设置为         
 * 直接指向 'raw_buf'，使 CopyLoadRawBuf() 直接把原始           
 * 数据载入 'input_buf'。CopyConvertBuf() 之后只需               
 * 验证数据在当前编码下有效。   
 *
 * 二进制模式下流程要简单得多。输入被载入          
 * 'raw_buf'，编码转换（如需要）由各数据类型专有的 
 * 接收函数完成。'input_buf' 和 'line_buf' 并不使用，          
 * 但 'attribute_buf' 会用作临时缓冲区，保存传给接收        
 * 函数时单个属性的数据。          
 *
 * 'raw_buf' 的大小固定为 64 kB（RAW_BUF_SIZE）。若需要      
 * 进行编码转换，'input_buf' 也是 64 kB（INPUT_BUF_SIZE）。   
 * 'line_buf' 和 'attribute_buf' 按需扩展，以容纳迄今为    
 * 止最长的行。 
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copyfromparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "commands/copy.h"
#include "commands/copyfrom_internal.h"
#include "commands/progress.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#define ISOCTAL(c) (((c) >= '0') && ((c) <= '7'))
#define OCTVALUE(c) ((c) - '0')

/*
 * 这些宏集中处理 line_buf 和 input_buf 缓冲区的代码。
 * 它们用宏实现是为了支持 continue/break 控制流，并避免
 * 在紧凑的 COPY 循环中产生函数调用开销。
 *
 * 我们必须使用 "if (1)" 形式，因为通常的 "do {...} while(0)" 包装会
 * 阻止 continue/break 处理正常工作。以 "else ((void) 0)" 结束 "if (1)"
 * 是为了确保该 "if" 不会意外匹配调用代码中的 "else"，同时避免编译器
 * 对空语句发出警告。参见 http://www.cit.gu.edu.au/~anthony/info/C/C.macros。
 *
 */

/*
 * 即使在多次预读的情况下，也能将循环顶部读取的字符
 * 保留在缓冲区中。
 */
#define IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && !hit_eof) \
	{ \
		input_buf_ptr = prev_raw_ptr; /* 撤销获取 */ \
		need_data = true; \
		continue; \
	} \
} else ((void) 0)

/* 这会消耗缓冲区的剩余内容并跳出        */
#define IF_NEED_REFILL_AND_EOF_BREAK(extralen) \
if (1) \
{ \
	if (input_buf_ptr + (extralen) >= copy_buf_len && hit_eof) \
	{ \
		if (extralen) \
			input_buf_ptr = copy_buf_len; /* 消耗部分字符 */ \
		/* EOF 前的反斜杠，视为数据字符 */ \
		result = true; \
		break; \
	} \
} else ((void) 0)

/*
 * 将已批准的数据传输到 line_buf；必须执行此操作以确保
 * input_buf 中有空间。
 */
#define REFILL_LINEBUF \
if (1) \
{ \
	if (input_buf_ptr > cstate->input_buf_index) \
	{ \
		appendBinaryStringInfo(&cstate->line_buf, \
							 cstate->input_buf + cstate->input_buf_index, \
							   input_buf_ptr - cstate->input_buf_index); \
		cstate->input_buf_index = input_buf_ptr; \
	} \
} else ((void) 0)

/* 撤销预读并跳出该块。 */
#define NO_END_OF_COPY_GOTO \
if (1) \
{ \
	input_buf_ptr = prev_raw_ptr + 1; \
	goto not_end_of_copy; \
} else ((void) 0)

/* 注意：copyto.c 中有此代码的副本 */
static const char BinarySignature[11] = "PGCOPY\n\377\r\n\0";


/* 非导出函数原型 */
static bool CopyReadLine(CopyFromState cstate);
static bool CopyReadLineText(CopyFromState cstate);
static int	CopyReadAttributesText(CopyFromState cstate);
static int	CopyReadAttributesCSV(CopyFromState cstate);
static Datum CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
									 Oid typioparam, int32 typmod,
									 bool *isnull);


/* 低层通信函数 */
static int	CopyGetData(CopyFromState cstate, void *databuf,
						int minread, int maxread);
static inline bool CopyGetInt32(CopyFromState cstate, int32 *val);
static inline bool CopyGetInt16(CopyFromState cstate, int16 *val);
static void CopyLoadInputBuf(CopyFromState cstate);
static int	CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes);

void
ReceiveCopyBegin(CopyFromState cstate)
{
	StringInfoData buf;
	int			natts = list_length(cstate->attnumlist);
	int16		format = (cstate->opts.binary ? 1 : 0);
	int			i;

	pq_beginmessage(&buf, 'G');
	pq_sendbyte(&buf, format);	/* 整体格式 */
	pq_sendint16(&buf, natts);
	for (i = 0; i < natts; i++)
		pq_sendint16(&buf, format); /* 每列格式 */
	pq_endmessage(&buf);
	cstate->copy_src = COPY_FRONTEND;
	cstate->fe_msgbuf = makeStringInfo();
	/* 必须在此刷新以确保前端知道它可以发送。 */
	pq_flush();
}

void
ReceiveCopyBinaryHeader(CopyFromState cstate)
{
	char		readSig[11];
	int32		tmp;

	/* 签名 */
	if (CopyReadBinaryData(cstate, readSig, 11) != 11 ||
		memcmp(readSig, BinarySignature, 11) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("COPY file signature not recognized")));
	/* 标记字段 */
	if (!CopyGetInt32(cstate, &tmp))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing flags)")));
	if ((tmp & (1 << 16)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (WITH OIDS)")));
	tmp &= ~(1 << 16);
	if ((tmp >> 16) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unrecognized critical flags in COPY file header")));
	/* 标头扩展长度 */
	if (!CopyGetInt32(cstate, &tmp) ||
		tmp < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid COPY file header (missing length)")));
	/* 如果存在则跳过扩展标头 */
	while (tmp-- > 0)
	{
		if (CopyReadBinaryData(cstate, readSig, 1) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("invalid COPY file header (wrong length)")));
	}
}

/*
 * CopyGetData - 从数据源（文件或前端）读取数据
 *
 * 尝试从数据源读取至少 minread 字节、最多 maxread 字节。
 * 返回实际读取的字节数；如果少于 minread，则表示检测到 EOF。
 *
 * 注意：从前端复制时，期望按协议收到正确的 EOF 标记；
 * 如果前端仅断开连接，我们会报告错误。在此情况下
 * 允许 COPY IN 正常完成似乎是不明智的。
 *
 * 注意：此处不进行数据转换。
 *
 */
static int
CopyGetData(CopyFromState cstate, void *databuf, int minread, int maxread)
{
	int			bytesread = 0;

	switch (cstate->copy_src)
	{
		case COPY_FILE:
			bytesread = fread(databuf, 1, maxread, cstate->copy_file);
			if (ferror(cstate->copy_file))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read from COPY file: %m")));
			if (bytesread == 0)
				cstate->raw_reached_eof = true;
			break;
		case COPY_FRONTEND:
			while (maxread > 0 && bytesread < minread && !cstate->raw_reached_eof)
			{
				int			avail;

				while (cstate->fe_msgbuf->cursor >= cstate->fe_msgbuf->len)
				{
					/* 尝试接收另一条消息 */
					int			mtype;
					int			maxmsglen;

			readmessage:
					HOLD_CANCEL_INTERRUPTS();
					pq_startmsgread();
					mtype = pq_getbyte();
					if (mtype == EOF)
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					/* 验证消息类型并设置包大小限制 */
					switch (mtype)
					{
						case 'd':	/* CopyData */
							maxmsglen = PQ_LARGE_MESSAGE_LIMIT;
							break;
						case 'c':	/* CopyDone */
						case 'f':	/* CopyFail */
						case 'H':	/* Flush */
						case 'S':	/* Sync */
							maxmsglen = PQ_SMALL_MESSAGE_LIMIT;
							break;
						default:
							ereport(ERROR,
									(errcode(ERRCODE_PROTOCOL_VIOLATION),
									 errmsg("unexpected message type 0x%02X during COPY from stdin",
											mtype)));
							maxmsglen = 0;	/* 让编译器保持安静 */
							break;
					}
					/* 现在收集消息体 */
					if (pq_getmessage(cstate->fe_msgbuf, maxmsglen))
						ereport(ERROR,
								(errcode(ERRCODE_CONNECTION_FAILURE),
								 errmsg("unexpected EOF on client connection with an open transaction")));
					RESUME_CANCEL_INTERRUPTS();
					/* ... 然后处理它 */
					switch (mtype)
					{
						case 'd':	/* CopyData */
							break;
						case 'c':	/* CopyDone */
							/* COPY IN 被前端正确终止 */
							cstate->raw_reached_eof = true;
							return bytesread;
						case 'f':	/* CopyFail */
							ereport(ERROR,
									(errcode(ERRCODE_QUERY_CANCELED),
									 errmsg("COPY from stdin failed: %s",
											pq_getmsgstring(cstate->fe_msgbuf))));
							break;
						case 'H':	/* Flush */
						case 'S':	/* Sync */

							/*
							 * 忽略 Flush/Sync 以方便客户端库（如 libpq），
							 * 它们可能在未注意到刚发送的命令是 COPY 的情况下
							 * 发送这些消息。
							 *
							 */
							goto readmessage;
						default:
							Assert(false);	/* NOT REACHED */
					}
				}
				avail = cstate->fe_msgbuf->len - cstate->fe_msgbuf->cursor;
				if (avail > maxread)
					avail = maxread;
				pq_copymsgbytes(cstate->fe_msgbuf, databuf, avail);
				databuf = (void *) ((char *) databuf + avail);
				maxread -= avail;
				bytesread += avail;
			}
			break;
		case COPY_CALLBACK:
			bytesread = cstate->data_source_cb(databuf, minread, maxread);
			break;
	}

	return bytesread;
}


/*
 * 以下函数会进行数据转换
 */

/*
 * CopyGetInt32 - 读取网络字节序的 int32
 *
 * 成功返回 true，EOF 返回 false
 */
static inline bool
CopyGetInt32(CopyFromState cstate, int32 *val)
{
	uint32		buf;

	if (CopyReadBinaryData(cstate, (char *) &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* 抑制编译器警告 */
		return false;
	}
	*val = (int32) pg_ntoh32(buf);
	return true;
}

/*
 * CopyGetInt16 - 读取网络字节序的 int16
 */
static inline bool
CopyGetInt16(CopyFromState cstate, int16 *val)
{
	uint16		buf;

	if (CopyReadBinaryData(cstate, (char *) &buf, sizeof(buf)) != sizeof(buf))
	{
		*val = 0;				/* 抑制编译器警告     */
		return false;
	}
	*val = (int16) pg_ntoh16(buf);
	return true;
}


/*
 * 对 'raw_buf' 中的数据执行编码转换，将转换后的数据
 * 写入 'input_buf'。
 *
 * 入口处，'raw_buf' 中必须有需要转换的数据。
 */
static void
CopyConvertBuf(CopyFromState cstate)
{
	/*
		 * 如果文件和服务器编码相同，则不需要编码转换。
		 * 但是我们仍需验证输入对该编码有效。
		 *
		 */
	if (!cstate->need_transcoding)
	{
		/*
			 * 当不需要转换时，input_buf 和 raw_buf 是相同的。
			 * raw_buf_len 是缓冲区中的总字节数，
			 * input_buf_len 跟踪其中已验证的字节数。
			 *
			 */
		int			preverifiedlen = cstate->input_buf_len;
		int			unverifiedlen = cstate->raw_buf_len - cstate->input_buf_len;
		int			nverified;

		if (unverifiedlen == 0)
		{
			/*
				 * 如果没有更多原始数据，向调用者报告 EOF。
				 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
			 * 验证新数据，包括上一轮中剩余的
			 * 未验证字节。
			 */
		nverified = pg_encoding_verifymbstr(cstate->file_encoding,
											cstate->raw_buf + preverifiedlen,
											unverifiedlen);
		if (nverified == 0)
		{
			/*
				 * 无法验证任何内容。
				 *
				 * 如果没有更多的原始输入数据，意味着末尾有不完整的
				 * 多字节序列。另外，如果剩余的输入"足够"多，
				 * 我们应该能够验证至少一个字符，失败则意味着
				 * 遇到了无效字节序列。
				 *
				 */
			if (cstate->raw_reached_eof || unverifiedlen >= pg_encoding_max_length(cstate->file_encoding))
				cstate->input_reached_error = true;
			return;
		}
		cstate->input_buf_len += nverified;
	}
	else
	{
		/*
			 * 需要进行编码转换。
			 */
		int			nbytes;
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;
		int			convertedlen;

		if (RAW_BUF_BYTES(cstate) == 0)
		{
			/*
			 * 若无更多原始数据到来，向调用者报告 EOF。  
			 */
			if (cstate->raw_reached_eof)
				cstate->input_reached_eof = true;
			return;
		}

		/*
			 * 首先，复制所有未处理的数据。
			 */
		nbytes = INPUT_BUF_BYTES(cstate);
		if (nbytes > 0 && cstate->input_buf_index > 0)
			memmove(cstate->input_buf, cstate->input_buf + cstate->input_buf_index,
					nbytes);
		cstate->input_buf_index = 0;
		cstate->input_buf_len = nbytes;
		cstate->input_buf[nbytes] = '\0';

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		/*
		 * 执行转换。若输入有无效字节序列，转换可能提  
		 * 前停止。此时我们会尽可能多地转换数据。      
		 * 。       
		 *
		 * 注意：即使遇到无效字节序列，我们也不会立即报
		 * 告错误，而是在全部有效字节消费完才报错。  
		 * 输入中可能包含输入结束标记 (\.)，因此我们不 
		 * 希望无效字节序列位于结束标记之后时还报
		 * 错误。我们可能不必要地转换结束标记之后的  
		 * 一些数据，只要它对于该编码是有效的，     
		 * 但这是无害的。         
		 */
		convertedlen = pg_do_encoding_conversion_buf(cstate->conversion_proc,
													 cstate->file_encoding,
													 GetDatabaseEncoding(),
													 src, srclen,
													 dst, dstlen,
													 true);
		if (convertedlen == 0)
		{
			/*
				 * 无法转换任何内容。如果没有更多的原始输入数据，
				 * 意味着末尾有不完整的多字节序列。另外，
				 * 如果剩余大量输入，我们应该能转换至少一个字符，
				 * 失败则必须意味着遇到了无效字节序列。
				 *
				 *
				 */
			if (cstate->raw_reached_eof || srclen >= MAX_CONVERSION_INPUT_LENGTH)
				cstate->input_reached_error = true;
			return;
		}
		cstate->raw_buf_index += convertedlen;
		cstate->input_buf_len += strlen((char *) dst);
	}
}

/*
 * 报告编码或转换错误。
 */
static void
CopyConversionError(CopyFromState cstate)
{
	Assert(cstate->raw_buf_len > 0);
	Assert(cstate->input_reached_error);

	if (!cstate->need_transcoding)
	{
		/*
			 * input_buf_len 之前的所有内容都已成功验证，
			 * input_buf_len 指向无效或不完整的字符。
			 */
		report_invalid_encoding(cstate->file_encoding,
								cstate->raw_buf + cstate->input_buf_len,
								cstate->raw_buf_len - cstate->input_buf_len);
	}
	else
	{
		/*
			 * raw_buf_index 指向无效或无法翻译的字符。
			 * 我们让转换例程报告错误，因为它可以提供比我们
			 * 更具体的错误消息。之前 CopyConvertBuf() 中
			 * 对转换例程的调用检测到有错误，现在再次调用
			 * 转换例程（noError=false）以使其抛出错误。
			 *
			 */
		unsigned char *src;
		int			srclen;
		unsigned char *dst;
		int			dstlen;

		src = (unsigned char *) cstate->raw_buf + cstate->raw_buf_index;
		srclen = cstate->raw_buf_len - cstate->raw_buf_index;
		dst = (unsigned char *) cstate->input_buf + cstate->input_buf_len;
		dstlen = INPUT_BUF_SIZE - cstate->input_buf_len + 1;

		(void) pg_do_encoding_conversion_buf(cstate->conversion_proc,
											 cstate->file_encoding,
											 GetDatabaseEncoding(),
											 src, srclen,
											 dst, dstlen,
											 false);

		/*
		 * 转换例程应该已经报告了错误，
		 * 因此不应执行到这里。
		 */
		elog(ERROR, "encoding conversion failed without error");
	}
}

/*
 * 从数据源加载更多数据到 raw_buf。
 *
 * 如果 RAW_BUF_BYTES(cstate) > 0，未处理的字节
 * 会移到缓冲区开头，然后在其后加载新数据。
 */
static void
CopyLoadRawBuf(CopyFromState cstate)
{
	int			nbytes;
	int			inbytes;

	/*
	 * 在文本模式中，如果不需要编码转换，raw_buf 和 input_buf
	 * 指向同一缓冲区。它们的 len/index 也应该一致。
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->raw_buf_index == cstate->input_buf_index);
		Assert(cstate->input_buf_len <= cstate->raw_buf_len);
	}

	/*
	 * 如果有未处理的数据则复制下来。
	 */
	nbytes = RAW_BUF_BYTES(cstate);
	if (nbytes > 0 && cstate->raw_buf_index > 0)
		memmove(cstate->raw_buf, cstate->raw_buf + cstate->raw_buf_index,
				nbytes);
	cstate->raw_buf_len -= cstate->raw_buf_index;
	cstate->raw_buf_index = 0;

	/*
	 * 如果 raw_buf 和 input_buf 实际上是同一缓冲区，
	 * 也调整 input_buf 变量。
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		cstate->input_buf_len -= cstate->input_buf_index;
		cstate->input_buf_index = 0;
	}

	/* 需要更多数据 */
	inbytes = CopyGetData(cstate, cstate->raw_buf + cstate->raw_buf_len,
						  1, RAW_BUF_SIZE - cstate->raw_buf_len);
	nbytes += inbytes;
	cstate->raw_buf[nbytes] = '\0';
	cstate->raw_buf_len = nbytes;

	cstate->bytes_processed += inbytes;
	pgstat_progress_update_param(PROGRESS_COPY_BYTES_PROCESSED, cstate->bytes_processed);

	if (inbytes == 0)
		cstate->raw_reached_eof = true;
}

/*
 * CopyLoadInputBuf - 向 input_buf 加载更多数据
 *
 * 返回时，至少一个输入字符被加载到 input_buf 中，
 * 或者 input_reached_eof 被设置。
 *
 * 如果 INPUT_BUF_BYTES(cstate) > 0，未处理的字节
 * 移动到缓冲区开头，然后在其后加载更多数据。
 */
static void
CopyLoadInputBuf(CopyFromState cstate)
{
	int			nbytes = INPUT_BUF_BYTES(cstate);

	/*
	 * 调用者已更新 input_buf_index 以指示已消耗且不再
	 * 需要的输入量。如果 input_buf 与 raw_buf 是
	 * 同一物理区域，则相应地更新 raw_buf_index。
	 */
	if (cstate->raw_buf == cstate->input_buf)
	{
		Assert(!cstate->need_transcoding);
		Assert(cstate->input_buf_index >= cstate->raw_buf_index);
		cstate->raw_buf_index = cstate->input_buf_index;
	}

	for (;;)
	{
		/* 如果现在有未转换的数据，尝试转换它 */
		CopyConvertBuf(cstate);

		/* 如果现在有更多输入字节就绪，返回它们 */
		if (INPUT_BUF_BYTES(cstate) > nbytes)
			return;

		/*
			 * 如果遇到无效字节序列，或者处于不完整的多字节字符
			 * 但没有更多原始输入数据，报告转换错误。
			 *
			 */
		if (cstate->input_reached_error)
			CopyConversionError(cstate);

		/* 没有更多输入，且一切已转换 */
		if (cstate->input_reached_eof)
			break;

		/* 尝试加载更多原始数据 */
		Assert(!cstate->raw_reached_eof);
		CopyLoadRawBuf(cstate);
	}
}

/*
 * CopyReadBinaryData - 读取二进制数据
 *
 * 通过 cstate->raw_buf 从 cstate->copy_file 读取最多 'nbytes' 字节
 * 并写入 'dest'。返回读取的字节数
 * （仅在到达 EOF 时可能少于 'nbytes'）。
 */
static int
CopyReadBinaryData(CopyFromState cstate, char *dest, int nbytes)
{
	int			copied_bytes = 0;

	if (RAW_BUF_BYTES(cstate) >= nbytes)
	{
		/* 缓冲区中有足够多的字节。 */
		memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, nbytes);
		cstate->raw_buf_index += nbytes;
		copied_bytes = nbytes;
	}
	else
	{
		/*
		 * 缓冲区中字节不足，因此必须从文件读取。
		 * 需要循环，因为 'nbytes' 可能大于缓冲区大小。
		 */
		do
		{
			int			copy_bytes;

			/* 如果缓冲区为空则加载更多数据。 */
			if (RAW_BUF_BYTES(cstate) == 0)
			{
				CopyLoadRawBuf(cstate);
				if (cstate->raw_reached_eof)
					break;		/* EOF */
			}

			/* 传输一些字节。 */
			copy_bytes = Min(nbytes - copied_bytes, RAW_BUF_BYTES(cstate));
			memcpy(dest, cstate->raw_buf + cstate->raw_buf_index, copy_bytes);
			cstate->raw_buf_index += copy_bytes;
			dest += copy_bytes;
			copied_bytes += copy_bytes;
		} while (copied_bytes < nbytes);
	}

	return copied_bytes;
}

/*
 * 读取文本或 CSV 模式 COPY FROM 的下一行原始字段。
 * 如果没有更多行则返回 false。
 *
 * 通过 'fields' 返回内部临时缓冲区。该缓冲区在下次调用
 * 本函数之前有效。由于函数返回输入文件中的所有原始字段，
 * 'nfields' 可能与关系中的列数不同。
 *
 * 注意：返回的字段不应用 force_not_null 选项。
 *
 */
bool
NextCopyFromRawFields(CopyFromState cstate, char ***fields, int *nfields)
{
	int			fldct;
	bool		done;

	/* 仅适用于文本或 CSV 输入 */
	Assert(!cstate->opts.binary);

	/* 输入时检查标头行是否正确（如果需要） */
	if (cstate->cur_lineno == 0 && cstate->opts.header_line)
	{
		ListCell   *cur;
		TupleDesc	tupDesc;

		tupDesc = RelationGetDescr(cstate->rel);

		cstate->cur_lineno++;
		done = CopyReadLine(cstate);

		if (cstate->opts.header_line == COPY_HEADER_MATCH)
		{
			int			fldnum;

			if (cstate->opts.csv_mode)
				fldct = CopyReadAttributesCSV(cstate);
			else
				fldct = CopyReadAttributesText(cstate);

			if (fldct != list_length(cstate->attnumlist))
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("wrong number of fields in header line: got %d, expected %d",
								fldct, list_length(cstate->attnumlist))));

			fldnum = 0;
			foreach(cur, cstate->attnumlist)
			{
				int			attnum = lfirst_int(cur);
				char	   *colName;
				Form_pg_attribute attr = TupleDescAttr(tupDesc, attnum - 1);

				Assert(fldnum < cstate->max_fields);

				colName = cstate->raw_fields[fldnum++];
				if (colName == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("column name mismatch in header line field %d: got null value (\"%s\"), expected \"%s\"",
									fldnum, cstate->opts.null_print, NameStr(attr->attname))));

				if (namestrcmp(&attr->attname, colName) != 0)
				{
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("column name mismatch in header line field %d: got \"%s\", expected \"%s\"",
									fldnum, colName, NameStr(attr->attname))));
				}
			}
		}

		if (done)
			return false;
	}

	cstate->cur_lineno++;

	/* 在此处实际将行读入内存 */
	done = CopyReadLine(cstate);

	/*
	 * 行首 EOF 表示已完成。如果在某些字符后遇到 EOF，
	 * 则视为换行后跟 EOF，即处理该行
	 * 并在下次迭代时退出循环。
	 */
	if (done && cstate->line_buf.len == 0)
		return false;

	/* 将行解析为去转义的字段值 */
	if (cstate->opts.csv_mode)
		fldct = CopyReadAttributesCSV(cstate);
	else
		fldct = CopyReadAttributesText(cstate);

	*fields = cstate->raw_fields;
	*nfields = fldct;
	return true;
}

/*
 * 读取文件中 COPY FROM 的下一个元组。没有更多元组时返回 false。
 *
 * 'econtext' 用于评估每个列（未从文件读取或使用 COPY FROM 的 DEFAULT 选项）的默认表达式。
 * 当没有使用默认值（即所有列都从文件读取且未设置 DEFAULT 选项）时，
 * 它可以为 NULL。
 *
 * 'values' 和 'nulls' 数组的长度必须与传递给
 * BeginCopyFrom 的关系的列数相同。此函数填充这些数组。
 *
 */
bool
NextCopyFrom(CopyFromState cstate, ExprContext *econtext,
			 Datum *values, bool *nulls)
{
	TupleDesc	tupDesc;
	AttrNumber	num_phys_attrs,
				attr_count,
				num_defaults = cstate->num_defaults;
	FmgrInfo   *in_functions = cstate->in_functions;
	Oid		   *typioparams = cstate->typioparams;
	int			i;
	int		   *defmap = cstate->defmap;
	ExprState **defexprs = cstate->defexprs;

	tupDesc = RelationGetDescr(cstate->rel);
	num_phys_attrs = tupDesc->natts;
	attr_count = list_length(cstate->attnumlist);

	/* 将行的所有值初始化为 NULL */
	MemSet(values, 0, num_phys_attrs * sizeof(Datum));
	MemSet(nulls, true, num_phys_attrs * sizeof(bool));
	MemSet(cstate->defaults, false, num_phys_attrs * sizeof(bool));

	if (!cstate->opts.binary)
	{
		char	  **field_strings;
		ListCell   *cur;
		int			fldct;
		int			fieldno;
		char	   *string;

		/* 读取下一行的原始字段 */
		if (!NextCopyFromRawFields(cstate, &field_strings, &fldct))
			return false;

		/* 检查字段溢出 */
		if (attr_count > 0 && fldct > attr_count)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));

		fieldno = 0;

		/* 循环读取行上的用户属性。 */
		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			int			m = attnum - 1;
			Form_pg_attribute att = TupleDescAttr(tupDesc, m);

			if (fieldno >= fldct)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("missing data for column \"%s\"",
								NameStr(att->attname))));
			string = field_strings[fieldno++];

			if (cstate->convert_select_flags &&
				!cstate->convert_select_flags[m])
			{
				/* 忽略输入字段，将列保留为 NULL */
				continue;
			}

			if (cstate->opts.csv_mode)
			{
				if (string == NULL &&
					cstate->opts.force_notnull_flags[m])
				{
					/*
					 * FORCE_NOT_NULL 选项已设置且列为 NULL ——
					 * 将其转换为 NULL 字符串。
					 */
					string = cstate->opts.null_print;
				}
				else if (string != NULL && cstate->opts.force_null_flags[m]
						 && strcmp(string, cstate->opts.null_print) == 0)
				{
					/*
					 * FORCE_NULL 选项已设置且列与 NULL 字符串匹配。
					 * 它一定被引用了，否则该字符串早已被设置为 NULL。
					 * 按指定将其转换为 NULL。
					 *
					 */
					string = NULL;
				}
			}

			cstate->cur_attname = NameStr(att->attname);
			cstate->cur_attval = string;

			if (string != NULL)
				nulls[m] = false;

			if (cstate->defaults[m])
			{
				/*
				 * 调用者必须提供 econtext 并已切换
				 * 到其中的每个元组内存上下文。
				 */
				Assert(econtext != NULL);
				Assert(CurrentMemoryContext == econtext->ecxt_per_tuple_memory);

				values[m] = ExecEvalExpr(defexprs[m], econtext, &nulls[m]);
			}
			else
				values[m] = InputFunctionCall(&in_functions[m],
											  string,
											  typioparams[m],
											  att->atttypmod);

			cstate->cur_attname = NULL;
			cstate->cur_attval = NULL;
		}

		Assert(fieldno == attr_count);
	}
	else
	{
		/* 二进制 */
		int16		fld_count;
		ListCell   *cur;

		cstate->cur_lineno++;

		if (!CopyGetInt16(cstate, &fld_count))
		{
			/* 检测到 EOF（文件尾或协议级 EOF） */
			return false;
		}

		if (fld_count == -1)
		{
			/*
			 * 收到 EOF 标记。等待协议级 EOF，
			 * 如果没有立即到达则报错。在 COPY FROM STDIN 中，
			 * 这确保如果客户端选择此时发送 CopyFail，
			 * 我们能正确处理。从文件复制时，我们可以像文本模式那样
			 * 忽略文件其余部分，但我们选择与 COPY FROM STDIN 保持一致。
			 *
			 */
			char		dummy;

			if (CopyReadBinaryData(cstate, &dummy, 1) > 0)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("received copy data after EOF marker")));
			return false;
		}

		if (fld_count != attr_count)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("row field count is %d, expected %d",
							(int) fld_count, attr_count)));

		foreach(cur, cstate->attnumlist)
		{
			int			attnum = lfirst_int(cur);
			int			m = attnum - 1;
			Form_pg_attribute att = TupleDescAttr(tupDesc, m);

			cstate->cur_attname = NameStr(att->attname);
			values[m] = CopyReadBinaryAttribute(cstate,
												&in_functions[m],
												typioparams[m],
												att->atttypmod,
												&nulls[m]);
			cstate->cur_attname = NULL;
		}
	}

	/*
	 * 然后为输入数据未提供的列计算并填入所有可用  
	 * 默认值。凡是此处或上文未处理到的内容，都将保持 
	 * 为 NULL。 
	 */
	for (i = 0; i < num_defaults; i++)
	{
		/*
		 * 调用者必须提供 econtext，并已切换到其中的 
		 * 每元组内存上下文。    
		 */
		Assert(econtext != NULL);
		Assert(CurrentMemoryContext == econtext->ecxt_per_tuple_memory);

		values[defmap[i]] = ExecEvalExpr(defexprs[defmap[i]], econtext,
										 &nulls[defmap[i]]);
	}

	return true;
}

/*
 * 读取下一条输入行并存入 line_buf。     
 *
 * 若读取因 EOF 而终止则结果为真，因换行终止则   
 * 为假。终止换行符或 EOF 标记不包含在 line_buf 的   
 * 最终值中。                
 */
static bool
CopyReadLine(CopyFromState cstate)
{
	bool		result;

	resetStringInfo(&cstate->line_buf);
	cstate->line_buf_valid = false;

	/* 解析数据并传输到 line_buf     */
	result = CopyReadLineText(cstate);

	if (result)
	{
		/*
		 * 到达 EOF。在协议版本 3，\. 之后直到复制数据  
		 * 协议结束为止的任何内容都忽略。（XXX 或许最好
		 * 不把 \. 视为特殊？）
		 */
		if (cstate->copy_src == COPY_FRONTEND)
		{
			int			inbytes;

			do
			{
				inbytes = CopyGetData(cstate, cstate->input_buf,
									  1, INPUT_BUF_SIZE);
			} while (inbytes > 0);
			cstate->input_buf_index = 0;
			cstate->input_buf_len = 0;
			cstate->raw_buf_index = 0;
			cstate->raw_buf_len = 0;
		}
	}
	else
	{
		/*
		 * 如果我们没有碰到 EOF，那么必然已把 EOL 标记连同
		 * 数据一起传到了 line_buf。把它去掉。
		 */
		switch (cstate->eol_type)
		{
			case EOL_NL:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CR:
				Assert(cstate->line_buf.len >= 1);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\r');
				cstate->line_buf.len--;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_CRNL:
				Assert(cstate->line_buf.len >= 2);
				Assert(cstate->line_buf.data[cstate->line_buf.len - 2] == '\r');
				Assert(cstate->line_buf.data[cstate->line_buf.len - 1] == '\n');
				cstate->line_buf.len -= 2;
				cstate->line_buf.data[cstate->line_buf.len] = '\0';
				break;
			case EOL_UNKNOWN:
				/* shouldn't get here */
				Assert(false);
				break;
		}
	}

	/* 现在把它用于错误消息就安全了        */
	cstate->line_buf_valid = true;

	return result;
}

/*
 * CopyReadLineText - CopyReadLine 文本模式的内层循环
 */
static bool
CopyReadLineText(CopyFromState cstate)
{
	char	   *copy_input_buf;
	int			input_buf_ptr;
	int			copy_buf_len;
	bool		need_data = false;
	bool		hit_eof = false;
	bool		result = false;

	/* CSV variables */
	bool		first_char_in_line = true;
	bool		in_quote = false,
				last_was_esc = false;
	char		quotec = '\0';
	char		escapec = '\0';

	if (cstate->opts.csv_mode)
	{
		quotec = cstate->opts.quote[0];
		escapec = cstate->opts.escape[0];
		/* 若转义字符即引号字符则忽略特殊转义处理   */
		if (quotec == escapec)
			escapec = '\0';
	}

	/*
	 * 本循环的目标是把下一条完整的输入行传输到        
	 * line_buf 中。因此，我们只需检测换行（\r 和/或        
	 * \n）以及结束标记（\.）。  
	 *
	 * 在 CSV 模式中，带引号字段内的 \r 和 \n 只是数据值   
	 * 的一部分，会被放入 line_buf。我们只保存足以判断     
	 * 当前是否在带引号字段内。   
	 *
	 * 这四个字符，及 CSV 的转义字符与引号字符，被假  
	 * 定在前端和后端编码中是相同的。      
	 *
	 * 输入已转换为数据库编码。所有受支持的服务器编 
	 * 码都有这样一个性质：多字节序列中所有字节    
	 * 的最高位均已置位，因此多字节字符内不可能嵌    
	 * 入换行或转义字符。所以，我们可以逐字节处理
	 * 多字节序列。                                                    
	 * 而无需考虑编码。   
	 *
	 * 为了速度，我们尝试按块把数据从 input_buf 搬到     
	 * line_buf，而不是一次一个字符。input_buf_ptr 指向下      
	 * 一个待检查的字符；从 input_buf_index 到          
	 * input_buf_ptr 之间的字符已被判定为该行的一部分，     
	 * 尚未传入 line_buf。
	 *
	 * 为了在循环内再快一点，我们把 input_buf 和       
	 * input_buf_len 存入局部变量。
	 */
	copy_input_buf = cstate->input_buf;
	input_buf_ptr = cstate->input_buf_index;
	copy_buf_len = cstate->input_buf_len;

	for (;;)
	{
		int			prev_raw_ptr;
		char		c;

		/*
		 * 按需装载数据。    
		 *
		 * TODO: 可以强制预读 4 字节来避免大量调             
		 * 用 IF_NEED_REFILL_AND_NOT_EOF_CONTINUE() 的次数。        
		 * 这在旧 v2 COPY 协议下不安全，但我们已不再      
		 * 支持  
		 */
		if (input_buf_ptr >= copy_buf_len || need_data)
		{
			REFILL_LINEBUF;

			CopyLoadInputBuf(cstate);
			/* 更新局部变量         */
			hit_eof = cstate->input_reached_eof;
			input_buf_ptr = cstate->input_buf_index;
			copy_buf_len = cstate->input_buf_len;

			/*
			 * 若数据已完全用尽，则跳出循环，并报告  
			 * EOF。        
			 */
			if (INPUT_BUF_BYTES(cstate) <= 0)
			{
				result = true;
				break;
			}
			need_data = false;
		}

		/* OK to fetch a character */
		prev_raw_ptr = input_buf_ptr;
		c = copy_input_buf[input_buf_ptr++];

		if (cstate->opts.csv_mode)
		{
			/*
			 * 如果字符为 '\\' 或 '\r'，可能需要向下预读。   
			 * 如果还没有取到下一个字符，则强制获取它。  
			 * 必须在改动 CSV 状态前这样做，以防这些字符 
			 * 同时也是引号字符或转义字符。             
			 */
			if (c == '\\' || c == '\r')
			{
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			}

			/*
			 * 此处对引号与转义的处理颇为微妙。如果引号 
			 * 字符同时即转义字符，则没问题 - 只需把该  
			 * 个字符当作开关来用。如果两者不同，就需确 
			 * 保只考虑带引号字段内部、且紧跟引号字符
			 * 前的转义，且非转义-转义序列中的              
			 * 第二个。                            
			 */
			if (in_quote && c == escapec)
				last_was_esc = !last_was_esc;
			if (c == quotec && !last_was_esc)
				in_quote = !in_quote;
			if (c != escapec)
				last_was_esc = false;

			/*
			 * 更新内嵌 CR 和/或 LF 字符的行数难免脆弱 -  
			 * 这个测试大概已接近我们能做到的极限。        
			 * （XXX 我们是否应该这样做本身就有争议 ---     
			 * cur_lineno 是物理计数还是逻辑计数？）    
			 */
			if (in_quote && c == (cstate->eol_type == EOL_NL ? '\n' : '\r'))
				cstate->cur_lineno++;
		}

		/* Process \r */
		if (c == '\r' && (!cstate->opts.csv_mode || !in_quote))
		{
			/* 检查首行的 \r\n，并同时处理 \r\n。   */
			if (cstate->eol_type == EOL_UNKNOWN ||
				cstate->eol_type == EOL_CRNL)
			{
				/*
				 * 若需更多数据，回到循环顶部装载。  
				 *
				 * 注意：若处于 EOF，c 最终会是 '\0'，因为     
				 * input_buf 有保证的填充。    
				 */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);

				/* get next char */
				c = copy_input_buf[input_buf_ptr];

				if (c == '\n')
				{
					input_buf_ptr++;	/* eat newline */
					cstate->eol_type = EOL_CRNL;	/* in case not set yet */
				}
				else
				{
					/* found \r, but no \n */
					if (cstate->eol_type == EOL_CRNL)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 !cstate->opts.csv_mode ?
								 errmsg("literal carriage return found in data") :
								 errmsg("unquoted carriage return found in data"),
								 !cstate->opts.csv_mode ?
								 errhint("Use \"\\r\" to represent carriage return.") :
								 errhint("Use quoted CSV field to represent carriage return.")));

					/*
					 * 走到这里说明这是第一行且没有找到       
					 * \n，所以不要消耗预读的字符   
					 */
					cstate->eol_type = EOL_CR;
				}
			}
			else if (cstate->eol_type == EOL_NL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->opts.csv_mode ?
						 errmsg("literal carriage return found in data") :
						 errmsg("unquoted carriage return found in data"),
						 !cstate->opts.csv_mode ?
						 errhint("Use \"\\r\" to represent carriage return.") :
						 errhint("Use quoted CSV field to represent carriage return.")));
			/* 如果走到这里，说明已找到行结束符 */
			break;
		}

		/* Process \n */
		if (c == '\n' && (!cstate->opts.csv_mode || !in_quote))
		{
			if (cstate->eol_type == EOL_CR || cstate->eol_type == EOL_CRNL)
				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 !cstate->opts.csv_mode ?
						 errmsg("literal newline found in data") :
						 errmsg("unquoted newline found in data"),
						 !cstate->opts.csv_mode ?
						 errhint("Use \"\\n\" to represent newline.") :
						 errhint("Use quoted CSV field to represent newline.")));
			cstate->eol_type = EOL_NL;	/* in case not set yet */
			/* 如果走到这里，说明已找到行结束符 */
			break;
		}

		/*
		 * 在 CSV 模式中，只识别单独成行的 \.，这是因为     
		 * \. 是合法 CSV 数据值。
		 */
		if (c == '\\' && (!cstate->opts.csv_mode || first_char_in_line))
		{
			char		c2;

			IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
			IF_NEED_REFILL_AND_EOF_BREAK(0);

			/* -----
			 * 获取下一字符
			 * 注意：不改 c，若它不是 \.，就可以继续  
			 * 落入并接着处理。        
			 * -----
			 */
			c2 = copy_input_buf[input_buf_ptr];

			if (c2 == '.')
			{
				input_buf_ptr++;	/* consume the '.' */

				/*
				 * 注意：若在此回环取数据，也不影响：  
				 * CSV 状态变化检查被重复执行并无妨碍；会    
				 * 带着未变化的重要状态回到这里。       
				 */
				if (cstate->eol_type == EOL_CRNL)
				{
					/* 获取下一字符     */
					IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
					/* hit_eof 时 c2 会变成 '\0'   */
					c2 = copy_input_buf[input_buf_ptr++];

					if (c2 == '\n')
					{
						if (!cstate->opts.csv_mode)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker does not match previous newline style")));
						else
							NO_END_OF_COPY_GOTO;
					}
					else if (c2 != '\r')
					{
						if (!cstate->opts.csv_mode)
							ereport(ERROR,
									(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
									 errmsg("end-of-copy marker corrupt")));
						else
							NO_END_OF_COPY_GOTO;
					}
				}

				/* 获取下一字符     */
				IF_NEED_REFILL_AND_NOT_EOF_CONTINUE(0);
				/* hit_eof 时 c2 会变成 '\0'   */
				c2 = copy_input_buf[input_buf_ptr++];

				if (c2 != '\r' && c2 != '\n')
				{
					if (!cstate->opts.csv_mode)
						ereport(ERROR,
								(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
								 errmsg("end-of-copy marker corrupt")));
					else
						NO_END_OF_COPY_GOTO;
				}

				if ((cstate->eol_type == EOL_NL && c2 != '\n') ||
					(cstate->eol_type == EOL_CRNL && c2 != '\n') ||
					(cstate->eol_type == EOL_CR && c2 != '\r'))
				{
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("end-of-copy marker does not match previous newline style")));
				}

				/*
				 * 把 \. 之前的数据传输到 line_buf，然后丢弃 
				 * 该数据以及 \. 序列。         
				 */
				if (prev_raw_ptr > cstate->input_buf_index)
					appendBinaryStringInfo(&cstate->line_buf,
										   cstate->input_buf + cstate->input_buf_index,
										   prev_raw_ptr - cstate->input_buf_index);
				cstate->input_buf_index = input_buf_ptr;
				result = true;	/* report EOF */
				break;
			}
			else if (!cstate->opts.csv_mode)
			{
				/*
				 * 到这里说明找到一个反斜杠，后面跟的不是
				 * 句点。非 CSV 模式下反斜杠后的字符皆特    
				 * 殊，因此也要跳过后一个字符。如果不这   
				 * 样做，\\. 便会被当作复制结束，而   
				 * 非 CSV 模式下它是字面反斜杠后跟句点。在
				 * CSV 模式中反斜杠不特殊，故须视作普通 
				 * 字符那样处理其后的字符，所以那些情况
				 * 下我们并不进行                                      
				 * 递增。                            
				 */
				input_buf_ptr++;
			}
		}

		/*
		 * 此标签用于 CSV 场景：\. 出现在行首，但其      
		 * 还有更多文本，这意味着它是一个数据值。对       
		 * CSV 模式中的 \. 我们更严格，因为 \. 可能是数    
		 * 据值；非 CSV 模式中 \. 无法是数据值。     
		 */
not_end_of_copy:
		first_char_in_line = false;
	}							/* end of outer loop */

	/*
	 * 把剩余未复制的数据传入 line_buf。
	 */
	REFILL_LINEBUF;

	return result;
}

/*
 *	返回十六进制数字符的十进制值  
 */
static int
GetDecimalFromHex(char hex)
{
	if (isdigit((unsigned char) hex))
		return hex - '0';
	else
		return tolower((unsigned char) hex) - 'a' + 10;
}

/*
 * 把当前行解析成独立的属性（字段），按需
 * 执行去转义。               
 *
 * 输入位于 line_buf 中。我们用 attribute_buf 保存结果  
 * 字符串。cstate->raw_fields[k] 被设为指向第 k 个           
 * 属性字符串；当输入与 NULL 串匹配时则为 NULL。
 * 此数组会按需扩展。         
 *
 * （注意：调用者无法检查 NULL，因为返回的字符 
 * 串是去转义之后的等价物，看起来可能与某些   
 * 合法数据串相同。）         
 *
 * delim 是列定界符串（目前必须只有一个字节）。      
 * null_print 是 NULL 串。注意：它是与去转义之前         
 * 的输入串比较的。        
 *
 * 返回值是实际读取的字段数。                
 */
static int
CopyReadAttributesText(CopyFromState cstate)
{
	char		delimc = cstate->opts.delim[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * 对于零列的表我们需要一个特例：检查输入行是    
	 * 否为空并返回。     
	 */
	if (cstate->max_fields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * 去转义后的属性必然不会比输入数据行长，所         
	 * 以我们只需强制把 attribute_buf 扩到足够大，然后     
	 * 不做任何空间检查即可传输数据。我们必须这样       
	 * 做：因为在传输中途扩大 attribute_buf 会使已存入        
	 * cstate->raw_fields[] 的指针失效。           
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* 为循环设置指针变量    */
	cur_ptr = cstate->line_buf.data;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* 外层循环遍历各个字段  */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;
		bool		saw_non_ascii = false;

		/* 确保有足够空间容纳下一个值            */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* 在输入与输出两侧都记下字段的起点       */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

		/*
		 * 扫描字段数据  
		 *
		 * 注意：在此循环中，我们既扫描以定位字段末      
		 * 端，又投机性地执行去转义。一旦找到字段末    
		 * 端，就可以把原始字段内容与 NULL 标记串进行      
		 * 比较。只有在该比较失败之后，我们才知道去    
		 * 转义才是真正该做的事；因此，在完成 NULL 标     
		 * 记串检查之前，我们*绝不可*抛出任何语法错   
		 * 误。
		 */
		for (;;)
		{
			char		c;

			end_ptr = cur_ptr;
			if (cur_ptr >= line_end_ptr)
				break;
			c = *cur_ptr++;
			if (c == delimc)
			{
				found_delim = true;
				break;
			}
			if (c == '\\')
			{
				if (cur_ptr >= line_end_ptr)
					break;
				c = *cur_ptr++;
				switch (c)
				{
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
						{
							/* handle \013 */
							int			val;

							val = OCTVALUE(c);
							if (cur_ptr < line_end_ptr)
							{
								c = *cur_ptr;
								if (ISOCTAL(c))
								{
									cur_ptr++;
									val = (val << 3) + OCTVALUE(c);
									if (cur_ptr < line_end_ptr)
									{
										c = *cur_ptr;
										if (ISOCTAL(c))
										{
											cur_ptr++;
											val = (val << 3) + OCTVALUE(c);
										}
									}
								}
							}
							c = val & 0377;
							if (c == '\0' || IS_HIGHBIT_SET(c))
								saw_non_ascii = true;
						}
						break;
					case 'x':
						/* Handle \x3F */
						if (cur_ptr < line_end_ptr)
						{
							char		hexchar = *cur_ptr;

							if (isxdigit((unsigned char) hexchar))
							{
								int			val = GetDecimalFromHex(hexchar);

								cur_ptr++;
								if (cur_ptr < line_end_ptr)
								{
									hexchar = *cur_ptr;
									if (isxdigit((unsigned char) hexchar))
									{
										cur_ptr++;
										val = (val << 4) + GetDecimalFromHex(hexchar);
									}
								}
								c = val & 0xff;
								if (c == '\0' || IS_HIGHBIT_SET(c))
									saw_non_ascii = true;
							}
						}
						break;
					case 'b':
						c = '\b';
						break;
					case 'f':
						c = '\f';
						break;
					case 'n':
						c = '\n';
						break;
					case 'r':
						c = '\r';
						break;
					case 't':
						c = '\t';
						break;
					case 'v':
						c = '\v';
						break;

						/*
						 * 其他情况下，'\' 后的字符原样   
						 * 对待。
						 */
				}
			}

			/* 把 c 加进输出串  */
			*output_ptr++ = c;
		}

		/* 原始输入是否匹配 NULL 标记串     */
		input_len = end_ptr - start_ptr;
		if (input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		/* 检查原始输入是否匹配默认标记串  */
		else if (fieldno < list_length(cstate->attnumlist) &&
				 cstate->opts.default_print &&
				 input_len == cstate->opts.default_print_len &&
				 strncmp(start_ptr, cstate->opts.default_print, input_len) == 0)
		{
			/* fieldno 从 0 起计，attnum 从 1 起计   */
			int			m = list_nth_int(cstate->attnumlist, fieldno) - 1;

			if (cstate->defexprs[m] != NULL)
			{
				/* defaults 包含所有物理属性的条目           */
				cstate->defaults[m] = true;
			}
			else
			{
				TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
				Form_pg_attribute att = TupleDescAttr(tupDesc, m);

				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unexpected default marker in COPY data"),
						 errdetail("Column \"%s\" has no default value.",
								   NameStr(att->attname))));
			}
		}
		else
		{
			/*
			 * 到这里我们已知道该字段应当包含数据。      
			 *
			 * 若去转义过任何非 7 位 ASCII 字符，须确保  
			 * 所得串对该数据库编码是有效数据。   
			 */
			if (saw_non_ascii)
			{
				char	   *fld = cstate->raw_fields[fieldno];

				pg_verifymbstr(fld, output_ptr - fld, false);
			}
		}

		/* 在输出区中终止属性值           */
		*output_ptr++ = '\0';

		fieldno++;
		/* 遇 EOL 而非定界符则完成      */
		if (!found_delim)
			break;
	}

	/* 清理 attribute_buf 的状态  */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}

/*
 * 把当前行解析成独立的属性（字段），按需
 * 执行去转义。其 API 与 CopyReadAttributesText 一致，     
 * 只是我们按照"标准"（即通常的）CSV 用法来      
 * 解析字段。                    
 */
static int
CopyReadAttributesCSV(CopyFromState cstate)
{
	char		delimc = cstate->opts.delim[0];
	char		quotec = cstate->opts.quote[0];
	char		escapec = cstate->opts.escape[0];
	int			fieldno;
	char	   *output_ptr;
	char	   *cur_ptr;
	char	   *line_end_ptr;

	/*
	 * 对于零列的表我们需要一个特例：检查输入行是    
	 * 否为空并返回。     
	 */
	if (cstate->max_fields <= 0)
	{
		if (cstate->line_buf.len != 0)
			ereport(ERROR,
					(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					 errmsg("extra data after last expected column")));
		return 0;
	}

	resetStringInfo(&cstate->attribute_buf);

	/*
	 * 去转义后的属性必然不会比输入数据行长，所         
	 * 以我们只需强制把 attribute_buf 扩到足够大，然后     
	 * 不做任何空间检查即可传输数据。我们必须这样       
	 * 做：因为在传输中途扩大 attribute_buf 会使已存入        
	 * cstate->raw_fields[] 的指针失效。           
	 */
	if (cstate->attribute_buf.maxlen <= cstate->line_buf.len)
		enlargeStringInfo(&cstate->attribute_buf, cstate->line_buf.len);
	output_ptr = cstate->attribute_buf.data;

	/* 为循环设置指针变量    */
	cur_ptr = cstate->line_buf.data;
	line_end_ptr = cstate->line_buf.data + cstate->line_buf.len;

	/* 外层循环遍历各个字段  */
	fieldno = 0;
	for (;;)
	{
		bool		found_delim = false;
		bool		saw_quote = false;
		char	   *start_ptr;
		char	   *end_ptr;
		int			input_len;

		/* 确保有足够空间容纳下一个值            */
		if (fieldno >= cstate->max_fields)
		{
			cstate->max_fields *= 2;
			cstate->raw_fields =
				repalloc(cstate->raw_fields, cstate->max_fields * sizeof(char *));
		}

		/* 在输入与输出两侧都记下字段的起点       */
		start_ptr = cur_ptr;
		cstate->raw_fields[fieldno] = output_ptr;

		/*
		 * 扫描字段数据  
		 *
		 * 循环以"非引号"模式开始，然后在该模式与         
		 * "引号内"模式之间切换。若处于"非引号"模      
		 * 式时见到定界符或行尾则正常退出。
		 */
		for (;;)
		{
			char		c;

			/* Not in quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					goto endfield;
				c = *cur_ptr++;
				/* unquoted field delimiter */
				if (c == delimc)
				{
					found_delim = true;
					goto endfield;
				}
				/* 带引号字段（或其部分）开始  */
				if (c == quotec)
				{
					saw_quote = true;
					break;
				}
				/* 把 c 加进输出串  */
				*output_ptr++ = c;
			}

			/* In quote */
			for (;;)
			{
				end_ptr = cur_ptr;
				if (cur_ptr >= line_end_ptr)
					ereport(ERROR,
							(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
							 errmsg("unterminated CSV quoted field")));

				c = *cur_ptr++;

				/* 带引号字段内的转义  */
				if (c == escapec)
				{
					/*
					 * 可能则预读下一字符，若其为转义字符或 
					 * 引号字符则转义之。      
					 */
					if (cur_ptr < line_end_ptr)
					{
						char		nextc = *cur_ptr;

						if (nextc == escapec || nextc == quotec)
						{
							*output_ptr++ = nextc;
							cur_ptr++;
							continue;
						}
					}
				}

				/*
				 * 带引号字段的结束。须先测试转义再做此  
				 * 测试，以防引号字符与转义字符相同      
				 * （这是常见情况）。
				 */
				if (c == quotec)
					break;

				/* 把 c 加进输出串  */
				*output_ptr++ = c;
			}
		}
endfield:

		/* 在输出区中终止属性值           */
		*output_ptr++ = '\0';

		/* 原始输入是否匹配 NULL 标记串     */
		input_len = end_ptr - start_ptr;
		if (!saw_quote && input_len == cstate->opts.null_print_len &&
			strncmp(start_ptr, cstate->opts.null_print, input_len) == 0)
			cstate->raw_fields[fieldno] = NULL;
		/* 检查原始输入是否匹配默认标记串  */
		else if (fieldno < list_length(cstate->attnumlist) &&
				 cstate->opts.default_print &&
				 input_len == cstate->opts.default_print_len &&
				 strncmp(start_ptr, cstate->opts.default_print, input_len) == 0)
		{
			/* fieldno 自 0 起，attnum 自 1 起     */
			int			m = list_nth_int(cstate->attnumlist, fieldno) - 1;

			if (cstate->defexprs[m] != NULL)
			{
				/* defaults 包含所有物理属性的条目           */
				cstate->defaults[m] = true;
			}
			else
			{
				TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
				Form_pg_attribute att = TupleDescAttr(tupDesc, m);

				ereport(ERROR,
						(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
						 errmsg("unexpected default marker in COPY data"),
						 errdetail("Column \"%s\" has no default value.",
								   NameStr(att->attname))));
			}
		}

		fieldno++;
		/* 遇 EOL 而非定界符则完成      */
		if (!found_delim)
			break;
	}

	/* 清理 attribute_buf 的状态  */
	output_ptr--;
	Assert(*output_ptr == '\0');
	cstate->attribute_buf.len = (output_ptr - cstate->attribute_buf.data);

	return fieldno;
}


/*
 * 读取二进制属性
 */
static Datum
CopyReadBinaryAttribute(CopyFromState cstate, FmgrInfo *flinfo,
						Oid typioparam, int32 typmod,
						bool *isnull)
{
	int32		fld_size;
	Datum		result;

	if (!CopyGetInt32(cstate, &fld_size))
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));
	if (fld_size == -1)
	{
		*isnull = true;
		return ReceiveFunctionCall(flinfo, NULL, typioparam, typmod);
	}
	if (fld_size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("invalid field size")));

	/* 重置 attribute_buf 为空并装入原始数据      */
	resetStringInfo(&cstate->attribute_buf);

	enlargeStringInfo(&cstate->attribute_buf, fld_size);
	if (CopyReadBinaryData(cstate, cstate->attribute_buf.data,
						   fld_size) != fld_size)
		ereport(ERROR,
				(errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				 errmsg("unexpected EOF in COPY data")));

	cstate->attribute_buf.len = fld_size;
	cstate->attribute_buf.data[fld_size] = '\0';

	/* 调用列类型的二进制输入转换函数 */
	result = ReceiveFunctionCall(flinfo, &cstate->attribute_buf,
								 typioparam, typmod);

	/* 若未吃掉整个缓冲区则有麻烦   */
	if (cstate->attribute_buf.cursor != cstate->attribute_buf.len)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("incorrect binary data format")));

	*isnull = false;
	return result;
}
