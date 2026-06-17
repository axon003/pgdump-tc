/*
 * wcx_pgdump.c -- Total Commander packer plugin (.wcx) that browses pg_dump
 *                 custom-format (-Fc) archives as a filesystem: schemas are
 *                 folders, objects (tables/views/functions/...) are files.
 *                 Handles standard archives and the eRGiss extensions
 *                 (zstd compression, AES-256 encryption — prompts for password).
 *                 Read-only. Extracting a file yields a sectioned text
 *                 ([SQL]/[INFO]/[DATA]) consumed by the pgdump WLX lister.
 *                 STANDALONE: the reader (pgfc) + zlib/zstd/libgcrypt are linked
 *                 into the DLL. No external process.
 * @author Gabriel Diaconu  —  PolyForm Noncommercial 1.0.0 (free for non-commercial use)
 */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "wcxhead.h"
#include "pgfc.h"

#define EXPORT __declspec(dllexport)

typedef struct {
	Pgfc *a;
	int   iter, count;
	char  arcname[1024];
	char  password[256];
} tArc;

static tProcessDataProc  g_pdProc  = NULL;
static tProcessDataProcW g_pdProcW = NULL;

/* session password cache (like zip): F3/F5 reopen without re-prompting */
static char g_pw_cache[256]={0};
static char g_pw_cache_arc[1024]={0};

/* ---------------- modal password dialog (Win32, no resources) ------------- */
static char g_dlg_text[256], g_dlg_out[256]; static int g_dlg_ok;
static LRESULT CALLBACK PwWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
	static HWND edit;
	switch(m){
	case WM_CREATE:
		CreateWindowA("STATIC",g_dlg_text,WS_CHILD|WS_VISIBLE,10,10,330,40,h,(HMENU)0,NULL,NULL);
		edit=CreateWindowA("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_PASSWORD|ES_AUTOHSCROLL,10,55,330,22,h,(HMENU)100,NULL,NULL);
		CreateWindowA("BUTTON","OK",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,180,90,75,26,h,(HMENU)IDOK,NULL,NULL);
		CreateWindowA("BUTTON","Cancel",WS_CHILD|WS_VISIBLE,265,90,75,26,h,(HMENU)IDCANCEL,NULL,NULL);
		SetFocus(edit); return 0;
	case WM_COMMAND:
		if(LOWORD(w)==IDOK){ GetWindowTextA(edit,g_dlg_out,sizeof g_dlg_out); g_dlg_ok=1; DestroyWindow(h); return 0; }
		if(LOWORD(w)==IDCANCEL){ g_dlg_ok=0; DestroyWindow(h); return 0; }
		break;
	case WM_CLOSE: g_dlg_ok=0; DestroyWindow(h); return 0;
	case WM_DESTROY: PostQuitMessage(0); return 0;
	}
	return DefWindowProcA(h,m,w,l);
}
static int prompt_password(const char *arc,char *out,int outlen){
	WNDCLASSA wc; HWND h; MSG msg; static int reg=0; HINSTANCE hi=GetModuleHandleA(NULL);
	if(!reg){ memset(&wc,0,sizeof wc); wc.lpfnWndProc=PwWndProc; wc.hInstance=hi; wc.lpszClassName="PgDumpPwDlg";
		wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); RegisterClassA(&wc); reg=1; }
	_snprintf(g_dlg_text,sizeof g_dlg_text,"Archive is encrypted (AES-256).\nPassword for: %s",arc);
	g_dlg_ok=0; g_dlg_out[0]=0;
	h=CreateWindowExA(WS_EX_DLGMODALFRAME|WS_EX_TOPMOST,"PgDumpPwDlg","pg_dump - password",
		WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,365,165,NULL,NULL,hi,NULL);
	if(!h) return 0;
	{ RECT r; int sw,sh; GetWindowRect(h,&r); sw=GetSystemMetrics(SM_CXSCREEN); sh=GetSystemMetrics(SM_CYSCREEN);
	  SetWindowPos(h,NULL,(sw-(r.right-r.left))/2,(sh-(r.bottom-r.top))/2,0,0,SWP_NOSIZE|SWP_NOZORDER); }
	while(GetMessage(&msg,NULL,0,0)){
		if(msg.message==WM_KEYDOWN&&msg.wParam==VK_RETURN){ SendMessageA(h,WM_COMMAND,IDOK,0); continue; }
		if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE){ SendMessageA(h,WM_COMMAND,IDCANCEL,0); continue; }
		TranslateMessage(&msg); DispatchMessage(&msg);
	}
	if(g_dlg_ok){ lstrcpynA(out,g_dlg_out,outlen); return 1; }
	return 0;
}

static int dostime(long long ut){
	FILETIME ft,lf; SYSTEMTIME st; WORD fd=0,ftm=0; ULONGLONG ll;
	if(ut<=0) return 0;
	ll=(ULONGLONG)ut*10000000ULL+116444736000000000ULL;
	ft.dwLowDateTime=(DWORD)ll; ft.dwHighDateTime=(DWORD)(ll>>32);
	if(!FileTimeToSystemTime(&ft,&st)) return 0;
	if(FileTimeToLocalFileTime(&ft,&lf)&&FileTimeToDosDateTime(&lf,&fd,&ftm)) return ((int)fd<<16)|(int)ftm;
	return 0;
}

/* ============================================================ WCX exports */
EXPORT HANDLE __stdcall OpenArchive(tOpenArchiveData *d)
{
	tArc *t; char err[512]; char pw[256]; pw[0]=0; Pgfc *a;
	int enc, used_cache=0;
	if(!d||!d->ArcName){ if(d) d->OpenResult=E_EOPEN; return NULL; }
	enc=pgfc_is_encrypted(d->ArcName);
	if(enc){
		if(g_pw_cache[0]&&lstrcmpiA(g_pw_cache_arc,d->ArcName)==0){ lstrcpynA(pw,g_pw_cache,sizeof pw); used_cache=1; }
		else if(!prompt_password(d->ArcName,pw,sizeof pw)){ d->OpenResult=E_EOPEN; return NULL; }
	}
	a=pgfc_open(d->ArcName, enc?pw:NULL, err, sizeof err);
	if(!a && enc && used_cache){ g_pw_cache[0]=0; g_pw_cache_arc[0]=0;
		if(prompt_password(d->ArcName,pw,sizeof pw)) a=pgfc_open(d->ArcName,pw,err,sizeof err); }
	if(!a){ if(d->OpenMode==PK_OM_EXTRACT) MessageBoxA(NULL,err,"pg_dump WCX",MB_OK|MB_ICONERROR); d->OpenResult=E_BAD_ARCHIVE; return NULL; }
	if(enc){ lstrcpynA(g_pw_cache,pw,sizeof g_pw_cache); lstrcpynA(g_pw_cache_arc,d->ArcName,sizeof g_pw_cache_arc); }
	t=(tArc*)calloc(1,sizeof(tArc));
	if(!t){ pgfc_close(a); d->OpenResult=E_NO_MEMORY; return NULL; }
	t->a=a; t->iter=0; t->count=pgfc_node_count(a);
	lstrcpynA(t->arcname,d->ArcName,sizeof t->arcname);
	lstrcpynA(t->password,pw,sizeof t->password);
	d->OpenResult=E_SUCCESS;
	return (HANDLE)t;
}

EXPORT int __stdcall ReadHeaderEx(HANDLE harc, tHeaderDataEx *h)
{
	tArc *t=(tArc*)harc; const PgfcNode *n;
	if(!t||!h) return E_BAD_DATA;
	if(t->iter>=t->count) return E_END_ARCHIVE;
	n=pgfc_node(t->a,t->iter);
	if(!n) return E_END_ARCHIVE;
	memset(h,0,sizeof *h);
	lstrcpynA(h->ArcName,t->arcname,sizeof h->ArcName);
	lstrcpynA(h->FileName,n->path,sizeof h->FileName);
	h->UnpSize=(unsigned)(n->size&0xffffffffULL);
	h->UnpSizeHigh=(unsigned)((unsigned long long)n->size>>32);
	h->PackSize=h->UnpSize;
	h->FileTime=dostime(n->mtime);
	h->FileAttr=n->is_dir?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_ARCHIVE;
	t->iter++;
	return E_SUCCESS;
}

EXPORT int __stdcall ReadHeader(HANDLE harc, tHeaderData *h)
{
	tHeaderDataEx ex; int r=ReadHeaderEx(harc,&ex);
	if(r!=E_SUCCESS) return r;
	memset(h,0,sizeof *h);
	lstrcpynA(h->ArcName,ex.ArcName,sizeof h->ArcName);
	lstrcpynA(h->FileName,ex.FileName,sizeof h->FileName);
	h->UnpSize=ex.UnpSize; h->PackSize=ex.PackSize; h->FileTime=ex.FileTime; h->FileAttr=ex.FileAttr;
	return E_SUCCESS;
}

EXPORT int __stdcall ProcessFile(HANDLE harc, int Operation, char *DestPath, char *DestName)
{
	tArc *t=(tArc*)harc; int cur; const PgfcNode *n; char dest[2048]; char err[512]; FILE *f;
	if(!t) return E_BAD_DATA;
	cur=t->iter-1;
	if(Operation==PK_SKIP||Operation==PK_TEST) return E_SUCCESS;
	if(Operation==PK_EXTRACT){
		n=pgfc_node(t->a,cur);
		if(!n) return E_BAD_DATA;
		if(n->is_dir) return E_SUCCESS;
		if(DestName&&DestName[0]) lstrcpynA(dest,DestName,sizeof dest);
		else if(DestPath) lstrcpynA(dest,DestPath,sizeof dest);
		else return E_ECREATE;
		f=fopen(dest,"wb");
		if(!f) return E_ECREATE;
		if(pgfc_emit_node(t->a,cur,f,err,sizeof err)){ fclose(f); return E_EWRITE; }
		fclose(f);
		if(g_pdProc) g_pdProc(dest,0);
	}
	return E_SUCCESS;
}

EXPORT int __stdcall CloseArchive(HANDLE harc)
{ tArc *t=(tArc*)harc; if(!t) return E_SUCCESS; if(t->a) pgfc_close(t->a); free(t); return E_SUCCESS; }

EXPORT void __stdcall SetChangeVolProc(HANDLE h, tChangeVolProc p){ (void)h;(void)p; }
EXPORT void __stdcall SetChangeVolProcW(HANDLE h, tChangeVolProcW p){ (void)h;(void)p; }
EXPORT void __stdcall SetProcessDataProc(HANDLE h, tProcessDataProc p){ (void)h; g_pdProc=p; }
EXPORT void __stdcall SetProcessDataProcW(HANDLE h, tProcessDataProcW p){ (void)h; g_pdProcW=p; }

EXPORT int __stdcall GetPackerCaps(void){ return PK_CAPS_BY_CONTENT|PK_CAPS_ENCRYPT|PK_CAPS_HIDE; }

EXPORT BOOL __stdcall CanYouHandleThisFile(char *FileName)
{
	HANDLE f; DWORD rd=0; char hdr[8];
	f=CreateFileA(FileName,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
	if(f==INVALID_HANDLE_VALUE) return FALSE;
	ReadFile(f,hdr,5,&rd,NULL); CloseHandle(f);
	return (rd==5 && memcmp(hdr,"PGDMP",5)==0) ? TRUE : FALSE;
}

EXPORT int __stdcall PackFiles(char *a,char *b,char *c,char *d,int e){ (void)a;(void)b;(void)c;(void)d;(void)e; return E_NOT_SUPPORTED; }
EXPORT int __stdcall DeleteFiles(char *a,char *b){ (void)a;(void)b; return E_NOT_SUPPORTED; }

BOOL WINAPI DllMain(HINSTANCE hi,DWORD r,LPVOID v){ (void)hi;(void)r;(void)v; return TRUE; }
