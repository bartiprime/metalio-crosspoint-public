#pragma once
// ---------------------------------------------------------------------------
// SHA-256 for the firmware flashers, on ESP-IDF 6.1 (mbedTLS 4.x / TF-PSA-Crypto).
//
// Why this exists: mbedTLS 4.x removed the legacy SHA-256 module. The header at
//   .../tf-psa-crypto/drivers/builtin/include/mbedtls/private/sha256.h
// still declares `typedef struct mbedtls_sha256_context`, which is why the type
// is visible, but the mbedtls_sha256_init/starts/update/finish/free prototypes
// are compiled out (the module is gone) -- so the type alone is useless.
//
// Rather than lean on a private, prototype-less header, this provides the same
// streaming lifecycle over PSA crypto under names that cannot collide with any
// mbedTLS symbol. The flashers call these directly.
// ---------------------------------------------------------------------------
#include <cstddef>
#include <cstdint>
#include <psa/crypto.h>

namespace crosspoint {

struct Sha256 {
    psa_hash_operation_t op;
};

namespace detail {
inline void ensurePsaInit() {
    static bool inited = false;
    if (!inited) {
        (void)psa_crypto_init();  // idempotent; failure surfaces at setup()
        inited = true;
    }
}
}  // namespace detail

inline void sha256Init(Sha256& ctx) { ctx.op = PSA_HASH_OPERATION_INIT; }

// Returns 0 on success, -1 on failure (same convention as the mbedTLS calls it
// replaces: 0 is success, non-zero is an error).
inline int sha256Starts(Sha256& ctx) {
    detail::ensurePsaInit();
    psa_hash_abort(&ctx.op);  // discard anything a previous starts() left behind
    ctx.op = PSA_HASH_OPERATION_INIT;
    return psa_hash_setup(&ctx.op, PSA_ALG_SHA_256) == PSA_SUCCESS ? 0 : -1;
}

inline int sha256Update(Sha256& ctx, const uint8_t* data, size_t len) {
    if (len == 0) return 0;
    return psa_hash_update(&ctx.op, data, len) == PSA_SUCCESS ? 0 : -1;
}

inline int sha256Finish(Sha256& ctx, uint8_t out[32]) {
    size_t olen = 0;
    const psa_status_t st = psa_hash_finish(&ctx.op, out, 32, &olen);
    return (st == PSA_SUCCESS && olen == 32) ? 0 : -1;
}

inline void sha256Free(Sha256& ctx) { psa_hash_abort(&ctx.op); }

}  // namespace crosspoint
