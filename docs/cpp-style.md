# GrimVault C++ convention

GrimVault follows the Google C++ Style Guide for consistency and the C++ Core Guidelines for safety.
The repository configuration is authoritative when either guide permits alternatives.

## Project rules

- Preserve the established `snake_case` vocabulary and namespace layout.
- Use three-space indentation, attached braces, spaces before parentheses, and a 100-column limit.
- Design public APIs from their call sites. Use the shortest name that remains unambiguous.
- Prefer guards and early exits. Keep the successful path flat and functions focused.
- Express ownership with values and RAII types. Avoid owning raw pointers and manual lifetime pairs.
- Return `core::Result<T>` at recoverable I/O, parsing, authentication, and service boundaries.
- Validate untrusted data before use. Use allowlists for destinations, methods, headers, and enums.
- Default collection and diagnostic sharing to off. Server consent never overrides an explicit opt-out.
- Do not place credentials, tokens, signed URLs, OCR text, or response bodies in logs or errors.
- Do not add code comments except logical section dividers. Names, types, and control flow carry meaning.
- Add focused tests for every boundary, regression, and public contract change.

## Enforcement

Run `clang-format` with the repository configuration on every changed C++ file. CI rejects changed C++
files that are not formatted and builds all Windows test targets with MSVC warnings enabled.

Use `.clang-tidy` for local static analysis. New warnings in enabled checks must be resolved before commit.

References: [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) and
[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).
