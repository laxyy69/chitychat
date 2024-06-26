#include "server_init.h"
#include "chat/db_def.h"
#include "server.h"
#include "server_events.h"
#include "server_ht.h"
#include "chat/cmd.h"
#include <sys/eventfd.h>

#define LISTEN_BACKLOG 100

static json_object* 
server_default_config(void)
{
    json_object* config = json_object_new_object();
    json_object_object_add(config, "root_dir", 
                           json_object_new_string("client/public"));
    json_object_object_add(config, "img_dir", 
                           json_object_new_string("client/public/upload/imgs"));
    json_object_object_add(config, "vid_dir", 
                           json_object_new_string("client/public/upload/vids"));
    json_object_object_add(config, "file_dir", 
                           json_object_new_string("client/public/upload/files"));
    json_object_object_add(config, "addr_ip", 
                           json_object_new_string("any"));
    json_object_object_add(config, "addr_port", 
                           json_object_new_int(8080));
    json_object_object_add(config, "addr_version", 
                           json_object_new_string("ipv6"));
    json_object_object_add(config, "log_level", 
                           json_object_new_string("debug"));
    json_object_object_add(config, "database_name", 
                           json_object_new_string("chitychat"));
    json_object_object_add(config, "thread_pool",
                           json_object_new_int(-1));

    return config;
}

static void 
server_chdir(const char* exe_path)
{
    char realpath_str[PATH_MAX];
    char* dir;
    realpath(exe_path, realpath_str);
    dir = dirname(dirname(realpath_str)); // Get the parent of the exe directory 

    if (chdir(dir) == -1)
        error("Failed to change directory to '%s': %s\n", dir, ERRSTR);
}

static void
print_help(const char* exe_path)
{
    printf(
        "Usage\n"\
        "\t%s [options]\n"\
        "Chity Chat server\n"\
        "\nArguments will override config.json\n\n"\
        "  -h, --help\t\t\tShow this message\n"\
        "  -v, --verbose\t\t\tSet log level to verbose\n"\
        "  -p, --port=PORT\t\tPort number to bind\n"\
        "  -d, --database-name=NAME\tPostgreSQL database name\n"\
        "  -T, --thread-pool=N\t\tSet the number of threads for the thread pool,\n"\
        "\t\t\t\tUse -1 (default) to automatically determine the number based on system threads.\n"\
        "  -6, --ipv6\t\t\tUse IPv6\n"\
        "  -4, --ipv4\t\t\tUse IPv4\n",
        exe_path
    );
}

static bool 
server_argv(server_t* server, int argc, char* const* argv)
{
    i32 opt;
    char* endptr;

    struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"verbose", 0, NULL, 'v'},
        {"database-name", required_argument, NULL, 'd'},
        {"ipv6", 0, NULL, '6'},
        {"ipv4", 0, NULL, '4'},
        {"fork", 0, NULL, 'f'},
        {"help", 0, NULL, 'h'},
        {"thread-pool", required_argument, NULL, 'T'},
        {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "T:p:d:v46hf", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 'p':
            {
                u32 port = strtoul(optarg, &endptr, 10);
                if (port == 0)
                {
                    error("Invalid port: '%s'\n", optarg);
                    return false;
                }
                else if (port > UINT16_MAX)
                {
                    error("Port '%u' too large (> %u)\n", port, UINT16_MAX);
                    return false;
                }
                server->conf.addr_port = port;
                break;
            }
            case 'v':
                server_set_loglevel(SERVER_VERBOSE);
                break;
            case 'd':
                strncpy(server->conf.database, optarg, NAME_MAX);
                break;
            case '4':
                server->conf.addr_version = IPv4;
                break;
            case '6':
                server->conf.addr_version = IPv6;
                break;
            case 'h':
                print_help(argv[0]);
                return false;
            case 'f':
                server->conf.fork = true;
                break;
            case 'T':
                server->conf.thread_pool = atoi(optarg);
                break;
            case '?':
                error("Unknown or missing argument\n");
                return false;
        }
    }

    return true;
}

static bool        
server_load_config(server_t* server, int argc, char* const* argv)
{
#define JSON_GET(x) json_object_object_get(config, x)

    i32 fd = -1;
    json_object* config = NULL;
    json_object* root_dir;
    json_object* img_dir;
    json_object* vid_dir;
    json_object* file_dir;
    json_object* addr_ip;
    json_object* addr_port;
    json_object* addr_version;
    json_object* database;
    json_object* log_level_json;
    json_object* thread_pool_json;
    const char* root_dir_str;
    const char* img_dir_str;
    const char* vid_dir_str;
    const char* file_dir_str;
    const char* addr_ip_str;
    const char* addr_version_str;
    const char* database_str;
    const char* loglevel_str;
    const char* thread_pool_str;
    enum server_log_level log_level = SERVER_DEBUG;
    const char* config_path = SERVER_CONFIG_PATH;

    server_chdir(argv[0]);

    fd = open(config_path, O_RDONLY);
    if (fd == -1)
    {
        if (errno == ENOENT)
        {
            config = server_default_config();
            if (!config)
            {
                fatal("Failed to create default config\n");
                return false;
            }

            fd = open(config_path, O_RDWR | O_CREAT, 
                        S_IRUSR | S_IWUSR);
            if (fd == -1)
            {
                fatal("Creating file %s failed %d (%s\n)",
                        config_path, errno, ERRSTR);
                return false;
            }

            i32 ret = json_object_to_fd(fd, config, 
                JSON_C_TO_STRING_PRETTY | 
                JSON_C_TO_STRING_PRETTY_TAB | 
                JSON_C_TO_STRING_SPACED | 
                JSON_C_TO_STRING_NOSLASHESCAPE);

            close(fd);
            fd = -1;
            if (ret == -1)
            {
                fatal("json_object_to_fd: %s\n",
                    json_util_get_last_err());
                return false;
            }
        }
        else
        {
            fatal("open %s failed error %d (%s)\n",
                config_path, errno, ERRSTR);
            return false;
        }
    }
    if (!config)
        config = json_object_from_fd(fd);
    if (fd != -1)
        close(fd);
    if (!config)
    {
        fatal("json_object_from_file: %s\n",
            json_util_get_last_err());
        return false;
    }

    root_dir = JSON_GET("root_dir");
    root_dir_str = json_object_get_string(root_dir);
    strncpy(server->conf.root_dir, root_dir_str, CONFIG_PATH_LEN);

    img_dir = JSON_GET("img_dir");
    img_dir_str = json_object_get_string(img_dir);
    strncpy(server->conf.img_dir, img_dir_str, CONFIG_PATH_LEN);

    vid_dir = JSON_GET("vid_dir");
    vid_dir_str = json_object_get_string(vid_dir);
    strncpy(server->conf.vid_dir, vid_dir_str, CONFIG_PATH_LEN);

    file_dir = JSON_GET("file_dir");
    file_dir_str = json_object_get_string(file_dir);
    strncpy(server->conf.file_dir, file_dir_str, CONFIG_PATH_LEN);

    addr_ip = JSON_GET("addr_ip");
    addr_ip_str = json_object_get_string(addr_ip);
    strncpy(server->conf.addr_ip, addr_ip_str, INET6_ADDRSTRLEN);

    addr_port = JSON_GET("addr_port");
    server->conf.addr_port = json_object_get_int(addr_port);

    addr_version = JSON_GET("addr_version");
    addr_version_str = json_object_get_string(addr_version);
    if (!strcmp(addr_version_str, "ipv4"))
        server->conf.addr_version = IPv4;
    else if (!strcmp(addr_version_str, "ipv6"))
        server->conf.addr_version = IPv6;
    else
        warn("Config: addr_version: \"%s\"? Default to IPv4\n", addr_version_str);

    database = JSON_GET("database_name");
    database_str = json_object_get_string(database);
    strncpy(server->conf.database, database_str, CONFIG_PATH_LEN);

    thread_pool_json = JSON_GET("thread_pool");
    thread_pool_str = json_object_get_string(thread_pool_json);
    server->conf.thread_pool = atoi(thread_pool_str);

    log_level_json = JSON_GET("log_level");
    if (log_level_json)
    {
        loglevel_str = json_object_get_string(log_level_json);

        if (!strcmp(loglevel_str, "fatal"))
            log_level = SERVER_FATAL;
        else if (!strcmp(loglevel_str, "error"))
            log_level = SERVER_ERROR;
        else if (!strcmp(loglevel_str, "warn") || !strcmp(loglevel_str, "warning"))
            log_level = SERVER_WARN;
        else if (!strcmp(loglevel_str, "info"))
            log_level = SERVER_INFO;
        else if (!strcmp(loglevel_str, "debug"))
            log_level = SERVER_DEBUG;
        else if (!strcmp(loglevel_str, "verbose"))
            log_level = SERVER_VERBOSE;
        else
        {
            fatal("JSON %s invalid \"log_level\": %s\n", config_path, loglevel_str);
            json_object_put(config);
            return false;
        }
    }

    server_set_loglevel(log_level);
    json_object_put(config);

    if (!server_argv(server, argc, argv))
        return false;

    verbose("Setting log level: %d\n", log_level);

    server->conf.sql_schema = "server/sql/schema.sql";

    server->conf.sql_insert_user = "server/sql/insert_user.sql";
    server->conf.sql_select_user = "server/sql/select_user.sql";

    server->conf.sql_insert_group = "server/sql/insert_group.sql";
    server->conf.sql_select_group = "server/sql/select_group.sql";

    server->conf.sql_insert_groupmember_code = "server/sql/insert_groupmember_code.sql";
    server->conf.sql_select_groupmember = "server/sql/select_groupmember.sql";

    server->conf.sql_insert_msg = "server/sql/insert_msg.sql";
    server->conf.sql_select_msg = "server/sql/select_msg.sql";

    server->conf.sql_update_user = "server/sql/update_user.sql";

    server->conf.sql_insert_userfiles = "server/sql/insert_userfiles.sql";

    if (server->conf.thread_pool == -1)
        server->conf.thread_pool = server_tm_system_threads();

    return true;
}

static bool 
server_init_socket(server_t* server)
{
    int domain;

    if (server->conf.addr_version == IPv4)
    {
        domain = AF_INET;
        server->addr_len = sizeof(struct sockaddr_in);
        server->addr_in.sin_family = AF_INET;
        server->addr_in.sin_port = htons(server->conf.addr_port);

        if (!strncmp(server->conf.addr_ip, "any", 3))
            server->addr_in.sin_addr.s_addr = INADDR_ANY;
        else
        {
            server->addr_in.sin_addr.s_addr = inet_addr(server->conf.addr_ip);
            if (server->addr_in.sin_addr.s_addr == INADDR_NONE)
            {
                fatal("Invalid IP in address: '%s'.\n", server->conf.addr_ip);
                return false;
            }
        }
    }
    else
    {
        domain = AF_INET6;
        server->addr_len = sizeof(struct sockaddr_in6);
        server->addr_in6.sin6_family = AF_INET6;
        server->addr_in6.sin6_port = htons(server->conf.addr_port);
        if (!strncmp(server->conf.addr_ip, "any", 3))
            server->addr_in6.sin6_addr = in6addr_any;
        else
        {
            if (inet_pton(AF_INET6, server->conf.addr_ip, &server->addr_in6.sin6_addr) <= 0)
            {
                fatal("inet_pton: Invalid IPv6 address: '%s'\n", server->conf.addr_ip);
                return false;
            }
        }
    }

    server->addr = (struct sockaddr*)&server->addr_in;

    server->sock = socket(domain, SOCK_STREAM, 0);
    if (server->sock == -1)
    {
        fatal("socket: %s\n", strerror(errno));
        return false;
    }

    int opt = 1;
    if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &opt, server->addr_len) == -1)
        error("setsockopt: %s\n", strerror(errno));

    if (bind(server->sock, server->addr, server->addr_len) == -1)
    {   
        fatal("bind: %s\n", strerror(errno));
        return false;
    }

    if (listen(server->sock, LISTEN_BACKLOG) == -1)
    {
        fatal("listen: %s\n", strerror(errno)); 
        return false;
    }

    return true;
}

static bool 
server_init_epoll(server_t* server)
{
    server->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (server->epfd == -1)
    {
        fatal("epoll_create1: %s\n", ERRSTR);
        return false;
    }

    if (server_new_event(server, server->sock, NULL, se_accept_conn, NULL) == NULL)
        return false;
    
    return true;
}

static bool 
server_init_ssl(server_t* server)
{
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_SSL_strings();
    OpenSSL_add_all_algorithms();

    server->ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!server->ssl_ctx)
    {
        error("SSL_CTX_new() returned NULL!\n");
        return false;
    }

    SSL_CTX_set_options(server->ssl_ctx, SSL_OP_SINGLE_DH_USE);
    SSL_CTX_set_ecdh_auto(server->ssl_ctx, 1);
    if (SSL_CTX_use_certificate_file(server->ssl_ctx, "server/server.crt", SSL_FILETYPE_PEM) <= 0)
    {
        error("SSL cert failed: %s\n", ERRSTR);
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, "server/server.key", SSL_FILETYPE_PEM) <= 0)   
    {
        error("SSL private key failed: %s\n", ERRSTR);
        return false;
    }

    return true;
}

static bool 
server_init_ht(server_t* server)
{
    const size_t ht_size = 10;

    if (server_ght_init(&server->event_ht, ht_size, NULL) == false)
        return false;

    if (server_ght_init(&server->client_ht, ht_size, NULL) == false)
        return false;

    if (server_ght_init(&server->user_ht, ht_size, NULL) == false)
        return false;

    if (server_ght_init(&server->session_ht, ht_size, NULL) == false)
        return false;

    if (server_ght_init(&server->upload_token_ht, ht_size, NULL) == false)
        return false;

    if (server_init_chatcmd(server) == false)
        return false;

    return true;
}

static enum se_status
eventfd_dummy_read(eworker_t* ew, UNUSED server_event_t* ev)
{
    /*
     * Won't read eventfd.
     * Make all threads wake up from epoll_wait().
     */
    debug("eventfd_dummy_read() from %d\n", ew->tid);
    return SE_OK;
}

static bool
server_init_eventfd(server_t* server)
{
    server_event_t* se;

    server->eventfd = eventfd(0, 0);
    if (server->eventfd == -1)
    {
        fatal("eventfd: %s\n", ERRSTR);
        return false;
    }

    se = server_new_event(server, server->eventfd, NULL, eventfd_dummy_read, NULL);
    if (se == NULL)
        return false;

    /*
     * Default server_new_event() will use EPOLLONESHOT,
     * in this case we don't, we want all threads get this event.
     */
    se->listen_events = EPOLLIN;
    if (server_event_rearm(server, se) == -1)
        return false;

    return true;
}

server_t*   
server_init(int argc, char* const* argv)
{
    server_t* server;

    server = calloc(1, sizeof(server_t));
    if (!server)
    {
        fatal("calloc() failed.\n");
        return NULL;
    }

    // Load config and command line arguments
    if (!server_load_config(server, argc, argv))
        goto error;

    // if --fork is used, fork and exit parent process 
    // e.i. becomes a background process
    if (server->conf.fork)
    {
        if (fork() != 0)
        {
            // Parent
            exit(0);
        }
    }

    // Self-Explanatory
    if (!server_init_socket(server))
        goto error;

    // Init Hash Tables
    if (!server_init_ht(server))
        goto error;

    // Init Linux's Event Poll
    if (!server_init_epoll(server))
        goto error;

    // Init DataBase
    if (!server_init_db(server))
        goto error;

    // Init libmagic (for file mime types)
    if (!server_init_magic(server))
        goto error;

    // Init OpenSSL
    if (!server_init_ssl(server))
        goto error;

    // Init EventFD (to wake up threads from epoll_wait())
    if (!server_init_eventfd(server))
        goto error;

    if (!server_init_signal(server))
        goto error;

    // Init Thread Manager
    if (!server_init_tm(server, server->conf.thread_pool))
        goto error;

    server->running = true;

    return server;
error:;
    server_cleanup(server);
    return NULL;
}
