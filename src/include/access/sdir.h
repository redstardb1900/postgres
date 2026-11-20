/*-------------------------------------------------------------------------
 *
 * sdir.h
 *	  POSTGRES 扫描方向定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/sdir.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SDIR_H
#define SDIR_H


/*
 * 定义表或索引的扫描方向。扫描从不
 * 以 NoMovementScanDirectionScans 调用。为方便，我们用
 * 值 -1 与 1 表示向后与向前扫描。这使我们能做
 * 若干数学技巧，如 ScanDirectionCombine 中所做的。
 */
typedef enum ScanDirection
{
	BackwardScanDirection = -1,
	NoMovementScanDirection = 0,
	ForwardScanDirection = 1
} ScanDirection;

/*
 * 确定两个方向指定的净效果。
 * 这依赖 ForwardScanDirection = +1、BackwardScanDirection = -1，
 * 且用于任何其他值时大概不会得到你想要的结果。
 */
#define ScanDirectionCombine(a, b)  ((a) * (b))

/*
 * ScanDirectionIsValid
 *		扫描方向有效时才为真。
 */
#define ScanDirectionIsValid(direction) \
	((bool) (BackwardScanDirection <= (direction) && \
			 (direction) <= ForwardScanDirection))

/*
 * ScanDirectionIsBackward
 *		扫描方向为向后时才为真。
 */
#define ScanDirectionIsBackward(direction) \
	((bool) ((direction) == BackwardScanDirection))

/*
 * ScanDirectionIsNoMovement
 *		扫描方向表示无移动时才为真。
 */
#define ScanDirectionIsNoMovement(direction) \
	((bool) ((direction) == NoMovementScanDirection))

/*
 * ScanDirectionIsForward
 *		扫描方向为向前才为真。
 */
#define ScanDirectionIsForward(direction) \
	((bool) ((direction) == ForwardScanDirection))

#endif							/* SDIR_H */
