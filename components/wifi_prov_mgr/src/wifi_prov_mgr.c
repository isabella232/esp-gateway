// Copyright 2022 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>

#include <wifi_provisioning/manager.h>

#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE
#include <wifi_provisioning/scheme_ble.h>
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE */

#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP
#include <wifi_provisioning/scheme_softap.h>
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP */
#include "qrcode.h"

#include "esp_gateway.h"

static const char *TAG = "esp_gateway_wifi_prov_mgr";

static bool wifi_prov_status = false;
static esp_timer_handle_t deinit_wifi_prov_mgr_timer = NULL;

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;

#define PROV_QR_VERSION         "v1"
#define PROV_TRANSPORT_SOFTAP   "softap"
#define PROV_TRANSPORT_BLE      "ble"
#define QRCODE_BASE_URL         "https://espressif.github.io/esp-jumpstart/qrcode.html"

/* Register Wi-Fi Provisioning events */
static void wifi_prov_event_register(void);

/* Unregister Wi-Fi Provisioning events */
static void wifi_prov_event_unregister(void);

bool wifi_provision_in_progress(void)
{
    return wifi_prov_status;
}

static void deinit_wifi_prov_mgr_timer_callback(void* arg)
{
    wifi_prov_event_unregister();

    if (wifi_provision_in_progress()) {
        wifi_prov_mgr_endpoint_unregister("custom-data");
        wifi_prov_mgr_stop_provisioning();
        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();
        wifi_prov_status = false;
    }

    ESP_ERROR_CHECK(esp_timer_delete(deinit_wifi_prov_mgr_timer));
}

/* Event handler for catching system events */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
#ifdef CONFIG_ESP_GATEWAY_RESET_PROV_MGR_ON_FAILURE
    static int retries;
#endif
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                esp_gateway_wifi_set_config_into_flash(ESP_IF_WIFI_STA, (wifi_config_t*)wifi_sta_cfg);
#if CONFIG_LITEMESH_ENABLE
                esp_litemesh_connect();
#else
                esp_wifi_disconnect();

                esp_wifi_connect();
#endif /* CONFIG_LITEMESH_ENABLE */
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
#ifdef CONFIG_ESP_GATEWAY_RESET_PROV_MGR_ON_FAILURE
                retries++;
                if (retries >= CONFIG_ESP_GATEWAY_PROV_MGR_MAX_RETRY_CNT) {
                    ESP_LOGI(TAG, "Failed to connect with provisioned AP, reseting provisioned credentials");
                    wifi_prov_mgr_reset_sm_state_on_failure();
                    retries = 0;
                }
#endif
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
#ifdef CONFIG_ESP_GATEWAY_RESET_PROV_MGR_ON_FAILURE
                retries = 0;
#endif
                break;
            case WIFI_PROV_END:
                esp_timer_stop(deinit_wifi_prov_mgr_timer);
                esp_err_t stat = esp_timer_delete(deinit_wifi_prov_mgr_timer);
                if (stat != ESP_OK) {
                    ESP_LOGI(TAG, "%s failed to delete timer, err 0x%x\n", __func__, stat);
                }
                wifi_prov_event_unregister();
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                wifi_prov_status = false;
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// register Wi-Fi Provisioning events
static void wifi_prov_event_register(void)
{
    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
}

// unregister Wi-Fi Provisioning events
static void wifi_prov_event_unregister(void)
{
    /* Unregister our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
}

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

/* Handler for the optional provisioning endpoint registered by the application.
 * The data format can be chosen by applications. Here, we are using plain ascii text.
 * Applications can choose to use other formats like protobuf, JSON, XML, etc.
 */
esp_err_t custom_prov_data_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen,
                                          uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    if (inbuf) {
        ESP_LOGI(TAG, "Received data: %.*s", inlen, (char *)inbuf);
    }
    char response[] = "SUCCESS";
    *outbuf = (uint8_t *)strdup(response);
    if (*outbuf == NULL) {
        ESP_LOGE(TAG, "System out of memory");
        return ESP_ERR_NO_MEM;
    }
    *outlen = strlen(response) + 1; /* +1 for NULL terminating byte */

    return ESP_OK;
}

static void wifi_prov_print_qr(const char *name, const char *pop, const char *transport)
{
    if (!name || !transport) {
        ESP_LOGW(TAG, "Cannot generate QR code payload. Data missing.");
        return;
    }
    char payload[150] = {0};
    if (pop) {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\"" \
                    ",\"pop\":\"%s\",\"transport\":\"%s\"}",
                    PROV_QR_VERSION, name, pop, transport);
    } else {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\"" \
                    ",\"transport\":\"%s\"}",
                    PROV_QR_VERSION, name, transport);
    }
#ifdef CONFIG_ESP_GATEWAY_PROV_SHOW_QR
    ESP_LOGI(TAG, "Scan this QR code from the provisioning application for Provisioning.");
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    esp_qrcode_generate(&cfg, payload);
#endif /* CONFIG_APP_WIFI_PROV_SHOW_QR */
    ESP_LOGI(TAG, "If QR code is not visible, copy paste the below URL in a browser.\n%s?data=%s", QRCODE_BASE_URL, payload);
}

void esp_gateway_wifi_prov_mgr(void)
{
    wifi_prov_status = true;

    wifi_prov_event_register();

#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP
    esp_netif_create_default_wifi_ap();
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP */

    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        /* What is the Provisioning Scheme that we want ?
         * wifi_prov_scheme_softap or wifi_prov_scheme_ble */
#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE
        .scheme = wifi_prov_scheme_ble,
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE */
#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP
        .scheme = wifi_prov_scheme_softap,
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP */

        /* Any default scheme specific event handler that you would
         * like to choose. Since our example application requires
         * neither BT nor BLE, we can choose to release the associated
         * memory once provisioning is complete, or not needed
         * (in case when device is already provisioned). Choosing
         * appropriate scheme specific event handler allows the manager
         * to take care of this automatically. This can be set to
         * WIFI_PROV_EVENT_HANDLER_NONE when using wifi_prov_scheme_softap*/
#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE */
#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP */
    };

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

#ifdef CONFIG_ESP_GATEWAY_RESET_PROVISIONED
    wifi_prov_mgr_reset_provisioning();
#endif

    ESP_LOGI(TAG, "Starting provisioning");

    /* What is the Device Service Name that we want
        * This translates to :
        *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
        *     - device name when scheme is wifi_prov_scheme_ble
        */
    char service_name[12];
    get_device_service_name(service_name, sizeof(service_name));

    /* What is the security level that we want (0 or 1):
        *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
        *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
        *          using X25519 key exchange and proof of possession (pop) and AES-CTR
        *          for encryption/decryption of messages.
        */
    wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

    /* Do we want a proof-of-possession (ignored if Security 0 is selected):
        *      - this should be a string with length > 0
        *      - NULL if not used
        */
    const char *pop = "abcd1234";

    /* What is the service key (could be NULL)
        * This translates to :
        *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
        *          (Minimum expected length: 8, maximum 64 for WPA2-PSK)
        *     - simply ignored when scheme is wifi_prov_scheme_ble
        */
    const char *service_key = NULL;

#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE
    /* This step is only useful when scheme is wifi_prov_scheme_ble. This will
        * set a custom 128 bit UUID which will be included in the BLE advertisement
        * and will correspond to the primary GATT service that provides provisioning
        * endpoints as GATT characteristics. Each GATT characteristic will be
        * formed using the primary service UUID as base, with different auto assigned
        * 12th and 13th bytes (assume counting starts from 0th byte). The client side
        * applications must identify the endpoints by reading the User Characteristic
        * Description descriptor (0x2901) for each characteristic, which contains the
        * endpoint name of the characteristic */
    uint8_t custom_service_uuid[] = {
        /* LSB <---------------------------------------
            * ---------------------------------------> MSB */
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };

    /* If your build fails with linker errors at this point, then you may have
        * forgotten to enable the BT stack or BTDM BLE settings in the SDK (e.g. see
        * the sdkconfig.defaults in the example project) */
    wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE */

    /* An optional endpoint that applications can create if they expect to
        * get some additional custom data during provisioning workflow.
        * The endpoint name can be anything of your choice.
        * This call must be made before starting the provisioning.
        */
    wifi_prov_mgr_endpoint_create("custom-data");
    /* Start provisioning service */
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));

    /* The handler for the optional endpoint created above.
        * This call must be made after starting the provisioning, and only if the endpoint
        * has already been created above.
        */
    wifi_prov_mgr_endpoint_register("custom-data", custom_prov_data_handler, NULL);

    /* Uncomment the following to wait for the provisioning to finish and then release
        * the resources of the manager. Since in this case de-initialization is triggered
        * by the default event loop handler, we don't need to call the following */
    // wifi_prov_mgr_wait();
    // wifi_prov_mgr_deinit();
    /* Print QR code for provisioning */
#ifdef CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE
    wifi_prov_print_qr(service_name, pop, PROV_TRANSPORT_BLE);
#else /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_SOFTAP */
    wifi_prov_print_qr(service_name, pop, PROV_TRANSPORT_SOFTAP);
#endif /* CONFIG_ESP_GATEWAY_PROV_TRANSPORT_BLE */

    const esp_timer_create_args_t deinit_wifi_prov_mgr_timer_args = {
            .callback = &deinit_wifi_prov_mgr_timer_callback,
            .name = "deinit_wifi_prov_mgr"
    };

    ESP_ERROR_CHECK(esp_timer_create(&deinit_wifi_prov_mgr_timer_args, &deinit_wifi_prov_mgr_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(deinit_wifi_prov_mgr_timer, 300 * 1000000));
}
