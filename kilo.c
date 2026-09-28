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
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

#define CTRL_KEY(k) ((k) & 0x1f) 
#define VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define QUIT_TIMES 3

enum editorKey {
	BACKSPACE = 127,
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
	int dirty;
	erow *row;
	char *file_name;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct editorConfig E;

void editor_set_status_message(const char *fmt, ...);

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

void editor_append_row(int at, char *s, size_t len) {
	if (at < 0 || at >= E.num_rows) return;

	E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.num_rows - at));

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.num_rows++;
	E.dirty++;

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editor_update_row(&E.row[at]);
}

void editor_free_row(erow *row) {
	free(row->render);
	free(row->chars);
}

void editor_del_row(int at) {
	if (at < 0 || at >= E.num_rows) return;
	editor_free_row(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.num_rows - at - 1));
	E.num_rows--;
	E.dirty++;
}

void editor_row_insert_char(erow *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size + 2);
	memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	editor_update_row(row);
	E.dirty++;
}

void editor_row_append_string(erow *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	editor_update_row(row);
	E.dirty++;
}

void editor_row_del_char(erow *row, int at) {
	if (at < 0 || at > row->size) return;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	editor_update_row(row);
	E.dirty++;
}

void editor_insert_char(int c) {
	if (E.cy == E.num_rows) {
		editor_append_row(E.num_rows, "", 0);
	}
	editor_row_insert_char(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editor_insert_new_line() {
	if (E.cx == 0) {
		editor_append_row(E.cy, "", 0);
	} else {
		erow *row = &E.row[E.cy];
		editor_append_row(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editor_update_row(row);
	}
	E.cy++;
	E.cx = 0;
}

void editor_del_char() {
	if (E.cy == E.num_rows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		editor_row_del_char(row, E.cx - 1);
		E.cx--;
	} else {
		E.cx = E.row[E.cy-1].size;
		editor_row_append_string(&E.row[E.cy - 1], row->chars, row->size);
		editor_del_row(E.cy);
		E.cy--;
	}
}

char *editor_rows_to_string(int *buf_len) {
	int tot_len = 0;
	int i;
	for (i = 0; i < E.num_rows; i++) tot_len += E.row[i].size + 1;
	*buf_len = tot_len;

	char *buf = malloc(tot_len);
	char *p = buf;
	for (i = 0; i < E.num_rows; i++) {
		memcpy(p, E.row[i].chars, E.row[i].size);
		p += E.row[i].size;
		*p = '\n';
		p++;
	}

	return buf;
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
		editor_append_row(E.num_rows, line, line_len);	
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void editor_save() {
	if (E.file_name == NULL) return;

	int len;
	char *buf = editor_rows_to_string(&len);

	int fd = open(E.file_name, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				E.dirty = 0;
				editor_set_status_message("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}

	free(buf);
	editor_set_status_message("I/O error: %s", strerror(errno));
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
	static int quit_times = QUIT_TIMES;
	int c = editor_read_key();

	switch(c) {
		case '\r':
			editor_insert_new_line();
			break;	

		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0) {
				editor_set_status_message("Warning! Unsaved changes. Press Ctrl-Q %d more times to quit", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		case CTRL_KEY('s'):
			editor_save();
			break;

		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
			editor_del_char();
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

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			editor_insert_char(c);
			break;
	}
	quit_times = QUIT_TIMES;
}

void editor_scroll() {
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
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.file_name ? E.file_name : "[No name]", E.num_rows, E.dirty ? "(modified)" : "");
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
	ab_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct abuf *ab) {
	ab_append(ab, "\x1b[K", 3);
	int msg_len = strlen(E.statusmsg);
	if (msg_len < E.screen_cols) msg_len = E.screen_cols;
	if(msg_len && time(NULL) - E.statusmsg_time < 5) ab_append(ab, E.statusmsg, msg_len);
}

void editor_clear_screen() {
	editor_scroll();

	struct abuf ab = ABUF_INIT;

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);
	editor_draw_status_bar(&ab);
	editor_draw_message_bar(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1, (E.rx - E.col_off) + 1);
	ab_append(&ab, buf, strlen(buf));

	ab_append(&ab, "\x1b[H", 3);
	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void editor_set_status_message(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

void init_editor() {
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.row_off = 0;
	E.col_off = 0;
	E.num_rows = 0;
	E.dirty = 0;
	E.row = NULL;
	E.file_name = NULL;
	E.statusmsg[0] = "\0";
	E.statusmsg_time = 0;
	if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) 死ね("get_window_size");
	E.screen_rows -= 2;
}

int main(int argc, char* argv[]) {
	enable_raw_mode();
	init_editor();
	
	if (argc >= 1) {
		editor_open(argv[1]);
	}
	
	editor_set_status_message("HELP: Ctrl-Q to quit | Ctrl-S to save");

	while (1) {
		editor_clear_screen();
		editor_process_keypress();
	}
	return 0;
}
