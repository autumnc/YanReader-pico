#include "pcf85063.h"

#include <esp_log.h>
#include "read_pico_board.h"
#include "read_pico_pmu.h"

static const char *TAG = "Rtc";

i2c_master_bus_handle_t pjournal_get_i2c_bus() {
    return read_pico_i2c_bus();
}

static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

bool PCF85063::begin() {
    _initialized = read_pico_pmu_ready();
    if (_initialized) {
        ESP_LOGI(TAG, "PMU RTC ready");
    } else {
        ESP_LOGW(TAG, "PMU not ready, RTC unavailable");
    }
    return _initialized;
}

bool PCF85063::setTime(time_t unixTime) {
    if (!read_pico_pmu_ready()) return false;
    uint8_t buf[4];
    wr_u32le(buf, (uint32_t)unixTime);
    if (read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, buf, 4) != ESP_OK) return false;
    return read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) == ESP_OK;
}

time_t PCF85063::getTime() {
    if (!read_pico_pmu_ready()) return 0;
    if (read_pico_pmu_refresh() != ESP_OK) return 0;
    const pmu_snapshot_t *s = read_pico_pmu_get();
    if (!s || !s->time_ok || s->unix_sec == 0) return 0;
    return (time_t)s->unix_sec;
}

bool PCF85063::hasValidTime() {
    return getTime() > 0;
}

PCF85063 g_rtc;
