#pragma once

// Host shim shadowing <psa/crypto.h>. Implements exactly the PSA Crypto API
// surface the tuya_crypto wrappers use, with self-contained software
// SHA-256 / MD5 / HMAC-SHA256 / AES-128 (ECB, CTR, GCM). Correctness is
// pinned by the repo's own golden-vector self-tests.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_usage_t;
typedef uint16_t psa_key_type_t;
typedef uint32_t psa_key_bits_t;
typedef uint32_t mbedtls_svc_key_id_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t) - 132)
#define PSA_ERROR_NOT_SUPPORTED ((psa_status_t) - 134)
#define PSA_ERROR_INVALID_ARGUMENT ((psa_status_t) - 135)
#define PSA_ERROR_INSUFFICIENT_MEMORY ((psa_status_t) - 141)
#define PSA_ERROR_INVALID_SIGNATURE ((psa_status_t) - 143)
#define PSA_ERROR_INVALID_HANDLE ((psa_status_t) - 136)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t) - 138)

// Algorithm identifiers: shim-local values; the wrappers only pass them back.
#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x0200)
#define PSA_ALG_MD5 ((psa_algorithm_t)0x0300)
#define PSA_ALG_HMAC(alg) ((psa_algorithm_t)(0x01000000u | (alg)))
#define PSA_ALG_GCM ((psa_algorithm_t)0x0A000001)
#define PSA_ALG_ECB_NO_PADDING ((psa_algorithm_t)0x0B000001)

#define PSA_KEY_TYPE_HMAC ((psa_key_type_t)0x0100)
#define PSA_KEY_TYPE_AES ((psa_key_type_t)0x2400)

#define PSA_KEY_USAGE_SIGN_MESSAGE ((psa_key_usage_t)0x2000)
#define PSA_KEY_USAGE_ENCRYPT ((psa_key_usage_t)0x0100)
#define PSA_KEY_USAGE_DECRYPT ((psa_key_usage_t)0x0200)

typedef struct psa_key_attributes_s {
    psa_key_type_t type;
    psa_key_bits_t bits;
    psa_key_usage_t usage;
    psa_algorithm_t alg;
} psa_key_attributes_t;

#define PSA_KEY_ATTRIBUTES_INIT {0, 0, 0, 0}

void psa_set_key_type(psa_key_attributes_t* attributes, psa_key_type_t type);
void psa_set_key_bits(psa_key_attributes_t* attributes, psa_key_bits_t bits);
void psa_set_key_usage_flags(psa_key_attributes_t* attributes, psa_key_usage_t usage);
void psa_set_key_algorithm(psa_key_attributes_t* attributes, psa_algorithm_t alg);

psa_status_t psa_crypto_init(void);

psa_status_t psa_import_key(const psa_key_attributes_t* attributes, const uint8_t* data,
                            size_t data_length, mbedtls_svc_key_id_t* key);
psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key);

psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t* input, size_t input_length,
                              uint8_t* hash, size_t hash_size, size_t* hash_length);

psa_status_t psa_mac_compute(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t* input,
                             size_t input_length, uint8_t* mac, size_t mac_size,
                             size_t* mac_length);

psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t* nonce,
                              size_t nonce_length, const uint8_t* additional_data,
                              size_t additional_data_length, const uint8_t* plaintext,
                              size_t plaintext_length, uint8_t* output, size_t output_size,
                              size_t* output_length);

psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t* nonce,
                              size_t nonce_length, const uint8_t* additional_data,
                              size_t additional_data_length, const uint8_t* ciphertext,
                              size_t ciphertext_length, uint8_t* output, size_t output_size,
                              size_t* output_length);

#ifdef __cplusplus
}
#endif
