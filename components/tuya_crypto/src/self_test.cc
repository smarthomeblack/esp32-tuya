#include "tuya_crypto/self_test.h"

#include <esp_log.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "hex_util.h"
#include "test_vectors.h"
#include "tuya_crypto/aes_gcm.h"
#include "tuya_crypto/at_rest_crypto.h"
#include "tuya_crypto/base64.h"
#include "tuya_crypto/hmac_sha256.h"
#include "tuya_crypto/json_guard.h"
#include "tuya_crypto/json_writer.h"
#include "tuya_crypto/md5.h"
#include "tuya_crypto/uuid.h"

static const char* TAG = "SelfTest";

namespace tuya_crypto {

namespace {

struct TestResult {
    int total = 0;
    int passed = 0;
};

// Keep logged expected/got strings short; never dump full binary garbage.
std::string TruncateForLog(const std::string& s, size_t max_chars = 64) {
    if (s.size() <= max_chars) {
        return s;
    }
    return s.substr(0, max_chars) + "...(" + std::to_string(s.size()) + "B)";
}

std::string HexForLog(const uint8_t* data, size_t len, size_t max_bytes = 24) {
    std::string out;
    AppendHexLower(out, data, len > max_bytes ? max_bytes : len);
    if (len > max_bytes) {
        out += "...";
    }
    return out + " (" + std::to_string(len) + "B)";
}

void CheckEqStr(TestResult& r, const char* name, const std::string& expected,
                const std::string& got) {
    ++r.total;
    if (expected == got) {
        ++r.passed;
    } else {
        ESP_LOGE(TAG, "FAIL %s: expected=\"%s\" got=\"%s\"", name, TruncateForLog(expected).c_str(),
                 TruncateForLog(got).c_str());
    }
}

void CheckTrue(TestResult& r, const char* name, bool ok, const std::string& detail = "") {
    ++r.total;
    if (ok) {
        ++r.passed;
    } else {
        ESP_LOGE(TAG, "FAIL %s%s%s", name, detail.empty() ? "" : ": ", detail.c_str());
    }
}

void TestMd5(TestResult& r) {
    for (const auto& v : test_vectors::kMd5) {
        const std::string input(v.input, v.input_len);
        std::string got;
        const bool ok = Md5Hex(input, got);
        CheckEqStr(r, "md5", v.expected_hex, ok ? got : "<error logged above>");
    }
}

void TestHmacSha256(TestResult& r) {
    for (const auto& v : test_vectors::kHmacSha256) {
        const std::string key(v.key, v.key_len);
        const std::string msg(v.msg, v.msg_len);

        std::string got;
        const bool ok = HmacSha256Hex(key, msg, got);
        CheckEqStr(r, "hmac_sha256_hex", v.expected_hex, ok ? got : "<error logged above>");

        if (&v == &test_vectors::kHmacSha256[0]) {
            // Raw variant: 32 bytes whose hex form must equal the digest.
            std::string raw;
            if (HmacSha256(key, msg, raw) && raw.size() == 32) {
                std::string raw_hex;
                AppendHexLower(raw_hex, reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
                CheckEqStr(r, "hmac_sha256_raw", v.expected_hex, raw_hex);
            } else {
                CheckTrue(r, "hmac_sha256_raw", false, "HmacSha256 failed or wrong size");
            }
        }
    }
}

void TestSecretGenerate(TestResult& r) {
    // secret = HMAC-SHA256(key=rid, msg=hash_key) hexdigest[:16]
    for (const auto& v : test_vectors::kSecretGenerate) {
        std::string hex;
        const bool ok = HmacSha256Hex(v.rid, v.hash_key, hex);
        const std::string got = ok ? hex.substr(0, 16) : "";
        CheckEqStr(r, "secret_generate", v.expected_secret, got);
    }
}

void TestAesGcm(TestResult& r) {
    for (const auto& v : test_vectors::kAesGcm) {
        const std::string secret(v.secret);
        const std::string nonce(v.nonce);
        const std::string plaintext(v.plaintext, v.plaintext_len);

        // Encrypt with the fixed nonce -> byte-exact encdata.
        std::string enc;
        const bool ok = AesGcmEncryptPackWithNonce(secret, nonce, plaintext, enc);
        CheckEqStr(r, "aes_gcm_pack", v.encdata, ok ? enc : "<error logged above>");

        // Decrypt the golden encdata -> original plaintext.
        std::string dec;
        const bool ok2 = AesGcmDecryptPack(secret, v.encdata, dec);
        CheckEqStr(r, "aes_gcm_unpack", plaintext, ok2 ? dec : "<error logged above>");
    }

    // Random-nonce pack/unpack roundtrip on the largest plaintext.
    {
        const auto& v = test_vectors::kAesGcm[1];
        const std::string plaintext(v.plaintext, v.plaintext_len);
        std::string enc;
        std::string dec;
        const bool ok =
            AesGcmEncryptPack(v.secret, plaintext, enc) && AesGcmDecryptPack(v.secret, enc, dec);
        CheckEqStr(r, "aes_gcm_random_roundtrip", plaintext, ok ? dec : "<error logged above>");
    }

    // A wrong secret must be rejected by the GCM tag, not mis-decrypt.
    {
        const auto& v = test_vectors::kAesGcm[0];
        std::string dec;
        const bool rejected = !AesGcmDecryptPack("ffffffffffffffff", v.encdata, dec);
        CheckTrue(r, "aes_gcm_wrong_key_rejected", rejected, "unpack with wrong key succeeded");
    }

    // Truncated/garbage encdata must fail cleanly.
    {
        std::string dec;
        const bool rejected = !AesGcmDecryptPack(test_vectors::kAesGcm[0].secret, "QUJD", dec);
        CheckTrue(r, "aes_gcm_short_encdata_rejected", rejected,
                  "unpack of a 4-char encdata succeeded");
    }
}

void TestAtRestCrypto(TestResult& r) {
    // Deterministic AES-256 test key (never the eFuse-derived one).
    std::string key;
    for (int i = 0; i < 32; ++i) {
        key.push_back(static_cast<char>(i));
    }
    std::string payload;
    for (int i = 0; i < 1500; ++i) {
        payload.push_back(static_cast<char>('a' + (i % 26)));
    }

    std::string sealed;
    const bool sealed_ok = Aes256GcmSeal(key, payload, sealed);
    CheckTrue(r, "atrest_magic", sealed_ok && AtRestIsEncrypted(sealed),
              "seal failed or magic missing");
    CheckTrue(r, "atrest_size", sealed.size() == 32 + payload.size(),
              "container size is not header+ct+tag");
    std::string opened;
    CheckEqStr(r, "atrest_roundtrip", payload,
               Aes256GcmOpen(key, sealed, opened) ? opened : "<error logged above>");
    CheckTrue(r, "atrest_legacy_not_encrypted", !AtRestIsEncrypted("{\"version\":1}"),
              "plaintext JSON detected as encrypted");

    // Empty plaintext exercises the zero-length PSA paths.
    std::string empty_sealed;
    std::string empty_opened;
    const bool empty_ok =
        Aes256GcmSeal(key, "", empty_sealed) && Aes256GcmOpen(key, empty_sealed, empty_opened);
    CheckTrue(r, "atrest_empty_roundtrip", empty_ok && empty_opened.empty(),
              "empty plaintext did not round-trip");

    const std::string wrong_key(32, '\xFF');
    CheckTrue(r, "atrest_wrong_key_rejected", !Aes256GcmOpen(wrong_key, sealed, opened),
              "open with wrong key succeeded");

    std::string corrupt = sealed;
    corrupt[corrupt.size() / 2] ^= 0x01;
    CheckTrue(r, "atrest_corrupt_rejected", !Aes256GcmOpen(key, corrupt, opened),
              "tampered ciphertext accepted");

    const std::string truncated = sealed.substr(0, sealed.size() - 1);
    CheckTrue(r, "atrest_truncated_rejected", !Aes256GcmOpen(key, truncated, opened),
              "truncated container accepted");

    // eFuse-derived key round-trip (present once main.cc ran InitAtRestKey).
    if (AtRestKeyReady()) {
        std::string enc;
        std::string dec;
        const bool ok = AtRestSeal(payload, enc) && AtRestOpen(enc, dec);
        CheckEqStr(r, "atrest_efuse_roundtrip", payload, ok ? dec : "<error logged above>");
    }
}

void TestJsonWriter(TestResult& r) {
    // Escaping: {"k": <input>} must equal json.dumps({"k": input}, ...).
    for (const auto& v : test_vectors::kJsonEscape) {
        JsonWriter w;
        w.BeginObject();
        w.Key("k");
        w.String(std::string_view(v.input, v.input_len));
        w.EndObject();
        CheckEqStr(r, "json_escape", v.expected_json, w.Str());
    }

    // Protocol-style name value with astral plane characters.
    for (const auto& v : test_vectors::kJsonName) {
        JsonWriter w;
        w.BeginObject();
        w.Key("name");
        w.String(std::string_view(v.input, v.input_len));
        w.EndObject();
        CheckEqStr(r, "json_name", v.expected_json, w.Str());
    }

    // Structural builds that double as the AES golden plaintexts.
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("homeId");
        w.String("123456");
        w.EndObject();
        const auto& v = test_vectors::kAesGcm[0];
        CheckEqStr(r, "json_struct_home", std::string(v.plaintext, v.plaintext_len), w.Str());
    }
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("commands");
        w.BeginArray();
        w.BeginObject();
        w.Key("code");
        w.String("switch_led");
        w.Key("value");
        w.Bool(true);
        w.EndObject();
        w.EndArray();
        w.EndObject();
        const auto& v = test_vectors::kAesGcm[1];
        CheckEqStr(r, "json_struct_commands", std::string(v.plaintext, v.plaintext_len), w.Str());
    }

    // Kitchen sink: empty containers, null, int64 min/uint64 max, double, booleans.
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("empty");
        w.BeginObject();
        w.EndObject();
        w.Key("arr");
        w.BeginArray();
        w.EndArray();
        w.Key("nil");
        w.Null();
        w.Key("i");
        w.Int(INT64_MIN);
        w.Key("u");
        w.Uint(UINT64_MAX);
        w.Key("d");
        w.Double(1.5);
        w.Key("t");
        w.Bool(true);
        w.Key("f");
        w.Bool(false);
        w.EndObject();
        CheckEqStr(r, "json_struct_misc", test_vectors::kJsonMiscExpected, w.Str());
    }

    // Nesting-depth guard: one object + 20 nested arrays must trip the limit.
    {
        JsonWriter w;
        w.BeginObject();
        w.Key("a");
        for (int i = 0; i < 20; ++i) {
            w.BeginArray();
        }
        CheckTrue(r, "json_depth_limit", w.HasError(), "depth 21 was not rejected");
    }
}

void TestJsonGuard(TestResult& r) {
    const auto ok = [](const std::string& s) {
        return JsonNestingWithinLimit(s.data(), s.size(), kMaxJsonDepth);
    };
    const auto brackets = [](int n) { return std::string(n, '[') + std::string(n, ']'); };

    CheckTrue(r, "json_guard_depth_30", ok(brackets(30)), "depth 30 rejected");
    CheckTrue(r, "json_guard_depth_32", ok(brackets(32)), "depth 32 rejected");
    CheckTrue(r, "json_guard_depth_33", !ok(brackets(33)), "depth 33 accepted");
    CheckTrue(r, "json_guard_unbalanced", !ok("[["), "unbalanced accepted");
    CheckTrue(r, "json_guard_empty", ok(""), "empty body rejected");
    CheckTrue(r, "json_guard_null", !JsonNestingWithinLimit(nullptr, 0, kMaxJsonDepth),
              "null body accepted");
    // Brackets inside a string value never count toward depth.
    CheckTrue(r, "json_guard_string_brackets", ok("{\"a\":\"[[[[\"}"),
              "string brackets counted as nesting");
    // An escaped quote keeps the string open: the deep run stays in-string.
    CheckTrue(r, "json_guard_escaped_quote", ok("{\"a\":\"\\\"" + brackets(40) + "\"}"),
              "escaped quote closed the string");
    // An escaped backslash must NOT swallow the real closing quote.
    CheckTrue(r, "json_guard_escaped_backslash", ok("{\"a\":\"x\\\\\",\"y\":[[]]}"),
              "backslash escaped the closing quote");
}

void TestBase64(TestResult& r) {
    for (const auto& v : test_vectors::kBase64Encode) {
        std::string got;
        const bool ok = Base64Encode(reinterpret_cast<const uint8_t*>(v.data), v.len, got);
        CheckEqStr(r, "base64_encode", v.expected, ok ? got : "<error logged above>");
    }

    for (const auto& v : test_vectors::kBase64Decode) {
        std::vector<uint8_t> got;
        const bool ok = Base64Decode(std::string(v.input), got);
        const bool eq =
            ok && got.size() == v.expected_len &&
            (v.expected_len == 0 || std::memcmp(got.data(), v.expected, v.expected_len) == 0);
        const std::string detail =
            ok ? "got " + HexForLog(got.data(), got.size()) + " expected " +
                     HexForLog(reinterpret_cast<const uint8_t*>(v.expected), v.expected_len)
               : "decode returned false";
        CheckTrue(r, "base64_decode", eq, detail);
    }

    for (const char* const bad : test_vectors::kBase64DecodeInvalid) {
        std::vector<uint8_t> got;
        const bool rejected = !Base64Decode(std::string(bad), got);
        CheckTrue(r, "base64_decode_invalid", rejected,
                  std::string("accepted invalid input \"") + bad + "\"");
    }
}

constexpr int kUuidSamples = 16;

void TestUuid(TestResult& r) {
    std::string samples[kUuidSamples];
    for (auto& u : samples) {
        u = Uuid4();
        bool ok = u.size() == 36;
        for (size_t i = 0; ok && i < u.size(); ++i) {
            const char ch = u[i];
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                ok = (ch == '-');
            } else {
                ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            }
        }
        ok = ok && u[14] == '4';  // version nibble
        ok = ok && (u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');  // variant
        CheckTrue(r, "uuid_format", ok, u);
    }
    // 16 random v4 UUIDs must all be distinct (RNG health probe).
    bool distinct = true;
    for (int i = 0; i < kUuidSamples; ++i) {
        for (int j = i + 1; j < kUuidSamples; ++j) {
            if (samples[i] == samples[j]) {
                distinct = false;
            }
        }
    }
    CheckTrue(r, "uuid_distinct", distinct, "duplicate UUIDs from the hardware RNG");
}

}  // namespace

bool RunSelfTest() {
    TestResult r;
    TestMd5(r);
    TestHmacSha256(r);
    TestSecretGenerate(r);
    TestAesGcm(r);
    TestAtRestCrypto(r);
    TestJsonWriter(r);
    TestJsonGuard(r);
    TestBase64(r);
    TestUuid(r);

    if (r.passed == r.total) {
        ESP_LOGI(TAG, "SELFTEST PASS (%d checks)", r.total);
        return true;
    }
    ESP_LOGE(TAG, "SELFTEST FAIL (%d/%d)", r.passed, r.total);
    return false;
}

}  // namespace tuya_crypto
