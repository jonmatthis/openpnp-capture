/*

    OpenPnp-Capture: a video capture subsystem.

    Linux context class to keep track of the global state.
    
    Created by Niels Moseley on 7/6/17.
    Copyright (c) 2017 Niels Moseley.

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

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string>
#include <cstring>
#include <cerrno>
#include <memory.h>
#include <linux/videodev2.h>

#include "../common/logging.h"
#include "platformstream.h"
#include "platformcontext.h"

// a platform factory function needed by
// libmain.cpp
Context* createPlatformContext()
{
    return new PlatformContext();
}

PlatformContext::PlatformContext() :
    Context()
{
    LOG_DEBUG("Context created");
    enumerateDevices();
}

PlatformContext::~PlatformContext()
{
}

bool PlatformContext::enumerateDevices()
{
    int fd;
    v4l2_capability  video_cap;

    LOG_DEBUG("Enumerating devices");

    const uint32_t maxDevices = 64; // FIXME: is this a sane number for linux?

    uint32_t dcount = 0;
    while(dcount < maxDevices)
    {
        char fname[100];
        snprintf(fname, sizeof(fname), "/dev/video%d", dcount++);

        if ((fd = ::open(fname, O_RDWR /* required */ | O_NONBLOCK)) == -1)
        {
            //LOG_ERROR("enumerateDevices: Can't open device %s", fname);
            continue;
        }

        if (ioctl(fd, VIDIOC_QUERYCAP, &video_cap) == -1)
        {
            ::close(fd);
            LOG_ERROR("enumerateDevices: Can't get capabilities");
            continue;
        }
        
        if ((video_cap.device_caps & V4L2_CAP_VIDEO_CAPTURE) != 0)
        {
            LOG_DEBUG("Name: '{}'", reinterpret_cast<const char*>(video_cap.card));
            LOG_DEBUG("Path: '{}'", fname);
            LOG_DEBUG("Bus : '{}'", reinterpret_cast<const char*>(video_cap.bus_info));
            LOG_DEBUG("capflags = {:08X}", video_cap.capabilities);
            LOG_DEBUG("devflags = {:08X}", video_cap.device_caps);

            if ((video_cap.device_caps & V4L2_CAP_READWRITE) != 0)
            {
                LOG_DEBUG("read/write supported");
            }
            else
            {
                LOG_DEBUG("read/write NOT supported");
            }

            if ((video_cap.device_caps & V4L2_CAP_STREAMING) != 0)
            {
                LOG_DEBUG("streaming I/O supported");
            }
            else
            {
                LOG_DEBUG("streaming I/O NOT supported");
            }

            if ((video_cap.device_caps & V4L2_CAP_ASYNCIO) != 0)
            {
                LOG_DEBUG("async I/O supported");
            }
            else
            {
                LOG_DEBUG("async I/O NOT supported");
            }   

            platformDeviceInfo* dinfo = new platformDeviceInfo();
            dinfo->m_name = std::string((const char*)video_cap.card);
            dinfo->m_devicePath = std::string(fname);
            dinfo->m_uniqueID = dinfo->m_name + " ";
            dinfo->m_uniqueID.append((const char*)video_cap.bus_info);
            
            // enumerate the frame formats
            v4l2_fmtdesc fmtdesc;
            memset(&fmtdesc, 0, sizeof(fmtdesc));
            uint32_t index = 0;
            fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            // FIXME: add FPS information
            // https://linuxtv.org/downloads/v4l-dvb-apis/uapi/v4l/vidioc-enum-frameintervals.html

            bool tryMore = true;
            while(tryMore)
            {
                fmtdesc.index = index;
            
                if (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == -1)
                {
                    tryMore = false;
                }
                else
                {
                    LOG_TRACE("Format {}", index);
                    LOG_TRACE("  FOURCC = {}", fourCCToString(fmtdesc.pixelformat).c_str());

                    // .. then we enumerate all the frame buffer sizes for that
                    // pixel format type.
                    uint32_t frmindex = 0;
                    CapFormatInfo cinfo;
                    cinfo.fourcc = fmtdesc.pixelformat;
                    while(queryFrameSize(fd, frmindex, fmtdesc.pixelformat, &cinfo.width, &cinfo.height))
                    {
                        frmindex++;
                        cinfo.fps = findMaxFrameRate(fd, fmtdesc.pixelformat, cinfo.width, cinfo.height);
                        dinfo->m_formats.push_back(cinfo);
                        LOG_TRACE("  {} x {}", cinfo.width, cinfo.height);
                    }
                }
                index++;
            }

            m_devices.push_back(dinfo);
        }

        ::close(fd);         
    }
    return true;
}


bool PlatformContext::queryFrameSize(int fd, uint32_t index, uint32_t pixelformat, uint32_t *width, uint32_t *height)
{
    v4l2_frmsizeenum frmSize;
    memset(&frmSize, 0, sizeof(frmSize));
    frmSize.index = index;
    frmSize.pixel_format = pixelformat;
    if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmSize) != -1)
    {
        if (frmSize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
        {
            *width  = frmSize.discrete.width;
            *height = frmSize.discrete.height;
        }
        else
        {
            LOG_WARN("queryFrameSize returned non-discrete frame size!");
            *width = 0;
            *height = 0;
        }

        return true;
    }
    return false;
}

bool PlatformContext::isDeviceAvailable(CapDeviceID id)
{
    std::lock_guard<std::recursive_mutex> lock(m_contextMutex);

    if (id >= m_devices.size() || m_devices[id] == nullptr)
    {
        return false;
    }

    platformDeviceInfo* info = static_cast<platformDeviceInfo*>(m_devices[id]);

    int fd = ::open(info->m_devicePath.c_str(), O_RDWR | O_NONBLOCK);
    if (fd == -1)
    {
        if (errno == EBUSY)
        {
            LOG_INFO("Device {} is busy (EBUSY)", info->m_devicePath.c_str());
        }
        else
        {
            LOG_ERROR("Device {} cannot be opened: {}",
                info->m_devicePath.c_str(), strerror(errno));
        }
        return false;
    }

    ::close(fd);
    return true;
}

uint32_t PlatformContext::findMaxFrameRate(int fd, uint32_t pixelformat,
    uint32_t width, uint32_t height)
{
    uint32_t fps = 0;

    // now search the frame rates
    v4l2_frmivalenum ivals;
    memset(&ivals, 0, sizeof(ivals));
    ivals.pixel_format = pixelformat;
    ivals.width = width;
    ivals.height = height;
    ivals.index = 0;
    LOG_TRACE("Finding max frame rates: ");
    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &ivals) != -1)
    {
        if (ivals.type == V4L2_FRMIVAL_TYPE_DISCRETE)
        {
            LOG_TRACE("  FPS {}/{}", ivals.discrete.denominator, ivals.discrete.numerator);
            uint32_t v = ivals.discrete.denominator/ivals.discrete.numerator;
            if (fps < v)
            {
                fps = v;
            }
        }
        ivals.index++;
    }

    return fps;
}
