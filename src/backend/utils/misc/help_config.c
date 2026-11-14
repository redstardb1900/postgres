/*-------------------------------------------------------------------------
 * help_config.c
 *
 * 显示大一统配置体系下的可用选项
 *
 * 标志位被置为 GUC_NO_SHOW_ALL、GUC_NOT_IN_SAMPLE
 * 或 GUC_DISALLOW_IN_FILE 的选项不予显示，除非用户
 * 明确按名称请求该变量
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/help_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <unistd.h>

#include "utils/guc_tables.h"
#include "utils/help_config.h"


/*
 * 这个联合类型让我们能够混合组织起来的多种不同
 * 类型的结构体。
 */
typedef union
{
	struct config_generic generic;
	struct config_bool _bool;
	struct config_real real;
	struct config_int integer;
	struct config_string string;
	struct config_enum _enum;
} mixedStruct;


static void printMixedStruct(mixedStruct *structToPrint);
static bool displayStruct(mixedStruct *structToDisplay);


void
GucInfoMain(void)
{
	struct config_generic **guc_vars;
	int			numOpts,
				i;

	/* 初始化 GUC 哈希表 */
	build_guc_variables();

	guc_vars = get_guc_variables(&numOpts);

	for (i = 0; i < numOpts; i++)
	{
		mixedStruct *var = (mixedStruct *) guc_vars[i];

		if (displayStruct(var))
			printMixedStruct(var);
	}

	exit(0);
}


/*
 * 如果传给该函数的结构体应当向用户显示，
 * 则此函数返回 true。
 */
static bool
displayStruct(mixedStruct *structToDisplay)
{
	return !(structToDisplay->generic.flags & (GUC_NO_SHOW_ALL |
											   GUC_NOT_IN_SAMPLE |
											   GUC_DISALLOW_IN_FILE));
}


/*
 * 此函数打印传给它的通用结构体。它会根据用户想
 * 看的内容以不同的格式打印。
 */
static void
printMixedStruct(mixedStruct *structToPrint)
{
	printf("%s\t%s\t%s\t",
		   structToPrint->generic.name,
		   GucContext_Names[structToPrint->generic.context],
		   _(config_group_names[structToPrint->generic.group]));

	switch (structToPrint->generic.vartype)
	{

		case PGC_BOOL:
			printf("BOOLEAN\t%s\t\t\t",
				   (structToPrint->_bool.reset_val == 0) ?
				   "FALSE" : "TRUE");
			break;

		case PGC_INT:
			printf("INTEGER\t%d\t%d\t%d\t",
				   structToPrint->integer.reset_val,
				   structToPrint->integer.min,
				   structToPrint->integer.max);
			break;

		case PGC_REAL:
			printf("REAL\t%g\t%g\t%g\t",
				   structToPrint->real.reset_val,
				   structToPrint->real.min,
				   structToPrint->real.max);
			break;

		case PGC_STRING:
			printf("STRING\t%s\t\t\t",
				   structToPrint->string.boot_val ? structToPrint->string.boot_val : "");
			break;

		case PGC_ENUM:
			printf("ENUM\t%s\t\t\t",
				   config_enum_lookup_by_value(&structToPrint->_enum,
											   structToPrint->_enum.boot_val));
			break;

		default:
			write_stderr("internal error: unrecognized run-time parameter type\n");
			break;
	}

	printf("%s\t%s\n",
		   (structToPrint->generic.short_desc == NULL) ? "" : _(structToPrint->generic.short_desc),
		   (structToPrint->generic.long_desc == NULL) ? "" : _(structToPrint->generic.long_desc));
}
