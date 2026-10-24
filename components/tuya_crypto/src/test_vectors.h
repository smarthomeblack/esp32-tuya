// GENERATED FILE — DO NOT EDIT. Regenerate: python scripts/gen_test_vectors.py
// Golden byte-level vectors for the tuya_crypto self-test: MD5, HMAC-SHA256,
// secret, AES-128-GCM encdata, JSON escaping, base64 (Tuya SDK ground truth).

#pragma once

#include <cstddef>

namespace tuya_crypto::test_vectors {

// ---- MD5: input -> 32-char lowercase hex digest (hash_key derivation) ----
struct Md5Vector { const char* input; size_t input_len; const char* expected_hex; };
inline constexpr Md5Vector kMd5[] = {
    // empty input (RFC 1321)
    {"", 0, "d41d8cd98f00b204e9800998ecf8427e"},
    // RFC 1321 'abc'
    {"abc", 3, "900150983cd24fb0d6963f7d28e17f72"},
    // rid + refresh_token (hash_key derivation)
    {"3f2504e0-4f89-11d3-9a0c-0305e82c3301sample-refresh-token-0123456789abcdef", 73, "af9041ae81be9b4a498f2384cfa3ce40"},
};

// ---- HMAC-SHA256: (key, msg) -> 64-char lowercase hex digest ----
struct HmacVector { const char* key; size_t key_len; const char* msg;
                     size_t msg_len; const char* expected_hex; };
inline constexpr HmacVector kHmacSha256[] = {
    // _secret_generating: key=rid, msg=hash_key
    {"3f2504e0-4f89-11d3-9a0c-0305e82c3301", 36, "af9041ae81be9b4a498f2384cfa3ce40", 32, "fec489ac7665231e74f846f3dba7c3c6c1b4325be07b6bf99d0832c58ced2877"},
    // _restful_sign: key=hash_key, msg=sign string
    {"af9041ae81be9b4a498f2384cfa3ce40", 32, "X-appKey=HA_3y9q4ak7g4ephrvke||X-requestId=3f2504e0-4f89-11d3-9a0c-0305e82c3301||X-time=1731838024000", 101, "a571919eed2ddcf5c4a0a035c3cdabc21c63984f345e445da4e3d1528cd9eee3"},
};

// ---- secret derivation: HMAC-SHA256(key=rid, msg=hash_key) hexdigest[:16] ----
struct SecretVector { const char* rid; const char* hash_key;
                       const char* expected_secret; };
inline constexpr SecretVector kSecretGenerate[] = {
    // secret = first 16 hex chars of HMAC(rid, hash_key)
    {"3f2504e0-4f89-11d3-9a0c-0305e82c3301", "af9041ae81be9b4a498f2384cfa3ce40", "fec489ac7665231e"},
};

// ---- AES-128-GCM pack: secret (16 hex chars = 16 ASCII key bytes), nonce ----
// ---- (12 chars from the Tuya alphabet), plaintext -> encdata ----
struct AesGcmVector { const char* secret; const char* nonce;
                       const char* plaintext; size_t plaintext_len;
                       const char* encdata; };
inline constexpr AesGcmVector kAesGcm[] = {
    // devices-by-home query plaintext
    {"0123456789abcdef", "ABCDEFGHJKMN", "{\"homeId\":\"123456\"}", 19, "QUJDREVGR0hKS01OzR8HZPeld6/qOHfFqOc0OTMdoRWMgB4EqqbY3Sh1uT/ZSbc="},
    // command body
    {"0123456789abcdef", "ABCDEFGHJKMN", "{\"commands\":[{\"code\":\"switch_led\",\"value\":true}]}", 49, "QUJDREVGR0hKS01OzR8MZPetX6WscXfOwa8ib2pbufAyKooozlJP8dVJ6L71uIxqWg5212JJwFx72ZC10NPWm/awfCjFAeo+Hj477WI="},
    // non-ASCII name: ensure_ascii escapes + astral surrogate pair
    {"0123456789abcdef", "ABCDEFGHJKMN", "{\"name\":\"\\u0110\\u00e8n ph\\u00f2ng kh\\u00e1ch \\ud83d\\udecb\"}", 59, "QUJDREVGR0hKS01OzR8BavelHPHqXiDEq+UwUHAP7LcwZtkvz3pZqbpDv7SwtMV0ZxczgiVC10Yu4JiMlUk2kBE5N2ndPWNwE30KhfO1cHyUPaGG2WEc"},
    // empty plaintext (ciphertext is tag-only)
    {"0123456789abcdef", "ABCDEFGHJKMN", "", 0, "QUJDREVGR0hKS01OlyQQuLuLg5NsxCl7pqTySg=="},
};

// ---- JSON string escaping: input (raw UTF-8) -> json.dumps({"k": input}) ----
struct JsonEscapeVector { const char* input; size_t input_len;
                           const char* expected_json; };
inline constexpr JsonEscapeVector kJsonEscape[] = {
    // Latin-1 supplement code points
    {"\304\220\303\250n", 5, "{\"k\":\"\\u0110\\u00e8n\"}"},
    // astral + variation selector (two surrogate pairs)
    {"emoji \360\237\233\213\357\270\217", 13, "{\"k\":\"emoji \\ud83d\\udecb\\ufe0f\"}"},
    // quote, backslash, newline, tab, NUL, C0 controls, DEL
    {"a\"b\\c\012d\011e\000f\001g\037h\177", 16, "{\"k\":\"a\\\"b\\\\c\\nd\\te\\u0000f\\u0001g\\u001fh\\u007f\"}"},
    // empty string
    {"", 0, "{\"k\":\"\"}"},
    // forward slash is NOT escaped
    {"slash/a/b", 9, "{\"k\":\"slash/a/b\"}"},
};

// ---- JSON object build: {"name": input} (unicode name value) ----
struct JsonNameVector { const char* input; size_t input_len;
                         const char* expected_json; };
inline constexpr JsonNameVector kJsonName[] = {
    // protocol-style name value
    {"\304\220\303\250n ph\303\262ng kh\303\241ch \360\237\233\213", 24, "{\"name\":\"\\u0110\\u00e8n ph\\u00f2ng kh\\u00e1ch \\ud83d\\udecb\"}"},
};

// ---- JSON structural kitchen sink: containers, null, int min/max, double, ----
// ---- booleans (built in code by self_test.cc; expected from json.dumps) ----
inline constexpr const char* kJsonMiscExpected = "{\"empty\":{},\"arr\":[],\"nil\":null,\"i\":-9223372036854775808,\"u\":18446744073709551615,\"d\":1.5,\"t\":true,\"f\":false}";

// ---- base64 encode: bytes -> standard base64 with padding ----
struct Base64EncodeVector { const char* data; size_t len;
                             const char* expected; };
inline constexpr Base64EncodeVector kBase64Encode[] = {
    // empty input
    {"", 0, ""},
    // bytes 0..15 (includes NUL)
    {"\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017", 16, "AAECAwQFBgcICQoLDA0ODw=="},
    // classic 6-byte input
    {"foobar", 6, "Zm9vYmFy"},
};

// ---- base64 decode: strict decode must reproduce the exact bytes ----
struct Base64DecodeVector { const char* input; const char* expected;
                             size_t expected_len; };
inline constexpr Base64DecodeVector kBase64Decode[] = {
    {"", "", 0},
    {"Zm9vYmFy", "foobar", 6},
    {"AAECAwQFBgcICQoLDA0ODw==", "\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017", 16},
    {"QUJDREVGR0hKS01O", "ABCDEFGHJKMN", 12},
};

// ---- base64 decode: inputs that must be rejected (strict validation) ----
inline constexpr const char* kBase64DecodeInvalid[] = {
    "A",
    "!!!!",
    "abcde",
    "AB=C",
    "====",
};

}  // namespace tuya_crypto::test_vectors
