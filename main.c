#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#define MINISH_TOK_BUFSIZE 64
#define MINISH_TOK_DELIM " \t\r\n\a"

struct minish_redirect {
    char *infile;   // file after '<', or NULL if none
    char *outfile;  // file after '>' or '>>', or NULL if none
    int append;     // 1 if '>>', 0 if '>'
};

static int minish_is_operator(const char *tok) {
    return strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0;
}

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

// splits args at each "|" and returns an array of commands (each one a char **).
// *count gets the number of commands. caller frees the returned array.
char **minish_split_pipeline(char **args, int *count) {
    int n = 1;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            n++;
        }
    }

    char ***cmds = malloc(n * sizeof *cmds);
    if (cmds == NULL) {
        perror("minish");
        exit(EXIT_FAILURE);
    }

    int k = 0;
    cmds[k++] = args;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            args[i] == NULL; // end the previous command here 
            cmds[k++] = &args[i + 1]; // next command starts after the pipe
        }
    }

    *count = n;
    return cmds;
}

void minish_redirect_fd(const char *path, int flags, int target_fd) {
    // opens or creates a file at the specified path according to the specified flags (O_WRONLY | O_CREAT | O_TRUNC)
    // if a new file is created, 0644 sets the permissions of it to read/write for the owner, read-only for everyone else.
    int fd = open(path, flags, 0644);
    if (fd == -1) {
        perror(path);
        _exit(EXIT_FAILURE);
    }
    if (dup2(fd, target_fd) == -1) {
        perror("minish: dup2");
        _exit(EXIT_FAILURE);
    }
    close(fd);
}

static void minish_apply_redirects(const struct minish_redirect *r) {
    if (r->infile != NULL) {
        minish_redirect_fd(r->infile, O_RDONLY, STDIN_FILENO);;
    }
    if (r->outfile != NULL) {
        int flags = O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC);
        minish_redirect_fd(r->outfile, flags, STDOUT_FILENO);
    }
}

static void minish_wait(pid_t pid) {
    while (waitpid(pid, NULL, 0) == -1) {
        if (errno != EINTR) {
            perror("minish: waitpid");
            break;
        }
    }
}

int minish_launch(char **args, struct minish_redirect *r) {
    pid_t pid = fork(); // fork() creates a child process. a process is a running instance of a program.

    if (pid == 0) {
        // child: set up redirections BEFORE exec.
        minish_apply_redirects(r);

        // child process: run the requested program.
	    execvp(args[0], args);
	    perror(args[0]);
	    _exit(EXIT_FAILURE);
    } else if (pid < 0) {
	    // the shell could not create a child.
	    perror("minish: fork");
    } else {
	    // parent process: wait for this child to finish.
	    minish_wait(pid);
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

static int minish_find_builtin(const char *name) {
    for (size_t i = 0; i < minish_num_builtins(); i++) {
        if (strcmp(name, builtin_names[i]) == 0) {
            return (int)i;
        }
    }
    return -1; // not a builtin
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

int minish_run_pipeline(char ***cmds, struct minish_redirect *redirs, int n) {
    pid_t *pids = malloc(n * sizeof *pids);
    if (pids == NULL) {
        perror("minish");
        exit(EXIT_FAILURE);
    }

    int started = 0; // how many children we actually forked
    int prev_read = -1; // read end of the previous pipe, or -1 for none

    for (int k = 0; k < n; k++) {
        int fd[2] = {-1, -1};

        // every command except the last gets a pipe to the next one.
        if (k < n - 1 && pipe(fd) == -1) {
            perror("minish: pipe");
            break;
        }

        pid_t pid = fork();

        if (pid == 0) {
            // child: connect stdin/stdout to the neighbouring pipes.
            if (prev_read != -1 && dup2(prev_read, STDIN_FILENO) == -1) {
                perror("minish: dup2");
                _exit(EXIT_FAILURE);
            }
            if (fd[1] != -1 && dup2(fd[1], STDOUT_FILENO) == -1) {
                perror("minish: dup2");
                _exit(EXIT_FAILURE);
            }

            // close every pipe fd. the dup2 copies on 0 and 1 stays open.
            if (prev_read != -1) close(prev_read);
            if (fd[0] != -1) close(fd[0]);
            if (fd[1] != -1) close(fd[1]);

            // explicit < and > override the pipe (as in the real shells)
            minish_apply_redirects(&redirs[k]);

            // builtins in a pipeline run in the child, then exit.
            int b = minish_find_builtin(cmds[k][0]);
            if (b >= 0) {
                builtin_functions[b](cmds[k]);
                fflush(stdout);
                _exit(EXIT_SUCCESS);
            }

            execvp(cmds[k][0], cmds[k]);
            perror(cmds[k][0]);
            _exit(EXIT_FAILURE);
        } else if (pid < 0) {
            perror("minish: fork");
            if (fd[0] != -1) close(fd[0]);
            if (fd[1] != -1) close(fd[1]);
            break;
        }

        pids[started++] = pid;

        // parent: it never uses these ends itself. so close them.
        if (prev_read != -1) close(prev_read);
        if (fd[1] != -1) close(fd[1]);
        prev_read = fd[0]; // the next command reads from here
    }
    
    // only still open if we broke out of the loop early.
    if (prev_read != -1) close(prev_read);

    for (int i = 0; i < started; i++) {
            minish_wait(pids[i]);
    }

    free(pids);
    return 1;
}

int minish_parse_redirects(char **args, struct minish_redirect *r) {
    r->infile = NULL;
    r->outfile = NULL;
    r->append = 0;

    int j = 0; // where the next kept token goes
    for (int i = 0; args[i] != NULL; i++) {
        if (minish_is_operator(args[i])) {
            char *target = args[i + 1];

            // there must be a filename, and it must not be another operator.
            if (target == NULL || minish_is_operator(target)) {
                fprintf(stderr, "minish syntax error: expected a filename after '%s'\n", args[i]);
                return -1;
            }

            if (strcmp(args[i], "<") == 0) {
                r->infile = target;
            } else {
                r->outfile = target;
                r->append = (strcmp(args[i], ">>") == 0);
            }

            i++; // also skip the filename token
        } else {
            args[j++] = args[i]; // keep this token
        }
    }

    args[j] = NULL; // re-terminate the shortened list
    return 0;    
}

int minish_execute(char **args) {
    if (args[0] == NULL) {
        return 1; // empty line.
    }

    int n = 0;
    char ***cmds = minish_split_pipeline(args, &n);

    struct minish_redirect *redirs = malloc(n * sizeof *redirs);
    if (redirs == NULL) {
        perror("minish");
        exit(EXIT_FAILURE);
    }

    // parse and validate every command BEFORE running anything.
    int ok = 1;
    for (int k = 0; k < n && ok; k++) {
        if (minish_parse_redirects(cmds[k], &redirs[k]) == -1) {
            ok = 0; // syntax error already printed.
        } else if (cmds[k][0] == NULL) {
            fprintf(stderr, "minish: missing command\n");
            ok = 0;
        }
    }

    int result = 1;
    if (ok) {
        if (n == 1) {
            // no pipes: same behaviour as before.
            int b = minish_find_builtin(cmds[0][0]);
            if (b >= 0) {
                result = builtin_functions[b](cmds[0]);
            } else {
                result = minish_launch(cmds[0], &redirs[0]);
            }
        } else {
            result = minish_run_pipeline(cmds, redirs, n);
        }
    }

    free(redirs);
    free(cmds);
    return result;
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
