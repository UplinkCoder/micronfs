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
static fileList_t* rootDir = 0;

static fileList_t* nextFree = 0;
static uint32_t freeListNodes = 0;

fileList_t* TraverseDownTo(const char** mPathP);
fileList_t* TraverseDownToDir(const char** mPathP);

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

    fileList_t* currentDir = rootDir;

	if (strcmp(path, "/") != 0)
    {
        const char* mPath = path + 1;
		currentDir = TraverseDownTo(&mPath);
    }

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

int AddDir(fileList_t* parent, const char* newDirName)
{
    if (newDirName[0] == '/') newDirName++;
    const char c = newDirName[0];

    const idx = (c & ~32) - 'A';

    assert(parent->bitarray_entires != 0);
    assert(freeListNodes);
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

fileList_t* TraverseDownTo_(const char** mPathP, bool stopAtDir)
{
    fprintf(stderr, "called TraverseDownTo with: {\"%s\", %d}\n",
        *mPathP, stopAtDir
    );
    fileList_t* parent = rootDir;
    fileList_t* result = rootDir;

    const char *resultPath =  *mPathP;
    const char *mPath = resultPath;
    char c = *mPath;
    const uint32_t bitfield = rootDir->bitarray_entires;
    int idx = (c & ~32) - 'A';
    fileList_t *canidate = 0;
    fileList_t *pastLastEntry;
    char component_storage[256];
    int len = 0;

    const char* mPathEnd = (const char*)rawmemchr(mPath, '\0');
    if (stopAtDir)
    {
        char c;
        for(const char* newPathEnd = mPathEnd;
            newPathEnd > mPath; --newPathEnd)
            {
                if ((c = *newPathEnd) == '/')
                {
                    mPathEnd = newPathEnd;
                    break;
                }
            }
    }


    if ((bitfield & (1 << 31)) == 0)
        return 0;

    if ((idx <= 26 && (bitfield & (1 << idx)))
        || bitfield & (1 << 27))
    {
        while(mPath < mPathEnd)
        {
            len = 0;
            resultPath = mPath;
            for(;(c = *mPath);mPath++)
            {
                if (c == '/')
                {
                    mPath++;
                    break;
                }
                else if (c == '.')
                {
                    int levels;
                    mPath = EatAndCountDotDotPath(mPath, &levels);
                    if (*mPath == '/')
                    {
                            // noop eat the '/'
                            mPath++;
                    }
                }
                else
                {
                    component_storage[len++] = c;
                }
            }
            component_storage[len] = '\0';
            printf ("found componenet: '%s'\n", component_storage);
            int lastComponent = (strlen(mPath) == len);
            {
                pastLastEntry = parent->entries + parent->n_entires;
                for(canidate = result->entries;
                    canidate < pastLastEntry;
                    canidate++)
                {
                    if (0 == strcmp(canidate->name, component_storage))
                    {
                        parent = canidate;
                        if (lastComponent)
                            result = canidate;
                        goto Lfound;
                    }
                }
                // if we reach here it means we couldn't find the path
                fprintf(stderr, "path [%s] not could be resolved\n\tfailing fragment:%s\n", *mPathP, mPath);
                result = 0;
                goto Lend;
            }
Lfound:
            // if we reach here we've found another path component
            if (*mPath == '/')
                mPath++;
            resultPath = mPath;
        }
    }

Lend:
    if (result) printf("entry: %s\n", result->name);
    *mPathP = resultPath;
    return result;
}

fileList_t* TraverseDownTo(const char** mPathP)
{
    return TraverseDownTo_(mPathP, false);
}

fileList_t* TraverseDownToDir(const char** mPathP)
{
    return TraverseDownTo_(mPathP, true);
}
static int cnfs_mkdir (const char *path, mode_t mode)
{
    AddLog("mkdir: '%s'", path);
    const char* mPath = path + 1;
    fileList_t* parent = TraverseDownToDir(&mPath);
    AddDir(parent, mPath);
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
	.open       = cnfs_open,
	.read       = cnfs_read,
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

void InitCache(cache_t* cache)
{
    uint32_t initial_name_storage_capacity = 65536;
    uint32_t initial_name_nodes_capacity = 4096;
    uint32_t initial_files_capacity = 16384;

    uint32_t initial_metadata_nodes = 16384 * 4;
    uint32_t initial_dir_nodes = 2048;
    uint32_t initial_toc_capacity = 2048;
    uint32_t initial_limb_capacity = 16384;

    uint8_t* cache_memory = (uint8_t*) malloc(
        sizeof(cache_t) + initial_name_storage_capacity
                        + (initial_name_nodes_capacity *
                            sizeof(name_cache_node_t)));

    toc_entry_t* toc_mem = (toc_entry_t*) calloc(
        initial_toc_capacity, sizeof(toc_entry_t));

    name_cache_node_t* tree_mem = (name_cache_node_t*)
                                (cache_memory + sizeof(cache_t)
                                + initial_name_storage_capacity);

    meta_data_entry_t* meta_mem = (meta_data_entry_t*) calloc(
        initial_metadata_nodes , sizeof(meta_data_entry_t));

    cached_dir_t* dirs_mem = (cached_dir_t*)calloc(
        initial_dir_nodes, sizeof(cached_dir_t));

    uint32_t* limbs_mem = (uint32_t*) calloc(
        initial_limb_capacity , sizeof(uint32_t));

    cached_file_t* files_mem = (cached_file_t*)
        calloc(initial_files_capacity, sizeof(cached_file_t));


    *cache = (cache_t) {
        .toc_entries = toc_mem,
        .root = meta_mem++,

        .toc_size = 0,
        .toc_capacity = initial_toc_capacity,

        .metadata_size = 1,
        .metadata_capacity = initial_metadata_nodes,

        .name_stringtable  = (char*)(cache_memory + sizeof(cache_t)),
        .name_stringtable_size = 0,
        .name_stringtable_capacity = initial_name_storage_capacity,

        .name_cache_root = tree_mem,
        .name_cache_node_size = 1,
        .name_cache_node_capacity = initial_name_nodes_capacity,

        .dir_entries = dirs_mem,
        .dir_entries_size = 0,
        .dir_entries_capacity = initial_dir_nodes,

        .file_entries = files_mem,
        .file_entries_size = 0,
        .file_entries_capacity = initial_files_capacity,

        .limbs = limbs_mem,
        .limbs_size = 0,
        .limbs_capacity = initial_limb_capacity
    };

    cache->root->cached_dir = cache->dir_entries + cache->dir_entries_size++;
    cache->root->cached_dir->fullPath = GetOrAddName(cache, "/");

    cache->root->cached_dir->entries = cache->root + cache->metadata_size;
    cache->root->cached_dir->entries_size = 0;
    cache->root->cached_dir->entries_capacity = 256;
    cache->metadata_size += 256;


    ResetCache(cache);
}


int main(int argc, char *argv[])
{
	int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    logStat.st_mode = S_IFREG | 0444;
    logStat.st_nlink = 1;
    logStat.st_size = 0;

    freeListNodes = 4096;
    nextFree = (fileList_t*)malloc(sizeof(fileList_t) * freeListNodes);
    log_buffer = malloc(sizeof(char[65536]));
    // setup dir cache and dummy file
    InitCache(&dirCache);
    AddFile(&dirCache, "/dont_read_me",
        "I told you not to read me\0",
        strlen("I told you not to read me") + 1);

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
