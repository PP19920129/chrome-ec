/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "dcrypto.h"
#include "internal.h"

#include "cryptoc/sha384.h"

void DCRYPTO_SHA384_init(LITE_SHA512_CTX *ctx)
{
	SHA384_init(ctx);
}

const uint8_t *DCRYPTO_SHA384_hash(const void *data, uint32_t n,
				uint8_t *digest)
{
	return SHA384_hash(data, n, digest);
}
