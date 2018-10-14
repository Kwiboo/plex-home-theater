/*
 *  Copyright (C) 2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoLayerBridgeRockchip.h"

#include "cores/VideoPlayer/Process/gbm/VideoBufferDRMPRIME.h"
#include "utils/log.h"
#include "windowing/gbm/DRMUtils.h"

#include <linux/videodev2.h>

using namespace KODI::WINDOWING::GBM;

enum hdmi_colorimetry {
	HDMI_COLORIMETRY_NONE,
	HDMI_COLORIMETRY_ITU_601,
	HDMI_COLORIMETRY_ITU_709,
	HDMI_COLORIMETRY_EXTENDED,
};

enum hdmi_extended_colorimetry {
	HDMI_EXTENDED_COLORIMETRY_XV_YCC_601,
	HDMI_EXTENDED_COLORIMETRY_XV_YCC_709,
	HDMI_EXTENDED_COLORIMETRY_S_YCC_601,
	HDMI_EXTENDED_COLORIMETRY_ADOBE_YCC_601,
	HDMI_EXTENDED_COLORIMETRY_ADOBE_RGB,

	/* The following EC values are only defined in CEA-861-F. */
	HDMI_EXTENDED_COLORIMETRY_BT2020_CONST_LUM,
	HDMI_EXTENDED_COLORIMETRY_BT2020,
	HDMI_EXTENDED_COLORIMETRY_RESERVED,
};

#define RK_HDMI_COLORIMETRY_BT2020 (HDMI_COLORIMETRY_EXTENDED + HDMI_EXTENDED_COLORIMETRY_BT2020)

static int GetColorSpace(bool is10bit, AVFrame* frame)
{
  if (is10bit && frame->color_primaries != AVCOL_PRI_BT709)
    return V4L2_COLORSPACE_BT2020;
  if (frame->color_primaries == AVCOL_PRI_SMPTE170M)
    return V4L2_COLORSPACE_SMPTE170M;
  return V4L2_COLORSPACE_REC709;
}

void CVideoLayerBridgeRockchip::Configure(IVideoBufferDRMPRIME* buffer)
{
  AVDRMFrameDescriptor* descriptor = buffer->GetDescriptor();
  AVDRMLayerDescriptor* layer = &descriptor->layers[0];
  bool is10bit = layer->format == DRM_FORMAT_NV12_10;
  AVFrame* frame = dynamic_cast<CVideoBufferDRMPRIME*>(buffer)->GetFrame();

  m_hdr_metadata.type = HDMI_STATIC_METADATA_TYPE1;
  m_hdr_metadata.eotf = buffer->GetEOTF();

  if (m_hdr_blob_id)
    drmModeDestroyPropertyBlob(m_DRM->GetFileDescriptor(), m_hdr_blob_id);
  m_hdr_blob_id = 0;

  if (m_hdr_metadata.eotf)
  {
    AVMasteringDisplayMetadata* mdmd = buffer->GetMasteringDisplayMetadata();
    if (mdmd && mdmd->has_primaries)
    {
      for (int i = 0; i < 3; i++)
      {
        m_hdr_metadata.display_primaries_x[i] = std::round(av_q2d(mdmd->display_primaries[i][0]) * 50000.0);
        m_hdr_metadata.display_primaries_y[i] = std::round(av_q2d(mdmd->display_primaries[i][1]) * 50000.0);
      }
      m_hdr_metadata.white_point_x = std::round(av_q2d(mdmd->white_point[0]) * 50000.0);
      m_hdr_metadata.white_point_y = std::round(av_q2d(mdmd->white_point[1]) * 50000.0);

      CLog::Log(LOGNOTICE, "CVideoLayerBridgeRockchip::{} - r({},{}) g({},{}) b({},{}) wp({},{})", __FUNCTION__,
                av_q2d(mdmd->display_primaries[0][0]), av_q2d(mdmd->display_primaries[0][1]),
                av_q2d(mdmd->display_primaries[1][0]), av_q2d(mdmd->display_primaries[1][1]),
                av_q2d(mdmd->display_primaries[2][0]), av_q2d(mdmd->display_primaries[2][1]),
                av_q2d(mdmd->white_point[0]), av_q2d(mdmd->white_point[1]));
    }
    if (mdmd && mdmd->has_luminance)
    {
      m_hdr_metadata.max_mastering_display_luminance = std::round(av_q2d(mdmd->max_luminance));
      m_hdr_metadata.min_mastering_display_luminance = std::round(av_q2d(mdmd->min_luminance) * 10000.0);

      CLog::Log(LOGNOTICE, "CVideoLayerBridgeRockchip::{} - min_luminance={}, max_luminance={}", __FUNCTION__, av_q2d(mdmd->min_luminance), av_q2d(mdmd->max_luminance));
    }

    AVContentLightMetadata* clmd = buffer->GetContentLightMetadata();
    if (clmd)
    {
      m_hdr_metadata.max_cll = clmd->MaxCLL;
      m_hdr_metadata.max_fall = clmd->MaxFALL;

      CLog::Log(LOGNOTICE, "CVideoLayerBridgeRockchip::{} - MaxCLL={}, MaxFALL={}", __FUNCTION__, clmd->MaxCLL, clmd->MaxFALL);
    }

    drmModeCreatePropertyBlob(m_DRM->GetFileDescriptor(), &m_hdr_metadata, sizeof(m_hdr_metadata), &m_hdr_blob_id);
  }

  CLog::Log(LOGNOTICE, "CVideoLayerBridgeRockchip::{} - format={} is10bit={} width={} height={} colorspace={} color_primaries={} color_trc={} color_range={} eotf={} blob_id={}",
            __FUNCTION__, layer->format, is10bit, frame->width, frame->height, frame->colorspace, frame->color_primaries, frame->color_trc, frame->color_range, m_hdr_metadata.eotf, m_hdr_blob_id);

  struct plane* plane = m_DRM->GetVideoPlane();
  m_DRM->AddProperty(plane, "COLOR_SPACE", GetColorSpace(is10bit, frame));
  m_DRM->AddProperty(plane, "EOTF", m_hdr_metadata.eotf);

  struct connector* connector = m_DRM->GetConnector();
  if (m_DRM->SupportsProperty(connector, "HDR_SOURCE_METADATA"))
    m_DRM->AddProperty(connector, "HDR_SOURCE_METADATA", m_hdr_blob_id);
  m_DRM->AddProperty(connector, "hdmi_output_depth", is10bit ? 10 : 8);
  m_DRM->AddProperty(connector, "hdmi_output_colorimetry", is10bit ? RK_HDMI_COLORIMETRY_BT2020 : 0);
  m_DRM->SetActive(true);
}

void CVideoLayerBridgeRockchip::Disable()
{
  CVideoLayerBridgeDRMPRIME::Disable();

  struct plane* plane = m_DRM->GetVideoPlane();
  m_DRM->AddProperty(plane, "COLOR_SPACE", V4L2_COLORSPACE_DEFAULT);
  m_DRM->AddProperty(plane, "EOTF", HDMI_EOTF_TRADITIONAL_GAMMA_SDR);

  struct connector* connector = m_DRM->GetConnector();
  if (m_DRM->SupportsProperty(connector, "HDR_SOURCE_METADATA"))
    m_DRM->AddProperty(connector, "HDR_SOURCE_METADATA", 0);
  m_DRM->AddProperty(connector, "hdmi_output_depth", 8);
  m_DRM->AddProperty(connector, "hdmi_output_colorimetry", 0);
  m_DRM->SetActive(true);

  if (m_hdr_blob_id)
    drmModeDestroyPropertyBlob(m_DRM->GetFileDescriptor(), m_hdr_blob_id);
  m_hdr_blob_id = 0;
}
