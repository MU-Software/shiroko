idf_component_register(SRCS main.c ../../../tests/bench/rec.c ../../../tests/bench/rec_api.c PRIV_INCLUDE_DIRS ../../../tests/bench
                       PRIV_REQUIRES shiroko esp_lcd esp_timer esp_partition esp_driver_ppa esp_driver_dma esp_mm)
foreach(_opt TAB5_LOADS TAB5_MOVES TAB5_DMA_MBPS TAB5_MOVE_CHUNKS TAB5_STEPS TAB5_REPLAY TAB5_REPLAY_FRAMES TAB5_REPLAY_PARTS
             TAB5_COST TAB5_FPS_CAP TAB5_SOAK TAB5_SOAK_K TAB5_ROTATION)
  if(DEFINED ${_opt})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_opt}=${${_opt}})
  endif()
endforeach()
