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
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>

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
  bool marked;           /* multi-select mark, only meaningful with -m */
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
    .marked = false,
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
 * Drawing stops once `max_x` is reached, so the preview pane (if any) is never
 * overwritten by an overlong entry.
 */
void
draw_label(int x, int y, wtf_entry_t *item, size_t primary_fg_attr, int max_x)
{
  const char *s = item->label;
  size_t byte_len = item->label_sz;
  bool *marks = item->markers;

  size_t bi = 0;   /* byte index */
  size_t ci = 0;   /* codepoint index */
  int col = x;     /* current column on screen */

  while (bi < byte_len)
  {
    if (col >= max_x) break;

    decoded_cp_t d = utf8_decode_one(s + bi, byte_len - bi);
    uint32_t cp = d.cp;

    size_t fg_attr = primary_fg_attr;
    if (marks && ci < item->label_cp_count && marks[ci])
      fg_attr |= TB_RED | TB_BOLD;

    if (cp == '\t')
    {
      /* Expand tab to spaces up to next tab stop */
      int tab_stop = TAB_WIDTH - ((col - x) % TAB_WIDTH);
      for (int t = 0; t < tab_stop && col < max_x; t++)
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

/* A single line into a preview buffer, as a (offset, length) span. */
typedef struct
{
  size_t offset;
  size_t len;
} preview_line_t;

/*
 * What kind of thing the preview pane is currently showing. Only
 * PREVIEW_KIND_FILE gets line numbers -- directory listings, binary
 * placeholders, symlink targets, etc. have their own formatting.
 */
typedef enum
{
  PREVIEW_KIND_MISSING,
  PREVIEW_KIND_SYMLINK,
  PREVIEW_KIND_DIR,
  PREVIEW_KIND_BINARY,
  PREVIEW_KIND_OTHER,
  PREVIEW_KIND_FILE,
} preview_kind_t;

/*
 * Split `buf` into a list of (offset, len) line spans on '\n', for
 * scrollable line-by-line rendering. Does not copy: spans point back
 * into `buf`, which must outlive `idx`.
 */
void
build_line_index(cvector(preview_line_t) *idx, const char *buf, size_t buflen)
{
  cvector_set_size(*idx, 0);

  size_t start = 0;
  for (size_t i = 0; i < buflen; i++)
  {
    if (buf[i] == '\n')
    {
      preview_line_t l = { .offset = start, .len = i - start };
      cvector_push_back(*idx, l);
      start = i + 1;
    }
  }

  if (start < buflen)
  {
    preview_line_t l = { .offset = start, .len = buflen - start };
    cvector_push_back(*idx, l);
  }
}

/*
 * Draw a plain (unhighlighted) line of raw bytes at (x, y), clipped to
 * `max_x`. Used for preview content, as opposed to draw_label() which
 * is specific to fuzzy-matched list entries.
 */
void
draw_text_line(int x, int y, const char *s, size_t byte_len, size_t fg_attr, int max_x)
{
  size_t bi = 0;
  int col = x;

  while (bi < byte_len)
  {
    if (col >= max_x) break;

    decoded_cp_t d = utf8_decode_one(s + bi, byte_len - bi);

    if (d.cp == '\t')
    {
      int tab_stop = TAB_WIDTH - ((col - x) % TAB_WIDTH);
      for (int t = 0; t < tab_stop && col < max_x; t++)
      {
        tb_set_cell(col, y, ' ', fg_attr, TB_DEFAULT);
        col++;
      }
    }
    else
    {
      tb_set_cell(col, y, d.cp, fg_attr, TB_DEFAULT);
      col += cp_display_width(d.cp);
    }

    bi += d.byte_len;
  }
}

/* Classic `ls -l`-style 10-char permission string, e.g. "drwxr-xr-x". */
void
format_mode_str(mode_t mode, char out[11])
{
  out[0] = S_ISDIR(mode)  ? 'd'
         : S_ISLNK(mode)  ? 'l'
         : S_ISCHR(mode)  ? 'c'
         : S_ISBLK(mode)  ? 'b'
         : S_ISFIFO(mode) ? 'p'
         : S_ISSOCK(mode) ? 's'
         : '-';

  out[1] = (mode & S_IRUSR) ? 'r' : '-';
  out[2] = (mode & S_IWUSR) ? 'w' : '-';
  out[3] = (mode & S_IXUSR) ? ((mode & S_ISUID) ? 's' : 'x') : ((mode & S_ISUID) ? 'S' : '-');
  out[4] = (mode & S_IRGRP) ? 'r' : '-';
  out[5] = (mode & S_IWGRP) ? 'w' : '-';
  out[6] = (mode & S_IXGRP) ? ((mode & S_ISGID) ? 's' : 'x') : ((mode & S_ISGID) ? 'S' : '-');
  out[7] = (mode & S_IROTH) ? 'r' : '-';
  out[8] = (mode & S_IWOTH) ? 'w' : '-';
  out[9] = (mode & S_IXOTH) ? ((mode & S_ISVTX) ? 't' : 'x') : ((mode & S_ISVTX) ? 'T' : '-');
  out[10] = '\0';
}

typedef struct
{
  char name[NAME_MAX + 1];
  struct stat st;
} preview_dirent_t;

int
cmp_preview_dirent(const void *a, const void *b)
{
  return strcmp(((const preview_dirent_t*)a)->name, ((const preview_dirent_t*)b)->name);
}

/* Detailed, `ls -la`-style directory listing appended to `out`. */
void
build_dir_detail_preview(cvector(char) *out, const char *path)
{
  DIR *dir = opendir(path);
  if (!dir)
  {
    const char *msg = "(cannot open directory)\n";
    for (const char *p = msg; *p; p++) cvector_push_back(*out, *p);
    return;
  }

  preview_dirent_t *entries = NULL;
  cvector_init(entries, 64, NULL);

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL)
  {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

    preview_dirent_t pe;
    strncpy(pe.name, ent->d_name, sizeof(pe.name) - 1);
    pe.name[sizeof(pe.name) - 1] = '\0';

    if (lstat(full, &pe.st) != 0) continue;

    cvector_push_back(entries, pe);
  }
  closedir(dir);

  qsort(entries, cvector_size(entries), sizeof(preview_dirent_t), cmp_preview_dirent);

  size_t n = cvector_size(entries);
  {
    char hdr[64];
    int hlen = snprintf(hdr, sizeof(hdr), "%zu item%s\n\n", n, n == 1 ? "" : "s");
    for (int i = 0; i < hlen && i < (int)sizeof(hdr); i++)
      cvector_push_back(*out, hdr[i]);
  }

  for (size_t i = 0; i < n; i++)
  {
    char perms[11];
    format_mode_str(entries[i].st.st_mode, perms);

    struct passwd *pw = getpwuid(entries[i].st.st_uid);
    struct group *gr = getgrgid(entries[i].st.st_gid);

    struct tm tmv;
    localtime_r(&entries[i].st.st_mtime, &tmv);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", &tmv);

    char line[PATH_MAX + 128];
    int len = snprintf(
      line, sizeof(line),
      "%s %2ld %-8.8s %-8.8s %8lld %s %s%s\n",
      perms,
      (long)entries[i].st.st_nlink,
      pw ? pw->pw_name : "?",
      gr ? gr->gr_name : "?",
      (long long)entries[i].st.st_size,
      timebuf,
      entries[i].name,
      S_ISDIR(entries[i].st.st_mode) ? "/" : ""
    );

    for (int j = 0; j < len && j < (int)sizeof(line); j++)
      cvector_push_back(*out, line[j]);
  }

  cvector_free(entries);
}

/*
 * `cat`-like file preview, capped at PREVIEW_MAX_BYTES. Sniffs the
 * first PREVIEW_BINARY_SNIFF_BYTES for a NUL byte first (same
 * heuristic git/grep use); if found, the file is treated as binary
 * and its content is never dumped, just a placeholder + its size.
 */
preview_kind_t
build_file_preview(cvector(char) *out, const char *path, off_t size_hint)
{
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    const char *msg = "(cannot open file)\n";
    for (const char *p = msg; *p; p++) cvector_push_back(*out, *p);
    return PREVIEW_KIND_OTHER;
  }

  unsigned char sniff[PREVIEW_BINARY_SNIFF_BYTES];
  size_t sniff_n = fread(sniff, 1, sizeof(sniff), f);

  bool binary = false;
  for (size_t i = 0; i < sniff_n; i++)
  {
    if (sniff[i] == '\0') { binary = true; break; }
  }

  if (binary)
  {
    fclose(f);
    char msg[64];
    int len = snprintf(msg, sizeof(msg), "(binary file, %lld bytes)\n", (long long)size_hint);
    for (int i = 0; i < len && i < (int)sizeof(msg); i++) cvector_push_back(*out, msg[i]);
    return PREVIEW_KIND_BINARY;
  }

  rewind(f);

  char chunk[4096];
  size_t total = 0;
  size_t n;
  bool truncated = false;

  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
  {
    size_t take = n;
    if (total + take > PREVIEW_MAX_BYTES)
    {
      take = PREVIEW_MAX_BYTES - total;
      truncated = true;
    }

    for (size_t i = 0; i < take; i++)
      cvector_push_back(*out, chunk[i]);

    total += take;
    if (truncated) break;
  }

  fclose(f);

  if (truncated)
  {
    const char *msg = "\n... (truncated) ...\n";
    for (const char *p = msg; *p; p++) cvector_push_back(*out, *p);
  }

  return PREVIEW_KIND_FILE;
}

/*
 * Resolve a list entry's label into a full path we can preview,
 * relative to `base_dir` (the directory wtf auto-sourced from, if
 * any -- "." otherwise). A leading '/' in the label is treated as
 * already-absolute. A trailing '/' directory marker is stripped.
 */
void
resolve_preview_path(char *out, size_t out_sz, const char *base_dir, const char *label, size_t label_sz)
{
  size_t len = label_sz;
  if (len > 0 && label[len - 1] == '/') len--;

  if (len > 0 && label[0] == '/')
  {
    size_t n = (len < out_sz - 1) ? len : out_sz - 1;
    memcpy(out, label, n);
    out[n] = '\0';
  }
  else
  {
    snprintf(out, out_sz, "%s/%.*s", base_dir ? base_dir : ".", (int)len, label);
  }
}

/*
 * (Re)build the preview content + line index for `full_path`: a
 * detailed listing for directories, file contents for regular files,
 * the symlink target for symlinks, or a placeholder otherwise.
 */
/*
 * (Re)build the preview content + line index for `full_path`: a
 * detailed listing for directories, file contents for regular files,
 * the symlink target for symlinks, or a placeholder otherwise.
 * Returns what kind of thing was previewed.
 */
preview_kind_t
build_preview(cvector(char) *out, cvector(preview_line_t) *idx, const char *full_path)
{
  cvector_set_size(*out, 0);
  preview_kind_t kind;

  struct stat st;
  if (lstat(full_path, &st) != 0)
  {
    const char *msg = "(no preview available)\n";
    for (const char *p = msg; *p; p++) cvector_push_back(*out, *p);
    kind = PREVIEW_KIND_MISSING;
  }
  else if (S_ISLNK(st.st_mode))
  {
    char target[PATH_MAX];
    ssize_t n = readlink(full_path, target, sizeof(target) - 1);
    char line[PATH_MAX + 32];
    int len;

    if (n >= 0)
    {
      target[n] = '\0';
      len = snprintf(line, sizeof(line), "symlink -> %s\n", target);
    }
    else
    {
      len = snprintf(line, sizeof(line), "(broken symlink)\n");
    }

    for (int i = 0; i < len && i < (int)sizeof(line); i++)
      cvector_push_back(*out, line[i]);

    kind = PREVIEW_KIND_SYMLINK;
  }
  else if (S_ISDIR(st.st_mode))
  {
    build_dir_detail_preview(out, full_path);
    kind = PREVIEW_KIND_DIR;
  }
  else if (S_ISREG(st.st_mode))
  {
    kind = build_file_preview(out, full_path, st.st_size);
  }
  else
  {
    const char *msg = "(no preview for this file type)\n";
    for (const char *p = msg; *p; p++) cvector_push_back(*out, *p);
    kind = PREVIEW_KIND_OTHER;
  }

  build_line_index(idx, *out, cvector_size(*out));
  return kind;
}

wtf_entry_t*
finder_start(
  cvector(wtf_entry_t) *list,
  const char *source_label,
  const char *base_dir,
  bool preview_requested,
  bool multi_mode,
  cvector(wtf_entry_t*) *out_selected
)
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

  /* Preview pane state. */
  char *preview_buf = NULL;
  preview_line_t *preview_idx = NULL;
  size_t preview_scroll = 0;
  wtf_entry_t *previewed_entry = NULL;
  preview_kind_t preview_kind = PREVIEW_KIND_MISSING;
  char preview_path[PATH_MAX];
  preview_path[0] = '\0';

  cvector_init(preview_buf, 512, NULL);
  cvector_init(preview_idx, 64, NULL);

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
      bool show_preview = PREVIEW_ENABLED && preview_requested && (int)tb_width() >= PREVIEW_MIN_TERM_WIDTH;
      int preview_w = show_preview ? ((int)tb_width() * PREVIEW_WIDTH_PCT / 100) : 0;
      int list_w = show_preview ? ((int)tb_width() - preview_w - 1) : (int)tb_width();

      /* Print source label (e.g. "ls", a path, "argv", "stdin"), its own slot */
      size_t label_w = 0;
      if (source_label && source_label[0] != '\0')
      {
        tb_printf_ex(
          0,
          calcy(0),
          SOURCE_LABEL_COLOR,
          TB_DEFAULT,
          &label_w,
          "[%s] ",
          source_label
        );
      }

      /* Print query prefix */
      tb_print(label_w, calcy(0), QUERY_PREFIX_COLOR, TB_DEFAULT, QUERY_PREFIX);

      /* Print query with proper UTF-8 rendering */
      {
        int qx = (int)label_w + QUERY_PREFIX_SZ + 1;
        size_t bi = 0;
        size_t ci = 0;
        size_t cursor_screen_x = qx;

        while (bi < cvector_size(query))
        {
          if (qx >= list_w) break;

          decoded_cp_t d = utf8_decode_one(query + bi, cvector_size(query) - bi);
          if (ci == cursor) cursor_screen_x = qx;

          if (d.cp == '\t')
          {
            int tab_stop = TAB_WIDTH - ((qx - ((int)label_w + QUERY_PREFIX_SZ + 1)) % TAB_WIDTH);
            for (int t = 0; t < tab_stop && qx < list_w; t++)
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

        const size_t remaining_dashes = (size_t)list_w;
        for (size_t i = (w + 1); i < remaining_dashes; i += STATUS_BAR_FILL_SZ)
          tb_print(i, calcy(1), STATUS_BAR_COLOR, TB_DEFAULT, STATUS_BAR_FILL);
      }

      /* Draw the filtered list using the new draw_label function. */
      size_t visible = cvector_size(filtered);
      if (visible > max_visible) visible = max_visible;

      int label_x = SELECTOR_SZ + 1;
      if (multi_mode) label_x += MARK_GLYPH_SZ + 1;

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

        if (multi_mode && item->marked)
          tb_print(SELECTOR_SZ + 1, calcy(2 + i), MARK_COLOR, TB_DEFAULT, MARK_GLYPH);

        draw_label(label_x, calcy(2 + i), item, primary_fg_attr, list_w);
      }

      /* Preview pane, for the currently selected entry. */
      if (show_preview)
      {
        wtf_entry_t *current = (cvector_size(filtered) > 0) ? filtered[selected] : NULL;

        if (current != previewed_entry)
        {
          previewed_entry = current;
          preview_scroll = 0;

          if (current)
          {
            resolve_preview_path(preview_path, sizeof(preview_path), base_dir, current->label, current->label_sz);
            preview_kind = build_preview(&preview_buf, &preview_idx, preview_path);
          }
          else
          {
            preview_path[0] = '\0';
            preview_kind = PREVIEW_KIND_MISSING;
            cvector_set_size(preview_buf, 0);
            cvector_set_size(preview_idx, 0);
          }
        }

        size_t total_lines = cvector_size(preview_idx);
        size_t max_scroll = (total_lines > max_visible) ? (total_lines - max_visible) : 0;
        if (preview_scroll > max_scroll) preview_scroll = max_scroll;

        int px = list_w + 1;

        for (int y = 0; y < (int)tb_height(); y++)
          tb_set_cell(list_w, y, 0x2502, PREVIEW_BORDER_COLOR, TB_DEFAULT);

        if (current)
          draw_text_line(px, calcy(0), preview_path, strlen(preview_path), PREVIEW_HEADER_COLOR, (int)tb_width());

        /* Only plain file content gets line numbers; listings/placeholders have their own format. */
        int lineno_w = 0;
        if (preview_kind == PREVIEW_KIND_FILE && total_lines > 0)
        {
          size_t n = total_lines;
          while (n > 0) { lineno_w++; n /= 10; }
        }

        for (size_t i = 0; i < max_visible; i++)
        {
          size_t line_idx = preview_scroll + i;
          if (line_idx >= total_lines) break;

          preview_line_t pl = preview_idx[line_idx];
          int content_x = px;

          if (lineno_w > 0)
          {
            char numbuf[24];
            int numlen = snprintf(numbuf, sizeof(numbuf), "%*zu ", lineno_w, line_idx + 1);
            draw_text_line(px, calcy(2 + i), numbuf, (size_t)numlen, PREVIEW_LINENO_COLOR, (int)tb_width());
            content_x = px + lineno_w + 1;
          }

          draw_text_line(content_x, calcy(2 + i), preview_buf + pl.offset, pl.len, TB_DEFAULT, (int)tb_width());
        }
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

    if (ev.key == KEY_QUIT) goto start_finder_cleanup;
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

        case KEY_LEFT:
          if (cursor > 0) {
            cursor--;
            recalc_cursor_bytes();
          }
          break;

        case KEY_RIGHT:
          if (cursor < query_cp_count()) {
            cursor++;
            recalc_cursor_bytes();
          }
          break;

        case KEY_LINE_HOME:
          cursor = 0;
          cursor_bytes = 0;
          break;

        case KEY_LINE_END:
          cursor = query_cp_count();
          cursor_bytes = cvector_size(query);
          break;

        case KEY_UP:
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

        case KEY_DOWN:
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

        case KEY_CONFIRM:
          if (multi_mode)
          {
            cvector_set_size(*out_selected, 0);
            for (size_t i = 0; i < cvector_size(*list); i++)
              if ((*list)[i].marked)
                cvector_push_back(*out_selected, &(*list)[i]);

            /* Nothing marked: fall back to whatever's highlighted, like fzf does. */
            if (cvector_size(*out_selected) == 0 && cvector_size(filtered) > 0)
              cvector_push_back(*out_selected, filtered[selected]);
          }
          else
          {
            entry = (cvector_size(filtered) > 0) ? filtered[selected] : NULL;
          }
          goto start_finder_cleanup;

        case KEY_TOGGLE_MARK:
          if (multi_mode && cvector_size(filtered) > 0)
            filtered[selected]->marked = !filtered[selected]->marked;
          break;

        case KEY_PREVIEW_DOWN:
        case KEY_PREVIEW_DOWN_ALT:
          preview_scroll += PREVIEW_SCROLL_STEP;
          break;

        case KEY_PREVIEW_UP:
        case KEY_PREVIEW_UP_ALT:
          if (preview_scroll > (size_t)PREVIEW_SCROLL_STEP)
            preview_scroll -= PREVIEW_SCROLL_STEP;
          else
            preview_scroll = 0;
          break;
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
    cvector_free(preview_buf);
    cvector_free(preview_idx);

    tb_shutdown();

    return entry;
}

/*
 * Read entries of `path` into `vec` as a newline-separated list,
 * skipping dotfiles/`.`/`..` (mirroring plain `ls`) and appending
 * a trailing '/' to entries that are themselves directories, so
 * they can be told apart from regular files at a glance.
 *
 * If `dirs_only` is true, regular files are skipped entirely and
 * only directories are listed.
 */
void
expanddir(cvector(char) *vec, const char *path, bool dirs_only)
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

    if (dirs_only && !is_dir) continue;

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
 * If `dirs_only` is true, directories are listed (with a trailing
 * '/') instead of regular files; the walk itself is unaffected, so
 * every directory in the tree is still visited and descended into.
 *
 * Mirrors expanddir()'s dotfile-skipping so hidden trees like .git
 * aren't crawled. Uses lstat() so symlinks are never followed into,
 * matching `find`'s default (non -L) behavior and avoiding symlink
 * loops.
 */
void
expanddir_recursive(cvector(char) *vec, const char *path, bool dirs_only)
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
      if (dirs_only)
      {
        const char *rel = full;
        if (rel[0] == '.' && rel[1] == '/') rel += 2;

        size_t len = strlen(rel);
        for (size_t i = 0; i < len; i++)
          cvector_push_back(*vec, rel[i]);

        cvector_push_back(*vec, '/');
        cvector_push_back(*vec, '\n');
      }

      expanddir_recursive(vec, full, dirs_only);
    }
    else if (!dirs_only && S_ISREG(st.st_mode))
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

/*
 * Best-effort detection of the process feeding our stdin pipe, so
 * e.g. `ls | wtf` can show "ls" as its source label. Only meaningful
 * when stdin is actually a pipe.
 *
 * This walks /proc, looking for a process with an fd pointing at
 * the same pipe inode as our stdin, opened for writing, then reads
 * its comm. This is inherently racy and Linux-only: a pipe carries
 * no record of who's on the other end, so if the writer has already
 * finished and exited (common for fast, small-output commands) by
 * the time we look, its /proc entry -- and thus its name -- is gone.
 * Returns a heap-allocated string the caller must free(), or NULL if
 * nothing could be determined.
 */
char *
detect_pipe_writer(void)
{
  struct stat pipe_st;
  if (fstat(STDIN_FILENO, &pipe_st) != 0 || !S_ISFIFO(pipe_st.st_mode))
    return NULL;

  DIR *proc = opendir("/proc");
  if (!proc) return NULL;

  pid_t self = getpid();
  char *result = NULL;
  struct dirent *pent;

  while (!result && (pent = readdir(proc)) != NULL)
  {
    char *end;
    long pid = strtol(pent->d_name, &end, 10);
    if (*end != '\0' || pid <= 0 || (pid_t)pid == self) continue;

    char fd_dir[64];
    snprintf(fd_dir, sizeof(fd_dir), "/proc/%ld/fd", pid);

    DIR *fds = opendir(fd_dir);
    if (!fds) continue;

    struct dirent *fent;
    while ((fent = readdir(fds)) != NULL)
    {
      if (fent->d_name[0] == '.') continue;

      char fd_path[PATH_MAX];
      snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir, fent->d_name);

      struct stat st;
      if (stat(fd_path, &st) != 0
          || !S_ISFIFO(st.st_mode)
          || st.st_ino != pipe_st.st_ino)
        continue;

      char fdinfo_path[PATH_MAX];
      snprintf(fdinfo_path, sizeof(fdinfo_path), "/proc/%ld/fdinfo/%s", pid, fent->d_name);

      FILE *fi = fopen(fdinfo_path, "r");
      if (!fi) continue;

      int is_writer = 0;
      char line[64];
      while (fgets(line, sizeof(line), fi))
      {
        unsigned long flags;
        if (sscanf(line, "flags: %lo", &flags) == 1)
        {
          if ((flags & O_ACCMODE) != O_RDONLY) is_writer = 1;
          break;
        }
      }
      fclose(fi);

      if (!is_writer) continue;

      char comm_path[64];
      snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);

      FILE *cf = fopen(comm_path, "r");
      if (!cf) break;

      char comm[256];
      if (fgets(comm, sizeof(comm), cf))
      {
        comm[strcspn(comm, "\n")] = '\0';
        if (comm[0] != '\0') result = strdup(comm);
      }
      fclose(cf);
      break;
    }

    closedir(fds);
  }

  closedir(proc);
  return result;
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
  "Usage: wtf [OPTIONS] [PATH]\n" \
  "\n" \
  "Simple interactive command line fuzzy finder.\n" \
  "Designed to take any kind of new-line separated list from STDIN.\n" \
  "\n" \
  "A source label is shown in its own slot left of the query bar,\n" \
  "e.g. \"[ls]\" for piped input (best-effort), \"[.]\"/\"[PATH]\" when\n" \
  "auto-sourcing a directory, or \"[argv]\" when args are used directly.\n" \
  "Use -l to set it explicitly. This matters for shell builtins like\n" \
  "cd: `cd $(wtf)` can never show \"[cd]\" on its own, since cd never\n" \
  "becomes a process wtf could detect, and it hasn't even run yet at\n" \
  "the point wtf is reading input -- wrap it instead, e.g.:\n" \
  "  cdw() { cd \"$(wtf -l cd -d \"$@\")\"; }\n" \
  "\n" \
  "If no input is piped in, wtf sources its own list automatically:\n" \
  "  - by default, PATH's entries (dirs get a trailing /), or the\n" \
  "    current directory's if PATH isn't given\n" \
  "  - with -r, every file under PATH, recursively\n" \
  "    (dotfiles/dirs like .git are skipped, symlinks aren't followed)\n" \
  "  - with -d, only directories are listed (combine with -r to walk\n" \
  "    the whole tree looking for them)\n" \
  "  - given multiple positional args (e.g. `wtf $(ls)`), or a single\n" \
  "    arg that isn't a directory, they're used directly as entries\n" \
  "\n" \
  "A preview pane on the right shows the selected entry: file contents\n" \
  "(cat-style, with line numbers; binary files are detected and shown\n" \
  "as a placeholder instead of dumped) for files, a detailed\n" \
  "`ls -la`-style listing for directories, or the target for symlinks.\n" \
  "Off by default -- pass -p to turn it on. Ctrl-D/PgDn and Ctrl-U/PgUp\n" \
  "scroll it. Hidden automatically in narrow terminals. Entries\n" \
  "are resolved relative to PATH (or \".\" if none was given / entries\n" \
  "came from stdin or argv), unless the entry itself is an absolute path.\n" \
  "\n" \
  "With -m, multiple entries can be selected: Tab marks/unmarks the\n" \
  "highlighted entry, Enter prints every marked entry (one per line),\n" \
  "or just the highlighted one if nothing was marked.\n" \
  "\n" \
  "Options:\n" \
  "  -h, --help     display this help and exit\n" \
  "  -r             when no stdin is piped, source input recursively\n" \
  "  -d             when no stdin is piped, list directories only\n" \
  "  -p             show the preview pane\n" \
  "  -m             multi-select mode (Tab to mark, Enter to confirm all)\n" \
  "  -l LABEL       set the source label explicitly\n" \
  "\n"

  fprintf(stream, HELP);
}

int
main(int argc, char **argv)
{
  /* Set locale for proper wcwidth() support */
  setlocale(LC_ALL, "");

  bool recursive = false;
  bool dirs_only = false;
  bool preview_requested = false;
  bool multi_mode = false;
  const char *label_override = NULL;

  const char **positional = malloc(sizeof(char*) * (size_t)(argc > 0 ? argc : 1));
  int npositional = 0;

  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "--help") == 0
        || strcmp(argv[i], "-h") == 0)
    {
      print_help(stdout);
      free(positional);
      return 0;
    }
    else if (strcmp(argv[i], "-r") == 0)
    {
      recursive = true;
    }
    else if (strcmp(argv[i], "-d") == 0)
    {
      dirs_only = true;
    }
    else if (strcmp(argv[i], "-p") == 0)
    {
      preview_requested = true;
    }
    else if (strcmp(argv[i], "-m") == 0)
    {
      multi_mode = true;
    }
    else if (strcmp(argv[i], "-l") == 0)
    {
      if (i + 1 >= argc)
      {
        print_help(stderr);
        free(positional);
        return 2;
      }
      label_override = argv[++i];
    }
    else if (argv[i][0] == '-')
    {
      print_help(stderr);
      free(positional);
      return 2;
    }
    else
    {
      positional[npositional++] = argv[i];
    }
  }

  /*
   * A single positional arg that names a real directory is treated
   * as PATH. Otherwise (zero args, or more than one, or an arg that
   * isn't a directory) every positional arg becomes a literal entry
   * in the list -- this is what makes `wtf $(ls)` work, since a
   * command substitution expands to a flat list of words, not a path.
   */
  const char *path = ".";
  bool argv_mode = false;

  if (npositional == 1)
  {
    struct stat st;
    if (stat(positional[0], &st) == 0 && S_ISDIR(st.st_mode))
      path = positional[0];
    else
      argv_mode = true;
  }
  else if (npositional > 1)
  {
    argv_mode = true;
  }

  int err = 0;
  char *buf = NULL;
  wtf_entry_t *list = NULL;
  char *source_label = NULL;

  cvector_init(buf, 512, NULL);
  cvector_init(list, 64, NULL);

  if (isatty(STDIN_FILENO))
  {
    /*
     * No piped input: auto-source a list, just like running
     * `ls | wtf`, but done natively so directories are marked
     * with a trailing '/'. With -r, recursively walk the tree
     * instead, fzf-style. With -d, only directories are listed.
     * Defaults to the current directory, or the given PATH if
     * one was passed -- unless argv_mode kicked in, in which case
     * the positional args themselves are the entries.
     */
    if (argv_mode)
    {
      for (int i = 0; i < npositional; i++)
      {
        size_t len = strlen(positional[i]);
        for (size_t j = 0; j < len; j++)
          cvector_push_back(buf, positional[i][j]);
        cvector_push_back(buf, '\n');
      }
      source_label = strdup("argv");
    }
    else
    {
      if (recursive)
        expanddir_recursive(&buf, path, dirs_only);
      else
        expanddir(&buf, path, dirs_only);
      source_label = strdup(path);
    }
  }
  else
  {
    source_label = detect_pipe_writer();
    read_to_vec(&buf, 255, stdin);
    if (!source_label) source_label = strdup("stdin");
  }

  if (label_override)
  {
    free(source_label);
    source_label = strdup(label_override);
  }

  free(positional);

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

  wtf_entry_t **selected_out = NULL;
  cvector_init(selected_out, 8, NULL);

  wtf_entry_t *entry = finder_start(&list, source_label, path, preview_requested, multi_mode, &selected_out);

  if (multi_mode)
  {
    for (size_t i = 0; i < cvector_size(selected_out); i++)
      printf("%.*s\n", (int)selected_out[i]->label_sz, selected_out[i]->label);

    if (cvector_size(selected_out) == 0) err = 1;
  }
  else if (entry)
  {
    printf("%.*s\n", (int)entry->label_sz, entry->label);
  }
  else
  {
    err = 1;
  }

  cvector_free(selected_out);
  free(source_label);
  cvector_set_elem_destructor(list, (void (*)(void*))str_rate_free);
  cvector_free(list);
  cvector_free(buf);

  return err;
}
