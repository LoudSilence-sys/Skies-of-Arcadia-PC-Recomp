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
int gx_tex_levels(int map);
void gx_tex_decode_level(int map, int lvl, u32* out, u32* ow, u32* oh);

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
"layout(location=0, index=0) out vec4 o_col; layout(location=0, index=1) out vec4 o_src1;\n"
"uniform sampler2D u_tex0, u_tex1, u_tex2, u_tex3, u_tex4, u_tex5, u_tex6, u_tex7;\n"
"uniform uint u_cenv[16]; uniform uint u_aenv[16]; uniform uint u_order[8]; uniform uint u_ksel[8];\n"
"uniform uint u_misc[6]; /* 0 genmode, 1 alpha compare, 2 no-alpha EFB, 3 dst alpha (BP 0x42), 4 ztex1 bias, 5 ztex2 */\n"
"uniform vec4 u_reg[4]; uniform vec4 u_konst[4];\n"
"uniform uint u_ind[40]; uniform int u_imtx[24]; /* ind: 0-15 IND_CMD, 16 IREF, 17-18 ind scale, 20-35 texcoord sizes s,t; imtx: 3 x (ma mc me w, mb md mf w) */\n"
"uniform float u_fogk[10]; uniform vec2 u_fogrc; /* fog range adjust: K table, (center, viewport width) */\n"
"uniform float u_scale;\n"
"uniform vec4 u_fog; uniform vec3 u_fogcol; uniform uint u_fogmode; /* fog: A, C, B magnitude, B shift; mode = fsel | proj<<3 */\n"
"vec4 samp(uint m, vec2 uv){ switch(m){ case 0u: return texture(u_tex0,uv); case 1u: return texture(u_tex1,uv); case 2u: return texture(u_tex2,uv); case 3u: return texture(u_tex3,uv);\n"
"  case 4u: return texture(u_tex4,uv); case 5u: return texture(u_tex5,uv); case 6u: return texture(u_tex6,uv); default: return texture(u_tex7,uv);} }\n"
"ivec2 tsize(uint m){ switch(m){ case 0u: return textureSize(u_tex0,0); case 1u: return textureSize(u_tex1,0); case 2u: return textureSize(u_tex2,0); case 3u: return textureSize(u_tex3,0);\n"
"  case 4u: return textureSize(u_tex4,0); case 5u: return textureSize(u_tex5,0); case 6u: return textureSize(u_tex6,0); default: return textureSize(u_tex7,0);} }\n"
"vec4 sampi(uint m, ivec2 uv){ return samp(m, vec2(uv)/(vec2(tsize(m))*128.0)); }\n"
"ivec2 fuv(uint tc){ vec3 t=v_t[tc]; vec2 uv=(t.z==0.0)?t.xy:t.xy/t.z; return ivec2(uv*vec2(float(u_ind[20u+2u*tc]),float(u_ind[21u+2u*tc]))*128.0); }\n"
"ivec2 mshift(ivec2 v, int w){ return (w>=0)?(v>>w):(v<<(-w)); }\n"
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
"  uint ntg=u_misc[0]&15u, nind=(u_misc[0]>>16)&7u; ivec3 iind[4]; ivec2 tevc=ivec2(0); int abump=0;\n"
"  for(uint i=0u;i<4u;i++){ iind[i]=ivec3(0); if(i>=nind) continue; uint im=(u_ind[16]>>(6u*i))&7u, itc=(u_ind[16]>>(6u*i+3u))&7u; if(itc>=ntg) itc=0u;\n"
"    uint sc=u_ind[17u+(i>>1)]>>((i&1u)*8u); ivec2 c=fuv(itc)>>ivec2(int(sc&15u),int((sc>>4)&15u)); ivec4 t=ivec4(floor(sampi(im,c)*255.0+0.5)); iind[i]=ivec3(t.a,t.b,t.g); }\n"
"  vec4 lastraw=vec4(0.0);\n"
"  for(uint s=0u;s<ns;s++){\n"
"    uint o=u_order[s>>1]; if((s&1u)!=0u) o>>=12;\n"
"    uint tmap=o&7u, tcrd=(o>>3)&7u, ten=(o>>6)&1u, chan=(o>>7)&7u;\n"
"    uint ce=u_cenv[s], ae=u_aenv[s];\n"
"    vec4 texc=vec4(1.0), rasc=vec4(0.0); uint tcu=(tcrd<ntg)?tcrd:0u; uint ic=u_ind[s];\n"
"    if(ic!=0u){ uint bt=ic&3u, fmt=(ic>>2)&3u, bia=(ic>>4)&7u, bs=(ic>>7)&3u, mi=(ic>>9)&3u, mid=(ic>>11)&3u, sw=(ic>>13)&7u, tw=(ic>>16)&7u;\n"
"      ivec2 fp=fuv(tcu); bool has=bt<nind; ivec3 iv=iind[bt&3u];\n"
"      if(has && bs!=0u){ int comp=(bs==1u)?iv.x:(bs==2u)?iv.y:iv.z; int sh=(fmt==0u)?0:int(6u-fmt); abump=(comp<<sh)&248; }\n"
"      ivec2 tr=ivec2(0);\n"
"      if(has && mi!=0u){ ivec3 crd=iv>>((fmt==0u)?0:int(fmt+2u)); int ba=(fmt==0u)?-128:1; if((bia&1u)!=0u) crd.x+=ba; if((bia&2u)!=0u) crd.y+=ba; if((bia&4u)!=0u) crd.z+=ba;\n"
"        int b0=int(mi-1u)*8; int w=u_imtx[b0+3];\n"
"        if(mid==0u) tr=ivec2(u_imtx[b0]*crd.x+u_imtx[b0+1]*crd.y+u_imtx[b0+2]*crd.z, u_imtx[b0+4]*crd.x+u_imtx[b0+5]*crd.y+u_imtx[b0+6]*crd.z)>>3;\n"
"        else if(mid==1u) tr=(fp*crd.xx)>>8; else if(mid==2u) tr=(fp*crd.yy)>>8;\n"
"        tr=mshift(tr,w); }\n"
"      ivec2 wc; wc.x=(sw==0u)?fp.x:(sw>=6u)?0:(fp.x&(((256>>int(sw-1u))<<7)-1)); wc.y=(tw==0u)?fp.y:(tw>=6u)?0:(fp.y&(((256>>int(tw-1u))<<7)-1));\n"
"      tevc=(((ic>>20)&1u)!=0u)?(tevc+wc+tr):(wc+tr); tevc=(tevc<<8)>>8;\n"
"      if(ten!=0u && ntg>0u) texc=swz(sampi(tmap,tevc),(ae>>2)&3u); }\n"
"    else { if(ten!=0u && ntg>0u){ vec3 t=v_t[tcu]; float q=(t.z==0.0)?1.0:t.z; texc=swz(samp(tmap,t.xy/q),(ae>>2)&3u); tevc=fuv(tcu); } }\n"
"    if(ntg==0u) texc=vec4(0.0);\n"
"    if(ten!=0u) lastraw=texc;\n"
"    if(chan==0u) rasc=swz(clamp(v_c0,0.0,1.0),ae&3u); else if(chan==1u) rasc=swz(clamp(v_c1,0.0,1.0),ae&3u);\n"
"    else if(chan==5u) rasc=vec4(float(abump)/255.0); else if(chan==6u) rasc=vec4(float(abump|(abump>>5))/255.0);\n"
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
"  float zout=gl_FragCoord.z; uint zop=(u_misc[5]>>2)&3u;\n"
"  if(zop!=0u){ uvec4 t=uvec4(floor(clamp(lastraw,0.0,1.0)*255.0+0.5)); uint zt=u_misc[5]&3u; uint zc;\n"
"    if(zt==0u) zc=t.a; else if(zt==1u) zc=t.r+t.a*256u; else zc=t.r*65536u+t.g*256u+t.b; zc+=u_misc[4];\n"
"    if(zop==1u) zc+=uint(clamp(gl_FragCoord.z,0.0,1.0)*16777215.0+0.5); zout=float(zc&0xFFFFFFu)/16777215.0; }\n"
"  gl_FragDepth=zout;\n"
"  uint fsel=u_fogmode&7u;\n"
"  if(fsel>=2u){ uint zc=uint(clamp(gl_FragCoord.z,0.0,1.0)*16777215.0+0.5); float ze;\n"
"    if((u_fogmode&8u)==0u){ float den=u_fog.z-float(zc>>uint(u_fog.w)); ze=(den!=0.0)?(u_fog.x*16777216.0)/den:0.0; } else ze=u_fog.x*float(zc)/16777216.0;\n"
"    if((u_fogmode&16u)!=0u){ float off=(2.0*(gl_FragCoord.x/u_scale/u_fogrc.y))-1.0-u_fogrc.x; float fi=clamp(9.0-abs(off)*9.0,0.0,9.0); uint il=uint(fi); uint iu=min(il+1u,9u);\n"
"      float k=mix(u_fogk[il],u_fogk[iu],fract(fi)); if(k>0.0) ze*=sqrt(off*off+k*k)/k; }\n"
"    float fog=clamp(ze-u_fog.y,0.0,1.0);\n"
"    if(fsel==4u) fog=1.0-exp2(-8.0*fog); else if(fsel==5u) fog=1.0-exp2(-8.0*fog*fog); else if(fsel==6u) fog=exp2(-8.0*(1.0-fog)); else if(fsel==7u){ fog=1.0-fog; fog=exp2(-8.0*fog*fog); }\n"
"    float ifog=floor(fog*256.0+0.5); vec3 pc=floor(c.rgb*255.0+0.5); c.rgb=floor((pc*(256.0-ifog)+u_fogcol*255.0*ifog)/256.0)/255.0; }\n"
"  o_src1=c; if((u_misc[3]&0x100u)!=0u) c.a=float(u_misc[3]&255u)/255.0; if(u_misc[2]!=0u) c.a=1.0;\n"
"  o_col=c;\n"
"}\n";

static GLuint g_prog, g_vao, g_vbo, g_fbo, g_fbtex, g_fbdepth;
static GLint u_scale, u_cenv, u_aenv, u_order, u_ksel, u_misc, u_reg, u_konst, u_tex[8], u_fog, u_fogcol, u_fogmode, u_ind, u_imtx, u_fogk, u_fogrc;
int g_gl_ok;

#define EFB_W 640
#define EFB_H 528
int g_scale = 1;                /* internal resolution multiplier (settings.ini resolution_scale) */
#define SW (EFB_W * g_scale)
#define SH (EFB_H * g_scale)
static GLuint g_rfbo, g_rtex, g_rdepth;   /* native-resolution resolve target for EFB readbacks */

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
    u_fog = glGetUniformLocation(g_prog, "u_fog"); u_fogcol = glGetUniformLocation(g_prog, "u_fogcol"); u_fogmode = glGetUniformLocation(g_prog, "u_fogmode");
    u_scale = glGetUniformLocation(g_prog, "u_scale"); glUniform1f(u_scale, (float)g_scale);
    u_fogk = glGetUniformLocation(g_prog, "u_fogk"); u_fogrc = glGetUniformLocation(g_prog, "u_fogrc");
    u_ind = glGetUniformLocation(g_prog, "u_ind"); u_imtx = glGetUniformLocation(g_prog, "u_imtx");
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fbtex, 0);
    glGenRenderbuffers(1, &g_fbdepth); glBindRenderbuffer(GL_RENDERBUFFER, g_fbdepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SW, SH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_fbdepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rt_fatal("EFB framebuffer incomplete (scale %d)\n", g_scale);
    /* native-size resolve target: scaled EFB regions are downsampled into it before reading back to RAM */
    glGenFramebuffers(1, &g_rfbo); glBindFramebuffer(GL_FRAMEBUFFER, g_rfbo);
    glGenTextures(1, &g_rtex); glBindTexture(GL_TEXTURE_2D, g_rtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, EFB_W, EFB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rtex, 0);
    glGenRenderbuffers(1, &g_rdepth); glBindRenderbuffer(GL_RENDERBUFFER, g_rdepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EFB_W, EFB_H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_rdepth);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, SW, SH);
    glClearColor(0, 0, 0, 0); glClearDepth(1.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_gl_ok = 1;
    return 1;
}

/* ---------------- texture cache ---------------- */
typedef struct { u32 key[4]; u32 hash; GLuint tex; u32 w, h; u64 frame; int levels; } TexEnt;
#define TCACHE 1024
static TexEnt g_tc[TCACHE];
u64 g_gl_frame;
static u32 fnv(const u8* p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i += 4) { h ^= p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (u32)p[i + 3] << 24; h *= 16777619u; } return h; }

/* ---------------- EFB copies to texture, kept on the GPU ----------------
   Like Dolphin's "store EFB copies to texture only": a texture copy is rendered straight into a GL texture
   (format conversion in a shader) and registered under its destination address. When the game later
   samples a texture at that address with the matching size and format, the GL texture is bound instead of
   decoding RAM. This removes the per-copy readback stall. RAM is not written; a sampled hash of the
   destination RAM is recorded so the entry is dropped if something else is later loaded there. */
static const char* CVS = "#version 330 core\nvoid main(){ vec2 p=vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2)); gl_Position=vec4(p*2.0-1.0,0.0,1.0); }\n";
static const char* CFS =
"#version 330 core\n"
"uniform sampler2D u_src; uniform vec4 u_rect; uniform vec2 u_dsize; uniform uint u_fmt; uniform uint u_flags;\n"
"out vec4 o;\n"
"float q(float v, float bits){ float m=exp2(bits)-1.0; return floor(v*m+0.0001)/m; }\n"   /* keep the top bits, as the GX encoders */
"void main(){\n"
"  vec2 d=gl_FragCoord.xy/u_dsize; vec2 p=vec2(u_rect.x+d.x*u_rect.z, u_rect.y+d.y*u_rect.w);\n"
"  vec4 c=texture(u_src, vec2(p.x/640.0, 1.0-p.y/528.0)); c=floor(c*255.0+0.5)/255.0;\n"
"  if((u_flags&2u)!=0u) c.a=1.0;\n"
"  if((u_flags&1u)!=0u){ float y=clamp((0.257*c.r+0.504*c.g+0.098*c.b)*255.0+16.0,0.0,255.0); y=floor(y)/255.0; c.rgb=vec3(y); }\n"
"  uint f=u_fmt; vec4 r;\n"
"  if(f==0u) r=vec4(q(c.r,4.0));\n"
"  else if(f==1u||f==8u) r=vec4(c.r); else if(f==7u) r=vec4(c.a); else if(f==9u) r=vec4(c.g); else if(f==10u) r=vec4(c.b);\n"
"  else if(f==2u) r=vec4(vec3(q(c.r,4.0)),q(c.a,4.0));\n"
"  else if(f==3u) r=vec4(vec3(c.r),c.a); else if(f==11u) r=vec4(vec3(c.r),c.g); else if(f==12u) r=vec4(vec3(c.g),c.b);\n"
"  else if(f==4u) r=vec4(q(c.r,5.0),q(c.g,6.0),q(c.b,5.0),1.0);\n"
"  else if(f==5u){ if(c.a>=224.0/255.0) r=vec4(q(c.r,5.0),q(c.g,5.0),q(c.b,5.0),1.0); else r=vec4(q(c.r,4.0),q(c.g,4.0),q(c.b,4.0),q(c.a,3.0)); }\n"
"  else r=c;\n"
"  o=r; }\n";
void gl_flush(void);
static GLuint g_cprog, g_cvao; static GLint uc_src, uc_rect, uc_dsize, uc_fmt, uc_flags;
/* Each address keeps up to three versions of its copy: the current one, the one that was current when the
   frame started (frame interpolation replays a frame from its start, so it must see that one), and a shadow
   that the replay itself renders into. */
typedef struct { u32 tw, th, tfmt, nbytes, rhash; GLuint tex, fbo; int gw, gh; } CopyVer;
typedef struct { u32 addr; CopyVer v[3]; int cur, start, shadow; u64 frame; int used; } CopyEnt;
#define NCOPY 128
static CopyEnt g_copies[NCOPY];
int g_gpu_copies = 1;
int gx_replaying(void);
static u32 sampled_hash(u32 a, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i + 4 <= n; i += 256) { h ^= *(const u32*)(g_ram + ((a + i) & 0x01FFFFFC)); h *= 16777619u; } return h ^ n; }
static GLuint compile(GLenum t, const char* src);
/* copy fmt (GX EFB copy format) -> texture format the game will sample it as */
static int copy_tex_fmt(int f) { static const int m[13] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3}; return f >= 0 && f < 13 ? m[f] : -1; }
void gl_copies_frame_start(void) { for (int i = 0; i < NCOPY; i++) { g_copies[i].start = g_copies[i].cur; g_copies[i].shadow = -1; } }
int gl_efb_copy_tex(int sx, int sy, int w, int h, int half, int fmt, int inten, int noalpha, u32 dst, u32 nbytes) {
    if (!g_gpu_copies || copy_tex_fmt(fmt) < 0) return 0;
    gl_flush();
    if (!g_cprog) {
        GLuint vs = compile(GL_VERTEX_SHADER, CVS), fs = compile(GL_FRAGMENT_SHADER, CFS);
        g_cprog = glCreateProgram(); glAttachShader(g_cprog, vs); glAttachShader(g_cprog, fs); glLinkProgram(g_cprog);
        GLint ok; glGetProgramiv(g_cprog, GL_LINK_STATUS, &ok); if (!ok) { g_gpu_copies = 0; rt_log("GL: copy shader link failed\n"); return 0; }
        uc_src = glGetUniformLocation(g_cprog, "u_src"); uc_rect = glGetUniformLocation(g_cprog, "u_rect"); uc_dsize = glGetUniformLocation(g_cprog, "u_dsize");
        uc_fmt = glGetUniformLocation(g_cprog, "u_fmt"); uc_flags = glGetUniformLocation(g_cprog, "u_flags");
        glGenVertexArrays(1, &g_cvao);
    }
    int tw = half ? w / 2 : w, th = half ? h / 2 : h; if (tw < 1 || th < 1) return 1;
    int gw = tw * g_scale, gh = th * g_scale;
    u32 a = dst & 0x01FFFFFF;
    int replay = gx_replaying();
    CopyEnt* e = 0; CopyEnt* old = &g_copies[0];
    for (int i = 0; i < NCOPY; i++) {
        CopyEnt* c = &g_copies[i];
        if (c->used && c->addr == a) { e = c; break; }
        if (old->used && (!c->used || c->frame < old->frame)) old = c;   /* prefer an unused slot, else the least recently used */
    }
    if (!e) { if (replay) return 1; e = old; e->used = 1; e->addr = a; e->cur = 0; e->start = -1; e->shadow = -1; }
    int slot;
    if (replay) {   /* the replay renders into its own version */
        if (e->shadow < 0) { for (slot = 0; slot < 3 && (slot == e->start || slot == e->cur); slot++) ; e->shadow = slot < 3 ? slot : e->cur; }
        slot = e->shadow;
    } else {
        if (e->cur == e->start) { for (slot = 0; slot < 3 && slot == e->start; slot++) ; e->cur = slot; }   /* keep the frame-start version intact */
        slot = e->cur;
    }
    CopyVer* v = &e->v[slot];
    if (!v->tex) { glGenTextures(1, &v->tex); glGenFramebuffers(1, &v->fbo); v->gw = v->gh = 0; }
    glBindTexture(GL_TEXTURE_2D, v->tex);
    if (v->gw != gw || v->gh != gh) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gw, gh, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, v->fbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->tex, 0);
        v->gw = gw; v->gh = gh;
    }
    v->tw = tw; v->th = th; v->tfmt = copy_tex_fmt(fmt); v->nbytes = nbytes; e->frame = g_gl_frame;
    v->rhash = sampled_hash(a, nbytes);
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo); glViewport(0, 0, gw, gh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_COLOR_LOGIC_OP); glColorMask(1, 1, 1, 1);
    glUseProgram(g_cprog); glBindVertexArray(g_cvao);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_fbtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(uc_src, 0); { float r[4] = {(float)sx, (float)sy, (float)w, (float)h}; glUniform4fv(uc_rect, 1, r); }
    { float ds[2] = {(float)gw, (float)gh}; glUniform2fv(uc_dsize, 1, ds); }
    glUniform1ui(uc_fmt, (GLuint)fmt); glUniform1ui(uc_flags, (inten ? 1u : 0u) | (noalpha ? 2u : 0u));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); glViewport(0, 0, SW, SH);
    glUseProgram(g_prog); glBindVertexArray(g_vao);
    return 1;
}
/* a registered GPU copy for this texture, or 0 */
static GLuint copy_lookup(u32 addr, u32 w, u32 h, u32 fmt) {
    u32 a = addr & 0x01FFFFFF;
    for (int i = 0; i < NCOPY; i++) {
        CopyEnt* c = &g_copies[i];
        if (!c->used || c->addr != a) continue;
        int slot = c->cur;
        if (gx_replaying()) slot = c->shadow >= 0 ? c->shadow : c->start >= 0 ? c->start : c->cur;
        CopyVer* v = &c->v[slot];
        if (!v->tex || v->tw != w || v->th != h || v->tfmt != fmt) return 0;
        if (sampled_hash(a, v->nbytes) != v->rhash) { if (!gx_replaying()) c->used = 0; return 0; }   /* overwritten by other data */
        c->frame = g_gl_frame; return v->tex;
    }
    return 0;
}

u64 host_tb(void);
u64 g_pf_idle, g_pf_snd, g_pf_hash, g_pf_flush, g_pf_read, g_pf_present, g_pf_draws, g_pf_upload;   /* perf counters (timebase ticks) */
static void bind_tex(int map) {
    u32 key[4], w, h, mode0, nbytes, addr;
    gx_tex_info(map, key, &w, &h, &mode0, &nbytes, &addr);
    int levels = gx_tex_levels(map);
    GLuint ctex = (nbytes && g_gpu_copies) ? copy_lookup(addr, w, h, key[2] & 15) : 0;
    if (ctex) { levels = 1; glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, ctex); goto params; }
    u32 a = addr & 0x01FFFFFF; if (a + nbytes > RAM_SIZE) nbytes = RAM_SIZE - a;
    u64 th = host_tb();
    u32 hash;
    {   /* RAM texture hashes are cached per "epoch": the epoch advances every frame and whenever something
           can rewrite texture memory mid-frame (EFB copies, TMEM/TLUT loads, DMA), so each texture is hashed
           at most once per frame instead of on every draw */
        extern u32 g_tex_epoch;
        typedef struct { u32 a, n, k0, k1, k2, epoch, h; } HC; static HC hc[4096];
        HC* c = &hc[((a >> 5) ^ (nbytes >> 3) * 31 ^ key[2] * 7) & 4095];
        if (c->epoch == g_tex_epoch && c->a == a && c->n == nbytes && c->k0 == key[0] && c->k1 == key[1] && c->k2 == key[2]) hash = c->h;
        else { hash = fnv(g_ram + a, nbytes & ~3u); c->a = a; c->n = nbytes; c->k0 = key[0]; c->k1 = key[1]; c->k2 = key[2]; c->epoch = g_tex_epoch; c->h = hash; }
        hash ^= key[3];
    }
    g_pf_hash += host_tb() - th;
    u32 slot = (key[0] * 2654435761u ^ key[1] * 40503u ^ key[2]) % TCACHE;
    TexEnt* e = 0;
    for (int probe = 0; probe < 8; probe++) {
        TexEnt* t = &g_tc[(slot + probe) % TCACHE];
        if (t->tex && !memcmp(t->key, key, 12) && t->hash == hash && t->levels >= levels) { e = t; break; }
    }
    if (!e) {
        TexEnt* t = &g_tc[slot]; u64 oldest = ~0ull;
        for (int probe = 0; probe < 8; probe++) { TexEnt* q = &g_tc[(slot + probe) % TCACHE]; if (!q->tex) { t = q; break; } if (q->frame < oldest) { oldest = q->frame; t = q; } }
        u64 tu = host_tb();
        static u32* buf; static u32 bufn;
        if (w * h > bufn) { bufn = w * h; buf = realloc(buf, bufn * 4); }
        gx_tex_decode(map, buf);
        if (!t->tex) glGenTextures(1, &t->tex);
        glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, t->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        for (int l = 1; l < levels; l++) {
            u32 lw, lh; gx_tex_decode_level(map, l, buf, &lw, &lh);
            glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        memcpy(t->key, key, 12); t->hash = hash; t->w = w; t->h = h; t->levels = levels; e = t;
        g_pf_upload += host_tb() - tu;
    }
    e->frame = g_gl_frame;
    glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, e->tex);
params:;
    static const GLint wraps[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[mode0 & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[(mode0 >> 2) & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    {   /* min filter: bit7 linear, bits5-6 mip mode (0 none, 1 point, 2 linear); LOD range/bias from mode0/mode1 */
        int lin = (mode0 >> 7) & 1, mip = (mode0 >> 5) & 3;
        GLint f = lin ? GL_LINEAR : GL_NEAREST;
        if (levels > 1 && mip) f = lin ? (mip == 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mip == 2 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        if (levels > 1) {
            int b = map < 4 ? map : map - 4 + 0x20; u32 m1 = gx_bp_regs()[0x84 + b];
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, (m1 & 0xFF) / 16.f);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, ((m1 >> 8) & 0xFF) / 16.f);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, (float)(s8)((mode0 >> 9) & 0xFF) / 32.f);
        }
    }
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
    case 4: return GL_SRC1_ALPHA; case 5: return GL_ONE_MINUS_SRC1_ALPHA; case 6: return noalpha ? GL_ONE : GL_DST_ALPHA; default: return noalpha ? GL_ZERO : GL_ONE_MINUS_DST_ALPHA; }
}
static GLenum gl_dst(u32 f, int noalpha) {
    switch (f) { case 0: return GL_ZERO; case 1: return GL_ONE; case 2: return GL_SRC_COLOR; case 3: return GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC1_ALPHA; case 5: return GL_ONE_MINUS_SRC1_ALPHA; case 6: return noalpha ? GL_ONE : GL_DST_ALPHA; default: return noalpha ? GL_ZERO : GL_ONE_MINUS_DST_ALPHA; }
}

static void gl_flush_(void);
void gl_flush(void) { if (!g_nv) return; u64 t = host_tb(); gl_flush_(); g_pf_flush += host_tb() - t; g_pf_draws++; }
static void gl_flush_(void) {
    u32* bp = gx_bp_regs();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); glViewport(0, 0, SW, SH);
    glUseProgram(g_prog); glBindVertexArray(g_vao);
    u32 cenv[16], aenv[16];
    for (int i = 0; i < 16; i++) { cenv[i] = bp[0xC0 + 2 * i]; aenv[i] = bp[0xC1 + 2 * i]; }
    glUniform1uiv(u_cenv, 16, cenv); glUniform1uiv(u_aenv, 16, aenv);
    glUniform1uiv(u_order, 8, bp + 0x28); glUniform1uiv(u_ksel, 8, bp + 0xF6);
    int noalpha = (bp[0x43] & 7) != 1;
    u32 misc[6] = { bp[0x00], bp[0xF3], (u32)noalpha, bp[0x42], bp[0xF4] & 0xFFFFFF, bp[0xF5] }; glUniform1uiv(u_misc, 6, misc);
    float reg[4][4], konst[4][4]; gx_tev_colors(reg, konst);
    glUniform4fv(u_reg, 4, &reg[0][0]); glUniform4fv(u_konst, 4, &konst[0][0]);
    {   /* fog (BP 0xEE-0xF2), as Dolphin */
        u32 a = bp[0xEE], c3 = bp[0xF1], col = bp[0xF2];
        u32 ai = ((a >> 19) & 1) << 31 | ((a >> 11) & 0xFF) << 23 | (a & 0x7FF) << 12, ci = ((c3 >> 19) & 1) << 31 | ((c3 >> 11) & 0xFF) << 23 | (c3 & 0x7FF) << 12;
        float af, cf; memcpy(&af, &ai, 4); memcpy(&cf, &ci, 4);
        u32 fsel = (c3 >> 21) & 7, proj = (c3 >> 20) & 1;
        if (((a >> 11) & 0xFF) == 0xFF || ((c3 >> 11) & 0xFF) == 0xFF) fsel = 0;   /* NaN/inf parameters: off */
        float fv[4] = {af, cf, (float)(bp[0xEF] & 0xFFFFFF), (float)(bp[0xF0] & 31)};
        float fc[3] = {((col >> 16) & 255) / 255.f, ((col >> 8) & 255) / 255.f, (col & 255) / 255.f};
        u32 rng = 0;
        if ((bp[0xE8] >> 10) & 1) {   /* range adjust, as Dolphin */
            extern float gx_viewport_wd(void);
            float wd = gx_viewport_wd(); if (wd < 0) wd = -wd; if (wd < 1) wd = 320;
            float cen = (float)((int)(bp[0xE8] & 0x3FF) - 342) / (2.0f * wd) * 2.0f - 1.0f;
            float k[10]; for (int i = 0; i < 5; i++) { u32 e = bp[0xE9 + i]; k[2 * i] = ((e >> 12) & 0xFFF) / 256.f * 4.f; k[2 * i + 1] = (e & 0xFFF) / 256.f * 4.f; }
            float rc[2] = {cen, 2.0f * wd};
            glUniform1fv(u_fogk, 10, k); glUniform2fv(u_fogrc, 1, rc); rng = 16;
        }
        glUniform4fv(u_fog, 1, fv); glUniform3fv(u_fogcol, 1, fc); glUniform1ui(u_fogmode, fsel | proj << 3 | rng);
    }
    {   /* indirect texturing state */
        u32 ind[40] = {0};
        for (int i = 0; i < 16; i++) ind[i] = bp[0x10 + i] & 0x1FFFFF;
        ind[16] = bp[0x27]; ind[17] = bp[0x25]; ind[18] = bp[0x26];
        for (int i = 0; i < 8; i++) { ind[20 + 2 * i] = (bp[0x30 + 2 * i] & 0xFFFF) + 1; ind[21 + 2 * i] = (bp[0x31 + 2 * i] & 0xFFFF) + 1; }
        GLint mt[24];
        for (int m = 0; m < 3; m++) {
            u32 a = bp[0x06 + 3 * m], b = bp[0x07 + 3 * m], c = bp[0x08 + 3 * m];
#define S11(x) ((GLint)((x) << 21) >> 21)
            int scale = ((a >> 22) & 3) | ((b >> 22) & 3) << 2 | ((c >> 22) & 3) << 4;
            GLint* r = mt + 8 * m;
            r[0] = S11(a & 0x7FF); r[1] = S11(b & 0x7FF); r[2] = S11(c & 0x7FF); r[3] = 17 - scale;
            r[4] = S11((a >> 11) & 0x7FF); r[5] = S11((b >> 11) & 0x7FF); r[6] = S11((c >> 11) & 0x7FF); r[7] = 17 - scale;
#undef S11
        }
        glUniform1uiv(u_ind, 40, ind); glUniform1iv(u_imtx, 24, mt);
    }
    /* textures used by enabled TEV stages and indirect stages */
    int ns = ((bp[0x00] >> 10) & 15) + 1; u32 used = 0;
    { int nind = (bp[0x00] >> 16) & 7; static int logged; if (nind && logged < 8) { logged++; rt_log("GL: indirect draw nind=%d iref=%06X cmd0=%06X cmd1=%06X mtx0=%06X %06X %06X\n", nind, bp[0x27], bp[0x10], bp[0x11], bp[0x06], bp[0x07], bp[0x08]); } for (int i = 0; i < nind && i < 4; i++) used |= 1u << ((bp[0x27] >> (6 * i)) & 7); }
    for (int s = 0; s < ns; s++) { u32 o = bp[0x28 + s / 2]; if (s & 1) o >>= 12; if ((o >> 6) & 1) used |= 1u << (o & 7); }
    for (int m = 0; m < 8; m++) if (used & (1u << m)) bind_tex(m);
    /* depth */
    u32 zm = bp[0x40];
    { extern int g_exp; if (g_exp & 2) zm = 0; }
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
    if (sx1 >= sx0 && sy1 >= sy0) { glEnable(GL_SCISSOR_TEST); glScissor(sx0 * g_scale, (EFB_H - 1 - sy1) * g_scale, (sx1 - sx0 + 1) * g_scale, (sy1 - sy0 + 1) * g_scale); }
    else glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_nv * 36 * 4, g_vb, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_nv);
    g_nv = 0;
}

/* read EFB region (y down) into RGBA8 buffer laid out EFB_W wide */
void gl_read_efb(u32* dst, int x, int y, int w, int h) {
    gl_flush(); u64 t0 = host_tb(); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_SCISSOR_TEST);
    if (g_scale > 1) {   /* downsample the scaled region to native size first */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rfbo);
        glBlitFramebuffer(x * g_scale, (EFB_H - y - h) * g_scale, (x + w) * g_scale, (EFB_H - y) * g_scale, x, EFB_H - y - h, x + w, EFB_H - y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rfbo);
    } else glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    static u32* tmp; static int tn; if (w * h > tn) { tn = w * h; tmp = realloc(tmp, (size_t)tn * 4); }
    glReadPixels(x, EFB_H - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int r = 0; r < h; r++) memcpy(dst + (size_t)(y + r) * EFB_W + x, tmp + (size_t)(h - 1 - r) * w, (size_t)w * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    g_pf_read += host_tb() - t0;
}
/* read EFB depth (24-bit) into a buffer laid out EFB_W wide */
void gl_read_efb_depth(u32* dst, int x, int y, int w, int h) {
    gl_flush(); glDisable(GL_SCISSOR_TEST);
    if (g_scale > 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rfbo);
        glBlitFramebuffer(x * g_scale, (EFB_H - y - h) * g_scale, (x + w) * g_scale, (EFB_H - y) * g_scale, x, EFB_H - y - h, x + w, EFB_H - y, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rfbo);
    } else glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    static u32* tmp; static int tn; if (w * h > tn) { tn = w * h; tmp = realloc(tmp, (size_t)tn * 4); }
    glReadPixels(x, EFB_H - y - h, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, tmp);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    for (int r = 0; r < h; r++) { const u32* s = tmp + (size_t)(h - 1 - r) * w; u32* d = dst + (size_t)(y + r) * EFB_W + x; for (int i = 0; i < w; i++) d[i] = s[i] >> 8; }
}
void gl_clear(int x, int y, int w, int h, u32 rgba, u32 z24, int color, int alpha, int depth) {
    gl_flush(); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glEnable(GL_SCISSOR_TEST); glScissor(x * g_scale, (EFB_H - y - h) * g_scale, w * g_scale, h * g_scale);
    glColorMask(color, color, color, alpha); glDepthMask(depth);
    glClearColor((rgba & 255) / 255.f, ((rgba >> 8) & 255) / 255.f, ((rgba >> 16) & 255) / 255.f, (rgba >> 24) / 255.f);
    glClearDepth(z24 / 16777215.0);
    glClear((color || alpha ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    glColorMask(1, 1, 1, 1);
}

/* frame interpolation: the real frame is kept in a texture while the in-between frame is shown */
static GLuint g_sfbo, g_stex; static int g_show_saved;
void gl_save_real(void) {
    gl_flush();
    if (!g_sfbo) {
        glGenFramebuffers(1, &g_sfbo); glGenTextures(1, &g_stex); glBindTexture(GL_TEXTURE_2D, g_stex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_sfbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_stex, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sfbo);
    glBlitFramebuffer(0, 0, SW, SH, 0, 0, SW, SH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}
void gl_show_saved(int on) { g_show_saved = on; }
void gl_blit_to_screen(int w, int h) {
    gl_flush();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h); glClearColor(0, 0, 0, 1); glColorMask(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    int dw = w, dh = w * 3 / 4; if (dh > h) { dh = h; dw = h * 4 / 3; }
    int dx = (w - dw) / 2, dy = (h - dh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_show_saved && g_sfbo ? g_sfbo : g_fbo);
    glBlitFramebuffer(0, (EFB_H - 480) * g_scale, 640 * g_scale, EFB_H * g_scale, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}
