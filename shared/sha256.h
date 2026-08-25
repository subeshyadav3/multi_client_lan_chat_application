#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>

/* Compute the SHA-256 digest of the string `in` and write it to `out`
 * as a 64-character lowercase hex string, followed by a '\0' terminator.
 * `out` must have room for at least 65 bytes.                       */
void sha256_hex(const char *in, char out[65]);

#endif /* SHA256_H */
