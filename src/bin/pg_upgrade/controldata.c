/*
 *	controldata.c
 *
 *	controldata functions
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/controldata.c
 */

#include "postgres_fe.h"

#include <ctype.h>

#include "pg_upgrade.h"
#include "common/string.h"


/*
 * get_control_data()
 *
 * 获取 "ctrl" 中的 pg_control 信息。假定 bindir        
 * 和 datadir 分别为 postgresql bin 与 pgdata 目录       
 * 的有效绝对路径，*且* pg_resetwal 与 datadir 版本    
 * 兼容。本函数的主要目的是以版本无关的方式        
 * 获取 pg_control 数据。          
 *
 * 此处采用的方法是调用带 -n 选项的 pg_resetwal，  
 * 然后读取其管道输出。经少量字符串解析         
 * 即可获得 pg_control 数据。服务器运行时无法运行        
 * pg_resetwal，因此我们使用 pg_controldata；                      
 * 后者不提供实际执行升级所需的全部字段，          
 * 但对检查模式已足够。我们不实现 pg_resetwal -n，      
 * 因难以对运行中服务器返回 xid。
 */
void
get_control_data(ClusterInfo *cluster, bool live_check)
{
	char		cmd[MAXPGPATH];
	char		bufin[MAX_STRING];
	FILE	   *output;
	char	   *p;
	bool		got_tli = false;
	bool		got_log_id = false;
	bool		got_log_seg = false;
	bool		got_xid = false;
	bool		got_oid = false;
	bool		got_multi = false;
	bool		got_oldestmulti = false;
	bool		got_oldestxid = false;
	bool		got_mxoff = false;
	bool		got_nextxlogfile = false;
	bool		got_float8_pass_by_value = false;
	bool		got_align = false;
	bool		got_blocksz = false;
	bool		got_largesz = false;
	bool		got_walsz = false;
	bool		got_walseg = false;
	bool		got_ident = false;
	bool		got_index = false;
	bool		got_toast = false;
	bool		got_large_object = false;
	bool		got_date_is_int = false;
	bool		got_data_checksum_version = false;
	bool		got_cluster_state = false;
	char	   *lc_collate = NULL;
	char	   *lc_ctype = NULL;
	char	   *lc_monetary = NULL;
	char	   *lc_numeric = NULL;
	char	   *lc_time = NULL;
	char	   *lang = NULL;
	char	   *language = NULL;
	char	   *lc_all = NULL;
	char	   *lc_messages = NULL;
	uint32		tli = 0;
	uint32		logid = 0;
	uint32		segno = 0;
	char	   *resetwal_bin;
	int			rc;

	/*
	 * 将 pg_resetwal 输出作字符串测试，须为英文。
	 * 复制自 pg_regress.c。
	                   */
	if (getenv("LC_COLLATE"))
		lc_collate = pg_strdup(getenv("LC_COLLATE"));
	if (getenv("LC_CTYPE"))
		lc_ctype = pg_strdup(getenv("LC_CTYPE"));
	if (getenv("LC_MONETARY"))
		lc_monetary = pg_strdup(getenv("LC_MONETARY"));
	if (getenv("LC_NUMERIC"))
		lc_numeric = pg_strdup(getenv("LC_NUMERIC"));
	if (getenv("LC_TIME"))
		lc_time = pg_strdup(getenv("LC_TIME"));
	if (getenv("LANG"))
		lang = pg_strdup(getenv("LANG"));
	if (getenv("LANGUAGE"))
		language = pg_strdup(getenv("LANGUAGE"));
	if (getenv("LC_ALL"))
		lc_all = pg_strdup(getenv("LC_ALL"));
	if (getenv("LC_MESSAGES"))
		lc_messages = pg_strdup(getenv("LC_MESSAGES"));

	unsetenv("LC_COLLATE");
	unsetenv("LC_CTYPE");
	unsetenv("LC_MONETARY");
	unsetenv("LC_NUMERIC");
	unsetenv("LC_TIME");
#ifndef WIN32
	unsetenv("LANG");
#else
	/* Windows 默认区域可非英文，故强制 en               */
	setenv("LANG", "en", 1);
#endif
	unsetenv("LANGUAGE");
	unsetenv("LC_ALL");
	setenv("LC_MESSAGES", "C", 1);

	/*
	 * 检查干净关闭
	       */
	if (!live_check || cluster == &new_cluster)
	{
		/* 仅 pg_controldata 输出集簇状态         */
		snprintf(cmd, sizeof(cmd), "\"%s/pg_controldata\" \"%s\"",
				 cluster->bindir, cluster->pgdata);
		fflush(NULL);

		if ((output = popen(cmd, "r")) == NULL)
			pg_fatal("could not get control data using %s: %s",
					 cmd, strerror(errno));

		/* cmd 结果在 output 中，逐行解析                             */
		while (fgets(bufin, sizeof(bufin), output))
		{
			if ((p = strstr(bufin, "Database cluster state:")) != NULL)
			{
				p = strchr(p, ':');

				if (p == NULL || strlen(p) <= 1)
					pg_fatal("%d: database cluster state problem", __LINE__);

				p++;			/* 去':'符       */

				/*
				 * 此前已查过 postmaster 锁文件，若找到        
				 * 则已尝试启动/停止服务器以重放 WAL。        
				 * 但 pg_ctl -m immediate 不留锁文件                
				 * 却需要 WAL 重放，故此处从 controldata         
				 * 视角检查服务器是否已干净关闭。         
				 *             
				 */
				/* 去除前导空格    */
				while (*p == ' ')
					p++;
				if (strcmp(p, "shut down in recovery\n") == 0)
				{
					if (cluster == &old_cluster)
						pg_fatal("The source cluster was shut down while in recovery mode.  To upgrade, use \"rsync\" as documented or shut it down as a primary.");
					else
						pg_fatal("The target cluster was shut down while in recovery mode.  To upgrade, use \"rsync\" as documented or shut it down as a primary.");
				}
				else if (strcmp(p, "shut down\n") != 0)
				{
					if (cluster == &old_cluster)
						pg_fatal("The source cluster was not shut down cleanly.");
					else
						pg_fatal("The target cluster was not shut down cleanly.");
				}
				got_cluster_state = true;
			}
		}

		rc = pclose(output);
		if (rc != 0)
			pg_fatal("could not get control data using %s: %s",
					 cmd, wait_result_to_str(rc));

		if (!got_cluster_state)
		{
			if (cluster == &old_cluster)
				pg_fatal("The source cluster lacks cluster state information:");
			else
				pg_fatal("The target cluster lacks cluster state information:");
		}
	}

	/* pg_resetxlog 在 v10 重命名为 pg_resetwal              */
	if (GET_MAJOR_VERSION(cluster->bin_version) <= 906)
		resetwal_bin = "pg_resetxlog\" -n";
	else
		resetwal_bin = "pg_resetwal\" -n";
	snprintf(cmd, sizeof(cmd), "\"%s/%s \"%s\"",
			 cluster->bindir,
			 live_check ? "pg_controldata\"" : resetwal_bin,
			 cluster->pgdata);
	fflush(NULL);

	if ((output = popen(cmd, "r")) == NULL)
		pg_fatal("could not get control data using %s: %s",
				 cmd, strerror(errno));

	/* 仅 <= 9.2     */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 902)
	{
		cluster->controldata.data_checksum_version = 0;
		got_data_checksum_version = true;
	}

	/* cmd 结果在 output 中，逐行解析                             */
	while (fgets(bufin, sizeof(bufin), output))
	{
		/* verbose 模式记录每行     */
		pg_strip_crlf(bufin);
		pg_log(PG_VERBOSE, "%s", bufin);

		if ((p = strstr(bufin, "pg_control version number:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: pg_resetwal problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.ctrl_ver = str2uint(p);
		}
		else if ((p = strstr(bufin, "Catalog version number:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.cat_ver = str2uint(p);
		}
		else if ((p = strstr(bufin, "Latest checkpoint's TimeLineID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			tli = str2uint(p);
			got_tli = true;
		}
		else if ((p = strstr(bufin, "First log file ID after reset:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			logid = str2uint(p);
			got_log_id = true;
		}
		else if ((p = strstr(bufin, "First log file segment after reset:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			segno = str2uint(p);
			got_log_seg = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextXID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.chkpnt_nxtepoch = str2uint(p);

			/*
			 * 9.6 中分隔符从 '/' 改为 ':'。我们不检测        
			 * 系统目录版本的变更，因为系统目录版本同样             
			 * 取自 pg_controldata，为此添加顺序                     
			 * 依赖并不值得 --- 我们仅检查字符串。     
			 */
			if (strchr(p, '/') != NULL)
				p = strchr(p, '/');
			else if (GET_MAJOR_VERSION(cluster->major_version) >= 906)
				p = strchr(p, ':');
			else
				p = NULL;

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去'/'或':'符        */
			cluster->controldata.chkpnt_nxtxid = str2uint(p);
			got_xid = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextOID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.chkpnt_nxtoid = str2uint(p);
			got_oid = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextMultiXactId:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.chkpnt_nxtmulti = str2uint(p);
			got_multi = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's oldestXID:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.chkpnt_oldstxid = str2uint(p);
			got_oldestxid = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's oldestMultiXid:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.chkpnt_oldstMulti = str2uint(p);
			got_oldestmulti = true;
		}
		else if ((p = strstr(bufin, "Latest checkpoint's NextMultiOffset:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.chkpnt_nxtmxoff = str2uint(p);
			got_mxoff = true;
		}
		else if ((p = strstr(bufin, "First log segment after reset:")) != NULL)
		{
			/* 跳过冒号及后空白                   */
			p = strchr(p, ':');
			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);
			p = strpbrk(p, "01234567890ABCDEF");
			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			/* 确保像有效 WAL 文件名                 */
			if (strspn(p, "0123456789ABCDEF") != 24)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			strlcpy(cluster->controldata.nextxlogfile, p, 25);
			got_nextxlogfile = true;
		}
		else if ((p = strstr(bufin, "Float8 argument passing:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			/* 后用于 contrib 检查     */
			cluster->controldata.float8_pass_by_value = strstr(p, "by value") != NULL;
			got_float8_pass_by_value = true;
		}
		else if ((p = strstr(bufin, "Maximum data alignment:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.align = str2uint(p);
			got_align = true;
		}
		else if ((p = strstr(bufin, "Database block size:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.blocksz = str2uint(p);
			got_blocksz = true;
		}
		else if ((p = strstr(bufin, "Blocks per segment of large relation:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.largesz = str2uint(p);
			got_largesz = true;
		}
		else if ((p = strstr(bufin, "WAL block size:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.walsz = str2uint(p);
			got_walsz = true;
		}
		else if ((p = strstr(bufin, "Bytes per WAL segment:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.walseg = str2uint(p);
			got_walseg = true;
		}
		else if ((p = strstr(bufin, "Maximum length of identifiers:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.ident = str2uint(p);
			got_ident = true;
		}
		else if ((p = strstr(bufin, "Maximum columns in an index:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.index = str2uint(p);
			got_index = true;
		}
		else if ((p = strstr(bufin, "Maximum size of a TOAST chunk:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.toast = str2uint(p);
			got_toast = true;
		}
		else if ((p = strstr(bufin, "Size of a large-object chunk:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.large_object = str2uint(p);
			got_large_object = true;
		}
		else if ((p = strstr(bufin, "Date/time type storage:")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.date_is_int = strstr(p, "64-bit integers") != NULL;
			got_date_is_int = true;
		}
		else if ((p = strstr(bufin, "checksum")) != NULL)
		{
			p = strchr(p, ':');

			if (p == NULL || strlen(p) <= 1)
				pg_fatal("%d: controldata retrieval problem", __LINE__);

			p++;				/* 去':'符       */
			cluster->controldata.data_checksum_version = str2uint(p);
			got_data_checksum_version = true;
		}
	}

	rc = pclose(output);
	if (rc != 0)
		pg_fatal("could not get control data using %s: %s",
				 cmd, wait_result_to_str(rc));

	/*
	 * 恢复环境变量。除 LANG 与 LC_MESSAGES
	 * 外均已在上文取消。
	           */
	if (lc_collate)
		setenv("LC_COLLATE", lc_collate, 1);
	if (lc_ctype)
		setenv("LC_CTYPE", lc_ctype, 1);
	if (lc_monetary)
		setenv("LC_MONETARY", lc_monetary, 1);
	if (lc_numeric)
		setenv("LC_NUMERIC", lc_numeric, 1);
	if (lc_time)
		setenv("LC_TIME", lc_time, 1);
	if (lang)
		setenv("LANG", lang, 1);
	else
		unsetenv("LANG");
	if (language)
		setenv("LANGUAGE", language, 1);
	if (lc_all)
		setenv("LC_ALL", lc_all, 1);
	if (lc_messages)
		setenv("LC_MESSAGES", lc_messages, 1);
	else
		unsetenv("LC_MESSAGES");

	pg_free(lc_collate);
	pg_free(lc_ctype);
	pg_free(lc_monetary);
	pg_free(lc_numeric);
	pg_free(lc_time);
	pg_free(lang);
	pg_free(language);
	pg_free(lc_all);
	pg_free(lc_messages);

	/*
	 * 9.3 前 pg_resetwal 将重置后首日志的 xlogid
	 * 和 segno 分列。9.3 起报告 WAL 文件名。
	 * 若旧集簇早于 9.3，从 xlogid 和 segno 构造
	 * WAL 文件名。
	                                                                                 */
	if (GET_MAJOR_VERSION(cluster->major_version) <= 902)
	{
		if (got_tli && got_log_id && got_log_seg)
		{
			snprintf(cluster->controldata.nextxlogfile, 25, "%08X%08X%08X",
					 tli, logid, segno);
			got_nextxlogfile = true;
		}
	}

	/* 验证已获取全部必需 pg_control 数据        */
	if (!got_xid || !got_oid ||
		!got_multi || !got_oldestxid ||
		(!got_oldestmulti &&
		 cluster->controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER) ||
		!got_mxoff || (!live_check && !got_nextxlogfile) ||
		!got_float8_pass_by_value || !got_align || !got_blocksz ||
		!got_largesz || !got_walsz || !got_walseg || !got_ident ||
		!got_index || !got_toast ||
		(!got_large_object &&
		 cluster->controldata.ctrl_ver >= LARGE_OBJECT_SIZE_PG_CONTROL_VER) ||
		!got_date_is_int || !got_data_checksum_version)
	{
		if (cluster == &old_cluster)
			pg_log(PG_REPORT,
				   "The source cluster lacks some required control information:");
		else
			pg_log(PG_REPORT,
				   "The target cluster lacks some required control information:");

		if (!got_xid)
			pg_log(PG_REPORT, "  checkpoint next XID");

		if (!got_oid)
			pg_log(PG_REPORT, "  latest checkpoint next OID");

		if (!got_multi)
			pg_log(PG_REPORT, "  latest checkpoint next MultiXactId");

		if (!got_oldestmulti &&
			cluster->controldata.cat_ver >= MULTIXACT_FORMATCHANGE_CAT_VER)
			pg_log(PG_REPORT, "  latest checkpoint oldest MultiXactId");

		if (!got_oldestxid)
			pg_log(PG_REPORT, "  latest checkpoint oldestXID");

		if (!got_mxoff)
			pg_log(PG_REPORT, "  latest checkpoint next MultiXactOffset");

		if (!live_check && !got_nextxlogfile)
			pg_log(PG_REPORT, "  first WAL segment after reset");

		if (!got_float8_pass_by_value)
			pg_log(PG_REPORT, "  float8 argument passing method");

		if (!got_align)
			pg_log(PG_REPORT, "  maximum alignment");

		if (!got_blocksz)
			pg_log(PG_REPORT, "  block size");

		if (!got_largesz)
			pg_log(PG_REPORT, "  large relation segment size");

		if (!got_walsz)
			pg_log(PG_REPORT, "  WAL block size");

		if (!got_walseg)
			pg_log(PG_REPORT, "  WAL segment size");

		if (!got_ident)
			pg_log(PG_REPORT, "  maximum identifier length");

		if (!got_index)
			pg_log(PG_REPORT, "  maximum number of indexed columns");

		if (!got_toast)
			pg_log(PG_REPORT, "  maximum TOAST chunk size");

		if (!got_large_object &&
			cluster->controldata.ctrl_ver >= LARGE_OBJECT_SIZE_PG_CONTROL_VER)
			pg_log(PG_REPORT, "  large-object chunk size");

		if (!got_date_is_int)
			pg_log(PG_REPORT, "  dates/times are integers?");

		/* Postgres 9.3 新增值      */
		if (!got_data_checksum_version)
			pg_log(PG_REPORT, "  data checksum version");

		pg_fatal("Cannot continue without required control information, terminating");
	}
}


/*
 * check_control_data()
 *
 * 检查 control 数据设置兼容
                           */
void
check_control_data(ControlData *oldctrl,
				   ControlData *newctrl)
{
	if (oldctrl->align == 0 || oldctrl->align != newctrl->align)
		pg_fatal("old and new pg_controldata alignments are invalid or do not match.\n"
				 "Likely one cluster is a 32-bit install, the other 64-bit");

	if (oldctrl->blocksz == 0 || oldctrl->blocksz != newctrl->blocksz)
		pg_fatal("old and new pg_controldata block sizes are invalid or do not match");

	if (oldctrl->largesz == 0 || oldctrl->largesz != newctrl->largesz)
		pg_fatal("old and new pg_controldata maximum relation segment sizes are invalid or do not match");

	if (oldctrl->walsz == 0 || oldctrl->walsz != newctrl->walsz)
		pg_fatal("old and new pg_controldata WAL block sizes are invalid or do not match");

	if (oldctrl->walseg == 0 || oldctrl->walseg != newctrl->walseg)
		pg_fatal("old and new pg_controldata WAL segment sizes are invalid or do not match");

	if (oldctrl->ident == 0 || oldctrl->ident != newctrl->ident)
		pg_fatal("old and new pg_controldata maximum identifier lengths are invalid or do not match");

	if (oldctrl->index == 0 || oldctrl->index != newctrl->index)
		pg_fatal("old and new pg_controldata maximum indexed columns are invalid or do not match");

	if (oldctrl->toast == 0 || oldctrl->toast != newctrl->toast)
		pg_fatal("old and new pg_controldata maximum TOAST chunk sizes are invalid or do not match");

	/* large_object 在 9.5 加入，旧集簇可无                       */
	if (oldctrl->large_object != 0 &&
		oldctrl->large_object != newctrl->large_object)
		pg_fatal("old and new pg_controldata large-object chunk sizes are invalid or do not match");

	if (oldctrl->date_is_int != newctrl->date_is_int)
		pg_fatal("old and new pg_controldata date/time storage types do not match");

	/*
	 * float8_pass_by_value 不需匹配，
	 * 用于 check_for_isn_and_int8_passing_mismatch()。
	               */

	/*
	 * 将来或许会允许从有校验和升级到无校验和      
	 * 的集簇
	 */
	if (oldctrl->data_checksum_version == 0 &&
		newctrl->data_checksum_version != 0)
		pg_fatal("old cluster does not use data checksums but the new one does");
	else if (oldctrl->data_checksum_version != 0 &&
			 newctrl->data_checksum_version == 0)
		pg_fatal("old cluster uses data checksums but the new one does not");
	else if (oldctrl->data_checksum_version != newctrl->data_checksum_version)
		pg_fatal("old and new cluster pg_controldata checksum versions do not match");
}


void
disable_old_cluster(void)
{
	char		old_path[MAXPGPATH],
				new_path[MAXPGPATH];

	/* 重命名 pg_control 防旧服意外启动                     */
	prep_status("Adding \".old\" suffix to old global/pg_control");

	snprintf(old_path, sizeof(old_path), "%s/global/pg_control", old_cluster.pgdata);
	snprintf(new_path, sizeof(new_path), "%s/global/pg_control.old", old_cluster.pgdata);
	if (pg_mv_file(old_path, new_path) != 0)
		pg_fatal("could not rename file \"%s\" to \"%s\": %m",
				 old_path, new_path);
	check_ok();

	pg_log(PG_REPORT, "\n"
		   "If you want to start the old cluster, you will need to remove\n"
		   "the \".old\" suffix from %s/global/pg_control.old.\n"
		   "Because \"link\" mode was used, the old cluster cannot be safely\n"
		   "started once the new cluster has been started.",
		   old_cluster.pgdata);
}
