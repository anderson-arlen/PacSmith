# Agent instructions

## File size

When a file exceeds 1500 lines, consider splitting it into multiple files, but only when that split is a real improvement (clearer boundaries, independent responsibilities, easier navigation). Do not break up a single class just because it is large — a 2k-line class that is one cohesive unit should stay together.

Hard limit: no file may exceed 3000 lines.

## Comments

Do not comment what the code already says. When the reason for a chunk of code is not obvious, add a comment that explains **why** it is written that way — for example niche business logic or a bug workaround.

## Database access

Use queries declared in `server/internal/sqlite/queries` and SQLC-generated bindings for application and test database access. Do not add raw SQL to application or test code. Raw SQL is limited to the SQLite bootstrap and migration implementation where SQLC cannot be used.
