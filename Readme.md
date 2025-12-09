# Basecamp Distributed System

A fault-tolerant distributed key-value store system with C++ and Python nodes.

## 1. Cloning the Repository

```bash
git clone https://github.com/D-J-M-Rohit/cmpe-275-mini-2.git
cd cmpe-275-mini-2/basecamp
```

## 2. Prerequisites

- **C++ Compiler**: `g++` or `clang++` (supporting C++17)
- **Build System**: `cmake`
- **Python**: Python 3.9+
- **Protobuf Compiler**: `protoc`

## 3. Build C++ Components

Compile the C++ server and client binaries:

```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
cd ..
```

This creates:
- `build/basecamp_server`
- `build/basecamp_client`

## 4. Python Environment Setup

The system requires Python nodes (E and F) to run alongside C++ nodes.

1. **Create and Activate Virtual Environment**:
   ```bash
   python3 -m venv .venv
   source .venv/bin/activate
   ```

2. **Install Dependencies**:
   ```bash
   pip install grpcio protobuf grpcio-tools
   ```

3. **Generate Python Protobuf Stubs**:
   ```bash
   # Generate basecamp definitions
   python -m grpc_tools.protoc -Iproto --python_out=py --grpc_python_out=py proto/basecamp.proto
   
   # Generate topology definitions
   python -m grpc_tools.protoc -Iproto --python_out=py --grpc_python_out=py proto/topology.proto
   ```

## 5. Running Tests

The repository includes automated scripts to test fault tolerance and caching.

### A. Fault Tolerance Test
Tests leader failure recovery (nodes B and E) and exponential backoff retry logic.

```bash
# Run with default payload (512 bytes)
./fault_tolerance_test.sh

# Run with custom payload size (e.g., 1KB)
./fault_tolerance_test.sh 1024
```

### B. Cache & Idempotency Test
Tests the query cache using duplicate request IDs and retry scenarios.

**Usage:** `./cache_retry_test.sh [ITERATIONS] [PAYLOAD_SIZE]`

```bash
# Run default test (5 iterations, 512 bytes)
./cache_retry_test.sh

# Run 10 iterations with default payload
./cache_retry_test.sh 10

# Run 5 iterations with 1KB payload
./cache_retry_test.sh 5 1024
```

## Troubleshooting

- **Server binding failed**: Ensure ports 50051-50056 are free.
- **Python module not found**: Ensure `.venv` is activated.
- **Permission denied**: Run `chmod +x *.sh` to make scripts executable.
