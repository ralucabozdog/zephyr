# SPDX-FileCopyrightText: Copyright Alif Semiconductor
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_include_directories(../common)

# Controller-mode setup for the DesignWare SSI instances
zephyr_sources_ifdef(CONFIG_SPI_DW ../common/ssi.c)

# Secure Enclave services client
if(CONFIG_UDC_DWC3 OR CONFIG_HWINFO_ALIF)
  zephyr_sources(../common/se.c)
endif()

# USB controller reset release and PHY power
zephyr_sources_ifdef(CONFIG_UDC_DWC3 ../common/usb.c)

# Non-secure DTCM window for bus masters, when enabled in devicetree
dt_nodelabel(dtcm_ns_node NODELABEL "dtcm_ns")
if(DEFINED dtcm_ns_node)
  dt_node_has_status(dtcm_ns_okay PATH "${dtcm_ns_node}" STATUS okay)
  if(dtcm_ns_okay)
    zephyr_sources(../common/dtcm_ns.c)
  endif()
endif()

add_subdirectory(common)
add_subdirectory(${CONFIG_SOC_SERIES})
