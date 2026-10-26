fiwic
=====

**F**ree**I**mage codec for the **W**indows **I**maging **C**omponent (WIC).  
`fiwic.dll` makes the image formats of the [FreeImage](https://freeimage.sourceforge.io)
library available to every WIC based application. Formats that Windows cannot handle by itself
(e.g. Targa, PCX, Photoshop, OpenEXR, JPEG 2000 or the Netpbm family) can then be opened and
saved by all programs that use WIC and get thumbnails in the Explorer.

```txt
+-------------+      +-----+      +-----------+      +-----------+
| application | ---> | WIC | ---> | fiwic.dll | ---> | FreeImage |
+-------------+      +-----+      +-----------+      +-----------+
```

Features
========

- one WIC decoder and, where FreeImage can write the format, one WIC encoder per file format
- no overlap with the codecs of Windows: BMP, ICO, JPEG, PNG, TIFF, GIF, DDS, JPEG XR, WebP and
  camera RAW are left to the system
- 1, 4 and 8 bit palette images, gray scale, true color with and without alpha channel,
  16 bit per channel and floating point images
- resolution, palette, ICC profile, embedded thumbnail and metadata (Exif, IPTC, XMP, ...)
  are passed on to WIC
- Explorer thumbnails for the registered extensions
- Windows Photo Viewer opens the registered extensions
- 64 bit and 32 bit version

Supported Formats
=================

The formats are taken from the loaded FreeImage library. With FreeImage 3.18.0 these are:

| Format                           | Extensions                  | Decode | Encode |
|----------------------------------|-----------------------------|:------:|:------:|
| Targa                            | `.tga` `.targa`             |   x    |   x    |
| Photoshop                        | `.psd` `.psb`               |   x    |   x    |
| OpenEXR                          | `.exr`                      |   x    |   x    |
| Radiance HDR                     | `.hdr`                      |   x    |   x    |
| Portable Float Map               | `.pfm`                      |   x    |   x    |
| JPEG 2000                        | `.jp2` `.j2k` `.j2c`        |   x    |   x    |
| JPEG Network Graphics            | `.jng`                      |   x    |   x    |
| Wireless Bitmap                  | `.wbmp` `.wap` `.wbm`       |   x    |   x    |
| X PixMap                         | `.xpm`                      |   x    |   x    |
| Netpbm bitmap, gray map, pix map | `.pbm` `.pgm` `.ppm`        |   x    |   x    |
| ZSoft PCX                        | `.pcx`                      |   x    |        |
| Sun Raster                       | `.ras`                      |   x    |        |
| Silicon Graphics                 | `.sgi` `.rgb` `.rgba` `.bw` |   x    |        |
| Kodak PhotoCD                    | `.pcd`                      |   x    |        |
| Amiga IFF                        | `.iff` `.lbm`               |   x    |        |
| Multiple-image Network Graphics  | `.mng`                      |   x    |        |
| X BitMap                         | `.xbm`                      |   x    |        |
| Commodore 64 Koala               | `.koa`                      |   x    |        |
| Dr. Halo                         | `.cut`                      |   x    |        |
| Group 3 fax                      | `.g3`                       |   x    |        |
| Macintosh PICT                   | `.pct` `.pict` `.pic`       |   x    |        |

The Netpbm family is registered twice per format, once for the ASCII and once for the binary variant.

Installation
============

Copy `fiwic.dll` and `FreeImage.dll` of the matching architecture (`bin/x64` or `bin/x32`)
into a permanent directory. Both files have to stay in the same directory.  
Register the codec with administrator rights for all users:

```sh
regsvr32 fiwic.dll
```

Or for the current user only:

```sh
regsvr32 /n /i:user fiwic.dll
```

If `regsvr32` is called without administrator rights, then it registers for the current user, too.
A 64 bit program uses the 64 bit codec and a 32 bit program the 32 bit codec. Register the one(s) you need.

The registration also makes Explorer show thumbnails for the extensions in the table above. This only
applies to extensions which do not have a thumbnail handler yet. It does not change which program opens
a file.

Windows Photo Viewer
--------------------

Windows Photo Viewer (`PhotoViewer.dll`) decodes through WIC, but only offers the file types on its
own list. If it is installed, the registration adds the extensions in the table above to it, so that
it opens them like TIFF files (it shows them with the TIFF type name and icon):

- The extension gets the Photo Viewer entry in its "Open with" list. This applies to registrations for
  all users and for the current user.
- A registration for all users also adds the extension to the file types of Photo Viewer
  (`HKLM\Software\Microsoft\Windows Photo Viewer\Capabilities\FileAssociations`, 64 bit view).
  This lets Photo Viewer be selected as default program for the extension.

Photo Viewer is never made the default program. Extensions which it already lists or offers stay
unchanged and `regsvr32 /u` removes only the entries that the registration added.
The Photos app of Windows 10 and 11 is not affected. It only takes the file types declared by its
package, which a codec cannot extend.

Remove everything again with:

```sh
regsvr32 /u fiwic.dll
```

Windows XP
----------

The 32 bit codec runs on Windows XP SP3 with WIC installed (part of .NET Framework 3.0 or the
WIC redistributable). WIC 1.0 differs from later versions:

- WIC only selects Authenticode signed codecs automatically. Sign `fiwic.dll` with a code signing
  certificate which the machine trusts before registering it. Applications can still create an
  unsigned decoder or encoder directly by its class ID.
- WIC hands a stream without file name to the decoders. Files of formats without a signature are
  therefore only recognized by content (see How It Works).
- Explorer thumbnails are not available.

Building
========

The following dependencies are given.
- C++11
- MinGW-w64 GCC (multilib for building both architectures with one compiler)
- GNU Make
- [FreeImage 3.18.0](https://freeimage.sourceforge.io) source distribution
- curl, sha256sum and unzip

Build `fiwic.dll` and `wictest.exe` for both architectures into `bin/x64` and `bin/x32`:

```sh
make
```

Use `ARCH=x64` or `ARCH=x32` to build only one of them. A debug build is created with `DEBUG=1`.
Without a multilib compiler, name the cross compiler prefixes, for example:

```sh
make PREFIX64=x86_64-w64-mingw32- PREFIX32=i686-w64-mingw32-
```

The output directory contains everything needed to install the codec, including `FreeImage.dll`.

FreeImage is built from source together with the codec. The official FreeImage binaries need Windows
Vista or newer. The build downloads `FreeImage3180.zip` into `bin` and checks its SHA-256 hash.
For an offline build, place the archive there beforehand. `make fetch` only downloads it.

Testing
=======

`wictest.exe` uses the codec through the public WIC interfaces, as any other application would.
Register `fiwic.dll` first.

```sh
wictest -list
wictest image.tga
wictest image.tga -encode copy.psd
wictest -fiwic image.tga
wictest -synth rgbaf -encode image.exr
```

- `-list` shows the registered components of this codec.
- Opening a file prints the decoder, the frames with size, pixel format, resolution, palette size,
  metadata count and a checksum over the pixels.
- `-encode` writes the first frame to a file, decodes it again and compares size and pixels. Lossy formats
  are expected to report differing pixels.
- `-fiwic` prefers the decoders of this codec.
- `-synth` encodes a generated gradient in a 16 bit or floating point pixel format
  (`gray16`, `rgb48`, `rgba16`, `grayf`, `rgbaf`) instead of a file.

How It Works
============

- **Components.** Each format has its own decoder and encoder class ID and its own container format GUID.
  They are derived from base GUIDs (see `src/fiwic.h`) and the FreeImage format index, so
  they stay the same on every machine and no table has to be kept in the registry.
  Registration reads the formats from FreeImage. Disabled plug-ins and formats owned by Windows are skipped.
- **Recognizing files.** WIC picks a decoder by the byte pattern registered for it. Patterns of the
  formats with a reliable signature are built in (`src/fiwicutil.cpp`). Targa, Koala, Dr. Halo, WBMP, G3 and
  PICT have none. Their decoders are offered every stream, sorted behind all others and accept
  a file if FreeImage recognizes its content or if the file name carries one of the extensions of the
  format and the content does not identify another format. Files of these formats that FreeImage cannot
  recognize by content (e.g. Targa files without the 2.0 footer) are therefore only decoded from streams
  with a file name.
- **Pixel formats.** Images are handed to WIC in the closest native pixel format. For encoding, the pixel
  format requested by the application is replaced by the closest one the FreeImage plug-in can write.
  WIC format converters bridge the difference.
- **Memory.** An image is read completely into memory before it is decoded.

Limitations
===========

- The encoders write a single image. Multiple frames are rejected.
- Metadata is exposed for reading but not written. Encoders do not offer options.
- Behavior of the FreeImage plug-ins applies. FreeImage does not read back palette images that it
  wrote as PSD and its Netpbm plug-in does not round trip 1 bit images unchanged.
- Only the first image of animated formats other than GIF, ICO and TIFF is decoded.
- Files larger than 2 GiB are not supported (limit of the FreeImage stream interface).
- Failures inside FreeImage are reported without detail (`E_FAIL` or `WINCODEC_ERR_BADIMAGE`).
  JPEG 2000 can fail on very small images because of the default number of resolution levels.

License
=======

See [LICENSE](LICENSE).  
FreeImage is distributed under its own license, which is part of the file.

Contributions
=============

No content contributions are accepted. Please file a bug report or feature request instead.  
This decision was made in consideration of the used license.

AI Assistance Disclaimer
========================

This software was developed with the assistance of AI-based tools.
All design decisions and final content remain the responsibility of the human author.

Users should be aware that AI-assisted code generation may introduce subtle errors,
security vulnerabilities or suboptimal patterns that standard review processes
might not catch. Independent review and thorough testing before use in production
environments is strongly recommended.
