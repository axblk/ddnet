/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include <engine/sqlite.h>

// SQLite for a program that keeps no database. A tool that turns one demo into
// one video file has nothing to store between runs, so the library stays out of
// its build and every handle these hand out is empty. What asks for one gets
// nothing and says so, which is the same answer it would get from a database it
// could not open.

void CSqliteDeleter::operator()(sqlite3 *)
{
}

void CSqliteStmtDeleter::operator()(sqlite3_stmt *)
{
}

int SqliteHandleError(int Error, sqlite3 *, const char *)
{
	return Error;
}

CSqlite SqliteOpen(IStorage *, const char *)
{
	return nullptr;
}

CSqliteStmt SqlitePrepare(sqlite3 *, const char *)
{
	return nullptr;
}

#endif
