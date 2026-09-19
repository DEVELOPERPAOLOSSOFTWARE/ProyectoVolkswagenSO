#ifndef BYDOS_AI_TOKENIZER_H
#define BYDOS_AI_TOKENIZER_H

/*
 * ai_tokenizer.h -- Tokenizador BPE propio
 * ============================================================================
 * ESTE MODULO LO ENTREGA EL EQUIPO DE IA.
 * Este header define el CONTRATO entre el OS y el tokenizador.
 * El equipo de IA implementa ai_tokenizer.c siguiendo exactamente
 * las firmas de funcion definidas aqui -- sin cambiar nada.
 * ============================================================================
 */

#define TOKENIZER_MAX_VOCAB     32000
#define TOKENIZER_MAX_TOKEN_LEN 64
#define TOKENIZER_MAX_OUTPUT    2048

/* Cargar vocabulario y reglas desde VWFS */
int  tokenizer_init(const char *vocab_path, const char *merges_path);

/* Texto -> tokens (retorna numero de tokens generados) */
int  tokenizer_encode(const char *text, unsigned short *out_tokens,
                      int max_tokens);

/* Tokens -> texto (retorna bytes escritos) */
int  tokenizer_decode(const unsigned short *tokens, int num_tokens,
                      char *out_text, int max_len);

/* Agregar token especial al inicio/fin del array */
void tokenizer_prepend_special(unsigned short *tokens, int *num_tokens,
                                unsigned short special_id);
void tokenizer_append_special(unsigned short *tokens, int *num_tokens,
                                unsigned short special_id);

void tokenizer_free(void);

#endif /* BYDOS_AI_TOKENIZER_H */