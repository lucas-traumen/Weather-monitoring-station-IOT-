#include <string.h>

#include "core.h"
#include "printstate.h"

int crypto_aead_decrypt_tag4(unsigned char *m,
                             unsigned long long *mlen,
                             unsigned char *nsec,
                             const unsigned char *c,
                             unsigned long long clen,
                             const unsigned char *ad,
                             unsigned long long adlen,
                             const unsigned char *npub,
                             const unsigned char *k)
{
    enum { ASCON_TAG4_BYTES = 4 };

    if ((m == NULL) || (mlen == NULL) || (c == NULL) || (npub == NULL) || (k == NULL)) {
        if (mlen != NULL) {
            *mlen = 0;
        }
        return -1;
    }

    if (clen < ASCON_TAG4_BYTES) {
        *mlen = 0;
        return -1;
    }

    ascon_state_t s;
    (void)nsec;

    /*
     * Input layout:
     *   c[0 .. *mlen-1]       = ciphertext
     *   c[*mlen .. *mlen+3]   = truncated tag, first 4 bytes
     */
    *mlen = clen - ASCON_TAG4_BYTES;

    print("decrypt_tag4\n");
    printbytes("k", k, CRYPTO_KEYBYTES);
    printbytes("n", npub, CRYPTO_NPUBBYTES);
    printbytes("a", ad, adlen);
    printbytes("c", c, *mlen);
    printbytes("t4", c + *mlen, ASCON_TAG4_BYTES);

    /*
     * ascon_core computes full internal 16-byte tag into s.b[3].
     * We only compare the first 4 bytes with the STM32 truncated tag.
     */
    ascon_core(&s, m, c, *mlen, ad, adlen, npub, k, ASCON_DEC);

    int result = 0;
    for (int i = 0; i < ASCON_TAG4_BYTES; ++i) {
        result |= c[*mlen + i] ^ *(s.b[3] + i);
    }

    result = (((result - 1) >> 8) & 1) - 1;

    if (result != 0) {
        memset(m, 0, *mlen);
        *mlen = 0;
        return -1;
    }

    printbytes("m", m, *mlen);
    print("\n");

    return 0;
}
