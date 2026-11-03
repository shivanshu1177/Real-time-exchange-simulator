# Order memory: one pool per engine, single owner

## Decision
Every matching engine owns a `MemoryPool<Order>` (`include/rtes/memory_pool.hpp`) sized by
`performance.order_pool_size` (orders per symbol). Only the engine's thread acquires and releases slots.
The gateway never holds an `Order*`: a new order crosses to the engine by value inside `OrderRequest`, and
the engine copies it into a slot from its own pool.

## Why
The first version had one pool shared by the gateway (acquire) and every engine (release), with a lock-free
Treiber-stack free list. That stack had no ABA protection. It was correct only while exactly one thread
acquired, and nothing enforced that. The two options were:

1. **Single owner per pool** (chosen). No atomics, no CAS, nothing to prove. The pool is a plain free-index
   stack; acquire and release are a few instructions. Debug builds assert that one thread uses the pool and
   that a slot is never released twice (`tests/test_memory_pool.cpp` has death tests for both).
2. A correct lock-free stack (tagged pointers or index+generation, 128-bit CAS). More code, and it would still
   need a multi-threaded stress test under TSan to be believed, for a pattern the design does not need.

## Consequences
- An order's slot lives on the engine thread from arrival to fill or cancel. Execution reports carry copies of
  the fields, never pointers.
- If a symbol's pool (or its book's id table, same size) is full, the engine rejects the new order with
  `SYSTEM_BUSY`, and the gateway releases the risk it reserved.
- Memory is fixed at start-up. With the default 262,144 orders per symbol, each engine allocates the pool
  (77 bytes per order: the `Order` plus the free-list bookkeeping) and the book's id table (24 bytes per slot,
  2 slots per order): 32.8 MB per symbol. The two 65,536-entry queues between gateway and engine add 7.9 MB.
- The type no longer claims to be thread-safe. Its comment says it is single-owner.
