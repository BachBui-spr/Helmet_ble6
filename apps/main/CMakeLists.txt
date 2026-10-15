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
)
