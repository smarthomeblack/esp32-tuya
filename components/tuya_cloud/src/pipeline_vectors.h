// GENERATED FILE — DO NOT EDIT. Regenerate: python scripts/gen_test_vectors.py
// Golden request-level vectors for tuya_cloud: signed requests + PercentEncode.
// rid/refresh_token shared with tuya_crypto/src/test_vectors.h (cross-checked).

#pragma once

#include <cstdint>

namespace tuya_cloud::pipeline_vectors {

// ---- Full signed request: inputs -> url, body, six header values, sign ----
struct PipelineVector {
    const char* name;
    const char* desc;
    // Inputs
    const char* endpoint;
    const char* path;
    const char* rid;
    const char* access_token;
    const char* refresh_token;
    int64_t time_ms;
    const char* nonce;
    const char* params_json;  // compact JSON plaintext, "" when absent
    const char* body_json;    // compact JSON plaintext, "" when absent
    // Expected outputs
    const char* url;
    const char* body;         // wire request body, "" when absent
    const char* app_key;      // X-appKey
    const char* request_id;   // X-requestId
    const char* sid;          // X-sid (always "", still sent, sign-skipped)
    const char* time;         // X-time (milliseconds, decimal string)
    const char* token;        // X-token ("" when no access token)
    const char* sign;         // X-sign
};
inline constexpr PipelineVector kPipeline[] = {
    // GET with encrypted query params: sign covers headers + query encdata, no body
    {"case_a_homes_query", "GET with encrypted query params: sign covers headers + query encdata, no body",
     "https://apigw.example.com", "/v1.0/m/life/users/homes",
     "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "sample-access-token-abcdef",
     "sample-refresh-token-0123456789abcdef", 1731838024000, "ABCDEFGHJKMN",
     "{\"homeId\":\"123456\"}", "",
     "https://apigw.example.com/v1.0/m/life/users/homes?encdata=QUJDREVGR0hKS01OZlwi0hzW0Pz6P52VAj5iNk6t%2B25wx8wHFvbQgrSdvtFGyCM%3D", "",
     "HA_3y9q4ak7g4ephrvke", "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "",
     "1731838024000", "sample-access-token-abcdef", "85ba50e24f73b58a6dee62d432be90143e29687bfce80d94420b8c1ba62eaae3"},
    // POST with encrypted body: sign covers headers + body encdata, no query string
    {"case_b_commands_body", "POST with encrypted body: sign covers headers + body encdata, no query string",
     "https://apigw.example.com", "/v1.1/m/thing/dev123/commands",
     "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "sample-access-token-abcdef",
     "sample-refresh-token-0123456789abcdef", 1731838025000, "ABCDEFGHJKLM",
     "", "{\"commands\":[{\"code\":\"switch_led\",\"value\":true}]}",
     "https://apigw.example.com/v1.1/m/thing/dev123/commands", "{\"encdata\":\"QUJDREVGR0hKS0xNe6nOV7XPOsT3XojmTBZS9LgcFvMWJL6PB1WeAKnegn0PsQKggtzy9VFJkeQVS6um8fROWyrZf4cYIta0ERPPkmg=\"}",
     "HA_3y9q4ak7g4ephrvke", "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "",
     "1731838025000", "sample-access-token-abcdef", "d45f2dc84ed51e061270a327e294b943221355f29fbd8c079461e53aacce7d6e"},
    // GET with empty access_token: X-token skipped in BOTH the sign string and the sent headers — covers the token refresh path shape (the refresh call sends the OLD token; a fully empty token exercises the conditional-header logic)
    {"case_c_no_access_token", "GET with empty access_token: X-token skipped in BOTH the sign string and the sent headers \342\200\224 covers the token refresh path shape (the refresh call sends the OLD token; a fully empty token exercises the conditional-header logic)",
     "https://apigw.example.com", "/v1.0/m/token/sample-refresh-token-0123456789abcdef",
     "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "",
     "sample-refresh-token-0123456789abcdef", 1731838026000, "ABCDEFGHJKNP",
     "", "",
     "https://apigw.example.com/v1.0/m/token/sample-refresh-token-0123456789abcdef", "",
     "HA_3y9q4ak7g4ephrvke", "3f2504e0-4f89-11d3-9a0c-0305e82c3301", "",
     "1731838026000", "", "695c7485934f7ba3fbda0b6ef9437239f0bfc828a9a89ffdcaa4f25b6301f1a0"},
};

// ---- PercentEncode: input -> RFC 3986 encoding (space -> %20) ----
struct PercentEncodeVector { const char* input; const char* expected; };
inline constexpr PercentEncodeVector kPercentEncode[] = {
    // base64 specials and a space: + / = escaped, space as %20 (RFC 3986)
    {"+/= a", "%2B%2F%3D%20a"},
    // RFC 3986 unreserved characters must stay unescaped
    {"AZaz09-_.~", "AZaz09-_.~"},
};

}  // namespace tuya_cloud::pipeline_vectors
