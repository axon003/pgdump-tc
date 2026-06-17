/*
 * wcxhead.h -- minimal Total Commander packer-plugin (WCX) API definitions.
 * Subset needed for a read-only archive plugin. Public TC SDK constants.
 */
#ifndef WCXHEAD_H
#define WCXHEAD_H

#include <windows.h>

/* OpenArchive OpenMode */
#define PK_OM_LIST     0
#define PK_OM_EXTRACT  1

/* ProcessFile Operation */
#define PK_SKIP        0
#define PK_TEST        1
#define PK_EXTRACT     2

/* tHeaderData.FileAttr is DOS attributes; dirs use FILE_ATTRIBUTE_DIRECTORY */

/* Error codes returned by ReadHeader/ProcessFile */
#define E_SUCCESS         0
#define E_END_ARCHIVE    10
#define E_NO_MEMORY      11
#define E_BAD_DATA       12
#define E_BAD_ARCHIVE    13
#define E_UNKNOWN_FORMAT 14
#define E_EOPEN          15
#define E_ECREATE        16
#define E_ECLOSE         17
#define E_EREAD          18
#define E_EWRITE         19
#define E_SMALL_BUF      20
#define E_EABORTED       21
#define E_NO_FILES       22
#define E_TOO_MANY_FILES 23
#define E_NOT_SUPPORTED  24

/* GetPackerCaps bits */
#define PK_CAPS_NEW        1
#define PK_CAPS_MODIFY     2
#define PK_CAPS_MULTIPLE   4
#define PK_CAPS_DELETE     8
#define PK_CAPS_OPTIONS   16
#define PK_CAPS_MEMPACK   32
#define PK_CAPS_BY_CONTENT 64
#define PK_CAPS_SEARCHTEXT 128
#define PK_CAPS_HIDE      256
#define PK_CAPS_ENCRYPT   512

/* tProcessDataProc return: 0 = abort */
typedef int  (__stdcall *tProcessDataProc)(char *FileName, int Size);
typedef int  (__stdcall *tProcessDataProcW)(WCHAR *FileName, int Size);
typedef int  (__stdcall *tChangeVolProc)(char *ArcName, int Mode);
typedef int  (__stdcall *tChangeVolProcW)(WCHAR *ArcName, int Mode);

typedef struct {
    char *ArcName;
    int   OpenMode;
    int   OpenResult;
    char *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
} tOpenArchiveData;

typedef struct {
    WCHAR *ArcName;
    int    OpenMode;
    int    OpenResult;
    WCHAR *CmtBuf;
    int    CmtBufSize;
    int    CmtSize;
    int    CmtState;
} tOpenArchiveDataW;

typedef struct {
    char  ArcName[260];
    char  FileName[260];
    int   Flags;
    int   PackSize;
    int   UnpSize;
    int   HostOS;
    int   FileCRC;
    int   FileTime;
    int   FileAttr;
    char *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
} tHeaderData;

typedef struct {
    char  ArcName[1024];
    char  FileName[1024];
    int   Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
    int   HostOS;
    int   FileCRC;
    int   FileTime;
    int   FileAttr;
    char *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
    char  Reserved[1024];
} tHeaderDataEx;

typedef struct {
    WCHAR ArcName[1024];
    WCHAR FileName[1024];
    int   Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
    int   HostOS;
    int   FileCRC;
    int   FileTime;
    int   FileAttr;
    WCHAR *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
    char  Reserved[1024];
} tHeaderDataExW;

#endif /* WCXHEAD_H */
