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
static float g_color[4] = {0.0f, 1.0f, 0.0f, 1.0f}; // hijau terang
static int   g_block_blend = 1; // toggle untuk force-disable GL_BLEND saat ped program aktif

// ─── Program cache (fix FPS drop) ────────────────────────────────────────────
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

static glUniform4fv_t         orig_glUniform4fv         = nullptr;
static glUseProgram_t         orig_glUseProgram         = nullptr;
static glGetUniformLocation_t orig_glGetUniformLocation = nullptr;
static glEnable_t             orig_glEnable             = nullptr;
static glDisable_t            orig_glDisable            = nullptr;
static glBlendFunc_t          orig_glBlendFunc          = nullptr;

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint g_current_program       = 0;
static GLint  g_materialDiffuse_loc   = -2;
static GLint  g_materialAmbient_loc   = -2;
static int    g_is_ped_program        = 0;
static int    g_blend_currently_on    = 0; // tracking state asli game (bukan state nyata di GPU)

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
static void _set_block_blend(int v) {
    g_block_blend = v ? 1 : 0;
    logff_("[SOLIDSKIN] block_blend set: %d", g_block_blend);
}
static int _get_block_blend(void) { return g_block_blend; }

// ─── Hook: glUseProgram ───────────────────────────────────────────────────────
static void hook_glUseProgram(GLuint program) {
    if (program != g_current_program) {
        g_current_program = program;

        if (program == 0) {
            g_materialDiffuse_loc = -2;
            g_materialAmbient_loc = -2;
            g_is_ped_program      = 0;
        } else {
            auto it = g_program_cache.find(program);
            if (it != g_program_cache.end()) {
                // Pakai cache — tidak perlu glGetUniformLocation lagi
                g_materialDiffuse_loc = it->second.diffuse_loc;
                g_materialAmbient_loc = it->second.ambient_loc;
                g_is_ped_program      = it->second.is_ped;
            } else {
                // Belum pernah dilihat, mark untuk resolve
                g_materialDiffuse_loc = -2;
                g_materialAmbient_loc = -2;
                g_is_ped_program      = 0;
            }
        }

        // Setiap kali program berganti, re-evaluasi apakah blend harus
        // dipaksa off berdasarkan state blend yang diminta game sebelumnya.
        if (g_enabled && g_block_blend && g_is_ped_program && g_blend_currently_on) {
            orig_glDisable(GL_BLEND);
        } else if (g_blend_currently_on) {
            orig_glEnable(GL_BLEND);
        }
    }
    orig_glUseProgram(program);
}

// ─── Hook: glEnable / glDisable (untuk blokir GL_BLEND di ped program) ───────
static void hook_glEnable(GLenum cap) {
    if (cap == GL_BLEND) {
        g_blend_currently_on = 1;
        if (g_enabled && g_block_blend && g_is_ped_program) {
            // Jangan benar-benar enable blend — supaya alpha output solid 1.0
            // tidak ke-blend dengan background / texture alpha cutout.
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

    // Lazy resolve — hanya sekali per program, lalu masuk cache
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

        // Program baru terdeteksi sebagai ped DAN blend lagi nyala -> matikan sekarang.
        if (g_enabled && g_block_blend && g_is_ped_program && g_blend_currently_on) {
            orig_glDisable(GL_BLEND);
        }
    }

    if (!g_is_ped_program) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    if (location != -1 && location == g_materialDiffuse_loc) {
        orig_glUniform4fv(location, count, g_color);
        return;
    }
    if (location != -1 && location == g_materialAmbient_loc) {
        // Alpha hardcode 1.0f — tidak transparan
        GLfloat ambient[4] = {g_color[0], g_color[1], g_color[2], 1.0f};
        orig_glUniform4fv(location, count, ambient);
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
};

// ─── AML entry points ────────────────────────────────────────────────────────
extern "C" {

EXPORT SolidSkinAPI solidskin_api = {
    _enable, _disable, _is_enabled, _set_color, _get_color,
    _set_block_blend, _get_block_blend
};

EXPORT void* __GetModInfo() {
    static const char* info = "solidskin|1.2|Solid color skin override via GL hook (no-blend)|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v1.2");
    g_enabled             = 0;
    g_current_program     = 0;
    g_materialDiffuse_loc = -2;
    g_materialAmbient_loc = -2;
    g_hook_call_count     = 0;
    g_is_ped_program      = 0;
    g_blend_currently_on  = 0;
    g_block_blend         = 1;
    g_program_cache.clear();
    // Hijau terang, full opaque
    g_color[0] = 0.0f; g_color[1] = 1.0f; g_color[2] = 0.0f; g_color[3] = 1.0f;
}

EXPORT void OnModLoad() {
    logf_("[SOLIDSKIN] OnModLoad mulai");

    // 1. Load Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf_("[SOLIDSKIN] ERROR: libdobby.so tidak ditemukan"); return; }
    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logf_("[SOLIDSKIN] ERROR: DobbyHook sym tidak ada"); return; }

    // 2. Load libGLESv2
    void* hGLES2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES2) { logf_("[SOLIDSKIN] ERROR: libGLESv2.so tidak ada"); return; }

    // 3. glGetUniformLocation (tidak di-hook, hanya dipakai langsung)
    orig_glGetUniformLocation = (glGetUniformLocation_t)dlsym(hGLES2, "glGetUniformLocation");
    if (!orig_glGetUniformLocation) {
        logf_("[SOLIDSKIN] ERROR: glGetUniformLocation null"); return;
    }
    logf_("[SOLIDSKIN] glGetUniformLocation OK");

    // 4. Hook glUseProgram
    orig_glUseProgram = (glUseProgram_t)dlsym(hGLES2, "glUseProgram");
    if (!orig_glUseProgram) {
        logf_("[SOLIDSKIN] ERROR: glUseProgram null"); return;
    }
    if (dobbyHook((void*)orig_glUseProgram,
                  (void*)hook_glUseProgram,
                  (void**)&orig_glUseProgram) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glUseProgram gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glUseProgram OK");

    // 5. Hook glUniform4fv
    orig_glUniform4fv = (glUniform4fv_t)dlsym(hGLES2, "glUniform4fv");
    if (!orig_glUniform4fv) {
        logf_("[SOLIDSKIN] ERROR: glUniform4fv null"); return;
    }
    logff_("[SOLIDSKIN] glUniform4fv addr = %p", (void*)orig_glUniform4fv);
    if (dobbyHook((void*)orig_glUniform4fv,
                  (void*)hook_glUniform4fv,
                  (void**)&orig_glUniform4fv) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glUniform4fv gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glUniform4fv OK");

    // 6. Hook glEnable (untuk blok GL_BLEND saat ped program)
    orig_glEnable = (glEnable_t)dlsym(hGLES2, "glEnable");
    if (!orig_glEnable) {
        logf_("[SOLIDSKIN] ERROR: glEnable null"); return;
    }
    if (dobbyHook((void*)orig_glEnable,
                  (void*)hook_glEnable,
                  (void**)&orig_glEnable) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glEnable gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glEnable OK");

    // 7. Hook glDisable (untuk tracking state blend asli game)
    orig_glDisable = (glDisable_t)dlsym(hGLES2, "glDisable");
    if (!orig_glDisable) {
        logf_("[SOLIDSKIN] ERROR: glDisable null"); return;
    }
    if (dobbyHook((void*)orig_glDisable,
                  (void*)hook_glDisable,
                  (void**)&orig_glDisable) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glDisable gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glDisable OK");

    // 8. Tulis alamat API untuk Lua
    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    g_enabled = 1;
    logf_("[SOLIDSKIN] OnModLoad SELESAI - auto enabled, warna hijau, block_blend=1");
}

} // extern "C"
