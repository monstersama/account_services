# HFT Design Checklist

## Contents

1. Hot path triage
2. Lock-free and ownership rules
3. Zero-copy and allocation rules
4. Cache layout and alignment rules
5. Template and abstraction rules
6. Anti-patterns that require justification
7. Validation evidence

## Hot Path Triage

- Treat per-order, per-fill, per-market-event, gateway mapping, shared-memory publish or consume, and event-loop work as hot by default.
- Move logging, string formatting, statistics aggregation, config lookup, and recovery bookkeeping off the hot path when possible.
- Classify each new branch as one of: always hot, hot but rare, or cold. Optimize the first two; isolate the third.

## Lock-Free and Ownership Rules

- Prefer single-writer ownership over multi-writer synchronization.
- Prefer fixed-capacity ring buffers and sequence-based protocols over dynamically growing queues.
- Use explicit atomics and explicit memory orders. Default to `relaxed` for counters and `acquire/release` for publication or handoff when correct.
- Avoid compare-exchange retry loops in contended paths unless they are measurably better than ownership partitioning.
- Avoid hidden sharing through `std::shared_ptr` or cross-thread mutable containers.
- Pad or align producer and consumer cursors so unrelated threads do not fight over the same cache line.

## Zero-Copy and Allocation Rules

- Make ownership boundaries obvious. Prefer views for read-only access and explicit buffer handoff for writes.
- Serialize or map directly into the final wire or shared-memory representation when lifetimes allow it.
- Reuse scratch buffers and reserve all known capacity before entering steady-state loops.
- Avoid heap allocation, deep copies, and temporary format conversions in callbacks and adapters that run per event.
- Treat `std::function`, capturing lambdas stored in owning wrappers, and generic type erasure as suspect when they can allocate.

## Cache Layout and Alignment Rules

- Split hot and cold members into separate structs or separate storage when access patterns differ.
- Keep hot structs compact enough that a working set stays in L1 or L2 where possible.
- Prefer contiguous arrays over pointer-rich structures when iteration dominates.
- Use `alignas(64)` or `std::hardware_destructive_interference_size` for shared counters, queue cursors, and producer or consumer state.
- Review field order after each change; one extra pointer or boolean can create avoidable padding and cache waste.

## Template and Abstraction Rules

- Use templates, policy types, and `constexpr` configuration to move decisions out of the hot path.
- Prefer static polymorphism or closed sets over virtual dispatch in per-message code.
- Mark hot-path helpers `noexcept` when true so failure behavior is explicit and optimizer assumptions are easier to maintain.
- Watch instruction-cache pressure. If template expansion duplicates large cold logic, refactor only the cold portion out.
- Prefer simple inlineable wrappers over “generic” abstractions that hide branches, copies, or heap work.

## Anti-Patterns That Require Justification

- Introducing `std::mutex`, `std::condition_variable`, or blocking waits in order, gateway, or shared-memory loops.
- Allocating or resizing containers per event.
- Copying large order, position, or market-data structs when a view or handle would suffice.
- Logging, formatting, or building diagnostic strings per event.
- Adding virtual dispatch, `std::function`, RTTI checks, or exception throwing and catching in hot code.
- Sharing mutable state across threads when ownership partitioning would remove the contention.

## Validation Evidence

- Add or run targeted unit tests for concurrency invariants, layout-sensitive code, and API contracts.
- Add or run focused benchmarks when a change alters the critical path, data representation, or synchronization.
- If a slower construct is retained, record why it is acceptable: cold path only, bounded frequency, ABI constraint, or measured neutrality.
