/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "test_tools.h"
#include "test_config.h"
#include <proton/condition.h>
#include <proton/connection.h>
#include <proton/event.h>
#include <proton/listener.h>
#include <proton/netaddr.h>
#include <proton/proactor.h>
#include <proton/ssl.h>
#include <proton/transport.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pn_millis_t timeout = 7*1000; /* timeout for hanging tests */

static const char *localhost = ""; /* host for connect/listen */

typedef pn_event_type_t (*test_handler_fn)(test_t *, pn_event_t*);

#define MAX_EVENT_LOG 2048         /* Max number of event types stored per proactor_test */

/* Proactor and handler that take part in a test */
typedef struct proactor_test_t {
  test_handler_fn handler;
  test_t *t;
  pn_proactor_t *proactor;
  pn_event_type_t log[MAX_EVENT_LOG]; /* Log of event types generated by proactor */
  size_t log_len;                     /* Number of events in the log */
} proactor_test_t;


/* Initialize an array of proactor_test_t */
static void proactor_test_init(proactor_test_t *pts, size_t n, test_t *t) {
  for (proactor_test_t *pt = pts; pt < pts + n; ++pt) {
    if (!pt->t) pt->t = t;
    if (!pt->proactor) pt->proactor = pn_proactor();
    pt->log_len = 0;
    pn_proactor_set_timeout(pt->proactor, timeout);
  }
}

#define PROACTOR_TEST_INIT(A, T) proactor_test_init(A, sizeof(A)/sizeof(*A), (T))

static void proactor_test_free(proactor_test_t *pts, size_t n) {
  for (proactor_test_t *pt = pts; pt < pts + n; ++pt) {
    pn_proactor_free(pt->proactor);
  }
}

#define PROACTOR_TEST_FREE(A) proactor_test_free(A, sizeof(A)/sizeof(*A))

#define TEST_LOG_EQUAL(T, A, PT) \
  TEST_ETYPES_EQUAL((T), (A), sizeof(A)/sizeof(*A), (PT).log, (PT).log_len)

#if 0                           /* FIXME aconway 2017-03-31:  */
/* Return the last event in the proactor_test's log or PN_EVENT_NONE if it is empty */
static pn_event_type_t  proactor_test_last_event(proactor_test_t *pt) {
  return pt->log_len ? pt->log[pt->log_len - 1] : PN_EVENT_NONE;
}
#endif

/* Set this to a pn_condition() to save condition data */
pn_condition_t *last_condition = NULL;

static void save_condition(pn_event_t *e) {
  if (last_condition) {
    pn_condition_t *cond = NULL;
    if (pn_event_listener(e)) {
      cond = pn_listener_condition(pn_event_listener(e));
    } else {
      cond = pn_event_condition(e);
    }
    if (cond) {
      pn_condition_copy(last_condition, cond);
    } else {
      pn_condition_clear(last_condition);
    }
  }
}

/* Process events on a proactor array until a handler returns an event, or
 * all proactors return NULL
 */
static pn_event_type_t proactor_test_get(proactor_test_t *pts, size_t n) {
  if (last_condition) pn_condition_clear(last_condition);
  while (true) {
    bool busy = false;
    for (proactor_test_t *pt = pts; pt < pts + n; ++pt) {
      pn_event_batch_t *eb =  pn_proactor_get(pt->proactor);
      if (eb) {
        busy = true;
        pn_event_type_t ret = PN_EVENT_NONE;
        for (pn_event_t* e = pn_event_batch_next(eb); e; e = pn_event_batch_next(eb)) {
          TEST_ASSERT(pt->log_len < MAX_EVENT_LOG);
          pt->log[pt->log_len++] = pn_event_type(e);
          save_condition(e);
          ret = pt->handler(pt->t, e);
          if (ret) break;
        }
        pn_proactor_done(pt->proactor, eb);
        if (ret) return ret;
      }
    }
    if (!busy) {
      return PN_EVENT_NONE;
    }
  }
}

/* Run an array of proactors till a handler returns an event. */
static pn_event_type_t proactor_test_run(proactor_test_t *pts, size_t n) {
  pn_event_type_t e;
  while ((e = proactor_test_get(pts, n)) == PN_EVENT_NONE)
         ;
  return e;
}


/* Drain and discard outstanding events from an array of proactors */
static void proactor_test_drain(proactor_test_t *pts, size_t n) {
  while (proactor_test_get(pts, n))
         ;
}


#define PROACTOR_TEST_GET(A) proactor_test_get((A), sizeof(A)/sizeof(*A))
#define PROACTOR_TEST_RUN(A) proactor_test_run((A), sizeof(A)/sizeof(*A))
#define PROACTOR_TEST_DRAIN(A) proactor_test_drain((A), sizeof(A)/sizeof(*A))

/* Combine a test_port with a pn_listener */
typedef struct proactor_test_listener_t {
  test_port_t port;
  pn_listener_t *listener;
} proactor_test_listener_t;

proactor_test_listener_t proactor_test_listen(proactor_test_t *pt, const char *host) {
  proactor_test_listener_t l = { test_port(host), pn_listener() };
  pn_proactor_listen(pt->proactor, l.listener, l.port.host_port, 4);
  TEST_ETYPE_EQUAL(pt->t, PN_LISTENER_OPEN, proactor_test_run(pt, 1));
  sock_close(l.port.sock);
  return l;
}


/* Wait for the next single event, return its type */
static pn_event_type_t wait_next(pn_proactor_t *proactor) {
  pn_event_batch_t *events = pn_proactor_wait(proactor);
  pn_event_type_t etype = pn_event_type(pn_event_batch_next(events));
  pn_proactor_done(proactor, events);
  return etype;
}

/* Test that interrupt and timeout events cause pn_proactor_wait() to return. */
static void test_interrupt_timeout(test_t *t) {
  pn_proactor_t *p = pn_proactor();
  TEST_CHECK(t, pn_proactor_get(p) == NULL); /* idle */
  pn_proactor_interrupt(p);
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_INTERRUPT, wait_next(p));
  TEST_CHECK(t, pn_proactor_get(p) == NULL); /* idle */

  /* Set an immediate timeout */
  pn_proactor_set_timeout(p, 0);
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_TIMEOUT, wait_next(p));

  /* Set a (very short) timeout */
  pn_proactor_set_timeout(p, 10);
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_TIMEOUT, wait_next(p));

  /* Set and cancel a timeout, make sure we don't get the timeout event */
  pn_proactor_set_timeout(p, 10);
  pn_proactor_cancel_timeout(p);
  TEST_CHECK(t, pn_proactor_get(p) == NULL); /* idle */

  pn_proactor_free(p);
}

/* Save the last connection accepted by the common_handler */
pn_connection_t *last_accepted = NULL;

/* Common handler for simple client/server interactions,  */
static pn_event_type_t common_handler(test_t *t, pn_event_t *e) {
  pn_connection_t *c = pn_event_connection(e);
  pn_listener_t *l = pn_event_listener(e);

  switch (pn_event_type(e)) {

    /* Stop on these events */
   case PN_TRANSPORT_CLOSED:
   case PN_PROACTOR_INACTIVE:
   case PN_PROACTOR_TIMEOUT:
   case PN_LISTENER_OPEN:
    return pn_event_type(e);

   case PN_LISTENER_ACCEPT:
    last_accepted = pn_connection();
    pn_listener_accept(l, last_accepted);
    pn_listener_close(l);       /* Only accept one connection */
    return PN_EVENT_NONE;

   case PN_CONNECTION_REMOTE_OPEN:
    pn_connection_open(c);      /* Return the open (no-op if already open) */
    return PN_EVENT_NONE;

   case PN_CONNECTION_REMOTE_CLOSE:
    pn_connection_close(c);     /* Return the close */
    return PN_EVENT_NONE;

    /* Ignore these events */
   case PN_CONNECTION_INIT:
   case PN_CONNECTION_BOUND:
   case PN_CONNECTION_LOCAL_OPEN:
   case PN_CONNECTION_LOCAL_CLOSE:
   case PN_LISTENER_CLOSE:
   case PN_TRANSPORT:
   case PN_TRANSPORT_ERROR:
   case PN_TRANSPORT_HEAD_CLOSED:
   case PN_TRANSPORT_TAIL_CLOSED:
    return PN_EVENT_NONE;

   default:
    TEST_ERRORF(t, "unexpected event %s", pn_event_type_name(pn_event_type(e)));
    return PN_EVENT_NONE;                   /* Fail the test but keep going */
  }
}

/* Like common_handler but does not auto-close the listener after one accept */
static pn_event_type_t listen_handler(test_t *t, pn_event_t *e) {
  switch (pn_event_type(e)) {
   case PN_LISTENER_ACCEPT:
    /* No automatic listener close/free for tests that accept multiple connections */
    last_accepted = pn_connection();
    pn_listener_accept(pn_event_listener(e), last_accepted);
    return PN_EVENT_NONE;

   case PN_LISTENER_CLOSE:
    /* No automatic free */
    return PN_LISTENER_CLOSE;

   default:
    return common_handler(t, e);
  }
}

/* close a connection when it is remote open */
static pn_event_type_t open_close_handler(test_t *t, pn_event_t *e) {
  switch (pn_event_type(e)) {
   case PN_CONNECTION_REMOTE_OPEN:
    pn_connection_close(pn_event_connection(e));
    return PN_EVENT_NONE;          /* common_handler will finish on TRANSPORT_CLOSED */
   default:
    return common_handler(t, e);
  }
}

/* Test simple client/server connection with 2 proactors */
static void test_client_server(test_t *t) {
  proactor_test_t pts[] ={ { open_close_handler }, { common_handler } };
  PROACTOR_TEST_INIT(pts, t);
  proactor_test_listener_t l = proactor_test_listen(&pts[1], localhost);
  /* Connect and wait for close at both ends */
  pn_proactor_connect(pts[0].proactor, pn_connection(), l.port.host_port);
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  PROACTOR_TEST_FREE(pts);
}

/* Return on connection open, close and return on wake */
static pn_event_type_t open_wake_handler(test_t *t, pn_event_t *e) {
  switch (pn_event_type(e)) {
   case PN_CONNECTION_REMOTE_OPEN:
    return pn_event_type(e);
   case PN_CONNECTION_WAKE:
    pn_connection_close(pn_event_connection(e));
    return pn_event_type(e);
   default:
    return common_handler(t, e);
  }
}

/* Test waking up a connection that is idle */
static void test_connection_wake(test_t *t) {
  proactor_test_t pts[] =  { { open_wake_handler }, { common_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port(localhost);          /* Hold a port */
  pn_proactor_listen(server, pn_listener(), port.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port.sock);

  pn_connection_t *c = pn_connection();
  pn_incref(c);                 /* Keep a reference for wake() after free */
  pn_proactor_connect(client, c, port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  TEST_CHECK(t, pn_proactor_get(client) == NULL); /* Should be idle */
  pn_connection_wake(c);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_WAKE, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  /* The pn_connection_t is still valid so wake is legal but a no-op */
  pn_connection_wake(c);

  PROACTOR_TEST_FREE(pts);
  /* The pn_connection_t is still valid so wake is legal but a no-op */
  pn_connection_wake(c);
  pn_decref(c);
}

/* Close the transport to abort a connection, i.e. close the socket without an AMQP close */
static pn_event_type_t listen_abort_handler(test_t *t, pn_event_t *e) {
  switch (pn_event_type(e)) {
   case PN_CONNECTION_REMOTE_OPEN:
    /* Close the transport - abruptly closes the socket */
    pn_transport_close_tail(pn_connection_transport(pn_event_connection(e)));
    pn_transport_close_head(pn_connection_transport(pn_event_connection(e)));
    return PN_EVENT_NONE;

   default:
    /* Don't auto-close the listener to keep the event sequences simple */
    return listen_handler(t, e);
  }
}

/* Verify that pn_transport_close_head/tail aborts a connection without an AMQP protoocol close */
static void test_abort(test_t *t) {
  proactor_test_t pts[] ={ { open_close_handler }, { listen_abort_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port(localhost);
  pn_listener_t *l = pn_listener();
  pn_proactor_listen(server, l, port.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port.sock);
  pn_proactor_connect(client, pn_connection(), port.host_port);
  /* server transport closes */
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  if (TEST_CHECK(t, last_condition) && TEST_CHECK(t, pn_condition_is_set(last_condition))) {
    TEST_STR_EQUAL(t, "amqp:connection:framing-error", pn_condition_get_name(last_condition));
    TEST_STR_IN(t, "abort", pn_condition_get_description(last_condition));
  }
  /* client transport closes */
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts)); /* client */
  if (TEST_CHECK(t, last_condition) && TEST_CHECK(t, pn_condition_is_set(last_condition))) {
    TEST_STR_EQUAL(t, "amqp:connection:framing-error", pn_condition_get_name(last_condition));
    TEST_STR_IN(t, "abort", pn_condition_get_description(last_condition));
  }
  pn_listener_close(l);
  PROACTOR_TEST_DRAIN(pts);

  /* Verify expected event sequences, no unexpected events */
  static const pn_event_type_t want_client[] = {
    PN_CONNECTION_INIT,
    PN_CONNECTION_LOCAL_OPEN,
    PN_CONNECTION_BOUND,
    PN_TRANSPORT_TAIL_CLOSED,
    PN_TRANSPORT_ERROR,
    PN_TRANSPORT_HEAD_CLOSED,
    PN_TRANSPORT_CLOSED
  };
  TEST_LOG_EQUAL(t, want_client, pts[0]);

  static const pn_event_type_t want_server[] = {
    PN_LISTENER_OPEN,
    PN_LISTENER_ACCEPT,
    PN_CONNECTION_INIT,
    PN_CONNECTION_BOUND,
    PN_CONNECTION_REMOTE_OPEN,
    PN_TRANSPORT_TAIL_CLOSED,
    PN_TRANSPORT_ERROR,
    PN_TRANSPORT_HEAD_CLOSED,
    PN_TRANSPORT_CLOSED,
    PN_LISTENER_CLOSE
  };
  TEST_LOG_EQUAL(t, want_server, pts[1]);

  PROACTOR_TEST_FREE(pts);
}

/* Refuse a connection: abort before the AMQP open sequence begins. */
static pn_event_type_t listen_refuse_handler(test_t *t, pn_event_t *e) {
  switch (pn_event_type(e)) {

   case PN_CONNECTION_BOUND:
    /* Close the transport - abruptly closes the socket */
    pn_transport_close_tail(pn_connection_transport(pn_event_connection(e)));
    pn_transport_close_head(pn_connection_transport(pn_event_connection(e)));
    return PN_EVENT_NONE;

   default:
    /* Don't auto-close the listener to keep the event sequences simple */
    return listen_handler(t, e);
  }
}

/* Verify that pn_transport_close_head/tail aborts a connection without an AMQP protoocol close */
static void test_refuse(test_t *t) {
  proactor_test_t pts[] = { { open_close_handler }, { listen_refuse_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor;
  proactor_test_listener_t l = proactor_test_listen(&pts[1], localhost);
  pn_proactor_connect(client, pn_connection(), l.port.host_port);

  /* client transport closes */
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts)); /* client */
  if (TEST_CHECK(t, last_condition) && TEST_CHECK(t, pn_condition_is_set(last_condition))) {
    TEST_STR_EQUAL(t, "amqp:connection:framing-error", pn_condition_get_name(last_condition));
  }
  pn_listener_close(l.listener);
  PROACTOR_TEST_DRAIN(pts);

  /* Verify expected event sequences, no unexpected events */
  static const pn_event_type_t want_client[] = {
    PN_CONNECTION_INIT,
    PN_CONNECTION_LOCAL_OPEN,
    PN_CONNECTION_BOUND,
    PN_TRANSPORT_TAIL_CLOSED,
    PN_TRANSPORT_ERROR,
    PN_TRANSPORT_HEAD_CLOSED,
    PN_TRANSPORT_CLOSED
  };
  TEST_LOG_EQUAL(t, want_client, pts[0]);

  static const pn_event_type_t want_server[] = {
    PN_LISTENER_OPEN,
    PN_LISTENER_ACCEPT,
    PN_CONNECTION_INIT,
    PN_CONNECTION_BOUND,
    PN_TRANSPORT_TAIL_CLOSED,
    PN_TRANSPORT_ERROR,
    PN_TRANSPORT_HEAD_CLOSED,
    PN_TRANSPORT_CLOSED,
    PN_LISTENER_CLOSE
  };
  TEST_LOG_EQUAL(t, want_server, pts[1]);

  PROACTOR_TEST_FREE(pts);
}

/* Test that INACTIVE event is generated when last connections/listeners closes. */
static void test_inactive(test_t *t) {
  proactor_test_t pts[] =  { { open_wake_handler }, { listen_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port(localhost);          /* Hold a port */

  pn_listener_t *l = pn_listener();
  pn_proactor_listen(server, l, port.host_port,  4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  pn_connection_t *c = pn_connection();
  pn_proactor_connect(client, c, port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  pn_connection_wake(c);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_WAKE, PROACTOR_TEST_RUN(pts));
  /* expect TRANSPORT_CLOSED from client and server, INACTIVE from client */
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_INACTIVE, PROACTOR_TEST_RUN(pts));
  /* server won't be INACTIVE until listener is closed */
  TEST_CHECK(t, pn_proactor_get(server) == NULL);
  pn_listener_close(l);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_CLOSE, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_INACTIVE, PROACTOR_TEST_RUN(pts));

  sock_close(port.sock);
  PROACTOR_TEST_FREE(pts);
}

/* Tests for error handling */
static void test_errors(test_t *t) {
  proactor_test_t pts[] =  { { open_wake_handler }, { listen_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port(localhost);          /* Hold a port */

  /* Invalid connect/listen parameters */
  pn_connection_t *c = pn_connection();
  pn_proactor_connect(client, c, "127.0.0.1:xxx");
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_STR_IN(t, "xxx", pn_condition_get_description(last_condition));
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_INACTIVE, PROACTOR_TEST_RUN(pts));

  pn_listener_t *l = pn_listener();
  pn_proactor_listen(server, l, "127.0.0.1:xxx", 1);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_LISTENER_CLOSE, PROACTOR_TEST_RUN(pts));
  TEST_STR_IN(t, "xxx", pn_condition_get_description(last_condition));
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_INACTIVE, PROACTOR_TEST_RUN(pts));

  /* Connect with no listener */
  c = pn_connection();
  pn_proactor_connect(client, c, port.host_port);
  if (TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts))) {
    TEST_STR_IN(t, "refused", pn_condition_get_description(last_condition));
    TEST_ETYPE_EQUAL(t, PN_PROACTOR_INACTIVE, PROACTOR_TEST_RUN(pts));
    sock_close(port.sock);
    PROACTOR_TEST_FREE(pts);
  }
}

/* Test that we can control listen/select on ipv6/v4 and listen on both by default */
static void test_ipv4_ipv6(test_t *t) {
  proactor_test_t pts[] ={ { open_close_handler }, { listen_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;

  /* Listen on all interfaces for IPv6 only. If this fails, skip IPv6 tests */
  test_port_t port6 = test_port("::");
  pn_listener_t *l6 = pn_listener();
  pn_proactor_listen(server, l6, port6.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port6.sock);
  pn_event_type_t e = PROACTOR_TEST_GET(pts);
  bool has_ipv6 = (e != PN_LISTENER_CLOSE);
  if (!has_ipv6) {
    TEST_LOGF(t, "skip IPv6 tests: %s", pn_condition_get_description(last_condition));
  }
  PROACTOR_TEST_DRAIN(pts);

  /* Listen on all interfaces for IPv4 only. */
  test_port_t port4 = test_port("0.0.0.0");
  pn_listener_t *l4 = pn_listener();
  pn_proactor_listen(server, l4, port4.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port4.sock);
  TEST_CHECKF(t, PROACTOR_TEST_GET(pts) != PN_LISTENER_CLOSE, "listener error: %s",  pn_condition_get_description(last_condition));
  PROACTOR_TEST_DRAIN(pts);

  /* Empty address listens on both IPv4 and IPv6 on all interfaces */
  test_port_t port = test_port("");
  pn_listener_t *l = pn_listener();
  pn_proactor_listen(server, l, port.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port.sock);
  e = PROACTOR_TEST_GET(pts);
  TEST_CHECKF(t, PROACTOR_TEST_GET(pts) != PN_LISTENER_CLOSE, "listener error: %s",  pn_condition_get_description(last_condition));
  PROACTOR_TEST_DRAIN(pts);

#define EXPECT_CONNECT(TP, HOST) do {                                   \
    pn_proactor_connect(client, pn_connection(), test_port_use_host(&(TP), (HOST))); \
    TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));   \
    TEST_CHECK(t, !pn_condition_is_set(last_condition));                 \
    PROACTOR_TEST_DRAIN(pts);                                           \
  } while(0)

#define EXPECT_FAIL(TP, HOST) do {                                      \
    pn_proactor_connect(client, pn_connection(), test_port_use_host(&(TP), (HOST))); \
    TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));   \
    if (TEST_CHECK(t, pn_condition_is_set(last_condition)))             \
      TEST_STR_IN(t, "refused", pn_condition_get_description(last_condition)); \
    PROACTOR_TEST_DRAIN(pts);                                           \
  } while(0)

  EXPECT_CONNECT(port4, "127.0.0.1"); /* v4->v4 */
  EXPECT_CONNECT(port4, "");          /* local->v4*/

  EXPECT_CONNECT(port, "127.0.0.1"); /* v4->all */
  EXPECT_CONNECT(port, "");          /* local->all */

  if (has_ipv6) {
    EXPECT_CONNECT(port6, "::"); /* v6->v6 */
    EXPECT_CONNECT(port6, "");     /* local->v6 */
    EXPECT_CONNECT(port, "::1"); /* v6->all */

    EXPECT_FAIL(port6, "127.0.0.1"); /* fail v4->v6 */
    EXPECT_FAIL(port4, "::1");     /* fail v6->v4 */
  }
  PROACTOR_TEST_DRAIN(pts);

  pn_listener_close(l);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_CLOSE, PROACTOR_TEST_RUN(pts));
  pn_listener_close(l4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_CLOSE, PROACTOR_TEST_RUN(pts));
  if (has_ipv6) {
    pn_listener_close(l6);
    TEST_ETYPE_EQUAL(t, PN_LISTENER_CLOSE, PROACTOR_TEST_RUN(pts));
  }

  PROACTOR_TEST_FREE(pts);
}

/* Make sure we clean up released connections and open sockets correctly */
static void test_release_free(test_t *t) {
  proactor_test_t pts[] = { { open_wake_handler }, { listen_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port(localhost);
  pn_listener_t *l = pn_listener();
  pn_proactor_listen(server, l, port.host_port, 2);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));

  /* leave one connection to the proactor  */
  pn_proactor_connect(client, pn_connection(), port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));

  /* release c1 and free immediately */
  pn_connection_t *c1 = pn_connection();
  pn_proactor_connect(client, c1, port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  pn_proactor_release_connection(c1); /* We free but socket should still be cleaned up */
  pn_connection_free(c1);
  TEST_CHECK(t, pn_proactor_get(client) == NULL); /* Should be idle */
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts)); /* Server closed */

  /* release c2 and but don't free till after proactor free */
  pn_connection_t *c2 = pn_connection();
  pn_proactor_connect(client, c2, port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  pn_proactor_release_connection(c2);
  TEST_CHECK(t, pn_proactor_get(client) == NULL); /* Should be idle */
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts)); /* Server closed */

  PROACTOR_TEST_FREE(pts);
  pn_connection_free(c2);

  /* Check freeing a listener or connection that was never given to a proactor */
  pn_listener_free(pn_listener());
  pn_connection_free(pn_connection());
}

/* TODO aconway 2017-03-27: need windows version with .p12 certs */
#define CERTFILE(NAME) CMAKE_CURRENT_SOURCE_DIR "/ssl_certs/" NAME ".pem"

static pn_event_type_t ssl_handler(test_t *t, pn_event_t *e) {
  pn_connection_t *c = pn_event_connection(e);
  switch (pn_event_type(e)) {

   case PN_CONNECTION_BOUND: {
     bool incoming = (pn_connection_state(c) & PN_LOCAL_UNINIT);
     pn_ssl_domain_t *ssld = pn_ssl_domain(incoming ? PN_SSL_MODE_SERVER : PN_SSL_MODE_CLIENT);
     TEST_CHECK(t, 0 == pn_ssl_domain_set_credentials(
                  ssld, CERTFILE("tserver-certificate"), CERTFILE("tserver-private-key"), "tserverpw"));
     TEST_CHECK(t, 0 == pn_ssl_init(pn_ssl(pn_event_transport(e)), ssld, NULL));
     pn_ssl_domain_free(ssld);
     return PN_EVENT_NONE;
   }

   case PN_CONNECTION_REMOTE_OPEN: {
     if (pn_connection_state(c) | PN_LOCAL_ACTIVE) {
       /* Outgoing connection is complete, close it */
       pn_connection_close(c);
     } else {
       /* Incoming connection, check for SSL */
       pn_ssl_t *ssl = pn_ssl(pn_event_transport(e));
       TEST_CHECK(t, ssl);
       TEST_CHECK(t, pn_ssl_get_protocol_name(ssl, NULL, 0));
       pn_connection_open(c);      /* Return the open (no-op if already open) */
     }
    return PN_CONNECTION_REMOTE_OPEN;
   }

   default:
    return common_handler(t, e);
  }
}

/* Establish an SSL connection between proactors*/
static void test_ssl(test_t *t) {
  if (!pn_ssl_present()) {
    TEST_LOGF(t, "Skip SSL test, no support");
    return;
  }

  proactor_test_t pts[] ={ { ssl_handler }, { ssl_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port(localhost);
  pn_proactor_listen(server, pn_listener(), port.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port.sock);
  pn_connection_t *c = pn_connection();
  pn_proactor_connect(client, c, port.host_port);
  /* Open ok at both ends */
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  TEST_CHECK(t, !pn_condition_is_set(last_condition));
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  TEST_CHECK(t, !pn_condition_is_set(last_condition));
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));

  PROACTOR_TEST_FREE(pts);
}

static void test_proactor_addr(test_t *t) {
  /* Test the address formatter */
  char addr[PN_MAX_ADDR];
  pn_proactor_addr(addr, sizeof(addr), "foo", "bar");
  TEST_STR_EQUAL(t, "foo:bar", addr);
  pn_proactor_addr(addr, sizeof(addr), "foo", "");
  TEST_STR_EQUAL(t, "foo:", addr);
  pn_proactor_addr(addr, sizeof(addr), "foo", NULL);
  TEST_STR_EQUAL(t, "foo:", addr);
  pn_proactor_addr(addr, sizeof(addr), "", "bar");
  TEST_STR_EQUAL(t, ":bar", addr);
  pn_proactor_addr(addr, sizeof(addr), NULL, "bar");
  TEST_STR_EQUAL(t, ":bar", addr);
  pn_proactor_addr(addr, sizeof(addr), "1:2:3:4", "5");
  TEST_STR_EQUAL(t, "1:2:3:4:5", addr);
  pn_proactor_addr(addr, sizeof(addr), "1:2:3:4", "");
  TEST_STR_EQUAL(t, "1:2:3:4:", addr);
  pn_proactor_addr(addr, sizeof(addr), "1:2:3:4", NULL);
  TEST_STR_EQUAL(t, "1:2:3:4:", addr);
}

/* Test pn_proactor_addr funtions */

/* FIXME aconway 2017-03-30: windows will need winsock2.h etc.
   These headers are *only* needed for test_netaddr and only for the getnameinfo part.
   This is the only non-portable part of the proactor test suite.
   */
#include <sys/socket.h>         /* For socket_storage */
#include <netdb.h>              /* For NI_MAXHOST/NI_MAXSERV */

static void test_netaddr(test_t *t) {
  proactor_test_t pts[] ={ { open_wake_handler }, { listen_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;
  test_port_t port = test_port("127.0.0.1"); /* Use IPv4 to get consistent results all platforms */
  pn_listener_t *l = pn_listener();
  pn_proactor_listen(server, l, port.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  pn_connection_t *c = pn_connection();
  pn_proactor_connect(client, c, port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));

  /* client remote, client local, server remote and server local address strings */
  char cr[1024], cl[1024], sr[1024], sl[1024];

  pn_transport_t *ct = pn_connection_transport(c);
  pn_netaddr_str(pn_netaddr_remote(ct), cr, sizeof(cr));
  TEST_STR_IN(t, test_port_use_host(&port, ""), cr); /* remote address has listening port */

  pn_connection_t *s = last_accepted; /* server side of the connection */
  pn_transport_t *st = pn_connection_transport(s);
  if (!TEST_CHECK(t, st)) return;
  pn_netaddr_str(pn_netaddr_local(st), sl, sizeof(sl));
  TEST_STR_EQUAL(t, cr, sl);  /* client remote == server local */

  pn_netaddr_str(pn_netaddr_local(ct), cl, sizeof(cl));
  pn_netaddr_str(pn_netaddr_remote(st), sr, sizeof(sr));
  TEST_STR_EQUAL(t, cl, sr);    /* client local == server remote */

  /* Examine as sockaddr */
  pn_netaddr_t *na = pn_netaddr_remote(ct);
  struct sockaddr *sa = pn_netaddr_sockaddr(na);
  TEST_CHECK(t, AF_INET == sa->sa_family);
  char host[NI_MAXHOST] = "";
  char serv[NI_MAXSERV] = "";
  int err = getnameinfo(sa, pn_netaddr_socklen(na),
                        host, sizeof(host), serv, sizeof(serv),
                        NI_NUMERICHOST | NI_NUMERICSERV);
  TEST_CHECK(t, 0 == err);
  TEST_STR_EQUAL(t, "127.0.0.1", host);
  TEST_STR_EQUAL(t, port.str, serv);

  /* Make sure you can use NULL, 0 to get length of address string without a crash */
  size_t len = pn_netaddr_str(pn_netaddr_local(ct), NULL, 0);
  TEST_CHECKF(t, strlen(cl) == len, "%d != %d", strlen(cl), len);

  sock_close(port.sock);
  PROACTOR_TEST_DRAIN(pts);
  PROACTOR_TEST_FREE(pts);
}

/* Test pn_proactor_disconnect */
static void test_disconnect(test_t *t) {
  proactor_test_t pts[] ={ { open_wake_handler }, { listen_handler } };
  PROACTOR_TEST_INIT(pts, t);
  pn_proactor_t *client = pts[0].proactor, *server = pts[1].proactor;

  test_port_t port = test_port(localhost);
  pn_listener_t* l = pn_listener();
  pn_proactor_listen(server, l, port.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port.sock);

  test_port_t port2 = test_port(localhost);
  pn_listener_t* l2 = pn_listener();
  pn_proactor_listen(server, l2, port2.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port2.sock);

  /* We will disconnect one connection after it is remote-open */
  pn_proactor_connect(client, pn_connection(), port.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  pn_proactor_connect(client, pn_connection(), port2.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));

  pn_condition_t *cond = pn_condition();
  pn_condition_set_name(cond, "test-name");
  pn_condition_set_description(cond, "test-description");

  pn_proactor_disconnect(client, cond);
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_STR_EQUAL(t, "test-name", pn_condition_get_name(last_condition));
  /* Note: pn_transport adds "(connection aborted)" on client side if transport closed early. */
  TEST_STR_EQUAL(t, "test-description (connection aborted)", pn_condition_get_description(last_condition));
  TEST_ETYPE_EQUAL(t, PN_TRANSPORT_CLOSED, PROACTOR_TEST_RUN(pts));
  TEST_ETYPE_EQUAL(t, PN_PROACTOR_INACTIVE, PROACTOR_TEST_RUN(pts));

  pn_proactor_disconnect(server, cond);
  int expect_tclose = 2, expect_lclose = 2;
  while (expect_tclose || expect_lclose) {
    pn_event_type_t et = PROACTOR_TEST_RUN(pts);
    switch (et) {
     case PN_TRANSPORT_CLOSED:
      TEST_CHECK(t, --expect_tclose >= 0);
        TEST_STR_EQUAL(t, "test-name", pn_condition_get_name(last_condition));
        TEST_STR_EQUAL(t, "test-description", pn_condition_get_description(last_condition));
      break;
     case PN_LISTENER_CLOSE:
      TEST_CHECK(t, --expect_lclose >= 0);
      TEST_STR_EQUAL(t, "test-name", pn_condition_get_name(last_condition));
      TEST_STR_EQUAL(t, "test-description", pn_condition_get_description(last_condition));
      break;
     default:
      TEST_ERRORF(t, "%s unexpected: want %d TRANSPORT_CLOSED, %d LISTENER_CLOSE",
                  pn_event_type_name(et), expect_tclose, expect_lclose);
      expect_lclose = expect_tclose = 0;
      continue;
    }
  }

  pn_condition_free(cond);

  /* Make sure the proactors are still functional */
  test_port_t port3 = test_port(localhost);
  pn_listener_t* l3 = pn_listener();
  pn_proactor_listen(server, l3, port3.host_port, 4);
  TEST_ETYPE_EQUAL(t, PN_LISTENER_OPEN, PROACTOR_TEST_RUN(pts));
  sock_close(port3.sock);
  pn_proactor_connect(client, pn_connection(), port3.host_port);
  TEST_ETYPE_EQUAL(t, PN_CONNECTION_REMOTE_OPEN, PROACTOR_TEST_RUN(pts));
  pn_proactor_disconnect(client, NULL);

  PROACTOR_TEST_DRAIN(pts);     /* Drain will  */
  PROACTOR_TEST_FREE(pts);
}

int main(int argc, char **argv) {
  int failed = 0;
  last_condition = pn_condition();
  RUN_ARGV_TEST(failed, t, test_inactive(&t));
  RUN_ARGV_TEST(failed, t, test_interrupt_timeout(&t));
  RUN_ARGV_TEST(failed, t, test_errors(&t));
  RUN_ARGV_TEST(failed, t, test_client_server(&t));
  RUN_ARGV_TEST(failed, t, test_connection_wake(&t));
  RUN_ARGV_TEST(failed, t, test_ipv4_ipv6(&t));
  RUN_ARGV_TEST(failed, t, test_release_free(&t));
  RUN_ARGV_TEST(failed, t, test_ssl(&t));
  RUN_ARGV_TEST(failed, t, test_proactor_addr(&t));
  RUN_ARGV_TEST(failed, t, test_netaddr(&t));
  RUN_ARGV_TEST(failed, t, test_disconnect(&t));
  RUN_ARGV_TEST(failed, t, test_abort(&t));
  RUN_ARGV_TEST(failed, t, test_refuse(&t));
  pn_condition_free(last_condition);
  return failed;
}
