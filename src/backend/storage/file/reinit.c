/*-------------------------------------------------------------------------
 *
 * reinit.c
 *	  未记录关系的重新初始化
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/reinit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "common/relpath.h"
#include "postmaster/startup.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/reinit.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

static void ResetUnloggedRelationsInTablespaceDir(const char *tsdirname,
												  int op);
static void ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname,
											   int op);

typedef struct
{
	Oid			reloid;			/* 散列键 */
} unlogged_relation_entry;

/*
 * 重置上次重启之前的未记录关系。
 *
 * 若 op 包含 UNLOGGED_RELATION_CLEANUP，我们就去掉任何
 * 带 init 分叉的关系的全部分叉，但 init 分叉本身除外。
 *
 * 若 op 包含 UNLOGGED_RELATION_INIT，我们把 init 分叉复制到主
 * 分叉。
 */
void
ResetUnloggedRelations(int op)
{
	char		temp_path[MAXPGPATH + 10 + sizeof(TABLESPACE_VERSION_DIRECTORY)];
	DIR		   *spc_dir;
	struct dirent *spc_de;
	MemoryContext tmpctx,
				oldctx;

	/* 记下来。 */
	elog(DEBUG1, "resetting unlogged relations: cleanup %d init %d",
		 (op & UNLOGGED_RELATION_CLEANUP) != 0,
		 (op & UNLOGGED_RELATION_INIT) != 0);

	/*
	 * 为了确保不泄漏任何内存，我们为这次操作创建一个临时
	 * 内存上下文。
	 */
	tmpctx = AllocSetContextCreate(CurrentMemoryContext,
								   "ResetUnloggedRelations",
								   ALLOCSET_DEFAULT_SIZES);
	oldctx = MemoryContextSwitchTo(tmpctx);

	/* 准备报告重置未记录关系的进度。 */
	begin_startup_progress_phase();

	/*
	 * 首先处理 pg_default（$PGDATA/base）里的未记录文件
	 */
	ResetUnloggedRelationsInTablespaceDir("base", op);

	/*
	 * 遍历所有非默认表空间的目录。
	 */
	spc_dir = AllocateDir("pg_tblspc");

	while ((spc_de = ReadDir(spc_dir, "pg_tblspc")) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "pg_tblspc/%s/%s",
				 spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		ResetUnloggedRelationsInTablespaceDir(temp_path, op);
	}

	FreeDir(spc_dir);

	/*
	 * 恢复内存上下文。
	 */
	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(tmpctx);
}

/*
 * 为 ResetUnloggedRelations 处理一个表空间目录
 */
static void
ResetUnloggedRelationsInTablespaceDir(const char *tsdirname, int op)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	/*
	 * 若在表空间目录上得到 ENOENT，就记日志并返回。这
	 * 可能发生在先前的 DROP TABLESPACE 在去掉表空间目录和
	 * 去掉 pg_tblspc 里的符号链接之间崩溃时。我们并不
	 * 真想在那种情形下阻止数据库启动，因此让它
	 * 通过。任何其他类型的错误会由 ReadDir
	 * 报告（从而导致启动失败）。
	 */
	if (ts_dir == NULL && errno == ENOENT)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						tsdirname)));
		return;
	}

	while ((de = ReadDir(ts_dir, tsdirname)) != NULL)
	{
		/*
		 * 我们只关心每个数据库的目录，它们的
		 * 名字是数字。注意这段代码也会（正确地）忽略 .
		 * 和 ..。
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);

		if (op & UNLOGGED_RELATION_INIT)
			ereport_startup_progress("resetting unlogged relations (init), elapsed time: %ld.%02d s, current path: %s",
									 dbspace_path);
		else if (op & UNLOGGED_RELATION_CLEANUP)
			ereport_startup_progress("resetting unlogged relations (cleanup), elapsed time: %ld.%02d s, current path: %s",
									 dbspace_path);

		ResetUnloggedRelationsInDbspaceDir(dbspace_path, op);
	}

	FreeDir(ts_dir);
}

/*
 * 为 ResetUnloggedRelations 处理一个每数据库空间目录
 */
static void
ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname, int op)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	/* 调用者必须至少指定一种操作。 */
	Assert((op & (UNLOGGED_RELATION_CLEANUP | UNLOGGED_RELATION_INIT)) != 0);

	/*
	 * 清理是两遍操作。第一遍，我们找出所有
	 * 带 init 分叉的文件。然后我们再走一遍，清掉
	 * 除 init 分叉以外、OID 相同的一切。
	 */
	if ((op & UNLOGGED_RELATION_CLEANUP) != 0)
	{
		HTAB	   *hash;
		HASHCTL		ctl;

		/*
		 * 有人可能在同一个数据库和表空间里创建大量未记录关系，
		 * 因此我们最好用散列表
		 * 而不是数组或链表来跟踪哪些文件
		 * 需要重置。否则这次清理操作会是
		 * O(n^2)。
		 */
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(unlogged_relation_entry);
		ctl.hcxt = CurrentMemoryContext;
		hash = hash_create("unlogged relation OIDs", 32, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/* 扫描该目录。 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			relnumchars;
			unlogged_relation_entry ent;

			/* 跳过任何看起来不像关系数据文件的东西。 */
			if (!parse_filename_for_nontemp_relation(de->d_name, &relnumchars,
													 &forkNum))
				continue;

			/* 除非这是 init 分叉，否则也跳过。 */
			if (forkNum != INIT_FORKNUM)
				continue;

			/*
			 * 把名字里的 OID 部分放进散列表，若
			 * 还没有的话。
			 */
			ent.reloid = atooid(de->d_name);
			(void) hash_search(hash, &ent, HASH_ENTER, NULL);
		}

		/* 第一遍完成。 */
		FreeDir(dbspace_dir);

		/*
		 * 若没有找到任何 init 分叉，就没有必要继续；
		 * 现在可以退出。
		 */
		if (hash_get_num_entries(hash) == 0)
		{
			hash_destroy(hash);
			return;
		}

		/*
		 * 现在做第二遍，去掉任何匹配的东西。
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			relnumchars;
			unlogged_relation_entry ent;

			/* 跳过任何看起来不像关系数据文件的东西。 */
			if (!parse_filename_for_nontemp_relation(de->d_name, &relnumchars,
													 &forkNum))
				continue;

			/* 我们从不去掉 init 分叉。 */
			if (forkNum == INIT_FORKNUM)
				continue;

			/*
			 * 看看名字里的 OID 部分是否出现在散列
			 * 表里。若是，就清掉它！
			 */
			ent.reloid = atooid(de->d_name);
			if (hash_search(hash, &ent, HASH_FIND, NULL))
			{
				snprintf(rm_path, sizeof(rm_path), "%s/%s",
						 dbspacedirname, de->d_name);
				if (unlink(rm_path) < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
				else
					elog(DEBUG2, "unlinked file \"%s\"", rm_path);
			}
		}

		/* 清理完成。 */
		FreeDir(dbspace_dir);
		hash_destroy(hash);
	}

	/*
	 * 初始化发生在清理完成之后：我们把每个 init
	 * 分叉文件复制到对应的主分叉文件。注意，若我们被
	 * 要求同时做清理和初始化，我们可能永远到不了这里：若
	 * 清理代码断定这个数据库空间里没有 init 分叉，
	 * 它会在我们到达这里之前返回。
	 */
	if ((op & UNLOGGED_RELATION_INIT) != 0)
	{
		/* 扫描该目录。 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			relnumchars;
			char		relnumbuf[OIDCHARS + 1];
			char		srcpath[MAXPGPATH * 2];
			char		dstpath[MAXPGPATH];

			/* 跳过任何看起来不像关系数据文件的东西。 */
			if (!parse_filename_for_nontemp_relation(de->d_name, &relnumchars,
													 &forkNum))
				continue;

			/* 除非这是 init 分叉，否则也跳过。 */
			if (forkNum != INIT_FORKNUM)
				continue;

			/* 构造源路径名。 */
			snprintf(srcpath, sizeof(srcpath), "%s/%s",
					 dbspacedirname, de->d_name);

			/* 构造目标路径名。 */
			memcpy(relnumbuf, de->d_name, relnumchars);
			relnumbuf[relnumchars] = '\0';
			snprintf(dstpath, sizeof(dstpath), "%s/%s%s",
					 dbspacedirname, relnumbuf, de->d_name + relnumchars + 1 +
					 strlen(forkNames[INIT_FORKNUM]));

			/* 好，可以执行实际的复制了。 */
			elog(DEBUG2, "copying %s to %s", srcpath, dstpath);
			copy_file(srcpath, dstpath);
		}

		FreeDir(dbspace_dir);

		/*
		 * 上面的 copy_file() 已经对它创建的文件调用了 pg_flush_data()。
		 * 现在我们需要对那些文件做 fsync，因为我们处于恢复期间时，
		 * 检查点不会为我们做这件事。我们把这放在
		 * 单独的一遍里，以便内核一次完成所有刷出
		 * （尤其是元数据刷出）。
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			relnumchars;
			char		relnumbuf[OIDCHARS + 1];
			char		mainpath[MAXPGPATH];

			/* 跳过任何看起来不像关系数据文件的东西。 */
			if (!parse_filename_for_nontemp_relation(de->d_name, &relnumchars,
													 &forkNum))
				continue;

			/* 除非这是 init 分叉，否则也跳过。 */
			if (forkNum != INIT_FORKNUM)
				continue;

			/* 构造主分叉路径名。 */
			memcpy(relnumbuf, de->d_name, relnumchars);
			relnumbuf[relnumchars] = '\0';
			snprintf(mainpath, sizeof(mainpath), "%s/%s%s",
					 dbspacedirname, relnumbuf, de->d_name + relnumchars + 1 +
					 strlen(forkNames[INIT_FORKNUM]));

			fsync_fname(mainpath, false);
		}

		FreeDir(dbspace_dir);

		/*
		 * 最后，对数据库目录本身做 fsync，确保
		 * 文件系统记住我们做过的文件创建和删除。
		 * 若这次调用只做
		 * UNLOGGED_RELATION_CLEANUP，我们就不必费这个事，因为若恢复在我们
		 * 做到 UNLOGGED_RELATION_INIT 之前崩溃，下一次启动尝试时
		 * 我们也会重做清理这一步。
		 */
		fsync_fname(dbspacedirname, true);
	}
}

/*
 * 对假定的关系文件名做基本解析。
 *
 * 若文件看起来符合非临时关系的正确格式，本函数返回真，
 * 否则返回假。
 *
 * 注意：若本函数返回真，调用者可以假定
 * *relnumchars 已被设为一个不超过 OIDCHARS 的值，因此
 * 一块 OIDCHARS+1 个字符的缓冲区就足以容纳文件名里的
 * RelFileNumber 部分。这对防止
 * 可能的缓冲区越界至关重要。
 */
bool
parse_filename_for_nontemp_relation(const char *name, int *relnumchars,
									ForkNumber *fork)
{
	int			pos;

	/* 寻找一串非空数字（而且不能太长）。 */
	for (pos = 0; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (pos == 0 || pos > OIDCHARS)
		return false;
	*relnumchars = pos;

	/* 检查分叉名。 */
	if (name[pos] != '_')
		*fork = MAIN_FORKNUM;
	else
	{
		int			forkchar;

		forkchar = forkname_chars(&name[pos + 1], fork);
		if (forkchar <= 0)
			return false;
		pos += forkchar + 1;
	}

	/* 检查段号。 */
	if (name[pos] == '.')
	{
		int			segchar;

		for (segchar = 1; isdigit((unsigned char) name[pos + segchar]); ++segchar)
			;
		if (segchar <= 1)
			return false;
		pos += segchar;
	}

	/* 现在应当到了末尾。 */
	if (name[pos] != '\0')
		return false;
	return true;
}
