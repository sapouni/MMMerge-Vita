#include "android-abi.h"

#include <stddef.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

_Static_assert(sizeof(AndroidStat) == sizeof(struct stat), "Android stat size");
_Static_assert(offsetof(AndroidStat, size) == offsetof(struct stat, st_size), "Android stat size offset");
_Static_assert(offsetof(AndroidStat, blocks) == offsetof(struct stat, st_blocks), "Android stat blocks offset");
_Static_assert(offsetof(AndroidStat, accessTime) == offsetof(struct stat, st_atim), "Android stat time offset");
_Static_assert(offsetof(AndroidStat, inode) == offsetof(struct stat, st_ino), "Android stat inode offset");
_Static_assert(sizeof(AndroidDirent) == sizeof(struct dirent), "Android dirent size");
_Static_assert(offsetof(AndroidDirent, name) == offsetof(struct dirent, d_name), "Android dirent name offset");
_Static_assert(offsetof(AndroidDirent, recordLength) == offsetof(struct dirent, d_reclen), "Android dirent length");
_Static_assert(sizeof(AndroidTimespec) == sizeof(struct timespec), "Android timespec size");
_Static_assert(sizeof(AndroidThreadAttributes) == sizeof(pthread_attr_t), "Android pthread attributes size");
_Static_assert(sizeof(pthread_mutex_t) == 4, "Android mutex size");
_Static_assert(sizeof(pthread_cond_t) == 4, "Android condition size");
