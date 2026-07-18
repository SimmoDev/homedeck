# Contributing to HomeDeck

Contributions are welcome. Check [docs/roadmap.md](docs/roadmap.md) for
the current milestone before starting — per
[CLAUDE.md](CLAUDE.md)'s scope-control philosophy, features get added in
milestone order, not opportunistically.

## Before you start

- **[CLAUDE.md](CLAUDE.md)** is the project's governing guide —
  architecture, coding standards, and scope philosophy. Significant
  architectural decisions need an ADR (see
  [docs/decisions/](docs/decisions/) for the existing ones and their
  format).
- **[DEVELOPMENT.md](DEVELOPMENT.md)** has the build/test setup for all
  three targets (simulator, firmware, unit tests) and points to whichever
  architecture docs are relevant to what you're working on — the doc set
  is large, and most of it covers milestones later than M1.
- For anything nontrivial, especially a new architectural decision or a
  scope question, open an issue or start a discussion before writing code
  — cheaper to align early than to rework later.

## Making a change

1. Branch off `main`.
2. Make the change. Update the relevant `docs/` — this project treats
   documentation as part of implementation, not an afterthought; a PR
   that changes behavior without updating the doc that describes it will
   get flagged in review. Docs describe the system as it is, not the
   process of building it — see [CLAUDE.md](CLAUDE.md#documentation).
3. Verify locally per [DEVELOPMENT.md](DEVELOPMENT.md#buildtest-workflow)
   for whichever target(s) you touched.
4. Open a PR against `main` and complete the test-plan checklist in the
   PR template.

`main` requires the three CI checks (simulator, firmware, unit tests)
passing and one approving review before a PR merges; branches are
squash-merged and deleted automatically after.

## Reporting bugs / requesting features

Open a GitHub issue. For anything security-related, see
[docs/architecture/security.md](docs/architecture/security.md) for the
project's current security posture and known gaps first.
