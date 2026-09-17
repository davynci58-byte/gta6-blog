/*
 * GTA6 Blog - FlashDB configuration
 * Uses real FlashDB (https://github.com/armink/FlashDB.git)
 * Linux file mode (POSIX), KVDB for posts/comments, TSDB for view logs.
 */
#ifndef _FDB_CFG_H_
#define _FDB_CFG_H_

/* using KVDB feature */
#define FDB_USING_KVDB

/* using TSDB (Time series database) feature */
#define FDB_USING_TSDB

/* Using file storage mode by POSIX file API */
#define FDB_USING_FILE_POSIX_MODE

/* print debug information (disable for clean server logs) */
/* #define FDB_DEBUG_ENABLE */

#endif /* _FDB_CFG_H_ */
