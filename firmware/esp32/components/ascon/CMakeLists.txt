file(GLOB_RECURSE ascon_sources
    "${CMAKE_CURRENT_LIST_DIR}/*.c"
)

idf_component_register(
    SRCS ${ascon_sources}
    INCLUDE_DIRS "."
)