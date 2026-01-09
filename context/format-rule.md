# Code

- Only up to C++20
- APIs start small, widen deliberately
- `detail::` is internal, never public API
- Layout: `include/` (public), `src/` (impl + private), `test/`
- RAII scope guards over try-catch; dismissible for commit/rollback
- order: dtor, ctors, assigns, compares
- Boost/snake_case naming
- 4-space indent, Allman braces
- Trailing return type when return type is verbose
- function name on new line
- `auto` for `detail` namespace or private return types
