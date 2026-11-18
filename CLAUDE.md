# CLAUDE.md — kiban

## What

Board SDK for the Metalio E-Ink 4: ESP-IDF components, **pure C (no C++)**. Consumed by the product
firmwares bunko (reader) and manabu (learning device) via `idf_component.yml`. Board logic from the
vendor firmware and FreeInk SDK (both MIT) is ported file by file into C with attribution; GPL code is not used.

This repo is **public**. Never commit personal data, local absolute paths, device identity, Wi-Fi
credentials, flash dumps, or names of the user's other projects.

## Language

Docs, code comments and commit messages in English. Conversation with the user in Chinese.

## Rules

- Discuss before implementing: no implementation without the user's explicit go; list any decision made on their behalf.
- ⛔ Charger (CX25601N): only `kiban_charger_apply()` / `kiban_charger_set_enabled()` write it (VREG, ICHG, EN_TERM,
  EN_CHG/HIZ/WDT). **VREG capped at 4200 mV** until the cell's rated charge voltage is confirmed; ICHG 480 mA default,
  max 960 mA; fail closed (user decisions 2026-11-18). No other charger register writes.
- ⛔ Never `espefuse burn*`. esptool without `--baud 921600`. Do not overwrite the bootloader or partition
  table without an explicit decision; flashing the device needs the user's go each time.
- The public API is the contract for both products: changing it requires stating the impact on bunko/manabu and bumping the version.
- No product logic (layout, UI, content, playback policy).
- Third-party repos are read-only references, shallow-cloned into `vendor/` (gitignored).

## Docs

- `docs/research/NNN_name.md`: hardware / board research. `docs/design/NNN_name.md`: kiban's own designs.
- Evidence tags: `[measured]` read from this device · `[source]` third-party code with file:line ·
  `[community]` README/docs/issues · `[inference]` our judgement · `[to-verify]`. Never state inference as fact.
- Describe the current state only, not how it got there (history lives in git log).
