---
name: prioritize-hft-performance
description: Prioritize low-latency design decisions for account_services. Use when creating or modifying latency-sensitive C/C++ code, data structures, shared memory layouts, gateway adapters, event loops, order routing, portfolio or risk hot paths, serialization, or concurrency in this high-frequency trading system. Require explicit consideration of lock-free or single-writer coordination, zero-copy data flow, cache-aware layout and alignment, templates, and zero-overhead abstractions before accepting slower or more allocation-heavy designs.
---

# Prioritize HFT Performance

Apply these rules before finalizing changes that can affect trading-path latency or jitter.

## Load System Context

1. Read `docs/cpp_naming_baseline.md` before introducing new symbols.
2. Treat code in `src/order/`, `src/risk/`, `src/shm/`, `src/core/`, `gateway/src/`, and latency-sensitive APIs as hot until you can prove the path is cold.
3. Separate data-plane work (per order, per fill, per market event) from control-plane work (startup, config, tooling, offline diagnostics).
4. Combine this skill with `enforce-cpp-raii` for ownership and `enforce-project-naming` for new symbols when those skills apply.

## Make Design Decisions in This Order

1. Preserve correctness, ABI expectations, and deterministic behavior.
2. Remove sharing, blocking, and cross-thread contention from the hot path.
3. Remove copies, allocations, and format conversions.
4. Shape data for cache locality and predictable access.
5. Move branching, validation, and polymorphism to compile time or setup time.
6. Accept a more general abstraction only when its runtime cost is negligible and justified.

## Prefer Lock-Free or Single-Writer Coordination

1. Prefer thread ownership, partitioning, SPSC or MPSC queues, and sequence counters before adding mutexes.
2. Use atomics with explicit `memory_order`; keep `seq_cst` as an exception, not a default.
3. Keep one writer per mutable cache line when practical.
4. Keep blocking waits, condition variables, sleeps, and control-plane handshakes off the trading path.
5. If a lock is unavoidable, prove that the code is cold, bounded, and outside the per-event loop.

## Prefer Zero-Copy Data Flow

1. Pass views such as `std::span` and `std::string_view` or references instead of owning containers in hot code.
2. Write directly into final network, shared-memory, or output buffers when safe.
3. Reuse preallocated storage and reserve capacity before the hot loop starts.
4. Avoid per-event temporary `std::string`, `std::vector`, JSON translation, or staging buffers in critical paths.
5. Keep serialization and mapping one-pass and avoid bounce buffers.

## Prefer Cache-Aware Layout

1. Keep hot fields contiguous and split cold metadata, logging state, and diagnostics away from them.
2. Prefer fixed-size or bounded data structures for hot-state tables when feasible.
3. Prevent false sharing with `alignas(64)` or `std::hardware_destructive_interference_size` when supported.
4. Prefer flat contiguous storage and structure-of-arrays when scanning or batching dominates.
5. Revisit field order, padding, and object size whenever a structure is shared or frequently touched.

## Prefer Compile-Time and Zero-Overhead Abstractions

1. Use templates, `constexpr`, policy types, and static dispatch when behavior varies by venue, adapter, or order policy.
2. Prefer zero-cost wrappers over virtual interfaces or heap-based type erasure on per-message paths.
3. Keep abstraction cost visible in generated code; avoid hidden allocations and indirect calls.
4. Use `std::function`, `std::any`, `std::shared_ptr`, RTTI, and exceptions on hot paths only with explicit justification.
5. Balance template use against code size; extract only cold or shared logic when duplication harms instruction cache more than dispatch would.

## Measure and Justify

1. State whether the change affects startup, control plane, or per-event latency.
2. Add or run focused tests or benchmarks when hot-path behavior, data layout, or concurrency changes.
3. Inspect copies, allocations, lock contention, and cache-line sharing instead of relying on intuition.
4. Document deliberate tradeoffs between readability, code size, and latency.

## Load Detailed References Only When Needed

- Read `references/hft-design-checklist.md` when you need deeper prompts for atomics, memory layout, ownership transfer, or hot-path API reviews.
- Read `references/hft-book-chapter-guide.md` when you need to choose the right chapter from the bundled HFT book before opening the PDF.

## Pre-Final Checklist

1. Verify the change does not add locks, waits, syscalls, or unbounded work to the trading path.
2. Verify the change does not add avoidable copies, temporary allocations, or formatting work.
3. Verify shared state is aligned, padded, and partitioned to minimize false sharing.
4. Verify abstraction choices compile to direct code on hot paths or are clearly isolated to cold paths.
5. Flag any unavoidable latency cost and explain the reason for the deviation.
