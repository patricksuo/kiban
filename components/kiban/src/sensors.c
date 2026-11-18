// Gauge / charger / RTC / accelerometer read paths.
// Register maps follow the vendor firmware (MIT, CloudZao/Metalio-E-INK4 @d12f481):
// main/boards/common/power/cx25601n.cc, main/boards/common/power/bq27220_gauge.cc,
// main/boards/common/sensor/sc7a20h.cc; BQ27220 standard commands per the TI datasheet;
// PCF8563 per the NXP datasheet.
#include "kiban/sensors.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "kiban/board.h"

static const char *TAG = "kiban.sensors";
#define XFER_TIMEOUT_MS 50

static i2c_master_dev_handle_t dev_for(uint8_t addr)
{
    static struct { uint8_t addr; i2c_master_dev_handle_t h; } cache[4];
    for (int i = 0; i < 4; i++)
        if (cache[i].h && cache[i].addr == addr) return cache[i].h;
    i2c_master_bus_handle_t bus = kiban_board_i2c();
    if (!bus) return NULL;
    for (int i = 0; i < 4; i++) {
        if (cache[i].h) continue;
        const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
                                         .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(bus, &cfg, &cache[i].h) != ESP_OK) return NULL;
        cache[i].addr = addr;
        return cache[i].h;
    }
    return NULL;
}

static esp_err_t rd(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_master_dev_handle_t h = dev_for(addr);
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_STATE, TAG, "no device 0x%02x", addr);
    return i2c_master_transmit_receive(h, &reg, 1, buf, n, XFER_TIMEOUT_MS);
}

static esp_err_t wr(uint8_t addr, uint8_t reg, uint8_t val)
{
    i2c_master_dev_handle_t h = dev_for(addr);
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_STATE, TAG, "no device 0x%02x", addr);
    const uint8_t b[2] = {reg, val};
    return i2c_master_transmit(h, b, 2, XFER_TIMEOUT_MS);
}

// ---- BQ27220 -------------------------------------------------------------------------------
static esp_err_t g16(uint8_t cmd, uint16_t *v)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(rd(KIBAN_GAUGE_ADDR, cmd, b, 2), TAG, "gauge 0x%02x", cmd);
    *v = (uint16_t)(b[0] | (b[1] << 8));
    return ESP_OK;
}

esp_err_t kiban_gauge_read(kiban_gauge_t *o)
{
    uint16_t v;
    ESP_RETURN_ON_ERROR(g16(0x08, &o->voltage_mv), TAG, "voltage");
    ESP_RETURN_ON_ERROR(g16(0x0C, &v), TAG, "current"); o->current_ma = (int16_t)v;
    ESP_RETURN_ON_ERROR(g16(0x14, &v), TAG, "avg current"); o->avg_current_ma = (int16_t)v;
    ESP_RETURN_ON_ERROR(g16(0x06, &v), TAG, "temp"); o->temp_c_x10 = (int16_t)((int)v - 2731);
    ESP_RETURN_ON_ERROR(g16(0x2C, &o->soc_pct), TAG, "soc");
    ESP_RETURN_ON_ERROR(g16(0x10, &o->remaining_mah), TAG, "rm");
    ESP_RETURN_ON_ERROR(g16(0x12, &o->full_mah), TAG, "fcc");
    ESP_RETURN_ON_ERROR(g16(0x3C, &o->design_mah), TAG, "design");
    ESP_RETURN_ON_ERROR(g16(0x2A, &o->cycle_count), TAG, "cycles");
    ESP_RETURN_ON_ERROR(g16(0x2E, &o->soh_pct), TAG, "soh");
    ESP_RETURN_ON_ERROR(g16(0x30, &o->charge_voltage_mv), TAG, "chg v");
    ESP_RETURN_ON_ERROR(g16(0x32, &o->charge_current_ma), TAG, "chg i");
    ESP_RETURN_ON_ERROR(g16(0x0A, &o->battery_status), TAG, "status");
    return ESP_OK;
}

// ---- CX25601N (read-only) ------------------------------------------------------------------
esp_err_t kiban_charger_read(kiban_charger_t *o)
{
    for (int r = 0; r < 0x40; r += 8) ESP_RETURN_ON_ERROR(rd(KIBAN_CHARGER_ADDR, (uint8_t)r, &o->regs[r], 8), TAG, "chg");
    const uint8_t *R = o->regs;
    o->vreg_mv = (uint32_t)(((R[0x04] >> 3) & 0x1F) | ((R[0x05] & 0x0F) << 5)) * 10;     // cx25601n.cc:106-108
    o->ichg_ma = (uint32_t)(((R[0x02] >> 6) & 0x03) | ((R[0x03] & 0x0F) << 2)) * 80;     // cx25601n.cc:645-647
    o->iindpm_ma = (uint32_t)(((R[0x06] >> 4) & 0x0F) | ((R[0x07] & 0x0F) << 4)) * 20;   // cx25601n.cc IINDPM
    o->vbus_stat = R[0x1E] & 0x07;                                                       // cx25601n.cc:741
    o->chrg_stat = (R[0x1E] >> 3) & 0x03;                                                // cx25601n.cc:730
    return ESP_OK;
}

// ---- PCF8563 -------------------------------------------------------------------------------
static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

esp_err_t kiban_rtc_get(kiban_rtc_time_t *o)
{
    uint8_t b[7];
    ESP_RETURN_ON_ERROR(rd(KIBAN_RTC_ADDR, 0x02, b, 7), TAG, "rtc");
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

// ---- SC7A20H -------------------------------------------------------------------------------
esp_err_t kiban_imu_init(uint8_t *who)
{
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(rd(KIBAN_IMU_ADDR, 0x0F, &id, 1), TAG, "who_am_i");
    if (who) *who = id;
    ESP_RETURN_ON_ERROR(wr(KIBAN_IMU_ADDR, 0x20, 0x57), TAG, "ctrl1");   // 100 Hz, XYZ (sc7a20h.cc:18-19)
    ESP_RETURN_ON_ERROR(wr(KIBAN_IMU_ADDR, 0x23, 0x88), TAG, "ctrl4");   // BDU + HR, +/-2 g
    return ESP_OK;
}

esp_err_t kiban_imu_read(kiban_imu_sample_t *o)
{
    uint8_t b[6];
    ESP_RETURN_ON_ERROR(rd(KIBAN_IMU_ADDR, 0x28 | 0x80, b, 6), TAG, "out");
    // 12-bit left-justified, +/-2 g high-resolution: 1 mg/LSB.
    o->x_mg = (int16_t)(b[0] | (b[1] << 8)) >> 4;
    o->y_mg = (int16_t)(b[2] | (b[3] << 8)) >> 4;
    o->z_mg = (int16_t)(b[4] | (b[5] << 8)) >> 4;
    return ESP_OK;
}

esp_err_t kiban_charger_set_vreg_mv(uint32_t mv)
{
    ESP_RETURN_ON_FALSE(mv >= KIBAN_CHARGER_VREG_MIN_MV && mv <= KIBAN_CHARGER_VREG_CAP_MV, ESP_ERR_INVALID_ARG, TAG,
                        "VREG %lu mV outside %d..%d", (unsigned long)mv, KIBAN_CHARGER_VREG_MIN_MV,
                        KIBAN_CHARGER_VREG_CAP_MV);
    const uint32_t code = mv / 10;
    uint8_t r4, r5, r16;
    ESP_RETURN_ON_ERROR(rd(KIBAN_CHARGER_ADDR, 0x04, &r4, 1), TAG, "read 0x04");
    ESP_RETURN_ON_ERROR(rd(KIBAN_CHARGER_ADDR, 0x05, &r5, 1), TAG, "read 0x05");
    ESP_RETURN_ON_ERROR(rd(KIBAN_CHARGER_ADDR, 0x16, &r16, 1), TAG, "read 0x16");
    const uint8_t n4 = (uint8_t)((r4 & 0x07) | ((code & 0x1F) << 3));
    const uint8_t n5 = (uint8_t)((r5 & 0xF0) | ((code >> 5) & 0x0F));
    if (n4 == r4 && n5 == r5) return ESP_OK;

    // [measured] VREG writes are ignored while charging is enabled. Sequence after the crossmux SDK
    // (0x1abin/freeink-sdk @e1008fc MetalioEink4Board.h:81-92): clear EN_CHG (0x16 bit5) and the
    // watchdog bits (0x16[1:0]), write VREG, re-enable charging with HIZ (bit4) clear.
    // If anything fails after charging is disabled, charging stays disabled (safe side).
    ESP_RETURN_ON_ERROR(wr(KIBAN_CHARGER_ADDR, 0x16, (uint8_t)(r16 & ~0x23)), TAG, "disable charging");
    // Vendor order: low part (0x04) then high part (0x05), both always written; a split field may
    // latch on the high-byte write (vendor cx25601n.cc:106-108).
    ESP_RETURN_ON_ERROR(wr(KIBAN_CHARGER_ADDR, 0x04, n4), TAG, "write 0x04 (charging left disabled)");
    ESP_RETURN_ON_ERROR(wr(KIBAN_CHARGER_ADDR, 0x05, n5), TAG, "write 0x05 (charging left disabled)");
    uint8_t v4, v5;
    ESP_RETURN_ON_ERROR(rd(KIBAN_CHARGER_ADDR, 0x04, &v4, 1), TAG, "verify 0x04 (charging left disabled)");
    ESP_RETURN_ON_ERROR(rd(KIBAN_CHARGER_ADDR, 0x05, &v5, 1), TAG, "verify 0x05 (charging left disabled)");
    const uint32_t got = (uint32_t)(((v4 >> 3) & 0x1F) | ((v5 & 0x0F) << 5)) * 10;
    ESP_RETURN_ON_FALSE(got == code * 10, ESP_ERR_INVALID_RESPONSE, TAG,
                        "VREG read back %lu mV; charging left disabled", (unsigned long)got);
    ESP_RETURN_ON_ERROR(wr(KIBAN_CHARGER_ADDR, 0x16, (uint8_t)((r16 & ~0x33) | 0x20)), TAG,
                        "re-enable charging");
    ESP_LOGI(TAG, "charger VREG %lu mV (was 0x04=%02x 0x05=%02x, 0x16=%02x)", (unsigned long)got, r4, r5, r16);
    return ESP_OK;
}

// PCF8563 countdown timer interrupt (datasheet: 0x01 Control_status_2 TIE=bit0 TF=bit2,
// 0x0E Timer_control TE=bit7 TD[1:0]=10 for 1 Hz, 0x0F Timer value). INT is open-drain, active low.
esp_err_t kiban_rtc_timer_irq(uint8_t seconds)
{
    uint8_t c2;
    ESP_RETURN_ON_ERROR(wr(KIBAN_RTC_ADDR, 0x0E, 0x00), TAG, "timer off");
    ESP_RETURN_ON_ERROR(rd(KIBAN_RTC_ADDR, 0x01, &c2, 1), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(wr(KIBAN_RTC_ADDR, 0x01, (uint8_t)((c2 & ~0x1C) | 0x01)), TAG, "TIE");   // clear AF/TF/TI_TP
    ESP_RETURN_ON_ERROR(wr(KIBAN_RTC_ADDR, 0x0F, seconds), TAG, "timer value");
    return wr(KIBAN_RTC_ADDR, 0x0E, 0x82);
}

esp_err_t kiban_rtc_timer_off(void)
{
    uint8_t c2;
    ESP_RETURN_ON_ERROR(wr(KIBAN_RTC_ADDR, 0x0E, 0x00), TAG, "timer off");
    ESP_RETURN_ON_ERROR(rd(KIBAN_RTC_ADDR, 0x01, &c2, 1), TAG, "ctrl2");
    return wr(KIBAN_RTC_ADDR, 0x01, (uint8_t)(c2 & ~0x1D));
}
