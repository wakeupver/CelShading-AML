// scene.h - adegan 3D sintetis (GLES2) untuk menguji renderer cel shading.
//
// BUKAN screenshot GTA SA: ini "game palsu" berisi langit, jalan beraspal, rumput, gedung
// berjendela, mobil, pohon, dan HUD tiruan, dengan tekstur prosedural di fragment shader
// (derau + pola) supaya perilaku garis tepi pada permukaan bertekstur juga terlihat.
#pragma once

#include <GLES2/gl2.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <vector>

namespace scene {

// ----------------------------------------------------------------------------- matematika
struct Mat4 { float m[16]; };

inline Mat4 Identity() { Mat4 r; memset(r.m, 0, sizeof(r.m)); r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f; return r; }
inline Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
        {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
inline Mat4 Translate(float x, float y, float z) { Mat4 r = Identity(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
inline Mat4 Scale(float x, float y, float z)     { Mat4 r = Identity(); r.m[0] = x; r.m[5] = y; r.m[10] = z; return r; }
inline Mat4 RotY(float a)
{
    Mat4 r = Identity();
    float c = cosf(a), s = sinf(a);
    r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c;
    return r;
}
inline Mat4 Perspective(float fovy, float aspect, float n, float f)
{
    Mat4 r; memset(r.m, 0, sizeof(r.m));
    float t = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = t / aspect; r.m[5] = t;
    r.m[10] = (f + n) / (n - f); r.m[11] = -1.0f;
    r.m[14] = 2.0f * f * n / (n - f);
    return r;
}
inline Mat4 LookAt(const float e[3], const float c[3], const float up[3])
{
    float f[3] = { c[0] - e[0], c[1] - e[1], c[2] - e[2] };
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; ++i) f[i] /= fl;
    float s[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
    float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (int i = 0; i < 3; ++i) s[i] /= sl;
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    Mat4 r = Identity();
    r.m[0] = s[0]; r.m[4] = s[1]; r.m[8]  = s[2];
    r.m[1] = u[0]; r.m[5] = u[1]; r.m[9]  = u[2];
    r.m[2] = -f[0]; r.m[6] = -f[1]; r.m[10] = -f[2];
    r.m[12] = -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]);
    r.m[13] = -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]);
    r.m[14] =  (f[0] * e[0] + f[1] * e[1] + f[2] * e[2]);
    return r;
}

// ----------------------------------------------------------------------------- shader
static const char* kObjVS =
    "attribute vec3 aPos;\n"
    "attribute vec3 aNrm;\n"
    "uniform mat4 uVP;\n"
    "uniform mat4 uModel;\n"
    "varying vec3 vN;\n"
    "varying vec3 vW;\n"
    "void main() {\n"
    "  vec4 w = uModel * vec4(aPos, 1.0);\n"
    "  vW = w.xyz;\n"
    "  vN = vec3(uModel * vec4(aNrm, 0.0));\n"
    "  gl_Position = uVP * w;\n"
    "}\n";

static const char* kObjFS =
    "precision mediump float;\n"
    "varying vec3 vN;\n"
    "varying vec3 vW;\n"
    "uniform vec3 uColor;\n"
    "uniform float uPattern;\n"
    "uniform vec3 uLight;\n"
    "uniform vec3 uCam;\n"
    "float hash(vec2 p) {\n"                         // tanpa sin(): sin() argumen besar bisa NaN di beberapa driver
    "  vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
    "  p3 += dot(p3, p3.yzx + 33.33);\n"
    "  return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "float noise(vec2 p) {\n"
    "  vec2 i = floor(p); vec2 f = fract(p); f = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);\n"
    "}\n"
    "void main() {\n"
    "  vec3 n = normalize(vN);\n"
    "  vec3 L = normalize(uLight);\n"
    "  float d = max(dot(n, L), 0.0);\n"
    "  vec2 uv = abs(n.y) > 0.5 ? vW.xz : (abs(n.x) > 0.5 ? vW.zy : vW.xy);\n"
    "  vec3 base = uColor;\n"
    "  if (uPattern < 0.5) {\n"
    "    base *= 0.90 + 0.20 * noise(uv * 7.0);\n"
    "  } else if (uPattern < 1.5) {\n"                         // gedung: jendela
    "    vec2 cell = uv * vec2(0.55, 0.45);\n"
    "    vec2 g = fract(cell);\n"
    "    float win = step(0.18, g.x) * step(g.x, 0.82) * step(0.22, g.y) * step(g.y, 0.78);\n"
    "    vec3 glass = vec3(0.42, 0.58, 0.78) * (0.65 + 0.55 * hash(floor(cell)));\n"
    "    base = mix(base * (0.92 + 0.16 * noise(uv * 11.0)), glass, win * 0.85);\n"
    "  } else if (uPattern < 2.5) {\n"                         // aspal + marka jalan
    "    base *= 0.78 + 0.34 * noise(uv * 24.0);\n"
    "    float dash = step(0.5, fract(vW.z * 0.12)) * step(abs(vW.x), 0.12);\n"
    "    base = mix(base, vec3(0.95, 0.93, 0.80), dash * 0.9);\n"
    "    float edge = step(3.6, abs(vW.x)) * step(abs(vW.x), 3.8);\n"
    "    base = mix(base, vec3(0.9), edge * 0.8);\n"
    "  } else {\n"                                              // rumput
    "    base *= 0.62 + 0.45 * noise(uv * 5.0) + 0.25 * noise(uv * 26.0);\n"
    "  }\n"
    "  vec3 V = normalize(uCam - vW);\n"
    "  vec3 H = normalize(L + V);\n"
    "  float spec = pow(max(dot(n, H), 0.0), 40.0) * (uPattern > 2.5 ? 0.0 : 0.35);\n"
    "  vec3 col = base * (0.30 + 0.85 * d) + vec3(spec);\n"
    "  gl_FragColor = vec4(col, 1.0);\n"
    "}\n";

static const char* kSkyVS =
    "attribute vec2 aPos;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.999, 1.0); }\n";

static const char* kSkyFS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  vec3 top = vec3(0.20, 0.42, 0.80);\n"
    "  vec3 hor = vec3(0.86, 0.90, 0.95);\n"
    "  float t = smoothstep(0.30, 0.95, vUV.y);\n"
    "  vec3 c = mix(hor, top, t);\n"
    "  float sun = smoothstep(0.045, 0.030, length((vUV - vec2(0.78, 0.66)) * vec2(1.78, 1.0)));\n"
    "  c = mix(c, vec3(1.0, 0.97, 0.80), sun);\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

// ----------------------------------------------------------------------------- mesh
struct Mesh { GLuint vbo, ibo; int count; };

inline Mesh MakeCube()
{
    static const float n[6][3] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
    std::vector<float> v; std::vector<unsigned short> idx;
    for (int f = 0; f < 6; ++f)
    {
        float a[3], b[3];   // dua sumbu bidang sisi
        const float* nn = n[f];
        int axis = nn[0] != 0 ? 0 : (nn[1] != 0 ? 1 : 2);
        int u = (axis + 1) % 3, w = (axis + 2) % 3;
        memset(a, 0, sizeof(a)); memset(b, 0, sizeof(b)); a[u] = 1; b[w] = 1;
        unsigned short base = (unsigned short)(v.size() / 6);
        const int sx[4] = { -1, 1, 1, -1 }, sy[4] = { -1, -1, 1, 1 };
        for (int i = 0; i < 4; ++i)
        {
            float p[3];
            for (int k = 0; k < 3; ++k) p[k] = nn[k] * 0.5f + a[k] * sx[i] * 0.5f + b[k] * sy[i] * 0.5f;
            v.insert(v.end(), { p[0], p[1], p[2], nn[0], nn[1], nn[2] });
        }
        // urutan CCW dilihat dari luar, tergantung orientasi sumbu
        float cx = a[1] * b[2] - a[2] * b[1], cy = a[2] * b[0] - a[0] * b[2], cz = a[0] * b[1] - a[1] * b[0];
        bool ccw = (cx * nn[0] + cy * nn[1] + cz * nn[2]) > 0;
        unsigned short tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int i = 0; i < 6; ++i) idx.push_back((unsigned short)(base + (ccw ? tri[i] : tri[5 - i])));
    }
    Mesh m = { 0, 0, (int)idx.size() };
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned short), idx.data(), GL_STATIC_DRAW);
    return m;
}

inline Mesh MakeSphere(int lat = 18, int lon = 28)
{
    std::vector<float> v; std::vector<unsigned short> idx;
    for (int i = 0; i <= lat; ++i)
    {
        float th = 3.14159265f * i / lat;
        for (int j = 0; j <= lon; ++j)
        {
            float ph = 2.0f * 3.14159265f * j / lon;
            float x = sinf(th) * cosf(ph), y = cosf(th), z = sinf(th) * sinf(ph);
            v.insert(v.end(), { x * 0.5f, y * 0.5f, z * 0.5f, x, y, z });
        }
    }
    for (int i = 0; i < lat; ++i)
        for (int j = 0; j < lon; ++j)
        {
            unsigned short a = (unsigned short)(i * (lon + 1) + j), b = (unsigned short)(a + lon + 1);
            idx.insert(idx.end(), { a, (unsigned short)(a + 1), b, (unsigned short)(a + 1), (unsigned short)(b + 1), b });
        }
    Mesh m = { 0, 0, (int)idx.size() };
    glGenBuffers(1, &m.vbo); glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &m.ibo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned short), idx.data(), GL_STATIC_DRAW);
    return m;
}

inline GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof(log), nullptr, log); fprintf(stderr, "scene shader: %s\n", log); }
    return s;
}
inline GLuint Link(const char* vs, const char* fs)
{
    GLuint p = glCreateProgram();
    GLuint a = Compile(GL_VERTEX_SHADER, vs), b = Compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, a); glAttachShader(p, b);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aNrm");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(p, sizeof(log), nullptr, log); fprintf(stderr, "scene link: %s\n", log); }
    return p;
}

// ----------------------------------------------------------------------------- adegan
struct Object { int mesh; float px, py, pz, sx, sy, sz, rot; float r, g, b; float pattern; };

class Scene
{
public:
    bool Init()
    {
        objProg = Link(kObjVS, kObjFS);
        skyProg = Link(kSkyVS, kSkyFS);
        cube = MakeCube();
        sphere = MakeSphere();
        static const float tri[6] = { -1, -1, 3, -1, -1, 3 };
        glGenBuffers(1, &skyVbo); glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STATIC_DRAW);

        // mesh: 0 = kubus, 1 = bola.   pola: 0 polos, 1 gedung, 2 aspal, 3 rumput
        objs = {
            { 0,  0.0f, -0.05f,  -20.0f, 120.0f, 0.1f, 140.0f, 0,  0.30f, 0.52f, 0.20f, 3 },   // rumput
            { 0,  0.0f,  0.00f,  -20.0f,   7.6f, 0.12f, 140.0f, 0, 0.30f, 0.30f, 0.33f, 2 },   // jalan
            { 0, -11.0f, 3.50f,  -14.0f,   7.0f, 7.0f,   8.0f, 0,  0.78f, 0.62f, 0.45f, 1 },   // gedung
            { 0, -12.0f, 6.00f,  -28.0f,   8.0f, 12.0f,  9.0f, 0,  0.62f, 0.34f, 0.30f, 1 },
            { 0, -10.5f, 4.50f,  -46.0f,  9.0f, 9.0f,   10.0f, 0,  0.52f, 0.60f, 0.66f, 1 },
            { 0,  12.0f, 4.00f,  -16.0f,   8.0f, 8.0f,   9.0f, 0,  0.85f, 0.80f, 0.60f, 1 },
            { 0,  13.0f, 7.50f,  -34.0f,   9.0f, 15.0f, 10.0f, 0,  0.50f, 0.50f, 0.62f, 1 },
            { 0,  11.5f, 3.00f,  -52.0f,  8.0f, 6.0f,   9.0f, 0,  0.70f, 0.48f, 0.38f, 1 },
            { 0,  2.0f,  0.55f,   -6.0f,   2.0f, 0.7f,   4.2f, 0.10f, 0.78f, 0.10f, 0.10f, 0 },  // mobil (badan)
            { 0,  2.0f,  1.15f,   -6.2f,   1.7f, 0.6f,   2.2f, 0.10f, 0.70f, 0.75f, 0.85f, 0 }, // kabin
            { 0, -1.8f,  0.55f,  -15.0f,   2.0f, 0.7f,   4.2f, 0.0f,  0.15f, 0.30f, 0.75f, 0 }, // mobil biru
            { 0, -1.8f,  1.15f, -15.1f,   1.7f, 0.6f,   2.2f, 0.0f,  0.80f, 0.85f, 0.90f, 0 },
            { 1,  7.0f,  2.0f,   -9.0f,   3.0f, 3.0f,   3.0f, 0,    0.14f, 0.40f, 0.14f, 0 },  // pohon
            { 1,  8.5f,  1.6f,  -22.0f,   2.6f, 2.6f,   2.6f, 0,    0.18f, 0.46f, 0.18f, 0 },
            { 1, -7.0f,  1.7f,   -8.0f,   2.8f, 2.8f,   2.8f, 0,    0.16f, 0.42f, 0.16f, 0 },
            { 0,  7.0f,  0.6f,   -9.0f,   0.35f, 1.2f,  0.35f, 0,   0.35f, 0.24f, 0.14f, 0 },  // batang pohon
            { 0, -4.5f,  2.6f,  -12.0f,   0.18f, 5.2f,  0.18f, 0,   0.25f, 0.25f, 0.28f, 0 },  // tiang lampu
            { 1, -4.5f,  5.3f,  -12.0f,   0.6f, 0.6f,   0.6f, 0,    1.00f, 0.92f, 0.65f, 0 },
            { 1,  0.0f,  0.9f,   -3.0f,   1.8f, 1.8f,   1.8f, 0,    0.92f, 0.62f, 0.15f, 0 },  // bola oranye di depan
        };
        return objProg && skyProg && cube.vbo && sphere.vbo;
    }

    // Menggambar adegan 3D saja (tanpa HUD).
    void DrawWorld(int w, int h, unsigned mask = ~0u, bool sky = true)
    {
        glViewport(0, 0, w, h);
        glClearColor(0.5f, 0.6f, 0.7f, 1.0f);
        glClearDepthf(1.0f);
        glClearStencil(0);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        // langit
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
        if (sky)
        {
            glUseProgram(skyProg);
            glBindBuffer(GL_ARRAY_BUFFER, skyVbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        // objek
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
        glUseProgram(objProg);
        float eye[3] = { 0.8f, 3.2f, 7.5f }, ctr[3] = { 0.0f, 2.2f, -22.0f }, up[3] = { 0, 1, 0 };
        Mat4 vp = Mul(Perspective(1.05f, (float)w / (float)h, 0.5f, 200.0f), LookAt(eye, ctr, up));
        glUniformMatrix4fv(glGetUniformLocation(objProg, "uVP"), 1, GL_FALSE, vp.m);
        glUniform3f(glGetUniformLocation(objProg, "uLight"), 0.55f, 0.80f, 0.35f);
        glUniform3f(glGetUniformLocation(objProg, "uCam"), eye[0], eye[1], eye[2]);
        GLint locModel = glGetUniformLocation(objProg, "uModel");
        GLint locColor = glGetUniformLocation(objProg, "uColor");
        GLint locPat   = glGetUniformLocation(objProg, "uPattern");
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        unsigned bit = 0;
        for (const Object& o : objs)
        {
            if (!((mask >> bit++) & 1u)) continue;
            const Mesh& m = o.mesh == 0 ? cube : sphere;
            Mat4 model = Mul(Translate(o.px, o.py, o.pz), Mul(RotY(o.rot), Scale(o.sx, o.sy, o.sz)));
            glUniformMatrix4fv(locModel, 1, GL_FALSE, model.m);
            glUniform3f(locColor, o.r, o.g, o.b);
            glUniform1f(locPat, o.pattern);
            glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (const void*)0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (const void*)12);
            glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_SHORT, (const void*)0);
        }
    }

    // HUD tiruan lewat scissor+clear (radar, "teks" uang, bar health) - meniru UI 2D game.
    void DrawHud(int w, int h)
    {
        auto rect = [&](int x, int y, int rw, int rh, float r, float g, float b)
        {
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, rw, rh);
            glClearColor(r, g, b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        };
        const int u = h / 36;                          // satuan relatif tinggi layar
        rect(u, u, 9 * u, 9 * u, 0.85f, 0.85f, 0.85f); // radar: bingkai terang
        rect(u + u / 3, u + u / 3, 9 * u - 2 * u / 3, 9 * u - 2 * u / 3, 0.40f, 0.52f, 0.40f);
        rect(5 * u, 5 * u, u / 2, u / 2, 0.9f, 0.1f, 0.1f);
        for (int i = 0; i < 7; ++i)                    // "angka" uang
            rect(w - 11 * u + i * (u + u / 2), h - 3 * u, u / 2, 2 * u, 0.95f, 0.95f, 0.95f);
        rect(w - 11 * u, h - 6 * u, 10 * u, u, 0.15f, 0.15f, 0.15f);   // bar health
        rect(w - 11 * u + 2, h - 6 * u + 2, 7 * u, u - 4, 0.75f, 0.12f, 0.12f);
        glDisable(GL_SCISSOR_TEST);
    }

    void Draw(int w, int h) { DrawWorld(w, h); DrawHud(w, h); }

private:
    GLuint objProg = 0, skyProg = 0, skyVbo = 0;
    Mesh cube = { 0, 0, 0 }, sphere = { 0, 0, 0 };
    std::vector<Object> objs;
};

} // namespace scene
