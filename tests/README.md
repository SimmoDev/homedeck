# tests/

Test suites for HomeDeck: GoogleTest+GoogleMock unit tests, run against the
[simulator](../simulator/) build (a separate host-native CMake project, not
ESP-IDF). No separate on-target test framework is used — see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#5-test-framework)
for why.

No tests exist yet. See
[DEVELOPMENT.md](../DEVELOPMENT.md#buildtest-workflow) for the intended
workflow.
