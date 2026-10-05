/*-------------------------------------------------------------------------
 *
 * printtup.c
 *	  把元组打印到目标的例程（这里同时支持前端
 *	  客户端和独立后端）。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/printtup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printtup.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "tcop/pquery.h"
#include "utils/lsyscache.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


static void printtup_startup(DestReceiver *self, int operation,
							 TupleDesc typeinfo);
static bool printtup(TupleTableSlot *slot, DestReceiver *self);
static void printtup_shutdown(DestReceiver *self);
static void printtup_destroy(DestReceiver *self);

/* ----------------------------------------------------------------
 *		printtup / debugtup 支持
 * ----------------------------------------------------------------
 */

/* ----------------
 *		printtup 目标对象的私有状态
 *
 * 注意：finfo 是 typoutput 或 typsend 的查找信息，取
 * 我们为本列所用的那一个。
 * ----------------
 */
typedef struct
{								/* 每个属性的信息 */
	Oid			typoutput;		/* 该类型文本输出函数的 OID */
	Oid			typsend;		/* 该类型二进制输出函数的 OID */
	bool		typisvarlena;	/* 是否为 varlena（即可被 TOAST）？ */
	int16		format;			/* 本列的格式代码 */
	FmgrInfo	finfo;			/* 输出函数的预计算调用信息 */
} PrinttupAttrInfo;

typedef struct
{
	DestReceiver pub;			/* 公开的函数指针 */
	Portal		portal;			/* 我们正在从中打印的 Portal */
	bool		sendDescrip;	/* 启动时发送 RowDescription？ */
	TupleDesc	attrinfo;		/* 我们已为之建好的属性信息 */
	int			nattrs;
	PrinttupAttrInfo *myinfo;	/* 每个属性的缓存信息 */
	StringInfoData buf;			/* 输出缓冲区（不在 tmpcontext 中） */
	MemoryContext tmpcontext;	/* 每行工作区的内存上下文 */
} DR_printtup;

/* ----------------
 *		初始化：为 printtup 创建一个 DestReceiver
 * ----------------
 */
DestReceiver *
printtup_create_DR(CommandDest dest)
{
	DR_printtup *self = (DR_printtup *) palloc0(sizeof(DR_printtup));

	self->pub.receiveSlot = printtup;	/* 以后可能会改 */
	self->pub.rStartup = printtup_startup;
	self->pub.rShutdown = printtup_shutdown;
	self->pub.rDestroy = printtup_destroy;
	self->pub.mydest = dest;

	/*
	 * 若是 DestRemote 就自动发送 T 消息，若是
	 * DestRemoteExecute 则不发送
	 */
	self->sendDescrip = (dest == DestRemote);

	self->attrinfo = NULL;
	self->nattrs = 0;
	self->myinfo = NULL;
	self->buf.data = NULL;
	self->tmpcontext = NULL;

	return (DestReceiver *) self;
}

/*
 * 为 DestRemote（或 DestRemoteExecute）接收器设置参数
 */
void
SetRemoteDestReceiverParams(DestReceiver *self, Portal portal)
{
	DR_printtup *myState = (DR_printtup *) self;

	Assert(myState->pub.mydest == DestRemote ||
		   myState->pub.mydest == DestRemoteExecute);

	myState->portal = portal;
}

static void
printtup_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_printtup *myState = (DR_printtup *) self;
	Portal		portal = myState->portal;

	/*
	 * 创建供全部消息使用的 I/O 缓冲区。它无法放在
	 * tmpcontext 里，因为我们要跨行复用它。
	 */
	initStringInfo(&myState->buf);

	/*
	 * 创建一个临时内存上下文，每行重置一次，以
	 * 回收 palloc 分配的内存。这样可以避免数据类型输出例程
	 * 内部泄漏的问题，而且应当比逐个 pfree
	 * 更快。
	 */
	myState->tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
												"printtup",
												ALLOCSET_DEFAULT_SIZES);

	/*
	 * 若应当发出行描述，就发送这些元组的
	 * 元组描述符。
	 */
	if (myState->sendDescrip)
		SendRowDescriptionMessage(&myState->buf,
								  typeinfo,
								  FetchPortalTargetList(portal),
								  portal->formats);

	/* ----------------
	 * 此时本可以建立派生的属性信息，但我们推迟到
	 * printtup 的第一次调用，原因有二：
	 * 1. 若根本没有元组要输出，就不会浪费时间
	 *	  （与旧做法相比）。
	 * 2. 在 printtup 中检查，使我们能处理元组中途
	 *	  改变类型的情形（尽管在当前执行器里
	 *	  这大概不会发生）。
	 * ----------------
	 */
}

/*
 * SendRowDescriptionMessage --- 向前端发送一条 RowDescription 消息
 *
 * 说明：TupleDesc 通常由 ExecTypeFromTL()
 * 或某个类似函数制造；它并不包含全套属性。
 * 执行没有计划的实用函数时，目标列表为 NIL。
 * 若目标列表不是 NIL，则它是 Query 结点的目标列表；
 * 忽略其中的 resjunk 列是我们的责任。formats[] 数组指针
 * 可能为 NULL（若正在对预备语句做 Describe）；
 * 那种情况下，格式代码发送零。
 */
void
SendRowDescriptionMessage(StringInfo buf, TupleDesc typeinfo,
						  List *targetlist, int16 *formats)
{
	int			natts = typeinfo->natts;
	int			i;
	ListCell   *tlist_item = list_head(targetlist);

	/* 元组描述符消息类型 */
	pq_beginmessage_reuse(buf, 'T');
	/* 元组中的属性数 */
	pq_sendint16(buf, natts);

	/*
	 * 为即将发送的整条消息预分配内存。这样就可以使用
	 * 快得多的内联 pqformat.h 函数，并避免
	 * 重新分配。
	 *
	 * 必须高估列名的大小，以计入
	 * 字符集开销。
	 */
	enlargeStringInfo(buf, (NAMEDATALEN * MAX_CONVERSION_GROWTH /* 属性名 */
							+ sizeof(Oid)	/* 来源表 */
							+ sizeof(AttrNumber)	/* 来源列 */
							+ sizeof(Oid)	/* 类型 OID */
							+ sizeof(int16) /* 类型长度 */
							+ sizeof(int32) /* 类型修饰 */
							+ sizeof(int16) /* 格式 */
							) * natts);

	for (i = 0; i < natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(typeinfo, i);
		Oid			atttypid = att->atttypid;
		int32		atttypmod = att->atttypmod;
		Oid			resorigtbl;
		AttrNumber	resorigcol;
		int16		format;

		/*
		 * 若列是域，则改为发送基类型和 typmod。
		 * 在发送任何整数之前先查找，以提高效率。
		 */
		atttypid = getBaseTypeAndTypmod(atttypid, &atttypmod);

		/* 是否有非 resjunk 的目标列表项？ */
		while (tlist_item &&
			   ((TargetEntry *) lfirst(tlist_item))->resjunk)
			tlist_item = lnext(targetlist, tlist_item);
		if (tlist_item)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(tlist_item);

			resorigtbl = tle->resorigtbl;
			resorigcol = tle->resorigcol;
			tlist_item = lnext(targetlist, tlist_item);
		}
		else
		{
			/* 没有可用信息，因此发送零 */
			resorigtbl = 0;
			resorigcol = 0;
		}

		if (formats)
			format = formats[i];
		else
			format = 0;

		pq_writestring(buf, NameStr(att->attname));
		pq_writeint32(buf, resorigtbl);
		pq_writeint16(buf, resorigcol);
		pq_writeint32(buf, atttypid);
		pq_writeint16(buf, att->attlen);
		pq_writeint32(buf, atttypmod);
		pq_writeint16(buf, format);
	}

	pq_endmessage_reuse(buf);
}

/*
 * 取得 printtup() 所需的查找信息
 */
static void
printtup_prepare_info(DR_printtup *myState, TupleDesc typeinfo, int numAttrs)
{
	int16	   *formats = myState->portal->formats;
	int			i;

	/* 丢掉任何旧数据 */
	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = typeinfo;
	myState->nattrs = numAttrs;
	if (numAttrs <= 0)
		return;

	myState->myinfo = (PrinttupAttrInfo *)
		palloc0(numAttrs * sizeof(PrinttupAttrInfo));

	for (i = 0; i < numAttrs; i++)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		int16		format = (formats ? formats[i] : 0);
		Form_pg_attribute attr = TupleDescAttr(typeinfo, i);

		thisState->format = format;
		if (format == 0)
		{
			getTypeOutputInfo(attr->atttypid,
							  &thisState->typoutput,
							  &thisState->typisvarlena);
			fmgr_info(thisState->typoutput, &thisState->finfo);
		}
		else if (format == 1)
		{
			getTypeBinaryOutputInfo(attr->atttypid,
									&thisState->typsend,
									&thisState->typisvarlena);
			fmgr_info(thisState->typsend, &thisState->finfo);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unsupported format code: %d", format)));
	}
}

/* ----------------
 *		printtup --- 向客户端发送一个元组
 * ----------------
 */
static bool
printtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	DR_printtup *myState = (DR_printtup *) self;
	MemoryContext oldcontext;
	StringInfo	buf = &myState->buf;
	int			natts = typeinfo->natts;
	int			i;

	/* 若需要，设置或更新派生的属性信息 */
	if (myState->attrinfo != typeinfo || myState->nattrs != natts)
		printtup_prepare_info(myState, typeinfo, natts);

	/* 确保元组已被完全拆开 */
	slot_getallattrs(slot);

	/* 切换到每行上下文，以便下面回收内存 */
	oldcontext = MemoryContextSwitchTo(myState->tmpcontext);

	/*
	 * 准备一条 DataRow 消息（注意缓冲区在每行上下文中）
	 */
	pq_beginmessage_reuse(buf, 'D');

	pq_sendint16(buf, natts);

	/*
	 * 发送这个元组的各个属性
	 */
	for (i = 0; i < natts; ++i)
	{
		PrinttupAttrInfo *thisState = myState->myinfo + i;
		Datum		attr = slot->tts_values[i];

		if (slot->tts_isnull[i])
		{
			pq_sendint32(buf, -1);
			continue;
		}

		/*
		 * 这里抓住那些返回给客户端、却未落盘的 Datum 中的
		 * 未定义字节；见 PageAddItem() 里相关检查处的注释。
		 * 这项测试对未压缩、非外部的 Datum 最有用，
		 * 但测试新的 C 函数时，这里很可能会
		 * 见到这样的 Datum。
		 */
		if (thisState->typisvarlena)
			VALGRIND_CHECK_MEM_IS_DEFINED(DatumGetPointer(attr),
										  VARSIZE_ANY(attr));

		if (thisState->format == 0)
		{
			/* 文本输出 */
			char	   *outputstr;

			outputstr = OutputFunctionCall(&thisState->finfo, attr);
			pq_sendcountedtext(buf, outputstr, strlen(outputstr), false);
		}
		else
		{
			/* 二进制输出 */
			bytea	   *outputbytes;

			outputbytes = SendFunctionCall(&thisState->finfo, attr);
			pq_sendint32(buf, VARSIZE(outputbytes) - VARHDRSZ);
			pq_sendbytes(buf, VARDATA(outputbytes),
						 VARSIZE(outputbytes) - VARHDRSZ);
		}
	}

	pq_endmessage_reuse(buf);

	/* 返回调用方的上下文，并清掉该行的临时内存 */
	MemoryContextSwitchTo(oldcontext);
	MemoryContextReset(myState->tmpcontext);

	return true;
}

/* ----------------
 *		printtup_shutdown - 关闭
 * ----------------
 */
static void
printtup_shutdown(DestReceiver *self)
{
	DR_printtup *myState = (DR_printtup *) self;

	if (myState->myinfo)
		pfree(myState->myinfo);
	myState->myinfo = NULL;

	myState->attrinfo = NULL;

	if (myState->buf.data)
		pfree(myState->buf.data);
	myState->buf.data = NULL;

	if (myState->tmpcontext)
		MemoryContextDelete(myState->tmpcontext);
	myState->tmpcontext = NULL;
}

/* ----------------
 *		printtup_destroy - 销毁
 * ----------------
 */
static void
printtup_destroy(DestReceiver *self)
{
	pfree(self);
}

/* ----------------
 *		printatt - 打印一个属性
 * ----------------
 */
static void
printatt(unsigned attributeId,
		 Form_pg_attribute attributeP,
		 char *value)
{
	printf("\t%2d: %s%s%s%s\t(typeid = %u, len = %d, typmod = %d, byval = %c)\n",
		   attributeId,
		   NameStr(attributeP->attname),
		   value != NULL ? " = \"" : "",
		   value != NULL ? value : "",
		   value != NULL ? "\"" : "",
		   (unsigned int) (attributeP->atttypid),
		   attributeP->attlen,
		   attributeP->atttypmod,
		   attributeP->attbyval ? 't' : 'f');
}

/* ----------------
 *		debugStartup - 准备为交互式后端打印元组
 * ----------------
 */
void
debugStartup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	int			natts = typeinfo->natts;
	int			i;

	/*
	 * 显示这些元组的返回类型
	 */
	for (i = 0; i < natts; ++i)
		printatt((unsigned) i + 1, TupleDescAttr(typeinfo, i), NULL);
	printf("\t----\n");
}

/* ----------------
 *		debugtup - 为交互式后端打印一个元组
 * ----------------
 */
bool
debugtup(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	typeinfo = slot->tts_tupleDescriptor;
	int			natts = typeinfo->natts;
	int			i;
	Datum		attr;
	char	   *value;
	bool		isnull;
	Oid			typoutput;
	bool		typisvarlena;

	for (i = 0; i < natts; ++i)
	{
		attr = slot_getattr(slot, i + 1, &isnull);
		if (isnull)
			continue;
		getTypeOutputInfo(TupleDescAttr(typeinfo, i)->atttypid,
						  &typoutput, &typisvarlena);

		value = OidOutputFunctionCall(typoutput, attr);

		printatt((unsigned) i + 1, TupleDescAttr(typeinfo, i), value);
	}
	printf("\t----\n");

	return true;
}
