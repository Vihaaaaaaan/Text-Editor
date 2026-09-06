#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <errno.h>

struct termios orig_termios;

void 死ね(const char *s) {
	perror(s);
	exit(1)
}

//reverse terminal back to original version
void disable_raw_mode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
		死ね("tcsetattr");
}

void enable_raw_mode() {
	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) 死ね("tcgetattr");
	atexit(disable_raw_mode);

	struct termios raw;

	tcgetattr(STDIN_FILENO, &raw);
	//turn off terminal echo and canonical mode
	raw.c_iflag &= ~(IXON | ICRNL | ISTRIP | INPCK | BRKINT);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VMAX] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) 死ね("tcsetattr");
}

int main() {
	enable_raw_mode();

	while (1) {
		char c = '\0';
		if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) 死ね("read");
		//check if c is a non-printable char
		if (iscntrl(c)) {
			printf("%d\r\n", c);
		} else {
			printf("%d ('%c')\r\n", c, c);
		}
		if (c == 'q') break;
	}
	return 0;
}
