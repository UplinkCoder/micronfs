#!/bin/sh
DST=$1

if [ -d "$1" ]; then
    cp micronfs.c nfsls.c rpc_serializer.c rpc_serializer.h endian.h stdint_msvc.h \
        build.bat build.sh micronfs.h nfs_common.inl sync.sh notes.txt README.md LICENSE $DST
    chmod +x $DST/build.sh $DST/sync.sh
    mkdir -p $DST/cache
    cp cache/cached_tree.c cache/cached_tree.h cache/crc32.c $DST/cache
    mkdir -p $DST/fuse
    cp fuse/cnfs_main.c fuse/micronfs_glue.c fuse/bld.sh $DST/fuse
    chmod +x $DST/fuse/bld.sh
    mkdir -p $DST/rfcs
    cp rfcs/rfc1057_sun_rpc_v2.txt  rfcs/rfc1813_nfs_v3.txt  rfcs/rfc1833_rpcbind.txt $DST/rfcs
    mkdir -p $DST/utils
    cp utils/enum_tochars.c utils/msvc_ver.c $DST/utils
else
    echo "You need to give a target directory as argument"
fi
