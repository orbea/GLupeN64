/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - main.c                                                  *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2012 CasualJames                                        *
 *   Copyright (C) 2008-2009 Richard Goedeken                              *
 *   Copyright (C) 2008 Ebenblues Nmn Okaygo Tillin9                       *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This is MUPEN64's main entry point. It contains code that is common
 * to both the gui and non-gui versions of mupen64. See
 * gui subdirectories for the gui-specific code.
 * if you want to implement an interface, you should look here
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libretro_memory.h>

#define M64P_CORE_PROTOTYPES 1
#include "ai/ai_controller.h"
#include "api/callbacks.h"
#include "api/config.h"
#include "api/debugger.h"
#include "api/m64p_config.h"
#include "api/m64p_types.h"
#include "api/m64p_vidext.h"
#include "api/vidext.h"
#include "main/cheat.h"
#include "main/eep_file.h"
#include "main/eventloop.h"
#include "main/fla_file.h"
#include "main/main.h"
#include "memory/memory.h"
#include "main/mpk_file.h"
#include "osal/files.h"
#include "osal/preproc.h"
#include "osd/osd.h"
#include "osd/screenshot.h"
#include "pi/pi_controller.h"
#include "plugin/emulate_game_controller_via_input_plugin.h"
#include "plugin/emulate_speaker_via_audio_plugin.h"
#include "plugin/get_time_using_C_localtime.h"
#include "plugin/plugin.h"
#include "plugin/rumble_via_input_plugin.h"
#include "main/profile.h"
#include "r4300/r4300.h"
#include "r4300/r4300_core.h"
#include "r4300/reset.h"
#include "rdp/rdp_core.h"
#include "ri/ri_controller.h"
#include "main/rom.h"
#include "rsp/rsp_core.h"
#include "main/savestates.h"
#include "si/si_controller.h"
#include "main/sra_file.h"
#include "main/util.h"
#include "vi/vi_controller.h"

#ifdef DBG
#include "debugger/dbg_debugger.h"
#include "debugger/dbg_types.h"
#endif

#include <libretro.h>

extern retro_input_poll_t poll_cb;

/* version number for Core config section */
#define CONFIG_PARAM_VERSION 1.01

/** globals **/
m64p_handle g_CoreConfig = NULL;

m64p_frame_callback g_FrameCallback = NULL;

int         g_MemHasBeenBSwapped = 0;   // store byte-swapped flag so we don't swap twice when re-playing game
int         g_EmulatorRunning = 0;      // need separate boolean to tell if emulator is running, since --nogui doesn't use a thread

ALIGN(16, uint32_t g_rdram[RDRAM_MAX_SIZE/4]);
struct ai_controller g_ai;
struct pi_controller g_pi;
struct ri_controller g_ri;
struct si_controller g_si;
struct vi_controller g_vi;
struct r4300_core g_r4300;
struct rdp_core g_dp;
struct rsp_core g_sp;

int g_delay_si = 0;

int g_gs_vi_counter = 0;

/** static (local) variables **/
static int   l_CurrentFrame = 0;         // frame counter
static int   l_TakeScreenshot = 0;       // Tell OSD Rendering callback to take a screenshot just before drawing the OSD
static int   l_SpeedFactor = 100;        // percentage of nominal game speed at which emulator is running
static int   l_FrameAdvance = 0;         // variable to check if we pause on next frame
static int   l_MainSpeedLimit = 1;       // insert delay during vi_interrupt to keep speed at real-time

/*********************************************************************************************************
* helper functions
*/

void main_message(m64p_msg_level level, unsigned int corner, const char *format, ...)
{
    va_list ap;
    char buffer[2049];
    va_start(ap, format);
    vsnprintf(buffer, 2047, format, ap);
    buffer[2048]='\0';
    va_end(ap);

    /* send message to front-end */
    DebugMessage(level, "%s", buffer);
}

void main_check_inputs(void)
{
    poll_cb();
}

/*********************************************************************************************************
* global functions, for adjusting the core emulator behavior
*/

int main_set_core_defaults(void)
{
    float fConfigParamsVersion;
    int bSaveConfig = 0, bUpgrade = 0;

    if (ConfigGetParameter(g_CoreConfig, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_WARNING, "No version number in 'Core' config section. Setting defaults.");
        ConfigDeleteSection("Core");
        ConfigOpenSection("Core", &g_CoreConfig);
        bSaveConfig = 1;
    }
    else if (((int) fConfigParamsVersion) != ((int) CONFIG_PARAM_VERSION))
    {
        DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in 'Core' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float) CONFIG_PARAM_VERSION);
        ConfigDeleteSection("Core");
        ConfigOpenSection("Core", &g_CoreConfig);
        bSaveConfig = 1;
    }
    else if ((CONFIG_PARAM_VERSION - fConfigParamsVersion) >= 0.0001f)
    {
        float fVersion = (float) CONFIG_PARAM_VERSION;
        ConfigSetParameter(g_CoreConfig, "Version", M64TYPE_FLOAT, &fVersion);
        DebugMessage(M64MSG_INFO, "Updating parameter set version in 'Core' config section to %.2f", fVersion);
        bUpgrade = 1;
        bSaveConfig = 1;
    }

    /* parameters controlling the operation of the core */
    ConfigSetDefaultFloat(g_CoreConfig, "Version", (float) CONFIG_PARAM_VERSION,  "Mupen64Plus Core config parameter set version number.  Please don't change this version number.");
    ConfigSetDefaultBool(g_CoreConfig, "OnScreenDisplay", 1, "Draw on-screen display if True, otherwise don't draw OSD");
#if defined(DYNAREC)
    ConfigSetDefaultInt(g_CoreConfig, "R4300Emulator", 2, "Use Pure Interpreter if 0, Cached Interpreter if 1, or Dynamic Recompiler if 2 or more");
#else
    ConfigSetDefaultInt(g_CoreConfig, "R4300Emulator", 1, "Use Pure Interpreter if 0, Cached Interpreter if 1, or Dynamic Recompiler if 2 or more");
#endif
    ConfigSetDefaultBool(g_CoreConfig, "NoCompiledJump", 0, "Disable compiled jump commands in dynamic recompiler (should be set to False) ");
    ConfigSetDefaultBool(g_CoreConfig, "DisableExtraMem", 0, "Disable 4MB expansion RAM pack. May be necessary for some games");
    ConfigSetDefaultBool(g_CoreConfig, "AutoStateSlotIncrement", 0, "Increment the save state slot after each save operation");
    ConfigSetDefaultBool(g_CoreConfig, "EnableDebugger", 0, "Activate the R4300 debugger when ROM execution begins, if core was built with Debugger support");
    ConfigSetDefaultBool(g_CoreConfig, "DelaySI", 1, "Delay interrupt after DMA SI read/write");
    ConfigSetDefaultInt(g_CoreConfig, "CountPerOp", 0, "Force number of cycles per emulated instruction");
    ConfigSetDefaultString(g_CoreConfig, "SharedDataPath", "", "Path to a directory to search when looking for shared data files");

    /* handle upgrades */
    if (bUpgrade)
    {
        if (fConfigParamsVersion < 1.01f)
        {  // added separate SaveSRAMPath parameter in v1.01
            const char *pccSaveStatePath = ConfigGetParamString(g_CoreConfig, "SaveStatePath");
            if (pccSaveStatePath != NULL)
                ConfigSetParameter(g_CoreConfig, "SaveSRAMPath", M64TYPE_STRING, pccSaveStatePath);
        }
    }

    if (bSaveConfig)
        ConfigSaveSection("Core");

    /* set config parameters for keyboard and joystick commands */
    return 1;
}

m64p_error main_core_state_query(m64p_core_param param, int *rval)
{
    switch (param)
    {
        case M64CORE_EMU_STATE:
            if (!g_EmulatorRunning)
                *rval = M64EMU_STOPPED;
            else
                *rval = M64EMU_RUNNING;
            break;
        default:
            return M64ERR_INPUT_INVALID;
    }

    return M64ERR_SUCCESS;
}

m64p_error main_core_state_set(m64p_core_param param, int val)
{
    switch (param)
    {
        case M64CORE_EMU_STATE:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (val == M64EMU_STOPPED)
            {        
                /* this stop function is asynchronous.  The emulator may not terminate until later */
                main_stop();
                return M64ERR_SUCCESS;
            }
            else if (val == M64EMU_RUNNING)
            {
                return M64ERR_SUCCESS;
            }
            else if (val == M64EMU_PAUSED)
            {    
                return M64ERR_SUCCESS;
            }
            return M64ERR_INPUT_INVALID;
        case M64CORE_VIDEO_MODE:
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            if (val == M64VIDEO_WINDOWED)
            {
                if (VidExt_InFullscreenMode())
                    gfx.changeWindow();
                return M64ERR_SUCCESS;
            }
            else if (val == M64VIDEO_FULLSCREEN)
            {
                if (!VidExt_InFullscreenMode())
                    gfx.changeWindow();
                return M64ERR_SUCCESS;
            }
            return M64ERR_INPUT_INVALID;
        case M64CORE_SPEED_LIMITER:
            return M64ERR_SUCCESS;
        case M64CORE_VIDEO_SIZE:
        {
            // the front-end app is telling us that the user has resized the video output frame, and so
            // we should try to update the video plugin accordingly.  First, check state
            int width, height;
            if (!g_EmulatorRunning)
                return M64ERR_INVALID_STATE;
            width = (val >> 16) & 0xffff;
            height = val & 0xffff;
            // then call the video plugin.  if the video plugin supports resizing, it will resize its viewport and call
            // VidExt_ResizeWindow to update the window manager handling our opengl output window
            //gfx.resizeVideoOutput(width, height);
            return M64ERR_SUCCESS;
        }
        // these are only used for callbacks; they cannot be queried or set
        case M64CORE_STATE_LOADCOMPLETE:
        case M64CORE_STATE_SAVECOMPLETE:
            return M64ERR_INPUT_INVALID;
        default:
            return M64ERR_INPUT_INVALID;
    }
}

m64p_error main_get_screen_size(int *width, int *height)
{
    gfx.readScreen(NULL, width, height, 0);
    return M64ERR_SUCCESS;
}

m64p_error main_read_screen(void *pixels, int bFront)
{
    int width_trash, height_trash;
    gfx.readScreen(pixels, &width_trash, &height_trash, bFront);
    return M64ERR_SUCCESS;
}

m64p_error main_reset(int do_hard_reset)
{
    if (do_hard_reset)
        reset_hard_job |= 1;
    else
        reset_soft();
    return M64ERR_SUCCESS;
}

/*********************************************************************************************************
* global functions, callbacks from the r4300 core or from other plugins
*/

void new_frame(void)
{
    if (g_FrameCallback != NULL)
        (*g_FrameCallback)(l_CurrentFrame);

    /* advance the current frame */
    l_CurrentFrame++;

    if (l_FrameAdvance) {
        rompause = 1;
        l_FrameAdvance = 0;
        StateChanged(M64CORE_EMU_STATE, M64EMU_PAUSED);
    }
}

/* called on vertical interrupt.
 * Allow the core to perform various things */
void new_vi(void)
{
    main_check_inputs();
}

static void connect_all(
        struct r4300_core* r4300,
        struct rdp_core* dp,
        struct rsp_core* sp,
        struct ai_controller* ai,
        struct pi_controller* pi,
        struct ri_controller* ri,
        struct si_controller* si,
        struct vi_controller* vi,
        uint32_t* dram,
        size_t dram_size,
        uint8_t* rom,
        size_t rom_size)
{
    connect_rdp(dp, r4300, sp, ri);
    connect_rsp(sp, r4300, dp, ri);
    connect_ai(ai, r4300, ri, vi);
    connect_pi(pi, r4300, ri, rom, rom_size);
    connect_ri(ri, dram, dram_size);
    connect_si(si, r4300, ri);
    connect_vi(vi, r4300);
}

static void dummy_save(void *user_data)
{
}

/*********************************************************************************************************
* emulation thread - runs the core
*/
m64p_error main_run(void)
{
    size_t i;
    unsigned int disable_extra_mem;
    struct eep_file eep;
    struct fla_file fla;
    struct mpk_file mpk;
    struct sra_file sra;
    static int channels[] = { 0, 1, 2, 3 };

    /* take the r4300 emulator mode from the config file at this point and cache it in a global variable */
    r4300emu = ConfigGetParamInt(g_CoreConfig, "R4300Emulator");

    /* set some other core parameters based on the config file values */
    no_compiled_jump = ConfigGetParamBool(g_CoreConfig, "NoCompiledJump");
    g_delay_si = ConfigGetParamBool(g_CoreConfig, "DelaySI");
    disable_extra_mem = ConfigGetParamInt(g_CoreConfig, "DisableExtraMem");
    count_per_op = ConfigGetParamInt(g_CoreConfig, "CountPerOp");
    if (count_per_op <= 0)
        count_per_op = ROM_PARAMS.countperop;

    /* do byte-swapping if it's not been done yet */
    if (g_MemHasBeenBSwapped == 0)
    {
        swap_buffer(g_rom, 4, g_rom_size/4);
        g_MemHasBeenBSwapped = 1;
    }

    connect_all(&g_r4300, &g_dp, &g_sp,
                &g_ai, &g_pi, &g_ri, &g_si, &g_vi,
                g_rdram, (disable_extra_mem == 0) ? 0x800000 : 0x400000,
                g_rom, g_rom_size);

    init_memory();

    // Attach rom to plugins
    printf("Gfx RomOpen.\n");
    if (!gfx.romOpen())
    {
        printf("Gfx RomOpen failed.\n");
        return M64ERR_PLUGIN_FAIL;
    }
    printf("Input RomOpen.\n");
    if (!input.romOpen())
    {
        printf("Input RomOpen failed.\n");
        gfx.romClosed();
        return M64ERR_PLUGIN_FAIL;
    }

    /* connect external time source to AF_RTC component */
    g_si.pif.af_rtc.user_data = NULL;
    g_si.pif.af_rtc.get_time = get_time_using_C_localtime;

    /* connect external game controllers */
    for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
    {
        g_si.pif.controllers[i].user_data = &channels[i];
        g_si.pif.controllers[i].is_connected = egcvip_is_connected;
        g_si.pif.controllers[i].get_input = egcvip_get_input;
    }

    /* connect external rumblepaks */
    for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
    {
        g_si.pif.controllers[i].rumblepak.user_data = &channels[i];
        g_si.pif.controllers[i].rumblepak.rumble = rvip_rumble;
    }

    /* connect saved_memory.mempacks to mempaks */
    for(i = 0; i < GAME_CONTROLLERS_COUNT; ++i)
    {
        g_si.pif.controllers[i].mempak.user_data = NULL;
        g_si.pif.controllers[i].mempak.save = dummy_save;
        g_si.pif.controllers[i].mempak.data = &saved_memory.mempack[i][0];
    }

    /* connect saved_memory.eeprom to eeprom */
    g_si.pif.eeprom.user_data = NULL;
    g_si.pif.eeprom.save = dummy_save;
    g_si.pif.eeprom.data = saved_memory.eeprom;
    if (ROM_SETTINGS.savetype != EEPROM_16KB)
    {
        /* 4kbits EEPROM */
        g_si.pif.eeprom.size = 0x200;
        g_si.pif.eeprom.id = 0x8000;
    }
    else
    {
        /* 16kbits EEPROM */
        g_si.pif.eeprom.size = 0x800;
        g_si.pif.eeprom.id = 0xc000;
    }

    /* connect saved_memory.flashram to flashram */
    g_pi.flashram.user_data = NULL;
    g_pi.flashram.save = dummy_save;
    g_pi.flashram.data = saved_memory.flashram;

    /* connect saved_memory.sram to SRAM */
    g_pi.sram.user_data = NULL;
    g_pi.sram.save = dummy_save;
    g_pi.sram.data = saved_memory.sram;

#ifdef DBG
    if (ConfigGetParamBool(g_CoreConfig, "EnableDebugger"))
        init_debugger();
#endif

    g_EmulatorRunning = 1;
    StateChanged(M64CORE_EMU_STATE, M64EMU_RUNNING);

    /* call r4300 CPU core and run the game */
    r4300_reset_hard();
    r4300_reset_soft();
    r4300_execute();

#ifdef DBG
    if (g_DebuggerActive)
        destroy_debugger();
#endif

    rsp.romClosed();
    input.romClosed();
    gfx.romClosed();

    // clean up
    g_EmulatorRunning = 0;
    StateChanged(M64CORE_EMU_STATE, M64EMU_STOPPED);

    return M64ERR_SUCCESS;
}

void main_stop(void)
{
    /* note: this operation is asynchronous.  It may be called from a thread other than the
       main emulator thread, and may return before the emulator is completely stopped */
    if (!g_EmulatorRunning)
        return;

    DebugMessage(M64MSG_STATUS, "Stopping emulation.");
    stop = 1;
#ifdef DBG
    if(g_DebuggerActive)
    {
        debugger_step();
    }
#endif        
}