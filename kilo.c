#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdlib.h>
#include <termios.h>
#include <errno.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f) 
#define VERSION "0.0.1"
#define KILO_TAB_STOP 8

enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	PAGE_UP,
	PAGE_DOWN
};

typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editorConfig {
	int cx, cy;
	int rx;
	int row_off;
	int col_off;
	int screen_rows;
	int screen_cols;
	int num_rows;
	erow *row;
	char *file_name;
	struct termios orig_termios;
};

struct editorConfig E;

void 死ね(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
	perror(s);
	exit(1);
}

//reverse terminal back to original version
void disable_raw_mode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		死ね("tcsetattr");
}

void enable_raw_mode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) 死ね("tcgetattr");
	atexit(disable_raw_mode);

	struct termios raw = E.orig_termios;

	tcgetattr(STDIN_FILENO, &raw);
	//turn off terminal echo and canonical mode
	raw.c_iflag &= ~(IXON | ICRNL | ISTRIP | INPCK | BRKINT);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) 死ね("tcsetattr");
}

int editor_read_key() {
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) 死ね("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch(seq[1]) {
						case '3': return DEL_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
				}
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int get_cursor_position(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;
	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	printf("\r\n");

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';
	
	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int get_window_size(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return get_cursor_position(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

int editor_get_rx(erow *row, int cx) {
	int rx = 0;
	int i;
	for (i = 0; i < cx; i++) {
		if (row->chars[i] == '\t') rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		rx++;
	}
	return rx;
}

void editor_update_row(erow *row) {
	int tabs = 0;
	int i;

	for (i = 0; i < row->size; i++) if (row->chars[i] == '\t') tabs++;
	free(row->render);
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	int idx = 0;
	for (i = 0; i < row->size; i++) {
		if (row->chars[i] == '\t') {
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
			row->render[idx++] = row->chars[i];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editor_append_row(char *s, size_t len) {
	E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

	int at = E.num_rows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.num_rows++;

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editor_update_row(&E.row[at]);
}

void editor_open(char *file) {
	free(E.file_name);
	E.file_name = strdup(file);

	FILE *fp = fopen(file, "r");
	if (!fp) 死ね("fopen");

	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	line_len = getline(&line, &line_cap, fp);
	while ((line_len = getline(&line, &line_cap, fp)) != -1) {
		while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) line_len--;
		editor_append_row(line, line_len);	
	}
	free(line);
	fclose(fp);
}

struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0};

void ab_append(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void ab_free(struct abuf *ab) {
	free(ab->b);
}

void editor_move_cursor(int key) {
	erow *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];

	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			} else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy != E.num_rows) {
				E.cy++;
			}
			break;
	}

	row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
	int row_len = row ? row->size : 0;
	if (E.cx > row_len) {
		E.cx = row_len;
	}
}

void editor_process_keypress() {
	int c = editor_read_key();

	switch(c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;


		case PAGE_UP:
		case PAGE_DOWN:
			{
				int times = E.screen_rows;
				while (times--) {
					editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;

		case ARROW_UP:
		case ARROW_LEFT:
		case ARROW_DOWN:
		case ARROW_RIGHT:
			editor_move_cursor(c);
			break;
	}
}

void editor_scroll() {
	E.rx = 0;
	if (E.cy < E.num_rows) {
		E.rx = editor_get_rx(&E.row[E.cy], E.cx);
	}
	if (E.cy < E.row_off) {
		E.row_off = E.cy;
	} 
	if (E.cy >= E.row_off + E.screen_rows) {
		E.row_off = E.cy - E.screen_rows + 1;
	}
	if (E.rx < E.col_off) {
		E.col_off = E.rx;
	}
	if (E.rx >= E.col_off + E.screen_cols) {
		E.col_off = E.rx - E.screen_cols + 1;
	}
}

void editor_draw_rows(struct abuf *ab) {
	int y;

	for (y = 0; y < E.screen_rows; y++) {
		int file_row = y + E.row_off;
		if (file_row >= E.num_rows) {
			if (E.num_rows == 0 && y == E.screen_rows / 3) {
				char welcome[80];
				int welcome_len = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", VERSION);
		
				if (welcome_len > E.screen_cols) welcome_len = E.screen_cols;

				int padding = (E.screen_cols - welcome_len) / 2;
				if (padding) {
					ab_append(ab, "~", 1);
					padding--;
				}
				while (padding--) ab_append(ab, " ", 1); 

				ab_append(ab, welcome, welcome_len);
			} else {
				ab_append(ab, "~", 1);
			}
		} else {
			int len = E.row[file_row].rsize - E.col_off;
			if (len < 0) len = 0;
			if (len > E.screen_cols) len = E.screen_cols;
			ab_append(ab, &E.row[file_row].render[E.col_off], len);
		}

		ab_append(ab, "\x1b[K", 3);
		ab_append(ab, "\r\n", 2);
	}
}

void editor_draw_status_bar(struct abuf *ab) {
	ab_append(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines", E.file_name ? E.file_name : "[No name]", E.num_rows);
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d%d", E.cy + 1, E.num_rows);
	if (len > E.screen_cols) len = E.screen_cols;
	ab_append(ab, status, len);
	while (len < E.screen_cols) {
		if (E.screen_cols - len == rlen) {
			ab_append(ab, rstatus, rlen);
			break;
		} else {
			ab_append(ab, " ", 1);
			len++;
		}
	}
	ab_append(ab , "\x1b[m", 3);
}

void editor_clear_screen() {
	editor_scroll();

	struct abuf ab = ABUF_INIT;

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1, (E.rx - E.col_off) + 1);
	ab_append(&ab, buf, strlen(buf));

	ab_append(&ab, "\x1b[H", 3);
	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void init_editor() {
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.row_off = 0;
	E.col_off = 0;
	E.num_rows = 0;
	E.row = NULL;
	E.file_name = NULL;
	if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) 死ね("get_window_size");
	E.screen_rows -= 1;
}

int main(int argc, char* argv[]) {
	enable_raw_mode();
	init_editor();
	
	if (argc >= 1) {
		editor_open(argv[1]);
	}

	while (1) {
		editor_clear_screen();
		editor_process_keypress();
	}
	return 0;
}
