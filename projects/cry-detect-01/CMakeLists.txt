cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
)

project(cry-detect-01)

spiffs_create_partition_image(yamnet spiffs FLASH_IN_PROJECT)
