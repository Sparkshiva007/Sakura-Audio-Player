# 🌸 Sakura Audio Player — Hardware Pinout & Architecture

This document provides the complete pin mapping and peripheral interconnects for the **Sakura Audio Player** built on the **Analog Devices MAX32655FTHR** platform (ARM Cortex-M4 @ 100MHz + BLE 5.2).

---

## 1. System Pinout Mapping Table

| Peripheral | Signal Name | MCU Pin | MAX32655 Port | Configuration / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **ST7789 Display** | LCD_SCK | Pin 21 | `P0.21` | SPI1 Master Clock (Up to 30 MHz) |
| | LCD_MOSI (SDA) | Pin 22 | `P0.22` | SPI1 Master Out Slave In |
| | LCD_CS | Pin 20 | `P0.20` | GPIO Output, Active-Low Chip Select |
| | LCD_DC | Pin 40 | `P1.8` | GPIO Output (0 = Command, 1 = Data) |
| | LCD_RST | Pin 41 | `P1.9` | GPIO Output, Active-Low Hardware Reset |
| | LCD_VCC | 3V3 | 3.3V Rail | Power rail (MAX20303 PMIC regulated) |
| | LCD_GND | GND | Ground | Common system ground |
| **MicroSD Slot** | SD_SCK | Pin 7 | `P0.7` | SPI0 Master Clock (25 MHz SD SPI Mode) |
| | SD_MOSI (DI) | Pin 5 | `P0.5` | SPI0 Master Out Slave In |
| | SD_MISO (DO) | Pin 6 | `P0.6` | SPI0 Master In Slave Out (Pull-Up) |
| | SD_CS | Pin 4 | `P0.4` | GPIO Output, Active-Low SD Chip Select |
| | SD_DET | Pin 12 | `P0.12` | GPIO Input, Card Detect (Active-Low) |
| | SD_PWR | Pin 15 | `P0.15` | GPIO Output, SD Card Power Switch |
| **Audio Codec (MAX9867)** | I2C_SCL | Pin 16 | `P0.16` | I2C1 Clock (100 kHz standard mode) |
| | I2C_SDA | Pin 17 | `P0.17` | I2C1 Data (100 kHz standard mode) |
| | I2S_BCLK | Pin 26 | `P0.26` | I2S0 Bit Clock |
| | I2S_LRCLK (WS) | Pin 27 | `P0.27` | I2S0 Left/Right Word Select (48 kHz) |
| | I2S_SDOUT | Pin 25 | `P0.25` | I2S0 Serial Data Output to Codec |
| **3-Way Jog Switch** | NAV_UP | Pin 44 | `P2.2` | GPIO Input, Internal Pull-Up (Track Previous/Up) |
| | NAV_CENTER | Pin 43 | `P2.1` | GPIO Input, Internal Pull-Up (Select/Menu) |
| | NAV_DOWN | Pin 42 | `P2.0` | GPIO Input, Internal Pull-Up (Track Next/Down) |
| **Pushbuttons** | SW2 | Pin 2 | `P0.2` | Onboard PB0 / Backward Button |
| | SW3 | Pin 3 | `P0.3` | Onboard PB1 / Forward Button |
| | SW4 | Pin 48 | `P3.0` | Play / Pause / Select Button |
| **PMIC (MAX20303)** | PMIC_I2C | Internal | I2C0 | 0x28 address, BUCK2 enabled for codec rail |
| **USB Serial / Debug** | UART0_TX | Pin 1 | `P0.1` | Connects to onboard MAX32625PICO (DAPLink) |
| | UART0_RX | Pin 0 | `P0.0` | Connects to onboard MAX32625PICO (DAPLink) |

---

## 2. Audio Pipeline Flow

```
┌─────────────────┐       SPI0 (25 MHz)       ┌────────────────────────┐
│  MicroSD Card   │ ────────────────────────> │   FatFS File System    │
│  (FAT32 / 16GB) │                           │     (MAX32655 MCU)     │
└─────────────────┘                           └───────────┬────────────┘
                                                          │ Stream 4KB Frames
                                                          ▼
┌─────────────────┐       I2S0 (DMA)          ┌────────────────────────┐
│ MAX9867 Stereo  │ <──────────────────────── │    minimp3 Decoder     │
│ Audio Codec     │   16-Bit Stereo @ 48 kHz  │ (ARM Cortex-M4 @ 100M) │
└────────┬────────┘                           └────────────────────────┘
         │
         ▼
 3.5mm Headphone Jack
 (Pristine Audio Playback)
```

---

## 3. Power Management

- **Power Management IC (PMIC):** Analog Devices `MAX20303`.
- **Battery:** 3.7V Single-Cell LiPo (500–1200 mAh recommended).
- **Deep Sleep Current:** `< 15 µA` in full system sleep mode.
- **Wakeup:** Instant hardware interrupt wakeup from `P2.1` (Center Jog Wheel) or `P3.0` (Center Button).

