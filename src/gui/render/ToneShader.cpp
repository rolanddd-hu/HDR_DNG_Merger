#include "ToneShader.h"
#include <spdlog/spdlog.h>
#include <vector>
#include <cmath>

namespace hdr::gui {

static const char* kVertexShaderSource = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char* kFragmentShaderSource = R"(#version 330 core
in vec2 vTexCoord;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform float uExposure;
uniform float uHighlights;
uniform float uShadows;
uniform float uWhites;
uniform float uBlacks;
uniform float uContrast;
uniform float uVibrance;
uniform float uSaturation;
uniform float uTemperature;
uniform float uTint;
uniform int uToneMap;
uniform int uShowClipping;

// Standard sRGB to linear conversion and luminance
float getLuminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// 1. Reinhard tone map
vec3 reinhard(vec3 x) {
    return x / (vec3(1.0) + x);
}

// 2. ACES Filmic curve fit by Krzysztof Narkowicz
vec3 acesFilm(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// 3. Reinhard Luminance-Preserving Extended
vec3 reinhardLuma(vec3 c) {
    float l = getLuminance(c);
    if (l <= 0.0001) return c;
    const float whitePoint = 4.0;
    float num = l * (1.0 + (l / (whitePoint * whitePoint)));
    float den = 1.0 + l;
    float lOut = clamp(num / den, 0.0, 1.0);
    return c * (lOut / l);
}

// 4. Hable (Uncharted 2) Filmic
vec3 hableCurve(vec3 x) {
    const float A = 0.15; // Shoulder Strength
    const float B = 0.50; // Linear Strength
    const float C = 0.10; // Linear Angle
    const float D = 0.20; // Toe Strength
    const float E = 0.02; // Toe Numerator
    const float F = 0.30; // Toe Denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - (E / F);
}

vec3 hable(vec3 x) {
    const float W = 11.2;
    vec3 curr = hableCurve(x * 2.0);
    vec3 whiteScale = vec3(1.0) / hableCurve(vec3(W));
    return clamp(curr * whiteScale, 0.0, 1.0);
}

// 5. AgX Tone Mapping (Troy Sobotka fit)
vec3 agxDefaultContrastApprox(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5     * x4 * x2
           - 40.14    * x4 * x
           + 31.96    * x4
           - 6.868    * x2 * x
           + 0.4298   * x2
           + 0.1191   * x
           - 0.00232;
}

vec3 agxToneMap(vec3 val) {
    const mat3 agx_mat = mat3(
        0.842479062253094, 0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772, 0.0784336,
        0.0792237451477743, 0.0791661274605434, 0.879142973798673
    );
    const mat3 agx_mat_inv = mat3(
        1.19687900512017, -0.0528968517005476, -0.0529716355084724,
        -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
        -0.0990297040700308, -0.0989611760587077, 1.15107367264188
    );
    val = agx_mat * max(val, vec3(1e-6));
    const float min_ev = -10.0;
    const float max_ev = 6.5;
    vec3 val_log = clamp((log2(val) - min_ev) / (max_ev - min_ev), 0.0, 1.0);
    vec3 val_mapped = clamp(agxDefaultContrastApprox(val_log), 0.0, 1.0);
    return clamp(agx_mat_inv * val_mapped, 0.0, 1.0);
}

// 6. Lottes (AMD) Tone Mapping
float lottesScalar(float x) {
    const float a = 1.6;
    const float d = 0.977;
    const float hdrMax = 8.0;
    const float midIn = 0.18;
    const float midOut = 0.267;

    const float b =
        (-pow(midIn, a) + pow(hdrMax, a) * midOut) /
        ((pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut);
    const float c =
        (pow(hdrMax, a * d) * pow(midIn, a) - pow(hdrMax, a) * pow(midIn, a * d) * midOut) /
        ((pow(hdrMax, a * d) - pow(midIn, a * d)) * midOut);

    return pow(max(x, 0.0), a) / (pow(max(x, 0.0), a * d) * b + c);
}

vec3 lottes(vec3 v) {
    return clamp(vec3(lottesScalar(v.r), lottesScalar(v.g), lottesScalar(v.b)), 0.0, 1.0);
}

// 7. Uchimura (Gran Turismo Sport) Tone Mapping
float uchimuraScalar(float x) {
    const float P = 1.0;
    const float a = 1.0;
    const float m = 0.22;
    const float l = 0.4;
    const float c = 1.33;
    const float b = 0.0;

    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    if (x < m) {
        return m * pow(max(x / m, 0.0), c) + b;
    } else if (x < S0) {
        return m + a * (x - m);
    } else {
        return P - (P - S1) * exp(CP * (x - S0));
    }
}

vec3 uchimura(vec3 v) {
    return clamp(vec3(uchimuraScalar(v.r), uchimuraScalar(v.g), uchimuraScalar(v.b)), 0.0, 1.0);
}

// sRGB display transfer function
vec3 linearToSrgb(vec3 c) {
    vec3 low = c * 12.92;
    vec3 high = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    vec3 isLow = step(c, vec3(0.0031308));
    return mix(high, low, isLow);
}

void main() {
    vec3 c = texture(uTexture, vTexCoord).rgb;

    // 1. Exposure compensation
    c *= pow(2.0, uExposure);

    // 2. White balance (Temp & Tint)
    // Positive temp warms (more R, less B). Positive tint is magenta (more G or less G).
    float tWarm = exp(uTemperature * 0.005);
    c.r *= tWarm;
    c.b /= tWarm;
    float tTint = exp(uTint * 0.005);
    c.g /= tTint;

    c = max(c, vec3(0.0));

    // 3. Highlights & Shadows (Photographic EV scaling)
    float lum = getLuminance(c);
    if (lum > 0.0001) {
        // Highlight recovery / boost
        float hlFactor = smoothstep(0.3, 1.2, lum);
        float hlEv = (uHighlights / 100.0) * 1.5 * hlFactor;
        float hlScale = pow(2.0, hlEv);
        
        // Shadow lift / drop
        float shFactor = 1.0 - smoothstep(0.0, 0.5, lum);
        float shEv = (uShadows / 100.0) * 1.5 * shFactor;
        float shScale = pow(2.0, shEv);

        c *= hlScale * shScale;
    }

    // 4. Whites and Blacks
    // Whites scales high end, Blacks shifts shadow floor
    float wScale = 1.0 / max(1.0 - (uWhites * 0.002), 0.1);
    c *= wScale;
    c = max(c + vec3(uBlacks * 0.001), vec3(0.0));

    // 5. Contrast (S-curve around middle gray 0.18)
    if (abs(uContrast) > 0.01) {
        float k = 1.0 + (uContrast * 0.01);
        c = pow(max(c / 0.18, vec3(0.0001)), vec3(k)) * 0.18;
    }

    // 6. Vibrance and Saturation
    lum = getLuminance(c);
    float maxCh = max(c.r, max(c.g, c.b));
    float minCh = min(c.r, min(c.g, c.b));
    float currentSat = (maxCh > 0.0001) ? (maxCh - minCh) / maxCh : 0.0;

    // Saturation directly adjusts distance from luminance
    float satMul = 1.0 + (uSaturation * 0.01);
    // Vibrance adjusts less saturated pixels more strongly
    float vibMul = 1.0 + (uVibrance * 0.01) * (1.0 - currentSat);
    c = mix(vec3(lum), c, max(satMul * vibMul, 0.0));
    c = max(c, vec3(0.0));

    // 7. Tone Mapping
    if (uToneMap == 1) {
        c = reinhard(c);
    } else if (uToneMap == 2) {
        c = acesFilm(c);
    } else if (uToneMap == 3) {
        c = reinhardLuma(c);
    } else if (uToneMap == 4) {
        c = hable(c);
    } else if (uToneMap == 5) {
        c = agxToneMap(c);
    } else if (uToneMap == 6) {
        c = lottes(c);
    } else if (uToneMap == 7) {
        c = uchimura(c);
    } else {
        c = clamp(c, 0.0, 1.0);
    }

    // 8. Clipping warning indicators
    if (uShowClipping == 1) {
        if (c.r >= 0.995 || c.g >= 0.995 || c.b >= 0.995) {
            FragColor = vec4(1.0, 0.0, 0.0, 1.0); // Red highlight clip
            return;
        }
        if (c.r <= 0.005 && c.g <= 0.005 && c.b <= 0.005) {
            FragColor = vec4(0.0, 0.3, 1.0, 1.0); // Blue shadow clip
            return;
        }
    }

    // 9. Display OETF (sRGB)
    c = linearToSrgb(c);

    FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

ToneShader::ToneShader() = default;

ToneShader::~ToneShader() {
    release();
}

void ToneShader::release() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

bool ToneShader::init() {
    if (!compileProgram()) {
        return false;
    }

    // Setup fullscreen quad (-1 to 1) with UVs (0 to 1)
    float quadVertices[] = {
        // Pos        // TexCoord
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,

        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glGenFramebuffers(1, &fbo_);

    return true;
}

bool ToneShader::compileProgram() {
    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);
        GLint success = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            spdlog::error("Shader compilation failed: {}", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShaderSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShaderSource);
    if (vs == 0 || fs == 0) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        spdlog::error("Program linking failed: {}", log);
        glDeleteProgram(program_);
        program_ = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    if (program_ == 0) return false;

    // Cache uniforms
    locTex_ = glGetUniformLocation(program_, "uTexture");
    locExposure_ = glGetUniformLocation(program_, "uExposure");
    locHighlights_ = glGetUniformLocation(program_, "uHighlights");
    locShadows_ = glGetUniformLocation(program_, "uShadows");
    locWhites_ = glGetUniformLocation(program_, "uWhites");
    locBlacks_ = glGetUniformLocation(program_, "uBlacks");
    locContrast_ = glGetUniformLocation(program_, "uContrast");
    locVibrance_ = glGetUniformLocation(program_, "uVibrance");
    locSaturation_ = glGetUniformLocation(program_, "uSaturation");
    locTemp_ = glGetUniformLocation(program_, "uTemperature");
    locTint_ = glGetUniformLocation(program_, "uTint");
    locToneMap_ = glGetUniformLocation(program_, "uToneMap");
    locShowClipping_ = glGetUniformLocation(program_, "uShowClipping");

    return true;
}

bool ToneShader::renderToTexture(
    const GlTexture& srcTexture,
    GlTexture& dstTexture,
    const ToneParams& params,
    bool showClipping,
    int dstW,
    int dstH
) {
    if (!srcTexture.isValid()) return false;
    if (program_ == 0 || vao_ == 0 || fbo_ == 0) return false;

    const int w = dstW > 0 ? dstW : srcTexture.width();
    const int h = dstH > 0 ? dstH : srcTexture.height();

    // Ensure dstTexture matches dimensions
    if (dstTexture.width() != w || dstTexture.height() != h) {
        dstTexture.createRenderTarget(w, h);
    }

    // Save previous viewport
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);

    // Bind FBO and attach dstTexture
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTexture.id(), 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        return false;
    }

    glViewport(0, 0, w, h);

    glUseProgram(program_);

    srcTexture.bind(0);
    if (locTex_ != -1) glUniform1i(locTex_, 0);

    if (locExposure_ != -1) glUniform1f(locExposure_, params.exposure);
    if (locHighlights_ != -1) glUniform1f(locHighlights_, params.highlights);
    if (locShadows_ != -1) glUniform1f(locShadows_, params.shadows);
    if (locWhites_ != -1) glUniform1f(locWhites_, params.whites);
    if (locBlacks_ != -1) glUniform1f(locBlacks_, params.blacks);
    if (locContrast_ != -1) glUniform1f(locContrast_, params.contrast);
    if (locVibrance_ != -1) glUniform1f(locVibrance_, params.vibrance);
    if (locSaturation_ != -1) glUniform1f(locSaturation_, params.saturation);
    if (locTemp_ != -1) glUniform1f(locTemp_, params.temperature);
    if (locTint_ != -1) glUniform1f(locTint_, params.tint);
    if (locToneMap_ != -1) glUniform1i(locToneMap_, static_cast<int>(params.toneMap));
    if (locShowClipping_ != -1) glUniform1i(locShowClipping_, showClipping ? 1 : 0);

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    GlTexture::unbind(0);
    glUseProgram(0);

    // Restore previous state
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);

    dstTexture.generateMipmaps();
    return true;
}

} // namespace hdr::gui
