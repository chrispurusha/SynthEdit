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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "synthlibDefs.h"
#include "panelConfig.h"

#define PANEL_LINE_LEN      256
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
    dial->gapBefore = *pendingGap;
    *pendingGap     = 0.0;

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
    } else if (strcmp(keyword, "manufacturerId") == 0) {
        config->manufacturerId = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "familyId") == 0) {
        config->familyId = (uint32_t)strtoul(tokens[1], NULL, 0);
    } else if (strcmp(keyword, "memberId") == 0) {
        config->memberId = (uint32_t)strtoul(tokens[1], NULL, 0);
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

    tPanelSection * currentSection = NULL;
    double          pendingGap     = 0.0;
    char            line[PANEL_LINE_LEN];
    uint32_t        lineNo         = 0;

    while (fgets(line, sizeof(line), file)) {
        lineNo++;

        size_t len = strlen(line);

        while ((len > 0) && ((line[len - 1] == '\n') || (line[len - 1] == '\r'))) {
            line[--len] = '\0';
        }
        process_line(config, &currentSection, &pendingGap, line, lineNo);
    }
    fclose(file);
    return true;
}

void layout_panel_section(tPanelSection * section, tRectangle origin) {
    double x = origin.coord.x;

    for (uint32_t i = 0; i < section->dialCount; i++) {
        tPanelDial * dial = &section->dials[i];

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

int32_t hit_test_panel_section(tPanelSection * section, tCoord point) {
    for (uint32_t i = 0; i < section->dialCount; i++) {
        if (within_rectangle(point, section->dials[i].rect)) {
            return (int32_t)i;
        }
    }

    return -1;
}
