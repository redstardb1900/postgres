/*
 * 处理散列值的工具。
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 */

#ifndef HASHFN_H
#define HASHFN_H


/*
 * 分别旋转高 32 位与低 32 位。标准散列函数有时在
 * 略
 * 种子为 0 时与该算法兼容，故不能只做普通旋转。
 * 不过这样可行。
 * 略
 */
#define ROTATE_HIGH_AND_LOW_32BITS(v) \
	((((v) << 1) & UINT64CONST(0xfffffffefffffffe)) | \
	(((v) >> 31) & UINT64CONST(0x100000001)))


extern uint32 hash_bytes(const unsigned char *k, int keylen);
extern uint64 hash_bytes_extended(const unsigned char *k,
								  int keylen, uint64 seed);
extern uint32 hash_bytes_uint32(uint32 k);
extern uint64 hash_bytes_uint32_extended(uint32 k, uint64 seed);

#ifndef FRONTEND
static inline Datum
hash_any(const unsigned char *k, int keylen)
{
	return UInt32GetDatum(hash_bytes(k, keylen));
}

static inline Datum
hash_any_extended(const unsigned char *k, int keylen, uint64 seed)
{
	return UInt64GetDatum(hash_bytes_extended(k, keylen, seed));
}

static inline Datum
hash_uint32(uint32 k)
{
	return UInt32GetDatum(hash_bytes_uint32(k));
}

static inline Datum
hash_uint32_extended(uint32 k, uint64 seed)
{
	return UInt64GetDatum(hash_bytes_uint32_extended(k, seed));
}
#endif

extern uint32 string_hash(const void *key, Size keysize);
extern uint32 tag_hash(const void *key, Size keysize);
extern uint32 uint32_hash(const void *key, Size keysize);

#define oid_hash uint32_hash	/*  最终请删我  */

/*
 * 合并两个 32 位散列值，得到另一散列值，并有
 * 略
 *
 * 类似 boost 的 hash_combine()。
 */
static inline uint32
hash_combine(uint32 a, uint32 b)
{
	a ^= b + 0x9e3779b9 + (a << 6) + (a >> 2);
	return a;
}

/*
 * 用与 hash_combine() 同类手法合并两个 64 位散列
 * 值。测试表明这也能产生良好位混合。
 * 略
 */
static inline uint64
hash_combine64(uint64 a, uint64 b)
{
	/* 略 */
	a ^= b + UINT64CONST(0x49a0f4dd15e5a8e3) + (a << 54) + (a >> 7);
	return a;
}

/*
 * 散列 32 位整数的简单内联 murmur 实现，为性能。
 * 略
 */
static inline uint32
murmurhash32(uint32 data)
{
	uint32		h = data;

	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

#endif							/* HASHFN_H */
