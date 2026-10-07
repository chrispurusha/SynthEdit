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
// Notes: Docs/code-notes/panelConfig.c.md - "// notes §k" refers there.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "synthlibDefs.h"
#include "utilsGraphics.h"   // draw_button_bounds()
#include "panelConfig.h"

#define PANEL_LINE_LEN      1024
#define PANEL_MAX_TOKENS    32
// notes §1
#define PANEL_TOKEN_LEN     1024

// notes §2
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

// "Program | EXi 1 | LFO 1/2" -> "Program|EXi 1|LFO 1/2": the spaces either side of a separator are layout only.
static void normalise_page_path(char * page) {
    char * out = page;

    for (const char * in = page; *in != '\0'; in++) {
        if (*in == PANEL_PAGE_SEPARATOR) {
            while ((out > page) && (out[-1] == ' ')) {
                out--;
            }
            *out++ = *in;

            while (in[1] == ' ') {
                in++;
            }
        } else {
            *out++ = *in;
        }
    }

    *out = '\0';
}

static int32_t find_list_index(const tPanelConfig * config, const char * name) {
    for (uint32_t i = 0; i < config->listCount; i++) {
        if (strcmp(config->lists[i].name, name) == 0) {
            return (int32_t)i;
        }
    }

    return -1;
}

static uint32_t copy_list_items(const tPanelConfig * config, const char * listName, char names[][PANEL_LABEL_LEN], uint32_t maxNames) {
    int32_t  index = find_list_index(config, listName);
    uint32_t count = 0;

    if (index >= 0) {
        const tPanelList * list = &config->lists[index];

        count = (list->itemCount < maxNames) ? list->itemCount : maxNames;
        memcpy(names, list->items, (size_t)count * PANEL_LABEL_LEN);
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
static void parse_dial_line(const tPanelConfig * config, tPanelSection * section, double * pendingGap, char tokens[][PANEL_TOKEN_LEN], uint32_t tokenCount, uint32_t lineNo) {
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
    dial->gapBefore   = *pendingGap;
    *pendingGap       = 0.0;
    dial->dumpOffset  = -1;               // not present in a program dump unless "dumpOffset=" says otherwise
    dial->dumpMask    = 0xFF;             // whole byte by default
    dial->dumpOffset2 = -1;               // no second bit-location chunk unless "dumpOffset2=" says otherwise
    dial->gridCol     = -1.0;             // not grid-positioned unless "col=" says otherwise
    dial->gridRow     = -1.0;
    dial->colour      = (tRgb)RGB_GREY_7; // neutral, matches an off toggle — overwritten below if "color=" says otherwise; a names= dial with no color= would otherwise render/menu as unset black (see voyager.txt's own notes on this)

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
        } else if (strcmp(key, "variantMax") == 0) {
            dial->variantMax = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "names") == 0) {
            static char names[PANEL_MAX_NAMES][PANEL_LABEL_LEN];

            if (val[0] == '@') {
                // notes §9
                dial->nameCount = copy_list_items(config, val + 1, names, PANEL_MAX_NAMES);

                if (dial->nameCount == 0) {
                    LOG_ERROR("panelConfig line %u: unknown or empty list '%s'\n", lineNo, val + 1);
                }
            } else {
                dial->nameCount = split_csv(val, names, PANEL_MAX_NAMES);
            }
            // notes §11
            dial->names   = (dial->nameCount > 0) ? malloc((size_t)dial->nameCount * PANEL_LABEL_LEN) : NULL;

            if (dial->names) {
                memcpy(dial->names, names, (size_t)dial->nameCount * PANEL_LABEL_LEN);
            } else {
                dial->nameCount = 0;
            }
            dial->display = dialDisplayNames;
        } else if (strcmp(key, "display") == 0) {
            if (strcmp(val, "raw") == 0) {
                dial->display = dialDisplayRaw;
            } else if (strcmp(val, "ccnative") == 0) {
                dial->display = dialDisplayCcNative;
            } else if (strcmp(val, "hiLo") == 0) {
                dial->display = dialDisplaySignedHiLo;
            } else if (strcmp(val, "note") == 0) {
                dial->display = dialDisplayNote;
            } else if (strcmp(val, "signed") == 0) {
                dial->display = dialDisplaySigned;
            } else {
                LOG_ERROR("panelConfig line %u: unknown display '%s'\n", lineNo, val);
            }
        } else if (strcmp(key, "offset") == 0) {
            dial->storageOffset = (int32_t)strtol(val, NULL, 0);
        } else if (strcmp(key, "displayOffset") == 0) {
            dial->displayOffset = (int32_t)strtol(val, NULL, 0);
        } else if (strcmp(key, "group") == 0) {
            dial->paramGroup = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "param") == 0) {
            dial->paramId = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "typ") == 0) {
            dial->kronosTyp      = (uint32_t)strtoul(val, NULL, 0);
            dial->hasKronosParam = true; // typ= is the one attribute of the five that marks this a Kronos-param dial at all — see hasKronosParam's own comment, panelConfig.h
        } else if (strcmp(key, "soc") == 0) {
            dial->kronosSoc = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "sub") == 0) {
            dial->kronosSub = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "pid") == 0) {
            dial->kronosPid = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "idx") == 0) {
            dial->kronosIdx = (uint32_t)strtoul(val, NULL, 0);
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
        } else if (strcmp(key, "dumpNativeMax") == 0) {
            dial->dumpNativeMax = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "dumpInvert") == 0) {
            dial->dumpInvert = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "dumpSigned") == 0) {
            dial->dumpSigned = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "col") == 0) {
            dial->gridCol = strtod(val, NULL);
        } else if (strcmp(key, "row") == 0) {
            dial->gridRow = strtod(val, NULL);
        } else if (strcmp(key, "noLabel") == 0) {
            dial->noLabel = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "readOnly") == 0) {
            dial->readOnly = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "asDial") == 0) {
            dial->asDial = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "asMenu") == 0) {
            dial->asMenu = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "wireSigned") == 0) {
            dial->wireSigned = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "linkedMaxDial") == 0) {
            strncpy(dial->linkedMaxDialId, val, sizeof(dial->linkedMaxDialId) - 1);
        } else if (strcmp(key, "linkedMinDial") == 0) {
            strncpy(dial->linkedMinDialId, val, sizeof(dial->linkedMinDialId) - 1);
        } else if (strcmp(key, "disableUnless") == 0) {
            // notes §3
            const char * colon = strchr(val, ':');

            if (colon) {
                size_t idLen = (size_t)(colon - val);

                if (idLen >= sizeof(dial->disabledUnlessDialId)) {
                    idLen = sizeof(dial->disabledUnlessDialId) - 1;
                }
                memcpy(dial->disabledUnlessDialId, val, idLen);
                dial->disabledUnlessDialId[idLen] = '\0';
                dial->disabledUnlessValue         = (uint32_t)strtoul(colon + 1, NULL, 0);
            } else {
                LOG_ERROR("panelConfig line %u: disableUnless missing ':<value>' — '%s'\n", lineNo, val);
            }
        } else if (strcmp(key, "hiLoOffset") == 0) {
            dial->hiLoOffset = (int32_t)strtol(val, NULL, 0);
        } else if (strcmp(key, "hiLoCoarseScale") == 0) {
            dial->hiLoCoarseScale = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "hiLoFineScale") == 0) {
            dial->hiLoFineScale = (uint32_t)strtoul(val, NULL, 0);
        } else {
            LOG_ERROR("panelConfig line %u: unknown dial attribute '%s'\n", lineNo, key);
        }
    }
}

// ── "color <name> <r> <g> <b>" ───────────────────────────────────────────────
// notes §13
static void parse_graph_points(tPanelGraph * graph, const char * spec, uint32_t lineNo) {
    char         item[PANEL_TOKEN_LEN];
    const char * p = spec;

    graph->pointCount = 0;

    while (*p && (graph->pointCount < PANEL_GRAPH_MAX_POINTS)) {
        tGraphPointSpec * point = &graph->points[graph->pointCount++];
        const char *      end   = strchr(p, ';');
        size_t            len   = end ? (size_t)(end - p) : strlen(p);

        memset(point, 0, sizeof(*point));
        snprintf(item, sizeof(item), "%.*s", (int)len, p);

        for (char * part = strtok(item, ","); part; part = strtok(NULL, ",")) {
            char *       eq  = strchr(part, '=');

            if (!eq) {
                LOG_ERROR("panelConfig line %u: graph point part '%s' has no '='\n", lineNo, part);
                continue;
            }
            *eq = '\0';
            const char * val = eq + 1;
            bool         num = (*val == '-') || (*val == '.') || ((*val >= '0') && (*val <= '9'));

            if (strcmp(part, "x") == 0) {
                point->xSegments = strtod(val, NULL);
            } else if (strcmp(part, "t") == 0) {
                strncpy(point->tDial, val, sizeof(point->tDial) - 1);
            } else if (strcmp(part, "X") == 0) {
                strncpy(point->xDial, val, sizeof(point->xDial) - 1);
            } else if ((strcmp(part, "y") == 0) && num) {
                point->yConst = strtod(val, NULL);
            } else if (strcmp(part, "y") == 0) {
                strncpy(point->yDial, val, sizeof(point->yDial) - 1);
            } else {
                LOG_ERROR("panelConfig line %u: unknown graph point part '%s'\n", lineNo, part);
            }
        }

        p += len + (end ? 1 : 0);
    }
}

static void parse_graph_line(tPanelGraph * graph, char tokens[][PANEL_TOKEN_LEN], uint32_t tokenCount, uint32_t lineNo) {
    graph->present  = true;
    graph->width    = 400.0;
    graph->height   = 100.0;
    graph->segments = 1;

    for (uint32_t i = 1; i < tokenCount; i++) {
        char key[64];
        char val[PANEL_TOKEN_LEN];

        if (!split_kv(tokens[i], key, sizeof(key), val, sizeof(val))) {
            LOG_ERROR("panelConfig line %u: expected key=value in graph, got '%s'\n", lineNo, tokens[i]);
            continue;
        }

        if (strcmp(key, "width") == 0) {
            graph->width = strtod(val, NULL);
        } else if (strcmp(key, "height") == 0) {
            graph->height = strtod(val, NULL);
        } else if (strcmp(key, "segments") == 0) {
            graph->segments = (uint32_t)strtoul(val, NULL, 0);
        } else if (strcmp(key, "readOnly") == 0) {
            graph->readOnly = (strtoul(val, NULL, 0) != 0);
        } else if (strcmp(key, "points") == 0) {
            parse_graph_points(graph, val, lineNo);
        } else {
            LOG_ERROR("panelConfig line %u: unknown graph key '%s'\n", lineNo, key);
        }
    }

    if (graph->segments == 0) {
        graph->segments = 1;
    }
}

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
    } else if (strcmp(keyword, "bankSelect") == 0) {
        // notes §12
        if ((tokenCount < 4) || (config->bankCount >= PANEL_MAX_BANKS)) {
            LOG_ERROR("panelConfig line %u: expected 'bankSelect <name> <msb> <lsb>' (max %u)\n", lineNo, (unsigned)PANEL_MAX_BANKS);
            return;
        }
        tBankSelect * bank = &config->banks[config->bankCount++];

        strncpy(bank->name, tokens[1], sizeof(bank->name) - 1);
        bank->msb           = (int32_t)strtol(tokens[2], NULL, 0);
        bank->lsb           = (int32_t)strtol(tokens[3], NULL, 0);
        bank->declaredMsb   = bank->msb;
        bank->declaredLsb   = bank->lsb;
        bank->msbDumpOffset = -1;
        bank->lsbDumpOffset = -1;
    } else if (strcmp(keyword, "bankMapRequest") == 0) {
        config->bankMapRequestLen = 0;

        for (uint32_t b = 1; (b < tokenCount) && (config->bankMapRequestLen < sizeof(config->bankMapRequest)); b++) {
            config->bankMapRequest[config->bankMapRequestLen++] = (uint8_t)strtoul(tokens[b], NULL, 16);
        }
    } else if (strcmp(keyword, "bankMapReply") == 0) {
        if (tokenCount < 3) {
            LOG_ERROR("panelConfig line %u: expected 'bankMapReply <func> <sub> <msbOffset> <lsbOffset> ...'\n", lineNo);
            return;
        }
        config->bankMapReplyFunc = (int32_t)strtol(tokens[1], NULL, 16);
        config->bankMapReplySub  = (int32_t)strtol(tokens[2], NULL, 16);

        for (uint32_t b = 0; (b < config->bankCount) && (3 + (b * 2) + 1 < tokenCount); b++) {
            config->banks[b].msbDumpOffset = (int32_t)strtol(tokens[3 + (b * 2)], NULL, 0);
            config->banks[b].lsbDumpOffset = (int32_t)strtol(tokens[4 + (b * 2)], NULL, 0);
        }
    } else if (strcmp(keyword, "dumpBlock") == 0) {
        // notes §14
        uint32_t     replyAt = 0;

        for (uint32_t t = 2; t < tokenCount; t++) {
            if (strcmp(tokens[t], "reply") == 0) {
                replyAt = t;
            }
        }

        if ((tokenCount < 6) || (replyAt < 3) || (replyAt + 2 >= tokenCount) || (config->dumpBlockCount >= PANEL_MAX_DUMP_BLOCKS)) {
            LOG_ERROR("panelConfig line %u: expected 'dumpBlock <name> <request bytes> reply <func> <sub>' (max %u)\n", lineNo, (unsigned)PANEL_MAX_DUMP_BLOCKS);
            return;
        }
        tDumpBlock * block   = &config->dumpBlocks[config->dumpBlockCount++];

        strncpy(block->name, tokens[1], sizeof(block->name) - 1);

        for (uint32_t t = 2; (t < replyAt) && (block->requestLen < sizeof(block->request)); t++) {
            block->request[block->requestLen++] = (uint8_t)strtoul(tokens[t], NULL, 16);
        }

        block->replyFunc = (int32_t)strtol(tokens[replyAt + 1], NULL, 16);
        block->replySub  = (int32_t)strtol(tokens[replyAt + 2], NULL, 16);
    } else if (strcmp(keyword, "fromDump") == 0) {
        if (!*currentSection || (tokenCount < 2)) {
            LOG_ERROR("panelConfig line %u: expected 'fromDump <dump block>' inside a page\n", lineNo);
            return;
        }

        for (uint32_t b = 0; b < config->dumpBlockCount; b++) {
            if (strcmp(config->dumpBlocks[b].name, tokens[1]) == 0) {
                (*currentSection)->dumpBlock = (int32_t)b;
            }
        }

        if ((*currentSection)->dumpBlock < 0) {
            LOG_ERROR("panelConfig line %u: no earlier dumpBlock named '%s'\n", lineNo, tokens[1]);
        }
    } else if (strcmp(keyword, "startupProgram") == 0) {
        if (tokenCount < 3) {
            LOG_ERROR("panelConfig line %u: expected 'startupProgram <bank index> <program 0-127>'\n", lineNo);
            return;
        }
        config->startupSlot = ((int32_t)strtol(tokens[1], NULL, 0) * 128) + (int32_t)strtol(tokens[2], NULL, 0);
    } else if (strcmp(keyword, "programChangeTransmit") == 0) {
        if (tokenCount < 4) {
            LOG_ERROR("panelConfig line %u: expected 'programChangeTransmit <offset> <shift> <mask>'\n", lineNo);
            return;
        }
        config->pcTransmitOffset = (int32_t)strtol(tokens[1], NULL, 0);
        config->pcTransmitShift  = (uint32_t)strtoul(tokens[2], NULL, 0);
        config->pcTransmitMask   = (uint32_t)strtoul(tokens[3], NULL, 0);
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
    } else if (strcmp(keyword, "supportsKorgProgramDump") == 0) {
        config->supportsKorgProgramDump = (strcmp(tokens[1], "no") != 0);
    } else if (strcmp(keyword, "productId") == 0) {
        config->productId = (uint8_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "list") == 0) {
        if (tokenCount < 3) {
            LOG_ERROR("panelConfig line %u: expected 'list <name> <items>'\n", lineNo);
            return;
        }
        // notes §10
        int32_t      index = find_list_index(config, tokens[1]);

        if (index < 0) {
            if (config->listCount >= PANEL_MAX_LISTS) {
                LOG_ERROR("panelConfig line %u: too many lists (max %u)\n", lineNo, (unsigned)PANEL_MAX_LISTS);
                return;
            }
            index = (int32_t)config->listCount++;
            strncpy(config->lists[index].name, tokens[1], sizeof(config->lists[index].name) - 1);
        }
        tPanelList * list  = &config->lists[index];

        list->itemCount += split_csv(tokens[2], list->items + list->itemCount, PANEL_MAX_LIST_ITEMS - list->itemCount);
    } else if (strcmp(keyword, "page") == 0) {
        if (config->sectionCount >= PANEL_MAX_SECTIONS) {
            LOG_ERROR("panelConfig line %u: too many sections (max %u)\n", lineNo, (unsigned)PANEL_MAX_SECTIONS);
            return;
        }
        *currentSection                 = &config->sections[config->sectionCount++];
        *pendingGap                     = 0.0;
        (*currentSection)->showIfOffset = -1;
        (*currentSection)->dumpBlock    = -1;
        join_tokens(tokens, 1, tokenCount, (*currentSection)->page, sizeof((*currentSection)->page));
        normalise_page_path((*currentSection)->page);
    } else if (strcmp(keyword, "modeTab") == 0) {
        if ((tokenCount < 3) || (config->modeTabCount >= PANEL_MAX_MODE_TABS)) {
            LOG_ERROR("panelConfig line %u: expected 'modeTab <mode> <top-level tab>' (max %u)\n", lineNo, (unsigned)PANEL_MAX_MODE_TABS);
            return;
        }
        tModeTab * modeTab = &config->modeTabs[config->modeTabCount++];

        modeTab->mode = (uint32_t)strtoul(tokens[1], NULL, 0);
        join_tokens(tokens, 2, tokenCount, modeTab->tab, sizeof(modeTab->tab));
    } else if (strcmp(keyword, "showIf") == 0) {
        if (!*currentSection || (tokenCount < 3)) {
            LOG_ERROR("panelConfig line %u: expected 'showIf <dumpOffset> <value>' inside a page\n", lineNo);
            return;
        }
        (*currentSection)->showIfOffset = (int32_t)strtol(tokens[1], NULL, 0);
        (*currentSection)->showIfValue  = (uint32_t)strtoul(tokens[2], NULL, 0);
    } else if (strcmp(keyword, "graph") == 0) {
        if (!*currentSection) {
            LOG_ERROR("panelConfig line %u: 'graph' outside a section\n", lineNo);
            return;
        }
        parse_graph_line(&(*currentSection)->graph, tokens, tokenCount, lineNo);
    } else if (strcmp(keyword, "variantParamDelta") == 0) {
        if (!*currentSection || (tokenCount < 2)) {
            LOG_ERROR("panelConfig line %u: expected 'variantParamDelta <delta>' inside a page\n", lineNo);
            return;
        }
        (*currentSection)->hasVariantParamDelta = true;
        (*currentSection)->variantParamDelta    = (int32_t)strtol(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "tabLabel") == 0) {
        if ((tokenCount < 4) || (config->tabLabelCount >= PANEL_MAX_TAB_LABELS)) {
            LOG_ERROR("panelConfig line %u: expected 'tabLabel \"<tab>\" <dumpOffset> <names>' (max %u)\n", lineNo, (unsigned)PANEL_MAX_TAB_LABELS);
            return;
        }
        tTabLabel * tabLabel = &config->tabLabels[config->tabLabelCount++];

        strncpy(tabLabel->tab, tokens[1], sizeof(tabLabel->tab) - 1);
        normalise_page_path(tabLabel->tab);
        tabLabel->dumpOffset = (int32_t)strtol(tokens[2], NULL, 0);
        tabLabel->nameCount  = split_csv(tokens[3], tabLabel->names, PANEL_MAX_NAMES);
    } else if (strcmp(keyword, "pageVariant") == 0) {
        if ((tokenCount < 5) || (config->pageVariantCount >= PANEL_MAX_PAGE_VARIANTS)) {
            LOG_ERROR("panelConfig line %u: expected 'pageVariant \"<variant>\" \"<base>\" <typDelta> <dumpDelta> [paramDelta]' (max %u)\n", lineNo, (unsigned)PANEL_MAX_PAGE_VARIANTS);
            return;
        }
        tPageVariant * variant = &config->pageVariants[config->pageVariantCount++];

        strncpy(variant->variant, tokens[1], sizeof(variant->variant) - 1);
        strncpy(variant->base, tokens[2], sizeof(variant->base) - 1);
        normalise_page_path(variant->variant);
        normalise_page_path(variant->base);
        variant->typDelta     = (int32_t)strtol(tokens[3], NULL, 0);
        variant->dumpDelta    = (int32_t)strtol(tokens[4], NULL, 0);
        variant->paramDelta   = (tokenCount > 5) ? (int32_t)strtol(tokens[5], NULL, 0) : 0;
        variant->showIfOffset = -1;
    } else if (strcmp(keyword, "variantShowIf") == 0) {
        tPageVariant * variant = NULL;
        char           name[PANEL_PAGE_LEN];

        if (tokenCount < 5) {
            LOG_ERROR("panelConfig line %u: expected 'variantShowIf \"<variant>\" <dumpOffset> <min> <max>'\n", lineNo);
            return;
        }
        strncpy(name, tokens[1], sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        normalise_page_path(name);

        for (uint32_t v = 0; v < config->pageVariantCount; v++) {
            if (strcmp(config->pageVariants[v].variant, name) == 0) {
                variant = &config->pageVariants[v];
            }
        }

        if (!variant) {
            LOG_ERROR("panelConfig line %u: variantShowIf names no earlier pageVariant '%s'\n", lineNo, tokens[1]);
            return;
        }
        variant->showIfOffset  = (int32_t)strtol(tokens[2], NULL, 0);
        variant->showIfMin     = (int32_t)strtol(tokens[3], NULL, 0);
        variant->showIfMax     = (int32_t)strtol(tokens[4], NULL, 0);
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
        parse_dial_line(config, *currentSection, pendingGap, tokens, tokenCount, lineNo);
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

        // notes §4
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
    config->supportsIdentity        = true;  // overridden by an explicit "identityQuery no" line
    config->supportsKorgProgramDump = true;  // overridden by an explicit "supportsKorgProgramDump no" line — see its own field comment in panelConfig.h
    config->panelNameOffset         = -1;    // overridden by an explicit "panelNameOffset" line
    config->presetNameOffset        = -1;    // overridden by an explicit "presetNameOffset" line
    config->bankMapReplyFunc        = -1;
    config->pcTransmitOffset        = -1;
    config->startupSlot             = -1;
    config->presetBankCount         = 1;     // overridden by an explicit "presetBankCount" line — see its own field comment in panelConfig.h

    tPanelSection * currentSection = NULL;
    double          pendingGap     = 0.0;
    char            line[PANEL_LINE_LEN];
    uint32_t        lineNo         = 0;

    while (fgets(line, sizeof(line), file)) {
        lineNo++;

        size_t len = strlen(line);

        // notes §5
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

tPanelDial * find_panel_dial_by_kronos_param(tPanelConfig * config, uint32_t typ, uint32_t soc, uint32_t sub, uint32_t pid, uint32_t idx) {
    for (uint32_t s = 0; s < config->sectionCount; s++) {
        tPanelSection * section = &config->sections[s];

        for (uint32_t i = 0; i < section->dialCount; i++) {
            tPanelDial * dial = &section->dials[i];

            if (  dial->hasKronosParam && (dial->kronosTyp == typ) && (dial->kronosSoc == soc)
               && (dial->kronosSub == sub) && (dial->kronosPid == pid) && (dial->kronosIdx == idx)) {
                return dial;
            }
        }
    }

    return NULL;
}

tPanelDial * find_panel_dial_by_label(tPanelConfig * config, const char * label) {
    for (uint32_t s = 0; s < config->sectionCount; s++) {
        tPanelSection * section = &config->sections[s];

        for (uint32_t i = 0; i < section->dialCount; i++) {
            if (strcasecmp(section->dials[i].label, label) == 0) {
                return &section->dials[i];
            }
        }
    }

    return NULL;
}

// notes §6
tRectangle panel_dial_hit_rect(const tPanelDial * dial) {
    if (dial == NULL) {
        return (tRectangle){
            {
                0.0, 0.0
            }, {
                0.0, 0.0
            }
        };
    }

    // The same condition synthGraphics.c draws on — a binary dial or one carrying a value menu is
    // drawn with draw_button(); anything else is drawn with render_dial(), exactly at its rect.
    if (panel_dial_is_binary(dial) || panel_dial_needs_value_menu(dial)) {
        return draw_button_bounds(dial->rect);
    }
    return dial->rect;
}

int32_t hit_test_panel_section(tPanelSection * section, tCoord point) {
    for (uint32_t i = 0; i < section->dialCount; i++) {
        if (within_rectangle(point, panel_dial_hit_rect(&section->dials[i]))) {
            return (int32_t)i;
        }
    }

    return -1;
}

bool panel_dial_is_toggle(const tPanelDial * dial) {
    // notes §7
    return dial
           && (dial->display == dialDisplayNames)
           && (dial->nameCount == 2)
           && (strcasecmp(dial->names[0], "Off") == 0)
           && (strcasecmp(dial->names[1], "On") == 0);
}

bool panel_dial_is_binary(const tPanelDial * dial) {
    return dial && (dial->display == dialDisplayNames) && (dial->nameCount == 2);
}

bool panel_dial_needs_value_menu(const tPanelDial * dial) {
    // notes §8
    return dial
           && (dial->display == dialDisplayNames)
           && ((dial->nameCount > 2) || dial->asMenu)
           && (dial->ccNumber == 0)
           && ((dial->dumpBitWidth > 0) || (dial->paramId != 0))
           && !dial->asDial;
}

bool panel_dial_is_disabled(const tPanelDial * dial, tPanelConfig * config) {
    if (!dial || (dial->disabledUnlessDialId[0] == '\0')) {
        return false;
    }
    tPanelDial * gate = find_panel_dial_anywhere(config, dial->disabledUnlessDialId);

    return gate && (get_panel_dial_value(gate) != dial->disabledUnlessValue);
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

const char * panel_mode_tab_name(const tPanelConfig * config, uint32_t mode) {
    for (uint32_t i = 0; i < config->modeTabCount; i++) {
        if (config->modeTabs[i].mode == mode) {
            return config->modeTabs[i].tab;
        }
    }

    return NULL;
}

int32_t panel_mode_for_tab(const tPanelConfig * config, const char * tab) {
    for (uint32_t i = 0; i < config->modeTabCount; i++) {
        if (strcmp(config->modeTabs[i].tab, tab) == 0) {
            return (int32_t)config->modeTabs[i].mode;
        }
    }

    return -1;
}

bool panel_page_is_under(const char * page, const char * prefix) {
    size_t len = strlen(prefix);

    return (strncmp(page, prefix, len) == 0) && ((page[len] == '\0') || (page[len] == PANEL_PAGE_SEPARATOR));
}

tPanelSection * panel_section_of_dial(tPanelConfig * config, const tPanelDial * dial) {
    for (uint32_t s = 0; s < config->sectionCount; s++) {
        tPanelSection * section = &config->sections[s];

        if ((dial >= &section->dials[0]) && (dial < &section->dials[section->dialCount])) {
            return section;
        }
    }

    return NULL;
}
