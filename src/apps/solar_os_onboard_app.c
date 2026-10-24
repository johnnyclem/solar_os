#include "solar_os_onboard_app.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "solar_os_agent.h"
#include "solar_os_agent_app.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_onboard_logic.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_wifi.h"

/* The smoke test runs solar_os_agent_run, so the worker needs the agent's stack. */
#define ONBOARD_TASK_STACK 16384U
#define ONBOARD_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(ONBOARD_TASK_STACK);

#define ONBOARD_SAVED_WIFI_TIMEOUT_MS 15000U
#define ONBOARD_WIFI_TIMEOUT_MS 20000U
#define ONBOARD_WIFI_FAIL_GRACE_MS 3000U
#define ONBOARD_WIFI_POLL_MS 250U
#define ONBOARD_HTTP_TIMEOUT_MS 10000U
#define ONBOARD_MODELS_BODY_MAX (96U * 1024U)
#define ONBOARD_INPUT_MAX SOLAR_OS_AGENT_ENDPOINT_MAX
#define ONBOARD_LABEL_WIDTH 15U
#define ONBOARD_REPLY_MAX 64U
#define ONBOARD_ERROR_MAX SOLAR_OS_AGENT_EVENT_TEXT_MAX
#define ONBOARD_SMOKE_PROMPT "Reply with the single word: ok"

_Static_assert(SOLAR_OS_ONBOARD_MODEL_MAX == SOLAR_OS_AGENT_MODEL_MAX,
               "model list entries must fit the agent model setting");
_Static_assert(SOLAR_OS_ONBOARD_ENDPOINT_MAX == SOLAR_OS_AGENT_ENDPOINT_MAX,
               "normalized endpoints must fit the agent endpoint setting");
_Static_assert(SOLAR_OS_AGENT_API_KEY_MAX <= ONBOARD_INPUT_MAX,
               "the input line must hold a full API key");

typedef enum {
    ONBOARD_MODE_SETUP = 0,
    ONBOARD_MODE_CHECK,
    ONBOARD_MODE_FOR_AGENT,
} onboard_mode_t;

typedef enum {
    ONBOARD_STEP_IDLE = 0,
    ONBOARD_STEP_BUSY,
    ONBOARD_STEP_WIFI_PICK,
    ONBOARD_STEP_WIFI_PASSWORD,
    ONBOARD_STEP_PROVIDER,
    ONBOARD_STEP_ENDPOINT,
    ONBOARD_STEP_KEY,
    ONBOARD_STEP_CHECK_FAILED,
    ONBOARD_STEP_MODEL,
    ONBOARD_STEP_SMOKE_FAILED,
    ONBOARD_STEP_DONE,
    ONBOARD_STEP_EXIT,
} onboard_step_t;

typedef enum {
    ONBOARD_JOB_NONE = 0,
    ONBOARD_JOB_WIFI_SAVED,
    ONBOARD_JOB_WIFI_SCAN,
    ONBOARD_JOB_WIFI_CONNECT,
    ONBOARD_JOB_CHECK,
    ONBOARD_JOB_SMOKE,
} onboard_job_t;

/* Written only by the worker while a job runs; read by the UI after task_done. */
typedef struct {
    esp_err_t err;
    /* Wi-Fi */
    solar_os_wifi_ap_t aps[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t ap_count;
    bool wifi_ok;
    uint8_t wifi_reason;
    /* server check */
    solar_os_onboard_check_t check;
    int http_status;
    bool models_listed;
    uint32_t check_ms;
    solar_os_onboard_model_list_t models;
    /* test request */
    bool smoke_ok;
    bool smoke_error_seen;
    int smoke_http_status;
    uint32_t smoke_ms;
    uint32_t smoke_tokens;
    char reply[ONBOARD_REPLY_MAX];
    char error[ONBOARD_ERROR_MAX];
} onboard_result_t;

typedef struct {
    solar_os_context_t *ctx;
    onboard_mode_t mode;
    onboard_step_t step;
    bool reconfigure;
    bool saved_wifi_tried;
    bool problems;

    char input[ONBOARD_INPUT_MAX];
    size_t input_len;
    bool input_masked;

    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char password[SOLAR_OS_WIFI_PASSWORD_MAX + 1];
    int preset;

    onboard_job_t job;
    TaskHandle_t task;
    volatile bool task_done;
    volatile bool stop_requested;
    onboard_result_t result;
} onboard_state_t;

typedef struct {
    onboard_state_t app;
    solar_os_shell_io_t fallback_io;
} onboard_cold_state_t;

static const char *TAG = "onboard";
static void *onboard_state;
#define onboard (((onboard_cold_state_t *)onboard_state)->app)
#define onboard_fallback_io (((onboard_cold_state_t *)onboard_state)->fallback_io)

static void onboard_enter_wifi(void);
static void onboard_enter_provider(void);
static void onboard_enter_key(void);
static void onboard_enter_check(void);
static void onboard_enter_model(void);
static void onboard_enter_smoke(void);
static void onboard_enter_done(void);

bool solar_os_onboard_needed(void)
{
    solar_os_agent_status_t status;
    if (solar_os_agent_get_status(&status) != ESP_OK) {
        return false;
    }
    return !status.configured;
}

/* ---- output -------------------------------------------------------------- */

static solar_os_shell_io_t *onboard_io(void)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(onboard.ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&onboard_fallback_io,
                                        solar_os_context_terminal(onboard.ctx));
        solar_os_context_set_shell_io(onboard.ctx, &onboard_fallback_io);
        io = &onboard_fallback_io;
    }
    return io;
}

static void onboard_vprint(const char *fmt, va_list args)
{
    char line[320];
    vsnprintf(line, sizeof(line), fmt, args);
    solar_os_shell_io_write(onboard_io(), line);
}

static void onboard_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    onboard_vprint(fmt, args);
    va_end(args);
    solar_os_shell_io_flush(onboard_io());
}

/* "  Label ........ " with no newline; the result follows on the same line. */
static void onboard_label(const char *label)
{
    solar_os_shell_io_t *io = onboard_io();
    char line[ONBOARD_LABEL_WIDTH + 8U];
    snprintf(line, sizeof(line), "  %s ", label);
    size_t len = strlen(line);
    while (len < ONBOARD_LABEL_WIDTH + 2U && len + 1U < sizeof(line)) {
        line[len++] = '.';
    }
    line[len] = '\0';
    solar_os_shell_io_write(io, line);
    solar_os_shell_io_put_char(io, ' ');
    solar_os_shell_io_flush(io);
}

static void onboard_line(const char *label, const char *fmt, ...)
{
    onboard_label(label);
    va_list args;
    va_start(args, fmt);
    onboard_vprint(fmt, args);
    va_end(args);
    solar_os_shell_io_newline(onboard_io());
    solar_os_shell_io_flush(onboard_io());
}

static void onboard_say(const char *fmt, ...)
{
    solar_os_shell_io_write(onboard_io(), "  ");
    va_list args;
    va_start(args, fmt);
    onboard_vprint(fmt, args);
    va_end(args);
    solar_os_shell_io_newline(onboard_io());
    solar_os_shell_io_flush(onboard_io());
}

static void onboard_wipe_input(void)
{
    memset(onboard.input, 0, sizeof(onboard.input));
    onboard.input_len = 0;
}

static void onboard_prompt(onboard_step_t step, bool masked, const char *fmt, ...)
{
    solar_os_shell_io_t *io = onboard_io();
    solar_os_shell_io_write(io, "  ");
    va_list args;
    va_start(args, fmt);
    onboard_vprint(fmt, args);
    va_end(args);
    solar_os_shell_io_write(io, "\n  > ");
    solar_os_shell_io_flush(io);
    onboard_wipe_input();
    onboard.input_masked = masked;
    onboard.step = step;
}

static void onboard_finish(void)
{
    solar_os_shell_io_t *io = onboard_io();
    solar_os_shell_io_printf(io, "  %s exits\n", solar_os_shell_io_app_exit_key(io));
    solar_os_shell_io_flush(io);
    onboard.step = ONBOARD_STEP_EXIT;
}

/* ---- worker -------------------------------------------------------------- */

static uint32_t onboard_elapsed_ms(int64_t start_us)
{
    const int64_t elapsed = esp_timer_get_time() - start_us;
    return elapsed > 0 ? (uint32_t)(elapsed / 1000) : 0U;
}

static bool onboard_wait_for_ip(uint32_t timeout_ms)
{
    const int64_t start = esp_timer_get_time();
    solar_os_wifi_status_t status;
    while (!onboard.stop_requested) {
        solar_os_wifi_get_status(&status);
        if (status.connected && status.has_ip) {
            return true;
        }
        const uint32_t elapsed = onboard_elapsed_ms(start);
        if (elapsed >= timeout_ms) {
            break;
        }
        if (elapsed >= ONBOARD_WIFI_FAIL_GRACE_MS &&
            status.state == SOLAR_OS_WIFI_STATE_FAILED) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(ONBOARD_WIFI_POLL_MS));
    }
    solar_os_wifi_get_status(&status);
    onboard.result.wifi_reason = status.disconnect_reason;
    return false;
}

static void onboard_job_wifi_saved(void)
{
    onboard.result.err = solar_os_wifi_connect_saved();
    onboard.result.wifi_ok = onboard.result.err == ESP_OK &&
        onboard_wait_for_ip(ONBOARD_SAVED_WIFI_TIMEOUT_MS);
}

static void onboard_job_wifi_scan(void)
{
    solar_os_wifi_ap_t found[SOLAR_OS_WIFI_SCAN_MAX_RESULTS];
    size_t count = 0;
    onboard.result.ap_count = 0;
    onboard.result.err = solar_os_wifi_scan(found, SOLAR_OS_WIFI_SCAN_MAX_RESULTS, &count);
    if (onboard.result.err != ESP_OK) {
        return;
    }
    /* One row per visible SSID, strongest signal first. */
    for (size_t i = 0; i < count && i < SOLAR_OS_WIFI_SCAN_MAX_RESULTS; i++) {
        if (found[i].hidden || found[i].ssid[0] == '\0') {
            continue;
        }
        size_t slot = onboard.result.ap_count;
        for (size_t j = 0; j < onboard.result.ap_count; j++) {
            if (strcmp(onboard.result.aps[j].ssid, found[i].ssid) == 0) {
                slot = j;
                break;
            }
        }
        if (slot == onboard.result.ap_count) {
            onboard.result.aps[onboard.result.ap_count++] = found[i];
        } else if (found[i].rssi > onboard.result.aps[slot].rssi) {
            onboard.result.aps[slot] = found[i];
        }
    }
    for (size_t i = 1; i < onboard.result.ap_count; i++) {
        solar_os_wifi_ap_t ap = onboard.result.aps[i];
        size_t j = i;
        while (j > 0 && onboard.result.aps[j - 1U].rssi < ap.rssi) {
            onboard.result.aps[j] = onboard.result.aps[j - 1U];
            j--;
        }
        onboard.result.aps[j] = ap;
    }
}

static void onboard_job_wifi_connect(void)
{
    onboard.result.err = solar_os_wifi_connect(onboard.ssid, onboard.password);
    memset(onboard.password, 0, sizeof(onboard.password));
    onboard.result.wifi_ok = onboard.result.err == ESP_OK &&
        onboard_wait_for_ip(ONBOARD_WIFI_TIMEOUT_MS);
}

static bool onboard_resolve(const char *host)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    const int rc = getaddrinfo(host, NULL, &hints, &res);
    if (res != NULL) {
        freeaddrinfo(res);
    }
    return rc == 0;
}

static void onboard_job_check(void)
{
    onboard_result_t *r = &onboard.result;
    r->check = SOLAR_OS_ONBOARD_CHECK_HTTP_ERROR;
    r->http_status = 0;
    r->models_listed = false;
    memset(&r->models, 0, sizeof(r->models));
    const int64_t start = esp_timer_get_time();

    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);

    char host[SOLAR_OS_ONBOARD_HOST_MAX];
    if (!solar_os_onboard_endpoint_host(status.endpoint, host, sizeof(host), NULL, NULL)) {
        r->check = SOLAR_OS_ONBOARD_CHECK_CONNECT;
        return;
    }
    if (!onboard_resolve(host)) {
        r->check = solar_os_onboard_classify(SOLAR_OS_ONBOARD_TRANSPORT_DNS_FAILED, 0);
        r->check_ms = onboard_elapsed_ms(start);
        return;
    }

    char models_url[SOLAR_OS_ONBOARD_ENDPOINT_MAX];
    if (!solar_os_onboard_models_url(status.endpoint, models_url, sizeof(models_url))) {
        /* Nothing to ask without sending a chat request; the test request covers it. */
        r->check = SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST;
        r->check_ms = onboard_elapsed_ms(start);
        return;
    }

    solar_os_http_buffered_response_t response;
    const esp_err_t err = solar_os_agent_probe_get(models_url,
                                                   ONBOARD_MODELS_BODY_MAX,
                                                   ONBOARD_HTTP_TIMEOUT_MS,
                                                   &onboard.stop_requested,
                                                   &response);
    solar_os_onboard_transport_t transport = SOLAR_OS_ONBOARD_TRANSPORT_OK;
    if (err == ESP_ERR_TIMEOUT || response.response.deadline_exceeded) {
        transport = SOLAR_OS_ONBOARD_TRANSPORT_TIMEOUT;
    } else if (err != ESP_OK) {
        transport = SOLAR_OS_ONBOARD_TRANSPORT_CONNECT_FAILED;
    }
    r->http_status = response.response.status_code;
    r->check = solar_os_onboard_classify(transport, r->http_status);
    if (r->check == SOLAR_OS_ONBOARD_CHECK_OK) {
        const solar_os_onboard_preset_t *preset =
            solar_os_onboard_preset_at(onboard.preset >= 0 ? (size_t)onboard.preset : SIZE_MAX);
        const char *preferred = status.model[0] != '\0' ? status.model :
            (preset != NULL ? preset->default_model : NULL);
        r->models_listed = !response.body_truncated &&
            solar_os_onboard_parse_models((const char *)response.body,
                                          response.body_len,
                                          preferred,
                                          &r->models);
        if (!r->models_listed) {
            /* The server answered with something other than a model list. */
            r->check = SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST;
        }
    }
    solar_os_http_buffered_response_clear(&response);
    r->check_ms = onboard_elapsed_ms(start);
    SOLAR_OS_LOGI(TAG, "check: %s, HTTP %d, %u models",
                  solar_os_onboard_check_label(r->check),
                  r->http_status,
                  (unsigned)r->models.count);
}

static esp_err_t onboard_smoke_event(const solar_os_agent_event_t *event, void *user_data)
{
    (void)user_data;
    onboard_result_t *r = &onboard.result;
    if (event == NULL) {
        return ESP_OK;
    }
    switch (event->type) {
    case SOLAR_OS_AGENT_EVENT_TEXT_DELTA: {
        const size_t used = strlen(r->reply);
        if (used + 1U < sizeof(r->reply)) {
            strlcat(r->reply, event->text, sizeof(r->reply));
        }
        break;
    }
    case SOLAR_OS_AGENT_EVENT_USAGE:
        r->smoke_tokens = event->total_tokens;
        break;
    case SOLAR_OS_AGENT_EVENT_ERROR:
        if (!r->smoke_error_seen) {
            strlcpy(r->error, event->text, sizeof(r->error));
        }
        r->smoke_error_seen = true;
        break;
    case SOLAR_OS_AGENT_EVENT_DONE:
        if (!event->success && r->error[0] == '\0') {
            strlcpy(r->error, event->text, sizeof(r->error));
            r->smoke_error_seen = true;
        }
        break;
    default:
        break;
    }
    return onboard.stop_requested ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void onboard_job_smoke(void)
{
    onboard_result_t *r = &onboard.result;
    r->smoke_ok = false;
    r->smoke_error_seen = false;
    r->smoke_tokens = 0;
    r->reply[0] = '\0';
    r->error[0] = '\0';
    const solar_os_agent_request_t request = {
        .prompt = ONBOARD_SMOKE_PROMPT,
        .event_handler = onboard_smoke_event,
    };
    const int64_t start = esp_timer_get_time();
    r->err = solar_os_agent_run(&request);
    r->smoke_ms = onboard_elapsed_ms(start);
    solar_os_agent_status_t status;
    if (solar_os_agent_get_status(&status) == ESP_OK) {
        r->smoke_http_status = status.last_http_status;
    }
    r->smoke_ok = r->err == ESP_OK && !r->smoke_error_seen;
}

static void onboard_task(void *arg)
{
    (void)arg;
    switch (onboard.job) {
    case ONBOARD_JOB_WIFI_SAVED:
        onboard_job_wifi_saved();
        break;
    case ONBOARD_JOB_WIFI_SCAN:
        onboard_job_wifi_scan();
        break;
    case ONBOARD_JOB_WIFI_CONNECT:
        onboard_job_wifi_connect();
        break;
    case ONBOARD_JOB_CHECK:
        onboard_job_check();
        break;
    case ONBOARD_JOB_SMOKE:
        onboard_job_smoke();
        break;
    default:
        break;
    }
    __sync_synchronize();
    onboard.task_done = true;
    solar_os_task_delete_internal(NULL);
}

static bool onboard_start_job(onboard_job_t job)
{
    onboard.job = job;
    onboard.task_done = false;
    onboard.step = ONBOARD_STEP_BUSY;
    const BaseType_t created = solar_os_task_create_pinned_internal(
        onboard_task,
        "solar_os_onboard",
        ONBOARD_TASK_STACK,
        NULL,
        ONBOARD_TASK_PRIORITY,
        &onboard.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        onboard.job = ONBOARD_JOB_NONE;
        onboard.task = NULL;
        onboard.task_done = true;
        onboard_print("\n");
        onboard_say("Could not start a worker task (out of memory).");
        onboard_finish();
        return false;
    }
    return true;
}

/* ---- Wi-Fi --------------------------------------------------------------- */

static void onboard_print_wifi_connected(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    onboard_print("connected (%s, %s)\n", status.ssid, status.ip);
}

static void onboard_after_wifi(void)
{
    if (onboard.mode != ONBOARD_MODE_CHECK && !solar_os_wifi_enabled_for_next_boot() &&
        solar_os_wifi_set_enabled_for_next_boot(true) == ESP_OK) {
        onboard_line("Wi-Fi at boot", "turned back on");
    }
    if (onboard.mode == ONBOARD_MODE_FOR_AGENT && !solar_os_onboard_needed()) {
        onboard_enter_done();
        return;
    }
    onboard_enter_provider();
}

static void onboard_show_networks(void)
{
    if (onboard.result.ap_count == 0) {
        onboard_say("No networks found.");
    }
    for (size_t i = 0; i < onboard.result.ap_count; i++) {
        const solar_os_wifi_ap_t *ap = &onboard.result.aps[i];
        onboard_print("    %2u  %-24.24s %4d dBm  %s\n",
                      (unsigned)(i + 1U),
                      ap->ssid,
                      (int)ap->rssi,
                      ap->auth);
    }
    onboard_prompt(ONBOARD_STEP_WIFI_PICK,
                   false,
                   "Network number, a hidden network's name, or r to rescan:");
}

static void onboard_start_scan(void)
{
    onboard_label("Wi-Fi");
    onboard_print("scanning\n");
    (void)onboard_start_job(ONBOARD_JOB_WIFI_SCAN);
}

static void onboard_enter_wifi(void)
{
    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);
    if (status.connected && status.has_ip) {
        onboard_label("Wi-Fi");
        onboard_print_wifi_connected();
        onboard_after_wifi();
        return;
    }
    if (!solar_os_wifi_enabled_for_current_boot()) {
        (void)solar_os_wifi_set_enabled_for_next_boot(true);
        onboard_line("Wi-Fi", "off for this boot");
        onboard_say("Wi-Fi is now enabled for the next boot. Reboot, then run onboard again.");
        onboard.problems = true;
        onboard_finish();
        return;
    }
    if (onboard.mode == ONBOARD_MODE_CHECK) {
        onboard_line("Wi-Fi", "not connected");
        onboard_say("Run onboard to join a network.");
        onboard.problems = true;
        onboard_finish();
        return;
    }
    if (!onboard.saved_wifi_tried &&
        (status.has_saved_config || status.saved_profile_count > 0U)) {
        onboard.saved_wifi_tried = true;
        onboard_label("Wi-Fi");
        onboard_print("connecting to %s\n",
                      status.saved_ssid[0] != '\0' ? status.saved_ssid : "a saved network");
        (void)onboard_start_job(ONBOARD_JOB_WIFI_SAVED);
        return;
    }
    onboard_start_scan();
}

static void onboard_connect_selected(void)
{
    onboard_label("Wi-Fi");
    onboard_print("connecting to %s\n", onboard.ssid);
    (void)onboard_start_job(ONBOARD_JOB_WIFI_CONNECT);
}

static void onboard_submit_wifi_pick(const char *text)
{
    if (strcmp(text, "r") == 0 || strcmp(text, "R") == 0) {
        onboard_start_scan();
        return;
    }
    size_t index;
    if (solar_os_onboard_parse_choice(text, onboard.result.ap_count, &index)) {
        const solar_os_wifi_ap_t *ap = &onboard.result.aps[index];
        strlcpy(onboard.ssid, ap->ssid, sizeof(onboard.ssid));
        if (strcmp(ap->auth, "open") == 0) {
            onboard.password[0] = '\0';
            onboard_connect_selected();
        } else {
            onboard_prompt(ONBOARD_STEP_WIFI_PASSWORD, true, "Password for %s:", onboard.ssid);
        }
        return;
    }
    if (text[0] == '\0') {
        onboard_prompt(ONBOARD_STEP_WIFI_PICK, false, "Pick a number, type a name, or r:");
        return;
    }
    if (strlen(text) > SOLAR_OS_WIFI_SSID_MAX) {
        onboard_prompt(ONBOARD_STEP_WIFI_PICK, false, "Network names are at most 32 characters:");
        return;
    }
    strlcpy(onboard.ssid, text, sizeof(onboard.ssid));
    onboard_prompt(ONBOARD_STEP_WIFI_PASSWORD,
                   true,
                   "Password for %s (Enter if open):",
                   onboard.ssid);
}

static void onboard_submit_wifi_password(const char *text)
{
    if (strlen(text) > SOLAR_OS_WIFI_PASSWORD_MAX) {
        onboard_prompt(ONBOARD_STEP_WIFI_PASSWORD, true, "Passwords are at most 64 characters:");
        return;
    }
    strlcpy(onboard.password, text, sizeof(onboard.password));
    onboard_connect_selected();
}

/* ---- provider, endpoint, key -------------------------------------------- */

static void onboard_print_saved_settings(const solar_os_agent_status_t *status)
{
    onboard_line("Endpoint", "%s", status->endpoint);
    onboard_line("Model", "%s", status->model);
    onboard_line("API key", "%s", status->api_key_set ? "saved" : "none");
}

static void onboard_enter_provider(void)
{
    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);
    onboard.preset = solar_os_onboard_preset_index_for_endpoint(status.endpoint);

    if (onboard.mode == ONBOARD_MODE_CHECK) {
        if (!status.configured) {
            onboard_line("Agent", "not set up");
            onboard_say("Run onboard to choose a server and model.");
            onboard.problems = true;
            onboard_finish();
            return;
        }
        onboard_print_saved_settings(&status);
        onboard_enter_check();
        return;
    }
    if (status.configured && !onboard.reconfigure) {
        onboard_print_saved_settings(&status);
        onboard_enter_check();
        return;
    }

    onboard_print("\n");
    onboard_say("Where should the agent send requests?");
    for (size_t i = 0; i < solar_os_onboard_preset_count(); i++) {
        const solar_os_onboard_preset_t *preset = solar_os_onboard_preset_at(i);
        onboard_print("    %u  %s: %s\n", (unsigned)(i + 1U), preset->name, preset->detail);
    }
    onboard_prompt(ONBOARD_STEP_PROVIDER, false, "Choose a number:");
}

static void onboard_submit_provider(const char *text)
{
    size_t index;
    if (!solar_os_onboard_parse_choice(text, solar_os_onboard_preset_count(), &index)) {
        onboard_prompt(ONBOARD_STEP_PROVIDER, false, "Choose a number from the list:");
        return;
    }
    onboard.preset = (int)index;
    const solar_os_onboard_preset_t *preset = solar_os_onboard_preset_at(index);
    if (preset->endpoint == NULL) {
        onboard_prompt(ONBOARD_STEP_ENDPOINT,
                       false,
                       "Server URL, for example 192.168.1.20:11434 for Ollama:");
        return;
    }
    const esp_err_t err = solar_os_agent_set_endpoint(preset->endpoint);
    if (err != ESP_OK) {
        onboard_say("Could not save the endpoint: %s", esp_err_to_name(err));
        onboard_finish();
        return;
    }
    onboard_line("Endpoint", "%s", preset->endpoint);
    onboard_enter_key();
}

static void onboard_submit_endpoint(const char *text)
{
    char endpoint[SOLAR_OS_ONBOARD_ENDPOINT_MAX];
    if (!solar_os_onboard_normalize_endpoint(text, endpoint, sizeof(endpoint))) {
        onboard_prompt(ONBOARD_STEP_ENDPOINT,
                       false,
                       "That is not an http:// or https:// address. Server URL:");
        return;
    }
    const esp_err_t err = solar_os_agent_set_endpoint(endpoint);
    if (err != ESP_OK) {
        onboard_prompt(ONBOARD_STEP_ENDPOINT,
                       false,
                       "The agent rejected that URL (%s). Server URL:",
                       esp_err_to_name(err));
        return;
    }
    onboard_line("Endpoint", "%s", endpoint);
    onboard_enter_key();
}

static bool onboard_key_required(void)
{
    const solar_os_onboard_preset_t *preset =
        onboard.preset >= 0 ? solar_os_onboard_preset_at((size_t)onboard.preset) : NULL;
    return preset != NULL && preset->key_required;
}

static void onboard_enter_key(void)
{
    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);
    if (status.api_key_set) {
        onboard_prompt(ONBOARD_STEP_KEY, true, "API key (Enter keeps the saved key):");
    } else if (onboard_key_required()) {
        onboard_prompt(ONBOARD_STEP_KEY, true, "API key:");
    } else {
        onboard_prompt(ONBOARD_STEP_KEY, true, "API key (Enter if the server needs none):");
    }
}

static void onboard_submit_key(const char *text)
{
    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);
    if (text[0] == '\0') {
        if (!status.api_key_set && onboard_key_required()) {
            onboard_prompt(ONBOARD_STEP_KEY, true, "This provider needs a key. API key:");
            return;
        }
        onboard_line("API key", "%s", status.api_key_set ? "kept" : "none");
        onboard_enter_check();
        return;
    }
    const esp_err_t err = solar_os_agent_set_api_key(text);
    if (err != ESP_OK) {
        onboard_prompt(ONBOARD_STEP_KEY,
                       true,
                       "The key was not saved (%s). API key:",
                       esp_err_to_name(err));
        return;
    }
    char masked[16];
    solar_os_onboard_mask_secret(text, masked, sizeof(masked));
    onboard_line("API key", "saved (%s)", masked);
    onboard_enter_check();
}

/* ---- server check -------------------------------------------------------- */

static void onboard_enter_check(void)
{
    onboard_label("Server check");
    (void)onboard_start_job(ONBOARD_JOB_CHECK);
}

static void onboard_offer_check_retry(void)
{
    onboard_say("r retries, e changes the server, k changes the key.");
    onboard.step = ONBOARD_STEP_CHECK_FAILED;
}

static void onboard_check_finished(void)
{
    const onboard_result_t *r = &onboard.result;
    switch (r->check) {
    case SOLAR_OS_ONBOARD_CHECK_OK:
        onboard_print("ok, %u model%s listed (%u ms)\n",
                      (unsigned)r->models.seen,
                      r->models.seen == 1U ? "" : "s",
                      (unsigned)r->check_ms);
        break;
    case SOLAR_OS_ONBOARD_CHECK_NO_MODEL_LIST:
    case SOLAR_OS_ONBOARD_CHECK_RATE_LIMITED:
        onboard_print("%s\n", solar_os_onboard_check_label(r->check));
        onboard_say("%s", solar_os_onboard_check_advice(r->check));
        break;
    default:
        if (r->http_status > 0) {
            onboard_print("%s (HTTP %d)\n", solar_os_onboard_check_label(r->check), r->http_status);
        } else {
            onboard_print("%s\n", solar_os_onboard_check_label(r->check));
        }
        onboard_say("%s", solar_os_onboard_check_advice(r->check));
        break;
    }

    if (onboard.mode == ONBOARD_MODE_CHECK) {
        if (!solar_os_onboard_check_is_usable(r->check)) {
            onboard.problems = true;
            onboard_enter_done();
            return;
        }
        onboard_enter_model();
        return;
    }
    if (r->check == SOLAR_OS_ONBOARD_CHECK_KEY_REJECTED) {
        onboard_prompt(ONBOARD_STEP_KEY, true, "API key:");
        return;
    }
    if (!solar_os_onboard_check_is_usable(r->check)) {
        onboard_offer_check_retry();
        return;
    }
    onboard_enter_model();
}

/* ---- model --------------------------------------------------------------- */

static int onboard_default_model_index(const char *current)
{
    const solar_os_onboard_model_list_t *models = &onboard.result.models;
    if (models->count == 0U) {
        return -1;
    }
    for (size_t i = 0; i < models->count; i++) {
        if (current != NULL && strcmp(models->ids[i], current) == 0) {
            return (int)i;
        }
    }
    /* The parser pins the preset's default first when the server offers it. */
    return 0;
}

static void onboard_enter_model(void)
{
    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);
    const solar_os_onboard_model_list_t *models = &onboard.result.models;
    const bool listed = onboard.result.models_listed;
    const bool current_listed = solar_os_onboard_model_list_contains(models, status.model);

    if (status.model[0] != '\0' && !onboard.reconfigure &&
        (!listed || current_listed)) {
        /* A saved model the server offers, or a server that lists nothing. */
        if (onboard.mode == ONBOARD_MODE_CHECK || listed) {
            onboard_line("Model", "%s%s", status.model, listed ? " (offered)" : "");
        }
        onboard_enter_smoke();
        return;
    }
    if (onboard.mode == ONBOARD_MODE_CHECK) {
        onboard_line("Model", "%s is not offered by this server", status.model);
        onboard.problems = true;
        onboard_enter_done();
        return;
    }
    if (status.model[0] != '\0' && listed && !current_listed && !onboard.reconfigure) {
        onboard_line("Model", "%s is not offered by this server", status.model);
    }

    if (models->count == 0U) {
        if (status.model[0] != '\0') {
            onboard_prompt(ONBOARD_STEP_MODEL, false, "Model name (Enter keeps %s):", status.model);
        } else {
            onboard_prompt(ONBOARD_STEP_MODEL, false, "Model name:");
        }
        return;
    }

    const int def = onboard_default_model_index(status.model);
    onboard_print("\n");
    for (size_t i = 0; i < models->count; i++) {
        onboard_print("   %c%2u  %s\n",
                      (int)i == def ? '*' : ' ',
                      (unsigned)(i + 1U),
                      models->ids[i]);
    }
    if (models->seen > models->count) {
        onboard_say("%u more on the server; type any name to use one of those.",
                    (unsigned)(models->seen - models->count));
    }
    onboard_prompt(ONBOARD_STEP_MODEL, false, "Model number or name (Enter picks *):");
}

static void onboard_submit_model(const char *text)
{
    solar_os_agent_status_t status;
    (void)solar_os_agent_get_status(&status);
    const solar_os_onboard_model_list_t *models = &onboard.result.models;
    char model[SOLAR_OS_AGENT_MODEL_MAX];
    size_t index;

    if (text[0] == '\0') {
        const int def = onboard_default_model_index(status.model);
        if (def >= 0) {
            strlcpy(model, models->ids[def], sizeof(model));
        } else if (status.model[0] != '\0') {
            strlcpy(model, status.model, sizeof(model));
        } else {
            onboard_prompt(ONBOARD_STEP_MODEL, false, "Type the model name:");
            return;
        }
    } else if (models->count > 0U &&
               solar_os_onboard_parse_choice(text, models->count, &index)) {
        strlcpy(model, models->ids[index], sizeof(model));
    } else if (strlen(text) < sizeof(model) && strchr(text, ' ') == NULL) {
        strlcpy(model, text, sizeof(model));
    } else {
        onboard_prompt(ONBOARD_STEP_MODEL, false, "Model names have no spaces. Model:");
        return;
    }

    const esp_err_t err = solar_os_agent_set_model(model);
    if (err != ESP_OK) {
        onboard_prompt(ONBOARD_STEP_MODEL,
                       false,
                       "The model was not saved (%s). Model:",
                       esp_err_to_name(err));
        return;
    }
    onboard_line("Model", "%s", model);
    onboard.reconfigure = false;
    onboard_enter_smoke();
}

/* ---- test request -------------------------------------------------------- */

static void onboard_enter_smoke(void)
{
    onboard_label("Test request");
    (void)onboard_start_job(ONBOARD_JOB_SMOKE);
}

static void onboard_smoke_finished(void)
{
    const onboard_result_t *r = &onboard.result;
    if (r->smoke_ok) {
        char reply[24];
        size_t n = 0;
        for (const char *p = r->reply; *p != '\0' && n + 1U < sizeof(reply); p++) {
            if (*p >= 0x20 && *p <= 0x7e) {
                reply[n++] = *p;
            }
        }
        reply[n] = '\0';
        onboard_print("ok, \"%s\" in %u.%u s", reply,
                      (unsigned)(r->smoke_ms / 1000U),
                      (unsigned)((r->smoke_ms % 1000U) / 100U));
        if (r->smoke_tokens > 0U) {
            onboard_print(", %u tokens", (unsigned)r->smoke_tokens);
        }
        onboard_print("\n");
        onboard_enter_done();
        return;
    }

    const int http = r->smoke_http_status;
    if (r->err == ESP_ERR_INVALID_STATE && r->error[0] == '\0') {
        onboard_print("busy\n");
        onboard_say("Another agent request is running. Finish it, then retry.");
    } else if (http > 0) {
        onboard_print("failed (HTTP %d)\n", http);
    } else {
        onboard_print("failed\n");
    }
    if (r->error[0] != '\0') {
        onboard_say("%s", r->error);
    }

    if (onboard.mode == ONBOARD_MODE_CHECK) {
        onboard.problems = true;
        onboard_enter_done();
        return;
    }
    if (http == 401 || http == 403) {
        onboard_say("The server refused the key.");
        onboard_prompt(ONBOARD_STEP_KEY, true, "API key:");
        return;
    }
    if (http == 400 || http == 404 || http == 422) {
        onboard_say("The server rejected the request. This is usually the model name.");
        onboard.reconfigure = true;
        onboard_enter_model();
        return;
    }
    onboard_say("r retries, m changes the model, k the key, e the server.");
    onboard.step = ONBOARD_STEP_SMOKE_FAILED;
}

/* ---- done ---------------------------------------------------------------- */

static void onboard_launch_agent(void)
{
    char arg0[] = "agent";
    char *argv[] = {arg0};
    const esp_err_t err =
        solar_os_context_request_launch(onboard.ctx, &solar_os_agent_app, 1, argv);
    if (err != ESP_OK) {
        onboard_say("Could not start the agent: %s", esp_err_to_name(err));
        onboard_finish();
    }
}

static void onboard_enter_done(void)
{
    onboard_print("\n");
    if (onboard.mode == ONBOARD_MODE_CHECK) {
        onboard_say(onboard.problems ?
                        "Problems found. Run onboard to fix them." :
                        "Everything works.");
        onboard_finish();
        return;
    }
    if (onboard.mode == ONBOARD_MODE_FOR_AGENT) {
        onboard_say("Ready. Starting the agent.");
        onboard_launch_agent();
        return;
    }
    onboard_say("Ready. Settings are saved and survive a reboot.");
    onboard_say("Enter starts the agent. c changes the server, key, or model.");
    onboard_finish();
    onboard.step = ONBOARD_STEP_DONE;
}

/* ---- dispatch ------------------------------------------------------------ */

static void onboard_job_finished(onboard_job_t job)
{
    switch (job) {
    case ONBOARD_JOB_WIFI_SAVED:
        onboard_label("Wi-Fi");
        if (onboard.result.wifi_ok) {
            onboard_print_wifi_connected();
            onboard_after_wifi();
        } else {
            onboard_print("saved network did not connect\n");
            onboard_start_scan();
        }
        break;
    case ONBOARD_JOB_WIFI_SCAN:
        if (onboard.result.err != ESP_OK) {
            onboard_line("Wi-Fi", "scan failed: %s", esp_err_to_name(onboard.result.err));
            onboard_prompt(ONBOARD_STEP_WIFI_PICK, false, "r rescans, or type a network name:");
        } else {
            onboard_show_networks();
        }
        break;
    case ONBOARD_JOB_WIFI_CONNECT:
        onboard_label("Wi-Fi");
        if (onboard.result.wifi_ok) {
            onboard_print_wifi_connected();
            onboard_after_wifi();
        } else {
            const char *hint = solar_os_onboard_wifi_reason_hint(onboard.result.wifi_reason);
            if (onboard.result.err != ESP_OK) {
                onboard_print("could not connect: %s\n", esp_err_to_name(onboard.result.err));
            } else {
                onboard_print("did not connect%s%s\n", hint != NULL ? ": " : "", hint != NULL ? hint : "");
            }
            onboard_show_networks();
        }
        break;
    case ONBOARD_JOB_CHECK:
        onboard_check_finished();
        break;
    case ONBOARD_JOB_SMOKE:
        onboard_smoke_finished();
        break;
    default:
        break;
    }
}

static void onboard_submit(void)
{
    char text[ONBOARD_INPUT_MAX];
    strlcpy(text, onboard.input, sizeof(text));
    onboard_wipe_input();
    solar_os_shell_io_newline(onboard_io());
    solar_os_shell_io_flush(onboard_io());

    const onboard_step_t step = onboard.step;
    onboard.step = ONBOARD_STEP_IDLE;
    switch (step) {
    case ONBOARD_STEP_WIFI_PICK:
        onboard_submit_wifi_pick(text);
        break;
    case ONBOARD_STEP_WIFI_PASSWORD:
        onboard_submit_wifi_password(text);
        break;
    case ONBOARD_STEP_PROVIDER:
        onboard_submit_provider(text);
        break;
    case ONBOARD_STEP_ENDPOINT:
        onboard_submit_endpoint(text);
        break;
    case ONBOARD_STEP_KEY:
        onboard_submit_key(text);
        break;
    case ONBOARD_STEP_MODEL:
        onboard_submit_model(text);
        break;
    default:
        break;
    }
    memset(text, 0, sizeof(text));
}

static bool onboard_step_takes_line(onboard_step_t step)
{
    switch (step) {
    case ONBOARD_STEP_WIFI_PICK:
    case ONBOARD_STEP_WIFI_PASSWORD:
    case ONBOARD_STEP_PROVIDER:
    case ONBOARD_STEP_ENDPOINT:
    case ONBOARD_STEP_KEY:
    case ONBOARD_STEP_MODEL:
        return true;
    default:
        return false;
    }
}

static void onboard_handle_key(uint8_t ch)
{
    const char key = (char)(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    switch (onboard.step) {
    case ONBOARD_STEP_CHECK_FAILED:
        if (key == 'r') {
            onboard_enter_check();
        } else if (key == 'e') {
            onboard.reconfigure = true;
            onboard_enter_provider();
        } else if (key == 'k') {
            onboard_enter_key();
        }
        break;
    case ONBOARD_STEP_SMOKE_FAILED:
        if (key == 'r') {
            onboard_enter_smoke();
        } else if (key == 'm') {
            onboard.reconfigure = true;
            onboard_enter_model();
        } else if (key == 'k') {
            onboard_enter_key();
        } else if (key == 'e') {
            onboard.reconfigure = true;
            onboard_enter_provider();
        }
        break;
    case ONBOARD_STEP_DONE:
        if (ch == '\r' || ch == '\n') {
            onboard_launch_agent();
        } else if (key == 'c') {
            onboard.reconfigure = true;
            onboard_enter_provider();
        }
        break;
    default:
        break;
    }
}

static void onboard_handle_line_char(uint8_t ch)
{
    solar_os_shell_io_t *io = onboard_io();
    if (ch == '\r' || ch == '\n') {
        onboard_submit();
        return;
    }
    if (ch == '\b' || ch == 0x7fU) {
        if (onboard.input_len > 0) {
            onboard.input[--onboard.input_len] = '\0';
            solar_os_shell_io_write(io, "\b \b");
            solar_os_shell_io_flush(io);
        }
        return;
    }
    if (ch >= 0x20U && ch <= 0x7eU && onboard.input_len + 1U < sizeof(onboard.input)) {
        onboard.input[onboard.input_len++] = (char)ch;
        onboard.input[onboard.input_len] = '\0';
        solar_os_shell_io_put_char(io, onboard.input_masked ? '*' : (char)ch);
        solar_os_shell_io_flush(io);
    }
}

static void onboard_wipe_secrets(void)
{
    onboard_wipe_input();
    memset(onboard.password, 0, sizeof(onboard.password));
}

static void onboard_request_stop(void)
{
    onboard.stop_requested = true;
    if (onboard.job == ONBOARD_JOB_SMOKE) {
        (void)solar_os_agent_cancel();
    }
}

static bool onboard_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    onboard.ctx = ctx;
    if (event->type == SOLAR_OS_EVENT_TICK) {
        if (onboard.job != ONBOARD_JOB_NONE && onboard.task_done) {
            const onboard_job_t job = onboard.job;
            onboard.job = ONBOARD_JOB_NONE;
            onboard.task = NULL;
            if (!onboard.stop_requested) {
                onboard_job_finished(job);
            }
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
        onboard_request_stop();
        onboard_wipe_secrets();
        solar_os_context_request_terminal_preserve(ctx);
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (ch == SOLAR_OS_KEY_PAGE_UP || ch == SOLAR_OS_KEY_PAGE_DOWN) {
        solar_os_terminal_t *terminal = solar_os_shell_io_terminal(onboard_io());
        if (terminal != NULL) {
            if (ch == SOLAR_OS_KEY_PAGE_UP) {
                solar_os_terminal_page_up(terminal);
            } else {
                solar_os_terminal_page_down(terminal);
            }
        }
        return true;
    }
    if (onboard_step_takes_line(onboard.step)) {
        onboard_handle_line_char(ch);
    } else {
        onboard_handle_key(ch);
    }
    return true;
}

static esp_err_t onboard_start(solar_os_context_t *ctx)
{
    if (onboard.task != NULL && !onboard.task_done) {
        onboard.ctx = ctx;
        onboard_say("onboard: the previous run is still stopping");
        return ESP_OK;
    }
    memset(&onboard, 0, sizeof(onboard));
    onboard.ctx = ctx;
    onboard.preset = -1;
    onboard.task_done = true;
    (void)solar_os_agent_init();

    const int argc = solar_os_context_argc(ctx);
    const char *arg = argc >= 2 ? solar_os_context_argv(ctx, 1) : "";
    if (argc == 2 && strcmp(arg, "check") == 0) {
        onboard.mode = ONBOARD_MODE_CHECK;
    } else if (argc == 2 && strcmp(arg, "--for-agent") == 0) {
        onboard.mode = ONBOARD_MODE_FOR_AGENT;
    } else if (argc != 1) {
        onboard_print("usage: onboard [check]\n");
        onboard_finish();
        return ESP_OK;
    }

    solar_os_shell_io_t *io = onboard_io();
    solar_os_shell_io_clear(io);
    if (onboard.mode == ONBOARD_MODE_CHECK) {
        solar_os_shell_io_write_bold(io, "Agent check");
        solar_os_shell_io_newline(io);
    } else {
        solar_os_shell_io_write_bold(io, "Agent setup");
        solar_os_shell_io_newline(io);
        if (onboard.mode == ONBOARD_MODE_FOR_AGENT) {
            onboard_say("The agent needs a few things before it can start.");
        }
        onboard_say("Each answer is saved as you go. %s leaves at any time.",
                    solar_os_shell_io_app_exit_key(io));
    }
    solar_os_shell_io_flush(io);
    onboard_enter_wifi();
    return ESP_OK;
}

static void onboard_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    onboard_request_stop();
    if (onboard.task != NULL &&
        !solar_os_task_wait_done(onboard.task, &onboard.task_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "onboard worker did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
    }
    onboard_wipe_secrets();
}

static bool onboard_state_release_ready(void)
{
    return onboard.task == NULL || onboard.task_done;
}

static void onboard_state_release_cleanup(void)
{
    onboard_wipe_secrets();
}

const solar_os_app_t solar_os_onboard_app = {
    .name = "onboard",
    .summary = "set up Wi-Fi and the agent",
    .start = onboard_start,
    .stop = onboard_stop,
    .event = onboard_event,
    .state_slot = &onboard_state,
    .state_size = sizeof(onboard_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .state_release_ready = onboard_state_release_ready,
    .state_release_cleanup = onboard_state_release_cleanup,
    .worker_stack_bytes = ONBOARD_TASK_STACK,
};
