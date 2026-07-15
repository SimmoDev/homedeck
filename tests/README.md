# tests/

Test suites for HomeDeck: GoogleTest+GoogleMock unit tests, built as their
own host-native CMake project (the same tooling approach as
[../simulator/](../simulator/), not ESP-IDF, but a separate CMake project
from it — this will need to link against wherever the shared Core library
ends up living, not simulator-specific LVGL/SDL2 code). No separate
on-target test framework is used — see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#5-test-framework)
for why.

Currently just a smoke test (`smoke_test.cpp`) proving the framework
actually builds, links, and runs — one plain `TEST()` and one
`MOCK_METHOD`/`EXPECT_CALL`-based test, since GoogleMock has its own
linking requirements distinct from GoogleTest. Real Core/module tests
arrive alongside the code they test, not before it exists.

Build and run locally:

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Runs in CI on every push/PR — see
[../.github/workflows/tests.yml](../.github/workflows/tests.yml).
