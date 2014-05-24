#include "lcbht.h"
#include "sllist-inl.h"
#include "contrib/http_parser/http_parser.h"
#include "settings.h"
enum last_call_type {
    CB_NONE,
    CB_HDR_KEY,
    CB_HDR_VALUE,
    CB_HDR_DONE,
    CB_BODY,
    CB_MSG_DONE
};

struct lcbht_PARSER {
    http_parser parser;
    lcbht_RESPONSE resp; /**< Current response being processed */
    lcb_settings *settings;
    enum last_call_type lastcall;

    /* this stuff for parse_ex() */
    const char *last_body;
    unsigned last_bodylen;
    char paused;
    char is_ex;
};

static int
on_hdr_key(http_parser *pb, const char *s, size_t n)
{
    lcbht_pPARSER p = (void *)pb;
    lcbht_MIMEHDR *hdr;
    lcbht_RESPONSE *resp = &p->resp;

    if (p->lastcall != CB_HDR_KEY) {
        /* new key */
        hdr = calloc(1, sizeof *hdr);
        sllist_append(&resp->headers, &hdr->slnode);
        lcb_string_init(&hdr->buf_);
    } else {
        hdr = SLLIST_ITEM(resp->headers.last, lcbht_MIMEHDR, slnode);
    }

    /* append key data */
    lcb_string_append(&hdr->buf_, s, n);
    p->lastcall = CB_HDR_KEY;
    return 0;
}

static int
on_hdr_value(http_parser *pb, const char *s, size_t n)
{
    lcbht_pPARSER p = (void *)pb;
    lcbht_MIMEHDR *hdr;
    lcbht_RESPONSE *resp = &p->resp;

    hdr = SLLIST_ITEM(resp->headers.last, lcbht_MIMEHDR, slnode);
    if (p->lastcall == CB_HDR_KEY) {
        lcb_string_appendz(&hdr->buf_, ":");
    }
    lcb_string_append(&hdr->buf_, s, n);
    p->lastcall = CB_HDR_VALUE;
    return 0;
}

static int
on_hdr_done(http_parser *pb)
{
    lcbht_pPARSER p = (void *)pb;
    lcbht_RESPONSE *resp = &p->resp;
    sllist_iterator iter;
    resp->state |= LCBHT_S_HTSTATUS|LCBHT_S_HEADER;

    /* extract the status */
    resp->status = pb->status_code;
    p->lastcall = CB_HDR_DONE;

    /** Iterate through all the headers and do proper formatting */
    SLLIST_ITERFOR(&resp->headers, &iter) {
        char *delim;
        lcbht_MIMEHDR *hdr = SLLIST_ITEM(iter.cur, lcbht_MIMEHDR, slnode);

        delim = strstr(hdr->buf_.base, ":");
        hdr->key = hdr->buf_.base;

        if (delim) {
            *delim = '\0';
            hdr->value = delim + 1;
        } else {
            hdr->value = "";
        }
    }
    return 0;
}

static int
on_body(http_parser *pb, const char *s, size_t n)
{
    lcbht_pPARSER p = (void *)pb;
    lcbht_RESPONSE *resp = &p->resp;
    if (p->is_ex) {
        p->last_body = s;
        p->last_bodylen = n;
        p->paused = 1;
        _lcb_http_parser_pause(pb, 1);
    } else {
        lcb_string_append(&resp->body, s, n);
    }

    p->lastcall = CB_BODY;
    resp->state |= LCBHT_S_BODY;
    return 0;
}

static int
on_msg_done(http_parser *pb)
{
    lcbht_pPARSER p = (void *)pb;
    lcbht_RESPONSE *resp = &p->resp;
    resp->state |= LCBHT_S_DONE;
    return 0;
}


static struct http_parser_settings Parser_Settings = {
        NULL, /* msg_begin */
        NULL, /* on_url */
        on_hdr_key,
        on_hdr_value,
        on_hdr_done,
        on_body,
        on_msg_done
};

lcbht_pPARSER
lcbht_new(lcb_settings *settings)
{
    lcbht_pPARSER ret = calloc(1, sizeof *ret);
    _lcb_http_parser_init(&ret->parser, HTTP_RESPONSE);
    lcbht_clear_response(&ret->resp);
    ret->settings = settings;
    lcb_settings_ref(settings);
    return ret;
}

void
lcbht_free(lcbht_pPARSER parser)
{
    lcbht_clear_response(&parser->resp);
    lcb_settings_unref(parser->settings);
    free(parser);
}

lcbht_RESPSTATE
lcbht_parse(lcbht_pPARSER parser, const void *data, unsigned ndata)
{
    size_t nb;
    parser->is_ex = 0;
    nb = _lcb_http_parser_execute(&parser->parser, &Parser_Settings, data, ndata);
    if (nb != ndata) {
        parser->resp.state |= LCBHT_S_ERROR;
    }
    return parser->resp.state;
}

lcbht_RESPSTATE
lcbht_parse_ex(lcbht_pPARSER parser, const void *data, unsigned ndata,
    unsigned *nused, unsigned *nbody, const char **pbody)
{
    size_t nb;
    parser->is_ex = 1;
    nb = _lcb_http_parser_execute(&parser->parser, &Parser_Settings, data, ndata);
    if (nb != ndata) {
        if (parser->paused) {
            _lcb_http_parser_pause(&parser->parser, 0);
            parser->paused = 0;
        } else {
            parser->resp.state |= LCBHT_S_ERROR;
            return parser->resp.state;
        }
    }

    *nused = nb;
    *nbody = parser->last_bodylen;
    *pbody = parser->last_body;

    parser->last_body = NULL;
    parser->last_bodylen = 0;
    return parser->resp.state;
}

lcbht_RESPONSE *
lcbht_get_response(lcbht_pPARSER parser)
{
    return &parser->resp;
}

void
lcbht_clear_response(lcbht_RESPONSE *resp)
{
    sllist_iterator iter;
    SLLIST_ITERFOR(&resp->headers, &iter) {
        lcbht_MIMEHDR *hdr = SLLIST_ITEM(iter.cur, lcbht_MIMEHDR, slnode);
        lcb_string_release(&hdr->buf_);
        sllist_iter_remove(&resp->headers, &iter);
        free(hdr);
    }
    lcb_string_release(&resp->body);
    resp->state = 0;
    resp->status = 0;
}

void
lcbht_reset(lcbht_pPARSER parser)
{
    lcbht_clear_response(&parser->resp);
}

const char *
lcbht_get_resphdr(const lcbht_RESPONSE *resp, const char *key)
{
    sllist_node *curnode;
    SLLIST_ITERBASIC(&resp->headers, curnode) {
        lcbht_MIMEHDR *hdr = SLLIST_ITEM(curnode, lcbht_MIMEHDR, slnode);
        if (!strcmp(hdr->key, key)) {
            return hdr->value;
        }
    }
    return NULL;
}

char **
lcbht_make_resphdrlist(lcbht_RESPONSE *response)
{
    char **ret;
    unsigned nhdrs = 0;
    unsigned curix = 0;
    sllist_node *curnode;
    SLLIST_ITERBASIC(&response->headers, curnode) {
        nhdrs++;
    }
    ret = malloc(sizeof(*ret) * (nhdrs + 1) * 2);

    SLLIST_ITERBASIC(&response->headers, curnode) {
        lcbht_MIMEHDR *hdr = SLLIST_ITEM(curnode, lcbht_MIMEHDR, slnode);
        ret[curix++] = strdup(hdr->key);
        ret[curix++] = strdup(hdr->value);
    }

    ret[curix] = NULL;
    return ret;
}
