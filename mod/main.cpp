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

static glUniform4fv_t         orig_glUniform4fv         = nullptr;
static glUseProgram_t         orig_glUseProgram         = nullptr;
static glGetUniformLocation_t orig_glGetUniformLocation = nullptr;

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint g_current_program       = 0;
static GLint  g_materialDiffuse_loc   = -2;
static GLint  g_materialAmbient_loc   = -2;
static int    g_is_ped_program        = 0;

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
    }
    orig_glUseProgram(program);
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
};

// ─── AML entry points ────────────────────────────────────────────────────────
extern "C" {

EXPORT SolidSkinAPI solidskin_api = {
    _enable, _disable, _is_enabled, _set_color, _get_color
};

EXPORT void* __GetModInfo() {
    static const char* info = "solidskin|1.1|Solid color skin override via GL hook|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v1.1");
    g_enabled             = 0;
    g_current_program     = 0;
    g_materialDiffuse_loc = -2;
    g_materialAmbient_loc = -2;
    g_hook_call_count     = 0;
    g_is_ped_program      = 0;
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

    // 6. Tulis alamat API untuk Lua
    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    g_enabled = 1;
    logf_("[SOLIDSKIN] OnModLoad SELESAI - auto enabled, warna hijau");
}

} // extern "C"