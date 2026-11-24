# ESP Stocks

Desk stock monitor for the **ESP32 Cheap Yellow Display** (ESP32-2432S028R, 2.8" 240x320 ILI9341 + XPT2046 touch).

Crypto, BIST and US markets on one touch screen, free APIs, no keys, no cloud account.

## Features

- Crypto (Binance public API), BIST and US stocks/ETFs (Yahoo Finance chart API)
- Built-in catalog of 71 symbols (crypto, BIST, US stocks/ETFs/indices, all verified)
- SET > Symbols: browse the catalog per market, tap rows to add/remove from the watchlist (max 40), RESET restores defaults
- Tiles: symbol, price, 24h/day change, sparkline, colour-coded trend
- Touch the tile for a detail view: big price, change, chart with min/max, prev close, day high/low, source and age
- Market tabs: CRYPTO / BIST / US / ALL
- Layouts: 1x1, 1x2, 2x2, 3x2 - tap the grid button in the bottom bar
- Paging: tap the header or swipe left/right; pages auto-rotate every 12 s while idle
- Tap the clock area to force a refresh
- Settings: refresh interval (30/60/120/300 s), backlight brightness, touch recalibration, WiFi reset
- WiFi setup portal on first boot, credentials stored in NVS
- Clock via NTP (Europe/Istanbul), WiFi status dot in the header

Data is polled, not streamed: a full pass takes one refresh interval, one HTTP request per symbol, spread out to stay well inside free-tier limits. BIST/US quotes come from Yahoo and can be delayed; crypto is Binance 15 m candles.

## Build

```bash
python3 -m venv .venv
.venv/bin/pip install platformio
.venv/bin/pio run
```

## Flash

Serial access needs the `uucp` group, or flash through docker:

```bash
sudo usermod -aG uucp $USER        # then log out/in
.venv/bin/pio run -t upload

# or, without group changes:
docker run --rm --device=/dev/ttyUSB0 -v "$PWD":/w \
  -v ~/.platformio/packages/framework-arduinoespressif32/tools/partitions:/parts -w /w \
  esptool:local esptool --chip esp32 --port /dev/ttyUSB0 --baud 921600 write-flash -z \
  0x1000 .pio/build/cyd/bootloader.bin \
  0x8000 .pio/build/cyd/partitions.bin \
  0xe000 /parts/boot_app0.bin \
  0x10000 .pio/build/cyd/firmware.bin
```

Monitor it with `.venv/bin/pio device monitor` (115200).

## WiFi

1. Join WiFi `ESP-Stocks`, password `espstocks`
2. Open `http://192.168.4.1`
3. Pick your network, enter the password, save

The portal closes after 4 minutes and the board restarts. Use **SET -> WiFi reset** to provision another network.

## Touch calibration

Defaults match the standard CYD panel. To recalibrate without touch:

1. Hold **BOOT**, press and release **RST** (or replug USB while holding BOOT), release BOOT
2. Tap the centre of the first cyan target, then the second one

Calibration can also be started from **SET -> Touch calibrate**. Values are stored in NVS.

## Controls

| Area | Action |
| --- | --- |
| Bottom bar left | CRYPTO / BIST / US / ALL |
| Bottom bar middle | cycle layout (1x1, 1x2, 2x2, 3x2) |
| Bottom bar right | settings |
| SET -> Symbols | browse catalog, tap to add/remove, RESET/DONE |
| Header clock area | force refresh |
| Header elsewhere / swipe | next page |
| Tile | open detail |
| Detail `<` / swipe right | back |

## Symbols

Everything on screen comes from the watchlist, stored in NVS and edited on the device:

1. **SET -> Symbols**
2. Pick CRYPTO / BIST / US
3. Tap a row to add or remove it (checked box = watched), swipe to scroll
4. **DONE** goes back to the grid, **RESET** restores the default 22

The watchlist cap is 40 symbols. Each watched symbol costs one HTTPS request per refresh cycle; 20-25 at 60 s is comfortable.

The catalog itself is in `src/symbols.cpp`:

```cpp
{"BTC", "Bitcoin", "BTCUSDT", SRC_BINANCE, MKT_CRYPTO, true},
{"THYAO", "Turkish Airlines", "THYAO.IS", SRC_YAHOO, MKT_BIST, true},
{"AAPL", "Apple", "AAPL", SRC_YAHOO, MKT_US, false},
```

`label` is the tile text, `name` the list/detail title, `id` the Binance pair or Yahoo ticker (BIST tickers end in `.IS`), `def` marks it watched on a fresh device. `MAX_SYMBOLS` in `src/config.h` bounds the table and the build asserts it.
