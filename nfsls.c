#define _BSD_SOURCE 1
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <assert.h>

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

void RPCCall_ByteFlip(RPCCall* self)
{
    ByteFlip_Array((u32*)self, sizeof(*self) / sizeof(u32));
}

typedef struct RPCService {
    uint32_t programm_id;
    uint32_t version_;
    uint32_t network_id_length;
    char* network_id_ptr;
} RPCService;


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
    const int option = 1;
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

void RPCSerializer_Finalize(RPCSerializer* self)
{
    RPCHeader* header = (RPCHeader*)self->BufferPtr;
    _Bool last_package = 1;
    header->size31_final1 = htonl(self->Size | (last_package << 31));
}

void RPCSerializer_Init(RPCSerializer* self, uint8_t* Buffer, uint32_t sz)
{
    self->BufferPtr = (uint8_t*) malloc(sz);
    self->WritePtr = (uint8_t*) (self->BufferPtr + 4);

    self->MaxSize = sz;
    self->Size = 0;
}
#define CALL_DEFAULT_SIZE 1024

void RPCSerializer_InitCall(RPCSerializer* self,  const RPCCall* q)
{
    if (!self->WritePtr)
    {
        self->BufferPtr = malloc(CALL_DEFAULT_SIZE);
        self->MaxSize = CALL_DEFAULT_SIZE;
        self->WritePtr = self->BufferPtr + sizeof(RPCCall);
    }

    self->Size = sizeof(RPCCall) - sizeof(u32);
    (*(RPCCall*)self->BufferPtr) =  *q;
    ByteFlip_Array((uint32_t*)self->BufferPtr, sizeof(RPCCall) / sizeof(u32));
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

void RPCSerializer_PushByteArray(RPCSerializer* self, uint8_t* array, uint32_t size)
{
    RPCSerializer_EnsureSize(self, 4 + size);
    RPCSerializer_PushU32(self, size);
    memcpy(self->BufferPtr, array, size);

    self->Size += size + !!(size & 3);
    self->BufferPtr += size + !!(size & 3);
}

int RPCSerializer_Send(RPCSerializer* self, int sock_fd)
{
    int sz_send = write(sock_fd, self->BufferPtr, self->Size + sizeof(u32));
    fsync(sock_fd);
    printf("send: %d of %d bytes out\n", sz_send, self->Size);
    return sz_send;
}

#define PORTMAP_PROGRAM 100000
#define MOUNTD_PROGRAM  100005
#define MESSAGE_TYPE_CALL 0

void portmap_nullCall(int sock_fd)
{
    RPCSerializer s = {0};

    RPCCall NullProc = (RPCCall) {
        .header = (RPCHeader) {
            .xid = 0,
            .call_or_reply = MESSAGE_TYPE_CALL
        },
        .rpc_version = 2,
        .program_id = PORTMAP_PROGRAM,
        .program_ver = 2,
        .procedure = 0,
    };

    RPCSerializer_InitCall(&s, &NullProc);
    {

        // AUTH_NONE
        RPCSerializer_PushU32(&s, 0);
        // AuthSize: 0
        RPCSerializer_PushU32(&s, 0);
        // --------------------------
        //  verifier
        // --------------------
        // AUTH_NONE
        RPCSerializer_PushU32(&s, 0);
        // AuthSize: 0
        RPCSerializer_PushU32(&s, 0);

    }
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, sock_fd);

}

portmap_dump_res* portmap_query_mountd(int sock_fd)
{
    static portmap_dump_res pm_dump_storage[6];
    portmap_dump_res* result =  (portmap_dump_res*)
        pm_dump_storage;

    RPCSerializer s = {0};
    const uint32_t portmap_dump_xid
        = RandomXid();
    // send dump command
    RPCCall portmap = (RPCCall) {
        .header = (RPCHeader) {
            .xid = portmap_dump_xid,
            .call_or_reply = MESSAGE_TYPE_CALL
        },
        .rpc_version = 2,
        .program_id = PORTMAP_PROGRAM,
        .program_ver = 2,
#define PORTMAP_DUMP_PROCEDURE 4
        .procedure = PORTMAP_DUMP_PROCEDURE,
    };
#undef PORTMAP_DUMP_PROCEDURE

    RPCSerializer_InitCall(&s, &portmap);
    {
        // AUTH_NONE
        RPCSerializer_PushU32(&s, 0);
        // Length: 0
        RPCSerializer_PushU32(&s, 0);
        // --------------------------
        //  verifier
        // --------------------
        // AUTH_NONE
        RPCSerializer_PushU32(&s, 0);
        // AuthSize: 0
        RPCSerializer_PushU32(&s, 0);
    }
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
        if (r.program == MOUNTD_PROGRAM
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

mountlist_t* mountd_dump(int mountd_fd)
{
    uint32_t mountd_dump_xid;
    RPCSerializer s = {0};
    mountlist_t* result = 0;

#define MOUNTD_DUMP_PROCEDURE 2
    RPCCall mountd = (RPCCall) {
        .header = (RPCHeader) {
            .xid = mountd_dump_xid,
            .call_or_reply = MESSAGE_TYPE_CALL
        },
        .rpc_version = 2,
        .program_id = MOUNTD_PROGRAM,
        .program_ver = 2,
        .procedure = MOUNTD_DUMP_PROCEDURE,
    };

    RPCSerializer_InitCall(&s, &mountd);
    {
        // AUTH_NONE
        RPCSerializer_PushU32(&s, 0);
        // Length: 0
        RPCSerializer_PushU32(&s, 0);
        // --------------------------
        //  verifier
        // --------------------
        // AUTH_NONE
        RPCSerializer_PushU32(&s, 0);
        // AuthSize: 0
        RPCSerializer_PushU32(&s, 0);
    }
    RPCSerializer_Finalize(&s);
    RPCSerializer_Send(&s, mountd_fd);

    int sz_read =
        read(mountd_fd, recvbuffer, sizeof(recvbuffer));

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

    char* mountlist_storage =
        malloc(8192 + sizeof(mountlist_t));
    char* write_ptr = mountlist_storage;
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
                    write_ptr;
            }
            entry = (mountlist_t*)
                write_ptr;
            write_ptr += sizeof(mountlist_t);
            storage_left -= sizeof(mountlist_t);
            {
                uint32_t hlen = (htonl(*readPtr++));
                const char* host_read_ptr = (const char*)readPtr;
                readPtr += (hlen >> 2) + !!(hlen & 3);

                entry->hostname = write_ptr;
                for(uint32_t i = 0; i < hlen; i++)
                {
                    *write_ptr++ = host_read_ptr[i];
                }
                *write_ptr++ = '\0';
            }
            {
                uint32_t dlen = (htonl(*readPtr++));
                const char* dir_read_ptr = (const char*)readPtr;
                char* dir_write_ptr = write_ptr;
                readPtr += (dlen >> 2) + !!(dlen % 3);
                entry->directory = write_ptr;
                {
                    for(int i = 0; i < dlen; i++)
                    {
                        *dir_write_ptr++ = dir_read_ptr[i];
                    }
                    storage_left -= (dlen + 1);
                }
                *dir_write_ptr++ =  '\0';
                write_ptr = dir_write_ptr;
            }
            hasNext = ((*readPtr++) != 0);
        }
    }

    return result;
}

int NFS_readdir(uint32_t rootHandle)
{
    #define READDIR_PROCN 16;

}



int main(int argc, char* argv[])
{
    /* Create socket. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock == -1) {
      perror("opening stream socket");
      exit(1);
    }

    int addr_len = sizeof(s_client);

    int portmap_fd = connect_name("192.168.178.26", "111");
    assert(portmap_fd != -1);

    // portmap_nullCall(portmap_fd);

    portmap_dump_res* mountd_entry
        = portmap_query_mountd(portmap_fd);
#if 1
    for(portmap_dump_res* dp = mountd_entry; dp; dp = dp->next)
    {
        printf("dp.version: %d\t dp.port: %d, dp.proto: %d\n",
                dp->version_,    dp->port,    dp->proto);
    }
#endif
    char port_str_[8];
    char* port_str = port_str_ + sizeof(port_str_) - 1;
    if (mountd_entry)
    {
        int port = mountd_entry->port;
        *port_str = '\0';
        int i = 0;
        for(; port; )
        {
            (--port_str)[i] = ((port % 10) + '0');
            port /= 10;
        }
    }

    int mountd_fd = connect_name("192.168.178.26", port_str);
    assert(mountd_fd != -1);

    mountlist_t* mounts = mountd_dump(mountd_fd);
    for(mountlist_t* m = mounts; m; m = m->next)
    {
        printf("hostname: %s directory: %s\n",
             m->hostname,  m->directory);
    }

    int connection_fd = connect_name("192.168.178.26", "2049");
    assert(connection_fd != -1);


    while(connection_fd == -1)
    {
        printf("Calling accept\n");
        connection_fd = accept(sock, (struct sockaddr*)&s_client, &addr_len);
    }

    int sendlen = sizeof(sendbuffer);

//    NFS_readir(sendbuffer, &sendlen);

    send(connection_fd, sendbuffer, sendlen, 0);

    int sz = read(connection_fd, recvbuffer, MAX_BUFFER_SIZE);

    printf ("We've got %d bytes yay\n", sz);
    close(connection_fd);

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

    printf("It's time to say Goodbye :)\n");


    // int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
}
