/*
 * wlx_pgdump.c -- Total Commander Lister plugin (.wlx) for ".pg" object files
 *   produced by the pg_dump WCX. Shows table data in a scrollable, Excel-like
 *   grid with cell selection, and cycles views with keys 1/2/3/4:
 *     1 = Data  (grid: cell cursor, arrows / Ctrl+arrows / Shift+arrows, Ctrl+C = TSV)
 *     2 = SQL   (CREATE statement of the object)
 *     3 = Info  (dependent indexes / constraints / triggers / rules)
 *     4 = Full  (complete reproducible script: CREATE + COPY data + indexes;
 *                Ctrl+C copies the whole thing -> paste straight into psql)
 *   Esc closes the lister.
 *   The .pg file is a sectioned container: "ERGPGT01" + \x01[SQL]/[INFO]/[DATA].
 * @author Gabriel Diaconu  —  PolyForm Noncommercial 1.0.0 (free for non-commercial use)
 * @version 2.1
 * @changes
 *   v1.0 2026-06-17 — grid + 1/2/3 tabs.
 *   v2.0 2026-06-17 — Esc=close; Excel-like cell selection (R1C1 on entry,
 *                     arrows/Ctrl/Shift navigation + mouse), Ctrl+C copies the
 *                     selected cell range as tab-separated UTF-16 (Excel paste);
 *                     new tab 4 = full reproducible SQL (CREATE+COPY+indexes),
 *                     Ctrl+C in tab 4 copies the whole script for psql.
 *   v2.1 2026-06-17 — grid shows SQL NULL ("\N") as lowercase "null" (display
 *                     only; COPY data and tab-4 script keep "\N" for psql).
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "listplug.h"

#define EXPORT __declspec(dllexport)
#define BARH 30
#define WLXCLASS "PgDumpLister"
#define WM_SWITCHTAB (WM_USER+11)

typedef struct {
	char  *buf;            /* whole file (owned) */
	long   buflen;
	char  *sql, *info;     /* NUL-terminated section bodies (point into buf) */
	char  *copystmt;       /* the COPY ... line (points into buf) */
	char **rows; int nrows;/* data rows (point into buf, NUL-terminated) */
	char  *cols[256]; int ncols;  /* column names (point into buf) */
	int    tab;            /* 1=data 2=sql 3=info 4=full */
	char  *full;           /* assembled full SQL script (owned, lazy) */
	int    cur_row, cur_col;   /* grid cell cursor */
	int    sel_row, sel_col;   /* grid selection anchor */
	char   title[600];
	HWND   grid, text;
	WNDPROC oldGrid, oldText;
} St;

static HFONT g_barfont = NULL;

/* ----------------- tiny growable string buffer ----------------- */
typedef struct { char *p; size_t len, cap; } SB;
static int  sb_init(SB *b){ b->cap=8192; b->len=0; b->p=malloc(b->cap); if(b->p)b->p[0]=0; return b->p!=NULL; }
static void sb_add(SB *b, const char *s){
	size_t l; if(!s) return; l=strlen(s);
	while(b->len+l+1>b->cap){ b->cap*=2; b->p=realloc(b->p,b->cap); }
	memcpy(b->p+b->len,s,l); b->len+=l; b->p[b->len]=0;
}

/* ----------------- clipboard helper (UTF-8 -> CF_UNICODETEXT) ---- */
static void clip_set_utf8(const char *utf8, int len){
	int wl; HGLOBAL h; WCHAR *wp;
	if(len<0) len=(int)strlen(utf8);
	wl=MultiByteToWideChar(CP_UTF8,0,utf8,len,NULL,0);
	h=GlobalAlloc(GMEM_MOVEABLE,(SIZE_T)(wl+1)*sizeof(WCHAR));
	if(!h) return;
	wp=(WCHAR*)GlobalLock(h);
	MultiByteToWideChar(CP_UTF8,0,utf8,len,wp,wl); wp[wl]=0;
	GlobalUnlock(h);
	if(OpenClipboard(NULL)){ EmptyClipboard(); SetClipboardData(CF_UNICODETEXT,h); CloseClipboard(); }
	else GlobalFree(h);
}
/* set an EDIT control's text from UTF-8 (so diacritics render correctly) */
static void set_edit_utf8(HWND e, const char *u){
	int wl; WCHAR *w;
	wl=MultiByteToWideChar(CP_UTF8,0,u,-1,NULL,0);
	w=malloc((size_t)wl*sizeof(WCHAR)); if(!w) return;
	MultiByteToWideChar(CP_UTF8,0,u,-1,w,wl);
	SetWindowTextW(e,w); free(w);
}

/* ------- parse the sectioned .pg container into St ------- */
static char *find_sec(char *buf, const char *name){
	/* sections begin with "\x01[NAME]\n" */
	char pat[32]; char *p;
	snprintf(pat,sizeof pat,"\x01[%s]\n",name);
	p=strstr(buf,pat);
	return p? p+strlen(pat) : NULL;
}
static void cut_at_sec(char *s){ /* terminate a section body at the next \x01 marker */
	if(!s) return; { char *n=strchr(s,'\x01'); if(n)*n=0; }
}

static void free_state(St *s){
	if(!s) return;
	free(s->full); free(s->rows); free(s->buf); free(s);
}

static int load_file(St *s, const char *path){
	FILE *f=fopen(path,"rb"); long n; char *data,*p;
	if(!f) return 0;
	fseek(f,0,SEEK_END); n=ftell(f); fseek(f,0,SEEK_SET);
	if(n<8){ fclose(f); return 0; }
	data=malloc(n+1); if(!data){ fclose(f); return 0; }
	if(fread(data,1,n,f)!=(size_t)n){ free(data); fclose(f); return 0; }
	fclose(f); data[n]=0;
	if(memcmp(data,"ERGPGT01",8)!=0){ free(data); return 0; }
	s->buf=data; s->buflen=n;
	/* find all section pointers BEFORE cutting any (cut_at_sec inserts \0 which stops strstr) */
	s->sql  = find_sec(data,"SQL");
	s->info = find_sec(data,"INFO");
	/* DATA: first line = COPY stmt, rest = rows */
	{
		char *d=find_sec(data,"DATA");
		cut_at_sec(s->sql);   /* NUL-terminate sql at \x01 before INFO */
		cut_at_sec(s->info);  /* NUL-terminate info at \x01 before DATA */
		s->copystmt=NULL; s->rows=NULL; s->nrows=0; s->ncols=0;
		if(d){
			char *nl=strchr(d,'\n');
			if(nl){ *nl=0; s->copystmt=d; p=nl+1; }
			else { s->copystmt=d; p=d+strlen(d); }
			/* columns from "COPY x.y (c1, c2, ...) FROM stdin;" */
			if(s->copystmt){
				char *op=strchr(s->copystmt,'('), *cp=op?strchr(op,')'):NULL;
				if(op&&cp&&cp>op){
					char *q=op+1; *cp=0;
					while(*q&&s->ncols<256){
						char *comma=strstr(q,", ");
						s->cols[s->ncols++]=q;
						if(!comma) break;
						*comma=0; q=comma+2;
					}
				}
			}
			/* rows: split p by '\n' until "\." or end */
			{
				int cap=1024; s->rows=malloc(sizeof(char*)*cap);
				while(p&&*p){
					char *nl2=strchr(p,'\n');
					if(nl2)*nl2=0;
					if(p[0]=='\\'&&p[1]=='.'&&p[2]==0) break;   /* COPY terminator */
					if(s->nrows>=cap){cap*=2;s->rows=realloc(s->rows,sizeof(char*)*cap);}
					s->rows[s->nrows++]=p;
					if(!nl2) break;
					p=nl2+1;
				}
			}
			if(s->ncols==0 && s->nrows>0){ /* fallback: count tabs in first row */
				char *r=s->rows[0]; int c=1; while(*r){ if(*r=='\t')c++; r++; } s->ncols=c;
			}
		}
	}
	/* title: derive from copystmt "COPY schema.table ..." */
	{
		const char *what = s->copystmt ? s->copystmt+5 : "(no data)";
		char nm[256]={0}; int i=0;
		if(s->copystmt){ const char*q=s->copystmt+5; while(*q&&*q!=' '&&i<255)nm[i++]=*q++; nm[i]=0; }
		if(s->nrows||s->copystmt) snprintf(s->title,sizeof s->title,"%s   —   %d rows", nm[0]?nm:"table", s->nrows);
		else snprintf(s->title,sizeof s->title,"object (SQL only)");
		(void)what;
	}
	s->cur_row=s->cur_col=s->sel_row=s->sel_col=0;
	s->full=NULL;
	return 1;
}

/* assemble the full reproducible script: CREATE + COPY data + indexes/triggers.
 * The COPY block is terminated with "\." so it can be pasted straight into psql. */
static char *assemble_full(St *s){
	SB b; int i;
	if(!sb_init(&b)) return NULL;
	if(s->sql && s->sql[0]){ sb_add(&b,s->sql); sb_add(&b,"\n"); }
	if(s->copystmt && s->copystmt[0]){
		sb_add(&b,"\n"); sb_add(&b,s->copystmt); sb_add(&b,"\n");
		for(i=0;i<s->nrows;i++){ sb_add(&b,s->rows[i]); sb_add(&b,"\n"); }
		sb_add(&b,"\\.\n");
	}
	if(s->info && s->info[0]){ sb_add(&b,"\n"); sb_add(&b,s->info); }
	return b.p;
}

/* get the col-th tab-separated field of a row line into out */
static void get_field(const char *line,int col,char *out,int outlen){
	int c=0; const char *p=line;
	out[0]=0;
	while(c<col && *p){ if(*p=='\t')c++; p++; }
	if(c==col){ int i=0; while(*p&&*p!='\t'&&i<outlen-1){ out[i++]=*p++; } out[i]=0; }
}
/* unescape a COPY text field for clipboard (\\N -> empty, \\\\ -> \\, drop \\n \\r,
 * \\t -> space so the TSV layout is preserved). */
static void unesc_field(const char *in, char *out, int outlen){
	int i=0; const char *p=in;
	if(strcmp(in,"\\N")==0){ out[0]=0; return; }
	while(*p && i<outlen-1){
		if(*p=='\\' && p[1]){
			char n=p[1]; p+=2;
			if(n=='t') out[i++]=' ';
			else if(n=='n'||n=='r'){ /* drop embedded newline */ }
			else if(n=='\\') out[i++]='\\';
			else out[i++]=n;
		} else out[i++]=*p++;
	}
	out[i]=0;
}

static void sel_bounds(St *s,int *r0,int *r1,int *c0,int *c1){
	*r0 = s->cur_row<s->sel_row?s->cur_row:s->sel_row;
	*r1 = s->cur_row>s->sel_row?s->cur_row:s->sel_row;
	*c0 = s->cur_col<s->sel_col?s->cur_col:s->sel_col;
	*c1 = s->cur_col>s->sel_col?s->cur_col:s->sel_col;
}

static void copy_grid_selection(St *s){
	int r0,r1,c0,c1,r,c; SB b; char fld[8192],ue[8192];
	if(s->nrows<=0||s->ncols<=0) return;
	sel_bounds(s,&r0,&r1,&c0,&c1);
	if(!sb_init(&b)) return;
	for(r=r0;r<=r1;r++){
		for(c=c0;c<=c1;c++){
			get_field(s->rows[r],c,fld,sizeof fld);
			unesc_field(fld,ue,sizeof ue);
			sb_add(&b,ue);
			if(c<c1) sb_add(&b,"\t");
		}
		sb_add(&b,"\r\n");
	}
	clip_set_utf8(b.p,(int)b.len);
	free(b.p);
}

static void populate_grid(St *s){
	int i; LVCOLUMNA col; memset(&col,0,sizeof col); col.mask=LVCF_TEXT|LVCF_WIDTH;
	while(ListView_DeleteColumn(s->grid,0));
	ListView_DeleteAllItems(s->grid);
	for(i=0;i<s->ncols;i++){
		col.pszText=s->cols[i]?s->cols[i]:(char*)"col"; col.cx=120;
		ListView_InsertColumn(s->grid,i,&col);
	}
	if(s->ncols==0){ col.pszText=(char*)"(no columns)"; col.cx=200; ListView_InsertColumn(s->grid,0,&col); }
	ListView_SetItemCountEx(s->grid,s->nrows,LVSICF_NOSCROLL);
	for(i=0;i<s->ncols;i++) ListView_SetColumnWidth(s->grid,i,LVSCW_AUTOSIZE_USEHEADER);
}

/* bring the focused cell into view (vertically + horizontally) */
static void ensure_cell_visible(St *s){
	RECT rc, cr;
	if(s->nrows<=0) return;
	ListView_EnsureVisible(s->grid,s->cur_row,FALSE);
	if(ListView_GetSubItemRect(s->grid,s->cur_row,s->cur_col,LVIR_BOUNDS,&rc)){
		int dx=0; GetClientRect(s->grid,&cr);
		if(rc.left<0) dx=rc.left-2;                                    /* scroll left to reveal */
		else if(s->cur_col>0 && rc.right>cr.right) dx=rc.right-cr.right+2; /* scroll right */
		if(dx) ListView_Scroll(s->grid,dx,0);
	}
}

static void grid_goto(St *s,int r,int c,int extend){
	int maxc;
	if(s->nrows<=0) return;
	maxc = s->ncols>0? s->ncols-1 : 0;
	if(r<0)r=0; if(r>s->nrows-1)r=s->nrows-1;
	if(c<0)c=0; if(c>maxc)c=maxc;
	s->cur_row=r; s->cur_col=c;
	if(!extend){ s->sel_row=r; s->sel_col=c; }
	ensure_cell_visible(s);
	InvalidateRect(s->grid,NULL,FALSE);
}

static void show_tab(HWND hwnd, St *s, int t){
	s->tab=t;
	if(t==1){
		ShowWindow(s->text,SW_HIDE); ShowWindow(s->grid,SW_SHOW);
		ensure_cell_visible(s); SetFocus(s->grid);
	} else {
		const char *txt;
		if(t==2)      txt = s->sql ? s->sql : "(no SQL)";
		else if(t==3) txt = s->info? s->info: "(no info)";
		else { if(!s->full) s->full=assemble_full(s); txt = s->full? s->full : "(no SQL)"; }
		set_edit_utf8(s->text,txt);
		SendMessageA(s->text,EM_SETSEL,0,0);
		ShowWindow(s->grid,SW_HIDE); ShowWindow(s->text,SW_SHOW); SetFocus(s->text);
	}
	InvalidateRect(hwnd,NULL,FALSE);
}

/* close the lister: forward Esc to TC's lister window (our window's grandparent) */
static void close_lister(HWND child){ PostMessage(GetParent(GetParent(child)),WM_KEYDOWN,VK_ESCAPE,0); }

/* subclass for grid: cell navigation + selection + copy; forwards 1..4 / Esc */
static LRESULT CALLBACK ChildProc(HWND h,UINT m,WPARAM w,LPARAM l){
	St *s=(St*)GetWindowLongPtr(GetParent(h),GWLP_USERDATA);
	WNDPROC old = (s && h==s->grid)? s->oldGrid : (s? s->oldText : NULL);
	if(!s) return DefWindowProcA(h,m,w,l);

	if(m==WM_KEYDOWN){
		int ctrl  = (GetKeyState(VK_CONTROL)&0x8000)!=0;
		int shift = (GetKeyState(VK_SHIFT)  &0x8000)!=0;
		if(w==VK_ESCAPE){ close_lister(h); return 0; }
		if(w>='1' && w<='4'){ PostMessage(GetParent(h),WM_SWITCHTAB,(int)(w-'0'),0); return 0; }

		if(h==s->grid){
			switch(w){
			case VK_UP:    grid_goto(s, ctrl?0:s->cur_row-1, s->cur_col, shift); return 0;
			case VK_DOWN:  grid_goto(s, ctrl?s->nrows-1:s->cur_row+1, s->cur_col, shift); return 0;
			case VK_LEFT:  grid_goto(s, s->cur_row, ctrl?0:s->cur_col-1, shift); return 0;
			case VK_RIGHT: grid_goto(s, s->cur_row, ctrl?s->ncols-1:s->cur_col+1, shift); return 0;
			case VK_HOME:  grid_goto(s, ctrl?0:s->cur_row, 0, shift); return 0;
			case VK_END:   grid_goto(s, ctrl?s->nrows-1:s->cur_row, s->ncols-1, shift); return 0;
			case VK_PRIOR:{ int per=ListView_GetCountPerPage(s->grid); if(per<1)per=10; grid_goto(s,s->cur_row-per,s->cur_col,shift); return 0; }
			case VK_NEXT: { int per=ListView_GetCountPerPage(s->grid); if(per<1)per=10; grid_goto(s,s->cur_row+per,s->cur_col,shift); return 0; }
			case 'A': if(ctrl){ s->sel_row=0; s->sel_col=0; grid_goto(s,s->nrows-1,s->ncols-1,1); return 0; } break;
			case 'C': if(ctrl){ copy_grid_selection(s); return 0; } break;
			case VK_INSERT: if(ctrl){ copy_grid_selection(s); return 0; } break;
			}
		} else { /* text views (2/3/4) */
			if(w=='A' && ctrl){ SendMessageA(h,EM_SETSEL,0,-1); return 0; }
			if((w=='C'||w==VK_INSERT) && ctrl){
				DWORD a=0,b=0; SendMessageA(h,EM_GETSEL,(WPARAM)&a,(LPARAM)&b);
				if(a==b){ /* no selection -> copy the whole visible script (correct UTF-16) */
					const char *full = (s->tab==4)?(s->full?s->full:"")
					                 : (s->tab==2)?(s->sql?s->sql:"")
					                 : (s->info?s->info:"");
					clip_set_utf8(full,-1); return 0;
				}
				/* else fall through: edit copies the selection itself */
			}
		}
	}
	else if(h==s->grid && m==WM_LBUTTONDOWN){
		LVHITTESTINFO ht; int row; int shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
		memset(&ht,0,sizeof ht); ht.pt.x=(short)LOWORD(l); ht.pt.y=(short)HIWORD(l);
		SetFocus(s->grid);
		row=ListView_SubItemHitTest(s->grid,&ht);
		if(row>=0) grid_goto(s,row,ht.iSubItem,shift);
		return 0;
	}
	else if(h==s->grid && (m==WM_SETFOCUS||m==WM_KILLFOCUS)){
		InvalidateRect(s->grid,NULL,FALSE);
	}
	return CallWindowProc(old,h,m,w,l);
}

static LRESULT CALLBACK WlxProc(HWND hwnd,UINT m,WPARAM w,LPARAM l){
	St *s=(St*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
	switch(m){
	case WM_CREATE: return 0;
	case WM_SIZE: {
		RECT r; GetClientRect(hwnd,&r);
		if(s){ MoveWindow(s->grid,0,BARH,r.right,r.bottom-BARH,TRUE);
		        MoveWindow(s->text,0,BARH,r.right,r.bottom-BARH,TRUE); }
		return 0; }
	case WM_SWITCHTAB: if(s) show_tab(hwnd,s,(int)w); return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps); RECT r; HFONT of;
		GetClientRect(hwnd,&r); r.bottom=BARH;
		FillRect(dc,&r,(HBRUSH)(COLOR_BTNFACE+1));
		{ RECT lb=r; lb.bottom=BARH; lb.top=BARH-1; FillRect(dc,&lb,(HBRUSH)GetStockObject(GRAY_BRUSH)); }
		of=(HFONT)SelectObject(dc, g_barfont?g_barfont:(HFONT)GetStockObject(DEFAULT_GUI_FONT));
		SetBkMode(dc,TRANSPARENT);
		if(s){
			char line[900]; const char *t1,*t2,*t3,*t4;
			t1=(s->tab==1)?"[1 Data]":" 1 Data ";
			t2=(s->tab==2)?"[2 SQL]" :" 2 SQL ";
			t3=(s->tab==3)?"[3 Info]":" 3 Info ";
			t4=(s->tab==4)?"[4 Full SQL]":" 4 Full SQL ";
			snprintf(line,sizeof line,"  %s %s %s %s        %s", t1,t2,t3,t4, s->title);
			SetTextColor(dc,RGB(20,20,20));
			{ RECT tr=r; tr.left=6; tr.top=6; DrawTextA(dc,line,-1,&tr,DT_LEFT|DT_SINGLELINE); }
		}
		SelectObject(dc,of);
		EndPaint(hwnd,&ps); return 0; }
	case WM_NOTIFY: {
		NMHDR *nh=(NMHDR*)l;
		if(s && nh->hwndFrom==s->grid){
			if(nh->code==LVN_GETDISPINFOA){
				NMLVDISPINFOA *di=(NMLVDISPINFOA*)l;
				if(di->item.mask&LVIF_TEXT){
					int it=di->item.iItem, sub=di->item.iSubItem;
					if(it>=0&&it<s->nrows){
						get_field(s->rows[it],sub,di->item.pszText,di->item.cchTextMax);
						/* COPY format: "\N" = SQL NULL -> show as lowercase "null" (display only) */
						if(strcmp(di->item.pszText,"\\N")==0) lstrcpynA(di->item.pszText,"null",di->item.cchTextMax);
					}
					else di->item.pszText[0]=0;
				}
				return 0;
			}
			if(nh->code==NM_CUSTOMDRAW){
				NMLVCUSTOMDRAW *cd=(NMLVCUSTOMDRAW*)l;
				switch(cd->nmcd.dwDrawStage){
				case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
				case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
				case CDDS_ITEMPREPAINT|CDDS_SUBITEM: {
					int row=(int)cd->nmcd.dwItemSpec, col=cd->iSubItem;
					int r0,r1,c0,c1; int focuswin=(GetFocus()==s->grid);
					sel_bounds(s,&r0,&r1,&c0,&c1);
					if(row==s->cur_row && col==s->cur_col){
						cd->clrTextBk = focuswin?RGB(38,97,181):RGB(170,170,170);
						cd->clrText   = RGB(255,255,255);
					} else if(row>=r0&&row<=r1&&col>=c0&&col<=c1){
						cd->clrTextBk = focuswin?RGB(197,217,241):RGB(228,228,228);
						cd->clrText   = RGB(0,0,0);
					} else {
						cd->clrTextBk = (row&1)?RGB(245,247,250):RGB(255,255,255);
						cd->clrText   = RGB(0,0,0);
					}
					return CDRF_NEWFONT;
				}
				}
				return CDRF_DODEFAULT;
			}
		}
		return 0; }
	case WM_DESTROY: return 0;
	}
	return DefWindowProcA(hwnd,m,w,l);
}

static void ensure_class(void){
	static int reg=0; WNDCLASSA wc; INITCOMMONCONTROLSEX ic;
	if(reg) return; reg=1;
	ic.dwSize=sizeof ic; ic.dwICC=ICC_LISTVIEW_CLASSES; InitCommonControlsEx(&ic);
	if(!g_barfont){ g_barfont=CreateFontA(-15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI"); }
	memset(&wc,0,sizeof wc);
	wc.lpfnWndProc=WlxProc; wc.hInstance=GetModuleHandleA(NULL);
	wc.lpszClassName=WLXCLASS; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
	wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
	RegisterClassA(&wc);
}

/* ================================================ WLX exports */
EXPORT HWND __stdcall ListLoad(HWND ParentWin, char *FileToLoad, int ShowFlags)
{
	St *s; HWND hwnd; RECT r;
	(void)ShowFlags;
	s=calloc(1,sizeof(St));
	if(!load_file(s,FileToLoad)){ free_state(s); return NULL; }   /* not ours -> TC built-in */
	ensure_class();
	GetClientRect(ParentWin,&r);
	hwnd=CreateWindowExA(0,WLXCLASS,"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
		0,0,r.right,r.bottom,ParentWin,NULL,GetModuleHandleA(NULL),NULL);
	if(!hwnd){ free_state(s); return NULL; }
	SetWindowLongPtr(hwnd,GWLP_USERDATA,(LONG_PTR)s);
	s->grid=CreateWindowExA(0,WC_LISTVIEWA,"",WS_CHILD|LVS_REPORT|LVS_OWNERDATA|LVS_SHOWSELALWAYS,
		0,BARH,r.right,r.bottom-BARH,hwnd,NULL,GetModuleHandleA(NULL),NULL);
	ListView_SetExtendedListViewStyle(s->grid,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
	s->text=CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",WS_CHILD|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL,
		0,BARH,r.right,r.bottom-BARH,hwnd,NULL,GetModuleHandleA(NULL),NULL);
	SendMessageA(s->text,EM_SETLIMITTEXT,0,0);   /* lift the default size cap (big tables in tab 4) */
	SendMessageA(s->text,WM_SETFONT,(WPARAM)GetStockObject(ANSI_FIXED_FONT),TRUE);
	if(g_barfont) SendMessageA(s->grid,WM_SETFONT,(WPARAM)g_barfont,TRUE);
	s->oldGrid=(WNDPROC)SetWindowLongPtr(s->grid,GWLP_WNDPROC,(LONG_PTR)ChildProc);
	s->oldText=(WNDPROC)SetWindowLongPtr(s->text,GWLP_WNDPROC,(LONG_PTR)ChildProc);
	populate_grid(s);
	/* default tab: Data view for any table (has columns), else SQL */
	show_tab(hwnd,s,(s->ncols>0)?1:2);
	return hwnd;
}

EXPORT int __stdcall ListLoadNext(HWND ParentWin, HWND ListWin, char *FileToLoad, int ShowFlags)
{
	St *s=(St*)GetWindowLongPtr(ListWin,GWLP_USERDATA); St tmp;
	(void)ParentWin;(void)ShowFlags;
	if(!s) return LISTPLUGIN_ERROR;
	memset(&tmp,0,sizeof tmp);
	if(!load_file(&tmp,FileToLoad)) return LISTPLUGIN_ERROR;
	/* swap parsed content into existing window */
	free(s->full); free(s->rows); free(s->buf);
	s->buf=tmp.buf; s->buflen=tmp.buflen; s->sql=tmp.sql; s->info=tmp.info;
	s->copystmt=tmp.copystmt; s->rows=tmp.rows; s->nrows=tmp.nrows; s->ncols=tmp.ncols;
	s->full=NULL; s->cur_row=s->cur_col=s->sel_row=s->sel_col=0;
	memcpy(s->cols,tmp.cols,sizeof s->cols); lstrcpynA(s->title,tmp.title,sizeof s->title);
	populate_grid(s);
	show_tab(ListWin,s,(s->ncols>0)?1:2);
	return LISTPLUGIN_OK;
}

EXPORT void __stdcall ListCloseWindow(HWND ListWin)
{
	St *s=(St*)GetWindowLongPtr(ListWin,GWLP_USERDATA);
	DestroyWindow(ListWin);
	if(s) free_state(s);
}

EXPORT void __stdcall ListGetDetectString(char *DetectString, int maxlen)
{
	lstrcpynA(DetectString,"EXT=\"PG\"",maxlen);
}

EXPORT int __stdcall ListSendCommand(HWND ListWin,int Command,int Parameter)
{ (void)ListWin;(void)Command;(void)Parameter; return LISTPLUGIN_OK; }

BOOL WINAPI DllMain(HINSTANCE hi,DWORD r,LPVOID v){ (void)hi;(void)r;(void)v; return TRUE; }
