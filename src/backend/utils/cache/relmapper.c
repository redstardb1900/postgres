/*-------------------------------------------------------------------------
 *
 * relmapper.c
 *	  目录到文件号的映射
 *
 * 对大多数表而言，表底层的物理文件都是由
 * pg_class.relfilenode 指定的。但这显然不适用于
 * pg_class 自身，也不适用于那些"钉住的"目录——我们
 * 必须能够在不访问 pg_class 的情况下建立可用的
 * Relation 条目。它同样不适用于共享目录，因为在搬迁
 * 共享目录时没有切实可行的办法去更新其他数据库的
 * pg_class 条目。因此，对于这些特殊的目录（下文称
 * 为"映射目录"），我们依赖一个单独维护的文件来展示
 * 从目录 OID 到文件号的映射。每个数据库都有一个
 * 用于本地映射目录的映射文件，而共享目录则另有一
 * 个映射文件。映射目录的 pg_class.relfilenode 条目为零。
 *
 * 普通表的搬迁在 pg_class 行更新提交时即行提交（即新
 * 物理文件变为权威）。对映射目录来说，更新映射文件
 * 这一动作实际上就是该搬迁的提交。我们把文件更新
 * 推迟到执行重写的事务提交之前，但两者之间必然
 * 存在一个窗口。因此，映射目录只能由 VACUUM FULL、
 * CLUSTER 这类不做任何有事务意义变更的操作来搬迁：
 * 即便事务本身中止，新文件替换旧文件也必须是安全
 * 的。这里的一个重要因素是：被映射目录的索引和
 * toast 表也必须被映射，从而使所有这些文件的
 * 重写/搬迁都随一次映射文件更新而提交，而不是
 * 与事务提交绑定。
 * to transaction commit.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/relmapper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "utils/inval.h"
#include "utils/relmapper.h"


/*
 * 映射文件是关键数据：我们没有任何自动方法可以从其
 * 丢失或损坏中恢复。我们用 CRC 以便能检测
 * 损坏。由于文件可能超过一个标准磁盘扇区的大小，
 * 因此无法依赖就地覆写。我们改为生成一个新文件并将
 * 它重命名到位，原子式地替换原始文件。
 *
 * mappings[] 数组中的条目没有特定的顺序。虽然
 * 强制 OID 顺序可以加速搜索，但鉴于映射集的预期
 * 规模，这样做并不值得。
 */
#define RELMAPPER_FILENAME		"pg_filenode.map"
#define RELMAPPER_TEMP_FILENAME	"pg_filenode.map.tmp"

#define RELMAPPER_FILEMAGIC		0x592717	/* 版本 ID 值 */

/*
 * 这个常量不必有任何特定的取值，如果我们最终拥有
 * 更多被映射的关系，也可以按需把它调大。眼下，
 * 我们只挑选一个比预期映射数量稍大一点的
 * 圆整数值。
 */
#define MAX_MAPPINGS			64

typedef struct RelMapping
{
	Oid			mapoid;			/* 目录的 OID */
	RelFileNumber mapfilenumber;	/* 其关系文件号 */
} RelMapping;

typedef struct RelMapFile
{
	int32		magic;			/* 恒为RELMAPPER_FILEMAGIC */
	int32		num_mappings;	/* 有效 RelMapping 条目数 */
	RelMapping	mappings[MAX_MAPPINGS];
	pg_crc32c	crc;			/* 以上之 CRC */
} RelMapFile;

/*
 * 用于向并行 worker 序列化本地与共享 relmapping 的状态
 * （仅活跃状态）。见 active_* 与 pending_* 更新状态说明。
 */
typedef struct SerializedActiveRelMaps
{
	RelMapFile	active_shared_updates;
	RelMapFile	active_local_updates;
} SerializedActiveRelMaps;

/*
 * 共享映射文件与本数据库本地映射文件的当前内容
 * 都存储在这里。一收到更新 sinval 消息即可立即
 * 从磁盘重新加载。
 */
static RelMapFile shared_map;
static RelMapFile local_map;

/*
 * 我们用同一 RelMapFile 结构来跟踪映射中未提交的
 * 本地改动（但注意这些变量中的 magic/crc 字段并未
 * 置为有效）。目前子事务内不允许映射更新，因此
 * 一组事务级的改动就足够了。
 *
 * active_xxx 变量包含在我们事务中有效的更新，
 * 并且应当被 RelationMapOidToFilenumber 所尊重。pending_xxx
 * 变量包含我们已获悉但尚未活跃的更新；它们将在下个
 * CommandCounterIncrement 时变为活跃。这种设置使映射
 * 更新类似 pg_class 行的更新，即只在下一个
 * CommandCounterIncrement 边界处才变得可见。
 *
 * 活跃共享与活跃本地更新由并行基础设施来序列化，
 * 并在并行 worker 内反序列化。
 */
static RelMapFile active_shared_updates;
static RelMapFile active_local_updates;
static RelMapFile pending_shared_updates;
static RelMapFile pending_local_updates;


/* 非导出函数的原型 */
static void apply_map_update(RelMapFile *map, Oid relationId,
							 RelFileNumber fileNumber, bool add_okay);
static void merge_map_updates(RelMapFile *map, const RelMapFile *updates,
							  bool add_okay);
static void load_relmap_file(bool shared, bool lock_held);
static void read_relmap_file(RelMapFile *map, char *dbpath, bool lock_held,
							 int elevel);
static void write_relmap_file(RelMapFile *newmap, bool write_wal,
							  bool send_sinval, bool preserve_files,
							  Oid dbid, Oid tsid, const char *dbpath);
static void perform_relmap_update(bool shared, const RelMapFile *updates);


/*
 * RelationMapOidToFilenumber
 *
 * 存在意义……给定关系 OID，查找其文件号。
 *
 * 尽管共享和本地关系 OID 不应重叠，但调用者总是知道
 * 哪一个 --- 因此传入该信息以避免无用的
 * 搜索。
 *
 * 如果 OID 未知，则返回 InvalidRelFileNumber（这本不该
 * 发生，但调用者更有条件报告有意义的错误）。
 * error).
 */
RelFileNumber
RelationMapOidToFilenumber(Oid relationId, bool shared)
{
	const RelMapFile *map;
	int32		i;

	/* 若存在活跃更新，则以它们为准而非主映射 */
	if (shared)
	{
		map = &active_shared_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenumber;
		}
		map = &shared_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenumber;
		}
	}
	else
	{
		map = &active_local_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenumber;
		}
		map = &local_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenumber;
		}
	}

	return InvalidRelFileNumber;
}

/*
 * RelationMapFilenumberToOid
 *
 * 执行与常规映射方向相反的映射查找操作。
 * RelationMapOidToFilenumber.
 *
 * 此函数并非供正常运行期间使用。
 * 而是用于查看文件系统或 xlog 时获取信息。
 *
 * 如果 OID 未知则返回 InvalidOid；如果 relfilenumber 并不
 * 属于某被映射关系，则很容易发生。
 */
Oid
RelationMapFilenumberToOid(RelFileNumber filenumber, bool shared)
{
	const RelMapFile *map;
	int32		i;

	/* 若存在活跃更新，则以它们为准而非主映射 */
	if (shared)
	{
		map = &active_shared_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenumber == map->mappings[i].mapfilenumber)
				return map->mappings[i].mapoid;
		}
		map = &shared_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenumber == map->mappings[i].mapfilenumber)
				return map->mappings[i].mapoid;
		}
	}
	else
	{
		map = &active_local_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenumber == map->mappings[i].mapfilenumber)
				return map->mappings[i].mapoid;
		}
		map = &local_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenumber == map->mappings[i].mapfilenumber)
				return map->mappings[i].mapoid;
		}
	}

	return InvalidOid;
}

/*
 * RelationMapOidToFilenumberForDatabase
 *
 * 与 RelationMapOidToFilenumber 类似，但从指定路径读取
 * 映射，而不是使用当前数据库的那一份。
 */
RelFileNumber
RelationMapOidToFilenumberForDatabase(char *dbpath, Oid relationId)
{
	RelMapFile	map;
	int			i;

	/* 从源数据库读取 relmap 文件。 */
	read_relmap_file(&map, dbpath, false, ERROR);

	/* 遍历 relmap 条目以找到输入的关系 OID。 */
	for (i = 0; i < map.num_mappings; i++)
	{
		if (relationId == map.mappings[i].mapoid)
			return map.mappings[i].mapfilenumber;
	}

	return InvalidRelFileNumber;
}

/*
 * RelationMapCopy
 *
 * 将 relmap 文件从源数据库路径复制到目标数据库路径，
 * 并将该操作 WAL 记录。它用于为尚无 relmap 文件的
 * 数据库创建新 relmap 文件，而非替换已有
 * 的文件。
 */
void
RelationMapCopy(Oid dbid, Oid tsid, char *srcdbpath, char *dstdbpath)
{
	RelMapFile	map;

	/*
	 * 从源数据库读取 relmap 文件。
	 */
	read_relmap_file(&map, srcdbpath, false, ERROR);

	/*
	 * 把同样的数据写入目标数据库的 relmap 文件中。
	 *
	 * 不需要 sinval，因为目前还没有人能连接到目标
	 * 数据库。
	 *
	 * 在这里试图保留文件毫无意义。反正新数据库
	 * 尚不可用，且若无法安装 relmap 文件则永远无法启用。
	 * file.
	 */
	LWLockAcquire(RelationMappingLock, LW_EXCLUSIVE);
	write_relmap_file(&map, true, false, false, dbid, tsid, dstdbpath);
	LWLockRelease(RelationMappingLock);
}

/*
 * RelationMapUpdateMap
 *
 * 为指定的关系安装一个新的 relfilenumber 映射。
 *
 * 如果 immediate 为真（或我们正在引导），映射将被
 * 立即激活。否则将保持待定直至 CommandCounterIncrement。
 */
void
RelationMapUpdateMap(Oid relationId, RelFileNumber fileNumber, bool shared,
					 bool immediate)
{
	RelMapFile *map;

	if (IsBootstrapProcessingMode())
	{
		/*
		 * 在引导模式下，映射会被安装到永久映射中。
		 */
		if (shared)
			map = &shared_map;
		else
			map = &local_map;
	}
	else
	{
		/*
		 * 目前不支持在子事务中进行映射变更，也不支持
		 * 并行模式下进行。这需要更多的簿记基础设施，
		 * 但目前看来并不值得。
		 */
		if (GetCurrentTransactionNestLevel() > 1)
			elog(ERROR, "cannot change relation mapping within subtransaction");

		if (IsInParallelMode())
			elog(ERROR, "cannot change relation mapping in parallel mode");

		if (immediate)
		{
			/* 使其活跃，但仅在本地 */
			if (shared)
				map = &active_shared_updates;
			else
				map = &active_local_updates;
		}
		else
		{
			/* 使其待定 */
			if (shared)
				map = &pending_shared_updates;
			else
				map = &pending_local_updates;
		}
	}
	apply_map_update(map, relationId, fileNumber, true);
}

/*
 * apply_map_update
 *
 * 将新映射插入给定映射变量中，替换同一关系已有的
 * 映射。
 *
 * 在某些情况下调用者知道必定已存在一个映射；传入
 * add_okay = false，若没有则报错。
 */
static void
apply_map_update(RelMapFile *map, Oid relationId, RelFileNumber fileNumber,
				 bool add_okay)
{
	int32		i;

	/* 替换已有映射 */
	for (i = 0; i < map->num_mappings; i++)
	{
		if (relationId == map->mappings[i].mapoid)
		{
			map->mappings[i].mapfilenumber = fileNumber;
			return;
		}
	}

	/* 不，需要新增映射 */
	if (!add_okay)
		elog(ERROR, "attempt to apply a mapping to unmapped relation %u",
			 relationId);
	if (map->num_mappings >= MAX_MAPPINGS)
		elog(ERROR, "ran out of space in relation map");
	map->mappings[map->num_mappings].mapoid = relationId;
	map->mappings[map->num_mappings].mapfilenumber = fileNumber;
	map->num_mappings++;
}

/*
 * merge_map_updates
 *
 * 将给定待定更新映射中的所有更新合并到目标映射中。
 * 它只是 apply_map_update 的批量形式。
 */
static void
merge_map_updates(RelMapFile *map, const RelMapFile *updates, bool add_okay)
{
	int32		i;

	for (i = 0; i < updates->num_mappings; i++)
	{
		apply_map_update(map,
						 updates->mappings[i].mapoid,
						 updates->mappings[i].mapfilenumber,
						 add_okay);
	}
}

/*
 * RelationMapRemoveMapping
 *
 * 移除某关系在映射中的条目。这只允许用于活跃的（但
 * 尚未提交的）本地映射。我们需要它以便在对被映射
 * 关系执行 VACUUM FULL/CLUSTER 时回退临时目标文件的
 * 条目。
 */
void
RelationMapRemoveMapping(Oid relationId)
{
	RelMapFile *map = &active_local_updates;
	int32		i;

	for (i = 0; i < map->num_mappings; i++)
	{
		if (relationId == map->mappings[i].mapoid)
		{
			/* 找到了，抹除它 */
			map->mappings[i] = map->mappings[map->num_mappings - 1];
			map->num_mappings--;
			return;
		}
	}
	elog(ERROR, "could not find temporary mapping for relation %u",
		 relationId);
}

/*
 * RelationMapInvalidate
 *
 * 此例程由 SI 缓存刷新消息调用。我们必须重读
 * 指定的映射文件。但我们可能在尚未、也永远
 * 不会加载映射文件的进程中收到 SI 消息；例如
 * autovacuum launcher，它*绝不可*试图读取本地映射
 * 它未连接任何特定数据库。所以只在映射
 * 当前有效时才重新读取。
 */
void
RelationMapInvalidate(bool shared)
{
	if (shared)
	{
		if (shared_map.magic == RELMAPPER_FILEMAGIC)
			load_relmap_file(true, false);
	}
	else
	{
		if (local_map.magic == RELMAPPER_FILEMAGIC)
			load_relmap_file(false, false);
	}
}

/*
 * RelationMapInvalidateAll
 *
 * 重新加载所有映射文件。它用于从 SI 消息缓冲
 * 溢出恢复：不确定是否错过了 inval 消息。
 * 同样仅重载当前有效的映射。
 */
void
RelationMapInvalidateAll(void)
{
	if (shared_map.magic == RELMAPPER_FILEMAGIC)
		load_relmap_file(true, false);
	if (local_map.magic == RELMAPPER_FILEMAGIC)
		load_relmap_file(false, false);
}

/*
 * AtCCI_RelationMap
 *
 * 在 CommandCounterIncrement 时激活任何"待定"的关系映射更新。
 */
void
AtCCI_RelationMap(void)
{
	if (pending_shared_updates.num_mappings != 0)
	{
		merge_map_updates(&active_shared_updates,
						  &pending_shared_updates,
						  true);
		pending_shared_updates.num_mappings = 0;
	}
	if (pending_local_updates.num_mappings != 0)
	{
		merge_map_updates(&active_local_updates,
						  &pending_local_updates,
						  true);
		pending_local_updates.num_mappings = 0;
	}
}

/*
 * AtEOXact_RelationMap
 *
 * 处理主事务提交或中止时的关系映射。
 *
 * 提交期间，必须尽可能晚地在真正的事务提交之前调用
 * 此函数，以便把事务在提交映射变更后仍可能回滚的
 * 时间窗口降到最小。虽然在这种情况下不会发生什么
 * 严重恶果，但我们仍宁愿它不要发生，因为可能
 * 会丢失对那些关系的有用更新，涉及
 * pg_class row(s).
 *
 * 中止期间，我们只需丢弃任何待定的映射变更。
 * 中止后的常规清理会负责修复 relcache 条目。
 * 并行 worker 的提交/中止通过重置活跃映射来处理，
 * 这些活跃映射可能是从 leader 进程接收来的。（并行
 * worker 中不应有待定更新。）
 */
void
AtEOXact_RelationMap(bool isCommit, bool isParallelWorker)
{
	if (isCommit && !isParallelWorker)
	{
		/*
		 * 不应带着"待定"更新走到这里。（逻辑上
		 * 我们本可把这类更新视为已提交，但在当前
		 * 代码中这不应发生。）
		 */
		Assert(pending_shared_updates.num_mappings == 0);
		Assert(pending_local_updates.num_mappings == 0);

		/*
		 * 将活跃更新写入实际映射文件，然后将它们重置。
		 */
		if (active_shared_updates.num_mappings != 0)
		{
			perform_relmap_update(true, &active_shared_updates);
			active_shared_updates.num_mappings = 0;
		}
		if (active_local_updates.num_mappings != 0)
		{
			perform_relmap_update(false, &active_local_updates);
			active_local_updates.num_mappings = 0;
		}
	}
	else
	{
		/* 中止或并行 worker --- 丢弃全部本地和待定更新 */
		Assert(!isParallelWorker || pending_shared_updates.num_mappings == 0);
		Assert(!isParallelWorker || pending_local_updates.num_mappings == 0);

		active_shared_updates.num_mappings = 0;
		active_local_updates.num_mappings = 0;
		pending_shared_updates.num_mappings = 0;
		pending_local_updates.num_mappings = 0;
	}
}

/*
 * AtPrepare_RelationMap
 *
 * PREPARE 时处理关系映射。
 *
 * 目前，我们不支持准备任何会改变映射的事务。
 */
void
AtPrepare_RelationMap(void)
{
	if (active_shared_updates.num_mappings != 0 ||
		active_local_updates.num_mappings != 0 ||
		pending_shared_updates.num_mappings != 0 ||
		pending_local_updates.num_mappings != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that modified relation mapping")));
}

/*
 * CheckPointRelationMap
 *
 * 此函数在检查点期间被调用。它必须确保在检查点开始
 * 之前已 WAL 记录的关系映射更新都安全刷新到
 * 磁盘且以后无需重放。这看起来不太可能是性能关键
 * 问题，所以我们采用一个简单的方法：只获取并释放
 * RelationMappingLock。这能确保任何已记录的映射更新都
 * 已完成，因为 write_relmap_file 会在释放锁之前 fsync
 * 该映射文件。
 */
void
CheckPointRelationMap(void)
{
	LWLockAcquire(RelationMappingLock, LW_SHARED);
	LWLockRelease(RelationMappingLock);
}

/*
 * RelationMapFinishBootstrap
 *
 * 在引导完成时写出初始关系映射文件。应当已经
 * 通过 RelationMapUpdateMap 调用把所有被映射文件告知
 * 了我们。
 */
void
RelationMapFinishBootstrap(void)
{
	Assert(IsBootstrapProcessingMode());

	/* 不应有任何"待定"项…… */
	Assert(active_shared_updates.num_mappings == 0);
	Assert(active_local_updates.num_mappings == 0);
	Assert(pending_shared_updates.num_mappings == 0);
	Assert(pending_local_updates.num_mappings == 0);

	/* 写出文件；无需 WAL 或 sinval */
	LWLockAcquire(RelationMappingLock, LW_EXCLUSIVE);
	write_relmap_file(&shared_map, false, false, false,
					  InvalidOid, GLOBALTABLESPACE_OID, "global");
	write_relmap_file(&local_map, false, false, false,
					  MyDatabaseId, MyDatabaseTableSpace, DatabasePath);
	LWLockRelease(RelationMappingLock);
}

/*
 * RelationMapInitialize
 *
 * 此函数在进程启动时初始化映射器模块。尚不能
 * 访问数据库，只需确保映射为空即可。
 */
void
RelationMapInitialize(void)
{
	/* 静态变量本应初始化为零，但我们还是要确认一下 */
	shared_map.magic = 0;		/* 标记未加载 */
	local_map.magic = 0;
	shared_map.num_mappings = 0;
	local_map.num_mappings = 0;
	active_shared_updates.num_mappings = 0;
	active_local_updates.num_mappings = 0;
	pending_shared_updates.num_mappings = 0;
	pending_local_updates.num_mappings = 0;
}

/*
 * RelationMapInitializePhase2
 *
 * 此函数用于在启动时准备访问 pg_database。现在我们
 * 应当能够读取共享映射文件了。
 */
void
RelationMapInitializePhase2(void)
{
	/*
	 * 引导模式下映射文件尚不存在，故什么都不做。
	 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * 加载共享映射，出错即终止。
	 */
	load_relmap_file(true, false);
}

/*
 * RelationMapInitializePhase3
 *
 * 此函数在我们确定 MyDatabaseId 并设置好 DatabasePath
 * 之后尽快被调用。此时应能读取本地映射文件。
 */
void
RelationMapInitializePhase3(void)
{
	/*
	 * 引导模式下映射文件尚不存在，故什么都不做。
	 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * 加载本地映射，出错即止。
	 */
	load_relmap_file(false, false);
}

/*
 * EstimateRelationMapSpace
 *
 * 估算把活跃的共享与本地 relmap 传递给并行 worker 所需
 * 空间
 */
Size
EstimateRelationMapSpace(void)
{
	return sizeof(SerializedActiveRelMaps);
}

/*
 * SerializeRelationMap
 *
 * 为并行 worker 序列化活跃的共享与本地 relmap 状态。
 */
void
SerializeRelationMap(Size maxSize, char *startAddress)
{
	SerializedActiveRelMaps *relmaps;

	Assert(maxSize >= EstimateRelationMapSpace());

	relmaps = (SerializedActiveRelMaps *) startAddress;
	relmaps->active_shared_updates = active_shared_updates;
	relmaps->active_local_updates = active_local_updates;
}

/*
 * RestoreRelationMap
 *
 * 在并行 worker 内恢复活跃的共享与本地 relmap 状态。
 */
void
RestoreRelationMap(char *startAddress)
{
	SerializedActiveRelMaps *relmaps;

	if (active_shared_updates.num_mappings != 0 ||
		active_local_updates.num_mappings != 0 ||
		pending_shared_updates.num_mappings != 0 ||
		pending_local_updates.num_mappings != 0)
		elog(ERROR, "parallel worker has existing mappings");

	relmaps = (SerializedActiveRelMaps *) startAddress;
	active_shared_updates = relmaps->active_shared_updates;
	active_local_updates = relmaps->active_local_updates;
}

/*
 * load_relmap_file -- 加载共享或本地映射文件
 *
 * 由于这些文件对于访问核心系统目录必不可少，加载
 * 其中任何一个失败都是致命错误。
 *
 * 注意本地情形需要已设置好 DatabasePath。
 */
static void
load_relmap_file(bool shared, bool lock_held)
{
	if (shared)
		read_relmap_file(&shared_map, "global", lock_held, FATAL);
	else
		read_relmap_file(&local_map, DatabasePath, lock_held, FATAL);
}

/*
 * read_relmap_file -- 从任意关系映射文件加载数据
 *
 * dbpath 必须是相关的数据库路径，共享关系则传 "global"。
 *
 * 除非 lock_held = true，否则 RelationMappingLock加锁后释放。
 *
 * 错误将以指定的 elevel 报告，该级别至少应为
 * ERROR.
 */
static void
read_relmap_file(RelMapFile *map, char *dbpath, bool lock_held, int elevel)
{
	char		mapfilename[MAXPGPATH];
	pg_crc32c	crc;
	int			fd;
	int			r;

	Assert(elevel >= ERROR);

	/*
	 * 获取锁以免文件在我们读取时被更新，除非调用者
	 * 已持有该锁。如果文件在我们查看后不久即被更新，
	 * sinval 信令机制会使我们在能够访问任何受此变更
	 * 影响的关系之前
	 * 重读它。
	 */
	if (!lock_held)
		LWLockAcquire(RelationMappingLock, LW_SHARED);

	/*
	 * 打开目标文件。
	 *
	 * 由于 Windows 不喜欢覆盖别人打开着的文件，我们只
	 * 在拿到锁之后才打开此文件，同样原因，我们会在
	 * 释放锁之前关闭它。这样，等到 write_relmap_file() 获取
	 * 独占锁时，其他任何人都不会再有它处于打开
	 * 状态。
	 */
	snprintf(mapfilename, sizeof(mapfilename), "%s/%s", dbpath,
			 RELMAPPER_FILENAME);
	fd = OpenTransientFile(mapfilename, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						mapfilename)));

	/* 现读取数据。 */
	pgstat_report_wait_start(WAIT_EVENT_RELATION_MAP_READ);
	r = read(fd, map, sizeof(RelMapFile));
	if (r != sizeof(RelMapFile))
	{
		if (r < 0)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", mapfilename)));
		else
			ereport(elevel,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							mapfilename, r, sizeof(RelMapFile))));
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						mapfilename)));

	if (!lock_held)
		LWLockRelease(RelationMappingLock);

	/* 检查魔数等是否正确 */
	if (map->magic != RELMAPPER_FILEMAGIC ||
		map->num_mappings < 0 ||
		map->num_mappings > MAX_MAPPINGS)
		ereport(elevel,
				(errmsg("relation mapping file \"%s\" contains invalid data",
						mapfilename)));

	/* 校验 CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) map, offsetof(RelMapFile, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, map->crc))
		ereport(elevel,
				(errmsg("relation mapping file \"%s\" contains incorrect checksum",
						mapfilename)));
}

/*
 * 写出具有给定内容的新共享或本地映射文件。
 *
 * 魔数和 CRC 会自动更新到 *newmap 中。成功后我们
 * 会把数据复制到相应的永久静态变量中。
 *
 * 如果 write_wal 为真，则发出相应的 WAL 消息。
 * （引导与 WAL 重放情形下它为假。）
 *
 * 如果 send_sinval 为真，则发送 SI 失效消息。
 * （除引导情形外都应为真。）
 *
 * 如果 preserve_files 为真，则警告存储管理器不要删除
 * 映射中列出的文件。
 *
 * 因为此函数可能在 WAL 重放期间、MyDatabaseId 与
 * DatabasePath 等尚无效时被调用，所以我们需要调用者
 * 传入合适的值；共享映射传 "global"。
 *
 * 调用者还负责确保当前不可能有并发映射更新正在
 * 发生。
 */
static void
write_relmap_file(RelMapFile *newmap, bool write_wal, bool send_sinval,
				  bool preserve_files, Oid dbid, Oid tsid, const char *dbpath)
{
	int			fd;
	char		mapfilename[MAXPGPATH];
	char		maptempfilename[MAXPGPATH];

	/*
	 * 即使没有对此映射的并发使用，CheckPointRelationMap() 也
	 * 依赖此锁。若没有它，在本函数 XLogInsert() 之后、其
	 * durable_rename() 之前获取的基础备份的恢复副本中将
	 * 没有这些变更。wal_level=minimal 不需要此锁，但这
	 * 并非性能关键到值得做这种微优化。
	 */
	Assert(LWLockHeldByMeInMode(RelationMappingLock, LW_EXCLUSIVE));

	/*
	 * 填入各开销字段并更新 CRC。
	 */
	newmap->magic = RELMAPPER_FILEMAGIC;
	if (newmap->num_mappings < 0 || newmap->num_mappings > MAX_MAPPINGS)
		elog(ERROR, "attempt to write bogus relation mapping");

	INIT_CRC32C(newmap->crc);
	COMP_CRC32C(newmap->crc, (char *) newmap, offsetof(RelMapFile, crc));
	FIN_CRC32C(newmap->crc);

	/*
	 * 构造文件名 —— 一个先用于写入数据的临时文件，
	 * 以及随后我们要将它重命名过去的永久名称。
	 */
	snprintf(mapfilename, sizeof(mapfilename), "%s/%s",
			 dbpath, RELMAPPER_FILENAME);
	snprintf(maptempfilename, sizeof(maptempfilename), "%s/%s",
			 dbpath, RELMAPPER_TEMP_FILENAME);

	/*
	 * 打开临时文件。若已有同名文件，那它一定是先前
	 * 崩溃遗留的，因此可以覆写它。不允许对此函数的
	 * 并发调用。
	 */
	fd = OpenTransientFile(maptempfilename,
						   O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						maptempfilename)));

	/* 把新数据写入文件 */
	pgstat_report_wait_start(WAIT_EVENT_RELATION_MAP_WRITE);
	if (write(fd, newmap, sizeof(RelMapFile)) != sizeof(RelMapFile))
	{
		/* 若写入未设置 errno，则假定为磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						maptempfilename)));
	}
	pgstat_report_wait_end();

	/* 关闭文件。 */
	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						maptempfilename)));

	if (write_wal)
	{
		xl_relmap_update xlrec;
		XLogRecPtr	lsn;

		/* 现在错误即致命。 */
		START_CRIT_SECTION();

		xlrec.dbid = dbid;
		xlrec.tsid = tsid;
		xlrec.nbytes = sizeof(RelMapFile);

		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec), MinSizeOfRelmapUpdate);
		XLogRegisterData((char *) newmap, sizeof(RelMapFile));

		lsn = XLogInsert(RM_RELMAP_ID, XLOG_RELMAP_UPDATE);

		/* 一如往常，WAL 必须先于数据更新落盘 */
		XLogFlush(lsn);
	}

	/*
	 * durable_rename() 完成了把临时文件以崩溃安全方式
	 * 重命名到位的全部繁重工作。
	 *
	 * 注意：虽然我们让 durable_rename() 使用 ERROR，但此刻
	 * 我们常常处于临界区内；若然，ERROR 会变成 PANIC。
	 */
	pgstat_report_wait_start(WAIT_EVENT_RELATION_MAP_REPLACE);
	durable_rename(maptempfilename, mapfilename, ERROR);
	pgstat_report_wait_end();

	/*
	 * 既然文件已安全落盘，就发送 sinval 消息，让其他
	 * 后台进程重读它。必须在临界区内完成此操作：
	 * 如果因故未能发出消息，我们就必须强制全数据库
	 * 范围的 PANIC。否则其他后台进程可能带着过期的
	 * 映射信息继续执行，一旦他人开始使用已提交的
	 * 新数据，就将是灾难性的。
	 */
	if (send_sinval)
		CacheInvalidateRelmap(dbid);

	/*
	 * 确保映射列出的文件在外层事务中止时不被删除。
	 * 此操作最好也在临界区内：它不易失败，但一旦
	 * 失败，就会在文件仍易损坏的情况下进入事务
	 * 中止。进行 PANIC 会让磁盘上的东西处于良好
	 * 状态。
	 *
	 * 注意：这里稍有作弊，假定被映射文件要么在
	 * pg_global 中，要么在数据库的默认表空间中。
	 */
	if (preserve_files)
	{
		int32		i;

		for (i = 0; i < newmap->num_mappings; i++)
		{
			RelFileLocator rlocator;

			rlocator.spcOid = tsid;
			rlocator.dbOid = dbid;
			rlocator.relNumber = newmap->mappings[i].mapfilenumber;
			RelationPreserveStorage(rlocator, false);
		}
	}

	/* 临界区结束 */
	if (write_wal)
		END_CRIT_SECTION();
}

/*
 * 将指定的更新合并到相应的"真实"映射中，
 * 并写出变更。此函数必须用于在常规多用户操作
 * 中提交更新。
 */
static void
perform_relmap_update(bool shared, const RelMapFile *updates)
{
	RelMapFile	newmap;

	/*
	 * 任何更新某关系映射信息的人都应当获取该关系的
	 * 排他锁并持有到提交。这确保同一映射值上不会有
	 * 并发更新；但同一文件中不同映射值上容易并发
	 * 更新。我们为此而获取 RelationMappingLock、重读
	 * 目标文件，以确保它是最新的、然后应用
	 * 这些更新，并在释放 RelationMappingLock 之前
	 * 把数据写出。
	 *
	 * 只有一个 RelationMappingLock。原则上我们可以尝试
	 * 为每个映射文件各配一把，但似乎并不值得
	 * trouble.
	 */
	LWLockAcquire(RelationMappingLock, LW_EXCLUSIVE);

	/* 确保能看到别人刚做的任何更新 */
	load_relmap_file(shared, true);

	/* 用局部变量准备更新后的数据 */
	if (shared)
		memcpy(&newmap, &shared_map, sizeof(RelMapFile));
	else
		memcpy(&newmap, &local_map, sizeof(RelMapFile));

	/*
	 * 把更新应用到 newmap。不应出现新映射，除非
	 * 有人正在向系统目录添加索引。
	 */
	merge_map_updates(&newmap, updates, allowSystemTableMods);

	/* 写出更新后的映射并执行其他必要任务 */
	write_relmap_file(&newmap, true, true, true,
					  (shared ? InvalidOid : MyDatabaseId),
					  (shared ? GLOBALTABLESPACE_OID : MyDatabaseTableSpace),
					  (shared ? "global" : DatabasePath));

	/*
	 * 我们已成功写出更新后的文件，因此本进程现在也
	 * 可以依赖这些新值了。
	 */
	if (shared)
		memcpy(&shared_map, &newmap, sizeof(RelMapFile));
	else
		memcpy(&local_map, &newmap, sizeof(RelMapFile));

	/* 现在可以释放锁了 */
	LWLockRelease(RelationMappingLock);
}

/*
 * RELMAP 资源管理器的例程
 */
void
relmap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* relmap 记录不使用备份块 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_RELMAP_UPDATE)
	{
		xl_relmap_update *xlrec = (xl_relmap_update *) XLogRecGetData(record);
		RelMapFile	newmap;
		char	   *dbpath;

		if (xlrec->nbytes != sizeof(RelMapFile))
			elog(PANIC, "relmap_redo: wrong size %u in relmap update record",
				 xlrec->nbytes);
		memcpy(&newmap, xlrec->data, sizeof(newmap));

		/* 我们需要构造此数据库的路径名 */
		dbpath = GetDatabasePath(xlrec->dbid, xlrec->tsid);

		/*
		 * 写出新映射并发送 sinval，但当然不要写新的
		 * WAL 条目。也没有外围事务可以告知其去
		 * 保留文件。
		 *
		 * 在 WAL 重放期间不应有其他人更新 relmap，
		 * 但要获取锁以与 load_relmap_file() 互斥。
		 *
		 * 注意我们更新既有数据库 relmap 的 WAL 记录与
		 * 建新数据库的记录相同。在后一情形下，
		 * 记录 relmap 日志并发送 sinval 消息并非必要，
		 * 但无害。如果想避免这一点，可以在 WAL 记录中
		 * 加入一个标志，用以指示正在执行的哪种
		 * 操作。
		 */
		LWLockAcquire(RelationMappingLock, LW_EXCLUSIVE);
		write_relmap_file(&newmap, false, true, false,
						  xlrec->dbid, xlrec->tsid, dbpath);
		LWLockRelease(RelationMappingLock);

		pfree(dbpath);
	}
	else
		elog(PANIC, "relmap_redo: unknown op code %u", info);
}
