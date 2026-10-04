/*-------------------------------------------------------------------------
 *
 * list.c
 *	  implementation for PostgreSQL generic list package
 *
 * See comments in pg_list.h.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/list.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "port/pg_bitutils.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


/*
 * 先前的 List 实现为每个 cons 单元单独做一次 palloc，
 * 因此有这样的性质：增删列表单元不会移动
 * 列表中其他已有单元的存储。相当一部分
 * 既有代码依赖这一点，会在对列表做这类操作之后
 * 仍然保留 ListCell 指针。本实现不提供这种保证，
 * 因此我们改为提供调试支持，用来
 * 帮助揪出现已不成立的假设。在编译本文件时定义 DEBUG_LIST_MEMORY_USAGE
 * 会使 List 操作在增删单元时强制移动
 * 列表中的所有单元。再配合
 * MEMORY_CONTEXT_CHECKING 和/或 Valgrind，通常就能暴露
 * 有问题的代码。不过这有些昂贵，因为比起默认构建，
 * palloc 周期多得多，数据复制也多得多。
 *
 * 默认情况下，为 Valgrind 构建时启用此项。
 */
#ifdef USE_VALGRIND
#define DEBUG_LIST_MEMORY_USAGE
#endif

/* List 头固定部分的开销，以 ListCells 计 */
#define LIST_HEADER_OVERHEAD  \
	((int) ((offsetof(List, initial_elements) - 1) / sizeof(ListCell) + 1))

/*
 * 简化列表类型断言的宏；
 * NIL 列表视为任意类型的空列表。
 */
#define IsPointerList(l)		((l) == NIL || IsA((l), List))
#define IsIntegerList(l)		((l) == NIL || IsA((l), IntList))
#define IsOidList(l)			((l) == NIL || IsA((l), OidList))
#define IsXidList(l)			((l) == NIL || IsA((l), XidList))

#ifdef USE_ASSERT_CHECKING
/*
 * 检查指定的 List 是否有效（就我们所能判断的范围而言）。
 */
static void
check_list_invariants(const List *list)
{
	if (list == NIL)
		return;

	Assert(list->length > 0);
	Assert(list->length <= list->max_length);
	Assert(list->elements != NULL);

	Assert(list->type == T_List ||
		   list->type == T_IntList ||
		   list->type == T_OidList ||
		   list->type == T_XidList);
}
#else
#define check_list_invariants(l)  ((void) 0)
#endif							/* USE_ASSERT_CHECKING */

/*
 * 返回一个新分配的 List，至少能放下 min_size 个单元。
 *
 * 由于空的非 NIL 列表是无效的，new_list() 把初始长度
 * 设为 min_size，相当于把那么多个单元标为有效；调用方
 * 负责填入它们的数据。
 */
static List *
new_list(NodeTag type, int min_size)
{
	List	   *newlist;
	int			max_size;

	Assert(min_size > 0);

	/*
	 * 我们把所请求的全部单元，也许还有一些额外单元，作为
	 * 与 List 头同一次 palloc 请求的一部分来分配。对
	 * 典型的短固定长度列表这是很大的收益。若我们分配了一个
	 * 中等长度的列表然后它又被扩展，就会有损失；浪费的
	 * initial_elements[] 空间会比把头做小更多。不过，
	 * 像正常代码路径那样把请求向上取整，对小幅扩展
	 * 提供了一些防护。
	 */

#ifndef DEBUG_LIST_MEMORY_USAGE

	/*
	 * 通常我们会给列表多留一些单元，使它不必
	 * repalloc 就能增长。优先选择能让总分配量
	 * 成为 2 的幂的单元数，因为 palloc 反正会向上取整到那么大。
	 * （对非常大的分配不再成立，但很长的列表
	 * 很少见，因此似乎不值得为这类情况写特殊逻辑。）
	 *
	 * 最小分配是 8 个 ListCell 单位，依机器字宽可提供 4 或 5 个
	 * 可用的 ListCells。把
	 * palloc 的开销算进去，这与旧实现中单单元
	 * 列表占用的空间相同，更长的列表则更省空间。
	 *
	 * 我们不必担心整数溢出；没有调用方传入的 min_size
	 * 会超过现有列表大小的两倍，因此 palloc 内部的大小限制
	 * 会保证这里不会溢出。
	 */
	max_size = pg_nextpower2_32(Max(8, min_size + LIST_HEADER_OVERHEAD));
	max_size -= LIST_HEADER_OVERHEAD;
#else

	/*
	 * 为了调试，不留任何额外空间。这会迫使任何新增单元
	 * 都走 enlarge_list()，从而移动已有数据。
	 */
	max_size = min_size;
#endif

	newlist = (List *) palloc(offsetof(List, initial_elements) +
							  max_size * sizeof(ListCell));
	newlist->type = type;
	newlist->length = min_size;
	newlist->max_length = max_size;
	newlist->elements = newlist->initial_elements;

	return newlist;
}

/*
 * 把已有的非 NIL List 扩大到至少能放下 min_size 个单元。
 *
 * 这不会更新 list->length，因为有些调用方会觉得那样
 * 不方便。（不过 list->length 最好就是已有
 * 有效单元的正确个数。）
 */
static void
enlarge_list(List *list, int min_size)
{
	int			new_max_len;

	Assert(min_size > list->max_length);	/* 否则我们不该到这里 */

#ifndef DEBUG_LIST_MEMORY_USAGE

	/*
	 * 同上，我们偏好总量为 2 的幂的分配；但这里
	 * 不必计入列表头的开销。
	 */

	/* 把最小值钳制为 16，一个多少有些随意的、较小的 2 的幂 */
	new_max_len = pg_nextpower2_32(Max(16, min_size));

#else
	/* 同上，不额外分配任何空间 */
	new_max_len = min_size;
#endif

	if (list->elements == list->initial_elements)
	{
		/*
		 * 用单独的 palloc 块替换原来的内联分配。
		 * 确保它与 List 头在同一内存上下文中。（先前的
		 * List 实现并不保证
		 * 所有列表单元都在同一上下文中，但现在
		 * 做出这样的保证看来是合理的。）
		 */
		list->elements = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   new_max_len * sizeof(ListCell));
		memcpy(list->elements, list->initial_elements,
			   list->length * sizeof(ListCell));

		/*
		 * 我们不得移动列表头，因此试图通过 repalloc 回收
		 * initial_elements[] 空间是不安全的。在调试构建中，
		 * 我们仍可以把那块空间清掉，并/或标为不可访问。
		 * （wipe_mem 包含 VALGRIND_MAKE_MEM_NOACCESS。）
		 */
#ifdef CLOBBER_FREED_MEMORY
		wipe_mem(list->initial_elements,
				 list->max_length * sizeof(ListCell));
#else
		VALGRIND_MAKE_MEM_NOACCESS(list->initial_elements,
								   list->max_length * sizeof(ListCell));
#endif
	}
	else
	{
#ifndef DEBUG_LIST_MEMORY_USAGE
		/* 通常让 repalloc 处理扩大 */
		list->elements = (ListCell *) repalloc(list->elements,
											   new_max_len * sizeof(ListCell));
#else
		/*
		 * repalloc() 也许会就地扩大空间，而我们为了
		 * 调试目的并不希望这样，因此强制把数据移到别处。
		 */
		ListCell   *newelements;

		newelements = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   new_max_len * sizeof(ListCell));
		memcpy(newelements, list->elements,
			   list->length * sizeof(ListCell));
		pfree(list->elements);
		list->elements = newelements;
#endif
	}

	list->max_length = new_max_len;
}

/*
 * 用给定值构造短 List 的便捷函数。
 * （通常经由 list_makeN 宏调用。）
 */
List *
list_make1_impl(NodeTag t, ListCell datum1)
{
	List	   *list = new_list(t, 1);

	list->elements[0] = datum1;
	check_list_invariants(list);
	return list;
}

List *
list_make2_impl(NodeTag t, ListCell datum1, ListCell datum2)
{
	List	   *list = new_list(t, 2);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	check_list_invariants(list);
	return list;
}

List *
list_make3_impl(NodeTag t, ListCell datum1, ListCell datum2,
				ListCell datum3)
{
	List	   *list = new_list(t, 3);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	list->elements[2] = datum3;
	check_list_invariants(list);
	return list;
}

List *
list_make4_impl(NodeTag t, ListCell datum1, ListCell datum2,
				ListCell datum3, ListCell datum4)
{
	List	   *list = new_list(t, 4);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	list->elements[2] = datum3;
	list->elements[3] = datum4;
	check_list_invariants(list);
	return list;
}

List *
list_make5_impl(NodeTag t, ListCell datum1, ListCell datum2,
				ListCell datum3, ListCell datum4, ListCell datum5)
{
	List	   *list = new_list(t, 5);

	list->elements[0] = datum1;
	list->elements[1] = datum2;
	list->elements[2] = datum3;
	list->elements[3] = datum4;
	list->elements[4] = datum5;
	check_list_invariants(list);
	return list;
}

/*
 * 在给定（非 NIL）列表中为新的头单元腾出空间。
 *
 * 新头单元中的数据未定义；调用方应当
 * 务必填入
 */
static void
new_head_cell(List *list)
{
	/* 必要时扩大数组 */
	if (list->length >= list->max_length)
		enlarge_list(list, list->length + 1);
	/* 然后把已有数据挪开 */
	memmove(&list->elements[1], &list->elements[0],
			list->length * sizeof(ListCell));
	list->length++;
}

/*
 * 在给定（非 NIL）列表中为新的尾单元腾出空间。
 *
 * 新尾单元中的数据未定义；调用方应当
 * 务必填入
 */
static void
new_tail_cell(List *list)
{
	/* 必要时扩大数组 */
	if (list->length >= list->max_length)
		enlarge_list(list, list->length + 1);
	list->length++;
}

/*
 * 把一个指针追加到列表。返回指向修改后列表的
 * 指针。注意本函数也许会、也许不会破坏性地
 * 修改列表；调用方应当始终使用本函数的返回
 * 值，而不是继续使用作为
 * 第一个参数传入的指针。
 */
List *
lappend(List *list, void *datum)
{
	Assert(IsPointerList(list));

	if (list == NIL)
		list = new_list(T_List, 1);
	else
		new_tail_cell(list);

	llast(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把一个整数追加到指定列表。见 lappend()
 */
List *
lappend_int(List *list, int datum)
{
	Assert(IsIntegerList(list));

	if (list == NIL)
		list = new_list(T_IntList, 1);
	else
		new_tail_cell(list);

	llast_int(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把一个 OID 追加到指定列表。见 lappend()
 */
List *
lappend_oid(List *list, Oid datum)
{
	Assert(IsOidList(list));

	if (list == NIL)
		list = new_list(T_OidList, 1);
	else
		new_tail_cell(list);

	llast_oid(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把一个 TransactionId 追加到指定列表。见 lappend()
 */
List *
lappend_xid(List *list, TransactionId datum)
{
	Assert(IsXidList(list));

	if (list == NIL)
		list = new_list(T_XidList, 1);
	else
		new_tail_cell(list);

	llast_xid(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 为位置 'pos'（从 0 计）腾出一个新单元。
 * 单元中的数据留空，必须由
 * 调用方填入。假定 'list' 不是 NIL，且 'pos' 必须是有效的
 * 列表位置，即 0 <= pos <= 列表长度。
 * 返回新单元的地址。
 */
static ListCell *
insert_new_cell(List *list, int pos)
{
	Assert(pos >= 0 && pos <= list->length);

	/* 必要时扩大数组 */
	if (list->length >= list->max_length)
		enlarge_list(list, list->length + 1);
	/* 然后把已有数据挪开 */
	if (pos < list->length)
		memmove(&list->elements[pos + 1], &list->elements[pos],
				(list->length - pos) * sizeof(ListCell));
	list->length++;

	return &list->elements[pos];
}

/*
 * 把给定 datum 插入列表中从 0 计的位置 'pos'。
 * 'pos' 必须有效，即 0 <= pos <= 列表长度。
 *
 * 注意这所花时间与到列表末尾的距离成正比，
 * 因为必须移动后面的项。
 */
List *
list_insert_nth(List *list, int pos, void *datum)
{
	if (list == NIL)
	{
		Assert(pos == 0);
		return list_make1(datum);
	}
	Assert(IsPointerList(list));
	lfirst(insert_new_cell(list, pos)) = datum;
	check_list_invariants(list);
	return list;
}

List *
list_insert_nth_int(List *list, int pos, int datum)
{
	if (list == NIL)
	{
		Assert(pos == 0);
		return list_make1_int(datum);
	}
	Assert(IsIntegerList(list));
	lfirst_int(insert_new_cell(list, pos)) = datum;
	check_list_invariants(list);
	return list;
}

List *
list_insert_nth_oid(List *list, int pos, Oid datum)
{
	if (list == NIL)
	{
		Assert(pos == 0);
		return list_make1_oid(datum);
	}
	Assert(IsOidList(list));
	lfirst_oid(insert_new_cell(list, pos)) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把新元素加到列表前端。返回指向修改后列表
 * 的指针。注意本函数也许会、也许不会破坏性地
 * 修改列表；调用方应当始终使用本函数的返回
 * 值，而不是继续使用作为
 * 第二个参数传入的指针。
 *
 * 注意这所花时间与列表长度成正比，
 * 因为必须移动已有项。
 *
 * 注意：在 Postgres 8.0 之前，原来的 List 不会被修改，
 * 可以认为它仍保持独立身份。现在
 * 已经不是这样了。
 */
List *
lcons(void *datum, List *list)
{
	Assert(IsPointerList(list));

	if (list == NIL)
		list = new_list(T_List, 1);
	else
		new_head_cell(list);

	linitial(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把一个整数加到列表前端。见 lcons()
 */
List *
lcons_int(int datum, List *list)
{
	Assert(IsIntegerList(list));

	if (list == NIL)
		list = new_list(T_IntList, 1);
	else
		new_head_cell(list);

	linitial_int(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把一个 OID 加到列表前端。见 lcons()
 */
List *
lcons_oid(Oid datum, List *list)
{
	Assert(IsOidList(list));

	if (list == NIL)
		list = new_list(T_OidList, 1);
	else
		new_head_cell(list);

	linitial_oid(list) = datum;
	check_list_invariants(list);
	return list;
}

/*
 * 把 list2 拼接到 list1 末尾，并返回 list1。
 *
 * 这等价于按顺序把 list2 的每个元素 lappend 到 list1。
 * list1 会被破坏性修改，list2 不会。（不过对于
 * 指针列表，list1 和 list2 将指向相同的结构。）
 *
 * 调用方应务必把返回值当作拼接后列表的新指针：
 * 传入的 'list1' 指针也许与
 * 返回的指针相同，也许不同。
 *
 * 注意这至少要花与 list2 长度成正比的时间。
 * 通常我们还得扩大 list1 的存储，
 * 可能再增加与 list1 长度成正比的时间。
 */
List *
list_concat(List *list1, const List *list2)
{
	int			new_len;

	if (list1 == NIL)
		return list_copy(list2);
	if (list2 == NIL)
		return list1;

	Assert(list1->type == list2->type);

	new_len = list1->length + list2->length;
	/* 必要时扩大数组 */
	if (new_len > list1->max_length)
		enlarge_list(list1, new_len);

	/* 即使 list1 == list2，这里使用 memcpy 也应是安全的 */
	memcpy(&list1->elements[list1->length], &list2->elements[0],
		   list2->length * sizeof(ListCell));
	list1->length = new_len;

	check_list_invariants(list1);
	return list1;
}

/*
 * 把 list1 与 list2 的元素拼接成一个新列表。
 *
 * 两个输入列表都不会被修改。（不过若它们是指针列表，
 * 输出列表将指向相同的结构。）
 *
 * 这等价于、但比下面更高效：
 * list_concat(list_copy(list1), list2)。
 * 注意某些 v13 之前的代码也许还会 list_copy list2，但那
 * 现在已无必要。
 */
List *
list_concat_copy(const List *list1, const List *list2)
{
	List	   *result;
	int			new_len;

	if (list1 == NIL)
		return list_copy(list2);
	if (list2 == NIL)
		return list_copy(list1);

	Assert(list1->type == list2->type);

	new_len = list1->length + list2->length;
	result = new_list(list1->type, new_len);
	memcpy(result->elements, list1->elements,
		   list1->length * sizeof(ListCell));
	memcpy(result->elements + list1->length, list2->elements,
		   list2->length * sizeof(ListCell));

	check_list_invariants(result);
	return result;
}

/*
 * 把 'list' 截断为最多包含 'new_size' 个元素。这会
 * 就地修改列表！尽管如此，调用方仍应使用
 * 本函数返回的指针来引用新截断的
 * 列表——它也许与传入的指针
 * 相同，也许不同。
 *
 * 注意 list_truncate() 去掉的任何单元都不会被 pfree。
 */
List *
list_truncate(List *list, int new_size)
{
	if (new_size <= 0)
		return NIL;				/* 截断到长度为零 */

	/* 若请求的效果其实是扩展列表，则什么都不做 */
	if (new_size < list_length(list))
		list->length = new_size;

	/*
	 * 注意：与逐个删除列表单元的函数不同，即使在
	 * DEBUG_LIST_MEMORY_USAGE 模式下，我们也不把列表单元移到新存储。
	 * 这是因为本操作中它们都不会移动，因此就像
	 * 旧的基于 cons 单元的实现一样，本函数不会
	 * 使指向列表单元的任何指针失效。同样也是这个原因，
	 * 不擦除被删单元的内存：旧代码也
	 * 不释放它们。也许以后我们会收紧这一点。
	 */

	return list;
}

/*
 * 若 'datum' 是列表成员则返回 true。相等性
 * 通过 equal() 判定，因此调用方应确保传入的
 * 'datum' 是一个 Node。
 *
 * 这是简单的线性搜索——避免在长列表上使用。
 */
bool
list_member(const List *list, const void *datum)
{
	const ListCell *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (equal(lfirst(cell), datum))
			return true;
	}

	return false;
}

/*
 * 若 'datum' 是列表成员则返回 true。相等性
 * 通过简单的指针比较判定。
 */
bool
list_member_ptr(const List *list, const void *datum)
{
	const ListCell *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst(cell) == datum)
			return true;
	}

	return false;
}

/*
 * 若整数 'datum' 是列表成员则返回 true。
 */
bool
list_member_int(const List *list, int datum)
{
	const ListCell *cell;

	Assert(IsIntegerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_int(cell) == datum)
			return true;
	}

	return false;
}

/*
 * 若 OID 'datum' 是列表成员则返回 true。
 */
bool
list_member_oid(const List *list, Oid datum)
{
	const ListCell *cell;

	Assert(IsOidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_oid(cell) == datum)
			return true;
	}

	return false;
}

/*
 * 若 TransactionId 'datum' 是列表成员则返回 true。
 */
bool
list_member_xid(const List *list, TransactionId datum)
{
	const ListCell *cell;

	Assert(IsXidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_xid(cell) == datum)
			return true;
	}

	return false;
}

/*
 * 删除列表中第 n 个单元（从 0 计）。
 *
 * 若这是最后一个成员，则 pfree 该 List。
 *
 * 注意这所花时间与到列表末尾的距离成正比，
 * 因为必须移动后面的项。
 */
List *
list_delete_nth_cell(List *list, int n)
{
	check_list_invariants(list);

	Assert(n >= 0 && n < list->length);

	/*
	 * 若即将删掉列表的最后一个节点，则改为释放整个
	 * 列表并返回 NIL，这是
	 * 零长度列表的唯一合法表示。
	 */
	if (list->length == 1)
	{
		list_free(list);
		return NIL;
	}

	/*
	 * 否则，通常只是把去掉的元素折叠掉。但为了
	 * 调试，把整个列表内容移到别处。
	 *
	 * （注意我们必须把内容保持在同一内存上下文中。）
	 */
#ifndef DEBUG_LIST_MEMORY_USAGE
	memmove(&list->elements[n], &list->elements[n + 1],
			(list->length - 1 - n) * sizeof(ListCell));
	list->length--;
#else
	{
		ListCell   *newelems;
		int			newmaxlen = list->length - 1;

		newelems = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   newmaxlen * sizeof(ListCell));
		memcpy(newelems, list->elements, n * sizeof(ListCell));
		memcpy(&newelems[n], &list->elements[n + 1],
			   (list->length - 1 - n) * sizeof(ListCell));
		if (list->elements != list->initial_elements)
			pfree(list->elements);
		else
		{
			/*
			 * 与 enlarge_list() 一样，清掉 initial_elements[] 空间并/或
			 * 把它标为不可访问。
			 */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(list->initial_elements,
					 list->max_length * sizeof(ListCell));
#else
			VALGRIND_MAKE_MEM_NOACCESS(list->initial_elements,
									   list->max_length * sizeof(ListCell));
#endif
		}
		list->elements = newelems;
		list->max_length = newmaxlen;
		list->length--;
		check_list_invariants(list);
	}
#endif

	return list;
}

/*
 * 从 'list' 中删除 'cell'。
 *
 * 若这是最后一个成员，则 pfree 该 List。不过我们不会
 * 触碰该单元也许指向的任何数据。
 *
 * 注意这所花时间与到列表末尾的距离成正比，
 * 因为必须移动后面的项。
 */
List *
list_delete_cell(List *list, ListCell *cell)
{
	return list_delete_nth_cell(list, cell - list->elements);
}

/*
 * 删除列表中第一个与 datum 匹配的单元（若有）。
 * 相等性通过 equal() 判定。
 *
 * 这是简单的线性搜索——避免在长列表上使用。
 */
List *
list_delete(List *list, void *datum)
{
	ListCell   *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (equal(lfirst(cell), datum))
			return list_delete_cell(list, cell);
	}

	/* 未找到匹配：原样返回列表 */
	return list;
}

/* 同上，但使用简单的指针相等 */
List *
list_delete_ptr(List *list, void *datum)
{
	ListCell   *cell;

	Assert(IsPointerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst(cell) == datum)
			return list_delete_cell(list, cell);
	}

	/* 未找到匹配：原样返回列表 */
	return list;
}

/* 同上，但针对整数 */
List *
list_delete_int(List *list, int datum)
{
	ListCell   *cell;

	Assert(IsIntegerList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_int(cell) == datum)
			return list_delete_cell(list, cell);
	}

	/* 未找到匹配：原样返回列表 */
	return list;
}

/* 同上，但针对 OIDs */
List *
list_delete_oid(List *list, Oid datum)
{
	ListCell   *cell;

	Assert(IsOidList(list));
	check_list_invariants(list);

	foreach(cell, list)
	{
		if (lfirst_oid(cell) == datum)
			return list_delete_cell(list, cell);
	}

	/* 未找到匹配：原样返回列表 */
	return list;
}

/*
 * 删除列表的第一个元素。
 *
 * 这可用于替换 Lisp 风格的代码 "list = lnext(list);"，当
 * 意图是修改列表而不是仅仅遍历它时。
 * 注意列表会被修改，而 Lisp 风格的写法会让
 * 原来的列表头保持原样，以防另有指针指向它。
 *
 * 注意这所花时间与列表长度成正比，
 * 因为必须移动剩余项。可以考虑把列表
 * 反序，以便改用 list_delete_last()。不过，
 * 若那导致你用 lcons() 替换 lappend()，你并没有
 * 把事情变得更好。（简言之，可以用
 * List 做出高效的栈，但做不出高效的 FIFO 队列。）
 */
List *
list_delete_first(List *list)
{
	check_list_invariants(list);

	if (list == NIL)
		return NIL;				/* 报错是否更好？ */

	return list_delete_nth_cell(list, 0);
}

/*
 * 删除列表的最后一个元素。
 */
List *
list_delete_last(List *list)
{
	check_list_invariants(list);

	if (list == NIL)
		return NIL;				/* 报错是否更好？ */

	/* list_truncate 在列表变空时不会释放列表，但这里应当 */
	if (list_length(list) <= 1)
	{
		list_free(list);
		return NIL;
	}

	return list_truncate(list, list_length(list) - 1);
}

/*
 * 删除列表的前 N 个单元。
 *
 * 若该请求导致所有单元都被删除，则 pfree 该 List。
 *
 * 注意这所花时间与到列表末尾的距离成正比，
 * 因为必须移动后面的项。
 */
List *
list_delete_first_n(List *list, int n)
{
	check_list_invariants(list);

	/* 空操作请求？ */
	if (n <= 0)
		return list;

	/* 删除整个列表？ */
	if (n >= list_length(list))
	{
		list_free(list);
		return NIL;
	}

	/*
	 * 否则，通常只是把去掉的元素折叠掉。但为了
	 * 调试，把整个列表内容移到别处。
	 *
	 * （注意我们必须把内容保持在同一内存上下文中。）
	 */
#ifndef DEBUG_LIST_MEMORY_USAGE
	memmove(&list->elements[0], &list->elements[n],
			(list->length - n) * sizeof(ListCell));
	list->length -= n;
#else
	{
		ListCell   *newelems;
		int			newmaxlen = list->length - n;

		newelems = (ListCell *)
			MemoryContextAlloc(GetMemoryChunkContext(list),
							   newmaxlen * sizeof(ListCell));
		memcpy(newelems, &list->elements[n], newmaxlen * sizeof(ListCell));
		if (list->elements != list->initial_elements)
			pfree(list->elements);
		else
		{
			/*
			 * 与 enlarge_list() 一样，清掉 initial_elements[] 空间并/或
			 * 把它标为不可访问。
			 */
#ifdef CLOBBER_FREED_MEMORY
			wipe_mem(list->initial_elements,
					 list->max_length * sizeof(ListCell));
#else
			VALGRIND_MAKE_MEM_NOACCESS(list->initial_elements,
									   list->max_length * sizeof(ListCell));
#endif
		}
		list->elements = newelems;
		list->max_length = newmaxlen;
		list->length = newmaxlen;
		check_list_invariants(list);
	}
#endif

	return list;
}

/*
 * 生成两个列表的并集。做法是用 list_copy() 复制
 * list1，然后把 list2 中
 * 尚不在 list1 里的成员全部加进去。
 *
 * 某元素是否已是列表成员，
 * 通过 equal() 判定。
 *
 * 返回的列表是新分配的，不过单元的内容
 * 相同（即任何被指向的对象都不会被复制）。
 *
 * 注意：本函数不会去掉 list1 中已有的
 * 任何重复项（因此只有在已知 list1 一开始就唯一时，
 * 它做的才是 "并集"）。另外，若你正要写 "x = list_union(x, y)"
 * 你多半想改用 list_concat_unique()，以免浪费
 * 旧 x 列表的存储。
 *
 * 注意这所花时间与两个列表长度的乘积成正比，
 * 因此对长列表要慎用。（我们也许可以
 * 改进这一点，但如果这会成为性能瓶颈，你其实应该换用
 * 别的数据结构。）
 */
List *
list_union(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member(result, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * list_union() 的这一变体通过简单的
 * 指针比较判定重复项。
 */
List *
list_union_ptr(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_ptr(result, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * list_union() 的这一变体用于整数列表。
 */
List *
list_union_int(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_int(result, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * list_union() 的这一变体用于 OIDs 列表。
 */
List *
list_union_oid(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	result = list_copy(list1);
	foreach(cell, list2)
	{
		if (!list_member_oid(result, lfirst_oid(cell)))
			result = lappend_oid(result, lfirst_oid(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * 返回一个列表，包含既在 list1 又在
 * list2 中的所有单元。返回的列表经 palloc() 新分配，但
 * 单元本身指向与
 * 输入列表的单元相同的对象。
 *
 * list1 中的重复项不会被去掉，因此只有事先
 * 已知 list1 唯一时，这才是真正的 "交集"。
 *
 * 本变体用于指针列表，并通过 equal()
 * 判定列表成员资格。注意结果中指向的是
 * list1 的成员。
 *
 * 注意这所花时间与两个列表长度的乘积成正比，
 * 因此对长列表要慎用。（我们也许可以
 * 改进这一点，但如果这会成为性能瓶颈，你其实应该换用
 * 别的数据结构。）
 */
List *
list_intersection(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	if (list1 == NIL || list2 == NIL)
		return NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	result = NIL;
	foreach(cell, list1)
	{
		if (list_member(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * 与 list_intersection 相同，但用于整数列表。
 */
List *
list_intersection_int(const List *list1, const List *list2)
{
	List	   *result;
	const ListCell *cell;

	if (list1 == NIL || list2 == NIL)
		return NIL;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	result = NIL;
	foreach(cell, list1)
	{
		if (list_member_int(list2, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * 返回一个列表，包含在 list1 中但不在
 * list2 中的所有单元。返回的列表经 palloc() 新分配，但
 * 单元本身指向与
 * 输入列表的单元相同的对象。
 *
 * 本变体用于指针列表，并通过 equal()
 * 判定列表成员资格
 *
 * 注意这所花时间与两个列表长度的乘积成正比，
 * 因此对长列表要慎用。（我们也许可以
 * 改进这一点，但如果这会成为性能瓶颈，你其实应该换用
 * 别的数据结构。）
 */
List *
list_difference(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * list_difference() 的这一变体通过
 * 简单的指针相等判定列表成员资格。
 */
List *
list_difference_ptr(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_ptr(list2, lfirst(cell)))
			result = lappend(result, lfirst(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * list_difference() 的这一变体用于整数列表。
 */
List *
list_difference_int(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_int(list2, lfirst_int(cell)))
			result = lappend_int(result, lfirst_int(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * list_difference() 的这一变体用于 OIDs 列表。
 */
List *
list_difference_oid(const List *list1, const List *list2)
{
	const ListCell *cell;
	List	   *result = NIL;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	if (list2 == NIL)
		return list_copy(list1);

	foreach(cell, list1)
	{
		if (!list_member_oid(list2, lfirst_oid(cell)))
			result = lappend_oid(result, lfirst_oid(cell));
	}

	check_list_invariants(result);
	return result;
}

/*
 * 把 datum 追加到列表，但仅当它尚不在列表中时。
 *
 * 某元素是否已是列表成员，
 * 通过 equal() 判定。
 *
 * 这是简单的线性搜索——避免在长列表上使用。
 */
List *
list_append_unique(List *list, void *datum)
{
	if (list_member(list, datum))
		return list;
	else
		return lappend(list, datum);
}

/*
 * list_append_unique() 的这一变体通过
 * 简单的指针相等判定列表成员资格。
 */
List *
list_append_unique_ptr(List *list, void *datum)
{
	if (list_member_ptr(list, datum))
		return list;
	else
		return lappend(list, datum);
}

/*
 * list_append_unique() 的这一变体用于整数列表。
 */
List *
list_append_unique_int(List *list, int datum)
{
	if (list_member_int(list, datum))
		return list;
	else
		return lappend_int(list, datum);
}

/*
 * list_append_unique() 的这一变体用于 OIDs 列表。
 */
List *
list_append_unique_oid(List *list, Oid datum)
{
	if (list_member_oid(list, datum))
		return list;
	else
		return lappend_oid(list, datum);
}

/*
 * 把 list2 中尚不在 list1 里的每个成员追加到 list1。
 *
 * 某元素是否已是列表成员，
 * 通过 equal() 判定。
 *
 * 这与 list_union() 的功能几乎相同，但 list1 是
 * 就地修改而不是被复制。不过，本函数的调用方
 * 也许对顺序有严格预期，即那些
 * 不是重复项的 list2 元素的相对顺序会被保留。
 *
 * 注意这所花时间与两个列表长度的乘积成正比，
 * 因此对长列表要慎用。（我们也许可以
 * 改进这一点，但如果这会成为性能瓶颈，你其实应该换用
 * 别的数据结构。）
 */
List *
list_concat_unique(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	foreach(cell, list2)
	{
		if (!list_member(list1, lfirst(cell)))
			list1 = lappend(list1, lfirst(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * list_concat_unique() 的这一变体通过
 * 简单的指针相等判定列表成员资格。
 */
List *
list_concat_unique_ptr(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsPointerList(list1));
	Assert(IsPointerList(list2));

	foreach(cell, list2)
	{
		if (!list_member_ptr(list1, lfirst(cell)))
			list1 = lappend(list1, lfirst(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * list_concat_unique() 的这一变体用于整数列表。
 */
List *
list_concat_unique_int(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsIntegerList(list1));
	Assert(IsIntegerList(list2));

	foreach(cell, list2)
	{
		if (!list_member_int(list1, lfirst_int(cell)))
			list1 = lappend_int(list1, lfirst_int(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * list_concat_unique() 的这一变体用于 OIDs 列表。
 */
List *
list_concat_unique_oid(List *list1, const List *list2)
{
	ListCell   *cell;

	Assert(IsOidList(list1));
	Assert(IsOidList(list2));

	foreach(cell, list2)
	{
		if (!list_member_oid(list1, lfirst_oid(cell)))
			list1 = lappend_oid(list1, lfirst_oid(cell));
	}

	check_list_invariants(list1);
	return list1;
}

/*
 * 去掉 OIDs 列表中相邻的重复项。
 *
 * 调用方有责任先把列表排好序，使重复项靠在一起，
 * 例如经由 list_sort(list, list_oid_cmp)。
 *
 * 注意这所花时间与列表长度成正比。
 */
void
list_deduplicate_oid(List *list)
{
	int			len;

	Assert(IsOidList(list));
	len = list_length(list);
	if (len > 1)
	{
		ListCell   *elements = list->elements;
		int			i = 0;

		for (int j = 1; j < len; j++)
		{
			if (elements[i].oid_value != elements[j].oid_value)
				elements[++i].oid_value = elements[j].oid_value;
		}
		list->length = i + 1;
	}
	check_list_invariants(list);
}

/*
 * 释放列表中的全部存储，以及可选地释放被指向的元素
 */
static void
list_free_private(List *list, bool deep)
{
	if (list == NIL)
		return;					/* 无事可做 */

	check_list_invariants(list);

	if (deep)
	{
		for (int i = 0; i < list->length; i++)
			pfree(lfirst(&list->elements[i]));
	}
	if (list->elements != list->initial_elements)
		pfree(list->elements);
	pfree(list);
}

/*
 * 释放列表的所有单元以及列表本身。列表
 * 单元所指向的任何对象都不会被
 * 释放。
 *
 * 返回时，本函数的参数已被释放，因此
 * 调用方为安全起见最好把它设为 NIL。
 */
void
list_free(List *list)
{
	list_free_private(list, false);
}

/*
 * 释放列表的所有单元、列表本身，以及
 * 列表单元所指向的全部
 * 对象（每个元素都必须包含指向一块 palloc() 区域的指针！）
 *
 * 返回时，本函数的参数已被释放，因此
 * 调用方为安全起见最好把它设为 NIL。
 */
void
list_free_deep(List *list)
{
	/*
	 * "深" 释放只对指针列表有意义。
	 */
	Assert(IsPointerList(list));
	list_free_private(list, true);
}

/*
 * 返回指定列表的浅拷贝。
 */
List *
list_copy(const List *oldlist)
{
	List	   *newlist;

	if (oldlist == NIL)
		return NIL;

	newlist = new_list(oldlist->type, oldlist->length);
	memcpy(newlist->elements, oldlist->elements,
		   newlist->length * sizeof(ListCell));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * 返回指定列表的浅拷贝，只含前 'len'
 * 个元素。若 oldlist 短于 'len'，则复制整个列表。
 */
List *
list_copy_head(const List *oldlist, int len)
{
	List	   *newlist;

	if (oldlist == NIL || len <= 0)
		return NIL;

	len = Min(oldlist->length, len);

	newlist = new_list(oldlist->type, len);
	memcpy(newlist->elements, oldlist->elements, len * sizeof(ListCell));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * 返回指定列表去掉前 N 个元素后的浅拷贝。
 */
List *
list_copy_tail(const List *oldlist, int nskip)
{
	List	   *newlist;

	if (nskip < 0)
		nskip = 0;				/* 改用 elog 是否更好？ */

	if (oldlist == NIL || nskip >= oldlist->length)
		return NIL;

	newlist = new_list(oldlist->type, oldlist->length - nskip);
	memcpy(newlist->elements, &oldlist->elements[nskip],
		   newlist->length * sizeof(ListCell));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * 返回指定列表的深拷贝。
 *
 * 列表元素经由 copyObject() 复制，因此本函数所说的
 * "深" 拷贝比 list_free_deep()
 * 用同一个词所指的要深得多。
 */
List *
list_copy_deep(const List *oldlist)
{
	List	   *newlist;

	if (oldlist == NIL)
		return NIL;

	/* 这只对指针 List 有意义 */
	Assert(IsA(oldlist, List));

	newlist = new_list(oldlist->type, oldlist->length);
	for (int i = 0; i < newlist->length; i++)
		lfirst(&newlist->elements[i]) =
			copyObjectImpl(lfirst(&oldlist->elements[i]));

	check_list_invariants(newlist);
	return newlist;
}

/*
 * 按指定的比较函数对列表排序。
 *
 * 列表就地排序。
 *
 * 比较函数声明为接收类型为
 * const ListCell * 的参数；这样它就可以使用 lfirst() 及其变体，
 * 而不必转换参数。除此之外，它的行为与
 * 标准 qsort() 的比较函数相同。
 *
 * 与 qsort() 一样，对相等的键
 * 不保证排序稳定性。
 *
 * 本实现基于 qsort()，因此运行时间同样是 O(N log N)。
 */
void
list_sort(List *list, list_sort_comparator cmp)
{
	typedef int (*qsort_comparator) (const void *a, const void *b);
	int			len;

	check_list_invariants(list);

	/* 若元素少于两个，则无事可做 */
	len = list_length(list);
	if (len > 1)
		qsort(list->elements, len, sizeof(ListCell), (qsort_comparator) cmp);
}

/*
 * 供 list_sort 把列表按整数升序排序的比较器。
 */
int
list_int_cmp(const ListCell *p1, const ListCell *p2)
{
	int			v1 = lfirst_int(p1);
	int			v2 = lfirst_int(p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}

/*
 * 供 list_sort 把列表按 OID 升序排序的比较器。
 */
int
list_oid_cmp(const ListCell *p1, const ListCell *p2)
{
	Oid			v1 = lfirst_oid(p1);
	Oid			v2 = lfirst_oid(p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}
