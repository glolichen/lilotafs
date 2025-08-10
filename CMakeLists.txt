set(LILOTAFS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(LILOTAFS_SRCS
    ${LILOTAFS_DIR}/lilotafs.c
    ${LILOTAFS_DIR}/flash.c
)

idf_component_register(
    SRCS         ${LILOTAFS_SRCS}
    INCLUDE_DIRS "."
    REQUIRES     spi_flash esp_partition
)