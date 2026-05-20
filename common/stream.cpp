/*
    OpenPnp-Capture: a video capture subsystem.

    Platform independent stream code

    Copyright (c) 2017 Jason von Nieda, Niels Moseley.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include <memory.h> // for memcpy
#include "stream.h"
#include "context.h"

// **********************************************************************
//   Stream
// **********************************************************************

Stream::Stream() :
    m_owner(nullptr),
    m_isOpen(false),
    m_frames(0),
    m_newFrame(false),
    m_rawMode(false),
    m_rawFrameSize(0)
{
}

Stream::~Stream()
{
    LOG_DEBUG("stream dtor: total_frames_captured={}", m_frames);
    //Note: close() should be called/handled by the PlatformStream!
}

bool Stream::hasNewFrame()
{
    m_bufferMutex.lock();
    bool ok = m_newFrame;
    m_bufferMutex.unlock();
    return ok;
}

bool Stream::captureFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (m_rawMode) {
        LOG_ERROR("captureFrame called on raw-mode stream — use captureFrameRaw or decodeFrame");
        return false;
    }
    if (!m_isOpen) return false;

    m_bufferMutex.lock();
    size_t maxBytes = RGBbufferBytes <= m_frameBuffer.size() ? RGBbufferBytes : m_frameBuffer.size();
    if (maxBytes != 0)
    {
        memcpy(RGBbufferPtr, &m_frameBuffer[0], maxBytes);
    }
    m_newFrame = false;
    m_bufferMutex.unlock();
    return true;
}

void Stream::submitBuffer(const uint8_t *ptr, size_t bytes)
{
    if (ptr == nullptr)
    {
        return;
    }

    m_bufferMutex.lock();

    if (m_frameBuffer.size() == 0)
    {
        LOG_ERROR("frame buffer size is 0 — can't store frames");
    }

    const uint32_t wantSize = m_width * m_height * 3;
    if ((bytes != wantSize) && ((m_frames % 100) == 0))
    {
        LOG_WARN("frame buffer size mismatch: got={} expected={} (frame #{})",
                 bytes, wantSize, m_frames);
    }

    if (m_frameBuffer.size() >= bytes)
    {
        memcpy(&m_frameBuffer[0], ptr, bytes);
        m_newFrame = true;
        m_frames++;
    }
    m_bufferMutex.unlock();
}

void Stream::submitRawBuffer(const uint8_t *ptr, size_t bytes)
{
    if (ptr == nullptr) return;

    m_bufferMutex.lock();

    if (bytes > m_rawBuffer.size()) {
        size_t newSize = bytes * 2;
        LOG_WARN("raw buffer resize: frame_bytes={} capacity={} new_capacity={}",
                 bytes, m_rawBuffer.size(), newSize);
        m_rawBuffer.resize(newSize);
    }

    memcpy(&m_rawBuffer[0], ptr, bytes);
    m_rawFrameSize = static_cast<uint32_t>(bytes);
    m_newFrame = true;
    m_frames++;

    if (m_frames == 1) {
        LOG_INFO("raw buffer: first frame received: bytes={} capacity={}",
                 bytes, m_rawBuffer.size());
    }
    LOG_TRACE("raw frame stored: frame=#{} bytes={}", m_frames, bytes);

    m_bufferMutex.unlock();
}

bool Stream::captureFrameRaw(uint8_t *outPtr, uint32_t outBytes, uint32_t *actualBytes)
{
    if (!m_isOpen) return false;
    if (!m_rawMode) {
        LOG_ERROR("captureFrameRaw called on non-raw stream");
        return false;
    }
    if (actualBytes == nullptr || outPtr == nullptr) {
        return false;
    }

    m_bufferMutex.lock();
    *actualBytes = m_rawFrameSize;

    if (outBytes < m_rawFrameSize) {
        LOG_WARN("captureFrameRaw: caller buffer too small: got={} need={}",
                 outBytes, m_rawFrameSize);
        m_bufferMutex.unlock();
        return false;
    }

    memcpy(outPtr, &m_rawBuffer[0], m_rawFrameSize);
    m_newFrame = false;
    LOG_DEBUG("captureFrameRaw: delivering bytes={} to caller", m_rawFrameSize);
    m_bufferMutex.unlock();
    return true;
}

bool Stream::getFrameSize(uint32_t *outBytes)
{
    if (!m_rawMode) return false;
    if (outBytes == nullptr) return false;

    m_bufferMutex.lock();
    *outBytes = m_rawFrameSize;
    bool ok = (m_rawFrameSize > 0);
    m_bufferMutex.unlock();
    return ok;
}

bool Stream::decodeFrame(uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (!m_isOpen) return false;
    if (!m_rawMode) {
        LOG_ERROR("decodeFrame called on non-raw stream");
        return false;
    }
    if (RGBbufferPtr == nullptr) return false;

    uint32_t needed = m_width * m_height * 3;
    if (RGBbufferBytes < needed) {
        LOG_WARN("decodeFrame: RGB buffer too small: got={} need={}",
                 RGBbufferBytes, needed);
        return false;
    }

    m_bufferMutex.lock();
    if (m_rawFrameSize == 0) {
        LOG_ERROR("decodeFrame: no raw frame available yet");
        m_bufferMutex.unlock();
        return false;
    }

    LOG_INFO("decodeFrame: decompressing: jpeg_bytes={} rgb={}x{}",
             m_rawFrameSize, m_width, m_height);

    bool ok = m_mjpegHelper.decompressFrame(
        &m_rawBuffer[0], m_rawFrameSize,
        RGBbufferPtr, m_width, m_height);

    if (!ok) {
        LOG_ERROR("decodeFrame: libjpeg-turbo decompress failed: frame_size={}", m_rawFrameSize);
    }

    m_bufferMutex.unlock();
    return ok;
}
