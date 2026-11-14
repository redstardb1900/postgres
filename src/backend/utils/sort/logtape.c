/*-------------------------------------------------------------------------
 *
 * logtape.c
 *	  临时文件内逻辑磁带(logical tapes)管理。
 *
 * 本模块用于支持经由多趟归并的排序（参见
 * tuplesort.c）。归并对磁带设备是理想算法，但若
 * 我们在磁盘上为每条磁带各建单独的文件，
 * 就有一个恼人的问题：空间用量的峰值至少为
 * 待排序实际数据量的两倍。（这是必然的，因为每个
 * 数据项都会同时出现在末趟归并的输入与输出
 * 磁带。）
 *
 * 我们可以这样绕过该问题：认识到任一
 * 磁带数据集(dataset)（最终输出可能除外）只以
 * 完全顺序的方式精确地写入和读取一次。因此，
 * 数据项被读取一次后不再需要，我们可以回收其
 * 空间供正在生成的新磁带数据集使用。这样，
 * 总空间用量本质上就只是实际数据量，外加
 * 微不足道的簿记和启停开销。
 *
 * 很少有 OSes 允许把文件的任意部分归还给系统，
 * 所以我们必须自行在单个
 * 逻辑文件(file)内实现空间回收。logtape.c 负责簿记，
 * 为 tuplesort.c 提供 N 个独立磁带设备的假象。注意
 * logtape.c 本身依赖 buffile.c 提供超出底层
 * OS 所支持大小的"逻辑文件"。
 *
 * 为简单起见，我们在底层文件中以 BLCKSZ 大小的块
 * 为单位分配和释放空间。空间分配归结为跟踪
 * 底层文件中哪些块属于哪条逻辑磁带，外加
 * 任何空闲的块（已回收但尚未重用）。
 * 每条逻辑磁带的块构成一条链，每块有 prev 和
 * next 指针。
 *
 * 初始写入趟保证会完全顺序地填满底层文件，
 * 无论数据如何划分成逻辑磁带。
 * 开始归并趟后，访问模式会变得难以预测得多
 * --- 但其中的寻道开销应与
 * 我们将每条逻辑磁带保存在单独文件的情况相当，
 * 而为换取回收的空间节约，付出的性能代价
 * 并不严重。我们尽量让写访问局部化，总是
 * 写往编号最小的空闲块；这是否大有帮助并不
 * 清楚，但也不会有害。（XXX 对空闲块采用 LIFO
 * 策略会不会更好？）
 *
 * 为进一步提高 I/O 顺序性，读取时可用更大的缓冲区
 * 并一次性地从同一磁带读取多个块，
 * 只要缓冲区变空。
 *
 * 为支持上述写往最小空闲块的策略，空闲链表
 * 是一个最小堆。
 *
 * 由于所有簿记与缓冲内存都用 palloc() 分配，底层
 * 文件由 OpenTemporaryFile 创建，逻辑磁带集的
 * 所有资源都必然会被清理，即使处理被
 * ereport(ERROR) 中止。为避免混淆，调用者应当注意
 * 单个 LogicalTapeSet 的全部调用都应在同一个 palloc
 * 上下文中。
 *
 * 为支持涉及多个工作进程协同调用
 * tuplesort.c 例程的并行排序，需把每个工作进程的
 * BufFile/tapeset 拼接成一个由主导进程管理的
 * 单一逻辑磁带集。工作进程此时应已产出一个
 * 物化磁带（其全部输出）。归并段数将恒等于
 * 输入磁带数目，且输入磁带数目与参与者（工作进程
 * Tuplesortstate）的数目相同。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/logtape.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>

#include "storage/buffile.h"
#include "utils/builtins.h"
#include "utils/logtape.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"

/*
 * TapeBlockTrailer 存储在每个 BLCKSZ 块的末尾。
 *
 * 磁带的第一个块 prev == -1。磁带的最后一个块
 * 在 'next' 中以取反形式存储该块的有效字节数，
 * 因此 next < 0 表示最后一个块。
 */
typedef struct TapeBlockTrailer
{
	long		prev;			/* 本磁带上的前一块号；首块取
								 * -1 */
	long		next;			/* 本磁带上的下一块；或末块
								 * 的有效字节数(若<0) */
} TapeBlockTrailer;

#define TapeBlockPayloadSize  (BLCKSZ - sizeof(TapeBlockTrailer))
#define TapeBlockGetTrailer(buf) \
	((TapeBlockTrailer *) ((char *) buf + TapeBlockPayloadSize))

#define TapeBlockIsLast(buf) (TapeBlockGetTrailer(buf)->next < 0)
#define TapeBlockGetNBytes(buf) \
	(TapeBlockIsLast(buf) ? \
	 (- TapeBlockGetTrailer(buf)->next) : TapeBlockPayloadSize)
#define TapeBlockSetNBytes(buf, nbytes) \
	(TapeBlockGetTrailer(buf)->next = -(nbytes))

/*
 * 当多条磁带被并发写入时（如 HashAgg 中），
 * 通过向各条磁带预分配块号来避免过度碎片化。
 * 每次预分配大小翻倍，自
 * TAPE_WRITE_PREALLOC_MIN 块起至 TAPE_WRITE_PREALLOC_MAX 块。
 *
 * 预分配不执行任何文件系统操作；只有块号
 * 被保留下来。这可能导致稀疏写入，从而令
 * ltsWriteBlock() 用零填充空洞。
 */
#define TAPE_WRITE_PREALLOC_MIN 8
#define TAPE_WRITE_PREALLOC_MAX 128

/*
 * 该数据结构表示同一文件所存逻辑磁带集合中的
 * 一条"逻辑磁带"。
 *
 * 写入时，我们把当前部分写就的数据块保存在
 * 缓冲区中。读取时，缓冲区中可容纳多个块。注意，
 * 块被读入缓冲区时，我们不会保留它的块尾，
 * 因此缓冲区中是一整段连续的磁带
 * 数据。
 */
struct LogicalTape
{
	LogicalTapeSet *tapeSet;	/* 所属磁带集 */

	bool		writing;		/* 写入阶段为 T */
	bool		frozen;			/* 读取时不应释放块则为 T */
	bool		dirty;			/* 缓冲区是否需要写出？ */

	/*
	 * 磁带的第一块、当前块和下一块的块号。
	 *
	 * "当前"块号仅在写入时或从冻结
	 * 磁带读取时才有效。读取非冻结磁带时，需用更大
	 * 的读缓冲区容纳多个块，故"当前"块有
	 * 歧义。
	 *
	 * 当执行工作进程磁带 BufFiles 的拼接时，读取会
	 * 对统一 BufFile 空间中第一块施加偏移量。
	 */
	long		firstBlockNumber;
	long		curBlockNumber;
	long		nextBlockNumber;
	long		offsetBlockNumber;

	/*
	 * 当前块(block)的缓冲区。
	 */
	char	   *buffer;			/* 物理缓冲区（单独 palloc） */
	int			buffer_size;	/* 缓冲区的已分配大小 */
	int			max_size;		/* buffer_size 最高安全值 */
	int			pos;			/* 缓冲区中下个读/写位置 */
	int			nbytes;			/* 缓冲区内有效字节总数 */

	/*
	 * 预分配的块号保存在一个按降序排序的数组中；
	 * 块从数组末端开始消耗（块号最小者
	 * 最先）。
	 */
	long	   *prealloc;
	int			nprealloc;		/* 列表中的元素数 */
	int			prealloc_size;	/* 列表可容纳的元素数 */
};

/*
 * 该数据结构表示一组共享同一底层文件空间的
 * 相关"逻辑磁带"。（但该"文件"在需要绕开
 * OS 文件大小限制时可为多个文件；buffile.c 代为处理。）
 * 属于磁带集的磁带可以按需即时地创建与销毁。
 *
 */
struct LogicalTapeSet
{
	BufFile    *pfile;			/* 整个磁带集的底层文件 */
	SharedFileSet *fileset;
	int			worker;			/* 共享为 worker 号，主导/串行-1 */

	/*
	 * 文件大小跟踪。nBlocksWritten 是底层文件的大小，
	 * 以 BLCKSZ 块计。nBlocksAllocated 是由
	 * ltsReleaseBlock() 分配的块数，它总是大于等于
	 * nBlocksWritten。介于 nBlocksAllocated 与
	 * nBlocksWritten 之间的块是已分配给某条磁带，
	 * 但尚未写入底层文件的块。nHoleBlocks 跟踪
	 * BufFile 拼接之后各工作进程空间之间未用空洞
	 * 的块数。
	 */
	long		nBlocksAllocated;	/* 已分配块数 */
	long		nBlocksWritten; /* 底层文件中已使用的块数 */
	long		nHoleBlocks;	/* 剩余"空洞"块数 */

	/*
	 * 我们把已回收且可用的块号存储在 freeBlocks[] 中。
	 * 当没有这种块时，我们就扩展底层文件。
	 *
	 * 若 forgetFreeSpace 为真，则任何被释放的块
	 * 都被直接遗忘，不记入 freeBlocks[]。参见
	 * LogicalTapeSetForgetFreeSpace()
	 */
	bool		forgetFreeSpace;	/* 是否记录空闲块？ */
	long	   *freeBlocks;		/* 持有最小堆的可调数组 */
	long		nFreeBlocks;	/* 当前空闲块数 */
	Size		freeBlocksLen;	/* freeBlocks[] 当前已分配长度 */
	bool		enable_prealloc;	/* 预分配写块？ */
};

static LogicalTape *ltsCreateTape(LogicalTapeSet *lts);
static void ltsWriteBlock(LogicalTapeSet *lts, long blocknum, const void *buffer);
static void ltsReadBlock(LogicalTapeSet *lts, long blocknum, void *buffer);
static long ltsGetBlock(LogicalTapeSet *lts, LogicalTape *lt);
static long ltsGetFreeBlock(LogicalTapeSet *lts);
static long ltsGetPreallocBlock(LogicalTapeSet *lts, LogicalTape *lt);
static void ltsReleaseBlock(LogicalTapeSet *lts, long blocknum);
static void ltsInitReadBuffer(LogicalTape *lt);


/*
 * 把一个块大小的缓冲区写入底层文件的指定块。
 *
 * 无需错误返回约定；任何错误都以 ereport() 报告。
 */
static void
ltsWriteBlock(LogicalTapeSet *lts, long blocknum, const void *buffer)
{
	/*
	 * BufFile 不支持"空洞"，因此当我们要写的块
	 * 越过当前文件末尾时，就在当前文件末尾与
	 * 目标块之间填充零。
	 *
	 * 这种情况可能发生在磁带预分配块时；也可能
	 * 发生在磁带末块尚未刷写时。
	 *
	 * 注意，BufFile 拼接可能在 BufFile 中各工作进程
	 * 拥有的块区间之间留下"空洞"。这些空洞仅为
	 * 报告目的而被跟踪。我们从不读写这些空洞块，
	 * 故此处不考虑。
	 */
	while (blocknum > lts->nBlocksWritten)
	{
		PGIOAlignedBlock zerobuf;

		MemSet(zerobuf.data, 0, sizeof(zerobuf));

		ltsWriteBlock(lts, lts->nBlocksWritten, zerobuf.data);
	}

	/* 写出请求的块 */
	if (BufFileSeekBlock(lts->pfile, blocknum) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to block %ld of temporary file",
						blocknum)));
	BufFileWrite(lts->pfile, buffer, BLCKSZ);

	/* 若扩展了文件，则更新 nBlocksWritten */
	if (blocknum == lts->nBlocksWritten)
		lts->nBlocksWritten++;
}

/*
 * 从底层文件的指定块读出一个块大小的缓冲区。
 *
 * 无需错误返回约定；任何错误都以 ereport() 报告。本
 * 模块绝不应尝试读取一个它并不知道存在的块。
 */
static void
ltsReadBlock(LogicalTapeSet *lts, long blocknum, void *buffer)
{
	if (BufFileSeekBlock(lts->pfile, blocknum) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to block %ld of temporary file",
						blocknum)));
	BufFileReadExact(lts->pfile, buffer, BLCKSZ);
}

/*
 * 尽量多地把块读入每条磁带的缓冲区。
 *
 * 读到数据返回 true，EOF 时为 'false'。
 */
static bool
ltsReadFillBuffer(LogicalTape *lt)
{
	lt->pos = 0;
	lt->nbytes = 0;

	do
	{
		char	   *thisbuf = lt->buffer + lt->nbytes;
		long		datablocknum = lt->nextBlockNumber;

		/* 取下一块号 */
		if (datablocknum == -1L)
			break;				/* EOF */
		/* 主导磁带集需应用工作进程偏移 */
		datablocknum += lt->offsetBlockNumber;

		/* 读该块 */
		ltsReadBlock(lt->tapeSet, datablocknum, thisbuf);
		if (!lt->frozen)
			ltsReleaseBlock(lt->tapeSet, datablocknum);
		lt->curBlockNumber = lt->nextBlockNumber;

		lt->nbytes += TapeBlockGetNBytes(thisbuf);
		if (TapeBlockIsLast(thisbuf))
		{
			lt->nextBlockNumber = -1L;
			/* EOF */
			break;
		}
		else
			lt->nextBlockNumber = TapeBlockGetTrailer(thisbuf)->next;

		/* 若还有缓冲区空间，则前进到下一块 */
	} while (lt->buffer_size - lt->nbytes > BLCKSZ);

	return (lt->nbytes > 0);
}

static inline unsigned long
left_offset(unsigned long i)
{
	return 2 * i + 1;
}

static inline unsigned long
right_offset(unsigned long i)
{
	return 2 * i + 2;
}

static inline unsigned long
parent_offset(unsigned long i)
{
	return (i - 1) / 2;
}

/*
 * 取下一个待写块。
 */
static long
ltsGetBlock(LogicalTapeSet *lts, LogicalTape *lt)
{
	if (lts->enable_prealloc)
		return ltsGetPreallocBlock(lts, lt);
	else
		return ltsGetFreeBlock(lts);
}

/*
 * 从磁带集的全局空闲最小堆中选出当前
 * 未用块。
 */
static long
ltsGetFreeBlock(LogicalTapeSet *lts)
{
	long	   *heap = lts->freeBlocks;
	long		blocknum;
	long		heapsize;
	long		holeval;
	unsigned long holepos;

	/* 空闲链表为空；分配新块 */
	if (lts->nFreeBlocks == 0)
		return lts->nBlocksAllocated++;

	/* 堆仅含一个元素时很简单 */
	if (lts->nFreeBlocks == 1)
	{
		lts->nFreeBlocks--;
		return lts->freeBlocks[0];
	}

	/* 移除最小堆顶 */
	blocknum = heap[0];

	/* 用最小堆数组的末元素顶替它 */
	holeval = heap[--lts->nFreeBlocks];

	/* 下沉 */
	holepos = 0;				/* holepos 即"空洞"所在 */
	heapsize = lts->nFreeBlocks;
	while (true)
	{
		unsigned long left = left_offset(holepos);
		unsigned long right = right_offset(holepos);
		unsigned long min_child;

		if (left < heapsize && right < heapsize)
			min_child = (heap[left] < heap[right]) ? left : right;
		else if (left < heapsize)
			min_child = left;
		else if (right < heapsize)
			min_child = right;
		else
			break;

		if (heap[min_child] >= holeval)
			break;

		heap[holepos] = heap[min_child];
		holepos = min_child;
	}
	heap[holepos] = holeval;

	return blocknum;
}

/*
 * 从磁带的预分配列表中返回最小的空闲块号。
 * 必要时，用磁带集空闲链表中的块重新填满
 * 该表。
 */
static long
ltsGetPreallocBlock(LogicalTapeSet *lts, LogicalTape *lt)
{
	/* 按降序排序，因此返回最后一个元素 */
	if (lt->nprealloc > 0)
		return lt->prealloc[--lt->nprealloc];

	if (lt->prealloc == NULL)
	{
		lt->prealloc_size = TAPE_WRITE_PREALLOC_MIN;
		lt->prealloc = (long *) palloc(sizeof(long) * lt->prealloc_size);
	}
	else if (lt->prealloc_size < TAPE_WRITE_PREALLOC_MAX)
	{
		/* 预分配列表用尽时，将大小翻倍 */
		lt->prealloc_size *= 2;
		if (lt->prealloc_size > TAPE_WRITE_PREALLOC_MAX)
			lt->prealloc_size = TAPE_WRITE_PREALLOC_MAX;
		lt->prealloc = (long *) repalloc(lt->prealloc,
										 sizeof(long) * lt->prealloc_size);
	}

	/* 重填预分配列表 */
	lt->nprealloc = lt->prealloc_size;
	for (int i = lt->nprealloc; i > 0; i--)
	{
		lt->prealloc[i - 1] = ltsGetFreeBlock(lts);

		/* 验证降序次序 */
		Assert(i == lt->nprealloc || lt->prealloc[i - 1] > lt->prealloc[i]);
	}

	return lt->prealloc[--lt->nprealloc];
}

/*
 * 将块号归还空闲链表。
 */
static void
ltsReleaseBlock(LogicalTapeSet *lts, long blocknum)
{
	long	   *heap;
	unsigned long holepos;

	/*
	 * 若我们不再需要记录空闲空间，则什么都不做。
	 */
	if (lts->forgetFreeSpace)
		return;

	/*
	 * freeBlocks 数组满则扩容。
	 */
	if (lts->nFreeBlocks >= lts->freeBlocksLen)
	{
		/*
		 * 若空闲链表变得非常大，就直接返回并泄漏这个
		 * 块。
		 */
		if (lts->freeBlocksLen * 2 * sizeof(long) > MaxAllocSize)
			return;

		lts->freeBlocksLen *= 2;
		lts->freeBlocks = (long *) repalloc(lts->freeBlocks,
											lts->freeBlocksLen * sizeof(long));
	}

	/* 在最小堆数组末端制造"空洞" */
	heap = lts->freeBlocks;
	holepos = lts->nFreeBlocks;
	lts->nFreeBlocks++;

	/* 上滤以插入 blocknum */
	while (holepos != 0)
	{
		unsigned long parent = parent_offset(holepos);

		if (heap[parent] < blocknum)
			break;

		heap[holepos] = heap[parent];
		holepos = parent;
	}
	heap[holepos] = blocknum;
}

/*
 * 惰性地分配并初始化读缓冲区。很多磁带同时打开、
 * 但并非都在回卷与读取之间活动时，这能避免
 * 浪费
 */
static void
ltsInitReadBuffer(LogicalTape *lt)
{
	Assert(lt->buffer_size > 0);
	lt->buffer = palloc(lt->buffer_size);

	/* 读取第一个块；磁带为空则重置 */
	lt->nextBlockNumber = lt->firstBlockNumber;
	lt->pos = 0;
	lt->nbytes = 0;
	ltsReadFillBuffer(lt);
}

/*
 * 创建一个磁带集，由临时底层文件支撑。
 *
 * 磁带集初始为空。可用 LogicalTapeCreate()
 * 建磁带。
 *
 * 单进程排序中，fileset 参数传 NULL，worker 参数传
 * -1。
 *
 * 并行排序中，各并行工作进程传入共享 fileset
 * 句柄和自己的 worker 号。工作进程全部结束后，在
 * 主导进程中创建磁带集，传入共享 fileset 句柄，
 * worker 传 -1，并用 LogicalTapeImport() 导入工作进程磁带。
 *
 * 目前，主导进程只会把工作进程磁带导入该集合，并不
 * 创建自己的磁带，尽管理论上这应该可行。
 *
 * 若 preallocate 为真，每条磁带的块都分批分配。
 * 这避免了同时写入多条磁带时发生碎片化。
 *
 */
LogicalTapeSet *
LogicalTapeSetCreate(bool preallocate, SharedFileSet *fileset, int worker)
{
	LogicalTapeSet *lts;

	/*
	 * 创建顶级结构，含每条磁带的 LogicalTape 结构。
	 */
	lts = (LogicalTapeSet *) palloc(sizeof(LogicalTapeSet));
	lts->nBlocksAllocated = 0L;
	lts->nBlocksWritten = 0L;
	lts->nHoleBlocks = 0L;
	lts->forgetFreeSpace = false;
	lts->freeBlocksLen = 32;	/* 合理的初始猜测 */
	lts->freeBlocks = (long *) palloc(lts->freeBlocksLen * sizeof(long));
	lts->nFreeBlocks = 0;
	lts->enable_prealloc = preallocate;

	lts->fileset = fileset;
	lts->worker = worker;

	/*
	 * 按需创建临时 BufFile 存储。
	 *
	 * 在主导进程中，我们劫持第一个被导入磁带的
	 * BufFile，并把后续磁带的 BufFiles 都拼接到其上。
	 * 因此不要在此创建 BufFile。工作进程情形与串行
	 * 情形则更简单。二者大体非常相似 -- 工作进程用
	 * 共享 fileset，而串行排序用常规的串行 BufFile。
	 */
	if (fileset && worker == -1)
		lts->pfile = NULL;
	else if (fileset)
	{
		char		filename[MAXPGPATH];

		pg_itoa(worker, filename);
		lts->pfile = BufFileCreateFileSet(&fileset->fs, filename);
	}
	else
		lts->pfile = BufFileCreateTemp(false);

	return lts;
}

/*
 * 从现有的共享 BufFile 中认领一条逻辑磁带。
 *
 * 调用者应当是主导进程。虽然磁带在工作进程中
 * 被标记为冻结，但在主导进程中打开时并非冻结，因为
 * 非冻结磁带读缓冲区更大。（冻结磁带读缓冲区更小，
 * 为随机优化）
 */
LogicalTape *
LogicalTapeImport(LogicalTapeSet *lts, int worker, TapeShare *shared)
{
	LogicalTape *lt;
	long		tapeblocks;
	char		filename[MAXPGPATH];
	BufFile    *file;
	int64		filesize;

	lt = ltsCreateTape(lts);

	/*
	 * 构建所有 buffile 的拼接视图，记住每个源文件
	 * 起始处的块号。
	 */
	pg_itoa(worker, filename);
	file = BufFileOpenFileSet(&lts->fileset->fs, filename, O_RDONLY, false);
	filesize = BufFileSize(file);

	/*
	 * 保存第一个 BufFile，后续 BufFiles 都拼接到其上，
	 * 沿途记录各磁带块偏移。
	 */
	lt->firstBlockNumber = shared->firstblocknumber;
	if (lts->pfile == NULL)
	{
		lts->pfile = file;
		lt->offsetBlockNumber = 0L;
	}
	else
	{
		lt->offsetBlockNumber = BufFileAppend(lts->pfile, file);
	}
	/* 读缓冲区的分配不要超过可能有用的程度 */
	lt->max_size = Min(MaxAllocSize, filesize);
	tapeblocks = filesize / BLCKSZ;

	/*
	 * 更新已分配块数与已写块数，以反映被导入的
	 * BufFile。已分配/已写块包含拼接的 BufFiles 之间
	 * 留下的空洞占用的空间。且跟踪空洞块数，让
	 * 我们之后能倒推出用于监测的
	 * 物理块数。
	 */
	lts->nHoleBlocks += lt->offsetBlockNumber - lts->nBlocksAllocated;

	lts->nBlocksAllocated = lt->offsetBlockNumber + tapeblocks;
	lts->nBlocksWritten = lts->nBlocksAllocated;

	return lt;
}

/*
 * 关闭逻辑磁带集并释放所有资源。
 *
 * 注意：这并不关闭任何磁带！必须先关闭它们，
 * 或者任其随内存上下文一起销毁。
 */
void
LogicalTapeSetClose(LogicalTapeSet *lts)
{
	BufFileClose(lts->pfile);
	pfree(lts->freeBlocks);
	pfree(lts);
}

/*
 * 在给定磁带集中创建逻辑磁带。
 *
 * 该磁带以写入状态初始化。
 */
LogicalTape *
LogicalTapeCreate(LogicalTapeSet *lts)
{
	/*
	 * 目前阻碍在主导进程中创建新磁带的唯一原因是
	 * 用 BufFileOpenShared() 打开的 BufFiles 按定义
	 * 是只读的，但如果值得，这一点可以改变。目前，
	 * 写主导磁带会引发"Bad file descriptor"（坏文件
	 * 描述符）错误，因此 tuplesort 必须彻底避免写入。
	 */
	if (lts->fileset && lts->worker == -1)
		elog(ERROR, "cannot create new tapes in leader process");

	return ltsCreateTape(lts);
}

static LogicalTape *
ltsCreateTape(LogicalTapeSet *lts)
{
	LogicalTape *lt;

	/*
	 * 创建每磁带结构。注意 I/O 缓冲区为惰性分配。
	 */
	lt = palloc(sizeof(LogicalTape));
	lt->tapeSet = lts;
	lt->writing = true;
	lt->frozen = false;
	lt->dirty = false;
	lt->firstBlockNumber = -1L;
	lt->curBlockNumber = -1L;
	lt->nextBlockNumber = -1L;
	lt->offsetBlockNumber = 0L;
	lt->buffer = NULL;
	lt->buffer_size = 0;
	/* palloc() 超过 MaxAllocSize 会失败 */
	lt->max_size = MaxAllocSize;
	lt->pos = 0;
	lt->nbytes = 0;
	lt->prealloc = NULL;
	lt->nprealloc = 0;
	lt->prealloc_size = 0;

	return lt;
}

/*
 * 关闭逻辑磁带。
 *
 * 注意：这不会把任何块归还给空闲链表！必须先
 * 把磁带读到末尾才能重用该空间。在当前用法中，
 * 我们只在完整读取磁带后才关闭。
 */
void
LogicalTapeClose(LogicalTape *lt)
{
	if (lt->buffer)
		pfree(lt->buffer);
	pfree(lt);
}

/*
 * 把逻辑磁带集标记为不再需要管理空闲空间。
 *
 * 若调用者不打算再向磁带集写入数据，但正在从
 * 非冻结磁带中读取，则应当调用此函数。由于不再
 * 计划写入，记住空闲块就不再有用。设置该标志可以
 * 避免在 ltsReleaseBlock() 中浪费时间与空间，该函数
 * 的设计初衷并非处理大量空闲块。
 */
void
LogicalTapeSetForgetFreeSpace(LogicalTapeSet *lts)
{
	lts->forgetFreeSpace = true;
}

/*
 * 写入逻辑磁带。
 *
 * 无错误返回；失败时以 ereport() 报告。
 */
void
LogicalTapeWrite(LogicalTape *lt, const void *ptr, size_t size)
{
	LogicalTapeSet *lts = lt->tapeSet;
	size_t		nthistime;

	Assert(lt->writing);
	Assert(lt->offsetBlockNumber == 0L);

	/* 首次写入时分配数据缓冲区和第一个块 */
	if (lt->buffer == NULL)
	{
		lt->buffer = (char *) palloc(BLCKSZ);
		lt->buffer_size = BLCKSZ;
	}
	if (lt->curBlockNumber == -1)
	{
		Assert(lt->firstBlockNumber == -1);
		Assert(lt->pos == 0);

		lt->curBlockNumber = ltsGetBlock(lts, lt);
		lt->firstBlockNumber = lt->curBlockNumber;

		TapeBlockGetTrailer(lt->buffer)->prev = -1L;
	}

	Assert(lt->buffer_size == BLCKSZ);
	while (size > 0)
	{
		if (lt->pos >= (int) TapeBlockPayloadSize)
		{
			/* 缓冲区满，即写出 */
			long		nextBlockNumber;

			if (!lt->dirty)
			{
				/* 嗯，直接从读取转入写入？ */
				elog(ERROR, "invalid logtape state: should be dirty");
			}

			/*
			 * 先分配下一块，以便能把它存入本块的
			 * 'next' 指针中。
			 */
			nextBlockNumber = ltsGetBlock(lt->tapeSet, lt);

			/* 设置 next 指针并写出当前块。 */
			TapeBlockGetTrailer(lt->buffer)->next = nextBlockNumber;
			ltsWriteBlock(lt->tapeSet, lt->curBlockNumber, lt->buffer);

			/* 初始化下一块的 prev 指针 */
			TapeBlockGetTrailer(lt->buffer)->prev = lt->curBlockNumber;
			lt->curBlockNumber = nextBlockNumber;
			lt->pos = 0;
			lt->nbytes = 0;
		}

		nthistime = TapeBlockPayloadSize - lt->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(lt->buffer + lt->pos, ptr, nthistime);

		lt->dirty = true;
		lt->pos += nthistime;
		if (lt->nbytes < lt->pos)
			lt->nbytes = lt->pos;
		ptr = (const char *) ptr + nthistime;
		size -= nthistime;
	}
}

/*
 * 回卷逻辑磁带并从写入切换到读取。
 *
 * 磁带当前必须处于写入状态，或读取状态下的"冻结"。
 *
 * 'buffer_size' 指定读缓冲区使用多少内存。
 * 无论参数如何，实际使用的内存量都介于 BLCKSZ 与
 * MaxAllocSize 之间，且为 BLCKSZ 的倍数。给定值会在
 * 必要时被向下取整并截断以适配约束。如果磁带
 * 已冻结，则会忽略 'buffer_size' 参数，而使用小的
 * BLCKSZ 缓冲区。
 */
void
LogicalTapeRewindForRead(LogicalTape *lt, size_t buffer_size)
{
	LogicalTapeSet *lts = lt->tapeSet;

	/*
	 * 按需取整封顶 buffer_size。
	 */
	if (lt->frozen)
		buffer_size = BLCKSZ;
	else
	{
		/* 至少需要一个块 */
		if (buffer_size < BLCKSZ)
			buffer_size = BLCKSZ;

		/* palloc() 超过 max_size 不太可能有帮助 */
		if (buffer_size > lt->max_size)
			buffer_size = lt->max_size;

		/* 按 BLCKSZ 边界向下取整 */
		buffer_size -= buffer_size % BLCKSZ;
	}

	if (lt->writing)
	{
		/*
		 * 写入阶段的收尾。刷写最后一个部分数据块，并
		 * 回卷以普通（破坏性）读。
		 */
		if (lt->dirty)
		{
			/*
			 * 只要我们至少填满过缓冲区一次，其内容从
			 * valgrind 的角度看就是完全有定义的，即使当前
			 * 终点之外的内容可能过时。不过至少在
			 * 并行排序的情形下，数据量可能小到一次都
			 * 填不满缓冲区。此时应告知 valgrind 其内容是
			 * 有定义的，免得它
			 * 抱怨。
			 */
			VALGRIND_MAKE_MEM_DEFINED(lt->buffer + lt->nbytes,
									  lt->buffer_size - lt->nbytes);

			TapeBlockSetNBytes(lt->buffer, lt->nbytes);
			ltsWriteBlock(lt->tapeSet, lt->curBlockNumber, lt->buffer);
		}
		lt->writing = false;
	}
	else
	{
		/*
		 * 磁带已冻结时才允许；回卷以进行再一轮读取。
		 *
		 */
		Assert(lt->frozen);
	}

	if (lt->buffer)
		pfree(lt->buffer);

	/* 缓冲区为惰性分配，但在此处设置大小 */
	lt->buffer = NULL;
	lt->buffer_size = buffer_size;

	/* 释放预分配列表，并把未用的块号归还 */
	if (lt->prealloc != NULL)
	{
		for (int i = lt->nprealloc; i > 0; i--)
			ltsReleaseBlock(lts, lt->prealloc[i - 1]);
		pfree(lt->prealloc);
		lt->prealloc = NULL;
		lt->nprealloc = 0;
		lt->prealloc_size = 0;
	}
}

/*
 * 从逻辑磁带读取。
 *
 * 返回值小于所请求的字节数即表示提前遇到 EOF。
 */
size_t
LogicalTapeRead(LogicalTape *lt, void *ptr, size_t size)
{
	size_t		nread = 0;
	size_t		nthistime;

	Assert(!lt->writing);

	if (lt->buffer == NULL)
		ltsInitReadBuffer(lt);

	while (size > 0)
	{
		if (lt->pos >= lt->nbytes)
		{
			/* 向缓冲区载入更多数据。 */
			if (!ltsReadFillBuffer(lt))
				break;			/* EOF */
		}

		nthistime = lt->nbytes - lt->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(ptr, lt->buffer + lt->pos, nthistime);

		lt->pos += nthistime;
		ptr = (char *) ptr + nthistime;
		size -= nthistime;
		nread += nthistime;
	}

	return nread;
}

/*
 * "冻结"磁带的内容，使其可以被多次读取
 * 和/或反向读取。磁带一旦冻结，其内容在
 * LogicalTapeSet 销毁之前都不会被释放。这预计
 * 只用于归并的最终输出趟。
 *
 * 这*必须*恰在写入趟结束时、磁带回卷之前调用
 * （回卷之后就来不及了！）。它会执行一次回卷
 * 并"免费"切换到读取模式。紧随其后的回卷读取
 * 调用可以，但并非必需。
 *
 * share 输出参数会被设置为磁带冻结后所用存储的
 * 细节，之后可传给主导进程内的 LogicalTapeSetCreate。
 * 这些元数据只有为主导进程冻结其最终输出的
 * 工作进程调用者才会关心（单一物化磁带）。串行
 * 排序应把 share 设为 NULL。
 */
void
LogicalTapeFreeze(LogicalTape *lt, TapeShare *share)
{
	LogicalTapeSet *lts = lt->tapeSet;

	Assert(lt->writing);
	Assert(lt->offsetBlockNumber == 0L);

	/*
	 * 写入阶段的收尾。刷写最后一个部分数据块，并
	 * 作非破坏性回读。
	 */
	if (lt->dirty)
	{
		/*
		 * 只要我们至少填满过缓冲区一次，其内容就从
		 * valgrind 视角看是完全有定义的，即使当前
		 * 终点之外的内容可能过时。不过至少在
		 * 并行排序的情形下，待排序的数据量可能小到
		 * 一次都填不满。告知 valgrind 其内容是有定义的，
		 * 免得它抱怨。
		 */
		VALGRIND_MAKE_MEM_DEFINED(lt->buffer + lt->nbytes,
								  lt->buffer_size - lt->nbytes);

		TapeBlockSetNBytes(lt->buffer, lt->nbytes);
		ltsWriteBlock(lt->tapeSet, lt->curBlockNumber, lt->buffer);
	}
	lt->writing = false;
	lt->frozen = true;

	/*
	 * 寻道与回退函数假定读缓冲区只有单个块。
	 * 就当前用法而言这没问题。更大的缓冲区有助于
	 * 在从多条磁带读取时，让底层文件的读取模式
	 * 对操作系统显得更顺序。但排序结束时，当磁带
	 * 已冻结时，反正也只会从单条磁带读取。
	 */
	if (!lt->buffer || lt->buffer_size != BLCKSZ)
	{
		if (lt->buffer)
			pfree(lt->buffer);
		lt->buffer = palloc(BLCKSZ);
		lt->buffer_size = BLCKSZ;
	}

	/* 读取第一个块；磁带为空则重置 */
	lt->curBlockNumber = lt->firstBlockNumber;
	lt->pos = 0;
	lt->nbytes = 0;

	if (lt->firstBlockNumber == -1L)
		lt->nextBlockNumber = -1L;
	ltsReadBlock(lt->tapeSet, lt->curBlockNumber, lt->buffer);
	if (TapeBlockIsLast(lt->buffer))
		lt->nextBlockNumber = -1L;
	else
		lt->nextBlockNumber = TapeBlockGetTrailer(lt->buffer)->next;
	lt->nbytes = TapeBlockGetNBytes(lt->buffer);

	/* 调用者要共享其磁带集时的额外处理 */
	if (share)
	{
		BufFileExportFileSet(lts->pfile);
		share->firstblocknumber = lt->firstBlockNumber;
	}
}

/*
 * 把磁带回退给定的字节数。（我们还支持更通用
 * 的寻道接口，见下文。）
 *
 * *只有*为读取而冻结的磁带才能回退；我们
 * 不支持写入时的随机访问，且非冻结的读磁带
 * 可能已丢弃所需数据！
 *
 * 返回回退的字节数。若当前位置之前
 * 没有那么多数据，回退量就会小于所请求的量。
 * 此时磁带会被定位到磁带的
 * 开头。
 */
size_t
LogicalTapeBackspace(LogicalTape *lt, size_t size)
{
	size_t		seekpos = 0;

	Assert(lt->frozen);
	Assert(lt->buffer_size == BLCKSZ);

	if (lt->buffer == NULL)
		ltsInitReadBuffer(lt);

	/*
	 * 在当前块内寻道的简单情形。
	 */
	if (size <= (size_t) lt->pos)
	{
		lt->pos -= (int) size;
		return size;
	}

	/*
	 * 不太简单的情形，必须沿块链往回走。这种
	 * 实现对长距离寻道来说效率相当低，但我们
	 * 实际并不这样（跨一个元组的寻道是典型）。
	 */
	seekpos = (size_t) lt->pos; /* 本块内的部分 */
	while (size > seekpos)
	{
		long		prev = TapeBlockGetTrailer(lt->buffer)->prev;

		if (prev == -1L)
		{
			/* 试图回退越过磁带开头。 */
			if (lt->curBlockNumber != lt->firstBlockNumber)
				elog(ERROR, "unexpected end of tape");
			lt->pos = 0;
			return seekpos;
		}

		ltsReadBlock(lt->tapeSet, prev, lt->buffer);

		if (TapeBlockGetTrailer(lt->buffer)->next != lt->curBlockNumber)
			elog(ERROR, "broken tape, next of block %ld is %ld, expected %ld",
				 prev,
				 TapeBlockGetTrailer(lt->buffer)->next,
				 lt->curBlockNumber);

		lt->nbytes = TapeBlockPayloadSize;
		lt->curBlockNumber = prev;
		lt->nextBlockNumber = TapeBlockGetTrailer(lt->buffer)->next;

		seekpos += TapeBlockPayloadSize;
	}

	/*
	 * 'seekpos' 现在可以大于 'size'，因为它指向
	 * 目标块的起始处。差值即为该页内的位置。
	 *
	 */
	lt->pos = seekpos - size;
	return size;
}

/*
 * 寻道到逻辑磁带中的任意位置。
 *
 * 只有为读而冻结的磁带可寻道。
 *
 * 必须传入之前由 LogicalTapeTell() 返回的
 * 块/偏移。
 */
void
LogicalTapeSeek(LogicalTape *lt, long blocknum, int offset)
{
	Assert(lt->frozen);
	Assert(offset >= 0 && offset <= TapeBlockPayloadSize);
	Assert(lt->buffer_size == BLCKSZ);

	if (lt->buffer == NULL)
		ltsInitReadBuffer(lt);

	if (blocknum != lt->curBlockNumber)
	{
		ltsReadBlock(lt->tapeSet, blocknum, lt->buffer);
		lt->curBlockNumber = blocknum;
		lt->nbytes = TapeBlockPayloadSize;
		lt->nextBlockNumber = TapeBlockGetTrailer(lt->buffer)->next;
	}

	if (offset > lt->nbytes)
		elog(ERROR, "invalid tape seek position");
	lt->pos = offset;
}

/*
 * 以适合之后 LogicalTapeSeek 的形式获取当前位置。
 *
 * 注意：在写入阶段这样做、以便之后冻结时用
 * 该位置寻道是没有问题的。不清楚是否有人需要。
 */
void
LogicalTapeTell(LogicalTape *lt, long *blocknum, int *offset)
{
	if (lt->buffer == NULL)
		ltsInitReadBuffer(lt);

	Assert(lt->offsetBlockNumber == 0L);

	/* 若缓冲区更大，'pos' 就不会等于页内偏移 */
	Assert(lt->buffer_size == BLCKSZ);

	*blocknum = lt->curBlockNumber;
	*offset = lt->pos;
}

/*
 * 获取一个 LogicalTapeSet 当前占用的总磁盘空间，以块
 * 计。若有打开的写缓冲则不计。
 */
long
LogicalTapeSetBlocks(LogicalTapeSet *lts)
{
	return lts->nBlocksWritten - lts->nHoleBlocks;
}
