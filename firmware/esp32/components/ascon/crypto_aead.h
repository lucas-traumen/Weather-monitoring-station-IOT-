#ifndef CRYPTO_AEAD_H_
#define CRYPTO_AEAD_H_

#include "api.h"

int crypto_aead_encrypt(unsigned char *c,
                        unsigned long long *clen,
                        const unsigned char *m,
                        unsigned long long mlen,
                        const unsigned char *ad,
                        unsigned long long adlen,
                        const unsigned char *nsec,
                        const unsigned char *npub,
                        const unsigned char *k);

int crypto_aead_decrypt(unsigned char *m,
                        unsigned long long *mlen,
                        unsigned char *nsec,
                        const unsigned char *c,
                        unsigned long long clen,
                        const unsigned char *ad,
                        unsigned long long adlen,
                        const unsigned char *npub,
                        const unsigned char *k);

/*
 * Project-specific truncated-tag decrypt:
 * ciphertext || tag4
 *
 * WARNING:
 * This verifies only the first 4 bytes of the ASCON tag.
 * It is compatible with STM32 sending mac_tag[4], but security is reduced.
 */
int crypto_aead_decrypt_tag4(unsigned char *m,
                             unsigned long long *mlen,
                             unsigned char *nsec,
                             const unsigned char *c,
                             unsigned long long clen,
                             const unsigned char *ad,
                             unsigned long long adlen,
                             const unsigned char *npub,
                             const unsigned char *k);

#endif

