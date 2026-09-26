#include "krtsp_history.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

typedef struct test_case {
    const char *name;
    bool (*function)(void);
} test_case;

#define NOW ((time_t)100000)
#define OLDEST (NOW - 300)   /* five minutes of buffer */

static bool test_a_new_history_is_live(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(history.mode == KRTSP_HISTORY_LIVE);
    CHECK(krtsp_history_position(&history, NOW) == NOW);
    CHECK(krtsp_history_behind(&history, NOW) == 0);
    /* Forward from live goes nowhere, and says so. */
    CHECK(!krtsp_history_apply(&history, KRTSP_HISTORY_FORWARD_SHORT, NOW,
                               OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_LIVE);
    return true;
}

static bool test_back_leaves_live_by_the_step(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_SHORT, NOW, OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_REPLAY);
    CHECK(krtsp_history_behind(&history, NOW) == 10);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW, OLDEST));
    CHECK(krtsp_history_behind(&history, NOW) == 70);
    return true;
}

static bool test_replay_keeps_its_distance_as_time_passes(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW, OLDEST));
    /* Thirty seconds later the picture has advanced thirty seconds: the
     * viewer is still a minute behind, not drifting toward live. */
    CHECK(krtsp_history_position(&history, NOW + 30) == NOW - 60 + 30);
    CHECK(krtsp_history_behind(&history, NOW + 30) == 60);
    return true;
}

static bool test_the_far_end_is_a_wall(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW, OLDEST));
    for (int steps = 0; steps < 4; ++steps) {
        CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW,
                                  OLDEST));
    }
    /* 5 x 60 = 300 s: exactly the oldest.  One more goes nowhere. */
    CHECK(krtsp_history_position(&history, NOW) == OLDEST);
    CHECK(history.at_oldest);
    CHECK(!krtsp_history_apply(&history, KRTSP_HISTORY_BACK_SHORT, NOW,
                               OLDEST));
    CHECK(krtsp_history_position(&history, NOW) == OLDEST);
    return true;
}

static bool test_a_step_past_the_oldest_lands_on_it(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    /* Only 25 seconds of buffer: a 60 second step goes to its start. */
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW,
                              NOW - 25));
    CHECK(krtsp_history_position(&history, NOW) == NOW - 25);
    CHECK(history.at_oldest);
    return true;
}

static bool test_forward_into_live_returns_to_live(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW, OLDEST));
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_FORWARD_SHORT, NOW,
                              OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_REPLAY);
    CHECK(krtsp_history_behind(&history, NOW) == 50);
    /* A step that reaches (or passes) the slack window is live. */
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_FORWARD_LONG, NOW,
                              OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_LIVE);
    CHECK(krtsp_history_behind(&history, NOW) == 0);
    return true;
}

static bool test_end_and_home(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_OLDEST, NOW, OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_REPLAY);
    CHECK(krtsp_history_behind(&history, NOW) == 300);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_LIVE_NOW, NOW, OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_LIVE);
    CHECK(!krtsp_history_apply(&history, KRTSP_HISTORY_LIVE_NOW, NOW, OLDEST));
    /* With nothing buffered there is no oldest to go to. */
    krtsp_history_init(&history);
    CHECK(!krtsp_history_apply(&history, KRTSP_HISTORY_OLDEST, NOW, 0));
    CHECK(!krtsp_history_apply(&history, KRTSP_HISTORY_BACK_SHORT, NOW, 0));
    CHECK(history.mode == KRTSP_HISTORY_LIVE);
    return true;
}

static bool test_pause_freezes_and_resume_continues_from_there(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_LONG, NOW, OLDEST));
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_TOGGLE_PAUSE, NOW + 5,
                              OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_PAUSED);
    /* Frozen: the instant does not move, so the distance behind grows. */
    CHECK(krtsp_history_position(&history, NOW + 5) == NOW - 60 + 5);
    CHECK(krtsp_history_position(&history, NOW + 65) == NOW - 60 + 5);
    CHECK(krtsp_history_behind(&history, NOW + 65) == 120);
    /* Resume from the frozen instant, not from where it would have got to. */
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_TOGGLE_PAUSE, NOW + 65,
                              OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_REPLAY);
    CHECK(krtsp_history_position(&history, NOW + 65) == NOW - 55);
    CHECK(krtsp_history_position(&history, NOW + 75) == NOW - 45);
    return true;
}

static bool test_pausing_live_freezes_now(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_TOGGLE_PAUSE, NOW, OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_PAUSED);
    CHECK(krtsp_history_behind(&history, NOW + 20) == 20);
    /* Stepping while paused stays paused: a frame to look at, moved. */
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_BACK_SHORT, NOW + 20,
                              OLDEST));
    CHECK(history.mode == KRTSP_HISTORY_PAUSED);
    CHECK(krtsp_history_position(&history, NOW + 20) == NOW - 10);
    return true;
}

static bool test_retention_can_pull_the_floor_from_under_a_viewer(void)
{
    krtsp_history history;

    krtsp_history_init(&history);
    CHECK(krtsp_history_apply(&history, KRTSP_HISTORY_OLDEST, NOW, OLDEST));
    /* Thirty seconds on the oldest segment has been pruned. */
    CHECK(!krtsp_history_clamp(&history, NOW + 30, OLDEST + 0));
    CHECK(krtsp_history_clamp(&history, NOW + 30, OLDEST + 40));
    CHECK(krtsp_history_position(&history, NOW + 30) == OLDEST + 40);
    /* Nothing left at all: live. */
    CHECK(krtsp_history_clamp(&history, NOW + 30, 0));
    CHECK(history.mode == KRTSP_HISTORY_LIVE);
    CHECK(!krtsp_history_clamp(&history, NOW + 30, 0));
    return true;
}

static bool test_format(void)
{
    char text[24];

    krtsp_history_format(32, text, sizeof(text));
    CHECK(strcmp(text, "-0:32") == 0);
    krtsp_history_format(725, text, sizeof(text));
    CHECK(strcmp(text, "-12:05") == 0);
    krtsp_history_format(3723, text, sizeof(text));
    CHECK(strcmp(text, "-1:02:03") == 0);
    krtsp_history_format(-4, text, sizeof(text));
    CHECK(strcmp(text, "-0:00") == 0);
    return true;
}

int
main(void)
{
    static const test_case tests[] = {
        {"a new history is live", test_a_new_history_is_live},
        {"back leaves live by the step", test_back_leaves_live_by_the_step},
        {"replay keeps its distance as time passes",
         test_replay_keeps_its_distance_as_time_passes},
        {"the far end is a wall", test_the_far_end_is_a_wall},
        {"a step past the oldest lands on it",
         test_a_step_past_the_oldest_lands_on_it},
        {"forward into live returns to live",
         test_forward_into_live_returns_to_live},
        {"end and home", test_end_and_home},
        {"pause freezes and resume continues from there",
         test_pause_freezes_and_resume_continues_from_there},
        {"pausing live freezes now", test_pausing_live_freezes_now},
        {"retention can pull the floor from under a viewer",
         test_retention_can_pull_the_floor_from_under_a_viewer},
        {"format", test_format}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
