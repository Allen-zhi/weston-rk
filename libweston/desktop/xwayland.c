/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 * Copyright © 2016 Quentin "Sardem FF7" Glidic
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <assert.h>

#include <wayland-server.h>

#include <libweston/libweston.h>
#include <libweston/zalloc.h>

#include <libweston/desktop.h>
#include "internal.h"
#include "xwayland/xwayland-internal-interface.h"

enum weston_desktop_xwayland_surface_state {
	NONE,
	TOPLEVEL,
	MAXIMIZED,
	FULLSCREEN,
	TRANSIENT,
	XWAYLAND,
};

struct weston_desktop_xwayland {
	struct weston_desktop *desktop;
	struct weston_desktop_client *client;
	struct weston_layer layer;
};

struct weston_desktop_xwayland_surface {
	struct weston_desktop_xwayland *xwayland;
	struct weston_desktop *desktop;
	struct weston_desktop_surface *surface;
	struct wl_listener resource_destroy_listener;
	struct weston_view *view;
	const struct weston_xwayland_client_interface *client_interface;
	struct weston_geometry next_geometry;
	bool has_next_geometry;
	bool committed;
	bool added;
	enum weston_desktop_xwayland_surface_state state;
	enum weston_desktop_xwayland_surface_state prev_state;
	bool state_updated;
};

static void
weston_desktop_xwayland_surface_change_state(struct weston_desktop_xwayland_surface *surface,
					     enum weston_desktop_xwayland_surface_state state,
					     struct weston_desktop_surface *parent,
					     const struct weston_coord_surface *offset)
{
	struct weston_surface *wsurface;
	bool to_add = (parent == NULL && state != XWAYLAND);

	assert(state != NONE);
	assert(!parent || state == TRANSIENT);
	assert(!parent || offset);

	if (to_add && surface->added) {
		surface->state = state;
		return;
	}

	wsurface = weston_desktop_surface_get_surface(surface->surface);
	/* In some cases when adding a surface, the state may be updated by the
	 * shell (e.g., to fullscreen). Track if this has happened and respect
	 * the updated value. */
	surface->state_updated = false;

	if (surface->state != state) {
		if (surface->state == XWAYLAND) {
			assert(!surface->added);

			weston_desktop_surface_unlink_view(surface->view);
			weston_view_destroy(surface->view);
			surface->view = NULL;
			weston_surface_unmap(wsurface);
		}

		if (to_add) {
			weston_desktop_surface_unset_relative_to(surface->surface);
			weston_desktop_api_surface_added(surface->desktop,
							 surface->surface);
			surface->added = true;
			if (surface->state == NONE && surface->committed)
				/* We had a race, and wl_surface.commit() was
				 * faster, just fake a commit to map the
				 * surface */
				weston_desktop_api_committed(surface->desktop,
							     surface->surface,
							     0, 0);

		} else if (surface->added) {
			weston_desktop_api_surface_removed(surface->desktop,
							   surface->surface);
			surface->added = false;
		}

		if (state == XWAYLAND) {
			assert(!surface->added);

			surface->view =
				weston_desktop_surface_create_view(surface->surface);
			weston_layer_entry_insert(&surface->xwayland->layer.view_list,
						  &surface->view->layer_link);
			surface->view->is_mapped = true;
			weston_surface_map(wsurface);
		}

		/* If the surface state was updated by the shell during this
		 * call, respect the updated value, otherwise assign the new
		 * value. */
		if (!surface->state_updated) {
			surface->state = state;
			surface->state_updated = true;
		}
	}

	if (parent != NULL) {
		struct weston_surface *psurface;

		psurface = weston_desktop_surface_get_surface(parent);
		assert(offset->coordinate_space_id == psurface);
		weston_desktop_surface_set_relative_to(surface->surface, parent,
						       offset->c.x,
						       offset->c.y, false);
	}
}

static void
weston_desktop_xwayland_surface_committed(struct weston_desktop_surface *dsurface,
					  void *user_data,
					  int32_t sx, int32_t sy)
{
	struct weston_desktop_xwayland_surface *surface = user_data;
	struct weston_geometry oldgeom;

	assert(dsurface == surface->surface);
	surface->committed = true;

#ifdef WM_DEBUG
	weston_log("%s: xwayland surface %p\n", __func__, surface);
#endif

	if (surface->has_next_geometry) {
		oldgeom = weston_desktop_surface_get_geometry(surface->surface);
		/* If we're transitioning away from fullscreen or maximized
		 * we've moved to old saved co-ordinates that were saved
		 * with window geometry in place, so avoid adajusting by
		 * the geometry in those cases.
		 */
		if (surface->state == surface->prev_state) {
			sx -= surface->next_geometry.x - oldgeom.x;
			sy -= surface->next_geometry.y - oldgeom.y;
		}
		surface->prev_state = surface->state;

		surface->has_next_geometry = false;
		weston_desktop_surface_set_geometry(surface->surface,
						    surface->next_geometry);
	}

	if (surface->added)
		weston_desktop_api_committed(surface->desktop, surface->surface,
					     sx, sy);

	/* If we're an override redirect window, the shell has no knowledge of
	 * our existence, so it won't assign us an output.
	 *
	 * We should already have dirty geometry if we're a new view, so just
	 * update the transform to get us an output assigned, or we won't
	 * cause a repaint.
	 */
	if (surface->state == XWAYLAND)
		weston_view_update_transform(surface->view);
}

static void
weston_desktop_xwayland_surface_set_size(struct weston_desktop_surface *dsurface,
					 void *user_data,
					 int32_t width, int32_t height)
{
	struct weston_desktop_xwayland_surface *surface = user_data;
	struct weston_surface *wsurface =
		weston_desktop_surface_get_surface(surface->surface);

	surface->client_interface->send_configure(wsurface, width, height);
}

static void
weston_desktop_xwayland_surface_set_fullscreen(struct weston_desktop_surface *dsurface,
					       void *user_data, bool fullscreen)
{
	struct weston_desktop_xwayland_surface *surface = user_data;
	struct weston_surface *wsurface =
		weston_desktop_surface_get_surface(surface->surface);

	surface->state = fullscreen ? FULLSCREEN : TOPLEVEL;
	surface->state_updated = true;
	surface->client_interface->send_fullscreen(wsurface, fullscreen);
}

static void
weston_desktop_xwayland_surface_destroy(struct weston_desktop_surface *dsurface,
					void *user_data)
{
	struct weston_desktop_xwayland_surface *surface = user_data;

	wl_list_remove(&surface->resource_destroy_listener.link);

	weston_desktop_surface_unset_relative_to(surface->surface);
	if (surface->added)
		weston_desktop_api_surface_removed(surface->desktop,
						   surface->surface);
	else if (surface->state == XWAYLAND)
		weston_desktop_surface_unlink_view(surface->view);

	free(surface);
}

static void
weston_desktop_xwayland_surface_close(struct weston_desktop_surface *dsurface,
					void *user_data)
{
	struct weston_desktop_xwayland_surface *surface = user_data;
	struct weston_surface *wsurface =
		weston_desktop_surface_get_surface(surface->surface);

	surface->client_interface->send_close(wsurface);
}

static bool
weston_desktop_xwayland_surface_get_maximized(struct weston_desktop_surface *dsurface,
					      void *user_data)
{
	struct weston_desktop_xwayland_surface *surface = user_data;

	return surface->state == MAXIMIZED;
}

static bool
weston_desktop_xwayland_surface_get_fullscreen(struct weston_desktop_surface *dsurface,
					       void *user_data)
{
	struct weston_desktop_xwayland_surface *surface = user_data;

	return surface->state == FULLSCREEN;
}

static const struct weston_desktop_surface_implementation weston_desktop_xwayland_surface_internal_implementation = {
	.committed = weston_desktop_xwayland_surface_committed,
	.set_size = weston_desktop_xwayland_surface_set_size,
	.set_fullscreen = weston_desktop_xwayland_surface_set_fullscreen,

	.get_maximized = weston_desktop_xwayland_surface_get_maximized,
	.get_fullscreen = weston_desktop_xwayland_surface_get_fullscreen,

	.destroy = weston_desktop_xwayland_surface_destroy,
	.close = weston_desktop_xwayland_surface_close,
};

static void
weston_destop_xwayland_resource_destroyed(struct wl_listener *listener,
					  void *data)
{
	struct weston_desktop_xwayland_surface *surface =
		wl_container_of(listener, surface, resource_destroy_listener);

	weston_desktop_surface_destroy(surface->surface);
}

static struct weston_desktop_xwayland_surface *
create_surface(struct weston_desktop_xwayland *xwayland,
	       struct weston_surface *wsurface,
	       const struct weston_xwayland_client_interface *client_interface)
{
	struct weston_desktop_xwayland_surface *surface;

	surface = zalloc(sizeof(struct weston_desktop_xwayland_surface));
	if (surface == NULL)
		return NULL;

	surface->xwayland = xwayland;
	surface->desktop = xwayland->desktop;
	surface->client_interface = client_interface;

	surface->surface =
		weston_desktop_surface_create(surface->desktop,
					      xwayland->client, wsurface,
					      &weston_desktop_xwayland_surface_internal_implementation,
					      surface);
	if (surface->surface == NULL) {
		free(surface);
		return NULL;
	}

	surface->resource_destroy_listener.notify =
		weston_destop_xwayland_resource_destroyed;
	wl_resource_add_destroy_listener(wsurface->resource,
					 &surface->resource_destroy_listener);

	weston_desktop_surface_set_pid(surface->surface, 0);

	return surface;
}

static void
set_toplevel(struct weston_desktop_xwayland_surface *surface)
{
	enum weston_desktop_xwayland_surface_state prev_state = surface->state;

	weston_desktop_xwayland_surface_change_state(surface, TOPLEVEL, NULL,
						     NULL);

	if (prev_state == FULLSCREEN) {
		weston_desktop_api_fullscreen_requested(surface->desktop,
							surface->surface, false,
							NULL);
	}
}

static void
set_toplevel_with_position(struct weston_desktop_xwayland_surface *surface,
			   struct weston_coord_global pos)
{
	set_toplevel(surface);
	weston_desktop_api_set_xwayland_position(surface->desktop,
						 surface->surface,
						 pos.c.x, pos.c.y);
}

static void
set_parent(struct weston_desktop_xwayland_surface *surface,
	   struct weston_surface *wparent)
{
	struct weston_desktop_surface *parent;

	if (!weston_surface_is_desktop_surface(wparent))
		return;

	parent = weston_surface_get_desktop_surface(wparent);
	weston_desktop_api_set_parent(surface->desktop, surface->surface, parent);
}

static void
set_transient(struct weston_desktop_xwayland_surface *surface,
	      struct weston_surface *wparent,
	      struct weston_coord_surface offset)
{
	struct weston_desktop_surface *parent;

	if (!weston_surface_is_desktop_surface(wparent))
		return;

	parent = weston_surface_get_desktop_surface(wparent);
	weston_desktop_xwayland_surface_change_state(surface, TRANSIENT, parent,
						     &offset);
}

static void
set_fullscreen(struct weston_desktop_xwayland_surface *surface,
	       struct weston_output *output)
{
	weston_desktop_xwayland_surface_change_state(surface, FULLSCREEN, NULL,
						     NULL);
	weston_desktop_api_fullscreen_requested(surface->desktop,
						surface->surface, true, output);
}

static void
set_xwayland(struct weston_desktop_xwayland_surface *surface,
	     struct weston_coord_global pos)
{
	weston_desktop_xwayland_surface_change_state(surface, XWAYLAND, NULL,
						     NULL);
	weston_view_set_position(surface->view, pos);
}

static int
move(struct weston_desktop_xwayland_surface *surface,
     struct weston_pointer *pointer)
{
	if (surface->state == TOPLEVEL ||
	    surface->state == MAXIMIZED ||
	    surface->state == FULLSCREEN)
		weston_desktop_api_move(surface->desktop, surface->surface,
					pointer->seat, pointer->grab_serial);
	return 0;
}

static int
resize(struct weston_desktop_xwayland_surface *surface,
       struct weston_pointer *pointer, uint32_t edges)
{
	if (surface->state == TOPLEVEL ||
	    surface->state == MAXIMIZED ||
	    surface->state == FULLSCREEN)
		weston_desktop_api_resize(surface->desktop, surface->surface,
					  pointer->seat, pointer->grab_serial,
					  edges);
	return 0;
}

static void
set_title(struct weston_desktop_xwayland_surface *surface, const char *title)
{
	weston_desktop_surface_set_title(surface->surface, title);
}

static void
set_window_geometry(struct weston_desktop_xwayland_surface *surface,
		    int32_t x, int32_t y, int32_t width, int32_t height)
{
	surface->has_next_geometry = true;
	surface->next_geometry.x = x;
	surface->next_geometry.y = y;
	surface->next_geometry.width = width;
	surface->next_geometry.height = height;
}

static void
set_maximized(struct weston_desktop_xwayland_surface *surface)
{
	weston_desktop_xwayland_surface_change_state(surface, MAXIMIZED, NULL,
						     NULL);
	weston_desktop_api_maximized_requested(surface->desktop,
					       surface->surface, true);
}

static void
set_minimized(struct weston_desktop_xwayland_surface *surface)
{
	weston_desktop_api_minimized_requested(surface->desktop,
					       surface->surface);
}

static void
set_pid(struct weston_desktop_xwayland_surface *surface, pid_t pid)
{
	weston_desktop_surface_set_pid(surface->surface, pid);
}

static void
get_position(struct weston_desktop_xwayland_surface *surface,
	     int32_t *x, int32_t *y)
{
	if (!surface->surface) {
		*x = 0;
		*y = 0;
		return;
	}
	weston_desktop_api_get_position(surface->desktop, surface->surface, x, y);
}

static const struct weston_desktop_xwayland_interface weston_desktop_xwayland_interface = {
	.create_surface = create_surface,
	.set_toplevel = set_toplevel,
	.set_toplevel_with_position = set_toplevel_with_position,
	.set_parent = set_parent,
	.set_transient = set_transient,
	.set_fullscreen = set_fullscreen,
	.set_xwayland = set_xwayland,
	.move = move,
	.resize = resize,
	.set_title = set_title,
	.set_window_geometry = set_window_geometry,
	.set_maximized = set_maximized,
	.set_minimized = set_minimized,
	.set_pid = set_pid,
	.get_position = get_position,
};

void
weston_desktop_xwayland_init(struct weston_desktop *desktop)
{
	struct weston_compositor *compositor = weston_desktop_get_compositor(desktop);
	struct weston_desktop_xwayland *xwayland;

	xwayland = zalloc(sizeof(struct weston_desktop_xwayland));
	if (xwayland == NULL)
		return;

	xwayland->desktop = desktop;
	xwayland->client = weston_desktop_client_create(desktop, NULL, NULL, NULL, NULL, 0, 0);

	weston_layer_init(&xwayland->layer, compositor);
	/* This is the layer we use for override redirect "windows", which
	 * ends up used for tooltips and drop down menus, among other things.
	 * Previously this was WESTON_LAYER_POSITION_NORMAL + 1, but this is
	 * below the fullscreen layer, so fullscreen apps would be above their
	 * menus and tooltips.
	 *
	 * Moving this to just below the TOP_UI layer ensures visibility at all
	 * times, with the minor drawback that they could be rendered above
	 * DESKTOP_UI.
	 *
	 * For tooltips with no transient window hints, this is probably the best
	 * we can do.
	 */
	weston_layer_set_position(&xwayland->layer,
				  WESTON_LAYER_POSITION_TOP_UI - 1);

	compositor->xwayland = xwayland;
	compositor->xwayland_interface = &weston_desktop_xwayland_interface;
}

void
weston_desktop_xwayland_fini(struct weston_desktop *desktop)
{
	struct weston_compositor *compositor = weston_desktop_get_compositor(desktop);
	struct weston_desktop_xwayland *xwayland;

	xwayland = compositor->xwayland;

	weston_desktop_client_destroy(xwayland->client);
	weston_layer_fini(&xwayland->layer);
	free(xwayland);

	compositor->xwayland = NULL;
	compositor->xwayland_interface = NULL;
}
