#include "cpp/common/cancellation.h"
#include <iostream>

// Global instance
CancellationRegistry g_cancel_registry;

void CancellationRegistry::Register(const std::string &req_id,
                                    const std::string &token) {
  std::lock_guard<std::mutex> lock(mu_);

  auto now = std::chrono::system_clock::now();
  auto timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  tokens_[req_id] = TokenState{token, false, timestamp};
}

bool CancellationRegistry::Cancel(const std::string &req_id,
                                  const std::string &token) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = tokens_.find(req_id);
  if (it == tokens_.end()) {
    return false;
  }

  if (it->second.token != token) {
    std::cerr << "Cancel token mismatch for " << req_id << "\n";
    return false;
  }

  it->second.cancelled = true;
  std::cerr << "Cancelled request: " << req_id << "\n";
  return true;
}

bool CancellationRegistry::IsCancelled(const std::string &req_id) const {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = tokens_.find(req_id);
  if (it == tokens_.end()) {
    return false;
  }

  return it->second.cancelled;
}

void CancellationRegistry::Cleanup(int max_age_seconds) {
  std::lock_guard<std::mutex> lock(mu_);

  auto now = std::chrono::system_clock::now();
  auto now_ts =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();

  auto it = tokens_.begin();
  while (it != tokens_.end()) {
    if (now_ts - it->second.timestamp > max_age_seconds) {
      std::cerr << "Cleaning up old token: " << it->first << "\n";
      it = tokens_.erase(it);
    } else {
      ++it;
    }
  }
}
