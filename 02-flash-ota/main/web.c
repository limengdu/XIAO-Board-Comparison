#include "web.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"

#define FILES_PREFIX "/api/files/"
#define WWW_DIR      STORAGE_BASE "/www"
#define IO_CHUNK     4096
#define PATH_MAX_LEN 320

static const char *TAG = "web";
static char s_io[IO_CHUNK];

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *error)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", error);
    return send_json(req, status, body);
}

/* Map a URI tail to a path under `dir`; rejects empty paths and "..".
 * 把 URI 尾部映射为 `dir` 下的路径；拒绝空路径和 ".."。 */
static bool map_path(const char *dir, const char *tail, char *out, size_t len)
{
    size_t n = strcspn(tail, "?#");
    if (n == 0 || strstr(tail, "..") != NULL || tail[0] == '/') {
        return false;
    }
    return snprintf(out, len, "%s/%.*s", dir, (int)n, tail) < (int)len;
}

static void make_parent_dirs(char *path)
{
    for (char *p = path + strlen(STORAGE_BASE) + 1; (p = strchr(p, '/')) != NULL; p++) {
        *p = '\0';
        mkdir(path, 0775);
        *p = '/';
    }
}

static esp_err_t info_get(httpd_req_t *req)
{
    static char json[2048];
    if (app_info_json(json, sizeof(json)) == 0) {
        return send_error(req, "500 Internal Server Error", "info_overflow");
    }
    return send_json(req, "200 OK", json);
}

static esp_err_t format_post(httpd_req_t *req)
{
    esp_err_t err = storage_format();
    return err == ESP_OK ? info_get(req) : send_error(req, "500 Internal Server Error", esp_err_to_name(err));
}

static esp_err_t file_put(httpd_req_t *req)
{
    char path[PATH_MAX_LEN];
    if (!map_path(STORAGE_BASE, req->uri + strlen(FILES_PREFIX), path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "bad_path");
    }
    make_parent_dirs(path);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return send_error(req, errno == ENOSPC ? "507 Insufficient Storage" : "500 Internal Server Error",
                          errno == ENOSPC ? "no_space" : "open_failed");
    }
    size_t left = req->content_len;
    bool full = false;
    while (left > 0) {
        int n = httpd_req_recv(req, s_io, left < IO_CHUNK ? left : IO_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            fclose(f);
            unlink(path);
            return ESP_FAIL;
        }
        if (!full && fwrite(s_io, 1, n, f) != (size_t)n) {
            full = true;
        }
        left -= n;
    }
    if (fclose(f) != 0) {
        full = true;
    }
    if (full) {
        unlink(path);
        return send_error(req, "507 Insufficient Storage", "no_space");
    }
    uint64_t total, free_bytes;
    storage_usage(&total, &free_bytes);
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":true,\"bytes\":%u,\"fs_free\":%" PRIu64 "}", (unsigned)req->content_len,
             free_bytes);
    return send_json(req, "201 Created", body);
}

static esp_err_t stream_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n;
    while ((n = fread(s_io, 1, IO_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, s_io, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t file_get(httpd_req_t *req)
{
    char path[PATH_MAX_LEN];
    if (!map_path(STORAGE_BASE, req->uri + strlen(FILES_PREFIX), path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "bad_path");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t err = stream_file(req, path);
    return err == ESP_ERR_NOT_FOUND ? send_error(req, "404 Not Found", "not_found") : err;
}

static esp_err_t file_delete(httpd_req_t *req)
{
    char path[PATH_MAX_LEN];
    if (!map_path(STORAGE_BASE, req->uri + strlen(FILES_PREFIX), path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "bad_path");
    }
    if (unlink(path) != 0) {
        return send_error(req, "404 Not Found", "not_found");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

static const char *content_type(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        { ".html", "text/html; charset=utf-8" }, { ".css", "text/css" },
        { ".js", "application/javascript" },     { ".json", "application/json" },
        { ".txt", "text/plain; charset=utf-8" }, { ".svg", "image/svg+xml" },
        { ".woff2", "font/woff2" },              { ".png", "image/png" },
    };
    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot && i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcmp(dot, types[i].ext) == 0) {
            return types[i].type;
        }
    }
    return "application/octet-stream";
}

static esp_err_t static_get(httpd_req_t *req)
{
    const char *tail = strcmp(req->uri, "/") == 0 ? "index.html" : req->uri + 1;
    char path[PATH_MAX_LEN];
    char gz[PATH_MAX_LEN + 3];
    if (!map_path(WWW_DIR, tail, path, sizeof(path))) {
        return send_error(req, "400 Bad Request", "bad_path");
    }
    httpd_resp_set_type(req, content_type(path));
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    snprintf(gz, sizeof(gz), "%s.gz", path);
    struct stat st;
    if (stat(gz, &st) == 0) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return stream_file(req, gz);
    }
    esp_err_t err = stream_file(req, path);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return send_error(req, "404 Not Found", "not_found");
    }
    return err;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL || req->content_len == 0 || req->content_len > next->size) {
        return send_error(req, "400 Bad Request", "bad_image_size");
    }
    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    size_t left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, s_io, left < IO_CHUNK ? left : IO_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            esp_ota_abort(ota);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota, s_io, n);
        if (err != ESP_OK) {
            esp_ota_abort(ota);
            return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
        }
        left -= n;
    }
    err = esp_ota_end(ota);
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(next);
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"written\":%u,\"slot\":\"%s\"}", (unsigned)req->content_len,
             next->label);
    send_json(req, "200 OK", body);
    ESP_LOGI(TAG, "OTA image written to %s, restarting", next->label);
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 10;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    cfg.lru_purge_enable = true;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/api/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/format", .method = HTTP_POST, .handler = format_post },
        { .uri = FILES_PREFIX "*", .method = HTTP_PUT, .handler = file_put },
        { .uri = FILES_PREFIX "*", .method = HTTP_GET, .handler = file_get },
        { .uri = FILES_PREFIX "*", .method = HTTP_DELETE, .handler = file_delete },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
        { .uri = "/*", .method = HTTP_GET, .handler = static_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    return ESP_OK;
}
