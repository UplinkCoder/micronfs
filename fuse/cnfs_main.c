/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/

/** @file
 *
 * minimal example filesystem using high-level API
 *
 * Compile with:
 *
 *     gcc -Wall hello.c `pkg-config fuse3 --cflags --libs` -o hello
 *
 * ## Source code ##
 * \include hello.c
 */

#ifndef FUSE_USE_VERSION
#  define FUSE_USE_VERSION 26
#endif

#ifndef _cplusplus
#  include <stdbool.h>
#endif
#define _GNU_SOURCE

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>

#include "../micronfs.h"
#include "../cache/cached_tree.h"

DEFN_PRINT_NAME_CACHE

#ifndef SOCKET
#  define SOCKET int
#endif

/*
 * Command line options
 *
 * We can't set default values for the char* fields here because
 * fuse_opt_parse would attempt to free() them when the user specifies
 * different values on the command line.
 */
static struct options {
	const char *filename;
	const char *contents;
	int show_help;
} options;

#define OPTION(t, p)                           \
    { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
	OPTION("--name=%s", filename),
	OPTION("--contents=%s", contents),
	OPTION("-h", show_help),
	OPTION("--help", show_help),
	FUSE_OPT_END
};

static void *cnfs_init(struct fuse_conn_info *conn)
{
	(void) conn;
//	cfg->kernel_cache = 1;
	return NULL;
}

static int logSize = 0;
char (*log_buffer)[65536] = 0;
static struct stat logStat;
static cache_t dirCache;

static void AddLog_(const char* msg, int len)
{
    len = (len ? len : strlen(msg));
    memcpy((*log_buffer) + logSize, msg, len);
    logSize +=  (len + 1);
    logStat.st_size = logSize;
    (*log_buffer)[logSize - 1] = '\n';
}
#define AddLog(STR, ...) \
    { \
        char fmt[512]; \
        int sz = sprintf(fmt, STR, __VA_ARGS__); \
        AddLog_(fmt, sz); \
    }

static int nfs_sock_fd = 0;

static int cnfs_getattr(const char *path, struct stat *stbuf)
{
    assert(path[0] == '/');
	int res = 0;
    //AddLog("getattr: path='%s'\n", path);
	memset(stbuf, 0, sizeof(struct stat));
	if (path[1] == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path+1, options.filename) == 0) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = strlen(options.contents);
	} else if (strcmp(path, "/log") == 0) {
        *stbuf = logStat;
    } else
    {
        meta_data_entry_t* entry =
            LookupPath(&dirCache, path, strlen(path));

        if (entry)
        {
            int isDir = entry->type == ENTRY_TYPE_DIRECTORY;
            if (isDir)
            {
        		stbuf->st_mode = S_IFDIR | 0755;
		        stbuf->st_nlink = 2 + entry->cached_dir->entries_size;
            }
            else
            {
        		stbuf->st_mode = S_IFREG | 0444;
                stbuf->st_size = entry->cached_file->size;
                stbuf->st_nlink = 1;
            }
        }
        else
        {
            res = -ENOENT;
        }
    }


	return res;
}

static int cnfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	(void) offset;
	(void) fi;

    // AddLog("readdir; path='%s', offset=%d", path, (int)offset);

	filler(buf, ".", NULL,  0);
	filler(buf, "..", NULL, 0);

    meta_data_entry_t* e =
        LookupPath(&dirCache, path, strlen(path));

    if (path[0] == '/' && path[1] == '\0')
    {
        filler(buf, "log", &logStat, 0);
        // filler(buf, options.filename, NULL,  0);
        e = dirCache.root;
    }
    else
    {
        if (e)
        {
            assert(e->type == ENTRY_TYPE_DIRECTORY);
        }
        else
        {
            return -ENOENT;
        }
    }


    meta_data_entry_t* onePastLast =
        e->cached_dir->entries + e->cached_dir->entries_size;

    for(meta_data_entry_t* ent = e->cached_dir->entries;
        ent < onePastLast; ent++)
    {
        struct stat s;
        if (ent->type == ENTRY_TYPE_FILE)
        {
            s.st_size = ent->cached_file->size;
            s.st_mode = S_IFREG;
        }
        else if (ent->type == ENTRY_TYPE_FILE)
        {
            s.st_mode = S_IFDIR;
        }
        filler(buf, toCharPtr(&dirCache, ent->name), &s, 0);
    }

	return 0;
}

static const char* EatAndCountDotDotPath(const char* path, int* levels_upP)
{
    char c;
    int levels_up = 0;

    while ((c = *path++))
    {
        if (c == '.' && *path++ == '.')
        {
            levels_up++;
            if (*path == '\\' || *path == '/')
                path++;
        }
    }

    *levels_upP = levels_up;

    return path;
}


static int cnfs_mkdir (const char *full_path, mode_t mode)
{
    // AddLog("mkdir: '%s'", full_path);
    mode |= S_IFDIR;

    lookup_parent_result_t p = LookupParent(&dirCache, full_path, strlen(full_path));

    if (!p.parentDir)
        return -ENOENT;

    if (!GetOrCreateSubdirectory(&dirCache,
        p.parentDir->cached_dir, p.entry_name, p.entry_name_length))
    {
        return -EEXIST;
    }

    return 0;
}

static int CheckAccess(meta_data_entry_t* entry, uint32_t accessFlags)
{
    // -EACCES
    return 0;
}

static int cnfs_open(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, "/log") == 0)
    {
        return 0;
    }
    const char* mPath = path + 1;
    meta_data_entry_t* entry = LookupPath(&dirCache, path, strlen(path));

    if (entry)
    {
        return CheckAccess(entry, (fi->flags & O_ACCMODE));
    }

	if (strcmp(path+1, options.filename) != 0)
    {
		return -ENOENT;
    }

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
    {
		return -EACCES;
    }

	return 0;
}
int nfs_write(int sock, const fhandle3*, const void* data, uint32_t size, uint64_t offset);

static int cnfs_write(const char *path, const char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    printf("Calling write\n");
    assert(offset == 0);
    meta_data_entry_t* e;
    e = LookupPath(&dirCache, path, strlen(path));
    if (e)
    {
        int isVirtual = e->flags & ENTRY_FLAG_VIRTUAL;
        if (!offset)
        {
            AddFile(&dirCache, path, buf, size, isVirtual);
        }
        else
        {
            UpdateFile(&dirCache, path, buf, size, offset, isVirtual);
        }

        if (!isVirtual)
        {
            fhandle3 handle = ptrToHandle(&dirCache, e->handle);
            nfs_write(nfs_sock_fd, &handle, (const void*)buf, size, offset);
        }

        return size;
    }
    else
    {
        return -ENOENT;
    }
}
fhandle3 nfs_create(SOCKET nfs_sock_fd, const fhandle3* parentDir, const char* filename, mode3 mode);

/*
    #define	__S_IFDIR	0040000	// Directory.
    #define	__S_IFCHR	0020000	// Character device.
    #define	__S_IFBLK	0060000	// Block device.
    #define	__S_IFREG	0100000	// Regular file.
    #define	__S_IFIFO	0010000	// FIFO.
    #define	__S_IFLNK	0120000	// Symbolic link.
    #define	__S_IFSOCK	0140000	// Socket.
*/

static int cnfs_mknod(const char * path, mode_t mode, dev_t dev)
{
    if (LookupPath(&dirCache, path, strlen(path)))
    {
        return -EEXIST;
    }

    if (!(mode & S_IFREG))
    {
        return -EINVAL;
    }

    lookup_parent_result_t result =
        LookupParent(&dirCache, path, strlen(path));

    meta_data_entry_t* file =
        LookupInDirectory(&dirCache, result.parentDir->cached_dir, result.entry_name, result.entry_name_length);

    if (file)
    {
        return -EEXIST;
    }

    // const char* parentPath = toCharPtr(&dirCache, result.parentDir->cached_dir->fullPath);
    // LookupPath(&dirCache, parentPath, strlen(parentPath));
    fhandle3 dirHandle = ptrToHandle(&dirCache, result.parentDir->handle);
    fhandle3 handle =
        nfs_create(nfs_sock_fd, &dirHandle, result.entry_name, mode & 0xFFFF);

    meta_data_entry_t* entry =
        CreateFileEntry(&dirCache, result.parentDir, result.entry_name, result.entry_name_length);
    entry->handle = handleToPtr(&dirCache, &handle);
    return 0;
}

int nfs_read(int sock, const fhandle3*, void* data, uint32_t size, uint64_t offset);

static int cnfs_read(const char *path, char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
	size_t len;
	(void) fi;

    if(strcmp(path, "/log") == 0)
    {
        len = logSize;
	    if (offset < len) {
		    if (offset + size > logSize)
			    size = logSize - offset;
		    memcpy(buf, log_buffer + offset, size);
        } else
            size = 0;

        static int AddedToLog = 0;
        if (AddedToLog)
        {
            AddedToLog = 0;
            return 0;
        }
        else
        {
            AddedToLog = 1;
            return logSize;
        }
    }
    else
    {
        // AddLog("reading file: %s size: %u offset %u", path, (unsigned int)size, (unsigned int)offset);

        meta_data_entry_t* entry =
            LookupPath(&dirCache, path, strlen(path));

        if (entry->type == ENTRY_TYPE_FILE)
        {
            fhandle3 handle = ptrToHandle(&dirCache, entry->handle);
            if (offset == 0)
            {
                entry->cached_file->data =
                    realloc(entry->cached_file->data, entry->cached_file->size);
            }
            int read = entry->cached_file->size;
            if (!(entry->flags & ENTRY_FLAG_VIRTUAL))
            {
                read = nfs_read(nfs_sock_fd, &handle
                    , buf, size, offset);
                memcpy(entry->cached_file->data + offset, buf, read);
            }
            else
            {
                if (size > (entry->cached_file->size - offset))
                    size = (entry->cached_file->size - offset);
                memcpy(buf, entry->cached_file->data + offset, size);
            }
            return read;
        }
        else
            return -ENOENT;
    }


	return size;
}

/** Change the permission bits of a file */
static int cnfs_chmod (const char * full_path, mode_t mode)
{
    return 0;
}

/** Change the owner and group of a file */
static int cnfs_chown (const char * full_path, uid_t uid, gid_t gid)
{
    return 0;
}
/** Set size of a file */
static int cnfs_truncate (const char * full_path, off_t newSize)
{
    meta_data_entry_t* file =
        LookupPath(&dirCache, full_path, strlen(full_path));
    if (!file)
    {
        return -ENOENT;
    }
    if (file->type != ENTRY_TYPE_FILE)
    {
        return -EISDIR;
    }

    file->cached_file->size = newSize;
    return 0;
}

void FreeEntry(cache_t* cache, cached_dir_t* parentDir, meta_data_entry_t* entry)
{
    assert(entry >= parentDir->entries && entry < parentDir->entries + parentDir->entries_size);
    size_t entry_idx = entry - parentDir->entries;
    size_t how_many = parentDir->entries_size - entry_idx;
    if (how_many)
    {
        memmove(entry, entry + 1, how_many - 1);
    }
    parentDir->entries_size--;
}


void nfs_remove(SOCKET nfs_fd, fhandle3* dirHandle, const char* filename, uint32_t filename_length);

/** Remove a file */
int cnfs_unlink (const char * full_path)
{
    lookup_parent_result_t result =
        LookupParent(&dirCache, full_path, strlen(full_path));

    if (!result.parentDir)
    {
        return -ENOENT;
    }

    meta_data_entry_t * file =
        LookupInDirectory(&dirCache, result.parentDir->cached_dir,
            result.entry_name, result.entry_name_length);
    if (!file)
    {
        return -ENOENT;
    }

    if (file->type == ENTRY_TYPE_DIRECTORY)
    {
        return -EISDIR;
    }

    fhandle3 handle = ptrToHandle(&dirCache, result.parentDir->handle);
    nfs_remove(nfs_sock_fd, &handle, result.entry_name, result.entry_name_length);
    
    FreeEntry(&dirCache, result.parentDir->cached_dir, file);

    return 0;
}

/** Remove a directory */
int cnfs_rmdir (const char * full_path)
{
    lookup_parent_result_t result =
        LookupParent(&dirCache, full_path, strlen(full_path));

    if (!result.parentDir)
    {
        return -ENOENT;
    }

    meta_data_entry_t * dir =
        LookupInDirectory(&dirCache, result.parentDir->cached_dir,
            result.entry_name, result.entry_name_length);

    if (!dir)
    {
        return -ENOENT;
    }
    assert(dir->type == ENTRY_TYPE_DIRECTORY);
    if(dir->cached_dir->entries_size != 0)
    {
        return -ENOTEMPTY;
    }
    FreeEntry(&dirCache, result.parentDir->cached_dir, dir);
}

static const struct fuse_operations cnfs_oper = {
	.init       = cnfs_init,
	.getattr    = cnfs_getattr,
	.readdir    = cnfs_readdir,
    .truncate   = cnfs_truncate,
    .unlink     = cnfs_unlink,
    .rmdir      = cnfs_rmdir,
	// .open       = cnfs_open,
	.read       = cnfs_read,
    .write      = cnfs_write,
    .mknod      = cnfs_mknod,
    .mkdir      = cnfs_mkdir,
};

static void show_help(const char *progname)
{
	printf("usage: %s [options] <mountpoint>\n\n", progname);
	printf("File-system specific options:\n"
	       "    --name=<s>          Name of the \"hello\" file\n"
	       "                        (default: \"hello\")\n"
	       "    --contents=<s>      Contents \"hello\" file\n"
	       "                        (default \"Hello, World!\\n\")\n"
	       "\n");
}

extern int nfs_init_cache(cache_t* dirCache, int argc, char* argv[]);
extern int connect_name(const char* hostname, const char* port);

int main(int argc, char *argv[])
{
	int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    logStat.st_mode = S_IFREG | 0444;
    logStat.st_nlink = 1;
    logStat.st_size = 0;

    log_buffer = malloc(sizeof(char[65536]));

    nfs_init_cache(&dirCache, 1, argv);
    
    nfs_sock_fd = connect_name("192.168.178.26", "2049");

	/* Set defaults -- we have to use strdup so that
	   fuse_opt_parse can free the defaults if other
	   values are specified */
	options.filename = strdup("hello");
	options.contents = strdup("Hello World!\n");

	/* Parse options */
	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
		return 1;

	/* When --help is specified, first print our own file-system
	   specific help text, then signal fuse_main to show
	   additional help (by adding `--help` to the options again)
	   without usage: line (by setting argv[0] to the empty
	   string) */
	if (options.show_help) {
		show_help(argv[0]);
		assert(fuse_opt_add_arg(&args, "--help") == 0);
		args.argv[0][0] = '\0';
	}

	ret = fuse_main(args.argc, args.argv, &cnfs_oper, NULL);
	fuse_opt_free_args(&args);
	return ret;
}
