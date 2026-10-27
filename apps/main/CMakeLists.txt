cmake_minimum_required(VERSION 3.20.0)

# Lop 1-2 dung chung (lib/) la mot Zephyr module: moi lop mot thu vien, chi build module bat
# trong prj.conf (Kconfig cua lib/ nam trong menu "Modules")
set(ME54_LIB ${CMAKE_CURRENT_SOURCE_DIR}/../../lib)
list(APPEND ZEPHYR_EXTRA_MODULES ${ME54_LIB})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(helmet_main)

# Lop 3-4 cua mu: include theo duong dan tu src/ ("app/app.h", "features/phone/le_audio.h")
target_include_directories(app PRIVATE
  src
  ${ME54_LIB}/services/include
  ${ME54_LIB}/hal/include
  ${ME54_LIB}/core/include
  ${ME54_LIB}/proto/include)

add_subdirectory(src/app)                 # lop 4: ung dung
add_subdirectory(src/features/phone)      # lop 3: chuc nang
add_subdirectory(src/features/intercom)
add_subdirectory(src/features/broadcast)
add_subdirectory(src/features/thermo)
add_subdirectory(src/features/nav)
add_subdirectory(src/debug)               # chan doan
