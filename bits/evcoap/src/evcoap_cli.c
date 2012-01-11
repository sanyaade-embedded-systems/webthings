#include <event2/event.h>
#include <event2/util.h>
#include <u/libu.h>
#include "evcoap_cli.h"
#include "evcoap_base.h"
#include "evcoap_debug.h"

#define NO_STRING(s)  ((s) == NULL || *(s) == '\0')

static int ec_client_check_transition(ec_cli_state_t cur, ec_cli_state_t next);
static bool ec_client_state_is_final(ec_cli_state_t state);
static int ec_client_handle_pdu(ev_uint8_t *raw, size_t raw_sz, int sd,
        struct sockaddr_storage *peer, ev_socklen_t peer_len, void *arg);
static void ec_cli_app_timeout(evutil_socket_t u0, short u1, void *c);
static void ec_client_dns_cb(int result, struct evutil_addrinfo *res, void *a);
static int ec_client_check_req_token(ec_client_t *cli);
static int ec_client_invoke_user_callback(ec_client_t *cli);

int ec_client_set_method(ec_client_t *cli, ec_method_t m)
{
    ec_flow_t *flow = &cli->flow;

    dbg_return_if (m < EC_GET || m > EC_DELETE, -1);

    flow->method = m;

    return 0;
}

int ec_client_set_proxy(ec_client_t *cli, const char *host, ev_uint16_t port)
{
    ec_conn_t *conn = &cli->flow.conn;

    dbg_return_if (NO_STRING(host), -1);

    conn->proxy_port = (port == 0) ? EC_COAP_DEFAULT_PORT : port;

    dbg_err_if (u_strlcpy(conn->proxy_addr, host, sizeof conn->proxy_addr));

    conn->use_proxy = 1;

    return 0;
err:
    return -1;
}

int ec_client_set_uri(ec_client_t *cli, const char *uri)
{
    ec_opts_t *opts;
    ec_conn_t *conn;
    const char *scheme, *host, *p;
    u_uri_t *u = NULL;

    opts = &cli->req.opts;
    conn = &cli->flow.conn;

    /* Do minimal URI validation: parse it according to STD 66 + expect
     * at least non empty scheme and host. */
    dbg_err_if (u_uri_crumble(uri, 0, &u));
    dbg_err_if ((scheme = u_uri_get_scheme(u)) == NULL || *scheme == '\0');
    dbg_err_if ((host = u_uri_get_host(u)) == NULL || *host == '\0');

    /* Set options. */
    if (conn->use_proxy)
        dbg_err_if (ec_opts_add_proxy_uri(opts, uri));
    else
    {
        /* Expect scheme==coap for any non proxy request. */
        if (strcasecmp(scheme, "coap"))
            dbg_err("expect URI with coap scheme on non-proxy requests");

        dbg_err_if (ec_opts_add_uri_host(opts, host));

        if ((p = u_uri_get_port(u)) && *p != '\0')
        {
            int port;

            dbg_err_if (u_atoi(p, &port));
            dbg_err_if (ec_opts_add_uri_port(opts, (ev_uint16_t) port));
        }

        /* Separate path components. */
        if ((p = u_uri_get_path(u)) && *p != '\0')
        {
            char *r, *s, path[1024];    /* TODO check path len. */

            dbg_err_if (u_strlcpy(path, p, sizeof path));

            for (s = path; (r = strsep(&s, "/")) != NULL; )
            {
                if (*r == '\0')
                    continue;

                dbg_err_if (ec_opts_add_uri_path(opts, r));
            }
        }

        /* Add query, if available. */
        if ((p = u_uri_get_query(u)) && *p != '\0')
            dbg_err_if (ec_opts_add_uri_query(opts, p));
    }

    return 0;
err:
    return -1;
}

void ec_client_free(ec_client_t *cli)
{
    if (cli)
    {
        ec_flow_t *flow = &cli->flow;
        ec_conn_t *conn = &flow->conn;

        /* Close socket. */
        evutil_closesocket(conn->socket);

        /* Free URI. */
        u_uri_free(flow->uri);

        /* TODO free cli->req */
        /* TODO free cli->res */
        /* TODO Destroy any associated timer ? */

        u_free(cli);
    }

    return;
}

int ec_client_set_msg_model(ec_client_t *cli, bool is_con)
{
    cli->flow.conn.is_confirmable = is_con;
    return 0;
}

ec_client_t *ec_client_new(struct ec_s *coap, ec_method_t m, const char *uri, 
        ec_msg_model_t mm, const char *proxy_host, ev_uint16_t proxy_port)
{
    ec_client_t *cli = NULL;

    dbg_return_if (coap == NULL, NULL);
    /* Assume all other input parameters are checked by called functions. */

    dbg_err_sif ((cli = u_zalloc(sizeof *cli)) == NULL);

    /* Must be done first because the following URI validation (namely the
     * scheme compliance test) depends on the fact that this request is 
     * expected to go through a proxy or not. */
    if (proxy_host)
        dbg_err_if (ec_client_set_proxy(cli, proxy_host, proxy_port));

    dbg_err_if (ec_pdu_init_options(&cli->req));
    dbg_err_if (ec_client_set_method(cli, m));
    dbg_err_if (ec_client_set_uri(cli, uri));
    dbg_err_if (ec_client_set_msg_model(cli, mm == EC_CON ? true : false));
    dbg_err_if (ec_pdu_set_flow(&cli->req, &cli->flow));

    /* Cache the base so that we don't need to pass it around every function
     * that manipulates the transaction. */
    cli->base = coap;

    return cli;
err:
    if (cli)
        ec_client_free(cli);
    return NULL;
}

int ec_client_go(ec_client_t *cli, ec_client_cb_t cb, void *cb_args,
        struct timeval *tout)
{
    ec_pdu_t *req;
    ec_flow_t *flow;
    ec_conn_t *conn;
    ec_cli_timers_t *timers;
    ec_opt_t *pu = NULL;
    const char *host;
    ev_uint16_t port;
    char sport[16];
    struct evutil_addrinfo hints;
    struct timeval app_tout_dflt = { 
        .tv_sec = EC_TIMERS_APP_TOUT, 
        .tv_usec = 0
    };

    dbg_return_if (cli == NULL, -1);

    /* Expect client with state NONE.  Otherwise we jump to err where the 
     * state is set to INTERNAL_ERR. */
    dbg_err_ifm (cli->state != EC_CLI_STATE_NONE,
            "unexpected state %u", cli->state);

    req = &cli->req;
    flow = &cli->flow;
    timers = &cli->timers;
    conn = &flow->conn;

    /* TODO Sanitize request. */ 

    /* Add a Token option, if missing. */
    dbg_err_if (ec_client_check_req_token(cli));

    /* Get destination for this flow. */
    if (conn->use_proxy)
    {
        /* Use user supplied proxy host and port (assume previous sanitization
         * done by ec_client_set_proxy(). */
        host = conn->proxy_addr;
        port = conn->proxy_port;
    }
    else
    {
        /* Use Uri-Host + optional Uri-Port */
        dbg_err_if ((host = ec_opts_get_uri_host(&req->opts)) == NULL);

        if (ec_opts_get_uri_port(&req->opts, &port))
            port = EC_COAP_DEFAULT_PORT;
    }

    dbg_err_if (u_snprintf(sport, sizeof sport, "%u", port));

    /* Set user defined callback. */
    cli->cb = cb;
    cli->cb_args = cb_args;

    /* Set application timeout. */
    timers->app_tout = tout ? *tout : app_tout_dflt;

    /* Set up hints needed by evdns_getaddrinfo(). */
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = 0;

    /* Pass the ball to evdns.  In case the evdns resolved immediately,
     * we return the send operation status hold by pdu->state.
     * Otherwise return ok and let the status of the send operation be 
     * given back to the user supplied callback.
     * Save the evdns_getaddrinfo_request pointer (may be NULL in case
     * of immediate resolution) so that the request can be canceled 
     * in a later moment if needed. */
    cli->dns_req = evdns_getaddrinfo(cli->base->dns, host, sport, &hints, 
            ec_client_dns_cb, cli);

    /* If we get here, either the client FSM has reached a final state (since
     * the callback has been shortcircuited), or the it's not yet started. */
    return 0;
err:
    ec_client_set_state(cli, EC_CLI_STATE_INTERNAL_ERR);
    return -1;
}

static void ec_client_dns_cb(int result, struct evutil_addrinfo *res, void *a)
{
#define EC_CLI_ASSERT(e, state)                 \
    do {                                        \
        if ((e))                                \
        {                                       \
            ec_client_set_state(cli, state);    \
            goto err;                           \
        }                                       \
    } while (0)

    struct evutil_addrinfo *ai;
    ec_client_t *cli = (ec_client_t *) a;
    ec_pdu_t *req = &cli->req;
    ec_conn_t *conn = &cli->flow.conn;

    /* Unset the evdns_getaddrinfo_request pointer, since when we get called
     * its lifetime is complete. */
    cli->dns_req = NULL;

    EC_CLI_ASSERT(result != DNS_ERR_NONE, EC_CLI_STATE_DNS_FAILED);

    ec_client_set_state(cli, EC_CLI_STATE_DNS_OK);

    /* Encode options and header. */
    EC_CLI_ASSERT(ec_pdu_encode(req), EC_CLI_STATE_INTERNAL_ERR);

    for (conn->socket = -1, ai = res; ai != NULL; ai = ai->ai_next)
    {
        conn->socket = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (conn->socket == -1)
           continue;

        /* Send the request PDU. */
        if (ec_pdu_send(req, (struct sockaddr_storage *) ai->ai_addr,
                    ai->ai_addrlen))
        {
            /* Mark this socket as failed and try again. */
            evutil_closesocket(conn->socket), conn->socket = -1;
            continue;
        }

        EC_CLI_ASSERT(evutil_make_socket_nonblocking(conn->socket),
                EC_CLI_STATE_INTERNAL_ERR);

        ec_client_set_state(cli, EC_CLI_STATE_REQ_SENT);
        break;
    }

    /* Check whether the request PDU was actually sent out on any socket. */
    EC_CLI_ASSERT(conn->socket == -1, EC_CLI_STATE_INTERNAL_ERR);

    /* Add this to the pending clients' list. */
    EC_CLI_ASSERT(ec_client_register(cli), EC_CLI_STATE_INTERNAL_ERR);

    /* TODO add to the duplicate machinery ? */

    return;
err:
    return;
    /* TODO Invoke user callback with the failure code. */
#undef EC_CLI_ASSERT
}

static int ec_client_check_transition(ec_cli_state_t cur, ec_cli_state_t next)
{
    switch (next)
    {
        /* Any state can switch to INTERNAL_ERROR. */
        case EC_CLI_STATE_INTERNAL_ERR:
            break;

        case EC_CLI_STATE_DNS_FAILED:
        case EC_CLI_STATE_DNS_OK:
            dbg_err_if (cur != EC_CLI_STATE_NONE);
            break;

        case EC_CLI_STATE_SEND_FAILED:
        case EC_CLI_STATE_REQ_SENT:
            dbg_err_if (cur != EC_CLI_STATE_DNS_OK
                    && cur != EC_CLI_STATE_COAP_RETRY);
            break;

        case EC_CLI_STATE_REQ_ACKD:
        case EC_CLI_STATE_COAP_RETRY:
        case EC_CLI_STATE_COAP_TIMEOUT:
            dbg_err_if (cur != EC_CLI_STATE_REQ_SENT);
            break;

        case EC_CLI_STATE_APP_TIMEOUT:
        case EC_CLI_STATE_REQ_DONE:
        case EC_CLI_STATE_REQ_RST:
            dbg_err_if (cur != EC_CLI_STATE_REQ_SENT
                    && cur != EC_CLI_STATE_REQ_ACKD);
            break;

        case EC_CLI_STATE_NONE:
        default:
            goto err;
    }

    return 0;
err:
    u_warn("invalid transition from '%s' to '%s'", ec_cli_state_str(cur),
            ec_cli_state_str(next));
    return -1;
}

static bool ec_client_state_is_final(ec_cli_state_t state)
{
    switch (state)
    {
        case EC_CLI_STATE_INTERNAL_ERR:
        case EC_CLI_STATE_DNS_FAILED:
        case EC_CLI_STATE_SEND_FAILED:
        case EC_CLI_STATE_COAP_TIMEOUT:
        case EC_CLI_STATE_APP_TIMEOUT:
        case EC_CLI_STATE_REQ_DONE:
        case EC_CLI_STATE_REQ_RST:
            return true;
        case EC_CLI_STATE_NONE:
        case EC_CLI_STATE_DNS_OK:
        case EC_CLI_STATE_REQ_SENT:
        case EC_CLI_STATE_REQ_ACKD:
        case EC_CLI_STATE_COAP_RETRY:
            return false;
        default:
            die(EXIT_FAILURE, "%s: no such state %u", __func__, state);
    }
}

static void ec_cli_app_timeout(evutil_socket_t u0, short u1, void *c)
{
    ec_client_t *cli = (ec_client_t *) c;

    /* Set state to APP_TIMEOUT. */
    ec_client_set_state(cli, EC_CLI_STATE_APP_TIMEOUT);

    /* Invoke client callback. */
    (void) ec_client_invoke_user_callback(cli);

    return;
}

int ec_cli_start_app_timer(ec_client_t *cli)
{
    dbg_return_if (cli == NULL, -1);

    ec_t *coap = cli->base;
    ec_cli_timers_t *t = &cli->timers;

    /* It is expected that the application timeout is set only once for each
     * client. */
    dbg_err_if (t->app != NULL);

    t->app = event_new(coap->base, -1, EV_PERSIST, ec_cli_app_timeout, cli);
    dbg_err_if (t->app == NULL || evtimer_add(t->app, &t->app_tout));

    return 0;
err:
    if (t->app)
        event_free(t->app);
    return -1;
}

int ec_cli_stop_app_timer(ec_client_t *cli)
{
    dbg_return_if (cli == NULL, -1);

    ec_cli_timers_t *t = &cli->timers;

    if (t->app)
    {
        event_free(t->app);
        t->app = NULL;
    }

    return 0;
}

void ec_client_set_state(ec_client_t *cli, ec_cli_state_t state)
{
    ec_cli_state_t cur = cli->state;

    u_dbg("[client=%p] transition request from '%s' to '%s'", cli, 
            ec_cli_state_str(cur), ec_cli_state_str(state));

    /* Check that the requested state transition is valid. */
    dbg_err_if (ec_client_check_transition(cur, state));

    if (state == EC_CLI_STATE_REQ_SENT)
    {
        /* After the _first_ successful send, start the application timeout. */
        if (cur == EC_CLI_STATE_DNS_OK)
            dbg_err_if (ec_cli_start_app_timer(cli));

        /* TODO if CON also start the retransmission timer. */
    }
    else if (ec_client_state_is_final(state))
    {
        /* Any final state MUST destroy all pending timers. */

        dbg_err_if (ec_cli_stop_app_timer(cli));

        /* TODO if CON also stop the retransmission timer. */
    }

    /* Finally set state. */
    cli->state = state;

    return;
err:
    /* Should never happen ! */
    die(EXIT_FAILURE, "%s failed (see logs)", __func__);
}

int ec_client_register(ec_client_t *cli)
{
    ec_t *coap;
    ec_conn_t *conn;
    struct event *ev_input = NULL;

    dbg_return_if (cli == NULL, -1);
        
    coap = cli->base;
    conn = &cli->flow.conn;

    /* Attach server response events to this socket. */
    dbg_err_if ((ev_input = event_new(coap->base, conn->socket, 
                    EV_READ | EV_PERSIST, ec_client_input, cli)) == NULL);

    /* Make the read event pending in the base. */
    dbg_err_if (event_add(ev_input, NULL) == -1);
    
    /* Attach input event on this socket. */
    conn->ev_input = ev_input, ev_input = NULL;

    TAILQ_INSERT_HEAD(&coap->clients, cli, next);

    return 0;
err:
    if (ev_input)
        event_del(ev_input);
    return -1;
}

/* XXX bad function name, could lead to confusion. */
struct ec_s *ec_client_get_base(ec_client_t *cli)
{
    dbg_return_if (cli == NULL, NULL);

    return cli->base;
}

ec_cli_state_t ec_client_get_state(ec_client_t *cli)
{
    /* We don't have any meaningful return code to return in case a NULL
     * client was supplied.  Trace this into the log just before we core 
     * dump on subsequent dereference attempt. */
    dbg_if (cli == NULL);

    return cli->state;
}

static int ec_client_handle_pdu(ev_uint8_t *raw, size_t raw_sz, int sd,
        struct sockaddr_storage *peer, ev_socklen_t peer_len, void *arg)
{
    ec_opt_t *t;
    ec_client_t *cli;
    size_t olen = 0, plen;
    ec_pdu_t *res = NULL;

    dbg_return_if ((cli = (ec_client_t *) arg) == NULL, -1);

    /* Make room for the new PDU. */
    dbg_err_sif ((res = ec_pdu_new_empty()) == NULL);

    /* Decode CoAP header and save it into the client context. */
    dbg_err_if (ec_pdu_decode_header(res, raw, raw_sz));

    ec_hdr_t *h = &res->hdr_bits;   /* shortcut */
    ec_flow_t *flow = &cli->flow;   /* shortcut */

    dbg_err_ifm (h->code >= 1 && h->code <= 31, 
            "unexpected request code in client response context");

    /* Pass MID and peer address to the dup handler machinery. */
    ec_dups_t *dups = &cli->base->dups;

    /* See return codes of evcoap_base.c:ec_dups_handle_incoming_res().
     *
     * TODO Keep an eye here, if we can factor out code in common with 
     * TODO ec_server_handle_pdu(). */
    switch (ec_dups_handle_incoming_srvmsg(dups, h->mid, sd, peer, peer_len))
    {
        case 0:
            /* Not a duplicate, proceed with normal processing. */
            break;
        case 1:
            /* Duplicate, possible resending of the paired message is handled 
             * by ec_dups_handle_incoming_srvmsg(). */
            return 0;
        default:
            /* Internal error. */
            u_dbg("Duplicate handling machinery failed !");
            goto err;
    }
    
    /* Handle empty responses (i.e. resets and separated acknowledgements)
     * specially. */
    if (!h->code)
        return ec_client_handle_empty_pdu(cli, h->t, h->mid);

    /* Parse options.  At least one option (namely the Token) must be present
     * because evcoap always sends one non-empty Token to its clients. */
    dbg_err_ifm (!h->oc, "no options in response !");
    dbg_err_ifm (ec_opts_decode(&res->opts, raw, raw_sz, h->oc, &olen),
            "CoAP options could not be parsed correctly");

    /* Check that there is a token and it matches the one we sent out with the 
     * request. */
    dbg_err_if ((t = ec_opts_get(&res->opts, EC_OPT_TOKEN)) == NULL);
    dbg_err_if (t->l != flow->token_sz || memcmp(t->v, flow->token, t->l));

    /* Attach payload, if any. */
    if ((plen = raw_sz - (olen + EC_COAP_HDR_SIZE)))
        (void) ec_pdu_set_payload(res, raw + EC_COAP_HDR_SIZE + olen, plen);

    /* TODO fill in the residual info (e.g. socket...). */

    /* Just before invoking the client callback, set state to DONE. */
    ec_client_set_state(cli, EC_CLI_STATE_REQ_DONE);

    /* Invoke the client callback */
    (void) ec_client_invoke_user_callback(cli);

    return 0;
err:
    return -1;
}

static int ec_client_invoke_user_callback(ec_client_t *cli)
{
    dbg_return_if (cli == NULL, -1);

    if (cli->cb)
        cli->cb(cli);
    else
    {
        /* TODO respond something standard in case there's no callback. */
    }

    return 0;
}

int ec_client_handle_empty_pdu(ec_client_t *cli, ev_uint8_t t, ev_uint16_t mid)
{
    /* TODO */
    return 0;
}

/* Just a wrapper around ec_net_pullup_all(). */
void ec_client_input(evutil_socket_t sd, short u, void *arg)
{
    ec_client_t *cli = (ec_client_t *) arg;

    u_unused_args(u);

    ec_net_pullup_all(sd, ec_client_handle_pdu, cli);
}

void *ec_client_get_args(ec_client_t *cli)
{
    dbg_return_if (cli == NULL, NULL);

    return cli->cb_args;
}

static int ec_client_check_req_token(ec_client_t *cli)
{
    ec_opt_t *t;
    ec_flow_t *flow;
    ec_pdu_t *req;
    ev_uint8_t tok[8];
    const size_t tok_sz = sizeof tok;

    dbg_return_if (cli == NULL, -1);

    req = &cli->req;    /* shortcut */
    flow = &cli->flow;  /* ditto */

    if ((t = ec_opts_get(&req->opts, EC_OPT_TOKEN)) == NULL)
    {
        evutil_secure_rng_get_bytes(tok, tok_sz);
        dbg_err_if (ec_opts_add_token(&req->opts, tok, tok_sz));
    }

    /* Cache the token value into the flow. */
    dbg_err_if (ec_flow_save_token(flow, t ? t->v : tok, t ? t->l : tok_sz));
 
    return 0;
err:
    return -1;
}


