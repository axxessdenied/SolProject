# ADR 0003 — C++ naming conventions

**Status:** Accepted

**Date:** 2026-08-12

## Context

SolEngine and Frontiers of Sol need one compact naming system that is easy for a solo developer and multiple AI tools to apply and review consistently. Naming should distinguish types, functions, private state, constants, and preprocessor macros without encoding ownership or type information into identifiers.

## Decision

- Use `sol` as the canonical root C++ namespace.
- Use PascalCase for C++ header/source filenames and types, including classes, structs, enums, aliases, and concepts.
- Use camelCase for functions, methods, local variables, and parameters.
- Prefix private data members with `m_`, followed by camelCase.
- Use `kPascalCase` for named constants, including `constexpr` and namespace-scope constants.
- Use SCREAMING_SNAKE_CASE only for preprocessor macros.
- Preserve conventional external/library names when wrapping or implementing a mandated interface.

## Examples

```cpp
namespace sol {

inline constexpr double kStandardGravity = 9.80665;

class FlightController {
public:
    void setThrottle(double normalizedThrottle);

private:
    double m_commandedThrottle{0.0};
};

} // namespace sol
```

## Consequences

- New project-owned code and filenames follow this convention.
- Review does not request mechanical renaming of third-party APIs or generated code.
- Subsystem namespaces and public source-API documentation are owned by ADR 0006. Shared-library export macros remain excluded by ADR 0005 unless a future binary boundary is accepted.

## Validation

- Reviewers and formatting/lint tooling check new code for these conventions once implementation begins.
