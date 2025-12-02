#pragma once
#include <map>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "cpp/basecamp.grpc.pb.h"
#include "cpp/common/neighbor_client.h"
#include "cpp/topology.pb.h"

struct NodeCtx {
  topo::Node self;
  std::map<std::string, topo::Node> neighbors;
  // v3: Map from neighbor name to NeighborClient for connection pooling
  std::map<std::string, std::unique_ptr<NeighborClient>> neighbor_clients;

  // Legacy: Map from neighbor name to gRPC stub (deprecated in favor of
  // neighbor_clients)
  std::map<std::string, std::unique_ptr<basecamp::Basecamp::Stub>> stubs;
};

std::string Addr(const topo::Node &n);

NodeCtx LoadNodeContext(const std::string &topology_file,
                        const std::string &node_name);

/// Returns "host:port" for a node
inline std::string Addr(const topo::Node &n) {
  return n.host() + ":" + std::to_string(n.port());
}
