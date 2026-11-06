# 🌸 Sakura Audio Player — Bill of Materials (BOM)

| Component | Description | Part Number / Reference | Quantity | Link / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Main Controller** | ARM Cortex-M4 MCU with BLE 5.2, 512KB Flash, 128KB SRAM | **MAX32655FTHR#** (Featherboard) | 1 | Analog Devices Evaluation Kit |
| **Audio Codec** | Ultra-Low Power 16/24-Bit Stereo Audio Codec with Headphone Amp | **MAX9867ETJ+** | 1 | I2S + I2C Interface |
| **Display** | 1.9-Inch 170x320 Full-Color IPS LCD (SPI Interface) | **MSP1901 (ST7789V2)** | 1 | 4-Wire SPI (CS, DC, RST, SDA, SCL) |
| **Storage** | MicroSD Card 8GB / 16GB / 32GB (FAT32 Formatted) | SanDisk Ultra / Kingston Class 10 | 1 | Plugs into MAX32655 MicroSD slot |
| **Navigation Input** | 3-Way Jog Switch / Rotary Navigation Dial (Up, Center-Press, Down) | 5-Pin SMD / Thru-Hole Jog Dial | 1 | Side-mounted for track & volume navigation |
| **Play/Pause Input** | Tactile Pushbutton (6x6mm momentary switch) | SPST Tactile Switch | 1 | Center Play / Pause / Power button |
| **Power Supply** | 3.7V Rechargeable Lithium Polymer Battery (LiPo) | 500mAh - 1200mAh LiPo Cell | 1 | JST-PH 2.0mm connector to MAX32655FTHR |
| **Enclosure** | Custom 3D Printed Handheld Case with Sakura Cherry Blossom Motifs | PLA / PETG Mint-Green Filament | 1 | STL files included in `hardware/enclosure/` |
| **Headphone Jack** | 3.5mm Stereo Audio Jack Socket | PJ-320A / PJ-392 | 1 | Stereo output from MAX9867 codec |

