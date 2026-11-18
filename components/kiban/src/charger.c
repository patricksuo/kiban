#include "kiban/charger.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i2c_dev.h"

static const char *TAG = "kiban.charger";
#define A KIBAN_CHARGER_ADDR

// 0x16: bit5 EN_CHG, bit4 EN_HIZ, bits1:0 WDT (cx25601n.cc:37,356-357,387). 0x14 bit2 EN_TERM (:145-146).
#define R16_EN_CHG 0x20
#define R16_HIZ    0x10
#define R16_WDT    0x03
#define R14_EN_TERM 0x04

static SemaphoreHandle_t s_lock;
static bool s_policy_ok;

static void lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();   // first use happens in board init (single task)
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

static esp_err_t rd1(uint8_t reg, uint8_t *v) { return kiban_i2c_rd(A, reg, v, 1); }
static esp_err_t wr1(uint8_t reg, uint8_t v) { return kiban_i2c_wr(A, reg, v); }

static uint32_t dec_vreg(uint8_t r4, uint8_t r5) { return (uint32_t)(((r4 >> 3) & 0x1F) | ((r5 & 0x0F) << 5)) * 10; }
static uint32_t dec_ichg(uint8_t r2, uint8_t r3) { return (uint32_t)(((r2 >> 6) & 0x03) | ((r3 & 0x0F) << 2)) * 80; }

esp_err_t kiban_charger_status(kiban_charger_status_t *o)
{
    uint8_t r[6];   // 0x02..0x07
    lock();
    esp_err_t err = kiban_i2c_rd(A, 0x02, r, sizeof r);
    if (err == ESP_OK) err = rd1(0x14, &o->reg14);
    if (err == ESP_OK) err = rd1(0x16, &o->reg16);
    if (err == ESP_OK) err = rd1(0x1A, &o->reg1a);
    if (err == ESP_OK) err = rd1(0x1E, &o->reg1e);
    unlock();
    ESP_RETURN_ON_ERROR(err, TAG, "status read");
    o->ichg_ma = dec_ichg(r[0], r[1]);
    o->vreg_mv = dec_vreg(r[2], r[3]);
    o->iindpm_ma = (uint32_t)(((r[4] >> 4) & 0x0F) | ((r[5] & 0x0F) << 4)) * 20;
    o->vbus_stat = o->reg1e & 0x07;
    o->chrg_stat = (o->reg1e >> 3) & 0x03;
    o->en_chg = o->reg16 & R16_EN_CHG;
    o->hiz = o->reg16 & R16_HIZ;
    o->wdt = o->reg16 & R16_WDT;
    o->en_term = o->reg14 & R14_EN_TERM;
    return ESP_OK;
}

esp_err_t kiban_charger_dump(uint8_t regs[64])
{
    lock();
    esp_err_t err = ESP_OK;
    for (int r = 0; r < 0x40 && err == ESP_OK; r += 8) err = kiban_i2c_rd(A, (uint8_t)r, &regs[r], 8);
    unlock();
    return err;
}

// Charging on with HIZ off and the watchdog left disabled; verified by read-back.
static esp_err_t enable_locked(uint8_t r16)
{
    const uint8_t want = (uint8_t)((r16 & ~(R16_EN_CHG | R16_HIZ | R16_WDT)) | R16_EN_CHG);
    ESP_RETURN_ON_ERROR(wr1(0x16, want), TAG, "enable");
    uint8_t v;
    ESP_RETURN_ON_ERROR(rd1(0x16, &v), TAG, "verify enable");
    ESP_RETURN_ON_FALSE((v & (R16_EN_CHG | R16_HIZ)) == R16_EN_CHG, ESP_ERR_INVALID_RESPONSE, TAG, "0x16 = %02x", v);
    return ESP_OK;
}

static esp_err_t disable_locked(uint8_t r16)
{
    ESP_RETURN_ON_ERROR(wr1(0x16, (uint8_t)(r16 & ~(R16_EN_CHG | R16_WDT))), TAG, "disable");
    uint8_t v;
    ESP_RETURN_ON_ERROR(rd1(0x16, &v), TAG, "verify disable");
    ESP_RETURN_ON_FALSE(!(v & R16_EN_CHG), ESP_ERR_INVALID_RESPONSE, TAG, "EN_CHG still set (0x16 = %02x)", v);
    return ESP_OK;
}

esp_err_t kiban_charger_apply(const kiban_charge_policy_t *p)
{
    ESP_RETURN_ON_FALSE(p && p->vreg_mv >= KIBAN_CHARGER_VREG_MIN_MV && p->vreg_mv <= KIBAN_CHARGER_VREG_CAP_MV &&
                            p->ichg_ma >= KIBAN_CHARGER_ICHG_STEP && p->ichg_ma <= KIBAN_CHARGER_ICHG_MAX_MA,
                        ESP_ERR_INVALID_ARG, TAG, "policy out of range");
    const uint32_t vcode = p->vreg_mv / 10, icode = p->ichg_ma / KIBAN_CHARGER_ICHG_STEP;
    uint8_t r[6], r14, r16;
    lock();
    s_policy_ok = false;
    esp_err_t err = kiban_i2c_rd(A, 0x02, r, sizeof r);
    if (err == ESP_OK) err = rd1(0x14, &r14);
    if (err == ESP_OK) err = rd1(0x16, &r16);
    if (err != ESP_OK) {
        // Fail closed: try to at least turn charging off.
        uint8_t v;
        if (rd1(0x16, &v) == ESP_OK) disable_locked(v);
        unlock();
        ESP_LOGE(TAG, "policy read failed (%s); charging disabled if possible", esp_err_to_name(err));
        return err;
    }
    const uint8_t n2 = (uint8_t)((r[0] & 0x3F) | ((icode & 0x03) << 6));
    const uint8_t n3 = (uint8_t)((r[1] & 0xF0) | ((icode >> 2) & 0x0F));
    const uint8_t n4 = (uint8_t)((r[2] & 0x07) | ((vcode & 0x1F) << 3));
    const uint8_t n5 = (uint8_t)((r[3] & 0xF0) | ((vcode >> 5) & 0x0F));
    const uint8_t n14 = p->en_term ? (uint8_t)(r14 | R14_EN_TERM) : (uint8_t)(r14 & ~R14_EN_TERM);
    const bool match = n2 == r[0] && n3 == r[1] && n4 == r[2] && n5 == r[3] && n14 == r14;

    if (!match) {
        err = disable_locked(r16);
        // Low part then high part, both always written (vendor order, cx25601n.cc:106-108, 645-647).
        if (err == ESP_OK) err = wr1(0x02, n2);
        if (err == ESP_OK) err = wr1(0x03, n3);
        if (err == ESP_OK) err = wr1(0x04, n4);
        if (err == ESP_OK) err = wr1(0x05, n5);
        if (err == ESP_OK) err = wr1(0x14, n14);
        uint8_t v[6] = {0}, v14 = 0;
        if (err == ESP_OK) err = kiban_i2c_rd(A, 0x02, v, sizeof v);
        if (err == ESP_OK) err = rd1(0x14, &v14);
        if (err == ESP_OK && (v[0] != n2 || v[1] != n3 || v[2] != n4 || v[3] != n5 || v14 != n14)) {
            ESP_LOGE(TAG, "verify failed: VREG %lu ICHG %lu 0x14=%02x", (unsigned long)dec_vreg(v[2], v[3]),
                     (unsigned long)dec_ichg(v[0], v[1]), v14);
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err != ESP_OK) {
            unlock();
            ESP_LOGE(TAG, "policy not applied (%s); charging left disabled", esp_err_to_name(err));
            return err;
        }
        if ((err = rd1(0x16, &r16)) != ESP_OK) { unlock(); return err; }
    }
    // Idempotent path too: make sure charging is on (a previous run may have died while it was off).
    err = enable_locked(r16);
    s_policy_ok = err == ESP_OK;
    unlock();
    if (err == ESP_OK)
        ESP_LOGI(TAG, "policy: VREG %lu mV, ICHG %lu mA, EN_TERM %d%s", (unsigned long)p->vreg_mv,
                 (unsigned long)(icode * KIBAN_CHARGER_ICHG_STEP), p->en_term, match ? " (already set)" : "");
    return err;
}

esp_err_t kiban_charger_set_enabled(bool on)
{
    ESP_RETURN_ON_FALSE(!on || s_policy_ok, ESP_ERR_INVALID_STATE, TAG, "policy not applied; refusing to enable");
    uint8_t r16;
    lock();
    esp_err_t err = rd1(0x16, &r16);
    if (err == ESP_OK) err = on ? enable_locked(r16) : disable_locked(r16);
    unlock();
    return err;
}

bool kiban_charger_policy_ok(void) { return s_policy_ok; }
