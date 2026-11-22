/* src/include/port/darwin.h */

#define __darwin__	1

#if HAVE_DECL_F_FULLFSYNC		/*  macOS 10.3 之前不存在  */
#define HAVE_FSYNC_WRITETHROUGH

#endif
