/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/MrKepzie/openfx-io>,
 * Copyright (C) 2015 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX ffmpegReader plugin.
 * Reads a video input file using the libav library.
 */

#if (defined(_STDINT_H) || defined(_STDINT_H_) || defined(_MSC_STDINT_H_)) && !defined(UINT64_C)
#warning "__STDC_CONSTANT_MACROS has to be defined before including <stdint.h>, this file will probably not compile."
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS // ...or stdint.h wont' define UINT64_C, needed by libavutil
#endif

#include <cmath>
#include <sstream>
#include <algorithm>

#include "IOUtility.h"

#include "GenericOCIO.h"
#include "GenericReader.h"
#include "FFmpegFile.h"
#include "ofxsCopier.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "ReadFFmpeg"
#define kPluginGrouping "Image/Readers"
#define kPluginDescription "Read video using FFmpeg."
#define kPluginIdentifier "fr.inria.openfx.ReadFFmpeg"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.
#define kPluginEvaluation 0

#define kParamMaxRetries "maxRetries"
#define kParamMaxRetriesLabel "Max retries per frame"
#define kParamMaxRetriesHint "Some video files are sometimes tricky to read and needs several retries before successfully decoding a frame. This" \
" parameter controls how many times we should attempt to decode the same frame before failing. "

#define kSupportsRGBA true
#define kSupportsRGB true
#define kSupportsAlpha false
#define kSupportsTiles false


class ReadFFmpegPlugin : public GenericReaderPlugin
{
    
    FFmpegFileManager& _manager;
    OFX::IntParam *_maxRetries;
    
public:

    ReadFFmpegPlugin(FFmpegFileManager& manager, OfxImageEffectHandle handle, const std::vector<std::string>& extensions);

    virtual ~ReadFFmpegPlugin();

    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;

    bool loadNearestFrame() const;

private:

    virtual bool isVideoStream(const std::string& filename) OVERRIDE FINAL;

    virtual void onInputFileChanged(const std::string& filename, bool setColorSpace, OFX::PreMultiplicationEnum *premult, OFX::PixelComponentEnum *components, int *componentCount) OVERRIDE FINAL;

    virtual void decode(const std::string& filename, OfxTime time, int /*view*/, bool isPlayback, const OfxRectI& renderWindow, float *pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int pixelComponentCount, int rowBytes) OVERRIDE FINAL;

    virtual bool getSequenceTimeDomain(const std::string& filename, OfxRangeI &range) OVERRIDE FINAL;

    virtual bool getFrameBounds(const std::string& filename, OfxTime time, OfxRectI *bounds, double *par, std::string *error, int* tile_width, int* tile_height) OVERRIDE FINAL;
    
    virtual bool getFrameRate(const std::string& filename, double* fps) OVERRIDE FINAL;
    
    virtual void restoreState(const std::string& filename) OVERRIDE FINAL;
    
    void convertDepthAndComponents(const void* srcPixelData,
                      const OfxRectI& renderWindow,
                      const OfxRectI& srcBounds,
                      OFX::PixelComponentEnum srcPixelComponents,
                      OFX::BitDepthEnum srcBitDepth,
                      int srcRowBytes,
                      float *dstPixelData,
                      const OfxRectI& dstBounds,
                      OFX::PixelComponentEnum dstPixelComponents,
                      int dstRowBytes);
};

ReadFFmpegPlugin::ReadFFmpegPlugin(FFmpegFileManager& manager, OfxImageEffectHandle handle, const std::vector<std::string>& extensions)
: GenericReaderPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsAlpha, kSupportsTiles, false)
, _manager(manager)
, _maxRetries(0)
{
    _maxRetries = fetchIntParam(kParamMaxRetries);
    assert(_maxRetries);
    int originalFrameRangeMin, originalFrameRangeMax;
    _originalFrameRange->getValue(originalFrameRangeMin, originalFrameRangeMax);
    if (originalFrameRangeMin == 0) {
        // probably a buggy instance from before Jan 19 2015, where 0 is the first frame
        _originalFrameRange->setValue(originalFrameRangeMin+1, originalFrameRangeMax+1);
        int timeOffset;
        _timeOffset->getValue(timeOffset);
        _timeOffset->setValue(timeOffset - 1);
    }
}

ReadFFmpegPlugin::~ReadFFmpegPlugin()
{
    
}

void
ReadFFmpegPlugin::restoreState(const std::string& /*filename*/)
{
    //_manager.getOrCreate(this, filename);
}

bool
ReadFFmpegPlugin::loadNearestFrame() const
{
    int v;
    _missingFrameParam->getValue(v);
    return v == 0;
}

void
ReadFFmpegPlugin::changedParam(const OFX::InstanceChangedArgs &args,
                               const std::string &paramName)
{
    GenericReaderPlugin::changedParam(args, paramName);
}

void
ReadFFmpegPlugin::onInputFileChanged(const std::string& filename,
                                     bool setColorSpace,
                                     OFX::PreMultiplicationEnum *premult,
                                     OFX::PixelComponentEnum *components,
                                     int *componentCount)
{
    if (setColorSpace) {
#     ifdef OFX_IO_USING_OCIO
        // Unless otherwise specified, video files are assumed to be rec709.
        if (_ocio->hasColorspace("Rec709")) {
            // nuke-default
            _ocio->setInputColorspace("Rec709");
        } else if (_ocio->hasColorspace("nuke_rec709")) {
            // blender
            _ocio->setInputColorspace("nuke_rec709");
        } else if (_ocio->hasColorspace("Rec.709 - Full")) {
            // out_rec709full or "Rec.709 - Full" in aces 1.0.0
            _ocio->setInputColorspace("Rec.709 - Full");
        } else if (_ocio->hasColorspace("out_rec709full")) {
            // out_rec709full or "Rec.709 - Full" in aces 1.0.0
            _ocio->setInputColorspace("out_rec709full");
        } else if (_ocio->hasColorspace("rrt_rec709_full_100nits")) {
            // rrt_rec709_full_100nits in aces 0.7.1
            _ocio->setInputColorspace("rrt_rec709_full_100nits");
        } else if (_ocio->hasColorspace("rrt_rec709")) {
            // rrt_rec709 in aces 0.1.1
            _ocio->setInputColorspace("rrt_rec709");
        } else if (_ocio->hasColorspace("hd10")) {
            // hd10 in spi-anim and spi-vfx
            _ocio->setInputColorspace("hd10");
        }
#     endif
    }
    assert(premult && components && componentCount);
    FFmpegFile* file = _manager.get(this, filename);
    if (!file) {
        //Clear all opened files by this plug-in since the user changed the selected file/sequence
        _manager.clear(this);
        file = _manager.getOrCreate(this, filename);
    }
    
    if (!file || file->isInvalid()) {
        if (file) {
            setPersistentMessage(OFX::Message::eMessageError, "", file->getError());
        } else {
            setPersistentMessage(OFX::Message::eMessageError, "", "Cannot open file.");
        }
        *components = OFX::ePixelComponentNone;
        *componentCount = 0;
        *premult = OFX::eImageOpaque;
        return;
    }
    *componentCount = file->getNumberOfComponents();
    *components = (*componentCount > 3) ? OFX::ePixelComponentRGBA : OFX::ePixelComponentRGB;
    ///Ffmpeg is RGB opaque.
    *premult = (*componentCount > 3) ? OFX::eImageUnPreMultiplied : OFX::eImageOpaque;
}


bool
ReadFFmpegPlugin::isVideoStream(const std::string& filename)
{
    return !FFmpegFile::isImageFile(filename);
}

template<typename SRCPIX, int srcMaxValue, int nSrcComp, int nDstComp>
class PixelConverterProcessor
: public OFX::PixelProcessor
{
    const SRCPIX* _srcPixelData;
    int _dstBufferRowBytes;
    int _srcBufferRowBytes;
    OfxRectI _srcBufferBounds;
    
public:
    // ctor
    PixelConverterProcessor(OFX::ImageEffect &instance)
    : OFX::PixelProcessor(instance)
    {
        
    }
    
    void setValues(const SRCPIX* srcPixelData,
                   OfxRectI srcBufferBounds,
                   int srcBufferRowBytes,
                   float* dstPixelData,
                   int dstBufferRowBytes,
                   OfxRectI dstBufferBounds)
    {
        _srcPixelData = srcPixelData;
        _srcBufferBounds = srcBufferBounds;
        _srcBufferRowBytes = srcBufferRowBytes;
        _dstBufferRowBytes = dstBufferRowBytes;
        
        _dstBounds = dstBufferBounds;
        _dstPixelData = dstPixelData;
    }
    
    // and do some processing
    void multiThreadProcessImages(OfxRectI procWindow)
    {
        assert(nSrcComp == 3 || nSrcComp == 4);
        assert(nDstComp == 3 || nDstComp == 4);
        
        for (int dsty = procWindow.y1; dsty < procWindow.y2; ++dsty) {
            if ( _effect.abort() ) {
                break;
            }
            
            int srcY = _dstBounds.y2 - dsty - 1;
            
            float* dst_pixels = (float*)((char*)_dstPixelData + _dstBufferRowBytes * (dsty - _dstBounds.y1))
                                         + (_dstBounds.x1 * nDstComp);
            const SRCPIX* src_pixels = (const SRCPIX*)((const char*)_srcPixelData + _srcBufferRowBytes * (srcY - _srcBufferBounds.y1))
            + (_srcBufferBounds.x1 * nSrcComp);

            
            assert(dst_pixels && src_pixels);
            
            for (int x = procWindow.x1; x < procWindow.x2; ++x) {
                
                int srcCol = x * nSrcComp ;
                int dstCol = x * nDstComp;
                dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] / (float)srcMaxValue;
                dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] / (float)srcMaxValue;
                dst_pixels[dstCol + 2] = src_pixels[srcCol + 2] / (float)srcMaxValue;
                if (nDstComp == 4) {
                    dst_pixels[dstCol + 3] = nSrcComp == 4 ? src_pixels[srcCol + 3] / (float)srcMaxValue : 1.f;
                }

            }
        }
    }
};

template<typename SRCPIX, int srcMaxValue, int nSrcComp, int nDstComp>
void
convertForDstNComps(OFX::ImageEffect* effect,
                    const SRCPIX* srcPixelData,
                    const OfxRectI& renderWindow,
                    const OfxRectI& srcBounds,
                    int srcRowBytes,
                    float *dstPixelData,
                    const OfxRectI& dstBounds,
                    int dstRowBytes)
{

    PixelConverterProcessor<SRCPIX, srcMaxValue, nSrcComp, nDstComp> p(*effect);
    p.setValues(srcPixelData, srcBounds, srcRowBytes, dstPixelData,  dstRowBytes,  dstBounds);
    p.setRenderWindow(renderWindow);
    p.process();
}

template<typename SRCPIX, int srcMaxValue, int nSrcComp>
void
convertForSrcNComps(OFX::ImageEffect* effect,
                    const SRCPIX* srcPixelData,
                    const OfxRectI& renderWindow,
                    const OfxRectI& srcBounds,
                    int srcRowBytes,
                    float *dstPixelData,
                    const OfxRectI& dstBounds,
                    OFX::PixelComponentEnum dstPixelComponents,
                    int dstRowBytes)
{
    
    switch (dstPixelComponents) {
        case OFX::ePixelComponentAlpha: {
            convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 1>(effect, srcPixelData, renderWindow, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        } break;
        case OFX::ePixelComponentRGB: {
            convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 3>(effect, srcPixelData, renderWindow, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        } break;
        case OFX::ePixelComponentRGBA: {
            convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 4>(effect, srcPixelData, renderWindow, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        } break;
        default:
            assert(false);
            break;
    }

}

template<typename SRCPIX, int srcMaxValue>
void
convertForDepth(OFX::ImageEffect* effect,
                const SRCPIX* srcPixelData,
                const OfxRectI& renderWindow,
                const OfxRectI& srcBounds,
                OFX::PixelComponentEnum srcPixelComponents,
                int srcRowBytes,
                float *dstPixelData,
                const OfxRectI& dstBounds,
                OFX::PixelComponentEnum dstPixelComponents,
                int dstRowBytes)
{
    switch (srcPixelComponents) {
        case OFX::ePixelComponentAlpha:
            convertForSrcNComps<SRCPIX, srcMaxValue, 1>(effect, srcPixelData, renderWindow, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
            break;
        case OFX::ePixelComponentRGB:
            convertForSrcNComps<SRCPIX, srcMaxValue, 3>(effect, srcPixelData, renderWindow, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
            break;
        case OFX::ePixelComponentRGBA:
            convertForSrcNComps<SRCPIX, srcMaxValue, 4>(effect, srcPixelData, renderWindow, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
            break;
        default:
            assert(false);
            break;
    }
}

void
ReadFFmpegPlugin::convertDepthAndComponents(const void* srcPixelData,
                                            const OfxRectI& renderWindow,
                                            const OfxRectI& srcBounds,
                                            OFX::PixelComponentEnum srcPixelComponents,
                                            OFX::BitDepthEnum srcBitDepth,
                                            int srcRowBytes,
                                            float *dstPixelData,
                                            const OfxRectI& dstBounds,
                                            OFX::PixelComponentEnum dstPixelComponents,
                                            int dstRowBytes)
{
    
    switch (srcBitDepth) {
        case OFX::eBitDepthFloat:
            convertForDepth<float, 1>(this, (const float*)srcPixelData, renderWindow, srcBounds, srcPixelComponents, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
            break;
        case OFX::eBitDepthUShort:
            convertForDepth<unsigned short, 65535>(this, (const unsigned short*)srcPixelData, renderWindow, srcBounds, srcPixelComponents, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
            break;
        case OFX::eBitDepthUByte:
            convertForDepth<unsigned char, 255>(this, (const unsigned char*)srcPixelData, renderWindow, srcBounds, srcPixelComponents, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
            break;
        default:
            assert(false);
            break;
    }
}

class RamBuffer
{
    unsigned char* data;
    
public:
    
    RamBuffer(std::size_t nBytes)
    : data(0)
    {
        data = (unsigned char*)malloc(nBytes);
    }
    
    unsigned char* getData() const
    {
        return data;
    }
    
    ~RamBuffer()
    {
        if (data) {
            free(data);
        }
    }
};

void
ReadFFmpegPlugin::decode(const std::string& filename,
                         OfxTime time,
                         int /*view*/,
                         bool /*isPlayback*/,
                         const OfxRectI& renderWindow,
                         float *pixelData,
                         const OfxRectI& imgBounds,
                         OFX::PixelComponentEnum pixelComponents,
                         int pixelComponentCount,
                         int rowBytes)
{
    FFmpegFile* file = _manager.getOrCreate(this, filename);
    if (file && file->isInvalid()) {
        setPersistentMessage(OFX::Message::eMessageError, "", file->getError());
        return;
    }

    /// we only support RGB or RGBA output clip
    if ((pixelComponents != OFX::ePixelComponentRGB) &&
        (pixelComponents != OFX::ePixelComponentRGBA) &&
        (pixelComponents != OFX::ePixelComponentAlpha)) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
        return;
    }
    assert((pixelComponents == OFX::ePixelComponentRGB && pixelComponentCount == 3) || (pixelComponents == OFX::ePixelComponentRGBA && pixelComponentCount == 4) || (pixelComponents == OFX::ePixelComponentAlpha && pixelComponentCount == 1));

    ///blindly ignore the filename, we suppose that the file is the same than the file loaded in the changedParam
    if (!file) {
        setPersistentMessage(OFX::Message::eMessageError, "", filename +  ": Missing frame");
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    
    int width,height,frames;
    double ap;
    file->getInfo(width, height, ap, frames);

    // wrong assert:
    // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsTiles
    //  "If a clip or plugin does not support tiled images, then the host should supply full RoD images to the effect whenever it fetches one."
    //  The renderWindow itself may or may not be the full image...
    //assert(kSupportsTiles || (renderWindow.x1 == 0 && renderWindow.x2 == width && renderWindow.y1 == 0 && renderWindow.y2 == height));

    if((imgBounds.x2 - imgBounds.x1) < width ||
       (imgBounds.y2 - imgBounds.y1) < height) {
        setPersistentMessage(OFX::Message::eMessageError, "", "The host provided an image of wrong size, can't decode.");
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    
    int maxRetries;
    _maxRetries->getValue(maxRetries);
    
    // not in FFmpeg Reader: initialize the output buffer
    // TODO: use avpicture_get_size? see WriteFFmpeg
    unsigned int numComponents = file->getNumberOfComponents();
    assert(numComponents == 3 || numComponents == 4);
    
    std::size_t sizeOfData = file->getSizeOfData();
    assert(sizeOfData == sizeof(unsigned char) || sizeOfData == sizeof(unsigned short));
    
    int srcRowBytes = width * numComponents * sizeOfData;
    std::size_t bufferSize =  height * srcRowBytes;
    
    RamBuffer bufferRaii(bufferSize);
    unsigned char* buffer = bufferRaii.getData();
    if (!buffer) {
        OFX::throwSuiteStatusException(kOfxStatErrMemory);
        return;
    }
    // this is the first stream (in fact the only one we consider for now), allocate the output buffer according to the bitdepth
    
    try {
        if ( !file->decode(this, (int)time, loadNearestFrame(), maxRetries, buffer) ) {
            if (abort()) {
                // decode() probably existed because plugin was aborted
                return;
            }
            setPersistentMessage(OFX::Message::eMessageError, "", file->getError());
            OFX::throwSuiteStatusException(kOfxStatFailed);
            return;
            
        }
    } catch (const std::exception& e) {
        int choice;
        _missingFrameParam->getValue(choice);
        if(choice == 1){ //error
            setPersistentMessage(OFX::Message::eMessageError, "", e.what());
            OFX::throwSuiteStatusException(kOfxStatFailed);
            return;
        }
        return;
    }


    convertDepthAndComponents(buffer, renderWindow, imgBounds, numComponents == 3 ? ePixelComponentRGB : ePixelComponentRGBA, sizeOfData == sizeof(unsigned char) ? eBitDepthUByte : eBitDepthUShort, srcRowBytes, pixelData, imgBounds, pixelComponents, rowBytes);

}

bool
ReadFFmpegPlugin::getSequenceTimeDomain(const std::string& filename, OfxRangeI &range)
{
    if (FFmpegFile::isImageFile(filename)) {
        range.min = range.max = 0.;
        return false;
    }

    int width,height,frames;
    double ap;
    FFmpegFile* file = _manager.getOrCreate(this, filename);
    if (!file || file->isInvalid()) {
        range.min = range.max = 0.;
        return false;
    }
    file->getInfo(width, height, ap, frames);

    range.min = 1;
    range.max = frames;
    return true;
}

bool
ReadFFmpegPlugin::getFrameRate(const std::string& filename,
                               double* fps)
{
    assert(fps);
    
    FFmpegFile* file = _manager.getOrCreate(this, filename);
    if (!file || file->isInvalid()) {
        return false;
    }

    bool gotFps = file->getFPS(*fps);
    return gotFps;
}


bool
ReadFFmpegPlugin::getFrameBounds(const std::string& filename,
                                 OfxTime /*time*/,
                                 OfxRectI *bounds,
                                 double *par,
                                 std::string *error,
                                  int* tile_width,
                                 int* tile_height)
{
    assert(bounds && par);
    FFmpegFile* file = _manager.getOrCreate(this, filename);
    if (!file || file->isInvalid()) {
        if (error && file) {
            *error = file->getError();
        }
        return false;
    }

    int width,height,frames;
    double ap;
    if (!file->getInfo(width, height, ap, frames)) {
        width = 0;
        height = 0;
        ap = 1.;
    }
    bounds->x1 = 0;
    bounds->x2 = width;
    bounds->y1 = 0;
    bounds->y2 = height;
    *par = ap;
    *tile_width = *tile_height = 0;
    return true;
}


class ReadFFmpegPluginFactory : public OFX::PluginFactoryHelper<ReadFFmpegPluginFactory>
{
    FFmpegFileManager _manager;
    
public:
    ReadFFmpegPluginFactory(const std::string& id, unsigned int verMaj, unsigned int verMin)
    : OFX::PluginFactoryHelper<ReadFFmpegPluginFactory>(id, verMaj, verMin)
    , _manager()
    {}
    
    virtual void load();
    virtual void unload() {}
    
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context);
    
    bool isVideoStreamPlugin() const { return true; }
    
    virtual void describe(OFX::ImageEffectDescriptor &desc);
    
    virtual void describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum context);

    std::vector<std::string> _extensions;
};


static std::string ffmpeg_versions()
{
    std::ostringstream oss;
#ifdef FFMS_USE_FFMPEG_COMPAT
    oss << "FFmpeg ";
#else
    oss << "libav";
#endif
    oss << " versions (compiled with / running with):" << std::endl;
    oss << "libavformat ";
    oss << LIBAVFORMAT_VERSION_MAJOR << '.' << LIBAVFORMAT_VERSION_MINOR << '.' << LIBAVFORMAT_VERSION_MICRO << " / ";
    oss << (avformat_version() >> 16) << '.' << (avformat_version() >> 8 & 0xff) << '.' << (avformat_version() & 0xff) << std::endl;
    //oss << "libavdevice ";
    //oss << LIBAVDEVICE_VERSION_MAJOR << '.' << LIBAVDEVICE_VERSION_MINOR << '.' << LIBAVDEVICE_VERSION_MICRO << " / ";
    //oss << avdevice_version() >> 16 << '.' << avdevice_version() >> 8 & 0xff << '.' << avdevice_version() & 0xff << std::endl;
    oss << "libavcodec ";
    oss << LIBAVCODEC_VERSION_MAJOR << '.' << LIBAVCODEC_VERSION_MINOR << '.' << LIBAVCODEC_VERSION_MICRO << " / ";
    oss << (avcodec_version() >> 16) << '.' << (avcodec_version() >> 8 & 0xff) << '.' << (avcodec_version() & 0xff) << std::endl;
    oss << "libavutil ";
    oss << LIBAVUTIL_VERSION_MAJOR << '.' << LIBAVUTIL_VERSION_MINOR << '.' << LIBAVUTIL_VERSION_MICRO << " / ";
    oss << (avutil_version() >> 16) << '.' << (avutil_version() >> 8 & 0xff) << '.' << (avutil_version() & 0xff) << std::endl;
    oss << "libswscale ";
    oss << LIBSWSCALE_VERSION_MAJOR << '.' << LIBSWSCALE_VERSION_MINOR << '.' << LIBSWSCALE_VERSION_MICRO << " / ";
    oss << (swscale_version() >> 16) << '.' << (swscale_version() >> 8 & 0xff) << '.' << (swscale_version() & 0xff) << std::endl;
    return oss.str();
}

#if 0
static
std::vector<std::string> &
split(const std::string &s, char delim, std::vector<std::string> &elems)
{
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}
#endif

static
std::list<std::string> &
split(const std::string &s, char delim, std::list<std::string> &elems)
{
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}


#ifdef OFX_IO_MT_FFMPEG
static int
FFmpegLockManager(void** mutex, enum AVLockOp op)
{
    switch (op) {
        case AV_LOCK_CREATE: // Create a mutex.
            try {
                *mutex = static_cast< void* >(new OFX::MultiThread::Mutex);
                return 0;
            }
            catch(...) {
                // Return error if mutex creation failed.
                return 1;
            }
            
        case AV_LOCK_OBTAIN: // Lock the specified mutex.
            try {
                static_cast< OFX::MultiThread::Mutex* >(*mutex)->lock();
                return 0;
            }
            catch(...) {
                // Return error if mutex lock failed.
                return 1;
            }
            
        case AV_LOCK_RELEASE: // Unlock the specified mutex.
            // Mutex unlock can't fail.
            static_cast< OFX::MultiThread::Mutex* >(*mutex)->unlock();
            return 0;
            
        case AV_LOCK_DESTROY: // Destroy the specified mutex.
            // Mutex destruction can't fail.
            delete static_cast< OFX::MultiThread::Mutex* >(*mutex);
            *mutex = 0;
            return 0;
            
        default: // Unknown operation.
            assert(false);
            return 1;
    }
}
#endif

void
ReadFFmpegPluginFactory::load()
{
    _extensions.clear();
#if 0
    // hard-coded extensions list
    const char* extensionsl[] = { "avi", "flv", "mov", "mp4", "mkv", "r3d", "bmp", "pix", "dpx", "exr", "jpeg", "jpg", "png", "pgm", "ppm", "ptx", "rgba", "rgb", "tiff", "tga", "gif", NULL };
    for (const char** ext = extensionsl; *ext != NULL; ++ext) {
        _extensions.push_back(*ext);
    }
#else
    std::list<std::string> extensionsl;
    AVInputFormat* iFormat = av_iformat_next(NULL);
    while (iFormat != NULL) {
        //printf("ReadFFmpeg: \"%s\", // %s (%s)\n", iFormat->extensions ? iFormat->extensions : iFormat->name, iFormat->name, iFormat->long_name);
        if (iFormat->extensions != NULL) {
            std::string extStr( iFormat->extensions );
            split(extStr, ',', extensionsl);
        }
        {
            // name's format defines (in general) extensions
            // require to fix extension in LibAV/FFMpeg to don't use it.
            std::string extStr( iFormat->name);
            split(extStr, ',', extensionsl);
        }
        iFormat = av_iformat_next( iFormat );
    }
    
    // Hack: Add basic video container extensions
    // as some versions of LibAV doesn't declare properly all extensions...
    // or there may be other well-known extensions (such as mts or m2ts)
    //extensionsl.push_back("pix"); // alias_pix (Alias/Wavefront PIX image)
    extensionsl.push_back("avi"); // AVI (Audio Video Interleaved)
    //extensionsl.push_back("bsa"); // bethsoftvid (Bethesda Softworks VID)
    //extensionsl.push_back("bik"); // bink (Bink)
    //extensionsl.push_back("cpk"); // film_cpk (Sega FILM / CPK)
    //extensionsl.push_back("cak"); // film_cpk (Sega FILM / CPK)
    //extensionsl.push_back("film"); // film_cpk (Sega FILM / CPK)
    //extensionsl.push_back("fli"); // flic (FLI/FLC/FLX animation)
    //extensionsl.push_back("flc"); // flic (FLI/FLC/FLX animation)
    //extensionsl.push_back("flx"); // flic (FLI/FLC/FLX animation)
    extensionsl.push_back("flv"); // flv (FLV (Flash Video))
    //extensionsl.push_back("ilbm"); // iff (IFF (Interchange File Format))
    //extensionsl.push_back("anim"); // iff (IFF (Interchange File Format))
    //extensionsl.push_back("mve"); // ipmovie (Interplay MVE)
    //extensionsl.push_back("lml"); // lmlm4 (raw lmlm4)
    extensionsl.push_back("mkv"); // matroska,webm (Matroska / WebM)
    extensionsl.push_back("mov"); // QuickTime / MOV
    extensionsl.push_back("mp4"); // MP4 (MPEG-4 Part 14)
    extensionsl.push_back("mpg"); // MPEG-1 Systems / MPEG program stream
    extensionsl.push_back("m2ts"); // mpegts (MPEG-TS (MPEG-2 Transport Stream))
    extensionsl.push_back("mts"); // mpegts (MPEG-TS (MPEG-2 Transport Stream))
    extensionsl.push_back("ts"); // mpegts (MPEG-TS (MPEG-2 Transport Stream))
    extensionsl.push_back("mxf"); // mxf (MXF (Material eXchange Format))

    // remove audio and subtitle-only formats
    const char* extensions_blacklist[] = {
        "aa", // aa (Audible AA format files)
        "aac", // aac (raw ADTS AAC (Advanced Audio Coding))
        "ac3", // ac3 (raw AC-3)
        "act", // act (ACT Voice file format)
        // "adf", // adf (Artworx Data Format)
        "adp", "dtk", // adp (ADP) Audio format used on the Nintendo Gamecube.
        "adx", // adx (CRI ADX) Audio-only format used in console video games.
        "aea", // aea (MD STUDIO audio)
        "afc", // afc (AFC) Audio format used on the Nintendo Gamecube.
        "aiff", // aiff (Audio IFF)
        "amr", // amr (3GPP AMR)
        // "anm", // anm (Deluxe Paint Animation)
        "apc", // apc (CRYO APC)
        "ape", "apl", "mac", // ape (Monkey's Audio)
        // "apng", // apng (Animated Portable Network Graphics)
        "aqt", "aqtitle", // aqtitle (AQTitle subtitles)
        // "asf", // asf (ASF (Advanced / Active Streaming Format))
        // "asf_o", // asf_o (ASF (Advanced / Active Streaming Format))
        "ass", // ass (SSA (SubStation Alpha) subtitle)
        "ast", // ast (AST (Audio Stream))
        "au", // au (Sun AU)
        // "avi", // avi (AVI (Audio Video Interleaved))
        "avr", // avr (AVR (Audio Visual Research)) Audio format used on Mac.
        // "avs", // avs (AVS)
        // "bethsoftvid", // bethsoftvid (Bethesda Softworks VID)
        // "bfi", // bfi (Brute Force & Ignorance)
        "bin", // bin (Binary text)
        // "bink", // bink (Bink)
        "bit", // bit (G.729 BIT file format)
        // "bmv", // bmv (Discworld II BMV)
        "bfstm", "bcstm", // bfstm (BFSTM (Binary Cafe Stream)) Audio format used on the Nintendo WiiU (based on BRSTM).
        "brstm", // brstm (BRSTM (Binary Revolution Stream)) Audio format used on the Nintendo Wii.
        "boa", // boa (Black Ops Audio)
        // "c93", // c93 (Interplay C93)
        "caf", // caf (Apple CAF (Core Audio Format))
        // "cavsvideo", // cavsvideo (raw Chinese AVS (Audio Video Standard))
        // "cdg", // cdg (CD Graphics)
        // "cdxl,xl", // cdxl (Commodore CDXL video)
        // "cine", // cine (Phantom Cine)
        // "concat", // concat (Virtual concatenation script)
        // "data", // data (raw data)
        "302", "daud", // daud (D-Cinema audio)
        // "dfa", // dfa (Chronomaster DFA)
        // "dirac", // dirac (raw Dirac)
        // "dnxhd", // dnxhd (raw DNxHD (SMPTE VC-3))
        "dsf", // dsf (DSD Stream File (DSF))
        // "dsicin", // dsicin (Delphine Software International CIN)
        "dss", // dss (Digital Speech Standard (DSS))
        "dts", // dts (raw DTS)
        "dtshd", // dtshd (raw DTS-HD)
        // "dv,dif", // dv (DV (Digital Video))
        "dvbsub", // dvbsub (raw dvbsub)
        // "dxa", // dxa (DXA)
        // "ea", // ea (Electronic Arts Multimedia)
        // "cdata", // ea_cdata (Electronic Arts cdata)
        "eac3", // eac3 (raw E-AC-3)
        "paf", "fap", "epaf", // epaf (Ensoniq Paris Audio File)
        "ffm", // ffm (FFM (FFserver live feed))
        "ffmetadata", // ffmetadata (FFmpeg metadata in text)
        // "flm", // filmstrip (Adobe Filmstrip)
        "flac", // flac (raw FLAC)
        // "flic", // flic (FLI/FLC/FLX animation)
        // "flv", // flv (FLV (Flash Video))
        // "flv", // live_flv (live RTMP FLV (Flash Video))
        // "4xm", // 4xm (4X Technologies)
        // "frm", // frm (Megalux Frame)
        "g722", "722", // g722 (raw G.722)
        "tco", "rco", "g723_1", // g723_1 (G.723.1)
        "g729", // g729 (G.729 raw format demuxer)
        // "gif", // gif (CompuServe Graphics Interchange Format (GIF))
        "gsm", // gsm (raw GSM)
        // "gxf", // gxf (GXF (General eXchange Format))
        // "h261", // h261 (raw H.261)
        // "h263", // h263 (raw H.263)
        // "h26l,h264,264,avc", // h264 (raw H.264 video)
        // "hevc,h265,265", // hevc (raw HEVC video)
        // "hls,applehttp", // hls,applehttp (Apple HTTP Live Streaming)
        // "hnm", // hnm (Cryo HNM v4)
        // "ico", // ico (Microsoft Windows ICO)
        // "idcin", // idcin (id Cinematic)
        // "idf", // idf (iCE Draw File)
        // "iff", // iff (IFF (Interchange File Format))
        "ilbc", // ilbc (iLBC storage)
        // "image2", // image2 (image2 sequence)
        "image2pipe", // image2pipe (piped image2 sequence)
        "alias_pix", // alias_pix (Alias/Wavefront PIX image)
        "brender_pix", // brender_pix (BRender PIX image)
        // "cgi", // ingenient (raw Ingenient MJPEG)
        "ipmovie", // ipmovie (Interplay MVE)
        "sf", "ircam", // ircam (Berkeley/IRCAM/CARL Sound Format)
        "iss", // iss (Funcom ISS)
        // "iv8", // iv8 (IndigoVision 8000 video)
        // "ivf", // ivf (On2 IVF)
        "jacosub", // jacosub (JACOsub subtitle format)
        "jv", // jv (Bitmap Brothers JV)
        "latm", // latm (raw LOAS/LATM)
        "lmlm4", // lmlm4 (raw lmlm4)
        "loas", // loas (LOAS AudioSyncStream)
        "lrc", // lrc (LRC lyrics)
        // "lvf", // lvf (LVF)
        // "lxf", // lxf (VR native stream (LXF))
        // "m4v", // m4v (raw MPEG-4 video)
        "mka", "mks", // "mkv,mk3d,mka,mks", // matroska,webm (Matroska / WebM)
        "mgsts", // mgsts (Metal Gear Solid: The Twin Snakes)
        "microdvd", // microdvd (MicroDVD subtitle format)
        // "mjpg,mjpeg,mpo", // mjpeg (raw MJPEG video)
        "mlp", // mlp (raw MLP)
        // "mlv", // mlv (Magic Lantern Video (MLV))
        "mm", // mm (American Laser Games MM)
        "mmf", // mmf (Yamaha SMAF)
        "m4a", // "mov,mp4,m4a,3gp,3g2,mj2", // mov,mp4,m4a,3gp,3g2,mj2 (QuickTime / MOV)
        "mp2", "mp3", "m2a", "mpa", // mp3 (MP2/3 (MPEG audio layer 2/3))
        "mpc", // mpc (Musepack)
        "mpc8", // mpc8 (Musepack SV8)
        // "mpeg", // mpeg (MPEG-PS (MPEG-2 Program Stream))
        "mpegts", // mpegts (MPEG-TS (MPEG-2 Transport Stream))
        "mpegtsraw", // mpegtsraw (raw MPEG-TS (MPEG-2 Transport Stream))
        "mpegvideo", // mpegvideo (raw MPEG video)
        // "mjpg", // mpjpeg (MIME multipart JPEG)
        "txt", "mpl2", // mpl2 (MPL2 subtitles)
        "sub", "mpsub", // mpsub (MPlayer subtitles)
        "msnwctcp", // msnwctcp (MSN TCP Webcam stream)
        // "mtv", // mtv (MTV)
        // "mv", // mv (Silicon Graphics Movie)
        // "mvi", // mvi (Motion Pixels MVI)
        // "mxf", // mxf (MXF (Material eXchange Format))
        // "mxg", // mxg (MxPEG clip)
        // "v", // nc (NC camera feed)
        "nist", "sph", "nistsphere", // nistsphere (NIST SPeech HEader REsources)
        // "nsv", // nsv (Nullsoft Streaming Video)
        // "nut", // nut (NUT)
        // "nuv", // nuv (NuppelVideo)
        // "ogg", // ogg (Ogg)
        "oma", "omg", "aa3", // oma (Sony OpenMG audio)
        // "paf", // paf (Amazing Studio Packed Animation File)
        "al", "alaw", // alaw (PCM A-law)
        "ul", "mulaw", // mulaw (PCM mu-law)
        "f64be", // f64be (PCM 64-bit floating-point big-endian)
        "f64le", // f64le (PCM 64-bit floating-point little-endian)
        "f32be", // f32be (PCM 32-bit floating-point big-endian)
        "f32le", // f32le (PCM 32-bit floating-point little-endian)
        "s32be", // s32be (PCM signed 32-bit big-endian)
        "s32le", // s32le (PCM signed 32-bit little-endian)
        "s24be", // s24be (PCM signed 24-bit big-endian)
        "s24le", // s24le (PCM signed 24-bit little-endian)
        "s16be", // s16be (PCM signed 16-bit big-endian)
        "sw", "s16le", // s16le (PCM signed 16-bit little-endian)
        "sb", "s8", // s8 (PCM signed 8-bit)
        "u32be", // u32be (PCM unsigned 32-bit big-endian)
        "u32le", // u32le (PCM unsigned 32-bit little-endian)
        "u24be", // u24be (PCM unsigned 24-bit big-endian)
        "u24le", // u24le (PCM unsigned 24-bit little-endian)
        "u16be", // u16be (PCM unsigned 16-bit big-endian)
        "uw", "u16le", // u16le (PCM unsigned 16-bit little-endian)
        "ub", "u8", // u8 (PCM unsigned 8-bit)
        "pjs", // pjs (PJS (Phoenix Japanimation Society) subtitles)
        "pmp", // pmp (Playstation Portable PMP)
        "pva", // pva (TechnoTrend PVA)
        "pvf", // pvf (PVF (Portable Voice Format))
        "qcp", // qcp (QCP)
        // "r3d", // r3d (REDCODE R3D)
        // "yuv,cif,qcif,rgb", // rawvideo (raw video)
        "rt", "realtext", // realtext (RealText subtitle format)
        "rsd", "redspark", // redspark (RedSpark)
        // "rl2", // rl2 (RL2)
        // "rm", // rm (RealMedia)
        // "roq", // roq (id RoQ)
        // "rpl", // rpl (RPL / ARMovie)
        "rsd", // rsd (GameCube RSD)
        "rso", // rso (Lego Mindstorms RSO)
        "rtp", // rtp (RTP input)
        "rtsp", // rtsp (RTSP input)
        "smi", "sami", // sami (SAMI subtitle format)
        "sap", // sap (SAP input)
        "sbg", // sbg (SBaGen binaural beats script)
        "sdp", // sdp (SDP)
        // "sdr2", // sdr2 (SDR2)
        "film_cpk", // film_cpk (Sega FILM / CPK)
        "shn", // shn (raw Shorten)
        // "vb,son", // siff (Beam Software SIFF)
        // "sln", // sln (Asterisk raw pcm)
        "smk", // smk (Smacker)
        // "mjpg", // smjpeg (Loki SDL MJPEG)
        "smush", // smush (LucasArts Smush)
        "sol", // sol (Sierra SOL)
        "sox", // sox (SoX native)
        "spdif", // spdif (IEC 61937 (compressed data in S/PDIF))
        "srt", // srt (SubRip subtitle)
        "psxstr", // psxstr (Sony Playstation STR)
        "stl", // stl (Spruce subtitle format)
        "sub", "subviewer1", // subviewer1 (SubViewer v1 subtitle format)
        "sub", "subviewer", // subviewer (SubViewer subtitle format)
        "sup", // sup (raw HDMV Presentation Graphic Stream subtitles)
        // "swf", // swf (SWF (ShockWave Flash))
        "tak", // tak (raw TAK)
        "tedcaptions", // tedcaptions (TED Talks captions)
        "thp", // thp (THP)
        "tiertexseq", // tiertexseq (Tiertex Limited SEQ)
        "tmv", // tmv (8088flex TMV)
        "thd", "truehd", // truehd (raw TrueHD)
        "tta", // tta (TTA (True Audio))
        // "txd", // txd (Renderware TeXture Dictionary)
        "ans", "art", "asc", "diz", "ice", "nfo", "txt", "vt", // tty (Tele-typewriter)
        // "vc1", // vc1 (raw VC-1)
        "vc1test", // vc1test (VC-1 test bitstream)
        // "viv", // vivo (Vivo)
        "vmd", // vmd (Sierra VMD)
        "idx", "vobsub", // vobsub (VobSub subtitle format)
        "voc", // voc (Creative Voice)
        "txt", "vplayer", // vplayer (VPlayer subtitles)
        "vqf", "vql", "vqe", // vqf (Nippon Telegraph and Telephone Corporation (NTT) TwinVQ)
        "w64", // w64 (Sony Wave64)
        "wav", // wav (WAV / WAVE (Waveform Audio))
        "wc3movie", // wc3movie (Wing Commander III movie)
        "webm_dash_manifest", // webm_dash_manifest (WebM DASH Manifest)
        "vtt", "webvtt", // webvtt (WebVTT subtitle)
        "wsaud", // wsaud (Westwood Studios audio)
        "wsvqa", // wsvqa (Westwood Studios VQA)
        "wtv", // wtv (Windows Television (WTV))
        "wv", // wv (WavPack)
        "xa", // xa (Maxis XA)
        "xbin", // xbin (eXtended BINary text (XBIN))
        "xmv", // xmv (Microsoft XMV)
        "xwma", // xwma (Microsoft xWMA)
        // "yop", // yop (Psygnosis YOP)
        // "y4m", // yuv4mpegpipe (YUV4MPEG pipe)
        "bmp_pipe", // bmp_pipe (piped bmp sequence)
        "dds_pipe", // dds_pipe (piped dds sequence)
        "dpx_pipe", // dpx_pipe (piped dpx sequence)
        "exr_pipe", // exr_pipe (piped exr sequence)
        "j2k_pipe", // j2k_pipe (piped j2k sequence)
        "jpeg_pipe", // jpeg_pipe (piped jpeg sequence)
        "jpegls_pipe", // jpegls_pipe (piped jpegls sequence)
        "pictor_pipe", // pictor_pipe (piped pictor sequence)
        "png_pipe", // png_pipe (piped png sequence)
        "qdraw_pipe", // qdraw_pipe (piped qdraw sequence)
        "sgi_pipe", // sgi_pipe (piped sgi sequence)
        "sunrast_pipe", // sunrast_pipe (piped sunrast sequence)
        "tiff_pipe", // tiff_pipe (piped tiff sequence)
        "webp_pipe", // webp_pipe (piped webp sequence)

        // OIIO and PFM extensions:
        "bmp", "cin", "dds", "dpx", "f3d", "fits", "hdr", "ico",
        "iff", "jpg", "jpe", "jpeg", "jif", "jfif", "jfi", "jp2", "j2k", "exr", "png",
        "pbm", "pgm", "ppm",
        "pfm",
        "psd", "pdd", "psb", "ptex", "rla", "sgi", "rgb", "rgba", "bw", "int", "inta", "pic", "tga", "tpic", "tif", "tiff", "tx", "env", "sm", "vsm", "zfile",

        NULL
    };
    for (const char*const* e = extensions_blacklist; *e != NULL; ++e) {
        extensionsl.remove(*e);
    }

    _extensions.assign(extensionsl.begin(), extensionsl.end());
    // sort / unique
    std::sort(_extensions.begin(), _extensions.end());
    _extensions.erase(std::unique(_extensions.begin(), _extensions.end()), _extensions.end());
#endif
}

/** @brief The basic describe function, passed a plugin descriptor */
void
ReadFFmpegPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    GenericReaderDescribe(desc, _extensions, kPluginEvaluation, kSupportsTiles, false);
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginDescription("Read images or video using "
#                             ifdef FFMS_USE_FFMPEG_COMPAT
                              "FFmpeg"
#                             else
                              "libav"
#                             endif
                              ".\n\n" + ffmpeg_versions());
#ifdef OFX_IO_MT_FFMPEG
    // Register a lock manager callback with FFmpeg, providing it the ability to use mutex locking around
    // otherwise non-thread-safe calls.
    av_lockmgr_register(FFmpegLockManager);
    desc.setRenderThreadSafety(OFX::eRenderFullySafe);
#else
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
#endif
    
    
    av_log_set_level(AV_LOG_WARNING);
    avcodec_register_all();
    av_register_all();
    
    
    _manager.init();
    
    
    // Thus effect prefers sequential render, but will still give correct results otherwise
    desc.getPropertySet().propSetInt(kOfxImageEffectInstancePropSequentialRender, 2);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
ReadFFmpegPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc,
                                           ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor *page = GenericReaderDescribeInContextBegin(desc, context, isVideoStreamPlugin(),
                                                                    kSupportsRGBA, kSupportsRGB, kSupportsAlpha, kSupportsTiles, false);
    
    {
        OFX::IntParamDescriptor *param = desc.defineIntParam(kParamMaxRetries);
        param->setLabel(kParamMaxRetriesLabel);
        param->setHint(kParamMaxRetriesHint);
        param->setAnimates(false);
        param->setDefault(10);
        param->setRange(0, 100);
        param->setDisplayRange(0, 20);
        param->setLayoutHint(OFX::eLayoutHintDivider);
        page->addChild(*param);
    }

    GenericReaderDescribeInContextEnd(desc, context, page, "rec709", "reference");
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect*
ReadFFmpegPluginFactory::createInstance(OfxImageEffectHandle handle,
                                        ContextEnum /*context*/)
{
    ReadFFmpegPlugin* ret =  new ReadFFmpegPlugin(_manager, handle, _extensions);
    ret->restoreStateFromParameters();
    return ret;
}


static ReadFFmpegPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
