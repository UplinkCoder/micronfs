#ifdef _WIN32
#  include "stdint_msvc.h"
#else
#  include <stdint.h>
#endif

#include <assert.h>
#include "endian.h"
#include "micronfs.h"

#ifndef _WIN32
 typedef int SOCKET;
#else
# include <winsock2.h>
#endif

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

typedef struct RPCDeserializer
{
    const uint32_t* ReadPtr;
    uint8_t* BufferPtr;

    /* amount of message unread */
    SOCKET SockFd;
    uint32_t Size;
    int32_t FragmentSizeLeft;
    uint32_t MaxBuffer;

    uint8_t InlineStorage[1460];
} RPCDeserializer;


static inline void ByteFlip_Array(u32* array, uint32_t length)
{
    u32* end = array + length;
	u32* p;
    for(p = array; p < end; p++)
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

void RPCSerializer_PushU64(RPCSerializer* self, uint64_t value);
void RPCSerializer_PushString(RPCSerializer* self,
                              uint32_t length, const char* str);

void RPCSerializer_PushU32Array(RPCSerializer *self,
                                uint32_t n_elements, const uint32_t array[]);

void RPCSerializer_Finalize(RPCSerializer* self);
int RPCSerializer_Send(RPCSerializer* self, SOCKET sock_fd);

#ifdef _MSC_VER
#  if _MSC_VER <= 1800
#    define inline
#  endif
#endif

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


#define PLACEHOLDER_XID 0x1234ABCD
#define PREP_RPC_CALL(PROG, PROG_VER, PROC) \
    PREP_RPC_CALL_XID(PROG, PROG_VER, PROC, PLACEHOLDER_XID)

#define RPCSerializer_InitCall(SELF, PROG, PROG_VER, PROC) \
	RPCSerializer_InitCall_xid((SELF), PLACEHOLDER_XID, (PROG), (PROG_VER), (PROC))

/// Retruns: Xid for the call in host order
static inline uint32_t RPCSerializer_InitCall_xid(RPCSerializer* self,
                                                  uint32_t xid,
                                                  uint32_t prog,
                                                  uint32_t prog_ver,
                                                  uint32_t proc)
{
    if (!self->WritePtr)
    {
        self->BufferPtr = self->InlineStorage;
        self->MaxSize = sizeof(self->InlineStorage);
        self->WritePtr = self->BufferPtr + sizeof(RPCCall);
    }
	RPCCall* q = ((RPCCall*)self->BufferPtr);

    if (xid == PLACEHOLDER_XID)
        xid = RandomXid();

    self->Size = sizeof(RPCCall) - sizeof(u32);
    q->header.size_final = 0;
    q->header.xid = xid;
    q->header.reply = 0;

    q->rpc_version = 2;
    q->program_id = prog;
    q->program_ver = prog_ver;
    q->procedure = proc;

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

static inline int32_t RPCDeserializer_BufferLeft(RPCDeserializer* self)
{
    int32_t result = self->Size -
                      ((self->ReadPtr - (u32*)self->BufferPtr) * sizeof(u32));
    return result;
}

static inline const uint32_t RPCDeserializer_ReadU32(RPCDeserializer* self)
{
    assert(RPCDeserializer_BufferLeft(self) >= 4);
    const uint32_t result = HTONL(*self->ReadPtr);
    self->ReadPtr++;
    return result;
}

static inline int RPCDeserializer_ReadBool(RPCDeserializer *self)
{
    assert(RPCDeserializer_BufferLeft(self) >= 4);
    return (*self->ReadPtr++ != 0);
}

void RPCDeserializer_Init(RPCDeserializer* self, SOCKET sock_fd);
RPCHeader RPCDeserializer_RecvHeader(RPCDeserializer* self);
void RPCDeserializer_SkipAuth(RPCDeserializer *self);

const char* RPCDeserializer_ReadString(RPCDeserializer* self
                                    , char ** writePtr, uint32_t length);

fhandle3 RPCDeserializer_ReadFileHandle(RPCDeserializer* self);
fattr3 RPCDeserializer_ReadFileAttribs(RPCDeserializer* self);
uint64_t RPCDeserializer_ReadU64(RPCDeserializer* self);
int32_t RPCDeserializer_BufferLeft(RPCDeserializer* self);
void RPCDeserializer_EnsureSize(RPCDeserializer* self, uint32_t sz);

#ifndef ALIGN4
#  define ALIGN4(VAR) (((VAR) + 3) & ~3)
#endif
