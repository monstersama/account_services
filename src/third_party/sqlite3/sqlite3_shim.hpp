#pragma once

// 优先使用系统 sqlite3 头文件；缺失时提供最小 ABI 声明用于编译。
#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#else

extern "C" {

typedef long long sqlite3_int64;

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

int sqlite3_open_v2(const char* filename, sqlite3** pp_db, int flags, const char* z_vfs);
int sqlite3_close(sqlite3* db);

int sqlite3_prepare_v2(
    sqlite3* db, const char* z_sql, int n_byte, sqlite3_stmt** pp_stmt, const char** pz_tail);
int sqlite3_finalize(sqlite3_stmt* p_stmt);
int sqlite3_bind_int64(sqlite3_stmt* p_stmt, int i, sqlite3_int64 value);
int sqlite3_step(sqlite3_stmt* p_stmt);

int sqlite3_column_type(sqlite3_stmt* p_stmt, int i_col);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt* p_stmt, int i_col);
const unsigned char* sqlite3_column_text(sqlite3_stmt* p_stmt, int i_col);
int sqlite3_column_bytes(sqlite3_stmt* p_stmt, int i_col);

int sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** err_msg);
void sqlite3_free(void* p);

}  // extern "C"

#ifndef SQLITE_OK
#define SQLITE_OK 0
#endif
#ifndef SQLITE_ROW
#define SQLITE_ROW 100
#endif
#ifndef SQLITE_DONE
#define SQLITE_DONE 101
#endif

#ifndef SQLITE_INTEGER
#define SQLITE_INTEGER 1
#endif
#ifndef SQLITE_TEXT
#define SQLITE_TEXT 3
#endif

#ifndef SQLITE_OPEN_READONLY
#define SQLITE_OPEN_READONLY 0x00000001
#endif
#ifndef SQLITE_OPEN_READWRITE
#define SQLITE_OPEN_READWRITE 0x00000002
#endif
#ifndef SQLITE_OPEN_CREATE
#define SQLITE_OPEN_CREATE 0x00000004
#endif

#endif  // __has_include(<sqlite3.h>)
