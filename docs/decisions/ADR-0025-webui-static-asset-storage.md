# ADR-0025: Web Management UI Static Asset Storage

## Status

Accepted. Supersedes part of [ADR-0002](ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)
(where the built bundle lives, not the Svelte/Vite framework choice
itself) and narrows [ADR-0017](ADR-0017-partition-table.md)'s description
of the `storage` partition.

## Context

[ADR-0002](ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)'s
Web Management UI frontend decision assumed the built Svelte/Vite bundle
would be served from the internal flash filesystem — the same FAT +
`wear_levelling` partition
[ADR-0012](ADR-0012-storage-tiers.md#decision-internal-flash-filesystem-choice)
uses for cached data and logs. Implementing static file serving surfaced
two problems with that, confirmed against ESP-IDF's actual build tooling
(`fatfs_create_spiflash_image`/`fatfs_create_partition_image` in
`components/fatfs/project_include.cmake`):

- The standard way to get files onto that partition writes the *entire*
  partition image on every `idf.py flash`, not just the web-asset files —
  a routine dev reflash would silently wipe whatever else lives on
  `storage` (cache, logs) alongside the bundle.
- ESP-IDF's OTA mechanism only ever updates the app partition
  (`ota_0`/`ota_1`), never `storage` — an OTA'd firmware update wouldn't
  bring a new frontend bundle with it, so backend and frontend could
  drift out of sync after an OTA update with nothing to prevent or detect
  it.

## Decision

**Options:**
- Keep the `storage` FAT partition, but flash the web-asset image only as
  an explicit separate step (not `FLASH_IN_PROJECT`) rather than on every
  `idf.py flash` — avoids the routine-reflash wipe, but doesn't solve the
  OTA drift problem, which is inherent to assets living on a partition
  the OTA mechanism doesn't touch.
- Embed the bundle directly into the firmware app image via ESP-IDF's
  `EMBED_FILES` (`idf_component_register(... EMBED_FILES ...)`), served
  from flash-mapped memory - no filesystem or partition involved.

**Decided: embed in the app image.** Frontend and backend are then always
the same OTA-versioned image and can't drift apart, and there's no
partition-wipe risk since nothing writes to `storage` as a side effect of
flashing. This narrows [ADR-0017](ADR-0017-partition-table.md)'s
description of the `storage` partition to cached data and logs only —
[ADR-0012](ADR-0012-storage-tiers.md)'s own tier description never named
the bundle specifically, so nothing there needs correcting.

The tradeoff: `EMBED_FILES` suits a small, fixed set of files well but not
Vite's default content-hashed many-chunk output. The Svelte build's
`vite.config.ts` overrides Rollup's output naming
(`entryFileNames`/`assetFileNames`) and relies on Vite's default
single-chunk output for an app this small, so the real build produces
exactly two fixed-named files (`index.html`, `app.js`), not Vite's
default many-chunk, content-hashed naming.

`ServeStaticFiles` (`src/platform/static_assets.h`/`.cpp`) is the
portable serving mechanism both targets use — firmware supplies
`EMBED_FILES`-linked flash data copied once at startup, the simulator
reads `webui/dist/` off disk once at startup (a real dev-convenience
divergence, consistent with the backend-implementation divergence
[ADR-0002](ADR-0002-technology-stack.md#3-embedded-webwebsocket-server)'s
embedded web/WebSocket server decision already accepts).

## Consequences

- The Vite build must keep producing a small, fixed set of file names
  (currently `index.html`, `app.js`) — a build change that reintroduces
  content-hashed or many-chunk output would need `EMBED_FILES`'s file
  list updated to match, or a different serving mechanism entirely.
- The Vite build (`npm ci && npm run build` in `webui/`) remains a
  separate, explicit step, not auto-invoked from `idf.py build` — see
  [DEVELOPMENT.md](../../DEVELOPMENT.md#buildtest-workflow) for the
  command.
