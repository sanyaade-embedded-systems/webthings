#include "evcoap.h"
#include "evcoap_base.h"
#include "evcoap_observe.h"

/* ec_observer_t private interfaces. */
static ec_observer_t *ec_observer_new(const uint8_t *token, size_t token_sz, 
        const uint8_t *etag, size_t etag_sz, ec_mt_t media_type, 
        ec_msg_model_t msg_model, const ec_conn_t *conn, ec_observe_cb_t cb, 
        void *args);
static int ec_observer_add(ec_observation_t *obs, const uint8_t *tok,
        size_t tok_sz, const uint8_t *etag, size_t etag_sz, ec_mt_t mt, 
        ec_msg_model_t mm, const ec_conn_t *conn, ec_observe_cb_t cb, 
        void *args);
static ec_observer_t *ec_observer_search(ec_observation_t *obs, ec_conn_t *cn,
        uint16_t mid);
static void ec_observer_free(ec_observer_t *ovr);
static int ec_observer_remove(ec_t *coap, ec_observation_t *obs, 
        ec_observer_t *ovr, bool *drained);

/* ec_observation_t private interfaces. */
static ec_observation_t *ec_observation_search(ec_t *coap, const char *uri);
static ec_observation_t *ec_observation_add(ec_t *coap, const char *uri, 
        uint32_t max_age);
static ec_observation_t *ec_observation_new(ec_t *coap, const char *uri, 
        uint32_t max_age);
static void ec_observation_free(ec_observation_t *obs);
static int ec_observation_start_countdown(ec_observation_t *obs);
static bool ec_source_match(const ec_conn_t *req_src, const ec_conn_t *obs_src);

/* 
 * TODO: The callback should produce all the requested representations in one 
 *       shot;
 *       Fix 'p' const'ness;
 */
static void ec_ob_cb(evutil_socket_t u0, short u1, void *c)
{
    bool drained;
    const uint8_t *p = NULL;
    size_t p_sz;
    uint16_t o_cnt;
    ec_observer_t *ovr;
    ec_pdu_t *nfy = NULL;
    ec_observation_t *obs = (ec_observation_t *) c;

    dbg_err_if (ec_get_observe_counter(&o_cnt));

    TAILQ_FOREACH(ovr, &obs->observers, next)
    {
        ec_flow_t flow;

        memset(&flow, 0, sizeof flow);

        /* Ask the user to produce the new resouce representation payload. */
        p = ovr->reps_cb(obs->uri, ovr->media_type, &p_sz, ovr->reps_cb_args);

        /* Create new ad-hoc PDU */
        dbg_err_sif ((nfy = ec_pdu_new_empty()) == NULL);

        /* Fill PDU with needed bits (TODO fix 'p' const'ness). */
        dbg_err_if (ec_pdu_set_flow(nfy, &flow));
        dbg_err_if (p && ec_pdu_set_payload(nfy, p, p_sz));
        dbg_err_if (ec_flow_set_resp_code(&flow, p ? EC_CONTENT : EC_DELETED));

        /* Stick the token sent by the client on the original request. */
        if (ovr->token_sz > 0)
        {
            dbg_err_if (ec_opts_add_token(&nfy->opts, ovr->token,
                        ovr->token_sz));
        }

        /* Add Observe option with counter. */
        dbg_err_if (ec_opts_add_observe(&nfy->opts, o_cnt));

        /* Encode PDU (NON). */
        dbg_err_if (ec_conn_copy(&ovr->conn, &flow.conn));
        dbg_err_if (ec_conn_set_confirmable(&flow.conn, false));
        dbg_err_if (ec_pdu_encode_response_separate(nfy));

        /* Send PDU (ignore ovr->msg_model for now, go NON all the way.)
         * 'NULL' means, don't go through the duplicate handling machinery. */
        dbg_err_if (ec_pdu_send(nfy, NULL));

        if (p == NULL)
        {
            /* Assume that a NULL payload signals the deletion of the 
             * corresponding resource => "the server SHOULD notify the 
             * client by sending a notification with an appropriate error 
             * response code (4.xx/5.xx) and MUST empty the list of observers 
             * of the resource." */
            dbg_if (ec_observer_remove(obs->base, obs, ovr, &drained));
        }
        else
        {
            /* Retrieve MID and save it in the observer context, so that we can 
             * identify it in case the observer RST's it. */
            ovr->last_mid = nfy->hdr_bits.mid;
        }

        ec_pdu_free(nfy), nfy = NULL;
    }
 
    /* In case observation has not been removed because of deletion of the
     * underlying resource, rearm the notification countdown. */
    if (!drained)
        dbg_err_if (ec_observation_start_countdown(obs));

err:
    if (nfy)
        ec_pdu_free(nfy);
    return;
}

/* Create new observer. */
static ec_observer_t *ec_observer_new(const uint8_t *token, size_t token_sz, 
        const uint8_t *etag, size_t etag_sz, ec_mt_t media_type, 
        ec_msg_model_t msg_model, const ec_conn_t *conn, ec_observe_cb_t cb, 
        void *cb_args)
{
    ec_observer_t *ovr = NULL;

    dbg_return_if (conn == NULL, NULL);
    dbg_return_if (token_sz > sizeof ovr->token, NULL);
    dbg_return_if (etag_sz > sizeof ovr->token, NULL);

    dbg_err_sif ((ovr = u_zalloc(sizeof *ovr)) == NULL);

    if (token && token_sz)
    {
        memcpy(ovr->token, token, token_sz);
        ovr->token_sz = token_sz;
    }

    if (etag && etag_sz)
    {
        memcpy(ovr->etag, etag, etag_sz);
        ovr->etag_sz = etag_sz;
    }

    ovr->media_type = media_type;
    ovr->msg_model = msg_model;

    /* Copy-in the needed bits from the connection object. */
    dbg_err_if (ec_conn_copy(conn, &ovr->conn));

    /* Attach user provided callback that will create the updated resource
     * representation. */
    ovr->reps_cb = cb;
    ovr->reps_cb_args = cb_args;

    return ovr;
err:
    if (ovr)
        ec_observer_free(ovr);
    return NULL;
}

/* Attach observer to the parent observation. */
static int ec_observer_add(ec_observation_t *obs, const uint8_t *tok,
        size_t tok_sz, const uint8_t *etag, size_t etag_sz, ec_mt_t mt, 
        ec_msg_model_t mm, const ec_conn_t *conn, ec_observe_cb_t cb, 
        void *args)
{
    ec_observer_t *ovr;

    dbg_return_if (obs == NULL, -1);

    /* Create a new observer given the supplied parameters. */
    ovr = ec_observer_new(tok, tok_sz, etag, etag_sz, mt, mm, conn, cb, args);
    dbg_return_ifm (ovr == NULL, -1, "observer creation failed");

    /* Add the observer to the parent observation. */
    TAILQ_INSERT_TAIL(&obs->observers, ovr, next);

    return 0;
}

/* See if the supplied requester matches an already active observer. */
static bool ec_source_match(const ec_conn_t *req_src, const ec_conn_t *obs_src)
{
    uint8_t peer_len;

    dbg_return_if (req_src == NULL, false);
    dbg_return_if (obs_src == NULL, false);

    dbg_err_if (ec_net_socklen(&req_src->peer, &peer_len));

    /* The source of a request is determined by the security mode used: with 
     * NoSec, it is determined by the source IP address and UDP port number. */
    if (!memcmp(&req_src->peer, &obs_src->peer, peer_len))
        return true;

    /* With other security modes, the source is also determined by the security
     * context. (TODO) */

    /* Fall through. */
err:
    return false;
}

/* Try to find an observer matching the given address/security context. */
static ec_observer_t *ec_observer_search(ec_observation_t *obs, ec_conn_t *conn,
        uint16_t mid)
{
    ec_observer_t *ovr;

    dbg_return_if (obs == NULL, NULL);
    dbg_return_if (conn == NULL, NULL);

    TAILQ_FOREACH(ovr, &obs->observers, next)
    {
        if (ec_source_match(conn, &ovr->conn))
        {
            /* If mid != 0, try to match also the last sent MID. */
            if (mid && ovr->last_mid != mid)
                continue;

            return ovr;
        }
    }

    return NULL;
}

/* Release resources allocated to the observer. */
static void ec_observer_free(ec_observer_t *ovr)
{
    if (ovr)
        u_free(ovr);
    /* TODO conn object de-initialization ? */
    return;
}

/* Search an observation matching the supplied URI. */
static ec_observation_t *ec_observation_search(ec_t *coap, const char *uri)
{
    ec_observation_t *obs;

    TAILQ_FOREACH(obs, &coap->observing, next)
    {
        if (!evutil_ascii_strcasecmp(obs->uri, uri))
            return obs;
    }

    return NULL;
}

/* Create a new parent observation. */
static ec_observation_t *ec_observation_new(ec_t *coap, const char *uri,
        uint32_t max_age)
{
    ec_observation_t *obs = NULL;

    dbg_err_if ((obs = u_zalloc(sizeof *obs)) == NULL);
    dbg_err_if (u_strlcpy(obs->uri, uri, sizeof obs->uri));
    obs->max_age = max_age ? max_age : EC_COAP_DEFAULT_MAX_AGE;
    obs->base = coap;

    /* Add timer. */
    dbg_err_if ((obs->notify = evtimer_new(coap->base, ec_ob_cb, obs)) == NULL);

    TAILQ_INIT(&obs->observers);

    return obs;
err:
    return NULL;
}

/* Add a new observation to the base. */
static ec_observation_t *ec_observation_add(ec_t *coap, const char *uri, 
        uint32_t max_age)
{
    ec_observation_t *obs;

    /* Let the creation interface check its own parameters. */
    dbg_return_if (coap == NULL, NULL);
 
    obs = ec_observation_new(coap, uri, max_age);
    dbg_err_ifm (obs == NULL, "observation creation failure");

    /* Start the countdown based on supplied resource max-age. */
    dbg_err_if (ec_observation_start_countdown(obs));

    TAILQ_INSERT_TAIL(&coap->observing, obs, next);

    return obs;
err:
    if (obs)
        ec_observation_free(obs);
    return NULL;
}

/* Fire the observation countdown. */
static int ec_observation_start_countdown(ec_observation_t *obs)
{
    dbg_return_if (obs == NULL, -1);

    struct timeval tout = { .tv_sec = obs->max_age, .tv_usec = 0 };

    dbg_return_if (evtimer_add(obs->notify, &tout), -1);

    return 0;
}

/* Free resources allocated to the supplied observation and related observers */
static void ec_observation_free(ec_observation_t *obs)
{
    ec_observer_t *ovr;

    if (obs)
    {
        /* Delete observers. */
        while ((ovr = TAILQ_FIRST(&obs->observers)))
        {
            TAILQ_REMOVE(&obs->observers, ovr, next);
            ec_observer_free(ovr);
        }

        /* Drop resource representations. */
        if (obs->cached_res)
            ec_resource_free(obs->cached_res);

        /* Cancel countdown timer. */
        evtimer_del(obs->notify);

        u_free(obs); 
    }
    return;
}

/**
 *  \brief  user adds a new observer for the implicit resource using this I/F
 */ 
int ec_add_observer(ec_server_t *srv, ec_observe_cb_t cb, void *cb_args,
        uint32_t max_age, ec_mt_t mt, ec_msg_model_t mm, const uint8_t *etag, 
        size_t etag_sz)
{
    ec_observation_t *obs, *new = NULL;

    dbg_return_if (srv == NULL, -1);
    dbg_return_if (cb == NULL, -1);

    /* Shortcuts (should they be assert'd?) */
    ec_t *coap = srv->base;
    ec_flow_t *flow = &srv->flow;

    /* Check whether this resource is already observed. */
    if ((obs = ec_observation_search(coap, flow->urlstr)) == NULL)
    {
        /* Create a new observe record and stick it to the base. */
        new = ec_observation_add(coap, flow->urlstr, max_age);
        dbg_err_ifm (new == NULL, "could not observe %s", flow->urlstr);
    }

    /* Push the newly created observer to base. */
    dbg_err_if (ec_observer_add(obs ? obs : new, flow->token, flow->token_sz, 
                etag, etag_sz, mt, mm, &flow->conn, cb, cb_args));

    return 0;
err:
    if (new)
        ec_observation_free(new);
    return -1;
}

/* TODO The lookup strategy is very inefficient: we should index observers by 
 * TODO addr and/or last sent MID to allow O(1) access. */
/* 
 *  0: not an active observer
 *  1: active observer deleted
 * -1: error (removal, param sanitization)
 */
int ec_observe_canceled_by_rst(ec_t *coap, ec_pdu_t *rst)
{
    ec_observation_t *obs;
    ec_observer_t *ovr = NULL;

    dbg_return_if (coap == NULL, -1);
    dbg_return_if (rst == NULL, -1);

    ec_flow_t *flow = rst->flow;
    ec_hdr_t *h = &rst->hdr_bits;

    /* Search each and every active observer. */
    TAILQ_FOREACH(obs, &coap->observing, next)
    {
        if ((ovr = ec_observer_search(obs, &flow->conn, h->mid)))
            break;
    }

    /* If found remove it (this also removes the parent observation in 
     * case it is the last observer.) */
    dbg_err_if (ovr && ec_observer_remove(coap, obs, ovr, NULL));

    return ovr ? 1 : 0;
err:
    return -1;
}

/* Same return codes as ec_observe_canceled_by_rst() */
int ec_observe_canceled_by_get(ec_server_t *srv)
{
    ec_observer_t *ovr;
    ec_observation_t *obs;

    dbg_return_if (srv == NULL, -1);

    ec_flow_t *flow = &srv->flow;
    ec_pdu_t *req = srv->req;
    ec_t *coap = srv->base;

    if (flow->method != EC_COAP_GET
            || (obs = ec_observation_search(coap, flow->urlstr)) == NULL)
        return 0;

    /* Delete observer (in case the Observe option has been set, the user
     * will later decide if an observation can be installed.) */
    if ((ovr = ec_observer_search(obs, &flow->conn, 0)))
        dbg_err_if (ec_observer_remove(coap, obs, ovr, NULL));

    return 1;
err:
    return -1;
}

/* Return '1' in case the observer removal is successful && the parent
 * observation has been removed. */
static int ec_observer_remove(ec_t *coap, ec_observation_t *obs, 
        ec_observer_t *ovr, bool *drained)
{
    dbg_return_if (coap == NULL, -1);
    dbg_return_if (obs == NULL, -1);
    dbg_return_if (ovr == NULL, -1);

    if (drained)
        *drained = false;

    /* Remove the observer and free memory. */
    TAILQ_REMOVE(&obs->observers, ovr, next);
    ec_observer_free(ovr);

    /* Also remove the observation in case there are no observers left. */
    if (TAILQ_EMPTY(&obs->observers))
    {
        if (drained)
            *drained = true;
        u_dbg("removing empty parent observation %p", obs);
        TAILQ_REMOVE(&coap->observing, obs, next)
        ec_observation_free(obs);
    }

    return 0;
}

int ec_rem_observer(ec_server_t *srv)
{
    ec_observer_t *ovr;
    ec_observation_t *obs;

    dbg_return_if (srv == NULL, -1);

    ec_flow_t *flow = &srv->flow;   /* shortcut */
    ec_t *coap = srv->base;         /* ditto */

    /* It's fine if the requested observation is not active, or there is no
     * such observer.  Just leave a trace in the (debug) log. */
    dbg_return_if (!(obs = ec_observation_search(coap, flow->urlstr)), 0);
    dbg_return_if (!(ovr = ec_observer_search(obs, &flow->conn, 0)), 0);

    /* Remove the observer and free memory. */
    return ec_observer_remove(coap, obs, ovr, NULL);
}

int ec_trigger_notification(ec_server_t *srv)
{
    dbg_return_if (srv == NULL, -1);
    u_con("TODO force a notification for the given resource");
    return 0;
}

int ec_observation_chores(void)
{
    u_con("Pass over all the observations and remove those likely to be stale");
    return 0;
}

int ec_observation_run(void)
{
    u_con("Execute a flush on the supplied observe queue");
    return 0;
}

