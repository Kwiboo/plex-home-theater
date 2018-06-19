/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <EGL/egl.h>
#include <unistd.h>

#include "WinSystemGbmGLESContext.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "windowing/GraphicContext.h"

#include "DRMUtils.h"

CDRMUtils::CDRMUtils()
  : m_connector(new connector)
  , m_crtc(new crtc)
  , m_primary_plane(new plane)
  , m_overlay_plane(new plane)
{
}

CDRMUtils::~CDRMUtils()
{
  delete m_connector;
  delete m_crtc;
  delete m_primary_plane;
  delete m_overlay_plane;
}

void CDRMUtils::WaitVBlank()
{
  drmVBlank vbl;
  vbl.request.type = DRM_VBLANK_RELATIVE;
  vbl.request.sequence = 1;
  drmWaitVBlank(m_fd, &vbl);
}

bool CDRMUtils::SetMode(const RESOLUTION_INFO& res)
{
  m_mode = &m_connector->connector->modes[atoi(res.strId.c_str())];
  m_width = res.iWidth;
  m_height = res.iHeight;

  CLog::Log(LOGDEBUG, "CDRMUtils::%s - found crtc mode: %dx%d%s @ %d Hz",
            __FUNCTION__,
            m_mode->hdisplay,
            m_mode->vdisplay,
            m_mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "",
            m_mode->vrefresh);

  return true;
}

void CDRMUtils::DrmFbDestroyCallback(struct gbm_bo *bo, void *data)
{
  struct drm_fb *fb = static_cast<drm_fb *>(data);

  if(fb->fb_id)
  {
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    drmModeRmFB(drm_fd, fb->fb_id);
  }

  delete (fb);
}

drm_fb * CDRMUtils::DrmFbGetFromBo(struct gbm_bo *bo)
{
  {
    struct drm_fb *fb = static_cast<drm_fb *>(gbm_bo_get_user_data(bo));
    if(fb)
    {
      return fb;
    }
  }

  struct drm_fb *fb = new drm_fb;
  fb->bo = bo;

  uint32_t width,
           height,
           handles[4] = {0},
           strides[4] = {0},
           offsets[4] = {0};

  width = gbm_bo_get_width(bo);
  height = gbm_bo_get_height(bo);

  handles[0] = gbm_bo_get_handle(bo).u32;
  strides[0] = gbm_bo_get_stride(bo);
  memset(offsets, 0, 16);

  auto ret = drmModeAddFB2(m_fd,
                           width,
                           height,
                           m_primary_plane->format,
                           handles,
                           strides,
                           offsets,
                           &fb->fb_id,
                           0);

  if(ret)
  {
    delete (fb);
    CLog::Log(LOGDEBUG, "CDRMUtils::%s - failed to add framebuffer", __FUNCTION__);
    return nullptr;
  }

  gbm_bo_set_user_data(bo, fb, DrmFbDestroyCallback);

  return fb;
}

static bool GetProperties(int fd, uint32_t id, uint32_t type, struct drm_object *object)
{
  drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, id, type);
  if (!props)
  {
    CLog::Log(LOGERROR, "CDRMUtils::%s - could not get properties for object %u", __FUNCTION__, id);
    return false;
  }

  object->id = id;
  object->type = type;
  object->props = props;
  object->props_info = new drmModePropertyPtr[props->count_props];

  for (uint32_t i = 0; i < props->count_props; i++)
    object->props_info[i] = drmModeGetProperty(fd, props->props[i]);

  return true;
}

static void FreeProperties(struct drm_object *object)
{
  if (object->props_info)
  {
    for (uint32_t i = 0; i < object->props->count_props; i++)
      drmModeFreeProperty(object->props_info[i]);

    delete [] object->props_info;
    object->props_info = nullptr;
  }

  drmModeFreeObjectProperties(object->props);
  object->props = nullptr;
  object->type = 0;
  object->id = 0;
}

static uint32_t GetPropertyId(struct drm_object *object, const char *name)
{
  for (uint32_t i = 0; i < object->props->count_props; i++)
    if (!strcmp(object->props_info[i]->name, name))
      return object->props_info[i]->prop_id;

  CLog::Log(LOGWARNING, "CDRMUtils::%s - could not find property %s", __FUNCTION__, name);
  return 0;
}

bool CDRMUtils::AddProperty(drmModeAtomicReqPtr req, struct drm_object *object, const char *name, uint64_t value)
{
  uint32_t property_id = GetPropertyId(object, name);
  if (!property_id)
    return false;

  if (drmModeAtomicAddProperty(req, object->id, property_id, value) < 0)
  {
    CLog::Log(LOGERROR, "CDRMUtils::%s - could not add property %s", __FUNCTION__, name);
    return false;
  }

  return true;
}

bool CDRMUtils::SetProperty(struct drm_object *object, const char *name, uint64_t value)
{
  uint32_t property_id = GetPropertyId(object, name);
  if (!property_id)
    return false;

  if (drmModeObjectSetProperty(m_fd, object->id, object->type, property_id, value) < 0)
  {
    CLog::Log(LOGERROR, "CDRMUtils::%s - could not set property %s", __FUNCTION__, name);
    return false;
  }

  return true;
}

static drmModeConnectorPtr GetConnector(int fd, drmModeResPtr resources)
{
  uint32_t fallback_id = 0;
  for (int i = 0; i < resources->count_connectors; i++)
  {
    drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[i]);
    if (connector &&
        connector->connection == DRM_MODE_CONNECTED &&
        connector->count_modes > 0)
    {
      // Prefer a connected connector with an attached encoder and modes
      if (connector->encoder_id)
      {
        CLog::Log(LOGDEBUG, "CDRMUtils::%s - found connector %u with encoder %u and %d modes", __FUNCTION__, connector->connector_id, connector->encoder_id, connector->count_modes);
        return connector;
      }
      else if (!fallback_id)
      {
        // Fall back to first connected connector with modes
        fallback_id = connector->connector_id;
      }
    }
    drmModeFreeConnector(connector);
  }

  if (fallback_id)
  {
    drmModeConnectorPtr connector = drmModeGetConnector(fd, fallback_id);
    if (connector)
    {
      CLog::Log(LOGDEBUG, "CDRMUtils::%s - found connector %u with %d modes", __FUNCTION__, connector->connector_id, connector->count_modes);
      return connector;
    }
  }

  CLog::Log(LOGERROR, "CDRMUtils::%s - could not find any connector", __FUNCTION__);
  return nullptr;
}

static drmModeCrtcPtr GetCrtc(int fd, drmModeResPtr resources, drmModeConnectorPtr connector)
{
  // Prefer the attached encoder crtc
  if (connector->encoder_id)
  {
    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoder_id);
    if (encoder && encoder->crtc_id)
    {
      drmModeCrtcPtr crtc = drmModeGetCrtc(fd, encoder->crtc_id);
      if (crtc)
      {
        CLog::Log(LOGDEBUG, "CDRMUtils::%s - found crtc %u with encoder %u", __FUNCTION__, crtc->crtc_id, encoder->encoder_id);
        drmModeFreeEncoder(encoder);
        return crtc;
      }
    }
    drmModeFreeEncoder(encoder);
  }

  // Fall back to first valid encoder and crtc combo
  for (int i = 0; i < connector->count_encoders; i++)
  {
    drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoders[i]);
    if (encoder)
    {
      for (int j = 0; j < resources->count_crtcs; j++)
      {
        if (encoder->possible_crtcs & (1 << j))
        {
          drmModeCrtcPtr crtc = drmModeGetCrtc(fd, resources->crtcs[j]);
          if (crtc)
          {
            CLog::Log(LOGDEBUG, "CDRMUtils::%s - found crtc %u with encoder %u", __FUNCTION__, crtc->crtc_id, encoder->encoder_id);
            drmModeFreeEncoder(encoder);
            return crtc;
          }
        }
      }
      drmModeFreeEncoder(encoder);
    }
  }

  CLog::Log(LOGERROR, "CDRMUtils::%s - could not find any crtc", __FUNCTION__);
  return nullptr;
}

static int GetCrtcIndex(drmModeResPtr resources, drmModeCrtcPtr crtc)
{
  for (int i = 0; i < resources->count_crtcs; i++)
    if (crtc->crtc_id == resources->crtcs[i])
      return i;

  CLog::Log(LOGERROR, "CDRMUtils::%s - could not find crtc index for crtc %u", __FUNCTION__, crtc->crtc_id);
  return 0;
}

static drmModeModeInfoPtr GetPreferredMode(drmModeConnectorPtr connector)
{
  drmModeModeInfoPtr fallback_mode = nullptr;
  for (int i = 0, fallback_area = 0; i < connector->count_modes; i++)
  {
    drmModeModeInfoPtr mode = &connector->modes[i];
    if (mode->type & DRM_MODE_TYPE_PREFERRED)
    {
      CLog::Log(LOGDEBUG, "CDRMUtils::%s - found preferred mode %dx%d @ %d Hz", __FUNCTION__, mode->hdisplay, mode->vdisplay, mode->vrefresh);
      return mode;
    }

    // Fall back to mode with highest resolution or 1920x1080 @ 60 Hz
    int area = mode->hdisplay * mode->vdisplay;
    if (area > fallback_area ||
        (mode->hdisplay == 1920 &&
         mode->vdisplay == 1080 &&
         mode->vrefresh == 60 &&
         mode->clock == 148500))
    {
      fallback_mode = mode;
      fallback_area = area;
    }
  }

  if (fallback_mode)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::%s - found fallback mode %dx%d @ %d Hz", __FUNCTION__, fallback_mode->hdisplay, fallback_mode->vdisplay, fallback_mode->vrefresh);
    return fallback_mode;
  }

  CLog::Log(LOGERROR, "CDRMUtils::%s - could not find preferred mode", __FUNCTION__);
  return nullptr;
}

// video_plane, osd_plane, gui_plane
// - video is always rendered on video_plane
// - gui is always rendered on gui_plane
//
// 1. find first plane with NVxx/YUxx/YVxx format -> use as video_plane
//    - test if plane supports AR24 -> set supports_gui=true
// 2a. if no video plane -> find primary plane -> use XR24 format -> set gui_plane (video/osd_plane=null)
// 2b. else -> find first plane that supports AR24 (and is not video_plane) -> set osd_plane (set gui_plane=video_plane if supports_gui=true, else gui_plane=osd_plane)
//
// on video layer activated:
// - has osd_plane and supports_gui=true -> set gui_plane=osd_plane
// on video layer deactivated:
// - has osd_plane and supports_gui=true -> set gui_plane=video_plane and disable osd_plane
//
// use drm prime egl rendering if video_plane=null

static bool SupportsFormat(drmModePlanePtr plane, uint32_t format)
{
  for (uint32_t i = 0; i < plane->count_formats; i++)
    if (plane->formats[i] == format)
      return true;

  return false;
}

#define fourcc_match(f, a, b) (((uint32_t)(f) & 0xffff) == fourcc_code(a, b, 0, 0))

static drmModePlanePtr GetVideoPlane(int fd, drmModePlaneResPtr resources, int crtc_index)
{
  for (uint32_t i = 0; i < resources->count_planes; i++)
  {
    drmModePlanePtr plane = drmModeGetPlane(fd, resources->planes[i]);
    if (plane && plane->possible_crtcs & (1 << crtc_index))
    {
      for (uint32_t j = 0; j < plane->count_formats; j++)
      {
        uint32_t format = plane->formats[j];
        // check for one of NVxx/YUxx/YVxx formats
        if (fourcc_match(format, 'N', 'V') ||
            fourcc_match(format, 'Y', 'U') ||
            fourcc_match(format, 'Y', 'V'))
        {
          CLog::Log(LOGDEBUG, "CDRMUtils::%s - found video plane %u", __FUNCTION__, plane->plane_id);
          return plane;
        }
      }
    }
    drmModeFreePlane(plane);
  }

  CLog::Log(LOGWARNING, "CDRMUtils::%s - could not find video plane", __FUNCTION__);
  return nullptr;
}

static drmModePlanePtr GetOsdPlane(int fd, drmModePlaneResPtr resources, int crtc_index, uint32_t video_plane_id)
{
  for (uint32_t i = 0; i < resources->count_planes; i++)
  {
    drmModePlanePtr plane = drmModeGetPlane(fd, resources->planes[i]);
    if (plane &&
        plane->possible_crtcs & (1 << crtc_index) &&
        plane->plane_id != video_plane_id &&
        SupportsFormat(plane, DRM_FORMAT_ARGB8888))
    {
      CLog::Log(LOGDEBUG, "CDRMUtils::%s - found osd plane %u", __FUNCTION__, plane->plane_id);
      return plane;
    }
    drmModeFreePlane(plane);
  }

  CLog::Log(LOGWARNING, "CDRMUtils::%s - could not find osd plane", __FUNCTION__);
  return nullptr;
}

static drmModePlanePtr GetPlane(int fd, drmModePlaneResPtr resources, int crtc_index, uint32_t type)
{
  for (uint32_t i = 0; i < resources->count_planes; i++)
  {
    drmModePlanePtr plane = drmModeGetPlane(fd, resources->planes[i]);
    if (plane && plane->possible_crtcs & (1 << crtc_index))
    {
      drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
      if (props)
      {
        for (uint32_t j = 0; j < props->count_props; j++)
        {
          drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
          if (prop &&
              !strcmp(prop->name, "type") &&
              props->prop_values[j] == type)
          {
            CLog::Log(LOGDEBUG, "CDRMUtils::%s - found plane %u", __FUNCTION__, plane->plane_id);
            drmModeFreeProperty(prop);
            drmModeFreeObjectProperties(props);
            return plane;
          }
          drmModeFreeProperty(prop);
        }
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreePlane(plane);
  }

  CLog::Log(LOGERROR, "CDRMUtils::%s - could not find plane", __FUNCTION__);
  return nullptr;
}

bool CDRMUtils::OpenDrm(bool atomic)
{
  static constexpr const char *modules[] =
  {
    "i915",
    "amdgpu",
    "radeon",
    "nouveau",
    "vmwgfx",
    "msm",
    "imx-drm",
    "rockchip",
    "vc4",
    "virtio_gpu",
    "sun4i-drm",
    "meson"
  };

  for (int i = 0; i < 10; ++i)
  {
    std::string device = "/dev/dri/card";
    device.append(std::to_string(i));

    for (auto module : modules)
    {
      m_fd = drmOpen(module, device.c_str());
      if (m_fd < 0)
        continue;

      if (drmSetClientCap(m_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1))
      {
        CLog::Log(LOGERROR, "CDRMUtils::%s - no universal planes support", __FUNCTION__);
        goto close;
      }

      if (atomic && drmSetClientCap(m_fd, DRM_CLIENT_CAP_ATOMIC, 1))
      {
        CLog::Log(LOGERROR, "CDRMUtils::%s - no atomic modesetting support", __FUNCTION__);
        goto close;
      }

      m_resources = drmModeGetResources(m_fd);
      if (!m_resources)
        goto close;

      m_connector->connector = GetConnector(m_fd, m_resources);
      if (!m_connector->connector ||
          !GetProperties(m_fd, m_connector->connector->connector_id, DRM_MODE_OBJECT_CONNECTOR, m_connector))
        goto close;

      m_mode = GetPreferredMode(m_connector->connector);
      if (!m_mode)
        goto close;

      m_crtc->crtc = GetCrtc(m_fd, m_resources, m_connector->connector);
      if (!m_crtc->crtc ||
          !GetProperties(m_fd, m_crtc->crtc->crtc_id, DRM_MODE_OBJECT_CRTC, m_crtc))
        goto close;

      m_plane_resources = drmModeGetPlaneResources(m_fd);
      if (!m_plane_resources)
        goto close;

      m_crtc_index = GetCrtcIndex(m_resources, m_crtc->crtc);

      m_primary_plane->plane = GetPlane(m_fd, m_plane_resources, m_crtc_index, DRM_PLANE_TYPE_PRIMARY);
      if (!m_primary_plane->plane ||
          !GetProperties(m_fd, m_primary_plane->plane->plane_id, DRM_MODE_OBJECT_PLANE, m_primary_plane))
        goto close;

      if (SupportsFormat(m_primary_plane->plane, DRM_FORMAT_ARGB8888))
        m_primary_plane->format = DRM_FORMAT_ARGB8888;
      else if (SupportsFormat(m_primary_plane->plane, DRM_FORMAT_XRGB8888))
        m_primary_plane->format = DRM_FORMAT_XRGB8888;
      else
        goto close;

      if (m_primary_plane->format == DRM_FORMAT_ARGB8888)
      {
        m_overlay_plane->plane = GetPlane(m_fd, m_plane_resources, m_crtc_index, DRM_PLANE_TYPE_OVERLAY);
        if (m_overlay_plane->plane)
        {
          if (!GetProperties(m_fd, m_overlay_plane->plane->plane_id, DRM_MODE_OBJECT_PLANE, m_overlay_plane))
            goto close;

          if (SupportsFormat(m_overlay_plane->plane, DRM_FORMAT_ARGB8888))
            m_overlay_plane->format = DRM_FORMAT_ARGB8888;
          else
            goto close;
        }
      }

    /*
    drmModePlanePtr video_plane = nullptr;
    drmModePlanePtr osd_plane = nullptr;
    drmModePlanePtr primary_plane = nullptr;
    if (atomic)
    {
      video_plane = GetVideoPlane(m_fd, plane_resources, crtc_index);
      if (video_plane)
        osd_plane = GetOsdPlane(m_fd, plane_resources, crtc_index, video_plane->plane_id);

      if (!osd_plane && video_plane)
      {
        drmModeFreePlane(video_plane);
        video_plane = nullptr;
      }
    }

    if (!video_plane)
      primary_plane = GetPlane(m_fd, plane_resources, crtc_index, DRM_PLANE_TYPE_PRIMARY);
    */

      m_module = module;
      m_device_path = device;

      CLog::Log(LOGDEBUG, "CDRMUtils::%s - opened device: %s using module: %s", __FUNCTION__, device.c_str(), module);
      return true;

close:
      CloseDrm();
    }
  }

  return false;
}

void CDRMUtils::CloseDrm()
{
  if (m_fd >= 0)
    drmClose(m_fd);

  m_fd = -1;
  m_mode = nullptr;

  drmModeFreeResources(m_resources);
  m_resources = nullptr;

  drmModeFreePlaneResources(m_plane_resources);
  m_plane_resources = nullptr;

  drmModeFreeConnector(m_connector->connector);
  m_connector->connector = nullptr;
  FreeProperties(m_connector);

  drmModeFreeCrtc(m_crtc->crtc);
  m_crtc->crtc = nullptr;
  FreeProperties(m_crtc);

  drmModeFreePlane(m_primary_plane->plane);
  m_primary_plane->plane = nullptr;
  m_primary_plane->format = 0;
  FreeProperties(m_primary_plane);

  drmModeFreePlane(m_overlay_plane->plane);
  m_overlay_plane->plane = nullptr;
  m_overlay_plane->format = 0;
  FreeProperties(m_overlay_plane);
}

bool CDRMUtils::InitDrm()
{
  if (m_fd < 0)
    return false;

  drmSetMaster(m_fd);

  m_orig_crtc = drmModeGetCrtc(m_fd, m_crtc->crtc->crtc_id);

  return true;
}

bool CDRMUtils::RestoreOriginalMode()
{
  if(!m_orig_crtc)
  {
    return false;
  }

  auto ret = drmModeSetCrtc(m_fd,
                            m_orig_crtc->crtc_id,
                            m_orig_crtc->buffer_id,
                            m_orig_crtc->x,
                            m_orig_crtc->y,
                            &m_connector->connector->connector_id,
                            1,
                            &m_orig_crtc->mode);

  if(ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::%s - failed to set original crtc mode", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGDEBUG, "CDRMUtils::%s - set original crtc mode", __FUNCTION__);

  drmModeFreeCrtc(m_orig_crtc);
  m_orig_crtc = nullptr;

  return true;
}

void CDRMUtils::DestroyDrm()
{
  RestoreOriginalMode();

  drmDropMaster(m_fd);

  CloseDrm();
}

RESOLUTION_INFO CDRMUtils::GetResolutionInfo(drmModeModeInfoPtr mode)
{
  RESOLUTION_INFO res;
  res.iScreen = 0;
  res.iScreenWidth = mode->hdisplay;
  res.iScreenHeight = mode->vdisplay;
  res.iWidth = res.iScreenWidth;
  res.iHeight = res.iScreenHeight;

  // TODO: limit iWidth/iHeight if enabled in settings
  if (HasVideoPlane() && res.iScreenWidth >= 3840 && res.iScreenHeight >= 2160)
  {
    res.iWidth = mode->vrefresh > 30 ? 1280 : 1920;
    res.iHeight = mode->vrefresh > 30 ? 720 : 1080;
  }

  if (mode->clock % 5 != 0)
    res.fRefreshRate = (float)mode->vrefresh * (1000.0f/1001.0f);
  else
    res.fRefreshRate = mode->vrefresh;
  res.iSubtitles = static_cast<int>(0.965 * res.iHeight);
  res.fPixelRatio = 1.0f;
  res.bFullScreen = true;

  if (mode->flags & DRM_MODE_FLAG_3D_MASK)
  {
    if (mode->flags & DRM_MODE_FLAG_3D_TOP_AND_BOTTOM)
      res.dwFlags = D3DPRESENTFLAG_MODE3DTB;
    else if (mode->flags & DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF)
      res.dwFlags = D3DPRESENTFLAG_MODE3DSBS;
  }
  else if (mode->flags & DRM_MODE_FLAG_INTERLACE)
    res.dwFlags = D3DPRESENTFLAG_INTERLACED;
  else
    res.dwFlags = D3DPRESENTFLAG_PROGRESSIVE;

  res.strMode = StringUtils::Format("%dx%d%s @ %.6f Hz", res.iScreenWidth, res.iScreenHeight,
                                    res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "", res.fRefreshRate);
  return res;
}

RESOLUTION_INFO CDRMUtils::GetCurrentMode()
{
  return GetResolutionInfo(m_mode);
}

std::vector<RESOLUTION_INFO> CDRMUtils::GetModes()
{
  // TODO: update m_connector->connector to get latest modes

  std::vector<RESOLUTION_INFO> resolutions;
  resolutions.reserve(m_connector->connector->count_modes);

  for (int i = 0; i < m_connector->connector->count_modes; i++)
  {
    RESOLUTION_INFO res = GetResolutionInfo(&m_connector->connector->modes[i]);
    res.strId = std::to_string(i);
    resolutions.push_back(res);
  }

  return resolutions;
}
