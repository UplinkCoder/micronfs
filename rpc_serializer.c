#include "rpc_serializer.h"
#include <string.h>

#ifndef _WIN32
#else
# include <unistd.h>
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
    _Bool last_package = 1;
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
#define ALIGN4(VAR) (((VAR) + 3) & ~3)

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
