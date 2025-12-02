import os, time, threading, grpc
from concurrent.futures import ThreadPoolExecutor
from google.protobuf import text_format
from py import basecamp_pb2 as pb, basecamp_pb2_grpc as rpc, topology_pb2 as tpb
from collections import OrderedDict
import sys

if hasattr(sys, "set_int_max_str_digits"):
    sys.set_int_max_str_digits(0) 

def load_topology(path, node):
    t = tpb.Topology()
    with open(path) as f:
        text_format.Merge(f.read(), t)
    me = next(n for n in t.nodes if n.name == node)
    by = {n.name: n for n in t.nodes}
    nbrs = {k: by[k] for k in me.neighbors if k in by}
    return me, nbrs

# Simple LRU Cache
class LRUCache:
    def __init__(self, capacity=1000, ttl=300):
        self.capacity = capacity
        self.ttl = ttl
        self.cache = OrderedDict()
        self.lock = threading.Lock()
    
    def get(self, key):
        with self.lock:
            if key in self.cache:
                val, ts = self.cache.pop(key)
                if time.time() - ts < self.ttl:
                    self.cache[key] = (val, ts)  # move to end (MRU)
                    return val
            return None
    
    def put(self, key, value):
        with self.lock:
            if key in self.cache:
                self.cache.pop(key)
            elif len(self.cache) >= self.capacity:
                self.cache.popitem(last=False)  # evict LRU
            self.cache[key] = (value, time.time())

# Cancellation Registry
class CancellationRegistry:
    def __init__(self):
        self.tokens = {}
        self.lock = threading.Lock()
    
    def register(self, req_id, token):
        with self.lock:
            self.tokens[req_id] = {"token": token, "cancelled": False, "ts": time.time()}
    
    def cancel(self, req_id, token):
        with self.lock:
            if req_id in self.tokens and self.tokens[req_id]["token"] == token:
                self.tokens[req_id]["cancelled"] = True
                return True
        return False
    
    def is_cancelled(self, req_id):
        with self.lock:
            return self.tokens.get(req_id, {}).get("cancelled", False)

# KV Dataset for node F (keys 750-999)
class KVDataset:
    def __init__(self, key_min, key_max):
        self.key_min = key_min
        self.key_max = key_max
        self.data = {}
        self._generate_data()
        print(f"KVDataset initialized: keys [{key_min}, {key_max}], count={len(self.data)}", flush=True)
    
    def _generate_data(self):
        # Deterministic hash: value = (key * prime1 + prime2) % 10000
        prime1 = 1315423911
        prime2 = 2654435761
        for k in range(self.key_min, self.key_max + 1):
            self.data[k] = (k * prime1 + prime2) % 10000
    
    def query_range(self, qmin, qmax, chunk_size=100):
        start = max(qmin, self.key_min)
        end = min(qmax, self.key_max)
        
        if start > end:
            return []  # no overlap
        
        results = []
        current_chunk = pb.KVResult()
        partial_sum = 0
        count = 0
        
        for k in range(start, end + 1):
            if k not in self.data:
                continue
            
            pair = current_chunk.pairs.add()
            pair.key = k
            pair.value = self.data[k]
            
            partial_sum += self.data[k]
            count += 1
            
            if count >= chunk_size:
                current_chunk.partial_sum = partial_sum
                results.append(current_chunk)
                current_chunk = pb.KVResult()
                partial_sum = 0
                count = 0
        
        if count > 0:
            current_chunk.partial_sum = partial_sum
            results.append(current_chunk)
        
        print(f"QueryRange [{qmin}, {qmax}] in partition [{self.key_min}, {self.key_max}] returned {len(results)} chunks", flush=True)
        return results

class Service(rpc.BasecampServicer):
    def __init__(self, me, nbrs):
        self.me, self.nbrs = me, nbrs
        self._lock = threading.Lock()
        self._inflight = 0
        self.cache = LRUCache()
        self.cancel_registry = CancellationRegistry()
        self.stubs = {
            name: rpc.BasecampStub(grpc.insecure_channel(f"{n.host}:{n.port}"))
            for name, n in nbrs.items()
        }
        # KV dataset only for node F (worker)
        self.dataset = None

    def Health(self, request, ctx):
        return pb.HealthReply(node=self.me.name)

    def Handle(self, request, ctx):
        print("E.Handle() received", flush=True)

        # Capacity guard (team leader only)
        if self.me.is_team_leader and self.me.max_inflight > 0:
            with self._lock:
                if self._inflight >= self.me.max_inflight:
                    ctx.abort(grpc.StatusCode.RESOURCE_EXHAUSTED, "overloaded")
                self._inflight += 1
        try:
            MASK = (1 << 64) - 1
            start = time.time()
            acc = 0
            for b in request.payload:                     # b is 0..255
                acc = ((acc * 1315423911 + b) & MASK) ^ 0x9E3779B97F4A7C15
                acc &= MASK
            for _ in range(1000):
                acc ^= ((acc << 13) & MASK); acc &= MASK
                acc ^= (acc >> 7);              acc &= MASK
                acc ^= ((acc << 17) & MASK);    acc &= MASK

            ms = int((time.time() - start) * 1000)
            local = pb.Result(
                request_id=request.request_id,
                compute_ms=ms,
                data=f"node=E;acc=0x{acc:016x};".encode()  # fixed 64-bit hex; no huge decimal
            )

            # Log request
            path_str = ",".join(request.path)
            print(f"[{request.request_id}] E received (path={path_str})", flush=True)

            # Optional one-hop inside team (E -> F)
            nbr = next((n for n in self.nbrs.values()
                        if n.team == self.me.team and n.name != self.me.name), None)
            if nbr and nbr.name in self.stubs:
                try:
                    # Forward with updated path
                    fwd_req = pb.Request()
                    fwd_req.CopyFrom(request)
                    fwd_req.path.append(self.me.name)
                    
                    print(f"[{request.request_id}] E -> {nbr.name} (size={len(request.payload)})", flush=True)
                    
                    sub = self.stubs[nbr.name].Handle(fwd_req)
                    return pb.Result(
                        request_id=request.request_id,
                        compute_ms=local.compute_ms + sub.compute_ms,
                        data=local.data + sub.data
                    )
                except grpc.RpcError:
                    pass
            return local
        finally:
            if self.me.is_team_leader and self.me.max_inflight > 0:
                with self._lock:
                    self._inflight -= 1
    
    # NEW: Chunked API
    def InitQuery(self, request, ctx):
        print(f"{self.me.name}.InitQuery() received: [{request.key_min}, {request.key_max}]", flush=True)
        
        # Register cancellation token
        self.cancel_registry.register(request.request_id, request.cancellation_token)
        
        # Check cache
        cache_key = f"query:{request.key_min}-{request.key_max}:0"
        cached = self.cache.get(cache_key)
        if cached:
            print(f"{self.me.name}: Cache HIT for {cache_key}", flush=True)
            chunk = pb.Chunk()
            chunk.ParseFromString(cached)
            return chunk
        
        # E is team leader, forward to F (worker)
        worker = next((n for n in self.nbrs.values() 
                      if n.team == self.me.team and not n.is_team_leader), None)
        
        if worker and worker.name in self.stubs:
            try:
                chunk = self.stubs[worker.name].InitQuery(request, timeout=5)
                # Cache result
                self.cache.put(cache_key, chunk.SerializeToString())
                return chunk
            except grpc.RpcError as e:
                print(f"{self.me.name}: Failed to forward to {worker.name}: {e}", flush=True)
                ctx.abort(grpc.StatusCode.UNAVAILABLE, f"worker {worker.name} unavailable")
        
        # Fallback: empty result
        return pb.Chunk(
            request_id=request.request_id,
            chunk_index=0,
            is_final=True,
            total_chunks=1,
            data=b""
        )
    
    def GetChunk(self, request, ctx):
        # Simplified: only chunk 0 supported
        if request.chunk_index != 0:
            ctx.abort(grpc.StatusCode.OUT_OF_RANGE, "chunk index out of range")
        
        if self.cancel_registry.is_cancelled(request.request_id):
            ctx.abort(grpc.StatusCode.CANCELLED, "request cancelled")
        
        ctx.abort(grpc.StatusCode.UNIMPLEMENTED, "GetChunk not yet fully implemented")
    
    def Cancel(self, request, ctx):
        success = self.cancel_registry.cancel(request.request_id, request.cancellation_token)
        
        if success:
            # Propagate to neighbors with cycle detection
            fwd_req = pb.CancelRequest()
            fwd_req.CopyFrom(request)
            fwd_req.path.append(self.me.name)
            
            for name, stub in self.stubs.items():
                # Skip if neighbor is already in path
                if name in request.path:
                    continue
                    
                try:
                    stub.Cancel(fwd_req, timeout=1)
                except:
                    pass
            return pb.CancelReply(cancelled=True, message="cancelled")
        else:
            return pb.CancelReply(cancelled=False, message="not found or token mismatch")


if __name__ == "__main__":
    topo = os.environ["TOPOLOGY_FILE"]
    me, nbrs = load_topology(topo, "E")

    server = grpc.server(ThreadPoolExecutor(max_workers=8))
    rpc.add_BasecampServicer_to_server(Service(me, nbrs), server)

    # Bind IPv4 and IPv6 (macOS loopback quirk safety)
    bound_v4 = server.add_insecure_port(f"{me.host}:{me.port}")  # e.g., 127.0.0.1:50055
    bound_v6 = server.add_insecure_port(f"[::]:{me.port}")       # ::1:50055

    if bound_v4 == 0 and bound_v6 == 0:
        raise RuntimeError(f"Failed to bind E on {me.host}:{me.port} (v4) or [::]:{me.port} (v6)")

    print(f"E listening (v4={bool(bound_v4)}, v6={bool(bound_v6)}) on {me.host}:{me.port}", flush=True)
    server.start()
    server.wait_for_termination()
