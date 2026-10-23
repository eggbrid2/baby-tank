#pragma once

#include <stdint.h>

typedef enum {
    BABY_TANK_FEED_REACTION_NORMAL = 0,
    BABY_TANK_FEED_REACTION_FAVORITE,
    BABY_TANK_FEED_REACTION_LESS_PREFERRED,
    BABY_TANK_FEED_REACTION_REPEATED,
    BABY_TANK_FEED_REACTION_AFTER_MEAL_DRINK,
} baby_tank_feed_reaction_t;

/* Volatile, per-boot interaction memory. Nothing in this module writes NVS or
 * contacts the cloud; restarting the head intentionally starts a new mood. */
void baby_tank_interaction_memory_init(void);

/* Item is one of the food/drink display expressions (39..52). */
baby_tank_feed_reaction_t baby_tank_interaction_memory_note_feed(uint8_t item);

/* Returns the number of pokes in the current eight-second burst, saturated. */
uint8_t baby_tank_interaction_memory_note_poke(void);

/* Result follows the diagnostics contract: 0=robot win, 1=robot lose, 2=draw.
 * Returns the consecutive count for the same outcome, saturated. */
uint8_t baby_tank_interaction_memory_note_game(uint8_t result);
