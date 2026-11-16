/*-------------------------------------------------------------------------
 *
 * tsgistidx.c
 *	  tsvector_ops 的 GiST 支持函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/tsgistidx.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gist.h"
#include "access/heaptoast.h"
#include "access/reloptions.h"
#include "lib/qunique.h"
#include "port/pg_bitutils.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"
#include "utils/pg_crc.h"


/* tsvector_ops 算子类选项 */
typedef struct
{
	int32		vl_len_;		/* varlena 头部（勿直接改动！） */
	int			siglen;			/* 签名长度 */
} GistTsVectorOptions;

#define SIGLEN_DEFAULT	(31 * 4)
#define SIGLEN_MAX		GISTMaxIndexKeySize
#define GET_SIGLEN()	(PG_HAS_OPCLASS_OPTIONS() ? \
						 ((GistTsVectorOptions *) PG_GET_OPCLASS_OPTIONS())->siglen : \
						 SIGLEN_DEFAULT)

#define SIGLENBIT(siglen) ((siglen) * BITS_PER_BYTE)

typedef char *BITVECP;

#define LOOPBYTE(siglen) \
			for (i = 0; i < siglen; i++)

#define GETBYTE(x,i) ( *( (BITVECP)(x) + (int)( (i) / BITS_PER_BYTE ) ) )
#define GETBITBYTE(x,i) ( ((char)(x)) >> (i) & 0x01 )
#define CLRBIT(x,i)   GETBYTE(x,i) &= ~( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define SETBIT(x,i)   GETBYTE(x,i) |=  ( 0x01 << ( (i) % BITS_PER_BYTE ) )
#define GETBIT(x,i) ( (GETBYTE(x,i) >> ( (i) % BITS_PER_BYTE )) & 0x01 )

#define HASHVAL(val, siglen) (((unsigned int)(val)) % SIGLENBIT(siglen))
#define HASH(sign, val, siglen) SETBIT((sign), HASHVAL(val, siglen))

#define GETENTRY(vec,pos) ((SignTSVector *) DatumGetPointer((vec)->vector[(pos)].key))

/*
 * GiST 索引键类型
 */

typedef struct
{
	int32		vl_len_;		/* varlena 头部（勿直接改动！） */
	int32		flag;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} SignTSVector;

#define ARRKEY		0x01
#define SIGNKEY		0x02
#define ALLISTRUE	0x04

#define ISARRKEY(x) ( ((SignTSVector*)(x))->flag & ARRKEY )
#define ISSIGNKEY(x)	( ((SignTSVector*)(x))->flag & SIGNKEY )
#define ISALLTRUE(x)	( ((SignTSVector*)(x))->flag & ALLISTRUE )

#define GTHDRSIZE	( VARHDRSZ + sizeof(int32) )
#define CALCGTSIZE(flag, len) ( GTHDRSIZE + ( ( (flag) & ARRKEY ) ? ((len)*sizeof(int32)) : (((flag) & ALLISTRUE) ? 0 : (len)) ) )

#define GETSIGN(x)	( (BITVECP)( (char*)(x)+GTHDRSIZE ) )
#define GETSIGLEN(x)( VARSIZE(x) - GTHDRSIZE )
#define GETARR(x)	( (int32*)( (char*)(x)+GTHDRSIZE ) )
#define ARRNELEM(x) ( ( VARSIZE(x) - GTHDRSIZE )/sizeof(int32) )

static int32 sizebitvec(BITVECP sign, int siglen);

Datum
gtsvectorin(PG_FUNCTION_ARGS)
{
	/* 无需支持 gtsvector 的输入 */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type %s", "gtsvector")));

	PG_RETURN_VOID();			/* 让编译器安静 */
}

#define SINGOUTSTR	"%d true bits, %d false bits"
#define ARROUTSTR	"%d unique words"
#define EXTRALEN	( 2*13 )

static int	outbuf_maxlen = 0;

Datum
gtsvectorout(PG_FUNCTION_ARGS)
{
	SignTSVector *key = (SignTSVector *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	char	   *outbuf;

	if (outbuf_maxlen == 0)
		outbuf_maxlen = 2 * EXTRALEN + Max(strlen(SINGOUTSTR), strlen(ARROUTSTR)) + 1;
	outbuf = palloc(outbuf_maxlen);

	if (ISARRKEY(key))
		sprintf(outbuf, ARROUTSTR, (int) ARRNELEM(key));
	else
	{
		int			siglen = GETSIGLEN(key);
		int			cnttrue = (ISALLTRUE(key)) ? SIGLENBIT(siglen) : sizebitvec(GETSIGN(key), siglen);

		sprintf(outbuf, SINGOUTSTR, cnttrue, (int) SIGLENBIT(siglen) - cnttrue);
	}

	PG_FREE_IF_COPY(key, 0);
	PG_RETURN_POINTER(outbuf);
}

static int
compareint(const void *va, const void *vb)
{
	int32		a = *((const int32 *) va);
	int32		b = *((const int32 *) vb);

	if (a == b)
		return 0;
	return (a > b) ? 1 : -1;
}

static void
makesign(BITVECP sign, SignTSVector *a, int siglen)
{
	int32		k,
				len = ARRNELEM(a);
	int32	   *ptr = GETARR(a);

	MemSet(sign, 0, siglen);
	for (k = 0; k < len; k++)
		HASH(sign, ptr[k], siglen);
}

static SignTSVector *
gtsvector_alloc(int flag, int len, BITVECP sign)
{
	int			size = CALCGTSIZE(flag, len);
	SignTSVector *res = palloc(size);

	SET_VARSIZE(res, size);
	res->flag = flag;

	if ((flag & (SIGNKEY | ALLISTRUE)) == SIGNKEY && sign)
		memcpy(GETSIGN(res), sign, len);

	return res;
}


Datum
gtsvector_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int			siglen = GET_SIGLEN();
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* tsvector */
		TSVector	val = DatumGetTSVector(entry->key);
		SignTSVector *res = gtsvector_alloc(ARRKEY, val->size, NULL);
		int32		len;
		int32	   *arr;
		WordEntry  *ptr = ARRPTR(val);
		char	   *words = STRPTR(val);

		arr = GETARR(res);
		len = val->size;
		while (len--)
		{
			pg_crc32	c;

			INIT_LEGACY_CRC32(c);
			COMP_LEGACY_CRC32(c, words + ptr->pos, ptr->len);
			FIN_LEGACY_CRC32(c);

			*arr = *(int32 *) &c;
			arr++;
			ptr++;
		}

		qsort(GETARR(res), val->size, sizeof(int), compareint);
		len = qunique(GETARR(res), val->size, sizeof(int), compareint);
		if (len != val->size)
		{
			/*
			 * 哈希函数发生了碰撞；len 必然小于
			 * val->size
			 */
			len = CALCGTSIZE(ARRKEY, len);
			res = (SignTSVector *) repalloc(res, len);
			SET_VARSIZE(res, len);
		}

		/* 数组过长时构造签名 */
		if (VARSIZE(res) > TOAST_INDEX_TARGET)
		{
			SignTSVector *ressign = gtsvector_alloc(SIGNKEY, siglen, NULL);

			makesign(GETSIGN(ressign), res, siglen);
			res = ressign;
		}

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	else if (ISSIGNKEY(DatumGetPointer(entry->key)) &&
			 !ISALLTRUE(DatumGetPointer(entry->key)))
	{
		int32		i;
		SignTSVector *res;
		BITVECP		sign = GETSIGN(DatumGetPointer(entry->key));

		LOOPBYTE(siglen)
		{
			if ((sign[i] & 0xff) != 0xff)
				PG_RETURN_POINTER(retval);
		}

		res = gtsvector_alloc(SIGNKEY | ALLISTRUE, siglen, sign);
		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(res),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	PG_RETURN_POINTER(retval);
}

Datum
gtsvector_decompress(PG_FUNCTION_ARGS)
{
	/*
	 * 我们需要对存储值做 detoast，因为其他 gtsvector
	 * 支持函数无法处理被 TOAST 的值。
	 */
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	SignTSVector *key = (SignTSVector *) PG_DETOAST_DATUM(entry->key);

	if (key != (SignTSVector *) DatumGetPointer(entry->key))
	{
		GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);

		PG_RETURN_POINTER(retval);
	}

	PG_RETURN_POINTER(entry);
}

typedef struct
{
	int32	   *arrb;
	int32	   *arre;
} CHKVAL;

/*
 * TS_execute 回调：tsquery 操作数与 GIST 叶子页数据匹配
 */
static TSTernaryValue
checkcondition_arr(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	int32	   *StopLow = ((CHKVAL *) checkval)->arrb;
	int32	   *StopHigh = ((CHKVAL *) checkval)->arre;
	int32	   *StopMiddle;

	/* 循环不变量：StopLow<=val<StopHigh */

	/*
	 * 无法靠哈希值找到前缀
	 */
	if (val->prefix)
		return TS_MAYBE;

	while (StopLow < StopHigh)
	{
		StopMiddle = StopLow + (StopHigh - StopLow) / 2;
		if (*StopMiddle == val->valcrc)
			return TS_MAYBE;
		else if (*StopMiddle < val->valcrc)
			StopLow = StopMiddle + 1;
		else
			StopHigh = StopMiddle;
	}

	return TS_NO;
}

/*
 * TS_execute 回调：tsquery 操作数与 GIST 非叶子页数据匹配
 */
static TSTernaryValue
checkcondition_bit(void *checkval, QueryOperand *val, ExecPhraseData *data)
{
	void	   *key = (SignTSVector *) checkval;

	/*
	 * 无法在签名树中找到前缀
	 */
	if (val->prefix)
		return TS_MAYBE;

	if (GETBIT(GETSIGN(key), HASHVAL(val->valcrc, GETSIGLEN(key))))
		return TS_MAYBE;
	else
		return TS_NO;
}

Datum
gtsvector_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	TSQuery		query = PG_GETARG_TSQUERY(1);

	/* StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2); */
	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	SignTSVector *key = (SignTSVector *) DatumGetPointer(entry->key);

	/* 此函数处理的情形均不精确 */
	*recheck = true;

	if (!query->size)
		PG_RETURN_BOOL(false);

	if (ISSIGNKEY(key))
	{
		if (ISALLTRUE(key))
			PG_RETURN_BOOL(true);

		PG_RETURN_BOOL(TS_execute(GETQUERY(query),
								  key,
								  TS_EXEC_PHRASE_NO_POS,
								  checkcondition_bit));
	}
	else
	{							/* 仅叶子页 */
		CHKVAL		chkval;

		chkval.arrb = GETARR(key);
		chkval.arre = chkval.arrb + ARRNELEM(key);
		PG_RETURN_BOOL(TS_execute(GETQUERY(query),
								  (void *) &chkval,
								  TS_EXEC_PHRASE_NO_POS,
								  checkcondition_arr));
	}
}

static int32
unionkey(BITVECP sbase, SignTSVector *add, int siglen)
{
	int32		i;

	if (ISSIGNKEY(add))
	{
		BITVECP		sadd = GETSIGN(add);

		if (ISALLTRUE(add))
			return 1;

		Assert(GETSIGLEN(add) == siglen);

		LOOPBYTE(siglen)
			sbase[i] |= sadd[i];
	}
	else
	{
		int32	   *ptr = GETARR(add);

		for (i = 0; i < ARRNELEM(add); i++)
			HASH(sbase, ptr[i], siglen);
	}
	return 0;
}


Datum
gtsvector_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
	SignTSVector *result = gtsvector_alloc(SIGNKEY, siglen, NULL);
	BITVECP		base = GETSIGN(result);
	int32		i;

	memset(base, 0, siglen);

	for (i = 0; i < entryvec->n; i++)
	{
		if (unionkey(base, GETENTRY(entryvec, i), siglen))
		{
			result->flag |= ALLISTRUE;
			SET_VARSIZE(result, CALCGTSIZE(result->flag, siglen));
			break;
		}
	}

	*size = VARSIZE(result);

	PG_RETURN_POINTER(result);
}

Datum
gtsvector_same(PG_FUNCTION_ARGS)
{
	SignTSVector *a = (SignTSVector *) PG_GETARG_POINTER(0);
	SignTSVector *b = (SignTSVector *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	int			siglen = GET_SIGLEN();

	if (ISSIGNKEY(a))
	{							/* 则 b 也 ISSIGNKEY */
		if (ISALLTRUE(a) && ISALLTRUE(b))
			*result = true;
		else if (ISALLTRUE(a))
			*result = false;
		else if (ISALLTRUE(b))
			*result = false;
		else
		{
			int32		i;
			BITVECP		sa = GETSIGN(a),
						sb = GETSIGN(b);

			Assert(GETSIGLEN(a) == siglen && GETSIGLEN(b) == siglen);

			*result = true;
			LOOPBYTE(siglen)
			{
				if (sa[i] != sb[i])
				{
					*result = false;
					break;
				}
			}
		}
	}
	else
	{							/* a and b ISARRKEY */
		int32		lena = ARRNELEM(a),
					lenb = ARRNELEM(b);

		if (lena != lenb)
			*result = false;
		else
		{
			int32	   *ptra = GETARR(a),
					   *ptrb = GETARR(b);
			int32		i;

			*result = true;
			for (i = 0; i < lena; i++)
				if (ptra[i] != ptrb[i])
				{
					*result = false;
					break;
				}
		}
	}

	PG_RETURN_POINTER(result);
}

static int32
sizebitvec(BITVECP sign, int siglen)
{
	return pg_popcount(sign, siglen);
}

static int
hemdistsign(BITVECP a, BITVECP b, int siglen)
{
	int			i,
				diff,
				dist = 0;

	LOOPBYTE(siglen)
	{
		diff = (unsigned char) (a[i] ^ b[i]);
		/* 在这里用 popcount 函数不太可能占优 */
		dist += pg_number_of_ones[diff];
	}
	return dist;
}

static int
hemdist(SignTSVector *a, SignTSVector *b)
{
	int			siglena = GETSIGLEN(a);
	int			siglenb = GETSIGLEN(b);

	if (ISALLTRUE(a))
	{
		if (ISALLTRUE(b))
			return 0;
		else
			return SIGLENBIT(siglenb) - sizebitvec(GETSIGN(b), siglenb);
	}
	else if (ISALLTRUE(b))
		return SIGLENBIT(siglena) - sizebitvec(GETSIGN(a), siglena);

	Assert(siglena == siglenb);

	return hemdistsign(GETSIGN(a), GETSIGN(b), siglena);
}

Datum
gtsvector_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0); /* 恒为 ISSIGNKEY */
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int			siglen = GET_SIGLEN();
	SignTSVector *origval = (SignTSVector *) DatumGetPointer(origentry->key);
	SignTSVector *newval = (SignTSVector *) DatumGetPointer(newentry->key);
	BITVECP		orig = GETSIGN(origval);

	*penalty = 0.0;

	if (ISARRKEY(newval))
	{
		BITVECP		sign = palloc(siglen);

		makesign(sign, newval, siglen);

		if (ISALLTRUE(origval))
		{
			int			siglenbit = SIGLENBIT(siglen);

			*penalty =
				(float) (siglenbit - sizebitvec(sign, siglen)) /
				(float) (siglenbit + 1);
		}
		else
			*penalty = hemdistsign(sign, orig, siglen);

		pfree(sign);
	}
	else
		*penalty = hemdist(origval, newval);
	PG_RETURN_POINTER(penalty);
}

typedef struct
{
	bool		allistrue;
	BITVECP		sign;
} CACHESIGN;

static void
fillcache(CACHESIGN *item, SignTSVector *key, int siglen)
{
	item->allistrue = false;
	if (ISARRKEY(key))
		makesign(item->sign, key, siglen);
	else if (ISALLTRUE(key))
		item->allistrue = true;
	else
		memcpy(item->sign, GETSIGN(key), siglen);
}

#define WISH_F(a,b,c) (double)( -(double)(((a)-(b))*((a)-(b))*((a)-(b)))*(c) )
typedef struct
{
	OffsetNumber pos;
	int32		cost;
} SPLITCOST;

static int
comparecost(const void *va, const void *vb)
{
	const SPLITCOST *a = (const SPLITCOST *) va;
	const SPLITCOST *b = (const SPLITCOST *) vb;

	if (a->cost == b->cost)
		return 0;
	else
		return (a->cost > b->cost) ? 1 : -1;
}


static int
hemdistcache(CACHESIGN *a, CACHESIGN *b, int siglen)
{
	if (a->allistrue)
	{
		if (b->allistrue)
			return 0;
		else
			return SIGLENBIT(siglen) - sizebitvec(b->sign, siglen);
	}
	else if (b->allistrue)
		return SIGLENBIT(siglen) - sizebitvec(a->sign, siglen);

	return hemdistsign(a->sign, b->sign, siglen);
}

Datum
gtsvector_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			siglen = GET_SIGLEN();
	OffsetNumber k,
				j;
	SignTSVector *datum_l,
			   *datum_r;
	BITVECP		union_l,
				union_r;
	int32		size_alpha,
				size_beta;
	int32		size_waste,
				waste = -1;
	int32		nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;
	BITVECP		ptr;
	int			i;
	CACHESIGN  *cache;
	char	   *cache_sign;
	SPLITCOST  *costvector;

	maxoff = entryvec->n - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	cache = (CACHESIGN *) palloc(sizeof(CACHESIGN) * (maxoff + 2));
	cache_sign = palloc(siglen * (maxoff + 2));

	for (j = 0; j < maxoff + 2; j++)
		cache[j].sign = &cache_sign[siglen * j];

	fillcache(&cache[FirstOffsetNumber], GETENTRY(entryvec, FirstOffsetNumber),
			  siglen);

	for (k = FirstOffsetNumber; k < maxoff; k = OffsetNumberNext(k))
	{
		for (j = OffsetNumberNext(k); j <= maxoff; j = OffsetNumberNext(j))
		{
			if (k == FirstOffsetNumber)
				fillcache(&cache[j], GETENTRY(entryvec, j), siglen);

			size_waste = hemdistcache(&(cache[j]), &(cache[k]), siglen);
			if (size_waste > waste)
			{
				waste = size_waste;
				seed_1 = k;
				seed_2 = j;
			}
		}
	}

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	if (seed_1 == 0 || seed_2 == 0)
	{
		seed_1 = 1;
		seed_2 = 2;
	}

	/* 生成初始 .. */
	datum_l = gtsvector_alloc(SIGNKEY | (cache[seed_1].allistrue ? ALLISTRUE : 0),
							  siglen, cache[seed_1].sign);
	datum_r = gtsvector_alloc(SIGNKEY | (cache[seed_2].allistrue ? ALLISTRUE : 0),
							  siglen, cache[seed_2].sign);
	union_l = GETSIGN(datum_l);
	union_r = GETSIGN(datum_r);
	maxoff = OffsetNumberNext(maxoff);
	fillcache(&cache[maxoff], GETENTRY(entryvec, maxoff), siglen);
	/* 排序前 ... */
	costvector = (SPLITCOST *) palloc(sizeof(SPLITCOST) * maxoff);
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		costvector[j - 1].pos = j;
		size_alpha = hemdistcache(&(cache[seed_1]), &(cache[j]), siglen);
		size_beta = hemdistcache(&(cache[seed_2]), &(cache[j]), siglen);
		costvector[j - 1].cost = abs(size_alpha - size_beta);
	}
	qsort(costvector, maxoff, sizeof(SPLITCOST), comparecost);

	for (k = 0; k < maxoff; k++)
	{
		j = costvector[k].pos;
		if (j == seed_1)
		{
			*left++ = j;
			v->spl_nleft++;
			continue;
		}
		else if (j == seed_2)
		{
			*right++ = j;
			v->spl_nright++;
			continue;
		}

		if (ISALLTRUE(datum_l) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_l) && cache[j].allistrue)
				size_alpha = 0;
			else
				size_alpha = SIGLENBIT(siglen) -
					sizebitvec((cache[j].allistrue) ?
							   GETSIGN(datum_l) :
							   cache[j].sign,
							   siglen);
		}
		else
			size_alpha = hemdistsign(cache[j].sign, GETSIGN(datum_l), siglen);

		if (ISALLTRUE(datum_r) || cache[j].allistrue)
		{
			if (ISALLTRUE(datum_r) && cache[j].allistrue)
				size_beta = 0;
			else
				size_beta = SIGLENBIT(siglen) -
					sizebitvec((cache[j].allistrue) ?
							   GETSIGN(datum_r) :
							   cache[j].sign,
							   siglen);
		}
		else
			size_beta = hemdistsign(cache[j].sign, GETSIGN(datum_r), siglen);

		if (size_alpha < size_beta + WISH_F(v->spl_nleft, v->spl_nright, 0.1))
		{
			if (ISALLTRUE(datum_l) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_l))
					memset(GETSIGN(datum_l), 0xff, siglen);
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(siglen)
					union_l[i] |= ptr[i];
			}
			*left++ = j;
			v->spl_nleft++;
		}
		else
		{
			if (ISALLTRUE(datum_r) || cache[j].allistrue)
			{
				if (!ISALLTRUE(datum_r))
					memset(GETSIGN(datum_r), 0xff, siglen);
			}
			else
			{
				ptr = cache[j].sign;
				LOOPBYTE(siglen)
					union_r[i] |= ptr[i];
			}
			*right++ = j;
			v->spl_nright++;
		}
	}

	*right = *left = FirstOffsetNumber;
	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	PG_RETURN_POINTER(v);
}

/*
 * 早年 gtsvector_consistent 在 pg_proc.h 中声明时所用参数
 * 不符合 GiST 支持函数文档化约定。我们修正了这一点，
 * 但仍需要一个使用旧签名的 pg_proc 条目，
 * 以支持重载 9.6 之前的 contrib/tsearch2 opclass 声明。
 * 这个兼容函数最终应当移除。
 */
Datum
gtsvector_consistent_oldsig(PG_FUNCTION_ARGS)
{
	return gtsvector_consistent(fcinfo);
}

Datum
gtsvector_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(GistTsVectorOptions));
	add_local_int_reloption(relopts, "siglen", "signature length",
							SIGLEN_DEFAULT, 1, SIGLEN_MAX,
							offsetof(GistTsVectorOptions, siglen));

	PG_RETURN_VOID();
}
