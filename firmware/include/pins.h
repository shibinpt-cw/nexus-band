// Pin map of the Nexus Band mainboard, rev A.
// Keep this file in step with pcb/nexus_band.kicad_sch.
#pragma once

// Display: ST7789 over 4-wire SPI (Newhaven NHD-2.4-240320AF-CSXP-CTP)
#define PIN_LCD_CS    10
#define PIN_LCD_MOSI  11
#define PIN_LCD_SCK   12
#define PIN_LCD_DC    13
#define PIN_LCD_RST   14
#define PIN_LCD_TE    21   // tearing-effect output from the panel (optional)
#define PIN_LCD_BL     9   // PWM, high = backlight on (N-MOSFET low side)

// Touch panel (FT5426, address 0x38) and haptic driver (DRV2605L, 0x5A) share I2C
#define PIN_I2C_SDA   17
#define PIN_I2C_SCL   18
#define PIN_TP_INT    16
#define PIN_TP_RST    15

// Two ICS-43434 microphones on one I2S bus (MK1 = left slot, MK2 = right slot)
#define PIN_MIC_SCK    5
#define PIN_MIC_WS     6
#define PIN_MIC_SD     7

// MAX98357A speaker amplifier
#define PIN_AMP_BCLK  40
#define PIN_AMP_LRCLK 41
#define PIN_AMP_DIN   42
#define PIN_AMP_SD    39   // low = shutdown, high = on (left channel)

// Power
#define PIN_BAT_ADC    4   // VBAT through 470k / 470k  ->  VBAT = 2 x reading
#define PIN_VBUS_SENSE 8   // dock 5 V through 100k / 100k, about 2.5 V when docked

// Side button, also the BOOT strap (active low, 10k pull-up on the board)
#define PIN_BUTTON     0
