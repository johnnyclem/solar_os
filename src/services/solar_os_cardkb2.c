#include "solar_os_cardkb2.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_cardkb2_codec.h"
#include "solar_os_input.h"
#include "solar_os_task.h"

#define CARDKB2_READ_TIMEOUT_MS 20U
#define CARDKB2_TASK_STACK 4096U
#define CARDKB2_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define CARDKB2_DEVICE_MAX 2U

typedef struct {
    bool active;
    volatile bool stop_requested;
    volatile bool worker_done;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    solar_os_input_source_t input_source;
    TaskHandle_t worker_task;
    solar_os_cardkb2_parser_t parser;
    solar_os_cardkb2_state_t state;
    uint32_t keys;
    uint32_t dropped;
    uint32_t bus_errors;
} solar_os_cardkb2_device_t;

static const char *TAG = "cardkb2";
static solar_os_cardkb2_device_t cardkb2_devices[CARDKB2_DEVICE_MAX];

static solar_os_cardkb2_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < CARDKB2_DEVICE_MAX; i++) {
        if (cardkb2_devices[i].active &&
            strcmp(cardkb2_devices[i].name, name) == 0) {
            return &cardkb2_devices[i];
        }
    }
    return NULL;
}

static solar_os_cardkb2_device_t *alloc_device(void)
{
    for (size_t i = 0; i < CARDKB2_DEVICE_MAX; i++) {
        if (!cardkb2_devices[i].active) {
            return &cardkb2_devices[i];
        }
    }
    return NULL;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len)
{
    bool have_uart = false;

    if (bindings == NULL || uart_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_bus[0] = '\0';
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind != SOLAR_OS_EXPANSION_BINDING_UART_PORT) {
            return ESP_ERR_INVALID_ARG;
        }
        if (have_uart) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(uart_bus, binding->target, uart_bus_len);
        have_uart = true;
    }
    return have_uart && solar_os_expansion_find_uart_port(uart_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static void cardkb2_emit(solar_os_cardkb2_device_t *device,
                         const solar_os_cardkb2_output_t *output)
{
    if (!output->emit) {
        return;
    }
    if (solar_os_input_write_key(device->input_source,
                                 output->physical,
                                 SOLAR_OS_INPUT_USAGE_NONE,
                                 output->key,
                                 0,
                                 output->pressed ? SOLAR_OS_INPUT_KEY_PRESS :
                                                   SOLAR_OS_INPUT_KEY_RELEASE) == ESP_OK) {
        if (output->pressed) {
            device->keys++;
        }
    } else {
        device->dropped++;
    }
}

static void cardkb2_worker(void *arg)
{
    solar_os_cardkb2_device_t *device = arg;
    bool bus_error_reported = false;

    while (!device->stop_requested) {
        uint8_t buffer[32];
        size_t read_len = 0;
        const esp_err_t read_err = solar_os_bus_uart_read(device->uart_bus,
                                                          buffer,
                                                          sizeof(buffer),
                                                          CARDKB2_READ_TIMEOUT_MS,
                                                          &read_len);
        if (read_len > 0 || read_err == ESP_OK || read_err == ESP_ERR_TIMEOUT) {
            if (read_err != ESP_OK && read_err != ESP_ERR_TIMEOUT) {
                device->bus_errors++;
            } else {
                bus_error_reported = false;
            }
            const uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            for (size_t i = 0; i < read_len; i++) {
                uint8_t key_id = 0;
                uint8_t key_state = 0;
                solar_os_cardkb2_output_t output;
                if (!solar_os_cardkb2_parse(&device->parser,
                                            buffer[i],
                                            &key_id,
                                            &key_state)) {
                    continue;
                }
                if (solar_os_cardkb2_apply(&device->state,
                                           key_id,
                                           key_state == 0x01U,
                                           now_ms,
                                           &output)) {
                    cardkb2_emit(device, &output);
                }
            }
        } else {
            device->bus_errors++;
            if (!bus_error_reported) {
                ESP_LOGW(TAG,
                         "%s read failed on %s: %s",
                         device->name,
                         device->uart_bus,
                         esp_err_to_name(read_err));
                bus_error_reported = true;
            }
        }
    }

    device->worker_done = true;
    solar_os_task_delete_internal(NULL);
}

static void clear_device(solar_os_cardkb2_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_release_all(device->input_source);
        solar_os_input_source_close(device->input_source);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_cardkb2_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    solar_os_cardkb2_device_t *device;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_device(name) != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, uart_bus, sizeof(uart_bus)),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->uart_bus, uart_bus, sizeof(device->uart_bus));

    esp_err_t err = solar_os_input_keyboard_source_open(device->name,
                                                       true,
                                                       &device->input_source);
    if (err != ESP_OK) {
        clear_device(device);
        return err;
    }
    if (solar_os_task_create_pinned_internal(cardkb2_worker,
                                             device->name,
                                             CARDKB2_TASK_STACK,
                                             device,
                                             CARDKB2_TASK_PRIORITY,
                                             &device->worker_task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%s attached on %s at %u baud", name, uart_bus, SOLAR_OS_CARDKB2_BAUD_RATE);
    return ESP_OK;
}

esp_err_t solar_os_cardkb2_detach(const char *name)
{
    solar_os_cardkb2_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    device->stop_requested = true;
    if (device->worker_task != NULL) {
        (void)xTaskNotifyGive(device->worker_task);
    }
    if (!solar_os_task_wait_done(device->worker_task,
                                 &device->worker_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG,
             "%s detached: %lu keys, %lu dropped, %lu bus errors",
             name,
             (unsigned long)device->keys,
             (unsigned long)device->dropped,
             (unsigned long)device->bus_errors);
    clear_device(device);
    return ESP_OK;
}
