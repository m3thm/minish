# MINISH

this is a small unix shell that I am writing to understand how unix runs and connects programs, using processes,
pipes and file descriptors, and signals directly rather than through abstractions.

## resources used

- [Write a Shell in C](https://brennan.io/2015/01/16/write-a-shell-in-c/) by Stephen Brennan.
- operating systems - three easy pieces, chapter 5.
- xv6: a simple, Unix-like teaching operating system, chapter 1

## how to run it yourself

compile the program by running `cc -Wall -Wextra main.c -o minish`
then run the program using `./minish`

you will see the prompt `minish> `. type a command and press enter. type `exit` or press Ctrl+D to quit.

## how to use it

```
minish> ls -l                          # run a program with arguments
minish> cd ..                          # change directory
minish> help                           # list the builtins
minish> echo hello > a.txt             # write output to a file (overwrites)
minish> echo world >> a.txt            # append output to a file
minish> sort < a.txt                   # read input from a file
minish> cat /etc/passwd | sort | head  # pipe commands together, any length
minish> cat < a.txt | sort > out.txt   # redirects and pipes can be combined
```

## things to know

- put spaces around `|`, `<`, `>` and `>>`. `ls|wc` will not work.
- quotes are not supported yet, so `echo "hi there"` will not behave like a normal shell.
- builtins can't be redirected, so `help > out.txt` ignores the `> out.txt` part.
- a builtin inside a pipeline runs in a child process, so `cd /tmp | ls` will not change the shell's directory.

## current features

- can run external commands with arguments (fork, execvp, waitpid)
- supports built-in commands (cd, help, exit)
- support input and output redirection (<, >, >>)
- support pipelines of any length (cmd1 | cmd2 | cmd3)

## goals for the future

- handle Ctrl+C so it interrupts the running command and not the shell
- run background jobs with & and reap finished children
