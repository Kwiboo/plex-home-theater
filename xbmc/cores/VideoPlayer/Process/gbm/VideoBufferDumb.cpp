/*
 *  Copyright (C) 2019 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoBufferDumb.h"
#include "threads/SingleLock.h"
#include "utils/log.h"

#include <linux/dma-buf.h>
#include <sys/mman.h>

#include "ServiceBroker.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "windowing/gbm/WinSystemGbm.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace KODI::WINDOWING::GBM;

CVideoBufferDumb::CVideoBufferDumb(IVideoBufferPool& pool, int id)
  : CVideoBufferDRMPRIME(id)
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{}", __FUNCTION__, m_id);
}

CVideoBufferDumb::~CVideoBufferDumb()
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{}", __FUNCTION__, m_id);
  Unref();
  Destroy();
}

AVDRMFrameDescriptor* CVideoBufferDumb::GetDescriptor() const
{
  return const_cast<AVDRMFrameDescriptor*>(&m_descriptor);
}

void CVideoBufferDumb::GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES])
{
  AVDRMFrameDescriptor* descriptor = &m_descriptor;
  AVDRMLayerDescriptor* layer = &descriptor->layers[0];

  for (int i = 0; i < layer->nb_planes; i++)
    planes[i] = static_cast<uint8_t*>(m_addr) + layer->planes[i].offset;
}

void CVideoBufferDumb::GetStrides(int(&strides)[YuvImage::MAX_PLANES])
{
  AVDRMFrameDescriptor* descriptor = &m_descriptor;
  AVDRMLayerDescriptor* layer = &descriptor->layers[0];

  for (int i = 0; i < layer->nb_planes; i++)
    strides[i] = layer->planes[i].pitch;
}

void CVideoBufferDumb::Create(uint32_t width, uint32_t height)
{
  if (m_width == width && m_height == height)
    return;

  Destroy();

  CLog::Log(LOGNOTICE, "CVideoBufferDumb::{} - id={} width={} height={}", __FUNCTION__, m_id, width, height);

  CWinSystemGbm* winSystem = dynamic_cast<CWinSystemGbm*>(CServiceBroker::GetWinSystem());
  int fd = winSystem->GetDrm()->GetFileDescriptor();

  struct drm_mode_create_dumb create_dumb = { .height = height + (height >> 1), .width = width, .bpp = 8, .flags = 0x01 };
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
  layer->format = DRM_FORMAT_YUV420;
  layer->nb_planes = 3;
  layer->planes[0].offset = 0;
  layer->planes[0].pitch = m_width;
  layer->planes[1].offset = m_width * m_height;
  layer->planes[1].pitch = m_width >> 1;
  layer->planes[2].offset = layer->planes[1].offset + ((m_width * m_height) >> 2);
  layer->planes[2].pitch = m_width >> 1;
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

void CVideoBufferDumb::SyncStart()
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{}", __FUNCTION__, m_id);

  struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW };
  int ret = drmIoctl(m_fd, DMA_BUF_IOCTL_SYNC, &sync);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - ioctl DMA_BUF_IOCTL_SYNC failed, ret={} errno={}", __FUNCTION__, ret, errno);
}

void CVideoBufferDumb::SyncEnd()
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{}", __FUNCTION__, m_id);

  struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW };
  int ret = drmIoctl(m_fd, DMA_BUF_IOCTL_SYNC, &sync);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferDumb::{} - ioctl DMA_BUF_IOCTL_SYNC failed, ret={} errno={}", __FUNCTION__, ret, errno);
}

void CVideoBufferDumb::Unref()
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{}", __FUNCTION__, m_id);
}

void CVideoBufferDumb::Alloc(AVCodecContext* avctx, AVFrame* frame)
{
  int width = frame->width;
  int height = frame->height;
  int linesize_align[AV_NUM_DATA_POINTERS];

  avcodec_align_dimensions2(avctx, &width, &height, linesize_align);

  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{} width:{} height:{}", __FUNCTION__, m_id, width, height);

  Create(width, height);
}

void CVideoBufferDumb::Export(AVFrame* frame)
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{} width:{} height:{}", __FUNCTION__, m_id, m_width, m_height);

  YuvImage image = {};
  GetPlanes(image.plane);
  GetStrides(image.stride);

  for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
  {
    frame->data[i] = i < YuvImage::MAX_PLANES ? image.plane[i] : nullptr;
    frame->linesize[i] = i < YuvImage::MAX_PLANES ? image.stride[i] : 0;
    frame->buf[i] = i == 0 ? frame->opaque_ref : nullptr;
  }

  frame->extended_data = frame->data;
  frame->opaque_ref = nullptr;
}

void CVideoBufferDumb::Import(AVFrame* frame)
{
  CLog::Log(LOGDEBUG, "CVideoBufferDumb::{} - id:{} width:{} height:{}", __FUNCTION__, m_id, m_width, m_height);

  YuvImage image = {};
  GetPlanes(image.plane);
  GetStrides(image.stride);

  memcpy(image.plane[0], frame->data[0], image.stride[0] * m_height);
  memcpy(image.plane[1], frame->data[1], image.stride[1] * (m_height >> 1));
  memcpy(image.plane[2], frame->data[2], image.stride[2] * (m_height >> 1));
}

CVideoBufferPoolDumb::~CVideoBufferPoolDumb()
{
  CLog::Log(LOGDEBUG, "CVideoBufferPoolDumb::{}", __FUNCTION__);
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

  CLog::Log(LOGDEBUG, "CVideoBufferPoolDumb::{} - id:{}", __FUNCTION__, buf->GetId());
  buf->Acquire(GetPtr());
  return buf;
}

void CVideoBufferPoolDumb::Return(int id)
{
  CSingleLock lock(m_critSection);

  CLog::Log(LOGDEBUG, "CVideoBufferPoolDumb::{} - id:{}", __FUNCTION__, id);
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
