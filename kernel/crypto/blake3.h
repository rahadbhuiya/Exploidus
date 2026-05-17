#pragma once
#include <stdint.h>

/*
 * blake3_hash — compute a 32-byte BLAKE3 digest.
 * input: data pointer and length
 * output: 32-byte digest buffer
 */
void blake3_hash(const uint8_t *input, uint64_t input_len, uint8_t *output);
