# SPDX-License-Identifier: MIT

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR
  src/input_processor.c
  src/input_processor_helpers.c
  src/behavior_pointer_mode.c)
zephyr_library_sources_ifdef(CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR
  src/input_processor_dynamic_settings.c)
zephyr_include_directories(include)
