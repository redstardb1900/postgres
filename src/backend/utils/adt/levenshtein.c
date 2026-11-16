/*-------------------------------------------------------------------------
 *
 * levenshtein.c
 *	  Levenshtein 编辑距离实现。
 *
 * Original author:  Joe Conway <mail@joeconway.com>
 *
 * 本文件被 varlena.c 包含两次，以提供匹配代码：(1)
 * 带自定义代价的 Levenshtein 距离，以及 (2) 带自定义
 * 且有 "max" 值的 Levenshtein 距离，超过该值时精确距离
 * 就不再有意义。包含之前，我们依赖内联函数
 * rest_of_char_same() 存在。
 *
 * 本实现基于 Michael Gilleland 所述算法描述写成，
 * 其描述见 http://www.merriampark.com/ld.htm。还参考了
 * PHP 4.0.6 发行版中的 levenshtein.c 以求启发。可配置
 * 代价扩展由 Volkan YAZICI <volkan.yazici@gmail.com. 引入。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	src/backend/utils/adt/levenshtein.c
 *
 *-------------------------------------------------------------------------
 */
#define MAX_LEVENSHTEIN_STRLEN		255

/*
 * 计算给定两个串之间的 Levenshtein 距离度量，两串不必
 * 以空字符结尾。
 *
 * source：源串，长 slen 字节。
 * target：目标串，长 tlen 字节。
 * ins_c, del_c, sub_c：插入、删除、替换各自收取的代价；
 *		其中 (1, 1, 1) 的代价足以应对常见情形，
 *		但具体效果因人而异。
 * max_d：若提供且 >= 0，即所关心的最大距离；见下文。
 * trusted：调用方可信，无需遵守 MAX_LEVENSHTEIN_STRLEN。
 *
 * 计算 Levenshtein 距离的一种方法是增量式地构建
 * (m+1)x(n+1) 矩阵，其中单元格 (i, j) 表示将 s 的前
 * i 个字符变换成 t 的前 j 个字符的最少操作数。
 * 最终一行的最后一列即为答
 * 案。
 *
 * 我们在此使用该算法并稍作修改。不把整个
 * 数组一次全部放进内存，我们只用两个大小
 * 为 m+1 的数组来存储累计值。每一步中一个
 * 代表大数组的 "previous" 行，另一个代表 "current"
 * 行。
 *
 * 若给定了 max_d 且 >= 0，则只有在该答案小于等于
 * max_d 时，我们才需要给出准确的结果。从矩阵中的
 * 任一单元格出发，到最终一行的最后一列都存在一
 * 个理论上的 "最小残余距离"。当两串未被变换的部
 * 分等长时，该最小残余距离为零（因为我们可能运气
 * 好，发现所有剩余字符全部匹配），否则它基于使两
 * 串变为等长所需的最少插入或删除次数。当我们朝
 * 矩阵的右上角或左下角移动时，残余距离会随之增
 * 大。当 max_d 界限足够紧时，我们可利用这一性质
 * 来避免计算每一行的全部内容；作为替代，我们维护
 * start_column 与 stop_column 两个变量，用以标识矩
 * 阵中靠近对角线、且仍能影响最终答案的
 * 那一部分。
 */
int
#ifdef LEVENSHTEIN_LESS_EQUAL
varstr_levenshtein_less_equal(const char *source, int slen,
							  const char *target, int tlen,
							  int ins_c, int del_c, int sub_c,
							  int max_d, bool trusted)
#else
varstr_levenshtein(const char *source, int slen,
				   const char *target, int tlen,
				   int ins_c, int del_c, int sub_c,
				   bool trusted)
#endif
{
	int			m,
				n;
	int		   *prev;
	int		   *curr;
	int		   *s_char_len = NULL;
	int			j;
	const char *y;

	/*
	 * 对于 varstr_levenshtein_less_equal，我们有着名为
	 * start_column 与 stop_column 的真实变量；否则它只是 0 和
	 * m.
	 */
#ifdef LEVENSHTEIN_LESS_EQUAL
	int			start_column,
				stop_column;

#undef START_COLUMN
#undef STOP_COLUMN
#define START_COLUMN start_column
#define STOP_COLUMN stop_column
#else
#undef START_COLUMN
#undef STOP_COLUMN
#define START_COLUMN 0
#define STOP_COLUMN m
#endif

	/* 将串长（字节数）转换为字符数 */
	m = pg_mbstrlen_with_len(source, slen);
	n = pg_mbstrlen_with_len(target, tlen);

	/*
	 * 我们可用 n 次插入将空串 s 变换为 t，或用 m 次
	 * 删除将非空 t 变为空 s。
	 */
	if (!m)
		return n * ins_c;
	if (!n)
		return m * del_c;

	/*
	 * 为安全起见，限制过度的 CPU+RAM 消耗。（本
	 * 实现使用 O(m) 内存、O(mn) 复杂度。）
	 * 若 "trusted" 为 true，调用方应自行负责避免过度
	 * 的请求，通常做法是配合有界的串使用较小的 max_d，
	 * 其界限虽不必恰好等于 MAX_LEVENSHTEIN_STRLEN。
	 */
	if (!trusted &&
		(m > MAX_LEVENSHTEIN_STRLEN ||
		 n > MAX_LEVENSHTEIN_STRLEN))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("levenshtein argument exceeds maximum length of %d characters",
						MAX_LEVENSHTEIN_STRLEN)));

#ifdef LEVENSHTEIN_LESS_EQUAL
	/* 初始化起始列与终止列。 */
	start_column = 0;
	stop_column = m + 1;

	/*
	 * 若 max_d >= 0，判断该界限是否紧得不可能。若是，
	 * 则立即返回 max_d + 1。否则判断它是否紧到足以
	 * 限制我们必须执行的计算。若是，则算出初始的
	 * 终止列。
	 */
	if (max_d >= 0)
	{
		int			min_theo_d; /* 理论最小距离。 */
		int			max_theo_d; /* 理论最大距离。 */
		int			net_inserts = n - m;

		min_theo_d = net_inserts < 0 ?
			-net_inserts * del_c : net_inserts * ins_c;
		if (min_theo_d > max_d)
			return max_d + 1;
		if (ins_c + del_c < sub_c)
			sub_c = ins_c + del_c;
		max_theo_d = min_theo_d + sub_c * Min(m, n);
		if (max_d >= max_theo_d)
			max_d = -1;
		else if (ins_c + del_c > 0)
		{
			/*
			 * 算出概念矩阵的第一行中需要填充多少
			 * 内容。若串在增长，理论最小距离已经包含
			 * 了为使两串等长所需删除的字符数的代价。
			 * 每多删除一个字符就迫使再多一次插入，因此
			 * 最好情况下的总代价增加 ins_c + del_c。若串
			 * 在缩短，理论最小代价假定无多余删除；
			 * 也就是说，起点不会比第 n - m 列更靠右。
			 * 若确实更靠右，则每右移一列，
			 * 最好情况下的总代价就会增加
			 * ins_c + del_c。
			 */
			int			slack_d = max_d - min_theo_d;
			int			best_column = net_inserts < 0 ? -net_inserts : 0;

			stop_column = best_column + (slack_d / (ins_c + del_c)) + 1;
			if (stop_column > m)
				stop_column = m + 1;
		}
	}
#endif

	/*
	 * 为避免对 s 中每个字符反复调用 pg_mblen()，我们在
	 * 开始主循环前缓存所有长度——但如果两串中的所
	 * 有字符都是单字节，则跳过这一步，并在主循环中
	 * 使用快速路径。若只有一串包含多字节字符，
	 * 我们仍会构建该数组，这样快速路径就无需处理
	 * 数组未初始化的情形。
	 */
	if (m != slen || n != tlen)
	{
		int			i;
		const char *cp = source;

		s_char_len = (int *) palloc((m + 1) * sizeof(int));
		for (i = 0; i < m; ++i)
		{
			s_char_len[i] = pg_mblen(cp);
			cp += s_char_len[i];
		}
		s_char_len[i] = 0;
	}

	/* 为初始化列与行多留一个单元格。 */
	++m;
	++n;

	/* 概念数组的前一行与当前行。 */
	prev = (int *) palloc(2 * m * sizeof(int));
	curr = prev + m;

	/*
	 * 要把 s 的前 i 个字符变换为 t 的前 0 个字符，
	 * 须执行 i 次删除。
	 */
	for (int i = START_COLUMN; i < STOP_COLUMN; i++)
		prev[i] = i * del_c;

	/* 遍历概念数组的各行 */
	for (y = target, j = 1; j < n; j++)
	{
		int		   *temp;
		const char *x = source;
		int			y_char_len = n != tlen + 1 ? pg_mblen(y) : 1;
		int			i;

#ifdef LEVENSHTEIN_LESS_EQUAL

		/*
		 * 最好情况下，各值沿对角线原样向下传递，因此
		 * 我们必须递增 stop_column，除非它已位于数组
		 * 的右端。内层循环会读取 prev[stop_column]，
		 * 因此即便它不应影响结果，也必须初始化它。
		 */
		if (stop_column < m)
		{
			prev[stop_column] = max_d + 1;
			++stop_column;
		}

		/*
		 * 主循环负责填充 curr，但 curr[0] 需要特例处理：
		 * 要把 s 的前 0 个字符变换为 t 的前 j 个字符，
		 * 必须执行 j 次插入。但若 start_column > 0，
		 * 则该特例不适用。
		 */
		if (start_column == 0)
		{
			curr[0] = j * ins_c;
			i = 1;
		}
		else
			i = start_column;
#else
		curr[0] = j * ins_c;
		i = 1;
#endif

		/*
		 * 该内层循环对性能至关重要，故我们加入一
		 * 条快速路径，处理没有多字节字符的（相当常
		 * 见）情形。快速路径可以假定：若 s_char_len 未被
		 * 初始化，则说明两串都仅包含
		 * 单字节字符。
		 */
		if (s_char_len != NULL)
		{
			for (; i < STOP_COLUMN; i++)
			{
				int			ins;
				int			del;
				int			sub;
				int			x_char_len = s_char_len[i - 1];

				/*
				 * 计算插入、删除与替换的代价。
				 *
				 * 计算替换代价时，先比较每个可能多字节
				 * 字符末字节即可排除多数不匹配。
				 * 若通过该测试，则再比较
				 * 长度与剩余
				 * 字节。
				 */
				ins = prev[i] + ins_c;
				del = curr[i - 1] + del_c;
				if (x[x_char_len - 1] == y[y_char_len - 1]
					&& x_char_len == y_char_len &&
					(x_char_len == 1 || rest_of_char_same(x, y, x_char_len)))
					sub = prev[i - 1];
				else
					sub = prev[i - 1] + sub_c;

				/* 取代价最小者。 */
				curr[i] = Min(ins, del);
				curr[i] = Min(curr[i], sub);

				/* 指向下一字符。 */
				x += x_char_len;
			}
		}
		else
		{
			for (; i < STOP_COLUMN; i++)
			{
				int			ins;
				int			del;
				int			sub;

				/* 计算插入、删除与替换的代价。 */
				ins = prev[i] + ins_c;
				del = curr[i - 1] + del_c;
				sub = prev[i - 1] + ((*x == *y) ? 0 : sub_c);

				/* 取代价最小者。 */
				curr[i] = Min(ins, del);
				curr[i] = Min(curr[i], sub);

				/* 指向下一字符。 */
				x++;
			}
		}

		/* 将当前行与前行交换。 */
		temp = curr;
		curr = prev;
		prev = temp;

		/* 指向下一字符。 */
		y += y_char_len;

#ifdef LEVENSHTEIN_LESS_EQUAL

		/*
		 * 若在没有 max_d 界限的情形下使用，这段代码会
		 * 造成显著的性能损失。其原因可能不在于
		 * max_d >= 0 测试本身昂贵，而在于需要执行这段
		 * 代码的可能性，阻碍了对整个循环进行
		 * 紧密优化。
		 */
		if (max_d >= 0)
		{
			/*
			 * "零点"是当前行中两串剩余部分等长的列。
			 * 目标串中有 n - 1 个字符，其中 j 个已被
			 * 变换。源串中有 m - 1 个字符，因此要为 zp
			 * 找出使
			 * (n-1)-j = (m-1)-zp 成立的值。
			 *
			 */
			int			zp = j - (n - m);

			/* 检查终止列能否向左滑动。 */
			while (stop_column > 0)
			{
				int			ii = stop_column - 1;
				int			net_inserts = ii - zp;

				if (prev[ii] + (net_inserts > 0 ? net_inserts * ins_c :
								-net_inserts * del_c) <= max_d)
					break;
				stop_column--;
			}

			/* 检查起始列能否向右滑动。 */
			while (start_column < stop_column)
			{
				int			net_inserts = start_column - zp;

				if (prev[start_column] +
					(net_inserts > 0 ? net_inserts * ins_c :
					 -net_inserts * del_c) <= max_d)
					break;

				/*
				 * 这些值再也不会被更新，因此必须确保其
				 * 中没有任何会干扰外层循环后续
				 * 迭代的残留内容。
				 */
				prev[start_column] = max_d + 1;
				curr[start_column] = max_d + 1;
				if (start_column != 0)
					source += (s_char_len != NULL) ? s_char_len[start_column - 1] : 1;
				start_column++;
			}

			/* 若两列交叉，我们将超出界限。 */
			if (start_column >= stop_column)
				return max_d + 1;
		}
#endif
	}

	/*
	 * 由于最终值已从前一行换到了当前行，所以
	 * 在该行中找到它。
	 */
	return prev[m - 1];
}
