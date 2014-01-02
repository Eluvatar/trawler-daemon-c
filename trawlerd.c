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

#include <zmq.h>

#include "trawler.h"

int main(/*const int argc, const char **argv*/) {
    return trawlerd_loop();
}

int trawlerd_init(CURL **ch, zmq_socket_t *server) {
    int err;
    zctx_t *ctx;
    err = curl_global_init(CURL_GLOBAL_NOTHING);
    if( err != CURLE_OK )
        return err;
    *ch = curl_easy_init();
    err = curl_easy_setopt(*ch, CURLOPT_WRITEFUNCTION, trawlerd_response_append);
    if( err != CURLE_OK )
        return err;
    ctx = zctx_new();
    *server = zsocket_new( ctx, ZMQ_ROUTER );
    assert(5557 == zsocket_bind( *server, "tcp://*:5557" ));
    return 0;
}

static inline void set_timeout( int *ret, struct timeval *then, struct timeval *now ) {
    *ret = TRAWLER_DELAY_MSEC + 1000*(then->tv_sec - now->tv_sec)
           + (then->tv_usec - now->tv_usec)/1000;
}

int trawlerd_loop() {
    trequest_list_t *list;
    int timeout, err;
    struct timeval then, now;
    CURL *ch;
    zmq_socket_t server;

    trequest_list_new( &list );
    trawlerd_init( &ch, &server );

    err=gettimeofday( &now, NULL);
    if( err ) exit( err );
    then = now;
    timeout = TRAWLER_DELAY_MSEC;

    while( 1 ) {
        trequest_t *active_request;
        trequest_list_peek(list, &active_request);
        if( active_request == NULL ) {
            trawlerd_receive(server, list);
        } else {
            bool ready = zsocket_poll(server, timeout);
            if( ready ) {
                set_timeout( &timeout, &then, &now );

                trawlerd_receive(server, list);
                err=gettimeofday( &now, NULL);
                if( err ) exit( err );
            } else {
                trequest_list_peek(list, &active_request);
                assert( active_request != NULL );

                then = now;
                timeout = TRAWLER_DELAY_MSEC;

                trawlerd_fulfill_request(server, active_request, ch);
                trequest_list_shift( list );
            }
        }
    }
}

int trawlerd_receive(zmq_socket_t src, trequest_list_t *list) {
    zmsg_t *msg = zmsg_recv(src);
	zframe_t *client = zmsg_pop(msg);
    zframe_t *content_frame = zmsg_pop(msg);
	byte *content = zframe_data( content_frame );
	size_t bufsize = zframe_size( content_frame );
    Trawler__Request *preq = trawler__request__unpack(NULL, bufsize, content);
    zmsg_destroy(&msg);
    zframe_destroy(&content_frame);
    if( preq->method == TRAWLER__REQUEST__METHOD__GET 
        || preq->method == TRAWLER__REQUEST__METHOD__POST ) {
        trequest_node_t *node = malloc(sizeof(trequest_node_t));
        trawlerd_receive_request(client, preq, &(node->req));
        trequest_list_append( list, node );
        trawlerd_ack(src, client, preq->id);
    } else {
        trawlerd_nack(src, client, preq->id, TRAWLER_NACK_UNSUPPORTED_METHOD);
    }
    trawler__request__free_unpacked(preq, NULL);
    return 0;
}

int trawlerd_receive_request(zframe_t *client, Trawler__Request *preq, 
                             trequest_t *treq) {
    treq->client = client;
    trawler__reply__init(&(treq->reply));
    treq->id = preq->id;
    treq->method = preq->method;
    treq->path = strdup(preq->path);
    if( preq->query != NULL ) {
        treq->query = strdup(preq->query);
    } else {
        treq->query = NULL;
    }
    if( preq->session != NULL ) {
        treq->session = strdup(preq->session);
    } else {
        treq->session = NULL;
    }
    treq->headers = preq->headers;
    return 0;
}

static void free_chunk( void *data, void *arg __attribute__((unused))) {
    free(data);
}

static int trawlerd_reply(zmq_socket_t src, zframe_t *client, 
                          Trawler__Reply *reply) {
    unsigned int len = trawler__reply__get_packed_size(reply);
    void *buf = malloc(len);
    zframe_t *reply_frame;
    trawler__reply__pack(reply,buf);
    reply_frame = zframe_new_zero_copy(buf, len, free_chunk, NULL);
    zframe_send(&client, src, ZFRAME_REUSE + ZFRAME_MORE);
    zframe_send(&reply_frame, src, ZFRAME_REUSE);
    zframe_destroy(&reply_frame);
    trawler__reply__free_unpacked(buf, NULL);
    return 0;
}

int trawlerd_ack(zmq_socket_t src, zframe_t *client, int32_t req_id) {
    Trawler__Reply reply = TRAWLER__REPLY__INIT;
    reply.reply_type = TRAWLER__REPLY__REPLY_TYPE__Ack;
    reply.req_id = req_id;
    reply.result = TRAWLER_ACK_RESULT;
    return trawlerd_reply(src, client, &reply);
}

int trawlerd_nack(zmq_socket_t src, zframe_t *client,  int32_t req_id, 
                  int32_t result) {
    Trawler__Reply reply = TRAWLER__REPLY__INIT;
    reply.reply_type = TRAWLER__REPLY__REPLY_TYPE__Nack;
    reply.req_id = req_id;
    reply.result = result;
    return trawlerd_reply(src, client, &reply);
}

static char *concat(const char *a, const char *b, const char *c, 
                    const char *d) {
    char *res = strdup(a);
    size_t total = strlen(res)+strlen(b)+strlen(c);
    res = realloc(res,total+1);
    strcat(res, b);
    strcat(res, c);
    strcat(res, d);
    return res;
}

int trawlerd_fulfill_request(zmq_socket_t src, trequest_t *req, CURL *ch) {
    int err=0;
    char *url = NULL;
    err |= curl_easy_setopt( ch, CURLOPT_WRITEDATA, req );
    switch( req->method ) {
    case GET:
        url = concat(TRAWLER_BASE_URL,req->path,"?",req->query);
        err |= curl_easy_setopt( ch, CURLOPT_URL, url );
        break;
    case POST:
        url = concat(TRAWLER_BASE_URL,req->path,"","");
        err |= curl_easy_setopt( ch, CURLOPT_URL, url);
        err |= curl_easy_setopt( ch, CURLOPT_POSTFIELDS, req->query );
        break;
    default:
        return -1; /*TODO come up with return code for this 'impossible' case*/
    }
    if( req->session != NULL ) {
        err |= curl_easy_setopt( ch, CURLOPT_COOKIE, req->session );
    }
    err |= curl_easy_perform( ch );
    free(url);
    err |= curl_easy_getinfo( ch, CURLINFO_RESPONSE_CODE,
                              &(req->reply.response) );
    req->reply.req_id = req->id;
    return trawlerd_reply(src, req->client, &(req->reply));
}

static inline int trawlerd_str_append( char **str, size_t *strlen, 
                                       const size_t size, const size_t nmemb,
                                       void *stream) {
    char *cur = *str;
    char *new = realloc( cur, *strlen + size*nmemb );
    memmove( new+*strlen, stream, size*nmemb );
    if( cur != new ) {
        *str = new;
    }
    *strlen += size*nmemb;
    return 0;
}

int trawlerd_headers_append(trequest_t *treq, size_t size, size_t nmemb,
                            void *stream) {
    return trawlerd_str_append( &(treq->reply.headers),
                                &(treq->reply_headers_len),
                                size, nmemb, stream );
}

int trawlerd_response_append(trequest_t *treq, size_t size, size_t nmemb,
                             void *stream) {
    return trawlerd_str_append( &(treq->reply.response),
                                &(treq->reply_response_len),
                                size, nmemb, stream );
}

int trequest_list_new( trequest_list_t **list ) {
    *list = calloc(1, sizeof(trequest_list_t));
    return 0;
}

int trequest_list_append( trequest_list_t *list, trequest_node_t *trnode ) {
    if( list->first == NULL ) {
        assert( list->last == NULL );
        list->first = trnode;
        list->last = trnode;
    } else {
        list->last->next = trnode;
        list->last = trnode;
    }
    return 0;
}

int trequest_list_peek( trequest_list_t *list, trequest_t **treq ) {
    *treq = &(list->first->req);
    return 0;
}

int trequest_list_shift( trequest_list_t *list ) {
    trequest_node_t *condemned = list->first;
    list->first = condemned->next;
    free( condemned );
    return 0;
}
