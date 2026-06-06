#include "ow.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_rom_sys.h"      // esp_rom_delay_us
#include "esp_cpu.h"

static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

// Pull the line low (output, drive 0).
static inline void ow_low(gpio_num_t pin) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
}
// Release the line (input — external pull-up takes it high).
static inline void ow_release(gpio_num_t pin) {
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}
static inline int ow_read(gpio_num_t pin) {
    return gpio_get_level(pin);
}

void ow_init(gpio_num_t pin) {
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << pin);
    io.mode         = GPIO_MODE_INPUT_OUTPUT_OD;  // open-drain capable
    io.pull_up_en   = GPIO_PULLUP_ENABLE;         // weak internal helps if ext. pullup marginal
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level(pin, 1);
    ow_release(pin);
}

bool ow_reset(gpio_num_t pin) {
    bool presence;
    portENTER_CRITICAL(&s_ow_mux);
    ow_low(pin);
    esp_rom_delay_us(480);
    ow_release(pin);
    esp_rom_delay_us(70);
    presence = (ow_read(pin) == 0);   // device pulls low = present
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);            // finish the 480us recovery window
    return presence;
}

static void ow_write_bit(gpio_num_t pin, int bit) {
    portENTER_CRITICAL(&s_ow_mux);
    if (bit) {
        ow_low(pin);
        esp_rom_delay_us(6);
        ow_release(pin);
        esp_rom_delay_us(64);
    } else {
        ow_low(pin);
        esp_rom_delay_us(60);
        ow_release(pin);
        esp_rom_delay_us(10);
    }
    portEXIT_CRITICAL(&s_ow_mux);
}

static int ow_read_bit(gpio_num_t pin) {
    int bit;
    portENTER_CRITICAL(&s_ow_mux);
    ow_low(pin);
    esp_rom_delay_us(6);
    ow_release(pin);
    esp_rom_delay_us(9);
    bit = ow_read(pin);
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(55);
    return bit;
}

void ow_write_byte(gpio_num_t pin, uint8_t b) {
    for (int i = 0; i < 8; i++) {
        ow_write_bit(pin, b & 0x01);
        b >>= 1;
    }
}

uint8_t ow_read_byte(gpio_num_t pin) {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        b >>= 1;
        if (ow_read_bit(pin)) b |= 0x80;
    }
    return b;
}

uint8_t ow_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t inbyte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ inbyte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            inbyte >>= 1;
        }
    }
    return crc;
}

// ---- DS18B20 ----

bool ds18b20_convert_all(gpio_num_t pin) {
    if (!ow_reset(pin)) return false;
    ow_write_byte(pin, 0xCC);   // SKIP ROM
    ow_write_byte(pin, 0x44);   // CONVERT T
    return true;
}

static float parse_scratchpad(const uint8_t *sp) {
    if (ow_crc8(sp, 8) != sp[8]) return NAN;     // CRC mismatch
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    float t = raw / 16.0f;
    // 85.0 with a fresh power-on (and no valid conversion) is the DS18B20
    // reset value; the control logic already treats >50C chamber as a
    // fault, so we pass it through unchanged.
    return t;
}

// Single read attempt (no retry). NAN on no-presence or CRC mismatch.
static float read_single_once(gpio_num_t pin) {
    if (!ow_reset(pin)) return NAN;
    ow_write_byte(pin, 0xCC);   // SKIP ROM
    ow_write_byte(pin, 0xBE);   // READ SCRATCHPAD
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read_byte(pin);
    return parse_scratchpad(sp);
}

static float read_addr_once(gpio_num_t pin, uint64_t rom) {
    if (!ow_reset(pin)) return NAN;
    ow_write_byte(pin, 0x55);   // MATCH ROM
    for (int i = 0; i < 8; i++) {
        ow_write_byte(pin, (uint8_t)(rom & 0xFF));
        rom >>= 8;
    }
    ow_write_byte(pin, 0xBE);   // READ SCRATCHPAD
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = ow_read_byte(pin);
    return parse_scratchpad(sp);
}

// Public reads retry a few times: a single dropped/garbled OneWire
// transaction (noise, a marginal timing slot) is common and almost always
// succeeds on an immediate retry, so a one-off glitch never surfaces as a
// sensor fault. The data is already in the device's scratchpad from the
// earlier convert, so re-reading is cheap and does not need another 750 ms.
#define DS18B20_READ_TRIES 3

float ds18b20_read_single(gpio_num_t pin) {
    for (int i = 0; i < DS18B20_READ_TRIES; i++) {
        float t = read_single_once(pin);
        if (!isnan(t)) return t;
    }
    return NAN;
}

float ds18b20_read_addr(gpio_num_t pin, uint64_t rom) {
    for (int i = 0; i < DS18B20_READ_TRIES; i++) {
        float t = read_addr_once(pin, rom);
        if (!isnan(t)) return t;
    }
    return NAN;
}
