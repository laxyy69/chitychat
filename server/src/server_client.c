#include "server_client.h"
#include "server.h"

client_t*   
server_get_client_fd(server_t* server, i32 fd)
{
    return server_ght_get(&server->client_ht, fd);
}

client_t*   
server_get_client_user_id(server_t* server, u64 id)
{
    return server_ght_get(&server->user_ht, id);
}

client_t*
server_accept_client(eworker_t* th)
{
    client_t* client;
    server_t* server = th->server;

    client = calloc(1, sizeof(client_t));
    client->addr.len = server->addr_len;
    client->addr.version = server->conf.addr_version;
    client->addr.addr_ptr = (struct sockaddr*)&client->addr.ipv4;
    client->addr.sock = accept(server->sock, client->addr.addr_ptr, &client->addr.len);
    if (client->addr.sock == -1)
    {
        error("accept: %s", ERRSTR);
        goto err;
    }
    if (server_client_ssl_handsake(server, client) == -1)
        goto err;
    server_get_client_info(client);
    pthread_mutex_init(&client->ssl_mutex, NULL);
    server_ght_insert(&server->client_ht, client->addr.sock, client);
    if (server_new_event(server, client->addr.sock, client, 
                         se_read_client, se_close_client) == NULL)
        goto err;

    return client;
err:
    server_free_client(th, client);
    return NULL;
}

void 
server_free_client(eworker_t* ew, client_t* client)
{
    server_t* server = ew->server;

    if (!client)
        return;
    server_ght_del(&server->client_ht, client->addr.sock);

    info("Client (fd:%d, IP: %s:%s, host: %s) disconnected.\n", 
            client->addr.sock, client->addr.ip_str, client->addr.serv, client->addr.host);
    if (client->dbuser)
    {
        if (client->dbuser->user_id)
            debug("\tUser:%u %s '%s' logged out.\n", 
                client->dbuser->user_id, client->dbuser->username, client->dbuser->displayname);
    }

    if (client->ssl)
    {
        if (client->err == CLIENT_ERR_NONE)
            SSL_shutdown(client->ssl);
        SSL_free(client->ssl);
    }

    if (client->session && client->session->timerfd == 0 && ew->server->running)
    {
        union timer_data data = {
            .session = client->session
        };
        // TODO: Make client session timer configurable
        server_timer_t* timer = server_addtimer(ew, MINUTES(30), 
                                                TIMER_ONCE, TIMER_CLIENT_SESSION, 
                                                &data, sizeof(void*));
        if (timer)
            client->session->timerfd = timer->fd;
    }

    if (client->recv.data)
        free(client->recv.data);
    if (client->dbuser)
    {
        server_ght_del(&ew->server->user_ht, client->dbuser->user_id);
        free(client->dbuser);
    }
    close(client->addr.sock);

    pthread_mutex_destroy(&client->ssl_mutex);
    free(client);
}

int 
server_client_ssl_handsake(server_t* server, client_t* client)
{
    i32 ret;
    client->ssl = SSL_new(server->ssl_ctx);
    if (!client->ssl)
    {
        error("SSL_new() failed.\n");
        return -1;
    }
    SSL_set_fd(client->ssl, client->addr.sock);
    ret = SSL_accept(client->ssl);
    if (ret == 1)
        return 0;
    server_set_client_err(client, CLIENT_ERR_SSL);
    return 0;
}

void 
server_get_client_info(client_t* client)
{
    i32 ret;
    i32 domain;

    ret = getnameinfo(client->addr.addr_ptr, client->addr.len, client->addr.host, NI_MAXHOST, client->addr.serv, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
    if (ret == -1)
    {
        error("getnameinfo: %s\n", ERRSTR);
    }

    if (client->addr.version == IPv4)
    {
        domain = AF_INET;
        inet_ntop(domain, &client->addr.ipv4.sin_addr, client->addr.ip_str, INET_ADDRSTRLEN);
    }
    else
    {
        domain = AF_INET6;
        inet_ntop(domain, &client->addr.ipv6.sin6_addr, client->addr.ip_str, INET6_ADDRSTRLEN);
    }
}

void        
server_set_client_err(client_t* client, u16 err)
{
    client->err = err;
}
