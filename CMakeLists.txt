cmake_minimum_required(VERSION 3.28...3.30)

option(BUILD_PARSER_TESTS_ONLY "Build standalone text parser tests without OBS or Qt" OFF)
if(BUILD_PARSER_TESTS_ONLY)
  project(obs-textppt-tests LANGUAGES CXX)
  enable_testing()
  add_subdirectory(tests)
  return()
endif()

include(cmake/common/bootstrap.cmake NO_POLICY_SCOPE)
project(${_name} VERSION ${_version} LANGUAGES C CXX)
if(NOT WIN32 AND NOT APPLE)
  message(FATAL_ERROR "Supported platforms: Windows x64 and macOS arm64")
endif()
include(compilerconfig)
include(defaults)
include(helpers)
find_package(libobs 32 REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Widgets)
add_library(${CMAKE_PROJECT_NAME} MODULE)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/native-text.cpp
    src/obs-textppt.cpp
    src/obs-textppt-dock.cpp
    src/obs-textppt-dock.h
    src/page-list.h
    src/text-parser.cpp
    src/text-layout.cpp
    src/text-editor.cpp
    src/group-source.cpp
)
if(WIN32)
  install(FILES install_obs-textppt.bat DESTINATION .)
else()
  install(PROGRAMS install_obs-textppt.sh DESTINATION .)
endif()
configure_file(src/plugin-macros.h.in "${CMAKE_CURRENT_BINARY_DIR}/plugin-macros.generated.h" @ONLY)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON PREFIX "")
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
target_add_resource(${CMAKE_PROJECT_NAME} "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

option(BUILD_OBS_TEST_PROBE "Build the isolated OBS UI test module" OFF)
option(BUILD_DOCK_TESTS "Build the standalone Dock list checks" OFF)
if(BUILD_OBS_TEST_PROBE OR BUILD_DOCK_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
