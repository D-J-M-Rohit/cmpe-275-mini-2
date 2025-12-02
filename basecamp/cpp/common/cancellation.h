#pragma once
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

class CancellationRegistry {
public:
  CancellationRegistry() = default;

  // Register new cancellation token
  void Register(const std::string &req_id, const std::string &token);

  // Mark request as cancelled
  bool Cancel(const std::string &req_id, const std::string &token);

  // Check if cancelled
  bool IsCancelled(const std::string &req_id) const;

  // Cleanup old tokens (called periodically)
  void Cleanup(int max_age_seconds = 300);

private:
  mutable std::mutex mu_;
  struct TokenState {
    std::string token;
    bool cancelled;
    int64_t timestamp;
  };
  std::map<std::string, TokenState> tokens_;
};

// Global registry (one per process)
extern CancellationRegistry g_cancel_registry;
