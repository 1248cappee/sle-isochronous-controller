#include "cis_radio.h"
#include <string.h>

static bool enabled_channel(const uint8_t map[10], unsigned ch)
{
    return (map[ch / 8] & (1u << (ch % 8))) != 0;
}

static uint8_t reverse_byte(uint8_t x)
{
    uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i)
        result = (uint8_t)((result << 1) | ((x >> i) & 1u));
    return result;
}

/* 6.10.3.2/.3: key from the negotiated FIRST sender's actual logical ID.
   The BIS base-ID/group XOR rule does not apply to unicast. */
static uint8_t event_channel(const cis_radio *r, uint32_t slot)
{
    uint32_t id = r->cfg.dir[0].link_id;
    uint16_t key = (uint16_t)(id ^ (id >> 16));
    uint16_t random = (uint16_t)(slot ^ key);
    for (unsigned i = 0; i < 3; ++i) {
        uint16_t reversed = (uint16_t)((reverse_byte((uint8_t)(random >> 8)) << 8) |
                                        reverse_byte((uint8_t)random));
        random = (uint16_t)(17u * reversed + key);
    }
    random ^= key;
    uint8_t channel = (uint8_t)(random % 76u);
    if (enabled_channel(r->channel_map, channel)) return channel;
    uint8_t usable[76];
    unsigned count = 0;
    for (channel = 0; channel < 76; ++channel)
        if (enabled_channel(r->channel_map, channel)) usable[count++] = channel;
    return usable[((uint32_t)random * count) >> 16];
}

bool cis_radio_init(cis_radio *r, sle_sch *sch, cis_radio_port port,
                    const uint8_t map[10], bool is_g)
{
    if (!r || !sch || !map || !port.submit || !port.cancel_and_drain) return false;
    unsigned ch = 0;
    while (ch < 76 && !enabled_channel(map, ch)) ++ch;
    if (ch == 76) return false;
    *r = (cis_radio){.sch = sch, .port = port, .is_g = is_g};
    memcpy(r->channel_map, map, sizeof(r->channel_map));
    return true;
}

static void retire(cis_radio *r, bool abort)
{
    if (!r->pending) return;
    if (abort) r->port.cancel_and_drain(r->port.ctx, r->active.token);
    sle_release(r->sch, r, r->active.token);
    r->pending = false;
}

void cis_radio_stop(cis_radio *r)
{
    retire(r, true);
    r->enabled = false;
}

bool cis_radio_apply(cis_radio *r, const cis_params *c, uint64_t anchor_us, uint32_t anchor_slot)
{
    if (!r || !r->sch || !c || anchor_slot > CIS_SLOT_MASK ||
        c->slot_us < 25 || c->slot_us > 125 || c->slot_us % 25 != 0 ||
        !c->group_slots || !c->events || !c->event_slots ||
        (uint32_t)(c->events - 1) * c->event_slots >= c->group_slots)
        return false;
    for (unsigned d = 0; d < 2; ++d) {
        if (c->dir[d].frame != 0 || c->dir[d].bandwidth != 0 ||
            c->dir[d].link_id > 0xffffffu || !c->dir[d].max_offset_slots ||
            c->dir[d].max_offset_slots > 512)
            return false;
    }
    cis_radio_stop(r);
    r->cfg = *c;
    r->anchor_us = anchor_us;
    r->anchor_slot = anchor_slot;
    r->next = 0;
    r->enabled = true;
    return true;
}

cis_radio_result cis_radio_tick(cis_radio *r, uint64_t now)
{
    if (!r->enabled) return RADIO_IDLE;
    if (r->pending && now >= r->active.end_us) {
        retire(r, true);
        return RADIO_TIMEOUT;
    }
    return RADIO_OK;
}

cis_radio_result cis_radio_finish(cis_radio *r, uint64_t token)
{
    if (!r->pending || token != r->active.token) return RADIO_STALE;
    retire(r, false);
    return RADIO_OK;
}

cis_radio_result cis_radio_schedule(cis_radio *r, uint64_t now, uint32_t first_air,
                                    uint32_t second_air)
{
    cis_radio_result tick = cis_radio_tick(r, now);
    if (tick != RADIO_OK) return tick;
    if (r->pending) return RADIO_BUSY;
    const cis_params *c = &r->cfg;
    if (!first_air || !second_air ||
        first_air > c->dir[0].max_offset_slots * 125u ||
        second_air > c->dir[1].max_offset_slots * 125u)
        return RADIO_INVALID;

    uint64_t period = (uint64_t)c->group_slots * c->slot_us;
    uint64_t step = (uint64_t)c->event_slots * c->slot_us;
    if (now > r->anchor_us) {
        uint64_t elapsed = now - r->anchor_us;
        uint64_t index = (elapsed % period + step - 1) / step;
        if (index > c->events) index = c->events;
        uint64_t earliest = elapsed / period * c->events + index;
        if (earliest > r->next) {
            r->next = earliest;
            return RADIO_MISSED;
        }
    }

    uint64_t cycle = r->next / c->events;
    uint8_t index = (uint8_t)(r->next % c->events);
    uint64_t offset = index * step;
    uint32_t duration = first_air + c->intra_us + second_air;
    uint64_t until_next = index + 1u < c->events ? step : period - offset;
    if ((uint64_t)duration + c->inter_us > until_next ||
        cycle > (UINT64_MAX - r->anchor_us) / period ||
        r->token_sequence == UINT64_MAX) return RADIO_INVALID;
    uint64_t base = r->anchor_us + cycle * period;
    if (UINT64_MAX - base < offset + duration) return RADIO_INVALID;
    uint64_t start = base + offset;
    uint32_t slot = (r->anchor_slot + (uint32_t)cycle * c->group_slots +
                     (uint32_t)index * c->event_slots) & CIS_SLOT_MASK;
    cis_radio_event e = {
        .token = ++r->token_sequence, .start_us = start,
        .second_us = start + first_air + c->intra_us, .end_us = start + duration,
        .first_air_us = first_air, .second_air_us = second_air, .cycle = cycle,
        .slot = slot, .index = index, .channel = event_channel(r, slot),
        .first_tx = r->is_g != c->t_sends_first
    };
    for (unsigned d = 0; d < 2; ++d) {
        e.link_id[d] = c->dir[d].link_id;
        e.crc32[d] = c->dir[d].crc32;
        e.crc_seed[d] = c->dir[d].crc_seed & (e.crc32[d] ? UINT32_MAX : UINT32_C(0xffffff));
    }
    ++r->next;
    if (!sle_reserve(r->sch, r, e.token, start, duration)) return RADIO_CONFLICT;
    if (!r->port.submit(r->port.ctx, &e)) {
        sle_release(r->sch, r, e.token);
        return RADIO_IO;
    }
    r->active = e;
    r->pending = true;
    return RADIO_OK;
}
