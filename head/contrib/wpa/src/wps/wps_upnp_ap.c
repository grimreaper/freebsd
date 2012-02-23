/*
 * Wi-Fi Protected Setup - UPnP AP functionality
 * Copyright (c) 2009, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "includes.h"

#include "common.h"
#include "eloop.h"
#include "uuid.h"
#include "wps_i.h"
#include "wps_upnp.h"
#include "wps_upnp_i.h"


static void upnp_er_set_selected_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct subscription *s = eloop_ctx;
	wpa_printf(MSG_DEBUG, "WPS: SetSelectedRegistrar from ER timed out");
	s->selected_registrar = 0;
	wps_registrar_selected_registrar_changed(s->reg);
}


int upnp_er_set_selected_registrar(struct wps_registrar *reg,
				   struct subscription *s,
				   const struct wpabuf *msg)
{
	struct wps_parse_attr attr;

	wpa_hexdump_buf(MSG_MSGDUMP, "WPS: SetSelectedRegistrar attributes",
			msg);

	if (wps_parse_msg(msg, &attr) < 0)
		return -1;
	if (!wps_version_supported(attr.version)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported SetSelectedRegistrar "
			   "version 0x%x", attr.version ? *attr.version : 0);
		return -1;
	}

	s->reg = reg;
	eloop_cancel_timeout(upnp_er_set_selected_timeout, s, NULL);

	if (attr.selected_registrar == NULL || *attr.selected_registrar == 0) {
		wpa_printf(MSG_DEBUG, "WPS: SetSelectedRegistrar: Disable "
			   "Selected Registrar");
		s->selected_registrar = 0;
	} else {
		s->selected_registrar = 1;
		s->dev_password_id = attr.dev_password_id ?
			WPA_GET_BE16(attr.dev_password_id) : DEV_PW_DEFAULT;
		s->config_methods = attr.sel_reg_config_methods ?
			WPA_GET_BE16(attr.sel_reg_config_methods) : -1;
		eloop_register_timeout(WPS_PBC_WALK_TIME, 0,
				       upnp_er_set_selected_timeout, s, NULL);
	}

	wps_registrar_selected_registrar_changed(reg);

	return 0;
}


void upnp_er_remove_notification(struct subscription *s)
{
	s->selected_registrar = 0;
	eloop_cancel_timeout(upnp_er_set_selected_timeout, s, NULL);
	if (s->reg)
		wps_registrar_selected_registrar_changed(s->reg);
}
