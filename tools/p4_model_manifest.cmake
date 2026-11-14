# 在 project() 后加载，确保 ESP-SR 的模型和烧录目标已存在。
idf_build_get_property(p4_build_dir BUILD_DIR)
idf_component_get_property(p4_sr_dir espressif__esp-sr COMPONENT_DIR)
idf_component_get_property(p4_main_lib main COMPONENT_LIB)

if(NOT TARGET srmodels_bin)
    message(FATAL_ERROR "P4 needs ESP-SR's srmodels_bin target and a model partition")
endif()

set(p4_model_header "${p4_build_dir}/generated/sr_model_manifest.hpp")
set(p4_model_report "${p4_build_dir}/p4_model_manifest.json")
add_custom_command(
    OUTPUT "${p4_model_header}" "${p4_model_report}"
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/prepare_sr_model.py"
            --build-dir "${p4_build_dir}" --component-dir "${p4_sr_dir}"
    DEPENDS "${p4_build_dir}/srmodels/srmodels.bin"
            "${p4_build_dir}/partition_table/partition-table.bin"
            "${p4_build_dir}/flasher_args.json"
            "${p4_sr_dir}/idf_component.yml"
            "${CMAKE_CURRENT_LIST_DIR}/prepare_sr_model.py"
    COMMENT "Verify ESP-SR model and create P4 deployment manifest"
    VERBATIM
)
add_custom_target(p4_sr_manifest DEPENDS "${p4_model_header}" "${p4_model_report}")
add_dependencies(p4_sr_manifest srmodels_bin partition_table_bin)
add_dependencies(${p4_main_lib} p4_sr_manifest)
target_include_directories(${p4_main_lib} PRIVATE "${p4_build_dir}/generated")
