/*-------------------------------------------------------------------------
 *
 * slot.c
 *	   复制槽管理。          
 *
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/slot.c
 *
 * NOTES
 *
 * 复制槽用于保存源自本集簇的复制流的状态。      
 * 其主要目的是防止以会干扰复制的方式过早移除 WAL    
 * 或旧元组版本；它们也可用于监控目的。                  
 * 槽必须是持久的（以允许重启）、崩溃安全的，并且    
 * 能在备库上分配（以支持级联设置）。由于槽需要能在   
 * 备库上使用，因此无法把它们存放在                         
 * 系统表中。                                                  
 *
 * 每个复制槽都在 $PGDATA/pg_replslot 下拥有自己的               
 * 目录，状态文件就存放在其中，包含槽自身的数据。  
 * 若有需要，其他数据也可与该文件一同存放。服务器   
 * 运行期间，状态数据还会同时缓存在内存中以求         
 * 高效。  
 *
 * ReplicationSlotAllocationLock 必须以独占模式持有才能          
 * 分配或释放槽。ReplicationSlotControlLock 须以共享模式     
 * 持有以遍历各槽，并以独占模式持有才能修改槽的        
 * in_use 标志。槽中其余数据由其互斥锁保护。            
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/transam.h"
#include "access/xlog_internal.h"
#include "access/xlogrecovery.h"
#include "common/file_utils.h"
#include "common/string.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/slot.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/builtins.h"

/*
 * 复制槽的磁盘数据结构。       
 */
typedef struct ReplicationSlotOnDisk
{
	/* 该结构体的前一部分需要与版本无关          */

	/* 校验和未覆盖的数据  */
	uint32		magic;
	pg_crc32c	checksum;

	/* 校验和覆盖的数据 */
	uint32		version;
	uint32		length;

	/*
	 * 槽中紧随其后的实际数据可能不同于上述                
	 * "版本"  
	 */

	ReplicationSlotPersistentData slotdata;
} ReplicationSlotOnDisk;

/* 与版本无关数据的大小   */
#define ReplicationSlotOnDiskConstantSize \
	offsetof(ReplicationSlotOnDisk, slotdata)
/* 槽中校验和未覆盖部分的大小                  */
#define ReplicationSlotOnDiskNotChecksummedSize  \
	offsetof(ReplicationSlotOnDisk, version)
/* 校验和覆盖部分的大小           */
#define ReplicationSlotOnDiskChecksummedSize \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskNotChecksummedSize
/* 版本相关的槽数据部分的大小         */
#define ReplicationSlotOnDiskV2Size \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskConstantSize

#define SLOT_MAGIC		0x1051CA1	/* 格式标识符   */
#define SLOT_VERSION	3		/* 新文件的版本号 */

/* 复制槽管理用的控制数组             */
ReplicationSlotCtlData *ReplicationSlotCtl = NULL;

/* 本后端在共享内存数组中的复制槽            */
ReplicationSlot *MyReplicationSlot = NULL;

/* GUC 变量   */
int			max_replication_slots = 10; /* the maximum number of replication
										 * slots */

static void ReplicationSlotShmemExit(int code, Datum arg);
static void ReplicationSlotDropAcquired(void);
static void ReplicationSlotDropPtr(ReplicationSlot *slot);

/* 内部持久化函数          */
static void RestoreSlotFromDisk(const char *name);
static void CreateSlotOnDisk(ReplicationSlot *slot);
static void SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel);

/*
 * 报告 ReplicationSlotsShmemInit 所需共享内存空间。   
 */
Size
ReplicationSlotsShmemSize(void)
{
	Size		size = 0;

	if (max_replication_slots == 0)
		return size;

	size = offsetof(ReplicationSlotCtlData, replication_slots);
	size = add_size(size,
					mul_size(max_replication_slots, sizeof(ReplicationSlot)));

	return size;
}

/*
 * 为复制槽分配并初始化共享内存。               
 */
void
ReplicationSlotsShmemInit(void)
{
	bool		found;

	if (max_replication_slots == 0)
		return;

	ReplicationSlotCtl = (ReplicationSlotCtlData *)
		ShmemInitStruct("ReplicationSlot Ctl", ReplicationSlotsShmemSize(),
						&found);

	if (!found)
	{
		int			i;

		/* 首次执行，故进行初始化 */
		MemSet(ReplicationSlotCtl, 0, ReplicationSlotsShmemSize());

		for (i = 0; i < max_replication_slots; i++)
		{
			ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];

			/* 其余内容均由上面的 memset 清零     */
			SpinLockInit(&slot->mutex);
			LWLockInitialize(&slot->io_in_progress_lock,
							 LWTRANCHE_REPLICATION_SLOT_IO);
			ConditionVariableInit(&slot->active_cv);
		}
	}
}

/*
 * 注册用于复制槽清理与释放的回调。                 
 */
void
ReplicationSlotInitialize(void)
{
	before_shmem_exit(ReplicationSlotShmemExit, 0);
}

/*
 * 释放并清理复制槽。           
 */
static void
ReplicationSlotShmemExit(int code, Datum arg)
{
	/* 确保已激活的复制槽都被释放         */
	if (MyReplicationSlot != NULL)
		ReplicationSlotRelease();

	/* 同时清理所有临时槽。        */
	ReplicationSlotCleanup();
}

/*
 * 检查传入的槽名是否有效，并在 elevel 级别报告错误。 
 *
 * 参见 ReplicationSlotValidateNameInternal() 注释    
 */
bool
ReplicationSlotValidateName(const char *name, int elevel)
{
	int			err_code;
	char	   *err_msg = NULL;
	char	   *err_hint = NULL;

	if (!ReplicationSlotValidateNameInternal(name, &err_code, &err_msg,
											 &err_hint))
	{
		ereport(elevel,
				errcode(err_code),
				errmsg_internal("%s", err_msg),
				(err_hint != NULL) ? errhint("%s", err_hint) : 0);

		pfree(err_msg);
		if (err_hint != NULL)
			pfree(err_hint);
		return false;
	}

	return true;
}

/*
 * 检查传入的槽名是否有效。        
 *
 * 槽名可由 [a-z0-9_]{1,NAMEDATALEN-1} 组成，这应能让              
 * 使其在所有受支持的 OS 上都可用作目录名。    
 *
 * 若槽名有效则返回 true；否则返回 false，并将错误码、    
 * 错误消息和可选提示分别存入 err_code、err_msg 和            
 * err_hint。调用方负责释放 palloc 分配的 err_msg 和            
 * err_hint。                  
 */
bool
ReplicationSlotValidateNameInternal(const char *name, int *err_code,
									char **err_msg, char **err_hint)
{
	const char *cp;

	if (strlen(name) == 0)
	{
		*err_code = ERRCODE_INVALID_NAME;
		*err_msg = psprintf(_("replication slot name \"%s\" is too short"), name);
		*err_hint = NULL;
		return false;
	}

	if (strlen(name) >= NAMEDATALEN)
	{
		*err_code = ERRCODE_NAME_TOO_LONG;
		*err_msg = psprintf(_("replication slot name \"%s\" is too long"), name);
		*err_hint = NULL;
		return false;
	}

	for (cp = name; *cp; cp++)
	{
		if (!((*cp >= 'a' && *cp <= 'z')
			  || (*cp >= '0' && *cp <= '9')
			  || (*cp == '_')))
		{
			*err_code = ERRCODE_INVALID_NAME;
			*err_msg = psprintf(_("replication slot name \"%s\" contains invalid character"), name);
			*err_hint = psprintf(_("Replication slot names may only contain lower case letters, numbers, and the underscore character."));
			return false;
		}
	}
	return true;
}

/*
 * 创建新的复制槽，并标记为由本后端使用。         
 *
 * name: 槽名          
 * db_specific: 逻辑解码是按数据库区分的；若该槽将用  
 *	   为此传 true，否则传 false。         
 * two_phase: 允许解码已准备好的事务。我们只允许在        
 *     创建槽时启用。若允许它在解码期间被改动，         
 *     那么很可能首次因该选项未启用而跳过 prepare，      
 *     而下次获取变更时，若 two_phase 选项已启用，又会因 
 *     起始解码点届时已移动而再次跳过 prepare。              
 *     这样用户到时就只会得到                                       
 *     commit prepared。                 
 */
void
ReplicationSlotCreate(const char *name, bool db_specific,
					  ReplicationSlotPersistency persistency, bool two_phase)
{
	ReplicationSlot *slot = NULL;
	int			i;

	Assert(MyReplicationSlot == NULL);

	ReplicationSlotValidateName(name, ERROR);

	/*
	 * 若其他后端与我们并发执行这段代码，我们很可能   
	 * 会同时分到同一个槽，那会很糟。还可能漏掉        
	 * 名称冲突。此外，我们不希望在别人正忙于清理旧槽  
	 * 时创建新槽，因为那样我们可能都在摆弄同一个    
	 * 目录。                                       
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/*
	 * 检查名称冲突，并找出一个可分配的槽。为此我们需  
	 * 要以共享模式持有 ReplicationSlotControlLock，这样别        
	 * 人就无法在我们查看期间改动 in_use 标志。     
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("replication slot \"%s\" already exists", name)));
		if (!s->in_use && slot == NULL)
			slot = s;
	}
	LWLockRelease(ReplicationSlotControlLock);

	/* 若所有槽都在用，就无法分配。  */
	if (slot == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("all replication slots are in use"),
				 errhint("Free one or increase max_replication_slots.")));

	/*
	 * 此槽既然不在使用中，别人就不应查看它除 in_use      
	 * 字段之外的任何部分，除非对方正试图分配它。而  
	 * 我们持有 ReplicationSlotAllocationLock，所以除我们之      
	 * 外无人能这样做。故初始化是安全的。 
	 */
	Assert(!slot->in_use);
	Assert(slot->active_pid == 0);

	/* 先初始化持久化数据      */
	memset(&slot->data, 0, sizeof(ReplicationSlotPersistentData));
	namestrcpy(&slot->data.name, name);
	slot->data.database = db_specific ? MyDatabaseId : InvalidOid;
	slot->data.persistency = persistency;
	slot->data.two_phase = two_phase;
	slot->data.two_phase_at = InvalidXLogRecPtr;

	/* 再初始化共享内存中才有的数据  */
	slot->just_dirtied = false;
	slot->dirty = false;
	slot->effective_xmin = InvalidTransactionId;
	slot->effective_catalog_xmin = InvalidTransactionId;
	slot->candidate_catalog_xmin = InvalidTransactionId;
	slot->candidate_xmin_lsn = InvalidXLogRecPtr;
	slot->candidate_restart_valid = InvalidXLogRecPtr;
	slot->candidate_restart_lsn = InvalidXLogRecPtr;

	/*
	 * 在磁盘上创建槽。此时槽实际上尚未标记为已分配，  
	 * 因此若此处出错也无需特别的清理。          
	 */
	CreateSlotOnDisk(slot);

	/*
	 * 翻转 in_use 标志期间，需短暂阻止任何其他后端      
	 * 遍历各槽。我们还需在持有 ControlLock 时设置 active  
	 * 标志，否则并发的 ReplicationSlotAcquire() 也可能 
	 * 获取到该槽。                                      
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);

	slot->in_use = true;

	/* 现在可将槽标记为 active，它从此归我们所有。*/
	SpinLockAcquire(&slot->mutex);
	Assert(slot->active_pid == 0);
	slot->active_pid = MyProcPid;
	SpinLockRelease(&slot->mutex);
	MyReplicationSlot = slot;

	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * 为新逻辑槽创建统计条目。物理槽不收集统计数据， 
	 * 因此无需为它们创建条目。至于为何要在释放         
	 * ReplicationSlotAllocationLock 之前就要这样做，参见          
	 * ReplicationSlotDropPtr。     
	 */
	if (SlotIsLogical(slot))
		pgstat_create_replslot(slot);

	/*
	 * 槽已被标记为 in_use 且 active，现在让别人尝试分配  
	 * 新槽也是安全的了。              
	 */
	LWLockRelease(ReplicationSlotAllocationLock);

	/* 让所有人都知道我们已修改此槽  */
	ConditionVariableBroadcast(&slot->active_cv);
}

/*
 * 搜索指定名称的复制槽。     
 *
 * 找到则返回该复制槽，否则返回 NULL。   
 */
ReplicationSlot *
SearchNamedReplicationSlot(const char *name, bool need_lock)
{
	int			i;
	ReplicationSlot *slot = NULL;

	if (need_lock)
		LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
		{
			slot = s;
			break;
		}
	}

	if (need_lock)
		LWLockRelease(ReplicationSlotControlLock);

	return slot;
}

/*
 * 返回该复制槽在 ReplicationSlotCtl-> 
 * replication_slots 中的下标。     
 *
 * 这主要是为了给存储复制槽统计数据提供一个高效键。   
 *       
 */
int
ReplicationSlotIndex(ReplicationSlot *slot)
{
	Assert(slot >= ReplicationSlotCtl->replication_slots &&
		   slot < ReplicationSlotCtl->replication_slots + max_replication_slots);

	return slot - ReplicationSlotCtl->replication_slots;
}

/*
 * 若 "index" 处的槽未被使用，则返回 false；否则将           
 * "name" 设为槽名并返回 true。 
 *
 * 这大概只对关闭时的 pgstat_replslot.c 有用，其他情况       
 * 下存在明显的 TOCTOU 问题。   
 */
bool
ReplicationSlotName(int index, Name name)
{
	ReplicationSlot *slot;
	bool		found;

	slot = &ReplicationSlotCtl->replication_slots[index];

	/*
	 * 确保在复制名称期间该槽无法被删除。由于既有槽  
	 * 的名称不会改变，因此无需自旋锁。                
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	found = slot->in_use;
	if (slot->in_use)
		namestrcpy(name, NameStr(slot->data.name));
	LWLockRelease(ReplicationSlotControlLock);

	return found;
}

/*
 * 查找先前创建的槽，并将其标记为由本进程使用。 
 *
 * 若 nowait 为 true 且该槽当前正在使用，则会报错。若     
 * nowait 为 false，我们将休眠直到槽被属主进程释放。        
 */
void
ReplicationSlotAcquire(const char *name, bool nowait)
{
	ReplicationSlot *s;
	int			active_pid;

	Assert(name != NULL);

retry:
	Assert(MyReplicationSlot == NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	/*
	 * 若未给定要获取的槽，则按指定名称搜索该槽。若   
	 * 找不到该槽，我们要么返回 -1，要么报错退出。       
	 */
	s = SearchNamedReplicationSlot(name, false);
	if (s == NULL || !s->in_use)
	{
		LWLockRelease(ReplicationSlotControlLock);

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist",
						name)));
	}

	/*
	 * 这就是想要的槽；检查它是否被他人激活。      
	 * 单用户模式下无需此检查。                    
	 */
	if (IsUnderPostmaster)
	{
		/*
		 * 准备好在该槽活跃时在其上休眠。（最后可能     
		 * 并不需要休眠，但不想在持有自旋锁时做这件   
		 * 事。） 
		 */
		if (!nowait)
			ConditionVariablePrepareToSleep(&s->active_cv);

		SpinLockAcquire(&s->mutex);
		if (s->active_pid == 0)
			s->active_pid = MyProcPid;
		active_pid = s->active_pid;
		SpinLockRelease(&s->mutex);
	}
	else
		s->active_pid = active_pid = MyProcPid;
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * 若我们找到了该槽，但它已在另一个进程中处于活 
	 * 跃状态，则等待属主进程发信号告知已释放，或者  
	 * 报错。 
	 */
	if (active_pid != MyProcPid)
	{
		if (!nowait)
		{
			/* 在此等待，直到收到信号后重新开始  */
			ConditionVariableSleep(&s->active_cv,
								   WAIT_EVENT_REPLICATION_SLOT_DROP);
			ConditionVariableCancelSleep();
			goto retry;
		}

		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("replication slot \"%s\" is active for PID %d",
						NameStr(s->data.name), active_pid)));
	}
	else if (!nowait)
		ConditionVariableCancelSleep(); /* 结果无需休眠        */

	/* 让所有人都知道我们已修改此槽  */
	ConditionVariableBroadcast(&s->active_cv);

	/* 此槽由我们激活，归我们所有。  */
	MyReplicationSlot = s;

	/*
	 * 调用 pgstat_acquire_replslot() 是为了防止                  
	 * pgstat_report_replslot() 期间出现重启前遗留的            
	 * 其他槽统计数据。 
	 */
	if (SlotIsLogical(s))
		pgstat_acquire_replslot(s);
}

/*
 * 释放本后端当前持有的复制槽。                      
 *
 * 该槽稍后可被任意后端重新获取。         
 * 该槽所需的资源将被保留。           
 */
void
ReplicationSlotRelease(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL && slot->active_pid != 0);

	if (slot->data.persistency == RS_EPHEMERAL)
	{
		/*
		 * 删除该槽。不存在允许失败的 !PANIC 场景，         
		 * 可能发生的只是磁盘上数据清理得不完整           
		 * 。  
		 */
		ReplicationSlotDropAcquired();
	}

	/*
	 * 若槽为了创建系统表快照而需要临时同时约束数据  
	 * 和系统表 xmin，请移除该临时约束。               
	 * 快照只有在初始快照仍被持有时才能被导出         
	 * 。      
	 */
	if (!TransactionIdIsValid(slot->data.xmin) &&
		TransactionIdIsValid(slot->effective_xmin))
	{
		SpinLockAcquire(&slot->mutex);
		slot->effective_xmin = InvalidTransactionId;
		SpinLockRelease(&slot->mutex);
		ReplicationSlotsComputeRequiredXmin(false);
	}

	if (slot->data.persistency == RS_PERSISTENT)
	{
		/*
		 * 把持久槽标记为不活跃。不是释放它，而是 
		 * 断开连接，但需唤醒可能在等待它的进程。    
		 */
		SpinLockAcquire(&slot->mutex);
		slot->active_pid = 0;
		SpinLockRelease(&slot->mutex);
		ConditionVariableBroadcast(&slot->active_cv);
	}

	MyReplicationSlot = NULL;

	/* 作为普通槽时该值可能尚未设置           */
	LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
	MyProc->statusFlags &= ~PROC_IN_LOGICAL_DECODING;
	ProcGlobal->statusFlags[MyProc->pgxactoff] = MyProc->statusFlags;
	LWLockRelease(ProcArrayLock);
}

/*
 * 清理当前会话中创建的所有临时槽。       
 */
void
ReplicationSlotCleanup(void)
{
	int			i;

	Assert(MyReplicationSlot == NULL);

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		if (s->active_pid == MyProcPid)
		{
			Assert(s->data.persistency == RS_TEMPORARY);
			SpinLockRelease(&s->mutex);
			LWLockRelease(ReplicationSlotControlLock);	/* 避免死锁   */

			ReplicationSlotDropPtr(s);

			ConditionVariableBroadcast(&s->active_cv);
			goto restart;
		}
		else
			SpinLockRelease(&s->mutex);
	}

	LWLockRelease(ReplicationSlotControlLock);
}

/*
 * 按传入名称永久删除复制槽。                            
 */
void
ReplicationSlotDrop(const char *name, bool nowait)
{
	Assert(MyReplicationSlot == NULL);

	ReplicationSlotAcquire(name, nowait);

	ReplicationSlotDropAcquired();
}

/*
 * 永久删除当前已获取的复制槽。               
 */
static void
ReplicationSlotDropAcquired(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(MyReplicationSlot != NULL);

	/* 槽已不再被获取       */
	MyReplicationSlot = NULL;

	ReplicationSlotDropPtr(slot);
}

/*
 * 永久删除该复制槽；本函数返回之时，该槽就已被       
 * 释放。             
 */
static void
ReplicationSlotDropPtr(ReplicationSlot *slot)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];

	/*
	 * 若其他后端与我们并发执行这段代码，我们可能在    
	 * 删除某个名称的槽时，别人正试图用同一名称创建   
	 * 新槽。                        
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/* 生成路径名。  */
	sprintf(path, "pg_replslot/%s", NameStr(slot->data.name));
	sprintf(tmppath, "pg_replslot/%s.tmp", NameStr(slot->data.name));

	/*
	 * 重命名磁盘上的槽目录，使我们不再把它识别为     
	 * 有效槽。注意若此步失败，我们必须先把槽标记为   
	 * 不活跃然后再退出。若删除的是瞬态或临时槽，       
	 * 就绝不应硬性失败，因为调用方不会预期槽还能存  
	 * 活下来，而且此函数可能在错误处理期间被调用。  
	 */
	if (rename(path, tmppath) == 0)
	{
		/*
		 * 我们需要对刚重命名的目录及其父目录执行         
		 * fsync()，以确保改动以崩溃安全的方式落盘。若      
		 * fsync() 失败，我们无法确定改动是否已落盘。目    
		 * 前我们以 panic 处理；               
		 * StartupReplicationSlots() 会在重启后尝试理清        
		 * 残局  
		 */
		START_CRIT_SECTION();
		fsync_fname(tmppath, true);
		fsync_fname("pg_replslot", true);
		END_CRIT_SECTION();
	}
	else
	{
		bool		fail_softly = slot->data.persistency != RS_PERSISTENT;

		SpinLockAcquire(&slot->mutex);
		slot->active_pid = 0;
		SpinLockRelease(&slot->mutex);

		/* 唤醒任何等待此槽的进程   */
		ConditionVariableBroadcast(&slot->active_cv);

		ereport(fail_softly ? WARNING : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						path, tmppath)));
	}

	/*
	 * 槽肯定已经消失。要锁定并发数组扫描足够长的     
	 * 时间以便将其清除。这里无需获取互斥锁即可清      
	 * 除活跃 PID，因为没有其他人能在此处扫描数组，      
	 * 也没有人能不扫描数组就附加到该槽并访问         
	 * 它。             
	 *
	 * 同时唤醒等待它的进程。     
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
	slot->active_pid = 0;
	slot->in_use = false;
	LWLockRelease(ReplicationSlotControlLock);
	ConditionVariableBroadcast(&slot->active_cv);

	/*
	 * 槽已消亡，不再阻止资源移除，需重新计算           
	 * 限额 
	 */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();

	/*
	 * 若删除目录失败，最坏的后果不过是用户在下一次  
	 * 服务器重启之前无法创建同名的新槽。我们会就此 
	 * 发出警告，仅此而已。                                   
	 */
	if (!rmtree(tmppath, true))
		ereport(WARNING,
				(errmsg("could not remove directory \"%s\"", tmppath)));

	/*
	 * 删除该复制槽的统计条目。此操作必须在持有      
	 * ReplicationSlotAllocationLock 的情况下进行，以免    
	 * 误删另一个会话刚刚用同名创建的其他槽的统计     
	 * 条目。       
	 */
	if (SlotIsLogical(slot))
		pgstat_drop_replslot(slot);

	/*
	 * 我们把该锁留到最后才释放，这样在我们还在清理     
	 * 旧槽残骸时，别人就无法开始尝试创建新槽。     
	 */
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * 将当前已获取槽的状态从内存序列化到磁盘，从而        
 * 保证当前状态能在崩溃后幸存。          
 */
void
ReplicationSlotSave(void)
{
	char		path[MAXPGPATH];

	Assert(MyReplicationSlot != NULL);

	sprintf(path, "pg_replslot/%s", NameStr(MyReplicationSlot->data.name));
	SaveSlotToPath(MyReplicationSlot, path, ERROR);
}

/*
 * 发出信号，表明将当前已获取槽刷写到磁盘可能会    
 * 有益。           
 *
 * 注意，实际刷写到磁盘可能延迟很久；如果正确性   
 * 要求如此，请显式执行 ReplicationSlotSave()。        
 */
void
ReplicationSlotMarkDirty(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(MyReplicationSlot != NULL);

	SpinLockAcquire(&slot->mutex);
	MyReplicationSlot->just_dirtied = true;
	MyReplicationSlot->dirty = true;
	SpinLockRelease(&slot->mutex);
}

/*
 * 把标记为 RS_EPHEMERAL 的槽转换为 RS_PERSISTENT 槽，       
 * 确保它在随后发生的崩溃后仍然存在。   
 */
void
ReplicationSlotPersist(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(slot->data.persistency != RS_PERSISTENT);

	SpinLockAcquire(&slot->mutex);
	slot->data.persistency = RS_PERSISTENT;
	SpinLockRelease(&slot->mutex);

	ReplicationSlotMarkDirty();
	ReplicationSlotSave();
}

/*
 * 计算所有槽中最老的 xmin 并存入 ProcArray。                
 *
 * 若 already_locked 为 true，ProcArrayLock 已被独占          
 * 获取。   
 */
void
ReplicationSlotsComputeRequiredXmin(bool already_locked)
{
	int			i;
	TransactionId agg_xmin = InvalidTransactionId;
	TransactionId agg_catalog_xmin = InvalidTransactionId;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		TransactionId effective_xmin;
		TransactionId effective_catalog_xmin;
		bool		invalidated;

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		effective_xmin = s->effective_xmin;
		effective_catalog_xmin = s->effective_catalog_xmin;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* 已失效的槽无需参与      */
		if (invalidated)
			continue;

		/* 检查数据 xmin   */
		if (TransactionIdIsValid(effective_xmin) &&
			(!TransactionIdIsValid(agg_xmin) ||
			 TransactionIdPrecedes(effective_xmin, agg_xmin)))
			agg_xmin = effective_xmin;

		/* 检查系统表 xmin   */
		if (TransactionIdIsValid(effective_catalog_xmin) &&
			(!TransactionIdIsValid(agg_catalog_xmin) ||
			 TransactionIdPrecedes(effective_catalog_xmin, agg_catalog_xmin)))
			agg_catalog_xmin = effective_catalog_xmin;
	}

	LWLockRelease(ReplicationSlotControlLock);

	ProcArraySetReplicationSlotXmin(agg_xmin, agg_catalog_xmin, already_locked);
}

/*
 * 计算所有槽中最老的 restart LSN，并告知 xlog 模块。     
 *
 * 注：虽然 max_slot_wal_keep_size 理论上与此相关，         
 * 但我们并不尝试把它计算在内，因为本模块不知道    
 * 该与什么作比较。     
 */
void
ReplicationSlotsComputeRequiredLSN(void)
{
	int			i;
	XLogRecPtr	min_required = InvalidXLogRecPtr;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		XLogRecPtr	restart_lsn;
		bool		invalidated;

		if (!s->in_use)
			continue;

		SpinLockAcquire(&s->mutex);
		restart_lsn = s->data.restart_lsn;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* 已失效的槽无需参与      */
		if (invalidated)
			continue;

		if (restart_lsn != InvalidXLogRecPtr &&
			(min_required == InvalidXLogRecPtr ||
			 restart_lsn < min_required))
			min_required = restart_lsn;
	}
	LWLockRelease(ReplicationSlotControlLock);

	XLogSetReplicationSlotMinimumLSN(min_required);
}

/*
 * 计算*逻辑*解码槽所需的最老 WAL LSN。                
 *
 * 若逻辑解码被禁用或无逻辑槽则返回 InvalidXLogRecPtr     
 *             
 *
 * 注意：此函数返回值 >= ReplicationSlotsComputeRequiredLSN()，    
 * 因其忽略了物理复制槽。  
 *
 * 此结果无需频繁获取，所以我们不维护预计算值，        
 * 不像 ComputeRequiredLSN() 和 ComputeRequiredXmin() 那样。     
 */
XLogRecPtr
ReplicationSlotsComputeLogicalRestartLSN(void)
{
	XLogRecPtr	result = InvalidXLogRecPtr;
	int			i;

	if (max_replication_slots <= 0)
		return InvalidXLogRecPtr;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;
		XLogRecPtr	restart_lsn;
		bool		invalidated;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* 持有 ReplicationSlotCtlLock 期间无法变更   */
		if (!s->in_use)
			continue;

		/* 我们只关心逻辑槽               */
		if (!SlotIsLogical(s))
			continue;

		/* 只读一次；检查期间它增大也没关系        */
		SpinLockAcquire(&s->mutex);
		restart_lsn = s->data.restart_lsn;
		invalidated = s->data.invalidated != RS_INVAL_NONE;
		SpinLockRelease(&s->mutex);

		/* 已失效的槽无需参与      */
		if (invalidated)
			continue;

		if (restart_lsn == InvalidXLogRecPtr)
			continue;

		if (result == InvalidXLogRecPtr ||
			restart_lsn < result)
			result = restart_lsn;
	}

	LWLockRelease(ReplicationSlotControlLock);

	return result;
}

/*
 * ReplicationSlotsCountDBSlots -- 统计引用所传数据库                
 * oid 的槽数。    
 *
 * 若存在任何引用该数据库的槽则返回 true。*nslots 将       
 * 被设为该数据库中槽的总数，*nactive 设为其中当前      
 * 活跃槽数。  
 */
bool
ReplicationSlotsCountDBSlots(Oid dboid, int *nslots, int *nactive)
{
	int			i;

	*nslots = *nactive = 0;

	if (max_replication_slots <= 0)
		return false;

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* 持有 ReplicationSlotCtlLock 期间无法变更   */
		if (!s->in_use)
			continue;

		/* 只有逻辑槽是数据库专属的，跳过  */
		if (!SlotIsLogical(s))
			continue;

		/* 非本库，跳过     */
		if (s->data.database != dboid)
			continue;

		/* 注意：有意计入已失效的槽         */

		/* 持自旋锁期间统计槽    */
		SpinLockAcquire(&s->mutex);
		(*nslots)++;
		if (s->active_pid != 0)
			(*nactive)++;
		SpinLockRelease(&s->mutex);
	}
	LWLockRelease(ReplicationSlotControlLock);

	if (*nslots > 0)
		return true;
	return false;
}

/*
 * ReplicationSlotsDropDBSlots -- 删除与传入数据库 oid 相关的    
 * 全部数据库专属槽。调用者应在 pg_database 上持有    
 * 该库的排他锁，以防止在该库上创建新槽或                  
 * 从既有槽重放。         
 *
 * 另一会话并发获取目标库上已有槽（多半要删除它）时    
 * 可能令本函数报错。若发生这种情况                          
 * 则可能已删掉部分槽。             
 *
 * 本例程的效率并非最优，但我们并不常删除         
 * 数据库，尤其是拥有大量槽的数据库。      
 */
void
ReplicationSlotsDropDBSlots(Oid dboid)
{
	int			i;

	if (max_replication_slots <= 0)
		return;

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s;
		char	   *slotname;
		int			active_pid;

		s = &ReplicationSlotCtl->replication_slots[i];

		/* 持有 ReplicationSlotCtlLock 期间无法变更   */
		if (!s->in_use)
			continue;

		/* 只有逻辑槽是数据库专属的，跳过  */
		if (!SlotIsLogical(s))
			continue;

		/* 非本库，跳过     */
		if (s->data.database != dboid)
			continue;

		/* 注意：有意包含已失效的槽          */

		/* 获取槽，以便复用 ReplicationSlotDropAcquired        */
		SpinLockAcquire(&s->mutex);
		/* 持有 ReplicationSlotControlLock 期间无法变更  */
		slotname = NameStr(s->data.name);
		active_pid = s->active_pid;
		if (active_pid == 0)
		{
			MyReplicationSlot = s;
			s->active_pid = MyProcPid;
		}
		SpinLockRelease(&s->mutex);

		/*
		 * 即使持有数据库对象的排他锁，该库的           
		 * 逻辑槽仍可能处于活跃状态，例如若它正    
		 * 被连接到另一数据库的后端并发删除。             
		 *
		 * 实际中这种情况相当少见，直接退出即可。  
		 */
		if (active_pid)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication slot \"%s\" is active for PID %d",
							slotname, active_pid)));

		/*
		 * 为避免重复 ReplicationSlotDropAcquired() 的实现，     
		 * 并避免在文件系统操作期间持有锁，先释放     
		 * ReplicationSlotControlLock 并改用      
		 * ReplicationSlotDropAcquired 
		 *
		 * 这意味着槽集合可能变化，因此每次释放锁后      
		 * 都要从头重新开始扫描。       
		 */
		LWLockRelease(ReplicationSlotControlLock);
		ReplicationSlotDropAcquired();
		goto restart;
	}
	LWLockRelease(ReplicationSlotControlLock);
}


/*
 * 检查服务器的配置是否支持使用复制                   
 * 槽   
 */
void
CheckSlotRequirements(void)
{
	/*
	 * 注意：新增要求很可能意味着 RestoreSlotFromDisk()       
	 * 同样需检查。   
	 */

	if (max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if max_replication_slots > 0")));

	if (wal_level < WAL_LEVEL_REPLICA)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if wal_level >= replica")));
}

/*
 * 检查用户是否具有使用复制槽的权限。           
 */
void
CheckSlotPermissions(void)
{
	if (!has_rolreplication(GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to use replication slots"),
				 errdetail("Only roles with the %s attribute may use replication slots.",
						   "REPLICATION")));
}

/*
 * 为当前活跃的槽保留 WAL。        
 *
 * 以适合该槽类型的方式计算并设置 restart_lsn，              
 * 且并发安全。            
 */
void
ReplicationSlotReserveWal(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL);
	Assert(slot->data.restart_lsn == InvalidXLogRecPtr);

	/*
	 * 复制槽机制用于防止所需的 WAL 被移除。                
	 * 由于本例程与检查点之间不存在互锁，WAL 段             
	 * 可能在已过期的 ReplicationSlotsComputeRequiredLSN() 返回值   
	 * 被使用时被并发移除。万一真的发生这种情况，        
	 * 我们只需重试。         
	 */
	while (true)
	{
		XLogSegNo	segno;
		XLogRecPtr	restart_lsn;

		/*
		 * 对逻辑槽，会记录一份备库快照并在该位置          
		 * 精确启动逻辑解码。这样能让槽更快地启动。   
		 * 但在备库上我们无法执行 WAL 写入，因此只能使用 
		 * 重放指针；实际上，在备库上创建逻辑槽的尝试， 
		 * 会使其等待主库上独立记录的 xl_running_xact          
		 * 记录，以便可以                                         
		 * 据此构建快照。  
		 *
		 * 对物理槽来说这些都不需要（实际上也无益）， 
		 * 因为它们反正会从最近记录的检查点开始重放。   
		 * 改为返回最后 redo LSN 的位置。                     
		 * 虽会略微增加重试几率，但那是基础备份       
		 * 必须重放的起点。      
		 */
		if (SlotIsPhysical(slot))
			restart_lsn = GetRedoRecPtr();
		else if (RecoveryInProgress())
			restart_lsn = GetXLogReplayRecPtr(NULL);
		else
			restart_lsn = GetXLogInsertRecPtr();

		SpinLockAcquire(&slot->mutex);
		slot->data.restart_lsn = restart_lsn;
		SpinLockRelease(&slot->mutex);

		/* 尽快阻止 WAL 被移除              */
		ReplicationSlotsComputeRequiredLSN();

		/*
		 * 若所需的 WAL 仍在，很好，否则重试。             
		 * 该槽应能阻止 WAL 进一步被移除，除非在       
		 * 我们在上文写入新的 restart_lsn 之后又有并发的      
		 * ReplicationSlotsComputeRequiredLSN()，故通常无需循环       
		 * 超过两轮。 
		 */
		XLByteToSeg(slot->data.restart_lsn, segno, wal_segment_size);
		if (XLogGetLastRemovedSegno() < segno)
			break;
	}

	if (!RecoveryInProgress() && SlotIsLogical(slot))
	{
		XLogRecPtr	flushptr;

		/* 确保已拥有足够的信息来启动       */
		flushptr = LogStandbySnapshot();

		/* 并确保已将其 fsync 到磁盘 */
		XLogFlush(flushptr);
	}
}

/*
 * 报告复制槽需要被置为失效                
 */
static void
ReportSlotInvalidation(ReplicationSlotInvalidationCause cause,
					   bool terminating,
					   int pid,
					   NameData slotname,
					   XLogRecPtr restart_lsn,
					   XLogRecPtr oldestLSN,
					   TransactionId snapshotConflictHorizon)
{
	StringInfoData err_detail;
	bool		hint = false;

	initStringInfo(&err_detail);

	switch (cause)
	{
		case RS_INVAL_WAL_REMOVED:
			{
				unsigned long long ex = oldestLSN - restart_lsn;

				hint = true;
				appendStringInfo(&err_detail,
								 ngettext("The slot's restart_lsn %X/%X exceeds the limit by %llu byte.",
										  "The slot's restart_lsn %X/%X exceeds the limit by %llu bytes.",
										  ex),
								 LSN_FORMAT_ARGS(restart_lsn),
								 ex);
				break;
			}
		case RS_INVAL_HORIZON:
			appendStringInfo(&err_detail, _("The slot conflicted with xid horizon %u."),
							 snapshotConflictHorizon);
			break;

		case RS_INVAL_WAL_LEVEL:
			appendStringInfo(&err_detail, _("Logical decoding on standby requires wal_level >= logical on the primary server."));
			break;
		case RS_INVAL_NONE:
			pg_unreachable();
	}

	ereport(LOG,
			terminating ?
			errmsg("terminating process %d to release replication slot \"%s\"",
				   pid, NameStr(slotname)) :
			errmsg("invalidating obsolete replication slot \"%s\"",
				   NameStr(slotname)),
			errdetail_internal("%s", err_detail.data),
			hint ? errhint("You might need to increase %s.", "max_slot_wal_keep_size") : 0);

	pfree(err_detail.data);
}

/*
 * InvalidateObsoleteReplicationSlots 的助手 
 *
 * 在必要且可能时获取给定的槽并将其标记为失效。     
 *
 * 返回期间是否释放过 ReplicationSlotControlLock（若释放过，   
 * 则返回时不再持有该锁，否则仍持有）。               
 *
 * 槽已失效时将 *invalidated 置为 true（否则不动）。          
 *
 * 这本质上有竞态：为系统调用会释放      
 * LWLock，返回 true 时调用者必须重来。       
 */
static bool
InvalidatePossiblyObsoleteSlot(ReplicationSlotInvalidationCause cause,
							   ReplicationSlot *s,
							   XLogRecPtr oldestLSN,
							   Oid dboid, TransactionId snapshotConflictHorizon,
							   bool *invalidated)
{
	int			last_signaled_pid = 0;
	bool		released_lock = false;

	for (;;)
	{
		XLogRecPtr	restart_lsn;
		NameData	slotname;
		int			active_pid = 0;
		ReplicationSlotInvalidationCause conflict = RS_INVAL_NONE;

		Assert(LWLockHeldByMeInMode(ReplicationSlotControlLock, LW_SHARED));

		if (!s->in_use)
		{
			if (released_lock)
				LWLockRelease(ReplicationSlotControlLock);
			break;
		}

		/*
		 * 检查该槽是否需要失效。若需要失效，         
		 * 且当前未被获取，则获取它并标记为已失效。      
		 * 这一步在持有自旋锁时进行，以                       
		 * 避免竞态条件——例如 restart_lsn 可能前移，     
		 * 或者该槽可能被删除。        
		 */
		SpinLockAcquire(&s->mutex);

		restart_lsn = s->data.restart_lsn;

		/*
		 * 若该槽已失效或是不冲突的槽，我们               
		 * 无需做任何事。     
		 */
		if (s->data.invalidated == RS_INVAL_NONE)
		{
			switch (cause)
			{
				case RS_INVAL_WAL_REMOVED:
					if (s->data.restart_lsn != InvalidXLogRecPtr &&
						s->data.restart_lsn < oldestLSN)
						conflict = cause;
					break;
				case RS_INVAL_HORIZON:
					if (!SlotIsLogical(s))
						break;
					/* 无效的 DB oid 表示共享关系      */
					if (dboid != InvalidOid && dboid != s->data.database)
						break;
					if (TransactionIdIsValid(s->effective_xmin) &&
						TransactionIdPrecedesOrEquals(s->effective_xmin,
													  snapshotConflictHorizon))
						conflict = cause;
					else if (TransactionIdIsValid(s->effective_catalog_xmin) &&
							 TransactionIdPrecedesOrEquals(s->effective_catalog_xmin,
														   snapshotConflictHorizon))
						conflict = cause;
					break;
				case RS_INVAL_WAL_LEVEL:
					if (SlotIsLogical(s))
						conflict = cause;
					break;
				case RS_INVAL_NONE:
					pg_unreachable();
			}
		}

		/* 若无冲突，即告完成        */
		if (conflict == RS_INVAL_NONE)
		{
			SpinLockRelease(&s->mutex);
			if (released_lock)
				LWLockRelease(ReplicationSlotControlLock);
			break;
		}

		slotname = s->data.name;
		active_pid = s->active_pid;

		/*
		 * 若能获取该槽，则获取它并立即                
		 * 标记为已失效。否则将在下文向持有者发信号并重 
		 * 试   
		 */
		if (active_pid == 0)
		{
			MyReplicationSlot = s;
			s->active_pid = MyProcPid;
			s->data.invalidated = conflict;

			/*
			 * XXX: 我们应考虑不覆写 restart_lsn，而是改为       
			 * 仅依赖 .invalidated。 
			 */
			if (conflict == RS_INVAL_WAL_REMOVED)
				s->data.restart_lsn = InvalidXLogRecPtr;

			/* 告知调用者 */
			*invalidated = true;
		}

		SpinLockRelease(&s->mutex);

		if (active_pid != 0)
		{
			/*
			 * 在释放锁之前先在槽的条件变量上            
			 * 以关闭一种可能的竞态：若槽在下方睡眠       
			 * 之前就被释放。                   
			 */
			ConditionVariablePrepareToSleep(&s->active_cv);

			LWLockRelease(ReplicationSlotControlLock);
			released_lock = true;

			/*
			 * 若尚未向拥有该槽的进程发信号，则向它   
			 * 发信号令其终止。（避免重复发信号     
			 * 是本例程中存在循环的唯一原因；               
			 * 否则本可依赖调用者的重启循环。）           
			 *
			 * 存在这样的竞态：其他进程可能在当前            
			 * 拥有者进程结束后、本进程获得它之前          
			 * 拥有该槽。为处理这种情况，只有当             
			 * 拥有者进程的 PID 相对上次发生了变化时       
			 * 才发信号。（假定 PID 不会很快被复用。）    
			 */
			if (last_signaled_pid != active_pid)
			{
				ReportSlotInvalidation(conflict, true, active_pid,
									   slotname, restart_lsn,
									   oldestLSN, snapshotConflictHorizon);

				if (MyBackendType == B_STARTUP)
					(void) SendProcSignal(active_pid,
										  PROCSIG_RECOVERY_CONFLICT_LOGICALSLOT,
										  InvalidBackendId);
				else
					(void) kill(active_pid, SIGTERM);

				last_signaled_pid = active_pid;
			}

			/* 等待槽被释放。            */
			ConditionVariableSleep(&s->active_cv,
								   WAIT_EVENT_REPLICATION_SLOT_DROP);

			/*
			 * 重新获取锁并从头再来；我们预期下次        
			 * 就能使该槽失效（除非期间另一进程获取了      
			 * 该槽） 
			 *
			 * 注意：槽可能在释放互斥锁与重新检查之间， 
			 * 将 restart_lsn 或 xmin 值推进到足够程度，          
			 * 从而从冲突状态变为非冲突状态。            
			 * 这是有意为之且安全的：如果槽在我们忙于    
			 * 此处期间已经赶上，那么我们所关心的资源 
			 * （WAL 段或元组）尚未被移除，                  
			 * 因此没有理由使该槽失效。                  
			 */
			LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
			continue;
		}
		else
		{
			/*
			 * 我们现在持有该槽且已使其失效；请刷写它     
			 * 以确保状态持久化。   
			 *
			 * 勿跨文件操作持有 ReplicationSlotControlLock，   
			 * 因此现在就释放它，但务必告知调用者            
			 * 从头重来。         
			 */
			LWLockRelease(ReplicationSlotControlLock);
			released_lock = true;

			/* 确保已失效状态能在服务器重启后持久保留      */
			ReplicationSlotMarkDirty();
			ReplicationSlotSave();
			ReplicationSlotRelease();
			pgstat_drop_replslot(s);

			ReportSlotInvalidation(conflict, false, active_pid,
								   slotname, restart_lsn,
								   oldestLSN, snapshotConflictHorizon);

			/* 此槽暂告一段落       */
			break;
		}
	}

	Assert(released_lock == !LWLockHeldByMe(ReplicationSlotControlLock));

	return released_lock;
}

/*
 * 使需要即将被移除的资源的槽失效。            
 *
 * 若有任何槽已失效则返回 true。       
 *
 * 是否需要失效取决于原因。下列情况槽会被             
 * 删除：     
 * - RS_INVAL_WAL_REMOVED: 需要比给定段更旧的 LSN            
 * - RS_INVAL_HORIZON: 在给定库中需要快照 <= 给定视界；       
 *   dboid 对共享关系可为 InvalidOid          
 * - RS_INVAL_WAL_LEVEL: 为逻辑 
 *
 * 注意：这在检查点中运行，尽可能避免报错。              
 */
bool
InvalidateObsoleteReplicationSlots(ReplicationSlotInvalidationCause cause,
								   XLogSegNo oldestSegno, Oid dboid,
								   TransactionId snapshotConflictHorizon)
{
	XLogRecPtr	oldestLSN;
	bool		invalidated = false;

	Assert(cause != RS_INVAL_HORIZON || TransactionIdIsValid(snapshotConflictHorizon));
	Assert(cause != RS_INVAL_WAL_REMOVED || oldestSegno > 0);
	Assert(cause != RS_INVAL_NONE);

	if (max_replication_slots == 0)
		return invalidated;

	XLogSegNoOffsetToRecPtr(oldestSegno, 0, wal_segment_size, oldestLSN);

restart:
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (int i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (!s->in_use)
			continue;

		if (InvalidatePossiblyObsoleteSlot(cause, s, oldestLSN, dboid,
										   snapshotConflictHorizon,
										   &invalidated))
		{
			/* 若锁曾被释放，则从头开始         */
			goto restart;
		}
	}
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * 若有槽已失效，则重新计算资源限制。                 
	 */
	if (invalidated)
	{
		ReplicationSlotsComputeRequiredXmin(false);
		ReplicationSlotsComputeRequiredLSN();
	}

	return invalidated;
}

/*
 * 把全部复制槽刷盘。         
 *
 * 这其实不必作为检查点的一部分，但此处              
 * 方便   
 */
void
CheckPointReplicationSlots(void)
{
	int			i;

	elog(DEBUG1, "performing replication slot checkpoint");

	/*
	 * 在我们活跃期间阻止任何槽被创建/删除。由于        
	 * 明确 *不* 想阻塞对 replication_slots 的遍历或             
	 * 获取槽，故我们无法拿控制锁——但没关系，     
	 * 因为持有 ReplicationSlotAllocationLock 严格更强，             
	 * 足以保证没有人能在我们眼皮下改动 in_use 位。   
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		char		path[MAXPGPATH];

		if (!s->in_use)
			continue;

		/* 把槽保存到磁盘，加锁在 SaveSlotToPath() 中处理  */
		sprintf(path, "pg_replslot/%s", NameStr(s->data.name));
		SaveSlotToPath(s, path, LOG);
	}
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * 在服务器启动时把所有复制槽从磁盘加载进内存。      
 * 这需要在开始崩溃恢复之前运行。  
 */
void
StartupReplicationSlots(void)
{
	DIR		   *replication_dir;
	struct dirent *replication_de;

	elog(DEBUG1, "starting up replication slots");

	/* 通过遍历所有磁盘上的条目来恢复全部槽  */
	replication_dir = AllocateDir("pg_replslot");
	while ((replication_de = ReadDir(replication_dir, "pg_replslot")) != NULL)
	{
		char		path[MAXPGPATH + 12];
		PGFileType	de_type;

		if (strcmp(replication_de->d_name, ".") == 0 ||
			strcmp(replication_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "pg_replslot/%s", replication_de->d_name);
		de_type = get_dirent_type(path, replication_de, false, DEBUG1);

		/* 这里只是创建目录，若不是我们的目录则跳过 */
		if (de_type != PGFILETYPE_ERROR && de_type != PGFILETYPE_DIR)
			continue;

		/* 槽正在建立或删除时发生崩溃，进行清理       */
		if (pg_str_endswith(replication_de->d_name, ".tmp"))
		{
			if (!rmtree(path, true))
			{
				ereport(WARNING,
						(errmsg("could not remove directory \"%s\"",
								path)));
				continue;
			}
			fsync_fname("pg_replslot", true);
			continue;
		}

		/* 看似处于正常状态的槽，予以恢复*/
		RestoreSlotFromDisk(replication_de->d_name);
	}
	FreeDir(replication_dir);

	/* 当前不存在任何槽，完成。  */
	if (max_replication_slots <= 0)
		return;

	/* 现已恢复全部数据，接下来计算复制 xmin             */
	ReplicationSlotsComputeRequiredXmin(false);
	ReplicationSlotsComputeRequiredLSN();
}

/* ----
 * 复制槽磁盘状态的操纵                    
 *
 * 注意：下述例程都不应关心某个槽是否是当前槽，         
 * 这些都由上一层负责处理。                 
 * ----
 */
static void
CreateSlotOnDisk(ReplicationSlot *slot)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	struct stat st;

	/*
	 * 无需获取 io_in_progress_lock，还没有别人能看到          
	 * 该槽，所以别人也不会写。我们复用了 SaveSlotToPath，  
	 * 而它取锁；若我们在此取锁就会死锁。           
	 */

	sprintf(path, "pg_replslot/%s", NameStr(slot->data.name));
	sprintf(tmppath, "pg_replslot/%s.tmp", NameStr(slot->data.name));

	/*
	 * 有一种极小的可能：先前创建或删除同名槽的           
	 * 尝试遗留了一个临时目录。如果看起来是这样，      
	 * 就尝试删除它。如果 rmtree() 失败，我们会在           
	 * 下面的 MakePGDirectory() 处报错，因此无需检查     
	 * 成败  
	 */
	if (stat(tmppath, &st) == 0 && S_ISDIR(st.st_mode))
		rmtree(tmppath, true);

	/* 创建临时槽目录并将其 fsync。        */
	if (MakePGDirectory(tmppath) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						tmppath)));
	fsync_fname(tmppath, true);

	/* 写入实际状态文件。  */
	slot->dirty = true;			/* 表示我们确实需要写         */
	SaveSlotToPath(slot, tmppath, ERROR);

	/* 把目录重命名到位。      */
	if (rename(tmppath, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));

	/*
	 * 如果此刻失败——可能性极小——我们将无从得知     
	 * 该槽崩溃后能否存续——所以强制重启。           
	 * 重启会反复尝试 fsync 直到成功。         
	 */
	START_CRIT_SECTION();

	fsync_fname(path, true);
	fsync_fname("pg_replslot", true);

	END_CRIT_SECTION();
}

/*
 * 保存与创建复制槽共用的功能。                          
 */
static void
SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	int			fd;
	ReplicationSlotOnDisk cp;
	bool		was_dirty;

	/* 先检查是否有内容需要写出               */
	SpinLockAcquire(&slot->mutex);
	was_dirty = slot->dirty;
	slot->just_dirtied = false;
	SpinLockRelease(&slot->mutex);

	/* 若无内容可写则什么都不做              */
	if (!was_dirty)
		return;

	LWLockAcquire(&slot->io_in_progress_lock, LW_EXCLUSIVE);

	/* 安抚 valgrind :(  */
	memset(&cp, 0, sizeof(ReplicationSlotOnDisk));

	sprintf(tmppath, "%s/state.tmp", dir);
	sprintf(path, "%s/state", dir);

	fd = OpenTransientFile(tmppath, O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (fd < 0)
	{
		/*
		 * 若非 ERROR，则在返回前释放锁。                       
		 * 若是 ERROR，错误恢复路径会自动释放锁，         
		 * 但即便在这种情况下显式释放也无妨。注意         
		 * LWLockRelease() 可能影响 errno。   
		 */
		int			save_errno = errno;

		LWLockRelease(&slot->io_in_progress_lock);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						tmppath)));
		return;
	}

	cp.magic = SLOT_MAGIC;
	INIT_CRC32C(cp.checksum);
	cp.version = SLOT_VERSION;
	cp.length = ReplicationSlotOnDiskV2Size;

	SpinLockAcquire(&slot->mutex);

	memcpy(&cp.slotdata, &slot->data, sizeof(ReplicationSlotPersistentData));

	SpinLockRelease(&slot->mutex);

	COMP_CRC32C(cp.checksum,
				(char *) (&cp) + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(cp.checksum);

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_WRITE);
	if ((write(fd, &cp, sizeof(cp))) != sizeof(cp))
	{
		int			save_errno = errno;

		pgstat_report_wait_end();
		CloseTransientFile(fd);
		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		/* 若写入未设 errno，则假定为磁盘空间不足     */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
		return;
	}
	pgstat_report_wait_end();

	/* fsync 临时文件       */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		pgstat_report_wait_end();
		CloseTransientFile(fd);
		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						tmppath)));
		return;
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
	{
		int			save_errno = errno;

		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						tmppath)));
		return;
	}

	/* 重命名为永久文件，fsync 文件和目录   */
	if (rename(tmppath, path) != 0)
	{
		int			save_errno = errno;

		unlink(tmppath);
		LWLockRelease(&slot->io_in_progress_lock);

		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));
		return;
	}

	/*
	 * 使用临界区的理由参见 CreateSlotOnDisk()。                   
	 */
	START_CRIT_SECTION();

	fsync_fname(path, false);
	fsync_fname(dir, true);
	fsync_fname("pg_replslot", true);

	END_CRIT_SECTION();

	/*
	 * 写入成功后清除脏位，除非已有人再次               
	 * 弄脏  
	 */
	SpinLockAcquire(&slot->mutex);
	if (!slot->just_dirtied)
		slot->dirty = false;
	SpinLockRelease(&slot->mutex);

	LWLockRelease(&slot->io_in_progress_lock);
}

/*
 * 把单个槽从磁盘加载进内存。  
 */
static void
RestoreSlotFromDisk(const char *name)
{
	ReplicationSlotOnDisk cp;
	int			i;
	char		slotdir[MAXPGPATH + 12];
	char		path[MAXPGPATH + 22];
	int			fd;
	bool		restored = false;
	int			readBytes;
	pg_crc32c	checksum;

	/* 此处无需加锁，尚不允许并发访问          */

	/* 临时文件存在则删除   */
	sprintf(slotdir, "pg_replslot/%s", name);
	sprintf(path, "%s/state.tmp", slotdir);
	if (unlink(path) < 0 && errno != ENOENT)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", path)));

	sprintf(path, "%s/state", slotdir);

	elog(DEBUG1, "restoring replication slot from \"%s\"", path);

	/* 在某些操作系统上 fsync 文件需要 O_RDWR        */
	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);

	/*
	 * 我们无需处理这个，因为我们只在状态文件被          
	 * fsync() 之后才把目录重命名到位。 
	 */
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/*
	 * 在读取状态文件之前先同步它。我们可能曾          
	 * 在它尚未被同步时崩溃，且不应基于该状态继续。 
	 */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_RESTORE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						path)));
	pgstat_report_wait_end();

	/* 也同步父目录             */
	START_CRIT_SECTION();
	fsync_fname(slotdir, true);
	END_CRIT_SECTION();

	/* 读取状态文件中保证与版本无关的那部分             */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_READ);
	readBytes = read(fd, &cp, ReplicationSlotOnDiskConstantSize);
	pgstat_report_wait_end();
	if (readBytes != ReplicationSlotOnDiskConstantSize)
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes,
							(Size) ReplicationSlotOnDiskConstantSize)));
	}

	/* 校验魔数 */
	if (cp.magic != SLOT_MAGIC)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has wrong magic number: %u instead of %u",
						path, cp.magic, SLOT_MAGIC)));

	/* 校验版本   */
	if (cp.version != SLOT_VERSION)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has unsupported version %u",
						path, cp.version)));

	/* 对长度做边界检查 */
	if (cp.length != ReplicationSlotOnDiskV2Size)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot file \"%s\" has corrupted length %u",
						path, cp.length)));

	/* 现已知道大小，读取整个文件         */
	pgstat_report_wait_start(WAIT_EVENT_REPLICATION_SLOT_READ);
	readBytes = read(fd,
					 (char *) &cp + ReplicationSlotOnDiskConstantSize,
					 cp.length);
	pgstat_report_wait_end();
	if (readBytes != cp.length)
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, (Size) cp.length)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	/* 现在校验 CRC   */
	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				(char *) &cp + ReplicationSlotOnDiskNotChecksummedSize,
				ReplicationSlotOnDiskChecksummedSize);
	FIN_CRC32C(checksum);

	if (!EQ_CRC32C(checksum, cp.checksum))
		ereport(PANIC,
				(errmsg("checksum mismatch for replication slot file \"%s\": is %u, should be %u",
						path, checksum, cp.checksum)));

	/*
	 * 若崩溃时有瞬态槽活跃，不要恢复而是删除它         
	 * .  
	 */
	if (cp.slotdata.persistency != RS_PERSISTENT)
	{
		if (!rmtree(slotdir, true))
		{
			ereport(WARNING,
					(errmsg("could not remove directory \"%s\"",
							slotdir)));
		}
		fsync_fname("pg_replslot", true);
		return;
	}

	/*
	 * 校验特定槽类型的前置条件是否满足。                
	 * 这很重要，因为若不满足，就无法保证为该槽        
	 * 保留全部所需资源。              
	 *
	 * 注意：瞬态槽必须在上述检查 *之后* 做，           
	 * 否则一个本不应再存在的槽可能会阻碍                
	 * 重启   
	 *
	 * 注意：在此改动前置条件还需相应调整      
	 * CheckSlotRequirements() 与 CheckLogicalDecodingRequirements() 
	 */
	if (cp.slotdata.database != InvalidOid)
	{
		if (wal_level < WAL_LEVEL_LOGICAL)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication slot \"%s\" exists, but wal_level < logical",
							NameStr(cp.slotdata.name)),
					 errhint("Change wal_level to be logical or higher.")));

		/*
		 * 备库模式下必须启用热备。此检查                  
		 * 旨在确保逻辑槽因 wal_level 不足而变得不兼容时    
		 * 会失效。否则，若主库在热备禁用时把          
		 * wal_level 降到 logical 之下，逻辑槽在提升后           
		 * 仍会保持有效。                                 
		 */
		if (StandbyMode && !EnableHotStandby)
			ereport(FATAL,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("logical replication slot \"%s\" exists on the standby, but hot_standby = off",
							NameStr(cp.slotdata.name)),
					 errhint("Change hot_standby to be on.")));
	}
	else if (wal_level < WAL_LEVEL_REPLICA)
		ereport(FATAL,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("physical replication slot \"%s\" exists, but wal_level < replica",
						NameStr(cp.slotdata.name)),
				 errhint("Change wal_level to be replica or higher.")));

	/* 尚不可能有活跃对象，无需加锁     */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot;

		slot = &ReplicationSlotCtl->replication_slots[i];

		if (slot->in_use)
			continue;

		/* 恢复整组持久化数据               */
		memcpy(&slot->data, &cp.slotdata,
			   sizeof(ReplicationSlotPersistentData));

		/* 初始化内存中的状态*/
		slot->effective_xmin = cp.slotdata.xmin;
		slot->effective_catalog_xmin = cp.slotdata.catalog_xmin;

		slot->candidate_catalog_xmin = InvalidTransactionId;
		slot->candidate_xmin_lsn = InvalidXLogRecPtr;
		slot->candidate_restart_lsn = InvalidXLogRecPtr;
		slot->candidate_restart_valid = InvalidXLogRecPtr;

		slot->in_use = true;
		slot->active_pid = 0;

		restored = true;
		break;
	}

	if (!restored)
		ereport(FATAL,
				(errmsg("too many replication slots active before shutdown"),
				 errhint("Increase max_replication_slots and try again.")));
}
