#!/bin/bash
if [ -z $CC ];
then
    CC=cc
fi
$CC -Os -march=native -mtune=native cache/cached_tree.c micronfs.c nfsls.c rpc_serializer.c -o nfsls $@
