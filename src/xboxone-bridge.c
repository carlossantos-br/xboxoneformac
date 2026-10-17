/* xboxone-bridge — makes a wired Xbox One controller usable by Wine games.
 *
 * macOS has no driver for the wired Xbox One protocol (GIP), and apps such as
 * Chrome may hold the USB device exclusively. While one of the configured
 * games is running, this service (root) captures the controller, reads it
 * with SDL + libusb and publishes its state to a small file. The SDL library
 * installed inside the Wine wrapper reads that file and shows Wine a normal
 * XInput controller. When the game closes, the USB device is released.
 *
 * While the controller is in use the display is kept awake, and moving it
 * counts as user activity (the Mac does not see controller input otherwise).
 */
#include <SDL.h>
#include <libusb.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include "bridge_protocol.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <unistd.h>

#define XBOX_VID 0x045e
#define XBOX_PID 0x02d1
#define MAX_GAMES 16

static volatile sig_atomic_t stopping;
static const char *games[MAX_GAMES];
static int game_count;

static void stop_handler(int sig) { (void)sig; stopping = 1; }

static void logmsg(const char *fmt, ...)
{
    char stamp[32];
    time_t now = time(NULL);
    va_list ap;
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stdout, "%s ", stamp);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int contains_nocase(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (!strncasecmp(haystack, needle, n)) return 1;
    return 0;
}

/* pbi_name keeps up to 31 characters of the executable name, pbi_comm 15. */
static int name_matches(const struct proc_bsdinfo *info, const char *game)
{
    size_t n = strlen(game), c = strlen(info->pbi_comm), p = strlen(info->pbi_name);
    if (p > 0 && p <= n && p >= (n < 31 ? n : 31) && !strncasecmp(info->pbi_name, game, p)) return 1;
    return c > 0 && c <= n && c >= (n < 15 ? n : 15) && !strncasecmp(info->pbi_comm, game, c);
}

/* Returns the PID of a configured game running under Wine for this user. */
static pid_t game_pid(uid_t uid)
{
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    pid_t found = 0;
    if (bytes <= 0) return 0;
    pid_t *pids = calloc(1, (size_t)bytes + 4096);
    if (!pids) return 0;
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, bytes + 4096);
    for (int i = 0; i < bytes / (int)sizeof(pid_t) && !found; i++) {
        struct proc_bsdinfo info = {0};
        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (pids[i] <= 0) continue;
        if (proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) continue;
        if (info.pbi_uid != uid) continue;
        int wanted = 0;
        for (int g = 0; g < game_count && !wanted; g++) wanted = name_matches(&info, games[g]);
        if (!wanted) continue;
        /* Only Wine processes: the executable is Wine's loader. */
        if (proc_pidpath(pids[i], path, sizeof(path)) <= 0 || !contains_nocase(path, "wine")) continue;
        found = pids[i];
    }
    free(pids);
    return found;
}

static int safe_file(const char *path, mode_t mode, uid_t owner)
{
    struct stat st;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        (st.st_uid != 0 && st.st_uid != owner) || fchmod(fd, mode) || fchown(fd, owner, (gid_t)-1)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_state(int fd, XBState *state, SDL_GameController *pad)
{
    state->magic = XB_MAGIC;
    state->connected = pad && SDL_GameControllerGetAttached(pad);
    state->buttons = 0;
    for (int i = 0; i < 6; i++) {
        int value = state->connected ? SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)i) : 0;
        /* Triggers are 0..32767 in SDL; send them as a full -32768..32767 axis. */
        state->axes[i] = i >= 4 ? (int16_t)((value * 65535LL + 32766) / 32767 - 32768) : (int16_t)value;
    }
    if (state->connected)
        for (int i = 0; i < 15; i++)
            if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)i)) state->buttons |= 1u << i;
    state->sequence += 2;
    state->sequence_end = state->sequence;
    state->timestamp_ns = xb_now_ns();
    return pwrite(fd, state, sizeof(*state), 0) == sizeof(*state) ? 0 : -1;
}

static void read_rumble(int fd, SDL_GameController *pad, uint64_t *last)
{
    XBRumble a, b;
    uint64_t now;
    if (pread(fd, &a, sizeof(a), 0) != sizeof(a) || pread(fd, &b, sizeof(b), 0) != sizeof(b)) return;
    now = xb_now_ns();
    if (memcmp(&a, &b, sizeof(a)) || a.sequence != a.sequence_end || a.sequence == *last ||
        a.timestamp_ns > now || now - a.timestamp_ns > 1000000000ull) return;
    *last = a.sequence;
    /* A frozen game cannot leave the motors running: every command expires. */
    SDL_GameControllerRumble(pad, a.low, a.high, 2500);
    SDL_GameControllerRumbleTriggers(pad, a.left_trigger, a.right_trigger, 2500);
}

/* ---- keeping the Mac awake while the controller is in use ---- */
#define AWAKE_AFTER_INPUT_NS (15ull * 60 * 1000000000ull) /* AFK longer than this: normal sleep */
static IOPMAssertionID display_assertion;
static int display_assertion_held;
static uint64_t next_assertion_try;
static IOPMAssertionID activity_assertion = kIOPMNullAssertionID;
static XBState last_active;
static uint64_t last_activity_ns, last_input_ns;

static void keep_display_awake(int on)
{
    if (on && !display_assertion_held) {
        uint64_t now = xb_now_ns();
        if (now < next_assertion_try) return;
        display_assertion_held = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep,
            kIOPMAssertionLevelOn, CFSTR("Xbox One controller in use (xboxoneformac)"),
            &display_assertion) == kIOReturnSuccess;
        if (!display_assertion_held) {
            logmsg("could not keep the display awake");
            next_assertion_try = now + 30000000000ull;
        }
    } else if (!on && display_assertion_held) {
        IOPMAssertionRelease(display_assertion);
        display_assertion_held = 0;
    }
}

/* Real input (not stick drift) resets the idle timer like a key press would. */
static void note_activity(const XBState *state)
{
    int moved = state->buttons != last_active.buttons;
    for (int i = 0; i < 6 && !moved; i++) moved = abs(state->axes[i] - last_active.axes[i]) > 6000;
    if (!moved) return;
    last_active = *state;
    uint64_t now = xb_now_ns();
    last_input_ns = now;
    if (now - last_activity_ns < 5000000000ull) return;
    last_activity_ns = now;
    IOPMAssertionDeclareUserActivity(CFSTR("Xbox One controller input (xboxoneformac)"),
                                     kIOPMUserActiveLocal, &activity_assertion);
}

/* ---- USB capture ---- */
static void release_device(libusb_context **ctx, libusb_device_handle **usb, SDL_GameController **pad)
{
    if (*pad) {
        SDL_GameControllerRumble(*pad, 0, 0, 0);
        SDL_GameControllerClose(*pad);
        *pad = NULL;
    }
    SDL_Quit();
    if (*usb) {
        libusb_attach_kernel_driver(*usb, 0);
        libusb_close(*usb);
        *usb = NULL;
    }
    if (*ctx) {
        libusb_exit(*ctx);
        *ctx = NULL;
    }
}

/* 1 = controller ready, 0 = no controller plugged in, -1 = error. */
static int capture(libusb_context **ctx, libusb_device_handle **usb, SDL_GameController **pad)
{
    int err;
    if (libusb_init(ctx)) return -1;
    *usb = libusb_open_device_with_vid_pid(*ctx, XBOX_VID, XBOX_PID);
    if (!*usb) {
        libusb_exit(*ctx);
        *ctx = NULL;
        return 0;
    }
    /* Takes the device from whoever holds it (needs root on macOS). */
    err = libusb_detach_kernel_driver(*usb, 0);
    if (err && err != LIBUSB_ERROR_NOT_FOUND) {
        logmsg("could not capture the controller: %s", libusb_error_name(err));
        libusb_close(*usb);
        *usb = NULL;
        libusb_exit(*ctx);
        *ctx = NULL;
        return -1;
    }
    if (SDL_Init(SDL_INIT_GAMECONTROLLER)) {
        logmsg("SDL init failed: %s", SDL_GetError());
        release_device(ctx, usb, pad);
        return -1;
    }
    for (int attempt = 0; attempt < 400 && !stopping; attempt++) {
        SDL_GameControllerUpdate();
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_JoystickGetDeviceVendor(i) != XBOX_VID || SDL_JoystickGetDeviceProduct(i) != XBOX_PID) continue;
            *pad = SDL_GameControllerOpen(i);
            if (*pad) {
                last_input_ns = xb_now_ns(); /* plugging it in counts as use */
                logmsg("controller connected");
                return 1;
            }
        }
        SDL_Delay(20);
    }
    logmsg("controller captured but not readable: %s", SDL_GetError());
    release_device(ctx, usb, pad);
    return -1;
}

static int parse_uid(const char *text, uid_t *uid)
{
    char *end = NULL;
    unsigned long number = strtoul(text, &end, 10);
    if (!*text || *end || number < 500 || number > UINT32_MAX) return -1;
    *uid = (uid_t)number;
    return 0;
}

static int usage(const char *self)
{
    fprintf(stderr, "Usage: %s --run UID [--game NAME.exe]...\n       %s --status UID [--game NAME.exe]...\n", self, self);
    return 2;
}

int main(int argc, char **argv)
{
    _Static_assert(sizeof(XBState) == 64, "state ABI");
    _Static_assert(sizeof(XBRumble) == 32, "rumble ABI");
    uid_t uid;
    if (argc < 3 || parse_uid(argv[2], &uid)) return usage(argv[0]);
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--game") || i + 1 >= argc || game_count == MAX_GAMES) return usage(argv[0]);
        games[game_count++] = argv[++i];
    }
    if (!game_count) games[game_count++] = "Warframe.x64.exe";

    if (!strcmp(argv[1], "--status")) {
        printf("game running for uid %u: %d\n", uid, (int)game_pid(uid));
        return 0;
    }
    if (strcmp(argv[1], "--run")) return usage(argv[0]);
    if (geteuid() != 0) {
        fprintf(stderr, "Run as root (it is started by launchd); USB capture needs it.\n");
        return 77;
    }
    struct stat dir;
    if (lstat(XB_DIRECTORY, &dir) || !S_ISDIR(dir.st_mode) || dir.st_uid != 0 || (dir.st_mode & 0022)) {
        fprintf(stderr, XB_DIRECTORY " must exist and belong to root.\n");
        return 78;
    }
    int state_fd = safe_file(XB_STATE_PATH, 0644, 0);
    int rumble_fd = safe_file(XB_RUMBLE_PATH, 0600, uid);
    if (state_fd < 0 || rumble_fd < 0 || flock(state_fd, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "cannot open the state files, or another bridge is running.\n");
        return 78;
    }
    XBRumble no_rumble = {0};
    pwrite(rumble_fd, &no_rumble, sizeof(no_rumble), 0);

    setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 1);
    setenv("SDL_JOYSTICK_MFI", "0", 1);
    setenv("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x045e/0x02d1", 1);
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);

    XBState state = {0};
    libusb_context *ctx = NULL;
    libusb_device_handle *usb = NULL;
    SDL_GameController *pad = NULL;
    uint64_t next_game_check = 0, next_capture = 0, next_idle_write = 0, last_rumble = 0;
    int was_connected = 0;
    pid_t game = 0;
    logmsg("started for uid %u", uid);

    while (!stopping) {
        uint64_t now = xb_now_ns();
        if (now >= next_game_check) {
            game = game_pid(uid);
            next_game_check = now + 1000000000ull;
        }
        if (!game && usb) {
            release_device(&ctx, &usb, &pad);
            logmsg("game closed; controller released");
        }
        if (game && !usb && now >= next_capture) {
            int result = capture(&ctx, &usb, &pad);
            next_capture = xb_now_ns() + (result < 0 ? 30000000000ull : 1000000000ull);
            last_rumble = 0;
        }
        if (pad) {
            SDL_GameControllerUpdate();
            if (!SDL_GameControllerGetAttached(pad)) {
                release_device(&ctx, &usb, &pad);
                logmsg("controller unplugged");
            } else {
                read_rumble(rumble_fd, pad, &last_rumble);
            }
        }
        /* Idle: a slow heartbeat is enough, Wine ignores a disconnected state. */
        if (pad || was_connected || now >= next_idle_write) {
            if (write_state(state_fd, &state, pad)) {
                logmsg("state write failed");
                break;
            }
            next_idle_write = now + 2000000000ull;
            was_connected = pad != NULL;
        }
        if (pad) note_activity(&state);
        keep_display_awake(pad != NULL && xb_now_ns() - last_input_ns < AWAKE_AFTER_INPUT_NS);
        usleep(pad ? 4000 : 100000);
    }
    release_device(&ctx, &usb, &pad);
    keep_display_awake(0);
    write_state(state_fd, &state, NULL);
    close(state_fd);
    close(rumble_fd);
    logmsg("stopped");
    return 0;
}
