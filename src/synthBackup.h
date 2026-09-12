/*
 * The SynthEdit application.
 *
 * Copyright (C) 2026 Chris Turner <chris_purusha@icloud.com>
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
// Notes: Docs/code-notes/synthBackup.h.md - "// notes §k" refers there.

// notes §1

#ifndef __SYNTH_BACKUP_H__
#define __SYNTH_BACKUP_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// notes §2
typedef enum {
    eBackupExpectNone = 0,
    eBackupExpectLive,        // live edit buffer — Voyager Panel Dump or Korg Current Program Dump
    eBackupExpectPreset,      // a specific stored preset, by number — Voyager Single Preset Dump only
    eBackupExpectBank,        // every stored preset in one message — Voyager All Presets Dump only
    // notes §3
    eBackupExpectKorgProgram,
} tBackupExpect;

// notes §4
void synth_backup_current_patch(void);

// notes §5
void synth_backup_patch_by_number(uint32_t presetNumber);

// notes §6
void synth_backup_patch_by_number_korg(uint8_t bank, uint32_t prog);

// notes §7
void synth_backup_bank(void);

// notes §8
void synth_backup_bank_to_folder(void);

// notes §9
void synth_backup_flush_bank_to_folder(void);

// notes §10
void synth_backup_flush_korg_name_sweep(void);

// notes §11
bool synth_backup_sweep_request_in_flight(void);

// notes §12
void synth_backup_flush_background_prefetch(void);

// notes §13
void synth_backup_capture_dump(const uint8_t * data, uint32_t length, tBackupExpect kind);

// notes §14

// notes §15
void synth_backup_restore_edit_buffer(void);

// notes §16
void synth_backup_restore_edit_buffer_from_path(const char * path);

// notes §17
void synth_backup_restore_patch(void);

// notes §18
void synth_backup_restore_patch_to_bank(void);

// notes §19
void synth_backup_restore_patch_to_bank_from_path(const char * path, uint8_t bank, uint32_t prog);

// notes §20
void synth_backup_restore_bank(void);

// notes §21
void synth_backup_restore_folder(void);

// notes §22
void synth_backup_flush_restore_folder(void);

// notes §23
void synth_backup_flush_korg_restore_folder(void);

// notes §24

// Which action to take once synth_backup_start_name_sweep() below finishes
// fetching every preset's name and shows the resulting picker.
typedef enum {
    eNameSweepPurposeLoad = 0,
    eNameSweepPurposeStore,
} tNameSweepPurpose;

// notes §25
void synth_backup_start_name_sweep(tNameSweepPurpose purpose);

// notes §26
void synth_backup_note_preset_name(uint32_t presetNumber, const char * name);

// notes §27
void synth_backup_reload_name_cache_for_device(void);

// notes §28
void synth_backup_clear_name_cache_for_device(void);

// notes §29

// notes §30
void synth_store_patch_to_bank(uint8_t bank, uint32_t presetNumber);

// notes §31
void synth_backup_flush_store(void);

// notes §32
void synth_backup_flush_pending_save(void);

// notes §33
bool synth_backup_get_export_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount);
bool synth_backup_get_restore_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount);

// notes §34
bool synth_backup_export_progress_is_name_sweep(void);

#ifdef __cplusplus
}
#endif

#endif // __SYNTH_BACKUP_H__
