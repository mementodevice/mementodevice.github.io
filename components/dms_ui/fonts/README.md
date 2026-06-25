# Extended UI font for Memento

The screen's default font (`lv_font_montserrat_14`) only contains **basic Latin (ASCII)**.
Accented / non‑ASCII characters — Croatian `č ć ž š đ`, curly quotes, em‑dashes, `€`, and any
unusual characters in a **stored password** — render as a missing‑glyph box `▯`.

This folder is where a **generated extended font** goes. Any `.c` here is compiled automatically
(see `CMakeLists.txt`). After adding the file, flip `MEMENTO_FONT_EXTENDED` to `1` in
[`dms_ui.c`](../dms_ui.c) and rebuild — that's the only code change.

> The glyph bitmaps must be produced by LVGL's font converter (fonts are generated assets, not
> hand‑written C). It's a 5‑minute, no‑coding step. Pick **either** method below.

## What to generate

- **Output name (symbol):** `font_memento_14`  → save as `font_memento_14.c` in this folder.
- **Size:** `14` px (matches the current layout metrics — other sizes may shift the hint rows).
- **Bits per pixel:** `1` (the e‑ink panel is 1‑bit black/white; 1 bpp is crisp and small).
- **Source TTF:** **DejaVuSans.ttf** (guaranteed full coverage) — or `Montserrat-Medium.ttf`
  to match the current look (Montserrat also covers Latin Extended‑A).
- **Unicode ranges** (covers ASCII + all common European accents + typographic & currency
  symbols — enough for Croatian text and special characters in passwords):

  ```
  0x20-0x7F,0xA0-0x17F,0x2013-0x2014,0x2018-0x201D,0x20AC,0x2022,0x2026,0x2122
  ```

  | Range | What it adds |
  |---|---|
  | `0x20-0x7F` | ASCII: letters, digits, and password punctuation `! " # $ % & ' ( ) * + , - . / : ; < = > ? @ [ \ ] ^ _ \` { \| } ~` |
  | `0xA0-0xFF` | Latin‑1: `à á â ã ä ç è é ê ñ ò ó ö ù ü …` and `£ ¥ § ° ± µ ¿ ¡ « » × ÷` |
  | `0x100-0x17F` | Latin Extended‑A: Croatian `Ćć Čč Đđ Šš Žž` (+ Polish/Czech/Hungarian/Baltic) |
  | `0x2013-0x2014` | en/em dash – — |
  | `0x2018-0x201D` | curly quotes ‘ ’ “ ” |
  | `0x20AC 0x2022 0x2026 0x2122` | € • … ™ |

## Method A — online converter (no install)

1. Open the LVGL **Font Converter** (v8): <https://lvgl.io/tools/fontconverter>
2. Name `font_memento_14`, Height `14`, Bpp `1 bit-per-pixel`, format **C file**.
3. Add your TTF; in **Range** paste the comma‑separated ranges above.
4. **Convert** → download → save as `font_memento_14.c` in this folder.

## Method B — `lv_font_conv` CLI (needs Node.js)

```bash
npm i -g lv_font_conv
lv_font_conv --font DejaVuSans.ttf --size 14 --bpp 1 --format lvgl --no-compress \
  -r 0x20-0x7F -r 0xA0-0x17F -r 0x2013-0x2014 -r 0x2018-0x201D \
  -r 0x20AC -r 0x2022 -r 0x2026 -r 0x2122 \
  --force-fast-kern-format --lv-font-name font_memento_14 -o font_memento_14.c
```

## Activate

1. Put `font_memento_14.c` in this folder.
2. In `dms_ui.c` set `#define MEMENTO_FONT_EXTENDED 1`.
3. Run `idf.py reconfigure` once so the new font file is picked up (a plain rebuild won't
   re-scan the folder), then `idf.py -p COM4 flash monitor`.

If the build can't find `lvgl.h` from the generated file, open it and make sure the top uses
`#include "lvgl.h"` (this project includes LVGL that way).

## Troubleshooting

- **`'lv_font_t' has no member named 'static_bitmap'`** — the converter emitted an LVGL **v9**
  field, but this project is **v8.4**. Guard that one line in the generated `.c`:
  ```c
  #if LVGL_VERSION_MAJOR >= 9
      .static_bitmap = 0,
  #endif
  ```
  (A `-Woverride-init` warning on `.dsc` next to it is harmless and goes away with the guard.)

## Notes

- This only affects what the **device screen can draw**. Notes/passwords are stored as raw bytes
  (UTF‑8) and are saved/round‑tripped correctly regardless of the font.
- The web upload page is rendered by the phone/PC browser, which already shows full Unicode.
- ~14 px @ 1 bpp over these ranges adds roughly **30–60 KB** of flash (well within budget).
