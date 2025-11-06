#include "cpp/common/topology_loader.h"

#include <fstream>
#include <sstream>

#include <google/protobuf/text_format.h>

static topo::Topology ReadTopologyTextproto(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("Cannot open TOPOLOGY_FILE: " + path);
  }
  std::stringstream buf;
  buf << in.rdbuf();
  topo::Topology t;
  if (!google::protobuf::TextFormat::ParseFromString(buf.str(), &t)) {
    throw std::runtime_error("Failed to parse topology textproto: " + path);
  }
  return t;
}

NodeCtx LoadNodeContext(const std::string& topology_file, const std::string& node_name) {
  topo::Topology topo_all = ReadTopologyTextproto(topology_file);

  const topo::Node* me = nullptr;
  for (const auto& n : topo_all.nodes()) {
    if (n.name() == node_name) { me = &n; break; }
  }
  if (!me) throw std::runtime_error("Node " + node_name + " not found in topology");

  NodeCtx ctx;
  ctx.self = *me;

  // Index all nodes by name for neighbor lookup
  std::map<std::string, topo::Node> by_name;
  for (const auto& n : topo_all.nodes()) by_name[n.name()] = n;

  // Build neighbor maps + stubs
  for (const auto& nbr : me->neighbors()) {
  auto it = by_name.find(nbr);
  if (it == by_name.end()) continue;

  ctx.neighbors[nbr] = it->second;

  auto addr = Addr(it->second);
  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());

  // Wait up to 2s for the channel to connect (helps diagnose E)
  auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
  bool ok = channel->WaitForConnected(deadline);
  if (!ok) {
    std::cerr << "WARN: neighbor " << nbr << " at " << addr << " not reachable during startup\n";
  } else {
    std::cerr << "INFO: neighbor " << nbr << " connected at " << addr << "\n";
  }

  ctx.stubs[nbr] = basecamp::Basecamp::NewStub(channel);
}
  return ctx;
}
