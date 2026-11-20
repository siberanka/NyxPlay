/*
 * Stream harness: a minimal client built on the shared moonlight-common-c.
 *
 * It does not pair or launch. A launcher (PowerShell in the test guest) performs /launch with
 * the remote input key it generated and passes the session URL and key to this program, which
 * then runs a stream through the library with fake renderers and reports what arrived.
 *
 *   stream_harness --addr HOST --url RTSP_URL --rikey HEX32 --rikeyid N
 *                  [--appver 7.1.431.-1] [--gfever 3.23.0.74] [--codec 1] [--enc N]
 *                  [--secs 10] [--ext] [--text TEXT] [--size WxH] [--fps N]
 *
 * --enc is the library encryptionFlags value (0 none, 1 audio, 2 video, 3 both, -1 all).
 * --ext sends every client side message of the protocol extensions once.
 *
 * Exit status: 0 when the stream started, delivered video and ended cleanly; 1 otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x) * 1000)
#endif

#include "Limelight.h"

static volatile long g_frames, g_idr, g_audio, g_stage_failed, g_started, g_terminated, g_text_state;
static volatile int g_term_code = -1;
static uint64_t g_bytes;
static uint32_t g_hash = 2166136261u;
static int g_first_format;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("lib: ");
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static int dr_setup(int fmt, int w, int h, int fps, void *ctx, int flags)
{
    (void)ctx; (void)flags;
    g_first_format = fmt;
    printf("video setup format=0x%x %dx%d fps=%d\n", fmt, w, h, fps);
    return 0;
}
static void dr_noop(void) {}

static int dr_submit(PDECODE_UNIT du)
{
    PLENTRY e;
    g_frames++;
    if (du->frameType == FRAME_TYPE_IDR) g_idr++;
    for (e = du->bufferList; e != NULL; e = e->next) {
        int i;
        g_bytes += (uint64_t)e->length;
        /* sample the data: first 16 bytes of each buffer are enough to catch NAL boundary damage */
        for (i = 0; i < e->length && i < 16; i++) {
            g_hash ^= (uint8_t)e->data[i];
            g_hash *= 16777619u;
        }
        g_hash ^= (uint32_t)e->length;
        g_hash *= 16777619u;
    }
    return DR_OK;
}

static int ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION oc, void *ctx, int fl)
{
    (void)cfg; (void)ctx; (void)fl;
    printf("audio init rate=%d channels=%d\n", oc->sampleRate, oc->channelCount);
    return 0;
}
static void ar_noop(void) {}
static void ar_play(char *data, int len) { (void)data; (void)len; g_audio++; }

static void cl_stage_start(int s) { printf("stage %d starting\n", s); fflush(stdout); }
static void cl_stage_done(int s) { printf("stage %d complete\n", s); fflush(stdout); }
static void cl_stage_fail(int s, int err) { g_stage_failed = s; printf("stage %d FAILED error=%d\n", s, err); fflush(stdout); }
static void cl_started(void) { g_started = 1; printf("connection started\n"); fflush(stdout); }
static void cl_terminated(int err) { g_terminated = 1; g_term_code = err; printf("connection terminated error=%d\n", err); fflush(stdout); }
static void cl_text_state(bool active, uint8_t flags) { g_text_state++; printf("text input state active=%d flags=0x%02x\n", active, flags); fflush(stdout); }
static void cl_rumble(uint16_t n, uint16_t lo, uint16_t hi) { printf("rumble %u %u %u\n", n, lo, hi); }
static void cl_rumble_trig(uint16_t n, uint16_t l, uint16_t r) { printf("rumble triggers %u %u %u\n", n, l, r); }
static void cl_motion(uint16_t n, uint8_t t, uint16_t rate) { printf("motion state %u %u %u\n", n, t, rate); }
static void cl_led(uint16_t n, uint8_t r, uint8_t g, uint8_t b) { printf("led %u %u %u %u\n", n, r, g, b); }
static void cl_adaptive(uint16_t n, uint8_t fl, uint8_t tl, uint8_t tr, uint8_t *l, uint8_t *r)
{
    (void)l; (void)r;
    printf("adaptive triggers %u flags=%u types=%u/%u\n", n, fl, tl, tr);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    const char *addr = NULL, *url = NULL, *rikey = NULL, *text = NULL;
    const char *appver = "7.1.431.-1", *gfever = "3.23.0.74";
    int rikeyid = 0, codec = 1, enc = 0, secs = 10, ext = 0, w = 1280, h = 720, fps = 30, i;
    SERVER_INFORMATION si;
    STREAM_CONFIGURATION sc;
    CONNECTION_LISTENER_CALLBACKS cl;
    DECODER_RENDERER_CALLBACKS dr;
    AUDIO_RENDERER_CALLBACKS ar;
    int rc, sent_fail = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--addr") && v) { addr = v; i++; }
        else if (!strcmp(a, "--url") && v) { url = v; i++; }
        else if (!strcmp(a, "--rikey") && v) { rikey = v; i++; }
        else if (!strcmp(a, "--rikeyid") && v) { rikeyid = atoi(v); i++; }
        else if (!strcmp(a, "--appver") && v) { appver = v; i++; }
        else if (!strcmp(a, "--gfever") && v) { gfever = v; i++; }
        else if (!strcmp(a, "--codec") && v) { codec = atoi(v); i++; }
        else if (!strcmp(a, "--enc") && v) { enc = atoi(v); i++; }
        else if (!strcmp(a, "--secs") && v) { secs = atoi(v); i++; }
        else if (!strcmp(a, "--text") && v) { text = v; i++; }
        else if (!strcmp(a, "--size") && v) { sscanf(v, "%dx%d", &w, &h); i++; }
        else if (!strcmp(a, "--fps") && v) { fps = atoi(v); i++; }
        else if (!strcmp(a, "--ext")) ext = 1;
        else { fprintf(stderr, "bad argument: %s\n", a); return 2; }
    }
    if (!addr || !url || !rikey || strlen(rikey) != 32) {
        fprintf(stderr, "usage: stream_harness --addr HOST --url RTSP_URL --rikey HEX32 --rikeyid N [options]\n");
        return 2;
    }

    LiInitializeServerInformation(&si);
    si.address = addr;
    si.serverInfoAppVersion = appver;
    si.serverInfoGfeVersion = gfever;
    si.rtspSessionUrl = url;
    si.serverCodecModeSupport = SCM_H264 | SCM_HEVC;

    LiInitializeStreamConfiguration(&sc);
    sc.width = w; sc.height = h; sc.fps = fps;
    sc.bitrate = 5000;
    sc.packetSize = 1024;
    sc.streamingRemotely = STREAM_CFG_LOCAL;
    sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    sc.supportedVideoFormats = codec == 1 ? VIDEO_FORMAT_H264 : (VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265);
    sc.clientRefreshRateX100 = fps * 100;
    sc.colorSpace = COLORSPACE_REC_709;
    sc.colorRange = COLOR_RANGE_LIMITED;
    sc.encryptionFlags = enc;
    for (i = 0; i < 16; i++) {
        int hi = hexval(rikey[2 * i]), lo = hexval(rikey[2 * i + 1]);
        if (hi < 0 || lo < 0) { fprintf(stderr, "rikey must be hex\n"); return 2; }
        sc.remoteInputAesKey[i] = (char)(hi * 16 + lo);
    }
    memset(sc.remoteInputAesIv, 0, sizeof(sc.remoteInputAesIv));
    sc.remoteInputAesIv[0] = (char)((rikeyid >> 24) & 0xff);
    sc.remoteInputAesIv[1] = (char)((rikeyid >> 16) & 0xff);
    sc.remoteInputAesIv[2] = (char)((rikeyid >> 8) & 0xff);
    sc.remoteInputAesIv[3] = (char)(rikeyid & 0xff);

    LiInitializeVideoCallbacks(&dr);
    dr.setup = dr_setup; dr.start = dr_noop; dr.stop = dr_noop; dr.cleanup = dr_noop;
    dr.submitDecodeUnit = dr_submit;
    dr.capabilities = 0;
    LiInitializeAudioCallbacks(&ar);
    ar.init = ar_init; ar.start = ar_noop; ar.stop = ar_noop; ar.cleanup = ar_noop;
    ar.decodeAndPlaySample = ar_play;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageStarting = cl_stage_start; cl.stageComplete = cl_stage_done; cl.stageFailed = cl_stage_fail;
    cl.connectionStarted = cl_started; cl.connectionTerminated = cl_terminated;
    cl.logMessage = logmsg;
    cl.rumble = cl_rumble; cl.rumbleTriggers = cl_rumble_trig; cl.setMotionEventState = cl_motion;
    cl.setControllerLED = cl_led; cl.setAdaptiveTriggers = cl_adaptive;
    cl.setTextInputState = cl_text_state;

    printf("harness start addr=%s enc=%d codec=%d secs=%d\n", addr, enc, codec, secs);
    fflush(stdout);
    rc = LiStartConnection(&si, &sc, &cl, &dr, &ar, NULL, 0, NULL, 0);
    printf("LiStartConnection returned %d\n", rc);
    fflush(stdout);
    if (rc != 0) return 1;

    if (ext) {
        int r;
        SLEEP_MS(1500);
        r = LiSendExecServerCmd(1);          printf("ext exec server cmd rc=%d\n", r);
        r = LiSendEmptyPayload();            printf("ext empty payload rc=%d\n", r);
        r = LiSendMouseMoveEvent(5, 5);      printf("ext mouse move rc=%d\n", r);
        r = LiSendKeyboardEvent2(0x41, KEY_ACTION_DOWN, 0, 0); printf("ext key down rc=%d\n", r);
        r = LiSendKeyboardEvent2(0x41, KEY_ACTION_UP, 0, 0);   printf("ext key up rc=%d\n", r);
        r = LiSendControllerArrivalEvent(0, 1, LI_CTYPE_PS, 0xFFFFFFFF, LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE | LI_CCAP_TOUCHPAD);
        printf("ext controller arrival rc=%d\n", r);
        r = LiSendControllerTouchEvent2(0, LI_TOUCH_EVENT_DOWN, 1, 7, 0.5f, 0.5f, 1.0f);
        printf("ext touchpad (second pad) rc=%d\n", r);
        r = LiSendControllerMotionEvent(0, LI_MOTION_TYPE_GYRO, 0.1f, 0.2f, 0.3f);
        printf("ext motion rc=%d\n", r);
        r = LiSendControllerBatteryEvent(0, LI_BATTERY_STATE_CHARGING, 80);
        printf("ext battery rc=%d\n", r);
        if (!text) text = "NyxPlay text \xC3\xA7\xC4\x9F\xC3\xBC";
    }
    if (text) {
        int r = LiSendUtf8TextEvent(text, (unsigned int)strlen(text));
        printf("text event rc=%d\n", r);
    }

    for (i = 0; i < secs * 10 && !g_terminated; i++) {
        if (i % 5 == 0) {
            if (LiSendMouseMoveEvent((short)((i / 5) % 2 ? 3 : -3), 0) != 0) sent_fail++;
        }
        SLEEP_MS(100);
    }
    {
        uint32_t rtt = 0, var = 0;
        if (LiGetEstimatedRttInfo(&rtt, &var)) printf("rtt=%u var=%u\n", rtt, var);
    }
    printf("stats frames=%ld idr=%ld audio_packets=%ld bytes=%llu hash=%08x format=0x%x input_send_failures=%d text_state_msgs=%ld\n",
           g_frames, g_idr, g_audio, (unsigned long long)g_bytes, g_hash, g_first_format, sent_fail, g_text_state);
    fflush(stdout);
    LiStopConnection();
    printf("connection stopped, terminated_by_host=%ld code=%d\n", g_terminated, g_term_code);
    return (g_frames > 0 && g_started && !g_stage_failed) ? 0 : 1;
}
