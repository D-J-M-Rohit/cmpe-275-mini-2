#!/usr/bin/env python3
"""
Worker server for node F (PINK team worker)
Handles KV partition 750-999
"""
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
                    self.cache[key] = (val, ts)
                    return val
            return None
    
    def put(self, key, value):
        with self.lock:
            if key in self.cache:
                self.cache.pop(key)
            elif len(self.cache) >= self.capacity:
                self.cache.popitem(last=False)
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

# KV Dataset
class KVDataset:
    def __init__(self, key_min, key_max):
        self.key_min = key_min
        self.key_max = key_max
        self.data = {}
        self._generate_data()
        print(f"KVDataset initialized: keys [{key_min}, {key_max}], count={len(self.data)}", flush=True)
    
    def _generate_data(self):
        prime1 = 1315423911
        prime2 = 2654435761
        for k in range(self.key_min, self.key_max + 1):
            self.data[k] = (k * prime1 + prime2) % 10000
    
    def query_range(self, qmin, qmax, chunk_size=100):
        start = max(qmin, self.key_min)
        end = min(qmax, self.key_max)
        
        if start > end:
            return []
        
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

class WorkerService(rpc.BasecampServicer):
    def __init__(self, me, nbrs):
        self.me = me
        self.nbrs = nbrs
        self.cache = LRUCache()
        self.cancel_registry = CancellationRegistry()
        self.stubs = {
            name: rpc.BasecampStub(grpc.insecure_channel(f"{n.host}:{n.port}"))
            for name, n in nbrs.items()
        }
        # F holds keys 750-999
        self.dataset = KVDataset(750, 999)

    def Health(self, request, ctx):
        return pb.HealthReply(node=self.me.name)

    def Handle(self, request, ctx):
        # Legacy API - simple local work
        MASK = (1 << 64) - 1
        start = time.time()
        acc = 0
        for b in request.payload:
            acc = ((acc * 1315423911 + b) & MASK) ^ 0x9E3779B97F4A7C15
            acc &= MASK
        for _ in range(1000):
            acc ^= ((acc << 13) & MASK); acc &= MASK
            acc ^= (acc >> 7); acc &= MASK
            acc ^= ((acc << 17) & MASK); acc &= MASK

        ms = int((time.time() - start) * 1000)
        return pb.Result(
            request_id=request.request_id,
            compute_ms=ms,
            data=f"node=F;acc=0x{acc:016x};".encode()
        )
    
    def InitQuery(self, request, ctx):
        print(f"F.InitQuery() received: [{request.key_min}, {request.key_max}]", flush=True)
        
        # Register cancellation
        self.cancel_registry.register(request.request_id, request.cancellation_token)
        
        # Check cache
        cache_key = f"query:{request.key_min}-{request.key_max}:0"
        cached = self.cache.get(cache_key)
        if cached:
            print(f"F: Cache HIT for {cache_key}", flush=True)
            chunk = pb.Chunk()
            chunk.ParseFromString(cached)
            return chunk
        
        # Check if cancelled
        if self.cancel_registry.is_cancelled(request.request_id):
            ctx.abort(grpc.StatusCode.CANCELLED, "request cancelled")
        
        # Query dataset
        start = time.time()
        results = self.dataset.query_range(request.key_min, request.key_max, chunk_size=100)
        ms = int((time.time() - start) * 1000)
        
        if not results:
            # No data in range
            chunk = pb.Chunk(
                request_id=request.request_id,
                chunk_index=0,
                is_final=True,
                total_chunks=1,
                data=pb.KVResult().SerializeToString(),
                compute_ms=ms
            )
        else:
            # Return first chunk (simplified single-chunk implementation)
            chunk = pb.Chunk(
                request_id=request.request_id,
                chunk_index=0,
                is_final=True,
                total_chunks=1,
                data=results[0].SerializeToString(),
                compute_ms=ms
            )
            print(f"F: Computed chunk 0, {len(results[0].pairs)} pairs, sum={results[0].partial_sum}", flush=True)
        
        # Cache result
        self.cache.put(cache_key, chunk.SerializeToString())
        return chunk
    
    def GetChunk(self, request, ctx):
        if request.chunk_index != 0:
            ctx.abort(grpc.StatusCode.OUT_OF_RANGE, "chunk index out of range")
        ctx.abort(grpc.StatusCode.UNIMPLEMENTED, "GetChunk not fully implemented")
    
    def Cancel(self, request, ctx):
        success = self.cancel_registry.cancel(request.request_id, request.cancellation_token)
        return pb.CancelReply(
            cancelled=success,
            message="cancelled" if success else "not found"
        )


if __name__ == "__main__":
    topo = os.environ["TOPOLOGY_FILE"]
    me, nbrs = load_topology(topo, "F")

    server = grpc.server(ThreadPoolExecutor(max_workers=8))
    rpc.add_BasecampServicer_to_server(WorkerService(me, nbrs), server)

    bound_v4 = server.add_insecure_port(f"{me.host}:{me.port}")
    bound_v6 = server.add_insecure_port(f"[::]:{me.port}")

    if bound_v4 == 0 and bound_v6 == 0:
        raise RuntimeError(f"Failed to bind F on {me.host}:{me.port}")

    print(f"F listening (v4={bool(bound_v4)}, v6={bool(bound_v6)}) on {me.host}:{me.port}", flush=True)
    server.start()
    server.wait_for_termination()
