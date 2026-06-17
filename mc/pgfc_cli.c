/*
 * pgfc_cli.c — command-line driver for the portable pg_dump reader (pgfc_lib.c),
 *   used by the Midnight Commander extfs plugin "upgdump".
 *     pgfc list <archive>                 -> ls -l style listing for mc extfs
 *     pgfc copyout <archive> <path> <dst> -> write one object as runnable SQL
 *   Encrypted archives: set password in env PGFC_PW.
 * @author Gabriel Diaconu (ERGISS Media)
 */
#define _GNU_SOURCE
#include "pgfc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void to_slash(char *s){ for(; *s; s++) if(*s=='\\') *s='/'; }

/* Transform the ERGPGT01 container (what pgfc_emit_node writes) into a single
 * runnable SQL script: CREATE ... ; then COPY data + "\." ; then indexes/triggers. */
static size_t rtrim(const char *s, size_t n){
    while(n && (s[n-1]=='\n'||s[n-1]=='\r'||s[n-1]==' '||s[n-1]=='\t')) n--;
    return n;
}
/* Emit order: CREATE (SQL) -> COPY data (+\. terminator) -> indexes/triggers (INFO). */
static void container_to_sql(const char *buf, FILE *out){
    char *sqlp=strstr(buf,"\x01[SQL]\n");
    char *infp=strstr(buf,"\x01[INFO]\n");
    char *datp=strstr(buf,"\x01[DATA]\n");           /* DATA is emitted last */
    const char *sql = sqlp? sqlp+7 : NULL;
    const char *inf = infp? infp+8 : NULL;
    const char *dat = datp? datp+8 : NULL;
    const char *eob = buf+strlen(buf);
    if(sql){ const char *end = infp? infp : (datp? datp : eob);
             size_t n=rtrim(sql,(size_t)(end-sql));
             if(n){ fwrite(sql,1,n,out); fputs("\n",out); } }
    if(dat){ size_t n=rtrim(dat,(size_t)(eob-dat));
             if(n){ fputs("\n",out); fwrite(dat,1,n,out); fputs("\n",out);
                    if(!(n>=2 && dat[n-1]=='.' && dat[n-2]=='\\')) fputs("\\.\n",out); } }
    if(inf){ const char *end = datp? datp : eob;
             size_t n=rtrim(inf,(size_t)(end-inf));
             if(n){ fputs("\n",out); fwrite(inf,1,n,out); fputs("\n",out); } }
}

int main(int argc, char **argv){
    const char *cmd, *arc, *pw=getenv("PGFC_PW");
    char err[512];
    Pgfc *a;
    if(argc<3){ fprintf(stderr,"usage: pgfc list <archive>\n       pgfc copyout <archive> <path> <dest>\n"); return 2; }
    cmd=argv[1]; arc=argv[2];
    a=pgfc_open(arc, (pw&&pw[0])?pw:NULL, err, sizeof err);
    if(!a){ fprintf(stderr,"pgfc: %s\n",err); return 1; }

    if(!strcmp(cmd,"list")){
        int n=pgfc_node_count(a), i, j, ns=0;
        static char seen[8192][128];
        for(i=0;i<n;i++){
            const PgfcNode *nd=pgfc_node(a,i);
            char path[600], sch[256], *sl; int found=0;
            snprintf(path,sizeof path,"%s",nd->path); to_slash(path);
            /* derive schema (dir) and turn "name.pg" file into "name.sql" */
            { char *dot;
              snprintf(sch,sizeof sch,"%s",path);
              sl=strchr(sch,'/'); if(sl)*sl=0;
              dot=strrchr(path,'.'); if(dot && !strcmp(dot,".pg")) strcpy(dot,".sql");
            }
            for(j=0;j<ns;j++) if(!strcmp(seen[j],sch)){ found=1; break; }
            if(!found && ns<8192){ strcpy(seen[ns++],sch);
                printf("dr-xr-xr-x 1 root root 0 Jan  1 2026 %s\n", sch); }
            printf("-r--r--r-- 1 root root %lld Jan  1 2026 %s\n", (long long)nd->size, path);
        }
    } else if(!strcmp(cmd,"copyout")){
        int n=pgfc_node_count(a), i, found=-1;
        char want[600]; char *dot; char *cbuf=NULL; size_t csz=0; FILE *mem, *out;
        if(argc<5){ fprintf(stderr,"copyout needs <path> <dest>\n"); pgfc_close(a); return 2; }
        snprintf(want,sizeof want,"%s",argv[3]); to_slash(want);
        /* mc shows "name.sql"; our node is "name.pg" -> map back */
        dot=strrchr(want,'.'); if(dot && !strcmp(dot,".sql")) strcpy(dot,".pg");
        for(i=0;i<n;i++){ const PgfcNode *nd=pgfc_node(a,i); char p[600];
            snprintf(p,sizeof p,"%s",nd->path); to_slash(p);
            if(!strcmp(p,want)){ found=i; break; } }
        if(found<0){ fprintf(stderr,"not found: %s\n",argv[3]); pgfc_close(a); return 1; }
        mem=open_memstream(&cbuf,&csz);
        if(!mem){ fprintf(stderr,"open_memstream failed\n"); pgfc_close(a); return 1; }
        if(pgfc_emit_node(a,found,mem,err,sizeof err)){ fclose(mem); free(cbuf); fprintf(stderr,"%s\n",err); pgfc_close(a); return 1; }
        fclose(mem);
        out=fopen(argv[4],"wb");
        if(!out){ free(cbuf); fprintf(stderr,"cannot create %s\n",argv[4]); pgfc_close(a); return 1; }
        container_to_sql(cbuf, out);
        fclose(out); free(cbuf);
    } else { fprintf(stderr,"unknown command: %s\n",cmd); pgfc_close(a); return 2; }

    pgfc_close(a);
    return 0;
}
