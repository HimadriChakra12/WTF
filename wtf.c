#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#include "config.h"
#include "cvector.h"

#define TB_IMPL
#include "termbox2.h"

#ifndef TAB_WIDTH
#define TAB_WIDTH 8
#endif

/*
 * A decoded Unicode codepoint from a UTF-8 byte sequence,
 * along with how many bytes it consumed.
 */
typedef struct
{
  uint32_t cp;      /* Unicode codepoint */
  size_t   byte_len; /* Number of bytes consumed from the source */
} decoded_cp_t;

/*
 * Decode one UTF-8 codepoint from `s` of max length `len`.
 * Returns the codepoint and how many bytes were consumed.
 * On invalid sequences, returns U+FFFD (replacement char) consuming 1 byte.
 */
decoded_cp_t
utf8_decode_one(const char *s, size_t len)
{
  decoded_cp_t result = { .cp = 0xFFFD, .byte_len = 1 };
  if (!s || len == 0) return result;

  unsigned char c = (unsigned char)s[0];

  if (c < 0x80)
  {
    result.cp = c;
    result.byte_len = 1;
  }
  else if ((c & 0xE0) == 0xC0 && len >= 2)
  {
    result.cp = (c & 0x1F) << 6
              | ((unsigned char)s[1] & 0x3F);
    result.byte_len = 2;
  }
  else if ((c & 0xF0) == 0xE0 && len >= 3)
  {
    result.cp = (c & 0x0F) << 12
              | ((unsigned char)s[1] & 0x3F) << 6
              | ((unsigned char)s[2] & 0x3F);
    result.byte_len = 3;
  }
  else if ((c & 0xF8) == 0xF0 && len >= 4)
  {
    result.cp = (c & 0x07) << 18
              | ((unsigned char)s[1] & 0x3F) << 12
              | ((unsigned char)s[2] & 0x3F) << 6
              | ((unsigned char)s[3] & 0x3F);
    result.byte_len = 4;
  }

  return result;
}

/*
 * Count the number of Unicode codepoints in a UTF-8 string.
 */
size_t
utf8_cp_count(const char *s, size_t byte_len)
{
  size_t count = 0;
  size_t i = 0;
  while (i < byte_len)
  {
    decoded_cp_t d = utf8_decode_one(s + i, byte_len - i);
    i += d.byte_len;
    count++;
  }
  return count;
}

/*
 * Get the display width of a Unicode codepoint.
 * Uses wcwidth() for proper East Asian / emoji / Nerd Font support.
 * Falls back to 1 for most characters, 0 for combining marks, 2 for wide chars.
 */
int
cp_display_width(uint32_t cp)
{
  if (cp == '\t') return TAB_WIDTH;
  if (cp < 32) return 0;

  int w = tb_wcwidth((wchar_t)cp);
  if (w < 0) return 1;
  return w;
}

typedef struct
{
  char *label;
  size_t label_sz;       /* byte length of label */
  size_t label_cp_count; /* number of Unicode codepoints */
  int distance;
  int inaccuracy;
  bool *markers;         /* one bool per codepoint (not per byte) */
} wtf_entry_t;

wtf_entry_t
wtf_entry_new(char *label, size_t lsz)
{
  size_t cp_count = utf8_cp_count(label, lsz);
  bool *markers = calloc(cp_count, sizeof(bool));

  return (wtf_entry_t){
    .label = label,
    .label_sz = lsz,
    .label_cp_count = cp_count,
    .distance = 0,
    .inaccuracy = 0,
    .markers = markers,
  };
}

void
str_rate_free(wtf_entry_t *entry)
{
  free(entry->markers);
}

#define eq_case_insensitive(a, b) \
  (towlower((wint_t)(a)) == towlower((wint_t)(b)))

int
minimum(int x, int y)
{
  return x < y ? x : y;
}

int
minimum3(int x, int y, int z)
{
  return x < y ? minimum(x, z) : minimum(y, z);
}

/*
 * Levenshtein distance operating on Unicode codepoints.
 */
int
ldistance_cp(uint32_t *a, size_t asz, uint32_t *b, size_t bsz)
{
  /* Use heap allocation for large inputs */
  int *d = calloc((asz + 1) * (bsz + 1), sizeof(int));
  #define D(i, j) d[(i) * (bsz + 1) + (j)]

  for (size_t i = 0; i <= asz; i++) D(i, 0) = i;
  for (size_t j = 0; j <= bsz; j++) D(0, j) = j;

  for (size_t j = 1; j <= bsz; j++)
  {
    for (size_t i = 1; i <= asz; i++)
    {
      int cost = (towlower(a[i-1]) == towlower(b[j-1])) ? 0 : 1;
      D(i, j) = minimum3(
        D(i-1, j) + 1,
        D(i, j-1) + 1,
        D(i-1, j-1) + cost
      );
    }
  }

  int result = D(asz, bsz);
  free(d);
  return result;

  #undef D
}

/*
 * Decode a UTF-8 string into an array of codepoints.
 * Caller must free the returned array.
 */
uint32_t*
utf8_to_cps(const char *s, size_t byte_len, size_t *out_count)
{
  size_t count = utf8_cp_count(s, byte_len);
  uint32_t *cps = malloc(count * sizeof(uint32_t));

  size_t bi = 0;
  size_t ci = 0;
  while (bi < byte_len && ci < count)
  {
    decoded_cp_t d = utf8_decode_one(s + bi, byte_len - bi);
    cps[ci++] = d.cp;
    bi += d.byte_len;
  }

  if (out_count) *out_count = count;
  return cps;
}

/*
 * Rate an entry against a pattern (both as UTF-8 strings).
 * Markers are per-codepoint.
 */
void
wtf_entry_rate(wtf_entry_t *entry, char *pat, size_t pat_sz)
{
  size_t lcp_count = 0;
  size_t pcp_count = 0;
  uint32_t *label_cps = utf8_to_cps(entry->label, entry->label_sz, &lcp_count);
  uint32_t *pat_cps   = utf8_to_cps(pat, pat_sz, &pcp_count);

  bool *marks = entry->markers;

  ssize_t most_distant_marker = -1;

  /* Clear old match markers. */
  memset(marks, false, lcp_count * sizeof(bool));

  entry->inaccuracy = pcp_count;

  size_t j = 0; /* Index in pattern codepoints. */
  for (size_t i = 0; i < lcp_count && j < pcp_count; i++)
  {
    if (eq_case_insensitive(label_cps[i], pat_cps[j]))
    {
      if (most_distant_marker < 0) most_distant_marker = i;
      marks[i] = true;
      entry->inaccuracy--;
      j++;
    }
  }

  entry->inaccuracy += (pcp_count - j);
  entry->distance = ldistance_cp(
    label_cps, lcp_count,
    pat_cps, pcp_count
  ) + (most_distant_marker >= 0 ? most_distant_marker : 0) + entry->inaccuracy;

  free(label_cps);
  free(pat_cps);
}

int
wtf_entry_cmp(const wtf_entry_t **a, const wtf_entry_t **b)
{
  return (*a)->distance - (*b)->distance;
}

/* Calculates Y coordinate from the bottom or top of the terminal. */
#ifdef DIRECTION_TOP
#define calcy(y) (y)
#endif

#ifdef DIRECTION_BTM
#define calcy(y) (tb_height() - (y + 1))
#endif

void
scroll_to_fit(size_t *scroll, size_t selected, size_t max_visible)
{
  if (selected < *scroll)
  {
    *scroll = selected;
  }
  else if (selected >= *scroll + max_visible)
  {
    *scroll = selected - max_visible + 1;
  }
}

#define copy_item_refs(src, dst)         \
  do {                                   \
    size_t src_sz = cvector_size((src)); \
    cvector_reserve((dst), src_sz);      \
    for (size_t i = 0; i < src_sz; i++)  \
      (dst)[i] = &(src)[i];              \
    cvector_set_size((dst), src_sz);     \
  } while (0)

/*
 * Draw a single entry label at (x, y) with proper UTF-8 / tab / wide char support.
 * `markers` has one bool per codepoint indicating highlighted match.
 */
void
draw_label(int x, int y, wtf_entry_t *item, size_t primary_fg_attr)
{
  const char *s = item->label;
  size_t byte_len = item->label_sz;
  bool *marks = item->markers;

  size_t bi = 0;   /* byte index */
  size_t ci = 0;   /* codepoint index */
  int col = x;     /* current column on screen */

  while (bi < byte_len)
  {
    decoded_cp_t d = utf8_decode_one(s + bi, byte_len - bi);
    uint32_t cp = d.cp;

    size_t fg_attr = primary_fg_attr;
    if (marks && ci < item->label_cp_count && marks[ci])
      fg_attr |= TB_RED | TB_BOLD;

    if (cp == '\t')
    {
      /* Expand tab to spaces up to next tab stop */
      int tab_stop = TAB_WIDTH - ((col - x) % TAB_WIDTH);
      for (int t = 0; t < tab_stop; t++)
      {
        tb_set_cell(col, y, ' ', fg_attr, TB_DEFAULT);
        col++;
      }
    }
    else
    {
      int w = cp_display_width(cp);
      tb_set_cell(col, y, cp, fg_attr, TB_DEFAULT);
      col += w;
    }

    bi += d.byte_len;
    ci++;
  }
}

wtf_entry_t*
finder_start(cvector(wtf_entry_t) *list)
{
  {
    int tb_status = tb_init();
    if (tb_status)
    {
      fprintf(stderr, "initializing termbox failed with code %d\n", tb_status);
      return NULL;
    }
  }

  /* Enable UTF-8 output mode if termbox2 supports it */
  tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
  tb_set_output_mode(TB_OUTPUT_NORMAL);
  tb_set_cursor(0, calcy(0));

  struct tb_event ev;

  /* The entry we return. */
  wtf_entry_t *entry = NULL;

  wtf_entry_t **filtered = NULL;

  char *query = NULL;
  size_t cursor = 0;        /* cursor in codepoints */
  size_t cursor_bytes = 0;  /* cursor position in bytes (for insertion) */

  size_t max_visible = tb_height() - 2;
  size_t selected = 0;
  size_t scroll = 0;

  cvector_init(filtered, 255, NULL);
  copy_item_refs(*list, filtered);

  cvector_init(query, 32, NULL);

  /* Helper: recalculate cursor_bytes from cursor (codepoint position) */
  #define recalc_cursor_bytes() do { \
    cursor_bytes = 0; \
    size_t _ci = 0; \
    while (_ci < cursor && cursor_bytes < cvector_size(query)) { \
      decoded_cp_t _d = utf8_decode_one(query + cursor_bytes, cvector_size(query) - cursor_bytes); \
      cursor_bytes += _d.byte_len; \
      _ci++; \
    } \
  } while(0)

  /* Helper: get codepoint count of query */
  #define query_cp_count() utf8_cp_count(query, cvector_size(query))

  do
  {
    if (cvector_size(filtered) == 0)
    {
      selected = 0;
      scroll = 0;
    }
    else
    {
      if (selected >= cvector_size(filtered))
      {
        selected = cvector_size(filtered) - 1;
        scroll = 0;
      }
      scroll_to_fit(&scroll, selected, max_visible);
    }

    /*
     * DRAWING
     */
    tb_clear();
    {
      /* Print query prefix */
      tb_print(0, calcy(0), QUERY_PREFIX_COLOR, TB_DEFAULT, QUERY_PREFIX);

      /* Print query with proper UTF-8 rendering */
      {
        int qx = QUERY_PREFIX_SZ + 1;
        size_t bi = 0;
        size_t ci = 0;
        size_t cursor_screen_x = qx;

        while (bi < cvector_size(query))
        {
          decoded_cp_t d = utf8_decode_one(query + bi, cvector_size(query) - bi);
          if (ci == cursor) cursor_screen_x = qx;

          if (d.cp == '\t')
          {
            int tab_stop = TAB_WIDTH - ((qx - (QUERY_PREFIX_SZ + 1)) % TAB_WIDTH);
            for (int t = 0; t < tab_stop; t++)
            {
              tb_set_cell(qx, calcy(0), ' ', TB_DEFAULT, TB_DEFAULT);
              qx++;
            }
          }
          else
          {
            int w = cp_display_width(d.cp);
            tb_set_cell(qx, calcy(0), d.cp, TB_DEFAULT, TB_DEFAULT);
            qx += w;
          }

          bi += d.byte_len;
          ci++;
        }
        if (ci == cursor) cursor_screen_x = qx;
        tb_set_cursor(cursor_screen_x, calcy(0));
      }

      /* Status bar */
      {
        size_t w = 0;

        tb_printf_ex(
          0,
          calcy(1),
          STATUS_BAR_COLOR,
          TB_DEFAULT,
          &w,
          "%s %ld/%ld",
          STATUS_BAR_FILL,
          cvector_size(filtered),
          cvector_size(*list)
        );

        const size_t remaining_dashes = tb_width();
        for (size_t i = (w + 1); i < remaining_dashes; i += STATUS_BAR_FILL_SZ)
          tb_print(i, calcy(1), STATUS_BAR_COLOR, TB_DEFAULT, STATUS_BAR_FILL);
      }

      /* Draw the filtered list using the new draw_label function. */
      size_t visible = cvector_size(filtered);
      if (visible > max_visible) visible = max_visible;

      for (size_t i = 0; i < visible; i++)
      {
        size_t real_idx = scroll + i;
        wtf_entry_t *item = filtered[real_idx];
        size_t primary_fg_attr = TB_DEFAULT;

        if (real_idx == selected)
        {
          tb_print(0, calcy(2 + i), SELECTOR_COLOR, TB_DEFAULT, SELECTOR);
          primary_fg_attr |= TB_BOLD;
        }

        draw_label(SELECTOR_SZ + 1, calcy(2 + i), item, primary_fg_attr);
      }
    }
    tb_present();

    /*
     * EVENT LOGIC
     */
    tb_poll_event(&ev);

    if (ev.type == TB_EVENT_RESIZE)
    {
      max_visible = tb_height() - 2;
      scroll = 0;
      scroll_to_fit(&scroll, selected, max_visible);
    }

    if (ev.key == TB_KEY_ESC) goto start_finder_cleanup;
    if (ev.type == TB_EVENT_KEY)
    {
      bool query_update = false;

      switch (ev.key)
      {
        case 0:
        {
          /*
           * ev.ch is a Unicode codepoint from termbox2.
           * Encode it back to UTF-8 and insert at cursor_bytes.
           */
          char utf8_buf[4];
          size_t utf8_len = 0;
          uint32_t cp = ev.ch;

          if (cp < 0x80) {
            utf8_buf[0] = cp;
            utf8_len = 1;
          } else if (cp < 0x800) {
            utf8_buf[0] = 0xC0 | (cp >> 6);
            utf8_buf[1] = 0x80 | (cp & 0x3F);
            utf8_len = 2;
          } else if (cp < 0x10000) {
            utf8_buf[0] = 0xE0 | (cp >> 12);
            utf8_buf[1] = 0x80 | ((cp >> 6) & 0x3F);
            utf8_buf[2] = 0x80 | (cp & 0x3F);
            utf8_len = 3;
          } else {
            utf8_buf[0] = 0xF0 | (cp >> 18);
            utf8_buf[1] = 0x80 | ((cp >> 12) & 0x3F);
            utf8_buf[2] = 0x80 | ((cp >> 6) & 0x3F);
            utf8_buf[3] = 0x80 | (cp & 0x3F);
            utf8_len = 4;
          }

          /* Insert each byte at cursor_bytes position */
          for (size_t b = 0; b < utf8_len; b++)
            cvector_insert(query, cursor_bytes + b, utf8_buf[b]);

          cursor++;
          cursor_bytes += utf8_len;
          query_update = true;
          break;
        }

        case TB_KEY_BACKSPACE:
        case TB_KEY_BACKSPACE2:
          if (cvector_size(query) > 0 && cursor > 0)
          {
            /* Find the byte position of the previous codepoint */
            cursor--;
            size_t old_cursor_bytes = cursor_bytes;
            recalc_cursor_bytes();
            size_t erase_bytes = old_cursor_bytes - cursor_bytes;

            for (size_t b = 0; b < erase_bytes; b++)
              cvector_erase(query, cursor_bytes);

            query_update = true;
          }
          break;

        case TB_KEY_ARROW_LEFT:
          if (cursor > 0) {
            cursor--;
            recalc_cursor_bytes();
          }
          break;

        case TB_KEY_ARROW_RIGHT:
          if (cursor < query_cp_count()) {
            cursor++;
            recalc_cursor_bytes();
          }
          break;

        case TB_KEY_CTRL_A:
          cursor = 0;
          cursor_bytes = 0;
          break;

        case TB_KEY_CTRL_E:
          cursor = query_cp_count();
          cursor_bytes = cvector_size(query);
          break;

        case TB_KEY_ARROW_UP:
          if (cvector_size(filtered))
          {
#ifdef DIRECTION_TOP
            if (selected > 0) selected--;
            else selected = cvector_size(filtered) - 1;
#else
            selected = (selected + 1) % cvector_size(filtered);
#endif
            scroll_to_fit(&scroll, selected, max_visible);
          }
          break;

        case TB_KEY_ARROW_DOWN:
          if (cvector_size(filtered))
          {
#ifdef DIRECTION_TOP
            selected = (selected + 1) % cvector_size(filtered);
#else
            if (selected > 0) selected--;
            else selected = cvector_size(filtered) - 1;
#endif
            scroll_to_fit(&scroll, selected, max_visible);
          }
          break;

        case TB_KEY_ENTER:
          entry = (cvector_size(filtered) > 0) ? filtered[selected] : NULL;
          goto start_finder_cleanup;
      }

      if (query_update)
      {
        size_t query_sz = cvector_size(query);

        if (query_sz > 0)
        {
          cvector_set_size(filtered, 0);
          for (size_t i = 0; i < cvector_size(*list); i++) {
            wtf_entry_rate(&(*list)[i], query, query_sz);
            if ((*list)[i].inaccuracy <= FUZZ_MAX_INACCURACY)
              cvector_push_back(filtered, &(*list)[i]);
          }

          qsort(
            filtered,
            cvector_size(filtered),
            sizeof(wtf_entry_t*),
            (int (*)(const void*, const void*))wtf_entry_cmp
          );
        }
        else
        {
          for (size_t i = 0; i < cvector_size(*list); i++)
            memset((*list)[i].markers, false, (*list)[i].label_cp_count * sizeof(bool));
          copy_item_refs(*list, filtered);
        }
      }
    }
  }
  while (true);

  #undef recalc_cursor_bytes
  #undef query_cp_count

start_finder_cleanup:
    cvector_free(query);
    cvector_free(filtered);

    tb_shutdown();

    return entry;
}

/*
 * Read entries of `path` into `vec` as a newline-separated list,
 * skipping dotfiles/`.`/`..` (mirroring plain `ls`) and appending
 * a trailing '/' to entries that are themselves directories, so
 * they can be told apart from regular files at a glance.
 */
void
expanddir(cvector(char) *vec, const char *path)
{
  DIR *dir = opendir(path);
  if (!dir) return;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL)
  {
    if (ent->d_name[0] == '.') continue; /* skip ., .., and dotfiles */

    size_t len = strlen(ent->d_name);
    bool is_dir = false;

#ifdef DT_DIR
    if (ent->d_type == DT_DIR)
      is_dir = true;
    else if (ent->d_type != DT_UNKNOWN)
      is_dir = false;
    else
#endif
    {
      char full[PATH_MAX];
      snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

      struct stat st;
      if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
        is_dir = true;
    }

    for (size_t i = 0; i < len; i++)
      cvector_push_back(*vec, ent->d_name[i]);

    if (is_dir)
      cvector_push_back(*vec, '/');

    cvector_push_back(*vec, '\n');
  }

  closedir(dir);
}

/*
 * Recursively walk `path`, appending every regular file found to `vec`
 * as a newline-separated relative-path list (fzf-style recursive
 * source, but done natively instead of shelling out to `find`).
 *
 * Mirrors expanddir()'s dotfile-skipping so hidden trees like .git
 * aren't crawled. Uses lstat() so symlinks are never followed into,
 * matching `find`'s default (non -L) behavior and avoiding symlink
 * loops.
 */
void
expanddir_recursive(cvector(char) *vec, const char *path)
{
  DIR *dir = opendir(path);
  if (!dir) return;

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL)
  {
    if (ent->d_name[0] == '.') continue; /* skip ., .., and dotfiles/dirs */

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

    struct stat st;
    if (lstat(full, &st) != 0) continue;

    if (S_ISDIR(st.st_mode))
    {
      expanddir_recursive(vec, full);
    }
    else if (S_ISREG(st.st_mode))
    {
      const char *rel = full;
      if (rel[0] == '.' && rel[1] == '/') rel += 2;

      size_t len = strlen(rel);
      for (size_t i = 0; i < len; i++)
        cvector_push_back(*vec, rel[i]);

      cvector_push_back(*vec, '\n');
    }
  }

  closedir(dir);
}

size_t
read_to_vec(cvector(char) *vec, size_t buf_size, FILE *stream)
{
  size_t total_read = 0;
  size_t i = 0;

  do
  {
    if (i + buf_size > cvector_capacity(*vec))
      cvector_reserve(*vec, cvector_capacity(*vec) * 2);

    total_read = fread((char*)(*vec + i), sizeof(char), buf_size, stream);
    i += total_read;
    cvector_set_size(*vec, i);
  }
  while (total_read);

  return i;
}

void
print_help(FILE *stream)
{
#define HELP \
  "Usage: wtf [OPTIONS]\n" \
  "\n" \
  "Simple interactive command line fuzzy finder.\n" \
  "Designed to take any kind of new-line separated list from STDIN.\n" \
  "\n" \
  "If no input is piped in, wtf sources its own list automatically:\n" \
  "  - by default, the current directory's entries (dirs get a trailing /)\n" \
  "  - with -r, every file under the current directory, recursively\n" \
  "    (dotfiles/dirs like .git are skipped, symlinks aren't followed)\n" \
  "\n" \
  "Options:\n" \
  "  -h, --help     display this help and exit\n" \
  "  -r             when no stdin is piped, source input recursively\n" \
  "\n"

  fprintf(stream, HELP);
}

int
main(int argc, char **argv)
{
  /* Set locale for proper wcwidth() support */
  setlocale(LC_ALL, "");

  bool recursive = false;

  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--help") == 0
        || strcmp(argv[i], "-h") == 0)
    {
      print_help(stdout);
      return 0;
    }
    else if (strcmp(argv[i], "-r") == 0)
    {
      recursive = true;
    }
    else
    {
      print_help(stderr);
      return 2;
    }
  }

  int err = 0;
  char *buf = NULL;
  wtf_entry_t *list = NULL;

  cvector_init(buf, 512, NULL);
  cvector_init(list, 64, NULL);

  if (isatty(STDIN_FILENO))
  {
    /*
     * No piped input: auto-source a list, just like running
     * `ls | wtf`, but done natively so directories are marked
     * with a trailing '/'. With -r, recursively walk the tree
     * instead, fzf-style.
     */
    if (recursive)
      expanddir_recursive(&buf, ".");
    else
      expanddir(&buf, ".");
  }
  else
  {
    read_to_vec(&buf, 255, stdin);
  }

  /*
   * Split entries by newlines.
   */
  {
    size_t offset = 0;
    size_t size = 0;

    for (size_t i = 0; i < cvector_size(buf); i++)
    {
      char *ch = &buf[i];

      if (*ch == '\n')
      {
        if (size > 0)
        {
          cvector_push_back(
            list,
            wtf_entry_new((char*)(buf + offset), size)
          );
          offset += size;
          size = 0;
        }
        offset++;
        *ch = '\0';
      }
      else size++;
    }

    if (size > 0)
      cvector_push_back(
        list,
        wtf_entry_new((char*)(buf + offset), size)
      );
  }

  wtf_entry_t *entry = finder_start(&list);
  if (entry)
  {
    printf("%.*s\n", (int)entry->label_sz, entry->label);
  }
  else
  {
    err = 1;
  }

  cvector_set_elem_destructor(list, (void (*)(void*))str_rate_free);
  cvector_free(list);
  cvector_free(buf);

  return err;
}
