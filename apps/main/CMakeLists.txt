cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(helmet_main)

target_sources(app PRIVATE
  src/main.c
  src/le_audio.c
  src/media_ctrl.c
  src/audio_out.c
  src/prompts.c
  src/capture.c
  src/buttons.c
  src/call_ctrl.c
  src/power.c
  src/mic_in.c
  src/voice_dsp.c
  src/voice_tx.c
  src/mlx90632.c
  src/thermo.c
  src/status_led.c
  src/intercom.c
  src/ic_service.c
  src/ic_audio.c
  src/broadcast_sink.c
)

# Cac file chay moi 10 ms trong cuoc goi: bien dich toi uu toc do thay cho -Os mac dinh cua
# Zephyr (CONFIG_SIZE_OPTIMIZATIONS). liblc3 da duoc module cua no bien dich -O3 -ffast-math.
# Do bang "helmet perf" va "helmet cpu".
set_source_files_properties(src/voice_dsp.c PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
set_source_files_properties(src/voice_tx.c src/audio_out.c PROPERTIES COMPILE_OPTIONS "-O3")
