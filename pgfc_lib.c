/*
 * pgfc.c — portable pg_dump custom-format (-Fc) reader library.
 * Handles standard archives (gzip) and the eRGiss extensions (zstd, AES-256).
 * @author Gabriel Diaconu
 */
#include "pgfc.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <zstd.h>
#include <gcrypt.h>

#ifdef _WIN32			/* mingw: 64-bit file offsets */
#define fseeko fseeko64
#define ftello ftello64
#endif

#define BLK_DATA   1
#define K_OFFSET_POS_SET 2
#define ENC_MAGIC      "ERGX1"
#define ENC_MAGIC_LEN  5
#define SALT_LEN  16
#define IV_LEN    16
#define KEY_LEN   32
#define KEYCHECK_LEN 32
#define ZSTD_BASE 1000

typedef struct {
	int    dumpId;
	char  *tag, *desc, *defn, *copyStmt, *namespace;
	int   *deps; int ndeps;
	int    dataState; long long dataPos;
} Entry;

struct Pgfc {
	FILE *fp;
	long long pos;
	int  intSize, offSize, version, compression, encrypted;
	long long enc_base;
	gcry_cipher_hd_t hd;
	unsigned char iv[IV_LEN];
	char *dbname;
	Entry *ent; int nent;
	PgfcNode *node; int nnode;
	long long arctime;
};

/* ---- low-level reader with transparent AES-CTR decryption ---- */
static void ctr_xor(Pgfc *a, void *buf, size_t len, unsigned long long pos)
{
	unsigned char ctr[IV_LEN];
	unsigned long long block = pos / 16, carry = block;
	unsigned off = (unsigned)(pos % 16);
	int i;
	if (!len) return;
	memcpy(ctr, a->iv, IV_LEN);
	for (i = IV_LEN-1; i>=0 && carry; i--){
		unsigned long long s=(unsigned long long)ctr[i]+(carry&0xff);
		ctr[i]=(unsigned char)(s&0xff); carry=(carry>>8)+(s>>8);
	}
	gcry_cipher_setctr(a->hd, ctr, IV_LEN);
	if (off){ unsigned char d[16]; memset(d,0,off); gcry_cipher_encrypt(a->hd,d,off,NULL,0); }
	gcry_cipher_encrypt(a->hd, buf, len, NULL, 0);
}
static int rd_byte(Pgfc *a){
	int c=getc(a->fp); if(c==EOF) return EOF;
	{ unsigned char b=(unsigned char)c; if(a->encrypted) ctr_xor(a,&b,1,a->pos-a->enc_base); c=b; }
	a->pos++; return c;
}
static int rd_buf(Pgfc *a, void *buf, size_t len){
	if (fread(buf,1,len,a->fp)!=len) return -1;
	if (a->encrypted) ctr_xor(a,buf,len,a->pos-a->enc_base);
	a->pos += len; return 0;
}
static int rd_int(Pgfc *a){
	int sign=0,res=0,sh=0,b;
	if (a->version>0x010000) sign=rd_byte(a);
	for(b=0;b<a->intSize;b++){ int bv=rd_byte(a)&0xff; res+=bv<<sh; sh+=8; }
	return sign?-res:res;
}
static char *rd_str(Pgfc *a){
	int l=rd_int(a); char *s;
	if(l<0) return NULL;
	s=malloc(l+1); if(!s||rd_buf(a,s,l)){ free(s); return NULL; } s[l]=0; return s;
}
static int rd_offset(Pgfc *a, long long *o){
	int flag=rd_byte(a)&0xff,b; *o=0;
	for(b=0;b<a->offSize;b++){ long long bv=rd_byte(a)&0xff; *o|=bv<<(b*8); }
	return flag;
}

/* ---- static header probes ---- */
static int read_hdr_basic(const char *path, int *enc_out)
{
	FILE *f=fopen(path,"rb"); char m[8]; int v[3],is,os,fmt,i;
	long long here; char m2[8];
	Pgfc tmp; memset(&tmp,0,sizeof tmp);
	if(!f) return 0;
	if(fread(m,1,5,f)!=5||memcmp(m,"PGDMP",5)){ fclose(f); return 0; }
	v[0]=getc(f);v[1]=getc(f);v[2]=getc(f);
	is=getc(f);os=getc(f);fmt=getc(f);(void)fmt;
	tmp.fp=f; tmp.pos=11; tmp.intSize=is; tmp.offSize=os; tmp.version=(v[0]<<16)|(v[1]<<8)|v[2];
	if(tmp.version>=0x010F00) rd_byte(&tmp); else rd_int(&tmp);  /* compression: 1.15+ = 1-byte algorithm, else int level */
	for(i=0;i<7;i++) rd_int(&tmp);      /* timestamp */
	free(rd_str(&tmp)); free(rd_str(&tmp)); free(rd_str(&tmp)); /* db, remote, pg */
	here=ftello(f);
	if(enc_out){ *enc_out = (fread(m2,1,ENC_MAGIC_LEN,f)==ENC_MAGIC_LEN && !memcmp(m2,ENC_MAGIC,ENC_MAGIC_LEN)); }
	(void)here; fclose(f); return 1;
}
int pgfc_detect(const char *path){ return read_hdr_basic(path,NULL); }
int pgfc_is_encrypted(const char *path){ int e=0; if(!read_hdr_basic(path,&e)) return 0; return e; }

/* ---- node list (schemas as dirs, objects as files) ---- */
static int is_file_object(const char *desc){
	static const char *yes[]={"TABLE","VIEW","MATERIALIZED VIEW","SEQUENCE","FUNCTION",
		"PROCEDURE","AGGREGATE","TYPE","DOMAIN","INDEX","TRIGGER","RULE",
		"CONSTRAINT","FK CONSTRAINT","CHECK CONSTRAINT",NULL};
	int i; for(i=0;yes[i];i++) if(!strcmp(desc,yes[i])) return 1; return 0;
}
static void sanitize(char *s){ for(;*s;s++) if(*s=='\\'||*s=='/'||*s=='\n'||*s=='\r') *s='_'; }
static long long count_rows(Pgfc *a, int ei);   /* fwd */

static void build_nodes(Pgfc *a){
	int i,cap=64;
	a->node=malloc(sizeof(PgfcNode)*cap); a->nnode=0;
	for(i=0;i<a->nent;i++){
		Entry *e=&a->ent[i];
		if(!e->namespace||!e->namespace[0]) continue;
		/* Only emit object files as "schema\name.pg"; Total Commander infers the
		 * schema folders from those paths. Emitting explicit directory nodes makes
		 * TC show a duplicate phantom entry (a 0-byte file next to the folder). */
		if(is_file_object(e->desc)){
			char nm[256]; PgfcNode *nd;
			snprintf(nm,sizeof nm,"%s",e->tag); sanitize(nm);
			if(a->nnode>=cap){cap*=2;a->node=realloc(a->node,sizeof(PgfcNode)*cap);}
			nd=&a->node[a->nnode++]; memset(nd,0,sizeof *nd);
			snprintf(nd->path,sizeof nd->path,"%s\\%s.pg",e->namespace,nm);
			nd->is_dir=0; nd->mtime=a->arctime; nd->ent_index=i; nd->size=0;
			/* table size column = number of data rows (decompress + count) */
			if(!strcmp(e->desc,"TABLE")||!strcmp(e->desc,"MATERIALIZED VIEW")){
				int k; for(k=0;k<a->nent;k++){ Entry*de=&a->ent[k];
					if((!strcmp(de->desc,"TABLE DATA")||!strcmp(de->desc,"MATERIALIZED VIEW DATA"))
					   && de->namespace && !strcmp(de->namespace,e->namespace) && !strcmp(de->tag,e->tag)){
						nd->size=count_rows(a,k); break; } }
			}
		}
	}
}

Pgfc *pgfc_open(const char *path, const char *pw, char *errbuf, int errlen)
{
	Pgfc *a=calloc(1,sizeof(Pgfc)); char m[8]; int vmaj,vmin,vrev,fmt,i,n; long long here;
#define FAIL(msg) do{ if(errbuf)snprintf(errbuf,errlen,"%s",msg); if(a){ if(a->fp)fclose(a->fp); free(a);} return NULL; }while(0)
	a->fp=fopen(path,"rb"); if(!a->fp) FAIL("cannot open archive");
	gcry_check_version(NULL); gcry_control(GCRYCTL_INITIALIZATION_FINISHED,0);
	if(fread(m,1,5,a->fp)!=5||memcmp(m,"PGDMP",5)) FAIL("not a pg_dump custom archive");
	a->pos=5;
	vmaj=getc(a->fp);vmin=getc(a->fp);vrev=getc(a->fp);a->pos+=3;
	a->version=(vmaj<<16)|(vmin<<8)|vrev;
	a->intSize=getc(a->fp);a->offSize=getc(a->fp);fmt=getc(a->fp);a->pos+=3;(void)fmt;
	/* compression: archive 1.15+ (PG16) stores a 1-byte algorithm id
	 * (0=none 1=gzip 2=lz4 3=zstd); older versions store an int level. */
	if(a->version>=0x010F00){ int alg=rd_byte(a); a->compression=(alg==3)?ZSTD_BASE:alg; }
	else a->compression=rd_int(a);
	{ int sec=rd_int(a),mn=rd_int(a),hr=rd_int(a),md=rd_int(a),mo=rd_int(a),yr=rd_int(a); rd_int(a);
	  (void)sec;(void)mn;(void)hr;(void)md;(void)mo;(void)yr; a->arctime=0; }
	a->dbname=rd_str(a); free(rd_str(a)); free(rd_str(a));
	here=a->pos;
	if(fread(m,1,ENC_MAGIC_LEN,a->fp)==ENC_MAGIC_LEN && !memcmp(m,ENC_MAGIC,ENC_MAGIC_LEN)){
		unsigned char salt[SALT_LEN],iv[IV_LEN],kc[KEYCHECK_LEN],key[KEY_LEN],kc2[KEYCHECK_LEN]; int iter;
		gcry_md_hd_t md2;
		if(fread(salt,1,SALT_LEN,a->fp)!=SALT_LEN||fread(iv,1,IV_LEN,a->fp)!=IV_LEN||
		   fread(&iter,sizeof(int),1,a->fp)!=1||fread(kc,1,KEYCHECK_LEN,a->fp)!=KEYCHECK_LEN) FAIL("bad encryption header");
		if(!pw||!pw[0]) FAIL("archive is encrypted; password required");
		if(gcry_kdf_derive(pw,strlen(pw),GCRY_KDF_PBKDF2,GCRY_MD_SHA256,salt,SALT_LEN,iter,KEY_LEN,key)) FAIL("kdf failed");
		if(gcry_cipher_open(&a->hd,GCRY_CIPHER_AES256,GCRY_CIPHER_MODE_CTR,0)||gcry_cipher_setkey(a->hd,key,KEY_LEN)) FAIL("cipher init");
		gcry_md_open(&md2,GCRY_MD_SHA256,0); gcry_md_write(md2,salt,SALT_LEN); gcry_md_write(md2,key,KEY_LEN);
		memcpy(kc2,gcry_md_read(md2,GCRY_MD_SHA256),KEYCHECK_LEN); gcry_md_close(md2); memset(key,0,sizeof key);
		if(memcmp(kc,kc2,KEYCHECK_LEN)) FAIL("incorrect encryption password");
		memcpy(a->iv,iv,IV_LEN); a->enc_base=ftello(a->fp); a->pos=a->enc_base; a->encrypted=1;
	} else { fseeko(a->fp,here,SEEK_SET); a->pos=here; }

	n=rd_int(a); a->nent=n; a->ent=calloc(n,sizeof(Entry));
	for(i=0;i<n;i++){
		Entry *e=&a->ent[i]; int cap=8,k=0; char *d;
		e->dumpId=rd_int(a); rd_int(a);                 /* hadDumper */
		free(rd_str(a)); free(rd_str(a));               /* tableoid, oid */
		e->tag=rd_str(a); e->desc=rd_str(a); rd_int(a);  /* section */
		e->defn=rd_str(a); free(rd_str(a));              /* dropStmt */
		e->copyStmt=rd_str(a); e->namespace=rd_str(a);
		free(rd_str(a));                                 /* tablespace */
		/* version-gated TOC fields added after pg_dump 11 (archive 1.13):
		 *   tableam (str) since 1.14 / PG12 ; relkind (int) since 1.16 / PG17.
		 * Without these the per-entry stream desyncs on newer dumps. */
		if(a->version >= 0x010E00) free(rd_str(a));      /* tableam  (>=1.14, PG12+) */
		if(a->version >= 0x011000) rd_int(a);            /* relkind  (>=1.16, PG17+) */
		free(rd_str(a));                                 /* owner */
		free(rd_str(a));                                 /* with-oids flag (>=1.9) */
		e->deps=malloc(sizeof(int)*cap);
		for(;;){ d=rd_str(a); if(!d) break; if(k>=cap){cap*=2;e->deps=realloc(e->deps,sizeof(int)*cap);} e->deps[k++]=atoi(d); free(d); }
		e->ndeps=k;
		e->dataState=rd_offset(a,&e->dataPos);
		if(!e->desc||!e->tag) FAIL("corrupt TOC entry");
	}
	build_nodes(a);
	return a;
#undef FAIL
}

int pgfc_node_count(Pgfc *a){ return a?a->nnode:0; }
const PgfcNode *pgfc_node(Pgfc *a, int i){ return (a&&i>=0&&i<a->nnode)?&a->node[i]:NULL; }

void pgfc_close(Pgfc *a){
	int i;
	if(!a) return;
	if(a->encrypted) gcry_cipher_close(a->hd);
	for(i=0;i<a->nent;i++){ Entry*e=&a->ent[i]; free(e->tag);free(e->desc);free(e->defn);free(e->copyStmt);free(e->namespace);free(e->deps); }
	free(a->ent); free(a->node); free(a->dbname);
	if(a->fp) fclose(a->fp);
	free(a);
}

/* sink for decompressed data: either write to a FILE, or count COPY rows.
 * In count mode we stop at the "\." terminator line (COPY end marker), so the
 * count is exact row count, not raw newline count. */
typedef struct { FILE *out; long long nl; int bol; int st; int term; } Sink;
static void sink_write(Sink *s, const char *buf, size_t len){
	size_t i;
	if(s->out){ fwrite(buf,1,len,s->out); return; }
	for(i=0;i<len;i++){
		char c=buf[i];
		if(s->term) return;                 /* past the COPY terminator */
		if(c=='\n'){
			if(s->st==2){ s->term=1; return; }  /* line was exactly "\." -> end */
			s->nl++; s->bol=1; s->st=0; continue;
		}
		if(s->bol && c=='\\'){ s->st=1; s->bol=0; continue; }
		if(s->st==1 && c=='.'){ s->st=2; continue; }
		s->st=0; s->bol=0;
	}
}

/* decompress the data block at the current position into the sink */
static int emit_data_block(Pgfc *a, Sink *s){
	int blkLen;
	if(a->compression==0){
		while((blkLen=rd_int(a))!=0){ char*b=malloc(blkLen); if(rd_buf(a,b,blkLen)){free(b);return -1;} sink_write(s,b,blkLen); free(b);} return 0;
	}
	if(a->compression>=ZSTD_BASE){
		ZSTD_DStream*ds=ZSTD_createDStream(); size_t outsz; char*ob; ZSTD_initDStream(ds);
		outsz=ZSTD_DStreamOutSize(); ob=malloc(outsz);
		while((blkLen=rd_int(a))!=0){ char*in=malloc(blkLen); ZSTD_inBuffer ib;
			if(rd_buf(a,in,blkLen)){free(in);free(ob);ZSTD_freeDStream(ds);return -1;}
			ib.src=in;ib.size=blkLen;ib.pos=0;
			while(ib.pos<ib.size){ ZSTD_outBuffer o={ob,outsz,0}; size_t r=ZSTD_decompressStream(ds,&o,&ib); if(ZSTD_isError(r)){free(in);free(ob);ZSTD_freeDStream(ds);return -1;} if(o.pos)sink_write(s,ob,o.pos);}
			free(in);
		}
		ZSTD_freeDStream(ds); free(ob); return 0;
	}
	{ z_stream zs; char ob[16384]; int rc=Z_OK; memset(&zs,0,sizeof zs); inflateInit(&zs);
		while((blkLen=rd_int(a))!=0){ char*in=malloc(blkLen);
			if(rd_buf(a,in,blkLen)){free(in);inflateEnd(&zs);return -1;}
			zs.next_in=(Bytef*)in; zs.avail_in=blkLen;
			while(zs.avail_in>0){ zs.next_out=(Bytef*)ob; zs.avail_out=sizeof ob; rc=inflate(&zs,0); if(rc!=Z_OK&&rc!=Z_STREAM_END){free(in);inflateEnd(&zs);return -1;} sink_write(s,ob,sizeof(ob)-zs.avail_out);}
			free(in);
		}
		inflateEnd(&zs); return 0;
	}
}

/* count data rows of a TABLE DATA entry (decompress, count '\n') */
static long long count_rows(Pgfc *a, int ei){
	Entry *de=&a->ent[ei]; Sink s; int t;
	if(de->dataState!=K_OFFSET_POS_SET) return 0;
	s.out=NULL; s.nl=0; s.bol=1; s.st=0; s.term=0;
	fseeko(a->fp,de->dataPos,SEEK_SET); a->pos=de->dataPos;
	t=rd_byte(a); rd_int(a);
	if(t!=BLK_DATA) return 0;
	if(emit_data_block(a,&s)) return 0;
	return s.nl;   /* each COPY row ends with '\n'; no trailing terminator in -Fc data */
}

static Entry *find_data_for(Pgfc *a, Entry *tbl){
	int i;
	for(i=0;i<a->nent;i++){ Entry*e=&a->ent[i];
		if((!strcmp(e->desc,"TABLE DATA")||!strcmp(e->desc,"MATERIALIZED VIEW DATA"))
		   && e->namespace && tbl->namespace && !strcmp(e->namespace,tbl->namespace)
		   && !strcmp(e->tag,tbl->tag)) return e; }
	return NULL;
}

int pgfc_emit_node(Pgfc *a, int node_index, FILE *out, char *errbuf, int errlen)
{
	const PgfcNode *nd=pgfc_node(a,node_index);
	Entry *e, *de; int i, t, id;
	if(!nd||nd->is_dir||nd->ent_index<0){ if(errbuf)snprintf(errbuf,errlen,"not a file node"); return -1; }
	e=&a->ent[nd->ent_index];

	fputs("ERGPGT01\n",out);

	/* [SQL] — the object definition */
	fputs("\x01[SQL]\n",out);
	if(e->defn) fputs(e->defn,out);
	fputc('\n',out);

	/* [INFO] — for tables: dependent indexes/constraints/triggers/rules */
	if(!strcmp(e->desc,"TABLE")){
		fputs("\x01[INFO]\n",out);
		for(i=0;i<a->nent;i++){ Entry*o=&a->ent[i]; int j,rel=0;
			if(o==e) continue;
			if(strcmp(o->desc,"INDEX")&&strcmp(o->desc,"CONSTRAINT")&&strcmp(o->desc,"FK CONSTRAINT")
			   &&strcmp(o->desc,"CHECK CONSTRAINT")&&strcmp(o->desc,"TRIGGER")&&strcmp(o->desc,"RULE")) continue;
			for(j=0;j<o->ndeps;j++) if(o->deps[j]==e->dumpId){rel=1;break;}
			if(rel && o->defn){ fprintf(out,"-- %s: %s\n",o->desc,o->tag); fputs(o->defn,out); fputc('\n',out); }
		}
	}

	/* [DATA] — COPY statement + decompressed rows */
	de = (!strcmp(e->desc,"TABLE")||!strcmp(e->desc,"MATERIALIZED VIEW")) ? find_data_for(a,e) : NULL;
	if(de && de->dataState==K_OFFSET_POS_SET){
		Sink s; s.out=out; s.nl=0;
		fputs("\x01[DATA]\n",out);
		if(de->copyStmt) fputs(de->copyStmt,out);
		fseeko(a->fp,de->dataPos,SEEK_SET); a->pos=de->dataPos;
		t=rd_byte(a); id=rd_int(a); (void)id;
		if(t!=BLK_DATA){ if(errbuf)snprintf(errbuf,errlen,"bad data block (%d)",t); return -1; }
		if(emit_data_block(a,&s)){ if(errbuf)snprintf(errbuf,errlen,"decompress failed"); return -1; }
	}
	return 0;
}
