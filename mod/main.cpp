#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <GLES2/gl2.h>

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
static float g_color[4] = {1.0f, 0.0f, 0.0f, 1.0f}; // default: merah

// ─── Function pointer types ───────────────────────────────────────────────────
typedef void     (*glUniform4fv_t)(GLint, GLsizei, const GLfloat*);
typedef void     (*glUniform3fv_t)(GLint, GLsizei, const GLfloat*);
typedef void     (*glUseProgram_t)(GLuint);
typedef GLint    (*glGetUniformLocation_t)(GLuint, const char*);

static glUniform4fv_t        orig_glUniform4fv        = nullptr;
static glUniform3fv_t        orig_glUniform3fv        = nullptr;
static glUseProgram_t        orig_glUseProgram        = nullptr;
static glGetUniformLocation_t orig_glGetUniformLocation = nullptr;

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint  g_current_program       = 0;
static GLint   g_materialDiffuse_loc   = -2; // -2 = belum di-resolve
static GLint   g_materialAmbient_loc   = -2;

// ─── Internal helpers ────────────────────────────────────────────────────────
static void _enable(void)               { g_enabled = 1; logf_("[SOLIDSKIN] enabled"); }
static void _disable(void)              { g_enabled = 0; logf_("[SOLIDSKIN] disabled"); }
static int  _is_enabled(void)           { return g_enabled; }
static void _set_color(float r, float g, float b, float a) {
    g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
    logff_("[SOLIDSKIN] color set: %.2f %.2f %.2f %.2f", r, g, b, a);
}
static void _get_color(float* out) {
    out[0] = g_color[0]; out[1] = g_color[1];
    out[2] = g_color[2]; out[3] = g_color[3];
}

// ─── Hook: glUseProgram ───────────────────────────────────────────────────────
// Reset uniform location cache setiap kali program shader ganti
static void hook_glUseProgram(GLuint program) {
    if (program != g_current_program) {
        g_current_program     = program;
        g_materialDiffuse_loc = -2;
        g_materialAmbient_loc = -2;
    }
    orig_glUseProgram(program);
}

// ─── Hook: glUniform4fv ───────────────────────────────────────────────────────
// Intercept saat MaterialDiffuse atau MaterialAmbient di-set
static void hook_glUniform4fv(GLint location, GLsizei count, const GLfloat* value) {
    if (!g_enabled || g_current_program == 0) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    // Lazy resolve uniform locations
    if (g_materialDiffuse_loc == -2) {
        g_materialDiffuse_loc = orig_glGetUniformLocation(g_current_program, "MaterialDiffuse");
        g_materialAmbient_loc = orig_glGetUniformLocation(g_current_program, "MaterialAmbient");
    }

    if (location != -1 && location == g_materialDiffuse_loc) {
        // Override warna diffuse ke solid color
        orig_glUniform4fv(location, count, g_color);
        return;
    }
    if (location != -1 && location == g_materialAmbient_loc) {
        // Override ambient juga (agar lighting tidak ngelap)
        GLfloat ambient[4] = {g_color[0], g_color[1], g_color[2], value[3]};
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
    static const char* info = "solidskin|1.0|Solid color skin override via GL hook|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v1.0");
    g_enabled             = 0;
    g_current_program     = 0;
    g_materialDiffuse_loc = -2;
    g_materialAmbient_loc = -2;
    g_color[0] = 1.0f; g_color[1] = 0.0f; g_color[2] = 0.0f; g_color[3] = 1.0f;
}

EXPORT void OnModLoad() {
    logf_("[SOLIDSKIN] OnModLoad mulai");

    // ── 1. Load Dobby ──────────────────────────────────────────────────────
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf_("[SOLIDSKIN] ERROR: libdobby.so tidak ditemukan"); return; }

    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logf_("[SOLIDSKIN] ERROR: DobbyHook sym tidak ada"); return; }

    // ── 2. Dapatkan base libGTASA ──────────────────────────────────────────
    void* hGTASA = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!hGTASA) { logf_("[SOLIDSKIN] ERROR: libGTASA.so tidak loaded"); return; }
    uintptr_t base = (uintptr_t)hGTASA;
    logff_("[SOLIDSKIN] libGTASA base = 0x%lx", (unsigned long)base);

    // ── 3. Ambil orig_glGetUniformLocation dari GOT libGTASA ──────────────
    // GOT offset hasil analisa: 0x6755ec
    uintptr_t got_getUniform = base + 0x6755ec;
    orig_glGetUniformLocation = *(glGetUniformLocation_t*)got_getUniform;
    if (!orig_glGetUniformLocation) {
        logf_("[SOLIDSKIN] ERROR: glGetUniformLocation ptr null");
        return;
    }
    logf_("[SOLIDSKIN] glGetUniformLocation OK");

    // ── 4. Hook glUseProgram via GOT offset 0x67468c ──────────────────────
    uintptr_t got_useProgram = base + 0x67468c;
    orig_glUseProgram = *(glUseProgram_t*)got_useProgram;
    if (!orig_glUseProgram) {
        logf_("[SOLIDSKIN] ERROR: glUseProgram ptr null");
        return;
    }
    if (dobbyHook((void*)orig_glUseProgram,
                  (void*)hook_glUseProgram,
                  (void**)&orig_glUseProgram) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glUseProgram gagal");
        return;
    }
    logf_("[SOLIDSKIN] hook glUseProgram OK");

    // ── 5. Hook glUniform4fv via dlsym libGLESv2 ───────────────────────
    void* hGLES = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES) { logf_("[SOLIDSKIN] ERROR: libGLESv2.so tidak ditemukan"); return; }
    orig_glUniform4fv = (glUniform4fv_t)dlsym(hGLES, "glUniform4fv");
    if (!orig_glUniform4fv) {
        logf_("[SOLIDSKIN] ERROR: glUniform4fv dlsym null");
        return;
    }
    logff_("[SOLIDSKIN] glUniform4fv addr = %p", (void*)orig_glUniform4fv);
    if (dobbyHook((void*)orig_glUniform4fv,
                  (void*)hook_glUniform4fv,
                  (void**)&orig_glUniform4fv) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glUniform4fv gagal");
        return;
    }
    logf_("[SOLIDSKIN] hook glUniform4fv OK");

    // ── 6. Tulis alamat API ke file untuk Lua (opsional) ──────────────────
    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    logf_("[SOLIDSKIN] OnModLoad SELESAI");
}

} // extern "C"
