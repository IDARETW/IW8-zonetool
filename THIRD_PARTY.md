# Third-party software

## JSON for Modern C++

`src/common/json.hpp` is JSON for Modern C++ 3.11.2 by Niels Lohmann and contributors. It is used
under the MIT License. The source file retains its copyright and SPDX license notices.

- Project: https://github.com/nlohmann/json
- License: https://github.com/nlohmann/json/blob/v3.11.2/LICENSE.MIT

## OpenAssetTools extension

The IW3 exporters iw8-zonetool reads (world, collision, light grid, static-model lighting inputs,
water spectrum, primary-light regions, map entities) live in a fork of OpenAssetTools:
https://github.com/IDARETW/OpenAssetTools, `main` (the export is commit `1b7fa28`, on upstream
`f556729`). Build its `Unlinker.exe` (Release, Win32) and pass it with `--unlinker`.
`tools/oat/ReplayMapDumpers.h` is a copy of the fork's main exporter header for reference. Both are
distributed under GPL-3.0; the license text is stored beside the header.

## LZ4

`src/thirdparty/lz4` contains LZ4 by Yann Collet and contributors. It is used under the BSD
2-Clause License; the license text is stored in that directory.

- Project: https://github.com/lz4/lz4
- License: https://github.com/lz4/lz4/blob/dev/LICENSE
