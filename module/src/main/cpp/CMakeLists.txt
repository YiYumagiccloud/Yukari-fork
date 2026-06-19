cmake_minimum_required(VERSION 3.22.1)
project(Yukari)

add_library(yukari SHARED
    main.cpp
    config.cpp
    service_match.cpp
    service_cache.cpp
    binder_hook.cpp
    logger.cpp
)

target_include_directories(yukari PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(yukari
    log
    dl
)

set_target_properties(yukari PROPERTIES
    OUTPUT_NAME "yukari"
)
