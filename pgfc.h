/*
 * pgfc.h — portable reader for pg_dump custom-format (-Fc) archives, incl. the
 * eRGiss extensions (zstd, AES-256). Used by the CLI test and the WCX plugin.
 * @author Gabriel Diaconu
 */
#ifndef PGFC_H
#define PGFC_H

#include <stdio.h>

typedef struct Pgfc Pgfc;

/* A virtual filesystem node presented to Total Commander. */
typedef struct {
	char  path[512];      /* "schema" (dir) or "schema\\object" (file), backslashes */
	int   is_dir;
	long long size;       /* uncompressed content size, or 0 if unknown */
	long long mtime;      /* archive creation time (unix) */
	int   ent_index;      /* index into the TOC entry array (files only; -1 dirs) */
} PgfcNode;

/* Open archive; pw may be NULL (required only if encrypted).
 * On error returns NULL and fills errbuf. */
Pgfc *pgfc_open(const char *path, const char *pw, char *errbuf, int errlen);
void  pgfc_close(Pgfc *a);

/* Is the archive encrypted?  (cheap: reads header only) — static helper. */
int   pgfc_is_encrypted(const char *path);
/* Does the file look like a pg_dump custom archive? (magic "PGDMP") */
int   pgfc_detect(const char *path);

/* Virtual node list (dirs for schemas + files for objects). */
int   pgfc_node_count(Pgfc *a);
const PgfcNode *pgfc_node(Pgfc *a, int i);

/* Write the sectioned content of a file node to `out`.
 * Container: line "ERGPGT01", then [SQL]/[INFO]/[DATA] sections. */
int   pgfc_emit_node(Pgfc *a, int node_index, FILE *out, char *errbuf, int errlen);

#endif
