#pragma once
#include <glad/glad.h>
#include <cstdint>
#include <utility>
#include <vector>

namespace hdr::gui {

class GlTexture {
public:
    GlTexture();
    ~GlTexture();

    GlTexture(const GlTexture&) = delete;
    GlTexture& operator=(const GlTexture&) = delete;

    GlTexture(GlTexture&& other) noexcept;
    GlTexture& operator=(GlTexture&& other) noexcept;

    void release();

    bool uploadRgbFloat(int width, int height, const float* data);
    bool uploadRgbaFloat(int width, int height, const float* data);
    bool uploadRgba8(int width, int height, const uint8_t* data);
    bool createRenderTarget(int width, int height);
    void generateMipmaps();
    // Reads mip level 0 of an RGBA8 texture.
    bool readRgba8(std::vector<uint8_t>& out) const;

    void bind(GLuint unit = 0) const;
    static void unbind(GLuint unit = 0);

    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool isValid() const { return id_ != 0 && width_ > 0 && height_ > 0; }

private:
    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace hdr::gui
