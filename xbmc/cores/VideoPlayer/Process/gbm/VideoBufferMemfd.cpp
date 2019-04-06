/*
 *  Copyright (C) 2019 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoBufferMemfd.h"
#include "threads/SingleLock.h"
#include "utils/log.h"

#include <drm_fourcc.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

CVideoBufferMemfd::CVideoBufferMemfd(IVideoBufferPool& pool, int id, int udmabuf)
  : CVideoBufferDRMPRIME(pool, id)
{
  CLog::Log(LOGDEBUG, "CVideoBufferMemfd::{} - id:{}", __FUNCTION__, m_id);
  m_udmabuf = udmabuf;
}

CVideoBufferMemfd::~CVideoBufferMemfd()
{
  CLog::Log(LOGDEBUG, "CVideoBufferMemfd::{} - id:{}", __FUNCTION__, m_id);
  Unref();
  Destroy();
}

void CVideoBufferMemfd::GetPlanes(uint8_t*(&planes)[YuvImage::MAX_PLANES])
{
  AVDRMFrameDescriptor* descriptor = &m_descriptor;
  AVDRMLayerDescriptor* layer = &descriptor->layers[0];

  for (int i = 0; i < layer->nb_planes; i++)
    planes[i] = static_cast<uint8_t*>(m_addr) + layer->planes[i].offset;
}

void CVideoBufferMemfd::GetStrides(int(&strides)[YuvImage::MAX_PLANES])
{
  AVDRMFrameDescriptor* descriptor = &m_descriptor;
  AVDRMLayerDescriptor* layer = &descriptor->layers[0];

  for (int i = 0; i < layer->nb_planes; i++)
    strides[i] = layer->planes[i].pitch;
}

void CVideoBufferMemfd::Create(uint32_t width, uint32_t height)
{
  if (m_width == width && m_height == height)
    return;

  Destroy();

  CLog::Log(LOGNOTICE, "CVideoBufferMemfd::{} - id={} width={} height={}", __FUNCTION__, m_id, width, height);

  m_size = width * (height + (height >> 1));

  int fd = memfd_create("videobuffer", MFD_ALLOW_SEALING);
  if (fd < 0)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - memfd_create MFD_ALLOW_SEALING failed, fd={} errno={}", __FUNCTION__, fd, errno);
  m_memfd = fd;

  int ret = ftruncate(fd, m_size);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - ftruncate {} failed, ret={} errno={}", __FUNCTION__, m_size, ret, errno);

  m_addr = mmap(NULL, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (m_addr == MAP_FAILED)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - mmap failed, errno={}", __FUNCTION__, errno);

  ret = fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - fcntl F_ADD_SEALS F_SEAL_SHRINK failed, ret={} errno={}", __FUNCTION__, ret, errno);

  struct udmabuf_create create = {
    .memfd = static_cast<uint32_t>(fd),
    .flags = 0,
    .offset = 0,
    .size = m_size,
  };
  m_dmafd = ioctl(m_udmabuf, UDMABUF_CREATE, &create);
  if (m_dmafd < 0)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - ioctl UDMABUF_CREATE failed, errno={}", __FUNCTION__, errno);

  m_width = width;
  m_height = height;

  AVDRMFrameDescriptor* descriptor = &m_descriptor;
  descriptor->nb_objects = 1;
  descriptor->objects[0].fd = m_dmafd;
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

  m_pFrame->data[0] = reinterpret_cast<uint8_t*>(descriptor);
}

void CVideoBufferMemfd::Destroy()
{
  if (!m_width && !m_height)
    return;

  CLog::Log(LOGNOTICE, "CVideoBufferMemfd::{} - id={} width={} height={} size={}", __FUNCTION__, m_id, m_width, m_height, m_size);

  int ret = close(m_dmafd);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - close dmafd failed, errno={}", __FUNCTION__, errno);
  m_dmafd = -1;

  ret = close(m_memfd);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - close memfd failed, errno={}", __FUNCTION__, errno);
  m_memfd = -1;

  ret = munmap(m_addr, m_size);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferMemfd::{} - munmap failed, errno={}", __FUNCTION__, errno);
  m_addr = nullptr;
  m_size = 0;

  m_width = 0;
  m_height = 0;
}

void CVideoBufferMemfd::SyncStart()
{
}

void CVideoBufferMemfd::SyncEnd()
{
}

void CVideoBufferMemfd::SetRef(const VideoPicture* picture)
{
  m_pFrame->width = picture->iWidth;
  m_pFrame->height = picture->iHeight;
  m_pFrame->color_range = picture->color_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_UNSPECIFIED;
  m_pFrame->color_primaries = static_cast<AVColorPrimaries>(picture->color_primaries);
  m_pFrame->color_trc = static_cast<AVColorTransferCharacteristic>(picture->color_transfer);
  m_pFrame->colorspace = static_cast<AVColorSpace>(picture->color_space);
}

void CVideoBufferMemfd::Unref()
{
  CLog::Log(LOGDEBUG, "CVideoBufferMemfd::{} - id:{}", __FUNCTION__, m_id);
}

void CVideoBufferMemfd::Alloc(AVCodecContext* avctx, AVFrame* frame)
{
  int width = frame->width;
  int height = frame->height;
  int linesize_align[AV_NUM_DATA_POINTERS];

  avcodec_align_dimensions2(avctx, &width, &height, linesize_align);

  CLog::Log(LOGDEBUG, "CVideoBufferMemfd::{} - id:{} width:{} height:{}", __FUNCTION__, m_id, width, height);

  Create(width, height);
}

void CVideoBufferMemfd::Export(AVFrame* frame)
{
  CLog::Log(LOGDEBUG, "CVideoBufferMemfd::{} - id:{} width:{} height:{}", __FUNCTION__, m_id, m_width, m_height);

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

void CVideoBufferMemfd::Import(AVFrame* frame)
{
  CLog::Log(LOGDEBUG, "CVideoBufferMemfd::{} - id:{} width:{} height:{}", __FUNCTION__, m_id, m_width, m_height);

  YuvImage image = {};
  GetPlanes(image.plane);
  GetStrides(image.stride);

  memcpy(image.plane[0], frame->data[0], image.stride[0] * m_height);
  memcpy(image.plane[1], frame->data[1], image.stride[1] * (m_height >> 1));
  memcpy(image.plane[2], frame->data[2], image.stride[2] * (m_height >> 1));
}

CVideoBufferPoolMemfd::~CVideoBufferPoolMemfd()
{
  CLog::Log(LOGDEBUG, "CVideoBufferPoolMemfd::{}", __FUNCTION__);
  int ret = close(m_fd);
  if (ret)
    CLog::Log(LOGERROR, "CVideoBufferPoolMemfd::{} - close failed, errno={}", __FUNCTION__, errno);
  m_fd = -1;
  for (auto buf : m_all)
    delete buf;
}

CVideoBuffer* CVideoBufferPoolMemfd::Get()
{
  CSingleLock lock(m_critSection);

  CVideoBufferMemfd* buf = nullptr;
  if (!m_free.empty())
  {
    int idx = m_free.front();
    m_free.pop_front();
    m_used.push_back(idx);
    buf = m_all[idx];
  }
  else
  {
    if (m_fd == -1)
      m_fd = open("/dev/udmabuf", O_RDWR);

    int id = m_all.size();
    buf = new CVideoBufferMemfd(*this, id, m_fd);
    m_all.push_back(buf);
    m_used.push_back(id);
  }

  CLog::Log(LOGDEBUG, "CVideoBufferPoolMemfd::{} - id:{}", __FUNCTION__, buf->GetId());
  buf->Acquire(GetPtr());
  return buf;
}

void CVideoBufferPoolMemfd::Return(int id)
{
  CSingleLock lock(m_critSection);

  CLog::Log(LOGDEBUG, "CVideoBufferPoolMemfd::{} - id:{}", __FUNCTION__, id);
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
