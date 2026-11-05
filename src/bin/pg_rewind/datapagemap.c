/*-------------------------------------------------------------------------
 *
 * datapagemap.c
 *	  A data structure for keeping track of data pages that have changed.
 *
 * This is a fairly simple bitmap.
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "common/logging.h"
#include "datapagemap.h"

struct datapagemap_iterator
{
	datapagemap_t *map;
	BlockNumber nextblkno;
};

/*****
 * 公共函数
             */

/*
 * 向位图添加一个块。
                  */
void
datapagemap_add(datapagemap_t *map, BlockNumber blkno)
{
	int			offset;
	int			bitno;

	offset = blkno / 8;
	bitno = blkno % 8;

	/* 按需扩展或创建位图                          */
	if (map->bitmapsize <= offset)
	{
		int			oldsize = map->bitmapsize;
		int			newsize;

		/*
		 * 容纳新位至少要 offset + 1。但要多留余量，   
		 * 以免在块按顺序修改（从关系开头到结尾）这一    
		 * 常见情形下反复扩展位图。                             
		 *                          
		 */
		newsize = offset + 1;
		newsize += 10;

		map->bitmap = pg_realloc(map->bitmap, newsize);

		/* 将新分配区域清零                            */
		memset(&map->bitmap[oldsize], 0, newsize - oldsize);

		map->bitmapsize = newsize;
	}

	/* 设置该位        */
	map->bitmap[offset] |= (1 << bitno);
}

/*
 * 开始遍历页面映射中的所有条目。
 *
 * datapagemap_iterate 后调用 datapagemap_next 返回
 * 条目，直到返回 false。完成后用 pg_free() 销毁
 * 迭代器。
                                                                                                               */
datapagemap_iterator_t *
datapagemap_iterate(datapagemap_t *map)
{
	datapagemap_iterator_t *iter;

	iter = pg_malloc(sizeof(datapagemap_iterator_t));
	iter->map = map;
	iter->nextblkno = 0;

	return iter;
}

bool
datapagemap_next(datapagemap_iterator_t *iter, BlockNumber *blkno)
{
	datapagemap_t *map = iter->map;

	for (;;)
	{
		BlockNumber blk = iter->nextblkno;
		int			nextoff = blk / 8;
		int			bitno = blk % 8;

		if (nextoff >= map->bitmapsize)
			break;

		iter->nextblkno++;

		if (map->bitmap[nextoff] & (1 << bitno))
		{
			*blkno = blk;
			return true;
		}
	}

	/* 此位图中没有更多已设置的位。                   */
	return false;
}

/*
 * 调试辅助工具。打印页面映射的内容。
                                         */
void
datapagemap_print(datapagemap_t *map)
{
	datapagemap_iterator_t *iter;
	BlockNumber blocknum;

	iter = datapagemap_iterate(map);
	while (datapagemap_next(iter, &blocknum))
		pg_log_debug("block %u", blocknum);

	pg_free(iter);
}
