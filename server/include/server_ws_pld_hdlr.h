#ifndef _SERVER_WS_PLD_HDLR_H_
#define _SERVER_WS_PLD_HDLR_H_

#include "server.h"
#include "json-c/json.h"

void server_ws_handle_text_frame(server_t* server, client_t* client, char* buf, size_t buf_len);

#endif // _SERVER_WS_PLD_HDLR_H_