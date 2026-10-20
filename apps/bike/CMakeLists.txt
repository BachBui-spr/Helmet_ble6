cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bike_hub)

target_sources(app PRIVATE
  src/main.c
  src/phone_sink.c
  src/bcast_source.c
  src/bhs.c
  src/hub_ui.c
)
