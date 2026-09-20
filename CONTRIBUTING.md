# Contributing

Thank you for looking at LISA. The project is pre-1.0 and moving fast.

## What helps most right now

- **Bug reports.** What you did, what happened, what you expected,
  `lisa --version`, and the relevant lines from `<data>/lisa.log`. If a
  document was not read correctly, say what kind of file it was — a
  small example file helps most.
- **Answer quality reports.** The question, the answer, and which
  document should have answered it. These drive the evaluation set.
- **Platform reports.** LISA is built for Apple Silicon Macs today;
  Linux and Windows are planned.

## Code contributions

We cannot merge code contributions yet. LISA is source-available under
the Business Source License 1.1, and a Contributor Licence Agreement has
to be in place before outside patches can be accepted. Until then,
please open an issue describing the change rather than sending a pull
request, so nobody's time is wasted.

## If you are working in this repository

`CLAUDE.md` is the working agreement (what to do, and what never to do);
`LISA_COMPLETION_PLAN.md` holds the plan and the decisions behind it.
In short:

- One work package at a time; finish, test, and commit before the next.
- Reuse permissively licensed libraries instead of writing new code;
  vendor them at a pinned version with an intake record.
- Correctness before speed. No performance claim without a measurement
  and the conditions it was measured under.
- Every change keeps the full test suite green in both builds:

      cmake -S . -B build && cmake --build build -j8
      ctest --test-dir build --output-on-failure
      cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLISA_SANITIZE=ON
      cmake --build build-asan -j8 && ctest --test-dir build-asan --output-on-failure

- New behaviour comes with tests; a bug fix comes with the test that
  would have caught it.
- Public interfaces (`include/lisa.h`, file formats, HTTP routes, CLI
  flags and output) change only additively, with a version bump and a
  note in the work package's report.

## Code style

C11. Four-space indent. A block comment above each function saying what
it guarantees and who owns what. Sizes and counts are `int64_t`. Every
OS call goes through `src/platform/`. New files start with
`/* SPDX-License-Identifier: BUSL-1.1 */`.
