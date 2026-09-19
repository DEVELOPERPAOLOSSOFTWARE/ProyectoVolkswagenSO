#include "input.h"
#include "timer.h"
#include "uart.h"

/*
 * input.c -- Sistema de eventos de entrada
 * ============================================================================
 * Cola circular de eventos + deteccion de gestos (tap, long press, swipe).
 * Completamente independiente del hardware -- recibe touch_point_t del driver
 * y produce input_event_t para la UI.
 * ============================================================================
 */

/* Cola circular de eventos */
static input_event_t event_queue[INPUT_QUEUE_SIZE];
static volatile int  queue_head = 0;   /* proximo a leer  */
static volatile int  queue_tail = 0;   /* proximo a escribir */

/* Handler registrado por la UI */
static input_handler_t ui_handler = 0;

/* Estado interno para deteccion de gestos */
static struct {
    int           active;           /* hay un toque en curso?     */
    int           start_x;          /* donde empezo el toque      */
    int           start_y;
    unsigned long start_tick;       /* cuando empezo (ticks)      */
    int           moved;            /* se movio mas de 10px?      */
    int           finger_id;
} gesture_state[10];               /* uno por dedo (multitouch)  */

/* Umbrales de deteccion de gestos */
#define GESTURE_TAP_MAX_MS       200    /* mas de esto no es tap      */
#define GESTURE_LONG_PRESS_MS    500    /* menos de esto no es long   */
#define GESTURE_MOVE_THRESHOLD   10     /* pixels para considerar move*/
#define GESTURE_SWIPE_MIN_PX     80     /* pixels minimos para swipe  */

/* -------------------------------------------------------------------------
 * input_push_event -- agregar un evento a la cola (llamado por el driver)
 * ------------------------------------------------------------------------- */
void input_push_event(const input_event_t *event)
{
    int next_tail = (queue_tail + 1) % INPUT_QUEUE_SIZE;

    /* Cola llena: descartar el evento mas viejo (head avanza) */
    if (next_tail == queue_head) {
        queue_head = (queue_head + 1) % INPUT_QUEUE_SIZE;
    }

    event_queue[queue_tail] = *event;
    queue_tail = next_tail;

    /* Notificar inmediatamente al handler de UI si esta registrado */
    if (ui_handler) {
        ui_handler(event);
    }
}

/* -------------------------------------------------------------------------
 * input_poll -- leer un evento de la cola (no bloqueante)
 * Retorna 1 si habia evento, 0 si la cola estaba vacia.
 * ------------------------------------------------------------------------- */
int input_poll(input_event_t *out_event)
{
    if (queue_head == queue_tail) return 0;
    *out_event = event_queue[queue_head];
    queue_head = (queue_head + 1) % INPUT_QUEUE_SIZE;
    return 1;
}

/* -------------------------------------------------------------------------
 * input_register_handler -- registrar callback de la UI
 * ------------------------------------------------------------------------- */
void input_register_handler(input_handler_t handler)
{
    ui_handler = handler;
}

/* -------------------------------------------------------------------------
 * input_process_touch -- convertir touch_point_t en gestos e input_event_t
 * -------------------------------------------------------------------------
 * Llamado por touch.c cada vez que llega una IRQ del chip touch.
 * Detecta: DOWN, UP, MOVE, TAP, LONG_PRESS, SWIPE_*
 * ------------------------------------------------------------------------- */
void input_process_touch(const touch_point_t *points, int count)
{
    unsigned long now = timer_get_ticks();

    for (int i = 0; i < count && i < HW_TOUCH_MAX_POINTS; i++) {
        const touch_point_t *pt = &points[i];
        int id = pt->id;
        if (id < 0 || id >= 10) continue;

        input_event_t evt;
        evt.finger_id = id;
        evt.timestamp = now;
        evt.key_code  = 0;

        if (pt->active && !gesture_state[id].active) {
            /* --- TOUCH DOWN --- */
            gesture_state[id].active     = 1;
            gesture_state[id].start_x    = pt->x;
            gesture_state[id].start_y    = pt->y;
            gesture_state[id].start_tick = now;
            gesture_state[id].moved      = 0;
            gesture_state[id].finger_id  = id;

            evt.type = INPUT_EVT_TOUCH_DOWN;
            evt.x    = pt->x;
            evt.y    = pt->y;
            evt.dx   = 0;
            evt.dy   = 0;
            input_push_event(&evt);

        } else if (pt->active && gesture_state[id].active) {
            /* --- TOUCH MOVE --- */
            int dx = pt->x - gesture_state[id].start_x;
            int dy = pt->y - gesture_state[id].start_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;

            if (adx > GESTURE_MOVE_THRESHOLD || ady > GESTURE_MOVE_THRESHOLD) {
                gesture_state[id].moved = 1;
            }

            evt.type = INPUT_EVT_TOUCH_MOVE;
            evt.x    = pt->x;
            evt.y    = pt->y;
            evt.dx   = dx;
            evt.dy   = dy;
            input_push_event(&evt);

        } else if (!pt->active && gesture_state[id].active) {
            /* --- TOUCH UP --- */
            unsigned long duration_ms =
                (now - gesture_state[id].start_tick) * 10;  /* ticks a ms (10ms/tick) */
            int dx = pt->x - gesture_state[id].start_x;
            int dy = pt->y - gesture_state[id].start_y;
            int adx = dx < 0 ? -dx : dx;
            int ady = dy < 0 ? -dy : dy;

            evt.type = INPUT_EVT_TOUCH_UP;
            evt.x    = pt->x;
            evt.y    = pt->y;
            evt.dx   = dx;
            evt.dy   = dy;
            input_push_event(&evt);

            /* --- Deteccion de gesto --- */
            if (!gesture_state[id].moved) {
                /* Sin movimiento: TAP o LONG PRESS */
                if (duration_ms < GESTURE_TAP_MAX_MS) {
                    evt.type = INPUT_EVT_TAP;
                    evt.x    = gesture_state[id].start_x;
                    evt.y    = gesture_state[id].start_y;
                    evt.dx   = 0; evt.dy = 0;
                    input_push_event(&evt);
                } else if (duration_ms >= GESTURE_LONG_PRESS_MS) {
                    evt.type = INPUT_EVT_LONG_PRESS;
                    evt.x    = gesture_state[id].start_x;
                    evt.y    = gesture_state[id].start_y;
                    evt.dx   = 0; evt.dy = 0;
                    input_push_event(&evt);
                }
            } else {
                /* Con movimiento: SWIPE si supera el minimo */
                if (adx > ady && adx >= GESTURE_SWIPE_MIN_PX) {
                    evt.type = (dx > 0) ? INPUT_EVT_SWIPE_RIGHT : INPUT_EVT_SWIPE_LEFT;
                    evt.x    = gesture_state[id].start_x;
                    evt.y    = gesture_state[id].start_y;
                    evt.dx   = dx; evt.dy = dy;
                    input_push_event(&evt);
                } else if (ady > adx && ady >= GESTURE_SWIPE_MIN_PX) {
                    evt.type = (dy > 0) ? INPUT_EVT_SWIPE_DOWN : INPUT_EVT_SWIPE_UP;
                    evt.x    = gesture_state[id].start_x;
                    evt.y    = gesture_state[id].start_y;
                    evt.dx   = dx; evt.dy = dy;
                    input_push_event(&evt);
                }
            }

            gesture_state[id].active = 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * input_init -- inicializar el sistema de eventos
 * ------------------------------------------------------------------------- */
void input_init(void)
{
    uart_puts("[input] Iniciando sistema de eventos...\n");

    queue_head = 0;
    queue_tail = 0;
    ui_handler = 0;

    for (int i = 0; i < 10; i++) {
        gesture_state[i].active = 0;
    }

    uart_puts("[input] Cola de eventos lista (");
    /* imprimir tamano */
    char buf[8]; int j = 0;
    unsigned int tmp = INPUT_QUEUE_SIZE;
    while (tmp) { buf[j++] = '0' + (tmp % 10); tmp /= 10; }
    for (int a=0,b=j-1; a<b; a++,b--) { char t=buf[a]; buf[a]=buf[b]; buf[b]=t; }
    buf[j] = '\0'; uart_puts(buf);
    uart_puts(" slots).\n");
    uart_puts("[input] Gestos activos: TAP, LONG_PRESS, SWIPE x4, MULTI_TOUCH.\n");
}