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
