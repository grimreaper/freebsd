/*
 * hostapd / UNIX domain socket -based control interface
 * Copyright (c) 2004-2009, Jouni Malinen <j@w1.fi>
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

#include "utils/includes.h"

#ifndef CONFIG_NATIVE_WINDOWS

#include <sys/un.h>
#include <sys/stat.h>
#include <stddef.h>

#include "utils/common.h"
#include "utils/eloop.h"
#include "common/ieee802_11_defs.h"
#include "drivers/driver.h"
#include "radius/radius_client.h"
#include "ap/hostapd.h"
#include "ap/ap_config.h"
#include "ap/ieee802_1x.h"
#include "ap/wpa_auth.h"
#include "ap/ieee802_11.h"
#include "ap/sta_info.h"
#include "ap/accounting.h"
#include "ap/wps_hostapd.h"
#include "ap/ctrl_iface_ap.h"
#include "ctrl_iface.h"


struct wpa_ctrl_dst {
	struct wpa_ctrl_dst *next;
	struct sockaddr_un addr;
	socklen_t addrlen;
	int debug_level;
	int errors;
};


static void hostapd_ctrl_iface_send(struct hostapd_data *hapd, int level,
				    const char *buf, size_t len);


static int hostapd_ctrl_iface_attach(struct hostapd_data *hapd,
				     struct sockaddr_un *from,
				     socklen_t fromlen)
{
	struct wpa_ctrl_dst *dst;

	dst = os_zalloc(sizeof(*dst));
	if (dst == NULL)
		return -1;
	os_memcpy(&dst->addr, from, sizeof(struct sockaddr_un));
	dst->addrlen = fromlen;
	dst->debug_level = MSG_INFO;
	dst->next = hapd->ctrl_dst;
	hapd->ctrl_dst = dst;
	wpa_hexdump(MSG_DEBUG, "CTRL_IFACE monitor attached",
		    (u8 *) from->sun_path,
		    fromlen - offsetof(struct sockaddr_un, sun_path));
	return 0;
}


static int hostapd_ctrl_iface_detach(struct hostapd_data *hapd,
				     struct sockaddr_un *from,
				     socklen_t fromlen)
{
	struct wpa_ctrl_dst *dst, *prev = NULL;

	dst = hapd->ctrl_dst;
	while (dst) {
		if (fromlen == dst->addrlen &&
		    os_memcmp(from->sun_path, dst->addr.sun_path,
			      fromlen - offsetof(struct sockaddr_un, sun_path))
		    == 0) {
			if (prev == NULL)
				hapd->ctrl_dst = dst->next;
			else
				prev->next = dst->next;
			os_free(dst);
			wpa_hexdump(MSG_DEBUG, "CTRL_IFACE monitor detached",
				    (u8 *) from->sun_path,
				    fromlen -
				    offsetof(struct sockaddr_un, sun_path));
			return 0;
		}
		prev = dst;
		dst = dst->next;
	}
	return -1;
}


static int hostapd_ctrl_iface_level(struct hostapd_data *hapd,
				    struct sockaddr_un *from,
				    socklen_t fromlen,
				    char *level)
{
	struct wpa_ctrl_dst *dst;

	wpa_printf(MSG_DEBUG, "CTRL_IFACE LEVEL %s", level);

	dst = hapd->ctrl_dst;
	while (dst) {
		if (fromlen == dst->addrlen &&
		    os_memcmp(from->sun_path, dst->addr.sun_path,
			      fromlen - offsetof(struct sockaddr_un, sun_path))
		    == 0) {
			wpa_hexdump(MSG_DEBUG, "CTRL_IFACE changed monitor "
				    "level", (u8 *) from->sun_path, fromlen -
				    offsetof(struct sockaddr_un, sun_path));
			dst->debug_level = atoi(level);
			return 0;
		}
		dst = dst->next;
	}

	return -1;
}


static int hostapd_ctrl_iface_new_sta(struct hostapd_data *hapd,
				      const char *txtaddr)
{
	u8 addr[ETH_ALEN];
	struct sta_info *sta;

	wpa_printf(MSG_DEBUG, "CTRL_IFACE NEW_STA %s", txtaddr);

	if (hwaddr_aton(txtaddr, addr))
		return -1;

	sta = ap_get_sta(hapd, addr);
	if (sta)
		return 0;

	wpa_printf(MSG_DEBUG, "Add new STA " MACSTR " based on ctrl_iface "
		   "notification", MAC2STR(addr));
	sta = ap_sta_add(hapd, addr);
	if (sta == NULL)
		return -1;

	hostapd_new_assoc_sta(hapd, sta, 0);
	return 0;
}


static int hostapd_ctrl_iface_deauthenticate(struct hostapd_data *hapd,
					     const char *txtaddr)
{
	u8 addr[ETH_ALEN];
	struct sta_info *sta;
	const char *pos;

	wpa_printf(MSG_DEBUG, "CTRL_IFACE DEAUTHENTICATE %s", txtaddr);

	if (hwaddr_aton(txtaddr, addr))
		return -1;

	pos = os_strstr(txtaddr, " test=");
	if (pos) {
		struct ieee80211_mgmt mgmt;
		int encrypt;
		if (hapd->driver->send_frame == NULL)
			return -1;
		pos += 6;
		encrypt = atoi(pos);
		os_memset(&mgmt, 0, sizeof(mgmt));
		mgmt.frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT,
						  WLAN_FC_STYPE_DEAUTH);
		os_memcpy(mgmt.da, addr, ETH_ALEN);
		os_memcpy(mgmt.sa, hapd->own_addr, ETH_ALEN);
		os_memcpy(mgmt.bssid, hapd->own_addr, ETH_ALEN);
		mgmt.u.deauth.reason_code =
			host_to_le16(WLAN_REASON_PREV_AUTH_NOT_VALID);
		if (hapd->driver->send_frame(hapd->drv_priv, (u8 *) &mgmt,
					     IEEE80211_HDRLEN +
					     sizeof(mgmt.u.deauth),
					     encrypt) < 0)
			return -1;
		return 0;
	}

	hapd->drv.sta_deauth(hapd, addr, WLAN_REASON_PREV_AUTH_NOT_VALID);
	sta = ap_get_sta(hapd, addr);
	if (sta)
		ap_sta_deauthenticate(hapd, sta,
				      WLAN_REASON_PREV_AUTH_NOT_VALID);

	return 0;
}


static int hostapd_ctrl_iface_disassociate(struct hostapd_data *hapd,
					   const char *txtaddr)
{
	u8 addr[ETH_ALEN];
	struct sta_info *sta;
	const char *pos;

	wpa_printf(MSG_DEBUG, "CTRL_IFACE DISASSOCIATE %s", txtaddr);

	if (hwaddr_aton(txtaddr, addr))
		return -1;

	pos = os_strstr(txtaddr, " test=");
	if (pos) {
		struct ieee80211_mgmt mgmt;
		int encrypt;
		if (hapd->driver->send_frame == NULL)
			return -1;
		pos += 6;
		encrypt = atoi(pos);
		os_memset(&mgmt, 0, sizeof(mgmt));
		mgmt.frame_control = IEEE80211_FC(WLAN_FC_TYPE_MGMT,
						  WLAN_FC_STYPE_DISASSOC);
		os_memcpy(mgmt.da, addr, ETH_ALEN);
		os_memcpy(mgmt.sa, hapd->own_addr, ETH_ALEN);
		os_memcpy(mgmt.bssid, hapd->own_addr, ETH_ALEN);
		mgmt.u.disassoc.reason_code =
			host_to_le16(WLAN_REASON_PREV_AUTH_NOT_VALID);
		if (hapd->driver->send_frame(hapd->drv_priv, (u8 *) &mgmt,
					     IEEE80211_HDRLEN +
					     sizeof(mgmt.u.deauth),
					     encrypt) < 0)
			return -1;
		return 0;
	}

	hapd->drv.sta_disassoc(hapd, addr, WLAN_REASON_PREV_AUTH_NOT_VALID);
	sta = ap_get_sta(hapd, addr);
	if (sta)
		ap_sta_disassociate(hapd, sta,
				    WLAN_REASON_PREV_AUTH_NOT_VALID);

	return 0;
}


#ifdef CONFIG_IEEE80211W
#ifdef NEED_AP_MLME
static int hostapd_ctrl_iface_sa_query(struct hostapd_data *hapd,
				       const char *txtaddr)
{
	u8 addr[ETH_ALEN];
	u8 trans_id[WLAN_SA_QUERY_TR_ID_LEN];

	wpa_printf(MSG_DEBUG, "CTRL_IFACE SA_QUERY %s", txtaddr);

	if (hwaddr_aton(txtaddr, addr) ||
	    os_get_random(trans_id, WLAN_SA_QUERY_TR_ID_LEN) < 0)
		return -1;

	ieee802_11_send_sa_query_req(hapd, addr, trans_id);

	return 0;
}
#endif /* NEED_AP_MLME */
#endif /* CONFIG_IEEE80211W */


#ifdef CONFIG_WPS
static int hostapd_ctrl_iface_wps_pin(struct hostapd_data *hapd, char *txt)
{
	char *pin = os_strchr(txt, ' ');
	char *timeout_txt;
	int timeout;

	if (pin == NULL)
		return -1;
	*pin++ = '\0';

	timeout_txt = os_strchr(pin, ' ');
	if (timeout_txt) {
		*timeout_txt++ = '\0';
		timeout = atoi(timeout_txt);
	} else
		timeout = 0;

	return hostapd_wps_add_pin(hapd, txt, pin, timeout);
}


#ifdef CONFIG_WPS_OOB
static int hostapd_ctrl_iface_wps_oob(struct hostapd_data *hapd, char *txt)
{
	char *path, *method, *name;

	path = os_strchr(txt, ' ');
	if (path == NULL)
		return -1;
	*path++ = '\0';

	method = os_strchr(path, ' ');
	if (method == NULL)
		return -1;
	*method++ = '\0';

	name = os_strchr(method, ' ');
	if (name != NULL)
		*name++ = '\0';

	return hostapd_wps_start_oob(hapd, txt, path, method, name);
}
#endif /* CONFIG_WPS_OOB */


static int hostapd_ctrl_iface_wps_ap_pin(struct hostapd_data *hapd, char *txt,
					 char *buf, size_t buflen)
{
	int timeout = 300;
	char *pos;
	const char *pin_txt;

	pos = os_strchr(txt, ' ');
	if (pos)
		*pos++ = '\0';

	if (os_strcmp(txt, "disable") == 0) {
		hostapd_wps_ap_pin_disable(hapd);
		return os_snprintf(buf, buflen, "OK\n");
	}

	if (os_strcmp(txt, "random") == 0) {
		if (pos)
			timeout = atoi(pos);
		pin_txt = hostapd_wps_ap_pin_random(hapd, timeout);
		if (pin_txt == NULL)
			return -1;
		return os_snprintf(buf, buflen, "%s", pin_txt);
	}

	if (os_strcmp(txt, "get") == 0) {
		pin_txt = hostapd_wps_ap_pin_get(hapd);
		if (pin_txt == NULL)
			return -1;
		return os_snprintf(buf, buflen, "%s", pin_txt);
	}

	if (os_strcmp(txt, "set") == 0) {
		char *pin;
		if (pos == NULL)
			return -1;
		pin = pos;
		pos = os_strchr(pos, ' ');
		if (pos) {
			*pos++ = '\0';
			timeout = atoi(pos);
		}
		if (os_strlen(pin) > buflen)
			return -1;
		if (hostapd_wps_ap_pin_set(hapd, pin, timeout) < 0)
			return -1;
		return os_snprintf(buf, buflen, "%s", pin);
	}

	return -1;
}
#endif /* CONFIG_WPS */


static void hostapd_ctrl_iface_receive(int sock, void *eloop_ctx,
				       void *sock_ctx)
{
	struct hostapd_data *hapd = eloop_ctx;
	char buf[256];
	int res;
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(from);
	char *reply;
	const int reply_size = 4096;
	int reply_len;

	res = recvfrom(sock, buf, sizeof(buf) - 1, 0,
		       (struct sockaddr *) &from, &fromlen);
	if (res < 0) {
		perror("recvfrom(ctrl_iface)");
		return;
	}
	buf[res] = '\0';
	wpa_hexdump_ascii(MSG_DEBUG, "RX ctrl_iface", (u8 *) buf, res);

	reply = os_malloc(reply_size);
	if (reply == NULL) {
		sendto(sock, "FAIL\n", 5, 0, (struct sockaddr *) &from,
		       fromlen);
		return;
	}

	os_memcpy(reply, "OK\n", 3);
	reply_len = 3;

	if (os_strcmp(buf, "PING") == 0) {
		os_memcpy(reply, "PONG\n", 5);
		reply_len = 5;
	} else if (os_strcmp(buf, "MIB") == 0) {
		reply_len = ieee802_11_get_mib(hapd, reply, reply_size);
		if (reply_len >= 0) {
			res = wpa_get_mib(hapd->wpa_auth, reply + reply_len,
					  reply_size - reply_len);
			if (res < 0)
				reply_len = -1;
			else
				reply_len += res;
		}
		if (reply_len >= 0) {
			res = ieee802_1x_get_mib(hapd, reply + reply_len,
						 reply_size - reply_len);
			if (res < 0)
				reply_len = -1;
			else
				reply_len += res;
		}
#ifndef CONFIG_NO_RADIUS
		if (reply_len >= 0) {
			res = radius_client_get_mib(hapd->radius,
						    reply + reply_len,
						    reply_size - reply_len);
			if (res < 0)
				reply_len = -1;
			else
				reply_len += res;
		}
#endif /* CONFIG_NO_RADIUS */
	} else if (os_strcmp(buf, "STA-FIRST") == 0) {
		reply_len = hostapd_ctrl_iface_sta_first(hapd, reply,
							 reply_size);
	} else if (os_strncmp(buf, "STA ", 4) == 0) {
		reply_len = hostapd_ctrl_iface_sta(hapd, buf + 4, reply,
						   reply_size);
	} else if (os_strncmp(buf, "STA-NEXT ", 9) == 0) {
		reply_len = hostapd_ctrl_iface_sta_next(hapd, buf + 9, reply,
							reply_size);
	} else if (os_strcmp(buf, "ATTACH") == 0) {
		if (hostapd_ctrl_iface_attach(hapd, &from, fromlen))
			reply_len = -1;
	} else if (os_strcmp(buf, "DETACH") == 0) {
		if (hostapd_ctrl_iface_detach(hapd, &from, fromlen))
			reply_len = -1;
	} else if (os_strncmp(buf, "LEVEL ", 6) == 0) {
		if (hostapd_ctrl_iface_level(hapd, &from, fromlen,
						    buf + 6))
			reply_len = -1;
	} else if (os_strncmp(buf, "NEW_STA ", 8) == 0) {
		if (hostapd_ctrl_iface_new_sta(hapd, buf + 8))
			reply_len = -1;
	} else if (os_strncmp(buf, "DEAUTHENTICATE ", 15) == 0) {
		if (hostapd_ctrl_iface_deauthenticate(hapd, buf + 15))
			reply_len = -1;
	} else if (os_strncmp(buf, "DISASSOCIATE ", 13) == 0) {
		if (hostapd_ctrl_iface_disassociate(hapd, buf + 13))
			reply_len = -1;
#ifdef CONFIG_IEEE80211W
#ifdef NEED_AP_MLME
	} else if (os_strncmp(buf, "SA_QUERY ", 9) == 0) {
		if (hostapd_ctrl_iface_sa_query(hapd, buf + 9))
			reply_len = -1;
#endif /* NEED_AP_MLME */
#endif /* CONFIG_IEEE80211W */
#ifdef CONFIG_WPS
	} else if (os_strncmp(buf, "WPS_PIN ", 8) == 0) {
		if (hostapd_ctrl_iface_wps_pin(hapd, buf + 8))
			reply_len = -1;
	} else if (os_strcmp(buf, "WPS_PBC") == 0) {
		if (hostapd_wps_button_pushed(hapd))
			reply_len = -1;
#ifdef CONFIG_WPS_OOB
	} else if (os_strncmp(buf, "WPS_OOB ", 8) == 0) {
		if (hostapd_ctrl_iface_wps_oob(hapd, buf + 8))
			reply_len = -1;
#endif /* CONFIG_WPS_OOB */
	} else if (os_strncmp(buf, "WPS_AP_PIN ", 11) == 0) {
		reply_len = hostapd_ctrl_iface_wps_ap_pin(hapd, buf + 11,
							  reply, reply_size);
#endif /* CONFIG_WPS */
	} else {
		os_memcpy(reply, "UNKNOWN COMMAND\n", 16);
		reply_len = 16;
	}

	if (reply_len < 0) {
		os_memcpy(reply, "FAIL\n", 5);
		reply_len = 5;
	}
	sendto(sock, reply, reply_len, 0, (struct sockaddr *) &from, fromlen);
	os_free(reply);
}


static char * hostapd_ctrl_iface_path(struct hostapd_data *hapd)
{
	char *buf;
	size_t len;

	if (hapd->conf->ctrl_interface == NULL)
		return NULL;

	len = os_strlen(hapd->conf->ctrl_interface) +
		os_strlen(hapd->conf->iface) + 2;
	buf = os_malloc(len);
	if (buf == NULL)
		return NULL;

	os_snprintf(buf, len, "%s/%s",
		    hapd->conf->ctrl_interface, hapd->conf->iface);
	buf[len - 1] = '\0';
	return buf;
}


static void hostapd_ctrl_iface_msg_cb(void *ctx, int level,
				      const char *txt, size_t len)
{
	struct hostapd_data *hapd = ctx;
	if (hapd == NULL)
		return;
	hostapd_ctrl_iface_send(hapd, level, txt, len);
}


int hostapd_ctrl_iface_init(struct hostapd_data *hapd)
{
	struct sockaddr_un addr;
	int s = -1;
	char *fname = NULL;

	hapd->ctrl_sock = -1;

	if (hapd->conf->ctrl_interface == NULL)
		return 0;

	if (mkdir(hapd->conf->ctrl_interface, S_IRWXU | S_IRWXG) < 0) {
		if (errno == EEXIST) {
			wpa_printf(MSG_DEBUG, "Using existing control "
				   "interface directory.");
		} else {
			perror("mkdir[ctrl_interface]");
			goto fail;
		}
	}

	if (hapd->conf->ctrl_interface_gid_set &&
	    chown(hapd->conf->ctrl_interface, 0,
		  hapd->conf->ctrl_interface_gid) < 0) {
		perror("chown[ctrl_interface]");
		return -1;
	}

	if (os_strlen(hapd->conf->ctrl_interface) + 1 +
	    os_strlen(hapd->conf->iface) >= sizeof(addr.sun_path))
		goto fail;

	s = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (s < 0) {
		perror("socket(PF_UNIX)");
		goto fail;
	}

	os_memset(&addr, 0, sizeof(addr));
#ifdef __FreeBSD__
	addr.sun_len = sizeof(addr);
#endif /* __FreeBSD__ */
	addr.sun_family = AF_UNIX;
	fname = hostapd_ctrl_iface_path(hapd);
	if (fname == NULL)
		goto fail;
	os_strlcpy(addr.sun_path, fname, sizeof(addr.sun_path));
	if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		wpa_printf(MSG_DEBUG, "ctrl_iface bind(PF_UNIX) failed: %s",
			   strerror(errno));
		if (connect(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
			wpa_printf(MSG_DEBUG, "ctrl_iface exists, but does not"
				   " allow connections - assuming it was left"
				   "over from forced program termination");
			if (unlink(fname) < 0) {
				perror("unlink[ctrl_iface]");
				wpa_printf(MSG_ERROR, "Could not unlink "
					   "existing ctrl_iface socket '%s'",
					   fname);
				goto fail;
			}
			if (bind(s, (struct sockaddr *) &addr, sizeof(addr)) <
			    0) {
				perror("bind(PF_UNIX)");
				goto fail;
			}
			wpa_printf(MSG_DEBUG, "Successfully replaced leftover "
				   "ctrl_iface socket '%s'", fname);
		} else {
			wpa_printf(MSG_INFO, "ctrl_iface exists and seems to "
				   "be in use - cannot override it");
			wpa_printf(MSG_INFO, "Delete '%s' manually if it is "
				   "not used anymore", fname);
			os_free(fname);
			fname = NULL;
			goto fail;
		}
	}

	if (hapd->conf->ctrl_interface_gid_set &&
	    chown(fname, 0, hapd->conf->ctrl_interface_gid) < 0) {
		perror("chown[ctrl_interface/ifname]");
		goto fail;
	}

	if (chmod(fname, S_IRWXU | S_IRWXG) < 0) {
		perror("chmod[ctrl_interface/ifname]");
		goto fail;
	}
	os_free(fname);

	hapd->ctrl_sock = s;
	eloop_register_read_sock(s, hostapd_ctrl_iface_receive, hapd,
				 NULL);
	hapd->msg_ctx = hapd;
	wpa_msg_register_cb(hostapd_ctrl_iface_msg_cb);

	return 0;

fail:
	if (s >= 0)
		close(s);
	if (fname) {
		unlink(fname);
		os_free(fname);
	}
	return -1;
}


void hostapd_ctrl_iface_deinit(struct hostapd_data *hapd)
{
	struct wpa_ctrl_dst *dst, *prev;

	if (hapd->ctrl_sock > -1) {
		char *fname;
		eloop_unregister_read_sock(hapd->ctrl_sock);
		close(hapd->ctrl_sock);
		hapd->ctrl_sock = -1;
		fname = hostapd_ctrl_iface_path(hapd);
		if (fname)
			unlink(fname);
		os_free(fname);

		if (hapd->conf->ctrl_interface &&
		    rmdir(hapd->conf->ctrl_interface) < 0) {
			if (errno == ENOTEMPTY) {
				wpa_printf(MSG_DEBUG, "Control interface "
					   "directory not empty - leaving it "
					   "behind");
			} else {
				perror("rmdir[ctrl_interface]");
			}
		}
	}

	dst = hapd->ctrl_dst;
	while (dst) {
		prev = dst;
		dst = dst->next;
		os_free(prev);
	}
}


static void hostapd_ctrl_iface_send(struct hostapd_data *hapd, int level,
				    const char *buf, size_t len)
{
	struct wpa_ctrl_dst *dst, *next;
	struct msghdr msg;
	int idx;
	struct iovec io[2];
	char levelstr[10];

	dst = hapd->ctrl_dst;
	if (hapd->ctrl_sock < 0 || dst == NULL)
		return;

	os_snprintf(levelstr, sizeof(levelstr), "<%d>", level);
	io[0].iov_base = levelstr;
	io[0].iov_len = os_strlen(levelstr);
	io[1].iov_base = (char *) buf;
	io[1].iov_len = len;
	os_memset(&msg, 0, sizeof(msg));
	msg.msg_iov = io;
	msg.msg_iovlen = 2;

	idx = 0;
	while (dst) {
		next = dst->next;
		if (level >= dst->debug_level) {
			wpa_hexdump(MSG_DEBUG, "CTRL_IFACE monitor send",
				    (u8 *) dst->addr.sun_path, dst->addrlen -
				    offsetof(struct sockaddr_un, sun_path));
			msg.msg_name = &dst->addr;
			msg.msg_namelen = dst->addrlen;
			if (sendmsg(hapd->ctrl_sock, &msg, 0) < 0) {
				int _errno = errno;
				wpa_printf(MSG_INFO, "CTRL_IFACE monitor[%d]: "
					   "%d - %s",
					   idx, errno, strerror(errno));
				dst->errors++;
				if (dst->errors > 10 || _errno == ENOENT) {
					hostapd_ctrl_iface_detach(
						hapd, &dst->addr,
						dst->addrlen);
				}
			} else
				dst->errors = 0;
		}
		idx++;
		dst = next;
	}
}

#endif /* CONFIG_NATIVE_WINDOWS */
