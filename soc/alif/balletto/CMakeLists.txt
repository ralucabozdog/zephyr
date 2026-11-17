# SPDX-FileCopyrightText: Copyright Alif Semiconductor
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_include_directories(../common)

# Controller-mode setup for the DesignWare SSI instances
zephyr_sources_ifdef(CONFIG_SPI_DW ../common/ssi.c)

add_subdirectory(common)
add_subdirectory(${CONFIG_SOC_SERIES})
