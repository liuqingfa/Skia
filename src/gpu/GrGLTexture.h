
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#ifndef GrGLTexture_DEFINED
#define GrGLTexture_DEFINED

#include "GrGpu.h"
#include "GrGLRenderTarget.h"

/**
 * A ref counted tex id that deletes the texture in its destructor.
 */
class GrGLTexID : public GrRefCnt {

public:
    GrGLTexID(const GrGLInterface* gl, GrGLuint texID, bool ownsID)
        : fGL(gl)
        , fTexID(texID)
        , fOwnsID(ownsID) {
    }

    virtual ~GrGLTexID() {
        if (0 != fTexID && fOwnsID) {
            GR_GL_CALL(fGL, DeleteTextures(1, &fTexID));
        }
    }

    void abandon() { fTexID = 0; }
    GrGLuint id() const { return fTexID; }

private:
    const GrGLInterface* fGL;
    GrGLuint             fTexID;
    bool                 fOwnsID;
};

////////////////////////////////////////////////////////////////////////////////


class GrGLTexture : public GrTexture {

public:
    enum Orientation {
        kBottomUp_Orientation,
        kTopDown_Orientation,
    };

    struct TexParams {
        GrGLenum fFilter;
        GrGLenum fWrapS;
        GrGLenum fWrapT;
        void invalidate() { memset(this, 0xff, sizeof(TexParams)); }
    };

    struct Desc {
        int             fContentWidth;
        int             fContentHeight;
        int             fAllocWidth;
        int             fAllocHeight;
        GrPixelConfig   fConfig;
        GrGLuint        fTextureID;
        bool            fOwnsID;
        GrGLenum        fUploadFormat;
        GrGLenum        fUploadType;
        Orientation     fOrientation;
    };

    // creates a texture that is also an RT
    GrGLTexture(GrGpuGL* gpu,
                const Desc& textureDesc,
                const GrGLRenderTarget::Desc& rtDesc);

    // creates a non-RT texture
    GrGLTexture(GrGpuGL* gpu,
                const Desc& textureDesc);


    virtual ~GrGLTexture() { this->release(); }

    // overrides of GrTexture
    virtual void uploadTextureData(int x,
                                   int y,
                                   int width,
                                   int height,
                                   const void* srcData,
                                   size_t rowBytes);
    virtual intptr_t getTextureHandle() const;

    // these functions 
    const TexParams& getCachedTexParams(GrGpu::ResetTimestamp* timestamp) const {
        *timestamp = fTexParamsTimestamp;
        return fTexParams;
    }
    void setCachedTexParams(const TexParams& texParams,
                            GrGpu::ResetTimestamp timestamp) {
        fTexParams = texParams;
        fTexParamsTimestamp = timestamp;
    }
    GrGLuint textureID() const { return fTexIDObj->id(); }

    GrGLenum uploadFormat() const { return fUploadFormat; }
    GrGLenum uploadType() const { return fUploadType; }

    /**
     * @return width() / allocWidth()
     */
    GrScalar contentScaleX() const { return fScaleX; }

    /**
     * @return height() / allocHeight()
     */
    GrScalar contentScaleY() const { return fScaleY; }

    // Ganesh assumes texture coordinates have their origin
    // in the top-left corner of the image. OpenGL, however,
    // has the origin in the lower-left corner. For content that
    // is loaded by Ganesh we just push the content "upside down"
    // (by GL's understanding of the world) in glTex*Image and the
    // addressing just works out. However, content generated by GL
    // (FBO or externally imported texture) will be updside down
    // and it is up to the GrGpuGL derivative to handle y-mirroing.
    Orientation orientation() const { return fOrientation; }

    static const GrGLenum* WrapMode2GLWrap(GrGLBinding binding);

protected:

    // overrides of GrTexture
    virtual void onAbandon();
    virtual void onRelease();

private:
    TexParams                       fTexParams;
    GrGpu::ResetTimestamp           fTexParamsTimestamp;
    GrGLTexID*                      fTexIDObj;
    GrGLenum                        fUploadFormat;
    GrGLenum                        fUploadType;
    // precomputed content / alloc ratios
    GrScalar                        fScaleX;
    GrScalar                        fScaleY;
    Orientation                     fOrientation;

    void init(GrGpuGL* gpu,
              const Desc& textureDesc,
              const GrGLRenderTarget::Desc* rtDesc);

    typedef GrTexture INHERITED;
};

#endif
