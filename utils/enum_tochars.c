/**
    Crappy tool to generate toChars functions for enums based on the NFS RFC
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

typedef struct enum_def_t
{
    const char* name;
    uint8_t sz_name;

    const char** member_names;
    const char** member_values;
    uint8_t* sz_members;
    uint8_t* sz_values;
    uint8_t n_members;
    const char* end;
} enum_def_t;

enum_def_t findEnum(const char* start)
{
    char* m_names[256];
    uint8_t sz_m_names[256];
    char* m_values[256];
    uint8_t sz_m_values[256];

    uint16_t n_members = 0;

    enum_def_t result;

    char* found;
    if ((found = strstr(start, " enum ")))
    {
        found += sizeof(" enum");
        assert(*(found - 1) == ' ');
        while(*found == ' ')
            found++;

        result.name = found;
        result.n_members = 0;
        result.sz_name = 0;

        while(*found++ != ' ')
        {
            result.sz_name++;
        }

        while(*found++ != '{') {}

        char** nameP = m_names;
        char** valueP = m_values;
        uint8_t* sz_nameP = sz_m_names;
        uint8_t* sz_valueP = sz_m_values;

        _Bool saw_space_after_name = 0;
        *sz_nameP = 0;
        *sz_valueP = 0;
        for(;;)
        {
            if ((*found >= 'A' && *found <= 'Z') || *found == '_')
            {
                if(*sz_nameP == 0)
                {
                    *sz_nameP = 1;
                    *nameP = found;
                }
                (*sz_nameP)++;
            }
            if ((*found >= '0' && *found <= '9') && saw_space_after_name)
            {
                if(*sz_valueP == 0)
                {
                    // *sz_valueP = 1;
                    *valueP = found;
                }
                (*sz_valueP)++;
            }

            if (*found == ',')
            {
                saw_space_after_name = 0;
                valueP++;
                nameP++;
                *(++sz_nameP) = 0;
                *(++sz_valueP) = 0;
                result.n_members++;
            }
            if ((*found == ' ') && *sz_nameP)
                saw_space_after_name = 1;

            if (*found == '}')
            {
                result.end = found;
                result.n_members++;

                result.sz_members = (uint8_t*)
                    malloc(result.n_members);
                result.sz_values =  (uint8_t*)
                    malloc(result.n_members);
                result.member_names =  (char**)
                    malloc(result.n_members * sizeof(char**));
                result.member_values =  (char**)
                    malloc(result.n_members * sizeof(char**));

                for(int i = 0; i < result.n_members; i++)
                {
                    result.member_names[i]
                        = m_names[i];
                    result.member_values[i]
                        = m_values[i];

                    result.sz_members[i]
                        = sz_m_names[i];
                    result.sz_values[i]
                        = sz_m_values[i];
                }
                break;
            }

            found++;
        }
        return result;
    }
    result.name = 0;
    return result;
}

int main(int argc, char* argv[])
{
    if (argc != 2)
        return -1;

    FILE* fd = fopen(argv[1], "r");
    if (fd)
    {
        fseek(fd, 0, SEEK_END);
        uint32_t sz = ftell(fd);
        fseek(fd, 0, SEEK_SET);
        char* buffer = (char*)malloc(sz + 1);
        fread(buffer, 1, sz, fd);
        buffer[sz] = '\0';

        char path[256];
        sprintf(path, "%s.out", argv[1]);
        enum_def_t r = findEnum(buffer);
        FILE* fout = fopen(path, "w+");

        while (r.name)
        {

            fprintf(fout, "const char* %.*s_toChars(%.*s self)\n{\n",
                r.sz_name, r.name, r.sz_name, r.name);

            for(int i = 0; i < r.n_members;i++)
            {
                fprintf(fout, "    if (self == %.*s)\n        result = \"%.*s\";\n",
                        (int)r.sz_values[i], r.member_values[i],
                        (int)r.sz_members[i], r.member_names[i]);
            }
            fprintf(fout, "}\n\n");
            r = findEnum(r.end);
        }
        fclose(fout);

    }
    return 0;
}

