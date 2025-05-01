#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>

// Task structure to replace the ShellTask class
struct ShellTaskInfo {
    std::vector<std::string> arguments;
    std::array<int, 2> inputOutput = {STDIN_FILENO, STDOUT_FILENO}; // [0]: input, [1]: output
    int outputDescriptor = STDOUT_FILENO;
    int errorDescriptor = STDERR_FILENO;
};

// Function to handle built-in commands
bool handleBuiltInCommands(const ShellTaskInfo& taskInfo) {
    if (taskInfo.arguments.empty()) {
        return false;
    }
    
    const std::string& cmd = taskInfo.arguments[0];
    
    if (cmd == "exit") {
        exit(0);
        return true;
    }
    
    if (cmd == "setenv" && taskInfo.arguments.size() >= 3) {
        setenv(taskInfo.arguments[1].c_str(), taskInfo.arguments[2].c_str(), 1);
        return true;
    }
    
    if (cmd == "printenv" && taskInfo.arguments.size() >= 2) {
        if (const char* envValue = getenv(taskInfo.arguments[1].c_str())) {
            std::cout << envValue << '\n';
        }
        return true;
    }
    
    return false; // Not a built-in command
}

// Function to execute an external command
void executeExternalCommand(const ShellTaskInfo& taskInfo) {
    // Convert args to C-style arguments for execvp
    std::vector<char*> execArgs(taskInfo.arguments.size() + 1);
    for (size_t i = 0; i < taskInfo.arguments.size(); i++) {
        execArgs[i] = strdup(taskInfo.arguments[i].c_str());
    }
    execArgs[taskInfo.arguments.size()] = nullptr;

    // Execute the command
    if (execvp(execArgs[0], execArgs.data()) == -1 && errno == ENOENT) {
        std::cerr << "Unknown command: [" << execArgs[0] << "].\n";
        exit(0);
    }
}

// Execute a shell task
void executeTask(const ShellTaskInfo& taskInfo) {
    // Handle built-in commands
    if (handleBuiltInCommands(taskInfo)) {
        return;
    }

    // Fork to execute external command
    pid_t processId;
    while ((processId = fork()) == -1) {
        if (errno == EAGAIN) {
            // Too many processes, wait for a child to release resources
            wait(nullptr);
        }
    }

    if (processId != 0) { // Parent process
        // Close pipe file descriptors in the parent
        if (taskInfo.inputOutput[0] != STDIN_FILENO) {
            close(taskInfo.inputOutput[0]);
            close(taskInfo.inputOutput[1]);
        }

        // Close output file descriptor if it's a regular file
        struct stat fileInfo;
        fstat(taskInfo.outputDescriptor, &fileInfo);
        if (taskInfo.outputDescriptor != STDOUT_FILENO && S_ISREG(fileInfo.st_mode)) {
            close(taskInfo.outputDescriptor);
        }

        // Wait for child if output isn't going to a pipe
        if (!S_ISFIFO(fileInfo.st_mode)) {
            waitpid(processId, nullptr, 0);
        }
        return;
    }

    // Child process
    // Set up stdin, stdout, stderr
    dup2(taskInfo.inputOutput[0], STDIN_FILENO);
    dup2(taskInfo.outputDescriptor, STDOUT_FILENO);
    dup2(taskInfo.errorDescriptor, STDERR_FILENO);

    // Execute the command
    executeExternalCommand(taskInfo);
}

// Update numbered pipes by decrementing their counters
void updatePipeNumbers(std::unordered_map<int, std::array<int, 2>>& pipeRegistry) {
    std::unordered_map<int, std::array<int, 2>> newRegistry;
    for (const auto& [num, fds] : pipeRegistry) {
        newRegistry[num - 1] = fds;  // Reduce pipe number
    }
    pipeRegistry = std::move(newRegistry);
}

// Function to create a new pipe
std::array<int, 2> createPipe() {
    std::array<int, 2> pipeFds;
    while (pipe(pipeFds.data()) == -1) {
        if (errno == EMFILE || errno == ENFILE) {
            wait(nullptr);  // Wait for resources
        }
    }
    fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
    return pipeFds;
}

// Function to handle a token (pipe, numbered pipe, output redirection)
void handleToken(const std::string& token, std::stringstream& tokenizer, 
                 std::vector<std::string>& currentArgs,
                 std::unordered_map<int, std::array<int, 2>>& pipeRegistry) {
    // Create and set up task object
    ShellTaskInfo taskInfo;
    taskInfo.arguments = std::move(currentArgs);
    currentArgs.clear();

    // Set input from previous pipe if available
    if (pipeRegistry.count(0)) {
        taskInfo.inputOutput = pipeRegistry[0];
        pipeRegistry.erase(0);
    }

    if (token[0] == '>') {
        // Handle output redirection
        std::string filename;
        std::getline(tokenizer, filename, ' ');
        taskInfo.outputDescriptor = open(filename.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0664);
    } else {
        // Handle pipe (regular or numbered)
        int pipeNum = 0;  // Default for regular pipe
        if (token.size() > 1) {
            pipeNum = std::stoi(token.substr(1));
        }

        // Create pipe if it doesn't exist
        if (pipeRegistry.count(pipeNum) == 0) {
            pipeRegistry[pipeNum] = createPipe();
        }

        // Set output to pipe
        taskInfo.outputDescriptor = pipeRegistry[pipeNum][1];
        
        // For ! pipe, also redirect stderr
        if (token[0] == '!') {
            taskInfo.errorDescriptor = pipeRegistry[pipeNum][1];
        }
    }

    // Execute the task
    executeTask(taskInfo);

    // Update pipe numbers if not a regular pipe
    if (token != "|") {
        updatePipeNumbers(pipeRegistry);
    }
}

// Process a line of input
void processInputLine(const std::string& inputLine, 
                      std::unordered_map<int, std::array<int, 2>>& pipeRegistry) {
    std::stringstream tokenizer(inputLine);
    std::string currentToken;
    std::vector<std::string> currentArgs;
    
    while (std::getline(tokenizer, currentToken, ' ')) {
        if (currentToken.empty()) {
            continue;
        }
        
        if (currentToken[0] == '|' || currentToken[0] == '!' || currentToken[0] == '>') {
            handleToken(currentToken, tokenizer, currentArgs, pipeRegistry);
        } else {
            // Add argument to current command
            currentArgs.push_back(currentToken);
        }
    }

    // Handle last command if any arguments remain
    if (!currentArgs.empty()) {
        ShellTaskInfo taskInfo;
        taskInfo.arguments = std::move(currentArgs);

        // Set input from previous pipe if available
        if (pipeRegistry.count(0)) {
            taskInfo.inputOutput = pipeRegistry[0];
            pipeRegistry.erase(0);
        }
        
        executeTask(taskInfo);
        updatePipeNumbers(pipeRegistry);
    }
}

int main() {
    // Set SIGCHLD to SIG_IGN to automatically reap child processes
    signal(SIGCHLD, SIG_IGN);
    
    // Set initial PATH
    setenv("PATH", "bin:.", 1);

    // Map to track numbered pipes: index -> [read_fd, write_fd]
    std::unordered_map<int, std::array<int, 2>> pipeRegistry;

    // Main shell loop
    while (true) {
        std::cout << "% ";
        std::cout.flush();

        std::string inputLine;
        std::getline(std::cin, inputLine);
        if (inputLine.empty()) {
            continue;
        }

        processInputLine(inputLine, pipeRegistry);
    }

    return 0;
}