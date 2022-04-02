#ifdef _WIN32
#  include <io.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  include "stdint_msvc.h"
#else
#  define _BSD_SOURCE 1
#  define _DEFAULT_SOURCE 1
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <sys/mman.h>
#endif

#ifndef _cpluslplus
# ifndef _WIN32
#  include <stdbool.h>
# else
#  define bool int
# endif
#endif

#include <stdio.h>
#include <sys/types.h>

#include "cache/cached_tree.h"
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>
#include <errno.h>

struct sockaddr_in server;
struct sockaddr_in m_client;
struct sockaddr_in s_client;

#include "srv.h"
#include "micronfs.h"
#include "rpc_serializer.h"

#ifndef MAP_UNINITIALIZED
#  define MAP_UNINITIALIZED 0
#endif
#define MAX_BUFFER_SIZE 4096

static char recvbuffer[MAX_BUFFER_SIZE];

/// connects to a name on a given port
/// returns -1 if it fails
int connect_name(const char* hostname, const char* port)
{
   struct addrinfo* addr = 0;

    int sfd = -1;
    if (getaddrinfo(hostname, port, 0, &addr) != 0)
    {
        perror("getaddrinfo failed ");
    }

    for (struct addrinfo* rp = addr; rp != NULL; rp = rp->ai_next) {
           sfd = socket(rp->ai_family, rp->ai_socktype,
                        rp->ai_protocol);
        if (sfd == -1)
           continue;

       if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
       {
            break;             /* Success */
       }
    }

    return sfd;
}


void PushUnixAuthN(RPCSerializer* self)
{
    uint32_t aux_gids[1] = {0};

    RPCSerializer_PushUnixAuth(self, 0, "", 0, 0, 1, aux_gids);
}


#define PORTMAP_PROGRAM         100000
#define PORTMAP_DUMP_PROCEDURE       4
#define PORTMAP_DUMP_GETPORT         3

#define MOUNT_PROGRAM           100005
#define MOUNT_MNT_PROCEDURE          1
#define MOUNT_DUMP_PROCEDURE         2
#define MOUNT_UMNT_PROCEDURE         3
#define MOUNT_EXPORT_PROCEDURE       5

#define NFS_PROGRAM             100003
#define NFS_READ_PROCEDURE           6
#define NFS_WRITE_PROCEDURE          7
#define NFS_CREATE_PROCEDURE         8
#define NFS_READDIR_PROCEDURE       16
#define NFS_READDIRPLUS_PROCEDURE   17
#define MESSAGE_TYPE_CALL 0
#define PROTO_TCP 6


void InitCache(cache_t* cache)
{
    uint32_t initial_name_storage_capacity = 65536;
    uint32_t initial_name_nodes_capacity = 4096;
    uint32_t initial_files_capacity = 16384;

    uint32_t initial_metadata_nodes = 16384 * 8;
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

    ResetCache(cache);

    cache->root->cached_dir = cache->dir_entries + cache->dir_entries_size++;
    cache->root->cached_dir->fullPath = GetOrAddName(cache, "/");
}

void portmap_nullCall(int sock_fd)
{
    RPCSerializer s = {0};

    RPCSerializer_InitCall(&s, PREP_RPC_CALL
        (PORTMAP_PROGRAM, 2, 0));
    RPCSerializer_PushNullAuth(&s);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, sock_fd);
}

uint16_t portmap_getport(int sock_fd,
    uint32_t program, uint32_t version_, uint32_t proto)
{
        RPCSerializer s = {0};

    RPCSerializer_InitCall(&s,
        PREP_RPC_CALL(PORTMAP_PROGRAM, 2, PORTMAP_DUMP_GETPORT));

    RPCSerializer_PushNullAuth(&s);

    // arguments to getport
    RPCSerializer_PushU32(&s, program);
    RPCSerializer_PushU32(&s, version_);
    RPCSerializer_PushU32(&s, proto);
    RPCSerializer_PushU32(&s, 0);

    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, sock_fd);

    RPCDeserializer d = {0};
    RPCDeserializer_Init(&d, sock_fd);
    RPCHeader header =
        RPCDeserializer_RecvHeader(&d);


    bool accepted = RPCDeserializer_ReadBool(&d);

    RPCDeserializer_SkipAuth(&d);

    nfsstat3 status = RPCDeserializer_ReadU32(&d);
    uint32_t port = RPCDeserializer_ReadU32(&d);
    assert(port <= UINT16_MAX);

    return port;
}

typedef struct mountlist_t {
    const char* hostname;
    const char* directory;
    struct mountlist_t* next;
} mountlist_t;


void printFileHandle(const fhandle3* handle)
{
    const uint32_t* ptr = (const uint32_t*)
        &handle->fhandle3[0];

    printf("fh: %x %x %x %x %x %x %x %x\n",
        ptr[0], ptr[1], ptr[2], ptr[3],
        ptr[4], ptr[5], ptr[6], ptr[7]);
}

void mountd_umnt(int mountd_fd, const char* dirPath)
{
    RPCSerializer s = {0};
    mountlist_t* result = 0;

    uint32_t mount_dump_xid =
        RPCSerializer_InitCall(&s,
            PREP_RPC_CALL(MOUNT_PROGRAM, 3, MOUNT_UMNT_PROCEDURE));

    RPCSerializer_PushNullAuth(&s);
    uint32_t dirPathLength = strlen(dirPath);

    RPCSerializer_PushString(&s, dirPathLength, dirPath);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, mountd_fd);

    // not sure if I should issue a read since it's a void proc
}

fhandle3 mountd_mnt(int mountd_fd, const char* dirPath)
{
    RPCSerializer s = {0};
    fhandle3 result = {{0}};

    uint32_t mount_dump_xid = RPCSerializer_InitCall(&s,
        PREP_RPC_CALL(MOUNT_PROGRAM, 3, MOUNT_MNT_PROCEDURE));

    //RPCSerializer_PushNullAuth(&s);
    PushUnixAuthN(&s);

    uint32_t dirPathLength = strlen(dirPath);

    RPCSerializer_PushString(&s, dirPathLength, dirPath);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, mountd_fd);

    RPCDeserializer d = {0};
    RPCDeserializer_Init(&d, mountd_fd);

    RPCDeserializer_RecvHeader(&d);

    int reply_accepted = RPCDeserializer_ReadBool(&d);

    RPCDeserializer_SkipAuth(&d);

    int rpc_accepted = RPCDeserializer_ReadBool(&d);

    nfsstat3 status = RPCDeserializer_ReadU32(&d);

    result = RPCDeserializer_ReadFileHandle(&d);
    //TODO we should ready the required auth here .... maybe

    return result;

}

mountlist_t* mountd_dump(int mountd_fd)
{
    RPCSerializer s = {0};
    mountlist_t* result = 0;

    uint32_t mount_dump_xid =
        RPCSerializer_InitCall(&s,
            PREP_RPC_CALL(MOUNT_PROGRAM, 3, MOUNT_DUMP_PROCEDURE));

    RPCSerializer_PushNullAuth(&s);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, mountd_fd);

    RPCDeserializer d;
    RPCDeserializer_Init(&d, mountd_fd);
    RPCHeader header = RPCDeserializer_RecvHeader(&d);

    bool accepted = RPCDeserializer_ReadBool(&d);

    RPCDeserializer_SkipAuth(&d);

    nfsstat3 status = RPCDeserializer_ReadU32(&d);
    // now the actual params come ...

    static char mountlist_storage[8192];
    char* writePtr = mountlist_storage;
    uint32_t storage_left = sizeof(mountlist_storage);

    bool hadPrev = 0;
    if (status == 0)
    {
        bool hasNext = RPCDeserializer_ReadBool(&d);
        mountlist_t* entry = 0;
        if (hasNext)
        {
            result = entry = (mountlist_t*) mountlist_storage;
        }

        while(hasNext)
        {
            if (hadPrev)
            {
                entry->next = (mountlist_t*)
                    writePtr;
            }

            entry = (mountlist_t*)
                writePtr;
            writePtr += sizeof(mountlist_t);
            storage_left -= sizeof(mountlist_t);

            {
                const uint32_t hlen = RPCDeserializer_ReadU32(&d);
                assert(storage_left > hlen + 1);
                entry->hostname = RPCDeserializer_ReadString(&d, &writePtr, hlen);
                storage_left -= (hlen + 1);
            }
            {
                uint32_t dlen = RPCDeserializer_ReadU32(&d);
                assert(storage_left > dlen - 1);
                entry->directory = RPCDeserializer_ReadString(&d, &writePtr, dlen);
                storage_left -= (dlen + 1);
            }
            hasNext = RPCDeserializer_ReadBool(&d);
            hadPrev = 1;
        }
    }
    else
    {
        printf("Error: %s\n", nfsstat3_toChars(status));
    }

    return result;
}


int64_t nfs_read(SOCKET nfs_fd, const fhandle3* file
               , void* data, uint32_t size
               , uint64_t offset)
{
    RPCSerializer s = {0};

    uint32_t read_xid = RPCSerializer_InitCall(&s,
        PREP_RPC_CALL(NFS_PROGRAM, 3, NFS_READ_PROCEDURE));

    PushUnixAuthN(&s);


    int length = fhandle3_length(file);
    RPCSerializer_PushString(&s, length, (const char*)file->fhandle3);
    RPCSerializer_PushU64(&s, offset);
    RPCSerializer_PushU32(&s, size);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, nfs_fd);
    // ----------------------------------------------
    RPCDeserializer d = {0};
    RPCDeserializer_Init(&d, nfs_fd);

    RPCHeader header = RPCDeserializer_RecvHeader(&d);

    assert(header.xid == read_xid);

    int accepted = RPCDeserializer_ReadBool(&d);
    RPCDeserializer_SkipAuth(&d);
    int accept_state = RPCDeserializer_ReadBool(&d);

    nfsstat3 status = RPCDeserializer_ReadU32(&d);
    if (status != 0) printf("Status: %s\n", nfsstat3_toChars(status));
    // -----------------------------------------------------

    if (status)
    {
        fprintf(stderr, "Error [%s] while reading '%s'\n"
             , nfsstat3_toChars(status)
             , "" /*LookupNameInCache(file)*/
        );
        return -1;
    }

    //TODO FIXME make sure size if less than rtMax form FSINFO Query!
    if (RPCDeserializer_ReadU32(&d) != 0)
    {
        (void) RPCDeserializer_ReadFileAttribs(&d);
    }
    uint32_t result_count = RPCDeserializer_ReadU32(&d);
    int eof = RPCDeserializer_ReadU32(&d) != 0;
    uint32_t arraySize = RPCDeserializer_ReadU32(&d);
    uint32_t bufferLeft = RPCDeserializer_BufferLeft(&d);
    
    uint32_t readAlready = 0;
    while(bufferLeft < arraySize - readAlready)
    {
        // RPCDeserializer_EnsureSize(&d, bufferLeft);
        memcpy(data + readAlready, d.ReadPtr, bufferLeft);
        readAlready += bufferLeft;
        d.ReadPtr += (ALIGN4(bufferLeft) / 4);
        RPCDeserializer_EnsureSize(&d, 4);
        bufferLeft = RPCDeserializer_BufferLeft(&d);
    }

    if (bufferLeft >= arraySize - readAlready)
    {
        memcpy(data + readAlready, d.ReadPtr, bufferLeft);
    }
    else
    {
        assert(0);
        while (arraySize)
        {
         //   RPCDeserializer_EnsureSize(&d, )
        }
    }

    return result_count;
}

int nfs_readdirplus(SOCKET nfs_fd, const fhandle3* dir
               , uint64_t *cookie, uint64_t *cookieverf
               , int (*fileIter)(const char* fName, const fhandle3* handle,
                                 const fattr3* attribs,
                                 void* userData)
               , void* userData)
{
    RPCSerializer s = {0};

    mountlist_t* result = 0;

    uint32_t readdirplus_xid = RPCSerializer_InitCall(&s,
            PREP_RPC_CALL(NFS_PROGRAM, 3, NFS_READDIRPLUS_PROCEDURE));

    PushUnixAuthN(&s);

    int length = fhandle3_length(dir);
    RPCSerializer_PushString(&s, length, (const char*)dir->fhandle3);

    uint32_t cookie_hi = *cookie >> 32;
    uint32_t cookie_lw = *cookie & UINT32_MAX;

    uint32_t cookie_verif_hi = *cookieverf >> 32;
    uint32_t cookie_verif_lw = *cookieverf & UINT32_MAX;

    RPCSerializer_PushU32(&s, cookie_hi);
    RPCSerializer_PushU32(&s, cookie_lw);

    RPCSerializer_PushU32(&s, cookie_verif_hi);
    RPCSerializer_PushU32(&s, cookie_verif_lw);

    RPCSerializer_PushU32(&s, 4096);
    // max size of attribs ... it's recommeded that that's shorter than max size

    RPCSerializer_PushU32(&s, 32768); // max size of result structure

    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, nfs_fd);
    // --------------------------------------------------------------

    RPCDeserializer d = {0};
    RPCDeserializer_Init(&d, nfs_fd);

    RPCHeader header = RPCDeserializer_RecvHeader(&d);

    assert(header.xid == readdirplus_xid);

    int accepted = RPCDeserializer_ReadBool(&d);  //16
    RPCDeserializer_SkipAuth(&d);
    int accept_state = RPCDeserializer_ReadBool(&d);

    nfsstat3 status = RPCDeserializer_ReadU32(&d);
    if (status != 0) printf("Status: %s\n", nfsstat3_toChars(status));
    // -------------------------------------------------------------------

    int hasAttrs = RPCDeserializer_ReadBool(&d);
    if (hasAttrs)
    {
        RPCDeserializer_ReadFileAttribs(&d);
    }

    *cookieverf = RPCDeserializer_ReadU64(&d);
    cookie3 lastCookie;
    // ---------------------------------------------------------------------
    int hasNext = RPCDeserializer_ReadBool(&d);
    int shouldContinueReading;
    while (hasNext)
    {
        RPCDeserializer_EnsureSize(&d, 12);
        char name_buffer[1024];
        char* namePtr = name_buffer;
        uint64_t fileid  = RPCDeserializer_ReadU64(&d);
        uint32_t name_length = RPCDeserializer_ReadU32(&d);

        // the name might be longer than our buffer can hold
        RPCDeserializer_EnsureSize(&d, ALIGN4(name_length));
        const char* name  = RPCDeserializer_ReadString(&d, &namePtr, name_length);
        uint32_t afterNameBufferLeft = RPCDeserializer_BufferLeft(&d);

        RPCDeserializer_EnsureSize(&d, 12);
        lastCookie = RPCDeserializer_ReadU64(&d);

        const fattr3* attribsPtr = 0;
        uint32_t bufferLeftBeforeAttribs;
        if (RPCDeserializer_ReadBool(&d))
        {
            bufferLeftBeforeAttribs = RPCDeserializer_BufferLeft(&d);
            const fattr3 attribs = RPCDeserializer_ReadFileAttribs(&d);
            attribsPtr = &attribs;
        }

        const fhandle3* handlePtr = 0;
        RPCDeserializer_EnsureSize(&d, 4);
        if (RPCDeserializer_ReadBool(&d))
        {
            const fhandle3 handle = RPCDeserializer_ReadFileHandle(&d);
            handlePtr = &handle;
        }

        if (!fileIter(name, handlePtr, attribsPtr, userData))
        {
            *cookie = lastCookie;
            shouldContinueReading = 0;
            // Flush out recv queue;
            while(((int)d.FragmentSizeLeft) > 0)
            {
                (*(int8_t**)&d.ReadPtr) += RPCDeserializer_BufferLeft(&d);
                assert(RPCDeserializer_BufferLeft(&d) == 0);
                int maxQuerySize = d.MaxBuffer;
                if(d.FragmentSizeLeft < maxQuerySize)
                    maxQuerySize = d.FragmentSizeLeft;
                RPCDeserializer_EnsureSize(&d, maxQuerySize);
            }
            goto Lreturn;
        }
        RPCDeserializer_EnsureSize(&d, 4);
        hasNext = RPCDeserializer_ReadBool(&d);
    }
    // printf("Writing lastCookie into ptr");
    *cookie = lastCookie;
    RPCDeserializer_EnsureSize(&d, 4);
    shouldContinueReading = !RPCDeserializer_ReadBool(&d);
Lreturn:
    return shouldContinueReading;

}

int nfs_readdir(int nfs_fd, const fhandle3* dir
               , uint64_t *cookie, uint64_t *cookieverf
               , int (*dirIter)(const char* fName, uint64_t fileId) )
{
    RPCSerializer s = {0};

    mountlist_t* result = 0;

    uint32_t readdir_xid = RPCSerializer_InitCall(&s,
            PREP_RPC_CALL(NFS_PROGRAM, 3, NFS_READDIR_PROCEDURE));

    PushUnixAuthN(&s);


    int length = fhandle3_length(dir);
    RPCSerializer_PushString(&s, length, (const char*)dir->fhandle3);

    uint32_t cookie_hi = *cookie >> 32;
    uint32_t cookie_lw = *cookie & UINT32_MAX;

    uint32_t cookie_verif_hi = *cookieverf >> 32;
    uint32_t cookie_verif_lw = *cookieverf & UINT32_MAX;

    RPCSerializer_PushU32(&s, cookie_hi);
    RPCSerializer_PushU32(&s, cookie_lw);

    RPCSerializer_PushU32(&s, cookie_verif_hi);
    RPCSerializer_PushU32(&s, cookie_verif_lw);

    RPCSerializer_PushU32(&s, 2048); // max size of result structure

    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, nfs_fd);

    RPCDeserializer d;
    RPCDeserializer_Init(&d, nfs_fd);

    const RPCHeader header = RPCDeserializer_RecvHeader(&d);
    int accepted = RPCDeserializer_ReadBool(&d);  //16
    RPCDeserializer_SkipAuth(&d);
    int accept_state = RPCDeserializer_ReadBool(&d);
    nfsstat3 status = RPCDeserializer_ReadU32(&d);

    if (status != 0) printf("Status: %s\n", nfsstat3_toChars(status));

    bool hasAttrs = RPCDeserializer_ReadBool(&d);
    if (hasAttrs)
    {
        RPCDeserializer_ReadFileAttribs(&d);
    }

    *cookieverf = RPCDeserializer_ReadU64(&d);
    cookie3 lastCookie;
    for(;;)
    {
        bool hasNext = RPCDeserializer_ReadBool(&d);
        if (!hasNext)
            break;
        fileid3 fileId = RPCDeserializer_ReadU64(&d);

        uint32_t name_length = RPCDeserializer_ReadU32(&d);

        char str_buf[1024];
        char* writePtr = str_buf;

        const char* fname = RPCDeserializer_ReadString(&d, &writePtr, name_length);

        lastCookie = RPCDeserializer_ReadU64(&d);
        if (!dirIter(fname, fileId))
            break;
    }
    bool wasLastList = RPCDeserializer_ReadBool(&d);

    return !wasLastList;
    // printf("%x %x %x %x", *readPtr++, *readPtr++, *readPtr++, *readPtr++);

}

#ifndef _WIN32
#  define INVALID_SOCKET -1
#endif

struct search_dir_t
{
    const char* name;
    fhandle3 result_handle;
};

int searchDir_cb(const char* fName, const fhandle3* handle,
                 const fattr3* attribs, void* userData)
{
    struct search_dir_t *search_req = (struct search_dir_t*) userData;
    // printf("name: %s [%d] {%s}\n", fName, attribs->size, ftype3_toChars(attribs->type));
    if (0 == strcmp(search_req->name, fName))
    {
        assert(handle);
        search_req->result_handle = *handle;
        return 0;
    }
    return 1;
}

typedef struct populate_cache_cb_args_t
{
    cache_t* cache;
    meta_data_entry_t* parentDir;
} populate_cache_cb_args_t;

int populateCache_cb(const char* fName, const fhandle3* handle,
                     const fattr3* attribs, void* userData)
{
    populate_cache_cb_args_t* args =
        (populate_cache_cb_args_t*) userData;

    cache_t* cache = args->cache;
    meta_data_entry_t* parentDir = args->parentDir;

    const uint32_t len = strlen(fName);
    meta_data_entry_t* entry = 0;

    if (attribs)
    {
        if (attribs->type == NF3DIR)
        {
            if (!parentDir->cached_dir)
            {
                parentDir->cached_dir = cache->dir_entries + cache->dir_entries_size++;
            }
            entry = GetOrCreateSubdirectory(cache, parentDir->cached_dir, fName, len);
            if (
                   (fName[0] != '.' && fName[1] != '\0')
                && (fName[0] != '.' && fName[1] != '.' && fName[2] != '\0')
            )
            {
                // printf("reading dir: %s\n", fName);
                uint64_t cookie = 0;
                uint64_t verifier = 0;
                populate_cache_cb_args_t newArgs = {
                    args->cache, entry
                };

                SOCKET newSock = connect_name("192.168.178.26", "2049");
                nfs_readdirplus(newSock, handle, &cookie, &verifier
                  , populateCache_cb, &newArgs);
                close(newSock);
            }
        }
        else if (attribs->type == NF3REG)
        {
            const uint32_t entry_key = EntryKey(fName, len);
            entry = CreateEntryInDirectoryByKey(cache, parentDir->cached_dir, fName, entry_key);
            entry->type = ENTRY_TYPE_FILE;
            entry->cached_file = cache->file_entries + cache->file_entries_size++;
            entry->cached_file->crc32 = 0;
            entry->cached_file->data = 0;
            entry->cached_file->size = attribs->size;
        }
        else
        {
            printf("Unexpected type: %s on file: %s\n", ftype3_toChars(attribs->type), fName);
            return 1;
        }
    }
    else
    {
        printf("No attribs for: %s\n", fName);
    }
    if (handle)
    {
        entry->handle = handleToPtr(cache, handle);
    }

    return 1;
}

uint32_t handleSum(const fhandle3* handle)
{
    uint32_t sum = 0;

    for(int i = 0; i < fhandle3_length(handle); i++)
    {
        sum += handle->fhandle3[i];
    }

    return sum;
}

