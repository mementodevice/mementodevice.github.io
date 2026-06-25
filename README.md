# Memento
Project overview - check out https://mementodevice.github.io/ 

## Description of the document and the system

This document is the entry point for the Memento project. It describes what the device is, the
hardware it requires and where to find the remaining documentation.

Memento is open-source firmware for the ESP32-S3 board (200×200 e-ink screen). It stores short text 
notes encrypted behind a PIN and allows the owner to configure a "dead-man's switch": a trusted 
person is granted access to selected notes only after a defined waiting period, unless the owner 
checks in (enters the master PIN) first. The device operates fully offline (no cloud service and 
no account). It is built with ESP-IDF 5.5 and LVGL v8.

> This is an experimental hobby project. It is not a legal will, carries no warranty, is a single 
> device with no backup.

A printable, non-technical guide for end users is provided in `docs/Memento-User-Guide.pdf`. The full
documentation set is: `docs/USER_GUIDE.md` (owners), `docs/TECHNICAL_GUIDE.md` (internals),
`docs/config_reference.csv` (settings), and `docs/TEST_PLAN.md` (test plan).

## Summary of functionality

- The **master PIN** (the owner) reads every note, manages the device over a Wi-Fi setup page, and
  acts as a check-in that cancels any running release countdown.
- An **alternative PIN** (a trusted person) starts that PIN's countdown when entered; if the owner
  never checks in before it elapses, that person may then read the notes assigned to them.
- The countdown advances only while the device is powered; a loss of power pauses it (it never
  resets or completes early).

## Hardware requirements

The firmware requires the **ESP32-S3** board (8 MB flash, 8 MB PSRAM, 200×200 monochrome e-ink, 
two buttons, PCF85063 RTC, LiPo battery) and a USB-C cable. Other boards are not supported without 
modification, as the user interface is built for this exact 200×200 panel.

## First run

The device boots to the PIN pad with the default master PIN **123**. The owner enters it, selects
**'WiFi Upload & Config'**, joins the Wi-Fi network shown on screen, opens `http://192.168.4.1`,
changes the master PIN, and then adds alternative PINs and notes. The user guide describes the full
walkthrough, including the recovery code, the lost-device message, and recommended practice.

## Security overview

- Each note is encrypted with AES-256-GCM. The encryption key is derived as
  `PBKDF2-HMAC-SHA256(PIN ‖ device_secret, salt)` and wraps a random per-note content key. Only
  salted verifiers are stored; PINs are never stored.
- The wrong-PIN delay is persisted so it survives a reboot or a power cut between guesses; the master 
  PIN must be at least six digits; Bluetooth is disabled; an optional recovery code; and unique-PIN 
  safeguards.
- A device is **provisioned** for production with flash encryption + Secure Boot V2 + encrypted NVS +
  secure download mode (an irreversible eFuse step — see `PROVISIONING.md`). Once provisioned, a flash
  dump yields only ciphertext and `device_secret` can't be recovered from the chip; only firmware
  signed with the owner's key boots, and signed bug-fixes can still be flashed over USB. **Until a
  device is provisioned**, treat it as a *map, not a vault* (the PIN is an access gate, not
  extraction-proof).
- The master-only Wi-Fi setup page is protected by a one-time **12-character random password** shown
  on screen, but it has no login of its own and shows your PINs/note text, so do setup **alone, in a
  private place**; it closes when you finish or time out. The release countdown is enforced by the
  firmware while it runs, not by a cryptographic time-lock.

## Repository layout

```
.
├─ README.md              Project overview and installation guide
├─ DISCLAIMER.md          Important notice; read before relying on the device
├─ LICENSE                MIT (this project's own code)
├─ THIRD_PARTY_NOTICES.md Licences and credits for bundled code (LVGL, SensorLib, Waveshare BSPs)
├─ CMakeLists.txt         ESP-IDF project
├─ sdkconfig.defaults     Build configuration (target, flash, PSRAM, Bluetooth off, LVGL)
├─ partitions.csv         Flash partition layout
├─ dependencies.lock      Pins the LVGL version
├─ main/                  Application entry point and the LVGL port
├─ components/            dms_* (this project) plus the board BSPs and SensorLib
└─ docs/                  User and technical guides, configuration reference
```

## Licence and credits

This project's own code is licensed under the MIT Licence (see `LICENSE`). Bundled third-party
components retain their own licences; see `THIRD_PARTY_NOTICES.md`.

