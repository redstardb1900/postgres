/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  动态函数管理器代码
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/dfmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#ifndef WIN32
#include <dlfcn.h>

/*
 * 在 macOS 上，<dlfcn.h> 强制包含 <stdbool.h>。如果使用
 * stdbool，undef bool 以消除其影响。
 */
#ifndef PG_USE_STDBOOL
#ifdef bool
#undef bool
#endif
#endif
#endif							/* !WIN32 */

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"


/* signature for PostgreSQL-specific library init function */
typedef void (*PG_init_t) (void);

/* 汇聚变量的哈希表条目 */
typedef struct
{
	char		varName[NAMEDATALEN];	/* hash key (must be first) */
	void	   *varValue;
} rendezvousHashEntry;

/*
 * 已动态加载文件的列表（保存在 malloc 内存中）
 */

typedef struct df_files
{
	struct df_files *next;		/* List link */
	dev_t		device;			/* Device file is on */
#ifndef WIN32					/* ensures we never again depend on this under
								 * win32
	ino_t		inode;			/* Inode number of file */
#endif
	void	   *handle;			/* a handle for pg_dl* functions */
	char		filename[FLEXIBLE_ARRAY_MEMBER];	/* Full pathname of file */
} DynamicFileList;

static DynamicFileList *file_list = NULL;
static DynamicFileList *file_tail = NULL;

/* Win32 下的 stat() 调用返回 st_ino 字段，但它没有 */
#ifndef WIN32
#define SAME_INODE(A,B) ((A).st_ino == (B).inode && (A).st_dev == (B).device)
#else
#define SAME_INODE(A,B) false
#endif

char	   *Dynamic_library_path;

static void *internal_load_library(const char *libname);
static void incompatible_module_error(const char *libname,
									  const Pg_magic_struct *module_magic_data) pg_attribute_noreturn();
static bool file_exists(const char *name);
static char *expand_dynamic_library_name(const char *name);
static void check_restricted_library_name(const char *name);
static char *substitute_libpath_macro(const char *name);
static char *find_in_dynamic_libpath(const char *basename);

/* 模块需匹配的魔数结构才能被接受加载 */
static const Pg_magic_struct magic_data = PG_MODULE_MAGIC_DATA;


/*
 * 加载指定的动态链接库文件，并在其中查找名为 funcn
 * 的函数。
 *
 * 如果未找到该函数，signalNotFound 为真则报错，否则返回
 * NULL。注意加载库时的错误不管 signalNotFound 
 * 触发 ereport()。
 *
 * 如果 filehandle 非 NULL，则 *filehandle 会被设置为标识
 * 库文件的句柄。该句柄可传给 internal_load_library 
 * lookup_external_function 来查找其他函数，开销低于重复调
 * 用 load_external_function。
 */
void *
load_external_function(const char *filename, const char *funcname,
					   bool signalNotFound, void **filehandle)
{
	char	   *fullname;
	void	   *lib_handle;
	void	   *retval;

	/* 将可能缩写的文件名展开为精确路径 */
	fullname = expand_dynamic_library_name(filename);

	/* 加载共享库（如果尚未加载） */
	lib_handle = internal_load_library(fullname);

	/* 调用者需要则返回句柄 */
	if (filehandle)
		*filehandle = lib_handle;

	/* 在库中查找该函数。 */
	retval = dlsym(lib_handle, funcname);

	if (retval == NULL && signalNotFound)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not find function \"%s\" in file \"%s\"",
						funcname, fullname)));

	pfree(fullname);
	return retval;
}

/*
 * 本函数加载一个 shlib 文件而不查找其中的任何具
 * 函数。如果同一 shlib 此前已加载，则不会卸载
 * 新加载它。
 *
 * 当 restricted 为真时，只能引用假定可信目录
 * $libdir/plugins 中的库。
 */
void
load_file(const char *filename, bool restricted)
{
	char	   *fullname;

	/* 如果需要则施加安全限制 */
	if (restricted)
		check_restricted_library_name(filename);

	/* 将可能缩写的文件名展开为精确路径 */
	fullname = expand_dynamic_library_name(filename);

	/* 加载共享库 */
	(void) internal_load_library(fullname);

	pfree(fullname);
}

/*
 * 查找其库文件已加载的函数。
 * 未找到则返回 NULL
 */
void *
lookup_external_function(void *filehandle, const char *funcname)
{
	return dlsym(filehandle, funcname);
}


/*
 * 加载指定的动态链接库文件（除非已加载）。返回
 * pg_dl* 句柄。
 *
 * 注意：libname 应为库文件的精确名称。
 *
 * 注意：目前没有办法卸载动态加载的库。如果我们确信已
 * 的卸载方法，也许有一天可以添加：从钩子函数指针解除
 * 释放自定义 GUC，以及
 * 其他目前明显不安全的操作。
 */
static void *
internal_load_library(const char *libname)
{
	DynamicFileList *file_scanner;
	PGModuleMagicFunction magic_func;
	char	   *load_error;
	struct stat stat_buf;
	PG_init_t	PG_init;

	/*
	 * 扫描已加载文件的列表，看该文件是否已加载。
	 */
	for (file_scanner = file_list;
		 file_scanner != NULL &&
		 strcmp(libname, file_scanner->filename) != 0;
		 file_scanner = file_scanner->next)
		;

	if (file_scanner == NULL)
	{
		/*
		 * 检查相同文件的不同路径（如符号链接或硬链接）
		 */
		if (stat(libname, &stat_buf) == -1)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not access file \"%s\": %m",
							libname)));

		for (file_scanner = file_list;
			 file_scanner != NULL &&
			 !SAME_INODE(stat_buf, *file_scanner);
			 file_scanner = file_scanner->next)
			;
	}

	if (file_scanner == NULL)
	{
		/*
		 * 文件尚未加载
		 */
		file_scanner = (DynamicFileList *)
			malloc(offsetof(DynamicFileList, filename) + strlen(libname) + 1);
		if (file_scanner == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));

		MemSet(file_scanner, 0, offsetof(DynamicFileList, filename));
		strcpy(file_scanner->filename, libname);
		file_scanner->device = stat_buf.st_dev;
#ifndef WIN32
		file_scanner->inode = stat_buf.st_ino;
#endif
		file_scanner->next = NULL;

		file_scanner->handle = dlopen(file_scanner->filename, RTLD_NOW | RTLD_GLOBAL);
		if (file_scanner->handle == NULL)
		{
			load_error = dlerror();
			free(file_scanner);
			/* errcode_for_file_access 在这里可能不适用？ */
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not load library \"%s\": %s",
							libname, load_error)));
		}

		/* 检查魔数函数以确定兼容性 */
		magic_func = (PGModuleMagicFunction)
			dlsym(file_scanner->handle, PG_MAGIC_FUNCTION_NAME_STRING);
		if (magic_func)
		{
			const Pg_magic_struct *magic_data_ptr = (*magic_func) ();

			if (magic_data_ptr->len != magic_data.len ||
				memcmp(magic_data_ptr, &magic_data, magic_data.len) != 0)
			{
				/* 在解除库链接前复制数据块 */
				Pg_magic_struct module_magic_data = *magic_data_ptr;

				/* 尝试关闭库 */
				dlclose(file_scanner->handle);
				free(file_scanner);

				/* 发出适当的抱怨 */
				incompatible_module_error(libname, &module_magic_data);
			}
		}
		else
		{
			/* 尝试关闭库 */
			dlclose(file_scanner->handle);
			free(file_scanner);
			/* 抱怨 */
			ereport(ERROR,
					(errmsg("incompatible library \"%s\": missing magic block",
							libname),
					 errhint("Extension libraries are required to use the PG_MODULE_MAGIC macro.")));
		}

		/*
		 * 如果库有 _PG_init() 函数，就调用它。
		 */
		PG_init = (PG_init_t) dlsym(file_scanner->handle, "_PG_init");
		if (PG_init)
			(*PG_init) ();

		/* 可以把它链接进列表了 */
		if (file_list == NULL)
			file_list = file_scanner;
		else
			file_tail->next = file_scanner;
		file_tail = file_scanner;
	}

	return file_scanner->handle;
}

/*
 * 为不兼容的魔数块报告适当的错误。
 */
static void
incompatible_module_error(const char *libname,
						  const Pg_magic_struct *module_magic_data)
{
	StringInfoData details;

	/*
	 * 如果版本不匹配，只报告版本不匹配，因为魔数
	 * 块可能连我们期望的字段都没有。
	 */
	if (magic_data.version != module_magic_data->version)
	{
		char		library_version[32];

		if (module_magic_data->version >= 1000)
			snprintf(library_version, sizeof(library_version), "%d",
					 module_magic_data->version / 100);
		else
			snprintf(library_version, sizeof(library_version), "%d.%d",
					 module_magic_data->version / 100,
					 module_magic_data->version % 100);
		ereport(ERROR,
				(errmsg("incompatible library \"%s\": version mismatch",
						libname),
				 errdetail("Server is version %d, library is version %s.",
						   magic_data.version / 100, library_version)));
	}

	/*
	 * 类似地，如果 ABI extra 字段不匹配，则报错，因为
	 * 字段也可能不匹配，但那没有参考价值——你用的是
	 * 完全错误的产品。
	 */
	if (strcmp(module_magic_data->abi_extra, magic_data.abi_extra) != 0)
	{
		ereport(ERROR,
				(errmsg("incompatible library \"%s\": ABI mismatch",
						libname),
				 errdetail("Server has ABI \"%s\", library has \"%s\".",
						   magic_data.abi_extra,
						   module_magic_data->abi_extra)));
	}

	/*
	 * 否则，详细说明哪些字段不一致。
	 *
	 * XXX 每当块中字段集变化时都必须调整此代码！
	 * block change!
	 */
	initStringInfo(&details);

	if (module_magic_data->funcmaxargs != magic_data.funcmaxargs)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has FUNC_MAX_ARGS = %d, library has %d."),
						 magic_data.funcmaxargs,
						 module_magic_data->funcmaxargs);
	}
	if (module_magic_data->indexmaxkeys != magic_data.indexmaxkeys)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has INDEX_MAX_KEYS = %d, library has %d."),
						 magic_data.indexmaxkeys,
						 module_magic_data->indexmaxkeys);
	}
	if (module_magic_data->namedatalen != magic_data.namedatalen)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has NAMEDATALEN = %d, library has %d."),
						 magic_data.namedatalen,
						 module_magic_data->namedatalen);
	}
	if (module_magic_data->float8byval != magic_data.float8byval)
	{
		if (details.len)
			appendStringInfoChar(&details, '\n');
		appendStringInfo(&details,
						 _("Server has FLOAT8PASSBYVAL = %s, library has %s."),
						 magic_data.float8byval ? "true" : "false",
						 module_magic_data->float8byval ? "true" : "false");
	}

	if (details.len == 0)
		appendStringInfoString(&details,
							   _("Magic block has unexpected length or padding difference."));

	ereport(ERROR,
			(errmsg("incompatible library \"%s\": magic block mismatch",
					libname),
			 errdetail_internal("%s", details.data)));
}

static bool
file_exists(const char *name)
{
	struct stat st;

	Assert(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}


/*
 * 如果名称含斜杠，检查文件是否存在，存在则返
 * 该名称。否则（无斜杠）尝试用搜索路径展开（
 * 下面的 find_in_dynamic_libpath）；如果成功，返回展
 * 后的文件名。如果都失败了，追加 DLSUFFIX 再试一
 * 如果全部失败，就返回原始名称。
 *
 * 结果总是新分配的内存。
 */
static char *
expand_dynamic_library_name(const char *name)
{
	bool		have_slash;
	char	   *new;
	char	   *full;

	Assert(name);

	have_slash = (first_dir_separator(name) != NULL);

	if (!have_slash)
	{
		full = find_in_dynamic_libpath(name);
		if (full)
			return full;
	}
	else
	{
		full = substitute_libpath_macro(name);
		if (file_exists(full))
			return full;
		pfree(full);
	}

	new = psprintf("%s%s", name, DLSUFFIX);

	if (!have_slash)
	{
		full = find_in_dynamic_libpath(new);
		pfree(new);
		if (full)
			return full;
	}
	else
	{
		full = substitute_libpath_macro(new);
		pfree(new);
		if (file_exists(full))
			return full;
		pfree(full);
	}

	/*
	 * 如果找不到文件，就按原样返回字符串。后续的加
	 * 尝试会失败并给出适当的错误消息。
	 */
	return pstrdup(name);
}

/*
 * 检查受限的库名。必须以 "$libdir/" 开头，之后不得有
 * 目录分隔符（这足以阻止".."风格的攻击）。
 * sufficient to prevent ".." style attacks).
 */
static void
check_restricted_library_name(const char *name)
{
	if (strncmp(name, "$libdir/plugins/", 16) != 0 ||
		first_dir_separator(name + 16) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("access to library \"%s\" is not allowed",
						name)));
}

/*
 * 替换给定字符串中出现的任何宏。
 * 结果总是新分配的内存。
 */
static char *
substitute_libpath_macro(const char *name)
{
	const char *sep_ptr;

	Assert(name != NULL);

	/* 目前只识别字符串开头的 $libdir。 */
	if (name[0] != '$')
		return pstrdup(name);

	if ((sep_ptr = first_dir_separator(name)) == NULL)
		sep_ptr = name + strlen(name);

	if (strlen("$libdir") != sep_ptr - name ||
		strncmp(name, "$libdir", strlen("$libdir")) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("invalid macro name in dynamic library path: %s",
						name)));

	return psprintf("%s%s", pkglib_path, sep_ptr);
}


/*
 * 在冒号分隔的路径 Dynamic_library_path 中搜索名为
 * 'basename' 的文件。如果找到，完整路径
 * 在新分配的内存中返回。如果找不到文件，则
 * 返回 NULL
 */
static char *
find_in_dynamic_libpath(const char *basename)
{
	const char *p;
	size_t		baselen;

	Assert(basename != NULL);
	Assert(first_dir_separator(basename) == NULL);
	Assert(Dynamic_library_path != NULL);

	p = Dynamic_library_path;
	if (strlen(p) == 0)
		return NULL;

	baselen = strlen(basename);

	for (;;)
	{
		size_t		len;
		char	   *piece;
		char	   *mangled;
		char	   *full;

		piece = first_path_var_separator(p);
		if (piece == p)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("zero-length component in parameter \"dynamic_library_path\"")));

		if (piece == NULL)
			len = strlen(p);
		else
			len = piece - p;

		piece = palloc(len + 1);
		strlcpy(piece, p, len + 1);

		mangled = substitute_libpath_macro(piece);
		pfree(piece);

		canonicalize_path(mangled);

		/* 只处理绝对路径 */
		if (!is_absolute_path(mangled))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("component in parameter \"dynamic_library_path\" is not an absolute path")));

		full = palloc(strlen(mangled) + 1 + baselen + 1);
		sprintf(full, "%s/%s", mangled, basename);
		pfree(mangled);

		elog(DEBUG3, "find_in_dynamic_libpath: trying \"%s\"", full);

		if (file_exists(full))
			return full;

		pfree(full);

		if (p[len] == '\0')
			break;
		else
			p += len + 1;
	}

	return NULL;
}


/*
 * 查找（或创建）一个汇聚变量，供某个动态
 * 载库用来与另一个库会合。
 *
 * 首次对特定 varName 调用此函数时，会创建一个
 * "汇聚变量"。
 * 变量的值是一个 void 指针（初始设为 NULL）。
 * 后续以相同 varName 调用只返回现有变量的地址。
 * 汇聚变量一旦创建，其生命周期与进程相同。
 * for the life of the process.
 *
 * 动态加载的库可以用汇聚变量相互查找和共
 * 变量名和它将指向的数据上达成一致即可。
 * on the variable name and the data it will point to.
 */
void	  **
find_rendezvous_variable(const char *varName)
{
	static HTAB *rendezvousHash = NULL;

	rendezvousHashEntry *hentry;
	bool		found;

	/* 如果本次尚未创建哈希表则创建 */
	if (rendezvousHash == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = NAMEDATALEN;
		ctl.entrysize = sizeof(rendezvousHashEntry);
		rendezvousHash = hash_create("Rendezvous variable hash",
									 16,
									 &ctl,
									 HASH_ELEM | HASH_STRINGS);
	}

	/* 查找或创建该 varName 的哈希表条目 */
	hentry = (rendezvousHashEntry *) hash_search(rendezvousHash,
												 varName,
												 HASH_ENTER,
												 &found);

	/* 首次则初始化为 NULL */
	if (!found)
		hentry->varValue = NULL;

	return &hentry->varValue;
}

/*
 * 估算序列化我们已加载的库列表所需的内存量。
 * we have loaded.
 */
Size
EstimateLibraryStateSpace(void)
{
	DynamicFileList *file_scanner;
	Size		size = 1;

	for (file_scanner = file_list;
		 file_scanner != NULL;
		 file_scanner = file_scanner->next)
		size = add_size(size, strlen(file_scanner->filename) + 1);

	return size;
}

/*
 * 将已加载的库列表序列化到一个内存块中。
 */
void
SerializeLibraryState(Size maxsize, char *start_address)
{
	DynamicFileList *file_scanner;

	for (file_scanner = file_list;
		 file_scanner != NULL;
		 file_scanner = file_scanner->next)
	{
		Size		len;

		len = strlcpy(start_address, file_scanner->filename, maxsize) + 1;
		Assert(len < maxsize);
		maxsize -= len;
		start_address += len;
	}
	start_address[0] = '\0';
}

/*
 * 加载序列化 backend 已加载的每一个库。
 */
void
RestoreLibraryState(char *start_address)
{
	while (*start_address != '\0')
	{
		internal_load_library(start_address);
		start_address += strlen(start_address) + 1;
	}
}
