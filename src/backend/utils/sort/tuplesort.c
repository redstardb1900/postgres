/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  通用元组排序例程。
 *
 * 本模块提供一套通用的元组排序设施，可用于
 * 不同种类的可排序对象。各具体排序
 * 变体的实现见 tuplesortvariants.c。
 * 本模块对小数据量和大数据量都能高效
 * 工作。小数据量用 qsort() 在内存中排序；大
 * 数据量则借助临时文件和标准外部排序
 * 算法。
 *
 * 关于外部排序算法的更多知识可参阅 Knuth
 * 第三卷。我们采用的算法是平衡 k 路归并。
 * PostgreSQL 15 之前我们使用多相归并算法
 * （Knuth 算法 5.4.2D），但在现代硬件上，简单
 * 的直接平衡归并更好。Knuth 假设磁带驱动器
 * 十分昂贵，尤其是假设归并段数总是远多于
 * 磁带驱动器数。多相归并算法擅长让所有磁带
 * 驱动器都保持忙碌，但在我们的实现中，"磁带
 * 驱动器"的代价只比几 Kb 内存缓冲区略高，因
 * 此可以拥有大量磁带驱动器。特别地，如果磁带驱
 * 动器数与归并段相同，重复 I/O 即可免。
 *
 * 历史上，我们使用替换选择来划分输入的归并
 * 段，其形式是用堆实现的优先树
 * （本质上即 Knuth 算法 5.2.3H），但现在我们总是
 * 生成归并段。
 *
 * 单次排序操作允许使用的内存近似量由调用者
 * 以 KB 为单位指定（多数传入 work_mem）。最初，
 * 我们吸收元组并简单地把它们存入一个未排序
 * 数组，只要还没有超出 workMem。如果输入已结
 * 束而未超出 workMem，我们就用 qsort() 排序该数组，
 * 然后通过顺序扫描元组数组来返回元组。如果
 * 确实超出了 workMem，我们就开始把元组输出到
 * 临时磁带上的归并段中。元组快速排序后批量
 * 倾倒时，我们以新的输出磁带开始新的归并段。
 * 如果达到磁带数量上限，我们就以轮转方式把
 * 后续归并段写入现有磁带。这种情况下将需要
 * 多轮归并才能完成。输入结束后，把内存中剩余
 * 元组转储为末段后归并。
 *
 * 归并归并段时，我们使用一个只包含每个源归并
 * 段最前面元组的堆；我们反复输出最小元组，并
 * 用其源磁带上的下一个元组替换它（如有）。当
 * 堆变空时，归并即告完成。因此基础归并算法只
 * 需要很少的内存 —— M 路归并只需 M 个元组，且
 * M 被限制在很小的数值。不过，我们仍然可以充分
 * 利用整个 workMem 分配额，从每条源磁带预读额外
 * 的块。如果不预读，我们对临时文件的访问模式
 * 会非常杂乱；平均而言，在向输出磁带写入 M 个
 * 块的同一时间，我们会从 M 条源磁带各读一个块，
 * 因而访问完全没有顺序性，这会让多数 Unix 内核
 * 所采用的预读方法失效。更糟的是，输出磁带会
 * 被写入临时文件中非常随机的一系列块，这注定
 * 让以后读取该磁带时的情况更加糟糕。一次直接
 * 的归并轮因此会花费大量时间等待磁盘寻道。我
 * 们可以改进这一点：从每条源磁带顺序地预读，
 * 依次从每条磁带载入约 workMem/M 字节，并让顺序
 * 的块立即可供重用。这种方式有助于让读和写访
 * 问都局部化。预读由 logtape.c 处理，我们只需告诉
 * 它缓冲区要用多少内存。
 *
 * 在目前的代码中，我们基于 workMem 来确定输入
 * 磁带数 M：我们希望 workMem/M 足够大，使得每次
 * 从磁带读取时都能读取可观的数据量，从而保持
 * 上述访问局部性。尽管如此，workMem 较大时我们
 * 可以有很多磁带。逻辑"磁带"由 logtape.c 实现，
 * 它会在每个块被读走后立刻回收磁盘空间，避免
 * 空间浪费。
 *
 * 当调用者要求对排序结果进行随机访问时，我们
 * 会在逻辑磁带上形成最终的已排序归并段，并将
 * 其"冻结"，以便随机访问。当调用者不需要随机
 * 访问时，只要每条逻辑磁带上只剩一个归并段，
 * tuplesort_performsort() 就会返回。最后的归并会在
 * 调用者反复调用 tuplesort_getXXX 时即时进行；这
 * 省去了把所有数据写盘再读入的整整一轮开销。
 *
 * 本模块支持并行排序。并行排序需要一个或多个
 * 工作进程与一个主导进程协作，每一个都拥有自
 * 己的 tuplesort 状态。主导进程（更准确地说，是
 * 与主导进程关联的 Tuplesortstate）会创建完整的
 * 磁带集，其中包含待归并的工作进程磁带；每个
 * 工作进程一个归并段。随后对其进行归并。工作
 * 进程保证从各自输入恰产出一个归并段。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_am.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"

/*
 * memtuples 数组的初始大小。我们试图选择这样的
 * 大小，使数组不超过 ALLOCSET_SEPARATE_THRESHOLD，并
 * 让分配开销有可能降低。不过，我们不考虑小于
 * 1024。
 *
 */
#define INITIAL_MEMTUPSIZE Max(1024, \
	ALLOCSET_SEPARATE_THRESHOLD / sizeof(SortTuple) + 1)

/* GUC 变量 */
#ifdef TRACE_SORT
bool		trace_sort = false;
#endif

#ifdef DEBUG_BOUNDED_SORT
bool		optimize_bounded_sort = true;
#endif


/*
 * 归并期间，我们使用一组预分配的定长槽位来容
 * 纳元组。免 palloc/pfree 开销。
 *
 * 归并不需要很多内存，所以我们浪费一点也承受
 * 得起，可以使用超大尺寸的槽位。如果元组超过
 * 1 kB，palloc() 开销就不再显著。
 *
 * 'nextfree' 在该块位于空闲链表时才有效。使用时
 * 槽位有元组。
 */
#define SLAB_SLOT_SIZE 1024

typedef union SlabSlot
{
	union SlabSlot *nextfree;
	char		buffer[SLAB_SLOT_SIZE];
} SlabSlot;

/*
 * Tuplesort 对象的可能状态。它们表示在 Tuplesort
 * 各例程调用之间保持的状态。
 */
typedef enum
{
	TSS_INITIAL,				/* 装入元组；仍在内存限额内 */
	TSS_BOUNDED,				/* 装入元组到有界大小的堆 */
	TSS_BUILDRUNS,				/* 装入元组；写入磁带 */
	TSS_SORTEDINMEM,			/* 排序完全在内存中完成 */
	TSS_SORTEDONTAPE,			/* 排序完成，末段在磁带上 */
	TSS_FINALMERGE				/* 正在即时执行最终归并 */
} TupSortStatus;

/*
 * 计算所要使用磁带数量的参数 —— 见 inittapes()
 * 与tuplesort_merge_order()
 *
 * 在此计算中，我们假定每条磁带大约要花费我们
 * 1 个块的缓冲区空间。这忽略了每条磁带所需的
 * 所有其他数据结构的开销，但大概足够接近了。
 *
 * MERGE_BUFFER_SIZE 是我们希望为每条输入磁带分配
 * 的缓冲区空间量，用于预读（见文件开头处的讨
 * 论）。这是在 TAPE_BUFFER_OVERHEAD 已含的 1 块之外。
 */
#define MINORDER		6		/* 最小归并路数 */
#define MAXORDER		500		/* 最大归并路数 */
#define TAPE_BUFFER_OVERHEAD		BLCKSZ
#define MERGE_BUFFER_SIZE			(BLCKSZ * 32)


/*
 * Tuplesort 操作的私有状态。
 */
struct Tuplesortstate
{
	TuplesortPublic base;
	TupSortStatus status;		/* 如上所示的枚举值 */
	bool		bounded;		/* 调用者是否指定了返回最大
								 * 元组数？ */
	bool		boundUsed;		/* 若确实使用了有界堆则为真 */
	int			bound;			/* 若有界，则为元组数上限 */
	int64		availMem;		/* 剩余可用内存，以字节计 */
	int64		allowedMem;		/* 总允许内存（字节） */
	int			maxTapes;		/* 每轮归并的输入磁带数上限
								*/
	int64		maxSpace;		/* 各组排序占用空间最大值，无论
								 * 在内存还是在磁盘上。 */
	bool		isMaxSpaceDisk; /* maxSpace 为磁盘空间值时为真；
								 * 为内存空间值时则为假。
								 */
	TupSortStatus maxSpaceStatus;	/* 达到 maxSpace 时的排序状态 */
	LogicalTapeSet *tapeset;	/* 临时文件磁带的 logtape.c 对象 */

	/*
	 * 此数组容纳当前位于排序内存中的元组。如果我们
	 * 处于 INITIAL 状态，元组无特定顺序；如果处于
	 * SORTEDINMEM 状态，元组处于最终已排序顺序；在
	 * BUILDRUNS 和 FINALMERGE 状态，元组呈算法 H
	 * "堆"序。SORTEDONTAPE 不用该数组。
	 */
	SortTuple  *memtuples;		/* SortTuple 结构数组 */
	int			memtupcount;	/* 当前存在的元组数 */
	int			memtupsize;		/* memtuples 数组的已分配长度 */
	bool		growmemtuples;	/* memtuples 增长仍在进行？ */

	/*
	 * 元组的内存有时是用一个简单的 slab 分配器分配
	 * 的，而非用 palloc()。目前，我们会在开始归并时切
	 * 换到 slab 分配。归并只需任意时刻在内存中保留
	 * 少量固定数目的元组，从而避免 palloc/pfree 的
	 * 开销，办法是循环使用固定数目的定长槽位来容纳
	 * 元组。
	 *
	 * 对于 slab，我们使用一次大分配，划分为 SLAB_SLOT_SIZE
	 * 个槽位。该分配的大小为每条磁带一个槽位，外加
	 * 一个附加槽位。我们需要这么多的槽位来容纳归并
	 * 期间保留在堆中的所有元组，外加最后从排序中
	 * tuplesort_gettuple返回者。
	 *
	 * 最初，所有槽位都保存在一个空闲槽位链表中。当
	 * 从磁带读出一个元组时，若放得下，就把它放入
	 * 下一个可用槽位。若元组大于 SLAB_SLOT_SIZE，就改用
	 * palloc
	 *
	 * 处理完一个元组后，我们把槽位交还空闲链表；如
	 * 果原为 palloc 则 pfree()。我们知道元组
	 * 是否来自 slab，判据是其指针值位于
	 * slabMemoryBegin-End 间
	 *
	 * 使用 slab 分配器时，不再使用 USEMEM/LACKMEM 机制
	 * 来跟踪内存使用。
	 */
	bool		slabAllocatorUsed;

	char	   *slabMemoryBegin;	/* slab 内存区起点 */
	char	   *slabMemoryEnd;	/* slab 内存区终点 */
	SlabSlot   *slabFreeHead;	/* 空闲链表头 */

	/* 输入和输出磁带缓冲区所用内存。 */
	size_t		tape_buffer_mem;

	/*
	 * 当我们在 tuplesort_gettuple_XXX 中向调用者返回一
	 * 个来自磁带的元组时（即处于 TSS_SORTEDONTAPE 或
	 * TSS_FINALMERGE 模式），我们会把它记入
	 * 'lastReturnedTuple'，下次回收。
	 */
	void	   *lastReturnedTuple;

	/*
	 * 构建初始归并段期间，这是当前的输出归并段号。
	 * 此后为我们已构建的初始归并段数。
	 */
	int			currentRun;

	/*
	 * 归并用逻辑磁带。
	 *
	 * 初始归并段写入输出磁带。在每轮归并中，上一轮
	 * 的输出磁带变成输入磁带，并且按需创建新的输出
	 * 磁带。nInputTapes 等于 nInputRuns 时，只剩最后一
	 * 轮归并。
	 */
	LogicalTape **inputTapes;
	int			nInputTapes;
	int			nInputRuns;

	LogicalTape **outputTapes;
	int			nOutputTapes;
	int			nOutputRuns;

	LogicalTape *destTape;		/* 当前输出磁带 */

	/*
	 * 这些变量在排序完成后用于跟踪下一个要返回的
	 * 元组。（磁带情形下，磁带当前的读位置也是关键
	 * 状态。）
	 */
	LogicalTape *result_tape;	/* 完成输出的实际磁带 */
	int			current;		/* 数组下标（仅 SORTEDINMEM 用） */
	bool		eof_reached;	/* 已达 EOF（游标需要） */

	/* markpos_xxx 保存 mark 与 restore 的标记位置 */
	long		markpos_block;	/* 磁带块号（仅 SORTEDONTAPE 用） */
	int			markpos_offset; /* 存下的 "current" 或块内偏移 */
	bool		markpos_eof;	/* eof_reached 存值 */

	/*
	 * 这些变量在并行排序期间使用。
	 *
	 * worker 是我们的工作进程标识符。遵循如下通用约
	 * 定：-1 表示主导进程 tuplesort，>= 0 表示工作进
	 * 程 tuplesort。（-1 亦可为串行。）
	 *
	 * shared 是可变的共享内存状态，用于协调并行排
	 * 序。
	 *
	 * nParticipants 是主导进程已知确实已启动的工作进
	 * 程 Tuplesortstates 数量，这意味着它们必须完成主导
	 * 进程需要归并的归并段。通常包括主导进程自身持
	 * 有的一个工作进程状态。只设置在主导进程
	 * 的Tuplesortstate。
	 */
	int			worker;
	Sharedsort *shared;
	int			nParticipants;

	/*
	 * 用于管理"缩略键" sortsupport 例程的附加状态
	 * （目前除哈希索引情形外，其余情形均可使用）。
	 * 跟踪检验该优化有效性的间隔。
	 *
	 */
	int64		abbrevNext;		/* 下次检查适用性的元组
								 * 序号 */

	/*
	 * 排序开始时刻的资源快照。
	 */
#ifdef TRACE_SORT
	PGRUsage	ru_start;
#endif
};

/*
 * tuplesort 并行操作的私有可变状态。分配于
 * 共享内存。
 */
struct Sharedsort
{
	/* 互斥锁保护 tapes 之前的字段 */
	slock_t		mutex;

	/*
	 * currentWorker 为并行排序工作进程生成顺序标识号。
	 * 这些编号从 0 开始，且总是无空缺。
	 *
	 * 工作进程递增 workersFinished 来表明自己已完成。
	 * 如果该值等于主导进程内的 state.nParticipants，
	 * 即可归并了。
	 */
	int			currentWorker;
	int			workersFinished;

	/* 临时文件空间 */
	SharedFileSet fileset;

	/* tapes 弹性数组的大小 */
	int			nTapes;

	/*
	 * 磁带数组，工作进程借此回报主导进程拼接全部
	 * 工作进程磁带以供归并所需的信息
	 */
	TapeShare	tapes[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * 给定的元组是否分配自 slab 内存区？
 */
#define IS_SLAB_SLOT(state, tuple) \
	((char *) (tuple) >= (state)->slabMemoryBegin && \
	 (char *) (tuple) < (state)->slabMemoryEnd)

/*
 * 把给定元组交还 slab 内存空闲链表；如果它原是
 * palloc 则释放。
 */
#define RELEASE_SLAB_SLOT(state, tuple) \
	do { \
		SlabSlot *buf = (SlabSlot *) tuple; \
		\
		if (IS_SLAB_SLOT((state), buf)) \
		{ \
			buf->nextfree = (state)->slabFreeHead; \
			(state)->slabFreeHead = buf; \
		} else \
			pfree(buf); \
	} while(0)

#define REMOVEABBREV(state,stup,count)	((*(state)->base.removeabbrev) (state, stup, count))
#define COMPARETUP(state,a,b)	((*(state)->base.comparetup) (a, b, state))
#define WRITETUP(state,tape,stup)	((*(state)->base.writetup) (state, tape, stup))
#define READTUP(state,stup,tape,len) ((*(state)->base.readtup) (state, stup, tape, len))
#define FREESTATE(state)	((state)->base.freestate ? (*(state)->base.freestate) (state) : (void) 0)
#define LACKMEM(state)		((state)->availMem < 0 && !(state)->slabAllocatorUsed)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))
#define SERIAL(state)		((state)->shared == NULL)
#define WORKER(state)		((state)->shared && (state)->worker != -1)
#define LEADER(state)		((state)->shared && (state)->worker == -1)

/*
 * NOTES：元组的磁带表示形式
 *
 * 已存储元组的首个 "unsigned int" 必须是整
 * 个元组的磁带上大小，含其自身（因此永不为零；全零
 * "unsigned int" 用来界定归并段）。已存储元组的其余
 * 可能与该元组在内存中的表示形式一致，也可能
 * 不一致；所需转换由 writetup 与 readtup 例程完成。
 *
 * 如果 state->sortopt 含有 TUPLESORT_RANDOMACCESS，则已存
 * 元组的表示形式之后必须跟另一个 "unsigned int"，它是长
 * 度的副本，故实际占用磁带空间比已存长度值多
 * 出 sizeof(unsigned int) 空间。这允许反向读取。当未指
 * 定随机访问标志时，写入/读取例程可省略额外的长
 * 度字。
 *
 * writetup 应当写入两个长度字以及元组
 * 数据。当调用 readtup 时，磁带恰好定位在
 * 前长度字；readtup 必须读取元组数据并越过
 * 后长度字（如存在）。
 *
 * 写入/读取例程可以在必要时利用
 * Tuplesortstate 结构内保存的元组描述数据。
 * 它们还应调整 state->availMem，幅度为所释放或
 * 消耗的内存空间量（而非磁带空间！）。writetup
 * 或 readtup 都应在失败时 ereport()。
 *
 *
 * 关于内存消耗计算的说明：
 *
 * 为元组分配的空间计入 workMem 限额，再加可变大
 * 小 memtuples 数组占用空间。定长空间不计入；它太
 * 小，不值得关注。
 *
 * 注意我们统计的是实际使用的空间（如
 * GetMemoryChunkSpace 所示）而非最初请求的大小。
 * 这一点很重要，因为 palloc 会增加可观的开销。
 * 由于我们不计入 palloc 分配块中浪费的空间，这
 * 仍比 7.3 前好得多。9.6 起调用者传入的元组
 * 使用独立的内存上下文。在关键增量点重置它，能
 * 显著缓解碎片化。
 * readtup 例程用 slab 分配器（无法用重置上下
 * 文，因为该上下文会在归并开始之时被删除）。
 *
 */


static void tuplesort_begin_batch(Tuplesortstate *state);
static bool consider_abort_common(Tuplesortstate *state);
static void inittapes(Tuplesortstate *state, bool mergeruns);
static void inittapestate(Tuplesortstate *state, int maxTapes);
static void selectnewtape(Tuplesortstate *state);
static void init_slab_allocator(Tuplesortstate *state, int numSlots);
static void mergeruns(Tuplesortstate *state);
static void mergeonerun(Tuplesortstate *state);
static void beginmerge(Tuplesortstate *state);
static bool mergereadnext(Tuplesortstate *state, LogicalTape *srcTape, SortTuple *stup);
static void dumptuples(Tuplesortstate *state, bool alltuples);
static void make_bounded_heap(Tuplesortstate *state);
static void sort_bounded_heap(Tuplesortstate *state);
static void tuplesort_sort_memtuples(Tuplesortstate *state);
static void tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple);
static void tuplesort_heap_replace_top(Tuplesortstate *state, SortTuple *tuple);
static void tuplesort_heap_delete_top(Tuplesortstate *state);
static void reversedirection(Tuplesortstate *state);
static unsigned int getlen(LogicalTape *tape, bool eofOK);
static void markrunend(LogicalTape *tape);
static int	worker_get_identifier(Tuplesortstate *state);
static void worker_freeze_result_tape(Tuplesortstate *state);
static void worker_nomergeruns(Tuplesortstate *state);
static void leader_takeover_tapes(Tuplesortstate *state);
static void free_sort_tuple(Tuplesortstate *state, SortTuple *stup);
static void tuplesort_free(Tuplesortstate *state);
static void tuplesort_updatemax(Tuplesortstate *state);

/*
 * 可以内联到专门排序中的专门比较器。目标是尝试
 * 不沿指针找到比较器或元组就对两个元组完成排
 * 序。
 *
 * XXX：目前，这些会退回到比较器函数，后者会把引
 * 导 datum 再比较一次。
 *
 * XXX：目前，对于 datum1 权威、甚至完全无需退到
 * 回调的情形还没有专门化（这适用于 int4/int8/
 * timestamp/date 这类类型，但文本缩略和多键排序并
 * 非如此。其实可以做到！值得吗？
 */

/* 第一键比较器为 ssup_datum_unsigned_cmp 时使用 */
static pg_attribute_always_inline int
qsort_tuple_unsigned_compare(SortTuple *a, SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplyUnsignedSortComparator(a->datum1, a->isnull1,
										  b->datum1, b->isnull1,
										  &state->base.sortKeys[0]);
	if (compare != 0)
		return compare;

	/*
	 * 当没有其他键可用于排序时，无需浪费力气调用
	 * 决胜函数。
	 */
	if (state->base.onlyKey != NULL)
		return 0;

	return state->base.comparetup(a, b, state);
}

#if SIZEOF_DATUM >= 8
/* 第一键比较器为 ssup_datum_signed_cmp 时使用 */
static pg_attribute_always_inline int
qsort_tuple_signed_compare(SortTuple *a, SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplySignedSortComparator(a->datum1, a->isnull1,
										b->datum1, b->isnull1,
										&state->base.sortKeys[0]);

	if (compare != 0)
		return compare;

	/*
	 * 当没有其他键可用于排序时，无需浪费力气调用
	 * 决胜函数。
	 */
	if (state->base.onlyKey != NULL)
		return 0;

	return state->base.comparetup(a, b, state);
}
#endif

/* 第一键比较器为 ssup_datum_int32_cmp 时使用 */
static pg_attribute_always_inline int
qsort_tuple_int32_compare(SortTuple *a, SortTuple *b, Tuplesortstate *state)
{
	int			compare;

	compare = ApplyInt32SortComparator(a->datum1, a->isnull1,
									   b->datum1, b->isnull1,
									   &state->base.sortKeys[0]);

	if (compare != 0)
		return compare;

	/*
	 * 当没有其他键可用于排序时，无需浪费力气调用
	 * 决胜函数。
	 */
	if (state->base.onlyKey != NULL)
		return 0;

	return state->base.comparetup(a, b, state);
}

/*
 * 专用于 SortTuple 对象的特殊 qsort 版本。qsort_tuple()
 * 用相应的 comparetup 函数排序任意 SortTuples 变体。
 * qsort_ssup() 专门用于 comparetup 函数退化为
 * ApplySortComparator() 的情形，即单键 MinimalTuple 排
 * 序和 Datum 排序。qsort_tuple_{unsigned,signed,int32} 专
 * 用于按值传递的引导 datum 的常用比较函数。
 */

#define ST_SORT qsort_tuple_unsigned
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, state) qsort_tuple_unsigned_compare(a, b, state)
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

#if SIZEOF_DATUM >= 8
#define ST_SORT qsort_tuple_signed
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, state) qsort_tuple_signed_compare(a, b, state)
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"
#endif

#define ST_SORT qsort_tuple_int32
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, state) qsort_tuple_int32_compare(a, b, state)
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT qsort_tuple
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE_RUNTIME_POINTER
#define ST_COMPARE_ARG_TYPE Tuplesortstate
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DECLARE
#define ST_DEFINE
#include "lib/sort_template.h"

#define ST_SORT qsort_ssup
#define ST_ELEMENT_TYPE SortTuple
#define ST_COMPARE(a, b, ssup) \
	ApplySortComparator((a)->datum1, (a)->isnull1, \
						(b)->datum1, (b)->isnull1, (ssup))
#define ST_COMPARE_ARG_TYPE SortSupportData
#define ST_CHECK_FOR_INTERRUPTS
#define ST_SCOPE static
#define ST_DEFINE
#include "lib/sort_template.h"

/*
 *		tuplesort_begin_xxx
 *
 * 为元组排序操作做初始化。
 *
 * 调用 tuplesort_begin 之后，调用者应调用 tuplesort_putXXX
 * 零次或多次，然后在全部元组都提供完毕后调用
 * tuplesort_performsort。performsort 之后，通过调用
 * tuplesort_getXXX 以排序后的顺序取回元组，直至其返回
 * false/NULL。（若请求了随机访问，rescan、markpos 与
 * restorepos 亦可。）tuplesort_end 终止并释放内存/磁盘空间。
 *
 * tuplesort_begin 的每个变体都有一个 workMem 参数，指定
 * 数据落盘前可使用的 RAM 千字节数上限。
 * （该参数的正常值是 work_mem，但有些调用者使用其他
 * 值。）每个变体还有 sortopt，它是排序选项的位掩
 * 码。见 tuplesort.h 中的 TUPLESORT_* 定义。
 */

Tuplesortstate *
tuplesort_begin_common(int workMem, SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state;
	MemoryContext maincontext;
	MemoryContext sortcontext;
	MemoryContext oldcontext;

	/* 随机访问支持见 leader_takeover_tapes() 的说明 */
	if (coordinate && (sortopt & TUPLESORT_RANDOMACCESS))
		elog(ERROR, "random access disallowed under parallel sort");

	/*
	 * tuplesort_reset 之后仍存活的内存上下文。
	 * 它保存排序多个相似批次期间值得保留的数据。
	 */
	maincontext = AllocSetContextCreate(CurrentMemoryContext,
										"TupleSort main",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * 为一个排序操作创建工作内存上下文。该上下文的内
	 * 容由 tuplesort_reset 删除。
	 */
	sortcontext = AllocSetContextCreate(maincontext,
										"TupleSort sort",
										ALLOCSET_DEFAULT_SIZES);

	/*
	 * 另外，会在 tuplesort_begin_batch 中建立元组用
	 * 专用工作内存。
	 */

	/*
	 * 把 Tuplesortstate 建在每个排序状态的上下文内。这样，
	 * 关闭时就无需再为它单独 pfree()。
	 */
	oldcontext = MemoryContextSwitchTo(maincontext);

	state = (Tuplesortstate *) palloc0(sizeof(Tuplesortstate));

#ifdef TRACE_SORT
	if (trace_sort)
		pg_rusage_init(&state->ru_start);
#endif

	state->base.sortopt = sortopt;
	state->base.tuples = true;
	state->abbrevNext = 10;

	/*
	 * workMem 被强制至少为 64KB，这是当前 work_mem GUC 的
	 * 最小有效值。这是对那些把内存分给众多工作进程、
	 * 以致每个进程只剩极少内存的并行排序调用者的
	 * 一种防御。
	 */
	state->allowedMem = Max(workMem, 64) * (int64) 1024;
	state->base.sortcontext = sortcontext;
	state->base.maincontext = maincontext;

	/*
	 * 数组的初始大小必须大于 ALLOCSET_SEPARATE_THRESHOLD；
	 * 见 grow_memtuples() 的注释。
	 */
	state->memtupsize = INITIAL_MEMTUPSIZE;
	state->memtuples = NULL;

	/*
	 * 在所有其他非并行相关状态之后，我们建立每一批所
	 * 需的全部状态。
	 */
	tuplesort_begin_batch(state);

	/*
	 * 根据调用者提供的协作信息初始化并行相关状态。
	 *
	 */
	if (!coordinate)
	{
		/* 串行 */
		state->shared = NULL;
		state->worker = -1;
		state->nParticipants = -1;
	}
	else if (coordinate->isWorker)
	{
		/* 并行工作进程从全部输入恰好产出一个最终段 */
		state->shared = coordinate->sharedsort;
		state->worker = worker_get_identifier(state);
		state->nParticipants = -1;
	}
	else
	{
		/* 并行主导进程状态仅用于最终归并 */
		state->shared = coordinate->sharedsort;
		state->worker = -1;
		state->nParticipants = coordinate->nParticipants;
		Assert(state->nParticipants >= 1);
	}

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 *		tuplesort_begin_batch
 *
 * 为用此排序状态处理新的一组元组而建立或重置全
 * 部所需状态。由 tuplesort_begin_common（第一次用此排
 * 序状态时）和 tuplesort_reset（后续使用时）调用。
 */
static void
tuplesort_begin_batch(Tuplesortstate *state)
{
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(state->base.maincontext);

	/*
	 * 调用者元组（IndexTuple）上下文。
	 *
	 * 一个专供调用者传入元组使用的子上下文，可简化
	 * 内存管理。在关键点位重置能减少碎片。
	 * 注意 SortTuples 的 memtuples 数组分配在父上下文中，
	 * 而非本上下文，因为没有必要提前释放
	 * memtuples。对于有界排序，元组可能以任意顺序被
	 * pfree，所以我们使用常规的 aset.c 上下文，以便它
	 * 能利用已释放的内存。排序无界时，我们改用
	 * generation.c 上下文，因为它让分配更紧凑、浪费少，
	 * 分配的 CPU 效率也略高一些。
	 */
	if (state->base.sortopt & TUPLESORT_ALLOWBOUNDED)
		state->base.tuplecontext = AllocSetContextCreate(state->base.sortcontext,
														 "Caller tuples",
														 ALLOCSET_DEFAULT_SIZES);
	else
		state->base.tuplecontext = GenerationContextCreate(state->base.sortcontext,
														   "Caller tuples",
														   ALLOCSET_DEFAULT_SIZES);


	state->status = TSS_INITIAL;
	state->bounded = false;
	state->boundUsed = false;

	state->availMem = state->allowedMem;

	state->tapeset = NULL;

	state->memtupcount = 0;

	/*
	 * 数组的初始大小必须大于 ALLOCSET_SEPARATE_THRESHOLD；
	 * 见 grow_memtuples() 的注释。
	 */
	state->growmemtuples = true;
	state->slabAllocatorUsed = false;
	if (state->memtuples != NULL && state->memtupsize != INITIAL_MEMTUPSIZE)
	{
		pfree(state->memtuples);
		state->memtuples = NULL;
		state->memtupsize = INITIAL_MEMTUPSIZE;
	}
	if (state->memtuples == NULL)
	{
		state->memtuples = (SortTuple *) palloc(state->memtupsize * sizeof(SortTuple));
		USEMEM(state, GetMemoryChunkSpace(state->memtuples));
	}

	/* workMem 必须大到足以容纳最小 memtuples 数组 */
	if (LACKMEM(state))
		elog(ERROR, "insufficient memory allowed for sort");

	state->currentRun = 0;

	/*
	 * 磁带变量（inputTapes、outputTapes 等）将由 inittapes() 在
	 * 需要时初始化。
	 */

	state->result_tape = NULL;	/* 标志：结果磁带尚未形成 */

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_set_bound
 *
 *	告知 tuplesort 至多只需前 N 个结果元组。
 *
 * 必须在插入任何元组之前调用。（其实只要排序尚未
 * 落盘，就可以允许，但目前似乎不需要这种延迟调
 * 用。）
 *
 * 这只是一个提示。tuplesort 仍可能返回多于请求数量
 * 的元组。主导进程并行排序总是忽略该提示。
 */
void
tuplesort_set_bound(Tuplesortstate *state, int64 bound)
{
	/* 断言在任何元组装入前被调用 */
	Assert(state->status == TSS_INITIAL && state->memtupcount == 0);
	/* 断言允许有界排序 */
	Assert(state->base.sortopt & TUPLESORT_ALLOWBOUNDED);
	/* 也不能设置两次界限 */
	Assert(!state->bounded);
	/* 另外不应在并行工作进程中调用 */
	Assert(!WORKER(state));

	/* 主导进程允许但忽略提示 */
	if (LEADER(state))
		return;

#ifdef DEBUG_BOUNDED_SORT
	/* 遵循禁用该特性的 GUC 设置（便于测试） */
	if (!optimize_bounded_sort)
		return;
#endif

	/* 我们要能计算 bound * 2，所以要限制该设置 */
	if (bound > (int64) (INT_MAX / 2))
		return;

	state->bounded = true;
	state->bound = (int) bound;

	/*
	 * 有界排序并不是缩略键优化的有效目标。
	 * 通过把状态设置为与不支持缩略一致来禁用。
	 *
	 */
	state->base.sortKeys->abbrev_converter = NULL;
	if (state->base.sortKeys->abbrev_full_comparator)
		state->base.sortKeys->comparator = state->base.sortKeys->abbrev_full_comparator;

	/* 并非严格必要，但求整洁 */
	state->base.sortKeys->abbrev_abort = NULL;
	state->base.sortKeys->abbrev_full_comparator = NULL;
}

/*
 * tuplesort_used_bound
 *
 * 允许调用者查明排序状态是否成功使用了界限。
 */
bool
tuplesort_used_bound(Tuplesortstate *state)
{
	return state->boundUsed;
}

/*
 * tuplesort_free
 *
 *	释放 tuplesort 资源的内部例程。
 */
static void
tuplesort_free(Tuplesortstate *state)
{
	/* 上下文切换多半不需要，但求稳妥 */
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

#ifdef TRACE_SORT
	long		spaceUsed;

	if (state->tapeset)
		spaceUsed = LogicalTapeSetBlocks(state->tapeset);
	else
		spaceUsed = (state->allowedMem - state->availMem + 1023) / 1024;
#endif

	/*
	 * 删除临时"磁带"文件，若有。
	 *
	 * 注：希望把它计入排序的报告总成本，因此需要两个
	 * #ifdef TRACE_SORT 段。
	 *
	 * 我们不在这里费心销毁各个磁带。它们会随排序上下
	 * 文一起消失。（TSS_FINALMERGE 状态下已关闭
	 * 完毕的磁带。）
	 */
	if (state->tapeset)
		LogicalTapeSetClose(state->tapeset);

#ifdef TRACE_SORT
	if (trace_sort)
	{
		if (state->tapeset)
			elog(LOG, "%s of worker %d ended, %ld disk blocks used: %s",
				 SERIAL(state) ? "external sort" : "parallel external sort",
				 state->worker, spaceUsed, pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "%s of worker %d ended, %ld KB used: %s",
				 SERIAL(state) ? "internal sort" : "unperformed parallel sort",
				 state->worker, spaceUsed, pg_rusage_show(&state->ru_start));
	}

	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, spaceUsed);
#else

	/*
	 * 如果禁用了 TRACE_SORT，你仍可以探测 sort__done，但拿
	 * 不到空间使用统计。
	 */
	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, 0L);
#endif

	FREESTATE(state);
	MemoryContextSwitchTo(oldcontext);

	/*
	 * 释放排序专属内存上下文，从而释放全部工作内存。
	 */
	MemoryContextReset(state->base.sortcontext);
}

/*
 * tuplesort_end
 *
 *	释放资源并清理。
 *
 * 注意：调用此函数后，tuplesort_getXXX 返回的任何指针都
 * 指向垃圾数据。当心此后不要试图使用或释放这类
 * 指针！
 */
void
tuplesort_end(Tuplesortstate *state)
{
	tuplesort_free(state);

	/*
	 * 释放主内存上下文，包括 Tuplesortstate 结构本身。
	 *
	 */
	MemoryContextDelete(state->base.maincontext);
}

/*
 * tuplesort_updatemax
 *
 *	更新最大资源使用统计信息。
 */
static void
tuplesort_updatemax(Tuplesortstate *state)
{
	int64		spaceUsed;
	bool		isSpaceDisk;

	/*
	 * 注：基于磁盘的排序似乎应同时提供内存与磁盘的
	 * 使用情况。然而，一旦我们开始向调用者返回元组，
	 * 当前代码就不再准确跟踪内存空间（因为我们没有
	 * 把调用者应做的 pfree 计入），故无法依赖
	 * 磁盘排序中的 availMem。修复它似乎不值得开销。
	 * 是否值得为内存上下文代码建一个 API 来告知我们
	 * sortcontext 的实际用量？
	 */
	if (state->tapeset)
	{
		isSpaceDisk = true;
		spaceUsed = LogicalTapeSetBlocks(state->tapeset) * BLCKSZ;
	}
	else
	{
		isSpaceDisk = false;
		spaceUsed = state->allowedMem - state->availMem;
	}

	/*
	 * 当排序无法把数据容纳进主内存时，它会把数据逐出
	 * 到磁盘。这就是我们假定磁盘占用空间比内存占用空
	 * 间对资源使用跟踪更重要的原因。注意，某个元组集
	 * 在磁盘上占用的空间量可能小于同一元组集在内存中
	 * 占用的空间量，这是因为其表示形式更加紧凑。
	 *
	 */
	if ((isSpaceDisk && !state->isMaxSpaceDisk) ||
		(isSpaceDisk == state->isMaxSpaceDisk && spaceUsed > state->maxSpace))
	{
		state->maxSpace = spaceUsed;
		state->isMaxSpaceDisk = isSpaceDisk;
		state->maxSpaceStatus = state->status;
	}
}

/*
 * tuplesort_reset
 *
 *	重置 tuplesort。重置 tuplesort 内的所有数据，但保留
 *	元信息。tuplesort_reset 之后，tuplesort 即可开始新的排
 *	序。这在排序多个小批次时避免了重建元组排序状态
 *	（从而节省资源）。
 */
void
tuplesort_reset(Tuplesortstate *state)
{
	tuplesort_updatemax(state);
	tuplesort_free(state);

	/*
	 * 释放完每批内存后，重新建立第一批和任何后续批次
	 * 共有的全部状态。
	 */
	tuplesort_begin_batch(state);

	state->lastReturnedTuple = NULL;
	state->slabMemoryBegin = NULL;
	state->slabMemoryEnd = NULL;
	state->slabFreeHead = NULL;
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
grow_memtuples(Tuplesortstate *state)
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
		 * 超出 availMem / sizeof(SortTuple) 个元素。实际上我们
		 * 希望增量远小于此，因为需要为
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
	if ((Size) newmemtupsize >= MaxAllocHugeSize / sizeof(SortTuple))
	{
		newmemtupsize = (int) (MaxAllocHugeSize / sizeof(SortTuple));
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
	if (state->availMem < (int64) ((newmemtupsize - memtupsize) * sizeof(SortTuple)))
		goto noalloc;

	/* 就做 */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	state->memtupsize = newmemtupsize;
	state->memtuples = (SortTuple *)
		repalloc_huge(state->memtuples,
					  state->memtupsize * sizeof(SortTuple));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));
	if (LACKMEM(state))
		elog(ERROR, "unexpected out-of-memory situation in tuplesort");
	return true;

noalloc:
	/* 若因任何原因未 realloc，则关闭后续尝试 */
	state->growmemtuples = false;
	return false;
}

/*
 * 元组与 datum 情形共用代码。
 */
void
tuplesort_puttuple_common(Tuplesortstate *state, SortTuple *tuple, bool useAbbrev)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(!LEADER(state));

	/* 统计行外数据的大小 */
	if (tuple->tuple != NULL)
		USEMEM(state, GetMemoryChunkSpace(tuple->tuple));

	if (!useAbbrev)
	{
		/*
		 * 保留普通 Datum 表示或 NULL 值不变。若有转换器，
		 * 它不会预期 NULL 值，成本模型也不要求计及
		 * NULL，故此情形下我们避免调用转换器，只把
		 * datum1 设为全零表示（保持一致，且支持
		 * NULL 缩略键的廉价不等测试）。
		 *
		 */
	}
	else if (!consider_abort_common(state))
	{
		/* 保存缩略键表示 */
		tuple->datum1 = state->base.sortKeys->abbrev_converter(tuple->datum1,
															   state->base.sortKeys);
	}
	else
	{
		/*
		 * 把状态设置为永不尝试缩略的一致形态。
		 *
		 * 修改已复制元组中的 datum1 表示，以确保表示
		 * 一致（当前元组刚处理过）。即使有已转储
		 * 元组已在磁带上排好序也没关系，序列化元组
		 * 不含缩略键（TSS_BUILDRUNS 状态反正会阻止
		 * 控制流到达此处）。
		 */
		REMOVEABBREV(state, state->memtuples, state->memtupcount);
	}

	switch (state->status)
	{
		case TSS_INITIAL:

			/*
			 * 将元组存入未排序数组。先按需增长数组。
			 * 注意还剩一个空闲槽位时我们就尝试增长数组
			 * —— 若失败，仍有空位存新元组，随后我们会
			 * 切换到基于磁带的操作。
			 *
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				(void) grow_memtuples(state);
				Assert(state->memtupcount < state->memtupsize);
			}
			state->memtuples[state->memtupcount++] = *tuple;

			/*
			 * 检查是否该改用有界堆排序。当输入元组数超
			 * 过所需元组数的两倍，或刚填满 workMem
			 * 且元组数足以满足界限时这么做。（这是
			 * 对堆何时比快排更便宜的启发式判断）。
			 *
			 *
			 * 注意，进入 TSS_BOUNDED 状态后我们总尝试以该方
			 * 式完成排序。最坏时，若后来的输入元组
			 * 大于先前元组，我们可能大幅超出 workMem。
			 *
			 */
			if (state->bounded &&
				(state->memtupcount > state->bound * 2 ||
				 (state->memtupcount > state->bound && LACKMEM(state))))
			{
#ifdef TRACE_SORT
				if (trace_sort)
					elog(LOG, "switching to bounded heapsort at %d tuples: %s",
						 state->memtupcount,
						 pg_rusage_show(&state->ru_start));
#endif
				make_bounded_heap(state);
				MemoryContextSwitchTo(oldcontext);
				return;
			}

			/*
			 * 若仍能放入可用内存且拥有数组槽位则完成。
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
			{
				MemoryContextSwitchTo(oldcontext);
				return;
			}

			/*
			 * 不；该切换到基于磁带的操作了。
			 */
			inittapes(state, true);

			/*
			 * 全部转储。
			 */
			dumptuples(state, false);
			break;

		case TSS_BOUNDED:

			/*
			 * 我们不打算在此增长数组，故先看新元组是否
			 * 能在放入之前就被丢弃。这应也是不错的提速
			 * 优化：输入元组远多于界限时，多数元组仅凭
			 * 一次比较即可丢弃。
			 * 注意，由于当前排序方向已反转，必须检查
			 *  <= 而不是 >=。
			 */
			if (COMPARETUP(state, tuple, &state->memtuples[0]) <= 0)
			{
				/* 新元组 <= 堆顶，故可丢弃 */
				free_sort_tuple(state, tuple);
				CHECK_FOR_INTERRUPTS();
			}
			else
			{
				/* 丢弃堆顶，用新元组替换它 */
				free_sort_tuple(state, &state->memtuples[0]);
				tuplesort_heap_replace_top(state, tuple);
			}
			break;

		case TSS_BUILDRUNS:

			/*
			 * 把元组存入未排序数组（必定有空间）
			 */
			state->memtuples[state->memtupcount++] = *tuple;

			/*
			 * 如果超出内存限额，转储全部元组。
			 */
			dumptuples(state, false);
			break;

		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}
	MemoryContextSwitchTo(oldcontext);
}

static bool
consider_abort_common(Tuplesortstate *state)
{
	Assert(state->base.sortKeys[0].abbrev_converter != NULL);
	Assert(state->base.sortKeys[0].abbrev_abort != NULL);
	Assert(state->base.sortKeys[0].abbrev_full_comparator != NULL);

	/*
	 * 检查缩略优化效果。还在内存限额内时考虑中止。
	 *
	 */
	if (state->status == TSS_INITIAL &&
		state->memtupcount >= state->abbrevNext)
	{
		state->abbrevNext *= 2;

		/*
		 * 检查 opclass 给出的缩略中止例程。它可能表示不该
		 * 继续缩略。
		 */
		if (!state->base.sortKeys->abbrev_abort(state->memtupcount,
												state->base.sortKeys))
			return false;

		/*
		 * 最后恢复权威比较器，并将 abbrev_converter 设
		 * 为 NULL 来表明缩略不在生效。
		 */
		state->base.sortKeys[0].comparator = state->base.sortKeys[0].abbrev_full_comparator;
		state->base.sortKeys[0].abbrev_converter = NULL;
		/* 并非严格必要，但求整洁 */
		state->base.sortKeys[0].abbrev_abort = NULL;
		state->base.sortKeys[0].abbrev_full_comparator = NULL;

		/* 放弃 —— 预期原本的按值传递表示 */
		return true;
	}

	return false;
}

/*
 * 所有元组均已提供；完成排序。
 */
void
tuplesort_performsort(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "performsort of worker %d starting: %s",
			 state->worker, pg_rusage_show(&state->ru_start));
#endif

	switch (state->status)
	{
		case TSS_INITIAL:

			/*
			 * 我们能在允许内存量内积累全部元组，或主导
			 * 进程接管工作进程磁带。
			 */
			if (SERIAL(state))
			{
				/* 直接 qsort 即完成 */
				tuplesort_sort_memtuples(state);
				state->status = TSS_SORTEDINMEM;
			}
			else if (WORKER(state))
			{
				/*
				 * 并行工作进程仍须转储元组到磁带。不过
				 * 要产出单一输出归并段则无需归并。
				 */
				inittapes(state, false);
				dumptuples(state, true);
				worker_nomergeruns(state);
				state->status = TSS_SORTEDONTAPE;
			}
			else
			{
				/*
				 * 主导进程将接管工作进程磁带并归并各段。
				 * 注意 mergeruns 会设正确的 state->status。
				 */
				leader_takeover_tapes(state);
				mergeruns(state);
			}
			state->current = 0;
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;

		case TSS_BOUNDED:

			/*
			 * 我们能在内存中用堆淘汰多余元组，积累输出
			 * 所需元组。接着把堆变为排好序的数组。
			 * 注意 sort_bounded_heap 会设正确的 state->status。
			 *
			 */
			sort_bounded_heap(state);
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;

		case TSS_BUILDRUNS:

			/*
			 * 完成基于磁带的排序。先把内存中剩余元组
			 * 刷写到磁带；然后归并至只剩一段（或者，当
			 * !randomAccess 且 !WORKER() 时，每条磁带一段）。
			 * 注意 mergeruns 会设正确的 state->status。
			 */
			dumptuples(state, true);
			mergeruns(state);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;

		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

#ifdef TRACE_SORT
	if (trace_sort)
	{
		if (state->status == TSS_FINALMERGE)
			elog(LOG, "performsort of worker %d done (except %d-way final merge): %s",
				 state->worker, state->nInputTapes,
				 pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "performsort of worker %d done: %s",
				 state->worker, pg_rusage_show(&state->ru_start));
	}
#endif

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 内部例程：以正向或反向把下一个元组取入
 *  *stup。若已无更多元组则返回假。
 * 返回的元组属于 tuplesort 内存上下文，调用者不得释放
 * 它。注意取到的元组存于可能被未来任何一次取
 * 组回收的内存中。
 */
bool
tuplesort_gettuple_common(Tuplesortstate *state, bool forward,
						  SortTuple *stup)
{
	unsigned int tuplen;
	size_t		nmoved;

	Assert(!WORKER(state));

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			Assert(forward || state->base.sortopt & TUPLESORT_RANDOMACCESS);
			Assert(!state->slabAllocatorUsed);
			if (forward)
			{
				if (state->current < state->memtupcount)
				{
					*stup = state->memtuples[state->current++];
					return true;
				}
				state->eof_reached = true;

				/*
				 * 若有界排序中调用者试图取的元组超过
				 * 最初请求的量则报错。这是因为此处返回
				 * EOF 可能是错误做法。
				 */
				if (state->bounded && state->current >= state->bound)
					elog(ERROR, "retrieved too many tuples in a bounded sort");

				return false;
			}
			else
			{
				if (state->current <= 0)
					return false;

				/*
				 * 若所有元组已取完，则返回最后
				 * 元组，否则返回前一个元组。
				 */
				if (state->eof_reached)
					state->eof_reached = false;
				else
				{
					state->current--;	/* 上次返回元组 */
					if (state->current <= 0)
						return false;
				}
				*stup = state->memtuples[state->current - 1];
				return true;
			}
			break;

		case TSS_SORTEDONTAPE:
			Assert(forward || state->base.sortopt & TUPLESORT_RANDOMACCESS);
			Assert(state->slabAllocatorUsed);

			/*
			 * 上一次 gettuple 调用里所返回元组的槽位
			 * 现在可以重用了。
			 */
			if (state->lastReturnedTuple)
			{
				RELEASE_SLAB_SLOT(state, state->lastReturnedTuple);
				state->lastReturnedTuple = NULL;
			}

			if (forward)
			{
				if (state->eof_reached)
					return false;

				if ((tuplen = getlen(state->result_tape, true)) != 0)
				{
					READTUP(state, stup, state->result_tape, tuplen);

					/*
					 * 记住所返回元组，以便下次调用回收
					 * 其内存。（在 !state->tuples 情形下可为
					 * NULL）。
					 */
					state->lastReturnedTuple = stup->tuple;

					return true;
				}
				else
				{
					state->eof_reached = true;
					return false;
				}
			}

			/*
			 * 反向。
			 *
			 * 若所有元组已取完，则返回最后返回的元组，
			 * 否则返回前一个元组。
			 */
			if (state->eof_reached)
			{
				/*
				 * 寻道位置正好位于文件末尾零 tuplen 之后，
				 * 需回退取最后元组的结尾长度字。寻道失
				 * 败则文件必然完全为空。
				 */
				nmoved = LogicalTapeBackspace(state->result_tape,
											  2 * sizeof(unsigned int));
				if (nmoved == 0)
					return false;
				else if (nmoved != 2 * sizeof(unsigned int))
					elog(ERROR, "unexpected tape position");
				state->eof_reached = false;
			}
			else
			{
				/*
				 * 回退并取上次返回元组的结尾长度
				 * 字。寻道失败则假定在文件开头。
				 */
				nmoved = LogicalTapeBackspace(state->result_tape,
											  sizeof(unsigned int));
				if (nmoved == 0)
					return false;
				else if (nmoved != sizeof(unsigned int))
					elog(ERROR, "unexpected tape position");
				tuplen = getlen(state->result_tape, false);

				/*
				 * 回退取它前面元组的结尾长度字。
				 */
				nmoved = LogicalTapeBackspace(state->result_tape,
											  tuplen + 2 * sizeof(unsigned int));
				if (nmoved == tuplen + sizeof(unsigned int))
				{
					/*
					 * 我们回退越过了前一元组，但其前面没有
					 * 结尾长度字。即前一元组是文件中的第一
					 * 个元组。现在是正向读取的下一个元组
					 * （未必显然正确，但内存中的情形
					 * 就是这样）。
					 */
					return false;
				}
				else if (nmoved != tuplen + 2 * sizeof(unsigned int))
					elog(ERROR, "bogus tuple length in backward scan");
			}

			tuplen = getlen(state->result_tape, false);

			/*
			 * 现在我们有了前一元组的长度，回退并读取它。
			 * 注：READTUP 预期我们位于元组初始长度字
			 * 之后，所以回退到该位置。
			 */
			nmoved = LogicalTapeBackspace(state->result_tape,
										  tuplen);
			if (nmoved != tuplen)
				elog(ERROR, "bogus tuple length in backward scan");
			READTUP(state, stup, state->result_tape, tuplen);

			/*
			 * 记住我们返回的元组，以便下次调用时回收
			 * 其内存。（Datum 情形下可为 NULL）。
			 */
			state->lastReturnedTuple = stup->tuple;

			return true;

		case TSS_FINALMERGE:
			Assert(forward);
			/* 我们正用 slab 分配器自行管理内存 */
			Assert(state->slabAllocatorUsed);

			/*
			 * 上次 gettuple 调用所返回元组所在的 slab 槽位
			 * 现在可以重用了。
			 */
			if (state->lastReturnedTuple)
			{
				RELEASE_SLAB_SLOT(state, state->lastReturnedTuple);
				state->lastReturnedTuple = NULL;
			}

			/*
			 * 此代码应与 mergeonerun() 的内层循环一致。
			 */
			if (state->memtupcount > 0)
			{
				int			srcTapeIndex = state->memtuples[0].srctape;
				LogicalTape *srcTape = state->inputTapes[srcTapeIndex];
				SortTuple	newtup;

				*stup = state->memtuples[0];

				/*
				 * 记住我们返回的元组，以便下次调用回收
				 * 其内存。（Datum 情形下可为 NULL）。
				 */
				state->lastReturnedTuple = stup->tuple;

				/*
				 * 从磁带取下个元组，用它替换堆顶元组。
				 *
				 */
				if (!mergereadnext(state, srcTape, &newtup))
				{
					/*
					 * 若无更多数据，即已到磁带归并段末端，
					 * 从堆中移除顶节点。
					 */
					tuplesort_heap_delete_top(state);
					state->nInputRuns--;

					/*
					 * 关闭磁带。它本会在排序结束时消失，
					 * 但尽早释放内存更好。
					 */
					LogicalTapeClose(srcTape);
					return true;
				}
				newtup.srctape = srcTapeIndex;
				tuplesort_heap_replace_top(state, &newtup);
				return true;
			}
			return false;

		default:
			elog(ERROR, "invalid tuplesort state");
			return false;		/* 让编译器安静 */
	}
}


/*
 * 沿正向或反向任一方向跳过 N 个元组，
 * 不返回任何数据。N==0 是无操作。
 * 成功则返回真，元组用尽则返回假。
 */
bool
tuplesort_skiptuples(Tuplesortstate *state, int64 ntuples, bool forward)
{
	MemoryContext oldcontext;

	/*
	 * 我们其实尚不支持向后跳过，因为还没有调用者需要
	 * 它。但 API 的设计允许日后支持该功能。
	 */
	Assert(forward);
	Assert(ntuples >= 0);
	Assert(!WORKER(state));

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			if (state->memtupcount - state->current >= ntuples)
			{
				state->current += ntuples;
				return true;
			}
			state->current = state->memtupcount;
			state->eof_reached = true;

			/*
			 * 若有界排序中调用者试图取的元组超过
			 * 最初请求的量则报错。这是因为此处返回
			 * EOF 可能是错误做法。
			 */
			if (state->bounded && state->current >= state->bound)
				elog(ERROR, "retrieved too many tuples in a bounded sort");

			return false;

		case TSS_SORTEDONTAPE:
		case TSS_FINALMERGE:

			/*
			 * 这些情形或许还能再优化，但眼下不值得费
			 * 这功夫。
			 */
			oldcontext = MemoryContextSwitchTo(state->base.sortcontext);
			while (ntuples-- > 0)
			{
				SortTuple	stup;

				if (!tuplesort_gettuple_common(state, forward, &stup))
				{
					MemoryContextSwitchTo(oldcontext);
					return false;
				}
				CHECK_FOR_INTERRUPTS();
			}
			MemoryContextSwitchTo(oldcontext);
			return true;

		default:
			elog(ERROR, "invalid tuplesort state");
			return false;		/* 让编译器安静 */
	}
}

/*
 * tuplesort_merge_order - 报告对给定内存将采用的归并路数
 * （注意："归并路数"就是归并中输入磁带的数量）。
 *
 * 此函数导出供规划器使用。allowedMem 以字节计。
 */
int
tuplesort_merge_order(int64 allowedMem)
{
	int			mOrder;

	/*----------
	 * 在归并阶段，我们需要为每条输入和输出磁带提供缓
	 * 冲区空间。平衡归并算法的每轮都从 M 条输入磁带读
	 * 取，向 N 条输出磁带写入。每条磁带消耗
	 * TAPE_BUFFER_OVERHEAD 字节内存；此外每条输入磁带还需一
	 * 份。
	 *
	 * totalMem = M * (TAPE_BUFFER_OVERHEAD + MERGE_BUFFER_SIZE) +
	 *            N * TAPE_BUFFER_OVERHEAD
	 *
	 * 除了最后和倒数第二轮归并（磁带可能较少）
	 * 之外，M = N。我们选择 M，使得输入缓冲区（
	 * TAPE_BUFFER_OVERHEAD+MERGE_BUFFER_SIZE）可用内存，
	 * 磁带缓冲区的总内存量为 allowedMem。
	 *
	 *
	 * 注：你或许想计入 memtuples[] 数组，但我们实际把
	 * 它当作 MERGE_BUFFER_SIZE 工作区的一部分。
	 *
	 *----------
	 */
	mOrder = allowedMem /
		(2 * TAPE_BUFFER_OVERHEAD + MERGE_BUFFER_SIZE);

	/*
	 * 即使在最小内存下，也要至少用 MINORDER 归并。另
	 * 一方面，即使内存充裕，也不要使用超过 MAXORDER 的
	 * 归并。磁带相当便宜，但非完全免费。每增加一条
	 * 磁带都会减少可用于构建归并段的内存量，这又可能
	 * 使同一排序需更多归并段，即使归并仍能单
	 * 轮完成，归并也会变慢。另外，高路数归并因 CPU
	 * 缓存效应而相当慢；执行多轮归并的 I/O 成本可能比
	 * 单轮跨越数百条磁带的归并更划算。
	 *
	 */
	mOrder = Max(mOrder, MINORDER);
	mOrder = Min(mOrder, MAXORDER);

	return mOrder;
}

/*
 * 辅助函数：计算在归并轮中为每条输入磁带的读缓冲
 * 区分配多少内存。
 *
 * 'avail_mem' 是可供所有磁带（输入和输出）的缓冲区使
 *		用的内存量。
 * 'nInputTapes' 与 'nInputRuns' 是输入磁带和归并段的数量。
 * 'maxOutputTapes' 是我们应产出的输出磁带数量上限。
 */
static int64
merge_read_buffer_size(int64 avail_mem, int nInputTapes, int nInputRuns,
					   int maxOutputTapes)
{
	int			nOutputRuns;
	int			nOutputTapes;

	/*
	 * 这一轮我们将产出多少条输出磁带？
	 *
	 * 即 nInputRuns / nInputTapes 向上取整。
	 */
	nOutputRuns = (nInputRuns + nInputTapes - 1) / nInputTapes;

	nOutputTapes = Min(nOutputRuns, maxOutputTapes);

	/*
	 * 每条输出磁带消耗 TAPE_BUFFER_OVERHEAD 字节内存。全部
	 * 剩余内存则在输入磁带间平均划分。
	 *
	 * 这也可由 tuplesort_merge_order 中的公式推出，但这里是
	 * 从可用内存量推算输入缓冲区大小，并结合
	 * M 与 N。
	 */
	return Max((avail_mem - TAPE_BUFFER_OVERHEAD * nOutputTapes) / nInputTapes, 0);
}

/*
 * inittapes - 初始化磁带排序。
 *
 * 只有发现无法在内存中排序时才调用此函数。
 */
static void
inittapes(Tuplesortstate *state, bool mergeruns)
{
	Assert(!LEADER(state));

	if (mergeruns)
	{
		/* 计算归并时要使用的输入磁带数量 */
		state->maxTapes = tuplesort_merge_order(state->allowedMem);
	}
	else
	{
		/* 工作进程有时只产出一个归并段，直接输出 */
		Assert(WORKER(state));
		state->maxTapes = MINORDER;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d switching to external sort with %d tapes: %s",
			 state->worker, state->maxTapes, pg_rusage_show(&state->ru_start));
#endif

	/* 创建磁带集 */
	inittapestate(state, state->maxTapes);
	state->tapeset =
		LogicalTapeSetCreate(false,
							 state->shared ? &state->shared->fileset : NULL,
							 state->worker);

	state->currentRun = 0;

	/*
	 * 初始化逻辑磁带数组。
	 */
	state->inputTapes = NULL;
	state->nInputTapes = 0;
	state->nInputRuns = 0;

	state->outputTapes = palloc0(state->maxTapes * sizeof(LogicalTape *));
	state->nOutputTapes = 0;
	state->nOutputRuns = 0;

	state->status = TSS_BUILDRUNS;

	selectnewtape(state);
}

/*
 * inittapestate - 初始化通用磁带管理状态
 */
static void
inittapestate(Tuplesortstate *state, int maxTapes)
{
	int64		tapeSpace;

	/*
	 * 调低 availMem 以反映磁带缓冲区所需空间；但别减到
	 * 没有元组容身之地的程度。（那种情况只可能出现在
	 * 按值传递 Datum 排序时；其他所有情形中 memtuples[] 数
	 * 组几乎不可能占据超过 allowedMem 一半的空间。按值传
	 * 递情形下计入元组空间并不重要，所以我们不在乎
	 * LACKMEM 变得不准确。
	 *
	 */
	tapeSpace = (int64) maxTapes * TAPE_BUFFER_OVERHEAD;

	if (tapeSpace + GetMemoryChunkSpace(state->memtuples) < state->allowedMem)
		USEMEM(state, tapeSpace);

	/*
	 * 确保磁带集底层文件(file)创建于合适的临时表空间
	 * 中。对于并行排序，本应已经调用过，
	 * 再调用一次也无妨。
	 */
	PrepareTempTablespaces();
}

/*
 * selectnewtape -- 选择下个输出磁带。
 *
 * 完成一个归并段且还须开始另一个之后调用。
 * 构建初始归并段期间和归并轮期间都会用到。
 *
 */
static void
selectnewtape(Tuplesortstate *state)
{
	/*
	 * 每轮归并开始时，nOutputTapes 与 nOutputRuns 均为 0。每
	 * 次调用时，我们都创建新输出磁带来容纳下一个
	 * 归并段，直到 maxTapes。此后以轮转方式把新归
	 * 并段分配给现有磁带。
	 */
	if (state->nOutputTapes < state->maxTapes)
	{
		/* 创建新磁带容纳下一段 */
		Assert(state->outputTapes[state->nOutputRuns] == NULL);
		Assert(state->nOutputRuns == state->nOutputTapes);
		state->destTape = LogicalTapeCreate(state->tapeset);
		state->outputTapes[state->nOutputTapes] = state->destTape;
		state->nOutputTapes++;
		state->nOutputRuns++;
	}
	else
	{
		/*
		 * 已达到磁带数上限。追加到一条现有磁带上。
		 *
		 */
		state->destTape = state->outputTapes[state->nOutputRuns % state->nOutputTapes];
		state->nOutputRuns++;
	}
}

/*
 * 按给定槽位数量初始化 slab 分配区。
 */
static void
init_slab_allocator(Tuplesortstate *state, int numSlots)
{
	if (numSlots > 0)
	{
		char	   *p;
		int			i;

		state->slabMemoryBegin = palloc(numSlots * SLAB_SLOT_SIZE);
		state->slabMemoryEnd = state->slabMemoryBegin +
			numSlots * SLAB_SLOT_SIZE;
		state->slabFreeHead = (SlabSlot *) state->slabMemoryBegin;
		USEMEM(state, numSlots * SLAB_SLOT_SIZE);

		p = state->slabMemoryBegin;
		for (i = 0; i < numSlots - 1; i++)
		{
			((SlabSlot *) p)->nextfree = (SlabSlot *) (p + SLAB_SLOT_SIZE);
			p += SLAB_SLOT_SIZE;
		}
		((SlabSlot *) p)->nextfree = NULL;
	}
	else
	{
		state->slabMemoryBegin = state->slabMemoryEnd = NULL;
		state->slabFreeHead = NULL;
	}
	state->slabAllocatorUsed = true;
}

/*
 * mergeruns -- 归并全部已完成的初始段。
 *
 * 这实现的即平衡 k 路归并算法。全部输入数据都已写
 * 入磁带上的初始归并段（见 dumptuples）。
 */
static void
mergeruns(Tuplesortstate *state)
{
	int			tapenum;

	Assert(state->status == TSS_BUILDRUNS);
	Assert(state->memtupcount == 0);

	if (state->base.sortKeys != NULL && state->base.sortKeys->abbrev_converter != NULL)
	{
		/*
		 * 如果待归并的归并段有多个，读回磁盘元组时
		 * 将没有存储缩略键，也不打算重新生成，
		 * 所以从此刻起禁用缩略。
		 *
		 */
		state->base.sortKeys->abbrev_converter = NULL;
		state->base.sortKeys->comparator = state->base.sortKeys->abbrev_full_comparator;

		/* 并非严格必要，但求整洁 */
		state->base.sortKeys->abbrev_abort = NULL;
		state->base.sortKeys->abbrev_full_comparator = NULL;
	}

	/*
	 * 重置元组内存。先前分配的元组均已释放，此后
	 * 将改用 slab 分配器。
	 */
	MemoryContextResetOnly(state->base.tuplecontext);

	/*
	 * 我们不再需要大型 memtuples 数组。（稍后会为堆分配一
	 * 个较小的。）
	 */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	pfree(state->memtuples);
	state->memtuples = NULL;

	/*
	 * 初始化 slab 分配器。每条输入磁带一个 slab 槽位，
	 * 用于堆中的元组，外加一个用于容纳最后经
	 * tuplesort_gettuple 返回者。（若排序按值传
	 * 递 Datum，则无需任何分配。）
	 *
	 * 在多轮归并中，若末轮磁带少于之前各轮，本可缩
	 * 小此分配，但我们不打算费这个事。
	 *
	 *
	 * 从此时起，我们不再使用 USEMEM()/LACKMEM() 机制来跟踪
	 * 单个元组的内存使用。
	 */
	if (state->base.tuples)
		init_slab_allocator(state, state->nOutputTapes + 1);
	else
		init_slab_allocator(state, 0);

	/*
	 * 为堆分配新的 memtuples 数组。它将容纳每条输入磁带
	 * 的一个元组。
	 *
	 * 在多轮归并的各轮间我们本可同样缩小它，但我们
	 * 不费这个事。（初始输入磁带仍在 outputTapes 中。
	 * 各轮之间输入磁带数不会增加。）
	 */
	state->memtupsize = state->nOutputTapes;
	state->memtuples = (SortTuple *) MemoryContextAlloc(state->base.maincontext,
														state->nOutputTapes * sizeof(SortTuple));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	/*
	 * 把剩余的全部可用磁带缓冲区内存分配给全部输入磁
	 * 带。每轮归并开始时，会把它分给该轮输入
	 * 和输出磁带。
	 */
	state->tape_buffer_mem = state->availMem;
	USEMEM(state, state->tape_buffer_mem);
#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d using %zu KB of memory for tape buffers",
			 state->worker, state->tape_buffer_mem / 1024);
#endif

	for (;;)
	{
		/*
		 * 首次迭代时，或多轮归并读完各输入磁带
		 * 归并段时，就应开始新一轮。回卷输出磁带，
		 * 让它们作为下一轮的输入。
		 *
		 */
		if (state->nInputRuns == 0)
		{
			int64		input_buffer_size;

			/* 关闭已排空的旧输入磁带 */
			if (state->nInputTapes > 0)
			{
				for (tapenum = 0; tapenum < state->nInputTapes; tapenum++)
					LogicalTapeClose(state->inputTapes[tapenum]);
				pfree(state->inputTapes);
			}

			/* 上一轮的输出成为下一轮的输入。 */
			state->inputTapes = state->outputTapes;
			state->nInputTapes = state->nOutputTapes;
			state->nInputRuns = state->nOutputRuns;

			/*
			 * 重置输出磁带变量。LogicalTapes 会按需创建，
			 * 这里我们只为容纳它们分配数组。
			 *
			 */
			state->outputTapes = palloc0(state->nInputTapes * sizeof(LogicalTape *));
			state->nOutputTapes = 0;
			state->nOutputRuns = 0;

			/*
			 * 在新输入/输出磁带间重新分配磁带缓冲内存。
			 *
			 */
			input_buffer_size = merge_read_buffer_size(state->tape_buffer_mem,
													   state->nInputTapes,
													   state->nInputRuns,
													   state->maxTapes);

#ifdef TRACE_SORT
			if (trace_sort)
				elog(LOG, "starting merge pass of %d input runs on %d tapes, " INT64_FORMAT " KB of memory for each input tape: %s",
					 state->nInputRuns, state->nInputTapes, input_buffer_size / 1024,
					 pg_rusage_show(&state->ru_start));
#endif

			/* 为新归并轮准备新的输入磁带。 */
			for (tapenum = 0; tapenum < state->nInputTapes; tapenum++)
				LogicalTapeRewindForRead(state->inputTapes[tapenum], input_buffer_size);

			/*
			 * 若每条输入磁带只剩一段，则只剩最后一轮
			 * 归并。若无需产出物化的已排序磁带，就可
			 * 在此停下，即时进行最终归并。
			 *
			 */
			if ((state->base.sortopt & TUPLESORT_RANDOMACCESS) == 0
				&& state->nInputRuns <= state->nInputTapes
				&& !WORKER(state))
			{
				/* 告知 logtape.c 不再写入 */
				LogicalTapeSetForgetFreeSpace(state->tapeset);
				/* 为最终归并轮做初始化 */
				beginmerge(state);
				state->status = TSS_FINALMERGE;
				return;
			}
		}

		/* 选择输出磁带 */
		selectnewtape(state);

		/* 每条输入磁带归并一段。 */
		mergeonerun(state);

		/*
		 * 若输入磁带已空，且只产出一个输出归并段，那
		 * 就完成了。当前输出磁带上就是最终结果。
		 */
		if (state->nInputRuns == 0 && state->nOutputRuns <= 1)
			break;
	}

	/*
	 * 完成。结果在单条磁带的单个归并段上。
	 */
	state->result_tape = state->outputTapes[0];
	if (!WORKER(state))
		LogicalTapeFreeze(state->result_tape, NULL);
	else
		worker_freeze_result_tape(state);
	state->status = TSS_SORTEDONTAPE;

	/* 关闭现已排空的全部输入磁带，释放其读缓冲区。 */
	for (tapenum = 0; tapenum < state->nInputTapes; tapenum++)
		LogicalTapeClose(state->inputTapes[tapenum]);
}

/*
 * 每条输入磁带归并一段。
 */
static void
mergeonerun(Tuplesortstate *state)
{
	int			srcTapeIndex;
	LogicalTape *srcTape;

	/*
	 * 开始归并：从每条活动源磁带向堆中载入一个元组。
	 *
	 */
	beginmerge(state);

	Assert(state->slabAllocatorUsed);

	/*
	 * 执行归并：反复取出堆中最小元组，写出并用同
	 * 一磁带上的下一元组替换它（如果该磁带
	 * 还有）。
	 */
	while (state->memtupcount > 0)
	{
		SortTuple	stup;

		/* 把元组写入 destTape */
		srcTapeIndex = state->memtuples[0].srctape;
		srcTape = state->inputTapes[srcTapeIndex];
		WRITETUP(state, state->destTape, &state->memtuples[0]);

		/* 回收刚写出的元组的槽位，供下一次读取使用 */
		if (state->memtuples[0].tuple)
			RELEASE_SLAB_SLOT(state, state->memtuples[0].tuple);

		/*
		 * 从磁带拉取下一个元组，替换堆中刚写出
		 * 的元组。
		 */
		if (mergereadnext(state, srcTape, &stup))
		{
			stup.srctape = srcTapeIndex;
			tuplesort_heap_replace_top(state, &stup);
		}
		else
		{
			tuplesort_heap_delete_top(state);
			state->nInputRuns--;
		}
	}

	/*
	 * 堆变空即告完成。在输出磁带上写归并段结束标记。
	 *
	 */
	markrunend(state->destTape);
}

/*
 * beginmerge - 为归并轮做初始化
 *
 * 用每条输入磁带的首个元组填充归并堆。
 */
static void
beginmerge(Tuplesortstate *state)
{
	int			activeTapes;
	int			srcTapeIndex;

	/* 此处堆应为空 */
	Assert(state->memtupcount == 0);

	activeTapes = Min(state->nInputTapes, state->nInputRuns);

	for (srcTapeIndex = 0; srcTapeIndex < activeTapes; srcTapeIndex++)
	{
		SortTuple	tup;

		if (mergereadnext(state, state->inputTapes[srcTapeIndex], &tup))
		{
			tup.srctape = srcTapeIndex;
			tuplesort_heap_insert(state, &tup);
		}
	}
}

/*
 * mergereadnext - 读归并输入磁带的下一个元组
 *
 * EOF 时返回假。
 */
static bool
mergereadnext(Tuplesortstate *state, LogicalTape *srcTape, SortTuple *stup)
{
	unsigned int tuplen;

	/* 读下个元组 */
	if ((tuplen = getlen(srcTape, true)) == 0)
		return false;
	READTUP(state, stup, srcTape, tuplen);

	return true;
}

/*
 * dumptuples - 从 memtuples 移除元组并把归并段写入磁带
 *
 * 当 alltuples = true 时，转储内存中现有的一切。（此情形
 * 只在输入末尾使用。）
 */
static void
dumptuples(Tuplesortstate *state, bool alltuples)
{
	int			memtupwrite;
	int			i;

	/*
	 * 若仍能容纳于可用内存且有数组槽位，则无事可做，
	 * 除非是初始归并段生成期间的最后一次调用。
	 */
	if (state->memtupcount < state->memtupsize && !LACKMEM(state) &&
		!alltuples)
		return;

	/*
	 * 最后一次调用可能无需排序：极少数情形下，我们
	 * 恰好在全部剩余元组都已装入内存、输入即将耗尽
	 * 之前就 LACKMEM() 过。
	 * 一般而言，较短的末段完全可能出现，但要避免
	 * 创建完全空的归并段。但在工作进程中必须至少
	 * 产出一条磁带，哪怕为空。
	 */
	if (state->memtupcount == 0 && state->currentRun > 0)
		return;

	Assert(state->status == TSS_BUILDRUNS);

	/*
	 * 这一上限似乎不可能超出，但我们不冒险。
	 *
	 */
	if (state->currentRun == INT_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot have more than %d runs for an external sort",
						INT_MAX)));

	if (state->currentRun > 0)
		selectnewtape(state);

	state->currentRun++;

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d starting quicksort of run %d: %s",
			 state->worker, state->currentRun,
			 pg_rusage_show(&state->ru_start));
#endif

	/*
	 * 用快速排序对该归并段在允许内存量内积累的全部
	 * 元组进行排序。
	 */
	tuplesort_sort_memtuples(state);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d finished quicksort of run %d: %s",
			 state->worker, state->currentRun,
			 pg_rusage_show(&state->ru_start));
#endif

	memtupwrite = state->memtupcount;
	for (i = 0; i < memtupwrite; i++)
	{
		SortTuple  *stup = &state->memtuples[i];

		WRITETUP(state, state->destTape, stup);

		/*
		 * 把释放元组计入账，但不必真 pfree，循环后会重
		 * 置 tuplecontext。
		 */
		if (stup->tuple != NULL)
			FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
	}

	state->memtupcount = 0;

	/*
	 * 重置元组内存。先前分配的元组均已全部释放。
	 * 当传入元组的大小发生剧烈变化时，避免碎片化很
	 * 重要。若不做这一步，AllocSetFree 按大小类
	 * 别分桶造成的碎片化可能特别严重。
	 *
	 */
	MemoryContextReset(state->base.tuplecontext);

	markrunend(state->destTape);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "worker %d finished writing run %d to tape %d: %s",
			 state->worker, state->currentRun, (state->currentRun - 1) % state->nOutputTapes + 1,
			 pg_rusage_show(&state->ru_start));
#endif
}

/*
 * tuplesort_rescan		- 回卷并重放扫描
 */
void
tuplesort_rescan(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(state->base.sortopt & TUPLESORT_RANDOMACCESS);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeRewindForRead(state->result_tape, 0);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_markpos	- 保存归并排序文件中的当前位置
 */
void
tuplesort_markpos(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(state->base.sortopt & TUPLESORT_RANDOMACCESS);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->markpos_offset = state->current;
			state->markpos_eof = state->eof_reached;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeTell(state->result_tape,
							&state->markpos_block,
							&state->markpos_offset);
			state->markpos_eof = state->eof_reached;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_restorepos - 把归并排序文件中当前位置恢复
 *						  为上次保存点
 */
void
tuplesort_restorepos(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->base.sortcontext);

	Assert(state->base.sortopt & TUPLESORT_RANDOMACCESS);

	switch (state->status)
	{
		case TSS_SORTEDINMEM:
			state->current = state->markpos_offset;
			state->eof_reached = state->markpos_eof;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeSeek(state->result_tape,
							state->markpos_block,
							state->markpos_offset);
			state->eof_reached = state->markpos_eof;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_get_stats - 提取汇总统计信息
 *
 * 可在 tuplesort_performsort() 后调用，以获得排序
 * 执行方式的可打印摘要信息。
 */
void
tuplesort_get_stats(Tuplesortstate *state,
					TuplesortInstrumentation *stats)
{
	/*
	 * 注：基于磁盘的排序似乎应同时提供内存与磁盘的
	 * 使用情况。然而，一旦我们开始向调用者返回元组，
	 * 当前代码就不再准确跟踪内存空间（因为我们没有
	 * 把调用者应做的 pfree 计入），故无法依赖
	 * 磁盘排序中的 availMem。修复它似乎不值得开销。
	 * 是否值得为内存上下文代码建一个 API 来告知我们
	 * sortcontext 的实际用量？
	 */
	tuplesort_updatemax(state);

	if (state->isMaxSpaceDisk)
		stats->spaceType = SORT_SPACE_TYPE_DISK;
	else
		stats->spaceType = SORT_SPACE_TYPE_MEMORY;
	stats->spaceUsed = (state->maxSpace + 1023) / 1024;

	switch (state->maxSpaceStatus)
	{
		case TSS_SORTEDINMEM:
			if (state->boundUsed)
				stats->sortMethod = SORT_TYPE_TOP_N_HEAPSORT;
			else
				stats->sortMethod = SORT_TYPE_QUICKSORT;
			break;
		case TSS_SORTEDONTAPE:
			stats->sortMethod = SORT_TYPE_EXTERNAL_SORT;
			break;
		case TSS_FINALMERGE:
			stats->sortMethod = SORT_TYPE_EXTERNAL_MERGE;
			break;
		default:
			stats->sortMethod = SORT_TYPE_STILL_IN_PROGRESS;
			break;
	}
}

/*
 * 把TuplesortMethod转为字符串。
 */
const char *
tuplesort_method_name(TuplesortMethod m)
{
	switch (m)
	{
		case SORT_TYPE_STILL_IN_PROGRESS:
			return "still in progress";
		case SORT_TYPE_TOP_N_HEAPSORT:
			return "top-N heapsort";
		case SORT_TYPE_QUICKSORT:
			return "quicksort";
		case SORT_TYPE_EXTERNAL_SORT:
			return "external sort";
		case SORT_TYPE_EXTERNAL_MERGE:
			return "external merge";
	}

	return "unknown";
}

/*
 * 把TuplesortSpaceType转为字符串。
 */
const char *
tuplesort_space_type_name(TuplesortSpaceType t)
{
	Assert(t == SORT_SPACE_TYPE_DISK || t == SORT_SPACE_TYPE_MEMORY);
	return t == SORT_SPACE_TYPE_DISK ? "Disk" : "Memory";
}


/*
 * 堆操作例程，依照 Knuth 算法 5.2.3H。
 */

/*
 * 把现有无序 SortTuples 数组转换为有界堆，丢弃除最
 * 小的 "state->bound" 个元组之外的一切。
 *
 * 使用有界堆时，我们要把最大项留在根部（数组项
 * 0）处，而非像普通排序情形那样保留最小项。这样
 * 我们就能廉价地丢弃最大项。
 * 因此，我们暂时反转排序方向。
 */
static void
make_bounded_heap(Tuplesortstate *state)
{
	int			tupcount = state->memtupcount;
	int			i;

	Assert(state->status == TSS_INITIAL);
	Assert(state->bounded);
	Assert(tupcount >= state->bound);
	Assert(SERIAL(state));

	/* 反转排序方向使最大项位于根部 */
	reversedirection(state);

	state->memtupcount = 0;		/* 置堆为空 */
	for (i = 0; i < tupcount; i++)
	{
		if (state->memtupcount < state->bound)
		{
			/* 将下个元组插入堆 */
			/* 必须复制源元组以免可能的覆写 */
			SortTuple	stup = state->memtuples[i];

			tuplesort_heap_insert(state, &stup);
		}
		else
		{
			/*
			 * 堆已满。用新元组换最大项；新元组若比堆
			 * 中已有项都大，则直接丢弃新元组。
			 *
			 */
			if (COMPARETUP(state, &state->memtuples[i], &state->memtuples[0]) <= 0)
			{
				free_sort_tuple(state, &state->memtuples[i]);
				CHECK_FOR_INTERRUPTS();
			}
			else
				tuplesort_heap_replace_top(state, &state->memtuples[i]);
		}
	}

	Assert(state->memtupcount == state->bound);
	state->status = TSS_BOUNDED;
}

/*
 * 把有界堆转换为正确排序的数组
 */
static void
sort_bounded_heap(Tuplesortstate *state)
{
	int			tupcount = state->memtupcount;

	Assert(state->status == TSS_BOUNDED);
	Assert(state->bounded);
	Assert(tupcount == state->bound);
	Assert(SERIAL(state));

	/*
	 * 我们可以原地解除堆化，因每次删堆顶调用都会移
	 * 除最大项，而我们可以立刻把它存入末端新腾出的槽
	 * 位中。一旦堆只剩单个项，就完成了。
	 */
	while (state->memtupcount > 1)
	{
		SortTuple	stup = state->memtuples[0];

		/* 这会把次大项上浮并减少 memtupcount */
		tuplesort_heap_delete_top(state);
		state->memtuples[state->memtupcount] = stup;
	}
	state->memtupcount = tupcount;

	/*
	 * 把排序方向反转回原来的状态。这并非真正必
	 * 要，但似乎是个求整洁的好主意。
	 */
	reversedirection(state);

	state->status = TSS_SORTEDINMEM;
	state->boundUsed = true;
}

/*
 * 用专用 qsort() 例程给全部 memtuples 排序。
 *
 * 快速排序用于小型内存排序以及外部排序归并段。
 */
static void
tuplesort_sort_memtuples(Tuplesortstate *state)
{
	Assert(!LEADER(state));

	if (state->memtupcount > 1)
	{
		/*
		 * datum1 中是首列的值还是缩略？其比较器有没有
		 * 专门化版本？
		 */
		if (state->base.haveDatum1 && state->base.sortKeys)
		{
			if (state->base.sortKeys[0].comparator == ssup_datum_unsigned_cmp)
			{
				qsort_tuple_unsigned(state->memtuples,
									 state->memtupcount,
									 state);
				return;
			}
#if SIZEOF_DATUM >= 8
			else if (state->base.sortKeys[0].comparator == ssup_datum_signed_cmp)
			{
				qsort_tuple_signed(state->memtuples,
								   state->memtupcount,
								   state);
				return;
			}
#endif
			else if (state->base.sortKeys[0].comparator == ssup_datum_int32_cmp)
			{
				qsort_tuple_int32(state->memtuples,
								  state->memtupcount,
								  state);
				return;
			}
		}

		/* 能否使用单键排序函数？ */
		if (state->base.onlyKey != NULL)
		{
			qsort_ssup(state->memtuples, state->memtupcount,
					   state->base.onlyKey);
		}
		else
		{
			qsort_tuple(state->memtuples,
						state->memtupcount,
						state->base.comparetup,
						state);
		}
	}
}

/*
 * 把新元组插入空堆或既有堆，同时维持堆不变量。
 * 调用者负责确保有空间。
 *
 * 注：某些调用者的 tuple 指向 memtuples[] 数组内堆末
 * 端之上的条目。只要它不紧邻堆末端（即不在
 * [memtupcount] 项），就是安全的 —— 否则它可能
 * 移入堆中之前被覆写！
 */
static void
tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple)
{
	SortTuple  *memtuples;
	int			j;

	memtuples = state->memtuples;
	Assert(state->memtupcount < state->memtupsize);

	CHECK_FOR_INTERRUPTS();

	/*
	 * 按 Knuth 5.2.3 练习 16 将新项上浮。注意 Knuth 使用的是
	 * 1 基数组下标，而非 0 基。
	 */
	j = state->memtupcount++;
	while (j > 0)
	{
		int			i = (j - 1) >> 1;

		if (COMPARETUP(state, tuple, &memtuples[i]) >= 0)
			break;
		memtuples[j] = memtuples[i];
		j = i;
	}
	memtuples[j] = *tuple;
}

/*
 * 从堆移除 state->memtuples[0] 的元组并递减
 * memtupcount，并上浮以维持堆不变量。
 *
 * 若有必要，调用者已释放顶节点所指的元组。
 *
 */
static void
tuplesort_heap_delete_top(Tuplesortstate *state)
{
	SortTuple  *memtuples = state->memtuples;
	SortTuple  *tuple;

	if (--state->memtupcount <= 0)
		return;

	/*
	 * 移除堆中最后一个元组并重新插入它，用它换
	 * 掉顶节点。
	 */
	tuple = &memtuples[state->memtupcount];
	tuplesort_heap_replace_top(state, tuple);
}

/*
 * 用新元组替换 state->memtuples[0] 处的元组。上浮以维持
 * 堆不变量。
 *
 * 这对应 Knuth 的"上浮"算法（算法 5.2.3H，堆排序，
 * 步骤 H3-H8）。
 */
static void
tuplesort_heap_replace_top(Tuplesortstate *state, SortTuple *tuple)
{
	SortTuple  *memtuples = state->memtuples;
	unsigned int i,
				n;

	Assert(state->memtupcount >= 1);

	CHECK_FOR_INTERRUPTS();

	/*
	 * state->memtupcount 是 "int"，但 i、j、n 用 "unsigned int"。
	 * 这可防止 "2 * i + 1" 计算溢出，因为循环顶部必有
	 * i < n <= INT_MAX <= UINT_MAX/2。
	 */
	n = state->memtupcount;
	i = 0;						/* i 是"洞"所在 */
	for (;;)
	{
		unsigned int j = 2 * i + 1;

		if (j >= n)
			break;
		if (j + 1 < n &&
			COMPARETUP(state, &memtuples[j], &memtuples[j + 1]) > 0)
			j++;
		if (COMPARETUP(state, tuple, &memtuples[j]) <= 0)
			break;
		memtuples[i] = memtuples[j];
		i = j;
	}
	memtuples[i] = *tuple;
}

/*
 * 从当前状态反转排序方向的函数
 *
 * 执行哈希元组排序时调用此函数并不安全
 */
static void
reversedirection(Tuplesortstate *state)
{
	SortSupport sortKey = state->base.sortKeys;
	int			nkey;

	for (nkey = 0; nkey < state->base.nKeys; nkey++, sortKey++)
	{
		sortKey->ssup_reverse = !sortKey->ssup_reverse;
		sortKey->ssup_nulls_first = !sortKey->ssup_nulls_first;
	}
}


/*
 * 磁带接口例程
 */

static unsigned int
getlen(LogicalTape *tape, bool eofOK)
{
	unsigned int len;

	if (LogicalTapeRead(tape,
						&len, sizeof(len)) != sizeof(len))
		elog(ERROR, "unexpected end of tape");
	if (len == 0 && !eofOK)
		elog(ERROR, "unexpected end of data");
	return len;
}

static void
markrunend(LogicalTape *tape)
{
	unsigned int len = 0;

	LogicalTapeWrite(tape, &len, sizeof(len));
}

/*
 * 在 READTUP() 例程内为元组获取内存。
 *
 * 我们从 slab 分配器取下个空闲槽位；元组太大时
 * 则改用 palloc()。
 */
void *
tuplesort_readtup_alloc(Tuplesortstate *state, Size tuplen)
{
	SlabSlot   *buf;

	/*
	 * 我们在 slab 区预分配了足够槽位，因此永不应耗尽。
	 *
	 */
	Assert(state->slabFreeHead);

	if (tuplen > SLAB_SLOT_SIZE || !state->slabFreeHead)
		return MemoryContextAlloc(state->base.sortcontext, tuplen);
	else
	{
		buf = state->slabFreeHead;
		/* 复用槽位 */
		state->slabFreeHead = buf->nextfree;

		return buf;
	}
}


/*
 * 并行排序例程
 */

/*
 * tuplesort_estimate_shared - 估算所需共享内存分配量
 *
 * nWorkers 是工作进程数量的估计值（它就是将要请求的
 * 数量）。
 */
Size
tuplesort_estimate_shared(int nWorkers)
{
	Size		tapesSize;

	Assert(nWorkers > 0);

	/* 确保 BufFile 共享状态经 MAXALIGN 对齐 */
	tapesSize = mul_size(sizeof(TapeShare), nWorkers);
	tapesSize = MAXALIGN(add_size(tapesSize, offsetof(Sharedsort, tapes)));

	return tapesSize;
}

/*
 * tuplesort_initialize_shared - 初始化共享 tuplesort 状态
 *
 * 须在启动工作进程前由主导进程调用，以预先建立
 * 工作进程 tuplesortstate 所需的预先状态。nWorkers
 * 应与传给 tuplesort_estimate_shared() 的参数一致。
 */
void
tuplesort_initialize_shared(Sharedsort *shared, int nWorkers, dsm_segment *seg)
{
	int			i;

	Assert(nWorkers > 0);

	SpinLockInit(&shared->mutex);
	shared->currentWorker = 0;
	shared->workersFinished = 0;
	SharedFileSetInit(&shared->fileset, seg);
	shared->nTapes = nWorkers;
	for (i = 0; i < nWorkers; i++)
	{
		shared->tapes[i].firstblocknumber = 0L;
	}
}

/*
 * tuplesort_attach_shared - 附着共享 tuplesort 状态
 *
 * 必须由所有工作进程调用。
 */
void
tuplesort_attach_shared(Sharedsort *shared, dsm_segment *seg)
{
	/* 附着 SharedFileSet */
	SharedFileSetAttach(&shared->fileset, seg);
}

/*
 * worker_get_identifier - 分配并返回工作进程的序号标识
 *
 * 这些标识的分配顺序没有良好定义，也不应该有影响；
 * 并行排序各参与者间的工作进程号只需互不相同且连
 * 续无空缺。logtape.c 需要如此。
 *
 * 注意此处分配的标识符与
 * ParallelWorkerNumber 毫无关系，以免对调用者需求做
 * 任何假设。不过，我们确实遵循 ParallelWorkerNumber 的
 * 惯例：用工作进程编号 -1 表示非工作进程。这包括
 * 主导进程以及串行 Tuplesort 进程。
 */
static int
worker_get_identifier(Tuplesortstate *state)
{
	Sharedsort *shared = state->shared;
	int			worker;

	Assert(WORKER(state));

	SpinLockAcquire(&shared->mutex);
	worker = shared->currentWorker++;
	SpinLockRelease(&shared->mutex);

	return worker;
}

/*
 * worker_freeze_result_tape - 为主导进程冻结结果磁带
 *
 * 工作进程在结果磁带确定之后立即调用此函数，而不是
 * 直接调用 LogicalTapeFreeze()。因为比起类似的串行
 * TSS_SORTEDONTAPE 外部排序，工作进程需几步；
 * 这些步骤同样发生在此处。额外步骤包括释放已不
 * 再需要的资源，并向主导进程表明工作进程输入归
 * 并段可供其归并。
 *
 * 每个工作进程应当只有一个最终输出归并段，它由最初
 * 输入到该工作进程的全部元组构成。
 */
static void
worker_freeze_result_tape(Tuplesortstate *state)
{
	Sharedsort *shared = state->shared;
	TapeShare	output;

	Assert(WORKER(state));
	Assert(state->result_tape != NULL);
	Assert(state->memtupcount == 0);

	/*
	 * 释放大部分剩余内存，防调用者对我们持续占内存
	 * 敏感。此时 memtuples 可能已非很小的归并堆。
	 */
	pfree(state->memtuples);
	/* 整洁 */
	state->memtuples = NULL;
	state->memtupsize = 0;

	/*
	 * 并行工作进程需要结果磁带元数据，它要存进共享内
	 * 存中供主导进程
	 */
	LogicalTapeFreeze(state->result_tape, &output);

	/* 保存输出磁带属性，并更新已完成工作进程计数 */
	SpinLockAcquire(&shared->mutex);
	shared->tapes[state->worker] = output;
	shared->workersFinished++;
	SpinLockRelease(&shared->mutex);
}

/*
 * worker_nomergeruns - 工作进程转储 memtuples，不归并
 *
 * 工作进程无需归并时，以此替代 mergeruns() 调用。
 *
 */
static void
worker_nomergeruns(Tuplesortstate *state)
{
	Assert(WORKER(state));
	Assert(state->result_tape == NULL);
	Assert(state->nOutputRuns == 1);

	state->result_tape = state->destTape;
	worker_freeze_result_tape(state);
}

/*
 * leader_takeover_tapes - 从工作进程磁带创建主导磁带集
 *
 * 到目前为止，主导进程的 Tuplesortstate 尚未执行任何实际
 * 排序。所有排序均已发生于工作进程中，且必然返回自
 * tuplesort_performsort()。
 *
 * 此函数返回时，主导进程的状态几乎与它以串行外
 * 部排序身份生成归并段后的状态无法区分。
 *
 */
static void
leader_takeover_tapes(Tuplesortstate *state)
{
	Sharedsort *shared = state->shared;
	int			nParticipants = state->nParticipants;
	int			workersFinished;
	int			j;

	Assert(LEADER(state));
	Assert(nParticipants >= 1);

	SpinLockAcquire(&shared->mutex);
	workersFinished = shared->workersFinished;
	SpinLockRelease(&shared->mutex);

	if (nParticipants != workersFinished)
		elog(ERROR, "cannot take over tapes before all workers finish");

	/*
	 * 由工作进程磁带创建磁带集，末尾含一条主导进程自
	 * 有磁带。并行工作进程比逻辑磁带贵得多，因此此处
	 * 分配的磁带数量绝不应过多。
	 */
	inittapestate(state, nParticipants);
	state->tapeset = LogicalTapeSetCreate(false, &shared->fileset, -1);

	/*
	 * 设 currentRun 以反映将归并的归并段数（它并无实际
	 * 用途，这只是走个形式）
	 */
	state->currentRun = nParticipants;

	/*
	 * 把状态初始化为与建完初始归并段后相同的样子。
	 *
	 *
	 * 每个工作进程总恰有 1 个归并段，且每段恰好
	 * 有 1 条输入磁带，因工作进程总恰好输出 1 个归
	 * 并段，哪怕无可排序的输入元组。
	 */
	state->inputTapes = NULL;
	state->nInputTapes = 0;
	state->nInputRuns = 0;

	state->outputTapes = palloc0(nParticipants * sizeof(LogicalTape *));
	state->nOutputTapes = nParticipants;
	state->nOutputRuns = nParticipants;

	for (j = 0; j < nParticipants; j++)
	{
		state->outputTapes[j] = LogicalTapeImport(state->tapeset, j, &shared->tapes[j]);
	}

	state->status = TSS_BUILDRUNS;
}

/*
 * 便捷例程：释放先前装入排序内存的元组。
 */
static void
free_sort_tuple(Tuplesortstate *state, SortTuple *stup)
{
	if (stup->tuple)
	{
		FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
		pfree(stup->tuple);
		stup->tuple = NULL;
	}
}

int
ssup_datum_unsigned_cmp(Datum x, Datum y, SortSupport ssup)
{
	if (x < y)
		return -1;
	else if (x > y)
		return 1;
	else
		return 0;
}

#if SIZEOF_DATUM >= 8
int
ssup_datum_signed_cmp(Datum x, Datum y, SortSupport ssup)
{
	int64		xx = DatumGetInt64(x);
	int64		yy = DatumGetInt64(y);

	if (xx < yy)
		return -1;
	else if (xx > yy)
		return 1;
	else
		return 0;
}
#endif

int
ssup_datum_int32_cmp(Datum x, Datum y, SortSupport ssup)
{
	int32		xx = DatumGetInt32(x);
	int32		yy = DatumGetInt32(y);

	if (xx < yy)
		return -1;
	else if (xx > yy)
		return 1;
	else
		return 0;
}
