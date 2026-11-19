/*-------------------------------------------------------------------------
 *
 * dsa.c
 *	  动态共享内存区域。
 *
 * 本模块提供动态共享内存区域，其构建于
 * DSM 段之上。虽然 dsm.c 允许创建共享内存中的段并在
 * 后端之间共享，但它并非为处理小对象而设
 * 计的。DSA 区域是一个共享内存堆，通常由一或多个
 * DSM 段支撑，可使用 dsa_allocate() 和 dsa_free() 分配内存。
 * 或者，它也可以在既有的共享内存中创建，包括在
 * 一个 DSM 段中，然后按需创建额外的 DSM 段。与常规
 * 的系统堆不同，它处理的是伪指针，这类指针必须
 * 在解引用前转换为后端本地指针。不过，这些
 * 指针可以与其他后端共享，并且可用于构建
 * 共享数据结构。
 *
 * 每个 DSA 区域管理一组 DSM 段，按需添加新段，
 * 并在不再需要时将其分离。每个段包含若干 4KB 页、
 * 一个追踪连续空闲页串的空闲页管理器，
 * 以及一个追踪每页上所分配对象来源的页映射。
 * 超过 8KB 的分配请求通过挑选段并在其空闲页
 * 管理器中寻找连续空闲页来处理。较小者则用一组
 * 选定大小的对象池来处理。每个池由若干 16 页
 * （64KB）超级块组成，其分配方式与大对象相同，
 * 大对象和新超级块的分配由单个 LWLock 串行化，但
 * 从既有超级块分配小对象时每池使用一个 LWLock。
 * 当前每个大小类只有一个池，因而也只有一个
 * 锁。用于提升并发度的每核心池，以及减少由此产生
 * 碎片化的策略，都是未来研究方向。每个超级块
 * 由一个"跨度"（span）管理，它追踪超级块的空闲链表。
 * 释放请求的处理方式是：在页映射中查找某地址
 * 从哪个跨度分配而来，从而小对象可归还至
 * 适当的空闲列表，而大对象页则可直接归还
 * 给空闲页映射。分配时，用于挑选
 * 段和超级块的简单启发式算法促使已占用内存
 * 集中，从而提高整个超级块变空并归还给空闲页
 * 管理器、以及整个段变空并
 * 归还给操作系统
 * 的可能性。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/dsa.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/freepage.h"
#include "utils/memutils.h"

/*
 * dsa_create 所创建的 dsa_area 背后的初始 DSM 段
 * 大小。在创建若干此大小的段之后，我们会将
 * 其大小翻倍，依此类推。必要时可能会创建更大的段，
 * 以满足大请求。
 */
#define DSA_INITIAL_SEGMENT_SIZE ((size_t) (1 * 1024 * 1024))

/*
 * 在将段大小翻倍之前要创建多少个段。若此值过低，
 * 则最大段中很可能有大量浪费的空间。若过高，
 * 则可能耗尽段槽位（参见 dsm.c 对段总数的
 * 限制），或者限制使用小指针时区域可管理的
 * 总大小。
 */
#define DSA_NUM_SEGMENTS_AT_EACH_SIZE 2

/*
 * 用于表示 dsa_pointer 偏移部分的位数。
 * 它决定了段的最大大小、最大可能的分配大小，
 * 以及每个区域的最大段数。
 */
#if SIZEOF_DSA_POINTER == 4
#define DSA_OFFSET_WIDTH 27		/* 最多 128MB 的 32 段 */
#else
#define DSA_OFFSET_WIDTH 40		/* 最多 1TB 的 1024 段 */
#endif

/*
 * 一个区域可拥有的 DSM 段最大数量，由剩余位数决定
 * （但上限为 1024）。
 */
#define DSA_MAX_SEGMENTS \
	Min(1024, (1 << ((SIZEOF_DSA_POINTER * 8) - DSA_OFFSET_WIDTH)))

/* 从 dsa_pointer 中提取偏移量的位掩码。 */
#define DSA_OFFSET_BITMASK (((dsa_pointer) 1 << DSA_OFFSET_WIDTH) - 1)

/* DSM 段的最大大小。 */
#define DSA_MAX_SEGMENT_SIZE ((size_t) 1 << DSA_OFFSET_WIDTH)

/* 每个常规超级块的页数（见 FPM_PAGE_SIZE）。 */
#define DSA_PAGES_PER_SUPERBLOCK		16

/*
 * 一个魔术数，用作对 DSA 区域后续各 DSM 段的
 * 健全性检查（该数值会与区域句柄及段下标作
 * XOR 运算）。
 */
#define DSA_SEGMENT_HEADER_MAGIC 0x0ce26608

/* 给定段号和偏移量，构建一个 dsa_pointer。 */
#define DSA_MAKE_POINTER(segment_number, offset) \
	(((dsa_pointer) (segment_number) << DSA_OFFSET_WIDTH) | (offset))

/* 从 dsa_pointer 中提取段号。 */
#define DSA_EXTRACT_SEGMENT_NUMBER(dp) ((dp) >> DSA_OFFSET_WIDTH)

/* 从 dsa_pointer 中提取偏移量。 */
#define DSA_EXTRACT_OFFSET(dp) ((dp) & DSA_OFFSET_BITMASK)

/* 用于段下标的类型（从零起算）。 */
typedef size_t dsa_segment_index;

/* dsa_segment_index 的哨兵值，表示"无"或"末尾"。 */
#define DSA_SEGMENT_INDEX_NONE (~(dsa_segment_index)0)

/*
 * 我们有多少个段桶？这些桶用于按段最大的连续
 * 空闲页串对段进行分类。
 */
#define DSA_NUM_SEGMENT_BINS 16

/*
 * 装有*可能*拥有 n 个连续空闲页的段的最低桶
 * 是什么？没必要去查看更低桶中的段；它们肯定
 * 无法满足对 n 个空闲页的请求。
 */
static inline size_t
contiguous_pages_to_segment_bin(size_t n)
{
	size_t		bin;

	if (n == 0)
		bin = 0;
	else
		bin = pg_leftmost_one_pos_size_t(n) + 1;

	return Min(bin, DSA_NUM_SEGMENT_BINS - 1);
}

/* 访问锁的宏。 */
#define DSA_AREA_LOCK(area) (&area->control->lock)
#define DSA_SCLASS_LOCK(area, sclass) (&area->control->pools[sclass].lock)

/*
 * 单个段的头部。它位于 DSA 区域所拥有的每个
 * DSM 段的开头，包括第一个段（在那里它作为
 * dsa_area_control 的一部分）。
 */
typedef struct
{
	/* 魔术值检查。 */
	uint32		magic;
	/* 本段的页总数（不含元数据区）。 */
	size_t		usable_pages;
	/* 本段的总字节大小。 */
	size_t		size;

	/*
	 * 同一段桶中位于本段之前的段下标，若本段是
	 * 第一个则为 DSA_SEGMENT_INDEX_NONE。
	 */
	dsa_segment_index prev;

	/*
	 * 同一段桶中位于本段之后的段下标，若本段是
	 * 最后一个则为 DSA_SEGMENT_INDEX_NONE。
	 */
	dsa_segment_index next;
	/* 包含本段的桶的下标。 */
	size_t		bin;

	/*
	 * 一个标志位，置位表示本段正在被归还给操作系统
	 * 并且已被解除钉住。
	 */
	bool		freed;
} dsa_segment_header;

/*
 * 超级块的元数据。
 *
 * 对大多数块而言，跨度对象是外置存储的；亦即，
 * 跨度对象并不存储在块自身之内。但作为例外，对于
 * "跨度的跨度"，跨度对象是"内联"存储的。其分配
 * 恰好是一页，dsa_area_span 对象就位于
 * 该页的开头。大小类是 DSA_SCLASS_BLOCK_OF_SPANS，
 * 其余字段的用法与普通块中的用法一样。我们不能
 * 从普通超级块分配跨度，因为创建普通超级块要求
 * 我们能够*先*分配出跨度。这样做可以避免
 * 该循环依赖。
 */
typedef struct
{
	dsa_pointer pool;			/* 所在池。 */
	dsa_pointer prevspan;		/* 前一跨度 */
	dsa_pointer nextspan;		/*下一跨度*/
	dsa_pointer start;			/* 起始地址。 */
	size_t		npages;			/* 跨度的页数。 */
	uint16		size_class;		/* 大小类 */
	uint16		ninitialized;	/* 曾分配过的对象数上限。 */
	uint16		nallocatable;	/* 当前可分配的对象数。 */
	uint16		firstfree;		/* 空闲列表首对象。 */
	uint16		nmax;			/* 可能对象数的上限。 */
	uint16		fclass;			/* 当前满度类。 */
} dsa_area_span;

/*
 * 给定指向跨度内某对象的指针，以左值形式访问
 * 同一跨度内下一空闲对象的下标（即空闲链表）。
 */
#define NextFreeObjectIndex(object) (* (uint16 *) (object))

/*
 * 小分配的处理方式是把单个内存块划分为许多大小
 * 相同的对象。可用的分配大小由下面的数组
 * 定义。较大大小类的间距比较小大小类更宽。对于
 * 大于 1kB 的大小类，我们对间距做了调整以免浪费
 * 空间：基于我们计划分配 64kB 块这一前提，我们把
 * 最大对象大小上调到仍能让同样数量的对象
 * 放入一个块中的最大 8 字节倍数。
 *
 * 注意：由于这种调整，如果将来对小型分配使用
 * 不同大小的块，这些大小类就需要重新设计，
 * 以适配新尺寸。
 *
 * 注意：大小类的最优间距，以及分配小型
 * 对象的块的尺寸，并不是一个有唯一正确答案的
 * 问题。有些分配器（如 tcmalloc）使用比我们这里更密的
 * 大小类，而另一些（如 aset.c）则使用更疏的
 * 大小类。把大小类排得更密可避免单个区块内的
 * 内存浪费，但也意味着可能未填满的块的
 * 数量更大。
 */
static const uint16 dsa_size_classes[] = {
	sizeof(dsa_area_span), 0,	/* 特殊大小类 */
	8, 16, 24, 32, 40, 48, 56, 64,	/* 以 8 字节分隔的 8 个类 */
	80, 96, 112, 128,			/* 以 16 字节分隔的 4 个类 */
	160, 192, 224, 256,			/* 以 32 字节分隔的 4 个类 */
	320, 384, 448, 512,			/* 以 64 字节分隔的 4 个类 */
	640, 768, 896, 1024,		/* 以 128 字节分隔的 4 个类 */
	1280, 1560, 1816, 2048,		/* 间隔约 256 字节的 4 个类 */
	2616, 3120, 3640, 4096,		/* 间隔约 512 字节的 4 个类 */
	5456, 6552, 7280, 8192		/* 间隔约 1024 字节的 4 个类 */
};
#define DSA_NUM_SIZE_CLASSES				lengthof(dsa_size_classes)

/* 特殊大小类。 */
#define DSA_SCLASS_BLOCK_OF_SPANS		0
#define DSA_SCLASS_SPAN_LARGE			1

/*
 * 下面的查找表用于把小对象（小于 1kB）的大小
 * 映射到对应的大小类。使用此表时，
 * 先把对象大小向上取整到 8 字节的下一个倍数，
 * 再索引本数组。
 */
static const uint8 dsa_size_class_map[] = {
	2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 11, 11, 12, 12, 13, 13,
	14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17, 17, 17,
	18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19,
	20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21,
	22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
	23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
	24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
	25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25
};
#define DSA_SIZE_CLASS_MAP_QUANTUM	8

/*
 * 超级块按其充满程度分桶。一般来说，每个满度类
 * 大致对应一个四分位，但用于分配的块总位于
 * 满度类 1 列表的头部，
 * 无论它实际有多满。
 */
#define DSA_FULLNESS_CLASSES		4

/*
 * dsa_area_pool 表示给定大小类的一组对象。
 *
 * 或许同一个大小类应当有多个池以避免争用，
 * 但目前只有一个！
 */
typedef struct
{
	/* 保护对本池访问的锁。 */
	LWLock		lock;
	/* 按满度排列的跨度链表集合。 */
	dsa_pointer spans[DSA_FULLNESS_CLASSES];
	/* 是否应填充到缓存行边界？ */
} dsa_area_pool;

/*
 * 一个区域的控制块。它位于共享内存中，即本区域
 * 所控制的第一个 DSM 段的开头。
 */
typedef struct
{
	/* 第一个段的段头部。 */
	dsa_segment_header segment_header;
	/* 本区域的句柄。 */
	dsa_handle	handle;
	/* 本区域拥有的段的句柄。 */
	dsm_handle	segment_handles[DSA_MAX_SEGMENTS];
	/* 按最大连续空闲页串分桶的段列表。 */
	dsa_segment_index segment_bins[DSA_NUM_SEGMENT_BINS];
	/* 每个大小类的对象池。 */
	dsa_area_pool pools[DSA_NUM_SIZE_CLASSES];
	/* 所有活跃段的总大小。 */
	size_t		total_segment_size;
	/* 我们允许的后备存储最大总大小。 */
	size_t		max_total_segment_size;
	/* 本区域历史上使用过的最高段下标。 */
	dsa_segment_index high_segment_index;
	/* 本区域的引用计数。 */
	int			refcnt;
	/* 指示本区域已被钉住的标志。 */
	bool		pinned;
	/* 段被释放的次数。 */
	size_t		freed_segment_counter;
	/* LWLock tranche ID。 */
	int			lwlock_tranche_id;
	/* 通用锁（保护除对象池之外的一切）。 */
	LWLock		lock;
} dsa_area_control;

/* 由池指针求 dsa_pointer。 */
#define DsaAreaPoolToDsaPointer(area, p)	\
	DSA_MAKE_POINTER(0, (char *) p - (char *) area->control)

/*
 * dsa_segment_map 存储在每个后端各自的私有内存
 * 中。它持有该段在此后端中的基地址，以及段内
 * 关键对象的地址。这些地址虽然也可以从基地址
 * 推导出来，但把它们备妥在手边也的确有
 * 用。
 */
typedef struct
{
	dsm_segment *segment;		/* DSM 段 */
	char	   *mapped_address; /* 该段的映射地址 */
	dsa_segment_header *header; /* 头部（同 mapped_address） */
	FreePageManager *fpm;		/* 段内的空闲页管理器。 */
	dsa_pointer *pagemap;		/* 段内的页映射。 */
} dsa_segment_map;

/*
 * 每个后端各自的存储区域状态。后端通过创建区域
 * 或使用句柄附接到既有区域来获得其中之一。每个
 * 需要使用区域的进程都用自己的对象来追踪各段
 * 映射到何处。
 */
struct dsa_area
{
	/* 指向共享内存中控制对象的指针。 */
	dsa_area_control *control;

	/* 映射是否已被钉住？ */
	bool		mapping_pinned;

	/*
	 * 本后端的段映射数组，按段下标排列，与
	 * control->segment_handles 相对应。区域中的
	 * 某些段可能尚未映射到本后端，还有些槽位可能
	 * 已被释放而需要分离；这些操作都按需进行。
	 */
	dsa_segment_map segment_maps[DSA_MAX_SEGMENTS];

	/* 本后端曾映射过的最高段下标。 */
	dsa_segment_index high_segment_index;

	/* 上次观测的 freed_segment_counter。 */
	size_t		freed_segment_counter;
};

#define DSA_SPAN_NOTHING_FREE	((uint16) -1)
#define DSA_SUPERBLOCK_SIZE (DSA_PAGES_PER_SUPERBLOCK * FPM_PAGE_SIZE)

/* 给定指向 segment_map 的指针，求出段下标。 */
#define get_segment_index(area, segment_map_ptr) \
	(segment_map_ptr - &area->segment_maps[0])

static void init_span(dsa_area *area, dsa_pointer span_pointer,
					  dsa_area_pool *pool, dsa_pointer start, size_t npages,
					  uint16 size_class);
static bool transfer_first_span(dsa_area *area, dsa_area_pool *pool,
								int fromclass, int toclass);
static inline dsa_pointer alloc_object(dsa_area *area, int size_class);
static bool ensure_active_superblock(dsa_area *area, dsa_area_pool *pool,
									 int size_class);
static dsa_segment_map *get_segment_by_index(dsa_area *area,
											 dsa_segment_index index);
static void destroy_superblock(dsa_area *area, dsa_pointer span_pointer);
static void unlink_span(dsa_area *area, dsa_area_span *span);
static void add_span_to_fullness_class(dsa_area *area, dsa_area_span *span,
									   dsa_pointer span_pointer, int fclass);
static void unlink_segment(dsa_area *area, dsa_segment_map *segment_map);
static dsa_segment_map *get_best_segment(dsa_area *area, size_t npages);
static dsa_segment_map *make_new_segment(dsa_area *area, size_t requested_pages);
static dsa_area *create_internal(void *place, size_t size,
								 int tranche_id,
								 dsm_handle control_handle,
								 dsm_segment *control_segment);
static dsa_area *attach_internal(void *place, dsm_segment *segment,
								 dsa_handle handle);
static void check_for_freed_segments(dsa_area *area);
static void check_for_freed_segments_locked(dsa_area *area);
static void rebin_segment(dsa_area *area, dsa_segment_map *segment_map);

/*
 * 在一个新的 DSM 段中创建新的共享区域。后续会按
 * 需分配更多 DSM 段以扩展可用空间。
 *
 * 我们无法在本函数内分配 LWLock tranche_id，因为
 * tranche ID 是稀缺资源；只有 64k 可用，尽量使用
 * 较小的编号很重要，而且我们没有回收它们的
 * 机制，故需调用者提供。
 */
dsa_area *
dsa_create(int tranche_id)
{
	dsm_segment *segment;
	dsa_area   *area;

	/*
	 * 创建将容纳共享控制对象以及第一段可用空间的
	 * DSM 段。
	 */
	segment = dsm_create(DSA_INITIAL_SEGMENT_SIZE, 0);

	/*
	 * 支撑本区域的所有段都被钉住，这样 DSA 便可以
	 * 显式控制它们的生命周期（否则，属于本区域的
	 * 新段可能在恰好将其映射入的唯一后端结束时被
	 * 释放，从而破坏该区域）。
	 */
	dsm_pin_segment(segment);

	/* 在本段中创建控制对象所在的新 DSA 区域。 */
	area = create_internal(dsm_segment_address(segment),
						   DSA_INITIAL_SEGMENT_SIZE,
						   tranche_id,
						   dsm_segment_handle(segment), segment);

	/* 当控制段分离时进行清理。 */
	on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
				  PointerGetDatum(dsm_segment_address(segment)));

	return area;
}

/*
 * 在既有的共享内存空间中创建新的共享区域，该空间
 * 可以是 DSM 或 postmaster 初始化的内存。DSM 段会
 * 按需分配以扩展可用空间，但这可以通过
 * dsa_set_size_limit(area, size) 阻止，所用大小同
 *  dsa_create_in_place。
 *
 * 就地创建的区域最终必须由创建它的后端以及所有
 * 附接它的后端释放。这项工作可以通过
 * dsa_release_in_place 显式完成，或者，在 'place' 恰好
 * 位于既有 DSM 段内的情况下，通过传入指向该段的
 * 指针，以便向包含它的 DSM 段注册一个分离挂
 * 钩。
 *
 * 关于 tranche 参数的说明见 dsa_create()。
 */
dsa_area *
dsa_create_in_place(void *place, size_t size,
					int tranche_id, dsm_segment *segment)
{
	dsa_area   *area;

	area = create_internal(place, size, tranche_id,
						   DSM_HANDLE_INVALID, NULL);

	/*
	 * 若提供了包含它的 DSM 段，则在控制段分离时
	 * 清理。
	 */
	if (segment != NULL)
		on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
					  PointerGetDatum(place));

	return area;
}

/*
 * 获取一个可传递给其他进程的句柄，使其能够附接
 * 到给定区域。对以 dsa_create_in_place 创建的区域
 * 不可调用。
 */
dsa_handle
dsa_get_handle(dsa_area *area)
{
	Assert(area->control->handle != DSA_HANDLE_INVALID);
	return area->control->handle;
}

/*
 * 附接到由 dsa_get_handle 生成（可能在其他进程中）
 * 的句柄的区域。该区域必须以 dsa_create 创建(而非
 * dsa_create_in_place).
 */
dsa_area *
dsa_attach(dsa_handle handle)
{
	dsm_segment *segment;
	dsa_area   *area;

	/*
	 * 区域句柄实际上就是第一个段的 DSM 段句柄，
	 * 因此我们直接附接它。
	 */
	segment = dsm_attach(handle);
	if (segment == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to dynamic shared area")));

	area = attach_internal(dsm_segment_address(segment), segment, handle);

	/* 当控制段分离时进行清理。 */
	on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
				  PointerGetDatum(dsm_segment_address(segment)));

	return area;
}

/*
 * 附接到以 dsa_create_in_place 创建的区域。调用者
 * 必须设法知道创建该区域时所用的内存位置，
 * 尽管它在本进程中可能被映射到不同的虚拟地
 * 址。
 *
 * 关于释放就地区域的说明见 dsa_create_in_place，
 * 以及可选的 'segment' 参数，若包含它的内存恰好是
 * 一个 DSM 段，则可自动释放。
 */
dsa_area *
dsa_attach_in_place(void *place, dsm_segment *segment)
{
	dsa_area   *area;

	area = attach_internal(place, NULL, DSA_HANDLE_INVALID);

	/*
	 * 若提供了包含它的 DSM 段，则在控制段分离时
	 * 清理。
	 */
	if (segment != NULL)
		on_dsm_detach(segment, &dsa_on_dsm_detach_release_in_place,
					  PointerGetDatum(place));

	return area;
}

/*
 * 释放由 dsa_create_in_place 或 dsa_attach_in_place
 * 产生的 DSA 区域。'segment' 参数会被忽略，但提供一个
 * 适用于 on_dsm_detach 的接口，为想在既有 DSM 段
 * 内创建 DSA 段的用户提供便利，使其能在包含它的
 * DSM 段被分离时自动释放。
 * 'place' 应为创建该区域时所处位置的地址。
 *
 * 本回调会被自动注册到包含就地创建区域控制
 * 对象的 DSM 段上，前提是曾向
 * dsa_create_in_place 或 dsa_attach_in_place 提供段；还有
 * dsa_create。
 */
void
dsa_on_dsm_detach_release_in_place(dsm_segment *segment, Datum place)
{
	dsa_release_in_place(DatumGetPointer(place));
}

/*
 * 释放由 dsa_create_in_place 或 dsa_attach_in_place
 * 产生的 DSA 区域。'code' 参数会被忽略，但提供一个
 * 适用于 on_shmem_exit 或 before_shmem_exit 的接口，
 * 为想在 DSM 段以外的共享内存中创建 DSA 段的
 * 用户提供了便利，使其在后端退出时自动释放。
 * 'place' 应为创建该区域时所处位置的地址。
 */
void
dsa_on_shmem_exit_release_in_place(int code, Datum place)
{
	dsa_release_in_place(DatumGetPointer(place));
}

/*
 * 释放由 dsa_create_in_place 或 dsa_attach_in_place
 * 产生的 DSA 区域。最好使用某个 'dsa_on_XXX' 回调，
 * 从而自动完成这一管理，因为未能释放就地创建
 * 的区域会永久泄漏其段。
 *
 * 对于由 dsa_create 或 dsa_attach 产生的区域，作为实现
 * 细节，本函数会被自动调用。
 */
void
dsa_release_in_place(void *place)
{
	dsa_area_control *control = (dsa_area_control *) place;
	int			i;

	LWLockAcquire(&control->lock, LW_EXCLUSIVE);
	Assert(control->segment_header.magic ==
		   (DSA_SEGMENT_HEADER_MAGIC ^ control->handle ^ 0));
	Assert(control->refcnt > 0);
	if (--control->refcnt == 0)
	{
		for (i = 0; i <= control->high_segment_index; ++i)
		{
			dsm_handle	handle;

			handle = control->segment_handles[i];
			if (handle != DSM_HANDLE_INVALID)
				dsm_unpin_segment(handle);
		}
	}
	LWLockRelease(&control->lock);
}

/*
 * 使 DSA 区域保持附接，直至会话结束或显式分离。
 *
 * 默认情况下，区域由当前资源所有者拥有，这意味着
 * 该作用域结束时它们会被自动分离。
 */
void
dsa_pin_mapping(dsa_area *area)
{
	int			i;

	Assert(!area->mapping_pinned);
	area->mapping_pinned = true;

	for (i = 0; i <= area->high_segment_index; ++i)
		if (area->segment_maps[i].segment != NULL)
			dsm_pin_mapping(area->segment_maps[i].segment);
}

/*
 * 在本存储区域中分配内存。返回值是一个 dsa_pointer，
 * 它可以传递给其他进程，并可通过 dsa_get_address
 * 转换为本地指针。'flags' 是一个位图，应由
 * 以下这些值构成：
 *
 * DSA_ALLOC_HUGE 允许 >= 1GB 的分配。否则，这类分配
 * 将导致 ERROR。
 *
 * DSA_ALLOC_NO_OOM 使本函数在无可用内存，或即将
 * 超过 dsa_set_size_limit 设定的大小限制时返回
 * InvalidDsaPointer。否则，这类分配将导致 ERROR。
 *
 * DSA_ALLOC_ZERO 使分配的内存被清零。否则，新分配
 * 内存的内容是不确定的。
 *
 * 这些标志对应于 MemoryContextAllocExtended()
 * 所用的类似命名的标志。另见宏 dsa_allocate 和
 * dsa_allocate0 它们展开为本函数调用并带上常用标
 * 志。
 */
dsa_pointer
dsa_allocate_extended(dsa_area *area, size_t size, int flags)
{
	uint16		size_class;
	dsa_pointer start_pointer;
	dsa_segment_map *segment_map;
	dsa_pointer result;

	Assert(size > 0);

	/* 对单个巨型分配大小的健全性检查。 */
	if (((flags & DSA_ALLOC_HUGE) != 0 && !AllocHugeSizeIsValid(size)) ||
		((flags & DSA_ALLOC_HUGE) == 0 && !AllocSizeIsValid(size)))
		elog(ERROR, "invalid DSA memory alloc request size %zu", size);

	/*
	 * 若大于最大大小类，就直接从空闲页管理器
	 * 拿走一段连续页，而不是从池中分配对象。
	 * 此时仍会有一个跨度，但它是特殊类别的跨度，
	 * 用于管理这整个分配，并在 dsa_free 被调用时把所有
	 * 页归还给空闲页管理器。
	 */
	if (size > dsa_size_classes[lengthof(dsa_size_classes) - 1])
	{
		size_t		npages = fpm_size_to_pages(size);
		size_t		first_page;
		dsa_pointer span_pointer;
		dsa_area_pool *pool = &area->control->pools[DSA_SCLASS_SPAN_LARGE];

		/* 取得跨度对象。 */
		span_pointer = alloc_object(area, DSA_SCLASS_BLOCK_OF_SPANS);
		if (!DsaPointerIsValid(span_pointer))
		{
			/* 若未要求抑制则报错。 */
			if ((flags & DSA_ALLOC_NO_OOM) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory"),
						 errdetail("Failed on DSA request of size %zu.",
								   size)));
			return InvalidDsaPointer;
		}

		LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);

		/* 找一个用于分配的段。 */
		segment_map = get_best_segment(area, npages);
		if (segment_map == NULL)
			segment_map = make_new_segment(area, npages);
		if (segment_map == NULL)
		{
			/* 无法再建段：游戏结束。 */
			LWLockRelease(DSA_AREA_LOCK(area));
			dsa_free(area, span_pointer);

			/* 若未要求抑制则报错。 */
			if ((flags & DSA_ALLOC_NO_OOM) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of memory"),
						 errdetail("Failed on DSA request of size %zu.",
								   size)));
			return InvalidDsaPointer;
		}

		/*
		 * 向空闲页管理器索要一段连续页，应总是成功，
		 * 因为 get_best_segment 和 make_new_segment 都应当
		 * 只在确有足够连续空闲空间时才返回非 NULL
		 * 指针。若真的失败，说明我们后端私有状态中
		 * 有地方出了问题，因此用 FATAL 终止进程。
		 */
		if (!FreePageManagerGet(segment_map->fpm, npages, &first_page))
			elog(FATAL,
				 "dsa_allocate could not find %zu free pages", npages);
		LWLockRelease(DSA_AREA_LOCK(area));

		start_pointer = DSA_MAKE_POINTER(get_segment_index(area, segment_map),
										 first_page * FPM_PAGE_SIZE);

		/* 初始化跨度、页映射 */
		LWLockAcquire(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE),
					  LW_EXCLUSIVE);
		init_span(area, span_pointer, pool, start_pointer, npages,
				  DSA_SCLASS_SPAN_LARGE);
		segment_map->pagemap[first_page] = span_pointer;
		LWLockRelease(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE));

		/* 若有要求则将内存清零。 */
		if ((flags & DSA_ALLOC_ZERO) != 0)
			memset(dsa_get_address(area, start_pointer), 0, size);

		return start_pointer;
	}

	/* 把分配映射到大小类。 */
	if (size < lengthof(dsa_size_class_map) * DSA_SIZE_CLASS_MAP_QUANTUM)
	{
		int			mapidx;

		/* 较小尺寸用查找表…… */
		mapidx = ((size + DSA_SIZE_CLASS_MAP_QUANTUM - 1) /
				  DSA_SIZE_CLASS_MAP_QUANTUM) - 1;
		size_class = dsa_size_class_map[mapidx];
	}
	else
	{
		uint16		min;
		uint16		max;

		/* ……其余则用二分法查找。 */
		min = dsa_size_class_map[lengthof(dsa_size_class_map) - 1];
		max = lengthof(dsa_size_classes) - 1;

		while (min < max)
		{
			uint16		mid = (min + max) / 2;
			uint16		class_size = dsa_size_classes[mid];

			if (class_size < size)
				min = mid + 1;
			else
				max = mid;
		}

		size_class = min;
	}
	Assert(size <= dsa_size_classes[size_class]);
	Assert(size_class == 0 || size > dsa_size_classes[size_class - 1]);

	/* 尝试从相应的池中分配一个对象。 */
	result = alloc_object(area, size_class);

	/* 检查是否分配失败。 */
	if (!DsaPointerIsValid(result))
	{
		/* 若未要求抑制则报错。 */
		if ((flags & DSA_ALLOC_NO_OOM) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory"),
					 errdetail("Failed on DSA request of size %zu.", size)));
		return InvalidDsaPointer;
	}

	/* 若有要求则将内存清零。 */
	if ((flags & DSA_ALLOC_ZERO) != 0)
		memset(dsa_get_address(area, result), 0, size);

	return result;
}

/*
 * 释放 dsa_allocate 所得内存。
 */
void
dsa_free(dsa_area *area, dsa_pointer dp)
{
	dsa_segment_map *segment_map;
	int			pageno;
	dsa_pointer span_pointer;
	dsa_area_span *span;
	char	   *superblock;
	char	   *object;
	size_t		size;
	int			size_class;

	/* 确保槽位中没有 'dp' 所指的过期段。 */
	check_for_freed_segments(area);

	/* 定位对象、跨度和池。 */
	segment_map = get_segment_by_index(area, DSA_EXTRACT_SEGMENT_NUMBER(dp));
	pageno = DSA_EXTRACT_OFFSET(dp) / FPM_PAGE_SIZE;
	span_pointer = segment_map->pagemap[pageno];
	span = dsa_get_address(area, span_pointer);
	superblock = dsa_get_address(area, span->start);
	object = dsa_get_address(area, dp);
	size_class = span->size_class;
	size = dsa_size_classes[size_class];

	/*
	 * 针对位于特殊跨度中的大对象的特殊情形：我们把
	 * 这些页直接归还给空闲页管理器并释放跨度。
	 */
	if (span->size_class == DSA_SCLASS_SPAN_LARGE)
	{

#ifdef CLOBBER_FREED_MEMORY
		memset(object, 0x7f, span->npages * FPM_PAGE_SIZE);
#endif

		/* 把页归还给空闲页管理器。 */
		LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
		FreePageManagerPut(segment_map->fpm,
						   DSA_EXTRACT_OFFSET(span->start) / FPM_PAGE_SIZE,
						   span->npages);

		/* 必要时把段移到相应的桶。 */
		rebin_segment(area, segment_map);
		LWLockRelease(DSA_AREA_LOCK(area));

		/* 摘链。 */
		LWLockAcquire(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE),
					  LW_EXCLUSIVE);
		unlink_span(area, span);
		LWLockRelease(DSA_SCLASS_LOCK(area, DSA_SCLASS_SPAN_LARGE));
		/* 释放跨度对象以便复用。 */
		dsa_free(area, span_pointer);
		return;
	}

#ifdef CLOBBER_FREED_MEMORY
	memset(object, 0x7f, size);
#endif

	LWLockAcquire(DSA_SCLASS_LOCK(area, size_class), LW_EXCLUSIVE);

	/* 对象挂到跨度空闲链表。 */
	Assert(object >= superblock);
	Assert(object < superblock + DSA_SUPERBLOCK_SIZE);
	Assert((object - superblock) % size == 0);
	NextFreeObjectIndex(object) = span->firstfree;
	span->firstfree = (object - superblock) / size;
	++span->nallocatable;

	/*
	 * 看看该跨度是否需要移到别的满度类，或者需要被
	 * 释放，以将其页归还给段。
	 */
	if (span->nallocatable == 1 && span->fclass == DSA_FULLNESS_CLASSES - 1)
	{
		/*
		 * 该块之前完全填满，且位于最高编号的
		 * 满度类，该类从不会被扫描寻找空闲
		 * 区块。我们必须把它移到低一号的满度类。
		 */
		unlink_span(area, span);
		add_span_to_fullness_class(area, span, span_pointer,
								   DSA_FULLNESS_CLASSES - 2);

		/*
		 * 如果这是唯一的跨度又没有活跃跨度，那么
		 * 或许应当把该跨度移到满度类 1。（否则，如果
		 * 你恰好分配光了唯一跨度中的所有对象，它会移到
		 * 第 3 类，然后全部释放，它移到 2，接着被归
		 * 还后无活跃跨度）。
		 */
	}
	else if (span->nallocatable == span->nmax &&
			 (span->fclass != 1 || span->prevspan != InvalidDsaPointer))
	{
		/*
		 * 这整个块都已空闲，且它不是此大小类的
		 * 活跃块。把内存归还给空闲页管理器。我们不
		 * 对活跃块这样做，以防止迟滞：如果我们
		 * 反复分配并释放活跃块中唯一的区块，那么
		 * 每次都解除并重新分配该块，会非常
		 * 低效。
		 */
		destroy_superblock(area, span_pointer);
	}

	LWLockRelease(DSA_SCLASS_LOCK(area, size_class));
}

/*
 * 获取 dsa_pointer 的后端本地地址。'dp' 必须指向由给定
 * 区域分配（可能在其他进程中）且尚未被释放的内
 * 存。这可能导致有段被映射进当前进程（如果有必
 * 要），并可能导致已释放的段被解除映射。
 */
void *
dsa_get_address(dsa_area *area, dsa_pointer dp)
{
	dsa_segment_index index;
	size_t		offset;

	/* InvalidDsaPointer 转为 NULL。 */
	if (!DsaPointerIsValid(dp))
		return NULL;

	/* 处理所有与已释放段分离的请求。 */
	check_for_freed_segments(area);

	/* 将 dsa_pointer 拆分为各部分。 */
	index = DSA_EXTRACT_SEGMENT_NUMBER(dp);
	offset = DSA_EXTRACT_OFFSET(dp);
	Assert(index < DSA_MAX_SEGMENTS);

	/* 检查是否需要让本段被映射进来。 */
	if (unlikely(area->segment_maps[index].mapped_address == NULL))
	{
		/* 为效果而调（无需结果）。 */
		get_segment_by_index(area, index);
	}

	return area->segment_maps[index].mapped_address + offset;
}

/*
 * 钉住本区域，使得即使所有后端都与它分离，它也
 * 能继续存在。此时，只要某处记录了句柄，就仍然
 * 可以重新附接该区域。
 */
void
dsa_pin(dsa_area *area)
{
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	if (area->control->pinned)
	{
		LWLockRelease(DSA_AREA_LOCK(area));
		elog(ERROR, "dsa_area already pinned");
	}
	area->control->pinned = true;
	++area->control->refcnt;
	LWLockRelease(DSA_AREA_LOCK(area));
}

/*
 * 解除 dsa_pin 的效果，使得在没有后端附接于给定区域
 * 时可以释放它。仅在已调用过 dsa_pin 之
 * 后。
 */
void
dsa_unpin(dsa_area *area)
{
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	Assert(area->control->refcnt > 1);
	if (!area->control->pinned)
	{
		LWLockRelease(DSA_AREA_LOCK(area));
		elog(ERROR, "dsa_area not pinned");
	}
	area->control->pinned = false;
	--area->control->refcnt;
	LWLockRelease(DSA_AREA_LOCK(area));
}

/*
 * 为本区域设置总大小限制。每当需要从操作系统分配
 * 新段时都会检查此限制。如果新的大小限制已经被
 * 超过，则不会有立即的效果。
 *
 * 注意：当段已被释放但尚未被所有附接于它们的
 * 后端分离时，虚拟内存总用量可能会暂时大于
 * 此限制。
 */
void
dsa_set_size_limit(dsa_area *area, size_t limit)
{
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	area->control->max_total_segment_size = limit;
	LWLockRelease(DSA_AREA_LOCK(area));
}

/*
 * 激进地释放所有空闲内存，以期把 DSM 段归还给
 * 操作系统。
 */
void
dsa_trim(dsa_area *area)
{
	int			size_class;

	/*
	 * 按池的逆序进行裁减，从而最后才处理跨度的跨度，
	 * 以免在处理其他所有池时它们恰好完全变空。
	 */
	for (size_class = DSA_NUM_SIZE_CLASSES - 1; size_class >= 0; --size_class)
	{
		dsa_area_pool *pool = &area->control->pools[size_class];
		dsa_pointer span_pointer;

		if (size_class == DSA_SCLASS_SPAN_LARGE)
		{
			/* 大对象的释放本来就激进地归还段。 */
			continue;
		}

		/*
		 * 只搜索满度类 1。我们预期在那里找到完全变空的
		 * 超级块（其他满度类中完全变空的超级块会由
		 * dsa_free 归还给空闲页映射）。
		 */
		LWLockAcquire(DSA_SCLASS_LOCK(area, size_class), LW_EXCLUSIVE);
		span_pointer = pool->spans[1];
		while (DsaPointerIsValid(span_pointer))
		{
			dsa_area_span *span = dsa_get_address(area, span_pointer);
			dsa_pointer next = span->nextspan;

			if (span->nallocatable == span->nmax)
				destroy_superblock(area, span_pointer);

			span_pointer = next;
		}
		LWLockRelease(DSA_SCLASS_LOCK(area, size_class));
	}
}

/*
 * 打印出共享内存区域内部状态的调试
 * 信息。
 */
void
dsa_dump(dsa_area *area)
{
	size_t		i,
				j;

	/*
	 * 注意：它一路上获取并释放各个锁，
	 * 所以快照并不一致……
	 */

	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	check_for_freed_segments_locked(area);
	fprintf(stderr, "dsa_area handle %x:\n", area->control->handle);
	fprintf(stderr, "  max_total_segment_size: %zu\n",
			area->control->max_total_segment_size);
	fprintf(stderr, "  total_segment_size: %zu\n",
			area->control->total_segment_size);
	fprintf(stderr, "  refcnt: %d\n", area->control->refcnt);
	fprintf(stderr, "  pinned: %c\n", area->control->pinned ? 't' : 'f');
	fprintf(stderr, "  segment bins:\n");
	for (i = 0; i < DSA_NUM_SEGMENT_BINS; ++i)
	{
		if (area->control->segment_bins[i] != DSA_SEGMENT_INDEX_NONE)
		{
			dsa_segment_index segment_index;

			if (i == 0)
				fprintf(stderr,
						"    segment bin %zu (no contiguous free pages):\n", i);
			else
				fprintf(stderr,
						"    segment bin %zu (at least %d contiguous pages free):\n",
						i, 1 << (i - 1));
			segment_index = area->control->segment_bins[i];
			while (segment_index != DSA_SEGMENT_INDEX_NONE)
			{
				dsa_segment_map *segment_map;

				segment_map =
					get_segment_by_index(area, segment_index);

				fprintf(stderr,
						"      segment index %zu, usable_pages = %zu, "
						"contiguous_pages = %zu, mapped at %p\n",
						segment_index,
						segment_map->header->usable_pages,
						fpm_largest(segment_map->fpm),
						segment_map->mapped_address);
				segment_index = segment_map->header->next;
			}
		}
	}
	LWLockRelease(DSA_AREA_LOCK(area));

	fprintf(stderr, "  pools:\n");
	for (i = 0; i < DSA_NUM_SIZE_CLASSES; ++i)
	{
		bool		found = false;

		LWLockAcquire(DSA_SCLASS_LOCK(area, i), LW_EXCLUSIVE);
		for (j = 0; j < DSA_FULLNESS_CLASSES; ++j)
			if (DsaPointerIsValid(area->control->pools[i].spans[j]))
				found = true;
		if (found)
		{
			if (i == DSA_SCLASS_BLOCK_OF_SPANS)
				fprintf(stderr, "    pool for blocks of span objects:\n");
			else if (i == DSA_SCLASS_SPAN_LARGE)
				fprintf(stderr, "    pool for large object spans:\n");
			else
				fprintf(stderr,
						"    pool for size class %zu (object size %hu bytes):\n",
						i, dsa_size_classes[i]);
			for (j = 0; j < DSA_FULLNESS_CLASSES; ++j)
			{
				if (!DsaPointerIsValid(area->control->pools[i].spans[j]))
					fprintf(stderr, "      fullness class %zu is empty\n", j);
				else
				{
					dsa_pointer span_pointer = area->control->pools[i].spans[j];

					fprintf(stderr, "      fullness class %zu:\n", j);
					while (DsaPointerIsValid(span_pointer))
					{
						dsa_area_span *span;

						span = dsa_get_address(area, span_pointer);
						fprintf(stderr,
								"        span descriptor at "
								DSA_POINTER_FORMAT ", superblock at "
								DSA_POINTER_FORMAT
								", pages = %zu, objects free = %hu/%hu\n",
								span_pointer, span->start, span->npages,
								span->nallocatable, span->nmax);
						span_pointer = span->nextspan;
					}
				}
			}
		}
		LWLockRelease(DSA_SCLASS_LOCK(area, i));
	}
}

/*
 * 返回可以成功提供给
 * dsa_create_in_place.
 */
size_t
dsa_minimum_size(void)
{
	size_t		size;
	int			pages = 0;

	size = MAXALIGN(sizeof(dsa_area_control)) +
		MAXALIGN(sizeof(FreePageManager));

	/* 算出我们需要多少页，含页映射…… */
	while (((size + FPM_PAGE_SIZE - 1) / FPM_PAGE_SIZE) > pages)
	{
		++pages;
		size += sizeof(dsa_pointer);
	}

	return pages * FPM_PAGE_SIZE;
}

/*
 * dsa_create 和 dsa_create_in_place 的工作函数。
 */
static dsa_area *
create_internal(void *place, size_t size,
				int tranche_id,
				dsm_handle control_handle,
				dsm_segment *control_segment)
{
	dsa_area_control *control;
	dsa_area   *area;
	dsa_segment_map *segment_map;
	size_t		usable_pages;
	size_t		total_pages;
	size_t		metadata_bytes;
	int			i;

	/* 对可用工作空间的健全性检查。 */
	if (size < dsa_minimum_size())
		elog(ERROR, "dsa_area space must be at least %zu, but %zu provided",
			 dsa_minimum_size(), size);

	/* 现在算出可用空间有多少 */
	total_pages = size / FPM_PAGE_SIZE;
	metadata_bytes =
		MAXALIGN(sizeof(dsa_area_control)) +
		MAXALIGN(sizeof(FreePageManager)) +
		total_pages * sizeof(dsa_pointer);
	/* 补齐填充至下一页边界。 */
	if (metadata_bytes % FPM_PAGE_SIZE != 0)
		metadata_bytes += FPM_PAGE_SIZE - (metadata_bytes % FPM_PAGE_SIZE);
	Assert(metadata_bytes <= size);
	usable_pages = (size - metadata_bytes) / FPM_PAGE_SIZE;

	/*
	 * 初始化位于空间开头的 dsa_area_control 对
	 * 象。
	 */
	control = (dsa_area_control *) place;
	memset(place, 0, sizeof(*control));
	control->segment_header.magic =
		DSA_SEGMENT_HEADER_MAGIC ^ control_handle ^ 0;
	control->segment_header.next = DSA_SEGMENT_INDEX_NONE;
	control->segment_header.prev = DSA_SEGMENT_INDEX_NONE;
	control->segment_header.usable_pages = usable_pages;
	control->segment_header.freed = false;
	control->segment_header.size = DSA_INITIAL_SEGMENT_SIZE;
	control->handle = control_handle;
	control->max_total_segment_size = (size_t) -1;
	control->total_segment_size = size;
	control->segment_handles[0] = control_handle;
	for (i = 0; i < DSA_NUM_SEGMENT_BINS; ++i)
		control->segment_bins[i] = DSA_SEGMENT_INDEX_NONE;
	control->refcnt = 1;
	control->lwlock_tranche_id = tranche_id;

	/*
	 * 创建本后端将用来访问该区域的
	 * dsa_area 对象。其他后端需要通过附接来取得自己的
	 * 对象。
	 */
	area = palloc(sizeof(dsa_area));
	area->control = control;
	area->mapping_pinned = false;
	memset(area->segment_maps, 0, sizeof(dsa_segment_map) * DSA_MAX_SEGMENTS);
	area->high_segment_index = 0;
	area->freed_segment_counter = 0;
	LWLockInitialize(&control->lock, control->lwlock_tranche_id);
	for (i = 0; i < DSA_NUM_SIZE_CLASSES; ++i)
		LWLockInitialize(DSA_SCLASS_LOCK(area, i),
						 control->lwlock_tranche_id);

	/* 为本进程的映射建立段映射。 */
	segment_map = &area->segment_maps[0];
	segment_map->segment = control_segment;
	segment_map->mapped_address = place;
	segment_map->header = (dsa_segment_header *) place;
	segment_map->fpm = (FreePageManager *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_area_control)));
	segment_map->pagemap = (dsa_pointer *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_area_control)) +
		 MAXALIGN(sizeof(FreePageManager)));

	/* 建立空闲页映射。 */
	FreePageManagerInitialize(segment_map->fpm, segment_map->mapped_address);
	/* 若大小为 dsa_minimum_size() 则可有 0 个可用页 */

	if (usable_pages > 0)
		FreePageManagerPut(segment_map->fpm, metadata_bytes / FPM_PAGE_SIZE,
						   usable_pages);

	/* 把本段放入相应的桶。 */
	control->segment_bins[contiguous_pages_to_segment_bin(usable_pages)] = 0;
	segment_map->header->bin = contiguous_pages_to_segment_bin(usable_pages);

	return area;
}

/*
 * dsa_attach 和 dsa_attach_in_place 的工作函数。
 */
static dsa_area *
attach_internal(void *place, dsm_segment *segment, dsa_handle handle)
{
	dsa_area_control *control;
	dsa_area   *area;
	dsa_segment_map *segment_map;

	control = (dsa_area_control *) place;
	Assert(control->handle == handle);
	Assert(control->segment_handles[0] == handle);
	Assert(control->segment_header.magic ==
		   (DSA_SEGMENT_HEADER_MAGIC ^ handle ^ 0));

	/* 构建后端本地的区域对象。 */
	area = palloc(sizeof(dsa_area));
	area->control = control;
	area->mapping_pinned = false;
	memset(&area->segment_maps[0], 0,
		   sizeof(dsa_segment_map) * DSA_MAX_SEGMENTS);
	area->high_segment_index = 0;

	/* 为本进程的映射建立段映射。 */
	segment_map = &area->segment_maps[0];
	segment_map->segment = segment; /* NULL（就地） */
	segment_map->mapped_address = place;
	segment_map->header = (dsa_segment_header *) segment_map->mapped_address;
	segment_map->fpm = (FreePageManager *)
		(segment_map->mapped_address + MAXALIGN(sizeof(dsa_area_control)));
	segment_map->pagemap = (dsa_pointer *)
		(segment_map->mapped_address + MAXALIGN(sizeof(dsa_area_control)) +
		 MAXALIGN(sizeof(FreePageManager)));

	/* 增加引用计数。 */
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	if (control->refcnt == 0)
	{
		/* 我们不能附接到已被销毁的 DSA 区域。 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to dynamic shared area")));
	}
	++control->refcnt;
	area->freed_segment_counter = area->control->freed_segment_counter;
	LWLockRelease(DSA_AREA_LOCK(area));

	return area;
}

/*
 * 向指定池的满度类 1 加入一个新跨度。
 */
static void
init_span(dsa_area *area,
		  dsa_pointer span_pointer,
		  dsa_area_pool *pool, dsa_pointer start, size_t npages,
		  uint16 size_class)
{
	dsa_area_span *span = dsa_get_address(area, span_pointer);
	size_t		obsize = dsa_size_classes[size_class];

	/*
	 * 必须持有每池锁，因为我们要操纵此池的跨度
	 * 列表。
	 */
	Assert(LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));

	/* 把此跨度推到满度类 1 跨度列表的前端。 */
	if (DsaPointerIsValid(pool->spans[1]))
	{
		dsa_area_span *head = (dsa_area_span *)
			dsa_get_address(area, pool->spans[1]);

		head->prevspan = span_pointer;
	}
	span->pool = DsaAreaPoolToDsaPointer(area, pool);
	span->nextspan = pool->spans[1];
	span->prevspan = InvalidDsaPointer;
	pool->spans[1] = span_pointer;

	span->start = start;
	span->npages = npages;
	span->size_class = size_class;
	span->ninitialized = 0;
	if (size_class == DSA_SCLASS_BLOCK_OF_SPANS)
	{
		/*
		 * 跨度的跨度块包含其自身的描述符，因此要把
		 * 一个对象标记为已初始化，可分配对象数减一。
		 * 在这里这样做还会顺带把 nmax 减一，
		 * 这对于确保在正确的时机释放此对象非常重要。
		 *
		 */
		span->ninitialized = 1;
		span->nallocatable = FPM_PAGE_SIZE / obsize - 1;
	}
	else if (size_class != DSA_SCLASS_SPAN_LARGE)
		span->nallocatable = DSA_SUPERBLOCK_SIZE / obsize;
	span->firstfree = DSA_SPAN_NOTHING_FREE;
	span->nmax = span->nallocatable;
	span->fclass = 1;
}

/*
 * 把一个满度类中的第一个跨度转移到另一个满度类
 * 头部。
 */
static bool
transfer_first_span(dsa_area *area,
					dsa_area_pool *pool, int fromclass, int toclass)
{
	dsa_pointer span_pointer;
	dsa_area_span *span;
	dsa_area_span *nextspan;

	/* 源列表为空时无法这样做。 */
	span_pointer = pool->spans[fromclass];
	if (!DsaPointerIsValid(span_pointer))
		return false;

	/* 从源列表头部移除跨度。 */
	span = dsa_get_address(area, span_pointer);
	pool->spans[fromclass] = span->nextspan;
	if (DsaPointerIsValid(span->nextspan))
	{
		nextspan = (dsa_area_span *)
			dsa_get_address(area, span->nextspan);
		nextspan->prevspan = InvalidDsaPointer;
	}

	/* 加到目标列表头部。 */
	span->nextspan = pool->spans[toclass];
	pool->spans[toclass] = span_pointer;
	if (DsaPointerIsValid(span->nextspan))
	{
		nextspan = (dsa_area_span *)
			dsa_get_address(area, span->nextspan);
		nextspan->prevspan = span_pointer;
	}
	span->fclass = toclass;

	return true;
}

/*
 * 从给定区域分配一个请求大小类的对象。
 */
static inline dsa_pointer
alloc_object(dsa_area *area, int size_class)
{
	dsa_area_pool *pool = &area->control->pools[size_class];
	dsa_area_span *span;
	dsa_pointer block;
	dsa_pointer result;
	char	   *object;
	size_t		size;

	/*
	 * 尽管 ensure_active_superblock 若需要分配新跨度时
	 * 会转而调用 alloc_object，那也总是从不同的池进行，
	 * 而且获取锁的顺序总是相同，所以本函数
	 * 在整个函数期间持有此锁没有问题。
	 */
	Assert(!LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));
	LWLockAcquire(DSA_SCLASS_LOCK(area, size_class), LW_EXCLUSIVE);

	/*
	 * 若无活跃超级块，我们必须成功获得一个，否则
	 * 请求失败。
	 */
	if (!DsaPointerIsValid(pool->spans[1]) &&
		!ensure_active_superblock(area, pool, size_class))
	{
		result = InvalidDsaPointer;
	}
	else
	{
		/*
		 * 此时满度类 1 中应有一个块，并且它绝不应完全
		 * 填满。因此我们既可以从空闲列表弹出一个对象，
		 * 如果失败，也可以初始化一个新对象。
		 */
		Assert(DsaPointerIsValid(pool->spans[1]));
		span = (dsa_area_span *)
			dsa_get_address(area, pool->spans[1]);
		Assert(span->nallocatable > 0);
		block = span->start;
		Assert(size_class < DSA_NUM_SIZE_CLASSES);
		size = dsa_size_classes[size_class];
		if (span->firstfree != DSA_SPAN_NOTHING_FREE)
		{
			result = block + span->firstfree * size;
			object = dsa_get_address(area, result);
			span->firstfree = NextFreeObjectIndex(object);
		}
		else
		{
			result = block + span->ninitialized * size;
			++span->ninitialized;
		}
		--span->nallocatable;

		/* 若现已填满，则移到编号最高的满度类。 */
		if (span->nallocatable == 0)
			transfer_first_span(area, pool, 1, DSA_FULLNESS_CLASSES - 1);
	}

	Assert(LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));
	LWLockRelease(DSA_SCLASS_LOCK(area, size_class));

	return result;
}

/*
 * 确保存在一个活跃（即满度类 1）超级块，除非所有
 * 既有超级块都完全填满且无法再分配。
 *
 * 满度类 K（0..N 中）大致意在表示利用率至少为 K/N
 * 的块，但我们只对编号最高的满度类严格执行这一点，
 * 即编号最高的那个满度类——它恰好只包含
 * 完全填满的那些块。除此之外，
 * 一个块处于比其逻辑上所属类编号更高的满度类是
 * 可以接受的。此外，活跃块——它始终是
 * 满度类 1 中的第一个块——被允许拥有更高的
 * 分配百分比；在它完全填满之前我们不会移动它，
 * 而一旦完全填满，它就会进入
 * 编号最高的满度类。
 *
 * 活跃块是满度类 1 而非满度类 0 的头部，这看起来
 * 可能有些奇怪，但其他分配器的经验表明，从一个
 * 半满的块而不是近乎空的块中分配通常更好。在合理
 * 可行的范围内，我们希望避免在一个很快就会变空
 * 的块中执行新的分配，以免
 * 其很快变空。
 */
static bool
ensure_active_superblock(dsa_area *area, dsa_area_pool *pool,
						 int size_class)
{
	dsa_pointer span_pointer;
	dsa_pointer start_pointer;
	size_t		obsize = dsa_size_classes[size_class];
	size_t		nmax;
	int			fclass;
	size_t		npages = 1;
	size_t		first_page;
	size_t		i;
	dsa_segment_map *segment_map;

	Assert(LWLockHeldByMe(DSA_SCLASS_LOCK(area, size_class)));

	/*
	 * 计算此大小类的块中所能容纳的对象数，
	 * 跨度的跨度块只是一页，且第一个
	 * 对象不可用，因为它描述的是跨度块自
	 * 身。
	 */
	if (size_class == DSA_SCLASS_BLOCK_OF_SPANS)
		nmax = FPM_PAGE_SIZE / obsize - 1;
	else
		nmax = DSA_SUPERBLOCK_SIZE / obsize;

	/*
	 * 若满度类 1 为空，尝试扫描编号更高的满度类
	 * 来为它找一个跨度（不包括最后一个类，
	 * 该类中的块必然全部完全填满）。
	 */
	for (fclass = 2; fclass < DSA_FULLNESS_CLASSES - 1; ++fclass)
	{
		span_pointer = pool->spans[fclass];

		while (DsaPointerIsValid(span_pointer))
		{
			int			tfclass;
			dsa_area_span *span;
			dsa_area_span *nextspan;
			dsa_area_span *prevspan;
			dsa_pointer next_span_pointer;

			span = (dsa_area_span *)
				dsa_get_address(area, span_pointer);
			next_span_pointer = span->nextspan;

			/* 算出应当由哪个满度类容纳此跨度。 */
			tfclass = (nmax - span->nallocatable)
				* (DSA_FULLNESS_CLASSES - 1) / nmax;

			/* 下一跨度。 */
			if (DsaPointerIsValid(span->nextspan))
				nextspan = (dsa_area_span *)
					dsa_get_address(area, span->nextspan);
			else
				nextspan = NULL;

			/*
			 * 若利用率已降到使其应属于其他满度类的程度，
			 * 则把它移到那个类。
			 */
			if (tfclass < fclass)
			{
				/* 从当前满度类列表中移除。 */
				if (pool->spans[fclass] == span_pointer)
				{
					/* 它是头部；移除它。 */
					Assert(!DsaPointerIsValid(span->prevspan));
					pool->spans[fclass] = span->nextspan;
					if (nextspan != NULL)
						nextspan->prevspan = InvalidDsaPointer;
				}
				else
				{
					/* 它不是头部。 */
					Assert(DsaPointerIsValid(span->prevspan));
					prevspan = (dsa_area_span *)
						dsa_get_address(area, span->prevspan);
					prevspan->nextspan = span->nextspan;
				}
				if (nextspan != NULL)
					nextspan->prevspan = span->prevspan;

				/* 推到新满度类列表的头部。 */
				span->nextspan = pool->spans[tfclass];
				pool->spans[tfclass] = span_pointer;
				span->prevspan = InvalidDsaPointer;
				if (DsaPointerIsValid(span->nextspan))
				{
					nextspan = (dsa_area_span *)
						dsa_get_address(area, span->nextspan);
					nextspan->prevspan = span_pointer;
				}
				span->fclass = tfclass;
			}

			/* 前进到下一个跨度。 */
			span_pointer = next_span_pointer;
		}

		/* 若已找到合适的块则停止。 */
		if (DsaPointerIsValid(pool->spans[1]))
			return true;
	}

	/*
	 * 如果没有本应属于满度类 1 的块，就从其他某个满度
	 * 类中挑一个并强行移过去，这样我们就有了一个分配
	 * 目标。我们的最后选择是转移一个几乎为空的块
	 * （放任不管它可能很快完全变空），但即便是
	 * 这样做也比失败要好，而如果完全没有含空闲空间
	 * 的块，那我们就只能失败。
	 */
	Assert(!DsaPointerIsValid(pool->spans[1]));
	for (fclass = 2; fclass < DSA_FULLNESS_CLASSES - 1; ++fclass)
		if (transfer_first_span(area, pool, fclass, 1))
			return true;
	if (!DsaPointerIsValid(pool->spans[1]) &&
		transfer_first_span(area, pool, 0, 1))
		return true;

	/*
	 * 未找到含空闲对象的既有跨度，因此需要分配
	 * 一个新超级块并构建一个新跨度来管理它。
	 *
	 * 首先，获取一个 dsa_area_span 对象来描述新的超级
	 * 块……除非本次分配正是针对一个 dsa_area_span 对象，
	 * 那样肯定行不通。我们的处理方式是内联存储描述
	 * 跨度块的跨度。
	 */
	if (size_class != DSA_SCLASS_BLOCK_OF_SPANS)
	{
		span_pointer = alloc_object(area, DSA_SCLASS_BLOCK_OF_SPANS);
		if (!DsaPointerIsValid(span_pointer))
			return false;
		npages = DSA_PAGES_PER_SUPERBLOCK;
	}

	/* 找到或创建一个段并分配超级块。 */
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	segment_map = get_best_segment(area, npages);
	if (segment_map == NULL)
	{
		segment_map = make_new_segment(area, npages);
		if (segment_map == NULL)
		{
			LWLockRelease(DSA_AREA_LOCK(area));
			return false;
		}
	}

	/*
	 * 这不应发生：get_best_segment() 或 make_new_segment()
	 * 已承诺我们能成功分配 npages。
	 */
	if (!FreePageManagerGet(segment_map->fpm, npages, &first_page))
		elog(FATAL,
			 "dsa_allocate could not find %zu free pages for superblock",
			 npages);
	LWLockRelease(DSA_AREA_LOCK(area));

	/* 计算超级块的起始位置。 */
	start_pointer =
		DSA_MAKE_POINTER(get_segment_index(area, segment_map),
						 first_page * FPM_PAGE_SIZE);

	/*
	 * 若为跨度的跨度块，则直接从已分配空间刻
	 * 出描述符。
	 */
	if (size_class == DSA_SCLASS_BLOCK_OF_SPANS)
	{
		/*
		 * 我们有一个指向段内的指针。需要用段下标和段内
		 * 的偏移量来构建一个 dsa_pointer。
		 */
		span_pointer = start_pointer;
	}

	/* 初始化跨度、页映射 */
	init_span(area, span_pointer, pool, start_pointer, npages, size_class);
	for (i = 0; i < npages; ++i)
		segment_map->pagemap[first_page + i] = span_pointer;

	return true;
}

/*
 * 返回与给定段下标对应的段映射，如有必要则将段
 * 映射进来。对于内部段簿记，调用本函数时持有区域
 * 锁。dsa_free 和 dsa_get_address 也会调用本函数，
 * 但不加任何锁，其依据是：它们持有一个已知活跃的
 * 段下标，并且它们总会调用 check_for_freed_segments，
 * 确保占据同槽位的已释放段先被分离。
 */
static dsa_segment_map *
get_segment_by_index(dsa_area *area, dsa_segment_index index)
{
	if (unlikely(area->segment_maps[index].mapped_address == NULL))
	{
		dsm_handle	handle;
		dsm_segment *segment;
		dsa_segment_map *segment_map;

		/*
		 * 如果到达此处的是 dsa_free 或 dsa_get_address，则被
		 * 引用段中必须至少有一个对象已被分配。否则，
		 * 其调用者有二次释放或释放后访问缺陷，而我们
		 * 根本无从检测。因此可知，不持锁访问此
		 * 槽位是安全的；它不会在我们脚下被改变。
		 * 此外，我们还知道可以看到槽位的最新内容，
		 * 如 check_for_freed_segments 所述，那些函数到达
		 * 此处之前都会调用它。
		 */
		handle = area->control->segment_handles[index];

		/* 尝试访问未使用的槽位是个错误。 */
		if (handle == DSM_HANDLE_INVALID)
			elog(ERROR,
				 "dsa_area could not attach to a segment that has been freed");

		segment = dsm_attach(handle);
		if (segment == NULL)
			elog(ERROR, "dsa_area could not attach to segment");
		if (area->mapping_pinned)
			dsm_pin_mapping(segment);
		segment_map = &area->segment_maps[index];
		segment_map->segment = segment;
		segment_map->mapped_address = dsm_segment_address(segment);
		segment_map->header =
			(dsa_segment_header *) segment_map->mapped_address;
		segment_map->fpm = (FreePageManager *)
			(segment_map->mapped_address +
			 MAXALIGN(sizeof(dsa_segment_header)));
		segment_map->pagemap = (dsa_pointer *)
			(segment_map->mapped_address +
			 MAXALIGN(sizeof(dsa_segment_header)) +
			 MAXALIGN(sizeof(FreePageManager)));

		/* 记住本后端曾映射过的最高下标。 */
		if (area->high_segment_index < index)
			area->high_segment_index = index;

		Assert(segment_map->header->magic ==
			   (DSA_SEGMENT_HEADER_MAGIC ^ area->control->handle ^ index));
	}

	/*
	 * dsa_get_address() 和 dsa_free() 的调用者不持有区域
	 * 锁，但地址不活跃就是调用代码缺陷和
	 * 未定义行为（即如果该段可能已被释放，它们就是在
	 * 使用悬空指针）。
	 *
	 * 对于持区域锁操纵 segment_bins 列表的 dsa.c 代码，
	 * 如果在这里到达一个已释放的段就是缺陷。在被标记
	 * 为已释放之后，任何后端对它唯一应做的就是解除其
	 * 映射，并且它应当总是已经在此
	 * check_for_freed_segments_locked() 中先完成，再解析
	 * 为 segment_map。
	 *
	 * 无论哪种方式，都可断言返回的不是已释放段。
	 */
	Assert(!area->segment_maps[index].header->freed);

	return &area->segment_maps[index];
}

/*
 * 把一个超级块归还给空闲页管理器。如果底层段已
 * 完全变空，则把它归还给操作系统。
 *
 * 必须持有相应的池锁。
 */
static void
destroy_superblock(dsa_area *area, dsa_pointer span_pointer)
{
	dsa_area_span *span = dsa_get_address(area, span_pointer);
	int			size_class = span->size_class;
	dsa_segment_map *segment_map;


	/* 把它从满度类列表中移除。 */
	unlink_span(area, span);

	/*
	 * 注意：这里我们在已持有每池锁的情况下获取区域
	 * 锁。我们绝不持有区域锁之后再取池锁，否则
	 * 就会死锁。
	 */
	LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
	check_for_freed_segments_locked(area);
	segment_map =
		get_segment_by_index(area, DSA_EXTRACT_SEGMENT_NUMBER(span->start));
	FreePageManagerPut(segment_map->fpm,
					   DSA_EXTRACT_OFFSET(span->start) / FPM_PAGE_SIZE,
					   span->npages);
	/* 检查段现在是否完全变空。 */
	if (fpm_largest(segment_map->fpm) == segment_map->header->usable_pages)
	{
		dsa_segment_index index = get_segment_index(area, segment_map);

		/* 若不是带有额外控制数据的段，则释放它。 */
		if (index != 0)
		{
			/*
			 * 把它归还给操作系统，并让其他后端能够检测到
			 * 自己需分离。
			 */
			unlink_segment(area, segment_map);
			segment_map->header->freed = true;
			Assert(area->control->total_segment_size >=
				   segment_map->header->size);
			area->control->total_segment_size -=
				segment_map->header->size;
			dsm_unpin_segment(dsm_segment_handle(segment_map->segment));
			dsm_detach(segment_map->segment);
			area->control->segment_handles[index] = DSM_HANDLE_INVALID;
			++area->control->freed_segment_counter;
			segment_map->segment = NULL;
			segment_map->header = NULL;
			segment_map->mapped_address = NULL;
		}
	}

	/* 必要时把段移到相应的桶。 */
	if (segment_map->header != NULL)
		rebin_segment(area, segment_map);

	LWLockRelease(DSA_AREA_LOCK(area));

	/*
	 * 跨度的跨度块把描述自身的跨度存储在块内，
	 * 因此释放该存储会隐式地释放描述符，
	 * 如果这是任何其他类型的块，我们就需要另行释放
	 * 跨度对象。这次对 dsa_free 的递归调用会获取跨度池
	 * 的锁。我们不可能死锁，因为获取顺序总是先某个
	 * 其他池然后才是跨度池。
	 */
	if (size_class != DSA_SCLASS_BLOCK_OF_SPANS)
		dsa_free(area, span_pointer);
}

static void
unlink_span(dsa_area *area, dsa_area_span *span)
{
	if (DsaPointerIsValid(span->nextspan))
	{
		dsa_area_span *next = dsa_get_address(area, span->nextspan);

		next->prevspan = span->prevspan;
	}
	if (DsaPointerIsValid(span->prevspan))
	{
		dsa_area_span *prev = dsa_get_address(area, span->prevspan);

		prev->nextspan = span->nextspan;
	}
	else
	{
		dsa_area_pool *pool = dsa_get_address(area, span->pool);

		pool->spans[span->fclass] = span->nextspan;
	}
}

static void
add_span_to_fullness_class(dsa_area *area, dsa_area_span *span,
						   dsa_pointer span_pointer,
						   int fclass)
{
	dsa_area_pool *pool = dsa_get_address(area, span->pool);

	if (DsaPointerIsValid(pool->spans[fclass]))
	{
		dsa_area_span *head = dsa_get_address(area,
											  pool->spans[fclass]);

		head->prevspan = span_pointer;
	}
	span->prevspan = InvalidDsaPointer;
	span->nextspan = pool->spans[fclass];
	pool->spans[fclass] = span_pointer;
	span->fclass = fclass;
}

/*
 * 从本进程曾创建或附接的区域分离。
 */
void
dsa_detach(dsa_area *area)
{
	int			i;

	/* 与所有段分离。 */
	for (i = 0; i <= area->high_segment_index; ++i)
		if (area->segment_maps[i].segment != NULL)
			dsm_detach(area->segment_maps[i].segment);

	/*
	 * 注意，"分离"（= 与 DSM 段分离）不包括
	 * "释放"（= 调整引用计数）。最好能合并两者，
	 * 但客户端代码可能永远不会走到
	 * 调用 dsa_detach 这一步（由于错误路径），而任何
	 * 特定段上的分离钩子此时再分离区域中其他段都
	 * 太迟了，非错误路径有"泄漏"警告之险。
	 */

	/* 释放后端本地区域对象。 */
	pfree(area);
}

/*
 * 把段从包含它的桶中解除链接。
 */
static void
unlink_segment(dsa_area *area, dsa_segment_map *segment_map)
{
	if (segment_map->header->prev != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *prev;

		prev = get_segment_by_index(area, segment_map->header->prev);
		prev->header->next = segment_map->header->next;
	}
	else
	{
		Assert(area->control->segment_bins[segment_map->header->bin] ==
			   get_segment_index(area, segment_map));
		area->control->segment_bins[segment_map->header->bin] =
			segment_map->header->next;
	}
	if (segment_map->header->next != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *next;

		next = get_segment_by_index(area, segment_map->header->next);
		next->header->prev = segment_map->header->prev;
	}
}

/*
 * 找到一个能满足 'npages' 连续内存请求的段，或
 * 在找不到时返回 NULL。这可能涉及附接之前未附接
 * 的段，以便我们能查询其空闲页
 * 映射。
 */
static dsa_segment_map *
get_best_segment(dsa_area *area, size_t npages)
{
	size_t		bin;

	Assert(LWLockHeldByMe(DSA_AREA_LOCK(area)));
	check_for_freed_segments_locked(area);

	/*
	 * 从第一个*可能*拥有足够连续页的桶开始搜
	 * 索。
	 */
	for (bin = contiguous_pages_to_segment_bin(npages);
		 bin < DSA_NUM_SEGMENT_BINS;
		 ++bin)
	{
		/*
		 * 此桶中任何段都应具有的最低连续大小，
		 * 若看到更小的段，就会重新分桶。
		 */
		size_t		threshold = (size_t) 1 << (bin - 1);
		dsa_segment_index segment_index;

		/* 在此桶中搜索有足够连续空间的段。 */
		segment_index = area->control->segment_bins[bin];
		while (segment_index != DSA_SEGMENT_INDEX_NONE)
		{
			dsa_segment_map *segment_map;
			dsa_segment_index next_segment_index;
			size_t		contiguous_pages;

			segment_map = get_segment_by_index(area, segment_index);
			next_segment_index = segment_map->header->next;
			contiguous_pages = fpm_largest(segment_map->fpm);

			/* 对请求不够，但对此桶仍然足够。 */
			if (contiguous_pages >= threshold && contiguous_pages < npages)
			{
				segment_index = next_segment_index;
				continue;
			}

			/* 若它不再位于适当的桶中则重新分桶。 */
			if (contiguous_pages < threshold)
			{
				rebin_segment(area, segment_map);

				/*
				 * 但继续往下执行，看看它是否足以满足
				 * 这个请求……
				 */
			}

			/* 检查是否完成。 */
			if (contiguous_pages >= npages)
				return segment_map;

			/* 继续搜索同一个桶。 */
			segment_index = next_segment_index;
		}
	}

	/* 未找到 */
	return NULL;
}

/*
 * 创建一个至少能处理 requested_pages 的新段。如果
 * 会超过请求的总大小限制或最大允许段数，则返回
 * NULL。
 */
static dsa_segment_map *
make_new_segment(dsa_area *area, size_t requested_pages)
{
	dsa_segment_index new_index;
	size_t		metadata_bytes;
	size_t		total_size;
	size_t		total_pages;
	size_t		usable_pages;
	dsa_segment_map *segment_map;
	dsm_segment *segment;

	Assert(LWLockHeldByMe(DSA_AREA_LOCK(area)));

	/* 找一个未用的段槽位（暂为线性扫描）。 */
	for (new_index = 1; new_index < DSA_MAX_SEGMENTS; ++new_index)
	{
		if (area->control->segment_handles[new_index] == DSM_HANDLE_INVALID)
			break;
	}
	if (new_index == DSA_MAX_SEGMENTS)
		return NULL;

	/*
	 * 如果总大小限制已被超过，那么我们提前退出，
	 * 从而避免下面无符号表达式中的算术回绕。
	 */
	if (area->control->total_segment_size >=
		area->control->max_total_segment_size)
		return NULL;

	/*
	 * 大小至少应与请求的一样大，且至少足以遵循
	 * 一个每次我们创建新段时都大约使总存储翻倍的
	 * 几何级数。我们使用几何增长，因底层 DSM 系统
	 * 并不是为大量段而设计的（否则
	 * 我们甚至可以考虑只为每个大分配和每个超级块
	 * 使用一个 DSM 段，那样
	 * 就无需再使用 FreePageManager）。
	 *
	 * 我们先确定段的总大小，以便产生整齐的
	 * 2 的幂大小的段。如果将来转向巨型页，
	 * 这是个好特性。然后我们反推出可以容纳的
	 * 页数。
	 */
	total_size = DSA_INITIAL_SEGMENT_SIZE *
		((size_t) 1 << (new_index / DSA_NUM_SEGMENTS_AT_EACH_SIZE));
	total_size = Min(total_size, DSA_MAX_SEGMENT_SIZE);
	total_size = Min(total_size,
					 area->control->max_total_segment_size -
					 area->control->total_segment_size);

	total_pages = total_size / FPM_PAGE_SIZE;
	metadata_bytes =
		MAXALIGN(sizeof(dsa_segment_header)) +
		MAXALIGN(sizeof(FreePageManager)) +
		sizeof(dsa_pointer) * total_pages;

	/* 补齐填充至下一页边界。 */
	if (metadata_bytes % FPM_PAGE_SIZE != 0)
		metadata_bytes += FPM_PAGE_SIZE - (metadata_bytes % FPM_PAGE_SIZE);
	if (total_size <= metadata_bytes)
		return NULL;
	usable_pages = (total_size - metadata_bytes) / FPM_PAGE_SIZE;
	Assert(metadata_bytes + usable_pages * FPM_PAGE_SIZE <= total_size);

	/* 看是否足够…… */
	if (requested_pages > usable_pages)
	{
		/*
		 * 我们将创建一个奇数大小的段，从请求的页数
		 * 正向推算。
		 */
		usable_pages = requested_pages;
		metadata_bytes =
			MAXALIGN(sizeof(dsa_segment_header)) +
			MAXALIGN(sizeof(FreePageManager)) +
			usable_pages * sizeof(dsa_pointer);

		/* 补齐填充至下一页边界。 */
		if (metadata_bytes % FPM_PAGE_SIZE != 0)
			metadata_bytes += FPM_PAGE_SIZE - (metadata_bytes % FPM_PAGE_SIZE);
		total_size = metadata_bytes + usable_pages * FPM_PAGE_SIZE;

		/* 对 dsa_pointer 寻址方案而言太大了吗？ */
		if (total_size > DSA_MAX_SEGMENT_SIZE)
			return NULL;

		/* 这会超过限制吗？ */
		if (total_size > area->control->max_total_segment_size -
			area->control->total_segment_size)
			return NULL;
	}

	/* 创建段。 */
	segment = dsm_create(total_size, 0);
	if (segment == NULL)
		return NULL;
	dsm_pin_segment(segment);
	if (area->mapping_pinned)
		dsm_pin_mapping(segment);

	/* 把句柄存入共享内存以便按下标查找。 */
	area->control->segment_handles[new_index] =
		dsm_segment_handle(segment);
	/* 追踪本区域历史上的最高段下标。 */
	if (area->control->high_segment_index < new_index)
		area->control->high_segment_index = new_index;
	/* 追踪本后端曾映射过的最高段下标。 */
	if (area->high_segment_index < new_index)
		area->high_segment_index = new_index;
	/* 追踪所有段的总大小。 */
	area->control->total_segment_size += total_size;
	Assert(area->control->total_segment_size <=
		   area->control->max_total_segment_size);

	/* 在本后端为该段构建段映射。 */
	segment_map = &area->segment_maps[new_index];
	segment_map->segment = segment;
	segment_map->mapped_address = dsm_segment_address(segment);
	segment_map->header = (dsa_segment_header *) segment_map->mapped_address;
	segment_map->fpm = (FreePageManager *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_segment_header)));
	segment_map->pagemap = (dsa_pointer *)
		(segment_map->mapped_address +
		 MAXALIGN(sizeof(dsa_segment_header)) +
		 MAXALIGN(sizeof(FreePageManager)));

	/* 建立空闲页映射。 */
	FreePageManagerInitialize(segment_map->fpm, segment_map->mapped_address);
	FreePageManagerPut(segment_map->fpm, metadata_bytes / FPM_PAGE_SIZE,
					   usable_pages);

	/* 建立段头部并把它放入相应的桶。 */
	segment_map->header->magic =
		DSA_SEGMENT_HEADER_MAGIC ^ area->control->handle ^ new_index;
	segment_map->header->usable_pages = usable_pages;
	segment_map->header->size = total_size;
	segment_map->header->bin = contiguous_pages_to_segment_bin(usable_pages);
	segment_map->header->prev = DSA_SEGMENT_INDEX_NONE;
	segment_map->header->next =
		area->control->segment_bins[segment_map->header->bin];
	segment_map->header->freed = false;
	area->control->segment_bins[segment_map->header->bin] = new_index;
	if (segment_map->header->next != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *next =
			get_segment_by_index(area, segment_map->header->next);

		Assert(next->header->bin == segment_map->header->bin);
		next->header->prev = new_index;
	}

	return segment_map;
}

/*
 * 检查是否有段已被 destroy_superblock 释放，以便我们
 * 能在本后端与它们分离。本函数由 dsa_get_address
 * 和 dsa_free 调用，以确保它们收到的 dsa_pointer 能
 * 被解析到正确的段。
 *
 * 我们想要防范的危险是：本后端中给定的槽位里可能
 * 还映射着一个旧段，而它们手中的 dsa_pointer 可能
 * 指向同一槽位中的某个新段。因此这些函数必须确保
 * 处理所有在本进程收到该 dsa_pointer 之前就已生成
 * 的与已释放段分离的指令，然后才调用
 * get_segment_by_index。
 */
static void
check_for_freed_segments(dsa_area *area)
{
	size_t		freed_segment_counter;

	/*
	 * 任何其他已释放段的进程都在持有 LWLock 的
	 * 同时递增 freed_segment_counter，而这必须先于任何
	 * 后端在持有 LWLock 的情况下在同一槽位中创建新段
	 * 的操作，而这又必须先于任何指向新段、可能到达
	 * 我们这里的 dsa_pointer 的创建，并且调用者必须已经
	 * 使用适当的内存同步（某种锁或原子原语或系统调
	 * 用）把该 dsa_pointer 发送到本进程。因此，在读取侧
	 * 我们需要做的只是要求 freed_segment_counter 的加载
	 * 发生在调用者加载其 dsa_pointer 之后，这样我们就能
	 * 确保检测到在该 dsa_pointer 到达本进程时任何已被
	 * 释放的段。
	 */
	pg_read_barrier();
	freed_segment_counter = area->control->freed_segment_counter;
	if (unlikely(area->freed_segment_counter != freed_segment_counter))
	{
		/* 检查当前映射的所有段以找出哪些已被释放。 */
		LWLockAcquire(DSA_AREA_LOCK(area), LW_EXCLUSIVE);
		check_for_freed_segments_locked(area);
		LWLockRelease(DSA_AREA_LOCK(area));
	}
}

/*
 * check_for_freed_segments() 的工作函数，也在已持有区
 * 域锁的路径中被直接使用。应当在获取锁之后、按
 * 下标查找任何段之前调用本函数，以确保我们解除
 * 任何可能曾拥有与当前段相同下标的过期段的
 * 映射。
 */
static void
check_for_freed_segments_locked(dsa_area *area)
{
	size_t		freed_segment_counter;
	int			i;

	Assert(LWLockHeldByMe(DSA_AREA_LOCK(area)));
	freed_segment_counter = area->control->freed_segment_counter;
	if (unlikely(area->freed_segment_counter != freed_segment_counter))
	{
		for (i = 0; i <= area->high_segment_index; ++i)
		{
			if (area->segment_maps[i].header != NULL &&
				area->segment_maps[i].header->freed)
			{
				dsm_detach(area->segment_maps[i].segment);
				area->segment_maps[i].segment = NULL;
				area->segment_maps[i].header = NULL;
				area->segment_maps[i].mapped_address = NULL;
			}
		}
		area->freed_segment_counter = freed_segment_counter;
	}
}

/*
 * 若段不再位于适当的桶中则重新分桶。
 */
static void
rebin_segment(dsa_area *area, dsa_segment_map *segment_map)
{
	size_t		new_bin;
	dsa_segment_index segment_index;

	new_bin = contiguous_pages_to_segment_bin(fpm_largest(segment_map->fpm));
	if (segment_map->header->bin == new_bin)
		return;

	/* 把它从当前桶移除。 */
	unlink_segment(area, segment_map);

	/* 把它推到新桶的前端。 */
	segment_index = get_segment_index(area, segment_map);
	segment_map->header->prev = DSA_SEGMENT_INDEX_NONE;
	segment_map->header->next = area->control->segment_bins[new_bin];
	segment_map->header->bin = new_bin;
	area->control->segment_bins[new_bin] = segment_index;
	if (segment_map->header->next != DSA_SEGMENT_INDEX_NONE)
	{
		dsa_segment_map *next;

		next = get_segment_by_index(area, segment_map->header->next);
		Assert(next->header->bin == new_bin);
		next->header->prev = segment_index;
	}
}
