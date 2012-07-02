
/*
 * Copyright 2011 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */



#include "SkCanvas.h"
#include "SkData.h"
#include "SkDevice.h"
#include "SkPaint.h"
#include "SkPathEffect.h"
#include "SkGPipe.h"
#include "SkGPipePriv.h"
#include "SkImageFilter.h"
#include "SkStream.h"
#include "SkTSearch.h"
#include "SkTypeface.h"
#include "SkWriter32.h"
#include "SkColorFilter.h"
#include "SkDrawLooper.h"
#include "SkMaskFilter.h"
#include "SkRasterizer.h"
#include "SkShader.h"
#include "SkOrderedWriteBuffer.h"

static SkFlattenable* get_paintflat(const SkPaint& paint, unsigned paintFlat) {
    SkASSERT(paintFlat < kCount_PaintFlats);
    switch (paintFlat) {
        case kColorFilter_PaintFlat:    return paint.getColorFilter();
        case kDrawLooper_PaintFlat:     return paint.getLooper();
        case kMaskFilter_PaintFlat:     return paint.getMaskFilter();
        case kPathEffect_PaintFlat:     return paint.getPathEffect();
        case kRasterizer_PaintFlat:     return paint.getRasterizer();
        case kShader_PaintFlat:         return paint.getShader();
        case kImageFilter_PaintFlat:    return paint.getImageFilter();
        case kXfermode_PaintFlat:       return paint.getXfermode();
    }
    SkDEBUGFAIL("never gets here");
    return NULL;
}

static size_t writeTypeface(SkWriter32* writer, SkTypeface* typeface) {
    SkASSERT(typeface);
    SkDynamicMemoryWStream stream;
    typeface->serialize(&stream);
    size_t size = stream.getOffset();
    if (writer) {
        writer->write32(size);
        SkAutoDataUnref data(stream.copyToData());
        writer->writePad(data.data(), size);
    }
    return 4 + SkAlign4(size);
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Shared heap for storing large things that can be shared, for a stream
 * used by multiple readers.
 * TODO: Make the allocations all come from cross process safe address space
 * TODO: Store paths (others?)
 * TODO: Allow reclaiming of memory. Will require us to know when all readers
 *       have used the object.
 */
class Heap {
public:
    Heap(bool shallow) : fCanDoShallowCopies(shallow) {}
    ~Heap() {
        for (int i = 0; i < fBitmaps.count(); i++) {
            delete fBitmaps[i].fBitmap;
        }
    }

    /*
     * Add a copy of a bitmap to the heap.
     * @param bm The SkBitmap to be copied and placed in the heap.
     * @return void* Pointer to the heap's copy of the bitmap. If NULL,
     *               the bitmap could not be copied.
     */
    const SkBitmap* addBitmap(const SkBitmap& orig) {
        const uint32_t genID = orig.getGenerationID();
        SkPixelRef* sharedPixelRef = NULL;
        for (int i = fBitmaps.count() - 1; i >= 0; i--) {
            if (genID == fBitmaps[i].fGenID) {
                if (orig.pixelRefOffset() != fBitmaps[i].fBitmap->pixelRefOffset()) {
                    // In this case, the bitmaps share a pixelRef, but have
                    // different offsets. Keep track of the other bitmap so that
                    // instead of making another copy of the pixelRef we can use
                    // the copy we already made.
                    sharedPixelRef = fBitmaps[i].fBitmap->pixelRef();
                    break;
                }
                return fBitmaps[i].fBitmap;
            }
        }
        SkBitmap* copy;
        // If the bitmap is mutable, we still need to do a deep copy, since the
        // caller may modify it afterwards. That said, if the bitmap is mutable,
        // but has no pixelRef, the copy constructor actually does a deep copy.
        if (fCanDoShallowCopies && (orig.isImmutable() || !orig.pixelRef())) {
            copy = new SkBitmap(orig);
        } else {
            if (sharedPixelRef != NULL) {
                // Do a shallow copy of the bitmap to get the width, height, etc
                copy = new SkBitmap(orig);
                // Replace the pixelRef with the copy that was already made, and
                // use the appropriate offset.
                copy->setPixelRef(sharedPixelRef, orig.pixelRefOffset());
            } else {
                copy = new SkBitmap();
                if (!orig.copyTo(copy, orig.getConfig())) {
                    delete copy;
                    return NULL;
                }
            }
        }
        BitmapInfo* info = fBitmaps.append();
        info->fBitmap = copy;
        info->fGenID = genID;
        return copy;
    }
private:
    struct BitmapInfo {
        SkBitmap* fBitmap;
        // Store the generation ID of the original bitmap, since copying does
        // not copy this field, so fBitmap's generation ID will not be useful
        // for comparing.
        uint32_t fGenID;
    };
    SkTDArray<BitmapInfo>   fBitmaps;
    const bool              fCanDoShallowCopies;
};

///////////////////////////////////////////////////////////////////////////////

class SkGPipeCanvas : public SkCanvas {
public:
    SkGPipeCanvas(SkGPipeController*, SkWriter32*, SkFactorySet*, uint32_t flags);
    virtual ~SkGPipeCanvas();

    void finish() {
        if (!fDone) {
            if (this->needOpBytes()) {
                this->writeOp(kDone_DrawOp);
                this->doNotify();
            }
            fDone = true;
        }
    }

    // overrides from SkCanvas
    virtual int save(SaveFlags) SK_OVERRIDE;
    virtual int saveLayer(const SkRect* bounds, const SkPaint*,
                          SaveFlags) SK_OVERRIDE;
    virtual void restore() SK_OVERRIDE;
    virtual bool translate(SkScalar dx, SkScalar dy) SK_OVERRIDE;
    virtual bool scale(SkScalar sx, SkScalar sy) SK_OVERRIDE;
    virtual bool rotate(SkScalar degrees) SK_OVERRIDE;
    virtual bool skew(SkScalar sx, SkScalar sy) SK_OVERRIDE;
    virtual bool concat(const SkMatrix& matrix) SK_OVERRIDE;
    virtual void setMatrix(const SkMatrix& matrix) SK_OVERRIDE;
    virtual bool clipRect(const SkRect& rect, SkRegion::Op op,
                          bool doAntiAlias = false) SK_OVERRIDE;
    virtual bool clipPath(const SkPath& path, SkRegion::Op op,
                          bool doAntiAlias = false) SK_OVERRIDE;
    virtual bool clipRegion(const SkRegion& region, SkRegion::Op op) SK_OVERRIDE;
    virtual void clear(SkColor) SK_OVERRIDE;
    virtual void drawPaint(const SkPaint& paint) SK_OVERRIDE;
    virtual void drawPoints(PointMode, size_t count, const SkPoint pts[],
                            const SkPaint&) SK_OVERRIDE;
    virtual void drawRect(const SkRect& rect, const SkPaint&) SK_OVERRIDE;
    virtual void drawPath(const SkPath& path, const SkPaint&) SK_OVERRIDE;
    virtual void drawBitmap(const SkBitmap&, SkScalar left, SkScalar top,
                            const SkPaint*) SK_OVERRIDE;
    virtual void drawBitmapRect(const SkBitmap&, const SkIRect* src,
                                const SkRect& dst, const SkPaint*) SK_OVERRIDE;
    virtual void drawBitmapMatrix(const SkBitmap&, const SkMatrix&,
                                  const SkPaint*) SK_OVERRIDE;
    virtual void drawBitmapNine(const SkBitmap& bitmap, const SkIRect& center,
                                const SkRect& dst, const SkPaint* paint = NULL) SK_OVERRIDE;
    virtual void drawSprite(const SkBitmap&, int left, int top,
                            const SkPaint*) SK_OVERRIDE;
    virtual void drawText(const void* text, size_t byteLength, SkScalar x,
                          SkScalar y, const SkPaint&) SK_OVERRIDE;
    virtual void drawPosText(const void* text, size_t byteLength,
                             const SkPoint pos[], const SkPaint&) SK_OVERRIDE;
    virtual void drawPosTextH(const void* text, size_t byteLength,
                              const SkScalar xpos[], SkScalar constY,
                              const SkPaint&) SK_OVERRIDE;
    virtual void drawTextOnPath(const void* text, size_t byteLength,
                            const SkPath& path, const SkMatrix* matrix,
                                const SkPaint&) SK_OVERRIDE;
    virtual void drawPicture(SkPicture& picture) SK_OVERRIDE;
    virtual void drawVertices(VertexMode, int vertexCount,
                          const SkPoint vertices[], const SkPoint texs[],
                          const SkColor colors[], SkXfermode*,
                          const uint16_t indices[], int indexCount,
                              const SkPaint&) SK_OVERRIDE;
    virtual void drawData(const void*, size_t) SK_OVERRIDE;

private:
    Heap fHeap;
    SkFactorySet* fFactorySet;  // optional, only used if cross-process
    SkGPipeController* fController;
    SkWriter32& fWriter;
    size_t      fBlockSize; // amount allocated for writer
    size_t      fBytesNotified;
    bool        fDone;
    uint32_t    fFlags;

    SkRefCntSet fTypefaceSet;

    uint32_t getTypefaceID(SkTypeface*);

    inline void writeOp(DrawOps op, unsigned flags, unsigned data) {
        fWriter.write32(DrawOp_packOpFlagData(op, flags, data));
    }

    inline void writeOp(DrawOps op) {
        fWriter.write32(DrawOp_packOpFlagData(op, 0, 0));
    }

    bool needOpBytes(size_t size = 0);

    inline void doNotify() {
        if (!fDone) {
            size_t bytes = fWriter.size() - fBytesNotified;
            if (bytes > 0) {
                fController->notifyWritten(bytes);
                fBytesNotified += bytes;
            }
        }
    }

    struct FlatData {
        uint32_t    fIndex; // always > 0
        uint32_t    fSize;

        void*       data() { return (char*)this + sizeof(*this); }

        static int Compare(const FlatData* a, const FlatData* b) {
            return memcmp(&a->fSize, &b->fSize, a->fSize + sizeof(a->fSize));
        }
    };

    SkTDArray<FlatData*> fBitmapArray;
    int flattenToIndex(const SkBitmap&);

    SkTDArray<FlatData*> fFlatArray;
    int fCurrFlatIndex[kCount_PaintFlats];
    int flattenToIndex(SkFlattenable* obj, PaintFlats);

    SkPaint fPaint;
    void writePaint(const SkPaint&);

    class AutoPipeNotify {
    public:
        AutoPipeNotify(SkGPipeCanvas* canvas) : fCanvas(canvas) {}
        ~AutoPipeNotify() { fCanvas->doNotify(); }
    private:
        SkGPipeCanvas* fCanvas;
    };
    friend class AutoPipeNotify;

    typedef SkCanvas INHERITED;
};

int SkGPipeCanvas::flattenToIndex(const SkBitmap & bitmap) {
    SkASSERT(shouldFlattenBitmaps(fFlags));
    SkOrderedWriteBuffer tmpWriter(1024);
    tmpWriter.setFlags((SkFlattenableWriteBuffer::Flags)
                       (SkFlattenableWriteBuffer::kInlineFactoryNames_Flag
                        | SkFlattenableWriteBuffer::kCrossProcess_Flag));
    tmpWriter.setFactoryRecorder(fFactorySet);
    bitmap.flatten(tmpWriter);

    size_t len = tmpWriter.size();
    size_t allocSize = len + sizeof(FlatData);

    SkAutoSMalloc<1024> storage(allocSize);
    FlatData* flat = (FlatData*)storage.get();
    flat->fSize = len;
    tmpWriter.flatten(flat->data());
    
    int index = SkTSearch<FlatData>((const FlatData**)fBitmapArray.begin(),
                                     fBitmapArray.count(), flat, sizeof(flat),
                                     &FlatData::Compare);
    if (index < 0) {
        index = ~index;
        FlatData* copy = (FlatData*)sk_malloc_throw(allocSize);
        memcpy(copy, flat, allocSize);
        // For bitmaps, we can use zero based indices, since we will never ask
        // for a NULL bitmap (unlike with paint flattenables).
        copy->fIndex = fBitmapArray.count();
        *fBitmapArray.insert(index) = copy;
        if (this->needOpBytes(len)) {
            this->writeOp(kDef_Bitmap_DrawOp, 0, copy->fIndex);
            fWriter.write(copy->data(), len);
        }
    }
    return fBitmapArray[index]->fIndex;
}

// return 0 for NULL (or unflattenable obj), or index-base-1
int SkGPipeCanvas::flattenToIndex(SkFlattenable* obj, PaintFlats paintflat) {
    if (NULL == obj) {
        return 0;
    }

    SkOrderedWriteBuffer tmpWriter(1024);
    
    if (fFlags & SkGPipeWriter::kCrossProcess_Flag) {
        tmpWriter.setFlags((SkFlattenableWriteBuffer::Flags)
                           (SkFlattenableWriteBuffer::kInlineFactoryNames_Flag
                           | SkFlattenableWriteBuffer::kCrossProcess_Flag));
        tmpWriter.setFactoryRecorder(fFactorySet);
    } else {
        // Needed for bitmap shaders.
        tmpWriter.setFlags(SkFlattenableWriteBuffer::kForceFlattenBitmapPixels_Flag);
    }

    tmpWriter.writeFlattenable(obj);
    size_t len = tmpWriter.size();
    size_t allocSize = len + sizeof(FlatData);

    SkAutoSMalloc<1024> storage(allocSize);
    FlatData* flat = (FlatData*)storage.get();
    flat->fSize = len;
    tmpWriter.flatten(flat->data());

    int index = SkTSearch<FlatData>((const FlatData**)fFlatArray.begin(),
                                    fFlatArray.count(), flat, sizeof(flat),
                                    &FlatData::Compare);
    if (index < 0) {
        index = ~index;
        FlatData* copy = (FlatData*)sk_malloc_throw(allocSize);
        memcpy(copy, flat, allocSize);
        *fFlatArray.insert(index) = copy;
        // call this after the insert, so that count() will have been grown
        copy->fIndex = fFlatArray.count();
//        SkDebugf("--- add flattenable[%d] size=%d index=%d\n", paintflat, len, copy->fIndex);

        if (this->needOpBytes(len)) {
            this->writeOp(kDef_Flattenable_DrawOp, paintflat, copy->fIndex);
            fWriter.write(copy->data(), len);
        }
    }
    return fFlatArray[index]->fIndex;
}

///////////////////////////////////////////////////////////////////////////////

#define MIN_BLOCK_SIZE  (16 * 1024)

SkGPipeCanvas::SkGPipeCanvas(SkGPipeController* controller,
                             SkWriter32* writer, SkFactorySet* fset, uint32_t flags)
: fHeap(!(flags & SkGPipeWriter::kCrossProcess_Flag)), fWriter(*writer), fFlags(flags) {
    fFactorySet = fset;
    fController = controller;
    fDone = false;
    fBlockSize = 0; // need first block from controller
    fBytesNotified = 0;
    sk_bzero(fCurrFlatIndex, sizeof(fCurrFlatIndex));

    // we need a device to limit our clip
    // should the caller give us the bounds?
    // We don't allocate pixels for the bitmap
    SkBitmap bitmap;
    bitmap.setConfig(SkBitmap::kARGB_8888_Config, 32767, 32767);
    SkDevice* device = SkNEW_ARGS(SkDevice, (bitmap));
    this->setDevice(device)->unref();
    // Tell the reader the appropriate flags to use.
    if (this->needOpBytes()) {
        this->writeOp(kReportFlags_DrawOp, fFlags, 0);
    }
}

SkGPipeCanvas::~SkGPipeCanvas() {
    this->finish();

    fFlatArray.freeAll();
    fBitmapArray.freeAll();
}

bool SkGPipeCanvas::needOpBytes(size_t needed) {
    if (fDone) {
        return false;
    }

    needed += 4;  // size of DrawOp atom
    if (fWriter.size() + needed > fBlockSize) {
        // Before we wipe out any data that has already been written, read it
        // out.
        this->doNotify();
        size_t blockSize = SkMax32(MIN_BLOCK_SIZE, needed);
        void* block = fController->requestBlock(blockSize, &fBlockSize);
        if (NULL == block) {
            fDone = true;
            return false;
        }
        fWriter.reset(block, fBlockSize);
        fBytesNotified = 0;
    }
    return true;
}

uint32_t SkGPipeCanvas::getTypefaceID(SkTypeface* face) {
    uint32_t id = 0; // 0 means default/null typeface
    if (face) {
        id = fTypefaceSet.find(face);
        if (0 == id) {
            id = fTypefaceSet.add(face);
            size_t size = writeTypeface(NULL, face);
            if (this->needOpBytes(size)) {
                this->writeOp(kDef_Typeface_DrawOp);
                writeTypeface(&fWriter, face);
            }
        }
    }
    return id;
}

///////////////////////////////////////////////////////////////////////////////

#define NOTIFY_SETUP(canvas)    \
    AutoPipeNotify apn(canvas)

int SkGPipeCanvas::save(SaveFlags flags) {
    NOTIFY_SETUP(this);
    if (this->needOpBytes()) {
        this->writeOp(kSave_DrawOp, 0, flags);
    }
    return this->INHERITED::save(flags);
}

int SkGPipeCanvas::saveLayer(const SkRect* bounds, const SkPaint* paint,
                             SaveFlags saveFlags) {
    NOTIFY_SETUP(this);
    size_t size = 0;
    unsigned opFlags = 0;

    if (bounds) {
        opFlags |= kSaveLayer_HasBounds_DrawOpFlag;
        size += sizeof(SkRect);
    }
    if (paint) {
        opFlags |= kSaveLayer_HasPaint_DrawOpFlag;
        this->writePaint(*paint);
    }

    if (this->needOpBytes(size)) {
        this->writeOp(kSaveLayer_DrawOp, opFlags, saveFlags);
        if (bounds) {
            fWriter.writeRect(*bounds);
        }
    }

    // we just pass on the save, so we don't create a layer
    return this->INHERITED::save(saveFlags);
}

void SkGPipeCanvas::restore() {
    NOTIFY_SETUP(this);
    if (this->needOpBytes()) {
        this->writeOp(kRestore_DrawOp);
    }
    this->INHERITED::restore();
}

bool SkGPipeCanvas::translate(SkScalar dx, SkScalar dy) {
    if (dx || dy) {
        NOTIFY_SETUP(this);
        if (this->needOpBytes(2 * sizeof(SkScalar))) {
            this->writeOp(kTranslate_DrawOp);
            fWriter.writeScalar(dx);
            fWriter.writeScalar(dy);
        }
    }
    return this->INHERITED::translate(dx, dy);
}

bool SkGPipeCanvas::scale(SkScalar sx, SkScalar sy) {
    if (sx || sy) {
        NOTIFY_SETUP(this);
        if (this->needOpBytes(2 * sizeof(SkScalar))) {
            this->writeOp(kScale_DrawOp);
            fWriter.writeScalar(sx);
            fWriter.writeScalar(sy);
        }
    }
    return this->INHERITED::scale(sx, sy);
}

bool SkGPipeCanvas::rotate(SkScalar degrees) {
    if (degrees) {
        NOTIFY_SETUP(this);
        if (this->needOpBytes(sizeof(SkScalar))) {
            this->writeOp(kRotate_DrawOp);
            fWriter.writeScalar(degrees);
        }
    }
    return this->INHERITED::rotate(degrees);
}

bool SkGPipeCanvas::skew(SkScalar sx, SkScalar sy) {
    if (sx || sy) {
        NOTIFY_SETUP(this);
        if (this->needOpBytes(2 * sizeof(SkScalar))) {
            this->writeOp(kSkew_DrawOp);
            fWriter.writeScalar(sx);
            fWriter.writeScalar(sy);
        }
    }
    return this->INHERITED::skew(sx, sy);
}

bool SkGPipeCanvas::concat(const SkMatrix& matrix) {
    if (!matrix.isIdentity()) {
        NOTIFY_SETUP(this);
        if (this->needOpBytes(matrix.writeToMemory(NULL))) {
            this->writeOp(kConcat_DrawOp);
            fWriter.writeMatrix(matrix);
        }
    }
    return this->INHERITED::concat(matrix);
}

void SkGPipeCanvas::setMatrix(const SkMatrix& matrix) {
    NOTIFY_SETUP(this);
    if (this->needOpBytes(matrix.writeToMemory(NULL))) {
        this->writeOp(kSetMatrix_DrawOp);
        fWriter.writeMatrix(matrix);
    }
    this->INHERITED::setMatrix(matrix);
}

bool SkGPipeCanvas::clipRect(const SkRect& rect, SkRegion::Op rgnOp,
                             bool doAntiAlias) {
    NOTIFY_SETUP(this);
    if (this->needOpBytes(sizeof(SkRect)) + sizeof(bool)) {
        this->writeOp(kClipRect_DrawOp, 0, rgnOp);
        fWriter.writeRect(rect);
        fWriter.writeBool(doAntiAlias);
    }
    return this->INHERITED::clipRect(rect, rgnOp, doAntiAlias);
}

bool SkGPipeCanvas::clipPath(const SkPath& path, SkRegion::Op rgnOp,
                             bool doAntiAlias) {
    NOTIFY_SETUP(this);
    if (this->needOpBytes(path.writeToMemory(NULL)) + sizeof(bool)) {
        this->writeOp(kClipPath_DrawOp, 0, rgnOp);
        fWriter.writePath(path);
        fWriter.writeBool(doAntiAlias);
    }
    // we just pass on the bounds of the path
    return this->INHERITED::clipRect(path.getBounds(), rgnOp, doAntiAlias);
}

bool SkGPipeCanvas::clipRegion(const SkRegion& region, SkRegion::Op rgnOp) {
    NOTIFY_SETUP(this);
    if (this->needOpBytes(region.writeToMemory(NULL))) {
        this->writeOp(kClipRegion_DrawOp, 0, rgnOp);
        fWriter.writeRegion(region);
    }
    return this->INHERITED::clipRegion(region, rgnOp);
}

///////////////////////////////////////////////////////////////////////////////

void SkGPipeCanvas::clear(SkColor color) {
    NOTIFY_SETUP(this);
    unsigned flags = 0;
    if (color) {
        flags |= kClear_HasColor_DrawOpFlag;
    }
    if (this->needOpBytes(sizeof(SkColor))) {
        this->writeOp(kDrawClear_DrawOp, flags, 0);
        if (color) {
            fWriter.write32(color);
        }
    }
}

void SkGPipeCanvas::drawPaint(const SkPaint& paint) {
    NOTIFY_SETUP(this);
    this->writePaint(paint);
    if (this->needOpBytes()) {
        this->writeOp(kDrawPaint_DrawOp);
    }
}

void SkGPipeCanvas::drawPoints(PointMode mode, size_t count,
                                   const SkPoint pts[], const SkPaint& paint) {
    if (count) {
        NOTIFY_SETUP(this);
        this->writePaint(paint);
        if (this->needOpBytes(4 + count * sizeof(SkPoint))) {
            this->writeOp(kDrawPoints_DrawOp, mode, 0);
            fWriter.write32(count);
            fWriter.write(pts, count * sizeof(SkPoint));
        }
    }
}

void SkGPipeCanvas::drawRect(const SkRect& rect, const SkPaint& paint) {
    NOTIFY_SETUP(this);
    this->writePaint(paint);
    if (this->needOpBytes(sizeof(SkRect))) {
        this->writeOp(kDrawRect_DrawOp);
        fWriter.writeRect(rect);
    }
}

void SkGPipeCanvas::drawPath(const SkPath& path, const SkPaint& paint) {
    NOTIFY_SETUP(this);
    this->writePaint(paint);
    if (this->needOpBytes(path.writeToMemory(NULL))) {
        this->writeOp(kDrawPath_DrawOp);
        fWriter.writePath(path);
    }
}

void SkGPipeCanvas::drawBitmap(const SkBitmap& bm, SkScalar left, SkScalar top,
                                   const SkPaint* paint) {
    bool flatten = shouldFlattenBitmaps(fFlags);
    const void* ptr = 0;
    int bitmapIndex = 0;
    if (flatten) {
        bitmapIndex = this->flattenToIndex(bm);
    } else {
        ptr = fHeap.addBitmap(bm);
        if (NULL == ptr) {
            return;
        }
    }

    NOTIFY_SETUP(this);
    if (paint) {
        this->writePaint(*paint);
    }

    size_t opBytesNeeded = sizeof(SkScalar) * 2 + sizeof(bool);
    if (!flatten) {
        opBytesNeeded += sizeof(void*);
    }
    if (this->needOpBytes(opBytesNeeded)) {
        this->writeOp(kDrawBitmap_DrawOp, 0, bitmapIndex);
        if (!flatten) {
            fWriter.writePtr(const_cast<void*>(ptr));
        }
        fWriter.writeBool(paint != NULL);
        fWriter.writeScalar(left);
        fWriter.writeScalar(top);
    }
}

void SkGPipeCanvas::drawBitmapRect(const SkBitmap& bm, const SkIRect* src,
                                       const SkRect& dst, const SkPaint* paint) {
    bool flatten = shouldFlattenBitmaps(fFlags);
    const void* ptr = 0;
    int bitmapIndex = 0;
    if (flatten) {
        bitmapIndex = this->flattenToIndex(bm);
    } else {
        ptr = fHeap.addBitmap(bm);
        if (NULL == ptr) {
            return;
        }
    }

    NOTIFY_SETUP(this);
    if (paint) {
        this->writePaint(*paint);
    }

    size_t opBytesNeeded = sizeof(SkRect) + sizeof(bool) * 2;
    bool hasSrc = src != NULL;
    if (hasSrc) {
        opBytesNeeded += sizeof(int32_t) * 4;
    }
    if (!flatten) {
        opBytesNeeded += sizeof(void*);
    }
    if (this->needOpBytes(opBytesNeeded)) {
        this->writeOp(kDrawBitmapRect_DrawOp, 0, bitmapIndex);
        if (!flatten) {
            fWriter.writePtr(const_cast<void*>(ptr));
        }
        fWriter.writeBool(paint != NULL);
        fWriter.writeBool(hasSrc);
        if (hasSrc) {
            fWriter.write32(src->fLeft);
            fWriter.write32(src->fTop);
            fWriter.write32(src->fRight);
            fWriter.write32(src->fBottom);
        }
        fWriter.writeRect(dst);
    }
}

void SkGPipeCanvas::drawBitmapMatrix(const SkBitmap&, const SkMatrix&,
                                         const SkPaint*) {
    UNIMPLEMENTED
}

void SkGPipeCanvas::drawBitmapNine(const SkBitmap& bm, const SkIRect& center,
                                   const SkRect& dst, const SkPaint* paint) {
    bool flatten = shouldFlattenBitmaps(fFlags);
    const void* ptr = 0;
    int bitmapIndex = 0;
    if (flatten) {
        bitmapIndex = this->flattenToIndex(bm);
    } else {
        ptr = fHeap.addBitmap(bm);
        if (NULL == ptr) {
            return;
        }
    }

    NOTIFY_SETUP(this);
    if (paint) {
        this->writePaint(*paint);
    }

    size_t opBytesNeeded = sizeof(int32_t) * 4 + sizeof(bool) + sizeof(SkRect);
    if (!flatten) {
        opBytesNeeded += sizeof(void*);
    }
    if (this->needOpBytes(opBytesNeeded)) {
        this->writeOp(kDrawBitmapNine_DrawOp, 0, bitmapIndex);
        if (!flatten) {
            fWriter.writePtr(const_cast<void*>(ptr));
        }
        fWriter.writeBool(paint != NULL);
        fWriter.write32(center.fLeft);
        fWriter.write32(center.fTop);
        fWriter.write32(center.fRight);
        fWriter.write32(center.fBottom);
        fWriter.writeRect(dst);
    }
}

void SkGPipeCanvas::drawSprite(const SkBitmap& bm, int left, int top,
                                   const SkPaint* paint) {
    bool flatten = shouldFlattenBitmaps(fFlags);
    const void* ptr = 0;
    int bitmapIndex = 0;
    if (flatten) {
        bitmapIndex = this->flattenToIndex(bm);
    } else {
        ptr = fHeap.addBitmap(bm);
        if (NULL == ptr) {
            return;
        }
    }

    NOTIFY_SETUP(this);
    if (paint) {
        this->writePaint(*paint);
    }

    size_t opBytesNeeded = sizeof(int32_t) * 2 + sizeof(bool);
    if (!flatten) {
        opBytesNeeded += sizeof(void*);
    }
    if (this->needOpBytes(opBytesNeeded)) {
        this->writeOp(kDrawSprite_DrawOp, 0, bitmapIndex);
        if (!flatten) {
            fWriter.writePtr(const_cast<void*>(ptr));
        }
        fWriter.writeBool(paint != NULL);
        fWriter.write32(left);
        fWriter.write32(top);
    }
}

void SkGPipeCanvas::drawText(const void* text, size_t byteLength, SkScalar x,
                                 SkScalar y, const SkPaint& paint) {
    if (byteLength) {
        NOTIFY_SETUP(this);
        this->writePaint(paint);
        if (this->needOpBytes(4 + SkAlign4(byteLength) + 2 * sizeof(SkScalar))) {
            this->writeOp(kDrawText_DrawOp);
            fWriter.write32(byteLength);
            fWriter.writePad(text, byteLength);
            fWriter.writeScalar(x);
            fWriter.writeScalar(y);
        }
    }
}

void SkGPipeCanvas::drawPosText(const void* text, size_t byteLength,
                                const SkPoint pos[], const SkPaint& paint) {
    if (byteLength) {
        NOTIFY_SETUP(this);
        this->writePaint(paint);
        int count = paint.textToGlyphs(text, byteLength, NULL);
        if (this->needOpBytes(4 + SkAlign4(byteLength) + 4 + count * sizeof(SkPoint))) {
            this->writeOp(kDrawPosText_DrawOp);
            fWriter.write32(byteLength);
            fWriter.writePad(text, byteLength);
            fWriter.write32(count);
            fWriter.write(pos, count * sizeof(SkPoint));
        }
    }
}

void SkGPipeCanvas::drawPosTextH(const void* text, size_t byteLength,
                                 const SkScalar xpos[], SkScalar constY,
                                 const SkPaint& paint) {
    if (byteLength) {
        NOTIFY_SETUP(this);
        this->writePaint(paint);
        int count = paint.textToGlyphs(text, byteLength, NULL);
        if (this->needOpBytes(4 + SkAlign4(byteLength) + 4 + count * sizeof(SkScalar) + 4)) {
            this->writeOp(kDrawPosTextH_DrawOp);
            fWriter.write32(byteLength);
            fWriter.writePad(text, byteLength);
            fWriter.write32(count);
            fWriter.write(xpos, count * sizeof(SkScalar));
            fWriter.writeScalar(constY);
        }
    }
}

void SkGPipeCanvas::drawTextOnPath(const void* text, size_t byteLength,
                                   const SkPath& path, const SkMatrix* matrix,
                                   const SkPaint& paint) {
    if (byteLength) {
        NOTIFY_SETUP(this);
        unsigned flags = 0;
        size_t size = 4 + SkAlign4(byteLength) + path.writeToMemory(NULL);
        if (matrix) {
            flags |= kDrawTextOnPath_HasMatrix_DrawOpFlag;
            size += matrix->writeToMemory(NULL);
        }
        this->writePaint(paint);
        if (this->needOpBytes(size)) {
            this->writeOp(kDrawTextOnPath_DrawOp, flags, 0);

            fWriter.write32(byteLength);
            fWriter.writePad(text, byteLength);

            fWriter.writePath(path);
            if (matrix) {
                fWriter.writeMatrix(*matrix);
            }
        }
    }
}

void SkGPipeCanvas::drawPicture(SkPicture& picture) {
    // we want to playback the picture into individual draw calls
    this->INHERITED::drawPicture(picture);
}

void SkGPipeCanvas::drawVertices(VertexMode mode, int vertexCount,
                                 const SkPoint vertices[], const SkPoint texs[],
                                 const SkColor colors[], SkXfermode*,
                                 const uint16_t indices[], int indexCount,
                                 const SkPaint& paint) {
    if (0 == vertexCount) {
        return;
    }

    NOTIFY_SETUP(this);
    size_t size = 4 + vertexCount * sizeof(SkPoint);
    this->writePaint(paint);
    unsigned flags = 0;
    if (texs) {
        flags |= kDrawVertices_HasTexs_DrawOpFlag;
        size += vertexCount * sizeof(SkPoint);
    }
    if (colors) {
        flags |= kDrawVertices_HasColors_DrawOpFlag;
        size += vertexCount * sizeof(SkColor);
    }
    if (indices && indexCount > 0) {
        flags |= kDrawVertices_HasIndices_DrawOpFlag;
        size += 4 + SkAlign4(indexCount * sizeof(uint16_t));
    }

    if (this->needOpBytes(size)) {
        this->writeOp(kDrawVertices_DrawOp, flags, 0);
        fWriter.write32(mode);
        fWriter.write32(vertexCount);
        fWriter.write(vertices, vertexCount * sizeof(SkPoint));
        if (texs) {
            fWriter.write(texs, vertexCount * sizeof(SkPoint));
        }
        if (colors) {
            fWriter.write(colors, vertexCount * sizeof(SkColor));
        }

        // TODO: flatten xfermode

        if (indices && indexCount > 0) {
            fWriter.write32(indexCount);
            fWriter.writePad(indices, indexCount * sizeof(uint16_t));
        }
    }
}

void SkGPipeCanvas::drawData(const void* ptr, size_t size) {
    if (size && ptr) {
        NOTIFY_SETUP(this);
        unsigned data = 0;
        if (size < (1 << DRAWOPS_DATA_BITS)) {
            data = (unsigned)size;
        }
        if (this->needOpBytes(4 + SkAlign4(size))) {
            this->writeOp(kDrawData_DrawOp, 0, data);
            if (0 == data) {
                fWriter.write32(size);
            }
            fWriter.writePad(ptr, size);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

template <typename T> uint32_t castToU32(T value) {
    union {
        T           fSrc;
        uint32_t    fDst;
    } data;
    data.fSrc = value;
    return data.fDst;
}

void SkGPipeCanvas::writePaint(const SkPaint& paint) {
    SkPaint& base = fPaint;
    uint32_t storage[32];
    uint32_t* ptr = storage;

    if (base.getFlags() != paint.getFlags()) {
        *ptr++ = PaintOp_packOpData(kFlags_PaintOp, paint.getFlags());
        base.setFlags(paint.getFlags());
    }
    if (base.getColor() != paint.getColor()) {
        *ptr++ = PaintOp_packOp(kColor_PaintOp);
        *ptr++ = paint.getColor();
        base.setColor(paint.getColor());
    }
    if (base.getStyle() != paint.getStyle()) {
        *ptr++ = PaintOp_packOpData(kStyle_PaintOp, paint.getStyle());
        base.setStyle(paint.getStyle());
    }
    if (base.getStrokeJoin() != paint.getStrokeJoin()) {
        *ptr++ = PaintOp_packOpData(kJoin_PaintOp, paint.getStrokeJoin());
        base.setStrokeJoin(paint.getStrokeJoin());
    }
    if (base.getStrokeCap() != paint.getStrokeCap()) {
        *ptr++ = PaintOp_packOpData(kCap_PaintOp, paint.getStrokeCap());
        base.setStrokeCap(paint.getStrokeCap());
    }
    if (base.getStrokeWidth() != paint.getStrokeWidth()) {
        *ptr++ = PaintOp_packOp(kWidth_PaintOp);
        *ptr++ = castToU32(paint.getStrokeWidth());
        base.setStrokeWidth(paint.getStrokeWidth());
    }
    if (base.getStrokeMiter() != paint.getStrokeMiter()) {
        *ptr++ = PaintOp_packOp(kMiter_PaintOp);
        *ptr++ = castToU32(paint.getStrokeMiter());
        base.setStrokeMiter(paint.getStrokeMiter());
    }
    if (base.getTextEncoding() != paint.getTextEncoding()) {
        *ptr++ = PaintOp_packOpData(kEncoding_PaintOp, paint.getTextEncoding());
        base.setTextEncoding(paint.getTextEncoding());
    }
    if (base.getHinting() != paint.getHinting()) {
        *ptr++ = PaintOp_packOpData(kHinting_PaintOp, paint.getHinting());
        base.setHinting(paint.getHinting());
    }
    if (base.getTextAlign() != paint.getTextAlign()) {
        *ptr++ = PaintOp_packOpData(kAlign_PaintOp, paint.getTextAlign());
        base.setTextAlign(paint.getTextAlign());
    }
    if (base.getTextSize() != paint.getTextSize()) {
        *ptr++ = PaintOp_packOp(kTextSize_PaintOp);
        *ptr++ = castToU32(paint.getTextSize());
        base.setTextSize(paint.getTextSize());
    }
    if (base.getTextScaleX() != paint.getTextScaleX()) {
        *ptr++ = PaintOp_packOp(kTextScaleX_PaintOp);
        *ptr++ = castToU32(paint.getTextScaleX());
        base.setTextScaleX(paint.getTextScaleX());
    }
    if (base.getTextSkewX() != paint.getTextSkewX()) {
        *ptr++ = PaintOp_packOp(kTextSkewX_PaintOp);
        *ptr++ = castToU32(paint.getTextSkewX());
        base.setTextSkewX(paint.getTextSkewX());
    }

    if (!SkTypeface::Equal(base.getTypeface(), paint.getTypeface())) {
        uint32_t id = this->getTypefaceID(paint.getTypeface());
        *ptr++ = PaintOp_packOpData(kTypeface_PaintOp, id);
        base.setTypeface(paint.getTypeface());
    }

    for (int i = 0; i < kCount_PaintFlats; i++) {
        int index = this->flattenToIndex(get_paintflat(paint, i), (PaintFlats)i);
        SkASSERT(index >= 0 && index <= fFlatArray.count());
        if (index != fCurrFlatIndex[i]) {
            *ptr++ = PaintOp_packOpFlagData(kFlatIndex_PaintOp, i, index);
            fCurrFlatIndex[i] = index;
        }
    }

    size_t size = (char*)ptr - (char*)storage;
    if (size && this->needOpBytes(size)) {
        this->writeOp(kPaintOp_DrawOp, 0, size);
        fWriter.write(storage, size);
        for (size_t i = 0; i < size/4; i++) {
//            SkDebugf("[%d] %08X\n", i, storage[i]);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

#include "SkGPipe.h"

SkGPipeWriter::SkGPipeWriter() : fWriter(0) {
    fCanvas = NULL;
}

SkGPipeWriter::~SkGPipeWriter() {
    this->endRecording();
    SkSafeUnref(fCanvas);
}

SkCanvas* SkGPipeWriter::startRecording(SkGPipeController* controller, uint32_t flags) {
    if (NULL == fCanvas) {
        fWriter.reset(NULL, 0);
        fFactorySet.reset();
        fCanvas = SkNEW_ARGS(SkGPipeCanvas, (controller, &fWriter,
                                             (flags & kCrossProcess_Flag) ?
                                             &fFactorySet : NULL, flags));
    }
    return fCanvas;
}

void SkGPipeWriter::endRecording() {
    if (fCanvas) {
        fCanvas->finish();
        fCanvas->unref();
        fCanvas = NULL;
    }
}

