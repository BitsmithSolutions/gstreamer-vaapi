/*
 *  gstvaapiwindow_wayland.c - VA/Wayland window abstraction
 *
 *  Copyright (C) 2012-2013 Intel Corporation
 *    Author: Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

/**
 * SECTION:gstvaapiwindow_wayland
 * @short_description: VA/Wayland window abstraction
 */

#include "sysdeps.h"
#include "gstvaapicompat.h"
#include "gstvaapiobject_priv.h"
#include "gstvaapiwindow_wayland.h"
#include "gstvaapiwindow_priv.h"
#include "gstvaapidisplay_wayland.h"
#include "gstvaapidisplay_wayland_priv.h"
#include "gstvaapiutils.h"
#include "gstvaapifilter.h"
#include "gstvaapisurfacepool.h"

GST_DEBUG_CATEGORY_EXTERN (gst_debug_vaapi_window);
#define GST_CAT_DEFAULT gst_debug_vaapi_window

#define GST_VAAPI_WINDOW_WAYLAND_CAST(obj) \
    ((GstVaapiWindowWayland *)(obj))

#define GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE(obj) \
    gst_vaapi_window_wayland_get_instance_private (GST_VAAPI_WINDOW_WAYLAND_CAST (obj))

#define GST_VAAPI_WINDOW_WAYLAND_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_VAAPI_WINDOW_WAYLAND, GstVaapiWindowWaylandClass))

typedef struct _GstVaapiWindowWaylandPrivate GstVaapiWindowWaylandPrivate;
typedef struct _GstVaapiWindowWaylandClass GstVaapiWindowWaylandClass;
typedef struct _FrameState FrameState;

struct _FrameState
{
  GstVaapiWindow *window;
  GstVaapiSurface *surface;
  GstVaapiVideoPool *surface_pool;
  struct wl_callback *callback;
  gboolean done;
};

static FrameState *
frame_state_new (GstVaapiWindow * window)
{
  FrameState *frame;

  frame = g_slice_new (FrameState);
  if (!frame)
    return NULL;

  frame->window = window;
  frame->surface = NULL;
  frame->surface_pool = NULL;
  frame->callback = NULL;
  frame->done = FALSE;
  return frame;
}

static void
frame_state_free (FrameState * frame)
{
  if (!frame)
    return;

  if (frame->surface) {
    if (frame->surface_pool)
      gst_vaapi_video_pool_put_object (frame->surface_pool, frame->surface);
    frame->surface = NULL;
  }
  gst_vaapi_video_pool_replace (&frame->surface_pool, NULL);

  g_clear_pointer (&frame->callback, wl_callback_destroy);
  g_slice_free (FrameState, frame);
}

struct _GstVaapiWindowWaylandPrivate
{
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  struct wl_shell_surface *wl_shell_surface;
  struct wl_surface *surface;
  struct wl_region *opaque_region;
  struct wl_event_queue *event_queue;
  FrameState *last_frame;
  GstPoll *poll;
  GstPollFD pollfd;
  guint is_shown:1;
  guint fullscreen_on_show:1;
  guint sync_failed:1;
  volatile guint num_frames_pending;
  gboolean need_vpp;
};

/**
 * GstVaapiWindowWayland:
 *
 * A Wayland window abstraction.
 */
struct _GstVaapiWindowWayland
{
  /*< private > */
  GstVaapiWindow parent_instance;
};

/**
 * GstVaapiWindowWaylandClass:
 *
 * An Wayland Window wrapper class.
 */
struct _GstVaapiWindowWaylandClass
{
  /*< private > */
  GstVaapiWindowClass parent_class;
};

G_DEFINE_TYPE_WITH_PRIVATE (GstVaapiWindowWayland, gst_vaapi_window_wayland,
    GST_TYPE_VAAPI_WINDOW);

/* Object signals */
enum
{
  SIZE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
handle_xdg_toplevel_configure (void *data, struct xdg_toplevel *xdg_toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
  GstVaapiWindow *window = GST_VAAPI_WINDOW (data);
  const uint32_t *state;

  GST_DEBUG ("Got XDG-toplevel::reconfigure, [width x height] = [%d x %d]",
      width, height);

  wl_array_for_each (state, states) {
    switch (*state) {
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
      case XDG_TOPLEVEL_STATE_RESIZING:
      case XDG_TOPLEVEL_STATE_ACTIVATED:
        break;
    }
  }

  if (width > 0 && height > 0) {
    gst_vaapi_window_set_size (window, width, height);
    g_signal_emit (window, signals[SIZE_CHANGED], 0, width, height);
  }
}

static void
handle_xdg_toplevel_close (void *data, struct xdg_toplevel *xdg_toplevel)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  handle_xdg_toplevel_configure,
  handle_xdg_toplevel_close,
};

static gboolean
gst_vaapi_window_wayland_show (GstVaapiWindow * window)
{
  GstVaapiWindowWaylandPrivate *priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);

  if (priv->xdg_surface == NULL) {
    GST_FIXME ("GstVaapiWindowWayland::show() unimplemented for wl_shell");
    return TRUE;
  }

  if (priv->xdg_toplevel != NULL) {
    GST_DEBUG ("XDG toplevel already mapped");
    return TRUE;
  }

  /* Create a toplevel window out of it */
  priv->xdg_toplevel = xdg_surface_get_toplevel (priv->xdg_surface);
  g_return_val_if_fail (priv->xdg_toplevel, FALSE);
  xdg_toplevel_set_title (priv->xdg_toplevel, "VA-API Wayland window");
  wl_proxy_set_queue ((struct wl_proxy *) priv->xdg_toplevel,
      priv->event_queue);

  xdg_toplevel_add_listener (priv->xdg_toplevel, &xdg_toplevel_listener,
      window);

  /* Commit the xdg_surface state as top-level window */
  wl_surface_commit (priv->surface);

  return TRUE;
}

static gboolean
gst_vaapi_window_wayland_hide (GstVaapiWindow * window)
{
  GstVaapiWindowWaylandPrivate *priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);

  if (priv->xdg_surface == NULL) {
    GST_FIXME ("GstVaapiWindowWayland::hide() unimplemented for wl_shell");
    return TRUE;
  }

  if (priv->xdg_toplevel != NULL) {
    g_clear_pointer (&priv->xdg_toplevel, xdg_toplevel_destroy);
    wl_surface_commit (priv->surface);
  }

  return TRUE;
}

static gboolean
gst_vaapi_window_wayland_sync (GstVaapiWindow * window)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);
  struct wl_display *const wl_display =
      GST_VAAPI_WINDOW_NATIVE_DISPLAY (window);

  if (priv->sync_failed)
    return FALSE;

  if (priv->pollfd.fd < 0) {
    priv->pollfd.fd = wl_display_get_fd (wl_display);
    gst_poll_add_fd (priv->poll, &priv->pollfd);
    gst_poll_fd_ctl_read (priv->poll, &priv->pollfd, TRUE);
  }

  while (g_atomic_int_get (&priv->num_frames_pending) > 0) {
    while (wl_display_prepare_read_queue (wl_display, priv->event_queue) < 0) {
      if (wl_display_dispatch_queue_pending (wl_display, priv->event_queue) < 0)
        goto error;
    }

    if (wl_display_flush (wl_display) < 0)
      goto error;

    if (g_atomic_int_get (&priv->num_frames_pending) == 0) {
      wl_display_cancel_read (wl_display);
      return TRUE;
    }

  again:
    if (gst_poll_wait (priv->poll, GST_CLOCK_TIME_NONE) < 0) {
      int saved_errno = errno;
      if (saved_errno == EAGAIN || saved_errno == EINTR)
        goto again;
      wl_display_cancel_read (wl_display);
      if (saved_errno == EBUSY) /* flushing */
        return FALSE;
      else
        goto error;
    }

    if (wl_display_read_events (wl_display) < 0)
      goto error;
    if (wl_display_dispatch_queue_pending (wl_display, priv->event_queue) < 0)
      goto error;
  }
  return TRUE;

  /* ERRORS */
error:
  {
    priv->sync_failed = TRUE;
    GST_ERROR ("Error on dispatching events: %s", g_strerror (errno));
    return FALSE;
  }
}

static void
handle_ping (void *data, struct wl_shell_surface *wl_shell_surface,
    uint32_t serial)
{
  wl_shell_surface_pong (wl_shell_surface, serial);
}

static void
handle_configure (void *data, struct wl_shell_surface *wl_shell_surface,
    uint32_t edges, int32_t width, int32_t height)
{
}

static void
handle_popup_done (void *data, struct wl_shell_surface *wl_shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
  handle_ping,
  handle_configure,
  handle_popup_done
};

static void
handle_xdg_surface_configure (void *data, struct xdg_surface *xdg_surface,
    uint32_t serial)
{
  xdg_surface_ack_configure (xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  handle_xdg_surface_configure,
};

static gboolean
gst_vaapi_window_wayland_set_fullscreen (GstVaapiWindow * window,
    gboolean fullscreen)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);

  if (!priv->is_shown) {
    priv->fullscreen_on_show = fullscreen;
    return TRUE;
  }

  /* XDG-shell */
  if (priv->xdg_toplevel != NULL) {
    if (fullscreen)
      xdg_toplevel_set_fullscreen (priv->xdg_toplevel, NULL);
    else
      xdg_toplevel_unset_fullscreen (priv->xdg_toplevel);
    return TRUE;
  }

  /* wl_shell fallback */
  if (!fullscreen)
    wl_shell_surface_set_toplevel (priv->wl_shell_surface);
  else {
    wl_shell_surface_set_fullscreen (priv->wl_shell_surface,
        WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
  }

  return TRUE;
}

static gboolean
gst_vaapi_window_wayland_create (GstVaapiWindow * window,
    guint * width, guint * height)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);
  GstVaapiDisplayWaylandPrivate *const priv_display =
      GST_VAAPI_DISPLAY_WAYLAND_GET_PRIVATE (GST_VAAPI_WINDOW_DISPLAY (window));

  GST_DEBUG ("create window, size %ux%u", *width, *height);

  g_return_val_if_fail (priv_display->compositor != NULL, FALSE);
  g_return_val_if_fail (priv_display->xdg_wm_base || priv_display->wl_shell,
      FALSE);

  GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
  priv->event_queue = wl_display_create_queue (priv_display->wl_display);
  GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
  if (!priv->event_queue)
    return FALSE;

  GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
  priv->surface = wl_compositor_create_surface (priv_display->compositor);
  GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
  if (!priv->surface)
    return FALSE;
  wl_proxy_set_queue ((struct wl_proxy *) priv->surface, priv->event_queue);

  /* Prefer XDG-shell over deprecated wl_shell (if available) */
  if (priv_display->xdg_wm_base) {
    /* Create the XDG surface. We make the toplevel on VaapiWindow::show() */
    GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
    priv->xdg_surface = xdg_wm_base_get_xdg_surface (priv_display->xdg_wm_base,
        priv->surface);
    GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
    if (!priv->xdg_surface)
      return FALSE;
    wl_proxy_set_queue ((struct wl_proxy *) priv->xdg_surface,
        priv->event_queue);
    xdg_surface_add_listener (priv->xdg_surface, &xdg_surface_listener, window);
  } else {
    /* Fall back to wl_shell */
    GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
    priv->wl_shell_surface = wl_shell_get_shell_surface (priv_display->wl_shell,
        priv->surface);
    GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
    if (!priv->wl_shell_surface)
      return FALSE;
    wl_proxy_set_queue ((struct wl_proxy *) priv->wl_shell_surface,
        priv->event_queue);

    wl_shell_surface_add_listener (priv->wl_shell_surface,
        &shell_surface_listener, priv);
    wl_shell_surface_set_toplevel (priv->wl_shell_surface);
  }

  priv->poll = gst_poll_new (TRUE);
  gst_poll_fd_init (&priv->pollfd);

  if (priv->fullscreen_on_show)
    gst_vaapi_window_wayland_set_fullscreen (window, TRUE);

  priv->is_shown = TRUE;

  return TRUE;
}

static void
gst_vaapi_window_wayland_finalize (GObject * object)
{
  GstVaapiWindow *window = GST_VAAPI_WINDOW (object);
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);
  struct wl_display *const wl_display =
      GST_VAAPI_WINDOW_NATIVE_DISPLAY (window);

  /* Make sure that the last wl buffer's callback could be called */
  GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
  if (priv->surface) {
    wl_surface_attach (priv->surface, NULL, 0, 0);
    wl_surface_commit (priv->surface);
    wl_display_flush (wl_display);
  }
  GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);

  gst_poll_set_flushing (priv->poll, TRUE);

  if (priv->event_queue)
    wl_display_roundtrip_queue (wl_display, priv->event_queue);

  g_clear_pointer (&priv->xdg_surface, xdg_surface_destroy);
  g_clear_pointer (&priv->wl_shell_surface, wl_shell_surface_destroy);
  g_clear_pointer (&priv->surface, wl_surface_destroy);
  g_clear_pointer (&priv->event_queue, wl_event_queue_destroy);

  gst_poll_free (priv->poll);

  G_OBJECT_CLASS (gst_vaapi_window_wayland_parent_class)->finalize (object);
}

static gboolean
gst_vaapi_window_wayland_resize (GstVaapiWindow * window,
    guint width, guint height)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);
  GstVaapiDisplayWaylandPrivate *const priv_display =
      GST_VAAPI_DISPLAY_WAYLAND_GET_PRIVATE (GST_VAAPI_WINDOW_DISPLAY (window));

  GST_DEBUG ("resize window, new size %ux%u", width, height);

  if (priv->opaque_region)
    wl_region_destroy (priv->opaque_region);
  GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
  priv->opaque_region = wl_compositor_create_region (priv_display->compositor);
  GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
  wl_region_add (priv->opaque_region, 0, 0, width, height);

  return TRUE;
}

static inline gboolean
frame_done (FrameState * frame)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (frame->window);

  g_atomic_int_set (&frame->done, TRUE);
  if (g_atomic_pointer_compare_and_exchange (&priv->last_frame, frame, NULL))
    return g_atomic_int_dec_and_test (&priv->num_frames_pending);
  return FALSE;
}

static void
frame_done_callback (void *data, struct wl_callback *callback, uint32_t time)
{
  if (!frame_done (data))
    GST_INFO ("cannot remove last frame because it didn't match or empty");
}

static const struct wl_callback_listener frame_callback_listener = {
  frame_done_callback
};

static void
frame_release_callback (void *data, struct wl_buffer *wl_buffer)
{
  FrameState *const frame = data;

  if (!frame->done)
    if (!frame_done (frame))
      GST_INFO ("cannot remove last frame because it didn't match or empty");
  wl_buffer_destroy (wl_buffer);
  frame_state_free (frame);
}

static const struct wl_buffer_listener frame_buffer_listener = {
  frame_release_callback
};

static gboolean
gst_vaapi_window_wayland_render (GstVaapiWindow * window,
    GstVaapiSurface * surface,
    const GstVaapiRectangle * src_rect,
    const GstVaapiRectangle * dst_rect, guint flags)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);
  GstVaapiDisplay *const display = GST_VAAPI_WINDOW_DISPLAY (window);
  struct wl_display *const wl_display =
      GST_VAAPI_WINDOW_NATIVE_DISPLAY (window);
  struct wl_buffer *buffer;
  FrameState *frame;
  guint width, height, va_flags;
  VAStatus status;

  /* Check that we don't need to crop source VA surface */
  gst_vaapi_surface_get_size (surface, &width, &height);
  if (src_rect->x != 0 || src_rect->y != 0)
    priv->need_vpp = TRUE;
  if (src_rect->width != width || src_rect->height != height)
    priv->need_vpp = TRUE;

  /* Check that we don't render to a subregion of this window */
  if (dst_rect->x != 0 || dst_rect->y != 0)
    priv->need_vpp = TRUE;
  if (dst_rect->width != window->width || dst_rect->height != window->height)
    priv->need_vpp = TRUE;

  /* Try to construct a Wayland buffer from VA surface as is (without VPP) */
  if (!priv->need_vpp) {
    GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
    va_flags = from_GstVaapiSurfaceRenderFlags (flags);
    status = vaGetSurfaceBufferWl (GST_VAAPI_DISPLAY_VADISPLAY (display),
        GST_VAAPI_OBJECT_ID (surface),
        va_flags & (VA_TOP_FIELD | VA_BOTTOM_FIELD), &buffer);
    GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
    if (status == VA_STATUS_ERROR_FLAG_NOT_SUPPORTED ||
        status == VA_STATUS_ERROR_UNIMPLEMENTED ||
        status == VA_STATUS_ERROR_INVALID_IMAGE_FORMAT)
      priv->need_vpp = TRUE;
    else if (!vaapi_check_status (status, "vaGetSurfaceBufferWl()"))
      return FALSE;
  }

  /* Try to construct a Wayland buffer with VPP */
  if (priv->need_vpp) {
    if (window->has_vpp) {
      GstVaapiSurface *const vpp_surface =
          gst_vaapi_window_vpp_convert_internal (window, surface, src_rect,
          dst_rect, flags);
      if (G_UNLIKELY (!vpp_surface))
        priv->need_vpp = FALSE;
      else {
        surface = vpp_surface;
        width = window->width;
        height = window->height;
      }
    }

    GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
    status = vaGetSurfaceBufferWl (GST_VAAPI_DISPLAY_VADISPLAY (display),
        GST_VAAPI_OBJECT_ID (surface), VA_FRAME_PICTURE, &buffer);
    GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
    if (!vaapi_check_status (status, "vaGetSurfaceBufferWl()"))
      return FALSE;
  }

  /* Wait for the previous frame to complete redraw */
  if (!gst_vaapi_window_wayland_sync (window)) {
    /* Release vpp surface if exists */
    if (priv->need_vpp && window->has_vpp)
      gst_vaapi_video_pool_put_object (window->surface_pool, surface);
    wl_buffer_destroy (buffer);
    return !priv->sync_failed;
  }

  frame = frame_state_new (window);
  if (!frame)
    return FALSE;
  g_atomic_pointer_set (&priv->last_frame, frame);
  g_atomic_int_inc (&priv->num_frames_pending);

  if (priv->need_vpp && window->has_vpp) {
    frame->surface = surface;
    frame->surface_pool = gst_vaapi_video_pool_ref (window->surface_pool);
  }

  /* XXX: attach to the specified target rectangle */
  GST_VAAPI_WINDOW_LOCK_DISPLAY (window);
  wl_surface_attach (priv->surface, buffer, 0, 0);
  wl_surface_damage (priv->surface, 0, 0, width, height);

  if (priv->opaque_region) {
    wl_surface_set_opaque_region (priv->surface, priv->opaque_region);
    wl_region_destroy (priv->opaque_region);
    priv->opaque_region = NULL;
  }

  wl_proxy_set_queue ((struct wl_proxy *) buffer, priv->event_queue);
  wl_buffer_add_listener (buffer, &frame_buffer_listener, frame);

  frame->callback = wl_surface_frame (priv->surface);
  wl_callback_add_listener (frame->callback, &frame_callback_listener, frame);

  wl_surface_commit (priv->surface);
  wl_display_flush (wl_display);
  GST_VAAPI_WINDOW_UNLOCK_DISPLAY (window);
  return TRUE;
}

static gboolean
gst_vaapi_window_wayland_unblock (GstVaapiWindow * window)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);

  gst_poll_set_flushing (priv->poll, TRUE);

  return TRUE;
}

static gboolean
gst_vaapi_window_wayland_unblock_cancel (GstVaapiWindow * window)
{
  GstVaapiWindowWaylandPrivate *const priv =
      GST_VAAPI_WINDOW_WAYLAND_GET_PRIVATE (window);

  gst_poll_set_flushing (priv->poll, FALSE);

  return TRUE;
}

static void
gst_vaapi_window_wayland_class_init (GstVaapiWindowWaylandClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstVaapiWindowClass *const window_class = GST_VAAPI_WINDOW_CLASS (klass);

  object_class->finalize = gst_vaapi_window_wayland_finalize;

  window_class->create = gst_vaapi_window_wayland_create;
  window_class->show = gst_vaapi_window_wayland_show;
  window_class->hide = gst_vaapi_window_wayland_hide;
  window_class->render = gst_vaapi_window_wayland_render;
  window_class->resize = gst_vaapi_window_wayland_resize;
  window_class->set_fullscreen = gst_vaapi_window_wayland_set_fullscreen;
  window_class->unblock = gst_vaapi_window_wayland_unblock;
  window_class->unblock_cancel = gst_vaapi_window_wayland_unblock_cancel;

  signals[SIZE_CHANGED] = g_signal_new ("size-changed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

static void
gst_vaapi_window_wayland_init (GstVaapiWindowWayland * window)
{
}

/**
 * gst_vaapi_window_wayland_new:
 * @display: a #GstVaapiDisplay
 * @width: the requested window width, in pixels
 * @height: the requested windo height, in pixels
 *
 * Creates a window with the specified @width and @height. The window
 * will be attached to the @display and remains invisible to the user
 * until gst_vaapi_window_show() is called.
 *
 * Return value: the newly allocated #GstVaapiWindow object
 */
GstVaapiWindow *
gst_vaapi_window_wayland_new (GstVaapiDisplay * display,
    guint width, guint height)
{
  g_return_val_if_fail (GST_VAAPI_IS_DISPLAY_WAYLAND (display), NULL);

  return gst_vaapi_window_new_internal (GST_TYPE_VAAPI_WINDOW_WAYLAND, display,
      GST_VAAPI_ID_INVALID, width, height);
}
