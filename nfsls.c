#ifdef _WIN32
#  include <io.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
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

void ByteFlip_Array(u32* array, uint32_t length)
{
    u32* end = array + length;

    for(u32* p = array; p < end; p++)
    {
        *p = htonl(*p);
    }
}


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
#define MAX_BUFFER_SIZE 1024

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


uint32_t RandomXid()
{
    static uint32_t i = 0;
    static const uint32_t arr[] = {
        0x12874512,
        0x12845512,
        0x12874235,
        0x12874232,
        0x12882513
    };

    return arr[i++ % sizeof(arr)];
}

typedef struct portmap_dump_res {
    u32 program;
    u32 version_;
    u32 proto;
    u32 port;
    struct portmap_dump_res* next;
} portmap_dump_res;

typedef struct RPCSerializer
{
    uint8_t* WritePtr;
    uint8_t* BufferPtr;

    uint32_t Size;
    uint32_t MaxSize;
} RPCSerializer;

#define PREP_RPC_CALL_XID(PROG, PROG_VER, PROC, XID) \
    (RPCCall) { \
        .header = (RPCHeader) { \
            .size_final = 0, \
            .xid = XID, \
            .reply = MESSAGE_TYPE_CALL \
        }, \
        .rpc_version = 2, \
        .program_id = PROG, \
        .program_ver = PROG_VER, \
        .procedure = PROC, \
    }

#define PLACEHOLDER_XID 0x1234ABCD
#define PREP_RPC_CALL(PROG, PROG_VER, PROC) \
    PREP_RPC_CALL_XID(PROG, PROG_VER, PROC, PLACEHOLDER_XID)

void RPCSerializer_Finalize(RPCSerializer* self)
{
    RPCHeader* header = (RPCHeader*)self->BufferPtr;
    _Bool last_package = 1;
    header->size_final = htonl(self->Size | (last_package << 31));
}

void RPCSerializer_Init(RPCSerializer* self, uint8_t* Buffer, uint32_t sz)
{
    self->BufferPtr = (uint8_t*) malloc(sz);
    self->WritePtr = (uint8_t*) (self->BufferPtr + 4);

    self->MaxSize = sz;
    self->Size = 0;
}
#define CALL_DEFAULT_SIZE 1024

/// Retruns: Xid for the call in host order
static inline uint32_t RPCSerializer_InitCall(RPCSerializer* self,  RPCCall q)
{
    if (!self->WritePtr)
    {
        self->BufferPtr = malloc(CALL_DEFAULT_SIZE);
        self->MaxSize = CALL_DEFAULT_SIZE;
        self->WritePtr = self->BufferPtr + sizeof(RPCCall);
    }

    uint32_t xid = q.header.xid;
    if (xid == PLACEHOLDER_XID)
        q.header.xid = RandomXid();

    self->Size = sizeof(RPCCall) - sizeof(u32);
    (*(RPCCall*)self->BufferPtr) =  q;
    ByteFlip_Array((uint32_t*)self->BufferPtr, sizeof(RPCCall) / sizeof(u32));

    return xid;
}

void RPCSerilizer_Reset(RPCSerializer* self)
{
    self->BufferPtr -= (self->Size + sizeof(u32));
    self->Size = 0;
}

void RPCSerializer_EnsureSize(RPCSerializer* self, uint32_t sz)
{
    assert(self->Size + sz < self->MaxSize);
}

void RPCSerializer_PushU32(RPCSerializer* self, uint32_t value)
{
    RPCSerializer_EnsureSize(self, 4);
    (*(uint32_t*)self->WritePtr) = htonl(value);
    self->WritePtr += 4;
    self->Size += 4;
}

void RPCSerializer_PushByteArray(RPCSerializer* self,
                                 uint8_t* array, uint32_t size)
{
    RPCSerializer_EnsureSize(self, 4 + size);
    RPCSerializer_PushU32(self, size);
    memcpy(self->BufferPtr, array, size);

    self->Size += size + !!(size & 3);
    self->BufferPtr += size + !!(size & 3);
}

void RPCSerializer_PushNullAuth(RPCSerializer* self)
{
    uint32_t* WritePtr = (uint32_t*) self->WritePtr;
    *WritePtr++ = 0;
    *WritePtr++ = 0;
    *WritePtr++ = 0;
    *WritePtr++ = 0;
    self->Size += 4 * sizeof(u32);
    self->WritePtr = (uint8_t*) WritePtr;
}
#define ALIGN4(VAR) (((VAR) + 3) & ~3)

void RPCSerializer_PushString(RPCSerializer* self,
                              uint32_t length, const char* str)
{
    const uint32_t n_bytes = ALIGN4(length);

    RPCSerializer_PushU32(self, length);
    char* strWritePtr = self->WritePtr;
    *self->WritePtr += n_bytes;
    self->Size += n_bytes;

    for(int i = 0; i < length; i++)
    {
        *strWritePtr++ = str[i];
    }

}

int RPCSerializer_Send(RPCSerializer* self, int sock_fd)
{
    int sz_send = write(sock_fd, self->BufferPtr, self->Size + sizeof(u32));
#ifdef _WIN32
    _commit(sock_fd);
#else
    fsync(sock_fd);
#endif
    // printf("send: %d of %d bytes out\n", sz_send, self->Size);
    return sz_send;
}

#define PORTMAP_PROGRAM 100000
#define MOUNT_PROGRAM  100005
#define PORTMAP_DUMP_PROCEDURE 4
#define PORTMAP_DUMP_GETPORT 3
#define MOUNT_MNT_PROCEDURE 1
#define MOUNT_DUMP_PROCEDURE 2
#define MOUNT_UMNT_PROCEDURE 3
#define MOUNT_EXPORT_PROCEDURE 5
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
        read(sock_fd, recvbuffer, sizeof(recvbuffer));

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
        read(sock_fd, recvbuffer, sizeof(recvbuffer));

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

typedef struct fhandle3
{
    uint8_t handleData[64];
} fhandle3;

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

fhandle3 readFileHandle(const uint32_t ** readPtrP)
{
    fhandle3 result = {{0}};

    const uint32_t* readPtr = *readPtrP;

    const uint32_t length = htonl(*readPtr++);
    const uint8_t* fhReadPtr = (const uint8_t*) readPtr;

    readPtr += (length >> 2) + !!(length & 3);
    *readPtrP = readPtr;

    for(int i = 0; i < length; i++)
    {
        result.handleData[i] = *fhReadPtr++;
    }

    return result;
}

void printFileHandle(const fhandle3* handle)
{
    const uint32_t* ptr = (const uint32_t*)
        &handle->handleData[0];

    printf("fh: %x %x %x %x %x %x %x %x\n",
        ptr[0], ptr[1], ptr[2], ptr[3],
        ptr[4], ptr[5], ptr[6], ptr[7]);
}

fhandle3 mountd_umnt(int mountd_fd, const char* dirPath)
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

    uint32_t mount_dump_xid =
        RPCSerializer_InitCall(&s,
            PREP_RPC_CALL(MOUNT_PROGRAM, 3, MOUNT_MNT_PROCEDURE));

    RPCSerializer_PushNullAuth(&s);
    uint32_t dirPathLength = strlen(dirPath);

    RPCSerializer_PushString(&s, dirPathLength, dirPath);
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, mountd_fd);

    int sz_read =
        read(mountd_fd, recvbuffer, sizeof(recvbuffer));

    const uint32_t* readPtr = (const uint32_t*)recvbuffer;

    RPCHeader header = parseRPCHeader(&readPtr);
    printf("FragmentLength: %d\n", header.size_final & ~(1 << 31));

    _Bool reply_accepted = (*readPtr++) == 0;

    SkipAuth(&readPtr);
    
    _Bool rpc_accepted = (*readPtr++) == 0;

    nfsstat3 status = htonl(*readPtr++);
    printf("mnt: '%s'   status: %s\n", dirPath, nfsstat3_toChars(status));
    int k = 12;

    result = readFileHandle(&readPtr);

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

    int sz_read =
        read(mountd_fd, recvbuffer, sizeof(recvbuffer));

    assert(sz_read > (int)sizeof(RPCHeader));

    const uint32_t* readPtr = (const uint32_t*)(recvbuffer);
    RPCHeader header = parseRPCHeader(&readPtr);

    _Bool accepted = (*readPtr++) == 0;

    SkipAuth(&readPtr);

    nfsstat3 status = htonl(*readPtr++);
    // now the actual params come ...

    char* mountlist_storage =
        malloc(8192 + sizeof(mountlist_t));
    char* writePtr = mountlist_storage;
    uint32_t storage_left = 8192;

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
        }
    }
    else
    {
        printf("Error: %s\n", nfsstat3_toChars(status));
    }

    return result;
}

int NFS_readdir(uint32_t rootHandle)
{
    #define READDIR_PROCN 16;

}

#ifndef _WIN32
#  define INVALID_SOCKET -1
#endif

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

    fhandle3 fd = mountd_mnt(mountd_fd, "/nfs/git");
    printFileHandle(&fd);
                  mountd_umnt(mountd_fd, "/nfs/git");

#if 0
    while(nfs_fd == -1)
    {
        printf("Calling accept\n");
        nfs_fd = accept(sock, (struct sockaddr*)&s_client, &addr_len);
    }

    // send request

    // read reply
    int sz = read(nfs_fd, recvbuffer, MAX_BUFFER_SIZE);

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
