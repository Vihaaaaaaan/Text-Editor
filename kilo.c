#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <termios.h>
#include <errno.h>
#include <string.h>

#define CTRL_KEY(k) ((k) & 0x1f) 
#define VERSION "0.0.1"

struct editorConfig {
	int screen_rows;
	int screen_cols;
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

char editor_read_key() {
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) 死ね("read");
	}
	return c;
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

void editor_process_keypress() {
	char c = editor_read_key();

	switch(c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
	}
}

void editor_draw_rows(struct abuf *ab) {
	int y;

	for (y = 0; y < E.screen_rows; y++) {
		
		if(y == E.screen_rows / 3) {
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

		ab_append(ab, "\x1b[K", 3);
		if (y < E.screen_rows - 1) {
			ab_append(ab, "\r\n", 2);
		}
	}
}

void editor_clear_screen() {
	struct abuf ab = ABUF_INIT;

	ab_append(&ab, "\x1b[?25l", 6);
	ab_append(&ab, "\x1b[H", 3);

	editor_draw_rows(&ab);

	ab_append(&ab, "\x1b[H", 3);
	ab_append(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	ab_free(&ab);
}

void init_editor() {
	if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) 死ね("get_window_size");
}

int main() {
	enable_raw_mode();
	init_editor();

	while (1) {
		editor_clear_screen();
		editor_process_keypress();
	}
	return 0;
}
