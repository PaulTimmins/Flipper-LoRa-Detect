#include "identity.h"
#include "crypto/tweetnacl.h"
#include <string.h>
#if defined(__arm__)
#include <furi_hal.h>
void randombytes(unsigned char* data, unsigned long long length) {
    furi_hal_random_fill_buf(data, (uint32_t)length);
}
#else
extern void randombytes(unsigned char*, unsigned long long);
#endif
static void wipe(void* data, unsigned n) {
    volatile uint8_t* p = data;
    while(n--)
        *p++ = 0;
}
void lora_identity_generate(LoraIdentity* id) {
    uint8_t secret[64];
    do {
        crypto_sign_keypair(id->mc_public, secret);
    } while(id->mc_public[0] == 0 || id->mc_public[0] == 255);
    /* MeshCore stores expanded/clamped Ed25519 secret, not seed || public. */
    crypto_hash(id->mc_private, secret, 32);
    id->mc_private[0] &= 248;
    id->mc_private[31] &= 63;
    id->mc_private[31] |= 64;
    wipe(secret, sizeof(secret));
    randombytes(id->mt_private, 32);
    id->mt_private[0] &= 248;
    id->mt_private[31] &= 127;
    id->mt_private[31] |= 64;
    crypto_scalarmult_base(id->mt_public, id->mt_private);
}
