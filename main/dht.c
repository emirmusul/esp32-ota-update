#include "dht.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "DHT";

esp_err_t dht_read(gpio_num_t pin, dht_reading_t *out)
{
    uint8_t data[5] = {0};

    // Phase A: send start signal
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(18000);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    // Phase B: wait for the sensor's response signal
    int64_t timeout = 0;

    while (gpio_get_level(pin) == 1) {
        if (++timeout > 1000) {
            ESP_LOGE(TAG, "Timeout waiting for sensor response (line stuck HIGH)");
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    timeout = 0;
    while (gpio_get_level(pin) == 0) {
        if (++timeout > 1000) {
            ESP_LOGE(TAG, "Timeout waiting for sensor response (line stuck LOW)");
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    timeout = 0;
    while (gpio_get_level(pin) == 1) {
        if (++timeout > 1000) {
            ESP_LOGE(TAG, "Timeout waiting for sensor response (line stuck HIGH)");
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(1);
    }

    // Phase C: read 40 data bits
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpio_get_level(pin) == 0) {
            if (++timeout > 1000) {
                ESP_LOGE(TAG, "Timeout during bit %d (waiting for HIGH)", i);
                return ESP_ERR_TIMEOUT;
            }
            esp_rom_delay_us(1);
        }

        esp_rom_delay_us(30);
        int bit = gpio_get_level(pin);

        timeout = 0;
        while (gpio_get_level(pin) == 1) {
            if (++timeout > 1000) {
                ESP_LOGE(TAG, "Timeout during bit %d (waiting for LOW)", i);
                return ESP_ERR_TIMEOUT;
            }
            esp_rom_delay_us(1);
        }

        data[i / 8] <<= 1;
        if (bit) {
            data[i / 8] |= 1;
        }
    }

    // Phase D: checksum verification (DHT22 protocol)
    uint8_t checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        ESP_LOGE(TAG, "Checksum error: computed=%02X, received=%02X", checksum, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    // DHT22: humidity and temperature are 16-bit values, 0.1 unit precision
    uint16_t raw_humidity = ((uint16_t)data[0] << 8) | data[1];
    out->humidity = raw_humidity / 10.0f;

    uint16_t raw_temp = (((uint16_t)data[2] & 0x7F) << 8) | data[3];
    out->temperature = raw_temp / 10.0f;
    if (data[2] & 0x80) {  // most significant bit set means negative temperature
        out->temperature = -out->temperature;
    }

    return ESP_OK;
}