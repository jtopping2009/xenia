# Patches for submodules

`ffmpeg-wma-decoders.patch` turns on FFmpeg's WMA (v1 and v2) decoders,
which the title music player needs for games whose soundtracks are WMA
files. Apply it to the FFmpeg submodule before building:

```
git -C third_party/FFmpeg apply ../../patches/ffmpeg-wma-decoders.patch
```
