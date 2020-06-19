/*
  FUSE: Filesystem in Userspace

  Copyright 2015 Peter Schlaile (peter at schlaile dot de)
  Copyright 2020 Andras Hrubak (andreas at uucp dot hu)
*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

static char src_dir[PATH_MAX];

struct chunk {
	struct chunk * next;

	int fd;
	off_t start_offset;
	size_t portion;
};

struct concat_file {
	struct concat_file * next;
	struct chunk * chunks;

	int fd;
	off_t fsize;
	int refcount;
};

static struct concat_file * open_files = 0;
static pthread_mutex_t  the_lock;

static void lock()
{
	pthread_mutex_lock(&the_lock);
}

static void unlock()
{
	pthread_mutex_unlock(&the_lock);
}

static struct concat_file * open_files_find(int fd)
{
	struct concat_file * cf;

	lock();

	for (cf = open_files; cf; cf = cf->next) {
		if (cf->fd == fd) {
			unlock();
			return cf;
		}
	}
	
	unlock();

	return 0;
}

static void open_files_push_front(struct concat_file * cf)
{
	lock();

	cf->next = open_files;
	open_files = cf;

	unlock();
}

static struct concat_file * open_files_erase(int fd)
{
	struct concat_file * rv = 0;
	struct concat_file * p;

	lock();

	if (open_files && open_files->fd == fd) {
		rv = open_files;
		open_files = rv->next;
	} else {
		for (p = open_files; p; p = p->next) {
			if (p->next && p->next->fd == fd) {
				break;
			}
		}

		if (p) {
			rv = p->next;
			p->next = p->next->next;
		}
	}

	if (rv) {
		rv->next = 0;
	}

	unlock();

	return rv;
}

static struct concat_file * open_concat_file(int fd, const char * path)
{
	struct concat_file * rv = 0;
	char bpath[PATH_MAX+1];
	char linebuf[PATH_MAX+1];
	char * fpath;
	char * base_dir;
	struct chunk * c = 0;
	
	FILE * fp;

	if (fd >= 0) {
		fp = fdopen(dup(fd), "r");
	} else {
		fp = fopen(path, "r");
	}

	if (!fp) {
		return 0;
	}

	rv = (struct concat_file *) calloc(sizeof(struct concat_file), 1);
	strncpy(bpath, path, sizeof(bpath));

	base_dir = dirname(bpath);

	linebuf[PATH_MAX] = 0;
	bpath[PATH_MAX] = 0;

	rv->fd = fd;
	rv->refcount = 1;

	while (fgets(linebuf, sizeof(linebuf), fp)) {
		char tpath[PATH_MAX];
		struct chunk * c_n;
		off_t start_offset, portion;

		linebuf[strlen(linebuf) - 1] = 0;
		
		if(sscanf(linebuf, "%llu %llu", &start_offset, &portion) != 2)
		{
			// TODO: die nicer
			abort();
		}
		/* Set fpath after to the 2nd space on the line */
		fpath = strchr(linebuf, ' ');
		if(fpath == NULL)
		{
			// TODO: die nicer
			abort();
		}
		fpath += 1;
		fpath = strchr(fpath, ' ');
		if(fpath == NULL)
		{
			// TODO: die nicer
			abort();
		}
		fpath += 1;
		/* fpath points to the target filename */

		rv->fsize += portion;

		if (fpath[0] == '/') {
			strncpy(tpath, fpath, sizeof(tpath));
		} else {
			snprintf(tpath, sizeof(tpath), "%s/%s",base_dir, fpath);
		}
		if (fd >= 0) {
			c_n = (struct chunk *) calloc(sizeof(struct chunk), 1);

			c_n->start_offset = start_offset;
			c_n->portion = portion;
			c_n->fd = open(tpath, O_RDONLY);

			if (c) {
				c->next = c_n;
			} else {
				rv->chunks = c_n;
			}
			c = c_n;
		}
	}
	fclose(fp);	
	return rv;
}


static void close_concat_file(struct concat_file * cf)
{
	struct chunk * c;

	if (!cf) {
		return;
	}

	for (c = cf->chunks; c;) {
		struct chunk * t;

		close(c->fd);
		
		t = c;

		c = c->next;

		free(t);
	}

	close(cf->fd);
	
	free(cf);
}

static off_t get_concat_file_size(const char * path)
{
	struct concat_file * c = open_concat_file(-1, path);
	off_t rv;

	if (!c) {
		return 0;
	}

	rv = c->fsize;

	close_concat_file(c);

	return rv;
}

static int read_concat_file(int fd, void *buf, size_t count, off_t offset)
{
	struct concat_file * cf = open_files_find(fd);
	struct chunk * c;
	ssize_t bytes_read = 0;
	off_t virtual_offset = 0;

	if (!cf) {
		return -EINVAL;
	}

	if (offset > cf->fsize) {
		return 0;
	}
	
	#ifdef DEBUG
	fprintf(stderr, "offset %lld, count %d, size %lld\n", offset, count, cf->fsize);
	#endif
	
	for (c = cf->chunks; c && count > 0; c = c->next) {
		#ifdef DEBUG
		fprintf(stderr, " virtual_offset %lld, portion %d, offset %lld, start_offset %lld, count %d, bytes_read %d\n", virtual_offset, c->portion, offset, c->start_offset, count, bytes_read);
		#endif
		
		if(virtual_offset + c->portion > offset)
		{
			off_t read_from_pos = c->start_offset + offset - virtual_offset;
			ssize_t read_this_much = count < c->portion ? count : c->portion;
			ssize_t bytes_read_now = pread(c->fd, buf, read_this_much, read_from_pos);
			
			#ifdef DEBUG
			fprintf(stderr, "  read_from_pos %lld, read_this_much %d, bytes_read_now %d\n", read_from_pos, read_this_much, bytes_read_now);
			#endif
			
			if (bytes_read_now < 0) return -errno;
			
			bytes_read += bytes_read_now;
			buf += bytes_read_now;
			offset += bytes_read_now;
			count -= bytes_read_now;
		}
		
		virtual_offset += c->portion;
	}
	
	return bytes_read;
}

static int concatfs_getattr(const char *path, struct stat *stbuf)
{
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	memset(stbuf, 0, sizeof(struct stat));

	if (lstat(fpath, stbuf) != 0)
		return -errno;
	
	stbuf->st_size = get_concat_file_size(fpath);

	return 0;
}

static int concatfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			    off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;
	DIR *dp;
	struct dirent *de;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	dp = opendir(fpath);

	if (!dp) {
		return -errno;
	}

	de = readdir(dp);
	if (de == 0) {
		closedir(dp);
		return -errno;
	}
	
	do {
		if (filler(buf, de->d_name, NULL, 0) != 0) {
			closedir(dp);
			return -ENOMEM;
		}
	} while ((de = readdir(dp)) != NULL);
	
	closedir(dp);

	return retstat;
}

static int concatfs_open(const char *path, struct fuse_file_info *fi)
{
	int fd;
	char fpath[PATH_MAX];
    
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);

	fd = open(fpath, fi->flags);

	if (fd < 0) {
		return -errno;
	}

	fi->fh = fd;

	open_files_push_front(open_concat_file(fd, fpath));

	return 0;
}

static int concatfs_release(const char * path, struct fuse_file_info * fi)
{
	close_concat_file(open_files_erase(fi->fh));

	return 0;
}

static int concatfs_read(const char *path, char *buf, size_t size, off_t offset,
			 struct fuse_file_info *fi)
{
	return read_concat_file(fi->fh, buf, size, offset);
}


static int concatfs_access(const char *path, int mask)
{
	int rv;
	char fpath[PATH_MAX];
   
	snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, path);
    
	rv = access(fpath, mask);
    
	if (rv < 0) {
		return -errno;
	}
    
	return rv;
}


static struct fuse_operations concatfs_oper = {
	.getattr	= concatfs_getattr,
	.open		= concatfs_open,
	.read		= concatfs_read,
	.release        = concatfs_release,
	.readdir	= concatfs_readdir,
	.access         = concatfs_access,
};

static void usage()
{
	fprintf(stderr, "Usage: concatfs <source-dir> <mountpoint> [<fuse-mount-options...>]\n");
	exit(-1);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		usage();
	}

	if ((getuid() == 0) || (geteuid() == 0)) {
		fprintf(stderr, 
			"WARNING! concatfs does *no* file access checking "
			"right now and therefore is *dangerous* to use "
			"as root!");
	}

	if (argv[1][0] == '/') {
		strncpy(src_dir, argv[1], sizeof(src_dir));
	} else {
		char cwd[PATH_MAX];

		getcwd(cwd, sizeof(cwd));

		snprintf(src_dir, sizeof(src_dir), "%s/%s",
			 cwd, argv[1]);
	}

	pthread_mutex_init(&the_lock, NULL);

	char ** argv_ = (char**) calloc(argc, sizeof(char*));

	argv_[0] = argv[0];

	memcpy(argv_ + 1, argv + 2, (argc - 2) * sizeof(char*));

	return fuse_main(argc - 1, argv_, &concatfs_oper, NULL);
}
