/*
 *  Copyright (C) 2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/HwDecRender/VideoLayerBridgeDRMPRIME.h"

struct hdr_static_metadata {
  uint16_t eotf;
  uint16_t type;
  uint16_t display_primaries_x[3];
  uint16_t display_primaries_y[3];
  uint16_t white_point_x;
  uint16_t white_point_y;
  uint16_t max_mastering_display_luminance;
  uint16_t min_mastering_display_luminance;
  uint16_t max_fall;
  uint16_t max_cll;
  uint16_t min_cll;
};

class CVideoLayerBridgeRockchip
  : public CVideoLayerBridgeDRMPRIME
{
public:
  CVideoLayerBridgeRockchip(std::shared_ptr<KODI::WINDOWING::GBM::CDRMUtils> drm) : CVideoLayerBridgeDRMPRIME(drm) {};
  void Disable() override;
  void Configure(CVideoBufferDRMPRIME* buffer) override;
private:
  uint32_t m_hdr_blob_id = 0;
  struct hdr_static_metadata m_hdr_metadata = {0};
};
