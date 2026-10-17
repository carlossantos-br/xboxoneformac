/* xboxoneformac — SDL joystick driver used inside Wine.
 * Included by src/joystick/SDL_joystick.c (see patches/sdl-wine-bridge.patch).
 * xboxone-bridge owns the physical controller; this driver only reads the
 * small state file it publishes and forwards vibration requests back. */
#include "bridge_protocol.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int XB_fd = -1;
static SDL_bool XB_present;
static SDL_JoystickID XB_instance = -1;
static XBState XB_state;
static XBRumble XB_rumble;

static const char *XB_StatePath(void)
{
    const char *override = SDL_getenv("XBOXONEFORMAC_STATE_FILE");
    return override && *override ? override : XB_STATE_PATH;
}

static SDL_bool XB_ReadState(void)
{
    XBState a, b;
    uint64_t now;
    if (XB_fd < 0) {
        struct stat st;
        XB_fd = open(XB_StatePath(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (XB_fd < 0) return SDL_FALSE;
        if (fstat(XB_fd, &st) || !S_ISREG(st.st_mode)) {
            close(XB_fd);
            XB_fd = -1;
            return SDL_FALSE;
        }
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (pread(XB_fd, &a, sizeof(a), 0) != sizeof(a) || pread(XB_fd, &b, sizeof(b), 0) != sizeof(b)) {
            close(XB_fd);
            XB_fd = -1;
            return SDL_FALSE;
        }
        if (SDL_memcmp(&a, &b, sizeof(a)) || a.sequence != a.sequence_end) continue;
        now = xb_now_ns();
        if (a.magic != XB_MAGIC || a.connected != 1 || a.timestamp_ns > now || now - a.timestamp_ns > 500000000ull)
            return SDL_FALSE;
        XB_state = a;
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

static void XB_Detect(void)
{
    SDL_bool present = XB_ReadState();
    if (present == XB_present) return;
    XB_present = present;
    if (present) {
        XB_instance = SDL_GetNextJoystickInstanceID();
        SDL_PrivateJoystickAdded(XB_instance);
    } else {
        SDL_PrivateJoystickRemoved(XB_instance);
    }
}

static int XB_Init(void)
{
    /* Wine's device service is never the focused app. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    XB_present = SDL_FALSE;
    XB_Detect();
    return 0;
}

static int XB_Count(void) { return XB_present ? 1 : 0; }
static const char *XB_Name(int index) { return "Xbox One Controller (xboxoneformac)"; }
static const char *XB_Path(int index) { return XB_StatePath(); }
static int XB_SteamSlot(int index) { return -1; }
static int XB_Player(int index) { return 0; }
static void XB_SetPlayer(int index, int player) { }

/* Presented as an Xbox 360 pad so Wine maps it straight to XInput. */
static SDL_JoystickGUID XB_GUID(int index)
{
    return SDL_CreateJoystickGUID(SDL_HARDWARE_BUS_USB, 0x045e, 0x028e, 1, "Microsoft", XB_Name(index), 'w', 0);
}

static SDL_JoystickID XB_ID(int index) { return XB_instance; }

static int XB_Open(SDL_Joystick *joystick, int index)
{
    if (!XB_present) return SDL_SetError("Xbox One controller disconnected");
    joystick->naxes = 6;
    joystick->nbuttons = 15;
    joystick->nhats = 0;
    joystick->epowerlevel = SDL_JOYSTICK_POWER_WIRED;
    return 0;
}

static int XB_WriteRumble(void)
{
    ssize_t written;
    const char *override = SDL_getenv("XBOXONEFORMAC_RUMBLE_FILE");
    int fd = open(override && *override ? override : XB_RUMBLE_PATH, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return SDL_SetError("xboxoneformac vibration unavailable");
    XB_rumble.sequence += 2;
    XB_rumble.sequence_end = XB_rumble.sequence;
    XB_rumble.timestamp_ns = xb_now_ns();
    written = pwrite(fd, &XB_rumble, sizeof(XB_rumble), 0);
    close(fd);
    return written == sizeof(XB_rumble) ? 0 : SDL_SetError("xboxoneformac vibration write failed");
}

static int XB_Rumble(SDL_Joystick *joy, Uint16 low, Uint16 high)
{
    XB_rumble.low = low;
    XB_rumble.high = high;
    return XB_WriteRumble();
}

static int XB_RumbleTriggers(SDL_Joystick *joy, Uint16 left, Uint16 right)
{
    XB_rumble.left_trigger = left;
    XB_rumble.right_trigger = right;
    return XB_WriteRumble();
}

static Uint32 XB_Caps(SDL_Joystick *joy) { return SDL_JOYCAP_RUMBLE | SDL_JOYCAP_RUMBLE_TRIGGERS; }
static int XB_LED(SDL_Joystick *joy, Uint8 r, Uint8 g, Uint8 b) { return SDL_Unsupported(); }
static int XB_Effect(SDL_Joystick *joy, const void *data, int size) { return SDL_Unsupported(); }
static int XB_Sensors(SDL_Joystick *joy, SDL_bool enabled) { return SDL_Unsupported(); }

static void XB_Update(SDL_Joystick *joy)
{
    if (!XB_ReadState()) {
        XB_Detect();
        return;
    }
    for (int i = 0; i < 6; ++i) SDL_PrivateJoystickAxis(joy, (Uint8)i, XB_state.axes[i]);
    for (int i = 0; i < 15; ++i) SDL_PrivateJoystickButton(joy, (Uint8)i, (XB_state.buttons >> i) & 1);
}

static void XB_Close(SDL_Joystick *joy) { }

static void XB_Quit(void)
{
    if (XB_fd >= 0) close(XB_fd);
    XB_fd = -1;
    XB_present = SDL_FALSE;
    SDL_zero(XB_rumble);
}

/* Buttons and axes already arrive in SDL game controller order. */
static SDL_bool XB_Mapping(int index, SDL_GamepadMapping *out)
{
    SDL_InputMapping *buttons[] = { &out->a, &out->b, &out->x, &out->y, &out->back, &out->guide, &out->start,
                                    &out->leftstick, &out->rightstick, &out->leftshoulder, &out->rightshoulder,
                                    &out->dpup, &out->dpdown, &out->dpleft, &out->dpright };
    SDL_InputMapping *axes[] = { &out->leftx, &out->lefty, &out->rightx, &out->righty,
                                 &out->lefttrigger, &out->righttrigger };
    SDL_zerop(out);
    for (int i = 0; i < 15; ++i) {
        buttons[i]->kind = EMappingKind_Button;
        buttons[i]->target = (Uint8)i;
    }
    for (int i = 0; i < 6; ++i) {
        axes[i]->kind = EMappingKind_Axis;
        axes[i]->target = (Uint8)i;
    }
    return SDL_TRUE;
}

static SDL_JoystickDriver SDL_XBOXONEFORMAC_JoystickDriver = {
    XB_Init, XB_Count, XB_Detect, XB_Name, XB_Path, XB_SteamSlot, XB_Player,
    XB_SetPlayer, XB_GUID, XB_ID, XB_Open, XB_Rumble, XB_RumbleTriggers,
    XB_Caps, XB_LED, XB_Effect, XB_Sensors, XB_Update, XB_Close, XB_Quit, XB_Mapping
};
