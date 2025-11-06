#include <cstdlib>
#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "cpp/server/handler.h"
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
  try {
    const char* tf_env = std::getenv("TOPOLOGY_FILE");
    if (!tf_env) {
      std::cerr << "TOPOLOGY_FILE env var is required\n";
      return 2;
    }
    std::string node = GetArg("node", argc, argv);
    if (node.empty()) {
      std::cerr << "Usage: " << argv[0] << " --node=A\n";
      return 2;
    }

    NodeCtx ctx = LoadNodeContext(tf_env, node);
    std::cerr << "Neighbors for " << ctx.self.name() << ":";
for (const auto& kv : ctx.neighbors) {
  std::cerr << " " << kv.first << "(" << Addr(kv.second) << ")";
}
std::cerr << "\n";
    std::string listen_addr = Addr(ctx.self);
    std::cout << "Starting node " << ctx.self.name() << " on " << listen_addr << "\n";

    Handler svc(std::move(ctx));
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&svc);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    std::cout << "Server started. Ctrl-C to exit.\n";
    server->Wait();
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
