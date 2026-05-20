/*

    OpenPnp-Capture: a video capture subsystem.

    Windows Stream class

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

#include "platformdeviceinfo.h"
#include "platformstream.h"
#include "platformcontext.h"
#include "scopedcomptr.h"

#include <climits>
#include <cmath>
#include <cstdio>

extern HRESULT FindCaptureDevice(IBaseFilter** ppSrcFilter, const wchar_t* wDeviceName);
extern void _FreeMediaType(AM_MEDIA_TYPE& mt);

// Delete a media type structure that was allocated on the heap.
extern void _DeleteMediaType(AM_MEDIA_TYPE *pmt);


Stream* createPlatformStream()
{
    return new PlatformStream();
}

// **********************************************************************
//   Property translation data
// **********************************************************************

struct property_t
{
    uint32_t dsProp;            // Directshow CameraControlProperty or VideoProcAmpProperty
    bool     isCameraControl;   // if true dsProp is CameraControlProperty
};

// the order must be the same as the CAPPROPID indeces!
static const property_t gs_properties[] =
{
    {0, true},                      // dummy
    {CameraControl_Exposure, true}, // exposure
    {CameraControl_Focus, true},
    {CameraControl_Zoom, true},
    {VideoProcAmp_WhiteBalance, false},
    {VideoProcAmp_Gain, false},
    {VideoProcAmp_Brightness, false},
    {VideoProcAmp_Contrast, false},
    {VideoProcAmp_Saturation, false},
    {VideoProcAmp_Gamma, false},
    {VideoProcAmp_Hue, false},
    {VideoProcAmp_Sharpness, false},
    {VideoProcAmp_BacklightCompensation, false},
    {KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY, false}
};


// **********************************************************************
//   StreamCallbackHandler
// **********************************************************************

HRESULT __stdcall StreamCallbackHandler::SampleCB(double time, IMediaSample* sample)
{
    if (sample == nullptr)
    {
        return S_OK;
    }

    m_callbackCounter++;

    size_t bytes = sample->GetActualDataLength();
    uint8_t *ptr;
    if ((sample->GetPointer(&ptr) == S_OK) && (m_stream != nullptr))
    {
        if (m_stream->isRawMode()) {
            m_stream->submitRawBuffer(ptr, bytes);
        } else {
            m_stream->submitBuffer(ptr, bytes);
        }
    
    }
    //sample->Release(); //who owns the IMediaSample ?!?
    return S_OK;
}



// **********************************************************************
//   PlatformStream
// **********************************************************************

PlatformStream::PlatformStream() : 
    Stream(),
    m_graph(nullptr),
    m_control(nullptr),
    m_callbackHandler(nullptr),
    m_sampleGrabberFilter(nullptr),
    m_sourceFilter(nullptr),
    m_nullRenderer(nullptr),
    m_sampleGrabber(nullptr),
    m_camControl(nullptr)
{

}

PlatformStream::~PlatformStream()
{
    close();
}

void PlatformStream::close()
{
    LOG_DEBUG("closing stream");

    #ifdef _DEBUG
    RemoveFromRot(dwRotRegister);
    #endif

    if (m_control != nullptr)
    {
        m_control->Stop();
    }

    if (m_sampleGrabberFilter != nullptr)
    {
        m_sampleGrabberFilter->Stop();
    }

    if (m_sourceFilter != nullptr)
    {
	    m_sourceFilter->Stop();
    }

    SafeRelease(&m_graph);
    SafeRelease(&m_control);
    SafeRelease(&m_capture);
    SafeRelease(&m_sourceFilter);
    SafeRelease(&m_sampleGrabberFilter);
    SafeRelease(&m_sampleGrabber);
    SafeRelease(&m_camControl);
    SafeRelease(&m_nullRenderer);
    SafeRelease(&m_videoProcAmp);

    if (m_callbackHandler != 0)
    {
        delete m_callbackHandler;
    }

    m_owner = nullptr;
    m_width = 0;
    m_height = 0;
    m_devicePath.clear();
    m_frameBuffer.resize(0);
    m_isOpen = false;
}


// ---------------------------------------------------------------------------
// Pin selection: CAPTURE (default) or PREVIEW (env-var override).
//
// CAPTURE is the correct pin for machine-vision cameras: it lets
// IAMStreamConfig::SetFormat trigger reconnect on a connected pin,
// which is how the post-Run MJPG re-apply works (see STEP 6 below).
//
// PREVIEW is offered as an escape hatch for cameras whose driver
// throttles the CAPTURE pin (e.g. old consumer webcams like the
// Microsoft LifeCam 3000).  Set OPENPNP_CAPTURE_USE_PREVIEW_PIN=1
// to opt into the PREVIEW pin path.
// ---------------------------------------------------------------------------
static const GUID& captureOrPreviewPin()
{
    static bool checked = false;
    static bool usePreview = false;
    if (!checked) {
        const char* env = getenv("OPENPNP_CAPTURE_USE_PREVIEW_PIN");
        usePreview = (env && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
        checked = true;
    }
    return usePreview ? PIN_CATEGORY_PREVIEW : PIN_CATEGORY_CAPTURE;
}

bool PlatformStream::open(Context *owner, deviceInfo *device, uint32_t width, uint32_t height,
    uint32_t fourCC, uint32_t fps)
{
    if (m_isOpen)
    {
        LOG_WARN("open() was called on an active stream.");
        close();
    }

    if (owner == nullptr)
    {
        LOG_ERROR("open() was with owner=NULL!");        
        return false;
    }

    if (device == nullptr)
    {
        LOG_ERROR("open() was with device=NULL!");
        return false;
    }

    platformDeviceInfo *dinfo = dynamic_cast<platformDeviceInfo*>(device);
    if (dinfo == NULL)
    {
        LOG_CRIT("Could not cast deviceInfo* to platformDeviceInfo*!");
        return false;
    }

    m_owner = owner;
    m_frames = 0;
    m_width = 0;
    m_height = 0;    

    // Create the filter graph object.
    HRESULT hr = CoCreateInstance (CLSID_FilterGraph, NULL, CLSCTX_INPROC, IID_IFilterGraph2, (void **) &m_graph);
    if (FAILED(hr))
    {
        LOG_ERROR("Could not create IFilterGraph2");
        return false;
    }

	//create the CaptureGraphBuilder
	hr = CoCreateInstance(CLSID_CaptureGraphBuilder2,NULL,CLSCTX_INPROC_SERVER,IID_ICaptureGraphBuilder2,(void**) &m_capture);
	if (FAILED(hr))
    {
        LOG_ERROR("Could not create ICaptureGraphBuilder2");
        return false;        
    }

    m_capture->SetFiltergraph(m_graph);

    //get the controller for the graph
	hr = m_graph->QueryInterface(IID_IMediaControl, (void**) &m_control);    
    if (FAILED(hr))
    {
        LOG_ERROR("Could not create IMediaControl");
        return false;
    }

    m_devicePath = dinfo->m_devicePath;
    m_requestedFourCC = fourCC;

    hr = FindCaptureDevice(&m_sourceFilter, dinfo->m_devicePath.c_str());
    if (hr != S_OK)
    {
        LOG_ERROR("Could not find source filter {}", PlatformContext::wstringToString(dinfo->m_devicePath).c_str());
        return false;
    }

    hr = m_graph->AddFilter(m_sourceFilter, L"Video Capture");
    if (hr != S_OK)
    {
        LOG_ERROR("Could add source filter to filter graph (HRESULT={:08X})", hr);
        return false;
    }

    //set the desired frame buffer format
    IAMStreamConfig *pConfig = NULL;
    const GUID& videoPin = captureOrPreviewPin();
    if (&videoPin == &PIN_CATEGORY_PREVIEW) {
        LOG_WARN("  [pin override] using PREVIEW pin (OPENPNP_CAPTURE_USE_PREVIEW_PIN=1). MJPEG renegotiation may be less reliable.");
    }

    hr = m_capture->FindInterface(&videoPin, 0, m_sourceFilter, IID_IAMStreamConfig, (void**)&pConfig);
    if (FAILED(hr))
    {
        LOG_ERROR("Could not create IAMStreamConfig");
        return false;
    }

    ScopedComPtr<IAMStreamConfig> streamConfig(pConfig);

    int iCount = 0, iSize = 0;
    hr = streamConfig->GetNumberOfCapabilities(&iCount, &iSize);
    if (FAILED(hr))
    {
        LOG_ERROR("Cannot retrieve device capabilities");
        return false;
    }
    else
    {
        LOG_DEBUG("PlatformStream::open() reveals {} stream capabilities", iCount);
    }

    // Check the size to make sure we pass in the correct structure.
    if (iSize == sizeof(VIDEO_STREAM_CONFIG_CAPS))
    {
        bool formatSet = false;
        LOG_TRACE("Searching for correct frame buffer mode..");
        LOG_TRACE("Looking for {} {}  {} at {} fps..", width, height,
            fourCCToString(fourCC).c_str(), fps);

        AM_MEDIA_TYPE *selectedConfig = nullptr;
        int bestFpsDistance = INT_MAX;

        // Use the video capabilities structure.
        for (int iFormat = 0; iFormat < iCount; iFormat++)
        {
            VIDEO_STREAM_CONFIG_CAPS scc;
            AM_MEDIA_TYPE *pmtConfig;
            hr = streamConfig->GetStreamCaps(iFormat, &pmtConfig, (BYTE*)&scc);
            if (SUCCEEDED(hr))
            {
                /* Examine the format, and possibly use it. */

                if ((pmtConfig->majortype == MEDIATYPE_Video) &&
                    (pmtConfig->formattype == FORMAT_VideoInfo) &&
                    (pmtConfig->cbFormat >= sizeof (VIDEOINFOHEADER)) &&
                    (pmtConfig->pbFormat != NULL))
                {
                    VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER*>(pmtConfig->pbFormat);

                    /* Note: pVih->bmiHeader.biCompression is usually the fourCC
                       except when it is equal to BI_RGB, BI_RLE8, BI_RLE4,
                       BI_BITFIELDS, BI_JPEG or BI_PNG
                    */

                    uint32_t format4CC = pVih->bmiHeader.biCompression;
                    switch(format4CC)
                    {
                    case BI_RGB:
                        format4CC = 'RGB ';
                        break;
                    }

                    uint32_t formatFps = pVih->AvgTimePerFrame > 0
                        ? (uint32_t)(10000000ULL / pVih->AvgTimePerFrame)
                        : 0;

                    LOG_TRACE("  {} x {} {} @ {} fps (AvgTimePerFrame={})", pVih->bmiHeader.biWidth,
                        pVih->bmiHeader.biHeight,
                        fourCCToString(format4CC).c_str(),
                        formatFps,
                        pVih->AvgTimePerFrame);

                    if ((pVih->bmiHeader.biWidth == width) &&
                        (pVih->bmiHeader.biHeight == height) &&
                        (format4CC == fourCC))
                    {
                        if (fps == 0)
                        {
                            // No FPS preference — take first matching format
                            selectedConfig = pmtConfig;
                            formatSet = true;
                            break;
                        }

                        // FPS-aware selection: skip formats with no timing info
                        if (pVih->AvgTimePerFrame == 0)
                        {
                            _DeleteMediaType(pmtConfig);
                            continue;
                        }

                        int distance = abs((int)formatFps - (int)fps);
                        if (distance < bestFpsDistance)
                        {
                            // New best match — free previous candidate
                            if (selectedConfig) _DeleteMediaType(selectedConfig);
                            selectedConfig = pmtConfig;
                            bestFpsDistance = distance;
                            formatSet = true;
                        }
                        else
                        {
                            _DeleteMediaType(pmtConfig);
                        }
                    }
                    else
                    {
                        _DeleteMediaType(pmtConfig);
                    }
                }
                else
                {
                    _DeleteMediaType(pmtConfig);
                }
            }
        }

        if (formatSet)
        {
            if (fps > 0 && selectedConfig)
            {
                VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER*>(selectedConfig->pbFormat);
                uint32_t oldAvg = pVih->AvgTimePerFrame;
                uint32_t oldFps = oldAvg > 0 ? (uint32_t)(10000000ULL / oldAvg) : 0;
                pVih->AvgTimePerFrame = 10000000ULL / fps;
                LOG_DEBUG("AvgTimePerFrame  {} -> {}  ({} fps -> {} fps)",
                    oldAvg, pVih->AvgTimePerFrame, oldFps, fps);
            }

            streamConfig->SetFormat(selectedConfig);
            _DeleteMediaType(selectedConfig);
            LOG_DEBUG("Capture format set!");
        }
        else
        {
            LOG_ERROR("Failed to find capture format!");
            return false;
        }
    }
    else
    {
        LOG_ERROR("Could not find video mode: VIDEO_STREAM_CONFIG_CAPS not found");
        return false;
    }


    // create camera control interface for exposure control etc . 
    m_camControl = nullptr;
    hr = m_sourceFilter->QueryInterface(IID_IAMCameraControl, (void **)&m_camControl); 
    if (hr != S_OK) 
    {
        // note: this is not an error because some cameras do not support camera control
        LOG_WARN("Could not create IAMCameraControl");
    }

    dumpCameraProperties();

    // create video processing control interface
    m_videoProcAmp = nullptr;
    hr = m_sourceFilter->QueryInterface(IID_IAMVideoProcAmp, (void **)&m_videoProcAmp); 
    if (hr != S_OK) 
    {
        // note: this is not an error, but in inconvenience
        LOG_WARN("Could not create IAMVideoProcAmp");
    }

    //create a samplegrabber filter for the device
    hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,IID_IBaseFilter, (void**)&m_sampleGrabberFilter);
    if (hr < 0)
    {
        LOG_ERROR("Could not create sample grabber filter");
        return false;        
    }

    //set mediatype on the samplegrabber
    hr = m_sampleGrabberFilter->QueryInterface(IID_ISampleGrabber, (void**)&m_sampleGrabber);
    if (hr != S_OK)
    {
        LOG_ERROR("Could not create ISampleGrabber (HRESULT={:08X})", hr);
        return false;
    }

    // generate a unique name for each sample grabber filter
    // used in the system.
    std::wstring filtername(L"SGF_");
    filtername.append(dinfo->m_filterName);
    hr = m_graph->AddFilter(m_sampleGrabberFilter, filtername.c_str());
    if (hr < 0)
    {
        LOG_ERROR("Could not add ISampleGrabber filter to graph (HRESULT={:08X})", hr);
        return false;
    }

    //set the media type
    AM_MEDIA_TYPE mt;
    memset(&mt, 0, sizeof(AM_MEDIA_TYPE));
    mt.majortype	= MEDIATYPE_Video;
    if (m_rawMode) {
        mt.subtype = MEDIASUBTYPE_MJPG;
        LOG_DEBUG("DirectShow: SampleGrabber media type set to MEDIASUBTYPE_MJPG (raw mode)");
    } else {
        mt.subtype = MEDIASUBTYPE_RGB24;
    }

    hr = m_sampleGrabber->SetMediaType(&mt);
    if (hr != S_OK)
    {
        LOG_ERROR("Could not set the samplegrabber media type to 24-bit RGB");
        return false;
    }
    
    //add the callback to the samplegrabber
    if (m_callbackHandler == nullptr)
    {
        m_callbackHandler = new StreamCallbackHandler(this);
    }
    else
    {
        m_callbackHandler->reset();
    }

    hr = m_sampleGrabber->SetCallback(m_callbackHandler,0);
    if (hr != S_OK)
    {
        LOG_ERROR("Could not set callback on sample grabber (HRESULT={:08X})", hr);
        return false;
    }       

    // See captureOrPreviewPin() above for the CAPTURE vs PREVIEW choice.
    // The Null renderer gives the video stream a sink without opening
    // a preview window on screen.

    // only create a valid NULL renderer in release builds!
    // FIXME: is this the behavior we actually want,
    //        or should we use a special define to 
    //        enable the preview window?
    #ifndef _DEBUG
    hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)(&m_nullRenderer));
    if (FAILED(hr))
    {
        LOG_WARN("Could not create a NULL renderer - using NULL ptr instead.");
    }
    else
    {
        // we need to add the filter to the graph to be able to use it.. 
        hr = m_graph->AddFilter(m_nullRenderer, L"NULLRenderer");
        if (hr < 0)
        {
            LOG_ERROR("Could not add NULL Renderer to graph (HRESULT={:08X})", hr);
            return false;
        }
    }
    #endif

    // Connect the camera's output pin to the SampleGrabber.
    // Pin selection is driven by captureOrPreviewPin():
    //   CAPTURE (default) — needed so post-Run SetFormat+Reconnect can
    //     force DirectShow to renegotiate the MJPEG media type.
    //   PREVIEW (opt-in env var) — escape hatch for cameras whose driver
    //     throttles the CAPTURE pin (e.g. LifeCam 3000 at 2.5fps).
    // This matches OpenCV's cap_dshow.cpp architecture.
    hr = m_capture->RenderStream(&videoPin, &MEDIATYPE_Video, m_sourceFilter, m_sampleGrabberFilter, m_nullRenderer);
    if (hr < 0)
    {
        LOG_ERROR("Error calling RenderStream (HRESULT={:08X})", hr);
        return false;
    }

    // =================================================================
    // STEP 4: Read back what DirectShow actually negotiated.
    //
    // RenderStream connected the camera's output pin to the
    // SampleGrabber's input pin.  During that connection, DirectShow
    // picked a media type that both sides accept.
    //
    // We read the SampleGrabber's INPUT type — this is what the camera
    // will deliver.  The important field is AvgTimePerFrame:
    //   333333 = 30 fps    500000 = 20 fps    1000000 = 10 fps
    // =================================================================
    AM_MEDIA_TYPE* info = new AM_MEDIA_TYPE();
    hr = m_sampleGrabber->GetConnectedMediaType(info);
    if ( hr == S_OK )
    {
        if (info->formattype == FORMAT_VideoInfo)
        {
            const VIDEOINFOHEADER * vi = reinterpret_cast<VIDEOINFOHEADER*>( info->pbFormat );
            m_width  = vi->bmiHeader.biWidth;
            m_height = vi->bmiHeader.biHeight;
            memcpy(&m_videoInfo, vi, sizeof(VIDEOINFOHEADER));
            uint32_t fc = vi->bmiHeader.biCompression;
            uint32_t fps_from_mt = vi->AvgTimePerFrame > 0
                ? 10000000 / vi->AvgTimePerFrame
                : 0;
            LOG_DEBUG(
                "Stream config: negotiated format %dx%d %s @ %d fps (AvgTimePerFrame=%d, bitrate=%d)\n",
                m_width, m_height,
                fourCCToString(fc).c_str(),
                fps_from_mt, vi->AvgTimePerFrame,
                vi->dwBitRate);

            m_frameBuffer.resize(m_width*m_height*3);
        }
        CoTaskMemFree( info->pbFormat );
    }
    else
    {
        LOG_ERROR(
            "Stream config: FAILED to read negotiated media type (hr=0x%08X)\n", hr);
    }
    free(info);

    // --- double-check media type before starting the graph ---
    {
        AM_MEDIA_TYPE* infoPre = new AM_MEDIA_TYPE();
        hr = m_sampleGrabber->GetConnectedMediaType(infoPre);
        if (hr == S_OK && infoPre->formattype == FORMAT_VideoInfo)
        {
            const VIDEOINFOHEADER *viPre = reinterpret_cast<VIDEOINFOHEADER*>(infoPre->pbFormat);
            uint32_t fpsPre = viPre->AvgTimePerFrame > 0
                ? 10000000 / viPre->AvgTimePerFrame
                : 0;
            LOG_DEBUG(
                "Stream config: pre-Run verification  %dx%d @ %dfps (format confirmed before graph start)\n",
                viPre->bmiHeader.biWidth, viPre->bmiHeader.biHeight, fpsPre);
            CoTaskMemFree(infoPre->pbFormat);
        }
        delete infoPre;
    }

    LOG_INFO("Stream to device {} opened", device->m_name.c_str());

    // =================================================================
    // STEP 5: Start the filter graph.
    //
    // m_control->Run() transitions every filter in the graph from
    // Stopped → Paused → Running.  At this boundary the camera driver
    // may renegotiate the output format AGAIN (this is the source of
    // the "MJPG doesn't stick" bug).  Our fix below re-applies the
    // format AFTER this transition, matching what the C++ PoC does.
    // =================================================================
    LOG_DEBUG(
        "Stream config: starting filter graph (camera begins streaming)\n");
    m_control->Run();
    LOG_DEBUG(
        "Stream config: filter graph is now running\n");

    // =================================================================
    // STEP 6: MJPG FIX — re-apply format while graph is RUNNING.
    //
    // This is the equivalent of:
    //   cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc('M','J','P','G'))
    // called AFTER the first cap.read() in the Python backend.
    //
    // Why this works: calling SetFormat on a CONNECTED capture pin
    // forces DirectShow to tear down and rebuild the pin connection,
    // negotiating the media type from scratch.  The camera hardware,
    // now running, accepts MJPG with correct timing.
    //
    // Part A: re-set FOURCC on the CAPTURE pin via IAMStreamConfig.
    // Part B: force-reconnect the pin via IFilterGraph2::Reconnect().
    // =================================================================
    LOG_DEBUG(
        "Stream config: re-negotiating MJPEG format (post-graph-start workaround for DirectShow MJPEG quirk)\n");
    {
        // --- Part A: find matching format and call SetFormat ---
        IAMStreamConfig *pConfig2 = NULL;
        hr = m_capture->FindInterface(&videoPin, 0,
            m_sourceFilter, IID_IAMStreamConfig, (void**)&pConfig2);
        LOG_DEBUG(
            "Stream config: found %s pin for format re-negotiation (IAMStreamConfig ptr=%p)\n",
                (&videoPin == &PIN_CATEGORY_PREVIEW) ? "PREVIEW" : "CAPTURE",
            (void*)pConfig2);
        if (SUCCEEDED(hr))
        {
            int iCount2 = 0, iSize2 = 0;
            pConfig2->GetNumberOfCapabilities(&iCount2, &iSize2);
            LOG_DEBUG(
                "Stream config: camera reports %d format capabilities for re-negotiation\n", iCount2);
            AM_MEDIA_TYPE *selectedConfig = nullptr;
            int bestFpsDistance = INT_MAX;
            for (int iFmt = 0; iFmt < iCount2; iFmt++)
            {
                VIDEO_STREAM_CONFIG_CAPS scc;
                AM_MEDIA_TYPE *pMt;
                if (SUCCEEDED(pConfig2->GetStreamCaps(iFmt, &pMt, (BYTE*)&scc)))
                {
                    if (pMt->majortype == MEDIATYPE_Video &&
                        pMt->formattype == FORMAT_VideoInfo &&
                        pMt->cbFormat >= sizeof(VIDEOINFOHEADER) &&
                        pMt->pbFormat != NULL)
                    {
                        VIDEOINFOHEADER *pVih = (VIDEOINFOHEADER*)pMt->pbFormat;
                        uint32_t fc = pVih->bmiHeader.biCompression;
                        if (fc == BI_RGB) fc = 'RGB ';
                        if (pVih->bmiHeader.biWidth == width &&
                            pVih->bmiHeader.biHeight == height && fc == fourCC)
                        {
                            if (fps == 0)
                            {
                                // No FPS preference — take first match
                                hr = pConfig2->SetFormat(pMt);
                                LOG_DEBUG(
                                    "Stream config: SetFormat(%dx%d %s) -> %s (hr=0x%08X)\n",
                                    width, height, fourCCToString(fourCC).c_str(),
                                    SUCCEEDED(hr) ? "OK" : "VFW_E_INVALIDMEDIATYPE (pin needs reconnect)", hr);
                                _DeleteMediaType(pMt);
                                break;
                            }

                            // FPS-aware: skip formats with no timing info
                            if (pVih->AvgTimePerFrame == 0)
                            {
                                _DeleteMediaType(pMt);
                                continue;
                            }

                            uint32_t formatFps = (uint32_t)(10000000ULL / pVih->AvgTimePerFrame);
                            int distance = abs((int)formatFps - (int)fps);
                            if (distance < bestFpsDistance)
                            {
                                if (selectedConfig) _DeleteMediaType(selectedConfig);
                                selectedConfig = pMt;
                                bestFpsDistance = distance;
                            }
                            else
                            {
                                _DeleteMediaType(pMt);
                            }
                        }
                        else
                        {
                            _DeleteMediaType(pMt);
                        }
                    }
                    else
                    {
                        _DeleteMediaType(pMt);
                    }
                }
            }

            if (fps > 0 && selectedConfig)
            {
                VIDEOINFOHEADER *pVih = (VIDEOINFOHEADER*)selectedConfig->pbFormat;
                uint32_t oldFps = pVih->AvgTimePerFrame > 0
                    ? (uint32_t)(10000000ULL / pVih->AvgTimePerFrame) : 0;
                pVih->AvgTimePerFrame = 10000000ULL / fps;
                hr = pConfig2->SetFormat(selectedConfig);
                LOG_DEBUG(
                    "Stream config: SetFormat(%dx%d %s @ %d fps, was %d) -> %s (hr=0x%08X)\n",
                    width, height, fourCCToString(fourCC).c_str(), fps, oldFps,
                    SUCCEEDED(hr) ? "OK" : "VFW_E_INVALIDMEDIATYPE (pin needs reconnect)", hr);
                _DeleteMediaType(selectedConfig);
            }
            pConfig2->Release();
        }

        // --- Part B: force-reconnect the connected output pin ---
        IEnumPins *enumPins = NULL;
        if (SUCCEEDED(m_sourceFilter->EnumPins(&enumPins)))
        {
            IPin *pin;
            while (enumPins->Next(1, &pin, NULL) == S_OK)
            {
                PIN_DIRECTION dir;
                IPin *connectedTo = NULL;
                if (SUCCEEDED(pin->QueryDirection(&dir)) &&
                    dir == PINDIR_OUTPUT &&
                    SUCCEEDED(pin->ConnectedTo(&connectedTo)) &&
                    connectedTo != NULL)
                {
                    LOG_DEBUG(
                        "Stream config: reconnecting output pin to re-negotiate format\n");
                    hr = m_graph->Reconnect(pin);
                    LOG_DEBUG(
                        "Stream config: Reconnect -> %s (hr=0x%08X)\n",
                        SUCCEEDED(hr) ? "OK, format re-negotiated"
                                      : "VFW_E_INVALIDMEDIATYPE (no compatible format found)", hr);
                    connectedTo->Release();
                    pin->Release();
                    break;
                }
                if (connectedTo) connectedTo->Release();
                pin->Release();
            }
            enumPins->Release();
        }
        else
        {
            LOG_ERROR(
                "Stream config: EnumPins failed, cannot reconnect output pin for format re-negotiation\n");
        }
    }

    // --- Final check: what did we end up with? ---
    {
        AM_MEDIA_TYPE* infoPost = new AM_MEDIA_TYPE();
        hr = m_sampleGrabber->GetConnectedMediaType(infoPost);
        if (hr == S_OK && infoPost->formattype == FORMAT_VideoInfo)
        {
            const VIDEOINFOHEADER *viPost = reinterpret_cast<VIDEOINFOHEADER*>(infoPost->pbFormat);
            uint32_t fpsPost = viPost->AvgTimePerFrame > 0
                ? 10000000 / viPost->AvgTimePerFrame
                : 0;
            m_width = viPost->bmiHeader.biWidth;
            m_height = viPost->bmiHeader.biHeight;
            LOG_DEBUG(
                "Stream config: final format  %dx%d @ %dfps  — ready for capture\n",
                m_width, m_height, fpsPost);
            CoTaskMemFree(infoPost->pbFormat);
        }
        delete infoPost;
    }

    // ── HARD FAIL on resolution mismatch ──────────────────────────────
    //
    // After RenderStream + MJPG fix, the camera driver may have silently
    // negotiated a different resolution than requested. This is a fatal
    // integrity error — downstream code depends on the resolution being
    // exactly what was configured. We must refuse to open the stream
    // rather than deliver unexpectedly-sized frames.
    // ──────────────────────────────────────────────────────────────────
    if (m_width != width || m_height != height) {
        LOG_ERROR(
            "  [FATAL] Resolution mismatch: requested %dx%d but DirectShow negotiated %dx%d\n"
            "  The camera driver silently overrode the requested format and the\n"
            "  MJPG-fix re-negotiation also failed. The stream will NOT open.\n",
            width, height, m_width, m_height);
        close();
        return false;
    }

	hr = m_sampleGrabberFilter->Run(0);

	hr = m_sourceFilter->Run(0);

    m_isOpen = true;
    dwRotRegister = 0;

    #ifdef _DEBUG    
    //SaveGraphFile(m_graph);
    IGraphBuilder *captureGraph;
    if (SUCCEEDED(m_capture->GetFiltergraph(&captureGraph)))
    {
        hr = AddToRot(captureGraph, &dwRotRegister);
    }
    #endif    

    return true;
}

bool PlatformStream::isDeviceConnected()
{
    if (m_devicePath.empty()) return false;

    // Re-enumerate DirectShow to check if the device is still on the bus,
    // rather than querying the cached COM filter (which survives unplug).
    IBaseFilter* pCap = nullptr;
    HRESULT hr = FindCaptureDevice(&pCap, m_devicePath.c_str());
    if (FAILED(hr) || pCap == nullptr)
    {
        return false;
    }
    pCap->Release();
    return true;
}

bool PlatformStream::setFrameRate(uint32_t fps)
{
    if (!m_isOpen || fps == 0) return false;

    const GUID& videoPin = captureOrPreviewPin();
    IAMStreamConfig *pConfig = NULL;
    HRESULT hr = m_capture->FindInterface(&videoPin, 0,
        m_sourceFilter, IID_IAMStreamConfig, (void**)&pConfig);
    if (FAILED(hr))
    {
        LOG_ERROR("setFrameRate: could not get IAMStreamConfig (hr=0x%08X)", hr);
        return false;
    }

    int iCount = 0, iSize = 0;
    pConfig->GetNumberOfCapabilities(&iCount, &iSize);

    uint32_t searchFourCC = m_requestedFourCC;

    LOG_DEBUG("setFrameRate: searching for {}x{} {} ({} caps)",
        m_width, m_height, fourCCToString(searchFourCC).c_str(), iCount);

    AM_MEDIA_TYPE *selectedConfig = nullptr;
    int bestFpsDistance = INT_MAX;

    for (int iFmt = 0; iFmt < iCount; iFmt++)
    {
        VIDEO_STREAM_CONFIG_CAPS scc;
        AM_MEDIA_TYPE *pMt;
        if (SUCCEEDED(pConfig->GetStreamCaps(iFmt, &pMt, (BYTE*)&scc)))
        {
            if (pMt->majortype == MEDIATYPE_Video &&
                pMt->formattype == FORMAT_VideoInfo &&
                pMt->cbFormat >= sizeof(VIDEOINFOHEADER) &&
                pMt->pbFormat != NULL)
            {
                VIDEOINFOHEADER *pVih = (VIDEOINFOHEADER*)pMt->pbFormat;
                uint32_t fc = pVih->bmiHeader.biCompression;
                if (fc == BI_RGB) fc = 'RGB ';
                LOG_TRACE("setFrameRate:   cap[{}] {}x{} {} (looking for {}x{} {})",
                    iFmt, pVih->bmiHeader.biWidth, pVih->bmiHeader.biHeight,
                    fourCCToString(fc).c_str(),
                    m_width, m_height, fourCCToString(searchFourCC).c_str());

                if (pVih->bmiHeader.biWidth == m_width &&
                    pVih->bmiHeader.biHeight == m_height &&
                    fc == searchFourCC)
                {
                    if (pVih->AvgTimePerFrame == 0)
                    {
                        _DeleteMediaType(pMt);
                        continue;
                    }

                    uint32_t formatFps = (uint32_t)(10000000ULL / pVih->AvgTimePerFrame);
                    int distance = abs((int)formatFps - (int)fps);
                    if (distance < bestFpsDistance)
                    {
                        if (selectedConfig) _DeleteMediaType(selectedConfig);
                        selectedConfig = pMt;
                        bestFpsDistance = distance;
                    }
                    else
                    {
                        _DeleteMediaType(pMt);
                    }
                }
                else
                {
                    _DeleteMediaType(pMt);
                }
            }
            else
            {
                _DeleteMediaType(pMt);
            }
        }
    }

    if (!selectedConfig)
    {
        LOG_ERROR("setFrameRate: no matching format for {}x{} {}",
            m_width, m_height, fourCCToString(searchFourCC).c_str());
        pConfig->Release();
        return false;
    }

    VIDEOINFOHEADER *pVih = (VIDEOINFOHEADER*)selectedConfig->pbFormat;
    uint32_t oldFps = pVih->AvgTimePerFrame > 0
        ? (uint32_t)(10000000ULL / pVih->AvgTimePerFrame) : 0;
    pVih->AvgTimePerFrame = 10000000ULL / fps;

    hr = pConfig->SetFormat(selectedConfig);
    LOG_DEBUG("setFrameRate: SetFormat({}x{} {} @ {} fps, was {}) -> {} (hr=0x{:08X})",
        m_width, m_height, fourCCToString(searchFourCC).c_str(), fps, oldFps,
        SUCCEEDED(hr) ? "OK" : "needs reconnect", hr);

    _DeleteMediaType(selectedConfig);
    pConfig->Release();

    // On a running graph SetFormat returns VFW_E_INVALIDMEDIATYPE because the
    // pin is already connected.  Try to force-reconnect the output pin —
    // this sometimes works during graph startup but rarely mid-stream.
    bool reconnected = false;
    IEnumPins *enumPins = NULL;
    if (SUCCEEDED(m_sourceFilter->EnumPins(&enumPins)) && enumPins)
    {
        IPin *pin = NULL;
        while (enumPins->Next(1, &pin, NULL) == S_OK && pin)
        {
            PIN_DIRECTION dir;
            IPin *connectedTo = NULL;
            if (SUCCEEDED(pin->QueryDirection(&dir)) &&
                dir == PINDIR_OUTPUT &&
                SUCCEEDED(pin->ConnectedTo(&connectedTo)) &&
                connectedTo != NULL)
            {
                hr = m_graph->Reconnect(pin);
                LOG_DEBUG("setFrameRate: Reconnect -> {} (hr=0x{:08X})",
                    SUCCEEDED(hr) ? "OK" : "FAILED", hr);
                reconnected = true;
                connectedTo->Release();
                pin->Release();
                break;
            }
            if (connectedTo) connectedTo->Release();
            pin->Release();
        }
        enumPins->Release();
    }

    return reconnected && SUCCEEDED(hr);
}

uint32_t PlatformStream::getFOURCC()
{
    if (!m_isOpen) return 0;

    if ((m_videoInfo.bmiHeader.biCompression == BI_RGB) || 
        (m_videoInfo.bmiHeader.biCompression == BI_BITFIELDS))
    {
        //FIXME: is this the correct 4CC?
        return MAKEFOURCC('R','G','B',' ');
    }
    else
    {
        return m_videoInfo.bmiHeader.biCompression;
    }
}

void PlatformStream::dumpCameraProperties()
{
    if (m_camControl != 0)
    {
		//query exposure
		long flags, mmin, mmax, delta, defaultValue;
        if (m_camControl->GetRange(CameraControl_Exposure, &mmin, &mmax,
            &delta, &defaultValue, &flags) == S_OK)
        {
            LOG_DEBUG("Exposure min     : {:.3f} seconds ({} integer)", std::pow(2.0f, (float)mmin), mmin);
            LOG_DEBUG("Exposure max     : {:.3f} seconds ({} integer)", std::pow(2.0f, (float)mmax), mmax);
            LOG_DEBUG("Exposure step    : {} (integer)", delta);
            LOG_DEBUG("Exposure default : {:.3f} seconds", pow(2.0f, (float)defaultValue));
            LOG_DEBUG("Flags            : {:08X}", flags);
        }
        else
        {
            LOG_WARN("Could not get exposure range information");
        }

        //query focus
        if (m_camControl->GetRange(CameraControl_Focus, &mmin, &mmax,
            &delta, &defaultValue, &flags) == S_OK)
        {
            LOG_DEBUG("Focus min     : {} integer", mmin);
            LOG_DEBUG("Focus max     : {} integer", mmax);
            LOG_DEBUG("Focus step    : {} integer", delta);
            LOG_DEBUG("Focus default : {} integer", defaultValue);
            LOG_DEBUG("Flags         : {:08X}", flags);
        }
        else
        {
            LOG_WARN("Could not get focus range information");
        }        

        // query zoom
        if (m_camControl->GetRange(CameraControl_Zoom, &mmin, &mmax,
            &delta, &defaultValue, &flags) == S_OK)
        {
            LOG_DEBUG("Zoom min     : {} integer", mmin);
            LOG_DEBUG("Zoom max     : {} integer", mmax);
            LOG_DEBUG("Zoom step    : {} integer", delta);
            LOG_DEBUG("Zoom default : {} integer", defaultValue);
            LOG_DEBUG("Flags         : {:08X}", flags);
        }
        else
        {
            LOG_WARN("Could not get Zoom range information");
        }         

#if 0
		if (m_camControl->get_Exposure(&value, &flags) == S_OK)
		{
			printf("Exposure: %2.3f seconds\n", pow(2.0f, (float)value));
			printf("Flags   : %08X\n", flags);
		}
		else
		{
			printf("Exposure info failed!\n");
		}
#endif       
    }
}


/** get the limits and default value of a camera/stream property (exposure, zoom etc) */
bool PlatformStream::getPropertyLimits(CapPropertyID propID, int32_t *emin, int32_t *emax, int32_t *dValue)
{
    if ((m_camControl == nullptr) || (emin == nullptr) || (emax == nullptr))
    {
        return false;
    }

    if (propID < CAPPROPID_LAST)
    {
        long flags, mmin, mmax, delta, defaultValue;
        if (gs_properties[propID].isCameraControl)
        {
            // use Camera control
            if (m_camControl->GetRange(gs_properties[propID].dsProp,
                    &mmin, &mmax, &delta, &defaultValue, &flags) == S_OK)
            {   
                *emin = mmin;
                *emax = mmax;
                *dValue = defaultValue;
                return true;
            }            
        }
        else
        {
            // use VideoProcAmp
            if (m_videoProcAmp == nullptr)
            {
                return false; // no VideoProcAmp on board camera
            }

            if (m_videoProcAmp->GetRange(gs_properties[propID].dsProp, 
                &mmin, &mmax, &delta, &defaultValue, &flags) == S_OK)
            {   
                *emin = mmin;
                *emax = mmax;
                *dValue = defaultValue;
                return true;
            }
        }
    }

    return false;
}


/** set property (exposure, zoom etc) of camera/stream */
bool PlatformStream::setProperty(uint32_t propID, int32_t value)
{
    if (m_camControl == nullptr)
    {
        return false;
    }


    if (propID < CAPPROPID_LAST)
    {
        long flags, dummy;
        if (gs_properties[propID].isCameraControl)
        {
            // use Camera control
            // first we get the property so we can retain the flag settings
            if (m_camControl->Get(gs_properties[propID].dsProp, &dummy, &flags) != S_OK)
            {
                return false;
            }

            // now we set the property.
            if (m_camControl->Set(gs_properties[propID].dsProp, value, flags) != S_OK)
            {
                return false;
            }

            return true;
        }
        else
        {
            // use VideoProcAmp
            if (m_videoProcAmp == nullptr)
            {
                return false; // no VideoProcAmp on board camera
            }

            // first we get the property so we can retain the flag settings
            if (m_videoProcAmp->Get(gs_properties[propID].dsProp, &dummy, &flags) != S_OK)
            {
                return false;
            }

            // now we set the property.
            if (m_videoProcAmp->Set(gs_properties[propID].dsProp, value, flags) != S_OK)
            {
                return false;
            }

            return true;
        }
    }

    return false;
}


/** set automatic state of property (exposure, zoom etc) of camera/stream */
bool PlatformStream::setAutoProperty(uint32_t propID, bool enabled)
{
    if (m_camControl == 0)
    {
        return false;
    }

    long prop = 0;
    switch(propID)
    {
    case CAPPROPID_EXPOSURE:
        prop = CameraControl_Exposure;
        break;
    case CAPPROPID_FOCUS:
        prop = CameraControl_Focus;
        break;
    case CAPPROPID_ZOOM:
        prop = CameraControl_Zoom;        
        break;
    case CAPPROPID_WHITEBALANCE:
        prop = VideoProcAmp_WhiteBalance; 
        break;
    case CAPPROPID_GAIN:
        prop = VideoProcAmp_Gain; 
        break;        
    default:
        return false;
    }

    if ((propID != CAPPROPID_WHITEBALANCE) && (propID != CAPPROPID_GAIN))
    {
        //FIXME: check return codes.
        if (enabled)
            m_camControl->Set(prop, 0, CameraControl_Flags_Auto | KSPROPERTY_CAMERACONTROL_FLAGS_RELATIVE);
        else
            m_camControl->Set(prop, 0, CameraControl_Flags_Manual | KSPROPERTY_CAMERACONTROL_FLAGS_RELATIVE);
    }
    else
    {
        //note: m_videoProcAmp only exists if the camera
        //      supports hardware accelleration of 
        //      video frame processing, such as
        //      white balance etc.
        if (m_videoProcAmp == nullptr)
        {
            return false;
        }

        // get the current value so we can just set the auto flag
        // but leave the actualy setting itself intact.
        long currentValue, flags;
        if (FAILED(m_videoProcAmp->Get(prop, &currentValue, &flags)))
        {
            return false;
        }

        //FIXME: check return codes.
        if (enabled)
            m_videoProcAmp->Set(prop, currentValue, VideoProcAmp_Flags_Auto);
        else
            m_videoProcAmp->Set(prop, currentValue, VideoProcAmp_Flags_Manual);
    }

    return true;
}

bool PlatformStream::getDSProperty(uint32_t propID, long &value, long &flags)
{
    if (m_camControl == 0)
    {
        return false;
    }

    if (propID < CAPPROPID_LAST)
    {
        if (gs_properties[propID].isCameraControl)
        {
            // use Camera control 
            if (FAILED(m_camControl->Get(gs_properties[propID].dsProp, &value, &flags)))
            {
                return false;
            }
            return true;                   
        }
        else
        {
            //note: m_videoProcAmp only exists if the camera
            //      supports hardware accelleration of 
            //      video frame processing, such as
            //      white balance etc.
            if (m_videoProcAmp == nullptr)
            {
                return false;
            }

            // get the current value so we can just set the auto flag
            // but leave the actualy setting itself intact.
            if (FAILED(m_videoProcAmp->Get(gs_properties[propID].dsProp, &value, &flags)))
            {
                return false;
            }
            return true;
        }
    }
    return false;
}

/** get property (exposure, zoom etc) of camera/stream */
bool PlatformStream::getProperty(uint32_t propID, int32_t &outValue)
{
    // in keeping with the documentation, we assume long here.. 
    // the DS documentation does not specify the actual bit-width
    // for the vars, but we use 32-bit ints in the capture lib
    // so we convert to 32-bits and hope for the best.. 

    long value, flags;
    if (PlatformStream::getDSProperty(propID, value, flags))
    {
        outValue = value;
        return true;
    }
    return false;
}

/** get automatic state of property (exposure, zoom etc) of camera/stream */
bool PlatformStream::getAutoProperty(uint32_t propID, bool &enabled)
{
    // Here, we assume that 
    // CameraControl_Flags_Auto == VideoProcAmp_Flags_Auto
    // and
    // CameraControl_Flags_Manual == VideoProcAmp_Flags_Manual
    // to simplify the code.
    // We make sure this assumption is true via a static assert
    
    static_assert(CameraControl_Flags_Auto == VideoProcAmp_Flags_Auto, "Boolean flags dont match - code change needed!");
    //static_assert(CameraControl_Flags_Manual == VideoProcAmp_Flags_Manual, "Boolean flags dont match - code change needed!");

    //LOG_TRACE("PlatformStream::getAutoProperty called");

    long value, flags;
    if (PlatformStream::getDSProperty(propID, value, flags))
    {        
        enabled = ((flags & CameraControl_Flags_Auto) != 0);
        return true;
    }
    return false;
}


void PlatformStream::submitBuffer(const uint8_t *ptr, size_t bytes)
{
    m_bufferMutex.lock();
    
    if (m_frameBuffer.size() == 0)
    {
        LOG_ERROR("Stream::m_frameBuffer size is 0 - cant store frame buffers!");
    }

    // Generate warning every 100 frames if the frame buffer is not
    // the expected size. 
    
    const uint32_t wantSize = m_width*m_height*3;
    if ((bytes != wantSize) && ((m_frames % 100) == 0))
    {
        LOG_WARN("Warning: captureFrame received incorrect buffer size (got {} want {})", bytes, wantSize);
    }

    if (bytes <= m_frameBuffer.size())
    {
        // The Win32 API delivers upside-down BGR frames.
        // Conversion to regular RGB frames is done by
        // byte-reversing the buffer
            
        for(size_t y=0; y<m_height; y++)
        {
            uint8_t *dst = &m_frameBuffer[(y*m_width)*3];
            const uint8_t *src = ptr + (m_width*3)*(m_height-y-1);
            for(uint32_t x=0; x<m_width; x++)
            {
                uint8_t b = *src++;
                uint8_t g = *src++;
                uint8_t r = *src++;
                *dst++ = r;
                *dst++ = g;
                *dst++ = b;
            }
        }

        m_newFrame = true; 
        m_frames++;        
    }

    m_bufferMutex.unlock();
}


HRESULT PlatformStream::AddToRot(IUnknown *pUnkGraph, DWORD *pdwRegister)
{
    IMoniker * pMoniker = NULL;
    IRunningObjectTable *pROT = NULL;

    if (FAILED(GetRunningObjectTable(0, &pROT))) 
    {
        LOG_DEBUG("AddToRot failed to get running object table");
        return E_FAIL;
    }
    
    const size_t STRING_LENGTH = 256;

    WCHAR wsz[STRING_LENGTH];
 
    StringCchPrintfW(
        wsz, STRING_LENGTH, 
        L"FilterGraph %08x pid %08x", 
        (DWORD_PTR)pUnkGraph, 
        GetCurrentProcessId()
    );
    
    HRESULT hr = CreateItemMoniker(L"!", wsz, &pMoniker);
    if (SUCCEEDED(hr)) 
    {
        hr = pROT->Register(ROTFLAGS_REGISTRATIONKEEPSALIVE, pUnkGraph,
            pMoniker, pdwRegister);
        pMoniker->Release();
        LOG_DEBUG("Graph registered in running object table", hr);
    }
    else
    {
        LOG_DEBUG("AddToRot failed to register graph (HRESULT={:08X})", hr);
    }
    pROT->Release();
    
    return hr;
}


void PlatformStream::RemoveFromRot(DWORD pdwRegister)
{
    IRunningObjectTable *pROT;
    if (SUCCEEDED(GetRunningObjectTable(0, &pROT))) 
    {
        pROT->Revoke(pdwRegister);
        pROT->Release();
    }
}

HRESULT PlatformStream::SaveGraphFile(IGraphBuilder *pGraph)
{
    const WCHAR wszStreamName[] = L"ActiveMovieGraph"; 
    const WCHAR wszPath[] = L"filtergraph.grf";
    HRESULT hr;
    
    IStorage *pStorage = NULL;
    hr = StgCreateDocfile(
        wszPath,
        STGM_CREATE | STGM_TRANSACTED | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
        0, &pStorage);
    if(FAILED(hr)) 
    {
        return hr;
    }

    IStream *pStream;
    hr = pStorage->CreateStream(
        wszStreamName,
        STGM_WRITE | STGM_CREATE | STGM_SHARE_EXCLUSIVE,
        0, 0, &pStream);
    if (FAILED(hr)) 
    {
        pStorage->Release();    
        return hr;
    }

    IPersistStream *pPersist = NULL;
    pGraph->QueryInterface(IID_IPersistStream, (void**)&pPersist);
    hr = pPersist->Save(pStream, TRUE);
    pStream->Release();
    pPersist->Release();
    if (SUCCEEDED(hr)) 
    {
        hr = pStorage->Commit(STGC_DEFAULT);
    }
    pStorage->Release();
    return hr;
}
