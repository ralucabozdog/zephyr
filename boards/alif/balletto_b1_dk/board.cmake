# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

board_runner_args(alif_flash "--device=AB1C1F4M51820_HE")
board_runner_args(jlink "--device=AB1C1F4M51820PH" "--speed=4000")

include(${ZEPHYR_BASE}/boards/common/alif_flash.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
