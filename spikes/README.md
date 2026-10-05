# spikes/

Time-boxed experiments from Fase 0 (docs/04). **Throwaway code**: not part of the product, not
held to the production rules, built only with `-DRMW_BUILD_SPIKES=ON`. Each spike ends in an ADR
under `docs/adr/`, which is what matters.

| Spike | Folder | ADR |
|---|---|---|
| S6 — process mitigations | `s6-mitigations/` | [ADR-0002](../docs/adr/0002-s6-mitigacoes-de-processo.md) |
| S3 — native taskbar (run in Windows Sandbox only) | `s3-taskbar/` | [ADR-0003](../docs/adr/0003-s3-barra-nativa.md) |
| S5 — monitor identity (host, read-only) and wallpaper snapshot (Sandbox only) | `s5-monitors-snapshot/` | [ADR-0004](../docs/adr/0004-s5-identidade-de-monitor-e-snapshot.md) |
| S4 — live light/dark + accent via IThemeManager2 (Sandbox only) | `s4-appearance/` | [ADR-0005](../docs/adr/0005-s4-aparencia-ao-vivo.md) |
| S1 — video wallpaper (`host` safe on the host; `desktop` Sandbox only) | `s1-video/` | [ADR-0006](../docs/adr/0006-s1-wallpaper-em-video.md) |
| S2 — Slint gallery (needs `scripts/fetch-deps.ps1 -Name slint`) | `s2-ui/` | [ADR-0007](../docs/adr/0007-s2-ui-slint.md) |
| S7 — bar with interpreted plugin widgets + Luau plugin host | `s7-bar-plugins/` | [ADR-0008](../docs/adr/0008-s7-barra-e-plugins.md) |
