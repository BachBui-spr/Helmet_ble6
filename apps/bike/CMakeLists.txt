cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bike_hub)

target_sources(app PRIVATE
  src/main.c
  src/phone_sink.c
  src/bcast_source.c
  src/bhs.c
  src/hub_ui.c
  src/nav_hub.c
)

# Dinh dang quang ba Dinh vi dung chung voi apps/main (common/nav_proto.h)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common)
