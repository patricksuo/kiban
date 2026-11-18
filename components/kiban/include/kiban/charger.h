// CX25601N single-cell charger at 0x6B. Register layout from the vendor firmware (MIT,
// CloudZao/Metalio-E-INK4 @d12f481 main/boards/common/power/cx25601n.cc); no public datasheet.
//
// Safety model (user decisions 2026-11-18):
//  - VREG capped at 4200 mV until the cell's rated charge voltage is confirmed (vendor used 4400 mV).
//  - Charge current 480 mA (vendor 960 mA); charge termination enabled (vendor left it off).
//  - Fail closed: if the policy cannot be applied and verified, charging is left disabled.
// Writes happen only with charging disabled (VREG writes are ignored otherwise [measured]).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_CHARGER_ADDR        0x6B
#define KIBAN_CHARGER_VREG_CAP_MV 4200
#define KIBAN_CHARGER_VREG_MIN_MV 3840
#define KIBAN_CHARGER_ICHG_MAX_MA 960
#define KIBAN_CHARGER_ICHG_STEP   80

typedef struct {
    uint32_t vreg_mv;   // termination voltage, <= KIBAN_CHARGER_VREG_CAP_MV
    uint32_t ichg_ma;   // fast-charge current, multiple of 80, <= KIBAN_CHARGER_ICHG_MAX_MA
    bool en_term;       // stop charging at termination current
} kiban_charge_policy_t;

#define KIBAN_CHARGE_POLICY_DEFAULT() { .vreg_mv = 4200, .ichg_ma = 480, .en_term = true }

typedef struct {
    uint32_t vreg_mv, ichg_ma, iindpm_ma;
    uint8_t vbus_stat;    // 0 none (D+/D- detection is off: usually 0 even with USB), 1 SDP, 2 CDP, 3 DCP, 7 OTG
    uint8_t chrg_stat;    // 0 not charging / terminated, 1 pre-charge / CC, 2 CV, 3 top-off
    bool en_chg, hiz, en_term;
    uint8_t wdt;          // 0x16[1:0]
    uint8_t reg14, reg16, reg1a, reg1e;
} kiban_charger_status_t;

// Reads only the configuration/status registers (not the fault registers, which may clear on read).
esp_err_t kiban_charger_status(kiban_charger_status_t *out);
// Full 0x00..0x3F dump for diagnostics; may clear latched fault flags.
esp_err_t kiban_charger_dump(uint8_t regs[64]);

// Apply and verify the policy. Ensures charging is enabled afterwards only if every write verified;
// otherwise returns an error with charging disabled. Idempotent: when the registers already match,
// it only makes sure charging is enabled (EN_CHG=1, HIZ=0).
esp_err_t kiban_charger_apply(const kiban_charge_policy_t *policy);
// Turn charging on/off (e.g. temperature gate). Enabling is refused until a policy applied successfully.
esp_err_t kiban_charger_set_enabled(bool on);
bool kiban_charger_policy_ok(void);

#ifdef __cplusplus
}
#endif
