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
static float g_color_visible[4]  = {0.0f, 1.0f, 0.0f, 1.0f}; // hijau: terlihat normal
static float g_color_occluded[4] = {1.0f, 1.0f, 0.0f, 1.0f}; // kuning: tertutup tembok
static int   g_block_blend = 1;
static int   g_force_blendfunc = 1;
static int   g_texture_override = 1;
static int   g_depth_bypass = 1;     // master toggle utk seluruh fitur dual-pass wallhack

// ─── Solid texture: SEKARANG DUA buah, satu per warna ───────────────────────
static GLuint g_solid_tex_visible  = 0;
static GLuint g_solid_tex_occluded = 0;
static int    g_solid_tex_ready = 0;
static int    g_solid_tex_init_attempted = 0;
static uint8_t g_tex_rgba_visible[4]  = {0, 255, 0, 255};
static uint8_t g_tex_rgba_occluded[4] = {255, 255, 0, 255};
static int g_active_texture_unit = GL_TEXTURE0;

// ─── Program cache ────────────────────────────────────────────────────────────
struct ProgramInfo {
    GLint diffuse_loc;
    GLint ambient_loc;
    int   is_ped;
};
static std::unordered_map<GLuint, ProgramInfo> g_program_cache;

// ─── Function pointer types ───────────────────────────────────────────────────
typedef void   (*glUniform4fv_t)(GLint, GLsizei, const GLfloat*);
typedef void   (*glUseProgram_t)(GLuint);
typedef GLint  (*glGetUniformLocation_t)(GLuint, const char*);
typedef void   (*glEnable_t)(GLenum);
typedef void   (*glDisable_t)(GLenum);
typedef void   (*glBlendFunc_t)(GLenum, GLenum);
typedef void   (*glDrawElements_t)(GLenum, GLsizei, GLenum, const void*);
typedef void   (*glDrawArrays_t)(GLenum, GLint, GLsizei);
typedef void   (*glBindTexture_t)(GLenum, GLuint);
typedef void   (*glActiveTexture_t)(GLenum);
typedef void   (*glGenTextures_t)(GLsizei, GLuint*);
typedef void   (*glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void   (*glTexParameteri_t)(GLenum, GLenum, GLint);
typedef GLenum (*glGetError_t)(void);
typedef void   (*glDepthFunc_t)(GLenum);
typedef void   (*glDepthMask_t)(GLboolean);
typedef void   (*glDepthRangef_t)(GLfloat, GLfloat);
typedef void   (*glStencilFunc_t)(GLenum, GLint, GLuint);
typedef void   (*glStencilOp_t)(GLenum, GLenum, GLenum);
typedef void   (*glStencilMask_t)(GLuint);
typedef void   (*glClear_t)(GLbitfield);
typedef void   (*glClearStencil_t)(GLint);

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
static glStencilFunc_t        orig_glStencilFunc        = nullptr;
static glStencilOp_t          orig_glStencilOp          = nullptr;
static glStencilMask_t        orig_glStencilMask        = nullptr;
static glClear_t              orig_glClear              = nullptr;
static glClearStencil_t       orig_glClearStencil       = nullptr;

// ─── Runtime state ───────────────────────────────────────────────────────────
static GLuint g_current_program       = 0;
static GLint  g_materialDiffuse_loc   = -2;
static GLint  g_materialAmbient_loc   = -2;
static int    g_is_ped_program        = 0;
static int    g_blend_currently_on    = 0;
static int    g_we_overrode_color     = 0;
static int    g_log_draw_count        = 0;
static int    g_log_bind_count        = 0;

static GLenum     g_game_depth_func        = GL_LESS;
static GLboolean  g_game_depth_mask        = GL_TRUE;
static GLfloat    g_game_depth_range_near  = 0.0f;
static GLfloat    g_game_depth_range_far   = 1.0f;
static int        g_stencil_buffer_available = 0; // diketahui setelah cek error pertama kali

// Re-entrancy guard: saat kita lakukan replay pass kedua secara manual di
// dalam hook draw call, kita akan memanggil orig_glDrawElements/Arrays lagi
// dari dalam hook itu sendiri -- harus dibedakan dari draw call asli game.
static int g_in_manual_redraw = 0;

// ─── Internal helpers ────────────────────────────────────────────────────────
static void sync_solid_textures(void);
static void try_init_solid_textures(void);

static void _enable(void)  { g_enabled = 1; logf_("[SOLIDSKIN] enabled"); }
static void _disable(void) { g_enabled = 0; logf_("[SOLIDSKIN] disabled"); }
static int  _is_enabled(void) { return g_enabled; }

static void _set_color_visible(float r, float g, float b, float a) {
    g_color_visible[0] = r; g_color_visible[1] = g; g_color_visible[2] = b; g_color_visible[3] = a;
    logff_("[SOLIDSKIN] color_visible set: %.2f %.2f %.2f %.2f", r, g, b, a);
    sync_solid_textures();
}
static void _get_color_visible(float* out) {
    out[0] = g_color_visible[0]; out[1] = g_color_visible[1];
    out[2] = g_color_visible[2]; out[3] = g_color_visible[3];
}
static void _set_color_occluded(float r, float g, float b, float a) {
    g_color_occluded[0] = r; g_color_occluded[1] = g; g_color_occluded[2] = b; g_color_occluded[3] = a;
    logff_("[SOLIDSKIN] color_occluded set: %.2f %.2f %.2f %.2f", r, g, b, a);
    sync_solid_textures();
}
static void _get_color_occluded(float* out) {
    out[0] = g_color_occluded[0]; out[1] = g_color_occluded[1];
    out[2] = g_color_occluded[2]; out[3] = g_color_occluded[3];
}

static void _set_block_blend(int v) { g_block_blend = v ? 1 : 0; }
static int  _get_block_blend(void) { return g_block_blend; }
static void _set_force_blendfunc(int v) { g_force_blendfunc = v ? 1 : 0; }
static int  _get_force_blendfunc(void) { return g_force_blendfunc; }
static void _set_texture_override(int v) { g_texture_override = v ? 1 : 0; }
static int  _get_texture_override(void) { return g_texture_override; }
static void _retry_texture_init(void) { g_solid_tex_init_attempted = 0; }
static int  _is_texture_ready(void) { return g_solid_tex_ready; }
static void _set_depth_bypass(int v) {
    g_depth_bypass = v ? 1 : 0;
    logff_("[SOLIDSKIN] depth_bypass (dual-pass wallhack) set: %d", g_depth_bypass);
}
static int  _get_depth_bypass(void) { return g_depth_bypass; }

// ─── Helper: paksa state non-transparan ─────────────────────────────────────
static inline void force_opaque_state(void) {
    if (g_blend_currently_on) {
        orig_glDisable(GL_BLEND);
    }
    if (g_force_blendfunc) {
        orig_glBlendFunc(GL_ONE, GL_ZERO);
    }
}

// ─── Solid textures: dua warna, lazy init di render thread ──────────────────
static void upload_one_solid_tex(GLuint tex, const uint8_t rgba[4]) {
    if (!tex || !orig_glBindTexture || !orig_glTexImage2D) return;
    orig_glBindTexture(GL_TEXTURE_2D, tex);
    orig_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (orig_glTexParameteri) {
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        orig_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

static void sync_solid_textures(void) {
    g_tex_rgba_visible[0] = (uint8_t)(g_color_visible[0] * 255.0f + 0.5f);
    g_tex_rgba_visible[1] = (uint8_t)(g_color_visible[1] * 255.0f + 0.5f);
    g_tex_rgba_visible[2] = (uint8_t)(g_color_visible[2] * 255.0f + 0.5f);
    g_tex_rgba_visible[3] = 255;

    g_tex_rgba_occluded[0] = (uint8_t)(g_color_occluded[0] * 255.0f + 0.5f);
    g_tex_rgba_occluded[1] = (uint8_t)(g_color_occluded[1] * 255.0f + 0.5f);
    g_tex_rgba_occluded[2] = (uint8_t)(g_color_occluded[2] * 255.0f + 0.5f);
    g_tex_rgba_occluded[3] = 255;

    if (g_solid_tex_ready) {
        upload_one_solid_tex(g_solid_tex_visible, g_tex_rgba_visible);
        upload_one_solid_tex(g_solid_tex_occluded, g_tex_rgba_occluded);
        logf_("[SOLIDSKIN] solid textures diupload ulang (visible+occluded)");
    }
}

static void try_init_solid_textures(void) {
    if (g_solid_tex_ready || g_solid_tex_init_attempted) return;
    g_solid_tex_init_attempted = 1;

    if (!orig_glGenTextures || !orig_glBindTexture || !orig_glTexImage2D) {
        logf_("[SOLIDSKIN] ERROR: fungsi texture belum siap, skip init solid textures");
        return;
    }

    GLuint tex_v = 0, tex_o = 0;
    orig_glGenTextures(1, &tex_v);
    orig_glGenTextures(1, &tex_o);

    if (orig_glGetError) {
        GLenum err = orig_glGetError();
        if (err != GL_NO_ERROR) {
            logff_("[SOLIDSKIN] glGenTextures glGetError=0x%x", err);
        }
    }

    if (tex_v == 0 || tex_o == 0) {
        logf_("[SOLIDSKIN] ERROR: glGenTextures gagal alokasi salah satu/kedua texture");
        return;
    }

    g_solid_tex_visible  = tex_v;
    g_solid_tex_occluded = tex_o;
    sync_solid_textures();
    g_solid_tex_ready = 1;
    upload_one_solid_tex(g_solid_tex_visible, g_tex_rgba_visible);
    upload_one_solid_tex(g_solid_tex_occluded, g_tex_rgba_occluded);
    logff_("[SOLIDSKIN] solid textures dibuat: visible=%u occluded=%u", g_solid_tex_visible, g_solid_tex_occluded);
}

// ─── Hook: glActiveTexture / glBindTexture ──────────────────────────────────
// Pass 1 (visible, hijau) butuh tex_visible. Pass 2 (occluded, kuning) butuh
// tex_occluded. Kita tentukan target texture lewat flag g_in_manual_redraw:
// false = pass 1 (dipicu game), true = pass 2 (kita yg replay manual).
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

    if (g_enabled && g_texture_override && !g_solid_tex_ready) {
        try_init_solid_textures();
    }

    if (g_enabled && g_texture_override && g_solid_tex_ready &&
        target == GL_TEXTURE_2D && g_is_ped_program &&
        texture != g_solid_tex_visible && texture != g_solid_tex_occluded) {

        GLuint chosen = g_in_manual_redraw ? g_solid_tex_occluded : g_solid_tex_visible;

        if (g_log_bind_count < 30) {
            logff_("[SOLIDSKIN] glBindTexture: redirect tex=%u -> %u (pass=%s, prog=%u)",
                   texture, chosen, g_in_manual_redraw ? "occluded" : "visible", g_current_program);
            g_log_bind_count++;
        }
        in_self_bind = 1;
        orig_glBindTexture(target, chosen);
        in_self_bind = 0;
        return;
    }

    orig_glBindTexture(target, texture);
}

static void hook_glGenTextures(GLsizei n, GLuint* textures) { orig_glGenTextures(n, textures); }
static void hook_glTexImage2D(GLenum target, GLint level, GLint internalformat,
                               GLsizei width, GLsizei height, GLint border,
                               GLenum format, GLenum type, const void* pixels) {
    orig_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
static void hook_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    orig_glTexParameteri(target, pname, param);
}

// ─── Hook: glDepthFunc / glDepthMask / glDepthRangef ────────────────────────
// Selama PASS 1 (g_in_manual_redraw == false, draw asli dari game): kita
// TIDAK mengubah apapun terkait depth -- biarkan depth test ASLI/NORMAL
// (GL_LESS, mask ON, range asli) supaya occlusion ke tembok terjadi secara
// natural, sekaligus berfungsi sbg "test" apakah ped kelihatan atau tidak.
//
// Selama PASS 2 (g_in_manual_redraw == true, replay manual kita):
// depth dipaksa ALWAYS + range (0,0) seperti v2.3, supaya tembus tembok.
static void hook_glDepthFunc(GLenum func) {
    g_game_depth_func = func;
    if (g_enabled && g_depth_bypass && g_is_ped_program && g_in_manual_redraw) {
        orig_glDepthFunc(GL_ALWAYS);
        return;
    }
    orig_glDepthFunc(func);
}

static void hook_glDepthMask(GLboolean flag) {
    g_game_depth_mask = flag;
    if (g_enabled && g_depth_bypass && g_is_ped_program && g_in_manual_redraw) {
        orig_glDepthMask(GL_TRUE);
        return;
    }
    orig_glDepthMask(flag);
}

static void hook_glDepthRangef(GLfloat n, GLfloat f) {
    g_game_depth_range_near = n;
    g_game_depth_range_far  = f;
    if (g_enabled && g_depth_bypass && g_is_ped_program && g_in_manual_redraw) {
        orig_glDepthRangef(0.0f, 0.0f);
        return;
    }
    orig_glDepthRangef(n, f);
}

// ─── Hook: glUseProgram ───────────────────────────────────────────────────────
static void hook_glUseProgram(GLuint program) {
    if (g_enabled && g_texture_override && !g_solid_tex_ready) {
        try_init_solid_textures();
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

// ─── Helper: set warna uniform manual (dipakai utk pass 2) ──────────────────
static void set_uniform_color_now(const float color[4], GLboolean alpha_one) {
    if (g_materialDiffuse_loc != -1 && g_materialDiffuse_loc >= 0) {
        orig_glUniform4fv(g_materialDiffuse_loc, 1, color);
    }
    if (g_materialAmbient_loc != -1 && g_materialAmbient_loc >= 0) {
        GLfloat ambient[4] = {color[0], color[1], color[2], alpha_one ? 1.0f : color[3]};
        orig_glUniform4fv(g_materialAmbient_loc, 1, ambient);
    }
}

// ─── Stencil setup per pass ──────────────────────────────────────────────────
// Pass 1 (visible/hijau): stencil func ALWAYS, op REPLACE saat depth pass ->
// tulis stencil=1 di pixel yg LOLOS depth test asli (artinya benar2 kelihatan).
// Stencil TIDAK diubah jika depth test gagal (op KEEP).
static inline void setup_stencil_pass1(void) {
    if (!g_stencil_buffer_available) return;
    orig_glEnable(GL_STENCIL_TEST);
    orig_glStencilFunc(GL_ALWAYS, 1, 0xFF);
    orig_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); // sfail, dpfail, dppass
    orig_glStencilMask(0xFF);
}

// Pass 2 (occluded/kuning): hanya gambar di pixel yg stencil-nya BELUM 1
// (artinya pass 1 gagal di situ -> tertutup tembok). stencil func NOTEQUAL 1.
static inline void setup_stencil_pass2(void) {
    if (!g_stencil_buffer_available) return;
    orig_glEnable(GL_STENCIL_TEST);
    orig_glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    orig_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); // jangan ubah stencil lagi di pass 2
    orig_glStencilMask(0x00); // read-only di pass ini
}

static inline void restore_stencil_to_game_state(void) {
    if (!g_stencil_buffer_available) return;
    orig_glDisable(GL_STENCIL_TEST);
}

// ─── Hook: glDrawElements / glDrawArrays ────────────────────────────────────
// Inti dual-pass: kalau ini draw call ped yg sudah di-override warnanya,
// JANGAN langsung pass-through ke orig draw -- lakukan urutan:
//   1) setup stencil pass1, set warna visible, panggil orig draw (PASS 1)
//   2) setup stencil pass2 + depth bypass, set warna occluded,
//      panggil orig draw LAGI dgn parameter sama (PASS 2, via flag manual)
//   3) restore semua state depth/stencil ke kondisi normal
// Draw call NON-ped tetap pass-through normal, tidak disentuh sama sekali.
static inline void do_dual_pass_draw(
    void (*draw_call)(void)
) {
    // PASS 1: visible (hijau), depth test NORMAL (apa adanya dari game saat ini)
    setup_stencil_pass1();
    set_uniform_color_now(g_color_visible, GL_TRUE);
    force_opaque_state();
    draw_call();

    // PASS 2: occluded (kuning), depth bypass, hanya area gagal stencil
    g_in_manual_redraw = 1;
    setup_stencil_pass2();
    set_uniform_color_now(g_color_occluded, GL_TRUE);
    // re-apply depth override skrg krn g_in_manual_redraw baru jadi true
    if (orig_glDepthFunc)   orig_glDepthFunc(GL_ALWAYS);
    if (orig_glDepthMask)   orig_glDepthMask(GL_TRUE);
    if (orig_glDepthRangef) orig_glDepthRangef(0.0f, 0.0f);
    force_opaque_state();
    draw_call();
    g_in_manual_redraw = 0;

    // Restore depth & stencil ke state asli game utk draw call berikutnya.
    if (orig_glDepthFunc)   orig_glDepthFunc(g_game_depth_func);
    if (orig_glDepthMask)   orig_glDepthMask(g_game_depth_mask);
    if (orig_glDepthRangef) orig_glDepthRangef(g_game_depth_range_near, g_game_depth_range_far);
    restore_stencil_to_game_state();
}

// Parameter draw call disimpan sementara lewat closure-style static (GLES2
// C API tidak punya lambda capture native, jadi kita pakai variabel statis
// + dua varian draw_call kecil utk Elements vs Arrays).
static GLenum g_pending_mode;
static GLsizei g_pending_count;
static GLenum g_pending_type;
static const void* g_pending_indices;
static GLint g_pending_first;

static void replay_draw_elements(void) {
    orig_glDrawElements(g_pending_mode, g_pending_count, g_pending_type, g_pending_indices);
}
static void replay_draw_arrays(void) {
    orig_glDrawArrays(g_pending_mode, g_pending_first, g_pending_count);
}

static int g_log_dualpass_count = 0;

static void hook_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (g_enabled && g_depth_bypass && g_is_ped_program && g_we_overrode_color && !g_in_manual_redraw) {
        if (g_log_dualpass_count < 10) {
            logff_("[SOLIDSKIN] dual-pass glDrawElements prog=%u stencil_avail=%d", g_current_program, g_stencil_buffer_available);
            g_log_dualpass_count++;
        }
        g_pending_mode = mode; g_pending_count = count; g_pending_type = type; g_pending_indices = indices;
        do_dual_pass_draw(replay_draw_elements);
        return;
    }
    orig_glDrawElements(mode, count, type, indices);
}

static void hook_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (g_enabled && g_depth_bypass && g_is_ped_program && g_we_overrode_color && !g_in_manual_redraw) {
        if (g_log_dualpass_count < 10) {
            logff_("[SOLIDSKIN] dual-pass glDrawArrays prog=%u stencil_avail=%d", g_current_program, g_stencil_buffer_available);
            g_log_dualpass_count++;
        }
        g_pending_mode = mode; g_pending_first = first; g_pending_count = count;
        do_dual_pass_draw(replay_draw_arrays);
        return;
    }
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

    // Override warna awal pakai g_color_visible (PASS 1 belum dimulai resmi
    // sampai draw call dipanggil -- ini hanya menandai g_we_overrode_color=1
    // supaya hook draw tahu program ini sudah siap utk dual-pass).
    if (location != -1 && location == g_materialDiffuse_loc) {
        orig_glUniform4fv(location, count, g_color_visible);
        g_we_overrode_color = 1;
        force_opaque_state();
        return;
    }
    if (location != -1 && location == g_materialAmbient_loc) {
        GLfloat ambient[4] = {g_color_visible[0], g_color_visible[1], g_color_visible[2], 1.0f};
        orig_glUniform4fv(location, count, ambient);
        g_we_overrode_color = 1;
        force_opaque_state();
        return;
    }

    orig_glUniform4fv(location, count, value);
}

// ─── Deteksi stencil buffer availability (best-effort, via glGetError) ─────
static void detect_stencil_availability(void) {
    if (!orig_glEnable || !orig_glGetError) { g_stencil_buffer_available = 0; return; }
    orig_glEnable(GL_STENCIL_TEST);
    GLenum err = orig_glGetError();
    orig_glDisable(GL_STENCIL_TEST);
    g_stencil_buffer_available = (err == GL_NO_ERROR) ? 1 : 0;
    logff_("[SOLIDSKIN] deteksi stencil buffer: available=%d (err=0x%x)", g_stencil_buffer_available, err);
}

// ─── API struct ──────────────────────────────────────────────────────────────
struct SolidSkinAPI {
    void (*enable)(void);
    void (*disable)(void);
    int  (*is_enabled)(void);
    void (*set_color_visible)(float r, float g, float b, float a);
    void (*get_color_visible)(float* out);
    void (*set_color_occluded)(float r, float g, float b, float a);
    void (*get_color_occluded)(float* out);
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
    _set_color_visible, _get_color_visible,
    _set_color_occluded, _get_color_occluded,
    _set_block_blend, _get_block_blend,
    _set_force_blendfunc, _get_force_blendfunc,
    _set_texture_override, _get_texture_override,
    _retry_texture_init, _is_texture_ready,
    _set_depth_bypass, _get_depth_bypass
};

EXPORT void* __GetModInfo() {
    static const char* info = "solidskin|2.4|Dual-pass wallhack: hijau jika terlihat, kuning jika tertutup|brruham";
    return (void*)info;
}

EXPORT void OnModPreLoad() {
    remove(LOGFILE);
    logf_("[SOLIDSKIN] OnModPreLoad v2.4");
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
    g_log_dualpass_count        = 0;
    g_block_blend               = 1;
    g_force_blendfunc           = 1;
    g_texture_override          = 1;
    g_depth_bypass              = 1;
    g_solid_tex_visible         = 0;
    g_solid_tex_occluded        = 0;
    g_solid_tex_ready           = 0;
    g_solid_tex_init_attempted  = 0;
    g_active_texture_unit       = GL_TEXTURE0;
    g_game_depth_func           = GL_LESS;
    g_game_depth_mask           = GL_TRUE;
    g_game_depth_range_near     = 0.0f;
    g_game_depth_range_far      = 1.0f;
    g_stencil_buffer_available  = 0;
    g_in_manual_redraw          = 0;
    g_program_cache.clear();
    g_color_visible[0]  = 0.0f; g_color_visible[1]  = 1.0f; g_color_visible[2]  = 0.0f; g_color_visible[3]  = 1.0f;
    g_color_occluded[0] = 1.0f; g_color_occluded[1] = 1.0f; g_color_occluded[2] = 0.0f; g_color_occluded[3] = 1.0f;
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
    if (!orig_glGetError) logf_("[SOLIDSKIN] WARNING: glGetError tidak ditemukan");

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

    orig_glDepthFunc = (glDepthFunc_t)dlsym(hGLES2, "glDepthFunc");
    if (!orig_glDepthFunc) { logf_("[SOLIDSKIN] ERROR: glDepthFunc null"); return; }
    if (dobbyHook((void*)orig_glDepthFunc, (void*)hook_glDepthFunc, (void**)&orig_glDepthFunc) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glDepthFunc gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glDepthFunc OK");

    orig_glDepthMask = (glDepthMask_t)dlsym(hGLES2, "glDepthMask");
    if (!orig_glDepthMask) { logf_("[SOLIDSKIN] ERROR: glDepthMask null"); return; }
    if (dobbyHook((void*)orig_glDepthMask, (void*)hook_glDepthMask, (void**)&orig_glDepthMask) != 0) {
        logf_("[SOLIDSKIN] ERROR: hook glDepthMask gagal"); return;
    }
    logf_("[SOLIDSKIN] hook glDepthMask OK");

    orig_glDepthRangef = (glDepthRangef_t)dlsym(hGLES2, "glDepthRangef");
    if (!orig_glDepthRangef) {
        logf_("[SOLIDSKIN] WARNING: glDepthRangef tidak ditemukan");
    } else {
        if (dobbyHook((void*)orig_glDepthRangef, (void*)hook_glDepthRangef, (void**)&orig_glDepthRangef) != 0) {
            logf_("[SOLIDSKIN] WARNING: hook glDepthRangef gagal");
            orig_glDepthRangef = nullptr;
        } else {
            logf_("[SOLIDSKIN] hook glDepthRangef OK");
        }
    }

    // ── Stencil functions (TIDAK di-hook, dipakai LANGSUNG oleh kita) ──
    orig_glStencilFunc = (glStencilFunc_t)dlsym(hGLES2, "glStencilFunc");
    orig_glStencilOp   = (glStencilOp_t)dlsym(hGLES2, "glStencilOp");
    orig_glStencilMask = (glStencilMask_t)dlsym(hGLES2, "glStencilMask");
    orig_glClear       = (glClear_t)dlsym(hGLES2, "glClear");
    orig_glClearStencil= (glClearStencil_t)dlsym(hGLES2, "glClearStencil");

    if (!orig_glStencilFunc || !orig_glStencilOp || !orig_glStencilMask) {
        logf_("[SOLIDSKIN] WARNING: fungsi stencil tidak lengkap, dual-pass occlusion TIDAK akan akurat (fallback semua occluded)");
    } else {
        logf_("[SOLIDSKIN] fungsi stencil tersedia (tidak di-hook, dipakai langsung)");
    }

    detect_stencil_availability();

    logf_("[SOLIDSKIN] solid textures akan diinisialisasi lazy di render thread");

    FILE* af = fopen("/storage/emulated/0/solidskin_addr.txt", "w");
    if (af) { fprintf(af, "%lu\n", (unsigned long)&solidskin_api); fclose(af); }

    g_enabled = 1;
    logf_("[SOLIDSKIN] OnModLoad SELESAI - auto enabled, dual-pass wallhack siap (hijau=terlihat, kuning=tertutup)");
}

} // extern "C"
