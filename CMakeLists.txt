cmake_minimum_required(VERSION 3.22)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER_FILE)
string(STRIP "${PROJECT_VER_FILE}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tuya_hub)
