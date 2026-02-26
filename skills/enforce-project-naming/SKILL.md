---
name: enforce-project-naming
description: Enforce account_services naming conventions for all newly written or modified code. Use when generating or editing C/C++ source, headers, tests, or APIs where new symbols are introduced. Ensure new names follow the repository baseline in docs/cpp_naming_baseline.md and avoid adding fresh naming-style drift.
---

# Enforce Project Naming

Apply repository naming conventions before finalizing any code change.

## Load Baseline

1. Read `docs/cpp_naming_baseline.md` before writing code.
2. Use `docs/naming_rename_backlog.md` when a rename decision is ambiguous.
3. Treat baseline as source of truth for new names.

## Apply Naming Rules

For C++ internal code (`src/**`, `include/broker_api/*.hpp`):
- Use `snake_case` for namespaces, types, functions, and local variables.
- Use `snake_case_` for member fields.
- Use `kXxx` for constants.
- Use `PascalCase` for enum values.
- Use `ALL_CAPS` only for macros.

For C API / ABI surface (`include/api/*.h`, exported C symbols):
- Keep `acct_*` function names and `ACCT_*` macro style.
- Keep ABI symbol names unchanged unless explicitly requested.

For file names:
- Use `.hpp` for C++ internal headers.
- Reserve `.h` for C-facing headers.

## Handle Existing Code

1. Preserve existing public names unless user asks for renaming.
2. Do not introduce new inconsistent names in touched code.
3. When extending an existing naming family in the same file/module, stay consistent with nearby established style.

## Enforce Before Final Answer

Run this checklist before finishing:
1. Verify every new symbol matches the baseline style.
2. Verify no new `ALL_CAPS` constants were introduced outside macros.
3. Verify newly added C++ header files use `.hpp`.
4. Verify C API names keep `acct_*` and `ACCT_*` conventions.
5. Flag any unavoidable naming exception and explain why.
