#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINISH_TOK_BUFSIZE 64
#define MINISH_TOK_DELIM " \t\r\n\a"

char **minish_split_line(char *line) {
    int bufsize = MINISH_TOK_BUFSIZE;
    int position = 0;
    char **tokens = malloc(bufsize * sizeof *tokens);
    char *token;

    if (tokens == NULL) {
        perror("minish");
	exit(EXIT_FAILURE);
    }
    
    // returns a pointer to the first token in the string.
    token = strtok(line, MINISH_TOK_DELIM);
    while (token != NULL) {
        tokens[position++] = token;
        
	// dynamically, expand the size of the buffer if position exceeds it.
	if (position >= bufsize) {
	    bufsize += MINISH_TOK_BUFSIZE;
	    char **grown = realloc(tokens, bufsize * sizeof *tokens);
	    if (grown == NULL) {
	        free(tokens);
		perror("minish");
		exit(EXIT_FAILURE);
	    }
	    tokens = grown;
	}
        
	// repeat the process until no token is returned by strtok.
	token = strtok(NULL, MINISH_TOK_DELIM);
    }

    tokens[position] = NULL;
    return tokens;
}

int main(void) {
    char *line = NULL;
    size_t capacity = 0;

    while(1) {
        printf("minish>");
	fflush(stdout);

	if (getline(&line, &capacity, stdin) == -1) {
	    break;
	}

	char **args = minish_split_line(line);

	for (int i = 0; args[i] != NULL; i++) {
	    printf("args[%d] = %s\n", i, args[i]);
	}

	free(args);
    }

    free(line);

    if (ferror(stdin)) {
        perror("minish");
	return 1;
    }

    return 0;
}
