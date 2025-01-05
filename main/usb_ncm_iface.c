/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/* DESCRIPTION:
 * This example contains code to make ESP32-S2/S3 as a USB network Device.
 */
#include <stdio.h>
#include <lwip/apps/netbiosns.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "tinyusb.h"
#include "tinyusb_net.h"
#include "wired_iface.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "esp_mac.h"
#include "esp_netif.h"    // for network interface management
#include "esp_log.h"       // for logging
#include "lwip/ip_addr.h"  // for IP address configuration
#include "lwip/sys.h"
#include "mdns.h"

static const char *TAG = "example_wired_tusb_ncm";
static esp_netif_t *s_netif = NULL;

// Interface counter
enum interface_count {
#if CFG_TUD_MIDI
    ITF_NUM_MIDI = 0,
    ITF_NUM_MIDI_STREAMING,
#endif
#if CFG_TUD_NCM
    ITF_NUM_NET,
    ITF_NUM_NET_DATA,
#endif
    ITF_COUNT
};

// USB Endpoint numbers
enum usb_endpoints {
    // Available USB Endpoints: 5 IN/OUT EPs and 1 IN EP
    EP_EMPTY = 0,
#if CFG_TUD_MIDI
    EPNUM_MIDI,
#endif
#if CFG_TUD_NCM
    EPNUM_NET_NOTIF,
    EPNUM_NET_DATA,
#endif
};

/** TinyUSB descriptors **/

#define TUSB_DESCRIPTOR_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_MIDI * TUD_MIDI_DESC_LEN + CFG_TUD_NCM * TUD_CDC_NCM_DESC_LEN)

/**
 * @brief String descriptor
 */
static const char *s_str_desc[7] = {
        // array of pointer to string descriptors
        (char[]) {0x09, 0x04},  // 0: is supported language is English (0x0409)
        "TBD",             // 1: Manufacturer
        "TBD-BBA",      // 2: Product
        "123456",              // 3: Serials, should use chip ID
        "TBD midi device", // 4: MIDI
        "TBD network device", // 5: NCM
        "000000000000", // 6: MAC
};

static const uint8_t s_midi_cfg_desc[] = {
        // Configuration number, interface count, string index, total length, attribute, power in mA
        TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, TUSB_DESCRIPTOR_TOTAL_LEN, 0, 100),

        // MIDI Interface number, string index, EP Out & EP In address, EP size
        TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 4, EPNUM_MIDI, (0x80 | EPNUM_MIDI), 64),

        // NCM Interface number, description string index, MAC address string index, EP notification address and size, EP data address (out, in), and size, max segment size
        TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_NET, 5, 6, (0x80 | EPNUM_NET_NOTIF), 64, EPNUM_NET_DATA, (0x80 | EPNUM_NET_DATA), 64, CFG_TUD_NET_MTU),
};

esp_err_t wired_send(void *buffer, uint16_t len, void *buff_free_arg)
{
    return tinyusb_net_send_sync(buffer, len, buff_free_arg, pdMS_TO_TICKS(100));
}

static void l2_free(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t netif_transmit (void *h, void *buffer, size_t len)
{
    if (wired_send(buffer, len, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send buffer to USB!");
    }
    return ESP_OK;
}

static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_netif) {
        void *buf_copy = malloc(len);
        if (!buf_copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(buf_copy, buffer, len);
        return esp_netif_receive(s_netif, buf_copy, len, NULL);
    }
    return ESP_OK;
}

esp_err_t wired_netif_init(void)
{
    /*const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
    };
     */

    tinyusb_config_t const tusb_cfg = {
            .device_descriptor = NULL, // If device_descriptor is NULL, tinyusb_driver_install() will use Kconfig
            .string_descriptor = s_str_desc,
            .string_descriptor_count = 7,
            .external_phy = false,
            .configuration_descriptor = s_midi_cfg_desc,
            .self_powered = false,
            .vbus_monitor_io = 0
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));


    const tinyusb_net_config_t net_config = {
        // locally administrated address for the ncm device as it's going to be used internally
        // for configuration only
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
    };

    esp_err_t ret = tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot initialize USB Net device");
        return ret;
    }

    // with OUI range MAC to create a virtual netif running http server
    // this needs to be different to usb_interface_mac (==client)
    uint8_t lwip_addr[6] =  {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    const esp_netif_ip_info_t ip_cfg = {
            .ip = { .addr = ESP_IP4TOADDR( 192, 168, 4, 1) },
            //.gw = { .addr = ESP_IP4TOADDR( 192, 168, 4, 1) },
            .gw = { .addr = ESP_IP4TOADDR( 0, 0, 0, 0) },
            .netmask = { .addr = ESP_IP4TOADDR( 255, 255, 255, 0) },
    };

    // Definition of
    // 1) Derive the base config (very similar to IDF's default WiFi AP with DHCP server)
    esp_netif_inherent_config_t base_cfg =  {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP, // Run DHCP server; set the netif "ip" immediately
        .ip_info = &ip_cfg,                    // Use the same IP ranges as IDF's soft AP
        .if_key = "wired",                                      // Set mame, key, priority
        .if_desc = "usb ncm config device",
        .route_prio = 10
    };
    // 2) Use static config for driver's config pointing only to static transmit and free functions
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,                // not using an instance, USB-NCM is a static singleton (must be != NULL)
        .transmit = netif_transmit,         // point to static Tx function
        .driver_free_rx_buffer = l2_free    // point to Free Rx buffer function
    };

    // 3) USB-NCM is an Ethernet netif from lwip perspective, we already have IO definitions for that:
    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input
        }
    };

    // Config the esp-netif with:
    //   1) inherent config (behavioural settings of an interface)
    //   2) driver's config (connection to IO functions -- usb)
    //   3) stack config (using lwip IO functions -- derive from eth)
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config
    };

    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_netif, lwip_addr);

    /*
    esp_netif_dns_info_t dns_info = {0};
    IP_ADDR4(&dns_info.ip, 8, 8, 8, 8);
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns_info);
     */

    // set the minimum lease time
    uint32_t  lease_opt = 60;
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease_opt, sizeof(lease_opt));

    // start the interface manually (as the driver has been started already)
    esp_netif_action_start(s_netif, 0, 0, 0);

    vTaskDelay(pdMS_TO_TICKS(5000));

    esp_err_t err;
    err = mdns_init();
    ESP_LOGI(TAG, "mdns_init returned %d", err);
    err = mdns_register_netif(s_netif);
    ESP_LOGI(TAG, "mdns_register_netif returned %d", err);
    err = mdns_netif_action(s_netif, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ENABLE_IP6);
    ESP_LOGI(TAG, "mdns_netif_action returned %d", err);
    err = mdns_netif_action(s_netif, MDNS_EVENT_ANNOUNCE_IP4 | MDNS_EVENT_ANNOUNCE_IP6);
    ESP_LOGI(TAG, "mdns_netif_action returned %d", err);
    err = mdns_netif_action(s_netif, MDNS_EVENT_IP4_REVERSE_LOOKUP | MDNS_EVENT_IP6_REVERSE_LOOKUP);
    ESP_LOGI(TAG, "mdns_netif_action returned %d", err);
    err = mdns_hostname_set("ctag-tbd");
    ESP_LOGI(TAG, "mdns_hostname_set returned %d", err);
    err = mdns_instance_name_set("ctag web server");
    ESP_LOGI(TAG, "mdns_instance_name_set returned %d", err);

    mdns_txt_item_t serviceTxtData[] = {
            {"board", "esp32"},
            {"path", "/"}
    };

    err = mdns_service_add("ctag-tbd", "_http", "_tcp", 80, serviceTxtData,
                           sizeof(serviceTxtData) / sizeof(serviceTxtData[0]));
    ESP_LOGI(TAG, "mdns_service_add returned %d", err);

    netbiosns_init();
    netbiosns_set_name("ctag-tbd");

    return ESP_OK;
}
