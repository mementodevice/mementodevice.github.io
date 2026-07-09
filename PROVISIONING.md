# Memento
Production provisioning guide (flash encryption + secure boot)
DO AT YOUR OWN RISK!

## Description of the document

This document describes how to harden a Memento device so that its secrets cannot be read out of a
flash dump: it enables **Flash Encryption (Release mode)**, **Secure Boot V2**, **NVS encryption**
(so `device_secret` is encrypted at rest), **Secure ROM download mode** (so signed firmware fixes can
still be flashed over USB, while flash read-out is blocked), and **disables JTAG**. It is the
production/owner provisioning step referred to throughout the other documents; until a device has been
through it, treat that device as a *map, not a vault*.

> ⚠️ **READ THIS FIRST — these steps are IRREVERSIBLE.**
> - eFuses are one-time-programmable. You **cannot** undo flash encryption, secure boot, or the JTAG
>   fuse once burned.
> - **Do a full dry run on a sacrificial test board first** (a spare ESP32-S3-ePaper-1.54), end to
>   end, before you touch the device you actually care about.
> - **Provision BEFORE entering real PINs/notes.** The process encrypts/erases NVS, so the cleanest
>   path is: provision a *fresh* device, then set up PINs and notes (the new `device_secret` is then
>   created directly inside encrypted NVS). If a device already holds real notes, **record that
>   information elsewhere first** (you should anyway — *map, not a vault*), provision, then re-enter.
>   Do not attempt to preserve a live vault through the burn.
> - **Back up the Secure Boot signing key** in at least two safe places. If you lose it you can
>   **never** sign a firmware update for that device again.
> - Command and Kconfig names below are correct for **ESP-IDF 5.5** to the best of this guide's
>   knowledge, but because the operation is irreversible you must **cross-check them against the
>   official Espressif guides for your exact IDF version before running them**:
>   *Flash Encryption* and *Secure Boot V2* under <https://docs.espressif.com/projects/esp-idf/> →
>   Security. This guide was prepared without on-hardware verification.

## What you end up with

| Protection | Fuse / setting | Effect |
|---|---|---|
| Flash Encryption (Release) | `CONFIG_SECURE_FLASH_ENC_ENABLED` + `..._MODE_RELEASE` | Flash contents (incl. NVS keys) unreadable off-chip |
| Secure Boot V2 | `CONFIG_SECURE_BOOT` + `CONFIG_SECURE_BOOT_V2_ENABLED` | Only firmware signed with **your** key boots |
| NVS encryption | `CONFIG_NVS_ENCRYPTION` + `nvs_key` partition | `device_secret` encrypted at rest |
| Secure ROM download | "UART ROM download mode → **Secure**" (`ENABLE_SECURITY_DOWNLOAD`) | Signed+encrypted reflash over USB still works; flash read-out blocked |
| JTAG disabled | `DIS_PAD_JTAG` / `DIS_USB_JTAG` eFuse | No debug read-out of RAM/keys |

Net: a flash dump yields only ciphertext, unsigned firmware will not boot, and `device_secret` (hence
the whole vault) is no longer recoverable from the chip — while you keep the ability to ship signed
bug-fixes over USB.

## Prerequisites

- ESP-IDF 5.5 environment (the one you already build with).
- The board on USB-C; hold **BOOT** + replug if the port doesn't appear (deep sleep drops it).
- `espsecure.py` and `espefuse.py` (ship with ESP-IDF).

## Step 1 — Generate and back up the Secure Boot signing key

Store `secure_boot_signing_key.pem` **secret and backed up** (offline, two locations). This single
file is what lets you sign future updates; losing it permanently ends updates for every device signed
with it. **Do not commit it** (the project `.gitignore` should exclude `*.pem`; verify).

## Step 2 — Add the NVS key partition

Add one line to `partitions.csv` (a 4 KB `nvs_keys` partition, marked encrypted). Example — adjust
offsets to your current table; the `storage` partition shifts, which is fine on a fresh provision but
**reflashing this new table reformats the data partitions**, so only do it as part of provisioning:

```
# Name      Type  SubType   Offset    Size     Flags
nvs_key     data  nvs_keys  ,         0x1000,  encrypted
```

## Step 3 — Switch NVS init to the secure variant

In `components/user_app/user_app.cpp`, guard `init_nvs()` so the encrypted path is used only in the
provisioning build (the normal build is unchanged):

```c
static void init_nvs(void)
{
#if defined(CONFIG_NVS_ENCRYPTION)
    const esp_partition_t *key_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    nvs_sec_cfg_t cfg;
    if (key_part && nvs_flash_read_security_cfg(key_part, &cfg) != ESP_OK) {
        // first boot: generate the keys into the (flash-encryption-protected) key partition
        ESP_ERROR_CHECK(nvs_flash_generate_keys(key_part, &cfg));
    }
    esp_err_t err = (key_part) ? nvs_flash_secure_init(&cfg) : ESP_ERR_NOT_FOUND;
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_secure_init(&cfg);
    }
    ESP_ERROR_CHECK(err);
#else
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
#endif
}
```

Add the includes it needs (`esp_partition.h`, `nvs_flash.h` is already present). **The NVS security
API changed across IDF versions** (`nvs_flash_read_security_cfg` / `nvs_sec_provider`); confirm the
exact calls for your IDF version. On ESP32-S3 you may alternatively use the HMAC-eFuse based
`nvs_sec_provider` scheme (no `nvs_key` partition); the flash-encryption-based scheme above is the
simpler default.

## Step 4 — Provisioning sdkconfig

In `idf.py menuconfig → Security features` (names as of IDF 5.5 — confirm against menuconfig):

- **Enable flash encryption on boot** → on (`CONFIG_SECURE_FLASH_ENC_ENABLED`)
- **Flash encryption mode** → **Release** (`CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`)
- **Enable hardware Secure Boot in bootloader** → on (`CONFIG_SECURE_BOOT`, **V2**)
- **Secure bootloader signing key** → `secure_boot_signing_key.pem`
- **UART ROM download mode** → **Enable Secure download mode** (`ENABLE_SECURITY_DOWNLOAD` — *not*
  "Disabled", so signed+encrypted USB reflashing of fixes still works)
- **NVS Encryption** → on (`CONFIG_NVS_ENCRYPTION`)

Keep these in a separate provisioning config (e.g. a `sdkconfig.prod` you select deliberately) — do
**not** commit them into the everyday `sdkconfig.defaults`, or a routine `idf.py flash` would trigger
the irreversible burn by accident.

## Step 5 — Build, then the first (irreversible) flash

```
idf.py set-target esp32s3
idf.py build                 # produces the signed bootloader + app
idf.py flash monitor         # FIRST FLASH: on first boot the device burns the flash-encryption key
                             # + secure-boot digest, enables them, and encrypts flash. POINT OF NO RETURN.
```

Watch the monitor: it logs the flash-encryption and secure-boot enablement on first boot.

## Step 6 — Lock down JTAG (and confirm secure download)

After the device is up on the encrypted, secure-boot image:

```
espefuse.py summary                       # review current fuses first
espefuse.py burn_efuse DIS_PAD_JTAG        # disable pad JTAG
espefuse.py burn_efuse DIS_USB_JTAG        # disable the USB-Serial/JTAG's JTAG (ESP32-S3)
```

Secure download mode is set by the sdkconfig in Step 4; verify it shows in `espefuse.py summary`
(`ENABLE_SECURITY_DOWNLOAD`/secure-download bit). **Do not** burn `DIS_DOWNLOAD_MODE` — that would
remove the ability to flash signed fixes over USB.

## Step 7 — Verify before trusting it

On the provisioned unit, confirm **all** of:

1. It boots (secure boot validated the image — no boot error in the monitor).
2. The **master PIN** logs in and the menu appears.
3. The **recovery code** still resets the master (test on the bench).
4. You can add PINs/notes, and they read back after a power cycle (vault works under encrypted NVS).
5. `espefuse.py summary` shows flash-encryption enabled, secure-boot enabled, secure-download set,
   JTAG disabled.

Only after this passes on the test unit should you provision a real device — and on a real device,
do setup (PINs/notes) **after** provisioning.

## Updating firmware later (you kept this ability)

A bug fix is still flashable, because Step 4 chose **secure download mode**, not full disable:

```
idf.py build                 # signed automatically with your key
idf.py encrypted-flash        # writes the signed, encrypted image over the secure-download channel
```

The `nvs` and `storage` partitions are untouched by an app update, so **the user's notes survive**.
Requirements: you still have the signing key (Step 1), and you flash *encrypted*. You cannot flash a
plaintext or unsigned image — that is the point.

## If something goes wrong

- **Boot fails after the burn:** the image was not signed/encrypted as expected. Re-`idf.py
  encrypted-flash` a correctly built image. If secure boot rejects everything and you have the key,
  rebuild and re-flash; if you lost the key, the device cannot be recovered (replace it).
- **NVS won't initialise:** the key partition or `CONFIG_NVS_ENCRYPTION` is mismatched; do not erase a
  populated encrypted NVS without understanding it will discard `device_secret` (and the vault).
- **Build error "Bootloader binary size 0x… too large for partition table offset 0x8000":** the Secure
  Boot bootloader is larger than the default leaves room for. Set `CONFIG_PARTITION_TABLE_OFFSET =
  0x10000` (Step 4); `partitions.csv` already starts the first partition at `0x11000` to match.
- **`erase_flash` fails: "ESP32-S3 ROM does not support function erase_flash":** secure download mode
  runs esptool with `--no-stub`, and a full-chip erase is a *stub* feature, so it's unavailable. You do
  **not** need a full erase for the real provisioning — `idf.py flash` overwrites the bootloader, table
  and app. To get a clean erase (e.g. for the dry run below), temporarily turn **Secure Boot + Flash
  Encryption OFF** in menuconfig (that re-enables the stub), then `idf.py erase-flash` works.
- **Boot loops with "No bootable app… invalid magic byte at 0x10000":** the chip has a stale/incomplete
  flash (old bootloader + old partition table, no app) — not a burn problem (no flash-enc/secure-boot
  lines appear). Flash a complete image. Safest recovery: a clean flash in a **normal (non-secure)
  config** first (proves the firmware + new partition layout boot), *then* provision.
- **Recommended dry run (no burn):** before the irreversible flash, build/flash once with Secure Boot,
  Flash Encryption and NVS Encryption all **OFF**, confirm the device boots to the PIN pad and the
  vault works, then re-enable all three (Flash Encryption = **Release**) and do the real flash. This
  separates "is my firmware good?" from the one-way burn.
- **You are unsure at any irreversible step:** stop and re-check the official Espressif Security docs
  for your IDF version before burning.
</content>
