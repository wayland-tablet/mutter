/*
 * Wayland Support
 *
 * Copyright (C) 2015 Red Hat
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#define _GNU_SOURCE

#include "config.h"

#include <glib.h>

#include <wayland-server.h>
#include "wayland-tablet-server-protocol.h"
#include "meta-wayland-private.h"
#include "meta-surface-actor-wayland.h"
#include "meta-wayland-tablet.h"
#include "meta-wayland-tablet-seat.h"
#include "meta-wayland-tablet-tool.h"

static void
unbind_resource (struct wl_resource *resource)
{
  wl_list_remove (wl_resource_get_link (resource));
}

static void
move_resources (struct wl_list *destination,
                struct wl_list *source)
{
  wl_list_insert_list (destination, source);
  wl_list_init (source);
}

static void
move_resources_for_client (struct wl_list   *destination,
                           struct wl_list   *source,
                           struct wl_client *client)
{
  struct wl_resource *resource, *tmp;

  wl_resource_for_each_safe (resource, tmp, source)
    {
      if (wl_resource_get_client (resource) == client)
        {
          wl_list_remove (wl_resource_get_link (resource));
          wl_list_insert (destination, wl_resource_get_link (resource));
        }
    }
}

static void
meta_wayland_tablet_tool_set_focus (MetaWaylandTabletTool *tool,
                                    MetaWaylandSurface    *surface)
{
  guint32 _time;

  if (tool->focus_surface == surface)
    return;

  _time = clutter_get_current_event_time ();

  if (tool->focus_surface != NULL)
    {
      struct wl_resource *resource;
      struct wl_list *l;

      l = &tool->focus_resource_list;
      if (!wl_list_empty (l))
        {
          wl_resource_for_each (resource, l)
            {
              wl_tablet_tool_send_proximity_out (resource, _time);
            }

          move_resources (&tool->resource_list, &tool->focus_resource_list);
        }

      wl_list_remove (&tool->focus_surface_destroy_listener.link);
      tool->focus_surface = NULL;
    }

  if (surface != NULL)
    {
      struct wl_resource *resource, *tablet_resource;
      struct wl_client *client;
      struct wl_list *l;

      tool->focus_surface = surface;
      client = wl_resource_get_client (tool->focus_surface->resource);
      wl_resource_add_destroy_listener (tool->focus_surface->resource,
                                        &tool->focus_surface_destroy_listener);

      move_resources_for_client (&tool->focus_resource_list,
                                 &tool->resource_list, client);

      tablet_resource = meta_wayland_tablet_lookup_resource (tool->current_tablet,
                                                             client);
      l = &tool->focus_resource_list;

      if (!wl_list_empty (l))
        {
          struct wl_client *client = wl_resource_get_client (tool->focus_surface->resource);
          struct wl_display *display = wl_client_get_display (client);

          tool->proximity_serial = wl_display_next_serial (display);

          wl_resource_for_each (resource, l)
            {
              wl_tablet_tool_send_proximity_in (resource, tool->proximity_serial, _time,
                                                tablet_resource, tool->focus_surface->resource);
            }
        }
    }
}

static guint32
input_device_get_capabilities (ClutterInputDevice *device)
{
  ClutterInputAxis axis;
  guint32 capabilities = 0, i;

  for (i = 0; i < clutter_input_device_get_n_axes (device); i++)
    {
      axis = clutter_input_device_get_axis (device, i);

      switch (axis)
        {
        case CLUTTER_INPUT_AXIS_PRESSURE:
          capabilities |= WL_TABLET_TOOL_CAPABILITY_PRESSURE;
          break;
        case CLUTTER_INPUT_AXIS_DISTANCE:
          capabilities |= WL_TABLET_TOOL_CAPABILITY_DISTANCE;
          break;
        case CLUTTER_INPUT_AXIS_XTILT:
        case CLUTTER_INPUT_AXIS_YTILT:
          capabilities |= WL_TABLET_TOOL_CAPABILITY_TILT;
          break;
        default:
          break;
        }
    }

  return capabilities;
}

static enum wl_tablet_tool_type
input_device_tool_get_type (ClutterInputDeviceTool *device_tool)
{
  ClutterInputDeviceToolType tool_type;

  tool_type = clutter_input_device_tool_get_tool_type (device_tool);

  switch (tool_type)
    {
    case CLUTTER_INPUT_DEVICE_TOOL_NONE:
    case CLUTTER_INPUT_DEVICE_TOOL_PEN:
      return WL_TABLET_TOOL_TYPE_PEN;
    case CLUTTER_INPUT_DEVICE_TOOL_ERASER:
      return WL_TABLET_TOOL_TYPE_ERASER;
    case CLUTTER_INPUT_DEVICE_TOOL_BRUSH:
      return WL_TABLET_TOOL_TYPE_BRUSH;
    case CLUTTER_INPUT_DEVICE_TOOL_PENCIL:
      return WL_TABLET_TOOL_TYPE_PENCIL;
    case CLUTTER_INPUT_DEVICE_TOOL_AIRBRUSH:
      return WL_TABLET_TOOL_TYPE_AIRBRUSH;
    case CLUTTER_INPUT_DEVICE_TOOL_FINGER:
      return WL_TABLET_TOOL_TYPE_FINGER;
    case CLUTTER_INPUT_DEVICE_TOOL_MOUSE:
      return WL_TABLET_TOOL_TYPE_MOUSE;
    case CLUTTER_INPUT_DEVICE_TOOL_LENS:
      return WL_TABLET_TOOL_TYPE_LENS;
    }

  g_assert_not_reached ();
  return 0;
}

static void
tablet_tool_handle_focus_surface_destroy (struct wl_listener *listener,
                                          void               *data)
{
  MetaWaylandTabletTool *tool;

  tool = wl_container_of (listener, tool, focus_surface_destroy_listener);
  meta_wayland_tablet_tool_set_focus (tool, NULL);
}

MetaWaylandTabletTool *
meta_wayland_tablet_tool_new (MetaWaylandTabletSeat  *seat,
                              ClutterInputDevice     *device,
                              ClutterInputDeviceTool *device_tool)
{
  MetaWaylandTabletTool *tool;

  tool = g_slice_new0 (MetaWaylandTabletTool);
  tool->seat = seat;
  tool->device = device;
  tool->device_tool = device_tool;
  wl_list_init (&tool->resource_list);
  wl_list_init (&tool->focus_resource_list);

  tool->focus_surface_destroy_listener.notify = tablet_tool_handle_focus_surface_destroy;

  return tool;
}

void
meta_wayland_tablet_tool_free (MetaWaylandTabletTool *tool)
{
  struct wl_resource *resource, *next;

  meta_wayland_tablet_tool_set_focus (tool, NULL);

  wl_resource_for_each_safe (resource, next, &tool->resource_list)
    {
      wl_tablet_tool_send_removed (resource);
      wl_resource_destroy (resource);
    }

  g_slice_free (MetaWaylandTabletTool, tool);
}

static void
tool_set_cursor (struct wl_client   *client,
                 struct wl_resource *resource,
                 uint32_t            serial,
                 struct wl_resource *surface_resource,
                 int32_t             hotspot_x,
                 int32_t             hotspot_y,
                 struct wl_resource *tablet_resource)
{
}

static void
tool_destroy (struct wl_client   *client,
              struct wl_resource *resource)
{
  wl_resource_destroy (resource);
}

static const struct wl_tablet_tool_interface tool_interface = {
  tool_set_cursor,
  tool_destroy
};

static void
meta_wayland_tablet_tool_notify_details (MetaWaylandTabletTool *tool,
                                         struct wl_resource    *resource)
{
  guint64 serial;

  wl_tablet_tool_send_type (resource,
                            input_device_tool_get_type (tool->device_tool));

  serial = (guint64) clutter_input_device_tool_get_serial (tool->device_tool);
  wl_tablet_tool_send_serial_id (resource, (uint32_t) (serial >> 32),
                                 (uint32_t) (serial & G_MAXUINT32));

  wl_tablet_tool_send_capability (resource,
                                  input_device_get_capabilities (tool->device));

  /* FIXME: wl_tablet_tool.hardware_id missing */

  wl_tablet_tool_send_done (resource);
}

static void
emit_proximity_in (MetaWaylandTabletTool *tool,
                   struct wl_resource    *resource)
{
  struct wl_resource *tablet_resource;
  struct wl_client *client;
  guint32 _time;

  if (!tool->focus_surface)
    return;

  _time = clutter_get_current_event_time ();
  client = wl_resource_get_client (resource);
  tablet_resource = meta_wayland_tablet_lookup_resource (tool->current_tablet,
                                                         client);

  wl_tablet_tool_send_proximity_in (resource, tool->proximity_serial, _time,
                                    tablet_resource, tool->focus_surface->resource);
}

struct wl_resource *
meta_wayland_tablet_tool_create_new_resource (MetaWaylandTabletTool *tool,
                                              struct wl_client      *client,
                                              struct wl_resource    *seat_resource,
                                              uint32_t               id)
{
  struct wl_resource *resource;

  resource = wl_resource_create (client, &wl_tablet_tool_interface,
                                 wl_resource_get_version (seat_resource), id);
  wl_resource_set_implementation (resource, &tool_interface,
                                  tool, unbind_resource);
  wl_resource_set_user_data (resource, tool);

  if (tool->focus_surface &&
      wl_resource_get_client (tool->focus_surface->resource) == client)
    {
      wl_list_insert (&tool->focus_resource_list, wl_resource_get_link (resource));
      emit_proximity_in (tool, resource);
    }
  else
    {
      wl_list_insert (&tool->resource_list, wl_resource_get_link (resource));
    }

  meta_wayland_tablet_tool_notify_details (tool, resource);

  return resource;
}

struct wl_resource *
meta_wayland_tablet_tool_lookup_resource (MetaWaylandTabletTool *tool,
                                          struct wl_client      *client)
{
  struct wl_resource *resource = NULL;

  if (!wl_list_empty (&tool->resource_list))
    resource = wl_resource_find_for_client (&tool->resource_list, client);

  if (!wl_list_empty (&tool->focus_resource_list))
    resource = wl_resource_find_for_client (&tool->focus_resource_list, client);

  return resource;
}

static void
meta_wayland_tablet_tool_account_button (MetaWaylandTabletTool *tool,
                                         const ClutterEvent    *event)
{
  if (event->type == CLUTTER_BUTTON_PRESS)
    tool->pressed_buttons |= 1 << (event->button.button - 1);
  else if (event->type == CLUTTER_BUTTON_RELEASE)
    tool->pressed_buttons &= ~(1 << (event->button.button - 1));
}

static void
sync_focus_surface (MetaWaylandTabletTool *tool)
{
  MetaDisplay *display = meta_get_display ();

  switch (display->event_route)
    {
    case META_EVENT_ROUTE_WINDOW_OP:
    case META_EVENT_ROUTE_COMPOSITOR_GRAB:
    case META_EVENT_ROUTE_FRAME_BUTTON:
      /* The compositor has a grab, so remove our focus */
      meta_wayland_tablet_tool_set_focus (tool, NULL);
      break;

    case META_EVENT_ROUTE_NORMAL:
    case META_EVENT_ROUTE_WAYLAND_POPUP:
      meta_wayland_tablet_tool_set_focus (tool, tool->current);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
repick_for_event (MetaWaylandTabletTool *tool,
                  const ClutterEvent    *for_event)
{
  ClutterActor *actor = NULL;

  actor = clutter_event_get_source (for_event);

  if (META_IS_SURFACE_ACTOR_WAYLAND (actor))
    tool->current = meta_surface_actor_wayland_get_surface (META_SURFACE_ACTOR_WAYLAND (actor));
  else
    tool->current = NULL;

  sync_focus_surface (tool);
}

void
meta_wayland_tablet_tool_update (MetaWaylandTabletTool *tool,
                                 const ClutterEvent    *event)
{
  switch (event->type)
    {
    case CLUTTER_BUTTON_PRESS:
    case CLUTTER_BUTTON_RELEASE:
      meta_wayland_tablet_tool_account_button (tool, event);
      break;
    case CLUTTER_MOTION:
      if (!tool->pressed_buttons)
        repick_for_event (tool, event);
      break;
    case CLUTTER_PROXIMITY_IN:
      tool->current_tablet =
        meta_wayland_tablet_seat_lookup_tablet (tool->seat,
                                                clutter_event_get_source_device (event));
      break;
    default:
      break;
    }
}

gboolean
meta_wayland_tablet_tool_handle_event (MetaWaylandTabletTool *tool,
                                       const ClutterEvent    *event)
{
  switch (event->type)
    {
    case CLUTTER_PROXIMITY_IN:
      /* We don't have much info here to make anything useful out of it,
       * wait until the first motion event so we have both coordinates
       * and tool.
       */
      break;
    case CLUTTER_PROXIMITY_OUT:
      meta_wayland_tablet_tool_set_focus (tool, NULL);
      break;
    default:
      return CLUTTER_EVENT_PROPAGATE;
    }

  return CLUTTER_EVENT_STOP;
}
