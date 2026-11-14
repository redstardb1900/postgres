/*-------------------------------------------------------------------------
 *
 * tuplestore.c
 *	  通用临时元组存储例程。
 *
 * 本模块负责元组的临时存储，服务对象包括
 * Materialize 节点、哈希连接批文件等。它实质上
 * 是 tuplesort.c 的简化版本；它不对元组做排序，
 * 只能存储并回吐元组序列。不过，
 * 因为不需要排序，所以允许在序列尚未全部写完
 * 之前就开始读取。这对游标特别有用，
 * 因为它允许在查询中已扫描部分内随机访问，
 * 而无需将底层扫描处理完毕。
 * 此外，它还可以支持多个独立的读指针。
 *
 * 若数据超出调用者指定的空间限制，便使用
 * 一个临时文件来处理。
 *
 * 元组存储可用内存的（近似）数量由调用者以
 * 千字节为单位指定。我们吸纳元组并简单地存入
 * 内存数组，只要尚未超过 maxKBytes。如果确实超过，
 * 就把所有元组倾倒进一个临时文件，之后需要时
 * 再读取。
 *
 * 创建时，元组存储支持一个读指针，编号为 0。
 * 可以使用 tuplestore_alloc_read_pointer 创建更多读指针。
 * 通过复制读指针来支持标记/恢复行为。
 *
 * 当调用者请求反向扫描能力时，我们会以允许
 * 正向或反向扫描的格式写入临时文件。否则只允许
 * 正向扫描。反向扫描的请求必须在向元组存储放
 * 入任何元组之前提出。回卷通常是允许的，但
 * 可以通过 tuplestore_set_eflags 关闭；为所有读指针
 * 关闭回卷即可在最旧的读取点处截断元组存储，
 * 从而将内存用量降到最低。（调用者必须显式调用
 * tuplestore_trim，截断才会真正发生。）
 *
 * 注：在 TSS_WRITEFILE 状态下，临时文件的寻道位置
 * 就是当前写入位置，而元组存储中的写位置变量
 * 并不保持最新。类似地，在 TSS_READFILE 状态下临时
 * 文件的寻道位置就是活动读指针的位置，该读指针
 * 也不保持最新。我们会使用 ftell() 更新相应的变量，
 * 然后再切换到另一状态或激活另一读指针。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplestore.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "utils/memutils.h"
#include "utils/resowner.h"


/*
 * Tuplestore 对象可能的状态。这些状态表示在
 * 各 Tuplestore 调用间持续的状态。
 */
typedef enum
{
	TSS_INMEM,					/* 元组仍能放入内存 */
	TSS_WRITEFILE,				/* 写入临时文件 */
	TSS_READFILE				/* 从临时文件读取 */
} TupStoreStatus;

/*
 * 单个读指针的状态。如果处于 INMEM 状态，则所有
 * 读指针的 "current" 字段都表示读取位置。在
 * WRITEFILE 状态下，file/offset 字段表示读取位置。在
 * READFILE 状态下，非活动读指针拥有有效的
 * file/offset，而活动读指针的位置隐含等于临时文件
 *
 * 特殊情况：若 eof_reached 为真，则该指针的读取位置
 * 隐含等于写入位置，而 current/file/offset 不再
 * 维护。这样我们就不必在每次写入时更新所有读
 * 指针。
 */
typedef struct
{
	int			eflags;			/* 能力标志 */
	bool		eof_reached;	/* 读取已到 EOF */
	int			current;		/* 下个待读下标 */
	int			file;			/* 文件号 */
	off_t		offset;			/* 文件内偏移 */
} TSReadPointer;

/*
 * Tuplestore 操作的私有状态。
 */
struct Tuplestorestate
{
	TupStoreStatus status;		/* 如上所示的枚举值 */
	int			eflags;			/* 能力标志（各指针标志的 OR） */
	bool		backward;		/* 存额外长度字？ */
	bool		interXact;		/* 跨事务保持打开？ */
	bool		truncated;		/* tuplestore_trim 已删元组？ */
	int64		availMem;		/* 剩余可用内存，以字节计 */
	int64		allowedMem;		/* 总允许内存（字节） */
	int64		tuples;			/* 已添加的元组数 */
	BufFile    *myfile;			/* 底层文件，无则为 NULL */
	MemoryContext context;		/* 持有元组的内存上下文 */
	ResourceOwner resowner;		/* 临时文件的 resowner */

	/*
	 * 这些函数指针将必须知道所处理元组种类的例程
	 * 与无需知道这一点的例程解耦。
	 * 它们由 tuplestore_begin_xxx 例程设置。
	 *
	 * （虽然 tuplestore.c 目前只支持堆元组，但我复制了
	 * tuplesort.c 的这一部分，以便将来需要时能轻松
	 * 扩展到其他对象。）
	 *
	 * 把输入元组复制到 palloc 空间的函数。（注：我们
	 * 假定之后单个 pfree() 就足以释放该元组，因此
	 * 表示必须"平坦"地处于单个 palloc 块。）state->availMem
	 * 必须减去所用的空间数量。
	 */
	void	   *(*copytup) (Tuplestorestate *state, void *tup);

	/*
	 * 把已存储元组写到磁带上的函数。元组在磁带上
	 * 的表示形式不必与在内存中相同；对磁带表示形式
	 * 的要求见下文。写入元组之后，
	 * 将其 pfree()，并把 state->availMem 增加由此释放的
	 * 内存空间。
	 */
	void		(*writetup) (Tuplestorestate *state, void *tup);

	/*
	 * 从磁带把已存储元组读回内存的函数。'len' 是
	 * 已读取的已存储元组长度。创建并返回一个
	 * palloc 副本，并把 state->availMem 减去所消耗的
	 * 内存空间。
	 */
	void	   *(*readtup) (Tuplestorestate *state, unsigned int len);

	/*
	 * 如果处于 INMEM 状态，此数组保存指向内存中元组
	 * 的指针。WRITEFILE 与 READFILE 态不用。
	 *
	 * 当 memtupdeleted > 0 时，前 memtupdeleted 个指针已因
	 * tuplestore_trim() 操作而释放，但我们尚未花费
	 * 精力把剩余的指针下滑。这些未使用的指针
	 * 都被设为 NULL 以捕获任何无效访问。注意 memtupcount
	 * 包含已删除的指针。
	 */
	void	  **memtuples;		/* 指向 palloc 元组的指针数组 */
	int			memtupdeleted;	/* 前 N 个槽位当前未被使用 */
	int			memtupcount;	/* 当前存在的元组数 */
	int			memtupsize;		/* memtuples 数组的已分配长度 */
	bool		growmemtuples;	/* memtuples 增长仍在进行？ */

	/*
	 * 这些变量用于跟踪当前位置。
	 *
	 * 在 WRITEFILE 状态下，当前文件寻道位置就是写入点；
	 * 在 READFILE 状态下，写入位置被记录在 writepos_xxx。
	 * （写入位置与 EOF 相同，但由于 BufFileSeek 目前
	 * 未实现 SEEK_END，所以必须显式记住它。）
	 */
	TSReadPointer *readptrs;	/* 读指针的数组 */
	int			activeptr;		/* 活动读指针的下标 */
	int			readptrcount;	/* 当前有效的指针数量 */
	int			readptrsize;	/* readptrs 数组的已分配长度 */

	int			writepos_file;	/* 文件号（READFILE 有效） */
	off_t		writepos_offset;	/* 偏移量（READFILE 有效） */
};

#define COPYTUP(state,tup)	((*(state)->copytup) (state, tup))
#define WRITETUP(state,tup) ((*(state)->writetup) (state, tup))
#define READTUP(state,len)	((*(state)->readtup) (state, len))
#define LACKMEM(state)		((state)->availMem < 0)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))

/*--------------------
 *
 * NOTES：元组的磁带表示形式
 *
 * 已存储元组的首个 "unsigned int" 必须是整
 * 个元组磁带总大小（含自身，永不为零）。
 * 已存储元组的其余部分
 * 可能与该元组在内存中的表示形式一致，也可能
 * 不一致；所需转换由 writetup 与 readtup 例程完成。
 *
 * 若 state->backward 为真，则已存储表示之后必须
 * 再跟一个 "unsigned int"，它是长度的副本——
 * 因此磁带总空间实际比存储的长度值多
 * 出 sizeof(unsigned int)，这使向后读取成为可能。当
 * state->backward 未设置时，写入/读取例程可以省略
 * 长度字。
 *
 * writetup 应当写入两个长度字以及元组
 * 数据。当调用 readtup 时，磁带恰好定位在
 * 前长度字；readtup 必须读取元组数据并越过
 * 后长度字（如存在）。
 *
 * 写入/读取例程可以在必要时利用
 * 存储在 Tuplestorestate 结构中的元组描述数据。
 * 它们还应调整 state->availMem，幅度为所释放或
 * 消耗的内存空间量（而非磁带空间！）。writetup
 * 或 readtup 都应在失败时 ereport()。
 *
 *
 * 关于内存消耗计算的说明：
 *
 * 我们把为元组分配的空间计入 maxKBytes 限制，
 * 外加可变大小数组 memtuples 所用的空间。
 * 固定大小空间（主要是 BufFile I/O 缓冲区）不计入。
 * 我们也不担心读指针数组的大小。
 *
 * 注意我们统计的是实际使用的空间（如
 * GetMemoryChunkSpace 所示）而非最初请求的大小。
 * 这一点很重要，因为 palloc 会增加可观的开销。
 * 由于我们不计入 palloc 分配块中浪费的空间，这
 * 仍比 7.3 之前的做法好得多。
 *
 *--------------------
 */


static Tuplestorestate *tuplestore_begin_common(int eflags,
												bool interXact,
												int maxKBytes);
static void tuplestore_puttuple_common(Tuplestorestate *state, void *tuple);
static void dumptuples(Tuplestorestate *state);
static unsigned int getlen(Tuplestorestate *state, bool eofOK);
static void *copytup_heap(Tuplestorestate *state, void *tup);
static void writetup_heap(Tuplestorestate *state, void *tup);
static void *readtup_heap(Tuplestorestate *state, unsigned int len);


/*
 *		tuplestore_begin_xxx
 *
 * 为元组存储操作做初始化。
 */
static Tuplestorestate *
tuplestore_begin_common(int eflags, bool interXact, int maxKBytes)
{
	Tuplestorestate *state;

	state = (Tuplestorestate *) palloc0(sizeof(Tuplestorestate));

	state->status = TSS_INMEM;
	state->eflags = eflags;
	state->interXact = interXact;
	state->truncated = false;
	state->allowedMem = maxKBytes * 1024L;
	state->availMem = state->allowedMem;
	state->myfile = NULL;
	state->context = CurrentMemoryContext;
	state->resowner = CurrentResourceOwner;

	state->memtupdeleted = 0;
	state->memtupcount = 0;
	state->tuples = 0;

	/*
	 * 数组的初始大小必须大于 ALLOCSET_SEPARATE_THRESHOLD；
	 * 见 grow_memtuples() 的注释。
	 */
	state->memtupsize = Max(16384 / sizeof(void *),
							ALLOCSET_SEPARATE_THRESHOLD / sizeof(void *) + 1);

	state->growmemtuples = true;
	state->memtuples = (void **) palloc(state->memtupsize * sizeof(void *));

	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	state->activeptr = 0;
	state->readptrcount = 1;
	state->readptrsize = 8;		/* 任意 */
	state->readptrs = (TSReadPointer *)
		palloc(state->readptrsize * sizeof(TSReadPointer));

	state->readptrs[0].eflags = eflags;
	state->readptrs[0].eof_reached = false;
	state->readptrs[0].current = 0;

	return state;
}

/*
 * tuplestore_begin_heap
 *
 * 创建新的元组存储；其他类型的元组存储（除了
 * "堆"元组存储，用于堆元组）是可能的，但目前
 * 未实现。
 *
 * randomAccess：若为真，允许对元组存储进行正向和
 * 反向两种访问。
 *
 * interXact：若为真，用于磁盘存储的文件会在当前
 * 事务结束之后继续存在。注意：调用者有责任在
 * 同样能跨越事务边界的内存上下文和资源所有者
 * 中创建这样的元组存储，并确保元组存储在不再
 * 需要时被关闭。
 *
 * maxKBytes：内存中存储多少数据（超出此量的数据
 * 将被换出到磁盘）。不确定时用 work_mem。
 */
Tuplestorestate *
tuplestore_begin_heap(bool randomAccess, bool interXact, int maxKBytes)
{
	Tuplestorestate *state;
	int			eflags;

	/*
	 * 这种对 randomAccess 含义的解读与元组存储在 8.3
	 * 之前的行为兼容。
	 */
	eflags = randomAccess ?
		(EXEC_FLAG_BACKWARD | EXEC_FLAG_REWIND) :
		(EXEC_FLAG_REWIND);

	state = tuplestore_begin_common(eflags, interXact, maxKBytes);

	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;

	return state;
}

/*
 * tuplestore_set_eflags
 *
 * 以比 tuplestore_begin_xxx 所允许的更细粒度设置读
 * 指针 0 的能力标志。必须在向元组存储插入任何
 * 数据之前调用它。
 *
 * eflags 是一个位掩码，含义遵循执行器节点启动
 * 标志的用法（见 executor.h）。元组存储关注以下各位：
 *		EXEC_FLAG_REWIND	需回卷到开头
 *		EXEC_FLAG_BACKWARD	需反向抓取
 * 若未调用 tuplestore_set_eflags，则允许 REWIND，而
 * BACKWARD 按 tuplestore_begin_xxx "randomAccess" 设定
 *
 * 注：设置 BACKWARD 而不设 REWIND 意味着指针可以
 * 向后读取，但不会超过截断点（上次调用
 * tuplestore_trim 时最靠后读指针的位置）。
 */
void
tuplestore_set_eflags(Tuplestorestate *state, int eflags)
{
	int			i;

	if (state->status != TSS_INMEM || state->memtupcount != 0)
		elog(ERROR, "too late to call tuplestore_set_eflags");

	state->readptrs[0].eflags = eflags;
	for (i = 1; i < state->readptrcount; i++)
		eflags |= state->readptrs[i].eflags;
	state->eflags = eflags;
}

/*
 * tuplestore_alloc_read_pointer - 分配另一个读指针。
 *
 * 返回指针的下标。
 *
 * 新指针最初复制读指针 0 的位置。
 * 它可以有自己的 eflags，但如果已有数据被插入
 * 元组存储，这些 eflags 就不得表示需求的
 * 增加。
 */
int
tuplestore_alloc_read_pointer(Tuplestorestate *state, int eflags)
{
	/* 检查是否可能增加需求 */
	if (state->status != TSS_INMEM || state->memtupcount != 0)
	{
		if ((state->eflags | eflags) != state->eflags)
			elog(ERROR, "too late to require new tuplestore eflags");
	}

	/* 需要时为新读指针腾出空间 */
	if (state->readptrcount >= state->readptrsize)
	{
		int			newcnt = state->readptrsize * 2;

		state->readptrs = (TSReadPointer *)
			repalloc(state->readptrs, newcnt * sizeof(TSReadPointer));
		state->readptrsize = newcnt;
	}

	/* 并设置 */
	state->readptrs[state->readptrcount] = state->readptrs[0];
	state->readptrs[state->readptrcount].eflags = eflags;

	state->eflags |= eflags;

	return state->readptrcount++;
}

/*
 * tuplestore_clear
 *
 *	删除元组存储的全部内容，并将它的读指针复位
 *	到起点。
 */
void
tuplestore_clear(Tuplestorestate *state)
{
	int			i;
	TSReadPointer *readptr;

	if (state->myfile)
		BufFileClose(state->myfile);
	state->myfile = NULL;
	if (state->memtuples)
	{
		for (i = state->memtupdeleted; i < state->memtupcount; i++)
		{
			FREEMEM(state, GetMemoryChunkSpace(state->memtuples[i]));
			pfree(state->memtuples[i]);
		}
	}
	state->status = TSS_INMEM;
	state->truncated = false;
	state->memtupdeleted = 0;
	state->memtupcount = 0;
	state->tuples = 0;
	readptr = state->readptrs;
	for (i = 0; i < state->readptrcount; readptr++, i++)
	{
		readptr->eof_reached = false;
		readptr->current = 0;
	}
}

/*
 * tuplestore_end
 *
 *	释放资源并清理。
 */
void
tuplestore_end(Tuplestorestate *state)
{
	int			i;

	if (state->myfile)
		BufFileClose(state->myfile);
	if (state->memtuples)
	{
		for (i = state->memtupdeleted; i < state->memtupcount; i++)
			pfree(state->memtuples[i]);
		pfree(state->memtuples);
	}
	pfree(state->readptrs);
	pfree(state);
}

/*
 * tuplestore_select_read_pointer - 激活指定的读指针
 */
void
tuplestore_select_read_pointer(Tuplestorestate *state, int ptr)
{
	TSReadPointer *readptr;
	TSReadPointer *oldptr;

	Assert(ptr >= 0 && ptr < state->readptrcount);

	/* 已活动则免 */
	if (ptr == state->activeptr)
		return;

	readptr = &state->readptrs[ptr];
	oldptr = &state->readptrs[state->activeptr];

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* 无事 */
			break;
		case TSS_READFILE:

			/*
			 * 首先，把当前读取位置保存进即将不活动的
			 * 指针。
			 */
			if (!oldptr->eof_reached)
				BufFileTell(state->myfile,
							&oldptr->file,
							&oldptr->offset);

			/*
			 * 必须使临时文件的寻道位置等于新读指针的
			 * 逻辑位置。eof_reached 状态下即为 EOF，
			 * 它得自已保存的
			 * 写入位置。
			 */
			if (readptr->eof_reached)
			{
				if (BufFileSeek(state->myfile,
								state->writepos_file,
								state->writepos_offset,
								SEEK_SET) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not seek in tuplestore temporary file")));
			}
			else
			{
				if (BufFileSeek(state->myfile,
								readptr->file,
								readptr->offset,
								SEEK_SET) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not seek in tuplestore temporary file")));
			}
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}

	state->activeptr = ptr;
}

/*
 * tuplestore_tuple_count
 *
 * 返回自创建或上次 tuplestore_clear() 以来添加
 * 的元组数。
 */
int64
tuplestore_tuple_count(Tuplestorestate *state)
{
	return state->tuples;
}

/*
 * tuplestore_ateof
 *
 * 返回活动读指针的 eof_reached 状态。
 */
bool
tuplestore_ateof(Tuplestorestate *state)
{
	return state->readptrs[state->activeptr].eof_reached;
}

/*
 * 在内存约束允许时增长 memtuples[] 数组。我们
 * 不得超过内存中 INT_MAX 个元组或调用者给定的
 * 内存上限。若能扩大数组则返回真，否则返回假。
 *
 * 通常，每次增长时我们都把数组大小翻倍。当
 * 这样做会超出限制时，我们尝试最后一次较小
 * 的增长（然后清除 growmemtuples 标志以不再
 * 尝试）。这使我们能用尽允许的全部内存；固守
 * 纯粹翻倍规则可能导致几乎一半被浪费。由于
 * availMem 随元组的增删而变动，我们需要某条
 * 规则来防止反复小幅增加 memtupsize，那只会
 * 是无谓的抖动。growmemtuples 标志可实现这点，
 * 也防止函数内重算。
 */
static bool
grow_memtuples(Tuplestorestate *state)
{
	int			newmemtupsize;
	int			memtupsize = state->memtupsize;
	int64		memNowUsed = state->allowedMem - state->availMem;

	/* 若 memtuples 已达上限则放弃，见上文注释 */
	if (!state->growmemtuples)
		return false;

	/* 选择 memtupsize 的新值 */
	if (memNowUsed <= state->availMem)
	{
		/*
		 * 已用量不超过 allowedMem 的一半；将用量翻倍，
		 * 以 INT_MAX 封顶。
		 */
		if (memtupsize < INT_MAX / 2)
			newmemtupsize = memtupsize * 2;
		else
		{
			newmemtupsize = INT_MAX;
			state->growmemtuples = false;
		}
	}
	else
	{
		/*
		 * 这将是 memtupsize 的最后一次增长。放弃翻倍
		 * 策略，改为在安全范围内尽量增加。
		 *
		 * 为了不超出 allowedMem，memtupsize 的增量不得
		 * 超过 availMem / sizeof(void *) 个元素。实际上我们
		 * 希望它的增量远小于此，因为需要给新槽位
		 * 将要引用的元组留出空间。我们
		 * 假定新元组与已见过的元组大小大致相同，
		 * 从而可以从迄今的
		 * 空间消耗推算 memtuples 数组的合适新大小。
		 * 最优值可能高于或低于该估计，
		 * 但很难事先得知。我们再次以
		 * INT_MAX 元组封顶。
		 *
		 * 这一计算可以确保不会把数组扩大到令 LACKMEM
		 * 成真的地步，因为当前使用的内存已包括现
		 * 有数组；因此即使当前没有使用任何其他内存，
		 * 也总有足够的 allowedMem 供新数组元素使用。
		 *
		 * 我们用 float8 做算术，否则 memtupsize 与
		 * allowedMem 的乘积可能溢出。结果中的任何
		 * 不准确都应当无关紧要；但即使我们算出了
		 * 完全离谱的结果，下面的检查也会防止任何
		 * 糟糕后果发生。
		 */
		double		grow_ratio;

		grow_ratio = (double) state->allowedMem / (double) memNowUsed;
		if (memtupsize * grow_ratio < INT_MAX)
			newmemtupsize = (int) (memtupsize * grow_ratio);
		else
			newmemtupsize = INT_MAX;

		/* 不再做进一步扩大的尝试 */
		state->growmemtuples = false;
	}

	/* 必须至少扩大一个元素，否则报告失败 */
	if (newmemtupsize <= memtupsize)
		goto noalloc;

	/*
	 * 在 32 位机器上，allowedMem 可能超过 MaxAllocHugeSize，
	 * 需加以钳制以确保请求不被拒绝。注意我们很
	 * 容易在遭遇这种结果之前耗尽地址空间。（这目前
	 * 不可能发生，因为 guc.c 的 MAX_KILOBYTES 对
	 * work_mem 限制，别依赖它。）
	 */
	if ((Size) newmemtupsize >= MaxAllocHugeSize / sizeof(void *))
	{
		newmemtupsize = (int) (MaxAllocHugeSize / sizeof(void *));
		state->growmemtuples = false;	/* 无法再增长 */
	}

	/*
	 * 我们必须确保不会使 LACKMEM 变成真，否则空间
	 * 管理算法将会失常。上面的代码绝不应生成危
	 * 险的请求，但为了安全，还是要显式检查数组
	 * 增长是否处于 availMem 之内。（如果与 memtuples
	 * 数组关联的内存块开销增加，仍可能导致
	 * LACKMEM。这不该发生，因为我们选择的初始数组
	 * 大小足以确保 palloc 把新旧数组都当作独立块
	 * 对待。但我们还是会在下方显式检查 LACKMEM，
	 * 以防万一。）
	 */
	if (state->availMem < (int64) ((newmemtupsize - memtupsize) * sizeof(void *)))
		goto noalloc;

	/* 就做 */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	state->memtupsize = newmemtupsize;
	state->memtuples = (void **)
		repalloc_huge(state->memtuples,
					  state->memtupsize * sizeof(void *));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));
	if (LACKMEM(state))
		elog(ERROR, "unexpected out-of-memory situation in tuplestore");
	return true;

noalloc:
	/* 若因任何原因未 realloc，则关闭后续尝试 */
	state->growmemtuples = false;
	return false;
}

/*
 * 接受一个元组，追加到元组存储。
 *
 * 注意输入元组总是被复制；调用者无需保存它。
 *
 * 如果活动读指针当前"位于 EOF"，它将保持不变（读
 * 指针隐含地随写指针一起前进）；否则读指针
 * 保持不变。非活动读指针不移动，这意味着它们
 * 在 puttuple 之后必然不"位于 EOF"。
 * 这一看似奇怪的行为是为了方便 nodeMaterial.c 和
 * nodeCtescan.c，否则它们将需要额外的指针
 * 定位
 *
 * tuplestore_puttupleslot() 是一个从
 * TupleTableSlot 收集数据的便利例程。
 */
void
tuplestore_puttupleslot(Tuplestorestate *state,
						TupleTableSlot *slot)
{
	MinimalTuple tuple;
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	/*
	 * 在工作内存中构成 MinimalTuple
	 */
	tuple = ExecCopySlotMinimalTuple(slot);
	USEMEM(state, GetMemoryChunkSpace(tuple));

	tuplestore_puttuple_common(state, (void *) tuple);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 从 HeapTuple 复制的"标准"情形。这实际如今
 * 已有些弃用了，但鉴于调用者众多，不值得除去。
 */
void
tuplestore_puttuple(Tuplestorestate *state, HeapTuple tuple)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	/*
	 * 复制元组。（即使在 WRITEFILE 情形也必须
	 * 这样做。COPYTUP 含 USEMEM，故此处免。）
	 */
	tuple = COPYTUP(state, tuple);

	tuplestore_puttuple_common(state, (void *) tuple);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 与 tuplestore_puttuple() 类似，但基于 values +
 * nulls 数组。省去一次元组构造操作。
 */
void
tuplestore_putvalues(Tuplestorestate *state, TupleDesc tdesc,
					 Datum *values, bool *isnull)
{
	MinimalTuple tuple;
	MemoryContext oldcxt = MemoryContextSwitchTo(state->context);

	tuple = heap_form_minimal_tuple(tdesc, values, isnull);
	USEMEM(state, GetMemoryChunkSpace(tuple));

	tuplestore_puttuple_common(state, (void *) tuple);

	MemoryContextSwitchTo(oldcxt);
}

static void
tuplestore_puttuple_common(Tuplestorestate *state, void *tuple)
{
	TSReadPointer *readptr;
	int			i;
	ResourceOwner oldowner;

	state->tuples++;

	switch (state->status)
	{
		case TSS_INMEM:

			/*
			 * 按需更新读指针；见上文的 API 规范。
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					readptr->current = state->memtupcount;
				}
			}

			/*
			 * 按需增长数组。注意我们在还剩一个空槽时
			 * 尝试增长数组——若失败，仍有空间存放
			 * 传入的元组，然后我们会
			 * 切换到磁带操作。
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				(void) grow_memtuples(state);
				Assert(state->memtupcount < state->memtupsize);
			}

			/* 把元组存入内存数组 */
			state->memtuples[state->memtupcount++] = tuple;

			/*
			 * 若仍能放入可用内存且拥有数组槽位则完成。
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
				return;

			/*
			 * 不行；该切换到基于磁带的操作了。确保
			 * temp file(s) 建在合适的临时表空间中。
			 */
			PrepareTempTablespaces();

			/* 把文件与存储的资源所有者关联 */
			oldowner = CurrentResourceOwner;
			CurrentResourceOwner = state->resowner;

			state->myfile = BufFileCreateTemp(state->interXact);

			CurrentResourceOwner = oldowner;

			/*
			 * 冻结关于是否使用尾随长度字的决定。一旦
			 * 数据上带后即无法改变此选择，即使调用者
			 * 可能放弃该需求。
			 */
			state->backward = (state->eflags & EXEC_FLAG_BACKWARD) != 0;
			state->status = TSS_WRITEFILE;
			dumptuples(state);
			break;
		case TSS_WRITEFILE:

			/*
			 * 按需更新读指针；见上文 API 规范。注意：
			 * BufFileTell 相当廉价，所以不值得设法避免
			 * 多次调用。
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					BufFileTell(state->myfile,
								&readptr->file,
								&readptr->offset);
				}
			}

			WRITETUP(state, tuple);
			break;
		case TSS_READFILE:

			/*
			 * 从读取切换到写入。
			 */
			if (!state->readptrs[state->activeptr].eof_reached)
				BufFileTell(state->myfile,
							&state->readptrs[state->activeptr].file,
							&state->readptrs[state->activeptr].offset);
			if (BufFileSeek(state->myfile,
							state->writepos_file, state->writepos_offset,
							SEEK_SET) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek in tuplestore temporary file")));
			state->status = TSS_WRITEFILE;

			/*
			 * 按需更新读指针；见上文的 API 规范。
			 */
			readptr = state->readptrs;
			for (i = 0; i < state->readptrcount; readptr++, i++)
			{
				if (readptr->eof_reached && i != state->activeptr)
				{
					readptr->eof_reached = false;
					readptr->file = state->writepos_file;
					readptr->offset = state->writepos_offset;
				}
			}

			WRITETUP(state, tuple);
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * 以正向或反向任一方向获取下一个元组。
 * 如果没有更多元组则返回 NULL。若设置了
 * should_free，则调用者用完后必须 pfree 它。
 *
 * 仅当 randomAccess 为真，或向 tuplestore_set_eflags()
 * 传入 EXEC_FLAG_BACKWARD 时才允许反向扫描。
 */
static void *
tuplestore_gettuple(Tuplestorestate *state, bool forward,
					bool *should_free)
{
	TSReadPointer *readptr = &state->readptrs[state->activeptr];
	unsigned int tuplen;
	void	   *tup;

	Assert(forward || (readptr->eflags & EXEC_FLAG_BACKWARD));

	switch (state->status)
	{
		case TSS_INMEM:
			*should_free = false;
			if (forward)
			{
				if (readptr->eof_reached)
					return NULL;
				if (readptr->current < state->memtupcount)
				{
					/* 还有元组，故返回它 */
					return state->memtuples[readptr->current++];
				}
				readptr->eof_reached = true;
				return NULL;
			}
			else
			{
				/*
				 * 若所有元组已取完，则返回最后
				 * 元组，否则返回前一个元组。
				 */
				if (readptr->eof_reached)
				{
					readptr->current = state->memtupcount;
					readptr->eof_reached = false;
				}
				else
				{
					if (readptr->current <= state->memtupdeleted)
					{
						Assert(!state->truncated);
						return NULL;
					}
					readptr->current--; /* 上一返回元组 */
				}
				if (readptr->current <= state->memtupdeleted)
				{
					Assert(!state->truncated);
					return NULL;
				}
				return state->memtuples[readptr->current - 1];
			}
			break;

		case TSS_WRITEFILE:
			/* 若只会返回 NULL 则跳过状态切换 */
			if (readptr->eof_reached && forward)
				return NULL;

			/*
			 * 从写入切换到读取。
			 */
			BufFileTell(state->myfile,
						&state->writepos_file, &state->writepos_offset);
			if (!readptr->eof_reached)
				if (BufFileSeek(state->myfile,
								readptr->file, readptr->offset,
								SEEK_SET) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not seek in tuplestore temporary file")));
			state->status = TSS_READFILE;
			/* FALLTHROUGH */

		case TSS_READFILE:
			*should_free = true;
			if (forward)
			{
				if ((tuplen = getlen(state, true)) != 0)
				{
					tup = READTUP(state, tuplen);
					return tup;
				}
				else
				{
					readptr->eof_reached = true;
					return NULL;
				}
			}

			/*
			 * 反向。
			 *
			 * 若所有元组已取完，则返回最后返回的元组，
			 * 否则返回前一个元组。
			 *
			 * 回退去取之前返回元组的结尾长度
			 * 字。寻道失败则假定在文件开头。
			 */
			if (BufFileSeek(state->myfile, 0, -(long) sizeof(unsigned int),
							SEEK_CUR) != 0)
			{
				/* 即使向后抓取失败也能脱离 eof 状态 */
				readptr->eof_reached = false;
				Assert(!state->truncated);
				return NULL;
			}
			tuplen = getlen(state, false);

			if (readptr->eof_reached)
			{
				readptr->eof_reached = false;
				/* 将在返回 NULL 前返回先前返回的元组 */
			}
			else
			{
				/*
				 * 回退取它前面元组的结尾长度字。
				 */
				if (BufFileSeek(state->myfile, 0,
								-(long) (tuplen + 2 * sizeof(unsigned int)),
								SEEK_CUR) != 0)
				{
					/*
					 * 若那样失败，则前一元组想必是文件中
					 * 第一个。回退使之成为正向下个要读的
					 * 元组（未必显然正确，但内存中的情形
					 * 就是这样）。
					 */
					if (BufFileSeek(state->myfile, 0,
									-(long) (tuplen + sizeof(unsigned int)),
									SEEK_CUR) != 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not seek in tuplestore temporary file")));
					Assert(!state->truncated);
					return NULL;
				}
				tuplen = getlen(state, false);
			}

			/*
			 * 现在我们有了前一元组的长度，回退并读取它。
			 * 注：READTUP 预期我们位于元组初始长度字
			 * 之后，所以回退到该位置。
			 */
			if (BufFileSeek(state->myfile, 0,
							-(long) tuplen,
							SEEK_CUR) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek in tuplestore temporary file")));
			tup = READTUP(state, tuplen);
			return tup;

		default:
			elog(ERROR, "invalid tuplestore state");
			return NULL;		/* 安抚编译器 */
	}
}

/*
 * tuplestore_gettupleslot - 导出的获取 MinimalTuple 函数
 *
 * 若成功，把元组放入槽位并返回真；否则清空槽
 * 位返回假。
 *
 * 若 copy 为真，槽位收到一个复制的元组（在当前
 * 内存上下文中分配），无论以后如何操纵元组存
 * 储的状态它都保持有效。若 copy 为假，槽位可能
 * 只收到指向元组存储所持元组的指针。后者更
 * 高效，但如果对元组存储有额外写入，槽位内容
 * 可能会损坏。（若使用 tuplestore_trim，见其中注释。）
 */
bool
tuplestore_gettupleslot(Tuplestorestate *state, bool forward,
						bool copy, TupleTableSlot *slot)
{
	MinimalTuple tuple;
	bool		should_free;

	tuple = (MinimalTuple) tuplestore_gettuple(state, forward, &should_free);

	if (tuple)
	{
		if (copy && !should_free)
		{
			tuple = heap_copy_minimal_tuple(tuple);
			should_free = true;
		}
		ExecStoreMinimalTuple(tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * tuplestore_advance - 导出的调整位置函数，不取回元组
 *
 * 我们可以优化此情形以避免 palloc/pfree 开销，但
 * 目前似乎不值得。
 */
bool
tuplestore_advance(Tuplestorestate *state, bool forward)
{
	void	   *tuple;
	bool		should_free;

	tuple = tuplestore_gettuple(state, forward, &should_free);

	if (tuple)
	{
		if (should_free)
			pfree(tuple);
		return true;
	}
	else
	{
		return false;
	}
}

/*
 * 沿正向或反向任一方向跳过 N 个元组，
 * 不返回任何数据。N<=0 为无操作。
 * 成功则返回真，元组用尽则返回假。
 */
bool
tuplestore_skiptuples(Tuplestorestate *state, int64 ntuples, bool forward)
{
	TSReadPointer *readptr = &state->readptrs[state->activeptr];

	Assert(forward || (readptr->eflags & EXEC_FLAG_BACKWARD));

	if (ntuples <= 0)
		return true;

	switch (state->status)
	{
		case TSS_INMEM:
			if (forward)
			{
				if (readptr->eof_reached)
					return false;
				if (state->memtupcount - readptr->current >= ntuples)
				{
					readptr->current += ntuples;
					return true;
				}
				readptr->current = state->memtupcount;
				readptr->eof_reached = true;
				return false;
			}
			else
			{
				if (readptr->eof_reached)
				{
					readptr->current = state->memtupcount;
					readptr->eof_reached = false;
					ntuples--;
				}
				if (readptr->current - state->memtupdeleted > ntuples)
				{
					readptr->current -= ntuples;
					return true;
				}
				Assert(!state->truncated);
				readptr->current = state->memtupdeleted;
				return false;
			}
			break;

		default:
			/* 我们目前不努力优化其他情形 */
			while (ntuples-- > 0)
			{
				void	   *tuple;
				bool		should_free;

				tuple = tuplestore_gettuple(state, forward, &should_free);

				if (tuple == NULL)
					return false;
				if (should_free)
					pfree(tuple);
				CHECK_FOR_INTERRUPTS();
			}
			return true;
	}
}

/*
 * dumptuples - 把元组移出内存并写到磁带
 *
 * 作为副作用，我们必须把每个读指针的位置从
 * "current" 转换为 file/offset 格式。但 eof_reached
 * 指针状态不变。
 */
static void
dumptuples(Tuplestorestate *state)
{
	int			i;

	for (i = state->memtupdeleted;; i++)
	{
		TSReadPointer *readptr = state->readptrs;
		int			j;

		for (j = 0; j < state->readptrcount; readptr++, j++)
		{
			if (i == readptr->current && !readptr->eof_reached)
				BufFileTell(state->myfile,
							&readptr->file, &readptr->offset);
		}
		if (i >= state->memtupcount)
			break;
		WRITETUP(state, state->memtuples[i]);
	}
	state->memtupdeleted = 0;
	state->memtupcount = 0;
}

/*
 * tuplestore_rescan		- 回卷活动读指针到起点
 */
void
tuplestore_rescan(Tuplestorestate *state)
{
	TSReadPointer *readptr = &state->readptrs[state->activeptr];

	Assert(readptr->eflags & EXEC_FLAG_REWIND);
	Assert(!state->truncated);

	switch (state->status)
	{
		case TSS_INMEM:
			readptr->eof_reached = false;
			readptr->current = 0;
			break;
		case TSS_WRITEFILE:
			readptr->eof_reached = false;
			readptr->file = 0;
			readptr->offset = 0;
			break;
		case TSS_READFILE:
			readptr->eof_reached = false;
			if (BufFileSeek(state->myfile, 0, 0, SEEK_SET) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not seek in tuplestore temporary file")));
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_copy_read_pointer - 复制读指针的状态到另一个
 */
void
tuplestore_copy_read_pointer(Tuplestorestate *state,
							 int srcptr, int destptr)
{
	TSReadPointer *sptr = &state->readptrs[srcptr];
	TSReadPointer *dptr = &state->readptrs[destptr];

	Assert(srcptr >= 0 && srcptr < state->readptrcount);
	Assert(destptr >= 0 && destptr < state->readptrcount);

	/* 自赋值无操作 */
	if (srcptr == destptr)
		return;

	if (dptr->eflags != sptr->eflags)
	{
		/* 整体 eflags 可能变化，故先复制再重算 */
		int			eflags;
		int			i;

		*dptr = *sptr;
		eflags = state->readptrs[0].eflags;
		for (i = 1; i < state->readptrcount; i++)
			eflags |= state->readptrs[i].eflags;
		state->eflags = eflags;
	}
	else
		*dptr = *sptr;

	switch (state->status)
	{
		case TSS_INMEM:
		case TSS_WRITEFILE:
			/* 无事 */
			break;
		case TSS_READFILE:

			/*
			 * 此情形有点棘手，因为活动读指针的位置
			 * 对应的是寻道点，而非其
			 * 变量中的值。赋给活动指针需要寻道，而
			 * 从活动指针赋值则需要 tell，除非处于
			 * eof_reached
			 */
			if (destptr == state->activeptr)
			{
				if (dptr->eof_reached)
				{
					if (BufFileSeek(state->myfile,
									state->writepos_file,
									state->writepos_offset,
									SEEK_SET) != 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not seek in tuplestore temporary file")));
				}
				else
				{
					if (BufFileSeek(state->myfile,
									dptr->file, dptr->offset,
									SEEK_SET) != 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not seek in tuplestore temporary file")));
				}
			}
			else if (srcptr == state->activeptr)
			{
				if (!dptr->eof_reached)
					BufFileTell(state->myfile,
								&dptr->file,
								&dptr->offset);
			}
			break;
		default:
			elog(ERROR, "invalid tuplestore state");
			break;
	}
}

/*
 * tuplestore_trim	- 移除不再需要的元组。
 *
 * 调用本函数即授权元组存储删除所有
 * 位于最旧读指针之前的元组，若无读指针被标为需要
 * REWIND 能力。
 *
 * 注：如果也没有任何指针拥有 BACKWARD 能力，这
 * 显然是安全的。若某指针被标为 BACKWARD 但
 * 无 REWIND，则它能后移，但不能越过最旧的其他
 * 指针
 */
void
tuplestore_trim(Tuplestorestate *state)
{
	int			oldest;
	int			nremove;
	int			i;

	/*
	 * 若有读指针需要回卷能力，则不许截断。
	 *
	 */
	if (state->eflags & EXEC_FLAG_REWIND)
		return;

	/*
	 * 我们不费心修剪临时文件，因为这通常比任由
	 * 它们留在内核缓冲区里直到老化淘汰更费工夫。
	 */
	if (state->status != TSS_INMEM)
		return;

	/* 找出最旧的读指针 */
	oldest = state->memtupcount;
	for (i = 0; i < state->readptrcount; i++)
	{
		if (!state->readptrs[i].eof_reached)
			oldest = Min(oldest, state->readptrs[i].current);
	}

	/*
	 * 注：你可能认为我们可以删除最旧"当前"位置之前
	 * 的所有元组，因为那一个就是要返回的下一个。
	 * 不过，由于 tuplestore_gettuple 返回的是指向我们
	 * 内部元组副本的直接指针，调用者很可能仍把
	 * "当前"之前的那个元组引用在槽位中。所以我们
	 * 在最旧"当前"之前保留一个额外元组。（严格来说，
	 * 本可以规定这类调用者给 tuplestore_gettupleslot
	 * 传 "copy" 标志，但出于效率特免此例。）
	 */
	nremove = oldest - 1;
	if (nremove <= 0)
		return;					/* 无事可做 */

	Assert(nremove >= state->memtupdeleted);
	Assert(nremove <= state->memtupcount);

	/* 释放不再需要的元组 */
	for (i = state->memtupdeleted; i < nremove; i++)
	{
		FREEMEM(state, GetMemoryChunkSpace(state->memtuples[i]));
		pfree(state->memtuples[i]);
		state->memtuples[i] = NULL;
	}
	state->memtupdeleted = nremove;

	/* 标记元组存储已截断（仅用于 Assert 交叉检查） */
	state->truncated = true;

	/*
	 * 若 nremove 小于 memtupcount 的 1/8，就到此为止，让
	 * "已删除"槽位保持为 NULL。这防止我们耗费 O(N^2)
	 * 的时间反复对一个大型指针数组做 memmove。空间
	 * 的最坏浪费相当小，因为只是指针而非整个元组。
	 */
	if (nremove < state->memtupcount / 8)
		return;

	/*
	 * 把数组下滑并重新调整指针。
	 *
	 * 在合并连接当前的用法中，可以证明总会恰好
	 * 只有一个未移除元组；故优化该情形。
	 */
	if (nremove + 1 == state->memtupcount)
		state->memtuples[0] = state->memtuples[nremove];
	else
		memmove(state->memtuples, state->memtuples + nremove,
				(state->memtupcount - nremove) * sizeof(void *));

	state->memtupdeleted = 0;
	state->memtupcount -= nremove;
	for (i = 0; i < state->readptrcount; i++)
	{
		if (!state->readptrs[i].eof_reached)
			state->readptrs[i].current -= nremove;
	}
}

/*
 * tuplestore_in_memory
 *
 * 如果元组存储尚未落盘则返回真。
 *
 * XXX 暴露此函数违反模块性……应该除掉它。
 */
bool
tuplestore_in_memory(Tuplestorestate *state)
{
	return (state->status == TSS_INMEM);
}


/*
 * 磁带接口例程
 */

static unsigned int
getlen(Tuplestorestate *state, bool eofOK)
{
	unsigned int len;
	size_t		nbytes;

	nbytes = BufFileReadMaybeEOF(state->myfile, &len, sizeof(len), eofOK);
	if (nbytes == 0)
		return 0;
	else
		return len;
}


/*
 * 针对 HeapTuple 情形专用的例程
 *
 * 存储形式其实是一个 MinimalTuple，但主要出于历史
 * 原因我们允许 COPYTUP 从 HeapTuple 工作。
 *
 * 由于 MinimalTuple 的第一个字已有长度，我们无需
 * 单独写入它。
 */

static void *
copytup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple;

	tuple = minimal_tuple_from_heap_tuple((HeapTuple) tup);
	USEMEM(state, GetMemoryChunkSpace(tuple));
	return (void *) tuple;
}

static void
writetup_heap(Tuplestorestate *state, void *tup)
{
	MinimalTuple tuple = (MinimalTuple) tup;

	/* 要写入的 MinimalTuple 部分： */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* 磁盘上总占用： */
	unsigned int tuplen = tupbodylen + sizeof(int);

	BufFileWrite(state->myfile, &tuplen, sizeof(tuplen));
	BufFileWrite(state->myfile, tupbody, tupbodylen);
	if (state->backward)		/* 要尾随长度字？ */
		BufFileWrite(state->myfile, &tuplen, sizeof(tuplen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_free_minimal_tuple(tuple);
}

static void *
readtup_heap(Tuplestorestate *state, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) palloc(tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* 读入元组本体 */
	tuple->t_len = tuplen;
	BufFileReadExact(state->myfile, tupbody, tupbodylen);
	if (state->backward)		/* 要尾随长度字？ */
		BufFileReadExact(state->myfile, &tuplen, sizeof(tuplen));
	return (void *) tuple;
}
