/*  Copyright 2013-2014 Eluvatar
 *  
 *  This file is part of Trawler
 *
 *  Trawler is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public Licence as published by
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

static int trawlerd_shutdown(trawler_t *trawler);

int main(const int argc, const char **argv) {
    long verbose = 0;
    if( argc > 1 ) {
        for( int i=0; i<argc; i++ ) {
            if( argv[i][0] == '-' && argv[i][1] == 'v' && argv[i][2] == '\0' ) {
                verbose = 1;
            }
        }
    }
    return trawlerd_loop(verbose);
}

int trawlerd_init(CURL **ch, zmq_socket_t *server, long verbose) {
    int err;
    zctx_t *ctx;
    err = curl_global_init(CURL_GLOBAL_NOTHING);
    if( err != CURLE_OK )
        return err;
    *ch = curl_easy_init();
    err = curl_easy_setopt(*ch, CURLOPT_VERBOSE, verbose);
    err = curl_easy_setopt(*ch, CURLOPT_WRITEFUNCTION, trawlerd_response_append);
    if( err != CURLE_OK )
        return err;
    ctx = zctx_new();
    *server = zsocket_new( ctx, ZMQ_ROUTER );
    const char *url = "tcp://*:"TOSTRING(TRAWLER_PORT);
    int port = zsocket_bind( *server, url );
    assert(TRAWLER_PORT == port );
    return 0;
}

static inline void set_timeout( int *ret, int64_t *then, int64_t *now ) {
    *ret = TRAWLER_DELAY_MSEC + then - now;
}

static int trawler_interrupted = 0;

int trawlerd_loop(long verbose) {
    trequest_list_t *requests;
    int timeout;
    int64_t then, now;
    CURL *ch;
    zmq_socket_t server;
	trawler_t trawler;
    trawler.sessions = zhash_new();
    if( trawler.sessions == NULL ) {
        return 1;
    }

    trequest_list_new( &requests );

    // the below call sets global state -- do not run multiple trawlerd_loops
    trawlerd_register_sighandler();

    trawlerd_init( &ch, &server, verbose );

    trawler.req_list = requests;
    trawler.src = server;

    now = zclock_time();
    then = now;
    timeout = TRAWLER_DELAY_MSEC;


    while( ! trawler_interrupted ) {
        trequest_t *active_request;
        trequest_list_peek(requests, &active_request);
        if( active_request == NULL ) {
            trawlerd_receive(&trawler);
        } else {
            bool ready = zsocket_poll(server, timeout);
            if( ready ) {
                set_timeout( &timeout, &then, &now );

                trawlerd_receive(&trawler);
                now = zclock_time();
            } else {
                trequest_list_peek(requests, &active_request);
                assert( active_request != NULL );

                then = now;
                timeout = TRAWLER_DELAY_MSEC;

                trawlerd_fulfill_request(server, trawler.sessions,
                                         active_request, ch);
                trequest_list_shift( requests );
            }
        }
        trawlerd_reap( &trawler );
    }
    return trawlerd_shutdown( &trawler );
}

int trawlerd_receive( trawler_t *trawler ) {
    if( zctx_interrupted ) {
        return 1;
    }
    zmq_socket_t src = trawler->src;
    zhash_t *sessions = trawler->sessions;
    trequest_list_t *list = trawler->req_list;
    zmsg_t *msg = zmsg_recv(src);
    if( msg == NULL ) {
        return 1;
    }
    zframe_t *client = zmsg_unwrap(msg);
    char *client_hex = zframe_strhex(client);
    zframe_t *content_frame = zmsg_pop(msg);
    byte *content = zframe_data( content_frame );
    size_t bufsize = zframe_size( content_frame );
    Trawler__Login *login = NULL;
    Trawler__Request *preq = NULL;
	tsession_t *session = zhash_lookup(sessions, client_hex);
    if( session != NULL ) {
		preq = trawler__request__unpack(NULL, bufsize, content);
        trawlerd_update_last_activity(session);
    } else {
        login = trawler__login__unpack(NULL, bufsize, content);
        if( login != NULL ) {
   	        trawlerd_login(trawler, client, client_hex, login);
        } else {
            trawlerd_logout(src, client, TRAWLER_LOGOUT_LOGIN_SYNTAX);
        }
    }
    free(client_hex);
    zmsg_destroy(&msg);
    zframe_destroy(&content_frame);
    if( preq == NULL ) {
        if( login == NULL ) {
            return 2;
        }
        return 0;
    }
    if( preq->method == TRAWLER__REQUEST__METHOD__GET 
        || preq->method == TRAWLER__REQUEST__METHOD__POST ) {
        trequest_node_t *node = calloc(1,sizeof(trequest_node_t));
        session->req_count++;
        trawlerd_receive_request(client, preq, &(node->req));
        trequest_list_append( list, node );
        trawlerd_ack(src, client, preq->id);
    } else {
        trawlerd_nack(src, client, preq->id, TRAWLER_NACK_UNSUPPORTED_METHOD);
    }
    trawler__request__free_unpacked(preq, NULL);
    return 0;
}

typedef struct reaping {
    trawler_t *trawler;
    time_t now;
} reaping_t;

static int trawlerd_reap_inactive_fn(const char *client_hex, void *v, void *r) {
    tsession_t *session = (tsession_t*) v;
    const reaping_t *reaping = (reaping_t *) r;
    const time_t now = reaping->now;
    if( session->req_count == 0 
        && session->last_activity > now + TRAWLER_SESSION_TIMEOUT ) {
        trawlerd_logout(reaping->trawler->src, session->client, 
                        TRAWLER_LOGOUT_TIMEOUT);
        zhash_delete( reaping->trawler->sessions, client_hex );
    }
    return 0;
}

int trawlerd_reap(trawler_t *trawler) {
    reaping_t reaping;
    reaping.trawler = trawler;
    reaping.now=time(NULL);
    zhash_foreach(trawler->sessions, trawlerd_reap_inactive_fn, &reaping);
    return 0;
}

int trawlerd_login(trawler_t *trawler, zframe_t *client, const char *client_hex,
                   Trawler__Login *login) {
    int zh_res;
    tsession_t *session = malloc(sizeof(tsession_t)+strlen(login->user_agent)+1);
    // TODO check if safe
    session->client = client;
    session->last_activity = time(NULL);
    session->req_count = 0;
    strcpy(session->user_agent, login->user_agent);
    zh_res = zhash_insert( trawler->sessions, client_hex, session);
    void *item = zhash_freefn( trawler->sessions, client_hex, free );
    if( item == NULL ) {
        free( session );
        return zh_res || 1;
    }
    return zh_res;
}


int trawlerd_update_last_activity(tsession_t *session) {
    session->last_activity = time(NULL);
    return 0;
}

int trawlerd_receive_request(zframe_t *client, Trawler__Request *preq, 
                             trequest_t *treq) {
    treq->client = client;
    trawler__reply__init(&(treq->reply));
    treq->reply.req_id = treq->id = preq->id;
    treq->method = preq->method;
    treq->path = strdup(preq->path);
    treq->reply.headers.data = NULL;
    treq->reply.headers.len = 0;
    treq->reply.response.data = NULL;
    treq->reply.response.len = 0;
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

int trequest_destroy( trequest_t *treq ) {
    zframe_destroy( &(treq->client) );
    trawler__reply__free_unpacked( &(treq->reply), NULL );
    free( treq->path );
    free( treq->query );
    free( treq->session );
    return 0;
}

static void free_chunk( void *data, void *arg __attribute__((unused))) {
    free(data);
}

static int trawlerd_reply(zmq_socket_t src, zframe_t *client, 
                          Trawler__Reply *reply) {
    unsigned int len = trawler__reply__get_packed_size(reply);
    void *buf = malloc(len);
    zmsg_t *reply_msg = zmsg_new();
    zframe_t *client_copy = zframe_dup(client);
    zframe_t *reply_frame;
    trawler__reply__pack(reply,buf);
    reply_frame = zframe_new_zero_copy( buf, len, free_chunk, NULL);
    zmsg_add(reply_msg, reply_frame);
    zmsg_wrap(reply_msg, client_copy);
    return zmsg_send(&reply_msg, src);
}

int trawlerd_ack(zmq_socket_t src, zframe_t *client, int32_t req_id) {
    if( zctx_interrupted ) {
        return 1;
    }
    Trawler__Reply reply = TRAWLER__REPLY__INIT;
    reply.reply_type = TRAWLER__REPLY__REPLY_TYPE__Ack;
    reply.req_id = req_id;
    reply.result = TRAWLER_ACK_RESULT;
    return trawlerd_reply(src, client, &reply);
}

int trawlerd_nack(zmq_socket_t src, zframe_t *client,  int32_t req_id, 
                  int32_t result) {
    if( zctx_interrupted ) {
        return 1;
    }
    Trawler__Reply reply = TRAWLER__REPLY__INIT;
    reply.reply_type = TRAWLER__REPLY__REPLY_TYPE__Nack;
    reply.req_id = req_id;
    reply.result = result;
    return trawlerd_reply(src, client, &reply);
}

int trawlerd_logout(zmq_socket_t src, zframe_t *client, int32_t result) {
    Trawler__Reply reply = TRAWLER__REPLY__INIT;
    reply.reply_type = TRAWLER__REPLY__REPLY_TYPE__Logout;
    reply.req_id = 0;
    reply.result = result;
    return trawlerd_reply(src, client, &reply);
}

static char *concat(const char *a, const char *b, const char *c, 
                    const char *d) {
    char *res = strdup(a);
    size_t total = strlen(res) 
        + (b?strlen(b):0) + (c?strlen(c):0) + (d?strlen(d):0);
    res = realloc(res,total+1);
    if(b) strcat(res, b);
    if(c) strcat(res, c);
    if(d) strcat(res, d);
    return res;
}

int trawlerd_fulfill_request(zmq_socket_t src, zhash_t *sessions, trequest_t *req,
                             CURL *ch) {
    int err=0;
    char *url = NULL;
    char *client_hex = zframe_strhex(req->client);
    tsession_t *session = zhash_lookup(sessions, client_hex);
    char * user_agent = session->user_agent;
    err |= curl_easy_setopt( ch, CURLOPT_USERAGENT, user_agent );
    err |= curl_easy_setopt( ch, CURLOPT_WRITEDATA, req );
    switch( req->method ) {
    case GET:
        url = concat(TRAWLER_BASE_URL,req->path,"?",req->query);
        err |= curl_easy_setopt( ch, CURLOPT_URL, url );
        err |= curl_easy_setopt( ch, CURLOPT_HTTPGET, 1 );
        break;
    case POST:
        url = concat(TRAWLER_BASE_URL,req->path,"","");
        err |= curl_easy_setopt( ch, CURLOPT_URL, url);
        err |= curl_easy_setopt( ch, CURLOPT_POSTFIELDS, req->query );
        err |= curl_easy_setopt( ch, CURLOPT_POST, 1 );
        break;
    default:
        return -1; /*TODO come up with return code for this 'impossible' case*/
    }
    err |= curl_easy_setopt( ch, CURLOPT_COOKIE, req->session );
    req->reply.has_response = true;
    if( req->headers ) {
        curl_easy_setopt( ch, CURLOPT_HEADERFUNCTION, trawlerd_headers_append);
        curl_easy_setopt( ch, CURLOPT_HEADERDATA, req );
        req->reply.has_headers = true;
    } else {
        curl_easy_setopt( ch, CURLOPT_HEADERFUNCTION, NULL );
        curl_easy_setopt( ch, CURLOPT_HEADERDATA, NULL );
    }
    err |= curl_easy_perform( ch );
    free(url);
    long result;
    err |= curl_easy_getinfo( ch, CURLINFO_RESPONSE_CODE,
                              &result );
    req->reply.result = result;
    session->req_count--;
    free(client_hex);
    return trawlerd_reply(src, req->client, &(req->reply));
}

static inline int trawlerd_str_append( uint8_t **str, size_t *strlen, 
                                       const size_t size, const size_t nmemb,
                                       void *stream) {
    uint8_t *cur = *str;
    uint8_t *new = realloc( cur, *strlen + size*nmemb );
    memmove( new+*strlen, stream, size*nmemb );
    if( cur != new ) {
        *str = new;
    }
    *strlen += size*nmemb;
    return size*nmemb;
}

int trawlerd_headers_append(void *stream, size_t size, size_t nmemb,
                            trequest_t *treq) {
    return trawlerd_str_append( &(treq->reply.headers.data),
                                &(treq->reply.headers.len),
                                size, nmemb, stream );
}

int trawlerd_response_append(void *stream, size_t size, size_t nmemb,
                             trequest_t *treq) {
    return trawlerd_str_append( &(treq->reply.response.data),
                                &(treq->reply.response.len),
                                size, nmemb, stream );
}

int trequest_list_new( trequest_list_t **list ) {
    *list = calloc(1, sizeof(trequest_list_t));
    return 0;
}

int trequest_list_destroy( trequest_list_t **list ) {
    int err = 0;
    while( err == 0 && (*list)->first != NULL ) {
        err = trequest_list_shift( *list );
    }
    if( err == 0 ) {
        free( *list );
        *list = NULL;
    }
    return err;
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
    if( list->first == NULL ) {
        list->last = NULL;
    }
    trequest_destroy( &condemned->req );
    free( condemned );
    return 0;
}

static int trawlerd_reap_logout_fn(__attribute__((unused))const char *client_hex,
                                   void *v, void *t) {
    tsession_t *session = (tsession_t*) v;
    trawler_t *trawler = (trawler_t *) t;
    trawlerd_logout(trawler->src, session->client, TRAWLER_LOGOUT_SHUTDOWN);
    return 0;
}

static int trawlerd_shutdown(trawler_t *trawler) {
    zhash_foreach(trawler->sessions, trawlerd_reap_logout_fn, trawler);
    //zhash_destroy(trawler->sessions);
    trequest_list_destroy(&(trawler->req_list));
    return 0;
}

// static trawler_t *_trawler;
static void trawlerd_sighandler(__attribute__((unused))int signal) {
    //trawlerd_shutdown(_trawler);
    trawler_interrupted = 1;
}

int trawlerd_register_sighandler() {
    // Is incompatible with multiple daemons running in the same process...
    // but the owning of the port already blocks that :P
    // _trawler = trawler;
    trawler_interrupted = 0;
    /// SO MUCH RAGE
    zsys_handler_reset();
    zsys_handler_set(trawlerd_sighandler);
    return 0;
}
