/*
 * libtsm - Screen Management
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Screen Management
 * This provides the abstracted screen management. It does not do any
 * terminal-emulation, instead it provides a resizable table of cells. You can
 * insert, remove and modify the cells freely.
 * A screen has always a fixed, but changeable, width and height. This defines
 * the number of columns and rows. The screen doesn't care for pixels, glyphs or
 * framebuffers. The screen only contains information about each cell.
 *
 * Screens are the logical model behind a real screen of a terminal emulator.
 * Users usually allocate a screen for each terminal-emulator they run. All they
 * have to do is render the screen onto their widget on each change and forward
 * any widget-events to the screen.
 *
 * The screen object already includes scrollback-buffers, selection support and
 * more. This simplifies terminal emulators a lot, but also prevents them from
 * accessing the real screen data. However, terminal emulators should have no
 * reason to access the data directly. The screen API should provide everything
 * they need.
 *
 * AGEING:
 * Each cell, line and screen has an "age" field. This field describes when it
 * was changed the last time. After drawing a screen, the current screen age is
 * returned. This allows users to skip drawing specific cells, if their
 * framebuffer was already drawn with a newer age than a given cell.
 * However, the screen-age might overflow. This is properly detected and causes
 * drawing functions to return "0" as age. Users must reset all their
 * framebuffer ages then. Otherwise, further drawing operations might
 * incorrectly skip cells.
 * Furthermore, if a cell has age "0", it means it _has_ to be drawn. No ageing
 * information is available.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "libtsm.h"
#include "libtsm_int.h"
#include "shl_llog.h"

#define LLOG_SUBSYSTEM "tsm_screen"

struct cell {
	tsm_symbol_t ch;
	unsigned int width;
	struct tsm_screen_attr attr;
	tsm_age_t age;
};

struct line {
	struct line *next;
	struct line *prev;

	unsigned int size;
	struct cell *cells;
	uint64_t sb_id;
	tsm_age_t age;
};

#define SELECTION_TOP -1
struct selection_pos {
	struct line *line;
	unsigned int x;
	int y;
};

struct tsm_screen {
	size_t ref;
	llog_submit_t llog;
	void *llog_data;
	unsigned int opts;
	unsigned int flags;
	struct tsm_symbol_table *sym_table;

	/* default attributes for new cells */
	struct tsm_screen_attr def_attr;

	/* ageing */
	tsm_age_t age_cnt;
	unsigned int age_reset : 1;

	/* current buffer */
	unsigned int size_x;
	unsigned int size_y;
	unsigned int margin_top;
	unsigned int margin_bottom;
	unsigned int line_num;
	struct line **lines;
	struct line **main_lines;
	struct line **alt_lines;
	tsm_age_t age;

	/* scroll-back buffer */
	unsigned int sb_count;		/* number of lines in sb */
	struct line *sb_first;		/* first line; was moved first */
	struct line *sb_last;		/* last line; was moved last*/
	unsigned int sb_max;		/* max-limit of lines in sb */
	struct line *sb_pos;		/* current position in sb or NULL */
	uint64_t sb_last_id;		/* last id given to sb-line */

	/* cursor */
	unsigned int cursor_x;
	unsigned int cursor_y;

	/* tab ruler */
	bool *tab_ruler;

	/* selection */
	bool sel_active;
	struct selection_pos sel_start;
	struct selection_pos sel_end;
};

static void inc_age(struct tsm_screen *con)
{
	if (!++con->age_cnt) {
		con->age_reset = 1;
		++con->age_cnt;
	}
}

void tsm_screen_inc_age(struct tsm_screen *con)
{
	if (!++con->age_cnt) {
		con->age_reset = 1;
		++con->age_cnt;
	}
}

static struct cell *get_cursor_cell(struct tsm_screen *con)
{
	unsigned int cur_x, cur_y;

	cur_x = con->cursor_x;
	if (cur_x >= con->size_x)
		cur_x = con->size_x - 1;

	cur_y = con->cursor_y;
	if (cur_y >= con->size_y)
		cur_y = con->size_y - 1;

	return &con->lines[cur_y]->cells[cur_x];
}

static void move_cursor(struct tsm_screen *con, unsigned int x, unsigned int y)
{
	struct cell *c;

	/* if cursor is hidden, just move it */
	if (con->flags & TSM_SCREEN_HIDE_CURSOR) {
		con->cursor_x = x;
		con->cursor_y = y;
		return;
	}

	/* If cursor is visible, we have to mark the current and the new cell
	 * as changed by resetting their age. We skip it if the cursor-position
	 * didn't actually change. */

	if (con->cursor_x == x && con->cursor_y == y)
		return;

	c = get_cursor_cell(con);
	c->age = con->age_cnt;

	con->cursor_x = x;
	con->cursor_y = y;

	c = get_cursor_cell(con);
	c->age = con->age_cnt;
}

static void cell_init(struct tsm_screen *con, struct cell *cell)
{
	cell->ch = 0;
	cell->width = 1;
	cell->age = con->age_cnt;
	memcpy(&cell->attr, &con->def_attr, sizeof(cell->attr));
}

static int line_new(struct tsm_screen *con, struct line **out,
		    unsigned int width)
{
	struct line *line;
	unsigned int i;

	if (!width)
		return -EINVAL;

	line = malloc(sizeof(*line));
	if (!line)
		return -ENOMEM;
	line->next = NULL;
	line->prev = NULL;
	line->size = width;
	line->age = con->age_cnt;

	line->cells = malloc(sizeof(struct cell) * width);
	if (!line->cells) {
		free(line);
		return -ENOMEM;
	}

	for (i = 0; i < width; ++i)
		cell_init(con, &line->cells[i]);

	*out = line;
	return 0;
}

static void line_free(struct line *line)
{
	free(line->cells);
	free(line);
}

static int line_resize(struct tsm_screen *con, struct line *line,
		       unsigned int width)
{
	struct cell *tmp;

	if (!line || !width)
		return -EINVAL;

	if (line->size < width) {
		tmp = realloc(line->cells, width * sizeof(struct cell));
		if (!tmp)
			return -ENOMEM;

		line->cells = tmp;

		while (line->size < width) {
			cell_init(con, &line->cells[line->size]);
			++line->size;
		}
	}

	return 0;
}

/* This links the given line into the scrollback-buffer */
static void link_to_scrollback(struct tsm_screen *con, struct line *line)
{
	struct line *tmp;

	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->sb_max == 0) {
		if (con->sel_active) {
			if (con->sel_start.line == line) {
				con->sel_start.line = NULL;
				con->sel_start.y = SELECTION_TOP;
			}
			if (con->sel_end.line == line) {
				con->sel_end.line = NULL;
				con->sel_end.y = SELECTION_TOP;
			}
		}
		line_free(line);
		return;
	}

	/* Remove a line from the scrollback buffer if it reaches its maximum.
	 * We must take care to correctly keep the current position as the new
	 * line is linked in after we remove the top-most line here.
	 * sb_max == 0 is tested earlier so we can assume sb_max > 0 here. In
	 * other words, buf->sb_first is a valid line if sb_count >= sb_max. */
	if (con->sb_count >= con->sb_max) {
		tmp = con->sb_first;
		con->sb_first = tmp->next;
		if (tmp->next)
			tmp->next->prev = NULL;
		else
			con->sb_last = NULL;
		--con->sb_count;

		/* (position == tmp && !next) means we have sb_max=1 so set
		 * position to the new line. Otherwise, set to new first line.
		 * If position!=tmp and we have a fixed-position then nothing
		 * needs to be done because we can stay at the same line. If we
		 * have no fixed-position, we need to set the position to the
		 * next inserted line, which can be "line", too. */
		if (con->sb_pos) {
			if (con->sb_pos == tmp ||
			    !(con->flags & TSM_SCREEN_FIXED_POS)) {
				if (con->sb_pos->next)
					con->sb_pos = con->sb_pos->next;
				else
					con->sb_pos = line;
			}
		}

		if (con->sel_active) {
			if (con->sel_start.line == tmp) {
				con->sel_start.line = NULL;
				con->sel_start.y = SELECTION_TOP;
			}
			if (con->sel_end.line == tmp) {
				con->sel_end.line = NULL;
				con->sel_end.y = SELECTION_TOP;
			}
		}
		line_free(tmp);
	}

	line->sb_id = ++con->sb_last_id;
	line->next = NULL;
	line->prev = con->sb_last;
	if (con->sb_last)
		con->sb_last->next = line;
	else
		con->sb_first = line;
	con->sb_last = line;
	++con->sb_count;
}

static void screen_scroll_up(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max, pos;
	int ret;

	if (!num)
		return;

	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* We cache lines on the stack to speed up the scrolling. However, if
	 * num is too big we might get overflows here so use recursion if num
	 * exceeds a hard-coded limit.
	 * 128 seems to be a sane limit that should never be reached but should
	 * also be small enough so we do not get stack overflows. */
	if (num > 128) {
		screen_scroll_up(con, 128);
		return screen_scroll_up(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		pos = con->margin_top + i;
		if (!(con->flags & TSM_SCREEN_ALTERNATE))
			ret = line_new(con, &cache[i], con->size_x);
		else
			ret = -EAGAIN;

		if (!ret) {
			link_to_scrollback(con, con->lines[pos]);
		} else {
			cache[i] = con->lines[pos];
			for (j = 0; j < con->size_x; ++j)
				cell_init(con, &cache[i]->cells[j]);
		}
	}

	if (num < max) {
		memmove(&con->lines[con->margin_top],
			&con->lines[con->margin_top + num],
			(max - num) * sizeof(struct line*));
	}

	memcpy(&con->lines[con->margin_top + (max - num)],
	       cache, num * sizeof(struct line*));

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y >= 0) {
			con->sel_start.y -= num;
			if (con->sel_start.y < 0) {
				con->sel_start.line = con->sb_last;
				while (con->sel_start.line && ++con->sel_start.y < 0)
					con->sel_start.line = con->sel_start.line->prev;
				con->sel_start.y = SELECTION_TOP;
			}
		}
		if (!con->sel_end.line && con->sel_end.y >= 0) {
			con->sel_end.y -= num;
			if (con->sel_end.y < 0) {
				con->sel_end.line = con->sb_last;
				while (con->sel_end.line && ++con->sel_end.y < 0)
					con->sel_end.line = con->sel_end.line->prev;
				con->sel_end.y = SELECTION_TOP;
			}
		}
	}
}

static void screen_scroll_down(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!num)
		return;

	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* see screen_scroll_up() for an explanation */
	if (num > 128) {
		screen_scroll_down(con, 128);
		return screen_scroll_down(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->margin_bottom - i];
		for (j = 0; j < con->size_x; ++j)
			cell_init(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->margin_top + num],
			&con->lines[con->margin_top],
			(max - num) * sizeof(struct line*));
	}

	memcpy(&con->lines[con->margin_top],
	       cache, num * sizeof(struct line*));

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y >= 0)
			con->sel_start.y += num;
		if (!con->sel_end.line && con->sel_end.y >= 0)
			con->sel_end.y += num;
	}
}

static void screen_write(struct tsm_screen *con, unsigned int x,
			  unsigned int y, tsm_symbol_t ch, unsigned int len,
			  const struct tsm_screen_attr *attr)
{
	struct line *line;
	unsigned int i;

	if (!len)
		return;

	if (x >= con->size_x || y >= con->size_y) {
		llog_warn(con, "writing beyond buffer boundary");
		return;
	}

	line = con->lines[y];

	if ((con->flags & TSM_SCREEN_INSERT_MODE) &&
	    (int)x < ((int)con->size_x - len)) {
		line->age = con->age_cnt;
		memmove(&line->cells[x + len], &line->cells[x],
			sizeof(struct cell) * (con->size_x - len - x));
	}

	line->cells[x].age = con->age_cnt;
	line->cells[x].ch = ch;
	line->cells[x].width = len;
	memcpy(&line->cells[x].attr, attr, sizeof(*attr));

	for (i = 1; i < len && i + x < con->size_x; ++i) {
		line->cells[x + i].age = con->age_cnt;
		line->cells[x + i].width = 0;
	}
}

void tsm_screen_erase_region(struct tsm_screen *con,
				 unsigned int x_from,
				 unsigned int y_from,
				 unsigned int x_to,
				 unsigned int y_to,
				 bool protect)
{
	unsigned int to;
	struct line *line;

	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (y_to >= con->size_y)
		y_to = con->size_y - 1;
	if (x_to >= con->size_x)
		x_to = con->size_x - 1;

	for ( ; y_from <= y_to; ++y_from) {
		line = con->lines[y_from];
		if (!line) {
			x_from = 0;
			continue;
		}

		if (y_from == y_to)
			to = x_to;
		else
			to = con->size_x - 1;
		for ( ; x_from <= to; ++x_from) {
			if (protect && line->cells[x_from].attr.protect)
				continue;

			cell_init(con, &line->cells[x_from]);
		}
		x_from = 0;
	}
}

static inline unsigned int to_abs_x(struct tsm_screen *con, unsigned int x)
{
	return x;
}

static inline unsigned int to_abs_y(struct tsm_screen *con, unsigned int y)
{
	if (!(con->flags & TSM_SCREEN_REL_ORIGIN))
		return y;

	return con->margin_top + y;
}

SHL_EXPORT
int tsm_screen_new(struct tsm_screen **out, tsm_log_t log, void *log_data)
{
	struct tsm_screen *con;
	int ret;
	unsigned int i;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	con->ref = 1;
	con->llog = log;
	con->llog_data = log_data;
	con->age_cnt = 1;
	con->age = con->age_cnt;
	con->def_attr.fr = 255;
	con->def_attr.fg = 255;
	con->def_attr.fb = 255;

	ret = tsm_symbol_table_new(&con->sym_table);
	if (ret)
		goto err_free;

	ret = tsm_screen_resize(con, 80, 24);
	if (ret)
		goto err_free;

	llog_debug(con, "new screen");
	*out = con;

	return 0;

err_free:
	for (i = 0; i < con->line_num; ++i) {
		line_free(con->main_lines[i]);
		line_free(con->alt_lines[i]);
	}
	free(con->main_lines);
	free(con->alt_lines);
	free(con->tab_ruler);
	tsm_symbol_table_unref(con->sym_table);
	free(con);
	return ret;
}

SHL_EXPORT
void tsm_screen_ref(struct tsm_screen *con)
{
	if (!con)
		return;

	++con->ref;
}

SHL_EXPORT
void tsm_screen_unref(struct tsm_screen *con)
{
	unsigned int i;

	if (!con || !con->ref || --con->ref)
		return;

	llog_debug(con, "destroying screen");

	for (i = 0; i < con->line_num; ++i) {
		line_free(con->main_lines[i]);
		line_free(con->alt_lines[i]);
	}
	free(con->main_lines);
	free(con->alt_lines);
	free(con->tab_ruler);
	tsm_symbol_table_unref(con->sym_table);
	free(con);
}

void tsm_screen_set_opts(struct tsm_screen *scr, unsigned int opts)
{
	if (!scr || !opts)
		return;

	scr->opts |= opts;
}

void tsm_screen_reset_opts(struct tsm_screen *scr, unsigned int opts)
{
	if (!scr || !opts)
		return;

	scr->opts &= ~opts;
}

unsigned int tsm_screen_get_opts(struct tsm_screen *scr)
{
	if (!scr)
		return 0;

	return scr->opts;
}

SHL_EXPORT
unsigned int tsm_screen_get_width(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->size_x;
}

SHL_EXPORT
unsigned int tsm_screen_get_height(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->size_y;
}

SHL_EXPORT
int tsm_screen_resize(struct tsm_screen *con, unsigned int x,
		      unsigned int y)
{
	struct line **cache;
	unsigned int i, j, width, diff, start;
	int ret;
	bool *tab_ruler;

	if (!con || !x || !y)
		return -EINVAL;

	inc_age(con);

	if (con->size_x == x && con->size_y == y)
		return 0;

	/* First make sure the line buffer is big enough for our new screen.
	 * That is, allocate all new lines and make sure each line has enough
	 * cells to hold the new screen or the current screen. If we fail, we
	 * can safely return -ENOMEM and the buffer is still valid. We must
	 * allocate the new lines to at least the same size as the current
	 * lines. Otherwise, if this function fails in later turns, we will have
	 * invalid lines in the buffer. */
	if (y > con->line_num) {
		/* resize main buffer */
		cache = realloc(con->main_lines, sizeof(struct line*) * y);
		if (!cache)
			return -ENOMEM;

		if (con->lines == con->main_lines)
			con->lines = cache;
		con->main_lines = cache;

		/* resize alt buffer */
		cache = realloc(con->alt_lines, sizeof(struct line*) * y);
		if (!cache)
			return -ENOMEM;

		if (con->lines == con->alt_lines)
			con->lines = cache;
		con->alt_lines = cache;

		/* allocate new lines */
		if (x > con->size_x)
			width = x;
		else
			width = con->size_x;

		while (con->line_num < y) {
			ret = line_new(con, &con->main_lines[con->line_num],
				       width);
			if (ret)
				return ret;

			ret = line_new(con, &con->alt_lines[con->line_num],
				       width);
			if (ret) {
				line_free(con->main_lines[con->line_num]);
				return ret;
			}

			++con->line_num;
		}
	}

	/* Resize all lines in the buffer if we increase screen width. This
	 * will guarantee that all lines are big enough so we can resize the
	 * buffer without reallocating them later. */
	if (x > con->size_x) {
		tab_ruler = realloc(con->tab_ruler, sizeof(bool) * x);
		if (!tab_ruler)
			return -ENOMEM;
		con->tab_ruler = tab_ruler;

		for (i = 0; i < con->line_num; ++i) {
			ret = line_resize(con, con->main_lines[i], x);
			if (ret)
				return ret;

			ret = line_resize(con, con->alt_lines[i], x);
			if (ret)
				return ret;
		}
	}

	/* clear expansion/padding area */
	start = x;
	if (x > con->size_x)
		start = con->size_x;
	for (j = 0; j < con->line_num; ++j) {
		/* main-lines may go into SB, so clear all cells */
		i = 0;
		if (j < con->size_y)
			i = start;

		for ( ; i < con->main_lines[j]->size; ++i)
			cell_init(con, &con->main_lines[j]->cells[i]);

		/* alt-lines never go into SB, only clear visible cells */
		i = 0;
		if (j < con->size_y)
			i = con->size_x;

		for ( ; i < x; ++i)
			cell_init(con, &con->alt_lines[j]->cells[i]);
	}

	/* xterm destroys margins on resize, so do we */
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;

	/* reset tabs */
	for (i = 0; i < x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}

	/* We need to adjust x-size first as screen_scroll_up() and friends may
	 * have to reallocate lines. The y-size is adjusted after them to avoid
	 * missing lines when shrinking y-size.
	 * We need to carefully look for the functions that we call here as they
	 * have stronger invariants as when called normally. */

	con->size_x = x;
	if (con->cursor_x >= con->size_x)
		move_cursor(con, con->size_x - 1, con->cursor_y);

	/* scroll buffer if screen height shrinks */
	if (con->cursor_y && y < con->size_y) {
		diff = con->size_y - y;
		screen_scroll_up(con, diff);
		if (con->cursor_y > diff)
			move_cursor(con, con->cursor_x, con->cursor_y - diff);
		else
			move_cursor(con, con->cursor_x, 0);
	}

	con->size_y = y;
	con->margin_bottom = con->size_y - 1;
	if (con->cursor_y >= con->size_y)
		move_cursor(con, con->cursor_x, con->size_y - 1);

	return 0;
}

static int ascii_test(struct tsm_screen *con, tsm_symbol_t inch)
{
	size_t len;
	const uint32_t *ch = tsm_symbol_get(con->sym_table, &inch, &len);

	size_t u8w = tsm_ucs4_get_width(*ch)+1;
	char u8_ch[u8w];
	size_t nch = tsm_ucs4_to_utf8(*ch, u8_ch);

	return !(nch == 1 && isspace(u8_ch[0]));
}

SHL_EXPORT
int tsm_screen_get_word(struct tsm_screen *con,
								unsigned x, unsigned y,
								unsigned *sx, unsigned *sy,
								unsigned *ex, unsigned *ey)
{
	if (y > con->size_y-1)
		return -EINVAL;

	struct line *cur = con->lines[y];

	int cy = y;

	if (!cur || x >= cur->size)
		return -EINVAL;

	*sx = x; *sy = y; *ex = x; *ey = y;

	struct line *wl = cur;
	if (!ascii_test(con, wl->cells[*sx].ch))
		return -EINVAL;

/* scan left */
	for(;;){
		int tx = *sx;
/* wrap around back */
		if (tx == 0){
			wl = wl->prev;
			if (!wl || !ascii_test(con, wl->cells[wl->size-1].ch))
				break;

			*sy--;
			*sx = wl->size - 1;
			continue;
		}
		else{
			tx = tx - 1;
			if (!ascii_test(con, wl->cells[tx].ch))
				break;
			*sx = tx;
		}
	}

	wl = cur;
/* scan right */
	for(;;){
		int tx = *ex;
		if (tx == wl->size-1){
			wl = wl->next;
			if (!wl || !ascii_test(con, wl->cells[0].ch))
				break;

			*ey++;
			*ex = 0;
		}
		else{
			tx = tx+1;
			if (!ascii_test(con, wl->cells[tx].ch))
				break;
			*ex = tx;
		}
	}

	return (*sx != *ex || *sy != *ey) ? 0 : -EINVAL;
}

/* use line_num and sb_count to figure out where we are */

SHL_EXPORT
int tsm_screen_set_margins(struct tsm_screen *con,
			       unsigned int top, unsigned int bottom)
{
	unsigned int upper, lower;

	if (!con)
		return -EINVAL;

	if (!top)
		top = 1;

	if (bottom <= top) {
		upper = 0;
		lower = con->size_y - 1;
	} else if (bottom > con->size_y) {
		upper = 0;
		lower = con->size_y - 1;
	} else {
		upper = top - 1;
		lower = bottom - 1;
	}

	con->margin_top = upper;
	con->margin_bottom = lower;
	return 0;
}

/* set maximum scrollback buffer size */
SHL_EXPORT
void tsm_screen_set_max_sb(struct tsm_screen *con,
			       unsigned int max)
{
	struct line *line;

	if (!con)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	while (con->sb_count > max) {
		line = con->sb_first;
		con->sb_first = line->next;
		if (line->next)
			line->next->prev = NULL;
		else
			con->sb_last = NULL;
		con->sb_count--;

		/* We treat fixed/unfixed position the same here because we
		 * remove lines from the TOP of the scrollback buffer. */
		if (con->sb_pos == line)
			con->sb_pos = con->sb_first;

		if (con->sel_active) {
			if (con->sel_start.line == line) {
				con->sel_start.line = NULL;
				con->sel_start.y = SELECTION_TOP;
			}
			if (con->sel_end.line == line) {
				con->sel_end.line = NULL;
				con->sel_end.y = SELECTION_TOP;
			}
		}
		line_free(line);
	}

	con->sb_max = max;
}

/* clear scrollback buffer */
SHL_EXPORT
void tsm_screen_clear_sb(struct tsm_screen *con)
{
	struct line *iter, *tmp;

	if (!con)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	for (iter = con->sb_first; iter; ) {
		tmp = iter;
		iter = iter->next;
		line_free(tmp);
	}

	con->sb_first = NULL;
	con->sb_last = NULL;
	con->sb_count = 0;
	con->sb_pos = NULL;

	if (con->sel_active) {
		if (con->sel_start.line) {
			con->sel_start.line = NULL;
			con->sel_start.y = SELECTION_TOP;
		}
		if (con->sel_end.line) {
			con->sel_end.line = NULL;
			con->sel_end.y = SELECTION_TOP;
		}
	}
}

SHL_EXPORT
void tsm_screen_sb_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	while (num--) {
		if (con->sb_pos) {
			if (!con->sb_pos->prev)
				return;

			con->sb_pos = con->sb_pos->prev;
		} else if (!con->sb_last) {
			return;
		} else {
			con->sb_pos = con->sb_last;
		}
	}
}

SHL_EXPORT
void tsm_screen_sb_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	while (num--) {
		if (con->sb_pos)
			con->sb_pos = con->sb_pos->next;
		else
			return;
	}
}

SHL_EXPORT
void tsm_screen_sb_page_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);
	tsm_screen_sb_up(con, num * con->size_y);
}

SHL_EXPORT
void tsm_screen_sb_page_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);
	tsm_screen_sb_down(con, num * con->size_y);
}

SHL_EXPORT
void tsm_screen_sb_reset(struct tsm_screen *con)
{
	if (!con || !con->sb_pos)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	con->sb_pos = NULL;
}

SHL_EXPORT
void tsm_screen_set_def_attr(struct tsm_screen *con,
				 const struct tsm_screen_attr *attr)
{
	if (!con || !attr)
		return;

	memcpy(&con->def_attr, attr, sizeof(*attr));
}

SHL_EXPORT
void tsm_screen_reset(struct tsm_screen *con)
{
	unsigned int i;

	if (!con)
		return;

	inc_age(con);
	con->age = con->age_cnt;

	con->flags = 0;
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;
	con->lines = con->main_lines;

	for (i = 0; i < con->size_x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}
}

SHL_EXPORT
void tsm_screen_set_flags(struct tsm_screen *con, unsigned int flags)
{
	unsigned int old;
	struct cell *c;

	if (!con || !flags)
		return;

	inc_age(con);

	old = con->flags;
	con->flags |= flags;

	if (!(old & TSM_SCREEN_ALTERNATE) && (flags & TSM_SCREEN_ALTERNATE)) {
		con->age = con->age_cnt;
		con->lines = con->alt_lines;
	}

	if (!(old & TSM_SCREEN_HIDE_CURSOR) &&
	    (flags & TSM_SCREEN_HIDE_CURSOR)) {
		c = get_cursor_cell(con);
		c->age = con->age_cnt;
	}

	if (!(old & TSM_SCREEN_INVERSE) && (flags & TSM_SCREEN_INVERSE))
		con->age = con->age_cnt;
}

SHL_EXPORT
void tsm_screen_reset_flags(struct tsm_screen *con, unsigned int flags)
{
	unsigned int old;
	struct cell *c;

	if (!con || !flags)
		return;

	inc_age(con);

	old = con->flags;
	con->flags &= ~flags;

	if ((old & TSM_SCREEN_ALTERNATE) && (flags & TSM_SCREEN_ALTERNATE)) {
		con->age = con->age_cnt;
		con->lines = con->main_lines;
	}

	if ((old & TSM_SCREEN_HIDE_CURSOR) &&
	    (flags & TSM_SCREEN_HIDE_CURSOR)) {
		c = get_cursor_cell(con);
		c->age = con->age_cnt;
	}

	if ((old & TSM_SCREEN_INVERSE) && (flags & TSM_SCREEN_INVERSE))
		con->age = con->age_cnt;
}

SHL_EXPORT
unsigned int tsm_screen_get_flags(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->flags;
}

SHL_EXPORT
unsigned int tsm_screen_get_cursor_x(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cursor_x;
}

SHL_EXPORT
unsigned int tsm_screen_get_cursor_y(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cursor_y;
}

SHL_EXPORT
void tsm_screen_set_tabstop(struct tsm_screen *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = true;
}

SHL_EXPORT
void tsm_screen_reset_tabstop(struct tsm_screen *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = false;
}

SHL_EXPORT
void tsm_screen_reset_all_tabstops(struct tsm_screen *con)
{
	unsigned int i;

	if (!con)
		return;

	for (i = 0; i < con->size_x; ++i)
		con->tab_ruler[i] = false;
}

SHL_EXPORT
void tsm_screen_write(struct tsm_screen *con, tsm_symbol_t ch,
			  const struct tsm_screen_attr *attr)
{
	unsigned int last, len;

	if (!con)
		return;

	len = tsm_symbol_get_width(con->sym_table, ch);
	if (!len)
		return;

	inc_age(con);

	if (con->cursor_y <= con->margin_bottom ||
	    con->cursor_y >= con->size_y)
		last = con->margin_bottom;
	else
		last = con->size_y - 1;

	if (con->cursor_x >= con->size_x) {
		if (con->flags & TSM_SCREEN_AUTO_WRAP)
			move_cursor(con, 0, con->cursor_y + 1);
		else
			move_cursor(con, con->size_x - 1, con->cursor_y);
	}

	if (con->cursor_y > last) {
		move_cursor(con, con->cursor_x, last);
		screen_scroll_up(con, 1);
	}

	screen_write(con, con->cursor_x, con->cursor_y, ch, len, attr);
	move_cursor(con, con->cursor_x + len, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_newline(struct tsm_screen *con)
{
	if (!con)
		return;

	inc_age(con);

	tsm_screen_move_down(con, 1, true);
	tsm_screen_move_line_home(con);
}

SHL_EXPORT
void tsm_screen_scroll_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);

	screen_scroll_up(con, num);
}

SHL_EXPORT
void tsm_screen_scroll_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);

	screen_scroll_down(con, num);
}

SHL_EXPORT
void tsm_screen_move_to(struct tsm_screen *con, unsigned int x,
			    unsigned int y)
{
	unsigned int last;

	if (!con)
		return;

	inc_age(con);

	if (con->flags & TSM_SCREEN_REL_ORIGIN)
		last = con->margin_bottom;
	else
		last = con->size_y - 1;

	x = to_abs_x(con, x);
	if (x >= con->size_x)
		x = con->size_x - 1;

	y = to_abs_y(con, y);
	if (y > last)
		y = last;

	move_cursor(con, x, y);
}

SHL_EXPORT
void tsm_screen_move_up(struct tsm_screen *con, unsigned int num,
			    bool scroll)
{
	unsigned int diff, size;

	if (!con || !num)
		return;

	inc_age(con);

	if (con->cursor_y >= con->margin_top)
		size = con->margin_top;
	else
		size = 0;

	diff = con->cursor_y - size;
	if (num > diff) {
		num -= diff;
		if (scroll)
			screen_scroll_down(con, num);
		move_cursor(con, con->cursor_x, size);
	} else {
		move_cursor(con, con->cursor_x, con->cursor_y - num);
	}
}

SHL_EXPORT
void tsm_screen_move_down(struct tsm_screen *con, unsigned int num,
			      bool scroll)
{
	unsigned int diff, size;

	if (!con || !num)
		return;

	inc_age(con);

	if (con->cursor_y <= con->margin_bottom)
		size = con->margin_bottom + 1;
	else
		size = con->size_y;

	diff = size - con->cursor_y - 1;
	if (num > diff) {
		num -= diff;
		if (scroll)
			screen_scroll_up(con, num);
		move_cursor(con, con->cursor_x, size - 1);
	} else {
		move_cursor(con, con->cursor_x, con->cursor_y + num);
	}
}

SHL_EXPORT
void tsm_screen_move_left(struct tsm_screen *con, unsigned int num)
{
	unsigned int x;

	if (!con || !num)
		return;

	inc_age(con);

	if (num > con->size_x)
		num = con->size_x;

	x = con->cursor_x;
	if (x >= con->size_x)
		x = con->size_x - 1;

	if (num > x)
		move_cursor(con, 0, con->cursor_y);
	else
		move_cursor(con, x - num, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_move_right(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	inc_age(con);

	if (num > con->size_x)
		num = con->size_x;

	if (num + con->cursor_x >= con->size_x)
		move_cursor(con, con->size_x - 1, con->cursor_y);
	else
		move_cursor(con, con->cursor_x + num, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_move_line_end(struct tsm_screen *con)
{
	if (!con)
		return;

	inc_age(con);

	move_cursor(con, con->size_x - 1, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_move_line_home(struct tsm_screen *con)
{
	if (!con)
		return;

	inc_age(con);

	move_cursor(con, 0, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_tab_right(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, x;

	if (!con || !num)
		return;

	inc_age(con);

	x = con->cursor_x;
	for (i = 0; i < num; ++i) {
		for (j = x + 1; j < con->size_x; ++j) {
			if (con->tab_ruler[j])
				break;
		}

		x = j;
		if (x + 1 >= con->size_x)
			break;
	}

	/* tabs never cause pending new-lines */
	if (x >= con->size_x)
		x = con->size_x - 1;

	move_cursor(con, x, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_tab_left(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, x;
	int j;

	if (!con || !num)
		return;

	inc_age(con);

	x = con->cursor_x;
	for (i = 0; i < num; ++i) {
		for (j = x - 1; j > 0; --j) {
			if (con->tab_ruler[j])
				break;
		}

		if (j <= 0) {
			x = 0;
			break;
		}
		x = j;
	}

	move_cursor(con, x, con->cursor_y);
}

SHL_EXPORT
void tsm_screen_insert_lines(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!con || !num)
		return;

	if (con->cursor_y < con->margin_top ||
	    con->cursor_y > con->margin_bottom)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom - con->cursor_y + 1;
	if (num > max)
		num = max;

	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->margin_bottom - i];
		for (j = 0; j < con->size_x; ++j)
			cell_init(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->cursor_y + num],
			&con->lines[con->cursor_y],
			(max - num) * sizeof(struct line*));

		memcpy(&con->lines[con->cursor_y],
		       cache, num * sizeof(struct line*));
	}

	con->cursor_x = 0;
}

SHL_EXPORT
void tsm_screen_delete_lines(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!con || !num)
		return;

	if (con->cursor_y < con->margin_top ||
	    con->cursor_y > con->margin_bottom)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	max = con->margin_bottom - con->cursor_y + 1;
	if (num > max)
		num = max;

	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->cursor_y + i];
		for (j = 0; j < con->size_x; ++j)
			cell_init(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->cursor_y],
			&con->lines[con->cursor_y + num],
			(max - num) * sizeof(struct line*));

		memcpy(&con->lines[con->cursor_y + (max - num)],
		       cache, num * sizeof(struct line*));
	}

	con->cursor_x = 0;
}

SHL_EXPORT
void tsm_screen_insert_chars(struct tsm_screen *con, unsigned int num)
{
	struct cell *cells;
	unsigned int max, mv, i;

	if (!con || !num || !con->size_y || !con->size_x)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	max = con->size_x - con->cursor_x;
	if (num > max)
		num = max;
	mv = max - num;

	cells = con->lines[con->cursor_y]->cells;
	if (mv)
		memmove(&cells[con->cursor_x + num],
			&cells[con->cursor_x],
			mv * sizeof(*cells));

	for (i = 0; i < num; ++i)
		cell_init(con, &cells[con->cursor_x + i]);
}

SHL_EXPORT
void tsm_screen_delete_chars(struct tsm_screen *con, unsigned int num)
{
	struct cell *cells;
	unsigned int max, mv, i;

	if (!con || !num || !con->size_y || !con->size_x)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	max = con->size_x - con->cursor_x;
	if (num > max)
		num = max;
	mv = max - num;

	cells = con->lines[con->cursor_y]->cells;
	if (mv)
		memmove(&cells[con->cursor_x],
			&cells[con->cursor_x + num],
			mv * sizeof(*cells));

	for (i = 0; i < num; ++i)
		cell_init(con, &cells[con->cursor_x + mv + i]);
}

SHL_EXPORT
void tsm_screen_erase_cursor(struct tsm_screen *con)
{
	unsigned int x;

	if (!con)
		return;

	inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	tsm_screen_erase_region(con, x, con->cursor_y, x, con->cursor_y, false);
}

SHL_EXPORT
void tsm_screen_erase_chars(struct tsm_screen *con, unsigned int num)
{
	unsigned int x;

	if (!con || !num)
		return;

	inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	tsm_screen_erase_region(con, x, con->cursor_y, x + num - 1, con->cursor_y,
			     false);
}

SHL_EXPORT
void tsm_screen_erase_cursor_to_end(struct tsm_screen *con,
				        bool protect)
{
	unsigned int x;

	if (!con)
		return;

	inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	tsm_screen_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_home_to_cursor(struct tsm_screen *con,
					 bool protect)
{
	if (!con)
		return;

	inc_age(con);

	tsm_screen_erase_region(con, 0, con->cursor_y, con->cursor_x,
			     con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_current_line(struct tsm_screen *con,
				       bool protect)
{
	if (!con)
		return;

	inc_age(con);

	tsm_screen_erase_region(con, 0, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_screen_to_cursor(struct tsm_screen *con,
					   bool protect)
{
	if (!con)
		return;

	inc_age(con);

	tsm_screen_erase_region(con, 0, 0, con->cursor_x, con->cursor_y, protect);
}

SHL_EXPORT
void tsm_screen_erase_cursor_to_screen(struct tsm_screen *con,
					   bool protect)
{
	unsigned int x;

	if (!con)
		return;

	inc_age(con);

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	tsm_screen_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->size_y - 1, protect);
}

SHL_EXPORT
void tsm_screen_erase_screen(struct tsm_screen *con, bool protect)
{
	if (!con)
		return;

	inc_age(con);

	tsm_screen_erase_region(con, 0, 0, con->size_x - 1, con->size_y - 1,
			     protect);
}

/*
 * Selection Code
 * If a running pty-client does not support mouse-tracking extensions, a
 * terminal can manually mark selected areas if it does mouse-tracking itself.
 * This tracking is slightly different than the integrated client-tracking:
 *
 * Initial state is no-selection. At any time selection_reset() can be called to
 * clear the selection and go back to initial state.
 * If the user presses a mouse-button, the terminal can calculate the selected
 * cell and call selection_start() to notify the terminal that the user started
 * the selection. While the mouse-button is held down, the terminal should call
 * selection_target() whenever a mouse-event occurs. This will tell the screen
 * layer to draw the selection from the initial start up to the last given
 * target.
 * Please note that the selection-start cannot be modified by the terminal
 * during a selection. Instead, the screen-layer automatically moves it along
 * with any scroll-operations or inserts/deletes. This also means, the terminal
 * must _not_ cache the start-position itself as it may change under the hood.
 * This selection takes also care of scrollback-buffer selections and correctly
 * moves selection state along.
 *
 * Please note that this is not the kind of selection that some PTY applications
 * support. If the client supports the mouse-protocol, then it can also control
 * a separate screen-selection which is always inside of the actual screen. This
 * is a totally different selection.
 */

static void selection_set(struct tsm_screen *con, struct selection_pos *sel,
			  unsigned int x, unsigned int y)
{
	struct line *pos;

	sel->line = NULL;
	pos = con->sb_pos;

	while (y && pos) {
		--y;
		pos = pos->next;
	}

	if (pos)
		sel->line = pos;

	sel->x = x;
	sel->y = y;
}

SHL_EXPORT
void tsm_screen_selection_reset(struct tsm_screen *con)
{
	if (!con)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	con->sel_active = false;
}

SHL_EXPORT
void tsm_screen_selection_start(struct tsm_screen *con,
				unsigned int posx,
				unsigned int posy)
{
	if (!con)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	con->sel_active = true;
	selection_set(con, &con->sel_start, posx, posy);
	memcpy(&con->sel_end, &con->sel_start, sizeof(con->sel_end));
}

SHL_EXPORT
void tsm_screen_selection_target(struct tsm_screen *con,
				 unsigned int posx,
				 unsigned int posy)
{
	if (!con || !con->sel_active)
		return;

	inc_age(con);
	/* TODO: more sophisticated ageing */
	con->age = con->age_cnt;

	selection_set(con, &con->sel_end, posx, posy);
}

/* TODO: tsm_ucs4_to_utf8 expects UCS4 characters, but a cell contains a
 * tsm-symbol (which can contain multiple UCS4 chars). Fix this when introducing
 * support for combining characters. */
static unsigned int copy_line(struct line *line, char *buf,
			      unsigned int start, unsigned int len)
{
	unsigned int i, end;
	char *pos = buf;

	end = start + len;
	for (i = start; i < line->size && i < end; ++i) {
		if (i < line->size || !line->cells[i].ch)
			pos += tsm_ucs4_to_utf8(line->cells[i].ch, pos);
		else
			pos += tsm_ucs4_to_utf8(' ', pos);
	}

	return pos - buf;
}

/* TODO: This beast definitely needs some "beautification", however, it's meant
 * as a "proof-of-concept" so its enough for now. */
SHL_EXPORT
int tsm_screen_selection_copy(struct tsm_screen *con, char **out)
{
	unsigned int len, i;
	struct selection_pos *start, *end;
	struct line *iter;
	char *str, *pos;

	if (!con || !out)
		return -EINVAL;

	if (!con->sel_active)
		return -ENOENT;

	/* check whether sel_start or sel_end comes first */
	if (!con->sel_start.line && con->sel_start.y == SELECTION_TOP) {
		if (!con->sel_end.line && con->sel_end.y == SELECTION_TOP) {
			str = strdup("");
			if (!str)
				return -ENOMEM;
			*out = str;
			return 0;
		}
		start = &con->sel_start;
		end = &con->sel_end;
	} else if (!con->sel_end.line && con->sel_end.y == SELECTION_TOP) {
		start = &con->sel_end;
		end = &con->sel_start;
	} else if (con->sel_start.line && con->sel_end.line) {
		if (con->sel_start.line->sb_id < con->sel_end.line->sb_id) {
			start = &con->sel_start;
			end = &con->sel_end;
		} else if (con->sel_start.line->sb_id > con->sel_end.line->sb_id) {
			start = &con->sel_end;
			end = &con->sel_start;
		} else if (con->sel_start.x < con->sel_end.x) {
			start = &con->sel_start;
			end = &con->sel_end;
		} else {
			start = &con->sel_end;
			end = &con->sel_start;
		}
	} else if (con->sel_start.line) {
		start = &con->sel_start;
		end = &con->sel_end;
	} else if (con->sel_end.line) {
		start = &con->sel_end;
		end = &con->sel_start;
	} else if (con->sel_start.y < con->sel_end.y) {
		start = &con->sel_start;
		end = &con->sel_end;
	} else if (con->sel_start.y > con->sel_end.y) {
		start = &con->sel_end;
		end = &con->sel_start;
	} else if (con->sel_start.x < con->sel_end.x) {
		start = &con->sel_start;
		end = &con->sel_end;
	} else {
		start = &con->sel_end;
		end = &con->sel_start;
	}

	/* calculate size of buffer */
	len = 0;
	iter = start->line;
	if (!iter && start->y == SELECTION_TOP)
		iter = con->sb_first;

	while (iter) {
		if (iter == start->line && iter == end->line) {
			if (iter->size > start->x) {
				if (iter->size > end->x)
					len += end->x - start->x + 1;
				else
					len += iter->size - start->x;
			}
			break;
		} else if (iter == start->line) {
			if (iter->size > start->x)
				len += iter->size - start->x;
		} else if (iter == end->line) {
			if (iter->size > end->x)
				len += end->x + 1;
			else
				len += iter->size;
			break;
		} else {
			len += iter->size;
		}

		++len;
		iter = iter->next;
	}

	if (!end->line) {
		if (start->line || start->y == SELECTION_TOP)
			i = 0;
		else
			i = start->y;
		for ( ; i < con->size_y; ++i) {
			if (!start->line && start->y == i && end->y == i) {
				if (con->size_x > start->x) {
					if (con->size_x > end->x)
						len += end->x - start->x + 1;
					else
						len += con->size_x - start->x;
				}
				break;
			} else if (!start->line && start->y == i) {
				if (con->size_x > start->x)
					len += con->size_x - start->x;
			} else if (end->y == i) {
				if (con->size_x > end->x)
					len += end->x + 1;
				else
					len += con->size_x;
				break;
			} else {
				len += con->size_x;
			}

			++len;
		}
	}

	/* allocate buffer */
	len *= 4;
	++len;
	str = malloc(len);
	if (!str)
		return -ENOMEM;
	pos = str;

	/* copy data into buffer */
	iter = start->line;
	if (!iter && start->y == SELECTION_TOP)
		iter = con->sb_first;

	while (iter) {
		if (iter == start->line && iter == end->line) {
			if (iter->size > start->x) {
				if (iter->size > end->x)
					len = end->x - start->x + 1;
				else
					len = iter->size - start->x;
				pos += copy_line(iter, pos, start->x, len);
			}
			break;
		} else if (iter == start->line) {
			if (iter->size > start->x)
				pos += copy_line(iter, pos, start->x,
						 iter->size - start->x);
		} else if (iter == end->line) {
			if (iter->size > end->x)
				len = end->x + 1;
			else
				len = iter->size;
			pos += copy_line(iter, pos, 0, len);
			break;
		} else {
			pos += copy_line(iter, pos, 0, iter->size);
		}

		*pos++ = '\n';
		iter = iter->next;
	}

	if (!end->line) {
		if (start->line || start->y == SELECTION_TOP)
			i = 0;
		else
			i = start->y;
		for ( ; i < con->size_y; ++i) {
			iter = con->lines[i];
			if (!start->line && start->y == i && end->y == i) {
				if (con->size_x > start->x) {
					if (con->size_x > end->x)
						len = end->x - start->x + 1;
					else
						len = con->size_x - start->x;
					pos += copy_line(iter, pos, start->x, len);
				}
				break;
			} else if (!start->line && start->y == i) {
				if (con->size_x > start->x)
					pos += copy_line(iter, pos, start->x,
							 con->size_x - start->x);
			} else if (end->y == i) {
				if (con->size_x > end->x)
					len = end->x + 1;
				else
					len = con->size_x;
				pos += copy_line(iter, pos, 0, len);
				break;
			} else {
				pos += copy_line(iter, pos, 0, con->size_x);
			}

			*pos++ = '\n';
		}
	}

	/* return buffer */
	*pos = 0;
	*out = str;
	return pos - str;
}

SHL_EXPORT
tsm_age_t tsm_screen_draw(struct tsm_screen *con, tsm_screen_draw_cb draw_cb,
			  void *data)
{
	unsigned int i, j, k;
	struct line *iter, *line = NULL;
	struct cell *cell, empty;
	struct tsm_screen_attr attr;
	int ret, warned = 0;
	const uint32_t *ch;
	size_t len;
	bool in_sel = false, sel_start = false, sel_end = false;
	bool was_sel = false;
	tsm_age_t age;

	if (!con || !draw_cb)
		return 0;

	cell_init(con, &empty);

	/* push each character into rendering pipeline */

	iter = con->sb_pos;
	k = 0;

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y == SELECTION_TOP)
			in_sel = !in_sel;
		if (!con->sel_end.line && con->sel_end.y == SELECTION_TOP)
			in_sel = !in_sel;

		if (con->sel_start.line &&
		    (!iter || con->sel_start.line->sb_id < iter->sb_id))
			in_sel = !in_sel;
		if (con->sel_end.line &&
		    (!iter || con->sel_end.line->sb_id < iter->sb_id))
			in_sel = !in_sel;
	}

	for (i = 0; i < con->size_y; ++i) {
		if (iter) {
			line = iter;
			iter = iter->next;
		} else {
			line = con->lines[k];
			k++;
		}

		if (con->sel_active) {
			if (con->sel_start.line == line ||
			    (!con->sel_start.line &&
			     con->sel_start.y == k - 1))
				sel_start = true;
			else
				sel_start = false;
			if (con->sel_end.line == line ||
			    (!con->sel_end.line &&
			     con->sel_end.y == k - 1))
				sel_end = true;
			else
				sel_end = false;

			was_sel = false;
		}

		for (j = 0; j < con->size_x; ++j) {
			if (j < line->size)
				cell = &line->cells[j];
			else
				cell = &empty;
			memcpy(&attr, &cell->attr, sizeof(attr));

			if (con->sel_active) {
				if (sel_start &&
				    j == con->sel_start.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
				if (sel_end &&
				    j == con->sel_end.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
			}

/*
 * ARCTERM edit -- don't draw cursor
			if (k == cur_y + 1 && j == cur_x &&
			    !(con->flags & TSM_SCREEN_HIDE_CURSOR))
				attr.inverse = !attr.inverse;
*/

			/* TODO: do some more sophisticated inverse here. When
			 * INVERSE mode is set, we should instead just select
			 * inverse colors instead of switching background and
			 * foreground */
			if (con->flags & TSM_SCREEN_INVERSE)
				attr.inverse = !attr.inverse;

			if (in_sel || was_sel) {
				was_sel = false;
				attr.inverse = !attr.inverse;
			}

			if (con->age_reset) {
				age = 0;
			} else {
				age = cell->age;
				if (line->age > age)
					age = line->age;
				if (con->age > age)
					age = con->age;
			}

			ch = tsm_symbol_get(con->sym_table, &cell->ch, &len);
			if (cell->ch == ' ' || cell->ch == 0)
				len = 0;
			ret = draw_cb(con, cell->ch, ch, len, cell->width,
				      j, i, &attr, age, data);
			if (ret && warned++ < 3) {
				llog_debug(con,
					   "cannot draw glyph at %ux%u via text-renderer",
					   j, i);
				if (warned == 3)
					llog_debug(con,
						   "suppressing further warnings during this rendering round");
			}
		}
	}

	if (con->age_reset) {
		con->age_reset = 0;
		return 0;
	} else {
		return con->age_cnt;
	}
}
