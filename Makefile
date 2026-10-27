# ============================================================
#  ME54BS62 (nRF54L15) - lenh tat build / nap / log
#  App: apps/main (mu bao hiem, mac dinh), apps/bike (bo trung tam tren xe).
#  Ma dung chung cua hai app (lop HAL / core / dich vu / giao thuc): lib/
#  Vi du:  make build          make flash         make rtt         make audio
#          make APP=apps/bike build flash         (bo trung tam tren xe)
#          make JLINK_SN=69750999 flash   (khi cam nhieu J-Link)
# ============================================================
SHELL := /bin/bash

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
APP       ?= apps/main
BOARD     ?= me54bs62/nrf54l15/cpuapp
BUILD_DIR ?= build/$(notdir $(APP))
UART_PORT ?= /dev/ttyACM0
UART_BAUD ?= 115200
JLINK_SN  ?=
EXTRA     ?=
AUDIO_OPTS ?=

ENV       := source $(ROOT)/env.sh >/dev/null &&
DEV_ID    := $(if $(JLINK_SN),--dev-id $(JLINK_SN),)
SN_OPT    := $(if $(JLINK_SN),--serial-number $(JLINK_SN),)

# IntelliSense (VS Code C/C++): build/compile_commands.json luon tro toi app vua build.
# Voi sysbuild, file nay nam trong thu muc con mang ten app: <BUILD_DIR>/<ten_app>/
LINK_CDB  := ln -sfn $(abspath $(BUILD_DIR))/$(notdir $(APP))/compile_commands.json $(ROOT)/build/compile_commands.json

.PHONY: help build pristine flash erase-flash recover reset rtt audio uart menuconfig \
        guiconfig debug devices info clean

help:
	@echo "make build          - build $(APP) cho $(BOARD)"
	@echo "make pristine       - build lai tu dau"
	@echo "make flash          - nap qua J-Link (SWD)"
	@echo "make erase-flash    - XOA TOAN BO RRAM + UICR roi nap"
	@echo "make recover        - mo khoa + XOA TOAN BO chip (khi bi APPROTECT)"
	@echo "make reset          - reset chip"
	@echo "make rtt            - xem log + shell qua RTT (chi can SWD)"
	@echo "make audio          - nghe/luu am thanh mu nhan duoc (thay cho make rtt)"
	@echo "make uart           - xem log + shell qua UART $(UART_PORT) @ $(UART_BAUD)"
	@echo "make menuconfig     - chinh Kconfig"
	@echo "make debug          - debug bang GDB qua J-Link"
	@echo "make devices / info - liet ke probe / thong tin chip"
	@echo "make APP=apps/bike build flash    - bo trung tam tren xe (bike hub)"

build:
	$(ENV) west build -b $(BOARD) -d $(BUILD_DIR) $(APP) -- -DBOARD_ROOT=$(ROOT) $(EXTRA)
	@$(LINK_CDB)

pristine:
	$(ENV) west build -p always -b $(BOARD) -d $(BUILD_DIR) $(APP) -- -DBOARD_ROOT=$(ROOT) $(EXTRA)
	@$(LINK_CDB)

flash:
	$(ENV) west flash -d $(BUILD_DIR) $(DEV_ID)

erase-flash:
	$(ENV) west flash -d $(BUILD_DIR) --erase $(DEV_ID)

recover:
	$(ENV) nrfutil device recover $(SN_OPT)

reset:
	$(ENV) nrfutil device reset --reset-kind RESET_SYSTEM $(SN_OPT)

rtt:
	JLINK_SN=$(JLINK_SN) $(ROOT)/scripts/rtt.sh $(BUILD_DIR)

# Chi danh cho apps/main (RTT kenh 1 "HelmetAudio", bat bang "helmet capture on"). Chay bang python3 cua he thong
# (can liblc3 + paplay), KHONG qua env.sh. Them tuy chon: make audio AUDIO_OPTS="--no-play"
audio:
	$(ROOT)/scripts/helmet_audio.py --build $(BUILD_DIR) $(if $(JLINK_SN),--sn $(JLINK_SN),) $(AUDIO_OPTS)

uart:
	picocom -b $(UART_BAUD) --imap lfcrlf $(UART_PORT)

menuconfig:
	$(ENV) west build -d $(BUILD_DIR) -t menuconfig

guiconfig:
	$(ENV) west build -d $(BUILD_DIR) -t guiconfig

debug:
	$(ENV) west debug -d $(BUILD_DIR) --runner jlink $(DEV_ID)

devices:
	$(ENV) nrfutil device list

info:
	$(ENV) nrfutil device device-info $(SN_OPT)

clean:
	rm -rf $(BUILD_DIR)
