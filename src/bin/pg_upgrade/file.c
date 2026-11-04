/*
 *	file.c
 *
 *	file system operations
 *
 *	Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/file.c
 */

#include "postgres_fe.h"

#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif

#include "access/visibilitymapdefs.h"
#include "common/file_perm.h"
#include "pg_upgrade.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/checksum_impl.h"


/*将关系文件从 src 克隆/重链接到 dst。schemaName/relName 仅用于错误消息。
 * 
 *
 * 
 *
 * 
                                          */
void
cloneFile(const char *src, const char *dst,
		  const char *schemaName, const char *relName)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(src, dst, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("error while cloning relation \"%s.%s\" (\"%s\" to \"%s\"): %s",
				 schemaName, relName, src, dst, strerror(errno));
#elif defined(__linux__) && defined(FICLONE)
	int			src_fd;
	int			dest_fd;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while cloning relation \"%s.%s\": could not open file \"%s\": %s",
				 schemaName, relName, src, strerror(errno));

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while cloning relation \"%s.%s\": could not create file \"%s\": %s",
				 schemaName, relName, dst, strerror(errno));

	if (ioctl(dest_fd, FICLONE, src_fd) < 0)
	{
		int			save_errno = errno;

		unlink(dst);

		pg_fatal("error while cloning relation \"%s.%s\" (\"%s\" to \"%s\"): %s",
				 schemaName, relName, src, dst, strerror(save_errno));
	}

	close(src_fd);
	close(dest_fd);
#endif
}


/*将关系文件从 src 复制到 dst。schemaName/relName 仅用于错误消息。
 * 
 *
 * 
 * 
                                          */
void
copyFile(const char *src, const char *dst,
		 const char *schemaName, const char *relName)
{
#ifndef WIN32
	int			src_fd;
	int			dest_fd;
	char	   *buffer;

	if ((src_fd = open(src, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %s",
				 schemaName, relName, src, strerror(errno));

	if ((dest_fd = open(dst, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
						pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %s",
				 schemaName, relName, dst, strerror(errno));

	/* 大块复制求最佳效率                     */
#define COPY_BUF_SIZE (50 * BLCKSZ)

	buffer = (char *) pg_malloc(COPY_BUF_SIZE);

	/* 执行数据复制：读 src，写 dst                         */
	while (true)
	{
		ssize_t		nbytes = read(src_fd, buffer, COPY_BUF_SIZE);

		if (nbytes < 0)
			pg_fatal("error while copying relation \"%s.%s\": could not read file \"%s\": %s",
					 schemaName, relName, src, strerror(errno));

		if (nbytes == 0)
			break;

		errno = 0;
		if (write(dest_fd, buffer, nbytes) != nbytes)
		{
			/* write 未设 errno 则假设盘空不足                   */
			if (errno == 0)
				errno = ENOSPC;
			pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %s",
					 schemaName, relName, dst, strerror(errno));
		}
	}

	pg_free(buffer);
	close(src_fd);
	close(dest_fd);

#else							/* WIN32 */

	if (CopyFile(src, dst, true) == 0)
	{
		_dosmaperr(GetLastError());
		pg_fatal("error while copying relation \"%s.%s\" (\"%s\" to \"%s\"): %s",
				 schemaName, relName, src, dst, strerror(errno));
	}

#endif							/* WIN32 */
}


/*
 * linkFile()
 *
 * 将关系文件从 src 硬链接到 dst。 
 * schemaName/relName 仅用于错误消息。                               
 */
void
linkFile(const char *src, const char *dst,
		 const char *schemaName, const char *relName)
{
	if (link(src, dst) < 0)
		pg_fatal("error while creating link for relation \"%s.%s\" (\"%s\" to \"%s\"): %s",
				 schemaName, relName, src, dst, strerror(errno));
}


/*
 * rewriteVisibilityMap()
 *
 * 转换可见性映射文件，从 src 复制到 dst。   
 * schemaName/relName 仅用于错误消息。                               
 *
 * 在 catversion 201603011 之前的 PostgreSQL 版本中，可见性  
 * 映射每个堆页包含一位；现在包含两位。             
 * 将那时的集簇升级到当前 PostgreSQL 版本时，本可以拒 
 * 绝把可见性映射从旧集簇复制到新集簇；下一次      
 * VACUUM 会重建它们，但代价是扫描整张表。            
 * 所以我们改为以新格式重写旧的可见性映射。这     
 * 样，之前置位的页面的 all-visible 位仍保持置位。  
 * all-frozen 位绝不会由此转换置位；这一点留给      
 * VACUUM 处理。                                                          
 */
void
rewriteVisibilityMap(const char *fromfile, const char *tofile,
					 const char *schemaName, const char *relName)
{
	int			src_fd;
	int			dst_fd;
	PGIOAlignedBlock buffer;
	PGIOAlignedBlock new_vmbuf;
	ssize_t		totalBytesRead = 0;
	ssize_t		src_filesize;
	int			rewriteVmBytesPerPage;
	BlockNumber new_blkno = 0;
	struct stat statbuf;

	/* 计算每新页的旧格式字节数            */
	rewriteVmBytesPerPage = (BLCKSZ - SizeOfPageHeaderData) / 2;

	if ((src_fd = open(fromfile, O_RDONLY | PG_BINARY, 0)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not open file \"%s\": %s",
				 schemaName, relName, fromfile, strerror(errno));

	if (fstat(src_fd, &statbuf) != 0)
		pg_fatal("error while copying relation \"%s.%s\": could not stat file \"%s\": %s",
				 schemaName, relName, fromfile, strerror(errno));

	if ((dst_fd = open(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
					   pg_file_create_mode)) < 0)
		pg_fatal("error while copying relation \"%s.%s\": could not create file \"%s\": %s",
				 schemaName, relName, tofile, strerror(errno));

	/* 存旧文件大小 */
	src_filesize = statbuf.st_size;

	/*
	 * 将每个可见性映射页逐个转换为 2 页。每个新页的  
	 * 页头与旧页相同。若最后一页的最后部分为空，则  
	 * 跳过它，主要是为了避免把小关系的单页可见性 
	 * 映射不必要地变成两页。                             
	 */
	while (totalBytesRead < src_filesize)
	{
		ssize_t		bytesRead;
		char	   *old_cur;
		char	   *old_break;
		char	   *old_blkend;
		PageHeaderData pageheader;
		bool		old_lastblk;

		if ((bytesRead = read(src_fd, buffer.data, BLCKSZ)) != BLCKSZ)
		{
			if (bytesRead < 0)
				pg_fatal("error while copying relation \"%s.%s\": could not read file \"%s\": %s",
						 schemaName, relName, fromfile, strerror(errno));
			else
				pg_fatal("error while copying relation \"%s.%s\": partial page found in file \"%s\"",
						 schemaName, relName, fromfile);
		}

		totalBytesRead += BLCKSZ;
		old_lastblk = (totalBytesRead == src_filesize);

		/* 保存页头数据        */
		memcpy(&pageheader, buffer.data, SizeOfPageHeaderData);

		/*
		 * 这些 old_* 变量指向旧可见性映射页。old_cur 指向
		 * 旧页上的当前位置。old_blkend 指向旧块的结尾。     
		 * old_break 是旧页上将被传输到当前新页的数据的 end+1
		 * 位置。                                             
		 */
		old_cur = buffer.data + SizeOfPageHeaderData;
		old_blkend = buffer.data + bytesRead;
		old_break = old_cur + rewriteVmBytesPerPage;

		while (old_break <= old_blkend)
		{
			char	   *new_cur;
			bool		empty = true;
			bool		old_lastpart;

			/* 首先，将旧页头复制到新页    */
			memcpy(new_vmbuf.data, &pageheader, SizeOfPageHeaderData);

			/* 重写旧页最后一页的最后部分？    */
			old_lastpart = old_lastblk && (old_break == old_blkend);

			new_cur = new_vmbuf.data + SizeOfPageHeaderData;

			/* 逐个处理旧页字节，将其转换为新页。           */
			while (old_cur < old_break)
			{
				uint8		byte = *(uint8 *) old_cur;
				uint16		new_vmbits = 0;
				int			i;

				/* 生成新格式位，同时保留旧信息。          */
				for (i = 0; i < BITS_PER_BYTE; i++)
				{
					if (byte & (1 << i))
					{
						empty = false;
						new_vmbits |=
							VISIBILITYMAP_ALL_VISIBLE << (BITS_PER_HEAPBLOCK * i);
					}
				}

				/* 将新可见性映射字节复制到新格式页 */
				new_cur[0] = (char) (new_vmbits & 0xFF);
				new_cur[1] = (char) (new_vmbits >> 8);

				old_cur++;
				new_cur += BITS_PER_HEAPBLOCK;
			}

			/* 若最后一页的最后部分为空，则跳过写入      */
			if (old_lastpart && empty)
				break;

			/* 启用时为可见性映射页设置新校验和     */
			if (new_cluster.controldata.data_checksum_version != 0)
				((PageHeader) new_vmbuf.data)->pd_checksum =
					pg_checksum_page(new_vmbuf.data, new_blkno);

			errno = 0;
			if (write(dst_fd, new_vmbuf.data, BLCKSZ) != BLCKSZ)
			{
				/* write 未设 errno 则假设盘空不足                   */
				if (errno == 0)
					errno = ENOSPC;
				pg_fatal("error while copying relation \"%s.%s\": could not write file \"%s\": %s",
						 schemaName, relName, tofile, strerror(errno));
			}

			/* 前进到下一个新页  */
			old_break += rewriteVmBytesPerPage;
			new_blkno++;
		}
	}

	/* 清理   */
	close(dst_fd);
	close(src_fd);
}

void
check_file_clone(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.clonetest", new_cluster.pgdata);
	unlink(new_link_file);		/*可能失败*/

#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(existing_file, new_link_file, NULL, COPYFILE_CLONE_FORCE) < 0)
		pg_fatal("could not clone file between old and new data directories: %s",
				 strerror(errno));
#elif defined(__linux__) && defined(FICLONE)
	{
		int			src_fd;
		int			dest_fd;

		if ((src_fd = open(existing_file, O_RDONLY | PG_BINARY, 0)) < 0)
			pg_fatal("could not open file \"%s\": %s",
					 existing_file, strerror(errno));

		if ((dest_fd = open(new_link_file, O_RDWR | O_CREAT | O_EXCL | PG_BINARY,
							pg_file_create_mode)) < 0)
			pg_fatal("could not create file \"%s\": %s",
					 new_link_file, strerror(errno));

		if (ioctl(dest_fd, FICLONE, src_fd) < 0)
			pg_fatal("could not clone file between old and new data directories: %s",
					 strerror(errno));

		close(src_fd);
		close(dest_fd);
	}
#else
	pg_fatal("file cloning not supported on this platform");
#endif

	unlink(new_link_file);
}

void
check_hard_link(void)
{
	char		existing_file[MAXPGPATH];
	char		new_link_file[MAXPGPATH];

	snprintf(existing_file, sizeof(existing_file), "%s/PG_VERSION", old_cluster.pgdata);
	snprintf(new_link_file, sizeof(new_link_file), "%s/PG_VERSION.linktest", new_cluster.pgdata);
	unlink(new_link_file);		/*可能失败*/

	if (link(existing_file, new_link_file) < 0)
		pg_fatal("could not create hard link between old and new data directories: %s\n"
				 "In link mode the old and new data directories must be on the same file system.",
				 strerror(errno));

	unlink(new_link_file);
}
