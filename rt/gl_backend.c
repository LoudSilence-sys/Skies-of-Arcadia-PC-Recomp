/* gl_backend.c - OpenGL 3.3 renderer for the GX pipeline.
   Vertices are transformed/lit/clipped on the CPU (gx.c); the GPU rasterizes and runs the TEV
   as a single "uber" fragment shader driven by the raw BP register words. */
#include "rt.h"
#include "glmini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rt_log(const char* fmt, ...);
void rt_fatal(const char* fmt, ...);
u32* gx_bp_regs(void);
void gx_tev_colors(float reg[4][4], float konst[4][4]);
int gx_tex_info(int map, u32* key, u32* w, u32* h, u32* mode0, u32* nbytes, u32* addr);
void gx_tex_decode(int map, u32* out);

#define GLDEF(ret, name, args) PFN_##name name;
GLFUNCS(GLDEF)
#undef GLDEF
int gl_load(void* (*getproc)(const char*)) {
#define GLLOAD(ret, name, args) name = (PFN_##name)getproc(#name); if (!name) { rt_log("GL: missing %s\n", #name); return 0; }
    GLFUNCS(GLLOAD)
#undef GLLOAD
    return 1;
}

/* ---------------- shaders ---------------- */
static const char* VS =
"#version 330 core\n"
"layout(location=0) in vec4 a_pos; layout(location=1) in vec4 a_c0; layout(location=2) in vec4 a_c1;\n"
"layout(location=3) in vec3 a_t0; layout(location=4) in vec3 a_t1; layout(location=5) in vec3 a_t2; layout(location=6) in vec3 a_t3;\n"
"layout(location=7) in vec3 a_t4; layout(location=8) in vec3 a_t5; layout(location=9) in vec3 a_t6; layout(location=10) in vec3 a_t7;\n"
"out vec4 v_c0; out vec4 v_c1; out vec3 v_t[8];\n"
"void main(){ gl_Position=a_pos; v_c0=a_c0; v_c1=a_c1; v_t[0]=a_t0; v_t[1]=a_t1; v_t[2]=a_t2; v_t[3]=a_t3; v_t[4]=a_t4; v_t[5]=a_t5; v_t[6]=a_t6; v_t[7]=a_t7; }\n";

static const char* FS =
"#version 330 core\n"
"in vec4 v_c0; in vec4 v_c1; in vec3 v_t[8];\n"
"out vec4 o_col;\n"
"uniform sampler2D u_tex0, u_tex1, u_tex2, u_tex3, u_tex4, u_tex5, u_tex6, u_tex7;\n"
"uniform uint u_cenv[16]; uniform uint u_aenv[16]; uniform uint u_order[8]; uniform uint u_ksel[8];\n"
"uniform uint u_misc[4]; /* 0 genmode, 1 alpha compare, 2 dst alpha, 3 unused */\n"
"uniform vec4 u_reg[4]; uniform vec4 u_konst[4];\n"
"vec4 samp(uint m, vec2 uv){ switch(m){ case 0u: return texture(u_tex0,uv); case 1u: return texture(u_tex1,uv); case 2u: return texture(u_tex2,uv); case 3u: return texture(u_tex3,uv);\n"
"  case 4u: return texture(u_tex4,uv); case 5u: return texture(u_tex5,uv); case 6u: return texture(u_tex6,uv); default: return texture(u_tex7,uv);} }\n"
"vec4 swz(vec4 c, uint t){ uint a=u_ksel[t*2u], b=u_ksel[t*2u+1u]; return vec4(c[a&3u], c[(a>>2)&3u], c[b&3u], c[(b>>2)&3u]); }\n"
"float kfrac(uint s){ return (s<8u) ? (8.0-float(s))/8.0 : 1.0; }\n"
"vec3 kcol(uint s){ if(s<8u) return vec3(kfrac(s)); if(s>=12u && s<16u) return u_konst[s-12u].rgb; if(s>=16u) return vec3(u_konst[(s-16u)&3u][(s-16u)>>2]); return vec3(1.0); }\n"
"float kalp(uint s){ if(s<8u) return kfrac(s); if(s>=16u) return u_konst[(s-16u)&3u][(s-16u)>>2]; return 1.0; }\n"
"float pack(vec3 v, uint mode){ vec3 q=floor(clamp(v,0.0,1.0)*255.0+0.5); if(mode==0u) return q.r; if(mode==1u) return q.g*256.0+q.r; return q.b*65536.0+q.g*256.0+q.r; }\n"
"bool acmp(uint c, float a, float r){ if(c==0u) return false; if(c==1u) return a<r; if(c==2u) return a==r; if(c==3u) return a<=r; if(c==4u) return a>r; if(c==5u) return a!=r; if(c==6u) return a>=r; return true; }\n"
"void main(){\n"
"  vec4 r[4]; r[0]=u_reg[0]; r[1]=u_reg[1]; r[2]=u_reg[2]; r[3]=u_reg[3];\n"
"  uint ns=((u_misc[0]>>10)&15u)+1u;\n"
"  const vec3 bias3=vec3(0.0); float bias[3]=float[3](0.0,0.5,-0.5); float scl[4]=float[4](1.0,2.0,4.0,0.5);\n"
"  for(uint s=0u;s<ns;s++){\n"
"    uint o=u_order[s>>1]; if((s&1u)!=0u) o>>=12;\n"
"    uint tmap=o&7u, tcrd=(o>>3)&7u, ten=(o>>6)&1u, chan=(o>>7)&7u;\n"
"    uint ce=u_cenv[s], ae=u_aenv[s];\n"
"    vec4 texc=vec4(1.0), rasc=vec4(0.0);\n"
"    if(ten!=0u){ vec3 t=v_t[tcrd]; float q=(t.z==0.0)?1.0:t.z; texc=swz(samp(tmap,t.xy/q),(ae>>2)&3u); }\n"
"    if(chan==0u) rasc=swz(clamp(v_c0,0.0,1.0),ae&3u); else if(chan==1u) rasc=swz(clamp(v_c1,0.0,1.0),ae&3u);\n"
"    uint ks=u_ksel[s>>1]; uint kcs=((s&1u)!=0u)?((ks>>14)&31u):((ks>>4)&31u); uint kas=((s&1u)!=0u)?((ks>>19)&31u):((ks>>9)&31u);\n"
"    vec3 kc=kcol(kcs); float ka=kalp(kas);\n"
"    vec3 ci[4]; uint cs[4]=uint[4]((ce>>12)&15u,(ce>>8)&15u,(ce>>4)&15u,ce&15u);\n"
"    for(int i=0;i<4;i++){ uint x=cs[i]; vec3 d;\n"
"      if(x<8u){ d=((x&1u)==0u)? r[x>>1].rgb : vec3(r[x>>1].a); }\n"
"      else if(x==8u) d=texc.rgb; else if(x==9u) d=vec3(texc.a); else if(x==10u) d=rasc.rgb; else if(x==11u) d=vec3(rasc.a);\n"
"      else if(x==12u) d=vec3(1.0); else if(x==13u) d=vec3(0.5); else if(x==14u) d=kc; else d=vec3(0.0);\n"
"      ci[i]=d; }\n"
"    float ai[4]; uint as_[4]=uint[4]((ae>>13)&7u,(ae>>10)&7u,(ae>>7)&7u,(ae>>4)&7u);\n"
"    for(int i=0;i<4;i++){ uint x=as_[i]; float d;\n"
"      if(x<4u) d=r[x].a; else if(x==4u) d=texc.a; else if(x==5u) d=rasc.a; else if(x==6u) d=ka; else d=0.0; ai[i]=d; }\n"
"    uint cb=(ce>>16)&3u, cop=(ce>>18)&1u, ccl=(ce>>19)&1u, csc=(ce>>20)&3u, cd=(ce>>22)&3u;\n"
"    vec3 res;\n"
"    if(cb!=3u){ vec3 a=clamp(ci[0],0.0,1.0), b=clamp(ci[1],0.0,1.0), c=clamp(ci[2],0.0,1.0); vec3 v=mix(a,b,c); if(cop!=0u) v=-v;\n"
"      v=(ci[3]+v+bias[cb])*scl[csc]; res=(ccl!=0u)?clamp(v,0.0,1.0):clamp(v,-4.0,4.0); }\n"
"    else { uint m=(csc<<1)|cop;\n"
"      if(m<6u){ float va=pack(ci[0],m>>1), vb=pack(ci[1],m>>1); bool p=((m&1u)!=0u)?(va==vb):(va>vb); res=ci[3]+(p?ci[2]:vec3(0.0)); }\n"
"      else { vec3 va=floor(clamp(ci[0],0.0,1.0)*255.0+0.5), vb=floor(clamp(ci[1],0.0,1.0)*255.0+0.5); bvec3 p=((m&1u)!=0u)?equal(va,vb):greaterThan(va,vb);\n"
"        res=ci[3]+vec3(p.x?ci[2].x:0.0, p.y?ci[2].y:0.0, p.z?ci[2].z:0.0); }\n"
"      if(ccl!=0u) res=clamp(res,0.0,1.0); }\n"
"    uint ab=(ae>>16)&3u, aop=(ae>>18)&1u, acl=(ae>>19)&1u, asc=(ae>>20)&3u, ad=(ae>>22)&3u;\n"
"    float ra;\n"
"    if(ab!=3u){ float a=clamp(ai[0],0.0,1.0), b=clamp(ai[1],0.0,1.0), c=clamp(ai[2],0.0,1.0); float v=mix(a,b,c); if(aop!=0u) v=-v;\n"
"      v=(ai[3]+v+bias[ab])*scl[asc]; ra=(acl!=0u)?clamp(v,0.0,1.0):clamp(v,-4.0,4.0); }\n"
"    else { uint m=(asc<<1)|aop; float va=floor(clamp(ai[0],0.0,1.0)*255.0+0.5), vb=floor(clamp(ai[1],0.0,1.0)*255.0+0.5);\n"
"      bool p=((m&1u)!=0u)?(va==vb):(va>vb); ra=ai[3]+(p?ai[2]:0.0); if(acl!=0u) ra=clamp(ra,0.0,1.0); }\n"
"    r[cd].rgb=res; r[ad].a=ra;\n"
"  }\n"
"  vec4 c=clamp(r[0],0.0,1.0);\n"
"  uint ac=u_misc[1]; float a8=floor(c.a*255.0+0.5);\n"
"  bool p0=acmp((ac>>16)&7u,a8,float(ac&255u)), p1=acmp((ac>>19)&7u,a8,float((ac>>8)&255u)); uint lg=(ac>>22)&3u;\n"
"  bool pass=(lg==0u)?(p0&&p1):(lg==1u)?(p0||p1):(lg==2u)?(p0!=p1):(p0==p1);\n"
"  if(!pass) discard;\n"
"  o_col=c;\n"
"}\n";

static GLuint g_prog, g_vao, g_vbo, g_fbo, g_fbtex, g_fbdepth;
static GLint u_cenv, u_aenv, u_order, u_ksel, u_misc, u_reg, u_konst, u_tex[8];
int g_gl_ok;

#define EFB_W 640
#define EFB_H 528

static GLuint compile(GLenum t, const char* src) {
    GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, 0); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; glGetShaderInfoLog(s, sizeof log, 0, log); rt_fatal("shader compile failed:\n%s\n", log); }
    return s;
}

/* ---------------- platform: headless EGL ---------------- */
#ifndef _WIN32
#include <EGL/egl.h>
static void* egl_getproc(const char* n) { return (void*)eglGetProcAddress(n); }
static int platform_init(void) {
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, 0, 0)) { rt_log("EGL: init failed\n"); return 0; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint cfga[] = { EGL_NONE }; EGLConfig cfg = 0; EGLint n = 0;
    if (!eglChooseConfig(d, cfga, &cfg, 1, &n) || n < 1) { rt_log("EGL: no config, trying configless\n"); cfg = 0; }
    EGLint ca[] = { 0x3098 /*MAJOR*/, 3, 0x30FB /*MINOR*/, 3, 0x30FD /*PROFILE_MASK*/, 1, EGL_NONE };
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ca);
    if (!ctx) { rt_log("EGL: context failed\n"); return 0; }
    if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { rt_log("EGL: make current failed\n"); return 0; }
    return gl_load(egl_getproc);
}
#else
int platform_init(void);
#endif

int gl_init(void) {
    if (!platform_init()) return 0;
    rt_log("GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    g_prog = glCreateProgram(); glAttachShader(g_prog, vs); glAttachShader(g_prog, fs); glLinkProgram(g_prog);
    GLint ok; glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[4096]; glGetProgramInfoLog(g_prog, sizeof log, 0, log); rt_fatal("link failed:\n%s\n", log); }
    glUseProgram(g_prog);
    u_cenv = glGetUniformLocation(g_prog, "u_cenv"); u_aenv = glGetUniformLocation(g_prog, "u_aenv");
    u_order = glGetUniformLocation(g_prog, "u_order"); u_ksel = glGetUniformLocation(g_prog, "u_ksel");
    u_misc = glGetUniformLocation(g_prog, "u_misc"); u_reg = glGetUniformLocation(g_prog, "u_reg"); u_konst = glGetUniformLocation(g_prog, "u_konst");
    for (int i = 0; i < 8; i++) { char n[16]; snprintf(n, sizeof n, "u_tex%d", i); u_tex[i] = glGetUniformLocation(g_prog, n); glUniform1i(u_tex[i], i); }
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    int stride = 36 * 4;
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, 0, stride, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, 0, stride, (void*)(4 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, stride, (void*)(8 * 4));
    for (int t = 0; t < 8; t++) { glEnableVertexAttribArray(3 + t); glVertexAttribPointer(3 + t, 3, GL_FLOAT, 0, stride, (void*)((size_t)(12 + 3 * t) * 4)); }
    /* EFB */
    glGenFramebuffers(1, &g_fbo); glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glGenTextures(1, &g_fbtex); glBindTexture(GL_TEXTURE_2D, g_fbtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W, EFB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbtex, 0);
    glGenRenderbuffers(1, &g_fbdepth); glBindRenderbuffer(GL_RENDERBUFFER, g_fbdepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EFB_W, EFB_H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbdepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rt_fatal("EFB framebuffer incomplete\n");
    glViewport(0, 0, EFB_W, EFB_H);
    glClearColor(0, 0, 0, 0); glClearDepth(1.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_gl_ok = 1;
    return 1;
}

/* ---------------- texture cache ---------------- */
typedef struct { u32 key[4]; u32 hash; GLuint tex; u32 w, h; u64 frame; } TexEnt;
#define TCACHE 1024
static TexEnt g_tc[TCACHE];
u64 g_gl_frame;
static u32 fnv(const u8* p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i += 4) { h ^= p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (u32)p[i + 3] << 24; h *= 16777619u; } return h; }

static void bind_tex(int map) {
    u32 key[4], w, h, mode0, nbytes, addr;
    gx_tex_info(map, key, &w, &h, &mode0, &nbytes, &addr);
    u32 a = addr & 0x01FFFFFF; if (a + nbytes > RAM_SIZE) nbytes = RAM_SIZE - a;
    u32 hash = fnv(g_ram + a, nbytes & ~3u) ^ key[3];
    u32 slot = (key[0] * 2654435761u ^ key[1] * 40503u ^ key[2]) % TCACHE;
    TexEnt* e = 0;
    for (int probe = 0; probe < 8; probe++) {
        TexEnt* t = &g_tc[(slot + probe) % TCACHE];
        if (t->tex && !memcmp(t->key, key, 12) && t->hash == hash) { e = t; break; }
    }
    if (!e) {
        TexEnt* t = &g_tc[slot]; u64 oldest = ~0ull;
        for (int probe = 0; probe < 8; probe++) { TexEnt* q = &g_tc[(slot + probe) % TCACHE]; if (!q->tex) { t = q; break; } if (q->frame < oldest) { oldest = q->frame; t = q; } }
        static u32* buf; static u32 bufn;
        if (w * h > bufn) { bufn = w * h; buf = realloc(buf, bufn * 4); }
        gx_tex_decode(map, buf);
        if (!t->tex) glGenTextures(1, &t->tex);
        glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        memcpy(t->key, key, 12); t->hash = hash; t->w = w; t->h = h; e = t;
    }
    e->frame = g_gl_frame;
    glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, e->tex);
    static const GLint wraps[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[mode0 & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[(mode0 >> 2) & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    u32 mf = (mode0 >> 5) & 7;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (mf == 0 || mf == 1 || mf == 2) ? GL_NEAREST : GL_LINEAR);
    if (mf == 4 || mf == 5 || mf == 6) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

/* ---------------- batching ---------------- */
static float* g_vb; static int g_nv, g_cap;
void gl_vertex(const float* pos4, const float* c0, const float* c1, const float tex[8][3]) {
    if (g_nv + 1 > g_cap) { g_cap = g_cap ? g_cap * 2 : 65536; g_vb = realloc(g_vb, (size_t)g_cap * 36 * 4); }
    float* v = g_vb + (size_t)g_nv * 36;
    memcpy(v, pos4, 16); memcpy(v + 4, c0, 16); memcpy(v + 8, c1, 16);
    for (int t = 0; t < 8; t++) { v[12 + 3 * t] = tex[t][0]; v[13 + 3 * t] = tex[t][1]; v[14 + 3 * t] = tex[t][2]; }
    g_nv++;
}

static GLenum gl_src(u32 f, int noalpha) {
    switch (f) { case 0: return GL_ZERO; case 1: return GL_ONE; case 2: return GL_DST_COLOR; case 3: return GL_ONE_MINUS_DST_COLOR;
    case 4: return GL_SRC_ALPHA; case 5: return GL_ONE_MINUS_SRC_ALPHA; case 6: return noalpha ? GL_ONE : GL_DST_ALPHA; default: return noalpha ? GL_ZERO : GL_ONE_MINUS_DST_ALPHA; }
}
static GLenum gl_dst(u32 f, int noalpha) {
    switch (f) { case 0: return GL_ZERO; case 1: return GL_ONE; case 2: return GL_SRC_COLOR; case 3: return GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA; case 5: return GL_ONE_MINUS_SRC_ALPHA; case 6: return noalpha ? GL_ONE : GL_DST_ALPHA; default: return noalpha ? GL_ZERO : GL_ONE_MINUS_DST_ALPHA; }
}

void gl_flush(void) {
    if (!g_nv) return;
    u32* bp = gx_bp_regs();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); glViewport(0, 0, EFB_W, EFB_H);
    glUseProgram(g_prog); glBindVertexArray(g_vao);
    u32 cenv[16], aenv[16];
    for (int i = 0; i < 16; i++) { cenv[i] = bp[0xC0 + 2 * i]; aenv[i] = bp[0xC1 + 2 * i]; }
    glUniform1uiv(u_cenv, 16, cenv); glUniform1uiv(u_aenv, 16, aenv);
    glUniform1uiv(u_order, 8, bp + 0x28); glUniform1uiv(u_ksel, 8, bp + 0xF6);
    int noalpha = (bp[0x43] & 7) != 1;
    u32 misc[4] = { bp[0x00], bp[0xF3], (u32)noalpha, 0 }; glUniform1uiv(u_misc, 4, misc);
    float reg[4][4], konst[4][4]; gx_tev_colors(reg, konst);
    glUniform4fv(u_reg, 4, &reg[0][0]); glUniform4fv(u_konst, 4, &konst[0][0]);
    /* textures used by enabled TEV stages */
    int ns = ((bp[0x00] >> 10) & 15) + 1; u32 used = 0;
    for (int s = 0; s < ns; s++) { u32 o = bp[0x28 + s / 2]; if (s & 1) o >>= 12; if ((o >> 6) & 1) used |= 1u << (o & 7); }
    for (int m = 0; m < 8; m++) if (used & (1u << m)) bind_tex(m);
    /* depth */
    u32 zm = bp[0x40];
    if (zm & 1) { static const GLenum f[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS}; glEnable(GL_DEPTH_TEST); glDepthFunc(f[(zm >> 1) & 7]); glDepthMask((zm >> 4) & 1); }
    else { glDisable(GL_DEPTH_TEST); glDepthMask(0); }
    /* blend */
    u32 bm = bp[0x41];
    glColorMask((bm >> 3) & 1, (bm >> 3) & 1, (bm >> 3) & 1, (bm >> 4) & 1);
    if (!(bm & 1) && ((bm >> 1) & 1) && ((bm >> 12) & 15) != 3) { glDisable(GL_BLEND); glEnable(GL_COLOR_LOGIC_OP); glLogicOp(0x1500 + ((bm >> 12) & 15)); }
    else glDisable(GL_COLOR_LOGIC_OP);
    if (bm & 1) {
        glEnable(GL_BLEND);
        if ((bm >> 11) & 1) { glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); glBlendFunc(GL_ONE, GL_ONE); }
        else { glBlendEquation(GL_FUNC_ADD); glBlendFunc(gl_src((bm >> 8) & 7, noalpha), gl_dst((bm >> 5) & 7, noalpha)); }
    } else glDisable(GL_BLEND);
    /* scissor */
    u32 sc0 = bp[0x20], sc1 = bp[0x21];
    int sy0 = (sc0 & 0x7FF) - 342, sx0 = ((sc0 >> 12) & 0x7FF) - 342, sy1 = (sc1 & 0x7FF) - 342, sx1 = ((sc1 >> 12) & 0x7FF) - 342;
    if (sx1 >= sx0 && sy1 >= sy0) { glEnable(GL_SCISSOR_TEST); glScissor(sx0, EFB_H - 1 - sy1, sx1 - sx0 + 1, sy1 - sy0 + 1); }
    else glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_nv * 36 * 4, g_vb, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_nv);
    g_nv = 0;
}

/* read EFB region (y down) into RGBA8 buffer laid out EFB_W wide */
void gl_read_efb(u32* dst, int x, int y, int w, int h) {
    gl_flush(); glDisable(GL_COLOR_LOGIC_OP);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    static u32* tmp; static int tn; if (w * h > tn) { tn = w * h; tmp = realloc(tmp, (size_t)tn * 4); }
    glReadPixels(x, EFB_H - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int r = 0; r < h; r++) memcpy(dst + (size_t)(y + r) * EFB_W + x, tmp + (size_t)(h - 1 - r) * w, (size_t)w * 4);
}
void gl_clear(int x, int y, int w, int h, u32 rgba, u32 z24, int color, int alpha, int depth) {
    gl_flush(); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glEnable(GL_SCISSOR_TEST); glScissor(x, EFB_H - y - h, w, h);
    glColorMask(color, color, color, alpha); glDepthMask(depth);
    glClearColor((rgba & 255) / 255.f, ((rgba >> 8) & 255) / 255.f, ((rgba >> 16) & 255) / 255.f, (rgba >> 24) / 255.f);
    glClearDepth(z24 / 16777215.0);
    glClear((color || alpha ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    glColorMask(1, 1, 1, 1);
}

void gl_blit_to_screen(int w, int h) {
    gl_flush();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h); glClearColor(0, 0, 0, 1); glColorMask(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    int dw = w, dh = w * 3 / 4; if (dh > h) { dh = h; dw = h * 4 / 3; }
    int dx = (w - dw) / 2, dy = (h - dh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glBlitFramebuffer(0, EFB_H - 480, 640, EFB_H, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}
