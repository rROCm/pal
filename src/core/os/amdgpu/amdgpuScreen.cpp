/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2015-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#include "core/os/amdgpu/amdgpuScreen.h"
#include "core/os/amdgpu/dri3/dri3WindowSystem.h"
#include "core/os/amdgpu/g_drmLoader.h"
#include "palSwapChain.h"
#include "palVectorImpl.h"

using namespace Util;

namespace Pal
{
namespace Amdgpu
{

// =====================================================================================================================
Screen::Screen(
    Device*  pDevice,
    Extent2d physicalDimension,
    Extent2d physicalResolution,
    uint32   connectorId)
    :
    m_pDevice(pDevice),
    m_physicalDimension(physicalDimension),
    m_physicalResolution(physicalResolution),
    m_connectorId(connectorId),
    m_drmMasterFd(InvalidFd),
    m_randrOutput(0)
{
    memset(&m_nativeColorGamut, 0, sizeof(m_nativeColorGamut));
    memset(&m_userColorGamut, 0, sizeof(m_userColorGamut));
}

// =====================================================================================================================
void Screen::Destroy()
{
}

// =====================================================================================================================
Result Screen::Init()
{
    static_cast<Device*>(m_pDevice)->GetHdrMetaData(m_connectorId, &m_nativeColorGamut);

    return Result::Success;
}

// =====================================================================================================================
Result Screen::GetProperties(
    ScreenProperties* pInfo
    ) const
{
    pInfo->hDisplay = nullptr;
    pInfo->screen   = m_connectorId;

    pInfo->physicalDimension.width  = m_physicalDimension.width;
    pInfo->physicalDimension.height = m_physicalDimension.height;

    pInfo->physicalResolution.width  = m_physicalResolution.width;
    pInfo->physicalResolution.height = m_physicalResolution.height;

    pInfo->pMainDevice = m_pDevice;

    // Don't support cross display for now.
    pInfo->otherDeviceCount = 0;

    // Todo.
    pInfo->supportWindowedWaitForVerticalBlank = false;
    pInfo->supportWindowedGetScanLine          = false;

    // Linux don't have pn source id concept.
    pInfo->vidPnSourceId = 0;

    Util::Strncpy(pInfo->displayName, "monitor", sizeof(pInfo->displayName));

    return Result::Success;
}

// =====================================================================================================================
Result Screen::GetScreenModeList(
    uint32*     pScreenModeCount,
    ScreenMode* pScreenModeList
    ) const
{
    return static_cast<Device*>(m_pDevice)->QueryScreenModesForConnector(m_connectorId,
                                                                         pScreenModeCount,
                                                                         pScreenModeList);
}

const SwizzledFormat PresentableSwizzledFormat[2] =
{
    {
        ChNumFormat::X8Y8Z8W8_Unorm,
        { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W }
    },
    {
        ChNumFormat::X8Y8Z8W8_Srgb,
        { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W }
    }
};

const SwizzledFormat PresentableHdrSwizzledFormat[1] =
{
    {
        ChNumFormat::X10Y10Z10W2_Unorm,
        { ChannelSwizzle::Z, ChannelSwizzle::Y, ChannelSwizzle::X, ChannelSwizzle::W }
    }
};

// =====================================================================================================================
Result Screen::GetFormats(
    uint32*          pFormatCount,
    SwizzledFormat*  pFormatList)
{
    Result       result          = Result::Success;
    const uint32 baseFormatCount = sizeof(PresentableSwizzledFormat) / sizeof(PresentableSwizzledFormat[0]);
    uint32       formatCount     = baseFormatCount;

    if (m_nativeColorGamut.metadata.eotf == HDMI_EOTF_SMPTE_ST2084)
    {
        formatCount += sizeof(PresentableHdrSwizzledFormat) / sizeof(PresentableHdrSwizzledFormat[0]);
    }

    PAL_ASSERT(((pFormatCount != nullptr) && (pFormatList != nullptr)) ||
               ((pFormatList == nullptr) && (pFormatCount != nullptr)));

    if ((pFormatCount != nullptr) && (pFormatList == nullptr))
    {
        *pFormatCount = formatCount;
    }
    else
    {
        const uint32 returnedFormat = Util::Min(*pFormatCount, formatCount);

        for (uint32_t i = 0; i < returnedFormat; i++)
        {
            if (i < baseFormatCount)
            {
                pFormatList[i] = PresentableSwizzledFormat[i];
            }
            else
            {
                pFormatList[i] = PresentableHdrSwizzledFormat[i - baseFormatCount];
            }
        }

        if (returnedFormat < formatCount)
        {
            result = Result::ErrorIncompleteResults;
        }

        *pFormatCount = returnedFormat;
    }

    return result;
}

// =====================================================================================================================
Result Screen::GetColorCapabilities(
    ScreenColorCapabilities* pCapabilities)
{
    uint32* pRefColorSpaces = reinterpret_cast<uint32*>(&pCapabilities->supportedColorSpaces);

    *pRefColorSpaces |= ScreenColorSpace::CsSrgb; // It's always supported

    if (m_nativeColorGamut.metadata.eotf != HDMI_EOTF_TRADITIONAL_GAMMA_SDR)
    {
        pCapabilities->nativeColorGamut.chromaticityRedX        = m_nativeColorGamut.metadata.chromaticityRedX;
        pCapabilities->nativeColorGamut.chromaticityRedY        = m_nativeColorGamut.metadata.chromaticityRedY;
        pCapabilities->nativeColorGamut.chromaticityGreenX      = m_nativeColorGamut.metadata.chromaticityGreenX;
        pCapabilities->nativeColorGamut.chromaticityGreenY      = m_nativeColorGamut.metadata.chromaticityGreenY;
        pCapabilities->nativeColorGamut.chromaticityBlueX       = m_nativeColorGamut.metadata.chromaticityBlueX;
        pCapabilities->nativeColorGamut.chromaticityBlueY       = m_nativeColorGamut.metadata.chromaticityBlueY;
        pCapabilities->nativeColorGamut.chromaticityWhitePointX = m_nativeColorGamut.metadata.chromaticityWhitePointX;
        pCapabilities->nativeColorGamut.chromaticityWhitePointY = m_nativeColorGamut.metadata.chromaticityWhitePointY;
        pCapabilities->nativeColorGamut.minLuminance            = m_nativeColorGamut.metadata.minLuminance;
        pCapabilities->nativeColorGamut.avgLuminance            = m_nativeColorGamut.metadata.maxFramAverageLightLevel;
        pCapabilities->nativeColorGamut.maxLuminance            = m_nativeColorGamut.metadata.maxLuminance;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 506
        pCapabilities->nativeColorGamut.maxContentLightLevel    = m_nativeColorGamut.metadata.maxContentLightLevel;
#endif
        pCapabilities->dolbyVisionSupported     = false; // Doesn't support yet.
        pCapabilities->freeSyncHdrSupported     = false; // Doesn't support yet.
        pCapabilities->freeSyncBacklightSupport = false; // Doesn't support yet.

        if (m_nativeColorGamut.metadata.eotf == HDMI_EOTF_SMPTE_ST2084)
        {
            *pRefColorSpaces |= ScreenColorSpace::TfPq2084;

            pCapabilities->hdr10Supported = true;
        }
    }
    else
    {
        pCapabilities->hdr10Supported           = false;
        pCapabilities->dolbyVisionSupported     = false;
        pCapabilities->freeSyncHdrSupported     = false;
        pCapabilities->freeSyncBacklightSupport = false;
    }

    return Result::Success;
}

// =====================================================================================================================
Result Screen::SetColorConfiguration(
    const ScreenColorConfig* pColorConfig)
{
    Result result = Result::Unsupported;

    m_userColorGamut.metadata.chromaticityRedX         = pColorConfig->userDefinedColorGamut.chromaticityRedX;
    m_userColorGamut.metadata.chromaticityRedY         = pColorConfig->userDefinedColorGamut.chromaticityRedY;
    m_userColorGamut.metadata.chromaticityGreenX       = pColorConfig->userDefinedColorGamut.chromaticityGreenX;
    m_userColorGamut.metadata.chromaticityGreenY       = pColorConfig->userDefinedColorGamut.chromaticityGreenY;
    m_userColorGamut.metadata.chromaticityBlueX        = pColorConfig->userDefinedColorGamut.chromaticityBlueX;
    m_userColorGamut.metadata.chromaticityBlueY        = pColorConfig->userDefinedColorGamut.chromaticityBlueY;
    m_userColorGamut.metadata.chromaticityWhitePointX  = pColorConfig->userDefinedColorGamut.chromaticityWhitePointX;
    m_userColorGamut.metadata.chromaticityWhitePointY  = pColorConfig->userDefinedColorGamut.chromaticityWhitePointY;
    m_userColorGamut.metadata.minLuminance             = pColorConfig->userDefinedColorGamut.minLuminance;
    m_userColorGamut.metadata.maxFramAverageLightLevel = pColorConfig->userDefinedColorGamut.avgLuminance;
    m_userColorGamut.metadata.maxLuminance             = pColorConfig->userDefinedColorGamut.maxLuminance;
#if PAL_CLIENT_INTERFACE_MAJOR_VERSION >= 506
    m_userColorGamut.metadata.maxContentLightLevel     = pColorConfig->userDefinedColorGamut.maxContentLightLevel;
#endif

    result = static_cast<Device*>(m_pDevice)->SetHdrMetaData(m_connectorId, &m_userColorGamut);

    return result;
}

// =====================================================================================================================
Result Screen::AcquireScreenAccess(
    OsDisplayHandle hDisplay,
    WsiPlatform     wsiPlatform)
{
    Result result = Result::ErrorPrivateScreenUsed;

    if (m_drmMasterFd == InvalidFd)
    {

        result = WindowSystem::AcquireScreenAccess(m_pDevice,
                                                   hDisplay,
                                                   wsiPlatform,
                                                   m_connectorId,
                                                   &m_randrOutput,
                                                   &m_drmMasterFd);
    }

    return result;
}

// =====================================================================================================================
Result Screen::ReleaseScreenAccess()
{
    Result result = Result::ErrorPrivateScreenNotEnabled;

    if (m_drmMasterFd != InvalidFd)
    {
        close(m_drmMasterFd);

        m_drmMasterFd = InvalidFd;
        result        = Result::Success;
    }

    return result;
}

// =====================================================================================================================
Result Screen::GetRandrOutput(
    OsDisplayHandle hDisplay,
    uint32*         pRandrOutput)
{
    Result result = Result::ErrorUnknown;

    if (m_randrOutput == 0)
    {
        result = WindowSystem::GetOutputFromConnector(hDisplay,
                                                      m_pDevice,
                                                      Pal::WsiPlatform::Xcb,
                                                      m_connectorId,
                                                      &m_randrOutput);
    }

    if (result == Result::Success)
    {
        *pRandrOutput = m_randrOutput;
    }

    return result;
}

// =====================================================================================================================
Result Screen::SetRandrOutput(
    uint32 randrOutput)
{
    m_randrOutput = randrOutput;

    return Result::Success;
}

} // Amdgpu
} // Pal