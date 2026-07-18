# Makefile for ESP-IDF v6.0 under Espressif Installation Manager (EIM)

# Force the use of Bash shell
SHELL := /bin/bash

# Target version managed by EIM (e.g., v6.0 or v6.0.1)
IDF_VER ?= v6.0

.PHONY: all build flash monitor clean menuconfig fullclean uf2 set-target

all: build

set-target:
	eim run "idf.py set-target esp32c3" $(IDF_VER)

build: 
	eim run "idf.py build" $(IDF_VER)

flash: 
	eim run "idf.py flash" $(IDF_VER)

monitor:
	eim run "idf.py monitor" $(IDF_VER)

menuconfig: 
	eim run "idf.py menuconfig" $(IDF_VER)

clean:
	eim run "idf.py clean" $(IDF_VER)

fullclean:
	eim run "idf.py fullclean" $(IDF_VER)

uf2:
	eim run "idf.py uf2" $(IDF_VER)
