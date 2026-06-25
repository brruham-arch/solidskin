# libsolidskin

AML mod untuk GTA SA Android — override warna material skin player menjadi solid color via OpenGL ES 2.0 hook.

## Cara kerja

Hook `glUseProgram` dan `glUniform4fv` di `libGTASA.so` via GOT patching.  
Saat `MaterialDiffuse` uniform di-set ke shader, nilainya diganti dengan warna solid pilihan.

## Offset GOT yang dipakai (libGTASA.so)

| Function | GOT offset |
|----------|------------|
| `glUniform4fv` | `0x673c98` |
| `glUseProgram` | `0x6746ec` |
| `glGetUniformLocation` | `0x6755ec` |

> Offset ini hasil analisa binary `libGTASA.so` build `Dec 31 2022`.  
> Jika versi game update, perlu re-analisa.

## Build

Via GitHub Actions (otomatis saat push), atau manual di Termux:

```bash
$NDK/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./jni/Android.mk
```

Output: `libs/arm64-v8a/libsolidskin.so`

## Install

Copy ke folder AML mods:
```bash
cp libs/arm64-v8a/libsolidskin.so /storage/emulated/0/mods/libsolidskin.so
```

## API

Mod ini expose `solidskin_api` struct untuk kontrol dari Lua via FFI:

```lua
local ffi = require "ffi"
ffi.cdef[[
    typedef struct {
        void (*enable)(void);
        void (*disable)(void);
        int  (*is_enabled)(void);
        void (*set_color)(float r, float g, float b, float a);
        void (*get_color)(float* out);
    } SolidSkinAPI;
]]
local addr = tonumber(io.open("/storage/emulated/0/solidskin_addr.txt","r"):read("*l"))
local api = ffi.cast("SolidSkinAPI*", addr)

api.enable()
api.set_color(0.0, 1.0, 0.0, 1.0) -- hijau
```

## Author

brruham
