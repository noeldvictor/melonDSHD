<p align="center"><img src="res/icon/melon_128x128.png"></p>
<h2 align="center"><b>melonDS HD -AN UNOFFICIAL FORK of MELONDS</b></h2>

> IMPORTANT: Unofficial, experimental fork
>
> - Please do not contact the original melonDS team about issues with this fork. Use this repository’s issue tracker instead.
> - This is a very rough, experimental side project focused on HD features. Expect breakage and rapid changes; it may stall at any time. No promises of future development or features.
> - I’m probably too busy to keep up with it and will not be responsive to issues or support requests.
> - If someone else wants to take over or finish it, great,  contributions are welcome.

Unofficial HD-focused fork of melonDS. This project is not affiliated with or endorsed by the original melonDS team. GitHub: https://github.com/noeldvictor/melonDSHD

The goal is take melonds and add the ability to play games with HD textures and sprites(one day). This is a very rough, experimental side project focused on HD features. Expect breakage and rapid changes; it may stall at any time.

<hr>

## How to use

Firmware boot (not direct boot) requires a BIOS/firmware dump from an original DS or DS Lite.
DS firmwares dumped from a DSi or 3DS aren't bootable and only contain configuration data, thus they are only suitable when booting games directly.

### Possible firmware sizes

 * 128KB: DSi/3DS DS-mode firmware (reduced size due to lacking bootcode)
 * 256KB: regular DS firmware
 * 512KB: iQue DS firmware

DS BIOS dumps from a DSi or 3DS can be used with no compatibility issues. DSi BIOS dumps (in DSi mode) are not compatible. Or maybe they are. I don't know.

As for the rest, the interface should be pretty straightforward. If you have a question, don't hesitate to ask, though!

## How to build
See [BUILD.md](./BUILD.md) for build instructions. Standard dependencies apply; binary name is `melonDSHD`.

## TODO LIST
* Sprite Texture Dumping
* HD Sprite Replacement


## Credits  
**The original melonDS team for the emulator itself**

 * Martin for GBAtek, a good piece of documentation
 * Cydrak for the extra 3D GPU research
 * limittox for the icon
 * All of you comrades who have been testing melonDS/melonDSHD, reporting issues, suggesting improvements, etc

## Licenses

[![GNU GPLv3 Image](https://www.gnu.org/graphics/gplv3-127x51.png)](http://www.gnu.org/licenses/gpl-3.0.en.html)

melonDS (and this fork) is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

### External
* Images used in the Input Config Dialog - see `src/frontend/qt_sdl/InputConfig/resources/LICENSE.md`
