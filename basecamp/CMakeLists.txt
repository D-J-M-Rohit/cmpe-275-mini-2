cmake_minimum_required(VERSION 3.20)
project(basecamp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer CONFIG packages from Homebrew
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

# gRPC (will pull matching Protobuf via find_dependency)
find_package(gRPC CONFIG REQUIRED)

# -------- Generated proto sources --------
add_library(proto_objs
  cpp/basecamp.pb.cc
  cpp/basecamp.grpc.pb.cc
  cpp/topology.pb.cc
)

# Let includes work as "#include \"cpp/...\"" (and plain generated headers)
target_include_directories(proto_objs PUBLIC
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/cpp
)

target_link_libraries(proto_objs PUBLIC gRPC::grpc++)

# -------- Server --------
add_executable(basecamp_server
  cpp/server/main_server.cc
  cpp/server/handler.cc
  cpp/common/topology_loader.cc
)
target_include_directories(basecamp_server PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(basecamp_server PRIVATE proto_objs gRPC::grpc++)

# -------- Client --------
add_executable(basecamp_client
  cpp/client/main_client.cc
  cpp/common/topology_loader.cc
)
target_include_directories(basecamp_client PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(basecamp_client PRIVATE proto_objs gRPC::grpc++)