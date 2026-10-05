/*-------------------------------------------------------------------------
 *
 * nbtpage.c
 *	  Postgres BTree（B-树）访问方法专用的页管理
 *	  代码。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtpage.c
 *
 *	说明（NOTES）
 *	   Postgres 的 B-树页看起来与普通关系页一样。高地址处的不透明
 *	   数据包含指向左右兄弟的指针，
 *	   以及描述页状态的标志数据。B-树的第一页，即第
 *	   零页，是特殊的——它存放描述整棵树的元信息。
 *	   第一页及更高页存放实际的树数据。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/nbtree.h"
#include "access/nbtxlog.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/indexfsm.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "storage/procarray.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

static BTMetaPageData *_bt_getmeta(Relation rel, Buffer metabuf);
static void _bt_delitems_delete(Relation rel, Buffer buf,
								TransactionId snapshotConflictHorizon,
								bool isCatalogRel,
								OffsetNumber *deletable, int ndeletable,
								BTVacuumPosting *updatable, int nupdatable);
static char *_bt_delitems_update(BTVacuumPosting *updatable, int nupdatable,
								 OffsetNumber *updatedoffsets,
								 Size *updatedbuflen, bool needswal);
static bool _bt_mark_page_halfdead(Relation rel, Relation heaprel,
								   Buffer leafbuf, BTStack stack);
static bool _bt_unlink_halfdead_page(Relation rel, Buffer leafbuf,
									 BlockNumber scanblkno,
									 bool *rightsib_empty,
									 BTVacState *vstate);
static bool _bt_lock_subtree_parent(Relation rel, Relation heaprel,
									BlockNumber child, BTStack stack,
									Buffer *subtreeparent, OffsetNumber *poffset,
									BlockNumber *topparent,
									BlockNumber *topparentrightsib);
static void _bt_pendingfsm_add(BTVacState *vstate, BlockNumber target,
							   FullTransactionId safexid);

/*
 *	_bt_initmetapage() -- 用正确的元页映像填充页缓冲区
 */
void
_bt_initmetapage(Page page, BlockNumber rootbknum, uint32 level,
				 bool allequalimage)
{
	BTMetaPageData *metad;
	BTPageOpaque metaopaque;

	_bt_pageinit(page, BLCKSZ);

	metad = BTPageGetMeta(page);
	metad->btm_magic = BTREE_MAGIC;
	metad->btm_version = BTREE_VERSION;
	metad->btm_root = rootbknum;
	metad->btm_level = level;
	metad->btm_fastroot = rootbknum;
	metad->btm_fastlevel = level;
	metad->btm_last_cleanup_num_delpages = 0;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;
	metad->btm_allequalimage = allequalimage;

	metaopaque = BTPageGetOpaque(page);
	metaopaque->btpo_flags = BTP_META;

	/*
	 * 把 pd_lower 设到元数据末尾之后。这一点至关重要，
	 * 因为若不这样做，xlog.c 压缩该页时
	 * 元数据就会丢失。
	 */
	((PageHeader) page)->pd_lower =
		((char *) metad + sizeof(BTMetaPageData)) - (char *) page;
}

/*
 *	_bt_upgrademetapage() -- 把元页从旧格式升级到版本
 *		3，这是不必广泛影响磁盘兼容性就能更新的最后一个版本。
 *		（升级到 v4 需要 REINDEX。）
 *
 *		本例程只在内存中升级映像。调用者
 *		负责加锁、记 WAL 等。
 */
void
_bt_upgrademetapage(Page page)
{
	BTMetaPageData *metad;
	BTPageOpaque metaopaque PG_USED_FOR_ASSERTS_ONLY;

	metad = BTPageGetMeta(page);
	metaopaque = BTPageGetOpaque(page);

	/* 它必须确实是可升级版本的元页 */
	Assert(metaopaque->btpo_flags & BTP_META);
	Assert(metad->btm_version < BTREE_NOVAC_VERSION);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);

	/* 设置版本号，并填入版本 3 新增的额外字段 */
	metad->btm_version = BTREE_NOVAC_VERSION;
	metad->btm_last_cleanup_num_delpages = 0;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;
	/* 只有 REINDEX 才能设置该字段 */
	Assert(!metad->btm_allequalimage);
	metad->btm_allequalimage = false;

	/* 调整 pd_lower（细节见 _bt_initmetapage()） */
	((PageHeader) page)->pd_lower =
		((char *) metad + sizeof(BTMetaPageData)) - (char *) page;
}

/*
 * 从持有共享锁、内含元页的缓冲区取得元数据，同时做
 * 标准的健全性检查。
 *
 * 把这里返回的数据缓存在本地的调用者应当注意：
 * 用 _bt_upgrademetapage() 做的即时升级可以改变版本字段
 * 以及 BTREE_NOVAC_VERSION 专有字段，却不会使本地缓存失效。
 */
static BTMetaPageData *
_bt_getmeta(Relation rel, Buffer metabuf)
{
	Page		metapg;
	BTPageOpaque metaopaque;
	BTMetaPageData *metad;

	metapg = BufferGetPage(metabuf);
	metaopaque = BTPageGetOpaque(metapg);
	metad = BTPageGetMeta(metapg);

	/* 对元页做健全性检查 */
	if (!P_ISMETA(metaopaque) ||
		metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree",
						RelationGetRelationName(rel))));

	if (metad->btm_version < BTREE_MIN_VERSION ||
		metad->btm_version > BTREE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("version mismatch in index \"%s\": file version %d, "
						"current version %d, minimal supported version %d",
						RelationGetRelationName(rel),
						metad->btm_version, BTREE_VERSION, BTREE_MIN_VERSION)));

	return metad;
}

/*
 * _bt_vacuum_needs_cleanup() -- 检查索引是否需要清理
 *
 * 当没有任何索引元组需要删除、因而从未调用 btbulkdelete 时，
 * 由 btvacuumcleanup 调用。
 */
bool
_bt_vacuum_needs_cleanup(Relation rel)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;
	uint32		btm_version;
	BlockNumber prev_num_delpages;

	/*
	 * 尽快把元页上的细节复制到局部变量。
	 *
	 * 注意，这里故意不用元页的缓存版本。
	 */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);
	btm_version = metad->btm_version;

	if (btm_version < BTREE_NOVAC_VERSION)
	{
		/*
		 * 元页需要动态升级，以便存放仅当
		 * btm_version >= BTREE_NOVAC_VERSION 时才存在的字段
		 */
		_bt_relbuf(rel, metabuf);
		return true;
	}

	prev_num_delpages = metad->btm_last_cleanup_num_delpages;
	_bt_relbuf(rel, metabuf);

	/*
	 * 在少数情况下，若 prev_num_delpages 超过索引总大小的 5%，
	 * 就触发清理。若决定在正在进行的 btvacuumcleanup 期间
	 * 调用 btvacuumscan，我们有理由期望（尽管不能保证）
	 * 能够回收这么多页。
	 * 进一步细节见 nbtree/README 中关于把已删除页放入
	 * FSM 的一节。
	 */
	if (prev_num_delpages > 0 &&
		prev_num_delpages > RelationGetNumberOfBlocks(rel) / 20)
		return true;

	return false;
}

/*
 * _bt_set_cleanup_info() -- 为 btvacuumcleanup 更新元页。
 *
 * 在 btvacuumcleanup 结束、num_delpages 的值已经
 * 定稿时调用。
 */
void
_bt_set_cleanup_info(Relation rel, BlockNumber num_delpages)
{
	Buffer		metabuf;
	Page		metapg;
	BTMetaPageData *metad;

	/*
	 * 磁盘兼容性说明：元页字段 btm_last_cleanup_num_delpages
	 * 起初是名为 btm_oldest_btpo_xact 的 TransactionId 字段。
	 * 两种"版本"都只是 uint32 字段。开始在已删除页上使用
	 * 64 位 XIDs 时，复用该字段很方便。
	 *
	 * 经过 pg_upgrade 的数据库有可能在
	 * 如今被认作元页 btm_last_cleanup_num_delpages
	 * 字段的位置留下一个 XID 值。_bt_vacuum_needs_cleanup() 甚至可能认为该值
	 * 表明有大量页需要回收，而
	 * 实际上只有一两页。最坏的情况是
	 * 会稍早调用一次 btvacuumscan，而当我们被调用时它会
	 * 把 btm_last_cleanup_num_delpages 设成合理的值。
	 *
	 * 还要注意，自 PostgreSQL 14 起，元页的 btm_last_cleanup_num_heap_tuples 字段
	 * 已不再使用。重写时我们把它设为 -1.0，只是
	 * 为了保持一致。
	 */
	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metad = BTPageGetMeta(metapg);

	/* BTREE_MIN_VERSION 时不要错过升级索引/元页的机会 */
	if (metad->btm_version >= BTREE_NOVAC_VERSION &&
		metad->btm_last_cleanup_num_delpages == num_delpages)
	{
		/* 通常意味着索引的 num_delpages 仍为 0 */
		_bt_relbuf(rel, metabuf);
		return;
	}

	/* 把读锁换成写锁 */
	_bt_unlockbuf(rel, metabuf);
	_bt_lockbuf(rel, metabuf, BT_WRITE);

	START_CRIT_SECTION();

	/* 需要时升级元页 */
	if (metad->btm_version < BTREE_NOVAC_VERSION)
		_bt_upgrademetapage(metapg);

	/* 更新与清理相关的信息 */
	metad->btm_last_cleanup_num_delpages = num_delpages;
	metad->btm_last_cleanup_num_heap_tuples = -1.0;
	MarkBufferDirty(metabuf);

	/* 需要时写入 WAL 记录 */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_metadata md;
		XLogRecPtr	recptr;

		XLogBeginInsert();
		XLogRegisterBuffer(0, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

		Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
		md.version = metad->btm_version;
		md.root = metad->btm_root;
		md.level = metad->btm_level;
		md.fastroot = metad->btm_fastroot;
		md.fastlevel = metad->btm_fastlevel;
		md.last_cleanup_num_delpages = num_delpages;
		md.allequalimage = metad->btm_allequalimage;

		XLogRegisterBufData(0, (char *) &md, sizeof(xl_btree_metadata));

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_META_CLEANUP);

		PageSetLSN(metapg, recptr);
	}

	END_CRIT_SECTION();

	_bt_relbuf(rel, metabuf);
}

/*
 *	_bt_getroot() -- 取得 B-树的根页。
 *
 *		由于根页可能在 B-树文件中移动，我们必须从
 *		元页读出它的位置，然后再读根页
 *		本身。若根页尚不存在，就必须创建一个。
 *
 *		访问类型参数（BT_READ 或 BT_WRITE）控制是否
 *		创建新根页。若 access = BT_READ，
 *		且根页不存在，就只返回 InvalidBuffer。对于
 *		BT_WRITE，若根页不存在，我们会尝试创建它。
 *		注意：即使 access = BT_WRITE，返回的根页上也只设置
 *		读锁！
 *
 *		若 access = BT_WRITE，必须设置 heaprel；否则调用者可以只
 *		传入 NULL。说明见 _bt_allocbuf。
 *
 *		返回的页不一定是真正的根——它可能是
 *		"快速根"（因删除而在其所在层上单独存在的一页）。
 *		另外，若我们"在途中"时根页发生分裂，
 *		返回的将是旧根，它现在只是
 *		某一层（可能并不很宽）上最左边的页。对大多数用途而言，这
 *		与真正的根一样好，甚至更好，因此我们并不费力
 *		坚持找到真正的根。不过我们保证
 *		返回的是活页（不是已删除或半死的页）。
 *
 *		成功返回时，根页已被钉住并加上读锁。
 *		退出时元页既不加锁也不钉住。
 */
Buffer
_bt_getroot(Relation rel, Relation heaprel, int access)
{
	Buffer		metabuf;
	Buffer		rootbuf;
	Page		rootpage;
	BTPageOpaque rootopaque;
	BlockNumber rootblkno;
	uint32		rootlevel;
	BTMetaPageData *metad;

	Assert(access == BT_READ || heaprel != NULL);

	/*
	 * 尝试用先前缓存的元页数据来找根。这
	 * 通常每次索引搜索能省一次缓冲区访问，对
	 * bufmgr 流量乃至争用都是很有帮助的节省。
	 */
	if (rel->rd_amcache != NULL)
	{
		metad = (BTMetaPageData *) rel->rd_amcache;
		/* 若下列任一检查失败，我们本不该缓存它 */
		Assert(metad->btm_magic == BTREE_MAGIC);
		Assert(metad->btm_version >= BTREE_MIN_VERSION);
		Assert(metad->btm_version <= BTREE_VERSION);
		Assert(!metad->btm_allequalimage ||
			   metad->btm_version > BTREE_NOVAC_VERSION);
		Assert(metad->btm_root != P_NONE);

		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		rootbuf = _bt_getbuf(rel, rootblkno, BT_READ);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = BTPageGetOpaque(rootpage);

		/*
		 * 由于缓存可能过时，这里对页的检查比平常
		 * 更仔细。我们必须检查它未被删除。若它
		 * 在所在层上并不单独存在，也拒绝它——这也许过于
		 * 谨慎，但宁可安全。注意我们不检查 P_ISROOT，
		 * 因为"快速根"上并不设置该标志。
		 */
		if (!P_IGNORE(rootopaque) &&
			rootopaque->btpo_level == rootlevel &&
			P_LEFTMOST(rootopaque) &&
			P_RIGHTMOST(rootopaque))
		{
			/* 可以，把缓存页当作根接受 */
			return rootbuf;
		}
		_bt_relbuf(rel, rootbuf);
		/* 缓存已过时，丢掉它 */
		if (rel->rd_amcache)
			pfree(rel->rd_amcache);
		rel->rd_amcache = NULL;
	}

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metad = _bt_getmeta(rel, metabuf);

	/* 若根页尚未初始化，就初始化它 */
	if (metad->btm_root == P_NONE)
	{
		Page		metapg;

		/* 若 access = BT_READ，调用者还不希望我们创建根 */
		if (access == BT_READ)
		{
			_bt_relbuf(rel, metabuf);
			return InvalidBuffer;
		}

		/* 把读锁换成写锁 */
		_bt_unlockbuf(rel, metabuf);
		_bt_lockbuf(rel, metabuf, BT_WRITE);

		/*
		 * 竞态：若在我们释放读锁与取得写锁之间
		 * 已有别人初始化了元数据，我们
		 * 必须避免再做一次。
		 */
		if (metad->btm_root != P_NONE)
		{
			/*
			 * 元数据已由别人初始化。为了保证不会
			 * 死锁，我们必须释放元页并从头
			 * 再来。（这真的必要吗？不过这种情形几乎不值得
			 * 去优化。）
			 */
			_bt_relbuf(rel, metabuf);
			return _bt_getroot(rel, heaprel, access);
		}

		/*
		 * 取得、初始化、写出新根页，并在其上留下适当类型的锁。
		 * 由于这是树中的第一页，它既是叶子
		 * 也是根。
		 */
		rootbuf = _bt_allocbuf(rel, heaprel);
		rootblkno = BufferGetBlockNumber(rootbuf);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = BTPageGetOpaque(rootpage);
		rootopaque->btpo_prev = rootopaque->btpo_next = P_NONE;
		rootopaque->btpo_flags = (BTP_LEAF | BTP_ROOT);
		rootopaque->btpo_level = 0;
		rootopaque->btpo_cycleid = 0;
		/* 取得元页的原始页指针 */
		metapg = BufferGetPage(metabuf);

		/* 元页更新之前不要 ELOG(ERROR) */
		START_CRIT_SECTION();

		/* 需要时升级元页 */
		if (metad->btm_version < BTREE_NOVAC_VERSION)
			_bt_upgrademetapage(metapg);

		metad->btm_root = rootblkno;
		metad->btm_level = 0;
		metad->btm_fastroot = rootblkno;
		metad->btm_fastlevel = 0;
		metad->btm_last_cleanup_num_delpages = 0;
		metad->btm_last_cleanup_num_heap_tuples = -1.0;

		MarkBufferDirty(rootbuf);
		MarkBufferDirty(metabuf);

		/* XLOG 相关 */
		if (RelationNeedsWAL(rel))
		{
			xl_btree_newroot xlrec;
			XLogRecPtr	recptr;
			xl_btree_metadata md;

			XLogBeginInsert();
			XLogRegisterBuffer(0, rootbuf, REGBUF_WILL_INIT);
			XLogRegisterBuffer(2, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

			Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
			md.version = metad->btm_version;
			md.root = rootblkno;
			md.level = 0;
			md.fastroot = rootblkno;
			md.fastlevel = 0;
			md.last_cleanup_num_delpages = 0;
			md.allequalimage = metad->btm_allequalimage;

			XLogRegisterBufData(2, (char *) &md, sizeof(xl_btree_metadata));

			xlrec.rootblk = rootblkno;
			xlrec.level = 0;

			XLogRegisterData((char *) &xlrec, SizeOfBtreeNewroot);

			recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_NEWROOT);

			PageSetLSN(rootpage, recptr);
			PageSetLSN(metapg, recptr);
		}

		END_CRIT_SECTION();

		/*
		 * 把根上的写锁换成读锁。新根页在解锁期间
		 * 没有别人会访问它的危险，因为还没有人
		 * 知道它在哪里。
		 */
		_bt_unlockbuf(rel, rootbuf);
		_bt_lockbuf(rel, rootbuf, BT_READ);

		/* 好，元数据正确，释放其上的锁且不缓存 */
		_bt_relbuf(rel, metabuf);
	}
	else
	{
		rootblkno = metad->btm_fastroot;
		Assert(rootblkno != P_NONE);
		rootlevel = metad->btm_fastlevel;

		/*
		 * 缓存元页数据，供下次使用
		 */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt,
											 sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));

		/*
		 * 元页已经用完；安排在第一次
		 * _bt_relandgetbuf 调用时释放它
		 */
		rootbuf = metabuf;

		for (;;)
		{
			rootbuf = _bt_relandgetbuf(rel, rootbuf, rootblkno, BT_READ);
			rootpage = BufferGetPage(rootbuf);
			rootopaque = BTPageGetOpaque(rootpage);

			if (!P_IGNORE(rootopaque))
				break;

			/* 此页已死，向右走一页 */
			if (P_RIGHTMOST(rootopaque))
				elog(ERROR, "no live root page found in index \"%s\"",
					 RelationGetRelationName(rel));
			rootblkno = rootopaque->btpo_next;
		}

		if (rootopaque->btpo_level != rootlevel)
			elog(ERROR, "root page %u of index \"%s\" has level %u, expected %u",
				 rootblkno, RelationGetRelationName(rel),
				 rootopaque->btpo_level, rootlevel);
	}

	/*
	 * 到这里，我们在根页上持有钉住和读锁，元页上
	 * 没有设置锁。返回根页的缓冲区。
	 */
	return rootbuf;
}

/*
 *	_bt_gettrueroot() -- 取得 B-树的真正根页。
 *
 *		这与 _bt_getroot() 的 BT_READ 情形相同，只是
 *		我们跟随真正根的链接，而不是快速根的链接。
 *
 * 等到我们取得根页上的锁时，它可能已经分裂，
 * 不再是真正的根。对目前本例程的用途而言这没问题；
 * 我们真正需要的只是至少能从所在的任何非根页
 * 向上移动一层。若将来确实需要锁住
 * 那一个真正的根页，可以在这里循环，每次失败都重新读元页。
 * （注意，在移向根页时持有元页上的锁是不行的——
 * 那会与任何并发的根分裂死锁。）
 */
Buffer
_bt_gettrueroot(Relation rel)
{
	Buffer		metabuf;
	Page		metapg;
	BTPageOpaque metaopaque;
	Buffer		rootbuf;
	Page		rootpage;
	BTPageOpaque rootopaque;
	BlockNumber rootblkno;
	uint32		rootlevel;
	BTMetaPageData *metad;

	/*
	 * 这里不尝试使用缓存的元页数据，因为 (a) 这条路径
	 * 对性能不关键，并且 (b) 我们走到这里本身就说明缓存
	 * 反正已经过时。鉴于 (b)，主动丢掉任何已缓存的
	 * 元页信息大概最安全。
	 */
	if (rel->rd_amcache)
		pfree(rel->rd_amcache);
	rel->rd_amcache = NULL;

	metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
	metapg = BufferGetPage(metabuf);
	metaopaque = BTPageGetOpaque(metapg);
	metad = BTPageGetMeta(metapg);

	if (!P_ISMETA(metaopaque) ||
		metad->btm_magic != BTREE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" is not a btree",
						RelationGetRelationName(rel))));

	if (metad->btm_version < BTREE_MIN_VERSION ||
		metad->btm_version > BTREE_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("version mismatch in index \"%s\": file version %d, "
						"current version %d, minimal supported version %d",
						RelationGetRelationName(rel),
						metad->btm_version, BTREE_VERSION, BTREE_MIN_VERSION)));

	/* 若根页尚未初始化，则失败 */
	if (metad->btm_root == P_NONE)
	{
		_bt_relbuf(rel, metabuf);
		return InvalidBuffer;
	}

	rootblkno = metad->btm_root;
	rootlevel = metad->btm_level;

	/*
	 * 元页已经用完；安排在第一次
	 * _bt_relandgetbuf 调用时释放它
	 */
	rootbuf = metabuf;

	for (;;)
	{
		rootbuf = _bt_relandgetbuf(rel, rootbuf, rootblkno, BT_READ);
		rootpage = BufferGetPage(rootbuf);
		rootopaque = BTPageGetOpaque(rootpage);

		if (!P_IGNORE(rootopaque))
			break;

		/* 此页已死，向右走一页 */
		if (P_RIGHTMOST(rootopaque))
			elog(ERROR, "no live root page found in index \"%s\"",
				 RelationGetRelationName(rel));
		rootblkno = rootopaque->btpo_next;
	}

	if (rootopaque->btpo_level != rootlevel)
		elog(ERROR, "root page %u of index \"%s\" has level %u, expected %u",
			 rootblkno, RelationGetRelationName(rel),
			 rootopaque->btpo_level, rootlevel);

	return rootbuf;
}

/*
 *	_bt_getrootheight() -- 取得 B-树搜索树的高度。
 *
 *		我们返回当前快速根的层号（从零计起）。
 *		它表示开始任何 B-树索引搜索时
 *		必须下降经过的树层数。
 *
 *		规划器用它做代价估计。既然只是
 *		估计，略为过时的数据也没问题，因此我们不操心
 *		更新先前缓存的数据。
 */
int
_bt_getrootheight(Relation rel)
{
	BTMetaPageData *metad;

	if (rel->rd_amcache == NULL)
	{
		Buffer		metabuf;

		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
		metad = _bt_getmeta(rel, metabuf);

		/*
		 * 若根页尚不存在，_bt_getroot() 并不期望建立缓存，
		 * 因此就停在这里，并报告索引高度为零。
		 * （XXX 也许应当改 _bt_getroot() 以允许这种情形。）
		 */
		if (metad->btm_root == P_NONE)
		{
			_bt_relbuf(rel, metabuf);
			return 0;
		}

		/*
		 * 缓存元页数据，供下次使用
		 */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt,
											 sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));
		_bt_relbuf(rel, metabuf);
	}

	/* 取得缓存页 */
	metad = (BTMetaPageData *) rel->rd_amcache;
	/* 若下列任一检查失败，我们本不该缓存它 */
	Assert(metad->btm_magic == BTREE_MAGIC);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);
	Assert(metad->btm_version <= BTREE_VERSION);
	Assert(!metad->btm_allequalimage ||
		   metad->btm_version > BTREE_NOVAC_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	return metad->btm_fastlevel;
}

/*
 *	_bt_metaversion() -- 从元页取得版本/状态信息。
 *
 *		用 B-树元页中的数据（可以是本地缓存的版本）设置调用者的
 *		*heapkeyspace 和 *allequalimage 参数。这些
 *		信息需要塞进插入扫描键，因此我们提供一个
 *		一次取回两者的函数。
 *
 *		这用来确定下降 B-树时必须使用的规则。
 *		版本 4 索引把堆 TID 当作决胜属性。
 *		经 pg_upgrade 的版本 3 索引在插入一个新的、按 BTScanInsert 来看
 *		与许多已满叶子页上的重复项重复的元组时，需要额外步骤
 *		才能保持合理性能。
 *
 *		还设置 allequalimage 字段，它表示是否
 *		可以安全地应用去重。我们依赖这样的假定：
 *		从 Postgres 12 pg_upgrade 来的 heapkeyspace 索引上，
 *		btm_allequalimage 会被清零。
 */
void
_bt_metaversion(Relation rel, bool *heapkeyspace, bool *allequalimage)
{
	BTMetaPageData *metad;

	if (rel->rd_amcache == NULL)
	{
		Buffer		metabuf;

		metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_READ);
		metad = _bt_getmeta(rel, metabuf);

		/*
		 * 若根页尚不存在，_bt_getroot() 并不期望建立缓存，
		 * 因此就停在这里。（XXX 也许应当改 _bt_getroot()
		 * 以允许这种情形。）
		 */
		if (metad->btm_root == P_NONE)
		{
			*heapkeyspace = metad->btm_version > BTREE_NOVAC_VERSION;
			*allequalimage = metad->btm_allequalimage;

			_bt_relbuf(rel, metabuf);
			return;
		}

		/*
		 * 缓存元页数据，供下次使用
		 *
		 * _bt_upgrademetapage() 做的即时版本升级
		 * 可以改变索引的 nbtree 版本，却不会使任何
		 * 本地缓存失效。这没问题，因为它只会在从
		 * 版本 2 升到版本 3 时发生，两者都是 !heapkeyspace
		 * 版本。
		 */
		rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt,
											 sizeof(BTMetaPageData));
		memcpy(rel->rd_amcache, metad, sizeof(BTMetaPageData));
		_bt_relbuf(rel, metabuf);
	}

	/* 取得缓存页 */
	metad = (BTMetaPageData *) rel->rd_amcache;
	/* 若下列任一检查失败，我们本不该缓存它 */
	Assert(metad->btm_magic == BTREE_MAGIC);
	Assert(metad->btm_version >= BTREE_MIN_VERSION);
	Assert(metad->btm_version <= BTREE_VERSION);
	Assert(!metad->btm_allequalimage ||
		   metad->btm_version > BTREE_NOVAC_VERSION);
	Assert(metad->btm_fastroot != P_NONE);

	*heapkeyspace = metad->btm_version > BTREE_NOVAC_VERSION;
	*allequalimage = metad->btm_allequalimage;
}

/*
 *	_bt_checkpage() -- 验证刚读入的页看起来健全。
 */
void
_bt_checkpage(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	/*
	 * ReadBuffer 会验证每个新读入的页都通过
	 * PageHeaderIsValid，这意味着它要么含有相当健全的
	 * 页头，要么全为零。不过我们必须防备全零
	 * 这种情形。
	 */
	if (PageIsNew(page))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains unexpected zero page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));

	/*
	 * 另外检查特殊区看起来健全。
	 */
	if (PageGetSpecialSize(page) != MAXALIGN(sizeof(BTPageOpaqueData)))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" contains corrupted page at block %u",
						RelationGetRelationName(rel),
						BufferGetBlockNumber(buf)),
				 errhint("Please REINDEX it.")));
}

/*
 *	_bt_getbuf() -- 取得缓冲区中的一个已有块，供读或写。
 *
 *		nbtree 的一般规则是：访问一页时，必须同时持有该页
 *		缓冲区上的缓冲区钉住和缓冲区锁，
 *		否则就不可以访问。
 *
 *		本例程返回时，所请求的缓冲区上已设置适当的锁，
 *		并且其引用计数已增加
 *		（即缓冲区"已锁定并钉住"）。我们还用
 *		_bt_checkpage 对该页做健全性检查，并执行 Valgrind
 *		客户端请求，以帮助 Valgrind 检测不安全的页访问。
 *
 *		注意：nbtree 中不允许直接调用 LockBuffer()；所有
 *		缓冲区锁请求都要经过包装函数，例如
 *		_bt_lockbuf()。
 */
Buffer
_bt_getbuf(Relation rel, BlockNumber blkno, int access)
{
	Buffer		buf;

	Assert(BlockNumberIsValid(blkno));

	/* 读取关系中的一个已有块 */
	buf = ReadBuffer(rel, blkno);
	_bt_lockbuf(rel, buf, access);
	_bt_checkpage(rel, buf);

	return buf;
}

/*
 *	_bt_allocbuf() -- 分配一个新块/页。
 *
 * 返回一个持有写锁的缓冲区，其中是尚未分配的 nbtree 页。
 *
 * 调用者必须传入有效的 heaprel。我们需要 heaprel，以便
 * 生成 snapshotConflictHorizon，使从 FSM 重用一页
 * 对可能在备库上运行的查询是安全的。
 */
Buffer
_bt_allocbuf(Relation rel, Relation heaprel)
{
	Buffer		buf;
	BlockNumber blkno;
	Page		page;

	Assert(heaprel != NULL);

	/*
	 * 先看 FSM 是否知道有空闲页。
	 *
	 * 不能无保留地信任 FSM 的报告；我们必须检查该
	 * 页仍然空闲。（例如，一张已经空闲的页可能在上次 VACUUM
	 * 扫描它之后、VACUUM 更新 FSM 之前
	 * 就被重新使用了。）
	 *
	 * 实际上比这更糟：我们甚至不能假定对所报告的页
	 * 加锁是安全的。若别人持有它的锁，
	 * 或者更糟，我们自己的调用者持有，就可能死锁。（调用者自己
	 * 持锁的情形其实并非罕见。考虑 serial 或
	 * timestamp 列上的索引。几乎所有分裂都发生在最右页，因此
	 * _bt_split 完全可能在持有最近从 FSM 取得的那一页的锁时
	 * 调用我们。与上一次分裂并发运行的 VACUUM
	 * 很可能已经把那一页
	 * 放回了 FSM。）
	 *
	 * 为绕过这一点，我们只对所报告的页请求条件锁。
	 * 若失败，则别人正在使用该页，我们就可以
	 * 合理地假定它并不空闲。（若我们碰巧错了，最坏
	 * 后果是该页直到下次 VACUUM 之前无法使用，这
	 * 不是大问题。）
	 */
	for (;;)
	{
		blkno = GetFreeIndexPage(rel);
		if (blkno == InvalidBlockNumber)
			break;
		buf = ReadBuffer(rel, blkno);
		if (_bt_conditionallockbuf(rel, buf))
		{
			page = BufferGetPage(buf);

			/*
			 * 索引中有可能找到全零页。例如，
			 * 某个后端可能成功把关系扩展了
			 * 一页，然后在能为加入该页
			 * 写下 WAL 记录之前崩溃。若找到全零页，
			 * 就立即收回它。
			 */
			if (PageIsNew(page))
			{
				/* 可以使用该页。初始化并返回它。 */
				_bt_pageinit(page, BufferGetPageSize(buf));
				return buf;
			}

			if (BTPageIsRecyclable(page, heaprel))
			{
				/*
				 * 若正在为热备生成 WAL，就创建一条 WAL
				 * 记录，以便与备库上运行的查询产生冲突，
				 * 以防它们的快照比 safexid
				 * 值更旧
				 */
				if (RelationNeedsWAL(rel) && XLogStandbyInfoActive())
				{
					xl_btree_reuse_page xlrec_reuse;

					/*
					 * 注意我们不把该缓冲区登记到这条记录上，
					 * 因为这次操作并不修改该页（那
					 * 已经发生过了，是在 VACUUM 删除该页时）。
					 * 这条记录的存在只是为热备提供一个冲突点。
					 * 见该记录 REDO 例程的注释。
					 */
					xlrec_reuse.locator = rel->rd_locator;
					xlrec_reuse.block = blkno;
					xlrec_reuse.snapshotConflictHorizon = BTPageGetDeleteXid(page);
					xlrec_reuse.isCatalogRel =
						RelationIsAccessibleInLogicalDecoding(heaprel);

					XLogBeginInsert();
					XLogRegisterData((char *) &xlrec_reuse, SizeOfBtreeReusePage);

					XLogInsert(RM_BTREE_ID, XLOG_BTREE_REUSE_PAGE);
				}

				/* 可以使用该页。重新初始化并返回它。 */
				_bt_pageinit(page, BufferGetPageSize(buf));
				return buf;
			}
			elog(DEBUG2, "FSM returned nonrecyclable page");
			_bt_relbuf(rel, buf);
		}
		else
		{
			elog(DEBUG2, "FSM returned nonlockable page");
			/* 没能拿到锁，因此只放下钉住 */
			ReleaseBuffer(buf);
		}
	}

	/*
	 * 把关系扩展一页。必须使用 RBM_ZERO_AND_LOCK，否则
	 * 会与 btvacuumscan 出现竞态——见其中的注释。
	 * 这迫使我们重复 _bt_lockbuf() 原本会发出的
	 * valgrind 请求，因为若使用 _bt_lockbuf() 就会引入
	 * 竞态。
	 */
	buf = ExtendBufferedRel(BMR_REL(rel), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_DEFINED(BufferGetPage(buf), BLCKSZ);

	/* 返回之前先初始化新页 */
	page = BufferGetPage(buf);
	Assert(PageIsNew(page));
	_bt_pageinit(page, BufferGetPageSize(buf));

	return buf;
}

/*
 *	_bt_relandgetbuf() -- 释放一个已锁定的缓冲区，再取得另一个。
 *
 * 这等价于先 _bt_relbuf 再 _bt_getbuf。另外，若 obuf 是
 * InvalidBuffer，它就退化为只做 _bt_getbuf；允许这种情形
 * 可以简化某些调用者。
 *
 * 最初使用它是为了避免进两次 bufmgr，而一次就够。
 * 不过现在主要只是写法上的方便。与 _bt_relbuf/_bt_getbuf 相比，
 * 它真正省事的唯一情形是目标页
 * 已经就在这个缓冲区里。
 */
Buffer
_bt_relandgetbuf(Relation rel, Buffer obuf, BlockNumber blkno, int access)
{
	Buffer		buf;

	Assert(BlockNumberIsValid(blkno));
	if (BufferIsValid(obuf))
		_bt_unlockbuf(rel, obuf);
	buf = ReleaseAndReadBuffer(obuf, rel, blkno);
	_bt_lockbuf(rel, buf, access);

	_bt_checkpage(rel, buf);
	return buf;
}

/*
 *	_bt_relbuf() -- 释放一个已锁定的缓冲区。
 *
 * 锁和钉住（引用计数）都会放下。
 */
void
_bt_relbuf(Relation rel, Buffer buf)
{
	_bt_unlockbuf(rel, buf);
	ReleaseBuffer(buf);
}

/*
 *	_bt_lockbuf() -- 锁住一个已钉住的缓冲区。
 *
 * 取得锁时不再另取钉住。这像一次直接的
 * LockBuffer() 调用，但会执行 Valgrind 所需的额外步骤。
 *
 * 注意：若 buf 上的钉住最初不是在 _bt_getbuf() 或
 * _bt_relandgetbuf() 中取得的，调用者可能需要对 buf 调用 _bt_checkpage()。
 */
void
_bt_lockbuf(Relation rel, Buffer buf, int access)
{
	/* LockBuffer() 断言钉住由本后端持有 */
	LockBuffer(buf, access);

	/*
	 * 即使发生 nbtree 错误（例如唯一性违反错误）时不会调用
	 * _bt_unlockbuf()，也没关系。那不会造成
	 * Valgrind 误报。
	 *
	 * nbtree 的客户端请求叠加在 bufmgr.c
	 * 缓冲区钉住客户端请求之上。发生 nbtree 错误时，该缓冲区
	 * 肯定会在后端再次
	 * 取得它的第一次钉住时被标为已定义。（当然，若该后端再也不
	 * 碰这个缓冲区，那么它对 Valgrind 仍然不可访问
	 * 也无关紧要。）
	 *
	 * 注意：若用持锁期间从页上读出的 ItemId 算出一个 IndexTuple 的 C 指针，
	 * 一旦释放锁，该 C 指针就永远不再能安全地
	 * 解引用。不过 Valgrind 只能
	 * 检测在没有 _current_
	 * 锁/钉住时解引用该指针的情形。
	 */
	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_DEFINED(BufferGetPage(buf), BLCKSZ);
}

/*
 *	_bt_unlockbuf() -- 解锁一个已钉住的缓冲区。
 */
void
_bt_unlockbuf(Relation rel, Buffer buf)
{
	/*
	 * 缓冲区已钉住并锁定，因此预期它是
	 * 已定义且可寻址的。主动检查这一点。
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(BufferGetPage(buf), BLCKSZ);

	/* LockBuffer() 断言钉住由本后端持有 */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_NOACCESS(BufferGetPage(buf), BLCKSZ);
}

/*
 *	_bt_conditionallockbuf() -- 有条件地对已钉住的
 *	缓冲区加 BT_WRITE 锁。
 *
 * 注意：若 buf 上的钉住最初不是在 _bt_getbuf() 或
 * _bt_relandgetbuf() 中取得的，调用者可能需要对 buf 调用 _bt_checkpage()。
 */
bool
_bt_conditionallockbuf(Relation rel, Buffer buf)
{
	/* ConditionalLockBuffer() 断言钉住由本后端持有 */
	if (!ConditionalLockBuffer(buf))
		return false;

	if (!RelationUsesLocalBuffers(rel))
		VALGRIND_MAKE_MEM_DEFINED(BufferGetPage(buf), BLCKSZ);

	return true;
}

/*
 *	_bt_upgradelockbufcleanup() -- 把锁升级为完整的清理锁。
 */
void
_bt_upgradelockbufcleanup(Relation rel, Buffer buf)
{
	/*
	 * 缓冲区已钉住并锁定，因此预期它是
	 * 已定义且可寻址的。主动检查这一点。
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(BufferGetPage(buf), BLCKSZ);

	/* LockBuffer() 断言钉住由本后端持有 */
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	LockBufferForCleanup(buf);
}

/*
 *	_bt_pageinit() -- 初始化一个新页。
 *
 * 返回时，页头已初始化；数据区为空；
 * 特殊区已清零。
 */
void
_bt_pageinit(Page page, Size size)
{
	PageInit(page, size, sizeof(BTPageOpaqueData));
}

/*
 * 在 VACUUM 期间从 B-树叶子页删除 item(s)。
 *
 * 本例程假定调用者已经在
 * 缓冲区上持有完整的清理锁。另外，给定的 deletable 与 updatable 数组*必须*
 * 按升序排列。
 *
 * 本例程处理这样的情形：现有倒排列表项中的部分（但不是全部）堆 TIDs
 * 要被删除。做法是
 * 用调用者给出的该项新版本更新/覆盖现有项
 * （新版本去掉了待删除的 TIDs）。
 *
 * 我们在 WAL 中对 VACUUMs 与 B-树删除的记法不同。删除必须
 * 直接从 tableam 生成自己的 snapshotConflictHorizon，
 * 而 VACUUM 则依赖最初的 VACUUM 表扫描所做的
 * WAL 记录，间接地为该表的索引
 * 处理这个问题。另外，我们会从页上去掉 VACUUM 周期 ID，B-树
 * 删除并不这样做。
 */
void
_bt_delitems_vacuum(Relation rel, Buffer buf,
					OffsetNumber *deletable, int ndeletable,
					BTVacuumPosting *updatable, int nupdatable)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	bool		needswal = RelationNeedsWAL(rel);
	char	   *updatedbuf = NULL;
	Size		updatedbuflen = 0;
	OffsetNumber updatedoffsets[MaxIndexTuplesPerPage];

	/* 除非有事可做，否则不该被调用 */
	Assert(ndeletable > 0 || nupdatable > 0);

	/* 生成去掉已删除 TIDs 的倒排列表新版本 */
	if (nupdatable > 0)
		updatedbuf = _bt_delitems_update(updatable, nupdatable,
										 updatedoffsets, &updatedbuflen,
										 needswal);

	/* 变更记入日志之前不要 ereport(ERROR) */
	START_CRIT_SECTION();

	/*
	 * 处理倒排元组更新。
	 *
	 * 故意在处理简单删除之前做这件事。若顺序相反
	 * （即 WAL 日志顺序——先简单删除再
	 * 更新），就必须对偏移号数组 'updatable'
	 * 做补偿性修改。
	 *
	 * PageIndexTupleOverwrite() 在某一项的 LP_DEAD 位碰巧已经设置时
	 * 不会清除它。重要的是我们不要干扰
	 * 将来任何简单索引元组删除操作。
	 */
	for (int i = 0; i < nupdatable; i++)
	{
		OffsetNumber updatedoffset = updatedoffsets[i];
		IndexTuple	itup;
		Size		itemsz;

		itup = updatable[i]->itup;
		itemsz = MAXALIGN(IndexTupleSize(itup));
		if (!PageIndexTupleOverwrite(page, updatedoffset, (Item) itup,
									 itemsz))
			elog(PANIC, "failed to update partially dead item in block %u of index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));
	}

	/* 现在处理整个元组的简单删除 */
	if (ndeletable > 0)
		PageIndexMultiDelete(page, deletable, ndeletable);

	/*
	 * 可以清除清理周期 ID，因为当前清理扫描
	 * 肯定已经处理过这一页。
	 */
	opaque = BTPageGetOpaque(page);
	opaque->btpo_cycleid = 0;

	/*
	 * 清除 BTP_HAS_GARBAGE 页标志。
	 *
	 * 该标志表示页上存在 LP_DEAD 项（尽管
	 * 并不可靠）。注意我们只对经 pg_upgrade 的
	 * !heapkeyspace 索引依赖它。因此在这里清除它通常不会
	 * 干扰简单索引元组删除。
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG 相关 */
	if (needswal)
	{
		XLogRecPtr	recptr;
		xl_btree_vacuum xlrec_vacuum;

		xlrec_vacuum.ndeleted = ndeletable;
		xlrec_vacuum.nupdated = nupdatable;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_vacuum, SizeOfBtreeVacuum);

		if (ndeletable > 0)
			XLogRegisterBufData(0, (char *) deletable,
								ndeletable * sizeof(OffsetNumber));

		if (nupdatable > 0)
		{
			XLogRegisterBufData(0, (char *) updatedoffsets,
								nupdatable * sizeof(OffsetNumber));
			XLogRegisterBufData(0, updatedbuf, updatedbuflen);
		}

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_VACUUM);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* 这里不能泄漏内存 */
	if (updatedbuf != NULL)
		pfree(updatedbuf);
	/* 释放 _bt_delitems_update() 内分配的元组 */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]->itup);
}

/*
 * 在单页清理期间从 B-树叶子页删除 item(s)。
 *
 * 本例程假定调用者已钉住并写锁定
 * 缓冲区。另外，给定的 deletable 与 updatable 数组*必须*按
 * 升序排列。
 *
 * 本例程处理这样的情形：现有倒排列表项中的部分（但不是全部）堆 TIDs
 * 要被删除。做法是
 * 用调用者给出的该项新版本更新/覆盖现有项
 * （新版本去掉了待删除的 TIDs）。
 *
 * 就对页所做的事而言，这与 _bt_delitems_vacuum 几乎相同，
 * 但它需要自己的 snapshotConflictHorizon 和 isCatalogRel
 * （来自 tableam）。REDO 例程用它们来产生恢复
 * 冲突。另一处差别是只有 _bt_delitems_vacuum 会
 * 清除页上的 VACUUM 周期 ID。
 */
static void
_bt_delitems_delete(Relation rel, Buffer buf,
					TransactionId snapshotConflictHorizon, bool isCatalogRel,
					OffsetNumber *deletable, int ndeletable,
					BTVacuumPosting *updatable, int nupdatable)
{
	Page		page = BufferGetPage(buf);
	BTPageOpaque opaque;
	bool		needswal = RelationNeedsWAL(rel);
	char	   *updatedbuf = NULL;
	Size		updatedbuflen = 0;
	OffsetNumber updatedoffsets[MaxIndexTuplesPerPage];

	/* 除非有事可做，否则不该被调用 */
	Assert(ndeletable > 0 || nupdatable > 0);

	/* 生成去掉已删除 TIDs 的倒排列表新版本 */
	if (nupdatable > 0)
		updatedbuf = _bt_delitems_update(updatable, nupdatable,
										 updatedoffsets, &updatedbuflen,
										 needswal);

	/* 变更记入日志之前不要 ereport(ERROR) */
	START_CRIT_SECTION();

	/* 像 _bt_delitems_vacuum 那样处理更新和删除 */
	for (int i = 0; i < nupdatable; i++)
	{
		OffsetNumber updatedoffset = updatedoffsets[i];
		IndexTuple	itup;
		Size		itemsz;

		itup = updatable[i]->itup;
		itemsz = MAXALIGN(IndexTupleSize(itup));
		if (!PageIndexTupleOverwrite(page, updatedoffset, (Item) itup,
									 itemsz))
			elog(PANIC, "failed to update partially dead item in block %u of index \"%s\"",
				 BufferGetBlockNumber(buf), RelationGetRelationName(rel));
	}

	if (ndeletable > 0)
		PageIndexMultiDelete(page, deletable, ndeletable);

	/*
	 * 与 _bt_delitems_vacuum 不同，此时*绝不能*清除清理周期 ID。
	 * 只有 VACUUM 命令控制清理周期 ID。
	 */
	opaque = BTPageGetOpaque(page);

	/*
	 * 清除 BTP_HAS_GARBAGE 页标志。
	 *
	 * 该标志表示页上存在 LP_DEAD 项（尽管
	 * 并不可靠）。注意我们只对经 pg_upgrade 的
	 * !heapkeyspace 索引依赖它。
	 */
	opaque->btpo_flags &= ~BTP_HAS_GARBAGE;

	MarkBufferDirty(buf);

	/* XLOG 相关 */
	if (needswal)
	{
		XLogRecPtr	recptr;
		xl_btree_delete xlrec_delete;

		xlrec_delete.snapshotConflictHorizon = snapshotConflictHorizon;
		xlrec_delete.ndeleted = ndeletable;
		xlrec_delete.nupdated = nupdatable;
		xlrec_delete.isCatalogRel = isCatalogRel;

		XLogBeginInsert();
		XLogRegisterBuffer(0, buf, REGBUF_STANDARD);
		XLogRegisterData((char *) &xlrec_delete, SizeOfBtreeDelete);

		if (ndeletable > 0)
			XLogRegisterBufData(0, (char *) deletable,
								ndeletable * sizeof(OffsetNumber));

		if (nupdatable > 0)
		{
			XLogRegisterBufData(0, (char *) updatedoffsets,
								nupdatable * sizeof(OffsetNumber));
			XLogRegisterBufData(0, updatedbuf, updatedbuflen);
		}

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_DELETE);

		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	/* 这里不能泄漏内存 */
	if (updatedbuf != NULL)
		pfree(updatedbuf);
	/* 释放 _bt_delitems_update() 内分配的元组 */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]->itup);
}

/*
 * 建立通过"更新"元组来从倒排列表元组删除 TIDs 所需的状态。
 * 执行 _bt_delitems_vacuum 与
 * _bt_delitems_delete 共同的步骤。这些步骤必须在每个函数的
 * 临界区开始之前完成。
 *
 * updatable 与 nupdatable 是输入，不过注意我们将用
 * _bt_update_posting() 把原来的 itup 替换为指向最终
 * 版本的指针，该版本在 palloc() 的内存中。调用者应在完成后释放这些元组。
 *
 * updatedoffsets 的前 nupdatable 项被设为调用者所更新的
 * 倒排列表元组的页偏移号。这主要有用，
 * 因为调用者可能需要对页偏移记 WAL（不过为了方便，
 * 我们总是替调用者做这件事）。
 *
 * 返回由 xl_btree_update 结构数组组成的缓冲区，
 * 描述我们在这里为调用者执行的步骤（不过仅当 needswal 为
 * true 时）。还把 *updatedbuflen 设为该缓冲区的最终大小。调用者在
 * 需要 WAL 日志时使用这个缓冲区。
 */
static char *
_bt_delitems_update(BTVacuumPosting *updatable, int nupdatable,
					OffsetNumber *updatedoffsets, Size *updatedbuflen,
					bool needswal)
{
	char	   *updatedbuf = NULL;
	Size		buflen = 0;

	/* 除非有事可做，否则不该被调用 */
	Assert(nupdatable > 0);

	for (int i = 0; i < nupdatable; i++)
	{
		BTVacuumPosting vacposting = updatable[i];
		Size		itemsz;

		/* 用更新后的版本替换工作区中的 IndexTuple */
		_bt_update_posting(vacposting);

		/* 顺便记录 updatedbuf 中 xl_btree_update 的大小 */
		itemsz = SizeOfBtreeUpdate + vacposting->ndeletedtids * sizeof(uint16);
		buflen += itemsz;

		/* 顺便构造 updatedoffsets 缓冲区 */
		updatedoffsets[i] = vacposting->updatedoffset;
	}

	/* XLOG 相关 */
	if (needswal)
	{
		Size		offset = 0;

		/* 分配，并为调用者设置最终大小 */
		updatedbuf = palloc(buflen);
		*updatedbuflen = buflen;
		for (int i = 0; i < nupdatable; i++)
		{
			BTVacuumPosting vacposting = updatable[i];
			Size		itemsz;
			xl_btree_update update;

			update.ndeletedtids = vacposting->ndeletedtids;
			memcpy(updatedbuf + offset, &update.ndeletedtids,
				   SizeOfBtreeUpdate);
			offset += SizeOfBtreeUpdate;

			itemsz = update.ndeletedtids * sizeof(uint16);
			memcpy(updatedbuf + offset, vacposting->deletetids, itemsz);
			offset += itemsz;
		}
	}

	return updatedbuf;
}

/*
 * _bt_delitems_delete_check() 用来把 deltids 数组
 * 恢复成原来的叶子页排序顺序的比较器
 */
static int
_bt_delitems_cmp(const void *a, const void *b)
{
	TM_IndexDelete *indexdelete1 = (TM_IndexDelete *) a;
	TM_IndexDelete *indexdelete2 = (TM_IndexDelete *) b;

	if (indexdelete1->id > indexdelete2->id)
		return 1;
	if (indexdelete1->id < indexdelete2->id)
		return -1;

	Assert(false);

	return 0;
}

/*
 * 在单页清理期间尝试从 B-树叶子页删除 item(s)。
 *
 * nbtree 对 table_index_delete_tuples() 的接口。从调用者的 deltids 数组中删除一部分索引
 * 元组：那些 TIDs 被 tableam 判定可以安全
 * 删除的（或已在索引中标为 LP_DEAD，因而已被
 * 我们的简单索引删除调用者知道可删除的）。我们最后才从 buf 叶子页上物理
 * 删除索引元组（仅限在 table_index_delete_tuples() 调用之后
 * 已知这样做安全的那些索引元组）。
 *
 * 简单索引删除的调用者只纳入来自已标
 * LP_DEAD 的索引元组的 TIDs，以及它在同一叶子页上发现、可以
 * 纳入却不增加整个删除操作所涉及的不同表块
 * 总数的额外 TIDs。这种做法常常让我们删掉
 * 一些对 tableam 来说几乎不花代价就能顺带检查的额外索引元组
 * （当它们实际上可以安全删除时）。大概只有当
 * tableam 是按块组织的，继续做这些额外检查才有意义
 * （否则检查大概不会几乎不花代价，而我们依赖这一点）。
 * 不过 tableam 接口需要 tableam 一侧来处理这个问题，所以这样没问题
 * （我们作为索引访问方法可以
 * 做简化假定：所有 tableam 都必须是基于块的）。
 *
 * 自底向上索引删除的调用者提供叶子页上的全部 TIDs，
 * 并不期望 tableam 会检查其中的大多数。tableam 对
 * 检查哪些项/块有相当大的裁量权。我们在
 * 为自底向上删除操作估价时的角色严格说只是建议性的。
 *
 * 注意：调用者必须已按叶子页顺序加入 deltids 项（即进入
 * delstate 主数组的项）：页偏移号顺序，
 * 同一倒排列表元组中的项按 TID 顺序（以
 * TID 决胜）。这里用这种顺序很方便。
 *
 * 注意：我们还依赖每个 deltids 元素的 id 字段"记下"这一
 * 原始的叶子页顺序。也就是说，我们期望只靠按 id
 * 字段对 deltids 排序就能回到
 * 原始的叶子页顺序（tableam 会出于自己的原因对 deltids 排序，
 * 因此之后需要把它放回叶子页顺序）。
 */
void
_bt_delitems_delete_check(Relation rel, Buffer buf, Relation heapRel,
						  TM_IndexDeleteOp *delstate)
{
	Page		page = BufferGetPage(buf);
	TransactionId snapshotConflictHorizon;
	bool		isCatalogRel;
	OffsetNumber postingidxoffnum = InvalidOffsetNumber;
	int			ndeletable = 0,
				nupdatable = 0;
	OffsetNumber deletable[MaxIndexTuplesPerPage];
	BTVacuumPosting updatable[MaxIndexTuplesPerPage];

	/* 先用 tableam 接口确定要删除哪些元组 */
	snapshotConflictHorizon = table_index_delete_tuples(heapRel, delstate);
	isCatalogRel = RelationIsAccessibleInLogicalDecoding(heapRel);

	/* 除非有必要，否则不应对 snapshotConflictHorizon 记 WAL */
	if (!XLogStandbyInfoActive())
		snapshotConflictHorizon = InvalidTransactionId;

	/*
	 * 构造一份按叶子页顺序的描述，说明 _bt_delitems_delete()
	 * 需要做什么才能从该页物理删除索引元组。
	 *
	 * 必须对 deltids 数组排序，以恢复叶子页顺序（调用
	 * tableam 之前的原始顺序）。这是该循环所期望的顺序。
	 *
	 * 注意 deltids 数组现在可能小得多。它甚至可能
	 * 一项都没有（自底向上删除的调用者就是如此），这种情况下
	 * 就没有什么可做的了。
	 */
	qsort(delstate->deltids, delstate->ndeltids, sizeof(TM_IndexDelete),
		  _bt_delitems_cmp);
	if (delstate->ndeltids == 0)
	{
		Assert(delstate->bottomup);
		return;
	}

	/* 我们肯定至少要删除一个索引元组（或一个 TID） */
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexStatus *dstatus = delstate->status + delstate->deltids[i].id;
		OffsetNumber idxoffnum = dstatus->idxoffnum;
		ItemId		itemid = PageGetItemId(page, idxoffnum);
		IndexTuple	itup = (IndexTuple) PageGetItem(page, itemid);
		int			nestedi,
					nitem;
		BTVacuumPosting vacposting;

		Assert(OffsetNumberIsValid(idxoffnum));

		if (idxoffnum == postingidxoffnum)
		{
			/*
			 * 这条 deltid 项是来自倒排列表元组的 TID，该元组
			 * 已经被完整处理过
			 */
			Assert(BTreeTupleIsPosting(itup));
			Assert(ItemPointerCompare(BTreeTupleGetHeapTID(itup),
									  &delstate->deltids[i].tid) < 0);
			Assert(ItemPointerCompare(BTreeTupleGetMaxHeapTID(itup),
									  &delstate->deltids[i].tid) >= 0);
			continue;
		}

		if (!BTreeTupleIsPosting(itup))
		{
			/* 普通的非枢轴元组 */
			Assert(ItemPointerEquals(&itup->t_tid, &delstate->deltids[i].tid));
			if (dstatus->knowndeletable)
				deletable[ndeletable++] = idxoffnum;
			continue;
		}

		/*
		 * itup 是倒排列表元组，其最低的 deltids 项（它可能
		 * 是也可能不是 itup 的第一个 TID）现在在这里考虑。
		 * 不过现在应当把该倒排列表的全部 deltids 项
		 * 放在一起处理（不只是最低的那项）。记住在最外层
		 * 循环的后续迭代中跳过
		 * 后来与 itup 相关的项。
		 */
		postingidxoffnum = idxoffnum;	/* 记住最外层循环中的工作 */
		nestedi = i;			/* 为第一条 itup deltids 项做初始化 */
		vacposting = NULL;		/* 描述对 itup 的最终动作 */
		nitem = BTreeTupleGetNPosting(itup);
		for (int p = 0; p < nitem; p++)
		{
			ItemPointer ptid = BTreeTupleGetPostingN(itup, p);
			int			ptidcmp = -1;

			/*
			 * 这个嵌套循环在取自 itup 的各个 ptid TID 之间复用工作。
			 * 我们利用这样的事实：itup 的 TIDs 与 deltids
			 * 项（在单个 itup/倒排列表分组内）都必须
			 * 按 TID 升序排列。
			 */
			for (; nestedi < delstate->ndeltids; nestedi++)
			{
				TM_IndexDelete *tcdeltid = &delstate->deltids[nestedi];
				TM_IndexStatus *tdstatus = (delstate->status + tcdeltid->id);

				/* 一旦走过所有与 itup 相关的 deltids 项就停止 */
				Assert(tdstatus->idxoffnum >= idxoffnum);
				if (tdstatus->idxoffnum != idxoffnum)
					break;

				/* 事先跳过与 itup 相关、不可删除的项 */
				if (!tdstatus->knowndeletable)
					continue;

				/* 该项是第一个部分 ptid 匹配（或精确匹配）吗？ */
				ptidcmp = ItemPointerCompare(&tcdeltid->tid, ptid);
				if (ptidcmp >= 0)
				{
					/* 大于或等于（部分或精确）匹配…… */
					break;
				}
			}

			/* ……与可删除 deltids 项的精确 ptid 匹配？ */
			if (ptidcmp != 0)
				continue;

			/* 与可删除 deltids 项精确匹配——ptid 将被删除 */
			if (vacposting == NULL)
			{
				vacposting = palloc(offsetof(BTVacuumPostingData, deletetids) +
									nitem * sizeof(uint16));
				vacposting->itup = itup;
				vacposting->updatedoffset = idxoffnum;
				vacposting->ndeletedtids = 0;
			}
			vacposting->deletetids[vacposting->ndeletedtids++] = p;
		}

		/* 对 itup（一个倒排列表元组）的最终决定 */

		if (vacposting == NULL)
		{
			/* itup 中没有要删除的 TIDs——什么也不做 */
		}
		else if (vacposting->ndeletedtids == nitem)
		{
			/* 直接删除 itup（以删除全部 TIDs） */
			deletable[ndeletable++] = idxoffnum;
			/* 结果是我们不需要细粒度信息 */
			pfree(vacposting);
		}
		else
		{
			/* 从 itup 删除部分（但不是全部）TIDs */
			Assert(vacposting->ndeletedtids > 0 &&
				   vacposting->ndeletedtids < nitem);
			updatable[nupdatable++] = vacposting;
		}
	}

	/* 用 deletable（或 updatable）物理删除元组（或 TIDs） */
	_bt_delitems_delete(rel, buf, snapshotConflictHorizon, isCatalogRel,
						deletable, ndeletable, updatable, nupdatable);

	/* 收拾干净 */
	for (int i = 0; i < nupdatable; i++)
		pfree(updatable[i]);
}

/*
 * 检查 leftsib 页（目标页的 btpo_prev）没有被标上
 * INCOMPLETE_SPLIT 标志。页删除期间使用。
 *
 * 返回 true 表示 leftsib 上设置了该页标志（它
 * 肯定仍是目标的左兄弟）。发生这种情况时，
 * 目标在父页中没有下行链接，而页删除算法
 * 没有准备处理这一点。无法删除目标页（或包含
 * 目标页的整棵子树）。
 *
 * 调用者本身不应当持有目标页上的锁，因为同一层上的页
 * 必须总是从左到右加锁，以避免死锁。
 */
static bool
_bt_leftsib_splitflag(Relation rel, BlockNumber leftsib, BlockNumber target)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	bool		result;

	/* 简单情形：没有左兄弟 */
	if (leftsib == P_NONE)
		return false;

	buf = _bt_getbuf(rel, leftsib, BT_READ);
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);

	/*
	 * 若左兄弟被并发分裂，以至于它的 next 指针
	 * 不再指向当前页，则创建
	 * 目标的那次分裂必须已经完成。调用者可以合理地期望
	 * 会有一条指向目标页的下行链接，并能用其栈来重新定位。
	 * （在上一次分裂完成之前，我们不允许再次分裂一张
	 * 未完成分裂的页。）
	 */
	result = (opaque->btpo_next == target && P_INCOMPLETE_SPLIT(opaque));
	_bt_relbuf(rel, buf);

	return result;
}

/*
 * 检查 leafrightsib 页（目标叶子页的 btpo_next）没有
 * 被标上 ISHALFDEAD 标志。页删除期间使用。
 *
 * 返回 true 表示 leafrightsib 上设置了该页标志，因此页
 * 删除无法继续。调用者没有准备处理这种情形：
 * 父页没有枢轴元组，其下行链接指向
 * leafrightsib（由于更早一次被中断的 VACUUM 操作）。不值得
 * 费力去教调用者处理它。
 * 等 VACUUM 完成对该
 * 半死页的删除之后，局面就会解决（未来某次 VACUUM 操作再次到达目标页
 * 时）。
 *
 * _bt_leftsib_splitflag() 对叶子页和内部页都会调用。
 * 不过 _bt_rightsib_halfdeadflag() 只对叶子页调用。这
 * 没问题，因为对作为其父页最右页的页有删除限制
 * （即只有必须删除
 * 整棵子树时才能做这种删除）。这里做的叶子层检查将作用于
 * 右"堂兄弟"叶子页，而不是简单的右兄弟叶子页，
 * 当调用者实际上继续尝试删除位于
 * 叶子页之上的页时就是如此。右堂兄弟叶子页代表
 * 待删除子树右侧那棵子树的
 * 左缘，而这正是调用者关心的条件。
 * （此外，内部页从不会被标为半死，因此甚至
 * 不可能 _directly_ 判断某一内部页是否属于另一棵
 * 待删除子树。）
 */
static bool
_bt_rightsib_halfdeadflag(Relation rel, BlockNumber leafrightsib)
{
	Buffer		buf;
	Page		page;
	BTPageOpaque opaque;
	bool		result;

	Assert(leafrightsib != P_NONE);

	buf = _bt_getbuf(rel, leafrightsib, BT_READ);
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);

	Assert(P_ISLEAF(opaque) && !P_ISDELETED(opaque));
	result = P_ISHALFDEAD(opaque);
	_bt_relbuf(rel, buf);

	return result;
}

/*
 * _bt_pagedel() -- 若合法，就从 B-树删除一张叶子页。
 *
 * 这一动作把叶子页从 B-树结构中解除链接，去掉所有
 * 通向它的指针——但不碰它自己的左右链接。
 * 该页无法立刻被物理收回，因为其他进程
 * 可能正在跟随通向该页的链接；必须
 * 允许它们用它的右链接来恢复。见 nbtree/README。
 *
 * 进入时，目标缓冲区必须已钉住并锁定（读锁或写
 * 锁都可以）。该页必须是空叶子页，它可能已经
 * 半死（不过半死页只应在更早一次
 * VACUUM 操作被中断时传给我们）。特别要注意，调用者
 * 绝不应当在这里传入含有已存在的已删除页的缓冲区。
 * 返回之前会放下调用者缓冲区上的锁和钉住。
 *
 * 为调用者维护批量删除统计，这些统计取自 vstate。我们
 * 这里要与调用者密切配合，使整个 VACUUM 操作
 * 可靠地避免对我们顺带删除的、从属于 leafbuf 的页
 * 重复计数。若这些页的块号
 * 超前于当前 scanblkno 位置，则预期调用者
 * 稍后直接计数。由我们来理解调用者的
 * 要求，比让调用者理解一张
 * 已删除页事后是何时以及如何变成已删除的，要简单。
 *
 * 注意：这里会泄漏内存。与其试图把一切
 * 仔细清理干净，不如在可以频繁重置的临时上下文中
 * 运行它。
 */
void
_bt_pagedel(Relation rel, Buffer leafbuf, BTVacState *vstate)
{
	BlockNumber rightsib;
	bool		rightsib_empty;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * 保存调用者原来的 leafbuf 块号。只有
	 * <= scanblkno 的已删除块才计入批量删除统计的 pages_deleted
	 * 计数。
	 */
	BlockNumber scanblkno = BufferGetBlockNumber(leafbuf);

	/*
	 * "stack" 是通向（大约）目标页的搜索栈。
	 * 它最初为 NULL，但迭代时我们保留它，以避免
	 * 重复的搜索工作。
	 *
	 * 另外，当 "stack" 不为 NULL 时，我们已经检查过
	 * 当前页不是未完成分裂的右半，即
	 * 左兄弟没有设置 INCOMPLETE_SPLIT 标志，包括
	 * 当前目标页位于调用者初始页
	 * （scanblkno 页）右侧的情形。
	 */
	BTStack		stack = NULL;

	for (;;)
	{
		page = BufferGetPage(leafbuf);
		opaque = BTPageGetOpaque(page);

		/*
		 * 内部页从不被直接删除，只作为删除
		 * 一直下到叶子层的整棵子树的一部分。
		 *
		 * 这里也检查已删除页。调用者从不传给我们完全
		 * 删除的页。只有 VACUUM 能删除页，因此不可能发生过
		 * 并发删除。假定我们是通过跟随兄弟链接到达这里遇到的任何
		 * 已删除页的，并且
		 * 索引已损坏。
		 */
		Assert(!P_ISDELETED(opaque));
		if (!P_ISLEAF(opaque) || P_ISDELETED(opaque))
		{
			/*
			 * 9.4 之前的页删除只把内部页标为半死，
			 * 但现在我们只在叶子页上使用该标志。旧算法
			 * 本来就不该把半死页留在树里，它只是
			 * 一种瞬态，但在错误情形下仍然可能发生。
			 * 我们不知道在这里如何处理它们。就搜索而言它们
			 * 是无害的，但向已删除键空间插入
			 * 可能在更高层加入乱序的下行链接。记一条通知，
			 * 希望管理员会注意到
			 * 并重建索引。
			 */
			if (P_ISHALFDEAD(opaque))
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("index \"%s\" contains a half-dead internal page",
								RelationGetRelationName(rel)),
						 errhint("This can be caused by an interrupted VACUUM in version 9.3 or older, before upgrade. Please REINDEX it.")));

			if (P_ISDELETED(opaque))
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("found deleted block %u while following right link from block %u in index \"%s\"",
										 BufferGetBlockNumber(leafbuf),
										 scanblkno,
										 RelationGetRelationName(rel))));

			_bt_relbuf(rel, leafbuf);
			return;
		}

		/*
		 * 我们绝不能删除最右页或根页。同时，
		 * 检查该页为空，因为 leafbuf 页
		 * 片刻之前可能是空的，但此后可能有过插入。
		 *
		 * 为保持算法简单，我们也从不删除未完成
		 * 分裂的页（它们应当足够少，因此这对磁盘用量
		 * 不会有任何有意义的差别）：
		 *
		 * 页上的 INCOMPLETE_SPLIT 标志告诉我们该页是否是
		 * 未完成分裂的左半，但要确保它不是
		 * 右半就更复杂。为此，我们必须用
		 * _bt_leftsib_splitflag() 检查左兄弟没有设置 INCOMPLETE_SPLIT 标志。
		 * 在第一次迭代中，我们暂时
		 * 释放 scanblkno/leafbuf 上的锁，检查左兄弟，并
		 * 构造通向 scanblkno 的搜索栈。在后续迭代中，
		 * 我们知道是从通过了这些检查的页向右走来的，因此
		 * 没问题。
		 */
		if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) ||
			P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			P_INCOMPLETE_SPLIT(opaque))
		{
			/* 删除半死页绝不应当失败 */
			Assert(!P_ISHALFDEAD(opaque));

			_bt_relbuf(rel, leafbuf);
			return;
		}

		/*
		 * 首先，去掉指向该页的下行链接（若要删除更高的子树，
		 * 则是指向该页的父页的下行链接），并把
		 * leafbuf 页标为半死
		 */
		if (!P_ISHALFDEAD(opaque))
		{
			/*
			 * 我们需要一个指向该页父页的近似指针。我们
			 * 用标准搜索机制的一个变体来搜索
			 * 该页的高键；这将给我们一条链接，指向当前父页，
			 * 或指向它左侧的某处（若存在多个
			 * 相等的高键，这在 !heapkeyspace 索引上是可能的）。
			 *
			 * 还要检查这是否是未完成分裂的右半
			 * （见上面的注释）。
			 */
			if (!stack)
			{
				BTScanInsert itup_key;
				ItemId		itemid;
				IndexTuple	targetkey;
				BlockNumber leftsib,
							leafblkno;
				Buffer		sleafbuf;

				itemid = PageGetItemId(page, P_HIKEY);
				targetkey = CopyIndexTuple((IndexTuple) PageGetItem(page, itemid));

				leftsib = opaque->btpo_prev;
				leafblkno = BufferGetBlockNumber(leafbuf);

				/*
				 * 为避免死锁，最好先放下叶子页锁
				 * 再继续。
				 */
				_bt_unlockbuf(rel, leafbuf);

				/*
				 * 继续之前，检查 leafbuf 的左兄弟（若有）没有
				 * 被标上 INCOMPLETE_SPLIT 标志
				 */
				Assert(leafblkno == scanblkno);
				if (_bt_leftsib_splitflag(rel, leftsib, leafblkno))
				{
					ReleaseBuffer(leafbuf);
					return;
				}

				/* 搜索需要插入型扫描键，因此构造一个 */
				itup_key = _bt_mkscankey(rel, targetkey);
				/* 找到高键/枢轴匹配的最左叶子页 */
				itup_key->pivotsearch = true;
				stack = _bt_search(rel, NULL, itup_key, &sleafbuf, BT_READ,
								   NULL);
				/* 不需要在 leafbuf 上再取第二把锁或钉住 */
				_bt_relbuf(rel, sleafbuf);

				/*
				 * 重新锁住叶子页，并重新开始，以便在
				 * _bt_mark_page_halfdead 中使用我们的栈。必须那样做，
				 * 因为 leafbuf 可能已经不能再
				 * 删除。我们需要重新检查。
				 *
				 * 注意：不能简单地改持有 sleafbuf 的锁，
				 * 因为 sleafbuf 刚好可能不是
				 * 与 leafbuf 同一页。这发生在我们原来的锁被放下之后、
				 * _bt_search 完成下降之前，leafbuf 发生了分裂。
				 * 我们依赖这样的假定：在这种情形下会发现
				 * leafbuf 已经不再能安全删除。
				 * （页删除能应付栈位于 leafbuf 左侧，
				 * 但不能应付栈位于 leafbuf 右侧。）
				 */
				_bt_lockbuf(rel, leafbuf, BT_WRITE);
				continue;
			}

			/*
			 * 看删除叶子页是否安全，并确定叶子层之上
			 * 有多少父页/内部页将被
			 * 删除。若安全，则 _bt_mark_page_halfdead 还会
			 * 执行删除的第一阶段，其中包括把
			 * leafbuf 页标为半死。
			 */
			Assert(P_ISLEAF(opaque) && !P_IGNORE(opaque));
			if (!_bt_mark_page_halfdead(rel, vstate->info->heaprel, leafbuf,
										stack))
			{
				_bt_relbuf(rel, leafbuf);
				return;
			}
		}

		/*
		 * 然后把它从兄弟中解除链接。每次调用
		 * _bt_unlink_halfdead_page 都把子树最顶上的页解除链接，
		 * 使子树变矮。迭代直到 leafbuf 页被删除。
		 */
		rightsib_empty = false;
		Assert(P_ISLEAF(opaque) && P_ISHALFDEAD(opaque));
		while (P_ISHALFDEAD(opaque))
		{
			/* 在 _bt_unlink_halfdead_page 中检查中断 */
			if (!_bt_unlink_halfdead_page(rel, leafbuf, scanblkno,
										  &rightsib_empty, vstate))
			{
				/*
				 * _bt_unlink_halfdead_page 绝不应当失败，因为我们
				 * 已在 _bt_mark_page_halfdead 中确定删除总体上是安全的
				 * ——索引必定已损坏。
				 *
				 * 注意 _bt_unlink_halfdead_page 已经替我们释放了
				 * leafbuf 上的锁和钉住。
				 */
				Assert(false);
				return;
			}
		}

		Assert(P_ISLEAF(opaque) && P_ISDELETED(opaque));

		rightsib = opaque->btpo_next;

		_bt_relbuf(rel, leafbuf);

		/*
		 * 在这里检查，因为调用循环会持有锁，使
		 * 中断无法被处理。
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 该页现已被删除。若其右兄弟完全
		 * 为空，我们先前没有删除它的原因可能是
		 * 它曾是父页的最右子页。既然我们
		 * 去掉了指向这一页的下行链接，右兄弟现在可能是
		 * 父页的唯一子页，因而可以被删除。下一次清理反正会
		 * 把它捡起来，但不妨现在就尝试
		 * 删除它，因此循环回去处理右兄弟。
		 *
		 * 注意：这依赖这样的假定：_bt_getstackbuf()
		 * 能够用不同的子块复用我们原来的下降栈
		 * （只要该子块位于 _bt_search() 所到达的
		 * 原来叶子页的右侧）。每次循环它甚至会更新
		 * 下降栈，避免重复工作。
		 */
		if (!rightsib_empty)
			break;

		leafbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
	}
}

/*
 * 页删除的第一阶段。
 *
 * 确定待删除子树的高度，leafbuf 位于其
 * 最低层，去掉指向该子树的下行链接，并把 leafbuf
 * 标为半死。最终的待删除子树通常只是 leafbuf 本身，
 * 但可能还包括额外的内部页（根之下的每一层
 * 至多一页）。
 *
 * 调用者必须传入有效的 heaprel，因为我们
 * 调用 _bt_lock_subtree_parent 时刚好可能需要分配新的索引页，
 * 以完成一次页分裂。每次调用 _bt_allocbuf 都需要传入 heaprel。
 *
 * 若 leafbuf 不能安全删除，返回 'false'，通常是因为 leafbuf 是
 * 其父页的最右子页（且父页有不止一条下行链接）。
 * 页删除的第一阶段成功完成时
 * 返回 'true'。
 */
static bool
_bt_mark_page_halfdead(Relation rel, Relation heaprel, Buffer leafbuf,
					   BTStack stack)
{
	BlockNumber leafblkno;
	BlockNumber leafrightsib;
	BlockNumber topparent;
	BlockNumber topparentrightsib;
	ItemId		itemid;
	Page		page;
	BTPageOpaque opaque;
	Buffer		subtreeparent;
	OffsetNumber poffset;
	OffsetNumber nextoffset;
	IndexTuple	itup;
	IndexTupleData trunctuple;

	page = BufferGetPage(leafbuf);
	opaque = BTPageGetOpaque(page);

	Assert(!P_RIGHTMOST(opaque) && !P_ISROOT(opaque) &&
		   P_ISLEAF(opaque) && !P_IGNORE(opaque) &&
		   P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));
	Assert(heaprel != NULL);

	/*
	 * 保存关于叶子页的信息。
	 */
	leafblkno = BufferGetBlockNumber(leafbuf);
	leafrightsib = opaque->btpo_next;

	/*
	 * 在尝试锁住父页之前，检查右兄弟
	 * 不处于半死状态。半死的右兄弟在
	 * 父页中不会有下行链接，稍后我们
	 * 删除下行链接时会非常令人困惑。它通不过下面的交叉检查：
	 * "目标页的右兄弟也是父页中的下一个子页"。
	 */
	if (_bt_rightsib_halfdeadflag(rel, leafrightsib))
	{
		elog(DEBUG1, "could not delete page %u because its right sibling %u is half-dead",
			 leafblkno, leafrightsib);
		return false;
	}

	/*
	 * 我们无法删除作为其直接
	 * 父页最右子页的页，除非它是唯一的子页——这种情况下父页也必须
	 * 被删除，并且同样的条件递归地适用于它。在尝试删除之前，我们
	 * 必须一直向上检查这个条件，
	 * 并锁住待删除子树之根的父页（
	 * "子树父页"）。_bt_lock_subtree_parent() 为我们锁住子树父页。
	 * 下面我们从子树父页去掉指向"顶父页"（子树根
	 * 页）的下行链接。
	 *
	 * 现在把 topparent 初始化为 leafbuf 页。最终的待删除
	 * 子树常常是只由
	 * leafbuf 页组成的退化单页子树。发生这种情况时，leafbuf 页就是最终的子树
	 * 根页/顶父页。
	 */
	topparent = leafblkno;
	topparentrightsib = leafrightsib;
	if (!_bt_lock_subtree_parent(rel, heaprel, leafblkno, stack,
								 &subtreeparent, &poffset,
								 &topparent, &topparentrightsib))
		return false;

	page = BufferGetPage(subtreeparent);
	opaque = BTPageGetOpaque(page);

#ifdef USE_ASSERT_CHECKING

	/*
	 * 这只是断言，因为 _bt_lock_subtree_parent 应当已经
	 * 保证元组具有预期的内容
	 */
	itemid = PageGetItemId(page, poffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	Assert(BTreeTupleGetDownLink(itup) == topparent);
#endif

	nextoffset = OffsetNumberNext(poffset);
	itemid = PageGetItemId(page, nextoffset);
	itup = (IndexTuple) PageGetItem(page, itemid);

	/*
	 * 检查我们即将在子树父页中删除/覆盖的
	 * 父页索引项含有我们所期望的内容。若索引
	 * 因某种原因已损坏，这可能失败。发生这种情况时，我们退出
	 * 对 leafbuf 子树的删除。（这正像
	 * _bt_lock_subtree_parent() 无法"重新找到" leafbuf 下行链接的情形。）
	 */
	if (BTreeTupleGetDownLink(itup) != topparentrightsib)
	{
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling %u of block %u is not next child %u of block %u in index \"%s\"",
								 topparentrightsib, topparent,
								 BTreeTupleGetDownLink(itup),
								 BufferGetBlockNumber(subtreeparent),
								 RelationGetRelationName(rel))));

		_bt_relbuf(rel, subtreeparent);
		Assert(false);
		return false;
	}

	/*
	 * 本来会落到该叶子块上的任何插入，现在都会落到它的
	 * 右兄弟。换句话说，键空间向右移动。
	 */
	PredicateLockPageCombine(rel, leafblkno, leafrightsib);

	/* 变更记入日志之前不要 ereport(ERROR) */
	START_CRIT_SECTION();

	/*
	 * 更新子树的父页。我们要删除指向顶
	 * 父页/子树根的下行链接，以及*随后*的那个键。最简单的办法
	 * 是把右兄弟的下行链接复制到指向
	 * 顶父页的那条下行链接上，然后删除右兄弟原来的枢轴
	 * 元组。
	 *
	 * Lanin 与 Shasha 在删除一页时让键空间向左移动，
	 * 而这里键空间向右移动。因此我们无法简单地
	 * 删除带有指向顶父页下行链接的枢轴元组。见
	 * nbtree/README。
	 */
	page = BufferGetPage(subtreeparent);
	opaque = BTPageGetOpaque(page);

	itemid = PageGetItemId(page, poffset);
	itup = (IndexTuple) PageGetItem(page, itemid);
	BTreeTupleSetDownLink(itup, topparentrightsib);

	nextoffset = OffsetNumberNext(poffset);
	PageIndexTupleDelete(page, nextoffset);

	/*
	 * 把叶子页标为半死，并在上面盖上指向顶
	 * 父页的链接。当叶子页同时也是顶父页时，该链接
	 * 被设为 InvalidBlockNumber。
	 */
	page = BufferGetPage(leafbuf);
	opaque = BTPageGetOpaque(page);
	opaque->btpo_flags |= BTP_HALF_DEAD;

	Assert(PageGetMaxOffsetNumber(page) == P_HIKEY);
	MemSet(&trunctuple, 0, sizeof(IndexTupleData));
	trunctuple.t_info = sizeof(IndexTupleData);
	if (topparent != leafblkno)
		BTreeTupleSetTopParent(&trunctuple, topparent);
	else
		BTreeTupleSetTopParent(&trunctuple, InvalidBlockNumber);

	if (!PageIndexTupleOverwrite(page, P_HIKEY, (Item) &trunctuple,
								 IndexTupleSize(&trunctuple)))
		elog(ERROR, "could not overwrite high key in half-dead page");

	/* XLogInsert 之前必须把缓冲区标为脏 */
	MarkBufferDirty(subtreeparent);
	MarkBufferDirty(leafbuf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_mark_page_halfdead xlrec;
		XLogRecPtr	recptr;

		xlrec.poffset = poffset;
		xlrec.leafblk = leafblkno;
		if (topparent != leafblkno)
			xlrec.topparent = topparent;
		else
			xlrec.topparent = InvalidBlockNumber;

		XLogBeginInsert();
		XLogRegisterBuffer(0, leafbuf, REGBUF_WILL_INIT);
		XLogRegisterBuffer(1, subtreeparent, REGBUF_STANDARD);

		page = BufferGetPage(leafbuf);
		opaque = BTPageGetOpaque(page);
		xlrec.leftblk = opaque->btpo_prev;
		xlrec.rightblk = opaque->btpo_next;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeMarkPageHalfDead);

		recptr = XLogInsert(RM_BTREE_ID, XLOG_BTREE_MARK_PAGE_HALFDEAD);

		page = BufferGetPage(subtreeparent);
		PageSetLSN(page, recptr);
		page = BufferGetPage(leafbuf);
		PageSetLSN(page, recptr);
	}

	END_CRIT_SECTION();

	_bt_relbuf(rel, subtreeparent);
	return true;
}

/*
 * 页删除的第二阶段。
 *
 * 把正在删除的子树中的单页从其
 * 兄弟解除链接。同时把该页标为已删除。
 *
 * 要去掉整棵子树，包括叶子页本身，就在这里反复调用，
 * 直到叶子页被删除。第一阶段确立的原来的"顶父页"
 * 在这里的第一次调用中被删除，而
 * 叶子页在这里的最后一次调用中被删除。注意叶子页本身
 * 常常就是最初的顶父页。
 *
 * 若该页无法解除链接，返回 'false'（不应当发生）。若
 * 当前目标页的右兄弟为空，*rightsib_empty 被
 * 设为 true，允许调用者顺带删除目标的右兄弟页。
 * 注意 *rightsib_empty 实际上只在
 * 目标页是 leafbuf 时才被调用者使用，即对 leafbuf/包含
 * leafbuf 的子树在这里做完最后一次调用之后。（我们总是为调用者设置 *rightsib_empty，只是为了
 * 保持一致。）
 *
 * 进入时必须在 leafbuf 上持有钉住和锁（读或写无所谓）。
 * 成功退出时，我们将持有钉住和写锁。失败退出时，
 * 返回之前会释放钉住和锁（我们这样定义，
 * 是为了避免重新获取已经释放过的锁）。
 */
static bool
_bt_unlink_halfdead_page(Relation rel, Buffer leafbuf, BlockNumber scanblkno,
						 bool *rightsib_empty, BTVacState *vstate)
{
	BlockNumber leafblkno = BufferGetBlockNumber(leafbuf);
	IndexBulkDeleteResult *stats = vstate->stats;
	BlockNumber leafleftsib;
	BlockNumber leafrightsib;
	BlockNumber target;
	BlockNumber leftsib;
	BlockNumber rightsib;
	Buffer		lbuf = InvalidBuffer;
	Buffer		buf;
	Buffer		rbuf;
	Buffer		metabuf = InvalidBuffer;
	Page		metapg = NULL;
	BTMetaPageData *metad = NULL;
	ItemId		itemid;
	Page		page;
	BTPageOpaque opaque;
	FullTransactionId safexid;
	bool		rightsib_is_rightmost;
	uint32		targetlevel;
	IndexTuple	leafhikey;
	BlockNumber leaftopparent;

	page = BufferGetPage(leafbuf);
	opaque = BTPageGetOpaque(page);

	Assert(P_ISLEAF(opaque) && !P_ISDELETED(opaque) && P_ISHALFDEAD(opaque));

	/*
	 * 记住关于叶子页的一些信息。
	 */
	itemid = PageGetItemId(page, P_HIKEY);
	leafhikey = (IndexTuple) PageGetItem(page, itemid);
	target = BTreeTupleGetTopParent(leafhikey);
	leafleftsib = opaque->btpo_prev;
	leafrightsib = opaque->btpo_next;

	_bt_unlockbuf(rel, leafbuf);

	/*
	 * 在这里检查，因为调用循环会持有锁，使
	 * 中断无法被处理。
	 */
	CHECK_FOR_INTERRUPTS();

	/* 解除子树当前顶父页的链接 */
	if (!BlockNumberIsValid(target))
	{
		/* 目标是叶子页（或者，如果你愿意，叶子页就是顶父页） */
		target = leafblkno;

		buf = leafbuf;
		leftsib = leafleftsib;
		targetlevel = 0;
	}
	else
	{
		/* 目标是取自叶子顶父链接的内部页 */
		Assert(target != leafblkno);

		/* 取目标左兄弟的块号 */
		buf = _bt_getbuf(rel, target, BT_READ);
		page = BufferGetPage(buf);
		opaque = BTPageGetOpaque(page);
		leftsib = opaque->btpo_prev;
		targetlevel = opaque->btpo_level;
		Assert(targetlevel > 0);

		/*
		 * 为避免死锁，最好先放下目标页锁
		 * 再继续。
		 */
		_bt_unlockbuf(rel, buf);
	}

	/*
	 * 我们必须按标准顺序锁住需要修改的页：
	 * 先向右，再向上。否则会与其他写者死锁。
	 *
	 * 因此，若叶子页不是目标，先锁住它。然后找到并
	 * 写锁住目标页当前的左兄弟。片刻之前
	 * 还是当前兄弟的那一页可能已经分裂，因此我们可能必须向
	 * 右走。
	 */
	if (target != leafblkno)
		_bt_lockbuf(rel, leafbuf, BT_WRITE);
	if (leftsib != P_NONE)
	{
		lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
		page = BufferGetPage(lbuf);
		opaque = BTPageGetOpaque(page);
		while (P_ISDELETED(opaque) || opaque->btpo_next != target)
		{
			bool		leftsibvalid = true;

			/*
			 * 在跟随片刻之前还是左
			 * 兄弟的那一页上的链接之前，先验证它的右链接。这
			 * 减少了在索引损坏时循环永远无法取得任何
			 * 进展的机会。
			 *
			 * 注意：我们依赖这样的假定：同一时刻只能有一个
			 * 清理进程在运行（针对同一个索引）。
			 */
			if (P_RIGHTMOST(opaque) || P_ISDELETED(opaque) ||
				leftsib == opaque->btpo_next)
				leftsibvalid = false;

			leftsib = opaque->btpo_next;
			_bt_relbuf(rel, lbuf);

			if (!leftsibvalid)
			{
				/*
				 * 现场已知这会失败；兄弟链接损坏
				 * 相对常见。继续清理，而不是
				 * 直接抛出 ERROR。
				 */
				ereport(LOG,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg_internal("valid left sibling for deletion target could not be located: "
										 "left sibling %u of target %u with leafblkno %u and scanblkno %u on level %u of index \"%s\"",
										 leftsib, target, leafblkno, scanblkno,
										 targetlevel, RelationGetRelationName(rel))));

				/* 失败退出时必须释放全部钉住和锁 */
				ReleaseBuffer(buf);
				if (target != leafblkno)
					_bt_relbuf(rel, leafbuf);

				return false;
			}

			CHECK_FOR_INTERRUPTS();

			/* 向右走一页 */
			lbuf = _bt_getbuf(rel, leftsib, BT_WRITE);
			page = BufferGetPage(lbuf);
			opaque = BTPageGetOpaque(page);
		}
	}
	else
		lbuf = InvalidBuffer;

	/* 接下来写锁住目标页本身 */
	_bt_lockbuf(rel, buf, BT_WRITE);
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);

	/*
	 * 检查该页仍然为空等等，否则放弃删除。这只是出于
	 * 谨慎；半死页不可能复活，因为同一时刻只能有
	 * 一个清理进程在运行。
	 */
	if (P_RIGHTMOST(opaque) || P_ISROOT(opaque) || P_ISDELETED(opaque))
		elog(ERROR, "target page changed status unexpectedly in block %u of index \"%s\"",
			 target, RelationGetRelationName(rel));

	if (opaque->btpo_prev != leftsib)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("target page left link unexpectedly changed from %u to %u in block %u of index \"%s\"",
								 leftsib, opaque->btpo_prev, target,
								 RelationGetRelationName(rel))));

	if (target == leafblkno)
	{
		if (P_FIRSTDATAKEY(opaque) <= PageGetMaxOffsetNumber(page) ||
			!P_ISLEAF(opaque) || !P_ISHALFDEAD(opaque))
			elog(ERROR, "target leaf page changed status unexpectedly in block %u of index \"%s\"",
				 target, RelationGetRelationName(rel));

		/* 叶子页也是目标页：不要设置 leaftopparent */
		leaftopparent = InvalidBlockNumber;
	}
	else
	{
		IndexTuple	finaldataitem;

		if (P_FIRSTDATAKEY(opaque) != PageGetMaxOffsetNumber(page) ||
			P_ISLEAF(opaque))
			elog(ERROR, "target internal page on level %u changed status unexpectedly in block %u of index \"%s\"",
				 targetlevel, target, RelationGetRelationName(rel));

		/* 目标是内部页：为这里的下一次调用设置 leaftopparent……  */
		itemid = PageGetItemId(page, P_FIRSTDATAKEY(opaque));
		finaldataitem = (IndexTuple) PageGetItem(page, itemid);
		leaftopparent = BTreeTupleGetDownLink(finaldataitem);
		/* ……除非那会成为指向自身的冗余指针 */
		if (leaftopparent == leafblkno)
			leaftopparent = InvalidBlockNumber;
	}

	/* 第 0 层（叶子页）或第 1 层目标没有 leaftopparent */
	Assert(!BlockNumberIsValid(leaftopparent) || targetlevel > 1);

	/*
	 * 接下来写锁住（当前的）右兄弟。
	 */
	rightsib = opaque->btpo_next;
	rbuf = _bt_getbuf(rel, rightsib, BT_WRITE);
	page = BufferGetPage(rbuf);
	opaque = BTPageGetOpaque(page);

	/*
	 * 验证目标的右兄弟页。它的左链接必须指回
	 * 目标页。
	 */
	if (opaque->btpo_prev != target)
	{
		/*
		 * 现场已知这会失败；兄弟链接损坏
		 * 相对常见。继续清理，而不是直接
		 * 抛出 ERROR（与片刻之前对左兄弟右链接的
		 * 验证检查所用方法相同）。
		 */
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("right sibling's left-link doesn't match: "
								 "right sibling %u of target %u with leafblkno %u "
								 "and scanblkno %u spuriously links to non-target %u "
								 "on level %u of index \"%s\"",
								 rightsib, target, leafblkno,
								 scanblkno, opaque->btpo_prev,
								 targetlevel, RelationGetRelationName(rel))));

		/* 失败退出时必须释放全部钉住和锁 */
		if (BufferIsValid(lbuf))
			_bt_relbuf(rel, lbuf);
		_bt_relbuf(rel, rbuf);
		_bt_relbuf(rel, buf);
		if (target != leafblkno)
			_bt_relbuf(rel, leafbuf);

		return false;
	}

	rightsib_is_rightmost = P_RIGHTMOST(opaque);
	*rightsib_empty = (P_FIRSTDATAKEY(opaque) > PageGetMaxOffsetNumber(page));

	/*
	 * 若我们正在删除目标所在层的倒数第二页，则
	 * rightsib 有资格成为新的快速根。（理论上，
	 * 也许还能把快速根再往下推，但这样做的机会
	 * 很小，加锁方面的考虑也很棘手。）
	 *
	 * 这里可以安全地取得元页上的锁——见
	 * _bt_newlevel() 的注释。
	 */
	if (leftsib == P_NONE && rightsib_is_rightmost)
	{
		page = BufferGetPage(rbuf);
		opaque = BTPageGetOpaque(page);
		if (P_RIGHTMOST(opaque))
		{
			/* rightsib 将成为该层上剩下的唯一一页 */
			metabuf = _bt_getbuf(rel, BTREE_METAPAGE, BT_WRITE);
			metapg = BufferGetPage(metabuf);
			metad = BTPageGetMeta(metapg);

			/*
			 * 这里预期的情形是 btm_fastlevel == targetlevel+1；若
			 * fastlevel <= targetlevel，则有问题，我们
			 * 选择覆盖它来修复。
			 */
			if (metad->btm_fastlevel > targetlevel + 1)
			{
				/* 不需要更新 */
				_bt_relbuf(rel, metabuf);
				metabuf = InvalidBuffer;
			}
		}
	}

	/*
	 * 这里开始执行删除。
	 */

	/* 变更记入日志之前不要 ereport(ERROR) */
	START_CRIT_SECTION();

	/*
	 * 更新兄弟的侧向链接。注意目标页的侧向链接将
	 * 继续指向这些兄弟。这里的断言只是重新检查
	 * 我们上面已经验证过的事情。
	 */
	if (BufferIsValid(lbuf))
	{
		page = BufferGetPage(lbuf);
		opaque = BTPageGetOpaque(page);
		Assert(opaque->btpo_next == target);
		opaque->btpo_next = rightsib;
	}
	page = BufferGetPage(rbuf);
	opaque = BTPageGetOpaque(page);
	Assert(opaque->btpo_prev == target);
	opaque->btpo_prev = leftsib;

	/*
	 * 若我们删除的是目标叶子页的父页，而不是叶子
	 * 本身，就把叶子更新为指向子树中
	 * 下一个仍然存在的子页。
	 *
	 * 注意：我们依赖这样的事实：自 leafhikey 初始化以来，叶子页上的
	 * 缓冲区钉住一直被持有。这是安全的，不过只是
	 * 因为那时该页已经半死。在没有持有锁的
	 * 那段时间里，叶子页不可能被任何其他后端
	 * 修改。
	 */
	if (target != leafblkno)
		BTreeTupleSetTopParent(leafhikey, leaftopparent);

	/*
	 * 把该页本身标为已删除。当所有当前
	 * 事务都消失后，它就可以回收。存放 GetTopTransactionId() 也能工作，但
	 * 我们正处于 VACUUM 中，否则不会有 XID。既然已经
	 * 更新了指向目标的链接，ReadNextFullTransactionId() 作为
	 * 上界就够了。任何仍持有一条现已过时链接的扫描，都在其
	 * PGPROC 中公布一个小于或等于我们这里读到的值的 xmin。
	 * 在该扫描持续期间，它会继续这样做，从而拖住 xmin
	 * 视界。
	 */
	page = BufferGetPage(buf);
	opaque = BTPageGetOpaque(page);
	Assert(P_ISHALFDEAD(opaque) || !P_ISLEAF(opaque));

	/*
	 * 存放上界 XID，用来判断已删除页何时不再
	 * 需要充当墓碑
	 */
	safexid = ReadNextFullTransactionId();
	BTPageSetDeleted(page, safexid);
	opaque->btpo_cycleid = 0;

	/* 需要时也更新元页 */
	if (BufferIsValid(metabuf))
	{
		/* 需要时升级元页 */
		if (metad->btm_version < BTREE_NOVAC_VERSION)
			_bt_upgrademetapage(metapg);
		metad->btm_fastroot = rightsib;
		metad->btm_fastlevel = targetlevel;
		MarkBufferDirty(metabuf);
	}

	/* XLogInsert 之前必须把缓冲区标为脏 */
	MarkBufferDirty(rbuf);
	MarkBufferDirty(buf);
	if (BufferIsValid(lbuf))
		MarkBufferDirty(lbuf);
	if (target != leafblkno)
		MarkBufferDirty(leafbuf);

	/* XLOG 相关 */
	if (RelationNeedsWAL(rel))
	{
		xl_btree_unlink_page xlrec;
		xl_btree_metadata xlmeta;
		uint8		xlinfo;
		XLogRecPtr	recptr;

		XLogBeginInsert();

		XLogRegisterBuffer(0, buf, REGBUF_WILL_INIT);
		if (BufferIsValid(lbuf))
			XLogRegisterBuffer(1, lbuf, REGBUF_STANDARD);
		XLogRegisterBuffer(2, rbuf, REGBUF_STANDARD);
		if (target != leafblkno)
			XLogRegisterBuffer(3, leafbuf, REGBUF_WILL_INIT);

		/* 存放在目标/待解除链接块上的信息 */
		xlrec.leftsib = leftsib;
		xlrec.rightsib = rightsib;
		xlrec.level = targetlevel;
		xlrec.safexid = safexid;

		/* 重建叶子块所需的信息（若它不是目标） */
		xlrec.leafleftsib = leafleftsib;
		xlrec.leafrightsib = leafrightsib;
		xlrec.leaftopparent = leaftopparent;

		XLogRegisterData((char *) &xlrec, SizeOfBtreeUnlinkPage);

		if (BufferIsValid(metabuf))
		{
			XLogRegisterBuffer(4, metabuf, REGBUF_WILL_INIT | REGBUF_STANDARD);

			Assert(metad->btm_version >= BTREE_NOVAC_VERSION);
			xlmeta.version = metad->btm_version;
			xlmeta.root = metad->btm_root;
			xlmeta.level = metad->btm_level;
			xlmeta.fastroot = metad->btm_fastroot;
			xlmeta.fastlevel = metad->btm_fastlevel;
			xlmeta.last_cleanup_num_delpages = metad->btm_last_cleanup_num_delpages;
			xlmeta.allequalimage = metad->btm_allequalimage;

			XLogRegisterBufData(4, (char *) &xlmeta, sizeof(xl_btree_metadata));
			xlinfo = XLOG_BTREE_UNLINK_PAGE_META;
		}
		else
			xlinfo = XLOG_BTREE_UNLINK_PAGE;

		recptr = XLogInsert(RM_BTREE_ID, xlinfo);

		if (BufferIsValid(metabuf))
		{
			PageSetLSN(metapg, recptr);
		}
		page = BufferGetPage(rbuf);
		PageSetLSN(page, recptr);
		page = BufferGetPage(buf);
		PageSetLSN(page, recptr);
		if (BufferIsValid(lbuf))
		{
			page = BufferGetPage(lbuf);
			PageSetLSN(page, recptr);
		}
		if (target != leafblkno)
		{
			page = BufferGetPage(leafbuf);
			PageSetLSN(page, recptr);
		}
	}

	END_CRIT_SECTION();

	/* 释放元页 */
	if (BufferIsValid(metabuf))
		_bt_relbuf(rel, metabuf);

	/* 释放兄弟 */
	if (BufferIsValid(lbuf))
		_bt_relbuf(rel, lbuf);
	_bt_relbuf(rel, rbuf);

	/* 若目标不是 leafbuf，现在已经用完——释放它 */
	if (target != leafblkno)
		_bt_relbuf(rel, buf);

	/*
	 * 维护 pages_newly_deleted，它就是
	 * 正在进行的 VACUUM 操作所删除的页数。
	 *
	 * 维护 pages_deleted 时要考虑到
	 * btvacuumpage() 将如何计数那些尚未成为
	 * scanblkno 的已删除页——只有当该页稍后不会得到那种处理时
	 * 才计数。
	 */
	stats->pages_newly_deleted++;
	if (target <= scanblkno)
		stats->pages_deleted++;

	/*
	 * 把关于目标页（现在是新删除的页）的信息
	 * 记在专用的 vstate 空间里，供稍后使用。该页将在当前 btvacuumscan()
	 * 调用结束时被当作放入
	 * FSM 的候选。
	 */
	_bt_pendingfsm_add(vstate, target, safexid);

	/* 成功——继续持有 leafbuf 上的锁（它也可能曾是目标） */
	return true;
}

/*
 * 确定页删除第一阶段中待删除子树
 * 将有多高。
 *
 * 调用者的 child 参数是调用者想要删除的页的块号
 * （这是 leafbuf 的块号，除非我们被递归调用）。
 * stack 是通向它的搜索栈。注意我们将更新栈 entry(s)，
 * 以反映当前的下行链接位置——这与
 * 页分裂处理中的对应之处类似。
 *
 * 若"第一阶段"调用者无法继续删除 _any_ 页，返回
 * false。成功时返回 true，此时调用者可以使用这里确立的某些
 * 细节来执行删除的第一阶段。本函数是页删除
 * 仍可能被判定为不安全的最后一点
 * （索引损坏或意外的并发页删除除外）。
 *
 * 成功时我们为调用者写锁住待删除子树之根的父页
 * （即把 *subtreeparent 缓冲区上的锁留给调用者）。
 * 调用者必须从 *subtreeparent 去掉一条下行链接。我们
 * 还在 *poffset 中设置 *subtreeparent 的偏移号，以指出
 * 含有相关下行链接的枢轴元组的位置。
 *
 * 待删除子树的根称为"顶父页"。注意
 * leafbuf 页常常就是最终的"顶父页"（发生这种情况时，可以把
 * leafbuf 页看成一棵退化的单页子树）。
 * 调用者应当把 *topparent 初始化为目标 leafbuf 页的块号
 * （同时 *topparentrightsib 应设为 leafbuf 的右兄弟块号）。
 * 我们将在这里为调用者更新 *topparent（以及 *topparentrightsib），
 * 不过仅当结果是调用者将至少删除一页内部页时
 * （即仅当调用者需要用 BTreeTupleSetTopParent()
 * 在 leafbuf 页中存放一条指向顶父块的有效链接时）。
 */
static bool
_bt_lock_subtree_parent(Relation rel, Relation heaprel, BlockNumber child,
						BTStack stack, Buffer *subtreeparent,
						OffsetNumber *poffset, BlockNumber *topparent,
						BlockNumber *topparentrightsib)
{
	BlockNumber parent,
				leftsibparent;
	OffsetNumber parentoffset,
				maxoff;
	Buffer		pbuf;
	Page		page;
	BTPageOpaque opaque;

	/*
	 * 定位下行链接指向 "child" 的枢轴元组。写锁住
	 * 父页本身。
	 */
	pbuf = _bt_getstackbuf(rel, heaprel, stack, child);
	if (pbuf == InvalidBuffer)
	{
		/*
		 * 未能"重新找到"下行链接与我们的子
		 * 块号在父层上匹配的枢轴元组——索引必定已损坏。
		 * 甚至不要尝试删除 leafbuf 子树。只报告
		 * 问题，并继续清理该索引。
		 *
		 * 注意：_bt_getstackbuf() 能从发生在
		 * 父层上的并发页分裂中恢复。它的方法是近乎穷尽的
		 * 线性搜索。这还使它在操作符类有缺陷或不一致时
		 * 有出人意料的好机会恢复。但我们
		 * 在这里并不依赖这一点。
		 */
		ereport(LOG,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg_internal("failed to re-find parent key in index \"%s\" for deletion target page %u",
								 RelationGetRelationName(rel), child)));
		Assert(false);
		return false;
	}

	parent = stack->bts_blkno;
	parentoffset = stack->bts_offset;

	page = BufferGetPage(pbuf);
	opaque = BTPageGetOpaque(page);
	maxoff = PageGetMaxOffsetNumber(page);
	leftsibparent = opaque->btpo_prev;

	/*
	 * 需要时，_bt_getstackbuf() 会在返回的父缓冲区上
	 * 完成页分裂。
	 *
	 * 一般而言，VACUUM 再占用更多磁盘空间是个坏主意，这
	 * 就是页删除大多数时候并不完成未完成页分裂的原因。
	 * 我们允许这一有限例外，因为风险低得多，
	 * 而不继续下去的潜在坏处高得多：单独一页
	 * 设置了 INCOMPLETE_SPLIT 标志的内部页，否则可能
	 * 阻止我们删除下一层
	 * 成百上千的空叶子页。
	 */
	Assert(!P_INCOMPLETE_SPLIT(opaque));

	if (parentoffset < maxoff)
	{
		/*
		 * 子页不是父页中的最右子页，因此可以安全地删除
		 * 根/顶父页为该子页的子树
		 */
		*subtreeparent = pbuf;
		*poffset = parentoffset;
		return true;
	}

	/*
	 * 子页是父页的最右子页。
	 *
	 * 既然它是父页的最右子页，删除该子页（或
	 * 删除根/顶父页为该子页的子树）只有在
	 * 同时也可以删除父页时才安全。
	 */
	Assert(parentoffset == maxoff);
	if (parentoffset != P_FIRSTDATAKEY(opaque) || P_RIGHTMOST(opaque))
	{
		/*
		 * 子页不是父页的唯一子页，或者父页在其
		 * 整层上是最右的。肯定无法删除任何页。
		 */
		_bt_relbuf(rel, pbuf);
		return false;
	}

	/*
	 * 现在通过检查
	 * 子页的祖父页，确保删除父页本身是安全的。递归，把父页当作
	 * 子页传入（子页的祖父是再上一层的父页）。若
	 * 删除父页不安全，则删除子页也必定不安全（在
	 * 这种情况下调用者根本无法删除任何页）。
	 */
	*topparent = parent;
	*topparentrightsib = opaque->btpo_next;

	/*
	 * 递归之前先释放父页上的锁。
	 *
	 * 在递归调用锁住
	 * 祖父页之前释放父页上的页锁是可以的。内部页只有在子页
	 * 分裂时才能获得新项，但只要我们仍然持有
	 * leafbuf 页上的锁，那就不可能发生。
	 */
	_bt_relbuf(rel, pbuf);

	/*
	 * 递归之前，先检查父页的左兄弟（若有）没有
	 * 被标上 INCOMPLETE_SPLIT 标志（必须在放下
	 * 父页锁之后做）。
	 *
	 * 注意：这里故意不完成未完成的分裂。
	 */
	if (_bt_leftsib_splitflag(rel, leftsibparent, parent))
		return false;

	/* 递归以检查子页的祖父页 */
	return _bt_lock_subtree_parent(rel, heaprel, parent, stack->bts_parent,
								   subtreeparent, poffset,
								   topparent, topparentrightsib);
}

/*
 * 初始化 VACUUM 为 _bt_pendingfsm_finalize
 * 优化所使用的本地内存状态。
 *
 * 在 btvacuumscan() 开始时调用。调用者的 cleanuponly 参数
 * 表示正在进行的 VACUUM 没有（也不会）调用 btbulkdelete()。
 *
 * 我们期望在这里于 VACUUM 的顶层内存上下文中分配内存。
 * 工作缓冲区受基于 work_mem 的限制。当数组不能再在该限制内
 * 增长时，我们的策略是停止再保存额外的新删除页，
 * 同时照常处理
 * 能够放下的那些页。
 */
void
_bt_pendingfsm_init(Relation rel, BTVacState *vstate, bool cleanuponly)
{
	int64		maxbufsize;

	/*
	 * 在仅清理的情形下不必费心做优化——我们不期望
	 * 有任何新删除的页。此外，对 btvacuumscan() 的仅清理调用
	 * 之所以会发生，只是因为上一次 VACUUM 期间
	 * 这项优化没有奏效。
	 */
	if (cleanuponly)
		return;

	/*
	 * 限制数组的最大大小，以便始终尊重 work_mem。这里避免
	 * int 溢出。
	 */
	vstate->bufsize = 256;
	maxbufsize = (work_mem * 1024L) / sizeof(BTPendingFSM);
	maxbufsize = Min(maxbufsize, INT_MAX);
	maxbufsize = Min(maxbufsize, MaxAllocSize / sizeof(BTPendingFSM));
	/* work_mem 很小时保持清醒 */
	maxbufsize = Max(maxbufsize, vstate->bufsize);
	vstate->maxbufsize = maxbufsize;

	/* 分配缓冲区，并表明当前有 0 个待处理页 */
	vstate->pendingpages = palloc(sizeof(BTPendingFSM) * vstate->bufsize);
	vstate->npendingpages = 0;
}

/*
 * 把任何新删除的页（即正在进行的 VACUUM 操作期间
 * _bt_pagedel() 删除的页）放入空闲空间映射——不过仅当
 * 到现在这样做实际上已经安全时。
 *
 * 在 btvacuumscan() 结束时调用，就在空闲空间映射清理
 * 开始之前。
 *
 * 释放 _bt_pendingfsm_init() 分配的内存，若有的话。
 */
void
_bt_pendingfsm_finalize(Relation rel, BTVacState *vstate)
{
	IndexBulkDeleteResult *stats = vstate->stats;
	Relation	heaprel = vstate->info->heaprel;

	Assert(stats->pages_newly_deleted >= vstate->npendingpages);
	Assert(heaprel != NULL);

	if (vstate->npendingpages == 0)
	{
		/* 无事可做时只释放内存 */
		if (vstate->pendingpages)
			pfree(vstate->pendingpages);

		return;
	}

#ifdef DEBUG_BTREE_PENDING_FSM

	/*
	 * 调试辅助：睡眠 5 秒，以大大增加把
	 * 待处理页放入 FSM 的机会。注意，若没有某个其他后端并发消耗
	 * XID，这项优化
	 * 永远不会生效。
	 */
	pg_usleep(5000000L);
#endif

	/*
	 * 重新计算 VACUUM 的 XID 边界。
	 *
	 * 我们实际上并不关心最老的不可移除 XID。计算
	 * 这样一个最老的 XID 有一个我们依赖的有用副作用：它
	 * 强制更新本后端的 XID 视界状态。这一步
	 * 必不可少；没有这一步，GlobalVisCheckRemovableFullXid() 不会可靠地识别出
	 * 现在回收新删除的页已经安全。
	 */
	GetOldestNonRemovableTransactionId(heaprel);

	for (int i = 0; i < vstate->npendingpages; i++)
	{
		BlockNumber target = vstate->pendingpages[i].target;
		FullTransactionId safexid = vstate->pendingpages[i].safexid;

		/*
		 * 做与检查 BTPageIsRecyclable() 等价的事，但不再
		 * 第二次访问该页。
		 *
		 * 放弃寻找第一张不可回收的页——后面的页
		 * 也必定不可回收，因为 _bt_pendingfsm_add() 按
		 * safexid 顺序把页加入数组。
		 */
		if (!GlobalVisCheckRemovableFullXid(heaprel, safexid))
			break;

		RecordFreeIndexPage(rel, target);
		stats->pages_free++;
	}

	pfree(vstate->pendingpages);
}

/*
 * 维护当前 btvacuumscan()
 * 调用期间被删除的页的数组，供 _bt_pendingfsm_finalize() 使用
 */
static void
_bt_pendingfsm_add(BTVacState *vstate,
				   BlockNumber target,
				   FullTransactionId safexid)
{
	Assert(vstate->npendingpages <= vstate->bufsize);
	Assert(vstate->bufsize <= vstate->maxbufsize);

#ifdef USE_ASSERT_CHECKING

	/*
	 * 验证 _bt_pendingfsm_finalize() 所做的一个假定：数组中的页
	 * 将始终按 safexid 顺序排列（因为我们
	 * 在这里就是按这个顺序保存它们的）
	 */
	if (vstate->npendingpages > 0)
	{
		FullTransactionId lastsafexid =
			vstate->pendingpages[vstate->npendingpages - 1].safexid;

		Assert(FullTransactionIdFollowsOrEquals(safexid, lastsafexid));
	}
#endif

	/*
	 * 若临时缓冲区达到 maxbufsize/work_mem 容量，就丢掉
	 * 关于这一页的信息。
	 *
	 * 注意这也覆盖了我们在 _bt_pendingfsm_init() 中
	 * 选择不使用该优化的情形。
	 */
	if (vstate->npendingpages == vstate->maxbufsize)
		return;

	/* 考虑扩大缓冲区 */
	if (vstate->npendingpages == vstate->bufsize)
	{
		int			newbufsize = vstate->bufsize * 2;

		/* 尊重 work_mem */
		if (newbufsize > vstate->maxbufsize)
			newbufsize = vstate->maxbufsize;

		vstate->bufsize = newbufsize;
		vstate->pendingpages =
			repalloc(vstate->pendingpages,
					 sizeof(BTPendingFSM) * vstate->bufsize);
	}

	/* 保存新删除页的元数据 */
	vstate->pendingpages[vstate->npendingpages].target = target;
	vstate->pendingpages[vstate->npendingpages].safexid = safexid;
	vstate->npendingpages++;
}
