cmake_minimum_required(VERSION 4.2...4.3)

project(eb-recorder VERSION 0.3.4 LANGUAGES CXX)

find_package(libobs CONFIG REQUIRED)
find_package(obs-frontend-api CONFIG REQUIRED)
find_package(Qt6 CONFIG REQUIRED COMPONENTS Core Widgets)

add_library(eb-recorder MODULE src/plugin-main.cpp)

target_compile_features(eb-recorder PRIVATE cxx_std_17)
target_compile_definitions(eb-recorder PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(eb-recorder PRIVATE /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
endif()

target_link_libraries(
  eb-recorder
  PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
)

set_target_properties(
  eb-recorder
  PROPERTIES
    PREFIX ""
    OUTPUT_NAME "eb-recorder"
)

install(
  TARGETS eb-recorder
  RUNTIME DESTINATION "eb-recorder"
  LIBRARY DESTINATION "eb-recorder"
  ARCHIVE DESTINATION "eb-recorder"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" DESTINATION "eb-recorder/data")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" DESTINATION "eb-recorder")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION "eb-recorder")

option(EB_RECORDER_BUILD_TESTS "Build headless recording lifecycle regression tests" OFF)
if(EB_RECORDER_BUILD_TESTS)
  enable_testing()
  add_executable(recording-lifecycle-tests tests/recording-lifecycle.cpp)
  target_compile_features(recording-lifecycle-tests PRIVATE cxx_std_17)
  target_compile_definitions(recording-lifecycle-tests PRIVATE UNICODE _UNICODE)
  if(MSVC)
    target_compile_options(recording-lifecycle-tests PRIVATE /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
  endif()
  target_link_libraries(recording-lifecycle-tests PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets)
  add_test(NAME recording-lifecycle COMMAND recording-lifecycle-tests)
  set_tests_properties(recording-lifecycle PROPERTIES TIMEOUT 30)
endif()

