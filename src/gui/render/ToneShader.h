#pragma once
#include <glad/glad.h>
#include "GlTexture.h"
#include "../../core/Types.h"

namespace hdr::gui {

class ToneShader {
public:
    ToneShader();
    ~ToneShader();

    ToneShader(const ToneShader&) = delete;
    ToneShader& operator=(const ToneShader&) = delete;

    bool init();
    void release();

    // Renders srcTexture through the tone pipeline into dstTexture (resized to dstW x dstH,
    // or the source size when 0) and regenerates dstTexture's mipmaps.
    bool renderToTexture(
        const GlTexture& srcTexture,
        GlTexture& dstTexture,
        const ToneParams& params,
        bool showClipping = false,
        int dstW = 0,
        int dstH = 0
    );

private:
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint fbo_ = 0;

    // Uniform locations
    GLint locTex_ = -1;
    GLint locExposure_ = -1;
    GLint locHighlights_ = -1;
    GLint locShadows_ = -1;
    GLint locWhites_ = -1;
    GLint locBlacks_ = -1;
    GLint locContrast_ = -1;
    GLint locVibrance_ = -1;
    GLint locSaturation_ = -1;
    GLint locTemp_ = -1;
    GLint locTint_ = -1;
    GLint locToneMap_ = -1;
    GLint locShowClipping_ = -1;

    bool compileProgram();
};

} // namespace hdr::gui
