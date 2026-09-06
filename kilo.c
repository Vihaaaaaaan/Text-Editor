#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>

struct termios orig_termios;

//reverse terminal back to original version
void disable_raw_mode() {
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(disable_raw_mode);

	struct termios raw;

	tcgetattr(STDIN_FILENO, &raw);
	//turn off terminal echo and canonical mode
	raw.c_lflag &= ~(ECHO | ICANON);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main() {
	enable_raw_mode();

	char c;
	while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
		//check if c is a non-printable char
		if (iscntrl(c)) {
			printf("%d\n", c);
		} else {
			printf("%d ('%c')\n", c, c);
		}
	}
	return 0;
}
