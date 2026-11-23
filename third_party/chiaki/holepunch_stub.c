// SPDX-License-Identifier: AGPL-3.0-only
//
// Builds without the way through Sony's servers (VELOPLAY_PSN off, as in the
// Linux CI): chiaki-ng's remote/holepunch.c needs curl with WebSocket support,
// json-c, libevent and miniupnpc. The session code and vp_session.c still
// reference its functions; here each one reports failure, and
// vp_psn_available() says the way is missing, so nothing calls them.

// holepunch.h needs the socket headers that session.h brings in before it.
#include <chiaki/session.h>

#include <string.h>

CHIAKI_EXPORT void chiaki_holepunch_session_fini(ChiakiHolepunchSession session)
{
	(void)session;
}

CHIAKI_EXPORT ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession session)
{
	(void)session;
	ChiakiHolepunchRegistInfo info;
	memset(&info, 0, sizeof(info));
	return info;
}

CHIAKI_EXPORT void chiaki_get_ps_selected_addr(ChiakiHolepunchSession session, char *ps_ip)
{
	(void)session;
	if(ps_ip)
		ps_ip[0] = '\0';
}

CHIAKI_EXPORT uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession session)
{
	(void)session;
	return 0;
}

CHIAKI_EXPORT chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession session, ChiakiHolepunchPortType type)
{
	(void)session;
	(void)type;
	return NULL;
}

CHIAKI_EXPORT ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession session)
{
	(void)session;
	return CHIAKI_ERR_UNINITIALIZED;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession session,
	ChiakiHolepunchPortType port_type)
{
	(void)session;
	(void)port_type;
	return CHIAKI_ERR_UNINITIALIZED;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_list_devices(const char *psn_oauth2_token,
	ChiakiHolepunchConsoleType console_type, ChiakiHolepunchDeviceInfo **devices, size_t *device_count, ChiakiLog *log)
{
	(void)psn_oauth2_token;
	(void)console_type;
	(void)log;
	if(devices)
		*devices = NULL;
	if(device_count)
		*device_count = 0;
	return CHIAKI_ERR_UNINITIALIZED;
}

CHIAKI_EXPORT void chiaki_holepunch_free_device_list(ChiakiHolepunchDeviceInfo **devices)
{
	(void)devices;
}

CHIAKI_EXPORT ChiakiHolepunchSession chiaki_holepunch_session_init(const char *psn_oauth2_token, ChiakiLog *log)
{
	(void)psn_oauth2_token;
	(void)log;
	return NULL;
}

CHIAKI_EXPORT void chiaki_holepunch_session_force_port_guessing(ChiakiHolepunchSession session, bool enabled)
{
	(void)session;
	(void)enabled;
}

CHIAKI_EXPORT void chiaki_holepunch_session_set_port_guessing_ports(ChiakiHolepunchSession session, int ports)
{
	(void)session;
	(void)ports;
}

CHIAKI_EXPORT void chiaki_holepunch_session_set_port_guessing_socks(ChiakiHolepunchSession session, int socks)
{
	(void)session;
	(void)socks;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_create(ChiakiHolepunchSession session)
{
	(void)session;
	return CHIAKI_ERR_UNINITIALIZED;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_holepunch_session_start(ChiakiHolepunchSession session, const uint8_t *console_uid,
	ChiakiHolepunchConsoleType console_type)
{
	(void)session;
	(void)console_uid;
	(void)console_type;
	return CHIAKI_ERR_UNINITIALIZED;
}

CHIAKI_EXPORT void chiaki_holepunch_main_thread_cancel(ChiakiHolepunchSession session, bool stop_thread)
{
	(void)session;
	(void)stop_thread;
}
