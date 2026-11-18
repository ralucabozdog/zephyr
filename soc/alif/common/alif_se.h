/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Analog Devices, Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_ALIF_COMMON_ALIF_SE_H_
#define ZEPHYR_SOC_ALIF_COMMON_ALIF_SE_H_

#include <stddef.h>
#include <stdint.h>

/* Error code value the Secure Enclave never returns */
#define ALIF_SE_NO_ANSWER 0xA5A5U

/* Header of every Secure Enclave service message */
struct alif_se_header {
	uint16_t service_id;
	uint16_t flags;
	uint16_t error_code;
	uint16_t padding;
};

/**
 * @brief Call a Secure Enclave service.
 *
 * The message starts with a struct alif_se_header and is replaced by the
 * answer. The service error code, which most services return at the end
 * of the message, is left for the caller to check.
 *
 * @param msg Request and answer of the service.
 * @param size Size of the message in bytes.
 *
 * @retval 0 The Secure Enclave handled the request.
 * @retval -EINVAL The message is too large.
 * @retval -EBUSY The Secure Enclave mailbox is busy.
 * @retval -ETIMEDOUT The Secure Enclave did not answer.
 * @retval -EIO The Secure Enclave reported a transport error.
 */
int alif_se_call(void *msg, size_t size);

#endif /* ZEPHYR_SOC_ALIF_COMMON_ALIF_SE_H_ */
