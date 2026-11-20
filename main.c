#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    char *line = NULL;
    size_t capacity = 0;

    while(1) {
        printf("minish>");
	fflush(stdout);

	if (getline(&line, &capacity, stdin) == -1) {
	    break;
	}

	printf("you typed: %s", line);
    }

    free(line);

    if (ferror(stdin)) {
        perror("minish");
	return 1;
    }

    return 0;
}
