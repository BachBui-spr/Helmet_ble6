# Sau khi nap, runner nrfutil mac dinh reset bang chan nRESET (RESET_PIN) voi nRF54L.
# Tren mach nay reset qua chan khong co tac dung (CPU dung o __start sau khi nap)
# -> dung reset he thong qua SWD (RESET_SYSTEM) de firmware chay ngay.
board_runner_args(nrfutil "--softreset")

if(CONFIG_SOC_NRF54L15_CPUAPP)
  board_runner_args(jlink "--device=nRF54L15_M33" "--speed=4000")
elseif(CONFIG_SOC_NRF54L15_CPUFLPR)
  board_runner_args(jlink "--device=nRF54L15_RV32" "--speed=4000")
endif()

if(CONFIG_TRUSTED_EXECUTION_NONSECURE)
  set(TFM_PUBLIC_KEY_FORMAT "full")
endif()

if(CONFIG_TFM_FLASH_MERGED_BINARY)
  set_property(TARGET runners_yaml_props_target PROPERTY hex_file tfm_merged.hex)
endif()

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)