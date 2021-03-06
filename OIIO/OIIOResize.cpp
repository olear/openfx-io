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
 * OIIOResize plugin.
 * Resize images using OIIO.
 */

#include <limits>
#include <algorithm>
#include <cfloat>

#include "ofxsMacros.h"

#include "OIIOGlobal.h"
GCC_DIAG_OFF(unused-parameter)
/*
 unfortunately, OpenImageIO/imagebuf.h includes OpenImageIO/thread.h,
 which includes boost/thread.hpp,
 which includes boost/system/error_code.hpp,
 which requires the library boost_system to get the symbol boost::system::system_category().

 the following define prevents including error_code.hpp, which is not used anyway.
 */
#define OPENIMAGEIO_THREAD_H
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/filter.h>
GCC_DIAG_ON(unused-parameter)

#include "ofxsProcessing.H"
#include "ofxsCopier.h"
#include "ofxsFormatResolution.h"
#include "ofxsCoords.h"

#include "IOUtility.h"

using namespace OFX;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "ResizeOIIO"
#define kPluginGrouping "Transform"
#define kPluginDescription  "Use OpenImageIO to resize images."

#define kPluginIdentifier "fr.inria.openfx.OIIOResize"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 0
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kParamType "type"
#define kParamTypeLabel "Type"
#define kParamTypeHint "Format: Converts between formats, the image is resized to fit in the target format. " \
"Size: Scales to fit into a box of a given width and height. " \
"Scale: Scales the image."
#define kParamTypeOptionFormat "Format"
#define kParamTypeOptionSize "Size"
#define kParamTypeOptionScale "Scale"

enum ResizeTypeEnum
{
    eResizeTypeFormat = 0,
    eResizeTypeSize,
    eResizeTypeScale,
};

#define kParamFormat "format"
#define kParamFormatLabel "Format"
#define kParamFormatHint "The output format"

#define kParamSize "size"
#define kParamSizeLabel "Size"
#define kParamSizeHint "The output size"

#define kParamPreservePAR "preservePAR"
#define kParamPreservePARLabel "Preserve PAR"
#define kParamPreservePARHint "Preserve Pixel Aspect Ratio (PAR). When checked, one direction will be clipped."

#define kParamScale "scale"
#define kParamScaleLabel "Scale"
#define kParamScaleHint "The scale factor to apply to the image."

#define kParamFilter "filter"
#define kParamFilterLabel "Filter"
#define kParamFilterHint "The filter used to resize. Lanczos3 is great for downscaling and blackman-harris is great for upscaling."
#define kParamFilterOptionImpulse "Impulse (no interpolation)"

#define kSrcClipChanged "srcClipChanged"

using namespace OpenImageIO;

class OIIOResizePlugin : public OFX::ImageEffect
{
public:

    OIIOResizePlugin(OfxImageEffectHandle handle);

    virtual ~OIIOResizePlugin();

    /* Override the render */
    virtual void render(const OFX::RenderArguments &args) OVERRIDE FINAL;

    /* override is identity */
    virtual bool isIdentity(const OFX::IsIdentityArguments &args, OFX::Clip * &identityClip, double &identityTime) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) OVERRIDE FINAL;
    
    virtual void changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName) OVERRIDE;

    /* override changed clip */
    //virtual void changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName) OVERRIDE FINAL;

    // override the rod call
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

    // override the roi call
    virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois) OVERRIDE FINAL;

    virtual void getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences) OVERRIDE FINAL;
private:
    
    template <typename PIX,int nComps>
    void renderInternal(const OFX::RenderArguments &args, TypeDesc srcType, const OFX::Image* srcImg, TypeDesc dstType, OFX::Image* dstImg);
    
    void fillWithBlack(OFX::PixelProcessorFilterBase & processor,
                       const OfxRectI &renderWindow,
                       void *dstPixelData,
                       const OfxRectI& dstBounds,
                       OFX::PixelComponentEnum dstPixelComponents,
                       int dstPixelComponentCount,
                       OFX::BitDepthEnum dstPixelDepth,
                       int dstRowBytes);
    
    // do not need to delete these, the ImageEffect is managing them for us
    OFX::Clip *_dstClip;
    OFX::Clip *_srcClip;

    OFX::ChoiceParam *_type;
    OFX::ChoiceParam *_format;
    OFX::ChoiceParam *_filter;
    OFX::Int2DParam *_size;
    OFX::Double2DParam *_scale;
    OFX::BooleanParam *_preservePAR;
    OFX::BooleanParam* _srcClipChanged; // set to true the first time the user connects src
};

OIIOResizePlugin::OIIOResizePlugin(OfxImageEffectHandle handle)
: OFX::ImageEffect(handle)
, _dstClip(0)
, _srcClip(0)
, _type(0)
, _format(0)
, _filter(0)
, _size(0)
, _scale(0)
, _preservePAR(0)
, _srcClipChanged(0)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    assert(_dstClip && (_dstClip->getPixelComponents() == OFX::ePixelComponentRGBA ||
                        _dstClip->getPixelComponents() == OFX::ePixelComponentRGB ||
                        _dstClip->getPixelComponents() == OFX::ePixelComponentAlpha));
    _srcClip = getContext() == OFX::eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert((!_srcClip && getContext() == OFX::eContextGenerator) ||
           (_srcClip && (_srcClip->getPixelComponents() == OFX::ePixelComponentRGBA ||
                         _srcClip->getPixelComponents() == OFX::ePixelComponentRGB ||
                         _srcClip->getPixelComponents() == OFX::ePixelComponentAlpha)));

    _type = fetchChoiceParam(kParamType);
    _format = fetchChoiceParam(kParamFormat);
    _filter = fetchChoiceParam(kParamFilter);
    _size = fetchInt2DParam(kParamSize);
    _scale = fetchDouble2DParam(kParamScale);
    _preservePAR = fetchBooleanParam(kParamPreservePAR);
    _srcClipChanged = fetchBooleanParam(kSrcClipChanged);
    
    assert(_type && _format &&  _filter && _size && _scale && _preservePAR);

    int type_i;
    _type->getValue(type_i);
    ResizeTypeEnum type = (ResizeTypeEnum)type_i;
    switch (type) {
        case eResizeTypeFormat:
            //specific output format
            _size->setIsSecret(true);
            _preservePAR->setIsSecret(true);
            _scale->setIsSecret(true);
            _format->setIsSecret(false);
            break;

        case eResizeTypeSize:
            //size
            _size->setIsSecret(false);
            _preservePAR->setIsSecret(false);
            _scale->setIsSecret(true);
            _format->setIsSecret(true);
            break;

        case eResizeTypeScale:
            //scaled
            _size->setIsSecret(true);
            _preservePAR->setIsSecret(true);
            _scale->setIsSecret(false);
            _format->setIsSecret(true);
            break;
    }
    
    initOIIOThreads();
}

OIIOResizePlugin::~OIIOResizePlugin()
{
}

/* Override the render */
void
OIIOResizePlugin::render(const OFX::RenderArguments &args)
{
    std::auto_ptr<OFX::Image> dst(_dstClip->fetchImage(args.time));
    if (!dst.get()) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    if (dst->getRenderScale().x != args.renderScale.x ||
        dst->getRenderScale().y != args.renderScale.y ||
        dst->getField() != args.fieldToRender) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OFX Host gave image with wrong scale or field properties");
        OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
    
    std::auto_ptr<const OFX::Image> src(_srcClip->fetchImage(args.time));
    if (src.get()) {
        OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
        OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
        assert(dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentRGBA ||
               dstComponents == OFX::ePixelComponentAlpha);
        
        OFX::BitDepthEnum    srcBitDepth      = src->getPixelDepth();
        OFX::PixelComponentEnum srcComponents = src->getPixelComponents();
        if (srcBitDepth != dstBitDepth || srcComponents != dstComponents) {
            OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
            return;
        }

        if (dstComponents == OFX::ePixelComponentRGBA) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    renderInternal<unsigned char, 4>(args,TypeDesc::UCHAR,src.get(),TypeDesc::UCHAR, dst.get());
                }   break;
                case OFX::eBitDepthUShort: {
                    renderInternal<unsigned short, 4>(args,TypeDesc::USHORT,src.get(),TypeDesc::USHORT, dst.get());
                }   break;
                case OFX::eBitDepthFloat: {
                    renderInternal<float, 4>(args,TypeDesc::FLOAT,src.get(),TypeDesc::FLOAT, dst.get());
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                    return;
            }
        } else if (dstComponents == OFX::ePixelComponentRGB) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    renderInternal<unsigned char, 3>(args,TypeDesc::UCHAR,src.get(),TypeDesc::UCHAR, dst.get());
                }   break;
                case OFX::eBitDepthUShort: {
                    renderInternal<unsigned short, 3>(args,TypeDesc::USHORT,src.get(),TypeDesc::USHORT, dst.get());
                }   break;
                case OFX::eBitDepthFloat: {
                    renderInternal<float, 3>(args,TypeDesc::FLOAT,src.get(),TypeDesc::FLOAT, dst.get());
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                    return;
            }
        } else {
            assert(dstComponents == OFX::ePixelComponentAlpha);
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    renderInternal<unsigned char, 1>(args,TypeDesc::UCHAR,src.get(),TypeDesc::UCHAR, dst.get());
                }   break;
                case OFX::eBitDepthUShort: {
                    renderInternal<unsigned short, 1>(args,TypeDesc::USHORT,src.get(),TypeDesc::USHORT, dst.get());
                }   break;
                case OFX::eBitDepthFloat: {
                    renderInternal<float, 1>(args,TypeDesc::FLOAT,src.get(),TypeDesc::FLOAT, dst.get());
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                    return;
            }
        }
    } else { //!src.get()
        void* dstPixelData;
        OfxRectI dstBounds;
        PixelComponentEnum dstComponents;
        BitDepthEnum dstBitDepth;
        int dstRowBytes;
        getImageData(dst.get(), &dstPixelData, &dstBounds, &dstComponents, &dstBitDepth, &dstRowBytes);
        int dstPixelComponentCount = dst->getPixelComponentCount();
        
        assert(dstComponents == OFX::ePixelComponentRGB || dstComponents == OFX::ePixelComponentRGBA ||
               dstComponents == OFX::ePixelComponentAlpha);
        
        if (dstComponents == OFX::ePixelComponentRGBA) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    BlackFiller<unsigned char> proc(*this, 4);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthUShort: {
                    BlackFiller<unsigned short> proc(*this, 4);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthFloat: {
                    BlackFiller<float> proc(*this, 4);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                    return;
            }
        } else if (dstComponents == OFX::ePixelComponentRGB) {
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    BlackFiller<unsigned char> proc(*this, 3);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthUShort: {
                    BlackFiller<unsigned short> proc(*this, 3);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthFloat: {
                    BlackFiller<float> proc(*this, 3);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                    return;
            }
        } else {
            assert(dstComponents == OFX::ePixelComponentAlpha);
            switch (dstBitDepth) {
                case OFX::eBitDepthUByte: {
                    BlackFiller<unsigned char> proc(*this, 1);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthUShort: {
                    BlackFiller<unsigned short> proc(*this, 1);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                case OFX::eBitDepthFloat: {
                    BlackFiller<float> proc(*this, 1);
                    fillWithBlack(proc, args.renderWindow, dstPixelData, dstBounds, dstComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
                }   break;
                default:
                    OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
                    return;
            }
        }
    }
}

template <typename PIX,int nComps>
void
OIIOResizePlugin::renderInternal(const OFX::RenderArguments &/*args*/,
                                 TypeDesc srcType,
                                 const OFX::Image* srcImg,
                                 TypeDesc dstType,
                                 OFX::Image* dstImg)
{
    ImageSpec srcSpec(srcType);
    const OfxRectI srcBounds = srcImg->getBounds();
    srcSpec.x = srcBounds.x1;
    srcSpec.y = srcBounds.y1;
    srcSpec.width = srcBounds.x2 - srcBounds.x1;
    srcSpec.height = srcBounds.y2 - srcBounds.y1;
    srcSpec.nchannels = nComps;
    srcSpec.full_x = srcSpec.x;
    srcSpec.full_y = srcSpec.y;
    srcSpec.full_width = srcSpec.width;
    srcSpec.full_height = srcSpec.height;
    srcSpec.default_channel_names();
    
    const ImageBuf srcBuf("src", srcSpec, const_cast<void*>(srcImg->getPixelAddress(srcBounds.x1, srcBounds.y1)));
    
    
    ///This code assumes that the dstImg has the target size hence that we don't support tiles
    const OfxRectI dstBounds = dstImg->getBounds();
    ImageSpec dstSpec(dstType);
    dstSpec.x = dstBounds.x1;
    dstSpec.y = dstBounds.y1;
    dstSpec.width = dstBounds.x2 - dstBounds.x1;
    dstSpec.height = dstBounds.y2 - dstBounds.y1;
    dstSpec.nchannels = nComps;
    dstSpec.full_x = dstSpec.x;
    dstSpec.full_y = dstSpec.y;
    dstSpec.full_width = dstSpec.width;
    dstSpec.full_height = dstSpec.height;
    dstSpec.default_channel_names();
    
    ImageBuf dstBuf("dst", dstSpec, dstImg->getPixelAddress(dstBounds.x1, dstBounds.y1));
    
    int filter;
    _filter->getValue(filter);

    if (filter == 0) {
        ///Use nearest neighboor
        if (!ImageBufAlgo::resample(dstBuf, srcBuf, /*interpolate*/false, ROI::All(), OFX::MultiThread::getNumCPUs())) {
            setPersistentMessage(OFX::Message::eMessageError, "", dstBuf.geterror());
        }
    } else {
        ///interpolate using the selected filter
        FilterDesc fd;
        Filter2D::get_filterdesc(filter - 1, &fd);
        // older versions of OIIO 1.2 don't have ImageBufAlgo::resize(dstBuf, srcBuf, fd.name, fd.width)
        float wratio = float(dstSpec.full_width) / float(srcSpec.full_width);
        float hratio = float(dstSpec.full_height) / float(srcSpec.full_height);
        float w = fd.width * std::max(1.0f, wratio);
        float h = fd.width * std::max(1.0f, hratio);
        std::auto_ptr<Filter2D> filter(Filter2D::create(fd.name, w, h));
        
        if (!ImageBufAlgo::resize(dstBuf, srcBuf, filter.get(), ROI::All(), OFX::MultiThread::getNumCPUs())) {
            setPersistentMessage(OFX::Message::eMessageError, "", dstBuf.geterror());
        }
    }
}

void
OIIOResizePlugin::fillWithBlack(OFX::PixelProcessorFilterBase & processor,
                                const OfxRectI &renderWindow,
                                void *dstPixelData,
                                const OfxRectI& dstBounds,
                                OFX::PixelComponentEnum dstPixelComponents,
                                int dstPixelComponentCount,
                                OFX::BitDepthEnum dstPixelDepth,
                                int dstRowBytes)
{
    // set the images
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstPixelDepth, dstRowBytes);
    
    // set the render window
    processor.setRenderWindow(renderWindow);
    
    // Call the base class process member, this will call the derived templated process code
    processor.process();
    
}


bool
OIIOResizePlugin::isIdentity(const OFX::IsIdentityArguments &args,
                             OFX::Clip * &identityClip,
                             double &/*identityTime*/)
{
    // must clear persistent message in isIdentity, or render() is not called by Nuke after an error
    clearPersistentMessage();

    int type_i;
    _type->getValue(type_i);
    ResizeTypeEnum type = (ResizeTypeEnum)type_i;
    switch (type) {
        case eResizeTypeFormat: {
            OfxRectD srcRoD = _srcClip->getRegionOfDefinition(args.time);
            double srcPAR = _srcClip->getPixelAspectRatio();
            int index;
            _format->getValue(index);
            double par;
            int w, h;
            getFormatResolution((OFX::EParamFormat)index, &w, &h, &par);
            if (srcPAR != par) {
                return false;
            }
            OfxPointD rsOne;
            rsOne.x = rsOne.y = 1.;
            OfxRectI srcRoDPixel;
            OFX::Coords::toPixelEnclosing(srcRoD, rsOne, srcPAR, &srcRoDPixel);
            if (srcRoDPixel.x1 == 0 && srcRoDPixel.y1 == 0 && srcRoDPixel.x2 == (int)w && srcRoD.y2 == (int)h) {
                identityClip = _srcClip;
                return true;
            }
            return false;
            break;
        }
        case eResizeTypeSize: {
            OfxRectD srcRoD = _srcClip->getRegionOfDefinition(args.time);
            double srcPAR = _srcClip->getPixelAspectRatio();
            OfxPointD rsOne;
            rsOne.x = rsOne.y = 1.;
            OfxRectI srcRoDPixel;
            OFX::Coords::toPixelEnclosing(srcRoD, rsOne, srcPAR, &srcRoDPixel);

            int w,h;
            _size->getValue(w, h);
            if (srcRoDPixel.x1 == 0 && srcRoDPixel.y1 == 0 && srcRoDPixel.x2 == w && srcRoDPixel.y2 == h) {
                identityClip = _srcClip;
                return true;
            }
            return false;
            break;
        }
        case eResizeTypeScale: {
            double sx,sy;
            _scale->getValue(sx, sy);
            if (sx == 1. && sy == 1.) {
                identityClip = _srcClip;
                return true;
            }
            return false;
            break;
        }
    }
    return false;
}


void
OIIOResizePlugin::changedParam(const OFX::InstanceChangedArgs &/*args*/,
                               const std::string &paramName)
{
    if (paramName == kParamType) {
        int type_i;
        _type->getValue(type_i);
        ResizeTypeEnum type = (ResizeTypeEnum)type_i;
        switch (type) {
            case eResizeTypeFormat:
                //specific output format
                _size->setIsSecret(true);
                _preservePAR->setIsSecret(true);
                _scale->setIsSecret(true);
                _format->setIsSecret(false);
                break;

            case eResizeTypeSize:
                //size
                _size->setIsSecret(false);
                _preservePAR->setIsSecret(false);
                _scale->setIsSecret(true);
                _format->setIsSecret(true);
                break;
                
            case eResizeTypeScale:
                //scaled
                _size->setIsSecret(true);
                _preservePAR->setIsSecret(true);
                _scale->setIsSecret(false);
                _format->setIsSecret(true);
                break;
        }
    }
}

void
OIIOResizePlugin::changedClip(const OFX::InstanceChangedArgs &args, const std::string &clipName)
{
    if (clipName == kOfxImageEffectSimpleSourceClipName && args.reason == OFX::eChangeUserEdit && !_srcClipChanged->getValue()) {
        _srcClipChanged->setValue(true);
        OfxRectD srcRod = _srcClip->getRegionOfDefinition(args.time);
        double srcpar = _srcClip->getPixelAspectRatio();

        ///Try to find a format matching the project format in which case we switch to format mode otherwise
        ///switch to size mode and set the size accordingly
        bool foundFormat = false;
        for (int i = (int)eParamFormatPCVideo; i < (int)eParamFormatSquare2k ; ++i) {
            int w, h;
            double par;
            getFormatResolution((OFX::EParamFormat)i, &w, &h, &par);
            if (w == (srcRod.x2 - srcRod.x1) && h == (srcRod.y2 - srcRod.y1) && par == srcpar) {
                _format->setValue((OFX::EParamFormat)i);
                _type->setValue((int)eResizeTypeFormat);
                foundFormat = true;
            }
        }
        _size->setValue((int)srcRod.x2 - srcRod.x1, (int)srcRod.y2 - srcRod.y1);
        if (!foundFormat) {
            _type->setValue((int)eResizeTypeSize);
        }
        
    }
}

bool
OIIOResizePlugin::getRegionOfDefinition(const OFX::RegionOfDefinitionArguments &args,
                                        OfxRectD &rod)
{
    int type_i;
    _type->getValue(type_i);
    ResizeTypeEnum type = (ResizeTypeEnum)type_i;
    switch (type) {
        case eResizeTypeFormat: {
            //specific output format
            int index;
            _format->getValue(index);
            double par;
            int w, h;
            getFormatResolution((OFX::EParamFormat)index, &w, &h, &par);
            OfxRectI rodPixel;
            rodPixel.x1 = rodPixel.y1 = 0;
            rodPixel.x2 = w;
            rodPixel.y2 = h;
            OfxPointD rsOne;
            rsOne.x = rsOne.y = 1.;
            OFX::Coords::toCanonical(rodPixel, rsOne, par, &rod);
        }   break;

        case eResizeTypeSize: {
            //size
            int w,h;
            _size->getValue(w, h);
            bool preservePar;
            _preservePAR->getValue(preservePar);
            if (preservePar) {
                OfxRectD srcRoD = _srcClip->getRegionOfDefinition(args.time);
                double srcW = srcRoD.x2 - srcRoD.x1;
                double srcH = srcRoD.y2 - srcRoD.y1 ;
                
                ///Don't crash if we were provided weird RoDs
                if (srcH < 1 || srcW < 1) {
                    return false;
                }
                if ((double)w / srcW < (double)h / srcH) {
                    ///Keep the given width, recompute the height
                    h = (int)(srcH * w / srcW);
                } else {
                    ///Keep the given height,recompute the width
                    w = (int)(srcW * h / srcH);
                }
                
            }
            rod.x1 = 0;
            rod.y1 = 0;
            rod.x2 = w;
            rod.y2 = h;
        }   break;
            
        case eResizeTypeScale: {
            //scaled
            OfxRectD srcRoD = _srcClip->getRegionOfDefinition(args.time);
            double sx,sy;
            _scale->getValue(sx, sy);
            srcRoD.x1 *= sx;
            srcRoD.y1 *= sy;
            srcRoD.x2 *= sx;
            srcRoD.y2 *= sy;
            rod.x1 = std::min(srcRoD.x1, srcRoD.x2 - 1);
            rod.x2 = std::max(srcRoD.x1 + 1, srcRoD.x2);
            rod.y1 = std::min(srcRoD.y1, srcRoD.y2 - 1);
            rod.y2 = std::max(srcRoD.y1 + 1, srcRoD.y2);
        }   break;
    }
    return true;
}

// override the roi call
void
OIIOResizePlugin::getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args,
                                       OFX::RegionOfInterestSetter &rois)
{
    if (!kSupportsTiles) {
        // The effect requires full images to render any region

        if (_srcClip && _srcClip->isConnected()) {
            OfxRectD srcRoD = _srcClip->getRegionOfDefinition(args.time);
            rois.setRegionOfInterest(*_srcClip, srcRoD);
        }
    }
}

void
OIIOResizePlugin::getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences)
{
    double par;
    int type_i;
    _type->getValue(type_i);
    ResizeTypeEnum type = (ResizeTypeEnum)type_i;
    switch (type) {
        case eResizeTypeFormat: {
            //specific output format
            int index;
            _format->getValue(index);
            int w, h;
            getFormatResolution((OFX::EParamFormat)index, &w, &h, &par);
            clipPreferences.setPixelAspectRatio(*_dstClip, par);
            break;
        }
        case eResizeTypeSize:
        case eResizeTypeScale:
            // don't change the pixel aspect ratio
            break;
    }
}


mDeclarePluginFactory(OIIOResizePluginFactory, {}, {});


/** @brief The basic describe function, passed a plugin descriptor */
void OIIOResizePluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    
   
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);

    // add supported pixel depths
    desc.addSupportedBitDepth(OFX::eBitDepthUByte);
    desc.addSupportedBitDepth(OFX::eBitDepthUShort);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    
    ///We don't support tiles: we can only resize the whole RoD at once
    desc.setSupportsTiles(kSupportsTiles);

    desc.setSupportsMultipleClipPARs(true); // plugin may setPixelAspectRatio on output clip

    ///We do support multiresolution
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    
    desc.setRenderThreadSafety(kRenderThreadSafety);
    
    ///Don't let the host multi-thread
    desc.setHostFrameThreading(true);
    
#ifdef OFX_EXTENSIONS_NUKE
    // ask the host to render all planes
    desc.setPassThroughForNotProcessedPlanes(OFX::ePassThroughLevelRenderAllRequestedPlanes);
#endif
    
    desc.setIsDeprecated(true);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void OIIOResizePluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum /*context*/)
{
    // Source clip only in the filter context
    // create the mandated source clip
    ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setSupportsTiles(kSupportsTiles);

    // make some pages and to things in
    PageParamDescriptor *page = desc.definePageParam("Controls");

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamType);
        param->setLabel(kParamTypeLabel);
        param->setHint(kParamTypeHint);
        assert(param->getNOptions() == eResizeTypeFormat);
        param->appendOption(kParamTypeOptionFormat);
        assert(param->getNOptions() == eResizeTypeSize);
        param->appendOption(kParamTypeOptionSize);
        assert(param->getNOptions() == eResizeTypeScale);
        param->appendOption(kParamTypeOptionScale);
        param->setDefault(0);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamFormat);
        param->setLabel(kParamFormatLabel);
        assert(param->getNOptions() == eParamFormatPCVideo);
        param->appendOption(kParamFormatPCVideoLabel);
        assert(param->getNOptions() == eParamFormatNTSC);
        param->appendOption(kParamFormatNTSCLabel);
        assert(param->getNOptions() == eParamFormatPAL);
        param->appendOption(kParamFormatPALLabel);
        assert(param->getNOptions() == eParamFormatHD);
        param->appendOption(kParamFormatHDLabel);
        assert(param->getNOptions() == eParamFormatNTSC169);
        param->appendOption(kParamFormatNTSC169Label);
        assert(param->getNOptions() == eParamFormatPAL169);
        param->appendOption(kParamFormatPAL169Label);
        assert(param->getNOptions() == eParamFormat1kSuper35);
        param->appendOption(kParamFormat1kSuper35Label);
        assert(param->getNOptions() == eParamFormat1kCinemascope);
        param->appendOption(kParamFormat1kCinemascopeLabel);
        assert(param->getNOptions() == eParamFormat2kSuper35);
        param->appendOption(kParamFormat2kSuper35Label);
        assert(param->getNOptions() == eParamFormat2kCinemascope);
        param->appendOption(kParamFormat2kCinemascopeLabel);
        assert(param->getNOptions() == eParamFormat4kSuper35);
        param->appendOption(kParamFormat4kSuper35Label);
        assert(param->getNOptions() == eParamFormat4kCinemascope);
        param->appendOption(kParamFormat4kCinemascopeLabel);
        assert(param->getNOptions() == eParamFormatSquare256);
        param->appendOption(kParamFormatSquare256Label);
        assert(param->getNOptions() == eParamFormatSquare512);
        param->appendOption(kParamFormatSquare512Label);
        assert(param->getNOptions() == eParamFormatSquare1k);
        param->appendOption(kParamFormatSquare1kLabel);
        assert(param->getNOptions() == eParamFormatSquare2k);
        param->appendOption(kParamFormatSquare2kLabel);
        param->setDefault(0);
        param->setHint(kParamFormatHint);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        Int2DParamDescriptor* param = desc.defineInt2DParam(kParamSize);
        param->setLabel(kParamSizeLabel);
        param->setHint(kParamSizeHint);
        param->setDefault(200, 200);
        param->setDisplayRange(0, 0, 10000, 10000);
        param->setAnimates(false);
        //param->setIsSecret(true); // done in the plugin constructor
        param->setRange(1, 1, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamPreservePAR);
        param->setLabel(kParamPreservePARLabel);
        param->setHint(kParamPreservePARHint);
        param->setAnimates(false);
        param->setDefault(false);
        //param->setIsSecret(true); // done in the plugin constructor
        param->setDefault(true);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamScale);
        param->setHint(kParamScaleHint);
        param->setLabel(kParamScaleLabel);
        param->setAnimates(true);
        //param->setIsSecret(true); // done in the plugin constructor
        param->setDefault(1., 1.);
        param->setRange(0., 0., DBL_MAX, DBL_MAX);
        param->setIncrement(0.05);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor *param = desc.defineChoiceParam(kParamFilter);
        param->setLabel(kParamFilterLabel);
        param->setHint(kParamFilterHint);
        param->setAnimates(false);
        param->appendOption(kParamFilterOptionImpulse);
        int nFilters = Filter2D::num_filters();
        int defIndex = 0;
        for (int i = 0; i < nFilters; ++i) {
            FilterDesc f;
            Filter2D::get_filterdesc(i, &f);
            param->appendOption(f.name);
            if (!strcmp(f.name , "lanczos3")) {
                defIndex = i + 1; // +1 because we added the "impulse" option
            }
        }
        param->setDefault(defIndex);
        if (page) {
            page->addChild(*param);
        }
    }
    
    // srcClipChanged
    {
        OFX::BooleanParamDescriptor* param = desc.defineBooleanParam(kSrcClipChanged);
        param->setDefault(false);
        param->setIsSecret(true);
        param->setAnimates(false);
        param->setEvaluateOnChange(false);
        if (page) {
            page->addChild(*param);
        }
    }
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect* OIIOResizePluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new OIIOResizePlugin(handle);
}


static OIIOResizePluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
