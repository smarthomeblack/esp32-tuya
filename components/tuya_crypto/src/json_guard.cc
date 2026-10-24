#include "tuya_crypto/json_guard.h"

namespace tuya_crypto {

bool JsonNestingWithinLimit(const char* body, size_t len, int max_depth) {
    if (body == nullptr) {
        return false;
    }
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < len; ++i) {
        const char c = body[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // the escaped byte cannot close the string
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0 && !in_string;
}

}  // namespace tuya_crypto
