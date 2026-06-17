# pgdump-tc — Total Commander plugins for PostgreSQL `pg_dump` custom dumps

Browse and read PostgreSQL **custom-format** dumps (`pg_dump -Fc`) directly inside
[Total Commander](https://www.ghisler.com/): schemas become folders, database
objects (tables, views, sequences, functions, types, indexes, triggers …) become
files, and tables open in an **Excel-like data grid**.

Works on **PostgreSQL 11 … 17** dump archives (archive versions 1.13 … 1.16),
including plain (gzip) archives and the optional eRGiss extensions
(zstd compression, AES-256 encryption).

By **Gabriel Diaconu** — part of the [eRGiss](https://ergiss.ro) tooling suite.
Project page: **https://ergiss.ro/pgdump**

---

## Two plugins

### 1. `pgdump.wcx64` — packer plugin (WCX)
Open a `.dump` / `.fc` archive like a folder tree:
- **schemas → folders**, **objects → `name.pg` files** (read-only);
- a table's *Size* column shows its **number of data rows**;
- handles plain (gzip) archives and the eRGiss zstd/AES-256 variants
  (prompts for a password when the archive is encrypted);
- standalone — zlib, Zstandard and libgcrypt are linked into the DLL.

### 2. `pgdump.wlx64` — lister plugin (WLX) — companion of the WCX
Press **F3** on a `name.pg` object file:
- **Tab 1 · Data** — Excel-like grid with cell selection:
  arrows / `Ctrl`+arrows / `Shift`+arrows, `Ctrl+C` copies the selection as
  tab-separated text (paste into Excel). SQL `NULL` is shown as `null`.
- **Tab 2 · SQL** — the `CREATE` statement.
- **Tab 3 · Info** — dependent indexes / constraints / triggers / rules.
- **Tab 4 · Full SQL** — a complete reproducible script (`CREATE` + `COPY` data +
  indexes); `Ctrl+C` copies the whole thing — **paste straight into `psql`**.
- `Esc` closes the lister.

> The WLX only makes sense together with the WCX (it views the `.pg` files the
> WCX extracts).

---

## Install

Download the latest [release](https://github.com/axon003/pgdump-tc/releases),
then **open each plugin's zip in Total Commander** and confirm the install prompt
(the bundled `pluginst.inf` registers it automatically). Or copy the `.wcx64` /
`.wlx64` into a folder and add it under *Configuration → Options → Plugins*.

Read-only: these plugins never write to or modify your dump files.

---

## Build (cross-compile on Linux with MinGW-w64)

```sh
# WCX (needs static zlib, zstd, libgcrypt, libgpg-error for the target):
bash build_wcx_pg.sh 64
# WLX (only system libraries):
x86_64-w64-mingw32-gcc -O2 -DWIN32 -D_WIN32_WINNT=0x0601 -I. -shared \
    wlx_pgdump.c pgdump_wlx.def -static -static-libgcc \
    -lcomctl32 -luser32 -lgdi32 -lkernel32 -o pgdump.wlx64
```

A portable reader library (`pgfc_lib.c` / `pgfc.h`) does all the archive parsing
and can be reused on its own.

---

## License

**PolyForm Noncommercial 1.0.0** — free for non-commercial use. See [LICENSE](LICENSE).

Bundled third-party libraries keep their own licenses: zlib (zlib license),
Zstandard (BSD), libgcrypt (LGPL-2.1) — the full source here lets you relink them.

---

## Links

- 🌐 Project page — https://ergiss.ro/pgdump
- 🏠 Main site — https://ergiss.ro
- 🧩 More Total Commander plugins (author **axon003**) — https://totalcmd.net/plugring/
- 💾 Releases — https://github.com/axon003/pgdump-tc/releases
