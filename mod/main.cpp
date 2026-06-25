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

// ─── Trace mode ──────────────────────────────────────────────────────────────
// Trigger sekarang: draw call PERTAMA untuk program is_ped=1 yang sudah
// di-override warnanya. Dari titik itu kita log mundur sedikit (lewat
// ring buffer kecil) + maju jauh (400 call) supaya frame lengkap tertangkap.
static int  g_trace_active    = 0;
static int  g_trace_remaining = 0;
static int  g_trace_done_once = 0;
#define TRACE_BUDGET 400
#define RING_SIZE 16

static char g_ring[RING_SIZE][160];
static int  g_ring_pos = 0;
static int  g_ring_count = 0;

static inline void ring_push(const char* msg) {
    snprintf(g_ring[g_ring_pos], sizeof(g_ring[g_ring_pos]), "%s", msg);
    g_ring_pos = (g_ring_pos + 1) % RING_SIZE;
    if (g_ring_count < RING_SIZE) g_ring_count++;
}
static inline void ring_flush_to_log(void) {
    logf_("[TRACE] --- ring buffer (sebelum trigger) ---");
    int start = (g_ring_pos - g_ring_count + RING_SIZE) % RING_SIZE;
    for (int i = 0; i < g_ring_count; i++) {
        int idx = (start + i) % RING_SIZE;
        logff_("[TRACE-PRE] %s", g_ring[idx]);
    }
    logf_("[TRACE] --- end ring buffer ---");
}

static inline void trace_log(const char* fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);

    if (g_trace_active) {
        logff_("[TRACE] %s", buf);
        g_trace_remaining--;
        if (g_trace_remaining <= 0) {
            g_trace_active = 0;
            logf_("[TRACE] === trace window selesai ===");
        }
    } else if (!g_trace_done_once) {
        // Belum trigger -> simpan ke ring buffer sebagai konteks "sebelum".
        ring_push(buf);
    }
}

static inline void maybe_trigger_trace(const char* reason) {
    if (!g_trace_done_once) {
        g_trace_done_once = 1;
        g_trace_active = 1;
        g_trace_remaining = TRACE_BUDGET;
        logff_("[TRACE] === TRIGGER: %s ===", reason);
        ring_flush_to_log();
    }
}

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

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint g_current_program       = 0;
static GLint  g_materialDiffuse_loc   = -2;
static GLint  g_materialAmbient_loc   = -2;
static int    g_is_ped_program        = 0;
static int    g_blend_currently_on    = 0;
static int    g_we_overrode_color     = 0;
static int    g_active_texture_unit   = GL_TEXTURE0;

// ─── Internal helpers ────────────────────────────────────────────────────────
static void _enable(void)  { g_enabled = 1; logf_("[SOLIDSKIN] enabled"); }
static void _disable(void) { g_enabled = 0; logf_("[SOLIDSKIN] disabled"); }
static int  _is_enabled(void) { return g_enabled; }
static void _set_color(float r, float g, float b, float a) {
    g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
    logff_("[SOLIDSKIN] color set: %.2f %.2f %.2f %.2f", r, g, b, a);
}
static void _get_color(float* out) {
    out[0] = g_color[0]; out[1] = g_color[1];
    out[2] = g_color[2]; out[3] = g_color[3];
}
static void _set_block_blend(int v) { g_block_blend = v ? 1 : 0; }
static int  _get_block_blend(void) { return g_block_blend; }
static void _set_force_blendfunc(int v) { g_force_blendfunc = v ? 1 : 0; }
static int  _get_force_blendfunc(void) { return g_force_blendfunc; }
static void _rearm_trace(void) {
    g_trace_done_once = 0;
    g_trace_active = 0;
    g_trace_remaining = 0;
    g_ring_pos = 0;
    g_ring_count = 0;
    logf_("[TRACE] === di-rearm manual, siap trigger ulang ===");
}

// ─── Helper: paksa state non-transparan ─────────────────────────────────────
static inline void force_opaque_state(void) {
    if (g_blend_currently_on) {
        orig_glDisable(GL_BLEND);
    }
    if (g_force_blendfunc) {
        orig_glBlendFunc(GL_ONE, GL_ZERO);
    }
}

// ─── Hook: glActiveTexture / glBindTexture (untuk lihat texture ped) ────────
static void hook_glActiveTexture(GLenum texture) {
    g_active_texture_unit = texture;
    trace_log("glActiveTexture(unit=%d)", texture - GL_TEXTURE0);
    orig_glActiveTexture(texture);
}

static void hook_glBindTexture(GLenum target, GLuint texture) {
    trace_log("glBindTexture(unit=%d, tex_id=%u) is_ped=%d",
              g_active_texture_unit - GL_TEXTURE0, texture, g_is_ped_program);
    orig_glBindTexture(target, texture);
}

// ─── Hook: glUseProgram ───────────────────────────────────────────────────────
static void hook_glUseProgram(GLuint program) {
    if (program != g_current_program) {
        trace_log("glUseProgram(%u)", program);

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
        trace_log("glEnable(GL_BLEND) is_ped=%d", g_is_ped_program);
        g_blend_currently_on = 1;
        if (g_enabled && g_block_blend && g_is_ped_program) {
            trace_log("  -> TELAN");
            return;
        }
    }
    orig_glEnable(cap);
}

static void hook_glDisable(GLenum cap) {
    if (cap == GL_BLEND) {
        trace_log("glDisable(GL_BLEND) is_ped=%d", g_is_ped_program);
        g_blend_currently_on = 0;
    }
    orig_glDisable(cap);
}

// ─── Hook: glBlendFunc ───────────────────────────────────────────────────────
static const char* blend_factor_name(GLenum f) {
    switch (f) {
        case GL_ZERO: return "ZERO";
        case GL_ONE: return "ONE";
        case GL_SRC_ALPHA: return "SRC_ALPHA";
        case GL_ONE_MINUS_SRC_ALPHA: return "ONE_MINUS_SRC_ALPHA";
        case GL_SRC_COLOR: return "SRC_COLOR";
        case GL_ONE_MINUS_SRC_COLOR: return "ONE_MINUS_SRC_COLOR";
        case GL_DST_ALPHA: return "DST_ALPHA";
        case GL_DST_COLOR: return "DST_COLOR";
        default: return "OTHER";
    }
}
static void hook_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    trace_log("glBlendFunc(%s, %s) is_ped=%d",
              blend_factor_name(sfactor), blend_factor_name(dfactor), g_is_ped_program);
    if (g_enabled && g_force_blendfunc && g_is_ped_program) {
        trace_log("  -> OVERRIDE jadi (ONE, ZERO)");
        orig_glBlendFunc(GL_ONE, GL_ZERO);
        return;
    }
    orig_glBlendFunc(sfactor, dfactor);
}

// ─── Hook: glDrawElements / glDrawArrays ────────────────────────────────────
static inline void pre_draw_check(const char* which) {
    trace_log("%s() prog=%u is_ped=%d blend_on=%d overrode=%d",
              which, g_current_program, g_is_ped_program, g_blend_currently_on, g_we_overrode_color);

    if (g_enabled && g_is_ped_program && g_we_overrode_color) {
        if (g_block_blend || g_force_blendfunc) {
            force_opaque_state();
        }
        // INI TRIGGER UTAMA: draw call pertama untuk ped yg sudah di-override.
        maybe_trigger_trace("draw call pertama utk ped yg sudah di-override warna");
    }
}

static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    pre_draw_check("glDrawElements");
    orig_glDrawElements(mode, count, type, indices);
}

static void hook_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    pre_draw_check("glDrawArrays");
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

    trace_log("glUniform4fv(loc=%d) prog=%u is_ped=%d diffuse_loc=%d ambient_loc=%d",
              location, g_current_program, g_is_ped_program, g_materialDiffuse_loc, g_materialAmbient_loc);

    if (!g_is_ped_program) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    if (location != -1 && location == g_materialDiffuse_loc) {
        trace_log("  -> OVERRIDE diffuse");
        orig_glUniform4fv(location, count, g_color);
        g_we_overrode_color = 1;
        force_opaque_state();
        return;
    }
    if (location != -1 && location == g_materialAmbient_loc) {
        GLfloat ambient[4] = {g_color[0], g_color[1], g_color[2], 1.0f};
        trace_log("  -> OVERRIDE ambient (alpha=1.0)");
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
    void (*rearm_trace)(void);
};

extern "C" {

EXPORT SolidSkinAPI solidskin_api = {
    _enable, _disable, _is_enabled, _set_color, _get_color,
    _set_block_blend, _get_block_blend,
    _set_force_blendfunc, _get_force_blendfunc,
    _rearm_trace
};

EXPORT void* __GetModInfo() {
    static const char* info = "solidskin|1.5|Solid color skin override + full draw-triggered trace|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v1.5");
    g_enabled             = 0;
    g_current_program     = 0;
    g_materialDiffuse_loc = -2;
    g_materialAmbient_loc = -2;
    g_hook_call_count     = 0;
    g_is_ped_program      = 0;
    g_blend_currently_on  = 0;
    g_we_overrode_color   = 0;
    g_block_blend         = 1;
    g_force_blendfunc     = 1;
    g_trace_active        = 0;
    g_trace_remaining      = 0;
    g_trace_done_once     = 0;
    g_ring_pos            = 0;
    g_ring_count          = 0;
    g_active_texture_unit = GL_TEXTURE0;
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

    orig_glBindTexture = (glBindTexture_t)dlsym(hGLES2, "glBindTexture");
    if (orig_glBindTexture) {
        if (dobbyHook((void*)orig_glBindTexture, (void*)hook_glBindTexture, (void**)&orig_glBindTexture) == 0) {
            logf_("[SOLIDSKIN] hook glBindTexture OK");
        } else {
            logf_("[SOLIDSKIN] WARNING: hook glBindTexture gagal");
            orig_glBindTexture = nullptr;
        }
    }

    orig_glActiveTexture = (glActiveTexture_t)dlsym(hGLES2, "glActiveTexture");
    if (orig_glActiveTexture) {
        if (dobbyHook((void*)orig_glActiveTexture, (void*)hook_glActiveTexture, (void**)&orig_glActiveTexture) == 0) {
            logf_("[SOLIDSKIN] hook glActiveTexture OK");
        } else {
            logf_("[SOLIDSKIN] WARNING: hook glActiveTexture gagal");
            orig_glActiveTexture = nullptr;
        }
    }

    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    g_enabled = 1;
    logf_("[SOLIDSKIN] OnModLoad SELESAI - auto enabled, trace siap (trigger di draw call ped pertama)");
}

} // extern "C"
