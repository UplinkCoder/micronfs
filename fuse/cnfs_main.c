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

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <stdlib.h>

#include "../micronfs.h"

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
static char log_buffer[65536];

static void AddLog_(const char* msg, int len)
{
    len = (len ? len : strlen(msg));
    memcpy(log_buffer + logSize, msg, len);
    logSize +=  (len + 1);
    log_buffer[logSize - 1] = '\n';
}
#define AddLog(STR, ...) \
    { \
        char fmt[512]; \
        int sz = sprintf(fmt, STR, __VA_ARGS__); \
        AddLog_(fmt, sz); \
    }

static int nfs_sock_fd = 0;
static fileList_t* rootDir = 0;
static fileList_t* currentDir = 0;

static fileList_t* nextFree = 0;
static uint32_t freeListNodes = 0;

fileList_t* TraverseDownInto(fileList_t* dir, const char** mPathP);

static int cnfs_getattr(const char *path, struct stat *stbuf)
{
	int res = -ENOENT;
    AddLog("getattr: path='%s'\n", path);
	memset(stbuf, 0, sizeof(struct stat));
	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
        res = 0;
	} else if (strcmp(path+1, options.filename) == 0) {
		stbuf->st_mode = S_IFREG | 0444;
		stbuf->st_nlink = 1;
		stbuf->st_size = strlen(options.contents);
        res = 0;
	} else if (strcmp(path, "/log") == 0) {
		stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_size = logSize;
        stbuf->st_nlink = 1;
        res = 0;
    } else
    {
        fileList_t* dir = currentDir;
        if (path[0] == '/')
        {
            char* mPath = path + 1;
            dir = TraverseDownInto(rootDir, &mPath);
            if (!dir) dir = rootDir;
            {
                printf("dir '%s' has :%d chlidren\n", dir->name, dir->n_entires);
            }
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

	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL,  0);
	filler(buf, "..", NULL, 0);
    for(int i = 0; i < currentDir->n_entires; i++)
    {
        fileList_t* entry = currentDir->entries + i;
        struct stat s;
        if (!entry->bitarray_entires)
        {
            s.st_size = entry->size;
        }
        filler(buf, entry->name, &s, 0);
    }
	//filler(buf, options.filename, NULL,  0);
    if (!path || path[0] == '/' && path[1] == '\0')
        filler(buf, "log", NULL, 0);

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

fileList_t* TraverseUp(fileList_t* dir, int levelsUp)
{
    while(levelsUp--)
    {
        if (!dir->parent)
            break;
        else
            dir = dir->parent;
    }
    
    return dir;
}

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
        if (0 == strcmp(entry->name, name))
        {
            result = entry;
            break;
        }
    }
Lreturn:
    return result;
}

int AddDir(fileList_t* parent, const char* newDirName)
{
    if (newDirName[0] == '/') newDirName++;
    const char c = newDirName[0];
    
    const idx = (c & ~32) - 'A';
    
    assert(parent->bitarray_entires != 0);
    assert(freeListNodes);
    
    if (LookupName(parent, newDirName) != 0)
        return -EEXIST;
    
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

fileList_t* TraverseDownInto(fileList_t* dir, const char** mPathP)
{
    const char* startPath =  *mPathP;
    const char *mPath = startPath;
    char c = *mPath;
    const uint32_t bitfield = dir ? dir->bitarray_entires : 0;
    int idx = (c & ~32) - 'A';
    fileList_t *canidate = 0;
    fileList_t *pastLastEntry;
    char component_storage[256];
    int len = 0;
    
    if ((bitfield & (1 << 31)) == 0)
        return 0;

    if ((idx <= 26 && (dir->bitarray_entires & (1 << idx))) 
        || dir->bitarray_entires & (1 << 27))
    {
        while(*mPath)
        {
            for(;(c = *mPath);mPath++)
            {
                if (c == '/')
                {
                    component_storage[len] = '\0';
                    break;
                }
                else
                {
                    component_storage[len++] = c;
                }
            }
            {
                pastLastEntry = dir->entries + dir->n_entires;
                for(canidate = dir->entries;
                    canidate < pastLastEntry;
                    canidate++)
                {
                    if (strcmp(canidate->name, component_storage))
                    {
                        dir = canidate;
                        break;
                    }
                }
                // if we reach here it means we couldn't find the path
                fprintf(stderr, "path [%s] could not be entered into\n", *mPath);
                goto Lend;
            }
        }
    }
    else
    {
        dir = 0;
    }
Lend:
    *mPathP = mPath; 
    return dir;
}

static int cnfs_mkdir (const char *name, mode_t mode)
{
    AddLog("mkdir: '%s'", name);
    AddDir(currentDir, name);
}

static int cnfs_open(const char *path, struct fuse_file_info *fi)
{
    if (strcmp(path, "/log") == 0)
    {
        return 0;
    }
    char* mPath;
    
    int levels_up;
    
    mPath = EatAndCountDotDotPath(path, &levels_up);
    fileList_t* relativeDir = TraverseUp(currentDir, levels_up);
    
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
    }

	if(strcmp(path+1, options.filename) != 0)
		return -ENOENT;

	len = strlen(options.contents);
	if (offset < len) {
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, options.contents + offset, size);
	} else
		size = 0;

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

int main(int argc, char *argv[])
{
	int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    freeListNodes = 4096;
    nextFree = (fileList_t*)malloc(sizeof(fileList_t) * freeListNodes);
    
    //setup the root entry
    rootDir = nextFree++;
    rootDir->bitarray_entires |= (1 << 31);
    rootDir->name = "/";
    currentDir = rootDir;

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
