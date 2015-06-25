//
// Copyright 2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// BufferD3D.h: Defines the rx::BufferD3D class, an implementation of BufferImpl.

#ifndef LIBANGLE_RENDERER_D3D_BUFFERD3D_H_
#define LIBANGLE_RENDERER_D3D_BUFFERD3D_H_

#include "libANGLE/angletypes.h"
#include "libANGLE/renderer/BufferImpl.h"

#include <stdint.h>
#include <map>

namespace rx
{
class BufferFactoryD3D;
class StaticIndexBufferInterface;
class StaticVertexBufferInterface;

class BufferD3D : public BufferImpl
{
  public:
    BufferD3D(BufferFactoryD3D *factory);
    virtual ~BufferD3D();

    unsigned int getSerial() const { return mSerial; }

    virtual size_t getSize() const = 0;
    virtual bool supportsDirectIndexBinding() const = 0;
    virtual bool supportsDirectVertexBindingForAttrib(const gl::VertexAttribute &attrib) = 0;

    virtual void markTransformFeedbackUsage() = 0;
    virtual gl::Error getData(const uint8_t **outData) = 0;

    StaticVertexBufferInterface *getStaticVertexBufferForAttribute(const gl::VertexAttribute &attrib);
    StaticIndexBufferInterface *getStaticIndexBuffer() { return mStaticIndexBuffer; }

    void enableStaticData();
    void invalidateStaticIndexData();
    void promoteStaticIndexUsage(int dataSize);
    void promoteStaticVertexUsageForAttrib(const gl::VertexAttribute &attrib, int dataSize);

    gl::Error getIndexRange(GLenum type, size_t offset, size_t count, gl::RangeUI *outRange) override;

  protected:
    void updateSerial();

    BufferFactoryD3D *mFactory;
    unsigned int mSerial;
    static unsigned int mNextSerial;

    StaticVertexBufferInterface *findStaticVertexBufferForAttribute(const gl::VertexAttribute &attrib);

    struct AttribElement
    {
        GLenum type;
        GLuint size;
        GLuint stride;
        bool normalized;
        bool pureInteger;
        size_t attributeOffset;

        bool operator<(const AttribElement &ve) const {
            return this->type < ve.type ||
                   (this->type == ve.type && this->size < ve.size) ||
                   (this->type == ve.type && this->size == ve.size && this->stride < ve.stride) ||
                   (this->type == ve.type && this->size == ve.size && this->stride == ve.stride && this->normalized < ve.normalized) ||
                   (this->type == ve.type && this->size == ve.size && this->stride == ve.stride && this->normalized == ve.normalized && this->pureInteger < ve.pureInteger) ||
                   (this->type == ve.type && this->size == ve.size && this->stride == ve.stride && this->normalized == ve.normalized && this->pureInteger == ve.pureInteger && this->attributeOffset < ve.attributeOffset);
        }
    };

    static AttribElement CreateAttribElementFromAttrib(const gl::VertexAttribute &attrib);

    bool mUseStaticBuffers;
    typedef std::map<AttribElement, StaticVertexBufferInterface*>::iterator StaticBufferIteratorType;
    std::map<AttribElement, StaticVertexBufferInterface*> mStaticVertexBufferForAttributeMap;
    std::map<AttribElement, unsigned int> mUnmodifiedVertexDataUseMap;

    StaticIndexBufferInterface *mStaticIndexBuffer;
    unsigned int mUnmodifiedIndexDataUse;

};

}

#endif // LIBANGLE_RENDERER_D3D_BUFFERD3D_H_
