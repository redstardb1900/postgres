/* src/include/port/solaris.h */

/*
 * 日后再对所有操作系统理清。__xxx
 * 符号在 GCC 与 Solaris CC 上都有定义,尽管 GCC
 * 未文档化。__xxx__ 符号仅在 GCC 上。
 */
#if defined(__i386) && !defined(__i386__)
#define __i386__
#endif

#if defined(__amd64) && !defined(__amd64__)
#define __amd64__
#endif

#if defined(__x86_64) && !defined(__x86_64__)
#define __x86_64__
#endif

#if defined(__sparc) && !defined(__sparc__)
#define __sparc__
#endif

#if defined(__i386__)
#include <sys/isa_defs.h>
#endif

/*
 * 原版 Solaris 上 PAM 对话过程声明缺 "const";
 * 但近版 OpenIndiana 默认加上。最省事的办法是
 * 定义 _PAM_LEGACY_NONCONST,使 OpenIndiana 按 Solaris
 * 传统声明 pam_conv,并用该符号控制我们代码中
 * 省略 "const"。
 */
#define _PAM_LEGACY_NONCONST 1
