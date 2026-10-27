# Lop CORE: thuat toan thuan (khong include Zephyr, thu duoc tren PC)
zephyr_library_named(me54_core)
target_include_directories(me54_core PRIVATE include)

zephyr_library_sources_ifdef(CONFIG_ME54_CORE_PROMPTS     prompts.c)
zephyr_library_sources_ifdef(CONFIG_ME54_CORE_AUDIO_DSP   pcm_ring.c resampler.c mixer.c)
zephyr_library_sources_ifdef(CONFIG_ME54_CORE_VOICE_DSP   voice_dsp.c)
zephyr_library_sources_ifdef(CONFIG_ME54_CORE_MLX90632    mlx90632_calc.c)
zephyr_library_sources_ifdef(CONFIG_ME54_CORE_LC3_SILENCE lc3_silence.c)

# Chay moi 10 ms trong duong am thanh / cuoc goi: toi uu toc do thay cho -Os mac dinh
set_source_files_properties(voice_dsp.c PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math")
set_source_files_properties(pcm_ring.c resampler.c mixer.c PROPERTIES COMPILE_OPTIONS "-O3")
