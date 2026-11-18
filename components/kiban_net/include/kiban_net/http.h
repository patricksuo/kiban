// HTTP(S) GET into a callback or a file. TLS uses the ESP-IDF certificate bundle unless the caller supplies
// a CA in PEM. Products must set CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y, otherwise chains whose top
// certificate is cross-signed by a root missing from the bundle fail (W1, e.g. Cloudflare's GTS Root R4 chain).
// Blocking; call from a task with >= 16 KB of stack when TLS is used.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *url;
    const char *ca_pem;        // NULL: certificate bundle
    uint32_t timeout_ms;       // per network operation (default 15000)
    uint64_t offset;           // > 0: request "Range: bytes=offset-"
} kiban_http_req_t;

typedef struct {
    int status;                // HTTP status (200, 206, ...), 0 if no response
    int64_t content_length;    // from the response headers, -1 if unknown
    uint64_t bytes;            // body bytes delivered by this call
    uint32_t ms;               // wall time
} kiban_http_result_t;

// Return non-ESP_OK from the sink to abort the transfer.
typedef esp_err_t (*kiban_http_sink_t)(const uint8_t *data, size_t len, void *ctx);

esp_err_t kiban_http_get(const kiban_http_req_t *req, kiban_http_sink_t sink, void *ctx, kiban_http_result_t *res);

typedef struct {
    const char *ca_pem;
    uint32_t timeout_ms;
    bool resume;               // continue an existing "<path>.part" with a Range request
} kiban_http_dl_opts_t;

// Download to `path` via "<path>.part"; renamed to `path` only when the body is complete (length matches
// Content-Length when known). An existing `path` is replaced. A server that ignores Range (200) restarts.
esp_err_t kiban_http_download(const char *url, const char *path, const kiban_http_dl_opts_t *opts,
                              kiban_http_result_t *res);

#ifdef __cplusplus
}
#endif
