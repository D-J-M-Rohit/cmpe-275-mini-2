#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/text_format.h>

#include "cpp/basecamp.grpc.pb.h"
#include "cpp/topology.pb.h"
#include "cpp/common/topology_loader.h"

static std::string GetArg(const std::string& key, int argc, char** argv, const std::string& def = "") {
  const std::string prefix = "--" + key + "=";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
  }
  return def;
}

int main(int argc, char** argv) {
  const char* tf_env = std::getenv("TOPOLOGY_FILE");
  if (!tf_env) {
    std::cerr << "TOPOLOGY_FILE env var is required\n";
    return 2;
  }

  // Find the leader to connect to
  topo::Topology t;
  try {
    // reuse loader
    NodeCtx dummy = LoadNodeContext(tf_env, "A"); // will throw if no A; fine for sample topologies
    (void)dummy;
  } catch (...) { /* ignore */ }

  // Parse topology to find leader
  std::ifstream in(tf_env);
  std::stringstream buf; buf << in.rdbuf();
  if (!google::protobuf::TextFormat::ParseFromString(buf.str(), &t)) {
    std::cerr << "Failed to parse topology\n"; return 2;
  }
  const topo::Node* leader = nullptr;
  for (const auto& n : t.nodes()) if (n.is_leader()) { leader = &n; break; }
  if (!leader) { std::cerr << "No leader in topology\n"; return 2; }

  std::string target = GetArg("target", argc, argv, "BOTH");
  std::string size_s = GetArg("payload_size", argc, argv, "1024");
  size_t payload_size = static_cast<size_t>(std::stoul(size_s));

  basecamp::Team team = basecamp::TEAM_BOTH;
  if (target == "GREEN") team = basecamp::TEAM_GREEN;
  else if (target == "PINK") team = basecamp::TEAM_PINK;

  std::string addr = Addr(*leader);
  auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
  auto stub = basecamp::Basecamp::NewStub(channel);

  basecamp::Request req;
  req.set_request_id("req-1");
  req.set_target(team);
  req.set_payload(std::string(payload_size, '\x01'));

  grpc::ClientContext ctx;
  basecamp::Result res;
  auto s = stub->Handle(&ctx, req, &res);
  if (!s.ok()) {
    std::cerr << "RPC failed: " << s.error_message() << "\n";
    return 1;
  }
  std::cout << "OK compute_ms=" << res.compute_ms()
            << " data_size=" << res.data().size() << "\n";
  return 0;
}
