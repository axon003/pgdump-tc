#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "wcxhead.h"
typedef HANDLE(__stdcall*Open_t)(tOpenArchiveData*);
typedef int(__stdcall*RHE_t)(HANDLE,tHeaderDataEx*);
typedef int(__stdcall*PF_t)(HANDLE,int,char*,char*);
typedef int(__stdcall*Close_t)(HANDLE);
typedef BOOL(__stdcall*Can_t)(char*);
int main(int argc,char**argv){
  HMODULE m=LoadLibraryA("pgdump.wcx64"); tOpenArchiveData d; tHeaderDataEx hx; HANDLE h; int n=0;
  Open_t Open; RHE_t RHE; PF_t PF; Close_t Close; Can_t Can;
  if(!m){printf("LoadLibrary fail %lu\n",(unsigned long)GetLastError());return 1;}
  Open=(Open_t)GetProcAddress(m,"OpenArchive"); RHE=(RHE_t)GetProcAddress(m,"ReadHeaderEx");
  PF=(PF_t)GetProcAddress(m,"ProcessFile"); Close=(Close_t)GetProcAddress(m,"CloseArchive");
  Can=(Can_t)GetProcAddress(m,"CanYouHandleThisFile");
  printf("Can=%d\n", Can?Can(argv[1]):-1);
  memset(&d,0,sizeof d); d.ArcName=argv[1]; d.OpenMode=PK_OM_LIST; h=Open(&d);
  printf("Open=%p result=%d\n",(void*)h,d.OpenResult); if(!h)return 2;
  while(RHE(h,&hx)==E_SUCCESS){ printf("  %s %s (%u)\n",(hx.FileAttr&FILE_ATTRIBUTE_DIRECTORY)?"DIR ":"FILE",hx.FileName,hx.UnpSize); PF(h,PK_SKIP,NULL,NULL); if(++n>100000)break; }
  Close(h);
  printf("--- extract et4\t1 -> out_t1.txt ---\n");
  memset(&d,0,sizeof d); d.ArcName=argv[1]; d.OpenMode=PK_OM_EXTRACT; h=Open(&d);
  while(RHE(h,&hx)==E_SUCCESS){ if(!strcmp(hx.FileName,"et4\t1")){ PF(h,PK_EXTRACT,NULL,"out_t1.txt"); break; } PF(h,PK_SKIP,NULL,NULL); }
  Close(h); return 0;
}
