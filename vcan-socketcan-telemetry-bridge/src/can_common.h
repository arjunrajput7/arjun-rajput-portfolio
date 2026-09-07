// SPDX-License-Identifier: MIT
// Shared frame definitions for the vcan telemetry bridge.
// Matches dbc/telemetry.dbc.
#pragma once

#include <stdint.h>

/* --- CAN IDs (standard 11-bit) ------------------------------------------- */
#define CANID_ENGINE_DATA   0x0C0u   /* RPM, coolant temp, throttle          */
#define CANID_VEHICLE_DATA  0x0D0u   /* speed, odometer                      */

/* --- ENGINE_DATA layout (8 bytes, big-endian multibyte) ----------------- */
/*  [0..1] RPM        : uint16, 0.25 rpm/bit,  offset 0      -> 0..16383 rpm */
/*  [2]    ECT        : uint8,  1 degC/bit,    offset -40    -> -40..215 C   */
/*  [3]    THROTTLE   : uint8,  0.4 %/bit,     offset 0      -> 0..102 %     */
/*  [4]    FLAGS      : bit0 = MIL (check-engine)                            */
/*  [5..7] reserved                                                         */

/* --- VEHICLE_DATA layout ---------------------------------------------- */
/*  [0..1] SPEED      : uint16, 0.01 km/h/bit, offset 0      -> 0..655 km/h  */
/*  [2..4] ODOMETER   : uint24, 1 m/bit                                      */
/*  [5..7] reserved                                                         */

/* --- alert thresholds (used by the receiver) -------------------------- */
#define ALERT_RPM_REDLINE     6500.0
#define ALERT_ECT_MAX_C        110.0
#define ALERT_SPEED_MAX_KMH    180.0

/* --- endian helpers --------------------------------------------------- */
static inline void be16_put(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline uint16_t be16_get(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t be24_get(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline void be24_put(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v & 0xFF);
}

/* --- physical <-> raw conversions ----------------------------------- */
#define RPM_SCALE        0.25
#define ECT_OFFSET       (-40)
#define THROTTLE_SCALE   0.4
#define SPEED_SCALE      0.01
