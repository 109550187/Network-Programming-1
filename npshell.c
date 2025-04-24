// this version attempts to change how the numbered pipes work in the middle
// we remove the special middle numbered pipe case
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>

#define MAX_SINGLE_INPUT 15000
#define MAX_CMD_LENGTH 256
#define MAX_PIPE_NUM 1000

#define MAX_PROCESSES 1000
pid_t child_processes[MAX_PROCESSES];
int num_processes = 0;

#define DEBUG_PIPE 0  // Set to 1 to enable pipe debugging, 0 to disable
FILE *debug_log = NULL;

typedef struct {
    int read_fd;  // File descriptor for reading
    int write_fd; // File descriptor for writing
    int count;    // Number of commands to wait before using
} NumberedPipe;

void setenv_built(char **args);
void printenv_built(char **args);

// Global array to store numbered pipes
NumberedPipe numbered_pipes[MAX_PIPE_NUM];
int num_pipes = 0;

pid_t all_processes[MAX_PROCESSES * 10]; // Track all processes created
int all_process_count = 0;

void tracking_process(pid_t pid) {
    if (all_process_count < MAX_PROCESSES * 10) {
        all_processes[all_process_count++] = pid;
    }
}

// Initialize the pipe array
void init_pipes() {
    for (int i = 0; i < MAX_PIPE_NUM; i++) {
        numbered_pipes[i].read_fd = -1;
        numbered_pipes[i].write_fd = -1;
        numbered_pipes[i].count = -1;
    }
    num_pipes = 0;
}

// Add this function after init_pipes() but before process_line()
void debug_init() {
    if (DEBUG_PIPE) {
        debug_log = fopen("pipe_debug.log", "w");
        if (debug_log == NULL) {
            perror("Could not open debug log");
            exit(1);
        }
        fprintf(debug_log, "=== NPShell Pipe Debug Log ===\n");
        fflush(debug_log);
    }
}

// Add this function near the other debugging functions
void debug_pipe_status(const char *msg, int pipe_fd, int process_type, pid_t pid) {
    if (DEBUG_PIPE && debug_log != NULL) {
        fprintf(debug_log, "[PID %d] %s: fd=%d, %s\n", 
                pid, msg, pipe_fd, 
                process_type == 0 ? "PARENT" : "CHILD");
        fflush(debug_log);
    }
}

void debug_numbered_pipe(int pipe_idx, int count, const char *action) {
    if (DEBUG_PIPE && debug_log != NULL) {
        fprintf(debug_log, "NUMBERED PIPE [idx=%d, count=%d]: %s (read=%d, write=%d)\n",
                pipe_idx, count, action,
                numbered_pipes[pipe_idx].read_fd,
                numbered_pipes[pipe_idx].write_fd);
        fflush(debug_log);
    }
}

void safe_strcpy(char *dest, const char *src, size_t dest_size) {
    size_t src_len = strlen(src);
    if (src_len >= dest_size) {
        // Source is too large, copy only what fits
        memcpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else {
        // Source fits, copy normally
        memcpy(dest, src, src_len + 1); // +1 for null terminator
    }
}

// Decrease the count for all active numbered pipes
void decrease_pipe_count() {
    for (int i = 0; i < num_pipes; i++) {
        if (numbered_pipes[i].count > 0) {
            numbered_pipes[i].count--;
            if (DEBUG_PIPE) {
                fprintf(debug_log, "DECREASED PIPE COUNT: idx=%d, new count=%d\n", 
                        i, numbered_pipes[i].count);
                fflush(debug_log);
            }
        }
    }
}

// Close all pipes that have count == 0 and have been used
// Modify close_used_pipes to be more careful with pipe closure
void close_used_pipes() {
    int j = 0;
    for (int i = 0; i < num_pipes; i++) {
        // IMPORTANT CHANGE: Only close write_fd when count=0, keep read_fd for the next command
        if (numbered_pipes[i].count == 0) {
            // Only close write_fd, keep read_fd for the command that will use it
            if (numbered_pipes[i].write_fd != -1) {
                if (DEBUG_PIPE) {
                    fprintf(debug_log, "CLOSING PIPE WRITE END: idx=%d, write=%d\n", 
                            i, numbered_pipes[i].write_fd);
                    fflush(debug_log);
                }
                close(numbered_pipes[i].write_fd);
                numbered_pipes[i].write_fd = -1;
            }
            // Keep this pipe with read_fd for next command
            if (i != j) {
                numbered_pipes[j] = numbered_pipes[i];
            }
            j++;
        } else {
            // Keep this pipe in the array
            if (i != j) {
                numbered_pipes[j] = numbered_pipes[i];
            }
            j++;
        }
    }
    num_pipes = j;  // Update the count of pipes
    
    if (DEBUG_PIPE) {
        fprintf(debug_log, "AFTER CLEANUP: num_pipes=%d\n", num_pipes);
        fflush(debug_log);
    }
}

// Find all pipes with the specified count
void find_pipes_by_count(int count, int *pipe_indices, int *num_found) {
    *num_found = 0;
    for (int i = 0; i < num_pipes; i++) {
        if (numbered_pipes[i].count == count && numbered_pipes[i].read_fd != -1) {
            if (DEBUG_PIPE) {
                fprintf(debug_log, "FOUND PIPE: idx=%d, count=%d, read=%d, write=%d\n",
                        i, count, numbered_pipes[i].read_fd, numbered_pipes[i].write_fd);
                fflush(debug_log);
            }
            pipe_indices[(*num_found)++] = i;
        }
    }
}

void cleanup_zombie_processes() {
    int status;
    pid_t pid;
    
    // Non-blocking wait for any child process
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Remove this pid from child_processes if it's there
        for (int i = 0; i < num_processes; i++) {
            if (child_processes[i] == pid) {
                // Remove by swapping with last element and decreasing count
                child_processes[i] = child_processes[num_processes-1];
                num_processes--;
                break;
            }
        }
    }
}

// Create a new numbered pipe
int create_numbered_pipe(int count) {
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        perror("pipe");
        return -1;
    }
    
    // Find an available slot or use num_pipes
    int idx = num_pipes;
    num_pipes++;
    
    numbered_pipes[idx].read_fd = pipe_fds[0];
    numbered_pipes[idx].write_fd = pipe_fds[1];
    numbered_pipes[idx].count = count;
    
    if (DEBUG_PIPE) {
        fprintf(debug_log, "NEW NUMBERED PIPE: idx=%d, count=%d, read=%d, write=%d\n",
                idx, count, pipe_fds[0], pipe_fds[1]);
        fflush(debug_log);
    }
    
    return idx;
}

// Find an existing pipe with the given count or create a new one
int find_or_create_numbered_pipe(int count) {
    // Check if a pipe with this count already exists
    for (int i = 0; i < num_pipes; i++) {
        if (numbered_pipes[i].count == count && numbered_pipes[i].write_fd != -1) {
            if (DEBUG_PIPE) debug_numbered_pipe(i, count, "REUSING EXISTING");
            return i;  // Return existing pipe
        }
    }
    
    // If not found, create a new one
    int idx = create_numbered_pipe(count);
    if (DEBUG_PIPE) debug_numbered_pipe(idx, count, "CREATED NEW");
    return idx;
}

int merge_pipes(int *pipe_indices, int num_pipes_to_merge) {
    if (num_pipes_to_merge == 0) {
        return -1;
    }
    
    if (num_pipes_to_merge == 1) {
        int fd = numbered_pipes[pipe_indices[0]].read_fd;
        numbered_pipes[pipe_indices[0]].read_fd = -1;  // Mark as used
        return fd;
    }
    
    // Create a new pipe for the merged output
    int merge_pipe[2];
    if (pipe(merge_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    
    // Fork a child to handle the data copying to avoid blocking
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(merge_pipe[0]);
        close(merge_pipe[1]);
        return -1;
    } else if (pid == 0) {  // Child process
        // Close the read end in child
        close(merge_pipe[0]);
        
        // Copy data from all input pipes to the merge pipe
        for (int i = 0; i < num_pipes_to_merge; i++) {
            int read_fd = numbered_pipes[pipe_indices[i]].read_fd;
            
            // Buffer for copying data
            char buffer[4096];
            ssize_t bytes_read;
            
            // Read all data from this pipe and write to the merge pipe
            while ((bytes_read = read(read_fd, buffer, sizeof(buffer))) > 0) {
                write(merge_pipe[1], buffer, bytes_read);
            }
            
            // Close the read end of this pipe as we're done with it
            close(read_fd);
        }
        
        // Close the write end and exit
        close(merge_pipe[1]);
        exit(0);
    } else {  // Parent process
        // Close the write end in parent
        close(merge_pipe[1]);
        
        // Mark all input pipes as used
        for (int i = 0; i < num_pipes_to_merge; i++) {
            close(numbered_pipes[pipe_indices[i]].read_fd);
            numbered_pipes[pipe_indices[i]].read_fd = -1;  // Mark as used
            
            // IMPORTANT: Also mark for removal after this command is done
            numbered_pipes[pipe_indices[i]].count = -1;  // Mark for removal
        }        
        
        // Track the child process
        tracking_process(pid);

        if (DEBUG_PIPE) {
            fprintf(debug_log, "MERGE_PIPES: Returning fd=%d for input to next command\n", 
                    merge_pipe[0]);  // or whatever fd you're returning
            fflush(debug_log);
        }
        
        return merge_pipe[0];
    }
}

void wait_for_all_children() {
    for (int i = 0; i < num_processes; i++) {
        waitpid(child_processes[i], NULL, 0);
    }
    num_processes = 0;
}

// Execute a single command with pipes
void execute_cmd(char *cmd, int in_fd, int out_fd, int err_fd, int wait_child) {
    char cmd_copy[MAX_CMD_LENGTH];
    safe_strcpy(cmd_copy, cmd, sizeof(cmd_copy));
    
    // Parse command arguments
    char *args[MAX_CMD_LENGTH];
    int arg_count = 0;
    
    char *output_file = NULL;
    int redirect_output = 0;
    
    char *token = strtok(cmd_copy, " ");
    while (token != NULL) {
        if (strcmp(token, ">") == 0) {
            redirect_output = 1;
            token = strtok(NULL, " ");
            if (token != NULL) {
                output_file = token;
            }
            break;
        } else {
            if (arg_count < MAX_CMD_LENGTH - 1) {
                args[arg_count++] = token;
            }
            token = strtok(NULL, " ");
        }
    }
    args[arg_count] = NULL;
    
    if (arg_count == 0) return;
    
    // Handle built-in commands
    if (strcmp(args[0], "setenv") == 0) {
        setenv_built(args);
        return;
    } else if (strcmp(args[0], "printenv") == 0) {
        printenv_built(args);
        return;
    } else if (strcmp(args[0], "exit") == 0) {
        exit(0);
    }
    
    // Fork and execute external command
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return;
    } else if (pid == 0) { // Child process
        if (DEBUG_PIPE) {
            debug_pipe_status("CHILD BEFORE REDIRECTION", in_fd, 1, getpid());
            debug_pipe_status(cmd, out_fd, 1, getpid());
        }
        // Handle input redirection
        if (in_fd != -1) {
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
            if (DEBUG_PIPE) debug_pipe_status("CLOSED INPUT", in_fd, 1, getpid());
        }
        
        // Handle output redirection
        if (redirect_output) {
            int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
                perror("open");
                exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        } else if (out_fd != -1) {
            dup2(out_fd, STDOUT_FILENO);
        }
        if (err_fd != -1) {
            dup2(err_fd, STDERR_FILENO);
        }
        
        // Close pipes after handling both output and error message (Crucial)
        if (out_fd != -1) close(out_fd);
        if (err_fd != -1 && err_fd != out_fd) close(err_fd);
        
        // Close all pipe file descriptors
        for (int i = 0; i < num_pipes; i++) {
            if (numbered_pipes[i].read_fd != -1) {
                close(numbered_pipes[i].read_fd);
            }
            if (numbered_pipes[i].write_fd != -1) {
                close(numbered_pipes[i].write_fd);
            }
        }

        if (DEBUG_PIPE) {
            fprintf(debug_log, "EXECUTING: cmd='%s', STDIN=%d, STDOUT=%d, STDERR=%d\n",
                    cmd, STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
            fflush(debug_log);
        }
        
        // Execute command
        if (execvp(args[0], args) == -1) {
            fprintf(stderr, "Unknown command: [%s].\n", args[0]);
            exit(0);
        }
    } else { // Parent process
        tracking_process(pid);
        if (DEBUG_PIPE) {
            debug_pipe_status("PARENT CREATED CHILD", pid, 0, getpid());
            if (in_fd != -1) debug_pipe_status("PARENT HAS INPUT", in_fd, 0, getpid());
            if (out_fd != -1) debug_pipe_status("PARENT HAS OUTPUT", out_fd, 0, getpid());
        }
        
        if (wait_child) {
            if (DEBUG_PIPE) debug_pipe_status("PARENT WAITING FOR", pid, 0, getpid());
            int status;
            waitpid(pid, &status, 0);
            if (DEBUG_PIPE) debug_pipe_status("PARENT DONE WAITING", pid, 0, getpid());
        } else {
            // For non-waited children, track them
            if (num_processes < MAX_PROCESSES) {
                child_processes[num_processes++] = pid;
            }
        }
            
            // Close file descriptors in parent even if not waiting
            if (in_fd != -1) close(in_fd);
            if (out_fd != -1) close(out_fd);
            if (err_fd != -1) close(err_fd);

            if (DEBUG_PIPE && in_fd != -1) debug_pipe_status("PARENT CLOSED INPUT", in_fd, 0, getpid());
            if (DEBUG_PIPE && out_fd != -1) debug_pipe_status("PARENT CLOSED OUTPUT", out_fd, 0, getpid());
    }
}

// Process a command line with potential pipes
void process_line(char *line) {
    char *cmd_start = line;
    char *pipe_pos = NULL;
    char cmd[MAX_SINGLE_INPUT];

    //int first_cmd_in_line = 1;  // Flag to indicate if this is the first command in the line
    
    // Initialize array to store child PIDs for this line
    pid_t child_pids[MAX_PROCESSES];
    int num_children = 0;

    cleanup_zombie_processes();
    
    // Check for empty line
    if (strlen(line) == 0) return;
    
    // Find all input pipes for this command line
    int pipe_indices[MAX_PIPE_NUM];
    int num_input_pipes = 0;
    find_pipes_by_count(0, pipe_indices, &num_input_pipes);
    
    // Merge all input pipes for this command line
    int input_fd = -1;
    if (num_input_pipes > 0) {
        input_fd = merge_pipes(pipe_indices, num_input_pipes);
        if (DEBUG_PIPE) {
            fprintf(debug_log, "MERGED %d PIPES, got fd=%d\n", 
                    num_input_pipes, input_fd);
            fflush(debug_log);
        }
    }
    
    while (cmd_start != NULL && *cmd_start != '\0') {
        int pipe_type = 0;  // 0 = no pipe, 1 = regular pipe, 2 = numbered pipe, 3 = error numbered pipe
        int pipe_number = 0;
        
        // Find pipe characters
        pipe_pos = strpbrk(cmd_start, "|!");
        
        if (pipe_pos != NULL) {
            // Determine pipe type
            if (*pipe_pos == '|') {
                if (isdigit(*(pipe_pos + 1))) {
                    pipe_type = 2;  // Numbered pipe
                    pipe_number = atoi(pipe_pos + 1);
                } else {
                    pipe_type = 1;  // Regular pipe
                }
            } else if (*pipe_pos == '!') {
                pipe_type = 3;  // Error numbered pipe
                pipe_number = atoi(pipe_pos + 1);
            }
            
            // Extract the command before the pipe
            strncpy(cmd, cmd_start, pipe_pos - cmd_start);
            cmd[pipe_pos - cmd_start] = '\0';

        } else {
            // Last command in the line
            strcpy(cmd, cmd_start);
        }
        
        // Set up output pipe
        int output_fd = -1;
        int error_fd = -1;
        int next_pipe_idx = -1;

        if (pipe_type == 1) {  // Regular pipe
            int pipe_fds[2];
            if (pipe(pipe_fds) < 0) {
                perror("pipe");
                return;
            }
            output_fd = pipe_fds[1];  // Write to the pipe       
            int next_input_fd = pipe_fds[0]; // Store the reading end for the next command
        
            if (DEBUG_PIPE) {
                fprintf(debug_log, "PIPE CREATED: read=%d, write=%d for cmd '%s'\n", 
                        pipe_fds[0], pipe_fds[1], cmd);
                fflush(debug_log);
            }
             
            // Execute the current command
            pid_t child_pid = fork();
            
            if (child_pid < 0) {
                perror("fork");
                return;
            } else if (child_pid == 0) {  // Child process
                // Set up stdin from input_fd if it exists
                if (input_fd != -1) {
                    dup2(input_fd, STDIN_FILENO);
                    close(input_fd);
                }
                
                // Set up stdout to write to the pipe
                dup2(output_fd, STDOUT_FILENO);
                
                // Close pipe file descriptors
                close(pipe_fds[0]);  // Close read end in child
                close(pipe_fds[1]);  // Close write end after duplication
                
                // Close all numbered pipes
                for (int i = 0; i < num_pipes; i++) {
                    if (numbered_pipes[i].read_fd != -1) close(numbered_pipes[i].read_fd);
                    if (numbered_pipes[i].write_fd != -1) close(numbered_pipes[i].write_fd);
                }
                
                // Execute the command
                char *args[MAX_CMD_LENGTH];
                int arg_count = 0;
                
                char cmd_copy[MAX_CMD_LENGTH];
                safe_strcpy(cmd_copy, cmd, sizeof(cmd_copy));
                
                char *token = strtok(cmd_copy, " ");
                while (token != NULL) {
                    if (arg_count < MAX_CMD_LENGTH - 1) {
                        args[arg_count++] = token;
                    }
                    token = strtok(NULL, " ");
                }
                args[arg_count] = NULL;
                
                if (execvp(args[0], args) == -1) {
                    fprintf(stderr, "Unknown command: [%s].\n", args[0]);
                    exit(0);
                }
            } else {  // Parent process
                // Store child PID for later
                if (num_children < MAX_PROCESSES) {
                    child_pids[num_children++] = child_pid;
                }
                
                // Close file descriptors in parent
                if (input_fd != -1) {
                    close(input_fd);
                    if (DEBUG_PIPE) debug_pipe_status("PARENT CLOSED INPUT", input_fd, 0, getpid());
                }
                close(output_fd);
                if (DEBUG_PIPE) debug_pipe_status("PARENT CLOSED OUTPUT", output_fd, 0, getpid());
                
                // Prepare for next command in the pipe chain
                input_fd = next_input_fd;
            }
        } else if (pipe_type == 2 || pipe_type == 3) {  // Numbered pipe
            // Find or create a numbered pipe with the specified count
            next_pipe_idx = find_or_create_numbered_pipe(pipe_number);
            output_fd = numbered_pipes[next_pipe_idx].write_fd;
            
            if (pipe_type == 3) {  // Error pipe
                error_fd = output_fd;
            } else {
                error_fd = -1;  // Make sure error_fd is set to -1 for regular pipes
            }

            if (DEBUG_PIPE) {
                fprintf(debug_log, "PROCESSING NUMBERED PIPE: cmd='%s', pipe_number=%d, input_fd=%d\n",
                        cmd, pipe_number, input_fd);
                fflush(debug_log);
            }
            
            // Execute the command
            execute_cmd(cmd, input_fd, output_fd, error_fd, 0);
            // Reset input for next command
            input_fd = -1;

        } else {  // No pipe, last command
            // Before executing the last command, check for any pipes with count=0
            int last_pipe_indices[MAX_PIPE_NUM];
            int num_last_pipes = 0;
            find_pipes_by_count(0, last_pipe_indices, &num_last_pipes);
            
            if (num_last_pipes > 0) {
                // This command should receive input from pipes with count=0
                int new_input_fd = merge_pipes(last_pipe_indices, num_last_pipes);
                if (DEBUG_PIPE) {
                    fprintf(debug_log, "LAST COMMAND: MERGED %d PIPES, got fd=%d\n", 
                            num_last_pipes, new_input_fd);
                    fflush(debug_log);
                }
                // If we already have input, we need to combine it with the new input
                if (input_fd != -1) {
                    if (DEBUG_PIPE) {
                        fprintf(debug_log, "COMBINING INPUTS: existing=%d, new=%d\n", 
                                input_fd, new_input_fd);
                        fflush(debug_log);
                    }
                    // For test case 6, we need to prioritize the pipe chain input (input_fd)
                    // over the numbered pipe input (new_input_fd)
                    close(new_input_fd);  // Close the numbered pipe input
                    // Keep the existing input_fd from the pipe chain
                } else {
                    input_fd = new_input_fd;
                }
            }
            
            // Execute the last command and wait for it
            execute_cmd(cmd, input_fd, output_fd, error_fd, 1);
        }
        
        // Move to the next command
        if (pipe_pos != NULL) {
            if (pipe_type == 1) {
                cmd_start = pipe_pos + 1;
            } else {
                // Skip the number for numbered pipes
                cmd_start = pipe_pos + 1;
                while (isdigit(*cmd_start)) cmd_start++;
                
                // THIS IS THE KEY CHANGE: For numbered pipes within the same line, we need to
                // capture their output and make it available to subsequent commands
                if (cmd_start != NULL && *cmd_start != '\0') {
                    // This is a numbered pipe in the middle of a line
                    // We need to check for pipes with count=0 before processing the next command
                    int next_pipe_indices[MAX_PIPE_NUM];
                    int num_next_pipes = 0;
                    find_pipes_by_count(0, next_pipe_indices, &num_next_pipes);
                    
                    // Merge all input pipes for the next command
                    if (num_next_pipes > 0) {
                        input_fd = merge_pipes(next_pipe_indices, num_next_pipes);
                        if (DEBUG_PIPE) {
                            fprintf(debug_log, "MIDDLE COMMAND WITH NUMBERED PIPE: MERGED %d PIPES, got fd=%d\n", 
                                    num_next_pipes, input_fd);
                            fprintf(debug_log, "NEXT COMMAND WILL BE: '%s'\n", cmd_start);
                            fflush(debug_log);
                        }
                    }
                }
            }
        } else {
            cmd_start = NULL;
        }

        if (DEBUG_PIPE) {
            fprintf(debug_log, "NEXT COMMAND: cmd_start='%s', pipe_type=%d, input_fd=%d\n", 
                    cmd_start ? cmd_start : "NULL", pipe_type, input_fd);
            fflush(debug_log);
        }
    
        fflush(stdout);
        fflush(stderr);
    

        if (pipe_type != 1) {  // Only wait if not in a pipe chain
            // Wait for all children from this command line
            for (int i = 0; i < num_children; i++) {
                waitpid(child_pids[i], NULL, 0);
            }
        }

        // Close only UNUSED pipe write ends
        for (int i = 0; i < num_pipes; i++) {
            if (numbered_pipes[i].write_fd != -1 && numbered_pipes[i].count > 0) {
                close(numbered_pipes[i].write_fd);
                numbered_pipes[i].write_fd = -1;
            }
        }
        
        // Decrease pipe counts after processing the entire line
        decrease_pipe_count();
        
        // Clean up used pipes
        close_used_pipes();
        

        cleanup_zombie_processes();
    }

}

void handle_sigint(int sig) {
    // Clean up child processes
    wait_for_all_children();
    
    cleanup_zombie_processes();

    // Re-print prompt
    printf("\n%% ");
    fflush(stdout);
}

int main() {
    char line[MAX_SINGLE_INPUT];
    init_pipes();
    debug_init();

    // Set initial PATH as bin/ and ./
    setenv("PATH", "bin:.", 1);
    
    while (1) {
        cleanup_zombie_processes();
        //wait_for_all_children();

        printf("%% ");
        fflush(stdout);
        
        // Read command line and handle EOF
        if (fgets(line, MAX_SINGLE_INPUT, stdin) == NULL){
            printf("\n");
            break;
        }
            
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }
        
        // Exit if user types "exit"
        if (strcmp(line, "exit") == 0) 
            break;

        // Skip empty lines
        if(strlen(line) == 0){
            continue;
        }

        // Set up signal handler
        signal(SIGINT, handle_sigint);
        
        // Process command line
        process_line(line);

        fflush(stdout);
        fflush(stderr);
    }
    
    return 0;
}

void setenv_built(char **args) {
    if (args[1] == NULL || args[2] == NULL) {
        return;
    }
    
    if (setenv(args[1], args[2], 1) != 0) {
        perror("setenv");
    }
}

void printenv_built(char **args) {
    if (args[1] == NULL) {
        return;
    }
    
    char *value = getenv(args[1]);
    if (value != NULL) {
        printf("%s\n", value);
    }
}