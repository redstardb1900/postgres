/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/crosstabview.c
 */
#include "postgres_fe.h"

#include "common.h"
#include "common/logging.h"
#include "crosstabview.h"
#include "pqexpbuffer.h"
#include "psqlscanslash.h"
#include "settings.h"

/*
 * 结果集中进入交叉表视图水平或垂直
 * 表头的值/位置。
 */
typedef struct _pivot_field
{
	/*
	 * 从 colV 或 colH 的 PQgetvalue() 获取的指针。每个不同的
	 * 值成为垂直表头 (colV) 或水平表头 (colH) 中的一个条目。
	 * Null 值由 NULL 指针表示。
	 */
	char	   *name;

	/*
	 * 当请求在备用列上排序时，这里保存与 <name> 对应的
	 * 排序列的 PQgetvalue()。如果 <name> 出现多次，保留的是
	 * 结果集中第一次出现的值。Null 值由 NULL 指针表示。
	 *
	 */
	char	   *sort_value;

	/*
	 * 此值的排名，从 0 开始。初始为 <name> 在结果集中
	 * 第一次出现的相对位置。例如，如果连续行包含
	 * B,A,C,A,D，则为 B:0,A:1,C:2,D:3。
	 * 当指定排序列时，排名会在最终遍历中更新以反映
	 * 所需的顺序。
	 */
	int			rank;
} pivot_field;

/* avl_tree 中的节点 */
typedef struct _avl_node
{
	/* 节点内容 */
	pivot_field field;

	/*
	 * 树中此节点的高度（从该节点到叶子节点的最长路径上的
	 * 节点数）。
	 */
	int			height;

	/*
	 * 子节点。[0] 指向左子树，[1] 指向右子树。
	 * 永不为 NULL，当没有左值或右值时指向空节点
	 * avl_tree.end。
	 */
	struct _avl_node *children[2];
} avl_node;

/*
 * AVL 树的控制结构（使用 AVL 算法保持平衡的
 * 二叉搜索树）
 */
typedef struct _avl_tree
{
	int			count;			/* 节点总数 */
	avl_node   *root;			/* 树的根节点 */
	avl_node   *end;			/* 不可变且可解引用的空树 */
} avl_tree;


static bool printCrosstab(const PGresult *result,
						  int num_columns, pivot_field *piv_columns, int field_for_columns,
						  int num_rows, pivot_field *piv_rows, int field_for_rows,
						  int field_for_data);
static void avlInit(avl_tree *tree);
static void avlMergeValue(avl_tree *tree, char *name, char *sort_value);
static int	avlCollectFields(avl_tree *tree, avl_node *node,
							 pivot_field *fields, int idx);
static void avlFree(avl_tree *tree, avl_node *node);
static void rankSort(int num_columns, pivot_field *piv_columns);
static int	indexOfColumn(char *arg, const PGresult *res);
static int	pivotFieldCompare(const void *a, const void *b);
static int	rankCompare(const void *a, const void *b);


/*
 * 本模块的主入口点。
 *
 * 根据 pset（全局）中的选项处理 *res 中的数据，
 * 生成水平和垂直表头内容，然后调用 printCrosstab()
 * 进行实际输出。
 */
bool
PrintResultInCrosstab(const PGresult *res)
{
	bool		retval = false;
	avl_tree	piv_columns;
	avl_tree	piv_rows;
	pivot_field *array_columns = NULL;
	pivot_field *array_rows = NULL;
	int			num_columns = 0;
	int			num_rows = 0;
	int			field_for_rows;
	int			field_for_columns;
	int			field_for_data;
	int			sort_field_for_columns;
	int			rn;

	avlInit(&piv_rows);
	avlInit(&piv_columns);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		pg_log_error("\\crosstabview: statement did not return a result set");
		goto error_return;
	}

	if (PQnfields(res) < 3)
	{
		pg_log_error("\\crosstabview: query must return at least three columns");
		goto error_return;
	}

	/* 处理第一个可选参数（垂直表头列） */
	if (pset.ctv_args[0] == NULL)
		field_for_rows = 0;
	else
	{
		field_for_rows = indexOfColumn(pset.ctv_args[0], res);
		if (field_for_rows < 0)
			goto error_return;
	}

	/* 处理第二个可选参数（水平表头列） */
	if (pset.ctv_args[1] == NULL)
		field_for_columns = 1;
	else
	{
		field_for_columns = indexOfColumn(pset.ctv_args[1], res);
		if (field_for_columns < 0)
			goto error_return;
	}

	/* 坚持表头列不同 */
	if (field_for_columns == field_for_rows)
	{
		pg_log_error("\\crosstabview: vertical and horizontal headers must be different columns");
		goto error_return;
	}

	/* 处理第三个可选参数（数据列） */
	if (pset.ctv_args[2] == NULL)
	{
		int			i;

		/*
			 * 如果未指定数据列，我们搜索既未用作垂直表头
			 * 也未用作水平表头的那一列。查询结果必须恰好
			 * 有三列，否则这不会是唯一的。
		 */
		if (PQnfields(res) != 3)
		{
			pg_log_error("\\crosstabview: data column must be specified when query returns more than three columns");
			goto error_return;
		}

		field_for_data = -1;
		for (i = 0; i < PQnfields(res); i++)
		{
			if (i != field_for_rows && i != field_for_columns)
			{
				field_for_data = i;
				break;
			}
		}
		Assert(field_for_data >= 0);
	}
	else
	{
		field_for_data = indexOfColumn(pset.ctv_args[2], res);
		if (field_for_data < 0)
			goto error_return;
	}

	/* 处理第四个可选参数（水平表头排序列） */
	if (pset.ctv_args[3] == NULL)
		sort_field_for_columns = -1;	/* 无排序列 */
	else
	{
		sort_field_for_columns = indexOfColumn(pset.ctv_args[3], res);
		if (sort_field_for_columns < 0)
			goto error_return;
	}

	/*
	 * 第一部分：累积将进入垂直和水平表头的名称，
	 * 每个名称放入一个 AVL 二叉树中以构建
	 * DISTINCT 值的集合。
	 */

	for (rn = 0; rn < PQntuples(res); rn++)
	{
		char	   *val;
		char	   *val1;

		/* 水平 */
		val = PQgetisnull(res, rn, field_for_columns) ? NULL :
			PQgetvalue(res, rn, field_for_columns);
		val1 = NULL;

		if (sort_field_for_columns >= 0 &&
			!PQgetisnull(res, rn, sort_field_for_columns))
			val1 = PQgetvalue(res, rn, sort_field_for_columns);

		avlMergeValue(&piv_columns, val, val1);

		if (piv_columns.count > CROSSTABVIEW_MAX_COLUMNS)
		{
			pg_log_error("\\crosstabview: maximum number of columns (%d) exceeded",
						 CROSSTABVIEW_MAX_COLUMNS);
			goto error_return;
		}

		/* 垂直 */
		val = PQgetisnull(res, rn, field_for_rows) ? NULL :
			PQgetvalue(res, rn, field_for_rows);

		avlMergeValue(&piv_rows, val, NULL);
	}

	/*
	 * 第二部分：从 AVL 树生成排序数组。
	 */

	num_columns = piv_columns.count;
	num_rows = piv_rows.count;

	array_columns = (pivot_field *)
		pg_malloc(sizeof(pivot_field) * num_columns);

	array_rows = (pivot_field *)
		pg_malloc(sizeof(pivot_field) * num_rows);

	avlCollectFields(&piv_columns, piv_columns.root, array_columns, 0);
	avlCollectFields(&piv_rows, piv_rows.root, array_rows, 0);

	/*
	 * 第三部分：可选地处理水平表头的排名数据
	 *
	 */
	if (sort_field_for_columns >= 0)
		rankSort(num_columns, array_columns);

	/*
	 * 第四部分：打印数据透视结果。
	 */
	retval = printCrosstab(res,
						   num_columns, array_columns, field_for_columns,
						   num_rows, array_rows, field_for_rows,
						   field_for_data);

error_return:
	avlFree(&piv_columns, piv_columns.root);
	avlFree(&piv_rows, piv_rows.root);
	pg_free(array_columns);
	pg_free(array_rows);

	return retval;
}

/*
 * 使用 printTable* 函数输出数据透视结果集。成功返回 true，
 * 失败返回 false。
 */
static bool
printCrosstab(const PGresult *result,
			  int num_columns, pivot_field *piv_columns, int field_for_columns,
			  int num_rows, pivot_field *piv_rows, int field_for_rows,
			  int field_for_data)
{
	printQueryOpt popt = pset.popt;
	printTableContent cont;
	int			i,
				rn;
	char		col_align;
	int		   *horiz_map;
	bool		retval = false;

	printTableInit(&cont, &popt.topt, popt.title, num_columns + 1, num_rows);

	/* 步骤 1：设置目标列名（水平表头） */

	/* 第一列的名称经数据透视后保持不变 */
	printTableAddHeader(&cont,
						PQfname(result, field_for_rows),
						false,
						column_type_alignment(PQftype(result,
													  field_for_rows)));

	/*
	 * 要按 piv_columns[].rank 迭代 piv_columns[]，创建一个反向
	 * 映射，将每个 piv_columns[].rank 关联到它在 piv_columns
	 * 中的索引。这避免了后续的 O(N^2) 循环。
	 */
	horiz_map = (int *) pg_malloc(sizeof(int) * num_columns);
	for (i = 0; i < num_columns; i++)
		horiz_map[piv_columns[i].rank] = i;

	/*
	 * 显示对齐取决于其 PQftype()。
	 */
	col_align = column_type_alignment(PQftype(result, field_for_data));

	for (i = 0; i < num_columns; i++)
	{
		char	   *colname;

		colname = piv_columns[horiz_map[i]].name ?
			piv_columns[horiz_map[i]].name :
			(popt.nullPrint ? popt.nullPrint : "");

		printTableAddHeader(&cont, colname, false, col_align);
	}
	pg_free(horiz_map);

	/* 步骤 2：在第一输出列中设置行名（垂直表头） */
	for (i = 0; i < num_rows; i++)
	{
		int			k = piv_rows[i].rank;

		cont.cells[k * (num_columns + 1)] = piv_rows[i].name ?
			piv_rows[i].name :
			(popt.nullPrint ? popt.nullPrint : "");
	}
	cont.cellsadded = num_rows * (num_columns + 1);

	/*
	 * 步骤 3：填充内容单元格。
	 */
	for (rn = 0; rn < PQntuples(result); rn++)
	{
		int			row_number;
		int			col_number;
		pivot_field *rp,
				   *cp;
		pivot_field elt;

		/* 查找目标行 */
		if (!PQgetisnull(result, rn, field_for_rows))
			elt.name = PQgetvalue(result, rn, field_for_rows);
		else
			elt.name = NULL;
		rp = (pivot_field *) bsearch(&elt,
									 piv_rows,
									 num_rows,
									 sizeof(pivot_field),
									 pivotFieldCompare);
		Assert(rp != NULL);
		row_number = rp->rank;

		/* 查找目标列 */
		if (!PQgetisnull(result, rn, field_for_columns))
			elt.name = PQgetvalue(result, rn, field_for_columns);
		else
			elt.name = NULL;

		cp = (pivot_field *) bsearch(&elt,
									 piv_columns,
									 num_columns,
									 sizeof(pivot_field),
									 pivotFieldCompare);
		Assert(cp != NULL);
		col_number = cp->rank;

		/* 将值放入单元格 */
		if (col_number >= 0 && row_number >= 0)
		{
			int			idx;

			/* cont.cells 数组中的索引 */
			idx = 1 + col_number + row_number * (num_columns + 1);

			/*
				 * 如果单元格已包含值，则引发错误。
			 */
			if (cont.cells[idx] != NULL)
			{
				pg_log_error("\\crosstabview: query result contains multiple data values for row \"%s\", column \"%s\"",
							 rp->name ? rp->name :
							 (popt.nullPrint ? popt.nullPrint : "(null)"),
							 cp->name ? cp->name :
							 (popt.nullPrint ? popt.nullPrint : "(null)"));
				goto error;
			}

			cont.cells[idx] = !PQgetisnull(result, rn, field_for_data) ?
				PQgetvalue(result, rn, field_for_data) :
				(popt.nullPrint ? popt.nullPrint : "");
		}
	}

	/*
	 * 未初始化的单元格必须为打印函数设置为空字符串
	 *
	 */
	for (i = 0; i < cont.cellsadded; i++)
	{
		if (cont.cells[i] == NULL)
			cont.cells[i] = "";
	}

	printTable(&cont, pset.queryFout, false, pset.logfile);
	retval = true;

error:
	printTableCleanup(&cont);

	return retval;
}

/*
 * 下面的 avl* 函数提供了一个最小化的 AVL 二叉树实现，
 * 用于高效收集将形成水平和垂直表头的不同值。
 * 它只支持添加新值，不支持删除甚至搜索。
 *
 */
static void
avlInit(avl_tree *tree)
{
	tree->end = (avl_node *) pg_malloc0(sizeof(avl_node));
	tree->end->children[0] = tree->end->children[1] = tree->end;
	tree->count = 0;
	tree->root = tree->end;
}

/* 从节点开始递归释放 AVL 树 */
static void
avlFree(avl_tree *tree, avl_node *node)
{
	if (node->children[0] != tree->end)
	{
		avlFree(tree, node->children[0]);
		pg_free(node->children[0]);
	}
	if (node->children[1] != tree->end)
	{
		avlFree(tree, node->children[1]);
		pg_free(node->children[1]);
	}
	if (node == tree->root)
	{
		/* 单独释放根节点，因为它不是任何节点的子节点 */
		if (node != tree->end)
			pg_free(node);
		/* 仅在其它所有都已释放后释放 tree->end 结构体 */
		pg_free(tree->end);
	}
}

/* 将高度设置为左右高度中较大者加 1 */
static void
avlUpdateHeight(avl_node *n)
{
	n->height = 1 + (n->children[0]->height > n->children[1]->height ?
					 n->children[0]->height :
					 n->children[1]->height);
}

/* 向左 (dir=0) 或向右 (dir=1) 旋转子树。非递归 */
static avl_node *
avlRotate(avl_node **current, int dir)
{
	avl_node   *before = *current;
	avl_node   *after = (*current)->children[dir];

	*current = after;
	before->children[dir] = after->children[!dir];
	avlUpdateHeight(before);
	after->children[!dir] = before;

	return after;
}

static int
avlBalance(avl_node *n)
{
	return n->children[0]->height - n->children[1]->height;
}

/*
	 * 插入后，可能需要对树重新平衡，使左右节点高度
	 * 之差不超过 1。可能更新 *node。
	 *
 */
static void
avlAdjustBalance(avl_tree *tree, avl_node **node)
{
	avl_node   *current = *node;
	int			b = avlBalance(current) / 2;

	if (b != 0)
	{
		int			dir = (1 - b) / 2;

		if (avlBalance(current->children[dir]) == -b)
			avlRotate(&current->children[dir], !dir);
		current = avlRotate(node, dir);
	}
	if (current != tree->end)
		avlUpdateHeight(current);
}

/*
	 * 插入一个新值/字段，从 *node 开始，通过递归到达树中
	 * 正确的位置。可能重新平衡树，可能更新 *node。
	 * 如果值已存在于树中，则不执行任何操作。
 */
static void
avlInsertNode(avl_tree *tree, avl_node **node, pivot_field field)
{
	avl_node   *current = *node;

	if (current == tree->end)
	{
		avl_node   *new_node = (avl_node *)
			pg_malloc(sizeof(avl_node));

		new_node->height = 1;
		new_node->field = field;
		new_node->children[0] = new_node->children[1] = tree->end;
		tree->count++;
		*node = new_node;
	}
	else
	{
		int			cmp = pivotFieldCompare(&field, &current->field);

		if (cmp != 0)
		{
			avlInsertNode(tree,
						  cmp > 0 ? &current->children[1] : &current->children[0],
						  field);
			avlAdjustBalance(tree, node);
		}
	}
}

/* 如果值尚不存在，则将其插入 AVL 树 */
static void
avlMergeValue(avl_tree *tree, char *name, char *sort_value)
{
	pivot_field field;

	field.name = name;
	field.rank = tree->count;
	field.sort_value = sort_value;
	avlInsertNode(tree, &tree->root, field);
}

/*
	 * 递归提取节点值到 names 数组中，按排序顺序通过
	 * 从左到右的树遍历。
	 * 返回要写入 names 数组的下一个候选偏移量。
	 * fields[] 必须预分配以容纳 tree->count 个条目
 */
static int
avlCollectFields(avl_tree *tree, avl_node *node, pivot_field *fields, int idx)
{
	if (node == tree->end)
		return idx;

	idx = avlCollectFields(tree, node->children[0], fields, idx);
	fields[idx] = node->field;
	return avlCollectFields(tree, node->children[1], fields, idx + 1);
}

static void
rankSort(int num_columns, pivot_field *piv_columns)
{
	int		   *hmap;			/* [[offset in piv_columns, rank], ...for
								 * every header entry] */
	int			i;

	hmap = (int *) pg_malloc(sizeof(int) * num_columns * 2);
	for (i = 0; i < num_columns; i++)
	{
		char	   *val = piv_columns[i].sort_value;

		/* 排名信息在非空且匹配 /^-?\d+$/ 时有效 */
		if (val &&
			((*val == '-' &&
			  strspn(val + 1, "0123456789") == strlen(val + 1)) ||
			 strspn(val, "0123456789") == strlen(val)))
		{
			hmap[i * 2] = atoi(val);
			hmap[i * 2 + 1] = i;
		}
		else
		{
			/* 无效的排名信息被忽略（等效于排名 0） */
			hmap[i * 2] = 0;
			hmap[i * 2 + 1] = i;
		}
	}

	qsort(hmap, num_columns, sizeof(int) * 2, rankCompare);

	for (i = 0; i < num_columns; i++)
	{
		piv_columns[hmap[i * 2 + 1]].rank = i;
	}

	pg_free(hmap);
}

/*
 * 查找列引用，可以是：
 * - 从 1 到 PQnfields(res) 的数字
 * - 与 PQfname(res,...) 之一匹配的列名
 *
 * 返回从 0 开始的列号，如果未找到或模糊则返回 -1。
 *
 * 注意：可能修改 "arg" 字符串的内容。
 */
static int
indexOfColumn(char *arg, const PGresult *res)
{
	int			idx;

	if (arg[0] && strspn(arg, "0123456789") == strlen(arg))
	{
		/* 如果 arg 仅包含数字，则它是一个列号 */
		idx = atoi(arg) - 1;
		if (idx < 0 || idx >= PQnfields(res))
		{
			pg_log_error("\\crosstabview: column number %d is out of range 1..%d",
						 idx + 1, PQnfields(res));
			return -1;
		}
	}
	else
	{
		int			i;

		/*
			 * 解引号并将列名转为小写。在此之前检查是否为
			 * 全数字，可以确保即使引号中的名称全是数字，
			 * 也会被当作名称而非数字处理。
		 */
		dequote_downcase_identifier(arg, true, pset.encoding);

		/* 现在在 res 的列名中查找匹配 */
		idx = -1;
		for (i = 0; i < PQnfields(res); i++)
		{
			if (strcmp(arg, PQfname(res, i)) == 0)
			{
				if (idx >= 0)
				{
					/* 已找到另一个同名的 idx */
					pg_log_error("\\crosstabview: ambiguous column name: \"%s\"", arg);
					return -1;
				}
				idx = i;
			}
		}
		if (idx == -1)
		{
			pg_log_error("\\crosstabview: column name not found: \"%s\"", arg);
			return -1;
		}
	}

	return idx;
}

/*
 * 垂直和水平表头的值比较器
 * 仅用于去重。
 * - 空值被视为相等
 * - 非空值 < 空值
 * - 非空值通过 strcmp() 比较
 */
static int
pivotFieldCompare(const void *a, const void *b)
{
	const pivot_field *pa = (const pivot_field *) a;
	const pivot_field *pb = (const pivot_field *) b;

	/* 测试空值 */
	if (!pb->name)
		return pa->name ? -1 : 0;
	else if (!pa->name)
		return 1;

	/* 非空值 */
	return strcmp(pa->name, pb->name);
}

static int
rankCompare(const void *a, const void *b)
{
	return *((const int *) a) - *((const int *) b);
}
