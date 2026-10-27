#include "indicator_alloc.h"

static bool indicator_reserved(int slot)
{
    return slot == NPP_INDICATOR_INCR || slot == NPP_INDICATOR_LINK;
}

bool npp_indicator_alloc(int *next, int count, int *out)
{
    if (out) *out = 0;
    if (!next || !out || count < 1) return false;
    int start = *next;
    if (start < NPP_INDICATOR_FIRST) start = NPP_INDICATOR_FIRST;
    while (start <= NPP_INDICATOR_LAST) {
        if (indicator_reserved(start)) {
            ++start;
            continue;
        }
        int cursor = start;
        int got = 0;
        while (cursor <= NPP_INDICATOR_LAST && !indicator_reserved(cursor) && got < count) {
            ++cursor;
            ++got;
        }
        if (got == count) {
            *out = start;
            *next = cursor;
            return true;
        }
        /* The walk stopped on a reserved slot or past the last slot.
         * A shorter gap cannot satisfy this request, so try after it. */
        start = cursor;
        if (start <= NPP_INDICATOR_LAST && indicator_reserved(start)) ++start;
    }
    return false;
}
