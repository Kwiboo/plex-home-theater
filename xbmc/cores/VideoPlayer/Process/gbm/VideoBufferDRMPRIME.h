/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/Process/VideoBuffer.h"

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/mastering_display_metadata.h>
}

// Color enums is copied from linux include/drm/drm_color_mgmt.h (strangely not part of uapi)
enum drm_color_encoding
{
  DRM_COLOR_YCBCR_BT601,
  DRM_COLOR_YCBCR_BT709,
  DRM_COLOR_YCBCR_BT2020,
};
enum drm_color_range
{
  DRM_COLOR_YCBCR_LIMITED_RANGE,
  DRM_COLOR_YCBCR_FULL_RANGE,
};
// HDR enums is copied from linux include/linux/hdmi.h (strangely not part of uapi)
enum hdmi_metadata_type
{
  HDMI_STATIC_METADATA_TYPE1 = 1,
};
enum hdmi_eotf
{
  HDMI_EOTF_TRADITIONAL_GAMMA_SDR,
  HDMI_EOTF_TRADITIONAL_GAMMA_HDR,
  HDMI_EOTF_SMPTE_ST2084,
  HDMI_EOTF_BT_2100_HLG,
};

class CVideoBufferDRMPRIME : public CVideoBuffer
{
public:
  CVideoBufferDRMPRIME() = delete;
  ~CVideoBufferDRMPRIME() override = default;

  virtual AVDRMFrameDescriptor* GetDescriptor() const = 0;
  uint32_t GetWidth() const;
  uint32_t GetHeight() const;
  int GetColorEncoding() const;
  int GetColorRange() const;
  uint8_t GetEOTF() const;
  const AVMasteringDisplayMetadata* GetMasteringDisplayMetadata() const;
  const AVContentLightMetadata* GetContentLightMetadata() const;

  virtual bool IsValid() const
  {
    return true;
  };
  virtual bool Map()
  {
    return true;
  };
  virtual void Unmap() {};

  uint32_t m_fb_id = 0;
  uint32_t m_handles[AV_DRM_MAX_PLANES] = {};

  VideoPicture DVDPic;

protected:
  explicit CVideoBufferDRMPRIME(int id);
};

class CVideoBufferDRMPRIMEFFmpeg : public CVideoBufferDRMPRIME
{
public:
  CVideoBufferDRMPRIMEFFmpeg(IVideoBufferPool& pool, int id);
  ~CVideoBufferDRMPRIMEFFmpeg() override;
  void SetRef(AVFrame* frame);
  void Unref();

  AVDRMFrameDescriptor* GetDescriptor() const override
  {
    return reinterpret_cast<AVDRMFrameDescriptor*>(m_pFrame->data[0]);
  }

  bool IsValid() const override;

protected:
  AVFrame* m_pFrame = nullptr;
};

class CVideoBufferPoolDRMPRIMEFFmpeg : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolDRMPRIMEFFmpeg() override;
  void Return(int id) override;
  CVideoBuffer* Get() override;

protected:
  CCriticalSection m_critSection;
  std::vector<CVideoBufferDRMPRIMEFFmpeg*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;
};
