/*-------------------------------------------------------------------------
 *
 * scansup.c
 *	  核心词法分析器的扫描器支持例程  
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/scansup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "mb/pg_wchar.h"
#include "parser/scansup.h"


/*
 * downcase_truncate_identifier() --- 对不带引号的标识符  
 * 做适当的小写折叠与截断。可选地发出截断警告。   
 *
 * 返回 palloc 的字符串，内含调整后的标识符。  
 *
 * 注意：某些用法传入字符串未以空字符结尾。  
 *
 * 注意：本函数的 API 设计上允许小写折叠转换       
 * 增加字符串长度，但我们尚不支持这一点。若     
 * 要实现它，需要修好 utils/adt/varlena.c 中的        
 * SplitIdentifierString()。                     
 */
char *
downcase_truncate_identifier(const char *ident, int len, bool warn)
{
	return downcase_identifier(ident, len, warn, true);
}

/*
 * downcase_truncate_identifier 的主力      
 */
char *
downcase_identifier(const char *ident, int len, bool warn, bool truncate)
{
	char	   *result;
	int			i;
	bool		enc_is_single_byte;

	result = palloc(len + 1);
	enc_is_single_byte = pg_database_encoding_max_length() == 1;

	/*
	 * SQL99 规定了感知 Unicode 的大小写规范化，我们        
	 * 尚无相应的基础设施。因此我们改用 tolower()         
	 * 来提供感知区域设置的转换。但有些区域设置         
	 * 中这也不正确（例如土耳其语对 'i' 和 'I' 的         
	 * 处理可能很奇怪）。我们目前的折衷方案是：         
	 * 高位字符使用 tolower()，只要它们不是多字节   
	 * 字符的组成部分；7 位字符用纯 ASCII 小写折叠。  
	 */
	for (i = 0; i < len; i++)
	{
		unsigned char ch = (unsigned char) ident[i];

		if (ch >= 'A' && ch <= 'Z')
			ch += 'a' - 'A';
		else if (enc_is_single_byte && IS_HIGHBIT_SET(ch) && isupper(ch))
			ch = tolower(ch);
		result[i] = (char) ch;
	}
	result[i] = '\0';

	if (i >= NAMEDATALEN && truncate)
		truncate_identifier(result, i, warn);

	return result;
}


/*
 * truncate_identifier() --- 将标识符截断到 NAMEDATALEN-1 字节。 
 *
 * 必要时原地修改给定的字符串。若被要求，         
 * 则发出警告。  
 *
 * 我们需要调用者传入字符串长度，这在某些            
 * 场合可省去 strlen() 调用。  
 */
void
truncate_identifier(char *ident, int len, bool warn)
{
	if (len >= NAMEDATALEN)
	{
		len = pg_mbcliplen(ident, len, NAMEDATALEN - 1);
		if (warn)
			ereport(NOTICE,
					(errcode(ERRCODE_NAME_TOO_LONG),
					 errmsg("identifier \"%s\" will be truncated to \"%.*s\"",
							ident, len, ident)));
		ident[len] = '\0';
	}
}

/*
 * scanner_isspace() --- 若 flex 扫描器认为字符为空白则返回真  
 *
 * 在需匹配词法分析器行为的场合，就应当用本函数，    
 * 而非可能依赖区域设置的 isspace()。             
 *
 * 原则上可能需要 isalnum 等类似函数，不过目前似乎只有 
 * isspace 是必需的。          
 */
bool
scanner_isspace(char ch)
{
	/* 必须与 scan.l 的 {space} 字符列表一致     */
	if (ch == ' ' ||
		ch == '\t' ||
		ch == '\n' ||
		ch == '\r' ||
		ch == '\f')
		return true;
	return false;
}
