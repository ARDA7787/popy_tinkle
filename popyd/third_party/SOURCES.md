# Vendored third-party headers

Single-header, permissively-licensed C/C++ libraries copied verbatim from upstream.
To audit: re-run `shasum -a 256` on each file and compare to the hashes below.

| File                  | Upstream                                                                                          | Version / commit                                | License        | SHA-256                                                            |
|-----------------------|---------------------------------------------------------------------------------------------------|-------------------------------------------------|----------------|--------------------------------------------------------------------|
| `picosha2.h`          | https://github.com/okdshin/PicoSHA2                                                               | commit `27fcf6979298949e8a462e16d09a0351c18fcaf2` | MIT            | `8f183eaae529cd9d6a3d4843c7559e2a3e3d68b6caaa223e7c24c3c899b3d988` |
| `nlohmann_json.hpp`   | https://github.com/nlohmann/json/releases/tag/v3.11.3 (asset `json.hpp`)                          | v3.11.3                                         | MIT            | `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6` |
| `stb_image.h`         | https://github.com/nothings/stb                                                                   | commit `5c205738c191bcb0abc65c4febfa9bd25ff35234` | MIT / public domain | `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3` |
| `stb_image_write.h`   | https://github.com/nothings/stb                                                                   | commit `5c205738c191bcb0abc65c4febfa9bd25ff35234` | MIT / public domain | `cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05` |
| `toml.hpp`            | https://github.com/marzer/tomlplusplus                                                            | tag `v3.4.0`                                    | MIT            | `6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e` |

## Why each is here

- **picosha2** — streaming SHA-256 (chunk-by-chunk hashing inside the libcurl write callback
  and the watcher's copy loop). Mirrors the browser extension's `hash-wasm` path in
  `src/offscreen/offscreen.ts`.
- **nlohmann/json** — sidecar (`<name>_popy.meta.json`) parse and emit. Mirrors
  `QuarantineRecord` in `src/lib/types/index.ts` field-for-field.
- **stb_image / stb_image_write** — decode and re-encode PNG/JPEG inside the sandboxed
  `popy-render` child. Re-encoding through stb strips EXIF, ICC, and ancillary metadata
  chunks, mirroring the browser's behaviour.
- **toml++** — parse `~/.config/popy/config.toml`.

## Updating

1. Download the new file from the upstream URL above.
2. Re-run `shasum -a 256` and update this table.
3. Run the smoke build (`cmake --build build && ctest --test-dir build`).
4. Commit.
