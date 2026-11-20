/*-------------------------------------------------------------------------
 *
 * reloptions.h
 *	  关系与表空间选项的核心支持（pg_class.reloptions 与
 *	  pg_tablespace.spcoptions）
 *
 * 注意：处理文本数组 reloptions 值的函数将其声明
 * 为 Datum，而非 ArrayType *，以免大量底层代码
 * 需要包含 array.h。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/reloptions.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELOPTIONS_H
#define RELOPTIONS_H

#include "access/amapi.h"
#include "access/htup.h"
#include "access/tupdesc.h"
#include "nodes/pg_list.h"
#include "storage/lock.h"

/* reloptions 支持的类型 */
typedef enum relopt_type
{
	RELOPT_TYPE_BOOL,
	RELOPT_TYPE_INT,
	RELOPT_TYPE_REAL,
	RELOPT_TYPE_ENUM,
	RELOPT_TYPE_STRING
} relopt_type;

/* reloptions 支持的种类 */
typedef enum relopt_kind
{
	RELOPT_KIND_LOCAL = 0,
	RELOPT_KIND_HEAP = (1 << 0),
	RELOPT_KIND_TOAST = (1 << 1),
	RELOPT_KIND_BTREE = (1 << 2),
	RELOPT_KIND_HASH = (1 << 3),
	RELOPT_KIND_GIN = (1 << 4),
	RELOPT_KIND_GIST = (1 << 5),
	RELOPT_KIND_ATTRIBUTE = (1 << 6),
	RELOPT_KIND_TABLESPACE = (1 << 7),
	RELOPT_KIND_SPGIST = (1 << 8),
	RELOPT_KIND_VIEW = (1 << 9),
	RELOPT_KIND_BRIN = (1 << 10),
	RELOPT_KIND_PARTITIONED = (1 << 11),
	/* 若新增种类，务必也更新 "last_default" */
	RELOPT_KIND_LAST_DEFAULT = RELOPT_KIND_PARTITIONED,
	/* 有的编译器视枚举为有符号 int，故无法用 1 << 31 */
	RELOPT_KIND_MAX = (1 << 30)
} relopt_kind;

/* 堆所允许的 reloption 命名空间 -- 目前仅 TOAST */
#define HEAP_RELOPT_NAMESPACES { "toast", NULL }

/* 存共享数据的通用结构体 */
typedef struct relopt_gen
{
	const char *name;			/* 须居首(用作列表终止
								 * 标记) */
	const char *desc;
	bits32		kinds;
	LOCKMODE	lockmode;
	int			namelen;
	relopt_type type;
} relopt_gen;

/* 保存已解析值 */
typedef struct relopt_value
{
	relopt_gen *gen;
	bool		isset;
	union
	{
		bool		bool_val;
		int			int_val;
		double		real_val;
		int			enum_val;
		char	   *string_val; /* 另行分配 */
	}			values;
} relopt_value;

/* 特定变量类型的 reloptions 记录 */
typedef struct relopt_bool
{
	relopt_gen	gen;
	bool		default_val;
} relopt_bool;

typedef struct relopt_int
{
	relopt_gen	gen;
	int			default_val;
	int			min;
	int			max;
} relopt_int;

typedef struct relopt_real
{
	relopt_gen	gen;
	double		default_val;
	double		min;
	double		max;
} relopt_real;

/*
 * relopt_enum_elt_def -- 枚举 reloption 可接受值数组的
 * 一个成员。
 */
typedef struct relopt_enum_elt_def
{
	const char *string_val;
	int			symbol_val;
} relopt_enum_elt_def;

typedef struct relopt_enum
{
	relopt_gen	gen;
	relopt_enum_elt_def *members;
	int			default_val;
	const char *detailmsg;
	/* 以 NUL 终止的成员数组 */
} relopt_enum;

/* 字符串的校验例程 */
typedef void (*validate_string_relopt) (const char *value);
typedef Size (*fill_string_relopt) (const char *value, void *ptr);

/* 整个选项集的校验例程 */
typedef void (*relopts_validator) (void *parsed_options, relopt_value *vals, int nvals);

typedef struct relopt_string
{
	relopt_gen	gen;
	int			default_len;
	bool		default_isnull;
	validate_string_relopt validate_cb;
	fill_string_relopt fill_cb;
	char	   *default_val;
} relopt_string;

/* build_reloptions() 所用的表数据类型 */
typedef struct
{
	const char *optname;		/* 选项名 */
	relopt_type opttype;		/* 选项类型 */
	int			offset;			/* 结果结构体中字段偏移 */
} relopt_parse_elt;

/* 局部 reloption 定义 */
typedef struct local_relopt
{
	relopt_gen *option;			/* 选项定义 */
	int			offset;			/* bytea 结构中已解析值的偏移 */
} local_relopt;

/* 供 build_local_reloptions() 存放局部 reloption 数据的结构 */
typedef struct local_relopts
{
	List	   *options;		/* local_relopt 定义的列表 */
	List	   *validators;		/* relopts_validator 回调的列表 */
	Size		relopt_struct_size; /* 已解析 bytea 结构大小 */
} local_relopts;

/*
 * 实用宏：选项一经解析即可取字符串 reloption 
 * 的值。得到指向字符串值本身的指针。"optstruct" 
 * 是 StdRdOptions 结构体或其等价物，"member" 是对应字符串
 * 选项的结构体成员。
 */
#define GET_STRING_RELOPTION(optstruct, member) \
	((optstruct)->member == 0 ? NULL : \
	 (char *)(optstruct) + (optstruct)->member)

extern relopt_kind add_reloption_kind(void);
extern void add_bool_reloption(bits32 kinds, const char *name, const char *desc,
							   bool default_val, LOCKMODE lockmode);
extern void add_int_reloption(bits32 kinds, const char *name, const char *desc,
							  int default_val, int min_val, int max_val,
							  LOCKMODE lockmode);
extern void add_real_reloption(bits32 kinds, const char *name, const char *desc,
							   double default_val, double min_val, double max_val,
							   LOCKMODE lockmode);
extern void add_enum_reloption(bits32 kinds, const char *name, const char *desc,
							   relopt_enum_elt_def *members, int default_val,
							   const char *detailmsg, LOCKMODE lockmode);
extern void add_string_reloption(bits32 kinds, const char *name, const char *desc,
								 const char *default_val, validate_string_relopt validator,
								 LOCKMODE lockmode);

extern void init_local_reloptions(local_relopts *relopts, Size relopt_struct_size);
extern void register_reloptions_validator(local_relopts *relopts,
										  relopts_validator validator);
extern void add_local_bool_reloption(local_relopts *relopts, const char *name,
									 const char *desc, bool default_val,
									 int offset);
extern void add_local_int_reloption(local_relopts *relopts, const char *name,
									const char *desc, int default_val,
									int min_val, int max_val, int offset);
extern void add_local_real_reloption(local_relopts *relopts, const char *name,
									 const char *desc, double default_val,
									 double min_val, double max_val,
									 int offset);
extern void add_local_enum_reloption(local_relopts *relopts,
									 const char *name, const char *desc,
									 relopt_enum_elt_def *members,
									 int default_val, const char *detailmsg,
									 int offset);
extern void add_local_string_reloption(local_relopts *relopts, const char *name,
									   const char *desc,
									   const char *default_val,
									   validate_string_relopt validator,
									   fill_string_relopt filler, int offset);

extern Datum transformRelOptions(Datum oldOptions, List *defList,
								 const char *namspace, char *validnsps[],
								 bool acceptOidsOff, bool isReset);
extern List *untransformRelOptions(Datum options);
extern bytea *extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
								amoptions_function amoptions);
extern void *build_reloptions(Datum reloptions, bool validate,
							  relopt_kind kind,
							  Size relopt_struct_size,
							  const relopt_parse_elt *relopt_elems,
							  int num_relopt_elems);
extern void *build_local_reloptions(local_relopts *relopts, Datum options,
									bool validate);

extern bytea *default_reloptions(Datum reloptions, bool validate,
								 relopt_kind kind);
extern bytea *heap_reloptions(char relkind, Datum reloptions, bool validate);
extern bytea *view_reloptions(Datum reloptions, bool validate);
extern bytea *partitioned_table_reloptions(Datum reloptions, bool validate);
extern bytea *index_reloptions(amoptions_function amoptions, Datum reloptions,
							   bool validate);
extern bytea *attribute_reloptions(Datum reloptions, bool validate);
extern bytea *tablespace_reloptions(Datum reloptions, bool validate);
extern LOCKMODE AlterTableGetRelOptionsLockLevel(List *defList);

#endif							/* RELOPTIONS_H */
