#pragma once
#include <atomic>
#include <future>
#include <string>

#include <grpcpp/grpcpp.h>

#include "cpp/basecamp.grpc.pb.h"
#include "cpp/common/topology_loader.h"

class Handler final : public basecamp::Basecamp::Service {
public:
  explicit Handler(NodeCtx ctx);

  ::grpc::Status Handle(::grpc::ServerContext*,
                        const basecamp::Request* req,
                        basecamp::Result* res) override;

  ::grpc::Status Health(::grpc::ServerContext*,
                        const basecamp::HealthRequest*,
                        basecamp::HealthReply* reply) override;

private:
  NodeCtx ctx_;
  std::atomic<int> inflight_{0};

  // Minimal deterministic “work”
  basecamp::Result DoLocalWork(const basecamp::Request& req) const;

  // Forward to a single neighbor by name; returns OK on success and fills res
  ::grpc::Status CallNeighbor(const std::string& name,
                              const basecamp::Request& req,
                              basecamp::Result* res);
};
