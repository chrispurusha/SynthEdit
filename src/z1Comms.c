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

#include <stdint.h>
#include "defs.h"
#include "globalVars.h"
#include "z1Comms.h"

void z1_on_connected(void) {
    LOG_DEBUG("Z1 connected — request initial state here\n");
    atomic_store(&gReDraw, true);
}

void z1_handle_message(const uint8_t * data, uint32_t length) {
    if (length < 2) {
        return;
    }
    LOG_DEBUG("Z1 SysEx rx %u bytes, type=0x%02X\n", (unsigned)length,
              (length > 4) ? data[4] : 0xFF);
    // TODO: decode Korg Z1 SysEx messages
    atomic_store(&gReDraw, true);
}
