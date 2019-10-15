#include <jansson.h>
#include "ndk_set_var.h"

ngx_module_t ngx_http_json_module;

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

static size_t ngx_http_json_headers_len(ngx_list_part_t *part) {
    size_t len;
    for (len = 0; part; part = part->next) {
        ngx_table_elt_t *elts = part->elts;
        for (ngx_uint_t i = 0; i < part->nelts; i++) {
            if (!elts[i].key.len) continue;
            if (!elts[i].value.len) continue;
            if (len) len += sizeof(",") - 1;
            len += (sizeof("\"\":\"\"") - 1) + elts[i].key.len + elts[i].value.len + ngx_escape_json(NULL, elts[i].value.data, elts[i].value.len);
        }
    }
    len += sizeof("{}") - 1;
    return len;
}

static u_char *ngx_http_json_headers_data(u_char *p, ngx_list_part_t *part) {
    *p++ = '{';
    for (u_char *headers = p; part; part = part->next) {
        ngx_table_elt_t *elts = part->elts;
        for (ngx_uint_t i = 0; i < part->nelts; i++) {
            if (!elts[i].key.len) continue;
            if (!elts[i].value.len) continue;
            if (p != headers) *p++ = ',';
            *p++ = '"';
            p = ngx_copy(p, elts[i].key.data, elts[i].key.len);
            *p++ = '"'; *p++ = ':'; *p++ = '"';
            p = (u_char *)ngx_escape_json(p, elts[i].value.data, elts[i].value.len);
            *p++ = '"';
        }
    }
    *p++ = '}';
    return p;
}

static ngx_int_t ngx_http_json_headers(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    v->len = ngx_http_json_headers_len(&r->headers_in.headers.part);
    if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_headers_data(v->data, &r->headers_in.headers.part) != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_headers_data != v->data + v->len"); return NGX_ERROR; }
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

static size_t ngx_http_json_cookies_len(size_t len, u_char *start, u_char *end) {
    while (start < end) {
        while (start < end && *start == ' ') start++;
        if (len) len++;
        len++;
        for (; start < end && *start != '='; len++, start++) if (*start == '\\' || *start == '"') len++;
        start++; len++; len++; len++;
        for (; start < end && *start != ';'; len++, start++) if (*start == '\\' || *start == '"') len++; 
        start++; len++;
    }
    return len;
}

static u_char *ngx_http_json_cookies_data(u_char *p, u_char *start, u_char *end, u_char *cookies) {
    while (start < end) {
        while (start < end && *start == ' ') start++;
        if (p != cookies) *p++ = ',';
        *p++ = '"';
        for (; start < end && *start != '='; *p++ = *start++) if (*start == '\\' || *start == '"') *p++ = '\\';
        start++; *p++ = '"'; *p++ = ':'; *p++ = '"';
        for (; start < end && *start != ';'; *p++ = *start++) if (*start == '\\' || *start == '"') *p++ = '\\'; 
        start++; *p++ = '"';
    }
    return p;
}

static ngx_int_t ngx_http_json_cookies(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    v->len = 0;
    ngx_table_elt_t **elts = r->headers_in.cookies.elts;
    for (ngx_uint_t i = 0; i < r->headers_in.cookies.nelts; i++) {
        if (!elts[i]->value.len) continue;
        v->len += ngx_http_json_cookies_len(v->len, elts[i]->value.data, elts[i]->value.data + elts[i]->value.len);
    }
    v->len += sizeof("{}") - 1;
    if (!(v->data = ngx_pnalloc(r->pool, v->len))) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    u_char *p = v->data;
    *p++ = '{';
    for (ngx_uint_t i = 0; i < r->headers_in.cookies.nelts; i++) {
        if (!elts[i]->value.len) continue;
        p = ngx_http_json_cookies_data(p, elts[i]->value.data, elts[i]->value.data + elts[i]->value.len, v->data + 1);
    }
    *p++ = '}';
    if (p != v->data + v->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "p != v->data + v->len"); return NGX_ERROR; }
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    return NGX_OK;
}

static size_t ngx_http_json_vars_size(u_char *start, u_char *end) {
    size_t size;
    for (size = 0; start < end; start++, size++) {
        if (*start == '\\' || *start == '"') size++;
        else if (*start == '&') size += sizeof("\"\":\"\",") - 1;
    }
    return size ? size + sizeof("{}\"\":\"\"") - 1 + 2 : 0;
}

#define unescape_characters { \
    switch (*start) { \
        case '%': { \
            start++; \
            c = *start++; \
            if (!isxdigit(c)) break; \
            if (c < 'A') c -= '0'; \
            else if(c < 'a') c -= 'A' - 10; \
            else c -= 'a' - 10; \
            *p = (c << 4); \
            c = *start++; \
            if (!isxdigit(c)) break; \
            if (c < 'A') c -= '0'; \
            else if(c < 'a') c -= 'A' - 10; \
            else c -= 'a' - 10; \
            *p += c; \
            c = *p; \
        } break; \
        case '+': { c = ' '; start++; } break; \
        default: c = *start++; \
    } \
    if (c == '\\' || c == '"') *p++ = '\\'; \
    *p++ = c; \
}
static u_char *ngx_http_json_vars_data(u_char *p, u_char *start, u_char *end, u_char *args) {
    for (u_char c; start < end; ) {
        if (p != args) *p++ = ',';
        *p++ = '"';
        while (start < end && (*start == '=' || *start == '&')) start++;
        while (start < end && *start != '=' && *start != '&') unescape_characters
        *p++ = '"'; *p++ = ':';
        if (start < end && *start++ != '&') {
            *p++ = '"';
            while (start < end && *start != '&') unescape_characters
            *p++ = '"';
        } else {
            *p++ = 'n'; *p++ = 'u'; *p++ = 'l'; *p++ = 'l';
        }
    }
    return p;
}

static ngx_int_t ngx_http_json_get_vars(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    size_t size = ngx_http_json_vars_size(r->args.data, r->args.data + r->args.len);
    if (!size) goto err;
    u_char *p = ngx_pnalloc(r->pool, size);
    if (!p) goto err;
    v->data = p;
    *p++ = '{';
    p = ngx_http_json_vars_data(p, r->args.data, r->args.data + r->args.len, v->data + 1);
    *p++ = '}';
    v->len = p - v->data;
    if (v->len > size) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_get_vars: result length %l exceeded allocated length %l", v->len, size); goto err; }
    return NGX_OK;
err:
    ngx_str_set(v, "");
    return NGX_OK;
}

static u_char *ngx_http_json_post_vars_data(u_char *p, ngx_pool_t *pool, u_char *content_type, u_char *request_body, u_char *body) {
    u_char *mime_type_end_ptr = (u_char *)ngx_strchr(content_type, ';');
    if (!mime_type_end_ptr) return NULL;
    u_char *boundary_start_ptr = ngx_strstrn(mime_type_end_ptr, "boundary=", sizeof("boundary=") - 1 - 1);
    if (!boundary_start_ptr) return NULL;
    boundary_start_ptr += sizeof("boundary=") - 1;
    u_char *boundary_end_ptr = boundary_start_ptr + strcspn((char *)boundary_start_ptr, " ;\n\r");
    if (boundary_end_ptr == boundary_start_ptr) return NULL;
    ngx_str_t boundary = {boundary_end_ptr - boundary_start_ptr + 4, ngx_pnalloc(pool, boundary_end_ptr - boundary_start_ptr + 4 + 1)};
    if (!boundary.data) return NULL;
    ngx_cpystrn(boundary.data + 4, boundary_start_ptr, boundary_end_ptr - boundary_start_ptr + 1);
    boundary.data[0] = '\r'; boundary.data[1] = '\n'; boundary.data[2] = '-'; boundary.data[3] = '-'; boundary.data[boundary.len] = '\0';
    for (
        u_char *s = request_body, *name_start_ptr;
        (name_start_ptr = ngx_strstrn(s, "\r\nContent-Disposition: form-data; name=\"", sizeof("\r\nContent-Disposition: form-data; name=\"") - 1 - 1));
        s += boundary.len
    ) {
        name_start_ptr += sizeof("\r\nContent-Disposition: form-data; name=\"") - 1;
        u_char *name_end_ptr = ngx_strstrn(name_start_ptr, "\"\r\n\r\n", sizeof("\"\r\n\r\n") - 1 - 1);
        if (!name_end_ptr) return NULL;
        if (p != body) *p++ = ',';
        *p++ = '"';
        p = (u_char*)ngx_escape_json(p, name_start_ptr, name_end_ptr - name_start_ptr);
        *p++ = '"'; *p++ = ':';
        u_char *value_start_ptr = name_end_ptr + sizeof("\"\r\n\r\n") - 1;
        u_char *value_end_ptr = ngx_strstrn(value_start_ptr, (char *)boundary.data, boundary.len - 1);
        if (!value_end_ptr) return NULL;
        *p++ = '"';
        p = (u_char*)ngx_escape_json(p, value_start_ptr, value_end_ptr - value_start_ptr);
        *p++ = '"';
    }
    return p;
}

static ngx_buf_t *ngx_http_json_read_request_body_to_buffer(ngx_http_request_t *r) {
    if (!r->request_body) return NULL;
    ngx_buf_t *buf = ngx_create_temp_buf(r->pool, r->headers_in.content_length_n + 1);
    if (!buf) return buf;
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
            if (n == NGX_FILE_ERROR) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_read_request_body_to_buffer: cannot read file with request body"); return NULL; }
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
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    if (r->headers_in.content_length_n <= 0) goto err;
    ngx_buf_t *buf = ngx_http_json_read_request_body_to_buffer(r);
    if (!buf) goto err;
    if (!r->headers_in.content_type) goto err;
    if (r->headers_in.content_type->value.len == sizeof("application/x-www-form-urlencoded") - 1 && !ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *)"application/x-www-form-urlencoded", sizeof("application/x-www-form-urlencoded") - 1)) {
        size_t size = ngx_http_json_vars_size(buf->pos, buf->last);
        u_char *p = ngx_pnalloc(r->pool, size);
        if (!p) goto err;
        v->data = p;
        *p++ = '{';
        p = ngx_http_json_vars_data(p, buf->pos, buf->last, v->data + 1);
        *p++ = '}';
        v->len = p - v->data;
        if (v->len > size) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_post_vars: result length %l exceeded allocated length %l", v->len, size); goto err; }
    } else if (r->headers_in.content_type->value.len == sizeof("application/json") - 1 && !ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *)"application/json", sizeof("application/json") - 1)) {
        u_char *p = ngx_pnalloc(r->pool, ngx_buf_size(buf));
        if (!p) goto err;
        v->data = p;
        p = ngx_copy(p, buf->pos, ngx_buf_size(buf));
        v->len = p - v->data;
        if (v->len > ngx_buf_size(buf)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_post_vars: result length %l exceeded allocated length %l", v->len, ngx_buf_size(buf)); goto err; }
    } else if (r->headers_in.content_type->value.len == sizeof("multipart/form-data") - 1 && !ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *)"multipart/form-data", sizeof("multipart/form-data") - 1)) {
        u_char *p = ngx_pnalloc(r->pool, ngx_buf_size(buf));
        if (!p) goto err;
        v->data = p;
        *p++ = '{';
        p = ngx_http_json_post_vars_data(p, r->pool, r->headers_in.content_type->value.data, buf->pos, v->data + 1);
        if (!p) goto err;
        *p++ = '}';
        v->len = p - v->data;
        if (v->len > ngx_buf_size(buf)) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_post_vars: result length %l exceeded allocated length %l", v->len, ngx_buf_size(buf)); goto err; }
    } else goto err;
    return NGX_OK;
err:
    ngx_str_set(v, "");
    return NGX_OK;
}

static ngx_http_variable_t ngx_http_json_variables[] = {
  { .name = ngx_string("json_headers"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_headers,
    .data = 0,
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_headers_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_headers"),
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_cookies"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_cookies,
    .data = 0,
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_cookies_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_cookies"),
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_get_vars"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_get_vars,
    .data = 0,
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_get_vars_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_get_vars"),
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_post_vars"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_post_vars,
    .data = 0,
    .flags = NGX_HTTP_VAR_CHANGEABLE,
    .index = 0 },
  { .name = ngx_string("json_post_vars_loads"),
    .set_handler = NULL,
    .get_handler = ngx_http_json_loads,
    .data = (uintptr_t)&(ngx_str_t)ngx_string("json_post_vars"),
    .flags = NGX_HTTP_VAR_CHANGEABLE,
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
    }
    return NGX_OK;
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

static ngx_int_t ngx_http_json_var_func(ngx_http_request_t *r, ngx_str_t *val, void *data) {
    ngx_array_t *fields = data;
    val->len = ngx_http_json_var_len(r, fields);
    if (!(val->data = ngx_pnalloc(r->pool, val->len))){ ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "!ngx_pnalloc"); return NGX_ERROR; }
    if (ngx_http_json_var_data(val->data, fields) != val->data + val->len) { ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_json_var_data != val->data + val->len"); return NGX_ERROR; }
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
       && ((field->value.len - 1 == sizeof("json_headers") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_headers", sizeof("json_headers") - 1))
        || (field->value.len - 1 == sizeof("json_cookies") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_cookies", sizeof("json_cookies") - 1))
        || (field->value.len - 1 == sizeof("json_get_vars") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_get_vars", sizeof("json_get_vars") - 1))
        || (field->value.len - 1 == sizeof("json_post_vars") - 1 && !ngx_strncasecmp(field->value.data + 1, (u_char *)"json_post_vars", sizeof("json_post_vars") - 1)));
    ngx_http_compile_complex_value_t ccv = {ctx->cf, &field->value, &field->cv, 0, 0, 0};
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) return "ngx_http_compile_complex_value != NGX_OK";
    field->name = elts[0];
    return NGX_CONF_OK;
}

static char *ngx_http_json_var_conf(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_array_t *fields = ngx_array_create(cf->pool, 4, sizeof(ngx_http_json_var_field_t));
    if (!fields) return "!ngx_array_create";
    ndk_set_var_t filter = {NDK_SET_VAR_DATA, ngx_http_json_var_func, 0, fields};
    ngx_str_t *elts = cf->args->elts;
    char *rv = ndk_set_var_core(cf, &elts[1], &filter);
    if (rv != NGX_CONF_OK) return rv;
    ngx_conf_t save = *cf;
    ngx_http_json_var_ctx_t ctx = {&save, fields};
    cf->ctx = &ctx;
    cf->handler = ngx_http_json_var_conf_handler;
    rv = ngx_conf_parse(cf, NULL);
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
    .postconfiguration = NULL,
    .create_main_conf = NULL,
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
