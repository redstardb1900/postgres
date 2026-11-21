/*-------------------------------------------------------------------------
 *
 * checksum_impl.h
 *	  数据页的校验和实现。
 *
 * 本文件供可能想检查 Postgres 页校验和的外部
 * 程序使用。它们可 #include 本文件以取得 
 * storage/checksum.h 所引用的代码。（注意：外部
 * 编译时或须把 Assert() 重定义为空。）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum_impl.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 页校验和所用算法选来追求极快计算。
 * 当数据库工作集能装进操作系统文件缓存却装不进
 * 共享缓冲区时，可读页极快，校验和算法本身
 * 可能成为最大瓶颈。
 *
 * 校验和算法本身基于 FNV-1a 散列（FNV 是 
 * Fowler/Noll/Vo 的缩写）。普通 FNV-1a 原语
 * 按下列公式每次折入 1 字节：
 *
 *	   hash = (hash ^ value) * FNV_PRIME
 *
 * FNV-1a 算法说明见 http://www.isthe.com/chongo/tech/comp/fnv/
 *
 * PostgreSQL 不直接用 FNV-1a 散列，因其高位混合差——
 * 输入高位只影响输出高位。为解决此点，我们在乘法前
 * 把值右移 17 位后再异或进去。选 17 是因为它与 
 * FNV_PRIME 中置位位置无公因数，且经验上能为最终
 * 迭代的高位提供最快混合，使其迅速雪崩到较低
 * 位。出于性能，我们选择每次合并 
 * 4 字节。实际作为基础的
 * 散列公式是：
 *
 *	   hash = (hash ^ value) * FNV_PRIME ^ ((hash ^ value) >> 17)
 *
 * 此计算的主要瓶颈是乘法延迟。为隐藏延迟并
 * 利用 SIMD 并行，会并行计算多个散列值。页被
 * 当作由 32 位值组成的 32 列二维数组。各列分别
 * 聚合成部分校验和。每个部分校验和使用不同
 * 初值（FNV 术语中的 offset basis）。实际所用
 * 初值是随机选取的，因数值本身不如它们彼此
 * 不同且不匹配真实数据来得重要。在初始化
 * 部分校验和后，列中每个值都按上述公式聚合。
 * 最后再用值 0 多执行两轮该公式，
 * 以混合最后加入之值的
 * 各位。
 *
 * 随后用异或把各部分校验和折叠成单个 
 * 32 位校验和。调用方可安全地用模 2^16-1 
 * 把值缩到 16 位。这会对较低值产生极轻微偏置，
 * 但对校验和的性能并无显著
 * 影响。
 *
 * 算法选择基于 SIMD 指令集中有哪些指令可用。
 * 这意味着又快又好的算法须以乘法为主要混合算子。
 * 最简单的基于乘法的校验和原语即 FNV 所用者。
 * 所选素数利于数值良好分散。尚无已知的简单模式会
 * 导致冲突。对该原语在 64 位键上做 5 位差分测试，
 * 在 100000 个随机键中未见有 3 个及以上值冲突的
 * 差分。雪崩测试显示仅最后一词的高位有偏置。
 * 对 1-4 个不相关位错误、散落的 0 与 0xFF 字节、
 * 自随机位置起用 0 字节覆盖至页末、以及用 0x00、
 * 0xFF 与随机数据覆盖页内随机段的测试，均显示
 * 在误差范围内达到最优的 
 * 2e-16 假阳性率。
 *
 * 算法向量化需要 32bit×32bit→32bit 整数乘法指令。
 * 截至 2013 年，对应指令在 x86 SSE4.1 扩展
 * （pmulld）与 ARM NEON（vmul.i32）上可用。
 * 向量化须由编译器完成。对较新的 GCC，标志 
 * -msse4.1 与 -funroll-loops 与 -ftree-vectorize 即
 * 足以实现向量化。
 *
 * 最优并行度取决于 CPU 特有的指令延迟、SIMD
 * 指令宽度、吞吐量以及可保存中间状态的寄存器
 * 数量。一般而言，并行度越高越好，直到状态装不进
 * 寄存器而需要额外 load-store 指令来换入/换出。
 * 所选数目是算法的固定部分，因改变并行度会改变
 * 校验和结果。
 * result.
 *
 * 并行度取 32 是因为这是能装入架构可见的 
 * x86 SSE 寄存器、并仍留出一些空闲寄存器给中间值的
 * 最大状态。对将来带 256 位向量寄存器的处理器，
 * 这会留下一些性能余量未用。当无法向量化时，
 * 或许宜重构计算，每次只算一部分列并做多趟，
 * 以避免寄存器溢出。此优化机会未使用。当前编码
 * 还假定编译器能展开内层循环以去掉循环开销，
 * 并尽量减少寄存器溢出。对较简陋的
 * 编译器，或许宜
 * 手动展开内层
 * 循环。
 */

#include "storage/bufpage.h"

/* 并行计算的校验和个数 */
#define N_SUMS 32
/* FNV-1a 散列的素数乘数 */
#define FNV_PRIME 16777619

/* 用联合体使本代码在严格别名规则下合法 */
typedef union
{
	PageHeaderData phdr;
	uint32		data[BLCKSZ / (sizeof(uint32) * N_SUMS)][N_SUMS];
} PGChecksummablePage;

/*
 * 用于把各并行 FNV 散列初始化到不同初始
 * 状态的基偏移。
 */
static const uint32 checksumBaseOffsets[N_SUMS] = {
	0x5B1F36E9, 0xB8525960, 0x02AB50AA, 0x1DE66D2A,
	0x79FF467A, 0x9BB9F8A3, 0x217E7CD2, 0x83E13D2C,
	0xF8D4474F, 0xE39EB970, 0x42C6AE16, 0x993216FA,
	0x7B093B5D, 0x98DAFF3C, 0xF718902A, 0x0B1C9CDB,
	0xE58F764B, 0x187636BC, 0x5D7B3BB1, 0xE73DE7DE,
	0x92BEC979, 0xCCA6C0B2, 0x304A0979, 0x85AA43D4,
	0x783125BB, 0x6CA8EAA2, 0xE407EAC6, 0x4B5CFC3E,
	0x9FBF8C76, 0x15CA20BE, 0xF2CA9FD3, 0x959BD756
};

/*
 * 计算一轮校验和。
 */
#define CHECKSUM_COMP(checksum, value) \
do { \
	uint32 __tmp = (checksum) ^ (value); \
	(checksum) = __tmp * FNV_PRIME ^ (__tmp >> 17); \
} while (0)

/*
 * 块校验和算法。页必须充分对齐
 * （至少 4 字节边界）。
 */
static uint32
pg_checksum_block(const PGChecksummablePage *page)
{
	uint32		sums[N_SUMS];
	uint32		result = 0;
	uint32		i,
				j;

	/* 确保大小与算法兼容 */
	Assert(sizeof(PGChecksummablePage) == BLCKSZ);

	/* 把各部分校验和初始化为对应偏移 */
	memcpy(sums, checksumBaseOffsets, sizeof(checksumBaseOffsets));

	/* 主校验和计算 */
	for (i = 0; i < (uint32) (BLCKSZ / (sizeof(uint32) * N_SUMS)); i++)
		for (j = 0; j < N_SUMS; j++)
			CHECKSUM_COMP(sums[j], page->data[i][j]);

	/* 最后再加两轮零值以进一步混合 */
	for (i = 0; i < 2; i++)
		for (j = 0; j < N_SUMS; j++)
			CHECKSUM_COMP(sums[j], 0);

	/* 用异或折叠各部分校验和 */
	for (i = 0; i < N_SUMS; i++)
		result ^= sums[i];

	return result;
}

/*
 * 计算 Postgres 页的校验和。
 *
 * 页必须充分对齐（至少在 4 字节边界上）。
 * 还须注意页的校验和字段会短暂被置零。
 *
 * 校验和包含块号（以检测页被挪到别处的
 * 情形）、页头（不含校验和本身）以及
 * 页数据。
 */
uint16
pg_checksum_page(char *page, BlockNumber blkno)
{
	PGChecksummablePage *cpage = (PGChecksummablePage *) page;
	uint16		save_checksum;
	uint32		checksum;

	/* 我们只为正确初始化的页计算校验和 */
	Assert(!PageIsNew((Page) page));

	/*
	 * 保存 pd_checksum 并临时置零，以免校验和计算
	 * 受页上旧校验和影响。之后再恢复，因为真正
	 * 更新校验和并非本函数
	 * API 的一部分。
	 */
	save_checksum = cpage->phdr.pd_checksum;
	cpage->phdr.pd_checksum = 0;
	checksum = pg_checksum_block(cpage);
	cpage->phdr.pd_checksum = save_checksum;

	/* 混入块号以检测被转置的页 */
	checksum ^= blkno;

	/*
	 * 用偏移 1 缩成 uint16（以适合 pd_checksum 字段）。
	 * 这样可避免校验和为零，似乎是个好主意。
	 */
	return (uint16) ((checksum % 65535) + 1);
}
