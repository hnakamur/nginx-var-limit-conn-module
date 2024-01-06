
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Hiroaki Nakamura
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define NGX_HTTP_VAR_LIMIT_CONN_PASSED            1
#define NGX_HTTP_VAR_LIMIT_CONN_REJECTED          2
#define NGX_HTTP_VAR_LIMIT_CONN_REJECTED_DRY_RUN  3


#define ngx_array_item(a, i) ((void *)((char *)(a)->elts + (a)->size * (i)))


typedef struct {
    u_char                        color;
    u_char                        len;
    u_short                       conn;
    ngx_uint_t                    limit_conn;
    ngx_uint_t                    limited;
    u_char                        data[1];
} ngx_http_var_limit_conn_node_t;


typedef struct {
    ngx_shm_zone_t               *shm_zone;
    ngx_rbtree_node_t            *node;
} ngx_http_var_limit_conn_cleanup_t;


typedef struct {
    ngx_rbtree_t                  rbtree;
    ngx_rbtree_node_t             sentinel;
} ngx_http_var_limit_conn_shctx_t;


typedef struct {
    ngx_http_var_limit_conn_shctx_t  *sh;
    ngx_slab_pool_t                  *shpool;
    ngx_http_complex_value_t          key;
    ngx_http_complex_value_t          conn_var;
    ngx_http_complex_value_t          dry_run_var;
    ngx_http_complex_value_t          status_var;
} ngx_http_var_limit_conn_ctx_t;


typedef struct {
    ngx_shm_zone_t               *shm_zone;
    ngx_uint_t                    conn;
} ngx_http_var_limit_conn_limit_t;


typedef struct {
    ngx_array_t                   limits;
    ngx_uint_t                    log_level;
    ngx_uint_t                    status_code;
    ngx_flag_t                    dry_run;
    ngx_uint_t                    actual_conn_plus_one;
    ngx_uint_t                    config_conn_plus_one;

    ngx_shm_zone_t               *status_shm_zone;
    ngx_uint_t                    default_n;
    ngx_int_t                     arg_n_var_index;
    ngx_int_t                     arg_key_var_index;
} ngx_http_var_limit_conn_conf_t;


typedef struct {
    ngx_str_t                     key;
    u_short                       conn;
    ngx_uint_t                    limit_conn;
} ngx_http_var_limit_conn_status_item_t;


static ngx_int_t ngx_http_var_limit_conn_handler(ngx_http_request_t *r);
static ngx_uint_t ngx_http_var_limit_conn_get_status(ngx_http_request_t *r,
    ngx_http_var_limit_conn_ctx_t *ctx);
static void ngx_http_var_limit_conn_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_rbtree_node_t *ngx_http_var_limit_conn_lookup(ngx_rbtree_t *rbtree,
    ngx_str_t *key, uint32_t hash);
static void ngx_http_var_limit_conn_cleanup(void *data);
static ngx_inline void ngx_http_var_limit_conn_cleanup_all(ngx_pool_t *pool);

static ngx_int_t ngx_http_var_limit_conn_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_var_limit_conn_actual_variable(
    ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_var_limit_conn_config_variable(
    ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_var_limit_conn_u_short_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, ngx_uint_t value, ngx_flag_t has_value);
static void *ngx_http_var_limit_conn_create_conf(ngx_conf_t *cf);
static char *ngx_http_var_limit_conn_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_var_limit_conn_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_var_limit_conn(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_int_t ngx_http_var_limit_conn_top_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_var_limit_conn_add_uint_header(ngx_http_request_t *r,
    const ngx_str_t *lowcase_key, ngx_uint_t value);
static ngx_int_t ngx_http_var_limit_conn_top_build_items(ngx_http_request_t *r,
    ngx_rbtree_t *rbtree, ngx_array_t *items, ngx_uint_t *num_all_keys);
static void ngx_http_var_limit_conn_set_status_item(
    ngx_http_var_limit_conn_status_item_t *item,
    ngx_http_var_limit_conn_node_t *lcn);
static ngx_uint_t ngx_http_var_limit_conn_binary_search(ngx_array_t *items,
    const ngx_http_var_limit_conn_status_item_t *item);
static ngx_int_t ngx_http_var_limit_conn_status_build_response_body(
    ngx_http_request_t *r, ngx_array_t *items, ngx_chain_t *out);
static ngx_int_t ngx_http_var_limit_conn_top_item_cmp(const void *a,
    const void *b);
static char *ngx_http_var_limit_conn_top(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_uint_t ngx_http_var_limit_conn_count_keys(
    ngx_str_t *comma_separated_value);
static ngx_int_t ngx_http_var_limit_conn_next_key(
    ngx_str_t *comma_separated_value, ngx_str_t *key);
static ngx_int_t ngx_http_var_limit_conn_monitor_build_items(
    ngx_http_request_t *r, ngx_rbtree_t *rbtree, ngx_array_t *items);
static ngx_http_var_limit_conn_node_t *ngx_http_var_limit_conn_do_lookup(
    ngx_rbtree_t *rbtree, ngx_str_t *key);
static ngx_int_t ngx_http_var_limit_conn_monitor_handler(ngx_http_request_t *r);
static char *ngx_http_var_limit_conn_monitor(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static ngx_int_t ngx_http_var_limit_conn_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_var_limit_conn_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_var_limit_conn_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_conf_num_bounds_t  ngx_http_var_limit_conn_status_bounds = {
    ngx_conf_check_num_bounds, 400, 599
};


static ngx_command_t  ngx_http_var_limit_conn_commands[] = {

    { ngx_string("var_limit_conn_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2|NGX_CONF_TAKE3|NGX_CONF_TAKE4
          |NGX_CONF_TAKE5,
      ngx_http_var_limit_conn_zone,
      0,
      0,
      NULL },

    { ngx_string("var_limit_conn"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_var_limit_conn,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("var_limit_conn_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_var_limit_conn_conf_t, log_level),
      &ngx_http_var_limit_conn_log_levels },

    { ngx_string("var_limit_conn_status"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_var_limit_conn_conf_t, status_code),
      &ngx_http_var_limit_conn_status_bounds },

    { ngx_string("var_limit_conn_dry_run"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_var_limit_conn_conf_t, dry_run),
      NULL },

    { ngx_string("var_limit_conn_top"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_var_limit_conn_top,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("var_limit_conn_monitor"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_var_limit_conn_monitor,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_var_limit_conn_module_ctx = {
    ngx_http_var_limit_conn_add_variables, /* preconfiguration */
    ngx_http_var_limit_conn_init,          /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_var_limit_conn_create_conf,   /* create location configuration */
    ngx_http_var_limit_conn_merge_conf     /* merge location configuration */
};


ngx_module_t  ngx_http_var_limit_conn_module = {
    NGX_MODULE_V1,
    &ngx_http_var_limit_conn_module_ctx,   /* module context */
    ngx_http_var_limit_conn_commands,      /* module directives */
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


static ngx_http_variable_t  ngx_http_var_limit_conn_vars[] = {

    { ngx_string("var_limit_conn_status"), NULL,
      ngx_http_var_limit_conn_status_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

    { ngx_string("var_limit_conn_actual"), NULL,
      ngx_http_var_limit_conn_actual_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

    { ngx_string("var_limit_conn_config"), NULL,
      ngx_http_var_limit_conn_config_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

      ngx_http_null_variable
};


static ngx_str_t  ngx_http_var_limit_conn_status[] = {
    ngx_string("PASSED"),
    ngx_string("REJECTED"),
    ngx_string("REJECTED_DRY_RUN")
};


#define ngx_str_eq_literal(s1, literal)                                      \
    ((s1)->len = sizeof(literal) - 1                                         \
     && ngx_strncmp((s1)->data, literal, sizeof(literal) - 1) == 0)


static ngx_int_t
ngx_http_var_limit_conn_handler(ngx_http_request_t *r)
{
    size_t                              n;
    uint32_t                            hash;
    ngx_str_t                           key, conn_var, dry_run_var;
    ngx_uint_t                          i, conn;
    ngx_int_t                           conn2;
    ngx_rbtree_node_t                  *node;
    ngx_pool_cleanup_t                 *cln;
    ngx_http_var_limit_conn_ctx_t      *ctx;
    ngx_http_var_limit_conn_node_t     *lc;
    ngx_http_var_limit_conn_conf_t     *lccf;
    ngx_http_var_limit_conn_limit_t    *limits;
    ngx_http_var_limit_conn_cleanup_t  *lccln;
    ngx_flag_t                          dry_run;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);

    /* skip already handled or called for var_limit_conn_top directive. */
    if (r->main->limit_conn_status || lccf->status_shm_zone != NULL) {
        return NGX_DECLINED;
    }

    limits = lccf->limits.elts;

    for (i = 0; i < lccf->limits.nelts; i++) {
        ctx = limits[i].shm_zone->data;
        conn = limits[i].conn;
        if (ngx_http_complex_value(r, &ctx->conn_var, &conn_var) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                            "cannot get value of \"%V\" parameter of "
                            "var_limit_conn_zone",
                            &ctx->conn_var);
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (ngx_str_eq_literal(&conn_var, "unlimited")) {
            conn = 65535 + 1;

        } else if (conn_var.len != 0) {
            conn2 = ngx_atoi(conn_var.data, conn_var.len);
            if (conn2 <= 0) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                            "invalid conn_var value \"%V\"", &conn_var);
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            conn = (ngx_uint_t) conn2;
        }
        lccf->config_conn_plus_one = conn + 1;
        lccf->actual_conn_plus_one = 0;

        if (ngx_http_complex_value(r, &ctx->key, &key) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (key.len == 0) {
            continue;
        }

        if (key.len > 255) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "the value of the \"%V\" key "
                          "is more than 255 bytes: \"%V\"",
                          &ctx->key.value, &key);
            continue;
        }

        r->main->limit_conn_status = NGX_HTTP_VAR_LIMIT_CONN_PASSED;

        hash = ngx_crc32_short(key.data, key.len);

        dry_run = lccf->dry_run;
        if (ngx_http_complex_value(r, &ctx->dry_run_var, &dry_run_var)
            != NGX_OK)
        {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (dry_run_var.len != 0) {
            if (ngx_strcasecmp(dry_run_var.data, (u_char *) "on") == 0) {
                dry_run = 1;

            } else if (ngx_strcasecmp(dry_run_var.data, (u_char *) "off")
                       == 0)
            {
                dry_run = 0;

            } else {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "the value of the \"%V\" key "
                              "must be \"on\" or \"off\": \"%V\"",
                              &ctx->dry_run_var.value, &dry_run_var);
                continue;
            }
        }

        ngx_shmtx_lock(&ctx->shpool->mutex);

        node = ngx_http_var_limit_conn_lookup(&ctx->sh->rbtree, &key, hash);

        if (node == NULL) {
            n = offsetof(ngx_rbtree_node_t, color)
                + offsetof(ngx_http_var_limit_conn_node_t, data)
                + key.len;

            node = ngx_slab_alloc_locked(ctx->shpool, n);

            if (node == NULL) {
                ngx_shmtx_unlock(&ctx->shpool->mutex);
                ngx_http_var_limit_conn_cleanup_all(r->pool);

                lccf->actual_conn_plus_one = 0 + 1;

                if (dry_run) {
                    r->main->limit_conn_status =
                                       NGX_HTTP_VAR_LIMIT_CONN_REJECTED_DRY_RUN;
                    return NGX_DECLINED;
                }

                r->main->limit_conn_status = NGX_HTTP_VAR_LIMIT_CONN_REJECTED;

                return ngx_http_var_limit_conn_get_status(r, ctx);
            }

            lc = (ngx_http_var_limit_conn_node_t *) &node->color;

            node->key = hash;
            lc->len = (u_char) key.len;
            lc->conn = 1;
            lc->limit_conn = conn;
            ngx_memcpy(lc->data, key.data, key.len);

            ngx_rbtree_insert(&ctx->sh->rbtree, node);

            lccf->actual_conn_plus_one = 1 + 1;

        } else {

            lc = (ngx_http_var_limit_conn_node_t *) &node->color;
            lccf->actual_conn_plus_one = lc->conn + 1 + 1;

            if ((ngx_uint_t) lc->conn >= conn) {

                ngx_shmtx_unlock(&ctx->shpool->mutex);

                ngx_log_error(lccf->log_level, r->connection->log, 0,
                              "limiting connections%s by zone \"%V\"",
                              dry_run ? ", dry run," : "",
                              &limits[i].shm_zone->shm.name);

                ngx_http_var_limit_conn_cleanup_all(r->pool);

                if (dry_run) {
                    r->main->limit_conn_status =
                                       NGX_HTTP_VAR_LIMIT_CONN_REJECTED_DRY_RUN;
                    return NGX_DECLINED;
                }

                r->main->limit_conn_status = NGX_HTTP_VAR_LIMIT_CONN_REJECTED;

                return ngx_http_var_limit_conn_get_status(r, ctx);
            }

            lc->conn++;
            lc->limit_conn = conn;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit conn: %08Xi %d", node->key, lc->conn);

        ngx_shmtx_unlock(&ctx->shpool->mutex);

        cln = ngx_pool_cleanup_add(r->pool,
                                   sizeof(ngx_http_var_limit_conn_cleanup_t));
        if (cln == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        cln->handler = ngx_http_var_limit_conn_cleanup;
        lccln = cln->data;

        lccln->shm_zone = limits[i].shm_zone;
        lccln->node = node;
    }

    return NGX_DECLINED;
}


static ngx_uint_t
ngx_http_var_limit_conn_get_status(ngx_http_request_t *r,
    ngx_http_var_limit_conn_ctx_t *ctx)
{
    ngx_http_var_limit_conn_conf_t     *lccf;
    ngx_str_t                           status_var;
    ngx_int_t                           status;

    if (ngx_http_complex_value(r, &ctx->status_var, &status_var) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    if (status_var.len != 0) {
        status = ngx_atoi(status_var.data, status_var.len);
        if (status >= 400 && status <= 599) {
            return (ngx_uint_t) status;
        }
    }
    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);
    return lccf->status_code;
}


static void
ngx_http_var_limit_conn_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t               **p;
    ngx_http_var_limit_conn_node_t   *lcn, *lcnt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lcn = (ngx_http_var_limit_conn_node_t *) &node->color;
            lcnt = (ngx_http_var_limit_conn_node_t *) &temp->color;

            p = (ngx_memn2cmp(lcn->data, lcnt->data, lcn->len, lcnt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_rbtree_node_t *
ngx_http_var_limit_conn_lookup(ngx_rbtree_t *rbtree, ngx_str_t *key,
    uint32_t hash)
{
    ngx_int_t                        rc;
    ngx_rbtree_node_t               *node, *sentinel;
    ngx_http_var_limit_conn_node_t  *lcn;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lcn = (ngx_http_var_limit_conn_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, lcn->data, key->len, (size_t) lcn->len);

        if (rc == 0) {
            return node;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


static void
ngx_http_var_limit_conn_cleanup(void *data)
{
    ngx_http_var_limit_conn_cleanup_t  *lccln = data;

    ngx_rbtree_node_t               *node;
    ngx_http_var_limit_conn_ctx_t   *ctx;
    ngx_http_var_limit_conn_node_t  *lc;

    ctx = lccln->shm_zone->data;
    node = lccln->node;
    lc = (ngx_http_var_limit_conn_node_t *) &node->color;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, lccln->shm_zone->shm.log, 0,
                   "limit conn cleanup: %08Xi %d", node->key, lc->conn);

    lc->conn--;

    if (lc->conn == 0) {
        ngx_rbtree_delete(&ctx->sh->rbtree, node);
        ngx_slab_free_locked(ctx->shpool, node);
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);
}


static ngx_inline void
ngx_http_var_limit_conn_cleanup_all(ngx_pool_t *pool)
{
    ngx_pool_cleanup_t  *cln;

    cln = pool->cleanup;

    while (cln && cln->handler == ngx_http_var_limit_conn_cleanup) {
        ngx_http_var_limit_conn_cleanup(cln->data);
        cln = cln->next;
    }

    pool->cleanup = cln;
}


static ngx_int_t
ngx_http_var_limit_conn_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_var_limit_conn_ctx_t  *octx = data;

    size_t                          len;
    ngx_http_var_limit_conn_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
        if (ctx->key.value.len != octx->key.value.len
            || ngx_strncmp(ctx->key.value.data, octx->key.value.data,
                           ctx->key.value.len)
               != 0)
        {
            ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                          "var_limit_conn_zone \"%V\" uses the \"%V\" key "
                          "while previously it used the \"%V\" key",
                          &shm_zone->shm.name, &ctx->key.value,
                          &octx->key.value);
            return NGX_ERROR;
        }

        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;

        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;

        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool,
                             sizeof(ngx_http_var_limit_conn_shctx_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_http_var_limit_conn_rbtree_insert_value);

    len = sizeof(" in var_limit_conn_zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in var_limit_conn_zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_conn_status_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_str_t  *status;

    if (r->main->limit_conn_status == 0) {
        v->not_found = 1;
        return NGX_OK;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    status = &ngx_http_var_limit_conn_status[r->main->limit_conn_status - 1];
    v->len = status->len;
    v->data = status->data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_conn_actual_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_var_limit_conn_conf_t  *lccf;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);
    return ngx_http_var_limit_conn_u_short_variable(r, v,
        lccf->actual_conn_plus_one - 1, lccf->actual_conn_plus_one);
}


static ngx_int_t
ngx_http_var_limit_conn_config_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_var_limit_conn_conf_t  *lccf;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);
    return ngx_http_var_limit_conn_u_short_variable(r, v,
        lccf->config_conn_plus_one - 1, lccf->config_conn_plus_one);
}


static ngx_int_t
ngx_http_var_limit_conn_u_short_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, ngx_uint_t value, ngx_flag_t has_value)
{
    u_char     *p, buf[sizeof("65535")];
    ngx_str_t   src;

    if (!has_value) {
        v->not_found = 1;
        return NGX_OK;
    }

    p = ngx_snprintf(buf, sizeof(buf), "%d", value);
    src.data = buf;
    src.len = p - buf;
    v->len = src.len;
    v->data = ngx_pstrdup(r->pool, &src);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


static void *
ngx_http_var_limit_conn_create_conf(ngx_conf_t *cf)
{
    ngx_http_var_limit_conn_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_var_limit_conn_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->limits.elts = NULL;
     */

    conf->log_level = NGX_CONF_UNSET_UINT;
    conf->status_code = NGX_CONF_UNSET_UINT;
    conf->dry_run = NGX_CONF_UNSET;

    conf->status_shm_zone = NGX_CONF_UNSET_PTR;

    return conf;
}


static char *
ngx_http_var_limit_conn_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_var_limit_conn_conf_t *prev = parent;
    ngx_http_var_limit_conn_conf_t *conf = child;

    if (conf->limits.elts == NULL) {
        conf->limits = prev->limits;
    }

    ngx_conf_merge_uint_value(conf->log_level, prev->log_level, NGX_LOG_ERR);
    ngx_conf_merge_uint_value(conf->status_code, prev->status_code,
                              NGX_HTTP_SERVICE_UNAVAILABLE);

    ngx_conf_merge_value(conf->dry_run, prev->dry_run, 0);

    ngx_conf_merge_ptr_value(conf->status_shm_zone, prev->status_shm_zone, NULL);

    return NGX_CONF_OK;
}


static char *
ngx_http_var_limit_conn_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                            *p;
    ssize_t                            size;
    ngx_str_t                         *value, name, s;
    ngx_uint_t                         i;
    ngx_shm_zone_t                    *shm_zone;
    ngx_http_var_limit_conn_ctx_t     *ctx;
    ngx_http_compile_complex_value_t   ccv;

    value = cf->args->elts;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_var_limit_conn_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

    ccv.cf = cf;
    ccv.value = &value[1];
    ccv.complex_value = &ctx->key;

    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    size = 0;
    name.len = 0;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "zone=", 5) == 0) {

            name.data = value[i].data + 5;

            p = (u_char *) ngx_strchr(name.data, ':');

            if (p == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            name.len = p - name.data;

            s.data = p + 1;
            s.len = value[i].data + value[i].len - s.data;

            size = ngx_parse_size(&s);

            if (size == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid zone size \"%V\"", &value[i]);
                return NGX_CONF_ERROR;
            }

            if (size < (ssize_t) (8 * ngx_pagesize)) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "zone \"%V\" is too small", &value[i]);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "conn_var=", 9) == 0) {

            s.data = value[i].data + 9;
            s.len = value[i].len - 9;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->conn_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "dry_run_var=", 12) == 0) {

            s.data = value[i].data + 12;
            s.len = value[i].len - 12;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->dry_run_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "status_var=", 11) == 0) {

            s.data = value[i].data + 11;
            s.len = value[i].len - 11;

            ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

            ccv.cf = cf;
            ccv.value = &s;
            ccv.complex_value = &ctx->status_var;

            if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (name.len == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"zone\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, &name, size,
                                     &ngx_http_var_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (shm_zone->data) {
        ctx = shm_zone->data;

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "%V \"%V\" is already bound to key \"%V\"",
                           &cmd->name, &name, &ctx->key.value);
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_var_limit_conn_init_zone;
    shm_zone->data = ctx;

    return NGX_CONF_OK;
}


static char *
ngx_http_var_limit_conn(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_shm_zone_t                   *shm_zone;
    ngx_http_var_limit_conn_conf_t   *lccf = conf;
    ngx_http_var_limit_conn_limit_t  *limit, *limits;

    ngx_str_t  *value;
    ngx_int_t   n;
    ngx_uint_t  i;

    value = cf->args->elts;

    shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                     &ngx_http_var_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    limits = lccf->limits.elts;

    if (limits == NULL) {
        if (ngx_array_init(&lccf->limits, cf->pool, 1,
                           sizeof(ngx_http_var_limit_conn_limit_t))
            != NGX_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    for (i = 0; i < lccf->limits.nelts; i++) {
        if (shm_zone == limits[i].shm_zone) {
            return "is duplicate";
        }
    }

    n = ngx_atoi(value[2].data, value[2].len);
    if (n <= 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid number of connections \"%V\"", &value[2]);
        return NGX_CONF_ERROR;
    }

    if (n > 65535) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "connection limit must be less 65536");
        return NGX_CONF_ERROR;
    }

    limit = ngx_array_push(&lccf->limits);
    if (limit == NULL) {
        return NGX_CONF_ERROR;
    }

    limit->conn = n;
    limit->shm_zone = shm_zone;

    return NGX_CONF_OK;
}


static ngx_str_t  top_n_name = ngx_string("x-top-n");
static ngx_str_t  num_all_keys_name = ngx_string("x-num-all-keys");


static ngx_int_t
ngx_http_var_limit_conn_top_handler(ngx_http_request_t *r)
{
    ngx_http_var_limit_conn_conf_t   *lccf;
    ngx_shm_zone_t                   *shm_zone;
    ngx_http_var_limit_conn_ctx_t     *ctx;
    ngx_int_t                         rc;
    ngx_chain_t                       out;
    ngx_array_t                       items;
    ngx_uint_t                        num_all_keys;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);
    if (lccf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    shm_zone = lccf->status_shm_zone;
    if (shm_zone == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_lowcase = NULL;

    ctx = shm_zone->data;
    ngx_shmtx_lock(&ctx->shpool->mutex);

    rc = ngx_http_var_limit_conn_top_build_items(r, &ctx->sh->rbtree, &items,
                                                 &num_all_keys);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_var_limit_conn_status_build_response_body(r, &items, &out);

    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_var_limit_conn_add_uint_header(r, &num_all_keys_name,
                                                 num_all_keys);
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_var_limit_conn_add_uint_header(r, &top_n_name, items.nelts);
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static ngx_int_t
ngx_http_var_limit_conn_add_uint_header(ngx_http_request_t *r,
    const ngx_str_t *lowcase_key, ngx_uint_t value)
{
    ngx_table_elt_t                *h;

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    h->hash = 1;
    h->key = *lowcase_key;
    h->lowcase_key = lowcase_key->data;
    h->value.data = ngx_pnalloc(r->pool, NGX_SIZE_T_LEN + 1);
    if (h->value.data == NULL) {
        h->hash = 0;
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    h->value.len = ngx_sprintf(h->value.data, "%d", value)
                   - h->value.data;
    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_conn_top_build_items(ngx_http_request_t *r,
    ngx_rbtree_t *rbtree, ngx_array_t *items, ngx_uint_t *num_all_keys)
{
    ngx_http_var_limit_conn_conf_t         *lccf;
    ngx_rbtree_node_t                      *root, *sentinel, *node;
    ngx_http_var_limit_conn_node_t         *lcn;
    ngx_http_variable_value_t              *arg_n_val;
    ngx_int_t                               rc, arg_n;
    ngx_uint_t                              i, top_n, old_nelts;
    ngx_http_var_limit_conn_status_item_t  *item, tmp_item;

    *num_all_keys = 0;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);

    top_n = lccf->default_n;
    arg_n_val = ngx_http_get_flushed_variable(r, lccf->arg_n_var_index);
    if (arg_n_val->valid) {
        arg_n = ngx_atoi(arg_n_val->data, arg_n_val->len);
        if (arg_n != NGX_ERROR) {
            top_n = (ngx_uint_t)arg_n;
        }
    }

    rc = ngx_array_init(items, r->pool, 16,
                        sizeof(ngx_http_var_limit_conn_status_item_t));
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    sentinel = rbtree->sentinel;
    root = rbtree->root;

    if (root == sentinel) {
        return NGX_OK;
    }

    /* keep only top n items whlie looping using binary search */
    for (node = ngx_rbtree_min(root, sentinel);
         node;
         node = ngx_rbtree_next(rbtree, node))
    {
        lcn = (ngx_http_var_limit_conn_node_t *) &node->color;
        ++*num_all_keys;

        ngx_http_var_limit_conn_set_status_item(&tmp_item, lcn);
        i = ngx_http_var_limit_conn_binary_search(items, &tmp_item);
        if (i > top_n) {
            continue;
        }

        old_nelts = items->nelts;
        if (old_nelts < top_n) {
            /*
             * no error or realloc happens since we initialized items with
             * top_n capacity.
             */
            item = ngx_array_push(items);
            /*
             * ex. top_n = 5, old_nelts = 4, i = 1
             *
             * | a | b | c | d |
             *       ^i
             */
            if (i < old_nelts) {
                ngx_memmove(ngx_array_item(items, i + 1),
                            ngx_array_item(items, i),
                            items->size * (old_nelts - i));
            }

        } else {
            item = ngx_array_item(items, i);

            /*
             * ex2. top_n = 5, old_nelts = 5, i = 1
             *
             * | a | b | c | d | e |
             *       ^i
             */
            if (i < old_nelts) {
                ngx_memmove(ngx_array_item(items, i + 1),
                            ngx_array_item(items, i),
                            items->size * (old_nelts - i - 1));
            }
        }

        *item = tmp_item;
    }

    /* Copy top n entries keys */
    for (i = 0; i < items->nelts; ++i) {
        item = ngx_array_item(items, i);
        item->key.data = ngx_pstrdup(r->pool, &item->key);
        if (item->key.data == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    return NGX_OK;
}


static void
ngx_http_var_limit_conn_set_status_item(
    ngx_http_var_limit_conn_status_item_t *item,
    ngx_http_var_limit_conn_node_t *lcn)
{
    item->conn = lcn->conn;
    item->limit_conn = lcn->limit_conn;
    item->key.len = lcn->len;
    item->key.data = lcn->data;
}


static ngx_uint_t
ngx_http_var_limit_conn_binary_search(ngx_array_t *items,
    const ngx_http_var_limit_conn_status_item_t *item)
{
    ngx_uint_t                                   min_index, one_past_max_index,
                                                 index;
    ngx_int_t                                    res;
    const ngx_http_var_limit_conn_status_item_t  *item2;

    min_index = 0;
    one_past_max_index = items->nelts;
    while (one_past_max_index != min_index) {
        index = (min_index + one_past_max_index) / 2;
        item2 = ngx_array_item(items, index);
        res = ngx_http_var_limit_conn_top_item_cmp(item, item2);
        if (res == 0) {
            return index;
        }
        if (res < 0) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}


static ngx_int_t
ngx_http_var_limit_conn_status_build_response_body(ngx_http_request_t *r,
    ngx_array_t *items, ngx_chain_t *out)
{
    ngx_uint_t                           i;
    ngx_http_var_limit_conn_status_item_t  *item, *items_ptr;
    size_t                               buf_size = 0;
    ngx_buf_t                           *b;

    if (items->nelts == 0) {
        r->header_only = 1;
        r->headers_out.status = NGX_HTTP_NO_CONTENT;
        return NGX_OK;
    }

    items_ptr = items->elts;
    for (i = 0; i < items->nelts; i++) {
        item = &items_ptr[i];
        buf_size += sizeof("key:") + item->key.len
                    + sizeof("\tconn:65535\tlimit:65536\n");
    }

    b = ngx_create_temp_buf(r->pool, buf_size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    for (i = 0; i < items->nelts; i++) {
        item = &items_ptr[i];
        b->last = ngx_sprintf(b->last, "key:%V\tconn:%d\tlimit:%d\n",
            &item->key, item->conn, item->limit_conn);
    }

    b->last_buf = 1;
    b->last_in_chain = 1;

    out->buf = b;
    out->next = NULL;
    b->last_buf = 1;
    r->headers_out.content_length_n = b->last - b->pos;
    r->headers_out.status = NGX_HTTP_OK;
    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_conn_top_item_cmp(const void *a, const void *b)
{
    const ngx_http_var_limit_conn_status_item_t  *item_a, *item_b;
    size_t                                     n;
    ngx_int_t                                  rc;

    item_a = a;
    item_b = b;

    /* order by conn desc, key asc */

    if (item_a->conn > item_b->conn) {
        return -1;
    }
    if (item_a->conn < item_b->conn) {
        return 1;
    }

    n = ngx_min(item_a->key.len, item_b->key.len);
    rc = ngx_strncmp(item_a->key.data, item_b->key.data, n);
    if (rc) {
        return rc;
    }
    if (item_a->key.len > item_b->key.len) {
        return 1;
    }
    if (item_a->key.len < item_b->key.len) {
        return -1;
    }
    return 0;
}


static char *
ngx_http_var_limit_conn_top(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_shm_zone_t                   *shm_zone;
    ngx_http_var_limit_conn_conf_t   *lccf = conf;
    ngx_http_core_loc_conf_t         *clcf;
    ngx_uint_t                        i;
    ngx_str_t                         default_n_str;
    ngx_int_t                         default_n = 0;
    static ngx_str_t                  arg_n_name = ngx_string("arg_n");

    ngx_str_t  *value;

    value = cf->args->elts;

    shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                     &ngx_http_var_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    lccf->status_shm_zone = shm_zone;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "default_n=", 10) == 0) {

            default_n_str.data = value[i].data + 10;
            default_n_str.len = value[i].len - 10;

            default_n = ngx_atoi(default_n_str.data, default_n_str.len);
            if (default_n == NGX_ERROR || default_n <= 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid default_n \"%V\"", &default_n_str);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    if (default_n == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must have \"default_n\" parameter",
                           &cmd->name);
        return NGX_CONF_ERROR;
    }

    lccf->default_n = (ngx_uint_t) default_n;
    lccf->arg_n_var_index = ngx_http_get_variable_index(cf, &arg_n_name);

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_var_limit_conn_top_handler;

    return NGX_CONF_OK;
}


static ngx_uint_t
ngx_http_var_limit_conn_count_keys(ngx_str_t *comma_separated_value)
{
    u_char      *p, *start, *last;
    ngx_uint_t   n = 0;

    p = comma_separated_value->data;
    start = p;
    last = p + comma_separated_value->len;

    while (p < last) {

        if (*p == ',') {
            if (p > start) {
                ++n;
            }
            start = p + 1;
        }

        p++;
    }

    if (p > start) {
        ++n;
    }

    return n;
}


static ngx_int_t
ngx_http_var_limit_conn_next_key(ngx_str_t *comma_separated_value,
    ngx_str_t *key)
{
    u_char  *p, *start, *last;

    last = comma_separated_value->data + comma_separated_value->len;
    start = key->data == NULL
                ? comma_separated_value->data
                : key->data + key->len + 1;
    p = start;

    while (p < last) {

        if (*p == ',') {
            if (p > start) {
                key->data = start;
                key->len = p - start;
                return 1;
            }
            start = p + 1;
        }

        p++;
    }

    if (p > start) {
        key->data = start;
        key->len = p - start;
        return 1;
    }

    return 0;
}


static ngx_int_t
ngx_http_var_limit_conn_monitor_build_items(ngx_http_request_t *r,
    ngx_rbtree_t *rbtree, ngx_array_t *items)
{
    ngx_http_var_limit_conn_conf_t         *lccf;
    ngx_http_variable_value_t              *arg_key_val;
    ngx_str_t                               keys, key;
    ngx_int_t                               rc;
    ngx_rbtree_node_t                      *root, *sentinel;
    ngx_uint_t                              key_count = 0;
    ngx_http_var_limit_conn_node_t         *lr;
    ngx_http_var_limit_conn_status_item_t  *item;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);
    ngx_str_null(&keys);
    arg_key_val = ngx_http_get_flushed_variable(r, lccf->arg_key_var_index);
    if (arg_key_val->valid) {
        keys.data = arg_key_val->data;
        keys.len = arg_key_val->len;
        key_count = ngx_http_var_limit_conn_count_keys(&keys);
    }

    rc = ngx_array_init(items, r->pool, key_count,
                        sizeof(ngx_http_var_limit_conn_status_item_t));
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    sentinel = rbtree->sentinel;
    root = rbtree->root;
    if (root == sentinel) {
        return NGX_OK;
    }

    ngx_str_null(&key);
    while (ngx_http_var_limit_conn_next_key(&keys, &key)) {
        lr = ngx_http_var_limit_conn_do_lookup(rbtree, &key);
        if (lr == NULL) {
            continue;
        }

        /*
         * ngx_array_push below always succeeds since we allocate enough items
         * in ngx_array_init above.
         */
        item = ngx_array_push(items);
        ngx_http_var_limit_conn_set_status_item(item, lr);
    }

    return NGX_OK;
}

static ngx_http_var_limit_conn_node_t *
ngx_http_var_limit_conn_do_lookup(ngx_rbtree_t *rbtree, ngx_str_t *key)
{
    uint32_t                         hash;
    ngx_rbtree_node_t               *node, *sentinel;
    ngx_http_var_limit_conn_node_t   *lr;
    ngx_int_t                        rc;

    hash = ngx_crc32_short(key->data, key->len);

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        lr = (ngx_http_var_limit_conn_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, lr->data, key->len, (size_t) lr->len);

        if (rc == 0) {
            return lr;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


static ngx_int_t
ngx_http_var_limit_conn_monitor_handler(ngx_http_request_t *r)
{
    ngx_http_var_limit_conn_conf_t  *lccf;
    ngx_shm_zone_t                  *shm_zone;
    ngx_http_var_limit_conn_ctx_t    *ctx;
    ngx_int_t                        rc;
    ngx_chain_t                      out;
    ngx_array_t                      items;

    lccf = ngx_http_get_module_loc_conf(r, ngx_http_var_limit_conn_module);
    if (lccf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    shm_zone = lccf->status_shm_zone;
    if (shm_zone == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_lowcase = NULL;

    ctx = shm_zone->data;
    ngx_shmtx_lock(&ctx->shpool->mutex);

    rc = ngx_http_var_limit_conn_monitor_build_items(r, &ctx->sh->rbtree,
                                                     &items);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_var_limit_conn_status_build_response_body(r, &items, &out);

    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static char *
ngx_http_var_limit_conn_monitor(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_shm_zone_t                   *shm_zone;
    ngx_http_var_limit_conn_conf_t   *lccf = conf;
    ngx_http_core_loc_conf_t         *clcf;
    static ngx_str_t                  arg_key_name = ngx_string("arg_key");

    ngx_str_t  *value;

    value = cf->args->elts;

    shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
                                     &ngx_http_var_limit_conn_module);
    if (shm_zone == NULL) {
        return NGX_CONF_ERROR;
    }

    lccf->status_shm_zone = shm_zone;

    lccf->arg_key_var_index = ngx_http_get_variable_index(cf, &arg_key_name);

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_var_limit_conn_monitor_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_var_limit_conn_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_var_limit_conn_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_var_limit_conn_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_var_limit_conn_handler;

    return NGX_OK;
}
