#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

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

int minish_launch(char **args) {
    pid_t pid = fork(); // fork() creates a child process. a process is a running instance of a program.

    if (pid == 0) {
        // child process: run the requested program.
	    execvp(args[0], args);
	    perror(args[0]);
	    _exit(EXIT_FAILURE);
    } else if (pid < 0) {
	    // the shell could not create a child.
	    perror("minish: fork");
    } else {
	    // parent process: wait for this child to finish.
	    while (waitpid(pid, NULL, 0) == -1) {
	        if (errno != EINTR) { 
                perror("minish: waitpid");
		        break;
	        }
	    }
    }

    return 1;

}

// function declarations for builtin shell commands
int minish_cd(char **args);
int minish_help(char **args);
int minish_exit(char **args);

const char *builtin_names[] = {"cd", "help", "exit"};

// each entry points to a function that takes char **args and returns an int.
// keep the order matched to the builtin_names. so "cd" is at index 0 in both.
int (*builtin_functions[])(char **) = {
    minish_cd,
    minish_help,
    minish_exit
};

size_t minish_num_builtins(void) {
    return sizeof builtin_names / sizeof builtin_names[0];
}

int minish_cd(char **args) {
    // example: for "cd ..", args[0] is "cd" and args[1] is "..".
    if (args[1] == NULL) {
        fprintf(stderr, "minish: cd needs a directory\n");
    // chdir(args[1]) changes minish's current directory to the path typed by user and returns -1 if it fails.
    } else if (chdir(args[1]) == -1) {
        perror("minish: cd");
    }
    return 1; // asks the shell to show another prompt.
}

int minish_help(char **args) {
    (void)args; // help takes the same parameter as the other functions but ignores it.
    puts("minish builtins:");
    for (size_t i = 0; i < minish_num_builtins(); i++) {
        printf(" %s\n", builtin_names[i]);
    }
    return 1;
}

int minish_exit(char **args) {
    (void)args; // exit also ignores args.
    return 0; // tell the shell loop to stop.
}

int minish_execute(char **args) {
    if (args[0] == NULL) {
        return 1; // empty line. show another prompt.
    }

    for (size_t i = 0; i < minish_num_builtins(); i++) {
        if (strcmp(args[0], builtin_names[i]) == 0) { // the names match
            return builtin_functions[i](args); // call the matching builtin.
        }
    }

    return minish_launch(args); // treat any other name as an external program.
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
        int keep_running = minish_execute(args);
        free(args);

	    if (keep_running == 0) {
            break;
        }
    }

    free(line);

    if (ferror(stdin)) {
        perror("minish");
	    return 1;
    }

    return 0;
}
