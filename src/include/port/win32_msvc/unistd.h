/* src/include/port/win32_msvc/unistd.h */

/*
 * MSVC 不定义这些,_fileno(stdin) 等也不能可靠工作
 * (stdin/out/err 关闭时返回 -1)。
 */
#define STDIN_FILENO	0
#define STDOUT_FILENO	1
#define STDERR_FILENO	2
