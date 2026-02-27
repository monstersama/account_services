---
name: enforce-cpp-raii
description: Enforce RAII and engineering-grade C++ implementation quality. Use when creating or modifying C/C++ source files, headers, tests, or adapters in account_services, especially when code introduces resource acquisition/release, ownership transfer, exception paths, concurrency primitives, file/socket/mutex handles, memory management, or lifecycle-sensitive APIs. Require deterministic cleanup, explicit ownership, and strong exception safety before finalizing code.
---

# Enforce C++ RAII

Apply these rules before finalizing any C++ code change.

## Load Project Context

1. Read `docs/cpp_naming_baseline.md` before introducing new symbols.
2. Preserve public C API/ABI names in `include/api/*.h` unless the user explicitly requests renaming.
3. Keep behavior compatible with nearby module patterns unless a change request requires refactoring.

## Enforce Ownership and Lifetime

1. Model every resource with one explicit owner object whose destructor releases it.
2. Prefer automatic storage duration and value semantics where practical.
3. Use `std::unique_ptr` as the default owning pointer for dynamic resources.
4. Use `std::shared_ptr` only when shared ownership is required; break potential cycles with `std::weak_ptr`.
5. Ban raw owning pointers and manual `new/delete` in newly added logic.
6. Wrap C handles and other non-RAII resources with custom deleters.
7. Transfer ownership with move semantics, not implicit raw-pointer handoff.

## Enforce Exception and Error Safety

1. Guarantee no leaks on exceptions, early returns, or partial initialization.
2. Establish class invariants in constructors; throw if invariants cannot be established.
3. Keep destructors non-throwing.
4. Use scope-bound rollback guards for multi-step mutations that require atomic cleanup.
5. Prefer APIs that encode ownership and validity in types instead of relying on call-order discipline.

## Enforce Concurrency Safety

1. Acquire mutexes via RAII wrappers (`std::lock_guard`, `std::unique_lock`), never manual `lock/unlock` pairs.
2. Manage thread lifetime with RAII (`std::jthread` or explicit join-on-destruction wrappers).
3. Ensure every lock or handle has a scope-bounded release path in the same abstraction layer.
4. Avoid exposing raw synchronization primitives from public interfaces when a safer wrapper is possible.

## Enforce Engineering Best Practices

1. Keep resource-owning types focused on one responsibility and hide raw handles in private members.
2. Declare special member functions intentionally (`rule of zero` first; `rule of five` only when required).
3. Mark constructors `explicit` where implicit conversion is unsafe.
4. Prefer deterministic cleanup in tests and production paths; avoid test-only ownership shortcuts.
5. Add or update tests to cover construction, move/transfer, failure path cleanup, and destruction behavior.

## Pre-Final Checklist

1. Verify every acquired resource has exactly one clear ownership model.
2. Verify no manual cleanup pair was introduced where RAII can apply.
3. Verify all error and early-return paths are leak-free and deadlock-free.
4. Verify ownership semantics of each new type are reflected by its copy/move operations.
5. Verify tests cover both normal flow and cleanup-on-failure flow.
6. Flag unavoidable non-RAII code and explain the concrete constraint (ABI, third-party API, or performance boundary).
