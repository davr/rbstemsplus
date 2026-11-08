/* rekordbox's library (master.db, SQLCipher): its playlists, read only */
#ifndef RBSP_LIBRARY_H
#define RBSP_LIBRARY_H
#include <stdio.h>

typedef struct library library;

/* rekordbox's library for this user: ~/Library/Pioneer/rekordbox/master.db, the home folder from
 * the user database (never HOME); "" without one */
void library_default_path(char *buf, size_t n);

/* opens the library read only, with the key built in; NULL and *why on failure */
library *library_open(const char *path, char *why, size_t n);
void library_close(library *l);

/* every playlist and folder as JSON lines, parents before children, each level in rekordbox's
 * order: {"playlist", "parent", "name", "kind": "playlist"|"folder"|"intelligent"|"other",
 * "tracks"} ("tracks": a playlist's rows, duplicates included; 0 for the others). 0, or -1 and
 * *why */
int library_list(library *l, FILE *out, char *why, size_t n);

/* playlist ID's tracks, in rekordbox's order, each path once (a track listed twice is prepared
 * once): *paths (malloc'd strings in a malloc'd array) and *count. Only a plain playlist: a
 * folder or an intelligent playlist is refused with *why. 0, or -1 and *why */
int library_tracks(library *l, const char *id, char ***paths, size_t *count, char *why, size_t n);

#endif
