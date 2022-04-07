#include "nfs_common.inl"

int main(int argc, char* argv[])
{
#ifdef _WIN32
	WSADATA  wsaData;
	WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
    char* hostname = "192.168.178.26";
    if (argc == 2)
        hostname = argv[1];

    /* Create socket. */
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock == INVALID_SOCKET) {
#ifdef _WIN32
	printf("WSAGetLastError: %d\n", WSAGetLastError());
#else
      perror("opening stream socket");
#endif
      exit(1);
    }

    int addr_len = sizeof(s_client);

    int portmap_fd = connect_name(hostname, "111");
    assert(portmap_fd != -1);

    // portmap_nullCall(portmap_fd);

    uint16_t mountd_port
        = portmap_getport(portmap_fd, MOUNT_PROGRAM, 3, PROTO_TCP);

    if (!mountd_port)
    {
        fprintf(stderr, "Could not find port for mountd service\n");
        return -1;
    }

    char port_str_[8];
    char* port_str = port_str_ + sizeof(port_str_) - 1;
    if (mountd_port)
    {
        int port = mountd_port;
        *port_str = '\0';
        int i = 0;
        for(; port; )
        {
            (--port_str)[i] = ((port % 10) + '0');
            port /= 10;
        }
    }

    int mountd_fd = connect_name(hostname, port_str);
    assert(mountd_fd != -1);

    mountlist_t* mounts = mountd_dump(mountd_fd);
    for(mountlist_t* m = mounts; m; m = m->next)
    {
        printf("hostname: %s directory: %s\n",
             m->hostname,  m->directory);
    }

    fhandle3 fh = mountd_mnt(mountd_fd, "/nfs/git");
    printFileHandle(&fh);

    int nfs_fd = connect_name("192.168.178.26", "2049");
    cookie3 cookie = 0;
    cookie3 verifier = 0;
/*
    read_more = nfs_readdir(nfs_fd, &fh
          , &cookie, &verifier
          , myCallBack
    );
*/

    cache_t dirCache;
    InitCache(&dirCache);
    dirCache.rootHandle = fh;
    populate_cache_cb_args_t args = {&dirCache, dirCache.root};
    struct search_dir_t searchResult = {"ll.txt"};

    for(;;) {
        cookie3 old_cookie = cookie;
        int shouldContinueReading =
        nfs_readdirplus(nfs_fd, &fh
              , &cookie, &verifier
              , populateCache_cb, &args
        );
        if (!shouldContinueReading)
            break;
    }

    cached_dir_t root = *dirCache.root->cached_dir;
    meta_data_entry_t* one_past_last_entry =
        root.entries + root.entries_size;

    printf("root_entrires:\n");
    for(meta_data_entry_t* entry = root.entries;
         entry < one_past_last_entry; entry++)
    {
        printf("%d: ", (int)(entry - root.entries));
        printf("\t%s\n", dirCache.name_stringtable + (entry->name.v - 4));

        if (entry->type == ENTRY_TYPE_DIRECTORY)
        {
            printf("d");
            for(meta_data_entry_t* e = entry->cached_dir->entries;
                e < entry->cached_dir->entries + entry->cached_dir->entries_size;
                e++)
            {
                printf("\t\t%s\n", dirCache.name_stringtable + (e->name.v -4));
            }
        }
        const fhandle3 handle = ptrToHandle(&dirCache, entry->handle);
        // printFileHandle(&handle);
    }
    meta_data_entry_t* div =
        LookupInDirectory(&dirCache, dirCache.root->cached_dir, "division.html", strlen("division.html"));
    if (div && div->type == ENTRY_TYPE_FILE)
    {
        cached_file_t file = *div->cached_file;
        printf("div: %d %d --- %s\n", div->type, file.size, dirCache.name_stringtable + (div->name.v - 4));
    }
    printf("Used %d bytes for handles\n", dirCache.limbs_size * 4);
    printf("Created %d entires\n", dirCache.metadata_size);
    if(handleSum(&searchResult.result_handle))
    {
       printFileHandle(&searchResult.result_handle);
       char buf[512];
       int size_read =
            nfs_read(nfs_fd, &searchResult.result_handle, buf, sizeof(buf), 0);
       buf[size_read] = '\0';

       printf("data read: %s\n", buf);
       // nfs_read(nfs_fs, &searchResult.handle
    }

    mountd_umnt(mountd_fd, "/nfs/git");

#if 0
    while(nfs_fd == -1)
    {
        printf("Calling accept\n");
        nfs_fd = accept(sock, (struct sockaddr*)&s_client, &addr_len);
    }

    // send request

    // read reply
    int sz = recv(nfs_fd, recvbuffer, MAX_BUFFER_SIZE, 0);

    printf ("We've got %d bytes yay\n", sz);
    closesocket(nfs_fd);


    for(int i = 0; i < sz; i++)
    {
        printf("%x ", recvbuffer[i]);
        if ((i & 3) == 3) printf("\n");
    }
    printf("\n");

    RPCReply reply = *(RPCReply*)recvbuffer;

    uint32_t network_order_xid = reply.header.xid;
    // RPCReply_ByteFlip(&reply);
    printf("xid: %d\n", network_order_xid);

    int k = 12;

    getchar();

    //remove("test.txt");
#endif
    printf("It's time to say Goodbye :)\n");


    // int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
}
