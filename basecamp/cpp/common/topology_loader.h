#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "cpp/basecamp.grpc.pb.h"
#include "cpp/topology.pb.h"

struct NodeCtx {
  topo::Node self;
  std::map<std::string, topo::Node> neighbors; // name -> node
  std::map<std::string, std::unique_ptr<basecamp::Basecamp::Stub>> stubs; // name -> stub
};

/// Loads the topology textproto from TOPOLOGY_FILE and builds stubs to neighbors
NodeCtx LoadNodeContext(const std::string& topology_file, const std::string& node_name);

/// Returns "host:port" for a node
inline std::string Addr(const topo::Node& n) {
  return n.host() + ":" + std::to_string(n.port());
}
