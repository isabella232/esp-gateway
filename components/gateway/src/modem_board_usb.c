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

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_modem_dce.h"
#include "esp_modem_recov_helper.h"
#include "esp_modem_dce_common_commands.h"
#include "esp_gateway_modem.h"
#include "led_indicator.h"

static const char *TAG = "usb_modem_board";

extern led_indicator_handle_t led_system_handle;

typedef struct {
    esp_modem_dce_t parent;
    esp_modem_recov_gpio_t *power_pin;
    esp_modem_recov_gpio_t *reset_pin;
    esp_err_t (*reset)(esp_modem_dce_t *dce);
    esp_err_t (*power_up)(esp_modem_dce_t *dce);
    esp_err_t (*power_down)(esp_modem_dce_t *dce);
    esp_modem_recov_resend_t *re_sync;
    esp_modem_recov_resend_t *re_store_profile;
} modem_board_t;

static esp_err_t modem_board_handle_powerup(esp_modem_dce_t *dce, const char *line)
{
    if (strstr(line, "PB DONE")) {
        ESP_LOGI(TAG, "Board ready after hard reset/power-cycle");
    } else {
        ESP_LOGI(TAG, "xxxxxxxxxxxxxx Board ready after hard reset/power-cycle");
    }
    return ESP_OK;
}

static esp_err_t modem_board_reset(esp_modem_dce_t *dce)
{
    modem_board_t *board = __containerof(dce, modem_board_t, parent);
    ESP_LOGI(TAG, "modem_board_reset!");
    dce->handle_line = modem_board_handle_powerup;
    if(board->reset_pin) board->reset_pin->pulse(board->reset_pin);
    return ESP_OK;
}

esp_err_t esp_modem_board_force_reset(void)
{
    ESP_LOGE(TAG, "Force reset system");
    gpio_config_t io_config = {
            .pin_bit_mask = BIT64(MODEM_RESET_GPIO),
            .mode = GPIO_MODE_OUTPUT
    };
    gpio_config(&io_config);
    gpio_set_level(MODEM_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t modem_board_power_up(esp_modem_dce_t *dce)
{
    modem_board_t *board = __containerof(dce, modem_board_t, parent);
    ESP_LOGI(TAG, "modem_board_power_up!");
    dce->handle_line = modem_board_handle_powerup;
    if(board->power_pin) board->power_pin->pulse(board->power_pin);
    return ESP_OK;
}

static esp_err_t modem_board_power_down(esp_modem_dce_t *dce)
{
    modem_board_t *board = __containerof(dce, modem_board_t, parent);
    ESP_LOGI(TAG, "modem_board_power_down!");
    /* power down sequence (typical values for SIM7600 Toff=min2.5s, Toff-status=26s) */
    dce->handle_line = modem_board_handle_powerup;
    if(board->power_pin) board->power_pin->pulse_special(board->power_pin, 1000, 2600);
    return ESP_OK;
}

static esp_err_t my_recov(esp_modem_recov_resend_t *retry_cmd, esp_err_t err, int timeouts, int errors)
{
    esp_modem_dce_t *dce = retry_cmd->dce;
    ESP_LOGI(TAG, "Current timeouts: %d and errors: %d", timeouts, errors);
    if (err == ESP_ERR_TIMEOUT) {
        if (timeouts < 2) {
            // first timeout, try to exit data mode and sync again
            dce->set_command_mode(dce, NULL, NULL);
            esp_modem_dce_sync(dce, NULL, NULL);
        } else if (timeouts < 3) {
            // try to reset with GPIO if resend didn't help
            ESP_LOGI(TAG, "Restart to connect modem........");
            if(led_system_handle) led_indicator_start(led_system_handle, BLINK_FACTORY_RESET);
            modem_board_t *board = __containerof(dce, modem_board_t, parent);
            board->reset(dce);
        } else {
            // otherwise power-cycle the board
            modem_board_t *board = __containerof(dce, modem_board_t, parent);
            board->power_down(dce);
            esp_modem_dce_sync(dce, NULL, NULL);
        }
    } else {
        // check if a PIN needs to be supplied in case of a failure
        bool ready = false;
        esp_modem_dce_read_pin(dce, NULL, &ready);
        if (!ready) {
            esp_modem_dce_set_pin(dce, "1234", NULL);
        }
        vTaskDelay(1000 / portTICK_RATE_MS);
        esp_modem_dce_read_pin(dce, NULL, &ready);
        if (!ready) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static DEFINE_RETRY_CMD(re_sync_fn, re_sync, modem_board_t)

static DEFINE_RETRY_CMD(re_store_profile_fn, re_store_profile, modem_board_t)

static esp_err_t modem_board_start_up(esp_modem_dce_t *dce)
{
    ESP_MODEM_CHECK(re_sync_fn(dce, NULL, NULL) == ESP_OK, "sending sync failed", err);
    ESP_MODEM_CHECK(dce->set_echo(dce, (void*)false, NULL) == ESP_OK, "set_echo failed", err);
    ESP_MODEM_CHECK(dce->set_flow_ctrl(dce, (void*)ESP_MODEM_FLOW_CONTROL_NONE, NULL) == ESP_OK, "set_flow_ctrl failed", err);
    ESP_MODEM_CHECK(dce->store_profile(dce, NULL, NULL) == ESP_OK, "store_profile failed", err);
    return ESP_OK;
err:
    return ESP_FAIL;

}

static esp_err_t modem_board_deinit(esp_modem_dce_t *dce)
{
    modem_board_t *board = __containerof(dce, modem_board_t, parent);
    if(board->power_pin) board->power_pin->destroy(board->power_pin);
    if(board->reset_pin) board->reset_pin->destroy(board->reset_pin);
    esp_err_t err = esp_modem_command_list_deinit(&board->parent);
    if (err == ESP_OK) {
        free(dce);
    }
    //TODO: Full Clean
    return err;
}

esp_modem_dce_t *usb_modem_board_create(esp_modem_dce_config_t *config)
{
    modem_board_t *board = calloc(1, sizeof(modem_board_t));
    ESP_MODEM_CHECK(board, "failed to allocate board-sim7600 object", err);
    ESP_MODEM_CHECK(esp_modem_dce_init(&board->parent, config) == ESP_OK, "Failed to init sim7600", err);
    // /* power on sequence (typical values for SIM7600 Ton=500ms, Ton-status=16s) */
    if(MODEM_POWER_GPIO) board->power_pin = esp_modem_recov_gpio_new(MODEM_POWER_GPIO, MODEM_POWER_GPIO_INACTIVE_LEVEL,
                                                MODEM_POWER_GPIO_ACTIVE_MS, MODEM_POWER_GPIO_INACTIVE_MS);
    // /* reset sequence (typical values for SIM7600 Treset=200ms, wait 10s after reset */
    if(MODEM_RESET_GPIO) board->reset_pin = esp_modem_recov_gpio_new(MODEM_RESET_GPIO, MODEM_RESET_GPIO_INACTIVE_LEVEL,
                                                MODEM_RESET_GPIO_ACTIVE_MS, MODEM_RESET_GPIO_INACTIVE_MS);
    board->parent.deinit = modem_board_deinit;
    board->reset = modem_board_reset;
    board->power_up = modem_board_power_up;
    board->power_down = modem_board_power_down;
    board->re_sync = esp_modem_recov_resend_new(&board->parent, board->parent.sync, my_recov, 5, 1);
    board->parent.start_up = modem_board_start_up;
    board->re_store_profile = esp_modem_recov_resend_new(&board->parent, board->parent.store_profile, my_recov, 2, 3);
    board->parent.store_profile = re_store_profile_fn;

    return &board->parent;
err:
    return NULL;
}
