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

static char sendbuffer[MAX_BUFFER_SIZE];
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

void SkipAuth(const uint32_t ** readPtrP)
{
    const uint32_t* readPtr = *readPtrP;

    uint32_t auth_flavor = htonl(*readPtr++);
    uint32_t length = htonl(*readPtr++);
    // let's skip the auth whatever it is
    u32 skipU32s = (length >> 2) + !!(length & 3);
    readPtr += skipU32s;

    *readPtrP = readPtr;
}

const RPCHeader parseRPCHeader(readPtrP_t readPtrP)
{
    const uint32_t* readPtr = *readPtrP;

    const uint32_t size_final = htonl(*readPtr++);
    const uint32_t xid = htonl(*readPtr++);
    const _Bool reply = (*readPtr++) != 0;

    *readPtrP = readPtr;

    return (RPCHeader){size_final, xid, reply};
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

    int sz_read =
        recv(sock_fd, recvbuffer, sizeof(recvbuffer), 0);

    const uint32_t* readPtr =
        (const uint32_t*)(recvbuffer);
    RPCHeader header = parseRPCHeader(&readPtr);

    _Bool accepted = (*readPtr++) == 0;

    SkipAuth(&readPtr);


    nfsstat3 status = htonl(*readPtr++);
    uint32_t port = htonl(*readPtr++);
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

    int sz_read =
        recv(sock_fd, recvbuffer, sizeof(recvbuffer), 0);

    uint32_t* readPtr = (uint32_t*)(recvbuffer);
    uint32_t sizeField = htonl(*readPtr++);
    printf("sizeField: %x\n", sizeField);

    uint32_t size =  sizeField & ~(1 << 31);
    _Bool final = ((sizeField & (1 << 31)) != 0);
    uint32_t xid = htonl(*readPtr++);

    _Bool reply = (*readPtr++) != 0;

    uint32_t auth_flavor = htonl(*readPtr++);
    uint32_t length = htonl(*readPtr++);
    // let's skip the auth whatever it is
    u32 skipU32s = (length >> 2) + !!(length & 3);
    readPtr += skipU32s;

    _Bool accepted = (*readPtr++) == 0;

    nfsstat3 status = htonl(*readPtr++);
    printf("XID: %x\n", xid);
    printf("size: %d\n", size);
    printf("Status: %s\n", nfsstat3_toChars(status));
    // now the actual params come ...

    _Bool hasNext = (*readPtr++) != 0;
    _Bool hadPrev = 0;
    portmap_dump_res* entry = result;
    while(hasNext)
    {
        portmap_dump_res r = {
            .program = htonl(*readPtr++),
            .version_ = htonl(*readPtr++),
            .port = htonl(*readPtr++),
            .proto = htonl(*readPtr++)
        };
        // printf("read a record\n");
        hasNext = (*readPtr++);
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

typedef struct fhandle3_t
{
    uint8_t fhandle3[64];
} fhandle3_t;

typedef struct mountlist_t {
    const char* hostname;
    const char* directory;
    struct mountlist_t* next;
} mountlist_t;

const char* readString(const uint32_t ** readPtrP,
                             char ** writePtrP,
                       const uint32_t length)
{
    char* writePtr = *writePtrP;
    const uint32_t* readPtr = *readPtrP;
    const char* readPtrString = (const char*) readPtr;

    readPtr += (length >> 2) + !!(length & 3);
    *readPtrP = readPtr;

    const char* result = (const char*)writePtr;

    for(uint32_t i = 0; i < length; i++)
    {
        *writePtr++ = readPtrString[i];
    }
    *writePtr++ = '\0';

    *writePtrP = writePtr;

    return result;
}

fhandle3_t readFileHandle(const uint32_t ** readPtrP)
{
    fhandle3_t result = {{0}};

    const uint32_t* readPtr = *readPtrP;

    const uint32_t length = htonl(*readPtr++);
    const uint8_t* fhReadPtr = (const uint8_t*) readPtr;

    readPtr += (length >> 2) + !!(length & 3);
    *readPtrP = readPtr;

    for(int i = 0; i < length; i++)
    {
        result.fhandle3[i] = *fhReadPtr++;
    }

    return result;
}

void printFileHandle(const fhandle3_t* handle)
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

fhandle3_t mountd_mnt(int mountd_fd, const char* dirPath)
{
    RPCSerializer s = {0};
    fhandle3_t result = {{0}};

    uint32_t mount_dump_xid = RPCSerializer_InitCall(&s,
        PREP_RPC_CALL(MOUNT_PROGRAM, 3, MOUNT_MNT_PROCEDURE));

    //RPCSerializer_PushNullAuth(&s);
    PushUnixAuthN(&s);

    uint32_t dirPathLength = strlen(dirPath);

    RPCSerializer_PushString(&s, dirPathLength, dirPath);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, mountd_fd);

    int sz_read =
        recv(mountd_fd, recvbuffer, sizeof(recvbuffer), 0);

    const uint32_t* readPtr = (const uint32_t*)recvbuffer;

    RPCHeader header = parseRPCHeader(&readPtr);

    _Bool reply_accepted = (*readPtr++) == 0;

    SkipAuth(&readPtr);

    _Bool rpc_accepted = (*readPtr++) == 0;

    nfsstat3 status = htonl(*readPtr++);
    printf("mnt: '%s'   status: %s\n", dirPath, nfsstat3_toChars(status));
    //TODO we should ready the required auth here ....
    result = readFileHandle(&readPtr);

    return result;
}


/*
 *       struct READDIR3args {
           nfs_fh3      dir;
           cookie3      cookie;
           cookieverf3  cookieverf;
           count3       count;
      };
*/
/*
      struct READDIRPLUS3args {
           nfs_fh3      dir;
           cookie3      cookie;
           cookieverf3  cookieverf;
           count3       dircount;
           count3       maxcount;
      };
*/
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

    int sz_read =
        recv(mountd_fd, recvbuffer, sizeof(recvbuffer), 0);

    assert(sz_read > (int)sizeof(RPCHeader));

     const uint32_t* readPtr = (const uint32_t*)(recvbuffer);
    RPCHeader header = parseRPCHeader(&readPtr);

    _Bool accepted = (*readPtr++) == 0;

    SkipAuth(&readPtr);

    nfsstat3 status = htonl(*readPtr++);
    // now the actual params come ...

    static char mountlist_storage[8192];
    char* writePtr = mountlist_storage;
    uint32_t storage_left = sizeof(mountlist_storage);

    _Bool hadPrev = 0;
    if (status == 0)
    {
        _Bool hasNext = ((*readPtr++) != 0);
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
                const uint32_t hlen = (htonl(*readPtr++));
                assert(storage_left > hlen + 1);
                entry->hostname = readString(&readPtr, &writePtr, hlen);
                storage_left -= (hlen + 1);
            }
            {
                uint32_t dlen = (htonl(*readPtr++));
                assert(storage_left > dlen - 1);
                entry->directory = readString(&readPtr, &writePtr, dlen);
                storage_left -= (dlen + 1);
            }
            hasNext = ((*readPtr++) != 0);
            hadPrev = 1;
        }
    }
    else
    {
        printf("Error: %s\n", nfsstat3_toChars(status));
    }

    return result;
}

fattr3 ReadFileAttribs(const uint32_t** readPtrP)
{
    fattr3 result;

    const uint32_t* readPtr = *readPtrP;

    result.type  = (ftype3) htonl(*readPtr++);
    result.mode  = (mode3) htonl(*readPtr++);
    result.nlink = htonl(*readPtr++);

    result.uid = htonl(*readPtr++);
    result.gid = htonl(*readPtr++);

    result.size = htonl(*readPtr++);
    result.size <<= 32;
    result.size |= htonl(*readPtr++);

    result.used = htonl(*readPtr++);
    result.used <<= 32;
    result.used |= htonl(*readPtr++);

    result.rdev.specdata1 = htonl(*readPtr++); // spec1
    result.rdev.specdata2 = htonl(*readPtr++); // spec2

    result.fsid = htonl(*readPtr++);
    result.fsid <<= 32;
    result.fsid |= htonl(*readPtr++);

    result.fileid = htonl(*readPtr++);
    result.fileid <<= 32;
    result.fileid |= htonl(*readPtr++);

    result.atime.seconds  = htonl(*readPtr++);
    result.atime.nseconds = htonl(*readPtr++);

    result.mtime.seconds  = htonl(*readPtr++);
    result.mtime.nseconds = htonl(*readPtr++);

    result.ctime.seconds   = htonl(*readPtr++);
    result.ctime.nseconds  = htonl(*readPtr++);

    *readPtrP = readPtr;

    return result;
}

static inline uint32_t fhandle3_length(const fhandle3_t* handle)
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

int nfs_readdir(int nfs_fd, const fhandle3_t* dir
               , uint64_t *cookie, uint64_t *cookieverf
               , _Bool (*dirIter)(const char* fName, uint64_t fileId) )
{
    RPCSerializer s = {0};
    mountlist_t* result = 0;

    uint32_t readdir_xid =
        RPCSerializer_InitCall(&s,
            PREP_RPC_CALL(NFS_PROGRAM, 3, NFS_READDIR_PROCEDURE));

    PushUnixAuthN(&s);


    int length = fhandle3_length(dir);
    RPCSerializer_PushString(&s, length, (const char*)dir->fhandle3);

    //TODO get rid of Endian specific code below
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

    int read_sz = recv(nfs_fd, recvbuffer, sizeof(recvbuffer), 0);

    printf("read: %d bytes as reply to readdir\n", read_sz);

    const uint32_t* readPtr = (const uint32_t*)(recvbuffer);
    RPCHeader header = parseRPCHeader(&readPtr);
    assert(header.xid == readdir_xid);

    _Bool accepted = (*readPtr++) == 0;

    SkipAuth(&readPtr);

    _Bool accept_state = (*readPtr++) == 0;

    nfsstat3 status = htonl(*readPtr++);

    printf("Status: %s\n", nfsstat3_toChars(status));

    _Bool hasAttrs = (*readPtr++) != 0;
    if (hasAttrs)
    {
        ReadFileAttribs(&readPtr);
    }

    cookie_verif_hi = htonl(*readPtr++);
    cookie_verif_lw = htonl(*readPtr++);
    for(;;)
    {
        _Bool hasNext = ((*readPtr++) != 0);
        if (!hasNext)
            break;
        uint32_t fileIdHi = htonl(*readPtr++);
        uint32_t fileIdLw = htonl(*readPtr++);

        uint32_t name_length = htonl(*readPtr++);

        fileid3 fileId = fileIdHi;
        fileId <<= 32;
        fileId |= fileIdLw;

        char str_buf[1024];
        char* writePtr = str_buf;

        const char* fname = readString(&readPtr, &writePtr, name_length);

        cookie_hi = htonl(*readPtr++);
        cookie_lw = htonl(*readPtr++);
        if (dirIter(fname, fileId))
            break;
    }
    _Bool wasLastList = (*readPtr++);

    return !wasLastList;
    // printf("%x %x %x %x", *readPtr++, *readPtr++, *readPtr++, *readPtr++);

}

#ifndef _WIN32
#  define INVALID_SOCKET -1
#endif

_Bool myCallBack(const char* filename, uint64_t fileId)
{
    return 0;
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

    fhandle3_t fh = mountd_mnt(mountd_fd, "/nfs/git");
    printFileHandle(&fh);

    int nfs_fd = connect_name("192.168.178.26", "2049");
    cookie3 cookie = 0;
    cookie3 verifier = 0;

    _Bool read_more = nfs_readdir(nfs_fd, &fh
          , &cookie, &verifier
          , myCallBack
    );

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
