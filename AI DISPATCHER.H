#ifndef BYDOS_AI_DISPATCHER_H
#define BYDOS_AI_DISPATCHER_H

/*
 * ai_dispatcher.h -- Motor de ejecucion de comandos del agente
 * ============================================================================
 * Parsea la respuesta del modelo buscando tokens [CMD]...[/CMD]
 * y ejecuta las funciones correspondientes en el OS.
 *
 * Ejemplo de respuesta del modelo:
 *   "[AST] [CMD] set_climate 22 [/CMD] Listo, clima a 22 grados. [EOS]"
 *
 * El dispatcher extrae "set_climate 22", lo parsea, y llama a la
 * funcion del OS correspondiente. El texto limpio ("Listo, clima...") 
 * va al TTS para la respuesta de voz.
 * ============================================================================
 */

/* Resultado de ejecutar un comando */
typedef struct {
    char command[64];      /* nombre del comando ejecutado       */
    char args[256];        /* argumentos del comando             */
    int  success;          /* 1 = ejecutado OK, 0 = error        */
    char clean_text[1024]; /* respuesta sin los tokens de comando */
} dispatch_result_t;

/* Registrar una funcion del OS como handler de un comando */
typedef int (*cmd_handler_t)(const char *args, char *out_response, int max_len);

void dispatcher_init(void);
void dispatcher_register(const char *command, cmd_handler_t handler);
int  dispatcher_process(const char *model_response, dispatch_result_t *result);

#endif /* BYDOS_AI_DISPATCHER_H */