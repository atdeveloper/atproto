/* Copyright (c) 2014 Ivan Grokhotkov. All rights reserved.
 * This file is part of the atproto AT protocol library
 *
 * Redistribution and use is permitted according to the conditions of the
 * 3-clause BSD license to be found in the LICENSE file.
 */

#include "user_interface.h"
#include "ip_commands_common.h"
#include "ip_commands_socket.h"
#include "dce_utils.h"

dce_result_t SECTION_ATTR ip_handle_CIPCREATE(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPCREATE=\"TCP|UDP\"[,port][,buffer_size]", -1, 1);
        return DCE_OK;
    }
    
    if (argc == 0 ||
        argv[0].type != ARG_TYPE_STRING ||
        (argc >= 2 && argv[1].type != ARG_TYPE_NUMBER && argv[1].type != ARG_NOT_SPECIFIED) ||
        (argc == 3 && argv[2].type != ARG_TYPE_NUMBER && argv[2].type != ARG_NOT_SPECIFIED) ||
        argc > 3)
    {
        DCE_DEBUG("invalid args");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    enum espconn_type connection_type = ESPCONN_INVALID;
    if (strcmp("TCP", argv[0].value.string) == 0)
        connection_type = ESPCONN_TCP;
    else if (strcmp("UDP", argv[0].value.string) == 0)
        connection_type = ESPCONN_UDP;
    else
    {
        DCE_DEBUG("invalid protocol");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    int port;
    if (argc == 2 && argv[1].type == ARG_TYPE_NUMBER)
        port = argv[1].value.number;
    else
        port = espconn_port();

    int rx_buffer_size = DEFAULT_RX_BUFFER_SIZE;
    if (argc == 3 && argv[2].type == ARG_TYPE_NUMBER)
        rx_buffer_size = argv[2].value.number;
    
    ip_ctx_t* ip_ctx = (ip_ctx_t*) group_ctx;
    int index = ip_espconn_get(ip_ctx, 0, connection_type, rx_buffer_size);
    if (index < 0) // all connections are in use
    {
        DCE_DEBUG("all connections in use");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    ip_connection_t* connection = ip_ctx->connections + index;
    if (connection_type == ESPCONN_TCP)
        connection->conn->proto.tcp->local_port = port;
    else
        connection->conn->proto.udp->local_port = port;
    
    arg_t args[] = {
        {ARG_TYPE_NUMBER, .value.number=index},
        {ARG_TYPE_NUMBER, .value.number=port},
        {ARG_TYPE_NUMBER, .value.number=rx_buffer_size}
    };
    dce_emit_extended_result_code_with_args(dce, "CIPCREATE", -1, args, 3, 1);
    return DCE_OK;
}

dce_result_t SECTION_ATTR ip_handle_CIPCLOSE(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPCLOSE=<index>", -1, 1);
        return DCE_OK;
    }
    if (argc != 1 || argv[0].type != ARG_TYPE_NUMBER)
    {
        DCE_DEBUG("invalid arguments");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    ip_ctx_t* ip_ctx = (ip_ctx_t*) group_ctx;
    int index = argv[0].value.number;
    ip_connection_t* connection = ip_ctx->connections + index;
    if (!connection->conn)
    {
        DCE_DEBUG("connection not in use");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    ip_espconn_release(ip_ctx, index);
    dce_emit_basic_result_code(dce, DCE_RC_OK);
    return DCE_RC_OK;
}

void ip_recv_callback(struct espconn* connection, char *pdata, unsigned short len)
{
    ip_connection_t* arg = (ip_connection_t*) connection->reverse;
    size_t size_to_copy = len;
    size_t size_available = arg->rx_buffer_size - arg->rx_buffer_pos;
    if (size_to_copy > size_available)
    {
        size_to_copy = size_available;
        DCE_DEBUGV("rx buffer overflow, index=%d, rx_buffer_size=%d",
                   arg->index, (int) arg->rx_buffer_size);
    }
    memcpy(arg->rx_buffer + arg->rx_buffer_pos, pdata, size_to_copy);
    arg->rx_buffer_pos += size_to_copy;
    
    arg_t args[] = {
        {ARG_TYPE_NUMBER, .value.number = arg->index},
        {ARG_TYPE_NUMBER, .value.number = (int) arg->rx_buffer_pos}
    };
    dce_emit_extended_result_code_with_args(arg->ctx->dce, "CIPDR", -1, args, 2, 0);
}

void ip_sent_callback(struct espconn* connection)
{
    ip_connection_t* arg = (ip_connection_t*) connection->reverse;
    arg_t res = {ARG_TYPE_NUMBER, .value.number = arg->index};
    dce_emit_extended_result_code_with_args(arg->ctx->dce, "CIPSENDI", -1, &res, 1, 0);
}

void ip_tcp_connect_callback(struct espconn* connection)
{
    ip_connection_t* arg = (ip_connection_t*) connection->reverse;
    DCE_DEBUG("connect callback");
    arg_t res = {ARG_TYPE_NUMBER, .value.number = arg->index};
    dce_emit_extended_result_code_with_args(arg->ctx->dce, "CIPCONNECT", -1, &res, 1, 0);
}

void ip_tcp_disconnect_callback(struct espconn* connection)
{
    ip_connection_t* arg = (ip_connection_t*) connection->reverse;
    arg_t res = {ARG_TYPE_NUMBER, .value.number = arg->index};
    dce_emit_extended_result_code_with_args(arg->ctx->dce, "CIPDISCONNECT", -1, &res, 1, 0);
}

void ip_tcp_reconnect_callback(struct espconn* connection, sint8 err)
{
    ip_connection_t* arg = (ip_connection_t*) connection->reverse;
    arg_t res[] = {
        {ARG_TYPE_NUMBER, .value.number = arg->index},
        {ARG_TYPE_NUMBER, .value.number = err}
    };
    dce_emit_extended_result_code_with_args(arg->ctx->dce, "CIPRECONNECT", -1, res, 2, 0);
}

static ip_ctx_t* s_tcp_accept_context = 0;

void ip_tcp_accept_callback(struct espconn* connection)
{
    int port = connection->proto.tcp->local_port;
    int rev = (int) connection->reverse;
    DCE_DEBUGV("ip_tcp_accept_callback, local_port=%d, rev=%d", port, rev);
}

dce_result_t SECTION_ATTR ip_handle_CIPCONNECT(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPCONNECT=<index>,\"ip_addr\",<remote_port>", -1, 1);
        return DCE_OK;
    }
    if (argc != 3 ||
        argv[0].type != ARG_TYPE_NUMBER ||
        argv[1].type != ARG_TYPE_STRING ||
        argv[2].type != ARG_TYPE_NUMBER)
    {
        DCE_DEBUG("invalid arguments");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    ip_ctx_t* ctx = (ip_ctx_t*) group_ctx;
    int index = argv[0].value.number;
    if (index >= MAX_ESP_CONNECTIONS ||
        !ctx->connections[index].conn)
    {
        DCE_DEBUG("invalid connection index");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    ip_connection_t* connection = ctx->connections + index;
    struct espconn* conn = connection->conn;
    uint8_t* remote_ip;
    if (conn->type == ESPCONN_TCP)
        remote_ip = conn->proto.tcp->remote_ip;
    else
        remote_ip = conn->proto.udp->remote_ip;
    if (dce_parse_ip(argv[1].value.string, remote_ip) != 0)
    {
        DCE_DEBUG("invalid remote IP address");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    if (conn->type == ESPCONN_TCP)
        conn->proto.tcp->remote_port = argv[2].value.number;
    else
        conn->proto.udp->remote_port = argv[2].value.number;
    
    espconn_regist_recvcb(conn, (espconn_recv_callback) &ip_recv_callback);
    espconn_regist_sentcb(conn, (espconn_sent_callback) &ip_sent_callback);
    if (conn->type == ESPCONN_TCP)
    {
        espconn_regist_connectcb(conn, (espconn_connect_callback) &ip_tcp_connect_callback);
        espconn_regist_reconcb(conn, (espconn_reconnect_callback) &ip_tcp_reconnect_callback);
        espconn_regist_disconcb(conn,  (espconn_connect_callback) &ip_tcp_disconnect_callback);
    }
    espconn_connect(conn);
    dce_emit_basic_result_code(dce, DCE_RC_OK);
    return DCE_OK;
}

dce_result_t SECTION_ATTR ip_handle_CIPDISCONNECT(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPDISCONNECT=<index>", -1, 1);
        return DCE_OK;
    }
    if (argc != 1 ||
        argv[0].type != ARG_TYPE_NUMBER)
    {
        DCE_DEBUG("invalid arguments");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    ip_ctx_t* ctx = (ip_ctx_t*) group_ctx;
    int index = argv[0].value.number;
    if (index >= MAX_ESP_CONNECTIONS ||
        !ctx->connections[index].conn)
    {
        DCE_DEBUG("invalid connection index");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    struct espconn* conn = ctx->connections[index].conn;
    espconn_disconnect(conn);
    dce_emit_basic_result_code(dce, DCE_RC_OK);
    return DCE_OK;
}
#if 0
dce_result_t SECTION_ATTR ip_handle_CIPLISTEN(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPLISTEN=<index>", -1, 1);
        return DCE_OK;
    }
    if (argc != 1 ||
        argv[0].type != ARG_TYPE_NUMBER)
    {
        DCE_DEBUG("invalid arguments");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    ip_ctx_t* ctx = (ip_ctx_t*) group_ctx;
    int index = argv[0].value.number;
    if (index >= MAX_ESP_CONNECTIONS ||
        ctx->connections[index].type == UNUSED)
    {
        DCE_DEBUG("invalid connection index");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    struct espconn* connection = ctx->connections[index].connection;

    int server_timeout = 20000;
    if (connection->type == ESPCONN_TCP)
        espconn_regist_connectcb(connection, (espconn_connect_callback) &ip_tcp_accept_callback);
    else
        espconn_regist_recvcb(connection, (espconn_recv_callback) &ip_recv_callback);
    
    s_tcp_accept_context = ctx;
    
    espconn_accept(connection);
    espconn_regist_time(connection, server_timeout, 0);
    dce_emit_basic_result_code(dce, DCE_RC_OK);
    return DCE_OK;
}
#endif

dce_result_t SECTION_ATTR ip_handle_CIPSENDI(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPSENDI=<index>,\"data_to_send\"", -1, 1);
        return DCE_OK;
    }
    if (argc != 2 ||
        argv[0].type != ARG_TYPE_NUMBER ||
        argv[1].type != ARG_TYPE_STRING)
    {
        DCE_DEBUG("invalid arguments");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    ip_ctx_t* ctx = (ip_ctx_t*) group_ctx;
    int index = argv[0].value.number;
    if (index >= MAX_ESP_CONNECTIONS ||
        !ctx->connections[index].conn)
    {
        DCE_DEBUG("invalid connection index");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    struct espconn* conn = ctx->connections[index].conn;
    espconn_sent(conn, (uint8_t*) argv[1].value.string, strlen(argv[1].value.string));
    dce_emit_basic_result_code(dce, DCE_RC_OK);
    return DCE_OK;
}

dce_result_t SECTION_ATTR ip_handle_CIPRD(dce_t* dce, void* group_ctx, int kind, size_t argc, arg_t* argv)
{
    if (kind == DCE_TEST)
    {
        dce_emit_extended_result_code(dce, "+CIPRD=<index>", -1, 1);
        return DCE_OK;
    }
    if (argc != 1 || argv[0].type != ARG_TYPE_NUMBER)
    {
        DCE_DEBUG("invalid arguments");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }
    
    ip_ctx_t* ctx = (ip_ctx_t*) group_ctx;
    int index = argv[0].value.number;
    if (index >= MAX_ESP_CONNECTIONS ||
        !ctx->connections[index].conn)
    {
        DCE_DEBUG("invalid connection index");
        dce_emit_basic_result_code(dce, DCE_RC_ERROR);
        return DCE_OK;
    }

    ip_connection_t* connection = ctx->connections + index;
    size_t size_to_read = connection->rx_buffer_pos;
    
    arg_t res[] = {
        {ARG_TYPE_NUMBER, .value.number = index},
        {ARG_TYPE_NUMBER, .value.number = (int) size_to_read},
    };
    dce_emit_extended_result_code_with_args(dce, "CIPRD", -1, res, 2, 0);
    dce_emit_information_response(dce, connection->rx_buffer, size_to_read);
    connection->rx_buffer_pos -= size_to_read;
    dce_emit_basic_result_code(dce, DCE_RC_OK);
    return DCE_OK;
}
