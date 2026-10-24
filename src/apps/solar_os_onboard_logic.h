#pragma once

/*
 * Pure helpers behind the onboard app: provider presets, endpoint
 * normalization, the model-list URL and parser, and the classification of a
 * reachability check into a stage the user can act on. No I/O, no ESP-IDF
 * dependencies, so tests/host/onboard_logic_test.c covers all of it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_ONBOARD_ENDPOINT_MAX 256U
#define SOLAR_OS_ONBOARD_MODEL_MAX 96U
#define SOLAR_OS_ONBOARD_MODEL_LIST_MAX 16U
#define SOLAR_OS_ONBOARD_HOST_MAX 128U

typedef struct {
    const char *name;
    const char *detail;
    /* NULL means the user types the endpoint. */
    const char *endpoint;
    /* Preselected in the model list when the server reports it. */
    const char *default_model;
    bool key_required;
} solar_os_onboard_preset_t;

size_t solar_os_onboard_preset_count(void);
const solar_os_onboard_preset_t *solar_os_onboard_preset_at(size_t index);
/* Index of the preset whose endpoint matches exactly, or -1. */
int solar_os_onboard_preset_index_for_endpoint(const char *endpoint);

/*
 * Turns what a person types into an endpoint the agent accepts. Leading and
 * trailing spaces are trimmed, a missing scheme becomes http://, and a URL
 * with no path, "/", or a path ending in /v1 gets /v1/chat/completions.
 * Returns false for an empty value, a non-HTTP scheme, a control character,
 * or a result that does not fit.
 */
bool solar_os_onboard_normalize_endpoint(const char *input,
                                         char *out,
                                         size_t out_len);

/*
 * The OpenAI-style model list next to a chat endpoint: the last path segment
 * /responses, /chat/completions, or /completions becomes /models. Returns
 * false when the endpoint has a query string or none of those suffixes.
 */
bool solar_os_onboard_models_url(const char *endpoint,
                                 char *out,
                                 size_t out_len);

/* Host, port, and scheme of an http:// or https:// URL. */
bool solar_os_onboard_endpoint_host(const char *endpoint,
                                    char *host,
                                    size_t host_len,
                                    uint16_t *port,
                                    bool *tls);

typedef struct {
    char ids[SOLAR_OS_ONBOARD_MODEL_LIST_MAX][SOLAR_OS_ONBOARD_MODEL_MAX];
    size_t count;
    /* Chat-capable IDs in the response, including those not kept. */
    size_t seen;
} solar_os_onboard_model_list_t;

/*
 * Parses {"data":[{"id":"..."},...]}. IDs that name embedding, speech,
 * image, moderation, or similar non-chat models are skipped. When more chat
 * models exist than fit, the lexicographically greatest are kept, which
 * favors newer version numbers. `preferred`, when present in the response,
 * is always kept and placed first. The rest are sorted descending. Returns
 * false when the document is not that shape.
 */
bool solar_os_onboard_parse_models(const char *json,
                                   size_t json_len,
                                   const char *preferred,
                                   solar_os_onboard_model_list_t *list);
bool solar_os_onboard_model_list_contains(
    const solar_os_onboard_model_list_t *list,
    const char *id);

typedef enum {
    SOLAR_OS_ONBOARD_TRANSPORT_OK = 0,
    SOLAR_OS_ONBOARD_TRANSPORT_DNS_FAILED,
    SOLAR_OS_ONBOARD_TRANSPORT_CONNECT_FAILED,
    SOLAR_OS_ONBOARD_TRANSPORT_TIMEOUT,
} solar_os_onboard_transport_t;

typedef enum {
    SOLAR_OS_ONBOARD_CHECK_OK = 0,
    SOLAR_OS_ONBOARD_CHECK_DNS,
    SOLAR_OS_ONBOARD_CHECK_CONNECT,
    SOLAR_OS_ONBOARD_CHECK_TIMEOUT,
    SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED,
    SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST,
    SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED,
    SOLAR_OS_ONBOARD_CHECK_SERVER_ERROR,
    SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR,
} solar_os_onboard_check_t;

solar_os_onboard_check_t solar_os_onboard_classify(
    solar_os_onboard_transport_t transport,
    int http_status);
/* A few words for the status line, such as "key rejected". */
const char *solar_os_onboard_check_label(solar_os_onboard_check_t check);
/* One sentence saying what to do next. */
const char *solar_os_onboard_check_advice(solar_os_onboard_check_t check);
/* True when the next request can still succeed without user changes. */
bool solar_os_onboard_check_is_usable(solar_os_onboard_check_t check);

/* A short hint for an ESP-IDF station disconnect reason, or NULL. */
const char *solar_os_onboard_wifi_reason_hint(uint8_t reason);

/* Parses a 1-based menu choice in [1, count] into a 0-based index. */
bool solar_os_onboard_parse_choice(const char *text,
                                   size_t count,
                                   size_t *index);

/* "****abcd": the last four characters, or "****" for keys of eight or less. */
void solar_os_onboard_mask_secret(const char *secret,
                                  char *out,
                                  size_t out_len);
