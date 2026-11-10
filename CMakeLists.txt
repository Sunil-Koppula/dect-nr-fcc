#
# Copyright 2023-2026 (c) IRISS Inc. All rights reserved.
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dect_nr_fcc)

target_sources(app PRIVATE
  src/main.c
  src/dect_bands.c
  src/settings.c
  src/rf_test.c
  src/at_cmd.c
  src/status_led.c
)
