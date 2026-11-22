/* src/include/port/linux.h */

/*
 * As of July 2007, all known versions of the Linux kernel will sometimes
 * return EIDRM for a shmctl() operation when EINVAL is correct (it happens
 * when the low-order 15 bits of the supplied shm ID match the slot number
 * assigned to a newer shmem segment).  We deal with this by assuming that
 * EIDRM means EINVAL in PGSharedMemoryIsInUse().  This is reasonably safe
 * since in fact Linux has no excuse for ever returning EIDRM; it doesn't
 * track removed segments in a way that would allow distinguishing them from
 * private ones.  But someday that code might get upgraded, and we'd have
 * to have a kernel version test here.
 */
#define HAVE_LINUX_EIDRM_BUG

/*
 * 默认 wal_sync_method 设为 fdatasync。近版 Linux 上
 * xlogdefs.h 常规规则会偏好 open_datasync,(a) 并不更优
 * 且 (b) 在 ext4 data=journal 文件系统上会直接失败,
 * 因其不支持 O_DIRECT。
 */
#define PLATFORM_DEFAULT_SYNC_METHOD	SYNC_METHOD_FDATASYNC
