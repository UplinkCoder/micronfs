#include "../nfs_common.inl"

extern int nfs_init_cache(cache_t* dirCache, int argc, char* argv[])
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

    InitCache(dirCache);
    dirCache->rootHandle = fh;
    populate_cache_cb_args_t args = {dirCache, dirCache->root};

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

    cached_dir_t root = *dirCache->root->cached_dir;
    meta_data_entry_t* one_past_last_entry =
        root.entries + root.entries_size;

    // TODO unmount on shutdown of filesystem
    // mountd_umnt(mountd_fd, "/nfs/git");
}
