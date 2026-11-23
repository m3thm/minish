# MINISH

this is a small unix shell that I am writing to understand how unix runs and connect programs, using processes, 
pipes and file descriptors, and signals directly rather than through abstractions. 

## resouces used

currently following the tutorial by Stephen Brennan called [Write a Shell in C](https://brennan.io/2015/01/16/write-a-shell-in-c/).

## how to run it yourself

compile the program by running, 'cc -Wall -Wextra main.c -o minish'
then run the program using, './minish'

## current features

- can run external commands with arguments (fork, execvp, waitpid)
- supports built-in commands (cd, exit, pwd)
- support input and output directions (<, >, >>)

## goals for the future

- support pipelines of any length (cmd1 | cmd2 | cmd3)
- handle Ctrl+C so it interrups the running command and not the shell
- run background jobs with & and reap finished children.
