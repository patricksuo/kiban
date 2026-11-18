// Gauge / RTC / accelerometer read paths.
// Register maps follow the vendor firmware (MIT, CloudZao/Metalio-E-INK4 @d12f481):
// main/boards/common/power/bq27220_gauge.cc, main/boards/common/sensor/sc7a20h.cc; BQ27220 standard
// commands per the TI datasheet; PCF8563 per the NXP datasheet.
#include "kiban/sensors.h"

#include "esp_check.h"
#include "i2c_dev.h"

static const char *TAG = "kiban.sensors";

// ---- BQ27220 -------------------------------------------------------------------------------
static esp_err_t g16(uint8_t cmd, uint16_t *v)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(kiban_i2c_rd(KIBAN_GAUGE_ADDR, cmd, b, 2), TAG, "gauge 0x%02x", cmd);
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return ESP_OK;
}

esp_err_t kiban_gauge_read_basic(kiban_gauge_basic_t *o)
{
    uint16_t v;
    ESP_RETURN_ON_ERROR(g16(0x08, &o->voltage_mv), TAG, "voltage");
    ESP_RETURN_ON_ERROR(g16(0x0C, &v), TAG, "current"); o->current_ma = (int16_t)v;
    ESP_RETURN_ON_ERROR(g16(0x14, &v), TAG, "avg current"); o->avg_current_ma = (int16_t)v;
    ESP_RETURN_ON_ERROR(g16(0x06, &v), TAG, "temp"); o->temp_c_x10 = (int16_t)((int)v - 2731);
    return ESP_OK;
}

esp_err_t kiban_gauge_read(kiban_gauge_t *o)
{
    kiban_gauge_basic_t b;
    ESP_RETURN_ON_ERROR(kiban_gauge_read_basic(&b), TAG, "basic");
    o->voltage_mv = b.voltage_mv;
    o->current_ma = b.current_ma;
    o->avg_current_ma = b.avg_current_ma;
    o->temp_c_x10 = b.temp_c_x10;
    ESP_RETURN_ON_ERROR(g16(0x2C, &o->soc_pct), TAG, "soc");
    ESP_RETURN_ON_ERROR(g16(0x10, &o->remaining_mah), TAG, "rm");
    ESP_RETURN_ON_ERROR(g16(0x12, &o->full_mah), TAG, "fcc");
    ESP_RETURN_ON_ERROR(g16(0x3C, &o->design_mah), TAG, "design");
    ESP_RETURN_ON_ERROR(g16(0x2A, &o->cycle_count), TAG, "cycles");
    ESP_RETURN_ON_ERROR(g16(0x2E, &o->soh_pct), TAG, "soh");
    ESP_RETURN_ON_ERROR(g16(0x30, &o->charge_voltage_mv), TAG, "chg v");
    ESP_RETURN_ON_ERROR(g16(0x32, &o->charge_current_ma), TAG, "chg i");
    ESP_RETURN_ON_ERROR(g16(0x0A, &o->battery_status), TAG, "status");
    ESP_RETURN_ON_ERROR(g16(0x3A, &o->operation_status), TAG, "op status");
    return ESP_OK;
}

// ---- PCF8563 -------------------------------------------------------------------------------
static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

esp_err_t kiban_rtc_get(kiban_rtc_time_t *o)
{
    uint8_t b[7];
    ESP_RETURN_ON_ERROR(kiban_i2c_rd(KIBAN_RTC_ADDR, 0x02, b, 7), TAG, "rtc");
    o->integrity_lost = b[0] & 0x80;
    o->second = bcd(b[0] & 0x7F);
    o->minute = bcd(b[1] & 0x7F);
    o->hour = bcd(b[2] & 0x3F);
    o->day = bcd(b[3] & 0x3F);
    o->weekday = b[4] & 0x07;
    o->month = bcd(b[5] & 0x1F);
    o->year = 2000 + bcd(b[6]) + ((b[5] & 0x80) ? 100 : 0);
    return ESP_OK;
}

// PCF8563: 0x01 Control_status_2 TIE=bit0 TF=bit2, 0x0E Timer_control TE=bit7 TD[1:0]=10 (1 Hz),
// 0x0F Timer value. INT is open-drain, active low.
esp_err_t kiban_rtc_timer_irq(uint8_t seconds)
{
    uint8_t c2;
    ESP_RETURN_ON_ERROR(kiban_i2c_wr(KIBAN_RTC_ADDR, 0x0E, 0x00), TAG, "timer off");
    ESP_RETURN_ON_ERROR(kiban_i2c_rd(KIBAN_RTC_ADDR, 0x01, &c2, 1), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(kiban_i2c_wr(KIBAN_RTC_ADDR, 0x01, (uint8_t)((c2 & ~0x1C) | 0x01)), TAG, "TIE");
    ESP_RETURN_ON_ERROR(kiban_i2c_wr(KIBAN_RTC_ADDR, 0x0F, seconds), TAG, "timer value");
    return kiban_i2c_wr(KIBAN_RTC_ADDR, 0x0E, 0x82);
}

esp_err_t kiban_rtc_timer_off(void)
{
    uint8_t c2;
    ESP_RETURN_ON_ERROR(kiban_i2c_wr(KIBAN_RTC_ADDR, 0x0E, 0x00), TAG, "timer off");
    ESP_RETURN_ON_ERROR(kiban_i2c_rd(KIBAN_RTC_ADDR, 0x01, &c2, 1), TAG, "ctrl2");
    return kiban_i2c_wr(KIBAN_RTC_ADDR, 0x01, (uint8_t)(c2 & ~0x1D));
}

// ---- SC7A20H -------------------------------------------------------------------------------
esp_err_t kiban_imu_init(uint8_t *who)
{
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(kiban_i2c_rd(KIBAN_IMU_ADDR, 0x0F, &id, 1), TAG, "who_am_i");
    if (who) *who = id;
    ESP_RETURN_ON_ERROR(kiban_i2c_wr(KIBAN_IMU_ADDR, 0x20, 0x57), TAG, "ctrl1");   // 100 Hz, XYZ (sc7a20h.cc:18-19)
    ESP_RETURN_ON_ERROR(kiban_i2c_wr(KIBAN_IMU_ADDR, 0x23, 0x88), TAG, "ctrl4");   // BDU + HR, +/-2 g
    return ESP_OK;
}

esp_err_t kiban_imu_read(kiban_imu_sample_t *o)
{
    uint8_t b[6];
    ESP_RETURN_ON_ERROR(kiban_i2c_rd(KIBAN_IMU_ADDR, 0x28 | 0x80, b, 6), TAG, "out");
    o->x_mg = (int16_t)(b[0] | (b[1] << 8)) >> 4;   // 12-bit left-justified, 1 mg/LSB at +/-2 g HR
    o->y_mg = (int16_t)(b[2] | (b[3] << 8)) >> 4;
    o->z_mg = (int16_t)(b[4] | (b[5] << 8)) >> 4;
    return ESP_OK;
}
