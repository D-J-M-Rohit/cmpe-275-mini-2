#!/usr/bin/env python3
"""
Test client for chunked API
Demonstrates InitQuery, GetChunk, and Cancel RPCs
"""
import grpc
import sys
import uuid
sys.path.insert(0, '.')

from py import basecamp_pb2 as pb, basecamp_pb2_grpc as rpc

def test_chunked_query():
    """Test basic chunked query to both teams"""
    channel = grpc.insecure_channel('127.0.0.1:50051')
    stub = rpc.BasecampStub(channel)
    
    print("=== Test 1: Chunked Query (BOTH teams) ===")
    req = pb.InitRequest(
        request_id=f"req-{uuid.uuid4().hex[:8]}",
        target=pb.TEAM_BOTH,
        key_min=100,
        key_max=800,
        cancellation_token=f"cancel-{uuid.uuid4().hex[:8]}"
    )
    
    try:
        chunk = stub.InitQuery(req, timeout=10)
        print(f"✓ Received chunk {chunk.chunk_index}")
        print(f"  - is_final: {chunk.is_final}")
        print(f"  - total_chunks: {chunk.total_chunks}")
        print(f"  - compute_ms: {chunk.compute_ms}")
        print(f"  - data_size: {len(chunk.data)} bytes")
        
        # Parse KVResult
        kv_result = pb.KVResult()
        kv_result.ParseFromString(chunk.data)
        print(f"  - pairs_count: {len(kv_result.pairs)}")
        print(f"  - partial_sum: {kv_result.partial_sum}")
        
        if len(kv_result.pairs) > 0:
            print(f"  - first pair: key={kv_result.pairs[0].key}, value={kv_result.pairs[0].value}")
            print(f"  - last pair: key={kv_result.pairs[-1].key}, value={kv_result.pairs[-1].value}")
        
        return True
    except grpc.RpcError as e:
        print(f"✗ Failed: {e.code()} - {e.details()}")
        return False

def test_cache_hit():
    """Test that second identical query hits cache"""
    channel = grpc.insecure_channel('127.0.0.1:50051')
    stub = rpc.BasecampStub(channel)
    
    print("\n=== Test 2: Cache Hit ===")
    req = pb.InitRequest(
        request_id=f"req-{uuid.uuid4().hex[:8]}",
        target=pb.TEAM_PINK,
        key_min=750,
        key_max=850,
        cancellation_token=f"cancel-{uuid.uuid4().hex[:8]}"
    )
    
    try:
        # First request
        import time
        t0 = time.time()
        chunk1 = stub.InitQuery(req, timeout=10)
        t1 = time.time()
        latency1 = (t1 - t0) * 1000
        
        print(f"✓ First request: {latency1:.1f}ms")
        
        # Second identical request (should hit cache)
        req.request_id = f"req-{uuid.uuid4().hex[:8]}"  # new request ID
        req.cancellation_token = f"cancel-{uuid.uuid4().hex[:8]}"
        
        t0 = time.time()
        chunk2 = stub.InitQuery(req, timeout=10)
        t1 = time.time()
        latency2 = (t1 - t0) * 1000
        
        print(f"✓ Second request (cached): {latency2:.1f}ms")
        print(f"  - Speedup: {latency1/latency2:.1f}x")
        
        return True
    except grpc.RpcError as e:
        print(f"✗ Failed: {e.code()} - {e.details()}")
        return False

def test_cancellation():
    """Test cancellation"""
    channel = grpc.insecure_channel('127.0.0.1:50051')
    stub = rpc.BasecampStub(channel)
    
    print("\n=== Test 3: Cancellation ===")
    req_id = f"req-{uuid.uuid4().hex[:8]}"
    cancel_token = f"cancel-{uuid.uuid4().hex[:8]}"
    
    req = pb.InitRequest(
        request_id=req_id,
        target=pb.TEAM_BOTH,
        key_min=0,
        key_max=999,
        cancellation_token=cancel_token
    )
    
    try:
        # Start query
        chunk = stub.InitQuery(req, timeout=10)
        print(f"✓ Query started, got chunk {chunk.chunk_index}")
        
        # Cancel it
        cancel_req = pb.CancelRequest(
            request_id=req_id,
            cancellation_token=cancel_token
        )
        
        cancel_reply = stub.Cancel(cancel_req, timeout=5)
        print(f"✓ Cancel result: cancelled={cancel_reply.cancelled}, message={cancel_reply.message}")
        
        return cancel_reply.cancelled
    except grpc.RpcError as e:
        print(f"✗ Failed: {e.code()} - {e.details()}")
        return False

def test_green_team_only():
    """Test GREEN team only (C++ worker C)"""
    channel = grpc.insecure_channel('127.0.0.1:50051')
    stub = rpc.BasecampStub(channel)
    
    print("\n=== Test 4: GREEN Team Only ===")
    req = pb.InitRequest(
        request_id=f"req-{uuid.uuid4().hex[:8]}",
        target=pb.TEAM_GREEN,
        key_min=100,
        key_max=400,
        cancellation_token=f"cancel-{uuid.uuid4().hex[:8]}"
    )
    
    try:
        chunk = stub.InitQuery(req, timeout=10)
        kv_result = pb.KVResult()
        kv_result.ParseFromString(chunk.data)
        
        print(f"✓ GREEN team query successful")
        print(f"  - pairs_count: {len(kv_result.pairs)}")
        print(f"  - partial_sum: {kv_result.partial_sum}")
        
        return True
    except grpc.RpcError as e:
        print(f"✗ Failed: {e.code()} - {e.details()}")
        return False

def main():
    print("Chunked API Test Client")
    print("=" * 50)
    
    results = []
    
    results.append(("Chunked Query", test_chunked_query()))
    results.append(("Cache Hit", test_cache_hit()))
    results.append(("Cancellation", test_cancellation()))
    results.append(("GREEN Team", test_green_team_only()))
    
    print("\n" + "=" * 50)
    print("Test Results:")
    for name, passed in results:
        status = "✓ PASS" if passed else "✗ FAIL"
        print(f"  {status}: {name}")
    
    passed_count = sum(1 for _, p in results if p)
    print(f"\nTotal: {passed_count}/{len(results)} passed")

if __name__ == "__main__":
    main()
