#include <stdint.h>
#include <string.h>
#include "crc32.c"

struct cached_file_t;
struct cached_dir_t;

/// metadata which doesn't change often
typedef struct meta_data_entry_t
{
    const char* name; //name of the file-system entry
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
    uint32_t size; /// size of the cached data
    uint32_t crc32; /// crc32_hash of the cached file
    uint32_t mtime; /// remote mtime at point of caching
    uint32_t last_checked_time; /// when has the last sync with remote happend

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
    uint32_t relative_name_pointer;

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
} cache_t;

meta_data_entry_t* LookupPath(cache_t* cache, const char* full_path,
                                uint16_t path_length)
{
    uint32_t toc_size = cache->toc_size;
    uint16_t lower_crc = (uint16_t) crc32c(~0, full_path, path_length);
    uint32_t entry_key =  (path_length << 16) | lower_crc;
    meta_data_entry_t* result = 0;

    toc_entry_t* one_past_last = cache->toc_entries + toc_size;
    for(toc_entry_t* toc_entry = cache->toc_entries;
        toc_entry < one_past_last; toc_entry++)
    {
        if (toc_entry->entry_key == entry_key)
        {
            const char* entry_path = 
                toc_entry->relative_name_pointer + cache->name_cache;
            if (!memcmp(entry_path, full_path, path_length))
            {
                result = toc_entry->entry;
                break;
            }
        }
    }

    return result;
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
}
