# legacy/

Original prototype code, kept **only as reference** while the new code is written
(see `docs/02-auditoria-codigo-atual.md` and `docs/04-mvp.md`, F0.4).

- It is **not** part of the build.
- Bugs listed in the audit are intentionally **not** fixed here: each feature is rewritten
  with tests under `libs/` and `apps/`.
- `Database` was migrated to `libs/storage` and `WinError` (fixed) to `libs/platform`.
- This folder will be deleted at the end of milestone M1.
