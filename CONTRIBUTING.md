# Contributing to Transnalysis

Transnalysis is a small, actively-used research tool, so the guidelines
here are intentionally lightweight.

## Building and testing

See the [README](README.md#building) for build prerequisites and steps.
After configuring with CMake, the unit test suite can be run with:

```bash
ctest --test-dir build --output-on-failure
```

CI runs the same build + test steps on every push and pull request against
`main` (see `.github/workflows/build.yml`).

## Code organization

- UI and Qt widget code lives in `mainwindow.*` and `plotwindow.*`.
- UI-independent numerical routines (interpolation, averaging,
  symmetrization) live in `transportmath.h/.cpp` and are covered by unit
  tests in `tests/`. New calculations that don't need a Qt model or widget
  belong here, not in `MainWindow`, so they stay testable.

## Reporting bugs / requesting features

Please open a GitHub Issue using the provided templates. For a bug report,
include the Qt version, OS, and, if possible, a small sample data file (or a
snippet) that reproduces the issue.

## Pull requests

- Keep PRs focused on one change (a bug fix, a feature, or a refactor) --
  please avoid mixing unrelated changes in one PR.
- Add or update a unit test in `tests/` for any change to `transportmath.*`.
- Make sure `ctest` passes locally before opening the PR; CI will also run
  it automatically.
- Write commit messages that explain *why*, not just *what*.

## License

By contributing, you agree that your contributions will be licensed under
the project's [Apache License 2.0](LICENSE).
