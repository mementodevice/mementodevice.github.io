# Memento
User documentation

## Description of the document and the system

The Memento user documentation contains information about how to use the Memento device. It does
not cover technical details or implementation; instead, it serves as a guide for (new) users
regarding the device's functionality, how it is accessed, how notes and PINs are entered, how the
release-countdown feature works, and the device's limitations.

Memento is a small, battery-powered device with an e-ink screen and two buttons that stores short
text notes locked behind a PIN. In addition to private note storage, the device can grant a trusted
person access to selected notes after a defined waiting period, provided the owner does not check
in during that period. The device operates entirely on its own and requires no internet connection
or paired phone for everyday use.

## Accessing the device and entering the PIN

Memento has only two buttons, referred to on screen as **UPR** (the upper button) and **LWR** (the
lower button). When idle, the screen shows the Memento logo; the image remains visible even while
the device sleeps, which is normal for e-ink and consumes no power.

By pressing either button the device wakes and the PIN pad is displayed. The PIN is entered using
the two buttons:

- **UPR** moves the highlight to the next key.
- A double-click of **UPR** moves the highlight back by one key.
- **LWR** selects the highlighted key (a digit, the 'DEL' key, or the 'OK' key).
- Pressing and holding **UPR** submits the entered PIN (a shortcut for 'OK').

The device is delivered with the default master PIN '123', which the owner should change before use
(see "Managing the device over Wi-Fi"). If an incorrect PIN is entered, the screen displays "Wrong
PIN" and the user may try again. After several consecutive incorrect attempts the device waits a
progressively longer time (up to about half a minute) before the next attempt is allowed. This wait
is remembered even if the device is switched off and on again, so repeated guessing cannot be sped up
by restarting; entering the correct PIN (or the recovery code) clears it at once. The wait is capped,
so it slows guessing without ever locking the owner out permanently.

At any time, a double-click of **LWR** opens an on-screen Help guide that lists the controls.

## Types of PIN

The device distinguishes two types of PIN:

- **Master PIN (the owner)** — grants access to all notes and to the "WiFi Upload & Config"
  management page. Entering the master PIN also acts as a *check-in* that cancels any release
  countdown that is currently running.
- **Alternative PIN (trusted persons)** — grants access only to the notes assigned to that PIN,
  and only after the waiting period described in the following section.

Each PIN is unique. The management page does not allow adding a PIN that is already in use, nor an
alternative PIN that is equal to the master PIN; the master PIN likewise cannot be changed to a
value already used by an alternative PIN.

## The release countdown ("dead-man's switch")

This is the central feature of Memento and works as follows:

- A trusted person enters their **alternative PIN**. This starts a **countdown**, the duration of
  which is defined in advance by the owner (for example, several days). The screen shows the
  remaining time and the device returns to sleep, waking itself periodically to refresh the
  displayed time.
- While the countdown is running, if the **owner enters the master PIN even once**, the countdown
  is cancelled and nothing is released.
- If the owner **never checks in** and the countdown completes, the device shows "Access Ready". The
  trusted person enters their PIN again and may then read the notes assigned to that PIN.

The waiting time is counted only while the device is powered. A loss of power pauses the countdown
(progress is preserved and the countdown never completes early), so the device must remain charged
for a countdown to complete.

The remaining time shown on screen is **approximate** (note the "~"): to save power the device only
wakes to redraw it about once an hour and rounds to whole minutes, so the figure can differ by a
minute or so from an exact count — the countdown itself is tracked precisely in the background. Battery
use during a countdown is modest: a full charge lasts on the order of a month or more of continuous
waiting, so a multi-day countdown uses only a fraction of it. For long countdowns, keep the device on
its charger (or top it up partway through) to be safe; lengthening the refresh interval in the settings
stretches battery life further at the cost of a less frequently updated display.

## Reading notes

How the notes are opened depends on the type of PIN:

- With the **master PIN**, the note list (together with the management options) appears immediately
  after the PIN is entered.
- With an **alternative PIN**, the notes become available only after that PIN's countdown has
  completed: once the device shows "Access Ready", the same alternative PIN is entered again to open
  the list.

In the list, **UPR** moves through the notes and **LWR** opens the highlighted note. Within a note
that spans several pages, **UPR** turns to the next page and **LWR** returns to the list. Pressing
and holding **LWR** puts the device to sleep.

## Managing the device over Wi-Fi

Management is performed from a phone or computer over the device's own temporary Wi-Fi network; no
internet connection is required. By entering the master PIN and selecting **'WiFi Upload & Config'**,
the device starts the access point and displays a network name, a password, and a web address
(`http://192.168.4.1`). The owner connects to the displayed network and opens the address in a web
browser. The network name and password are regenerated each time setup is opened. Pressing and
holding **UPR** closes setup; the access point also turns itself off automatically after a period
of inactivity.

For privacy, do this setup **alone, in a private place**. The screen shows the Wi-Fi password and,
once you are connected, your PINs and the text of your notes; while setup is open, anyone who joins
that temporary network can reach the page. Closing setup (hold **UPR**) or letting it time out ends
that window, and you can shorten the inactivity timeout in the settings if you want it to close
sooner. Treat an open setup session like having the device unlocked in your hand.

The management page is organised into the following sections.

### Changing the master PIN

A new master PIN is entered and confirmed; it must be at least six digits (the more digits, the
harder it is to guess). All existing notes remain readable under the new master PIN. The master PIN
can be changed but never removed, so the device always has one.

### Alternative PINs

A new alternative PIN is added together with an optional name or label (for example "Spouse" or
"Lawyer") and its own waiting time in hours and minutes. An alternative PIN must be at least four
digits. Existing PINs can be renamed or deleted at any time. After each action the page displays a
short confirmation message.

### Notes

A note consists of a unique name and text content (up to 20 KB per note). For each note, the owner
selects which alternative PINs may read it; the master can always read every note. An existing
note can be edited (its text is opened for changes and saved again, keeping the same readers) and
the set of readers can be changed later without re-entering the text. The device can hold a maximum
of 64 notes. Deleting a note requires confirmation.

### Recovery code

By design there is no built-in way to recover a forgotten master PIN. As an optional safety net, the
owner may generate a **recovery code** (off by default). The device displays a ten-digit code once,
which the owner writes down and stores safely. If the master PIN is later forgotten, the recovery
code is entered on the PIN pad and the master PIN is reset to '123'. Anyone who has the recovery code
can reset the master PIN, so it must be kept like a spare key. If the recovery code is forgotten or
lost, a new one can be created by clicking "Regenerate".

### Lost-device message

The owner may optionally enter a short message (up to 160 characters) that is shown before the PIN
pad — for example a name and contact details — so that a finder can return a lost device. The message
reveals no notes and is off by default.

### Settings

The page allows changing the inactivity timeouts (for the PIN pad, the reader, and the Wi-Fi page),
the interval at which the device wakes to refresh a running countdown, and the battery-low warning
threshold. Each field includes a short description on the page.

## Battery and power

A battery percentage is shown in the top-right corner of most screens. When the charge is critically
low, the idle screen displays "Battery low - charge". The device should be kept charged, especially
while a countdown is running, because the countdown does not advance while the device is off. After a
full power loss (the battery being fully drained or disconnected), the device may not wake on its own
when power returns; in that case the **LWR** button is pressed and held to turn the device back on.

## Recommendations

- Change the master PIN from the default '123' immediately after setup.
- Keep the device charged, particularly during an active countdown.
- The device does not use its SD-card slot (it is disabled in the firmware), so it never reads a
  card. For larger or additional files, you can keep a password-protected, encrypted archive on an
  SD card (opened on a computer) and store only its **password** here on Memento.
- Memento is a helper, not a legal will; important information should also be kept elsewhere.

## Quick button reference

| Action                        | What to press        |
|-------------------------------|----------------------|
| Wake the device               | Either button        |
| Move / next                   | UPR — single click   |
| Move back / previous          | UPR — double-click   |
| Select / open / enter a digit | LWR — single click   |
| Submit the PIN / go back      | UPR — press and hold |
| Put the device to sleep       | LWR — press and hold |
| Open Help                     | LWR — double-click   |
