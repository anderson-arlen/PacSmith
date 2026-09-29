# Agent instructions

## Command output

Command output may be redirected to keep session context concise. Use a fixed, predictable log path based on the project and command, and reuse that exact path on every run (for example, `make test` uses `/tmp/pacsmith-make-test.log`). Do not use timestamps, random suffixes, task-specific names, or changing filenames. Keep the command invocation stable so an approval can be reused. When requesting approval, offer a narrowly scoped reusable rule that includes the fixed redirect when the approval system requires it. Avoid concurrent runs that write to the same log.

## File size

When a file exceeds 1500 lines, consider splitting it into multiple files, but only when that split is a real improvement (clearer boundaries, independent responsibilities, easier navigation). Do not break up a single class just because it is large — a 2k-line class that is one cohesive unit should stay together.

Hard limit: no file may exceed 3000 lines.

## Comments

Do not comment what the code already says. When the reason for a chunk of code is not obvious, add a comment that explains **why** it is written that way — for example niche business logic or a bug workaround.

## Database access

Use queries declared in `server/internal/sqlite/queries` and SQLC-generated bindings for application and test database access. Do not add raw SQL to application or test code. Raw SQL is limited to the SQLite bootstrap and migration implementation where SQLC cannot be used.
