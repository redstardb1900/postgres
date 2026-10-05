/*-------------------------------------------------------------------------
 *
 * session.c
 *		用户会话的封装。
 *
 * 这里存放需要在为同一客户端会话工作的多个后端之间
 * 共享的数据。具体而言，并行查询的领导进程与工作进程
 * 会共享这样一个会话。以后它也可能成为把后端
 * 与客户端连接分开的基础设施，例如用于
 * 连接池。
 *
 * 目前这套基础设施用于共享：
 * - 临时行类型的 typemod 注册表，即 BlessTupleDesc 等。
 *
 * Portions Copyright (c) 2017-2023, PostgreSQL Global Development Group
 *
 * src/backend/access/common/session.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/session.h"
#include "storage/lwlock.h"
#include "storage/shm_toc.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

/* 每会话动态共享内存 TOC 的魔数。 */
#define SESSION_MAGIC						0xabb0fbc9

/*
 * 我们要创建一个 DSA 区，存放与会话寿命相同的共享状态。
 * 到目前为止，它只用来保存共享的记录类型注册表。
 * 我们不希望在常见情况下它立刻就得创建任何动态共享内存段，
 * 因此给它足够的空间，以放下一个很小的
 * SharedRecordTypmodRegistry。
 */
#define SESSION_DSA_SIZE					0x30000

/*
 * 每会话动态共享内存区中用于状态共享的魔数。
 */
#define SESSION_KEY_DSA						UINT64CONST(0xFFFFFFFFFFFF0001)
#define SESSION_KEY_RECORD_TYPMOD_REGISTRY	UINT64CONST(0xFFFFFFFFFFFF0002)

/* 本后端的当前会话。 */
Session    *CurrentSession = NULL;

/*
 * 让 CurrentSession 指向一个空的 Session 对象。
 */
void
InitializeSession(void)
{
	CurrentSession = MemoryContextAllocZero(TopMemoryContext, sizeof(Session));
}

/*
 * 若每会话动态共享内存段尚未初始化，则把它初始化，并
 * 返回其句柄，以便工作进程能够挂接。
 *
 * 与按上下文划分的动态共享内存段不同，本段及其内容
 * 会被以后的并行查询复用。
 *
 * 若因资源不足而无法分配段，则返回
 * DSM_HANDLE_INVALID。
 */
dsm_handle
GetSessionDsmHandle(void)
{
	shm_toc_estimator estimator;
	shm_toc    *toc;
	dsm_segment *seg;
	size_t		typmod_registry_size;
	size_t		size;
	void	   *dsa_space;
	void	   *typmod_registry_space;
	dsa_area   *dsa;
	MemoryContext old_context;

	/*
	 * 若本后端已经创建了会话范围的动态共享内存段，
	 * 就返回其句柄。在本后端的余下生命期内
	 * 都会使用同一段。
	 */
	if (CurrentSession->segment != NULL)
		return dsm_segment_handle(CurrentSession->segment);

	/* 否则，准备建立一个。 */
	old_context = MemoryContextSwitchTo(TopMemoryContext);
	shm_toc_initialize_estimator(&estimator);

	/* 估算每会话 DSA 区所需的空间。 */
	shm_toc_estimate_keys(&estimator, 1);
	shm_toc_estimate_chunk(&estimator, SESSION_DSA_SIZE);

	/* 估算每会话记录 typemod 注册表所需的空间。 */
	typmod_registry_size = SharedRecordTypmodRegistryEstimate();
	shm_toc_estimate_keys(&estimator, 1);
	shm_toc_estimate_chunk(&estimator, typmod_registry_size);

	/* 建立段和 TOC。 */
	size = shm_toc_estimate(&estimator);
	seg = dsm_create(size, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (seg == NULL)
	{
		MemoryContextSwitchTo(old_context);

		return DSM_HANDLE_INVALID;
	}
	toc = shm_toc_create(SESSION_MAGIC,
						 dsm_segment_address(seg),
						 size);

	/* 创建每会话 DSA 区。 */
	dsa_space = shm_toc_allocate(toc, SESSION_DSA_SIZE);
	dsa = dsa_create_in_place(dsa_space,
							  SESSION_DSA_SIZE,
							  LWTRANCHE_PER_SESSION_DSA,
							  seg);
	shm_toc_insert(toc, SESSION_KEY_DSA, dsa_space);


	/* 创建会话范围的共享记录 typemod 注册表。 */
	typmod_registry_space = shm_toc_allocate(toc, typmod_registry_size);
	SharedRecordTypmodRegistryInit((SharedRecordTypmodRegistry *)
								   typmod_registry_space, seg, dsa);
	shm_toc_insert(toc, SESSION_KEY_RECORD_TYPMOD_REGISTRY,
				   typmod_registry_space);

	/*
	 * 若能走到这里，就可以钉住共享内存，使映射在
	 * 本后端的余下生命期内保持。若走不到这里，上面安装的
	 * 任何东西（即目前的 SharedRecordTypmodRegistry）的清理
	 * 回调会在 CurrentResourceOwner 脱离该动态共享内存段时运行，
	 * 这样就不会留下一个损坏的 CurrentSession。
	 */
	dsm_pin_mapping(seg);
	dsa_pin_mapping(dsa);

	/* 经由 CurrentSession 提供该段和该区。 */
	CurrentSession->segment = seg;
	CurrentSession->area = dsa;

	MemoryContextSwitchTo(old_context);

	return dsm_segment_handle(seg);
}

/*
 * 挂接到并行领导进程提供的每会话动态共享内存段。
 */
void
AttachSession(dsm_handle handle)
{
	dsm_segment *seg;
	shm_toc    *toc;
	void	   *dsa_space;
	void	   *typmod_registry_space;
	dsa_area   *dsa;
	MemoryContext old_context;

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* 挂接到该动态共享内存段。 */
	seg = dsm_attach(handle);
	if (seg == NULL)
		elog(ERROR, "could not attach to per-session DSM segment");
	toc = shm_toc_attach(SESSION_MAGIC, dsm_segment_address(seg));

	/* 挂接到该 DSA 区。 */
	dsa_space = shm_toc_lookup(toc, SESSION_KEY_DSA, false);
	dsa = dsa_attach_in_place(dsa_space, seg);

	/* 经由当前会话提供它们。 */
	CurrentSession->segment = seg;
	CurrentSession->area = dsa;

	/* 挂接到共享记录 typemod 注册表。 */
	typmod_registry_space =
		shm_toc_lookup(toc, SESSION_KEY_RECORD_TYPMOD_REGISTRY, false);
	SharedRecordTypmodRegistryAttach((SharedRecordTypmodRegistry *)
									 typmod_registry_space);

	/* 保持挂接，直到后端结束或调用 DetachSession()。 */
	dsm_pin_mapping(seg);
	dsa_pin_mapping(dsa);

	MemoryContextSwitchTo(old_context);
}

/*
 * 脱离当前会话的动态共享内存段。由于后端退出时会自动
 * 脱离，显式这样做并非严格必要；但若将来复用并行
 * 工作进程，工作进程在挂接到另一个会话之前先脱离
 * 当前会话就会变得重要。注意，这里会运行
 * 脱离钩子。
 */
void
DetachSession(void)
{
	/* 运行脱离钩子。 */
	dsm_detach(CurrentSession->segment);
	CurrentSession->segment = NULL;
	dsa_detach(CurrentSession->area);
	CurrentSession->area = NULL;
}
