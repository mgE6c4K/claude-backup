/* Zip archive writer (deflate) and extractor, built on zlib. ZIP64 aware. */
#ifndef CB_ZIP_H
#define CB_ZIP_H
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct ZipWriter ZipWriter;

ZipWriter *zip_create(const char *path);                       /* NULL on error */
int  zip_add_file(ZipWriter *z, const char *src, const char *arcname); /* 0 ok, -1 read error */
int  zip_add_data(ZipWriter *z, const char *arcname, const void *data, size_t len);
int  zip_close(ZipWriter *z);                                   /* writes central directory */

/* Extracts every entry under dest. Rejects absolute names and "..". Returns 0 or -1.
   On error *err (static string) says why. */
int  zip_extract(const char *zip, const char *dest, const char **err);

/* Lists entry names (malloc'd array of malloc'd strings); returns count or -1. */
long zip_list(const char *zip, char ***names);

#endif
