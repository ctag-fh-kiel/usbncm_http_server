/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <esp_timer.h>
#include <lwip/apps/netbiosns.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "wired_iface.h"
#include "esp_http_server.h"
//#include "dns_server.h"
#include "mdns.h"


static const char *TAG = "example_sta2wired";
static httpd_handle_t s_web_server = NULL;

static esp_err_t http_get_handler(httpd_req_t *req)
{
    char msg[] = "Hello world!";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, msg, sizeof(msg));

    return ESP_OK;
}

static const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = http_get_handler,
};

static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&s_web_server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(s_web_server, &root);
    }
}

void app_main(void){
    ESP_LOGI(TAG, "Starting netif");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


    // starts the wired interface with virtual network used to configure/provision the example
    wired_netif_init();

    ESP_LOGI(TAG, "Starting webserver");
    start_webserver();
    // Start the DNS server that will reply to "wifi.settings" with "usb" network interface address
    //dns_server_config_t config = DNS_SERVER_CONFIG_SINGLE("ctag.tbd" /* name */, "wired" /* USB netif ID */);
    //start_dns_server(&config);

    ESP_LOGI(TAG, "End app_main");
}
