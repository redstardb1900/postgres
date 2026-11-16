/*-------------------------------------------------------------------------
 *
 * like_support.c
 *	  LIKE、正则及相关算子的规划器支持函数。
 *
 * 这些例程对这样一类算子做特殊优化：它们可
 * 以配合索引扫描使用，尽管执行器的索引扫描
 * 机制并不认识它们。关键思路是：这些算子使
 * 我们能推导出近似的索引扫描条件，使得任何
 * 通过了算子条件本身的元组也必然满足更简单
 * 的索引扫描条件。这样我们就能利用索引扫描
 * 机制来避免扫描表中原本必须扫描的部分，同
 * 时把原算子作为 qpqual 条件加以应用，从而确
 * 保只交付我们想要的元组。（本质上，这是把普通
 * 索引当作有损索引用。）
 *
 * 我们做法的一个示例：
 *			textfield LIKE 'abc%def'
 * 由此可生成能用索引扫描的条件
 *			textfield >= 'abc' AND textfield < 'abd'
 * 从而能对 textfield 上的索引高效扫描。
 * （现实中，字符集与排序规则问题使得从 LIKE 到
 * 索引扫描限界的变换比想象的更难……但这
 * 是基本思路。）
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/like_support.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "utils/varlena.h"


typedef enum
{
	Pattern_Type_Like,
	Pattern_Type_Like_IC,
	Pattern_Type_Regex,
	Pattern_Type_Regex_IC,
	Pattern_Type_Prefix
} Pattern_Type;

typedef enum
{
	Pattern_Prefix_None, Pattern_Prefix_Partial, Pattern_Prefix_Exact
} Pattern_Prefix_Status;

static Node *like_regex_support(Node *rawreq, Pattern_Type ptype);
static List *match_pattern_prefix(Node *leftop,
								  Node *rightop,
								  Pattern_Type ptype,
								  Oid expr_coll,
								  Oid opfamily,
								  Oid indexcollation);
static double patternsel_common(PlannerInfo *root,
								Oid oprid,
								Oid opfuncid,
								List *args,
								int varRelid,
								Oid collation,
								Pattern_Type ptype,
								bool negate);
static Pattern_Prefix_Status pattern_fixed_prefix(Const *patt,
												  Pattern_Type ptype,
												  Oid collation,
												  Const **prefix,
												  Selectivity *rest_selec);
static Selectivity prefix_selectivity(PlannerInfo *root,
									  VariableStatData *vardata,
									  Oid eqopr, Oid ltopr, Oid geopr,
									  Oid collation,
									  Const *prefixcon);
static Selectivity like_selectivity(const char *patt, int pattlen,
									bool case_insensitive);
static Selectivity regex_selectivity(const char *patt, int pattlen,
									 bool case_insensitive,
									 int fixed_prefix_len);
static int	pattern_char_isalpha(char c, bool is_multibyte,
								 pg_locale_t locale, bool locale_is_c);
static Const *make_greater_string(const Const *str_const, FmgrInfo *ltproc,
								  Oid collation);
static Datum string_to_datum(const char *str, Oid datatype);
static Const *string_to_const(const char *str, Oid datatype);
static Const *string_to_bytea_const(const char *str, size_t str_len);


/*
 * LIKE、正则及相关算子的规划器支持函数
 */
Datum
textlike_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Like));
}

Datum
texticlike_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Like_IC));
}

Datum
textregexeq_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Regex));
}

Datum
texticregexeq_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Regex_IC));
}

Datum
text_starts_with_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);

	PG_RETURN_POINTER(like_regex_support(rawreq, Pattern_Type_Prefix));
}

/* 以上公共代码 */
static Node *
like_regex_support(Node *rawreq, Pattern_Type ptype)
{
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSelectivity))
	{
		/*
		 * 为函数调用做一次选择率估算，就像该调用是
		 * 通过相应算子发起的一样。
		 */
		SupportRequestSelectivity *req = (SupportRequestSelectivity *) rawreq;
		Selectivity s1;

		if (req->is_join)
		{
			/*
			 * 目前我们只是放弃。若 patternjoinsel 将来
			 * 改进得更好，此处应改为调用它。
			 */
			s1 = DEFAULT_MATCH_SEL;
		}
		else
		{
			/* 与算子限制选择率函数共享代码 */
			s1 = patternsel_common(req->root,
								   InvalidOid,
								   req->funcid,
								   req->args,
								   req->varRelid,
								   req->inputcollid,
								   ptype,
								   false);
		}
		req->selectivity = s1;
		ret = (Node *) req;
	}
	else if (IsA(rawreq, SupportRequestIndexCondition))
	{
		/* 尝试把算子/函数调用转换为索引条件 */
		SupportRequestIndexCondition *req = (SupportRequestIndexCondition *) rawreq;

		/*
		 * 目前我们没有模式在左的"反向"匹配算子，因此
		 * 只需考虑索引键位于左
		 * 侧
		 */
		if (req->indexarg != 0)
			return NULL;

		if (is_opclause(req->node))
		{
			OpExpr	   *clause = (OpExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_pattern_prefix((Node *) linitial(clause->args),
									 (Node *) lsecond(clause->args),
									 ptype,
									 clause->inputcollid,
									 req->opfamily,
									 req->indexcollation);
		}
		else if (is_funcclause(req->node))	/* 求稳妥 */
		{
			FuncExpr   *clause = (FuncExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_pattern_prefix((Node *) linitial(clause->args),
									 (Node *) lsecond(clause->args),
									 ptype,
									 clause->inputcollid,
									 req->opfamily,
									 req->indexcollation);
		}
	}

	return ret;
}

/*
 * match_pattern_prefix
 *	  尝试为 LIKE 或正则算子生成索引条件。
 */
static List *
match_pattern_prefix(Node *leftop,
					 Node *rightop,
					 Pattern_Type ptype,
					 Oid expr_coll,
					 Oid opfamily,
					 Oid indexcollation)
{
	List	   *result;
	Const	   *patt;
	Const	   *prefix;
	Pattern_Prefix_Status pstatus;
	Oid			ldatatype;
	Oid			rdatatype;
	Oid			eqopr;
	Oid			ltopr;
	Oid			geopr;
	Oid			preopr = InvalidOid;
	bool		collation_aware;
	Expr	   *expr;
	FmgrInfo	ltproc;
	Const	   *greaterstr;

	/*
	 * 对非常量或 NULL 的模式参数无能为力。
	 *
	 * 注意，由于我们只限于右侧为硬常量的
	 * 情形，它必然更是伪常量，无须再担心
	 * 验证这一点。
	 */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return NIL;
	patt = (Const *) rightop;

	/*
	 * 表达式排序规则为非确定性时不支持。经优化的
	 * 相等或前缀测试使用按字节比较，这与非确定
	 * 性排序规则不一致。实际的模式匹配实现
	 * 函数稍后会报错说非确定性排序规则不支持模
	 * 式匹配。（我们本可以在这里就报错，但放到
	 * 后面可以获得更精确的错误消息。）（至少
	 * 支持 Pattern_Prefix_Exact 应该是可行的，但
	 * 只要实际的模式匹配实现不支持它，就没
	 * 有必要这么做。）
	 *
	 * expr_coll 在 bytea 等不感知排序规则的类型上不会被设置
	 */
	if (expr_coll && !get_collation_isdeterministic(expr_coll))
		return NIL;

	/*
	 * 尝试从模式中提取固定前缀。
	 */
	pstatus = pattern_fixed_prefix(patt, ptype, expr_coll,
								   &prefix, NULL);

	/* 无固定前缀失败 */
	if (pstatus == Pattern_Prefix_None)
		return NIL;

	/*
	 * 根据左侧参数的类型确定要使用哪些算子。
	 * 通常它们就是该类型常规的比较算子，但若
	 * 考虑的是半旧式的"pattern" opclass，则改用
	 * "pattern" 算子。它们不感知排序规则，而总是
	 * 如我们所愿地使用 C 排序规则。所选算子还决
	 * 定了前缀常量所需的
	 * 类型
	 */
	ldatatype = exprType(leftop);
	switch (ldatatype)
	{
		case TEXTOID:
			if (opfamily == TEXT_PATTERN_BTREE_FAM_OID)
			{
				eqopr = TextEqualOperator;
				ltopr = TextPatternLessOperator;
				geopr = TextPatternGreaterEqualOperator;
				collation_aware = false;
			}
			else if (opfamily == TEXT_SPGIST_FAM_OID)
			{
				eqopr = TextEqualOperator;
				ltopr = TextPatternLessOperator;
				geopr = TextPatternGreaterEqualOperator;
				/* 该 opfamily 直接支持前缀 */
				preopr = TextPrefixOperator;
				collation_aware = false;
			}
			else
			{
				eqopr = TextEqualOperator;
				ltopr = TextLessOperator;
				geopr = TextGreaterEqualOperator;
				collation_aware = true;
			}
			rdatatype = TEXTOID;
			break;
		case NAMEOID:

			/*
			 * 注意这里需要 RHS 类型为 text，这样比较值
			 * 才不会被不当截断到 NAMEDATALEN。
			 */
			eqopr = NameEqualTextOperator;
			ltopr = NameLessTextOperator;
			geopr = NameGreaterEqualTextOperator;
			collation_aware = true;
			rdatatype = TEXTOID;
			break;
		case BPCHAROID:
			if (opfamily == BPCHAR_PATTERN_BTREE_FAM_OID)
			{
				eqopr = BpcharEqualOperator;
				ltopr = BpcharPatternLessOperator;
				geopr = BpcharPatternGreaterEqualOperator;
				collation_aware = false;
			}
			else
			{
				eqopr = BpcharEqualOperator;
				ltopr = BpcharLessOperator;
				geopr = BpcharGreaterEqualOperator;
				collation_aware = true;
			}
			rdatatype = BPCHAROID;
			break;
		case BYTEAOID:
			eqopr = ByteaEqualOperator;
			ltopr = ByteaLessOperator;
			geopr = ByteaGreaterEqualOperator;
			collation_aware = false;
			rdatatype = BYTEAOID;
			break;
		default:
			/* 除非挂接了错误的算子，否则到不了这里 */
			return NIL;
	}

	/*
	 * 必要时把前缀常量强制转换为正确类型。给定
	 * 的前缀常量是 text 或 bytea 类型，因此唯一需
	 * 要做处理的情形是 text 转换为 bpchar。这两
	 * 个类型二进制兼容，所以重新标记 Const 节点即
	 * 可。
	 */
	if (prefix->consttype != rdatatype)
	{
		Assert(prefix->consttype == TEXTOID &&
			   rdatatype == BPCHAROID);
		prefix->consttype = rdatatype;
	}

	/*
	 * 若找到精确匹配的模式，则生成"="索引条件。
	 *
	 * 此处及下文都要检查所需算子是否真的被索引
	 * opclass 支持，不支持则安静地失败。这使我
	 * 们无须关心具体的 opclass（旧式"pattern"情
	 * 形除外）；任何正确实现了这些算子的索引都
	 * 可用。
	 */
	if (pstatus == Pattern_Prefix_Exact)
	{
		if (!op_in_opfamily(eqopr, opfamily))
			return NIL;
		expr = make_opclause(eqopr, BOOLOID, false,
							 (Expr *) leftop, (Expr *) prefix,
							 InvalidOid, indexcollation);
		result = list_make1(expr);
		return result;
	}

	/*
	 * 否则我们就有一个非空的必要值前缀。有些
	 * opclass 直接支持前缀检查，否则我们将尝试
	 * 生成范围约束。
	 */
	if (OidIsValid(preopr) && op_in_opfamily(preopr, opfamily))
	{
		expr = make_opclause(preopr, BOOLOID, false,
							 (Expr *) leftop, (Expr *) prefix,
							 InvalidOid, indexcollation);
		result = list_make1(expr);
		return result;
	}

	/*
	 * 由于需要范围约束，只有索引不感知排序规则
	 * 或具有"C"排序规则时它才能可靠工作。注意，
	 * 这里看的是索引的排序规则，而非表达式的
	 * 排序规则——该测试*并不*依赖 LIKE/正则算子
	 * 的。
	 */
	if (collation_aware &&
		!lc_collate_is_c(indexcollation))
		return NIL;

	/*
	 * 我们总能说"x >= prefix"。
	 */
	if (!op_in_opfamily(geopr, opfamily))
		return NIL;
	expr = make_opclause(geopr, BOOLOID, false,
						 (Expr *) leftop, (Expr *) prefix,
						 InvalidOid, indexcollation);
	result = list_make1(expr);

	/*-------
	 * 若能构造出比前缀更大的字符串，我们就能说
	 * "x < greaterstr"。注意：我们依赖 make_greater_string()
	 * 生成保证更大的字符串，而不只是可能更大的
	 * 字符串。一般只有 C locale 才有保证，故最好使用
	 * C locale 索引排序规则。
	 *-------
	 */
	if (!op_in_opfamily(ltopr, opfamily))
		return result;
	fmgr_info(get_opcode(ltopr), &ltproc);
	greaterstr = make_greater_string(prefix, &ltproc, indexcollation);
	if (greaterstr)
	{
		expr = make_opclause(ltopr, BOOLOID, false,
							 (Expr *) leftop, (Expr *) greaterstr,
							 InvalidOid, indexcollation);
		result = lappend(result, expr);
	}

	return result;
}


/*
 * patternsel_common —— 模式匹配限制选择率的通用代码。
 *
 * 为支持从算子或函数两条路径调用，调用者可以
 * 传入算子 OID 或底层函数 OID；必要时我们由前
 * 者查出后者。（本可以让 patternsel() 直接调用
 * get_opcode()，但若无须比较固定前缀与
 * pg_statistic 数据，就白费了。）
 *
 * 注意 oprid 和/或 opfuncid 应为正向匹配算子，
 * negate 为真时亦然。
 */
static double
patternsel_common(PlannerInfo *root,
				  Oid oprid,
				  Oid opfuncid,
				  List *args,
				  int varRelid,
				  Oid collation,
				  Pattern_Type ptype,
				  bool negate)
{
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	Oid			vartype;
	Oid			rdatatype;
	Oid			eqopr;
	Oid			ltopr;
	Oid			geopr;
	Pattern_Prefix_Status pstatus;
	Const	   *patt;
	Const	   *prefix = NULL;
	Selectivity rest_selec = 0;
	double		nullfrac = 0.0;
	double		result;

	/*
	 * 根据是匹配还是不匹配算子，把结果初始化为
	 * 相应的默认估算值。
	 */
	if (negate)
		result = 1.0 - DEFAULT_MATCH_SEL;
	else
		result = DEFAULT_MATCH_SEL;

	/*
	 * 若表达式不是"变量 算子 常量"形式，则放弃并返
	 * 回默认值。
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return result;
	if (!varonleft || !IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * 若常量为 NULL，则假设算子严格并返回零，即算
	 * 子永远不会返回 TRUE。（即便是取反算子也是零。）
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		return 0.0;
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * 对所有受支持的算子来说，右侧常量都是 text 或
	 * bytea 类型。这里不应出现二进制兼容类型，
	 * 因为常量折叠应当已把常量重新标记为与算子
	 * 声明的类型一致。
	 */
	if (consttype != TEXTOID && consttype != BYTEAOID)
	{
		ReleaseVariableStats(vardata);
		return result;
	}

	/*
	 * 类似地，左侧暴露出来的类型也应该是我们所知
	 * 的类型之一。（不要看 vardata.atttype，它可能
	 * 是二进制兼容但不同的类型。）我们可以用它
	 * 来识别比较算子以及比较常量所需的类型，
	 * 如同 match_pattern_prefix() 中那样。
	 */
	vartype = vardata.vartype;

	switch (vartype)
	{
		case TEXTOID:
			eqopr = TextEqualOperator;
			ltopr = TextLessOperator;
			geopr = TextGreaterEqualOperator;
			rdatatype = TEXTOID;
			break;
		case NAMEOID:

			/*
			 * 注意这里需要 RHS 类型为 text，这样比较值
			 * 才不会被不当截断到 NAMEDATALEN。
			 */
			eqopr = NameEqualTextOperator;
			ltopr = NameLessTextOperator;
			geopr = NameGreaterEqualTextOperator;
			rdatatype = TEXTOID;
			break;
		case BPCHAROID:
			eqopr = BpcharEqualOperator;
			ltopr = BpcharLessOperator;
			geopr = BpcharGreaterEqualOperator;
			rdatatype = BPCHAROID;
			break;
		case BYTEAOID:
			eqopr = ByteaEqualOperator;
			ltopr = ByteaLessOperator;
			geopr = ByteaGreaterEqualOperator;
			rdatatype = BYTEAOID;
			break;
		default:
			/* 除非挂接了错误的算子，否则到不了这里 */
			ReleaseVariableStats(vardata);
			return result;
	}

	/*
	 * 取 nullfrac 供下面用。
	 */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * 提取模式所隐含的任何固定前缀，并估算模式
	 * 其余部分的比例选择率。与许多其他选择率估
	 * 算器不同，这一步我们使用模式算子实际的排
	 * 序规则。这并非因为我们指望排序规则会对选
	 * 择率估算产生多大影响（它很少有影响），而
	 * 是因为我们要确保在正确的缓存键下缓存编译
	 * 好的正则，使其在运行时可以得到复用。
	 */
	patt = (Const *) other;
	pstatus = pattern_fixed_prefix(patt, ptype, collation,
								   &prefix, &rest_selec);

	/*
	 * 必要时把前缀常量强制转换为正确类型。唯一
	 * 需要处理的情形是 text 转换为 bpchar。这两
	 * 个类型二进制兼容，所以重新标记 Const 节点即
	 * 可。
	 */
	if (prefix && prefix->consttype != rdatatype)
	{
		Assert(prefix->consttype == TEXTOID &&
			   rdatatype == BPCHAROID);
		prefix->consttype = rdatatype;
	}

	if (pstatus == Pattern_Prefix_Exact)
	{
		/*
		 * 模式指定精确匹配，故按"="估算
		 */
		result = var_eq_const(&vardata, eqopr, collation, prefix->constvalue,
							  false, true, false);
	}
	else
	{
		/*
		 * 非精确匹配模式。若有足够大的直方图，
		 * 就通过统计直方图中的匹配数来估算总体中直
		 * 方图部分的选择率。若无，则分别估算固定前
		 * 缀与模式其余部分的选择率，再结合两者
		 * 来得到直方图所代表的列总体部分的选择
		 * 率。（对于小直方图，
		 * 我们把这两种做法结合起来使
		 * 用。）
		 *
		 * 然后我们为所有最常用值（MCV）累加数据；
		 * 这些值不在直方图总体之内，而把模式算子应
		 * 用于它们就能得到精确答案，所以没有理由去
		 * 近似。（若 MCV 覆盖了总体的相当大一部分，
		 * 就会给我们带来精度上的很大优势。）
		 */
		Selectivity selec;
		int			hist_size;
		FmgrInfo	opproc;
		double		mcv_selec,
					sumcommon;

		/* 尝试利用直方图条目来获得选择率 */
		if (!OidIsValid(opfuncid))
			opfuncid = get_opcode(oprid);
		fmgr_info(opfuncid, &opproc);

		selec = histogram_selectivity(&vardata, &opproc, collation,
									  constval, true,
									  10, 1, &hist_size);

		/* 若不足 100 个条目，则使用启发式方法 */
		if (hist_size < 100)
		{
			Selectivity heursel;
			Selectivity prefixsel;

			if (pstatus == Pattern_Prefix_Partial)
				prefixsel = prefix_selectivity(root, &vardata,
											   eqopr, ltopr, geopr,
											   collation,
											   prefix);
			else
				prefixsel = 1.0;
			heursel = prefixsel * rest_selec;

			if (selec < 0)		/* 直方图条目少于 10 个？ */
				selec = heursel;
			else
			{
				/*
				 * 直方图大小从 10 到 100 时，我们把直
				 * 方图选择率与启发式选择率相结合，直方图
				 * 越大就越相信直方图。
				 */
				double		hist_weight = hist_size / 100.0;

				selec = selec * hist_weight + heursel * (1.0 - hist_weight);
			}
		}

		/* 无论如何，不要相信过小或过大的估算值。 */
		if (selec < 0.0001)
			selec = 0.0001;
		else if (selec > 0.9999)
			selec = 0.9999;

		/*
		 * 若我们有最常用值（MCV）信息，就把
		 * 满足 MCV OP PATTERN 的 MCV 条目占比累加起来。
		 * 这些占比直接计入结果选择率。同时累加 MCV
		 * 条目的总占比。
		 */
		mcv_selec = mcv_selectivity(&vardata, &opproc, collation,
									constval, true,
									&sumcommon);

		/*
		 * 现在把 MCV 部分与直方图部分的结果合并，
		 * 注意直方图只覆盖未列入 MCV 的
		 * 非空值。
		 */
		selec *= 1.0 - nullfrac - sumcommon;
		selec += mcv_selec;
		result = selec;
	}

	/* 若想要的是不匹配而非匹配，现在调整 */
	if (negate)
		result = 1.0 - result - nullfrac;

	/* 结果应在范围内，但要确认…… */
	CLAMP_PROBABILITY(result);

	if (prefix)
	{
		pfree(DatumGetPointer(prefix->constvalue));
		pfree(prefix);
	}

	ReleaseVariableStats(vardata);

	return result;
}

/*
 * 修复 SQL 可调用函数与 patternsel_common 间的阻抗失配
 */
static double
patternsel(PG_FUNCTION_ARGS, Pattern_Type ptype, bool negate)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();

	/*
	 * 若这是用于 NOT LIKE 或类似算子，则取出对应
	 * 的正向匹配算子并据此工作。
	 */
	if (negate)
	{
		operator = get_negator(operator);
		if (!OidIsValid(operator))
			elog(ERROR, "patternsel called for operator without a negator");
	}

	return patternsel_common(root,
							 operator,
							 InvalidOid,
							 args,
							 varRelid,
							 collation,
							 ptype,
							 negate);
}

/*
 *		regexeqsel —— 正则表达式模式匹配的选择率。
 */
Datum
regexeqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex, false));
}

/*
 *		icregexeqsel —— 大小写不敏感正则匹配选择率
 */
Datum
icregexeqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex_IC, false));
}

/*
 *		likesel —— LIKE 模式匹配选择率。
 */
Datum
likesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like, false));
}

/*
 *		prefixsel —— 前缀算子的选择率
 */
Datum
prefixsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Prefix, false));
}

/*
 *
 *		iclikesel —— ILIKE 模式匹配选择率
 */
Datum
iclikesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like_IC, false));
}

/*
 *		regexnesel —— 正则表达式模式不匹配的选择率。
 */
Datum
regexnesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex, true));
}

/*
 *		icregexnesel —— 大小写不敏感正则不匹配选择率
 */
Datum
icregexnesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Regex_IC, true));
}

/*
 *		nlikesel —— LIKE 模式不匹配选择率。
 */
Datum
nlikesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like, true));
}

/*
 *		icnlikesel —— ILIKE 模式不匹配选择率。
 */
Datum
icnlikesel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternsel(fcinfo, Pattern_Type_Like_IC, true));
}

/*
 * patternjoinsel —— 模式匹配连接选择率通用代码。
 */
static double
patternjoinsel(PG_FUNCTION_ARGS, Pattern_Type ptype, bool negate)
{
	/* 目前我们只是放弃。 */
	return negate ? (1.0 - DEFAULT_MATCH_SEL) : DEFAULT_MATCH_SEL;
}

/*
 *		regexeqjoinsel —— 正则表达式模式匹配的连接选择率。
 */
Datum
regexeqjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex, false));
}

/*
 *		icregexeqjoinsel —— 大小写不敏感正则连接选择率。
 */
Datum
icregexeqjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex_IC, false));
}

/*
 *		likejoinsel —— LIKE 模式连接选择率。
 */
Datum
likejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like, false));
}

/*
 *		prefixjoinsel —— 前缀算子的连接选择率
 */
Datum
prefixjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Prefix, false));
}

/*
 *		iclikejoinsel —— ILIKE 模式连接选择率。
 */
Datum
iclikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like_IC, false));
}

/*
 *		regexnejoinsel —— 正则不匹配连接选择率
 */
Datum
regexnejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex, true));
}

/*
 *		icregexnejoinsel —— 大小写不敏感正则不匹配连接选择率
 */
Datum
icregexnejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Regex_IC, true));
}

/*
 *		nlikejoinsel —— LIKE 模式不匹配连接选择率。
 */
Datum
nlikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like, true));
}

/*
 *		icnlikejoinsel —— ILIKE 模式不匹配连接选择率。
 */
Datum
icnlikejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(patternjoinsel(fcinfo, Pattern_Type_Like_IC, true));
}


/*-------------------------------------------------------------------------
 *
 * 模式分析函数
 *
 * 这些例程为规划器/优化器对 LIKE 与正则表达式
 * 模式分析提供支持。重要的是它们要与
 * backend/regex/ 的正则代码及 backend/utils/adt/like.c
 * 中的 LIKE 代码保持一致。此外，固定前缀的计
 * 算必须保守：如果我们报告的字符串比真实固
 * 定前缀更长，查询可能产生真正错误的结果，
 * 而不只是糟糕的选择率！
 *
 *-------------------------------------------------------------------------
 */

/*
 * 提取模式对应的固定前缀（如有）。
 *
 * *prefix 被设为 palloc 出来的前缀串（以 Const
 *	节点形式），若无固定前缀则设为 NULL。
 * 若 rest_selec 非 NULL，则 *rest_selec 被设为模式
 *	其余部分（不含任何固定前缀）选择率的估算值。
 * 前缀 Const 与输入模式具有相同类型（TEXT 或 BYTEA）。
 *
 * 返回值用于区分：无固定前缀、部分前缀、
 * 或仅精确匹配的模式。
 */

static Pattern_Prefix_Status
like_fixed_prefix(Const *patt_const, bool case_insensitive, Oid collation,
				  Const **prefix_const, Selectivity *rest_selec)
{
	char	   *match;
	char	   *patt;
	int			pattlen;
	Oid			typeid = patt_const->consttype;
	int			pos,
				match_pos;
	bool		is_multibyte = (pg_database_encoding_max_length() > 1);
	pg_locale_t locale = 0;
	bool		locale_is_c = false;

	/* 右侧常量类型为 text 或 bytea */
	Assert(typeid == BYTEAOID || typeid == TEXTOID);

	if (case_insensitive)
	{
		if (typeid == BYTEAOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("case insensitive matching not supported on type bytea")));

		if (!OidIsValid(collation))
		{
			/*
			 * 这通常意味着解析器无法消解隐式排序
			 * 规则冲突，所以按这种方式报告。
			 */
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_COLLATION),
					 errmsg("could not determine which collation to use for ILIKE"),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
		}

		/* 大小写不敏感时需 locale 信息 */
		if (lc_ctype_is_c(collation))
			locale_is_c = true;
		else
			locale = pg_newlocale_from_collation(collation);
	}

	if (typeid != BYTEAOID)
	{
		patt = TextDatumGetCString(patt_const->constvalue);
		pattlen = strlen(patt);
	}
	else
	{
		bytea	   *bstr = DatumGetByteaPP(patt_const->constvalue);

		pattlen = VARSIZE_ANY_EXHDR(bstr);
		patt = (char *) palloc(pattlen);
		memcpy(patt, VARDATA_ANY(bstr), pattlen);
		Assert((Pointer) bstr == DatumGetPointer(patt_const->constvalue));
	}

	match = palloc(pattlen + 1);
	match_pos = 0;
	for (pos = 0; pos < pattlen; pos++)
	{
		/* % 和 _ 是 LIKE 中的通配符 */
		if (patt[pos] == '%' ||
			patt[pos] == '_')
			break;

		/* 反斜杠转义下一个字符 */
		if (patt[pos] == '\\')
		{
			pos++;
			if (pos >= pattlen)
				break;
		}

		/* 遇到大小写变化的字符就停（算通配符） */
		if (case_insensitive &&
			pattern_char_isalpha(patt[pos], is_multibyte, locale, locale_is_c))
			break;

		match[match_pos++] = patt[pos];
	}

	match[match_pos] = '\0';

	if (typeid != BYTEAOID)
		*prefix_const = string_to_const(match, typeid);
	else
		*prefix_const = string_to_bytea_const(match, match_pos);

	if (rest_selec != NULL)
		*rest_selec = like_selectivity(&patt[pos], pattlen - pos,
									   case_insensitive);

	pfree(patt);
	pfree(match);

	/* LIKE 中空模式即为精确匹配！ */
	if (pos == pattlen)
		return Pattern_Prefix_Exact;	/* 到模式末尾，故为精确 */

	if (match_pos > 0)
		return Pattern_Prefix_Partial;

	return Pattern_Prefix_None;
}

static Pattern_Prefix_Status
regex_fixed_prefix(Const *patt_const, bool case_insensitive, Oid collation,
				   Const **prefix_const, Selectivity *rest_selec)
{
	Oid			typeid = patt_const->consttype;
	char	   *prefix;
	bool		exact;

	/*
	 * 应当不必要——并没有定义 bytea 正则算子。因
	 * 此应当指出，本函数其余部分*并未*针对二进制
	 * （可能含 NULL）字符串做到安全。
	 */
	if (typeid == BYTEAOID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("regular-expression matching not supported on type bytea")));

	/* 若有固定前缀，则用正则机制提取 */
	prefix = regexp_fixed_prefix(DatumGetTextPP(patt_const->constvalue),
								 case_insensitive, collation,
								 &exact);

	if (prefix == NULL)
	{
		*prefix_const = NULL;

		if (rest_selec != NULL)
		{
			char	   *patt = TextDatumGetCString(patt_const->constvalue);

			*rest_selec = regex_selectivity(patt, strlen(patt),
											case_insensitive,
											0);
			pfree(patt);
		}

		return Pattern_Prefix_None;
	}

	*prefix_const = string_to_const(prefix, typeid);

	if (rest_selec != NULL)
	{
		if (exact)
		{
			/* 精确匹配，故没有额外的选择率 */
			*rest_selec = 1.0;
		}
		else
		{
			char	   *patt = TextDatumGetCString(patt_const->constvalue);

			*rest_selec = regex_selectivity(patt, strlen(patt),
											case_insensitive,
											strlen(prefix));
			pfree(patt);
		}
	}

	pfree(prefix);

	if (exact)
		return Pattern_Prefix_Exact;	/* 模式指定精确匹配 */
	else
		return Pattern_Prefix_Partial;
}

static Pattern_Prefix_Status
pattern_fixed_prefix(Const *patt, Pattern_Type ptype, Oid collation,
					 Const **prefix, Selectivity *rest_selec)
{
	Pattern_Prefix_Status result;

	switch (ptype)
	{
		case Pattern_Type_Like:
			result = like_fixed_prefix(patt, false, collation,
									   prefix, rest_selec);
			break;
		case Pattern_Type_Like_IC:
			result = like_fixed_prefix(patt, true, collation,
									   prefix, rest_selec);
			break;
		case Pattern_Type_Regex:
			result = regex_fixed_prefix(patt, false, collation,
										prefix, rest_selec);
			break;
		case Pattern_Type_Regex_IC:
			result = regex_fixed_prefix(patt, true, collation,
										prefix, rest_selec);
			break;
		case Pattern_Type_Prefix:
			/* 前缀类型工作很简单 */
			result = Pattern_Prefix_Partial;
			*prefix = makeConst(patt->consttype,
								patt->consttypmod,
								patt->constcollid,
								patt->constlen,
								datumCopy(patt->constvalue,
										  patt->constbyval,
										  patt->constlen),
								patt->constisnull,
								patt->constbyval);
			if (rest_selec != NULL)
				*rest_selec = 1.0;	/* 全 */
			break;
		default:
			elog(ERROR, "unrecognized ptype: %d", (int) ptype);
			result = Pattern_Prefix_None;	/* 让编译器安静 */
			break;
	}
	return result;
}

/*
 * 估算固定前缀对模式匹配的选择率。
 *
 * 固定前缀 "foo" 被估算为表达式的选择率
 * "variable >= 'foo' AND variable < 'fop'".
 *
 * 选择率估算针对的是直方图所代表的列总体部
 * 分——调用者必须把它与 MCV 及 NULL 的信息合
 * 并到一起。
 *
 * 我们使用给定的比较算子与排序规则来做估算。
 * 给定的变量与 Const 必须属于相关联的类型。
 *
 * XXX 注意：即使所处 locale 使得我们无法信赖上边
 * 界字符串，我们仍利用上边界来估算算子选择率。
 * 选择率本来就只需近似正确，所以使用上边界
 * 的代码看起来比不用更有用。
 */
static Selectivity
prefix_selectivity(PlannerInfo *root, VariableStatData *vardata,
				   Oid eqopr, Oid ltopr, Oid geopr,
				   Oid collation,
				   Const *prefixcon)
{
	Selectivity prefixsel;
	FmgrInfo	opproc;
	Const	   *greaterstrcon;
	Selectivity eq_sel;

	/* 估算 "x >= prefix" 的选择率 */
	fmgr_info(get_opcode(geopr), &opproc);

	prefixsel = ineq_histogram_selectivity(root, vardata,
										   geopr, &opproc, true, true,
										   collation,
										   prefixcon->constvalue,
										   prefixcon->consttype);

	if (prefixsel < 0.0)
	{
		/* 没有直方图……返回一个合适的默认估算值 */
		return DEFAULT_MATCH_SEL;
	}

	/*
	 * 若能构造出比前缀更大的字符串，就说 "x < greaterstr"。
	 */
	fmgr_info(get_opcode(ltopr), &opproc);
	greaterstrcon = make_greater_string(prefixcon, &opproc, collation);
	if (greaterstrcon)
	{
		Selectivity topsel;

		topsel = ineq_histogram_selectivity(root, vardata,
											ltopr, &opproc, false, false,
											collation,
											greaterstrcon->constvalue,
											greaterstrcon->consttype);

		/* ineq_histogram_selectivity 之前成功了，现在不应失败 */
		Assert(topsel >= 0.0);

		/*
		 * 以与范围查询相同的方式合并两个选择率
		 * （见 clauselist_selectivity()）。注意无须担心把
		 * NULL 排除两次，因为 ineq_histogram_selectivity
		 * 反正也不统计它们。
		 */
		prefixsel = topsel + prefixsel - 1.0;
	}

	/*
	 * 若前缀很长，两个边界值可能靠得太近，直方图
	 * 无法有效区分它们，导致估算值为零（加减舍入
	 * 误差）。为避免返回小得离谱的估算值，先计算
	 * "variable = 'foo'" 的估算选择率，并把结果
	 * 钳制到该值。（显然所得估算值至少应
	 * 达到该值。）
	 *
	 * 即使我们无法构造出更大的字符串，我们也应用这
	 * 一做法。该情形说明前缀已接近可能的最大值，
	 * 因而很可能落在直方图末端之外，因此我们从 >= 条件
	 * 得到的估算值很可能非常小，故仍需钳制。
	 */
	eq_sel = var_eq_const(vardata, eqopr, collation, prefixcon->constvalue,
						  false, true, false);

	prefixsel = Max(prefixsel, eq_sel);

	return prefixsel;
}


/*
 * 估算指定类型模式的选择率。
 * 注意模式的任何固定前缀都已被移除，所以实
 * 际上我们看到的可能只是模式的一个片段。
 *
 * 目前我们使用一种非常简单的方法：固定字符
 * 会大幅降低选择率，字符范围小幅降低它，
 * 而通配符（如 LIKE % 或正则 .*）会提高它。
 */

#define FIXED_CHAR_SEL	0.20	/* 约 1/5 */
#define CHAR_RANGE_SEL	0.25
#define ANY_CHAR_SEL	0.9		/* 不是 1，因为它匹配不到串尾 */
#define FULL_WILDCARD_SEL 5.0
#define PARTIAL_WILDCARD_SEL 2.0

static Selectivity
like_selectivity(const char *patt, int pattlen, bool case_insensitive)
{
	Selectivity sel = 1.0;
	int			pos;

	/* 跳过任何前导通配符；它已计入初始选择率 */
	for (pos = 0; pos < pattlen; pos++)
	{
		if (patt[pos] != '%' && patt[pos] != '_')
			break;
	}

	for (; pos < pattlen; pos++)
	{
		/* % 和 _ 是 LIKE 中的通配符 */
		if (patt[pos] == '%')
			sel *= FULL_WILDCARD_SEL;
		else if (patt[pos] == '_')
			sel *= ANY_CHAR_SEL;
		else if (patt[pos] == '\\')
		{
			/* 反斜杠引用下一个字符 */
			pos++;
			if (pos >= pattlen)
				break;
			sel *= FIXED_CHAR_SEL;
		}
		else
			sel *= FIXED_CHAR_SEL;
	}
	/* 多个通配符时 sel 可能 > 1 */
	if (sel > 1.0)
		sel = 1.0;
	return sel;
}

static Selectivity
regex_selectivity_sub(const char *patt, int pattlen, bool case_insensitive)
{
	Selectivity sel = 1.0;
	int			paren_depth = 0;
	int			paren_pos = 0;	/* 哑初始化，让编译器安静 */
	int			pos;

	/* 本函数会递归，因此可能被驱动到栈溢出 */
	check_stack_depth();

	for (pos = 0; pos < pattlen; pos++)
	{
		if (patt[pos] == '(')
		{
			if (paren_depth == 0)
				paren_pos = pos;	/* 记住括号项的起始位置 */
			paren_depth++;
		}
		else if (patt[pos] == ')' && paren_depth > 0)
		{
			paren_depth--;
			if (paren_depth == 0)
				sel *= regex_selectivity_sub(patt + (paren_pos + 1),
											 pos - (paren_pos + 1),
											 case_insensitive);
		}
		else if (patt[pos] == '|' && paren_depth == 0)
		{
			/*
			 * 若模式中括号层级 0 处出现未加引号的 |，
			 * 则有多个备选分支；概率相加。
			 */
			sel += regex_selectivity_sub(patt + (pos + 1),
										 pattlen - (pos + 1),
										 case_insensitive);
			break;				/* 其余模式已处理完毕 */
		}
		else if (patt[pos] == '[')
		{
			bool		negclass = false;

			if (patt[++pos] == '^')
			{
				negclass = true;
				pos++;
			}
			if (patt[pos] == ']')	/* ']' 处于类开头时不特殊 */
				pos++;
			while (pos < pattlen && patt[pos] != ']')
				pos++;
			if (paren_depth == 0)
				sel *= (negclass ? (1.0 - CHAR_RANGE_SEL) : CHAR_RANGE_SEL);
		}
		else if (patt[pos] == '.')
		{
			if (paren_depth == 0)
				sel *= ANY_CHAR_SEL;
		}
		else if (patt[pos] == '*' ||
				 patt[pos] == '?' ||
				 patt[pos] == '+')
		{
			/* 对量词本应更聪明一些…… */
			if (paren_depth == 0)
				sel *= PARTIAL_WILDCARD_SEL;
		}
		else if (patt[pos] == '{')
		{
			while (pos < pattlen && patt[pos] != '}')
				pos++;
			if (paren_depth == 0)
				sel *= PARTIAL_WILDCARD_SEL;
		}
		else if (patt[pos] == '\\')
		{
			/* 反斜杠引用下一个字符 */
			pos++;
			if (pos >= pattlen)
				break;
			if (paren_depth == 0)
				sel *= FIXED_CHAR_SEL;
		}
		else
		{
			if (paren_depth == 0)
				sel *= FIXED_CHAR_SEL;
		}
	}
	/* 多个通配符时 sel 可能 > 1 */
	if (sel > 1.0)
		sel = 1.0;
	return sel;
}

static Selectivity
regex_selectivity(const char *patt, int pattlen, bool case_insensitive,
				  int fixed_prefix_len)
{
	Selectivity sel;

	/* 若 patt 不以 $ 结尾，则视为带有尾部通配符 */
	if (pattlen > 0 && patt[pattlen - 1] == '$' &&
		(pattlen == 1 || patt[pattlen - 2] != '\\'))
	{
		/* 有尾部 $ */
		sel = regex_selectivity_sub(patt, pattlen - 1, case_insensitive);
	}
	else
	{
		/* 无尾部 $ */
		sel = regex_selectivity_sub(patt, pattlen, case_insensitive);
		sel *= FULL_WILDCARD_SEL;
	}

	/*
	 * 若有固定前缀，则对其选择率打折扣。这里必须
	 * 小心，因为非常长的前缀可能导致 pow 的结果
	 * 下溢为零（此时 "sel" 很可能同样如此）。
	 */
	if (fixed_prefix_len > 0)
	{
		double		prefixsel = pow(FIXED_CHAR_SEL, fixed_prefix_len);

		if (prefixsel > 0.0)
			sel /= prefixsel;
	}

	/* 确保结果保持在范围内 */
	CLAMP_PROBABILITY(sel);
	return sel;
}

/*
 * 检查字符是否为字母（因而会受大小写折叠影响）
 *
 * 在多字节字符集或 ICU 下无法使用 isalpha，而
 * 尝试转换为 wchar_t 去用 iswalpha 或 u_isalpha
 * 又似乎不值得。因此，直接假定任何非 ASCII
 * 字符都可能变大小写，字母集硬编码。
 */
static int
pattern_char_isalpha(char c, bool is_multibyte,
					 pg_locale_t locale, bool locale_is_c)
{
	if (locale_is_c)
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
	else if (is_multibyte && IS_HIGHBIT_SET(c))
		return true;
	else if (locale && locale->provider == COLLPROVIDER_ICU)
		return IS_HIGHBIT_SET(c) ||
			(c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
#ifdef HAVE_LOCALE_T
	else if (locale && locale->provider == COLLPROVIDER_LIBC)
		return isalpha_l((unsigned char) c, locale->info.lt);
#endif
	else
		return isalpha((unsigned char) c);
}


/*
 * 对 bytea 而言，递增函数只需递增当前字节
 * （没有需要担心的多字节字符）。
 */
static bool
byte_increment(unsigned char *ptr, int len)
{
	if (*ptr >= 255)
		return false;
	(*ptr)++;
	return true;
}

/*
 * 尝试生成一个比给定字符串更大、或比以它为
 * 前缀的字符串都大的字符串。若成功，则返回
 * palloc 的 Const 节点；否则 NULL。
 *
 * 调用者必须提供用于测试字符串的相应"小于"
 * 比较函数，以及要使用的排序规则。
 *
 * 这里的关键要求是：给定前缀串 "foo"，我们
 * 必须能生成另一个字符串 "fop"，它比所有
 * 以 "foo" 开头的 "foobar" 串都大。我们可以测
 * 试我们生成的串是否大于前缀串，但在非 C 排
 * 序规则下这并不能万无一失地保证字符串的扩
 * 展不会排在它之后；一个例子是 "foo " 小于
 * "foo!"，但"字典"式排序是否认为 "foo!" 小于
 * "foo bar" 并不明晰。当心：因此，在非 C 排序
 * 规则下工作时本函数只应用于估算目的。
 *
 * 为尽量覆盖扩展串本可能排在结果值之前的
 * 大多数情形，我们确定 "Z"、"z"、"y"、"9" 中
 * 哪个被排序规则视为最大，并在尝试寻找比较
 * 值更大的串之前把它追加到给定前缀上。
 *
 * 为搜索更大的串，我们反复使用特定编码的字符
 * 递增函数来"递增"最右边的字符。当无法再
 * 递增最后一个字符时，我们把该字符截去，并
 * 开始递增次右边的字符。例如，若 "z" 是排序
 * 顺序中的最后一个字符，那么我们可以生成
 * "foo" 作为大于 "fonz" 的串。
 *
 * 最坏情形下这可能相当慢，但多数情形我们
 * 只需尝试一两个字符串就能成功。
 *
 * 注意，字符递增器不应过分执着于产出每个可
 * 能的字符码，因为在某些情形下得到更大串的
 * 唯一办法就是递增前面的某个字符位置。所以
 * 我们不想在最后一个位置上耗费太多时间逐个
 * 尝试所有字符码。好的经验法则是把尝试次数
 * 限制在 K 字节字符至多 256*K 个值以内（当
 * 然绝不是 256^K，穷举搜索才会逼近该量级）。
 */
static Const *
make_greater_string(const Const *str_const, FmgrInfo *ltproc, Oid collation)
{
	Oid			datatype = str_const->consttype;
	char	   *workstr;
	int			len;
	Datum		cmpstr;
	char	   *cmptxt = NULL;
	mbcharacter_incrementer charinc;

	/*
	 * 以 C 串格式获得前缀串的可修改副本，并把将
	 * 要与之比较的串设置为 Datum。在 C locale 下这
	 * 可以直接用给定的前缀串，否则需要添加后缀。
	 * bytea 按字节排序，因此同样无须后缀。
	 */
	if (datatype == BYTEAOID)
	{
		bytea	   *bstr = DatumGetByteaPP(str_const->constvalue);

		len = VARSIZE_ANY_EXHDR(bstr);
		workstr = (char *) palloc(len);
		memcpy(workstr, VARDATA_ANY(bstr), len);
		Assert((Pointer) bstr == DatumGetPointer(str_const->constvalue));
		cmpstr = str_const->constvalue;
	}
	else
	{
		if (datatype == NAMEOID)
			workstr = DatumGetCString(DirectFunctionCall1(nameout,
														  str_const->constvalue));
		else
			workstr = TextDatumGetCString(str_const->constvalue);
		len = strlen(workstr);
		if (lc_collate_is_c(collation) || len == 0)
			cmpstr = str_const->constvalue;
		else
		{
			/* 若是第一轮，则确定要使用的后缀 */
			static char suffixchar = 0;
			static Oid	suffixcollation = 0;

			if (!suffixchar || suffixcollation != collation)
			{
				char	   *best;

				best = "Z";
				if (varstr_cmp(best, 1, "z", 1, collation) < 0)
					best = "z";
				if (varstr_cmp(best, 1, "y", 1, collation) < 0)
					best = "y";
				if (varstr_cmp(best, 1, "9", 1, collation) < 0)
					best = "9";
				suffixchar = *best;
				suffixcollation = collation;
			}

			/* 然后构造要与之比较的串 */
			if (datatype == NAMEOID)
			{
				cmptxt = palloc(len + 2);
				memcpy(cmptxt, workstr, len);
				cmptxt[len] = suffixchar;
				cmptxt[len + 1] = '\0';
				cmpstr = PointerGetDatum(cmptxt);
			}
			else
			{
				cmptxt = palloc(VARHDRSZ + len + 1);
				SET_VARSIZE(cmptxt, VARHDRSZ + len + 1);
				memcpy(VARDATA(cmptxt), workstr, len);
				*(VARDATA(cmptxt) + len) = suffixchar;
				cmpstr = PointerGetDatum(cmptxt);
			}
		}
	}

	/* 选择适当的字符递增函数 */
	if (datatype == BYTEAOID)
		charinc = byte_increment;
	else
		charinc = pg_database_encoding_character_incrementer();

	/* 搜索… */
	while (len > 0)
	{
		int			charlen;
		unsigned char *lastchar;

		/* 识别最后一个字符——bytea 就是最后一字节 */
		if (datatype == BYTEAOID)
			charlen = 1;
		else
			charlen = len - pg_mbcliplen(workstr, len, len - 1);
		lastchar = (unsigned char *) (workstr + len - charlen);

		/*
		 * 尝试通过递增最后一个字符来生成更大的串
		 * （BYTEA 则把每字节当作一个字符）
		 *
		 * 注意：期望递增函数在生成了编码合法的新字
		 * 符时返回 true，否则返回 false。返回 false 时
		 * 该字符的内容是未定义的。
		 */
		while (charinc(lastchar, charlen))
		{
			Const	   *workstr_const;

			if (datatype == BYTEAOID)
				workstr_const = string_to_bytea_const(workstr, len);
			else
				workstr_const = string_to_const(workstr, datatype);

			if (DatumGetBool(FunctionCall2Coll(ltproc,
											   collation,
											   cmpstr,
											   workstr_const->constvalue)))
			{
				/* 成功构造出比 cmpstr 更大的串 */
				if (cmptxt)
					pfree(cmptxt);
				pfree(workstr);
				return workstr_const;
			}

			/* 不行，释放不可用的值再试一次 */
			pfree(DatumGetPointer(workstr_const->constvalue));
			pfree(workstr_const);
		}

		/*
		 * 此处无果，于是截去最后字符，并尝试
		 * 递增下一个。
		 */
		len -= charlen;
		workstr[len] = '\0';
	}

	/* 失败… */
	if (cmptxt)
		pfree(cmptxt);
	pfree(workstr);

	return NULL;
}

/*
 * 从 C 字符串生成相应类型的 Datum。
 * 注意所有受支持的类型都按引用传递，因此
 * 返回值在不再需要时应被 pfree。
 */
static Datum
string_to_datum(const char *str, Oid datatype)
{
	Assert(str != NULL);

	/*
	 * 我们稍微作弊了一下：假定 CStringGetTextDatum()
	 * 对 bpchar/varchar 常量也适用
	 */
	if (datatype == NAMEOID)
		return DirectFunctionCall1(namein, CStringGetDatum(str));
	else if (datatype == BYTEAOID)
		return DirectFunctionCall1(byteain, CStringGetDatum(str));
	else
		return CStringGetTextDatum(str);
}

/*
 * 从 C 字符串生成相应类型的 Const 节点。
 */
static Const *
string_to_const(const char *str, Oid datatype)
{
	Datum		conval = string_to_datum(str, datatype);
	Oid			collation;
	int			constlen;

	/*
	 * 这里只需支持少数几种数据类型，所以直接
	 * 写死属性，免去查询系统目录的开销。
	 */
	switch (datatype)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			collation = DEFAULT_COLLATION_OID;
			constlen = -1;
			break;

		case NAMEOID:
			collation = C_COLLATION_OID;
			constlen = NAMEDATALEN;
			break;

		case BYTEAOID:
			collation = InvalidOid;
			constlen = -1;
			break;

		default:
			elog(ERROR, "unexpected datatype in string_to_const: %u",
				 datatype);
			return NULL;
	}

	return makeConst(datatype, -1, collation, constlen,
					 conval, false, false);
}

/*
 * 由二进制 C 串与长度生成 bytea 类型的 Const 节点。
 */
static Const *
string_to_bytea_const(const char *str, size_t str_len)
{
	bytea	   *bstr = palloc(VARHDRSZ + str_len);
	Datum		conval;

	memcpy(VARDATA(bstr), str, str_len);
	SET_VARSIZE(bstr, VARHDRSZ + str_len);
	conval = PointerGetDatum(bstr);

	return makeConst(BYTEAOID, -1, InvalidOid, -1, conval, false, false);
}
