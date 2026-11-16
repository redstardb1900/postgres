/*
 * dbsize.c
 *		数据库对象尺寸函数与相关查询
 *
 * Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/dbsize.c
 *
 */

#include "postgres.h"

#include <sys/stat.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/relfilenumbermap.h"
#include "utils/relmapper.h"
#include "utils/syscache.h"

/* 除以二并远离零舍入 */
#define half_rounded(x)   (((x) + ((x) < 0 ? -1 : 1)) / 2)

/* pg_size_pretty 函数所用的单位。所有单位必须是 2 的幂 */
struct size_pretty_unit
{
	const char *name;			/* 字节 kB MB GB 等 */
	uint32		limit;			/* 半舍入转换成该单位
								 * 之前的上限。 */
	bool		round;			/* 该单位做半舍入 */
	uint8		unitbits;		/* (1 << unitbits) 字节折合 1 个单
								 * 位 */
};

/* 在此处添加单位时，也请更新文档与 pg_size_bytes 的错误信息 */
static const struct size_pretty_unit size_pretty_units[] = {
	{"bytes", 10 * 1024, false, 0},
	{"kB", 20 * 1024 - 1, true, 10},
	{"MB", 20 * 1024 - 1, true, 20},
	{"GB", 20 * 1024 - 1, true, 30},
	{"TB", 20 * 1024 - 1, true, 40},
	{"PB", 20 * 1024 - 1, true, 50},
	{NULL, 0, false, 0}
};

/* pg_size_bytes 接受的附加单位别名 */
struct size_bytes_unit_alias
{
	const char *alias;
	int			unit_index;		/* 对应的 size_pretty_units 元素 */
};

/* 在此处添加单位时，也请更新文档与 pg_size_bytes 的错误信息 */
static const struct size_bytes_unit_alias size_bytes_aliases[] = {
	{"B", 0},
	{NULL}
};

/* 返回目录内容的物理尺寸；目录不存在时返回 0 */
static int64
db_dir_size(const char *path)
{
	int64		dirsize = 0;
	struct dirent *direntry;
	DIR		   *dirdesc;
	char		filename[MAXPGPATH * 2];

	dirdesc = AllocateDir(path);

	if (!dirdesc)
		return 0;

	while ((direntry = ReadDir(dirdesc, path)) != NULL)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(filename, sizeof(filename), "%s/%s", path, direntry->d_name);

		if (stat(filename, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", filename)));
		}
		dirsize += fst.st_size;
	}

	FreeDir(dirdesc);
	return dirsize;
}

/*
 * 计算所有表空间中的数据库尺寸
 */
static int64
calculate_database_size(Oid dbOid)
{
	int64		totalsize;
	DIR		   *dirdesc;
	struct dirent *direntry;
	char		dirpath[MAXPGPATH];
	char		pathname[MAXPGPATH + 21 + sizeof(TABLESPACE_VERSION_DIRECTORY)];
	AclResult	aclresult;

	/*
	 * 用户须有目标数据库的连接权限，或拥有
	 * pg_read_all_stats
	 */
	aclresult = object_aclcheck(DatabaseRelationId, dbOid, GetUserId(), ACL_CONNECT);
	if (aclresult != ACLCHECK_OK &&
		!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		aclcheck_error(aclresult, OBJECT_DATABASE,
					   get_database_name(dbOid));
	}

	/* pg_global 中的共享存储不计入 */

	/* 计入 pg_default 存储 */
	snprintf(pathname, sizeof(pathname), "base/%u", dbOid);
	totalsize = db_dir_size(pathname);

	/* 扫描非默认表空间 */
	snprintf(dirpath, MAXPGPATH, "pg_tblspc");
	dirdesc = AllocateDir(dirpath);

	while ((direntry = ReadDir(dirdesc, dirpath)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(pathname, sizeof(pathname), "pg_tblspc/%s/%s/%u",
				 direntry->d_name, TABLESPACE_VERSION_DIRECTORY, dbOid);
		totalsize += db_dir_size(pathname);
	}

	FreeDir(dirdesc);

	return totalsize;
}

Datum
pg_database_size_oid(PG_FUNCTION_ARGS)
{
	Oid			dbOid = PG_GETARG_OID(0);
	int64		size;

	size = calculate_database_size(dbOid);

	if (size == 0)
		PG_RETURN_NULL();

	PG_RETURN_INT64(size);
}

Datum
pg_database_size_name(PG_FUNCTION_ARGS)
{
	Name		dbName = PG_GETARG_NAME(0);
	Oid			dbOid = get_database_oid(NameStr(*dbName), false);
	int64		size;

	size = calculate_database_size(dbOid);

	if (size == 0)
		PG_RETURN_NULL();

	PG_RETURN_INT64(size);
}


/*
 * 计算表空间的总尺寸。若表空间目录找不到，则
 * 返回 -1。
 */
static int64
calculate_tablespace_size(Oid tblspcOid)
{
	char		tblspcPath[MAXPGPATH];
	char		pathname[MAXPGPATH * 2];
	int64		totalsize = 0;
	DIR		   *dirdesc;
	struct dirent *direntry;
	AclResult	aclresult;

	/*
	 * 用户必须拥有 pg_read_all_stats 权限，或对目标
	 * 表空间有 CREATE 权限，显式授予或因它是当前
	 * 默认表空间而隐式获得。
	 */
	if (tblspcOid != MyDatabaseTableSpace &&
		!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS))
	{
		aclresult = object_aclcheck(TableSpaceRelationId, tblspcOid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tblspcOid));
	}

	if (tblspcOid == DEFAULTTABLESPACE_OID)
		snprintf(tblspcPath, MAXPGPATH, "base");
	else if (tblspcOid == GLOBALTABLESPACE_OID)
		snprintf(tblspcPath, MAXPGPATH, "global");
	else
		snprintf(tblspcPath, MAXPGPATH, "pg_tblspc/%u/%s", tblspcOid,
				 TABLESPACE_VERSION_DIRECTORY);

	dirdesc = AllocateDir(tblspcPath);

	if (!dirdesc)
		return -1;

	while ((direntry = ReadDir(dirdesc, tblspcPath)) != NULL)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(direntry->d_name, ".") == 0 ||
			strcmp(direntry->d_name, "..") == 0)
			continue;

		snprintf(pathname, sizeof(pathname), "%s/%s", tblspcPath, direntry->d_name);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				continue;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathname)));
		}

		if (S_ISDIR(fst.st_mode))
			totalsize += db_dir_size(pathname);

		totalsize += fst.st_size;
	}

	FreeDir(dirdesc);

	return totalsize;
}

Datum
pg_tablespace_size_oid(PG_FUNCTION_ARGS)
{
	Oid			tblspcOid = PG_GETARG_OID(0);
	int64		size;

	size = calculate_tablespace_size(tblspcOid);

	if (size < 0)
		PG_RETURN_NULL();

	PG_RETURN_INT64(size);
}

Datum
pg_tablespace_size_name(PG_FUNCTION_ARGS)
{
	Name		tblspcName = PG_GETARG_NAME(0);
	Oid			tblspcOid = get_tablespace_oid(NameStr(*tblspcName), false);
	int64		size;

	size = calculate_tablespace_size(tblspcOid);

	if (size < 0)
		PG_RETURN_NULL();

	PG_RETURN_INT64(size);
}


/*
 * 计算关系（某个分支）的尺寸
 *
 * 注意：我们可以安全地把本函数用于其他会话的
 * 临时表，此处与调用点均不检查。
 */
static int64
calculate_relation_size(RelFileLocator *rfn, BackendId backend, ForkNumber forknum)
{
	int64		totalsize = 0;
	char	   *relationpath;
	char		pathname[MAXPGPATH];
	unsigned int segcount = 0;

	relationpath = relpathbackend(*rfn, backend, forknum);

	for (segcount = 0;; segcount++)
	{
		struct stat fst;

		CHECK_FOR_INTERRUPTS();

		if (segcount == 0)
			snprintf(pathname, MAXPGPATH, "%s",
					 relationpath);
		else
			snprintf(pathname, MAXPGPATH, "%s.%u",
					 relationpath, segcount);

		if (stat(pathname, &fst) < 0)
		{
			if (errno == ENOENT)
				break;
			else
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathname)));
		}
		totalsize += fst.st_size;
	}

	return totalsize;
}

Datum
pg_relation_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	text	   *forkName = PG_GETARG_TEXT_PP(1);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	/*
	 * 在 9.2 之前，若关系不存在我们会抛出错误，但这
	 * 会让形如 "SELECT pg_relation_size(oid) FROM pg_class"
	 * 的查询不够健壮：我们用 MVCC 快照扫描
	 * 时别人可能删掉该表。对已删除的表返回
	 * NULL 要好于抛出错误并中止整个查询。
	 */
	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_relation_size(&(rel->rd_locator), rel->rd_backend,
								   forkname_to_number(text_to_cstring(forkName)));

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 * 计算 TOAST 关系的总磁盘尺寸，包括其索引。
 * 不得用于非 TOAST 关系。
 */
static int64
calculate_toast_table_size(Oid toastrelid)
{
	int64		size = 0;
	Relation	toastRel;
	ForkNumber	forkNum;
	ListCell   *lc;
	List	   *indexlist;

	toastRel = relation_open(toastrelid, AccessShareLock);

	/* toast 堆尺寸，含 FSM 和 VM 尺寸 */
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(toastRel->rd_locator),
										toastRel->rd_backend, forkNum);

	/* toast 索引尺寸，含 FSM/VM 尺寸 */
	indexlist = RelationGetIndexList(toastRel);

	/* 尺寸用所有可用索引来计算 */
	foreach(lc, indexlist)
	{
		Relation	toastIdxRel;

		toastIdxRel = relation_open(lfirst_oid(lc),
									AccessShareLock);
		for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
			size += calculate_relation_size(&(toastIdxRel->rd_locator),
											toastIdxRel->rd_backend, forkNum);

		relation_close(toastIdxRel, AccessShareLock);
	}
	list_free(indexlist);
	relation_close(toastRel, AccessShareLock);

	return size;
}

/*
 * 计算给定表的总磁盘尺寸，
 * 包括 FSM/VM，如有 TOAST 表也计入。
 * 除 TOAST 表索引之外的索引不计入。
 *
 * 注意若用于索引或 toast 表，本函数的行为也是合理的；
 * 它们没有附接的 toast 表，但可以有多个分支。
 */
static int64
calculate_table_size(Relation rel)
{
	int64		size = 0;
	ForkNumber	forkNum;

	/*
	 * 堆尺寸，含 FSM 与 VM
	 */
	for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
		size += calculate_relation_size(&(rel->rd_locator), rel->rd_backend,
										forkNum);

	/*
	 * toast 关系尺寸
	 */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
		size += calculate_toast_table_size(rel->rd_rel->reltoastrelid);

	return size;
}

/*
 * 计算附接到给定表的所有索引的总磁盘尺寸。
 *
 * 可安全地用于索引，但只会得到零。
 */
static int64
calculate_indexes_size(Relation rel)
{
	int64		size = 0;

	/*
	 * 聚合给定关系上的所有索引
	 */
	if (rel->rd_rel->relhasindex)
	{
		List	   *index_oids = RelationGetIndexList(rel);
		ListCell   *cell;

		foreach(cell, index_oids)
		{
			Oid			idxOid = lfirst_oid(cell);
			Relation	idxRel;
			ForkNumber	forkNum;

			idxRel = relation_open(idxOid, AccessShareLock);

			for (forkNum = 0; forkNum <= MAX_FORKNUM; forkNum++)
				size += calculate_relation_size(&(idxRel->rd_locator),
												idxRel->rd_backend,
												forkNum);

			relation_close(idxRel, AccessShareLock);
		}

		list_free(index_oids);
	}

	return size;
}

Datum
pg_table_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_table_size(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

Datum
pg_indexes_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_indexes_size(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 *	计算关系所有文件的磁盘尺寸，
 *	包括堆、索引、toast 数据、FSM、VM。
 */
static int64
calculate_total_relation_size(Relation rel)
{
	int64		size;

	/*
	 * 聚合表尺寸，其中包括堆、toast 以及
	 * toast 索引与空闲空间和可见性图
	 */
	size = calculate_table_size(rel);

	/*
	 * 再加上所有附接索引的尺寸
	 */
	size += calculate_indexes_size(rel);

	return size;
}

Datum
pg_total_relation_size(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	Relation	rel;
	int64		size;

	rel = try_relation_open(relOid, AccessShareLock);

	if (rel == NULL)
		PG_RETURN_NULL();

	size = calculate_total_relation_size(rel);

	relation_close(rel, AccessShareLock);

	PG_RETURN_INT64(size);
}

/*
 * 以尺寸单位格式化
 */
Datum
pg_size_pretty(PG_FUNCTION_ARGS)
{
	int64		size = PG_GETARG_INT64(0);
	char		buf[64];
	const struct size_pretty_unit *unit;

	for (unit = size_pretty_units; unit->name != NULL; unit++)
	{
		uint8		bits;
		uint64		abs_size = size < 0 ? 0 - (uint64) size : (uint64) size;

		/*
		 * 若已无更多单位或绝对尺寸低于当前单位
		 * 的限制时，使用该单位。
		 */
		if (unit[1].name == NULL || abs_size < unit->limit)
		{
			if (unit->round)
				size = half_rounded(size);

			snprintf(buf, sizeof(buf), INT64_FORMAT " %s", size, unit->name);
			break;
		}

		/*
		 * 确定构造除数所用的位数。若下一级单位做半
		 * 舍入，就要比本级与下一级的差值少用 1 位。
		 * 或者，若本级做半舍入而下一级不做，
		 * 就额外多移 1 位。为此我们用除法而不是
		 * 按此位数右移，以确保正值与负值能
		 * 以相同的方式
		 * 舍入。
		 */
		bits = (unit[1].unitbits - unit->unitbits - (unit[1].round == true)
				+ (unit->round == true));
		size /= ((int64) 1) << bits;
	}

	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

static char *
numeric_to_cstring(Numeric n)
{
	Datum		d = NumericGetDatum(n);

	return DatumGetCString(DirectFunctionCall1(numeric_out, d));
}

static bool
numeric_is_less(Numeric a, Numeric b)
{
	Datum		da = NumericGetDatum(a);
	Datum		db = NumericGetDatum(b);

	return DatumGetBool(DirectFunctionCall2(numeric_lt, da, db));
}

static Numeric
numeric_absolute(Numeric n)
{
	Datum		d = NumericGetDatum(n);
	Datum		result;

	result = DirectFunctionCall1(numeric_abs, d);
	return DatumGetNumeric(result);
}

static Numeric
numeric_half_rounded(Numeric n)
{
	Datum		d = NumericGetDatum(n);
	Datum		zero;
	Datum		one;
	Datum		two;
	Datum		result;

	zero = NumericGetDatum(int64_to_numeric(0));
	one = NumericGetDatum(int64_to_numeric(1));
	two = NumericGetDatum(int64_to_numeric(2));

	if (DatumGetBool(DirectFunctionCall2(numeric_ge, d, zero)))
		d = DirectFunctionCall2(numeric_add, d, one);
	else
		d = DirectFunctionCall2(numeric_sub, d, one);

	result = DirectFunctionCall2(numeric_div_trunc, d, two);
	return DatumGetNumeric(result);
}

static Numeric
numeric_truncated_divide(Numeric n, int64 divisor)
{
	Datum		d = NumericGetDatum(n);
	Datum		divisor_numeric;
	Datum		result;

	divisor_numeric = NumericGetDatum(int64_to_numeric(divisor));
	result = DirectFunctionCall2(numeric_div_trunc, d, divisor_numeric);
	return DatumGetNumeric(result);
}

Datum
pg_size_pretty_numeric(PG_FUNCTION_ARGS)
{
	Numeric		size = PG_GETARG_NUMERIC(0);
	char	   *result = NULL;
	const struct size_pretty_unit *unit;

	for (unit = size_pretty_units; unit->name != NULL; unit++)
	{
		unsigned int shiftby;

		/* 若已无更多单位或已低于该限制，就用此单位 */
		if (unit[1].name == NULL ||
			numeric_is_less(numeric_absolute(size),
							int64_to_numeric(unit->limit)))
		{
			if (unit->round)
				size = numeric_half_rounded(size);

			result = psprintf("%s %s", numeric_to_cstring(size), unit->name);
			break;
		}

		/*
		 * 确定构造除数所用的位数。若下一级单位做半
		 * 舍入，就要比本级与下一级的差少用 1 位；
		 * 或者，若本级做半舍入而下一级不做，
		 * 就额外多移 1 位。
		 *
		 */
		shiftby = (unit[1].unitbits - unit->unitbits - (unit[1].round == true)
				   + (unit->round == true));
		size = numeric_truncated_divide(size, ((int64) 1) << shiftby);
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * 把人类可读的尺寸转换为字节数
 */
Datum
pg_size_bytes(PG_FUNCTION_ARGS)
{
	text	   *arg = PG_GETARG_TEXT_PP(0);
	char	   *str,
			   *strptr,
			   *endptr;
	char		saved_char;
	Numeric		num;
	int64		result;
	bool		have_digits = false;

	str = text_to_cstring(arg);

	/* 跳过前导空白 */
	strptr = str;
	while (isspace((unsigned char) *strptr))
		strptr++;

	/* 检查数字是否有效，并确定其结束位置 */
	endptr = strptr;

	/* (1)：符号 */
	if (*endptr == '-' || *endptr == '+')
		endptr++;

	/* (2)：主要数字串 */
	if (isdigit((unsigned char) *endptr))
	{
		have_digits = true;
		do
			endptr++;
		while (isdigit((unsigned char) *endptr));
	}

	/* (3)：可选的小数点与小数位 */
	if (*endptr == '.')
	{
		endptr++;
		if (isdigit((unsigned char) *endptr))
		{
			have_digits = true;
			do
				endptr++;
			while (isdigit((unsigned char) *endptr));
		}
	}

	/* 若此时没有有效数字则报错 */
	if (!have_digits)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid size: \"%s\"", str)));

	/* (4)：可选的指数 */
	if (*endptr == 'e' || *endptr == 'E')
	{
		long		exponent;
		char	   *cp;

		/*
		 * 注意我们日后可能支持 EB 单位，若 'E' 后面
		 * 跟的不是数字，就整体当单位来解析。
		 */
		exponent = strtol(endptr + 1, &cp, 10);
		(void) exponent;		/* 消除 -Wunused-result 警告 */
		if (cp > endptr + 1)
			endptr = cp;
	}

	/*
	 * 解析数字，保存下一字符，其可能是单位
	 * 串的首字符。
	 */
	saved_char = *endptr;
	*endptr = '\0';

	num = DatumGetNumeric(DirectFunctionCall3(numeric_in,
											  CStringGetDatum(strptr),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(-1)));

	*endptr = saved_char;

	/* 跳过数字与单位之间的空白 */
	strptr = endptr;
	while (isspace((unsigned char) *strptr))
		strptr++;

	/* 处理潜在单位 */
	if (*strptr != '\0')
	{
		const struct size_pretty_unit *unit;
		int64		multiplier = 0;

		/* 去除尾随空白 */
		endptr = str + VARSIZE_ANY_EXHDR(arg) - 1;

		while (isspace((unsigned char) *endptr))
			endptr--;

		endptr++;
		*endptr = '\0';

		for (unit = size_pretty_units; unit->name != NULL; unit++)
		{
			/* 解析单位，不分大小写 */
			if (pg_strcasecmp(strptr, unit->name) == 0)
				break;
		}

		/* 若未找到，则查找别名表 */
		if (unit->name == NULL)
		{
			for (const struct size_bytes_unit_alias *a = size_bytes_aliases; a->alias != NULL; a++)
			{
				if (pg_strcasecmp(strptr, a->alias) == 0)
				{
					unit = &size_pretty_units[a->unit_index];
					break;
				}
			}
		}

		/* 核实上面的循环找到了有效单位 */
		if (unit->name == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid size: \"%s\"", text_to_cstring(arg)),
					 errdetail("Invalid size unit: \"%s\".", strptr),
					 errhint("Valid units are \"bytes\", \"B\", \"kB\", \"MB\", \"GB\", \"TB\", and \"PB\".")));

		multiplier = ((int64) 1) << unit->unitbits;

		if (multiplier > 1)
		{
			Numeric		mul_num;

			mul_num = int64_to_numeric(multiplier);

			num = DatumGetNumeric(DirectFunctionCall2(numeric_mul,
													  NumericGetDatum(mul_num),
													  NumericGetDatum(num)));
		}
	}

	result = DatumGetInt64(DirectFunctionCall1(numeric_int8,
											   NumericGetDatum(num)));

	PG_RETURN_INT64(result);
}

/*
 * 获取关系的 filenode
 *
 * 预期会用于形如下面的查询
 *		SELECT pg_relation_filenode(oid) FROM pg_class;
 * 这带来若干抉择。我们仅凭 pg_class 行工作，
 * 而不是真正打开每个关系，这是为了效率。我们
 * 找不到关系时不报错——某些行可能仍然
 * 在查询的 MVCC 快照中可见，即使那些关系已被删除。
 * （注意：我们可以不使用 catcache，但意义不大，
 * 因为关系映射器同样能在"当前时刻"工作。）我们
 * 也不会在关系没有存储时报错。在所有这些情况
 * 下静默返回 NULL 更好。
 */
Datum
pg_relation_filenode(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	RelFileNumber result;
	HeapTuple	tuple;
	Form_pg_class relform;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	relform = (Form_pg_class) GETSTRUCT(tuple);

	if (RELKIND_HAS_STORAGE(relform->relkind))
	{
		if (relform->relfilenode)
			result = relform->relfilenode;
		else					/* 查询关系映射器 */
			result = RelationMapOidToFilenumber(relid,
												relform->relisshared);
	}
	else
	{
		/* 无存储，返回NULL */
		result = InvalidRelFileNumber;
	}

	ReleaseSysCache(tuple);

	if (!RelFileNumberIsValid(result))
		PG_RETURN_NULL();

	PG_RETURN_OID(result);
}

/*
 * 按 (reltablespace, relfilenumber) 获取关系
 *
 * 预期用途是：当有人想把文件系统上的单个文件
 * 匹配回其所属的表时使用。通过 pg_class 无法
 * 做到，因为 pg_class 中不含共享与钉住表的编
 * 号。
 *
 * 若找不到映射，我们不报错而是返回 NULL。
 *
 * 临时关系不会被检测到，而返回 NULL（缘由
 * 参见 RelidByRelfilenumber()）。
 *
 * 也可以传入 InvalidOid 来代替当前数据库的默认表
 * 空间。
 */
Datum
pg_filenode_relation(PG_FUNCTION_ARGS)
{
	Oid			reltablespace = PG_GETARG_OID(0);
	RelFileNumber relfilenumber = PG_GETARG_OID(1);
	Oid			heaprel;

	/* 需此测试以防 RelidByRelfilenumber 行为异常 */
	if (!RelFileNumberIsValid(relfilenumber))
		PG_RETURN_NULL();

	heaprel = RelidByRelfilenumber(reltablespace, relfilenumber);

	if (!OidIsValid(heaprel))
		PG_RETURN_NULL();
	else
		PG_RETURN_OID(heaprel);
}

/*
 * 获取关系的路径名（相对于 $PGDATA）
 *
 * 注释见 pg_relation_filenode。
 */
Datum
pg_relation_filepath(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_class relform;
	RelFileLocator rlocator;
	BackendId	backend;
	char	   *path;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		PG_RETURN_NULL();
	relform = (Form_pg_class) GETSTRUCT(tuple);

	if (RELKIND_HAS_STORAGE(relform->relkind))
	{
		/* 此逻辑须与 RelationInitPhysicalAddr 一致 */
		if (relform->reltablespace)
			rlocator.spcOid = relform->reltablespace;
		else
			rlocator.spcOid = MyDatabaseTableSpace;
		if (rlocator.spcOid == GLOBALTABLESPACE_OID)
			rlocator.dbOid = InvalidOid;
		else
			rlocator.dbOid = MyDatabaseId;
		if (relform->relfilenode)
			rlocator.relNumber = relform->relfilenode;
		else					/* 查询关系映射器 */
			rlocator.relNumber = RelationMapOidToFilenumber(relid,
															relform->relisshared);
	}
	else
	{
		/* 无存储，返回NULL */
		rlocator.relNumber = InvalidRelFileNumber;
		/* 若没有下面这两行，某些编译器会产生警告 */
		rlocator.dbOid = InvalidOid;
		rlocator.spcOid = InvalidOid;
	}

	if (!RelFileNumberIsValid(rlocator.relNumber))
	{
		ReleaseSysCache(tuple);
		PG_RETURN_NULL();
	}

	/* 确定所属的后端。 */
	switch (relform->relpersistence)
	{
		case RELPERSISTENCE_UNLOGGED:
		case RELPERSISTENCE_PERMANENT:
			backend = InvalidBackendId;
			break;
		case RELPERSISTENCE_TEMP:
			if (isTempOrTempToastNamespace(relform->relnamespace))
				backend = BackendIdForTempRelations();
			else
			{
				/* 用笨办法。 */
				backend = GetTempNamespaceBackendId(relform->relnamespace);
				Assert(backend != InvalidBackendId);
			}
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c", relform->relpersistence);
			backend = InvalidBackendId; /* 安抚编译器 */
			break;
	}

	ReleaseSysCache(tuple);

	path = relpathbackend(rlocator, backend, MAIN_FORKNUM);

	PG_RETURN_TEXT_P(cstring_to_text(path));
}
