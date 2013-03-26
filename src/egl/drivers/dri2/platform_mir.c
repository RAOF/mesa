/*
 * Copyright © 2012 Canonical, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
 */

#include <mir_toolkit/mir_client_library.h>
#include <mir_toolkit/mesa/native_display.h>

#include "egl_dri2.h"

#include <stdlib.h>
#include <string.h>

static __DRIbuffer *
dri2_get_buffers_with_format(__DRIdrawable * driDrawable,
			     int *width, int *height,
			     unsigned int *attachments, int count,
			     int *out_count, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   int i;

   dri2_surf->buffer_count = 0;
   for (i = 0; i < 2*count; i+=2) {
      assert(attachments[i] < __DRI_BUFFER_COUNT);
      assert(dri2_surf->buffer_count < 5);

      if (dri2_surf->dri_buffers[attachments[i]] == NULL) {
         /* Our frame callback must keep these buffers valid */
         assert(attachments[i] != __DRI_BUFFER_FRONT_LEFT);
         assert(attachments[i] != __DRI_BUFFER_BACK_LEFT);

         dri2_surf->dri_buffers[attachments[i]] =
            dri2_dpy->dri2->allocateBuffer(dri2_dpy->dri_screen,
                  attachments[i], attachments[i+1],
                  dri2_surf->base.Width, dri2_surf->base.Height);

         if (!dri2_surf->dri_buffers[attachments[i]]) 
            continue;
      }

      memcpy(&dri2_surf->buffers[dri2_surf->buffer_count],
             dri2_surf->dri_buffers[attachments[i]],
             sizeof(__DRIbuffer));

      dri2_surf->buffer_count++;
   }

   assert(dri2_surf->base.Type == EGL_PIXMAP_BIT ||
          dri2_surf->dri_buffers[__DRI_BUFFER_BACK_LEFT]);

   *out_count = dri2_surf->buffer_count;
   if (dri2_surf->buffer_count == 0)
	   return NULL;

   *width = dri2_surf->base.Width;
   *height = dri2_surf->base.Height;

   return dri2_surf->buffers;
}

static __DRIbuffer *
dri2_get_buffers(__DRIdrawable * driDrawable,
		 int *width, int *height,
		 unsigned int *attachments, int count,
		 int *out_count, void *loaderPrivate)
{
   unsigned int *attachments_with_format;
   __DRIbuffer *buffer;
   const unsigned int format = 32;
   int i;

   attachments_with_format = calloc(count * 2, sizeof(unsigned int));
   if (!attachments_with_format) {
      *out_count = 0;
      return NULL;
   }

   for (i = 0; i < count; ++i) {
      attachments_with_format[2*i] = attachments[i];
      attachments_with_format[2*i + 1] = format;
   }

   buffer =
      dri2_get_buffers_with_format(driDrawable,
				   width, height,
				   attachments_with_format, count,
				   out_count, loaderPrivate);

   free(attachments_with_format);

   return buffer;
}


static void
dri2_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
   (void) driDrawable;

   /* FIXME: Does EGL support front buffer rendering at all? */

#if 0
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   dri2WaitGL(dri2_surf);
#else
   (void) loaderPrivate;
#endif
}

static void
mir_copy_back_to_front(struct dri2_egl_surface *surf)
{
   memcpy(surf->dri_buffers[__DRI_BUFFER_FRONT_LEFT],
          surf->dri_buffers[__DRI_BUFFER_BACK_LEFT],
          sizeof(*surf->dri_buffers[__DRI_BUFFER_BACK_LEFT]));

   surf->dri_buffers[__DRI_BUFFER_FRONT_LEFT]->attachment =
      __DRI_BUFFER_FRONT_LEFT;
}

static void
mir_populate_colour_buffers(struct dri2_egl_surface *surf)
{
   MirBufferPackage buffer_package;

   surf->mir_disp->surface_get_current_buffer(surf->mir_disp,
                                              (EGLNativeWindowType)surf->mir_surf,
                                              &buffer_package);
   /* We expect [name, pitch] in our buffer */
   assert(buffer_package.data_items == 1);

   /* Frontbuffer hack; Mir doesn't give us access to the front buffer */
   mir_copy_back_to_front(surf);

   surf->dri_buffers[__DRI_BUFFER_BACK_LEFT]->name = buffer_package.data[0];
   /* Man, I hope that Intel's just being funky when they multiply pitch by
      cpp */
   surf->dri_buffers[__DRI_BUFFER_BACK_LEFT]->pitch = buffer_package.stride;
}

/**
 * Called via eglCreateWindowSurface(), drv->API.CreateWindowSurface().
 */
static _EGLSurface *
dri2_create_mir_window_surface(_EGLDriver *drv, _EGLDisplay *disp,
                               _EGLConfig *conf, EGLNativeWindowType window,
                               const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   MirSurfaceParameters surf_params;

   (void) drv;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }
   
   if (!_eglInitSurface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf, attrib_list))
      goto cleanup_surf;

   // TODO: Strange to copy this...
   dri2_surf->mir_disp = dri2_dpy->mir_disp;
   dri2_surf->mir_surf = (MirSurface *)window;
   dri2_surf->mir_disp->surface_get_parameters(dri2_surf->mir_disp, (EGLNativeWindowType)dri2_surf->mir_surf,
                                               &surf_params);
   dri2_surf->base.Width = surf_params.width;
   dri2_surf->base.Height = surf_params.height;

   dri2_surf->dri_buffers[__DRI_BUFFER_FRONT_LEFT] = 
      calloc(sizeof(*dri2_surf->dri_buffers[0]), 1);
   dri2_surf->dri_buffers[__DRI_BUFFER_BACK_LEFT] =
      calloc(sizeof(*dri2_surf->dri_buffers[0]), 1);
   
   dri2_surf->dri_buffers[__DRI_BUFFER_BACK_LEFT]->attachment = 
      __DRI_BUFFER_BACK_LEFT;
   /* We only do ARGB 8888 for the moment */
   dri2_surf->dri_buffers[__DRI_BUFFER_BACK_LEFT]->cpp = 4;

   mir_populate_colour_buffers(dri2_surf);
   /* Hack: this is not actually the front buffer, but we can't get it */
   mir_copy_back_to_front(dri2_surf);

   dri2_surf->dri_drawable = 
      (*dri2_dpy->dri2->createNewDrawable) (dri2_dpy->dri_screen,
                                            dri2_conf->dri_double_config,
                                            dri2_surf);

   if (dri2_surf->dri_drawable == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2->createNewDrawable");
   }
   
   return &dri2_surf->base;

cleanup_surf:
   free(dri2_surf);
   return NULL;
}

static EGLBoolean
dri2_destroy_mir_surface(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int i;

   (void) drv;

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

   (*dri2_dpy->core->destroyDrawable)(dri2_surf->dri_drawable);

   for (i = 0; i < __DRI_BUFFER_COUNT; ++i) {
      if (dri2_surf->dri_buffers[i] && !((i == __DRI_BUFFER_FRONT_LEFT) ||
                                         (i == __DRI_BUFFER_BACK_LEFT))) {
         dri2_dpy->dri2->releaseBuffer(dri2_dpy->dri_screen,
                                       dri2_surf->dri_buffers[i]);
      }
   }

   free(surf);

   return EGL_TRUE;
}

/**
 * Called via eglSwapBuffers(), drv->API.SwapBuffers().
 */
static EGLBoolean
dri2_swap_buffers(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);

   dri2_surf->mir_disp->surface_advance_buffer(dri2_surf->mir_disp, (EGLNativeWindowType)dri2_surf->mir_surf);

   mir_populate_colour_buffers(dri2_surf);

   (*dri2_dpy->flush->flush)(dri2_surf->dri_drawable);
   (*dri2_dpy->flush->invalidate)(dri2_surf->dri_drawable);

   return EGL_TRUE;
}

static void connection_callback(MirConnection *conn, void *context)
{
   struct dri2_egl_display *dri2_dpy = context;

   dri2_dpy->mir_disp = (MirMesaEGLNativeDisplay *)mir_connection_get_egl_native_display(conn);
}

static int
dri2_mir_authenticate(_EGLDisplay *disp, uint32_t id)
{
   return 0;
}

EGLBoolean
dri2_initialize_mir(_EGLDriver *drv, _EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy;
   MirPlatformPackage platform;
   const __DRIconfig *config;
   static const unsigned int argb_masks[4] =
      { 0xff0000, 0xff00, 0xff, 0xff000000 };
   uint32_t types;
   int i;

   drv->API.CreateWindowSurface = dri2_create_mir_window_surface;
   drv->API.DestroySurface = dri2_destroy_mir_surface;
   drv->API.SwapBuffers = dri2_swap_buffers;
/*   drv->API.CreatePixmapSurface = dri2_create_pixmap_surface;
   drv->API.CreatePbufferSurface = dri2_create_pbuffer_surface;
   drv->API.CopyBuffers = dri2_copy_buffers;
   drv->API.CreateImageKHR = dri2_x11_create_image_khr;
   drv->API.SwapInterval = dri2_swap_interval;
*/

   dri2_dpy = calloc(1, sizeof *dri2_dpy);
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *) dri2_dpy;
   if (disp->PlatformDisplay == NULL) {
      mir_wait_for(mir_connect("some_socket_file", "EGL Client",
                               connection_callback, dri2_dpy));
   } else {
      dri2_dpy->mir_disp = (MirConnection *)disp->PlatformDisplay;
   }


// TODO: What to do about mir_connection_is_valid?
//   if (!mir_connection_is_valid(dri2_dpy->mir_conn)) {
//      _eglLog(_EGL_WARNING, "DRI2: mir_connect failed: %s",
//              mir_connection_get_error_message(dri2_dpy->mir_conn));
//      goto cleanup_dpy;
//   }

   dri2_dpy->mir_disp->display_get_platform(dri2_dpy->mir_disp, &platform);
   dri2_dpy->fd = platform.fd[0];
   dri2_dpy->driver_name = dri2_get_driver_for_fd(dri2_dpy->fd);
   dri2_dpy->device_name = dri2_get_device_name_for_fd(dri2_dpy->fd);

   if (dri2_dpy->driver_name == NULL ||
       dri2_dpy->device_name == NULL)
      goto cleanup_conn;

   if (!dri2_load_driver(disp))
      goto cleanup_conn;

   dri2_dpy->dri2_loader_extension.base.name = __DRI_DRI2_LOADER;
   dri2_dpy->dri2_loader_extension.base.version = 3;
   dri2_dpy->dri2_loader_extension.getBuffers = dri2_get_buffers;
   dri2_dpy->dri2_loader_extension.flushFrontBuffer = dri2_flush_front_buffer;
   dri2_dpy->dri2_loader_extension.getBuffersWithFormat =
      dri2_get_buffers_with_format;
      
   dri2_dpy->extensions[0] = &dri2_dpy->dri2_loader_extension.base;
   dri2_dpy->extensions[1] = &image_lookup_extension.base;
   dri2_dpy->extensions[2] = &use_invalidate.base;
   dri2_dpy->extensions[3] = NULL;

   if (!dri2_create_screen(disp))
      goto cleanup_conn;

   types = EGL_WINDOW_BIT;
   for (i = 0; dri2_dpy->driver_configs[i]; i++) {
      config = dri2_dpy->driver_configs[i];
      dri2_add_config(disp, config, i + 1, 0, types, NULL, argb_masks);
   }

   dri2_dpy->authenticate = dri2_mir_authenticate;

   disp->VersionMajor = 1;
   disp->VersionMinor = 4;

   return EGL_TRUE;

 cleanup_conn:
   // TODO: Release NativeDisplay...
//   if (disp->PlatformDisplay == NULL)
//      mir_connection_release(dri2_dpy->mir_conn);
 cleanup_dpy:
   free(dri2_dpy);

   return EGL_FALSE;
}
