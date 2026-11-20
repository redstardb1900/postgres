/*-------------------------------------------------------------------------
 *
 * rmgrdesc_utils.h
 *	  rmgrdesc 例程的支持函数
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 * src/include/access/rmgrdesc_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RMGRDESC_UTILS_H_
#define RMGRDESC_UTILS_H_

/*
 * rmgrdesc 例程作者指南：
 *
 * 这些指南旨在避免各 rmgr 之间无谓的不一致，
 * 并让用户能不太费力地解析 desc 输出串。
 * 这不是 API 规范，也不是交换格式。
 * (反正目前仅 heapam 与 nbtree 的 desc 例程遵循这些
 * 指南。)
 *
 * 记录描述类似于 JSON 风格的键/值对象。但
 * 没有显式的 "string" 类型/字符串转义。顶层 { } 
 * 括号应予省略。例如：
 *
 * snapshotConflictHorizon: 0, flags: 0x03
 *
 * 记录描述可含可变长数组。例如：
 *
 * nunused: 5, unused: [1, 2, 3, 4, 5]
 *
 * 嵌套对象由 { } 括号支持。它们一般出现
 * 于可变长数组内。例如：
 *
 * ndeleted: 0, nupdated: 1, deleted: [], updated: [{ off: 45, nptids: 1, ptids: [0] }]
 *
 * 尽量按如实反映底层物理 WAL 记录
 * 结构体字段顺序的次序输出。键名应在
 * 同一嵌套层级内唯一，以便于解析。宜让
 * 数组项数出现在数组之前。
 *
 * 个别 WAL 记录类型可以自创约定。
 * 例如，Heap2 的 PRUNE 记录描述用自定义数组格式
 * 对其 "redirected" 字段：
 *
 * ... redirected: [1->4, 5->9], dead: [10, 11], unused: [3, 7, 8]
 *
 * 亦可说 desc 例程对此改用对象记法更好。
 * 但自定义格式若能传达有用的底层物理
 * 数据结构信息，则仍有价值。
 *
 * 此特设格式的优点是接近 "dead" 与 "unused" 
 * 数组所用格式（它们遵循页偏移号数组的
 * 标准 desc 约定）。这暗示所示 "redirected" 
 * 元素只是页偏移号对（实际即如此）。
 */
extern void array_desc(StringInfo buf, void *array, size_t elem_size, int count,
					   void (*elem_desc) (StringInfo buf, void *elem, void *data),
					   void *data);
extern void offset_elem_desc(StringInfo buf, void *offset, void *data);
extern void redirect_elem_desc(StringInfo buf, void *offset, void *data);
extern void oid_elem_desc(StringInfo buf, void *relid, void *data);

#endif							/* RMGRDESC_UTILS_H_ */
