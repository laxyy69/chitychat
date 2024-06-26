#include "chat/user_file.h"
#include "chat/db.h"
#include "chat/db_def.h"
#include "chat/db_userfile.h"
#include "server.h"

static const char*
server_mime_type_dir(server_t* server, const char* mime_type)
{
    const char* ret;

    if (strstr(mime_type, "image/"))
        ret = server->conf.img_dir;
    else if (strstr(mime_type, "video/"))
        ret = server->conf.vid_dir;
    else
        ret = server->conf.file_dir;

    return ret;
}

bool 
server_init_magic(server_t* server)
{
    server->magic_cookie = magic_open(MAGIC_MIME_TYPE);
    if (server->magic_cookie == NULL)
    {
        error("Unable to initialize magic library\n");
        return false;
    }
 
    if (magic_load(server->magic_cookie, NULL) != 0)
    {
        error("Unable to load magic database\n");
        return false;
    }

    return true;
}

void 
server_close_magic(server_t* server)
{
    magic_close(server->magic_cookie);
}

const char* 
server_mime_type(server_t* server, const void* data, size_t size)
{
    const char* mime_type = magic_buffer(server->magic_cookie, data, size);
    return mime_type;
}

static ssize_t
server_read_file(void* data, size_t size, 
        const char* dir, const char* name)
{
    ssize_t bytes_read = -1;
    char* paew = calloc(1, PATH_MAX);
    i32 fd = -1;

    snprintf(paew, PATH_MAX, "%s/%s", dir, name);

    fd = open(paew, O_RDONLY);
    if (fd == -1) 
    {
        error("open file %s failed: %s\n",
            paew, ERRSTR);
        goto cleanup;
    }

    if ((bytes_read = read(fd, data, size)) == -1)
    {
        error("Failed to read file %s: %s\n",
            paew, ERRSTR);
    }

cleanup:
    free(paew);
    if (fd != -1)
        close(fd);
    return bytes_read;
}

static bool
server_write_file(const void* data, 
                  size_t size, 
                  const char* dir, 
                  const char* name)
{
    i32 fd = -1;
    char* path = calloc(1, PATH_MAX);
    bool ret = false;

    snprintf(path, PATH_MAX, "%s/%s", dir, name);

    debug("Writing file to: %s\n", path);

    fd = open(path, O_WRONLY | O_CREAT, 
                    S_IRUSR | S_IWUSR);
    if (fd == -1)
    {
        error("Failed to open/create file at %s: %s\n",
              path, ERRSTR);
        goto cleanup;
    }

    if (write(fd, data, size) == -1)
    {
        error("Failed to write to %s: %s\n", 
              path, ERRSTR);
        goto cleanup;
    }

    ret = true;
cleanup:
    free(path);
    if (fd != -1)
        close(fd);
    return ret;
}

bool 
server_save_file(UNUSED eworker_t* ew, UNUSED const void* data, 
                 UNUSED size_t size, UNUSED const char* name)
{
    // dbuser_file_t file;
    debug("Implement server_save_file()\n");

    return false;
}

static const char* 
do_save_file_img(eworker_t* ew, dbcmd_ctx_t* ctx)
{
    const void* data = ctx->param.str;
    dbuser_file_t* file;
    dbcmd_ctx_t* refcount_ctx = ctx->next;
    i32 ref_count;

    if (refcount_ctx == NULL)
    {
        warn("do_save_file_img() ctx->next is NULL!\n");
        return "Internal error";
    }
    if (ctx->ret == DB_ASYNC_ERROR)
        return "Failed to save image";

    file = ctx->data;
    ref_count = refcount_ctx->data_size;

    if (ref_count == 1)
    {
        server_write_file(data, file->size,
                          ew->server->conf.img_dir, 
                          file->hash);
    }

    free((void*)data);

    return NULL;
}

bool 
server_save_file_img(eworker_t* ew, const void* data, size_t size, 
                     dbuser_file_t** file_output, bool free_file)
{
    dbuser_file_t* file;
    const char* mime_type;
    bool ret = true;

    mime_type = server_mime_type(ew->server, data, size);
    if (mime_type == NULL || strstr(mime_type, "image/") == NULL)
    {
        warn("save img mime_type failed: %s\n", mime_type);
        info("data: %s\n", (const char*)data);
        print_hex(data, 20);
        return false;
    }
    file = calloc(1, sizeof(dbuser_file_t));
    strncpy(file->mime_type, mime_type, DB_MIME_TYPE_LEN);

    server_sha256_str(data, size, file->hash);
    file->size = size;

    dbcmd_ctx_t ctx = {
        .exec = do_save_file_img,
        .param.str = data,
        .data = file,
        .flags = (free_file) ? 0 : DB_CTX_DONT_FREE 
    };

    if ((ret = db_async_insert_userfile(&ew->db, file, &ctx)))
    {
        ctx.exec = NULL;
        ctx.data = NULL;
        ret = db_async_userfile_refcount(&ew->db, file->hash, &ctx);
    }

    if (ret && file_output)
        *file_output = file;

    return ret;
}

void* 
server_get_file(eworker_t* ew, dbuser_file_t* file)
{
    void* data;
    const char* dir;
    ssize_t actual_size;

    dir = server_mime_type_dir(ew->server, file->mime_type);

    data = calloc(1, file->size);

    actual_size = server_read_file(data, file->size, dir, file->hash);
    if (actual_size == -1)
        goto error;
    else if ((size_t)actual_size != file->size)
    {
        warn("acti_size != file->size: %zu != %zu\n",
                actual_size, file->size);
        file->size = actual_size;
    }
    
    return data;
error:
    free(data);
    return NULL;
}

static const char*
do_delete_file(eworker_t* ew, dbcmd_ctx_t* ctx)
{
    dbcmd_ctx_t* ctx_next = ctx->next;
    dbuser_file_t* file;
    const char* dir;
    char* path;
    i32 ref_count;

    if (ctx_next == NULL)
    {
        warn("do_delete_file: ctx->next is NULL!\n");
        return NULL;
    }
    if (ctx->ret == DB_ASYNC_ERROR)
        return NULL;

    file = ctx->data;
    ref_count = ctx_next->data_size;

    if (ref_count <= 0)
    {
        dir = server_mime_type_dir(ew->server, file->mime_type);
        path = calloc(1, PATH_MAX);
        
        snprintf(path, PATH_MAX, "%s/%s", dir, file->hash);

        debug("Unlinking file: %s (%s)\n", path, file->name);

        if (unlink(path) == -1)
            error("unlink %s failed: %s\n", path, ERRSTR);

        free(path);
    }

    return NULL;
}

bool 
server_delete_file(eworker_t* ew, dbuser_file_t* file)
{
    bool ret;
    dbcmd_ctx_t ctx = {
        .exec = do_delete_file,
        .data = file
    };

    if ((ret = db_async_delete_userfile(&ew->db, file->hash, &ctx)))
    {
        ctx.exec = NULL;
        ctx.data = NULL;
        ret = db_async_userfile_refcount(&ew->db, file->hash, &ctx);
    }
    return ret;
}
