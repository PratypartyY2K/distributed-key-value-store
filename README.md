# Distributed Key-Value Store (ABD Protocol)
> Implements linearizable replicated storage using quorum-based read and write operations.

Work in progress — Implementation begins once language is approved.

Quorum sizes:
  N = 5
  R = W = (N/2) + 1 = 3

Replica state:
  (value, timestamp, writer_id)

Blocking GET:
  Acquire locks from N → stop after R granted → read from those R → unlock all.

ABD GET:
  Phase 1: read from R replicas
  Phase 2: write-back (value, timestamp) to W replicas
