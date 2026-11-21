// SPDX-License-Identifier: AGPL-3.0-only
//
// VeloPlay connects to consoles in the local network only. chiaki-ng's
// connection over the internet (remote/holepunch.c) needs curl, json-c,
// libevent and miniupnpc, none of which are built here. The session code still
// references a handful of its functions. They are reachable only when a
// holepunch session is passed in ChiakiConnectInfo, which this client never
// does, so each one reports failure.

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
