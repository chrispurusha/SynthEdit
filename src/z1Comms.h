/*
 * The Z1-Edit application.
 *
 * Copyright (C) 2025 Chris Turner <chris_purusha@icloud.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __Z1_COMMS_H__
#define __Z1_COMMS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called when a Korg Z1 is identified on the MIDI bus
void z1_on_connected(void);

// Dispatch an incoming SysEx message from the Z1
void z1_handle_message(const uint8_t * data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif // __Z1_COMMS_H__
