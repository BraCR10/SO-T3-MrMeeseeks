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

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define INPUT_SIZE 256

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
    printf("   Every request has a maximum time. Press Enter for 300 seconds.\n");
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

int main(void) {
    char option[INPUT_SIZE];

    while (1) {
        clearScreen();
        showMenu();
        if (!readLine("Choose an option: ", option, sizeof option) || strcmp(option, "5") == 0) {
            break;
        }

        if (strcmp(option, "4") == 0) {
            showHelp();
        } else if (strcmp(option, "1") == 0 || strcmp(option, "2") == 0 || strcmp(option, "3") == 0) {
            printf("\nNot available yet.\n");
        } else {
            printf("\nInvalid option. Choose a number from 1 to 5.\n");
        }
        waitForEnter();
    }

    printf("\nGoodbye from the Mr. Meeseeks Box!\n");
    return 0;
}
