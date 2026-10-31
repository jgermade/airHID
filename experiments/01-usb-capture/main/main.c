/*
 * airHID — Fase 1: captura USB Host
 *
 * Enumera dispositivos HID (directos o detrás de un hub) y vuelca cada
 * reporte de entrada por la consola UART, con marca de tiempo y el
 * intervalo desde el reporte anterior de la misma interfaz.
 *
 * Formato de línea (pensado para poder parsearlo después):
 *   R t=<us> dt=<us> a=<addr> i=<iface> p=<proto> n=<len> | xx xx xx ...
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "capture";

#define REPORT_MAX_LEN 64

typedef struct {
    uint8_t addr;
    uint8_t iface;
    uint8_t proto;
    int64_t last_us;
    uint32_t count;
} iface_ctx_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} driver_event_t;

static QueueHandle_t driver_event_queue;

static const char *proto_name(uint8_t proto)
{
    switch (proto) {
    case HID_PROTOCOL_KEYBOARD: return "keyboard";
    case HID_PROTOCOL_MOUSE:    return "mouse";
    default:                    return "none";
    }
}

#if CONFIG_AIRHID_DUMP_REPORT_DESCRIPTOR
static void dump_hex(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf(i % 16 == 0 ? "\n    %02x" : " %02x", data[i]);
    }
    printf("\n");
}
#endif

static void print_report(iface_ctx_t *ctx, const uint8_t *data, size_t len)
{
    int64_t now = esp_timer_get_time();
    int64_t dt = ctx->last_us ? now - ctx->last_us : 0;
    ctx->last_us = now;
    ctx->count++;

    printf("R t=%" PRId64 " dt=%" PRId64 " a=%u i=%u p=%u n=%u |",
           now, dt, ctx->addr, ctx->iface, ctx->proto, (unsigned)len);
    for (size_t i = 0; i < len; i++) {
        printf(" %02x", data[i]);
    }
    printf("\n");
}

static void interface_callback(hid_host_device_handle_t handle,
                               const hid_host_interface_event_t event,
                               void *arg)
{
    iface_ctx_t *ctx = arg;
    uint8_t data[REPORT_MAX_LEN];
    size_t len = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &len) == ESP_OK) {
            print_report(ctx, data, len);
        }
        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "a=%u i=%u desconectado (%" PRIu32 " reportes)",
                 ctx->addr, ctx->iface, ctx->count);
        ESP_ERROR_CHECK(hid_host_device_close(handle));
        free(ctx);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "a=%u i=%u error de transferencia", ctx->addr, ctx->iface);
        break;
    default:
        ESP_LOGW(TAG, "a=%u i=%u evento desconocido %d", ctx->addr, ctx->iface, event);
        break;
    }
}

static void open_device(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    ESP_ERROR_CHECK(hid_host_device_get_params(handle, &params));

    iface_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGE(TAG, "sin memoria para a=%u i=%u", params.addr, params.iface_num);
        return;
    }
    ctx->addr = params.addr;
    ctx->iface = params.iface_num;
    ctx->proto = params.proto;

    ESP_LOGI(TAG, "conectado a=%u i=%u subclass=%u proto=%s",
             params.addr, params.iface_num, params.sub_class, proto_name(params.proto));

    const hid_host_device_config_t dev_config = {
        .callback = interface_callback,
        .callback_arg = ctx,
    };
    esp_err_t err = hid_host_device_open(handle, &dev_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "a=%u i=%u no se pudo abrir: %s",
                 params.addr, params.iface_num, esp_err_to_name(err));
        free(ctx);
        return;
    }

#if CONFIG_AIRHID_DUMP_REPORT_DESCRIPTOR
    size_t desc_len = 0;
    const uint8_t *desc = hid_host_get_report_descriptor(handle, &desc_len);
    if (desc != NULL) {
        printf("D a=%u i=%u n=%u", params.addr, params.iface_num, (unsigned)desc_len);
        dump_hex(desc, desc_len);
    }
#endif

#if CONFIG_AIRHID_FORCE_BOOT_PROTOCOL
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        ESP_ERROR_CHECK(hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT));
    }
#endif
    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        // Sin auto-repetición: el host solo recibe reportes cuando cambia el estado.
        hid_class_request_set_idle(handle, 0, 0);
    }

    ESP_ERROR_CHECK(hid_host_device_start(handle));
}

static void driver_callback(hid_host_device_handle_t handle,
                            const hid_host_driver_event_t event,
                            void *arg)
{
    // Se ejecuta en la tarea del driver HID: se delega a app_main.
    const driver_event_t evt = { .handle = handle, .event = event };
    xQueueSend(driver_event_queue, &evt, 0);
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "airHID fase 1 — captura USB Host");

    driver_event_queue = xQueueCreate(10, sizeof(driver_event_t));
    assert(driver_event_queue);

    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                            xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    assert(ok == pdTRUE);
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));

    const hid_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&driver_config));

    ESP_LOGI(TAG, "esperando dispositivos HID...");

    driver_event_t evt;
    while (true) {
        if (xQueueReceive(driver_event_queue, &evt, portMAX_DELAY) == pdTRUE &&
            evt.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            open_device(evt.handle);
        }
    }
}
