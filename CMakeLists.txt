# 以下基础配置必须保持当前顺序，才能让 CMake 正确加载 ESP-IDF 项目。
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice_assistant)

include(${CMAKE_CURRENT_LIST_DIR}/tools/lvgl_psram_pool.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/tools/p4_model_manifest.cmake)
