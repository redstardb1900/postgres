/*-------------------------------------------------------------------------
 *
 * memdebug.c
 *	  内存上下文实现中用到的声明，
 *	  不属于内存管理子系统的公开 API。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/mmgr/memdebug.c
 *
 *
 *	关于 CLOBBER_FREED_MEMORY
 *
 *	若定义了此符号，所有被释放的内存均以 0x7F 覆写。
 *	这有助于定位引用了已释放内存的代码位置。
 *
 *	关于 MEMORY_CONTEXT_CHECKING
 *
 *	由于请求大小通常会被取整到 2 的幂，
 *	紧邻申请数据区之后往往还有一些未用空间。
 *	因此，若有人犯了常见错误，写入超出其申请的
 *	范围，问题多半不致被察觉……直到某天再也
 *	没有浪费空间时——也许因为新平台内存对齐
 *	方式不同，或别的什么影响。为捕获这类问题，
 *	MEMORY_CONTEXT_CHECKING 选项会在请求小于
 *	实际区块大小时，在申请空间之外紧邻处写入 0x7E，
 *	并在区块释放时校验该字节未受损。
 *
 *
 *	关于 USE_VALGRIND 与 Valgrind 客户端请求
 *
 *	Valgrind 提供"客户端请求"宏，与宿主 Valgrind（若存
 *	在的话）交换信息。在 !USE_VALGRIND 下，memdebug.h 会将
 *	现用宏做成桩。
 *
 *	在 Valgrind 下运行时，我们希望分配前后都有一段
 *	NOACCESS 内存区域。区块头部看似可充当其前的那段，
 *	但 mcxt.c 需要能够检视标准的区块头部字段，因此
 *	我们在可行时改用 requested_size 字段及其后续填充。
 *	在把区块指针交还调用者之前，requested_size 会被置为
 *	NOACCESS。不过，为了减少客户端请求的通信量，
 *	在空闲链表区块中它保持 DEFINED。
 *
 *	区块取整后的容量通常充当分配后的 NOACCESS 区域。
 *	若请求恰好耗尽整块，则不存在该区域；紧随其后
 *	可能紧跟另一个区块头部。这种情况下，Valgrind 将检测
 *	不到对区块末尾之外的访问。
 *
 *	另见 mcxt.c 中配合的 Valgrind 客户端请求。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memdebug.h"

#ifdef RANDOMIZE_ALLOCATED_MEMORY

/*
 * 用"随机"数据填充一块刚分配的内存。它其实并不怎么
 * 随机，只是重复一个素数长度的序列。主要想要的
 * 是：两次分配相同字节数的 palloc 大概率以彼此
 * 不同的初始数据开局。
 *
 * 该区域可能是 NOACCESS，故先把它置为 UNDEFINED，
 * 以免填充时出错。填充使区域变为 DEFINED，因此之后
 * 要再置为 UNDEFINED。最终置为 UNDEFINED 还是
 * NOACCESS 相当随意。UNDEFINED 对 SlabRealloc()
 * 更合适，其他人随意。
 */
void
randomize_mem(char *ptr, size_t size)
{
	static int	save_ctr = 1;
	size_t		remaining = size;
	int			ctr;

	ctr = save_ctr;
	VALGRIND_MAKE_MEM_UNDEFINED(ptr, size);
	while (remaining-- > 0)
	{
		*ptr++ = ctr;
		if (++ctr > 251)
			ctr = 1;
	}
	VALGRIND_MAKE_MEM_UNDEFINED(ptr - size, size);
	save_ctr = ctr;
}

#endif							/* RANDOMIZE_ALLOCATED_MEMORY */
