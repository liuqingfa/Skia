/*
 * Copyright 2008 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkFontConfigInterface.h"
#include "SkFontDescriptor.h"
#include "SkFontHost.h"
#include "SkFontHost_FreeType_common.h"
#include "SkFontStream.h"
#include "SkStream.h"
#include "SkTypeface.h"
#include "SkTypefaceCache.h"

SK_DECLARE_STATIC_MUTEX(gFontConfigInterfaceMutex);
static SkFontConfigInterface* gFontConfigInterface;

SkFontConfigInterface* SkFontConfigInterface::RefGlobal() {
    SkAutoMutexAcquire ac(gFontConfigInterfaceMutex);

    return SkSafeRef(gFontConfigInterface);
}

SkFontConfigInterface* SkFontConfigInterface::SetGlobal(SkFontConfigInterface* fc) {
    SkAutoMutexAcquire ac(gFontConfigInterfaceMutex);

    SkRefCnt_SafeAssign(gFontConfigInterface, fc);
    return fc;
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

// convenience function to create the direct interface if none is installed.
extern SkFontConfigInterface* SkCreateDirectFontConfigInterface();

static SkFontConfigInterface* RefFCI() {
    for (;;) {
        SkFontConfigInterface* fci = SkFontConfigInterface::RefGlobal();
        if (fci) {
            return fci;
        }
        fci = SkFontConfigInterface::GetSingletonDirectInterface();
        SkFontConfigInterface::SetGlobal(fci)->unref();
    }
}

class FontConfigTypeface : public SkTypeface_FreeType {
    SkFontConfigInterface::FontIdentity fIdentity;
    SkString fFamilyName;
    SkStream* fLocalStream;

public:
    FontConfigTypeface(Style style,
                       const SkFontConfigInterface::FontIdentity& fi,
                       const SkString& familyName)
            : INHERITED(style, SkTypefaceCache::NewFontID(), false)
            , fIdentity(fi)
            , fFamilyName(familyName)
            , fLocalStream(NULL) {}

    FontConfigTypeface(Style style, SkStream* localStream)
            : INHERITED(style, SkTypefaceCache::NewFontID(), false) {
        // we default to empty fFamilyName and fIdentity
        fLocalStream = localStream;
        SkSafeRef(localStream);
    }

    virtual ~FontConfigTypeface() {
        SkSafeUnref(fLocalStream);
    }

    const SkFontConfigInterface::FontIdentity& getIdentity() const {
        return fIdentity;
    }

    const char* getFamilyName() const { return fFamilyName.c_str(); }
    SkStream*   getLocalStream() const { return fLocalStream; }

    bool isFamilyName(const char* name) const {
        return fFamilyName.equals(name);
    }

protected:
    friend class SkFontHost;    // hack until we can make public versions

    virtual int onGetTableTags(SkFontTableTag tags[]) const SK_OVERRIDE;
    virtual size_t onGetTableData(SkFontTableTag, size_t offset,
                                  size_t length, void* data) const SK_OVERRIDE;
    virtual void onGetFontDescriptor(SkFontDescriptor*) const SK_OVERRIDE;
    virtual SkStream* onOpenStream(int* ttcIndex) const SK_OVERRIDE;

private:
    typedef SkTypeface_FreeType INHERITED;
};

///////////////////////////////////////////////////////////////////////////////

struct FindRec {
    FindRec(const char* name, SkTypeface::Style style)
        : fFamilyName(name)  // don't need to make a deep copy
        , fStyle(style) {}

    const char* fFamilyName;
    SkTypeface::Style fStyle;
};

static bool find_proc(SkTypeface* face, SkTypeface::Style style, void* ctx) {
    FontConfigTypeface* fci = (FontConfigTypeface*)face;
    const FindRec* rec = (const FindRec*)ctx;

    return rec->fStyle == style && fci->isFamilyName(rec->fFamilyName);
}

SkTypeface* SkFontHost::CreateTypeface(const SkTypeface* familyFace,
                                       const char familyName[],
                                       SkTypeface::Style style) {
    SkAutoTUnref<SkFontConfigInterface> fci(RefFCI());
    if (NULL == fci.get()) {
        return NULL;
    }

    if (familyFace) {
        FontConfigTypeface* fct = (FontConfigTypeface*)familyFace;
        familyName = fct->getFamilyName();
    }

    FindRec rec(familyName, style);
    SkTypeface* face = SkTypefaceCache::FindByProcAndRef(find_proc, &rec);
    if (face) {
//        SkDebugf("found cached face <%s> <%s> %p [%d]\n", familyName, ((FontConfigTypeface*)face)->getFamilyName(), face, face->getRefCnt());
        return face;
    }

    SkFontConfigInterface::FontIdentity indentity;
    SkString                            outFamilyName;
    SkTypeface::Style                   outStyle;

    if (!fci->matchFamilyName(familyName, style,
                              &indentity, &outFamilyName, &outStyle)) {
        return NULL;
    }

    face = SkNEW_ARGS(FontConfigTypeface, (outStyle, indentity, outFamilyName));
    SkTypefaceCache::Add(face, style);
//    SkDebugf("add face <%s> <%s> %p [%d]\n", familyName, outFamilyName.c_str(), face, face->getRefCnt());
    return face;
}

SkTypeface* SkFontHost::CreateTypefaceFromStream(SkStream* stream) {
    if (!stream) {
        return NULL;
    }
    const size_t length = stream->getLength();
    if (!length) {
        return NULL;
    }
    if (length >= 1024 * 1024 * 1024) {
        return NULL;  // don't accept too large fonts (>= 1GB) for safety.
    }

    // TODO should the caller give us the style?
    SkTypeface::Style style = SkTypeface::kNormal;
    SkTypeface* face = SkNEW_ARGS(FontConfigTypeface, (style, stream));
    SkTypefaceCache::Add(face, style);
    return face;
}

SkTypeface* SkFontHost::CreateTypefaceFromFile(const char path[]) {
    SkAutoTUnref<SkStream> stream(SkStream::NewFromFile(path));
    return stream.get() ? CreateTypefaceFromStream(stream) : NULL;
}

///////////////////////////////////////////////////////////////////////////////

// DEPRECATED
int SkFontHost::CountTables(SkFontID fontID) {
    SkTypeface* face = SkTypefaceCache::FindByID(fontID);
    return face ? face->onGetTableTags(NULL) : 0;
}

// DEPRECATED
int SkFontHost::GetTableTags(SkFontID fontID, SkFontTableTag tags[]) {
    SkTypeface* face = SkTypefaceCache::FindByID(fontID);
    return face ? face->onGetTableTags(tags) : 0;
}

// DEPRECATED
size_t SkFontHost::GetTableSize(SkFontID fontID, SkFontTableTag tag) {
    SkTypeface* face = SkTypefaceCache::FindByID(fontID);
    return face ? face->onGetTableData(tag, 0, ~0U, NULL) : 0;
}

// DEPRECATED
size_t SkFontHost::GetTableData(SkFontID fontID, SkFontTableTag tag,
                                size_t offset, size_t length, void* dst) {
    SkTypeface* face = SkTypefaceCache::FindByID(fontID);
    return face ? face->onGetTableData(tag, offset, length, dst) : 0;
}

// DEPRECATED
SkTypeface* SkFontHost::NextLogicalTypeface(SkFontID curr, SkFontID orig) {
    // We don't handle font fallback.
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////

// Serialize, Deserialize need to be compatible across platforms, hence the use
// of SkFontDescriptor.

void SkFontHost::Serialize(const SkTypeface* face, SkWStream* stream) {
    FontConfigTypeface* fct = (FontConfigTypeface*)face;

    SkFontDescriptor desc;
    fct->onGetFontDescriptor(&desc);
    desc.serialize(stream);

    // by convention, we also write out the actual sfnt data, preceeded by
    // a packed-length. For now we skip that, so we just write the zero.
    stream->writePackedUInt(0);
}

SkTypeface* SkFontHost::Deserialize(SkStream* stream) {
    SkFontDescriptor descriptor(stream);
    const char* familyName = descriptor.getFamilyName();
    const SkTypeface::Style style = descriptor.getStyle();

    size_t length = stream->readPackedUInt();
    if (length > 0) {
        void* addr = sk_malloc_flags(length, 0);
        if (addr) {
            SkAutoTUnref<SkStream> localStream(SkNEW_ARGS(SkMemoryStream,
                                                        (addr, length, false)));
            return SkFontHost::CreateTypefaceFromStream(localStream.get());
        }
        // failed to allocate, so just skip and create-from-name
        stream->skip(length);
    }

    return SkFontHost::CreateTypeface(NULL, familyName, style);
}

///////////////////////////////////////////////////////////////////////////////

SkStream* FontConfigTypeface::onOpenStream(int* ttcIndex) const {
    SkStream* stream = this->getLocalStream();
    if (stream) {
        // TODO: fix issue 1176.
        // As of now open_stream will return a stream and unwind it, but the
        // SkStream is not thread safe, and if two threads use the stream they
        // may collide and print preview for example could still fail,
        // or there could be some failures in rendering if this stream is used
        // there.
        stream->rewind();
        stream->ref();
        // should have been provided by CreateFromStream()
        *ttcIndex = 0;
    } else {
        SkAutoTUnref<SkFontConfigInterface> fci(RefFCI());
        if (NULL == fci.get()) {
            return NULL;
        }
        stream = fci->openStream(this->getIdentity());
        *ttcIndex = this->getIdentity().fTTCIndex;
    }
    return stream;
}

SkStream* SkFontHost::OpenStream(uint32_t) {
    SkASSERT(!"SkFontHost::OpenStream is DEPRECATED: call SkTypeface::openStream\n");
    return NULL;
}

size_t SkFontHost::GetFileName(SkFontID fontID, char path[], size_t length,
                               int32_t* index) {
    SkASSERT(!"SkFontHost::GetFileName is DEPRECATED: call SkTypeface::openStream\n");
    return 0;
}

///////////////////////////////////////////////////////////////////////////////

int FontConfigTypeface::onGetTableTags(SkFontTableTag tags[]) const {
    int ttcIndex;
    SkAutoTUnref<SkStream> stream(this->openStream(&ttcIndex));
    return stream.get()
                ? SkFontStream::GetTableTags(stream, ttcIndex, tags)
                : 0;
}

size_t FontConfigTypeface::onGetTableData(SkFontTableTag tag, size_t offset,
                                  size_t length, void* data) const {
    int ttcIndex;
    SkAutoTUnref<SkStream> stream(this->openStream(&ttcIndex));
    return stream.get()
                ? SkFontStream::GetTableData(stream, ttcIndex,
                                             tag, offset, length, data)
                : 0;
}

void FontConfigTypeface::onGetFontDescriptor(SkFontDescriptor* desc) const {
    desc->setStyle(this->style());
    desc->setFamilyName(this->getFamilyName());
}
