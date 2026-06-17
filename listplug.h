/* listplug.h — minimal Total Commander Lister-plugin (WLX) API subset. */
#ifndef LISTPLUG_H
#define LISTPLUG_H
#include <windows.h>

/* ShowFlags bits */
#define lcp_ansi          1
#define lcp_unicode       2
#define lcp_forceshow     4
#define lcp_wraptext      8
#define lcp_fittowindow  16
#define lcp_ansiutf8     32

/* ListSendCommand commands */
#define lc_copy           1
#define lc_newparams      2
#define lc_selectall      3
#define lc_setpercent     4

/* return codes */
#define LISTPLUGIN_OK     0
#define LISTPLUGIN_ERROR  1

#endif
