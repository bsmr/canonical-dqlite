#include <errno.h>

#include <raft.h>
#include <sqlite3.h>

#include "../../include/dqlite.h"

#include "../lib/config.h"
#include "../lib/fs.h"
#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/sqlite.h"

#include "../../src/format.h"
#include "../../src/vfs.h"

/******************************************************************************
 *
 * Fixture
 *
 ******************************************************************************/

struct fixture
{
	struct sqlite3_vfs vfs;
};

static void *setUp(const MunitParameter params[], void *user_data)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rv;
	SETUP_HEAP;
	SETUP_SQLITE;
	rv = VfsInit(&f->vfs, "dqlite");
	munit_assert_int(rv, ==, 0);
	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;
	VfsClose(&f->vfs);
	TEAR_DOWN_SQLITE;
	TEAR_DOWN_HEAP;
	free(f);
}

/******************************************************************************
 *
 * Helpers
 *
 ******************************************************************************/

/* Helper for creating a new file */
static sqlite3_file *__file_create(sqlite3_vfs *vfs,
				   const char *name,
				   int type_flag)
{
	sqlite3_file *file = munit_malloc(vfs->szOsFile);

	int flags;
	int rc;

	flags = SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_CREATE | type_flag;

	rc = vfs->xOpen(vfs, name, file, flags, &flags);
	munit_assert_int(rc, ==, 0);

	return file;
}

/* Helper for creating a new database file */
static sqlite3_file *__file_create_main_db(sqlite3_vfs *vfs)
{
	return __file_create(vfs, "test.db", SQLITE_OPEN_MAIN_DB);
}

/* Helper for creating a new WAL file */
static sqlite3_file *__file_create_wal(sqlite3_vfs *vfs)
{
	return __file_create(vfs, "test.db-wal", SQLITE_OPEN_WAL);
}

/* Helper for allocating a buffer of 100 bytes containing a database header with
 * a page size field set to 512 bytes. */
static void *__buf_header_main_db()
{
	char *buf = munit_malloc(100 * sizeof *buf);

	/* Set page size to 512. */
	buf[16] = 2;
	buf[17] = 0;

	return buf;
}

/* Helper for allocating a buffer of 32 bytes containing a WAL header with
 * a page size field set to 512 bytes. */
static void *__buf_header_wal()
{
	char *buf = munit_malloc(32 * sizeof *buf);

	/* Set page size to 512. */
	buf[10] = 2;
	buf[11] = 0;

	return buf;
}

/* Helper for allocating a buffer of 24 bytes containing a WAL frame header. */
static void *__buf_header_wal_frame()
{
	char *buf = munit_malloc(24 * sizeof *buf);

	return buf;
}

/* Helper for allocating a buffer with the content of the first page, i.e. the
 * the header and some other bytes. */
static void *__buf_page_1()
{
	char *buf = munit_malloc(512 * sizeof *buf);

	/* Set page size to 512. */
	buf[16] = 2;
	buf[17] = 0;

	/* Set some other bytes */
	buf[101] = 1;
	buf[256] = 2;
	buf[511] = 3;

	return buf;
}

/* Helper for allocating a buffer with the content of the second page. */
static void *__buf_page_2()
{
	char *buf = munit_malloc(512 * sizeof *buf);

	buf[0] = 4;
	buf[256] = 5;
	buf[511] = 6;

	return buf;
}

/* Helper to execute a SQL statement. */
static void __db_exec(sqlite3 *db, const char *sql)
{
	int rc;

	rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
	munit_assert_int(rc, ==, SQLITE_OK);
}

/* Helper to open and initialize a database, setting the page size and
 * WAL mode. */
static sqlite3 *__db_open()
{
	sqlite3 *db;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	int rc;

	rc = sqlite3_open_v2("test.db", &db, flags, "dqlite");
	munit_assert_int(rc, ==, SQLITE_OK);

	__db_exec(db, "PRAGMA page_size=512");
	__db_exec(db, "PRAGMA synchronous=OFF");
	__db_exec(db, "PRAGMA journal_mode=WAL");

	return db;
}

/* Helper to close a database. */
static void __db_close(sqlite3 *db)
{
	int rv;
	rv = sqlite3_close(db);
	munit_assert_int(rv, ==, SQLITE_OK);
}

/* Helper get the mxFrame value of the WAL index object associated with the
 * given database. */
static uint32_t __wal_idx_mx_frame(sqlite3 *db)
{
	sqlite3_file *file;
	volatile void *region;
	uint32_t mx_frame;
	int rc;

	rc = sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = file->pMethods->xShmMap(file, 0, 0, 0, &region);
	munit_assert_int(rc, ==, SQLITE_OK);

	format__get_mx_frame((const uint8_t *)region, &mx_frame);

	return mx_frame;
}

/* Helper get the read mark array of the WAL index object associated with the
 * given database. */
static uint32_t *__wal_idx_read_marks(sqlite3 *db)
{
	sqlite3_file *file;
	volatile void *region;
	uint32_t *marks;
	int rc;

	marks = munit_malloc(FORMAT__WAL_NREADER * sizeof *marks);

	rc = sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = file->pMethods->xShmMap(file, 0, 0, 0, &region);
	munit_assert_int(rc, ==, SQLITE_OK);

	format__get_read_marks((const uint8_t *)region, marks);

	return marks;
}

/* Helper that returns true if the i'th lock of the shared memmory reagion
 * associated with the given database is currently held. */
static int __shm_shared_lock_held(sqlite3 *db, int i)
{
	sqlite3_file *file;
	int flags;
	int locked;
	int rc;

	rc = sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file);
	munit_assert_int(rc, ==, SQLITE_OK);

	/* Try to acquire an exclusive lock, which will fail if the shared lock
	 * is held. */
	flags = SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE;
	rc = file->pMethods->xShmLock(file, i, 1, flags);

	locked = rc == SQLITE_BUSY;

	if (rc == SQLITE_OK) {
		flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE;
		rc = file->pMethods->xShmLock(file, i, 1, flags);
		munit_assert_int(rc, ==, SQLITE_OK);
	}

	return locked;
}

/******************************************************************************
 *
 * xOpen
 *
 ******************************************************************************/

SUITE(VfsOpen)

/* If the EXCLUSIVE and CREATE flag are given, and the file already exists, an
 * error is returned. */
TEST(VfsOpen, exclusive, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);

	munit_assert_int(rc, ==, SQLITE_OK);

	flags |= SQLITE_OPEN_EXCLUSIVE;
	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);

	munit_assert_int(rc, ==, SQLITE_CANTOPEN);
	munit_assert_int(EEXIST, ==, f->vfs.xGetLastError(&f->vfs, 0, 0));

	free(file);

	return MUNIT_OK;
}

/* It's possible to open again a previously created file. In that case passing
 * SQLITE_OPEN_CREATE is not necessary. */
TEST(VfsOpen, again, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);

	munit_assert_int(rc, ==, SQLITE_OK);

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, SQLITE_OK);

	flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_MAIN_DB;
	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);

	munit_assert_int(rc, ==, 0);

	free(file);

	return MUNIT_OK;
}

/* If the file does not exist and the SQLITE_OPEN_CREATE flag is not passed, an
 * error is returned. */
TEST(VfsOpen, noent, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, 0, &flags);

	munit_assert_int(rc, ==, SQLITE_CANTOPEN);
	munit_assert_int(ENOENT, ==, f->vfs.xGetLastError(&f->vfs, 0, 0));

	free(file);

	return MUNIT_OK;
}

/* There's an hard-coded limit for the number of files that can be opened. */
TEST(VfsOpen, entfile, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;
	int i;
	char name[20];

	(void)params;

	flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;

	for (i = 0; i < 64; i++) {
		sprintf(name, "test-%d.db", i);
		rc = f->vfs.xOpen(&f->vfs, name, file, flags, &flags);
		munit_assert_int(rc, ==, 0);
	}

	rc = f->vfs.xOpen(&f->vfs, "test-64.db", file, flags, &flags);

	munit_assert_int(rc, ==, SQLITE_CANTOPEN);
	munit_assert_int(ENFILE, ==, f->vfs.xGetLastError(&f->vfs, 0, 0));

	free(file);

	return MUNIT_OK;
}

/* Trying to open a WAL file before its main database file results in an
 * error. */
TEST(VfsOpen, walBeforeDb, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_WAL;
	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);

	munit_assert_int(rc, ==, SQLITE_CORRUPT);

	free(file);

	return MUNIT_OK;
}

/* Trying to run queries against a database that hasn't turned off the
 * synchronous flag results in an error. */
TEST(VfsOpen, synchronous, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3 *db;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	int rc;

	(void)params;

	rc = sqlite3_vfs_register(&f->vfs, 0);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = sqlite3_open_v2("test.db", &db, flags, f->vfs.zName);
	munit_assert_int(rc, ==, SQLITE_OK);

	__db_exec(db, "PRAGMA page_size=4092");

	rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
	munit_assert_int(rc, ==, SQLITE_IOERR);

	munit_assert_string_equal(sqlite3_errmsg(db), "disk I/O error");

	__db_close(db);

	rc = sqlite3_vfs_unregister(&f->vfs);
	munit_assert_int(rc, ==, SQLITE_OK);

	return MUNIT_OK;
}

/* If no page size is set explicitely, the default one is used. */
TEST(VfsOpen, noPageSize, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3 *db;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	sqlite3_int64 size;
	int rc;
	(void)params;

	rc = sqlite3_vfs_register(&f->vfs, 0);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = sqlite3_open_v2("test.db", &db, flags, f->vfs.zName);
	munit_assert_int(rc, ==, SQLITE_OK);

	__db_exec(db, "PRAGMA synchronous=OFF");
	__db_exec(db, "PRAGMA journal_mode=WAL");

	rc = sqlite3_exec(db, "CREATE TABLE foo (n INT)", NULL, NULL, NULL);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 4096);

	rc = f->vfs.xOpen(&f->vfs, "test.db-wal", file, flags, &flags);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 8272);

	__db_close(db);

	rc = sqlite3_vfs_unregister(&f->vfs);
	munit_assert_int(rc, ==, SQLITE_OK);

	free(file);

	return MUNIT_OK;
}

/* Out of memory when creating the content structure for a new file. */
TEST(VfsOpen, oom, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	int rc;

	(void)params;

	test_heap_fault_config(0, 1);
	test_heap_fault_enable();

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(file);

	return MUNIT_OK;
}

/* Out of memory when internally copying the filename. */
TEST(VfsOpen, oomFilename, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	int rc;

	(void)params;

	test_heap_fault_config(1, 1);
	test_heap_fault_enable();

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(file);

	return MUNIT_OK;
}

/* Out of memory when creating the WAL file header. */
TEST(VfsOpen, oomWal, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_WAL;
	int rc;

	(void)params;

	test_heap_fault_config(2, 1);
	test_heap_fault_enable();

	rc = f->vfs.xOpen(&f->vfs, "test.db-wal", file, flags, &flags);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(file);

	return MUNIT_OK;
}

/* Open a temporary file. */
TEST(VfsOpen, tmp, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = 0;
	char buf[16];
	int rc;

	(void)params;

	flags |= SQLITE_OPEN_CREATE;
	flags |= SQLITE_OPEN_READWRITE;
	flags |= SQLITE_OPEN_TEMP_JOURNAL;
	flags |= SQLITE_OPEN_DELETEONCLOSE;

	rc = f->vfs.xOpen(&f->vfs, NULL, file, flags, &flags);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = file->pMethods->xWrite(file, "hello", 5, 0);
	munit_assert_int(rc, ==, SQLITE_OK);

	memset(buf, 0, sizeof buf);
	rc = file->pMethods->xRead(file, buf, 5, 0);
	munit_assert_int(rc, ==, SQLITE_OK);

	munit_assert_string_equal(buf, "hello");

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, SQLITE_OK);

	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xDelete
 *
 ******************************************************************************/

SUITE(VfsDelete)

/* Delete a file. */
TEST(VfsDelete, success, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, SQLITE_OPEN_CREATE, &flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, 0);

	rc = f->vfs.xDelete(&f->vfs, "test.db", 0);
	munit_assert_int(rc, ==, 0);

	/* Trying to open the file again without the SQLITE_OPEN_CREATE flag
	 * results in an error. */
	rc = f->vfs.xOpen(&f->vfs, "test.db", file, 0, &flags);
	munit_assert_int(rc, ==, SQLITE_CANTOPEN);

	free(file);

	return MUNIT_OK;
}

/* Attempt to delete a file with open file descriptors. */
TEST(VfsDelete, busy, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, SQLITE_OPEN_CREATE, &flags);
	munit_assert_int(rc, ==, 0);

	rc = f->vfs.xDelete(&f->vfs, "test.db", 0);
	munit_assert_int(rc, ==, SQLITE_IOERR_DELETE);
	munit_assert_int(EBUSY, ==, f->vfs.xGetLastError(&f->vfs, 0, 0));

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, 0);

	free(file);

	return MUNIT_OK;
}

/* Trying to delete a non-existing file results in an error. */
TEST(VfsDelete, enoent, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;

	int rc;

	(void)params;

	rc = f->vfs.xDelete(&f->vfs, "test.db", 0);
	munit_assert_int(rc, ==, SQLITE_IOERR_DELETE_NOENT);
	munit_assert_int(ENOENT, ==, f->vfs.xGetLastError(&f->vfs, 0, 0));

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xAccess
 *
 ******************************************************************************/

SUITE(VfsAccess)

/* Accessing an existing file returns true. */
TEST(VfsAccess, success, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;
	int exists;

	(void)params;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, SQLITE_OPEN_CREATE, &flags);

	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, 0);

	rc = f->vfs.xAccess(&f->vfs, "test.db", 0, &exists);
	munit_assert_int(rc, ==, 0);

	munit_assert_true(exists);

	free(file);

	return MUNIT_OK;
}

/* Trying to access a non existing file returns false. */
TEST(VfsAccess, noent, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;

	int rc;
	int exists;

	(void)params;

	rc = f->vfs.xAccess(&f->vfs, "test.db", 0, &exists);
	munit_assert_int(rc, ==, 0);

	munit_assert_false(exists);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xFullPathname
 *
 ******************************************************************************/

SUITE(VfsFullPathname);

/* The xFullPathname API returns the filename unchanged. */
TEST(VfsFullPathname, success, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;

	int rc;
	char pathname[10];

	(void)params;

	rc = f->vfs.xFullPathname(&f->vfs, "test.db", 10, pathname);
	munit_assert_int(rc, ==, 0);

	munit_assert_string_equal(pathname, "test.db");

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xClose
 *
 ******************************************************************************/

SUITE(VfsClose)

/* Closing a file decreases its refcount so it's possible to delete it. */
TEST(VfsClose, thenDelete, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);

	int flags;
	int rc;

	(void)params;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, SQLITE_OPEN_CREATE, &flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, 0);

	rc = f->vfs.xDelete(&f->vfs, "test.db", 0);
	munit_assert_int(rc, ==, 0);

	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xRead
 *
 ******************************************************************************/

SUITE(VfsRead)

/* Trying to read a file that was not written yet, results in an error. */
TEST(VfsRead, neverWritten, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);

	int rc;
	char buf[1] = {123};

	(void)params;

	rc = file->pMethods->xRead(file, (void *)buf, 1, 0);
	munit_assert_int(rc, ==, SQLITE_IOERR_SHORT_READ);

	/* The buffer gets filled with zero */
	munit_assert_int(buf[0], ==, 0);

	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xWrite
 *
 ******************************************************************************/

SUITE(VfsWrite)

/* Write the header of the database file. */
TEST(VfsWrite, dbHeader, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);

	void *buf = __buf_header_main_db();

	int rc;

	(void)params;

	rc = file->pMethods->xWrite(file, buf, 100, 0);
	munit_assert_int(rc, ==, 0);

	free(file);
	free(buf);

	return MUNIT_OK;
}

/* Write the header of the database file, then the full first page and a second
 * page. */
TEST(VfsWrite, andReadPages, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);

	int rc;
	char buf[512];
	void *buf_header_main = __buf_header_main_db();
	void *buf_page_1 = __buf_page_1();
	void *buf_page_2 = __buf_page_2();

	(void)params;

	memset(buf, 0, 512);

	/* Write the header. */
	rc = file->pMethods->xWrite(file, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, 0);

	/* Write the first page, containing the header and some content. */
	rc = file->pMethods->xWrite(file, buf_page_1, 512, 0);
	munit_assert_int(rc, ==, 0);

	/* Write a second page. */
	rc = file->pMethods->xWrite(file, buf_page_2, 512, 512);
	munit_assert_int(rc, ==, 0);

	/* Read the page header. */
	rc = file->pMethods->xRead(file, (void *)buf, 512, 0);
	munit_assert_int(rc, ==, 0);

	munit_assert_int(buf[16], ==, 2);
	munit_assert_int(buf[17], ==, 0);
	munit_assert_int(buf[101], ==, 1);
	munit_assert_int(buf[256], ==, 2);
	munit_assert_int(buf[511], ==, 3);

	/* Read the second page. */
	memset(buf, 0, 512);
	rc = file->pMethods->xRead(file, (void *)buf, 512, 512);
	munit_assert_int(rc, ==, 0);

	munit_assert_int(buf[0], ==, 4);
	munit_assert_int(buf[256], ==, 5);
	munit_assert_int(buf[511], ==, 6);

	free(buf_header_main);
	free(buf_page_1);
	free(buf_page_2);
	free(file);

	return MUNIT_OK;
}

/* Write the header of a WAL file, then two frames. */
TEST(VfsWrite, andReadWalFrames, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file1 = __file_create_main_db(&f->vfs);
	sqlite3_file *file2 = __file_create_wal(&f->vfs);
	void *buf_header_main = __buf_header_main_db();
	void *buf_header_wal = __buf_header_wal();
	void *buf_header_wal_frame_1 = __buf_header_wal_frame();
	void *buf_header_wal_frame_2 = __buf_header_wal_frame();
	void *buf_page_1 = __buf_page_1();
	void *buf_page_2 = __buf_page_2();

	int rc;
	char buf[512];

	(void)params;

	memset(buf, 0, 512);

	/* First write the main database header, which sets the page size. */
	rc = file1->pMethods->xWrite(file1, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, 0);

	/* Open the associated WAL file and write the WAL header. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal, 32, 0);
	munit_assert_int(rc, ==, 0);

	/* Write the header of the first frame. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal_frame_1, 24, 32);
	munit_assert_int(rc, ==, 0);

	/* Write the page of the first frame. */
	rc = file2->pMethods->xWrite(file2, buf_page_1, 512, 32 + 24);
	munit_assert_int(rc, ==, 0);

	/* Write the header of the second frame. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal_frame_2, 24,
				     32 + 24 + 512);
	munit_assert_int(rc, ==, 0);

	/* Write the page of the second frame. */
	rc =
	    file2->pMethods->xWrite(file2, buf_page_2, 512, 32 + 24 + 512 + 24);
	munit_assert_int(rc, ==, 0);

	/* Read the WAL header. */
	rc = file2->pMethods->xRead(file2, (void *)buf, 32, 0);
	munit_assert_int(rc, ==, 0);

	/* Read the header of the first frame. */
	rc = file2->pMethods->xRead(file2, (void *)buf, 24, 32);
	munit_assert_int(rc, ==, 0);

	/* Read the page of the first frame. */
	rc = file2->pMethods->xRead(file2, (void *)buf, 512, 32 + 24);
	munit_assert_int(rc, ==, 0);

	/* Read the header of the second frame. */
	rc = file2->pMethods->xRead(file2, (void *)buf, 24, 32 + 24 + 512);
	munit_assert_int(rc, ==, 0);

	/* Read the page of the second frame. */
	rc =
	    file2->pMethods->xRead(file2, (void *)buf, 512, 32 + 24 + 512 + 24);
	munit_assert_int(rc, ==, 0);

	free(buf_page_1);
	free(buf_page_2);
	free(buf_header_wal_frame_1);
	free(buf_header_wal_frame_2);
	free(buf_header_wal);
	free(buf_header_main);
	free(file1);
	free(file2);

	return MUNIT_OK;
}

/* Out of memory when trying to create a new page. */
TEST(VfsWrite, oomPage, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_header_main = __buf_header_main_db();
	char buf[512];
	int rc;

	test_heap_fault_config(0, 1);
	test_heap_fault_enable();

	(void)params;

	memset(buf, 0, 512);

	/* Write the database header, which triggers creating the first page. */
	rc = file->pMethods->xWrite(file, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(buf_header_main);
	free(file);

	return MUNIT_OK;
}

/* Out of memory when trying to append a new page to the internal page array of
 * the content object. */
TEST(VfsWrite, oomPageArray, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_header_main = __buf_header_main_db();
	char buf[512];
	int rc;

	test_heap_fault_config(2, 1);
	test_heap_fault_enable();

	(void)params;

	memset(buf, 0, 512);

	/* Write the database header, which triggers creating the first page. */
	rc = file->pMethods->xWrite(file, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(buf_header_main);
	free(file);

	return MUNIT_OK;
}

/* Out of memory when trying to create the content buffer of a new page. */
TEST(VfsWrite, oomPageBuf, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_header_main = __buf_header_main_db();
	char buf[512];
	int rc;

	test_heap_fault_config(1, 1);
	test_heap_fault_enable();

	(void)params;

	memset(buf, 0, 512);

	/* Write the database header, which triggers creating the first page. */
	rc = file->pMethods->xWrite(file, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(buf_header_main);
	free(file);

	return MUNIT_OK;
}

/* Out of memory when trying to create the header buffer of a new WAL page. */
TEST(VfsWrite, oomPageHdr, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file1 = __file_create_main_db(&f->vfs);
	sqlite3_file *file2 = __file_create_wal(&f->vfs);
	void *buf_header_main = __buf_header_main_db();
	void *buf_header_wal = __buf_header_wal();
	void *buf_header_wal_frame = __buf_header_wal_frame();
	char buf[512];
	int rc;

	(void)params;

	memset(buf, 0, 512);

	test_heap_fault_config(6, 1);
	test_heap_fault_enable();

	/* First write the main database header, which sets the page size. */
	rc = file1->pMethods->xWrite(file1, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, 0);

	/* Write the WAL header */
	rc = file2->pMethods->xWrite(file2, buf_header_wal, 32, 0);
	munit_assert_int(rc, ==, 0);

	/* Write the header of the first frame, which triggers creating the
	 * first page. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal_frame, 24, 32);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(buf_header_main);
	free(buf_header_wal);
	free(buf_header_wal_frame);
	free(file1);
	free(file2);

	return MUNIT_OK;
}

/* Trying to write the second page without writing the first results in an
 * error. */
TEST(VfsWrite, beyondFirst, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_page_1 = __buf_page_1();
	char buf[512];
	int rc;

	(void)params;

	memset(buf, 0, 512);

	/* Write the second page, without writing the first. */
	rc = file->pMethods->xWrite(file, buf_page_1, 512, 512);
	munit_assert_int(rc, ==, SQLITE_IOERR_WRITE);

	free(buf_page_1);
	free(file);

	return MUNIT_OK;
}

/* Trying to write two pages beyond the last one results in an error. */
TEST(VfsWrite, beyondLast, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_page_1 = __buf_page_1();
	void *buf_page_2 = __buf_page_2();
	char buf[512];
	int rc;

	(void)params;

	memset(buf, 0, 512);

	/* Write the first page. */
	rc = file->pMethods->xWrite(file, buf_page_1, 512, 0);
	munit_assert_int(rc, ==, 0);

	/* Write the third page, without writing the second. */
	rc = file->pMethods->xWrite(file, buf_page_2, 512, 1024);
	munit_assert_int(rc, ==, SQLITE_IOERR_WRITE);

	free(buf_page_1);
	free(buf_page_2);
	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xTruncate
 *
 ******************************************************************************/

SUITE(VfsTruncate);

/* Truncate the main database file. */
TEST(VfsTruncate, database, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_page_1 = __buf_page_1();
	void *buf_page_2 = __buf_page_2();

	int rc;

	sqlite_int64 size;

	(void)params;

	/* Initial size is 0. */
	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	/* Truncating an empty file is a no-op. */
	rc = file->pMethods->xTruncate(file, 0);
	munit_assert_int(rc, ==, 0);

	/* The size is still 0. */
	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	/* Write the first page, containing the header. */
	rc = file->pMethods->xWrite(file, buf_page_1, 512, 0);
	munit_assert_int(rc, ==, 0);

	/* Write a second page. */
	rc = file->pMethods->xWrite(file, buf_page_2, 512, 512);
	munit_assert_int(rc, ==, 0);

	/* The size is 1024. */
	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 1024);

	/* Truncate the second page. */
	rc = file->pMethods->xTruncate(file, 512);
	munit_assert_int(rc, ==, 0);

	/* The size is 512. */
	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 512);

	/* Truncate also the first. */
	rc = file->pMethods->xTruncate(file, 0);
	munit_assert_int(rc, ==, 0);

	/* The size is 0. */
	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	free(buf_page_1);
	free(buf_page_2);
	free(file);

	return MUNIT_OK;
}

/* Truncate the WAL file. */
TEST(VfsTruncate, wal, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file1 = __file_create_main_db(&f->vfs);
	sqlite3_file *file2 = __file_create_wal(&f->vfs);
	void *buf_header_main = __buf_header_main_db();
	void *buf_header_wal = __buf_header_wal();
	void *buf_header_wal_frame_1 = __buf_header_wal_frame();
	void *buf_header_wal_frame_2 = __buf_header_wal_frame();
	void *buf_page_1 = __buf_page_1();
	void *buf_page_2 = __buf_page_2();

	int rc;

	sqlite3_int64 size;

	(void)params;

	/* First write the main database header, which sets the page size. */
	rc = file1->pMethods->xWrite(file1, buf_header_main, 100, 0);
	munit_assert_int(rc, ==, 0);

	/* Initial size of the WAL file is 0. */
	rc = file2->pMethods->xFileSize(file2, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	/* Truncating an empty WAL file is a no-op. */
	rc = file2->pMethods->xTruncate(file2, 0);
	munit_assert_int(rc, ==, 0);

	/* The size is still 0. */
	rc = file2->pMethods->xFileSize(file2, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	/* Write the WAL header. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal, 32, 0);
	munit_assert_int(rc, ==, 0);

	/* Write the header of the first frame. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal_frame_1, 24, 32);
	munit_assert_int(rc, ==, 0);

	/* Write the page of the first frame. */
	rc = file2->pMethods->xWrite(file2, buf_page_1, 512, 32 + 24);
	munit_assert_int(rc, ==, 0);

	/* Write the header of the second frame. */
	rc = file2->pMethods->xWrite(file2, buf_header_wal_frame_2, 24,
				     32 + 24 + 512);
	munit_assert_int(rc, ==, 0);

	/* Write the page of the second frame. */
	rc =
	    file2->pMethods->xWrite(file2, buf_page_2, 512, 32 + 24 + 512 + 24);
	munit_assert_int(rc, ==, 0);

	/* The size is 1104. */
	rc = file2->pMethods->xFileSize(file2, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 1104);

	/* Truncate the WAL file. */
	rc = file2->pMethods->xTruncate(file2, 0);
	munit_assert_int(rc, ==, 0);

	/* The size is 0. */
	rc = file2->pMethods->xFileSize(file2, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	free(buf_header_wal_frame_2);
	free(buf_header_wal_frame_1);
	free(buf_page_1);
	free(buf_page_2);
	free(buf_header_wal);
	free(buf_header_main);
	free(file1);
	free(file2);

	return MUNIT_OK;
}

/* Truncating a file which is not the main db file or the WAL file produces an
 * error. */
TEST(VfsTruncate, unexpected, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_JOURNAL;
	char buf[32];
	int rc;

	(void)params;

	/* Open a journal file. */
	rc = f->vfs.xOpen(&f->vfs, "test.db-journal", file, flags, &flags);
	munit_assert_int(rc, ==, 0);

	/* Write some content. */
	rc = file->pMethods->xWrite(file, buf, 32, 0);
	munit_assert_int(rc, ==, 0);

	/* Truncating produces an error. */
	rc = file->pMethods->xTruncate(file, 0);
	munit_assert_int(rc, ==, SQLITE_IOERR_TRUNCATE);

	free(file);

	return MUNIT_OK;
}

/* Truncating an empty file is a no-op. */
TEST(VfsTruncate, empty, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	sqlite_int64 size;
	int rc;

	(void)params;

	/* Truncating an empty file is a no-op. */
	rc = file->pMethods->xTruncate(file, 0);
	munit_assert_int(rc, ==, SQLITE_OK);

	/* Size is 0. */
	rc = file->pMethods->xFileSize(file, &size);
	munit_assert_int(rc, ==, 0);
	munit_assert_int(size, ==, 0);

	free(file);

	return MUNIT_OK;
}

/* Trying to grow an empty file produces an error. */
TEST(VfsTruncate, emptyGrow, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	int rc;

	(void)params;

	/* Truncating an empty file is a no-op. */
	rc = file->pMethods->xTruncate(file, 512);
	munit_assert_int(rc, ==, SQLITE_IOERR_TRUNCATE);

	free(file);

	return MUNIT_OK;
}

/* Trying to truncate a main database file to a size which is not a multiple of
 * the page size produces an error. */
TEST(VfsTruncate, misaligned, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	void *buf_page_1 = __buf_page_1();

	int rc;

	(void)params;

	/* Write the first page, containing the header. */
	rc = file->pMethods->xWrite(file, buf_page_1, 512, 0);
	munit_assert_int(rc, ==, 0);

	/* Truncating to an invalid size. */
	rc = file->pMethods->xTruncate(file, 400);
	munit_assert_int(rc, ==, SQLITE_IOERR_TRUNCATE);

	free(buf_page_1);
	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xShmMap
 *
 ******************************************************************************/

SUITE(VfsShmMap);

static char *test_shm_map_oom_delay[] = {"0", "1", "2", NULL};
static char *test_shm_map_oom_repeat[] = {"1", NULL};

static MunitParameterEnum test_shm_map_oom_params[] = {
    {TEST_HEAP_FAULT_DELAY, test_shm_map_oom_delay},
    {TEST_HEAP_FAULT_REPEAT, test_shm_map_oom_repeat},
    {NULL, NULL},
};

/* Out of memory when trying to initialize the internal VFS shm data struct. */
TEST(VfsShmMap, oom, setUp, tearDown, 0, test_shm_map_oom_params)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	volatile void *region;
	int rc;

	(void)params;
	(void)data;

	test_heap_fault_enable();

	rc = file->pMethods->xShmMap(file, 0, 512, 1, &region);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xShmLock
 *
 ******************************************************************************/

SUITE(VfsShmLock)

/* If an exclusive lock is in place, getting a shared lock on any index of its
 * range fails. */
TEST(VfsShmLock, sharedBusy, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	volatile void *region;
	int rc;

	(void)params;
	(void)data;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xShmMap(file, 0, 512, 1, &region);
	munit_assert_int(rc, ==, 0);

	/* Take an exclusive lock on a range. */
	flags = SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE;
	rc = file->pMethods->xShmLock(file, 2, 3, flags);
	munit_assert_int(rc, ==, 0);

	/* Attempting to get a shared lock on an index in that range fails. */
	flags = SQLITE_SHM_LOCK | SQLITE_SHM_SHARED;
	rc = file->pMethods->xShmLock(file, 3, 1, flags);
	munit_assert_int(rc, ==, SQLITE_BUSY);

	free(file);

	return MUNIT_OK;
}

/* If a shared lock is in place on any of the indexes of the requested range,
 * getting an exclusive lock fails. */
TEST(VfsShmLock, exclBusy, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	volatile void *region;
	int rc;

	(void)params;
	(void)data;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xShmMap(file, 0, 512, 1, &region);
	munit_assert_int(rc, ==, 0);

	/* Take a shared lock on index 3. */
	flags = SQLITE_SHM_LOCK | SQLITE_SHM_SHARED;
	rc = file->pMethods->xShmLock(file, 3, 1, flags);
	munit_assert_int(rc, ==, 0);

	/* Attempting to get an exclusive lock on a range that contains index 3
	 * fails. */
	flags = SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE;
	rc = file->pMethods->xShmLock(file, 2, 3, flags);
	munit_assert_int(rc, ==, SQLITE_BUSY);

	free(file);

	return MUNIT_OK;
}

/* The native unix VFS implementation from SQLite allows to release a shared
 * memory lock without acquiring it first. */
TEST(VfsShmLock, releaseUnix, setUp, tearDown, 0, NULL)
{
	(void)data;
	struct sqlite3_vfs *vfs = sqlite3_vfs_find("unix");
	sqlite3_file *file = munit_malloc(vfs->szOsFile);
	int flags =
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	char *dir = test_dir_setup();
	char buf[1024];
	char *path;
	volatile void *region;
	int rc;

	(void)params;
	(void)data;

	/* The SQLite pager stores the Database filename, Journal filename, and
	 * WAL filename consecutively in memory, in that order. The database
	 * filename is prefixed by four zero bytes. Emulate that behavior here,
	 * since the internal SQLite code triggered by the xShmMap unix
	 * implementation relies on that.*/
	memset(buf, 0, sizeof buf);
	path = buf + 4;
	sprintf(path, "%s/test.db", dir);

	rc = vfs->xOpen(vfs, path, file, flags, &flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xShmMap(file, 0, 4096, 1, &region);
	munit_assert_int(rc, ==, 0);

	flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE;
	rc = file->pMethods->xShmLock(file, 3, 1, flags);
	munit_assert_int(rc, ==, 0);

	flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED;
	rc = file->pMethods->xShmLock(file, 2, 1, flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xShmUnmap(file, 1);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, 0);

	test_dir_tear_down(dir);

	free(file);

	return MUNIT_OK;
}

/* The dqlite VFS implementation allows to release a shared memory lock without
 * acquiring it first. This is important because at open time sometimes SQLite
 * will do just that (release before acquire). */
TEST(VfsShmLock, release, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = munit_malloc(f->vfs.szOsFile);
	int flags = SQLITE_OPEN_CREATE | SQLITE_OPEN_MAIN_DB;
	volatile void *region;
	int rc;

	(void)params;
	(void)data;

	rc = f->vfs.xOpen(&f->vfs, "test.db", file, flags, &flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xShmMap(file, 0, 512, 1, &region);
	munit_assert_int(rc, ==, 0);

	flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED;
	rc = file->pMethods->xShmLock(file, 3, 1, flags);
	munit_assert_int(rc, ==, 0);

	flags = SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED;
	rc = file->pMethods->xShmLock(file, 2, 1, flags);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xShmUnmap(file, 1);
	munit_assert_int(rc, ==, 0);

	rc = file->pMethods->xClose(file);
	munit_assert_int(rc, ==, 0);

	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xFileControl
 *
 ******************************************************************************/

SUITE(VfsFileControl)

/* Trying to set the page size to a value different than the current one
 * produces an error. */
TEST(VfsFileControl, pageSize, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	char *fnctl[] = {
	    "",
	    "page_size",
	    "512",
	    "",
	};
	int rc;

	(void)params;
	(void)data;

	/* Setting the page size a first time returns NOTFOUND, which is what
	 * SQLite effectively expects. */
	rc = file->pMethods->xFileControl(file, SQLITE_FCNTL_PRAGMA, fnctl);
	munit_assert_int(rc, ==, SQLITE_NOTFOUND);

	/* Trying to change the page size results in an error. */
	fnctl[2] = "1024";
	rc = file->pMethods->xFileControl(file, SQLITE_FCNTL_PRAGMA, fnctl);
	munit_assert_int(rc, ==, SQLITE_IOERR);

	free(file);

	return MUNIT_OK;
}

/* Trying to set the journal mode to anything other than "wal" produces an
 * error. */
TEST(VfsFileControl, journal, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3_file *file = __file_create_main_db(&f->vfs);
	char *fnctl[] = {
	    "",
	    "journal_mode",
	    "memory",
	    "",
	};
	int rc;

	(void)params;
	(void)data;

	/* Setting the page size a first time returns NOTFOUND, which is what
	 * SQLite effectively expects. */
	rc = file->pMethods->xFileControl(file, SQLITE_FCNTL_PRAGMA, fnctl);
	munit_assert_int(rc, ==, SQLITE_IOERR);

	free(file);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xCurrentTime
 *
 ******************************************************************************/

SUITE(VfsCurrentTime)

TEST(VfsCurrentTime, success, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	double now;
	int rc;

	(void)params;

	rc = f->vfs.xCurrentTime(&f->vfs, &now);
	munit_assert_int(rc, ==, SQLITE_OK);

	munit_assert_double(now, >, 0);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * xSleep
 *
 ******************************************************************************/

SUITE(VfsSleep)

/* The xSleep implementation is a no-op. */
TEST(VfsSleep, success, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	int microseconds;

	(void)params;

	microseconds = f->vfs.xSleep(&f->vfs, 123);

	munit_assert_int(microseconds, ==, 123);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * VfsInit
 *
 ******************************************************************************/

SUITE(VfsInit);

static char *test_create_oom_delay[] = {"0", "1", NULL};
static char *test_create_oom_repeat[] = {"1", NULL};

static MunitParameterEnum test_create_oom_params[] = {
    {TEST_HEAP_FAULT_DELAY, test_create_oom_delay},
    {TEST_HEAP_FAULT_REPEAT, test_create_oom_repeat},
    {NULL, NULL},
};

TEST(VfsInit, oom, setUp, tearDown, 0, test_create_oom_params)
{
	struct sqlite3_vfs vfs;
	int rv;

	(void)params;
	(void)data;

	test_heap_fault_enable();

	rv = VfsInit(&vfs, "dqlite");
	munit_assert_int(rv, ==, DQLITE_NOMEM);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * Integration
 *
 ******************************************************************************/

SUITE(VfsIntegration)

/* Integration test, registering an in-memory VFS and performing various
 * database operations. */
TEST(VfsIntegration, db, setUp, tearDown, 0, NULL)
{
	sqlite3 *db;
	sqlite3_stmt *stmt;
	const char *tail;
	int i;
	int size;
	int ckpt;
	int rc;

	(void)data;
	(void)params;

	db = __db_open();

	/* Create a test table and insert a few rows into it. */
	__db_exec(db, "CREATE TABLE test (n INT)");

	rc = sqlite3_prepare(db, "INSERT INTO test(n) VALUES(?)", -1, &stmt,
			     &tail);
	munit_assert_int(rc, ==, SQLITE_OK);

	for (i = 0; i < 100; i++) {
		rc = sqlite3_bind_int(stmt, 1, i);
		munit_assert_int(rc, ==, SQLITE_OK);

		rc = sqlite3_step(stmt);
		munit_assert_int(rc, ==, SQLITE_DONE);

		rc = sqlite3_reset(stmt);
		munit_assert_int(rc, ==, SQLITE_OK);
	}

	rc = sqlite3_finalize(stmt);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_TRUNCATE,
				       &size, &ckpt);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = sqlite3_close(db);
	munit_assert_int(rc, ==, SQLITE_OK);

	return MUNIT_OK;
}

/* Test our expections on the memory-mapped WAl index format. */
TEST(VfsIntegration, wal, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	uint32_t *read_marks;
	int i;

	(void)data;
	(void)params;

	db1 = __db_open();
	db2 = __db_open();

	__db_exec(db1, "CREATE TABLE test (n INT)");

	munit_assert_int(__wal_idx_mx_frame(db1), ==, 2);

	read_marks = __wal_idx_read_marks(db1);
	munit_assert_uint32(read_marks[0], ==, 0);
	munit_assert_uint32(read_marks[1], ==, 0);
	munit_assert_uint32(read_marks[2], ==, 0xffffffff);
	munit_assert_uint32(read_marks[3], ==, 0xffffffff);
	munit_assert_uint32(read_marks[4], ==, 0xffffffff);
	free(read_marks);

	/* Start a read transaction on db2 */
	__db_exec(db2, "BEGIN");
	__db_exec(db2, "SELECT * FROM test");

	/* The max frame is set to 2, which is the current size of the WAL. */
	munit_assert_int(__wal_idx_mx_frame(db2), ==, 2);

	/* The starting mx frame value has been saved in the read marks */
	read_marks = __wal_idx_read_marks(db2);
	munit_assert_uint32(read_marks[0], ==, 0);
	munit_assert_uint32(read_marks[1], ==, 2);
	munit_assert_uint32(read_marks[2], ==, 0xffffffff);
	munit_assert_uint32(read_marks[3], ==, 0xffffffff);
	munit_assert_uint32(read_marks[4], ==, 0xffffffff);
	free(read_marks);

	/* A shared lock is held on the second read mark (read locks start at
	 * 3). */
	munit_assert_true(__shm_shared_lock_held(db2, 3 + 1));

	/* Start a write transaction on db1 */
	__db_exec(db1, "BEGIN");

	for (i = 0; i < 100; i++) {
		__db_exec(db1, "INSERT INTO test(n) VALUES(1)");
	}

	/* The mx frame is still 2 since the transaction is not committed. */
	munit_assert_int(__wal_idx_mx_frame(db1), ==, 2);

	/* No extra read mark wal taken. */
	read_marks = __wal_idx_read_marks(db1);
	munit_assert_uint32(read_marks[0], ==, 0);
	munit_assert_uint32(read_marks[1], ==, 2);
	munit_assert_uint32(read_marks[2], ==, 0xffffffff);
	munit_assert_uint32(read_marks[3], ==, 0xffffffff);
	munit_assert_uint32(read_marks[4], ==, 0xffffffff);
	free(read_marks);

	__db_exec(db1, "COMMIT");

	/* The mx frame is now 6. */
	munit_assert_int(__wal_idx_mx_frame(db1), ==, 6);

	/* The old read lock is still in place. */
	munit_assert_true(__shm_shared_lock_held(db2, 3 + 1));

	/* Start a read transaction on db1 */
	__db_exec(db1, "BEGIN");
	__db_exec(db1, "SELECT * FROM test");

	/* The mx frame is still unchanged. */
	munit_assert_int(__wal_idx_mx_frame(db1), ==, 6);

	/* A new read mark was taken. */
	read_marks = __wal_idx_read_marks(db1);
	munit_assert_uint32(read_marks[0], ==, 0);
	munit_assert_uint32(read_marks[1], ==, 2);
	munit_assert_uint32(read_marks[2], ==, 6);
	munit_assert_uint32(read_marks[3], ==, 0xffffffff);
	munit_assert_uint32(read_marks[4], ==, 0xffffffff);
	free(read_marks);

	/* The old read lock is still in place. */
	munit_assert_true(__shm_shared_lock_held(db2, 3 + 1));

	/* The new read lock is in place as well. */
	munit_assert_true(__shm_shared_lock_held(db2, 3 + 2));

	__db_close(db1);
	__db_close(db2);

	return SQLITE_OK;
}

/* Full checkpoints are possible only when no read mark is set. */
TEST(VfsIntegration, checkpoint, setUp, tearDown, 0, NULL)
{
	sqlite3 *db1;
	sqlite3 *db2;
	sqlite3_file *file1; /* main DB file */
	sqlite3_file *file2; /* WAL file */
	sqlite_int64 size;
	uint32_t *read_marks;
	unsigned mx_frame;
	char stmt[128];
	int log, ckpt;
	int i;
	int rv;

	(void)data;
	(void)params;

	db1 = __db_open();

	__db_exec(db1, "CREATE TABLE test (n INT)");

	/* Insert a few rows so we grow the size of the WAL. */
	__db_exec(db1, "BEGIN");

	for (i = 0; i < 500; i++) {
		sprintf(stmt, "INSERT INTO test(n) VALUES(%d)", i);
		__db_exec(db1, stmt);
	}

	__db_exec(db1, "COMMIT");

	/* Get the file objects for the main database and the WAL. */
	rv = sqlite3_file_control(db1, "main", SQLITE_FCNTL_FILE_POINTER,
				  &file1);
	munit_assert_int(rv, ==, 0);

	rv = sqlite3_file_control(db1, "main", SQLITE_FCNTL_JOURNAL_POINTER,
				  &file2);
	munit_assert_int(rv, ==, 0);

	/* The WAL file has now 13 pages */
	rv = file2->pMethods->xFileSize(file2, &size);
	munit_assert_int(format__wal_calc_pages(512, size), ==, 13);

	mx_frame = __wal_idx_mx_frame(db1);
	munit_assert_int(mx_frame, ==, 13);

	/* Start a read transaction on a different connection, acquiring a
	 * shared lock on all WAL pages. */
	db2 = __db_open();
	__db_exec(db2, "BEGIN");
	__db_exec(db2, "SELECT * FROM test");

	read_marks = __wal_idx_read_marks(db1);
	munit_assert_int(read_marks[1], ==, 13);
	free(read_marks);

	rv = file1->pMethods->xShmLock(file1, 3 + 1, 1,
				       SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE);
	munit_assert_int(rv, ==, SQLITE_BUSY);

	munit_assert_true(__shm_shared_lock_held(db1, 3 + 1));

	/* Execute a new write transaction, deleting some of the pages we
	 * inserted and creating new ones. */
	__db_exec(db1, "BEGIN");
	__db_exec(db1, "DELETE FROM test WHERE n > 200");

	for (i = 0; i < 1000; i++) {
		sprintf(stmt, "INSERT INTO test(n) VALUES(%d)", i);
		__db_exec(db1, stmt);
	}

	__db_exec(db1, "COMMIT");

	/* Since there's a shared read lock, a full checkpoint will fail. */
	rv = sqlite3_wal_checkpoint_v2(db1, "main", SQLITE_CHECKPOINT_TRUNCATE,
				       &log, &ckpt);
	munit_assert_int(rv, !=, 0);

	/* If we complete the read transaction the shared lock is realeased and
	 * the checkpoint succeeds. */
	__db_exec(db2, "COMMIT");

	rv = sqlite3_wal_checkpoint_v2(db1, "main", SQLITE_CHECKPOINT_TRUNCATE,
				       &log, &ckpt);
	munit_assert_int(rv, ==, 0);

	__db_close(db1);
	__db_close(db2);

	return SQLITE_OK;
}

/******************************************************************************
 *
 * vfs file read/write
 *
 ******************************************************************************/

SUITE(VfsFileRead);

/* If the file being read does not exists, an error is returned. */
TEST(VfsFileRead, cantOpen, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	void *buf;
	size_t len;
	int rv;
	(void)params;
	rv = VfsFileRead(f->vfs.zName, "test.db", &buf, &len);
	munit_assert_int(rv, ==, SQLITE_CANTOPEN);
	return MUNIT_OK;
}

/* Read the content of an empty file. */
TEST(VfsFileRead, empty, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3 *db;
	void *buf;
	size_t len;
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
	int rv;

	(void)params;

	rv = sqlite3_open_v2("test.db", &db, flags, f->vfs.zName);
	munit_assert_int(rv, ==, SQLITE_OK);

	rv = VfsFileRead(f->vfs.zName, "test.db", &buf, &len);
	munit_assert_int(rv, ==, SQLITE_OK);

	munit_assert_ptr_null(buf);
	munit_assert_int(len, ==, 0);

	rv = sqlite3_close(db);
	munit_assert_int(rv, ==, SQLITE_OK);

	return MUNIT_OK;
}

/* Read the content of a database and WAL files and then write them back. */
TEST(VfsFileRead, thenWrite, setUp, tearDown, 0, NULL)
{
	struct fixture *f = data;
	sqlite3 *db = __db_open();
	int rc;
	void *buf1;
	void *buf2;
	size_t len1;
	size_t len2;
	sqlite3_stmt *stmt;
	const char *tail;

	(void)params;

	__db_exec(db, "CREATE TABLE test (n INT)");

	rc = VfsFileRead(f->vfs.zName, "test.db", &buf1, &len1);
	munit_assert_int(rc, ==, SQLITE_OK);

	munit_assert_ptr_not_equal(buf1, NULL);
	munit_assert_int(len1, ==, 512);

	rc = VfsFileRead(f->vfs.zName, "test.db-wal", &buf2, &len2);
	munit_assert_int(rc, ==, SQLITE_OK);

	munit_assert_ptr_not_equal(buf2, NULL);
	munit_assert_int(len2, ==, 1104);

	rc = sqlite3_close(db);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = VfsFileWrite(f->vfs.zName, "test.db", buf1, len1);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = VfsFileWrite(f->vfs.zName, "test.db-wal", buf2, len2);
	munit_assert_int(rc, ==, SQLITE_OK);

	raft_free(buf1);
	raft_free(buf2);

	rc = sqlite3_open_v2("test.db", &db, SQLITE_OPEN_READWRITE,
			     f->vfs.zName);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = sqlite3_prepare(db, "INSERT INTO test(n) VALUES(?)", -1, &stmt,
			     &tail);
	munit_assert_int(rc, ==, SQLITE_OK);

	rc = sqlite3_finalize(stmt);
	munit_assert_int(rc, ==, SQLITE_OK);

	__db_close(db);

	return MUNIT_OK;
}

static char *file_read_oom_delay[] = {"0", NULL};
static char *file_read_oom_repeat[] = {"1", NULL};

static MunitParameterEnum file_read_oom_params[] = {
    {TEST_HEAP_FAULT_DELAY, file_read_oom_delay},
    {TEST_HEAP_FAULT_REPEAT, file_read_oom_repeat},
    {NULL, NULL},
};

/* Test out of memory scenarios. */
TEST(VfsFileRead, oom, setUp, tearDown, 0, file_read_oom_params)
{
	struct fixture *f = data;
	sqlite3 *db = __db_open();
	void *buf;
	size_t len;
	int rc;

	(void)params;

	__db_exec(db, "CREATE TABLE test (n INT)");

	test_heap_fault_enable();

	rc = VfsFileRead(f->vfs.zName, "test.db", &buf, &len);
	munit_assert_int(rc, ==, SQLITE_NOMEM);

	rc = sqlite3_close(db);
	munit_assert_int(rc, ==, SQLITE_OK);

	return MUNIT_OK;
}
