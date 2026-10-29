# ESP32-P4 Launcher (factory app).
# Board selection:  idf.py -DLX_BOARD=<board_id> build   (default: guition_p4_43)
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED LX_BOARD)
    set(LX_BOARD "guition_p4_43")
endif()
set(LX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
if(NOT EXISTS "${LX_ROOT}/boards/${LX_BOARD}/board/CMakeLists.txt")
    message(FATAL_ERROR "Unknown board '${LX_BOARD}' (no boards/${LX_BOARD}/board)")
endif()

set(EXTRA_COMPONENT_DIRS "${LX_ROOT}/components" "${LX_ROOT}/boards/${LX_BOARD}")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${LX_ROOT}/boards/${LX_BOARD}/sdkconfig.board")
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${LX_BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(launcher)
