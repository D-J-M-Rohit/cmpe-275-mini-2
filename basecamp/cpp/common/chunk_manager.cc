#include "cpp/common/chunk_manager.h"
#include <iostream>

basecamp::Chunk ChunkManager::InitRequest(const basecamp::InitRequest &req) {
  std::unique_lock<std::mutex> lock(mu_);

  auto *state = GetOrCreateState(req.request_id());
  state->cancel_token = req.cancellation_token();
  state->cancelled = false;

  // Wait for first chunk to arrive (with timeout)
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  cv_.wait_until(lock, deadline,
                 [&]() { return !state->chunks.empty() || state->cancelled; });

  if (state->cancelled) {
    basecamp::Chunk err;
    err.set_request_id(req.request_id());
    err.set_chunk_index(0);
    err.set_is_final(true);
    err.set_total_chunks(0);
    return err;
  }

  if (state->chunks.empty()) {
    // Timeout - return empty chunk
    basecamp::Chunk timeout;
    timeout.set_request_id(req.request_id());
    timeout.set_chunk_index(0);
    timeout.set_is_final(true);
    timeout.set_total_chunks(0);
    return timeout;
  }

  return state->chunks[0];
}

basecamp::Chunk ChunkManager::GetChunk(const std::string &req_id,
                                       int chunk_idx) {
  std::unique_lock<std::mutex> lock(mu_);

  auto it = states_.find(req_id);
  if (it == states_.end()) {
    // Request not found
    basecamp::Chunk err;
    err.set_request_id(req_id);
    err.set_chunk_index(chunk_idx);
    err.set_is_final(true);
    err.set_total_chunks(0);
    return err;
  }

  auto &state = it->second;

  // Wait for chunk to arrive (with timeout)
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  cv_.wait_until(lock, deadline, [&]() {
    return chunk_idx < static_cast<int>(state.chunks.size()) ||
           state.cancelled || state.complete;
  });

  if (state.cancelled) {
    basecamp::Chunk err;
    err.set_request_id(req_id);
    err.set_chunk_index(chunk_idx);
    err.set_is_final(true);
    err.set_total_chunks(0);
    return err;
  }

  if (chunk_idx >= static_cast<int>(state.chunks.size())) {
    // Chunk not available yet or out of range
    basecamp::Chunk err;
    err.set_request_id(req_id);
    err.set_chunk_index(chunk_idx);
    err.set_is_final(true);
    err.set_total_chunks(static_cast<int>(state.chunks.size()));
    return err;
  }

  return state.chunks[chunk_idx];
}

bool ChunkManager::Cancel(const std::string &req_id, const std::string &token) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = states_.find(req_id);
  if (it == states_.end()) {
    return false;
  }

  auto &state = it->second;
  if (state.cancel_token != token) {
    return false; // wrong token
  }

  state.cancelled = true;
  cv_.notify_all();
  return true;
}

void ChunkManager::StoreChunk(const basecamp::Chunk &chunk) {
  std::lock_guard<std::mutex> lock(mu_);

  auto *state = GetOrCreateState(chunk.request_id());

  // Ensure chunks vector is large enough
  if (chunk.chunk_index() >= static_cast<int>(state->chunks.size())) {
    state->chunks.resize(chunk.chunk_index() + 1);
  }

  state->chunks[chunk.chunk_index()] = chunk;
  state->total_compute_ms += chunk.compute_ms();

  if (chunk.is_final()) {
    state->complete = true;
  }

  cv_.notify_all();
}

bool ChunkManager::IsCancelled(const std::string &req_id) const {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = states_.find(req_id);
  if (it == states_.end()) {
    return false;
  }

  return it->second.cancelled;
}

void ChunkManager::EvictExpired() {
  std::lock_guard<std::mutex> lock(mu_);

  auto now = std::chrono::steady_clock::now();
  auto it = states_.begin();
  while (it != states_.end()) {
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                   now - it->second.created_at)
                   .count();

    if (age > MAX_CHUNK_AGE_SEC) {
      std::cerr << "Evicting expired request: " << it->first << "\n";
      it = states_.erase(it);
    } else {
      ++it;
    }
  }
}

void ChunkManager::MergeChunks(const std::string &req_id) {
  // Future: merge chunks from GREEN and PINK teams
  // For now, chunks are stored as-is
}

ChunkState *ChunkManager::GetOrCreateState(const std::string &req_id) {
  auto it = states_.find(req_id);
  if (it == states_.end()) {
    it = states_.emplace(req_id, ChunkState()).first;
    it->second.request_id = req_id;
  }
  return &it->second;
}
