#include "tuya_cloud/token_manager.h"

namespace tuya_cloud {

void TokenManager::SetToken(const TokenInfo& token) {
    Listener listener_to_fire;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool changed = token_.t != token.t || token_.expire_time != token.expire_time ||
                             token_.uid != token.uid || token_.access_token != token.access_token ||
                             token_.refresh_token != token.refresh_token;
        token_ = token;
        if (changed && listener_) {
            listener_to_fire = listener_;
        }
    }
    // Invoke outside the lock — the listener must be free to call back.
    if (listener_to_fire) {
        listener_to_fire(token);
    }
}

TokenInfo TokenManager::GetToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_;
}

bool TokenManager::IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return token_.access_token.empty() && token_.refresh_token.empty();
}

bool TokenManager::NeedsRefresh(int64_t now_ms) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (token_.refresh_token.empty()) {
        return false;
    }
    return now_ms >= token_.ExpireMs() - kRefreshMarginMs;
}

void TokenManager::SetRefreshing(bool refreshing) {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshing_ = refreshing;
}

bool TokenManager::IsRefreshing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return refreshing_;
}

void TokenManager::SetListener(Listener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

}  // namespace tuya_cloud
