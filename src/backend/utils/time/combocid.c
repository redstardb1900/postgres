/*-------------------------------------------------------------------------
 *
 * combocid.c
 *	  组合命令 ID 支持例程
 *
 * 在 8.3 版之前，HeapTupleHeaderData 为 cmin 和 cmax
 * 设置了独立的字段。为缩小头部大小，cmin 和 cmax 现
 * 在重叠到头部的同一字段中。这通常可行，因为同一
 * 事务里很少既插入又删除同一元组，而且我们也不
 * 需要这两个字段在产生它的事务结束后仍然有效。
 * 插入事务确实删除该元组时，为让它仍能工作，我们
 * 会创建"组合"命令 ID 存入元组头部，用来代替
 * cmin 和 cmax。组合命令 ID 可以通过一个后端私有
 * 数组映射回真实 cmin 和 cmax，该数组由本模块
 * 管理。
 *
 * 为复用已有组合 CID，我们还维护一个哈希表，把
 * cmin,cmax 对映射到组合 CID。这在多数情况下能让数
 * 据结构保持合理大小，因为任一事务用到的去重对数
 * 往往都很小。
 *
 * 用 32 位组合命令 ID 可以表示 2^32 种不同的
 * cmin,cmax 组合。在最病态的情况下，每条命令都删除
 * 之前每条命令产生的元组，N 条命令需要的组合数
 * 量 ID 为 N*(N+1)/2。也就是说最坏情况下，
 * 这对 92682 条命令也足够了。实际上你会先耗尽内存
 * 和/或磁盘空间，远到不了那个上限。
 *
 * 数组和哈希表保存在 TopTransactionContext 中，并在
 * 每个事务结束时销毁。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/combocid.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/shmem.h"
#include "utils/combocid.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/* 按 cmin 和 cmax 查找组合 CID 的哈希表 */
static HTAB *comboHash = NULL;

/* 哈希表的键与条目结构 */
typedef struct
{
	CommandId	cmin;
	CommandId	cmax;
} ComboCidKeyData;

typedef ComboCidKeyData *ComboCidKey;

typedef struct
{
	ComboCidKeyData key;
	CommandId	combocid;
} ComboCidEntryData;

typedef ComboCidEntryData *ComboCidEntry;

/* 哈希表的初始大小 */
#define CCID_HASH_SIZE			100


/*
 * cmin,cmax 对的数组，以组合命令 ID 为下标。
 * 要把组合 CID 转成 cmin/cmax，做一次简单数组
 */
static ComboCidKey comboCids = NULL;
static int	usedComboCids = 0;	/* comboCids 中的元素数 */
static int	sizeComboCids = 0;	/* 已分配数组大小 */

/* 数组的初始大小 */
#define CCID_ARRAY_SIZE			100


/* 内部函数的原型 */
static CommandId GetComboCommandId(CommandId cmin, CommandId cmax);
static CommandId GetRealCmin(CommandId combocid);
static CommandId GetRealCmax(CommandId combocid);


/**** 外部 API ****/

/*
 * GetCmin 和 GetCmax 会断言自己只在有意义的情况下被
 * 调用，即能给出有用答案的场合。若有理由从产生
 * 它的事务以外检查元组的 t_cid 字段，请直接使用
 * HeapTupleHeaderGetRawCommandId()。
 */

CommandId
HeapTupleHeaderGetCmin(HeapTupleHeader tup)
{
	CommandId	cid = HeapTupleHeaderGetRawCommandId(tup);

	Assert(!(tup->t_infomask & HEAP_MOVED));
	Assert(TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tup)));

	if (tup->t_infomask & HEAP_COMBOCID)
		return GetRealCmin(cid);
	else
		return cid;
}

CommandId
HeapTupleHeaderGetCmax(HeapTupleHeader tup)
{
	CommandId	cid = HeapTupleHeaderGetRawCommandId(tup);

	Assert(!(tup->t_infomask & HEAP_MOVED));

	/*
	 * 由于当 xmax 是 multixact 时 GetUpdateXid() 会执行
	 * 内存分配，故不能在临界区内 Assert()。这会削
	 * 弱该检查，但在临界区内不用 GetCmax() 又会把事情
	 * 复杂化。
	 */
	Assert(CritSectionCount > 0 ||
		   TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tup)));

	if (tup->t_infomask & HEAP_COMBOCID)
		return GetRealCmax(cid);
	else
		return cid;
}

/*
 * 给定一个即将删除的元组，确定要存入其 t_cid 字段
 * 的正确值。
 *
 * 如果我们不需要组合 CID，*cmax 不变且 *iscombo 置
 * 为假。如果需要，*cmax 被替换为组合 CID 且 *iscombo
 * 置为真。
 *
 * 这一步与实际的 HeapTupleHeaderSetCmax() 操作分开
 * 的原因是它可能因内存耗尽而失败。故必须在进入
 * 真正修改共享缓冲区中元组的临界区之前完成这
 * 一步。
 */
void
HeapTupleHeaderAdjustCmax(HeapTupleHeader tup,
						  CommandId *cmax,
						  bool *iscombo)
{
	/*
	 * 若删的元组出自我们（或子事务），用组合
	 * 命令 ID。先测 HeapTupleHeaderXminCommitted()，
	 * 因为它比 TransactionIdIsCurrentTransactionId 调用更
	 * 便宜。
	 */
	if (!HeapTupleHeaderXminCommitted(tup) &&
		TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tup)))
	{
		CommandId	cmin = HeapTupleHeaderGetCmin(tup);

		*cmax = GetComboCommandId(cmin, *cmax);
		*iscombo = true;
	}
	else
	{
		*iscombo = false;
	}
}

/*
 * 组合命令 ID 只有插入/删除事务关心，所以可在事务
 * 结束时忘掉它们。
 */
void
AtEOXact_ComboCid(void)
{
	/*
	 * 不必费心 pfree。这些都在 TopTransactionContext 中分
	 * 配，反正事务结束时它们就会消失。
	 */
	comboHash = NULL;

	comboCids = NULL;
	usedComboCids = 0;
	sizeComboCids = 0;
}


/**** 内部例程 ****/

/*
 * 取映射到 cmin/cmax 的组合 CID。
 *
 * 我们尽量复用旧组合命令 ID。
 */
static CommandId
GetComboCommandId(CommandId cmin, CommandId cmax)
{
	CommandId	combocid;
	ComboCidKeyData key;
	ComboCidEntry entry;
	bool		found;

	/*
	 * 在事务里第一次要用组合 CID 时，创建哈希表与数
	 * 组。
	 */
	if (comboHash == NULL)
	{
		HASHCTL		hash_ctl;

		/* 先建数组；哈希表的存在断言了数组存在 */
		comboCids = (ComboCidKeyData *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(ComboCidKeyData) * CCID_ARRAY_SIZE);
		sizeComboCids = CCID_ARRAY_SIZE;
		usedComboCids = 0;

		hash_ctl.keysize = sizeof(ComboCidKeyData);
		hash_ctl.entrysize = sizeof(ComboCidEntryData);
		hash_ctl.hcxt = TopTransactionContext;

		comboHash = hash_create("Combo CIDs",
								CCID_HASH_SIZE,
								&hash_ctl,
								HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	/*
	 * 若无空闲槽则扩展数组。这必须在可能新建哈希
	 * 表条目之前完成；否则 repalloc 失败会留下损坏
	 * 希表条目。
	 */
	if (usedComboCids >= sizeComboCids)
	{
		int			newsize = sizeComboCids * 2;

		comboCids = (ComboCidKeyData *)
			repalloc(comboCids, sizeof(ComboCidKeyData) * newsize);
		sizeComboCids = newsize;
	}

	/* 查找或创建具有所需 cmin/cmax 的哈希条目 */

	/* 我们假定 ComboCidKeyData 没有结构填充！ */
	key.cmin = cmin;
	key.cmax = cmax;
	entry = (ComboCidEntry) hash_search(comboHash,
										&key,
										HASH_ENTER,
										&found);

	if (found)
	{
		/* 复用已有的组合 CID */
		return entry->combocid;
	}

	/* 必须新建组合 CID；我们已在数组中腾出空间 */
	combocid = usedComboCids;

	comboCids[combocid].cmin = cmin;
	comboCids[combocid].cmax = cmax;
	usedComboCids++;

	entry->combocid = combocid;

	return combocid;
}

static CommandId
GetRealCmin(CommandId combocid)
{
	Assert(combocid < usedComboCids);
	return comboCids[combocid].cmin;
}

static CommandId
GetRealCmax(CommandId combocid)
{
	Assert(combocid < usedComboCids);
	return comboCids[combocid].cmax;
}

/*
 * 估算序列化当前组合 CID 状态所需的空间
 * 量。
 */
Size
EstimateComboCIDStateSpace(void)
{
	Size		size;

	/* 加上保存 usedComboCids 所需的空间 */
	size = sizeof(int);

	/* 加上保存 ComboCidKeyData 所需的空间 */
	size = add_size(size, mul_size(sizeof(ComboCidKeyData), usedComboCids));

	return size;
}

/*
 * 把组合 CID 状态序列化到从 start_address 开始的内存
 * 中。maxsize 至少应与 EstimateComboCIDStateSpace 返
 * 回的值一样大。
 */
void
SerializeComboCIDState(Size maxsize, char *start_address)
{
	char	   *endptr;

	/* 首先，我们存储现存组合 CID 的数量。 */
	*(int *) start_address = usedComboCids;

	/* 若 maxsize 太小则抛错。 */
	endptr = start_address + sizeof(int) +
		(sizeof(ComboCidKeyData) * usedComboCids);
	if (endptr < start_address || endptr > start_address + maxsize)
		elog(ERROR, "not enough space to serialize ComboCID state");

	/* 复制实际的 cmin/cmax 对。 */
	if (usedComboCids > 0)
		memcpy(start_address + sizeof(int), comboCids,
			   (sizeof(ComboCidKeyData) * usedComboCids));
}

/*
 * 读取指定地址的组合 CID 状态，并用相同组合 CID 初
 * 始化本后端。这仅对当前没有组合 CID 的后端有效（
 * 而且也只有事务状态同样被序列化和恢复时才有意义
 * ）。
 */
void
RestoreComboCIDState(char *comboCIDstate)
{
	int			num_elements;
	ComboCidKeyData *keydata;
	int			i;
	CommandId	cid;

	Assert(!comboCids && !comboHash);

	/* 首先，取出被序列化的组合 CID 数量。 */
	num_elements = *(int *) comboCIDstate;
	keydata = (ComboCidKeyData *) (comboCIDstate + sizeof(int));

	/* 用 GetComboCommandId 恢复每个组合 CID。 */
	for (i = 0; i < num_elements; i++)
	{
		cid = GetComboCommandId(keydata[i].cmin, keydata[i].cmax);

		/* 验证我们得到了预期的答案。 */
		if (cid != i)
			elog(ERROR, "unexpected command ID while restoring combo CIDs");
	}
}
