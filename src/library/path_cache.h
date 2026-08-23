#ifndef PATH_CACHE_H
#define PATH_CACHE_H

#include <stdbool.h>

/* Small path-list files next to the tagcache directory. Used for app
 * lists tagcache itself does not store (playlist index, books). */

#define PATH_CACHE_PLAYLISTS "playlists.list"
#define PATH_CACHE_BOOKS "books.list"
#define PATH_CACHE_BOOK_FAVORITES "book_favorites.list"

void path_cache_replace(const char * name, char * const * paths, int count);
void path_cache_load(const char * name, char *** out_paths, int * out_count);
void path_cache_load_matching(const char * name, const char * filter_name, char *** out_paths, int * out_count);
void path_cache_insert(const char * name, const char * path);
void path_cache_delete(const char * name, const char * path);
bool path_cache_has(const char * name, const char * path);

#endif /* PATH_CACHE_H */
