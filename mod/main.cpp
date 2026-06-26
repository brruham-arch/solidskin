#include <stdint.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdio.h>
#include <stdarg.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <EGL/egl.h>
#include <unordered_map>
#include <vector>

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
static int   g_enabled          = 0;

// Warna HIJAU = ped kelihatan normal
static float g_color[4]         = {0.0f, 1.0f, 0.0f, 1.0f};

// Warna KUNING = ped di balik tembok
static float g_color_behind[4]  = {1.0f, 1.0f, 0.0f, 1.0f};

static int   g_block_blend      = 1;
static int   g_force_blendfunc  = 1;
static int   g_texture_override = 1;
static int   g_depth_bypass     = 1;

// ─── Solid texture state ─────────────────────────────────────────────────────
static GLuint  g_solid_tex               = 0;
static int     g_solid_tex_ready         = 0;
static int     g_solid_tex_init_attempted= 0;
static uint8_t g_solid_tex_rgba[4]       = {0, 255, 0, 255};
static int     g_active_texture_unit     = GL_TEXTURE0;

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
typedef void  (*glDepthFunc_t)(GLenum);
typedef void  (*glDepthMask_t)(GLboolean);
typedef void  (*glDepthRangef_t)(GLfloat, GLfloat);
typedef void  (*glDrawArraysInstanced_t)(GLenum, GLint, GLsizei, GLsizei);
typedef void  (*glDrawElementsInstanced_t)(GLenum, GLsizei, GLenum, const void*, GLsizei);
typedef void  (*glDrawRangeElements_t)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*);
typedef void  (*glMultiDrawArrays_t)(GLenum, const GLint*, const GLsizei*, GLsizei);
typedef void  (*glMultiDrawElements_t)(GLenum, const GLsizei*, GLenum, const void* const*, GLsizei);
typedef void  (*glDrawArraysIndirect_t)(GLenum, const void*);
typedef void  (*glDrawElementsIndirect_t)(GLenum, GLenum, const void*);
typedef void  (*glDrawElementsBaseVertex_t)(GLenum, GLsizei, GLenum, const void*, GLint);
typedef __eglMustCastToProperFunctionPointerType (*eglGetProcAddress_t)(const char*);

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
static glDepthFunc_t          orig_glDepthFunc          = nullptr;
static glDepthMask_t          orig_glDepthMask          = nullptr;
static glDepthRangef_t        orig_glDepthRangef        = nullptr;
static glDrawArraysInstanced_t   orig_glDrawArraysInstanced   = nullptr;
static glDrawElementsInstanced_t  orig_glDrawElementsInstanced  = nullptr;
static glDrawRangeElements_t      orig_glDrawRangeElements      = nullptr;
static glMultiDrawArrays_t        orig_glMultiDrawArrays        = nullptr;
static glMultiDrawElements_t      orig_glMultiDrawElements      = nullptr;
static glDrawArraysIndirect_t     orig_glDrawArraysIndirect     = nullptr;
static glDrawElementsIndirect_t   orig_glDrawElementsIndirect   = nullptr;
static glDrawElementsBaseVertex_t orig_glDrawElementsBaseVertex = nullptr;
static eglGetProcAddress_t        orig_eglGetProcAddress        = nullptr;

// Flag: prog yang sedang kita trace secara aktif
static GLuint g_trace_prog = 149; // prog ped yang tidak punya draw call
static int    g_trace_active = 0; // 1 kalau g_trace_prog sedang aktif
static int    g_trace_log_count = 0;

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint    g_current_program      = 0;
static GLint     g_materialDiffuse_loc  = -2;
static GLint     g_materialAmbient_loc  = -2;
static int       g_is_ped_program       = 0;
static int       g_blend_currently_on   = 0;
static int       g_we_overrode_color    = 0;
static int       g_log_draw_count       = 0;
static int       g_log_bind_count       = 0;
static int       g_hook_call_count      = 0;

static GLenum    g_game_depth_func      = GL_LESS;
static GLboolean g_game_depth_mask      = GL_TRUE;
static GLfloat   g_game_depth_range_near= 0.0f;
static GLfloat   g_game_depth_range_far = 1.0f;

// ─── Forward declarations ─────────────────────────────────────────────────────
static void sync_solid_tex_color(void);
static void try_init_solid_texture(void);

// ─── API internals ────────────────────────────────────────────────────────────
static void _enable(void)  { g_enabled = 1; logf_("[SOLIDSKIN] enabled"); }
static void _disable(void) { g_enabled = 0; logf_("[SOLIDSKIN] disabled"); }
static int  _is_enabled(void) { return g_enabled; }

static void _set_color(float r, float g, float b, float a) {
    g_color[0] = r; g_color[1] = g; g_color[2] = b; g_color[3] = a;
    logff_("[SOLIDSKIN] color (visible) set: %.2f %.2f %.2f %.2f", r, g, b, a);
    sync_solid_tex_color();
}
static void _get_color(float* out) {
    out[0] = g_color[0]; out[1] = g_color[1];
    out[2] = g_color[2]; out[3] = g_color[3];
}

// Setter warna behind-wall (default kuning, bisa diubah via API)
static void _set_color_behind(float r, float g, float b, float a) {
    g_color_behind[0] = r; g_color_behind[1] = g;
    g_color_behind[2] = b; g_color_behind[3] = a;
    logff_("[SOLIDSKIN] color (behind wall) set: %.2f %.2f %.2f %.2f", r, g, b, a);
}
static void _get_color_behind(float* out) {
    out[0] = g_color_behind[0]; out[1] = g_color_behind[1];
    out[2] = g_color_behind[2]; out[3] = g_color_behind[3];
}

static void _set_block_blend(int v)      { g_block_blend     = v ? 1 : 0; }
static int  _get_block_blend(void)       { return g_block_blend; }
static void _set_force_blendfunc(int v)  { g_force_blendfunc = v ? 1 : 0; }
static int  _get_force_blendfunc(void)   { return g_force_blendfunc; }
static void _set_texture_override(int v) { g_texture_override= v ? 1 : 0; }
static int  _get_texture_override(void)  { return g_texture_override; }
static void _retry_texture_init(void)    { g_solid_tex_init_attempted = 0; }
static int  _is_texture_ready(void)      { return g_solid_tex_ready; }
static void _set_depth_bypass(int v)     {
    g_depth_bypass = v ? 1 : 0;
    logff_("[SOLIDSKIN] depth_bypass set: %d", g_depth_bypass);
}
static int  _get_depth_bypass(void)      { return g_depth_bypass; }

// ─── Helper: paksa non-transparan ────────────────────────────────────────────
static inline void force_opaque_state(void) {
    if (g_blend_currently_on)  orig_glDisable(GL_BLEND);
    if (g_force_blendfunc)     orig_glBlendFunc(GL_ONE, GL_ZERO);
}

// ─── Solid texture ────────────────────────────────────────────────────────────
static void upload_solid_tex_pixel(void) {
    if (!g_solid_tex_ready || !orig_glBindTexture || !orig_glTexImage2D) return;
    orig_glBindTexture(GL_TEXTURE_2D, g_solid_tex);
    orig_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, g_solid_tex_rgba);
    if (orig_glTexParameteri) {
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    logff_("[SOLIDSKIN] solid_tex upload: id=%u rgba=%d,%d,%d,%d",
           g_solid_tex, g_solid_tex_rgba[0], g_solid_tex_rgba[1],
           g_solid_tex_rgba[2], g_solid_tex_rgba[3]);
}

static void sync_solid_tex_color(void) {
    g_solid_tex_rgba[0] = (uint8_t)(g_color[0] * 255.0f + 0.5f);
    g_solid_tex_rgba[1] = (uint8_t)(g_color[1] * 255.0f + 0.5f);
    g_solid_tex_rgba[2] = (uint8_t)(g_color[2] * 255.0f + 0.5f);
    g_solid_tex_rgba[3] = 255;
    if (g_solid_tex_ready) upload_solid_tex_pixel();
}

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
        if (err != GL_NO_ERROR)
            logff_("[SOLIDSKIN] glGenTextures err=0x%x tex=%u", err, tex);
    }

    if (tex == 0) {
        logf_("[SOLIDSKIN] ERROR: glGenTextures gagal, tex=0");
        return;
    }

    g_solid_tex = tex;
    sync_solid_tex_color();
    g_solid_tex_ready = 1;
    upload_solid_tex_pixel();
    logff_("[SOLIDSKIN] solid_tex ready (lazy render thread): id=%u", g_solid_tex);
}

// ─── Hook: glDepthRangef ──────────────────────────────────────────────────────
// Simpan state asli game; nilai aktual diset manual di draw call pass.
static void hook_glDepthRangef(GLfloat n, GLfloat f) {
    g_game_depth_range_near = n;
    g_game_depth_range_far  = f;
    // Jangan forward langsung -- draw call hook yg kendalikan depth range.
    // Jika tidak sedang di ped, teruskan normal.
    if (!(g_enabled && g_depth_bypass && g_is_ped_program)) {
        orig_glDepthRangef(n, f);
    }
}

// ─── Hook: glActiveTexture / glBindTexture ───────────────────────────────────
static void hook_glActiveTexture(GLenum texture) {
    g_active_texture_unit = texture;
    orig_glActiveTexture(texture);
}

static void hook_glBindTexture(GLenum target, GLuint texture) {
    static int in_self_bind = 0;
    if (in_self_bind) { orig_glBindTexture(target, texture); return; }

    if (g_enabled && g_texture_override && !g_solid_tex_ready)
        try_init_solid_texture();

    if (g_enabled && g_texture_override && g_solid_tex_ready &&
        target == GL_TEXTURE_2D && g_is_ped_program && texture != g_solid_tex) {
        if (g_log_bind_count < 30) {
            logff_("[SOLIDSKIN] bindTex redirect %u->%u prog=%u trace=%d",
                   texture, g_solid_tex, g_current_program, g_trace_active);
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
    orig_glTexImage2D(target, level, internalformat, width, height,
                      border, format, type, pixels);
}
static void hook_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    orig_glTexParameteri(target, pname, param);
}

// ─── Hook: glDepthFunc / glDepthMask ─────────────────────────────────────────
static void hook_glDepthFunc(GLenum func) {
    g_game_depth_func = func;
    if (g_enabled && g_depth_bypass && g_is_ped_program) return; // dikendalikan draw call
    orig_glDepthFunc(func);
}
static void hook_glDepthMask(GLboolean flag) {
    g_game_depth_mask = flag;
    if (g_enabled && g_depth_bypass && g_is_ped_program) return; // dikendalikan draw call
    orig_glDepthMask(flag);
}

// ─── Hook: glUseProgram ───────────────────────────────────────────────────────
static void hook_glUseProgram(GLuint program) {
    if (g_enabled && g_texture_override && !g_solid_tex_ready)
        try_init_solid_texture();

    if (program != g_current_program) {
        g_current_program   = program;
        g_we_overrode_color = 0;

        if (program == 0) {
            g_materialDiffuse_loc = -2;
            g_materialAmbient_loc = -2;
            g_is_ped_program      = 0;
        } else {
            auto it = g_program_cache.find(program);
            if (it != g_program_cache.end()) {
                // Sudah di cache, langsung pakai
                g_materialDiffuse_loc = it->second.diffuse_loc;
                g_materialAmbient_loc = it->second.ambient_loc;
                g_is_ped_program      = it->second.is_ped;
            } else {
                // Belum di cache — resolve SEKARANG, jangan tunggu glUniform4fv.
                // Game sering cache uniform antar frame, jadi glUniform4fv
                // tidak selalu dipanggil lagi untuk program yang sudah aktif.
                GLint d = orig_glGetUniformLocation(program, "MaterialDiffuse");
                GLint a = orig_glGetUniformLocation(program, "MaterialAmbient");
                GLint b = orig_glGetUniformLocation(program, "Bones");
                int is_ped = (b != -1) ? 1 : 0;

                g_materialDiffuse_loc = d;
                g_materialAmbient_loc = a;
                g_is_ped_program      = is_ped;

                g_program_cache[program] = {d, a, is_ped};

                logff_("[SOLIDSKIN] resolve(UseProgram) prog=%u diffuse=%d ambient=%d bones=%d is_ped=%d",
                       program, d, a, b, is_ped);
            }
        }

        if (g_enabled && g_block_blend && g_is_ped_program)
            force_opaque_state();

        // Restore depth state normal saat ganti ke non-ped program
        if (!g_is_ped_program || !g_enabled || !g_depth_bypass) {
            orig_glDepthFunc(g_game_depth_func);
            orig_glDepthMask(g_game_depth_mask);
            if (orig_glDepthRangef)
                orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
        }
    }
    // Trace: tandai kalau program yang dicurigai aktif
    g_trace_active = (g_enabled && g_is_ped_program &&
                      g_current_program == g_trace_prog) ? 1 : 0;
    orig_glUseProgram(program);
}

// ─── Hook: glEnable / glDisable ──────────────────────────────────────────────
static void hook_glEnable(GLenum cap) {
    if (cap == GL_BLEND) {
        g_blend_currently_on = 1;
        if (g_enabled && g_block_blend && g_is_ped_program) return;
    }
    orig_glEnable(cap);
}
static void hook_glDisable(GLenum cap) {
    if (cap == GL_BLEND) g_blend_currently_on = 0;
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

// ─── TWO-PASS DRAW HELPER ─────────────────────────────────────────────────────
// Pass 1 (KUNING): depth GL_GREATER + depthRange normal  -> hanya fragment
//                  yang ada DI BALIK tembok yang lolos.  DepthMask=FALSE
//                  supaya depth buffer tidak rusak utk pass 2.
// Pass 2 (HIJAU) : depth GL_ALWAYS + depthRange(0,0)    -> ped tampil di
//                  atas segalanya (visible). DepthMask=TRUE -> depth=0.0
//                  ditulis agar ped "menang" dari objek berikutnya.
//
// Setelah dua pass selesai, depth state dikembalikan ke nilai game asli
// supaya rendering frame berikutnya tidak terganggu.

static inline void set_uniform_color(const float col[4]) {
    // Langsung set uniform tanpa cek g_we_overrode_color
    // Dipanggil dari dalam two-pass, loc sudah pasti resolved (bukan -2)
    if (g_materialDiffuse_loc >= 0)
        orig_glUniform4fv(g_materialDiffuse_loc, 1, col);
    if (g_materialAmbient_loc >= 0)
        orig_glUniform4fv(g_materialAmbient_loc, 1, col);
}

static void two_pass_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    force_opaque_state();

    // ── Pass 1: KUNING – ped di balik tembok ──────────────────────────────
    if (orig_glDepthRangef)
        orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
    orig_glDepthFunc(GL_GREATER);
    orig_glDepthMask(GL_FALSE);
    set_uniform_color(g_color_behind);
    orig_glDrawElements(mode, count, type, indices);

    // ── Pass 2: HIJAU – ped terlihat normal ───────────────────────────────
    if (orig_glDepthRangef)
        orig_glDepthRangef(0.0f, 0.0f);
    orig_glDepthFunc(GL_ALWAYS);
    orig_glDepthMask(GL_TRUE);
    set_uniform_color(g_color);
    orig_glDrawElements(mode, count, type, indices);

    // ── Restore depth state game ──────────────────────────────────────────
    if (orig_glDepthRangef)
        orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
    orig_glDepthFunc(g_game_depth_func);
    orig_glDepthMask(g_game_depth_mask);
}

static void two_pass_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    force_opaque_state();

    // ── Pass 1: KUNING ────────────────────────────────────────────────────
    if (orig_glDepthRangef)
        orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
    orig_glDepthFunc(GL_GREATER);
    orig_glDepthMask(GL_FALSE);
    set_uniform_color(g_color_behind);
    orig_glDrawArrays(mode, first, count);

    // ── Pass 2: HIJAU ─────────────────────────────────────────────────────
    if (orig_glDepthRangef)
        orig_glDepthRangef(0.0f, 0.0f);
    orig_glDepthFunc(GL_ALWAYS);
    orig_glDepthMask(GL_TRUE);
    set_uniform_color(g_color);
    orig_glDrawArrays(mode, first, count);

    // ── Restore ───────────────────────────────────────────────────────────
    if (orig_glDepthRangef)
        orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
    orig_glDepthFunc(g_game_depth_func);
    orig_glDepthMask(g_game_depth_mask);
}

// ─── Hook: glDrawElements / glDrawArrays ─────────────────────────────────────
static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    // Cukup is_ped=1 untuk two-pass, tidak perlu tunggu g_we_overrode_color
    // karena game kadang cache uniform dan tidak panggil glUniform4fv lagi
    if (g_enabled && g_is_ped_program && g_materialDiffuse_loc >= 0) {
        if (g_log_draw_count < 20) {
            logff_("[SOLIDSKIN] DrawElements two-pass prog=%u count=%d", g_current_program, count);
            g_log_draw_count++;
        }
        if (g_depth_bypass) {
            two_pass_draw_elements(mode, count, type, indices);
        } else {
            force_opaque_state();
            set_uniform_color(g_color);
            orig_glDrawElements(mode, count, type, indices);
        }
        return;
    }
    orig_glDrawElements(mode, count, type, indices);
}

static void hook_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (g_enabled && g_is_ped_program && g_materialDiffuse_loc >= 0) {
        if (g_log_draw_count < 20) {
            logff_("[SOLIDSKIN] DrawArrays two-pass prog=%u count=%d", g_current_program, count);
            g_log_draw_count++;
        }
        if (g_depth_bypass) {
            two_pass_draw_arrays(mode, first, count);
        } else {
            force_opaque_state();
            set_uniform_color(g_color);
            orig_glDrawArrays(mode, first, count);
        }
        return;
    }
    orig_glDrawArrays(mode, first, count);
}

// ─── Hook: glDrawArraysInstanced / glDrawElementsInstanced ──────────────────
// GTA SA Mobile pakai instanced draw untuk ped LOD / ped jauh.
// Sama persis logikanya dengan non-instanced, hanya forward ke instanced orig.

static void hook_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    if (g_enabled && g_is_ped_program && g_materialDiffuse_loc >= 0) {
        if (g_log_draw_count < 20) {
            logff_("[SOLIDSKIN] DrawArraysInstanced two-pass prog=%u count=%d inst=%d",
                   g_current_program, count, instancecount);
            g_log_draw_count++;
        }
        if (g_depth_bypass) {
            // Pass 1: KUNING - behind wall
            if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
            orig_glDepthFunc(GL_GREATER);
            orig_glDepthMask(GL_FALSE);
            set_uniform_color(g_color_behind);
            orig_glDrawArraysInstanced(mode, first, count, instancecount);
            // Pass 2: HIJAU - visible
            if (orig_glDepthRangef) orig_glDepthRangef(0.0f, 0.0f);
            orig_glDepthFunc(GL_ALWAYS);
            orig_glDepthMask(GL_TRUE);
            set_uniform_color(g_color);
            orig_glDrawArraysInstanced(mode, first, count, instancecount);
            // Restore
            if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
            orig_glDepthFunc(g_game_depth_func);
            orig_glDepthMask(g_game_depth_mask);
        } else {
            force_opaque_state();
            set_uniform_color(g_color);
            orig_glDrawArraysInstanced(mode, first, count, instancecount);
        }
        return;
    }
    orig_glDrawArraysInstanced(mode, first, count, instancecount);
}

static void hook_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                          const void* indices, GLsizei instancecount) {
    if (g_enabled && g_is_ped_program && g_materialDiffuse_loc >= 0) {
        if (g_log_draw_count < 20) {
            logff_("[SOLIDSKIN] DrawElementsInstanced two-pass prog=%u count=%d inst=%d",
                   g_current_program, count, instancecount);
            g_log_draw_count++;
        }
        if (g_depth_bypass) {
            // Pass 1: KUNING
            if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
            orig_glDepthFunc(GL_GREATER);
            orig_glDepthMask(GL_FALSE);
            set_uniform_color(g_color_behind);
            orig_glDrawElementsInstanced(mode, count, type, indices, instancecount);
            // Pass 2: HIJAU
            if (orig_glDepthRangef) orig_glDepthRangef(0.0f, 0.0f);
            orig_glDepthFunc(GL_ALWAYS);
            orig_glDepthMask(GL_TRUE);
            set_uniform_color(g_color);
            orig_glDrawElementsInstanced(mode, count, type, indices, instancecount);
            // Restore
            if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
            orig_glDepthFunc(g_game_depth_func);
            orig_glDepthMask(g_game_depth_mask);
        } else {
            force_opaque_state();
            set_uniform_color(g_color);
            orig_glDrawElementsInstanced(mode, count, type, indices, instancecount);
        }
        return;
    }
    orig_glDrawElementsInstanced(mode, count, type, indices, instancecount);
}

// ─── Hook: eglGetProcAddress (DIAGNOSTIK v2.9) ──────────────────────────────
// Game bisa load fungsi GL lewat eglGetProcAddress, bukan dlsym.
// Kita intercept untuk tahu fungsi draw apa yang di-load, dan
// replace pointer-nya kalau itu adalah draw call yang kita butuhkan.
static const char* g_draw_keywords[] = {
    "Draw", "draw", nullptr
};

static __eglMustCastToProperFunctionPointerType hook_eglGetProcAddress(const char* procname) {
    auto result = orig_eglGetProcAddress(procname);
    if (procname) {
        // Log semua proc yang mengandung kata "Draw"
        for (int i = 0; g_draw_keywords[i]; i++) {
            if (strstr(procname, g_draw_keywords[i])) {
                logff_("[SOLIDSKIN] eglGetProcAddress("%s") -> %p", procname, (void*)result);
                break;
            }
        }
    }
    return result;
}

// ─── Hook: kandidat draw tambahan (DIAGNOSTIK v2.8) ─────────────────────────
// Tujuan: cari tahu fungsi draw MANA yang dipakai prog ped yang tidak
// terdeksi oleh glDrawArrays/Elements/Instanced. Setiap fungsi log
// prog+is_ped agar kita tahu persis mana yang terpanggil.

static int g_diag_count = 0;

#define DIAG_LOG(fname) \
    if (g_is_ped_program && g_diag_count < 40) { \
        logff_("[SOLIDSKIN] DIAG " fname " prog=%u is_ped=%d diffuse=%d", \
               g_current_program, g_is_ped_program, g_materialDiffuse_loc); \
        g_diag_count++; \
    }

static void hook_glDrawRangeElements(GLenum mode, GLuint start, GLuint end,
                                      GLsizei count, GLenum type, const void* indices) {
    DIAG_LOG("glDrawRangeElements")
    if (g_enabled && g_is_ped_program && g_materialDiffuse_loc >= 0 && g_depth_bypass) {
        if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
        orig_glDepthFunc(GL_GREATER); orig_glDepthMask(GL_FALSE);
        set_uniform_color(g_color_behind);
        orig_glDrawRangeElements(mode, start, end, count, type, indices);
        if (orig_glDepthRangef) orig_glDepthRangef(0.0f, 0.0f);
        orig_glDepthFunc(GL_ALWAYS); orig_glDepthMask(GL_TRUE);
        set_uniform_color(g_color);
        orig_glDrawRangeElements(mode, start, end, count, type, indices);
        if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
        orig_glDepthFunc(g_game_depth_func); orig_glDepthMask(g_game_depth_mask);
        return;
    }
    orig_glDrawRangeElements(mode, start, end, count, type, indices);
}

static void hook_glMultiDrawArrays(GLenum mode, const GLint* first,
                                    const GLsizei* count, GLsizei drawcount) {
    DIAG_LOG("glMultiDrawArrays")
    orig_glMultiDrawArrays(mode, first, count, drawcount);
}

static void hook_glMultiDrawElements(GLenum mode, const GLsizei* count, GLenum type,
                                      const void* const* indices, GLsizei drawcount) {
    DIAG_LOG("glMultiDrawElements")
    orig_glMultiDrawElements(mode, count, type, indices, drawcount);
}

static void hook_glDrawArraysIndirect(GLenum mode, const void* indirect) {
    DIAG_LOG("glDrawArraysIndirect")
    orig_glDrawArraysIndirect(mode, indirect);
}

static void hook_glDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect) {
    DIAG_LOG("glDrawElementsIndirect")
    orig_glDrawElementsIndirect(mode, type, indirect);
}

static void hook_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                           const void* indices, GLint basevertex) {
    DIAG_LOG("glDrawElementsBaseVertex")
    if (g_enabled && g_is_ped_program && g_materialDiffuse_loc >= 0 && g_depth_bypass) {
        if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
        orig_glDepthFunc(GL_GREATER); orig_glDepthMask(GL_FALSE);
        set_uniform_color(g_color_behind);
        orig_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
        if (orig_glDepthRangef) orig_glDepthRangef(0.0f, 0.0f);
        orig_glDepthFunc(GL_ALWAYS); orig_glDepthMask(GL_TRUE);
        set_uniform_color(g_color);
        orig_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
        if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
        orig_glDepthFunc(g_game_depth_func); orig_glDepthMask(g_game_depth_mask);
        return;
    }
    orig_glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
}

// ─── Hook: glUniform4fv ───────────────────────────────────────────────────────
static void hook_glUniform4fv(GLint location, GLsizei count, const GLfloat* value) {
    if (g_hook_call_count < 10) {
        logff_("[SOLIDSKIN] glUniform4fv: loc=%d prog=%u", location, g_current_program);
        g_hook_call_count++;
    }

    if (!g_enabled || g_current_program == 0) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    // Resolve uniform location saat pertama kali program ini dipakai
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

        if (g_enabled && g_block_blend && g_is_ped_program)
            force_opaque_state();
    }

    if (!g_is_ped_program) {
        orig_glUniform4fv(location, count, value);
        return;
    }

    // Intercept warna diffuse/ambient -> set ke warna HIJAU (g_color)
    // Warna KUNING untuk behind-wall diterapkan langsung di draw call pass 1.
    if (location != -1 && location == g_materialDiffuse_loc) {
        orig_glUniform4fv(location, count, g_color);
        g_we_overrode_color = 1;
        return;
    }
    if (location != -1 && location == g_materialAmbient_loc) {
        orig_glUniform4fv(location, count, g_color);
        g_we_overrode_color = 1;
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
    void (*set_color_behind)(float r, float g, float b, float a);
    void (*get_color_behind)(float* out);
    void (*set_block_blend)(int v);
    int  (*get_block_blend)(void);
    void (*set_force_blendfunc)(int v);
    int  (*get_force_blendfunc)(void);
    void (*set_texture_override)(int v);
    int  (*get_texture_override)(void);
    void (*retry_texture_init)(void);
    int  (*is_texture_ready)(void);
    void (*set_depth_bypass)(int v);
    int  (*get_depth_bypass)(void);
};

extern "C" {

EXPORT SolidSkinAPI solidskin_api = {
    _enable, _disable, _is_enabled,
    _set_color, _get_color,
    _set_color_behind, _get_color_behind,
    _set_block_blend, _get_block_blend,
    _set_force_blendfunc, _get_force_blendfunc,
    _set_texture_override, _get_texture_override,
    _retry_texture_init, _is_texture_ready,
    _set_depth_bypass, _get_depth_bypass
};

EXPORT void* __GetModInfo() {
    static const char* info =
        "solidskin|2.9diag2|Diagnostik: eglGetProcAddress + trace prog 149 draw call|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v2.9diag2 (+ eglGetProcAddress hook + trace prog 149)");

    g_enabled                   = 0;
    g_current_program           = 0;
    g_materialDiffuse_loc       = -2;
    g_materialAmbient_loc       = -2;
    g_hook_call_count           = 0;
    g_is_ped_program            = 0;
    g_blend_currently_on        = 0;
    g_we_overrode_color         = 0;
    g_log_draw_count            = 0;
    g_diag_count                = 0;
    g_trace_active              = 0;
    g_trace_log_count           = 0;
    g_log_bind_count            = 0;
    g_block_blend               = 1;
    g_force_blendfunc           = 1;
    g_texture_override          = 1;
    g_depth_bypass              = 1;
    g_solid_tex                 = 0;
    g_solid_tex_ready           = 0;
    g_solid_tex_init_attempted  = 0;
    g_active_texture_unit       = GL_TEXTURE0;
    g_game_depth_func           = GL_LESS;
    g_game_depth_mask           = GL_TRUE;
    g_game_depth_range_near     = 0.0f;
    g_game_depth_range_far      = 1.0f;
    g_program_cache.clear();

    // Visible = HIJAU
    g_color[0] = 0.0f; g_color[1] = 1.0f; g_color[2] = 0.0f; g_color[3] = 1.0f;
    // Behind wall = KUNING
    g_color_behind[0] = 1.0f; g_color_behind[1] = 1.0f;
    g_color_behind[2] = 0.0f; g_color_behind[3] = 1.0f;
}

EXPORT void OnModLoad() {
    logf_("[SOLIDSKIN] OnModLoad v2.9diag2 mulai");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { logf_("[SOLIDSKIN] ERROR: libdobby.so tidak ditemukan"); return; }
    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { logf_("[SOLIDSKIN] ERROR: DobbyHook sym tidak ada"); return; }

    void* hGLES2 = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES2) { logf_("[SOLIDSKIN] ERROR: libGLESv2.so tidak ada"); return; }

    // ── glGetUniformLocation (tidak di-hook, dipakai langsung) ──
    orig_glGetUniformLocation = (glGetUniformLocation_t)dlsym(hGLES2, "glGetUniformLocation");
    if (!orig_glGetUniformLocation) { logf_("[SOLIDSKIN] ERROR: glGetUniformLocation null"); return; }
    logf_("[SOLIDSKIN] glGetUniformLocation OK");

    orig_glGetError = (glGetError_t)dlsym(hGLES2, "glGetError");

    // ── Hook semua fungsi GL yang diperlukan ──────────────────────────────
    #define HOOK(name)                                                              \
        orig_##name = (name##_t)dlsym(hGLES2, #name);                             \
        if (!orig_##name) { logf_("[SOLIDSKIN] ERROR: " #name " null"); return; }  \
        if (dobbyHook((void*)orig_##name, (void*)hook_##name,                      \
                      (void**)&orig_##name) != 0) {                                \
            logf_("[SOLIDSKIN] ERROR: hook " #name " gagal"); return;              \
        }                                                                           \
        logf_("[SOLIDSKIN] hook " #name " OK");

    HOOK(glUseProgram)
    HOOK(glUniform4fv)
    HOOK(glEnable)
    HOOK(glDisable)
    HOOK(glBlendFunc)
    HOOK(glDrawElements)
    HOOK(glDrawArrays)
    HOOK(glActiveTexture)
    HOOK(glBindTexture)
    HOOK(glDepthFunc)
    HOOK(glDepthMask)

    #undef HOOK

    // ── Hook opsional (warning saja jika gagal) ───────────────────────────
    #define HOOK_WARN(name)                                                         \
        orig_##name = (name##_t)dlsym(hGLES2, #name);                             \
        if (orig_##name) {                                                          \
            if (dobbyHook((void*)orig_##name, (void*)hook_##name,                  \
                          (void**)&orig_##name) != 0) {                            \
                logf_("[SOLIDSKIN] WARNING: hook " #name " gagal");                \
                orig_##name = nullptr;                                              \
            } else {                                                                \
                logf_("[SOLIDSKIN] hook " #name " OK");                            \
            }                                                                       \
        } else {                                                                    \
            logf_("[SOLIDSKIN] WARNING: " #name " tidak ditemukan");               \
        }

    HOOK_WARN(glGenTextures)
    HOOK_WARN(glTexImage2D)
    HOOK_WARN(glTexParameteri)
    HOOK_WARN(glDepthRangef)
    HOOK_WARN(glDrawArraysInstanced)
    HOOK_WARN(glDrawElementsInstanced)
    HOOK_WARN(glDrawRangeElements)
    HOOK_WARN(glMultiDrawArrays)
    HOOK_WARN(glMultiDrawElements)
    HOOK_WARN(glDrawArraysIndirect)
    HOOK_WARN(glDrawElementsIndirect)
    HOOK_WARN(glDrawElementsBaseVertex)

    #undef HOOK_WARN

    // ── Hook eglGetProcAddress dari libEGL.so ─────────────────────────────
    void* hEGL = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (hEGL) {
        orig_eglGetProcAddress = (eglGetProcAddress_t)dlsym(hEGL, "eglGetProcAddress");
        if (orig_eglGetProcAddress) {
            if (dobbyHook((void*)orig_eglGetProcAddress, (void*)hook_eglGetProcAddress,
                          (void**)&orig_eglGetProcAddress) == 0) {
                logf_("[SOLIDSKIN] hook eglGetProcAddress OK");
            } else {
                logf_("[SOLIDSKIN] WARNING: hook eglGetProcAddress gagal");
                orig_eglGetProcAddress = nullptr;
            }
        }
    } else {
        logf_("[SOLIDSKIN] WARNING: libEGL.so tidak ditemukan");
    }

    // ── Coba load draw func dari libGLESv3.so juga ───────────────────────
    void* hGLES3 = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
    if (hGLES3) {
        logf_("[SOLIDSKIN] INFO: libGLESv3.so ditemukan");
        // Cek apakah draw func ada di sini dan berbeda dari libGLESv2
        void* gles3_drawArrays = dlsym(hGLES3, "glDrawArrays");
        void* gles2_drawArrays = (void*)orig_glDrawArrays; // sudah di-hook, ambil dari ptr asli
        logff_("[SOLIDSKIN] glDrawArrays: gles2=%p gles3=%p same=%d",
               gles2_drawArrays, gles3_drawArrays,
               gles2_drawArrays == gles3_drawArrays ? 1 : 0);
    } else {
        logf_("[SOLIDSKIN] INFO: libGLESv3.so tidak ada (normal)");
    }

    // Tulis alamat API utk Lua/AML consumer
    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    g_enabled = 1;
    logf_("[SOLIDSKIN] OnModLoad SELESAI v2.9diag2 - auto enabled");
    logf_("[SOLIDSKIN] KUNING = di balik tembok, HIJAU = kelihatan (v2.9diag2: eglGetProcAddress + trace prog 149)");
}

} // extern "C"
