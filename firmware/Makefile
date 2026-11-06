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

ifeq "$(TARGET)" ""
TARGET := MAX32655
TARGET_UC := MAX32655
TARGET_LC := max32655
else
TARGET_UC := $(subst m,M,$(subst a,A,$(subst x,X,$(TARGET))))
TARGET_LC := $(subst M,m,$(subst A,a,$(subst X,x,$(TARGET))))
endif

BOARD ?= FTHR_Apps_P1

ifneq "$(MAXIM_PATH)" ""
MAXIM_PATH := $(subst \,/,$(MAXIM_PATH))
LIBS_DIR := $(abspath $(MAXIM_PATH)/Libraries)
CMSIS_ROOT := $(LIBS_DIR)/CMSIS
endif

PROJECTMK ?= $(abspath ./project.mk)
include $(PROJECTMK)

ifeq "$(MAXIM_PATH)" ""
DEPTH := ../../../../
MAXIM_PATH := $(abspath $(DEPTH))
endif

LIBS_DIR := $(abspath $(MAXIM_PATH)/Libraries)
CMSIS_ROOT := $(LIBS_DIR)/CMSIS

TARGET_UC := $(subst m,M,$(subst a,A,$(subst x,X,$(TARGET))))
TARGET_LC := $(subst M,m,$(subst A,a,$(subst X,x,$(TARGET))))

export TARGET
export TARGET_UC
export TARGET_LC
export CMSIS_ROOT

VPATH += .
VPATH += src
VPATH := $(VPATH)

IPATH += .
IPATH += include
IPATH := $(IPATH)

AUTOSEARCH ?= 1
ifeq ($(AUTOSEARCH), 1)
SRCS += $(wildcard $(addsuffix /*.c, $(VPATH)))
SRCS += $(wildcard $(addsuffix /*.cpp, $(VPATH)))
endif

SRCS := $(SRCS)
PROJECT ?= $(TARGET_LC)

ifeq "$(COMPILER)" ""
COMPILER := GCC
endif

MXC_OPTIMIZE_CFLAGS ?= -O2

PROJ_CFLAGS += -Wall
PROJ_CFLAGS += -DMXC_ASSERT_ENABLE

MFLOAT_ABI ?= softfp
export MFLOAT_ABI

VPATH += $(CMSIS_ROOT)/Device/Maxim/$(TARGET_UC)/Source

SBT ?= 0

ifeq "$(.DEFAULT_GOAL)" ""
override .DEFAULT_GOAL := all
endif

ifeq "$(MAKECMDGOALS)" ""
MAKECMDGOALS:=$(.DEFAULT_GOAL)
endif

include $(LIBS_DIR)/libs.mk
include $(CMSIS_ROOT)/Device/Maxim/$(TARGET_UC)/Source/$(COMPILER)/$(TARGET_LC).mk

all:
	$(PREFIX)-size --format=berkeley $(BUILD_DIR)/$(PROJECT).elf
