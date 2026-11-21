// SPDX-License-Identifier: AGPL-3.0-only

#include "vp_session.h"

#include <chiaki/audiosender.h>
#include <chiaki/common.h>
#include <chiaki/controller.h>
#include <chiaki/log.h>
#include <chiaki/orientation.h>
#include <chiaki/session.h>
#include <chiaki/thread.h>
#include <chiaki/videoreceiver.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define VP_STANDARD_GRAVITY 9.80665f

struct VpSession
{
	ChiakiSession session; // first, so the struct stays aligned as chiaki expects
	ChiakiLog log;
	VpCallbacks callbacks;
	ChiakiOrientationTracker orientation;
	ChiakiAccelNewZero accel_zero;
	bool started;
	// The microphone sends into the stream's transport from a thread of the
	// caller. mic_mutex guards the three fields below; stream_open is true
	// from the stream's start until just before the library closes it.
	ChiakiMutex mic_mutex;
	bool mic_mutex_ready;
	bool stream_open;
	ChiakiAudioSender *mic_sender;
};

// Called by the library's stream connection right before it closes its
// transport (see the patch in CMakeLists.txt). Whoever sends microphone
// packets holds mic_mutex while doing so, so after this returns nobody is
// inside the transport and nobody will enter it again.
void vp_stream_closing(ChiakiSession *session)
{
	VpSession *vp = (VpSession *)session; // the session is the first member
	if(!vp->mic_mutex_ready)
		return;
	chiaki_mutex_lock(&vp->mic_mutex);
	vp->stream_open = false;
	chiaki_mutex_unlock(&vp->mic_mutex);
}

static void log_cb(ChiakiLogLevel level, const char *msg, void *user)
{
	VpSession *vp = user;
	if(!vp->callbacks.log)
		return;
	int out = VP_LOG_INFO;
	if(level == CHIAKI_LOG_ERROR)
		out = VP_LOG_ERROR;
	else if(level == CHIAKI_LOG_WARNING)
		out = VP_LOG_WARNING;
	vp->callbacks.log(vp->callbacks.user, out, msg);
}

static int map_quit_reason(ChiakiQuitReason reason)
{
	switch(reason)
	{
		case CHIAKI_QUIT_REASON_STOPPED:
			return VP_QUIT_STOPPED;
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN:
			return VP_QUIT_CONSOLE_SHUT_DOWN;
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
		case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
			return VP_QUIT_CONNECTION_REFUSED;
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
			return VP_QUIT_IN_USE;
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
			return VP_QUIT_CONSOLE_CRASHED;
		case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
			return VP_QUIT_VERSION_MISMATCH;
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN:
		case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED:
			return VP_QUIT_CONNECTION_LOST;
		default:
			return VP_QUIT_SETUP_FAILED;
	}
}

static void event_cb(ChiakiEvent *event, void *user)
{
	VpSession *vp = user;
	const VpCallbacks *cb = &vp->callbacks;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			// Sent from inside the running stream connection: its transport is open from here on.
			if(vp->mic_mutex_ready)
			{
				chiaki_mutex_lock(&vp->mic_mutex);
				vp->stream_open = true;
				chiaki_mutex_unlock(&vp->mic_mutex);
			}
			if(cb->connected)
				cb->connected(cb->user);
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			if(cb->login_pin_requested)
				cb->login_pin_requested(cb->user, event->login_pin_request.pin_incorrect ? 1 : 0);
			break;
		case CHIAKI_EVENT_RUMBLE:
			if(cb->rumble)
				cb->rumble(cb->user, event->rumble.left, event->rumble.right);
			break;
		case CHIAKI_EVENT_TRIGGER_EFFECTS:
			if(cb->trigger_effects)
				cb->trigger_effects(cb->user, event->trigger_effects.type_left, event->trigger_effects.left,
					event->trigger_effects.type_right, event->trigger_effects.right);
			break;
		case CHIAKI_EVENT_HAPTIC_INTENSITY:
		case CHIAKI_EVENT_TRIGGER_INTENSITY:
			// The library's values: Off 0, Strong 1, Medium 2, Weak 3.
			if(cb->effect_strength)
				cb->effect_strength(cb->user, event->type == CHIAKI_EVENT_TRIGGER_INTENSITY ? 1 : 0, (int)event->intensity);
			break;
		case CHIAKI_EVENT_QUIT:
			if(cb->quit)
			{
				// The console's own wording is more specific than chiaki's
				// generic text, so prefer it when there is one.
				const char *detail = event->quit.reason_str;
				if(!detail || !*detail)
					detail = chiaki_quit_reason_string(event->quit.reason);
				cb->quit(cb->user, map_quit_reason(event->quit.reason), detail);
			}
			break;
		default:
			break;
	}
}

static bool video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	VpSession *vp = user;
	if(!vp->callbacks.video)
		return true;
	if(vp->callbacks.video(vp->callbacks.user, buf, buf_size, frames_lost, frame_recovered ? 1 : 0) != 0)
		return true;

	// The receiver of this frame could not use it, or lost frames before it
	// that the library believes were delivered. The library would now refer
	// later frames to pictures the decoder never saw. Only a key frame gets
	// both sides back in step: ask for one and let the library skip
	// everything until it arrives. It repeats the request once a second.
	ChiakiVideoReceiver *receiver = vp->session.stream_connection.video_receiver;
	if(receiver && !chiaki_video_receiver_get_waiting_for_idr(receiver))
	{
		if(chiaki_session_request_idr(&vp->session) == CHIAKI_ERR_SUCCESS)
			chiaki_video_receiver_set_waiting_for_idr(receiver, true);
	}
	return false;
}

static void audio_header_cb(ChiakiAudioHeader *header, void *user)
{
	VpSession *vp = user;
	if(vp->callbacks.audio_format)
		vp->callbacks.audio_format(vp->callbacks.user, header->channels, header->bits, header->rate, header->frame_size);
}

static void audio_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
	VpSession *vp = user;
	if(vp->callbacks.audio)
		vp->callbacks.audio(vp->callbacks.user, buf, buf_size);
}

static void haptics_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
	VpSession *vp = user;
	// The packet is native-endian int16 pairs; buf comes from malloc and is
	// aligned for that.
	if(vp->callbacks.haptics && buf_size >= 2 * sizeof(int16_t))
		vp->callbacks.haptics(vp->callbacks.user, (const int16_t *)buf, buf_size / (2 * sizeof(int16_t)));
}

int vp_library_init(void)
{
	return chiaki_lib_init() == CHIAKI_ERR_SUCCESS ? 0 : -1;
}

VpSession *vp_session_create(const VpConnectInfo *info, const VpCallbacks *callbacks)
{
	if(!info || !info->host || !callbacks)
		return NULL;

	VpSession *vp = calloc(1, sizeof(VpSession));
	if(!vp)
		return NULL;
	vp->callbacks = *callbacks;

	// Verbose and debug output contains hex dumps of the handshake, including
	// material derived from the pairing keys. It stays off.
	chiaki_log_init(&vp->log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, log_cb, vp);

	chiaki_orientation_tracker_init(&vp->orientation);
	chiaki_accel_new_zero_set_inactive(&vp->accel_zero, false);

	ChiakiConnectInfo connect;
	memset(&connect, 0, sizeof(connect));
	connect.ps5 = true;
	connect.host = info->host;
	memcpy(connect.regist_key, info->regist_key, sizeof(connect.regist_key));
	memcpy(connect.morning, info->rp_key, sizeof(connect.morning));
	connect.video_profile.width = info->width;
	connect.video_profile.height = info->height;
	connect.video_profile.max_fps = info->fps;
	connect.video_profile.bitrate = info->bitrate_kbps;
	connect.video_profile.codec = info->codec == VP_CODEC_H264 ? CHIAKI_CODEC_H264
		: info->codec == VP_CODEC_HEVC_HDR ? CHIAKI_CODEC_H265_HDR
		: CHIAKI_CODEC_H265;
	connect.video_profile_auto_downgrade = true;
	connect.enable_keyboard = false;
	connect.enable_dualsense = info->dualsense != 0;
	connect.audio_video_disabled = CHIAKI_NONE_DISABLED;
	connect.auto_regist = false;
	connect.holepunch_session = NULL;
	connect.rudp_sock = NULL;
	connect.packet_loss_max = info->packet_loss_max;
	// As in chiaki-ng by default: a frame that cannot be repaired is reported
	// to the console, which then stops referring to it. Asking for a whole
	// key frame instead sends a burst that a weak link loses again.
	connect.enable_idr_on_fec_failure = false;

	// On failure chiaki_session_init() has already released what it set up.
	ChiakiErrorCode err = chiaki_session_init(&vp->session, &connect, &vp->log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(&vp->log, "Session init failed: %s", chiaki_error_string(err));
		free(vp);
		return NULL;
	}

	vp->mic_mutex_ready = chiaki_mutex_init(&vp->mic_mutex, false) == CHIAKI_ERR_SUCCESS;

	chiaki_session_set_event_cb(&vp->session, event_cb, vp);
	chiaki_session_set_video_sample_cb(&vp->session, video_cb, vp);

	ChiakiAudioSink sink;
	memset(&sink, 0, sizeof(sink));
	sink.user = vp;
	sink.header_cb = audio_header_cb;
	sink.frame_cb = audio_frame_cb;
	chiaki_session_set_audio_sink(&vp->session, &sink);

	if(info->dualsense)
	{
		ChiakiAudioSink haptics;
		memset(&haptics, 0, sizeof(haptics));
		haptics.user = vp;
		haptics.frame_cb = haptics_frame_cb;
		chiaki_session_set_haptics_sink(&vp->session, &haptics);
	}

	return vp;
}

int vp_session_start(VpSession *vp)
{
	if(!vp || vp->started)
		return -1;
	if(chiaki_session_start(&vp->session) != CHIAKI_ERR_SUCCESS)
		return -1;
	vp->started = true;
	return 0;
}

void vp_session_stop(VpSession *vp)
{
	if(vp && vp->started)
		chiaki_session_stop(&vp->session);
}

void vp_session_destroy(VpSession *vp)
{
	if(!vp)
		return;
	if(vp->started)
	{
		chiaki_session_stop(&vp->session);
		chiaki_session_join(&vp->session);
	}
	// The session thread is gone, and with it the stream; nothing sends any more.
	if(vp->mic_mutex_ready)
	{
		chiaki_mutex_lock(&vp->mic_mutex);
		vp->stream_open = false;
		ChiakiAudioSender *sender = vp->mic_sender;
		vp->mic_sender = NULL;
		chiaki_mutex_unlock(&vp->mic_mutex);
		chiaki_audio_sender_free(sender);
		chiaki_mutex_fini(&vp->mic_mutex);
	}
	chiaki_session_fini(&vp->session);
	free(vp);
}

int vp_session_mic_open(VpSession *vp)
{
	if(!vp || !vp->started || !vp->mic_mutex_ready)
		return -1;
	int result = -1;
	chiaki_mutex_lock(&vp->mic_mutex);
	if(vp->stream_open)
	{
		if(!vp->mic_sender)
			vp->mic_sender = chiaki_audio_sender_new(&vp->log, &vp->session);
		if(vp->mic_sender && chiaki_session_connect_microphone(&vp->session) == CHIAKI_ERR_SUCCESS)
			result = 0;
	}
	chiaki_mutex_unlock(&vp->mic_mutex);
	return result;
}

int vp_session_mic_mute(VpSession *vp, int muted)
{
	if(!vp || !vp->started || !vp->mic_mutex_ready)
		return -1;
	int result = -1;
	chiaki_mutex_lock(&vp->mic_mutex);
	// The library's argument reads the other way round: true asks the console to lift the mute.
	if(vp->stream_open && chiaki_session_toggle_microphone(&vp->session, muted ? false : true) == CHIAKI_ERR_SUCCESS)
		result = 0;
	chiaki_mutex_unlock(&vp->mic_mutex);
	return result;
}

void vp_session_mic_send(VpSession *vp, const uint8_t *opus, size_t size)
{
	if(!vp || !opus || size != VP_MIC_PACKET_SIZE || !vp->mic_mutex_ready)
		return;
	chiaki_mutex_lock(&vp->mic_mutex);
	if(vp->stream_open && vp->mic_sender)
	{
		// The sender copies the packet; it only lacks the const.
		uint8_t packet[VP_MIC_PACKET_SIZE];
		memcpy(packet, opus, sizeof(packet));
		chiaki_audio_sender_opus_data(vp->mic_sender, packet, sizeof(packet));
	}
	chiaki_mutex_unlock(&vp->mic_mutex);
}

void vp_session_set_controller(VpSession *vp, const VpControllerState *in)
{
	if(!vp || !in || !vp->started)
		return;

	ChiakiControllerState state;
	chiaki_controller_state_set_idle(&state);
	state.buttons = in->buttons & 0xffffu; // higher bits mean something else to chiaki
	state.l2_state = in->l2;
	state.r2_state = in->r2;
	state.left_x = in->left_x;
	state.left_y = in->left_y;
	state.right_x = in->right_x;
	state.right_y = in->right_y;
	for(int i = 0; i < CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		state.touches[i].id = in->touches[i].active ? (int8_t)(in->touches[i].id & 0x7f) : -1;
		state.touches[i].x = in->touches[i].x;
		state.touches[i].y = in->touches[i].y;
	}

	if(in->motion_valid)
	{
		chiaki_orientation_tracker_update(&vp->orientation,
			in->gyro[0], in->gyro[1], in->gyro[2],
			in->accel[0] / VP_STANDARD_GRAVITY, in->accel[1] / VP_STANDARD_GRAVITY, in->accel[2] / VP_STANDARD_GRAVITY,
			&vp->accel_zero, false, in->motion_time_us);
		chiaki_orientation_tracker_apply_to_controller_state(&vp->orientation, &state);
	}

	chiaki_session_set_controller_state(&vp->session, &state);
}

int vp_session_set_login_pin(VpSession *vp, const char *digits)
{
	if(!vp || !digits || !vp->started)
		return -1;
	return chiaki_session_set_login_pin(&vp->session, (const uint8_t *)digits, strlen(digits)) == CHIAKI_ERR_SUCCESS ? 0 : -1;
}

int vp_session_sleep_console(VpSession *vp)
{
	if(!vp || !vp->started)
		return -1;
	return chiaki_session_goto_bed(&vp->session) == CHIAKI_ERR_SUCCESS ? 0 : -1;
}

void vp_session_link_stats(VpSession *vp, VpLinkStats *out)
{
	if(!out)
		return;
	out->rtt_ms = 0.0;
	out->packet_loss = 0.0;
	if(!vp)
		return;
	// Both values are plain fields written by the library's threads without a
	// lock. chiaki-ng's own client reads them the same way; a torn read can at
	// worst show one odd number in the statistics.
	out->rtt_ms = (double)vp->session.rtt_us / 1000.0;
	out->packet_loss = vp->session.stream_connection.congestion_control.packet_loss;
}
