# Basecamp (minimal, required-only)

A tiny distributed system: **six processes (A–F)** talking via **synchronous gRPC**, with a **fixed overlay**:
`AB, BC, BD, AE, EF, ED`. One server (**E**) is in **Python**; the rest and the client are **C++**.  
No hardcoding: all roles/hosts/capacity come from a **topology textproto**.

---

## 0) Prereqs

- macOS (Homebrew) or Linux
- **CMake ≥ 3.20**, **C++17** compiler
- **gRPC + Protobuf (C++)** — e.g. on macOS:
  ```bash
  brew install grpc protobuf
  ```
- **Python 3.10+** for server **E**
  ```bash
  python3 -m venv .venv
  source .venv/bin/activate
  python -m pip install --upgrade pip
  python -m pip install grpcio grpcio-tools
  ```

---

## 1) Repo layout

```
basecamp/
  CMakeLists.txt
  proto/
    basecamp.proto
    topology.proto
  config/
    topology_2hosts.textproto
    topology_3hosts.textproto
  cpp/
    basecamp.pb.cc/.h           (generated)
    basecamp.grpc.pb.cc/.h      (generated)
    topology.pb.cc/.h           (generated)
    common/
      topology_loader.{h,cc}
    server/
      main_server.cc
      handler.{h,cc}
    client/
      main_client.cc
  py/
    __init__.py
    server_e.py                 (Python node E)
  build/                        (cmake output)
  run/
```

---

## 2) (Re)generate stubs (only if you edit the protos)

```bash
# C++
protoc -I=proto --cpp_out=cpp --grpc_out=cpp   --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) proto/basecamp.proto
protoc -I=proto --cpp_out=cpp proto/topology.proto

# Python
python -m grpc_tools.protoc -I proto   --python_out=py --grpc_python_out=py proto/basecamp.proto
python -m grpc_tools.protoc -I proto   --python_out=py proto/topology.proto
```

---

## 3) Build (C++)

```bash
cmake -S . -B build   -DgRPC_DIR="$(brew --prefix grpc)/lib/cmake/grpc"   -DProtobuf_DIR="$(brew --prefix protobuf)/lib/cmake/protobuf"
cmake --build build -j
```

This builds:
- `build/basecamp_server`
- `build/basecamp_client`

---

## 4) Configure topology

For local dev, `config/topology_2hosts.textproto` can point everything at localhost:

```textproto
nodes: [
  { name:"A" host:"127.0.0.1" port:50051 is_leader:true  is_team_leader:false team:""      neighbors:["B","E"] max_inflight:64 },
  { name:"B" host:"127.0.0.1" port:50052 is_leader:false is_team_leader:true  team:"GREEN" neighbors:["A","C","D"] max_inflight:32 },
  { name:"C" host:"127.0.0.1" port:50053 is_leader:false is_team_leader:false team:"GREEN" neighbors:["B"] max_inflight:32 },
  { name:"D" host:"127.0.0.1" port:50054 is_leader:false is_team_leader:false team:"PINK"  neighbors:["B","E"] max_inflight:32 },
  { name:"E" host:"127.0.0.1" port:50055 is_leader:false is_team_leader:true  team:"PINK"  neighbors:["A","F","D"] max_inflight:32 },
  { name:"F" host:"127.0.0.1" port:50056 is_leader:false is_team_leader:false team:"PINK"  neighbors:["E"] max_inflight:32 }
]
```

**Note:** The overlay must remain exactly `AB, BC, BD, AE, EF, ED`.

---

## 5) Run (each node in its own shell)

Common env (use absolute path):

```bash
export TOPOLOGY_FILE="$(pwd)/config/topology_2hosts.textproto"
```

### Start servers (5 shells + 1 for Python):

```bash
# A
./build/basecamp_server --node=A

# B
./build/basecamp_server --node=B

# C
./build/basecamp_server --node=C

# D
./build/basecamp_server --node=D

# E (Python)
source .venv/bin/activate
export PYTHONPATH=".:./py"
python -m py.server_e

# F
./build/basecamp_server --node=F
```

`server_e.py` binds IPv4 and IPv6 to avoid macOS loopback quirks and prints when it receives RPCs.

---

## 6) Test (client → leader A)

```bash
# GREEN path: A -> B -> (C)
./build/basecamp_client --target=GREEN --payload_size=1024

# PINK path: A -> E (Python) -> (F)
./build/basecamp_client --target=PINK  --payload_size=1024

# BOTH: A -> (B + E) in parallel, then merge
./build/basecamp_client --target=BOTH  --payload_size=1024
```

**Expected:** all `OK`, and `data_size(BOTH) ≈ data_size(GREEN) + data_size(PINK)`.

---

## 7) Capacity / backpressure demo (required behavior)

Set small capacity for team leaders in the topology, e.g.:

```textproto
# B and E as team leaders
max_inflight: 1
```

Restart **only** B and E, then:

```bash
for i in 1 2 3 4; do ./build/basecamp_client --target=BOTH --payload_size=512 & done; wait
```

You should see some successes and partial/overload behavior (leader merges what it gets; if both teams fail, returns error).

---

## 8) Two-host / three-host layouts (assignment constraint)

- **2-host:** host1 `{A,B,D}`, host2 `{C,E,F}`
- **3-host:** host1 `{A,C}`, host2 `{B,D}`, host3 `{E,F}`

Edit the `host:` fields to real IPs, copy the same topology file to each machine, export **absolute** `TOPOLOGY_FILE` on each host, and start the assigned nodes. The client results should match local runs.

---

## 9) What this proves (grading highlights)

- Correct routing + merge per fixed overlay (GREEN, PINK, BOTH).
- No hardcoding: roles/hosts/capacity are read from `TOPOLOGY_FILE`.
- Sync gRPC APIs only (C++ & Python).
- Python server **E** participates and interoperates with C++.
- Capacity limits enforced at team leaders (B/E) with `RESOURCE_EXHAUSTED`.
- Runs on ≥2 machines by changing only the topology file.

---

## Troubleshooting (quick)

- **`TOPOLOGY_FILE env var is required`**  
  Export an **absolute path** before starting any node/client.

- **`Failed to add port` / name not found**  
  Use real IPs or `127.0.0.1` in the topology.

- **PINK path fails on macOS**  
  Ensure `server_e.py` is running and dual-binds (it does by default). Keep E’s `host` as `127.0.0.1` or `localhost`.

- **Python import errors (`ModuleNotFoundError: py`)**  
  Ensure `py/__init__.py` exists and `export PYTHONPATH=".:./py"`.

- **Python big-int error**  
  Fixed: E masks `acc` to 64-bit and formats as hex.

- **“Both teams failed”**  
  Check B/E are up; watch leader logs. We set short RPC deadlines and log neighbor errors.

