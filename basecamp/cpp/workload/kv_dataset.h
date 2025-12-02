#pragma once
#include "cpp/basecamp.pb.h"
#include <map>
#include <string>
#include <vector>

class KVDataset {
public:
  // Initialize partition for this node
  // C: keys 0-499, D: 500-749, F: 750-999
  KVDataset(int key_min, int key_max);

  // Query range [qmin, qmax] and return results in chunks
  std::vector<basecamp::KVResult> QueryRange(int qmin, int qmax,
                                             int chunk_size = 100);

  // Get partition bounds
  int GetMinKey() const { return key_min_; }
  int GetMaxKey() const { return key_max_; }

  // Get total number of keys in this partition
  int GetKeyCount() const { return static_cast<int>(data_.size()); }

private:
  int key_min_, key_max_;
  std::map<int, int64_t> data_;

  void GenerateData();            // deterministic seeded generation
  int64_t HashKey(int key) const; // deterministic hash function
};
