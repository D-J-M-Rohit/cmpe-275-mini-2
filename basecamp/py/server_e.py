import os, time, threading, grpc
from concurrent.futures import ThreadPoolExecutor
from google.protobuf import text_format
from py import basecamp_pb2 as pb, basecamp_pb2_grpc as rpc, topology_pb2 as tpb
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

class Service(rpc.BasecampServicer):
    def __init__(self, me, nbrs):
        self.me, self.nbrs = me, nbrs
        self._lock = threading.Lock()
        self._inflight = 0
        self.stubs = {
            name: rpc.BasecampStub(grpc.insecure_channel(f"{n.host}:{n.port}"))
            for name, n in nbrs.items()
        }

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

            # Optional one-hop inside team (E -> F)
            nbr = next((n for n in self.nbrs.values()
                        if n.team == self.me.team and n.name != self.me.name), None)
            if nbr and nbr.name in self.stubs:
                try:
                    sub = self.stubs[nbr.name].Handle(request)
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
