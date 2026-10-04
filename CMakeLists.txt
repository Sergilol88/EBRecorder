cmake_minimum_required(VERSION 4.2...4.3)

project(eb-recorder VERSION 0.3.0 LANGUAGES CXX)

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
  RUNTIME DESTINATION "eb-recorder/bin/64bit"
  LIBRARY DESTINATION "eb-recorder/bin/64bit"
  ARCHIVE DESTINATION "eb-recorder/bin/64bit"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/" DESTINATION "eb-recorder/data")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" DESTINATION "eb-recorder")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION "eb-recorder")
