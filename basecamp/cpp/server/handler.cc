#include "cpp/server/handler.h"

#include <atomic>
#include <chrono>
#include <iostream>   // <-- for error logging
#include <thread>

using ::grpc::Status;
using ::grpc::StatusCode;

Handler::Handler(NodeCtx ctx) : ctx_(std::move(ctx)) {}

// RAII guard to decrement inflight only if we incremented it
struct InflightGuard {
  std::atomic<int>* ctr = nullptr;
  bool active = false;
  ~InflightGuard() { if (active && ctr) ctr->fetch_sub(1); }
};

basecamp::Result Handler::DoLocalWork(const basecamp::Request& req) const {
  auto t0 = std::chrono::steady_clock::now();

  // Tiny deterministic CPU loop proportional to payload size
  uint64_t acc = 0;
  for (const auto c : req.payload())
    acc = (acc * 1315423911u + static_cast<unsigned char>(c)) ^ 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < 1000; ++i)
    acc ^= (acc << 13), acc ^= (acc >> 7), acc ^= (acc << 17);

  auto t1 = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  std::string tag = "node=" + ctx_.self.name() + ";acc=" + std::to_string(acc) + ";";
  basecamp::Result r;
  r.set_request_id(req.request_id());
  r.set_compute_ms(static_cast<int64_t>(ms));
  r.set_data(tag); // bytes
  return r;
}

Status Handler::CallNeighbor(const std::string& name,
                             const basecamp::Request& req,
                             basecamp::Result* res) {
  auto it = ctx_.stubs.find(name);
  if (it == ctx_.stubs.end()) {
    return Status(StatusCode::UNAVAILABLE, "neighbor " + name + " has no stub");
  }
  ::grpc::ClientContext cctx;
  // Short deadline so we fail fast if neighbor isn't reachable
  cctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));

  auto s = it->second->Handle(&cctx, req, res);
  if (!s.ok()) {
    std::cerr << "RPC to neighbor " << name
              << " failed: code=" << static_cast<int>(s.error_code())
              << " msg=\"" << s.error_message() << "\"\n";
  }
  return s;
}

::grpc::Status Handler::Health(::grpc::ServerContext*, const basecamp::HealthRequest*, basecamp::HealthReply* reply) {
  reply->set_node(ctx_.self.name());
  return Status::OK;
}

::grpc::Status Handler::Handle(::grpc::ServerContext*, const basecamp::Request* req, basecamp::Result* out) {
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
    const bool need_green = (req->target() == basecamp::TEAM_GREEN || req->target() == basecamp::TEAM_BOTH);
    const bool need_pink  = (req->target() == basecamp::TEAM_PINK  || req->target() == basecamp::TEAM_BOTH);

    basecamp::Result rb, re;
    // Initialize as "not requested" (non-OK) so only requested+successful teams count
    Status sb = Status(StatusCode::FAILED_PRECONDITION, "green not requested");
    Status se = Status(StatusCode::FAILED_PRECONDITION, "pink not requested");

    if (need_green) sb = CallNeighbor("B", *req, &rb);
    if (need_pink)  se = CallNeighbor("E", *req, &re);

    const bool got_green = need_green && sb.ok();
    const bool got_pink  = need_pink  && se.ok();

    if (!got_green && !got_pink) {
      return Status(StatusCode::UNAVAILABLE, "no team succeeded");
    }

    basecamp::Result merged;
    merged.set_request_id(req->request_id());
    int64_t total_ms = 0;
    std::string data;
    if (got_green) { total_ms += rb.compute_ms(); data.append(rb.data()); }
    if (got_pink)  { total_ms += re.compute_ms(); data.append(re.data()); }
    merged.set_compute_ms(total_ms);
    merged.set_data(std::move(data));
    *out = std::move(merged);
    return Status::OK;
  }

  // Team leaders B/E: do local work and optionally forward to one in-team neighbor
  if (ctx_.self.is_team_leader()) {
    auto local = DoLocalWork(*req);

    // pick a same-team neighbor if present (B->C, E->F)
    std::string same_team_neighbor;
    for (const auto& kv : ctx_.neighbors) {
      if (kv.second.team() == ctx_.self.team() && kv.second.name() != ctx_.self.name()) {
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

  // Workers C/D/F: local only
  *out = DoLocalWork(*req);
  return Status::OK;
}
