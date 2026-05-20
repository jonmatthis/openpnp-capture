/*

    OpenPnp-Capture: a video capture subsystem.

    Platform independent context class.

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

#include <vector>
#include <chrono>
#include <thread>
#include "context.h"
#include "logging.h"
#include "stream.h"

Context::Context() :
    m_streamCounter(0)
{
    //NOTE: derived platform dependent class must enumerate
    //      the devices here and place them in m_devices.
}

Context::~Context()
{
    // delete stream objects
    auto iter = m_streams.begin();
    while(iter != m_streams.end())
    {
        delete iter->second;
        iter++;
    }

    //delete capture devices
    auto iter2 = m_devices.begin();
    while(iter2 != m_devices.end())
    {
        delete *iter2;
        iter2++;
    }
    LOG_DEBUG("Context destroyed");
}

const char* Context::getDeviceName(CapDeviceID id) const
{
    if (id >= m_devices.size())
    {
        LOG_ERROR("Device with ID {} not found", id);
        return NULL; // no such device ID!
    }
    if (m_devices[id] == nullptr)
    {
        LOG_ERROR("Internal device pointer is NULL");
        return NULL; // device pointer is NULL!
    }
    return m_devices[id]->m_name.c_str();
}

const char* Context::getDeviceUniqueID(CapDeviceID id) const
{
    if (id >= m_devices.size())
    {
        LOG_ERROR("Device with ID {} not found", id);
        return NULL; // no such device ID!
    }
    if (m_devices[id] == nullptr)
    {
        LOG_ERROR("Internal device pointer is NULL");
        return NULL; // device pointer is NULL!
    }
    return m_devices[id]->m_uniqueID.c_str();    
}

uint32_t Context::getDeviceCount() const
{
    return static_cast<uint32_t>(m_devices.size());
}


int32_t Context::getNumFormats(CapDeviceID index) const
{
    if (index >= m_devices.size())
    {
        LOG_ERROR("Device with ID {} not found", index);
        return -1; // no such device ID!
    }
    if (m_devices[index] == nullptr)
    {
        LOG_ERROR("Internal device pointer is NULL");
        return -1; // device pointer is NULL!
    }
    return static_cast<int32_t>(m_devices[index]->m_formats.size());
}


bool Context::getFormatInfo(CapDeviceID index, CapFormatID formatID, CapFormatInfo *info) const
{
    if (index >= m_devices.size())
    {
        LOG_ERROR("Device with ID {} not found", index);
        return false; // no such device ID!
    }
    if (m_devices[index] == nullptr)
    {
        LOG_ERROR("Internal device pointer is NULL");
        return false; // device pointer is NULL!
    }
    if (formatID < m_devices[index]->m_formats.size())
    {
        *info = m_devices[index]->m_formats[formatID];
    }
    else
    {
        LOG_ERROR("Invalid format ID (got {} but max ID is {})", formatID, m_devices[index]->m_formats.size());
        return false; // invalid format ID 
    }
    return true;
}

int32_t Context::openStream(CapDeviceID id, CapFormatID formatID)
{
    deviceInfo *device = nullptr;

    if (m_devices.size() > id)
    {
        device = m_devices[id];
    }
    else
    {
        LOG_ERROR("openStream: No devices found");
        return -1;
    }

    // lookup desired format
    if (formatID >= device->m_formats.size())
    {
        LOG_ERROR("openStream: Requested format index out of range");
        return -1;        
    }

    Stream *s = createPlatformStream();

    if (!s->open(this, device, device->m_formats[formatID].width,
                 device->m_formats[formatID].height,
                 device->m_formats[formatID].fourcc,
                 device->m_formats[formatID].fps))
    {
        LOG_ERROR("Could not open stream for device {}", device->m_name.c_str());
        return -1;
    }
    int32_t streamID = storeStream(s);
    LOG_INFO("Stream opened: device=\"{}\" stream={}  {}x{}  {}  @{} fps",
             device->m_name.c_str(), streamID,
             s->getWidth(), s->getHeight(),
             fourCCToString(s->getFOURCC()).c_str(),
             device->m_formats[formatID].fps);
    return streamID;
}

int32_t Context::openStreamRaw(CapDeviceID id, CapFormatID formatID)
{
    deviceInfo *device = nullptr;

    if (m_devices.size() > id) {
        device = m_devices[id];
    } else {
        LOG_ERROR("openStreamRaw: No devices found");
        return -1;
    }

    if (formatID >= device->m_formats.size()) {
        LOG_ERROR("openStreamRaw: Requested format index out of range");
        return -1;
    }

    uint32_t fourcc = device->m_formats[formatID].fourcc;
    if (fourcc != 0x47504A4D) { // MJPG
        LOG_ERROR("openStreamRaw: Format {} is not MJPEG. Raw mode requires MJPG.",
            fourCCToString(fourcc).c_str());
        return -1;
    }

    Stream *s = createPlatformStream();

    uint32_t estimatedJPEGSize = device->m_formats[formatID].width *
                                 device->m_formats[formatID].height * 3 / 4;

    LOG_DEBUG(
        "Opening stream in RAW mode (device {}, format={}, fourcc={}, "
        "{}x{} @ {} fps), initial raw buffer capacity: {} bytes",
        device->m_name.c_str(), formatID, fourCCToString(fourcc).c_str(),
        device->m_formats[formatID].width, device->m_formats[formatID].height,
        device->m_formats[formatID].fps, estimatedJPEGSize);

    s->setRawMode(true, estimatedJPEGSize);

    if (!s->open(this, device, device->m_formats[formatID].width,
                 device->m_formats[formatID].height,
                 device->m_formats[formatID].fourcc,
                 device->m_formats[formatID].fps)) {
        LOG_ERROR("Could not open raw stream for device {}", device->m_name.c_str());
        delete s;
        return -1;
    }

    int32_t streamID = storeStream(s);
    LOG_INFO("Stream opened: device=\"{}\" stream={}  {}x{}  {}  @{} fps  (raw MJPEG)",
             device->m_name.c_str(), streamID,
             s->getWidth(), s->getHeight(),
             fourCCToString(s->getFOURCC()).c_str(),
             device->m_formats[formatID].fps);
    return streamID;
}

bool Context::closeStream(int32_t streamID)
{
    if (streamID < 0)
    {
        LOG_ERROR("closeStream was called with a negative stream ID");
        return false;
    }

    // remove and delete stream from collection
    if (!removeStream(streamID))
    {
        LOG_ERROR("could not remove stream with ID {} from m_streams.", streamID);
    }
    
    return true;
}

uint32_t Context::isOpenStream(int32_t streamID)
{
    if (streamID < 0)
    {
        LOG_ERROR("isOpenStream was called with a negative stream ID");
        return 0;
    }    

    if (m_streams.find(streamID) == m_streams.cend())
    {
        LOG_ERROR("isOpenStream was called with an invalid stream ID");
        return 0;        
    }

    return m_streams[streamID]->isOpen() ? 1 : 0;
}

bool Context::captureFrame(int32_t streamID, uint8_t *RGBbufferPtr, size_t RGBbufferBytes)
{
    if (streamID < 0)
    {
        LOG_ERROR("captureFrame was called with a negative stream ID");
        return false;
    }    

    Stream *stream = m_streams[streamID];
    if (stream == nullptr)
    {
        LOG_ERROR("hasNewFrame was called with an unknown stream ID");
        return false; 
    }
    
    return m_streams[streamID]->captureFrame(RGBbufferPtr, static_cast<uint32_t>(RGBbufferBytes));
}

bool Context::captureFrameRaw(int32_t streamID, uint8_t *jpegBufferPtr, uint32_t jpegBufferBytes, uint32_t *outBytes)
{
    if (streamID < 0) {
        LOG_ERROR("captureFrameRaw: negative stream ID");
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        LOG_ERROR("captureFrameRaw: unknown stream ID");
        return false;
    }
    return stream->captureFrameRaw(jpegBufferPtr, jpegBufferBytes, outBytes);
}

bool Context::getFrameSize(int32_t streamID, uint32_t *outBytes)
{
    if (streamID < 0) {
        LOG_ERROR("getFrameSize: negative stream ID");
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        LOG_ERROR("getFrameSize: unknown stream ID");
        return false;
    }
    return stream->getFrameSize(outBytes);
}

bool Context::decodeFrame(int32_t streamID, uint8_t *RGBbufferPtr, uint32_t RGBbufferBytes)
{
    if (streamID < 0) {
        LOG_ERROR("decodeFrame: negative stream ID");
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        LOG_ERROR("decodeFrame: unknown stream ID");
        return false;
    }
    return stream->decodeFrame(RGBbufferPtr, RGBbufferBytes);
}

bool Context::hasNewFrame(int32_t streamID)
{
    if (streamID < 0)
    {
        LOG_ERROR("hasNewFrame was called with a negative stream ID");
        return false;
    }    

    Stream *stream = m_streams[streamID];
    if (stream == nullptr)
    {
        LOG_ERROR("hasNewFrame was called with an unknown stream ID");
        return false; 
    }

    return stream->hasNewFrame();
}

uint32_t Context::getStreamFrameCount(int32_t streamID)
{
    if (streamID < 0)
    {
        LOG_ERROR("getStreamFrameCount was called with a negative stream ID");
        return 0;
    }    

    Stream *stream = m_streams[streamID];
    if (stream == nullptr)
    {
        LOG_ERROR("hasNewFrame was called with an unknown stream ID");
        return false; 
    }

    return stream->getFrameCount();
}

bool Context::getStreamResolution(int32_t streamID, uint32_t *outWidth, uint32_t *outHeight)
{
    if (streamID < 0) {
        LOG_ERROR("getStreamResolution: negative stream ID");
        return false;
    }
    Stream *stream = m_streams[streamID];
    if (stream == nullptr) {
        LOG_ERROR("getStreamResolution: unknown stream ID");
        return false;
    }
    if (outWidth != nullptr) *outWidth = stream->getWidth();
    if (outHeight != nullptr) *outHeight = stream->getHeight();
    return true;
}

bool Context::setStreamFrameRate(int32_t streamID, uint32_t fps)
{
    if (streamID < 0)
    {
        LOG_ERROR("setStreamFrameRate was called with a negative stream ID");
        return 0;
    }    

    Stream *stream = m_streams[streamID];
    if (stream == nullptr)
    {
        LOG_ERROR("setStreamFrameRate was called with an unknown stream ID");
        return false; 
    }

    return stream->setFrameRate(fps);
}

#if 0
/** Lookup a stream by ID and return a pointer
    to it if it exists. If it doesnt exist, 
    return NULL */
Stream* Context::lookupStreamByID(int32_t ID)
{
    auto it = m_streams.find(ID);
    if (it != m_streams.end())
    {
        return it->second;
    }
    return nullptr;
}
#endif

/** Store a stream pointer in the m_streams map
    and return its unique ID */
int32_t Context::storeStream(Stream *stream)
{   
    int32_t ID = m_streamCounter++; 
    m_streams.insert(std::pair<int32_t,Stream*>(ID, stream));    
    return ID;
}

/** Remove a stream from the m_streams map.
    Return true if this was successful */
bool Context::removeStream(int32_t ID)
{
    auto it = m_streams.find(ID);
    if (it != m_streams.end())
    {
        delete it->second;
        m_streams.erase(it);
        return true;
    }
    return false;
}

bool Context::getStreamPropertyLimits(int32_t streamID, uint32_t propertyID, 
        int32_t *min, int32_t *max, int32_t *dValue)
{
    Stream* stream = m_streams[streamID];
    if (stream == nullptr) return false;
    return stream->getPropertyLimits(propertyID, min, max, dValue);
}

bool Context::setStreamAutoProperty(int32_t streamID, uint32_t propertyID, bool enable)
{
    Stream* stream = m_streams[streamID];
    if (stream == nullptr) return false;
    return stream->setAutoProperty(propertyID, enable);
}

bool Context::setStreamProperty(int32_t streamID, uint32_t propertyID, int32_t value)
{
    Stream* stream = m_streams[streamID];
    if (stream == nullptr) return false;
    return stream->setProperty(propertyID, value);
}


bool Context::getStreamProperty(int32_t streamID, uint32_t propertyID, int32_t &outValue)
{
    Stream* stream = m_streams[streamID];
    if (stream == nullptr) return false;
    return stream->getProperty(propertyID, outValue);
}


bool Context::getStreamAutoProperty(int32_t streamID, uint32_t propertyID, bool &enable)
{
    Stream* stream = m_streams[streamID];
    if (stream == nullptr) return false;
    return stream->getAutoProperty(propertyID, enable);
}

bool Context::refreshDevices()
{
    std::lock_guard<std::recursive_mutex> lock(m_contextMutex);

    for (auto* d : m_devices) {
        delete d;
    }
    m_devices.clear();

    return enumerateDevices();
}

bool Context::isDeviceStillConnected(int32_t streamID)
{
    std::lock_guard<std::recursive_mutex> lock(m_contextMutex);

    auto it = m_streams.find(streamID);
    if (it == m_streams.end()) {
        return false;
    }

    Stream* stream = it->second;
    if (!stream->isOpen()) {
        return false;
    }

    return stream->isDeviceConnected();
}

bool Context::probeDevice(CapDeviceID id, CapFormatID formatID, uint32_t timeoutMs)
{
    if (timeoutMs == 0) timeoutMs = 2000;

    int32_t streamID = openStream(id, formatID);
    if (streamID < 0) {
        LOG_DEBUG("probeDevice: openStream failed for device {}", id);
        return false;
    }

    LOG_DEBUG("probeDevice: opened device {}, waiting for frame (timeout={}ms)", id, timeoutMs);

    uint32_t waited = 0;
    const uint32_t pollInterval = 50;
    bool gotFrame = false;
    while (waited < timeoutMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollInterval));
        waited += pollInterval;
        if (hasNewFrame(streamID)) {
            gotFrame = true;
            break;
        }
    }

    if (gotFrame) {
        LOG_DEBUG("probeDevice: device {} delivered frame after {}ms", id, waited);
    } else {
        LOG_DEBUG("probeDevice: device {} produced no frame within {}ms", id, timeoutMs);
    }

    closeStream(streamID);
    return gotFrame;
}

/** convert a FOURCC uint32_t to human readable form */
std::string fourCCToString(uint32_t fourcc)
{
    std::string v;
    for(uint32_t i=0; i<4; i++)
    {
        v += static_cast<char>(fourcc & 0xFF);
        fourcc >>= 8;
    }
    return v;
};
