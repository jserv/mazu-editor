#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test if editor features work by direct invocation */

int test_file_content(const char *filename, const char *expected)
{
    FILE *f = fopen(filename, "r");
    if (!f)
        return 0;

    char buffer[1024];
    fgets(buffer, sizeof(buffer), f);
    fclose(f);

    /* Remove newline if present */
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';

    return strcmp(buffer, expected) == 0;
}

int test_redo()
{
    /* Create test file */
    FILE *f = fopen("test_redo.txt", "w");
    fprintf(f, "Initial");
    fclose(f);

    /* Create a pipe to send commands */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child process - run editor */
        close(pipefd[1]);
        dup2(pipefd[0], STDIN_FILENO);

        /* Redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);

        execlp("../me", "me", "test_redo.txt", NULL);
        exit(1);
    }

    /* Parent - send commands */
    close(pipefd[0]);

    usleep(100000); /* Wait for editor to start */

    /* Commands to test redo:
     * End of line, add " text", undo, redo, save, quit */
    write(pipefd[1], "\033[F", 3); /* End key */
    usleep(50000);
    write(pipefd[1], " text", 5); /* Add text */
    usleep(50000);
    write(pipefd[1], "\032", 1); /* Ctrl-Z (undo) */
    usleep(50000);
    write(pipefd[1], "\022", 1); /* Ctrl-R (redo) */
    usleep(50000);
    write(pipefd[1], "\023", 1); /* Ctrl-S (save) */
    usleep(50000);
    write(pipefd[1], "\021", 1); /* Ctrl-Q (quit) */

    close(pipefd[1]);

    /* Wait for editor to exit */
    int status;
    waitpid(pid, &status, 0);

    /* Check result */
    int result = test_file_content("test_redo.txt", "Initial text");

    unlink("test_redo.txt");
    return result;
}

int main()
{
    printf("Testing editor features...\n");

    if (test_redo()) {
        printf("✓ Redo functionality works\n");
    } else {
        printf("✗ Redo functionality failed\n");
    }

    return 0;
}
