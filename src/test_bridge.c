/* Tests the Wine-side SDL driver against a simulated bridge state file:
 * buttons, axes, triggers, vibration, stale data, bad frames, unplug/replug. */
#include <SDL.h>
#include "bridge_protocol.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static int state_fd;
static XBState state;
static void publish(int connected) {
    state.magic=XB_MAGIC;
    state.connected=connected;
    state.timestamp_ns=xb_now_ns();
    state.sequence+=2;
    state.sequence_end=state.sequence;
    assert(pwrite(state_fd,&state,sizeof(state),0)==sizeof(state));
    SDL_JoystickUpdate();
}
static void pump(void) {
    for(int i=0;i<4;i++) { SDL_JoystickUpdate(); SDL_Delay(5); }
}
int main(void) {
    _Static_assert(sizeof(XBState)==64,"state ABI");
    _Static_assert(sizeof(XBRumble)==32,"rumble ABI");
    char path[]="/tmp/xboxoneformac-state.XXXXXX";
    state_fd=mkstemp(path); assert(state_fd>=0);
    char rumble_path[]="/tmp/xboxoneformac-rumble.XXXXXX";
    int rumble_fd=mkstemp(rumble_path); assert(rumble_fd>=0);
    setenv("XBOXONEFORMAC_STATE_FILE",path,1);
    setenv("XBOXONEFORMAC_RUMBLE_FILE",rumble_path,1);
    setenv("SDL_JOYSTICK_HIDAPI","0",1);
    setenv("SDL_JOYSTICK_MFI","0",1);
    state.axes[4]=state.axes[5]=-32768;
    publish(1);
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER)==0);
    publish(1);
    pump();
    if(SDL_NumJoysticks()!=1) {
        fprintf(stderr,"FAIL: expected the bridge controller; found %d\n",SDL_NumJoysticks());
        SDL_Quit(); unlink(path); unlink(rumble_path); return 1;
    }
    SDL_GameController *pad=SDL_GameControllerOpen(0);
    assert(pad);
    SDL_Joystick *joy=SDL_GameControllerGetJoystick(pad);
    assert(SDL_JoystickGetVendor(joy)==0x045e);
    assert(SDL_JoystickGetProduct(joy)==0x028e);
    assert(SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT)==0);
    for(int b=0;b<15;b++) {
        state.buttons=1u<<b; publish(1); pump();
        for(int j=0;j<15;j++) assert(SDL_GameControllerGetButton(pad,j)==(j==b));
    }
    state.buttons=0;
    for(int axis=0;axis<6;axis++) {
        const int values[]={-32768,0,32767};
        for(int k=0;k<3;k++) {
            state.axes[axis]=values[k]; publish(1); pump();
            assert(SDL_JoystickGetAxis(joy,axis)==values[k]);
            int expected=axis>=4?(int)((values[k]+32768LL)*32767/65535):values[k];
            if(SDL_GameControllerGetAxis(pad,axis)!=expected) fprintf(stderr,"axis=%d input=%d expected=%d actual=%d mapping=%s\n",axis,values[k],expected,SDL_GameControllerGetAxis(pad,axis),SDL_GameControllerMapping(pad));
            assert(SDL_GameControllerGetAxis(pad,axis)==expected);
        }
    }
    assert(SDL_GameControllerRumble(pad,1234,2345,100)==0);
    XBRumble rumble; assert(pread(rumble_fd,&rumble,sizeof(rumble),0)==sizeof(rumble));
    assert(rumble.low==1234 && rumble.high==2345 && rumble.sequence==rumble.sequence_end);
    SDL_Delay(150); publish(1); pump();
    assert(pread(rumble_fd,&rumble,sizeof(rumble),0)==sizeof(rumble));
    assert(rumble.low==0 && rumble.high==0);
    SDL_JoystickID old_id=SDL_JoystickInstanceID(joy);
    publish(0); pump();
    assert(!SDL_GameControllerGetAttached(pad));
    assert(SDL_NumJoysticks()==0);
    SDL_GameControllerClose(pad);
    publish(1); pump();
    assert(SDL_NumJoysticks()==1);
    pad=SDL_GameControllerOpen(0); assert(pad);
    assert(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))!=old_id);
    state.timestamp_ns=xb_now_ns()-2000000000ull;
    assert(pwrite(state_fd,&state,sizeof(state),0)==sizeof(state));
    pump(); assert(!SDL_GameControllerGetAttached(pad));
    SDL_GameControllerClose(pad);
    state.timestamp_ns=xb_now_ns(); state.sequence_end=state.sequence+2;
    assert(pwrite(state_fd,&state,sizeof(state),0)==sizeof(state));
    pump(); assert(SDL_NumJoysticks()==0);
    publish(1); pump(); assert(SDL_NumJoysticks()==1);
    state.magic=0; assert(pwrite(state_fd,&state,sizeof(state),0)==sizeof(state));
    pump(); assert(SDL_NumJoysticks()==0);
    SDL_Quit(); close(state_fd); close(rumble_fd); unlink(path); unlink(rumble_path);
    puts("PASS: 15 buttons, 6 axes, triggers, vibration, timeout, invalid frames, disconnect and reconnect");
    return 0;
}
