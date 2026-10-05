/*-------------------------------------------------------------------------
 *
 * bloomfilter.c
 *		节省空间的集合成员检测
 *
 * Bloom 过滤器是一种概率数据结构，用来检测某个
 * 元素是否属于集合。可能出现误报，但不会出现
 * 漏报；成员检测要么返回"可能
 * 在集合中"，要么返回"肯定不在集合中"。这通常非常节省空间，
 * 因而可以成为决定性的优势。
 *
 * 元素可以加入集合，但不能移除。加入的元素
 * 越多，误报概率越大。调用者必须在初始化 Bloom 过滤器时
 * 给出集合总大小的估计。
 * 这用来在内存用量与最终误报
 * 率之间取得平衡。
 *
 * 本实现很适合无序集合之间的数据同步问题，
 * 尤其是在性能需要可预测、并且
 * 可以接受一些误报的场合。它也同样适合缓存
 * 过滤：对一个相对较小且/或基数较低的集合取
 * 指纹，尤其是随后许多成员检测最终都
 * 表明所关心的值并不存在。这应当为调用者省下
 * 许多权威查找，例如对大得多的
 * 磁盘上结构做昂贵探测。
 *
 * Copyright (c) 2018-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/bloomfilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/hashfn.h"
#include "lib/bloomfilter.h"
#include "port/pg_bitutils.h"

#define MAX_HASH_FUNCS		10

struct bloom_filter
{
	/* 使用 K 个哈希函数，由调用者的种子做种 */
	int			k_hash_funcs;
	uint64		seed;
	/* m 是位集大小，以位计。必须是 <= 2^32 的二的幂。  */
	uint64		m;
	unsigned char bitset[FLEXIBLE_ARRAY_MEMBER];
};

static int	my_bloom_power(uint64 target_bitset_bits);
static int	optimal_k(uint64 bitset_bits, int64 total_elems);
static void k_hashes(bloom_filter *filter, uint32 *hashes, unsigned char *elem,
					 size_t len);
static inline uint32 mod_m(uint32 val, uint64 m);

/*
 * 在调用者的内存上下文中创建 Bloom 过滤器。当位集大小
 * 不受内存可用性约束时，我们瞄准 1% 到 2% 之间的误报
 * 率。
 *
 * total_elems 是集合最终大小的估计。它应当大致
 * 正确，但实现能很好地应对偏差也许达到五倍或更
 * 多。为何如此，见 "Bloom Filters in
 * Probabilistic Verification" (Dillinger & Manolios, 2004) 的细节。
 *
 *
 * bloom_work_mem 以 KB 计，与一般 work_mem 约定一致。
 * 这决定底层位集的大小（琐碎的簿记空间不计入）。
 * 位集大小总是二的幂位数，最大可能位集是 512MB
 * (2^32 位)。实现只分配瞄准其标准误报率所需的
 * 内存，用调用者的 total_elems 估计作为输入的简单
 * 公式。即使 bloom_work_mem 高得多，位集也可能小到
 * 1MB。
 *
 * Bloom 过滤器使用调用者提供的值做种。每次调用使用
 * 不同种子值，使同一集合第二次取指纹时不太可能再
 * 出现相同的误报。不关心这一点的调用者传入常量
 * 作为种子，通常是 0。调用者也可以用伪随机种子，
 * 例如来自 pg_prng_uint64()。
 *
 */
bloom_filter *
bloom_create(int64 total_elems, int bloom_work_mem, uint64 seed)
{
	bloom_filter *filter;
	int			bloom_power;
	uint64		bitset_bytes;
	uint64		bitset_bits;

	/*
	 * 瞄准每元素两字节；这足以使误报率低于 1%，
	 * 与位集大小或元素总数无关。另外，若把位集
	 * 大小向下取整到下一个更低的二的幂是显著下降，
	 * 几乎所有情况下误报率仍不会超过
	 * 2%。
	 */
	bitset_bytes = Min(bloom_work_mem * UINT64CONST(1024), total_elems * 2);
	bitset_bytes = Max(1024 * 1024, bitset_bytes);

	/*
	 * 以位计的大小应是 <= 目标的最高二的幂。bitset_bits
	 * 是 uint64，因为 PG_UINT32_MAX 是 2^32 - 1，不是 2^32
	 */
	bloom_power = my_bloom_power(bitset_bytes * BITS_PER_BYTE);
	bitset_bits = UINT64CONST(1) << bloom_power;
	bitset_bytes = bitset_bits / BITS_PER_BYTE;

	/* 分配位集未设置的 Bloom 过滤器 */
	filter = palloc0(offsetof(bloom_filter, bitset) +
					 sizeof(unsigned char) * bitset_bytes);
	filter->k_hash_funcs = optimal_k(bitset_bits, total_elems);
	filter->seed = seed;
	filter->m = bitset_bits;

	return filter;
}

/*
 * 释放 Bloom 过滤器
 */
void
bloom_free(bloom_filter *filter)
{
	pfree(filter);
}

/*
 * 向 Bloom 过滤器添加元素
 */
void
bloom_add_element(bloom_filter *filter, unsigned char *elem, size_t len)
{
	uint32		hashes[MAX_HASH_FUNCS];
	int			i;

	k_hashes(filter, hashes, elem, len);

	/* 把按位地址映射到按字节地址 + 位偏移 */
	for (i = 0; i < filter->k_hash_funcs; i++)
	{
		filter->bitset[hashes[i] >> 3] |= 1 << (hashes[i] & 7);
	}
}

/*
 * 测试 Bloom 过滤器是否肯定不含该元素。
 *
 * 若该元素肯定不在 bloom_add_element() 观察到的元素
 * 集合中，返回 true。否则返回 false，表示该元素
 * 很可能在集合中。
 */
bool
bloom_lacks_element(bloom_filter *filter, unsigned char *elem, size_t len)
{
	uint32		hashes[MAX_HASH_FUNCS];
	int			i;

	k_hashes(filter, hashes, elem, len);

	/* 把按位地址映射到按字节地址 + 位偏移 */
	for (i = 0; i < filter->k_hash_funcs; i++)
	{
		if (!(filter->bitset[hashes[i] >> 3] & (1 << (hashes[i] & 7))))
			return true;
	}

	return false;
}

/*
 * 当前有多大比例的位被设置？
 *
 * 返回比例，表示为过滤器大小的乘数。那一般应
 * 接近 0.5，即使我们有绰绰有余的内存来保证误
 * 报率落在目标 1% 到 2% 区间，因为每元素可用
 * 内存越多，使用的哈希函数就越多。
 *
 * 这是唯一开销低到足以出现在调试跟踪中的仪表。
 * 调试 Bloom 过滤器代码时，直接测试误报率往
 * 往有趣得多。
 */
double
bloom_prop_bits_set(bloom_filter *filter)
{
	int			bitset_bytes = filter->m / BITS_PER_BYTE;
	uint64		bits_set = pg_popcount((char *) filter->bitset, bitset_bytes);

	return bits_set / (double) filter->m;
}

/*
 * 二的幂序列中哪个元素小于或等于
 * target_bitset_bits？
 *
 * 这里返回的值必须作为实际位集大小的基础大体
 * 安全。
 *
 * 位集从不允许超过 2 ^ 32 位 (512MB)。这足以满足
 * 所有当前调用者的需要，并允许我们使用 32 位哈希
 * 函数。这也使保持在 MaxAllocSize 限制之下更容易
 * （调用者需要为出现在柔性数组成员之前的非位集
 * 字段留出空间，因此 1GB 位集会使用刚好超过
 * MaxAllocSize 的分配）。
 */
static int
my_bloom_power(uint64 target_bitset_bits)
{
	int			bloom_power = -1;

	while (target_bitset_bits > 0 && bloom_power < 32)
	{
		bloom_power++;
		target_bitset_bits >>= 1;
	}

	return bloom_power;
}

/*
 * 根据过滤器以位计的大小以及预计元素总数，确定
 * 最优哈希函数个数。最优个数是使误报率最小的
 * 那个。
 */
static int
optimal_k(uint64 bitset_bits, int64 total_elems)
{
	int			k = rint(log(2.0) * bitset_bits / total_elems);

	return Max(1, Min(k, MAX_HASH_FUNCS));
}

/*
 * 为元素生成 k 个哈希值。
 *
 * 调用者传入数组，填入由哈希调用者元素确定的 k
 * 个值。
 *
 * 实际只使用 2 个真正独立的哈希函数来支持最多
 * MAX_HASH_FUNCS 个哈希函数的接口；用增强双重哈希
 * 来做到这一点。我们偏好增强双重哈希而非经典
 * 双重哈希的主要原因是后者在使用二的幂大小位集
 * 时有碰撞问题。完整细节见
 * Dillinger & Manolios。
 */
static void
k_hashes(bloom_filter *filter, uint32 *hashes, unsigned char *elem, size_t len)
{
	uint64		hash;
	uint32		x,
				y;
	uint64		m;
	int			i;

	/* 用 64 位哈希得到两个独立的 32 位哈希 */
	hash = DatumGetUInt64(hash_any_extended(elem, len, filter->seed));
	x = (uint32) hash;
	y = (uint32) (hash >> 32);
	m = filter->m;

	x = mod_m(x, m);
	y = mod_m(y, m);

	/* 累积哈希 */
	hashes[0] = x;
	for (i = 1; i < filter->k_hash_funcs; i++)
	{
		x = mod_m(x + y, m);
		y = mod_m(y + i, m);

		hashes[i] = x;
	}
}

/*
 * 廉价地计算 "val MOD m"。
 *
 * 假定 m（即位集大小）是二的幂。
 *
 * 用二的幂位数作为位集大小，使我们能用按位 AND
 * 运算计算哈希值的模。这也是避免模偏差效应的
 * 简单办法。
 */
static inline uint32
mod_m(uint32 val, uint64 m)
{
	Assert(m <= PG_UINT32_MAX + UINT64CONST(1));
	Assert(((m - 1) & m) == 0);

	return val & (m - 1);
}
