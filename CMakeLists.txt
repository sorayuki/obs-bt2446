cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/bt2446-filter.c)

# Keep the effect inside the module: some hosts load
# plugins from <host>/plugins and only resolve module data under
# <host>/data/plugins, so a data directory next to the DLL is not guaranteed
# to exist.
#
# The header is produced by a CMake script run through `${CMAKE_COMMAND} -P`, so
# building the plugin needs nothing beyond the compiler and CMake itself.
set(_bt2446_generated_header "${CMAKE_CURRENT_BINARY_DIR}/bt2446-resources.h")
add_custom_command(
  OUTPUT "${_bt2446_generated_header}"
  COMMAND
    ${CMAKE_COMMAND}
    "-DEFFECT=${CMAKE_CURRENT_SOURCE_DIR}/data/bt2446.effect"
    "-DOUTPUT=${_bt2446_generated_header}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/src/gen_resources.cmake"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gen_resources.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/bt2446.effect"
  COMMENT "Generating bt2446-resources.h"
  VERBATIM
)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${_bt2446_generated_header}")
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
