# C++23 Error Handling and Diagnostics

Use this reference when the task involves recoverable failures, replacing
`bool` plus out-parameters, `std::expected`, `std::unexpected`,
`std::stacktrace`, or boundary-level diagnostics.

## Contents

- [Selection Guide](#selection-guide)
- [`std::expected<T, E>` Example](#stdexpectedt-e-example)
- [`std::stacktrace` Example](#stdstacktrace-example)
- [`std::unreachable()` Example](#stdunreachable-example)
- [Practical Guidance](#practical-guidance)

## Selection Guide

- Use `std::expected<T, E>` when failure is part of normal control flow and the
  caller needs a reason.
- Use `std::optional<T>` when absence is valid and no error detail is needed.
- Use exceptions when the current layer cannot recover locally or when an API
  already exposes exception semantics.
- Use `std::error_code` or a domain enum as the `E` type when interoperating
  with OS, filesystem, or networking style errors.
- Use `std::stacktrace` for diagnostics and reporting, not for branching logic.
- Use `std::unreachable()` only after exhaustive validation or a fully-covered
  switch over impossible states.

## `std::expected<T, E>` Example

```cpp
#include <charconv>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

enum class ParseError {
    kInvalidNumber,
    kOutOfRange,
    kNonPositive,
};

std::expected<int, ParseError> parse_int(std::string_view text) {
    int value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);

    if (ec == std::errc::invalid_argument || ptr != end) {
        return std::unexpected(ParseError::kInvalidNumber);
    }
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(ParseError::kOutOfRange);
    }
    return value;
}

std::expected<int, ParseError> parse_positive_lot(std::string_view text) {
    return parse_int(text).and_then([](int value) -> std::expected<int, ParseError> {
        if (value <= 0) {
            return std::unexpected(ParseError::kNonPositive);
        }
        return value;
    });
}

std::string describe_parse_error(ParseError error) {
    switch (error) {
    case ParseError::kInvalidNumber:
        return "invalid number";
    case ParseError::kOutOfRange:
        return "number out of range";
    case ParseError::kNonPositive:
        return "value must be positive";
    }
    return "unknown parse error";
}

std::expected<int, std::string> parse_lot_in_shares(std::string_view text) {
    return parse_positive_lot(text)
        .transform([](int lots) { return lots * 100; })
        .transform_error(describe_parse_error);
}
```

## `std::stacktrace` Example

```cpp
#include <exception>
#include <iostream>
#include <stacktrace>
#include <string_view>

void log_failure(std::string_view operation, const std::exception& ex) {
    std::cerr << operation << " failed: " << ex.what() << '\n';
    std::cerr << std::stacktrace::current() << '\n';
}
```

Guidance:

- Capture the stack trace at the point where you are translating or reporting
  the error.
- Avoid putting stack-trace capture on a success path or in tight low-latency
  loops.
- Verify standard library support on the deployed toolchain; compiler support
  alone does not guarantee `std::stacktrace` availability.

## `std::unreachable()` Example

```cpp
#include <utility>

enum class OrderState {
    kPending,
    kFilled,
    kCancelled,
};

const char* order_state_name(OrderState state) {
    switch (state) {
    case OrderState::kPending:
        return "pending";
    case OrderState::kFilled:
        return "filled";
    case OrderState::kCancelled:
        return "cancelled";
    }
    std::unreachable();
}
```

Reaching `std::unreachable()` is undefined behavior. Only use it when every
valid state has already been covered.

## Practical Guidance

- Prefer `expected` at parser, validation, and protocol boundaries where
  failure is expected and common.
- Prefer exceptions for constructors or setup paths that cannot produce a
  partially usable object.
- Do not mix `expected` and exceptions in the same routine failure path unless
  one layer is explicitly translating between them.
- Avoid `bool` plus output parameter APIs for new code when a value-or-error
  return type is clearer.
- Keep the error channel cheap to move and easy to pattern-match in callers.
