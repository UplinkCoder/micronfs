
#define _DEFAULT_SOURCE 1
#include "micronfs.h"
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/types.h>

uint32_t enumerate_directory(const char* path);

int main(int argc, char* argv[])
{
    printf("%u entries in current directory\n"
        , enumerate_directory(".")
    );
}

uint32_t enumerate_directory(const char* path)
{
    uint32_t n_entries = 0;
    DIR* dir = opendir(path);
    char errorBuf[512];

    if (!dir)
    {
        sprintf(errorBuf, "opendir(\"%s\")", path);
        perror(errorBuf);
    }
    struct dirent* entry = readdir(dir);
    while(entry)
    {
        if (entry->d_name[0] == '.')
        {
            goto LnextEntry;
        }

        //printf("%s   ", entry->d_name);
        n_entries++;
LnextEntry:
        entry = readdir(dir);
    }
    closedir(dir);

    return n_entries;
}
