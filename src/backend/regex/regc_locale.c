/*
 * regc_locale.c --
 *
 *	本文件含区域设置相关的正则例程。
 *	本文件被 regcomp.c #include。
 *
 * Copyright (c) 1998 by Scriptics Corporation.
 *
 * This software is copyrighted by the Regents of the University of
 * California, Sun Microsystems, Inc., Scriptics Corporation, ActiveState
 * Corporation and other parties.  The following terms apply to all files
 * associated with the software unless explicitly disclaimed in
 * individual files.
 *
 * The authors hereby grant permission to use, copy, modify, distribute,
 * and license this software and its documentation for any purpose, provided
 * that existing copyright notices are retained in all copies and that this
 * notice is included verbatim in any distributions. No written agreement,
 * license, or royalty fee is required for any of the authorized uses.
 * Modifications to this software may be copyrighted by their authors
 * and need not follow the licensing terms described here, provided that
 * the new terms are clearly indicated on the first page of each file where
 * they apply.
 *
 * IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY
 * FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY
 * DERIVATIVES THEREOF, EVEN IF THE AUTHORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  THIS SOFTWARE
 * IS PROVIDED ON AN "AS IS" BASIS, AND THE AUTHORS AND DISTRIBUTORS HAVE
 * NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
 * MODIFICATIONS.
 *
 * GOVERNMENT USE: If you are acquiring this software on behalf of the
 * U.S. government, the Government shall have only "Restricted Rights"
 * in the software and related documentation as defined in the Federal
 * Acquisition Regulations (FARs) in Clause 52.227.19 (c) (2).  If you
 * are acquiring the software on behalf of the Department of Defense, the
 * software shall be classified as "Commercial Computer Software" and the
 * Government shall have only "Restricted Rights" as defined in Clause
 * 252.227-7013 (c) (1) of DFARs.  Notwithstanding the foregoing, the
 * authors grant the U.S. Government and others acting in its behalf
 * permission to use and distribute the software in accordance with the
 * terms specified in this license.
 *
 * src/backend/regex/regc_locale.c
 */

/* ASCII 字符名表 */

static const struct cname
{
	const char *name;
	const char	code;
}			cnames[] =

{
	{
		"NUL", '\0'
	},
	{
		"SOH", '\001'
	},
	{
		"STX", '\002'
	},
	{
		"ETX", '\003'
	},
	{
		"EOT", '\004'
	},
	{
		"ENQ", '\005'
	},
	{
		"ACK", '\006'
	},
	{
		"BEL", '\007'
	},
	{
		"alert", '\007'
	},
	{
		"BS", '\010'
	},
	{
		"backspace", '\b'
	},
	{
		"HT", '\011'
	},
	{
		"tab", '\t'
	},
	{
		"LF", '\012'
	},
	{
		"newline", '\n'
	},
	{
		"VT", '\013'
	},
	{
		"vertical-tab", '\v'
	},
	{
		"FF", '\014'
	},
	{
		"form-feed", '\f'
	},
	{
		"CR", '\015'
	},
	{
		"carriage-return", '\r'
	},
	{
		"SO", '\016'
	},
	{
		"SI", '\017'
	},
	{
		"DLE", '\020'
	},
	{
		"DC1", '\021'
	},
	{
		"DC2", '\022'
	},
	{
		"DC3", '\023'
	},
	{
		"DC4", '\024'
	},
	{
		"NAK", '\025'
	},
	{
		"SYN", '\026'
	},
	{
		"ETB", '\027'
	},
	{
		"CAN", '\030'
	},
	{
		"EM", '\031'
	},
	{
		"SUB", '\032'
	},
	{
		"ESC", '\033'
	},
	{
		"IS4", '\034'
	},
	{
		"FS", '\034'
	},
	{
		"IS3", '\035'
	},
	{
		"GS", '\035'
	},
	{
		"IS2", '\036'
	},
	{
		"RS", '\036'
	},
	{
		"IS1", '\037'
	},
	{
		"US", '\037'
	},
	{
		"space", ' '
	},
	{
		"exclamation-mark", '!'
	},
	{
		"quotation-mark", '"'
	},
	{
		"number-sign", '#'
	},
	{
		"dollar-sign", '$'
	},
	{
		"percent-sign", '%'
	},
	{
		"ampersand", '&'
	},
	{
		"apostrophe", '\''
	},
	{
		"left-parenthesis", '('
	},
	{
		"right-parenthesis", ')'
	},
	{
		"asterisk", '*'
	},
	{
		"plus-sign", '+'
	},
	{
		"comma", ','
	},
	{
		"hyphen", '-'
	},
	{
		"hyphen-minus", '-'
	},
	{
		"period", '.'
	},
	{
		"full-stop", '.'
	},
	{
		"slash", '/'
	},
	{
		"solidus", '/'
	},
	{
		"zero", '0'
	},
	{
		"one", '1'
	},
	{
		"two", '2'
	},
	{
		"three", '3'
	},
	{
		"four", '4'
	},
	{
		"five", '5'
	},
	{
		"six", '6'
	},
	{
		"seven", '7'
	},
	{
		"eight", '8'
	},
	{
		"nine", '9'
	},
	{
		"colon", ':'
	},
	{
		"semicolon", ';'
	},
	{
		"less-than-sign", '<'
	},
	{
		"equals-sign", '='
	},
	{
		"greater-than-sign", '>'
	},
	{
		"question-mark", '?'
	},
	{
		"commercial-at", '@'
	},
	{
		"left-square-bracket", '['
	},
	{
		"backslash", '\\'
	},
	{
		"reverse-solidus", '\\'
	},
	{
		"right-square-bracket", ']'
	},
	{
		"circumflex", '^'
	},
	{
		"circumflex-accent", '^'
	},
	{
		"underscore", '_'
	},
	{
		"low-line", '_'
	},
	{
		"grave-accent", '`'
	},
	{
		"left-brace", '{'
	},
	{
		"left-curly-bracket", '{'
	},
	{
		"vertical-line", '|'
	},
	{
		"right-brace", '}'
	},
	{
		"right-curly-bracket", '}'
	},
	{
		"tilde", '~'
	},
	{
		"DEL", '\177'
	},
	{
		NULL, 0
	}
};

/*
 * 以下数组定义合法的字符类名。
 * 各项须与 regguts.h 中 enum char_classes 一致。
 */
static const char *const classNames[NUM_CCLASSES + 1] = {
	"alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph",
	"lower", "print", "punct", "space", "upper", "xdigit", "word",
	NULL
};

/*
 * 我们不用 Tcl 那种硬接线的 Unicode 分类表。
 * 这是因为 (a) 我们还需处理 Unicode 以外的编码，
 * 且 (b) 我们想尽量紧跟 libc 区域设置例程的
 * 行为。例如，某区域设置不把每个 Unicode 字母
 * 都当字母也不无理。故即便对 Unicode，我们也
 * 靠询问 libc 来构建字符分类 cvec。
 */


/*
 * element - 将排序元素名映射为 chr
 */
static chr
element(struct vars *v,			/* 语境 */
		const chr *startp,		/* 指向名称起点 */
		const chr *endp)		/* 指向刚过名末之处 */
{
	const struct cname *cn;
	size_t		len;

	/* 通例：单 chr 名即表示自身 */
	assert(startp < endp);
	len = endp - startp;
	if (len == 1)
		return *startp;

	NOTE(REG_ULOCALE);

	/* 搜索表 */
	for (cn = cnames; cn->name != NULL; cn++)
	{
		if (strlen(cn->name) == len &&
			pg_char_and_wchar_strncmp(cn->name, startp, len) == 0)
		{
			break;				/* 注意跳出 */
		}
	}
	if (cn->name != NULL)
		return CHR(cn->code);

	/* 找不到它 */
	ERR(REG_ECOLLATE);
	return 0;
}

/*
 * range - 为范围提供 cvec，含合法性检查
 */
static struct cvec *
range(struct vars *v,			/* 语境 */
	  chr a,					/* 范围始 */
	  chr b,					/* 范围末，可等于 a */
	  int cases)				/* 大小写无关? */
{
	int			nchrs;
	struct cvec *cv;
	chr			c,
				cc;

	if (a != b && !before(a, b))
	{
		ERR(REG_ERANGE);
		return NULL;
	}

	if (!cases)
	{							/* 简易版 */
		cv = getcvec(v, 0, 1);
		NOERRN();
		addrange(cv, a, b);
		return cv;
	}

	/*
	 * 大小写无关时，很难判断何时可用 cvec 范围，
	 * 故至少目前我们不尝试。我们对原先指定的
	 * chr 用一范围，再把该范围外的大小写等价者
	 * 作为个别 chr 追加。
	 *
	 * 为确保有人指定极大范围时行为正常，将分配
	 * 大小限制为 100000 个 chr（任意），并在下方
	 * 循环内查overrun。
	 */
	nchrs = b - a + 1;
	if (nchrs <= 0 || nchrs > 100000)
		nchrs = 100000;

	cv = getcvec(v, nchrs, 1);
	NOERRN();
	addrange(cv, a, b);

	for (c = a; c <= b; c++)
	{
		cc = pg_wc_tolower(c);
		if (cc != c &&
			(before(cc, a) || before(b, cc)))
		{
			if (cv->nchrs >= cv->chrspace)
			{
				ERR(REG_ETOOBIG);
				return NULL;
			}
			addchr(cv, cc);
		}
		cc = pg_wc_toupper(c);
		if (cc != c &&
			(before(cc, a) || before(b, cc)))
		{
			if (cv->nchrs >= cv->chrspace)
			{
				ERR(REG_ETOOBIG);
				return NULL;
			}
			addchr(cv, cc);
		}
		INTERRUPT(v->re);
	}

	return cv;
}

/*
 * before - chr x 是否在 chr y 之前（范围合法性）？
 */
static int						/* 谓词 */
before(chr x, chr y)
{
	if (x < y)
		return 1;
	return 0;
}

/*
 * eclass - 为等价类提供 cvec
 * 按请求须含大小写对应者。
 */
static struct cvec *
eclass(struct vars *v,			/* 语境 */
	   chr c,					/* 代表该等价类的
								 * 排序元素。*/
	   int cases)				/* 大小写? */
{
	struct cvec *cv;

	/* 供测试的粗糙伪等价类 */
	if ((v->cflags & REG_FAKE) && c == 'x')
	{
		cv = getcvec(v, 4, 0);
		addchr(cv, CHR('x'));
		addchr(cv, CHR('y'));
		if (cases)
		{
			addchr(cv, CHR('X'));
			addchr(cv, CHR('Y'));
		}
		return cv;
	}

	/* 否则无 */
	if (cases)
		return allcases(v, c);
	cv = getcvec(v, 1, 0);
	assert(cv != NULL);
	addchr(cv, c);
	return cv;
}

/*
 * lookupcclass - 按名查找字符类
 *
 * 失败时在 *v 中置错误码；结果随之为垃圾。
 */
static enum char_classes
lookupcclass(struct vars *v,	/* 语境（返回错误） */
			 const chr *startp, /* 名称起点 */
			 const chr *endp)	/* 刚过名称末尾 */
{
	size_t		len;
	const char *const *namePtr;
	int			i;

	/*
	 * 将名称映射到对应的枚举值。
	 */
	len = endp - startp;
	for (namePtr = classNames, i = 0; *namePtr != NULL; namePtr++, i++)
	{
		if (strlen(*namePtr) == len &&
			pg_char_and_wchar_strncmp(*namePtr, startp, len) == 0)
			return (enum char_classes) i;
	}

	ERR(REG_ECTYPE);
	return (enum char_classes) 0;
}

/*
 * cclasscvec - 为字符类提供 cvec
 *
 * 若 "cases" 为真则须含大小写对应者。
 *
 * 返回的 cvec 可能是 getcvec() 取得的暂态 cvec，
 * 或 pg_ctype_get_cache() 的永久缓存者。均可，
 * 因两种情形调用方都不应显式释放结果。
 */
static struct cvec *
cclasscvec(struct vars *v,		/* 语境 */
		   enum char_classes cclasscode,	/* 要为其建 cvec 的类 */
		   int cases)			/* 大小写无关? */
{
	struct cvec *cv = NULL;

	/*
	 * 若大小写不敏感，将 lower/upper 重映射为 alpha。
	 */

	if (cases &&
		(cclasscode == CC_LOWER ||
		 cclasscode == CC_UPPER))
		cclasscode = CC_ALPHA;

	/*
	 * 现在计算字符类内容。对基于 <wctype.h> 或
	 * <ctype.h> 函数行为的类，我们用
	 * pg_ctype_get_cache 以便缓存结果。其他类
	 * 在此硬接线定义，对那些我们只是
	 * 当场构造暂态 cvec。
	 *
	 * 须与下方 cclass_column_index() 保持同步。
	 */

	switch (cclasscode)
	{
		case CC_PRINT:
			cv = pg_ctype_get_cache(pg_wc_isprint, cclasscode);
			break;
		case CC_ALNUM:
			cv = pg_ctype_get_cache(pg_wc_isalnum, cclasscode);
			break;
		case CC_ALPHA:
			cv = pg_ctype_get_cache(pg_wc_isalpha, cclasscode);
			break;
		case CC_WORD:
			cv = pg_ctype_get_cache(pg_wc_isword, cclasscode);
			break;
		case CC_ASCII:
			/* 硬接线含义 */
			cv = getcvec(v, 0, 1);
			if (cv)
				addrange(cv, 0, 0x7f);
			break;
		case CC_BLANK:
			/* 硬接线含义 */
			cv = getcvec(v, 2, 0);
			addchr(cv, '\t');
			addchr(cv, ' ');
			break;
		case CC_CNTRL:
			/* 硬接线含义 */
			cv = getcvec(v, 0, 2);
			addrange(cv, 0x0, 0x1f);
			addrange(cv, 0x7f, 0x9f);
			break;
		case CC_DIGIT:
			cv = pg_ctype_get_cache(pg_wc_isdigit, cclasscode);
			break;
		case CC_PUNCT:
			cv = pg_ctype_get_cache(pg_wc_ispunct, cclasscode);
			break;
		case CC_XDIGIT:

			/*
			 * 非西方区域设置下如何定义这一点并不清楚，
			 * 尝试是否有何用处就更不清楚。故
			 * 就硬接线其含义。
			 */
			cv = getcvec(v, 0, 3);
			if (cv)
			{
				addrange(cv, '0', '9');
				addrange(cv, 'a', 'f');
				addrange(cv, 'A', 'F');
			}
			break;
		case CC_SPACE:
			cv = pg_ctype_get_cache(pg_wc_isspace, cclasscode);
			break;
		case CC_LOWER:
			cv = pg_ctype_get_cache(pg_wc_islower, cclasscode);
			break;
		case CC_UPPER:
			cv = pg_ctype_get_cache(pg_wc_isupper, cclasscode);
			break;
		case CC_GRAPH:
			cv = pg_ctype_get_cache(pg_wc_isgraph, cclasscode);
			break;
	}

	/* 若此时 cv 为 NULL，原因必是内存耗尽 */
	if (cv == NULL)
		ERR(REG_ESPACE);
	return cv;
}

/*
 * cclass_column_index - 取 chr 适用的高色图列下标
 */
static int
cclass_column_index(struct colormap *cm, chr c)
{
	int			colnum = 0;

	/* 简单 chr 不必走这套周折 */
	assert(c > MAX_SIMPLE_CHR);

	/*
	 * 注意：不应见到要求考虑未被上方
	 * cclasscvec() 当作区域设置相关的 cclass。
	 */
	if (cm->classbits[CC_PRINT] && pg_wc_isprint(c))
		colnum |= cm->classbits[CC_PRINT];
	if (cm->classbits[CC_ALNUM] && pg_wc_isalnum(c))
		colnum |= cm->classbits[CC_ALNUM];
	if (cm->classbits[CC_ALPHA] && pg_wc_isalpha(c))
		colnum |= cm->classbits[CC_ALPHA];
	if (cm->classbits[CC_WORD] && pg_wc_isword(c))
		colnum |= cm->classbits[CC_WORD];
	assert(cm->classbits[CC_ASCII] == 0);
	assert(cm->classbits[CC_BLANK] == 0);
	assert(cm->classbits[CC_CNTRL] == 0);
	if (cm->classbits[CC_DIGIT] && pg_wc_isdigit(c))
		colnum |= cm->classbits[CC_DIGIT];
	if (cm->classbits[CC_PUNCT] && pg_wc_ispunct(c))
		colnum |= cm->classbits[CC_PUNCT];
	assert(cm->classbits[CC_XDIGIT] == 0);
	if (cm->classbits[CC_SPACE] && pg_wc_isspace(c))
		colnum |= cm->classbits[CC_SPACE];
	if (cm->classbits[CC_LOWER] && pg_wc_islower(c))
		colnum |= cm->classbits[CC_LOWER];
	if (cm->classbits[CC_UPPER] && pg_wc_isupper(c))
		colnum |= cm->classbits[CC_UPPER];
	if (cm->classbits[CC_GRAPH] && pg_wc_isgraph(c))
		colnum |= cm->classbits[CC_GRAPH];

	return colnum;
}

/*
 * allcases - 为某 chr 的全部大小写对应者提供 cvec（含自身）
 *
 * 这是对简单字符的捷径，最好要高效；
 * 棘手情形走 range()。
 */
static struct cvec *
allcases(struct vars *v,		/* 语境 */
		 chr c)					/* 取大小写等价的字符 */
{
	struct cvec *cv;
	chr			lc,
				uc;

	lc = pg_wc_tolower(c);
	uc = pg_wc_toupper(c);

	cv = getcvec(v, 2, 0);
	addchr(cv, lc);
	if (lc != uc)
		addchr(cv, uc);
	return cv;
}

/*
 * cmp - chr 子串比较
 *
 * 反向引用需要它。最好要高效。
 * 注意只需报告相等/不等。
 * 另请注意长度是精确的，比较不应
 * 止于内嵌 NUL！
 */
static int						/* 0 为相等，非零为不等 */
cmp(const chr *x, const chr *y, /* 待比较字符串 */
	size_t len)					/* 比较的精确长度 */
{
	return memcmp(VS(x), VS(y), len * sizeof(chr));
}

/*
 * casecmp - 大小写无关的 chr 子串比较
 *
 * REG_ICASE 反向引用需要它。最好要高效。
 * 注意只需报告相等/不等。
 * 另请注意长度是精确的，比较不应
 * 止于内嵌 NUL！
 */
static int						/* 0 为相等，非零为不等 */
casecmp(const chr *x, const chr *y, /* 待比较字符串 */
		size_t len)				/* 比较的精确长度 */
{
	for (; len > 0; len--, x++, y++)
	{
		if ((*x != *y) && (pg_wc_tolower(*x) != pg_wc_tolower(*y)))
			return 1;
	}
	return 0;
}
