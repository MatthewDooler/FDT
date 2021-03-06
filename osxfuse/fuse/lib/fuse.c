/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/


/* For pthread_rwlock_t */
#define _GNU_SOURCE

#include "config.h"
#include "fuse_i.h"
#include "fuse_lowlevel.h"
#include "fuse_opt.h"
#include "fuse_misc.h"
#include "fuse_common_compat.h"
#include "fuse_compat.h"
#ifdef __APPLE__
#include "fuse_darwin_private.h"
#endif
#include "cJSON.c"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <assert.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <semaphore.h>

#define FUSE_MAX_PATH 4096
#define FUSE_DEFAULT_INTR_SIGNAL SIGUSR1

#define FUSE_UNKNOWN_INO 0xffffffff
#define OFFSET_MAX 0x7fffffffffffffffLL

struct fuse_config {
	unsigned int uid;
	unsigned int gid;
	unsigned int  umask;
	double entry_timeout;
	double negative_timeout;
	double attr_timeout;
	double ac_attr_timeout;
	int ac_attr_timeout_set;
	int debug;
	int hard_remove;
	int use_ino;
	int readdir_ino;
	int set_mode;
	int set_uid;
	int set_gid;
	int direct_io;
	int kernel_cache;
	int auto_cache;
	int intr;
	int intr_signal;
	int help;
	char *modules;
};

struct fuse_fs {
	struct fuse_operations op;
	struct fuse_wrapper_operations wrapper_op;
	int fdt_debug_mode;
	struct fuse_module *m;
	void *user_data;
	int compat;
	int next_seqnum;
	pthread_mutex_t seqnum_lock;
#ifdef __APPLE__
	struct fuse *fuse;
#endif
};

struct fusemod_so {
	void *handle;
	int ctr;
};

struct fuse {
	struct fuse_session *se;
	struct node **name_table;
	size_t name_table_size;
	struct node **id_table;
	size_t id_table_size;
	fuse_ino_t ctr;
	unsigned int generation;
	unsigned int hidectr;
	pthread_mutex_t lock;
	pthread_rwlock_t tree_lock;
	struct fuse_config conf;
	int intr_installed;
	struct fuse_fs *fs;
};

struct lock {
	int type;
	off_t start;
	off_t end;
	pid_t pid;
	uint64_t owner;
	struct lock *next;
};

struct node {
	struct node *name_next;
	struct node *id_next;
	fuse_ino_t nodeid;
	unsigned int generation;
	int refctr;
	struct node *parent;
	char *name;
	uint64_t nlookup;
	int open_count;
	int is_hidden;
	struct timespec stat_updated;
	struct timespec mtime;
	off_t size;
	int cache_valid;
	struct lock *locks;
};

struct fuse_dh {
	pthread_mutex_t lock;
	struct fuse *fuse;
	fuse_req_t req;
	char *contents;
	int allocated;
	unsigned len;
	unsigned size;
	unsigned needlen;
	int filled;
	uint64_t fh;
	int error;
	fuse_ino_t nodeid;
};

/* old dir handle */
struct fuse_dirhandle {
	fuse_fill_dir_t filler;
	void *buf;
};

struct fuse_context_i {
	struct fuse_context ctx;
	fuse_req_t req;
};

static pthread_key_t fuse_context_key;
static pthread_mutex_t fuse_context_lock = PTHREAD_MUTEX_INITIALIZER;
static int fuse_context_ref;
static struct fusemod_so *fuse_current_so;
static struct fuse_module *fuse_modules;

static int fuse_load_so_name(const char *soname)
{
	struct fusemod_so *so;

	so = calloc(1, sizeof(struct fusemod_so));
	if (!so) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		return -1;
	}

	fuse_current_so = so;
	so->handle = dlopen(soname, RTLD_NOW);
	fuse_current_so = NULL;
	if (!so->handle) {
		fprintf(stderr, "fuse: %s\n", dlerror());
		goto err;
	}
	if (!so->ctr) {
		fprintf(stderr, "fuse: %s did not register any modules",
			soname);
		goto err;
	}
	return 0;

err:
	if (so->handle)
		dlclose(so->handle);
	free(so);
	return -1;
}

static int fuse_load_so_module(const char *module)
{
	int res;
	char *soname = malloc(strlen(module) + 64);
	if (!soname) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		return -1;
	}
	sprintf(soname, "libfusemod_%s.so", module);
	res = fuse_load_so_name(soname);
	free(soname);
	return res;
}

static struct fuse_module *fuse_find_module(const char *module)
{
	struct fuse_module *m;
	for (m = fuse_modules; m; m = m->next) {
		if (strcmp(module, m->name) == 0) {
			m->ctr++;
			break;
		}
	}
	return m;
}

static struct fuse_module *fuse_get_module(const char *module)
{
	struct fuse_module *m;

	pthread_mutex_lock(&fuse_context_lock);
	m = fuse_find_module(module);
	if (!m) {
		int err = fuse_load_so_module(module);
		if (!err)
			m = fuse_find_module(module);
	}
	pthread_mutex_unlock(&fuse_context_lock);
	return m;
}

static void fuse_put_module(struct fuse_module *m)
{
	pthread_mutex_lock(&fuse_context_lock);
	assert(m->ctr > 0);
	m->ctr--;
	if (!m->ctr && m->so) {
		struct fusemod_so *so = m->so;
		assert(so->ctr > 0);
		so->ctr--;
		if (!so->ctr) {
			struct fuse_module **mp;
			for (mp = &fuse_modules; *mp;) {
				if ((*mp)->so == so)
					*mp = (*mp)->next;
				else
					mp = &(*mp)->next;
			}
			dlclose(so->handle);
			free(so);
		}
	}
	pthread_mutex_unlock(&fuse_context_lock);
}






const char *debugFifoName = "fuse-debug.fifo";
FILE *debugFifo = NULL;

const char *stepSemName = "fuse-step.sem";
sem_t *stepSem = NULL;

void log_init(void) {
    if(debugFifo == NULL) {
    	debugFifo = fopen(debugFifoName, "w");
    }
    stepSem = sem_open(stepSemName, 0);
}
void log_destroy(void) {
	if(debugFifo != NULL) {
		fclose(debugFifo);
		debugFifo = NULL;
		unlink(debugFifoName);
	}
}

cJSON * statToJSONObject(struct stat * s)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "st_dev", s->st_dev);
    cJSON_AddNumberToObject(obj, "st_ino", s->st_ino);
    cJSON_AddNumberToObject(obj, "st_mode", s->st_mode);
    cJSON_AddNumberToObject(obj, "st_nlink", s->st_nlink);
    cJSON_AddNumberToObject(obj, "st_uid", s->st_uid);
    cJSON_AddNumberToObject(obj, "st_gid", s->st_gid);
    cJSON_AddNumberToObject(obj, "st_rdev", s->st_rdev);
    cJSON_AddNumberToObject(obj, "st_size", s->st_size);
    cJSON_AddNumberToObject(obj, "st_atime", s->st_atime);
    cJSON_AddNumberToObject(obj, "st_mtime", s->st_mtime);
    cJSON_AddNumberToObject(obj, "st_ctime", s->st_ctime);
    cJSON_AddNumberToObject(obj, "st_blksize", s->st_blksize);
    cJSON_AddNumberToObject(obj, "st_blocks", s->st_blocks);
    return obj;
}

cJSON * fuseFileInfoToJSONObject(struct fuse_file_info * fi)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "flags", fi->flags);
    cJSON_AddNumberToObject(obj, "fh_old", fi->fh_old);
    cJSON_AddNumberToObject(obj, "writepage", fi->writepage);
    cJSON_AddNumberToObject(obj, "direct_io", fi->direct_io);
    cJSON_AddNumberToObject(obj, "keep_cache", fi->keep_cache);
    cJSON_AddNumberToObject(obj, "flush", fi->flush);
    cJSON_AddNumberToObject(obj, "fh", fi->fh);
    cJSON_AddNumberToObject(obj, "lock_owner", fi->lock_owner);
    return obj;
}

cJSON * utimbufToJSONObject(struct utimbuf * ubuf)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "actime", ubuf->actime);
    cJSON_AddNumberToObject(obj, "modtime", ubuf->modtime);
    return obj;
}

cJSON * timespecArrayToJSONObject(const struct timespec * tv, size_t size)
{
    cJSON *obj = cJSON_CreateArray();
    size_t i;
    for(i = 0; i < size; i++) {
    	cJSON_AddItemToArray(obj, timespecToJSONObject(tv[i]));
    }
    return obj;
}

cJSON * timespecToJSONObject(const struct timespec tv)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "tv_sec", tv.tv_sec);
    cJSON_AddNumberToObject(obj, "tv_nsec", tv.tv_nsec);
    return obj;
}

cJSON * statvfsToJSONObject(struct statvfs * statvfs)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "f_bsize", statvfs->f_bsize);
    cJSON_AddNumberToObject(obj, "f_frsize", statvfs->f_frsize);
    cJSON_AddNumberToObject(obj, "f_blocks", statvfs->f_blocks);
    cJSON_AddNumberToObject(obj, "f_bfree", statvfs->f_bfree);
    cJSON_AddNumberToObject(obj, "f_bavail", statvfs->f_bavail);
    cJSON_AddNumberToObject(obj, "f_files", statvfs->f_files);
    cJSON_AddNumberToObject(obj, "f_ffree", statvfs->f_ffree);
    cJSON_AddNumberToObject(obj, "f_favail", statvfs->f_favail);
    cJSON_AddNumberToObject(obj, "f_fsid", statvfs->f_fsid);
    cJSON_AddNumberToObject(obj, "f_flag", statvfs->f_flag);
    cJSON_AddNumberToObject(obj, "f_namemax", statvfs->f_namemax);
    return obj;
}

cJSON * fuseConnInfoToJSONObject(struct fuse_conn_info *conn)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "proto_major", conn->proto_major);
    cJSON_AddNumberToObject(obj, "proto_minor", conn->proto_minor);
    cJSON_AddNumberToObject(obj, "async_read", conn->async_read);
    cJSON_AddNumberToObject(obj, "max_write", conn->max_write);
    cJSON_AddNumberToObject(obj, "max_readahead", conn->max_readahead);

#ifdef __APPLE__
    cJSON *enable = cJSON_CreateObject();
    cJSON_AddNumberToObject(enable, "case_insensitive", conn->enable.case_insensitive);
    cJSON_AddNumberToObject(enable, "setvolname", conn->enable.setvolname);
    cJSON_AddNumberToObject(enable, "xtimes", conn->enable.xtimes);
    cJSON_AddItemToObject(obj, "enable", enable);
#endif /* __APPLE__ */

    return obj;
}

cJSON * flockToJSONObject(struct flock * flock)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "l_type", flock->l_type);
    cJSON_AddNumberToObject(obj, "l_whence", flock->l_whence);
    cJSON_AddNumberToObject(obj, "l_start", flock->l_start);
    cJSON_AddNumberToObject(obj, "l_len", flock->l_len);
    cJSON_AddNumberToObject(obj, "l_pid", flock->l_pid);
    return obj;
}

cJSON * setattrXToJSONObject(struct setattr_x *attr)
{
    cJSON *obj = cJSON_CreateObject();
	cJSON_AddNumberToObject(obj, "valid", attr->valid);
	cJSON_AddNumberToObject(obj, "mode", attr->mode);
	cJSON_AddNumberToObject(obj, "uid", attr->uid);
	cJSON_AddNumberToObject(obj, "gid", attr->gid);
	cJSON_AddNumberToObject(obj, "size", attr->size);
	cJSON_AddItemToObject(obj, "acctime", timespecToJSONObject(attr->acctime));
	cJSON_AddItemToObject(obj, "modtime", timespecToJSONObject(attr->modtime));
	cJSON_AddItemToObject(obj, "crtime", timespecToJSONObject(attr->crtime));
	cJSON_AddItemToObject(obj, "chgtime", timespecToJSONObject(attr->chgtime));
	cJSON_AddItemToObject(obj, "bkuptime", timespecToJSONObject(attr->bkuptime));
	cJSON_AddNumberToObject(obj, "flags", attr->flags);
    return obj;
}

void report_fs_call(struct fuse_fs * fs, const char *name, int seqnum, cJSON * params)
{
	if(fs->fdt_debug_mode) {
		cJSON *event = cJSON_CreateObject();
		cJSON_AddStringToObject(event, "type", "invoke");
		cJSON_AddStringToObject(event, "name", name);
		cJSON_AddNumberToObject(event, "seqnum", seqnum);
		cJSON_AddItemToObject(event, "params", params);

		char *event_json = cJSON_Print(event);
		cJSON_Delete(event);
	    fwrite(event_json, sizeof(char), strlen(event_json), debugFifo);
	    fflush(debugFifo);
		free(event_json);

		// Wait until the debugger advances execution
		sem_wait(stepSem);
	}
}

void report_fs_call_return(struct fuse_fs * fs, const char *name, int seqnum, int *return_val_ptr, cJSON * modified_params)
{
    if(fs->fdt_debug_mode) {
	    cJSON *event = cJSON_CreateObject();
		cJSON_AddStringToObject(event, "type", "return");
		cJSON_AddStringToObject(event, "name", name);
		cJSON_AddNumberToObject(event, "seqnum", seqnum);
		
		if(return_val_ptr != NULL) {
		    cJSON_AddNumberToObject(event, "returnval", *return_val_ptr);
	    } else {
	        cJSON_AddNullToObject(event, "returnval");
	    }

	    cJSON_AddItemToObject(event, "modified_params", modified_params);
	    
		char *event_json = cJSON_Print(event);
		cJSON_Delete(event);
	    fwrite(event_json, sizeof(char), strlen(event_json), debugFifo);
	    fflush(debugFifo);
		free(event_json);
	}
}

// Thread-safe generation of sequence numbers for function calls on a filesystem
int next_seqnum(struct fuse_fs * fs) {
	pthread_mutex_lock(&fs->seqnum_lock);
	int seqnum = fs->next_seqnum++;
	pthread_mutex_unlock(&fs->seqnum_lock);
	return seqnum;
}

// Wrappers around user-defined FUSE ops
int fuse_op_wrapper_getattr(void *fs_ptr, const char * path, struct stat * s)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "stat", statToJSONObject(s));
    report_fs_call(fs, "getattr", seqnum, params);
    
    int r = fs->op.getattr(path, s);

    cJSON *modified_params = cJSON_CreateObject();
	cJSON_AddItemToObject(modified_params, "stat", statToJSONObject(s));
    report_fs_call_return(fs, "getattr", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_readlink(void *fs_ptr, const char * path, char * link, size_t size)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "link", (uintptr_t) link);
	cJSON_AddNumberToObject(params, "size", size);
    report_fs_call(fs, "readlink", seqnum, params);
    
    int r = fs->op.readlink(path, link, size);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddStringToObject(modified_params, "link", link);
    report_fs_call_return(fs, "readlink", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_getdir(void *fs_ptr, const char * path, fuse_dirh_t b, fuse_dirfil_t c)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
    report_fs_call(fs, "getdir", seqnum, params);
    
    int r = fs->op.getdir(path, b, c);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "getdir", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_mknod(void *fs_ptr, const char * path, mode_t mode, dev_t dev)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "mode", mode);
	cJSON_AddNumberToObject(params, "dev", dev);
    report_fs_call(fs, "mknod", seqnum, params);
    
    int r = fs->op.mknod(path, mode, dev);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "mknod", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_mkdir(void *fs_ptr, const char * path, mode_t mode)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "mode", mode);
    report_fs_call(fs, "mkdir", seqnum, params);
    
    int r = fs->op.mkdir(path, mode);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "mkdir", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_unlink(void *fs_ptr, const char * path)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
    report_fs_call(fs, "unlink", seqnum, params);
    
    int r = fs->op.unlink(path);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "unlink", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_rmdir(void *fs_ptr, const char * path)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
    report_fs_call(fs, "rmdir", seqnum, params);
    
    int r = fs->op.rmdir(path);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "rmdir", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_symlink(void *fs_ptr, const char * path, const char * link)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddStringToObject(params, "link", link);
    report_fs_call(fs, "symlink", seqnum, params);
    
    int r = fs->op.symlink(path, link);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "symlink", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_rename(void *fs_ptr, const char * path, const char * newpath)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddStringToObject(params, "newpath", newpath);
    report_fs_call(fs, "rename", seqnum, params);
    
    int r = fs->op.rename(path, newpath);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "rename", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_link(void *fs_ptr, const char * path, const char * newpath)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddStringToObject(params, "newpath", newpath);
    report_fs_call(fs, "link", seqnum, params);
    
    int r = fs->op.link(path, newpath);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "link", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_chmod(void *fs_ptr, const char * path, mode_t mode)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "mode", mode);
    report_fs_call(fs, "chmod", seqnum, params);
    
    int r = fs->op.chmod(path, mode);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "chmod", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_chown(void *fs_ptr, const char * path, uid_t uid, gid_t gid)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "uid", uid);
	cJSON_AddNumberToObject(params, "gid", gid);
    report_fs_call(fs, "chown", seqnum, params);
    
    int r = fs->op.chown(path, uid, gid);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "chown", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_truncate(void *fs_ptr, const char * path, off_t newsize)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "newsize", newsize);
    report_fs_call(fs, "truncate", seqnum, params);
    
    int r = fs->op.truncate(path, newsize);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "truncate", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_utime(void *fs_ptr, const char * path, struct utimbuf * ubuf)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "ubuf", utimbufToJSONObject(ubuf));
    report_fs_call(fs, "utime", seqnum, params);
    
    int r = fs->op.utime(path, ubuf);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "ubuf", utimbufToJSONObject(ubuf));
    report_fs_call_return(fs, "utime", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_open(void *fs_ptr, const char * path, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "open", seqnum, params);
    
    int r = fs->op.open(path, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "open", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_read(void *fs_ptr, const char * path, char * buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "size", size);
	cJSON_AddNumberToObject(params, "offset", offset);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "read", seqnum, params);
    
    int r = fs->op.read(path, buf, size, offset, fi);

    cJSON *modified_params = cJSON_CreateObject();
	char buf_cpy[r+1];
	strncpy(buf_cpy, buf, r);
	buf_cpy[r] = '\0';
    cJSON_AddStringToObject(modified_params, "buf", buf_cpy);
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "read", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_write(void *fs_ptr, const char * path, const char * buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	char buf_cpy[size+1];
	strncpy(buf_cpy, buf, size);
	buf_cpy[size] = '\0';
	cJSON_AddStringToObject(params, "buf", buf_cpy);
	cJSON_AddNumberToObject(params, "size", size);
	cJSON_AddNumberToObject(params, "offset", offset);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "write", seqnum, params);
    
    int r = fs->op.write(path, buf, size, offset, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "write", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_statfs(void *fs_ptr, const char * path, struct statvfs * statvfs)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "statvfs", statvfsToJSONObject(statvfs));
    report_fs_call(fs, "statfs", seqnum, params);
    
    int r = fs->op.statfs(path, statvfs);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "statvfs", statvfsToJSONObject(statvfs));
    report_fs_call_return(fs, "statfs", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_flush(void *fs_ptr, const char * path, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "flush", seqnum, params);
    
    int r = fs->op.flush(path, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "flush", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_release(void *fs_ptr, const char * path, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "release", seqnum, params);
    
    int r = fs->op.release(path, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "release", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_fsync(void *fs_ptr, const char * path, int datasync, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "datasync", datasync);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "fsync", seqnum, params);
    
    int r = fs->op.fsync(path, datasync, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "fsync", seqnum, &r, modified_params);
    return r;
}

#ifdef __APPLE__
	int fuse_op_wrapper_setxattr(void *fs_ptr, const char * path, const char * name, const char * value, size_t size, int flags, uint32_t position)
#else
	int fuse_op_wrapper_setxattr(void *fs_ptr, const char * path, const char * name, const char * value, size_t size, int flags)
#endif
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddStringToObject(params, "name", name);
	cJSON_AddStringToObject(params, "value", value);
	cJSON_AddNumberToObject(params, "size", size);
	cJSON_AddNumberToObject(params, "flags", flags);
	#ifdef __APPLE__
	cJSON_AddNumberToObject(params, "position", position);
	#endif /* __APPLE__ */
    report_fs_call(fs, "setxattr", seqnum, params);
    
#ifdef __APPLE__
	int r = fs->op.setxattr(path, name, value, size, flags, position);
#else
	int r = fs->op.setxattr(path, name, value, size, flags);
#endif

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "setxattr", seqnum, &r, modified_params);
    return r;
}
#ifdef __APPLE__
	int fuse_op_wrapper_getxattr(void *fs_ptr, const char * path, const char * name, char * value, size_t size, uint32_t position)
#else
	int fuse_op_wrapper_getxattr(void *fs_ptr, const char * path, const char * name, char * value, size_t size)
#endif
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddStringToObject(params, "name", name);
	cJSON_AddNumberToObject(params, "value", (uintptr_t) value);
	cJSON_AddNumberToObject(params, "size", size);
	#ifdef __APPLE__
	cJSON_AddNumberToObject(params, "position", position);
	#endif /* __APPLE__ */
    report_fs_call(fs, "getxattr", seqnum, params);
    
#ifdef __APPLE__
	int r = fs->op.getxattr(path, name, value, size, position);
#else
	int r = fs->op.getxattr(path, name, value, size);
#endif

    cJSON *modified_params = cJSON_CreateObject();
    if(value != NULL) {
    	cJSON_AddStringToObject(modified_params, "value", value);
    } else {
    	cJSON_AddNullToObject(modified_params, "value");
    }
    report_fs_call_return(fs, "getxattr", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_listxattr(void *fs_ptr, const char * path, char * list, size_t size)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "list", (uintptr_t) list);
	cJSON_AddNumberToObject(params, "size", size);
    report_fs_call(fs, "listxattr", seqnum, params);
    
    int r = fs->op.listxattr(path, list, size);

    cJSON *modified_params = cJSON_CreateObject();
    if(list != NULL) {
    	cJSON_AddStringToObject(modified_params, "list", list);
    } else {
    	cJSON_AddNullToObject(modified_params, "list");
    }
    report_fs_call_return(fs, "listxattr", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_removexattr(void *fs_ptr, const char * path, const char * name)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddStringToObject(params, "name", name);
    report_fs_call(fs, "removexattr", seqnum, params);
    
    int r = fs->op.removexattr(path, name);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "removexattr", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_opendir(void *fs_ptr, const char * path, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "opendir", seqnum, params);
    
    int r = fs->op.opendir(path, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "opendir", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_readdir(void *fs_ptr, const char * path, void * buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "buf", (uintptr_t) buf);
	cJSON_AddNumberToObject(params, "filler", (uintptr_t) filler);
	cJSON_AddNumberToObject(params, "offset", offset);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "readdir", seqnum, params);
    
    int r = fs->op.readdir(path, buf, filler, offset, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "readdir", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_releasedir(void *fs_ptr, const char * path, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "releasedir", seqnum, params);
    
    int r = fs->op.releasedir(path, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "releasedir", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_fsyncdir(void *fs_ptr, const char * path, int datasync, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "datasync", datasync);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "fsyncdir", seqnum, params);
    
    int r = fs->op.fsyncdir(path, datasync, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "fsyncdir", seqnum, &r, modified_params);
    return r;
}
void * fuse_op_wrapper_init(void *fs_ptr, struct fuse_conn_info *conn)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "conn", fuseConnInfoToJSONObject(conn));
    report_fs_call(fs, "init", seqnum, params);
    void * r = fs->op.init(conn);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "conn", fuseConnInfoToJSONObject(conn));
    report_fs_call_return(fs, "init", seqnum, NULL, modified_params);
    return r;
}
void fuse_op_wrapper_destroy(void *fs_ptr, void * userdata)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "userdata", (uintptr_t) userdata);
    report_fs_call(fs, "destroy", seqnum, params);

    fs->op.destroy(userdata);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "destroy", seqnum, NULL, modified_params);
}
int fuse_op_wrapper_access(void *fs_ptr, const char * path, int mask)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "mask", mask);
    report_fs_call(fs, "access", seqnum, params);
    
    int r = fs->op.access(path, mask);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "access", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_create(void *fs_ptr, const char * path, mode_t mode, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "mode", mode);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "create", seqnum, params);
    
    int r = fs->op.create(path, mode, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "create", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_ftruncate(void *fs_ptr, const char * path, off_t offset, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "offset", offset);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "ftruncate", seqnum, params);
    
    int r = fs->op.ftruncate(path, offset, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "ftruncate", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_fgetattr(void *fs_ptr, const char * path, struct stat * s, struct fuse_file_info * fi)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "stat", statToJSONObject(s));
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "fgetattr", seqnum, params);
    
    int r = fs->op.fgetattr(path, s, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "stat", statToJSONObject(s));
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "fgetattr", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_lock(void *fs_ptr, const char * path, struct fuse_file_info * fi, int cmd, struct flock * flock)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
	cJSON_AddNumberToObject(params, "cmd", cmd);
	cJSON_AddItemToObject(params, "flock", flockToJSONObject(flock));
    report_fs_call(fs, "lock", seqnum, params);
    
    int r = fs->op.lock(path, fi, cmd, flock);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    cJSON_AddItemToObject(modified_params, "flock", flockToJSONObject(flock));
    report_fs_call_return(fs, "lock", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_utimens(void *fs_ptr, const char * path, const struct timespec tv[2])
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "tv", timespecArrayToJSONObject(tv, 2));
    report_fs_call(fs, "utimens", seqnum, params);
    
    int r = fs->op.utimens(path, tv);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "utimens", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_bmap(void *fs_ptr, const char * path, size_t blocksize, uint64_t * idx)
{
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "blocksize", blocksize);
	if(idx != NULL) {
		cJSON_AddNumberToObject(params, "idx", *idx);
	} else {
		cJSON_AddNullToObject(params, "idx");
	}
    report_fs_call(fs, "bmap", seqnum, params);
    
    int r = fs->op.bmap(path, blocksize, idx);

    cJSON *modified_params = cJSON_CreateObject();
    if(idx != NULL) {
		cJSON_AddNumberToObject(modified_params, "idx", *idx);
	} else {
		cJSON_AddNullToObject(modified_params, "idx");
	}
    report_fs_call_return(fs, "bmap", seqnum, &r, modified_params);
    return r;
}

// osxfuse-only function wrappers
#ifdef __APPLE__
int fuse_op_wrapper_setvolname(void *fs_ptr, const char *volname) {
    struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "volname", volname);
    report_fs_call(fs, "setvolname", seqnum, params);
    
    int r = fs->op.setvolname(volname);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "setvolname", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_exchange(void *fs_ptr, const char *path1, const char *path2, unsigned long options) {
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path1", path1);
	cJSON_AddStringToObject(params, "path2", path2);
	cJSON_AddNumberToObject(params, "options", options);
    report_fs_call(fs, "exchange", seqnum, params);
    
    int r = fs->op.exchange(path1, path2, options);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "exchange", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_getxtimes(void *fs_ptr, const char *path, struct timespec *bkuptime, struct timespec *crtime) {
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "bkuptime", timespecToJSONObject(*bkuptime));
	cJSON_AddItemToObject(params, "crtime", timespecToJSONObject(*crtime));
    report_fs_call(fs, "getxtimes", seqnum, params);
    
    int r = fs->op.getxtimes(path, bkuptime, crtime);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "bkuptime", timespecToJSONObject(*bkuptime));
	cJSON_AddItemToObject(modified_params, "crtime", timespecToJSONObject(*crtime));
    report_fs_call_return(fs, "getxtimes", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_setbkuptime(void *fs_ptr, const char *path, const struct timespec *tv) {
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "tv", timespecToJSONObject(*tv));
    report_fs_call(fs, "setbkuptime", seqnum, params);
    
    int r = fs->op.setbkuptime(path, tv);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "setbkuptime", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_setchgtime(void *fs_ptr, const char *path, const struct timespec *tv) {
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "tv", timespecToJSONObject(*tv));
    report_fs_call(fs, "setchgtime", seqnum, params);
    
    int r = fs->op.setchgtime(path, tv);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "setchgtime", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_setcrtime(void *fs_ptr, const char *path, const struct timespec *tv) {
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "tv", timespecToJSONObject(*tv));
    report_fs_call(fs, "setcrtime", seqnum, params);
    
    int r = fs->op.setcrtime(path, tv);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "setcrtime", seqnum, &r, modified_params);
    return r;
}
int fuse_op_wrapper_chflags(void *fs_ptr, const char *path, uint32_t flags) {
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddNumberToObject(params, "flags", flags);
    report_fs_call(fs, "chflags", seqnum, params);
    
    int r = fs->op.chflags(path, flags);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "chflags", seqnum, &r, modified_params);
    return r;
}
#endif /* __APPLE__ */

int fuse_op_wrapper_setattr_x(void *fs_ptr, const char *path, struct setattr_x *attr)
{
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "attr", setattrXToJSONObject(attr));
    report_fs_call(fs, "setattr_x", seqnum, params);
    
    int r = fs->op.setattr_x(path, attr);

    cJSON *modified_params = cJSON_CreateObject();
    report_fs_call_return(fs, "setattr_x", seqnum, &r, modified_params);
    return r;
}

int fuse_op_wrapper_fsetattr_x(void *fs_ptr, const char *path, struct setattr_x *attr, struct fuse_file_info *fi)
{
	struct fuse_fs *fs = (struct fuse_fs *) fs_ptr;
    int seqnum = next_seqnum(fs);

    cJSON *params = cJSON_CreateObject();	
	cJSON_AddStringToObject(params, "path", path);
	cJSON_AddItemToObject(params, "attr", setattrXToJSONObject(attr));
	cJSON_AddItemToObject(params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call(fs, "fsetattr_x", seqnum, params);
    
    int r = fs->op.fsetattr_x(path, attr, fi);

    cJSON *modified_params = cJSON_CreateObject();
    cJSON_AddItemToObject(modified_params, "fi", fuseFileInfoToJSONObject(fi));
    report_fs_call_return(fs, "fsetattr_x", seqnum, &r, modified_params);
    return r;
}

static struct node *get_node_nocheck(struct fuse *f, fuse_ino_t nodeid)
{
	size_t hash = nodeid % f->id_table_size;
	struct node *node;

	for (node = f->id_table[hash]; node != NULL; node = node->id_next)
		if (node->nodeid == nodeid)
			return node;

	return NULL;
}

static struct node *get_node(struct fuse *f, fuse_ino_t nodeid)
{
	struct node *node = get_node_nocheck(f, nodeid);
	if (!node) {
		fprintf(stderr, "fuse internal error: node %llu not found\n",
			(unsigned long long) nodeid);
		abort();
	}
	return node;
}

static void free_node(struct node *node)
{
	free(node->name);
	free(node);
}

static void unhash_id(struct fuse *f, struct node *node)
{
	size_t hash = node->nodeid % f->id_table_size;
	struct node **nodep = &f->id_table[hash];

	for (; *nodep != NULL; nodep = &(*nodep)->id_next)
		if (*nodep == node) {
			*nodep = node->id_next;
			return;
		}
}

static void hash_id(struct fuse *f, struct node *node)
{
	size_t hash = node->nodeid % f->id_table_size;
	node->id_next = f->id_table[hash];
	f->id_table[hash] = node;
}

static unsigned int name_hash(struct fuse *f, fuse_ino_t parent,
			      const char *name)
{
	unsigned int hash = *name;

	if (hash)
		for (name += 1; *name != '\0'; name++)
			hash = (hash << 5) - hash + *name;

	return (hash + parent) % f->name_table_size;
}

static void unref_node(struct fuse *f, struct node *node);

static void unhash_name(struct fuse *f, struct node *node)
{
	if (node->name) {
		size_t hash = name_hash(f, node->parent->nodeid, node->name);
		struct node **nodep = &f->name_table[hash];

		for (; *nodep != NULL; nodep = &(*nodep)->name_next)
			if (*nodep == node) {
				*nodep = node->name_next;
				node->name_next = NULL;
				unref_node(f, node->parent);
				free(node->name);
				node->name = NULL;
				node->parent = NULL;
				return;
			}
		fprintf(stderr,
			"fuse internal error: unable to unhash node: %llu\n",
			(unsigned long long) node->nodeid);
		abort();
	}
}

static int hash_name(struct fuse *f, struct node *node, fuse_ino_t parentid,
		     const char *name)
{
	size_t hash = name_hash(f, parentid, name);
	struct node *parent = get_node(f, parentid);
	node->name = strdup(name);
	if (node->name == NULL)
		return -1;

	parent->refctr ++;
	node->parent = parent;
	node->name_next = f->name_table[hash];
	f->name_table[hash] = node;
	return 0;
}

static void delete_node(struct fuse *f, struct node *node)
{
	if (f->conf.debug)
		fprintf(stderr, "delete: %llu\n",
			(unsigned long long) node->nodeid);

	assert(!node->name);
	unhash_id(f, node);
	free_node(node);
}

static void unref_node(struct fuse *f, struct node *node)
{
	assert(node->refctr > 0);
	node->refctr --;
	if (!node->refctr)
		delete_node(f, node);
}

static fuse_ino_t next_id(struct fuse *f)
{
	do {
		f->ctr = (f->ctr + 1) & 0xffffffff;
		if (!f->ctr)
			f->generation ++;
	} while (f->ctr == 0 || f->ctr == FUSE_UNKNOWN_INO ||
		 get_node_nocheck(f, f->ctr) != NULL);
	return f->ctr;
}

static struct node *lookup_node(struct fuse *f, fuse_ino_t parent,
				const char *name)
{
	size_t hash = name_hash(f, parent, name);
	struct node *node;

	for (node = f->name_table[hash]; node != NULL; node = node->name_next)
		if (node->parent->nodeid == parent &&
		    strcmp(node->name, name) == 0)
			return node;

	return NULL;
}

static struct node *find_node(struct fuse *f, fuse_ino_t parent,
			      const char *name)
{
	struct node *node;

	pthread_mutex_lock(&f->lock);
	node = lookup_node(f, parent, name);
	if (node == NULL) {
		node = (struct node *) calloc(1, sizeof(struct node));
		if (node == NULL)
			goto out_err;

		node->refctr = 1;
		node->nodeid = next_id(f);
		node->open_count = 0;
		node->is_hidden = 0;
		node->generation = f->generation;
		if (hash_name(f, node, parent, name) == -1) {
			free(node);
			node = NULL;
			goto out_err;
		}
		hash_id(f, node);
	}
	node->nlookup ++;
out_err:
	pthread_mutex_unlock(&f->lock);
	return node;
}

static char *add_name(char *buf, char *s, const char *name)
{
	size_t len = strlen(name);
	s -= len;
	if (s <= buf) {
		fprintf(stderr, "fuse: path too long: ...%s\n", s + len);
		return NULL;
	}
	strncpy(s, name, len);
	s--;
	*s = '/';

	return s;
}

static char *get_path_name(struct fuse *f, fuse_ino_t nodeid, const char *name)
{
	char buf[FUSE_MAX_PATH];
	char *s = buf + FUSE_MAX_PATH - 1;
	struct node *node;

	*s = '\0';

	if (name != NULL) {
		s = add_name(buf, s, name);
		if (s == NULL)
			return NULL;
	}

	pthread_mutex_lock(&f->lock);
	for (node = get_node(f, nodeid); node && node->nodeid != FUSE_ROOT_ID;
	     node = node->parent) {
		if (node->name == NULL) {
			s = NULL;
			break;
		}

		s = add_name(buf, s, node->name);
		if (s == NULL)
			break;
	}
	pthread_mutex_unlock(&f->lock);

	if (node == NULL || s == NULL)
		return NULL;
	else if (*s == '\0')
		return strdup("/");
	else
		return strdup(s);
}

static char *get_path(struct fuse *f, fuse_ino_t nodeid)
{
	return get_path_name(f, nodeid, NULL);
}

static void forget_node(struct fuse *f, fuse_ino_t nodeid, uint64_t nlookup)
{
	struct node *node;
	if (nodeid == FUSE_ROOT_ID)
		return;
	pthread_mutex_lock(&f->lock);
	node = get_node(f, nodeid);
	assert(node->nlookup >= nlookup);
	node->nlookup -= nlookup;
	if (!node->nlookup) {
		unhash_name(f, node);
		unref_node(f, node);
	}
	pthread_mutex_unlock(&f->lock);
}

static void remove_node(struct fuse *f, fuse_ino_t dir, const char *name)
{
	struct node *node;

	pthread_mutex_lock(&f->lock);
	node = lookup_node(f, dir, name);
	if (node != NULL)
		unhash_name(f, node);
	pthread_mutex_unlock(&f->lock);
}

static int rename_node(struct fuse *f, fuse_ino_t olddir, const char *oldname,
		       fuse_ino_t newdir, const char *newname, int hide)
{
	struct node *node;
	struct node *newnode;
	int err = 0;

	pthread_mutex_lock(&f->lock);
	node  = lookup_node(f, olddir, oldname);
	newnode	 = lookup_node(f, newdir, newname);
	if (node == NULL)
		goto out;

	if (newnode != NULL) {
		if (hide) {
			fprintf(stderr, "fuse: hidden file got created during hiding\n");
			err = -EBUSY;
			goto out;
		}
		unhash_name(f, newnode);
	}

	unhash_name(f, node);
	if (hash_name(f, node, newdir, newname) == -1) {
		err = -ENOMEM;
		goto out;
	}

	if (hide)
		node->is_hidden = 1;

out:
	pthread_mutex_unlock(&f->lock);
	return err;
}

static void set_stat(struct fuse *f, fuse_ino_t nodeid, struct stat *stbuf)
{
	if (!f->conf.use_ino)
		stbuf->st_ino = nodeid;
	if (f->conf.set_mode)
		stbuf->st_mode = (stbuf->st_mode & S_IFMT) |
				 (0777 & ~f->conf.umask);
	if (f->conf.set_uid)
		stbuf->st_uid = f->conf.uid;
	if (f->conf.set_gid)
		stbuf->st_gid = f->conf.gid;
}

static struct fuse *req_fuse(fuse_req_t req)
{
	return (struct fuse *) fuse_req_userdata(req);
}

static void fuse_intr_sighandler(int sig)
{
	(void) sig;
	/* Nothing to do */
}

struct fuse_intr_data {
	pthread_t id;
	pthread_cond_t cond;
	int finished;
};

static void fuse_interrupt(fuse_req_t req, void *d_)
{
	struct fuse_intr_data *d = d_;
	struct fuse *f = req_fuse(req);

	if (d->id == pthread_self())
		return;

	pthread_mutex_lock(&f->lock);
	while (!d->finished) {
		struct timeval now;
		struct timespec timeout;

		pthread_kill(d->id, f->conf.intr_signal);
		gettimeofday(&now, NULL);
		timeout.tv_sec = now.tv_sec + 1;
		timeout.tv_nsec = now.tv_usec * 1000;
		pthread_cond_timedwait(&d->cond, &f->lock, &timeout);
	}
	pthread_mutex_unlock(&f->lock);
}

static void fuse_do_finish_interrupt(struct fuse *f, fuse_req_t req,
				     struct fuse_intr_data *d)
{
	pthread_mutex_lock(&f->lock);
	d->finished = 1;
	pthread_cond_broadcast(&d->cond);
	pthread_mutex_unlock(&f->lock);
	fuse_req_interrupt_func(req, NULL, NULL);
	pthread_cond_destroy(&d->cond);
}

static void fuse_do_prepare_interrupt(fuse_req_t req, struct fuse_intr_data *d)
{
	d->id = pthread_self();
	pthread_cond_init(&d->cond, NULL);
	d->finished = 0;
	fuse_req_interrupt_func(req, fuse_interrupt, d);
}

static inline void fuse_finish_interrupt(struct fuse *f, fuse_req_t req,
					 struct fuse_intr_data *d)
{
	if (f->conf.intr)
		fuse_do_finish_interrupt(f, req, d);
}

static inline void fuse_prepare_interrupt(struct fuse *f, fuse_req_t req,
					  struct fuse_intr_data *d)
{
	if (f->conf.intr)
		fuse_do_prepare_interrupt(req, d);
}

#if !defined(__FreeBSD__) && !defined(__APPLE__)

static int fuse_compat_open(struct fuse_fs *fs, const char *path,
			    struct fuse_file_info *fi)
{
	int err;
	if (!fs->compat || fs->compat >= 25)
		err = fs->wrapper_op.open(fs, path, fi);
	else if (fs->compat == 22) {
		struct fuse_file_info_compat tmp;
		memcpy(&tmp, fi, sizeof(tmp));
		err = ((struct fuse_operations_compat22 *) &fs->op)->open(path,
									  &tmp);
		memcpy(fi, &tmp, sizeof(tmp));
		fi->fh = tmp.fh;
	} else
		err = ((struct fuse_operations_compat2 *) &fs->op)
			->open(path, fi->flags);
	return err;
}

static int fuse_compat_release(struct fuse_fs *fs, const char *path,
			       struct fuse_file_info *fi)
{
	if (!fs->compat || fs->compat >= 22)
		return fs->wrapper_op.release(fs, path, fi);
	else
		return ((struct fuse_operations_compat2 *) &fs->op)
			->release(path, fi->flags);
}

static int fuse_compat_opendir(struct fuse_fs *fs, const char *path,
			       struct fuse_file_info *fi)
{
	if (!fs->compat || fs->compat >= 25)
		return fs->wrapper_op.opendir(fs, path, fi);
	else {
		int err;
		struct fuse_file_info_compat tmp;
		memcpy(&tmp, fi, sizeof(tmp));
		err = ((struct fuse_operations_compat22 *) &fs->op)
			->opendir(path, &tmp);
		memcpy(fi, &tmp, sizeof(tmp));
		fi->fh = tmp.fh;
		return err;
	}
}

static void convert_statfs_compat(struct fuse_statfs_compat1 *compatbuf,
				  struct statvfs *stbuf)
{
	stbuf->f_bsize	 = compatbuf->block_size;
	stbuf->f_blocks	 = compatbuf->blocks;
	stbuf->f_bfree	 = compatbuf->blocks_free;
	stbuf->f_bavail	 = compatbuf->blocks_free;
	stbuf->f_files	 = compatbuf->files;
	stbuf->f_ffree	 = compatbuf->files_free;
	stbuf->f_namemax = compatbuf->namelen;
}

static void convert_statfs_old(struct statfs *oldbuf, struct statvfs *stbuf)
{
	stbuf->f_bsize	 = oldbuf->f_bsize;
	stbuf->f_blocks	 = oldbuf->f_blocks;
	stbuf->f_bfree	 = oldbuf->f_bfree;
	stbuf->f_bavail	 = oldbuf->f_bavail;
	stbuf->f_files	 = oldbuf->f_files;
	stbuf->f_ffree	 = oldbuf->f_ffree;
	stbuf->f_namemax = oldbuf->f_namelen;
}

static int fuse_compat_statfs(struct fuse_fs *fs, const char *path,
			      struct statvfs *buf)
{
	int err;

	if (!fs->compat || fs->compat >= 25) {
		err = fs->wrapper_op.statfs(fs, fs->compat == 25 ? "/" : path, buf);
	} else if (fs->compat > 11) {
		struct statfs oldbuf;
		err = ((struct fuse_operations_compat22 *) &fs->op)
			->statfs("/", &oldbuf);
		if (!err)
			convert_statfs_old(&oldbuf, buf);
	} else {
		struct fuse_statfs_compat1 compatbuf;
		memset(&compatbuf, 0, sizeof(struct fuse_statfs_compat1));
		err = ((struct fuse_operations_compat1 *) &fs->op)
			->statfs(&compatbuf);
		if (!err)
			convert_statfs_compat(&compatbuf, buf);
	}
	return err;
}

#else /* __FreeBSD__ || __APPLE__ */

static inline int fuse_compat_open(struct fuse_fs *fs, char *path,
				   struct fuse_file_info *fi)
{
	return fs->wrapper_op.open(fs, path, fi);
}

static inline int fuse_compat_release(struct fuse_fs *fs, const char *path,
				      struct fuse_file_info *fi)
{
	return fs->wrapper_op.release(fs, path, fi);
}

static inline int fuse_compat_opendir(struct fuse_fs *fs, const char *path,
				      struct fuse_file_info *fi)
{
	return fs->wrapper_op.opendir(fs, path, fi);
}

static inline int fuse_compat_statfs(struct fuse_fs *fs, const char *path,
				     struct statvfs *buf)
{
	return fs->wrapper_op.statfs(fs, fs->compat == 25 ? "/" : path, buf);
}

int fuse_fs_setattr_x(struct fuse_fs *fs, const char *path,
      		      struct setattr_x *attr)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.setattr_x)
		return fs->wrapper_op.setattr_x(fs, path, attr);
	else
		return -ENOSYS;
}

int fuse_fs_fsetattr_x(struct fuse_fs *fs, const char *path,
      		       struct setattr_x *attr, struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.fsetattr_x)
		return fs->wrapper_op.fsetattr_x(fs, path, attr, fi);
	else
		return -ENOSYS;
}

#endif /* !__FreeBSD__ && !__APPLE__ */

int fuse_fs_getattr(struct fuse_fs *fs, const char *path, struct stat *buf)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.getattr)
		return fs->wrapper_op.getattr(fs, path, buf);
	else
		return -ENOSYS;
}

int fuse_fs_fgetattr(struct fuse_fs *fs, const char *path, struct stat *buf,
		     struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.fgetattr)
		return fs->wrapper_op.fgetattr(fs, path, buf, fi);
	else if (fs->wrapper_op.getattr)
		return fs->wrapper_op.getattr(fs, path, buf);
	else
		return -ENOSYS;
}

int fuse_fs_rename(struct fuse_fs *fs, const char *oldpath,
		   const char *newpath)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.rename)
		return fs->wrapper_op.rename(fs, oldpath, newpath);
	else
		return -ENOSYS;
}

#ifdef __APPLE__

int fuse_fs_setvolname(struct fuse_fs *fs, const char *volname)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.setvolname)
		return fs->wrapper_op.setvolname(fs, volname);
	else
		return -ENOSYS;
}

int fuse_fs_exchange(struct fuse_fs *fs, const char *path1,
		     const char *path2, unsigned long options)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.exchange)
		return fs->wrapper_op.exchange(fs, path1, path2, options);
	else
		return -ENOSYS;
}

int fuse_fs_getxtimes(struct fuse_fs *fs, const char *path,
		      struct timespec *bkuptime, struct timespec *crtime)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.getxtimes)
		return fs->wrapper_op.getxtimes(fs, path, bkuptime, crtime);
	else
		return -ENOSYS;
}

int fuse_fs_setbkuptime(struct fuse_fs *fs, const char *path,
			const struct timespec *tv)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.setbkuptime)
		return fs->wrapper_op.setbkuptime(fs, path, tv);
	else
		return -ENOSYS;
}

int fuse_fs_setchgtime(struct fuse_fs *fs, const char *path,
		       const struct timespec *tv)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.setchgtime)
		return fs->wrapper_op.setchgtime(fs, path, tv);
	else
		return -ENOSYS;
}

int fuse_fs_setcrtime(struct fuse_fs *fs, const char *path,
		      const struct timespec *tv)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.setcrtime)
		return fs->wrapper_op.setcrtime(fs, path, tv);
	else
		return -ENOSYS;
}

#endif /* __APPLE__ */

int fuse_fs_unlink(struct fuse_fs *fs, const char *path)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.unlink)
		return fs->wrapper_op.unlink(fs, path);
	else
		return -ENOSYS;
}

int fuse_fs_rmdir(struct fuse_fs *fs, const char *path)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.rmdir)
		return fs->wrapper_op.rmdir(fs, path);
	else
		return -ENOSYS;
}

int fuse_fs_symlink(struct fuse_fs *fs, const char *linkname, const char *path)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.symlink)
		return fs->wrapper_op.symlink(fs, linkname, path);
	else
		return -ENOSYS;
}

int fuse_fs_link(struct fuse_fs *fs, const char *oldpath, const char *newpath)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.link)
		return fs->wrapper_op.link(fs, oldpath, newpath);
	else
		return -ENOSYS;
}

int fuse_fs_release(struct fuse_fs *fs,	 const char *path,
		    struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.release)
		return fuse_compat_release(fs, path, fi);
	else
		return 0;
}

int fuse_fs_opendir(struct fuse_fs *fs, const char *path,
		    struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.opendir)
		return fuse_compat_opendir(fs, path, fi);
	else
		return 0;
}

int fuse_fs_open(struct fuse_fs *fs, const char *path,
		 struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.open)
		return fuse_compat_open(fs, (char *)path, fi);
	else
		return 0;
}

int fuse_fs_read(struct fuse_fs *fs, const char *path, char *buf, size_t size,
		 off_t off, struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.read)
		return fs->wrapper_op.read(fs, path, buf, size, off, fi);
	else
		return -ENOSYS;
}

int fuse_fs_write(struct fuse_fs *fs, const char *path, const char *buf,
		  size_t size, off_t off, struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.write)
		return fs->wrapper_op.write(fs, path, buf, size, off, fi);
	else
		return -ENOSYS;
}

int fuse_fs_fsync(struct fuse_fs *fs, const char *path, int datasync,
		  struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.fsync)
		return fs->wrapper_op.fsync(fs, path, datasync, fi);
	else
		return -ENOSYS;
}

int fuse_fs_fsyncdir(struct fuse_fs *fs, const char *path, int datasync,
		     struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.fsyncdir)
		return fs->wrapper_op.fsyncdir(fs, path, datasync, fi);
	else
		return -ENOSYS;
}

int fuse_fs_flush(struct fuse_fs *fs, const char *path,
		  struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.flush)
		return fs->wrapper_op.flush(fs, path, fi);
	else
		return -ENOSYS;
}

int fuse_fs_statfs(struct fuse_fs *fs, const char *path, struct statvfs *buf)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.statfs)
		return fuse_compat_statfs(fs, path, buf);
	else {
		buf->f_namemax = 255;
		buf->f_bsize = 512;
		return 0;
	}
}

int fuse_fs_releasedir(struct fuse_fs *fs, const char *path,
		       struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.releasedir)
		return fs->wrapper_op.releasedir(fs, path, fi);
	else
		return 0;
}

static int fill_dir_old(struct fuse_dirhandle *dh, const char *name, int type,
			ino_t ino)
{
	int res;
	struct stat stbuf;

	memset(&stbuf, 0, sizeof(stbuf));
	stbuf.st_mode = type << 12;
	stbuf.st_ino = ino;

	res = dh->filler(dh->buf, name, &stbuf, 0);
	return res ? -ENOMEM : 0;
}

int fuse_fs_readdir(struct fuse_fs *fs, const char *path, void *buf,
		    fuse_fill_dir_t filler, off_t off,
		    struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.readdir)
		return fs->wrapper_op.readdir(fs, path, buf, filler, off, fi);
	else if (fs->wrapper_op.getdir) {
		struct fuse_dirhandle dh;
		dh.filler = filler;
		dh.buf = buf;
		return fs->wrapper_op.getdir(fs, path, &dh, fill_dir_old);
	} else
		return -ENOSYS;
}

int fuse_fs_create(struct fuse_fs *fs, const char *path, mode_t mode,
		   struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.create)
		return fs->wrapper_op.create(fs, path, mode, fi);
	else
		return -ENOSYS;
}

int fuse_fs_lock(struct fuse_fs *fs, const char *path,
		 struct fuse_file_info *fi, int cmd, struct flock *lock)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.lock)
		return fs->wrapper_op.lock(fs, path, fi, cmd, lock);
	else
		return -ENOSYS;
}

int fuse_fs_chown(struct fuse_fs *fs, const char *path, uid_t uid, gid_t gid)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.chown)
		return fs->wrapper_op.chown(fs, path, uid, gid);
	else
		return -ENOSYS;
}

int fuse_fs_truncate(struct fuse_fs *fs, const char *path, off_t size)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.truncate)
		return fs->wrapper_op.truncate(fs, path, size);
	else
		return -ENOSYS;
}

int fuse_fs_ftruncate(struct fuse_fs *fs, const char *path, off_t size,
		      struct fuse_file_info *fi)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.ftruncate)
		return fs->wrapper_op.ftruncate(fs, path, size, fi);
	else if (fs->wrapper_op.truncate)
		return fs->wrapper_op.truncate(fs, path, size);
	else
		return -ENOSYS;
}

int fuse_fs_utimens(struct fuse_fs *fs, const char *path,
		    const struct timespec tv[2])
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.utimens)
		return fs->wrapper_op.utimens(fs, path, tv);
	else if(fs->wrapper_op.utime) {
		struct utimbuf buf;
		buf.actime = tv[0].tv_sec;
		buf.modtime = tv[1].tv_sec;
		return fs->wrapper_op.utime(fs, path, &buf);
	} else
		return -ENOSYS;
}

int fuse_fs_access(struct fuse_fs *fs, const char *path, int mask)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.access)
		return fs->wrapper_op.access(fs, path, mask);
	else
		return -ENOSYS;
}

int fuse_fs_readlink(struct fuse_fs *fs, const char *path, char *buf,
		     size_t len)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.readlink)
		return fs->wrapper_op.readlink(fs, path, buf, len);
	else
		return -ENOSYS;
}

int fuse_fs_mknod(struct fuse_fs *fs, const char *path, mode_t mode,
		  dev_t rdev)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.mknod)
		return fs->wrapper_op.mknod(fs, path, mode, rdev);
	else
		return -ENOSYS;
}

int fuse_fs_mkdir(struct fuse_fs *fs, const char *path, mode_t mode)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.mkdir)
		return fs->wrapper_op.mkdir(fs, path, mode);
	else
		return -ENOSYS;
}

int fuse_fs_setxattr(struct fuse_fs *fs, const char *path, const char *name,
#ifdef __APPLE__
  	             const char *value, size_t size, int flags, uint32_t position)
#else
  	             const char *value, size_t size, int flags);
#endif
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.setxattr)
#ifdef __APPLE__
		return fs->wrapper_op.setxattr(fs, path, name, value, size, flags, position);
#else
		return fs->wrapper_op.setxattr(fs, path, name, value, size, flags);
#endif
	else
		return -ENOSYS;
}

int fuse_fs_getxattr(struct fuse_fs *fs, const char *path, const char *name,
#ifdef __APPLE__
		     char *value, size_t size, uint32_t position)
#else
		     char *value, size_t size)
#endif
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.getxattr)
#ifdef __APPLE__
		return fs->wrapper_op.getxattr(fs, path, name, value, size, position);
#else
		return fs->wrapper_op.getxattr(fs, path, name, value, size);
#endif
	else
		return -ENOSYS;
}

int fuse_fs_listxattr(struct fuse_fs *fs, const char *path, char *list,
		      size_t size)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.listxattr)
		return fs->wrapper_op.listxattr(fs, path, list, size);
	else
		return -ENOSYS;
}

int fuse_fs_bmap(struct fuse_fs *fs, const char *path, size_t blocksize,
		 uint64_t *idx)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.bmap)
		return fs->wrapper_op.bmap(fs, path, blocksize, idx);
	else
		return -ENOSYS;
}

int fuse_fs_removexattr(struct fuse_fs *fs, const char *path, const char *name)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.removexattr)
		return fs->wrapper_op.removexattr(fs, path, name);
	else
		return -ENOSYS;
}

static int is_open(struct fuse *f, fuse_ino_t dir, const char *name)
{
	struct node *node;
	int isopen = 0;
	pthread_mutex_lock(&f->lock);
	node = lookup_node(f, dir, name);
	if (node && node->open_count > 0)
		isopen = 1;
	pthread_mutex_unlock(&f->lock);
	return isopen;
}

static char *hidden_name(struct fuse *f, fuse_ino_t dir, const char *oldname,
			 char *newname, size_t bufsize)
{
	struct stat buf;
	struct node *node;
	struct node *newnode;
	char *newpath;
	int res;
	int failctr = 10;

	do {
		pthread_mutex_lock(&f->lock);
		node = lookup_node(f, dir, oldname);
		if (node == NULL) {
			pthread_mutex_unlock(&f->lock);
			return NULL;
		}
		do {
			f->hidectr ++;
			snprintf(newname, bufsize, ".fuse_hidden%08x%08x",
				 (unsigned int) node->nodeid, f->hidectr);
			newnode = lookup_node(f, dir, newname);
		} while(newnode);
		pthread_mutex_unlock(&f->lock);

		newpath = get_path_name(f, dir, newname);
		if (!newpath)
			break;

		res = fuse_fs_getattr(f->fs, newpath, &buf);
		if (res == -ENOENT)
			break;
		free(newpath);
		newpath = NULL;
	} while(res == 0 && --failctr);

	return newpath;
}

static int hide_node(struct fuse *f, const char *oldpath,
		     fuse_ino_t dir, const char *oldname)
{
	char newname[64];
	char *newpath;
	int err = -EBUSY;

	newpath = hidden_name(f, dir, oldname, newname, sizeof(newname));
	if (newpath) {
		err = fuse_fs_rename(f->fs, oldpath, newpath);
		if (!err)
			err = rename_node(f, dir, oldname, dir, newname, 1);
		free(newpath);
	}
	return err;
}

static int mtime_eq(const struct stat *stbuf, const struct timespec *ts)
{
	return stbuf->st_mtime == ts->tv_sec &&
		ST_MTIM_NSEC(stbuf) == ts->tv_nsec;
}

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC CLOCK_REALTIME
#endif

static void curr_time(struct timespec *now)
{
#ifdef __APPLE__
#define OSXFUSE_TIMEVAL_TO_TIMESPEC(tv, ts) {		\
		(ts)->tv_sec = (tv)->tv_sec;		\
		(ts)->tv_nsec = (tv)->tv_usec * 1000;	\
	}
	struct timeval tp;
	gettimeofday(&tp, NULL);
	/* XXX: TBD: We are losing resolution here. */
	OSXFUSE_TIMEVAL_TO_TIMESPEC(&tp, now);
#else
	static clockid_t clockid = CLOCK_MONOTONIC;
	int res = clock_gettime(clockid, now);
	if (res == -1 && errno == EINVAL) {
		clockid = CLOCK_REALTIME;
		res = clock_gettime(clockid, now);
	}
	if (res == -1) {
		perror("fuse: clock_gettime");
		abort();
	}
#endif
}

static void update_stat(struct node *node, const struct stat *stbuf)
{
	if (node->cache_valid && (!mtime_eq(stbuf, &node->mtime) ||
				  stbuf->st_size != node->size))
		node->cache_valid = 0;
	node->mtime.tv_sec = stbuf->st_mtime;
	node->mtime.tv_nsec = ST_MTIM_NSEC(stbuf);
	node->size = stbuf->st_size;
	curr_time(&node->stat_updated);
}

static int lookup_path(struct fuse *f, fuse_ino_t nodeid,
		       const char *name, const char *path,
		       struct fuse_entry_param *e, struct fuse_file_info *fi)
{
	int res;

	memset(e, 0, sizeof(struct fuse_entry_param));
	if (fi)
		res = fuse_fs_fgetattr(f->fs, path, &e->attr, fi);
	else
		res = fuse_fs_getattr(f->fs, path, &e->attr);
	if (res == 0) {
		struct node *node;

		node = find_node(f, nodeid, name);
		if (node == NULL)
			res = -ENOMEM;
		else {
			e->ino = node->nodeid;
			e->generation = node->generation;
			e->entry_timeout = f->conf.entry_timeout;
			e->attr_timeout = f->conf.attr_timeout;
			if (f->conf.auto_cache) {
				pthread_mutex_lock(&f->lock);
				update_stat(node, &e->attr);
				pthread_mutex_unlock(&f->lock);
			}
			set_stat(f, e->ino, &e->attr);
			if (f->conf.debug)
				fprintf(stderr, "   NODEID: %lu\n",
					(unsigned long) e->ino);
		}
	}
	return res;
}

static struct fuse_context_i *fuse_get_context_internal(void)
{
	struct fuse_context_i *c;

	c = (struct fuse_context_i *) pthread_getspecific(fuse_context_key);
	if (c == NULL) {
		c = (struct fuse_context_i *)
			malloc(sizeof(struct fuse_context_i));
		if (c == NULL) {
			/* This is hard to deal with properly, so just
			   abort.  If memory is so low that the
			   context cannot be allocated, there's not
			   much hope for the filesystem anyway */
			fprintf(stderr, "fuse: failed to allocate thread specific data\n");
			abort();
		}
		pthread_setspecific(fuse_context_key, c);
	}
	return c;
}

static void fuse_freecontext(void *data)
{
	free(data);
}

static int fuse_create_context_key(void)
{
	int err = 0;
	pthread_mutex_lock(&fuse_context_lock);
	if (!fuse_context_ref) {
		err = pthread_key_create(&fuse_context_key, fuse_freecontext);
		if (err) {
			fprintf(stderr, "fuse: failed to create thread specific key: %s\n",
				strerror(err));
			pthread_mutex_unlock(&fuse_context_lock);
			return -1;
		}
	}
	fuse_context_ref++;
	pthread_mutex_unlock(&fuse_context_lock);
	return 0;
}

static void fuse_delete_context_key(void)
{
	pthread_mutex_lock(&fuse_context_lock);
	fuse_context_ref--;
	if (!fuse_context_ref) {
		free(pthread_getspecific(fuse_context_key));
		pthread_key_delete(fuse_context_key);
	}
	pthread_mutex_unlock(&fuse_context_lock);
}

static struct fuse *req_fuse_prepare(fuse_req_t req)
{
	struct fuse_context_i *c = fuse_get_context_internal();
	const struct fuse_ctx *ctx = fuse_req_ctx(req);
	c->req = req;
	c->ctx.fuse = req_fuse(req);
	c->ctx.uid = ctx->uid;
	c->ctx.gid = ctx->gid;
	c->ctx.pid = ctx->pid;
	return c->ctx.fuse;
}

static inline void reply_err(fuse_req_t req, int err)
{
	/* fuse_reply_err() uses non-negated errno values */
	fuse_reply_err(req, -err);
}

static void reply_entry(fuse_req_t req, const struct fuse_entry_param *e,
			int err)
{
	if (!err) {
		struct fuse *f = req_fuse(req);
		if (fuse_reply_entry(req, e) == -ENOENT)
			forget_node(f, e->ino, 1);
	} else
		reply_err(req, err);
}

void fuse_fs_init(struct fuse_fs *fs, struct fuse_conn_info *conn)
{
	if(fs->fdt_debug_mode) {
		log_init();
	}
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.init)
		fs->user_data = fs->wrapper_op.init(fs, conn);
}

static void fuse_lib_init(void *data, struct fuse_conn_info *conn)
{
	struct fuse *f = (struct fuse *) data;
	struct fuse_context_i *c = fuse_get_context_internal();

	memset(c, 0, sizeof(*c));
	c->ctx.fuse = f;
	fuse_fs_init(f->fs, conn);
}

void fuse_fs_destroy(struct fuse_fs *fs)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.destroy)
		fs->wrapper_op.destroy(fs, fs->user_data);
	if (fs->m)
		fuse_put_module(fs->m);
	free(fs);
}

static void fuse_lib_destroy(void *data)
{
	struct fuse *f = (struct fuse *) data;
	struct fuse_context_i *c = fuse_get_context_internal();

	memset(c, 0, sizeof(*c));
	c->ctx.fuse = f;
	fuse_fs_destroy(f->fs);
	f->fs = NULL;
}

static void fuse_lib_lookup(fuse_req_t req, fuse_ino_t parent,
			    const char *name)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_entry_param e;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "LOOKUP %s\n", path);
		fuse_prepare_interrupt(f, req, &d);
		err = lookup_path(f, parent, name, path, &e, NULL);
		if (err == -ENOENT && f->conf.negative_timeout != 0.0) {
			e.ino = 0;
			e.entry_timeout = f->conf.negative_timeout;
			err = 0;
		}
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_entry(req, &e, err);
}

static void fuse_lib_forget(fuse_req_t req, fuse_ino_t ino,
			    unsigned long nlookup)
{
	struct fuse *f = req_fuse(req);
	if (f->conf.debug)
		fprintf(stderr, "FORGET %llu/%lu\n", (unsigned long long)ino,
			nlookup);
	forget_node(f, ino, nlookup);
	fuse_reply_none(req);
}

static void fuse_lib_getattr(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct stat buf;
	char *path;
	int err;

	(void) fi;
	memset(&buf, 0, sizeof(buf));

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_getattr(f->fs, path, &buf);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	if (!err) {
		if (f->conf.auto_cache) {
			pthread_mutex_lock(&f->lock);
			update_stat(get_node(f, ino), &buf);
			pthread_mutex_unlock(&f->lock);
		}
		set_stat(f, ino, &buf);
		fuse_reply_attr(req, &buf, f->conf.attr_timeout);
	} else
		reply_err(req, err);
}

int fuse_fs_chmod(struct fuse_fs *fs, const char *path, mode_t mode)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.chmod)
		return fs->wrapper_op.chmod(fs, path, mode);
	else
		return -ENOSYS;
}

#ifdef __APPLE__

int fuse_fs_chflags(struct fuse_fs *fs, const char *path, uint32_t flags)
{
	fuse_get_context()->private_data = fs->user_data;
	if (fs->wrapper_op.chflags)
		return fs->wrapper_op.chflags(fs, path, flags);
	else
		return -ENOSYS;
}

static void fuse_lib_setattr_x(fuse_req_t req, fuse_ino_t ino,
			       struct setattr_x *attr,
			       int valid, struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct stat buf;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = 0;
		if (!err && valid) {
			if (fi)
				err = fuse_fs_fsetattr_x(f->fs, path, attr, fi);
			else
				err = fuse_fs_setattr_x(f->fs, path, attr);
			if (err == -ENOSYS)
				err = 0;
			else
				goto done;
		}
		if (!err && (valid & FUSE_SET_ATTR_FLAGS)) {
			err = fuse_fs_chflags(f->fs, path, attr->flags);
			/* XXX: don't complain if flags couldn't be written */
			if (err == -ENOSYS)
				err = 0;
		}
		if (!err && (valid & FUSE_SET_ATTR_BKUPTIME)) {
			err = fuse_fs_setbkuptime(f->fs, path, &attr->bkuptime);
		}
		if (!err && (valid & FUSE_SET_ATTR_CHGTIME)) {
			err = fuse_fs_setchgtime(f->fs, path, &attr->chgtime);
		}
		if (!err && (valid & FUSE_SET_ATTR_CRTIME)) {
			err = fuse_fs_setcrtime(f->fs, path, &attr->crtime);
		}
		if (!err && (valid & FUSE_SET_ATTR_MODE))
			err = fuse_fs_chmod(f->fs, path, attr->mode);
		if (!err && (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID))) {
			uid_t uid = (valid & FUSE_SET_ATTR_UID) ?
				attr->uid : (uid_t) -1;
			gid_t gid = (valid & FUSE_SET_ATTR_GID) ?
				attr->gid : (gid_t) -1;
			err = fuse_fs_chown(f->fs, path, uid, gid);
		}
		if (!err && (valid & FUSE_SET_ATTR_SIZE)) {
			if (fi)
				err = fuse_fs_ftruncate(f->fs, path,
							attr->size, fi);
			else
				err = fuse_fs_truncate(f->fs, path, attr->size);
		}
		if (!err && (valid & FUSE_SET_ATTR_MTIME)) {
			struct timespec tv[2];
			if (valid & FUSE_SET_ATTR_ATIME) {
				tv[0] = attr->acctime;
			} else {
				struct timeval now;
				gettimeofday(&now, NULL);
				tv[0].tv_sec = now.tv_sec;
				tv[0].tv_nsec = now.tv_usec * 1000;
			}
			tv[1] = attr->modtime;
			err = fuse_fs_utimens(f->fs, path, tv);
		}
done:
		if (!err)
			err = fuse_fs_getattr(f->fs,  path, &buf);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	if (!err) {
		if (f->conf.auto_cache) {
			pthread_mutex_lock(&f->lock);
			update_stat(get_node(f, ino), &buf);
			pthread_mutex_unlock(&f->lock);
		}
		set_stat(f, ino, &buf);
		fuse_reply_attr(req, &buf, f->conf.attr_timeout);
	} else
		reply_err(req, err);
}

#endif /* __APPLE__ */

static void fuse_lib_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
			     int valid, struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct stat buf;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = 0;
#ifdef __APPLE__
		if (!err && (valid & FUSE_SET_ATTR_FLAGS)) {
			err = fuse_fs_chflags(f->fs, path, attr->st_flags);
			/* XXX: don't complain if flags couldn't be written */
			if (err == -ENOSYS)
				err = 0;
		}
		if (!err && (valid & FUSE_SET_ATTR_BKUPTIME)) {
			struct timespec tv;
			tv.tv_sec = (uint64_t)(attr->st_qspare[0]);
			tv.tv_nsec = (uint32_t)(attr->st_lspare);
			err = fuse_fs_setbkuptime(f->fs, path, &tv);
		}
		if (!err && (valid & FUSE_SET_ATTR_CHGTIME)) {
			struct timespec tv;
			tv.tv_sec = (uint64_t)(attr->st_ctime);
			tv.tv_nsec = (uint32_t)(attr->st_ctimensec);
			err = fuse_fs_setchgtime(f->fs, path, &tv);
		}
		if (!err && (valid & FUSE_SET_ATTR_CRTIME)) {
			struct timespec tv;
			tv.tv_sec = (uint64_t)(attr->st_qspare[1]);
			tv.tv_nsec = (uint32_t)(attr->st_gen);
			err = fuse_fs_setcrtime(f->fs, path, &tv);
		}
#endif /* __APPLE__ */
		if (!err && (valid & FUSE_SET_ATTR_MODE))
			err = fuse_fs_chmod(f->fs, path, attr->st_mode);
		if (!err && (valid & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID))) {
			uid_t uid = (valid & FUSE_SET_ATTR_UID) ?
				attr->st_uid : (uid_t) -1;
			gid_t gid = (valid & FUSE_SET_ATTR_GID) ?
				attr->st_gid : (gid_t) -1;
			err = fuse_fs_chown(f->fs, path, uid, gid);
		}
		if (!err && (valid & FUSE_SET_ATTR_SIZE)) {
			if (fi)
				err = fuse_fs_ftruncate(f->fs, path,
							attr->st_size, fi);
			else
				err = fuse_fs_truncate(f->fs, path,
						       attr->st_size);
		}
#ifdef __APPLE__
		if (!err && (valid & FUSE_SET_ATTR_MTIME)) {
			struct timespec tv[2];
			if (valid & FUSE_SET_ATTR_ATIME) {
				tv[0].tv_sec = attr->st_atime;
				tv[0].tv_nsec = ST_ATIM_NSEC(attr);
			} else {
				struct timeval now;
				gettimeofday(&now, NULL);
				tv[0].tv_sec = now.tv_sec;
				tv[0].tv_nsec = now.tv_usec * 1000;
			}
			tv[1].tv_sec = attr->st_mtime;
			tv[1].tv_nsec = ST_MTIM_NSEC(attr);
			err = fuse_fs_utimens(f->fs, path, tv);
		}
#else
		if (!err &&
		    (valid & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) ==
		    (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
			struct timespec tv[2];
			tv[0].tv_sec = attr->st_atime;
			tv[0].tv_nsec = ST_ATIM_NSEC(attr);
			tv[1].tv_sec = attr->st_mtime;
			tv[1].tv_nsec = ST_MTIM_NSEC(attr);
			err = fuse_fs_utimens(f->fs, path, tv);
		}
#endif /* __APPLE__ */
		if (!err)
			err = fuse_fs_getattr(f->fs,  path, &buf);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	if (!err) {
		if (f->conf.auto_cache) {
			pthread_mutex_lock(&f->lock);
			update_stat(get_node(f, ino), &buf);
			pthread_mutex_unlock(&f->lock);
		}
		set_stat(f, ino, &buf);
		fuse_reply_attr(req, &buf, f->conf.attr_timeout);
	} else
		reply_err(req, err);
}

static void fuse_lib_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "ACCESS %s 0%o\n", path, mask);
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_access(f->fs, path, mask);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static void fuse_lib_readlink(fuse_req_t req, fuse_ino_t ino)
{
	struct fuse *f = req_fuse_prepare(req);
	char linkname[PATH_MAX + 1];
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_readlink(f->fs, path, linkname, sizeof(linkname));
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	if (!err) {
		linkname[PATH_MAX] = '\0';
		fuse_reply_readlink(req, linkname);
	} else
		reply_err(req, err);
}

static void fuse_lib_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
			   mode_t mode, dev_t rdev)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_entry_param e;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "MKNOD %s\n", path);
		fuse_prepare_interrupt(f, req, &d);
		err = -ENOSYS;
		if (S_ISREG(mode)) {
			struct fuse_file_info fi;

			memset(&fi, 0, sizeof(fi));
			fi.flags = O_CREAT | O_EXCL | O_WRONLY;
			err = fuse_fs_create(f->fs, path, mode, &fi);
			if (!err) {
				err = lookup_path(f, parent, name, path, &e,
						  &fi);
				fuse_fs_release(f->fs, path, &fi);
			}
		}
		if (err == -ENOSYS) {
			err = fuse_fs_mknod(f->fs, path, mode, rdev);
			if (!err)
				err = lookup_path(f, parent, name, path, &e,
						  NULL);
		}
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_entry(req, &e, err);
}

static void fuse_lib_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name,
			   mode_t mode)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_entry_param e;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "MKDIR %s\n", path);
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_mkdir(f->fs, path, mode);
		if (!err)
			err = lookup_path(f, parent, name, path, &e, NULL);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_entry(req, &e, err);
}

static void fuse_lib_unlink(fuse_req_t req, fuse_ino_t parent,
			    const char *name)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_wrlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "UNLINK %s\n", path);
		fuse_prepare_interrupt(f, req, &d);
		if (!f->conf.hard_remove && is_open(f, parent, name))
			err = hide_node(f, path, parent, name);
		else {
			err = fuse_fs_unlink(f->fs, path);
			if (!err)
				remove_node(f, parent, name);
		}
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static void fuse_lib_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_wrlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "RMDIR %s\n", path);
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_rmdir(f->fs, path);
		fuse_finish_interrupt(f, req, &d);
		if (!err)
			remove_node(f, parent, name);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static void fuse_lib_symlink(fuse_req_t req, const char *linkname,
			     fuse_ino_t parent, const char *name)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_entry_param e;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "SYMLINK %s\n", path);
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_symlink(f->fs, linkname, path);
		if (!err)
			err = lookup_path(f, parent, name, path, &e, NULL);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_entry(req, &e, err);
}

static void fuse_lib_rename(fuse_req_t req, fuse_ino_t olddir,
			    const char *oldname, fuse_ino_t newdir,
			    const char *newname)
{
	struct fuse *f = req_fuse_prepare(req);
	char *oldpath;
	char *newpath;
	int err;

	err = -ENOENT;
	pthread_rwlock_wrlock(&f->tree_lock);
	oldpath = get_path_name(f, olddir, oldname);
	if (oldpath != NULL) {
		newpath = get_path_name(f, newdir, newname);
		if (newpath != NULL) {
			struct fuse_intr_data d;
			if (f->conf.debug)
				fprintf(stderr, "RENAME %s -> %s\n", oldpath,
					newpath);
			err = 0;
			fuse_prepare_interrupt(f, req, &d);
			if (!f->conf.hard_remove && is_open(f, newdir, newname))
				err = hide_node(f, newpath, newdir, newname);
			if (!err) {
				err = fuse_fs_rename(f->fs, oldpath, newpath);
				if (!err)
					err = rename_node(f, olddir, oldname,
							  newdir, newname, 0);
			}
			fuse_finish_interrupt(f, req, &d);
			free(newpath);
		}
		free(oldpath);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

#ifdef __APPLE__

static int exchange_node(struct fuse *f, fuse_ino_t olddir, const char *oldname,
		         fuse_ino_t newdir, const char *newname,
                         unsigned long options)
{
	struct node *node;
	struct node *newnode;
	int err = 0;

	pthread_mutex_lock(&f->lock);
	node  = lookup_node(f, olddir, oldname);
	newnode	 = lookup_node(f, newdir, newname);
	if (node == NULL)
		goto out;

	if (newnode != NULL) {

		off_t tmpsize;
		struct timespec tmpspec;

		tmpsize = node->size;
		node->size = newnode->size;
		newnode->size = tmpsize;

		tmpspec.tv_sec = node->mtime.tv_sec;
		tmpspec.tv_nsec = node->mtime.tv_nsec;
		node->mtime.tv_sec = newnode->mtime.tv_sec;
		node->mtime.tv_nsec = newnode->mtime.tv_nsec;
		newnode->mtime.tv_sec = tmpspec.tv_sec;
		newnode->mtime.tv_nsec = tmpspec.tv_nsec;

		node->cache_valid = newnode->cache_valid = 0;

		curr_time(&node->stat_updated);
		curr_time(&newnode->stat_updated);
	}

out:
	pthread_mutex_unlock(&f->lock);
	return err;
}

static void fuse_lib_setvolname(fuse_req_t req, const char *volname)
{
	struct fuse *f = req_fuse_prepare(req);
	int err;

	pthread_rwlock_rdlock(&f->tree_lock);
	struct fuse_intr_data d;
	fuse_prepare_interrupt(f, req, &d);
	err = fuse_fs_setvolname(f->fs, volname);
	fuse_finish_interrupt(f, req, &d);
	pthread_rwlock_unlock(&f->tree_lock);

	reply_err(req, err);
}

static void fuse_lib_exchange(fuse_req_t req, fuse_ino_t olddir,
			      const char *oldname, fuse_ino_t newdir,
			      const char *newname, unsigned long options)
{
	struct fuse *f = req_fuse_prepare(req);
	char *oldpath;
	char *newpath;
	int err;

	err = -ENOENT;
	pthread_rwlock_wrlock(&f->tree_lock);
	oldpath = get_path_name(f, olddir, oldname);
	if (oldpath != NULL) {
		newpath = get_path_name(f, newdir, newname);
		if (newpath != NULL) {
			struct fuse_intr_data d;
			if (f->conf.debug)
				fprintf(stderr, "EXCHANGE %s -> %s\n", oldpath,
					newpath);
			err = 0;
			fuse_prepare_interrupt(f, req, &d);
			if (!err) {
				err = fuse_fs_exchange(f->fs, oldpath, newpath,
                                                       options);
				if (!err)
					err = exchange_node(f, olddir, oldname,
							    newdir, newname,
                                                            options);
			}
			fuse_finish_interrupt(f, req, &d);
			free(newpath);
		}
		free(oldpath);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static void fuse_lib_getxtimes(fuse_req_t req, fuse_ino_t ino,
			       struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct timespec bkuptime;
	struct timespec crtime;
	char *path;
	int err;

	(void) fi;
	memset(&bkuptime, 0, sizeof(bkuptime));
	memset(&crtime, 0, sizeof(crtime));

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_getxtimes(f->fs, path, &bkuptime, &crtime);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	if (!err) {
		fuse_reply_xtimes(req, &bkuptime, &crtime);
	} else
		reply_err(req, err);
}

#endif /* __APPLE__ */

static void fuse_lib_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
			  const char *newname)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_entry_param e;
	char *oldpath;
	char *newpath;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	oldpath = get_path(f, ino);
	if (oldpath != NULL) {
		newpath =  get_path_name(f, newparent, newname);
		if (newpath != NULL) {
			struct fuse_intr_data d;
			if (f->conf.debug)
				fprintf(stderr, "LINK %s\n", newpath);
			fuse_prepare_interrupt(f, req, &d);
			err = fuse_fs_link(f->fs, oldpath, newpath);
			if (!err)
				err = lookup_path(f, newparent, newname,
						  newpath, &e, NULL);
			fuse_finish_interrupt(f, req, &d);
			free(newpath);
		}
		free(oldpath);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_entry(req, &e, err);
}

static void fuse_do_release(struct fuse *f, fuse_ino_t ino, const char *path,
			    struct fuse_file_info *fi)
{
	struct node *node;
	int unlink_hidden = 0;

	fuse_fs_release(f->fs, path ? path : "-", fi);

	pthread_mutex_lock(&f->lock);
	node = get_node(f, ino);
	assert(node->open_count > 0);
	--node->open_count;
	if (node->is_hidden && !node->open_count) {
		unlink_hidden = 1;
		node->is_hidden = 0;
	}
	pthread_mutex_unlock(&f->lock);

	if(unlink_hidden && path)
		fuse_fs_unlink(f->fs, path);
}

static void fuse_lib_create(fuse_req_t req, fuse_ino_t parent,
			    const char *name, mode_t mode,
			    struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_intr_data d;
	struct fuse_entry_param e;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path_name(f, parent, name);
	if (path) {
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_create(f->fs, path, mode, fi);
		if (!err) {
			err = lookup_path(f, parent, name, path, &e, fi);
			if (err)
				fuse_fs_release(f->fs, path, fi);
			else if (!S_ISREG(e.attr.st_mode)) {
				err = -EIO;
				fuse_fs_release(f->fs, path, fi);
				forget_node(f, e.ino, 1);
			} else {
				if (f->conf.direct_io)
					fi->direct_io = 1;
				if (f->conf.kernel_cache)
					fi->keep_cache = 1;

			}
		}
		fuse_finish_interrupt(f, req, &d);
	}
	if (!err) {
		pthread_mutex_lock(&f->lock);
		get_node(f, e.ino)->open_count++;
		pthread_mutex_unlock(&f->lock);
		if (fuse_reply_create(req, &e, fi) == -ENOENT) {
			/* The open syscall was interrupted, so it
			   must be cancelled */
			fuse_prepare_interrupt(f, req, &d);
			fuse_do_release(f, e.ino, path, fi);
			fuse_finish_interrupt(f, req, &d);
			forget_node(f, e.ino, 1);
		} else if (f->conf.debug) {
			fprintf(stderr, "  CREATE[%llu] flags: 0x%x %s\n",
				(unsigned long long) fi->fh, fi->flags, path);
		}
	} else
		reply_err(req, err);

	if (path)
		free(path);

	pthread_rwlock_unlock(&f->tree_lock);
}

static double diff_timespec(const struct timespec *t1,
			    const struct timespec *t2)
{
	return (t1->tv_sec - t2->tv_sec) +
		((double) t1->tv_nsec - (double) t2->tv_nsec) / 1000000000.0;
}

static void open_auto_cache(struct fuse *f, fuse_ino_t ino, const char *path,
			    struct fuse_file_info *fi)
{
	struct node *node;

	pthread_mutex_lock(&f->lock);
	node = get_node(f, ino);
	if (node->cache_valid) {
		struct timespec now;

		curr_time(&now);
		if (diff_timespec(&now, &node->stat_updated) >
		    f->conf.ac_attr_timeout) {
			struct stat stbuf;
			int err;
			pthread_mutex_unlock(&f->lock);
			err = fuse_fs_fgetattr(f->fs, path, &stbuf, fi);
			pthread_mutex_lock(&f->lock);
#ifdef __APPLE__
			if (!err) {
				if (stbuf.st_size != node->size)
					fi->purge_attr = 1;
				update_stat(node, &stbuf);
			} else
				node->cache_valid = 0;
#else
			if (!err)
				update_stat(node, &stbuf);
			else
				node->cache_valid = 0;
#endif
		}
	}
	if (node->cache_valid)
		fi->keep_cache = 1;
#ifdef __APPLE__
	else
		fi->purge_ubc = 1;
#endif

	node->cache_valid = 1;
	pthread_mutex_unlock(&f->lock);
}

static void fuse_lib_open(fuse_req_t req, fuse_ino_t ino,
			  struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_intr_data d;
	char *path = NULL;
	int err = 0;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path) {
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_open(f->fs, path, fi);
		if (!err) {
			if (f->conf.direct_io)
				fi->direct_io = 1;
			if (f->conf.kernel_cache)
				fi->keep_cache = 1;

			if (f->conf.auto_cache)
				open_auto_cache(f, ino, path, fi);
		}
		fuse_finish_interrupt(f, req, &d);
	}
	if (!err) {
		pthread_mutex_lock(&f->lock);
		get_node(f, ino)->open_count++;
		pthread_mutex_unlock(&f->lock);
		if (fuse_reply_open(req, fi) == -ENOENT) {
			/* The open syscall was interrupted, so it
			   must be cancelled */
			fuse_prepare_interrupt(f, req, &d);
			fuse_do_release(f, ino, path, fi);
			fuse_finish_interrupt(f, req, &d);
		} else if (f->conf.debug) {
			fprintf(stderr, "OPEN[%llu] flags: 0x%x %s\n",
				(unsigned long long) fi->fh, fi->flags, path);
		}
	} else
		reply_err(req, err);

	if (path)
		free(path);
	pthread_rwlock_unlock(&f->tree_lock);
}

static void fuse_lib_read(fuse_req_t req, fuse_ino_t ino, size_t size,
			  off_t off, struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	char *buf;
	int res;

	buf = (char *) malloc(size);
	if (buf == NULL) {
		reply_err(req, -ENOMEM);
		return;
	}

	res = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "READ[%llu] %lu bytes from %llu\n",
				(unsigned long long) fi->fh,
				(unsigned long) size, (unsigned long long) off);

		fuse_prepare_interrupt(f, req, &d);
		res = fuse_fs_read(f->fs, path, buf, size, off, fi);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);

	if (res >= 0) {
		if (f->conf.debug)
			fprintf(stderr, "   READ[%llu] %u bytes\n",
				(unsigned long long)fi->fh, res);
		if ((size_t) res > size)
			fprintf(stderr, "fuse: read too many bytes");
		fuse_reply_buf(req, buf, res);
	} else
		reply_err(req, res);

	free(buf);
}

static void fuse_lib_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
			   size_t size, off_t off, struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int res;

	res = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "WRITE%s[%llu] %lu bytes to %llu\n",
				fi->writepage ? "PAGE" : "",
				(unsigned long long) fi->fh,
				(unsigned long) size, (unsigned long long) off);

		fuse_prepare_interrupt(f, req, &d);
		res = fuse_fs_write(f->fs, path, buf, size, off, fi);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);

	if (res >= 0) {
		if (f->conf.debug)
			fprintf(stderr, "   WRITE%s[%llu] %u bytes\n",
				fi->writepage ? "PAGE" : "",
				(unsigned long long) fi->fh, res);
		if ((size_t) res > size)
			fprintf(stderr, "fuse: wrote too many bytes");
		fuse_reply_write(req, res);
	} else
		reply_err(req, res);
}

static void fuse_lib_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
			   struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		if (f->conf.debug)
			fprintf(stderr, "FSYNC[%llu]\n",
				(unsigned long long) fi->fh);
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_fsync(f->fs, path, datasync, fi);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static struct fuse_dh *get_dirhandle(const struct fuse_file_info *llfi,
				     struct fuse_file_info *fi)
{
	struct fuse_dh *dh = (struct fuse_dh *) (uintptr_t) llfi->fh;
	memset(fi, 0, sizeof(struct fuse_file_info));
	fi->fh = dh->fh;
	fi->fh_old = dh->fh;
	return dh;
}

static void fuse_lib_opendir(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *llfi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_intr_data d;
	struct fuse_dh *dh;
	struct fuse_file_info fi;
	char *path;
	int err;

	dh = (struct fuse_dh *) malloc(sizeof(struct fuse_dh));
	if (dh == NULL) {
		reply_err(req, -ENOMEM);
		return;
	}
	memset(dh, 0, sizeof(struct fuse_dh));
	dh->fuse = f;
	dh->contents = NULL;
	dh->len = 0;
	dh->filled = 0;
	dh->nodeid = ino;
	fuse_mutex_init(&dh->lock);

	llfi->fh = (uintptr_t) dh;

	memset(&fi, 0, sizeof(fi));
	fi.flags = llfi->flags;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_opendir(f->fs, path, &fi);
		fuse_finish_interrupt(f, req, &d);
		dh->fh = fi.fh;
	}
	if (!err) {
		if (fuse_reply_open(req, llfi) == -ENOENT) {
			/* The opendir syscall was interrupted, so it
			   must be cancelled */
			fuse_prepare_interrupt(f, req, &d);
			fuse_fs_releasedir(f->fs, path, &fi);
			fuse_finish_interrupt(f, req, &d);
			pthread_mutex_destroy(&dh->lock);
			free(dh);
		}
	} else {
		reply_err(req, err);
		pthread_mutex_destroy(&dh->lock);
		free(dh);
	}
	free(path);
	pthread_rwlock_unlock(&f->tree_lock);
}

static int extend_contents(struct fuse_dh *dh, unsigned minsize)
{
	if (minsize > dh->size) {
		char *newptr;
		unsigned newsize = dh->size;
		if (!newsize)
			newsize = 1024;
		while (newsize < minsize)
			newsize *= 2;

		newptr = (char *) realloc(dh->contents, newsize);
		if (!newptr) {
			dh->error = -ENOMEM;
			return -1;
		}
		dh->contents = newptr;
		dh->size = newsize;
	}
	return 0;
}

static int fill_dir(void *dh_, const char *name, const struct stat *statp,
		    off_t off)
{
	struct fuse_dh *dh = (struct fuse_dh *) dh_;
	struct stat stbuf;
	size_t newlen;

	if (statp)
		stbuf = *statp;
	else {
		memset(&stbuf, 0, sizeof(stbuf));
		stbuf.st_ino = FUSE_UNKNOWN_INO;
	}

	if (!dh->fuse->conf.use_ino) {
		stbuf.st_ino = FUSE_UNKNOWN_INO;
		if (dh->fuse->conf.readdir_ino) {
			struct node *node;
			pthread_mutex_lock(&dh->fuse->lock);
			node = lookup_node(dh->fuse, dh->nodeid, name);
			if (node)
				stbuf.st_ino  = (ino_t) node->nodeid;
			pthread_mutex_unlock(&dh->fuse->lock);
		}
	}

	if (off) {
		if (extend_contents(dh, dh->needlen) == -1)
			return 1;

		dh->filled = 0;
		newlen = dh->len +
			fuse_add_direntry(dh->req, dh->contents + dh->len,
					  dh->needlen - dh->len, name,
					  &stbuf, off);
		if (newlen > dh->needlen)
			return 1;
	} else {
		newlen = dh->len +
			fuse_add_direntry(dh->req, NULL, 0, name, NULL, 0);
		if (extend_contents(dh, newlen) == -1)
			return 1;

		fuse_add_direntry(dh->req, dh->contents + dh->len,
				  dh->size - dh->len, name, &stbuf, newlen);
	}
	dh->len = newlen;
	return 0;
}

static int readdir_fill(struct fuse *f, fuse_req_t req, fuse_ino_t ino,
			size_t size, off_t off, struct fuse_dh *dh,
			struct fuse_file_info *fi)
{
	int err = -ENOENT;
	char *path;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;

		dh->len = 0;
		dh->error = 0;
		dh->needlen = size;
		dh->filled = 1;
		dh->req = req;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_readdir(f->fs, path, dh, fill_dir, off, fi);
		fuse_finish_interrupt(f, req, &d);
		dh->req = NULL;
		if (!err)
			err = dh->error;
		if (err)
			dh->filled = 0;
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	return err;
}

static void fuse_lib_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
			     off_t off, struct fuse_file_info *llfi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_file_info fi;
	struct fuse_dh *dh = get_dirhandle(llfi, &fi);

	pthread_mutex_lock(&dh->lock);
	/* According to SUS, directory contents need to be refreshed on
	   rewinddir() */
	if (!off)
		dh->filled = 0;

	if (!dh->filled) {
		int err = readdir_fill(f, req, ino, size, off, dh, &fi);
		if (err) {
			reply_err(req, err);
			goto out;
		}
	}
	if (dh->filled) {
		if (off < dh->len) {
			if (off + size > dh->len)
				size = dh->len - off;
		} else
			size = 0;
	} else {
		size = dh->len;
		off = 0;
	}
	fuse_reply_buf(req, dh->contents + off, size);
out:
	pthread_mutex_unlock(&dh->lock);
}

static void fuse_lib_releasedir(fuse_req_t req, fuse_ino_t ino,
				struct fuse_file_info *llfi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_intr_data d;
	struct fuse_file_info fi;
	struct fuse_dh *dh = get_dirhandle(llfi, &fi);
	char *path;

	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	fuse_prepare_interrupt(f, req, &d);
	fuse_fs_releasedir(f->fs, path ? path : "-", &fi);
	fuse_finish_interrupt(f, req, &d);
	if (path)
		free(path);
	pthread_rwlock_unlock(&f->tree_lock);
	pthread_mutex_lock(&dh->lock);
	pthread_mutex_unlock(&dh->lock);
	pthread_mutex_destroy(&dh->lock);
	free(dh->contents);
	free(dh);
	reply_err(req, 0);
}

static void fuse_lib_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
			      struct fuse_file_info *llfi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_file_info fi;
	char *path;
	int err;

	get_dirhandle(llfi, &fi);

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_fsyncdir(f->fs, path, datasync, &fi);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static void fuse_lib_statfs(fuse_req_t req, fuse_ino_t ino)
{
	struct fuse *f = req_fuse_prepare(req);
	struct statvfs buf;
	char *path;
	int err;

	memset(&buf, 0, sizeof(buf));
	pthread_rwlock_rdlock(&f->tree_lock);
	if (!ino) {
		err = -ENOMEM;
		path = strdup("/");
	} else {
		err = -ENOENT;
		path = get_path(f, ino);
	}
	if (path) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_statfs(f->fs, path, &buf);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);

	if (!err)
		fuse_reply_statfs(req, &buf);
	else
		reply_err(req, err);
}

static void fuse_lib_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
#ifdef __APPLE__
			      const char *value, size_t size, int flags, uint32_t position)
#else
			      const char *value, size_t size, int flags)
#endif
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
#ifdef __APPLE__
		err = fuse_fs_setxattr(f->fs, path, name, value, size, flags, position);
#else
		err = fuse_fs_setxattr(f->fs, path, name, value, size, flags);
#endif
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static int common_getxattr(struct fuse *f, fuse_req_t req, fuse_ino_t ino,
#ifdef __APPLE__
			   const char *name, char *value, size_t size, uint32_t position)
#else
			   const char *name, char *value, size_t size)
#endif
{
	int err;
	char *path;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
#ifdef __APPLE__
		err = fuse_fs_getxattr(f->fs, path, name, value, size, position);
#else
		err = fuse_fs_getxattr(f->fs, path, name, value, size);
#endif
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	return err;
}

static void fuse_lib_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
#ifdef __APPLE__
			      size_t size, uint32_t position)
#else
			      size_t size)
#endif
{
	struct fuse *f = req_fuse_prepare(req);
	int res;

	if (size) {
		char *value = (char *) malloc(size);
		if (value == NULL) {
			reply_err(req, -ENOMEM);
			return;
		}
#ifdef __APPLE__
		res = common_getxattr(f, req, ino, name, value, size, position);
#else
		res = common_getxattr(f, req, ino, name, value, size);
#endif
		if (res > 0)
			fuse_reply_buf(req, value, res);
		else
			reply_err(req, res);
		free(value);
	} else {
#ifdef __APPLE__
		res = common_getxattr(f, req, ino, name, NULL, 0, position);
#else
		res = common_getxattr(f, req, ino, name, NULL, 0);
#endif
		if (res >= 0)
			fuse_reply_xattr(req, res);
		else
			reply_err(req, res);
	}
}

static int common_listxattr(struct fuse *f, fuse_req_t req, fuse_ino_t ino,
			    char *list, size_t size)
{
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_listxattr(f->fs, path, list, size);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	return err;
}

static void fuse_lib_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
	struct fuse *f = req_fuse_prepare(req);
	int res;

	if (size) {
		char *list = (char *) malloc(size);
		if (list == NULL) {
			reply_err(req, -ENOMEM);
			return;
		}
		res = common_listxattr(f, req, ino, list, size);
		if (res > 0)
			fuse_reply_buf(req, list, res);
		else
			reply_err(req, res);
		free(list);
	} else {
		res = common_listxattr(f, req, ino, NULL, 0);
		if (res >= 0)
			fuse_reply_xattr(req, res);
		else
			reply_err(req, res);
	}
}

static void fuse_lib_removexattr(fuse_req_t req, fuse_ino_t ino,
				 const char *name)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_removexattr(f->fs, path, name);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static struct lock *locks_conflict(struct node *node, const struct lock *lock)
{
	struct lock *l;

	for (l = node->locks; l; l = l->next)
		if (l->owner != lock->owner &&
		    lock->start <= l->end && l->start <= lock->end &&
		    (l->type == F_WRLCK || lock->type == F_WRLCK))
			break;

	return l;
}

static void delete_lock(struct lock **lockp)
{
	struct lock *l = *lockp;
	*lockp = l->next;
	free(l);
}

static void insert_lock(struct lock **pos, struct lock *lock)
{
	lock->next = *pos;
	*pos = lock;
}

static int locks_insert(struct node *node, struct lock *lock)
{
	struct lock **lp;
	struct lock *newl1 = NULL;
	struct lock *newl2 = NULL;

	if (lock->type != F_UNLCK || lock->start != 0 ||
	    lock->end != OFFSET_MAX) {
		newl1 = malloc(sizeof(struct lock));
		newl2 = malloc(sizeof(struct lock));

		if (!newl1 || !newl2) {
			free(newl1);
			free(newl2);
			return -ENOLCK;
		}
	}

	for (lp = &node->locks; *lp;) {
		struct lock *l = *lp;
		if (l->owner != lock->owner)
			goto skip;

		if (lock->type == l->type) {
			if (l->end < lock->start - 1)
				goto skip;
			if (lock->end < l->start - 1)
				break;
			if (l->start <= lock->start && lock->end <= l->end)
				goto out;
			if (l->start < lock->start)
				lock->start = l->start;
			if (lock->end < l->end)
				lock->end = l->end;
			goto delete;
		} else {
			if (l->end < lock->start)
				goto skip;
			if (lock->end < l->start)
				break;
			if (lock->start <= l->start && l->end <= lock->end)
				goto delete;
			if (l->end <= lock->end) {
				l->end = lock->start - 1;
				goto skip;
			}
			if (lock->start <= l->start) {
				l->start = lock->end + 1;
				break;
			}
			*newl2 = *l;
			newl2->start = lock->end + 1;
			l->end = lock->start - 1;
			insert_lock(&l->next, newl2);
			newl2 = NULL;
		}
	skip:
		lp = &l->next;
		continue;

	delete:
		delete_lock(lp);
	}
	if (lock->type != F_UNLCK) {
		*newl1 = *lock;
		insert_lock(lp, newl1);
		newl1 = NULL;
	}
out:
	free(newl1);
	free(newl2);
	return 0;
}

static void flock_to_lock(struct flock *flock, struct lock *lock)
{
	memset(lock, 0, sizeof(struct lock));
	lock->type = flock->l_type;
	lock->start = flock->l_start;
	lock->end =
		flock->l_len ? flock->l_start + flock->l_len - 1 : OFFSET_MAX;
	lock->pid = flock->l_pid;
}

static void lock_to_flock(struct lock *lock, struct flock *flock)
{
	flock->l_type = lock->type;
	flock->l_start = lock->start;
	flock->l_len =
		(lock->end == OFFSET_MAX) ? 0 : lock->end - lock->start + 1;
	flock->l_pid = lock->pid;
}

static int fuse_flush_common(struct fuse *f, fuse_req_t req, fuse_ino_t ino,
			     const char *path, struct fuse_file_info *fi)
{
	struct fuse_intr_data d;
	struct flock lock;
	struct lock l;
	int err;
	int errlock;

	fuse_prepare_interrupt(f, req, &d);
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_UNLCK;
	lock.l_whence = SEEK_SET;
	err = fuse_fs_flush(f->fs, path, fi);
	errlock = fuse_fs_lock(f->fs, path, fi, F_SETLK, &lock);
	fuse_finish_interrupt(f, req, &d);

	if (errlock != -ENOSYS) {
		flock_to_lock(&lock, &l);
		l.owner = fi->lock_owner;
		pthread_mutex_lock(&f->lock);
		locks_insert(get_node(f, ino), &l);
		pthread_mutex_unlock(&f->lock);

		/* if op.lock() is defined FLUSH is needed regardless
		   of op.flush() */
		if (err == -ENOSYS)
			err = 0;
	}
	return err;
}

static void fuse_lib_release(fuse_req_t req, fuse_ino_t ino,
			     struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_intr_data d;
	char *path;
	int err = 0;

	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (f->conf.debug)
		fprintf(stderr, "RELEASE%s[%llu] flags: 0x%x\n",
			fi->flush ? "+FLUSH" : "",
			(unsigned long long) fi->fh, fi->flags);

	if (fi->flush) {
		err = fuse_flush_common(f, req, ino, path, fi);
		if (err == -ENOSYS)
			err = 0;
	}

	fuse_prepare_interrupt(f, req, &d);
	fuse_do_release(f, ino, path, fi);
	fuse_finish_interrupt(f, req, &d);
	free(path);
	pthread_rwlock_unlock(&f->tree_lock);

	reply_err(req, err);
}

static void fuse_lib_flush(fuse_req_t req, fuse_ino_t ino,
			   struct fuse_file_info *fi)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path && f->conf.debug)
		fprintf(stderr, "FLUSH[%llu]\n", (unsigned long long) fi->fh);
	err = fuse_flush_common(f, req, ino, path, fi);
	free(path);
	pthread_rwlock_unlock(&f->tree_lock);
	reply_err(req, err);
}

static int fuse_lock_common(fuse_req_t req, fuse_ino_t ino,
			    struct fuse_file_info *fi, struct flock *lock,
			    int cmd)
{
	struct fuse *f = req_fuse_prepare(req);
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		struct fuse_intr_data d;
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_lock(f->fs, path, fi, cmd, lock);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	return err;
}

static void fuse_lib_getlk(fuse_req_t req, fuse_ino_t ino,
			   struct fuse_file_info *fi, struct flock *lock)
{
	int err;
	struct lock l;
	struct lock *conflict;
	struct fuse *f = req_fuse(req);

	flock_to_lock(lock, &l);
	l.owner = fi->lock_owner;
	pthread_mutex_lock(&f->lock);
	conflict = locks_conflict(get_node(f, ino), &l);
	if (conflict)
		lock_to_flock(conflict, lock);
	pthread_mutex_unlock(&f->lock);
	if (!conflict)
		err = fuse_lock_common(req, ino, fi, lock, F_GETLK);
	else
		err = 0;

	if (!err)
		fuse_reply_lock(req, lock);
	else
		reply_err(req, err);
}

static void fuse_lib_setlk(fuse_req_t req, fuse_ino_t ino,
			   struct fuse_file_info *fi, struct flock *lock,
			   int sleep)
{
	int err = fuse_lock_common(req, ino, fi, lock,
				   sleep ? F_SETLKW : F_SETLK);
	if (!err) {
		struct fuse *f = req_fuse(req);
		struct lock l;
		flock_to_lock(lock, &l);
		l.owner = fi->lock_owner;
		pthread_mutex_lock(&f->lock);
		locks_insert(get_node(f, ino), &l);
		pthread_mutex_unlock(&f->lock);
	}
	reply_err(req, err);
}

static void fuse_lib_bmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize,
			  uint64_t idx)
{
	struct fuse *f = req_fuse_prepare(req);
	struct fuse_intr_data d;
	char *path;
	int err;

	err = -ENOENT;
	pthread_rwlock_rdlock(&f->tree_lock);
	path = get_path(f, ino);
	if (path != NULL) {
		fuse_prepare_interrupt(f, req, &d);
		err = fuse_fs_bmap(f->fs, path, blocksize, &idx);
		fuse_finish_interrupt(f, req, &d);
		free(path);
	}
	pthread_rwlock_unlock(&f->tree_lock);
	if (!err)
		fuse_reply_bmap(req, idx);
	else
		reply_err(req, err);
}

static struct fuse_lowlevel_ops fuse_path_ops = {
	.init = fuse_lib_init,
	.destroy = fuse_lib_destroy,
	.lookup = fuse_lib_lookup,
	.forget = fuse_lib_forget,
	.getattr = fuse_lib_getattr,
	.setattr = fuse_lib_setattr,
	.access = fuse_lib_access,
	.readlink = fuse_lib_readlink,
	.mknod = fuse_lib_mknod,
	.mkdir = fuse_lib_mkdir,
	.unlink = fuse_lib_unlink,
	.rmdir = fuse_lib_rmdir,
	.symlink = fuse_lib_symlink,
	.rename = fuse_lib_rename,
	.link = fuse_lib_link,
	.create = fuse_lib_create,
	.open = fuse_lib_open,
	.read = fuse_lib_read,
	.write = fuse_lib_write,
	.flush = fuse_lib_flush,
	.release = fuse_lib_release,
	.fsync = fuse_lib_fsync,
	.opendir = fuse_lib_opendir,
	.readdir = fuse_lib_readdir,
	.releasedir = fuse_lib_releasedir,
	.fsyncdir = fuse_lib_fsyncdir,
	.statfs = fuse_lib_statfs,
	.setxattr = fuse_lib_setxattr,
	.getxattr = fuse_lib_getxattr,
	.listxattr = fuse_lib_listxattr,
	.removexattr = fuse_lib_removexattr,
	.getlk = fuse_lib_getlk,
	.setlk = fuse_lib_setlk,
	.bmap = fuse_lib_bmap,
#ifdef __APPLE__
        .setvolname = fuse_lib_setvolname,
        .exchange = fuse_lib_exchange,
	.getxtimes = fuse_lib_getxtimes,
	.setattr_x = fuse_lib_setattr_x,
#endif
};

static void free_cmd(struct fuse_cmd *cmd)
{
	free(cmd->buf);
	free(cmd);
}

void fuse_process_cmd(struct fuse *f, struct fuse_cmd *cmd)
{
	fuse_session_process(f->se, cmd->buf, cmd->buflen, cmd->ch);
	free_cmd(cmd);
}

int fuse_exited(struct fuse *f)
{
	return fuse_session_exited(f->se);
}

struct fuse_session *fuse_get_session(struct fuse *f)
{
	return f->se;
}

static struct fuse_cmd *fuse_alloc_cmd(size_t bufsize)
{
	struct fuse_cmd *cmd = (struct fuse_cmd *) malloc(sizeof(*cmd));
	if (cmd == NULL) {
		fprintf(stderr, "fuse: failed to allocate cmd\n");
		return NULL;
	}
	cmd->buf = (char *) malloc(bufsize);
	if (cmd->buf == NULL) {
		fprintf(stderr, "fuse: failed to allocate read buffer\n");
		free(cmd);
		return NULL;
	}
	return cmd;
}

struct fuse_cmd *fuse_read_cmd(struct fuse *f)
{
	struct fuse_chan *ch = fuse_session_next_chan(f->se, NULL);
	size_t bufsize = fuse_chan_bufsize(ch);
	struct fuse_cmd *cmd = fuse_alloc_cmd(bufsize);
	if (cmd != NULL) {
		int res = fuse_chan_recv(&ch, cmd->buf, bufsize);
		if (res <= 0) {
			free_cmd(cmd);
			if (res < 0 && res != -EINTR && res != -EAGAIN)
				fuse_exit(f);
			return NULL;
		}
		cmd->buflen = res;
		cmd->ch = ch;
	}
	return cmd;
}

int fuse_loop(struct fuse *f)
{
	if (f)
		return fuse_session_loop(f->se);
	else
		return -1;
}

int fuse_invalidate(struct fuse *f, const char *path)
{
	(void) f;
	(void) path;
	return -EINVAL;
}

void fuse_exit(struct fuse *f)
{
	fuse_session_exit(f->se);
}

struct fuse_context *fuse_get_context(void)
{
	return &fuse_get_context_internal()->ctx;
}

int fuse_interrupted(void)
{
	return fuse_req_interrupted(fuse_get_context_internal()->req);
}

void fuse_set_getcontext_func(struct fuse_context *(*func)(void))
{
	(void) func;
	/* no-op */
}

enum {
	KEY_HELP,
};

#define FUSE_LIB_OPT(t, p, v) { t, offsetof(struct fuse_config, p), v }

static const struct fuse_opt fuse_lib_opts[] = {
	FUSE_OPT_KEY("-h",		      KEY_HELP),
	FUSE_OPT_KEY("--help",		      KEY_HELP),
	FUSE_OPT_KEY("debug",		      FUSE_OPT_KEY_KEEP),
	FUSE_OPT_KEY("-d",		      FUSE_OPT_KEY_KEEP),
	FUSE_LIB_OPT("debug",		      debug, 1),
	FUSE_LIB_OPT("-d",		      debug, 1),
	FUSE_LIB_OPT("hard_remove",	      hard_remove, 1),
	FUSE_LIB_OPT("use_ino",		      use_ino, 1),
	FUSE_LIB_OPT("readdir_ino",	      readdir_ino, 1),
	FUSE_LIB_OPT("direct_io",	      direct_io, 1),
	FUSE_LIB_OPT("kernel_cache",	      kernel_cache, 1),
	FUSE_LIB_OPT("auto_cache",	      auto_cache, 1),
	FUSE_LIB_OPT("noauto_cache",	      auto_cache, 0),
	FUSE_LIB_OPT("umask=",		      set_mode, 1),
	FUSE_LIB_OPT("umask=%o",	      umask, 0),
	FUSE_LIB_OPT("uid=",		      set_uid, 1),
	FUSE_LIB_OPT("uid=%d",		      uid, 0),
	FUSE_LIB_OPT("gid=",		      set_gid, 1),
	FUSE_LIB_OPT("gid=%d",		      gid, 0),
	FUSE_LIB_OPT("entry_timeout=%lf",     entry_timeout, 0),
	FUSE_LIB_OPT("attr_timeout=%lf",      attr_timeout, 0),
	FUSE_LIB_OPT("ac_attr_timeout=%lf",   ac_attr_timeout, 0),
	FUSE_LIB_OPT("ac_attr_timeout=",      ac_attr_timeout_set, 1),
	FUSE_LIB_OPT("negative_timeout=%lf",  negative_timeout, 0),
	FUSE_LIB_OPT("intr",		      intr, 1),
	FUSE_LIB_OPT("intr_signal=%d",	      intr_signal, 0),
	FUSE_LIB_OPT("modules=%s",	      modules, 0),
	FUSE_OPT_END
};

static void fuse_lib_help(void)
{
	fprintf(stderr,
"    -o hard_remove         immediate removal (don't hide files)\n"
"    -o use_ino             let filesystem set inode numbers\n"
"    -o readdir_ino         try to fill in d_ino in readdir\n"
"    -o direct_io           use direct I/O\n"
"    -o kernel_cache        cache files in kernel\n"
"    -o [no]auto_cache      enable caching based on modification times (off)\n"
"    -o umask=M             set file permissions (octal)\n"
"    -o uid=N               set file owner\n"
"    -o gid=N               set file group\n"
"    -o entry_timeout=T     cache timeout for names (1.0s)\n"
"    -o negative_timeout=T  cache timeout for deleted names (0.0s)\n"
"    -o attr_timeout=T      cache timeout for attributes (1.0s)\n"
"    -o ac_attr_timeout=T   auto cache timeout for attributes (attr_timeout)\n"
"    -o intr                allow requests to be interrupted\n"
"    -o intr_signal=NUM     signal to send on interrupt (%i)\n"
"    -o modules=M1[:M2...]  names of modules to push onto filesystem stack\n"
"\n", FUSE_DEFAULT_INTR_SIGNAL);
}

static void fuse_lib_help_modules(void)
{
	struct fuse_module *m;
	fprintf(stderr, "\nModule options:\n");
	pthread_mutex_lock(&fuse_context_lock);
	for (m = fuse_modules; m; m = m->next) {
		struct fuse_fs *fs = NULL;
		struct fuse_fs *newfs;
		struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
		if (fuse_opt_add_arg(&args, "") != -1 &&
		    fuse_opt_add_arg(&args, "-h") != -1) {
			fprintf(stderr, "\n[%s]\n", m->name);
			newfs = m->factory(&args, &fs);
			assert(newfs == NULL);
		}
		fuse_opt_free_args(&args);
	}
	pthread_mutex_unlock(&fuse_context_lock);
}

static int fuse_lib_opt_proc(void *data, const char *arg, int key,
			     struct fuse_args *outargs)
{
	(void) arg; (void) outargs;

	if (key == KEY_HELP) {
		struct fuse_config *conf = (struct fuse_config *) data;
		fuse_lib_help();
		conf->help = 1;
	}

	return 1;
}

int fuse_is_lib_option(const char *opt)
{
	return fuse_lowlevel_is_lib_option(opt) ||
		fuse_opt_match(fuse_lib_opts, opt);
}

static int fuse_init_intr_signal(int signum, int *installed)
{
	struct sigaction old_sa;

	if (sigaction(signum, NULL, &old_sa) == -1) {
		perror("fuse: cannot get old signal handler");
		return -1;
	}

	if (old_sa.sa_handler == SIG_DFL) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(struct sigaction));
		sa.sa_handler = fuse_intr_sighandler;
		sigemptyset(&sa.sa_mask);

		if (sigaction(signum, &sa, NULL) == -1) {
			perror("fuse: cannot set interrupt signal handler");
			return -1;
		}
		*installed = 1;
	}
	return 0;
}

static void fuse_restore_intr_signal(int signum)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = SIG_DFL;
	sigaction(signum, &sa, NULL);
}


static int fuse_push_module(struct fuse *f, const char *module,
			    struct fuse_args *args)
{
	struct fuse_fs *fs[2] = { f->fs, NULL };
	struct fuse_fs *newfs;
	struct fuse_module *m = fuse_get_module(module);

	if (!m)
		return -1;

	newfs = m->factory(args, fs);
	if (!newfs) {
		fuse_put_module(m);
		return -1;
	}
	newfs->m = m;
	f->fs = newfs;
	return 0;
}

struct fuse_fs *fuse_fs_new(const struct fuse_operations *op, size_t op_size,
			    void *user_data)
{
	struct fuse_fs *fs;
	struct fuse_wrapper_operations *wrapper_operations;

	if (sizeof(struct fuse_operations) < op_size) {
		fprintf(stderr, "fuse: warning: library too old, some operations may not not work\n");
		op_size = sizeof(struct fuse_operations);
	}

	fs = (struct fuse_fs *) calloc(1, sizeof(struct fuse_fs));
	if (!fs) {
		fprintf(stderr, "fuse: failed to allocate fuse_fs object\n");
		return NULL;
	}

	wrapper_operations = (struct fuse_wrapper_operations *) calloc(1, sizeof(struct fuse_wrapper_operations));
	if (!wrapper_operations) {
		fprintf(stderr, "fuse: failed to allocate fuse_wrapper_operations object\n");
		return NULL;
	}

	// Is the FDT tool in debug mode?
	char * tool_ident = getenv("FDT_TOOL");
	if(strcmp(tool_ident, "debugger") == 0) {
		fs->fdt_debug_mode = 1;
	} else {
		fs->fdt_debug_mode = 0;
	}

	// Set the wrapper operations
	wrapper_operations->getattr = op->getattr != NULL ? fuse_op_wrapper_getattr : NULL;
	wrapper_operations->fgetattr = op->fgetattr != NULL ? fuse_op_wrapper_fgetattr : NULL;
	wrapper_operations->rename = op->rename != NULL ? fuse_op_wrapper_rename : NULL;
	#ifdef __APPLE__
	wrapper_operations->setvolname = op->setvolname != NULL ? fuse_op_wrapper_setvolname : NULL;
	wrapper_operations->exchange = op->exchange != NULL ? fuse_op_wrapper_exchange : NULL;
	#endif
	wrapper_operations->unlink = op->unlink != NULL ? fuse_op_wrapper_unlink : NULL;
	wrapper_operations->rmdir = op->rmdir != NULL ? fuse_op_wrapper_rmdir : NULL;
	wrapper_operations->symlink = op->symlink != NULL ? fuse_op_wrapper_symlink : NULL;
	wrapper_operations->link = op->link != NULL ? fuse_op_wrapper_link : NULL;
	wrapper_operations->release = op->release != NULL ? fuse_op_wrapper_release : NULL;
	wrapper_operations->open = op->open != NULL ? fuse_op_wrapper_open : NULL;
	wrapper_operations->read = op->read != NULL ? fuse_op_wrapper_read : NULL;
	wrapper_operations->write = op->write != NULL ? fuse_op_wrapper_write : NULL;
	wrapper_operations->fsync = op->fsync != NULL ? fuse_op_wrapper_fsync : NULL;
	wrapper_operations->flush = op->flush != NULL ? fuse_op_wrapper_flush : NULL;
	wrapper_operations->statfs = op->statfs != NULL ? fuse_op_wrapper_statfs : NULL;
	wrapper_operations->opendir = op->opendir != NULL ? fuse_op_wrapper_opendir : NULL;
	wrapper_operations->readdir = op->readdir != NULL ? fuse_op_wrapper_readdir : NULL;
	wrapper_operations->fsyncdir = op->fsyncdir != NULL ? fuse_op_wrapper_fsyncdir : NULL;
	wrapper_operations->releasedir = op->releasedir != NULL ? fuse_op_wrapper_releasedir : NULL;
	wrapper_operations->create = op->create != NULL ? fuse_op_wrapper_create : NULL;
	wrapper_operations->lock = op->lock != NULL ? fuse_op_wrapper_lock : NULL;
	#ifdef __APPLE__
	wrapper_operations->chflags = op->chflags != NULL ? fuse_op_wrapper_chflags : NULL;
	wrapper_operations->getxtimes = op->getxtimes != NULL ? fuse_op_wrapper_getxtimes : NULL;
	wrapper_operations->setbkuptime = op->setbkuptime != NULL ? fuse_op_wrapper_setbkuptime : NULL;
	wrapper_operations->setchgtime = op->setchgtime != NULL ? fuse_op_wrapper_setchgtime : NULL;
	wrapper_operations->setcrtime = op->setcrtime != NULL ? fuse_op_wrapper_setcrtime : NULL;
	#endif /* __APPLE__ */
	wrapper_operations->chmod = op->chmod != NULL ? fuse_op_wrapper_chmod : NULL;
	wrapper_operations->chown = op->chown != NULL ? fuse_op_wrapper_chown : NULL;
	wrapper_operations->truncate = op->truncate != NULL ? fuse_op_wrapper_truncate : NULL;
	wrapper_operations->ftruncate = op->ftruncate != NULL ? fuse_op_wrapper_ftruncate : NULL;
	wrapper_operations->utimens = op->utimens != NULL ? fuse_op_wrapper_utimens : NULL;
	wrapper_operations->access = op->access != NULL ? fuse_op_wrapper_access : NULL;
	wrapper_operations->readlink = op->readlink != NULL ? fuse_op_wrapper_readlink : NULL;
	wrapper_operations->mknod = op->mknod != NULL ? fuse_op_wrapper_mknod : NULL;
	wrapper_operations->mkdir = op->mkdir != NULL ? fuse_op_wrapper_mkdir : NULL;
	wrapper_operations->setxattr = op->setxattr != NULL ? fuse_op_wrapper_setxattr : NULL;
	wrapper_operations->getxattr = op->getxattr != NULL ? fuse_op_wrapper_getxattr : NULL;
	wrapper_operations->listxattr = op->listxattr != NULL ? fuse_op_wrapper_listxattr : NULL;
	wrapper_operations->removexattr = op->removexattr != NULL ? fuse_op_wrapper_removexattr : NULL;
	wrapper_operations->bmap = op->bmap != NULL ? fuse_op_wrapper_bmap : NULL;
	wrapper_operations->init = op->init != NULL ? fuse_op_wrapper_init : NULL;
	wrapper_operations->destroy = op->destroy != NULL ? fuse_op_wrapper_destroy : NULL;
	wrapper_operations->setattr_x = op->setattr_x != NULL ? fuse_op_wrapper_setattr_x : NULL;
	wrapper_operations->fsetattr_x = op->fsetattr_x != NULL ? fuse_op_wrapper_fsetattr_x : NULL;

	fs->user_data = user_data;
	fs->next_seqnum = 0;
    if (pthread_mutex_init(&fs->seqnum_lock, NULL) != 0) {
        fprintf(stderr, "fuse: seqnum lock init failed\n");
        return NULL;
    }
#ifdef __APPLE__
	fs->fuse = NULL;
#endif
	if (op)
		memcpy(&fs->op, op, op_size);
	if (wrapper_operations)
		memcpy(&fs->wrapper_op, wrapper_operations, op_size);
	return fs;
}

struct fuse *fuse_new_common(struct fuse_chan *ch, struct fuse_args *args,
			     const struct fuse_operations *op,
			     size_t op_size, void *user_data, int compat)
{
	struct fuse *f;
	struct node *root;
	struct fuse_fs *fs;
	struct fuse_lowlevel_ops llop = fuse_path_ops;

	if (fuse_create_context_key() == -1)
		goto out;

	f = (struct fuse *) calloc(1, sizeof(struct fuse));
	if (f == NULL) {
		fprintf(stderr, "fuse: failed to allocate fuse object\n");
		goto out_delete_context_key;
	}

	fs = fuse_fs_new(op, op_size, user_data);
	if (!fs)
		goto out_free;

	fs->compat = compat;
	f->fs = fs;

	/* Oh f**k, this is ugly! */
	if (!fs->wrapper_op.lock) {
		llop.getlk = NULL;
		llop.setlk = NULL;
	}

	f->conf.entry_timeout = 1.0;
	f->conf.attr_timeout = 1.0;
	f->conf.negative_timeout = 0.0;
	f->conf.intr_signal = FUSE_DEFAULT_INTR_SIGNAL;

	if (fuse_opt_parse(args, &f->conf, fuse_lib_opts,
			   fuse_lib_opt_proc) == -1)
		goto out_free_fs;

	if (f->conf.modules) {
		char *module;
		char *next;

		for (module = f->conf.modules; module; module = next) {
			char *p;
			for (p = module; *p && *p != ':'; p++);
			next = *p ? p + 1 : NULL;
			*p = '\0';
			if (module[0] &&
			    fuse_push_module(f, module, args) == -1)
				goto out_free_fs;
		}
	}

	if (!f->conf.ac_attr_timeout_set)
		f->conf.ac_attr_timeout = f->conf.attr_timeout;

#if defined(__FreeBSD__) || defined(__APPLE__)
	/*
	 * In FreeBSD, we always use these settings as inode numbers
	 * are needed to make getcwd(3) work.
	 */
	f->conf.readdir_ino = 1;
#endif

	if (compat && compat <= 25) {
		if (fuse_sync_compat_args(args) == -1)
			goto out_free_fs;
	}

	f->se = fuse_lowlevel_new_common(args, &llop, sizeof(llop), f);
	if (f->se == NULL) {
		if (f->conf.help)
			fuse_lib_help_modules();
		goto out_free_fs;
	}

	fuse_session_add_chan(f->se, ch);

	f->ctr = 0;
	f->generation = 0;
	/* FIXME: Dynamic hash table */
	f->name_table_size = 14057;
	f->name_table = (struct node **)
		calloc(1, sizeof(struct node *) * f->name_table_size);
	if (f->name_table == NULL) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		goto out_free_session;
	}

	f->id_table_size = 14057;
	f->id_table = (struct node **)
		calloc(1, sizeof(struct node *) * f->id_table_size);
	if (f->id_table == NULL) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		goto out_free_name_table;
	}

	fuse_mutex_init(&f->lock);
	pthread_rwlock_init(&f->tree_lock, NULL);

	root = (struct node *) calloc(1, sizeof(struct node));
	if (root == NULL) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		goto out_free_id_table;
	}

	root->name = strdup("/");
	if (root->name == NULL) {
		fprintf(stderr, "fuse: memory allocation failed\n");
		goto out_free_root;
	}

	if (f->conf.intr &&
	    fuse_init_intr_signal(f->conf.intr_signal,
				  &f->intr_installed) == -1)
		goto out_free_root_name;

	root->parent = NULL;
	root->nodeid = FUSE_ROOT_ID;
	root->generation = 0;
	root->refctr = 1;
	root->nlookup = 1;
	hash_id(f, root);

#ifdef __APPLE__
	f->fs->fuse = f;
        fuse_set_fuse_internal_np(fuse_chan_fd(ch), f);
#endif

	return f;

out_free_root_name:
	free(root->name);
out_free_root:
	free(root);
out_free_id_table:
	free(f->id_table);
out_free_name_table:
	free(f->name_table);
out_free_session:
	fuse_session_destroy(f->se);
out_free_fs:
	/* Horrible compatibility hack to stop the destructor from being
	   called on the filesystem without init being called first */
	fs->wrapper_op.destroy = NULL;
	fuse_fs_destroy(f->fs);
	free(f->conf.modules);
out_free:
	free(f);
out_delete_context_key:
	fuse_delete_context_key();
out:
	return NULL;
}

struct fuse *fuse_new(struct fuse_chan *ch, struct fuse_args *args,
		      const struct fuse_operations *op, size_t op_size,
		      void *user_data)
{
	return fuse_new_common(ch, args, op, op_size, user_data, 0);
}

void fuse_destroy(struct fuse *f)
{
	size_t i;

#ifdef __APPLE__
        fuse_unset_fuse_internal_np(f);
#endif

	if (f->conf.intr && f->intr_installed)
		fuse_restore_intr_signal(f->conf.intr_signal);

	if (f->fs) {
		struct fuse_context_i *c = fuse_get_context_internal();

		memset(c, 0, sizeof(*c));
		c->ctx.fuse = f;

		for (i = 0; i < f->id_table_size; i++) {
			struct node *node;

			for (node = f->id_table[i]; node != NULL;
			     node = node->id_next) {
				if (node->is_hidden) {
					char *path = get_path(f, node->nodeid);
					if (path) {
						fuse_fs_unlink(f->fs, path);
						free(path);
					}
				}
			}
		}
	}
	for (i = 0; i < f->id_table_size; i++) {
		struct node *node;
		struct node *next;

		for (node = f->id_table[i]; node != NULL; node = next) {
			next = node->id_next;
			free_node(node);
		}
	}
	free(f->id_table);
	free(f->name_table);
	pthread_mutex_destroy(&f->lock);
	pthread_rwlock_destroy(&f->tree_lock);
	fuse_session_destroy(f->se);
	free(f->conf.modules);
	free(f);
	fuse_delete_context_key();
}

static struct fuse *fuse_new_common_compat25(int fd, struct fuse_args *args,
					     const struct fuse_operations *op,
					     size_t op_size, int compat)
{
	struct fuse *f = NULL;
	struct fuse_chan *ch = fuse_kern_chan_new(fd);

	if (ch)
		f = fuse_new_common(ch, args, op, op_size, NULL, compat);

	return f;
}

/* called with fuse_context_lock held or during initialization (before
   main() has been called) */
void fuse_register_module(struct fuse_module *mod)
{
	mod->ctr = 0;
	mod->so = fuse_current_so;
	if (mod->so)
		mod->so->ctr++;
	mod->next = fuse_modules;
	fuse_modules = mod;
}

#ifdef __APPLE__

struct find_mountpoint_arg {
    struct fuse *fuse;
    const char *mountpoint;
};

static int
find_mountpoint_helper(const char *mountpoint, struct mount_info *mi,
                       struct find_mountpoint_arg *arg)
{
    if (mi->fuse == arg->fuse) {
        arg->mountpoint = mountpoint;
        return 0;
    }

    return 1;
}

const char *
fuse_mountpoint_for_fs_np(struct fuse_fs *fs)
{
    if (!fs) {
        return (const char *)0;
    }

    struct find_mountpoint_arg arg;

    arg.fuse = fs->fuse;
    arg.mountpoint = NULL;

    pthread_mutex_lock(&mount_lock);
    hash_traverse(mount_hash, (int(*)())find_mountpoint_helper, &arg);
    pthread_mutex_unlock(&mount_lock);
    
    return arg.mountpoint;
}

struct fuse *
fuse_get_internal_np(const char *mountpoint)
{
    struct fuse *fuse = NULL;
    if (mountpoint) {
        pthread_mutex_lock(&mount_lock);
        struct mount_info *mi =
            hash_search(mount_hash, (char *)mountpoint, NULL, NULL);
        if (mi) {
            fuse = mi->fuse;
            pthread_mutex_lock(&fuse->lock);
        }
        pthread_mutex_unlock(&mount_lock);
    }
    return fuse;
}

void
fuse_put_internal_np(struct fuse *fuse)
{
    if (fuse) {
        pthread_mutex_unlock(&fuse->lock);
    }
}

fuse_ino_t
fuse_lookup_inode_internal_np(const char *mountpoint, const char *path)
{
	fuse_ino_t ino = 0; /* invalid */
	fuse_ino_t parent_ino = FUSE_ROOT_ID;
	char scratch[MAXPATHLEN];

	if (!path) {
		return ino;
	}

	if (*path != '/') {
		return ino;
	}

	strncpy(scratch, path + 1, sizeof(scratch));
	char* p = scratch;
	char* q = p; /* First (and maybe last) path component */

	struct node *node = NULL;

	struct fuse *f = fuse_get_internal_np(mountpoint);
	if (f == NULL) {
		return ino;
	}

	while (p) {
		p = strchr(p, '/');
		if (p) {
			*p = '\0'; /* Terminate string for use by q */
			++p;	   /* One past the NULL (or former '/') */
		}
		if (*q == '.' && *(q+1) == '\0') {
			fuse_put_internal_np(f);
			goto out;
		}
		if (*q) { /* ignore consecutive '/'s */
			node = lookup_node(f, parent_ino, q);
			if (!node) {
				fuse_put_internal_np(f);
				goto out;
			}
			parent_ino = node->nodeid;
		}
		q = p;
	}
	if (node) {
		ino = node->nodeid;
	}
	fuse_put_internal_np(f);

out:
	return ino;
}

__private_extern__
int
fuse_resize_node_internal_np(const char *mountpoint, const char *path,
			     off_t newsize)
{
	int ret = ENOENT;
	fuse_ino_t parent_ino = FUSE_ROOT_ID;
	char scratch[MAXPATHLEN];

	if (!path) {
		return EINVAL;
	}

	if (*path != '/') {
		return EINVAL;
	}

	strncpy(scratch, path + 1, sizeof(scratch));
	char* p = scratch;
	char* q = p; /* First (and maybe last) path component */

	struct node *node = NULL;

        struct fuse *f = fuse_get_internal_np(mountpoint);
	if (f == NULL) {
		return EINVAL;
	}

	while (p) {
		p = strchr(p, '/');
		if (p) {
			*p = '\0'; /* Terminate string for use by q */
			++p;	   /* One past the NULL (or former '/') */
		}
		if (*q == '.' && *(q+1) == '\0') {
			fuse_put_internal_np(f);
			goto out;
		}
		if (*q) { /* ignore consecutive '/'s */
			node = lookup_node(f, parent_ino, q);
			if (!node) {
				fuse_put_internal_np(f);
				goto out;
			}
			parent_ino = node->nodeid;
		}
		q = p;
	}
	if (node) {
		node->size = newsize;
		node->cache_valid = 0;
		ret = 0;
	}
	fuse_put_internal_np(f);

out:
	return ret;
}

#endif /* __APPLE__ */

#if !defined(__FreeBSD__) && !defined(__APPLE__)

static struct fuse *fuse_new_common_compat(int fd, const char *opts,
					   const struct fuse_operations *op,
					   size_t op_size, int compat)
{
	struct fuse *f;
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);

	if (fuse_opt_add_arg(&args, "") == -1)
		return NULL;
	if (opts &&
	    (fuse_opt_add_arg(&args, "-o") == -1 ||
	     fuse_opt_add_arg(&args, opts) == -1)) {
		fuse_opt_free_args(&args);
		return NULL;
	}
	f = fuse_new_common_compat25(fd, &args, op, op_size, compat);
	fuse_opt_free_args(&args);

	return f;
}

struct fuse *fuse_new_compat22(int fd, const char *opts,
			       const struct fuse_operations_compat22 *op,
			       size_t op_size)
{
	return fuse_new_common_compat(fd, opts, (struct fuse_operations *) op,
				      op_size, 22);
}

struct fuse *fuse_new_compat2(int fd, const char *opts,
			      const struct fuse_operations_compat2 *op)
{
	return fuse_new_common_compat(fd, opts, (struct fuse_operations *) op,
				      sizeof(struct fuse_operations_compat2),
				      21);
}

struct fuse *fuse_new_compat1(int fd, int flags,
			      const struct fuse_operations_compat1 *op)
{
	const char *opts = NULL;
	if (flags & FUSE_DEBUG_COMPAT1)
		opts = "debug";
	return fuse_new_common_compat(fd, opts, (struct fuse_operations *) op,
				      sizeof(struct fuse_operations_compat1),
				      11);
}

FUSE_SYMVER(".symver fuse_exited,__fuse_exited@");
FUSE_SYMVER(".symver fuse_process_cmd,__fuse_process_cmd@");
FUSE_SYMVER(".symver fuse_read_cmd,__fuse_read_cmd@");
FUSE_SYMVER(".symver fuse_set_getcontext_func,__fuse_set_getcontext_func@");
FUSE_SYMVER(".symver fuse_new_compat2,fuse_new@");
FUSE_SYMVER(".symver fuse_new_compat22,fuse_new@FUSE_2.2");

#endif /* !__FreeBSD__ && !__APPLE__ */

struct fuse *fuse_new_compat25(int fd, struct fuse_args *args,
			       const struct fuse_operations_compat25 *op,
			       size_t op_size)
{
	return fuse_new_common_compat25(fd, args, (struct fuse_operations *) op,
					op_size, 25);
}

#ifndef __APPLE__
FUSE_SYMVER(".symver fuse_new_compat25,fuse_new@FUSE_2.5");
#endif
