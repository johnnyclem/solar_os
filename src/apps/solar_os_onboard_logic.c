#include "solar_os_onboard_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define ONBOARD_JSON_DEPTH_MAX 32

static const solar_os_onboard_preset_t onboard_presets[] = {
    {
        .name = "OpenAI",
        .detail = "Responses API, needs an API key",
        .endpoint = "https://api.openai.com/v1/responses",
        .default_model = NULL,
        .key_required = true,
    },
    {
        .name = "Local or other server",
        .detail = "any OpenAI-compatible URL: Ollama, LM Studio, llama.cpp",
        .endpoint = NULL,
        .default_model = NULL,
        .key_required = false,
    },
};

size_t solar_os_onboard_preset_count(void)
{
    return sizeof(onboard_presets) / sizeof(onboard_presets[0]);
}

const solar_os_onboard_preset_t *solar_os_onboard_preset_at(size_t index)
{
    return index < solar_os_onboard_preset_count() ? &onboard_presets[index] : NULL;
}

int solar_os_onboard_preset_index_for_endpoint(const char *endpoint)
{
    if (endpoint == NULL || endpoint[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < solar_os_onboard_preset_count(); i++) {
        if (onboard_presets[i].endpoint != NULL &&
            strcmp(onboard_presets[i].endpoint, endpoint) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool onboard_ends_with(const char *text, size_t len, const char *suffix)
{
    const size_t suffix_len = strlen(suffix);
    return len >= suffix_len && memcmp(text + len - suffix_len, suffix, suffix_len) == 0;
}

/* Offset of the first '/' after "scheme://host[:port]", or the length. */
static size_t onboard_path_offset(const char *url)
{
    const char *authority = strstr(url, "://");
    if (authority == NULL) {
        return strlen(url);
    }
    authority += 3;
    const char *slash = strchr(authority, '/');
    return slash != NULL ? (size_t)(slash - url) : strlen(url);
}

bool solar_os_onboard_normalize_endpoint(const char *input,
                                         char *out,
                                         size_t out_len)
{
    if (input == NULL || out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';

    while (*input == ' ' || *input == '\t') {
        input++;
    }
    size_t len = strlen(input);
    while (len > 0U && (input[len - 1U] == ' ' || input[len - 1U] == '\t')) {
        len--;
    }
    if (len == 0U) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)input[i];
        if (ch < 0x21U || ch == 0x7fU) {
            return false;
        }
    }

    char buffer[SOLAR_OS_ONBOARD_ENDPOINT_MAX];
    const char *scheme_end = NULL;
    for (size_t i = 0; i + 2U < len; i++) {
        if (input[i] == ':' && input[i + 1U] == '/' && input[i + 2U] == '/') {
            scheme_end = input + i;
            break;
        }
    }
    int written;
    if (scheme_end == NULL) {
        written = snprintf(buffer, sizeof(buffer), "http://%.*s", (int)len, input);
    } else {
        const size_t scheme_len = (size_t)(scheme_end - input);
        const bool http = scheme_len == 4U && strncasecmp(input, "http", 4) == 0;
        const bool https = scheme_len == 5U && strncasecmp(input, "https", 5) == 0;
        if (!http && !https) {
            return false;
        }
        written = snprintf(buffer,
                           sizeof(buffer),
                           "%s%.*s",
                           https ? "https" : "http",
                           (int)(len - scheme_len),
                           input + scheme_len);
    }
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return false;
    }

    size_t total = (size_t)written;
    const size_t authority = (size_t)(strstr(buffer, "://") - buffer) + 3U;
    const size_t path = onboard_path_offset(buffer);
    if (path == authority) {
        return false; /* no host */
    }
    const char *append = NULL;
    if (path >= total || strcmp(buffer + path, "/") == 0) {
        total = path;
        append = "/v1/chat/completions";
    } else if (onboard_ends_with(buffer, total, "/v1")) {
        append = "/chat/completions";
    } else if (onboard_ends_with(buffer, total, "/v1/")) {
        append = "chat/completions";
    }
    if (append != NULL) {
        buffer[total] = '\0';
        if (total + strlen(append) >= sizeof(buffer)) {
            return false;
        }
        strcat(buffer, append);
    }
    if (strlen(buffer) >= out_len) {
        return false;
    }
    strcpy(out, buffer);
    return true;
}

bool solar_os_onboard_models_url(const char *endpoint, char *out, size_t out_len)
{
    if (endpoint == NULL || out == NULL || out_len == 0U) {
        return false;
    }
    out[0] = '\0';
    if (strchr(endpoint, '?') != NULL || strchr(endpoint, '#') != NULL) {
        return false;
    }
    static const char * const suffixes[] = {
        "/chat/completions",
        "/responses",
        "/completions",
    };
    const size_t len = strlen(endpoint);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (!onboard_ends_with(endpoint, len, suffixes[i])) {
            continue;
        }
        const size_t base = len - strlen(suffixes[i]);
        if (strstr(endpoint, "://") == NULL || base < onboard_path_offset(endpoint)) {
            return false;
        }
        const int written = snprintf(out, out_len, "%.*s/models", (int)base, endpoint);
        if (written < 0 || (size_t)written >= out_len) {
            out[0] = '\0';
            return false;
        }
        return true;
    }
    return false;
}

bool solar_os_onboard_endpoint_host(const char *endpoint,
                                    char *host,
                                    size_t host_len,
                                    uint16_t *port,
                                    bool *tls)
{
    if (endpoint == NULL || host == NULL || host_len == 0U) {
        return false;
    }
    host[0] = '\0';
    bool is_tls;
    const char *p;
    if (strncasecmp(endpoint, "https://", 8) == 0) {
        is_tls = true;
        p = endpoint + 8;
    } else if (strncasecmp(endpoint, "http://", 7) == 0) {
        is_tls = false;
        p = endpoint + 7;
    } else {
        return false;
    }

    const char *host_start = p;
    const char *host_end;
    if (*p == '[') {
        host_start = p + 1;
        host_end = strchr(host_start, ']');
        if (host_end == NULL) {
            return false;
        }
        p = host_end + 1;
    } else {
        while (*p != '\0' && *p != ':' && *p != '/') {
            p++;
        }
        host_end = p;
    }
    const size_t name_len = (size_t)(host_end - host_start);
    if (name_len == 0U || name_len >= host_len) {
        return false;
    }

    unsigned long port_value = is_tls ? 443UL : 80UL;
    if (*p == ':') {
        p++;
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        port_value = 0;
        while (isdigit((unsigned char)*p)) {
            port_value = port_value * 10UL + (unsigned long)(*p - '0');
            if (port_value > 65535UL) {
                return false;
            }
            p++;
        }
        if (port_value == 0UL) {
            return false;
        }
    }
    if (*p != '\0' && *p != '/') {
        return false;
    }

    memcpy(host, host_start, name_len);
    host[name_len] = '\0';
    if (port != NULL) {
        *port = (uint16_t)port_value;
    }
    if (tls != NULL) {
        *tls = is_tls;
    }
    return true;
}

/* ---- a bounded JSON walker, just enough for {"data":[{"id":...}]} ---- */

typedef struct {
    const char *p;
    const char *end;
} onboard_json_t;

static void json_skip_ws(onboard_json_t *j)
{
    while (j->p < j->end &&
           (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static bool json_hex4(const char *p)
{
    for (int i = 0; i < 4; i++) {
        if (!isxdigit((unsigned char)p[i])) {
            return false;
        }
    }
    return true;
}

/* Reads a string at j->p. out may be NULL to skip. Non-ASCII escapes become '?'. */
static bool json_string(onboard_json_t *j, char *out, size_t out_len, bool *fits)
{
    if (j->p >= j->end || *j->p != '"') {
        return false;
    }
    j->p++;
    size_t n = 0;
    bool ok_fit = true;
    while (j->p < j->end) {
        char ch = *j->p++;
        if (ch == '"') {
            if (out != NULL && out_len > 0U) {
                out[n < out_len ? n : out_len - 1U] = '\0';
            }
            if (fits != NULL) {
                *fits = ok_fit;
            }
            return true;
        }
        if ((unsigned char)ch < 0x20U) {
            return false;
        }
        if (ch == '\\') {
            if (j->p >= j->end) {
                return false;
            }
            const char esc = *j->p++;
            switch (esc) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
                if (j->end - j->p < 4 || !json_hex4(j->p)) {
                    return false;
                }
                {
                    unsigned value = 0;
                    for (int i = 0; i < 4; i++) {
                        const char h = j->p[i];
                        value = value * 16U +
                            (unsigned)(isdigit((unsigned char)h) ? h - '0' :
                                       tolower((unsigned char)h) - 'a' + 10);
                    }
                    ch = value < 0x80U ? (char)value : '?';
                }
                j->p += 4;
                break;
            default:
                return false;
            }
        }
        if (out != NULL) {
            if (n + 1U < out_len) {
                out[n++] = ch;
            } else {
                ok_fit = false;
            }
        }
    }
    return false;
}

static bool json_skip_value(onboard_json_t *j, int depth);

static bool json_skip_container(onboard_json_t *j, int depth, char open, char close)
{
    if (depth > ONBOARD_JSON_DEPTH_MAX || *j->p != open) {
        return false;
    }
    j->p++;
    json_skip_ws(j);
    if (j->p < j->end && *j->p == close) {
        j->p++;
        return true;
    }
    while (j->p < j->end) {
        if (open == '{') {
            if (!json_string(j, NULL, 0, NULL)) {
                return false;
            }
            json_skip_ws(j);
            if (j->p >= j->end || *j->p != ':') {
                return false;
            }
            j->p++;
            json_skip_ws(j);
        }
        if (!json_skip_value(j, depth + 1)) {
            return false;
        }
        json_skip_ws(j);
        if (j->p >= j->end) {
            return false;
        }
        if (*j->p == ',') {
            j->p++;
            json_skip_ws(j);
            continue;
        }
        if (*j->p == close) {
            j->p++;
            return true;
        }
        return false;
    }
    return false;
}

static bool json_skip_value(onboard_json_t *j, int depth)
{
    json_skip_ws(j);
    if (j->p >= j->end) {
        return false;
    }
    switch (*j->p) {
    case '"':
        return json_string(j, NULL, 0, NULL);
    case '{':
        return json_skip_container(j, depth, '{', '}');
    case '[':
        return json_skip_container(j, depth, '[', ']');
    default: {
        const char *start = j->p;
        while (j->p < j->end &&
               (isalnum((unsigned char)*j->p) || *j->p == '-' || *j->p == '+' ||
                *j->p == '.')) {
            j->p++;
        }
        return j->p > start;
    }
    }
}

static bool onboard_contains_ci(const char *text, const char *needle)
{
    const size_t n = strlen(needle);
    for (const char *p = text; *p != '\0'; p++) {
        if (strncasecmp(p, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool onboard_is_chat_model(const char *id)
{
    static const char * const excluded[] = {
        "embed", "tts", "whisper", "dall-e", "moderation", "image", "audio",
        "transcribe", "realtime", "rerank", "davinci", "babbage", "search",
    };
    if (id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        if (onboard_contains_ci(id, excluded[i])) {
            return false;
        }
    }
    return true;
}

bool solar_os_onboard_model_list_contains(const solar_os_onboard_model_list_t *list,
                                          const char *id)
{
    if (list == NULL || id == NULL) {
        return false;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

/* Keeps ids sorted descending and bounded; drops the smallest when full. */
static void onboard_rank_insert(solar_os_onboard_model_list_t *list, const char *id)
{
    if (solar_os_onboard_model_list_contains(list, id)) {
        return;
    }
    size_t pos = 0;
    while (pos < list->count && strcmp(list->ids[pos], id) > 0) {
        pos++;
    }
    if (pos >= SOLAR_OS_ONBOARD_MODEL_LIST_MAX) {
        return;
    }
    size_t last = list->count < SOLAR_OS_ONBOARD_MODEL_LIST_MAX ?
        list->count : SOLAR_OS_ONBOARD_MODEL_LIST_MAX - 1U;
    for (size_t i = last; i > pos; i--) {
        memcpy(list->ids[i], list->ids[i - 1U], sizeof(list->ids[i]));
    }
    size_t id_len = strlen(id);
    if (id_len >= sizeof(list->ids[pos])) {
        id_len = sizeof(list->ids[pos]) - 1U;
    }
    memcpy(list->ids[pos], id, id_len);
    list->ids[pos][id_len] = '\0';
    if (list->count < SOLAR_OS_ONBOARD_MODEL_LIST_MAX) {
        list->count++;
    }
}

static bool onboard_parse_model_entry(onboard_json_t *j,
                                      const char *preferred,
                                      bool *preferred_found,
                                      solar_os_onboard_model_list_t *list)
{
    j->p++; /* '{' */
    json_skip_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return true;
    }
    char id[SOLAR_OS_ONBOARD_MODEL_MAX];
    bool have_id = false;
    while (j->p < j->end) {
        char key[8];
        bool key_fits = false;
        if (!json_string(j, key, sizeof(key), &key_fits)) {
            return false;
        }
        json_skip_ws(j);
        if (j->p >= j->end || *j->p != ':') {
            return false;
        }
        j->p++;
        json_skip_ws(j);
        if (key_fits && strcmp(key, "id") == 0 && j->p < j->end && *j->p == '"') {
            bool id_fits = false;
            if (!json_string(j, id, sizeof(id), &id_fits)) {
                return false;
            }
            have_id = id_fits && onboard_is_chat_model(id);
        } else if (!json_skip_value(j, 2)) {
            return false;
        }
        json_skip_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            json_skip_ws(j);
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            if (have_id) {
                list->seen++;
                if (preferred != NULL && strcmp(id, preferred) == 0) {
                    *preferred_found = true;
                } else {
                    onboard_rank_insert(list, id);
                }
            }
            return true;
        }
        return false;
    }
    return false;
}

static bool onboard_parse_data_array(onboard_json_t *j,
                                     const char *preferred,
                                     bool *preferred_found,
                                     solar_os_onboard_model_list_t *list,
                                     bool partial)
{
    if (j->p >= j->end || *j->p != '[') {
        return false;
    }
    j->p++;
    json_skip_ws(j);
    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return true;
    }
    while (j->p < j->end) {
        json_skip_ws(j);
        bool ok;
        if (j->p < j->end && *j->p == '{') {
            ok = onboard_parse_model_entry(j, preferred, preferred_found, list);
        } else {
            ok = json_skip_value(j, 1);
        }
        if (!ok) {
            return partial && j->p >= j->end && list->seen > 0U;
        }
        json_skip_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            return true;
        }
        return partial && j->p >= j->end && list->seen > 0U;
    }
    return partial && list->seen > 0U;
}

static bool onboard_parse_models(const char *json,
                                 size_t json_len,
                                 const char *preferred,
                                 solar_os_onboard_model_list_t *list,
                                 bool partial)
{
    if (json == NULL || json_len == 0U) {
        return false;
    }
    if (preferred != NULL && preferred[0] == '\0') {
        preferred = NULL;
    }

    onboard_json_t j = {.p = json, .end = json + json_len};
    json_skip_ws(&j);
    if (j.p >= j.end || *j.p != '{') {
        return false;
    }
    j.p++;
    json_skip_ws(&j);
    bool saw_data = false;
    bool preferred_found = false;
    while (j.p < j.end && *j.p != '}') {
        char key[8];
        bool key_fits = false;
        if (!json_string(&j, key, sizeof(key), &key_fits)) {
            return false;
        }
        json_skip_ws(&j);
        if (j.p >= j.end || *j.p != ':') {
            return false;
        }
        j.p++;
        json_skip_ws(&j);
        if (key_fits && strcmp(key, "data") == 0) {
            if (!onboard_parse_data_array(&j, preferred, &preferred_found, list, partial)) {
                return false;
            }
            saw_data = true;
        } else if (!json_skip_value(&j, 1)) {
            if (partial && j.p >= j.end && saw_data) {
                break;
            }
            return false;
        }
        json_skip_ws(&j);
        if (j.p < j.end && *j.p == ',') {
            j.p++;
            json_skip_ws(&j);
        } else if (partial && j.p >= j.end && saw_data) {
            break;
        } else if (j.p >= j.end || *j.p != '}') {
            return false;
        }
    }
    if (!saw_data) {
        return false;
    }

    if (preferred_found && strlen(preferred) < SOLAR_OS_ONBOARD_MODEL_MAX) {
        size_t last = list->count < SOLAR_OS_ONBOARD_MODEL_LIST_MAX ?
            list->count : SOLAR_OS_ONBOARD_MODEL_LIST_MAX - 1U;
        for (size_t i = last; i > 0U; i--) {
            memcpy(list->ids[i], list->ids[i - 1U], sizeof(list->ids[i]));
        }
        strcpy(list->ids[0], preferred);
        if (list->count < SOLAR_OS_ONBOARD_MODEL_LIST_MAX) {
            list->count++;
        }
    }
    return true;
}

bool solar_os_onboard_parse_models(const char *json,
                                   size_t json_len,
                                   const char *preferred,
                                   solar_os_onboard_model_list_t *list)
{
    if (list == NULL) {
        return false;
    }
    memset(list, 0, sizeof(*list));
    if (!onboard_parse_models(json, json_len, preferred, list, false)) {
        memset(list, 0, sizeof(*list));
        return false;
    }
    return true;
}

bool solar_os_onboard_parse_models_partial(const char *json,
                                           size_t json_len,
                                           const char *preferred,
                                           solar_os_onboard_model_list_t *list)
{
    if (list == NULL) {
        return false;
    }
    memset(list, 0, sizeof(*list));
    if (!onboard_parse_models(json, json_len, preferred, list, true) ||
        list->seen == 0U) {
        memset(list, 0, sizeof(*list));
        return false;
    }
    return true;
}

solar_os_onboard_check_t solar_os_onboard_classify(solar_os_onboard_transport_t transport,
                                                   int http_status)
{
    switch (transport) {
    case SOLAR_OS_ONBOARD_TRANSPORT_DNS_FAILED:
        return SOLAR_OS_ONBOARD_CHECK_DNS;
    case SOLAR_OS_ONBOARD_TRANSPORT_CONNECT_FAILED:
        return SOLAR_OS_ONBOARD_CHECK_CONNECT;
    case SOLAR_OS_ONBOARD_TRANSPORT_TIMEOUT:
        return SOLAR_OS_ONBOARD_CHECK_TIMEOUT;
    case SOLAR_OS_ONBOARD_TRANSPORT_OK:
    default:
        break;
    }
    if (http_status >= 200 && http_status < 300) {
        return SOLAR_OS_ONBOARD_CHECK_OK;
    }
    if (http_status == 401 || http_status == 403) {
        return SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED;
    }
    if (http_status == 404 || http_status == 405 || http_status == 501) {
        return SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST;
    }
    if (http_status == 429) {
        return SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED;
    }
    if (http_status >= 500 && http_status < 600) {
        return SOLAR_OS_ONBOARD_CHECK_SERVER_ERROR;
    }
    return SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR;
}

const char *solar_os_onboard_check_label(solar_os_onboard_check_t check)
{
    switch (check) {
    case SOLAR_OS_ONBOARD_CHECK_OK:
        return "ok";
    case SOLAR_OS_ONBOARD_CHECK_DNS:
        return "server name not found";
    case SOLAR_OS_ONBOARD_CHECK_CONNECT:
        return "could not connect";
    case SOLAR_OS_ONBOARD_CHECK_TIMEOUT:
        return "timed out";
    case SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED:
        return "key rejected";
    case SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST:
        return "reachable, no model list";
    case SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED:
        return "rate limited";
    case SOLAR_OS_ONBOARD_CHECK_SERVER_ERROR:
        return "server error";
    case SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR:
    default:
        return "unexpected response";
    }
}

const char *solar_os_onboard_check_advice(solar_os_onboard_check_t check)
{
    switch (check) {
    case SOLAR_OS_ONBOARD_CHECK_OK:
        return "The server answered and accepted the key.";
    case SOLAR_OS_ONBOARD_CHECK_DNS:
        return "Wi-Fi works but the host name did not resolve. Check the endpoint spelling.";
    case SOLAR_OS_ONBOARD_CHECK_CONNECT:
        return "The host resolved but refused or failed the connection. Check the port and http vs https.";
    case SOLAR_OS_ONBOARD_CHECK_TIMEOUT:
        return "No answer in time. Check that the server is running and reachable from this network.";
    case SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED:
        return "The server refused the API key. Paste it again.";
    case SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST:
        return "The server answered but does not list models. Type the model name.";
    case SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED:
        return "The key works but is rate limited or out of credit right now.";
    case SOLAR_OS_ONBOARD_CHECK_SERVER_ERROR:
        return "The server had an internal error. Try again in a minute.";
    case SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR:
    default:
        return "The server answered with an unexpected status. Check the endpoint URL.";
    }
}

bool solar_os_onboard_check_is_usable(solar_os_onboard_check_t check)
{
    return check == SOLAR_OS_ONBOARD_CHECK_OK ||
        check == SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST ||
        check == SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED;
}

const char *solar_os_onboard_wifi_reason_hint(uint8_t reason)
{
    /* Values from esp_wifi_types.h wifi_err_reason_t. */
    switch (reason) {
    case 2:   /* AUTH_EXPIRE */
    case 15:  /* 4WAY_HANDSHAKE_TIMEOUT */
    case 202: /* AUTH_FAIL */
    case 204: /* HANDSHAKE_TIMEOUT */
        return "wrong password?";
    case 201: /* NO_AP_FOUND */
        return "network not in range";
    case 205: /* CONNECTION_FAIL */
        return "the access point refused the connection";
    default:
        return NULL;
    }
}

bool solar_os_onboard_parse_choice(const char *text, size_t count, size_t *index)
{
    if (text == NULL || index == NULL || count == 0U) {
        return false;
    }
    while (*text == ' ') {
        text++;
    }
    if (!isdigit((unsigned char)*text)) {
        return false;
    }
    size_t value = 0;
    while (isdigit((unsigned char)*text)) {
        value = value * 10U + (size_t)(*text - '0');
        if (value > count) {
            return false;
        }
        text++;
    }
    while (*text == ' ') {
        text++;
    }
    if (*text != '\0' || value == 0U) {
        return false;
    }
    *index = value - 1U;
    return true;
}

void solar_os_onboard_mask_secret(const char *secret, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    const size_t len = secret != NULL ? strlen(secret) : 0U;
    if (len <= 8U) {
        snprintf(out, out_len, "****");
        return;
    }
    snprintf(out, out_len, "****%s", secret + len - 4U);
}
