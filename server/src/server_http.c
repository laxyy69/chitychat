#include "server_http.h"
#include "server.h"

#define NAME_CMP(x) !strncmp(header->name, x, HTTP_HEAD_NAME_LEN)

http_to_str_t 
http_to_str(const http_t* http)
{
    http_to_str_t to_str;
    size_t size;

#define HEADER_LINE_LEN (sizeof(http_header_t) + sizeof(HTTP_NL) + sizeof(": "))

    size = (HEADER_LINE_LEN * http->n_headers) + sizeof(HTTP_END);
    if (http->type == HTTP_REQUEST)
        size += sizeof(http_req_t);
    else
        size += sizeof(http_resp_t);

    if (http->body)
        size += http->body_len;
    
    to_str.max = size;
    to_str.str = calloc(1, size);


    if (http->type == HTTP_REQUEST)
        snprintf(to_str.str, size, "%s %s %s" HTTP_NL, http->req.method, http->req.url, http->req.version);
    else
        snprintf(to_str.str, size, "%s %u %s" HTTP_NL, http->resp.version, http->resp.code, http->resp.msg);

    for (size_t i = 0; i < http->n_headers; i++)
    {
        const http_header_t* header = &http->headers[i];
        if (header->name[0] == 0x00 || header->val[0] == 0x00)
            continue;

        char header_line[HEADER_LINE_LEN];
        snprintf(header_line, HEADER_LINE_LEN, "%s: %s%s", 
            header->name,
            header->val, 
            (i + 1 >= http->n_headers) ? "" : HTTP_NL // New line or not
        );
        strcat(to_str.str, header_line);
    }

    strcat(to_str.str, HTTP_END);
    size_t current_len = strnlen(to_str.str, to_str.max);
    if (http->body)
        memcpy(to_str.str + current_len, http->body, http->body_len);

    to_str.len = current_len + http->body_len;

    return to_str;
}

static void 
set_client_connection(client_t* client, http_header_t* header)
{
    char* connection;
    char* saveptr;
    char* token;

    connection = header->val;
    token = strtok_r(connection, ", ", &saveptr);

    while (token)
    {
        if (!strncmp(token, "keep-alive", HTTP_HEAD_VAL_LEN))
            client->state |= CLIENT_STATE_KEEP_ALIVE;
        else if (!strncmp(token, "close", HTTP_HEAD_VAL_LEN))
            client->state = CLIENT_STATE_SHORT_LIVE;
        else if (!strncmp(token, "Upgrade", HTTP_HEAD_VAL_LEN))
            client->state |= CLIENT_STATE_UPGRADE_PENDING;
        else
            warn("HTTP Connection: '%s' not implemented.\n", token);

        token = strtok_r(NULL, ", ", &saveptr);
    }
}

static void 
handle_http_upgrade(client_t* client, http_header_t* header)
{
    if (client->state & CLIENT_STATE_UPGRADE_PENDING)
    {
        if (!strncmp(header->val, "websocket", HTTP_HEAD_VAL_LEN))
        {
            client->state |= CLIENT_STATE_WEBSOCKET;
        }
        else
        {
            warn("Connection upgrade '%s' not implemented!\n", header->val);
        }
    }
    else
    {
        warn("Client no upgrade connection?\n");
    }
}

static void 
handle_websocket_key(http_t* http, http_header_t* header)
{
    http->websocket_key = server_compute_websocket_key(header->val);
}

static void 
http_handle_content_len(http_t* http, http_header_t* header)
{
    char* endptr;
    http->body_len = strtoull(header->val, &endptr, 10);
    
    if ((errno == ERANGE && (http->body_len == ULONG_MAX)) || (errno != 0 && http->body_len == 0))
    {
        error("HTTP Content-Length: '%s' failed to convert to uint64_t: %s\n",
            header->val, ERRSTR);
    }

    if (endptr == header->val)
    {
        error("HTTP strtoull: No digits were found.\n");
    }
}

static void 
handle_http_header(client_t* client, http_t* http, http_header_t* header)
{
    if (NAME_CMP("Content-Length"))
        http_handle_content_len(http, header);
    else if (NAME_CMP("Connection"))
        set_client_connection(client, header);
    else if (NAME_CMP("Sec-WebSocket-Key"))
        handle_websocket_key(http, header);
}

static void 
parse_url(http_t* http, char* url)
{
    char* path;
    char* params_line;
    char* param;
    char* endptr;

    path = strtok_r(url, "?", &params_line);
    
    if (*params_line)
    {
        param = strtok_r(params_line, "&", &endptr);

        while (param && http->n_params < HTTP_MAX_PARAMS)
        {
            char* key;
            char* val;
            http_header_t* http_param = http->params + http->n_params;

            key = strtok_r(param, "=", &val);

            strncpy(http_param->name, key, HTTP_HEAD_NAME_LEN);
            strncpy(http_param->val, val, HTTP_HEAD_VAL_LEN);

            param = strtok_r(NULL, "&", &endptr);
            http->n_params++;
        }
    }

    strncpy(http->req.url, path, HTTP_URL_LEN);
}

static http_t* 
parse_http(client_t* client, char* buf, size_t buf_len) 
{
    http_t* http;
    char* saveptr;
    char* token;
    char* header;
    char* header_line;
    size_t header_len;
    size_t actual_body_len;

    http = calloc(1, sizeof(http_t));

    header = strsplit(buf, HTTP_END, &saveptr);
    if (!header)
    {
        warn("parse_http first token is NULL!?!\n");
        free(http);
        return NULL;
    }

    http->body = strsplit(NULL, HTTP_END, &saveptr);

    header_len = strlen(header) + strlen(HTTP_END);
    if (http->body)
        actual_body_len = buf_len - header_len;
    else
        actual_body_len = 0;

    header_line = strsplit(header, HTTP_NL, &saveptr);

    token = strtok(header_line, " ");
    if (token)
    {
        if (strstr(token, "HTTP/"))
        {
            http->type = HTTP_RESPOND;
            strncpy(http->resp.version, token, HTTP_VERSION_LEN);
        }
        else
        {
            http->type = HTTP_REQUEST;
            strncpy(http->req.method, token, HTTP_METHOD_LEN);
        }
    }
    else
    {
        free(http);
        return NULL;
    }

    token = strtok(NULL, " ");
    if (token)
    {
        if (http->type == HTTP_REQUEST)
            parse_url(http, token);
        else
            http->resp.code = atoi(token);
    }

    token = strtok(NULL, HTTP_NL);
    if (token)
    {
        if (http->type == HTTP_REQUEST)
            strncpy(http->req.version, token, HTTP_METHOD_LEN);
        else
            strncpy(http->resp.msg, token, HTTP_STATUS_MSG_LEN);
    }

    header_line = strsplit(NULL, HTTP_NL, &saveptr);

    while (header_line)
    {
        http_header_t* http_header = &http->headers[http->n_headers];
        char* name;
        char* val;

        token = strtok(header_line, ": ");
        if (!token)
            break;
        name = token;

        token = strtok(NULL, "");
        if (!token)
            break;
        if (*token == ' ')
            token++;
        val = token;

        strncpy(http_header->name, name, HTTP_HEAD_NAME_LEN);
        strncpy(http_header->val, val, HTTP_HEAD_VAL_LEN);

        header_line = strsplit(NULL, HTTP_NL, &saveptr);
        http->n_headers++;
        if (http->n_headers >= HTTP_MAX_HEADERS)
        {
            http->n_headers = HTTP_MAX_HEADERS - 1;
            break;
        }
    }

    for (size_t i = 0; i < http->n_headers; i++)
        handle_http_header(client, http, &http->headers[i]);

    if (client->state & CLIENT_STATE_UPGRADE_PENDING)
    {
        handle_http_upgrade(client, 
            http_get_header(
                http, 
                HTTP_HEAD_CONN_UPGRADE
            )
        );
    }

    if (http->body || http->body_len)
    {
        if (http->body_len > actual_body_len)
        {
            http->buf.missing = true;
            http->buf.total_recv = actual_body_len;
            char* new_body = calloc(1, http->body_len);
            memcpy(new_body, http->body, actual_body_len);
            http->body = new_body;
            http->body_inheap = true;
            client->recv.http = http;
        }
    }

    return http;
}

void 
print_parsed_http(const http_t* http)
{
    if (server_get_loglevel() != SERVER_VERBOSE)
        return;
    
    verbose("Parsed HTTP: %s\n", (http->type == HTTP_REQUEST) ? "Request" : "Respond");
    if (http->type == HTTP_REQUEST)
    {
        verbose("\tMethod: '%s'\n\t\t\tURL: '%s'\n\t\t\tVersion: '%s'\n", http->req.method, http->req.url, http->req.version);
        for (size_t i = 0; i < http->n_params; i++)
        {
            const http_header_t* param = http->params + i;
            verbose("\t'%s' = '%s'\n", param->name, param->val);
        }
    }
    else
        verbose("\tVersion: '%s'\n\t\t\tCode: %u\n\t\t\tStatus: '%s'\n", http->resp.version, http->resp.code, http->resp.msg);

    for (size_t i = 0; i < http->n_headers; i++)
    {
        const http_header_t* header = &http->headers[i];
        verbose("H:\t'%s' = '%s'\n", header->name, header->val);
    }

    if (http->body)
        verbose("BODY (strlen: %zu, http: %zu):\t'%s'\n", strlen(http->body), http->body_len, (http->body) ? http->body : "NULL");
}

void 
http_free(http_t* http)
{
    if (!http)
        return;

    if (http->body && http->body_inheap)
        free(http->body);

    free(http);
}

http_header_t* 
http_get_header(const http_t* http, const char* name)
{
    for (size_t i = 0; i < http->n_headers; i++)
    {
        http_header_t* header = (http_header_t*)http->headers + i;

        if (NAME_CMP(name))
            return header;
    }

    return NULL;
}

static void 
http_add_header(http_t* http, const char* name, const char* val)
{
    if (!http || !name || !val)
        return;

    http_header_t* to_header = NULL;

    for (size_t i = 0; i < http->n_headers; i++)
    {
        http_header_t* header = http->headers + i;
        if (NAME_CMP(name) || header->name[0] == 0x00 || header->val[0] == 0x00)
        {
            to_header = header;
            break;
        }
    }

    if (!to_header)
    {
        if (http->n_headers >= HTTP_MAX_HEADERS)
        {
            warn("http_add_header(name: %s, val: %s): n headers is FULL!\n", name, val);
            return;
        }

        to_header = http->headers + http->n_headers;
        http->n_headers++;
    }

    strncpy(to_header->name, name, HTTP_HEAD_NAME_LEN);
    strncpy(to_header->val, val, HTTP_HEAD_VAL_LEN);
}

static void 
server_upgrade_client_to_websocket(client_t* client, http_t* req_http)
{
    http_t* http = http_new_resp(HTTP_CODE_SW_PROTO, "Switching Protocols", NULL, 0);
    http_add_header(http, "Connection", HTTP_HEAD_CONN_UPGRADE);
    http_add_header(http, "Upgrade", "websocket");
    http_add_header(http, HTTP_HEAD_WS_ACCEPT, req_http->websocket_key);

    for (size_t i = 0; i < http->n_params; i++)
    {
        http_header_t* param = http->params + i;
        debug("ws: '%s' = '%s'\n", param->name, param->val);
    }

    if (http_send(client, http) != -1)
        client->state |= CLIENT_STATE_WEBSOCKET;
    http_free(http);
    free(req_http->websocket_key);
}

static void 
server_handle_client_upgrade(client_t* client, http_t* http)
{
    const http_header_t* upgrade = http_get_header(http, HTTP_HEAD_CONN_UPGRADE);
    if (upgrade == NULL)
    {
        warn("Client fd:%d (Upgrade pending): No upgrade header in HTTP.\n", client->addr.sock);
        client->state ^= CLIENT_STATE_UPGRADE_PENDING;
        return;
    }

    if (!strncmp(upgrade->val, "websocket", HTTP_HEAD_VAL_LEN))
        server_upgrade_client_to_websocket(client, http);
    else
        warn("Connection upgrade '%s' not implemented.\n", upgrade);

    client->state ^= CLIENT_STATE_UPGRADE_PENDING;
}

static void 
http_add_body(http_t* restrict http, const char* restrict body, size_t body_len)
{
    http->body = calloc(1, body_len);
    memcpy(http->body, body, body_len);
    http->body_inheap = true;
    http->body_len = body_len;

    char val[HTTP_HEAD_VAL_LEN];
    snprintf(val, HTTP_HEAD_VAL_LEN, "%zu", body_len);

    http_add_header(http, HTTP_HEAD_CONTENT_LEN, val);
}

http_t* 
http_new_resp(u16 code, const char* status_msg, const char* body, size_t body_len)
{
    http_t* http = calloc(1, sizeof(http_t));

    http->type = HTTP_RESPOND;
    http->resp.code = code;
    strncpy(http->resp.msg, status_msg, HTTP_STATUS_MSG_LEN);
    strncpy(http->resp.version, HTTP_VERSION, HTTP_VERSION_LEN);

    http_add_header(http, "Server", SERVER_NAME);

    if (body)
        http_add_body(http, body, body_len);

    return http;
}

void 
server_http_resp_ok(client_t* client, char* content, size_t content_len, const char* content_type)
{
    http_t* http = http_new_resp(HTTP_CODE_OK, "OK", content, content_len);
    http_add_header(http, HTTP_HEAD_CONTENT_TYPE, content_type);

    http_send(client, http);

    http_free(http);
}

void 
server_http_resp_error(client_t* client, u16 error_code, const char* status_msg)
{
    http_t* http = http_new_resp(error_code, status_msg, NULL, 0);

    http_send(client, http);

    http_free(http);
}

void 
server_http_resp_404_not_found(client_t* client)
{
    const char* not_found_html = "<h1>Not Found</h1>";
    size_t len = strlen(not_found_html);

    http_t* http = http_new_resp(HTTP_CODE_NOT_FOUND, "Not Found", not_found_html, len);

    http_send(client, http);

    http_free(http);
}

int 
server_http_url_checks(http_t* http)
{
    const char* url = http->req.url;

    if (!http || !url)
        return -1;

    if (strstr(url, "../"))
    {
        warn("GET URL '%s' very sus.\n", url);
        return -1;
    }

    return 0;
}

static enum client_recv_status 
server_handle_http_req(eworker_t* th, client_t* client, http_t* http)
{
#define HTTP_CMP_METHOD(x) strncmp(http->req.method, x, HTTP_METHOD_LEN)
    enum client_recv_status ret = RECV_OK;

    if (!HTTP_CMP_METHOD("GET"))
        server_handle_http_get(th->server, client, http);
    else if (!HTTP_CMP_METHOD("POST"))
        server_handle_http_post(th, client, http);
    else
    {
        warn("Need to implement '%s' HTTP request.\n", http->req.method);
        ret = RECV_DISCONNECT;
    }

    return ret;
}

static enum client_recv_status
server_handle_http_resp(UNUSED server_t* server, UNUSED client_t* client, UNUSED http_t* http)
{
    debug("Implement handling response http\n");
    return RECV_ERROR;
}

enum client_recv_status
server_handle_http(eworker_t* th, client_t* client, http_t* http)
{
    enum client_recv_status ret = RECV_OK;

    if (client->state & CLIENT_STATE_UPGRADE_PENDING)
        server_handle_client_upgrade(client, http);
    else
    {
        if (http->type == HTTP_REQUEST)
            ret = server_handle_http_req(th, client, http);
        else if (http->type == HTTP_RESPOND)
            ret = server_handle_http_resp(th->server, client, http);
        else
        {
            warn("Unknown http type: %d. Request or Respond? Ignored.\n", http->type);
            ret = RECV_DISCONNECT;
        }
    }

    http_free(http);

    return ret;
}

enum client_recv_status 
server_http_parse(eworker_t* th, client_t* client, u8* buf, size_t buf_len)
{
    http_t* http;
    enum client_recv_status ret = RECV_OK;

    http = parse_http(client, (char*)buf, buf_len);
    if (!http)
    {
        error("parse_http() returned NULL, deleting client.\n");
        return RECV_ERROR;
    }

    print_parsed_http(http);

    if (!http->buf.missing)
        ret = server_handle_http(th, client, http);

    return ret;
}

ssize_t 
http_send(client_t* client, http_t* http)
{
    if (!client || !http)
    {
        warn("http_send(%p, %p) something is NULL\n", client, http);
        return -1;
    }

    ssize_t bytes_sent = 0;
    http_to_str_t to_str = http_to_str(http);

    verbose("HTTP send to fd:%d (%s:%s), len: %zu\n%s\n", client->addr.sock, client->addr.ip_str, client->addr.serv, to_str.len, to_str.str);

    if ((bytes_sent = server_send(client, to_str.str, to_str.len)) == -1)
    {
        error("HTTP send to (fd: %d, IP: %s:%s): %s\n",
            client->addr.sock, client->addr.ip_str, client->addr.serv, ERRSTR
        );
    }

    free(to_str.str);

    return bytes_sent;
}
