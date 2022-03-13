#ifdef _WIN32
#  include <io.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  include "stdint_msvc.h"
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <byteswap.h>
#  include <sys/mman.h>
#  define _BSD_SOURCE 1
#endif

#include <stdio.h>
#include <sys/types.h>


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

#define HTON_RPC(RPC_STRUCT) \
    ByteFlip_Array((u32*)&(RPC_STRUCT), sizeof((RPC_STRUCT)) / sizeof(u32));

typedef struct RPCService {
    uint32_t programm_id;
    uint32_t version_;
    uint32_t network_id_length;
    char* network_id_ptr;
} RPCService;

typedef const uint32_t ** readPtrP_t;


#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0
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


typedef struct portmap_dump_res {
    u32 program;
    u32 version_;
    u32 proto;
    u32 port;
    struct portmap_dump_res* next;
} portmap_dump_res;


void PushUnixAuthN(RPCSerializer* self)
{
    RPCSerializer_PushUnixAuth(self, 0, "", 0, 0, 1, (uint32_t[1]){0});
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
#define NFS_READDIR_PROCEDURE       16
#define NFS_READDIRPLUS_PROCEDURE   17
#define MESSAGE_TYPE_CALL 0
#define PROTO_TCP 6



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


    _Bool accepted = RPCDeserializer_ReadBool(&d);

    RPCDeserializer_SkipAuth(&d);


    nfsstat3 status = RPCDeserializer_ReadU32(&d);
    uint32_t port = RPCDeserializer_ReadU32(&d);
    assert(port <= UINT16_MAX);

    return port;
}

portmap_dump_res* portmap_dump(int sock_fd)
{
    static portmap_dump_res pm_dump_storage[6];
    portmap_dump_res* result =  (portmap_dump_res*)
        pm_dump_storage;

    RPCSerializer s = {0};

    RPCSerializer_InitCall(&s,
        PREP_RPC_CALL(PORTMAP_PROGRAM, 2, PORTMAP_DUMP_PROCEDURE));

    RPCSerializer_PushNullAuth(&s);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, sock_fd);

    RPCDeserializer d = {0};

    RPCDeserializer_Init(&d, sock_fd);
    RPCHeader header =
        RPCDeserializer_RecvHeader(&d);


    _Bool accepted = RPCDeserializer_ReadBool(&d);

    RPCDeserializer_SkipAuth(&d);


    nfsstat3 status = RPCDeserializer_ReadU32(&d);

    printf("Status: %s\n", nfsstat3_toChars(status));
    // now the actual params come ...

    _Bool hasNext =  RPCDeserializer_ReadBool(&d);
    _Bool hadPrev = 0;
    portmap_dump_res* entry = result;
    while(hasNext)
    {
        portmap_dump_res r = {
            .program = RPCDeserializer_ReadU32(&d),
            .version_ = RPCDeserializer_ReadU32(&d),
            .port = RPCDeserializer_ReadU32(&d),
            .proto = RPCDeserializer_ReadU32(&d)
        };
        // printf("read a record\n");
        hasNext = RPCDeserializer_ReadBool(&d);
        if (r.program == MOUNT_PROGRAM
            && r.version_ == 3
            && r.proto == 6)
        {
            if (hadPrev)
            {
                entry->next = entry + 1;
                ++entry;
            }
            else
                hadPrev = 1;

            *entry = r;
        }
    }

    return result;
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

    _Bool accepted = RPCDeserializer_ReadBool(&d);

    RPCDeserializer_SkipAuth(&d);

    nfsstat3 status = RPCDeserializer_ReadU32(&d);
    // now the actual params come ...

    static char mountlist_storage[8192];
    char* writePtr = mountlist_storage;
    uint32_t storage_left = sizeof(mountlist_storage);

    _Bool hadPrev = 0;
    if (status == 0)
    {
        _Bool hasNext = RPCDeserializer_ReadBool(&d);
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

static inline uint64_t ReadU64(const uint32_t ** readPtrP)
{
    const uint32_t * readPtr = *readPtrP;

    const uint32_t rd_hi = *readPtr++;
    const uint32_t hi = htonl(rd_hi);
    const uint32_t rd_lw = *readPtr++;
    const uint32_t lw = htonl(rd_lw);


    uint64_t result = hi;
    result <<= 32;
    result |= lw;

    // printf("hi: %x, lw: %x, res: %llx\n", hi, lw, result);

    *readPtrP = readPtr;
}

static inline uint32_t fhandle3_length(const fhandle3* handle)
{
    uint32_t length = 0;

    for(int i = 0; i < 8;i++)
    {
        if (((uint32_t*)handle->fhandle3)[i] == 0)
            break;
        length += 4;
    }

    return length;
}

int nfs_readdirplus(int nfs_fd, const fhandle3* dir
               , uint64_t *cookie, uint64_t *cookieverf
               , int (*fileIter)(const char* fName, uint64_t fileId,
                                 const fhandle3* handle, const fattr3* attribs) )
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

    RPCSerializer_PushU32(&s, 3000);
    // max size of attribs ... it's recommeded that that's shorter than max size

    RPCSerializer_PushU32(&s, 4000); // max size of result structure

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
    printf("Status: %s\n", nfsstat3_toChars(status));
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
    while (hasNext)
    {
        char name_buffer[1024];
        char* namePtr = name_buffer;
        uint64_t fileid  = RPCDeserializer_ReadU64(&d);
        uint32_t name_length = RPCDeserializer_ReadU32(&d);
        const char* name  = RPCDeserializer_ReadString(&d, &namePtr, name_length);
        lastCookie = RPCDeserializer_ReadU64(&d);
        const fattr3* attribsPtr = 0;
        if (RPCDeserializer_ReadBool(&d))
        {
            const fattr3 attribs = RPCDeserializer_ReadFileAttribs(&d);
            attribsPtr = &attribs;
        }
        const fhandle3* handlePtr = 0;
        if (RPCDeserializer_ReadBool(&d))
        {
            const fhandle3 handle = RPCDeserializer_ReadFileHandle(&d);
            handlePtr = &handle;
        }

        if (!fileIter(name, fileid, handlePtr, attribsPtr))
        {
            *cookie = lastCookie;
            break;
        }
        hasNext = RPCDeserializer_ReadBool(&d);
    }
    // printf("Writing lastCookie into ptr");
    *cookie = lastCookie;
    return RPCDeserializer_ReadBool(&d);

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

    printf("Status: %s\n", nfsstat3_toChars(status));

    _Bool hasAttrs = RPCDeserializer_ReadBool(&d);
    if (hasAttrs)
    {
        RPCDeserializer_ReadFileAttribs(&d);
    }

    *cookieverf = RPCDeserializer_ReadU64(&d);
    cookie3 lastCookie;
    for(;;)
    {
        _Bool hasNext = RPCDeserializer_ReadBool(&d);
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
    _Bool wasLastList = RPCDeserializer_ReadBool(&d);

    return !wasLastList;
    // printf("%x %x %x %x", *readPtr++, *readPtr++, *readPtr++, *readPtr++);

}

#ifndef _WIN32
#  define INVALID_SOCKET -1
#endif

int myPlusCallBack(const char* fName, uint64_t fileId,
                   const fhandle3* handle, const fattr3* attribs)
{
    printf("name: %s, fileid %ull \n", fName, fileId);
    return 1;
}

int myCallBack(const char* filename, uint64_t fileId)
{
    return 1;
}

int main(int argc, char* argv[])
{
#ifdef _WIN32
	WSADATA  wsaData;
	WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
    char* hostname = "192.168.178.26";
    if (argc == 2)
        hostname = argv[1];

    /* Create socket. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock == INVALID_SOCKET) {
#ifdef _WIN32
	printf("WSAGetLastError: %d\n", WSAGetLastError());
#else
      perror("opening stream socket");
#endif
      exit(1);
    }

    int addr_len = sizeof(s_client);

    int portmap_fd = connect_name(hostname, "111");
    assert(portmap_fd != -1);

    // portmap_nullCall(portmap_fd);

    uint16_t mountd_port
        = portmap_getport(portmap_fd, MOUNT_PROGRAM, 3, PROTO_TCP);

    if (!mountd_port)
    {
        fprintf(stderr, "Could not find port for mountd service\n");
        return -1;
    }

    char port_str_[8];
    char* port_str = port_str_ + sizeof(port_str_) - 1;
    if (mountd_port)
    {
        int port = mountd_port;
        *port_str = '\0';
        int i = 0;
        for(; port; )
        {
            (--port_str)[i] = ((port % 10) + '0');
            port /= 10;
        }
    }

    int mountd_fd = connect_name(hostname, port_str);
    assert(mountd_fd != -1);

    mountlist_t* mounts = mountd_dump(mountd_fd);
    for(mountlist_t* m = mounts; m; m = m->next)
    {
        printf("hostname: %s directory: %s\n",
             m->hostname,  m->directory);
    }

    fhandle3 fh = mountd_mnt(mountd_fd, "/nfs/git");
    printFileHandle(&fh);

    int nfs_fd = connect_name("192.168.178.26", "2049");
    cookie3 cookie = 0;
    cookie3 verifier = 0;
/*
    read_more = nfs_readdir(nfs_fd, &fh
          , &cookie, &verifier
          , myCallBack
    );
*/
    for(;;) {
        cookie3 old_cookie = cookie;
        nfs_readdirplus(nfs_fd, &fh
              , &cookie, &verifier
              , myPlusCallBack
        );
        if (cookie == old_cookie)
            break;
    }
    mountd_umnt(mountd_fd, "/nfs/git");

#if 0
    while(nfs_fd == -1)
    {
        printf("Calling accept\n");
        nfs_fd = accept(sock, (struct sockaddr*)&s_client, &addr_len);
    }

    // send request

    // read reply
    int sz = recv(nfs_fd, recvbuffer, MAX_BUFFER_SIZE, 0);

    printf ("We've got %d bytes yay\n", sz);
    close(nfs_fd);


    for(int i = 0; i < sz; i++)
    {
        printf("%x ", recvbuffer[i]);
        if ((i & 3) == 3) printf("\n");
    }
    printf("\n");

    RPCReply reply = *(RPCReply*)recvbuffer;

    uint32_t network_order_xid = reply.header.xid;
    // RPCReply_ByteFlip(&reply);
    printf("xid: %d\n", network_order_xid);

    int k = 12;

    getchar();

    //remove("test.txt");
#endif
    printf("It's time to say Goodbye :)\n");


    // int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
}
