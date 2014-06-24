/*  Copyright 2013 Eluvatar
 *  
 *  This file is part of Trawler
 *
 *  Trawler is free software: you can redistribute it and/or modify
 *  it under the terms of hte GNU General Public Licence as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Trawler is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Trawler.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <curl/curl.h>
#include <czmq.h>
#include <stdint.h>
#include <time.h>

#include "trawler.pb-c.h" 

#define TRAWLER_ACK_RESULT (200)
#define TRAWLER_LOGOUT_TIMEOUT (408)
#define TRAWLER_NACK_GENERIC (500)
#define TRAWLER_NACK_UNSUPPORTED_METHOD (501)

#ifndef DEBUG
#define TRAWLER_BASE_URL ("http://www.nationstates.net/")
#else
#define TRAWLER_BASE_URL ("http://localhost:6260/")
#endif

#define TRAWLER_PORT 5557
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#ifndef TRAWLER_MAX_REQUESTS
#define TRAWLER_MAX_REQUESTS (1000)
#endif
#ifndef TRAWLER_SESSION_TIMEOUT
#define TRAWLER_SESSION_TIMEOUT (30)
#endif
#define TRAWLER_DELAY_MSEC (660)
#define TRAWLER_VERSION ("0.1.0")

int trawlerd_loop(long verbose);

typedef void * zmq_socket_t;
typedef enum {
    /* RFC 2616 */
    OPTIONS,
    GET,
    HEAD,
    POST,
    PUT,
    DELETE,
    TRACE,
    CONNECT
} method_t;
typedef struct trequest {
    /* identifier of client */
    zframe_t *client ;
    /* reply being built for the response */
    Trawler__Reply reply;
    /* arguments from client */
    int32_t id;
    method_t method;
    char *path;
    char *query;
    char *session;
    bool headers;
} trequest_t;

typedef struct trequest_node {
    trequest_t req;
    struct trequest_node *next;
} trequest_node_t;
typedef struct trequest_list {
    trequest_node_t *first;
    trequest_node_t *last;
} trequest_list_t;

typedef struct tsession {
    zframe_t *client;
    time_t last_activity;
    unsigned int req_count;
    char user_agent[];
} tsession_t;

typedef struct trawler {
    zmq_socket_t src;
    trequest_list_t *req_list;
	zhash_t *sessions;
} trawler_t;

/* This function is called at the beginning of trawlerd_loop to set things up. */
int trawlerd_init(CURL **ch, zmq_socket_t *server, long verbose);

/* This function is called when a socket has something. It determines which
 * _receive_ function to call and calls it. */
int trawlerd_receive(trawler_t *trawler);
int trawlerd_login(trawler_t *trawler, zframe_t *client, const char *client_hex, 
                   Trawler__Login *login);
/* This function looks through the list of sessions for sessions to logout. */
int trawlerd_reap(trawler_t  *trawler);
int trawlerd_update_last_activity(tsession_t *session);
int trawlerd_receive_request(zframe_t *client, Trawler__Request *preq, 
                             trequest_t *treq);
int trawlerd_ack(zmq_socket_t src, zframe_t *client, int32_t req_id);
int trawlerd_nack(zmq_socket_t src, zframe_t *client, int32_t req_id,
                  int32_t result);
int trawlerd_logout(trawler_t *trawler, const char *client_hex, tsession_t *session);
int trawlerd_fulfill_request(zmq_socket_t src, zhash_t *sessions,
                             trequest_t *treq, CURL *ch);
int trawlerd_headers_append(void *stream, size_t size, size_t nmemb, 
                            trequest_t *treq);
int trawlerd_response_append(void *stream, size_t size, size_t nmemb, 
                             trequest_t *treq);

int trequest_list_new( trequest_list_t **list );
int trequest_list_append( trequest_list_t *list, trequest_node_t *trnode );
int trequest_list_peek( trequest_list_t *list, trequest_t **treq );
int trequest_list_shift( trequest_list_t *list );
