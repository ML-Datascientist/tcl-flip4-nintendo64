/* n64stubs.c — link-time holes left by the NOGLIDE=1 angrylion-only build.
 *
 * DMEM/IMEM: rsp-cxd4 declares these `extern pu8`; the dll-build rsp.c defined
 * them and the libretro fork dropped the definitions. module.c assigns both in
 * InitiateRSP (GET_RSP_INFO) — bare storage is all the linker needs.
 *
 * GL/osal stubs: reachable ONLY from GLideN64/parallel-RDP code paths, which
 * the frontend never selects (rdp-plugin=angrylion). Benign no-ops.
 * In the GL build (build_gl.sh defines N64HAVE_GL) the REAL definitions are
 * linked (glsm's gl pointers, osal_files_unix.c, GLideN64's gln64 entry
 * points + config) — the stubs would be duplicate symbols, so they compile out.
 *
 * gln64/Config/dyna stubs: must never execute in an angrylion build;
 * abort() so a wiring bug crashes loudly instead of rendering nothing.
 * (On ARM cross builds dyna_* come from new_dynarec.o — these stay unused.)
 */
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GLideN64 defines DMEM/IMEM (N64.cpp) and retroChangeWindow
 * (CommonAPIImpl_mupenplus.cpp) when linked; the angrylion-only tree needs
 * the stubs (rsp-cxd4 leaves the DMEM/IMEM definitions out). */
#ifndef N64HAVE_GL

uint8_t *DMEM;
uint8_t *IMEM;

void retroChangeWindow(void) { }

#endif

static void n64play_dead(const char *what)
{
   (void)what; abort();
}

/* dynarec entry points: never defined by the ARM new-dynarec objects either
 * (pure-interp fallback hooks) — same abort semantics in every build. */
void dyna_jump(void) { n64play_dead("dyna_jump"); }
void dynarec_jump_to(void) { n64play_dead("dynarec_jump_to"); }
void dyna_stop(void) { n64play_dead("dyna_stop"); }

#ifndef N64HAVE_GL

void glBindTexture(void) { }
void glBindFramebuffer(void) { }
void glBlendFuncSeparate(void) { }
void glClearDepthf(void) { }
void glEnableVertexAttribArray(void) { }
void glUseProgram(void) { }
void glValidateProgram(void) { }
void glVertexAttribPointer(void) { }
void glBlendFunc(void) { }
void glClear(void) { }
void glClearColor(void) { }
void glClearDepth(void) { }
void glColorMask(void) { }
void glCullFace(void) { }
void glDeleteTextures(void) { }
void glDepthFunc(void) { }
void glDepthMask(void) { }
void glDepthRange(void) { }
void glDisable(void) { }
void glDrawArrays(void) { }
void glDrawElements(void) { }
void glEnable(void) { }
void glFrontFace(void) { }
void glGenTextures(void) { }
void glGetError(void) { }
void glGetIntegerv(void) { }
void glLineWidth(void) { }
void glPixelStorei(void) { }
void glPolygonMode(void) { }
void glPolygonOffset(void) { }
void glReadBuffer(void) { }
void glReadPixels(void) { }
void glScissor(void) { }
void glStencilFunc(void) { }
void glStencilMask(void) { }
void glStencilOp(void) { }
void glTexSubImage2D(void) { }
void glViewport(void) { }
void initGLFunctions(void) { }
void osal_is_directory(void) { }
void osal_mkdirp(void) { }
void osal_path_existsW(void) { }
void threaded_gl_safe_shutdown(void) { }

void glActiveTexture(void) { }
void glAttachShader(void) { }
void glBindAttribLocation(void) { }
void glBindBuffer(void) { }
void glBindRenderbuffer(void) { }
void glBlendColor(void) { }
void glBlendEquation(void) { }
void glBlendEquationSeparate(void) { }
void glBufferData(void) { }
void glBufferSubData(void) { }
void glCheckFramebufferStatus(void) { }
void glCompileShader(void) { }
void glCompressedTexImage2D(void) { }
void glCreateProgram(void) { }
void glCreateShader(void) { }
void glDeleteBuffers(void) { }
void glDeleteFramebuffers(void) { }
void glDeleteProgram(void) { }
void glDeleteRenderbuffers(void) { }
void glDeleteShader(void) { }
void glDepthRangef(void) { }
void glDetachShader(void) { }
void glDisableVertexAttribArray(void) { }
void glFramebufferRenderbuffer(void) { }
void glFramebufferTexture2D(void) { }
void glGenBuffers(void) { }
void glGenFramebuffers(void) { }
void glGenRenderbuffers(void) { }
void glGenerateMipmap(void) { }
void glGetActiveUniform(void) { }
void glGetAttribLocation(void) { }
void glGetFramebufferAttachmentParameteriv(void) { }
void glGetProgramInfoLog(void) { }
void glGetProgramiv(void) { }
void glGetShaderInfoLog(void) { }
void glGetShaderiv(void) { }
void glGetString(void) { }
void glGetUniformLocation(void) { }
void glIsProgram(void) { }
void glLinkProgram(void) { }
void glRenderbufferStorage(void) { }
void glShaderSource(void) { }
void glUniform1f(void) { }
void glUniform1fv(void) { }
void glUniform1i(void) { }
void glUniform1iv(void) { }
void glUniform2f(void) { }
void glUniform2fv(void) { }
void glUniform2i(void) { }
void glUniform2iv(void) { }
void glUniform3f(void) { }
void glUniform3fv(void) { }
void glUniform4f(void) { }
void glUniform4fv(void) { }
void glUniform4i(void) { }
void glUniformMatrix4fv(void) { }
void glVertexAttrib4f(void) { }
void glVertexAttrib4fv(void) { }

void Config_LoadConfig(void) { n64play_dead("Config_LoadConfig"); }
void gln64ChangeWindow(void) { n64play_dead("gln64ChangeWindow"); }
void gln64DestroyGfxContext(void) { n64play_dead("gln64DestroyGfxContext"); }
void gln64FBGetFrameBufferInfo(void) { n64play_dead("gln64FBGetFrameBufferInfo"); }
void gln64FBRead(void) { n64play_dead("gln64FBRead"); }
void gln64FBWrite(void) { n64play_dead("gln64FBWrite"); }
void gln64InitiateGFX(void) { n64play_dead("gln64InitiateGFX"); }
void gln64MoveScreen(void) { n64play_dead("gln64MoveScreen"); }
void gln64PluginGetVersion(void) { n64play_dead("gln64PluginGetVersion"); }
void gln64ProcessDList(void) { n64play_dead("gln64ProcessDList"); }
void gln64ProcessRDPList(void) { n64play_dead("gln64ProcessRDPList"); }
void gln64ReadScreen2(void) { n64play_dead("gln64ReadScreen2"); }
void gln64ReinitGfxContext(void) { n64play_dead("gln64ReinitGfxContext"); }
void gln64RomClosed(void) { n64play_dead("gln64RomClosed"); }
void gln64RomOpen(void) { n64play_dead("gln64RomOpen"); }
void gln64SetRenderingCallback(void) { n64play_dead("gln64SetRenderingCallback"); }
void gln64ShowCFB(void) { n64play_dead("gln64ShowCFB"); }
void gln64_thr_gl_invoke_command_loop(void) { n64play_dead("gln64_thr_gl_invoke_command_loop"); }
void gln64UpdateScreen(void) { n64play_dead("gln64UpdateScreen"); }
void gln64ViStatusChanged(void) { n64play_dead("gln64ViStatusChanged"); }
void gln64ViWidthChanged(void) { n64play_dead("gln64ViWidthChanged"); }

#endif /* !N64HAVE_GL */
#ifdef __cplusplus
}
#endif
