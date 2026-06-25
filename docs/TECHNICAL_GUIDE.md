# Memento
Technical documentation

## Description of the document and the system

This document contains the technical details of the implementation of Memento, an offline encrypted
note vault with a dead-man's switch feature. The code and all required elements are available on
GitHub. The firmware is built with ESP-IDF 5.5 and LVGL v8 for the ESP32-S3, 8 MB flash, 8 MB PSRAM, 
200×200 monochrome e-ink, two buttons, PCF85063 RTC.

## General description of the problem and solution

The objective is a self-contained device that stores short text notes encrypted at rest and releases
selected notes to a trusted person only after a defined waiting period, unless the owner checks in.
The device must operate offline (no network or server dependency), keep each note readable only by
the PINs authorised for it, and preserve countdown progress across power loss.

The solution encrypts every note with AES-256-GCM under a random per-note content key. That content
key is wrapped (encrypted) once per authorising PIN, using a key derived from the PIN and a
device-bound secret. The dead-man countdown is stored as accrued, powered-only time, so a loss of
power pauses it rather than completing it. Management (adding PINs and notes, assigning readers,
editing notes, settings) is performed over a temporary Wi-Fi access point that is started only from
the master menu.

## Architecture and components

The firmware is organised into the following components:

- main – ESP-IDF application entry point and the LVGL display port (flush to the e-ink panel).
- user_app – orchestrator: board bring-up, initialisation of NVS, crypto, vault and configuration,
  and the host hooks used by the user interface (current time, deep sleep, Wi-Fi setup, battery).
- dms_secret – generation and storage of the 32-byte device_secret in NVS.
- dms_crypto – cryptographic primitives: PBKDF2-HMAC-SHA256, AES-256-GCM, SHA-256, Base64
  (implemented with mbedTLS).
- dms_vault – the encrypted manifest and state, PIN verification, per-note content keys, file
  add/read/edit/delete, and the dead-man state.
- dms_config – runtime settings, persisted as plain JSON.
- dms_ui – the LVGL screens (PIN pad, note list, reader, countdown, help) and the flow state machine.
- dms_web – the master-only Wi-Fi setup server.
- Board support components reused from the manufacturer's example: epaper_driver_bsp, button_bsp,
  board_power_bsp, i2c_bsp, i2c_equipment (PCF85063 RTC) and adc_bsp (battery voltage).

## Cryptography and key storage

The encryption scheme is as follows:

- device_secret – a 32-byte random value generated on first boot and stored in NVS. It binds the
  encrypted data to the specific device.
- Per-PIN key – PK = PBKDF2-HMAC-SHA256(PIN ‖ device_secret, salt). Only a salted verifier,
  SHA-256(PK), is stored; the PIN itself is never stored.
- Per-note content key – a random key that encrypts the note (AES-256-GCM). It is wrapped under the
  PK of every authorising PIN, and additionally under a manifest key derived from device_secret so
  that management operations can re-wrap it without requiring the PIN.

Constant-time comparison is used when checking PIN and recovery-code verifiers.

## On-device storage layout

All data is stored on the internal-flash FAT partition (mounted at `/sdcard`, despite the name there
is no memory card). The layout is:

- manifest.enc (with a .bak copy) – AES-GCM([iv | tag | JSON]) under the manifest key, containing the
  PINs (salt, verifier, wrapped PK, countdown) and the notes (content iv/tag/length and the per-PIN
  wrapped content keys).
- files/NNNN.enc – the AES-GCM ciphertext of one note.
- state.enc (with a .bak copy) – the armed state for the dead-man countdown.
- config.json – plain-text runtime settings (not secret).

Writes of the manifest and state are crash-safe (written to a temporary file, flushed, then renamed,
keeping the previous copy as .bak). Editing a note writes a new note file and switches to it
atomically before the old file is removed, so a power loss during an edit never loses the note.

## The release countdown (dead-man state)

Each armed PIN stores `accrued` time, an `anchor` timestamp (from the PCF85063 RTC) and `manc`, an
anchor on the ESP32's own monotonic clock. On every wake the elapsed time since the anchor is folded
into `accrued` and both anchors are reset. If the external RTC has moved backward (for example after
it lost power), the elapsed time is credited as zero, so a power loss pauses the countdown and
preserves progress.

Forward clock changes are bounded by an independent witness. The PCF85063 is an external chip on the
I2C bus that an attacker who opens the device could set forward; the ESP32's own clock
(`gettimeofday`, RTC-timer-backed, and surviving deep sleep) is never set from it — the firmware only
*reads* the PCF85063, it never calls `settimeofday` — so it cannot be advanced over I2C. Each wake
credits at most the real time the witness shows has elapsed (plus a small slack), so an attacker who
fast-forwards the external RTC while the device stays powered gains nothing. Only a full power cycle
resets the witness; after one, a per-wake fallback cap (a built-in constant, **not** taken from the
editable config, so tampering with `config.json` cannot loosen it) limits the credit to one interval.
The countdown can therefore only be advanced at roughly its real rate, one bounded step per full
power cycle. A forward jump far beyond real elapsed time raises a flag shown to the owner on the
master menu (advisory only). Entering the master PIN clears all armed entries (check-in).

## User interface and power management

A single input task reads the two buttons and dispatches actions to the current screen. On a button
wake the device enters the normal flow; on a timer wake it only refreshes the countdown face and
returns to sleep; on a cold boot it shows the logo. If the owner has enabled a lost-device message,
it is shown before the PIN pad and dismissed by any button.

Normal redraws use the fast partial e-ink update; a full (ghost-clearing) refresh is performed only
when closing a note and before deep sleep. Note pagination is measured against the rendered text
height, with the measurement window bounded so that large notes do not stall the interface; note
names are clipped to a single line in the list and the reader header.

Incorrect PINs are throttled by a short, escalating delay (0, 0, 1, 2, 5, 10, 20, 30 seconds, capped
at 30). The count is persisted in NVS together with a "penalty owed" flag, so the delay survives a
reboot and cannot be skipped by cutting power mid-delay: an owed penalty is served at the start of
the next attempt, before the guess is evaluated. A correct PIN or the recovery code clears it, and
the ceiling is low and absolute, so it stays a throttle and never becomes a permanent lockout.

## Wi-Fi setup server

The server (esp_http_server) runs only while setup is open and serves a single page organised into
cards. The form handlers are:

- /addpin – add an alternative PIN (with optional label and countdown).
- /setmaster – change the master PIN (re-wraps the master's content keys for every note).
- /renamepin, /delpin – rename or delete a PIN.
- /addfile – add a note; rejected if larger than 20 KB.
- /editfile, /savefile – open a note for editing and save the new content, keeping its readers.
- /setreaders – change which PINs may read a note.
- /delfile – delete a note.
- /genrecovery, /norecovery – generate (shown once) or disable the recovery code.
- /ownermsg – save the lost-device message and its on/off setting.
- /config – save the runtime settings.

Each action redirects back to the page with a short confirmation or error message. Destructive
actions (deleting a PIN or note, disabling the recovery code) require an on-screen confirmation.

The setup server has **no authentication of its own** — it trusts the Wi-Fi layer, so anyone who
joins the access point gets master-level access (the edit path decrypts notes via the manifest key,
no PIN needed). The only gate is the per-session Wi-Fi password, now generated as a **12-character
random alphanumeric password (~70 bits)** from an unambiguous character set and shown on the device
screen — long enough that a captured WPA2 handshake cannot be brute-forced within a session window
(it was previously 8 numeric digits, ~27 bits, which was crackable in minutes). The AP is up only
during an active setup session, closes on a configurable idle timeout, and uses a fresh random
password each session. Because the server still has no auth of its own, **do setup alone in a private
place** (guards both shoulder-surfing of the on-screen password/PINs/notes and over-the-air capture).
This is the one runtime exposure the eFuse provisioning does not address, so it is handled in firmware
instead.

## Runtime configuration

Settings are loaded once at start-up from config.json and may be changed from the Settings card.
The keys, defaults and allowed ranges are listed in `config_reference.csv`. The runtime settings are
not secret; PINs and per-PIN countdowns are stored only in the encrypted manifest.

## Build and flash

The project is built with ESP-IDF 5.5. Using the command line:

```
idf.py set-target esp32s3
idf.py -p COM4 flash monitor
```

Using Espressif-IDE, the project is imported as an existing IDF project, the target is set to
esp32s3, the serial port is selected, and the project is built and flashed from the toolbar. Detailed
end-user steps are provided in the project README.
