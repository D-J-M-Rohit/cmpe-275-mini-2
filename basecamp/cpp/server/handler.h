#pragma once
#include <atomic>
#include <future>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "cpp/basecamp.grpc.pb.h"
#include "cpp/common/admission_control.h"
#include "cpp/common/cache.h"
#include "cpp/common/cancellation.h"
#include "cpp/common/chunk_manager.h"
#include "cpp/common/topology_loader.h"
#include "cpp/workload/kv_dataset.h"

class Handler final : public basecamp::Basecamp::Service {
public:
  explicit Handler(NodeCtx ctx);

  // Legacy API
  ::grpc::Status Handle(::grpc::ServerContext *, const basecamp::Request *req,
                        basecamp::Result *res) override;

  ::grpc::Status Health(::grpc::ServerContext *,
                        const basecamp::HealthRequest *,
                        basecamp::HealthReply *reply) override;

  // New chunked API
  ::grpc::Status InitQuery(::grpc::ServerContext *,
                           const basecamp::InitRequest *req,
                           basecamp::Chunk *chunk) override;

  ::grpc::Status GetChunk(::grpc::ServerContext *,
                          const basecamp::ChunkRequest *req,
                          basecamp::Chunk *chunk) override;

  ::grpc::Status Cancel(::grpc::ServerContext *,
                        const basecamp::CancelRequest *req,
                        basecamp::CancelReply *reply) override;

private:
  NodeCtx ctx_;
  std::atomic<int> inflight_{0};

  // New components
  std::unique_ptr<ChunkManager> chunk_mgr_;
  std::unique_ptr<FairScheduler> scheduler_;
  std::unique_ptr<KVDataset> dataset_;
  LRUCache<std::string> cache_{1000, 300}; // 1000 entries, 5min TTL

  // Legacy helpers
  basecamp::Result DoLocalWork(const basecamp::Request &req) const;
  ::grpc::Status CallNeighbor(const std::string &name,
                              const basecamp::Request &req,
                              basecamp::Result *res);

  // Helper to build a request for a neighbor with updated path
  basecamp::Request
  BuildForwardRequest(const basecamp::Request &incoming) const;

  // New helpers for chunked API
  ::grpc::Status InitQueryLeader(const basecamp::InitRequest *req,
                                 basecamp::Chunk *chunk);
  ::grpc::Status InitQueryTeamLeader(const basecamp::InitRequest *req,
                                     basecamp::Chunk *chunk);
  ::grpc::Status InitQueryWorker(const basecamp::InitRequest *req,
                                 basecamp::Chunk *chunk);

  void PropagateCancel(const std::string &req_id, const std::string &token,
                       const std::vector<std::string> &path);

  // Forward chunked request to neighbor
  ::grpc::Status CallNeighborChunked(const std::string &name,
                                     const basecamp::InitRequest &req,
                                     basecamp::Chunk *chunk);
};
