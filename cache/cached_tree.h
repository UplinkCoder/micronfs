#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "crc32.c"

struct cached_file_t;
struct cached_dir_t;

typedef struct name_cache_ptr_t
{
    uint32_t v;
} name_cache_ptr_t;

typedef struct name_cache_node_t
{
    uint32_t entry_key;
    int32_t left;
    int32_t right;

    name_cache_ptr_t name_ptr;
} name_cache_node_t;

/// metadata which doesn't change often
typedef struct meta_data_entry_t
{
    name_cache_ptr_t name;
    uint16_t name_length;
    uint16_t lower_crc32;
    uint8_t type; /// nfs type3

    union
    {
        struct cached_file_t* cached_file;
        struct cached_dir_t* cached_dir;
    };
} meta_data_entry_t;

/// contains cached data which is likely to change
typedef struct cached_file_t
{
    uint32_t crc32; /// crc32_hash of the cached file
    uint32_t mtime; /// remote mtime at point of caching
    uint32_t last_checked_time; /// when has the last sync with remote happend
    uint32_t size; /// size of the cached data

    void* data;
} cached_file_t;

/// contains cached data which is likely to change
typedef struct cached_dir_t
{
    uint32_t crc32; /// mixed_file_hashes of all the content
    uint32_t mtime; /// remote mtime at point of caching
    uint32_t last_checked_time; /// when has the last sync with remote happend
    uint32_t n_entires; /// how many entires the directory has

    meta_data_entry_t* entires;
} cached_dir_t;

typedef enum entry_type_t {
    ENTRY_TYPE_NONE,
    ENTRY_TYPE_FILE,
    ENTRY_TYPE_DIRECTORY,
    ENTRY_TYPE_MAX,
} entry_type_t;

typedef struct toc_entry_t
{
    union
    {
        struct {
            uint16_t lower_crc32;
            uint16_t path_length;
        };
        uint32_t entry_key;
    };
    name_cache_ptr_t relative_name_pointer;

    meta_data_entry_t* entry;
} toc_entry_t;

typedef struct cache_t
{
    toc_entry_t* toc_entries;
    meta_data_entry_t* root;

    uint32_t toc_size;
    uint32_t toc_capacity;

    uint32_t metadata_size;
    uint32_t metadata_capacity;

    char* name_cache;
    uint32_t name_cache_size;
    uint32_t name_cache_capacity;

    /// the root has the virtual value of 7fff
    /// which means whenever we get hash is to 7fff we store it here
    name_cache_node_t* name_cache_root;
    uint32_t name_cache_node_size;
    uint32_t name_cache_node_capacity;
} cache_t;

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
                toc_entry->relative_name_pointer.v + cache->name_cache;
            if (!memcmp(entry_path, full_path, path_length))
            {
                result = toc_entry->entry;
                break;
            }
        }
    }

    return result;
}

name_cache_ptr_t GetOrAddNameLength(cache_t* cache, const char* name,
                                    size_t length);

name_cache_ptr_t GetOrAddName(cache_t* cache, const char* name)
{
    return GetOrAddNameLength(cache, name, strlen(name));
}

name_cache_ptr_t GetOrAddNameLength(cache_t* cache, const char* name,
                                    size_t length)
{
    assert(length <= 0xFFFF);

    uint32_t crcName = crc32c(~0, name, length);
    uint32_t entry_key = (crcName & 0xFFFF | ((uint16_t)length) << 16);

    name_cache_node_t* lastVisited = 0;

    for(name_cache_node_t* currentBranch = cache->name_cache_root;
        currentBranch;
        lastVisited = currentBranch)
    {
        int cmp_result = entry_key - currentBranch->entry_key;
        if (!cmp_result)
        {
            const char* cached_name = currentBranch->name_ptr.v
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

                name_cache_ptr_t name_ptr = { cache->name_cache_size };
                cache->name_cache_size += length + 1;

                *nextBranch =
                    (name_cache_node_t){entry_key, 0, 0, name_ptr};
                return nextBranch->name_ptr;
            }
        }
    }

    assert(0);
}

meta_data_entry_t* CreateEntry(cache_t* cache, const char* full_path,
                               uint16_t path_length)
{
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

#if TEST_CACHE
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv)
{
    int initial_name_storage_capacity = 8192;
    int initial_name_nodes_capacity = 16;
    uint8_t* cache_memory = (uint8_t*) malloc(
        sizeof(cache_t) + initial_name_storage_capacity
                        + (initial_name_nodes_capacity *
                            sizeof(name_cache_node_t)));

    name_cache_node_t* tree_mem = (name_cache_node_t*)
                                (cache_memory + sizeof(cache_t)
                                + initial_name_storage_capacity);
    cache_t cache = {
        .toc_entries = 0,
        .root = 0,

        .toc_size = 0,
        .toc_capacity = 0,

        .metadata_size = 0,
        .metadata_capacity = 0,

        .name_cache  = (char*)(cache_memory + sizeof(cache_t)),
        .name_cache_size = 0,
        .name_cache_capacity = initial_name_storage_capacity,

        .name_cache_root = tree_mem,
        .name_cache_node_size = 1,
        .name_cache_node_capacity = initial_name_nodes_capacity
    };

    name_cache_ptr_t w = GetOrAddName(&cache, "William");
    printf("W NameCachePtr: %u\n", w.v);
    name_cache_ptr_t c = GetOrAddName(&cache, "Christoph");
    printf("C NameCachePtr: %u\n", c.v);
    name_cache_ptr_t w2 = GetOrAddName(&cache, "William");
    printf("W2 NameCachePtr: %u\n", w2.v);
}
#endif
