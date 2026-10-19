#include <assert.h>
#include <stdbool.h>

#include "wake_phrase.h"

int main(void)
{
    assert(baby_tank_wake_phrase_matches("baby坦克"));
    assert(baby_tank_wake_phrase_matches("BABY 坦克"));
    assert(baby_tank_wake_phrase_matches("  Baby，坦克！ "));
    assert(!baby_tank_wake_phrase_matches(NULL));
    assert(!baby_tank_wake_phrase_matches("baby"));
    assert(!baby_tank_wake_phrase_matches("坦克"));
    assert(!baby_tank_wake_phrase_matches("你好 baby坦克"));
    assert(!baby_tank_wake_phrase_matches("baby坦克你好"));
    assert(!baby_tank_wake_phrase_matches("baby坦克车"));
    return 0;
}
