#include <stdint.h>

typedef enum nfsstat3 {
    NFS3ERR_OK          = 0,
    NFS3ERR_PERM        = 1,
    NFS3ERR_NOENT       = 2,

    NFS3ERR_IO          = 5,
    NFS3ERR_NXIO        = 6,

    NFS3ERR_ACCES       = 13,

    NFS3ERR_EXIST       = 17,
    NFS3ERR_XDEV        = 18,
    NFS3ERR_NODEV       = 19,
    NFS3ERR_NOTDIR      = 20,
    NFS3ERR_ISDIR       = 21,
    NFS3ERR_INVAL       = 22,

    NFS3ERR_FBIG        = 27,
    NFS3ERR_NOSPC       = 28,
    NFS3ERR_ROFS        = 30,
    NFS3ERR_MLINK       = 31,

    NFS3ERR_NAMETOOLONG = 63,

    NFS3ERR_NOTEMPTY    = 66,

    NFS3ERR_DQUOT       = 69,

    NFS3ERR_STALE       = 70,
    NFS3ERR_REMOTE      = 71,

    NFS3ERR_BADHANDLE   = 10001,
    NFS3ERR_NOT_SYNC    = 10002,
    NFS3ERR_BAD_COOKIE  = 10003,
    NFS3ERR_NOTSUPP     = 10004,
    NFS3ERR_TOOSMALL    = 10005,
    NFS3ERR_SERVERFAULT = 10006,
    NFS3ERR_BADTYPE     = 10007,
    NFS3ERR_JUKEBOX     = 10008
} nfsstat3;


typedef enum ftype3 {
    NF3NON    = 0,
    NF3REG    = 1,
    NF3DIR    = 2,
    NF3BLK    = 3,
    NF3CHR    = 4,
    NF3LNK    = 5,
    NF3SOCK   = 6,
    NF3FIFO   = 7
} ftype3;

typedef enum time_how {
    DONT_CHANGE        = 0,
    SET_TO_SERVER_TIME = 1,
    SET_TO_CLIENT_TIME = 2
} time_how;


typedef enum stable_how {
    UNSTABLE  = 0,
    DATA_SYNC = 1,
    FILE_SYNC = 2
} stable_how;

typedef enum createmode3 {
    UNCHECKED = 0,
    GUARDED   = 1,
    EXCLUSIVE = 2
} createmode3;

typedef enum mountstat3 {
    MNT3_OK = 0,                 /* no error */
    MNT3ERR_PERM = 1,            /* Not owner */
    MNT3ERR_NOENT = 2,           /* No such file or directory */
    MNT3ERR_IO = 5,              /* I/O error */
    MNT3ERR_ACCES = 13,          /* Permission denied */
    MNT3ERR_NOTDIR = 20,         /* Not a directory */
    MNT3ERR_INVAL = 22,          /* Invalid argument */
    MNT3ERR_NAMETOOLONG = 63,    /* Filename too long */
    MNT3ERR_NOTSUPP = 10004,     /* Operation not supported */
    MNT3ERR_SERVERFAULT = 10006  /* A failure on the server */
} mountstat3;

typedef enum nlm4_stats {
    NLM4_GRANTED = 0,
    NLM4_DENIED = 1,
    NLM4_DENIED_NOLOCKS = 2,
    NLM4_BLOCKED = 3,
    NLM4_DENIED_GRACE_PERIOD = 4,
    NLM4_DEADLCK = 5,
    NLM4_ROFS = 6,
    NLM4_STALE_FH = 7,
    NLM4_FBIG = 8,
    NLM4_FAILED = 9
} nlm4_stats;

const char* nfsstat3_toChars(nfsstat3 self);
const char* ftype3_toChars(ftype3 self);
const char* stable_how_toChars(stable_how self);
const char* mountstat3_toChars(mountstat3 self);
const char* nlm4_stats_toChars(nlm4_stats self);

typedef uint32_t uid3;
typedef uint32_t gid3;
typedef uint64_t size3;
typedef uint64_t offset3;
typedef uint32_t mode3;
typedef uint64_t fileid3;
typedef uint64_t cookie3;

#pragma pack(push, 1)
typedef struct nfstime3 {
    uint32_t   seconds;
    uint32_t   nseconds;
} nfstime3;

typedef struct specdata3 {
    uint32_t   specdata1;
    uint32_t   specdata2;
} specdata3;

typedef struct fattr3 {
    ftype3     type;
    mode3      mode;
    uint32_t   nlink;
    uid3       uid;
    gid3       gid;
    size3      size;
    size3      used;
    specdata3  rdev;
    uint64_t   fsid;
    fileid3    fileid;
    nfstime3   atime;
    nfstime3   mtime;
    nfstime3   ctime;
} fattr3;
#pragma pack(pop)
