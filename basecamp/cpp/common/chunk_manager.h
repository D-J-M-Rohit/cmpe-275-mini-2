#pragma once
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <condition_variable>
#include <chrono>
#include "cpp/basecamp.pb.h"

constexpr int CHUNK_SIZE = 100;  // KV pairs per chunk
constexpr int MAX_CHUNK_AGE_SEC = 60;  // evict after 60s

struct ChunkState {
  std::string request_id;
  std::string cancel_token;
  bool cancelled = false;
  std::vector<basecamp::Chunk> chunks;  // assembled chunks from both teams
  bool complete = false;
  int64_t total_compute_ms = 0;
  std::chrono::steady_clock::time_point created_at;
  
  ChunkState() : created_at(std::chrono::steady_clock::now()) {}
};

class ChunkManager {
public:
  ChunkManager() = default;
  
  // Initialize new request, returns first chunk (may block until available)
  basecamp::Chunk InitRequest(const basecamp::InitRequest& req);
  
  // Get chunk by index (blocks until available or cancelled)
  basecamp::Chunk GetChunk(const std::string& req_id, int chunk_idx);
  
  // Cancel request
  bool Cancel(const std::string& req_id, const std::string& token);
  
  // Store chunk from downstream (B/E)
  void StoreChunk(const basecamp::Chunk& chunk);
  
  // Check if request is cancelled
  bool IsCancelled(const std::string& req_id) const;
  
  // Cleanup old requests
  void EvictExpired();

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, ChunkState> states_;
  
  void MergeChunks(const std::string& req_id);
  ChunkState* GetOrCreateState(const std::string& req_id);
};
