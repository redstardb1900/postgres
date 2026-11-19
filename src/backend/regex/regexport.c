/*-------------------------------------------------------------------------
 *
 * regexport.c
 *	  导出正则之 NFA 信息的函数
 *
 * 本实现中，NFA 定义串匹配该正则的必要但非充分
 * 条件：即可有串匹配 NFA 却不匹配完整正则，
 * 但反之不然。
 * 因此例如，下列函数把环视约束
 * 当作空操作即可，因其只是进一步约束该串。
 *
 * 注意这些函数把信息写入调用方提供的数组
 * 而非自行 malloc。这简化了 API，因
 * 消除了一类错误条件，且就色而言
 * 允许调用方自行决定多大才不值得理会。
 *
 *
 * Portions Copyright (c) 2013-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1998, 1999 Henry Spencer
 *
 * IDENTIFICATION
 *	  src/backend/regex/regexport.c
 *
 *-------------------------------------------------------------------------
 */

#include "regex/regguts.h"

#include "regex/regexport.h"


/*
 * 取 NFA 状态总数。
 */
int
pg_reg_getnumstates(const regex_t *regex)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	return cnfa->nstates;
}

/*
 * 取 NFA 初始状态。
 */
int
pg_reg_getinitialstate(const regex_t *regex)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	return cnfa->pre;
}

/*
 * 取 NFA 终结状态。
 */
int
pg_reg_getfinalstate(const regex_t *regex)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	return cnfa->post;
}

/*
 * pg_reg_getnumoutarcs() 与 pg_reg_getoutarcs() 对调用方掩盖 LACON
 * 弧之存在，将任一 LACON 视为已自动满足。
 * 因输出表示不支持遍历时不消费
 * 字符的弧，故须在此递归遍历 LACON 弧，
 * 并报告经 LACON 弧可达的一切普通弧。
 * 注意：若可经 LACON 遍历到达终结状态则此法
 * 行不通，但正则库从不构建带有
 * 直通终结状态的 LACON 弧的 NFA。（因为
 * 正则执行器设计为在名义匹配末后再消费
 * 一个字符——可能是 EOS 指示——故总有一组
 * 通向终结状态的普通弧。）
 *
 * traverse_lacons 是两导出函数共用的递归子程序，
 * 用以计数并写出可达的普通弧。*arcs_count 按
 * 可达弧数递增，并在 arcs_len 可容纳范围内
 * （可为 0）把弧写入 arcs[]。
 */
static void
traverse_lacons(struct cnfa *cnfa, int st,
				int *arcs_count,
				regex_arc_t *arcs, int arcs_len)
{
	struct carc *ca;

	/*
	 * 因本函数递归，理论上可被逼至栈
	 * 溢出。实践中，这主要用作防止
	 * 正则编译器未消除 LACON 弧环的后盾。
	 */
	check_stack_depth();

	for (ca = cnfa->states[st]; ca->co != COLORLESS; ca++)
	{
		if (ca->co < cnfa->ncolors)
		{
			/* 普通弧，故计数并可能写出 */
			int			ndx = (*arcs_count)++;

			if (ndx < arcs_len)
			{
				arcs[ndx].co = ca->co;
				arcs[ndx].to = ca->to;
			}
		}
		else
		{
			/* LACON 弧——假定已满足并递归…… */
			/* 但先断言其不直接通向 post 状态 */
			Assert(ca->to != cnfa->post);

			traverse_lacons(cnfa, ca->to, arcs_count, arcs, arcs_len);
		}
	}
}

/*
 * 取状态号 "st" 的出 NFA 弧数。
 */
int
pg_reg_getnumoutarcs(const regex_t *regex, int st)
{
	struct cnfa *cnfa;
	int			arcs_count;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (st < 0 || st >= cnfa->nstates)
		return 0;
	arcs_count = 0;
	traverse_lacons(cnfa, st, &arcs_count, NULL, 0);
	return arcs_count;
}

/*
 * 把状态号 "st" 的出 NFA 弧数组写入 arcs[]，
 * 其长度 arcs_len 须至少达 pg_reg_getnumoutarcs()
 * 所示，否则并非全部弧都会返回。
 */
void
pg_reg_getoutarcs(const regex_t *regex, int st,
				  regex_arc_t *arcs, int arcs_len)
{
	struct cnfa *cnfa;
	int			arcs_count;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (st < 0 || st >= cnfa->nstates || arcs_len <= 0)
		return;
	arcs_count = 0;
	traverse_lacons(cnfa, st, &arcs_count, arcs, arcs_len);
}

/*
 * 取色的总数。
 */
int
pg_reg_getnumcolors(const regex_t *regex)
{
	struct colormap *cm;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	return cm->max + 1;
}

/*
 * 检查色是否为行/串之始。
 *
 * （日后或需对伪色提供更精细的处理，
 * 但眼下够用。）
 */
int
pg_reg_colorisbegin(const regex_t *regex, int co)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (co == cnfa->bos[0] || co == cnfa->bos[1])
		return true;
	else
		return false;
}

/*
 * 检查色是否为行/串之末。
 */
int
pg_reg_colorisend(const regex_t *regex, int co)
{
	struct cnfa *cnfa;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cnfa = &((struct guts *) regex->re_guts)->search;

	if (co == cnfa->eos[0] || co == cnfa->eos[1])
		return true;
	else
		return false;
}

/*
 * 取色号 "co" 的成员 chr 数。
 *
 * 注意：返回 -1，若色号无效，或它是特殊
 * 色（WHITE、彩虹 或伪色），或成员数
 * 未定。
 * 若返回 -1，调用方不应试图提取成员。
 */
int
pg_reg_getnumcharacters(const regex_t *regex, int co)
{
	struct colormap *cm;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	if (co <= 0 || co > cm->max)	/* <= 0 拒绝 WHITE 与 彩虹 */
		return -1;
	if (cm->cd[co].flags & PSEUDO)	/* 亦拒伪色（BOS 等） */
		return -1;

	/*
	 * 若该色出现在高色图任何处，则视其成员
	 * 数为不定。原则上可确定每项对应的全部
	 * 具体 chr，但代价高昂
	 * （尤其若需字符类测试），且
	 * 不值得。
	 */
	if (cm->cd[co].nuchrs != 0)
		return -1;

	/* 好，返回已知的成员 chr 数 */
	return cm->cd[co].nschrs;
}

/*
 * 把色号 "co" 的成员 chr 数组写入 chars[]，
 * 其长度 chars_len 须至少达 pg_reg_getnumcharacters()
 * 所示，否则并非全部字符都会返回。
 *
 * 不支持提取 WHITE、彩虹 或伪色的成员。
 *
 * 注意：这是相对昂贵的操作。
 */
void
pg_reg_getcharacters(const regex_t *regex, int co,
					 pg_wchar *chars, int chars_len)
{
	struct colormap *cm;
	chr			c;

	assert(regex != NULL && regex->re_magic == REMAGIC);
	cm = &((struct guts *) regex->re_guts)->cmap;

	if (co <= 0 || co > cm->max || chars_len <= 0)
		return;
	if (cm->cd[co].flags & PSEUDO)
		return;

	/*
	 * 只需检查低字符映射；高映射中不应有
	 * 匹配的项。
	 */
	for (c = CHR_MIN; c <= MAX_SIMPLE_CHR; c++)
	{
		if (cm->locolormap[c - CHR_MIN] == co)
		{
			*chars++ = c;
			if (--chars_len == 0)
				break;
		}
	}
}
