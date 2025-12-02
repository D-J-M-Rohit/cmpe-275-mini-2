#pragma once

#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

#include <grpcpp/grpcpp.h>

// Retry configuration
struct RetryConfig {
  int max_retries = 3;
  int initial_backoff_ms = 100;
  int max_backoff_ms = 1000;
  double backoff_multiplier = 2.0;
};

// Helper to check if status is retriable
inline bool IsRetriable(const grpc::Status &status) {
  switch (status.error_code()) {
  case grpc::StatusCode::UNAVAILABLE:
  case grpc::StatusCode::DEADLINE_EXCEEDED:
  case grpc::StatusCode::RESOURCE_EXHAUSTED:
    return true;
  default:
    return false;
  }
}

// Template function to retry gRPC calls
// Func should be a lambda that returns grpc::Status
template <typename Func>
grpc::Status CallWithRetries(Func func,
                             const RetryConfig &config = RetryConfig(),
                             const std::string &context_info = "") {
  grpc::Status status;
  int backoff = config.initial_backoff_ms;

  for (int i = 0; i <= config.max_retries; ++i) {
    status = func();

    if (status.ok()) {
      return status;
    }

    if (!IsRetriable(status)) {
      return status; // Fatal error, don't retry
    }

    if (i < config.max_retries) {
      if (!context_info.empty()) {
        std::cerr << "WARN: RPC failed (" << context_info
                  << "): " << status.error_message() << ". Retrying in "
                  << backoff << "ms (" << (i + 1) << "/" << config.max_retries
                  << ")\n";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(backoff));
      backoff = std::min(static_cast<int>(backoff * config.backoff_multiplier),
                         config.max_backoff_ms);
    }
  }

  if (!context_info.empty()) {
    std::cerr << "ERROR: RPC failed permanently (" << context_info << ") after "
              << config.max_retries << " retries: " << status.error_message()
              << "\n";
  }
  return status;
}
