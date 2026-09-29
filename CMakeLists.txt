zephyr_library()

if(CONFIG_ECBM_ZEPHYR)
    zephyr_library_sources(
        ecbm_zephyr.c
    )
endif()

zephyr_library_include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ecbm-c
    ${CMAKE_CURRENT_SOURCE_DIR}/../safe-c
    ${CMAKE_CURRENT_SOURCE_DIR}/../framer7b-c
)
