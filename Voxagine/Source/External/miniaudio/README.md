# miniaudio

Vendored, unmodified, from https://github.com/mackron/miniaudio at tag
**0.11.25**. Choice of public domain (Unlicense) or MIT-0 — see the licence
block at the end of `miniaudio.h`.

| File | Upstream path |
|---|---|
| `miniaudio.h` | `miniaudio.h` |
| `stb_vorbis.c` | `extras/stb_vorbis.c` (stb_vorbis 1.22, public domain) |
| `miniaudio_impl.c` | **not upstream** — written for this tree, see below |

`miniaudio_impl.c` is the single implementation translation unit. It exists
because of the order miniaudio's Vorbis support has to be wired up in: Vorbis
is compiled into miniaudio only when `STB_VORBIS_INCLUDE_STB_VORBIS_H` is
already defined when `MINIAUDIO_IMPLEMENTATION` is expanded, so the stb_vorbis
*declarations* have to precede miniaudio's implementation. `stb_vorbis.c`
itself is compiled as its own C translation unit (its header-only section is
guarded by `STB_VORBIS_HEADER_ONLY`, and it carries `extern "C"` guards, so a
C++ caller links against it correctly).

**This matters: all 68 of the game's sound assets are `.ogg`.** Drop the
Vorbis wiring and every one of them fails to load, silently, because
`SoundReference::Load` reports failure the same way whether the file is absent
or the format is unsupported.

Both files are compiled as C with `-w`, like the other vendored sources.
Nothing else in the tree may include `miniaudio.h` outside
`Core/Platform/Audio/MiniaudioContext.cpp` — it is 4 MB of header and it is
the backend's private business.
