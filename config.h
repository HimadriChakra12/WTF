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

/*
 * Color of the source label slot, shown to the left of the query
 * prefix (e.g. "[ls] " or "[.] "), indicating where the entry list
 * came from.
 */
#define SOURCE_LABEL_COLOR TB_CYAN | TB_BOLD

/*
 * Preview pane: shown to the right of the list for the currently
 * selected entry. Files are shown like `cat`, scrollable. Directories
 * get a detailed listing (permissions/owner/size/mtime), like `ls -la`.
 *
 * Disabled automatically if the terminal is narrower than
 * PREVIEW_MIN_TERM_WIDTH columns.
 */
#define PREVIEW_ENABLED 1
#define PREVIEW_MIN_TERM_WIDTH 60
#define PREVIEW_WIDTH_PCT 45
#define PREVIEW_MAX_BYTES (256 * 1024)
#define PREVIEW_BINARY_SNIFF_BYTES 8000
#define PREVIEW_SCROLL_STEP 10
#define PREVIEW_BORDER_COLOR TB_WHITE
#define PREVIEW_HEADER_COLOR TB_YELLOW | TB_BOLD
#define PREVIEW_LINENO_COLOR TB_WHITE

/*
 * Keybinds. Values are termbox2 key codes (see termbox2.h). Each
 * "_ALT" is a second binding for the same action -- set it equal to
 * its primary to effectively disable it, but never set two DIFFERENT
 * actions to the same code, that's a duplicate switch-case and won't
 * compile.
 */
#define KEY_QUIT             TB_KEY_ESC
#define KEY_CONFIRM          TB_KEY_ENTER
#define KEY_UP               TB_KEY_ARROW_UP
#define KEY_DOWN             TB_KEY_ARROW_DOWN
#define KEY_LEFT             TB_KEY_ARROW_LEFT
#define KEY_RIGHT            TB_KEY_ARROW_RIGHT
#define KEY_LINE_HOME        TB_KEY_CTRL_A
#define KEY_LINE_END         TB_KEY_CTRL_E
#define KEY_PREVIEW_DOWN     TB_KEY_CTRL_D
#define KEY_PREVIEW_DOWN_ALT TB_KEY_PGDN
#define KEY_PREVIEW_UP       TB_KEY_CTRL_U
#define KEY_PREVIEW_UP_ALT   TB_KEY_PGUP
#define KEY_TOGGLE_MARK      TB_KEY_TAB

/*
 * Multi-select mode (-m): glyph and color shown in the selector
 * column for marked entries.
 */
#define MARK_GLYPH "*"
#define MARK_GLYPH_SZ 1
#define MARK_COLOR TB_GREEN | TB_BOLD
