#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>

/* Compute the SHA-256 digest of `in` as a 64-char lowercase hex string.
 * `out` must be at least 65 bytes. */
void sha256_hex(const char *in, char out[65]);

#endif
