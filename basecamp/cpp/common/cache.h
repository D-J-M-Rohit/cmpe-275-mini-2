#pragma once
#include <chrono>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>

template <typename V> class LRUCache {
public:
  LRUCache(size_t capacity, int ttl_seconds)
      : capacity_(capacity), ttl_seconds_(ttl_seconds) {}

  // Get value if exists and not expired
  std::optional<V> Get(const std::string &key) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
      return std::nullopt;
    }

    auto &entry = it->second;

    // Check TTL
    auto now = std::chrono::steady_clock::now();
    auto age =
        std::chrono::duration_cast<std::chrono::seconds>(now - entry.timestamp)
            .count();

    if (age > ttl_seconds_) {
      // Expired
      lru_list_.erase(entry.lru_it);
      cache_.erase(it);
      return std::nullopt;
    }

    // Move to front (MRU)
    lru_list_.splice(lru_list_.begin(), lru_list_, entry.lru_it);

    return entry.value;
  }

  // Put value
  void Put(const std::string &key, const V &value) {
    std::lock_guard<std::mutex> lock(mu_);

    auto it = cache_.find(key);
    if (it != cache_.end()) {
      // Update existing
      it->second.value = value;
      it->second.timestamp = std::chrono::steady_clock::now();
      lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_it);
      return;
    }

    // Evict if at capacity
    if (cache_.size() >= capacity_) {
      Evict();
    }

    // Insert new
    lru_list_.push_front(key);
    Entry entry;
    entry.value = value;
    entry.timestamp = std::chrono::steady_clock::now();
    entry.lru_it = lru_list_.begin();
    cache_[key] = entry;
  }

  // Evict expired entries
  void EvictExpired() {
    std::lock_guard<std::mutex> lock(mu_);

    auto now = std::chrono::steady_clock::now();
    auto it = cache_.begin();
    while (it != cache_.end()) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(
                     now - it->second.timestamp)
                     .count();

      if (age > ttl_seconds_) {
        lru_list_.erase(it->second.lru_it);
        it = cache_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Clear all
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    cache_.clear();
    lru_list_.clear();
  }

  // Get stats
  size_t Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cache_.size();
  }

private:
  struct Entry {
    V value;
    std::chrono::steady_clock::time_point timestamp;
    typename std::list<std::string>::iterator lru_it;
  };

  const size_t capacity_;
  const int ttl_seconds_;
  mutable std::mutex mu_;
  std::map<std::string, Entry> cache_;
  std::list<std::string> lru_list_; // MRU at front

  void Evict() {
    if (lru_list_.empty())
      return;

    // Evict LRU (back of list)
    const std::string &key = lru_list_.back();
    cache_.erase(key);
    lru_list_.pop_back();
  }
};

// Cache key builder for KV queries
inline std::string MakeCacheKey(const std::string &req_type, int key_min,
                                int key_max, int chunk_idx) {
  return req_type + ":" + std::to_string(key_min) + "-" +
         std::to_string(key_max) + ":" + std::to_string(chunk_idx);
}
