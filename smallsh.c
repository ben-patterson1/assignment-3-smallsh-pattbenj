#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

volatile sig_atomic_t foreground_only_mode = 0;

struct Command {
	char* command;
	char* args[21];
	char* input_file;
	char* output_file;
	bool background;
};

void SIGTSTP_handler(int sig_num) {
	if (!foreground_only_mode) {
		foreground_only_mode = 1;
		char* message = "\nEntering foreground-only mode (& is now ignored)\n:";
		write(STDOUT_FILENO, message, strlen(message)); // Writes message to standard output, using write() instead of printf() because write() is reentrant
	}
	else {
		foreground_only_mode = 0;
		char* message = "\nExiting foreground-only mode\n:";
		write(STDOUT_FILENO, message, strlen(message)); // Writes message to standard output, using write() instead of printf() because write() is reentrant
	}
}

void check_background_processes(pid_t* bg_pids, int* bg_count) {
	int status;
	pid_t pid = waitpid(-1, &status, WNOHANG); // Checks if any child background processes have terminated, so the program can print the correct exit message

	while (pid > 0) {
		if (WIFEXITED(status)) { // Checks if the current child process terminated normally to print exit value
            printf("background pid %d is done: exit value %d\n", pid, WEXITSTATUS(status)); // Prints exit code
        } else if (WIFSIGNALED(status)) { // Checks if the current child process was killed by a signal to print terminating signal
            printf("background pid %d is done: terminated by signal %d\n", pid, WTERMSIG(status)); // Prints signal number
        }

		for (int i = 0; i < *bg_count; i++) {
			if (bg_pids[i] == pid) {
				for (int j = i; j < *bg_count - 1; j++) {
					bg_pids[j] = bg_pids[j + 1];
				}
				bg_count--;
				break;
			}
		}

		pid = waitpid(-1, &status, WNOHANG); // Checks if any other child background processes have terminated, so the program can print the correct exit message
	}
}

void shell_variable_expansion(char* command, size_t size) {
	char buffer[size * 2];
	char pid[32];
	char* src = command;
	char* dst = buffer;

	// Writes PID to pid variable
	snprintf(pid, sizeof(pid), "%d", getpid()); // getpid() gets process id to store

	size_t pid_len = strlen(pid);

	while (*src) {
		if (src[0] == '$' && src[1] == '$') {
			memcpy(dst, pid, pid_len); // Copies memory from pid to dst
			dst += pid_len;
			src += 2;
		}
		else {
			*dst = *src;
			dst++;
			src++;
		}
	}

	*dst = '\0';

	strncpy(command, buffer, size - 1); // Copies buffer to command
	command[size - 1] = '\0';
}

void tokenize_command(char* command, struct Command* cmd) {
	char* token = strtok(command, " "); // Tokenizes command ending at empty space, and gets first argument of command
	int count = 0;

	cmd->command = NULL;
	cmd->input_file = NULL;
	cmd->output_file = NULL;
	cmd->background = false;

	while (token != NULL) {
		if (strcmp(token, "<") == 0) { // Checks if argument is "<" to know if there's an input file
			token = strtok(NULL, " "); // Gets input file name from next token to store
			cmd->input_file = token;
		}
		else if (strcmp(token, ">") == 0) { // Checks if argument is "<" to know if there's an output file
			token = strtok(NULL, " "); // Gets output file name from next token to store
			cmd->output_file = token;
		}
		else if (strcmp(token, "&") == 0) { // Checks if argument is "&" to know if command runs in background
			cmd->background = true;
		}
		else if (!cmd->command) {
			cmd->command = token;
		}
		else {
			cmd->args[count] = token;
			count++;
		}
		token = strtok(NULL, " "); // Get next token for next iteration
	}
	cmd->args[count] = NULL;
}

int execute_command(struct Command* cmd, pid_t* bg_pids, int* bg_count, int* children) {
	if (*children >= 10) {
		return 1;
	}

	int exit_status = 0;
	pid_t pid = fork();

	struct sigaction default_action = {0};
	default_action.sa_handler = SIG_DFL;

	struct sigaction ignore_action = {0};
	ignore_action.sa_handler = SIG_IGN;

	if (pid == -1) return exit_status;
	if (pid == 0) {
		if (!cmd->background) {
			sigaction(SIGINT, &default_action, NULL); // Sets the call handling of the interrupt signal to the default action, so that foreground child processes are terminated by CTRL+C
		}
		
		sigaction(SIGTSTP, &ignore_action, NULL); // Ignores SIGTSTP so child processes ignore CTRL+Z
		

		int fd;

		if (cmd->input_file) {
			fd = open(cmd->input_file, O_RDONLY); // Opens input file from command to read only so that the file can be set to standard input
			if (fd == -1) {
				printf("bash: %s: No such file or directory\n", cmd->input_file);
				exit(1); // Terminate child process because of error
			}
			dup2(fd, STDIN_FILENO); // Redirects standard input to file descriptor so process reads from file
			close(fd); // Closes file descriptor
		}

		if (cmd->output_file) {
			fd = open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644); // Opens output file from command to write only, truncates if file exists, and creates file if it doesn't exist (so that file can be set to standard output)
			if (fd == -1) {
				printf("bash: %s: No such file or directory\n", cmd->output_file);
				exit(1); // Terminate child process because of error
			}
			dup2(fd, STDOUT_FILENO); // Redirects standard output to file descriptor so process writes to file
			close(fd); // Closes file descriptor
		}

		char* cmd_args[22];
		cmd_args[0] = cmd->command;
		int i = 0;
		while (cmd->args[i] != NULL && i < 20) {
			cmd_args[i + 1] = cmd->args[i];
			i++;
		}
		cmd_args[i + 1] = NULL;

		if (execvp(cmd->command, cmd_args) == -1) { // Replaces current process image with a new process image (current process stops program to execute new program) to run the command program specified by the user
			printf("bash: %s: command not found\n", cmd->command);
    		exit(1); // Terminate child process because of error
		}
	}
	else {
		if (!cmd->background) {
			int status;
			waitpid(pid, &status, 0); // Wait until the foreground child process finishes or terminates, because it is a foreground process and shell needs to wait until it finishes
			if (WIFEXITED(status)) { // Checks if the child process terminated normally to store exit value
				exit_status = WEXITSTATUS(status); // Gets exit code to store
			}
			else if (WIFSIGNALED(status)) { // Checks if the child process was killed by a signal to print terminating signal
				exit_status = WTERMSIG(status); // Gets signal number to store and print
				printf("\nterminated by signal %d\n", exit_status);
			}
		}
		else {
			if (*bg_count < 100) {
				bg_pids[*bg_count] = pid;
				(*bg_count)++;
			}
			printf("background pid is %d\n", pid);
		}
	}
	return exit_status;
}

int main() {
	char command[2049];
	int exit_status = 0;
	pid_t bg_pids[100];
	int bg_count = 0;
	int children = 0;

	struct sigaction ignore_action = {0};
	ignore_action.sa_handler = SIG_IGN;
	sigaction(SIGINT, &ignore_action, NULL); // Ignores SIGINT so processes ignore CTRL+C

	struct sigaction handler = {0};
	handler.sa_handler = SIGTSTP_handler;
	sigfillset(&handler.sa_mask); // Blocks all signals during handling so handler isn't interrupted during execution
	handler.sa_flags = SA_RESTART; // If signal interrupts system call, restart the system call after signal is handled
	sigaction(SIGTSTP, &handler, NULL); // Run handler on SIGTSTP, so that CTRL+Z toggles foreground only mode

	while (1) {
		check_background_processes(bg_pids, &bg_count);
		printf(":");
		fflush(stdout); // Flush command prompt so that it is immediately displayed

		if (fgets(command, sizeof(command), stdin) == NULL) break; // Reads from standard input to get command and break if it is NULL
		command[strcspn(command, "\n")] = '\0'; // Scans for newline and replaces it with null terminator for correct format of C string

		if (command[0] == '\0' || command[0] == '#') continue;
		if (strstr(command, "$$")) { // Checks if command has "$$" so they can be replaced by pid
			shell_variable_expansion(command, sizeof(command));
			printf("%s\n", command);
		}

		struct Command cmd;
		tokenize_command(command, &cmd);

		if (foreground_only_mode && cmd.background) {
			cmd.background = false;
		}

		if (strcmp(cmd.command, "exit") == 0) { // Checks if command is exit so that program can terminate
			for (int i = 0; i < bg_count; i++) {
				kill(bg_pids[i], SIGTERM); // Force-kills all background child processes so that all processes can be terminated
			}
			break;
		}
		else if (strcmp(cmd.command, "cd") == 0) { // Checks if command is cd so that directory can be changed
			if (!cmd.args[0]) {
				chdir(getenv("HOME")); // Gets HOME environment variable path and changes current working directory to it (this happens when cd has no arguments)
			}
			else {
				chdir(cmd.args[0]); // Changes current working directory to the argument if an argument is given
			}
		}
		else if (strcmp(cmd.command, "status") == 0) { // Checks if command is status so that previous exit status can be printed
			printf("exit status %d\n", exit_status);
		}
		else {
			exit_status = execute_command(&cmd, bg_pids, &bg_count, &children);
		}
	}
	return 0;
}
