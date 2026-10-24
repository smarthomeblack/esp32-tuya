#!/usr/bin/env python3
"""Generate golden byte-level + request-level test vectors for tuya_crypto
and tuya_cloud (ground truth: the Tuya Device Sharing SDK Python code;
formulas documented in the generated headers). Run: python scripts/gen_test_vectors.py
Requires `cryptography` (the only third-party dep): pip install -r scripts/requirements.txt"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
from pathlib import Path
from urllib.parse import quote

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
OUT_PATH = PROJECT_ROOT / "components" / "tuya_crypto" / "src" / "test_vectors.h"
PIPELINE_OUT_PATH = PROJECT_ROOT / "components" / "tuya_cloud" / "src" / "pipeline_vectors.h"

# --- Fixed protocol constants (Tuya Device Sharing SDK reference) ---
RID = "3f2504e0-4f89-11d3-9a0c-0305e82c3301"
REFRESH_TOKEN = "sample-refresh-token-0123456789abcdef"
HASH_KEY = hashlib.md5((RID + REFRESH_TOKEN).encode("utf-8")).hexdigest()
SIGN_INPUT = (
    "X-appKey=HA_3y9q4ak7g4ephrvke"
    f"||X-requestId={RID}"
    "||X-time=1731838024000"
)

# AES vectors: fixed 16-hex-char secret (used as 16 raw ASCII bytes = AES-128
# key) and fixed 12-char nonce from the Tuya alphabet.
AES_SECRET = "0123456789abcdef"
AES_NONCE = "ABCDEFGHJKMN"

# Non-ASCII vector input: Latin-Extended letters + astral (U+1F6CB), pinned
# byte-for-byte by the golden vectors in test_vectors.h (\u escapes: ASCII source).
NAME_UNICODE = "\u0110\u00e8n ph\u00f2ng kh\u00e1ch \U0001F6CB"


# --- Reference implementations (mirror the Tuya Device Sharing SDK exactly) ---
def md5_hex(s: str) -> str:
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def hmac_sha256_hex(key: str, msg: str) -> str:
    return hmac.new(key.encode("utf-8"), msg.encode("utf-8"), hashlib.sha256).hexdigest()


def dumps_compact(obj) -> str:
    """The SDK's _form_to_json: json.dumps(separators=(',', ':'))."""
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=True)


def aes_gcm_pack(secret: str, nonce: str, plaintext: bytes) -> str:
    """The SDK's _aes_gcm_encrypt, but with an explicit fixed nonce."""
    key = secret.encode("utf-8")          # 16 ASCII hex chars -> AES-128 key
    iv = nonce.encode("utf-8")             # 12 ASCII bytes
    ct = AESGCM(key).encrypt(iv, plaintext, None)  # ct || 16-byte tag
    enc = base64.b64encode(iv) + base64.b64encode(ct)
    return enc.decode("ascii")


# --- C string literal escaping ---
def c_escape(data: bytes) -> str:
    """C string literal body. Non-printable/non-ASCII bytes use 3-digit
    octal escapes (never absorb a following hex digit, unlike \\xNN)."""
    parts = []
    for b in data:
        if b == 0x22:  # '"'
            parts.append('\\"')
        elif b == 0x5C:  # '\\'
            parts.append("\\\\")
        elif 0x20 <= b <= 0x7E:
            parts.append(chr(b))
        else:
            parts.append("\\%03o" % b)
    return "".join(parts)


def c_lit(data) -> str:
    """C string literal for str (UTF-8) or bytes input."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    return '"' + c_escape(data) + '"'


def c_lit_len(data) -> str:
    """C string literal + explicit length ('"abc", 3') — NUL-safe."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    return f'{c_lit(data)}, {len(data)}'


# --- Vector definitions ---
MD5_CASES = [
    ("", "empty input (RFC 1321)"),
    ("abc", "RFC 1321 'abc'"),
    (RID + REFRESH_TOKEN, "rid + refresh_token (hash_key derivation)"),
]

HMAC_CASES = [
    (RID, HASH_KEY, "_secret_generating: key=rid, msg=hash_key"),
    (HASH_KEY, SIGN_INPUT, "_restful_sign: key=hash_key, msg=sign string"),
]

SECRET_CASES = [
    (RID, HASH_KEY, "secret = first 16 hex chars of HMAC(rid, hash_key)"),
]

AES_CASES = [
    (dumps_compact({"homeId": "123456"}),
     "devices-by-home query plaintext"),
    (dumps_compact({"commands": [{"code": "switch_led", "value": True}]}),
     "command body"),
    (dumps_compact({"name": NAME_UNICODE}),
     "non-ASCII name: ensure_ascii escapes + astral surrogate pair"),
    ("", "empty plaintext (ciphertext is tag-only)"),
]

JSON_ESCAPE_CASES = [
    ("\u0110\u00e8n", "Latin-1 supplement code points"),
    ("emoji 🛋️", "astral + variation selector (two surrogate pairs)"),
    ("a\"b\\c\nd\te\x00f\x01g\x1fh\x7f",
     "quote, backslash, newline, tab, NUL, C0 controls, DEL"),
    ("", "empty string"),
    ("slash/a/b", "forward slash is NOT escaped"),
]

JSON_NAME_CASES = [
    (NAME_UNICODE, "protocol-style name value"),
]

JSON_MISC_EXPECTED = dumps_compact({
    "empty": {},
    "arr": [],
    "nil": None,
    "i": -9223372036854775808,
    "u": 18446744073709551615,
    "d": 1.5,
    "t": True,
    "f": False,
})

B64_ENCODE_CASES = [
    (b"", "empty input"),
    (bytes(range(16)), "bytes 0..15 (includes NUL)"),
    (b"foobar", "classic 6-byte input"),
]

B64_DECODE_CASES = [
    ("", b""),
    ("Zm9vYmFy", b"foobar"),
    ("AAECAwQFBgcICQoLDA0ODw==", bytes(range(16))),  # roundtrip of the 0..15 vector
    ("QUJDREVGR0hKS01O", b"ABCDEFGHJKMN"),  # 12 bytes, no padding
]

B64_DECODE_INVALID = ["A", "!!!!", "abcde", "AB=C", "===="]


# --- Request pipeline reference implementation (SDK __request stage) ---
CLIENT_ID = "HA_3y9q4ak7g4ephrvke"  # Tuya login client id


def restful_sign(hash_key: str, query_encdata: str, body_encdata: str,
                 headers: dict) -> str:
    """The SDK's _restful_sign, mirrored exactly."""
    sign_headers = ["X-appKey", "X-requestId", "X-sid", "X-time", "X-token"]
    header_sign_str = ""
    for item in sign_headers:
        val = headers.get(item, "")
        if val != "":
            header_sign_str += item + "=" + val + "||"
    sign_str = header_sign_str[:-2]

    if query_encdata is not None and query_encdata != "":
        sign_str += query_encdata
    if body_encdata is not None and body_encdata != "":
        sign_str += body_encdata

    return hmac_sha256_hex(hash_key, sign_str)


def percent_encode(s: str) -> str:
    """RFC 3986: every byte except [A-Za-z0-9-_.~] as %XX (space -> %20;
    encdata never contains spaces, so this agrees with the compact-JSON
    sign rule on every character that occurs)."""
    return quote(s, safe="")


def build_signed_request(endpoint: str, path: str, rid: str, access_token: str,
                          refresh_token: str, time_ms: int, nonce: str,
                          params_json: str, body_json: str) -> dict:
    """The SDK's __request signing stage, explicit fixed nonce ->
    the wire-level request (url, body, the six X-* headers)."""
    hash_key = md5_hex(rid + refresh_token)
    secret = hmac_sha256_hex(rid, hash_key)[:16]

    query_encdata = ""
    if params_json:
        query_encdata = aes_gcm_pack(secret, nonce, params_json.encode("utf-8"))
    body_encdata = ""
    if body_json:
        body_encdata = aes_gcm_pack(secret, nonce, body_json.encode("utf-8"))

    headers = {
        "X-appKey": CLIENT_ID,
        "X-requestId": rid,
        "X-sid": "",          # always empty (still sent, sign-skipped)
        "X-time": str(time_ms),
    }
    if access_token:
        headers["X-token"] = access_token
    sign = restful_sign(hash_key, query_encdata, body_encdata, headers)

    url = endpoint + path
    if query_encdata:
        url += "?encdata=" + percent_encode(query_encdata)
    body = ""
    if body_json:
        body = dumps_compact({"encdata": body_encdata})

    return {
        "url": url,
        "body": body,
        "app_key": headers["X-appKey"],
        "request_id": headers["X-requestId"],
        "sid": headers["X-sid"],
        "time": headers["X-time"],
        "token": headers.get("X-token", ""),
        "sign": sign,
    }


PIPELINE_CASES = [
    {
        "name": "case_a_homes_query",
        "desc": "GET with encrypted query params: "
                "sign covers headers + query encdata, no body",
        "endpoint": "https://apigw.example.com",
        "path": "/v1.0/m/life/users/homes",
        "rid": RID,
        "access_token": "sample-access-token-abcdef",
        "refresh_token": REFRESH_TOKEN,
        "time_ms": 1731838024000,
        "nonce": "ABCDEFGHJKMN",
        "params_json": dumps_compact({"homeId": "123456"}),
        "body_json": "",
    },
    {
        "name": "case_b_commands_body",
        "desc": "POST with encrypted body: sign covers "
                "headers + body encdata, no query string",
        "endpoint": "https://apigw.example.com",
        "path": "/v1.1/m/thing/dev123/commands",
        "rid": RID,
        "access_token": "sample-access-token-abcdef",
        "refresh_token": REFRESH_TOKEN,
        "time_ms": 1731838025000,
        "nonce": "ABCDEFGHJKLM",
        "params_json": "",
        "body_json": dumps_compact({"commands": [{"code": "switch_led", "value": True}]}),
    },
    {
        "name": "case_c_no_access_token",
        "desc": "GET with empty access_token: X-token skipped in BOTH the sign "
                "string and the sent headers — covers the token "
                "refresh path shape (the refresh call sends the OLD token; a "
                "fully empty token exercises the conditional-header logic)",
        "endpoint": "https://apigw.example.com",
        "path": "/v1.0/m/token/sample-refresh-token-0123456789abcdef",
        "rid": RID,
        "access_token": "",
        "refresh_token": REFRESH_TOKEN,
        "time_ms": 1731838026000,
        "nonce": "ABCDEFGHJKNP",
        "params_json": "",
        "body_json": "",
    },
]

PERCENT_ENCODE_CASES = [
    ("+/= a", "%2B%2F%3D%20a",
     "base64 specials and a space: + / = escaped, space as %20 (RFC 3986)"),
    ("AZaz09-_.~", "AZaz09-_.~",
     "RFC 3986 unreserved characters must stay unescaped"),
]


# --- Header generation ---
def generate() -> str:
    L: list[str] = []
    a = L.append

    a("// GENERATED FILE — DO NOT EDIT. Regenerate: python scripts/gen_test_vectors.py")
    a("// Golden byte-level vectors for the tuya_crypto self-test: MD5, HMAC-SHA256,")
    a("// secret, AES-128-GCM encdata, JSON escaping, base64 (Tuya SDK ground truth).")
    a("")
    a("#pragma once")
    a("")
    a("#include <cstddef>")
    a("")
    a("namespace tuya_crypto::test_vectors {")
    a("")

    # ---- MD5 ----
    a("// ---- MD5: input -> 32-char lowercase hex digest (hash_key derivation) ----")
    a("struct Md5Vector { const char* input; size_t input_len; const char* expected_hex; };")
    a("inline constexpr Md5Vector kMd5[] = {")
    for text, desc in MD5_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit_len(text)}, {c_lit(md5_hex(text))}}},")
    a("};")
    a("")

    # ---- HMAC-SHA256 ----
    a("// ---- HMAC-SHA256: (key, msg) -> 64-char lowercase hex digest ----")
    a("struct HmacVector { const char* key; size_t key_len; const char* msg;")
    a("                     size_t msg_len; const char* expected_hex; };")
    a("inline constexpr HmacVector kHmacSha256[] = {")
    for key, msg, desc in HMAC_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit_len(key)}, {c_lit_len(msg)}, {c_lit(hmac_sha256_hex(key, msg))}}},")
    a("};")
    a("")

    # ---- secret generation ----
    a("// ---- secret derivation: HMAC-SHA256(key=rid, msg=hash_key) hexdigest[:16] ----")
    a("struct SecretVector { const char* rid; const char* hash_key;")
    a("                       const char* expected_secret; };")
    a("inline constexpr SecretVector kSecretGenerate[] = {")
    for rid, hash_key, desc in SECRET_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit(rid)}, {c_lit(hash_key)},"
          f" {c_lit(hmac_sha256_hex(rid, hash_key)[:16])}}},")
    a("};")
    a("")

    # ---- AES-128-GCM ----
    a("// ---- AES-128-GCM pack: secret (16 hex chars = 16 ASCII key bytes), nonce ----")
    a("// ---- (12 chars from the Tuya alphabet), plaintext -> encdata ----")
    a("struct AesGcmVector { const char* secret; const char* nonce;")
    a("                       const char* plaintext; size_t plaintext_len;")
    a("                       const char* encdata; };")
    a("inline constexpr AesGcmVector kAesGcm[] = {")
    for plaintext, desc in AES_CASES:
        pt_bytes = plaintext.encode("utf-8")
        enc = aes_gcm_pack(AES_SECRET, AES_NONCE, pt_bytes)
        # Sanity: the two-string concat equals one base64 blob of nonce||ct
        # (valid because 12 % 3 == 0).
        blob = AES_NONCE.encode("utf-8") + AESGCM(AES_SECRET.encode()).encrypt(
            AES_NONCE.encode(), pt_bytes, None)
        assert base64.b64encode(blob).decode("ascii") == enc, "encdata concat invariant violated"
        a(f"    // {desc}")
        a(f"    {{{c_lit(AES_SECRET)}, {c_lit(AES_NONCE)},"
          f" {c_lit_len(pt_bytes)}, {c_lit(enc)}}},")
    a("};")
    a("")

    # ---- JSON escaping ----
    a("// ---- JSON string escaping: input (raw UTF-8) -> json.dumps({\"k\": input}) ----")
    a("struct JsonEscapeVector { const char* input; size_t input_len;")
    a("                           const char* expected_json; };")
    a("inline constexpr JsonEscapeVector kJsonEscape[] = {")
    for text, desc in JSON_ESCAPE_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit_len(text)}, {c_lit(dumps_compact({'k': text}))}}},")
    a("};")
    a("")

    a("// ---- JSON object build: {\"name\": input} (unicode name value) ----")
    a("struct JsonNameVector { const char* input; size_t input_len;")
    a("                         const char* expected_json; };")
    a("inline constexpr JsonNameVector kJsonName[] = {")
    for text, desc in JSON_NAME_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit_len(text)}, {c_lit(dumps_compact({'name': text}))}}},")
    a("};")
    a("")

    a("// ---- JSON structural kitchen sink: containers, null, int min/max, double, ----")
    a("// ---- booleans (built in code by self_test.cc; expected from json.dumps) ----")
    a(f"inline constexpr const char* kJsonMiscExpected = {c_lit(JSON_MISC_EXPECTED)};")
    a("")

    # ---- base64 ----
    a("// ---- base64 encode: bytes -> standard base64 with padding ----")
    a("struct Base64EncodeVector { const char* data; size_t len;")
    a("                             const char* expected; };")
    a("inline constexpr Base64EncodeVector kBase64Encode[] = {")
    for data, desc in B64_ENCODE_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit_len(data)}, {c_lit(base64.b64encode(data))}}},")
    a("};")
    a("")

    a("// ---- base64 decode: strict decode must reproduce the exact bytes ----")
    a("struct Base64DecodeVector { const char* input; const char* expected;")
    a("                             size_t expected_len; };")
    a("inline constexpr Base64DecodeVector kBase64Decode[] = {")
    for text, expected in B64_DECODE_CASES:
        a(f"    {{{c_lit(text)}, {c_lit_len(expected)}}},")
    a("};")
    a("")

    a("// ---- base64 decode: inputs that must be rejected (strict validation) ----")
    a("inline constexpr const char* kBase64DecodeInvalid[] = {")
    for text in B64_DECODE_INVALID:
        a(f"    {c_lit(text)},")
    a("};")
    a("")

    a("}  // namespace tuya_crypto::test_vectors")
    a("")

    return "\n".join(L)


def generate_pipeline() -> str:
    L: list[str] = []
    a = L.append

    a("// GENERATED FILE — DO NOT EDIT. Regenerate: python scripts/gen_test_vectors.py")
    a("// Golden request-level vectors for tuya_cloud: signed requests + PercentEncode.")
    a("// rid/refresh_token shared with tuya_crypto/src/test_vectors.h (cross-checked).")
    a("")
    a("#pragma once")
    a("")
    a("#include <cstdint>")
    a("")
    a("namespace tuya_cloud::pipeline_vectors {")
    a("")
    a("// ---- Full signed request: inputs -> url, body, six header values, sign ----")
    a("struct PipelineVector {")
    a("    const char* name;")
    a("    const char* desc;")
    a("    // Inputs")
    a("    const char* endpoint;")
    a("    const char* path;")
    a("    const char* rid;")
    a("    const char* access_token;")
    a("    const char* refresh_token;")
    a("    int64_t time_ms;")
    a("    const char* nonce;")
    a("    const char* params_json;  // compact JSON plaintext, \"\" when absent")
    a("    const char* body_json;    // compact JSON plaintext, \"\" when absent")
    a("    // Expected outputs")
    a("    const char* url;")
    a("    const char* body;         // wire request body, \"\" when absent")
    a("    const char* app_key;      // X-appKey")
    a("    const char* request_id;   // X-requestId")
    a("    const char* sid;          // X-sid (always \"\", still sent, sign-skipped)")
    a("    const char* time;         // X-time (milliseconds, decimal string)")
    a("    const char* token;        // X-token (\"\" when no access token)")
    a("    const char* sign;         // X-sign")
    a("};")
    a("inline constexpr PipelineVector kPipeline[] = {")
    for case in PIPELINE_CASES:
        out = build_signed_request(
            case["endpoint"], case["path"], case["rid"], case["access_token"],
            case["refresh_token"], case["time_ms"], case["nonce"],
            case["params_json"], case["body_json"])
        a(f"    // {case['desc']}")
        a(f"    {{{c_lit(case['name'])}, {c_lit(case['desc'])},")
        a(f"     {c_lit(case['endpoint'])}, {c_lit(case['path'])},")
        a(f"     {c_lit(case['rid'])}, {c_lit(case['access_token'])},")
        a(f"     {c_lit(case['refresh_token'])}, {case['time_ms']}, {c_lit(case['nonce'])},")
        a(f"     {c_lit(case['params_json'])}, {c_lit(case['body_json'])},")
        a(f"     {c_lit(out['url'])}, {c_lit(out['body'])},")
        a(f"     {c_lit(out['app_key'])}, {c_lit(out['request_id'])}, {c_lit(out['sid'])},")
        a(f"     {c_lit(out['time'])}, {c_lit(out['token'])}, {c_lit(out['sign'])}}},")
    a("};")
    a("")

    a("// ---- PercentEncode: input -> RFC 3986 encoding (space -> %20) ----")
    a("struct PercentEncodeVector { const char* input; const char* expected; };")
    a("inline constexpr PercentEncodeVector kPercentEncode[] = {")
    for text, expected, desc in PERCENT_ENCODE_CASES:
        a(f"    // {desc}")
        a(f"    {{{c_lit(text)}, {c_lit(expected)}}},")
    a("};")
    a("")

    a("}  // namespace tuya_cloud::pipeline_vectors")
    a("")

    return "\n".join(L)


def main() -> None:
    # Self-validate the hand-written decode pairings before emitting anything.
    for text, expected in B64_DECODE_CASES:
        assert base64.b64decode(text) == expected, f"bad decode case: {text!r}"
    assert json.loads(JSON_MISC_EXPECTED)["i"] == -9223372036854775808

    # Self-validate the pipeline cases: expected URL/body shape must hold.
    for case in PIPELINE_CASES:
        out = build_signed_request(
            case["endpoint"], case["path"], case["rid"], case["access_token"],
            case["refresh_token"], case["time_ms"], case["nonce"],
            case["params_json"], case["body_json"])
        assert out["url"].startswith(case["endpoint"] + case["path"]), case["name"]
        if case["params_json"]:
            assert "?encdata=" in out["url"], case["name"]
        else:
            assert "?encdata=" not in out["url"], case["name"]
        if case["body_json"]:
            assert out["body"].startswith('{"encdata":"'), case["name"]
        else:
            assert out["body"] == "", case["name"]

    content = generate()
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    # LF line endings regardless of platform -> byte-identical output.
    OUT_PATH.write_text(content, encoding="utf-8", newline="\n")

    pipeline_content = generate_pipeline()
    PIPELINE_OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    PIPELINE_OUT_PATH.write_text(pipeline_content, encoding="utf-8", newline="\n")

    counts = {
        "md5": len(MD5_CASES),
        "hmac_sha256": len(HMAC_CASES),
        "secret_generate": len(SECRET_CASES),
        "aes_gcm": len(AES_CASES),
        "json_escape": len(JSON_ESCAPE_CASES),
        "json_name": len(JSON_NAME_CASES),
        "json_misc": 1,
        "base64_encode": len(B64_ENCODE_CASES),
        "base64_decode": len(B64_DECODE_CASES),
        "base64_decode_invalid": len(B64_DECODE_INVALID),
    }
    total = sum(counts.values())
    print(f"Generated {OUT_PATH}")
    for name, n in counts.items():
        print(f"  {name:24s} {n} vector(s)")
    print(f"  {'TOTAL':24s} {total} vector entries")

    pipeline_counts = {
        "pipeline": len(PIPELINE_CASES),
        "percent_encode": len(PERCENT_ENCODE_CASES),
    }
    print(f"Generated {PIPELINE_OUT_PATH}")
    for name, n in pipeline_counts.items():
        print(f"  {name:24s} {n} vector(s)")

    print("Sanity checks passed:")
    print("  - encdata two-string concat == single-blob base64")
    print(f"  - hash_key = {HASH_KEY}")
    print(f"  - secret   = {hmac_sha256_hex(RID, HASH_KEY)[:16]} (16 hex chars)")


if __name__ == "__main__":
    main()
