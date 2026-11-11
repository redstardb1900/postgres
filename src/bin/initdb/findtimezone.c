/*-------------------------------------------------------------------------
 *
 * findtimezone.c
 *	  Functions for determining the default timezone to use.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/initdb/findtimezone.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pgtz.h"

/* 理想情况下本应放在 .h 文件中，但似乎不值得这样麻烦 */
extern const char *select_default_timezone(const char *share_path);


#ifndef SYSTEMTZDIR
static char tzdirpath[MAXPGPATH];
#endif


/*
 * 返回时区数据目录的完整路径名
 *
 * 在此文件中，假设 tzdirpath 已由 select_default_timezone 设置。
      */
static const char *
pg_TZDIR(void)
{
#ifndef SYSTEMTZDIR
	/* 常规情况：时区文件位于 share 目录下  */
	return tzdirpath;
#else
	/* 我们配置为使用系统的时区数据库      */
	return SYSTEMTZDIR;
#endif
}


/*
 * 给定一个时区名称，用 open() 打开时区数据文件。    
 * 成功返回描述符，否则 -1。   
 *
 * 这比同名的后端函数更简单，因为
 * 我们假设输入字符串的大小写已经正确，所以
 * 不需要大小写折叠。（如果我们最初从文件系统获取
 * 文件名，这显然是正确的。它唯一可能的其他来源
 * 是环境变量 TZ，似乎没有必要允许
 * 其中的大小写变化；其他程序不太可能这样做。）
 *
 * 如果 "canonname" 不为 NULL，则在成功时将给定名称的规范拼写
 * 存储在那里（缓冲区必须大于 TZ_STRLEN_MAX 字节！）。
 * 这是冗余的，但为了与后端代码兼容而保留。
                                                           */
int
pg_open_tzfile(const char *name, char *canonname)
{
	char		fullname[MAXPGPATH];

	if (canonname)
		strlcpy(canonname, name, TZ_STRLEN_MAX + 1);

	strlcpy(fullname, pg_TZDIR(), sizeof(fullname));
	if (strlen(fullname) + 1 + strlen(name) >= MAXPGPATH)
		return -1;				/* 放不下     */
	strcat(fullname, "/");
	strcat(fullname, name);

	return open(fullname, O_RDONLY | PG_BINARY, 0);
}



/*
 * 加载时区定义。
 * 不验证时区是否可接受！
 *
 * 这对应于后端的 pg_tzset()，区别在于我们
 * 一次只支持一个加载的时区。
                                 */
static pg_tz *
pg_load_tz(const char *name)
{
	static pg_tz tz;

	if (strlen(name) > TZ_STRLEN_MAX)
		return NULL;			/* 放不下        */

	/*
	 * "GMT" 总是被传给 tzparse()；参见 pg_tzset() 的说明  
	 */
	if (strcmp(name, "GMT") == 0)
	{
		if (!tzparse(name, &tz.state, true))
		{
			/* 这真的真的不应该发生 ...        */
			return NULL;
		}
	}
	else if (tzload(name, NULL, &tz.state, true) != 0)
	{
		if (name[0] == ':' || !tzparse(name, &tz.state, false))
		{
			return NULL;		/* 未知时区     */
		}
	}

	strcpy(tz.TZname, name);

	return &tz;
}


/*
 * 以下代码块尝试确定我们时区数据库中哪个时区        
 * 是活动系统时区的最佳匹配。                            
 *
 * 在大多数系统上，我们依赖于尝试匹配 C 库             
 * localtime() 函数的可观察行为。匹配到过去最远的      
 * 数据库时区就是我们要使用的。通常会有多个时区    
 * 具有相同的排名（因为 IANA 数据库为许多时区           
 * 分配了多个名称）。我们通过首先检查"首选"           
 * 名称（如 "UTC"），然后任意地偏好较短、然后           
 * 按字母顺序较早的时区名称来打破平局。（若        
 * 未显式偏好 "UTC"，因字母顺序，会得到别名 "UCT"   
 * 。）    
 *
 * 许多现代系统使用 IANA 数据库，所以如果我们能      
 * 确定系统认为它使用的是哪个时区，并且其行为与       
 * 我们同名的时区匹配，我们可以跳过在数据库中搜       
 * 索所有时区的相当昂贵的遍历。这个捷径路径也确保     
 * 我们拼写时区名称的方式与系统设置相同，即使         
 * 同一时区有多个别名。        
 *
 * Win32 关于时区的原生知识似乎太不完整，且与         
 * IANA 数据库差异太大，上述匹配策略毫无用处。          
 * 但可用时区的数量有限，所以我们可以依赖手工  
 * 制作的映射表。                                         
 */

#ifndef WIN32

#define T_DAY	((time_t) (60*60*24))
#define T_WEEK	((time_t) (60*60*24*7))
#define T_MONTH ((time_t) (60*60*24*31))

#define MAX_TEST_TIMES (52*100) /* 100 年   */

struct tztry
{
	int			n_test_times;
	time_t		test_times[MAX_TEST_TIMES];
};

static bool check_system_link_file(const char *linkname, struct tztry *tt,
								   char *bestzonename);
static void scan_available_timezones(char *tzdir, char *tzdirsub,
									 struct tztry *tt,
									 int *bestscore, char *bestzonename);


/*
 * 从系统 struct tm 取 GMT 偏移量 
 */
static int
get_timezone_offset(struct tm *tm)
{
#if defined(HAVE_STRUCT_TM_TM_ZONE)
	return tm->tm_gmtoff;
#elif defined(HAVE_INT_TIMEZONE)
	return -TIMEZONE_GLOBAL;
#else
#error No way to determine TZ? Can this happen?
#endif
}

/*
 * 将 y/m/d 转换为 time_t 的辅助函数（而非 pg_time_t） 
 */
static time_t
build_time_t(int year, int month, int day)
{
	struct tm	tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_mday = day;
	tm.tm_mon = month - 1;
	tm.tm_year = year - 1900;
	tm.tm_isdst = -1;

	return mktime(&tm);
}

/*
 * 系统 tm 值是否与我们自己计算的值匹配？
 */
static bool
compare_tm(struct tm *s, struct pg_tm *p)
{
	if (s->tm_sec != p->tm_sec ||
		s->tm_min != p->tm_min ||
		s->tm_hour != p->tm_hour ||
		s->tm_mday != p->tm_mday ||
		s->tm_mon != p->tm_mon ||
		s->tm_year != p->tm_year ||
		s->tm_wday != p->tm_wday ||
		s->tm_yday != p->tm_yday ||
		s->tm_isdst != p->tm_isdst)
		return false;
	return true;
}

/*
 * 查看特定时区设置与系统行为的匹配程度              
 *
 * 我们根据时区设置匹配的测试时间数量来评分。     
 * （测试时间按从晚到早排序，但此例程实际上并         
 * 不知道；它只是扫描到第一个不匹配为止。）         
 *
 * 对于完全不可用的设置返回 -1；这比可工作但          
 * 不匹配任何第一个测试时间的设置的零分更            
 * 差。    
 */
static int
score_timezone(const char *tzname, struct tztry *tt)
{
	int			i;
	pg_time_t	pgtt;
	struct tm  *systm;
	struct pg_tm *pgtm;
	char		cbuf[TZ_STRLEN_MAX + 1];
	pg_tz	   *tz;

	/* 加载时区定义       */
	tz = pg_load_tz(tzname);
	if (!tz)
		return -1;				/* 无法识别的时区名  */

	/* 如果涉及闰秒则拒绝     */
	if (!pg_tz_acceptable(tz))
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "Reject TZ \"%s\": uses leap seconds\n", tzname);
#endif
		return -1;
	}

	/* 在所有测试时间检查匹配     */
	for (i = 0; i < tt->n_test_times; i++)
	{
		pgtt = (pg_time_t) (tt->test_times[i]);
		pgtm = pg_localtime(&pgtt, tz);
		if (!pgtm)
			return -1;			/* 可能不应该发生     */
		systm = localtime(&(tt->test_times[i]));
		if (!systm)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "TZ \"%s\" scores %d: at %ld %04d-%02d-%02d %02d:%02d:%02d %s, system had no data\n",
					tzname, i, (long) pgtt,
					pgtm->tm_year + 1900, pgtm->tm_mon + 1, pgtm->tm_mday,
					pgtm->tm_hour, pgtm->tm_min, pgtm->tm_sec,
					pgtm->tm_isdst ? "dst" : "std");
#endif
			return i;
		}
		if (!compare_tm(systm, pgtm))
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "TZ \"%s\" scores %d: at %ld %04d-%02d-%02d %02d:%02d:%02d %s versus %04d-%02d-%02d %02d:%02d:%02d %s\n",
					tzname, i, (long) pgtt,
					pgtm->tm_year + 1900, pgtm->tm_mon + 1, pgtm->tm_mday,
					pgtm->tm_hour, pgtm->tm_min, pgtm->tm_sec,
					pgtm->tm_isdst ? "dst" : "std",
					systm->tm_year + 1900, systm->tm_mon + 1, systm->tm_mday,
					systm->tm_hour, systm->tm_min, systm->tm_sec,
					systm->tm_isdst ? "dst" : "std");
#endif
			return i;
		}
		if (systm->tm_isdst >= 0)
		{
			/* 也检查时区名称的匹配 */
			if (pgtm->tm_zone == NULL)
				return -1;		/* 可能不应该发生     */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", systm);	/* 缩写    */
			if (strcmp(cbuf, pgtm->tm_zone) != 0)
			{
#ifdef DEBUG_IDENTIFY_TIMEZONE
				fprintf(stderr, "TZ \"%s\" scores %d: at %ld \"%s\" versus \"%s\"\n",
						tzname, i, (long) pgtt,
						pgtm->tm_zone, cbuf);
#endif
				return i;
			}
		}
	}

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "TZ \"%s\" gets max score %d\n", tzname, i);
#endif

	return i;
}

/*
 * 测试给定的时区名称是否与 localtime() 行为完全匹配
     */
static bool
perfect_timezone_match(const char *tzname, struct tztry *tt)
{
	return (score_timezone(tzname, tt) == tt->n_test_times);
}


/*
 * 尝试识别一个时区名称（在我们的术语中），
 * 它最匹配系统 localtime() 函数的观察行为。
             */
static const char *
identify_system_timezone(void)
{
	static char resultbuf[TZ_STRLEN_MAX + 1];
	time_t		tnow;
	time_t		t;
	struct tztry tt;
	struct tm  *tm;
	int			thisyear;
	int			bestscore;
	char		tmptzdir[MAXPGPATH];
	int			std_ofs;
	char		std_zone_name[TZ_STRLEN_MAX + 1],
				dst_zone_name[TZ_STRLEN_MAX + 1];
	char		cbuf[TZ_STRLEN_MAX + 1];

	/* 初始化操作系统时区库 */
	tzset();

	/*
	 * 设置要探测的日期列表，以查看我们的时区
	 * 与系统时区的匹配程度。我们首先探测当前年份的
	 * 一月和七月；这可以快速排除绝大多数
	 * TZ 数据库条目。如果这些日期匹配，我们每隔一周向前探测 100
	 * 年，从当前七月开始向后。（每周分辨率足以
	 * 识别 DST 转换规则，因为每个人都在周日切换。）
	 * 这足以覆盖大部分 Unix time_t 范围，
	 * 我们不想看得更远，因为许多系统在更远的过去
	 * 不会有合理的 TZ 行为。时区匹配得越远，
	 * 我们的评分就越高。这可能看起来像是一种相当随意的
	 * 做法，但经验表明，系统提供的
	 * 时区定义在最近的过去很可能有正确的 DST 行为，
	 * 而在更远的过去则不那么准确。以这种方式评分
	 * 允许我们识别与 IANA 数据库有一些共性的时区，
	 * 而不要求完全匹配。（注意：我们探测周四，
	 * 而不是周日，以避免触发 localtime 本身的
	 * DST 转换错误。）
	                                                                                                                 */
	tnow = time(NULL);
	tm = localtime(&tnow);
	if (!tm)
		return NULL;			/* 若 localtime 失效则放弃     */
	thisyear = tm->tm_year + 1900;

	t = build_time_t(thisyear, 1, 15);

	/*
	 * 回退到 GMT 周四午夜。这依赖于 time_t 原点
	 * 是 1970 年 1 月 1 日周四的知识。（使用不同的原点
	 * 我们会探测一周中的另一天，但这无关紧要，
	 * 除非 localtime() 有 DST 转换错误。）
	                           */
	t -= (t % T_WEEK);

	tt.n_test_times = 0;
	tt.test_times[tt.n_test_times++] = t;

	t = build_time_t(thisyear, 7, 15);
	t -= (t % T_WEEK);

	tt.test_times[tt.n_test_times++] = t;

	while (tt.n_test_times < MAX_TEST_TIMES)
	{
		t -= T_WEEK;
		tt.test_times[tt.n_test_times++] = t;
	}

	/*
	 * 尝试通过查看是否能直接识别
	 * 系统的时区设置来避免暴力搜索。
	 *
	 * 目前我们只检查 /etc/localtime；还有其他约定，
	 * 但这似乎是唯一在足够多平台上使用的约定，
	 * 值得为此费心。
	                                          */
	if (check_system_link_file("/etc/localtime", &tt, resultbuf))
		return resultbuf;

	/* 不走运，所以搜索最佳匹配的时区文件    */
	strlcpy(tmptzdir, pg_TZDIR(), sizeof(tmptzdir));
	bestscore = -1;
	resultbuf[0] = '\0';
	scan_available_timezones(tmptzdir, tmptzdir + strlen(tmptzdir) + 1,
							 &tt,
							 &bestscore, resultbuf);
	if (bestscore > 0)
	{
		/* 忽略 IANA 相当愚蠢的 "Factory" 时区；改用 GMT  */
		if (strcmp(resultbuf, "Factory") == 0)
			return NULL;
		return resultbuf;
	}

	/*
	 * 数据库中找不到匹配，因此接下来尝试构造的时区    
	 * 名（如 "PST8PDT"） 
	 *
	 * 首先需要确定本地标准时区和夏令时时区的名称。     
	 * 思路是从今天起向前扫描，直到已见到这两个时区    
	 * （若两者都在使用）。 
	 */
	memset(std_zone_name, 0, sizeof(std_zone_name));
	memset(dst_zone_name, 0, sizeof(dst_zone_name));
	std_ofs = 0;

	tnow = time(NULL);

	/*
	 * 回退到 GMT 午夜，这样结果不依赖于当地时             
	 * 间
	 */
	tnow -= (tnow % T_DAY);

	/*
	 * 我们必须向前看比一年更远一点，以防今天
	 * 刚刚过了一个 DST 边界，该边界在年份中比下一个
	 * 相似的边界更早。任意扫描最多 14 个月。
	               */
	for (t = tnow; t <= tnow + T_MONTH * 14; t += T_MONTH)
	{
		tm = localtime(&t);
		if (!tm)
			continue;
		if (tm->tm_isdst < 0)
			continue;
		if (tm->tm_isdst == 0 && std_zone_name[0] == '\0')
		{
			/* 找到 STD 区 */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* 缩写    */
			strcpy(std_zone_name, cbuf);
			std_ofs = get_timezone_offset(tm);
		}
		if (tm->tm_isdst > 0 && dst_zone_name[0] == '\0')
		{
			/* 找到 DST 区 */
			memset(cbuf, 0, sizeof(cbuf));
			strftime(cbuf, sizeof(cbuf) - 1, "%Z", tm); /* 缩写    */
			strcpy(dst_zone_name, cbuf);
		}
		/* 均找到则完成 */
		if (std_zone_name[0] && dst_zone_name[0])
			break;
	}

	/* 至此我们应该已找到 STD 时区名称   */
	if (std_zone_name[0] == '\0')
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "could not determine system time zone\n");
#endif
		return NULL;			/* 去 GMT   */
	}

	/* 若找到 DST 则尝试 STD<ofs>DST  */
	if (dst_zone_name[0] != '\0')
	{
		snprintf(resultbuf, sizeof(resultbuf), "%s%d%s",
				 std_zone_name, -std_ofs / 3600, dst_zone_name);
		if (score_timezone(resultbuf, &tt) > 0)
			return resultbuf;
	}

	/* 只尝试 STD 时区（至少对 GMT 有效）     */
	strcpy(resultbuf, std_zone_name);
	if (score_timezone(resultbuf, &tt) > 0)
		return resultbuf;

	/* 试 STD<ofs> */
	snprintf(resultbuf, sizeof(resultbuf), "%s%d",
			 std_zone_name, -std_ofs / 3600);
	if (score_timezone(resultbuf, &tt) > 0)
		return resultbuf;

	/*
	 * 没有找到时区。回退使用 GMT 时区。请注意，
	 * IANA 时区数据库以 POSIX 风格命名 GMT 偏移时区：
	 * 加号表示格林威治以西。不幸的是，这与 SQL
	 * 约定相反。我们是否应该因此更改名称？可能不...
	                               */
	snprintf(resultbuf, sizeof(resultbuf), "Etc/GMT%s%d",
			 (-std_ofs > 0) ? "+" : "", -std_ofs / 3600);

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "could not recognize system time zone, using \"%s\"\n",
			resultbuf);
#endif
	return resultbuf;
}

/*
 * 检查系统提供的符号链接文件，看它是否告诉我们时区信息。
 *
 * 不幸的是，在没有 TZ 环境设置的情况下，
 * 系统默认时区的确定方式几乎没有标准化。
 * 但一个常见的策略是在众所周知的位置创建符号链接。
 * 如果 "linkname" 标识了一个可读的符号链接，并且其内容的尾部
 * 与我们已知的时区名称匹配，并且 localtime() 的实际行为
 * 与我们对该时区的理解一致，那么我们可以使用该时区名称。
 *
 * 我们坚持完全的行为匹配，如果系统
 * 使用与我们不同的 IANA 数据库版本，这可能不会发生；
 * 但在这种情况下，最好回退到暴力搜索。
 *
 * linkname 是要探测的符号链接文件位置。
 *
 * tt 告诉我们需要匹配的系统时区行为。
 *
 * 如果我们成功识别了时区名称，将其存储在 *bestzonename 中并
 * 返回 true；否则返回 false。bestzonename 必须是
 * TZ_STRLEN_MAX + 1 长度的缓冲区。
        */
static bool
check_system_link_file(const char *linkname, struct tztry *tt,
					   char *bestzonename)
{
#ifdef HAVE_READLINK
	char		link_target[MAXPGPATH];
	int			len;
	const char *cur_name;

	/*
	 * 尝试读取 symlink。若不存在、非 symlink 等等，         
	 * 就静默失败；确切原因无需关心。       
	 */
	len = readlink(linkname, link_target, sizeof(link_target));
	if (len < 0 || len >= sizeof(link_target))
		return false;
	link_target[len] = '\0';

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "symbolic link \"%s\" contains \"%s\"\n",
			linkname, link_target);
#endif

	/*
	 * 符号链接可能是 "/path/to/zones/zone/name" 的形式，
	 * 也可能是相对路径。没有人把时区数据库直接放在
	 * 根目录中，所以我们可以肯定地跳过第一个组件；但
	 * 之后就是试错法，以确定哪个路径组件开始
	 * 时区名称。
	                        */
	cur_name = link_target;
	while (*cur_name)
	{
		/* 前进到路径的下一个段  */
		cur_name = strchr(cur_name + 1, '/');
		if (cur_name == NULL)
			break;
		/* 如果有连续的斜杠，跳过所有，就像内核那样    */
		do
		{
			cur_name++;
		} while (*cur_name == '/');

		/*
		 * 测试路径其余部分，看是否匹配时区名。      
		 * 相对路径可能包含 ".."；如果第一个成分就是它，  
		 * 则无需再测试。同时也要防范超长的名称。      
		 */
		if (*cur_name && *cur_name != '.' &&
			strlen(cur_name) <= TZ_STRLEN_MAX &&
			perfect_timezone_match(cur_name, tt))
		{
			/* 成功   */
			strcpy(bestzonename, cur_name);
			return true;
		}
	}

	/* 无法提取匹配的时区名称     */
	return false;
#else
	/* 没有 symlink？算了 */
	return false;
#endif
}

/*
 * 给定一个时区名称，确定它在同等良好的匹配              
 * 中是否应优先于其他名称。输出值是任意的，             
 * 但我们约定 0 表示"中性"默认偏好；值越大越优先。     
 */
static int
zone_name_pref(const char *zonename)
{
	/*
	 * 优先选择 UTC 而非 UCT 之类的替代项。Etc/UTC 亦       
	 * 优于 Etc/UCT；但 UTC 优于 Etc/UTC  
	 */
	if (strcmp(zonename, "UTC") == 0)
		return 50;
	if (strcmp(zonename, "Etc/UTC") == 0)
		return 40;

	/*
	 * 我们不想选择 "localtime" 或 "posixrules"，
	 * 除非我们找不到该时区的其他名称。
	                                         */
	if (strcmp(zonename, "localtime") == 0 ||
		strcmp(zonename, "posixrules") == 0)
		return -50;

	return 0;
}

/*
 * 递归扫描时区数据库，寻找与系统时区行为           
 * 最匹配的时区。        
 *
 * tzdir 指向一个大小为 MAXPGPATH 的缓冲区。              
 * 进入时，其中保存着一个包含 TZ 文件的目录           
 * 的路径名。我们会在内部修改它以保存子目录       
 * 和文件路径，退出前还原。 
 *
 * tzdirsub 指向 tzdir 中代表子文件名的那部分              
 * （即 tzdir 加上原目录名的长度，再加一个          
 * '/')。          
 *
 * tt 描述我们需要匹配的系统时区行为。          
 *
 * *bestscore 和 *bestzonename 进入时保存迄今                    
 * 为止的最佳分数和时区名。若发现更佳分数，则覆盖
 * 二者。bestzonename 须为 TZ_STRLEN_MAX + 1 的缓冲区。    
 */
static void
scan_available_timezones(char *tzdir, char *tzdirsub, struct tztry *tt,
						 int *bestscore, char *bestzonename)
{
	int			tzdir_orig_len = strlen(tzdir);
	char	  **names;
	char	  **namep;

	names = pgfnames(tzdir);
	if (!names)
		return;

	for (namep = names; *namep; namep++)
	{
		char	   *name = *namep;
		struct stat statbuf;

		/* 忽略 . 和 ..，以及其他"隐藏"文件   */
		if (name[0] == '.')
			continue;

		snprintf(tzdir + tzdir_orig_len, MAXPGPATH - tzdir_orig_len,
				 "/%s", name);

		if (stat(tzdir, &statbuf) != 0)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not stat \"%s\": %s\n",
					tzdir, strerror(errno));
#endif
			tzdir[tzdir_orig_len] = '\0';
			continue;
		}

		if (S_ISDIR(statbuf.st_mode))
		{
			/* 递归进入子目录     */
			scan_available_timezones(tzdir, tzdirsub, tt,
									 bestscore, bestzonename);
		}
		else
		{
			/* 加载并测试文件   */
			int			score = score_timezone(tzdirsub, tt);

			if (score > *bestscore)
			{
				*bestscore = score;
				strlcpy(bestzonename, tzdirsub, TZ_STRLEN_MAX + 1);
			}
			else if (score == *bestscore)
			{
				/* 考虑如何打破平局    */
				int			namepref = (zone_name_pref(tzdirsub) -
										zone_name_pref(bestzonename));

				if (namepref > 0 ||
					(namepref == 0 &&
					 (strlen(tzdirsub) < strlen(bestzonename) ||
					  (strlen(tzdirsub) == strlen(bestzonename) &&
					   strcmp(tzdirsub, bestzonename) < 0))))
					strlcpy(bestzonename, tzdirsub, TZ_STRLEN_MAX + 1);
			}
		}

		/* 恢复 tzdir  */
		tzdir[tzdir_orig_len] = '\0';
	}

	pgfnames_cleanup(names);
}
#else							/* WIN32 */

static const struct
{
	const char *stdname;		/* 标准时区的 Windows 名称    */
	const char *dstname;		/* 夏令时区的 Windows 名称    */
	const char *pgtzname;		/* 要映射到的 pgsql 时区名  */
}			win32_tzmap[] =

{
	/*
	 * 此列表根据以下注册表内容构建：           
	 * HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time
	 * Zones（Windows 7、Windows 10 和 Windows Server 2019）。         
	 * 最近的一些补充是通过比对 CLDR 项目的        
	 * windowsZones.xml。   
	 *
	 * 这些时区依据 CLDR 中"territory 001"的映射与               
	 * IANA 时区匹配。
	 */
	{
		/* (UTC+04:30) 喀布尔 */
		"Afghanistan Standard Time", "Afghanistan Daylight Time",
		"Asia/Kabul"
	},
	{
		/* (UTC-09:00) 阿拉斯加 */
		"Alaskan Standard Time", "Alaskan Daylight Time",
		"America/Anchorage"
	},
	{
		/* (UTC-10:00) 阿留申群岛  */
		"Aleutian Standard Time", "Aleutian Daylight Time",
		"America/Adak"
	},
	{
		/* (UTC+07:00) 巴尔瑙尔、戈尔诺-阿尔泰斯克 */
		"Altai Standard Time", "Altai Daylight Time",
		"Asia/Barnaul"
	},
	{
		/* (UTC+03:00) 科威特、利雅得 */
		"Arab Standard Time", "Arab Daylight Time",
		"Asia/Riyadh"
	},
	{
		/* (UTC+04:00) 阿布扎比、马斯喀特 */
		"Arabian Standard Time", "Arabian Daylight Time",
		"Asia/Dubai"
	},
	{
		/* (UTC+03:00) 巴格达 */
		"Arabic Standard Time", "Arabic Daylight Time",
		"Asia/Baghdad"
	},
	{
		/* (UTC-03:00) 布宜诺斯艾利斯市 */
		"Argentina Standard Time", "Argentina Daylight Time",
		"America/Buenos_Aires"
	},
	{
		/* (UTC+04:00) 巴库、第比利斯、埃里温 */
		"Armenian Standard Time", "Armenian Daylight Time",
		"Asia/Yerevan"
	},
	{
		/* (UTC+04:00) 阿斯特拉罕、乌里扬诺夫斯克 */
		"Astrakhan Standard Time", "Astrakhan Daylight Time",
		"Europe/Astrakhan"
	},
	{
		/* (UTC-04:00) 大西洋时间（加拿大） */
		"Atlantic Standard Time", "Atlantic Daylight Time",
		"America/Halifax"
	},
	{
		/* (UTC+09:30) 达尔文 */
		"AUS Central Standard Time", "AUS Central Daylight Time",
		"Australia/Darwin"
	},
	{
		/* (UTC+08:45) 尤克拉 */
		"Aus Central W. Standard Time", "Aus Central W. Daylight Time",
		"Australia/Eucla"
	},
	{
		/* (UTC+10:00) 堪培拉、墨尔本、悉尼 */
		"AUS Eastern Standard Time", "AUS Eastern Daylight Time",
		"Australia/Sydney"
	},
	{
		/* (UTC+04:00) 巴库 */
		"Azerbaijan Standard Time", "Azerbaijan Daylight Time",
		"Asia/Baku"
	},
	{
		/* (UTC-01:00) 亚速尔群岛 */
		"Azores Standard Time", "Azores Daylight Time",
		"Atlantic/Azores"
	},
	{
		/* (UTC-03:00) 萨尔瓦多 */
		"Bahia Standard Time", "Bahia Daylight Time",
		"America/Bahia"
	},
	{
		/* (UTC+06:00) 达卡 */
		"Bangladesh Standard Time", "Bangladesh Daylight Time",
		"Asia/Dhaka"
	},
	{
		/* (UTC+03:00) 明斯克 */
		"Belarus Standard Time", "Belarus Daylight Time",
		"Europe/Minsk"
	},
	{
		/* (UTC+11:00) 布干维尔岛     */
		"Bougainville Standard Time", "Bougainville Daylight Time",
		"Pacific/Bougainville"
	},
	{
		/* (UTC-01:00) 佛得角群岛 */
		"Cabo Verde Standard Time", "Cabo Verde Daylight Time",
		"Atlantic/Cape_Verde"
	},
	{
		/* (UTC-06:00) 萨斯喀彻温 */
		"Canada Central Standard Time", "Canada Central Daylight Time",
		"America/Regina"
	},
	{
		/* (UTC-01:00) 佛得角群岛 */
		"Cape Verde Standard Time", "Cape Verde Daylight Time",
		"Atlantic/Cape_Verde"
	},
	{
		/* (UTC+04:00) 埃里温 */
		"Caucasus Standard Time", "Caucasus Daylight Time",
		"Asia/Yerevan"
	},
	{
		/* (UTC+09:30) 阿德莱德 */
		"Cen. Australia Standard Time", "Cen. Australia Daylight Time",
		"Australia/Adelaide"
	},
	{
		/* (UTC-06:00) 中美洲       */
		"Central America Standard Time", "Central America Daylight Time",
		"America/Guatemala"
	},
	{
		/* (UTC+06:00) 阿斯塔纳 */
		"Central Asia Standard Time", "Central Asia Daylight Time",
		"Asia/Almaty"
	},
	{
		/* (UTC-04:00) 库亚巴 */
		"Central Brazilian Standard Time", "Central Brazilian Daylight Time",
		"America/Cuiaba"
	},
	{
		/* (UTC+01:00) 贝尔格莱德、布拉迪斯拉发、布达佩斯、卢布尔雅那、布拉格 */
		"Central Europe Standard Time", "Central Europe Daylight Time",
		"Europe/Budapest"
	},
	{
		/* (UTC+01:00) 萨拉热窝、斯科普里、华沙、萨格勒布 */
		"Central European Standard Time", "Central European Daylight Time",
		"Europe/Warsaw"
	},
	{
		/* (UTC+11:00) 所罗门群岛、新喀里多尼亚 */
		"Central Pacific Standard Time", "Central Pacific Daylight Time",
		"Pacific/Guadalcanal"
	},
	{
		/* (UTC-06:00) 中部时间（美国和加拿大） */
		"Central Standard Time", "Central Daylight Time",
		"America/Chicago"
	},
	{
		/* (UTC-06:00) 瓜达拉哈拉、墨西哥城、蒙特雷 */
		"Central Standard Time (Mexico)", "Central Daylight Time (Mexico)",
		"America/Mexico_City"
	},
	{
		/* (UTC+12:45) 查塔姆群岛 */
		"Chatham Islands Standard Time", "Chatham Islands Daylight Time",
		"Pacific/Chatham"
	},
	{
		/* (UTC+08:00) 北京、重庆、香港、乌鲁木齐 */
		"China Standard Time", "China Daylight Time",
		"Asia/Shanghai"
	},
	{
		/* (UTC) 协调世界时            */
		"Coordinated Universal Time", "Coordinated Universal Time",
		"UTC"
	},
	{
		/* (UTC-05:00) 哈瓦那 */
		"Cuba Standard Time", "Cuba Daylight Time",
		"America/Havana"
	},
	{
		/* (UTC-12:00) 国际日期变更线西     */
		"Dateline Standard Time", "Dateline Daylight Time",
		"Etc/GMT+12"
	},
	{
		/* (UTC+03:00) 内罗毕 */
		"E. Africa Standard Time", "E. Africa Daylight Time",
		"Africa/Nairobi"
	},
	{
		/* (UTC+10:00) 布里斯班 */
		"E. Australia Standard Time", "E. Australia Daylight Time",
		"Australia/Brisbane"
	},
	{
		/* (UTC+02:00) 基希讷乌 */
		"E. Europe Standard Time", "E. Europe Daylight Time",
		"Europe/Chisinau"
	},
	{
		/* (UTC-03:00) 巴西利亚 */
		"E. South America Standard Time", "E. South America Daylight Time",
		"America/Sao_Paulo"
	},
	{
		/* (UTC-06:00) 复活节岛  */
		"Easter Island Standard Time", "Easter Island Daylight Time",
		"Pacific/Easter"
	},
	{
		/* (UTC-05:00) 东部时间（美国和加拿大） */
		"Eastern Standard Time", "Eastern Daylight Time",
		"America/New_York"
	},
	{
		/* (UTC-05:00) 切图马尔 */
		"Eastern Standard Time (Mexico)", "Eastern Daylight Time (Mexico)",
		"America/Cancun"
	},
	{
		/* (UTC+02:00) 开罗 */
		"Egypt Standard Time", "Egypt Daylight Time",
		"Africa/Cairo"
	},
	{
		/* (UTC+05:00) 叶卡捷琳堡 */
		"Ekaterinburg Standard Time", "Ekaterinburg Daylight Time",
		"Asia/Yekaterinburg"
	},
	{
		/* (UTC+12:00) 斐济 */
		"Fiji Standard Time", "Fiji Daylight Time",
		"Pacific/Fiji"
	},
	{
		/* (UTC+02:00) 赫尔辛基、基辅、里加、索非亚、塔林、维尔纽斯 */
		"FLE Standard Time", "FLE Daylight Time",
		"Europe/Kiev"
	},
	{
		/* (UTC+04:00) 第比利斯 */
		"Georgian Standard Time", "Georgian Daylight Time",
		"Asia/Tbilisi"
	},
	{
		/* (UTC+00:00) 都柏林、爱丁堡、里斯本、伦敦 */
		"GMT Standard Time", "GMT Daylight Time",
		"Europe/London"
	},
	{
		/* (UTC-03:00) 格陵兰 */
		"Greenland Standard Time", "Greenland Daylight Time",
		"America/Godthab"
	},
	{
		/*
		 * Windows 在靠近本初子午线、但不在英国的多处    
		 * 地方使用此时区名称。然而多数人可能认为   
		 * "Greenwich" 指英国民用时间，甚至指纯粹的            
		 * UTC。Atlantic/Reykjavik 相当符合这种理解，           
		 * 因为冰岛自 1968 年起就未实行夏令时。           
		 */
		/* (UTC+00:00) 蒙罗维亚、雷克雅未克 */
		"Greenwich Standard Time", "Greenwich Daylight Time",
		"Atlantic/Reykjavik"
	},
	{
		/* (UTC+02:00) 雅典、布加勒斯特 */
		"GTB Standard Time", "GTB Daylight Time",
		"Europe/Bucharest"
	},
	{
		/* (UTC-05:00) 海地 */
		"Haiti Standard Time", "Haiti Daylight Time",
		"America/Port-au-Prince"
	},
	{
		/* (UTC-10:00) 夏威夷 */
		"Hawaiian Standard Time", "Hawaiian Daylight Time",
		"Pacific/Honolulu"
	},
	{
		/* (UTC+05:30) 金奈、加尔各答、孟买、新德里 */
		"India Standard Time", "India Daylight Time",
		"Asia/Calcutta"
	},
	{
		/* (UTC+03:30) 德黑兰 */
		"Iran Standard Time", "Iran Daylight Time",
		"Asia/Tehran"
	},
	{
		/* (UTC+02:00) 耶路撒冷 */
		"Israel Standard Time", "Israel Daylight Time",
		"Asia/Jerusalem"
	},
	{
		/* (UTC+02:00) 耶路撒冷（旧区域名拼法）  */
		"Jerusalem Standard Time", "Jerusalem Daylight Time",
		"Asia/Jerusalem"
	},
	{
		/* (UTC+02:00) 安曼 */
		"Jordan Standard Time", "Jordan Daylight Time",
		"Asia/Amman"
	},
	{
		/* (UTC+02:00) 加里宁格勒 */
		"Kaliningrad Standard Time", "Kaliningrad Daylight Time",
		"Europe/Kaliningrad"
	},
	{
		/* (UTC+12:00) 堪察加彼得罗巴甫洛夫斯克 - 旧 */
		"Kamchatka Standard Time", "Kamchatka Daylight Time",
		"Asia/Kamchatka"
	},
	{
		/* (UTC+09:00) 首尔 */
		"Korea Standard Time", "Korea Daylight Time",
		"Asia/Seoul"
	},
	{
		/* (UTC+02:00) 的黎波里 */
		"Libya Standard Time", "Libya Daylight Time",
		"Africa/Tripoli"
	},
	{
		/* (UTC+14:00) 基里蒂马蒂岛 */
		"Line Islands Standard Time", "Line Islands Daylight Time",
		"Pacific/Kiritimati"
	},
	{
		/* (UTC+10:30) 豪勋爵岛     */
		"Lord Howe Standard Time", "Lord Howe Daylight Time",
		"Australia/Lord_Howe"
	},
	{
		/* (UTC+11:00) 马加丹 */
		"Magadan Standard Time", "Magadan Daylight Time",
		"Asia/Magadan"
	},
	{
		/* (UTC-03:00) 蓬塔阿雷纳斯 */
		"Magallanes Standard Time", "Magallanes Daylight Time",
		"America/Punta_Arenas"
	},
	{
		/* (UTC+08:00) 吉隆坡，新加坡   */
		"Malay Peninsula Standard Time", "Malay Peninsula Daylight Time",
		"Asia/Kuala_Lumpur"
	},
	{
		/* (UTC-09:30) 马克萨斯群岛 */
		"Marquesas Standard Time", "Marquesas Daylight Time",
		"Pacific/Marquesas"
	},
	{
		/* (UTC+04:00) 路易港  */
		"Mauritius Standard Time", "Mauritius Daylight Time",
		"Indian/Mauritius"
	},
	{
		/* (UTC-06:00) 瓜达拉哈拉、墨西哥城、蒙特雷 */
		"Mexico Standard Time", "Mexico Daylight Time",
		"America/Mexico_City"
	},
	{
		/* (UTC-07:00) 奇瓦瓦、拉巴斯、马萨特兰 */
		"Mexico Standard Time 2", "Mexico Daylight Time 2",
		"America/Chihuahua"
	},
	{
		/* (UTC-02:00) 中大西洋 - 旧 */
		"Mid-Atlantic Standard Time", "Mid-Atlantic Daylight Time",
		"Atlantic/South_Georgia"
	},
	{
		/* (UTC+02:00) 贝鲁特 */
		"Middle East Standard Time", "Middle East Daylight Time",
		"Asia/Beirut"
	},
	{
		/* (UTC-03:00) 蒙得维的亚 */
		"Montevideo Standard Time", "Montevideo Daylight Time",
		"America/Montevideo"
	},
	{
		/* (UTC+01:00) 卡萨布兰卡 */
		"Morocco Standard Time", "Morocco Daylight Time",
		"Africa/Casablanca"
	},
	{
		/* (UTC-07:00) 山地时间（美国和加拿大） */
		"Mountain Standard Time", "Mountain Daylight Time",
		"America/Denver"
	},
	{
		/* (UTC-07:00) 奇瓦瓦、拉巴斯、马萨特兰 */
		"Mountain Standard Time (Mexico)", "Mountain Daylight Time (Mexico)",
		"America/Chihuahua"
	},
	{
		/* (UTC+06:30) 仰光           */
		"Myanmar Standard Time", "Myanmar Daylight Time",
		"Asia/Rangoon"
	},
	{
		/* (UTC+07:00) 新西伯利亚 */
		"N. Central Asia Standard Time", "N. Central Asia Daylight Time",
		"Asia/Novosibirsk"
	},
	{
		/* (UTC+02:00) 温得和克 */
		"Namibia Standard Time", "Namibia Daylight Time",
		"Africa/Windhoek"
	},
	{
		/* (UTC+05:45) 加德满都 */
		"Nepal Standard Time", "Nepal Daylight Time",
		"Asia/Katmandu"
	},
	{
		/* (UTC+12:00) 奥克兰、惠灵顿 */
		"New Zealand Standard Time", "New Zealand Daylight Time",
		"Pacific/Auckland"
	},
	{
		/* (UTC-03:30) 纽芬兰    */
		"Newfoundland Standard Time", "Newfoundland Daylight Time",
		"America/St_Johns"
	},
	{
		/* (UTC+11:00) 诺福克岛   */
		"Norfolk Standard Time", "Norfolk Daylight Time",
		"Pacific/Norfolk"
	},
	{
		/* (UTC+08:00) 伊尔库茨克 */
		"North Asia East Standard Time", "North Asia East Daylight Time",
		"Asia/Irkutsk"
	},
	{
		/* (UTC+07:00) 克拉斯诺亚尔斯克 */
		"North Asia Standard Time", "North Asia Daylight Time",
		"Asia/Krasnoyarsk"
	},
	{
		/* (UTC+09:00) 平壤    */
		"North Korea Standard Time", "North Korea Daylight Time",
		"Asia/Pyongyang"
	},
	{
		/* (UTC+07:00) 新西伯利亚 */
		"Novosibirsk Standard Time", "Novosibirsk Daylight Time",
		"Asia/Novosibirsk"
	},
	{
		/* (UTC+06:00) 鄂木斯克 */
		"Omsk Standard Time", "Omsk Daylight Time",
		"Asia/Omsk"
	},
	{
		/* (UTC-04:00) 圣地亚哥 */
		"Pacific SA Standard Time", "Pacific SA Daylight Time",
		"America/Santiago"
	},
	{
		/* (UTC-08:00) 太平洋时间（美国和加拿大） */
		"Pacific Standard Time", "Pacific Daylight Time",
		"America/Los_Angeles"
	},
	{
		/* (UTC-08:00) 下加利福尼亚 */
		"Pacific Standard Time (Mexico)", "Pacific Daylight Time (Mexico)",
		"America/Tijuana"
	},
	{
		/* (UTC+05:00) 伊斯兰堡、卡拉奇 */
		"Pakistan Standard Time", "Pakistan Daylight Time",
		"Asia/Karachi"
	},
	{
		/* (UTC-04:00) 亚松森 */
		"Paraguay Standard Time", "Paraguay Daylight Time",
		"America/Asuncion"
	},
	{
		/* (UTC+05:00) 克孜洛达 */
		"Qyzylorda Standard Time", "Qyzylorda Daylight Time",
		"Asia/Qyzylorda"
	},
	{
		/* (UTC+01:00) 布鲁塞尔、哥本哈根、马德里、巴黎 */
		"Romance Standard Time", "Romance Daylight Time",
		"Europe/Paris"
	},
	{
		/* (UTC+04:00) 伊热夫斯克、萨马拉 */
		"Russia Time Zone 3", "Russia Time Zone 3",
		"Europe/Samara"
	},
	{
		/* (UTC+11:00) 乔库尔达赫 */
		"Russia Time Zone 10", "Russia Time Zone 10",
		"Asia/Srednekolymsk"
	},
	{
		/* (UTC+12:00) 阿纳德尔、堪察加彼得罗巴甫洛夫斯克 */
		"Russia Time Zone 11", "Russia Time Zone 11",
		"Asia/Kamchatka"
	},
	{
		/* (UTC+02:00) 加里宁格勒 */
		"Russia TZ 1 Standard Time", "Russia TZ 1 Daylight Time",
		"Europe/Kaliningrad"
	},
	{
		/* (UTC+03:00) 莫斯科、圣彼得堡 */
		"Russia TZ 2 Standard Time", "Russia TZ 2 Daylight Time",
		"Europe/Moscow"
	},
	{
		/* (UTC+04:00) 伊热夫斯克、萨马拉 */
		"Russia TZ 3 Standard Time", "Russia TZ 3 Daylight Time",
		"Europe/Samara"
	},
	{
		/* (UTC+05:00) 叶卡捷琳堡 */
		"Russia TZ 4 Standard Time", "Russia TZ 4 Daylight Time",
		"Asia/Yekaterinburg"
	},
	{
		/* (UTC+06:00) 新西伯利亚（RTZ 5） */
		"Russia TZ 5 Standard Time", "Russia TZ 5 Daylight Time",
		"Asia/Novosibirsk"
	},
	{
		/* (UTC+07:00) 克拉斯诺亚尔斯克 */
		"Russia TZ 6 Standard Time", "Russia TZ 6 Daylight Time",
		"Asia/Krasnoyarsk"
	},
	{
		/* (UTC+08:00) 伊尔库茨克 */
		"Russia TZ 7 Standard Time", "Russia TZ 7 Daylight Time",
		"Asia/Irkutsk"
	},
	{
		/* (UTC+09:00) 雅库茨克 */
		"Russia TZ 8 Standard Time", "Russia TZ 8 Daylight Time",
		"Asia/Yakutsk"
	},
	{
		/* (UTC+10:00) 符拉迪沃斯托克 */
		"Russia TZ 9 Standard Time", "Russia TZ 9 Daylight Time",
		"Asia/Vladivostok"
	},
	{
		/* (UTC+11:00) 乔库尔达赫 */
		"Russia TZ 10 Standard Time", "Russia TZ 10 Daylight Time",
		"Asia/Magadan"
	},
	{
		/* (UTC+12:00) 阿纳德尔、堪察加彼得罗巴甫洛夫斯克 */
		"Russia TZ 11 Standard Time", "Russia TZ 11 Daylight Time",
		"Asia/Anadyr"
	},
	{
		/* (UTC+03:00) 莫斯科、圣彼得堡 */
		"Russian Standard Time", "Russian Daylight Time",
		"Europe/Moscow"
	},
	{
		/* (UTC-03:00) 卡宴、福塔莱萨 */
		"SA Eastern Standard Time", "SA Eastern Daylight Time",
		"America/Cayenne"
	},
	{
		/* (UTC-05:00) 波哥大、利马、基多、里奥布朗库 */
		"SA Pacific Standard Time", "SA Pacific Daylight Time",
		"America/Bogota"
	},
	{
		/* (UTC-04:00) 乔治敦、拉巴斯、玛瑙斯、圣胡安 */
		"SA Western Standard Time", "SA Western Daylight Time",
		"America/La_Paz"
	},
	{
		/* (UTC-03:00) 圣皮埃尔和密克隆  */
		"Saint Pierre Standard Time", "Saint Pierre Daylight Time",
		"America/Miquelon"
	},
	{
		/* (UTC+11:00) 萨哈林 */
		"Sakhalin Standard Time", "Sakhalin Daylight Time",
		"Asia/Sakhalin"
	},
	{
		/* (UTC+13:00) 萨摩亚 */
		"Samoa Standard Time", "Samoa Daylight Time",
		"Pacific/Apia"
	},
	{
		/* (UTC+00:00) 圣多美 */
		"Sao Tome Standard Time", "Sao Tome Daylight Time",
		"Africa/Sao_Tome"
	},
	{
		/* (UTC+04:00) 萨拉托夫 */
		"Saratov Standard Time", "Saratov Daylight Time",
		"Europe/Saratov"
	},
	{
		/* (UTC+07:00) 曼谷、河内、雅加达 */
		"SE Asia Standard Time", "SE Asia Daylight Time",
		"Asia/Bangkok"
	},
	{
		/* (UTC+08:00) 吉隆坡，新加坡   */
		"Singapore Standard Time", "Singapore Daylight Time",
		"Asia/Singapore"
	},
	{
		/* (UTC+02:00) 哈拉雷、比勒陀利亚 */
		"South Africa Standard Time", "South Africa Daylight Time",
		"Africa/Johannesburg"
	},
	{
		/* (UTC+02:00) 朱巴 */
		"South Sudan Standard Time", "South Sudan Daylight Time",
		"Africa/Juba"
	},
	{
		/* (UTC+05:30) 斯里贾亚瓦德纳普拉 */
		"Sri Lanka Standard Time", "Sri Lanka Daylight Time",
		"Asia/Colombo"
	},
	{
		/* (UTC+02:00) 喀土穆 */
		"Sudan Standard Time", "Sudan Daylight Time",
		"Africa/Khartoum"
	},
	{
		/* (UTC+02:00) 大马士革 */
		"Syria Standard Time", "Syria Daylight Time",
		"Asia/Damascus"
	},
	{
		/* (UTC+08:00) 台北 */
		"Taipei Standard Time", "Taipei Daylight Time",
		"Asia/Taipei"
	},
	{
		/* (UTC+10:00) 霍巴特 */
		"Tasmania Standard Time", "Tasmania Daylight Time",
		"Australia/Hobart"
	},
	{
		/* (UTC-03:00) 阿拉瓜伊纳 */
		"Tocantins Standard Time", "Tocantins Daylight Time",
		"America/Araguaina"
	},
	{
		/* (UTC+09:00) 大阪、札幌、东京 */
		"Tokyo Standard Time", "Tokyo Daylight Time",
		"Asia/Tokyo"
	},
	{
		/* (UTC+07:00) 托木斯克 */
		"Tomsk Standard Time", "Tomsk Daylight Time",
		"Asia/Tomsk"
	},
	{
		/* (UTC+13:00) 努库阿洛法 */
		"Tonga Standard Time", "Tonga Daylight Time",
		"Pacific/Tongatapu"
	},
	{
		/* (UTC+09:00) 赤塔 */
		"Transbaikal Standard Time", "Transbaikal Daylight Time",
		"Asia/Chita"
	},
	{
		/* (UTC+03:00) 伊斯坦布尔 */
		"Turkey Standard Time", "Turkey Daylight Time",
		"Europe/Istanbul"
	},
	{
		/* (UTC-05:00) 特克斯和凯科斯群岛 */
		"Turks And Caicos Standard Time", "Turks And Caicos Daylight Time",
		"America/Grand_Turk"
	},
	{
		/* (UTC+08:00) 乌兰巴托 */
		"Ulaanbaatar Standard Time", "Ulaanbaatar Daylight Time",
		"Asia/Ulaanbaatar"
	},
	{
		/* (UTC-05:00) 印第安纳（东部） */
		"US Eastern Standard Time", "US Eastern Daylight Time",
		"America/Indianapolis"
	},
	{
		/* (UTC-07:00) 亚利桑那 */
		"US Mountain Standard Time", "US Mountain Daylight Time",
		"America/Phoenix"
	},
	{
		/* (UTC) 协调世界时            */
		"UTC", "UTC",
		"UTC"
	},
	{
		/* (UTC+12:00) 协调世界时+12            */
		"UTC+12", "UTC+12",
		"Etc/GMT-12"
	},
	{
		/* (UTC+13:00) 协调世界时+13            */
		"UTC+13", "UTC+13",
		"Etc/GMT-13"
	},
	{
		/* (UTC-02:00) 协调世界时-02            */
		"UTC-02", "UTC-02",
		"Etc/GMT+2"
	},
	{
		/* (UTC-08:00) 协调世界时-08            */
		"UTC-08", "UTC-08",
		"Etc/GMT+8"
	},
	{
		/* (UTC-09:00) 协调世界时-09            */
		"UTC-09", "UTC-09",
		"Etc/GMT+9"
	},
	{
		/* (UTC-11:00) 协调世界时-11            */
		"UTC-11", "UTC-11",
		"Etc/GMT+11"
	},
	{
		/* (UTC-04:00) 加拉加斯 */
		"Venezuela Standard Time", "Venezuela Daylight Time",
		"America/Caracas"
	},
	{
		/* (UTC+10:00) 符拉迪沃斯托克 */
		"Vladivostok Standard Time", "Vladivostok Daylight Time",
		"Asia/Vladivostok"
	},
	{
		/* (UTC+04:00) 伏尔加格勒 */
		"Volgograd Standard Time", "Volgograd Daylight Time",
		"Europe/Volgograd"
	},
	{
		/* (UTC+08:00) 珀斯 */
		"W. Australia Standard Time", "W. Australia Daylight Time",
		"Australia/Perth"
	},
	{
		/* (UTC+01:00) 中西非           */
		"W. Central Africa Standard Time", "W. Central Africa Daylight Time",
		"Africa/Lagos"
	},
	{
		/* (UTC+01:00) 阿姆斯特丹、柏林、伯尔尼、罗马、斯德哥尔摩、维也纳 */
		"W. Europe Standard Time", "W. Europe Daylight Time",
		"Europe/Berlin"
	},
	{
		/* (UTC+07:00) 科布多 */
		"W. Mongolia Standard Time", "W. Mongolia Daylight Time",
		"Asia/Hovd"
	},
	{
		/* (UTC+05:00) 阿什哈巴德、塔什干 */
		"West Asia Standard Time", "West Asia Daylight Time",
		"Asia/Tashkent"
	},
	{
		/* (UTC+02:00) 加沙、希伯伦 */
		"West Bank Gaza Standard Time", "West Bank Gaza Daylight Time",
		"Asia/Gaza"
	},
	{
		/* (UTC+02:00) 加沙、希伯伦 */
		"West Bank Standard Time", "West Bank Daylight Time",
		"Asia/Hebron"
	},
	{
		/* (UTC+10:00) 关岛、莫尔兹比港 */
		"West Pacific Standard Time", "West Pacific Daylight Time",
		"Pacific/Port_Moresby"
	},
	{
		/* (UTC+09:00) 雅库茨克 */
		"Yakutsk Standard Time", "Yakutsk Daylight Time",
		"Asia/Yakutsk"
	},
	{
		/* (UTC-07:00) 育空 */
		"Yukon Standard Time", "Yukon Daylight Time",
		"America/Whitehorse"
	},
	{
		NULL, NULL, NULL
	}
};

static const char *
identify_system_timezone(void)
{
	int			i;
	char		tzname[128];
	char		localtzname[256];
	time_t		t = time(NULL);
	struct tm  *tm = localtime(&t);
	HKEY		rootKey;
	int			idx;

	if (!tm)
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "could not identify system time zone: localtime() failed\n");
#endif
		return NULL;			/* 去 GMT   */
	}

	memset(tzname, 0, sizeof(tzname));
	strftime(tzname, sizeof(tzname) - 1, "%Z", tm);

	for (i = 0; win32_tzmap[i].stdname != NULL; i++)
	{
		if (strcmp(tzname, win32_tzmap[i].stdname) == 0 ||
			strcmp(tzname, win32_tzmap[i].dstname) == 0)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "TZ \"%s\" matches system time zone \"%s\"\n",
					win32_tzmap[i].pgtzname, tzname);
#endif
			return win32_tzmap[i].pgtzname;
		}
	}

	/*
	 * 本地化的 Windows 版本返回本地化的时区名称。       
	 * 扫描注册表以找到英文名称，然后再次尝试与     
	 * 我们的表匹配。   
	 */
	memset(localtzname, 0, sizeof(localtzname));
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
					 "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
					 0,
					 KEY_READ,
					 &rootKey) != ERROR_SUCCESS)
	{
#ifdef DEBUG_IDENTIFY_TIMEZONE
		fprintf(stderr, "could not open registry key to identify system time zone: error code %lu\n",
				GetLastError());
#endif
		return NULL;			/* 去 GMT   */
	}

	for (idx = 0;; idx++)
	{
		char		keyname[256];
		char		zonename[256];
		DWORD		namesize;
		FILETIME	lastwrite;
		HKEY		key;
		LONG		r;

		memset(keyname, 0, sizeof(keyname));
		namesize = sizeof(keyname);
		if ((r = RegEnumKeyEx(rootKey,
							  idx,
							  keyname,
							  &namesize,
							  NULL,
							  NULL,
							  NULL,
							  &lastwrite)) != ERROR_SUCCESS)
		{
			if (r == ERROR_NO_MORE_ITEMS)
				break;
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not enumerate registry subkeys to identify system time zone: %d\n",
					(int) r);
#endif
			break;
		}

		if ((r = RegOpenKeyEx(rootKey, keyname, 0, KEY_READ, &key)) != ERROR_SUCCESS)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not open registry subkey to identify system time zone: %d\n",
					(int) r);
#endif
			break;
		}

		memset(zonename, 0, sizeof(zonename));
		namesize = sizeof(zonename);
		if ((r = RegQueryValueEx(key, "Std", NULL, NULL, (unsigned char *) zonename, &namesize)) != ERROR_SUCCESS)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not query value for key \"std\" to identify system time zone \"%s\": %d\n",
					keyname, (int) r);
#endif
			RegCloseKey(key);
			continue;			/* 继续查看下一个时区          */
		}
		if (strcmp(tzname, zonename) == 0)
		{
			/* 匹配时区 */
			strcpy(localtzname, keyname);
			RegCloseKey(key);
			break;
		}
		memset(zonename, 0, sizeof(zonename));
		namesize = sizeof(zonename);
		if ((r = RegQueryValueEx(key, "Dlt", NULL, NULL, (unsigned char *) zonename, &namesize)) != ERROR_SUCCESS)
		{
#ifdef DEBUG_IDENTIFY_TIMEZONE
			fprintf(stderr, "could not query value for key \"dlt\" to identify system time zone \"%s\": %d\n",
					keyname, (int) r);
#endif
			RegCloseKey(key);
			continue;			/* 继续查看下一个时区          */
		}
		if (strcmp(tzname, zonename) == 0)
		{
			/* 匹配 DST 区   */
			strcpy(localtzname, keyname);
			RegCloseKey(key);
			break;
		}

		RegCloseKey(key);
	}

	RegCloseKey(rootKey);

	if (localtzname[0])
	{
		/* 找到了本地化名称，也扫描那个       */
		for (i = 0; win32_tzmap[i].stdname != NULL; i++)
		{
			if (strcmp(localtzname, win32_tzmap[i].stdname) == 0 ||
				strcmp(localtzname, win32_tzmap[i].dstname) == 0)
			{
#ifdef DEBUG_IDENTIFY_TIMEZONE
				fprintf(stderr, "TZ \"%s\" matches localized system time zone \"%s\" (\"%s\")\n",
						win32_tzmap[i].pgtzname, tzname, localtzname);
#endif
				return win32_tzmap[i].pgtzname;
			}
		}
	}

#ifdef DEBUG_IDENTIFY_TIMEZONE
	fprintf(stderr, "could not find a match for system time zone \"%s\"\n",
			tzname);
#endif
	return NULL;				/* 去 GMT   */
}
#endif							/* WIN32 */


/*
 * 若给定的时区名有效且是"可接受"的时区，则返回真。 
 */
static bool
validate_zone(const char *tzname)
{
	pg_tz	   *tz;

	if (!tzname || !tzname[0])
		return false;

	tz = pg_load_tz(tzname);
	if (!tz)
		return false;

	if (!pg_tz_acceptable(tz))
		return false;

	return true;
}

/*
 * 根据环境识别一个合适的默认时区设置。                
 *
 * 必须传入安装的 share_path，因为那是时区数据          
 * 库目录的默认位置。                  
 *
 * 我们首先查看 TZ 环境变量。如果找不到，或者     
 * 我们自己的代码无法识别，就尝试根据系统时区   
 * 库的行为来识别时区。当一切都失败时，返回           
 * NULL，表示我们应回退到 GMT。                
 */
const char *
select_default_timezone(const char *share_path)
{
	const char *tzname;

	/* 如果需要，初始化时区目录路径    */
#ifndef SYSTEMTZDIR
	snprintf(tzdirpath, sizeof(tzdirpath), "%s/timezone", share_path);
#endif

	/* 检查 TZ 环境变量        */
	tzname = getenv("TZ");
	if (validate_zone(tzname))
		return tzname;

	/* 不行，所以尝试识别系统时区      */
	tzname = identify_system_timezone();
	if (validate_zone(tzname))
		return tzname;

	return NULL;
}
