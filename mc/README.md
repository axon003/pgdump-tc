# Midnight Commander (mc) plugin — browse pg_dump dumps on Linux

An mc **extfs** helper that browses a PostgreSQL `pg_dump -Fc` dump as a virtual
filesystem: schemas are folders, objects are runnable **`.sql`** files
(CREATE + COPY data + indexes). Read-only; view with mc's built-in F3 viewer.

## Files
- `pgfc_cli.c` — the CLI (`pgfc list` / `pgfc copyout`) built on the portable
  reader `../pgfc_lib.c`. Build static so it runs on any Linux:
  `gcc -O2 -static pgfc_cli.c ../pgfc_lib.c -lzstd -lgcrypt -lgpg-error -lz -o pgfc`
- `upgdump` — the extfs helper (calls `pgfc`).

## Install
```sh
cp pgfc /usr/local/bin/                       # the CLI in PATH
cp upgdump /usr/libexec/mc/extfs.d/           # (or /usr/lib/mc/extfs.d)
chmod 755 /usr/libexec/mc/extfs.d/upgdump
# register in /etc/mc/mc.ext  (mc < 4.8.21 syntax):
#   shell/i/.dump
#       Open=%cd %p/upgdump://
#       View=%view{ascii} /usr/local/bin/pgfc list %f
```
Then press Enter on a `.dump`/`.fc` file in mc. Encrypted dumps: `export PGFC_PW=...`.

Companion: `ufsa` (in the eRGfsarchiver project) does the same for `.fsa` archives.
