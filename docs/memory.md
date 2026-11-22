# Internal RAM

The ESP32-S3 has about 340 KB of internal SRAM, shared between code that must run from IRAM and the heap. Everything that is not speed-critical or touched by DMA lives in the 8 MB of PSRAM instead (most task stacks, mbedTLS, cJSON, the model, Wi-Fi/lwIP buffers where the driver allows it, the doorbell JPEG decoder). This page records what still needs internal RAM, how much, and what the November 2026 memory pass changed.

## Targets and result

Worst case: every source connected, doorbell video running, the waterfall's receive audio on, an alert chime every minute and the settings page loaded every 30 seconds, for 10 minutes and more.

| Measure | Before | After |
|---|---|---|
| Internal free, steady under full load | 45 to 48 KB | 84 to 86 KB |
| Internal free, lowest point (worst case soak) | 4.8 KB | 49.5 KB (50,691 B) |
| Internal free, lowest point after boot | 8.9 KB | 54 KB |
| Display fps under full load (same page) | 26.4 | 25.7 |
| Display fps, idle | 32 | 31.5 to 32 |
| OTA upload under full load | works | works, 14 s |

The one cost is the LVGL band height (below): about 1 fps under the heaviest load, none when idle.

## How to measure

`GET /api/mem` (admin) returns internal and PSRAM free, lowest and largest block, and every task's stack: bytes never used (`stack_left`), and whether it is in PSRAM. `POST /api/test/load?listen=1&video=1` (admin) turns on the waterfall audio (muted) and the doorbell video as the pages would, `0` turns them off. The status line on the console prints `heap ..K int (min ..K)` and fps every 30 seconds.

## Where internal RAM goes

| Consumer | Before | After | Notes |
|---|---|---|---|
| LVGL draw bands (two, DMA) | 2 x 51,840 B (72 lines) | 2 x 46,080 B (64 lines) | 11.5 KB saved, costs about 1 fps under full load: 360 lines take 6 bands instead of 5 |
| LVGL task stack | 12,288 B (6 KB never used) | 10,240 B (3.9 KB never used) | internal: it runs flash-writing menu actions |
| Web server stack | 12,288 B | 10,240 B (2.4 KB never used under load) | internal: settings save, OTA and crash erase write flash |
| Main task stack | 8,192 B | 6,144 B (3.4 KB never used) | internal: first boot writes the set-up AP password to NVS |
| Power task stack | 3,072 B | 4,096 B (3.3 KB never used) | internal: the brightness save on sleep writes NVS, about 2 KB of stack |
| MQTT task stack | 6,144 B | 4,096 B (1.9 KB never used) | no flash writes |
| esp_timer, Wi-Fi, lwIP, IPC, idle stacks | unchanged | unchanged | system tasks; esp_timer runs the delayed brightness save |
| Source task stacks (ping, KISS, metrics, energy, rooms, ADS-B, waterfall, GPSDO, door, audio, LEDs, haptic) | PSRAM | PSRAM | none of them writes flash |
| Wi-Fi static TX buffers | 16 x 1.6 KB | 8 x 1.6 KB | dynamic TX buffers are not allowed with Wi-Fi/lwIP in PSRAM |
| Wi-Fi static RX buffers | 8 x 1.6 KB | unchanged | |
| Wi-Fi dynamic RX buffers | up to 32 x 1.6 KB | unchanged | the main cause of short dips; fewer buffers or a smaller block-ack window made the knob drop packets |
| TCP receive window | 32 KB | 16 KB | received frames wait in internal RAM until the task reads them, so a burst (a page sending its history on connect) could take tens of KB |
| PHY code in IRAM | yes | no | IRAM and heap share the same SRAM |
| Small allocations forced internal | under 1024 B | under 256 B | LVGL objects and other small blocks move to PSRAM, about 8 KB, no fps change measured |
| JPEG decoder (doorbell) | aligned buffers internal | PSRAM | the door task's non-DMA aligned allocations are steered to PSRAM |
| All sources connecting at boot | at once | one at a time, 1.5 s apart | at once was the lowest point after boot, about 25 KB below steady |
| mbedTLS, cJSON, model, httpd buffers | PSRAM | unchanged | |

## Levers tried and measured

Each step was built in a fresh build directory, flashed and soaked with the worst-case load. Lowest internal free in each soak:

| Build | Change | Lowest free | Steady free | fps under load |
|---|---|---|---|---|
| baseline | | 4.8 KB | 46 KB | not measured separately (about 26.5) |
| opt1 | stacks, TX buffers, PHY out of IRAM, JPEG aligned allocations | 26 KB | 69 KB | |
| opt2 | sources staggered, 16 dynamic RX buffers, block-ack 8 | 25.6 KB | | 25.9 |
| opt3 | 12 dynamic RX buffers, block-ack 8, bands 64 | network failures (DNS errors, timeouts, slow OTA): reverted | | |
| opt4 | bands 64, TCP window 16 KB, RX settings back to defaults | 41.1 KB | 80 KB | 25.3 |
| ab72 | as opt4 with bands 72 | 24 KB | 68 KB | 26.4 |
| ab72a256 | bands 72, small allocations under 256 B internal | about 35 KB | 76 KB | 26.3 |
| opt5 | as ab72a256, 24 dynamic RX buffers | 31 KB | 73 KB | 27 to 29 (page changed) |
| final | bands 64, small allocations under 256 B, power stack 4 KB, main 6 KB | 49.5 KB | 86 KB | 25.7 |

Bands of 72 lines would recover the 1 fps but leave the lowest point near 32 KB, below the 40 KB target.

## Things to keep in mind

- A task that writes flash (NVS, OTA, coredump erase, the OTA health check) needs an internal stack, and enough of it: an NVS write takes about 2 KB. During the memory pass the power task's stack was cut to 2 KB and the first sleep after a brightness change overflowed it and crashed the knob (the backtrace ended in the FreeRTOS tick handler, the overflow having corrupted the task list). It is now 4 KB.
- The LVGL draw bands must stay in internal DMA-capable RAM.
- Keep `CONFIG_LWIP_MAX_SOCKETS=24`: the default of 10 starves the web server.
- Change sdkconfig.defaults in a fresh build directory, or delete `sdkconfig`, or the old values stay in force.

## The 2.4 GHz scope

The scope's capture engine writes into a fixed 64 KB bank of internal SRAM (0x3fcd0000 to 0x3fce0000). That bank is reserved at link time and handed back to the heap at the start of every normal boot, so the pages lose nothing to it; only a scope boot keeps it out. Measured after the scope went in (build c69fe3f):

| Measure | Value |
|---|---|
| Normal boot, steady under the full worst-case load | 80 to 81 KB |
| Normal boot, lowest point in a 6 minute worst-case soak (including the boot) | 44.6 KB |
| Scope boot, after the radio starts | 57 KB |
| Scope boot, capturing (FFT buffer internal, window and sums in PSRAM) | 49 KB, lowest 48 KB |
| Scope boot, in the moment of restarting back | dips to about 17 KB, then the restart frees everything |

The FFT works on 1024 points for every span (a 2048-point version left only 25 KB free in a scope boot).
