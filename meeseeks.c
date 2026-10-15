/*
 * ============================================================================
 *  Instituto Tecnológico de Costa Rica
 *  Campus Tecnológico Central Cartago
 *  Escuela de Ingeniería en Computación
 *  Carrera: Ingeniería en Computación
 *  Curso: IC-6600 Principios de Sistemas Operativos
 *  Periodo: II Semestre 2026
 *  Profesor: M.Sc. Esteban Arias Méndez
 *  Estudiante: Brian Esteban Ramírez Arias
 *  Carné: 2024109557
 *  Fecha: 14 de octubre de 2026
 *
 *  Tarea #3 - Mr. Meeseeks
 * ============================================================================
 *
 *  Descripción
 *
 *  Este programa simula la caja de Mr. Meeseeks de la serie Rick and Morty.
 *  La caja recibe pedidos del usuario y, por cada pedido, crea un proceso
 *  nuevo que intenta cumplirlo.
 *
 *  El programa acepta tres tipos de pedidos:
 *
 *  1. Tareas de texto, por ejemplo "Quiero ser feliz". Cada tarea tiene una
 *     dificultad de 0 a 100, donde 100 es muy fácil y 0 es imposible. Si un
 *     Meeseeks no logra la tarea, crea otros Meeseeks para que lo ayuden, y
 *     la tarea se vuelve un poco más fácil en cada nivel de ayudantes.
 *
 *  2. Cálculos, por ejemplo (3 + 4) * 2 > 10. Un solo Meeseeks resuelve la
 *     operación aritmética o lógica y muestra el resultado.
 *
 *  3. Programas externos, por ejemplo ls -l. El Meeseeks ejecuta el
 *     programa y la tarea se cumple si el programa termina sin errores.
 *
 *  Los Meeseeks se comunican entre sí con pipes, y comparten los contadores
 *  de niveles e instancias en memoria compartida protegida con un semáforo.
 *
 *  Cuando el usuario se despide, la caja muestra un resumen de todas las
 *  tareas:
 *    - Cuánto duró cada una.
 *    - Si se cumplió.
 *    - Cuántos Meeseeks hicieron falta. 
 *   Si una tarea tarda más del tiempo máximo, se declara caos planetario y se terminan todos los Meeseeks.
 *
 *  Compilar: gcc -pthread meeseeks.c -o meeseeks
 *  Ejecutar: ./meeseeks
 * ============================================================================
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define INPUT_SIZE 256
#define MAX_LEVELS 64
#define MAX_FILE_DESCRIPTOR 1024

typedef struct {
    pid_t pid;
    pid_t ppid;
    int level;
    int instance;
} MeeseeksIdentity;

typedef struct {
    sem_t lock;
    int instancesPerLevel[MAX_LEVELS];
    int totalCreated;
    int aliveCount;
    int maxLevel;
} SharedState;

typedef enum { TASK_TEXT, TASK_CALCULATE, TASK_EXECUTE } TaskType;

typedef enum { MESSAGE_REQUEST, MESSAGE_SOLVED, MESSAGE_FAILED, MESSAGE_STOP } MessageType;

typedef struct {
    MessageType type;
    TaskType taskType;
    char text[INPUT_SIZE];
    MeeseeksIdentity solver;
} Message;

typedef struct {
    pid_t pid;
    int toChild;
    int fromChild;
} ChildLink;

typedef struct {
    SharedState *shared;
    MeeseeksIdentity me;
    int fromParent;
    int toParent;
    Message request;
} Meeseeks;

/* ---------------------------------------------------------------- Utilities */

static struct timespec currentTime(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
}

static double secondsSince(struct timespec start) {
    struct timespec now = currentTime();
    return (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
}

static void say(const MeeseeksIdentity *me, const char *format, ...) {
    char line[INPUT_SIZE * 2];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    printf("%*s[pid %d, ppid %d] %s\n", (me->level - 1) * 2, "", me->pid, me->ppid, line);
}

/* ------------------------------------------------------------- Shared state */

static SharedState *createSharedState(void) {
    SharedState *shared = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    // El segundo argumento en 1 hace que el semáforo se comparta entre procesos y no entre hilos
    sem_init(&shared->lock, 1, 1);
    return shared;
}

static void destroySharedState(SharedState *shared) {
    sem_destroy(&shared->lock);
    munmap(shared, sizeof(SharedState));
}

static void resetSharedCounters(SharedState *shared) {
    sem_wait(&shared->lock);
    memset(shared->instancesPerLevel, 0, sizeof shared->instancesPerLevel);
    shared->totalCreated = 0;
    shared->aliveCount = 0;
    shared->maxLevel = 0;
    sem_post(&shared->lock);
}

static int registerMeeseeks(SharedState *shared, int level) {
    sem_wait(&shared->lock);
    int instance = ++shared->instancesPerLevel[level];
    shared->totalCreated++;
    if (level > shared->maxLevel) {
        shared->maxLevel = level;
    }
    sem_post(&shared->lock);
    return instance;
}

static void reserveSlots(SharedState *shared, int count) {
    sem_wait(&shared->lock);
    shared->aliveCount += count;
    sem_post(&shared->lock);
}

static void releaseSlot(SharedState *shared) {
    sem_wait(&shared->lock);
    shared->aliveCount--;
    sem_post(&shared->lock);
}

/* -------------------------------------------------------------------- Pipes */

static int sendMessage(int fileDescriptor, const Message *message) {
    // Un mensaje más pequeño que PIPE_BUF se escribe de forma atómica: nunca llega partido ni mezclado
    return write(fileDescriptor, message, sizeof *message) == (ssize_t)sizeof *message;
}

static int receiveMessage(int fileDescriptor, Message *message) {
    return read(fileDescriptor, message, sizeof *message) == (ssize_t)sizeof *message;
}

static Message outcomeMessage(MessageType type, MeeseeksIdentity solver, const char *text) {
    Message message = { .type = type, .solver = solver };
    snprintf(message.text, sizeof message.text, "%s", text);
    return message;
}

/* --------------------------------------------------------------- Calculator */

/*
 * Analizador de descenso recursivo. Cada función resuelve un nivel de
 * precedencia y llama al siguiente más fuerte:
 *   or -> and -> comparación -> suma/resta -> producto -> unario -> número o paréntesis
 */
typedef struct {
    const char *cursor;
    const char *error;
} Parser;

static double parseOr(Parser *parser);

static void skipSpaces(Parser *parser) {
    while (isspace((unsigned char)*parser->cursor)) {
        parser->cursor++;
    }
}

static int accept(Parser *parser, const char *token) {
    skipSpaces(parser);
    size_t length = strlen(token);
    if (strncmp(parser->cursor, token, length) != 0) {
        return 0;
    }
    parser->cursor += length;
    return 1;
}

static double parsePrimary(Parser *parser) {
    if (accept(parser, "(")) {
        double value = parseOr(parser);
        if (!accept(parser, ")")) {
            parser->error = "missing closing parenthesis";
        }
        return value;
    }
    if (!isdigit((unsigned char)*parser->cursor) && *parser->cursor != '.') {
        parser->error = "number expected";
        return 0;
    }
    char *end;
    double value = strtod(parser->cursor, &end);
    parser->cursor = end;
    return value;
}

static double parseUnary(Parser *parser) {
    if (accept(parser, "-")) {
        return -parseUnary(parser);
    }
    if (accept(parser, "!")) {
        return !parseUnary(parser);
    }
    return parsePrimary(parser);
}

static double parseProduct(Parser *parser) {
    double value = parseUnary(parser);
    while (1) {
        if (accept(parser, "*")) {
            value *= parseUnary(parser);
        } else if (accept(parser, "/")) {
            double divisor = parseUnary(parser);
            if (divisor == 0) {
                parser->error = "division by zero";
            } else {
                value /= divisor;
            }
        } else if (accept(parser, "%")) {
            // El módulo se calcula con enteros, como en C
            long divisor = (long)parseUnary(parser);
            if (divisor == 0) {
                parser->error = "division by zero";
            } else {
                value = (long)value % divisor;
            }
        } else {
            return value;
        }
    }
}

static double parseSum(Parser *parser) {
    double value = parseProduct(parser);
    while (1) {
        if (accept(parser, "+")) {
            value += parseProduct(parser);
        } else if (accept(parser, "-")) {
            value -= parseProduct(parser);
        } else {
            return value;
        }
    }
}

static double parseComparison(Parser *parser) {
    double left = parseSum(parser);
    // Los operadores de dos caracteres se revisan primero para que "<=" no se lea como "<"
    if (accept(parser, "<=")) return left <= parseSum(parser);
    if (accept(parser, ">=")) return left >= parseSum(parser);
    if (accept(parser, "==")) return left == parseSum(parser);
    if (accept(parser, "!=")) return left != parseSum(parser);
    if (accept(parser, "<")) return left < parseSum(parser);
    if (accept(parser, ">")) return left > parseSum(parser);
    return left;
}

static double parseAnd(Parser *parser) {
    double value = parseComparison(parser);
    while (accept(parser, "&&")) {
        double right = parseComparison(parser);
        value = value && right;
    }
    return value;
}

static double parseOr(Parser *parser) {
    double value = parseAnd(parser);
    while (accept(parser, "||")) {
        double right = parseAnd(parser);
        value = value || right;
    }
    return value;
}

static int evaluateExpression(const char *expression, double *result, const char **error) {
    Parser parser = { expression, NULL };
    *result = parseOr(&parser);
    skipSpaces(&parser);
    if (parser.error == NULL && *parser.cursor != '\0') {
        parser.error = "unexpected character";
    }
    *error = parser.error;
    return parser.error == NULL;
}

/* ----------------------------------------------------------------- Meeseeks */

static void closeInheritedFileDescriptors(int keepRead, int keepWrite) {
    // El hijo hereda los pipes del padre y de sus hermanos; se cierran para que cada pipe tenga un solo dueño
    for (int fileDescriptor = STDERR_FILENO + 1; fileDescriptor < MAX_FILE_DESCRIPTOR; fileDescriptor++) {
        if (fileDescriptor != keepRead && fileDescriptor != keepWrite) {
            close(fileDescriptor);
        }
    }
}

static Message calculate(Meeseeks *self) {
    double result;
    const char *error;
    char text[INPUT_SIZE];

    say(&self->me, "Calculating %s ...", self->request.text);
    if (!evaluateExpression(self->request.text, &result, &error)) {
        snprintf(text, sizeof text, "error: %s", error);
        say(&self->me, "I couldn't calculate it (%s). Existence is pain! Goodbye!", error);
        return outcomeMessage(MESSAGE_FAILED, self->me, text);
    }
    snprintf(text, sizeof text, "result %g", result);
    say(&self->me, "%s = %g. Request completed, thanks for letting me help! Goodbye!", self->request.text, result);
    return outcomeMessage(MESSAGE_SOLVED, self->me, text);
}

static Message executeProgram(Meeseeks *self) {
    char text[INPUT_SIZE];

    say(&self->me, "Executing \"%s\" ...", self->request.text);
    struct timespec start = currentTime();
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        say(&self->me, "I couldn't start the program. Existence is pain! Goodbye!");
        return outcomeMessage(MESSAGE_FAILED, self->me, "fork failed");
    }
    if (pid == 0) {
        close(self->fromParent);
        close(self->toParent);
        execl("/bin/sh", "sh", "-c", self->request.text, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    double seconds = secondsSince(start);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    snprintf(text, sizeof text, "exit code %d after %.3f s", exitCode, seconds);

    if (exitCode != 0) {
        say(&self->me, "The program failed with %s. Existence is pain! Goodbye!", text);
        return outcomeMessage(MESSAGE_FAILED, self->me, text);
    }
    say(&self->me, "The program finished with %s. Request completed, thanks for letting me help! Goodbye!", text);
    return outcomeMessage(MESSAGE_SOLVED, self->me, text);
}

static Message attendRequest(Meeseeks *self) {
    switch (self->request.taskType) {
        case TASK_CALCULATE:
            return calculate(self);
        case TASK_EXECUTE:
            return executeProgram(self);
        default:
            return outcomeMessage(MESSAGE_SOLVED, self->me, "request received");
    }
}

static void runMeeseeks(SharedState *shared, int level, int fromParent, int toParent) {
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 16));
    if (level == 1) {
        // El Meeseeks original crea su propio grupo de procesos; todos sus descendientes lo heredan
        setpgid(0, 0);
    }

    Meeseeks self = { .shared = shared, .fromParent = fromParent, .toParent = toParent };
    self.me = (MeeseeksIdentity){ getpid(), getppid(), level, registerMeeseeks(shared, level) };
    say(&self.me, "Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)",
        self.me.pid, self.me.ppid, self.me.level, self.me.instance);

    Message outcome = outcomeMessage(MESSAGE_FAILED, self.me, "no request received");
    if (receiveMessage(fromParent, &self.request)) {
        outcome = attendRequest(&self);
    }

    sendMessage(toParent, &outcome);
    releaseSlot(shared);
    exit(EXIT_SUCCESS);
}

static ChildLink summonMeeseeks(SharedState *shared, int level, const Message *request) {
    int toChild[2];
    int fromChild[2];
    if (pipe(toChild) < 0 || pipe(fromChild) < 0) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid_t parentPid = getpid();
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        // Si el padre muere, el kernel mata al hijo; así nunca quedan Meeseeks huérfanos
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parentPid) {
            exit(EXIT_FAILURE);
        }
        closeInheritedFileDescriptors(toChild[0], fromChild[1]);
        runMeeseeks(shared, level, toChild[0], fromChild[1]);
    }

    close(toChild[0]);
    close(fromChild[1]);
    ChildLink child = { pid, toChild[1], fromChild[0] };
    sendMessage(child.toChild, request);
    return child;
}

static void closeChildLink(const ChildLink *child) {
    close(child->toChild);
    close(child->fromChild);
}

/* ---------------------------------------------------------------------- Box */

static void clearScreen(void) {
    // Solo se limpia en una terminal real, para que las salidas guardadas en archivos no tengan códigos ANSI
    if (isatty(STDOUT_FILENO)) {
        printf("\033[H\033[2J");
    }
}

static int readLine(const char *prompt, char *buffer, int size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, size, stdin) == NULL) {
        return 0;
    }
    buffer[strcspn(buffer, "\n")] = '\0';
    return 1;
}

static void waitForEnter(void) {
    char ignored[INPUT_SIZE];
    readLine("\nPress Enter to continue...", ignored, sizeof ignored);
}

static void showMenu(void) {
    printf("=== Mr. Meeseeks Box ===\n\n");
    printf("1. Text task\n");
    printf("2. Calculate\n");
    printf("3. Execute\n");
    printf("4. Help\n");
    printf("5. Exit\n\n");
}

static void showHelp(void) {
    clearScreen();
    printf("=== Help ===\n\n");

    printf("The Mr. Meeseeks Box summons a Mr. Meeseeks (a new process) for every\n");
    printf("request. Requests are attended one at a time.\n\n");

    printf("1. Text task\n");
    printf("   Ask for anything, for example \"I want to be happy\". The task is simulated.\n");
    printf("   - Difficulty: 0 to 100. 100 is very easy, 0 is impossible.\n");
    printf("     Press Enter to let luck decide (bell curve centered at 50).\n");
    printf("   - Work time: seconds each Meeseeks spends trying before it knows if it\n");
    printf("     succeeded. Press Enter for a random time between 0.5 and 5 seconds.\n");
    printf("   - From 85.01 to 100 a single Meeseeks solves the task.\n");
    printf("     From 45.01 to 85 a Meeseeks that fails summons at least 2 helpers.\n");
    printf("     From 0 to 45 a Meeseeks that fails summons at least 5 helpers.\n");
    printf("     Helpers share the work, so the task gets easier on every level.\n");
    printf("   - Difficulty 0 is impossible: it is never solved, Meeseeks keep being\n");
    printf("     summoned and each generation works slower than the previous one.\n\n");

    printf("2. Calculate\n");
    printf("   A single Meeseeks evaluates a math or logic operation.\n");
    printf("   Operators: + - * / %% ( )  < > <= >= == !=  && || !\n");
    printf("   Logic results are 1 (true) or 0 (false).\n");
    printf("   Example: (3 + 4) * 2 > 10 && 1\n\n");

    printf("3. Execute\n");
    printf("   A single Meeseeks runs a program, for example: ls -l\n");
    printf("   The task is completed only if the program exits with code 0.\n\n");

    printf("Time limit\n");
    printf("   Every request has a maximum time in minutes. Press Enter for 5 minutes.\n");
    printf("   Decimals are allowed, for example 0.5 for 30 seconds.\n");
    printf("   If a request takes longer, PLANETARY CHAOS is declared: every Meeseeks\n");
    printf("   is terminated, the final report is shown and the Box closes.\n");
    printf("   A time limit of 0 means no limit. This is dangerous: an impossible task\n");
    printf("   would run forever, and you would have to stop it with Ctrl+C.\n\n");

    printf("Process limit\n");
    printf("   To protect the system, only a limited number of Meeseeks can be alive\n");
    printf("   at the same time. When the limit is reached, a Meeseeks keeps trying by\n");
    printf("   itself instead of summoning helpers.\n\n");

    printf("5. Exit\n");
    printf("   Closes the Box and shows the final report: every task, its duration,\n");
    printf("   whether it was completed and how many Meeseeks were summoned.\n");
}

static void runTask(SharedState *shared, const Message *request) {
    resetSharedCounters(shared);
    reserveSlots(shared, 1);
    ChildLink root = summonMeeseeks(shared, 1, request);
    setpgid(root.pid, root.pid);
    waitpid(root.pid, NULL, 0);

    Message outcome;
    if (receiveMessage(root.fromChild, &outcome)) {
        printf("\nOutcome: %s (%s)\n", outcome.type == MESSAGE_SOLVED ? "completed" : "not completed", outcome.text);
    }
    printf("Meeseeks summoned for this request: %d\n", shared->totalCreated);
    closeChildLink(&root);
}

int main(void) {
    char option[INPUT_SIZE];
    setvbuf(stdout, NULL, _IOLBF, 0);
    // Escribir STOP a un Meeseeks que ya terminó no debe matar al que escribe
    signal(SIGPIPE, SIG_IGN);
    SharedState *shared = createSharedState();

    while (1) {
        clearScreen();
        showMenu();
        if (!readLine("Choose an option: ", option, sizeof option) || strcmp(option, "5") == 0) {
            break;
        }

        if (strcmp(option, "4") == 0) {
            showHelp();
        } else if (strcmp(option, "1") == 0) {
            Message request = { .type = MESSAGE_REQUEST, .taskType = TASK_TEXT };
            if (!readLine("Request: ", request.text, sizeof request.text)) {
                break;
            }
            printf("\n");
            runTask(shared, &request);
        } else if (strcmp(option, "2") == 0) {
            Message request = { .type = MESSAGE_REQUEST, .taskType = TASK_CALCULATE };
            if (!readLine("Expression: ", request.text, sizeof request.text)) {
                break;
            }
            printf("\n");
            runTask(shared, &request);
        } else if (strcmp(option, "3") == 0) {
            Message request = { .type = MESSAGE_REQUEST, .taskType = TASK_EXECUTE };
            if (!readLine("Command: ", request.text, sizeof request.text)) {
                break;
            }
            printf("\n");
            runTask(shared, &request);
        } else {
            printf("\nInvalid option. Choose a number from 1 to 5.\n");
        }
        waitForEnter();
    }

    printf("\nGoodbye from the Mr. Meeseeks Box!\n");
    destroySharedState(shared);
    return 0;
}
