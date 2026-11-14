# 在 project() 后加载。使用 LVGL 公开内存池钩子，
# 以 PSRAM 替代其 64 KiB 内部静态数组，不修改托管组件。
idf_component_get_property(voice_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_include_directories(${voice_lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../main")
target_compile_definitions(${voice_lvgl_lib} PRIVATE
    LV_MEM_POOL_ALLOC=voice_lvgl_pool_alloc
    "LV_MEM_POOL_INCLUDE=\"display/lvgl_memory.h\""
)
# 钩子由应用静态库提供。
# 即使该静态库在链接命令中排在 LVGL 之前，也要强制将符号纳入最终链接。
target_link_libraries(${voice_lvgl_lib} INTERFACE "-Wl,--undefined=voice_lvgl_pool_alloc")
