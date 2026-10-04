/*-------------------------------------------------------------------------
 *
 * integerset.c
 *	  Data structure to hold a large set of 64-bit integers efficiently
 *
 * IntegerSet provides an in-memory data structure to hold a set of
 * arbitrary 64-bit integers.  Internally, the values are stored in a
 * B-tree, with a special packed representation at the leaf level using
 * the Simple-8b algorithm, which can pack clusters of nearby values
 * very tightly.
 *
 * Memory consumption depends on the number of values stored, but also
 * on how far the values are from each other.  In the best case, with
 * long runs of consecutive integers, memory consumption can be as low as
 * 0.1 bytes per integer.  In the worst case, if integers are more than
 * 2^32 apart, it uses about 8 bytes per integer.  In typical use, the
 * consumption per integer is somewhere between those extremes, depending
 * on the range of integers stored, and how "clustered" they are.
 *
 *
 * Interface
 * ---------
 *
 *	intset_create			- Create a new, empty set
 *	intset_add_member		- Add an integer to the set
 *	intset_is_member		- Test if an integer is in the set
 *	intset_begin_iterate	- Begin iterating through all integers in set
 *	intset_iterate_next		- Return next set member, if any
 *
 * intset_create() creates the set in the current memory context.  Subsequent
 * operations that add to the data structure will continue to allocate from
 * that same context, even if it's not current anymore.
 *
 * Note that there is no function to free an integer set.  If you need to do
 * that, create a dedicated memory context to hold it, and destroy the memory
 * context instead.
 *
 *
 * Limitations
 * -----------
 *
 * - Values must be added in order.  (Random insertions would require
 *   splitting nodes, which hasn't been implemented.)
 *
 * - Values cannot be added while iteration is in progress.
 *
 * - No support for removing values.
 *
 * None of these limitations are fundamental to the data structure, so they
 * could be lifted if needed, by writing some new code.  But the current
 * users of this facility don't need them.
 *
 *
 * References
 * ----------
 *
 * Simple-8b encoding is based on:
 *
 * Vo Ngoc Anh, Alistair Moffat, Index compression using 64-bit words,
 *   Software - Practice & Experience, v.40 n.2, p.131-147, February 2010
 *   (https://doi.org/10.1002/spe.948)
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/lib/integerset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "lib/integerset.h"
#include "port/pg_bitutils.h"
#include "utils/memutils.h"


/*
 * 单个 Simple-8b 码字中可编码的整数的最大个数。
 * （在其他一切之前定义，以便我们可以用它来确定
 * 数组大小。）
 */
#define SIMPLE8B_MAX_VALUES_PER_CODEWORD 240

/*
 * 内存中 B-树形状的参数。
 *
 * 这些设定每个内部节点和叶子节点的大小。它们不必
 * 相同，因为该树只是内存中的结构。
 * 默认 64 时，每个节点大约 1 kb。
 *
 * 若你改这些，也必须重新计算 MAX_TREE_LEVELS！
 */
#define MAX_INTERNAL_ITEMS	64
#define MAX_LEAF_ITEMS	64

/*
 * 树的最大高度。
 *
 * MAX_TREE_ITEMS 由 B-树的 "扇出" 算出。我们能
 * 在一个集合中存储的理论上限是 2^64，因此
 * MAX_TREE_LEVELS 应设成使：
 *
 *   MAX_LEAF_ITEMS * MAX_INTERNAL_ITEMS ^ (MAX_TREE_LEVELS - 1) >= 2^64。
 *
 * 实践中我们需要的层数少得多，因为远在达到该数目
 * 之前就会耗尽内存，但我们采取保守做法。
 */
#define MAX_TREE_LEVELS		11

/*
 * 内存中 B-树的节点结构。
 *
 * 内部节点持有若干指向叶子节点、或指向更低层内部
 * 节点的下行链接指针。对每个下行链接，对应下层节点
 * 的键值存在一个已排序数组中。所存键值是低键。
 * 换句话说，若下行链接的值是 X，则该孩子上存储的
 * 所有项都 >= X。
 *
 * 每个叶子节点持有若干 "项"，每项打包的整数个数
 * 可变。每项由两个 64 位字组成：
 * 第一个字以明文格式保存该项中存储的第一个整数。
 * 第二个字用 Simple-8b 编码打包 0 到 240 个更多
 * 整数。通过把第一个整数以明文、未打包格式存储，
 * 我们可以用二分搜索快速找到持有（或将会持有）
 * 某特定整数的项。而把其余以打包形式存储，若存在
 * 取值相近的成组整数，我们仍能得到相当好的内存密度。
 *
 * 每个叶子节点还有指向下一叶子节点的指针，以便
 * 迭代时可以方便地从头到尾走遍叶子节点。
 *
 */
typedef struct intset_node intset_node;
typedef struct intset_leaf_node intset_leaf_node;
typedef struct intset_internal_node intset_internal_node;

/* 叶子节点和内部节点的公共结构。 */
struct intset_node
{
	uint16		level;			/* 本节点的树层级 */
	uint16		num_items;		/* 本节点中的项数 */
};

/* 内部节点 */
struct intset_internal_node
{
	/* 公共头部，必须与 intset_node 匹配 */
	uint16		level;			/* 内部节点上 >= 1 */
	uint16		num_items;

	/*
	 * 'values' 是键值数组，'downlinks' 是指向
	 * 下层节点的指针，与这些键值对应。
	 */
	uint64		values[MAX_INTERNAL_ITEMS];
	intset_node *downlinks[MAX_INTERNAL_ITEMS];
};

/* 叶子节点 */
typedef struct
{
	uint64		first;			/* 本项中的第一个整数 */
	uint64		codeword;		/* 相对 'first' 的 simple8b 编码差值 */
} leaf_item;

#define MAX_VALUES_PER_LEAF_ITEM	(1 + SIMPLE8B_MAX_VALUES_PER_CODEWORD)

struct intset_leaf_node
{
	/* 公共头部，必须与 intset_node 匹配 */
	uint16		level;			/* 叶子上为 0 */
	uint16		num_items;

	intset_leaf_node *next;		/* 右兄弟，若有 */

	leaf_item	items[MAX_LEAF_ITEMS];
};

/*
 * 我们在打包并插入 B-树之前，把插入缓存在一个简单
 * 数组中。MAX_BUFFERED_VALUES 设定缓冲区大小。编码器
 * 假定它足够大，使我们总能用缓冲的新项填满一个叶子
 * 项。换句话说，MAX_BUFFERED_VALUES 必须大于
 * MAX_VALUES_PER_LEAF_ITEM。为效率起见，把它设得大得多。
 */
#define MAX_BUFFERED_VALUES			(MAX_VALUES_PER_LEAF_ITEM * 2)

/*
 * IntegerSet 是表示该集合的顶层对象。
 *
 * 整数存在内存中的 B-树结构里，外加一个存放新加
 * 整数的数组。IntegerSet 还跟踪内存用量信息，以及
 * 用 intset_begin_iterate / intset_iterate_next
 * 迭代集合时的当前位置。
 */
struct IntegerSet
{
	/*
	 * 'context' 是持有本整数集合及其全部树节点的
	 * 内存上下文。
	 *
	 * 'mem_used' 跟踪已用内存量。integerset.c 本身
	 * 不用它做任何事，但调用者可用
	 * intset_memory_usage() 询问它。
	 */
	MemoryContext context;
	uint64		mem_used;

	uint64		num_entries;	/* 集合中值的总数 */
	uint64		highest_value;	/* 本集合中存储的最高值 */

	/*
	 * 持有打包值的 B-树。
	 *
	 * 'rightmost_nodes' 持有每层最右节点的指针。
	 * rightmost_parent[0] 是最右叶子，rightmost_parent[1]
	 * 是其父，如此一直到根。添加新值时需要这些。
	 * （当前我们需要新值加在末尾。）
 *
 */
	int			num_levels;		/* 树的高度 */
	intset_node *root;			/* 根节点 */
	intset_node *rightmost_nodes[MAX_TREE_LEVELS];
	intset_leaf_node *leftmost_leaf;	/* 最左叶子节点 */

	/*
	 * 尚未插入树的新项的暂存区。
	 */
	uint64		buffered_values[MAX_BUFFERED_VALUES];
	int			num_buffered_values;

	/*
	 * 迭代器支持。
	 *
	 * 'iter_values' 是准备返回给调用者的整数数组；
	 * 'iter_num_values' 是该数组的长度，
	 * 'iter_valueno' 是下一索引。'iter_node' 和
	 * 'iter_itemno' 指向要从中取下一批值的叶子
	 * 节点，以及该叶子节点内的项。
	 *
	 * 通常 'iter_values' 指向 'iter_values_buf'，其中
	 * 持有从叶子项解码出的项。但在扫完整棵 B-树
	 * 之后，我们也通过把 iter_values 指向
	 * 'buffered_values' 来迭代所有未缓冲的值。
	 */
	bool		iter_active;	/* 迭代是否正在进行？ */

	const uint64 *iter_values;
	int			iter_num_values;	/* 'iter_values' 中的元素个数 */
	int			iter_valueno;	/* 'iter_values' 中的下一索引 */

	intset_leaf_node *iter_node;	/* 当前叶子节点 */
	int			iter_itemno;	/* 'iter_node' 中下一个要解码的项 */

	uint64		iter_values_buf[MAX_VALUES_PER_LEAF_ITEM];
};

/*
 * 内部函数原型。
 */
static void intset_update_upper(IntegerSet *intset, int level,
								intset_node *child, uint64 child_key);
static void intset_flush_buffered_values(IntegerSet *intset);

static int	intset_binsrch_uint64(uint64 item, uint64 *arr, int arr_elems,
								  bool nextkey);
static int	intset_binsrch_leaf(uint64 item, leaf_item *arr, int arr_elems,
								bool nextkey);

static uint64 simple8b_encode(const uint64 *ints, int *num_encoded, uint64 base);
static int	simple8b_decode(uint64 codeword, uint64 *decoded, uint64 base);
static bool simple8b_contains(uint64 codeword, uint64 key, uint64 base);


/*
 * 创建一个新的、初始为空的整数集合。
 *
 * 该整数集合在当前内存上下文中创建。
 * 我们随后的所有分配也在同一上下文中进行，无论
 * 向集合添加新整数时当前是哪个内存上下文。
 */
IntegerSet *
intset_create(void)
{
	IntegerSet *intset;

	intset = (IntegerSet *) palloc(sizeof(IntegerSet));
	intset->context = CurrentMemoryContext;
	intset->mem_used = GetMemoryChunkSpace(intset);

	intset->num_entries = 0;
	intset->highest_value = 0;

	intset->num_levels = 0;
	intset->root = NULL;
	memset(intset->rightmost_nodes, 0, sizeof(intset->rightmost_nodes));
	intset->leftmost_leaf = NULL;

	intset->num_buffered_values = 0;

	intset->iter_active = false;
	intset->iter_node = NULL;
	intset->iter_itemno = 0;
	intset->iter_valueno = 0;
	intset->iter_num_values = 0;
	intset->iter_values = NULL;

	return intset;
}

/*
 * 分配一个新节点。
 */
static intset_internal_node *
intset_new_internal_node(IntegerSet *intset)
{
	intset_internal_node *n;

	n = (intset_internal_node *) MemoryContextAlloc(intset->context,
													sizeof(intset_internal_node));
	intset->mem_used += GetMemoryChunkSpace(n);

	n->level = 0;				/* 调用者必须设置 */
	n->num_items = 0;

	return n;
}

static intset_leaf_node *
intset_new_leaf_node(IntegerSet *intset)
{
	intset_leaf_node *n;

	n = (intset_leaf_node *) MemoryContextAlloc(intset->context,
												sizeof(intset_leaf_node));
	intset->mem_used += GetMemoryChunkSpace(n);

	n->level = 0;
	n->num_items = 0;
	n->next = NULL;

	return n;
}

/*
 * 返回整数集合中的条目数。
 */
uint64
intset_num_entries(IntegerSet *intset)
{
	return intset->num_entries;
}

/*
 * 返回整数集合使用的内存量。
 */
uint64
intset_memory_usage(IntegerSet *intset)
{
	return intset->mem_used;
}

/*
 * 向集合添加一个值。
 *
 * 值必须按顺序添加。
 */
void
intset_add_member(IntegerSet *intset, uint64 x)
{
	if (intset->iter_active)
		elog(ERROR, "cannot add new values to integer set while iteration is in progress");

	if (x <= intset->highest_value && intset->num_entries > 0)
		elog(ERROR, "cannot add value to integer set out of order");

	if (intset->num_buffered_values >= MAX_BUFFERED_VALUES)
	{
		/* 该刷新我们的缓冲区了 */
		intset_flush_buffered_values(intset);
		Assert(intset->num_buffered_values < MAX_BUFFERED_VALUES);
	}

	/* 把它加到新增值的缓冲区 */
	intset->buffered_values[intset->num_buffered_values] = x;
	intset->num_buffered_values++;
	intset->num_entries++;
	intset->highest_value = x;
}

/*
 * 取一批缓冲的值，并把它们打包进 B-树。
 */
static void
intset_flush_buffered_values(IntegerSet *intset)
{
	uint64	   *values = intset->buffered_values;
	uint64		num_values = intset->num_buffered_values;
	int			num_packed = 0;
	intset_leaf_node *leaf;

	leaf = (intset_leaf_node *) intset->rightmost_nodes[0];

	/*
	 * 若树完全为空，创建第一张叶子页，它也是
	 * 根。
	 */
	if (leaf == NULL)
	{
		/*
		 * 这是集合中的第一项。
		 *
		 * 分配根节点。它也是叶子。
		 */
		leaf = intset_new_leaf_node(intset);

		intset->root = (intset_node *) leaf;
		intset->leftmost_leaf = leaf;
		intset->rightmost_nodes[0] = (intset_node *) leaf;
		intset->num_levels = 1;
	}

	/*
	 * 若缓冲区中的值少于 MAX_VALUES_PER_LEAF_ITEM，
	 * 则停止。多数情况下我们无法在单个值中编码那么
	 * 多值，但这样编码器就不必担心输入耗尽。
 *
 */
	while (num_values - num_packed >= MAX_VALUES_PER_LEAF_ITEM)
	{
		leaf_item	item;
		int			num_encoded;

		/*
		 * 构造下一个叶子项，尽可能多地打包缓冲
		 * 的值。
		 */
		item.first = values[num_packed];
		item.codeword = simple8b_encode(&values[num_packed + 1],
										&num_encoded,
										item.first);

		/*
		 * 把该项加到节点上，若旧节点已满则分配
		 * 新节点。
		 */
		if (leaf->num_items >= MAX_LEAF_ITEMS)
		{
			/* 分配新叶子并把它链到树上 */
			intset_leaf_node *old_leaf = leaf;

			leaf = intset_new_leaf_node(intset);
			old_leaf->next = leaf;
			intset->rightmost_nodes[0] = (intset_node *) leaf;
			intset_update_upper(intset, 1, (intset_node *) leaf, item.first);
		}
		leaf->items[leaf->num_items++] = item;

		num_packed += 1 + num_encoded;
	}

	/*
	 * 把任何剩余的缓冲值移到数组开头。
	 */
	if (num_packed < intset->num_buffered_values)
	{
		memmove(&intset->buffered_values[0],
				&intset->buffered_values[num_packed],
				(intset->num_buffered_values - num_packed) * sizeof(uint64));
	}
	intset->num_buffered_values -= num_packed;
}

/*
 * 在创建新节点之后，把下行链接插入父节点。
 *
 * 若父节点也满了则递归。
 */
static void
intset_update_upper(IntegerSet *intset, int level, intset_node *child,
					uint64 child_key)
{
	intset_internal_node *parent;

	Assert(level > 0);

	/*
	 * 如有必要，创建一个新根节点。
	 */
	if (level >= intset->num_levels)
	{
		intset_node *oldroot = intset->root;
		uint64		downlink_key;

		/* MAX_TREE_LEVELS 应绰绰有余，这不应发生 */
		if (intset->num_levels == MAX_TREE_LEVELS)
			elog(ERROR, "could not expand integer set, maximum number of levels reached");
		intset->num_levels++;

		/*
		 * 取旧根页上的第一个值，用作
		 * 下行链接。
		 */
		if (intset->root->level == 0)
			downlink_key = ((intset_leaf_node *) oldroot)->items[0].first;
		else
			downlink_key = ((intset_internal_node *) oldroot)->values[0];

		parent = intset_new_internal_node(intset);
		parent->level = level;
		parent->values[0] = downlink_key;
		parent->downlinks[0] = oldroot;
		parent->num_items = 1;

		intset->root = (intset_node *) parent;
		intset->rightmost_nodes[level] = (intset_node *) parent;
	}

	/*
	 * 把下行链接放到父页上。
	 */
	parent = (intset_internal_node *) intset->rightmost_nodes[level];

	if (parent->num_items < MAX_INTERNAL_ITEMS)
	{
		parent->values[parent->num_items] = child_key;
		parent->downlinks[parent->num_items] = child;
		parent->num_items++;
	}
	else
	{
		/*
		 * 放不下。分配新父，以下行链接作为其上第一
		 * 项，并递归地把新父的下行链接插入祖父。
 *
 */
		parent = intset_new_internal_node(intset);
		parent->level = level;
		parent->values[0] = child_key;
		parent->downlinks[0] = child;
		parent->num_items = 1;

		intset->rightmost_nodes[level] = (intset_node *) parent;

		intset_update_upper(intset, level + 1, (intset_node *) parent, child_key);
	}
}

/*
 * 集合是否包含给定值？
 */
bool
intset_is_member(IntegerSet *intset, uint64 x)
{
	intset_node *node;
	intset_leaf_node *leaf;
	int			level;
	int			itemno;
	leaf_item  *item;

	/*
	 * 该值可能在新增值的缓冲区中。
	 */
	if (intset->num_buffered_values > 0 && x >= intset->buffered_values[0])
	{
		itemno = intset_binsrch_uint64(x,
									   intset->buffered_values,
									   intset->num_buffered_values,
									   false);
		if (itemno >= intset->num_buffered_values)
			return false;
		else
			return (intset->buffered_values[itemno] == x);
	}

	/*
	 * 从根开始，沿 B-树向下走，找到正确的叶子
	 * 节点。
	 */
	if (!intset->root)
		return false;
	node = intset->root;
	for (level = intset->num_levels - 1; level > 0; level--)
	{
		intset_internal_node *n = (intset_internal_node *) node;

		Assert(node->level == level);

		itemno = intset_binsrch_uint64(x, n->values, n->num_items, true);
		if (itemno == 0)
			return false;
		node = n->downlinks[itemno - 1];
	}
	Assert(node->level == 0);
	leaf = (intset_leaf_node *) node;

	/*
	 * 二分搜索以在叶子页上找到正确的项
	 */
	itemno = intset_binsrch_leaf(x, leaf->items, leaf->num_items, true);
	if (itemno == 0)
		return false;
	item = &leaf->items[itemno - 1];

	/* 这是否与该项上的第一个值匹配？ */
	if (item->first == x)
		return true;
	Assert(x > item->first);

	/* 它是否在打包的码字中？ */
	if (simple8b_contains(item->codeword, x, item->first))
		return true;

	return false;
}

/*
 * 开始按序扫描所有值。
 *
 * 迭代进行期间，无法向集合添加新值。
 */
void
intset_begin_iterate(IntegerSet *intset)
{
	/* 注意我们允许中途放弃一次迭代 */
	intset->iter_active = true;
	intset->iter_node = intset->leftmost_leaf;
	intset->iter_itemno = 0;
	intset->iter_valueno = 0;
	intset->iter_num_values = 0;
	intset->iter_values = intset->iter_values_buf;
}

/*
 * 迭代时返回下一个整数。
 *
 * 必须先调用 intset_begin_iterate()。intset_iterate_next()
 * 返回集合中的下一个值。若还有另一个值则返回 true，并把
 * 该值存入 *next。否则返回 false。
 */
bool
intset_iterate_next(IntegerSet *intset, uint64 *next)
{
	Assert(intset->iter_active);
	for (;;)
	{
		/* 若有则返回下一个 iter_values[] 条目 */
		if (intset->iter_valueno < intset->iter_num_values)
		{
			*next = intset->iter_values[intset->iter_valueno++];
			return true;
		}

		/* 若有则解码当前叶子节点中的下一项 */
		if (intset->iter_node &&
			intset->iter_itemno < intset->iter_node->num_items)
		{
			leaf_item  *item;
			int			num_decoded;

			item = &intset->iter_node->items[intset->iter_itemno++];

			intset->iter_values_buf[0] = item->first;
			num_decoded = simple8b_decode(item->codeword,
										  &intset->iter_values_buf[1],
										  item->first);
			intset->iter_num_values = num_decoded + 1;
			intset->iter_valueno = 0;
			continue;
		}

		/* 本叶子上没有更多项，步进到下一节点 */
		if (intset->iter_node)
		{
			intset->iter_node = intset->iter_node->next;
			intset->iter_itemno = 0;
			continue;
		}

		/*
		 * 我们已到达 B-树的末尾。但新增值的缓冲区
		 * 中可能仍有一些整数。
		 */
		if (intset->iter_values == (const uint64 *) intset->iter_values_buf)
		{
			intset->iter_values = intset->buffered_values;
			intset->iter_num_values = intset->num_buffered_values;
			intset->iter_valueno = 0;
			continue;
		}

		break;
	}

	/* 没有更多结果。 */
	intset->iter_active = false;
	*next = 0;					/* 防止未初始化变量警告 */
	return false;
}

/*
 * intset_binsrch_uint64() -- 搜索已排序的 uint64 数组
 *
 * 返回键小于或等于给定键的第一个位置。
 * 返回的位置将是给定键的 "插入" 位置，
 * 也就是新键应被插入到的位置。
 *
 * 'nextkey' 影响相等键上的行为。若为 true，且数组中
 * 有相等键，则返回紧接该相等键之后的位置。若为
 * false，则返回相等键本身的位置。
 */
static int
intset_binsrch_uint64(uint64 item, uint64 *arr, int arr_elems, bool nextkey)
{
	int			low,
				high,
				mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (nextkey)
		{
			if (item >= arr[mid])
				low = mid + 1;
			else
				high = mid;
		}
		else
		{
			if (item > arr[mid])
				low = mid + 1;
			else
				high = mid;
		}
	}

	return low;
}

/* 相同，但针对叶子项数组 */
static int
intset_binsrch_leaf(uint64 item, leaf_item *arr, int arr_elems, bool nextkey)
{
	int			low,
				high,
				mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (nextkey)
		{
			if (item >= arr[mid].first)
				low = mid + 1;
			else
				high = mid;
		}
		else
		{
			if (item > arr[mid].first)
				low = mid + 1;
			else
				high = mid;
		}
	}

	return low;
}

/*
 * Simple-8b 编码。
 *
 * simple-8b 算法把 1 到 240 个整数打包进 64 位字，
 * 称为 "码字"。打进单个码字的整数个数取决于被打包
 * 的整数；小整数比大整数用更少的位编码。例如单个
 * 码字可以存一个 60 位整数，或两个 30 位整数。
 *
 * 由于我们存储的是唯一、已排序的整数集合，我们实际
 * 编码的是连续整数之间的 *差值*。这样，彼此接近的
 * 成组整数会被高效打包，无论其绝对值如何。
 *
 * 在 Simple-8b 中，每个码字由一个 4 位选择器组成，
 * 它指示码字中编码了多少整数，编码后的整数被打包进
 * 其余 60 位。选择器允许用 16 种不同方式使用其余
 * 60 位，称为 "模式"。每种模式打进单个码字的整数
 * 个数列在下面的 simple8b_modes 表中。例如考虑
 * 下面的码字：
 *
 *      20-bit integer       20-bit integer       20-bit integer
 * 1101 00000000000000010010 01111010000100100000 00000000000000010100
 * ^
 * selector
 *
 * 选择器 1101 的十进制是 13。从下面的模式表可见，
 * 它表示该码字编码三个 20 位整数。十进制下这些
 * 整数是 18、500000 和 20。因为我们编码的是差值
 * 而不是绝对值，它们表示的实际值是 18、500018 和
 * 500038。
 *
 * 模式 0 和 1 有点特殊；它们编码一串 240 或 120 个
 * 零（由于我们编码的是整数间差值，这意味着 240 或
 * 120 个连续整数），而不把码字其余位用于任何事。
 *
 * Simple-8b 无法编码大于 60 位的整数。比那更大的
 * 值总是存在叶子项的 'first' 字段中，从不在打包
 * 码字里。若有一串整数彼此相隔超过 2^60，那些项上
 * 的码字将不被使用。为表示这一点，我们使用魔术
 * EMPTY_CODEWORD 码字值。
 *
 *
 *
 */
static const struct simple8b_mode
{
	uint8		bits_per_int;
	uint8		num_ints;
}			simple8b_modes[17] =

{
	{0, 240},					/* 模式  0: 240 个零 */
	{0, 120},					/* 模式  1: 120 个零 */
	{1, 60},					/* 模式  2: 六十个 1 位整数 */
	{2, 30},					/* 模式  3: 三十个 2 位整数 */
	{3, 20},					/* 模式  4: 二十个 3 位整数 */
	{4, 15},					/* 模式  5: 十五个 4 位整数 */
	{5, 12},					/* 模式  6: 十二个 5 位整数 */
	{6, 10},					/* 模式  7: 十个 6 位整数 */
	{7, 8},						/* 模式  8: 八个 7 位整数（浪费四
								 * 位） */
	{8, 7},						/* 模式  9: 七个 8 位整数（浪费四
								 * 位） */
	{10, 6},					/* 模式 10: 六个 10 位整数 */
	{12, 5},					/* 模式 11: 五个 12 位整数 */
	{15, 4},					/* 模式 12: 四个 15 位整数 */
	{20, 3},					/* 模式 13: 三个 20 位整数 */
	{30, 2},					/* 模式 14: 两个 30 位整数 */
	{60, 1},					/* 模式 15: 一个 60 位整数 */

	{0, 0}						/* 哨兵值 */
};

/*
 * EMPTY_CODEWORD 是一个特殊值，用来表示 "无值"。
 * 若下一个值太大而无法用 Simple-8b 编码则使用它。
 *
 * 该值看起来像模式 0 码字，但我们可以区分它，
 * 因为常规模式 0 码字会在未用位上为全零。
 */
#define EMPTY_CODEWORD		UINT64CONST(0x0FFFFFFFFFFFFFFF)

/*
 * 把若干整数编码进一个 Simple-8b 码字。
 *
 * （我们实际编码的是连续整数之间的差值。
 * "base" 是 ints[0] 之前的值。）
 *
 * 输入数组必须至少含 SIMPLE8B_MAX_VALUES_PER_CODEWORD
 * 个元素，以确保我们能产生一个满的码字。
 *
 * 返回编码后的码字，并把 *num_encoded 设为已编码
 * 的输入整数个数。那可以为零，若第一个差值
 * 太大而无法编码。
 */
static uint64
simple8b_encode(const uint64 *ints, int *num_encoded, uint64 base)
{
	int			selector;
	int			nints;
	int			bits;
	uint64		diff;
	uint64		last_val;
	uint64		codeword;
	int			i;

	Assert(ints[0] > base);

	/*
	 * 为本码字选择要使用的 "模式"。
	 *
	 * 每次迭代，检查下一个值能否用我们正在考虑的
	 * 当前模式表示。若太大，则把模式升到更宽的一
	 * 档并重复。若放得下，则转到下一个整数。重复
	 * 直到按当前模式码字已满。
	 *
	 * 注意我们没有办法表示码字中未用的槽，因此
	 * 我们需要每个码字都是 "满的"。除非第一个差值
	 * 太大而无法编码，否则总可以产生满码字。例如，
	 * 若第一个差值小但第二个太大而无法编码，我们将
	 * 最终使用最后一个 "模式"，其 nints == 1。
 *
 */
	selector = 0;
	nints = simple8b_modes[0].num_ints;
	bits = simple8b_modes[0].bits_per_int;
	diff = ints[0] - base - 1;
	last_val = ints[0];
	i = 0;						/* 我们已接受的差值个数 */
	for (;;)
	{
		if (diff >= (UINT64CONST(1) << bits))
		{
			/* 太大，升到下一模式 */
			selector++;
			nints = simple8b_modes[selector].num_ints;
			bits = simple8b_modes[selector].bits_per_int;
			/* 我们可能已为本模式接受了足够的差值 */
			if (i >= nints)
				break;
		}
		else
		{
			/* 接受此差值；若码字已满则完成 */
			i++;
			if (i >= nints)
				break;
			/* 检查下一个差值 */
			Assert(ints[i] > last_val);
			diff = ints[i] - last_val - 1;
			last_val = ints[i];
		}
	}

	if (nints == 0)
	{
		/*
		 * 第一个差值太大，无法用 Simple-8b 编码。
		 *
		 * 若输入中至少有一个不太大的整数，我们将用
		 * 模式 15（或更紧凑的模式）编码它。因此，
		 * 只有 *第一个* 差值 >= 2^60 时才能到这里。
		 */
		Assert(i == 0);
		*num_encoded = 0;
		return EMPTY_CODEWORD;
	}

	/*
	 * 用选定的模式编码这些整数。注意我们按相反
	 * 顺序把它们移入码字，以便解码器按正确顺序
	 * 取出它们。
	 */
	codeword = 0;
	if (bits > 0)
	{
		for (i = nints - 1; i > 0; i--)
		{
			diff = ints[i] - ints[i - 1] - 1;
			codeword |= diff;
			codeword <<= bits;
		}
		diff = ints[0] - base - 1;
		codeword |= diff;
	}

	/* 把选择器加到码字上并返回 */
	codeword |= (uint64) selector << 60;

	*num_encoded = nints;
	return codeword;
}

/*
 * 把码字解码到整数数组中。
 * 返回解码出的整数个数。
 */
static int
simple8b_decode(uint64 codeword, uint64 *decoded, uint64 base)
{
	int			selector = (codeword >> 60);
	int			nints = simple8b_modes[selector].num_ints;
	int			bits = simple8b_modes[selector].bits_per_int;
	uint64		mask = (UINT64CONST(1) << bits) - 1;
	uint64		curr_value;

	if (codeword == EMPTY_CODEWORD)
		return 0;

	curr_value = base;
	for (int i = 0; i < nints; i++)
	{
		uint64		diff = codeword & mask;

		curr_value += 1 + diff;
		decoded[i] = curr_value;
		codeword >>= bits;
	}
	return nints;
}

/*
 * 这与 simple8b_decode() 非常相似，但不是把所有
 * 值解码到数组，而只是检查给定的 "key" 是否属于
 * 该码字。
 */
static bool
simple8b_contains(uint64 codeword, uint64 key, uint64 base)
{
	int			selector = (codeword >> 60);
	int			nints = simple8b_modes[selector].num_ints;
	int			bits = simple8b_modes[selector].bits_per_int;

	if (codeword == EMPTY_CODEWORD)
		return false;

	if (bits == 0)
	{
		/* 对 0 位情形的特殊处理。 */
		return (key - base) <= nints;
	}
	else
	{
		uint64		mask = (UINT64CONST(1) << bits) - 1;
		uint64		curr_value;

		curr_value = base;
		for (int i = 0; i < nints; i++)
		{
			uint64		diff = codeword & mask;

			curr_value += 1 + diff;

			if (curr_value >= key)
			{
				if (curr_value == key)
					return true;
				else
					return false;
			}

			codeword >>= bits;
		}
	}
	return false;
}
