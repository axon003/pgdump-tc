#!/bin/bash
# build_wcx_pg.sh — cross-build pgdump.wcx64 (and .wcx 32-bit) Total Commander
# packer plugin for pg_dump custom-format archives. Standalone (pgfc + zlib +
# zstd + libgcrypt linked static). @author Gabriel Diaconu
set -e
ARCH="${1:-64}"
WCX=/root/pg_wcx
if [ "$ARCH" = "32" ]; then
  CC=i686-w64-mingw32-gcc; DEPS=/root/erg_fsa_build/windeps32; OUT=$WCX/pgdump.wcx
else
  CC=x86_64-w64-mingw32-gcc; DEPS=/root/erg_fsa_build/windeps; OUT=$WCX/pgdump.wcx64
fi
DEFS="-DWIN32 -D_WIN32_WINNT=0x0601"
INCS="-I$WCX -I$DEPS/include"
LIBS="-L$DEPS/lib -lzstd -lgcrypt -lgpg-error -lz -lwinpthread -lws2_32 -luser32 -lgdi32 -lkernel32"
CFLAGS="-O2 -Wall -Wno-unused-function -Wno-unused-variable"
LDSTATIC="-static -static-libgcc"
echo ">>> building $OUT ($ARCH-bit) with $CC"
$CC $CFLAGS $DEFS $INCS -shared $WCX/pgfc_lib.c $WCX/wcx_pgdump.c $WCX/pgdump.def $LDSTATIC $LIBS -o "$OUT"
echo ">>> built:"; ls -la "$OUT"
echo ">>> external DLLs:"; ${CC%-gcc}-objdump -p "$OUT" | grep 'DLL Name' || echo "(none)"
md5sum "$OUT"
