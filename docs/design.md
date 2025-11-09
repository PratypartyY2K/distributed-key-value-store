# Design
- Model: linearizable single-key register per key
- Tag ordering: (counter, client_id)
- ABD ops: Read (discover+writeback), Write (discover+propagate)
- Blocking variant: per-key exclusive lock/lease serializes ops
- Quorums: N=1 (1,1), N=3 (2,2), N=5 (3,3)
- Failure model: crash-stop replicas, lossy/delayed network

