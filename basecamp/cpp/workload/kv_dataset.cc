#include "cpp/workload/kv_dataset.h"
#include <iostream>

KVDataset::KVDataset(int key_min, int key_max)
    : key_min_(key_min), key_max_(key_max) {
  GenerateData();
  std::cerr << "KVDataset initialized: keys [" << key_min_ << ", " << key_max_
            << "], count=" << data_.size() << "\n";
}

void KVDataset::GenerateData() {
  for (int k = key_min_; k <= key_max_; ++k) {
    data_[k] = HashKey(k);
  }
}

int64_t KVDataset::HashKey(int key) const {
  // Deterministic hash: value = (key * prime1 + prime2) % 10000
  const int64_t prime1 = 1315423911LL;
  const int64_t prime2 = 2654435761LL;
  return ((static_cast<int64_t>(key) * prime1 + prime2) % 10000);
}

std::vector<basecamp::KVResult> KVDataset::QueryRange(int qmin, int qmax,
                                                      int chunk_size) {
  std::vector<basecamp::KVResult> results;

  // Clamp query range to partition bounds
  int start = std::max(qmin, key_min_);
  int end = std::min(qmax, key_max_);

  if (start > end) {
    // No overlap with this partition
    return results;
  }

  basecamp::KVResult current_chunk;
  int64_t partial_sum = 0;
  int count = 0;

  for (int k = start; k <= end; ++k) {
    auto it = data_.find(k);
    if (it == data_.end()) {
      continue; // key not in partition
    }

    auto *pair = current_chunk.add_pairs();
    pair->set_key(k);
    pair->set_value(it->second);

    partial_sum += it->second;
    count++;

    if (count >= chunk_size) {
      // Chunk is full, finalize and start new one
      current_chunk.set_partial_sum(partial_sum);
      results.push_back(std::move(current_chunk));

      current_chunk = basecamp::KVResult();
      partial_sum = 0;
      count = 0;
    }
  }

  // Add final chunk if not empty
  if (count > 0) {
    current_chunk.set_partial_sum(partial_sum);
    results.push_back(std::move(current_chunk));
  }

  std::cerr << "QueryRange [" << qmin << ", " << qmax << "] in partition ["
            << key_min_ << ", " << key_max_ << "] returned " << results.size()
            << " chunks\n";

  return results;
}
