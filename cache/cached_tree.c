#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "crc32.c"

#include "cached_tree.h"

meta_data_entry_t* LookupPath(cache_t* cache, const char* full_path,
                              uint16_t path_length)
{
    uint32_t toc_size = cache->toc_size;
    uint16_t lower_crc = (uint16_t) crc32c(~0, full_path, path_length);
    uint32_t entry_key =  (((uint16_t)path_length) << 16) | lower_crc;
    meta_data_entry_t* result = 0;

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

name_cache_ptr_t GetOrAddName(cache_t* cache, const char* name)
{
    return GetOrAddNameLength(cache, name, strlen(name));
}

name_cache_ptr_t GetOrAddNameLength(cache_t* cache, const char* name,
                                    size_t length)
{
    assert(length <= 0xFFFF);

    uint32_t crcName = crc32c(~0, name, length);
    uint32_t entry_key = ((crcName & 0xFFFF) | ((uint16_t)length) << 16);

    name_cache_node_t* lastVisited = 0;

    for(name_cache_node_t* currentBranch = cache->name_cache_root;
        currentBranch;
        lastVisited = currentBranch)
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
                cache->name_cache_size += length + 1;

                *nextBranch =
                    (name_cache_node_t){entry_key, 0, 0, name_ptr};
                return nextBranch->name_ptr;
            }
        }
    }

    assert(0);
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


meta_data_entry_t* CreateEntry(cache_t* cache, const char* full_path,
                               uint16_t path_length)
{
    cached_dir_t* currentDir = cache->root->cached_dir;
    uint32_t path_remaining = path_length;
    const char* begin_segment = full_path;
    char *end_segment = memchr(begin_segment, '/', path_remaining);
LbeginLoop:
    while(end_segment)
    {
        const uint32_t segment_length =
            end_segment - begin_segment;
        const uint32_t segment_crc =
            crc32c(~0, begin_segment, segment_length);
        const uint32_t entry_key = (segment_crc & 0xFFFF)
                                 | (segment_length << 16);

        const meta_data_entry_t const * one_past_last_entry =
            currentDir->entires + currentDir->n_entires;

        for(meta_data_entry_t* entry = currentDir->entires;
            entry < one_past_last_entry;
            entry++)
        {
            if (entry_key == entry->entry_key)
            {
                const char* entry_name = 
                    cache->name_cache + (entry->name.v - 4);
                if (!memcmp(begin_segment, entry_name, segment_length))
                {
                    assert(entry->type == ENTRY_TYPE_DIRECTORY);
                    currentDir = entry->cached_dir;

                    path_remaining -= (segment_length + 1);
                    begin_segment += (segment_length + 1);

                    end_segment =
                        memchr(begin_segment, '/', path_remaining);
                    int k = 2;
                    goto LbeginLoop;
                }
            }
        }
        // when we reach this point we cannot find any more entries matching the path
        break;
    }
#if 0
    meta_data_entry_t* subdir =
        cache.root->cached_dir->entires =
            cache.root + cache.metadata_size++;

    subdir->name = GetOrAddName(&cache, "Hello");
    subdir->entry_key =
        (strlen("Hello") << 16) | (crc32c(~0, "Hello", strlen("Hello"))
            & 0xFFFF);
    printf("Subdir entry key:%x\n", subdir->entry_key);
    subdir->type = ENTRY_TYPE_DIRECTORY;
    subdir->cached_dir = dirs_mem++;
#endif
//    printf("unmatched part: '%s'\n", begin_segment);
 
    return 0;
}


/// Adds or updates a file
meta_data_entry_t* AddFile(cache_t* cache, const char* full_path,
                            const void* content, uint32_t content_size)
{
    // Take_toc_lock

    uint32_t toc_size = cache->toc_size;
    uint16_t path_length = (uint16_t)strlen(full_path);

    meta_data_entry_t* result
        = LookupPath(cache, full_path, path_length);

    if (!result)
    {
        result = CreateEntry(cache, full_path, path_length);
        result->type = ENTRY_TYPE_FILE;
    }
    assert(result->type == ENTRY_TYPE_FILE);
}
#if 0

#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv)
{
    uint32_t initial_name_storage_capacity = 8192;
    uint32_t initial_name_nodes_capacity = 256;
    uint32_t initial_metadata_nodes = 256;
    uint32_t initial_dir_nodes = 256;

    uint8_t* cache_memory = (uint8_t*) malloc(
        sizeof(cache_t) + initial_name_storage_capacity
                        + (initial_name_nodes_capacity *
                            sizeof(name_cache_node_t)));

    name_cache_node_t* tree_mem = (name_cache_node_t*)
                                (cache_memory + sizeof(cache_t)
                                + initial_name_storage_capacity);

    meta_data_entry_t* meta_mem = (meta_data_entry_t*) calloc(
        initial_metadata_nodes , sizeof(meta_data_entry_t));

    cached_dir_t* dirs_mem = (cached_dir_t*)calloc(
        initial_dir_nodes, sizeof(cached_dir_t));

    cache_t cache = {
        .toc_entries = 0,
        .root = meta_mem++,

        .toc_size = 0,
        .toc_capacity = 0,

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

    cache.root->cached_dir->n_entires = 1;
    meta_data_entry_t* subdir = 
        cache.root->cached_dir->entires =
            cache.root + cache.metadata_size++;

    subdir->name = GetOrAddName(&cache, "Hello");
    subdir->entry_key = 
        (strlen("Hello") << 16) | (crc32c(~0, "Hello", strlen("Hello"))
            & 0xFFFF);
    printf("Subdir entry key:%x\n", subdir->entry_key);    
    subdir->type = ENTRY_TYPE_DIRECTORY;
    subdir->cached_dir = dirs_mem++;


    CreateEntry(&cache, "Hello/World", strlen("Hello/World"));

    name_cache_ptr_t w = GetOrAddName(&cache, "William");
    printf("W NameCachePtr: %u\n", w.v);
    name_cache_ptr_t c = GetOrAddName(&cache, "Christoph");
    printf("C NameCachePtr: %u\n", c.v);
    name_cache_ptr_t w2 = GetOrAddName(&cache, "William");
    printf("W2 NameCachePtr: %u\n", w2.v);

    printf("\n\n\t sizeof(cache_t): %d\n", sizeof(cache_t));
}

#endif
