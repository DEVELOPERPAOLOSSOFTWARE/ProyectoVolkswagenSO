#include "scheduler.h"
#include "timer.h"
#include "uart.h"

/*
 * scheduler.c -- Round-robin scheduler
 * ============================================================================
 * Primera implementacion funcional: todas las tareas comparten CPU en
 * rodajas iguales de HW_TIMER_TICK_MS ms. Sin prioridades variables todavia
 * (eso viene en la siguiente iteracion junto con TASK_BLOCKED/mutexes).
 * ============================================================================
 */

/* Declaracion externa de context_switch (implementado en context_switch.S) */
extern void context_switch(cpu_context_t *old_ctx, cpu_context_t *new_ctx);

/* Pool estatico de tareas y stacks -- sin malloc por ahora */
static task_t  tasks[SCHEDULER_MAX_TASKS];
static unsigned char task_stacks[SCHEDULER_MAX_TASKS][SCHEDULER_STACK_SIZE]
    __attribute__((aligned(16)));  /* ARM64 exige stack alineado a 16 bytes */

static int     task_count   = 0;
static int     current_task = 0;   /* indice de la tarea que corre ahora */
static int     scheduler_started = 0;

/* Tarea "idle": corre cuando ninguna otra tarea esta lista.
 * El kernel siempre tiene al menos esta tarea -- nunca se queda sin
 * nada que ejecutar. */
static void idle_task(void)
{
    while (1) {
        /* wfe consume menos energia que un loop vacio puro */
        __asm__ volatile("wfe");
    }
}

/* -------------------------------------------------------------------------
 * scheduler_init -- preparar el scheduler (antes de crear tareas)
 * ------------------------------------------------------------------------- */
void scheduler_init(void)
{
    uart_puts("[scheduler] Iniciando scheduler (round-robin)...\n");

    for (int i = 0; i < SCHEDULER_MAX_TASKS; i++) {
        tasks[i].state = TASK_DEAD;
        tasks[i].id    = i;
        tasks[i].name  = 0;
    }
    task_count   = 0;
    current_task = 0;

    /* Crear la tarea idle como tarea 0 -- siempre existe */
    scheduler_create_task("idle", idle_task);

    uart_puts("[scheduler] Tarea idle registrada.\n");
}

/* -------------------------------------------------------------------------
 * scheduler_create_task -- registrar una nueva tarea
 * -------------------------------------------------------------------------
 * Prepara el contexto inicial de la tarea para que cuando el scheduler
 * haga un context_switch hacia ella por primera vez, empiece a ejecutar
 * desde el inicio de 'func' con su propio stack limpio.
 * ------------------------------------------------------------------------- */
int scheduler_create_task(const char *name, task_func_t func)
{
    if (task_count >= SCHEDULER_MAX_TASKS) {
        uart_puts("[scheduler] ERROR: limite de tareas alcanzado.\n");
        return -1;
    }

    int idx = task_count++;
    task_t *t = &tasks[idx];

    t->name       = name;
    t->state      = TASK_READY;
    t->stack_base = (unsigned long)task_stacks[idx];

    /* Limpiar el contexto inicial -- loop explicito, sin memset
     * (bare-metal: no tenemos libc) */
    volatile unsigned long *ctx_raw = (volatile unsigned long *)&t->context;
    for (unsigned int i = 0; i < sizeof(cpu_context_t) / 8; i++) {
        ctx_raw[i] = 0;
    }

    /* PC = primera instruccion de la funcion */
    t->context.pc = (unsigned long)func;

    /* SP = tope del stack (crece hacia abajo, alineado a 16 bytes) */
    t->context.sp = (unsigned long)(task_stacks[idx] + SCHEDULER_STACK_SIZE);
    t->context.sp &= ~0xFUL;  /* asegurar alineacion a 16 bytes */

    /* SPSR = EL1h, interrupciones habilitadas (DAIF = 0) */
    t->context.spsr = 0x5;    /* EL1h con interrupciones habilitadas */

    uart_puts("[scheduler] Tarea creada: ");
    uart_puts(name);
    uart_puts("\n");

    return idx;
}

/* -------------------------------------------------------------------------
 * scheduler_tick -- llamado por el timer cada HW_TIMER_TICK_MS ms
 * -------------------------------------------------------------------------
 * Selecciona la siguiente tarea READY en round-robin y hace el switch.
 * Esta funcion se ejecuta en contexto de IRQ -- debe ser rapida.
 * ------------------------------------------------------------------------- */
void scheduler_tick(void)
{
    if (!scheduler_started || task_count <= 1) return;

    int old_task = current_task;

    /* Buscar la siguiente tarea READY en la lista circular */
    int next = (current_task + 1) % task_count;
    int searched = 0;
    while (searched < task_count) {
        if (tasks[next].state == TASK_READY || tasks[next].state == TASK_RUNNING) {
            break;
        }
        next = (next + 1) % task_count;
        searched++;
    }

    /* Si no encontramos ninguna (no deberia pasar, idle siempre esta READY),
     * quedarnos en la tarea actual */
    if (searched >= task_count) return;
    if (next == old_task) return;  /* solo una tarea READY -- no cambiar */

    /* Marcar estados */
    if (tasks[old_task].state == TASK_RUNNING) {
        tasks[old_task].state = TASK_READY;
    }
    tasks[next].state  = TASK_RUNNING;
    current_task       = next;

    /* Context switch -- el corazon del scheduler */
    context_switch(&tasks[old_task].context, &tasks[next].context);
}

/* -------------------------------------------------------------------------
 * scheduler_start -- iniciar el scheduler y el timer
 * -------------------------------------------------------------------------
 * Despues de esto, el sistema es multitarea. No regresa hasta que
 * el kernel se detenga.
 * ------------------------------------------------------------------------- */
void scheduler_start(void)
{
    if (task_count == 0) {
        uart_puts("[scheduler] ERROR: no hay tareas. Llamar create_task primero.\n");
        return;
    }

    uart_puts("[scheduler] Iniciando timer del sistema...\n");
    timer_init(scheduler_tick);

    uart_puts("[scheduler] Sistema multitarea activo.\n");

    /* Activar la primera tarea (la que este en current_task = 0) */
    tasks[0].state = TASK_RUNNING;
    scheduler_started = 1;

    /* "Saltar" a la primera tarea directamente usando un contexto dummy.
     * volatile para evitar que el compilador genere memset en bare-metal */
    volatile cpu_context_t dummy_storage;
    volatile unsigned long *p = (volatile unsigned long *)&dummy_storage;
    for (unsigned int i = 0; i < sizeof(cpu_context_t) / 8; i++) p[i] = 0;
    context_switch((cpu_context_t *)&dummy_storage, &tasks[0].context);

    /* Nunca llega aqui */
}

/* -------------------------------------------------------------------------
 * scheduler_current_task -- quien esta corriendo ahora
 * ------------------------------------------------------------------------- */
task_t *scheduler_current_task(void)
{
    return &tasks[current_task];
}