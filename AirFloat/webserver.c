//
//  webserver.c
//  AirFloat
//
//  Copyright (c) 2013, Kristian Trenskow All rights reserved.
//
//  Redistribution and use in source and binary forms, with or
//  without modification, are permitted provided that the following
//  conditions are met:
//
//  Redistributions of source code must retain the above copyright
//  notice, this list of conditions and the following disclaimer.
//  Redistributions in binary form must reproduce the above
//  copyright notice, this list of conditions and the following
//  disclaimer in the documentation and/or other materials provided
//  with the distribution. THIS SOFTWARE IS PROVIDED BY THE
//  COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
//  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
//  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
//  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "log.h"
#include "mutex.h"
#include "sockaddr.h"
#include "socket.h"
#include "webserverconnection.h"

#include "obj.h"

#include "webserver.h"

struct web_server_connection_t {
    web_server_connection_p web_connection;
    socket_p socket;
};

struct web_server_t {
    socket_p socket_ipv4;
    socket_p socket_ipv6;
    sockaddr_type socket_types;
    bool is_running;
    struct web_server_connection_t* connections;
    uint32_t connection_count;
    mutex_p mutex;
    struct {
        web_server_accept_callback accept;
        struct {
            void* accept;
        } ctx;
    } callbacks;
};

void _web_server_socket_closed(socket_p socket, void* ctx) {
    
    struct web_server_t* ws = (struct web_server_t*)ctx;
    
    mutex_lock(ws->mutex);
    
    for (uint32_t i = 0 ; i < ws->connection_count ; i++)
        if (ws->connections[i].socket == socket) {
            
            web_server_connection_release(ws->connections[i].web_connection);
            socket_release(ws->connections[i].socket);
            
            for (uint32_t x = i ; x < ws->connection_count - 1 ; x++)
                ws->connections[x] = ws->connections[x + 1];
            
            ws->connection_count--;
            
            break;
            
        }
    
    mutex_unlock(ws->mutex);
    
}

struct web_server_t* web_server_create(sockaddr_type socket_types) {
    
    struct web_server_t* ws = (struct web_server_t*)obj_create(sizeof(struct web_server_t));
    
    ws->socket_types = socket_types;
    ws->mutex = mutex_create_recursive();
    
    return ws;
    
}

void _web_server_destroy(void* obj) {
    
    struct web_server_t* ws = (struct web_server_t*)obj;
    
    web_server_stop(ws);
    
    mutex_release(ws->mutex);
    
}

struct web_server_t* web_server_retain(struct web_server_t* ws) {
    
    return obj_retain(ws);
    
}

struct web_server_t* web_server_release(struct web_server_t* ws) {
    
    return obj_release(ws, _web_server_destroy);
    
}

socket_p _web_server_bind(struct web_server_t* ws, uint16_t port, sockaddr_type socket_type) {
    
    socket_p socket = NULL;
    
    if (ws->socket_types & socket_type) {
        
        socket = socket_create("Web server", false);
        struct sockaddr* end_point = sockaddr_create(NULL, port, socket_type, 0);
        
        bool ret = socket_bind(socket, end_point);
        
        sockaddr_release(end_point);
        
        if (!ret)
            socket = socket_release(socket);
        
    }
    
    return socket;
    
}

bool _web_server_socket_accept_callback(socket_p socket, socket_p new_socket, void* ctx) {
    
    struct web_server_t* ws = (struct web_server_t*)ctx;
    
    bool ret = false;
    
    if (new_socket != NULL) {
        
        mutex_lock(ws->mutex);
        
        web_server_connection_p new_web_connection = web_server_connection_create(new_socket, ws);
        
        bool should_live = false;
        if (ws->callbacks.accept)
            should_live = ws->callbacks.accept(ws, new_web_connection, ws->callbacks.ctx.accept);
        
        if (!should_live)
            web_server_connection_release(new_web_connection);
        else {
            
            socket_set_closed_callback(new_socket, _web_server_socket_closed, ws);
            
            struct web_server_connection_t new_connection = { new_web_connection, new_socket };
            
            ws->connections = (struct web_server_connection_t*)realloc(ws->connections, sizeof(struct web_server_connection_t) * (ws->connection_count + 1));
            ws->connections[ws->connection_count] = new_connection;
            ws->connection_count++;
            
            web_server_connection_take_off(new_web_connection);
            
            ret = true;
            
        }
        
        mutex_unlock(ws->mutex);
        
    }
    
    return ret;
    
}

bool web_server_start(struct web_server_t* ws, uint16_t port) {
    
    mutex_lock(ws->mutex);
    
    if (!ws->is_running) {
        
        log_message(LOG_INFO, "Trying port %d", port);
        
        ws->socket_ipv4 = _web_server_bind(ws, port, sockaddr_type_inet_4);
        ws->socket_ipv6 = _web_server_bind(ws, port, sockaddr_type_inet_6);
        
        if (((ws->socket_types & sockaddr_type_inet_4) == 0 || ws->socket_ipv4 != NULL) && ((ws->socket_types & sockaddr_type_inet_6) == 0 || ws->socket_ipv6 != NULL)) {
            
            if ((ws->socket_types & sockaddr_type_inet_4) != 0)
                socket_set_accept_callback(ws->socket_ipv4, _web_server_socket_accept_callback, ws);
            if ((ws->socket_types & sockaddr_type_inet_6) != 0)
                socket_set_accept_callback(ws->socket_ipv6, _web_server_socket_accept_callback, ws);
            
            ws->is_running = true;
            
            log_message(LOG_INFO, "Server started.");
            
        } else {
            
            ws->socket_ipv4 = socket_release(ws->socket_ipv4);
            ws->socket_ipv6 = socket_release(ws->socket_ipv6);
            
            log_message(LOG_ERROR, "Cannot start: Cannot bind to port %d", port);
            
        }
        
    } else
        log_message(LOG_ERROR, "Cannot start: Server is already running");
    
    bool ret = ws->is_running;
    
    mutex_unlock(ws->mutex);
    
    return ret;
    
}

bool web_server_is_running(struct web_server_t* ws) {
    
    mutex_lock(ws->mutex);
    bool ret = ws->is_running;
    mutex_unlock(ws->mutex);
    
    return ret;
    
}

void web_server_stop(struct web_server_t* ws) {
    
    mutex_lock(ws->mutex);
    
    if (ws->is_running) {
        
        ws->is_running = false;
        
        socket_close(ws->socket_ipv4);
        socket_close(ws->socket_ipv6);
        
        ws->socket_ipv4 = socket_release(ws->socket_ipv4);
        ws->socket_ipv6 = socket_release(ws->socket_ipv6);
        
        for (uint32_t i = 0 ; i < ws->connection_count ; i++) {
            socket_close(ws->connections[i].socket);
            socket_release(ws->connections[i].socket);
            web_server_connection_release(ws->connections[i].web_connection);
        }
        ws->connection_count = 0;
        
        log_message(LOG_INFO, "Server stopped");
        
    } else
        log_message(LOG_ERROR, "Cannot stop: Server is not running");
    
    mutex_unlock(ws->mutex);
    
}

uint32_t web_server_get_connection_count(struct web_server_t* ws) {
    
    mutex_lock(ws->mutex);
    uint32_t ret = ws->connection_count;
    mutex_unlock(ws->mutex);
    
    return ret;
    
}

struct sockaddr* web_server_get_local_end_point(struct web_server_t* ws, sockaddr_type socket_type) {
    
    return socket_get_local_end_point((ws->socket_types & sockaddr_type_inet_4 ? ws->socket_ipv4 : ws->socket_ipv6));
    
    return NULL;

}

void web_server_set_accept_callback(struct web_server_t* ws, web_server_accept_callback accept_callback, void* ctx) {
    
    mutex_lock(ws->mutex);
    ws->callbacks.accept = accept_callback;
    ws->callbacks.ctx.accept = ctx;
    mutex_unlock(ws->mutex);
    
}
