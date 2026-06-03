#include "termbox2.h"
#define DIRECTION_TOP

/*
 * Indicates the highest possible entry inaccuracy from the query,
 * before it is filtered out.
 *
 * Recommended value: 1
 */
#define FUZZ_MAX_INACCURACY 1
#define STATUS_BAR_FILL " "
/* You can use the Em Dash to fill in the gaps. */
// #define STATUS_BAR_FILL "—"
#define STATUS_BAR_FILL_SZ 2

/*
 * Color of the status bar.
 * Colors are defined in `termbox2.h:253`.
 */
#define STATUS_BAR_COLOR TB_WHITE

#define SELECTOR "|"
#define SELECTOR_SZ 1
#define SELECTOR_COLOR TB_WHITE | TB_BOLD

#define QUERY_PREFIX ">"
#define QUERY_PREFIX_SZ 1
#define QUERY_PREFIX_COLOR TB_YELLOW
