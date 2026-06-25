# Project Coding and Safety Rules

## Strict C Linter Requirements
1. **Linting Required:** All modifications and new code in C files (both userspace code like SDK, CLI tools, and game mock, and kernel module code) **MUST** pass static analysis without warnings/errors using the project's strict `clang-tidy` profile.
2. **Treat Warnings as Errors:** The linter is configured with `WarningsAsErrors: '*'` to block compilation if any security issues, logic flaws, or CERT secure coding standard violations are present.
3. **Execution command:** To lint the entire C codebase:
   ```bash
   make lint
   ```
4. **Disallowed Vulnerabilities:**
   - Any form of buffer overflow, buffer underflow, or unaligned memory access (enforced by `clang-analyzer-security.ArrayBound` and `clang-analyzer-security.PointerSub`).
   - Use of unchecked numeric conversion functions (like `atoi` or `sscanf` without error validation). Use safer alternatives (like `strtol` or `strtoull`) as required by the `cert-err34-c` check.
   - Ignoring function return values for critical calls (like `fprintf` or system APIs). Explicitly cast ignored returns to `(void)` to suppress warnings when safe to do so.
