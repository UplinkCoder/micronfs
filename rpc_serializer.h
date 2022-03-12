#include <stdint.h>
#include <assert.h>
#include "endian.h"

#pragma pack(push, 1)
typedef uint32_t u32;

typedef struct RPCHeader
{
    u32 size_final;
    u32 xid;
    u32 reply;
} RPCHeader;

typedef struct RPCCall
{
    RPCHeader header;
    u32 rpc_version;// : 2      {0x00 00 00 02}
    u32 program_id;// : 100003 {0x00 01 86 a3}
    u32 program_ver;//: 3      {0x00 00 00 03}
    u32 procedure;//   : 1      {0x00 00 00 01}
} RPCCall;

typedef struct RPCReply
{
    RPCHeader header;
} RPCReply;
#pragma pack(pop)


typedef struct RPCSerializer
{
    uint8_t* WritePtr;
    uint8_t* BufferPtr;

    uint32_t Size;
    uint32_t MaxSize;

    uint8_t InlineStorage[512];
} RPCSerializer;

static inline void ByteFlip_Array(u32* array, uint32_t length)
{
    u32* end = array + length;

    for(u32* p = array; p < end; p++)
    {
        *p = HTONL(*p);
    }
}

void RPCSerializer_Init(RPCSerializer* self, uint8_t* Buffer, uint32_t sz);

void RPCSerializer_PushNullAuth(RPCSerializer* self);

void RPCSerializer_PushUnixAuth(RPCSerializer* self,
                                uint32_t stamp,
                                const char* machine_name,
                                uint32_t uid, uint32_t gid,
                                const uint32_t n_aux_gids, const uint32_t aux_gids[]);

void RPCSerializer_PushString(RPCSerializer* self,
                              uint32_t length, const char* str);

void RPCSerializer_PushU32Array(RPCSerializer *self,
                                uint32_t n_elements, const uint32_t array[]);

void RPCSerializer_Finalize(RPCSerializer* self);
int RPCSerializer_Send(RPCSerializer* self, int sock_fd);

static inline uint32_t RandomXid()
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

/// Retruns: Xid for the call in host order
static inline uint32_t RPCSerializer_InitCall(RPCSerializer* self,  RPCCall q)
{
    if (!self->WritePtr)
    {
        self->BufferPtr = self->InlineStorage;
        self->MaxSize = sizeof(self->InlineStorage);
        self->WritePtr = self->BufferPtr + sizeof(RPCCall);
    }

    uint32_t xid = q.header.xid;
    if (xid == PLACEHOLDER_XID)
        xid = q.header.xid = RandomXid();

    self->Size = sizeof(RPCCall) - sizeof(u32);
    (*(RPCCall*)self->BufferPtr) =  q;
    ByteFlip_Array((uint32_t*)self->BufferPtr, sizeof(RPCCall) / sizeof(u32));

    return xid;
}

static inline void RPCSerializer_EnsureSize(RPCSerializer* self, uint32_t sz)
{
    assert(self->Size + sz < self->MaxSize);
}

static inline void RPCSerializer_PushU32(RPCSerializer* self, uint32_t value)
{
    RPCSerializer_EnsureSize(self, 4);
    (*(uint32_t*)self->WritePtr) = HTONL(value);
    self->WritePtr += 4;
    self->Size += 4;
}
