#include "tagcache.h"
#include "tagcache_generation_refs.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void must(int ok, const char *what) { if (!ok) { fprintf(stderr, "tagcache refs: %s\n", what); exit(1); } }
static void fixture(const char *p) { int fd=open(p,O_CREAT|O_TRUNC|O_WRONLY,0644); must(fd>=0,"fixture"); must(write(fd,"x",1)==1,"fixture write"); close(fd); }
static int gen(const char *root) { char p[700],b[32]; snprintf(p,sizeof(p),"%s/tagcache.gen",root); int fd=open(p,O_RDONLY); must(fd>=0,"gen open"); ssize_t n=read(fd,b,31); close(fd); must(n>0,"gen read"); b[n]=0; return atoi(b); }
static unsigned char *backup(const char *p,size_t *sz) { struct stat st; must(stat(p,&st)==0,"backup stat"); *sz=(size_t)st.st_size; unsigned char *b=malloc(*sz); must(b||!*sz,"backup alloc"); int fd=open(p,O_RDONLY); must(fd>=0,"backup open"); must(read(fd,b,*sz)==(ssize_t)*sz,"backup read"); close(fd); return b; }
static void restore(const char *p,const unsigned char *b,size_t sz) { int fd=open(p,O_CREAT|O_WRONLY|O_TRUNC,0644); must(fd>=0,"restore open"); must(write(fd,b,sz)==(ssize_t)sz,"restore write"); close(fd); }
static void check_title(const char *p,const char *want) { tagcache_song_t s; must(tagcache_song_by_path(p,&s),"lookup"); must(s.title&&strcmp(s.title,want)==0,"title"); }
static void commit(const char *a,const char *b,int rev) { char title[32]; snprintf(title,sizeof(title),"Title-%d",rev); tagcache_begin_update(); tagcache_upsert(a,rev,1,title,"Artist","Album","Artist","Genre",1,1); tagcache_upsert(b,rev,1,"Second","Artist","Album","Artist","Genre",2,1); must(tagcache_end_update(),"commit"); }
static void cleanup(const char *root) { DIR *d=opendir(root); if(!d)return; struct dirent *e; char p[700]; while((e=readdir(d)))if(e->d_name[0]!='.'){snprintf(p,sizeof(p),"%s/%s",root,e->d_name);unlink(p);}closedir(d);rmdir(root); }

int main(void) {
    char root[]="/tmp/tagcache-refs-XXXXXX",a[700],b[700],p[700]; must(mkdtemp(root) != NULL,"mkdtemp");
    snprintf(a,sizeof(a),"%s/a.flac",root); snprintf(b,sizeof(b),"%s/b.flac",root); fixture(a); fixture(b);
    must(tagcache_open(root),"open"); commit(a,b,1); for(int i=2;i<=5;i++)commit(a,b,i);
    int latest=gen(root); snprintf(p,sizeof(p),"%s/tagcache.refs.g%d",root,latest); size_t refs_sz; unsigned char *refs_b=backup(p,&refs_sz); struct tagcache_generation_refs refs; must(refs_sz==sizeof(refs),"refs size"); memcpy(&refs,refs_b,sizeof(refs)); must(refs.source[9]==1,"filename source"); check_title(a,"Title-5");
    tagcache_close(); must(tagcache_open(root),"reopen"); check_title(a,"Title-5");
    int fd=open(p,O_WRONLY); must(fd>=0,"bad refs"); unsigned char bad=0; must(write(fd,&bad,1)==1,"bad refs write"); close(fd); tagcache_close(); must(tagcache_open(root),"refs fallback"); check_title(a,"Title-4"); restore(p,refs_b,refs_sz);
    snprintf(p,sizeof(p),"%s/database_8.tcd.g5",root); size_t title_sz; unsigned char *title_b=backup(p,&title_sz); unlink(p); snprintf(p,sizeof(p),"%s/database_idx.tcd.g5",root); size_t master_sz; unsigned char *master_b=backup(p,&master_sz); fd=open(p,O_WRONLY); must(fd>=0,"bad master"); must(write(fd,&bad,1)==1,"bad master write"); close(fd); tagcache_close(); must(tagcache_open(root),"source fallback"); check_title(a,"Title-4"); snprintf(p,sizeof(p),"%s/database_8.tcd.g5",root); restore(p,title_b,title_sz); snprintf(p,sizeof(p),"%s/database_idx.tcd.g5",root); restore(p,master_b,master_sz); free(title_b);
    snprintf(p,sizeof(p),"%s/database_9.tcd.g1",root); unlink(p); snprintf(p,sizeof(p),"%s/database_idx.tcd.g5",root); fd=open(p,O_WRONLY); must(fd>=0,"bad latest master"); must(write(fd,&bad,1)==1,"bad latest master write"); close(fd); tagcache_close(); must(tagcache_open(root),"open missing shared source"); check_title(a,"Title-4"); tagcache_close(); restore(p,master_b,master_sz); free(master_b);
    char legacy[]="/tmp/tagcache-legacy-XXXXXX", la[700], lb[700]; must(mkdtemp(legacy)!=NULL,"legacy mkdtemp"); snprintf(la,sizeof(la),"%s/a.flac",legacy); snprintf(lb,sizeof(lb),"%s/b.flac",legacy); fixture(la); fixture(lb);
    must(tagcache_open(legacy),"legacy seed open"); commit(la,lb,1); tagcache_close();
    DIR *d=opendir(legacy); struct dirent *e; while(d&&(e=readdir(d)))if(strncmp(e->d_name,"tagcache.refs.g",15)==0){snprintf(p,sizeof(p),"%s/%s",legacy,e->d_name);unlink(p);}if(d)closedir(d);
    must(tagcache_open(legacy),"legacy open"); commit(la,lb,2); check_title(la,"Title-2"); free(refs_b); tagcache_close(); cleanup(legacy); cleanup(root); puts("tagcache refs: PASS"); return 0;
}
