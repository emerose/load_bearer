/**
 * @file   load_bearer.c
 * @brief  stupidly simple HTTP server, for use in performance/load tests
 * @mainpage
 *
 * Requires libevent: http://monkey.org/~provos/libevent/
 * To compile: gcc -levent -o load_bearer load_bearer.c
 *
 * @author  Sam Quigley <quigley@emerose.com>
 * Copyright (c) 2009; All rights reserved.
 * 
 * You can do whatever you want with this code, so long as the following
 * conditions are met:
 *  1. This copyright notice must be preserved in all copies, substantial
 *     portions of, and derivations from this work.
 *  2. You understand and agree that this code is provided "as-is"; that
 *     no warranties express or implied are provided; and that I am in no
 *     way responsible for what you do or do not do with it.
 *  2. If you like this code, you owe me a beer.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/queue.h>
#include <time.h>
#include <assert.h>
#include <event.h>
#include <evhttp.h>

#define LISTEN_IP           "0.0.0.0"
#define LISTEN_PORT         5000
#define NULL_RESP_PATH      "/"
#define DELAYED_RESP_PATH   "/delay"
#define BLOCKING_RESP_PATH  "/block"

/** 
 * @brief               Callback that simply returns a 200 OK 
 * @param[in,out]  req  a pointer to the evhttp_request struct we're responding to
 * @param[in,out]  args a void pointer (unused)
 * @return         void
 *
 * Simplest possible responder: returns a 200 with "OK" in the body.  
 *
 * @todo memoize the evbuffer in order to avoid runtime allocation.
 */
void null_response_cb (struct evhttp_request *req, void *args) {
  struct evbuffer *buf = evbuffer_new();
  
  evbuffer_add_printf(buf, "OK");
  evhttp_send_reply(req, 200, "OK", buf);
  evbuffer_free(buf);
  return;
}


/** 
 * Object for tracking pending HTTP responses.  Used when the callback doesn't
 * immediately respond to the request.
 */
struct http_response {
  struct evhttp_request *req;                   /** the request this is a response to */
  struct evbuffer *buf;                         /** the body of the response */
};


/** 
 * @brief               creates a new http_response object
 * @param[in]     req   a pointer to an evhttp_request (or NULL)
 * @param[in]     buf   a pointer to an evbuffer (or NULL)
 * @return        pointer to new http_response object
 *
 * creates a new http_response struct.  if memory cannot be allocated, causes
 * an assertion error.
 */
struct http_response *http_response_new(struct evhttp_request *req,
                                        struct evbuffer       *buf) {
  struct http_response *resp = malloc(sizeof(struct http_response));
  assert(resp);
  resp->req = req;
  resp->buf = buf;
  return resp;
}


/** 
 * @brief               destroys an http_response object 
 * @param[in]     ptr   pointer to an http_response object
 * @return        void
 *
 */
void http_response_free(struct http_response *ptr) {
  if (ptr->req) {
    evhttp_request_free(ptr->req);
  }
  if (ptr->buf) {
    evbuffer_free(ptr->buf);
  }
  free(ptr);
  return;
}


/** 
 * @brief                 Sends a pending HTTP response 
 * @param[in]     fd      an integer file descriptor (ignored)
 * @param[in]     type    a short representing an event type (also ignored)
 * @param[in]     args    a void pointer which points to an http_response
 * @return        void
 *
 * this callback delivers a pending HTTP response (struct http_response) with 
 * status code 200, and then frees the http_response.  the interface contains
 * the parameters required for event callbacks (cf. event_set in libevent),
 * but currently ignores all but the void *args parameter.  that parameter is
 * expected to point to an http_response struct, which will be freed by this
 * method.
 */
void send_response_cb(int fd, short type, void *args) {
  struct http_response *resp = (struct http_response *)args;

  evhttp_send_reply(resp->req, 200, "OK", resp->buf);
  // send_reply also frees the request, so we set it to NULL to prevent confusion
  resp->req = NULL;
  http_response_free(resp);
  return;
}


/** 
 * @brief               Parses an evhttp_request's URL parameters and returns 
 *                      the value for delay, if any. 
 * @param[in]     req   a pointer to an evhttp_request struct
 * @return        the (int) value of the 'delay' query string parameter, or 0 if none.
 *
 * uses evhttp_parse_query to parse the request's query string, returning an
 * int representing the value of the 'delay' param, if any, and 0 otherwise.
 *
 * @bug this is way too specific; if any other params are added, something
 * cleverer will be required.
 */
int requested_delay(struct evhttp_request *req) {
  struct evkeyval *param;
  struct evkeyvalq params;
  int wait = 0;

  evhttp_parse_query(evhttp_request_uri(req), &params);
  TAILQ_FOREACH(param, &params, next) {
    if (strncmp(param->key, "delay", 5)==0) {
      wait = atoi(param->value);
    }
  }
  return wait;
}


/** 
 * @brief               Delayed response callback, nonblocking version.
 * @param[in]     req   a pointer to the evhttp_request to respond to
 * @param[in]     args  a void pointer (unused)
 * @return        void
 *
 * intended to be triggered by an evhttp callback, this function prepares an
 * http_response struct and then calls event_once to schedule its delivery.
 */
void delayed_response_cb(struct evhttp_request *req, void *args) {
  int wait = requested_delay(req);
  struct timeval delay;
  delay.tv_sec = 0;
  delay.tv_usec = 1000 * wait;

  struct evbuffer *buf = evbuffer_new();
  evbuffer_add_printf(buf, "Waited %d ms", wait);

  struct http_response *resp = http_response_new(req, buf);

  event_once(-1, EV_TIMEOUT, send_response_cb, (void *)resp, &delay);
  return;
}


/** 
 * @brief               Delayed response callback, blocking version. 
 * @param[in]     req   a pointer to the evhttp_request to respond to
 * @param[in]     args  a void pointer (unused)
 * @return        void
 *
 * as with delayed_response_cb, this is intended to be triggered by an evhttp
 * callback, and sends a simple response back after the requested delay.
 * in contrast, however, this function calls usleep, and thus blocks the
 * entire process for the duration of the delay.  this can be useful to
 * simulate single-threaded backend server processes (eg, mongrel).
 */
void blocking_response_cb(struct evhttp_request *req, void *args) {
  int wait = requested_delay(req);

  struct evbuffer *buf = evbuffer_new();
  evbuffer_add_printf(buf, "Waited %d ms", wait);

  struct http_response *resp = http_response_new(req, buf);

  assert(usleep(wait*1000)==0);

  send_response_cb(-1, EV_TIMEOUT, resp);
  return;
}


int main ( int argc, char *argv[] ) {
  event_init();
  struct evhttp *http = evhttp_start(LISTEN_IP, LISTEN_PORT);
  evhttp_set_cb(http, NULL_RESP_PATH, null_response_cb, NULL);
  evhttp_set_cb(http, DELAYED_RESP_PATH, delayed_response_cb, NULL);
  evhttp_set_cb(http, BLOCKING_RESP_PATH, blocking_response_cb, NULL);

  event_dispatch();
  evhttp_free(http);
  return EXIT_SUCCESS;
}
