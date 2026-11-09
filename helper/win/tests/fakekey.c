/* the test sqlite3.dll's one addition: SQLCipher's sqlite3_key, which accepts any key (the test database isn't encrypted) */
__declspec(dllexport) int sqlite3_key(void *db, const void *key, int n) { (void)db; (void)key; (void)n; return 0; }
