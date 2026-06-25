#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <GLES2/gl2.h>
#include <unordered_map>

#define LOG_TAG  "libsolidskin"
#define LOGFILE  "/storage/emulated/0/solidskin_log.txt"
#define EXPORT   __attribute__((visibility("default")))

// ─── Logger ──────────────────────────────────────────────────────────────────
static void logf_(const char* msg) {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}
static void logff_(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    logf_(buf);
}

// ─── State global ────────────────────────────────────────────────────────────
static int   g_enabled = 0;
static float g_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
static int   g_block_blend = 1;
static int   g_force_blendfunc = 1;
static int   g_texture_override = 1;

// ─── Solid texture state ─────────────────────────────────────────────────────
static GLuint g_solid_tex = 0;
static int    g_solid_tex_ready = 0;
static int    g_solid_tex_init_attempted = 0; // supaya cuma coba init sekali, hindari spam log kalau gagal terus
static uint8_t g_solid_tex_rgba[4] = {0, 255, 0, 255};
static int g_active_texture_unit = GL_TEXTURE0;

// ─── Program cache ────────────────────────────────────────────────────────────
struct ProgramInfo {
    GLint diffuse_loc;
    GLint ambient_loc;
    int   is_ped;
};
static std::unordered_map<GLuint, ProgramInfo> g_program_cache;

// ─── Function pointer types ───────────────────────────────────────────────────
typedef void  (*glUniform4fv_t)(GLint, GLsizei, const GLfloat*);
typedef void  (*glUseProgram_t)(GLuint);
typedef GLint (*glGetUniformLocation_t)(GLuint, const char*);
typedef void  (*glEnable_t)(GLenum);
typedef void  (*glDisable_t)(GLenum);
typedef void  (*glBlendFunc_t)(GLenum, GLenum);
typedef void  (*glDrawElements_t)(GLenum, GLsizei, GLenum, const void*);
typedef void  (*glDrawArrays_t)(GLenum, GLint, GLsizei);
typedef void  (*glBindTexture_t)(GLenum, GLuint);
typedef void  (*glActiveTexture_t)(GLenum);
typedef void  (*glGenTextures_t)(GLsizei, GLuint*);
typedef void  (*glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void  (*glTexParameteri_t)(GLenum, GLenum, GLint);
typedef GLenum (*glGetError_t)(void);

static glUniform4fv_t         orig_glUniform4fv         = nullptr;
static glUseProgram_t         orig_glUseProgram         = nullptr;
static glGetUniformLocation_t orig_glGetUniformLocation = nullptr;
static glEnable_t             orig_glEnable             = nullptr;
static glDisable_t            orig_glDisable            = nullptr;
static glBlendFunc_t          orig_glBlendFunc          = nullptr;
static glDrawElements_t       orig_glDrawElements       = nullptr;
static glDrawArrays_t         orig_glDrawArrays         = nullptr;
static glBindTexture_t        orig_glBindTexture        = nullptr;
static glActiveTexture_t      orig_glActiveTexture      = nullptr;
static glGenTextures_t        orig_glGenTextures        = nullptr;
static glTexImage2D_t         orig_glTexImage2D         = nullptr;
static glTexParameteri_t      orig_glTexParameteri      = nullptr;
static glGetError_t           orig_glGetError           = nullptr;

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint g_current_program       = 0;
static GLint  g_materialDiffuse_loc   = -2;
static GLint  g_materialAmbient_loc   = -2;
static int    g_is_ped_program        = 0;
static int    g_blend_currently_on    = 0;
static int    g_we_overrode_color     = 0;
static int    g_log_draw_count        = 0;
static int    g_log_bind_count        = 0;

// ─── Internal helpers ────────────────────────────────────────────────────────
static void sync_solid_tex_color(void);
static void try_init_solid_texture(void); // lazy init, dipanggil dari render thread

static void _enable(void)  { g_enabled = 1; logf_("[SOLIDSKIN] enabled"); }
static void _disable(void) { g_enabled = 0; logf_("[SOLIDSKIN] disabled"); }
static int  _is_enabled(void) { return g_enabled; }
static void _set_color(float r, float g, float b, float a) {
    g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
    logff_("[SOLIDSKIN] color set: %.2f %.2f %.2f %.2f", r, g, b, a);
    sync_solid_tex_color();
}
static void _get_color(float* out) {
    out[0] = g_color[0]; out[1] = g_color[1];
    out[2] = g_color[2]; out[3] = g_color[3];
}
static void _set_block_blend(int v) { g_block_blend = v ? 1 : 0; }
static int  _get_block_blend(void) { return g_block_blend; }
static void _set_force_blendfunc(int v) { g_force_blendfunc = v ? 1 : 0; }
static int  _get_force_blendfunc(void) { return g_force_blendfunc; }
static void _set_texture_override(int v) {
    g_texture_override = v ? 1 : 0;
    logff_("[SOLIDSKIN] texture_override set: %d", g_texture_override);
}
static int  _get_texture_override(void) { return g_texture_override; }
static void _retry_texture_init(void) {
    g_solid_tex_init_attempted = 0;
    logf_("[SOLIDSKIN] texture init di-reset, akan dicoba lagi di hook berikutnya");
}
static int _is_texture_ready(void) { return g_solid_tex_ready; }

// ─── Helper: paksa state non-transparan ─────────────────────────────────────
static inline void force_opaque_state(void) {
    if (g_blend_currently_on) {
        orig_glDisable(GL_BLEND);
    }
    if (g_force_blendfunc) {
        orig_glBlendFunc(GL_ONE, GL_ZERO);
    }
}

// ─── Solid texture: lazy init di render thread ──────────────────────────────
static void upload_solid_tex_pixel(void) {
    if (!g_solid_tex_ready || !orig_glBindTexture || !orig_glTexImage2D) return;
    static int in_upload = 0;
    in_upload = 1;
    orig_glBindTexture(GL_TEXTURE_2D, g_solid_tex);
    orig_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_solid_tex_rgba);
    if (orig_glTexParameteri) {
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    in_upload = 0;
    logff_("[SOLIDSKIN] solid_tex diupload ulang: tex_id=%u rgba=%d,%d,%d,%d",
           g_solid_tex, g_solid_tex_rgba[0], g_solid_tex_rgba[1], g_solid_tex_rgba[2], g_solid_tex_rgba[3]);
}

static void sync_solid_tex_color(void) {
    g_solid_tex_rgba[0] = (uint8_t)(g_color[0] * 255.0f + 0.5f);
    g_solid_tex_rgba[1] = (uint8_t)(g_color[1] * 255.0f + 0.5f);
    g_solid_tex_rgba[2] = (uint8_t)(g_color[2] * 255.0f + 0.5f);
    g_solid_tex_rgba[3] = 255;
    if (g_solid_tex_ready) upload_solid_tex_pixel();
}

// Dipanggil dari DALAM hook GL (render thread, context pasti current).
// Aman dipanggil berkali-kali -- hanya benar-benar mengeksekusi sekali.
static void try_init_solid_texture(void) {
    if (g_solid_tex_ready || g_solid_tex_init_attempted) return;
    g_solid_tex_init_attempted = 1;

    if (!orig_glGenTextures || !orig_glBindTexture || !orig_glTexImage2D) {
        logf_("[SOLIDSKIN] ERROR: fungsi texture belum siap, skip init solid_tex");
        return;
    }

    GLuint tex = 0;
    orig_glGenTextures(1, &tex);

    if (orig_glGetError) {
        GLenum err = orig_glGetError();
        if (err != GL_NO_ERROR) {
            logff_("[SOLIDSKIN] glGenTextures glGetError=0x%x (tex_id=%u)", err, tex);
        }
    }

    if (tex == 0) {
        logf_("[SOLIDSKIN] ERROR: glGenTextures masih gagal walau dipanggil dari render thread hook");
        return;
    }

    g_solid_tex = tex;
    sync_solid_tex_color();
    g_solid_tex_ready = 1;
    upload_solid_tex_pixel();
    logff_("[SOLIDSKIN] solid_tex berhasil dibuat (lazy, render thread): tex_id=%u", g_solid_tex);
}

// ─── Hook: glActiveTexture / glBindTexture ──────────────────────────────────
static void hook_glActiveTexture(GLenum texture) {
    g_active_texture_unit = texture;
    orig_glActiveTexture(texture);
}

static void hook_glBindTexture(GLenum target, GLuint texture) {
    static int in_self_bind = 0;
    if (in_self_bind) {
        orig_glBindTexture(target, texture);
        return;
    }

    // Lazy init pertama kali kita benar2 berada di render thread lewat hook ini.
    if (g_enabled && g_texture_override && !g_solid_tex_ready) {
        try_init_solid_texture();
    }

    if (g_enabled && g_texture_override && g_solid_tex_ready &&
        target == GL_TEXTURE_2D && g_is_ped_program && texture != g_solid_tex) {

        if (g_log_bind_count < 30) {
            logff_("[SOLIDSKIN] glBindTexture: redirect tex=%u -> solid_tex=%u (prog=%u)",
                   texture, g_solid_tex, g_current_program);
            g_log_bind_count++;
        }
        in_self_bind = 1;
        orig_glBindTexture(target, g_solid_tex);
        in_self_bind = 0;
        return;
    }

    orig_glBindTexture(target, texture);
}

static void hook_glGenTextures(GLsizei n, GLuint* textures) {
    orig_glGenTextures(n, textures);
}
static void hook_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                               GLsizei width, GLsizei height, GLint border,
                               GLenum format, GLenum type, const void* pixels) {
    orig_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
static void hook_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    orig_glTexParameteri(target, pname, param);
}

// ─── Hook: glUseProgram ───────────────────────────────────────────────────────
static void hook_glUseProgram(GLuint program) {
    // Lazy init juga dicoba di sini sbg jalur cadangan kalau glBindTexture
    // entah kenapa belum pernah terpanggil duluan.
    if (g_enabled && g_texture_override && !g_solid_tex_ready) {
        try_init_solid_texture();
    }

    if (program != g_current_program) {
        g_current_program = program;
        g_we_overrode_color = 0;

        if (program == 0) {
            g_materialDiffuse_loc = -2;
            g_materialAmbient_loc = -2;
            g_is_ped_program      = 0;
        } else {
            auto it = g_program_cache.find(program);
            if (it != g_program_cache.end()) {
                g_materialDiffuse_loc = it->second.diffuse_loc;
                g_materialAmbient_loc = it->second.ambient_loc;
                g_is_ped_program      = it->second.is_ped;
            } else {
                g_materialDiffuse_loc = -2;
                g_materialAmbient_loc = -2;
                g_is_ped_program      = 0;
            }
        }

        if (g_enabled && g_block_blend && g_is_ped_program) {
            force_opaque_state();
        }
    }
    orig_glUseProgram(program);
}

// ─── Hook: glEnable / glDisable ──────────────────────────────────────────────
static void hook_glEnable(GLenum cap) {
    if (cap == GL_BLEND) {
        g_blend_currently_on = 1;
        if (g_enabled && g_block_blend && g_is_ped_program) {
            return;
        }
    }
    orig_glEnable(cap);
}

static void hook_glDisable(GLenum cap) {
    if (cap == GL_BLEND) {
        g_blend_currently_on = 0;
    }
    orig_glDisable(cap);
}

// ─── Hook: glBlendFunc ───────────────────────────────────────────────────────
static void hook_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    if (g_enabled && g_force_blendfunc && g_is_ped_program) {
        orig_glBlendFunc(GL_ONE, GL_ZERO);
        return;
    }
    orig_glBlendFunc(sfactor, dfactor);
}

// ─── Hook: glDrawElements / glDrawArrays ────────────────────────────────────
static inline void pre_draw_check(void) {
    if (g_enabled && g_is_ped_program && g_we_overrode_color) {
        if (g_block_blend || g_force_blendfunc) {
            force_opaque_state();
        }
        if (g_log_draw_count < 20) {
            logff_("[SOLIDSKIN] pre_draw prog=%u blend_on=%d tex_ready=%d", g_current_program, g_blend_currently_on, g_solid_tex_ready);
            g_log_draw_count++;
        }
    }
}

static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    pre_draw_check();
    orig_glDrawElements(mode, count, type, indices);
}

static void hook_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    pre_draw_check();
    orig_glDrawArrays(mode, first, count);
}

// ─── Hook: glUniform4fv ───────────────────────────────────────────────────────
static int g_hook_call_count = 0;
static void hook_glUniform4fv(GLint location, GLsizei count, const GLfloat* value) {
    if (g_hook_call_count < 10) {
        logff_("[SOLIDSKIN] glUniform4fv: loc=%d enabled=%d prog=%u",
               location, g_enabled, g_current_program);
        g_hook_call_count++;
    }

    if (!g_enabled || g_current_program == 0) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    if (g_materialDiffuse_loc == -2) {
        GLint d = orig_glGetUniformLocation(g_current_program, "MaterialDiffuse");
        GLint a = orig_glGetUniformLocation(g_current_program, "MaterialAmbient");
        GLint b = orig_glGetUniformLocation(g_current_program, "Bones");
        int is_ped = (b != -1) ? 1 : 0;

        g_materialDiffuse_loc = d;
        g_materialAmbient_loc = a;
        g_is_ped_program      = is_ped;

        g_program_cache[g_current_program] = {d, a, is_ped};

        logff_("[SOLIDSKIN] resolve prog=%u diffuse=%d ambient=%d bones=%d is_ped=%d",
               g_current_program, d, a, b, is_ped);

        if (g_enabled && g_block_blend && g_is_ped_program) {
            force_opaque_state();
        }
    }

    if (!g_is_ped_program) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    if (location != -1 && location == g_materialDiffuse_loc) {
        orig_glUniform4fv(location, count, g_color);
        g_we_overrode_color = 1;
        force_opaque_state();
        return;
    }
    if (location != -1 && location == g_materialAmbient_loc) {
        GLfloat ambient[4] = {g_color[0], g_color[1], g_color[2], 1.0f};
        orig_glUniform4fv(location, count, ambient);
        g_we_overrode_color = 1;
        force_opaque_state();
        return;
    }

    orig_glUniform4fv(location, count, value);
}

// ─── API struct ──────────────────────────────────────────────────────────────
struct SolidSkinAPI {
    void (*enable)(void);
    void (*disable)(void);
    int  (*is_enabled)(void);
    void (*set_color)(float r, float g, float b, float a);
    void (*get_color)(float* out);
    void (*set_block_blend)(int v);
    int  (*get_block_blend)(void);
    void (*set_force_blendfunc)(int v);
    int  (*get_force_blendfunc)(void);
    void (*set_texture_override)(int v);
    int  (*get_texture_override)(void);
    void (*retry_texture_init)(void);
    int  (*is_texture_ready)(void);
};

extern "C" {

EXPORT SolidSkinAPI solidskin_api = {
    _enable, _disable, _is_enabled, _set_color, _get_color,
    _set_block_blend, _get_block_blend,
    _set_force_blendfunc, _get_force_blendfunc,
    _set_texture_override, _get_texture_override,
    _retry_texture_init, _is_texture_ready
};

EXPORT void* __GetModInfo() {
    static const char* info = "solidskin|2.1|Solid color skin override + lazy texture substitution|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v2.1");
    g_enabled                   = 0;
    g_current_program           = 0;
    g_materialDiffuse_loc       = -2;
    g_materialAmbient_loc       = -2;
    g_hook_call_count           = 0;
    g_is_ped_program            = 0;
    g_blend_currently_on        = 0;
    g_we_overrode_color         = 0;
    g_log_draw_count            = 0;
    g_log_bind_count            = 0;
    g_block_blend               = 1;
    g_force_blendfunc           = 1;
    g_texture_override          = 1;
    g_solid_tex                 = 0;
    g_solid_tex_ready           = 0;
    g_solid_tex_init_attempted  = 0;
    g_active_texture_unit       = GL_TEXTURE0;
    g_program_cache.clear();
    g_color[0] = 0.0f; g_color[1] = 1.0f; g_color[2] = 0.0f; g_color[3] = 1.0f;
}

EXPORT void OnModLoad() {
    logf_("[SOLIDSKIN] OnModLoad mulai");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf_("[SOLIDSKIN] ERROR: libdobby.so tidak ditemukan"); return; }
    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logf_("[SOLIDSKIN] ERROR: DobbyHook sym tidak ada"); return; }

    void* hGLES2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES2) { logf_("[SOLIDSKIN] ERROR: libGLESv2.so tidak ada"); return; }

    orig_glGetUniformLocation = (glGetUniformLocation_t)dlsym(hGLES2, "glGetUniformLocation");
    if (!orig_glGetUniformLocation) { logf_("[SOLIDSKIN] ERROR: glGetUniformLocation null"); return; }
    logf_("[SOLIDSKIN] glGetUniformLocation OK");

    orig_glGetError = (glGetError_t)dlsym(hGLES2, "glGetError");
    if (orig_glGetError) {
        logf_("[SOLIDSKIN] glGetError tersedia (tidak di-hook, dipakai langsung)");
    } else {
        logf_("[SOLIDSKIN] WARNING: glGetError tidak ditemukan, error texture tidak akan terdiagnosis detail");
    }

    orig_glUseProgram = (glUseProgram_t)dlsym(hGLES2, "glUseProgram");
    if (!orig_glUseProgram) { logf_("[SOLIDSKIN] ERROR: glUseProgram null"); return; }
    if (dobbyHook((void*)orig_glUseProgram, (void*)hook_glUseProgram, (void**)&orig_glUseProgram) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glUseProgram gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glUseProgram OK");

    orig_glUniform4fv = (glUniform4fv_t)dlsym(hGLES2, "glUniform4fv");
    if (!orig_glUniform4fv) { logf_("[SOLIDSKIN] ERROR: glUniform4fv null"); return; }
    if (dobbyHook((void*)orig_glUniform4fv, (void*)hook_glUniform4fv, (void**)&orig_glUniform4fv) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glUniform4fv gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glUniform4fv OK");

    orig_glEnable = (glEnable_t)dlsym(hGLES2, "glEnable");
    if (!orig_glEnable) { logf_("[SOLIDSKIN] ERROR: glEnable null"); return; }
    if (dobbyHook((void*)orig_glEnable, (void*)hook_glEnable, (void**)&orig_glEnable) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glEnable gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glEnable OK");

    orig_glDisable = (glDisable_t)dlsym(hGLES2, "glDisable");
    if (!orig_glDisable) { logf_("[SOLIDSKIN] ERROR: glDisable null"); return; }
    if (dobbyHook((void*)orig_glDisable, (void*)hook_glDisable, (void**)&orig_glDisable) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glDisable gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glDisable OK");

    orig_glBlendFunc = (glBlendFunc_t)dlsym(hGLES2, "glBlendFunc");
    if (!orig_glBlendFunc) { logf_("[SOLIDSKIN] ERROR: glBlendFunc null"); return; }
    if (dobbyHook((void*)orig_glBlendFunc, (void*)hook_glBlendFunc, (void**)&orig_glBlendFunc) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glBlendFunc gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glBlendFunc OK");

    orig_glDrawElements = (glDrawElements_t)dlsym(hGLES2, "glDrawElements");
    if (!orig_glDrawElements) { logf_("[SOLIDSKIN] ERROR: glDrawElements null"); return; }
    if (dobbyHook((void*)orig_glDrawElements, (void*)hook_glDrawElements, (void**)&orig_glDrawElements) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glDrawElements gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glDrawElements OK");

    orig_glDrawArrays = (glDrawArrays_t)dlsym(hGLES2, "glDrawArrays");
    if (!orig_glDrawArrays) { logf_("[SOLIDSKIN] ERROR: glDrawArrays null"); return; }
    if (dobbyHook((void*)orig_glDrawArrays, (void*)hook_glDrawArrays, (void**)&orig_glDrawArrays) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glDrawArrays gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glDrawArrays OK");

    orig_glActiveTexture = (glActiveTexture_t)dlsym(hGLES2, "glActiveTexture");
    if (!orig_glActiveTexture) { logf_("[SOLIDSKIN] ERROR: glActiveTexture null"); return; }
    if (dobbyHook((void*)orig_glActiveTexture, (void*)hook_glActiveTexture, (void**)&orig_glActiveTexture) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glActiveTexture gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glActiveTexture OK");

    orig_glBindTexture = (glBindTexture_t)dlsym(hGLES2, "glBindTexture");
    if (!orig_glBindTexture) { logf_("[SOLIDSKIN] ERROR: glBindTexture null"); return; }
    if (dobbyHook((void*)orig_glBindTexture, (void*)hook_glBindTexture, (void**)&orig_glBindTexture) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glBindTexture gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glBindTexture OK");

    orig_glGenTextures = (glGenTextures_t)dlsym(hGLES2, "glGenTextures");
    if (!orig_glGenTextures) { logf_("[SOLIDSKIN] ERROR: glGenTextures null"); return; }
    if (dobbyHook((void*)orig_glGenTextures, (void*)hook_glGenTextures, (void**)&orig_glGenTextures) != 0) {
        logf_("[SOLIDSKIN] WARNING: hook glGenTextures gagal, lanjut tanpa hook");
    } else {
        logf_("[SOLIDSKIN] hook glGenTextures OK");
    }

    orig_glTexImage2D = (glTexImage2D_t)dlsym(hGLES2, "glTexImage2D");
    if (!orig_glTexImage2D) { logf_("[SOLIDSKIN] ERROR: glTexImage2D null"); return; }
    if (dobbyHook((void*)orig_glTexImage2D, (void*)hook_glTexImage2D, (void**)&orig_glTexImage2D) != 0) {
        logf_("[SOLIDSKIN] WARNING: hook glTexImage2D gagal, lanjut tanpa hook");
    } else {
        logf_("[SOLIDSKIN] hook glTexImage2D OK");
    }

    orig_glTexParameteri = (glTexParameteri_t)dlsym(hGLES2, "glTexParameteri");
    if (orig_glTexParameteri) {
        if (dobbyHook((void*)orig_glTexParameteri, (void*)hook_glTexParameteri, (void**)&orig_glTexParameteri) == 0) {
            logf_("[SOLIDSKIN] hook glTexParameteri OK");
        } else {
            logf_("[SOLIDSKIN] WARNING: hook glTexParameteri gagal");
            orig_glTexParameteri = nullptr;
        }
    }

    // CATATAN: solid texture TIDAK dibuat di sini lagi. Dibuat lazy, pertama
    // kali hook_glBindTexture atau hook_glUseProgram terpanggil dari render
    // thread yang benar (lihat try_init_solid_texture()).
    logf_("[SOLIDSKIN] solid_tex akan diinisialisasi lazy di render thread");

    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    g_enabled = 1;
    logf_("[SOLIDSKIN] OnModLoad SELESAI - auto enabled, texture_override=1, block_blend=1, force_blendfunc=1");
}

} // extern "C"
