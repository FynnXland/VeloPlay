// SPDX-License-Identifier: AGPL-3.0-only
//
// The only header of this directory the rest of VeloPlay includes. It hides
// libchiaki behind plain C types: the library is C99 with variable-length
// arrays and is compiled by clang, while the client is C++ compiled by MSVC.
// Keeping chiaki's own headers out of the C++ code means only this small
// surface has to agree between the two compilers.

#ifndef VELOPLAY_VP_SESSION_H
#define VELOPLAY_VP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VpSession VpSession;

enum { VP_CODEC_H264 = 0, VP_CODEC_HEVC = 1, VP_CODEC_HEVC_HDR = 2 };

enum { VP_LOG_ERROR = 0, VP_LOG_WARNING = 1, VP_LOG_INFO = 2 };

/* Why a session ended. */
enum {
	VP_QUIT_STOPPED = 0,            /* we asked for it */
	VP_QUIT_CONSOLE_SHUT_DOWN = 1,  /* console went to rest mode or powered off */
	VP_QUIT_CONNECTION_REFUSED = 2, /* nothing listens on the console, usually rest mode */
	VP_QUIT_IN_USE = 3,             /* another Remote Play client is connected */
	VP_QUIT_CONSOLE_CRASHED = 4,    /* Remote Play on the console crashed */
	VP_QUIT_VERSION_MISMATCH = 5,   /* console speaks a protocol version we do not */
	VP_QUIT_SETUP_FAILED = 6,       /* handshake failed for another reason */
	VP_QUIT_CONNECTION_LOST = 7,    /* stream broke or the console dropped us */
	/* Through Sony's servers only: */
	VP_QUIT_PSN_NO_ANSWER = 8,      /* the console did not answer Sony's servers: off, asleep, no internet */
	VP_QUIT_PSN_UNREACHABLE = 9,    /* both ends were told, but no way through the two networks was found */
	VP_QUIT_PSN_FAILED = 10,        /* Sony's servers refused or another step failed (sign-in expired, say) */
	VP_QUIT_PSN_REGIST_FAILED = 11  /* the console answered but would not let this client in */
};

#define VP_PSN_DUID_SIZE 32
#define VP_PSN_ACCOUNT_ID_SIZE 8

typedef struct VpConnectInfo
{
	const char *host;        /* IPv4 address, zero-terminated; unused through Sony's servers */
	/* Set: connect through Sony's servers instead of the local network. An
	 * access token of a sign-in with the four scopes in PsnAccount.cpp; the
	 * console's id from vp_psn_list_consoles(); the account's id. The keys
	 * below are not used then: the console pairs again on this way. */
	const char *psn_token;
	uint8_t psn_duid[VP_PSN_DUID_SIZE];
	uint8_t psn_account_id[VP_PSN_ACCOUNT_ID_SIZE];
	char regist_key[16];     /* ASCII, padded with zeros */
	uint8_t rp_key[16];
	uint32_t width;
	uint32_t height;
	uint32_t fps;            /* 30 or 60 */
	uint32_t bitrate_kbps;
	int codec;               /* VP_CODEC_* */
	int dualsense;           /* ask for adaptive trigger and haptics data */
	double packet_loss_max;  /* upper bound of the loss reported to the console, 0..1 */
} VpConnectInfo;

typedef struct VpControllerState
{
	uint32_t buttons;        /* bits 0..15 as in input::Button */
	uint8_t l2;
	uint8_t r2;
	int16_t left_x;
	int16_t left_y;
	int16_t right_x;
	int16_t right_y;
	struct
	{
		int active;
		uint8_t id;
		uint16_t x;
		uint16_t y;
	} touches[2];
	int motion_valid;
	float gyro[3];           /* rad/s */
	float accel[3];          /* m/s^2, gravity included */
	uint32_t motion_time_us; /* wraps; only differences matter */
} VpControllerState;

/* All callbacks run on threads owned by the library. They must return quickly
 * and must not call vp_session_destroy(). */
typedef struct VpCallbacks
{
	void *user;
	void (*log)(void *user, int level, const char *message);
	void (*connected)(void *user);
	void (*login_pin_requested)(void *user, int previous_was_wrong);
	/* Last callback of a session. `detail` may be NULL. */
	void (*quit)(void *user, int reason, const char *detail);
	void (*rumble)(void *user, uint8_t low, uint8_t high);
	void (*trigger_effects)(void *user, uint8_t type_left, const uint8_t *left, uint8_t type_right, const uint8_t *right);
	/* How strong the console wants the controller's effects, as set on the
	 * console under Accessories > Controllers. `triggers` is nonzero for the
	 * adaptive triggers and zero for vibration and haptics. `level`: 0 off,
	 * 1 strong, 2 medium, 3 weak. */
	void (*effect_strength)(void *user, int triggers, int level);
	/* One access unit of the video bitstream in Annex B format. The first
	 * call carries the parameter sets.
	 * Returns nonzero if the unit was decoded; zero requests a new key frame. */
	int (*video)(void *user, const uint8_t *data, size_t size, int32_t frames_lost, int recovered);
	void (*audio_format)(void *user, uint32_t channels, uint32_t bits, uint32_t rate, uint32_t frame_size);
	/* One Opus packet. */
	void (*audio)(void *user, const uint8_t *data, size_t size);
	/* DualSense haptics: `frames` left/right pairs of 16-bit PCM at 3000 Hz.
	 * Only sent when the session was created with `dualsense`. */
	void (*haptics)(void *user, const int16_t *stereo, size_t frames);
} VpCallbacks;

typedef struct VpLinkStats
{
	double rtt_ms;      /* measured once while the session was set up; 0 before */
	double packet_loss; /* 0..1, share of lost packets in the last interval */
} VpLinkStats;

#define VP_TRIGGER_EFFECT_SIZE 10

/* Call once per process before anything else. Returns 0 on success. */
int vp_library_init(void);

/* Nonzero if this build can connect through Sony's servers. */
int vp_psn_available(void);

typedef struct VpPsnConsole
{
	char name[33];
	uint8_t duid[VP_PSN_DUID_SIZE];
	int remote_play_enabled;
} VpPsnConsole;

/* Asks Sony's servers for the PS5 consoles of the signed-in account. Blocks for
 * up to a few seconds; never call it on the window thread. Returns the number
 * written to `out` (at most `max`), or -1 on failure. `log` may be NULL. */
int vp_psn_list_consoles(const char *token, VpPsnConsole *out, int max,
	void (*log)(void *user, int level, const char *message), void *user);


/* Returns NULL if the session could not be set up; the reason is logged. */
VpSession *vp_session_create(const VpConnectInfo *info, const VpCallbacks *callbacks);

/* Starts the session thread. Returns 0 on success. Through Sony's servers the
 * way is found first on a thread of its own, which takes up to a minute or so;
 * a failure there ends with the quit callback like any other. */
int vp_session_start(VpSession *session);

/* Asks a running session to end. The quit callback follows. */
void vp_session_stop(VpSession *session);

/* Stops if needed, waits for the session thread and frees everything. */
void vp_session_destroy(VpSession *session);

/* vp_session_destroy() in two halves, for a caller that must not wait: the
 * first stops, waits for the session's threads and drops the callbacks (no
 * callback runs after it returns). The second frees the rest; through Sony's
 * servers it says goodbye to them first, which can take ten seconds or more,
 * so it may run on a thread of its own. Returns nonzero from the first if the
 * second may take that long. */
int vp_session_release(VpSession *session);
void vp_session_finish(VpSession *session);

void vp_session_set_controller(VpSession *session, const VpControllerState *state);

/* `digits` is the console's login passcode as ASCII digits. Returns 0 on success. */
int vp_session_set_login_pin(VpSession *session, const char *digits);

/* Puts the console into rest mode. Returns 0 if the request was sent. */
int vp_session_sleep_console(VpSession *session);

void vp_session_link_stats(VpSession *session, VpLinkStats *out);

/* The microphone. Its sound goes to the console as Opus packets of exactly
 * VP_MIC_PACKET_SIZE bytes, each made from VP_MIC_FRAME_SAMPLES samples per
 * channel of 48 kHz stereo; the console drops any other size.
 *
 * All three may be called from any thread at any time. Outside a running
 * stream they do nothing: the library closes the stream's transport by
 * itself when the console ends the stream, and sending is shut off under a
 * lock just before that. */
#define VP_MIC_PACKET_SIZE 40
#define VP_MIC_FRAME_SAMPLES 480
#define VP_MIC_SAMPLE_RATE 48000
#define VP_MIC_CHANNELS 2

/* Tells the console that a microphone is there. Returns 0 on success, nonzero
 * while no stream is running. */
int vp_session_mic_open(VpSession *session);
/* The console starts with the microphone muted. Returns 0 if the request was sent. */
int vp_session_mic_mute(VpSession *session, int muted);
void vp_session_mic_send(VpSession *session, const uint8_t *opus, size_t size);

#ifdef __cplusplus
}
#endif

#endif
