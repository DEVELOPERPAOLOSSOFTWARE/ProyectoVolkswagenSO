#ifndef BYDOS_AI_AGENT_H
#define BYDOS_AI_AGENT_H

/*
 * ai_agent.h -- Agente de Voz Embebido del VW OS
 * ============================================================================
 *
 * El agente tiene 4 capas, cada una con su propio modulo:
 *
 *   CAPA 1 — STT (Speech To Text)
 *     Whisper small cuantizado. Convierte audio del microfono en texto.
 *     Modulo: ai_stt.c / ai_stt.h
 *     Entrada: buffer de audio float32 a 16kHz
 *     Salida:  texto transcrito (UTF-8, max 512 chars)
 *
 *   CAPA 2 — TOKENIZADOR
 *     BPE propio de 32K tokens + 16 tokens especiales del OS.
 *     Modulo: ai_tokenizer.c / ai_tokenizer.h  <-- LLEGA DEL EQUIPO
 *     Entrada: texto transcrito
 *     Salida:  array de token IDs (unsigned short)
 *
 *   CAPA 3 — INFERENCIA (modelo 500M parametros)
 *     Motor de inferencia transformer en C puro (llama.cpp adaptado).
 *     Modulo: ai_inference.c / ai_inference.h
 *     Entrada: token IDs + contexto del vehiculo
 *     Salida:  token IDs de la respuesta
 *
 *   CAPA 4 — TTS (Text To Speech)
 *     Sintetizador de voz embebido propio.
 *     Modulo: ai_tts.c / ai_tts.h
 *     Entrada: texto de la respuesta
 *     Salida:  audio para las bocinas
 *
 *   CAPA 5 — DISPATCHER (motor de comandos)
 *     Interpreta tokens [CMD]...[/CMD] y los ejecuta en el OS.
 *     Modulo: ai_dispatcher.c / ai_dispatcher.h
 *     Entrada: respuesta del modelo con tokens de comando
 *     Salida:  llamadas a funciones del OS (vehicle bridge, UI, etc.)
 *
 * ============================================================================
 * ESTADOS DEL AGENTE
 * ============================================================================
 *
 *   IDLE       -- esperando la palabra de activacion
 *   LISTENING  -- grabando la voz del conductor (max 5 segundos)
 *   PROCESSING -- Whisper + tokenizador + modelo corriendo
 *   RESPONDING -- TTS reproduciendo la respuesta
 *   ERROR      -- algo fallo, volver a IDLE
 *
 * ============================================================================
 */

#include "hw_config.h"
#include "vehicle_bridge.h"

/* Estados del agente */
typedef enum {
    AI_STATE_IDLE       = 0,
    AI_STATE_LISTENING  = 1,
    AI_STATE_PROCESSING = 2,
    AI_STATE_RESPONDING = 3,
    AI_STATE_ERROR      = 4
} ai_state_t;

/* Resultado de una interaccion completa */
typedef struct {
    char     input_text[512];     /* lo que dijo el conductor (STT)      */
    char     response_text[1024]; /* lo que responde el agente           */
    char     command[128];        /* comando ejecutado (si hubo alguno)  */
    int      command_executed;    /* 1 = se ejecuto un comando del OS    */
    ai_state_t final_state;       /* estado al terminar                  */
    unsigned long processing_ms;  /* tiempo total de procesamiento       */
} ai_result_t;

/* Tipo de callback cuando el agente tiene una respuesta lista */
typedef void (*ai_response_callback_t)(const ai_result_t *result);

/* Tipo de callback para notificar cambio de estado (para la UI) */
typedef void (*ai_state_callback_t)(ai_state_t new_state);

/* ------------------------------------------------------------------
 * API publica del agente
 * ------------------------------------------------------------------ */

/* Inicializacion -- carga modelos desde VWFS */
int  ai_init(void);

/* Activar escucha -- el agente empieza a grabar audio */
void ai_start_listening(void);

/* Parar escucha -- procesar lo grabado */
void ai_stop_listening(void);

/* Inyectar texto directo (sin STT) -- para pruebas sin microfono */
void ai_inject_text(const char *text);

/* Registrar callbacks */
void ai_register_response_callback(ai_response_callback_t cb);
void ai_register_state_callback(ai_state_callback_t cb);

/* Estado actual del agente */
ai_state_t ai_get_state(void);

/* Tick del agente -- llamado por el scheduler cada ciclo */
void ai_tick(void);

/* Liberar recursos */
void ai_free(void);

/* ------------------------------------------------------------------
 * Tokens especiales -- deben coincidir con el tokenizador del equipo
 * ------------------------------------------------------------------ */
#define AI_TOKEN_BOS    0
#define AI_TOKEN_EOS    1
#define AI_TOKEN_USR    2
#define AI_TOKEN_AST    3
#define AI_TOKEN_CMD    4
#define AI_TOKEN_CMDEND 5
#define AI_TOKEN_VEH    6
#define AI_TOKEN_VEHEND 7
#define AI_TOKEN_ALRT   8
#define AI_TOKEN_NAV    9
#define AI_TOKEN_MEDIA  10
#define AI_TOKEN_CLIMA  11
#define AI_TOKEN_CALL   12
#define AI_TOKEN_DIAG   13
#define AI_TOKEN_UNK    14
#define AI_TOKEN_PAD    15

#define AI_VOCAB_SIZE   32000
#define AI_MAX_TOKENS   2048
#define AI_MAX_NEW_TOKENS 256

#endif /* BYDOS_AI_AGENT_H */