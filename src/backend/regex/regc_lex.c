/*
 * 词法分析器
 * 本文件被 regcomp.c #include。
 *
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
 * src/backend/regex/regc_lex.c
 *
 */

/* 扫描宏（了解 v） */
#define ATEOS()		(v->now >= v->stop)
#define HAVE(n)		(v->stop - v->now >= (n))
#define NEXT1(c)	(!ATEOS() && *v->now == CHR(c))
#define NEXT2(a,b)	(HAVE(2) && *v->now == CHR(a) && *(v->now+1) == CHR(b))
#define NEXT3(a,b,c)	(HAVE(3) && *v->now == CHR(a) && \
						*(v->now+1) == CHR(b) && \
						*(v->now+2) == CHR(c))
#define SET(c)		(v->nexttype = (c))
#define SETV(c, n)	(v->nexttype = (c), v->nextvalue = (n))
#define RET(c)		return (SET(c), 1)
#define RETV(c, n)	return (SETV(c, n), 1)
#define FAILW(e)	return (ERR(e), 0)	/* ERR 会 SET(EOS) */
#define LASTTYPE(t) (v->lasttype == (t))

/* 词法上下文 */
#define L_ERE	1				/* 主线 ERE/ARE */
#define L_BRE	2				/* 主线 BRE */
#define L_Q 3					/* REG_QUOTE */
#define L_EBND	4				/* ERE/ARE 界 */
#define L_BBND	5				/* BRE 界 */
#define L_BRACK 6				/* 括号 */
#define L_CEL	7				/* 对照元素 */
#define L_ECL	8				/* 等价类 */
#define L_CCL	9				/* 字符类 */
#define INTOCON(c)	(v->lexcon = (c))
#define INCON(con)	(v->lexcon == (con))

/* 构造越过 chr 数组末尾的指针 */
#define ENDOF(array)	((array) + sizeof(array)/sizeof(chr))

/*
 * lexstart - 设置词法相关，扫描前导选项
 */
static void
lexstart(struct vars *v)
{
	prefixes(v);				/* 或会打开新类型位等。 */
	NOERR();

	if (v->cflags & REG_QUOTE)
	{
		assert(!(v->cflags & (REG_ADVANCED | REG_EXPANDED | REG_NEWLINE)));
		INTOCON(L_Q);
	}
	else if (v->cflags & REG_EXTENDED)
	{
		assert(!(v->cflags & REG_QUOTE));
		INTOCON(L_ERE);
	}
	else
	{
		assert(!(v->cflags & (REG_QUOTE | REG_ADVF)));
		INTOCON(L_BRE);
	}

	v->nexttype = EMPTY;		/* 记住我们曾在起始处 */
	next(v);					/* 设置第一个记号 */
}

/*
 * prefixes - 实现各种特殊前缀
 */
static void
prefixes(struct vars *v)
{
	/* 字面串得不到这些东西 */
	if (v->cflags & REG_QUOTE)
		return;

	/* 起始 "***" 有特殊处理 */
	if (HAVE(4) && NEXT3('*', '*', '*'))
		switch (*(v->now + 3))
		{
			case CHR('?'):		/* ***? 错误，消息含版本 */
				ERR(REG_BADPAT);
				return;			/* 不再继续 */
				break;
			case CHR('='):		/* ***= 切到字面串 */
				NOTE(REG_UNONPOSIX);
				v->cflags |= REG_QUOTE;
				v->cflags &= ~(REG_ADVANCED | REG_EXPANDED | REG_NEWLINE);
				v->now += 4;
				return;			/* 且其后不能再有前缀 */
				break;
			case CHR(':'):		/* ***: 切到 ARE */
				NOTE(REG_UNONPOSIX);
				v->cflags |= REG_ADVANCED;
				v->now += 4;
				break;
			default:			/* 否则 *** 只是错误 */
				ERR(REG_BADRPT);
				return;
				break;
		}

	/* BRE 与 ERE 无嵌入选项 */
	if ((v->cflags & REG_ADVANCED) != REG_ADVANCED)
		return;

	/* 嵌入选项（仅 ARE） */
	if (HAVE(3) && NEXT2('(', '?') && iscalpha(*(v->now + 2)))
	{
		NOTE(REG_UNONPOSIX);
		v->now += 2;
		for (; !ATEOS() && iscalpha(*v->now); v->now++)
			switch (*v->now)
			{
				case CHR('b'):	/* BREs (but why???) */
					v->cflags &= ~(REG_ADVANCED | REG_QUOTE);
					break;
				case CHR('c'):	/* 辨大小写 */
					v->cflags &= ~REG_ICASE;
					break;
				case CHR('e'):	/* 普通 ERE */
					v->cflags |= REG_EXTENDED;
					v->cflags &= ~(REG_ADVF | REG_QUOTE);
					break;
				case CHR('i'):	/* 不辨大小写 */
					v->cflags |= REG_ICASE;
					break;
				case CHR('m'):	/* 类 Perl 的 n 同义 */
				case CHR('n'):	/* \n 影响 ^ $ . [^ */
					v->cflags |= REG_NEWLINE;
					break;
				case CHR('p'):	/* ~Perl，\n 影响 . [^ */
					v->cflags |= REG_NLSTOP;
					v->cflags &= ~REG_NLANCH;
					break;
				case CHR('q'):	/* 字面串 */
					v->cflags |= REG_QUOTE;
					v->cflags &= ~REG_ADVANCED;
					break;
				case CHR('s'):	/* 单行，\n 普通 */
					v->cflags &= ~REG_NEWLINE;
					break;
				case CHR('t'):	/* 紧语法 */
					v->cflags &= ~REG_EXPANDED;
					break;
				case CHR('w'):	/* 怪，\n 只影响 ^ $ */
					v->cflags &= ~REG_NLSTOP;
					v->cflags |= REG_NLANCH;
					break;
				case CHR('x'):	/* 展开语法 */
					v->cflags |= REG_EXPANDED;
					break;
				default:
					ERR(REG_BADOPT);
					return;
			}
		if (!NEXT1(')'))
		{
			ERR(REG_BADOPT);
			return;
		}
		v->now++;
		if (v->cflags & REG_QUOTE)
			v->cflags &= ~(REG_EXPANDED | REG_NEWLINE);
	}
}

/*
 * next - 取下记号
 */
static int						/* 1 正常，0 失败 */
next(struct vars *v)
{
	chr			c;

next_restart:					/* 吃掉注释后在此循环 */

	/* 出错则产生无穷失败序列 */
	if (ISERR())
		return 0;				/* 错误已将 nexttype 设为 EOS */

	/* 记住上一记号的味道 */
	v->lasttype = v->nexttype;

	/* REG_BOSONLY */
	if (v->nexttype == EMPTY && (v->cflags & REG_BOSONLY))
	{
		/* 在 REG_BOSONLY RE 起始 */
		RETV(SBEGIN, 0);		/* 同 \A */
	}

	/* 适当时跳过空白等（不在字面或 [] 中） */
	if (v->cflags & REG_EXPANDED)
		switch (v->lexcon)
		{
			case L_ERE:
			case L_BRE:
			case L_EBND:
			case L_BBND:
				skip(v);
				break;
		}

	/* 按语境处理 EOS */
	if (ATEOS())
	{
		switch (v->lexcon)
		{
			case L_ERE:
			case L_BRE:
			case L_Q:
				RET(EOS);
				break;
			case L_EBND:
			case L_BBND:
				FAILW(REG_EBRACE);
				break;
			case L_BRACK:
			case L_CEL:
			case L_ECL:
			case L_CCL:
				FAILW(REG_EBRACK);
				break;
		}
		assert(NOTREACHED);
	}

	/* 好，该真正取一个字符了 */
	c = *v->now++;

	/* 处理易语境，ERE 丢给下方代码 */
	switch (v->lexcon)
	{
		case L_BRE:				/* BRE 丢给独立函数 */
			return brenext(v, c);
			break;
		case L_ERE:				/* 见下 */
			break;
		case L_Q:				/* 字面串很容易 */
			RETV(PLAIN, c);
			break;
		case L_BBND:			/* 界相当简单 */
		case L_EBND:
			switch (c)
			{
				case CHR('0'):
				case CHR('1'):
				case CHR('2'):
				case CHR('3'):
				case CHR('4'):
				case CHR('5'):
				case CHR('6'):
				case CHR('7'):
				case CHR('8'):
				case CHR('9'):
					RETV(DIGIT, (chr) DIGITVAL(c));
					break;
				case CHR(','):
					RET(',');
					break;
				case CHR('}'):	/* ERE 界以 } 结束 */
					if (INCON(L_EBND))
					{
						INTOCON(L_ERE);
						if ((v->cflags & REG_ADVF) && NEXT1('?'))
						{
							v->now++;
							NOTE(REG_UNONPOSIX);
							RETV('}', 0);
						}
						RETV('}', 1);
					}
					else
						FAILW(REG_BADBR);
					break;
				case CHR('\\'): /* BRE 界以 \} 结束 */
					if (INCON(L_BBND) && NEXT1('}'))
					{
						v->now++;
						INTOCON(L_BRE);
						RETV('}', 1);
					}
					else
						FAILW(REG_BADBR);
					break;
				default:
					FAILW(REG_BADBR);
					break;
			}
			assert(NOTREACHED);
			break;
		case L_BRACK:			/* 括号不太难 */
			switch (c)
			{
				case CHR(']'):
					if (LASTTYPE('['))
						RETV(PLAIN, c);
					else
					{
						INTOCON((v->cflags & REG_EXTENDED) ?
								L_ERE : L_BRE);
						RET(']');
					}
					break;
				case CHR('\\'):
					NOTE(REG_UBBS);
					if (!(v->cflags & REG_ADVF))
						RETV(PLAIN, c);
					NOTE(REG_UNONPOSIX);
					if (ATEOS())
						FAILW(REG_EESCAPE);
					if (!lexescape(v))
						return 0;
					switch (v->nexttype)
					{			/* 非所有转义皆可 */
						case PLAIN:
						case CCLASSS:
						case CCLASSC:
							return 1;
							break;
					}
					/* 不是可接受的转义之一 */
					FAILW(REG_EESCAPE);
					break;
				case CHR('-'):
					if (LASTTYPE('[') || NEXT1(']'))
						RETV(PLAIN, c);
					else
						RETV(RANGE, c);
					break;
				case CHR('['):
					if (ATEOS())
						FAILW(REG_EBRACK);
					switch (*v->now++)
					{
						case CHR('.'):
							INTOCON(L_CEL);
							/* 或是或不是区域设置相关 */
							RET(COLLEL);
							break;
						case CHR('='):
							INTOCON(L_ECL);
							NOTE(REG_ULOCALE);
							RET(ECLASS);
							break;
						case CHR(':'):
							INTOCON(L_CCL);
							NOTE(REG_ULOCALE);
							RET(CCLASS);
							break;
						default:	/* 糟 */
							v->now--;
							RETV(PLAIN, c);
							break;
					}
					assert(NOTREACHED);
					break;
				default:
					RETV(PLAIN, c);
					break;
			}
			assert(NOTREACHED);
			break;
		case L_CEL:				/* 对照元素很容易 */
			if (c == CHR('.') && NEXT1(']'))
			{
				v->now++;
				INTOCON(L_BRACK);
				RETV(END, '.');
			}
			else
				RETV(PLAIN, c);
			break;
		case L_ECL:				/* 等价类同上 */
			if (c == CHR('=') && NEXT1(']'))
			{
				v->now++;
				INTOCON(L_BRACK);
				RETV(END, '=');
			}
			else
				RETV(PLAIN, c);
			break;
		case L_CCL:				/* 字符类同上 */
			if (c == CHR(':') && NEXT1(']'))
			{
				v->now++;
				INTOCON(L_BRACK);
				RETV(END, ':');
			}
			else
				RETV(PLAIN, c);
			break;
		default:
			assert(NOTREACHED);
			break;
	}

	/* 除 ERE 与 ARE 外都已处理掉 */
	assert(INCON(L_ERE));

	/* 处理 ERE 与 ARE，反斜杠除外 */
	switch (c)
	{
		case CHR('|'):
			RET('|');
			break;
		case CHR('*'):
			if ((v->cflags & REG_ADVF) && NEXT1('?'))
			{
				v->now++;
				NOTE(REG_UNONPOSIX);
				RETV('*', 0);
			}
			RETV('*', 1);
			break;
		case CHR('+'):
			if ((v->cflags & REG_ADVF) && NEXT1('?'))
			{
				v->now++;
				NOTE(REG_UNONPOSIX);
				RETV('+', 0);
			}
			RETV('+', 1);
			break;
		case CHR('?'):
			if ((v->cflags & REG_ADVF) && NEXT1('?'))
			{
				v->now++;
				NOTE(REG_UNONPOSIX);
				RETV('?', 0);
			}
			RETV('?', 1);
			break;
		case CHR('{'):			/* 界开始或普通字符 */
			if (v->cflags & REG_EXPANDED)
				skip(v);
			if (ATEOS() || !iscdigit(*v->now))
			{
				NOTE(REG_UBRACES);
				NOTE(REG_UUNSPEC);
				RETV(PLAIN, c);
			}
			else
			{
				NOTE(REG_UBOUNDS);
				INTOCON(L_EBND);
				RET('{');
			}
			assert(NOTREACHED);
			break;
		case CHR('('):			/* 括号，或高级扩展 */
			if ((v->cflags & REG_ADVF) && NEXT1('?'))
			{
				NOTE(REG_UNONPOSIX);
				v->now++;
				if (ATEOS())
					FAILW(REG_BADRPT);
				switch (*v->now++)
				{
					case CHR(':'):	/* 非捕获括号 */
						RETV('(', 0);
						break;
					case CHR('#'):	/* 注释 */
						while (!ATEOS() && *v->now != CHR(')'))
							v->now++;
						if (!ATEOS())
							v->now++;
						assert(v->nexttype == v->lasttype);
						goto next_restart;
					case CHR('='):	/* 正前瞻 */
						NOTE(REG_ULOOKAROUND);
						RETV(LACON, LATYPE_AHEAD_POS);
						break;
					case CHR('!'):	/* 负前瞻 */
						NOTE(REG_ULOOKAROUND);
						RETV(LACON, LATYPE_AHEAD_NEG);
						break;
					case CHR('<'):
						if (ATEOS())
							FAILW(REG_BADRPT);
						switch (*v->now++)
						{
							case CHR('='):	/* 正后顾 */
								NOTE(REG_ULOOKAROUND);
								RETV(LACON, LATYPE_BEHIND_POS);
								break;
							case CHR('!'):	/* 负后顾 */
								NOTE(REG_ULOOKAROUND);
								RETV(LACON, LATYPE_BEHIND_NEG);
								break;
							default:
								FAILW(REG_BADRPT);
								break;
						}
						assert(NOTREACHED);
						break;
					default:
						FAILW(REG_BADRPT);
						break;
				}
				assert(NOTREACHED);
			}
			RETV('(', 1);
			break;
		case CHR(')'):
			if (LASTTYPE('('))
				NOTE(REG_UUNSPEC);
			RETV(')', c);
			break;
		case CHR('['):			/* 易，除 [[:<:]] 与 [[:>:]] */
			if (HAVE(6) && *(v->now + 0) == CHR('[') &&
				*(v->now + 1) == CHR(':') &&
				(*(v->now + 2) == CHR('<') ||
				 *(v->now + 2) == CHR('>')) &&
				*(v->now + 3) == CHR(':') &&
				*(v->now + 4) == CHR(']') &&
				*(v->now + 5) == CHR(']'))
			{
				c = *(v->now + 2);
				v->now += 6;
				NOTE(REG_UNONPOSIX);
				RET((c == CHR('<')) ? '<' : '>');
			}
			INTOCON(L_BRACK);
			if (NEXT1('^'))
			{
				v->now++;
				RETV('[', 0);
			}
			RETV('[', 1);
			break;
		case CHR('.'):
			RET('.');
			break;
		case CHR('^'):
			RET('^');
			break;
		case CHR('$'):
			RET('$');
			break;
		case CHR('\\'):			/* 反斜杠多半丢给下方代码 */
			if (ATEOS())
				FAILW(REG_EESCAPE);
			break;
		default:				/* 普通字符 */
			RETV(PLAIN, c);
			break;
	}

	/* ERE/ARE 反斜杠处理；反斜杠已吃掉 */
	assert(!ATEOS());
	if (!(v->cflags & REG_ADVF))
	{							/* 仅 ARE 有非平凡转义 */
		if (iscalnum(*v->now))
		{
			NOTE(REG_UBSALNUM);
			NOTE(REG_UUNSPEC);
		}
		RETV(PLAIN, *v->now++);
	}
	return lexescape(v);
}

/*
 * lexescape - 解析 ARE 反斜杠转义（反斜杠已吃掉）
 *
 * 用于 ARE 反斜杠：常态下与括号表达式内皆然。
 * 后者并非所有转义类型皆允许，
 * 但调用方须在我们返回后拒绝不想要的。
 */
static int
lexescape(struct vars *v)
{
	chr			c;
	static const chr alert[] = {
		CHR('a'), CHR('l'), CHR('e'), CHR('r'), CHR('t')
	};
	static const chr esc[] = {
		CHR('E'), CHR('S'), CHR('C')
	};
	const chr  *save;

	assert(v->cflags & REG_ADVF);

	assert(!ATEOS());
	c = *v->now++;

	/* 若非字母数字 ASCII，则当作普通字符 */
	if (!('a' <= c && c <= 'z') &&
		!('A' <= c && c <= 'Z') &&
		!('0' <= c && c <= '9'))
		RETV(PLAIN, c);

	NOTE(REG_UNONPOSIX);
	switch (c)
	{
		case CHR('a'):
			RETV(PLAIN, chrnamed(v, alert, ENDOF(alert), CHR('\007')));
			break;
		case CHR('A'):
			RETV(SBEGIN, 0);
			break;
		case CHR('b'):
			RETV(PLAIN, CHR('\b'));
			break;
		case CHR('B'):
			RETV(PLAIN, CHR('\\'));
			break;
		case CHR('c'):
			NOTE(REG_UUNPORT);
			if (ATEOS())
				FAILW(REG_EESCAPE);
			RETV(PLAIN, (chr) (*v->now++ & 037));
			break;
		case CHR('d'):
			NOTE(REG_ULOCALE);
			RETV(CCLASSS, CC_DIGIT);
			break;
		case CHR('D'):
			NOTE(REG_ULOCALE);
			RETV(CCLASSC, CC_DIGIT);
			break;
		case CHR('e'):
			NOTE(REG_UUNPORT);
			RETV(PLAIN, chrnamed(v, esc, ENDOF(esc), CHR('\033')));
			break;
		case CHR('f'):
			RETV(PLAIN, CHR('\f'));
			break;
		case CHR('m'):
			RET('<');
			break;
		case CHR('M'):
			RET('>');
			break;
		case CHR('n'):
			RETV(PLAIN, CHR('\n'));
			break;
		case CHR('r'):
			RETV(PLAIN, CHR('\r'));
			break;
		case CHR('s'):
			NOTE(REG_ULOCALE);
			RETV(CCLASSS, CC_SPACE);
			break;
		case CHR('S'):
			NOTE(REG_ULOCALE);
			RETV(CCLASSC, CC_SPACE);
			break;
		case CHR('t'):
			RETV(PLAIN, CHR('\t'));
			break;
		case CHR('u'):
			c = lexdigits(v, 16, 4, 4);
			if (ISERR() || !CHR_IS_IN_RANGE(c))
				FAILW(REG_EESCAPE);
			RETV(PLAIN, c);
			break;
		case CHR('U'):
			c = lexdigits(v, 16, 8, 8);
			if (ISERR() || !CHR_IS_IN_RANGE(c))
				FAILW(REG_EESCAPE);
			RETV(PLAIN, c);
			break;
		case CHR('v'):
			RETV(PLAIN, CHR('\v'));
			break;
		case CHR('w'):
			NOTE(REG_ULOCALE);
			RETV(CCLASSS, CC_WORD);
			break;
		case CHR('W'):
			NOTE(REG_ULOCALE);
			RETV(CCLASSC, CC_WORD);
			break;
		case CHR('x'):
			NOTE(REG_UUNPORT);
			c = lexdigits(v, 16, 1, 255);	/* RE 长>255 超出规范 */
			if (ISERR() || !CHR_IS_IN_RANGE(c))
				FAILW(REG_EESCAPE);
			RETV(PLAIN, c);
			break;
		case CHR('y'):
			NOTE(REG_ULOCALE);
			RETV(WBDRY, 0);
			break;
		case CHR('Y'):
			NOTE(REG_ULOCALE);
			RETV(NWBDRY, 0);
			break;
		case CHR('Z'):
			RETV(SEND, 0);
			break;
		case CHR('1'):
		case CHR('2'):
		case CHR('3'):
		case CHR('4'):
		case CHR('5'):
		case CHR('6'):
		case CHR('7'):
		case CHR('8'):
		case CHR('9'):
			save = v->now;
			v->now--;			/* 放回首位数字 */
			c = lexdigits(v, 10, 1, 255);	/* RE 长>255 超出规范 */
			if (ISERR())
				FAILW(REG_EESCAPE);
			/* 丑陋启发（首测是"恰 1 位?"） */
			if (v->now == save || ((int) c > 0 && (int) c <= v->nsubexp))
			{
				NOTE(REG_UBACKREF);
				RETV(BACKREF, c);
			}
			/* 糟，毕竟不像是反向引用... */
			v->now = save;
			/* 并落入八进制数 */
			/* FALLTHROUGH */
		case CHR('0'):
			NOTE(REG_UUNPORT);
			v->now--;			/* 放回首位数字 */
			c = lexdigits(v, 8, 1, 3);
			if (ISERR())
				FAILW(REG_EESCAPE);
			if (c > 0xff)
			{
				/* 越界，故多处理了一位数字 */
				v->now--;
				c >>= 3;
			}
			RETV(PLAIN, c);
			break;
		default:

			/*
			 * 对未识别的 ASCII 字母转义序列报错，
			 * 以便需要时为其保留将来用途。
			 */
			FAILW(REG_EESCAPE);
			break;
	}
	assert(NOTREACHED);
}

/*
 * lexdigits - 吞入数字并返回 chr 值
 *
 * 不计溢出；若 maxlen 大到可能溢出，调用方应
 * 对结果做范围检查。
 */
static chr						/* chr 值；错误经 ERR 通报 */
lexdigits(struct vars *v,
		  int base,
		  int minlen,
		  int maxlen)
{
	uchr		n;				/* 无符号，以免溢出行为失常 */
	int			len;
	chr			c;
	int			d;
	const uchr	ub = (uchr) base;

	n = 0;
	for (len = 0; len < maxlen && !ATEOS(); len++)
	{
		c = *v->now++;
		switch (c)
		{
			case CHR('0'):
			case CHR('1'):
			case CHR('2'):
			case CHR('3'):
			case CHR('4'):
			case CHR('5'):
			case CHR('6'):
			case CHR('7'):
			case CHR('8'):
			case CHR('9'):
				d = DIGITVAL(c);
				break;
			case CHR('a'):
			case CHR('A'):
				d = 10;
				break;
			case CHR('b'):
			case CHR('B'):
				d = 11;
				break;
			case CHR('c'):
			case CHR('C'):
				d = 12;
				break;
			case CHR('d'):
			case CHR('D'):
				d = 13;
				break;
			case CHR('e'):
			case CHR('E'):
				d = 14;
				break;
			case CHR('f'):
			case CHR('F'):
				d = 15;
				break;
			default:
				v->now--;		/* 糟，根本不是数字 */
				d = -1;
				break;
		}

		if (d >= base)
		{						/* 不是像样的数字 */
			v->now--;
			d = -1;
		}
		if (d < 0)
			break;				/* 注意跳出 */
		n = n * ub + (uchr) d;
	}
	if (len < minlen)
		ERR(REG_EESCAPE);

	return (chr) n;
}

/*
 * brenext - 取下 BRE 记号
 *
 * 很像 ERE，只是反斜杠愚蠢且有些事有
 * 语境依赖性。
 */
static int						/* 1 正常，0 失败 */
brenext(struct vars *v,
		chr c)
{
	switch (c)
	{
		case CHR('*'):
			if (LASTTYPE(EMPTY) || LASTTYPE('(') || LASTTYPE('^'))
				RETV(PLAIN, c);
			RETV('*', 1);
			break;
		case CHR('['):
			if (HAVE(6) && *(v->now + 0) == CHR('[') &&
				*(v->now + 1) == CHR(':') &&
				(*(v->now + 2) == CHR('<') ||
				 *(v->now + 2) == CHR('>')) &&
				*(v->now + 3) == CHR(':') &&
				*(v->now + 4) == CHR(']') &&
				*(v->now + 5) == CHR(']'))
			{
				c = *(v->now + 2);
				v->now += 6;
				NOTE(REG_UNONPOSIX);
				RET((c == CHR('<')) ? '<' : '>');
			}
			INTOCON(L_BRACK);
			if (NEXT1('^'))
			{
				v->now++;
				RETV('[', 0);
			}
			RETV('[', 1);
			break;
		case CHR('.'):
			RET('.');
			break;
		case CHR('^'):
			if (LASTTYPE(EMPTY))
				RET('^');
			if (LASTTYPE('('))
			{
				NOTE(REG_UUNSPEC);
				RET('^');
			}
			RETV(PLAIN, c);
			break;
		case CHR('$'):
			if (v->cflags & REG_EXPANDED)
				skip(v);
			if (ATEOS())
				RET('$');
			if (NEXT2('\\', ')'))
			{
				NOTE(REG_UUNSPEC);
				RET('$');
			}
			RETV(PLAIN, c);
			break;
		case CHR('\\'):
			break;				/* 见下 */
		default:
			RETV(PLAIN, c);
			break;
	}

	assert(c == CHR('\\'));

	if (ATEOS())
		FAILW(REG_EESCAPE);

	c = *v->now++;
	switch (c)
	{
		case CHR('{'):
			INTOCON(L_BBND);
			NOTE(REG_UBOUNDS);
			RET('{');
			break;
		case CHR('('):
			RETV('(', 1);
			break;
		case CHR(')'):
			RETV(')', c);
			break;
		case CHR('<'):
			NOTE(REG_UNONPOSIX);
			RET('<');
			break;
		case CHR('>'):
			NOTE(REG_UNONPOSIX);
			RET('>');
			break;
		case CHR('1'):
		case CHR('2'):
		case CHR('3'):
		case CHR('4'):
		case CHR('5'):
		case CHR('6'):
		case CHR('7'):
		case CHR('8'):
		case CHR('9'):
			NOTE(REG_UBACKREF);
			RETV(BACKREF, (chr) DIGITVAL(c));
			break;
		default:
			if (iscalnum(c))
			{
				NOTE(REG_UBSALNUM);
				NOTE(REG_UUNSPEC);
			}
			RETV(PLAIN, c);
			break;
	}

	assert(NOTREACHED);
	return 0;
}

/*
 * skip - 在展开形式中跳过空白与注释
 */
static void
skip(struct vars *v)
{
	const chr  *start = v->now;

	assert(v->cflags & REG_EXPANDED);

	for (;;)
	{
		while (!ATEOS() && iscspace(*v->now))
			v->now++;
		if (ATEOS() || *v->now != CHR('#'))
			break;				/* 注意跳出 */
		assert(NEXT1('#'));
		while (!ATEOS() && *v->now != CHR('\n'))
			v->now++;
		/* 把换行留给 iscspace 循环拾取 */
	}

	if (v->now != start)
		NOTE(REG_UNONPOSIX);
}

/*
 * newline - 返回换行的 chr
 *
 * 有助把 CHR 的使用限制在本源文件。
 */
static chr
newline(void)
{
	return CHR('\n');
}

/*
 * chrnamed - 返回已知（chr 串）名对应的 chr
 *
 * 代码略笨拙，但本例程用途如此专门，
 * 几乎无关紧要。
 */
static chr
chrnamed(struct vars *v,
		 const chr *startp,		/* 名之始 */
		 const chr *endp,		/* 刚过名之末 */
		 chr lastresort)		/* 名查找失败时返回何值 */
{
	chr			c;
	int			errsave;
	int			e;
	struct cvec *cv;

	errsave = v->err;
	v->err = 0;
	c = element(v, startp, endp);
	e = v->err;
	v->err = errsave;

	if (e != 0)
		return lastresort;

	cv = range(v, c, c, 0);
	if (cv->nchrs == 0)
		return lastresort;
	return cv->chrs[0];
}
