#include "cpp/server/handler.h"

#include <atomic>
#include <chrono>
#include <iostream> // <-- for error logging
#include <thread>

#include "cpp/common/rpc_utils.h"
#include "cpp/common/topology_loader.h"
#include "cpp/workload/kv_dataset.h"

using ::grpc::Status;
using ::grpc::StatusCode;

Handler::Handler(NodeCtx ctx) : ctx_(std::move(ctx)) {
  // Initialize chunk manager (only for leader A)
  if (ctx_.self.is_leader()) {
    chunk_mgr_ = std::make_unique<ChunkManager>();
    scheduler_ = std::make_unique<FairScheduler>(
        ctx_.self.max_inflight() / 2); // split capacity between teams
  }

  // Initialize KV dataset based on node role
  // C: 0-499, D: 500-749, F: 750-999
  if (ctx_.self.name() == "C") {
    dataset_ = std::make_unique<KVDataset>(0, 499);
  } else if (ctx_.self.name() == "D") {
    dataset_ = std::make_unique<KVDataset>(500, 749);
  } else if (ctx_.self.name() == "F") {
    dataset_ = std::make_unique<KVDataset>(750, 999);
  }
}

// RAII guard to decrement inflight only if we incremented it
struct InflightGuard {
  std::atomic<int> *ctr = nullptr;
  bool active = false;
  ~InflightGuard() {
    if (active && ctr)
      ctr->fetch_sub(1);
  }
};

basecamp::Result Handler::DoLocalWork(const basecamp::Request &req) const {
  auto t0 = std::chrono::steady_clock::now();

  // Tiny deterministic CPU loop proportional to payload size
  uint64_t acc = 0;
  for (const auto c : req.payload())
    acc = (acc * 1315423911u + static_cast<unsigned char>(c)) ^
          0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < 1000; ++i)
    acc ^= (acc << 13), acc ^= (acc >> 7), acc ^= (acc << 17);

  auto t1 = std::chrono::steady_clock::now();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  std::string tag =
      "node=" + ctx_.self.name() + ";acc=" + std::to_string(acc) + ";";
  basecamp::Result r;
  r.set_request_id(req.request_id());
  r.set_compute_ms(static_cast<int64_t>(ms));
  r.set_data(tag); // bytes
  return r;
}

basecamp::Request
Handler::BuildForwardRequest(const basecamp::Request &incoming) const {
  basecamp::Request out = incoming;
  // Append self to path
  out.add_path(ctx_.self.name());
  return out;
}

Status Handler::CallNeighbor(const std::string &name,
                             const basecamp::Request &req,
                             basecamp::Result *res) {
  auto it = ctx_.stubs.find(name);
  if (it == ctx_.stubs.end()) {
    return Status(StatusCode::UNAVAILABLE, "neighbor " + name + " has no stub");
  }

  // Build forwarded request with updated path
  basecamp::Request fwd_req = BuildForwardRequest(req);

  std::cout << "[" << req.request_id() << "] " << ctx_.self.name() << " -> "
            << name << " (size=" << req.payload().size() << ")\n";

  auto do_rpc = [&]() {
    ::grpc::ClientContext cctx;
    // Short deadline so we fail fast if neighbor isn't reachable
    cctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(2));
    return it->second->Handle(&cctx, fwd_req, res);
  };

  // Use retries for fault tolerance
  auto s = CallWithRetries(do_rpc, RetryConfig(), "CallNeighbor " + name);

  if (!s.ok()) {
    std::cerr << "RPC to neighbor " << name
              << " failed: code=" << static_cast<int>(s.error_code())
              << " msg=\"" << s.error_message() << "\"\n";
  }
  return s;
}

::grpc::Status Handler::Health(::grpc::ServerContext *,
                               const basecamp::HealthRequest *,
                               basecamp::HealthReply *reply) {
  reply->set_node(ctx_.self.name());
  return Status::OK;
}

::grpc::Status Handler::Handle(::grpc::ServerContext *,
                               const basecamp::Request *req,
                               basecamp::Result *out) {
  std::cout << "[" << req->request_id() << "] " << ctx_.self.name()
            << " received (path=";
  for (const auto &p : req->path())
    std::cout << p << ",";
  std::cout << ")\n";
  // Capacity guard only for team leaders
  InflightGuard guard{&inflight_, false};
  if (ctx_.self.is_team_leader() && ctx_.self.max_inflight() > 0) {
    int now = inflight_.fetch_add(1) + 1;
    if (now > ctx_.self.max_inflight()) {
      inflight_.fetch_sub(1);
      return Status(StatusCode::RESOURCE_EXHAUSTED, "overloaded");
    }
    guard.active = true;
  }

  // Leader A: route to B/E or both
  if (ctx_.self.is_leader()) {
    const bool need_green = (req->target() == basecamp::TEAM_GREEN ||
                             req->target() == basecamp::TEAM_BOTH);
    const bool need_pink = (req->target() == basecamp::TEAM_PINK ||
                            req->target() == basecamp::TEAM_BOTH);

    basecamp::Result rb, re;
    // Initialize as "not requested" (non-OK) so only requested+successful teams
    // count
    Status sb = Status(StatusCode::FAILED_PRECONDITION, "green not requested");
    Status se = Status(StatusCode::FAILED_PRECONDITION, "pink not requested");

    if (need_green)
      sb = CallNeighbor("B", *req, &rb);
    if (need_pink)
      se = CallNeighbor("E", *req, &re);

    const bool got_green = need_green && sb.ok();
    const bool got_pink = need_pink && se.ok();

    if (!got_green && !got_pink) {
      return Status(StatusCode::UNAVAILABLE, "no team succeeded");
    }

    basecamp::Result merged;
    merged.set_request_id(req->request_id());
    int64_t total_ms = 0;
    std::string data;
    if (got_green) {
      total_ms += rb.compute_ms();
      data.append(rb.data());
    }
    if (got_pink) {
      total_ms += re.compute_ms();
      data.append(re.data());
    }
    merged.set_compute_ms(total_ms);
    merged.set_data(std::move(data));
    *out = std::move(merged);
    return Status::OK;
  }

  // Team leaders B/E: do local work and optionally forward to one in-team
  // neighbor
  if (ctx_.self.is_team_leader()) {
    auto local = DoLocalWork(*req);

    // pick a same-team neighbor if present (B->C, E->F)
    std::string same_team_neighbor;
    for (const auto &kv : ctx_.neighbors) {
      if (kv.second.team() == ctx_.self.team() &&
          kv.second.name() != ctx_.self.name()) {
        same_team_neighbor = kv.second.name();
        break;
      }
    }

    if (!same_team_neighbor.empty()) {
      basecamp::Result sub;
      auto s = CallNeighbor(same_team_neighbor, *req, &sub);
      if (s.ok()) {
        local.set_compute_ms(local.compute_ms() + sub.compute_ms());
        std::string merged = local.data();
        merged.append(sub.data());
        local.set_data(std::move(merged));
      }
    }
    *out = std::move(local);
    return Status::OK;
  }

  // Workers C/D/F: local only with idempotency check
  // v2: Check idempotency cache for duplicate requests
  auto cached_response = idempotency_cache_.Get(req->request_id());
  if (cached_response.has_value()) {
    std::cerr << ctx_.self.name() << ": Idempotency HIT for "
              << req->request_id() << "\n";
    out->ParseFromString(*cached_response);
    return Status::OK;
  }

  *out = DoLocalWork(*req);

  // Cache response for idempotency
  idempotency_cache_.Put(req->request_id(), out->SerializeAsString());

  return Status::OK;
}

// ============================================================================
// NEW CHUNKED API IMPLEMENTATIONS
// ============================================================================

::grpc::Status Handler::InitQuery(::grpc::ServerContext *ctx,
                                  const basecamp::InitRequest *req,
                                  basecamp::Chunk *chunk) {
  // Register cancellation token
  g_cancel_registry.Register(req->request_id(), req->cancellation_token());

  // Check cache first
  std::string cache_key =
      MakeCacheKey("query", req->key_min(), req->key_max(), 0);
  auto cached = cache_.Get(cache_key);
  if (cached.has_value()) {
    std::cerr << ctx_.self.name() << ": Cache HIT for " << cache_key << "\n";
    chunk->ParseFromString(*cached);
    return Status::OK;
  }

  // Route based on node role
  if (ctx_.self.is_leader()) {
    return InitQueryLeader(req, chunk);
  } else if (ctx_.self.is_team_leader()) {
    return InitQueryTeamLeader(req, chunk);
  } else {
    return InitQueryWorker(req, chunk);
  }
}

::grpc::Status Handler::InitQueryLeader(const basecamp::InitRequest *req,
                                        basecamp::Chunk *chunk) {
  // A: Launch parallel queries to B and E
  const bool need_green = (req->target() == basecamp::TEAM_GREEN ||
                           req->target() == basecamp::TEAM_BOTH);
  const bool need_pink = (req->target() == basecamp::TEAM_PINK ||
                          req->target() == basecamp::TEAM_BOTH);

  basecamp::Chunk green_chunk, pink_chunk;
  Status sb = Status(StatusCode::FAILED_PRECONDITION, "green not requested");
  Status se = Status(StatusCode::FAILED_PRECONDITION, "pink not requested");

  if (need_green) {
    AdmissionGuard guard(scheduler_.get(), "GREEN");
    if (!guard.Acquired()) {
      return Status(StatusCode::RESOURCE_EXHAUSTED, "GREEN team overloaded");
    }
    sb = CallNeighborChunked("B", *req, &green_chunk);
  }

  if (need_pink) {
    AdmissionGuard guard(scheduler_.get(), "PINK");
    if (!guard.Acquired()) {
      return Status(StatusCode::RESOURCE_EXHAUSTED, "PINK team overloaded");
    }
    se = CallNeighborChunked("E", *req, &pink_chunk);
  }

  const bool got_green = need_green && sb.ok();
  const bool got_pink = need_pink && se.ok();

  if (!got_green && !got_pink) {
    return Status(StatusCode::UNAVAILABLE, "no team succeeded");
  }

  // Merge chunks from both teams
  basecamp::KVResult merged_result;
  int64_t total_ms = 0;

  if (got_green) {
    basecamp::KVResult green_result;
    green_result.ParseFromString(green_chunk.data());
    for (const auto &pair : green_result.pairs()) {
      *merged_result.add_pairs() = pair;
    }
    merged_result.set_partial_sum(merged_result.partial_sum() +
                                  green_result.partial_sum());
    total_ms += green_chunk.compute_ms();
  }

  if (got_pink) {
    basecamp::KVResult pink_result;
    pink_result.ParseFromString(pink_chunk.data());
    for (const auto &pair : pink_result.pairs()) {
      *merged_result.add_pairs() = pair;
    }
    merged_result.set_partial_sum(merged_result.partial_sum() +
                                  pink_result.partial_sum());
    total_ms += pink_chunk.compute_ms();
  }

  // Build response chunk
  chunk->set_request_id(req->request_id());
  chunk->set_chunk_index(0);
  chunk->set_is_final(true); // For now, single chunk
  chunk->set_total_chunks(1);
  chunk->set_data(merged_result.SerializeAsString());
  chunk->set_compute_ms(total_ms);

  // Cache result
  std::string cache_key =
      MakeCacheKey("query", req->key_min(), req->key_max(), 0);
  cache_.Put(cache_key, chunk->SerializeAsString());

  return Status::OK;
}

::grpc::Status Handler::InitQueryTeamLeader(const basecamp::InitRequest *req,
                                            basecamp::Chunk *chunk) {
  // B/E: Forward to worker in same team
  std::string worker;
  for (const auto &kv : ctx_.neighbors) {
    if (kv.second.team() == ctx_.self.team() && !kv.second.is_team_leader()) {
      worker = kv.second.name();
      break;
    }
  }

  if (worker.empty()) {
    return Status(StatusCode::NOT_FOUND, "no worker in team");
  }

  // Check cache first
  std::string cache_key =
      MakeCacheKey("query", req->key_min(), req->key_max(), 0);
  auto cached = cache_.Get(cache_key);
  if (cached.has_value()) {
    std::cerr << ctx_.self.name() << ": Cache HIT for " << cache_key << "\n";
    chunk->ParseFromString(*cached);
    return Status::OK;
  }

  // Forward to worker
  auto status = CallNeighborChunked(worker, *req, chunk);

  if (status.ok()) {
    // Cache result
    cache_.Put(cache_key, chunk->SerializeAsString());
  }

  return status;
}

::grpc::Status Handler::InitQueryWorker(const basecamp::InitRequest *req,
                                        basecamp::Chunk *chunk) {
  // C/D/F: Execute query on local KV dataset
  if (!dataset_) {
    return Status(StatusCode::FAILED_PRECONDITION, "no dataset on this node");
  }

  // Check if cancelled
  if (g_cancel_registry.IsCancelled(req->request_id())) {
    return Status(StatusCode::CANCELLED, "request cancelled");
  }

  // Check cache
  std::string cache_key =
      MakeCacheKey("query", req->key_min(), req->key_max(), 0);
  auto cached = cache_.Get(cache_key);
  if (cached.has_value()) {
    std::cerr << ctx_.self.name() << ": Cache HIT for " << cache_key << "\n";
    chunk->ParseFromString(*cached);
    return Status::OK;
  }

  auto t0 = std::chrono::steady_clock::now();

  // Query dataset
  auto results =
      dataset_->QueryRange(req->key_min(), req->key_max(), CHUNK_SIZE);

  auto t1 = std::chrono::steady_clock::now();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  if (results.empty()) {
    // No data in this partition's range
    basecamp::KVResult empty;
    chunk->set_request_id(req->request_id());
    chunk->set_chunk_index(0);
    chunk->set_is_final(true);
    chunk->set_total_chunks(1);
    chunk->set_data(empty.SerializeAsString());
    chunk->set_compute_ms(ms);
    return Status::OK;
  }

  // For now, return first chunk only (single-chunk implementation)
  chunk->set_request_id(req->request_id());
  chunk->set_chunk_index(0);
  chunk->set_is_final(true);
  chunk->set_total_chunks(1);
  chunk->set_data(results[0].SerializeAsString());
  chunk->set_compute_ms(ms);

  // Cache result
  cache_.Put(cache_key, chunk->SerializeAsString());

  std::cerr << ctx_.self.name() << ": Computed chunk 0 for [" << req->key_min()
            << ", " << req->key_max() << "], " << results[0].pairs_size()
            << " pairs, sum=" << results[0].partial_sum() << "\n";

  return Status::OK;
}

::grpc::Status Handler::GetChunk(::grpc::ServerContext *,
                                 const basecamp::ChunkRequest *req,
                                 basecamp::Chunk *chunk) {
  // For now, only chunk 0 is supported (single-chunk implementation)
  if (req->chunk_index() != 0) {
    return Status(StatusCode::OUT_OF_RANGE, "chunk index out of range");
  }

  // Check if cancelled
  if (g_cancel_registry.IsCancelled(req->request_id())) {
    return Status(StatusCode::CANCELLED, "request cancelled");
  }

  // This is a simplified implementation - in full version, would retrieve from
  // chunk_mgr_
  return Status(StatusCode::UNIMPLEMENTED,
                "GetChunk not yet fully implemented");
}

::grpc::Status Handler::Cancel(::grpc::ServerContext *,
                               const basecamp::CancelRequest *req,
                               basecamp::CancelReply *reply) {
  bool success =
      g_cancel_registry.Cancel(req->request_id(), req->cancellation_token());

  if (success) {
    // Propagate to neighbors
    std::vector<std::string> path(req->path().begin(), req->path().end());
    path.push_back(ctx_.self.name());
    PropagateCancel(req->request_id(), req->cancellation_token(), path);
    reply->set_cancelled(true);
    reply->set_message("cancelled");
  } else {
    reply->set_cancelled(false);
    reply->set_message("not found or token mismatch");
  }

  return Status::OK;
}

void Handler::PropagateCancel(const std::string &req_id,
                              const std::string &token,
                              const std::vector<std::string> &path) {
  basecamp::CancelRequest cancel_req;
  cancel_req.set_request_id(req_id);
  cancel_req.set_cancellation_token(token);
  for (const auto &p : path) {
    cancel_req.add_path(p);
  }

  for (const auto &kv : ctx_.stubs) {
    // Skip if neighbor is already in path
    bool visited = false;
    for (const auto &p : path) {
      if (p == kv.first) {
        visited = true;
        break;
      }
    }
    if (visited)
      continue;

    ::grpc::ClientContext cctx;
    cctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(1));

    basecamp::CancelReply reply;
    auto status = kv.second->Cancel(&cctx, cancel_req, &reply);

    if (!status.ok()) {
      std::cerr << "Failed to propagate cancel to " << kv.first << "\n";
    }
  }
}

::grpc::Status Handler::CallNeighborChunked(const std::string &name,
                                            const basecamp::InitRequest &req,
                                            basecamp::Chunk *chunk) {
  auto it = ctx_.stubs.find(name);
  if (it == ctx_.stubs.end()) {
    return Status(StatusCode::UNAVAILABLE, "neighbor " + name + " has no stub");
  }

  auto do_rpc = [&]() {
    ::grpc::ClientContext cctx;
    cctx.set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(5));
    return it->second->InitQuery(&cctx, req, chunk);
  };

  auto s = CallWithRetries(do_rpc, RetryConfig(), "InitQuery " + name);

  if (!s.ok()) {
    std::cerr << "InitQuery to neighbor " << name
              << " failed: code=" << static_cast<int>(s.error_code())
              << " msg=\"" << s.error_message() << "\"\n";
  }
  return s;
}
