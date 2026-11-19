/*
 * Copyright (c) 1998, 1999 Henry Spencer.  All rights reserved.
 *
 * Development of this software was funded, in part, by Cray Research Inc.,
 * UUNET Communications Services Inc., Sun Microsystems Inc., and Scriptics
 * Corporation, none of whom are responsible for the results.  The author
 * thanks all of them.
 *
 * Redistribution and use in source and binary forms -- with or without
 * modification -- are permitted for any purpose, provided that
 * redistributions in source form retain this entire copyright notice and
 * indicate the origin and nature of any modifications.
 *
 * I'd appreciate being given credit for this package in the documentation
 * of software which uses it, but that is not a requirement.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * HENRY SPENCER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * src/include/regex/regcustom.h
 */

/* 头文件(有) */

/*
 * 按 Postgres 编码惯例，不宜在头文件中
 * 包含 postgres.h，但此处容许违规，因正则库
 * 文件特意指望本文件提供应用相关
 * 头文件，并谨慎地在一切之前先包含本文件。
 */
#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <wctype.h>

#include "mb/pg_wchar.h"

#include "miscadmin.h"			/* 供rstacktoodeep所需 */


/* 对 regguts.h 定义的覆盖，若有 */
#define FUNCPTR(name, args) (*name) args
#define MALLOC(n)		palloc_extended((n), MCXT_ALLOC_NO_OOM)
#define FREE(p)			pfree(VS(p))
#define REALLOC(p,n)	repalloc_extended(VS(p),(n), MCXT_ALLOC_NO_OOM)
#define INTERRUPT(re)	CHECK_FOR_INTERRUPTS()
#define assert(x)		Assert(x)

/* 内部字符类型及相关 */
typedef pg_wchar chr;			/* 类型本身 */
typedef unsigned uchr;			/* 可容纳 chr 的无符号类型 */

#define CHR(c)	((unsigned char) (c))	/* char 字面量转成 chr 字面量 */
#define DIGITVAL(c) ((c)-'0')	/* 把 chr 数字转为数值 */
#define CHRBITS 32				/* chr 中的位数；禁用 sizeof */
#define CHR_MIN 0x00000000		/* 最小与最大 chr；其值 */
#define CHR_MAX 0x7ffffffe		/* CHR_MAX-CHR_MIN+1 须装入 int，且
								 * CHR_MAX+1 须装入 chr 变量*/

/*
 * 检查 chr 值是否在范围内。理想写法是
 *		((c) >= CHR_MIN && (c) <= CHR_MAX)
 * 然而，若 chr 无符号且 CHR_MIN 为零，则前半
 * 是空操作，某些过分保姆式编译器会对它告警。
 * 故此处略去。若欲使 chr 有符号且/或 CHR_MIN 
 * 非零，则按上式重定义本宏。调用方应假定
 * 该宏可能多次求值其参数，纵然今日并未如此。
 */
#define CHR_IS_IN_RANGE(c)	((c) <= CHR_MAX)

/*
 * MAX_SIMPLE_CHR 是色图逻辑中"简单"与"复杂"
 * 处理的分界。通常应选得足够高，以确保
 * 常见字符皆 <= MAX_SIMPLE_CHR。但过大
 * 的值适得其反，因其增加正则准备时间。
 * 另，较小值有助于测试高色图逻辑，
 * 用普通 ASCII 输入。
 */
#define MAX_SIMPLE_CHR	0x7FF	/* 适于 Unicode 的取值 */

/* 作用于 chr 的函数 */
#define iscalnum(x) pg_wc_isalnum(x)
#define iscalpha(x) pg_wc_isalpha(x)
#define iscdigit(x) pg_wc_isdigit(x)
#define iscspace(x) pg_wc_isspace(x)

/* 并纳入标准头文件 */
#include "regex.h"
