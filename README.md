# SeamlessSkyrim

An SKSE plugin for Skyrim SE and AE: the space behind a load door is seen through its open
doorway, and the player walks through without a loading screen.

https://www.nexusmods.com/skyrimspecialedition/mods/199801

With Community Shaders it needs a patched Community Shaders 1.9.1, whose source is at
https://github.com/OneManMonkeySquad/CommunityShaders-Seamless (branch `seamless-portal`).

Building: `docs/BUILD.md`.

## Compatibility
Faster Objects - Compatible, recommended
Load Screens Hider - Compatible, recommended
Community Shaders - Compatible
FSMP - Faster HDT-SMP - Compatible
Precision - Compatible
Faster Loadscreens - Compatible
Faster Decompression - Compatible

## Translating the in-game menu

The text of the SKSE Menu Framework menu is in `SKSE\Plugins\SeamlessSkyrim_Translations\english.json`.
Copy it to a file named after the game's language (`sLanguage` in Skyrim.ini, lower case:
`german.json`, `french.json`, `russian.json`, ...) and translate the values, not the keys. Keep
every `{}`: a number or name goes there. What the file lacks, or gets wrong, stays in English and
is named in `SeamlessSkyrim.log`.

## License

GPL-3.0-or-later: SeamlessSkyrim is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version. See `LICENSE`.

The plugin is built with code under other licenses, each kept with that code: CommonLibSSE NG
(GPL-3.0-or-later with exceptions, parts MIT and BSD) and SKSE Menu Framework (MIT) in `extern/`,
and through vcpkg DirectXMath, DirectXTK, fmt, nlohmann/json, spdlog (MIT), rapidcsv and xbyak
(BSD-3-Clause). The release archive's `THIRD-PARTY-NOTICES.txt` holds all their notices, and
those of the libraries in the patched Community Shaders.