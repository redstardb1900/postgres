/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/bin/psql/variables.c
 */
#include "postgres_fe.h"

#include "common.h"
#include "common/logging.h"
#include "variables.h"

/*
 * 检查变量名是否允许。
 *
 * 我们允许任何非 ASCII 字符，以及 ASCII 字母、数字和下划线。
 * 请与 psqlscan.l 和 psqlscanslash.l 中的 variable_char 定义
 * 保持同步。
 */
static bool
valid_variable_name(const char *name)
{
	const unsigned char *ptr = (const unsigned char *) name;

	/* 不能为零长度 */
	if (*ptr == '\0')
		return false;

	while (*ptr)
	{
		if (IS_HIGHBIT_SET(*ptr) ||
			strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
				   "_0123456789", *ptr) != NULL)
			ptr++;
		else
			return false;
	}

	return true;
}

/*
 * "变量空间"由一个原本未使用的 struct _variable 表示，
 * 该结构体用作列表头。
 *
 * 列表条目按名称顺序（根据 strcmp）排列。这主要是为了让
 * PrintVariables() 的输出更加美观。
 */
VariableSpace
CreateVariableSpace(void)
{
	struct _variable *ptr;

	ptr = pg_malloc(sizeof *ptr);
	ptr->name = NULL;
	ptr->value = NULL;
	ptr->substitute_hook = NULL;
	ptr->assign_hook = NULL;
	ptr->next = NULL;

	return ptr;
}

/*
 * 获取变量的字符串值，如果未定义则返回 NULL。
 *
 * 注意：结果在下一次变量赋值之前有效。
 */
const char *
GetVariable(VariableSpace space, const char *name)
{
	struct _variable *current;

	if (!space)
		return NULL;

	for (current = space->next; current; current = current->next)
	{
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
		{
			/* 当值为 NULL 时，这也是正确答案 */
			return current->value;
		}
		if (cmp > 0)
			break;				/* 不存在 */
	}

	return NULL;
}

/*
 * 尝试将 "value" 解释为布尔值，如果成功则将其存储到 *result 中。
 * 否则不要覆盖 *result。
 *
 * 有效值为：true、false、yes、no、on、off、1、0；以及它们的
 * 唯一前缀。
 *
 * "name" 是我们要赋值的变量名，用于错误报告（如果有的话）。
 * 传入 name == NULL 以抑制错误报告。
 *
 * 当 "value" 语法有效时返回 true，否则返回 false。
 */
bool
ParseVariableBool(const char *value, const char *name, bool *result)
{
	size_t		len;
	bool		valid = true;

	/* 将 "unset" 视为空字符串，这将导致下面的错误 */
	if (value == NULL)
		value = "";

	len = strlen(value);

	if (len > 0 && pg_strncasecmp(value, "true", len) == 0)
		*result = true;
	else if (len > 0 && pg_strncasecmp(value, "false", len) == 0)
		*result = false;
	else if (len > 0 && pg_strncasecmp(value, "yes", len) == 0)
		*result = true;
	else if (len > 0 && pg_strncasecmp(value, "no", len) == 0)
		*result = false;
	/* 'o' 不够唯一 */
	else if (pg_strncasecmp(value, "on", (len > 2 ? len : 2)) == 0)
		*result = true;
	else if (pg_strncasecmp(value, "off", (len > 2 ? len : 2)) == 0)
		*result = false;
	else if (pg_strcasecmp(value, "1") == 0)
		*result = true;
	else if (pg_strcasecmp(value, "0") == 0)
		*result = false;
	else
	{
		/* 字符串无法识别；不要覆盖 *result */
		if (name)
			pg_log_error("unrecognized value \"%s\" for \"%s\": Boolean expected",
						 value, name);
		valid = false;
	}
	return valid;
}

/*
 * 尝试将 "value" 解释为整数值，如果成功则将其存储到 *result 中。
 * 否则不要覆盖 *result。
 *
 * "name" 是我们要赋值的变量名，用于错误报告（如果有的话）。
 * 传入 name == NULL 以抑制错误报告。
 *
 * 当 "value" 语法有效时返回 true，否则返回 false。
 */
bool
ParseVariableNum(const char *value, const char *name, int *result)
{
	char	   *end;
	long		numval;

	/* 将 "unset" 视为空字符串，这将导致下面的错误 */
	if (value == NULL)
		value = "";

	errno = 0;
	numval = strtol(value, &end, 0);
	if (errno == 0 && *end == '\0' && end != value && numval == (int) numval)
	{
		*result = (int) numval;
		return true;
	}
	else
	{
		/* 字符串无法识别；不要覆盖 *result */
		if (name)
			pg_log_error("invalid value \"%s\" for \"%s\": integer expected",
						 value, name);
		return false;
	}
}

/*
 * 打印所有变量的值。
 */
void
PrintVariables(VariableSpace space)
{
	struct _variable *ptr;

	if (!space)
		return;

	for (ptr = space->next; ptr; ptr = ptr->next)
	{
		if (ptr->value)
			printf("%s = '%s'\n", ptr->name, ptr->value);
		if (cancel_pressed)
			break;
	}
}

/*
 * 将名为 "name" 的变量设置为值 "value"，
 * 如果 "value" 为 NULL 则删除它。
 *
 * 成功时返回 true，失败时返回 false；在后一种情况下会打印
 * 适当的错误消息，除非 space 或 name 为 NULL。
 *
 */
bool
SetVariable(VariableSpace space, const char *name, const char *value)
{
	struct _variable *current,
			   *previous;

	if (!space || !name)
		return false;

	if (!valid_variable_name(name))
	{
		/* 删除不存在的变量不是错误 */
		if (!value)
			return true;
		pg_log_error("invalid variable name: \"%s\"", name);
		return false;
	}

	for (previous = space, current = space->next;
		 current;
		 previous = current, current = current->next)
	{
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
		{
			/*
			 * 找到条目，因此更新，除非 assign hook 返回 false。
			 *
			 * 我们必须先复制传入的值。这简化了 substitute hook
			 * 的 API。此外，某些 assign hook 假设传入的值与变量
			 * 具有相同的生命周期。在失败时需要再次释放字符串，
			 * 这是保持这些 API 简洁的小代价。
			 *
			 */
			char	   *new_value = value ? pg_strdup(value) : NULL;
			bool		confirmed;

			if (current->substitute_hook)
				new_value = current->substitute_hook(new_value);

			if (current->assign_hook)
				confirmed = current->assign_hook(new_value);
			else
				confirmed = true;

			if (confirmed)
			{
				pg_free(current->value);
				current->value = new_value;

				/*
				 * 如果我们删除了值，并且没有钩子需要记住，
				 * 我们可以完全丢弃该变量。
				 */
				if (new_value == NULL &&
					current->substitute_hook == NULL &&
					current->assign_hook == NULL)
				{
					previous->next = current->next;
					free(current->name);
					free(current);
				}
			}
			else
				pg_free(new_value); /* current->value 保持不变 */

			return confirmed;
		}
		if (cmp > 0)
			break;				/* it's not there */
	}

	/* 不存在，创建新条目……除非我们被要求删除 */
	if (value)
	{
		current = pg_malloc(sizeof *current);
		current->name = pg_strdup(name);
		current->value = pg_strdup(value);
		current->substitute_hook = NULL;
		current->assign_hook = NULL;
		current->next = previous->next;
		previous->next = current;
	}
	return true;
}

/*
 * 将 substitute 和/或 assign hook 函数附加到指定的变量。
 * 如果只需要一个 hook，则为另一个传入 NULL。
 *
 * 如果变量尚不存在，则以 NULL 值创建它，只是为了有一个
 * 地方存储 hook 函数。（substitute hook 可能会立即将 NULL
 * 更改为其他值；如果不更改，则此状态在外部看来与变量
 * 未定义相同。）
 *
 * 如果提供了 substitute hook，则立即对变量的值调用它。
 * 然后如果提供了 assign hook，则对变量的值调用它。
 * 这旨在让它更新任何派生的 psql 状态。如果 assign hook
 * 不喜欢当前值，它会打印相应的消息，但我们会忽略它。
 * 通常我们不期望在此处发生此类失败，因为这应该在分配
 * 任何用户提供的值之前调用。
 */
void
SetVariableHooks(VariableSpace space, const char *name,
				 VariableSubstituteHook shook,
				 VariableAssignHook ahook)
{
	struct _variable *current,
			   *previous;

	if (!space || !name)
		return;

	if (!valid_variable_name(name))
		return;

	for (previous = space, current = space->next;
		 current;
		 previous = current, current = current->next)
	{
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
		{
			/* 找到条目，更新 */
			current->substitute_hook = shook;
			current->assign_hook = ahook;
			if (shook)
				current->value = (*shook) (current->value);
			if (ahook)
				(void) (*ahook) (current->value);
			return;
		}
		if (cmp > 0)
			break;				/* it's not there */
	}

	/* 不存在，创建新条目 */
	current = pg_malloc(sizeof *current);
	current->name = pg_strdup(name);
	current->value = NULL;
	current->substitute_hook = shook;
	current->assign_hook = ahook;
	current->next = previous->next;
	previous->next = current;
	if (shook)
		current->value = (*shook) (current->value);
	if (ahook)
		(void) (*ahook) (current->value);
}

/*
 * 如果指定变量具有 substitute 和/或 assign hook 函数，
 * 则返回 true。
 */
bool
VariableHasHook(VariableSpace space, const char *name)
{
	struct _variable *current;

	Assert(space);
	Assert(name);

	for (current = space->next; current; current = current->next)
	{
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
			return (current->substitute_hook != NULL ||
					current->assign_hook != NULL);
		if (cmp > 0)
			break;				/* it's not there */
	}

	return false;
}

/*
 * 便捷函数，将变量的值设置为 "on"。
 */
bool
SetVariableBool(VariableSpace space, const char *name)
{
	return SetVariable(space, name, "on");
}

/*
 * 尝试删除变量。
 *
 * 如果不成功，打印消息并返回 "false"。
 * 删除不存在的变量不是错误。
 */
bool
DeleteVariable(VariableSpace space, const char *name)
{
	return SetVariable(space, name, NULL);
}

/*
 * 为接受枚举风格参数的变量或命令发送带有建议的错误。
 * 此函数仅用于标准化措辞。
 * suggestions 应遵循格式 "fee, fi, fo, fum"。
 *
 */
void
PsqlVarEnumError(const char *name, const char *value, const char *suggestions)
{
	pg_log_error("unrecognized value \"%s\" for \"%s\"\n"
				 "Available values are: %s.",
				 value, name, suggestions);
}
