/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererDRMPRIME.h"

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/VideoLayerBridgeDRMPRIME.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/VideoLayerBridgeRockchip.h"
#include "cores/VideoPlayer/VideoRenderers/RenderCapture.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "settings/lib/Setting.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"
#include "windowing/gbm/DRMAtomic.h"
#include "windowing/gbm/WinSystemGbm.h"
#include "windowing/GraphicContext.h"
#include "ServiceBroker.h"

#include <sys/mman.h>

using namespace KODI::WINDOWING::GBM;

const std::string SETTING_VIDEOPLAYER_USEPRIMERENDERER = "videoplayer.useprimerenderer";

//------------------------------------------------------------------------------
// Video Buffers
//------------------------------------------------------------------------------

class CVideoBufferDumb
  : public CVideoBufferDRMPRIME
{
public:
  CVideoBufferDumb(IVideoBufferPool& pool, int id);
  ~CVideoBufferDumb();
  void SetRef(const VideoPicture& picture);
  void Unref();
private:
  void Create(uint32_t width, uint32_t height);
  void Destroy();

  AVDRMFrameDescriptor m_descriptor = {};
  CVideoBuffer* m_buffer = nullptr;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_handle = 0;
  uint64_t m_size = 0;
  uint64_t m_offset = 0;
  void *m_addr = nullptr;
  int m_fd = -1;
};

CVideoBufferDumb::CVideoBufferDumb(IVideoBufferPool& pool, int id)
  : CVideoBufferDRMPRIME(pool, id)
{
}

CVideoBufferDumb::~CVideoBufferDumb()
{
  Unref();
  Destroy();
}

void CVideoBufferDumb::Create(uint32_t width, uint32_t height)
{
  if (m_width == width && m_height == height)
    return;

  Destroy();

  CLog::Log(LOGNOTICE, "CVideoBufferDumb::{} - id={} width={} height={}", __FUNCTION__, m_id, width, height);

  CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  int fd = winSystem->GetDrm()->GetFileDescriptor();

  struct drm_mode_create_dumb create_dumb = { .height = height + (height / 2), .width = width, .bpp = 8 };
  int ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - ioctl DRM_IOCTL_MODE_CREATE_DUMB failed, ret={} errno={}", __FUNCTION__, ret, errno);
  m_handle = create_dumb.handle;
  m_size = create_dumb.size;

  struct drm_mode_map_dumb map_dumb = { .handle = m_handle };
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - ioctl DRM_IOCTL_MODE_MAP_DUMB failed, ret={} errno={}", __FUNCTION__, ret, errno);
  m_offset = map_dumb.offset;

  m_addr = mmap(NULL, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m_offset);
  if (m_addr == MAP_FAILED)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - mmap failed, errno={}", __FUNCTION__, errno);

  struct drm_prime_handle prime_handle = { .handle = m_handle };
  ret = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - ioctl DRM_IOCTL_PRIME_HANDLE_TO_FD failed, ret={} errno={}", __FUNCTION__, ret, errno);
  m_fd = prime_handle.fd;

  m_width = width;
  m_height = height;

  AVDRMFrameDescriptor* descriptor = &m_descriptor;
  descriptor->nb_objects = 1;
  descriptor->objects[0].fd = m_fd;
  descriptor->objects[0].size = m_size;
  descriptor->nb_layers = 1;
  AVDRMLayerDescriptor* layer = &descriptor->layers[0];
  layer->format = DRM_FORMAT_NV12;
  layer->nb_planes = 2;
  layer->planes[0].offset = 0;
  layer->planes[0].pitch = m_width;
  layer->planes[1].offset = m_width * m_height;
  layer->planes[1].pitch = m_width;

  m_pFrame->data[0] = reinterpret_cast<uint8_t*>(descriptor);
}

void CVideoBufferDumb::Destroy()
{
  if (!m_width && !m_height)
    return;

  CLog::Log(LOGNOTICE, "CVideoBufferDumb::{} - id={} width={} height={} size={}", __FUNCTION__, m_id, m_width, m_height, m_size);

  CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());

  int ret = close(m_fd);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - close failed, errno={}", __FUNCTION__, errno);
  m_fd = -1;

  ret = munmap(m_addr, m_size);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - munmap failed, errno={}", __FUNCTION__, errno);
  m_addr = nullptr;
  m_offset = 0;
  m_size = 0;

  struct drm_mode_destroy_dumb destroy_dumb = { .handle = m_handle };
  ret = drmIoctl(winSystem->GetDrm()->GetFileDescriptor(), DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - ioctl DRM_IOCTL_MODE_DESTROY_DUMB failed, ret={} errno={}", __FUNCTION__, ret, errno);
  m_handle = 0;

  m_width = 0;
  m_height = 0;
}

void CVideoBufferDumb::SetRef(const VideoPicture& picture)
{
  m_buffer = picture.videoBuffer;
  m_buffer->Acquire();

  m_pFrame->width = picture.iWidth;
  m_pFrame->height = picture.iHeight;
  m_pFrame->color_range = picture.color_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_UNSPECIFIED;
  m_pFrame->color_primaries = static_cast<AVColorPrimaries>(picture.color_primaries);
  m_pFrame->color_trc = static_cast<AVColorTransferCharacteristic>(picture.color_transfer);
  m_pFrame->colorspace = static_cast<AVColorSpace>(picture.color_space);

  YuvImage image = {};
  m_buffer->GetPlanes(image.plane);
  m_buffer->GetStrides(image.stride);

  Create(image.stride[0], (picture.iHeight + 15) & -16);

  memcpy(m_addr, image.plane[0], image.stride[0] * picture.iHeight);
  uint8_t* offset = static_cast<uint8_t*>(m_addr) + m_width * m_height;
  for (int y = 0, h = picture.iHeight / 2; y < h; y++)
  {
    uint8_t* dst = offset + m_width * y;
    uint8_t* src1 = image.plane[1] + image.stride[1] * y;
    uint8_t* src2 = image.plane[2] + image.stride[2] * y;

    for (int x = 0, w = image.stride[1]; x < w; x++)
    {
      *dst++ = *(src1 + x);
      *dst++ = *(src2 + x);
    }
  }
}

void CVideoBufferDumb::Unref()
{
  if (!m_buffer)
    return;

  m_buffer->Release();
  m_buffer = nullptr;
}

//------------------------------------------------------------------------------

class CVideoBufferPoolDumb
  : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolDumb();
  void Return(int id) override;
  CVideoBuffer* Get() override;

protected:
  CCriticalSection m_critSection;
  std::vector<CVideoBufferDumb*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;
};

CVideoBufferPoolDumb::~CVideoBufferPoolDumb()
{
  for (auto buf : m_all)
    delete buf;
}

CVideoBuffer* CVideoBufferPoolDumb::Get()
{
  CSingleLock lock(m_critSection);

  CVideoBufferDumb* buf = nullptr;
  if (!m_free.empty())
  {
    int idx = m_free.front();
    m_free.pop_front();
    m_used.push_back(idx);
    buf = m_all[idx];
  }
  else
  {
    int id = m_all.size();
    buf = new CVideoBufferDumb(*this, id);
    m_all.push_back(buf);
    m_used.push_back(id);
  }

  buf->Acquire(GetPtr());
  return buf;
}

void CVideoBufferPoolDumb::Return(int id)
{
  CSingleLock lock(m_critSection);

  m_all[id]->Unref();
  auto it = m_used.begin();
  while (it != m_used.end())
  {
    if (*it == id)
    {
      m_used.erase(it);
      break;
    }
    else
      ++it;
  }
  m_free.push_back(id);
}

//------------------------------------------------------------------------------
// main class
//------------------------------------------------------------------------------

CRendererDRMPRIME::~CRendererDRMPRIME()
{
  Flush(false);
}

CBaseRenderer* CRendererDRMPRIME::Create(CVideoBuffer* buffer)
{
  if (buffer && (dynamic_cast<CVideoBufferDRMPRIME*>(buffer) || buffer->GetFormat() == AV_PIX_FMT_YUV420P) &&
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(SETTING_VIDEOPLAYER_USEPRIMERENDERER) == 0)
  {
    CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
    if (winSystem && winSystem->GetDrm()->GetVideoPlane()->plane &&
        std::dynamic_pointer_cast<CDRMAtomic>(winSystem->GetDrm()))
      return new CRendererDRMPRIME();
  }

  return nullptr;
}

void CRendererDRMPRIME::Register()
{
  CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  if (!winSystem)
    return;

  std::shared_ptr<CDRMAtomic> drm = std::dynamic_pointer_cast<CDRMAtomic>(winSystem->GetDrm());
  if (drm && drm->GetVideoPlane()->plane)
  {
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

    settings->GetSetting(SETTING_VIDEOPLAYER_USEPRIMERENDERER)->SetVisible(true);
    VIDEOPLAYER::CRendererFactory::RegisterRenderer("drm_prime", CRendererDRMPRIME::Create);

    if (drm->SupportsProperty(drm->GetConnector(), "hdmi_output_format"))
      settings->GetSetting(CDRMUtils::SETTING_VIDEOPLAYER_HDMIOUTPUTFORMAT)->SetVisible(true);

    if (drm->SupportsProperty(drm->GetConnector(), "content type"))
      settings->GetSetting(CDRMUtils::SETTING_VIDEOPLAYER_HDMICONTENTTYPE)->SetVisible(true);
  }
}

bool CRendererDRMPRIME::Configure(const VideoPicture& picture, float fps, unsigned int orientation)
{
  m_format = picture.videoBuffer->GetFormat();
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorMatrix(picture.color_space, picture.iWidth, picture.iHeight) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  Flush(false);

  m_videoBufferPool = std::make_shared<CVideoBufferPoolDumb>();

  m_bConfigured = true;
  return true;
}

void CRendererDRMPRIME::ManageRenderArea()
{
  CBaseRenderer::ManageRenderArea();

  RESOLUTION_INFO info = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  if (info.iScreenWidth != info.iWidth)
  {
    CalcNormalRenderRect(0, 0, info.iScreenWidth, info.iScreenHeight,
                         GetAspectRatio() * CDisplaySettings::GetInstance().GetPixelRatio(),
                         CDisplaySettings::GetInstance().GetZoomAmount(),
                         CDisplaySettings::GetInstance().GetVerticalShift());
  }
}

void CRendererDRMPRIME::AddVideoPicture(const VideoPicture& picture, int index)
{
  BUFFER& buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    CLog::LogF(LOGERROR, "unreleased video buffer");
    buf.videoBuffer->Release();
  }
  if (dynamic_cast<CVideoBufferDRMPRIME*>(picture.videoBuffer))
  {
    buf.videoBuffer = picture.videoBuffer;
    buf.videoBuffer->Acquire();
  }
  else
  {
    CVideoBufferDumb* buffer = dynamic_cast<CVideoBufferDumb*>(m_videoBufferPool->Get());
    buffer->SetRef(picture);
    buf.videoBuffer = buffer;
  }
}

bool CRendererDRMPRIME::Flush(bool saveBuffers)
{
  if (!saveBuffers)
    for (int i = 0; i < NUM_BUFFERS; i++)
      ReleaseBuffer(i);

  m_iLastRenderBuffer = -1;
  return saveBuffers;
}

void CRendererDRMPRIME::ReleaseBuffer(int index)
{
  BUFFER& buf = m_buffers[index];
  if (buf.videoBuffer)
  {
    buf.videoBuffer->Release();
    buf.videoBuffer = nullptr;
  }
}

bool CRendererDRMPRIME::NeedBuffer(int index)
{
  if (m_iLastRenderBuffer == index)
    return true;

  CVideoBufferDRMPRIME* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(m_buffers[index].videoBuffer);
  if (buffer && buffer->m_fb_id)
    return true;

  return false;
}

CRenderInfo CRendererDRMPRIME::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = NUM_BUFFERS;
  return info;
}

void CRendererDRMPRIME::Update()
{
  if (!m_bConfigured)
    return;

  ManageRenderArea();
}

void CRendererDRMPRIME::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  if (m_iLastRenderBuffer == index && m_videoLayerBridge)
  {
    m_videoLayerBridge->UpdateVideoPlane();
    return;
  }

  CVideoBufferDRMPRIME* buffer = dynamic_cast<CVideoBufferDRMPRIME*>(m_buffers[index].videoBuffer);
  if (!buffer)
    return;

  AVDRMFrameDescriptor* descriptor = buffer->GetDescriptor();
  if (!descriptor || !descriptor->nb_layers)
    return;

  if (!m_videoLayerBridge)
  {
    CWinSystemGbm* winSystem = static_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
    m_videoLayerBridge = std::dynamic_pointer_cast<CVideoLayerBridgeDRMPRIME>(winSystem->GetVideoLayerBridge());
    if (!m_videoLayerBridge)
    {
      if (winSystem->GetDrm()->GetModule() == "rockchip")
        m_videoLayerBridge = std::make_shared<CVideoLayerBridgeRockchip>(winSystem->GetDrm());
      else
        m_videoLayerBridge = std::make_shared<CVideoLayerBridgeDRMPRIME>(winSystem->GetDrm());
    }
    winSystem->RegisterVideoLayerBridge(m_videoLayerBridge);
  }

  if (m_iLastRenderBuffer == -1)
    m_videoLayerBridge->Configure(buffer);

  m_videoLayerBridge->SetVideoPlane(buffer, m_destRect);

  m_iLastRenderBuffer = index;
}

bool CRendererDRMPRIME::RenderCapture(CRenderCapture* capture)
{
  capture->BeginRender();
  capture->EndRender();
  return true;
}

bool CRendererDRMPRIME::ConfigChanged(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

  return false;
}

bool CRendererDRMPRIME::Supports(ERENDERFEATURE feature)
{
  if (feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_PIXEL_RATIO)
    return true;

  return false;
}

bool CRendererDRMPRIME::Supports(ESCALINGMETHOD method)
{
  return false;
}
