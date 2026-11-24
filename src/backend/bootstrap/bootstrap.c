/*-------------------------------------------------------------------------
 *
 * bootstrap.c
 *	支持以引导模式运行 postgres 的例程。
 *	引导模式用来创建最初的模板数据库。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/bootstrap/bootstrap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "bootstrap/bootstrap.h"
#include "catalog/index.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/link-canary.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "pg_getopt.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relmapper.h"

uint32		bootstrap_data_checksum_version = 0;	/* No checksum */


static void CheckerModeMain(void);
static void bootstrap_signals(void);
static Form_pg_attribute AllocateAttribute(void);
static void populate_typ_list(void);
static Oid	gettype(char *type);
static void cleanup(void);

/* ----------------
 *	全局变量
 * ----------------
 */

Relation	boot_reldesc;		/* 当前关系描述符 */

Form_pg_attribute attrtypes[MAXATTR];	/* 指向属性信息 */
int			numattr;			/* 当前关系的属性个数 */


/*
 * 每个类型的基本信息。pg_type 填好前就用，
 * 故须覆盖核心已引导目录里、用作列类型的
 * 那些数据类型。
 *
 *	XXX 其中若干输入/输出函数会扫描目录
 *	 (例如 F_REGPROCIN 扫描 pg_proc)。这显然会造成一些
 *	目录创建过程中的顺序依赖。
 */
struct typinfo
{
	char		name[NAMEDATALEN];
	Oid			oid;
	Oid			elem;
	int16		len;
	bool		byval;
	char		align;
	char		storage;
	Oid			collation;
	Oid			inproc;
	Oid			outproc;
};

static const struct typinfo TypInfo[] = {
	{"bool", BOOLOID, 0, 1, true, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_BOOLIN, F_BOOLOUT},
	{"bytea", BYTEAOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_BYTEAIN, F_BYTEAOUT},
	{"char", CHAROID, 0, 1, true, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, InvalidOid,
	F_CHARIN, F_CHAROUT},
	{"int2", INT2OID, 0, 2, true, TYPALIGN_SHORT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT2IN, F_INT2OUT},
	{"int4", INT4OID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT4IN, F_INT4OUT},
	{"float4", FLOAT4OID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_FLOAT4IN, F_FLOAT4OUT},
	{"name", NAMEOID, CHAROID, NAMEDATALEN, false, TYPALIGN_CHAR, TYPSTORAGE_PLAIN, C_COLLATION_OID,
	F_NAMEIN, F_NAMEOUT},
	{"regclass", REGCLASSOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGCLASSIN, F_REGCLASSOUT},
	{"regproc", REGPROCOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGPROCIN, F_REGPROCOUT},
	{"regtype", REGTYPEOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGTYPEIN, F_REGTYPEOUT},
	{"regrole", REGROLEOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGROLEIN, F_REGROLEOUT},
	{"regnamespace", REGNAMESPACEOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_REGNAMESPACEIN, F_REGNAMESPACEOUT},
	{"text", TEXTOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_TEXTIN, F_TEXTOUT},
	{"oid", OIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_OIDIN, F_OIDOUT},
	{"tid", TIDOID, 0, 6, false, TYPALIGN_SHORT, TYPSTORAGE_PLAIN, InvalidOid,
	F_TIDIN, F_TIDOUT},
	{"xid", XIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_XIDIN, F_XIDOUT},
	{"cid", CIDOID, 0, 4, true, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_CIDIN, F_CIDOUT},
	{"pg_node_tree", PG_NODE_TREEOID, 0, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_PG_NODE_TREE_IN, F_PG_NODE_TREE_OUT},
	{"int2vector", INT2VECTOROID, INT2OID, -1, false, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_INT2VECTORIN, F_INT2VECTOROUT},
	{"oidvector", OIDVECTOROID, OIDOID, -1, false, TYPALIGN_INT, TYPSTORAGE_PLAIN, InvalidOid,
	F_OIDVECTORIN, F_OIDVECTOROUT},
	{"_int4", INT4ARRAYOID, INT4OID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_text", 1009, TEXTOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, DEFAULT_COLLATION_OID,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_oid", 1028, OIDOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_char", 1002, CHAROID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT},
	{"_aclitem", 1034, ACLITEMOID, -1, false, TYPALIGN_INT, TYPSTORAGE_EXTENDED, InvalidOid,
	F_ARRAY_IN, F_ARRAY_OUT}
};

static const int n_types = sizeof(TypInfo) / sizeof(struct typinfo);

struct typmap
{								/* 变通 */
	Oid			am_oid;
	FormData_pg_type am_typ;
};

static List *Typ = NIL;			/* typmap 结构指针表 */
static struct typmap *Ap = NULL;

static Datum values[MAXATTR];	/* 当前行的各属性值 */
static bool Nulls[MAXATTR];

static MemoryContext nogc = NULL;	/* special no-gc mem context */

/*
 *	引导时我们先声明全部待建索引，再
 *	构建它们。IndexList 结构存有足够信息，
 *	让我们能在声明之后构建这些索引。
 */

typedef struct _IndexList
{
	Oid			il_heap;
	Oid			il_ind;
	IndexInfo  *il_info;
	struct _IndexList *il_next;
} IndexList;

static IndexList *ILHead = NULL;


/*
 * 在共享内存检查模式下，我们真正要做的只是创建共享
 * 内存与信号量（只为证明用当前 GUC
 *  设置就能做到）。其实那已经由
 *  CreateSharedMemoryAndSemaphores() 做过，此处无更多事。
 */
static void
CheckerModeMain(void)
{
	proc_exit(0);
}

/*
 *	以引导模式运行 backend 的主入口。
 *
 *	引导模式用来初始化模板数据库。
 *	引导 backend 不讲 SQL，而是期待
 *	一种特殊引导语言的命令。
 *
 *	当 check_only 为真时，启动只进行到足以验证
 *	当前配置，特别是传入的、关于
 *	共享内存尺寸的选项能用（至少到共享内存
 *	创建都不出错）。
 */
void
BootstrapModeMain(int argc, char *argv[], bool check_only)
{
	int			i;
	char	   *progname = argv[0];
	int			flag;
	char	   *userDoption = NULL;

	Assert(!IsUnderPostmaster);

	InitStandaloneProcess(argv[0]);

	/* 先设默认值，再由下面的显式选项覆盖 */
	InitializeGUCOptions();

	/* 开头应当有 --boot 或 --check */
	Assert(argc > 1
		   && (strcmp(argv[1], "--boot") == 0
			   || strcmp(argv[1], "--check") == 0));
	argv++;
	argc--;

	while ((flag = getopt(argc, argv, "B:c:d:D:Fkr:X:-:")) != -1)
	{
		switch (flag)
		{
			case 'B':
				SetConfigOption("shared_buffers", optarg, PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'c':
			case '-':
				{
					char	   *name,
							   *value;

					ParseLongOption(optarg, &name, &value);
					if (!value)
					{
						if (flag == '-')
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("--%s requires a value",
											optarg)));
						else
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("-c %s requires a value",
											optarg)));
					}

					SetConfigOption(name, value, PGC_POSTMASTER, PGC_S_ARGV);
					pfree(name);
					pfree(value);
					break;
				}
			case 'D':
				userDoption = pstrdup(optarg);
				break;
			case 'd':
				{
					/* 打开引导过程的调试 */
					char	   *debugstr;

					debugstr = psprintf("debug%s", optarg);
					SetConfigOption("log_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					SetConfigOption("client_min_messages", debugstr,
									PGC_POSTMASTER, PGC_S_ARGV);
					pfree(debugstr);
				}
				break;
			case 'F':
				SetConfigOption("fsync", "false", PGC_POSTMASTER, PGC_S_ARGV);
				break;
			case 'k':
				bootstrap_data_checksum_version = PG_DATA_CHECKSUM_VERSION;
				break;
			case 'r':
				strlcpy(OutputFileName, optarg, MAXPGPATH);
				break;
			case 'X':
				{
					int			WalSegSz = strtoul(optarg, NULL, 0);

					if (!IsValidWalSegSize(WalSegSz))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("-X requires a power of two value between 1 MB and 1 GB")));
					SetConfigOption("wal_segment_size", optarg, PGC_INTERNAL,
									PGC_S_DYNAMIC_DEFAULT);
				}
				break;
			default:
				write_stderr("Try \"%s --help\" for more information.\n",
							 progname);
				proc_exit(1);
				break;
		}
	}

	if (argc != optind)
	{
		write_stderr("%s: invalid command-line arguments\n", progname);
		proc_exit(1);
	}

	/* 取得配置参数 */
	if (!SelectConfigFiles(userDoption, progname))
		proc_exit(1);

	/*
	 * 校验所给 DataDir 看起来合理，并切入其中。
	 *
	 */
	checkDataDir();
	ChangeToDataDir();

	CreateDataDirLockFile(false);

	SetProcessingMode(BootstrapProcessing);
	IgnoreSystemIndexes = true;

	InitializeMaxBackends();

	CreateSharedMemoryAndSemaphores();

	/*
	 * XXX：将来把这段挪进独立函数或许有道理，
	 * 但眼下看来会造成更多代码重复，并不
	 * 值得。
	 */
	if (check_only)
	{
		SetProcessingMode(NormalProcessing);
		CheckerModeMain();
		abort();
	}

	/*
	 * 为引导模式做类似 backend 的初始化
	 */
	InitProcess();

	BaseInit();

	bootstrap_signals();
	BootStrapXLOG();

	/*
	 * 为确保 src/common/link-canary.c 被链进 backend，我们
	 * 必须在某处调用它。这里就很合适。
	 */
	if (pg_link_canary_is_frontend())
		elog(ERROR, "backend is incorrectly linked to frontend functions");

	InitPostgres(NULL, InvalidOid, NULL, InvalidOid, false, false, NULL);

	/* 初始化引导文件处理所需的东西 */
	for (i = 0; i < MAXATTR; i++)
	{
		attrtypes[i] = NULL;
		Nulls[i] = false;
	}

	/*
	 * 处理引导输入。
	 */
	StartTransactionCommand();
	boot_yyparse();
	CommitTransactionCommand();

	/*
	 * 现在应当已经知道全部已映射关系，故可以写出
	 * 最初的关系映射文件。
	 */
	RelationMapFinishBootstrap();

	/* 清理并退出 */
	cleanup();
	proc_exit(0);
}


/* ----------------------------------------------------------------
 *	杂项函数
 * ----------------------------------------------------------------
 */

/*
 * 为引导进程建立信号处理
 */
static void
bootstrap_signals(void)
{
	Assert(!IsUnderPostmaster);

	/*
	 * 引导模式下其实不需要非默认信号处理；
	 * 这些情况退出就够了。
	 * 我们明确设置该处理，至少当作文档。
	 */
	pqsignal(SIGHUP, SIG_DFL);
	pqsignal(SIGINT, SIG_DFL);
	pqsignal(SIGTERM, SIG_DFL);
	pqsignal(SIGQUIT, SIG_DFL);
}

/* ----------------------------------------------------------------
 *	手工 backend 交互接口命令
 * ----------------------------------------------------------------
 */

/* ----------------
 *		boot_openrel
 *
 * 执行 BKI OPEN 命令。
 * ----------------
 */
void
boot_openrel(char *relname)
{
	int			i;

	if (strlen(relname) >= NAMEDATALEN)
		relname[NAMEDATALEN - 1] = '\0';

	/*
	 * 任何 OPEN 前必须已填好 pg_type，故我们
	 * 若尚未有，现在就填充 Typ
	 */
	if (Typ == NIL)
		populate_typ_list();

	if (boot_reldesc != NULL)
		closerel(NULL);

	elog(DEBUG4, "open relation %s, attrsize %d",
		 relname, (int) ATTRIBUTE_FIXED_PART_SIZE);

	boot_reldesc = table_openrv(makeRangeVar(NULL, relname, -1), NoLock);
	numattr = RelationGetNumberOfAttributes(boot_reldesc);
	for (i = 0; i < numattr; i++)
	{
		if (attrtypes[i] == NULL)
			attrtypes[i] = AllocateAttribute();
		memmove((char *) attrtypes[i],
				(char *) TupleDescAttr(boot_reldesc->rd_att, i),
				ATTRIBUTE_FIXED_PART_SIZE);

		{
			Form_pg_attribute at = attrtypes[i];

			elog(DEBUG4, "create attribute %d name %s len %d num %d type %u",
				 i, NameStr(at->attname), at->attlen, at->attnum,
				 at->atttypid);
		}
	}
}

/* ----------------
 *		closerel
 * ----------------
 */
void
closerel(char *relname)
{
	if (relname)
	{
		if (boot_reldesc)
		{
			if (strcmp(RelationGetRelationName(boot_reldesc), relname) != 0)
				elog(ERROR, "close of %s when %s was expected",
					 relname, RelationGetRelationName(boot_reldesc));
		}
		else
			elog(ERROR, "close of %s before any relation was opened",
				 relname);
	}

	if (boot_reldesc == NULL)
		elog(ERROR, "no open relation to close");
	else
	{
		elog(DEBUG4, "close relation %s",
			 RelationGetRelationName(boot_reldesc));
		table_close(boot_reldesc, NoLock);
		boot_reldesc = NULL;
	}
}



/* ----------------
 * DEFINEATTR()
 *
 * 定义 <field,type> 对。
 * 若待创建的关系有 n 个字段，本例程
 * 将被调用 n 次
 * ----------------
 */
void
DefineAttr(char *name, char *type, int attnum, int nullness)
{
	Oid			typeoid;

	if (boot_reldesc != NULL)
	{
		elog(WARNING, "no open relations allowed with CREATE command");
		closerel(NULL);
	}

	if (attrtypes[attnum] == NULL)
		attrtypes[attnum] = AllocateAttribute();
	MemSet(attrtypes[attnum], 0, ATTRIBUTE_FIXED_PART_SIZE);

	namestrcpy(&attrtypes[attnum]->attname, name);
	elog(DEBUG4, "column %s %s", NameStr(attrtypes[attnum]->attname), type);
	attrtypes[attnum]->attnum = attnum + 1;

	typeoid = gettype(type);

	if (Typ != NIL)
	{
		attrtypes[attnum]->atttypid = Ap->am_oid;
		attrtypes[attnum]->attlen = Ap->am_typ.typlen;
		attrtypes[attnum]->attbyval = Ap->am_typ.typbyval;
		attrtypes[attnum]->attalign = Ap->am_typ.typalign;
		attrtypes[attnum]->attstorage = Ap->am_typ.typstorage;
		attrtypes[attnum]->attcompression = InvalidCompressionMethod;
		attrtypes[attnum]->attcollation = Ap->am_typ.typcollation;
		/* 若是数组类型，就假定为一维属性 */
		if (Ap->am_typ.typelem != InvalidOid && Ap->am_typ.typlen < 0)
			attrtypes[attnum]->attndims = 1;
		else
			attrtypes[attnum]->attndims = 0;
	}
	else
	{
		attrtypes[attnum]->atttypid = TypInfo[typeoid].oid;
		attrtypes[attnum]->attlen = TypInfo[typeoid].len;
		attrtypes[attnum]->attbyval = TypInfo[typeoid].byval;
		attrtypes[attnum]->attalign = TypInfo[typeoid].align;
		attrtypes[attnum]->attstorage = TypInfo[typeoid].storage;
		attrtypes[attnum]->attcompression = InvalidCompressionMethod;
		attrtypes[attnum]->attcollation = TypInfo[typeoid].collation;
		/* 若是数组类型，就假定为一维属性 */
		if (TypInfo[typeoid].elem != InvalidOid &&
			attrtypes[attnum]->attlen < 0)
			attrtypes[attnum]->attndims = 1;
		else
			attrtypes[attnum]->attndims = 0;
	}

	/*
	 * 若系统目录列会感知排序规则，就强制它使用 C
	 *  排序规则，使其行为不依赖该数据库的
	 * 排序规则。必须能够用不同的数据库排序规则
	 * 来克隆 template0。
	 */
	if (OidIsValid(attrtypes[attnum]->attcollation))
		attrtypes[attnum]->attcollation = C_COLLATION_OID;

	attrtypes[attnum]->attstattarget = -1;
	attrtypes[attnum]->attcacheoff = -1;
	attrtypes[attnum]->atttypmod = -1;
	attrtypes[attnum]->attislocal = true;

	if (nullness == BOOTCOL_NULL_FORCE_NOT_NULL)
	{
		attrtypes[attnum]->attnotnull = true;
	}
	else if (nullness == BOOTCOL_NULL_FORCE_NULL)
	{
		attrtypes[attnum]->attnotnull = false;
	}
	else
	{
		Assert(nullness == BOOTCOL_NULL_AUTO);

		/*
		 * 若类型是定宽，且先前各列同样
		 * 是定宽且非空，则标成非空。这对应于
		 * 该列可经 C 结构体声明直接访问的情形。
		 */
		if (attrtypes[attnum]->attlen > 0)
		{
			int			i;

			/* 检查更早的属性 */
			for (i = 0; i < attnum; i++)
			{
				if (attrtypes[i]->attlen <= 0 ||
					!attrtypes[i]->attnotnull)
					break;
			}
			if (i == attnum)
				attrtypes[attnum]->attnotnull = true;
		}
	}
}


/* ----------------
 *		InsertOneTuple
 *
 * 若 objectid 不是零，它就是要赋给该元组的特定 OID。
 * 否则 OID 将由 heap_insert 分配（若需要）。
 * ----------------
 */
void
InsertOneTuple(void)
{
	HeapTuple	tuple;
	TupleDesc	tupDesc;
	int			i;

	elog(DEBUG4, "inserting row with %d columns", numattr);

	tupDesc = CreateTupleDesc(numattr, attrtypes);
	tuple = heap_form_tuple(tupDesc, values, Nulls);
	pfree(tupDesc);				/* 只释放 tupDesc，而非 attrtypes */

	simple_heap_insert(boot_reldesc, tuple);
	heap_freetuple(tuple);
	elog(DEBUG4, "row inserted");

	/*
	 * 为下一元组重置空标记
	 */
	for (i = 0; i < numattr; i++)
		Nulls[i] = false;
}

/* ----------------
 *		InsertOneValue
 * ----------------
 */
void
InsertOneValue(char *value, int i)
{
	Oid			typoid;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			typinput;
	Oid			typoutput;

	Assert(i >= 0 && i < MAXATTR);

	elog(DEBUG4, "inserting column %d value \"%s\"", i, value);

	typoid = TupleDescAttr(boot_reldesc->rd_att, i)->atttypid;

	boot_get_type_io_data(typoid,
						  &typlen, &typbyval, &typalign,
						  &typdelim, &typioparam,
						  &typinput, &typoutput);

	values[i] = OidInputFunctionCall(typinput, value, typioparam, -1);

	/*
	 * 这里用 ereport 而不用 elog，以便除非
	 * 消息将打印才求值参数，而通常不会。
	 */
	ereport(DEBUG4,
			(errmsg_internal("inserted -> %s",
							 OidOutputFunctionCall(typoutput, values[i]))));
}

/* ----------------
 *		InsertOneNull
 * ----------------
 */
void
InsertOneNull(int i)
{
	elog(DEBUG4, "inserting column %d NULL", i);
	Assert(i >= 0 && i < MAXATTR);
	if (TupleDescAttr(boot_reldesc->rd_att, i)->attnotnull)
		elog(ERROR,
			 "NULL value specified for not-null column \"%s\" of relation \"%s\"",
			 NameStr(TupleDescAttr(boot_reldesc->rd_att, i)->attname),
			 RelationGetRelationName(boot_reldesc));
	values[i] = PointerGetDatum(NULL);
	Nulls[i] = true;
}

/* ----------------
 *		cleanup
 * ----------------
 */
static void
cleanup(void)
{
	if (boot_reldesc != NULL)
		closerel(NULL);
}

/* ----------------
 *		populate_typ_list
 *
 * 读取 pg_type，装入 Typ 表。
 * ----------------
 */
static void
populate_typ_list(void)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	MemoryContext old;

	Assert(Typ == NIL);

	rel = table_open(TypeRelationId, NoLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	old = MemoryContextSwitchTo(TopMemoryContext);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_type typForm = (Form_pg_type) GETSTRUCT(tup);
		struct typmap *newtyp;

		newtyp = (struct typmap *) palloc(sizeof(struct typmap));
		Typ = lappend(Typ, newtyp);

		newtyp->am_oid = typForm->oid;
		memcpy(&newtyp->am_typ, typForm, sizeof(newtyp->am_typ));
	}
	MemoryContextSwitchTo(old);
	table_endscan(scan);
	table_close(rel, NoLock);
}

/* ----------------
 *		gettype
 *
 * 注意：这确实很丑；它返回 TypInfo[] 的整数下标，
 * 根本不是 OID，直到首次引用 TypInfo[] 里未知的
 * 类型。届时它读入并缓存 pg_type 到 Typ，
 * 此后才返回真正的 OID（并把全局指针 Ap 设成
 * 指向 Typ 中找到的行）。故调用者必须检查 Typ
 *  是否仍为 NIL，以判断返回的是什么
 * ----------------
 */
static Oid
gettype(char *type)
{
	if (Typ != NIL)
	{
		ListCell   *lc;

		foreach(lc, Typ)
		{
			struct typmap *app = lfirst(lc);

			if (strncmp(NameStr(app->am_typ.typname), type, NAMEDATALEN) == 0)
			{
				Ap = app;
				return app->am_oid;
			}
		}

		/*
		 * 该类型原先未知；重新装入 pg_type 的内容再检查，
		 * 以处理自上次装入该表后新增的复合类型。
		 */

		list_free_deep(Typ);
		Typ = NIL;
		populate_typ_list();

		/*
		 * 调用 gettype 对 pg_type 所缺类型会无限递归，
		 * 所以只是把查找再做一遍。
		 */
		foreach(lc, Typ)
		{
			struct typmap *app = lfirst(lc);

			if (strncmp(NameStr(app->am_typ.typname), type, NAMEDATALEN) == 0)
			{
				Ap = app;
				return app->am_oid;
			}
		}
	}
	else
	{
		int			i;

		for (i = 0; i < n_types; i++)
		{
			if (strncmp(type, TypInfo[i].name, NAMEDATALEN) == 0)
				return i;
		}
		/* 不在 TypInfo 里，所以现在最好能读到 pg_type */
		elog(DEBUG4, "external type: %s", type);
		populate_typ_list();
		return gettype(type);
	}
	elog(ERROR, "unrecognized type \"%s\"", type);
	/* 到不了，为让编译器满意 */
	return 0;
}

/* ----------------
 *		boot_get_type_io_data
 *
 * 在引导时取得类型 I/O 信息。这是有意让它
 * 几乎与 lsyscache.c 的 get_type_io_data 同一 API，只是
 * 我们只支持取得 typinput 与 typoutput，不支持
 * 二进制 I/O。导出它，是为了让 array_in 与 array_out
 *  能在早期引导期间工作。
 * ----------------
 */
void
boot_get_type_io_data(Oid typid,
					  int16 *typlen,
					  bool *typbyval,
					  char *typalign,
					  char *typdelim,
					  Oid *typioparam,
					  Oid *typinput,
					  Oid *typoutput)
{
	if (Typ != NIL)
	{
		/* 已有 pg_type 的引导期内容，故用它 */
		struct typmap *ap = NULL;
		ListCell   *lc;

		foreach(lc, Typ)
		{
			ap = lfirst(lc);
			if (ap->am_oid == typid)
				break;
		}

		if (!ap || ap->am_oid != typid)
			elog(ERROR, "type OID %u not found in Typ list", typid);

		*typlen = ap->am_typ.typlen;
		*typbyval = ap->am_typ.typbyval;
		*typalign = ap->am_typ.typalign;
		*typdelim = ap->am_typ.typdelim;

		/* XXX 此逻辑必须匹配 getTypeIOParam() */
		if (OidIsValid(ap->am_typ.typelem))
			*typioparam = ap->am_typ.typelem;
		else
			*typioparam = typid;

		*typinput = ap->am_typ.typinput;
		*typoutput = ap->am_typ.typoutput;
	}
	else
	{
		/* 还没有 pg_type，故用写死的 TypInfo 数组 */
		int			typeindex;

		for (typeindex = 0; typeindex < n_types; typeindex++)
		{
			if (TypInfo[typeindex].oid == typid)
				break;
		}
		if (typeindex >= n_types)
			elog(ERROR, "type OID %u not found in TypInfo", typid);

		*typlen = TypInfo[typeindex].len;
		*typbyval = TypInfo[typeindex].byval;
		*typalign = TypInfo[typeindex].align;
		/* 假定所有引导期类型的 typdelim 是 ',' */
		*typdelim = ',';

		/* XXX 此逻辑必须匹配 getTypeIOParam() */
		if (OidIsValid(TypInfo[typeindex].elem))
			*typioparam = TypInfo[typeindex].elem;
		else
			*typioparam = typid;

		*typinput = TypInfo[typeindex].inproc;
		*typoutput = TypInfo[typeindex].outproc;
	}
}

/* ----------------
 *		AllocateAttribute
 *
 * 注意：引导从不设置按列 ACL，故每个属性只需
 *  ATTRIBUTE_FIXED_PART_SIZE 的空间。
 * ----------------
 */
static Form_pg_attribute
AllocateAttribute(void)
{
	return (Form_pg_attribute)
		MemoryContextAllocZero(TopMemoryContext, ATTRIBUTE_FIXED_PART_SIZE);
}

/*
 *	index_register()：记下已设好、稍后构建的索引。
 *
 *
 *	引导时，我们在系统目录上定义一批索引。
 *	但真正构建要推迟到初始化快要
 *	完成之前。因为这些索引
 *	自身也有目录项，而那些项必须收进
 *	这些目录上的索引里。分两阶段是最简单的
 *	办法，才能确保索引最终内容正确。
 */
void
index_register(Oid heap,
			   Oid ind,
			   IndexInfo *indexInfo)
{
	IndexList  *newind;
	MemoryContext oldcxt;

	/*
	 * XXX mao 10/31/92：引导时不要回收索引 reldesc
	 *  及相关信息。现在就声明索引，但稍后再创建。
	 *
	 */

	if (nogc == NULL)
		nogc = AllocSetContextCreate(NULL,
									 "BootstrapNoGC",
									 ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(nogc);

	newind = (IndexList *) palloc(sizeof(IndexList));
	newind->il_heap = heap;
	newind->il_ind = ind;
	newind->il_info = (IndexInfo *) palloc(sizeof(IndexInfo));

	memcpy(newind->il_info, indexInfo, sizeof(IndexInfo));
	/* 表达式多半是 null，但仍不妨复制它 */
	newind->il_info->ii_Expressions =
		copyObject(indexInfo->ii_Expressions);
	newind->il_info->ii_ExpressionsState = NIL;
	/* 谓词多半是 null，但仍不妨复制它 */
	newind->il_info->ii_Predicate =
		copyObject(indexInfo->ii_Predicate);
	newind->il_info->ii_PredicateState = NULL;
	/* 引导时没有排除约束，故不必复制 */
	Assert(indexInfo->ii_ExclusionOps == NULL);
	Assert(indexInfo->ii_ExclusionProcs == NULL);
	Assert(indexInfo->ii_ExclusionStrats == NULL);

	newind->il_next = ILHead;
	ILHead = newind;

	MemoryContextSwitchTo(oldcxt);
}


/*
 * build_indices：填好先前登记的全部索引
 */
void
build_indices(void)
{
	for (; ILHead != NULL; ILHead = ILHead->il_next)
	{
		Relation	heap;
		Relation	ind;

		/* 引导期间不必费心管锁 */
		heap = table_open(ILHead->il_heap, NoLock);
		ind = index_open(ILHead->il_ind, NoLock);

		index_build(heap, ind, ILHead->il_info, false, false);

		index_close(ind, NoLock);
		table_close(heap, NoLock);
	}
}
