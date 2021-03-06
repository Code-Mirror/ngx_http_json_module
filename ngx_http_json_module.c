#include <jansson.h>
#include <ngx_http.h>

ngx_module_t ngx_http_json_module;

typedef struct {
    ngx_flag_t read_request_body;
} ngx_http_json_main_conf_t;

typedef struct {
    ngx_flag_t done;
    ngx_flag_t waiting_more_body;
    ngx_uint_t content_type;
} ngx_http_json_ctx_t;

typedef struct {
    ngx_uint_t index;
    ngx_uint_t nelts;
} ngx_http_json_var_index_nelts_t;

typedef struct {
    ngx_http_complex_value_t cv;
    ngx_str_t command;
    ngx_str_t name;
    ngx_str_t value;
    ngx_uint_t index;
    ngx_uint_t json;
    uintptr_t escape;
} ngx_http_json_var_field_t;

typedef struct {
    ngx_conf_t *cf;
    ngx_array_t *fields;
} ngx_http_json_var_ctx_t;

typedef struct {
    ngx_str_t key;
    ngx_array_t value;
} ngx_http_json_key_value_t;

static void ngx_http_json_json_object_clear(json_t *json) {
    (int)json_object_clear(json);
}

enum {
    NGX_JSON_OBJECT = JSON_OBJECT,
    NGX_JSON_ARRAY = JSON_ARRAY,
    NGX_JSON_STRING = JSON_STRING,
    NGX_JSON_INTEGER = JSON_INTEGER,
    NGX_JSON_REAL = JSON_REAL,
    NGX_JSON_TRUE = JSON_TRUE,
    NGX_JSON_FALSE = JSON_FALSE,
    NGX_JSON_NULL = JSON_NULL,
    NGX_JSON_LOADS
};

enum {
    NGX_JSON_APPLICATION_JSON,
    NGX_JSON_MULTIPART_FORM_DATA,
    NGX_JSON_APPLICATION_X_WWW_FORM_URLENCODED
};

static ngx_str_t *ngx_http_json_value(ngx_http_request_t *r, ngx_array_t *array, ngx_str_t *key) {
    ngx_http_json_key_value_t *elts = array->elts;
    ngx_uint_t j;
    for (j = 0; j < array->nelts; j++) if (key->len == elts[j].key.len && !ngx_strncasecmp(key->data, elts[j].key.data, key->len)) { elts = &elts[j]; break; }
    if (j == array->nelts) elts = NULL;
    if (!elts) {
        if (!(elts = ngx_array_push(array))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_push"); return NULL; }
        ngx_memzero(elts, sizeof(ngx_http_json_key_value_t));
    }
    if (!elts->value.elts && ngx_array_init(&elts->value, r->pool, 1, sizeof(ngx_str_t)) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_array_init != NGX_OK"); return NULL; }
    ngx_str_t *value = ngx_array_push(&elts->value);
    if (!value) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_push"); return NULL; }
    elts->key = *key;
    return value;
}

static ngx_array_t *ngx_http_json_headers_array(ngx_http_request_t *r, ngx_list_part_t *part) {
    ngx_array_t *array = ngx_array_create(r->pool, 4, sizeof(ngx_http_json_key_value_t));
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_create"); return NULL; }
    for (; part; part = part->next) {
        ngx_table_elt_t *elts = part->elts;
        for (ngx_uint_t i = 0; i < part->nelts; i++) {
            if (!elts[i].key.len) continue;
            if (!elts[i].value.len) continue;
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "elts[%i] = %V:%V", i, &elts[i].key, &elts[i].value);
            ngx_str_t *value = ngx_http_json_value(r, array, &elts[i].key);
            if (!value) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_value"); return NULL; }
            *value = elts[i].value;
        }
    }
    return array;
}

static size_t ngx_http_json_len(ngx_http_request_t *r, ngx_array_t *array) {
    size_t len = 0;
    ngx_http_json_key_value_t *elts = array->elts;
    for (ngx_uint_t i = 0; i < array->nelts; i++) {
        if (i) len += sizeof(",") - 1;
        len += sizeof("\"\":") - 1 + elts[i].key.len + ngx_escape_json(NULL, elts[i].key.data, elts[i].key.len);
        ngx_str_t *value = elts[i].value.elts;
        switch (elts[i].value.nelts) {
            case 1: {
                if (!value[0].data) len += sizeof("null") - 1;
                else len += sizeof("\"\"") - 1 + value[0].len + ngx_escape_json(NULL, value[0].data, value[0].len);
            } break;
            default: {
                len += sizeof("[]") - 1;
                for (ngx_uint_t j = 0; j < elts[i].value.nelts; j++) {
                    if (j) len += sizeof(",") - 1;
                    if (!value[j].data) len += sizeof("null") - 1;
                    else len += sizeof("\"\"") - 1 + value[j].len + ngx_escape_json(NULL, value[j].data, value[j].len);
                }
            } break;
        }
    }
    len += sizeof("{}") - 1;
    return len;
}

static u_char *ngx_http_json_data(ngx_http_request_t *r, ngx_array_t *array, u_char *p) {
    *p++ = '{';
    ngx_http_json_key_value_t *elts = array->elts;
    for (ngx_uint_t i = 0; i < array->nelts; i++) {
        if (i) *p++ = ',';
        *p++ = '"';
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "elts[%i].key = %V", i, &elts[i].key);
        p = (u_char *)ngx_escape_json(p, elts[i].key.data, elts[i].key.len);
        *p++ = '"'; *p++ = ':';
        ngx_str_t *value = elts[i].value.elts;
        switch (elts[i].value.nelts) {
            case 1: {
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "value = %V", &value[0]);
                if (!value[0].data) p = ngx_copy(p, "null", sizeof("null") - 1); else {
                    *p++ = '"';
                    p = (u_char *)ngx_escape_json(p, value[0].data, value[0].len);
                    *p++ = '"';
                }
            } break;
            default: {
                *p++ = '[';
                for (ngx_uint_t j = 0; j < elts[i].value.nelts; j++) {
                    if (j) *p++ = ',';
                    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "value[%i] = %V", j, &value[j]);
                    if (!value[j].data) p = ngx_copy(p, "null", sizeof("null") - 1); else {
                        *p++ = '"';
                        p = (u_char *)ngx_escape_json(p, value[j].data, value[j].len);
                        *p++ = '"';
                    }
                }
                *p++ = ']';
            } break;
        }
    }
    *p++ = '}';
    return p;
}

static ngx_int_t ngx_http_json_response_headers(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_array_t *array = ngx_http_json_headers_array(r, &r->headers_out.headers.part);
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_headers_array"); return NGX_ERROR; }
    v->len = ngx_http_json_len(r, array);
    if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_data(r, array, v->data) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_data != v->data + v->len"); return NGX_ERROR; }
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static ngx_int_t ngx_http_json_headers(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_array_t *array = ngx_http_json_headers_array(r, &r->headers_in.headers.part);
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_headers_array"); return NGX_ERROR; }
    v->len = ngx_http_json_len(r, array);
    if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_data(r, array, v->data) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_data != v->data + v->len"); return NGX_ERROR; }
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static ngx_int_t ngx_http_json_loads(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_uint_t index = (ngx_uint_t)data;
    ngx_http_variable_value_t *vv = ngx_http_get_indexed_variable(r, index);
    if (!vv) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_get_indexed_variable"); return NGX_ERROR; }
    if (!vv->data) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!vv->data"); return NGX_ERROR; }
    if (!vv->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!vv->len"); return NGX_ERROR; }
    json_error_t error;
    json_t *json = json_loadb((char *)vv->data, vv->len, JSON_DECODE_ANY, &error);
    if (!json) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_loadb: %s", error.text); return NGX_ERROR; }
    ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
    if (!cln) { ngx_http_json_json_object_clear(json); ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pool_cleanup_add"); return NGX_ERROR; }
    cln->handler = (ngx_pool_cleanup_pt)ngx_http_json_json_object_clear;
    cln->data = json;
    v->data = (u_char *)json;
    v->len = sizeof(json_t);
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static ngx_array_t *ngx_http_json_cookies_array(ngx_http_request_t *r) {
    ngx_array_t *array = ngx_array_create(r->pool, 4, sizeof(ngx_http_json_key_value_t));
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_create"); return NULL; }
    ngx_table_elt_t **elts = r->headers_in.cookies.elts;
    for (ngx_uint_t i = 0; i < r->headers_in.cookies.nelts; i++) {
        if (!elts[i]->value.len) continue;
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "elts[%i] = %V:%V", i, &elts[i]->key, &elts[i]->value);
        for (u_char *start = elts[i]->value.data, *end = elts[i]->value.data + elts[i]->value.len; start < end; ) {
            ngx_str_t key;
            for (key.data = start; start < end && *start != '='; start++);
            key.len = start - key.data;
            start++;
            ngx_str_t *value = ngx_http_json_value(r, array, &key);
            if (!value) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_value"); return NULL; }
            for (value->data = start; start < end && *start != ';'; start++);
            value->len = start - value->data;
            start++;
        }
    }
    return array;
}

static ngx_int_t ngx_http_json_cookies(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_array_t *array = ngx_http_json_cookies_array(r);
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_cookies_array"); return NGX_ERROR; }
    v->len = ngx_http_json_len(r, array);
    if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_data(r, array, v->data) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_data != v->data + v->len"); return NGX_ERROR; }
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static ngx_array_t *ngx_http_json_get_vars_array(ngx_http_request_t *r, u_char *start, u_char *end, ngx_array_t *array) {
    if (!array && !(array = ngx_array_create(r->pool, 4, sizeof(ngx_http_json_key_value_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_create"); return NULL; }
    while (start < end) {
        for (; start < end && (*start == '=' || *start == '&'); start++);
        ngx_str_t key;
        for (key.data = start; start < end && *start != '=' && *start != '&'; start++);
        key.len = start - key.data;
        u_char *src = key.data;
        u_char *dst = ngx_pnalloc(r->pool, key.len);
        if (!dst) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NULL; }
        ngx_memcpy(dst, key.data, key.len);
        key.data = dst;
        ngx_unescape_uri(&dst, &src, key.len, NGX_UNESCAPE_URI);
        key.len = dst - key.data;
        ngx_str_t *value = ngx_http_json_value(r, array, &key);
        if (!value) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_value"); return NULL; }
        ngx_str_null(value);
        if (start < end && *start++ != '&') {
            for (value->data = start; start < end && *start != '&'; start++);
            value->len = start - value->data;
            src = value->data;
            dst = ngx_pnalloc(r->pool, value->len);
            if (!dst) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NULL; }
            ngx_memcpy(dst, value->data, value->len);
            value->data = dst;
            ngx_unescape_uri(&dst, &src, value->len, NGX_UNESCAPE_URI);
            value->len = dst - value->data;
        }
    }
    return array;
}

static ngx_int_t ngx_http_json_get_vars(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_array_t *array = ngx_http_json_get_vars_array(r, r->args.data, r->args.data + r->args.len, NULL);
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_get_vars_array"); return NGX_ERROR; }
    v->len = ngx_http_json_len(r, array);
    if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_data(r, array, v->data) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_data != v->data + v->len"); return NGX_ERROR; }
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static ngx_array_t *ngx_http_json_post_vars_array(ngx_http_request_t *r, ngx_str_t *boundary, u_char *start, u_char *end, ngx_array_t *array) {
    if (!array && !(array = ngx_array_create(r->pool, 4, sizeof(ngx_http_json_key_value_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_create"); return NULL; }
    if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_array_create"); return NULL; }
    for (u_char *val; start < end; start += 2) {
        if (ngx_strncmp(start, boundary->data + 2, boundary->len - 2)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_strncmp"); return NULL; }
        start += boundary->len - 2;
        if (ngx_strncmp(start, (u_char *)"\r\nContent-Disposition: form-data; name=\"=", sizeof("\r\nContent-Disposition: form-data; name=\"") - 1)) break;
        start += sizeof("\r\nContent-Disposition: form-data; name=\"") - 1;
        if (!(val = ngx_strstrn(start, "\"\r\n\r\n", sizeof("\"\r\n\r\n") - 1 - 1))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_strstrn"); return NULL; }
        ngx_str_t key = {val - start, start};
        ngx_str_t *value = ngx_http_json_value(r, array, &key);
        if (!value) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_value"); return NULL; }
        val += sizeof("\"\r\n\r\n") - 1;
        if (!(start = ngx_strstrn(val, (char *)boundary->data, boundary->len - 1))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_strstrn"); return NULL; }
        value->data = val;
        value->len = start - val;
    }
    return array;
}

static ngx_buf_t *ngx_http_json_read_request_body_to_buffer(ngx_http_request_t *r) {
    if (!r->request_body) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!r->request_body"); return NULL; }
    ngx_buf_t *buf = ngx_create_temp_buf(r->pool, r->headers_in.content_length_n + 1);
    if (!buf) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_create_temp_buf"); return NULL; }
    buf->memory = 1;
    buf->temporary = 0;
    ngx_memset(buf->start, '\0', r->headers_in.content_length_n + 1);
    for (ngx_chain_t *chain = r->request_body->bufs; chain && chain->buf; chain = chain->next) {
        off_t len = ngx_buf_size(chain->buf);
        if (len >= r->headers_in.content_length_n) {
            buf->start = buf->pos;
            buf->last = buf->pos;
            len = r->headers_in.content_length_n;
        }
        if (chain->buf->in_file) {
            ssize_t n = ngx_read_file(chain->buf->file, buf->start, len, 0);
            if (n == NGX_FILE_ERROR) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_read_file == NGX_FILE_ERROR"); return NULL; }
            buf->last = buf->last + len;
            ngx_delete_file(chain->buf->file->name.data);
            chain->buf->file->fd = NGX_INVALID_FILE;
        } else {
            buf->last = ngx_copy(buf->start, chain->buf->pos, len);
        }
        buf->start = buf->last;
    }
    return buf;
}

static ngx_int_t ngx_http_json_post_vars(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    if (r->headers_in.content_length_n <= 0) { ngx_str_set(v, "{}"); goto ret; }
    ngx_buf_t *buf = ngx_http_json_read_request_body_to_buffer(r);
    if (!buf) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_read_request_body_to_buffer"); return NGX_ERROR; }
    if (!r->headers_in.content_type) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!r->headers_in.content_type"); return NGX_ERROR; }
    ngx_http_json_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_json_module);
    switch (ctx->content_type) {
        case NGX_JSON_APPLICATION_X_WWW_FORM_URLENCODED: {
            ngx_array_t *array = ngx_http_json_get_vars_array(r, buf->pos, buf->last, NULL);
            if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_get_vars_array"); return NGX_ERROR; }
            v->len = ngx_http_json_len(r, array);
            if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
            if (ngx_http_json_data(r, array, v->data) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_data != v->data + v->len"); return NGX_ERROR; }
        } break;
        case NGX_JSON_APPLICATION_JSON: {
            v->len = ngx_buf_size(buf);
            if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
            if (ngx_copy(v->data, buf->pos, v->len) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_copy != v->data + v->len"); return NGX_ERROR; }
        } break;
        case NGX_JSON_MULTIPART_FORM_DATA: {
            if (ngx_strncmp(r->headers_in.content_type->value.data, (u_char *)"multipart/form-data; boundary=", sizeof("multipart/form-data; boundary=") - 1)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_strncmp"); return NGX_ERROR; }
            size_t boundary_len = r->headers_in.content_type->value.len - (sizeof("multipart/form-data; boundary=") - 1);
            u_char *boundary_data = ngx_pnalloc(r->pool, boundary_len + 4);
            if (!boundary_data) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
            ngx_memcpy(boundary_data, "\r\n--", sizeof("\r\n--") - 1);
            ngx_memcpy(boundary_data + 4, r->headers_in.content_type->value.data + sizeof("multipart/form-data; boundary=") - 1, boundary_len);
            boundary_len += 4;
            ngx_str_t boundary = {boundary_len, boundary_data};
            ngx_array_t *array = ngx_http_json_post_vars_array(r, &boundary, buf->pos, buf->last, NULL);
            if (!array) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_json_post_vars_array"); return NGX_ERROR; }
            v->len = ngx_http_json_len(r, array);
            if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
            if (ngx_http_json_data(r, array, v->data) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_data != v->data + v->len"); return NGX_ERROR; }
        } break;
        default: ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unsupported content type %V", &r->headers_in.content_type->value); ngx_str_set(v, "{}"); goto ret;
    }
ret:
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static ngx_http_variable_t ngx_http_json_variables[] = {
  { .name = ngx_string("json_response_headers"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_response_headers,
    .data = 0,
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_response_headers_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_response_headers"),
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_headers"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_headers,
    .data = 0,
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_headers_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_headers"),
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_cookies"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_cookies,
    .data = 0,
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_cookies_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_cookies"),
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_get_vars"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_get_vars,
    .data = 0,
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_get_vars_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_get_vars"),
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_post_vars"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_post_vars,
    .data = 0,
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_post_vars_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_post_vars"),
    .flags = NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
    ngx_http_null_variable
};

static ngx_int_t ngx_http_json_preconfiguration(ngx_conf_t *cf) {
    for (ngx_http_variable_t *v = ngx_http_json_variables; v->name.len; v++) {
        ngx_http_variable_t *var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (!var) return NGX_ERROR;
        *var = *v;
        if (var->data) {
            ngx_str_t *name = (ngx_str_t *)var->data;
            ngx_int_t index = ngx_http_get_variable_index(cf, name);
            if (index == NGX_ERROR) return NGX_ERROR;
            var->data = (uintptr_t)index;
        }
        if (var->get_handler == ngx_http_json_post_vars) {
            ngx_http_json_main_conf_t *main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_json_module);
            main_conf->read_request_body = 1;
        }
    }
    return NGX_OK;
}

static void ngx_http_json_post_read(ngx_http_request_t *r) {
    ngx_http_json_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_json_module);
    ctx->done = 1;
    r->main->count--;
    if (ctx->waiting_more_body) { ctx->waiting_more_body = 0; ngx_http_core_run_phases(r); }
}

static ngx_int_t ngx_http_json_handler(ngx_http_request_t *r) {
    ngx_http_json_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_json_module);
    if (ctx) {
        if (ctx->done) { ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "ctx->done"); return NGX_DECLINED; }
        return NGX_DONE;
    }
    if (r->method != NGX_HTTP_POST && r->method != NGX_HTTP_PUT) return NGX_DECLINED;
    if (r->headers_in.content_type == NULL || r->headers_in.content_type->value.data == NULL) return NGX_DECLINED;
    if (!(ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_json_ctx_t)))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pcalloc"); return NGX_ERROR; }
    if (r->headers_in.content_type->value.len == sizeof("application/x-www-form-urlencoded") - 1 && !ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *)"application/x-www-form-urlencoded", sizeof("application/x-www-form-urlencoded") - 1)) {
        ctx->content_type = NGX_JSON_APPLICATION_X_WWW_FORM_URLENCODED;
    } else if (r->headers_in.content_type->value.len >= sizeof("application/json") - 1 && !ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *)"application/json", sizeof("application/json") - 1)) {
        ctx->content_type = NGX_JSON_APPLICATION_JSON;
    } else if (r->headers_in.content_type->value.len > sizeof("multipart/form-data") - 1 && !ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *)"multipart/form-data", sizeof("multipart/form-data") - 1)) {
        ctx->content_type = NGX_JSON_MULTIPART_FORM_DATA;
    } else return NGX_DECLINED;
    ngx_http_set_ctx(r, ctx, ngx_http_json_module);
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "start to read client request body");
    ngx_int_t rc = ngx_http_read_client_request_body(r, ngx_http_json_post_read);
    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_read_client_request_body = %i", rc); return rc; }
    if (rc == NGX_AGAIN) { ctx->waiting_more_body = 1; return NGX_DONE; }
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "has read the request body in one run");
    return NGX_DECLINED;
}

static ngx_int_t ngx_http_json_postconfiguration(ngx_conf_t *cf) {
    ngx_http_json_main_conf_t *main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_json_module);
    if (!main_conf->read_request_body) return NGX_OK;
    ngx_http_core_main_conf_t *core_main_conf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    ngx_http_handler_pt *handler = ngx_array_push(&core_main_conf->phases[NGX_HTTP_POST_READ_PHASE].handlers);
    if (!handler) return NGX_ERROR;
    *handler = ngx_http_json_handler;
    ngx_http_core_loc_conf_t *core_loc_conf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    core_loc_conf->client_body_in_single_buffer = 1;
    return NGX_OK;
}

static void *ngx_http_json_create_main_conf(ngx_conf_t *cf) {
    ngx_http_json_main_conf_t *main_conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_json_main_conf_t));
    if (!main_conf) return NULL;
    return main_conf;
}

static ngx_int_t ngx_http_json_loads_func(ngx_http_request_t *r, ngx_str_t *val, ngx_http_variable_value_t *v) {
    json_error_t error;
    json_t *json = json_loadb((char *)v->data, v->len, JSON_DECODE_ANY, &error);
    if (!json) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_loadb: %s", error.text); return NGX_ERROR; }
    ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
    if (!cln) { ngx_http_json_json_object_clear(json); ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pool_cleanup_add"); return NGX_ERROR; }
    cln->handler = (ngx_pool_cleanup_pt)ngx_http_json_json_object_clear;
    cln->data = json;
    val->data = (u_char *)json;
    val->len = sizeof(json_t);
    return NGX_OK;
}

static ngx_int_t ngx_http_json_dumps_func(ngx_http_request_t *r, ngx_str_t *val, ngx_http_variable_value_t *v, void *data) {
    ngx_http_json_var_index_nelts_t *index_nelts = data;
    ngx_http_variable_value_t *vv = ngx_http_get_indexed_variable(r, index_nelts->index);
    if (!vv) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_get_indexed_variable"); return NGX_ERROR; }
    if (!vv->data) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!vv->data"); return NGX_ERROR; }
    if (vv->len != sizeof(json_t)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "vv->len != sizeof(json_t)"); return NGX_ERROR; }
    json_t *json = (json_t *)vv->data;
    for (ngx_uint_t i = 0; json && i < index_nelts->nelts; i++) {
        u_char *key = ngx_pnalloc(r->pool, v[i].len + 1);
        if (!key) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
        (void)ngx_cpystrn(key, v[i].data, v[i].len + 1);
        json = json_object_get(json, (const char *)key);
    }
    const char *value = index_nelts->nelts ? json_string_value(json) : json_dumps(json, JSON_PRESERVE_ORDER | JSON_ENCODE_ANY | JSON_COMPACT);
    if (index_nelts->nelts && !value) { ngx_log_error(NGX_LOG_WARN, r->connection->log, 0, "!json_string_value"); value = json_dumps(json, JSON_PRESERVE_ORDER | JSON_ENCODE_ANY | JSON_COMPACT); }
    if (!value) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_dumps"); return NGX_ERROR; }
    val->data = (u_char *)value;
    val->len = ngx_strlen(value);
    return NGX_OK;
}

static char *ngx_http_json_dumps_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_str_t *elts = cf->args->elts;
    if (elts[2].data[0] != '$') return "invalid variable name";
    elts[2].len--;
    elts[2].data++;
    ngx_int_t index = ngx_http_get_variable_index(cf, &elts[2]);
    if (index == NGX_ERROR) return "ngx_http_get_variable_index == NGX_ERROR";
    ngx_http_json_var_index_nelts_t *index_nelts = ngx_palloc(cf->pool, sizeof(ngx_http_json_var_index_nelts_t));
    if (!index_nelts) return "!ngx_palloc";
    index_nelts->index = (ngx_uint_t) index;
    index_nelts->nelts = cf->args->nelts - 3;
    ndk_set_var_t filter = { NDK_SET_VAR_MULTI_VALUE_DATA, ngx_http_json_dumps_func, index_nelts->nelts, index_nelts };
    return ndk_set_var_multi_value_core(cf, &elts[1], &elts[3], &filter);
}

static size_t ngx_http_json_var_len(ngx_http_request_t *r, ngx_array_t *fields) {
    size_t len = 0;
    ngx_http_json_var_field_t *elts = fields->elts;
    for (ngx_uint_t i = 0; i < fields->nelts; i++) {
        if (!elts[i].name.len) continue;
        if (ngx_http_complex_value(r, &elts[i].cv, &elts[i].value) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_complex_value != NGX_OK"); return NGX_ERROR; }
        if (!elts[i].value.len) continue;
        if (len) len++;
        len += sizeof("\"\":") - 1 + elts[i].name.len;
        if (elts[i].json) len += elts[i].value.len; else {
            elts[i].escape = ngx_escape_json(NULL, elts[i].value.data, elts[i].value.len);
            len += sizeof("\"\"") - 1 + elts[i].value.len + elts[i].escape;
        }
    }
    len += sizeof("{}") - 1;
    return len;
}

static u_char *ngx_http_json_var_data(u_char *p, ngx_array_t *fields) {
    *p++ = '{';
    u_char *var = p;
    ngx_http_json_var_field_t *elts = fields->elts;
    for (ngx_uint_t i = 0; i < fields->nelts; i++) {
        if (!elts[i].name.len) continue;
        if (!elts[i].value.len) continue;
        if (p != var) *p++ = ',';
        *p++ = '"';
        p = ngx_copy(p, elts[i].name.data, elts[i].name.len);
        *p++ = '"';
        *p++ = ':';
        if (elts[i].json) p = ngx_copy(p, elts[i].value.data, elts[i].value.len); else {
            *p++ = '"';
            if (elts[i].escape) p = (u_char *)ngx_escape_json(p, elts[i].value.data, elts[i].value.len);
            else p = ngx_copy(p, elts[i].value.data, elts[i].value.len);
            *p++ = '"';
        }
    }
    *p++ = '}';
    return p;
}

static ngx_int_t ngx_http_json_var_http_handler(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    ngx_array_t *fields = (ngx_array_t *)data;
    v->len = ngx_http_json_var_len(r, fields);
    if (!(v->data = ngx_pnalloc(r->pool, v->len))){ ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_var_data(v->data, fields) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_var_data != v->data + v->len"); return NGX_ERROR; }
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static char *ngx_http_json_var_conf_handler(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_str_t *elts = cf->args->elts;
    if (cf->args->nelts != 2) return "cf->args->nelts != 2";
    ngx_http_json_var_ctx_t *ctx = cf->ctx;
    ngx_http_json_var_field_t *field = ngx_array_push(ctx->fields);
    if (!field) return "!ngx_array_push";
    field->value = elts[1];
    field->json = field->value.data[0] == '$'
       && ((field->value.len - 1 == sizeof("json_response_headers") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_response_headers", sizeof("json_response_headers") - 1))
        || (field->value.len - 1 == sizeof("json_headers") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_headers", sizeof("json_headers") - 1))
        || (field->value.len - 1 == sizeof("json_cookies") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_cookies", sizeof("json_cookies") - 1))
        || (field->value.len - 1 == sizeof("json_get_vars") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_get_vars", sizeof("json_get_vars") - 1))
        || (field->value.len - 1 == sizeof("json_post_vars") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_post_vars", sizeof("json_post_vars") - 1)));
    ngx_http_compile_complex_value_t ccv = {ctx->cf, &field->value, &field->cv, 0, 0, 0};
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) return "ngx_http_compile_complex_value != NGX_OK";
    field->name = elts[0];
    return NGX_CONF_OK;
}

static char *ngx_http_json_var_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_str_t *elts = cf->args->elts;
    ngx_str_t name = elts[1];
    if (name.data[0] != '$') return "invalid variable name";
    name.len--;
    name.data++;
    ngx_http_variable_t *var = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_NOCACHEABLE|NGX_HTTP_VAR_CHANGEABLE);
    if (!var) return "!ngx_http_add_variable";
    var->get_handler = ngx_http_json_var_http_handler;
    ngx_array_t *fields = ngx_array_create(cf->pool, 4, sizeof(ngx_http_json_var_field_t));
    if (!fields) return "!ngx_array_create";
    var->data = (uintptr_t)fields;
    ngx_conf_t save = *cf;
    ngx_http_json_var_ctx_t ctx = {&save, fields};
    cf->ctx = &ctx;
    cf->handler = ngx_http_json_var_conf_handler;
    char *rv = ngx_conf_parse(cf, NULL);
    *cf = save;
    if (rv != NGX_CONF_OK) return rv;
    if (!fields->nelts) return "!fields->nelts";
    return rv;
}

static ngx_int_t ngx_http_json_var_loads_func(ngx_http_request_t *r, ngx_str_t *val, void *data) {
    json_t *json = json_object();
    if (!json) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_object"); return NGX_ERROR; }
    ngx_pool_cleanup_t *cln = ngx_pool_cleanup_add(r->pool, 0);
    if (!cln) { ngx_http_json_json_object_clear(json); ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pool_cleanup_add"); return NGX_ERROR; }
    cln->handler = (ngx_pool_cleanup_pt)ngx_http_json_json_object_clear;
    cln->data = json;
    ngx_array_t *fields = data;
    ngx_http_json_var_field_t *elts = fields->elts;
    for (ngx_uint_t i = 0; i < fields->nelts; i++) {
        json_t *value;
        switch (elts[i].json) {
            case NGX_JSON_TRUE: value = json_true(); break;
            case NGX_JSON_FALSE: value = json_false(); break;
            case NGX_JSON_NULL: value = json_null(); break;
            case NGX_JSON_STRING: {
                if (ngx_http_complex_value(r, &elts[i].cv, &elts[i].value) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_complex_value != NGX_OK"); return NGX_ERROR; }
                if (!(value = json_stringn((const char *)elts[i].value.data, elts[i].value.len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_stringn"); return NGX_ERROR; }
            } break;
            case NGX_JSON_INTEGER: {
                if (ngx_http_complex_value(r, &elts[i].cv, &elts[i].value) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_complex_value != NGX_OK"); return NGX_ERROR; }
                ngx_int_t n = ngx_atoi(elts[i].value.data, elts[i].value.len);
                if (n == NGX_ERROR) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_atoi == NGX_ERROR"); return NGX_ERROR; }
                if (!(value = json_integer(n))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_integer"); return NGX_ERROR; }
            } break;
            case NGX_JSON_REAL: {
                if (ngx_http_complex_value(r, &elts[i].cv, &elts[i].value) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_complex_value != NGX_OK"); return NGX_ERROR; }
                u_char *point = ngx_strstrn(elts[i].value.data, ".", sizeof(".") - 1 - 1);
                if (!point) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_strstrn"); return NGX_ERROR; }
                size_t p = elts[i].value.data + elts[i].value.len - point - 1;
                ngx_int_t n = ngx_atofp(elts[i].value.data, elts[i].value.len, p);
                if (n == NGX_ERROR) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_atofp == NGX_ERROR"); return NGX_ERROR; }
                double d = n;
                while (p--) d /= 10.0;
                if (!(value = json_real(d))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_real"); return NGX_ERROR; }
            } break;
            case NGX_JSON_LOADS: {
                if (ngx_http_complex_value(r, &elts[i].cv, &elts[i].value) != NGX_OK) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_complex_value != NGX_OK"); return NGX_ERROR; }
                json_error_t error;
                if (!(value = json_loadb((char *)elts[i].value.data, elts[i].value.len, JSON_DECODE_ANY, &error))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!json_loadb: %s", error.text); return NGX_ERROR; }
            } break;
            case NGX_JSON_OBJECT: {
                ngx_http_variable_value_t *vv = ngx_http_get_indexed_variable(r, elts[i].index);
                if (!vv) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_http_get_indexed_variable"); return NGX_ERROR; }
                if (!vv->data) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!vv->data"); return NGX_ERROR; }
                if (vv->len != sizeof(json_t)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "vv->len != sizeof(json_t)"); return NGX_ERROR; }
                value = (json_t *)vv->data;
            } break;
            default: ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unknown json type %d", elts[i].json); return NGX_ERROR;
        }
        u_char *key = ngx_pnalloc(r->pool, elts[i].name.len + 1);
        if (!key) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
        (void)ngx_cpystrn(key, elts[i].name.data, elts[i].name.len + 1);
        if (json_object_set_new(json, (const char *)key, value)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "json_object_set_new"); return NGX_ERROR; }
    }
    val->data = (u_char *)json;
    val->len = sizeof(json_t);
    return NGX_OK;
}

static char *ngx_http_json_var_loads_conf_handler(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_json_var_ctx_t *ctx = cf->ctx;
    ngx_http_json_var_field_t *field = ngx_array_push(ctx->fields);
    if (!field) return "!ngx_array_push";
    ngx_str_t *elts = cf->args->elts;
    field->name = elts[0];
    field->command = elts[1];
    if (field->command.len == sizeof("true") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"true", sizeof("true") - 1)) { if (cf->args->nelts != 2) return "cf->args->nelts != 2"; field->json = NGX_JSON_TRUE; return NGX_CONF_OK; }
    if (field->command.len == sizeof("false") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"false", sizeof("false") - 1)) { if (cf->args->nelts != 2) return "cf->args->nelts != 2"; field->json = NGX_JSON_FALSE; return NGX_CONF_OK; }
    if (field->command.len == sizeof("null") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"null", sizeof("null") - 1)) { if (cf->args->nelts != 2) return "cf->args->nelts != 2"; field->json = NGX_JSON_NULL; return NGX_CONF_OK; }
    if (cf->args->nelts != 3) return "cf->args->nelts != 3";
    field->value = elts[2];
    if (field->command.len == sizeof("object") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"object", sizeof("object") - 1)) {
        ngx_log_error(NGX_LOG_WARN, cf->log, 0, "field->value = %V", &field->value);
        if (field->value.data[0] != '$') return "invalid variable name";
        field->value.len--;
        field->value.data++;
        ngx_log_error(NGX_LOG_WARN, cf->log, 0, "field->value = %V", &field->value);
        ngx_int_t index = ngx_http_get_variable_index(cf, &field->value);
        if (index == NGX_ERROR) return "ngx_http_get_variable_index == NGX_ERROR";
        field->index = (ngx_uint_t) index;
        ngx_log_error(NGX_LOG_WARN, cf->log, 0, "index = %i", index);
        field->json = NGX_JSON_OBJECT;
        return NGX_CONF_OK;
    }
    ngx_http_compile_complex_value_t ccv = {ctx->cf, &field->value, &field->cv, 0, 0, 0};
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) return "ngx_http_compile_complex_value != NGX_OK";
    if (field->command.len == sizeof("string") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"string", sizeof("string") - 1)) { field->json = NGX_JSON_STRING; return NGX_CONF_OK; }
    if (field->command.len == sizeof("integer") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"integer", sizeof("integer") - 1)) { field->json = NGX_JSON_INTEGER; return NGX_CONF_OK; }
    if (field->command.len == sizeof("real") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"real", sizeof("real") - 1)) { field->json = NGX_JSON_REAL; return NGX_CONF_OK; }
    if (field->command.len == sizeof("loads") - 1 && !ngx_strncasecmp(field->command.data, (u_char *)"loads", sizeof("loads") - 1)) { field->json = NGX_JSON_LOADS; return NGX_CONF_OK; }
    return "invalid command";
}

static char *ngx_http_json_var_loads_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_array_t *fields = ngx_array_create(cf->pool, 4, sizeof(ngx_http_json_var_field_t));
    if (!fields) return "!ngx_array_create";
    ndk_set_var_t filter = {NDK_SET_VAR_DATA, ngx_http_json_var_loads_func, 0, fields};
    ngx_str_t *elts = cf->args->elts;
    char *rv = ndk_set_var_core(cf, &elts[1], &filter);
    if (rv != NGX_CONF_OK) return rv;
    ngx_conf_t save = *cf;
    ngx_http_json_var_ctx_t ctx = {&save, fields};
    cf->ctx = &ctx;
    cf->handler = ngx_http_json_var_loads_conf_handler;
    rv = ngx_conf_parse(cf, NULL);
    *cf = save;
    if (rv != NGX_CONF_OK) return rv;
    if (fields->nelts <= 0) return "no fields defined";
    return rv;
}

static ngx_command_t ngx_http_json_commands[] = {
  { .name = ngx_string("json_loads"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
    .set = ndk_set_var_value,
    .conf = NGX_HTTP_LOC_CONF_OFFSET,
    .offset = 0,
    .post = &(ndk_set_var_t){ NDK_SET_VAR_VALUE, ngx_http_json_loads_func, 1, NULL } },
  { .name = ngx_string("json_dumps"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_2MORE,
    .set = ngx_http_json_dumps_conf,
    .conf = NGX_HTTP_LOC_CONF_OFFSET,
    .offset = 0,
    .post = NULL },
  { .name = ngx_string("json_var"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
    .set = ngx_http_json_var_conf,
    .conf = 0,
    .offset = 0,
    .post = NULL },
  { .name = ngx_string("json_var_loads"),
    .type = NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
    .set = ngx_http_json_var_loads_conf,
    .conf = 0,
    .offset = 0,
    .post = NULL },
    ngx_null_command
};

static ngx_http_module_t ngx_http_json_ctx = {
    .preconfiguration = ngx_http_json_preconfiguration,
    .postconfiguration = ngx_http_json_postconfiguration,
    .create_main_conf = ngx_http_json_create_main_conf,
    .init_main_conf = NULL,
    .create_srv_conf = NULL,
    .merge_srv_conf = NULL,
    .create_loc_conf = NULL,
    .merge_loc_conf = NULL
};

ngx_module_t ngx_http_json_module = {
    NGX_MODULE_V1,
    .ctx = &ngx_http_json_ctx,
    .commands = ngx_http_json_commands,
    .type = NGX_HTTP_MODULE,
    .init_master = NULL,
    .init_module = NULL,
    .init_process = NULL,
    .init_thread = NULL,
    .exit_thread = NULL,
    .exit_process = NULL,
    .exit_master = NULL,
    NGX_MODULE_V1_PADDING
};
