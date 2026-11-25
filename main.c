#define _POSIX_C_SOURCE 200809L // ask for the standard unix (POSIX) features, e.g. getline and strtok.
#include <fcntl.h> // open() and the O_RDONLY / O_WRONLY / ... flags
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h> // waitpid
#include <unistd.h> // fork, execvp, pipe, dup2, close, chdir

// the word list starts with room for 64 words and grows by 64 when full.
#define MINISH_TOK_BUFSIZE 64 
// characters that separate words: space, tab, carriage return, newline, bell.
#define MINISH_TOK_DELIM " \t\r\n\a"

 // remembers the redirections ( < > >> ) of ONE command.
 // example: for "sort < in.txt > out.txt" : infile = "in.txt", outfile = "out.txt", append = 0 
struct minish_redirect {
    char *infile;   // file after '<', or NULL if none
    char *outfile;  // file after '>' or '>>', or NULL if none
    int append;     // 1 if '>>', 0 if '>'
};

static int minish_is_operator(const char *tok) {
    return strcmp(tok, "<") == 0 || strcmp(tok, ">") == 0 || strcmp(tok, ">>") == 0;
}

// turn one line of text into a list of words. example: "ls -l /home" becomes: ["ls", "-l", "/home", NULL]
// the list ends with NULL so other code (like execvp) knows where it stops.
char **minish_split_line(char *line) {
    int bufsize = MINISH_TOK_BUFSIZE;
    int position = 0;
    // char ** = "a list of strings". reserve room for bufsize pointers.
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
            // save realloc's answer in a temporary variable first. 
            // if it fails it returns NULL, and we still need the old pointer to free it.
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

/*
 * cut the word list into separate commands at each "|".
 * example: the user typed "cat a.txt | sort | uniq", so args is: [cat, a.txt, |, sort, |, uniq, NULL]
 *
 * we overwrite each "|" with NULL, which ends the command before it:
 *    [cat, a.txt, NULL, sort, NULL, uniq, NULL]
 *     ^cmds[0]          ^cmds[1]   ^cmds[2]
 *
 * cmds[k] is a pointer to where command k starts. each command is now its
 * own NULL-terminated list, which is exactly what execvp wants.
 * nothing is copied. we only mark where each command begins.
 *
 * the return type is char ***, read as "an array of (lists of strings)".
 *   char *    one string
 *   char **   a list of strings (one command)
 *   char ***  a list of commands
 *
 * *count is set to the number of commands. the caller must free the result.
 */
char ***minish_split_pipeline(char **args, int *count) {
    int n = 1;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            n++;
        }
    }
    
    // now we know the exact size needed, so there's no limit on length.
    char ***cmds = malloc(n * sizeof *cmds);
    if (cmds == NULL) {
        perror("minish");
        exit(EXIT_FAILURE);
    }

    int k = 0;
    cmds[k++] = args; // the first command starts at the beginning.
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "|") == 0) {
            args[i] = NULL; // end the previous command here 
            cmds[k++] = &args[i + 1]; // next command starts after the pipe
        }
    }

    *count = n;
    return cmds;
}

/*
 * redirection helper: make one of our standard fds point at a file.
 *
 * background: every process has numbered "file descriptors" (fds):
 *   0 = stdin (input), 1 = stdout (output), 2 = stderr (errors).
 * programs read from fd 0 and write to fd 1 without knowing what they are
 * connected to. if we change what fd 1 points to, the program writes into
 * a file and never notices.
 *
 *   path      the file to open
 *   flags     how to open it (read, write, create, append, ...)
 *   target_fd the fd to rewire (0 for input, 1 for output)
 *
 * this only runs inside a CHILD process (see minish_launch), so if
 * something fails we exit the child with _exit instead of returning.
 * otherwise the command would run without its redirection.
 */
void minish_redirect_fd(const char *path, int flags, int target_fd) {
    // opens or creates a file at the specified path according to the specified flags (O_WRONLY | O_CREAT | O_TRUNC)
    // if a new file is created, 0644 sets the permissions of it to read/write for the owner, read-only for everyone else.
    int fd = open(path, flags, 0644);
    if (fd == -1) {
        perror(path);
        _exit(EXIT_FAILURE);
    }

    // dup2(fd, target_fd): make target_fd point to the same file as fd.
    // if target_fd was already open (like the terminal), it is closed first.
    if (dup2(fd, target_fd) == -1) {
        perror("minish: dup2");
        _exit(EXIT_FAILURE);
    }

    // target_fd now has its own link to the file, so the original fd is just an extra copy. close it so we don't leak it.
    close(fd);
}

// applies one command's redirections. call this in the child, before exec.
static void minish_apply_redirects(const struct minish_redirect *r) {
    // "cmd < file": read input from the file instead of the keyboard.
    if (r->infile != NULL) {
        minish_redirect_fd(r->infile, O_RDONLY, STDIN_FILENO);
    }

    // "cmd > file" or "cmd >> file": send output to the file.
    if (r->outfile != NULL) {
        // O_WRONLY  open for writing only
        // O_CREAT   create the file if it does not exist then one of:
        // O_APPEND  ">>"  new output goes at the end of the file
        // O_TRUNC   ">"   wipe the file first, then write
        int flags = O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC);
        minish_redirect_fd(r->outfile, flags, STDOUT_FILENO);
    }
}

/*
 * wait for one child process to finish.
 * waitpid returns -1 on a problem. EINTR just means "a signal interrupted
 * the wait", which is not a real error, so we try again. any other error
 * is printed and we give up waiting.
 */
static void minish_wait(pid_t pid) {
    while (waitpid(pid, NULL, 0) == -1) {
        if (errno != EINTR) {
            perror("minish: waitpid");
            break;
        }
    }
}

// run ONE external program (like ls), with optional redirection.
// fork (clone the shell), turn the clone into the new program with execvp, and have the original shell wait for it to finish.
// returns 1, meaning "keep the shell running."
int minish_launch(char **args, struct minish_redirect *r) {
    // fork() copies the running shell. afterwards there are TWO processes running this same code, and fork returns a different value in each:
    //   child  -> 0
    //   parent -> the child's process id (a positive number)
    //   error  -> -1 (no child was made)
    pid_t pid = fork();
    if (pid == 0) {
        // we are the CHILD. rewire stdin/stdout BEFORE exec. we do it in the child so the shell's own stdin/stdout stay untouched. 
        // rewired fds survive execvp, so the new program inherits them.
        minish_apply_redirects(r);

        // execvp REPLACES this process with the named program, searching the PATH. if it works it never returns, because this process is
        // now "ls" (or whatever). the lines below only run if it FAILED, for example when the command does not exist.
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

/*
* builtins are commands the shell runs ITSELF instead of launching a program. 
* cd and exit must work this way because they change the shell: a child process that did "cd" would change only itself, then vanish.
*/

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

/*
* run several commands connected by pipes.
*
* what is a pipe? pipe(fd) makes a hidden in-memory channel with TWO ends: fd[0] = the READ end, fd[1] = the WRITE end.
* whatever goes into the write end comes out of the read end.
*
*   ls | wc :    ls --writes--> [ pipe ] --reads--> wc
*
* so we make ls's stdout the write end and wc's stdin the read end, using dup2 just like we did for files.
*
* for N commands we need N-1 pipes:  A --pipe1--> B --pipe2--> C
*
* we handle one command per loop pass. "prev_read" holds the read end of the pipe that the PREVIOUS command writes into, 
* so the current command can read from it (-1 means "none", as for the first command).
*
* every process must close the pipe ends it does not use. a reader only sees "end of input" once
* EVERY copy of the write end is closed. if any process keeps one open by mistake, the reader waits forever and the shell hangs.
*
*   cmds    the commands (from minish_split_pipeline)
*   redirs  redirections for each command (same order as cmds)
*   n       number of commands
*/
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

/*
* redirection parsing: find < > >> in ONE command, remember them, and remove them from the word list.
*
* example: ["sort", "<", "in.txt", "-r", NULL] -> r->infile = "in.txt" and then the list becomes ["sort", "-r", NULL]
* so execvp only sees the real command and its real arguments.
*
* we use two positions in the list:
*   i = the word we are READING
*   j = where the next word we KEEP is written
* j never gets ahead of i, so we only overwrite words already looked at.
*
* returns 0 on success, -1 on a syntax error (like "ls >" with no file).
*/
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

/*
* decide how to run what the user typed.
*
*   1. cut the words into commands at each "|"
*   2. pull out redirections for every command, and check for mistakes
*   3. run it:
*        one command, builtin   -> run inside the shell
*        one command, program   -> fork + exec
*        several commands       -> run a pipeline
*
* returns 1 to keep the shell running, 0 to stop it (the "exit" builtin).
*/
int minish_execute(char **args) {
    if (args[0] == NULL) {
        return 1; // empty line.
    }

    int n = 0; // number of commands
    char ***cmds = minish_split_pipeline(args, &n);

    // one redirect record per command
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

    int result = 1; // default: keep the shell running
    if (ok) {
        if (n == 1) {
            // a single command. a builtin must run inside the shell itself, or "cd" and "exit" would have no effect.
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

    // cmds and redirs only hold pointers into args/line, so freeing these two arrays is enough. main frees args and line.
    free(redirs);
    free(cmds);
    return result;
}

int main(void) {
    char *line = NULL; // the text the user typed. getline allocates it.
    size_t capacity = 0; // how big line's buffer is. getline updates it.

    while(1) {
        // 1. prompt. fflush forces it to appear NOW. without it the text can sit in a buffer, because there is no newline at the end.
        printf("minish> ");
	    fflush(stdout);

        // 2. read a line. getline returns -1 when there is nothing more to read, for example when the user presses Ctrl+D. 
        // the & passes the ADDRESSES of line and capacity so getline can update them.
	    if (getline(&line, &capacity, stdin) == -1) {
	        break;
	    }   

        // 3. split into words. 
        // 4. run. then free the word LIST (the words themselves live inside line, which is freed once at the very end).
	    char **args = minish_split_line(line);
        int keep_running = minish_execute(args);
        free(args);

        // the "exit" builtin makes minish_execute return 0.
	    if (keep_running == 0) {
            break;
        }
    }

    free(line);
    
    // we may have left the loop because of a read error, not a normal Ctrl+D.
    if (ferror(stdin)) {
        perror("minish");
	    return 1;
    }

    return 0;
}
