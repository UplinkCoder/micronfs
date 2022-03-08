#include "micronfs.h"

const char* nfsstat3_toChars(nfsstat3 self)
{
    const char* result = 0;

    if (self == 0)
        result = "NFS3ERR_OK";
    if (self == 1)
        result = "NFS3ERR_PERM";
    if (self == 2)
        result = "NFS3ERR_NOENT";
    if (self == 5)
        result = "NFS3ERR_IO";
    if (self == 6)
        result = "NFS3ERR_NXIO";
    if (self == 13)
        result = "NFS3ERR_ACCES";
    if (self == 17)
        result = "NFS3ERR_EXIST";
    if (self == 18)
        result = "NFS3ERR_XDEV";
    if (self == 19)
        result = "NFS3ERR_NODEV";
    if (self == 20)
        result = "NFS3ERR_NOTDIR";
    if (self == 21)
        result = "NFS3ERR_ISDIR";
    if (self == 22)
        result = "NFS3ERR_INVAL";
    if (self == 27)
        result = "NFS3ERR_FBIG";
    if (self == 28)
        result = "NFS3ERR_NOSPC";
    if (self == 30)
        result = "NFS3ERR_ROFS";
    if (self == 31)
        result = "NFS3ERR_MLINK";
    if (self == 63)
        result = "NFS3ERR_NAMETOOLONG";
    if (self == 66)
        result = "NFS3ERR_NOTEMPTY";
    if (self == 69)
        result = "NFS3ERR_DQUOT";
    if (self == 70)
        result = "NFS3ERR_STALE";
    if (self == 71)
        result = "NFS3ERR_REMOTE";
    if (self == 10001)
        result = "NFS3ERR_BADHANDLE";
    if (self == 10002)
        result = "NFS3ERR_NOT_SYNC";
    if (self == 10003)
        result = "NFS3ERR_BAD_COOKIE";
    if (self == 10004)
        result = "NFS3ERR_NOTSUPP";
    if (self == 10005)
        result = "NFS3ERR_TOOSMALL";
    if (self == 10006)
        result = "NFS3ERR_SERVERFAULT";
    if (self == 10007)
        result = "NFS3ERR_BADTYPE";
    if (self == 10008)
        result = "NFS3ERR_JUKEBOX";

    return result;
}

const char* ftype3_toChars(ftype3 self)
{
    const char* result = 0;

    if (self == 0)
        result = "NF3NON";
    if (self == 1)
        result = "NF3REG";
    if (self == 2)
        result = "NF3DIR";
    if (self == 3)
        result = "NF3BLK";
    if (self == 4)
        result = "NF3CHR";
    if (self == 5)
        result = "NF3LNK";
    if (self == 6)
        result = "NF3SOCK";
    if (self == 7)
        result = "NF3FIFO";

    return result;
}

const char* stable_how_toChars(stable_how self)
{
    const char* result = 0;

    if (self == 0)
        result = "UNSTABLE ";
    if (self == 1)
        result = "DATA_SYNC ";
    if (self == 2)
        result = "FILE_SYNC ";

    return result;
}

const char* mountstat3_toChars(mountstat3 self)
{
    const char* result = 0;

    if (self == 0)
        result = "MNT3_OK";
    if (self == 1)
        result = "MNT3ERR_PERM";
    if (self == 2)
        result = "MNT3ERR_NOENT";
    if (self == 5)
        result = "MNT3ERR_IO";
    if (self == 13)
        result = "MNT3ERR_ACCES";
    if (self == 20)
        result = "MNT3ERR_NOTDIR";
    if (self == 22)
        result = "MNT3ERR_INVAL";
    if (self == 63)
        result = "MNT3ERR_NAMETOOLONG";
    if (self == 10004)
        result = "MNT3ERR_NOTSUPP";
    if (self == 10006)
        result = "MNT3ERR_SERVERFAULT";

    return result;
}

const char* nlm4_stats_toChars(nlm4_stats self)
{
    const char* result = 0;

    if (self == 0)
        result = "NLM4_GRANTED";
    if (self == 1)
        result = "NLM4_DENIED";
    if (self == 2)
        result = "NLM4_DENIED_NOLOCKS";
    if (self == 3)
        result = "NLM4_BLOCKED";
    if (self == 4)
        result = "NLM4_DENIED_GRACE_PERIOD";
    if (self == 5)
        result = "NLM4_DEADLCK";
    if (self == 6)
        result = "NLM4_ROFS";
    if (self == 7)
        result = "NLM4_STALE_FH";
    if (self == 8)
        result = "NLM4_FBIG";
    if (self == 9)
        result = "NLM4_FAILED";

    return result;
}
