#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "tuya_cloud/credentials.h"

namespace tuya_cloud {

/// Thread-safe token state holder with refresh bookkeeping.
/// The listener runs OUTSIDE the internal lock — it must not call back into TokenManager.
class TokenManager {
public:
    /// Called with the new token after it changed (persistence hook).
    using Listener = std::function<void(const TokenInfo&)>;

    /// Stores the token; fires the listener when any field differs.
    void SetToken(const TokenInfo& token);

    /// Thread-safe copy of the current token.
    TokenInfo GetToken() const;

    /// True when no token is stored at all.
    bool IsEmpty() const;

    /// True iff now_ms >= ExpireMs() - 60000 AND a refresh token exists; an absent token never
    /// triggers.
    bool NeedsRefresh(int64_t now_ms) const;

    /// In-flight refresh guard.
    void SetRefreshing(bool refreshing);
    bool IsRefreshing() const;

    void SetListener(Listener listener);

private:
    /// Refresh margin: last 60 s of validity.
    static constexpr int64_t kRefreshMarginMs = 60000;

    mutable std::mutex mutex_;
    TokenInfo token_;
    bool refreshing_ = false;
    Listener listener_;
};

}  // namespace tuya_cloud
