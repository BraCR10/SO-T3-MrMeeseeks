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
#include <fcntl.h>
#include <poll.h>
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

#define EASY_DIFFICULTY 85.0
#define HARD_DIFFICULTY 45.0
#define MAX_HELPERS 10
#define MAX_ALIVE_MEESEEKS 200
#define FRUSTRATION_FACTOR 1.2
#define DEFAULT_TIME_LIMIT_MINUTES 5.0
#define MAX_TASKS 100
#define MIN_RANDOM_WORK_SECONDS 0.5
#define MAX_RANDOM_WORK_SECONDS 5.0

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
    int peakAlive;
    int limitReachedCount;
    int maxLevel;
    int solved;
} SharedState;

typedef enum { TASK_TEXT, TASK_CALCULATE, TASK_EXECUTE } TaskType;

typedef enum { MESSAGE_REQUEST, MESSAGE_SOLVED, MESSAGE_FAILED, MESSAGE_STOP } MessageType;

typedef struct {
    MessageType type;
    TaskType taskType;
    double difficulty;
    double workSeconds;
    int workTimeIsRandom;
    double simulatedSeconds;
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

typedef struct {
    TaskType type;
    char request[INPUT_SIZE];
    double difficulty;
    int difficultyWasRandom;
    double workSeconds;
    int workTimeWasRandom;
    double timeLimitMinutes;
    int completed;
    int planetaryChaos;
    double totalSeconds;
    double realSeconds;
    int meeseeksCreated;
    int maxLevel;
    int meeseeksPerLevel[MAX_LEVELS];
    int peakAlive;
    int limitReachedCount;
    MeeseeksIdentity root;
    MeeseeksIdentity solver;
    char result[INPUT_SIZE];
} TaskLog;

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

static double randomUniform(double min, double max) {
    return min + (max - min) * ((double)rand() / RAND_MAX);
}

static double randomDifficulty(void) {
    // Irwin-Hall: el promedio de 3 uniformes forma una campana centrada en 50 que nunca sale de 0 a 100
    return (randomUniform(0, 100) + randomUniform(0, 100) + randomUniform(0, 100)) / 3.0;
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
    shared->peakAlive = 0;
    shared->limitReachedCount = 0;
    shared->maxLevel = 0;
    shared->solved = 0;
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

static int reserveSlots(SharedState *shared, int count, int level) {
    // Se reservan todos los lugares o ninguno, dentro del semáforo, para que dos Meeseeks no tomen el mismo lugar
    int reserved = 0;
    sem_wait(&shared->lock);
    if (level < MAX_LEVELS && shared->aliveCount + count <= MAX_ALIVE_MEESEEKS) {
        shared->aliveCount += count;
        if (shared->aliveCount > shared->peakAlive) {
            shared->peakAlive = shared->aliveCount;
        }
        reserved = 1;
    }
    sem_post(&shared->lock);
    return reserved;
}

static int claimSolution(SharedState *shared) {
    // Solo el primer Meeseeks que lo logra se queda con la solución; sin esto, ramas lejanas podían
    // resolverla también mientras el STOP todavía viajaba por los pipes
    sem_wait(&shared->lock);
    int isFirst = !shared->solved;
    shared->solved = 1;
    sem_post(&shared->lock);
    return isFirst;
}

static int isSolved(SharedState *shared) {
    sem_wait(&shared->lock);
    int solved = shared->solved;
    sem_post(&shared->lock);
    return solved;
}

static void registerLimitReached(SharedState *shared) {
    sem_wait(&shared->lock);
    shared->limitReachedCount++;
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
        snprintf(text, sizeof text, "Error, %s", error);
        say(&self->me, "I couldn't calculate it (%s). Existence is pain! Goodbye!", error);
        return outcomeMessage(MESSAGE_FAILED, self->me, text);
    }
    snprintf(text, sizeof text, "%g", result);
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
        // El programa no está en primer plano: si leyera de la terminal, Linux lo detendría para siempre
        int devNull = open("/dev/null", O_RDONLY);
        dup2(devNull, STDIN_FILENO);
        close(devNull);
        execl("/bin/sh", "sh", "-c", self->request.text, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    double seconds = secondsSince(start);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    snprintf(text, sizeof text, "Exit code %d after %.3f s", exitCode, seconds);

    if (exitCode != 0) {
        say(&self->me, "The program failed. %s. Existence is pain! Goodbye!", text);
        return outcomeMessage(MESSAGE_FAILED, self->me, text);
    }
    say(&self->me, "The program finished. %s. Request completed, thanks for letting me help! Goodbye!", text);
    return outcomeMessage(MESSAGE_SOLVED, self->me, text);
}

static double workTime(const Message *request, int level) {
    double seconds = request->workSeconds;
    if (request->workTimeIsRandom) {
        seconds = randomUniform(MIN_RANDOM_WORK_SECONDS, MAX_RANDOM_WORK_SECONDS);
    }
    if (request->difficulty == 0) {
        // Ante lo imposible cada generación está más frustrada y trabaja un 20 % más lento que la anterior
        for (int generation = 1; generation < level; generation++) {
            seconds *= FRUSTRATION_FACTOR;
        }
    }
    return seconds;
}

static int canSolve(double difficulty) {
    if (difficulty > EASY_DIFFICULTY) {
        return 1;
    }
    // La confianza del Meeseeks es la dificultad al cuadrado: con 10 la probabilidad baja de 10 % a 1 %
    double confidence = (difficulty / 100.0) * (difficulty / 100.0);
    return randomUniform(0, 1) < confidence;
}

static int workUnlessStopped(int fromParent, double seconds) {
    // Trabajar es esperar en el pipe del padre: si llega STOP o el padre muere, el Meeseeks deja de trabajar
    struct pollfd parentPipe = { .fd = fromParent, .events = POLLIN };
    return poll(&parentPipe, 1, (int)(seconds * 1000)) == 0;
}

static Message stopWorking(Meeseeks *self) {
    say(&self->me, "Another Meeseeks solved the request. My job here is done, goodbye!");
    return outcomeMessage(MESSAGE_STOP, self->me, "");
}

static int helperCount(double difficulty) {
    int minimum = difficulty <= HARD_DIFFICULTY ? 5 : 2;
    // Cuanto más lejos de 85 está la dificultad, más ayudantes extra puede llamar (hasta 4 más con 0)
    int extra = rand() % (1 + (int)((EASY_DIFFICULTY - difficulty) / 20));
    return minimum + extra;
}

static double agedDifficulty(double difficulty, int helpers) {
    if (difficulty == 0) {
        return 0;
    }
    // Aging: los ayudantes se reparten lo que falta para llegar a 100, así la tarea se simplifica en cada nivel
    return difficulty + (100 - difficulty) / helpers * randomUniform(0.5, 1.0);
}

static ChildLink summonMeeseeks(SharedState *shared, int level, const Message *request);
static void closeChildLink(const ChildLink *child);

static Message waitForHelpers(Meeseeks *self, const ChildLink children[], int count) {
    struct pollfd pipes[MAX_HELPERS + 1];
    for (int i = 0; i < count; i++) {
        pipes[i] = (struct pollfd){ .fd = children[i].fromChild, .events = POLLIN };
    }
    pipes[count] = (struct pollfd){ .fd = self->fromParent, .events = POLLIN };

    int pending = count;
    while (pending > 0) {
        poll(pipes, count + 1, -1);
        if (pipes[count].revents != 0) {
            return outcomeMessage(MESSAGE_STOP, self->me, "");
        }
        for (int i = 0; i < count; i++) {
            if (pipes[i].revents == 0) {
                continue;
            }
            Message message;
            int received = receiveMessage(pipes[i].fd, &message);
            // Un descriptor negativo hace que poll ignore a ese hijo de ahí en adelante
            pipes[i].fd = -1;
            pending--;
            if (received && message.type == MESSAGE_SOLVED) {
                return message;
            }
        }
    }
    return outcomeMessage(MESSAGE_STOP, self->me, "");
}

static void dismissHelpers(const ChildLink children[], int count) {
    Message stop = { .type = MESSAGE_STOP };
    for (int i = 0; i < count; i++) {
        sendMessage(children[i].toChild, &stop);
    }
    for (int i = 0; i < count; i++) {
        waitpid(children[i].pid, NULL, 0);
        closeChildLink(&children[i]);
    }
}

static Message askForHelp(Meeseeks *self, int helpers, double difficulty, double simulatedSeconds) {
    ChildLink children[MAX_HELPERS];
    Message request = self->request;
    request.simulatedSeconds = simulatedSeconds;

    say(&self->me, "This is harder than I thought! Summoning %d Meeseeks to help me.", helpers);
    for (int i = 0; i < helpers; i++) {
        request.difficulty = agedDifficulty(difficulty, helpers);
        children[i] = summonMeeseeks(self->shared, self->me.level + 1, &request);
    }

    Message outcome = waitForHelpers(self, children, helpers);
    // Primero se despiden los hijos y después el padre, así la despedida avanza hacia atrás hasta el original
    dismissHelpers(children, helpers);
    if (outcome.type != MESSAGE_SOLVED) {
        return stopWorking(self);
    }
    say(&self->me, "One of my helpers did it! Thanks for letting me help, goodbye!");
    return outcome;
}

static Message attemptTextTask(Meeseeks *self) {
    double difficulty = self->request.difficulty;
    double simulatedSeconds = self->request.simulatedSeconds;
    int waitingForRoom = 0;

    while (1) {
        double seconds = workTime(&self->request, self->me.level);
        if (!waitingForRoom) {
            say(&self->me, "Working on \"%s\" (difficulty %.2f, %.2f s) ...", self->request.text, difficulty, seconds);
        }
        if (!workUnlessStopped(self->fromParent, seconds)) {
            return stopWorking(self);
        }
        simulatedSeconds += seconds;
        if (isSolved(self->shared)) {
            return stopWorking(self);
        }

        if (canSolve(difficulty) && claimSolution(self->shared)) {
            say(&self->me, "I did it! Request completed, thanks for letting me help! Goodbye!");
            Message outcome = outcomeMessage(MESSAGE_SOLVED, self->me, "");
            outcome.simulatedSeconds = simulatedSeconds;
            return outcome;
        }

        int helpers = helperCount(difficulty);
        if (reserveSlots(self->shared, helpers, self->me.level + 1)) {
            return askForHelp(self, helpers, difficulty, simulatedSeconds);
        }
        // Plan de contingencia: sin lugar para más procesos, el Meeseeks sigue intentando solo en vez de hacer fork.
        // Lo avisa una sola vez para que cientos de Meeseeks reintentando no inunden la pantalla.
        if (!waitingForRoom) {
            say(&self->me, "Too many Meeseeks alive! I'll keep trying by myself, quietly.");
            waitingForRoom = 1;
            registerLimitReached(self->shared);
        }
        difficulty = agedDifficulty(difficulty, helpers);
    }
}

static void announceOutcome(const Meeseeks *self, const Message *outcome) {
    const MeeseeksIdentity *solver = &outcome->solver;
    if (outcome->type == MESSAGE_SOLVED) {
        say(&self->me, "Request \"%s\" completed! It was solved by Mr. Meeseeks (%d, %d, %d, %d).",
            self->request.text, solver->pid, solver->ppid, solver->level, solver->instance);
    } else {
        say(&self->me, "Request \"%s\" could not be completed.", self->request.text);
    }
}

static Message attendRequest(Meeseeks *self) {
    switch (self->request.taskType) {
        case TASK_TEXT:
            return attemptTextTask(self);
        case TASK_CALCULATE:
            return calculate(self);
        case TASK_EXECUTE:
            return executeProgram(self);
    }
    return outcomeMessage(MESSAGE_FAILED, self->me, "unknown task");
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
        if (level == 1 && self.request.taskType == TASK_TEXT) {
            announceOutcome(&self, &outcome);
        }
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
    printf("   To protect the system from collapsing, at most %d Meeseeks can be alive\n", MAX_ALIVE_MEESEEKS);
    printf("   at the same time. A hard task can create 5, 25, 125, 625... processes,\n");
    printf("   so without this limit thousands of processes could freeze the computer.\n");
    printf("   When a Meeseeks needs helpers but there is no room, it does not create\n");
    printf("   any: it says so once and keeps trying by itself. When other Meeseeks\n");
    printf("   finish, room is freed and helpers can be summoned again.\n");
    printf("   With difficulty 0 the limit stays full until the time limit ends it.\n");
    printf("   The final report shows the peak of Meeseeks alive and how many of them\n");
    printf("   found the limit full.\n\n");

    printf("5. Exit\n");
    printf("   Closes the Box and shows the final report: every task, its duration,\n");
    printf("   whether it was completed and how many Meeseeks were summoned.\n");
}

static const char *taskTypeName(TaskType type) {
    switch (type) {
        case TASK_TEXT:
            return "Text task";
        case TASK_CALCULATE:
            return "Calculate";
        case TASK_EXECUTE:
            return "Execute";
    }
    return "Unknown";
}

static TaskLog newTaskLog(TaskType type) {
    TaskLog log = { .type = type, .difficulty = -1, .workSeconds = 0, .timeLimitMinutes = -1 };
    return log;
}

static void showTaskScreen(const TaskLog *log) {
    clearScreen();
    printf("=== %s ===\n\n", taskTypeName(log->type));
    if (log->request[0] != '\0') {
        printf("Request:    %s\n", log->request);
    }
    if (log->difficulty >= 0) {
        printf("Difficulty: %.2f%s\n", log->difficulty, log->difficultyWasRandom ? " (chosen by luck)" : "");
    }
    if (log->workTimeWasRandom) {
        printf("Work time:  random (%.1f to %.1f s per Meeseeks)\n", MIN_RANDOM_WORK_SECONDS, MAX_RANDOM_WORK_SECONDS);
    } else if (log->workSeconds > 0) {
        printf("Work time:  %.2f s per Meeseeks\n", log->workSeconds);
    }
    if (log->timeLimitMinutes == 0) {
        printf("Time limit: none (planetary chaos disabled)\n");
    } else if (log->timeLimitMinutes > 0) {
        printf("Time limit: %.2f minutes\n", log->timeLimitMinutes);
    }
    printf("\n");
}

static int parseNumber(const char *text, double min, double max, double *value) {
    char *end;
    *value = strtod(text, &end);
    return end != text && *end == '\0' && *value >= min && *value <= max;
}

static int askRequest(TaskLog *log) {
    const char *prompt = log->type == TASK_TEXT ? "What do you need? "
                       : log->type == TASK_CALCULATE ? "Expression to calculate: "
                       : "Command to execute: ";
    do {
        if (!readLine(prompt, log->request, sizeof log->request)) {
            return 0;
        }
    } while (log->request[0] == '\0');
    return 1;
}

static int askDifficulty(TaskLog *log) {
    char input[INPUT_SIZE];
    while (readLine("Difficulty from 0 (impossible) to 100 (very easy), or press Enter for random: ", input, sizeof input)) {
        if (input[0] == '\0') {
            log->difficulty = randomDifficulty();
            log->difficultyWasRandom = 1;
            return 1;
        }
        if (parseNumber(input, 0, 100, &log->difficulty)) {
            return 1;
        }
        printf("Please enter a number between 0 and 100.\n");
    }
    return 0;
}

static int askWorkTime(TaskLog *log) {
    char input[INPUT_SIZE];
    while (readLine("Work time in seconds per Meeseeks, or press Enter for random (0.5 to 5): ", input, sizeof input)) {
        if (input[0] == '\0') {
            log->workTimeWasRandom = 1;
            return 1;
        }
        if (parseNumber(input, 0.1, 600, &log->workSeconds)) {
            return 1;
        }
        printf("Please enter a number between 0.1 and 600.\n");
    }
    return 0;
}

static int confirmNoTimeLimit(void) {
    char answer[INPUT_SIZE];
    printf("\nWARNING: with no time limit, planetary chaos is never declared.\n");
    printf("An impossible (difficulty 0) or very hard task may run forever while the\n");
    printf("Meeseeks keep using the maximum number of processes. You would have to stop\n");
    printf("the Box with Ctrl+C and the final report would be lost.\n");
    if (!readLine("Continue without a time limit? [y/N]: ", answer, sizeof answer)) {
        return 0;
    }
    return answer[0] == 'y' || answer[0] == 'Y';
}

static int askTimeLimit(TaskLog *log) {
    char input[INPUT_SIZE];
    while (readLine("Time limit in minutes (0 = no limit), or press Enter for 5: ", input, sizeof input)) {
        if (input[0] == '\0') {
            log->timeLimitMinutes = DEFAULT_TIME_LIMIT_MINUTES;
            return 1;
        }
        if (!parseNumber(input, 0, 1440, &log->timeLimitMinutes)) {
            printf("Please enter a number between 0 and 1440.\n");
        } else if (log->timeLimitMinutes > 0 || confirmNoTimeLimit()) {
            return 1;
        }
    }
    return 0;
}

static int setupTask(TaskLog *log) {
    showTaskScreen(log);
    if (!askRequest(log)) {
        return 0;
    }
    if (log->type == TASK_TEXT) {
        showTaskScreen(log);
        if (!askDifficulty(log)) {
            return 0;
        }
        showTaskScreen(log);
        if (!askWorkTime(log)) {
            return 0;
        }
    }
    if (log->type == TASK_CALCULATE) {
        log->timeLimitMinutes = DEFAULT_TIME_LIMIT_MINUTES;
    } else {
        showTaskScreen(log);
        if (!askTimeLimit(log)) {
            return 0;
        }
    }
    showTaskScreen(log);
    return 1;
}

static Message requestFromLog(const TaskLog *log) {
    Message request = {
        .type = MESSAGE_REQUEST,
        .taskType = log->type,
        .difficulty = log->difficulty,
        .workSeconds = log->workSeconds,
        .workTimeIsRandom = log->workTimeWasRandom,
    };
    snprintf(request.text, sizeof request.text, "%s", log->request);
    return request;
}

static int waitWithTimeLimit(pid_t rootPid, double timeLimitMinutes, struct timespec start) {
    // Se revisa cada 10 ms si el Meeseeks original terminó; así el error de medición es de 10 ms como máximo
    while (waitpid(rootPid, NULL, WNOHANG) == 0) {
        if (timeLimitMinutes > 0 && secondsSince(start) >= timeLimitMinutes * 60) {
            return 0;
        }
        usleep(10000);
    }
    return 1;
}

static void declarePlanetaryChaos(pid_t rootPid, double timeLimitMinutes) {
    printf("\n!!! PLANETARY CHAOS !!! The request exceeded %.2f minutes.\n", timeLimitMinutes);
    printf("Terminating every Mr. Meeseeks...\n");
    // Un PID negativo en kill envía la señal a todo el grupo de procesos del Meeseeks original
    kill(-rootPid, SIGKILL);
    waitpid(rootPid, NULL, 0);
}

static int runTask(SharedState *shared, TaskLog *log) {
    Message request = requestFromLog(log);
    resetSharedCounters(shared);
    reserveSlots(shared, 1, 1);
    struct timespec start = currentTime();
    ChildLink root = summonMeeseeks(shared, 1, &request);
    setpgid(root.pid, root.pid);

    log->root = (MeeseeksIdentity){ root.pid, getpid(), 1, 1 };

    int finished = waitWithTimeLimit(root.pid, log->timeLimitMinutes, start);
    if (!finished) {
        declarePlanetaryChaos(root.pid, log->timeLimitMinutes);
    }
    log->totalSeconds = secondsSince(start);
    log->realSeconds = log->totalSeconds;
    log->planetaryChaos = !finished;

    Message outcome;
    if (finished && receiveMessage(root.fromChild, &outcome)) {
        log->completed = outcome.type == MESSAGE_SOLVED;
        log->solver = outcome.solver;
        // El tiempo real descuenta el trabajo simulado de la cadena de Meeseeks que llevó a la solución
        log->realSeconds = log->totalSeconds - outcome.simulatedSeconds;
        snprintf(log->result, sizeof log->result, "%s", outcome.text);
    }
    log->meeseeksCreated = shared->totalCreated;
    log->maxLevel = shared->maxLevel;
    log->peakAlive = shared->peakAlive;
    log->limitReachedCount = shared->limitReachedCount;
    memcpy(log->meeseeksPerLevel, shared->instancesPerLevel, sizeof log->meeseeksPerLevel);
    closeChildLink(&root);
    return finished;
}

static const char *taskStatus(const TaskLog *log) {
    if (log->completed) {
        return "Completed";
    }
    return log->planetaryChaos ? "Not completed, planetary chaos" : "Not completed";
}

static void printTaskLog(const TaskLog *log, int number) {
    printf("Task %d: %s\n", number, taskTypeName(log->type));
    printf("  Request:          %s\n", log->request);
    if (log->type == TASK_TEXT) {
        printf("  Difficulty:       %.2f%s\n", log->difficulty, log->difficultyWasRandom ? ", chosen by luck" : "");
    }
    printf("  Status:           %s\n", taskStatus(log));
    if (log->result[0] != '\0') {
        printf("  Result:           %s\n", log->result);
    }
    printf("  Total time:       %.3f s\n", log->totalSeconds);
    if (log->type == TASK_TEXT && log->completed) {
        printf("  Real time:        %.3f s without simulated work\n", log->realSeconds);
    }
    printf("  Meeseeks created: %d\n", log->meeseeksCreated);
    if (log->type == TASK_TEXT) {
        for (int level = 1; level <= log->maxLevel; level++) {
            char label[INPUT_SIZE];
            snprintf(label, sizeof label, "Level %d:", level);
            printf("    %-16s%d\n", label, log->meeseeksPerLevel[level]);
        }
        printf("  Peak alive:       %d of %d\n", log->peakAlive, MAX_ALIVE_MEESEEKS);
        if (log->limitReachedCount > 0) {
            printf("  Process limit:    Reached by %d Meeseeks\n", log->limitReachedCount);
        } else {
            printf("  Process limit:    Not reached\n");
        }
    }
    if (log->completed) {
        const MeeseeksIdentity *solver = &log->solver;
        printf("  Solved by:        PID %d, PPID %d, level %d, instance %d\n",
               solver->pid, solver->ppid, solver->level, solver->instance);
    }
}

static void printReport(const TaskLog logs[], int count) {
    int completed = 0;
    printf("\nMr. Meeseeks Box final report\n\n");
    for (int i = 0; i < count; i++) {
        printTaskLog(&logs[i], i + 1);
        printf("\n");
        completed += logs[i].completed;
    }
    printf("Tasks requested:  %d\n", count);
    printf("Completed:        %d\n", completed);
    printf("Not completed:    %d\n", count - completed);
}

int main(void) {
    char option[INPUT_SIZE];
    TaskLog logs[MAX_TASKS];
    int logCount = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    // Escribir STOP a un Meeseeks que ya terminó no debe matar al que escribe
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL));
    SharedState *shared = createSharedState();

    while (1) {
        clearScreen();
        showMenu();
        if (!readLine("Choose an option: ", option, sizeof option) || strcmp(option, "5") == 0) {
            break;
        }

        if (strcmp(option, "4") == 0) {
            showHelp();
        } else if (strcmp(option, "1") == 0 || strcmp(option, "2") == 0 || strcmp(option, "3") == 0) {
            if (logCount == MAX_TASKS) {
                printf("\nThe Box is full (%d tasks). Choose 5 to exit and see the report.\n", MAX_TASKS);
                waitForEnter();
                continue;
            }
            TaskLog *log = &logs[logCount];
            *log = newTaskLog((TaskType)(option[0] - '1'));
            if (!setupTask(log)) {
                break;
            }
            logCount++;
            int finished = runTask(shared, log);
            printf("\n");
            printTaskLog(log, logCount);
            if (!finished) {
                break;
            }
        } else {
            printf("\nInvalid option. Choose a number from 1 to 5.\n");
        }
        waitForEnter();
    }

    printReport(logs, logCount);
    printf("\nGoodbye from the Mr. Meeseeks Box!\n");
    destroySharedState(shared);
    return 0;
}
