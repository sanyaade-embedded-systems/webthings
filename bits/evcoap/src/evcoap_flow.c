#include "evcoap_flow.h"

int ec_flow_save_token(ec_flow_t *flow, ev_uint8_t *tok, size_t tok_sz)
{
    dbg_return_if (flow == NULL, -1);

    if (tok == NULL || tok_sz == 0)
    {
        memset(flow->token, 0, sizeof flow->token);
        flow->token_sz = 0;
    }
    else
    {
        memcpy(flow->token, tok, tok_sz); 
        flow->token_sz = tok_sz;
    }

    return 0;
}

int ec_flow_save_url(ec_flow_t *flow, u_uri_t *url)
{
    dbg_return_if (flow == NULL, -1);
    dbg_return_if (url == NULL, -1);

    dbg_return_if (u_uri_knead(url, flow->urlstr), -1);
    flow->uri = url;

    return 0;
}
