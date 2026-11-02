/* vitaGL stand-in: desktop GL (compatibility context, made by vglInitExtended in vpc_gl.c). GL 1.1 calls come from
   opengl32's exports; the rest are loaded at init and reached through the macros below. */
#pragma once
#ifndef APIENTRY
#define APIENTRY __stdcall
#endif
#ifndef WINGDIAPI
#define WINGDIAPI __declspec(dllimport)
#endif
#include <GL/gl.h>
#include <GL/glext.h>

#define VPC_GL_PROCS(X)                                                                                              \
    X(glActiveTexture, ACTIVETEXTURE) X(glAttachShader, ATTACHSHADER) X(glBindAttribLocation, BINDATTRIBLOCATION)   \
    X(glBindBuffer, BINDBUFFER) X(glBindFramebuffer, BINDFRAMEBUFFER) X(glBindRenderbuffer, BINDRENDERBUFFER)       \
    X(glBlendEquation, BLENDEQUATION) X(glBufferData, BUFFERDATA) X(glCheckFramebufferStatus, CHECKFRAMEBUFFERSTATUS) \
    X(glCompileShader, COMPILESHADER) X(glCompressedTexImage2D, COMPRESSEDTEXIMAGE2D)                               \
    X(glCreateProgram, CREATEPROGRAM) X(glCreateShader, CREATESHADER) X(glDeleteBuffers, DELETEBUFFERS)             \
    X(glDeleteFramebuffers, DELETEFRAMEBUFFERS) X(glDeleteShader, DELETESHADER)                                     \
    X(glDeleteRenderbuffers, DELETERENDERBUFFERS)                                                                   \
    X(glDisableVertexAttribArray, DISABLEVERTEXATTRIBARRAY) X(glEnableVertexAttribArray, ENABLEVERTEXATTRIBARRAY)   \
    X(glFramebufferRenderbuffer, FRAMEBUFFERRENDERBUFFER) X(glFramebufferTexture2D, FRAMEBUFFERTEXTURE2D)           \
    X(glGenBuffers, GENBUFFERS) X(glGenFramebuffers, GENFRAMEBUFFERS) X(glGenRenderbuffers, GENRENDERBUFFERS)       \
    X(glGenerateMipmap, GENERATEMIPMAP) X(glGetShaderInfoLog, GETSHADERINFOLOG) X(glGetShaderiv, GETSHADERIV)       \
    X(glGetProgramiv, GETPROGRAMIV) X(glGetProgramInfoLog, GETPROGRAMINFOLOG)                                       \
    X(glGetUniformLocation, GETUNIFORMLOCATION) X(glLinkProgram, LINKPROGRAM)                                       \
    X(glRenderbufferStorage, RENDERBUFFERSTORAGE) X(glShaderSource, SHADERSOURCE) X(glUniform1i, UNIFORM1I)         \
    X(glUniform4fv, UNIFORM4FV) X(glUseProgram, USEPROGRAM) X(glVertexAttrib4f, VERTEXATTRIB4F)                     \
    X(glVertexAttribPointer, VERTEXATTRIBPOINTER)
#define X(n, N) extern PFNGL##N##PROC p_##n;
VPC_GL_PROCS(X)
#undef X
#define glActiveTexture p_glActiveTexture
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glBindBuffer p_glBindBuffer
#define glBindFramebuffer p_glBindFramebuffer
#define glBindRenderbuffer p_glBindRenderbuffer
#define glBlendEquation p_glBlendEquation
#define glBufferData p_glBufferData
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glCompileShader p_glCompileShader
#define glCompressedTexImage2D p_glCompressedTexImage2D
#define glCreateProgram p_glCreateProgram
#define glCreateShader p_glCreateShader
#define glDeleteBuffers p_glDeleteBuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glDeleteShader p_glDeleteShader
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glGenBuffers p_glGenBuffers
#define glGenFramebuffers p_glGenFramebuffers
#define glGenRenderbuffers p_glGenRenderbuffers
#define glGenerateMipmap p_glGenerateMipmap
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glGetShaderiv p_glGetShaderiv
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glGetUniformLocation p_glGetUniformLocation
#define glLinkProgram p_glLinkProgram
#define glRenderbufferStorage p_glRenderbufferStorage
#define glUniform1i p_glUniform1i
#define glUniform4fv p_glUniform4fv
#define glUseProgram p_glUseProgram
#define glVertexAttrib4f p_glVertexAttrib4f
#define glVertexAttribPointer p_glVertexAttribPointer
/* vitaGL takes GLES-flavoured GLSL; vpc_gl.c gives desktop GL a #version line and drops the precision statements */
void vpc_shader_source(GLuint s, GLsizei n, const GLchar* const* src, const GLint* len);
#define glShaderSource vpc_shader_source
#define glClearDepthf(z) glClearDepth(z)
#define glDepthRangef(a, b) glDepthRange(a, b)

#define SCE_GXM_MULTISAMPLE_NONE 0
enum { VGL_MEM_VRAM, VGL_MEM_RAM };
int vglInitExtended(int pool, int w, int h, int ram, int msaa);
static inline int vglInitWithCustomThreshold(int pool, int w, int h, int ram, int cdram, int phycont, int cdlg, int msaa) {
    return vglInitExtended(pool, w, h, ram, msaa);  /* the pools are a Vita matter */
}
void vglSwapBuffers(GLboolean dialog);
size_t vglMemFree(int type);
