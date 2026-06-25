# Memento
Disclaimer

## Description of the document

This document sets out the limitations and risks of the Memento device and must be read before the
device is relied upon. Memento is an experimental, hobbyist prototype provided "as is", with no
warranty of any kind (see `LICENSE`). It should not be relied upon as the only safeguard for anything
important.

## Legal status

Memento is not a will, nor a legal document. A device releasing a note to a person has no legal 
standing. A proper will, prepared with a lawyer, should be used for anything that matters legally.

## Possible failure modes

The device does not release notes on its own. A release happens only when a PIN that authorises
those notes is entered on the physically held device, so the realistic failure modes are narrow and,
in normal use, within the owner's control.

### Releasing earlier than intended

The release countdown can only be started by entering, on the device itself, a PIN the owner issued
to a trusted person, and it completes only if the owner never enters the master PIN to check in. Two
situations are worth understanding:

- **The mechanism working as designed.** If a person the owner chose to trust holds the device,
  enters their PIN, and the owner never checks in, that person's notes are released. This is the
  intended behaviour, and it rests on the owner's own choice of whom to trust and with what waiting
  time — not on any device fault.
- **Deliberate tampering.** Accelerating the countdown by moving the clock is a technical attack: it
  requires physical possession and a valid PIN, is bounded by the ESP32's own monotonic clock (which
  cannot be set through the external real-time-clock chip), and is reported to the owner on the next
  master login. It amounts to attacking the device with a known password, which lies outside the
  trust the device is designed to express.

There is no path by which the device discloses a note without a held, authorising PIN.

### Failing to release

A failure to release is only ever caused by events outside the device's control, for none of which
the authors can be held responsible:

- **Physical destruction or loss** — fire, water, damage, or the device being lost or stolen.
- **Flash wear-out** over the very very long term.
- **A forgotten master PIN with no recovery code set**, for which there is deliberately no backdoor.

Several of these have built-in mitigations: an optional **recovery code** restores access after a
forgotten master PIN, an optional **lost-device message** can show owner contact details on a found
device, and the *map, not a vault* practice — storing pointers and keeping important information in
more than one place — means that losing any single device is not the same as losing the information
itself.

## Backups

The device is a single unit with no built-in backup. If it is lost, broken, or its flash storage
degrades, the stored notes are gone. Important information should also be kept elsewhere.

## Security scope

- A device is **provisioned** with flash encryption + secure boot + encrypted NVS before it is relied
  upon (an irreversible eFuse step). On a provisioned device a flash dump yields only ciphertext and 
  the secret key cannot be recovered from the chip.
  **On an un-provisioned device**, the PIN is an access gate rather than protection against extraction: 
  a person with the physical device and laboratory tools can dump the flash and read everything offline 
  (the secret key is in flash, and from it the note store and every note follow without guessing a PIN), 
  so an un-provisioned device must be treated as a *map, not a vault*.
- Within that scope, several attacks that do **not** require a flash dump are mitigated:
  - Guessing the PIN on the device is throttled by an escalating delay that is saved to flash, so it
    survives a reboot or a power cut between attempts; the master PIN must be at least six digits.
  - Fast-forwarding the release countdown by moving the clock is bounded: the countdown is measured
    against the ESP32's own clock, which cannot be set through the external real-time-clock chip, so
    it can advance only at roughly its real rate, and a large forward jump is reported to the owner.
- Bluetooth is disabled, and the setup Wi-Fi access point is active only while setup is in use.
- **Do Wi-Fi setup alone, in a private place.** The setup page has no login of its own beyond the
  one-time Wi-Fi password shown on the device's screen (a 12-character random password, strong enough
  that a captured handshake cannot be cracked within a session). 
  Setting up in private guards against two distinct risks: someone reading the password, PINs or note 
  text over your shoulder, and someone capturing the brief over-the-air connection. Closing the setup 
  page (or letting it time out — the idle timeout can be shortened in the settings) ends that window. 
  The release mechanism itself never discloses a note without a held, authorising PIN being entered on
  the device; the countdown is enforced by the device's firmware while it runs, not by a time-lock in
  the encryption, so the protections above are what stand between an attacker and the data until flash
  encryption + secure boot are provisioned.

## User responsibility

The authors and contributors are not liable for any data loss, wrongful disclosure, or other
harm arising from its use. Anyone who is not comfortable evaluating these trade-offs should not use
the device for sensitive information.
