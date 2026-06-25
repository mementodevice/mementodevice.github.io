# Memento
Third-party notices

## Description of the document

This document lists the third-party software that Memento bundles or builds against. Each component
is the property of its respective authors and is used under the licence noted below. The Memento
project's own code is licensed separately under the MIT Licence (see `LICENSE`).

## Bundled and referenced components

| Component | Licence | Copyright | Where it is used |
|---|---|---|---|
| LVGL | MIT | (c) 2021 LVGL Kft | `managed_components/lvgl__lvgl/` (fetched by the ESP-IDF component manager; pinned in `dependencies.lock`) |
| SensorLib | MIT | (c) 2022 lewis he | `components/SensorLib/` (PCF85063 RTC driver, via `i2c_equipment`); full text in `components/SensorLib/LICENSE` |
| MultiButton (`multi_button`) | MIT | (c) 2016 Zibin Zheng | `components/button_bsp/` (`multi_button.c/.h`) |
| ESP-IDF and Mbed TLS | Apache 2.0 | (c) Espressif Systems and the Mbed TLS contributors | Not redistributed in this repository — they are the build SDK and its bundled crypto library, supplied by the local ESP-IDF installation at build time |
| Waveshare board drivers | (see note) | Waveshare | `components/epaper_driver_bsp/`, `board_power_bsp/`, `i2c_bsp/`, `i2c_equipment/`, `adc_bsp/` |

Upstream sources: LVGL <https://github.com/lvgl/lvgl>; SensorLib
<https://github.com/lewisxhe/SensorLib>; MultiButton <https://github.com/0x1abin/MultiButton>;
ESP-IDF <https://github.com/espressif/esp-idf>; Mbed TLS <https://github.com/Mbed-TLS/mbedtls>.

## Note on the Waveshare board drivers

The board support components listed above are derived from Waveshare's `12_RTC_Sleep_Test` example
project. Waveshare publishes this as product demo code and does not ship a formal open-source licence 
with it. The code is included here with attribution so that the project builds out of the box for owners
of this exact board (its intended audience). This is common practice for hobby projects but is not a
formally granted licence; all credit for that driver code remains with Waveshare. Anyone reusing it
elsewhere should consider asking Waveshare.

## MIT Licence (full text)

The following text applies to LVGL, SensorLib and MultiButton.

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
