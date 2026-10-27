# Lop DICH VU: thread / hang doi / trang thai, ghep HAL + CORE
zephyr_library_named(me54_services)
target_include_directories(me54_services PRIVATE
  include
  ${ME54_LIB_DIR}/hal/include
  ${ME54_LIB_DIR}/core/include
  ${ME54_LIB_DIR}/proto/include)

zephyr_library_sources_ifdef(CONFIG_ME54_SVC_AUDIO_STREAM  audio_stream.c)
zephyr_library_sources_ifdef(CONFIG_ME54_SVC_AUDIO_OUT     audio_out.c)
zephyr_library_sources_ifdef(CONFIG_ME54_SVC_VOICE_TX      voice_tx.c)
zephyr_library_sources_ifdef(CONFIG_ME54_SVC_BUTTONS       buttons.c)
zephyr_library_sources_ifdef(CONFIG_ME54_SVC_PROMPT_PLAYER prompt_player.c)

set_source_files_properties(audio_out.c voice_tx.c PROPERTIES COMPILE_OPTIONS "-O3")
