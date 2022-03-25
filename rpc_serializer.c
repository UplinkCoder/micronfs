#include "rpc_serializer.h"
#include <string.h>

#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <sys/socket.h>
   typedef int SOCKET;
#endif

#ifndef _cplusplus
#  include <stdbool.h>
#endif

void RPCSerializer_Init(RPCSerializer* self, uint8_t* Buffer, uint32_t sz)
{
    self->BufferPtr = Buffer ? Buffer : self->InlineStorage;
    self->WritePtr = (uint8_t*) (self->BufferPtr + 4);

    self->MaxSize = sz;
    self->Size = 0;
}

void RPCSerializer_Finalize(RPCSerializer* self)
{
    RPCHeader* header = (RPCHeader*)self->BufferPtr;
    bool last_package = 1;
    header->size_final = HTONL(self->Size | (last_package << 31));
}

void RPCSerializer_PushNullAuth(RPCSerializer* self)
{
    RPCSerializer_EnsureSize(self, 16);

    uint32_t* WritePtr = (uint32_t*) self->WritePtr;
    *WritePtr++ = 0;
    *WritePtr++ = 0;
    *WritePtr++ = 0;
    *WritePtr++ = 0;
    self->Size += 4 * sizeof(u32);
    self->WritePtr = (uint8_t*) WritePtr;
}

void RPCSerializer_PushU64(RPCSerializer* self, uint64_t value)
{
    RPCSerializer_EnsureSize(self, 8);
    (*(uint32_t*)self->WritePtr) = HTONL(value >> 32);
    self->WritePtr += 4;
    (*(uint32_t*)self->WritePtr) = HTONL(value & UINT32_MAX);
    self->WritePtr += 4;
    self->Size += 8;
}

void RPCSerializer_PushString(RPCSerializer* self,
                              uint32_t length, const char* str)
{
    const uint32_t n_bytes = ALIGN4(length);
    RPCSerializer_EnsureSize(self, n_bytes + 4);

    RPCSerializer_PushU32(self, length);
    char* strWritePtr = (char*)self->WritePtr;
    self->WritePtr += n_bytes;
    self->Size += n_bytes;

    for(int i = 0; i < length; i++)
    {
        *strWritePtr++ = str[i];
    }
}

void RPCSerializer_PushU32Array(RPCSerializer *self,
                                uint32_t n_elements, const uint32_t array[])
{
    RPCSerializer_EnsureSize(self, 4 + (4 * n_elements));

    RPCSerializer_PushU32(self, n_elements);
    for(int i = 0; i < n_elements; i++)
    {
        RPCSerializer_PushU32(self, array[i]);
    }
}

static inline uint32_t ComputeAuthSize(uint32_t string_length, uint32_t n_aux_gids)
{
    return 4 + 4 + ALIGN4(string_length)
            + 4 + 4 + 4 + (n_aux_gids * 4);
}

void RPCSerializer_PushUnixAuth(RPCSerializer* self,
                                uint32_t stamp,
                                const char* machine_name,
                                uint32_t uid, uint32_t gid,
                                const uint32_t n_aux_gids, const uint32_t aux_gids[])
{
    uint32_t name_size = strlen(machine_name);
    uint32_t auth_size = ComputeAuthSize(name_size, n_aux_gids);

    RPCSerializer_EnsureSize(self, auth_size);

    RPCSerializer_PushU32(self, 1);
    RPCSerializer_PushU32(self, auth_size);
    RPCSerializer_PushU32(self, stamp);

    RPCSerializer_PushString(self, name_size, machine_name);
    RPCSerializer_PushU32(self, uid);
    RPCSerializer_PushU32(self, gid);

    RPCSerializer_PushU32Array(self, n_aux_gids, aux_gids);
    // NONE VERIFIER

    RPCSerializer_PushU32(self, 0);
    RPCSerializer_PushU32(self, 0);
}

int RPCSerializer_Send(RPCSerializer* self, SOCKET sock_fd)
{
    int sz_send = send(sock_fd, self->BufferPtr, self->Size + sizeof(u32), 0);
    // printf("send: %d of %d bytes out\n", sz_send, self->Size);
    return sz_send;
}

void RPCDeserializer_Init(RPCDeserializer* self, SOCKET sock_fd)
{
    self->SockFd = sock_fd;
    self->BufferPtr = self->InlineStorage;
    self->MaxBuffer = sizeof(self->InlineStorage);
    self->ReadPtr = (const uint32_t*)self->InlineStorage;
}

RPCHeader RPCDeserializer_RecvHeader(RPCDeserializer* self)
{
    self->Size = recv(self->SockFd, self->BufferPtr, self->MaxBuffer, 0);
    // printf("initial recv: %d\n", self->Size);
    assert(self->Size >= sizeof(RPCHeader));

    const uint32_t size_final = HTONL(*self->ReadPtr); self->ReadPtr++;
    const uint32_t xid = HTONL(*self->ReadPtr); self->ReadPtr++;
    const int reply = (*self->ReadPtr++) != 0;
    self->FragmentSizeLeft = (size_final & ~(1 << 31));
    // don't count the size field
    self->FragmentSizeLeft -= (self->Size - 4);
    return (RPCHeader){size_final, xid, reply};
}

static inline void RPCDeserializer_RefillBuffer(RPCDeserializer* self)
{
    const int32_t oldSize = RPCDeserializer_BufferLeft(self);
    assert(oldSize >= 0);
    memmove(self->BufferPtr, self->ReadPtr, oldSize);

    int RecivedBytes =
        recv(self->SockFd, self->BufferPtr + oldSize, self->MaxBuffer - oldSize, 0);
    // printf("Refill recv: %d\n", RecivedBytes);
    int missing_bytes = self->MaxBuffer - (oldSize + RecivedBytes);

    self->FragmentSizeLeft -= RecivedBytes;
    self->Size = oldSize + RecivedBytes;
    // TODO have a look at this again
    if (self->FragmentSizeLeft < missing_bytes)
        missing_bytes = self->FragmentSizeLeft;

    if (missing_bytes)
    {
        RecivedBytes = recv(self->SockFd, self->BufferPtr + self->Size, missing_bytes, 0);
        // printf("Refill recv (again): %d\n", RecivedBytes);
        self->Size += RecivedBytes;
        self->FragmentSizeLeft  -= RecivedBytes;
    }

    self->ReadPtr = (const uint32_t*)self->BufferPtr;
}

void RPCDeserializer_EnsureSize(RPCDeserializer* self, uint32_t sz)
{
    if (RPCDeserializer_BufferLeft(self) < (int32_t)sz)
    {
        RPCDeserializer_RefillBuffer(self);
    }
}

void RPCDeserializer_SkipAuth(RPCDeserializer *self)
{

    uint32_t auth_flavor = HTONL(*self->ReadPtr); self->ReadPtr++;
    uint32_t length = HTONL(*self->ReadPtr); self->ReadPtr++;
    RPCDeserializer_EnsureSize(self, length);

    // let's skip the auth whatever it is
    u32 skipU32s = (length >> 2) + !!(length & 3);
    self->ReadPtr += skipU32s;

    return ;
}

const char* RPCDeserializer_ReadString(RPCDeserializer* self
                                    , char ** writePtrP, uint32_t length)

{
    char* writePtr = *writePtrP;
    const char* readPtrString = (const char*) self->ReadPtr;

    self->ReadPtr += (length >> 2) + !!(length & 3);

    const char* result = (const char*)writePtr;

    for(uint32_t i = 0; i < length; i++)
    {
        *writePtr++ = readPtrString[i];
    }
    *writePtr++ = '\0';

    *writePtrP = writePtr;

    return result;
}

fhandle3 RPCDeserializer_ReadFileHandle(RPCDeserializer* self)
{
    fhandle3 result = {{0}};

    RPCDeserializer_EnsureSize(self, 4);
    const uint32_t length = HTONL(*self->ReadPtr); self->ReadPtr++;
    RPCDeserializer_EnsureSize(self, length);
    const uint8_t* fhReadPtr = (const uint8_t*) self->ReadPtr;

    self->ReadPtr += (length >> 2) + !!(length & 3);

    for(int i = 0; i < length; i++)
    {
        result.fhandle3[i] = *fhReadPtr++;

    }

    return result;
}

fattr3 RPCDeserializer_ReadFileAttribs(RPCDeserializer* self)
{
    RPCDeserializer_EnsureSize(self, sizeof(fattr3));
    fattr3 result;

    const uint32_t* ReadPtr = self->ReadPtr;

    result.type  = (ftype3) HTONL(*ReadPtr); ReadPtr++;
    result.mode  = (mode3) HTONL(*ReadPtr); ReadPtr++;
    result.nlink = HTONL(*ReadPtr); ReadPtr++;

    result.uid = HTONL(*ReadPtr); ReadPtr++;
    result.gid = HTONL(*ReadPtr); ReadPtr++;

    result.size = HTONL(*ReadPtr); ReadPtr++;
    result.size <<= 32;
    result.size |= HTONL(*ReadPtr); ReadPtr++;

    result.used = HTONL(*ReadPtr); ReadPtr++;
    result.used <<= 32;
    result.used |= HTONL(*ReadPtr); ReadPtr++;

    result.rdev.specdata1 = HTONL(*ReadPtr); ReadPtr++; // spec1
    result.rdev.specdata2 = HTONL(*ReadPtr); ReadPtr++; // spec2

    result.fsid = HTONL(*ReadPtr); ReadPtr++;
    result.fsid <<= 32;
    result.fsid |= HTONL(*ReadPtr); ReadPtr++;

    result.fileid = HTONL(*ReadPtr); ReadPtr++;
    result.fileid <<= 32;
    result.fileid |= HTONL(*ReadPtr); ReadPtr++;

    result.atime.seconds  = HTONL(*ReadPtr); ReadPtr++;
    result.atime.nseconds = HTONL(*ReadPtr); ReadPtr++;

    result.mtime.seconds  = HTONL(*ReadPtr); ReadPtr++;
    result.mtime.nseconds = HTONL(*ReadPtr); ReadPtr++;

    result.ctime.seconds   = HTONL(*ReadPtr); ReadPtr++;
    result.ctime.nseconds  = HTONL(*ReadPtr); ReadPtr++;

    self->ReadPtr = (const uint32_t*)ReadPtr;

    return result;
}

uint64_t RPCDeserializer_ReadU64(RPCDeserializer* self)
{
    assert(RPCDeserializer_BufferLeft(self) >= 8);
    uint64_t result;

    result = HTONL(*self->ReadPtr); self->ReadPtr++;
    result <<= 32;
    result |= HTONL(*self->ReadPtr); self->ReadPtr++;

    return result;
}
