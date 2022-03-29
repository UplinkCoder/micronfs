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


#define FUSE_USE_VERSION 26

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

typedef struct fileList_t {
    const char* name;
    struct fileList_t* parent;
    uint32_t bitarray_entires; // bit 31 indicates a directory
                               // bits 0-26 stand for a-z entries exisitng bit 26 stands for anything else
                               // bits 27-31 have no meaning at the moment
    union
    {
        struct {
            int n_entires;
            struct fileList_t* entries;
        }; // for direcories

        struct {
            uint32_t size;

        } // for files
    };
} fileList_t;
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


fileList_t* LookupName(fileList_t* parent, const char* name)
{
    const char c = name[0] == '/' ? name[1] : name[0];
    const idx = (c & ~32) - 'A';
    fileList_t* result = 0;
    const uint32_t bitfield = parent->bitarray_entires;

    assert(bitfield != 0);
    if (idx < 26 && !(bitfield & (1 << idx)))
        goto Lreturn;
    fileList_t* onePastLast = parent->entries + parent->n_entires;

    for(fileList_t* entry = parent->entries;
        entry < onePastLast;
        entry++)
    {
        printf("'%s' == '%s'\n", entry->name, name);
        if (0 == strcmp(entry->name, name))
        {
            result = entry;
            break;
        }
    }
Lreturn:
    return result;
}

static int cnfs_getattr(const char *path, struct stat *stbuf)
{
    assert(path[0] == '/');
	int res = 0;
    AddLog("getattr: path='%s'\n", path);
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
            printf("Got entry\n");
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

    AddLog("readdir; path='%s', offset=%d", path, offset);

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
#if 0
int AddDir(fileList_t* parent, const char* newDirName)
{
    if (newDirName[0] == '/') newDirName++;
    const char c = newDirName[0];

    const idx = (c & ~32) - 'A';

    fileList_t* lookupResult = 0;

    if ((lookupResult = LookupName(rootDir, newDirName)) != 0)
    {
        int k = 12;
        return -EEXIST;
    }
    fileList_t newNode;

    newNode.bitarray_entires = (1 << 31);
    newNode.n_entires = 0;
    newNode.parent = parent;
    newNode.name = strdup(newDirName);

    if (idx < 26)
    {
        parent->bitarray_entires |= (1 << idx);
    }
    else
    {
        parent->bitarray_entires |= (1 << 27);
    }
    // alloc new directory_entry array
    // let's simply reserve a block from our freeNodes
    if (!parent->entries)
    {
        parent->entries = nextFree;
        nextFree += 256;
        freeListNodes -= 256;
    }
    else
    {
        assert(parent->n_entires < 256);
        // we cannot have more than 256 entries we would override thingys
    }
    {
        parent->entries[parent->n_entires++] = newNode;
    }

    return 0;
}
#endif

static int cnfs_mkdir (const char *full_path, mode_t mode)
{
    AddLog("mkdir: '%s'", full_path);
    
    
    char* end = rawmemchr(full_path, '\0');
    size_t path_length = end - full_path;
    size_t slash_posiiton = 0;
    
    for(const char* p = end;
        p > full_path;
        p--
    )
    {
        if (*p == '/')
        {
            slash_posiiton =  p - full_path;
            break;
        }
    }
    
    size_t last_component_length = path_length - (slash_posiiton + 1);
    const char* last_component_ptr = full_path + (slash_posiiton + 1);

    size_t dir_path_size = path_length - (last_component_length + 1);
    const uint32_t dir_entry_key = EntryKey(full_path, dir_path_size);
    
    cached_dir_t* parent_dir 
        = LookupPath(&dirCache, full_path, dir_path_size)->cached_dir;
    
    if (!parent_dir)
        return -ENOENT;
    
    if (!GetOrCreateSubdirectory(&dirCache, 
        parent_dir, last_component_ptr, last_component_length))
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
    char* mPath = path + 1;
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

static int cnfs_write(const char *path, const char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi)
{
    printf("Calling write\n");
    assert(offset == 0);
    meta_data_entry_t* e;
    e = LookupPath(&dirCache, path, strlen(path));
    if (e)
    {
        AddFile(&dirCache, path, buf, size);
        return size;
    }
    else
    {
        return -ENOENT;
    }
}

static int cnfs_mknod(const char * path, mode_t mode, dev_t dev)
{
    if (LookupPath(&dirCache, path, strlen(path)))
    {
        return -EEXIST;
    }
    
    AddFile(&dirCache, path, 0, 0);
    return 0;
}
fhandle3 ptrToHandle(cache_t* cache, filehandle_ptr_t fh_ptr);
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
        AddLog("reading file: %s", path);

        meta_data_entry_t* entry =
            LookupPath(&dirCache, path, strlen(path));

        if (entry->type == ENTRY_TYPE_FILE)
        {
            if (entry->cached_file->data == 0)
            {
                fhandle3 handle = ptrToHandle(&dirCache, entry->handle);
                entry->cached_file->data = 
                    realloc(entry->cached_file->data, entry->cached_file->size);
                int read = nfs_read(nfs_sock_fd, &handle
                    , buf, size, offset);
                memcpy(entry->cached_file->data, buf, read);
                return read;
            }
            len = entry->cached_file->size;

            if (offset < len) {
                if (offset + size > logSize)
                    size = logSize - offset;
                memcpy(buf, entry->cached_file->data + offset, size);
            } else
                size = 0;
        }
        else
            return -ENOENT;
    }


	return size;
}

static const struct fuse_operations cnfs_oper = {
	.init       = cnfs_init,
	.getattr    = cnfs_getattr,
	.readdir    = cnfs_readdir,
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
