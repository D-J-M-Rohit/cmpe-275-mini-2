#pragma once

#include "cpp/basecamp.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// NeighborClient manages a persistent gRPC channel and stub for a neighbor node
// This enables connection reuse and amortizes connection setup costs
class NeighborClient {
public:
  NeighborClient(const std::string &name, const std::string &address)
      : name_(name), address_(address) {
    channel_ = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    stub_ = basecamp::Basecamp::NewStub(channel_);
  }

  // Get the neighbor's name
  const std::string &Name() const { return name_; }

  // Get the neighbor's address
  const std::string &Address() const { return address_; }

  // Get the shared channel (for monitoring/debugging)
  std::shared_ptr<grpc::Channel> Channel() const { return channel_; }

  // Get the stub for making RPC calls
  basecamp::Basecamp::Stub *Stub() const { return stub_.get(); }

  // Check if the channel is ready
  bool IsReady() const {
    return channel_->GetState(false) == GRPC_CHANNEL_READY;
  }

  // Wait for the channel to be ready (with timeout)
  bool WaitForReady(int timeout_seconds = 5) {
    auto deadline = std::chrono::system_clock::now() +
                    std::chrono::seconds(timeout_seconds);
    return channel_->WaitForConnected(deadline);
  }

private:
  std::string name_;
  std::string address_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<basecamp::Basecamp::Stub> stub_;
};
