###############################################################################
 # Copyright (C) 2024 Analog Devices, Inc.
 #
 # Licensed under the Apache License, Version 2.0 (the "License");
 # you may not use this file except in compliance with the License.
 # You may obtain a copy of the License at
 #
 #     http://www.apache.org/licenses/LICENSE-2.0
 #
 # Unless required by applicable law or agreed to in writing, software
 # distributed under the License is distributed on an "AS IS" BASIS,
 # WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 # See the License for the specific language governing permissions and
 # limitations under the License.
 #
 ##############################################################################

# This project requires MAX32655FTHR board
BOARD = FTHR_Apps_P1

# Enable Cordio BLE library
LIB_CORDIO = 1

# Cordio library options
INIT_PERIPHERAL = 1
INIT_CENTRAL = 0

# TRACE option
TRACE = 0

# Enable SDHC + FatFS library
LIB_SDHC = 1
FATFS_VERSION = ff15

# Stack size for WSF, FatFS work area, and minimp3 frame decoder
PROJ_CFLAGS += -DSTACK_SIZE=0x8000

# Route console UART to UART0
PROJ_CFLAGS += -DCONSOLE_UART=0
PROJ_CFLAGS += -DCONSOLE_BAUD=115200

# Optimization for real-time audio playback
MXC_OPTIMIZE_CFLAGS = -O2
