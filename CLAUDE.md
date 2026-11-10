# CLAUDE.md

## Never freeze the owner's computer

The owner uses this machine while you work. A command that takes every core makes it unusable,
and that is worse than any wait. **Slower is always acceptable; a frozen desktop never is.**

- **Builds:** never a bare `-j` (it means unlimited). Always `nice -n 19 cmake --build --preset <p> -j 2`.
  The first build of each preset compiles Drogon too and takes several minutes: run it in the
  background.
- **Server tests:** `nice -n 19 ctest --preset <p> -j 1`, or one binary filtered by tag while
  iterating: `nice -n 19 ./build/server/tests/app_tests "[spike]"`. The whole suite once at the end.
- **One heavy command at a time.** Never build or test several presets in one command or back to
  back without being asked. Build and test the other presets (`release`, `clang`) only when the
  owner asks, or once before a PR, one preset per command.
- **Docker builds:** `nice -n 19 docker build …`, `JOBS` at most 2, one image build at a time,
  `docker compose build server` before the page.
- **Sanitizer builds (`asan`, `tsan`), the chaos test and the load test** only when the owner asks,
  one run per command. Don't loop them.
- **Long jobs go in the background** (`run_in_background`), still under `nice` and `-j 2`, so the
  owner can keep working and can stop them.

## Comments

- A comment says why the code is the way it is, when the code cannot say it. One to three lines.
- Never mention phases, plan sections, PR numbers, review findings, dates or who decided something.
  That is history, and history goes in the commit message.
- Never describe code that was removed or what the code used to do.
- No capitals for emphasis. Keep the warnings that prevent a crash, a deadlock or a hang, and make
  them short.
- Say a reason once, next to the code it explains. Don't send the reader to another function.

This applies to every comment you write or change. Existing comments that break it are cleaned up
in PRs of their own, one area at a time, not mixed into unrelated changes.

## Branch workflow

Branches are stacked: `master` -> the parent branch -> `web/phase-N-<name>`. The parent branch is
currently `ccr-c8ac042f-e6e18n`.

Phase branches are reviewed and PR'd against the parent branch, never `master`. When running
`/code-review` on a phase branch, diff against the parent, or pass the PR number so GitHub's base
branch is used. The review procedure, and when a phase branch is merged, are in section 13 of
`docs/server-plan.md`.

## Changing a PR: never `gh pr edit`

`gh pr edit` fails on the owner's repositories, with "GraphQL: Projects (classic) is being
deprecated …", and changes nothing. Don't try it first. Change a PR's description or title through
the REST API from the start, with the body written to a file:

```
gh api -X PATCH repos/conectionist/romanian_whist_server/pulls/<N> -F body=@<file>
gh api -X PATCH repos/conectionist/romanian_whist_server/pulls/<N> -f title='<title>'
```

`gh pr view` and `gh pr create` work as usual.
