/*
 *  Copyright (C) 2019 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VideoBufferDRMPRIME.h"

class VideoPicture;

class CVideoBufferMemfd
  : public CVideoBufferDRMPRIME
{
public:
  CVideoBufferMemfd(IVideoBufferPool& pool, int id, int udmabuf);
  ~CVideoBufferMemfd();
  void GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES]) override;
  void GetStrides(int(&strides)[YuvImage::MAX_PLANES]) override;
  void SetRef(const VideoPicture* picture);
  void Unref();

  void Alloc(struct AVCodecContext* avctx, AVFrame* frame);
  void Export(AVFrame* frame);
  void Import(AVFrame* frame);

  void SyncStart();
  void SyncEnd();

private:
  void Create(uint32_t width, uint32_t height);
  void Destroy();

  AVDRMFrameDescriptor m_descriptor = {};
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint64_t m_size = 0;
  void *m_addr = nullptr;
  int m_memfd = -1;
  int m_dmafd = -1;
  int m_udmabuf = -1;
};

class CVideoBufferPoolMemfd
  : public IVideoBufferPool
{
public:
  ~CVideoBufferPoolMemfd();
  void Return(int id) override;
  CVideoBuffer* Get() override;

protected:
  CCriticalSection m_critSection;
  std::vector<CVideoBufferMemfd*> m_all;
  std::deque<int> m_used;
  std::deque<int> m_free;
  int m_fd = -1;
};
