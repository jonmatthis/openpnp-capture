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
    LOG(LOG_DEBUG,"Stream::~Stream reports %d frames captured.\n", m_frames);
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
        LOG(LOG_ERR, "captureFrame called on raw-mode stream. Use captureFrameRaw or decodeFrame.\n");
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
    // sanity check
    if (ptr == nullptr)
    {
        return;
    }
    
    m_bufferMutex.lock();
    
    if (m_frameBuffer.size() == 0)
    {
        LOG(LOG_ERR,"Stream::m_frameBuffer size is 0 - cant store frame buffers!\n");
    }

    // Generate warning every 100 frames if the frame buffer is not
    // the expected size. 
    
    const uint32_t wantSize = m_width*m_height*3;
    if ((bytes != wantSize) && ((m_frames % 100) == 0))
    {
        LOG(LOG_WARNING, "Warning: captureFrame received incorrect buffer size (got %d want %d)\n", bytes, wantSize);
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

    // grow buffer with 2x headroom if needed
    if (bytes > m_rawBuffer.size()) {
        size_t newSize = bytes * 2;
        LOG(LOG_WARNING,
            "Raw buffer growing: frame %u bytes exceeds capacity %u, resizing to %u\n",
            static_cast<uint32_t>(bytes),
            static_cast<uint32_t>(m_rawBuffer.size()),
            static_cast<uint32_t>(newSize));
        m_rawBuffer.resize(newSize);
    }

    memcpy(&m_rawBuffer[0], ptr, bytes);
    m_rawFrameSize = static_cast<uint32_t>(bytes);
    m_newFrame = true;
    m_frames++;

    if (m_frames == 1) {
        LOG(LOG_INFO,
            "Raw buffer: first frame received, %u bytes (capacity %u)\n",
            static_cast<uint32_t>(bytes),
            static_cast<uint32_t>(m_rawBuffer.size()));
    }
    LOG(LOG_DEBUG, "Raw frame #%u stored: %u bytes\n", m_frames, static_cast<uint32_t>(bytes));

    m_bufferMutex.unlock();
}

bool Stream::captureFrameRaw(uint8_t *outPtr, uint32_t outBytes, uint32_t *actualBytes)
{
    if (!m_isOpen) return false;
    if (!m_rawMode) {
        LOG(LOG_ERR, "captureFrameRaw called on non-raw stream\n");
        return false;
    }
    if (actualBytes == nullptr || outPtr == nullptr) {
        return false;
    }

    m_bufferMutex.lock();
    *actualBytes = m_rawFrameSize;

    if (outBytes < m_rawFrameSize) {
        LOG(LOG_WARNING,
            "captureFrameRaw: caller buffer too small (%u < %u needed)\n",
            outBytes, m_rawFrameSize);
        m_bufferMutex.unlock();
        return false;
    }

    memcpy(outPtr, &m_rawBuffer[0], m_rawFrameSize);
    m_newFrame = false;
    LOG(LOG_DEBUG, "captureFrameRaw: delivering %u bytes to caller\n", m_rawFrameSize);
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
        LOG(LOG_ERR, "decodeFrame called on non-raw stream\n");
        return false;
    }
    if (RGBbufferPtr == nullptr) return false;

    uint32_t needed = m_width * m_height * 3;
    if (RGBbufferBytes < needed) {
        LOG(LOG_WARNING,
            "decodeFrame: RGB buffer too small (%u < %u needed)\n",
            RGBbufferBytes, needed);
        return false;
    }

    m_bufferMutex.lock();
    if (m_rawFrameSize == 0) {
        LOG(LOG_ERR, "decodeFrame: no raw frame available yet\n");
        m_bufferMutex.unlock();
        return false;
    }

    LOG(LOG_INFO,
        "decodeFrame: decompressing %u JPEG bytes -> %ux%u RGB\n",
        m_rawFrameSize, m_width, m_height);

    bool ok = m_mjpegHelper.decompressFrame(
        &m_rawBuffer[0], m_rawFrameSize,
        RGBbufferPtr, m_width, m_height);

    if (!ok) {
        LOG(LOG_ERR, "decodeFrame: libjpeg-turbo failed on %u byte frame\n", m_rawFrameSize);
    }

    m_bufferMutex.unlock();
    return ok;
}
