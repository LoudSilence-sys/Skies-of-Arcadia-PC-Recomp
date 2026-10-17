/* glmini.h - the small subset of OpenGL 3.3 core we use, loaded at runtime */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei;
typedef float GLfloat; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef char GLchar;
typedef ptrdiff_t GLsizeiptr; typedef ptrdiff_t GLintptr; typedef void GLvoid; typedef uint8_t GLubyte;

#define GL_TRIANGLES 0x0004
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_SRC1_ALPHA 0x8589
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_CLAMP 0x864F
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_MAX_SAMPLES 0x8D57
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_MULTISAMPLE 0x809D
#define GL_CLIP_DISTANCE0 0x3000
#define GL_CLIP_DISTANCE1 0x3001
#define GL_CLIP_DISTANCE2 0x3002
#define GL_CLIP_DISTANCE3 0x3003
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT 0x1902
#define GL_UNSIGNED_INT 0x1405
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_CULL_FACE 0x0B44
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

#define GLFUNCS(X) \
  X(void, glEnable, (GLenum)) X(void, glDisable, (GLenum)) X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
  X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, glClearDepth, (double)) X(void, glClear, (GLbitfield)) X(void, glDepthFunc, (GLenum)) X(void, glDepthMask, (GLboolean)) \
  X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) X(void, glBlendFunc, (GLenum, GLenum)) \
  X(void, glBlendEquation, (GLenum)) X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) X(void, glBlendEquationSeparate, (GLenum, GLenum)) X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
  X(void, glPixelStorei, (GLenum, GLint)) X(void, glGenTextures, (GLsizei, GLuint*)) X(void, glDeleteTextures, (GLsizei, const GLuint*)) X(void, glGenerateMipmap, (GLenum)) \
  X(void, glCompressedTexImage2D, (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*)) X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
  X(void, glGetIntegerv, (GLenum, GLint*)) X(void, glGetFloatv, (GLenum, GLfloat*)) \
  X(void, glBindTexture, (GLenum, GLuint)) X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
  X(void, glTexParameteri, (GLenum, GLenum, GLint)) X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) X(void, glActiveTexture, (GLenum)) X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
  X(void, glGenBuffers, (GLsizei, GLuint*)) X(void, glBindBuffer, (GLenum, GLuint)) X(void, glBufferData, (GLenum, GLsizeiptr, const void*, GLenum)) X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*)) \
  X(void, glGenVertexArrays, (GLsizei, GLuint*)) X(void, glBindVertexArray, (GLuint)) X(void, glEnableVertexAttribArray, (GLuint)) \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) X(GLuint, glCreateShader, (GLenum)) \
  X(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) X(void, glCompileShader, (GLuint)) \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint*)) X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
  X(GLuint, glCreateProgram, (void)) X(void, glAttachShader, (GLuint, GLuint)) X(void, glLinkProgram, (GLuint)) \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint*)) X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
  X(void, glUseProgram, (GLuint)) X(GLint, glGetUniformLocation, (GLuint, const GLchar*)) X(void, glUniform1i, (GLint, GLint)) \
  X(void, glUniform1uiv, (GLint, GLsizei, const GLuint*)) X(void, glUniform4fv, (GLint, GLsizei, const GLfloat*)) \
  X(void, glUniform1iv, (GLint, GLsizei, const GLint*)) X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*)) \
  X(void, glGenFramebuffers, (GLsizei, GLuint*)) X(void, glBindFramebuffer, (GLenum, GLuint)) \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) X(GLenum, glCheckFramebufferStatus, (GLenum)) \
  X(void, glGenRenderbuffers, (GLsizei, GLuint*)) X(void, glBindRenderbuffer, (GLenum, GLuint)) \
  X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
  X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
  X(const GLubyte*, glGetString, (GLenum)) X(void, glFinish, (void)) X(GLenum, glGetError, (void)) \
  X(void, glLogicOp, (GLenum)) X(void, glUniform3fv, (GLint, GLsizei, const GLfloat*)) X(void, glUniform1fv, (GLint, GLsizei, const GLfloat*)) X(void, glUniform1f, (GLint, GLfloat)) X(void, glDeleteFramebuffers, (GLsizei, const GLuint*)) X(void, glUniform2f, (GLint, GLfloat, GLfloat)) X(void, glUniform2fv, (GLint, GLsizei, const GLfloat*)) X(void, glUniform1ui, (GLint, GLuint)) X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*))

#define GLDECL(ret, name, args) typedef ret (*PFN_##name) args; extern PFN_##name name;
GLFUNCS(GLDECL)
#undef GLDECL
int gl_load(void* (*getproc)(const char*));
