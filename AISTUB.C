/*
 * ai_stubs.c -- Stubs vacios para modulos pendientes del equipo de IA
 * ============================================================================
 * TEMPORAL -- estos stubs se REEMPLAZAN cuando lleguen:
 *   ai_tokenizer.c  (equipo de IA -- tokenizador BPE 32K)
 *   ai_inference.c  (equipo de IA -- motor de inferencia 500M)
 *   ai_tts.c        (equipo de IA -- sintetizador de voz)
 *
 * El linker necesita que existan los simbolos aunque los modulos
 * aun no esten implementados. Cada stub retorna error (-1) o 0
 * para que ai_agent.c detecte que no estan disponibles.
 * ============================================================================
 */

#include "ai_tokenizer.h"
#include "ai_inference.h"
#include "ai_tts.h"

/* ---- Tokenizador (pendiente equipo IA) ---- */
int  tokenizer_init(const char *v, const char *m) { (void)v;(void)m; return -1; }
int  tokenizer_encode(const char *t, unsigned short *o, int max)
     { (void)t;(void)o;(void)max; return 0; }
int  tokenizer_decode(const unsigned short *t, int n, char *o, int max)
     { (void)t;(void)n;(void)o;(void)max; return 0; }
void tokenizer_prepend_special(unsigned short *t, int *n, unsigned short id)
     { (void)t;(void)n;(void)id; }
void tokenizer_append_special(unsigned short *t, int *n, unsigned short id)
     { (void)t;(void)n;(void)id; }
void tokenizer_free(void) {}

/* ---- Motor de inferencia (pendiente equipo IA) ---- */
int  inference_init(const char *m, const char *c) { (void)m;(void)c; return -1; }
int  inference_generate(const unsigned short *in, int ni,
                         unsigned short *out, int max)
     { (void)in;(void)ni;(void)out;(void)max; return 0; }
void inference_free(void) {}

/* ---- TTS (pendiente equipo IA) ---- */
int  tts_init(const char *m) { (void)m; return -1; }
int  tts_synthesize(const char *t, float *a, int *s)
     { (void)t;(void)a;(void)s; return -1; }
void tts_free(void) {}