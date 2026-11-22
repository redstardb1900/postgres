/*-------------------------------------------------------------------------
 *
 * jsonb.h
 *	  Declarations for jsonb data type support.
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/utils/jsonb.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __JSONB_H__
#define __JSONB_H__

#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/numeric.h"

/*  顺序处理 jsonb 值时所用的词元  */
typedef enum
{
	WJB_DONE,
	WJB_KEY,
	WJB_VALUE,
	WJB_ELEM,
	WJB_BEGIN_ARRAY,
	WJB_END_ARRAY,
	WJB_BEGIN_OBJECT,
	WJB_END_OBJECT
} JsonbIteratorToken;

/*  GIN 索引操作类的策略编号  */
#define JsonbContainsStrategyNumber		7
#define JsonbExistsStrategyNumber		9
#define JsonbExistsAnyStrategyNumber	10
#define JsonbExistsAllStrategyNumber	11
#define JsonbJsonpathExistsStrategyNumber		15
#define JsonbJsonpathPredicateStrategyNumber	16


/*
 * In the standard jsonb_ops GIN opclass for jsonb, we choose to index both
 * keys and values.  The storage format is text.  The first byte of the text
 * string distinguishes whether this is a key (always a string), null value,
 * boolean value, numeric value, or string value.  However, array elements
 * that are strings are marked as though they were keys; this imprecision
 * supports the definition of the "exists" operator, which treats array
 * elements like keys.  The remainder of the text string is empty for a null
 * value, "t" or "f" for a boolean value, a normalized print representation of
 * a numeric value, or the text of a string value.  However, if the length of
 * this text representation would exceed JGIN_MAXLENGTH bytes, we instead hash
 * the text representation and store an 8-hex-digit representation of the
 * uint32 hash value, marking the prefix byte with an additional bit to
 * distinguish that this has happened.  Hashing long strings saves space and
 * ensures that we won't overrun the maximum entry length for a GIN index.
 * (But JGIN_MAXLENGTH is quite a bit shorter than GIN's limit.  It's chosen
 * to ensure that the on-disk text datum will have a short varlena header.)
 * Note that when any hashed item appears in a query, we must recheck index
 * matches against the heap tuple; currently, this costs nothing because we
 * must always recheck for other reasons.
 */
#define JGINFLAG_KEY	0x01	/*  键或字符串数组元素  */
#define JGINFLAG_NULL	0x02	/*  空值  */
#define JGINFLAG_BOOL	0x03	/*  布尔值  */
#define JGINFLAG_NUM	0x04	/*  数值  */
#define JGINFLAG_STR	0x05	/*  字符串值（若非数组元素）  */
#define JGINFLAG_HASHED 0x10	/*  若值已哈希则 OR 进标志  */
#define JGIN_MAXLENGTH	125		/*  哈希前文本部分最大长度  */

typedef struct JsonbPair JsonbPair;
typedef struct JsonbValue JsonbValue;

/*
 * Jsonbs are varlena objects, so must meet the varlena convention that the
 * first int32 of the object contains the total object size in bytes.  Be sure
 * to use VARSIZE() and SET_VARSIZE() to access it, though!
 *
 * Jsonb is the on-disk representation, in contrast to the in-memory JsonbValue
 * representation.  Often, JsonbValues are just shims through which a Jsonb
 * buffer is accessed, but they can also be deep copied and passed around.
 *
 * Jsonb is a tree structure. Each node in the tree consists of a JEntry
 * header and a variable-length content (possibly of zero size).  The JEntry
 * header indicates what kind of a node it is, e.g. a string or an array,
 * and provides the length of its variable-length portion.
 *
 * The JEntry and the content of a node are not stored physically together.
 * Instead, the container array or object has an array that holds the JEntrys
 * of all the child nodes, followed by their variable-length portions.
 *
 * The root node is an exception; it has no parent array or object that could
 * hold its JEntry. Hence, no JEntry header is stored for the root node.  It
 * is implicitly known that the root node must be an array or an object,
 * so we can get away without the type indicator as long as we can distinguish
 * the two.  For that purpose, both an array and an object begin with a uint32
 * header field, which contains an JB_FOBJECT or JB_FARRAY flag.  When a naked
 * scalar value needs to be stored as a Jsonb value, what we actually store is
 * an array with one element, with the flags in the array's header field set
 * to JB_FSCALAR | JB_FARRAY.
 *
 * Overall, the Jsonb struct requires 4-bytes alignment. Within the struct,
 * the variable-length portion of some node types is aligned to a 4-byte
 * boundary, while others are not. When alignment is needed, the padding is
 * in the beginning of the node that requires it. For example, if a numeric
 * node is stored after a string node, so that the numeric node begins at
 * offset 3, the variable-length portion of the numeric node will begin with
 * one padding byte so that the actual numeric data is 4-byte aligned.
 */

/*
 * JEntry format.
 *
 * The least significant 28 bits store either the data length of the entry,
 * or its end+1 offset from the start of the variable-length portion of the
 * containing object.  The next three bits store the type of the entry, and
 * the high-order bit tells whether the least significant bits store a length
 * or an offset.
 *
 * The reason for the offset-or-length complication is to compromise between
 * access speed and data compressibility.  In the initial design each JEntry
 * always stored an offset, but this resulted in JEntry arrays with horrible
 * compressibility properties, so that TOAST compression of a JSONB did not
 * work well.  Storing only lengths would greatly improve compressibility,
 * but it makes random access into large arrays expensive (O(N) not O(1)).
 * So what we do is store an offset in every JB_OFFSET_STRIDE'th JEntry and
 * a length in the rest.  This results in reasonably compressible data (as
 * long as the stride isn't too small).  We may have to examine as many as
 * JB_OFFSET_STRIDE JEntrys in order to find out the offset or length of any
 * given item, but that's still O(1) no matter how large the container is.
 *
 * We could avoid eating a flag bit for this purpose if we were to store
 * the stride in the container header, or if we were willing to treat the
 * stride as an unchangeable constant.  Neither of those options is very
 * attractive though.
 */
typedef uint32 JEntry;

#define JENTRY_OFFLENMASK		0x0FFFFFFF
#define JENTRY_TYPEMASK			0x70000000
#define JENTRY_HAS_OFF			0x80000000

/*  类型位中存放的值  */
#define JENTRY_ISSTRING			0x00000000
#define JENTRY_ISNUMERIC		0x10000000
#define JENTRY_ISBOOL_FALSE		0x20000000
#define JENTRY_ISBOOL_TRUE		0x30000000
#define JENTRY_ISNULL			0x40000000
#define JENTRY_ISCONTAINER		0x50000000	/* array or object */

/*  访问宏。注意可能多次求值  */
#define JBE_OFFLENFLD(je_)		((je_) & JENTRY_OFFLENMASK)
#define JBE_HAS_OFF(je_)		(((je_) & JENTRY_HAS_OFF) != 0)
#define JBE_ISSTRING(je_)		(((je_) & JENTRY_TYPEMASK) == JENTRY_ISSTRING)
#define JBE_ISNUMERIC(je_)		(((je_) & JENTRY_TYPEMASK) == JENTRY_ISNUMERIC)
#define JBE_ISCONTAINER(je_)	(((je_) & JENTRY_TYPEMASK) == JENTRY_ISCONTAINER)
#define JBE_ISNULL(je_)			(((je_) & JENTRY_TYPEMASK) == JENTRY_ISNULL)
#define JBE_ISBOOL_TRUE(je_)	(((je_) & JENTRY_TYPEMASK) == JENTRY_ISBOOL_TRUE)
#define JBE_ISBOOL_FALSE(je_)	(((je_) & JENTRY_TYPEMASK) == JENTRY_ISBOOL_FALSE)
#define JBE_ISBOOL(je_)			(JBE_ISBOOL_TRUE(je_) || JBE_ISBOOL_FALSE(je_))

/*  把偏移变量推进到下一 JEntry 的宏  */
#define JBE_ADVANCE_OFFSET(offset, je) \
	do { \
		JEntry	je_ = (je); \
		if (JBE_HAS_OFF(je_)) \
			(offset) = JBE_OFFLENFLD(je_); \
		else \
			(offset) += JBE_OFFLENFLD(je_); \
	} while(0)

/*
 * We store an offset, not a length, every JB_OFFSET_STRIDE children.
 * Caution: this macro should only be referenced when creating a JSONB
 * value.  When examining an existing value, pay attention to the HAS_OFF
 * bits instead.  This allows changes in the offset-placement heuristic
 * without breaking on-disk compatibility.
 */
#define JB_OFFSET_STRIDE		32

/*
 * A jsonb array or object node, within a Jsonb Datum.
 *
 * An array has one child for each element, stored in array order.
 *
 * An object has two children for each key/value pair.  The keys all appear
 * first, in key sort order; then the values appear, in an order matching the
 * key order.  This arrangement keeps the keys compact in memory, making a
 * search for a particular key more cache-friendly.
 */
typedef struct JsonbContainer
{
	uint32		header;			/*  元素或键/值对数，以及
								 * flags */
	JEntry		children[FLEXIBLE_ARRAY_MEMBER];

	/*  各子节点的数据紧随其后.  */
} JsonbContainer;

/*  JsonbContainer 头字段的标志  */
#define JB_CMASK				0x0FFFFFFF	/*  计数字段掩码  */
#define JB_FSCALAR				0x10000000	/* flag bits */
#define JB_FOBJECT				0x20000000
#define JB_FARRAY				0x40000000

/*  访问 JsonbContainer 结构体的便利宏  */
#define JsonContainerSize(jc)		((jc)->header & JB_CMASK)
#define JsonContainerIsScalar(jc)	(((jc)->header & JB_FSCALAR) != 0)
#define JsonContainerIsObject(jc)	(((jc)->header & JB_FOBJECT) != 0)
#define JsonContainerIsArray(jc)	(((jc)->header & JB_FARRAY) != 0)

/*  jsonb 的顶层磁盘格式。  */
typedef struct
{
	int32		vl_len_;		/*  varlena 头（勿直接碰！）  */
	JsonbContainer root;
} Jsonb;

/*  访问 Jsonb 根容器的便利宏  */
#define JB_ROOT_COUNT(jbp_)		(*(uint32 *) VARDATA(jbp_) & JB_CMASK)
#define JB_ROOT_IS_SCALAR(jbp_) ((*(uint32 *) VARDATA(jbp_) & JB_FSCALAR) != 0)
#define JB_ROOT_IS_OBJECT(jbp_) ((*(uint32 *) VARDATA(jbp_) & JB_FOBJECT) != 0)
#define JB_ROOT_IS_ARRAY(jbp_)	((*(uint32 *) VARDATA(jbp_) & JB_FARRAY) != 0)


enum jbvType
{
	/* Scalar types */
	jbvNull = 0x0,
	jbvString,
	jbvNumeric,
	jbvBool,
	/*  复合类型  */
	jbvArray = 0x10,
	jbvObject,
	/*  二进制 Jsonb 的 jbvArray/jbvObject  */
	jbvBinary,

	/*
	 * 虚拟类型.
	 *
	 * 这些类型仅用于内存中 JSON 处理，输出到
	 * json/jsonb 时序列化为 JSON 字符串。
	 */
	jbvDatetime = 0x20,
};

/*
 * JsonbValue:	In-memory representation of Jsonb.  This is a convenient
 * deserialized representation, that can easily support using the "val"
 * union across underlying types during manipulation.  The Jsonb on-disk
 * representation has various alignment considerations.
 */
struct JsonbValue
{
	enum jbvType type;			/*  影响排序顺序  */

	union
	{
		Numeric numeric;
		bool		boolean;
		struct
		{
			int			len;
			char	   *val;	/*  不一定空字符结尾  */
		}			string;		/* String primitive type */

		struct
		{
			int			nElems;
			JsonbValue *elems;
			bool		rawScalar;	/*  顶层"裸标量"数组？  */
		}			array;		/*  数组容器类型  */

		struct
		{
			int			nPairs; /*  1 对，2 元素  */
			JsonbPair  *pairs;
		}			object;		/*  关联容器类型  */

		struct
		{
			int			len;
			JsonbContainer *data;
		}			binary;		/*  数组或对象，磁盘格式  */

		struct
		{
			Datum		value;
			Oid			typid;
			int32		typmod;
			int			tz;		/*  数值时区，单位秒，用于
								 * TimestampTz 类型  */
		}			datetime;
	}			val;
};

#define IsAJsonbScalar(jsonbval)	(((jsonbval)->type >= jbvNull && \
									  (jsonbval)->type <= jbvBool) || \
									  (jsonbval)->type == jbvDatetime)

/*
 * Key/value pair within an Object.
 *
 * This struct type is only used briefly while constructing a Jsonb; it is
 * *not* the on-disk representation.
 *
 * Pairs with duplicate keys are de-duplicated.  We store the originally
 * observed pair ordering for the purpose of removing duplicates in a
 * well-defined way (which is "last observed wins").
 */
struct JsonbPair
{
	JsonbValue	key;			/* Must be a jbvString */
	JsonbValue	value;			/* May be of any type */
	uint32		order;			/* Pair's index in original sequence */
};

/*  从文本解析 Jsonb 或做类型强制时的转换状态  */
typedef struct JsonbParseState
{
	JsonbValue	contVal;
	Size		size;
	struct JsonbParseState *next;
	bool		unique_keys;	/*  检查对象键唯一性  */
	bool		skip_nulls;		/*  跳过空对象字段  */
} JsonbParseState;

/*
 * JsonbIterator holds details of the type for each iteration. It also stores a
 * Jsonb varlena buffer, which can be directly accessed in some contexts.
 */
typedef enum
{
	JBI_ARRAY_START,
	JBI_ARRAY_ELEM,
	JBI_OBJECT_START,
	JBI_OBJECT_KEY,
	JBI_OBJECT_VALUE
} JsonbIterState;

typedef struct JsonbIterator
{
	/*  正在迭代的容器  */
	JsonbContainer *container;
	uint32		nElems;			/*  children 数组元素个数(对象则为
								 * nPairs）  */
	bool		isScalar;		/*  伪数组标量值？  */
	JEntry	   *children;		/*  子节点的 JEntrys  */
	/*  数据本体。指向变长数据的起始  */
	char	   *dataProper;

	/*  缓冲区当前项(至多 nElems)  */
	int			curIndex;

	/*  对应当前项的数据偏移  */
	uint32		curDataOffset;

	/*
	 * If the container is an object, we want to return keys and values
	 * alternately; so curDataOffset points to the current key, and
	 * curValueOffset points to the current value.
	 */
	uint32		curValueOffset;

	/* Private state */
	JsonbIterState state;

	struct JsonbIterator *parent;
} JsonbIterator;


/*  便利宏  */
static inline Jsonb *
DatumGetJsonbP(Datum d)
{
	return (Jsonb *) PG_DETOAST_DATUM(d);
}

static inline Jsonb *
DatumGetJsonbPCopy(Datum d)
{
	return (Jsonb *) PG_DETOAST_DATUM_COPY(d);
}

static inline Datum
JsonbPGetDatum(const Jsonb *p)
{
	return PointerGetDatum(p);
}

#define PG_GETARG_JSONB_P(x)	DatumGetJsonbP(PG_GETARG_DATUM(x))
#define PG_GETARG_JSONB_P_COPY(x)	DatumGetJsonbPCopy(PG_GETARG_DATUM(x))
#define PG_RETURN_JSONB_P(x)	PG_RETURN_POINTER(x)

/*  支持函数  */
extern uint32 getJsonbOffset(const JsonbContainer *jc, int index);
extern uint32 getJsonbLength(const JsonbContainer *jc, int index);
extern int	compareJsonbContainers(JsonbContainer *a, JsonbContainer *b);
extern JsonbValue *findJsonbValueFromContainer(JsonbContainer *container,
											   uint32 flags,
											   JsonbValue *key);
extern JsonbValue *getKeyJsonValueFromContainer(JsonbContainer *container,
												const char *keyVal, int keyLen,
												JsonbValue *res);
extern JsonbValue *getIthJsonbValueFromContainer(JsonbContainer *container,
												 uint32 i);
extern JsonbValue *pushJsonbValue(JsonbParseState **pstate,
								  JsonbIteratorToken seq, JsonbValue *jbval);
extern JsonbIterator *JsonbIteratorInit(JsonbContainer *container);
extern JsonbIteratorToken JsonbIteratorNext(JsonbIterator **it, JsonbValue *val,
											bool skipNested);
extern void JsonbToJsonbValue(Jsonb *jsonb, JsonbValue *val);
extern Jsonb *JsonbValueToJsonb(JsonbValue *val);
extern bool JsonbDeepContains(JsonbIterator **val,
							  JsonbIterator **mContained);
extern void JsonbHashScalarValue(const JsonbValue *scalarVal, uint32 *hash);
extern void JsonbHashScalarValueExtended(const JsonbValue *scalarVal,
										 uint64 *hash, uint64 seed);

/*  jsonb.c 支持函数  */
extern char *JsonbToCString(StringInfo out, JsonbContainer *in,
							int estimated_len);
extern char *JsonbToCStringIndent(StringInfo out, JsonbContainer *in,
								  int estimated_len);
extern bool JsonbExtractScalar(JsonbContainer *jbc, JsonbValue *res);
extern const char *JsonbTypeName(JsonbValue *val);

extern Datum jsonb_set_element(Jsonb *jb, Datum *path, int path_len,
							   JsonbValue *newval);
extern Datum jsonb_get_element(Jsonb *jb, Datum *path, int npath,
							   bool *isnull, bool as_text);
extern bool to_jsonb_is_immutable(Oid typoid);
extern Datum jsonb_build_object_worker(int nargs, Datum *args, bool *nulls,
									   Oid *types, bool absent_on_null,
									   bool unique_keys);
extern Datum jsonb_build_array_worker(int nargs, Datum *args, bool *nulls,
									  Oid *types, bool absent_on_null);

#endif							/* __JSONB_H__ */
