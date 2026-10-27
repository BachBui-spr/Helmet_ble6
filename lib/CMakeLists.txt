# Thu vien dung chung cua cac app (lop 1-2), nap nhu mot Zephyr module (zephyr/module.yml). Moi lop la mot thu vien rieng va CHI thay header
# cua chinh no va cua lop no duoc phep dung -> include sai lop la loi bien dich:
#   core     : <me54/core/...>                       (C thuan, khong Zephyr)
#   hal      : <me54/hal/...>, <me54/core/...>
#   services : <me54/services/...>, hal, core, proto
#   proto    : chi header (<me54/proto/...>)
# Moi module chi duoc build khi app bat Kconfig tuong ung (lib/Kconfig).

set(ME54_LIB_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE INTERNAL "lib/ cua ME54BS62")

if(CONFIG_ME54_LIB_CORE)
  add_subdirectory(core)
endif()
if(CONFIG_ME54_LIB_HAL)
  add_subdirectory(hal)
endif()
if(CONFIG_ME54_LIB_SERVICES)
  add_subdirectory(services)
endif()
