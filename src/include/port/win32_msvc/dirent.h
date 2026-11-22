/*
 * port/dirent.c 头文件,dirent 的 win32 原生实现
 *
 * src/include/port/win32_msvc/dirent.h
 */

#ifndef _WIN32VC_DIRENT_H
#define _WIN32VC_DIRENT_H
struct dirent
{
	long		d_ino;
	unsigned short d_reclen;
	unsigned char d_type;
	unsigned short d_namlen;
	char		d_name[MAX_PATH];
};

typedef struct DIR DIR;

DIR		   *opendir(const char *);
struct dirent *readdir(DIR *);
int			closedir(DIR *);

/*  d_type 文件类型。  */
#define DT_UNKNOWN		0
#define DT_FIFO		1
#define DT_CHR			2
#define DT_DIR			4
#define DT_BLK			6
#define DT_REG			8
#define DT_LNK			10
#define DT_SOCK		12
#define DT_WHT			14
#endif
