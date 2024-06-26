#include "server_log.h"

#include "common.h"
#include <stdarg.h>
#include <string.h>

#define SERVER_LOG_MAX 4096


/* Terminal colors */
#define FG_RED "\033[91m"
#define BG_RED "\033[101m"
#define FG_GREEN "\033[92m"
#define FG_ORANGE "\033[93m"
#define FG_DEFAULT "\033[39m"
#define BG_DEFAULT "\033[49m"
#define FG_BLACK "\033[30m"

#define C_DEFAULT FG_DEFAULT BG_DEFAULT
#define C_FATAL (BG_RED FG_BLACK)

static const char* wa_log_str[SERVER_LOG_LEVEL_LEN] = {
    "[" FG_RED "FF" FG_DEFAULT "]",
    "[" FG_RED "EE" FG_DEFAULT "]",
    "[" FG_ORANGE "WW" FG_DEFAULT"]",
    "[" FG_GREEN "II" FG_DEFAULT "]",
    "[DD]",
    "[VV]",
};

static const char* wa_log_color[SERVER_LOG_LEVEL_LEN] = {
    C_FATAL,            /* WA_FATAL */
    FG_RED,             /* WA_ERROR */
    FG_DEFAULT,         /* WA_WARN */
    FG_DEFAULT,         /* WA_INFO */
    FG_DEFAULT,         /* WA_DEBUG */
    FG_DEFAULT          /* WA_VERBOSE */
};

static enum server_log_level log_level = SERVER_DEBUG;

void server_set_loglevel(enum server_log_level level)
{
    log_level = level;
}

enum server_log_level server_get_loglevel(void)
{
    return log_level;
}

void server_log(enum server_log_level level, const char* filename, int line, const char* format, ...)
{
    if (log_level < level)
        return;

    char output[SERVER_LOG_MAX];
    memset(output, 0, SERVER_LOG_MAX);
    //char* output = calloc(1, SERVELOG_MAX);
    FILE* file = (level <= SERVER_WARN) ? stderr : stdout;

    va_list args;
    va_start(args, format);

    vsnprintf(output, SERVER_LOG_MAX, format, args);

    va_end(args);

    if (filename)
        fprintf(file, "%s [%s:%d]: %s", wa_log_str[level], filename, line, wa_log_color[level]);
    else
        fprintf(file, "%s: %s", wa_log_str[level], wa_log_color[level]);

    fprintf(file, "%s%s%s", wa_log_color[level], output, C_DEFAULT);

    fflush(file);

    // free(output);
}
