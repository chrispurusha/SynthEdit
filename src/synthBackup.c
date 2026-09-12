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
// Notes: Docs/code-notes/synthBackup.c.md - "// notes §k" refers there.

#include <ctype.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "defs.h"
#include "synthlibDefs.h"
#include "types.h"
#include "globalVars.h"
#include "panelConfig.h"
#include "synthComms.h"
#include "synthGraphics.h"
#include "fileBrowser.h"
#include "alertDialog.h"
#include "bankBrowser.h"
#include "misc.h"
#include "midiComms.h"
#include "synthBackup.h"
#include "prefs.h"

// notes §1
static _Atomic int       gBackupExpect                     = eBackupExpectNone;

// Valid only while gBackupExpect == eBackupExpectPreset — which preset number
// (1-based) the pending request was for, purely so the save dialog can
// suggest a filename that says so.
static uint32_t          gBackupPresetNum                  = 0;

// notes §2
static uint8_t           gBackupKorgBank                   = 0;
static uint32_t          gBackupKorgProg                   = 0;

// notes §3
static bool              gKorgSweepActive;

// notes §4
static uint8_t *         gPendingBackupData                = NULL;
static uint32_t          gPendingBackupLen                 = 0;

// CoreMIDI-thread-sets/main-thread-opens handoff for the save dialog itself
// — see gPendingBackupData's own comment just above for why this exists.
static _Atomic bool      gPendingBackupSaveReady           = false;
static char              gPendingBackupSaveDefaultName[96] = {0};

// notes §5
static uint32_t          gStoreArmedPresetNumber           = 0;

// notes §6
static _Atomic bool      gStoreReplyReady                  = false;
static uint8_t *         gStoreReplyData                   = NULL;
static uint32_t          gStoreReplyLen                    = 0;
static uint32_t          gStoreReplyPresetNumber           = 0;

// notes §7
static _Atomic bool      gBackupBatchActive                = false;
static _Atomic bool      gBackupBatchReplyReady            = false;
static uint8_t *         gBackupBatchReplyData             = NULL; // valid only while gBackupBatchReplyReady
static uint32_t          gBackupBatchReplyLen              = 0;

static char              gBackupBatchFolder[1024]          = {0};
static uint32_t          gBackupBatchCurrentPreset         = 0; // 1-based; which preset the outstanding request is for
static double            gBackupBatchRequestSinceMs        = 0.0;
static uint32_t          gBackupBatchRepliedCount          = 0;
static uint32_t          gBackupBatchMissingCount          = 0;
static uint32_t          gBackupBatchRetryCount            = 0; // resets to 0 whenever gBackupBatchCurrentPreset genuinely advances — see NAME_SWEEP_MAX_RETRIES above. Used by BOTH modes.
// notes §8
static double            gBackupBatchNextRequestMs         = 0.0;

// notes §9
typedef enum {
    eBatchModeExportFiles = 0,
    eBatchModeNameSweep,
} tBackupBatchMode;

static tBackupBatchMode  gBackupBatchMode                  = eBatchModeExportFiles;
static tNameSweepPurpose gNameSweepPurpose                 = eNameSweepPurposeLoad;

#define BACKUP_BATCH_PRESET_COUNT    128    // matches misc.mm's own "Patch by Number" range / synth_request_single_preset_dump()'s own range check (synthComms.c) — a base Voyager's single bank
// notes §10
#define BACKUP_BATCH_TIMEOUT_MS      1000.0
// notes §11
#define NAME_SWEEP_LABEL_LEN         64

// notes §12
#define NAME_CACHE_JOIN_CHAR         '\x1e'

// notes §13
#define NAME_CACHE_FIELD_CHAR        '\x1f'

// notes §14
#define NAME_SWEEP_PACING_MS         500.0

// notes §15
#define NAME_SWEEP_MAX_RETRIES       2

static char              gNameSweepLabels[BACKUP_BATCH_PRESET_COUNT][NAME_SWEEP_LABEL_LEN];

// notes §16
static uint8_t           gNameSweepCategoryIndex[BACKUP_BATCH_PRESET_COUNT];

// notes §17
static bool              gNameCacheValid = false;

// notes §18
static void backup_sanitize_name_for_file(const char * name, char * out, size_t outSize) {
    out[0] = '\0';

    if ((name == NULL) || (name[0] == '\0')) {
        return;
    }
    char * o = out;

    for (const char * p = name; (*p != '\0') && (o < out + outSize - 1); p++) {
        if (*p == '\n') {
            if ((o == out) || (*(o - 1) != ' ')) {
                *o++ = ' ';
            }
        } else if (*p == '/') {
            *o++ = '-';
        } else {
            *o++ = *p;
        }
    }

    *o     = '\0';
}

// notes §19
static void backup_index_file_path(char * outPath, size_t outPathSize, const char * folder) {
    const char * deviceName = synth_panel_config()->deviceName;
    char         sanitized[64];

    backup_sanitize_name_for_file((deviceName[0] != '\0') ? deviceName : "Device", sanitized, sizeof(sanitized));
    snprintf(outPath, outPathSize, "%s/Patches-%s.txt", folder, (sanitized[0] != '\0') ? sanitized : "Device");
}

// notes §20
static bool warn_if_not_connected(const char * title) {
    if (gDevice.connected) {
        return false;
    }
    LOG_ERROR("%s: no device connected\n", title);
    show_alert(title, "No synth is connected. Use Device > Scan Devices once the "
               "hardware is switched on and its MIDI interface is attached, "
               "then try again.");
    return true;
}

void synth_backup_current_patch(void) {
    if (warn_if_not_connected("Save Patch")) {
        return;
    }
    gBackupExpect = eBackupExpectLive;
    synth_request_state_dump();
    LOG_DEBUG("Backup: requested a fresh state dump to capture\n");
}

// notes §21
static uint8_t  gPendingStoreBank         = 0;
static uint32_t gPendingStorePresetNumber = 0;
// notes §135
static bool     gStoreVerifyCurrentSlot   = false;
static char     gStoreExpectedName[SYNTH_PROG_NAME_MAXLEN];

// notes §22
static void on_store_patch_to_bank_korg_confirmed(bool confirmed) {
    char message[160];

    if (!confirmed) {
        LOG_DEBUG("Store: cancelled at confirmation\n");
        return;
    }
    synth_send_korg_program_write_request(gPendingStoreBank, gPendingStorePresetNumber);
    snprintf(message, sizeof(message), "Sent — Bank %c, Program %u should now match the current edit buffer.",
             gPendingStoreBank ? 'B' : 'A', (unsigned)gPendingStorePresetNumber);
    show_alert("Store Patch to Bank", message);
}

// notes §23
static void arm_moog_store(bool verifyCurrentSlot) {
    gStoreVerifyCurrentSlot = verifyCurrentSlot;
    gStoreArmedPresetNumber = gPendingStorePresetNumber;
    gBackupExpect           = eBackupExpectLive;
    synth_request_state_dump();
    LOG_DEBUG("Store: requested a fresh state dump to store as preset %u\n", (unsigned)gPendingStorePresetNumber);
}

static void on_store_patch_to_bank_moog_confirmed(bool confirmed) {
    if (!confirmed) {
        LOG_DEBUG("Store: cancelled at confirmation\n");
        return;
    }
    arm_moog_store(false);
}

void synth_store_patch_to_bank(uint8_t bank, uint32_t presetNumber) {
    if (!gDevice.connected) {
        LOG_ERROR("Store: no device connected\n");
        return;
    }

    if ((presetNumber < 1) || (presetNumber > 128)) {
        LOG_ERROR("Store: preset number %u out of range (1-128)\n", (unsigned)presetNumber);
        return;
    }
    char message[160];

    gPendingStoreBank         = bank;
    gPendingStorePresetNumber = presetNumber;

    // notes §24
    if (!synth_panel_config()->moogStyleDump) {
        snprintf(message, sizeof(message),
                 "This will overwrite Bank %c, Program %u on the connected device with the CURRENT edit buffer. This cannot be undone.",
                 bank ? 'B' : 'A', (unsigned)presetNumber);
        show_confirm("Store Patch to Bank", message, "Store...", on_store_patch_to_bank_korg_confirmed);
        return;
    }
    snprintf(message, sizeof(message),
             "This will overwrite Preset %u on the connected device with the CURRENT edit buffer. This cannot be undone.",
             (unsigned)presetNumber);
    show_confirm("Store Patch to Bank", message, "Store...", on_store_patch_to_bank_moog_confirmed);
}

#define STORE_CURRENT_TITLE    "Store Patch to Current Slot"

static void on_store_to_current_slot_confirmed(bool confirmed) {
    if (!confirmed) {
        LOG_DEBUG("Store to current slot: cancelled at confirmation\n");
        return;
    }
    arm_moog_store(true);
}

// notes §136
void synth_store_patch_to_current_slot(void) {
    char     message[320];
    uint32_t presetNumber = (uint32_t)(gDevice.currentProgram + 1);

    if (!gDevice.connected) {
        show_alert(STORE_CURRENT_TITLE, "No synth is connected.");
        return;
    }

    if (!synth_panel_config()->moogStyleDump) {
        show_alert(STORE_CURRENT_TITLE, "This device's current bank isn't tracked, so its current slot can't be known for certain. "
                   "Use Store Patch to Bank... to choose the slot.");
        return;
    }

    switch (gDevice.programCertainty) {
        case eProgramConfirmed:
            break;

        case eProgramMatchedByName:
            snprintf(message, sizeof(message), "Preset %u was found by its name alone, which isn't certain enough to overwrite it. "
                     "Step to it with Prev/Next, or select it on the synth, to confirm it - or use Store Patch to Bank... to choose the slot.",
                     (unsigned)presetNumber);
            show_alert(STORE_CURRENT_TITLE, message);
            return;

        case eProgramFromProgramChange:
            snprintf(message, sizeof(message), "Preset %u came from a Program Change, but the synth's patch name hasn't confirmed it - "
                     "the name cache may not hold that preset yet, or another preset has the same name. "
                     "Use Store Patch to Bank... to choose the slot.", (unsigned)presetNumber);
            show_alert(STORE_CURRENT_TITLE, message);
            return;

        default:
            show_alert(STORE_CURRENT_TITLE, "The synth's current preset isn't known. Select one with Prev/Next or on the synth first - "
                       "or use Store Patch to Bank... to choose the slot.");
            return;
    }
    const char * slotName = synth_backup_cached_preset_name(presetNumber);

    gPendingStoreBank         = 0;
    gPendingStorePresetNumber = presetNumber;
    snprintf(gStoreExpectedName, sizeof(gStoreExpectedName), "%s", gDevice.confirmedSlotName);

    if (synth_prog_names_equal(slotName, gStoreExpectedName)) {
        snprintf(message, sizeof(message), "This will overwrite Preset %u (\"%s\") with the current edit buffer. This cannot be undone.",
                 (unsigned)presetNumber, slotName);
    } else {
        snprintf(message, sizeof(message), "This will overwrite Preset %u (\"%s\") with the current edit buffer (\"%s\"). This cannot be undone.",
                 (unsigned)presetNumber, slotName, gStoreExpectedName);
    }
    show_confirm(STORE_CURRENT_TITLE, message, "Store...", on_store_to_current_slot_confirmed);
}

// Checked again on the fresh dump the store is about to write - notes §136.
static bool store_target_still_current(uint32_t presetNumber, const uint8_t * data, uint32_t length) {
    tPanelConfig * cfg                            = synth_panel_config();
    char           name[sizeof(gDevice.progName)] = "";
    char           message[320];

    if (length > 2) {
        synth_decode_moog_name(data + 1, length - 2, cfg->panelNameOffset, cfg->panelNameBitOffset, cfg->panelNameLen, cfg->nameLineWidth, name, sizeof(name));
    }

    if ((gDevice.programCertainty != eProgramConfirmed) || ((uint32_t)(gDevice.currentProgram + 1) != presetNumber)) {
        snprintf(message, sizeof(message), "Nothing was written: Preset %u is no longer confirmed as the synth's current preset.", (unsigned)presetNumber);
    } else if (!synth_prog_names_equal(name, gStoreExpectedName)) {
        snprintf(message, sizeof(message), "Nothing was written: the synth's edit buffer is now \"%s\", not \"%s\" - it may have moved to another preset.",
                 name, gStoreExpectedName);
    } else {
        return true;
    }
    LOG_ERROR("Store to current slot: %s\n", message);
    show_alert(STORE_CURRENT_TITLE, message);
    return false;
}
// notes §25

void synth_backup_patch_by_number(uint32_t presetNumber) {
    if (warn_if_not_connected("Save Patch by Number")) {
        return;
    }

    // notes §26
    if (gBackupBatchActive || (gBackupExpect != eBackupExpectNone)) {
        LOG_ERROR("Backup: another backup/restore operation is already in progress\n");
        return;
    }
    gBackupPresetNum = presetNumber;
    gBackupExpect    = eBackupExpectPreset;
    synth_request_single_preset_dump(presetNumber); // logs its own error and leaves gBackupExpect armed-but-unfulfilled if out of range/wrong device
}

void synth_backup_patch_by_number_korg(uint8_t bank, uint32_t prog) {
    if (warn_if_not_connected("Save Patch by Number")) {
        return;
    }

    // notes §27
    if (gKorgSweepActive || (gBackupExpect != eBackupExpectNone)) {
        LOG_ERROR("Backup: another backup/restore operation is already in progress\n");
        return;
    }
    gBackupKorgBank = bank;
    gBackupKorgProg = prog;
    gBackupExpect   = eBackupExpectKorgProgram;
    synth_request_korg_program_dump(bank, prog); // logs its own error and leaves gBackupExpect armed-but-unfulfilled if out of range/wrong device
}

void synth_backup_bank(void) {
    if (warn_if_not_connected("Backup Bank")) {
        return;
    }
    gBackupExpect = eBackupExpectBank;
    synth_request_all_presets_dump(); // logs its own error and leaves gBackupExpect armed-but-unfulfilled if not Moog-style
}

static double backup_monotonic_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((double)now.tv_sec * 1000.0) + ((double)now.tv_nsec / 1e6);
}

// notes §28
static void backup_batch_append_index_line(uint32_t presetNumber, const char * name) {
    char   indexPath[1280];

    backup_index_file_path(indexPath, sizeof(indexPath), gBackupBatchFolder);
    FILE * f = fopen(indexPath, "a");

    if (f != NULL) {
        fprintf(f, "%03u  %s\n", (unsigned)presetNumber, name);
        fclose(f);
    } else {
        LOG_ERROR("Backup: couldn't append to %s\n", indexPath);
    }
}

// notes §29
static void name_sweep_show_picker(void);

// notes §30
static void name_cache_save_to_disk(void);

// Presets between progressive cache flushes during a name sweep - see the use site for the
// trade-off between write volume and how much work an interrupted sweep loses.
#define NAME_CACHE_SAVE_INTERVAL    (16)

// Both defined alongside name_cache_save_to_disk() below, forward-declared here for the same reason
// it is: the sweep advance and start paths above need them.
static void name_cache_set_complete(bool complete);
static bool name_cache_is_complete(void);

// A slot counts as fetched once it holds anything other than the "---" placeholder or an empty
// string (never written). A timed-out slot keeps its "(no response)" label and counts as fetched on
// purpose - see gNameCacheValid's own comment - so resuming a sweep does not retry it forever.
static bool name_slot_fetched(const char * label) {
    return (label[0] != '\0') && (strcmp(label, "---") != 0);
}

// Same reasoning as name_cache_save_to_disk() above, for the Korg side —
// forward-declared here since korg_sweep_advance() (further down, but still
// before korg_sweep_set_label()'s own definition) needs to reach it too.
static void korg_name_cache_save_to_disk(void);
static void korg_name_cache_set_complete(bool complete);
static bool korg_name_cache_is_complete(void);

// notes §31
static void backup_batch_request_current(void) {
    gBackupExpect              = eBackupExpectPreset;
    synth_request_single_preset_dump(gBackupBatchCurrentPreset);
    gBackupBatchRequestSinceMs = backup_monotonic_ms();
}

// notes §32
static void moog_name_sweep_start(void) {
    name_cache_set_complete(false); // cleared before the first request, set again only on the completion path

    gBackupBatchMode          = eBatchModeNameSweep;
    gBackupBatchActive        = true;
    gBackupBatchCurrentPreset = 1;
    gBackupBatchRetryCount    = 0;
    gBackupBatchNextRequestMs = 0.0;
    gBackupBatchRepliedCount  = 0;
    gBackupBatchMissingCount  = 0;

    // Resume where the last attempt stopped rather than restarting at the first slot. A partial
    // cache (progressive flush, see NAME_CACHE_SAVE_INTERVAL) already holds every slot that attempt
    // got through, and re-requesting those costs NAME_SWEEP_PACING_MS each for names we already have.
    uint32_t resumeFrom = 0;

    while ((resumeFrom < BACKUP_BATCH_PRESET_COUNT) && name_slot_fetched(gNameSweepLabels[resumeFrom])) {
        resumeFrom++;
    }

    if (resumeFrom >= BACKUP_BATCH_PRESET_COUNT) {
        resumeFrom = 0; // every slot filled but the cache was not marked complete - sweep the lot
    }

    // Only the unfetched tail is reset; everything before resumeFrom is cached data we are keeping.
    for (uint32_t i = resumeFrom; i < BACKUP_BATCH_PRESET_COUNT; i++) {
        snprintf(gNameSweepLabels[i], NAME_SWEEP_LABEL_LEN, "---");
        gNameSweepCategoryIndex[i] = 0xFF;
    }

    gBackupBatchCurrentPreset = resumeFrom + 1; // 1-based

    backup_batch_request_current();
    LOG_DEBUG("Load/Store: starting a %u-preset name sweep\n", (unsigned)BACKUP_BATCH_PRESET_COUNT);
}

// notes §33
static void backup_batch_advance(void) {
    // notes §34
    synthlib_request_redraw();
    gBackupBatchCurrentPreset++;
    gBackupBatchRetryCount = 0; // fresh slot, fresh retry budget — see NAME_SWEEP_MAX_RETRIES's own comment

    // notes §35
    if (  (gBackupBatchMode == eBatchModeNameSweep)
       && (gBackupBatchCurrentPreset <= BACKUP_BATCH_PRESET_COUNT)
       && ((gBackupBatchCurrentPreset % NAME_CACHE_SAVE_INTERVAL) == 0)) {
        name_cache_save_to_disk();
    }

    if (gBackupBatchCurrentPreset > BACKUP_BATCH_PRESET_COUNT) {
        gBackupBatchActive = false;

        if (gBackupBatchMode == eBatchModeNameSweep) {
            LOG_DEBUG("Name sweep finished — %u replied, %u missing\n",
                      (unsigned)gBackupBatchRepliedCount, (unsigned)gBackupBatchMissingCount);
            gNameCacheValid = true; // even a slot that timed out keeps its "N: (no response)" label — good enough to skip re-sweeping; a future Load/Store on that slot will just show that placeholder rather than silently retrying
            name_cache_save_to_disk();
            name_cache_set_complete(true);
            // notes §36
            return;
        }
        LOG_DEBUG("Backup: bank-to-folder export finished — %u captured, %u missing, folder %s\n",
                  (unsigned)gBackupBatchRepliedCount, (unsigned)gBackupBatchMissingCount, gBackupBatchFolder);
        // notes §37
        synth_request_state_dump();
        return;
    }

    if (gBackupBatchMode == eBatchModeNameSweep) {
        // notes §38
        gBackupBatchNextRequestMs = backup_monotonic_ms() + NAME_SWEEP_PACING_MS;
        return;
    }
    backup_batch_request_current();
}

// notes §39
static void name_cache_set_label(uint32_t presetNumber, const char * name, uint8_t categoryIndex) {
    if ((presetNumber < 1) || (presetNumber > BACKUP_BATCH_PRESET_COUNT)) {
        return;
    }
    char   cleaned[sizeof(gDevice.progName)];

    strncpy(cleaned, name ? name : "", sizeof(cleaned) - 1);
    cleaned[sizeof(cleaned) - 1]              = '\0';

    // notes §40
    for (char * p = cleaned; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }

    char * label = gNameSweepLabels[presetNumber - 1];

    if (cleaned[0] == '\0') {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "(unnamed)");
    } else {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%s", cleaned);
    }
    gNameSweepCategoryIndex[presetNumber - 1] = categoryIndex;
}

// notes §41
static void name_cache_set_complete(bool complete) {
    char key[80];

    snprintf(key, sizeof(key), "nameCacheMoogDone_%s", synth_panel_config()->deviceName);
    cache_set_string(key, complete ? "1" : "0");
}

// Absent key means a cache written before progressive saving existed - back then a blob was only
// ever written at 100%, so treating it as complete is right and avoids a pointless re-sweep on the
// first run after upgrading.
static bool name_cache_is_complete(void) {
    char key[80];

    snprintf(key, sizeof(key), "nameCacheMoogDone_%s", synth_panel_config()->deviceName);
    return strcmp(cache_get_string(key, "1"), "1") == 0;
}

static void name_cache_save_to_disk(void) {
    char        key[80];

    snprintf(key, sizeof(key), "nameCacheMoog_%s", synth_panel_config()->deviceName);

    static char blob[BACKUP_BATCH_PRESET_COUNT * (NAME_SWEEP_LABEL_LEN + 4) + 1];
    size_t      offset = 0;

    for (uint32_t i = 0; i < BACKUP_BATCH_PRESET_COUNT; i++) {
        size_t len = strlen(gNameSweepLabels[i]);

        memcpy(&blob[offset], gNameSweepLabels[i], len);
        offset        += len;
        blob[offset++] = NAME_CACHE_FIELD_CHAR;
        offset        += (size_t)snprintf(&blob[offset], 3, "%02X", gNameSweepCategoryIndex[i]);
        blob[offset++] = NAME_CACHE_JOIN_CHAR;
    }

    blob[offset] = '\0';

    cache_set_string(key, blob);
}

// notes §42
static void name_cache_load_from_disk(void) {
    char         key[80];

    snprintf(key, sizeof(key), "nameCacheMoog_%s", synth_panel_config()->deviceName);
    const char * blob  = cache_get_string(key, "");

    if (blob[0] == '\0') {
        return;
    }
    uint32_t     index = 0;
    const char * p     = blob;

    while ((*p != '\0') && (index < BACKUP_BATCH_PRESET_COUNT)) {
        const char * recordEnd = strchr(p, NAME_CACHE_JOIN_CHAR);
        size_t       recordLen = recordEnd ? (size_t)(recordEnd - p) : strlen(p);
        const char * fieldSep  = (const char *)memchr(p, NAME_CACHE_FIELD_CHAR, recordLen);
        size_t       len       = fieldSep ? (size_t)(fieldSep - p) : recordLen;

        if (len >= NAME_SWEEP_LABEL_LEN) {
            len = NAME_SWEEP_LABEL_LEN - 1;
        }
        memcpy(gNameSweepLabels[index], p, len);
        gNameSweepLabels[index][len]   = '\0';

        gNameSweepCategoryIndex[index] = 0xFF; // older cache format, or a malformed field — see NAME_CACHE_FIELD_CHAR's own comment

        if (fieldSep != NULL) {
            unsigned int value = 0;

            if (sscanf(fieldSep + 1, "%2x", &value) == 1) {
                gNameSweepCategoryIndex[index] = (uint8_t)value;
            }
        }
        index++;

        if (!recordEnd) {
            break;
        }
        p                              = recordEnd + 1;
    }
    gNameCacheValid = name_cache_is_complete();
}

// notes §43
static void name_cache_clear_disk(void) {
    char key[80];

    snprintf(key, sizeof(key), "nameCacheMoog_%s", synth_panel_config()->deviceName);
    cache_set_string(key, "");
}

// notes §44
static void name_cache_update_from_preset_dump(uint32_t presetNumber, const uint8_t * data, uint32_t length) {
    tPanelConfig *  cfg           = synth_panel_config();
    const uint8_t * payload       = data + 1;                      // skip F0, matches every other Moog dump handler
    uint32_t        payloadLen    = (length > 2) ? length - 2 : 0; // exclude leading skip + trailing F7
    char            name[sizeof(gDevice.progName)];
    char            category[32];
    uint8_t         categoryIndex = 0xFF;

    name[0]     = '\0';
    category[0] = '\0';
    synth_decode_moog_name(payload, payloadLen, cfg->presetNameOffset, cfg->presetNameBitOffset, cfg->presetNameLen, cfg->nameLineWidth, name, sizeof(name));
    synth_decode_moog_category(data, length, category, sizeof(category), &categoryIndex);
    name_cache_set_label(presetNumber, name, categoryIndex);
    name_cache_save_to_disk();
}

// notes §45
static void name_sweep_capture_name(const uint8_t * data, uint32_t length) {
    name_cache_update_from_preset_dump(gBackupBatchCurrentPreset, data, length);
    gBackupBatchRepliedCount++;
}

// Writes the just-captured reply to its own file and appends the index
// line, then advances. Called only from the main/render thread.
static void backup_batch_write_capture(const uint8_t * data, uint32_t length) {
    if (gBackupBatchMode == eBatchModeNameSweep) {
        name_sweep_capture_name(data, length);
        return;
    }
    char   nameForFile[sizeof(gDevice.progName)];

    backup_sanitize_name_for_file(gDevice.progName, nameForFile, sizeof(nameForFile));

    char   filePath[1280];

    if (nameForFile[0] != '\0') {
        snprintf(filePath, sizeof(filePath), "%s/%03u %s.syx", gBackupBatchFolder, (unsigned)gBackupBatchCurrentPreset, nameForFile);
    } else {
        snprintf(filePath, sizeof(filePath), "%s/%03u.syx", gBackupBatchFolder, (unsigned)gBackupBatchCurrentPreset);
    }
    FILE * f = fopen(filePath, "wb");

    if (f != NULL) {
        fwrite(data, 1, length, f);
        fclose(f);
        gBackupBatchRepliedCount++;
        backup_batch_append_index_line(gBackupBatchCurrentPreset, (nameForFile[0] != '\0') ? nameForFile : "(unnamed)");
    } else {
        LOG_ERROR("Backup: couldn't open %s for writing\n", filePath);
        gBackupBatchMissingCount++;
        backup_batch_append_index_line(gBackupBatchCurrentPreset, "(write failed)");
    }
}

// Runs on the main thread once the user has chosen (or cancelled) a backup
// folder — see synth_backup_bank_to_folder() below for where the picker is
// opened.
static void backup_batch_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Backup: bank-to-folder export cancelled\n");
        return;
    }
    strncpy(gBackupBatchFolder, path, sizeof(gBackupBatchFolder) - 1);
    gBackupBatchFolder[sizeof(gBackupBatchFolder) - 1] = '\0';
    set_last_backup_folder(synth_current_device_config(), gBackupBatchFolder); // so a later single-file Backup save defaults here too — see its own comment (misc.h)

    // notes §46
    char   indexPath[1280];
    backup_index_file_path(indexPath, sizeof(indexPath), gBackupBatchFolder);
    FILE * f = fopen(indexPath, "w");

    if (f != NULL) {
        const char * deviceName = synth_panel_config()->deviceName;
        time_t       now        = time(NULL);
        char         timestamp[32];

        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", localtime(&now));
        fprintf(f, "%s Bank Backup — %s\n", (deviceName[0] != '\0') ? deviceName : "Patch", timestamp);
        fprintf(f, "%u presets requested from device\n\n", (unsigned)BACKUP_BATCH_PRESET_COUNT);
        fclose(f);
    }
    gBackupBatchActive                                 = true;
    gBackupBatchCurrentPreset                          = 1;
    gBackupBatchRetryCount                             = 0;
    gBackupBatchNextRequestMs                          = 0.0;
    gBackupBatchRepliedCount                           = 0;
    gBackupBatchMissingCount                           = 0;
    backup_batch_request_current();
    LOG_DEBUG("Backup: starting bank-to-folder export of %u presets to %s\n",
              (unsigned)BACKUP_BATCH_PRESET_COUNT, gBackupBatchFolder);
}

// notes §47

void synth_backup_note_preset_name(uint32_t presetNumber, const char * name) {
    name_cache_set_label(presetNumber, name, 0xFF); // no category on hand at this call site — see synth_backup_note_preset_name()'s own comment (synthBackup.h)
}

static bool name_cache_label_is_a_name(const char * label) {
    return (label[0] != '\0') && (strcmp(label, "(unnamed)") != 0) && (strcmp(label, "(no response)") != 0);
}

uint32_t synth_backup_unique_preset_named(const char * name) {
    uint32_t found = 0;

    if (!gNameCacheValid) {
        return 0;
    }

    for (uint32_t i = 0; i < BACKUP_BATCH_PRESET_COUNT; i++) {
        if (name_cache_label_is_a_name(gNameSweepLabels[i]) && synth_prog_names_equal(gNameSweepLabels[i], name)) {
            if (found != 0) {
                return 0;
            }
            found = i + 1;
        }
    }

    return found;
}

bool synth_backup_cached_name_is(uint32_t presetNumber, const char * name) {
    const char * label = synth_backup_cached_preset_name(presetNumber);

    return name_cache_label_is_a_name(label) && synth_prog_names_equal(label, name);
}

const char * synth_backup_cached_preset_name(uint32_t presetNumber) {
    if ((presetNumber < 1) || (presetNumber > BACKUP_BATCH_PRESET_COUNT)) {
        return "";
    }
    return gNameSweepLabels[presetNumber - 1];
}

// notes §48
#define KORG_SWEEP_PRESET_COUNT    256 // 2 banks x 128 — index i is bank (i/128, 0=A/1=B), program ((i%128)+1)

// notes §49
typedef enum {
    eKorgSweepModeNameSweep = 0,
    eKorgSweepModeExportFiles,
} tKorgSweepMode;

static tKorgSweepMode gKorgSweepMode                 = eKorgSweepModeNameSweep;
static char           gKorgSweepFolder[1024]         = {0}; // only meaningful while gKorgSweepMode == eKorgSweepModeExportFiles

static bool           gKorgSweepActive               = false;
static uint32_t       gKorgSweepIndex                = 0; // 0-based, 0..(KORG_SWEEP_PRESET_COUNT-1)
static uint32_t       gKorgSweepRetryCount           = 0; // resets to 0 whenever gKorgSweepIndex genuinely advances — see NAME_SWEEP_MAX_RETRIES above
static double         gKorgSweepRequestSinceMs       = 0.0;
// notes §50
static double         gKorgSweepNextRequestMs        = 0.0;
static uint32_t       gKorgSweepRepliedCount         = 0;
static uint32_t       gKorgSweepMissingCount         = 0;
static bool           gKorgNameCacheValid            = false;
static char           gKorgSweepLabels[KORG_SWEEP_PRESET_COUNT][NAME_SWEEP_LABEL_LEN];

// Korg counterpart to gNameSweepCategoryIndex above — see its own comment.
static uint8_t        gKorgSweepCategoryIndex[KORG_SWEEP_PRESET_COUNT];

// notes §51
static bool           gKorgRestoreFolderActive       = false;
static char           gKorgRestoreFolderFolder[1024] = {0};
static uint32_t       gKorgRestoreFolderEntries[KORG_SWEEP_PRESET_COUNT]; // sweep-style indices (0..255, bank*128+prog-1), in Patches.txt order, filtered to ones a matching file was actually found for
static uint32_t       gKorgRestoreFolderCount        = 0;
static uint32_t       gKorgRestoreFolderIndex        = 0;
static double         gKorgRestoreFolderNextSendMs   = 0.0;
static uint32_t       gKorgRestoreFolderSentCount    = 0;
static uint32_t       gKorgRestoreFolderMissingCount = 0;

// notes §52
bool synth_backup_sweep_request_in_flight(void) {
    if (gKorgSweepActive) {
        return gKorgSweepNextRequestMs == 0.0;
    }

    if (gBackupBatchActive && (gBackupBatchMode == eBatchModeNameSweep)) {
        return gBackupBatchNextRequestMs == 0.0;
    }
    return false;
}

// CoreMIDI-thread-copies/main-thread-decodes handoff — same reasoning and
// shape as gBackupBatchReplyReady/Data/Len above (name decoding isn't safe
// to do off the main thread; see that block's own comment).
static _Atomic bool   gKorgSweepReplyReady           = false;
static uint8_t *      gKorgSweepReplyData            = NULL;
static uint32_t       gKorgSweepReplyLen             = 0;

static void korg_sweep_request_current(void) {
    uint8_t  bank = (uint8_t)(gKorgSweepIndex / 128);
    uint32_t prog = (gKorgSweepIndex % 128) + 1;

    gBackupExpect            = eBackupExpectKorgProgram;
    synth_request_korg_program_dump(bank, prog);
    gKorgSweepRequestSinceMs = backup_monotonic_ms();
}

// notes §53
static void korg_sweep_advance(void) {
    synthlib_request_redraw();
    gKorgSweepIndex++;
    gKorgSweepRetryCount = 0;    // fresh slot, fresh retry budget — see NAME_SWEEP_MAX_RETRIES's own comment

    // Same progressive flush as the Moog path — see NAME_CACHE_SAVE_INTERVAL's own comment for why
    // it is batched rather than per-reply. Export mode is excluded for the same reason there: it
    // writes its own files and does not want the extra cache writes.
    if (  (gKorgSweepMode == eKorgSweepModeNameSweep)
       && (gKorgSweepIndex < KORG_SWEEP_PRESET_COUNT)
       && ((gKorgSweepIndex % NAME_CACHE_SAVE_INTERVAL) == 0)) {
        korg_name_cache_save_to_disk();
    }

    if (gKorgSweepIndex >= KORG_SWEEP_PRESET_COUNT) {
        gKorgSweepActive    = false;

        if (gKorgSweepMode == eKorgSweepModeExportFiles) {
            LOG_DEBUG("Backup: Korg bank-to-folder export finished — %u captured, %u missing, folder %s\n",
                      (unsigned)gKorgSweepRepliedCount, (unsigned)gKorgSweepMissingCount, gKorgSweepFolder);
        } else {
            LOG_DEBUG("Korg name sweep finished — %u replied, %u missing\n",
                      (unsigned)gKorgSweepRepliedCount, (unsigned)gKorgSweepMissingCount);
        }
        // notes §54
        gKorgNameCacheValid = true;
        korg_name_cache_save_to_disk();
        korg_name_cache_set_complete(true);
        return;
    }
    // Paced, not immediate — the actual send happens on a later
    // synth_backup_flush_korg_name_sweep() tick once this elapses.
    gKorgSweepNextRequestMs = backup_monotonic_ms() + NAME_SWEEP_PACING_MS;
}

// notes §55
static void korg_sweep_append_index_line(uint8_t bank, uint32_t prog, const char * name) {
    char   indexPath[1280];

    backup_index_file_path(indexPath, sizeof(indexPath), gKorgSweepFolder);
    FILE * f = fopen(indexPath, "a");

    if (f != NULL) {
        fprintf(f, "%c%03u  %s\n", bank ? 'B' : 'A', (unsigned)prog, name);
        fclose(f);
    } else {
        LOG_ERROR("Backup: couldn't append to %s\n", indexPath);
    }
}

// notes §56
static void korg_sweep_write_capture_file(uint8_t bank, uint32_t prog, const char * name, const uint8_t * data, uint32_t length) {
    char   nameForFile[sizeof(gDevice.progName)];

    backup_sanitize_name_for_file(name, nameForFile, sizeof(nameForFile));

    char   filePath[1280];

    if (nameForFile[0] != '\0') {
        snprintf(filePath, sizeof(filePath), "%s/%c%03u %s.syx", gKorgSweepFolder, bank ? 'B' : 'A', (unsigned)prog, nameForFile);
    } else {
        snprintf(filePath, sizeof(filePath), "%s/%c%03u.syx", gKorgSweepFolder, bank ? 'B' : 'A', (unsigned)prog);
    }
    FILE * f = fopen(filePath, "wb");

    if (f != NULL) {
        fwrite(data, 1, length, f);
        fclose(f);
        gKorgSweepRepliedCount++;
        korg_sweep_append_index_line(bank, prog, nameForFile[0] != '\0' ? nameForFile : "(unnamed)");
    } else {
        LOG_ERROR("Backup: couldn't open %s for writing\n", filePath);
        gKorgSweepMissingCount++;
        korg_sweep_append_index_line(bank, prog, "(write failed)");
    }
}

// notes §57
static void korg_sweep_set_label(uint8_t bank, uint32_t prog, const char * name, uint8_t categoryIndex) {
    if ((prog < 1) || (prog > 128)) {
        return;
    }
    char * label = gKorgSweepLabels[(bank ? 128 : 0) + (prog - 1)];

    if ((name == NULL) || (name[0] == '\0')) {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "(unnamed)");
    } else {
        snprintf(label, NAME_SWEEP_LABEL_LEN, "%s", name);
    }
    gKorgSweepCategoryIndex[(bank ? 128 : 0) + (prog - 1)] = categoryIndex;
}

// notes §58
static void korg_name_cache_set_complete(bool complete) {
    char key[80];

    snprintf(key, sizeof(key), "nameCacheKorgDone_%s", synth_panel_config()->deviceName);
    cache_set_string(key, complete ? "1" : "0");
}

// Absent key means a pre-progressive-save cache, which was only ever written at 100%.
static bool korg_name_cache_is_complete(void) {
    char key[80];

    snprintf(key, sizeof(key), "nameCacheKorgDone_%s", synth_panel_config()->deviceName);
    return strcmp(cache_get_string(key, "1"), "1") == 0;
}

static void korg_name_cache_save_to_disk(void) {
    char        key[80];

    snprintf(key, sizeof(key), "nameCacheKorg_%s", synth_panel_config()->deviceName);

    static char blob[KORG_SWEEP_PRESET_COUNT * (NAME_SWEEP_LABEL_LEN + 4) + 1];
    size_t      offset = 0;

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        size_t len = strlen(gKorgSweepLabels[i]);

        memcpy(&blob[offset], gKorgSweepLabels[i], len);
        offset        += len;
        blob[offset++] = NAME_CACHE_FIELD_CHAR;
        offset        += (size_t)snprintf(&blob[offset], 3, "%02X", gKorgSweepCategoryIndex[i]);
        blob[offset++] = NAME_CACHE_JOIN_CHAR;
    }

    blob[offset] = '\0';

    cache_set_string(key, blob);
}

// Korg counterpart to name_cache_load_from_disk() above.
static void korg_name_cache_load_from_disk(void) {
    char         key[80];

    snprintf(key, sizeof(key), "nameCacheKorg_%s", synth_panel_config()->deviceName);
    const char * blob  = cache_get_string(key, "");

    if (blob[0] == '\0') {
        return;
    }
    uint32_t     index = 0;
    const char * p     = blob;

    while ((*p != '\0') && (index < KORG_SWEEP_PRESET_COUNT)) {
        const char * recordEnd = strchr(p, NAME_CACHE_JOIN_CHAR);
        size_t       recordLen = recordEnd ? (size_t)(recordEnd - p) : strlen(p);
        const char * fieldSep  = (const char *)memchr(p, NAME_CACHE_FIELD_CHAR, recordLen);
        size_t       len       = fieldSep ? (size_t)(fieldSep - p) : recordLen;

        if (len >= NAME_SWEEP_LABEL_LEN) {
            len = NAME_SWEEP_LABEL_LEN - 1;
        }
        memcpy(gKorgSweepLabels[index], p, len);
        gKorgSweepLabels[index][len]   = '\0';

        gKorgSweepCategoryIndex[index] = 0xFF; // older cache format, or a malformed field — see NAME_CACHE_FIELD_CHAR's own comment

        if (fieldSep != NULL) {
            unsigned int value = 0;

            if (sscanf(fieldSep + 1, "%2x", &value) == 1) {
                gKorgSweepCategoryIndex[index] = (uint8_t)value;
            }
        }
        index++;

        if (!recordEnd) {
            break;
        }
        p                              = recordEnd + 1;
    }
    gKorgNameCacheValid = korg_name_cache_is_complete();
}

// Korg counterpart to name_cache_clear_disk() above.
static void korg_name_cache_clear_disk(void) {
    char key[80];

    snprintf(key, sizeof(key), "nameCacheKorg_%s", synth_panel_config()->deviceName);
    cache_set_string(key, "");
}

// notes §59
void synth_backup_reload_name_cache_for_device(void) {
    gNameCacheValid     = false;
    gKorgNameCacheValid = false;
    memset(gNameSweepLabels, 0, sizeof(gNameSweepLabels));
    memset(gKorgSweepLabels, 0, sizeof(gKorgSweepLabels));
    memset(gNameSweepCategoryIndex, 0xFF, sizeof(gNameSweepCategoryIndex));
    memset(gKorgSweepCategoryIndex, 0xFF, sizeof(gKorgSweepCategoryIndex));

    name_cache_load_from_disk();
    korg_name_cache_load_from_disk();
}

// notes §60
void synth_backup_clear_name_cache_for_device(void) {
    gNameCacheValid     = false;
    gKorgNameCacheValid = false;
    memset(gNameSweepLabels, 0, sizeof(gNameSweepLabels));
    memset(gKorgSweepLabels, 0, sizeof(gKorgSweepLabels));
    memset(gNameSweepCategoryIndex, 0xFF, sizeof(gNameSweepCategoryIndex));
    memset(gKorgSweepCategoryIndex, 0xFF, sizeof(gKorgSweepCategoryIndex));

    name_cache_clear_disk();
    korg_name_cache_clear_disk();
    synthlib_request_redraw();
}

// notes §61
static void korg_name_cache_update_from_dump(uint8_t bank, uint32_t prog, const uint8_t * data, uint32_t length) {
    char    name[sizeof(gDevice.progName)];
    char    category[32];
    uint8_t categoryIndex = 0xFF;

    name[0]     = '\0';
    category[0] = '\0';
    synth_decode_korg_name(data, length, name, sizeof(name));
    synth_decode_korg_category(data, length, category, sizeof(category), &categoryIndex);

    for (char * p = name; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = ' ';
        }
    }

    korg_sweep_set_label(bank, prog, name, categoryIndex);
    korg_name_cache_save_to_disk();
}

// notes §62
static void korg_sweep_capture_reply(const uint8_t * data, uint32_t length) {
    uint8_t  bank          = (uint8_t)(gKorgSweepIndex / 128);
    uint32_t prog          = (gKorgSweepIndex % 128) + 1;
    char     name[sizeof(gDevice.progName)];
    char     category[32];
    uint8_t  categoryIndex = 0xFF;

    name[0]     = '\0';
    category[0] = '\0';
    synth_decode_korg_name(data, length, name, sizeof(name));
    synth_decode_korg_category(data, length, category, sizeof(category), &categoryIndex);

    for (char * p = name; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = ' '; // same single-line dropdown constraint name_cache_set_label() already handles for Moog
        }
    }

    korg_sweep_set_label(bank, prog, name, categoryIndex);

    if (gKorgSweepMode == eKorgSweepModeExportFiles) {
        korg_sweep_write_capture_file(bank, prog, name, data, length); // owns its own Replied/Missing counting
    } else {
        gKorgSweepRepliedCount++;
    }
}

// notes §63
void synth_backup_flush_korg_name_sweep(void) {
    if (!gKorgSweepActive) {
        return;
    }

    // Paced gap between requests (see NAME_SWEEP_PACING_MS) — nothing is in
    // flight yet, so skip the reply/timeout checks below until it elapses.
    if (gKorgSweepNextRequestMs > 0.0) {
        if (backup_monotonic_ms() < gKorgSweepNextRequestMs) {
            return;
        }
        gKorgSweepNextRequestMs = 0.0;
        korg_sweep_request_current();
        return;
    }

    if (gKorgSweepReplyReady) {
        gKorgSweepReplyReady = false;
        uint8_t * data   = gKorgSweepReplyData;
        uint32_t  length = gKorgSweepReplyLen;

        gKorgSweepReplyData  = NULL;
        gKorgSweepReplyLen   = 0;
        korg_sweep_capture_reply(data, length);
        free(data);
        korg_sweep_advance();
        return;
    }

    if ((backup_monotonic_ms() - gKorgSweepRequestSinceMs) >= BACKUP_BATCH_TIMEOUT_MS) {
        uint8_t  bank = (uint8_t)(gKorgSweepIndex / 128);
        uint32_t prog = (gKorgSweepIndex % 128) + 1;

        // notes §64
        if (gKorgSweepRetryCount < NAME_SWEEP_MAX_RETRIES) {
            gKorgSweepRetryCount++;
            LOG_DEBUG("Korg name sweep: %c%u timed out after %.0fms, retrying (%u/%u)\n",
                      bank ? 'B' : 'A', (unsigned)prog, BACKUP_BATCH_TIMEOUT_MS,
                      (unsigned)gKorgSweepRetryCount, (unsigned)NAME_SWEEP_MAX_RETRIES);
            korg_sweep_request_current();
            return;
        }
        LOG_DEBUG("Korg name sweep: %c%u timed out after %u attempt(s)\n",
                  bank ? 'B' : 'A', (unsigned)prog, (unsigned)(NAME_SWEEP_MAX_RETRIES + 1));
        snprintf(gKorgSweepLabels[gKorgSweepIndex], NAME_SWEEP_LABEL_LEN, "(no response)");
        gKorgSweepCategoryIndex[gKorgSweepIndex] = 0xFF;
        gKorgSweepMissingCount++;
        korg_sweep_advance();
    }
}

// notes §65
static void korg_sweep_start(void) {
    korg_name_cache_set_complete(false);               // cleared up front, set again only on the completion path below

    gKorgSweepMode          = eKorgSweepModeNameSweep; // always the name-only entry point — korg_batch_folder_chosen() below sets up export mode itself, without going through here
    gKorgSweepActive        = true;
    gKorgSweepIndex         = 0;
    gKorgSweepRetryCount    = 0;
    gKorgSweepRepliedCount  = 0;
    gKorgSweepMissingCount  = 0;
    gKorgSweepNextRequestMs = 0.0;

    // Resume where the last attempt stopped rather than restarting at the first slot. A partial
    // cache (progressive flush, see NAME_CACHE_SAVE_INTERVAL) already holds every slot that attempt
    // got through, and re-requesting those costs NAME_SWEEP_PACING_MS each for names we already have.
    uint32_t resumeFrom = 0;

    while ((resumeFrom < KORG_SWEEP_PRESET_COUNT) && name_slot_fetched(gKorgSweepLabels[resumeFrom])) {
        resumeFrom++;
    }

    if (resumeFrom >= KORG_SWEEP_PRESET_COUNT) {
        resumeFrom = 0; // every slot filled but the cache was not marked complete - sweep the lot
    }

    // Only the unfetched tail is reset; everything before resumeFrom is cached data we are keeping.
    for (uint32_t i = resumeFrom; i < KORG_SWEEP_PRESET_COUNT; i++) {
        snprintf(gKorgSweepLabels[i], NAME_SWEEP_LABEL_LEN, "---");
        gKorgSweepCategoryIndex[i] = 0xFF;
    }

    gKorgSweepIndex = resumeFrom; // 0-based

    korg_sweep_request_current(); // first request goes out right away; every one after this is paced (korg_sweep_advance())
    LOG_DEBUG("Load/Store: starting a %u-program Korg name sweep (2 banks x 128), paced %.0fms/request\n",
              (unsigned)KORG_SWEEP_PRESET_COUNT, NAME_SWEEP_PACING_MS);
}

// notes §66
static void korg_batch_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Backup: Korg bank-to-folder export cancelled\n");
        return;
    }
    strncpy(gKorgSweepFolder, path, sizeof(gKorgSweepFolder) - 1);
    gKorgSweepFolder[sizeof(gKorgSweepFolder) - 1] = '\0';
    set_last_backup_folder(synth_current_device_config(), gKorgSweepFolder);

    // Fresh index file each run — see backup_batch_folder_chosen()'s own
    // identical comment for why (truncates any previous export into the
    // same folder rather than appending onto stale content).
    char   indexPath[1280];
    backup_index_file_path(indexPath, sizeof(indexPath), gKorgSweepFolder);
    FILE * f = fopen(indexPath, "w");

    if (f != NULL) {
        const char * deviceName = synth_panel_config()->deviceName;
        time_t       now        = time(NULL);
        char         timestamp[32];

        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", localtime(&now));
        fprintf(f, "%s Bank Backup — %s\n", (deviceName[0] != '\0') ? deviceName : "Patch", timestamp);
        fprintf(f, "%u programs requested from device (2 banks x 128)\n\n", (unsigned)KORG_SWEEP_PRESET_COUNT);
        fclose(f);
    }
    gKorgSweepMode                                 = eKorgSweepModeExportFiles;
    gKorgSweepActive                               = true;
    gKorgSweepIndex                                = 0;
    gKorgSweepRetryCount                           = 0;
    gKorgSweepRepliedCount                         = 0;
    gKorgSweepMissingCount                         = 0;
    gKorgSweepNextRequestMs                        = 0.0;

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        snprintf(gKorgSweepLabels[i], NAME_SWEEP_LABEL_LEN, "---");
        gKorgSweepCategoryIndex[i] = 0xFF;
    }

    korg_sweep_request_current();
    LOG_DEBUG("Backup: starting Korg bank-to-folder export of %u programs to %s\n",
              (unsigned)KORG_SWEEP_PRESET_COUNT, gKorgSweepFolder);
}

void synth_backup_bank_to_folder(void) {
    if (warn_if_not_connected("Backup Bank to Folder")) {
        return;
    }

    // notes §67
    if (!synth_panel_config()->moogStyleDump) {
        if (gKorgSweepActive) {
            LOG_ERROR("Backup: a Korg sweep is already in progress\n");
            return;
        }

        if (gBackupExpect != eBackupExpectNone) {
            LOG_ERROR("Backup: another backup operation is already in progress\n");
            return;
        }
        open_file_browser_folder(korg_batch_folder_chosen, "Choose Backup Folder");
        return;
    }

    if (gBackupBatchActive) {
        LOG_ERROR("Backup: a bank-to-folder export is already in progress\n");
        return;
    }

    if (gBackupExpect != eBackupExpectNone) {
        LOG_ERROR("Backup: another backup operation is already in progress\n");
        return;
    }
    open_file_browser_folder(backup_batch_folder_chosen, "Choose Backup Folder");
}

// notes §68
static void on_korg_sweep_picked(bool confirmed, uint32_t bank1Indexed, uint32_t location1Indexed) {
    if (!confirmed) {
        LOG_DEBUG("Load/Store: picker cancelled\n");
        return;
    }
    uint8_t  bank = (uint8_t)(bank1Indexed - 1);
    uint32_t prog = location1Indexed;

    if (gNameSweepPurpose == eNameSweepPurposeLoad) {
        synth_load_patch_from_bank(bank, prog);
    } else {
        synth_store_patch_to_bank(bank, prog);
    }
}

// notes §69
static void korg_sweep_show_picker(void) {
    tBankBrowserItem items[KORG_SWEEP_PRESET_COUNT];

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        items[i].name             = gKorgSweepLabels[i];
        items[i].category         = gKorgSweepCategoryIndex[i];
        items[i].bank1Indexed     = (i / 128) + 1;
        items[i].location1Indexed = (i % 128) + 1;
    }

    tPanelDial *     categoryDial      = find_panel_dial_by_label(synth_panel_config(), "Category");
    const char *     categoryNames[PANEL_MAX_NAMES];
    uint32_t         categoryNameCount = 0;

    if (categoryDial != NULL) {
        categoryNameCount = categoryDial->nameCount;

        for (uint32_t i = 0; i < categoryNameCount; i++) {
            categoryNames[i] = categoryDial->names[i];
        }
    }
    const char *     title             = (gNameSweepPurpose == eNameSweepPurposeLoad) ? "Load Patch from Bank" : "Store Patch to Bank";
    const char *     message           = (gNameSweepPurpose == eNameSweepPurposeLoad)
                ? "Choose a program to load into the live edit buffer:"
                : "Choose a program to store the current edit buffer to:";

    open_bank_browser(title, message, "Next...", items, KORG_SWEEP_PRESET_COUNT,
                      (categoryNameCount > 0) ? categoryNames : NULL, categoryNameCount, on_korg_sweep_picked);
}

void synth_backup_start_name_sweep(tNameSweepPurpose purpose) {
    if (warn_if_not_connected("Patch Names")) {
        return;
    }
    gNameSweepPurpose = purpose;

    // Korg-style (Z1): fully separate sweep/picker — see this whole
    // block's own header comment for why. Added 2026-07-14.
    if (!synth_panel_config()->moogStyleDump) {
        // notes §70
        if (  gBackupBatchActive || gKorgRestoreFolderActive
           || (gKorgSweepActive && (gKorgSweepMode == eKorgSweepModeExportFiles))
           || ((gBackupExpect != eBackupExpectNone) && (gBackupExpect != eBackupExpectKorgProgram))) {
            LOG_ERROR("Load/Store: another backup operation is already in progress\n");
            return;
        }

        if (!gKorgSweepActive && !gKorgNameCacheValid) {
            // notes §71
            korg_sweep_start();
        }
        korg_sweep_show_picker();
        return;
    }
    // notes §72
    bool ownSweepInFlight = gBackupBatchActive && (gBackupBatchMode == eBatchModeNameSweep);

    // notes §73
    if (!ownSweepInFlight && (gBackupBatchActive || (gBackupExpect != eBackupExpectNone))) {
        LOG_ERROR("Load/Store: another backup operation is already in progress\n");
        return;
    }

    if (!ownSweepInFlight && !gNameCacheValid) {
        // notes §74
        moog_name_sweep_start();
    } else {
        // notes §75
        LOG_DEBUG("Load/Store: %s preset names (purpose=%d)\n",
                  ownSweepInFlight ? "reusing in-progress" : "using cached", (int)purpose);
    }
    name_sweep_show_picker();
}

// notes §76
static void on_name_sweep_picked(bool confirmed, uint32_t bank1Indexed, uint32_t location1Indexed) {
    (void)bank1Indexed; // always 1 — see this callback's own comment

    if (!confirmed) {
        LOG_DEBUG("Load/Store: picker cancelled\n");
        return;
    }

    if (gNameSweepPurpose == eNameSweepPurposeLoad) {
        synth_load_patch_from_bank(0, location1Indexed); // bank ignored for Moog-style — see synth_load_patch_from_bank()'s own comment (synthComms.h)
    } else {
        synth_store_patch_to_bank(0, location1Indexed);  // bank ignored for Moog-style — see synth_store_patch_to_bank()'s own comment (synthBackup.h)
    }
}

// notes §77
static void name_sweep_show_picker(void) {
    tBankBrowserItem items[BACKUP_BATCH_PRESET_COUNT];

    for (uint32_t i = 0; i < BACKUP_BATCH_PRESET_COUNT; i++) {
        items[i].name             = gNameSweepLabels[i];
        items[i].category         = gNameSweepCategoryIndex[i];
        items[i].bank1Indexed     = 1; // single implicit bank — Moog-style has no bank concept
        items[i].location1Indexed = i + 1;
    }

    tPanelDial *     categoryDial      = find_panel_dial_by_label(synth_panel_config(), "Category");
    const char *     categoryNames[PANEL_MAX_NAMES];
    uint32_t         categoryNameCount = 0;

    if (categoryDial != NULL) {
        categoryNameCount = categoryDial->nameCount;

        for (uint32_t i = 0; i < categoryNameCount; i++) {
            categoryNames[i] = categoryDial->names[i];
        }
    }
    const char *     title             = (gNameSweepPurpose == eNameSweepPurposeLoad) ? "Load Patch from Bank" : "Store Patch to Bank";
    const char *     message           = (gNameSweepPurpose == eNameSweepPurposeLoad)
                ? "Choose a preset to load into the live edit buffer:"
                : "Choose a preset to store the current edit buffer to:";

    open_bank_browser(title, message, "Next...", items, BACKUP_BATCH_PRESET_COUNT,
                      (categoryNameCount > 0) ? categoryNames : NULL, categoryNameCount, on_name_sweep_picked);
}

void synth_backup_flush_bank_to_folder(void) {
    if (!gBackupBatchActive) {
        return;
    }

    // notes §78
    if (gBackupBatchNextRequestMs > 0.0) {
        if (backup_monotonic_ms() < gBackupBatchNextRequestMs) {
            return;
        }
        gBackupBatchNextRequestMs = 0.0;
        backup_batch_request_current();
        return;
    }

    if (gBackupBatchReplyReady) {
        gBackupBatchReplyReady = false; // clear before using — see the batch state block's own comment on why this is safe without a lock
        backup_batch_write_capture(gBackupBatchReplyData, gBackupBatchReplyLen);
        free(gBackupBatchReplyData);
        gBackupBatchReplyData  = NULL;
        gBackupBatchReplyLen   = 0;
        backup_batch_advance();
        return;
    }

    if ((backup_monotonic_ms() - gBackupBatchRequestSinceMs) >= BACKUP_BATCH_TIMEOUT_MS) {
        // notes §79
        if (gBackupBatchRetryCount < NAME_SWEEP_MAX_RETRIES) {
            gBackupBatchRetryCount++;
            LOG_DEBUG("Backup: preset %u timed out after %ums, retrying (%u/%u)\n",
                      (unsigned)gBackupBatchCurrentPreset, (unsigned)BACKUP_BATCH_TIMEOUT_MS,
                      (unsigned)gBackupBatchRetryCount, (unsigned)NAME_SWEEP_MAX_RETRIES);
            backup_batch_request_current();
            return;
        }
        // notes §80
        LOG_ERROR("Backup: preset %u did not reply after %u attempt(s), skipping\n",
                  (unsigned)gBackupBatchCurrentPreset, (unsigned)(NAME_SWEEP_MAX_RETRIES + 1));
        gBackupBatchMissingCount++;

        if (gBackupBatchMode == eBatchModeNameSweep) {
            snprintf(gNameSweepLabels[gBackupBatchCurrentPreset - 1], NAME_SWEEP_LABEL_LEN, "(no response)");
            gNameSweepCategoryIndex[gBackupBatchCurrentPreset - 1] = 0xFF;
        } else {
            backup_batch_append_index_line(gBackupBatchCurrentPreset, "(no response)");
        }
        gBackupExpect = eBackupExpectNone;
        backup_batch_advance();
    }
}

// Runs on the main thread once the user has chosen (or cancelled) a save
// location — see synth_backup_capture_dump() below for where gPendingBackup*
// gets set just before this dialog is opened.
static void backup_save_callback(const char * path) {
    if (path != NULL) {
        FILE * f = fopen(path, "wb");

        if (f != NULL) {
            fwrite(gPendingBackupData, 1, gPendingBackupLen, f);
            fclose(f);
            LOG_DEBUG("Backup: wrote %u bytes to %s\n", (unsigned)gPendingBackupLen, path);

            // notes §81
            const char * lastSlash = strrchr(path, '/');

            if (lastSlash != NULL) {
                char   folder[1024];
                size_t len = (size_t)(lastSlash - path);

                if (len >= sizeof(folder)) {
                    len = sizeof(folder) - 1;
                }
                memcpy(folder, path, len);
                folder[len] = '\0';
                set_last_backup_folder(synth_current_device_config(), folder);
            }
        } else {
            LOG_ERROR("Backup: couldn't open %s for writing\n", path);
        }
    } else {
        LOG_DEBUG("Backup: save dialog cancelled\n");
    }
    free(gPendingBackupData);
    gPendingBackupData = NULL;
    gPendingBackupLen  = 0;
}

void synth_backup_capture_dump(const uint8_t * data, uint32_t length, tBackupExpect kind) {
    if (gBackupExpect != kind) {
        return;
    }
    gBackupExpect = eBackupExpectNone;

    if ((kind == eBackupExpectLive) && (gStoreArmedPresetNumber != 0)) {
        // notes §82
        uint8_t * storeCopy = (uint8_t *)malloc(length);

        if (storeCopy == NULL) {
            LOG_ERROR("Store: out of memory copying %u byte dump\n", (unsigned)length);
            gStoreArmedPresetNumber = 0;
            return;
        }
        memcpy(storeCopy, data, length);
        gStoreReplyData         = storeCopy;
        gStoreReplyLen          = length;
        gStoreReplyPresetNumber = gStoreArmedPresetNumber;
        gStoreArmedPresetNumber = 0;
        gStoreReplyReady        = true;
        return;
    }

    if ((kind == eBackupExpectPreset) && gBackupBatchActive) {
        // notes §83
        if (!synth_moog_single_preset_dump_intact(data, length)) {
            LOG_DEBUG("Backup: preset %u reply looked corrupt (dropped MIDI byte?), discarding — will retry on timeout\n",
                      (unsigned)gBackupBatchCurrentPreset);
            return;
        }
        // notes §84
        uint8_t * batchCopy = (uint8_t *)malloc(length);

        if (batchCopy == NULL) {
            LOG_ERROR("Backup: out of memory copying %u byte dump (bank-to-folder)\n", (unsigned)length);
            return;
        }
        memcpy(batchCopy, data, length);
        gBackupBatchReplyData  = batchCopy;
        gBackupBatchReplyLen   = length;
        gBackupBatchReplyReady = true;
        return;
    }

    if ((kind == eBackupExpectKorgProgram) && gKorgSweepActive) {
        // notes §85
        if (!synth_korg_program_dump_intact(data, length)) {
            LOG_DEBUG("Load/Store: Korg sweep reply looked corrupt, discarding — will retry on timeout\n");
            return;
        }
        // notes §86
        uint8_t * korgCopy = (uint8_t *)malloc(length);

        if (korgCopy == NULL) {
            LOG_ERROR("Backup: out of memory copying %u byte dump (Korg name sweep)\n", (unsigned)length);
            return;
        }
        memcpy(korgCopy, data, length);
        gKorgSweepReplyData  = korgCopy;
        gKorgSweepReplyLen   = length;
        gKorgSweepReplyReady = true;
        return;
    }
    uint8_t *    copy       = (uint8_t *)malloc(length);

    if (copy == NULL) {
        LOG_ERROR("Backup: out of memory copying %u byte dump\n", (unsigned)length);
        return;
    }
    memcpy(copy, data, length);
    gPendingBackupData = copy;
    gPendingBackupLen  = length;

    const char * deviceName = synth_panel_config()->deviceName;
    char         defaultName[96];

    // notes §87
    if (kind == eBackupExpectBank) {
        snprintf(defaultName, sizeof(defaultName), "%s Bank.syx", (deviceName[0] != '\0') ? deviceName : "patch");
    } else {
        char nameForFile[sizeof(gDevice.progName)];

        backup_sanitize_name_for_file(gDevice.progName, nameForFile, sizeof(nameForFile));

        if (nameForFile[0] != '\0') {
            snprintf(defaultName, sizeof(defaultName), "%s.syx", nameForFile);
        } else if (kind == eBackupExpectPreset) {
            snprintf(defaultName, sizeof(defaultName), "%s Preset %u.syx",
                     (deviceName[0] != '\0') ? deviceName : "patch", (unsigned)gBackupPresetNum);
        } else if (kind == eBackupExpectKorgProgram) {
            // notes §88
            char korgName[sizeof(gDevice.progName)];

            korgName[0] = '\0';
            synth_decode_korg_name(gPendingBackupData, gPendingBackupLen, korgName, sizeof(korgName));
            char korgNameForFile[sizeof(gDevice.progName)];

            backup_sanitize_name_for_file(korgName, korgNameForFile, sizeof(korgNameForFile));

            if (korgNameForFile[0] != '\0') {
                snprintf(defaultName, sizeof(defaultName), "%s.syx", korgNameForFile);
            } else {
                snprintf(defaultName, sizeof(defaultName), "%s %c%03u.syx",
                         (deviceName[0] != '\0') ? deviceName : "patch", gBackupKorgBank ? 'B' : 'A', (unsigned)gBackupKorgProg);
            }
        } else {
            snprintf(defaultName, sizeof(defaultName), "%s.syx", (deviceName[0] != '\0') ? deviceName : "patch");
        }
    }
    LOG_DEBUG("Backup: captured %u byte dump, opening save dialog\n", (unsigned)length);
    // notes §89
    strncpy(gPendingBackupSaveDefaultName, defaultName, sizeof(gPendingBackupSaveDefaultName) - 1);
    gPendingBackupSaveDefaultName[sizeof(gPendingBackupSaveDefaultName) - 1] = '\0';
    gPendingBackupSaveReady                                                  = true;
}

// notes §90
void synth_backup_flush_pending_save(void) {
    if (!gPendingBackupSaveReady) {
        return;
    }
    gPendingBackupSaveReady = false;
    open_file_browser_write(backup_save_callback, gPendingBackupSaveDefaultName);
}

// ── Restore ───────────────────────────────────────────────────────────────────
// See synthBackup.h's own comment on this whole section for the mechanism
// and its 2026-07-11 hardware confirmation.

// Reads an entire file into a malloc'd buffer. Returns NULL (and logs) on
// any failure; caller owns the returned buffer. *outLen receives its size.
static uint8_t * restore_read_file(const char * path, uint32_t * outLen) {
    FILE *    f       = fopen(path, "rb");

    if (f == NULL) {
        LOG_ERROR("Restore: couldn't open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long      size    = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        LOG_ERROR("Restore: %s is empty or unreadable\n", path);
        fclose(f);
        return NULL;
    }
    uint8_t * data    = (uint8_t *)malloc((size_t)size);

    if (data == NULL) {
        LOG_ERROR("Restore: out of memory reading %s (%ld bytes)\n", path, size);
        fclose(f);
        return NULL;
    }
    size_t    readLen = fread(data, 1, (size_t)size, f);

    fclose(f);

    if (readLen != (size_t)size) {
        LOG_ERROR("Restore: short read on %s\n", path);
        free(data);
        return NULL;
    }
    *outLen = (uint32_t)size;
    return data;
}

// notes §91
static bool restore_validate_moog_dump(const uint8_t * data, uint32_t length, uint8_t expectedMode, const char * what,
                                       char * reason, size_t reasonSize) {
    tPanelConfig * cfg = synth_panel_config();

    if (!cfg->moogStyleDump) {
        snprintf(reason, reasonSize, "The connected device isn't Moog-style — this Restore action doesn't support it yet.");
        LOG_ERROR("Restore: connected device isn't Moog-style\n");
        return false;
    }

    if ((length < 6) || (data[0] != MIDI_SYSEX_START) || (data[length - 1] != MIDI_SYSEX_END)) {
        snprintf(reason, reasonSize, "This file doesn't look like a raw SysEx capture (%u bytes) — was it saved by this app's own Backup, unmodified?", (unsigned)length);
        LOG_ERROR("Restore: %s doesn't look like a raw SysEx capture (%u bytes)\n", what, (unsigned)length);
        return false;
    }

    if ((data[1] != cfg->manufacturerId[0]) || (data[2] != cfg->productId)) {
        snprintf(reason, reasonSize, "This file isn't a dump for the connected device (manufacturer/product ID mismatch).");
        LOG_ERROR("Restore: %s isn't a dump for the connected device (mfrId/productId mismatch)\n", what);
        return false;
    }

    if (data[4] != expectedMode) {
        snprintf(reason, reasonSize,
                 "This file is a %s, not a %s — pick a file saved with the matching Backup action.",
                 (data[4] == 0x01) ? "whole Bank dump" : (data[4] == 0x02) ? "Edit Buffer dump" : (data[4] == 0x03) ? "Patch by Number dump" : "dump of an unrecognized type",
                 what);
        LOG_ERROR("Restore: %s is mode 0x%02X, expected 0x%02X\n", what, (unsigned)data[4], (unsigned)expectedMode);
        return false;
    }
    return true;
}

// notes §92
static bool restore_validate_korg_dump(const uint8_t * data, uint32_t length, char * reason, size_t reasonSize,
                                       uint8_t * outBank, uint32_t * outProg) {
    tPanelConfig * cfg     = synth_panel_config();

    if (cfg->moogStyleDump) {
        snprintf(reason, reasonSize, "The connected device isn't Korg-style — this Restore action doesn't support it.");
        LOG_ERROR("Restore: connected device isn't Korg-style\n");
        return false;
    }
    uint32_t       n       = cfg->manufacturerIdLen;
    uint32_t       funcPos = 3 + n; // F0 + mfrId(n) + (0x30|channel) + familyId, THEN func — see is_synth_sysex()'s own comment (synthComms.c)

    if ((length < funcPos + 4) || (data[0] != MIDI_SYSEX_START) || (data[length - 1] != MIDI_SYSEX_END)) {
        snprintf(reason, reasonSize, "This file doesn't look like a raw SysEx capture (%u bytes) — was it saved by this app's own Backup, unmodified?", (unsigned)length);
        LOG_ERROR("Restore: Patch by Number dump doesn't look like a raw SysEx capture (%u bytes)\n", (unsigned)length);
        return false;
    }

    if ((memcmp(&data[1], cfg->manufacturerId, n) != 0) || ((data[n + 1] & 0xF0) != 0x30) || (data[n + 2] != cfg->familyId)) {
        snprintf(reason, reasonSize, "This file isn't a dump for the connected device (manufacturer/family ID mismatch).");
        LOG_ERROR("Restore: Patch by Number dump isn't for the connected device (mfrId/familyId mismatch)\n");
        return false;
    }

    if (data[funcPos] != SYNTH_FUNC_PROG_DUMP) {
        snprintf(reason, reasonSize, "This file is a func 0x%02X message, not a Program Data Dump — pick a file saved with Backup > Bank (Individual Files).", (unsigned)data[funcPos]);
        LOG_ERROR("Restore: Patch by Number dump func 0x%02X, expected 0x%02X\n", (unsigned)data[funcPos], (unsigned)SYNTH_FUNC_PROG_DUMP);
        return false;
    }

    if (outBank != NULL) {
        *outBank = data[funcPos + 1] & 0x01;
    }

    if (outProg != NULL) {
        *outProg = (uint32_t)data[funcPos + 2] + 1;
    }
    return true;
}

// notes §93
static uint8_t * convert_preset_dump_to_panel_dump(const uint8_t * src, uint32_t srcLength, uint32_t * outLen) {
    if (srcLength < 7) { // F0 mfrId productId deviceId mode presetNum ...data... F7, minimum shape
        return NULL;
    }
    uint32_t  dstLength = srcLength - 1;
    uint8_t * dst       = (uint8_t *)malloc(dstLength);

    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, 4);                          // F0 mfrId productId deviceId
    dst[4]  = 0x02;                               // mode: Panel Dump (was 0x03, Single Preset Dump)
    memcpy(&dst[5], &src[6], srcLength - 6);      // everything after the removed preset-number byte, including the trailing F7
    *outLen = dstLength;
    return dst;
}

// notes §94
static uint8_t * convert_panel_dump_to_preset_dump(const uint8_t * src, uint32_t srcLength, uint8_t presetNumber0based, uint32_t * outLen) {
    if (srcLength < 6) { // F0 mfrId productId deviceId mode ...data... F7, minimum shape
        return NULL;
    }
    uint32_t  dstLength = srcLength + 1;
    uint8_t * dst       = (uint8_t *)malloc(dstLength);

    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, 4);                          // F0 mfrId productId deviceId
    dst[4]  = 0x03;                               // mode: Single Preset Dump (was 0x02, Panel Dump)
    dst[5]  = presetNumber0based;                 // inserted preset-number byte
    memcpy(&dst[6], &src[5], srcLength - 5);      // everything after the mode byte, including the trailing F7
    *outLen = dstLength;
    return dst;
}

// notes §95
void synth_backup_flush_store(void) {
    if (!gStoreReplyReady) {
        return;
    }
    gStoreReplyReady        = false;

    uint8_t * data         = gStoreReplyData;
    uint32_t  length       = gStoreReplyLen;
    uint32_t  presetNumber = gStoreReplyPresetNumber;
    bool      verifySlot   = gStoreVerifyCurrentSlot;

    gStoreReplyData         = NULL;
    gStoreReplyLen          = 0;
    gStoreVerifyCurrentSlot = false;

    if (verifySlot && !store_target_still_current(presetNumber, data, length)) {
        free(data);
        return;
    }
    uint32_t  convertedLen = 0;
    uint8_t * converted    = convert_panel_dump_to_preset_dump(data, length, (uint8_t)(presetNumber - 1), &convertedLen);

    free(data);

    if (converted == NULL) {
        LOG_ERROR("Store: failed to convert %u byte Panel Dump for preset %u\n", (unsigned)length, (unsigned)presetNumber);
        show_alert("Store Patch Failed", "Couldn't prepare the current edit buffer for sending — see the debug log.");
        return;
    }
    char      message[160];

    if (midi_send(converted, convertedLen)) {
        LOG_DEBUG("Store: sent %u byte Single Preset Dump (stored to preset %u)\n",
                  (unsigned)convertedLen, (unsigned)presetNumber);
        // Keeps the name cache (gNameCacheValid) accurate for this slot
        // without needing a full re-sweep — see that flag's own comment.
        name_cache_update_from_preset_dump(presetNumber, converted, convertedLen);
        snprintf(message, sizeof(message), "Sent — Preset %u should now match the current edit buffer.", (unsigned)presetNumber);
        show_alert("Store Patch to Bank", message);
    } else {
        LOG_ERROR("Store: failed to send %u byte Single Preset Dump for preset %u\n",
                  (unsigned)convertedLen, (unsigned)presetNumber);
        show_alert("Store Patch Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(converted);
}

// notes §96
static void restore_edit_buffer_korg_file(const uint8_t * data, uint32_t length, const char * path, char * reason, size_t reasonSize) {
    if (!restore_validate_korg_dump(data, length, reason, reasonSize, NULL, NULL)) {
        show_alert("Restore Edit Buffer Failed", reason);
        return;
    }
    static uint8_t decoded[4096];
    uint32_t       decodedLen = 0;

    if (!synth_decode_korg_prog_dump(data, length, decoded, sizeof(decoded), &decodedLen)) {
        snprintf(reason, reasonSize, "Couldn't decode this file's Program Data Dump payload.");
        show_alert("Restore Edit Buffer Failed", reason);
        return;
    }
    tPanelConfig * cfg        = synth_panel_config();

    if (decodedLen < cfg->progNameLen) {
        snprintf(reason, reasonSize, "This file's decoded payload (%u bytes) is too short for this device's own program name field.", (unsigned)decodedLen);
        show_alert("Restore Edit Buffer Failed", reason);
        return;
    }
    // notes §97
    uint32_t       n          = cfg->manufacturerIdLen;
    uint32_t       funcPos    = 3 + n;
    uint32_t       rawStart   = funcPos + 4;
    uint32_t       rawLen     = length - rawStart - 1;   // exclude trailing F7

    // notes §98
    gKorgSweepActive = false;

    synth_send_korg_current_program_dump(data + rawStart, rawLen);

    // notes §99
    synth_apply_korg_prog_dump_locally(decoded, decodedLen);
    synthlib_request_redraw();

    // notes §100
    if (!synth_dump_patch_in_flight()) {
        synth_request_state_dump();
    }
    LOG_DEBUG("Restore: Korg-style edit buffer restore from %s sent as one Current Program Data Dump (%u byte payload)\n", path, (unsigned)rawLen);
    show_alert("Restore Edit Buffer", "Sent — the connected device's live edit buffer should now match this file.");
}

// notes §101
static void restore_edit_buffer_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: edit buffer restore cancelled\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    if (!synth_panel_config()->moogStyleDump) {
        restore_edit_buffer_korg_file(data, length, path, reason, sizeof(reason));
        free(data);
        return;
    }

    // notes §102
    if ((length >= 5) && (data[0] == MIDI_SYSEX_START) && (data[4] == 0x03)) {
        uint32_t  convertedLen = 0;
        uint8_t * converted    = convert_preset_dump_to_panel_dump(data, length, &convertedLen);

        if (converted != NULL) {
            free(data);
            data   = converted;
            length = convertedLen;
            LOG_DEBUG("Restore: converted a Single Preset Dump (%s) to a Panel Dump for loading\n", path);
        }
    }

    if (!restore_validate_moog_dump(data, length, 0x02, "Panel Dump", reason, sizeof(reason))) {
        show_alert("Restore Edit Buffer Failed", reason);
        free(data);
        return;
    }

    if (midi_send(data, length)) {
        LOG_DEBUG("Restore: sent %u byte Panel Dump from %s (loads live edit buffer only)\n", (unsigned)length, path);

        // notes §103
        synth_apply_moog_panel_dump_locally(data, length);
        synthlib_request_redraw();
        show_alert("Restore Edit Buffer", "Sent — the connected device's live edit buffer should now match this file.");
    } else {
        LOG_ERROR("Restore: failed to send %u byte Panel Dump from %s\n", (unsigned)length, path);
        show_alert("Restore Edit Buffer Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(data);
}

void synth_backup_restore_edit_buffer(void) {
    if (warn_if_not_connected("Open File")) {
        return;
    }
    open_file_browser_read(restore_edit_buffer_file_chosen);
}

void synth_backup_restore_edit_buffer_from_path(const char * path) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }
    restore_edit_buffer_file_chosen(path);
}

// notes §104
static uint8_t * gPendingRestorePatchData         = NULL;
static uint32_t  gPendingRestorePatchLen          = 0;
static uint32_t  gPendingRestorePatchPresetNumber = 0;
static char      gPendingRestorePatchPath[1024]   = {0};

static void on_restore_patch_confirmed(bool confirmed) {
    char message[160];

    if (!confirmed) {
        LOG_DEBUG("Restore: patch restore cancelled at confirmation\n");
        free(gPendingRestorePatchData);
        gPendingRestorePatchData = NULL;
        return;
    }

    if (midi_send(gPendingRestorePatchData, gPendingRestorePatchLen)) {
        LOG_DEBUG("Restore: sent %u byte Single Preset Dump from %s (overwrote preset %u)\n",
                  (unsigned)gPendingRestorePatchLen, gPendingRestorePatchPath, (unsigned)gPendingRestorePatchPresetNumber);
        // Keeps the name cache (gNameCacheValid) accurate for this slot
        // without needing a full re-sweep — see that flag's own comment.
        name_cache_update_from_preset_dump(gPendingRestorePatchPresetNumber, gPendingRestorePatchData, gPendingRestorePatchLen);
        snprintf(message, sizeof(message), "Sent — Preset %u should now match this file.", (unsigned)gPendingRestorePatchPresetNumber);
        show_alert("Restore Patch", message);
    } else {
        LOG_ERROR("Restore: failed to send %u byte Single Preset Dump from %s\n", (unsigned)gPendingRestorePatchLen, gPendingRestorePatchPath);
        show_alert("Restore Patch Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(gPendingRestorePatchData);
    gPendingRestorePatchData = NULL;
}

// Runs on the main thread once the user has chosen (or cancelled) a Single
// Preset Dump file to restore — see synth_backup_restore_patch() below.
static void restore_patch_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: patch restore cancelled\n");
        return;
    }
    uint32_t  length       = 0;
    uint8_t * data         = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    if (!restore_validate_moog_dump(data, length, 0x03, "Patch by Number dump", reason, sizeof(reason))) {
        show_alert("Restore Patch Failed", reason);
        free(data);
        return;
    }
    // notes §105
    uint32_t  presetNumber = (uint32_t)data[5] + 1;
    char      message[160];

    snprintf(message, sizeof(message),
             "This will overwrite Preset %u on the connected device with the contents of this file. This cannot be undone.",
             (unsigned)presetNumber);

    gPendingRestorePatchData                                       = data;
    gPendingRestorePatchLen                                        = length;
    gPendingRestorePatchPresetNumber                               = presetNumber;
    strncpy(gPendingRestorePatchPath, path, sizeof(gPendingRestorePatchPath) - 1);
    gPendingRestorePatchPath[sizeof(gPendingRestorePatchPath) - 1] = '\0';
    show_confirm("Restore Patch", message, "Restore...", on_restore_patch_confirmed);
}

// notes §106
static uint8_t * gPendingKorgRestorePatchData       = NULL;
static uint32_t  gPendingKorgRestorePatchLen        = 0;
static uint8_t   gPendingKorgRestorePatchBank       = 0;
static uint32_t  gPendingKorgRestorePatchProg       = 0;
static char      gPendingKorgRestorePatchPath[1024] = {0};

static void on_korg_restore_patch_confirmed(bool confirmed) {
    char message[192];

    if (!confirmed) {
        LOG_DEBUG("Restore: patch restore cancelled at confirmation\n");
        free(gPendingKorgRestorePatchData);
        gPendingKorgRestorePatchData = NULL;
        return;
    }

    if (midi_send(gPendingKorgRestorePatchData, gPendingKorgRestorePatchLen)) {
        LOG_DEBUG("Restore: sent %u byte Program Data Dump from %s (overwrote Bank %c, Program %u)\n",
                  (unsigned)gPendingKorgRestorePatchLen, gPendingKorgRestorePatchPath,
                  gPendingKorgRestorePatchBank ? 'B' : 'A', (unsigned)gPendingKorgRestorePatchProg);
        // Keeps the Korg name cache (gKorgNameCacheValid) accurate for
        // this slot without needing a full re-sweep — see
        // korg_name_cache_update_from_dump()'s own comment.
        korg_name_cache_update_from_dump(gPendingKorgRestorePatchBank, gPendingKorgRestorePatchProg, gPendingKorgRestorePatchData, gPendingKorgRestorePatchLen);
        snprintf(message, sizeof(message), "Sent — Bank %c, Program %u should now match this file.",
                 gPendingKorgRestorePatchBank ? 'B' : 'A', (unsigned)gPendingKorgRestorePatchProg);
        show_alert("Restore Patch", message);
    } else {
        LOG_ERROR("Restore: failed to send %u byte Program Data Dump from %s\n", (unsigned)gPendingKorgRestorePatchLen, gPendingKorgRestorePatchPath);
        show_alert("Restore Patch Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(gPendingKorgRestorePatchData);
    gPendingKorgRestorePatchData = NULL;
}

static void korg_restore_patch_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: patch restore cancelled\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];
    uint8_t   bank   = 0;
    uint32_t  prog   = 0;

    if (!restore_validate_korg_dump(data, length, reason, sizeof(reason), &bank, &prog)) {
        show_alert("Restore Patch Failed", reason);
        free(data);
        return;
    }
    char      message[192];

    snprintf(message, sizeof(message),
             "This will overwrite Bank %c, Program %u on the connected device with the contents of this file. This cannot be undone.",
             bank ? 'B' : 'A', (unsigned)prog);

    gPendingKorgRestorePatchData                                           = data;
    gPendingKorgRestorePatchLen                                            = length;
    gPendingKorgRestorePatchBank                                           = bank;
    gPendingKorgRestorePatchProg                                           = prog;
    strncpy(gPendingKorgRestorePatchPath, path, sizeof(gPendingKorgRestorePatchPath) - 1);
    gPendingKorgRestorePatchPath[sizeof(gPendingKorgRestorePatchPath) - 1] = '\0';
    show_confirm("Restore Patch", message, "Restore...", on_korg_restore_patch_confirmed);
}

void synth_backup_restore_patch(void) {
    if (warn_if_not_connected("Load Patch by Number")) {
        return;
    }

    if (synth_panel_config()->moogStyleDump) {
        open_file_browser_read(restore_patch_file_chosen);
    } else {
        open_file_browser_read(korg_restore_patch_file_chosen);
    }
}

// notes §107
static void korg_restore_patch_to_bank_send(const uint8_t * data, uint32_t length, uint8_t bank, uint32_t prog) {
    tPanelConfig * cfg      = synth_panel_config();
    uint32_t       n        = cfg->manufacturerIdLen;
    uint32_t       funcPos  = 3 + n;
    uint32_t       rawStart = funcPos + 4;
    uint32_t       rawLen   = length - rawStart - 1;

    synth_send_korg_program_data_dump(bank, prog, data + rawStart, rawLen);
    // notes §108
    korg_name_cache_update_from_dump(bank, prog, data, length);
}

// notes §109
static uint8_t * gPendingRestoreToBankData = NULL;
static uint32_t  gPendingRestoreToBankLen  = 0;
static uint8_t   gPendingRestoreToBankBank = 0;
static uint32_t  gPendingRestoreToBankProg = 0;

// Confirmed-callback for the final "are you sure" — sends and reports the
// result, then frees the file data regardless of outcome.
static void on_restore_patch_to_bank_confirmed(bool confirmed) {
    char message[192];

    if (!confirmed) {
        LOG_DEBUG("Restore: patch-to-bank restore cancelled at confirmation\n");
        free(gPendingRestoreToBankData);
        gPendingRestoreToBankData = NULL;
        return;
    }
    korg_restore_patch_to_bank_send(gPendingRestoreToBankData, gPendingRestoreToBankLen, gPendingRestoreToBankBank, gPendingRestoreToBankProg);
    snprintf(message, sizeof(message), "Sent — Bank %c, Program %u should now match this file.",
             gPendingRestoreToBankBank ? 'B' : 'A', (unsigned)gPendingRestoreToBankProg);
    show_alert("Restore Patch to Bank", message);
    free(gPendingRestoreToBankData);
    gPendingRestoreToBankData = NULL;
}

// Confirmed-callback for the destination-slot bank browser below — stashes
// the chosen bank/program and moves on to the overwrite-warning confirm.
static void on_restore_patch_to_bank_target_chosen(bool confirmed, uint32_t bank1Indexed, uint32_t location1Indexed) {
    char message[192];

    if (!confirmed) {
        LOG_DEBUG("Restore: patch-to-bank picker cancelled\n");
        free(gPendingRestoreToBankData);
        gPendingRestoreToBankData = NULL;
        return;
    }
    gPendingRestoreToBankBank = (uint8_t)(bank1Indexed - 1);
    gPendingRestoreToBankProg = location1Indexed;
    snprintf(message, sizeof(message),
             "This will overwrite Bank %c, Program %u on the connected device with the contents of this file. This cannot be undone.",
             gPendingRestoreToBankBank ? 'B' : 'A', (unsigned)gPendingRestoreToBankProg);
    show_confirm("Restore Patch to Bank", message, "Restore...", on_restore_patch_to_bank_confirmed);
}

static void korg_restore_patch_to_bank_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: patch-to-bank restore cancelled\n");
        return;
    }
    uint32_t         length = 0;
    uint8_t *        data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char             reason[192];

    if (!restore_validate_korg_dump(data, length, reason, sizeof(reason), NULL, NULL)) {
        show_alert("Restore Patch Failed", reason);
        free(data);
        return;
    }
    // notes §110
    tBankBrowserItem items[KORG_SWEEP_PRESET_COUNT];

    for (uint32_t i = 0; i < KORG_SWEEP_PRESET_COUNT; i++) {
        items[i].name             = gKorgSweepLabels[i];
        items[i].category         = gKorgSweepCategoryIndex[i];
        items[i].bank1Indexed     = (i / 128) + 1;
        items[i].location1Indexed = (i % 128) + 1;
    }

    tPanelDial *     categoryDial      = find_panel_dial_by_label(synth_panel_config(), "Category");
    const char *     categoryNames[PANEL_MAX_NAMES];
    uint32_t         categoryNameCount = 0;

    if (categoryDial != NULL) {
        categoryNameCount = categoryDial->nameCount;

        for (uint32_t i = 0; i < categoryNameCount; i++) {
            categoryNames[i] = categoryDial->names[i];
        }
    }
    gPendingRestoreToBankData = data;
    gPendingRestoreToBankLen  = length;
    open_bank_browser("Restore Patch to Bank", "Choose a destination to load this file into:", "Next...",
                      items, KORG_SWEEP_PRESET_COUNT,
                      (categoryNameCount > 0) ? categoryNames : NULL, categoryNameCount, on_restore_patch_to_bank_target_chosen);
}

void synth_backup_restore_patch_to_bank(void) {
    if (warn_if_not_connected("Load Patch File to Bank Slot")) {
        return;
    }

    if (synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Restore Patch to Bank: connected device isn't Korg-style — this action doesn't support it yet\n");
        return;
    }
    open_file_browser_read(korg_restore_patch_to_bank_file_chosen);
}

void synth_backup_restore_patch_to_bank_from_path(const char * path, uint8_t bank, uint32_t prog) {
    if (!gDevice.connected) {
        LOG_ERROR("Restore: no device connected\n");
        return;
    }

    if (synth_panel_config()->moogStyleDump) {
        LOG_ERROR("Restore Patch to Bank: connected device isn't Korg-style — this action doesn't support it yet\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    if (!restore_validate_korg_dump(data, length, reason, sizeof(reason), NULL, NULL)) {
        LOG_ERROR("Restore Patch to Bank: %s\n", reason);
        free(data);
        return;
    }
    korg_restore_patch_to_bank_send(data, length, bank, prog);
    free(data);
}

// notes §111
static uint8_t * gPendingRestoreBankData       = NULL;
static uint32_t  gPendingRestoreBankLen        = 0;
static char      gPendingRestoreBankPath[1024] = {0};

static void on_restore_bank_confirmed(bool confirmed) {
    if (!confirmed) {
        LOG_DEBUG("Restore: bank restore cancelled at confirmation\n");
        free(gPendingRestoreBankData);
        gPendingRestoreBankData = NULL;
        return;
    }

    if (midi_send(gPendingRestoreBankData, gPendingRestoreBankLen)) {
        LOG_DEBUG("Restore: sent %u byte Bank dump from %s (overwrote entire current bank)\n", (unsigned)gPendingRestoreBankLen, gPendingRestoreBankPath);
        // notes §112
        gNameCacheValid = false;
        name_cache_clear_disk();
        show_alert("Restore Bank", "Sent — the current bank should now match this file.");
    } else {
        LOG_ERROR("Restore: failed to send %u byte Bank dump from %s\n", (unsigned)gPendingRestoreBankLen, gPendingRestoreBankPath);
        show_alert("Restore Bank Failed", "The message couldn't be sent — see the debug log for the exact MIDI error.");
    }
    free(gPendingRestoreBankData);
    gPendingRestoreBankData = NULL;
}

// Runs on the main thread once the user has chosen (or cancelled) a whole-
// bank dump file to restore — see synth_backup_restore_bank() below.
static void restore_bank_file_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: bank restore cancelled\n");
        return;
    }
    uint32_t  length = 0;
    uint8_t * data   = restore_read_file(path, &length);

    if (data == NULL) {
        return;
    }
    char      reason[192];

    if (!restore_validate_moog_dump(data, length, 0x01, "Bank dump", reason, sizeof(reason))) {
        show_alert("Restore Bank Failed", reason);
        free(data);
        return;
    }
    gPendingRestoreBankData                                      = data;
    gPendingRestoreBankLen                                       = length;
    strncpy(gPendingRestoreBankPath, path, sizeof(gPendingRestoreBankPath) - 1);
    gPendingRestoreBankPath[sizeof(gPendingRestoreBankPath) - 1] = '\0';
    show_confirm("Restore Bank",
                 "This will overwrite ALL 128 presets in the current bank on the connected device with the contents of this file. This cannot be undone.",
                 "Restore...", on_restore_bank_confirmed);
}

void synth_backup_restore_bank(void) {
    if (warn_if_not_connected("Restore Bank")) {
        return;
    }
    open_file_browser_read(restore_bank_file_chosen);
}

// notes §113
static bool     gRestoreFolderActive       = false;
static char     gRestoreFolderFolder[1024] = {0};
static uint32_t gRestoreFolderEntries[BACKUP_BATCH_PRESET_COUNT]; // preset numbers, in Patches.txt order, filtered to ones a matching file was actually found for
static uint32_t gRestoreFolderCount        = 0;                   // how many entries above are valid
static uint32_t gRestoreFolderIndex        = 0;                   // which entry synth_backup_flush_restore_folder() sends next
static double   gRestoreFolderNextSendMs   = 0.0;
static uint32_t gRestoreFolderSentCount    = 0;
static uint32_t gRestoreFolderMissingCount = 0;

// notes §114
#define RESTORE_FOLDER_SEND_PACING_MS         150.0

// notes §115
#define KORG_RESTORE_FOLDER_SEND_PACING_MS    500.0

// notes §116
static uint32_t restore_folder_parse_index(const char * folder, uint32_t * outNumbers, uint32_t maxCount) {
    char     indexPath[1280];

    backup_index_file_path(indexPath, sizeof(indexPath), folder);
    FILE *   f     = fopen(indexPath, "r");

    if (f == NULL) {
        return 0;
    }
    uint32_t count = 0;
    char     line[512];

    while ((count < maxCount) && (fgets(line, sizeof(line), f) != NULL)) {
        if (  (strlen(line) >= 5) && isdigit((unsigned char)line[0]) && isdigit((unsigned char)line[1])
           && isdigit((unsigned char)line[2]) && (line[3] == ' ') && (line[4] == ' ')) {
            uint32_t num = (uint32_t)((line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0'));

            if ((num >= 1) && (num <= BACKUP_BATCH_PRESET_COUNT)) {
                outNumbers[count++] = num;
            }
        }
    }
    fclose(f);
    return count;
}

// notes §117
static bool restore_folder_find_file(const char * folder, uint32_t presetNumber, char * outPath, size_t outPathSize) {
    DIR *           dp    = opendir(folder);

    if (dp == NULL) {
        return false;
    }
    char            prefix[4];

    snprintf(prefix, sizeof(prefix), "%03u", presetNumber);
    bool            found = false;
    struct dirent * entry;

    while ((entry = readdir(dp)) != NULL) {
        if (  (strlen(entry->d_name) >= 4) && (strncmp(entry->d_name, prefix, 3) == 0)
           && ((entry->d_name[3] == ' ') || (entry->d_name[3] == '.'))) {
            snprintf(outPath, outPathSize, "%s/%s", folder, entry->d_name);
            found = true;
            break;
        }
    }
    closedir(dp);
    return found;
}

// notes §118
static uint32_t gPendingRestoreFolderMissingCount = 0;

static void on_restore_folder_confirmed(bool confirmed) {
    if (!confirmed) {
        LOG_DEBUG("Restore: folder restore cancelled at confirmation\n");
        return;
    }
    gRestoreFolderIndex        = 0;
    gRestoreFolderSentCount    = 0;
    gRestoreFolderMissingCount = gPendingRestoreFolderMissingCount; // entries listed but never found on disk
    gRestoreFolderActive       = true;
    gRestoreFolderNextSendMs   = backup_monotonic_ms();             // send the first one on the very next flush tick
    LOG_DEBUG("Restore: starting folder restore of %u preset(s) from %s\n", (unsigned)gRestoreFolderCount, gRestoreFolderFolder);
}

// Runs on the main thread once the user has chosen (or cancelled) a folder
// to restore from — see synth_backup_restore_folder() below.
static void restore_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: folder restore cancelled\n");
        return;
    }
    uint32_t numbers[BACKUP_BATCH_PRESET_COUNT];
    uint32_t indexCount = restore_folder_parse_index(path, numbers, BACKUP_BATCH_PRESET_COUNT);

    if (indexCount == 0) {
        show_alert("Restore Folder Failed",
                   "No index for the connected device found in this folder (or it has no entries) — pick a folder created by Backup > Bank (Individual Files) for this same device.");
        return;
    }
    // notes §119
    gRestoreFolderCount                                    = 0;

    for (uint32_t i = 0; i < indexCount; i++) {
        char filePath[1280];

        if (restore_folder_find_file(path, numbers[i], filePath, sizeof(filePath))) {
            gRestoreFolderEntries[gRestoreFolderCount++] = numbers[i];
        }
    }

    if (gRestoreFolderCount == 0) {
        show_alert("Restore Folder Failed", "The index lists presets, but none of their files could be found in this folder.");
        return;
    }
    strncpy(gRestoreFolderFolder, path, sizeof(gRestoreFolderFolder) - 1);
    gRestoreFolderFolder[sizeof(gRestoreFolderFolder) - 1] = '\0';

    char message[256];

    snprintf(message, sizeof(message),
             "This will restore %u preset(s) found in this folder, overwriting their exact matching slots on the connected device. This cannot be undone.",
             (unsigned)gRestoreFolderCount);

    gPendingRestoreFolderMissingCount                      = indexCount - gRestoreFolderCount;
    show_confirm("Restore Folder", message, "Restore...", on_restore_folder_confirmed);
}

// notes §120

void synth_backup_flush_restore_folder(void) {
    if (!gRestoreFolderActive) {
        return;
    }

    if (backup_monotonic_ms() < gRestoreFolderNextSendMs) {
        return; // still pacing since the last send
    }
    // notes §121
    synthlib_request_redraw();
    uint32_t presetNumber = gRestoreFolderEntries[gRestoreFolderIndex];
    char     filePath[1280];

    if (restore_folder_find_file(gRestoreFolderFolder, presetNumber, filePath, sizeof(filePath))) {
        uint32_t  length = 0;
        uint8_t * data   = restore_read_file(filePath, &length);

        if (data != NULL) {
            char reason[192];

            if (restore_validate_moog_dump(data, length, 0x03, "Single Preset Dump", reason, sizeof(reason)) && midi_send(data, length)) {
                gRestoreFolderSentCount++;
                // notes §122
                name_cache_update_from_preset_dump(presetNumber, data, length);
                LOG_DEBUG("Restore: sent preset %u from %s (%u/%u)\n", (unsigned)presetNumber, filePath,
                          (unsigned)(gRestoreFolderIndex + 1), (unsigned)gRestoreFolderCount);
            } else {
                gRestoreFolderMissingCount++;
                LOG_ERROR("Restore: failed to send preset %u from %s (%s)\n", (unsigned)presetNumber, filePath, reason);
            }
            free(data);
        } else {
            gRestoreFolderMissingCount++;
        }
    } else {
        gRestoreFolderMissingCount++; // file listed a moment ago at restore_folder_chosen() time but gone now — race with something else touching the folder mid-sweep
    }
    gRestoreFolderIndex++;

    if (gRestoreFolderIndex >= gRestoreFolderCount) {
        gRestoreFolderActive = false;
        // notes §123
        synth_request_state_dump();
        char summary[192];
        snprintf(summary, sizeof(summary), "Restored %u preset(s), %u missing/failed.",
                 (unsigned)gRestoreFolderSentCount, (unsigned)gRestoreFolderMissingCount);
        show_alert("Restore Folder", summary);
        LOG_DEBUG("Restore: folder restore finished — %u sent, %u missing/failed\n",
                  (unsigned)gRestoreFolderSentCount, (unsigned)gRestoreFolderMissingCount);
        return;
    }
    gRestoreFolderNextSendMs = backup_monotonic_ms() + RESTORE_FOLDER_SEND_PACING_MS;
}

// notes §124

// notes §125
static uint32_t korg_restore_folder_parse_index(const char * folder, uint32_t * outIndices, uint32_t maxCount) {
    char     indexPath[1280];

    backup_index_file_path(indexPath, sizeof(indexPath), folder);
    FILE *   f     = fopen(indexPath, "r");

    if (f == NULL) {
        return 0;
    }
    uint32_t count = 0;
    char     line[512];

    while ((count < maxCount) && (fgets(line, sizeof(line), f) != NULL)) {
        if (  ((line[0] == 'A') || (line[0] == 'B'))
           && isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2]) && isdigit((unsigned char)line[3])
           && (line[4] == ' ') && (line[5] == ' ')) {
            uint8_t  bank = (line[0] == 'B') ? 1 : 0;
            uint32_t prog = (uint32_t)((line[1] - '0') * 100 + (line[2] - '0') * 10 + (line[3] - '0'));

            if ((prog >= 1) && (prog <= 128)) {
                outIndices[count++] = (bank ? 128 : 0) + (prog - 1);
            }
        }
    }
    fclose(f);
    return count;
}

// notes §126
static bool korg_restore_folder_find_file(const char * folder, uint32_t index, char * outPath, size_t outPathSize) {
    DIR *           dp    = opendir(folder);

    if (dp == NULL) {
        return false;
    }
    uint8_t         bank  = (uint8_t)(index / 128);
    uint32_t        prog  = (index % 128) + 1;
    char            prefix[5];

    snprintf(prefix, sizeof(prefix), "%c%03u", bank ? 'B' : 'A', prog);
    bool            found = false;
    struct dirent * entry;

    while ((entry = readdir(dp)) != NULL) {
        if (  (strlen(entry->d_name) >= 5) && (strncmp(entry->d_name, prefix, 4) == 0)
           && ((entry->d_name[4] == ' ') || (entry->d_name[4] == '.'))) {
            snprintf(outPath, outPathSize, "%s/%s", folder, entry->d_name);
            found = true;
            break;
        }
    }
    closedir(dp);
    return found;
}

// Stashed by korg_restore_folder_chosen() immediately before opening the
// now-asynchronous confirmation dialog — the Korg counterpart to
// gPendingRestoreFolderMissingCount above (Moog).
static uint32_t gPendingKorgRestoreFolderMissingCount = 0;

static void on_korg_restore_folder_confirmed(bool confirmed) {
    if (!confirmed) {
        LOG_DEBUG("Restore: folder restore cancelled at confirmation\n");
        return;
    }
    gKorgRestoreFolderIndex        = 0;
    gKorgRestoreFolderSentCount    = 0;
    gKorgRestoreFolderMissingCount = gPendingKorgRestoreFolderMissingCount; // entries listed but never found on disk
    gKorgRestoreFolderActive       = true;
    gKorgRestoreFolderNextSendMs   = backup_monotonic_ms();                 // send the first one on the very next flush tick
    LOG_DEBUG("Restore: starting Korg folder restore of %u program(s) from %s\n", (unsigned)gKorgRestoreFolderCount, gKorgRestoreFolderFolder);
}

// Runs on the main thread once the user has chosen (or cancelled) a folder
// to restore from — the Korg counterpart to restore_folder_chosen() above
// (Moog). See synth_backup_restore_folder() below.
static void korg_restore_folder_chosen(const char * path) {
    if (path == NULL) {
        LOG_DEBUG("Restore: folder restore cancelled\n");
        return;
    }
    uint32_t indices[KORG_SWEEP_PRESET_COUNT];
    uint32_t indexCount = korg_restore_folder_parse_index(path, indices, KORG_SWEEP_PRESET_COUNT);

    if (indexCount == 0) {
        show_alert("Restore Folder Failed",
                   "No index for the connected device found in this folder (or it has no entries) — pick a folder created by Backup > Bank (Individual Files) for this same device.");
        return;
    }
    gKorgRestoreFolderCount                                        = 0;

    for (uint32_t i = 0; i < indexCount; i++) {
        char filePath[1280];

        if (korg_restore_folder_find_file(path, indices[i], filePath, sizeof(filePath))) {
            gKorgRestoreFolderEntries[gKorgRestoreFolderCount++] = indices[i];
        }
    }

    if (gKorgRestoreFolderCount == 0) {
        show_alert("Restore Folder Failed", "The index lists programs, but none of their files could be found in this folder.");
        return;
    }
    strncpy(gKorgRestoreFolderFolder, path, sizeof(gKorgRestoreFolderFolder) - 1);
    gKorgRestoreFolderFolder[sizeof(gKorgRestoreFolderFolder) - 1] = '\0';

    char message[256];

    snprintf(message, sizeof(message),
             "This will restore %u program(s) found in this folder, overwriting their exact matching slots on the connected device. This cannot be undone.",
             (unsigned)gKorgRestoreFolderCount);

    gPendingKorgRestoreFolderMissingCount                          = indexCount - gKorgRestoreFolderCount;
    show_confirm("Restore Folder", message, "Restore...", on_korg_restore_folder_confirmed);
}

void synth_backup_restore_folder(void) {
    if (warn_if_not_connected("Restore Folder")) {
        return;
    }

    // Korg-style (Z1): fully separate folder-chosen/flush path — see this
    // whole block's own header comment for why. Added 2026-07-14.
    if (!synth_panel_config()->moogStyleDump) {
        if (gKorgRestoreFolderActive || (gBackupExpect != eBackupExpectNone)) {
            LOG_ERROR("Restore: another backup/restore operation is already in progress\n");
            return;
        }
        open_file_browser_folder(korg_restore_folder_chosen, "Choose Folder to Restore From");
        return;
    }

    if (gRestoreFolderActive || gBackupBatchActive || (gBackupExpect != eBackupExpectNone)) {
        LOG_ERROR("Restore: another backup/restore operation is already in progress\n");
        return;
    }
    open_file_browser_folder(restore_folder_chosen, "Choose Folder to Restore From");
}

// notes §127
void synth_backup_flush_korg_restore_folder(void) {
    if (!gKorgRestoreFolderActive) {
        return;
    }

    if (backup_monotonic_ms() < gKorgRestoreFolderNextSendMs) {
        return; // still pacing since the last send
    }
    synthlib_request_redraw();
    uint32_t index = gKorgRestoreFolderEntries[gKorgRestoreFolderIndex];
    uint8_t  bank  = (uint8_t)(index / 128);
    uint32_t prog  = (index % 128) + 1;
    char     filePath[1280];

    if (korg_restore_folder_find_file(gKorgRestoreFolderFolder, index, filePath, sizeof(filePath))) {
        uint32_t  length = 0;
        uint8_t * data   = restore_read_file(filePath, &length);

        if (data != NULL) {
            char reason[192];

            if (restore_validate_korg_dump(data, length, reason, sizeof(reason), NULL, NULL) && midi_send(data, length)) {
                gKorgRestoreFolderSentCount++;
                korg_name_cache_update_from_dump(bank, prog, data, length);
                LOG_DEBUG("Restore: sent Bank %c, Program %u from %s (%u/%u)\n", bank ? 'B' : 'A', (unsigned)prog, filePath,
                          (unsigned)(gKorgRestoreFolderIndex + 1), (unsigned)gKorgRestoreFolderCount);
            } else {
                gKorgRestoreFolderMissingCount++;
                LOG_ERROR("Restore: failed to send Bank %c, Program %u from %s (%s)\n", bank ? 'B' : 'A', (unsigned)prog, filePath, reason);
            }
            free(data);
        } else {
            gKorgRestoreFolderMissingCount++;
        }
    } else {
        gKorgRestoreFolderMissingCount++; // file listed a moment ago at korg_restore_folder_chosen() time but gone now — race with something else touching the folder mid-sweep
    }
    gKorgRestoreFolderIndex++;

    if (gKorgRestoreFolderIndex >= gKorgRestoreFolderCount) {
        gKorgRestoreFolderActive = false;
        char summary[192];
        snprintf(summary, sizeof(summary), "Restored %u program(s), %u missing/failed.",
                 (unsigned)gKorgRestoreFolderSentCount, (unsigned)gKorgRestoreFolderMissingCount);
        show_alert("Restore Folder", summary);
        LOG_DEBUG("Restore: Korg folder restore finished — %u sent, %u missing/failed\n",
                  (unsigned)gKorgRestoreFolderSentCount, (unsigned)gKorgRestoreFolderMissingCount);
        return;
    }
    gKorgRestoreFolderNextSendMs = backup_monotonic_ms() + KORG_RESTORE_FOLDER_SEND_PACING_MS;
}

bool synth_backup_get_export_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount) {
    // notes §128
    if (gKorgSweepActive) {
        *outCurrent     = gKorgSweepIndex + 1; // 0-based index -> 1-based "Nth of M"
        *outTotal       = KORG_SWEEP_PRESET_COUNT;
        *outActionCount = gKorgSweepRepliedCount;
        return true;
    }

    if (!gBackupBatchActive) {
        return false;
    }
    *outCurrent     = gBackupBatchCurrentPreset;
    *outTotal       = BACKUP_BATCH_PRESET_COUNT;
    *outActionCount = gBackupBatchRepliedCount;
    return true;
}

bool synth_backup_export_progress_is_name_sweep(void) {
    // notes §129
    return (gKorgSweepActive && (gKorgSweepMode == eKorgSweepModeNameSweep))
           || (gBackupBatchActive && (gBackupBatchMode == eBatchModeNameSweep));
}

bool synth_backup_get_restore_progress(uint32_t * outCurrent, uint32_t * outTotal, uint32_t * outActionCount) {
    // notes §130
    if (gKorgRestoreFolderActive) {
        *outCurrent     = gKorgRestoreFolderIndex + 1; // 0-based index -> 1-based "Nth of M"
        *outTotal       = gKorgRestoreFolderCount;
        *outActionCount = gKorgRestoreFolderSentCount;
        return true;
    }

    if (!gRestoreFolderActive) {
        return false;
    }
    *outCurrent     = gRestoreFolderIndex + 1; // 0-based index -> 1-based "Nth of M"
    *outTotal       = gRestoreFolderCount;
    *outActionCount = gRestoreFolderSentCount;
    return true;
}

// notes §131
#define BACKGROUND_PREFETCH_SETTLE_MS    2000.0

static double gBackgroundPrefetchEligibleSinceMs = 0.0;

// notes §132
void synth_backup_flush_background_prefetch(void) {
    if (!gDevice.connected) {
        gBackgroundPrefetchEligibleSinceMs = 0.0;
        return;
    }
    bool moog    = synth_panel_config()->moogStyleDump;

    // notes §133
    if (!moog && !synth_panel_config()->supportsKorgProgramDump) {
        return;
    }

    if (moog ? gNameCacheValid : gKorgNameCacheValid) {
        return;
    }

    // Already running (a sweep this function itself started earlier, an
    // explicit Load/Store click's own sweep, or — Moog only — someone
    // mid-export) — nothing to (re)start either way.
    if (moog ? gBackupBatchActive : gKorgSweepActive) {
        return;
    }
    // notes §134
    bool blocked = (moog ? gRestoreFolderActive : gKorgRestoreFolderActive)
                   || (moog ? (gBackupExpect != eBackupExpectNone)
                           : ((gBackupExpect != eBackupExpectNone) && (gBackupExpect != eBackupExpectKorgProgram)));

    if (blocked) {
        gBackgroundPrefetchEligibleSinceMs = 0.0; // reset — start counting again once whatever's running finishes
        return;
    }

    if (gBackgroundPrefetchEligibleSinceMs == 0.0) {
        gBackgroundPrefetchEligibleSinceMs = backup_monotonic_ms();
        return;
    }

    if ((backup_monotonic_ms() - gBackgroundPrefetchEligibleSinceMs) < BACKGROUND_PREFETCH_SETTLE_MS) {
        return;
    }

    if (moog) {
        LOG_DEBUG("Load/Store: starting background name prefetch (Moog-style)\n");
        moog_name_sweep_start();
    } else {
        LOG_DEBUG("Load/Store: starting background name prefetch (Korg-style)\n");
        korg_sweep_start();
    }
}
