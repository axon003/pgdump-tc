pgdump — Total Commander plugins for PostgreSQL custom-format dumps (pg_dump -Fc)
================================================================================
Author : Gabriel Diaconu    License: PolyForm Noncommercial 1.0.0 (free, non-commercial)
Platform: Windows 64-bit
Supports PostgreSQL 11 .. 17 dump archives (archive versions 1.13 .. 1.16).

There are TWO plugins, distributed separately:

1) pgdump.wcx64  — PACKER plugin (WCX)
   Browse a pg_dump custom-format archive (.dump / .fc) like a folder tree:
     - schemas appear as folders, objects (tables, views, sequences,
       functions, types, indexes, triggers, ...) appear as files "name.pg"
     - a table's "size" column shows its number of data rows
     - read-only; handles plain (gzip) archives and the eRGiss extensions
       (zstd compression, AES-256 encryption — prompts for a password)
   Standalone: zlib / zstd / libgcrypt are linked into the DLL.

2) pgdump.wlx64  — LISTER plugin (WLX)  [companion of the WCX]
   Press F3 on an extracted "name.pg" object file to view it:
     - tab 1  Data  : Excel-like grid with cell selection
                      arrows / Ctrl+arrows / Shift+arrows, Ctrl+C = TSV (paste
                      into Excel); SQL NULL shown as "null"
     - tab 2  SQL   : the CREATE statement
     - tab 3  Info  : dependent indexes / constraints / triggers / rules
     - tab 4  Full  : complete reproducible script (CREATE + COPY data +
                      indexes); Ctrl+C copies it all — paste straight into psql
     Esc closes the lister.
   The WLX only makes sense together with the WCX (it views the .pg files the
   WCX produces).

Installation
------------
Open each plugin's zip in Total Commander and confirm the install prompt
(pluginst.inf auto-registers it), or copy the .wcx64 / .wlx64 into a folder and
add it under Configuration > Options > Plugins.

Notes
-----
- Read-only: these plugins never write to or modify your dump files.
- The archive format is the standard pg_dump custom format; the optional
  zstd/AES variants are produced by a companion custom pg_dump build.
