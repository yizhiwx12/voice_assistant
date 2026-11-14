idf_component_register(
    SRCS "main.cpp" "app/voice_assistant_app.cpp"
         "app/conversation_controller.cpp"
         "app/memory_diagnostics.cpp"
         "app/audio_idle_guard.cpp"
         "app/local_buttons.cpp"
         "audio/ics43434_microphone.cpp" "audio/max98357a_audio.cpp"
         "audio/playback_reference.cpp"
         "audio/reference_resampler.cpp" "audio/reference_timeline.cpp"
         "audio/reference_monitor.cpp"
         "audio/acoustic_delay_probe.cpp"
         "audio/audio_service.cpp" "audio/opus_codec.cpp"
         "audio/local_audio_frontend.cpp" "audio/sr_model_store.cpp"
         "display/st7789_display.cpp" "display/voice_assistant_ui.cpp"
         "display/lvgl_memory.cpp"
         "network/wifi_station.cpp"
         "network/voice_client.cpp" "network/voice_protocol.cpp"
         "fonts/lv_font_chinese_16.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_driver_ledc esp_driver_i2s esp_driver_uart
            esp_wifi esp_event esp_netif nvs_flash esp_http_client lvgl esp_lvgl_port
            esp_websocket_client 78__esp-opus espressif__cjson esp_timer mbedtls
            espressif__esp-sr esp_partition
)

# IDF 公开写入接口不暴露接收 PCM 的具体物理 DMA 缓冲区。
# 在构建目录生成带观测逻辑的副本，不修改已安装的 SDK。
# 保留原有写入、调度和清零行为；
# 发现未知 SDK 实现时明确拒绝，不静默估计时间。
idf_component_get_property(voice_i2s_lib esp_driver_i2s COMPONENT_LIB)
idf_component_get_property(voice_i2s_dir esp_driver_i2s COMPONENT_DIR)
set(voice_i2s_source "${voice_i2s_dir}/i2s_common.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${voice_i2s_source}")
file(READ "${voice_i2s_source}" voice_i2s_original)
set(voice_i2s_needle "        memcpy(data_ptr, src_byte, bytes_can_write);")
# MATCHALL 中不能包含 C 语句末尾的分号：CMake 将它视为列表分隔符，
# 会把一次匹配误计为两个列表元素。
# 下面再用 FIND 校验包含分号的完整语句。
string(REGEX MATCHALL "memcpy\\(data_ptr, src_byte, bytes_can_write\\)" voice_i2s_matches "${voice_i2s_original}")
list(LENGTH voice_i2s_matches voice_i2s_match_count)
if(NOT voice_i2s_match_count EQUAL 1)
    message(FATAL_ERROR "Voice subtitle DMA observer needs the verified IDF 6.1 i2s_channel_write memcpy; SDK changed")
endif()
string(FIND "${voice_i2s_original}" "${voice_i2s_needle}" voice_i2s_needle_position)
if(voice_i2s_needle_position LESS 0)
    message(FATAL_ERROR "Voice subtitle DMA observer cannot find the exact verified write statement")
endif()
set(voice_i2s_hook "        voice_i2s_write_observer(handle, handle->dma.curr_ptr, handle->dma.rw_pos, (size_t)(src_byte - (const char *)src), bytes_can_write, 0);\n${voice_i2s_needle}\n        voice_i2s_write_observer(handle, handle->dma.curr_ptr, handle->dma.rw_pos, (size_t)(src_byte - (const char *)src), bytes_can_write, 1);")
string(REPLACE "${voice_i2s_needle}" "${voice_i2s_hook}" voice_i2s_observed "${voice_i2s_original}")
set(voice_i2s_observed "#include \"i2s_write_observer.h\"\n${voice_i2s_observed}")
# 保留 SDK 目标文件的基础名：中断和控制函数配置到 IRAM 时，
# esp_driver_i2s/linker.lf 会按 i2s_common 选择代码。
# 重命名 C 文件会静默绕过这些放置规则。
set(voice_i2s_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/voice_i2s_observed")
file(MAKE_DIRECTORY "${voice_i2s_generated_dir}")
set(voice_i2s_generated "${voice_i2s_generated_dir}/i2s_common.c")
file(WRITE "${voice_i2s_generated}" "${voice_i2s_observed}")
get_target_property(voice_i2s_sources ${voice_i2s_lib} SOURCES)
list(LENGTH voice_i2s_sources voice_i2s_before_count)
list(FILTER voice_i2s_sources EXCLUDE REGEX "(^|[/\\\\])i2s_common\\.c$")
list(LENGTH voice_i2s_sources voice_i2s_after_count)
math(EXPR voice_i2s_removed "${voice_i2s_before_count} - ${voice_i2s_after_count}")
if(NOT voice_i2s_removed EQUAL 1)
    message(FATAL_ERROR "Cannot replace exactly one esp_driver_i2s/i2s_common.c source for the voice DMA observer")
endif()
list(APPEND voice_i2s_sources "${voice_i2s_generated}")
set_property(TARGET ${voice_i2s_lib} PROPERTY SOURCES "${voice_i2s_sources}")
target_include_directories(${voice_i2s_lib} PRIVATE "${voice_i2s_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/audio")

if(CONFIG_VOICE_AFE_SIGNAL_PROBE_BOOT)
    target_sources(${COMPONENT_LIB} PRIVATE "audio/afe_signal_probe.cpp")
endif()

# 参考音频转换与 AEC 并行，每秒持续处理 100 个 DMA 块。
# 保留其他应用代码的可调试性，只优化这段有界 DSP 和时间线路径，
# 避免为它沿用项目默认的 -Og。
# 保留 IEEE 检查以及 FIR 乘加运算的舍入顺序，不启用快速数学优化。
set_source_files_properties(
    audio/reference_resampler.cpp
    audio/reference_timeline.cpp
    audio/reference_monitor.cpp
    audio/playback_reference.cpp
    PROPERTIES COMPILE_OPTIONS "-O2;-ffp-contract=off"
)
