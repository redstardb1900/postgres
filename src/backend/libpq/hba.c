/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  Routines to handle host based authentication (that's the scheme
 *	  wherein you authenticate a user by seeing what IP address the system
 *	  says he comes from and choosing authentication method based on it).
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/hba.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "common/string.h"
#include "funcapi.h"
#include "libpq/ifaddr.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "regex/regex.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/conffiles.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

#ifdef USE_LDAP
#ifdef WIN32
#include <winldap.h>
#else
#include <ldap.h>
#endif
#endif


/* check_network_callback 的回调数据 */
typedef struct check_network_data
{
	IPCompareMethod method;		/* 测试方法 */
	SockAddr   *raddr;			/* 客户端的实际地址 */
	bool		result;			/* 若匹配则设为 true */
} check_network_data;

typedef struct
{
	const char *filename;
	int			linenum;
} tokenize_error_callback_arg;

#define token_has_regexp(t)	(t->regex != NULL)
#define token_is_member_check(t)	(!t->quoted && t->string[0] == '+')
#define token_is_keyword(t, k)	(!t->quoted && strcmp(t->string, k) == 0)
#define token_matches(t, k)  (strcmp(t->string, k) == 0)
#define token_matches_insensitive(t,k) (pg_strcasecmp(t->string, k) == 0)

/*
 * 解析 HBA 或 ident 配置文件时，存放 TokenizedAuthLines 列表的
 * 内存上下文。打开第一个文件时创建它
 * （深度为 CONF_FILE_START_DEPTH）。
 */
static MemoryContext tokenize_context = NULL;

/*
 * 预先解析的 HBA 配置文件内容：HbaLine 结构体列表。
 * parsed_hba_context 是它所属的内存上下文。
 */
static List *parsed_hba_lines = NIL;
static MemoryContext parsed_hba_context = NULL;

/*
 * 预先解析的 ident 映射文件内容：IdentLine 结构体列表。
 * parsed_ident_context 是它所属的内存上下文。
 */
static List *parsed_ident_lines = NIL;
static MemoryContext parsed_ident_context = NULL;

/*
 * 下面的字符数组表示 PostgreSQL 所支持的认证
 * 方法的名称。
 *
 * 注意：请与 hba.h 中的 UserAuth 枚举保持同步。
 */
static const char *const UserAuthName[] =
{
	"reject",
	"implicit reject",			/* 不是用户可见的选项 */
	"trust",
	"ident",
	"password",
	"md5",
	"scram-sha-256",
	"gss",
	"sspi",
	"pam",
	"bsd",
	"ldap",
	"cert",
	"radius",
	"peer"
};

/*
 * 确保 UserAuthName[] 跟上 UserAuth 枚举的增补
 */
StaticAssertDecl(lengthof(UserAuthName) == USER_AUTH_LAST + 1,
				 "UserAuthName[] must match the UserAuth enum");


static List *tokenize_expand_file(List *tokens, const char *outer_filename,
								  const char *inc_filename, int elevel,
								  int depth, char **err_msg);
static bool parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
							   int elevel, char **err_msg);
static int	regcomp_auth_token(AuthToken *token, char *filename, int line_num,
							   char **err_msg, int elevel);
static int	regexec_auth_token(const char *match, AuthToken *token,
							   size_t nmatch, regmatch_t pmatch[]);
static void tokenize_error_callback(void *arg);


/*
 * isblank() 已见于 ISO C99 规范，但可移植性还不够，
 * 因此提供我们自己的版本。
 */
bool
pg_isblank(const char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}


/*
 * 从 *lineptr 所指的字符串中取出一个词元。
 *
 * 词元是由空白字符界定的非空白字符组成的字符串，
 * 界线还有逗号、行首和行尾。空白指空格或制表符。
 *
 * 词元可以用双引号界定（这样就能包含
 * 逗号、空白和 '#'，换行除外）。与 SQL 一样，写两个
 * 双引号表示一个双引号。
 *
 * 以未加引号的 '#' 开头的注释会被跳过，也就是该行
 * 剩余部分被忽略。
 *
 * （注意，续行处理发生在词法分析之前。
 * 因此，若续行出现在带引号文本或注释内部，
 * 则该带引号文本或注释被视为延续到下一行。）
 *
 * 若有词元，就放进 buf（替换先前的
 * 内容），并把 *lineptr 移过该词元。
 *
 * 此外，我们设置 *initial_quote，表示第一个字符之前是否有引号。
 * （我们用它来防止把 "@x" 当成
 * 文件包含请求。注意 @"x" 应当被当成包含请求；
 * 我们希望允许这样做，以支持文件路径中的嵌入空格。）
 *
 * 我们设置 *terminating_comma，表示词元是否以
 * 逗号结束（该逗号既不返回，也不跳过）。
 *
 * 唯一可能的错误是缺少结束引号，但我们
 * 目前并不检测这种情况，只是返回该行的剩余部分。
 *
 * 若成功：把去掉引号的词元存入 buf 并返回 true。
 * 若该行没有更多词元：把 buf 置空并返回 false。
 */
static bool
next_token(char **lineptr, StringInfo buf,
		   bool *initial_quote, bool *terminating_comma)
{
	int			c;
	bool		in_quote = false;
	bool		was_quote = false;
	bool		saw_quote = false;

	/* 初始化输出参数 */
	resetStringInfo(buf);
	*initial_quote = false;
	*terminating_comma = false;

	/* 跳过下一个词元之前的任何空白和逗号 */
	while ((c = (*(*lineptr)++)) != '\0' && (pg_isblank(c) || c == ','))
		;

	/*
	 * 把接下来直到行尾、未加引号的逗号或
	 * 未加引号的空白之前的字符组成词元，放进 buf。
	 */
	while (c != '\0' &&
		   (!pg_isblank(c) || in_quote))
	{
		/* 跳过注释直到行尾 */
		if (c == '#' && !in_quote)
		{
			while ((c = (*(*lineptr)++)) != '\0')
				;
			break;
		}

		/* 词元中不返回结尾的逗号 */
		if (c == ',' && !in_quote)
		{
			*terminating_comma = true;
			break;
		}

		if (c != '"' || was_quote)
			appendStringInfoChar(buf, c);

		/* 字面双引号由两个双引号表示 */
		if (in_quote && c == '"')
			was_quote = !was_quote;
		else
			was_quote = false;

		if (c == '"')
		{
			in_quote = !in_quote;
			saw_quote = true;
			if (buf->len == 0)
				*initial_quote = true;
		}

		c = *(*lineptr)++;
	}

	/*
	 * 把词元正后方的字符退回去（若它是 '\0' 则至关重要，
	 * 否则下一次调用会读过字符串末尾）。
	 */
	(*lineptr)--;

	return (saw_quote || buf->len > 0);
}

/*
 * 构造一个 palloc 出来的 AuthToken 结构体，并复制给定字符串。
 */
static AuthToken *
make_auth_token(const char *token, bool quoted)
{
	AuthToken  *authtoken;
	int			toklen;

	toklen = strlen(token);
	/* 我们把字符串复制到与结构体相同的 palloc 块中 */
	authtoken = (AuthToken *) palloc0(sizeof(AuthToken) + toklen + 1);
	authtoken->string = (char *) authtoken + sizeof(AuthToken);
	authtoken->quoted = quoted;
	authtoken->regex = NULL;
	memcpy(authtoken->string, token, toklen + 1);

	return authtoken;
}

/*
 * 释放 AuthToken。它可能包含需要
 * 显式清理的正则表达式。
 */
static void
free_auth_token(AuthToken *token)
{
	if (token_has_regexp(token))
		pg_regfree(token->regex);
}

/*
 * 把 AuthToken 结构体复制到新 palloc 出来的内存中。
 */
static AuthToken *
copy_auth_token(AuthToken *in)
{
	AuthToken  *out = make_auth_token(in->string, in->quoted);

	return out;
}

/*
 * 编译正则表达式，并存入输入中给定的 AuthToken。
 * 返回 pg_regcomp() 的结果。出错时，细节
 * 存在 "err_msg" 中。
 */
static int
regcomp_auth_token(AuthToken *token, char *filename, int line_num,
				   char **err_msg, int elevel)
{
	pg_wchar   *wstr;
	int			wlen;
	int			rc;

	Assert(token->regex == NULL);

	if (token->string[0] != '/')
		return 0;				/* 没有什么可编译的 */

	token->regex = (regex_t *) palloc0(sizeof(regex_t));
	wstr = palloc((strlen(token->string + 1) + 1) * sizeof(pg_wchar));
	wlen = pg_mb2wchar_with_len(token->string + 1,
								wstr, strlen(token->string + 1));

	rc = pg_regcomp(token->regex, wstr, wlen, REG_ADVANCED, C_COLLATION_OID);

	if (rc)
	{
		char		errstr[100];

		pg_regerror(rc, token->regex, errstr, sizeof(errstr));
		ereport(elevel,
				(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
				 errmsg("invalid regular expression \"%s\": %s",
						token->string + 1, errstr),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, filename)));

		*err_msg = psprintf("invalid regular expression \"%s\": %s",
							token->string + 1, errstr);
	}

	pfree(wstr);
	return rc;
}

/*
 * 执行 AuthToken 中已计算好的正则表达式，检查是否与
 * "match" 中指定的字符串匹配。调用方可以可选地给出一个
 * 数组来存放匹配。返回 pg_regexec() 的结果。
 */
static int
regexec_auth_token(const char *match, AuthToken *token, size_t nmatch,
				   regmatch_t pmatch[])
{
	pg_wchar   *wmatchstr;
	int			wmatchlen;
	int			r;

	Assert(token->string[0] == '/' && token->regex);

	wmatchstr = palloc((strlen(match) + 1) * sizeof(pg_wchar));
	wmatchlen = pg_mb2wchar_with_len(match, wmatchstr, strlen(match));

	r = pg_regexec(token->regex, wmatchstr, wmatchlen, 0, NULL, nmatch, pmatch, 0);

	pfree(wmatchstr);
	return r;
}

/*
 * 从一行中词法分析一个 HBA 字段，处理文件包含和逗号列表。
 *
 * filename：当前文件的路径名（解析相对路径时需要）
 * *lineptr：当前行指针，会移过该字段
 *
 * 若出错，以 ereport 级别 elevel 记一条消息，并
 * 把 *err_msg 设为描述该错误的字符串。注意结果
 * 无论如何都可能不是 NIL，因此必须检查 *err_msg 才能确定
 * 是否出错。
 *
 * 结果是 AuthToken 结构体的 List，该字段中每个词元一个，
 * 若到达行尾则为 NIL。
 */
static List *
next_field_expand(const char *filename, char **lineptr,
				  int elevel, int depth, char **err_msg)
{
	StringInfoData buf;
	bool		trailing_comma;
	bool		initial_quote;
	List	   *tokens = NIL;

	initStringInfo(&buf);

	do
	{
		if (!next_token(lineptr, &buf,
						&initial_quote, &trailing_comma))
			break;

		/* 这是在引用一个文件吗？ */
		if (!initial_quote && buf.len > 1 && buf.data[0] == '@')
			tokens = tokenize_expand_file(tokens, filename, buf.data + 1,
										  elevel, depth + 1, err_msg);
		else
		{
			MemoryContext oldcxt;

			/*
			 * lappend() 可能自己做分配，因此切换到
			 * 词元列表所用的上下文。
			 */
			oldcxt = MemoryContextSwitchTo(tokenize_context);
			tokens = lappend(tokens, make_auth_token(buf.data, initial_quote));
			MemoryContextSwitchTo(oldcxt);
		}
	} while (trailing_comma && (*err_msg == NULL));

	pfree(buf.data);

	return tokens;
}

/*
 * tokenize_include_file
 *		把另一个文件包含进 hba "字段"。
 *
 * 打开并词法分析从另一个认证文件包含进来的文件，
 * 包含记录是 "include"、"include_if_exists" 或
 * "include_dir" 之一，并把找到的全部值赋给已有的
 * AuthTokens 列表的列表。
 *
 * 所有新词元都分配在专用于词法分析的内存上下文中，
 * 即 tokenize_context。
 *
 * 若 missing_ok 为 true，则忽略缺少的文件。
 *
 * 若出错，以 ereport 级别 elevel 记一条消息，并
 * 把 *err_msg 设为描述该错误的字符串。注意结果
 * 无论如何都可能不是 NIL，因此必须检查 *err_msg 才能确定
 * 是否出错。
 */
static void
tokenize_include_file(const char *outer_filename,
					  const char *inc_filename,
					  List **tok_lines,
					  int elevel,
					  int depth,
					  bool missing_ok,
					  char **err_msg)
{
	char	   *inc_fullname;
	FILE	   *inc_file;

	inc_fullname = AbsoluteConfigLocation(inc_filename, outer_filename);
	inc_file = open_auth_file(inc_fullname, elevel, depth, err_msg);

	if (!inc_file)
	{
		if (errno == ENOENT && missing_ok)
		{
			ereport(elevel,
					(errmsg("skipping missing authentication file \"%s\"",
							inc_fullname)));
			*err_msg = NULL;
			pfree(inc_fullname);
			return;
		}

		/* 错误在 err_msg 中，因此离开并报告 */
		pfree(inc_fullname);
		Assert(err_msg);
		return;
	}

	tokenize_auth_file(inc_fullname, inc_file, tok_lines, elevel,
					   depth);
	free_auth_file(inc_file, depth);
	pfree(inc_fullname);
}

/*
 * tokenize_expand_file
 *		把从另一个文件包含进来的文件展开进 hba "字段"
 *
 * 打开并词法分析用 @ 从另一个 HBA 配置文件包含进来的文件，
 * 并把其中找到的全部值作为扁平的 AuthTokens 列表返回。若发现
 * @ 词元或 include 记录，就递归展开它。新读到的
 * 词元追加到 "tokens"（这样 foo,bar,@baz 才会符合预期）。
 * 所有新词元都分配在专用于
 * TokenizedAuthLines 列表的内存上下文中，即 tokenize_context。
 *
 * 若出错，以 ereport 级别 elevel 记一条消息，并
 * 把 *err_msg 设为描述该错误的字符串。注意结果
 * 无论如何都可能不是 NIL，因此必须检查 *err_msg 才能确定
 * 是否出错。
 */
static List *
tokenize_expand_file(List *tokens,
					 const char *outer_filename,
					 const char *inc_filename,
					 int elevel,
					 int depth,
					 char **err_msg)
{
	char	   *inc_fullname;
	FILE	   *inc_file;
	List	   *inc_lines = NIL;
	ListCell   *inc_line;

	inc_fullname = AbsoluteConfigLocation(inc_filename, outer_filename);
	inc_file = open_auth_file(inc_fullname, elevel, depth, err_msg);

	if (inc_file == NULL)
	{
		/* 错误已经记入日志 */
		pfree(inc_fullname);
		return tokens;
	}

	/*
	 * 若文件包含 @ 或 include
	 * 记录，这里可能递归。
	 */
	tokenize_auth_file(inc_fullname, inc_file, &inc_lines, elevel,
					   depth);

	pfree(inc_fullname);

	/*
	 * 把文件中找到的全部词元移到词元列表。它们
	 * 已经保存在 tokenize_context 中。
	 */
	foreach(inc_line, inc_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(inc_line);
		ListCell   *inc_field;

		/* 若任一行有错误，就把它向上传给调用方 */
		if (tok_line->err_msg)
		{
			*err_msg = pstrdup(tok_line->err_msg);
			break;
		}

		foreach(inc_field, tok_line->fields)
		{
			List	   *inc_tokens = lfirst(inc_field);
			ListCell   *inc_token;

			foreach(inc_token, inc_tokens)
			{
				AuthToken  *token = lfirst(inc_token);
				MemoryContext oldcxt;

				/*
				 * lappend() 可能自己做分配，因此切换到
				 * 词元列表所用的上下文。
				 */
				oldcxt = MemoryContextSwitchTo(tokenize_context);
				tokens = lappend(tokens, token);
				MemoryContextSwitchTo(oldcxt);
			}
		}
	}

	free_auth_file(inc_file, depth);
	return tokens;
}

/*
 * free_auth_file
 *		释放由 open_auth_file() 打开的文件。
 */
void
free_auth_file(FILE *file, int depth)
{
	FreeFile(file);

	/* 若这是最后一次清理，就去掉词法分析上下文 */
	if (depth == CONF_FILE_START_DEPTH)
	{
		MemoryContextDelete(tokenize_context);
		tokenize_context = NULL;
	}
}

/*
 * open_auth_file
 *		打开给定文件。
 *
 * filename：目标文件的绝对路径
 * elevel：消息日志级别
 * depth：打开文件时的递归层级
 * err_msg：错误的细节
 *
 * 返回值是已打开的文件。出错时返回 NULL，细节
 * 存在 "err_msg" 中。
 */
FILE *
open_auth_file(const char *filename, int elevel, int depth,
			   char **err_msg)
{
	FILE	   *file;

	/*
	 * 拒绝过深的 include 嵌套。这只是一项安全检查，
	 * 避免包含文件循环回到自身时因栈溢出而做核心转储。
	 * 最大嵌套深度相当随意。
	 */
	if (depth > CONF_FILE_MAX_DEPTH)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": maximum nesting depth exceeded",
						filename)));
		if (err_msg)
			*err_msg = psprintf("could not open file \"%s\": maximum nesting depth exceeded",
								filename);
		return NULL;
	}

	file = AllocateFile(filename, "r");
	if (file == NULL)
	{
		int			save_errno = errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						filename)));
		if (err_msg)
			*err_msg = psprintf("could not open file \"%s\": %s",
								filename, strerror(save_errno));
		/* 调用方可能关心某个特定的 errno */
		errno = save_errno;
		return NULL;
	}

	/*
	 * 打开顶层文件时，创建词法分析所用的
	 * 内存上下文。回到这一层清理时，该上下文会随这个
	 * 文件一起关闭。
	 */
	if (depth == CONF_FILE_START_DEPTH)
	{
		/*
		 * 上下文可能还在，但假定它
		 * 已经被去掉了。
		 */
		tokenize_context = AllocSetContextCreate(CurrentMemoryContext,
												 "tokenize_context",
												 ALLOCSET_START_SMALL_SIZES);
	}

	return file;
}

/*
 * tokenize_auth_file() 的错误上下文回调
 */
static void
tokenize_error_callback(void *arg)
{
	tokenize_error_callback_arg *callback_arg = (tokenize_error_callback_arg *) arg;

	errcontext("line %d of configuration file \"%s\"",
			   callback_arg->linenum, callback_arg->filename);
}

/*
 * tokenize_auth_file
 *		对给定文件做词法分析。
 *
 * 输出是 TokenizedAuthLine 结构体的列表；结构体定义见
 * libpq/hba.h。这是负责解析
 * 认证文件的核心部分。本函数的全部操作都在它自己的
 * 局部内存上下文中进行，便于清理这里分配的任何东西。在
 * postmaster 中重新加载认证文件时，这一点很重要。
 *
 * filename：目标文件的绝对路径
 * file：已经打开的目标文件
 * tok_lines：接收输出列表，保存在 tokenize_context 中
 * elevel：消息日志级别
 * depth：词法分析目标文件时的递归层级
 *
 * 错误通过以 ereport 级别 elevel 记日志来报告，并
 * 把含有非空 err_msg 字段的 TokenizedAuthLine 结构体加入
 * 输出列表。
 */
void
tokenize_auth_file(const char *filename, FILE *file, List **tok_lines,
				   int elevel, int depth)
{
	int			line_number = 1;
	StringInfoData buf;
	MemoryContext linecxt;
	MemoryContext funccxt;		/* 本函数调用方的上下文 */
	ErrorContextCallback tokenerrcontext;
	tokenize_error_callback_arg callback_arg;

	Assert(tokenize_context);

	callback_arg.filename = filename;
	callback_arg.linenum = line_number;

	tokenerrcontext.callback = tokenize_error_callback;
	tokenerrcontext.arg = (void *) &callback_arg;
	tokenerrcontext.previous = error_context_stack;
	error_context_stack = &tokenerrcontext;

	/*
	 * 全部局部词法分析都在它自己的上下文中进行，便于清理
	 * 词法分析期间分配的任何内存。
	 */
	linecxt = AllocSetContextCreate(CurrentMemoryContext,
									"tokenize_auth_file",
									ALLOCSET_SMALL_SIZES);
	funccxt = MemoryContextSwitchTo(linecxt);

	initStringInfo(&buf);

	if (depth == CONF_FILE_START_DEPTH)
		*tok_lines = NIL;

	while (!feof(file) && !ferror(file))
	{
		TokenizedAuthLine *tok_line;
		MemoryContext oldcxt;
		char	   *lineptr;
		List	   *current_line = NIL;
		char	   *err_msg = NULL;
		int			last_backslash_buflen = 0;
		int			continuations = 0;

		/* 收集下一行输入，处理反斜杠续行 */
		resetStringInfo(&buf);

		while (pg_get_line_append(file, &buf, NULL))
		{
			/* 去掉结尾的换行，在 Windows 上也包括 \r */
			buf.len = pg_strip_crlf(buf.data);

			/*
			 * 检查反斜杠续行。反斜杠必须在
			 * 上次发现续行的位置之后，否则两个反斜杠
			 * 后面跟着两个 \n 会表现得很意外。
			 */
			if (buf.len > last_backslash_buflen &&
				buf.data[buf.len - 1] == '\\')
			{
				/* 是续行，因此去掉它并继续读 */
				buf.data[--buf.len] = '\0';
				last_backslash_buflen = buf.len;
				continuations++;
				continue;
			}

			/* 不是，因此已经得到整行 */
			break;
		}

		if (ferror(file))
		{
			/* I/O 错误！ */
			int			save_errno = errno;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", filename)));
			err_msg = psprintf("could not read file \"%s\": %s",
							   filename, strerror(save_errno));
			break;
		}

		/* 解析字段 */
		lineptr = buf.data;
		while (*lineptr && err_msg == NULL)
		{
			List	   *current_field;

			current_field = next_field_expand(filename, &lineptr,
											  elevel, depth, &err_msg);
			/* 把字段加入该行，除非已到行尾或注释开头 */
			if (current_field != NIL)
			{
				/*
				 * lappend() 可能自己做分配，因此切换到
				 * 词元列表所用的上下文。
				 */
				oldcxt = MemoryContextSwitchTo(tokenize_context);
				current_line = lappend(current_line, current_field);
				MemoryContextSwitchTo(oldcxt);
			}
		}

		/*
		 * 已到行尾；若这一行没有实质内容，就不必把它送进
		 * TokenizedAuthLine 列表。
		 */
		if (current_line == NIL && err_msg == NULL)
			goto next_line;

		/* 若该行有效，检查它是不是 include 指令 */
		if (err_msg == NULL && list_length(current_line) == 2)
		{
			AuthToken  *first,
					   *second;

			first = linitial(linitial_node(List, current_line));
			second = linitial(lsecond_node(List, current_line));

			if (strcmp(first->string, "include") == 0)
			{
				tokenize_include_file(filename, second->string, tok_lines,
									  elevel, depth + 1, false, &err_msg);

				if (err_msg)
					goto process_line;

				/*
				 * tokenize_auth_file() 已经负责创建了
				 * TokenizedAuthLines。
				 */
				goto next_line;
			}
			else if (strcmp(first->string, "include_dir") == 0)
			{
				char	  **filenames;
				char	   *dir_name = second->string;
				int			num_filenames;
				StringInfoData err_buf;

				filenames = GetConfFilesInDir(dir_name, filename, elevel,
											  &num_filenames, &err_msg);

				if (!filenames)
				{
					/* 错误在 err_msg 中，因此创建一条记录 */
					goto process_line;
				}

				initStringInfo(&err_buf);
				for (int i = 0; i < num_filenames; i++)
				{
					tokenize_include_file(filename, filenames[i], tok_lines,
										  elevel, depth + 1, false, &err_msg);
					/* 若有错误就累积起来 */
					if (err_msg)
					{
						if (err_buf.len > 0)
							appendStringInfoChar(&err_buf, '\n');
						appendStringInfoString(&err_buf, err_msg);
					}
				}

				/* 清理现场 */
				for (int i = 0; i < num_filenames; i++)
					pfree(filenames[i]);
				pfree(filenames);

				/*
				 * 若没有错误，该行已完全处理完毕，
				 * 跳过一般的 TokenizedAuthLine 处理。
				 */
				if (err_buf.len == 0)
					goto next_line;

				/* 否则，处理累积的错误（若有）。 */
				err_msg = err_buf.data;
				goto process_line;
			}
			else if (strcmp(first->string, "include_if_exists") == 0)
			{

				tokenize_include_file(filename, second->string, tok_lines,
									  elevel, depth + 1, true, &err_msg);
				if (err_msg)
					goto process_line;

				/*
				 * tokenize_auth_file() 已经负责创建了
				 * TokenizedAuthLines。
				 */
				goto next_line;
			}
		}

process_line:

		/*
		 * 一般处理：若有错误就报告，并把该行送进
		 * TokenizedAuthLine。它保存在专用于
		 * 这个列表的内存上下文中。
		 */
		oldcxt = MemoryContextSwitchTo(tokenize_context);
		tok_line = (TokenizedAuthLine *) palloc0(sizeof(TokenizedAuthLine));
		tok_line->fields = current_line;
		tok_line->file_name = pstrdup(filename);
		tok_line->line_num = line_number;
		tok_line->raw_line = pstrdup(buf.data);
		tok_line->err_msg = err_msg ? pstrdup(err_msg) : NULL;
		*tok_lines = lappend(*tok_lines, tok_line);
		MemoryContextSwitchTo(oldcxt);

next_line:
		line_number += continuations + 1;
		callback_arg.linenum = line_number;
	}

	MemoryContextSwitchTo(funccxt);
	MemoryContextDelete(linecxt);

	error_context_stack = tokenerrcontext.previous;
}


/*
 * 用户属于该角色吗？
 *
 * userid 是作为尝试登录标识给出的角色的 OID。
 * 我们检查它是否为指定角色名的成员。
 */
static bool
is_member(Oid userid, const char *role)
{
	Oid			roleid;

	if (!OidIsValid(userid))
		return false;			/* 若用户不存在，就回答 "no" */

	roleid = get_role_oid(role, true);

	if (!OidIsValid(roleid))
		return false;			/* 若目标角色不存在，就回答 "no" */

	/*
	 * 看用户是否直接或间接是该角色的成员。为此，
	 * 超级用户不被视为自动成为该角色的成员，
	 * 因此组认证只适用于显式成员身份。
	 */
	return is_member_of_role_nosuper(userid, roleid);
}

/*
 * 检查 AuthToken 列表是否与角色匹配，允许组名。
 *
 * 列出的每个 AuthToken 逐一检查。先处理关键字
 * （这些无法带有正则表达式），然后是正则
 * 表达式（若有）、不区分大小写的匹配（若指定了）以及
 * 精确匹配。
 */
static bool
check_role(const char *role, Oid roleid, List *tokens, bool case_insensitive)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (token_is_member_check(tok))
		{
			if (is_member(roleid, tok->string + 1))
				return true;
		}
		else if (token_is_keyword(tok, "all"))
			return true;
		else if (token_has_regexp(tok))
		{
			if (regexec_auth_token(role, tok, 0, NULL) == REG_OKAY)
				return true;
		}
		else if (case_insensitive)
		{
			if (token_matches_insensitive(tok, role))
				return true;
		}
		else if (token_matches(tok, role))
			return true;
	}
	return false;
}

/*
 * 检查数据库/角色组合是否匹配 AuthToken 列表。
 *
 * 列出的每个 AuthToken 逐一检查。先检查关键字
 * （这些无法带有正则表达式），然后是正则
 * 表达式（若有）以及精确匹配。
 */
static bool
check_db(const char *dbname, const char *role, Oid roleid, List *tokens)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (am_walsender && !am_db_walsender)
		{
			/*
			 * 物理复制的 walsender 连接只能匹配
			 * replication 关键字
			 */
			if (token_is_keyword(tok, "replication"))
				return true;
		}
		else if (token_is_keyword(tok, "all"))
			return true;
		else if (token_is_keyword(tok, "sameuser"))
		{
			if (strcmp(dbname, role) == 0)
				return true;
		}
		else if (token_is_keyword(tok, "samegroup") ||
				 token_is_keyword(tok, "samerole"))
		{
			if (is_member(roleid, dbname))
				return true;
		}
		else if (token_is_keyword(tok, "replication"))
			continue;			/* 若不是 walsender 则永不匹配 */
		else if (token_has_regexp(tok))
		{
			if (regexec_auth_token(dbname, tok, 0, NULL) == REG_OKAY)
				return true;
		}
		else if (token_matches(tok, dbname))
			return true;
	}
	return false;
}

static bool
ipv4eq(struct sockaddr_in *a, struct sockaddr_in *b)
{
	return (a->sin_addr.s_addr == b->sin_addr.s_addr);
}

static bool
ipv6eq(struct sockaddr_in6 *a, struct sockaddr_in6 *b)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (a->sin6_addr.s6_addr[i] != b->sin6_addr.s6_addr[i])
			return false;

	return true;
}

/*
 * 检查主机名是否匹配模式。
 */
static bool
hostname_match(const char *pattern, const char *actual_hostname)
{
	if (pattern[0] == '.')		/* 后缀匹配 */
	{
		size_t		plen = strlen(pattern);
		size_t		hlen = strlen(actual_hostname);

		if (hlen < plen)
			return false;

		return (pg_strcasecmp(pattern, actual_hostname + (hlen - plen)) == 0);
	}
	else
		return (pg_strcasecmp(pattern, actual_hostname) == 0);
}

/*
 * 检查正在连接的 IP 是否匹配给定的主机名。
 */
static bool
check_hostname(hbaPort *port, const char *hostname)
{
	struct addrinfo *gai_result,
			   *gai;
	int			ret;
	bool		found;

	/* 若远程主机名已知有问题，立即返回 */
	if (port->remote_hostname_resolv < 0)
		return false;

	/* 若尚未查找远程主机名，则现在查找 */
	if (!port->remote_hostname)
	{
		char		remote_hostname[NI_MAXHOST];

		ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								 remote_hostname, sizeof(remote_hostname),
								 NULL, 0,
								 NI_NAMEREQD);
		if (ret != 0)
		{
			/* 记住失败；先不要在 postmaster 日志里报错 */
			port->remote_hostname_resolv = -2;
			port->remote_hostname_errcode = ret;
			return false;
		}

		port->remote_hostname = pstrdup(remote_hostname);
	}

	/* 现在看远程主机名是否匹配这一 pg_hba 行 */
	if (!hostname_match(hostname, port->remote_hostname))
		return false;

	/* 若已经验证过正向查找，就结束了 */
	if (port->remote_hostname_resolv == +1)
		return true;

	/* 从主机名查找 IP，并与原始 IP 核对 */
	ret = getaddrinfo(port->remote_hostname, NULL, NULL, &gai_result);
	if (ret != 0)
	{
		/* 记住失败；先不要在 postmaster 日志里报错 */
		port->remote_hostname_resolv = -2;
		port->remote_hostname_errcode = ret;
		return false;
	}

	found = false;
	for (gai = gai_result; gai; gai = gai->ai_next)
	{
		if (gai->ai_addr->sa_family == port->raddr.addr.ss_family)
		{
			if (gai->ai_addr->sa_family == AF_INET)
			{
				if (ipv4eq((struct sockaddr_in *) gai->ai_addr,
						   (struct sockaddr_in *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
			else if (gai->ai_addr->sa_family == AF_INET6)
			{
				if (ipv6eq((struct sockaddr_in6 *) gai->ai_addr,
						   (struct sockaddr_in6 *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
		}
	}

	if (gai_result)
		freeaddrinfo(gai_result);

	if (!found)
		elog(DEBUG2, "pg_hba.conf host name \"%s\" rejected because address resolution did not return a match with IP address of client",
			 hostname);

	port->remote_hostname_resolv = found ? +1 : -1;

	return found;
}

/*
 * 检查正在连接的 IP 是否匹配给定的地址和网络掩码。
 */
static bool
check_ip(SockAddr *raddr, struct sockaddr *addr, struct sockaddr *mask)
{
	if (raddr->addr.ss_family == addr->sa_family &&
		pg_range_sockaddr(&raddr->addr,
						  (struct sockaddr_storage *) addr,
						  (struct sockaddr_storage *) mask))
		return true;
	return false;
}

/*
 * pg_foreach_ifaddr 回调：客户端地址是否匹配本机的这个接口？
 */
static void
check_network_callback(struct sockaddr *addr, struct sockaddr *netmask,
					   void *cb_data)
{
	check_network_data *cn = (check_network_data *) cb_data;
	struct sockaddr_storage mask;

	/* 已经找到匹配了吗？ */
	if (cn->result)
		return;

	if (cn->method == ipCmpSameHost)
	{
		/* 为该地址族生成长度合适的全 1 网络掩码 */
		pg_sockaddr_cidr_mask(&mask, NULL, addr->sa_family);
		cn->result = check_ip(cn->raddr, addr, (struct sockaddr *) &mask);
	}
	else
	{
		/* 使用接口自身的网络掩码 */
		cn->result = check_ip(cn->raddr, addr, netmask);
	}
}

/*
 * 用 pg_foreach_ifaddr 检查 samehost 或 samenet 匹配
 */
static bool
check_same_host_or_net(SockAddr *raddr, IPCompareMethod method)
{
	check_network_data cn;

	cn.method = method;
	cn.raddr = raddr;
	cn.result = false;

	errno = 0;
	if (pg_foreach_ifaddr(check_network_callback, &cn) < 0)
	{
		ereport(LOG,
				(errmsg("error enumerating network interfaces: %m")));
		return false;
	}

	return cn.result;
}


/*
 * 用于检查并报告无效配置选项的宏。
 * 出错时：以级别 elevel 记一条消息，设置 *err_msg，并退出函数。
 * 这些宏并不像看上去那样通用，因为它们知道
 * 调用函数的错误退出值是什么。
 *
 * INVALID_AUTH_OPTION = 当为某个方法指定了
 *						 它所不支持的选项时报告。
 * REQUIRE_AUTH_OPTION = 与 INVALID_AUTH_OPTION 相同，只是它还检查
 *						 该方法是否确实是所指定的那个。当
 *						 该选项只对一种认证方法有效时，用作快捷方式。
 * MANDATORY_AUTH_ARG = 检查某种认证方法是否设置了必需选项，
 *						 若没有就报告错误。
 */
#define INVALID_AUTH_OPTION(optname, validmethods) \
do { \
	ereport(elevel, \
			(errcode(ERRCODE_CONFIG_FILE_ERROR), \
			 /* translator: the second %s is a list of auth methods */ \
			 errmsg("authentication option \"%s\" is only valid for authentication methods %s", \
					optname, _(validmethods)), \
			 errcontext("line %d of configuration file \"%s\"", \
					line_num, file_name))); \
	*err_msg = psprintf("authentication option \"%s\" is only valid for authentication methods %s", \
						optname, validmethods); \
	return false; \
} while (0)

#define REQUIRE_AUTH_OPTION(methodval, optname, validmethods) \
do { \
	if (hbaline->auth_method != methodval) \
		INVALID_AUTH_OPTION(optname, validmethods); \
} while (0)

#define MANDATORY_AUTH_ARG(argvar, argname, authname) \
do { \
	if (argvar == NULL) { \
		ereport(elevel, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("authentication method \"%s\" requires argument \"%s\" to be set", \
						authname, argname), \
				 errcontext("line %d of configuration file \"%s\"", \
						line_num, file_name))); \
		*err_msg = psprintf("authentication method \"%s\" requires argument \"%s\" to be set", \
							authname, argname); \
		return NULL; \
	} \
} while (0)

/*
 * 处理 pg_ident 问题的宏，与上面类似。
 *
 * IDENT_FIELD_ABSENT：
 * 当给定的 ident 字段 ListCell 未被填充时报告。
 *
 * IDENT_MULTI_VALUE：
 * 当给定的 ident 词元 List 含有不止一个元素时报告。
 */
#define IDENT_FIELD_ABSENT(field) \
do { \
	if (!field) { \
		ereport(elevel, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("missing entry at end of line"), \
				 errcontext("line %d of configuration file \"%s\"", \
							line_num, file_name))); \
		*err_msg = pstrdup("missing entry at end of line"); \
		return NULL; \
	} \
} while (0)

#define IDENT_MULTI_VALUE(tokens) \
do { \
	if (tokens->length > 1) { \
		ereport(elevel, \
				(errcode(ERRCODE_CONFIG_FILE_ERROR), \
				 errmsg("multiple values in ident field"), \
				 errcontext("line %d of configuration file \"%s\"", \
							line_num, file_name))); \
		*err_msg = pstrdup("multiple values in ident field"); \
		return NULL; \
	} \
} while (0)


/*
 * 解析 hba 配置文件中已做词法分析的一行，并把结果存入
 * HbaLine 结构体。
 *
 * 若解析失败，以 ereport 级别 elevel 记一条消息，把错误
 * 字符串存入 tok_line->err_msg，并返回 NULL。（某些非错误
 * 情况也可能产生这样的消息。）
 *
 * 注意：出错时本函数会泄漏内存。调用方应当已经
 * 设置一个内存上下文，在本函数返回
 * NULL 时重置该上下文。
 */
HbaLine *
parse_hba_line(TokenizedAuthLine *tok_line, int elevel)
{
	int			line_num = tok_line->line_num;
	char	   *file_name = tok_line->file_name;
	char	  **err_msg = &tok_line->err_msg;
	char	   *str;
	struct addrinfo *gai_result;
	struct addrinfo hints;
	int			ret;
	char	   *cidr_slash;
	char	   *unsupauth;
	ListCell   *field;
	List	   *tokens;
	ListCell   *tokencell;
	AuthToken  *token;
	HbaLine    *parsedline;

	parsedline = palloc0(sizeof(HbaLine));
	parsedline->sourcefile = pstrdup(file_name);
	parsedline->linenumber = line_num;
	parsedline->rawline = pstrdup(tok_line->raw_line);

	/* 检查记录类型。 */
	Assert(tok_line->fields != NIL);
	field = list_head(tok_line->fields);
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multiple values specified for connection type"),
				 errhint("Specify exactly one connection type per line."),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "multiple values specified for connection type";
		return NULL;
	}
	token = linitial(tokens);
	if (strcmp(token->string, "local") == 0)
	{
		parsedline->conntype = ctLocal;
	}
	else if (strcmp(token->string, "host") == 0 ||
			 strcmp(token->string, "hostssl") == 0 ||
			 strcmp(token->string, "hostnossl") == 0 ||
			 strcmp(token->string, "hostgssenc") == 0 ||
			 strcmp(token->string, "hostnogssenc") == 0)
	{

		if (token->string[4] == 's')	/* "hostssl" */
		{
			parsedline->conntype = ctHostSSL;
			/* 若 SSL 支持未启用，记一条警告 */
#ifdef USE_SSL
			if (!EnableSSL)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("hostssl record cannot match because SSL is disabled"),
						 errhint("Set ssl = on in postgresql.conf."),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = "hostssl record cannot match because SSL is disabled";
			}
#else
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostssl record cannot match because SSL is not supported by this build"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "hostssl record cannot match because SSL is not supported by this build";
#endif
		}
		else if (token->string[4] == 'g')	/* "hostgssenc" */
		{
			parsedline->conntype = ctHostGSS;
#ifndef ENABLE_GSS
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("hostgssenc record cannot match because GSSAPI is not supported by this build"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "hostgssenc record cannot match because GSSAPI is not supported by this build";
#endif
		}
		else if (token->string[4] == 'n' && token->string[6] == 's')
			parsedline->conntype = ctHostNoSSL;
		else if (token->string[4] == 'n' && token->string[6] == 'g')
			parsedline->conntype = ctHostNoGSS;
		else
		{
			/* "host" */
			parsedline->conntype = ctHost;
		}
	}							/* 记录类型 */
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid connection type \"%s\"",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = psprintf("invalid connection type \"%s\"", token->string);
		return NULL;
	}

	/* 取得数据库。 */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before database specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "end-of-line before database specification";
		return NULL;
	}
	parsedline->databases = NIL;
	tokens = lfirst(field);
	foreach(tokencell, tokens)
	{
		AuthToken  *tok = copy_auth_token(lfirst(tokencell));

		/* 必要时为数据库词元编译正则表达式 */
		if (regcomp_auth_token(tok, file_name, line_num, err_msg, elevel))
			return NULL;

		parsedline->databases = lappend(parsedline->databases, tok);
	}

	/* 取得角色。 */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before role specification"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "end-of-line before role specification";
		return NULL;
	}
	parsedline->roles = NIL;
	tokens = lfirst(field);
	foreach(tokencell, tokens)
	{
		AuthToken  *tok = copy_auth_token(lfirst(tokencell));

		/* 必要时从角色词元编译正则表达式 */
		if (regcomp_auth_token(tok, file_name, line_num, err_msg, elevel))
			return NULL;

		parsedline->roles = lappend(parsedline->roles, tok);
	}

	if (parsedline->conntype != ctLocal)
	{
		/* 读取 IP 地址字段。（可以带或不带 CIDR 网络掩码） */
		field = lnext(tok_line->fields, field);
		if (!field)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("end-of-line before IP address specification"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "end-of-line before IP address specification";
			return NULL;
		}
		tokens = lfirst(field);
		if (tokens->length > 1)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("multiple values specified for host address"),
					 errhint("Specify one address range per line."),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "multiple values specified for host address";
			return NULL;
		}
		token = linitial(tokens);

		if (token_is_keyword(token, "all"))
		{
			parsedline->ip_cmp_method = ipCmpAll;
		}
		else if (token_is_keyword(token, "samehost"))
		{
			/* 允许本机上的任何 IP 连接 */
			parsedline->ip_cmp_method = ipCmpSameHost;
		}
		else if (token_is_keyword(token, "samenet"))
		{
			/* 允许该主机各子网上的任何 IP 连接 */
			parsedline->ip_cmp_method = ipCmpSameNet;
		}
		else
		{
			/* 指定了 IP 和网络掩码 */
			parsedline->ip_cmp_method = ipCmpMask;

			/* 需要词元的一份可修改副本 */
			str = pstrdup(token->string);

			/* 检查它是否有 CIDR 后缀，若有就把它分离出来 */
			cidr_slash = strchr(str, '/');
			if (cidr_slash)
				*cidr_slash = '\0';

			/* 无论哪种方式都取得 IP 地址 */
			hints.ai_flags = AI_NUMERICHOST;
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = 0;
			hints.ai_protocol = 0;
			hints.ai_addrlen = 0;
			hints.ai_canonname = NULL;
			hints.ai_addr = NULL;
			hints.ai_next = NULL;

			ret = pg_getaddrinfo_all(str, NULL, &hints, &gai_result);
			if (ret == 0 && gai_result)
			{
				memcpy(&parsedline->addr, gai_result->ai_addr,
					   gai_result->ai_addrlen);
				parsedline->addrlen = gai_result->ai_addrlen;
			}
			else if (ret == EAI_NONAME)
				parsedline->hostname = str;
			else
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid IP address \"%s\": %s",
								str, gai_strerror(ret)),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = psprintf("invalid IP address \"%s\": %s",
									str, gai_strerror(ret));
				if (gai_result)
					pg_freeaddrinfo_all(hints.ai_family, gai_result);
				return NULL;
			}

			pg_freeaddrinfo_all(hints.ai_family, gai_result);

			/* 取得网络掩码 */
			if (cidr_slash)
			{
				if (parsedline->hostname)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("specifying both host name and CIDR mask is invalid: \"%s\"",
									token->string),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, file_name)));
					*err_msg = psprintf("specifying both host name and CIDR mask is invalid: \"%s\"",
										token->string);
					return NULL;
				}

				if (pg_sockaddr_cidr_mask(&parsedline->mask, cidr_slash + 1,
										  parsedline->addr.ss_family) < 0)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid CIDR mask in address \"%s\"",
									token->string),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, file_name)));
					*err_msg = psprintf("invalid CIDR mask in address \"%s\"",
										token->string);
					return NULL;
				}
				parsedline->masklen = parsedline->addrlen;
				pfree(str);
			}
			else if (!parsedline->hostname)
			{
				/* 读取掩码字段。 */
				pfree(str);
				field = lnext(tok_line->fields, field);
				if (!field)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("end-of-line before netmask specification"),
							 errhint("Specify an address range in CIDR notation, or provide a separate netmask."),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, file_name)));
					*err_msg = "end-of-line before netmask specification";
					return NULL;
				}
				tokens = lfirst(field);
				if (tokens->length > 1)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("multiple values specified for netmask"),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, file_name)));
					*err_msg = "multiple values specified for netmask";
					return NULL;
				}
				token = linitial(tokens);

				ret = pg_getaddrinfo_all(token->string, NULL,
										 &hints, &gai_result);
				if (ret || !gai_result)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("invalid IP mask \"%s\": %s",
									token->string, gai_strerror(ret)),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, file_name)));
					*err_msg = psprintf("invalid IP mask \"%s\": %s",
										token->string, gai_strerror(ret));
					if (gai_result)
						pg_freeaddrinfo_all(hints.ai_family, gai_result);
					return NULL;
				}

				memcpy(&parsedline->mask, gai_result->ai_addr,
					   gai_result->ai_addrlen);
				parsedline->masklen = gai_result->ai_addrlen;
				pg_freeaddrinfo_all(hints.ai_family, gai_result);

				if (parsedline->addr.ss_family != parsedline->mask.ss_family)
				{
					ereport(elevel,
							(errcode(ERRCODE_CONFIG_FILE_ERROR),
							 errmsg("IP address and mask do not match"),
							 errcontext("line %d of configuration file \"%s\"",
										line_num, file_name)));
					*err_msg = "IP address and mask do not match";
					return NULL;
				}
			}
		}
	}							/* != ctLocal */

	/* 取得认证方法 */
	field = lnext(tok_line->fields, field);
	if (!field)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("end-of-line before authentication method"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "end-of-line before authentication method";
		return NULL;
	}
	tokens = lfirst(field);
	if (tokens->length > 1)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("multiple values specified for authentication type"),
				 errhint("Specify exactly one authentication type per line."),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "multiple values specified for authentication type";
		return NULL;
	}
	token = linitial(tokens);

	unsupauth = NULL;
	if (strcmp(token->string, "trust") == 0)
		parsedline->auth_method = uaTrust;
	else if (strcmp(token->string, "ident") == 0)
		parsedline->auth_method = uaIdent;
	else if (strcmp(token->string, "peer") == 0)
		parsedline->auth_method = uaPeer;
	else if (strcmp(token->string, "password") == 0)
		parsedline->auth_method = uaPassword;
	else if (strcmp(token->string, "gss") == 0)
#ifdef ENABLE_GSS
		parsedline->auth_method = uaGSS;
#else
		unsupauth = "gss";
#endif
	else if (strcmp(token->string, "sspi") == 0)
#ifdef ENABLE_SSPI
		parsedline->auth_method = uaSSPI;
#else
		unsupauth = "sspi";
#endif
	else if (strcmp(token->string, "reject") == 0)
		parsedline->auth_method = uaReject;
	else if (strcmp(token->string, "md5") == 0)
	{
		if (Db_user_namespace)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("MD5 authentication is not supported when \"db_user_namespace\" is enabled"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "MD5 authentication is not supported when \"db_user_namespace\" is enabled";
			return NULL;
		}
		parsedline->auth_method = uaMD5;
	}
	else if (strcmp(token->string, "scram-sha-256") == 0)
		parsedline->auth_method = uaSCRAM;
	else if (strcmp(token->string, "pam") == 0)
#ifdef USE_PAM
		parsedline->auth_method = uaPAM;
#else
		unsupauth = "pam";
#endif
	else if (strcmp(token->string, "bsd") == 0)
#ifdef USE_BSD_AUTH
		parsedline->auth_method = uaBSD;
#else
		unsupauth = "bsd";
#endif
	else if (strcmp(token->string, "ldap") == 0)
#ifdef USE_LDAP
		parsedline->auth_method = uaLDAP;
#else
		unsupauth = "ldap";
#endif
	else if (strcmp(token->string, "cert") == 0)
#ifdef USE_SSL
		parsedline->auth_method = uaCert;
#else
		unsupauth = "cert";
#endif
	else if (strcmp(token->string, "radius") == 0)
		parsedline->auth_method = uaRADIUS;
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\"",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = psprintf("invalid authentication method \"%s\"",
							token->string);
		return NULL;
	}

	if (unsupauth)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("invalid authentication method \"%s\": not supported by this build",
						token->string),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = psprintf("invalid authentication method \"%s\": not supported by this build",
							token->string);
		return NULL;
	}

	/*
	 * XXX：在本地连接上使用 ident 时，把它改成 peer，以求
	 * 向后兼容。
	 */
	if (parsedline->conntype == ctLocal &&
		parsedline->auth_method == uaIdent)
		parsedline->auth_method = uaPeer;

	/* 无效的认证组合 */
	if (parsedline->conntype == ctLocal &&
		parsedline->auth_method == uaGSS)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("gssapi authentication is not supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "gssapi authentication is not supported on local sockets";
		return NULL;
	}

	if (parsedline->conntype != ctLocal &&
		parsedline->auth_method == uaPeer)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("peer authentication is only supported on local sockets"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "peer authentication is only supported on local sockets";
		return NULL;
	}

	/*
	 * SSPI 认证永远无法在 ctLocal 连接上启用，
	 * 因为它只在 Windows 上受支持，而那里不支持 ctLocal。
	 */


	if (parsedline->conntype != ctHostSSL &&
		parsedline->auth_method == uaCert)
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("cert authentication is only supported on hostssl connections"),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = "cert authentication is only supported on hostssl connections";
		return NULL;
	}

	/*
	 * 对 GSS 和 SSPI，把 include_realm 的默认值设为 true。
	 * 在多领域情形下把 include_realm 设为 false 很危险，
	 * 而且通常被认为是不良做法。我们保留这项
	 * 能力是为了向后兼容，但将来某个时候可能想
	 * 去掉它。仍需剥掉领域的用户，更好的办法是
	 * 在 pg_ident.conf 映射里使用合适的
	 * 正则表达式。
	 */
	if (parsedline->auth_method == uaGSS ||
		parsedline->auth_method == uaSSPI)
		parsedline->include_realm = true;

	/*
	 * 对 SSPI，include_realm 默认使用 SAM 兼容域（即
	 * NetBIOS 名）和用户名，而不是 Kerberos 主体名，以求
	 * 兼容。
	 */
	if (parsedline->auth_method == uaSSPI)
	{
		parsedline->compat_realm = true;
		parsedline->upn_username = false;
	}

	/* 解析其余参数 */
	while ((field = lnext(tok_line->fields, field)) != NULL)
	{
		tokens = lfirst(field);
		foreach(tokencell, tokens)
		{
			char	   *val;

			token = lfirst(tokencell);

			str = pstrdup(token->string);
			val = strchr(str, '=');
			if (val == NULL)
			{
				/*
				 * 得到的不是 name=value 对。
				 */
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("authentication option not in name=value format: %s", token->string),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = psprintf("authentication option not in name=value format: %s",
									token->string);
				return NULL;
			}

			*val++ = '\0';		/* str 现在保存着 "name"，val 保存着 "value" */
			if (!parse_hba_auth_opt(str, val, parsedline, elevel, err_msg))
				/* parse_hba_auth_opt 已经把错误消息记入日志 */
				return NULL;
			pfree(str);
		}
	}

	/*
	 * 检查所选认证方法是否有尚未设置的
	 * 必需参数。
	 */
	if (parsedline->auth_method == uaLDAP)
	{
#ifndef HAVE_LDAP_INITIALIZE
		/* 对 OpenLDAP 不是必需的，因为它可以使用 DNS SRV 记录 */
		MANDATORY_AUTH_ARG(parsedline->ldapserver, "ldapserver", "ldap");
#endif

		/*
		 * LDAP 可以按两种模式工作：要么用 ldapprefix 和 ldapsuffix
		 * 做直接绑定，要么用 ldapbasedn、ldapbinddn、ldapbindpasswd
		 * 以及 ldapsearchattribute 或 ldapsearchfilter 之一
		 * 做搜索加绑定。
		 * 不允许混用这些参数。
		 */
		if (parsedline->ldapprefix || parsedline->ldapsuffix)
		{
			if (parsedline->ldapbasedn ||
				parsedline->ldapbinddn ||
				parsedline->ldapbindpasswd ||
				parsedline->ldapsearchattribute ||
				parsedline->ldapsearchfilter)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("cannot use ldapbasedn, ldapbinddn, ldapbindpasswd, ldapsearchattribute, ldapsearchfilter, or ldapurl together with ldapprefix"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = "cannot use ldapbasedn, ldapbinddn, ldapbindpasswd, ldapsearchattribute, ldapsearchfilter, or ldapurl together with ldapprefix";
				return NULL;
			}
		}
		else if (!parsedline->ldapbasedn)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("authentication method \"ldap\" requires argument \"ldapbasedn\", \"ldapprefix\", or \"ldapsuffix\" to be set"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "authentication method \"ldap\" requires argument \"ldapbasedn\", \"ldapprefix\", or \"ldapsuffix\" to be set";
			return NULL;
		}

		/*
		 * 使用搜索加绑定时，可以用一个简单属性
		 * （默认是 "uid"），也可以用完全自定义的搜索过滤器。无法
		 * 两者同时使用。
		 */
		if (parsedline->ldapsearchattribute && parsedline->ldapsearchfilter)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("cannot use ldapsearchattribute together with ldapsearchfilter"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "cannot use ldapsearchattribute together with ldapsearchfilter";
			return NULL;
		}
	}

	if (parsedline->auth_method == uaRADIUS)
	{
		MANDATORY_AUTH_ARG(parsedline->radiusservers, "radiusservers", "radius");
		MANDATORY_AUTH_ARG(parsedline->radiussecrets, "radiussecrets", "radius");

		if (parsedline->radiusservers == NIL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS servers cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "list of RADIUS servers cannot be empty";
			return NULL;
		}

		if (parsedline->radiussecrets == NIL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("list of RADIUS secrets cannot be empty"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "list of RADIUS secrets cannot be empty";
			return NULL;
		}

		/*
		 * 核对选项列表的长度。每个都可以是 0（密钥除外，
		 * 但那在上面已经检查过）、1（各处使用同一个值）
		 * 或与服务器数量相同。
		 */
		if (!(list_length(parsedline->radiussecrets) == 1 ||
			  list_length(parsedline->radiussecrets) == list_length(parsedline->radiusservers)))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("the number of RADIUS secrets (%d) must be 1 or the same as the number of RADIUS servers (%d)",
							list_length(parsedline->radiussecrets),
							list_length(parsedline->radiusservers)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = psprintf("the number of RADIUS secrets (%d) must be 1 or the same as the number of RADIUS servers (%d)",
								list_length(parsedline->radiussecrets),
								list_length(parsedline->radiusservers));
			return NULL;
		}
		if (!(list_length(parsedline->radiusports) == 0 ||
			  list_length(parsedline->radiusports) == 1 ||
			  list_length(parsedline->radiusports) == list_length(parsedline->radiusservers)))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("the number of RADIUS ports (%d) must be 1 or the same as the number of RADIUS servers (%d)",
							list_length(parsedline->radiusports),
							list_length(parsedline->radiusservers)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = psprintf("the number of RADIUS ports (%d) must be 1 or the same as the number of RADIUS servers (%d)",
								list_length(parsedline->radiusports),
								list_length(parsedline->radiusservers));
			return NULL;
		}
		if (!(list_length(parsedline->radiusidentifiers) == 0 ||
			  list_length(parsedline->radiusidentifiers) == 1 ||
			  list_length(parsedline->radiusidentifiers) == list_length(parsedline->radiusservers)))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("the number of RADIUS identifiers (%d) must be 1 or the same as the number of RADIUS servers (%d)",
							list_length(parsedline->radiusidentifiers),
							list_length(parsedline->radiusservers)),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = psprintf("the number of RADIUS identifiers (%d) must be 1 or the same as the number of RADIUS servers (%d)",
								list_length(parsedline->radiusidentifiers),
								list_length(parsedline->radiusservers));
			return NULL;
		}
	}

	/*
	 * 落实由其他设置所蕴含的任何参数。
	 */
	if (parsedline->auth_method == uaCert)
	{
		/*
		 * 对认证方法 cert，客户端证书校验是必需的，
		 * 并且它蕴含 verify-full 这一级别。
		 */
		parsedline->clientcert = clientCertFull;
	}

	return parsedline;
}


/*
 * 把一个名称-值对作为认证选项解析进给定的
 * HbaLine。若成功解析该选项则返回 true，若
 * 遇到错误则返回 false。出错时，还要以
 * ereport 级别 elevel 记一条消息，并把消息字符串存入 *err_msg。
 */
static bool
parse_hba_auth_opt(char *name, char *val, HbaLine *hbaline,
				   int elevel, char **err_msg)
{
	int			line_num = hbaline->linenumber;
	char	   *file_name = hbaline->sourcefile;

#ifdef USE_LDAP
	hbaline->ldapscope = LDAP_SCOPE_SUBTREE;
#endif

	if (strcmp(name, "map") == 0)
	{
		if (hbaline->auth_method != uaIdent &&
			hbaline->auth_method != uaPeer &&
			hbaline->auth_method != uaGSS &&
			hbaline->auth_method != uaSSPI &&
			hbaline->auth_method != uaCert)
			INVALID_AUTH_OPTION("map", gettext_noop("ident, peer, gssapi, sspi, and cert"));
		hbaline->usermap = pstrdup(val);
	}
	else if (strcmp(name, "clientcert") == 0)
	{
		if (hbaline->conntype != ctHostSSL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("clientcert can only be configured for \"hostssl\" rows"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "clientcert can only be configured for \"hostssl\" rows";
			return false;
		}

		if (strcmp(val, "verify-full") == 0)
		{
			hbaline->clientcert = clientCertFull;
		}
		else if (strcmp(val, "verify-ca") == 0)
		{
			if (hbaline->auth_method == uaCert)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("clientcert only accepts \"verify-full\" when using \"cert\" authentication"),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				*err_msg = "clientcert can only be set to \"verify-full\" when using \"cert\" authentication";
				return false;
			}

			hbaline->clientcert = clientCertCA;
		}
		else
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid value for clientcert: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			return false;
		}
	}
	else if (strcmp(name, "clientname") == 0)
	{
		if (hbaline->conntype != ctHostSSL)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("clientname can only be configured for \"hostssl\" rows"),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = "clientname can only be configured for \"hostssl\" rows";
			return false;
		}

		if (strcmp(val, "CN") == 0)
		{
			hbaline->clientcertname = clientCertCN;
		}
		else if (strcmp(val, "DN") == 0)
		{
			hbaline->clientcertname = clientCertDN;
		}
		else
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid value for clientname: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			return false;
		}
	}
	else if (strcmp(name, "pamservice") == 0)
	{
		REQUIRE_AUTH_OPTION(uaPAM, "pamservice", "pam");
		hbaline->pamservice = pstrdup(val);
	}
	else if (strcmp(name, "pam_use_hostname") == 0)
	{
		REQUIRE_AUTH_OPTION(uaPAM, "pam_use_hostname", "pam");
		if (strcmp(val, "1") == 0)
			hbaline->pam_use_hostname = true;
		else
			hbaline->pam_use_hostname = false;
	}
	else if (strcmp(name, "ldapurl") == 0)
	{
#ifdef LDAP_API_FEATURE_X_OPENLDAP
		LDAPURLDesc *urldata;
		int			rc;
#endif

		REQUIRE_AUTH_OPTION(uaLDAP, "ldapurl", "ldap");
#ifdef LDAP_API_FEATURE_X_OPENLDAP
		rc = ldap_url_parse(val, &urldata);
		if (rc != LDAP_SUCCESS)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse LDAP URL \"%s\": %s", val, ldap_err2string(rc))));
			*err_msg = psprintf("could not parse LDAP URL \"%s\": %s",
								val, ldap_err2string(rc));
			return false;
		}

		if (strcmp(urldata->lud_scheme, "ldap") != 0 &&
			strcmp(urldata->lud_scheme, "ldaps") != 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("unsupported LDAP URL scheme: %s", urldata->lud_scheme)));
			*err_msg = psprintf("unsupported LDAP URL scheme: %s",
								urldata->lud_scheme);
			ldap_free_urldesc(urldata);
			return false;
		}

		if (urldata->lud_scheme)
			hbaline->ldapscheme = pstrdup(urldata->lud_scheme);
		if (urldata->lud_host)
			hbaline->ldapserver = pstrdup(urldata->lud_host);
		hbaline->ldapport = urldata->lud_port;
		if (urldata->lud_dn)
			hbaline->ldapbasedn = pstrdup(urldata->lud_dn);

		if (urldata->lud_attrs)
			hbaline->ldapsearchattribute = pstrdup(urldata->lud_attrs[0]);	/* 只使用第一个 */
		hbaline->ldapscope = urldata->lud_scope;
		if (urldata->lud_filter)
			hbaline->ldapsearchfilter = pstrdup(urldata->lud_filter);
		ldap_free_urldesc(urldata);
#else							/* 不是 OpenLDAP */
		ereport(elevel,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("LDAP URLs not supported on this platform")));
		*err_msg = "LDAP URLs not supported on this platform";
#endif							/* 不是 OpenLDAP */
	}
	else if (strcmp(name, "ldaptls") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldaptls", "ldap");
		if (strcmp(val, "1") == 0)
			hbaline->ldaptls = true;
		else
			hbaline->ldaptls = false;
	}
	else if (strcmp(name, "ldapscheme") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapscheme", "ldap");
		if (strcmp(val, "ldap") != 0 && strcmp(val, "ldaps") != 0)
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid ldapscheme value: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
		hbaline->ldapscheme = pstrdup(val);
	}
	else if (strcmp(name, "ldapserver") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapserver", "ldap");
		hbaline->ldapserver = pstrdup(val);
	}
	else if (strcmp(name, "ldapport") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapport", "ldap");
		hbaline->ldapport = atoi(val);
		if (hbaline->ldapport == 0)
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("invalid LDAP port number: \"%s\"", val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = psprintf("invalid LDAP port number: \"%s\"", val);
			return false;
		}
	}
	else if (strcmp(name, "ldapbinddn") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapbinddn", "ldap");
		hbaline->ldapbinddn = pstrdup(val);
	}
	else if (strcmp(name, "ldapbindpasswd") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapbindpasswd", "ldap");
		hbaline->ldapbindpasswd = pstrdup(val);
	}
	else if (strcmp(name, "ldapsearchattribute") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapsearchattribute", "ldap");
		hbaline->ldapsearchattribute = pstrdup(val);
	}
	else if (strcmp(name, "ldapsearchfilter") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapsearchfilter", "ldap");
		hbaline->ldapsearchfilter = pstrdup(val);
	}
	else if (strcmp(name, "ldapbasedn") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapbasedn", "ldap");
		hbaline->ldapbasedn = pstrdup(val);
	}
	else if (strcmp(name, "ldapprefix") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapprefix", "ldap");
		hbaline->ldapprefix = pstrdup(val);
	}
	else if (strcmp(name, "ldapsuffix") == 0)
	{
		REQUIRE_AUTH_OPTION(uaLDAP, "ldapsuffix", "ldap");
		hbaline->ldapsuffix = pstrdup(val);
	}
	else if (strcmp(name, "krb_realm") == 0)
	{
		if (hbaline->auth_method != uaGSS &&
			hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("krb_realm", gettext_noop("gssapi and sspi"));
		hbaline->krb_realm = pstrdup(val);
	}
	else if (strcmp(name, "include_realm") == 0)
	{
		if (hbaline->auth_method != uaGSS &&
			hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("include_realm", gettext_noop("gssapi and sspi"));
		if (strcmp(val, "1") == 0)
			hbaline->include_realm = true;
		else
			hbaline->include_realm = false;
	}
	else if (strcmp(name, "compat_realm") == 0)
	{
		if (hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("compat_realm", gettext_noop("sspi"));
		if (strcmp(val, "1") == 0)
			hbaline->compat_realm = true;
		else
			hbaline->compat_realm = false;
	}
	else if (strcmp(name, "upn_username") == 0)
	{
		if (hbaline->auth_method != uaSSPI)
			INVALID_AUTH_OPTION("upn_username", gettext_noop("sspi"));
		if (strcmp(val, "1") == 0)
			hbaline->upn_username = true;
		else
			hbaline->upn_username = false;
	}
	else if (strcmp(name, "radiusservers") == 0)
	{
		struct addrinfo *gai_result;
		struct addrinfo hints;
		int			ret;
		List	   *parsed_servers;
		ListCell   *l;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusservers", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_servers))
		{
			/* 列表中有语法错误 */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS server list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			return false;
		}

		/* 对列表中的每一项加以转换 */
		foreach(l, parsed_servers)
		{
			MemSet(&hints, 0, sizeof(hints));
			hints.ai_socktype = SOCK_DGRAM;
			hints.ai_family = AF_UNSPEC;

			ret = pg_getaddrinfo_all((char *) lfirst(l), NULL, &hints, &gai_result);
			if (ret || !gai_result)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("could not translate RADIUS server name \"%s\" to address: %s",
								(char *) lfirst(l), gai_strerror(ret)),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));
				if (gai_result)
					pg_freeaddrinfo_all(hints.ai_family, gai_result);

				list_free(parsed_servers);
				return false;
			}
			pg_freeaddrinfo_all(hints.ai_family, gai_result);
		}

		/* 所有项都没问题，因此把它们存起来 */
		hbaline->radiusservers = parsed_servers;
		hbaline->radiusservers_s = pstrdup(val);
	}
	else if (strcmp(name, "radiusports") == 0)
	{
		List	   *parsed_ports;
		ListCell   *l;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusports", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_ports))
		{
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS port list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			*err_msg = psprintf("invalid RADIUS port number: \"%s\"", val);
			return false;
		}

		foreach(l, parsed_ports)
		{
			if (atoi(lfirst(l)) == 0)
			{
				ereport(elevel,
						(errcode(ERRCODE_CONFIG_FILE_ERROR),
						 errmsg("invalid RADIUS port number: \"%s\"", val),
						 errcontext("line %d of configuration file \"%s\"",
									line_num, file_name)));

				return false;
			}
		}
		hbaline->radiusports = parsed_ports;
		hbaline->radiusports_s = pstrdup(val);
	}
	else if (strcmp(name, "radiussecrets") == 0)
	{
		List	   *parsed_secrets;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiussecrets", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_secrets))
		{
			/* 列表中有语法错误 */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS secret list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			return false;
		}

		hbaline->radiussecrets = parsed_secrets;
		hbaline->radiussecrets_s = pstrdup(val);
	}
	else if (strcmp(name, "radiusidentifiers") == 0)
	{
		List	   *parsed_identifiers;
		char	   *dupval = pstrdup(val);

		REQUIRE_AUTH_OPTION(uaRADIUS, "radiusidentifiers", "radius");

		if (!SplitGUCList(dupval, ',', &parsed_identifiers))
		{
			/* 列表中有语法错误 */
			ereport(elevel,
					(errcode(ERRCODE_CONFIG_FILE_ERROR),
					 errmsg("could not parse RADIUS identifiers list \"%s\"",
							val),
					 errcontext("line %d of configuration file \"%s\"",
								line_num, file_name)));
			return false;
		}

		hbaline->radiusidentifiers = parsed_identifiers;
		hbaline->radiusidentifiers_s = pstrdup(val);
	}
	else
	{
		ereport(elevel,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("unrecognized authentication option name: \"%s\"",
						name),
				 errcontext("line %d of configuration file \"%s\"",
							line_num, file_name)));
		*err_msg = psprintf("unrecognized authentication option name: \"%s\"",
							name);
		return false;
	}
	return true;
}

/*
 *	扫描预先解析好的 hba 文件，寻找与该端口的连接
 *	请求相匹配的项。
 */
static void
check_hba(hbaPort *port)
{
	Oid			roleid;
	ListCell   *line;
	HbaLine    *hba;

	/* 取得目标角色的 OID。注意，角色无效时我们并不报错退出。 */
	roleid = get_role_oid(port->user_name, true);

	foreach(line, parsed_hba_lines)
	{
		hba = (HbaLine *) lfirst(line);

		/* 检查连接类型 */
		if (hba->conntype == ctLocal)
		{
			if (port->raddr.addr.ss_family != AF_UNIX)
				continue;
		}
		else
		{
			if (port->raddr.addr.ss_family == AF_UNIX)
				continue;

			/* 检查 SSL 状态 */
			if (port->ssl_in_use)
			{
				/* 连接是 SSL，同时匹配 "host" 和 "hostssl" */
				if (hba->conntype == ctHostNoSSL)
					continue;
			}
			else
			{
				/* 连接不是 SSL，同时匹配 "host" 和 "hostnossl" */
				if (hba->conntype == ctHostSSL)
					continue;
			}

			/* 检查 GSSAPI 状态 */
#ifdef ENABLE_GSS
			if (port->gss && port->gss->enc &&
				hba->conntype == ctHostNoGSS)
				continue;
			else if (!(port->gss && port->gss->enc) &&
					 hba->conntype == ctHostGSS)
				continue;
#else
			if (hba->conntype == ctHostGSS)
				continue;
#endif

			/* 检查 IP 地址 */
			switch (hba->ip_cmp_method)
			{
				case ipCmpMask:
					if (hba->hostname)
					{
						if (!check_hostname(port,
											hba->hostname))
							continue;
					}
					else
					{
						if (!check_ip(&port->raddr,
									  (struct sockaddr *) &hba->addr,
									  (struct sockaddr *) &hba->mask))
							continue;
					}
					break;
				case ipCmpAll:
					break;
				case ipCmpSameHost:
				case ipCmpSameNet:
					if (!check_same_host_or_net(&port->raddr,
												hba->ip_cmp_method))
						continue;
					break;
				default:
					/* 不应到达这里，但若到达了就视为不匹配 */
					continue;
			}
		}						/* != ctLocal */

		/* 检查数据库和角色 */
		if (!check_db(port->database_name, port->user_name, roleid,
					  hba->databases))
			continue;

		if (!check_role(port->user_name, roleid, hba->roles, false))
			continue;

		/* 找到了一条匹配的记录！ */
		port->hba = hba;
		return;
	}

	/* 若没有找到匹配项，则隐式拒绝。 */
	hba = palloc0(sizeof(HbaLine));
	hba->auth_method = uaImplicitReject;
	port->hba = hba;
}

/*
 * 读取配置文件，并为其内容创建 HbaLine 记录的 List。
 *
 * 配置先读进一个临时列表，若发生任何解析错误，
 * 就保留旧列表并返回 false。只有
 * 整个文件解析成功才替换该列表，函数返回 true。
 *
 * 结果为 false 时，若这是初次启动，调用方会负责报告 FATAL 错误。
 * 若发生在重新加载时，我们就继续用旧数据
 * 运行。
 */
bool
load_hba(void)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	ListCell   *line;
	List	   *new_parsed_lines = NIL;
	bool		ok = true;
	MemoryContext oldcxt;
	MemoryContext hbacxt;

	file = open_auth_file(HbaFileName, LOG, 0, NULL);
	if (file == NULL)
	{
		/* 错误已经记入日志 */
		return false;
	}

	tokenize_auth_file(HbaFileName, file, &hba_lines, LOG, 0);

	/* 现在解析所有行 */
	Assert(PostmasterContext);
	hbacxt = AllocSetContextCreate(PostmasterContext,
								   "hba parser context",
								   ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(hbacxt);
	foreach(line, hba_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		HbaLine    *newline;

		/* 不要解析已经有错误的行 */
		if (tok_line->err_msg != NULL)
		{
			ok = false;
			continue;
		}

		if ((newline = parse_hba_line(tok_line, LOG)) == NULL)
		{
			/* 解析错误；记住出了问题 */
			ok = false;

			/*
			 * 继续解析文件的其余部分，这样我们就能报告
			 * 第一行以外的错误。错误已经记入日志，
			 * 这里不必再多说。
			 */
			continue;
		}

		new_parsed_lines = lappend(new_parsed_lines, newline);
	}

	/*
	 * 有效的 HBA 文件必须至少有一条记录；否则没有办法
	 * 连接到 postmaster。但只有在还没有
	 * 解析错误时才对此报错。
	 */
	if (ok && new_parsed_lines == NIL)
	{
		ereport(LOG,
				(errcode(ERRCODE_CONFIG_FILE_ERROR),
				 errmsg("configuration file \"%s\" contains no entries",
						HbaFileName)));
		ok = false;
	}

	/* 释放词法分析器内存 */
	free_auth_file(file, 0);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		/*
		 * 文件含有一处或多处错误，因此退出。MemoryContextDelete
		 * 足以清理一切，包括正则表达式。
		 */
		MemoryContextDelete(hbacxt);
		return false;
	}

	/* 已成功加载新文件，替换我们正在使用的那份 */
	if (parsed_hba_context != NULL)
		MemoryContextDelete(parsed_hba_context);
	parsed_hba_context = hbacxt;
	parsed_hba_lines = new_parsed_lines;

	return true;
}


/*
 * 解析 ident 配置文件中已做词法分析的一行，并把结果存入
 * IdentLine 结构体。
 *
 * 若解析失败，以 ereport 级别 elevel 记一条消息，把错误
 * 字符串存入 tok_line->err_msg 并返回 NULL。
 *
 * 若 ident_user 是正则表达式（即以斜杠开头），就编译它
 * 并存入 IdentLine 结构体。
 *
 * 注意：出错时本函数会泄漏内存。调用方应当已经
 * 设置一个内存上下文，在本函数返回
 * NULL 时重置该上下文。
 */
IdentLine *
parse_ident_line(TokenizedAuthLine *tok_line, int elevel)
{
	int			line_num = tok_line->line_num;
	char	   *file_name = tok_line->file_name;
	char	  **err_msg = &tok_line->err_msg;
	ListCell   *field;
	List	   *tokens;
	AuthToken  *token;
	IdentLine  *parsedline;

	Assert(tok_line->fields != NIL);
	field = list_head(tok_line->fields);

	parsedline = palloc0(sizeof(IdentLine));
	parsedline->linenumber = line_num;

	/* 取得映射词元（必须存在） */
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->usermap = pstrdup(token->string);

	/* 取得 ident 用户词元 */
	field = lnext(tok_line->fields, field);
	IDENT_FIELD_ABSENT(field);
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);

	/* 复制 ident 用户词元 */
	parsedline->system_user = copy_auth_token(token);

	/* 取得 PG 角色名词元 */
	field = lnext(tok_line->fields, field);
	IDENT_FIELD_ABSENT(field);
	tokens = lfirst(field);
	IDENT_MULTI_VALUE(tokens);
	token = linitial(tokens);
	parsedline->pg_user = copy_auth_token(token);

	/*
	 * 字段校验已经完成，必要时从用户
	 * 词元编译正则表达式。
	 */
	if (regcomp_auth_token(parsedline->system_user, file_name, line_num,
						   err_msg, elevel))
	{
		/* err_msg 中包含要报告的错误 */
		return NULL;
	}

	if (regcomp_auth_token(parsedline->pg_user, file_name, line_num,
						   err_msg, elevel))
	{
		/* err_msg 中包含要报告的错误 */
		return NULL;
	}

	return parsedline;
}

/*
 *	处理已解析 ident 配置行中的一行。
 *
 *	把输入的已解析 ident 行与所需的映射、pg_user 和 system_user 比较。
 *	根据结果设置 *found_p 和 *error_p。
 */
static void
check_ident_usermap(IdentLine *identLine, const char *usermap_name,
					const char *pg_user, const char *system_user,
					bool case_insensitive, bool *found_p, bool *error_p)
{
	Oid			roleid;

	*found_p = false;
	*error_p = false;

	if (strcmp(identLine->usermap, usermap_name) != 0)
		/* 该行不匹配我们要找的映射名，因此直接中止 */
		return;

	/* 取得目标角色的 OID。注意，角色无效时我们并不报错退出。 */
	roleid = get_role_oid(pg_user, true);

	/* 匹配吗？ */
	if (token_has_regexp(identLine->system_user))
	{
		/*
		 * 把系统用户名当作恰好返回
		 * 一个匹配的正则表达式来处理。若数据库用户名
		 * 字符串中有 \1，就用它替换。
		 */
		int			r;
		regmatch_t	matches[2];
		char	   *ofs;
		AuthToken  *expanded_pg_user_token;
		bool		created_temporary_token = false;

		r = regexec_auth_token(system_user, identLine->system_user, 2, matches);
		if (r)
		{
			char		errstr[100];

			if (r != REG_NOMATCH)
			{
				/* REG_NOMATCH 不是错误，其余都是 */
				pg_regerror(r, identLine->system_user->regex, errstr, sizeof(errstr));
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression match for \"%s\" failed: %s",
								identLine->system_user->string + 1, errstr)));
				*error_p = true;
			}
			return;
		}

		/*
		 * 除非该字段已有某种特殊含义，否则用第一个捕获组替换 \1，
		 * 特殊含义例如组成员身份，或基于正则表达式的
		 * 检查。
		 */
		if (!token_is_member_check(identLine->pg_user) &&
			!token_has_regexp(identLine->pg_user) &&
			(ofs = strstr(identLine->pg_user->string, "\\1")) != NULL)
		{
			char	   *expanded_pg_user;
			int			offset;

			/* 请求替换第一个参数 */
			if (matches[1].rm_so < 0)
			{
				ereport(LOG,
						(errcode(ERRCODE_INVALID_REGULAR_EXPRESSION),
						 errmsg("regular expression \"%s\" has no subexpressions as requested by backreference in \"%s\"",
								identLine->system_user->string + 1, identLine->pg_user->string)));
				*error_p = true;
				return;
			}

			/*
			 * 长度：原长度减去 \1 的长度，加上匹配的长度，
			 * 再加上空字符结尾符
			 */
			expanded_pg_user = palloc0(strlen(identLine->pg_user->string) - 2 + (matches[1].rm_eo - matches[1].rm_so) + 1);
			offset = ofs - identLine->pg_user->string;
			memcpy(expanded_pg_user, identLine->pg_user->string, offset);
			memcpy(expanded_pg_user + offset,
				   system_user + matches[1].rm_so,
				   matches[1].rm_eo - matches[1].rm_so);
			strcat(expanded_pg_user, ofs + 2);

			/*
			 * 把该词元标为带引号，这样它只会按字面比较，
			 * 而不会按某种特殊含义比较，例如 "all" 或
			 * 组成员身份检查。
			 */
			expanded_pg_user_token = make_auth_token(expanded_pg_user, true);
			created_temporary_token = true;
			pfree(expanded_pg_user);
		}
		else
		{
			expanded_pg_user_token = identLine->pg_user;
		}

		/* 检查 Postgres 用户 */
		*found_p = check_role(pg_user, roleid,
							  list_make1(expanded_pg_user_token),
							  case_insensitive);

		if (created_temporary_token)
			free_auth_token(expanded_pg_user_token);

		return;
	}
	else
	{
		/*
		 * 不是正则表达式，因此做完整匹配。若系统
		 * 用户不匹配，就直接返回。
		 */
		if (case_insensitive)
		{
			if (!token_matches_insensitive(identLine->system_user,
										   system_user))
				return;
		}
		else
		{
			if (!token_matches(identLine->system_user, system_user))
				return;
		}

		/* 检查 Postgres 用户 */
		*found_p = check_role(pg_user, roleid,
							  list_make1(identLine->pg_user),
							  case_insensitive);
	}
}


/*
 *	逐行扫描（预先解析好的）ident 用户映射文件，寻找匹配
 *
 *	看 ident 用户名为 "system_user" 的系统用户是否被允许作为
 *	Postgres 用户 "pg_user"，依据是用户映射 "usermap_name"。
 *
 *	特殊情况：用户映射为 NULL，等价于以前所谓的
 *	"sameuser" 或 "samerole"，意思是不要查看用户映射文件。
 *	那是一张隐含映射，其中 "pg_user" 必须与
 *	"system_user" 相同才能获得授权。
 *
 *	当且仅当已授权时返回 STATUS_OK，否则返回 STATUS_ERROR。
 */
int
check_usermap(const char *usermap_name,
			  const char *pg_user,
			  const char *system_user,
			  bool case_insensitive)
{
	bool		found_entry = false,
				error = false;

	if (usermap_name == NULL || usermap_name[0] == '\0')
	{
		if (case_insensitive)
		{
			if (pg_strcasecmp(pg_user, system_user) == 0)
				return STATUS_OK;
		}
		else
		{
			if (strcmp(pg_user, system_user) == 0)
				return STATUS_OK;
		}
		ereport(LOG,
				(errmsg("provided user name (%s) and authenticated user name (%s) do not match",
						pg_user, system_user)));
		return STATUS_ERROR;
	}
	else
	{
		ListCell   *line_cell;

		foreach(line_cell, parsed_ident_lines)
		{
			check_ident_usermap(lfirst(line_cell), usermap_name,
								pg_user, system_user, case_insensitive,
								&found_entry, &error);
			if (found_entry || error)
				break;
		}
	}
	if (!found_entry && !error)
	{
		ereport(LOG,
				(errmsg("no match in usermap \"%s\" for user \"%s\" authenticated as \"%s\"",
						usermap_name, pg_user, system_user)));
	}
	return found_entry ? STATUS_OK : STATUS_ERROR;
}


/*
 * 读取 ident 配置文件，并为其内容创建 IdentLine 记录的
 * List。
 *
 * 做法与 load_hba() 相同，但是针对用户配置文件。
 */
bool
load_ident(void)
{
	FILE	   *file;
	List	   *ident_lines = NIL;
	ListCell   *line_cell;
	List	   *new_parsed_lines = NIL;
	bool		ok = true;
	MemoryContext oldcxt;
	MemoryContext ident_context;
	IdentLine  *newline;

	/* 不是 FATAL……我们只是不做任何特殊的 ident 映射 */
	file = open_auth_file(IdentFileName, LOG, 0, NULL);
	if (file == NULL)
	{
		/* 错误已经记入日志 */
		return false;
	}

	tokenize_auth_file(IdentFileName, file, &ident_lines, LOG, 0);

	/* 现在解析所有行 */
	Assert(PostmasterContext);
	ident_context = AllocSetContextCreate(PostmasterContext,
										  "ident parser context",
										  ALLOCSET_SMALL_SIZES);
	oldcxt = MemoryContextSwitchTo(ident_context);
	foreach(line_cell, ident_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line_cell);

		/* 不要解析已经有错误的行 */
		if (tok_line->err_msg != NULL)
		{
			ok = false;
			continue;
		}

		if ((newline = parse_ident_line(tok_line, LOG)) == NULL)
		{
			/* 解析错误；记住出了问题 */
			ok = false;

			/*
			 * 继续解析文件的其余部分，这样我们就能报告
			 * 第一行以外的错误。错误已经记入日志，
			 * 这里不必再多说。
			 */
			continue;
		}

		new_parsed_lines = lappend(new_parsed_lines, newline);
	}

	/* 释放词法分析器内存 */
	free_auth_file(file, 0);
	MemoryContextSwitchTo(oldcxt);

	if (!ok)
	{
		/*
		 * 文件含有一处或多处错误，因此退出。MemoryContextDelete
		 * 足以清理一切，包括正则表达式。
		 */
		MemoryContextDelete(ident_context);
		return false;
	}

	/* 已成功加载新文件，替换我们正在使用的那份 */
	if (parsed_ident_context != NULL)
		MemoryContextDelete(parsed_ident_context);

	parsed_ident_context = ident_context;
	parsed_ident_lines = new_parsed_lines;

	return true;
}



/*
 *	确定从来自前端 "raddr" 的用户 "user" 访问数据库
 *	"database" 时应当使用哪种认证方法。返回该方法以及
 *	一个可选参数（存在 *port 的字段中），还有 STATUS_OK。
 *
 *	若文件不含任何匹配该请求的记录，我们就返回
 *	method = uaImplicitReject。
 */
void
hba_getauthmethod(hbaPort *port)
{
	check_hba(port);
}


/*
 * 返回正在使用的认证方法名（"gss"、"md5"、"trust" 等）。
 *
 * 返回值是静态分配的（见 UserAuthName 数组），
 * 不应释放。
 */
const char *
hba_authname(UserAuth auth_method)
{
	return UserAuthName[auth_method];
}
