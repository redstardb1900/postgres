/*-------------------------------------------------------------------------
 *
 * fsm_internals.h
 *	  空闲空间映射的内部函数
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fsm_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FSM_INTERNALS_H
#define FSM_INTERNALS_H

#include "storage/buf.h"
#include "storage/bufpage.h"

/*
 * FSM 页结构。详见 src/backend/storage/freespace/README
 * details.
 */
typedef struct
{
	/*
	 * fsm_search_avail() 试图用轮转方式把负载分散到
	 * 多个后端：向不同后端返回不同页。
	 * fp_next_slot 指向下次要返回的槽（假定其上
	 * 有足够空间满足请求）。定义为 int，因为会在
	 * 无独占锁下更新。uint16 更合适，但 int 更
	 * 易于原子
	 * 取/存。
	 */
	int			fp_next_slot;

	/*
	 * fp_nodes 含以数组存放的二叉树。前 
	 * NonLeafNodesPerPage 个是上层结点，随后 
	 * LeafNodesPerPage 个是叶结点。未用结点为零。
	 */
	uint8		fp_nodes[FLEXIBLE_ARRAY_MEMBER];
} FSMPageData;

typedef FSMPageData *FSMPage;

/*
 * FSM 页上非叶结点、叶结点及结点总数。
 * 这些定义仅供 fsmpage.c 内部使用。
 */
#define NodesPerPage (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) - \
					  offsetof(FSMPageData, fp_nodes))

#define NonLeafNodesPerPage (BLCKSZ / 2 - 1)
#define LeafNodesPerPage (NodesPerPage - NonLeafNodesPerPage)

/*
 * FSM 页上的 FSM "槽" 数。应在 fsmpage.c 
 * 之外使用。
 */
#define SlotsPerFSMPage LeafNodesPerPage

/* fsmpage.c 中函数原型 */
extern int	fsm_search_avail(Buffer buf, uint8 minvalue, bool advancenext,
							 bool exclusive_lock_held);
extern uint8 fsm_get_avail(Page page, int slot);
extern uint8 fsm_get_max_avail(Page page);
extern bool fsm_set_avail(Page page, int slot, uint8 value);
extern bool fsm_truncate_avail(Page page, int nslots);
extern bool fsm_rebuild_page(Page page);

#endif							/* FSM_INTERNALS_H */
