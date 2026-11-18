#include "kiban_net/http.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "kiban.http";
#define CHUNK 4096

esp_err_t kiban_http_get(const kiban_http_req_t *req, kiban_http_sink_t sink, void *ctx, kiban_http_result_t *res)
{
    ESP_RETURN_ON_FALSE(req && req->url && sink, ESP_ERR_INVALID_ARG, TAG, "args");
    kiban_http_result_t r = {.content_length = -1};
    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = req->timeout_ms ? (int)req->timeout_ms : 15000,
        .buffer_size = CHUNK,
        .cert_pem = req->ca_pem,
        .crt_bundle_attach = req->ca_pem ? NULL : esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(c, ESP_ERR_NO_MEM, TAG, "client");
    if (req->offset) {
        char range[48];
        snprintf(range, sizeof range, "bytes=%llu-", (unsigned long long)req->offset);
        esp_http_client_set_header(c, "Range", range);
    }
    uint8_t *buf = malloc(CHUNK);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = buf ? esp_http_client_open(c, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        r.content_length = esp_http_client_fetch_headers(c);
        r.status = esp_http_client_get_status_code(c);
        if (r.status < 200 || r.status >= 300) err = ESP_ERR_INVALID_RESPONSE;
        while (err == ESP_OK) {
            const int n = esp_http_client_read(c, (char *)buf, CHUNK);
            if (n < 0) { err = ESP_FAIL; break; }
            if (n == 0) {
                if (esp_http_client_is_complete_data_received(c)) break;
                err = ESP_ERR_TIMEOUT;   // connection ended early
                break;
            }
            r.bytes += (uint64_t)n;
            err = sink(buf, (size_t)n, ctx);
        }
    }
    r.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (res) *res = r;
    return err;
}

static esp_err_t file_sink(const uint8_t *d, size_t n, void *ctx)
{
    return fwrite(d, 1, n, (FILE *)ctx) == n ? ESP_OK : ESP_FAIL;
}

esp_err_t kiban_http_download(const char *url, const char *path, const kiban_http_dl_opts_t *opts,
                              kiban_http_result_t *res)
{
    ESP_RETURN_ON_FALSE(url && path, ESP_ERR_INVALID_ARG, TAG, "args");
    const kiban_http_dl_opts_t o = opts ? *opts : (kiban_http_dl_opts_t){0};
    char part[256];
    ESP_RETURN_ON_FALSE(snprintf(part, sizeof part, "%s.part", path) < (int)sizeof part, ESP_ERR_INVALID_ARG, TAG, "path");
    uint64_t have = 0;
    struct stat st;
    if (o.resume && stat(part, &st) == 0) have = (uint64_t)st.st_size;
    FILE *f = fopen(part, have ? "ab" : "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s", part);
    kiban_http_req_t req = {.url = url, .ca_pem = o.ca_pem, .timeout_ms = o.timeout_ms, .offset = have};
    kiban_http_result_t r;
    esp_err_t err = kiban_http_get(&req, file_sink, f, &r);
    if (have && r.status == 200) {   // server ignored Range: start over
        fclose(f);
        f = fopen(part, "wb");
        ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "reopen %s", part);
        have = 0;
        req.offset = 0;
        err = kiban_http_get(&req, file_sink, f, &r);
    }
    if (fclose(f) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK && r.content_length >= 0 && r.bytes != (uint64_t)r.content_length) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        unlink(path);
        if (rename(part, path) != 0) err = ESP_FAIL;
    }
    if (res) *res = r;
    return err;
}
