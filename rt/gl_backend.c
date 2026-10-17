/* gl_backend.c - OpenGL 3.3 renderer for the GX pipeline.
   Vertices are transformed/lit/clipped on the CPU (gx.c); the GPU rasterizes and runs the TEV
   as a single "uber" fragment shader driven by the raw BP register words. */
#include "rt.h"
#include "glmini.h"
#include "pc.h"
int settings_get(const char* key, int def);
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
void gl_state_dirty(void);
#define VSTRIDE 40   /* floats per GL vertex: pos4, col0, col1, 8 x (s,t,q), 4 clip distances */
static const char* VS =
"#version 330 core\n"
"layout(location=0) in vec4 a_pos; layout(location=1) in vec4 a_c0; layout(location=2) in vec4 a_c1;\n"
"layout(location=3) in vec3 a_t0; layout(location=4) in vec3 a_t1; layout(location=5) in vec3 a_t2; layout(location=6) in vec3 a_t3;\n"
"layout(location=7) in vec3 a_t4; layout(location=8) in vec3 a_t5; layout(location=9) in vec3 a_t6; layout(location=10) in vec3 a_t7;\n"
"layout(location=11) in vec4 a_clip;\n"   /* GX near/far clip planes (z >= -w, z <= 0 in GX clip space), as Dolphin: the GPU clips, depth is clamped; zw: widescreen 4:3 side planes */
"out vec4 v_c0; out vec4 v_c1; out vec3 v_t[8];\n"
"void main(){ gl_Position=a_pos; gl_ClipDistance[0]=a_clip.x; gl_ClipDistance[1]=a_clip.y; gl_ClipDistance[2]=a_clip.z; gl_ClipDistance[3]=a_clip.w; v_c0=a_c0; v_c1=a_c1; v_t[0]=a_t0; v_t[1]=a_t1; v_t[2]=a_t2; v_t[3]=a_t3; v_t[4]=a_t4; v_t[5]=a_t5; v_t[6]=a_t6; v_t[7]=a_t7; }\n";

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
"uniform float u_scale; uniform vec2 u_texdim[8]; /* logical texture size per map */\n"
"uniform vec4 u_fog; uniform vec3 u_fogcol; uniform uint u_fogmode; /* fog: A, C, B magnitude, B shift; mode = fsel | proj<<3 */\n"
"vec4 samp(uint m, vec2 uv){ switch(m){ case 0u: return texture(u_tex0,uv); case 1u: return texture(u_tex1,uv); case 2u: return texture(u_tex2,uv); case 3u: return texture(u_tex3,uv);\n"
"  case 4u: return texture(u_tex4,uv); case 5u: return texture(u_tex5,uv); case 6u: return texture(u_tex6,uv); default: return texture(u_tex7,uv);} }\n"
"ivec2 tsize(uint m){ switch(m){ case 0u: return textureSize(u_tex0,0); case 1u: return textureSize(u_tex1,0); case 2u: return textureSize(u_tex2,0); case 3u: return textureSize(u_tex3,0);\n"
"  case 4u: return textureSize(u_tex4,0); case 5u: return textureSize(u_tex5,0); case 6u: return textureSize(u_tex6,0); default: return textureSize(u_tex7,0);} }\n"
"vec4 sampi(uint m, ivec2 uv){ return samp(m, vec2(uv)/(u_texdim[m]*128.0)); }\n"
"/* GX texture coordinates are scaled by SU_SSIZE/TSIZE (BP 0x30+), normally equal to the texture size but not always */\n"
"vec4 sampc(uint m, uint tc, vec2 uv){ return samp(m, uv*vec2(float(u_ind[20u+2u*tc]),float(u_ind[21u+2u*tc]))/u_texdim[m]); }\n"
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
"    else { if(ten!=0u && ntg>0u){ vec3 t=v_t[tcu]; float q=(t.z==0.0)?1.0:t.z; texc=swz(sampc(tmap,tcu,t.xy/q),(ae>>2)&3u); tevc=fuv(tcu); } }\n"
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
/* MSAA (options): the game renders into a multisampled copy of the EFB, resolved into g_fbo/g_fbtex before
   anything reads it (EFB copies, readbacks, presentation) */
static int g_msaa = 1, g_ms_dirty; static GLuint g_msfbo, g_msc, g_msd;
#define DRAWFBO (g_msaa > 1 ? g_msfbo : g_fbo)
static void efb_resolve(void);
static GLint u_texdim, u_scale, u_cenv, u_aenv, u_order, u_ksel, u_misc, u_reg, u_konst, u_tex[8], u_fog, u_fogcol, u_fogmode, u_ind, u_imtx, u_fogk, u_fogrc;
int g_gl_ok;

#define EFB_W 640
#define EFB_H 528
int g_scale = 1;                /* internal resolution multiplier (settings.ini resolution_scale) */
int g_ws;                       /* widescreen: the EFB is 4/3 wider; the game's 640-wide picture is squeezed into it (gx.c) */
#define SW ((int)(EFB_W * g_scale * (g_ws ? 4.0 / 3.0 : 1.0) + 0.5))
#define SH (EFB_H * g_scale)
#define XS(v) ((int)((double)(v) * SW / EFB_W + 0.5))   /* EFB x (logical) -> framebuffer x */
#define YS(v) ((v) * g_scale)
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
    u_texdim = glGetUniformLocation(g_prog, "u_texdim");
    u_scale = glGetUniformLocation(g_prog, "u_scale"); glUniform1f(u_scale, (float)SW / EFB_W);
    u_fogk = glGetUniformLocation(g_prog, "u_fogk"); u_fogrc = glGetUniformLocation(g_prog, "u_fogrc");
    u_ind = glGetUniformLocation(g_prog, "u_ind"); u_imtx = glGetUniformLocation(g_prog, "u_imtx");
    for (int i = 0; i < 8; i++) { char n[16]; snprintf(n, sizeof n, "u_tex%d", i); u_tex[i] = glGetUniformLocation(g_prog, n); glUniform1i(u_tex[i], i); }
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    int stride = VSTRIDE * 4;
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, 0, stride, (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_FLOAT, 0, stride, (void*)(4 * 4));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, stride, (void*)(8 * 4));
    for (int t = 0; t < 8; t++) { glEnableVertexAttribArray(3 + t); glVertexAttribPointer(3 + t, 3, GL_FLOAT, 0, stride, (void*)((size_t)(12 + 3 * t) * 4)); }
    glEnableVertexAttribArray(11); glVertexAttribPointer(11, 4, GL_FLOAT, 0, stride, (void*)(36 * 4));
    glEnable(GL_DEPTH_CLAMP);
    /* the four clip planes stay enabled (toggling them per draw makes some drivers rebuild their pipeline); every
       vertex shader writes them, the copy and overlay ones with "inside" */
    glEnable(GL_CLIP_DISTANCE0); glEnable(GL_CLIP_DISTANCE1); glEnable(GL_CLIP_DISTANCE2); glEnable(GL_CLIP_DISTANCE3);
    /* EFB */
    glGenFramebuffers(1, &g_fbo); glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
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
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    glViewport(0, 0, SW, SH);
    glClearColor(0, 0, 0, 0); glClearDepth(1.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    g_gl_ok = 1;
    { int ms = settings_get("msaa", 1); if (getenv("SOA_MSAA")) ms = atoi(getenv("SOA_MSAA")); if (ms > 1) gl_set_msaa(ms); }
    return 1;
}

/* ---------------- texture cache ---------------- */
typedef struct { u32 key[4]; u32 hash; GLuint tex; u32 w, h; u64 frame; int levels; } TexEnt;
#define TCACHE 4096
static TexEnt g_tc[TCACHE];
u64 g_gl_frame;
static u32 fnv(const u8* p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i += 4) { h ^= p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (u32)p[i + 3] << 24; h *= 16777619u; } return h; }

/* ---------------- EFB copies to texture, kept on the GPU ----------------
   Like Dolphin's "store EFB copies to texture only": a texture copy is rendered straight into a GL texture
   (format conversion in a shader) and registered under its destination address. When the game later
   samples a texture at that address with the matching size and format, the GL texture is bound instead of
   decoding RAM. This removes the per-copy readback stall. RAM is not written; a sampled hash of the
   destination RAM is recorded so the entry is dropped if something else is later loaded there. */
static const char* CVS = "#version 330 core\nvoid main(){ vec2 p=vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2)); gl_Position=vec4(p*2.0-1.0,0.0,1.0); gl_ClipDistance[0]=1.0; gl_ClipDistance[1]=1.0; gl_ClipDistance[2]=1.0; gl_ClipDistance[3]=1.0; }\n";
static const char* CFS =
"#version 330 core\n"
"uniform sampler2D u_src; uniform vec4 u_rect; uniform vec2 u_dsize; uniform uint u_fmt; uniform uint u_flags; uniform float u_gamma;\n"
"out vec4 o;\n"
"float q(float v, float bits){ float m=exp2(bits)-1.0; return floor(v*m+0.0001)/m; }\n"   /* keep the top bits, as the GX encoders */
"void main(){\n"
"  vec2 d=gl_FragCoord.xy/u_dsize; vec2 p=vec2(u_rect.x+d.x*u_rect.z, u_rect.y+d.y*u_rect.w);\n"
"  vec4 c=texture(u_src, vec2(p.x/640.0, 1.0-p.y/528.0)); c=floor(c*255.0+0.5)/255.0;\n"
"  if((u_flags&2u)!=0u) c.a=1.0;\n"
"  if(u_gamma!=1.0) c.rgb=pow(c.rgb, vec3(1.0/u_gamma));\n"
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
static GLuint g_cprog, g_cvao; static GLint uc_src, uc_rect, uc_dsize, uc_fmt, uc_flags, uc_gamma;
/* Each address keeps up to three versions of its copy: the current one, the one that was current when the
   frame started (frame interpolation replays a frame from its start, so it must see that one), and a shadow
   that the replay itself renders into. */
typedef struct { u32 tw, th, tfmt, nbytes, rhash; GLuint tex, fbo; int gw, gh; u32 gen; GLuint ctex, cfbo; u32 cw, ch, cgen; } CopyVer;
typedef struct { u32 addr; CopyVer v[3]; int cur, start, shadow; u64 frame; int used; } CopyEnt;
#define NCOPY 128
static CopyEnt g_copies[NCOPY];
int g_gpu_copies = 1;
int gx_replaying(void);
static u32 sampled_hash(u32 a, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i + 4 <= n; i += 256) { h ^= *(const u32*)(g_ram + ((a + i) & 0x01FFFFFC)); h *= 16777619u; } return h ^ n; }
static GLuint compile(GLenum t, const char* src);
/* copy fmt (GX EFB copy format) -> texture format the game will sample it as */
static int copy_tex_fmt(int f) { static const int m[13] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3}; return f >= 0 && f < 13 ? m[f] : -1; }
void gl_copies_clear(void) { for (int i = 0; i < NCOPY; i++) g_copies[i].used = 0; }
void gl_copies_frame_start(void) { for (int i = 0; i < NCOPY; i++) { g_copies[i].start = g_copies[i].cur; g_copies[i].shadow = -1; } }
int gl_efb_copy_tex(int sx, int sy, int w, int h, int half, int fmt, int inten, int noalpha, u32 dst, u32 nbytes, int gamma) {
    if (!g_gpu_copies || copy_tex_fmt(fmt) < 0) return 0;
    gl_flush(); gl_state_dirty(); efb_resolve();
    if (!g_cprog) {
        GLuint vs = compile(GL_VERTEX_SHADER, CVS), fs = compile(GL_FRAGMENT_SHADER, CFS);
        g_cprog = glCreateProgram(); glAttachShader(g_cprog, vs); glAttachShader(g_cprog, fs); glLinkProgram(g_cprog);
        GLint ok; glGetProgramiv(g_cprog, GL_LINK_STATUS, &ok); if (!ok) { g_gpu_copies = 0; rt_log("GL: copy shader link failed\n"); return 0; }
        uc_src = glGetUniformLocation(g_cprog, "u_src"); uc_rect = glGetUniformLocation(g_cprog, "u_rect"); uc_dsize = glGetUniformLocation(g_cprog, "u_dsize");
        uc_fmt = glGetUniformLocation(g_cprog, "u_fmt"); uc_flags = glGetUniformLocation(g_cprog, "u_flags"); uc_gamma = glGetUniformLocation(g_cprog, "u_gamma");
        glGenVertexArrays(1, &g_cvao);
    }
    int tw = half ? w / 2 : w, th = half ? h / 2 : h; if (tw < 1 || th < 1) return 1;
    int gw = XS(tw), gh = YS(th);
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
    v->tw = tw; v->th = th; v->tfmt = copy_tex_fmt(fmt); v->nbytes = nbytes; e->frame = g_gl_frame; v->gen++;
    v->rhash = sampled_hash(a, nbytes);
    glBindFramebuffer(GL_FRAMEBUFFER, v->fbo); glViewport(0, 0, gw, gh);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_COLOR_LOGIC_OP); glColorMask(1, 1, 1, 1);
    glUseProgram(g_cprog); glBindVertexArray(g_cvao);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_fbtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(uc_src, 0); { float r[4] = {(float)sx, (float)sy, (float)w, (float)h}; glUniform4fv(uc_rect, 1, r); }
    { float ds[2] = {(float)gw, (float)gh}; glUniform2fv(uc_dsize, 1, ds); }
    glUniform1ui(uc_fmt, (GLuint)fmt); glUniform1ui(uc_flags, (inten ? 1u : 0u) | (noalpha ? 2u : 0u));
    glUniform1f(uc_gamma, gamma == 1 ? 1.7f : gamma == 2 ? 2.2f : 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO); glViewport(0, 0, SW, SH);
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
        if (!v->tex || v->tfmt != fmt || w < v->tw || h < v->th) return 0;
        if (sampled_hash(a, v->nbytes) != v->rhash) { if (!gx_replaying()) c->used = 0; return 0; }   /* overwritten by other data */
        c->frame = g_gl_frame;
        if (v->tw == w && v->th == h) return v->tex;
        /* the copy fills the top-left of a larger texture (e.g. a 640x480 screen grab into a 1024x512 texture that
           the battle transition warps): sample it from a texture of the full size with the copy in that corner */
        if (!v->ctex || v->cw != w || v->ch != h || v->cgen != v->gen) {
            int cw = (int)((u64)v->gw * w / v->tw), chh = (int)((u64)v->gh * h / v->th);
            if (!v->ctex) { glGenTextures(1, &v->ctex); glGenFramebuffers(1, &v->cfbo); }
            gl_state_dirty();   /* called while a draw is being set up (bind_tex): the rest of gl_flush_ re-applies its state */
            glBindTexture(GL_TEXTURE_2D, v->ctex);
            { void* z = calloc((size_t)cw * chh, 4); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, chh, 0, GL_RGBA, GL_UNSIGNED_BYTE, z); free(z); }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, v->cfbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->ctex, 0);
            glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, v->fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, v->cfbo);
            glBlitFramebuffer(0, 0, v->gw, v->gh, 0, 0, v->gw, v->gh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
            v->cw = w; v->ch = h; v->cgen = v->gen;
        }
        return v->ctex;
    }
    return 0;
}

u64 host_tb(void);
u64 g_pf_idle, g_pf_snd, g_pf_hash, g_pf_flush, g_pf_read, g_pf_present, g_pf_draws, g_pf_upload, g_pf_ccopy, g_pf_zcopy, g_pf_ztime, g_pf_upn, g_pf_upbytes;   /* perf counters (timebase ticks) */
static float g_texdim[8][2];
/* ---------------- custom textures (Dolphin-format packs, pcfeat.c) ---------------- */
int texpack_enabled(void); const char* texpack_lookup(const char* full, const char* wild_tlut, const char* wild_tex);
u8* texpack_load_png(const char* path, int* w, int* h); void texpack_free(void* p); void texpack_mip_path(const char* base, int level, char* out, int cap);
int gx_tex_dolphin_name(int map, char* out, int cap, char* wild_tlut, char* wild_tex);
static int g_aniso_max = -1;
int g_aniso = -1;   /* anisotropic filtering level (options; 0/1 = off) */
/* DDS (BC1/BC2/BC3/BC7 or RGBA8), straight to the GPU; returns the number of levels uploaded, 0 if unsupported */
static int upload_dds(const char* path) {
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* b = malloc(n); if (!b || fread(b, 1, n, f) != (size_t)n || n < 128 || memcmp(b, "DDS ", 4)) { fclose(f); free(b); return 0; }
    fclose(f);
    u32 h = *(u32*)(b + 12), w = *(u32*)(b + 16), mips = *(u32*)(b + 28), pff = *(u32*)(b + 80), cc = *(u32*)(b + 84); long off = 128;
    if (!mips) mips = 1;
    GLenum fmt = 0; u32 bsz = 16;
    if (pff & 4) {
        if (cc == 0x31545844) { fmt = 0x83F1; bsz = 8; } else if (cc == 0x33545844) fmt = 0x83F2; else if (cc == 0x35545844) fmt = 0x83F3;
        else if (cc == 0x30315844 && n >= 148) { u32 dx = *(u32*)(b + 128); off = 148; if (dx == 98 || dx == 99) fmt = 0x8E8C; else if (dx == 71 || dx == 72) { fmt = 0x83F1; bsz = 8; } else if (dx == 77 || dx == 78) fmt = 0x83F3; else if (dx == 74 || dx == 75) fmt = 0x83F2; else if (dx == 28 || dx == 29) fmt = 1; }
    } else if (*(u32*)(b + 88) == 32) fmt = 2;   /* uncompressed 32-bit */
    if (!fmt) { free(b); return 0; }
    u32 lw = w, lh = h; int lv = 0;
    for (u32 m = 0; m < mips; m++) {
        if (fmt == 1 || fmt == 2) {
            u32 sz = lw * lh * 4; if (off + sz > n) break;
            if (fmt == 2) { u8* p = b + off; for (u32 i = 0; i < lw * lh; i++) { u8 t = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = t; } }   /* BGRA -> RGBA */
            glTexImage2D(GL_TEXTURE_2D, (GLint)m, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, b + off); off += sz;
        } else {
            u32 sz = ((lw + 3) / 4) * ((lh + 3) / 4) * bsz; if (off + sz > n) break;
            glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)m, fmt, lw, lh, 0, (GLsizei)sz, b + off); off += sz;
        }
        lv++; lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    free(b);
    if (!lv) return 0;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lv - 1);
    return lv;
}
static int upload_custom(int map, int levels) {
    char full[128], wt[96], wx[96];
    if (!gx_tex_dolphin_name(map, full, sizeof full, wt, wx)) return 0;
    const char* path = texpack_lookup(full, wt, wx); if (!path) return 0;
    { static int n; if (n++ < 40) rt_log("texture pack: %s -> %s\n", full, path); }
    size_t pl = strlen(path);
    if (pl > 4 && (!strcmp(path + pl - 4, ".dds") || !strcmp(path + pl - 4, ".DDS"))) {
        int lv = upload_dds(path); if (!lv) return 0;
        if (lv == 1 && levels > 1) { glGenerateMipmap(GL_TEXTURE_2D); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000); return 11; }
        return lv;
    }
    int w, h; u8* px = texpack_load_png(path, &w, &h); if (!px) return 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    texpack_free(px);
    int lv = 1;
    if (levels > 1) {   /* the pack's own mip levels when present, else generated */
        for (int l = 1; l < 14; l++) { char mp[1100]; texpack_mip_path(path, l, mp, sizeof mp); int mw, mh; u8* m = texpack_load_png(mp, &mw, &mh); if (!m) break; glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, mw, mh, 0, GL_RGBA, GL_UNSIGNED_BYTE, m); texpack_free(m); lv++; }
        if (lv == 1) { glGenerateMipmap(GL_TEXTURE_2D); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000); return 11; }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lv - 1);
    return lv;
}
/* drop all cached textures (texture pack toggled): they are re-uploaded on next use */
void gl_texcache_clear(void) { for (int i = 0; i < TCACHE; i++) if (g_tc[i].tex) { glDeleteTextures(1, &g_tc[i].tex); memset(&g_tc[i], 0, sizeof g_tc[i]); } }
static void bind_tex(int map) {
    u32 key[4], w, h, mode0, nbytes, addr;
    gx_tex_info(map, key, &w, &h, &mode0, &nbytes, &addr);
    g_texdim[map][0] = (float)w; g_texdim[map][1] = (float)h;
    int levels = gx_tex_levels(map);
    GLuint ctex = (nbytes && g_gpu_copies) ? copy_lookup(addr, w, h, key[2] & 15) : 0;
    GLuint bound;
    if (ctex) { levels = 1; glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, ctex); bound = ctex; goto params; }
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
    /* cached by content (hash, size, format, palette), not by address: the game renders text glyphs into a
       small ring of buffers every frame, so the same glyph turns up at changing addresses */
    u32 slot = (hash * 2654435761u ^ key[1] * 40503u ^ key[2]) % TCACHE;
    TexEnt* e = 0;
    for (int probe = 0; probe < 8; probe++) {
        TexEnt* t = &g_tc[(slot + probe) % TCACHE];
        if (t->tex && t->key[1] == key[1] && t->key[2] == key[2] && t->hash == hash && t->levels >= levels) { e = t; break; }
    }
    if (!e) {
        TexEnt* t = &g_tc[slot]; u64 oldest = ~0ull;
        for (int probe = 0; probe < 8; probe++) { TexEnt* q = &g_tc[(slot + probe) % TCACHE]; if (!q->tex) { t = q; break; } if (q->frame < oldest) { oldest = q->frame; t = q; } }
        u64 tu = host_tb();
        static u32* buf; static u32 bufn;
        if (w * h > bufn) { bufn = w * h; buf = realloc(buf, bufn * 4); }
        if (!t->tex) glGenTextures(1, &t->tex);
        glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, t->tex);
        int hd = texpack_enabled() ? upload_custom(map, levels) : 0;
        if (!hd) {
            gx_tex_decode(map, buf);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            for (int l = 1; l < levels; l++) {
                u32 lw, lh; gx_tex_decode_level(map, l, buf, &lw, &lh);
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        } else if (hd > 1) levels = hd;
        memcpy(t->key, key, 12); t->hash = hash; t->w = w; t->h = h; t->levels = levels; e = t;
        g_pf_upload += host_tb() - tu; g_pf_upn++; g_pf_upbytes += (u64)w * h * 4;
        { static int lg = -1; if (lg < 0) lg = getenv("SOA_TEXLOG") ? atoi(getenv("SOA_TEXLOG")) : 0; if (lg > 0) { lg--; extern int g_frame_no_pub; rt_log("texup f%d map %d addr %08X %ux%u fmt %u levels %d (%u B) tlut %08X\n", g_frame_no_pub, map, addr, w, h, key[2] & 15, levels, nbytes, key[1]); } }
    }
    e->frame = g_gl_frame;
    glActiveTexture(GL_TEXTURE0 + map); glBindTexture(GL_TEXTURE_2D, e->tex); bound = e->tex;
params:;
    {   /* sampler state lives in the texture object: only re-send it when it differs from what the object has */
        GLuint cur = bound;
        int b = map < 4 ? map : map - 4 + 0x20; u32 m1 = levels > 1 ? gx_bp_regs()[0x84 + b] : 0;
        extern int g_aniso; if (g_aniso < 0) g_aniso = settings_get("anisotropy", 4);
        u32 want0 = (mode0 & 0xFFFF) | (levels > 1 ? 1u << 16 : 0) | (u32)g_aniso << 20, want1 = m1 & 0xFFFF;
        typedef struct { GLuint tex; u32 m0, m1; } SP; static SP sp[4096];
        SP* e = &sp[cur & 4095];
        if (e->tex == cur && e->m0 == want0 && e->m1 == want1) return;
        e->tex = cur; e->m0 = want0; e->m1 = want1;
    }
    static const GLint wraps[4] = { GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_REPEAT };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wraps[mode0 & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wraps[(mode0 >> 2) & 3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (mode0 >> 4) & 1 ? GL_LINEAR : GL_NEAREST);
    {   /* min filter: bit7 linear, bits5-6 mip mode (0 none, 1 point, 2 linear); LOD range/bias from mode0/mode1 */
        int lin = (mode0 >> 7) & 1, mip = (mode0 >> 5) & 3;
        GLint f = lin ? GL_LINEAR : GL_NEAREST;
        if (levels > 1 && mip) f = lin ? (mip == 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mip == 2 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        {   /* anisotropic filtering (options): only for filtered, mipmapped textures, as on PC games */
            extern int g_aniso; if (g_aniso_max < 0) { GLfloat m = 1; glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &m); g_aniso_max = (int)m; while (glGetError()) ; }
            float a = (lin && levels > 1 && mip && g_aniso > 1) ? (float)(g_aniso < g_aniso_max ? g_aniso : g_aniso_max) : 1.f;
            if (g_aniso_max > 1) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, a);
        }
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
void gl_vertex(const float* pos4, const float* clip4, const float* c0, const float* c1, const float tex[8][3]) {
    if (g_nv + 1 > g_cap) { g_cap = g_cap ? g_cap * 2 : 65536; g_vb = realloc(g_vb, (size_t)g_cap * VSTRIDE * 4); }
    float* v = g_vb + (size_t)g_nv * VSTRIDE;
    memcpy(v, pos4, 16); memcpy(v + 4, c0, 16); memcpy(v + 8, c1, 16);
    memcpy(v + 12, tex, 8 * 3 * 4);
    memcpy(v + 36, clip4, 16);
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
/* shadow of the GL state the batch flush sets, so unchanged state is not re-sent (NVIDIA/AMD drivers charge for every
   call); every other path that touches this state marks it dirty */
static struct { int valid; u32 zm, bm, bp42, noalpha, sc0, sc1; int scale, ws; } g_gs;
void gl_state_dirty(void) { g_gs.valid = 0; }
void gl_flush(void) { if (!g_nv) return; u64 t = host_tb(); gl_flush_(); g_pf_flush += host_tb() - t; g_pf_draws++; }
static void gl_flush_(void) {
    u32* bp = gx_bp_regs();
    int fresh = !g_gs.valid || g_gs.scale != g_scale || g_gs.ws != g_ws;
    if (fresh) { glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO); glViewport(0, 0, SW, SH); glUseProgram(g_prog); glBindVertexArray(g_vao); glBindBuffer(GL_ARRAY_BUFFER, g_vbo); }
    /* skip re-uploading the shader constants when none of the registers they come from changed since the last draw */
    static u32 g_ulast[0x50 + 0x40 + 16 + 1]; static int g_uvalid; u32 ucur[0x50 + 0x40 + 16 + 1];
    { extern u32 g_tevcol[2][8]; memcpy(ucur, bp, 0x50 * 4); memcpy(ucur + 0x50, bp + 0xC0, 0x40 * 4); memcpy(ucur + 0x90, g_tevcol, 16 * 4); ucur[0xA0] = (u32)g_scale << 1 | (u32)g_ws; }
    int same = g_uvalid && !memcmp(ucur, g_ulast, sizeof ucur);
    memcpy(g_ulast, ucur, sizeof ucur); g_uvalid = 1;
    int noalpha = (bp[0x43] & 7) != 1;
    if (!same) {
    u32 cenv[16], aenv[16];
    for (int i = 0; i < 16; i++) { cenv[i] = bp[0xC0 + 2 * i]; aenv[i] = bp[0xC1 + 2 * i]; }
    glUniform1uiv(u_cenv, 16, cenv); glUniform1uiv(u_aenv, 16, aenv);
    glUniform1uiv(u_order, 8, bp + 0x28); glUniform1uiv(u_ksel, 8, bp + 0xF6);
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
    }   /* !same */
    /* textures used by enabled TEV stages and indirect stages */
    int ns = ((bp[0x00] >> 10) & 15) + 1; u32 used = 0;
    { int nind = (bp[0x00] >> 16) & 7; static int logged; if (nind && logged < 8) { logged++; rt_log("GL: indirect draw nind=%d iref=%06X cmd0=%06X cmd1=%06X mtx0=%06X %06X %06X\n", nind, bp[0x27], bp[0x10], bp[0x11], bp[0x06], bp[0x07], bp[0x08]); } for (int i = 0; i < nind && i < 4; i++) used |= 1u << ((bp[0x27] >> (6 * i)) & 7); }
    for (int s = 0; s < ns; s++) { u32 o = bp[0x28 + s / 2]; if (s & 1) o >>= 12; if ((o >> 6) & 1) used |= 1u << (o & 7); }
    for (int m = 0; m < 8; m++) if (used & (1u << m)) bind_tex(m);
    if (!g_gs.valid) fresh = 1;   /* binding a texture may have had to render (copy_lookup) */
    glUniform2fv(u_texdim, 8, &g_texdim[0][0]);
    /* depth */
    u32 zm = bp[0x40];
    { extern int g_exp; if (g_exp & 2) zm = 0; }
    if (fresh || g_gs.zm != zm) {
    if (zm & 1) { static const GLenum f[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS}; glEnable(GL_DEPTH_TEST); glDepthFunc(f[(zm >> 1) & 7]); glDepthMask((zm >> 4) & 1); }
    else { glDisable(GL_DEPTH_TEST); glDepthMask(0); }
    }
    /* blend, derived as Dolphin's BlendingState::Generate */
    u32 bm = bp[0x41];
    if (fresh || g_gs.bm != bm || g_gs.bp42 != bp[0x42] || g_gs.noalpha != (u32)noalpha) {
    int color_update = (bm >> 3) & 1, alpha_update = ((bm >> 4) & 1) && !noalpha;
    int dst_alpha = ((bp[0x42] >> 8) & 1) && alpha_update;
    int blend_en = bm & 1, logic_en = (bm >> 1) & 1, logic_mode = (bm >> 12) & 15;
    glDisable(GL_COLOR_LOGIC_OP);
    if (blend_en) {
        glEnable(GL_BLEND);
        if ((bm >> 11) & 1) {   /* subtract: dst - src for colour; alpha too unless a constant destination alpha is written */
            glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, dst_alpha ? GL_FUNC_ADD : GL_FUNC_REVERSE_SUBTRACT);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, dst_alpha ? GL_ZERO : GL_ONE);
        } else {
            u32 sf = (bm >> 8) & 7, df = (bm >> 5) & 7;
            /* alpha factors: colour factors cannot be used for alpha, so SRCCLR/DSTCLR become the matching alpha factors */
            u32 sfa = sf == 2 ? 6 : sf == 3 ? 7 : sf, dfa = df == 2 ? 4 : df == 3 ? 5 : df;
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(gl_src(sf, noalpha), gl_dst(df, noalpha), dst_alpha ? GL_ONE : gl_src(sfa, noalpha), dst_alpha ? GL_ZERO : gl_dst(dfa, noalpha));
        }
    } else {
        glDisable(GL_BLEND);
        if (logic_en) {
            if (logic_mode == 3) { color_update = 0; alpha_update = alpha_update && dst_alpha; }   /* NOOP: a depth-only pass (colour untouched) */
            else { glEnable(GL_COLOR_LOGIC_OP); glLogicOp(0x1500 + logic_mode); }
        }
    }
    glColorMask(color_update, color_update, color_update, alpha_update);
    }
    /* scissor */
    u32 sc0 = bp[0x20], sc1 = bp[0x21];
    if (fresh || g_gs.sc0 != sc0 || g_gs.sc1 != sc1) {
    int sy0 = (sc0 & 0x7FF) - 342, sx0 = ((sc0 >> 12) & 0x7FF) - 342, sy1 = (sc1 & 0x7FF) - 342, sx1 = ((sc1 >> 12) & 0x7FF) - 342;
    if (sx1 >= sx0 && sy1 >= sy0) { glEnable(GL_SCISSOR_TEST); glScissor(XS(sx0), YS(EFB_H - 1 - sy1), XS(sx1 + 1) - XS(sx0), YS(sy1 - sy0 + 1)); }
    else glDisable(GL_SCISSOR_TEST);
    }
    g_gs.valid = 1; g_gs.zm = zm; g_gs.bm = bm; g_gs.bp42 = bp[0x42]; g_gs.noalpha = (u32)noalpha; g_gs.sc0 = sc0; g_gs.sc1 = sc1; g_gs.scale = g_scale; g_gs.ws = g_ws;
    /* vertex streaming: one large buffer written sequentially, orphaned when it wraps (no per-draw allocation) */
    #define VBO_BYTES (48u << 20)
    static u32 vbo_off, vbo_alloc;
    u32 bytes = (u32)g_nv * VSTRIDE * 4;
    if (bytes > VBO_BYTES) { glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, g_vb, GL_STREAM_DRAW); vbo_alloc = 0; vbo_off = 0; }
    else {
        if (!vbo_alloc || vbo_off + bytes > VBO_BYTES) { glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)VBO_BYTES, 0, GL_STREAM_DRAW); vbo_alloc = 1; vbo_off = 0; }
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)vbo_off, (GLsizeiptr)bytes, g_vb);
    }
    glDrawArrays(GL_TRIANGLES, (GLint)(vbo_off / (VSTRIDE * 4)), g_nv);
    g_ms_dirty = 1;
    if (vbo_alloc) vbo_off += bytes;
    g_nv = 0;
}

/* read EFB region (y down) into RGBA8 buffer laid out EFB_W wide */
void gl_read_efb(u32* dst, int x, int y, int w, int h) {
    gl_flush(); gl_state_dirty(); efb_resolve(); u64 t0 = host_tb(); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_SCISSOR_TEST);
    if (SW != EFB_W || SH != EFB_H) {   /* downsample the scaled region to native size first */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rfbo);
        glBlitFramebuffer(XS(x), YS(EFB_H - y - h), XS(x + w), YS(EFB_H - y), x, EFB_H - y - h, x + w, EFB_H - y, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rfbo);
    } else glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);   /* (resolved) */
    static u32* tmp; static int tn; if (w * h > tn) { tn = w * h; tmp = realloc(tmp, (size_t)tn * 4); }
    glReadPixels(x, EFB_H - y - h, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int r = 0; r < h; r++) memcpy(dst + (size_t)(y + r) * EFB_W + x, tmp + (size_t)(h - 1 - r) * w, (size_t)w * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    g_pf_read += host_tb() - t0;
}
/* read EFB depth (24-bit) into a buffer laid out EFB_W wide */
void gl_read_efb_depth(u32* dst, int x, int y, int w, int h) {
    gl_flush(); gl_state_dirty(); efb_resolve(); glDisable(GL_SCISSOR_TEST);
    if (SW != EFB_W || SH != EFB_H) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_rfbo);
        glBlitFramebuffer(XS(x), YS(EFB_H - y - h), XS(x + w), YS(EFB_H - y), x, EFB_H - y - h, x + w, EFB_H - y, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rfbo);
    } else glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);   /* (resolved) */
    static u32* tmp; static int tn; if (w * h > tn) { tn = w * h; tmp = realloc(tmp, (size_t)tn * 4); }
    glReadPixels(x, EFB_H - y - h, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, tmp);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    for (int r = 0; r < h; r++) { const u32* s = tmp + (size_t)(h - 1 - r) * w; u32* d = dst + (size_t)(y + r) * EFB_W + x; for (int i = 0; i < w; i++) d[i] = s[i] >> 8; }
}
void gl_clear(int x, int y, int w, int h, u32 rgba, u32 z24, int color, int alpha, int depth) {
    gl_flush(); gl_state_dirty(); glDisable(GL_COLOR_LOGIC_OP); glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    glEnable(GL_SCISSOR_TEST); glScissor(XS(x), YS(EFB_H - y - h), XS(x + w) - XS(x), YS(h));
    glColorMask(color, color, color, alpha); glDepthMask(depth);
    glClearColor((rgba & 255) / 255.f, ((rgba >> 8) & 255) / 255.f, ((rgba >> 16) & 255) / 255.f, (rgba >> 24) / 255.f);
    glClearDepth(z24 / 16777215.0);
    glClear((color || alpha ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    glColorMask(1, 1, 1, 1); g_ms_dirty = 1;
}

static GLuint g_sfbo, g_stex;
/* ---------------- UI overlay (options screen): screen-space quads, solid / RGBA image / font atlas ---------------- */
static const char* UVS = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 t; layout(location=2) in vec4 c;\nuniform vec2 u_ss; out vec2 v_t; out vec4 v_c;\n"
    "void main(){ v_t=t; v_c=c; gl_Position=vec4(p.x/u_ss.x*2.0-1.0, 1.0-p.y/u_ss.y*2.0, 0.0, 1.0); gl_ClipDistance[0]=1.0; gl_ClipDistance[1]=1.0; gl_ClipDistance[2]=1.0; gl_ClipDistance[3]=1.0; }\n";
static const char* UFS = "#version 330 core\nin vec2 v_t; in vec4 v_c; out vec4 o; uniform sampler2D u_t; uniform int u_mode;\n"
    "void main(){ if(u_mode==0) o=v_c; else if(u_mode==1) o=texture(u_t,v_t)*v_c; else o=vec4(v_c.rgb, v_c.a*texture(u_t,v_t).r); }\n";
static GLuint g_uprog, g_uvao, g_uvbo, g_ufont, g_utitle; static GLint uu_ss, uu_mode, uu_t;
static float g_uv[6 * 8 * 512]; static int g_un, g_umode = -1; static float g_uw, g_uh;
extern const int g_uifont_cw, g_uifont_ch, g_uifont_cols, g_uifont_w, g_uifont_h; extern const unsigned char g_uifont_adv[95], g_uifont[];
extern const unsigned char g_options_title[];
static void ui_flush(void) {
    if (!g_un) return;
    glBindBuffer(GL_ARRAY_BUFFER, g_uvbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_un * 8 * 4, g_uv, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, g_un); g_un = 0;
}
static void ui_mode(int m, GLuint tex) {
    if (m != g_umode || m) { ui_flush(); g_umode = m; glUniform1i(uu_mode, m); if (m) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); } }
}
static void ui_vtx(float x, float y, float u, float v, const float* c) { if (g_un >= 6 * 512) ui_flush(); float* q = g_uv + g_un * 8; q[0] = x; q[1] = y; q[2] = u; q[3] = v; q[4] = c[0]; q[5] = c[1]; q[6] = c[2]; q[7] = c[3]; g_un++; }
static void ui_quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1, const float* ct, const float* cb) {
    ui_vtx(x, y, u0, v0, ct); ui_vtx(x + w, y, u1, v0, ct); ui_vtx(x, y + h, u0, v1, cb);
    ui_vtx(x + w, y, u1, v0, ct); ui_vtx(x + w, y + h, u1, v1, cb); ui_vtx(x, y + h, u0, v1, cb);
}
void ui_begin(int w, int h) {
    if (!g_uprog) {
        GLuint vs = compile(GL_VERTEX_SHADER, UVS), fs = compile(GL_FRAGMENT_SHADER, UFS);
        g_uprog = glCreateProgram(); glAttachShader(g_uprog, vs); glAttachShader(g_uprog, fs); glLinkProgram(g_uprog);
        uu_ss = glGetUniformLocation(g_uprog, "u_ss"); uu_mode = glGetUniformLocation(g_uprog, "u_mode"); uu_t = glGetUniformLocation(g_uprog, "u_t");
        glGenVertexArrays(1, &g_uvao); glBindVertexArray(g_uvao); glGenBuffers(1, &g_uvbo); glBindBuffer(GL_ARRAY_BUFFER, g_uvbo);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 32, (void*)0);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, 32, (void*)8);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, 0, 32, (void*)16);
        glGenTextures(1, &g_ufont); glBindTexture(GL_TEXTURE_2D, g_ufont);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, g_uifont_w, g_uifont_h, 0, GL_RED, GL_UNSIGNED_BYTE, g_uifont);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glGenTextures(1, &g_utitle); glBindTexture(GL_TEXTURE_2D, g_utitle);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 312, 80, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_options_title);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    gl_flush(); gl_state_dirty();
    glViewport(0, 0, w, h); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_COLOR_LOGIC_OP); glColorMask(1, 1, 1, 1);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_uprog); glBindVertexArray(g_uvao); { float ss[2] = {(float)w, (float)h}; glUniform2fv(uu_ss, 1, ss); } glUniform1i(uu_t, 0);
    g_umode = -1; g_un = 0; g_uw = (float)w; g_uh = (float)h;
}
void ui_image(float x, float y, float w, float h, unsigned tex, float a) { float c[4] = {1, 1, 1, a}; if (!tex) return; ui_mode(1, (GLuint)tex); ui_quad(x, y, w, h, 0, 0, 1, 1, c, c); }
/* textures for the options screen (save-state thumbnails): RGBA8, rows top-down */
unsigned gl_ui_texture(const u8* rgba, int w, int h) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_state_dirty(); return t;
}
void gl_ui_texture_free(unsigned t) { GLuint g = t; if (g) glDeleteTextures(1, &g); }
/* a small copy of the last finished frame (save-state thumbnails), refreshed at every XFB copy */
static GLuint g_thfbo, g_thtex; static int g_thw, g_thh;
void gl_thumb_capture(void) {
    int tw = g_ws ? 256 : 192, th = 144;
    gl_flush(); gl_state_dirty(); efb_resolve();
    if (!g_thfbo || g_thw != tw) {
        if (!g_thfbo) { glGenFramebuffers(1, &g_thfbo); glGenTextures(1, &g_thtex); }
        glBindTexture(GL_TEXTURE_2D, g_thtex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_thfbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_thtex, 0);
        g_thw = tw; g_thh = th;
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_thfbo);
    glBlitFramebuffer(0, YS(EFB_H - 480), SW, SH, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
}
/* rows top-down, alpha opaque; returns 0 when there is no picture yet */
int gl_thumb_read(u8* out, int cap, int* w, int* h) {
    if (!g_thfbo || g_thw * g_thh * 4 > cap) return 0;
    u8* tmp = malloc((size_t)g_thw * g_thh * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_thfbo); glReadPixels(0, 0, g_thw, g_thh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int r = 0; r < g_thh; r++) memcpy(out + (size_t)r * g_thw * 4, tmp + (size_t)(g_thh - 1 - r) * g_thw * 4, (size_t)g_thw * 4);
    for (int i = 0; i < g_thw * g_thh; i++) out[4 * i + 3] = 255;
    free(tmp); glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO); gl_state_dirty();
    *w = g_thw; *h = g_thh; return 1;
}
void ui_rect(float x, float y, float w, float h, const float* ct, const float* cb) { ui_mode(0, 0); ui_quad(x, y, w, h, 0, 0, 0, 0, ct, cb); }
void ui_title(float x, float y, float w, float h, float a) { float c[4] = {1, 1, 1, a}; ui_mode(1, g_utitle); ui_quad(x, y, w, h, 0, 0, 1, 1, c, c); }
float ui_text_width(const char* s, float size) { float k = size / 26.f, x = 0; for (; *s; s++) { int ch = (unsigned char)*s - 32; if (ch >= 0 && ch < 95) x += g_uifont_adv[ch] * k; } return x; }
void ui_text(float x, float y, float size, const char* s, const float* c) {
    ui_mode(2, g_ufont); float k = size / 26.f;
    for (; *s; s++) {
        int ch = (unsigned char)*s - 32; if (ch < 0 || ch >= 95) continue;
        float u0 = (float)((ch % g_uifont_cols) * g_uifont_cw) / g_uifont_w, v0 = (float)((ch / g_uifont_cols) * g_uifont_ch) / g_uifont_h;
        ui_quad(x, y, g_uifont_cw * k, g_uifont_ch * k, u0, v0, u0 + (float)g_uifont_cw / g_uifont_w, v0 + (float)g_uifont_ch / g_uifont_h, c, c);
        x += g_uifont_adv[ch] * k;
    }
}
void ui_end(void) { ui_flush(); glDisable(GL_BLEND); glBindVertexArray(g_vao); glUseProgram(g_prog); glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO); glViewport(0, 0, SW, SH); }
/* the options screen draws itself into a 4:3 rectangle of the current framebuffer */
WEAK void options_draw(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }
WEAK int options_is_open(void) { return 0; }
/* headless runs: draw the overlay into the EFB so frame dumps show it */
void gl_overlay_into_efb(void) {
    if (!options_is_open()) return;
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    ui_begin(SW, SH); { float h = 480.f * g_scale, w = h * 4 / 3; options_draw((SW - w) / 2, 0, w, h); } ui_end();
}
/* live resolution change: recreate the EFB at the new scale */
static void gl_resize_efb(int ow, int oh);
void gl_set_scale(int s) {
    if (s < 1) s = 1; if (s > 8) s = 8; if (s == g_scale) return;
    gl_flush(); gl_state_dirty(); int ow = SW, oh = SH; g_scale = s; gl_resize_efb(ow, oh);
}
void gl_set_widescreen(int on) {
    on = on ? 1 : 0; if (on == g_ws) return;
    gl_flush(); gl_state_dirty(); int ow = SW, oh = SH; g_ws = on; gl_resize_efb(ow, oh);
    rt_log("GL: widescreen %s\n", on ? "on" : "off");
}
static void ms_alloc(void) {   /* (re)create the multisampled EFB for the current size and sample count */
    if (g_msaa <= 1) return;
    if (!g_msfbo) { glGenFramebuffers(1, &g_msfbo); glGenRenderbuffers(1, &g_msc); glGenRenderbuffers(1, &g_msd); }
    glBindRenderbuffer(GL_RENDERBUFFER, g_msc); glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaa, GL_RGBA8, SW, SH);
    glBindRenderbuffer(GL_RENDERBUFFER, g_msd); glRenderbufferStorageMultisample(GL_RENDERBUFFER, g_msaa, GL_DEPTH_COMPONENT24, SW, SH);
    glBindFramebuffer(GL_FRAMEBUFFER, g_msfbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_msc);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_msd);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { rt_log("GL: %dx MSAA unavailable\n", g_msaa); g_msaa = 1; glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); return; }
    /* start from the current picture */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_msfbo);
    glBlitFramebuffer(0, 0, SW, SH, 0, 0, SW, SH, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_msfbo);
}
static void efb_resolve(void) {
    if (g_msaa <= 1 || !g_ms_dirty) return;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_msfbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    glBlitFramebuffer(0, 0, SW, SH, 0, 0, SW, SH, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    g_ms_dirty = 0;
}
/* options: 1 (off), 2, 4 or 8 samples */
void gl_set_msaa(int samples) {
    if (samples < 1) samples = 1;
    { GLint mx = 1; glGetIntegerv(GL_MAX_SAMPLES, &mx); if (samples > mx) samples = mx; }
    if (samples == g_msaa) return;
    gl_flush(); gl_state_dirty(); efb_resolve();
    g_msaa = samples; ms_alloc(); g_ms_dirty = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
    rt_log("GL: MSAA %dx\n", g_msaa);
}
static void gl_resize_efb(int ow, int oh) {
    efb_resolve();
    glBindTexture(GL_TEXTURE_2D, g_fbtex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, g_fbdepth); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SW, SH);
    ms_alloc();
    if (g_stex) {   /* keep the paused frame visible under the options screen: rescale it into a new texture */
        GLuint nt, nf; glGenTextures(1, &nt); glBindTexture(GL_TEXTURE_2D, nt); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glGenFramebuffers(1, &nf); glBindFramebuffer(GL_FRAMEBUFFER, nf); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, nt, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_sfbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, nf);
        glBlitFramebuffer(0, 0, ow, oh, 0, 0, SW, SH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glDeleteFramebuffers(1, &g_sfbo); glDeleteTextures(1, &g_stex); g_sfbo = nf; g_stex = nt;
    }
    glUseProgram(g_prog); glUniform1f(u_scale, (float)SW / EFB_W);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO); glViewport(0, 0, SW, SH);
    rt_log("GL: internal resolution %dx%d\n", SW, 480 * g_scale);
}

/* frame interpolation: the real frame is kept in a texture while the in-between frame is shown */
static int g_show_saved;
void gl_save_real(void) {
    gl_flush(); gl_state_dirty(); efb_resolve();
    if (!g_sfbo) {
        glGenFramebuffers(1, &g_sfbo); glGenTextures(1, &g_stex); glBindTexture(GL_TEXTURE_2D, g_stex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SW, SH, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_sfbo); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_stex, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sfbo);
    glBlitFramebuffer(0, 0, SW, SH, 0, 0, SW, SH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
}

/* ---------------- output: scaling and post-processing (options) ---------------- */
static const char* PVS = "#version 330 core\nuniform vec4 u_dst; out vec2 v_t;\nvoid main(){ vec2 p=vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2)); v_t=p; gl_Position=vec4(u_dst.xy+p*u_dst.zw,0.0,1.0); gl_ClipDistance[0]=1.0; gl_ClipDistance[1]=1.0; gl_ClipDistance[2]=1.0; gl_ClipDistance[3]=1.0; }\n";
static const char* PFS =
"#version 330 core\n"
"uniform sampler2D u_src; uniform vec4 u_srect; uniform vec2 u_texel; uniform vec2 u_out; uniform int u_mode; in vec2 v_t; out vec4 o;\n"
"vec3 S(vec2 t){ return texture(u_src, u_srect.xy + clamp(t,0.0,1.0)*u_srect.zw).rgb; }\n"
"float L(vec3 c){ return dot(c, vec3(0.299,0.587,0.114)); }\n"
"void main(){\n"
"  vec2 t=v_t; vec2 px=u_texel/u_srect.zw;\n"
"  if(u_mode==1){\n"   /* FXAA (simplified FXAA 3.11 console) */
"    vec3 rgbM=S(t); float lM=L(rgbM);\n"
"    float lNW=L(S(t+vec2(-1,-1)*px)), lNE=L(S(t+vec2(1,-1)*px)), lSW=L(S(t+vec2(-1,1)*px)), lSE=L(S(t+vec2(1,1)*px));\n"
"    float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE))), lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));\n"
"    vec2 dir=vec2(-((lNW+lNE)-(lSW+lSE)), ((lNW+lSW)-(lNE+lSE)));\n"
"    float red=max((lNW+lNE+lSW+lSE)*0.03125, 1.0/128.0); float rcp=1.0/(min(abs(dir.x),abs(dir.y))+red);\n"
"    dir=clamp(dir*rcp, vec2(-8.0), vec2(8.0))*px;\n"
"    vec3 a=0.5*(S(t+dir*(1.0/3.0-0.5))+S(t+dir*(2.0/3.0-0.5)));\n"
"    vec3 b=a*0.5+0.25*(S(t+dir*-0.5)+S(t+dir*0.5)); float lB=L(b);\n"
"    o=vec4((lB<lMin||lB>lMax)?a:b,1.0); return; }\n"
"  if(u_mode==2){\n"   /* sharpen: contrast-adaptive, after AMD CAS */
"    vec3 c=S(t), n=S(t+vec2(0,-1)*px), s=S(t+vec2(0,1)*px), w=S(t+vec2(-1,0)*px), e=S(t+vec2(1,0)*px);\n"
"    vec3 mn=min(c,min(min(n,s),min(w,e))), mx=max(c,max(max(n,s),max(w,e)));\n"
"    vec3 amp=sqrt(clamp(min(mn,1.0-mx)/max(mx,1e-4),0.0,1.0)); vec3 wgt=-amp/mix(8.0,5.0,0.6);\n"
"    o=vec4(clamp((c+(n+s+w+e)*wgt)/(1.0+4.0*wgt),0.0,1.0),1.0); return; }\n"
"  if(u_mode==3){\n"   /* CRT: slight curvature, scanlines at the game's 480 lines, aperture mask, vignette */
"    vec2 q=t*2.0-1.0; q*=1.0+vec2(q.y*q.y,q.x*q.x)*vec2(0.035,0.045); vec2 tc=q*0.5+0.5;\n"
"    if(tc.x<0.0||tc.x>1.0||tc.y<0.0||tc.y>1.0){ o=vec4(0,0,0,1); return; }\n"
"    vec3 c=S(tc); float line=tc.y*240.0; float sl=0.62+0.38*pow(abs(sin(line*3.14159265)),1.6);\n"
"    float m=mod(gl_FragCoord.x,3.0); vec3 mask=m<1.0?vec3(1.08,0.94,0.94):m<2.0?vec3(0.94,1.08,0.94):vec3(0.94,0.94,1.08);\n"
"    float v=clamp(1.0-0.20*dot(q,q),0.0,1.0); o=vec4(pow(c,vec3(0.95))*sl*mask*v*1.18,1.0); return; }\n"
"  o=vec4(S(t),1.0); }\n";
static GLuint g_pprog, g_pvao; static GLint up_dst, up_srect, up_texel, up_out, up_mode, up_src;
/* draws the game picture into (dx,dy,dw,dh) of the default framebuffer with the chosen filter; 0 = not handled */
static GLuint g_post_target;   /* 0 = the window (tests draw into their own framebuffer) */
static int post_draw(GLuint src_tex, int sw, int sh, int y0, int dx, int dy, int dw, int dh, int w, int h, int mode) {
    if (!g_pprog) {
        GLuint vs = compile(GL_VERTEX_SHADER, PVS), fs = compile(GL_FRAGMENT_SHADER, PFS);
        g_pprog = glCreateProgram(); glAttachShader(g_pprog, vs); glAttachShader(g_pprog, fs); glLinkProgram(g_pprog);
        GLint ok; glGetProgramiv(g_pprog, GL_LINK_STATUS, &ok); if (!ok) { rt_log("GL: post shader link failed\n"); g_pprog = 0xFFFFFFFFu; }
        if (ok) { up_dst = glGetUniformLocation(g_pprog, "u_dst"); up_srect = glGetUniformLocation(g_pprog, "u_srect"); up_texel = glGetUniformLocation(g_pprog, "u_texel");
                  up_out = glGetUniformLocation(g_pprog, "u_out"); up_mode = glGetUniformLocation(g_pprog, "u_mode"); up_src = glGetUniformLocation(g_pprog, "u_src"); glGenVertexArrays(1, &g_pvao); }
    }
    if (g_pprog == 0xFFFFFFFFu) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_post_target); glViewport(0, 0, w, h); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_COLOR_LOGIC_OP); glColorMask(1, 1, 1, 1);
    glUseProgram(g_pprog); glBindVertexArray(g_pvao);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(up_src, 0); glUniform1i(up_mode, mode);
    /* the picture is rows y0..sh of the texture, stored bottom-up: map v_t (0 = top) onto them */
    { float r[4] = {0.f, 1.f, 1.f, -(float)(sh - y0) / sh}; glUniform4fv(up_srect, 1, r); }
    { float tx[2] = {1.f / sw, 1.f / sh}; glUniform2fv(up_texel, 1, tx); }
    { float o2[2] = {(float)dw, (float)dh}; glUniform2fv(up_out, 1, o2); }
    { float d[4] = {2.f * dx / w - 1.f, 1.f - 2.f * dy / h, 2.f * dw / w, -2.f * dh / h}; glUniform4fv(up_dst, 1, d); }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(g_prog); glBindVertexArray(g_vao); gl_state_dirty();
    return 1;
}
/* test: SOA_POSTTEST=frame,mode renders the output filter at 1280x960 into postN.png */
void gl_post_test(int mode, const char* path) {
    gl_flush(); efb_resolve();
    int w = 1280, h = 960; GLuint f, t; glGenFramebuffers(1, &f); glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0); glBindFramebuffer(GL_FRAMEBUFFER, f); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    g_post_target = f; post_draw(g_fbtex, SW, SH, YS(EFB_H - 480), 0, 0, w, h, w, h, mode); g_post_target = 0;
    u8* px = malloc((size_t)w * h * 4); u8* fl = malloc((size_t)w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, f); glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int r = 0; r < h; r++) memcpy(fl + (size_t)r * w * 4, px + (size_t)(h - 1 - r) * w * 4, (size_t)w * 4);
    extern int stbi_write_png(char const*, int, int, int, const void*, int); stbi_write_png(path, w, h, 4, fl, w * 4);
    free(px); free(fl); glDeleteFramebuffers(1, &f); glDeleteTextures(1, &t); glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO); gl_state_dirty();
}
void gl_show_saved(int on) { g_show_saved = on; }
/* F12: screenshot of the game picture at the internal resolution (no overlays) */
void gl_take_screenshot(void) {
    extern int g_shot_req; extern void screenshot_write(u8* rgba, int w, int h);
    if (!g_shot_req) return;
    g_shot_req = 0;
    gl_flush(); efb_resolve();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_show_saved && g_sfbo ? g_sfbo : g_fbo);
    int y0 = YS(EFB_H - 480), sw = SW, sh = SH - y0;
    u8* px = malloc((size_t)sw * sh * 4); u8* fl = malloc((size_t)sw * sh * 4);
    if (px && fl) {
        glReadPixels(0, y0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int r = 0; r < sh; r++) memcpy(fl + (size_t)r * sw * 4, px + (size_t)(sh - 1 - r) * sw * 4, (size_t)sw * 4);
        free(px); screenshot_write(fl, sw, sh);
    } else { free(px); free(fl); }
    gl_state_dirty();
}
void gl_blit_to_screen(int w, int h) {
    gl_flush(); gl_state_dirty(); efb_resolve();
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h); glClearColor(0, 0, 0, 1); glColorMask(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    int an = g_ws ? 16 : 4, ad = g_ws ? 9 : 3;
    int dw = w, dh = w * ad / an; if (dh > h) { dh = h; dw = h * an / ad; }
    static int pmode = -1, ints = -1; static u64 chk; if (pmode < 0 || g_gl_frame - chk > 60) { pmode = settings_get("post_filter", 0); ints = settings_get("integer_scaling", 0); chk = g_gl_frame; }
    if (ints) {   /* whole multiples of the game's 480 lines (width follows the aspect) */
        int k = h / 480; if (k < 1) k = 1; int iw = 640 * k * an / 4 / (ad / 3); if (g_ws) iw = 640 * k * 4 / 3;
        while (k > 1 && (iw > w || 480 * k > h)) { k--; iw = g_ws ? 640 * k * 4 / 3 : 640 * k; }
        if (iw <= w && 480 * k <= h) { dw = iw; dh = 480 * k; }
    }
    int dx = (w - dw) / 2, dy = (h - dh) / 2;
    if (!pmode || !post_draw(g_show_saved && g_sfbo ? g_stex : g_fbtex, SW, SH, YS(EFB_H - 480), dx, h - dy - dh, dw, dh, w, h, pmode)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_show_saved && g_sfbo ? g_sfbo : g_fbo);
        glBlitFramebuffer(0, YS(EFB_H - 480), SW, SH, dx, dy, dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, ints ? GL_NEAREST : GL_LINEAR);
    }
    gl_take_screenshot();
    if (options_is_open()) { glBindFramebuffer(GL_FRAMEBUFFER, 0); ui_begin(w, h); float ow = dh * 4.f / 3.f; options_draw(dx + (dw - ow) / 2, (float)(h - dy - dh), ow, (float)dh); ui_end(); }
    { extern int g_perf_overlay; extern char g_perf_text[3][200];
      if (g_perf_overlay && g_perf_text[0][0]) {   /* F3: performance readout, top left */
          glBindFramebuffer(GL_FRAMEBUFFER, 0); ui_begin(w, h);
          float sz = h / 48.f; if (sz < 12) sz = 12; float pad = sz * 0.5f, lh = sz * 1.25f;
          float tw = 0; for (int i = 0; i < 3; i++) { float t = ui_text_width(g_perf_text[i], sz); if (t > tw) tw = t; }
          float bg[4] = {0, 0, 0, 0.6f}; ui_rect(pad, pad, tw + 2 * pad, 3 * lh + 2 * pad, bg, bg);
          float white[4] = {1, 1, 1, 1}, gold[4] = {1.0f, 0.85f, 0.4f, 1};
          for (int i = 0; i < 3; i++) ui_text(2 * pad, 2 * pad + i * lh, sz, g_perf_text[i], i == 0 ? gold : white);
          ui_end();
      } }
    {   /* on-screen messages, bottom left */
        extern int osd_get(int k, const char** text, float* alpha);
        const char* t; float al;
        if (osd_get(0, &t, &al)) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0); ui_begin(w, h);
            float sz = h / 40.f; if (sz < 14) sz = 14; float pad = sz * 0.6f, lh = sz * 1.5f;
            int n = 0; while (n < 4 && osd_get(n, &t, &al)) n++;
            for (int k = 0; k < n; k++) {
                osd_get(k, &t, &al);
                float y = h - pad - (n - k) * lh, tw = ui_text_width(t, sz);
                float bg[4] = {0.02f, 0.06f, 0.22f, 0.75f * al}, rim[4] = {0.62f, 0.78f, 1.f, 0.8f * al}, fg[4] = {1, 1, 1, al};
                ui_rect(pad - 2, y - 2, tw + 2 * pad + 4, lh - sz * 0.2f + 4, rim, rim);
                ui_rect(pad, y, tw + 2 * pad, lh - sz * 0.2f, bg, bg);
                ui_text(2 * pad, y + sz * 0.1f, sz, t, fg);
            }
            ui_end();
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, DRAWFBO);
}
