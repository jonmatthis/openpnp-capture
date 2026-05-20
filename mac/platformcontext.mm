/*

    OpenPnp-Capture: a video capture subsystem.

    OSX platform context class

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

#include "../common/logging.h"
#include "platformstream.h"
#include "platformcontext.h"
#import <AVFoundation/AVFoundation.h>

#include <chrono>
#include <thread>

// a platform factory function needed by
// libmain.cpp
Context* createPlatformContext()
{
    return new PlatformContext();
}

PlatformContext::PlatformContext() :
    Context()
{
    LOG_DEBUG( "Platform context created");
    if ([AVCaptureDevice respondsToSelector:@selector(authorizationStatusForMediaType:)]) {
        cameraPermissionReceived = 0;
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] == AVAuthorizationStatusAuthorized) {
            LOG_DEBUG( "Already have camera permission");
            cameraPermissionReceived = 1;
        }
        else {
            LOG_INFO( "Requesting permission, bundle path for Info.plist: {}", [[[NSBundle mainBundle] bundlePath] UTF8String]);
            [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
                if (granted) {
                    cameraPermissionReceived = 1;
                } else {
                    cameraPermissionReceived = -1;
                }
                if (granted) {
                    LOG_INFO( "Permission granted");
                } else {
                    LOG_WARN( "Failed to get permission");
                }
            } ];
            while (cameraPermissionReceived == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        if (cameraPermissionReceived == 1) {
            enumerateDevices();
        }
    }
    else {
        enumerateDevices();
    }
}

PlatformContext::~PlatformContext()
{
    LOG_DEBUG( "Platform context destroyed");
}

bool PlatformContext::enumerateDevices()
{
    LOG_DEBUG( "enumerateDevices called");

    m_devices.clear();
    
    NSMutableArray *deviceTypes = [NSMutableArray array];
    [deviceTypes addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
    
    if (@available(macOS 14.0, *)) {
        [deviceTypes addObject:AVCaptureDeviceTypeContinuityCamera];
        [deviceTypes addObject:AVCaptureDeviceTypeExternal];
    } else {
        [deviceTypes addObject:AVCaptureDeviceTypeExternalUnknown];
    }
    
    AVCaptureDeviceDiscoverySession *captureDeviceDiscoverySession = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:deviceTypes
                                          mediaType:AVMediaTypeVideo
                                           position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* device in [captureDeviceDiscoverySession devices])
    {
        platformDeviceInfo* deviceInfo = new platformDeviceInfo();
        deviceInfo->m_captureDevice = CFBridgingRetain(device);
        deviceInfo->m_name = std::string(device.localizedName.UTF8String) + " (" + std::string(device.manufacturer.UTF8String) + ")";
        deviceInfo->m_uniqueID = deviceInfo->m_name  + " " + std::string(device.uniqueID.UTF8String);

        std::string model = device.modelID.UTF8String;
        LOG_DEBUG( "Name : {}", deviceInfo->m_name.c_str());
        LOG_DEBUG( "Model: {}", model.c_str());
        LOG_DEBUG( "U ID : {}", deviceInfo->m_uniqueID.c_str());

        // extract the PID/VID from the model name
        NSRange vidRange = [device.modelID rangeOfString:@"VendorID_"];
        if (vidRange.length > 0)
        {
            unsigned long maxLen = device.modelID.length - vidRange.location - 9;
            maxLen = (maxLen > 5) ? 5 : maxLen;        
            deviceInfo->m_vid = [[device.modelID substringWithRange:NSMakeRange(vidRange.location + 9, maxLen)] intValue];
        }
        else
        {
            LOG_WARN( "OSX Unable to extract vendor ID");
        }
        

        NSRange pidRange = [device.modelID rangeOfString:@"ProductID_"];
        if (pidRange.length > 0)
        {
            unsigned long maxLen = device.modelID.length - pidRange.location - 10;
            maxLen = (maxLen > 5) ? 5 : maxLen;
            deviceInfo->m_pid = [[device.modelID substringWithRange:NSMakeRange(pidRange.location + 10, maxLen)] intValue];
        }
        else
        {
            LOG_WARN( "OSX Unable to extract product ID");
        }
        

        LOG_DEBUG( "USB      : vid=%04X  pid=%04X", deviceInfo->m_vid, deviceInfo->m_pid);

        // the unique ID seem to be comprised of a 10-character PCI/USB location address
        // followed by the VID and PID in hex, e.g. 0x26210000046d0825
        deviceInfo->m_busLocation = 0;
        if (device.uniqueID.length == 18)
        {
            std::string locStdStr = std::string(device.uniqueID.UTF8String);
            
            // sanity check for PID and VID to make sure the unique ID is indeed
            // in the format we expect..
            char pidStr[5];
            char vidStr[5];
            snprintf(pidStr, sizeof(pidStr), "%04x", deviceInfo->m_pid);
            snprintf(vidStr, sizeof(vidStr), "%04x", deviceInfo->m_vid);

            if ((locStdStr.substr(10,4) == std::string(vidStr)) &&
                (locStdStr.substr(14,4) == std::string(pidStr)))
            {
                // format seems to be correct
                NSString *hexString = [device.uniqueID substringWithRange:NSMakeRange(2,8)];
                NSScanner *scanner = [NSScanner scannerWithString:hexString];
                [scanner scanHexInt:&(deviceInfo->m_busLocation)];

                LOG_DEBUG( "Location : {:08X}", deviceInfo->m_busLocation);
            }
            else
            {
                LOG_DEBUG( "VID/PID mismatch!");
                LOG_DEBUG( "Extracted VID {}", locStdStr.substr(10,4).c_str());
                LOG_DEBUG( "Extracted PID {}", locStdStr.substr(14,4).c_str());
            }
        }
        
        if (deviceInfo->m_busLocation == 0)
        {
            LOG_WARN( "OSX Unique ID is not exactly 18 characters - wrong format to extract location.");
            LOG_WARN( "We might have trouble identifying the UVC control interface.");
        }

        for (AVCaptureDeviceFormat* format in device.formats) 
        {
            //Do we really need a complete list of frame rates?
            //Hopefully, we can search for a suitable frame rate
            //when we open the device later...
            //
            //This is more in line with the Windows and Linux
            //versions.
            //
            // For now, just report the max frame rate

            #if 0
            for (AVFrameRateRange* frameRateRange in format.videoSupportedFrameRateRanges) {
                for (int frameRate = frameRateRange.minFrameRate; frameRate <= frameRateRange.maxFrameRate; frameRate++) {
                    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
                    CapFormatInfo formatInfo;
                    formatInfo.width = dims.width;
                    formatInfo.height = dims.height;
                    formatInfo.fourcc = CMFormatDescriptionGetMediaSubType(format.formatDescription);
                    formatInfo.fps = frameRate;
                    deviceInfo->m_formats.push_back(formatInfo);
                }
            }
            #endif

            CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
            CapFormatInfo formatInfo;
            formatInfo.width = dims.width;
            formatInfo.height = dims.height;
            formatInfo.fourcc = CMFormatDescriptionGetMediaSubType(format.formatDescription);
            
            uint32_t maxFrameRate = 0;
            for (AVFrameRateRange* frameRateRange in format.videoSupportedFrameRateRanges) 
            {
                // find max frame rate
                if (maxFrameRate < frameRateRange.maxFrameRate)
                {
                    maxFrameRate = frameRateRange.maxFrameRate;
                }
            }
            formatInfo.fps = maxFrameRate; // just use maximum for now!
            deviceInfo->m_formats.push_back(formatInfo);
            deviceInfo->m_platformFormats.push_back(format);
        }
        
        m_devices.push_back(deviceInfo);
    }
    
    return true;
}

bool PlatformContext::isDeviceAvailable(CapDeviceID id)
{
    std::lock_guard<std::recursive_mutex> lock(m_contextMutex);

    if (id >= m_devices.size() || m_devices[id] == nullptr)
    {
        return false;
    }

    platformDeviceInfo* info = static_cast<platformDeviceInfo*>(m_devices[id]);
    AVCaptureDevice* device = (__bridge AVCaptureDevice*)info->m_captureDevice;

    if (device == nil) return false;

    if (![device isConnected])
    {
        return false;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if ([device isInUseByAnotherApplication])
    {
        LOG_INFO( "Device {} is in use by another application",
            info->m_name.c_str());
        return false;
    }
#pragma clang diagnostic pop

    return true;
}

