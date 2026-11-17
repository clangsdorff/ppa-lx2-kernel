/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _HISI_TYPEC_SECUREC_H
#define _HISI_TYPEC_SECUREC_H

#include <linux/kernel.h>
#include <linux/string.h>

#define EOK 0

static inline int memcpy_s(void *d, size_t dmax, const void *s, size_t n)
{
	if (!d || !s || n > dmax)
		return -EINVAL;
	memcpy(d, s, n);
	return EOK;
}

static inline int memset_s(void *d, size_t dmax, int c, size_t n)
{
	if (!d || n > dmax)
		return -EINVAL;
	memset(d, c, n);
	return EOK;
}

#define snprintf_s(buf, dmax, n, fmt, ...) \
	snprintf((buf), min_t(size_t, (dmax), (size_t)(n) + 1), (fmt), ##__VA_ARGS__)

#endif
