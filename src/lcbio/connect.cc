/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"
#include "connect.h"
#include "ioutils.h"
#include "iotable.h"
#include "settings.h"
#include "timer-ng.h"
#include "timer-cxx.h"
#include <errno.h>

using namespace lcb::io;

/* win32 lacks EAI_SYSTEM */
#ifndef EAI_SYSTEM
#define EAI_SYSTEM 0
#endif
#define LOGARGS(conn, lvl) conn->settings, "connection", LCB_LOG_##lvl, __FILE__, __LINE__
static const lcb_host_t *get_loghost(lcbio_SOCKET *s) {
    static lcb_host_t host = { "NOHOST", "NOPORT" };
    if (!s) { return &host; }
    if (!s->info) { return &host; }
    return &s->info->ep;
}

/** Format string arguments for %p%s:%s */
#define CSLOGID(sock) get_loghost(sock)->host, get_loghost(sock)->port, (void*)sock
#define CSLOGFMT "<%s:%s> (SOCK=%p) "

namespace lcb {
namespace io {
struct Connstart {
    Connstart(lcbio_TABLE*, lcb_settings*, const lcb_host_t*,
              uint32_t, lcbio_CONNDONE_cb, void*);

    ~Connstart();
    void unwatch();
    void handler();
    void cancel();
    void C_connect();

    enum State {
        CS_PENDING, CS_CANCELLED, CS_CONNECTED, CS_ERROR
    };

    void state_signal(State next_state, lcb_error_t status);
    void notify_success();
    void notify_error(lcb_error_t err);
    bool ensure_sock();
    void clear_sock();

    lcbio_CONNDONE_cb user_handler;
    void *user_arg;

    lcbio_SOCKET *sock;
    lcbio_OSERR syserr;
    void *event;
    bool ev_active; /* whether the event pointer is active (Event only) */
    bool in_uhandler; /* Whether we're inside the user-defined handler */
    addrinfo *ai_root;
    addrinfo *ai;
    State state;
    lcb_error_t last_error;
    Timer<Connstart, &Connstart::handler> timer;
};
}
}

void Connstart::unwatch() {
    if (sock && ev_active) {
        lcb_assert(sock->u.fd != INVALID_SOCKET);
        IOT_V0EV(sock->io).cancel(IOT_ARG(sock->io), sock->u.fd, event);
        ev_active = false;
    }
}

/**
 * Handler invoked to deliver final status for a connection. This will invoke
 * the user supplied callback with the relevant status (if it has not been
 * cancelled) and then free the CONNSTART object.
 */
void Connstart::handler() {
    lcb_error_t err;

    if (sock && event) {
        unwatch();
        IOT_V0EV(sock->io).destroy(IOT_ARG(sock->io), event);
    }

    if (state == CS_PENDING) {
        /* state was not changed since initial scheduling */
        err = LCB_ETIMEDOUT;
    } else if (state == CS_CONNECTED) {
        /* clear pending error */
        err = LCB_SUCCESS;
    } else {
        if (sock != NULL && last_error == LCB_CONNECT_ERROR) {
            err = lcbio_mklcberr(syserr, sock->settings);
        } else {
            err = last_error;
        }
    }

    if (state == CS_CANCELLED) {
        /* ignore everything. Clean up resources */
        goto GT_DTOR;
    }

    if (sock) {
        lcbio__load_socknames(sock);
        if (err == LCB_SUCCESS) {
            lcb_log(LOGARGS(sock, INFO), CSLOGFMT "Connected established", CSLOGID(sock));

            if (sock->settings->tcp_nodelay) {
                lcb_error_t ndrc = lcbio_disable_nagle(sock);
                if (ndrc != LCB_SUCCESS) {
                    lcb_log(LOGARGS(sock, INFO), CSLOGFMT "Couldn't set TCP_NODELAY", CSLOGID(sock));
                } else {
                    lcb_log(LOGARGS(sock, DEBUG), CSLOGFMT "Successfuly set TCP_NODELAY", CSLOGID(sock));
                }
            }
        } else {
            lcb_log(LOGARGS(sock, ERR), CSLOGFMT "Failed to establish connection: %s, os errno=%u", CSLOGID(sock), lcb_strerror_short(err), syserr);
        }
    }

    /** Handler section */
    in_uhandler = true;
    user_handler(err == LCB_SUCCESS ? sock : NULL, user_arg, err, syserr);
    in_uhandler = false;

    GT_DTOR:
    delete this;
}

Connstart::~Connstart() {
    timer.release();
    if (sock) {
        lcbio_unref(sock);
    }
    if (ai_root) {
        freeaddrinfo(ai_root);
    }
}

void Connstart::state_signal(State next_state, lcb_error_t err) {
    if (state != CS_PENDING) {
        /** State already set */
        return;
    }


    if (state == CS_CONNECTED) {
        /* clear last errors if we're successful */
        last_error = LCB_SUCCESS;
    } else if (last_error == LCB_SUCCESS) {
        /* set error code only if previous code was not a failure */
        last_error = err;
    }

    state = next_state;
    timer.signal();
}

void Connstart::notify_success() {
    state_signal(CS_CONNECTED, LCB_SUCCESS);
}

void Connstart::notify_error(lcb_error_t err) {
    state_signal(CS_ERROR, err);
}

/** Cancels and mutes any pending event */
void lcbio_connect_cancel(lcbio_pCONNSTART cs) {
    cs->cancel();
}

void Connstart::cancel() {
    if (in_uhandler) {
        /* already inside user-defined handler */
        return;
    }
    state = CS_CANCELLED;
    handler();
}


bool Connstart::ensure_sock() {
    lcbio_TABLE *io = sock->io;
    int errtmp = 0;

    if (ai == NULL) {
        return false;
    }

    if (IOT_IS_EVENT(io)) {
        if (sock->u.fd != INVALID_SOCKET) {
            /* already have one? */
            return true;
        }

        while (sock->u.fd == INVALID_SOCKET && ai != NULL) {
            sock->u.fd = lcbio_E_ai2sock(io, &ai, &errtmp);
            if (sock->u.fd != INVALID_SOCKET) {
                lcb_log(LOGARGS(sock, DEBUG), CSLOGFMT "Created new socket with FD=%d", CSLOGID(sock), sock->u.fd);
                return true;
            }
        }
    } else {
        if (sock->u.sd) {
            return true;
        }

        while (sock->u.sd == NULL && ai != NULL) {
            sock->u.sd = lcbio_C_ai2sock(io, &ai, &errtmp);
            if (sock->u.sd) {
                sock->u.sd->lcbconn = const_cast<lcbio_SOCKET*>(sock);
                sock->u.sd->parent = IOT_ARG(io);
                return true;
            }
        }
    }

    if (ai == NULL) {
        lcbio_mksyserr(IOT_ERRNO(io), &syserr);
        return false;
    }
    return true;
}

void Connstart::clear_sock() {
    lcbio_TABLE *iot = sock->io;
    if (ai) {
        ai = ai->ai_next;
    }

    if (!ai) {
        return;
    }

    if (IOT_IS_EVENT(iot)) {
        unwatch();
        IOT_V0IO(iot).close(IOT_ARG(iot), sock->u.fd);
        sock->u.fd = INVALID_SOCKET;
    } else {
        if (sock->u.sd) {
            IOT_V1(iot).close(IOT_ARG(iot), sock->u.sd);
            sock->u.sd = NULL;
        }
    }
}

static void
E_conncb(lcb_socket_t, short events, void *arg)
{
    Connstart *cs = reinterpret_cast<Connstart*>(arg);
    lcbio_SOCKET *s = cs->sock;
    lcbio_TABLE *io = s->io;
    int retry_once = 0;
    lcbio_CSERR connstatus;
    int rv = 0;
    addrinfo *ai = NULL;

    GT_NEXTSOCK:
    if (!cs->ensure_sock()) {
        cs->notify_error(LCB_CONNECT_ERROR);
        return;
    }

    if (events & LCB_ERROR_EVENT) {
        socklen_t errlen = sizeof(int);
        int sockerr = 0;
        lcb_log(LOGARGS(s, TRACE), CSLOGFMT "Received ERROR_EVENT", CSLOGID(s));
        getsockopt(s->u.fd, SOL_SOCKET, SO_ERROR, (char *)&sockerr, &errlen);
        lcbio_mksyserr(sockerr, &cs->syserr);
        cs->clear_sock();
        goto GT_NEXTSOCK;

    } else {
        rv = 0;
        ai = cs->ai;

        GT_CONNECT:
        rv = IOT_V0IO(io).connect0(
                IOT_ARG(io), s->u.fd, ai->ai_addr, (unsigned)ai->ai_addrlen);

        if (rv == 0) {
            cs->unwatch();
            cs->notify_success();
            return;
        }
    }

    connstatus = lcbio_mkcserr(IOT_ERRNO(io));
    lcbio_mksyserr(IOT_ERRNO(io), &cs->syserr);


    switch (connstatus) {

    case LCBIO_CSERR_INTR:
        goto GT_CONNECT;

    case LCBIO_CSERR_CONNECTED:
        cs->unwatch();
        cs->notify_success();
        return;

    case LCBIO_CSERR_BUSY:
        lcb_log(LOGARGS(s, TRACE), CSLOGFMT "Scheduling I/O watcher for asynchronous connection completion.", CSLOGID(s));
        IOT_V0EV(io).watch(
                IOT_ARG(io), s->u.fd, cs->event, LCB_WRITE_EVENT, cs, E_conncb);
        cs->ev_active = 1;
        return;

    case LCBIO_CSERR_EINVAL:
        if (!retry_once) {
            retry_once = 1;
            goto GT_CONNECT;
        }
        /* fallthrough */

    case LCBIO_CSERR_EFAIL:
    default:
        /* close the current socket and try again */
        lcb_log(LOGARGS(s, TRACE), CSLOGFMT "connect() failed. errno=%d [%s]", CSLOGID(s), IOT_ERRNO(io), strerror(IOT_ERRNO(io)));
        cs->clear_sock();
        goto GT_NEXTSOCK;
    }
}


static void
C_conncb(lcb_sockdata_t *sock, int status)
{
    lcbio_SOCKET *s = reinterpret_cast<lcbio_SOCKET*>(sock->lcbconn);
    Connstart *cs = reinterpret_cast<Connstart*>(s->ctx);

    lcb_log(LOGARGS(s, TRACE), CSLOGFMT "Received completion handler. Status=%d. errno=%d", CSLOGID(s), status, IOT_ERRNO(s->io));

    if (!--s->refcount) {
        lcbio__destroy(s);
        return;
    }

    if (!status) {
        if (cs->state == Connstart::CS_PENDING) {
            cs->state = Connstart::CS_CONNECTED;
        }
        cs->handler();
    } else {
        lcbio_mksyserr(IOT_ERRNO(s->io), &cs->syserr);
        cs->clear_sock();
        cs->C_connect();
    }
}

void Connstart::C_connect()
{
    int rv;
    bool retry_once = 0;
    lcbio_CSERR status;
    lcbio_TABLE *io = sock->io;

    GT_NEXTSOCK:
    if (!ensure_sock()) {
        lcbio_mksyserr(IOT_ERRNO(io), &syserr);
        notify_error(LCB_CONNECT_ERROR);
        return;
    }

    GT_CONNECT:
    rv = IOT_V1(io).connect(IOT_ARG(io), sock->u.sd, ai->ai_addr,
                            (unsigned)ai->ai_addrlen, C_conncb);
    if (rv == 0) {
        lcbio_ref(sock);
        return;
    }

    lcbio_mksyserr(IOT_ERRNO(io), &syserr);
    status = lcbio_mkcserr(IOT_ERRNO(io));
    switch (status) {

    case LCBIO_CSERR_INTR:
        goto GT_CONNECT;

    case LCBIO_CSERR_CONNECTED:
        notify_success();
        return;

    case LCBIO_CSERR_BUSY:
        return;

    case LCBIO_CSERR_EINVAL:
        if (!retry_once) {
            retry_once = 1;
            goto GT_CONNECT;
        }
        /* fallthrough */

    case LCBIO_CSERR_EFAIL:
    default:
        clear_sock();
        goto GT_NEXTSOCK;
    }
}

Connstart *
lcbio_connect(lcbio_TABLE *iot, lcb_settings *settings, const lcb_host_t *dest,
              uint32_t timeout, lcbio_CONNDONE_cb handler, void *arg)
{
    return new Connstart(iot, settings, dest, timeout, handler, arg);
}

Connstart::Connstart(lcbio_TABLE* iot_, lcb_settings* settings_,
                    const lcb_host_t *dest, uint32_t timeout,
                    lcbio_CONNDONE_cb handler, void *arg)
    : user_handler(handler), user_arg(arg), sock(NULL), syserr(0),
      event(NULL), ev_active(false), in_uhandler(false),
      ai_root(NULL), ai(NULL), state(CS_PENDING), last_error(LCB_SUCCESS),
      timer(iot_, this) {

    addrinfo hints;
    int rv;

    sock = reinterpret_cast<lcbio_SOCKET*>(calloc(1, sizeof(*sock)));

    /** Initialize the socket first */
    sock->io = iot_;
    sock->settings = settings_;
    sock->ctx = this;
    sock->refcount = 1;
    sock->info = reinterpret_cast<lcbio_CONNINFO*>(calloc(1, sizeof(*sock->info)));
    sock->info->ep = *dest;
    lcbio_table_ref(sock->io);
    lcb_settings_ref(sock->settings);
    lcb_list_init(&sock->protos);

    if (IOT_IS_EVENT(iot_)) {
        sock->u.fd = INVALID_SOCKET;
        event = IOT_V0EV(iot_).create(IOT_ARG(iot_));
    }

    timer.rearm(timeout);
    lcb_log(LOGARGS(sock, INFO), CSLOGFMT "Starting. Timeout=%uus", CSLOGID(sock), timeout);

    /** Hostname lookup: */
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    if (settings_->ipv6 == LCB_IPV6_DISABLED) {
        hints.ai_family = AF_INET;
    } else if (settings_->ipv6 == LCB_IPV6_ONLY) {
        hints.ai_family = AF_INET6;
    } else {
        hints.ai_family = AF_UNSPEC;
    }

    if ((rv = getaddrinfo(dest->host, dest->port, &hints, &ai_root))) {
        const char *errstr = rv != EAI_SYSTEM ? gai_strerror(rv) : "";
        lcb_log(LOGARGS(sock, ERR), CSLOGFMT "Couldn't look up %s (%s) [EAI=%d]", CSLOGID(sock), dest->host, errstr, rv);
        notify_error(LCB_UNKNOWN_HOST);
    } else {
        ai = ai_root;

        /** Figure out how to connect */
        if (IOT_IS_EVENT(iot_)) {
            E_conncb(-1, LCB_WRITE_EVENT, this);
        } else {
            C_connect();
        }
    }
}

Connstart *
lcbio_connect_hl(lcbio_TABLE *iot, lcb_settings *settings,
                 hostlist_t hl, int rollover, uint32_t timeout,
                 lcbio_CONNDONE_cb handler, void *arg)
{
    const lcb_host_t *cur;
    unsigned ii, hlmax;
    ii = 0;
    hlmax = hostlist_size(hl);

    while ( (cur = hostlist_shift_next(hl, rollover)) && ii++ < hlmax) {
        Connstart *ret = lcbio_connect(
                iot, settings, cur, timeout, handler, arg);
        if (ret) {
            return ret;
        }
    }

    return NULL;
}

lcbio_SOCKET *
lcbio_wrap_fd(lcbio_pTABLE iot, lcb_settings *settings, lcb_socket_t fd)
{
    lcbio_SOCKET *ret = reinterpret_cast<lcbio_SOCKET*>(calloc(1, sizeof(*ret)));
    lcbio_CONNDONE_cb *ci = reinterpret_cast<lcbio_CONNDONE_cb*>(calloc(1, sizeof(*ci)));

    if (ret == NULL || ci == NULL) {
        free(ret);
        free(ci);
        return NULL;
    }

    assert(iot->model = LCB_IOMODEL_EVENT);

    lcb_list_init(&ret->protos);
    ret->settings = settings;
    ret->io = iot;
    ret->refcount = 1;
    ret->u.fd = fd;

    lcbio_table_ref(ret->io);
    lcb_settings_ref(ret->settings);
    lcbio__load_socknames(ret);
    return ret;
}

void
lcbio_shutdown(lcbio_SOCKET *s)
{
    lcbio_TABLE *io = s->io;

    lcbio__protoctx_delall(s);
    if (IOT_IS_EVENT(io)) {
        if (s->u.fd != INVALID_SOCKET) {
            IOT_V0IO(io).close(IOT_ARG(io), s->u.fd);
            s->u.fd = INVALID_SOCKET;
        }
    } else {
        if (s->u.sd) {
            IOT_V1(io).close(IOT_ARG(io), s->u.sd);
            s->u.sd = NULL;
        }
    }
}

void
lcbio__destroy(lcbio_SOCKET *s)
{
    lcbio_shutdown(s);
    if (s->info) {
        free(s->info);
    }
    lcbio_table_unref(s->io);
    lcb_settings_unref(s->settings);
    free(s);
}