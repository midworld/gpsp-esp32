/* Generic HID gamepad layout from a report descriptor.
 *
 * Copyright (C) 2026 Midworld Kim
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hid_gamepad.h"

#include <new>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"
/* A system include keeps the unmodified header out of project warnings. */
#include <hid_report_parser.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

enum field_kind
{
    FIELD_VALUE, FIELD_ARRAY, FIELD_HAT, FIELD_X, FIELD_Y,
};

#define BUTTON_HOME 16
#define BUTTON_NONE 0xff
#define PAGE_DESKTOP 0x01
#define PAGE_BUTTON 0x09
#define PAGE_CONSUMER 0x0c

/* HID numbers buttons without names. Unknown pads use one fixed order;
   the Controls menu remaps it. Home needs a Home usage. */
static const uint8_t s_sequential[] = {
    RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A,
    RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
    RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
    RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
    RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3,
};

/* DualShock 4 and DualSense report 0x01: square, cross, circle, triangle,
   ..., PS. */
static const uint8_t s_sony[] = {
    RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_B,
    RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
    RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2,
    RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
    RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3, BUTTON_HOME,
};

static unsigned button_id(const struct gpsp_hid_gamepad *pad, unsigned page,
        unsigned usage)
{
    if (page == PAGE_BUTTON && usage)
    {
        bool sony = pad->layout == GPSP_HID_LAYOUT_SONY;
        const uint8_t *table = sony ? s_sony : s_sequential;
        unsigned count = sony ? sizeof(s_sony) : sizeof(s_sequential);

        return usage <= count ? table[usage - 1] : BUTTON_NONE;
    }
    if (page == PAGE_DESKTOP)
        switch (usage)
        {
            case 0x3d: return RETRO_DEVICE_ID_JOYPAD_START;
            case 0x3e: return RETRO_DEVICE_ID_JOYPAD_SELECT;
            case 0x85: return BUTTON_HOME;
            case 0x90: return RETRO_DEVICE_ID_JOYPAD_UP;
            case 0x91: return RETRO_DEVICE_ID_JOYPAD_DOWN;
            case 0x92: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
            case 0x93: return RETRO_DEVICE_ID_JOYPAD_LEFT;
        }
    if (page == PAGE_CONSUMER && usage == 0x223)
        return BUTTON_HOME;
    return BUTTON_NONE;
}

namespace {

/* Collects the controls of joystick and gamepad collections into the
   caller's bounded table. Bit offsets follow every input field. */
class Mapper : public hid::DescriptorParser::EventHandler
{
public:
    struct gpsp_hid_gamepad *pad;
    uint32_t offsets[256];
    uint32_t app_depth;
    bool x, y, hat;

protected:
    int Field(const hid::DescriptorParser::FieldParams &fp) override
    {
        uint8_t id = fp.globals->report_id;
        uint32_t offset = offsets[id];

        if (fp.report_type != hid::ReportType::input)
            return 0;
        offsets[id] += fp.bit_size;
        /* Relative values are motion, not positions or presses. */
        if (!app_depth || (fp.flags & (hid::FLAG_FIELD_CONST | hid::FLAG_FIELD_RELATIVE)))
            return 0;
        if (fp.flags & hid::FLAG_FIELD_VARIABLE)
            AddValues(fp, offset);
        else
            AddArray(fp, offset);
        return 0;
    }

    int Padding(hid::ReportType rt, uint8_t report_id, uint32_t bit_size) override
    {
        if (rt == hid::ReportType::input)
            offsets[report_id] += bit_size;
        return 0;
    }

    int BeginCollection(uint8_t type, uint16_t page, uint16_t usage,
            uint32_t depth) override
    {
        if (!app_depth && type == hid::COLLECTION_TYPE_APPLICATION &&
                page == PAGE_DESKTOP && (usage == 0x04 || usage == 0x05))
            app_depth = depth;
        return 0;
    }

    int EndCollection(uint32_t depth) override
    {
        if (depth == app_depth)
            app_depth = 0;
        return 0;
    }

private:
    void Drop()
    {
        if (pad->dropped < 0xff)
            pad->dropped++;
    }

    void Add(const hid::DescriptorParser::FieldParams &fp, uint32_t offset,
            uint8_t kind, uint16_t page, uint16_t usage, uint16_t usage_max)
    {
        const hid::DescriptorParser::Globals *g = fp.globals;
        uint32_t size = g->report_size;

        if (size > 32 || offset + size > GPSP_HID_GAMEPAD_REPORT_BYTES * 8)
            return;
        if (pad->field_count == GPSP_HID_GAMEPAD_FIELDS)
        {
            Drop();
            return;
        }
        unsigned slot = 0;

        while (slot < pad->report_count && pad->reports[slot] != g->report_id)
            slot++;
        if (slot == GPSP_HID_GAMEPAD_REPORTS)
        {
            Drop();
            return;
        }
        if (slot == pad->report_count)
            pad->reports[pad->report_count++] = g->report_id;
        if (pad->bits[slot] < offset + size)
            pad->bits[slot] = offset + size;
        pad->fields[pad->field_count++] = (struct gpsp_hid_gamepad_field){
            .offset = offset, .min = g->logical_min, .max = g->logical_max,
            .page = page, .usage = usage, .usage_max = usage_max,
            .report = g->report_id, .size = (uint8_t)size, .kind = kind,
            .slot = (uint8_t)slot,
        };
    }

    static bool Hat(const hid::DescriptorParser::Globals *g)
    {
        int64_t range = (int64_t)g->logical_max - g->logical_min;

        return g->report_size <= 8 && (range == 3 || range == 7);
    }

    void AddValues(const hid::DescriptorParser::FieldParams &fp, uint32_t offset)
    {
        const hid::UsageRange *range = fp.usage_ranges;
        const hid::UsageRange *last = fp.usage_ranges + fp.num_usage_ranges - 1;
        uint32_t usage = range->usage_min;

        /* Controls past the last usage repeat it; they add nothing. */
        for (uint32_t i = 0; i < fp.globals->report_count;
                i++, offset += fp.globals->report_size)
        {
            uint16_t page = range->usage_page;

            if (page == PAGE_DESKTOP && usage == 0x30 && !x)
                x = true, Add(fp, offset, FIELD_X, page, usage, usage);
            else if (page == PAGE_DESKTOP && usage == 0x31 && !y)
                y = true, Add(fp, offset, FIELD_Y, page, usage, usage);
            else if (page == PAGE_DESKTOP && usage == 0x39 && !hat && Hat(fp.globals))
                hat = true, Add(fp, offset, FIELD_HAT, page, usage, usage);
            else if (button_id(pad, page, usage) != BUTTON_NONE)
                Add(fp, offset, FIELD_VALUE, page, usage, usage);
            else if (page == PAGE_BUTTON)
                Drop();
            if (usage < range->usage_max)
                usage++;
            else if (range < last)
                usage = (++range)->usage_min;
            else
                break;
        }
    }

    void AddArray(const hid::DescriptorParser::FieldParams &fp, uint32_t offset)
    {
        const hid::UsageRange *range = fp.usage_ranges;
        bool mapped = false;

        /* Array values index one usage range; others are not mapped. */
        if (fp.num_usage_ranges > 1)
            Drop();
        for (uint32_t usage = range->usage_min; usage <= range->usage_max && !mapped;
                usage++)
            mapped = button_id(pad, range->usage_page, usage) != BUTTON_NONE;
        if (!mapped)
            return;
        /* Each slot holds one pressed control. */
        for (uint32_t i = 0; i < fp.globals->report_count;
                i++, offset += fp.globals->report_size)
        {
            if (i == GPSP_HID_GAMEPAD_ARRAY_SLOTS)
            {
                Drop();
                break;
            }
            Add(fp, offset, FIELD_ARRAY, range->usage_page, range->usage_min,
                    range->usage_max);
        }
    }
};

}

static void *gamepad_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return calloc(1, size);
#endif
}

static void gamepad_free(void *memory)
{
#ifdef ESP_PLATFORM
    heap_caps_free(memory);
#else
    free(memory);
#endif
}

struct parse_work
{
    hid::DescriptorParser parser;
    Mapper mapper;
};

extern "C" enum gpsp_hid_gamepad_result gpsp_hid_gamepad_init(
        struct gpsp_hid_gamepad *pad, const uint8_t *descriptor, size_t size,
        enum gpsp_hid_gamepad_layout layout)
{
    *pad = (struct gpsp_hid_gamepad){};
    if (!descriptor || !size)
        return GPSP_HID_GAMEPAD_MALFORMED;
    void *memory = gamepad_alloc(sizeof(parse_work));

    if (!memory)
        return GPSP_HID_GAMEPAD_MALFORMED;
    parse_work *work = new (memory) parse_work();
    work->mapper.pad = pad;
    pad->layout = layout;
    int result = work->parser.Parse(descriptor, size, &work->mapper);
    bool ids = false;

    for (unsigned i = 1; i < 256; i++)
        ids |= work->mapper.offsets[i] != 0;
    work->~parse_work();
    gamepad_free(memory);
    if (result)
    {
        *pad = (struct gpsp_hid_gamepad){};
        return GPSP_HID_GAMEPAD_MALFORMED;
    }
    pad->report_ids = ids;
    if (!pad->field_count)
    {
        *pad = (struct gpsp_hid_gamepad){};
        return GPSP_HID_GAMEPAD_NOT_GAMEPAD;
    }
    return GPSP_HID_GAMEPAD_OK;
}

static uint32_t field_bits(const uint8_t *data, uint32_t offset, unsigned size)
{
    uint32_t value = 0;

    for (unsigned i = 0; i < size; i++, offset++)
        value |= (uint32_t)((data[offset >> 3] >> (offset & 7)) & 1) << i;
    return value;
}

static void press(unsigned *buttons, unsigned id)
{
    if (id != BUTTON_NONE)
        *buttons |= 1u << id;
}

static void decode_field(const struct gpsp_hid_gamepad *pad,
        const struct gpsp_hid_gamepad_field *f, const uint8_t *data,
        unsigned *buttons)
{
    uint32_t raw = field_bits(data, f->offset, f->size);
    int64_t value = raw;

    /* A negative minimum means two's complement values. */
    if (f->min < 0 && f->size < 32 && (raw & (1u << (f->size - 1))))
        value -= (int64_t)1 << f->size;
    else if (f->min < 0 && f->size == 32)
        value = (int32_t)raw;
    int64_t min = f->min, max = f->min < 0 ? f->max : (int64_t)(uint32_t)f->max;

    /* Values outside the logical range are null: no press or direction. */
    if (value < min || value > max)
        return;
    switch (f->kind)
    {
        case FIELD_VALUE:
            if (f->size == 1 ? value != 0 : value > min + (max - min) / 2)
                press(buttons, button_id(pad, f->page, f->usage));
            break;
        case FIELD_ARRAY:
            if (value - min <= f->usage_max - f->usage)
                press(buttons, button_id(pad, f->page, f->usage + (unsigned)(value - min)));
            break;
        case FIELD_HAT:
        {
            static const uint8_t eight[8] = {1, 9, 8, 10, 2, 6, 4, 5};
            static const uint8_t four[4] = {1, 8, 2, 4};
            int64_t step = value - min;
            unsigned bits = 0;

            if (step >= 0 && step < 8 && max - min == 7)
                bits = eight[step];
            else if (step >= 0 && step < 4 && max - min == 3)
                bits = four[step];
            if (bits & 1)
                press(buttons, RETRO_DEVICE_ID_JOYPAD_UP);
            if (bits & 2)
                press(buttons, RETRO_DEVICE_ID_JOYPAD_DOWN);
            if (bits & 4)
                press(buttons, RETRO_DEVICE_ID_JOYPAD_LEFT);
            if (bits & 8)
                press(buttons, RETRO_DEVICE_ID_JOYPAD_RIGHT);
            break;
        }
        case FIELD_X:
        case FIELD_Y:
        {
            /* Half of either side of center is the dead zone. */
            int64_t center = min + (max - min) / 2, zone = (max - min) / 4;

            if (max - min < 2)
                break;
            if (value < center - zone)
                press(buttons, f->kind == FIELD_X ? RETRO_DEVICE_ID_JOYPAD_LEFT :
                        RETRO_DEVICE_ID_JOYPAD_UP);
            else if (value > center + zone)
                press(buttons, f->kind == FIELD_X ? RETRO_DEVICE_ID_JOYPAD_RIGHT :
                        RETRO_DEVICE_ID_JOYPAD_DOWN);
            break;
        }
    }
}

extern "C" bool gpsp_hid_gamepad_decode(struct gpsp_hid_gamepad *pad,
        const uint8_t *report, size_t size, unsigned *buttons)
{
    uint8_t id = 0;
    unsigned slot = 0, state = 0;

    if (pad->report_ids)
    {
        if (!size)
            return false;
        id = *report++;
        size--;
    }
    while (slot < pad->report_count && pad->reports[slot] != id)
        slot++;
    if (slot == pad->report_count || size * 8 < pad->bits[slot])
        return false;
    for (unsigned i = 0; i < pad->field_count; i++)
        if (pad->fields[i].slot == slot)
            decode_field(pad, &pad->fields[i], report, &state);
    pad->state[slot] = state;
    *buttons = 0;
    for (unsigned i = 0; i < pad->report_count; i++)
        *buttons |= pad->state[i];
    return true;
}

extern "C" void gpsp_hid_gamepad_reset(struct gpsp_hid_gamepad *pad)
{
    memset(pad->state, 0, sizeof(pad->state));
}
