/* ============================================================
   VW OS — Tokenizador BPE (Sección 3 del documento)
   ============================================================ */
#ifndef VW_TOKENIZER_H
#define VW_TOKENIZER_H

#include <stddef.h>
#include <stdint.h>

#define VOCAB_SIZE          32016   /* 32000 + 16 especiales */
#define MAX_TOKEN_LEN       64
#define MAX_SEQUENCE_LEN    2048

/* IDs de tokens especiales (obligatorios - Sección 3.2) */
enum {
    TOK_BOS   = 0,   /* [BOS]  Inicio de conversación */
    TOK_EOS   = 1,   /* [EOS]  Fin de conversación / respuesta */
    TOK_USR   = 2,   /* [USR]  Turno del conductor (entrada) */
    TOK_AST   = 3,   /* [AST]  Turno del asistente (salida) */
    TOK_CMD   = 4,   /* [CMD]  Inicio de comando ejecutable en el OS */
    TOK_CMDE  = 5,   /* [/CMD] Fin del comando ejecutable */
    TOK_VEH   = 6,   /* [VEH]  Inicio de dato del vehículo */
    TOK_VEHE  = 7,   /* [/VEH] Fin del dato del vehículo */
    TOK_ALRT  = 8,   /* [ALRT] Alerta del sistema */
    TOK_NAV   = 9,   /* [NAV]  Comando de navegación */
    TOK_MEDIA = 10,  /* [MEDIA] Comando de música / audio */
    TOK_CLIMA = 11,  /* [CLIMA] Comando de climatizador */
    TOK_CALL  = 12,  /* [CALL] Comando de llamada telefónica */
    TOK_DIAG  = 13,  /* [DIAG] Diagnóstico del vehículo */
    TOK_UNK   = 14,  /* [UNK]  Token desconocido */
    TOK_PAD   = 15   /* [PAD]  Padding para batches */
};

typedef struct {
    char **vocab;           /* vocab[i] = string del token i */
    int *merges;            /* pares de fusión BPE (src1, src2, dst) */
    int num_merges;
    int vocab_size;
} Tokenizer;

/* API pública */
int  tokenizer_init(Tokenizer *tok, const char *vocab_path, const char *merges_path);
void tokenizer_free(Tokenizer *tok);

/* Tokeniza texto → IDs (respeta tokens especiales primero) */
int  tokenizer_encode(const Tokenizer *tok,
                      const char *text,
                      int *out_ids,
                      int max_ids);

/* Detokeniza IDs → texto */
int  tokenizer_decode(const Tokenizer *tok,
                      const int *ids,
                      int n_ids,
                      char *out_text,
                      int max_len);

/* Helpers para tokens especiales */
const char *tokenizer_special_str(int id);
int         tokenizer_is_special(int id);

#endif /* VW_TOKENIZER_H */
