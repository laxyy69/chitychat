#include "chat/user_upload.h"
#include "chat/db.h"
#include "chat/db_def.h"
#include "chat/db_userfile.h"
#include "chat/group.h"
#include "chat/db_group.h"
#include "chat/db_user.h"
#include "chat/user_file.h"
#include "server.h"
#include "server_client.h"

static upload_token_t*
server_check_upload_token(server_t* server, const http_t* http, u32* user_id)
{
    char* endptr;
    const http_header_t* upload_token_header = http_get_header(http, "Upload-Token");
    const char* token_str = upload_token_header->val;
    if (!token_str)
    {
        error("No Upload-Token in POST request!\n");
        return NULL;
    }

    const u64 token = strtoull(token_str, &endptr, 10);
    if ((errno == ERANGE && (token == ULONG_MAX)) || (errno != 0 && token == 0))
    {
        error("HTTP POST Upload-Token: '%s' failed to convert to uint64_t: %s\n",
            token_str, ERRSTR);
        return NULL;
    }

    if (endptr == token_str)
    {
        error("HTTP POST strtoull: No digits were found.\n");
        return NULL;
    }

    upload_token_t* real_token = server_get_upload_token(server, token);
    if (real_token == NULL)
    {
        error("No upload token found: %zu\n", token);
        return NULL;
    }

    if (real_token->token != token)
    {
        error("*real_token != token?? %zu != %zu\n", *real_token, token);
        return NULL;
    }

    if (real_token->type == UT_USER_PFP && user_id)
        *user_id = real_token->user_id;

    return real_token;
}

static const char*
update_pfp_result(eworker_t* ew, dbcmd_ctx_t* ctx)
{
    dbuser_t* user;
    dbuser_file_t* file;
    dbuser_file_t* old_file;

    if (ctx->ret == DB_ASYNC_ERROR)
        return NULL;

    user = ctx->param.ptr;
    file = ctx->data;
    old_file = calloc(1, sizeof(dbuser_file_t));

    strncpy(old_file->hash, user->pfp_hash, DB_PFP_HASH_MAX);
    strcpy(old_file->mime_type, "image/");
    server_delete_file(ew, old_file);

    strncpy(user->pfp_hash, file->hash, DB_PFP_HASH_MAX);
    server_rtusm_user_pfp_change(ew, user);

    return NULL;
}

static bool 
update_user_pfp(eworker_t* ew, dbuser_t* user, dbuser_file_t* file)
{
    bool ret;
    dbcmd_ctx_t ctx = {
        .exec = update_pfp_result,
        .param.ptr = user,
        .data = file
    };
    ret = db_async_update_user(&ew->db, NULL, NULL, file->hash, user->user_id, &ctx);
    return !ret;
    // bool failed = false;
    // if (!server_db_update_user(&ew->db, NULL, NULL, hash, user->user_id))
    //     failed = true;
    // else
    // {
    //     dbuser_file_t* dbfile = server_db_select_userfile(&ew->db, user->pfp_hash);
    //     if (dbfile)
    //     {
    //         server_delete_file(ew, dbfile);
    //         free(dbfile);
    //     }
    //     else
    //         warn("User:%d %s (%s) no pfp?\n", user->user_id, user->username, user->displayname);
    // }
    // return failed;
}

static void 
server_handle_user_pfp_update(eworker_t* ew, client_t* client, const http_t* http, u32 user_id)
{
    http_t* resp = NULL;
    dbuser_t* user = NULL;
    client_t* user_client;
    bool failed = false;
    const char* post_img_cmd = "/img/";
    size_t post_img_cmd_len = strlen(post_img_cmd);

    if (strncmp(http->req.url, post_img_cmd, post_img_cmd_len) != 0)
    {
        resp = http_new_resp(HTTP_CODE_BAD_REQ, "Not image", NULL, 0);
        goto respond;
    }

    user_client = server_get_client_user_id(ew->server, user_id);
    if (!user_client)
    {
        warn("User %u not found.\n", user_id);
        failed = true;
        goto respond;
    }
    user = user_client->dbuser;

    dbuser_file_t* file;

    if (server_save_file_img(ew, http->body, http->body_len, &file, false))
        failed = update_user_pfp(ew, user, file);
    else
    {
        failed = true;
    }
respond:
    if (!resp)
    {
        if (failed)
            resp = http_new_resp(HTTP_CODE_INTERAL_ERROR, "Interal server error", NULL, 0);
        else
        {
            resp = http_new_resp(HTTP_CODE_OK, "OK", NULL, 0);
            ((http_t*)http)->body_inheap = false;
        }
    }

    http_send(client, resp);
    http_free(resp);
}

static const char*
do_insert_msg_after(eworker_t* ew, dbcmd_ctx_t* ctx)
{
    server_event_t* se;
    dbmsg_t* msg;
    upload_token_t* ut = ctx->param.ptr;
    if (ctx->ret == DB_ASYNC_ERROR)
        goto clear;

    msg = ctx->data;
    server_get_send_group_msg(ew, msg);
clear:
    if ((se = server_get_event(ew->server, ut->timerfd)))
        server_del_event(ew, se);
    else
        server_del_upload_token(ew, ut);
    ctx->data = NULL;
    return NULL; 
}

static void 
server_handle_msg_attach(eworker_t* ew, client_t* client, 
                         const http_t* http, upload_token_t* ut)
{
    http_t* resp = NULL;
    dbmsg_t* msg = &ut->msg_state.msg;
    size_t attach_index = 0;
    char* endptr;
    http_header_t* attach_index_header = http_get_header(http, "Attach-Index");
    json_object* attach_json;

    if (attach_index_header == NULL)
    {
        resp = http_new_resp(HTTP_CODE_BAD_REQ, "No Attach-Index header", NULL, 0);
        goto respond;
    }

    attach_index = strtoul(attach_index_header->val, &endptr, 10);
    if (attach_index == ULONG_MAX)
    {
        warn("Invalid Attach-Index: %s = %s\n", 
                attach_index_header->name, attach_index_header->val);
        resp = http_new_resp(HTTP_CODE_BAD_REQ, "Invalid Attach-Index", NULL, 0);
        goto respond;
    }

    if (attach_index > ut->msg_state.total)
    {
        warn("Attach-Index > msg_state.total. %zu/%zu\n", 
                attach_index, ut->msg_state.total);
    }

    attach_json = json_object_array_get_idx(msg->attachments_json, attach_index);
    if (attach_json)
    {
        dbuser_file_t* file = NULL;
        if (server_save_file_img(ew, http->body, http->body_len, &file, true))
        {
            json_object_object_add(attach_json, "hash",
                                   json_object_new_string(file->hash));

            ut->msg_state.current++;
            if (ut->msg_state.current >= ut->msg_state.total)
            {
                msg->attachments = (char*)json_object_to_json_string(msg->attachments_json);
                dbcmd_ctx_t ctx = {
                    .exec = do_insert_msg_after,
                    .param.ptr = ut,
                    .client = NULL
                };
                db_async_insert_group_msg(&ew->db, msg, &ctx);
            }

            /*
             * Hacky set http->body_inheap to false so the body wont be freed.
             * Why? server_save_file_img() will free it in do_save_file_img()
             */
            ((http_t*)http)->body_inheap = false;
        }
    }
    else
        warn("Failed to get json array index: %zu\n", attach_index);

respond:
    if (resp)
    {
        http_send(client, resp);
        http_free(resp);
    }
}

void 
server_handle_user_upload(eworker_t* ew, client_t* client, const http_t* http)
{
    server_t* server = ew->server;
    http_t* resp = NULL;
    u32 user_id;
    upload_token_t* ut = NULL;
    server_event_t* se;

    ut = server_check_upload_token(server, http, &user_id);

    if (ut == NULL)
    {
        resp = http_new_resp(HTTP_CODE_BAD_REQ, "Upload-Token failed", NULL, 0);
        goto respond;
    }

    if (ut->type == UT_USER_PFP)
    {
        server_handle_user_pfp_update(ew, client, http, user_id);
        se = server_get_event(ew->server, ut->timerfd);
        if (se)
            server_del_event(ew, se);
        else
            server_del_upload_token(ew, ut);
    }
    else if (ut->type == UT_MSG_ATTACHMENT)
    {
        server_handle_msg_attach(ew, client, http, ut);
    }
    else
    {
        warn("Upload Token unknown type: %d\n", ut->type);
        free(ut);
    }

respond:
    if (resp)
    {
        http_send(client, resp);
        http_free(resp);
    }
}
