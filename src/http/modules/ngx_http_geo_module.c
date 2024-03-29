
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_http_variable_value_t       *value;
    u_short                          start;
    u_short                          end;
} ngx_http_geo_range_t;


typedef struct {
    ngx_http_geo_range_t           **low;
    ngx_http_variable_value_t       *default_value;
} ngx_http_geo_high_ranges_t;


typedef struct {
    ngx_str_node_t                   sn;
    ngx_http_variable_value_t       *value;
    size_t                           offset;
} ngx_http_geo_variable_value_node_t;


typedef struct {
    ngx_http_variable_value_t       *value;
    ngx_str_t                       *net;
    ngx_http_geo_high_ranges_t       high;
    ngx_radix_tree_t                *tree;
    ngx_rbtree_t                     rbtree;
    ngx_rbtree_node_t                sentinel;
    ngx_array_t                     *proxies;
    ngx_pool_t                      *pool;
    ngx_pool_t                      *temp_pool;

    size_t                           data_size;

    ngx_str_t                        include_name;
    ngx_uint_t                       includes;
    ngx_uint_t                       entries;

    unsigned                         ranges:1;
    unsigned                         outside_entries:1;
    unsigned                         allow_binary_include:1;
    unsigned                         binary_include:1;
} ngx_http_geo_conf_ctx_t;


typedef struct {
    union {
        ngx_radix_tree_t            *tree;
        ngx_http_geo_high_ranges_t   high;
    } u;

    ngx_array_t                     *proxies;

    ngx_int_t                        index;

    ngx_str_t                        var;
} ngx_http_geo_ctx_t;


static in_addr_t ngx_http_geo_addr(ngx_http_request_t *r,
    ngx_http_geo_ctx_t *ctx);
static in_addr_t ngx_http_geo_real_addr(ngx_http_request_t *r,
    ngx_http_geo_ctx_t *ctx);
static char *ngx_http_geo_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_geo(ngx_conf_t *cf, ngx_command_t *dummy, void *conf);
static char *ngx_http_geo_range(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *value);
static char *ngx_http_geo_add_range(ngx_conf_t *cf,
    ngx_http_geo_conf_ctx_t *ctx, in_addr_t start, in_addr_t end);
static ngx_uint_t ngx_http_geo_delete_range(ngx_conf_t *cf,
    ngx_http_geo_conf_ctx_t *ctx, in_addr_t start, in_addr_t end);
static char *ngx_http_geo_cidr(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *value);
static ngx_http_variable_value_t *ngx_http_geo_value(ngx_conf_t *cf,
    ngx_http_geo_conf_ctx_t *ctx, ngx_str_t *value);
static char *ngx_http_geo_add_proxy(ngx_conf_t *cf,
    ngx_http_geo_conf_ctx_t *ctx, ngx_cidr_t *cidr);
static ngx_int_t ngx_http_geo_cidr_value(ngx_conf_t *cf, ngx_str_t *net,
    ngx_cidr_t *cidr);
static char *ngx_http_geo_include(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *name);
static ngx_int_t ngx_http_geo_include_binary_base(ngx_conf_t *cf,
    ngx_http_geo_conf_ctx_t *ctx, ngx_str_t *name);
static void ngx_http_geo_create_binary_base(ngx_http_geo_conf_ctx_t *ctx);
static u_char *ngx_http_geo_copy_values(u_char *base, u_char *p,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);


static ngx_command_t  ngx_http_geo_commands[] = {

    { ngx_string("geo"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE12,
      ngx_http_geo_block,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_geo_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_geo_module = {
    NGX_MODULE_V1,
    &ngx_http_geo_module_ctx,              /* module context */
    ngx_http_geo_commands,                 /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


typedef struct {
    u_char    GEORNG[6];
    u_char    version;
    u_char    ptr_size;
    uint32_t  endianess;
    uint32_t  crc32;
} ngx_http_geo_header_t;


static ngx_http_geo_header_t  ngx_http_geo_header = {
    { 'G', 'E', 'O', 'R', 'N', 'G' }, 0, sizeof(void *), 0x12345678, 0
};


/* AF_INET only */

static ngx_int_t
ngx_http_geo_cidr_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_geo_ctx_t *ctx = (ngx_http_geo_ctx_t *) data;

    ngx_http_variable_value_t  *vv;

    vv = (ngx_http_variable_value_t *)
              ngx_radix32tree_find(ctx->u.tree, ngx_http_geo_addr(r, ctx));

    *v = *vv;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http geo: %v", v);

    return NGX_OK;
}

static void ngx_http_geo_addr_record(ngx_http_request_t *r, in_addr_t addr) {
    ngx_str_t ip;
    ip.len = NGX_INET_ADDRSTRLEN + sizeof(":65536") - 1;
    ip.data =  ngx_pnalloc(r->pool, ip.len);
    if (ip.data == NULL) {
        return;
    }

    in_addr_t naddr = htonl(addr);

    ip.len = ngx_inet_ntop(AF_INET, &naddr, ip.data, ip.len);
    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "unknown mall real ip %V", &ip);
}

static ngx_int_t ngx_http_geo_addr_add_cookie(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, ngx_str_t *var) {
    size_t fixlen = sizeof("; Max-Age=259200; path=/; domain=.10010.com") - 1;
    size_t len = var->len + 1 + v->len + fixlen;
    

    u_char *cookie = ngx_pnalloc(r->pool, len);
    if (cookie == NULL) {
        return NGX_ERROR;
    }

    u_char *p = ngx_copy(cookie, var->data, var->len);
    *p++ = '=';
    p = ngx_copy(p, v->data, v->len);
    p = ngx_cpymem(p, "; Max-Age=259200; path=/; domain=.10010.com", fixlen);

    ngx_table_elt_t *set_cookie = ngx_list_push(&r->headers_out.headers);
    if (set_cookie == NULL) {
        return NGX_ERROR;
    }

    set_cookie->hash = 1;
    ngx_str_set(&set_cookie->key, "Set-Cookie");
    set_cookie->value.len = p - cookie;
    set_cookie->value.data = cookie;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "set cookie: \"%V\"", &set_cookie->value);
    // End set cookie after IP ranges lookup
    

    return NGX_OK;
}


static ngx_int_t
ngx_http_geo_range_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_geo_ctx_t *ctx = (ngx_http_geo_ctx_t *) data;

    in_addr_t              addr;
    ngx_uint_t             n;
    ngx_http_geo_range_t  *range;

    // Begin check cookie before IP ranges lookup
    ngx_str_t  cookie_data;

    if (ngx_http_parse_multi_header_lines(&r->headers_in.cookies, &ctx->var, &cookie_data) != NGX_DECLINED)  {
        v->len = cookie_data.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = cookie_data.data;

        return NGX_OK;
    }
    // End check cookie before IP ranges lookup

    *v = *ctx->u.high.default_value;

    addr = ngx_http_geo_addr(r, ctx);
    unsigned found = 0;

    range = ctx->u.high.low[addr >> 16];

    if (range) {
        n = addr & 0xffff;
        do {
            if (n >= (ngx_uint_t) range->start && n <= (ngx_uint_t) range->end) {
                *v = *range->value;
                found = 1;
                break;
            }
        } while ((++range)->value);
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "http geo: %v", v);

    // Record unkown location IP to error with warn level.
    if(found == 0) {
        ngx_http_geo_addr_record(r, addr);
    }

    // Set cookie after IP ranges lookup
    if (ngx_http_geo_addr_add_cookie(r, v, &(ctx->var)) != NGX_OK){
        return NGX_ERROR;
    }
    ngx_str_t gipgeo = ngx_string("gipgeo");
    if (ngx_http_geo_addr_add_cookie(r, v, &gipgeo) != NGX_OK){
        return NGX_ERROR;
    }

    return NGX_OK;
}


static in_addr_t
ngx_http_geo_addr(ngx_http_request_t *r, ngx_http_geo_ctx_t *ctx)
{
    u_char           *p, *ip;
    size_t            len;
    in_addr_t         addr;
    //ngx_uint_t        i, n;
    //ngx_in_cidr_t    *proxies;
    ngx_table_elt_t  *xfwd;

    addr = ngx_http_geo_real_addr(r, ctx);
    
    // ngx_http_geo_addr_record(r, addr);

    xfwd = r->headers_in.x_forwarded_for;

    if (xfwd == NULL /*|| ctx->proxies == NULL*/) {
        return addr;
    }

    //ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "unknown mall xfwd ip %V", &(xfwd->value));

    //proxies = ctx->proxies->elts;
    //n = ctx->proxies->nelts;

    //for (i = 0; i < n; i++) {
        //if ((addr & proxies[i].mask) == proxies[i].addr) {

            len = xfwd->value.len;
            ip = xfwd->value.data;

            for (p = ip + len - 1; p > ip; p--) {
                if (*p == ' ' || *p == ',') {
                    p++;
                    len -= p - ip;
                    ip = p;
                    break;
                }
            }

            return ntohl(ngx_inet_addr(ip, len));
        //}
    //}

    //return addr;
}


static in_addr_t
ngx_http_geo_real_addr(ngx_http_request_t *r, ngx_http_geo_ctx_t *ctx)
{
    struct sockaddr_in         *sin;
    ngx_http_variable_value_t  *v;
#if (NGX_HAVE_INET6)
    u_char                     *p;
    in_addr_t                   addr;
    struct sockaddr_in6        *sin6;
#endif

    if (ctx->index == -1) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http geo started: %V", &r->connection->addr_text);

        switch (r->connection->sockaddr->sa_family) {

        case AF_INET:
            sin = (struct sockaddr_in *) r->connection->sockaddr;
            return ntohl(sin->sin_addr.s_addr);

#if (NGX_HAVE_INET6)

        case AF_INET6:
            sin6 = (struct sockaddr_in6 *) r->connection->sockaddr;

            if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
                p = sin6->sin6_addr.s6_addr;
                addr = p[12] << 24;
                addr += p[13] << 16;
                addr += p[14] << 8;
                addr += p[15];

                return addr;
            }

#endif
        }

        return INADDR_NONE;
    }

    v = ngx_http_get_flushed_variable(r, ctx->index);

    if (v == NULL || v->not_found) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http geo not found");

        return 0;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http geo started: %v", v);

    return ntohl(ngx_inet_addr(v->data, v->len));
}


static char *
ngx_http_geo_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                     *rv;
    void                    **p;
    size_t                    len;
    ngx_str_t                *value, name;
    ngx_uint_t                i;
    ngx_conf_t                save;
    ngx_pool_t               *pool;
    ngx_array_t              *a;
    ngx_http_variable_t      *var;
    ngx_http_geo_ctx_t       *geo;
    ngx_http_geo_conf_ctx_t   ctx;

    value = cf->args->elts;

    geo = ngx_palloc(cf->pool, sizeof(ngx_http_geo_ctx_t));
    if (geo == NULL) {
        return NGX_CONF_ERROR;
    }

    name = value[1];
    name.len--;
    name.data++;

    if (cf->args->nelts == 3) {

        geo->index = ngx_http_get_variable_index(cf, &name);
        if (geo->index == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }

        name = value[2];
        name.len--;
        name.data++;

    } else {
        geo->index = -1;
    }

    var = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_CHANGEABLE);
    if (var == NULL) {
        return NGX_CONF_ERROR;
    }

    geo->var = name;

    pool = ngx_create_pool(16384, cf->log);
    if (pool == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ctx, sizeof(ngx_http_geo_conf_ctx_t));

    ctx.temp_pool = ngx_create_pool(16384, cf->log);
    if (ctx.temp_pool == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_rbtree_init(&ctx.rbtree, &ctx.sentinel, ngx_str_rbtree_insert_value);

    ctx.pool = cf->pool;
    ctx.data_size = sizeof(ngx_http_geo_header_t)
                  + sizeof(ngx_http_variable_value_t)
                  + 0x10000 * sizeof(ngx_http_geo_range_t *);
    ctx.allow_binary_include = 1;

    save = *cf;
    cf->pool = pool;
    cf->ctx = &ctx;
    cf->handler = ngx_http_geo;
    cf->handler_conf = conf;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    geo->proxies = ctx.proxies;

    if (ctx.high.low) {

        if (!ctx.binary_include) {
            for (i = 0; i < 0x10000; i++) {
                a = (ngx_array_t *) ctx.high.low[i];

                if (a == NULL || a->nelts == 0) {
                    continue;
                }

                len = a->nelts * sizeof(ngx_http_geo_range_t);

                ctx.high.low[i] = ngx_palloc(cf->pool, len + sizeof(void *));
                if (ctx.high.low[i] == NULL) {
                    return NGX_CONF_ERROR;
                }

                p = (void **) ngx_cpymem(ctx.high.low[i], a->elts, len);
                *p = NULL;
                ctx.data_size += len + sizeof(void *);
            }

            if (ctx.allow_binary_include
                && !ctx.outside_entries
                && ctx.entries > 100000
                && ctx.includes == 1)
            {
                ngx_http_geo_create_binary_base(&ctx);
            }
        }

        geo->u.high = ctx.high;

        var->get_handler = ngx_http_geo_range_variable;
        var->data = (uintptr_t) geo;

        if (ctx.high.default_value == NULL) {
            ctx.high.default_value = &ngx_http_variable_null_value;
        }

        ngx_destroy_pool(ctx.temp_pool);
        ngx_destroy_pool(pool);

    } else {
        if (ctx.tree == NULL) {
            ctx.tree = ngx_radix_tree_create(cf->pool, -1);
            if (ctx.tree == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        geo->u.tree = ctx.tree;

        var->get_handler = ngx_http_geo_cidr_variable;
        var->data = (uintptr_t) geo;

        ngx_destroy_pool(ctx.temp_pool);
        ngx_destroy_pool(pool);

        if (ngx_radix32tree_find(ctx.tree, 0) != NGX_RADIX_NO_VALUE) {
            return rv;
        }

        if (ngx_radix32tree_insert(ctx.tree, 0, 0,
                                   (uintptr_t) &ngx_http_variable_null_value)
            == NGX_ERROR)
        {
            return NGX_CONF_ERROR;
        }
    }

    return rv;
}


static char *
ngx_http_geo(ngx_conf_t *cf, ngx_command_t *dummy, void *conf)
{
    char                     *rv;
    ngx_str_t                *value;
    ngx_cidr_t                cidr;
    ngx_http_geo_conf_ctx_t  *ctx;

    ctx = cf->ctx;

    value = cf->args->elts;

    if (cf->args->nelts == 1) {

        if (ngx_strcmp(value[0].data, "ranges") == 0) {

            if (ctx->tree) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "the \"ranges\" directive must be "
                                   "the first directive inside \"geo\" block");
                goto failed;
            }

            ctx->ranges = 1;

            rv = NGX_CONF_OK;

            goto done;
        }
    }

    if (cf->args->nelts != 2) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number of the geo parameters");
        goto failed;
    }

    if (ngx_strcmp(value[0].data, "include") == 0) {

        rv = ngx_http_geo_include(cf, ctx, &value[1]);

        goto done;

    } else if (ngx_strcmp(value[0].data, "proxy") == 0) {

        if (ngx_http_geo_cidr_value(cf, &value[1], &cidr) != NGX_OK) {
            goto failed;
        }

        rv = ngx_http_geo_add_proxy(cf, ctx, &cidr);

        goto done;
    }

    if (ctx->ranges) {
        rv = ngx_http_geo_range(cf, ctx, value);

    } else {
        rv = ngx_http_geo_cidr(cf, ctx, value);
    }

done:

    ngx_reset_pool(cf->pool);

    return rv;

failed:

    ngx_reset_pool(cf->pool);

    return NGX_CONF_ERROR;
}


static char *
ngx_http_geo_range(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *value)
{
    u_char      *p, *last;
    in_addr_t    start, end;
    ngx_str_t   *net;
    ngx_uint_t   del;

    if (ngx_strcmp(value[0].data, "default") == 0) {

        if (ctx->high.default_value) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                "duplicate default geo range value: \"%V\", old value: \"%v\"",
                &value[1], ctx->high.default_value);
        }

        ctx->high.default_value = ngx_http_geo_value(cf, ctx, &value[1]);
        if (ctx->high.default_value == NULL) {
            return NGX_CONF_ERROR;
        }

        return NGX_CONF_OK;
    }

    if (ctx->binary_include) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "binary geo range base \"%s\" cannot be mixed with usual entries",
            ctx->include_name.data);
        return NGX_CONF_ERROR;
    }

    if (ctx->high.low == NULL) {
        ctx->high.low = ngx_pcalloc(ctx->pool,
                                    0x10000 * sizeof(ngx_http_geo_range_t *));
        if (ctx->high.low == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    ctx->entries++;
    ctx->outside_entries = 1;

    if (ngx_strcmp(value[0].data, "delete") == 0) {
        net = &value[1];
        del = 1;

    } else {
        net = &value[0];
        del = 0;
    }

    last = net->data + net->len;

    p = ngx_strlchr(net->data, last, '-');

    if (p == NULL) {
        goto invalid;
    }

    start = ngx_inet_addr(net->data, p - net->data);

    if (start == INADDR_NONE) {
        goto invalid;
    }

    start = ntohl(start);

    p++;

    end = ngx_inet_addr(p, last - p);

    if (end == INADDR_NONE) {
        goto invalid;
    }

    end = ntohl(end);

    if (start > end) {
        goto invalid;
    }

    if (del) {
        if (ngx_http_geo_delete_range(cf, ctx, start, end)) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "no address range \"%V\" to delete", net);
        }

        return NGX_CONF_OK;
    }

    ctx->value = ngx_http_geo_value(cf, ctx, &value[1]);

    if (ctx->value == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->net = net;

    return ngx_http_geo_add_range(cf, ctx, start, end);

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid range \"%V\"", net);

    return NGX_CONF_ERROR;
}


/* the add procedure is optimized to add a growing up sequence */

static char *
ngx_http_geo_add_range(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    in_addr_t start, in_addr_t end)
{
    in_addr_t              n;
    ngx_uint_t             h, i, s, e;
    ngx_array_t           *a;
    ngx_http_geo_range_t  *range;

    for (n = start; n <= end; n = (n + 0x10000) & 0xffff0000) {

        h = n >> 16;

        if (n == start) {
            s = n & 0xffff;
        } else {
            s = 0;
        }

        if ((n | 0xffff) > end) {
            e = end & 0xffff;

        } else {
            e = 0xffff;
        }

        a = (ngx_array_t *) ctx->high.low[h];

        if (a == NULL) {
            a = ngx_array_create(ctx->temp_pool, 64,
                                 sizeof(ngx_http_geo_range_t));
            if (a == NULL) {
                return NGX_CONF_ERROR;
            }

            ctx->high.low[h] = (ngx_http_geo_range_t *) a;
        }

        i = a->nelts;
        range = a->elts;

        while (i) {

            i--;

            if (e < (ngx_uint_t) range[i].start) {
                continue;
            }

            if (s > (ngx_uint_t) range[i].end) {

                /* add after the range */

                range = ngx_array_push(a);
                if (range == NULL) {
                    return NGX_CONF_ERROR;
                }

                range = a->elts;

                ngx_memmove(&range[i + 2], &range[i + 1],
                           (a->nelts - 2 - i) * sizeof(ngx_http_geo_range_t));

                range[i + 1].start = (u_short) s;
                range[i + 1].end = (u_short) e;
                range[i + 1].value = ctx->value;

                goto next;
            }

            if (s == (ngx_uint_t) range[i].start
                && e == (ngx_uint_t) range[i].end)
            {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                    "duplicate range \"%V\", value: \"%v\", old value: \"%v\"",
                    ctx->net, ctx->value, range[i].value);

                range[i].value = ctx->value;

                goto next;
            }

            if (s > (ngx_uint_t) range[i].start
                && e < (ngx_uint_t) range[i].end)
            {
                /* split the range and insert the new one */

                range = ngx_array_push(a);
                if (range == NULL) {
                    return NGX_CONF_ERROR;
                }

                range = ngx_array_push(a);
                if (range == NULL) {
                    return NGX_CONF_ERROR;
                }

                range = a->elts;

                ngx_memmove(&range[i + 3], &range[i + 1],
                           (a->nelts - 3 - i) * sizeof(ngx_http_geo_range_t));

                range[i + 2].start = (u_short) (e + 1);
                range[i + 2].end = range[i].end;
                range[i + 2].value = range[i].value;

                range[i + 1].start = (u_short) s;
                range[i + 1].end = (u_short) e;
                range[i + 1].value = ctx->value;

                range[i].end = (u_short) (s - 1);

                goto next;
            }

            if (s == (ngx_uint_t) range[i].start
                && e < (ngx_uint_t) range[i].end)
            {
                /* shift the range start and insert the new range */

                range = ngx_array_push(a);
                if (range == NULL) {
                    return NGX_CONF_ERROR;
                }

                range = a->elts;

                ngx_memmove(&range[i + 1], &range[i],
                           (a->nelts - 1 - i) * sizeof(ngx_http_geo_range_t));

                range[i + 1].start = (u_short) (e + 1);

                range[i].start = (u_short) s;
                range[i].end = (u_short) e;
                range[i].value = ctx->value;

                goto next;
            }

            if (s > (ngx_uint_t) range[i].start
                && e == (ngx_uint_t) range[i].end)
            {
                /* shift the range end and insert the new range */

                range = ngx_array_push(a);
                if (range == NULL) {
                    return NGX_CONF_ERROR;
                }

                range = a->elts;

                ngx_memmove(&range[i + 2], &range[i + 1],
                           (a->nelts - 2 - i) * sizeof(ngx_http_geo_range_t));

                range[i + 1].start = (u_short) s;
                range[i + 1].end = (u_short) e;
                range[i + 1].value = ctx->value;

                range[i].end = (u_short) (s - 1);

                goto next;
            }

            s = (ngx_uint_t) range[i].start;
            e = (ngx_uint_t) range[i].end;

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "range \"%V\" overlaps \"%d.%d.%d.%d-%d.%d.%d.%d\"",
                         ctx->net,
                         h >> 8, h & 0xff, s >> 8, s & 0xff,
                         h >> 8, h & 0xff, e >> 8, e & 0xff);

            return NGX_CONF_ERROR;
        }

        /* add the first range */

        range = ngx_array_push(a);
        if (range == NULL) {
            return NGX_CONF_ERROR;
        }

        range->start = (u_short) s;
        range->end = (u_short) e;
        range->value = ctx->value;

    next:

        continue;
    }

    return NGX_CONF_OK;
}


static ngx_uint_t
ngx_http_geo_delete_range(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    in_addr_t start, in_addr_t end)
{
    in_addr_t              n;
    ngx_uint_t             h, i, s, e, warn;
    ngx_array_t           *a;
    ngx_http_geo_range_t  *range;

    warn = 0;

    for (n = start; n <= end; n += 0x10000) {

        h = n >> 16;

        if (n == start) {
            s = n & 0xffff;
        } else {
            s = 0;
        }

        if ((n | 0xffff) > end) {
            e = end & 0xffff;

        } else {
            e = 0xffff;
        }

        a = (ngx_array_t *) ctx->high.low[h];

        if (a == NULL) {
            warn = 1;
            continue;
        }

        range = a->elts;
        for (i = 0; i < a->nelts; i++) {

            if (s == (ngx_uint_t) range[i].start
                && e == (ngx_uint_t) range[i].end)
            {
                ngx_memmove(&range[i], &range[i + 1],
                           (a->nelts - 1 - i) * sizeof(ngx_http_geo_range_t));

                a->nelts--;

                break;
            }

            if (s != (ngx_uint_t) range[i].start
                && e != (ngx_uint_t) range[i].end)
            {
                continue;
            }

            warn = 1;
        }
    }

    return warn;
}


static char *
ngx_http_geo_cidr(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *value)
{
    ngx_int_t                        rc, del;
    ngx_str_t                       *net;
    ngx_uint_t                       i;
    ngx_cidr_t                       cidr;
    ngx_http_variable_value_t       *val, *old;

    if (ctx->tree == NULL) {
        ctx->tree = ngx_radix_tree_create(ctx->pool, -1);
        if (ctx->tree == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    if (ngx_strcmp(value[0].data, "default") == 0) {
        cidr.u.in.addr = 0;
        cidr.u.in.mask = 0;
        net = &value[0];

    } else {
        if (ngx_strcmp(value[0].data, "delete") == 0) {
            net = &value[1];
            del = 1;

        } else {
            net = &value[0];
            del = 0;
        }

        if (ngx_http_geo_cidr_value(cf, net, &cidr) != NGX_OK) {
            return NGX_CONF_ERROR;
        }

        if (del) {
            if (ngx_radix32tree_delete(ctx->tree, cidr.u.in.addr,
                                       cidr.u.in.mask)
                != NGX_OK)
            {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "no network \"%V\" to delete", net);
            }

            return NGX_CONF_OK;
        }
    }

    val = ngx_http_geo_value(cf, ctx, &value[1]);

    if (val == NULL) {
        return NGX_CONF_ERROR;
    }

    for (i = 2; i; i--) {
        rc = ngx_radix32tree_insert(ctx->tree, cidr.u.in.addr, cidr.u.in.mask,
                                    (uintptr_t) val);
        if (rc == NGX_OK) {
            return NGX_CONF_OK;
        }

        if (rc == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }

        /* rc == NGX_BUSY */

        old = (ngx_http_variable_value_t *)
              ngx_radix32tree_find(ctx->tree, cidr.u.in.addr & cidr.u.in.mask);

        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                "duplicate network \"%V\", value: \"%v\", old value: \"%v\"",
                net, val, old);

        rc = ngx_radix32tree_delete(ctx->tree, cidr.u.in.addr, cidr.u.in.mask);

        if (rc == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid radix tree");
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_ERROR;
}


static ngx_http_variable_value_t *
ngx_http_geo_value(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *value)
{
    uint32_t                             hash;
    ngx_http_variable_value_t           *val;
    ngx_http_geo_variable_value_node_t  *gvvn;

    hash = ngx_crc32_long(value->data, value->len);

    gvvn = (ngx_http_geo_variable_value_node_t *)
               ngx_str_rbtree_lookup(&ctx->rbtree, value, hash);

    if (gvvn) {
        return gvvn->value;
    }

    val = ngx_palloc(ctx->pool, sizeof(ngx_http_variable_value_t));
    if (val == NULL) {
        return NULL;
    }

    val->len = value->len;
    val->data = ngx_pstrdup(ctx->pool, value);
    if (val->data == NULL) {
        return NULL;
    }

    val->valid = 1;
    val->no_cacheable = 0;
    val->not_found = 0;

    gvvn = ngx_palloc(ctx->temp_pool,
                      sizeof(ngx_http_geo_variable_value_node_t));
    if (gvvn == NULL) {
        return NULL;
    }

    gvvn->sn.node.key = hash;
    gvvn->sn.str.len = val->len;
    gvvn->sn.str.data = val->data;
    gvvn->value = val;
    gvvn->offset = 0;

    ngx_rbtree_insert(&ctx->rbtree, &gvvn->sn.node);

    ctx->data_size += ngx_align(sizeof(ngx_http_variable_value_t) + value->len,
                                sizeof(void *));

    return val;
}


static char *
ngx_http_geo_add_proxy(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_cidr_t *cidr)
{
    ngx_in_cidr_t  *c;

    if (ctx->proxies == NULL) {
        ctx->proxies = ngx_array_create(ctx->pool, 4, sizeof(ngx_in_cidr_t));
        if (ctx->proxies == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    c = ngx_array_push(ctx->proxies);
    if (c == NULL) {
        return NGX_CONF_ERROR;
    }

    c->addr = cidr->u.in.addr;
    c->mask = cidr->u.in.mask;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_geo_cidr_value(ngx_conf_t *cf, ngx_str_t *net, ngx_cidr_t *cidr)
{
    ngx_int_t  rc;

    if (ngx_strcmp(net->data, "255.255.255.255") == 0) {
        cidr->u.in.addr = 0xffffffff;
        cidr->u.in.mask = 0xffffffff;

        return NGX_OK;
    }

    rc = ngx_ptocidr(net, cidr);

    if (rc == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid network \"%V\"", net);
        return NGX_ERROR;
    }

    if (cidr->family != AF_INET) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "\"geo\" supports IPv4 only");
        return NGX_ERROR;
    }

    if (rc == NGX_DONE) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "low address bits of %V are meaningless", net);
    }

    cidr->u.in.addr = ntohl(cidr->u.in.addr);
    cidr->u.in.mask = ntohl(cidr->u.in.mask);

    return NGX_OK;
}


static char *
ngx_http_geo_include(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *name)
{
    char       *rv;
    ngx_str_t   file;

    file.len = name->len + 4;
    file.data = ngx_pnalloc(ctx->temp_pool, name->len + 5);
    if (file.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_sprintf(file.data, "%V.bin%Z", name);

    if (ngx_conf_full_name(cf->cycle, &file, 1) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    if (ctx->ranges) {
        ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "include %s", file.data);

        switch (ngx_http_geo_include_binary_base(cf, ctx, &file)) {
        case NGX_OK:
            return NGX_CONF_OK;
        case NGX_ERROR:
            return NGX_CONF_ERROR;
        default:
            break;
        }
    }

    file.len -= 4;
    file.data[file.len] = '\0';

    ctx->include_name = file;

    if (ctx->outside_entries) {
        ctx->allow_binary_include = 0;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_CORE, cf->log, 0, "include %s", file.data);

    rv = ngx_conf_parse(cf, &file);

    ctx->includes++;
    ctx->outside_entries = 0;

    return rv;
}


static ngx_int_t
ngx_http_geo_include_binary_base(ngx_conf_t *cf, ngx_http_geo_conf_ctx_t *ctx,
    ngx_str_t *name)
{
    u_char                     *base, ch;
    time_t                      mtime;
    size_t                      size, len;
    ssize_t                     n;
    uint32_t                    crc32;
    ngx_err_t                   err;
    ngx_int_t                   rc;
    ngx_uint_t                  i;
    ngx_file_t                  file;
    ngx_file_info_t             fi;
    ngx_http_geo_range_t       *range, **ranges;
    ngx_http_geo_header_t      *header;
    ngx_http_variable_value_t  *vv;

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.name = *name;
    file.log = cf->log;

    file.fd = ngx_open_file(name->data, NGX_FILE_RDONLY, 0, 0);
    if (file.fd == NGX_INVALID_FILE) {
        err = ngx_errno;
        if (err != NGX_ENOENT) {
            ngx_conf_log_error(NGX_LOG_CRIT, cf, err,
                               ngx_open_file_n " \"%s\" failed", name->data);
        }
        return NGX_DECLINED;
    }

    if (ctx->outside_entries) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "binary geo range base \"%s\" cannot be mixed with usual entries",
            name->data);
        rc = NGX_ERROR;
        goto done;
    }

    if (ctx->binary_include) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "second binary geo range base \"%s\" cannot be mixed with \"%s\"",
            name->data, ctx->include_name.data);
        rc = NGX_ERROR;
        goto done;
    }

    if (ngx_fd_info(file.fd, &fi) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, ngx_errno,
                           ngx_fd_info_n " \"%s\" failed", name->data);
        goto failed;
    }

    size = (size_t) ngx_file_size(&fi);
    mtime = ngx_file_mtime(&fi);

    ch = name->data[name->len - 4];
    name->data[name->len - 4] = '\0';

    if (ngx_file_info(name->data, &fi) == NGX_FILE_ERROR) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, ngx_errno,
                           ngx_file_info_n " \"%s\" failed", name->data);
        goto failed;
    }

    name->data[name->len - 4] = ch;

    if (mtime < ngx_file_mtime(&fi)) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                           "stale binary geo range base \"%s\"", name->data);
        goto failed;
    }

    base = ngx_palloc(ctx->pool, size);
    if (base == NULL) {
        goto failed;
    }

    n = ngx_read_file(&file, base, size, 0);

    if (n == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, ngx_errno,
                           ngx_read_file_n " \"%s\" failed", name->data);
        goto failed;
    }

    if ((size_t) n != size) {
        ngx_conf_log_error(NGX_LOG_CRIT, cf, 0,
            ngx_read_file_n " \"%s\" returned only %z bytes instead of %z",
            name->data, n, size);
        goto failed;
    }

    header = (ngx_http_geo_header_t *) base;

    if (size < 16 || ngx_memcmp(&ngx_http_geo_header, header, 12) != 0) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
             "incompatible binary geo range base \"%s\"", name->data);
        goto failed;
    }

    ngx_crc32_init(crc32);

    vv = (ngx_http_variable_value_t *) (base + sizeof(ngx_http_geo_header_t));

    while(vv->data) {
        len = ngx_align(sizeof(ngx_http_variable_value_t) + vv->len,
                        sizeof(void *));
        ngx_crc32_update(&crc32, (u_char *) vv, len);
        vv->data += (size_t) base;
        vv = (ngx_http_variable_value_t *) ((u_char *) vv + len);
    }
    ngx_crc32_update(&crc32, (u_char *) vv, sizeof(ngx_http_variable_value_t));
    vv++;

    ranges = (ngx_http_geo_range_t **) vv;

    for (i = 0; i < 0x10000; i++) {
        ngx_crc32_update(&crc32, (u_char *) &ranges[i], sizeof(void *));
        if (ranges[i]) {
            ranges[i] = (ngx_http_geo_range_t *)
                            ((u_char *) ranges[i] + (size_t) base);
        }
    }

    range = (ngx_http_geo_range_t *) &ranges[0x10000];

    while ((u_char *) range < base + size) {
        while (range->value) {
            ngx_crc32_update(&crc32, (u_char *) range,
                             sizeof(ngx_http_geo_range_t));
            range->value = (ngx_http_variable_value_t *)
                               ((u_char *) range->value + (size_t) base);
            range++;
        }
        ngx_crc32_update(&crc32, (u_char *) range, sizeof(void *));
        range = (ngx_http_geo_range_t *) ((u_char *) range + sizeof(void *));
    }

    ngx_crc32_final(crc32);

    if (crc32 != header->crc32) {
        ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                  "CRC32 mismatch in binary geo range base \"%s\"", name->data);
        goto failed;
    }

    ngx_conf_log_error(NGX_LOG_NOTICE, cf, 0,
                       "using binary geo range base \"%s\"", name->data);

    ctx->include_name = *name;
    ctx->binary_include = 1;
    ctx->high.low = ranges;
    rc = NGX_OK;

    goto done;

failed:

    rc = NGX_DECLINED;

done:

    if (ngx_close_file(file.fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, cf->log, ngx_errno,
                      ngx_close_file_n " \"%s\" failed", name->data);
    }

    return rc;
}


static void
ngx_http_geo_create_binary_base(ngx_http_geo_conf_ctx_t *ctx)
{
    u_char                              *p;
    uint32_t                             hash;
    ngx_str_t                            s;
    ngx_uint_t                           i;
    ngx_file_mapping_t                   fm;
    ngx_http_geo_range_t                *r, *range, **ranges;
    ngx_http_geo_header_t               *header;
    ngx_http_geo_variable_value_node_t  *gvvn;

    fm.name = ngx_pnalloc(ctx->temp_pool, ctx->include_name.len + 5);
    if (fm.name == NULL) {
        return;
    }

    ngx_sprintf(fm.name, "%V.bin%Z", &ctx->include_name);

    fm.size = ctx->data_size;
    fm.log = ctx->pool->log;

    ngx_log_error(NGX_LOG_NOTICE, fm.log, 0,
                  "creating binary geo range base \"%s\"", fm.name);

    if (ngx_create_file_mapping(&fm) != NGX_OK) {
        return;
    }

    p = ngx_cpymem(fm.addr, &ngx_http_geo_header,
                   sizeof(ngx_http_geo_header_t));

    p = ngx_http_geo_copy_values(fm.addr, p, ctx->rbtree.root,
                                 ctx->rbtree.sentinel);

    p += sizeof(ngx_http_variable_value_t);

    ranges = (ngx_http_geo_range_t **) p;

    p += 0x10000 * sizeof(ngx_http_geo_range_t *);

    for (i = 0; i < 0x10000; i++) {
        r = ctx->high.low[i];
        if (r == NULL) {
            continue;
        }

        range = (ngx_http_geo_range_t *) p;
        ranges[i] = (ngx_http_geo_range_t *) (p - (u_char *) fm.addr);

        do {
            s.len = r->value->len;
            s.data = r->value->data;
            hash = ngx_crc32_long(s.data, s.len);
            gvvn = (ngx_http_geo_variable_value_node_t *)
                        ngx_str_rbtree_lookup(&ctx->rbtree, &s, hash);

            range->value = (ngx_http_variable_value_t *) gvvn->offset;
            range->start = r->start;
            range->end = r->end;
            range++;

        } while ((++r)->value);

        range->value = NULL;

        p = (u_char *) range + sizeof(void *);
    }

    header = fm.addr;
    header->crc32 = ngx_crc32_long((u_char *) fm.addr
                                       + sizeof(ngx_http_geo_header_t),
                                   fm.size - sizeof(ngx_http_geo_header_t));

    ngx_close_file_mapping(&fm);
}


static u_char *
ngx_http_geo_copy_values(u_char *base, u_char *p, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel)
{
    ngx_http_variable_value_t           *vv;
    ngx_http_geo_variable_value_node_t  *gvvn;

    if (node == sentinel) {
        return p;
    }

    gvvn = (ngx_http_geo_variable_value_node_t *) node;
    gvvn->offset = p - base;

    vv = (ngx_http_variable_value_t *) p;
    *vv = *gvvn->value;
    p += sizeof(ngx_http_variable_value_t);
    vv->data = (u_char *) (p - base);

    p = ngx_cpymem(p, gvvn->sn.str.data, gvvn->sn.str.len);

    p = ngx_align_ptr(p, sizeof(void *));

    p = ngx_http_geo_copy_values(base, p, node->left, sentinel);

    return ngx_http_geo_copy_values(base, p, node->right, sentinel);
}
