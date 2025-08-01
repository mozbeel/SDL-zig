/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#include "SDL_windowsvideo.h"

// GameInput currently has a bug with keys stuck on focus change, and crashes on initialization on some systems, so we'll disable it until these issues are fixed.
#undef HAVE_GAMEINPUT_H

#ifdef HAVE_GAMEINPUT_H

#include "../../core/windows/SDL_gameinput.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/scancodes_windows.h"


#define MAX_GAMEINPUT_BUTTONS   7   // GameInputMouseWheelTiltRight is the highest button

static const Uint8 GAMEINPUT_button_map[MAX_GAMEINPUT_BUTTONS] = {
    SDL_BUTTON_LEFT,
    SDL_BUTTON_RIGHT,
    SDL_BUTTON_MIDDLE,
    SDL_BUTTON_X1,
    SDL_BUTTON_X2,
    6,
    7
};

typedef struct GAMEINPUT_Device
{
    IGameInputDevice *pDevice;
    const GameInputDeviceInfo *info;
    char *name;
    Uint32 instance_id; // generated by SDL
    bool registered;
    bool delete_requested;
    IGameInputReading *last_mouse_reading;
    IGameInputReading *last_keyboard_reading;
} GAMEINPUT_Device;

struct WIN_GameInputData
{
    IGameInput *pGameInput;
    GameInputCallbackToken gameinput_callback_token;
    int num_devices;
    GAMEINPUT_Device **devices;
    GameInputKind enabled_input;
    SDL_Mutex *lock;
    uint64_t timestamp_offset;
};

static bool GAMEINPUT_InternalAddOrFind(WIN_GameInputData *data, IGameInputDevice *pDevice)
{
    GAMEINPUT_Device **devicelist = NULL;
    GAMEINPUT_Device *device = NULL;
    const GameInputDeviceInfo *info;
    bool result = false;

    info = IGameInputDevice_GetDeviceInfo(pDevice);

    SDL_LockMutex(data->lock);
    {
        for (int i = 0; i < data->num_devices; ++i) {
            device = data->devices[i];
            if (device && device->pDevice == pDevice) {
                // we're already added
                device->delete_requested = false;
                result = true;
                goto done;
            }
        }

        device = (GAMEINPUT_Device *)SDL_calloc(1, sizeof(*device));
        if (!device) {
            goto done;
        }

        devicelist = (GAMEINPUT_Device **)SDL_realloc(data->devices, (data->num_devices + 1) * sizeof(*devicelist));
        if (!devicelist) {
            SDL_free(device);
            goto done;
        }

        if (info->deviceStrings) {
            // In theory we could get the manufacturer and product strings here, but they're NULL for all the devices I've tested
        }

        if (info->displayName) {
            // This could give us a product string, but it's NULL for all the devices I've tested
        }

        IGameInputDevice_AddRef(pDevice);
        device->pDevice = pDevice;
        device->instance_id = SDL_GetNextObjectID();
        device->info = info;

        data->devices = devicelist;
        data->devices[data->num_devices++] = device;

        result = true;
    }
done:
    SDL_UnlockMutex(data->lock);

    return result;
}

static bool GAMEINPUT_InternalRemoveByIndex(WIN_GameInputData *data, int idx)
{
    GAMEINPUT_Device **devicelist = NULL;
    GAMEINPUT_Device *device;
    bool result = false;

    SDL_LockMutex(data->lock);
    {
        if (idx < 0 || idx >= data->num_devices) {
            result = SDL_SetError("GAMEINPUT_InternalRemoveByIndex argument idx %d is out of range", idx);
            goto done;
        }

        device = data->devices[idx];
        if (device) {
            if (device->registered) {
                if (device->info->supportedInput & GameInputKindMouse) {
                    SDL_RemoveMouse(device->instance_id, true);
                }
                if (device->info->supportedInput & GameInputKindKeyboard) {
                    SDL_RemoveKeyboard(device->instance_id, true);
                }
                if (device->last_mouse_reading) {
                    IGameInputReading_Release(device->last_mouse_reading);
                    device->last_mouse_reading = NULL;
                }
                if (device->last_keyboard_reading) {
                    IGameInputReading_Release(device->last_keyboard_reading);
                    device->last_keyboard_reading = NULL;
                }
            }
            IGameInputDevice_Release(device->pDevice);
            SDL_free(device->name);
            SDL_free(device);
        }
        data->devices[idx] = NULL;

        if (data->num_devices == 1) {
            // last element in the list, free the entire list then
            SDL_free(data->devices);
            data->devices = NULL;
        } else {
            if (idx != data->num_devices - 1) {
                size_t bytes = sizeof(*devicelist) * (data->num_devices - idx - 1);
                SDL_memmove(&data->devices[idx], &data->devices[idx + 1], bytes);
            }
        }

        // decrement the count and return
        --data->num_devices;
        result = true;
    }
done:
    SDL_UnlockMutex(data->lock);

    return result;
}

static void CALLBACK GAMEINPUT_InternalDeviceCallback(
    _In_ GameInputCallbackToken callbackToken,
    _In_ void* context,
    _In_ IGameInputDevice *pDevice,
    _In_ uint64_t timestamp,
    _In_ GameInputDeviceStatus currentStatus,
    _In_ GameInputDeviceStatus previousStatus)
{
    WIN_GameInputData *data = (WIN_GameInputData *)context;
    int idx = 0;
    GAMEINPUT_Device *device = NULL;

    if (!pDevice) {
        // This should never happen, but ignore it if it does
        return;
    }

    if (currentStatus & GameInputDeviceConnected) {
        GAMEINPUT_InternalAddOrFind(data, pDevice);
    } else {
        for (idx = 0; idx < data->num_devices; ++idx) {
            device = data->devices[idx];
            if (device && device->pDevice == pDevice) {
                // will be deleted on the next Detect call
                device->delete_requested = true;
                break;
            }
        }
    }
}

bool WIN_InitGameInput(SDL_VideoDevice *_this)
{
    WIN_GameInputData *data;
    HRESULT hr;
    bool result = false;

    if (_this->internal->gameinput_context) {
        return true;
    }

    data = (WIN_GameInputData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        goto done;
    }
    _this->internal->gameinput_context = data;

    data->lock = SDL_CreateMutex();
    if (!data->lock) {
        goto done;
    }

    if (!SDL_InitGameInput(&data->pGameInput)) {
        goto done;
    }

    hr = IGameInput_RegisterDeviceCallback(data->pGameInput,
                                           NULL,
                                           (GameInputKindMouse | GameInputKindKeyboard),
                                           GameInputDeviceConnected,
                                           GameInputBlockingEnumeration,
                                           data,
                                           GAMEINPUT_InternalDeviceCallback,
                                           &data->gameinput_callback_token);
    if (FAILED(hr)) {
        SDL_SetError("IGameInput::RegisterDeviceCallback failure with HRESULT of %08X", hr);
        goto done;
    }

    // Calculate the relative offset between SDL timestamps and GameInput timestamps
    Uint64 now = SDL_GetTicksNS();
    uint64_t timestampUS = IGameInput_GetCurrentTimestamp(data->pGameInput);
    data->timestamp_offset = (SDL_NS_TO_US(now) - timestampUS);

    result = true;

done:
    if (!result) {
        WIN_QuitGameInput(_this);
    }
    return result;
}

static void GAMEINPUT_InitialMouseReading(WIN_GameInputData *data, SDL_Window *window, GAMEINPUT_Device *device, IGameInputReading *reading)
{
    GameInputMouseState state;
    if (SUCCEEDED(IGameInputReading_GetMouseState(reading, &state))) {
        Uint64 timestamp = SDL_US_TO_NS(IGameInputReading_GetTimestamp(reading) + data->timestamp_offset);
        SDL_MouseID mouseID = device->instance_id;

        for (int i = 0; i < MAX_GAMEINPUT_BUTTONS; ++i) {
            const GameInputMouseButtons mask = (1 << i);
            bool down = ((state.buttons & mask) != 0);
            SDL_SendMouseButton(timestamp, window, mouseID, GAMEINPUT_button_map[i], down);
        }

        // Invalidate mouse button flags
        window->internal->mouse_button_flags = (WPARAM)-1;
    }
}

static void GAMEINPUT_HandleMouseDelta(WIN_GameInputData *data, SDL_Window *window, GAMEINPUT_Device *device, IGameInputReading *last_reading, IGameInputReading *reading)
{
    GameInputMouseState last;
    GameInputMouseState state;
    if (SUCCEEDED(IGameInputReading_GetMouseState(last_reading, &last)) &&
        SUCCEEDED(IGameInputReading_GetMouseState(reading, &state))) {
        Uint64 timestamp = SDL_US_TO_NS(IGameInputReading_GetTimestamp(reading) + data->timestamp_offset);
        SDL_MouseID mouseID = device->instance_id;

        GameInputMouseState delta;
        delta.buttons = (state.buttons ^ last.buttons);
        delta.positionX = (state.positionX - last.positionX);
        delta.positionY = (state.positionY - last.positionY);
        delta.wheelX = (state.wheelX - last.wheelX);
        delta.wheelY = (state.wheelY - last.wheelY);

        if (delta.positionX || delta.positionY) {
            SDL_SendMouseMotion(timestamp, window, mouseID, true, (float)delta.positionX, (float)delta.positionY);
        }
        if (delta.buttons) {
            for (int i = 0; i < MAX_GAMEINPUT_BUTTONS; ++i) {
                const GameInputMouseButtons mask = (1 << i);
                if (delta.buttons & mask) {
                    bool down = ((state.buttons & mask) != 0);
                    SDL_SendMouseButton(timestamp, window, mouseID, GAMEINPUT_button_map[i], down);
                }
            }

            // Invalidate mouse button flags
            window->internal->mouse_button_flags = (WPARAM)-1;
        }
        if (delta.wheelX || delta.wheelY) {
            float fAmountX = (float)delta.wheelX / WHEEL_DELTA;
            float fAmountY = (float)delta.wheelY / WHEEL_DELTA;
            SDL_SendMouseWheel(timestamp, SDL_GetMouseFocus(), device->instance_id, fAmountX, fAmountY, SDL_MOUSEWHEEL_NORMAL);
        }
    }
}

static SDL_Scancode GetScancodeFromKeyState(const GameInputKeyState *state)
{
    Uint8 index = (Uint8)(state->scanCode & 0xFF);
    if ((state->scanCode & 0xFF00) == 0xE000) {
        index |= 0x80;
    }
    return windows_scancode_table[index];
}

static bool KeysHaveScancode(const GameInputKeyState *keys, uint32_t count, SDL_Scancode scancode)
{
    for (uint32_t i = 0; i < count; ++i) {
        if (GetScancodeFromKeyState(&keys[i]) == scancode) {
            return true;
        }
    }
    return false;
}

static void GAMEINPUT_InitialKeyboardReading(WIN_GameInputData *data, SDL_Window *window, GAMEINPUT_Device *device, IGameInputReading *reading)
{
    Uint64 timestamp = SDL_US_TO_NS(IGameInputReading_GetTimestamp(reading) + data->timestamp_offset);
    SDL_KeyboardID keyboardID = device->instance_id;

    uint32_t max_keys = device->info->keyboardInfo->maxSimultaneousKeys;
    GameInputKeyState *keys = SDL_stack_alloc(GameInputKeyState, max_keys);
    if (!keys) {
        return;
    }

    uint32_t num_keys = IGameInputReading_GetKeyState(reading, max_keys, keys);
    if (!num_keys) {
        // FIXME: We probably need to track key state by keyboardID
        SDL_ResetKeyboard();
        return;
    }

    // Go through and send key up events for any key that's not held down
    int num_scancodes;
    const bool *keyboard_state = SDL_GetKeyboardState(&num_scancodes);
    for (int i = 0; i < num_scancodes; ++i) {
        if (keyboard_state[i] && !KeysHaveScancode(keys, num_keys, (SDL_Scancode)i)) {
            SDL_SendKeyboardKey(timestamp, keyboardID, keys[i].scanCode, (SDL_Scancode)i, false);
        }
    }

    // Go through and send key down events for any key that's held down
    for (uint32_t i = 0; i < num_keys; ++i) {
        SDL_SendKeyboardKey(timestamp, keyboardID, keys[i].scanCode, GetScancodeFromKeyState(&keys[i]), true);
    }
}

#ifdef DEBUG_KEYS
static void DumpKeys(const char *prefix, GameInputKeyState *keys, uint32_t count)
{
    SDL_Log("%s", prefix);
    for (uint32_t i = 0; i < count; ++i) {
        char str[5];
        *SDL_UCS4ToUTF8(keys[i].codePoint, str) = '\0';
        SDL_Log("    Key 0x%.2x (%s)", keys[i].scanCode, str);
    }
}
#endif // DEBUG_KEYS

static void GAMEINPUT_HandleKeyboardDelta(WIN_GameInputData *data, SDL_Window *window, GAMEINPUT_Device *device, IGameInputReading *last_reading, IGameInputReading *reading)
{
    Uint64 timestamp = SDL_US_TO_NS(IGameInputReading_GetTimestamp(reading) + data->timestamp_offset);
    SDL_KeyboardID keyboardID = device->instance_id;

    uint32_t max_keys = device->info->keyboardInfo->maxSimultaneousKeys;
    GameInputKeyState *last = SDL_stack_alloc(GameInputKeyState, max_keys);
    GameInputKeyState *keys = SDL_stack_alloc(GameInputKeyState, max_keys);
    if (!last || !keys) {
        return;
    }

    uint32_t index_last = 0;
    uint32_t index_keys = 0;
    uint32_t num_last = IGameInputReading_GetKeyState(last_reading, max_keys, last);
    uint32_t num_keys = IGameInputReading_GetKeyState(reading, max_keys, keys);
#ifdef DEBUG_KEYS
    SDL_Log("Timestamp: %llu", timestamp);
    DumpKeys("Last keys:", last, num_last);
    DumpKeys("New keys:", keys, num_keys);
#endif
    while (index_last < num_last || index_keys < num_keys) {
        if (index_last < num_last && index_keys < num_keys) {
            if (last[index_last].scanCode == keys[index_keys].scanCode) {
                // No change
                ++index_last;
                ++index_keys;
            } else {
                // This key was released
                SDL_SendKeyboardKey(timestamp, keyboardID, last[index_last].scanCode, GetScancodeFromKeyState(&last[index_last]), false);
                ++index_last;
            }
        } else if (index_last < num_last) {
            // This key was released
            SDL_SendKeyboardKey(timestamp, keyboardID, last[index_last].scanCode, GetScancodeFromKeyState(&last[index_last]), false);
            ++index_last;
        } else {
            // This key was pressed
            SDL_SendKeyboardKey(timestamp, keyboardID, keys[index_keys].scanCode, GetScancodeFromKeyState(&keys[index_keys]), true);
            ++index_keys;
        }
    }
}

void WIN_UpdateGameInput(SDL_VideoDevice *_this)
{
    WIN_GameInputData *data = _this->internal->gameinput_context;

    SDL_LockMutex(data->lock);
    {
        // Key events and relative mouse motion both go to the window with keyboard focus
        SDL_Window *window = SDL_GetKeyboardFocus();

        for (int i = 0; i < data->num_devices; ++i) {
            GAMEINPUT_Device *device = data->devices[i];
            IGameInputReading *reading;

            if (!device->registered) {
                if (device->info->supportedInput & GameInputKindMouse) {
                    SDL_AddMouse(device->instance_id, device->name, true);
                }
                if (device->info->supportedInput & GameInputKindKeyboard) {
                    SDL_AddKeyboard(device->instance_id, device->name, true);
                }
                device->registered = true;
            }

            if (device->delete_requested) {
                GAMEINPUT_InternalRemoveByIndex(data, i--);
                continue;
            }

            if (!(device->info->supportedInput & data->enabled_input)) {
                continue;
            }

            if (!window) {
                continue;
            }

            if (data->enabled_input & GameInputKindMouse) {
                if (device->last_mouse_reading) {
                    HRESULT hr;
                    while (SUCCEEDED(hr = IGameInput_GetNextReading(data->pGameInput, device->last_mouse_reading, GameInputKindMouse, device->pDevice, &reading))) {
                        GAMEINPUT_HandleMouseDelta(data, window, device, device->last_mouse_reading, reading);
                        IGameInputReading_Release(device->last_mouse_reading);
                        device->last_mouse_reading = reading;
                    }
                    if (hr != GAMEINPUT_E_READING_NOT_FOUND) {
                        if (SUCCEEDED(IGameInput_GetCurrentReading(data->pGameInput, GameInputKindMouse, device->pDevice, &reading))) {
                            GAMEINPUT_HandleMouseDelta(data, window, device, device->last_mouse_reading, reading);
                            IGameInputReading_Release(device->last_mouse_reading);
                            device->last_mouse_reading = reading;
                        }
                    }
                } else {
                    if (SUCCEEDED(IGameInput_GetCurrentReading(data->pGameInput, GameInputKindMouse, device->pDevice, &reading))) {
                        GAMEINPUT_InitialMouseReading(data, window, device, reading);
                        device->last_mouse_reading = reading;
                    }
                }
            }

            if (data->enabled_input & GameInputKindKeyboard) {
                if (window->text_input_active) {
                    // Reset raw input while text input is active
                    if (device->last_keyboard_reading) {
                        IGameInputReading_Release(device->last_keyboard_reading);
                        device->last_keyboard_reading = NULL;
                    }
                } else {
                    if (device->last_keyboard_reading) {
                        HRESULT hr;
                        while (SUCCEEDED(hr = IGameInput_GetNextReading(data->pGameInput, device->last_keyboard_reading, GameInputKindKeyboard, device->pDevice, &reading))) {
                            GAMEINPUT_HandleKeyboardDelta(data, window, device, device->last_keyboard_reading, reading);
                            IGameInputReading_Release(device->last_keyboard_reading);
                            device->last_keyboard_reading = reading;
                        }
                        if (hr != GAMEINPUT_E_READING_NOT_FOUND) {
                            if (SUCCEEDED(IGameInput_GetCurrentReading(data->pGameInput, GameInputKindKeyboard, device->pDevice, &reading))) {
                                GAMEINPUT_HandleKeyboardDelta(data, window, device, device->last_keyboard_reading, reading);
                                IGameInputReading_Release(device->last_keyboard_reading);
                                device->last_keyboard_reading = reading;
                            }
                        }
                    } else {
                        if (SUCCEEDED(IGameInput_GetCurrentReading(data->pGameInput, GameInputKindKeyboard, device->pDevice, &reading))) {
                            GAMEINPUT_InitialKeyboardReading(data, window, device, reading);
                            device->last_keyboard_reading = reading;
                        }
                    }
                }
            }
        }
    }
    SDL_UnlockMutex(data->lock);
}

bool WIN_UpdateGameInputEnabled(SDL_VideoDevice *_this)
{
    WIN_GameInputData *data = _this->internal->gameinput_context;
    bool raw_mouse_enabled = _this->internal->raw_mouse_enabled;
    bool raw_keyboard_enabled = _this->internal->raw_keyboard_enabled;

    SDL_LockMutex(data->lock);
    {
        data->enabled_input = (raw_mouse_enabled ? GameInputKindMouse : GameInputKindUnknown) |
                             (raw_keyboard_enabled ? GameInputKindKeyboard : GameInputKindUnknown);

        // Reset input if not enabled
        for (int i = 0; i < data->num_devices; ++i) {
            GAMEINPUT_Device *device = data->devices[i];

            if (device->last_mouse_reading && !raw_mouse_enabled) {
                IGameInputReading_Release(device->last_mouse_reading);
                device->last_mouse_reading = NULL;
            }

            if (device->last_keyboard_reading && !raw_keyboard_enabled) {
                IGameInputReading_Release(device->last_keyboard_reading);
                device->last_keyboard_reading = NULL;
            }
        }
    }
    SDL_UnlockMutex(data->lock);

    return true;
}

void WIN_QuitGameInput(SDL_VideoDevice *_this)
{
    WIN_GameInputData *data = _this->internal->gameinput_context;

    if (!data) {
        return;
    }

    if (data->pGameInput) {
        // free the callback
        if (data->gameinput_callback_token != GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE) {
            IGameInput_UnregisterCallback(data->pGameInput, data->gameinput_callback_token, /*timeoutInUs:*/ 10000);
            data->gameinput_callback_token = GAMEINPUT_INVALID_CALLBACK_TOKEN_VALUE;
        }

        // free the list
        while (data->num_devices > 0) {
            GAMEINPUT_InternalRemoveByIndex(data, 0);
        }

        IGameInput_Release(data->pGameInput);
        data->pGameInput = NULL;
    }

    if (data->pGameInput) {
        SDL_QuitGameInput();
        data->pGameInput = NULL;
    }

    if (data->lock) {
        SDL_DestroyMutex(data->lock);
        data->lock = NULL;
    }

    SDL_free(data);
    _this->internal->gameinput_context = NULL;
}

#else // !HAVE_GAMEINPUT_H

bool WIN_InitGameInput(SDL_VideoDevice* _this)
{
    return SDL_Unsupported();
}

bool WIN_UpdateGameInputEnabled(SDL_VideoDevice *_this)
{
    return SDL_Unsupported();
}

void WIN_UpdateGameInput(SDL_VideoDevice* _this)
{
    return;
}

void WIN_QuitGameInput(SDL_VideoDevice* _this)
{
    return;
}

#endif // HAVE_GAMEINPUT_H
