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

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synthlibDefs.h"
#include "panelConfig.h"

#define PANEL_LINE_LEN      512
#define PANEL_MAX_TOKENS    16
#define PANEL_TOKEN_LEN     64

// ── Line tokenizer ────────────────────────────────────────────────────────────
// Splits on whitespace, except inside "..." (which may itself contain spaces,
// e.g. label="F1 Cut"). Quotes are stripped from the output. '#' outside
// quotes starts a comment and ends tokenizing for the rest of the line.
static uint32_t tokenize(const char * line, char tokens[][PANEL_TOKEN_LEN], uint32_t maxTokens) {
    uint32_t count    = 0;
    uint32_t written  = 0;
    bool     inQuotes = false;
    bool     inToken  = false;

    for (const char * p = line; ; p++) {
        char c       = *p;

        if ((c == '#') && !inQuotes) {
            break;
        }

        if (c == '"') {
            inQuotes = !inQuotes;
            inToken  = true;
            continue;
        }
        bool atEnd   = (c == '\0');
        bool isSpace = (c == ' ') || (c == '\t') || atEnd;

        if (isSpace && !inQuotes) {
            if (inToken) {
                tokens[count][written] = '\0';
                count++;
                written                = 0;
                inToken                = false;
            }

            if (atEnd || (count >= maxTokens)) {
                break;
            }
            continue;
        }

        if (count >= maxTokens) {
            break;
        }
        inToken = true;

        if (written < (PANEL_TOKEN_LEN - 1)) {
            tokens[count][written++] = c;
        }
    }

    return count;
}

static void join_tokens(char tokens[][PANEL_TOKEN_LEN], uint32_t from, uint32_t count, char * out, size_t outMax) {
    out[0] = '\0';

    for (uint32_t i = from; i < count; i++) {
        if (i > from) {
            strncat(out, " ", outMax - strlen(out) - 1);
        }
        strncat(out, tokens[i], outMax - strlen(out) - 1);
    }
}

static bool split_kv(const char * token, char * key, size_t keyMax, char * val, size_t valMax) {
    const char * eq     = strchr(token, '=');

    if (!eq) {
        return false;
    }
    size_t       keyLen = (size_t)(eq - token);

    if (keyLen >= keyMax) {
        keyLen = keyMax - 1;
    }
    memcpy(key, token, keyLen);
    key[keyLen]     = '\0';

    strncpy(val, eq + 1, valMax - 1);
    val[valMax - 1] = '\0';
    return true;
}

static uint32_t split_csv(const char * value, char names[][PANEL_LABEL_LEN], uint32_t maxNames) {
    uint32_t     count = 0;
    const char * p     = value;

    while (*p && (count < maxNames)) {
        const char * start = p;

        while (*p && (*p != ',')) {
            p++;
        }
        size_t       len   = (size_t)(p - start);

        if (len >= PANEL_LABEL_LEN) {
            len = PANEL_LABEL_LEN - 1;
        }
        memcpy(names[count], start, len);
        names[count][len] = '\0';
        count++;

        if (*p == ',') {
            p++;
        }
    }
    return count;
}

static bool find_section_colour(tPanelSection * section, const char * name, tRgb * outColour) {
    for (uint32_t i = 0; i < section->colourCount; i++) {
        if (strcmp(section->colours[i].name, name) == 0) {
            *outColour = section->colours[i].colour;
            return true;
        }
    }

    return false;
}

// ── "dial <id> key=value ..." ────────────────────────────────────────────────
static void parse_dial_line(tPanelSection * section, double * pendingGap, char tokens[][PANEL_TOKEN_LEN], uint32_t tokenCount, uint32_t lineNo) {
    if (tokenCount < 2) {
        LOG_ERROR("panelConfig line %u: 'dial' with no id\n", lineNo);
        return;
    }

    if (section->dialCount >= PANEL_MAX_DIALS) {
        LOG_ERROR("panelConfig line %u: too many dials in section (max %u)\n", lineNo, (unsigned)PANEL_MAX_DIALS);
        return;
    }
    tPanelDial * dial = &section->dials[section->dialCount++];

    memset(dial, 0, sizeof(*dial));
    strncpy(dial->id, tokens[1], sizeof(dial->id) - 1);
    dial->gapBefore  = *pendingGap;
    *pendingGap      = 0.0;
    dial->dumpOffset  = -1;  // not present in a program dump unless "dumpOffset=" says otherwise
    dial->dumpMask    = 0xFF; // whole byte by default
    dial->dumpOffset2 = -1;  // no second bit-location chunk unless "dumpOffset2=" says otherwise
    dial->gridCol    = -1.0; // not grid-positioned unless "col=" says otherwise
    dial->gridRow    = -1.0;

    for (uint32_t i = 2; i < tokenCount; i++) {
        char key[32];
        char val[PANEL_TOKEN_LEN];

        if (!split_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
            LOG_ERROR("panelConfig line %u: expected key=value, got '%s'\n", lineNo, tokens[i]);
            continue;
        }

        if (strcmp(key, "label") == 0) {
            strncpy(dial->label, val, sizeof(dial->label) - 1);
        } else if (strcmp(key, "color") == 0) {
            strncpy(dial->colourName, val, sizeof(dial->colourName) - 1);

            if (!find_section_colour(section, val, &dial->colour)) {
                LOG_ERROR("panelConfig line %u: unknown colour '%s'\n", lineNo, val);
            }
        } else if (strcmp(key, "max") == 0) {
            dial->max = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "names") == 0) {
            dial->nameCount = split_csv(val, dial->names, PANEL_MAX_NAMES);
            dial->display   = dialDisplayNames;
        } else if (strcmp(key, "display") == 0) {
            if (strcmp(val, "raw") == 0) {
                dial->display = dialDisplayRaw;
            } else if (strcmp(val, "ccnative") == 0) {
                dial->display = dialDisplayCcNative;
            } else {
                LOG_ERROR("panelConfig line %u: unknown display '%s'\n", lineNo, val);
            }
        } else if (strcmp(key, "offset") == 0) {
            dial->storageOffset = (int32_t)strtol(val, NULL, 0);
        } else if (strcmp(key, "group") == 0) {
            dial->paramGroup = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "param") == 0) {
            dial->paramId = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "cc") == 0) {
            dial->ccNumber = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "ccLsb") == 0) {
            dial->ccLsbNumber = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "nativeMax") == 0) {
            dial->nativeMax = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpOffset") == 0) {
            dial->dumpOffset = (int32_t)strtol(val, NULL, 0);
        } else if (strcmp(key, "dumpShift") == 0) {
            dial->dumpShift = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpMask") == 0) {
            dial->dumpMask = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpBitOffset") == 0) {
            dial->dumpBitOffset = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpBitWidth") == 0) {
            dial->dumpBitWidth = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpOffset2") == 0) {
            dial->dumpOffset2 = (int32_t)strtol(val, NULL, 0);
        } else if (strcmp(key, "dumpBitOffset2") == 0) {
            dial->dumpBitOffset2 = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpBitWidth2") == 0) {
            dial->dumpBitWidth2 = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "col") == 0) {
            dial->gridCol = strtod(val, NULL);
        } else if (strcmp(key, "row") == 0) {
            dial->gridRow = strtod(val, NULL);
        } else {
            LOG_ERROR("panelConfig line %u: unknown dial attribute '%s'\n", lineNo, key);
        }
    }
}

// ── "color <name> <r> <g> <b>" ───────────────────────────────────────────────
static void parse_colour_line(tPanelSection * section, char tokens[][PANEL_TOKEN_LEN], uint32_t tokenCount, uint32_t lineNo) {
    if (tokenCount < 5) {
        LOG_ERROR("panelConfig line %u: expected 'color <name> <r> <g> <b>'\n", lineNo);
        return;
    }

    if (section->colourCount >= PANEL_MAX_COLOURS) {
        LOG_ERROR("panelConfig line %u: too many colours in section (max %u)\n", lineNo, (unsigned)PANEL_MAX_COLOURS);
        return;
    }
    tPanelColour * colour = &section->colours[section->colourCount++];

    strncpy(colour->name, tokens[1], sizeof(colour->name) - 1);
    colour->colour.red   = strtod(tokens[2], NULL);
    colour->colour.green = strtod(tokens[3], NULL);
    colour->colour.blue  = strtod(tokens[4], NULL);
}

static void process_line(tPanelConfig * config, tPanelSection ** currentSection, double * pendingGap, const char * line, uint32_t lineNo) {
    char         tokens[PANEL_MAX_TOKENS][PANEL_TOKEN_LEN];
    uint32_t     tokenCount = tokenize(line, tokens, PANEL_MAX_TOKENS);

    if (tokenCount == 0) {
        return; // blank line or comment-only
    }
    const char * keyword    = tokens[0];

    if (strcmp(keyword, "device") == 0) {
        join_tokens(tokens, 1, tokenCount, config->deviceName, sizeof(config->deviceName));
    } else if (strcmp(keyword, "description") == 0) {
        join_tokens(tokens, 1, tokenCount, config->description, sizeof(config->description));
    } else if (strcmp(keyword, "manufacturerId") == 0) {
        // 1 value = classic single-byte ID (e.g. Korg 0x42); 3 values = an
        // "extended" ID (e.g. Novation) for manufacturers registered after
        // single-byte IDs ran out — see the tPanelConfig field comment.
        uint32_t valueCount = tokenCount - 1;

        if ((valueCount != 1) && (valueCount != 3)) {
            LOG_ERROR("panelConfig line %u: manufacturerId needs 1 or 3 byte values, got %u\n", lineNo, (unsigned)valueCount);
            valueCount = 1;
        }
        config->manufacturerIdLen = valueCount;

        for (uint32_t b = 0; b < valueCount; b++) {
            config->manufacturerId[b] = (uint8_t)strtoul(tokens[1 + b], NULL, 0);
        }
    } else if (strcmp(keyword, "familyId") == 0) {
        config->familyId = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "memberId") == 0) {
        config->memberId = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "progNameLen") == 0) {
        config->progNameLen = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "panelNameOffset") == 0) {
        config->panelNameOffset = (int32_t)strtol(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "panelNameBitOffset") == 0) {
        config->panelNameBitOffset = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "panelNameLen") == 0) {
        config->panelNameLen = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "presetNameOffset") == 0) {
        config->presetNameOffset = (int32_t)strtol(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "presetNameBitOffset") == 0) {
        config->presetNameBitOffset = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "presetNameLen") == 0) {
        config->presetNameLen = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "nameLineWidth") == 0) {
        config->nameLineWidth = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "presetBankCount") == 0) {
        config->presetBankCount = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "gridColWidth") == 0) {
        config->gridColWidth = strtod(tokens[1], NULL);
    } else if (strcmp(keyword, "gridRowHeight") == 0) {
        config->gridRowHeight = strtod(tokens[1], NULL);
    } else if (strcmp(keyword, "scrollDial") == 0) {
        strncpy(config->scrollDialId, tokens[1], sizeof(config->scrollDialId) - 1);
    } else if (strcmp(keyword, "identityQuery") == 0) {
        config->supportsIdentity = (strcmp(tokens[1], "no") != 0);
    } else if (strcmp(keyword, "midiChannel") == 0) {
        config->midiChannel = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "midiPort") == 0) {
        join_tokens(tokens, 1, tokenCount, config->midiPortName, sizeof(config->midiPortName));
    } else if (strcmp(keyword, "stateRequestSysEx") == 0) {
        uint32_t valueCount = tokenCount - 1;

        if (valueCount > (uint32_t)sizeof(config->stateRequestSysEx)) {
            LOG_ERROR("panelConfig line %u: stateRequestSysEx too long (max %u bytes)\n",
                      lineNo, (unsigned)sizeof(config->stateRequestSysEx));
            valueCount = (uint32_t)sizeof(config->stateRequestSysEx);
        }
        config->stateRequestSysExLen = valueCount;

        for (uint32_t b = 0; b < valueCount; b++) {
            config->stateRequestSysEx[b] = (uint8_t)strtoul(tokens[1 + b], NULL, 0);
        }
    } else if (strcmp(keyword, "dumpFormat") == 0) {
        config->moogStyleDump = (strcmp(tokens[1], "moog") == 0);
    } else if (strcmp(keyword, "productId") == 0) {
        config->productId = (uint8_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "list") == 0) {
        if (tokenCount < 3) {
            LOG_ERROR("panelConfig line %u: expected 'list <name> <items>'\n", lineNo);
            return;
        }

        if (config->listCount >= PANEL_MAX_LISTS) {
            LOG_ERROR("panelConfig line %u: too many lists (max %u)\n", lineNo, (unsigned)PANEL_MAX_LISTS);
            return;
        }
        tPanelList * list = &config->lists[config->listCount++];

        strncpy(list->name, tokens[1], sizeof(list->name) - 1);
        list->itemCount = split_csv(tokens[2], list->items, PANEL_MAX_LIST_ITEMS);
    } else if (strcmp(keyword, "page") == 0) {
        if (config->sectionCount >= PANEL_MAX_SECTIONS) {
            LOG_ERROR("panelConfig line %u: too many sections (max %u)\n", lineNo, (unsigned)PANEL_MAX_SECTIONS);
            return;
        }
        *currentSection = &config->sections[config->sectionCount++];
        *pendingGap     = 0.0;
        join_tokens(tokens, 1, tokenCount, (*currentSection)->page, sizeof((*currentSection)->page));
    } else if (strcmp(keyword, "section") == 0) {
        if (!*currentSection) {
            LOG_ERROR("panelConfig line %u: 'section' with no preceding 'page'\n", lineNo);
            return;
        }
        join_tokens(tokens, 1, tokenCount, (*currentSection)->section, sizeof((*currentSection)->section));
    } else if (strcmp(keyword, "dialSize") == 0) {
        if (*currentSection) {
            (*currentSection)->dialSize = strtod(tokens[1], NULL);
        }
    } else if (strcmp(keyword, "spacing") == 0) {
        if (*currentSection) {
            (*currentSection)->spacing = strtod(tokens[1], NULL);
        }
    } else if (strcmp(keyword, "hidden") == 0) {
        if (*currentSection) {
            (*currentSection)->hidden = true;
        }
    } else if (strcmp(keyword, "gap") == 0) {
        *pendingGap += strtod(tokens[1], NULL);
    } else if (strcmp(keyword, "color") == 0) {
        if (!*currentSection) {
            LOG_ERROR("panelConfig line %u: 'color' with no preceding 'page'\n", lineNo);
            return;
        }
        parse_colour_line(*currentSection, tokens, tokenCount, lineNo);
    } else if (strcmp(keyword, "dial") == 0) {
        if (!*currentSection) {
            LOG_ERROR("panelConfig line %u: 'dial' with no preceding 'page'\n", lineNo);
            return;
        }
        parse_dial_line(*currentSection, pendingGap, tokens, tokenCount, lineNo);
    } else if (strcmp(keyword, "columnLabel") == 0) {
        if (!*currentSection) {
            LOG_ERROR("panelConfig line %u: 'columnLabel' with no preceding 'page'\n", lineNo);
            return;
        }

        if (tokenCount < 3) {
            LOG_ERROR("panelConfig line %u: expected 'columnLabel <col> <label>'\n", lineNo);
            return;
        }

        if (config->columnLabelCount >= PANEL_MAX_COLUMN_LABELS) {
            LOG_ERROR("panelConfig line %u: too many columnLabels (max %u)\n", lineNo, (unsigned)PANEL_MAX_COLUMN_LABELS);
            return;
        }
        tColumnLabel * columnLabel = &config->columnLabels[config->columnLabelCount++];

        // Page comes from the section currently open, same as how a dial
        // line implicitly belongs to whichever page/section is open when
        // it's parsed — a columnLabel isn't tied to any one section, but
        // still needs to know which page's columns it's labelling.
        strncpy(columnLabel->page, (*currentSection)->page, sizeof(columnLabel->page) - 1);
        columnLabel->col = (int32_t)strtol(tokens[1], NULL, 0);
        join_tokens(tokens, 2, tokenCount, columnLabel->label, sizeof(columnLabel->label));
    } else {
        LOG_ERROR("panelConfig line %u: unknown directive '%s'\n", lineNo, keyword);
    }
}

bool load_panel_config(const char * path, tPanelConfig * config) {
    FILE *          file           = fopen(path, "r");

    if (!file) {
        LOG_ERROR("panelConfig: failed to open '%s'\n", path);
        return false;
    }
    memset(config, 0, sizeof(*config));
    config->supportsIdentity = true;  // overridden by an explicit "identityQuery no" line
    config->panelNameOffset  = -1;    // overridden by an explicit "panelNameOffset" line
    config->presetNameOffset = -1;    // overridden by an explicit "presetNameOffset" line
    config->presetBankCount  = 1;     // overridden by an explicit "presetBankCount" line — see its own field comment in panelConfig.h

    tPanelSection * currentSection = NULL;
    double          pendingGap     = 0.0;
    char            line[PANEL_LINE_LEN];
    uint32_t        lineNo         = 0;

    while (fgets(line, sizeof(line), file)) {
        lineNo++;

        size_t len = strlen(line);

        // A physical line longer than PANEL_LINE_LEN-1 leaves fgets() without
        // its trailing newline — the remainder would otherwise be read as a
        // bogus separate "line" (garbage directive) by the next fgets() call.
        // Detect that and discard the rest of the real line instead, so an
        // over-long line degrades to "this one line didn't fully load"
        // rather than corrupting parsing of everything after it.
        if ((len == sizeof(line) - 1) && (line[len - 1] != '\n')) {
            LOG_ERROR("panelConfig line %u: line longer than %u bytes, truncated\n", lineNo, (unsigned)sizeof(line) - 1);
            int c;

            while (((c = fgetc(file)) != EOF) && (c != '\n')) {
            }
        }

        while ((len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r'))) {
            line[--len] = '\0';
        }
        process_line(config, &currentSection, &pendingGap, line, lineNo);
    }
    fclose(file);
    return true;
}

void layout_panel_section(tPanelSection * section, tRectangle origin, double gridColWidth, double gridRowHeight) {
    double x = origin.coord.x;

    for (uint32_t i = 0; i < section->dialCount; i++) {
        tPanelDial * dial = &section->dials[i];

        if ((dial->gridCol >= 0.0) && (gridColWidth > 0.0) && (gridRowHeight > 0.0)) {
            double row = (dial->gridRow >= 0.0) ? dial->gridRow : 0.0;

            dial->rect = (tRectangle){{
                                          origin.coord.x + (dial->gridCol * gridColWidth),
                                          origin.coord.y + (row * gridRowHeight)
                                      }, {
                                          section->dialSize, section->dialSize
                                      }
            };
            continue; // grid-positioned — doesn't touch the auto-flow x runner below
        }
        x         += dial->gapBefore;
        dial->rect = (tRectangle){{
                                      x, origin.coord.y
                                  }, {
                                      section->dialSize, section->dialSize
                                  }
        };
        x         += section->spacing;
    }
}

tPanelSection * find_panel_section(tPanelConfig * config, const char * page, const char * section) {
    for (uint32_t i = 0; i < config->sectionCount; i++) {
        if ((strcmp(config->sections[i].page, page) == 0) && (strcmp(config->sections[i].section, section) == 0)) {
            return &config->sections[i];
        }
    }

    return NULL;
}

tPanelDial * find_panel_dial(tPanelSection * section, const char * id) {
    for (uint32_t i = 0; i < section->dialCount; i++) {
        if (strcmp(section->dials[i].id, id) == 0) {
            return &section->dials[i];
        }
    }

    return NULL;
}

tPanelDial * find_panel_dial_anywhere(tPanelConfig * config, const char * id) {
    for (uint32_t s = 0; s < config->sectionCount; s++) {
        tPanelDial * dial = find_panel_dial(&config->sections[s], id);

        if (dial) {
            return dial;
        }
    }

    return NULL;
}

tPanelDial * find_panel_dial_by_param(tPanelSection * section, uint32_t group, uint32_t paramId) {
    for (uint32_t i = 0; i < section->dialCount; i++) {
        if ((section->dials[i].paramGroup == group) && (section->dials[i].paramId == paramId)) {
            return &section->dials[i];
        }
    }

    return NULL;
}

tPanelDial * find_panel_dial_by_cc(tPanelConfig * config, uint8_t cc) {
    for (uint32_t s = 0; s < config->sectionCount; s++) {
        tPanelSection * section = &config->sections[s];

        for (uint32_t i = 0; i < section->dialCount; i++) {
            if (  ((section->dials[i].ccNumber != 0) && (section->dials[i].ccNumber == cc))
               || ((section->dials[i].ccLsbNumber != 0) && (section->dials[i].ccLsbNumber == cc))) {
                return &section->dials[i];
            }
        }
    }

    return NULL;
}

int32_t hit_test_panel_section(tPanelSection * section, tCoord point) {
    for (uint32_t i = 0; i < section->dialCount; i++) {
        if (within_rectangle(point, section->dials[i].rect)) {
            return (int32_t)i;
        }
    }

    return -1;
}

bool panel_dial_is_toggle(const tPanelDial * dial) {
    return dial
           && (dial->display == dialDisplayNames)
           && (dial->nameCount == 2)
           && (strcmp(dial->names[0], "Off") == 0)
           && (strcmp(dial->names[1], "On") == 0);
}

bool panel_dial_is_binary(const tPanelDial * dial) {
    return dial && (dial->display == dialDisplayNames) && (dial->nameCount == 2);
}

bool panel_dial_needs_value_menu(const tPanelDial * dial) {
    return dial
           && (dial->display == dialDisplayNames)
           && (dial->nameCount > 2)
           && (dial->ccNumber == 0)
           && (dial->dumpBitWidth > 0);
}

uint32_t get_panel_dial_value(const tPanelDial * dial) {
    if (!dial) {
        return 0;
    }
    return (uint32_t)(dial->value - dial->storageOffset);
}

uint32_t get_panel_dial_native_value(const tPanelDial * dial) {
    if (!dial) {
        return 0;
    }
    return (uint32_t)dial->nativeValue;
}

const char * get_panel_list_item(const tPanelConfig * config, const char * listName, uint32_t index) {
    for (uint32_t i = 0; i < config->listCount; i++) {
        if (strcmp(config->lists[i].name, listName) == 0) {
            return (index < config->lists[i].itemCount) ? config->lists[i].items[index] : "?";
        }
    }

    return "?";
}

uint32_t get_panel_list_count(const tPanelConfig * config, const char * listName) {
    for (uint32_t i = 0; i < config->listCount; i++) {
        if (strcmp(config->lists[i].name, listName) == 0) {
            return config->lists[i].itemCount;
        }
    }

    return 0;
}

uint32_t scan_panel_configs(const char * dir, tPanelConfigCandidate * outCandidates, uint32_t maxCandidates) {
    DIR *               dp    = opendir(dir);

    if (!dp) {
        LOG_ERROR("scan_panel_configs: couldn't open '%s'\n", dir);
        return 0;
    }
    uint32_t            count = 0;
    static tPanelConfig scratch;    // one config at a time — too big for the stack, parsed and discarded per file
    struct dirent *     entry;

    while ((count < maxCandidates) && ((entry = readdir(dp)) != NULL)) {
        size_t                  nameLen   = strlen(entry->d_name);

        if ((nameLen < 5) || (strcmp(entry->d_name + nameLen - 4, ".txt") != 0)) {
            continue;
        }
        char                    path[1152];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

        if (!load_panel_config(path, &scratch)) {
            continue;
        }
        tPanelConfigCandidate * candidate = &outCandidates[count++];

        strncpy(candidate->filename, entry->d_name, sizeof(candidate->filename) - 1);
        candidate->filename[sizeof(candidate->filename) - 1]       = '\0';
        strncpy(candidate->deviceName, scratch.deviceName, sizeof(candidate->deviceName) - 1);
        candidate->deviceName[sizeof(candidate->deviceName) - 1]   = '\0';
        strncpy(candidate->description, scratch.description, sizeof(candidate->description) - 1);
        candidate->description[sizeof(candidate->description) - 1] = '\0';
    }
    closedir(dp);
    return count;
}
