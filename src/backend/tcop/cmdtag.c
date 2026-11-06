/*-------------------------------------------------------------------------
 *
 * cmdtag.c
 *	  命令标签名称与枚举的数据和例程。       
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/tcop/cmdtag.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "tcop/cmdtag.h"
#include "utils/builtins.h"


typedef struct CommandTagBehavior
{
	const char *name;			/* 标签名，如 "SELECT"*/
	const uint8 namelen;		/* 设为 strlen(name) */
	const bool	event_trigger_ok;
	const bool	table_rewrite_ok;
	const bool	display_rowcount;	/* should the number of rows affected be
									 * shown in the command completion string */
} CommandTagBehavior;

#define PG_CMDTAG(tag, name, evtrgok, rwrok, rowcnt) \
	{ name, (uint8) (sizeof(name) - 1), evtrgok, rwrok, rowcnt },

static const CommandTagBehavior tag_behavior[COMMAND_TAG_NEXTTAG] = {
#include "tcop/cmdtaglist.h"
};

#undef PG_CMDTAG

void
InitializeQueryCompletion(QueryCompletion *qc)
{
	qc->commandTag = CMDTAG_UNKNOWN;
	qc->nprocessed = 0;
}

const char *
GetCommandTagName(CommandTag commandTag)
{
	return tag_behavior[commandTag].name;
}

const char *
GetCommandTagNameAndLen(CommandTag commandTag, Size *len)
{
	*len = (Size) tag_behavior[commandTag].namelen;
	return tag_behavior[commandTag].name;
}

bool
command_tag_display_rowcount(CommandTag commandTag)
{
	return tag_behavior[commandTag].display_rowcount;
}

bool
command_tag_event_trigger_ok(CommandTag commandTag)
{
	return tag_behavior[commandTag].event_trigger_ok;
}

bool
command_tag_table_rewrite_ok(CommandTag commandTag)
{
	return tag_behavior[commandTag].table_rewrite_ok;
}

/*
 * 按名称查 CommandTag  
 *
 * 返回 CommandTag，未知则为 CMDTAG_UNKNOWN        
 */
CommandTag
GetCommandTagEnum(const char *commandname)
{
	const CommandTagBehavior *base,
			   *last,
			   *position;
	int			result;

	if (commandname == NULL || *commandname == '\0')
		return CMDTAG_UNKNOWN;

	base = tag_behavior;
	last = tag_behavior + lengthof(tag_behavior) - 1;
	while (last >= base)
	{
		position = base + ((last - base) >> 1);
		result = pg_strcasecmp(commandname, position->name);
		if (result == 0)
			return (CommandTag) (position - tag_behavior);
		else if (result < 0)
			last = position - 1;
		else
			base = position + 1;
	}
	return CMDTAG_UNKNOWN;
}

/*
 * BuildQueryCompletionString
 *		构造一个字符串，含命令标签名；若标签 
 *		设置了 display_rowcount，还含 QueryCompletion 的            
 *		nprocessed。返回所构造字符串的 strlen。 
 *
 * 调用者必须保证 buff 至少有 COMPLETION_TAG_BUFSIZE 字节。     
 *
 * 若 nameonly 为真，构造出的字符串将只含标签名。          
 *      
 */
Size
BuildQueryCompletionString(char *buff, const QueryCompletion *qc,
						   bool nameonly)
{
	CommandTag	tag = qc->commandTag;
	Size		taglen;
	const char *tagname = GetCommandTagNameAndLen(tag, &taglen);
	char	   *bufp;

	/*
	 * 我们假定标签名是纯 ASCII，因此不需要编码转换。     
	 *            
	 */
	memcpy(buff, tagname, taglen);
	bufp = buff + taglen;

	/* 确保标签名不会长到溢出缓冲区                      */
	Assert(taglen <= COMPLETION_TAG_BUFSIZE - MAXINT8LEN - 4);

	/*
	 * PostgreSQL 11 及更早版本还能创建带 OIDS 的表；       
	 * 向这样的表插入时，INSERT 曾把新记录的 Oid 放进   
	 * 完成标签里。为了线上协议保持兼容，现在        
	 * 我们在曾写新记录 Oid 的位置写 "0"（表示              
	 * InvalidOid）。                                                     
	 */
	if (command_tag_display_rowcount(tag) && !nameonly)
	{
		if (tag == CMDTAG_INSERT)
		{
			*bufp++ = ' ';
			*bufp++ = '0';
		}
		*bufp++ = ' ';
		bufp += pg_ulltoa_n(qc->nprocessed, bufp);
	}

	/* 最后为字符串补 NUL 终止符   */
	*bufp = '\0';

	Assert((bufp - buff) == strlen(buff));

	return bufp - buff;
}
