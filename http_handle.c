#include "http_handle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <curl/curl.h>
#include <curl/easy.h>

typedef struct {
    const char * key;
    const char * value;
} dict_val_t;

typedef struct {
    size_t size;
    size_t offset;
    char * data;
} buffer_t;

typedef struct {
    int closed;
    const char * url;
    CURL * handle;
    buffer_t buf;
    int headers_size;
    dict_val_t * headers;
} http_handle_t;

int http_handle_close(lua_State *L) {
    http_handle_t* handle = (http_handle_t*)lua_touserdata(L, lua_upvalueindex(1));
    if (handle->closed) return 0;
    handle->closed = 1;
    curl_easy_cleanup(handle->handle);
    free(handle->buf.data);
    for (int i = 0; i < handle->headers_size; i++) {
        free((void*)handle->headers[i].key);
        free((void*)handle->headers[i].value);
    }
    free(handle->headers);
    free(handle);
    return 0;
}

extern char checkChar(char c);

int http_handle_readAll(lua_State *L) {
    http_handle_t * handle = (http_handle_t*)lua_touserdata(L, lua_upvalueindex(1));
    if (handle->closed || handle->buf.offset >= handle->buf.size) return 0;
    char * retval = (char*)malloc(handle->buf.size - handle->buf.offset);
    for (int i = handle->buf.offset; i < handle->buf.size; i++)
        retval[i-handle->buf.offset] = handle->buf.data[i];
    lua_pushlstring(L, retval, handle->buf.size - handle->buf.offset);
    handle->buf.offset = handle->buf.size;
    free(retval);
    return 1;
}

int http_handle_readLine(lua_State *L) {
    http_handle_t * handle = (http_handle_t*)lua_touserdata(L, lua_upvalueindex(1));
    if (handle->closed || handle->buf.offset >= handle->buf.size) return 0;
    size_t size = 0;
    for (int i = handle->buf.offset; i < handle->buf.size && handle->buf.data[i] != '\n'; i++) size++;
    char * retval = (char*)malloc(size);
    for (int i = 0; i < size; i++) retval[i] = checkChar(handle->buf.data[handle->buf.offset+i]);
    lua_pushlstring(L, retval, size);
    handle->buf.offset += size + 1;
    free(retval);
    return 1;
}

int http_handle_read(lua_State *L) {
    http_handle_t * handle = (http_handle_t*)lua_touserdata(L, lua_upvalueindex(1));
    if (handle->closed || handle->buf.offset >= handle->buf.size) return 0;
    char retval[2];
    retval[0] = checkChar(handle->buf.data[handle->buf.offset++]);
    lua_pushstring(L, retval);
    return 1;
}

int http_handle_getResponseCode(lua_State *L) {
    http_handle_t * handle = (http_handle_t*)lua_touserdata(L, lua_upvalueindex(1));
    if (handle->closed) return 0;
    long code = 0;
    curl_easy_getinfo(handle->handle, CURLINFO_RESPONSE_CODE, &code);
    lua_pushinteger(L, code);
    return 1;
}

int http_handle_getResponseHeaders(lua_State *L) {
    http_handle_t * handle = (http_handle_t*)lua_touserdata(L, lua_upvalueindex(1));
    if (handle->closed) return 0;
    lua_newtable(L);
    for (int i = 0; i < handle->headers_size; i++) {
        lua_pushstring(L, handle->headers[i].key);
        lua_pushstring(L, handle->headers[i].value);
        lua_settable(L, -2);
    }
    return 1;
}