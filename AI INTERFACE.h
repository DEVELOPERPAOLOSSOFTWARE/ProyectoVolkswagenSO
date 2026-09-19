#ifndef BYDOS_AI_INFERENCE_H
#define BYDOS_AI_INFERENCE_H

/*
 * ai_inference.h -- Motor de inferencia del modelo 500M
 * ============================================================================
 * Motor transformer en C puro, basado en llama.cpp adaptado para
 * nuestro OS bare-metal. Carga el modelo desde VWFS al arrancar.
 * ============================================================================
 */

/* Configuracion del modelo (se lee de /ai/config.json al init) */
typedef struct {
    int    n_layers;       /* 24                          */
    int    d_model;        /* 1024                        */
    int    n_heads;        /* 16                          */
    int    d_ffn;          /* 4096                        */
    int    vocab_size;     /* 32000                       */
    int    max_seq_len;    /* 2048                        */
    float  rope_theta;     /* 10000.0 (RoPE base)         */
    int    quantized;      /* 1 = INT8, 0 = FP16          */
} ai_model_config_t;

/* Cargar modelo desde VWFS */
int  inference_init(const char *model_path, const char *config_path);

/* Generar tokens de respuesta
 *   input_tokens   : tokens de entrada ([USR] texto [EOS] [VEH] ctx [/VEH])
 *   n_input        : numero de tokens de entrada
 *   out_tokens     : buffer para los tokens generados
 *   max_new_tokens : maximo de tokens a generar
 *   retorna        : numero de tokens generados
 */
int  inference_generate(const unsigned short *input_tokens, int n_input,
                         unsigned short *out_tokens, int max_new_tokens);

void inference_free(void);

#endif /* BYDOS_AI_INFERENCE_H */