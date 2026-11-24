#pragma once

#define SCR_W 320
#define SCR_H 240

#define HDR_H 22
#define NAV_H 30
#define NAV_Y (SCR_H - NAV_H)
#define BODY_Y HDR_H
#define BODY_H (SCR_H - HDR_H - NAV_H)

// touch controller wiring (CYD, separate SPI bus from the LCD)
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

#define PIN_BL 21

#define REFRESH_DEFAULT 60
#define REFRESH_MIN 30
#define REFRESH_MAX 300

#define HTTP_TIMEOUT_MS 5000
#define HIST_LEN 48
#define MAX_SYMBOLS 96
#define MAX_WATCH 40

// touch calibration defaults, raw XPT2046 ranges for the CYD panel
#define CAL_X1 125
#define CAL_X2 3810
#define CAL_Y1 70
#define CAL_Y2 3920
#define CAL_VERSION 3

// RGB565 palette
#define C_BG 0x0861
#define C_CARD 0x10E3
#define C_CARD2 0x1946
#define C_LINE 0x2965
#define C_TXT 0xFFFF
#define C_DIM 0x8C71
#define C_ACC 0x07FF
#define C_UP 0x07E0
#define C_DOWN 0xF800
#define C_FLAT 0xBDF7
#define C_AMBER 0xFD20

#define WIFI_AP_NAME "ESP-Stocks"
#define WIFI_AP_PASS "espstocks"
