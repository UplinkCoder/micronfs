#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "cached_tree.h"
#include "crc32.c"

static int err;

meta_data_entry_t* LookupPathByKey(cache_t* cache, const char* full_path,
                                   uint32_t entry_key)
{
    uint32_t toc_size = cache->toc_size;
    meta_data_entry_t* result = 0;
    size_t path_length = (entry_key >> 16);

    toc_entry_t* one_past_last = cache->toc_entries + toc_size;
    for(toc_entry_t* toc_entry = cache->toc_entries;
        toc_entry < one_past_last; toc_entry++)
    {
        if (toc_entry->entry_key == entry_key)
        {
            const char* entry_path =
                (toc_entry->relative_name_pointer.v - 4)
                + cache->name_cache;
            if (!memcmp(entry_path, full_path, path_length))
            {
                result = toc_entry->entry;
                break;
            }
        }
    }

    return result;
}

meta_data_entry_t* LookupPath(cache_t* cache, const char* full_path,
                              size_t path_length)
{
    uint16_t lower_crc = (uint16_t) crc32c(~0, full_path, path_length);
    uint32_t entry_key =  (((uint16_t)path_length) << 16) | lower_crc;
    return LookupPathByKey(cache, full_path, entry_key);
}

name_cache_ptr_t GetOrAddNameByKey(cache_t* cache, const char* name,
                                   uint32_t entry_key)
{

    size_t length = (entry_key >> 16);
    if (!length)
        return (name_cache_ptr_t) {0};

    for(name_cache_node_t* currentBranch = cache->name_cache_root;
        currentBranch;)
    {
        int cmp_result = entry_key - currentBranch->entry_key;
        if (!cmp_result)
        {
            const char* cached_name = (currentBranch->name_ptr.v - 4)
                                    + cache->name_cache;
            if ((cmp_result = memcmp(name, cached_name, length)) == 0)
            {
                return currentBranch->name_ptr;
            }
        }
        // when we end up here we need to branch
        {
            name_cache_node_t* nextBranch = currentBranch +
                                            ((cmp_result < 0)
                                              ? currentBranch->left
                                              : currentBranch->right);
            if (nextBranch != currentBranch)
            {
                currentBranch = nextBranch;
                continue;
            }

            // here we need to insert a new node
            {
                assert(cache->name_cache_node_size
                       < cache->name_cache_node_capacity);
                nextBranch = (cache->name_cache_node_size++
                           + cache->name_cache_root);
                if (cmp_result < 0)
                {
                    currentBranch->left = nextBranch - currentBranch;
                }
                else
                {
                    currentBranch->right = nextBranch - currentBranch;
                }

                assert(cache->name_cache_size
                       < cache->name_cache_capacity);
                char* cached_name =
                    cache->name_cache_size + cache->name_cache;
                memcpy(cached_name, name, length);
                *(cached_name + length) = '\0';


                name_cache_ptr_t name_ptr = { cache->name_cache_size + 4 };
                cache->name_cache_size += ALIGN4(length + 1);

                *nextBranch =
                    (name_cache_node_t){entry_key, 0, 0, name_ptr};
                return nextBranch->name_ptr;
            }
        }
    }

    assert(0);
}

name_cache_ptr_t GetOrAddNameLength(cache_t* cache, const char* name,
                                    size_t length)
{
    assert(length <= 0xFFFF);

    uint32_t crcName = crc32c(~0, name, length);
    uint32_t entry_key = ((crcName & 0xFFFF) | ((uint16_t)length) << 16);

    return GetOrAddNameByKey(cache, name, entry_key);
}

name_cache_ptr_t GetOrAddName(cache_t* cache, const char* name)
{
    return GetOrAddNameLength(cache, name, strlen(name));
}

void ResetCache(cache_t* cache)
{
    cache->toc_size = 0;
    cache->metadata_size = 0;
    cache->name_cache_root->entry_key = 0x7fff;
    cache->name_cache_root->left = 0;
    cache->name_cache_root->right = 0;
    cache->name_cache_size = 0;
    cache->name_cache_node_size = 1;
}

meta_data_entry_t* LookupInDirectoryByKey(cache_t* cache, cached_dir_t* lookupDir,
                                          const char* name, uint32_t entry_key)
{
    meta_data_entry_t* result = 0;

    const size_t name_length = (entry_key >> 16);

    meta_data_entry_t const * one_past_last_entry =
        lookupDir->entries + lookupDir->entries_size;

    for(meta_data_entry_t* entry = lookupDir->entries;
        entry < one_past_last_entry;
        entry++)
    {
        if (entry_key == entry->entry_key)
        {
            const char* entry_name =
                cache->name_cache + (entry->name.v - 4);
            if (!memcmp(name, entry_name, name_length))
            {
                result = entry;
                break;
            }
        }
    }

    return result;
}

static inline uint32_t EntryKey(const char* name, size_t name_length)
{
    assert(name_length <= 0xFFFF);

    const uint32_t name_crc =
    crc32c(~0, name, name_length);
    const uint32_t entry_key = (name_crc & 0xFFFF)
                             | (name_length << 16);
    return entry_key;
}

meta_data_entry_t* LookupInDirectory(cache_t* cache, cached_dir_t* lookupDir,
                                     const char* name, size_t name_length)
{
    const uint32_t name_crc =
        crc32c(~0, name, name_length);
    const uint32_t entry_key = (name_crc & 0xFFFF)
                             | (name_length << 16);
    return LookupInDirectoryByKey(cache, lookupDir, name, entry_key);
}

cached_dir_t* GetOrCreateSubdirectoryByKey(cache_t* cache, cached_dir_t* parentDir,
                                      const char* directory_name, uint32_t entry_key)
{
    cached_dir_t* result = 0;

    meta_data_entry_t* lookupResult =
        LookupInDirectoryByKey(cache, parentDir, directory_name, entry_key);

    if (lookupResult)
    {
        if (lookupResult->type == ENTRY_TYPE_DIRECTORY)
            result = lookupResult->cached_dir;
        else
            err = -EEXIST;
        goto Lret;
    }

    if (parentDir->entries_capacity == 0)
    {
        // allocate 64 direntires for now
        assert(cache->metadata_size + 64 < cache->metadata_capacity);
        meta_data_entry_t*  entires = cache->root + cache->metadata_size;
        cache->metadata_size += 64;
        parentDir->entries = entires;
        parentDir->entries_capacity = 64;
    }

    assert(cache->dir_entries_capacity > cache->dir_entries_size);
    result = cache->dir_entries + cache->dir_entries_size++;

    assert(parentDir->entries_size < parentDir->entries_capacity);
    meta_data_entry_t* subdir =
        parentDir->entries + parentDir->entries_size++;

    subdir->entry_key = entry_key;
    subdir->name = GetOrAddNameByKey(cache, directory_name, entry_key);

    subdir->type = ENTRY_TYPE_DIRECTORY;
    subdir->cached_dir = result;
Lret:
    return result;
}

cached_dir_t* GetOrCreateSubdirectory(cache_t* cache, cached_dir_t* parentDir,
                                      const char* directory_name, size_t name_length)
{
    const uint32_t key = EntryKey(directory_name, name_length);
    return GetOrCreateSubdirectoryByKey(cache, parentDir, directory_name, key);
}


meta_data_entry_t* CreateEntry(cache_t* cache, const char* full_path,
                               size_t path_length)
{
    meta_data_entry_t* result = 0;
    assert(full_path[0] == '/');
    assert(full_path[path_length - 1] != '/');
    if (path_length < 2)
        return cache->root;

    // make sure we dont have an empty last part

    uint32_t full_path_key = EntryKey(full_path, path_length);

    if (LookupPathByKey(cache, full_path, full_path_key))
    {
        goto Lexists;
    }

    cached_dir_t* currentDir = cache->root->cached_dir;
    uint32_t path_remaining = path_length - 1;
    const char* begin_segment = full_path + 1;

    for (;;)
    {
        const char *end_segment =
            memchr(begin_segment, '/', path_remaining);
        if (end_segment == 0)
            break;

        size_t segment_length = end_segment - begin_segment;
        uint32_t segment_key = EntryKey(begin_segment, segment_length);
        currentDir = GetOrCreateSubdirectoryByKey(cache, currentDir, begin_segment, segment_key);
        begin_segment += segment_length + 1;
        path_remaining -= (segment_length + 1);
    }

    uint32_t entry_key = EntryKey(begin_segment, path_remaining);

    result = LookupInDirectoryByKey(cache, currentDir, begin_segment, entry_key);
    if (result)
    {
Lexists:
        err = -EEXIST;
        result = 0;
        goto Lret;
    }
    // when we get here we can create our entry
    assert(currentDir->entries_capacity > currentDir->entries_size);
    result = currentDir->entries + currentDir->entries_size++;
    result->entry_key = entry_key;
    result->name = GetOrAddNameByKey(cache, begin_segment, entry_key);

    assert(cache->toc_capacity > cache->toc_size);
    toc_entry_t* toc_entry = cache->toc_entries + cache->toc_size++;
    toc_entry->entry_key = full_path_key;
    toc_entry->relative_name_pointer =
        GetOrAddNameByKey(cache, full_path, full_path_key);

Lret:
    return result;
}


/// Adds or updates a file
meta_data_entry_t* AddFile(cache_t* cache, const char* full_path,
                            const void* content, uint32_t content_size)
{
    uint16_t path_length = (uint16_t)strlen(full_path);

    meta_data_entry_t* result
        = LookupPath(cache, full_path, path_length);

    if (!result)
    {
        result = CreateEntry(cache, full_path, path_length);
        result->type = ENTRY_TYPE_FILE;
        assert(cache->dir_entries_capacity > cache->dir_entries_size);
        result->cached_file =
            (cached_file_t*)cache->dir_entries + cache->dir_entries_size++;

        result->cached_file->crc32 = 0;
        result->cached_file->size = 0;
        result->cached_file->data = 0;
    }

    assert(result->type == ENTRY_TYPE_FILE);
    cached_file_t *file =  result->cached_file;

    uint32_t content_crc = crc32c(~0, content, content_size);
    if (content_size == file->size && content_crc == file->crc32)
    {
        // puts("size and crc match .. not updating content\n");
    }
    else
    {
        if (content_size != file->size)
            file->data = realloc(file->data, content_size);

        memcpy(file->data, content, content_size);
        file->crc32 = content_crc;
        file->size = content_size;
    }

    return result;
}

#if TEST_CACHE

#include <stdio.h>
#include <stdlib.h>
void PrintNameCache(cache_t* cache)
{
    char const * one_past_last = cache->name_cache
                               + cache->name_cache_size;
    int i = 0;
    for(const char* str = cache->name_cache;
        str < one_past_last;
        str += (ALIGN4(strlen(str) + 1)))
    {
        printf("%d: %s\n", ++i, str);
    }
}

int main(int argc, char** argv)
{
    uint32_t initial_name_storage_capacity = 8192;
    uint32_t initial_name_nodes_capacity = 256;
    uint32_t initial_metadata_nodes = 512;
    uint32_t initial_dir_nodes = 256;
    uint32_t initial_toc_capacity = 256;

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

    cache_t cache = {
        .toc_entries = toc_mem,
        .root = meta_mem++,

        .toc_size = 0,
        .toc_capacity = initial_toc_capacity,

        .metadata_size = 1,
        .metadata_capacity = initial_metadata_nodes,

        .name_cache  = (char*)(cache_memory + sizeof(cache_t)),
        .name_cache_size = 0,
        .name_cache_capacity = initial_name_storage_capacity,

        .name_cache_root = tree_mem,
        .name_cache_node_size = 1,
        .name_cache_node_capacity = initial_name_nodes_capacity,

        .dir_entries = dirs_mem,
        .dir_entries_size = 0,
        .dir_entries_capacity = initial_dir_nodes
    };

    cache.root->cached_dir = dirs_mem++;

    ResetCache(&cache);
    CreateEntry(&cache, "/Hello/World", strlen("/Hello/World"));

    name_cache_ptr_t w = GetOrAddName(&cache, "William");
    printf("W NameCachePtr: %u\n", w.v);
    name_cache_ptr_t c = GetOrAddName(&cache, "Christoph");
    printf("C NameCachePtr: %u\n", c.v);
    name_cache_ptr_t w2 = GetOrAddName(&cache, "William");
    printf("W2 NameCachePtr: %u\n", w2.v);

    printf("\n\n\t sizeof(cache_t): %d\n", sizeof(cache_t));

    return 0;
}

#endif
