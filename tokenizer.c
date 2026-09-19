/* ============================================================
   VW OS — Tokenizador BPE (implementación)
   Sección 3: BPE + 16 tokens especiales obligatorios
   ============================================================ */
#include "tokenizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------- Tokens especiales (Sección 3.2) ---------- */
static const char *SPECIAL_TOKENS[16] = {
    "[BOS]", "[EOS]", "[USR]", "[AST]",
    "[CMD]", "[/CMD]", "[VEH]", "[/VEH]",
    "[ALRT]", "[NAV]", "[MEDIA]", "[CLIMA]",
    "[CALL]", "[DIAG]", "[UNK]", "[PAD]"
};

const char *tokenizer_special_str(int id) {
    if (id >= 0 && id < 16) return SPECIAL_TOKENS[id];
    return NULL;
}

int tokenizer_is_special(int id) {
    return (id >= 0 && id < 16);
}

/* ---------- Utilidades de fichero ---------- */
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static char *my_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ---------- Carga de vocab.json ---------- */
static int load_vocab(Tokenizer *tok, const char *path) {
    size_t len = 0;
    char *json = read_file(path, &len);
    if (!json) return -1;

    tok->vocab = (char **)calloc(VOCAB_SIZE, sizeof(char *));
    if (!tok->vocab) { free(json); return -1; }

    /* Garantizar los 16 especiales */
    for (int i = 0; i < 16; i++) {
        tok->vocab[i] = strdup(SPECIAL_TOKENS[i]);
    }

    /* Parseo simple de {"token": id, ...} */
    char *p = json;
    int loaded = 16;
    while (*p && loaded < VOCAB_SIZE) {
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++; /* salta " */
        char *start = p;
        while (*p && *p != '"') p++;
        if (!*p) break;
        size_t tlen = (size_t)(p - start);
        p++; /* salta " */
        while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        int id = atoi(p);
        if (id >= 16 && id < VOCAB_SIZE && tok->vocab[id] == NULL) {
            tok->vocab[id] = my_strndup(start, tlen);
            loaded++;
        }
        while (*p && *p != ',' && *p != '}') p++;
        if (*p == ',') p++;
    }

    /* Rellenar huecos con [UNK] */
    for (int i = 0; i < VOCAB_SIZE; i++) {
        if (tok->vocab[i] == NULL)
            tok->vocab[i] = strdup("[UNK]");
    }

    tok->vocab_size = VOCAB_SIZE;
    free(json);
    return 0;
}

/* ---------- Carga de merges.txt ---------- */
static int load_merges(Tokenizer *tok, const char *path) {
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) {
        tok->merges = NULL;
        tok->num_merges = 0;
        return 0; /* modo fallback sin merges */
    }

    int nlines = 0;
    for (char *q = data; *q; q++) if (*q == '\n') nlines++;

    tok->merges = (int *)malloc((size_t)nlines * 3 * sizeof(int));
    if (!tok->merges) { free(data); return -1; }
    tok->num_merges = 0;

    char *line = data;
    while (*line) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';

        if (line[0] && line[0] != '#') {
            char a[MAX_TOKEN_LEN], b[MAX_TOKEN_LEN];
            if (sscanf(line, "%63s %63s", a, b) == 2) {
                int id_a = -1, id_b = -1;
                for (int i = 0; i < tok->vocab_size; i++) {
                    if (strcmp(tok->vocab[i], a) == 0) id_a = i;
                    if (strcmp(tok->vocab[i], b) == 0) id_b = i;
                }
                if (id_a >= 0 && id_b >= 0) {
                    char fused[MAX_TOKEN_LEN * 2];
                    snprintf(fused, sizeof(fused), "%s%s", a, b);
                    int id_dst = -1;
                    for (int i = 0; i < tok->vocab_size; i++) {
                        if (strcmp(tok->vocab[i], fused) == 0) {
                            id_dst = i;
                            break;
                        }
                    }
                    if (id_dst >= 0) {
                        int *m = &tok->merges[tok->num_merges * 3];
                        m[0] = id_a;
                        m[1] = id_b;
                        m[2] = id_dst;
                        tok->num_merges++;
                    }
                }
            }
        }
        if (!end) break;
        line = end + 1;
    }
    free(data);
    return 0;
}

int tokenizer_init(Tokenizer *tok, const char *vocab_path, const char *merges_path) {
    if (!tok) return -1;
    memset(tok, 0, sizeof(*tok));
    if (load_vocab(tok, vocab_path) != 0) return -1;
    if (load_merges(tok, merges_path) != 0) {
        tokenizer_free(tok);
        return -1;
    }
    return 0;
}

void tokenizer_free(Tokenizer *tok) {
    if (!tok) return;
    if (tok->vocab) {
        for (int i = 0; i < tok->vocab_size; i++)
            free(tok->vocab[i]);
        free(tok->vocab);
    }
    free(tok->merges);
    memset(tok, 0, sizeof(*tok));
}

/* ---------- Encode ---------- */
static int find_special(const char *text, int *out_id, int *out_len) {
    for (int i = 0; i < 16; i++) {
        size_t len = strlen(SPECIAL_TOKENS[i]);
        if (strncmp(text, SPECIAL_TOKENS[i], len) == 0) {
            *out_id = i;
            *out_len = (int)len;
            return 1;
        }
    }
    return 0;
}

static int bpe_encode_word(const Tokenizer *tok, const char *word,
                           int *out_ids, int max_ids) {
    int n = 0;
    const char *p = word;
    while (*p && n < max_ids) {
        int best_id = TOK_UNK;
        int best_len = 1;

        /* Buscar el token más largo posible (greedy longest match) */
        for (int i = 0; i < tok->vocab_size; i++) {
            size_t len = strlen(tok->vocab[i]);
            if (len > (size_t)best_len &&
                strncmp(p, tok->vocab[i], len) == 0) {
                best_id = i;
                best_len = (int)len;
            }
        }

        /* Fallback a carácter individual */
        if (best_id == TOK_UNK) {
            char single[8];
            int clen = 1;
            /* UTF-8 simple (1-4 bytes) */
            unsigned char c = (unsigned char)*p;
            if (c >= 0xC0) {
                if ((c & 0xE0) == 0xC0) clen = 2;
                else if ((c & 0xF0) == 0xE0) clen = 3;
                else if ((c & 0xF8) == 0xF0) clen = 4;
            }
            if (clen > 1) {
                memcpy(single, p, (size_t)clen);
                single[clen] = '\0';
                for (int i = 0; i < tok->vocab_size; i++) {
                    if (strcmp(tok->vocab[i], single) == 0) {
                        best_id = i;
                        best_len = clen;
                        break;
                    }
                }
            } else {
                single[0] = *p;
                single[1] = '\0';
                for (int i = 0; i < tok->vocab_size; i++) {
                    if (strcmp(tok->vocab[i], single) == 0) {
                        best_id = i;
                        break;
                    }
                }
            }
        }

        out_ids[n++] = best_id;
        p += best_len;
    }
    return n;
}

int tokenizer_encode(const Tokenizer *tok, const char *text,
                     int *out_ids, int max_ids) {
    if (!tok || !text || !out_ids || max_ids <= 0) return -1;

    int n = 0;
    const char *p = text;

    while (*p && n < max_ids) {
        /* 1. Tokens especiales tienen prioridad absoluta */
        int sid = 0, slen = 0;
        if (find_special(p, &sid, &slen)) {
            out_ids[n++] = sid;
            p += slen;
            continue;
        }

        /* 2. Saltar espacios */
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }

        /* 3. Extraer siguiente “palabra” hasta espacio o especial */
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) {
            int dummy_id, dummy_len;
            if (find_special(p, &dummy_id, &dummy_len)) break;
            p++;
        }
        size_t wlen = (size_t)(p - start);
        if (wlen == 0) { p++; continue; }

        char word[MAX_TOKEN_LEN];
        if (wlen >= MAX_TOKEN_LEN) wlen = MAX_TOKEN_LEN - 1;
        memcpy(word, start, wlen);
        word[wlen] = '\0';

        /* 4. BPE / longest-match sobre la palabra */
        int ids[128];
        int nids = bpe_encode_word(tok, word, ids, 128);
        for (int i = 0; i < nids && n < max_ids; i++) {
            out_ids[n++] = ids[i];
        }
    }
    return n;
}

/* ---------- Decode ---------- */
int tokenizer_decode(const Tokenizer *tok, const int *ids, int n_ids,
                     char *out_text, int max_len) {
    if (!tok || !ids || !out_text || max_len <= 0) return -1;
    out_text[0] = '\0';
    int pos = 0;

    for (int i = 0; i < n_ids; i++) {
        int id = ids[i];
        if (id < 0 || id >= tok->vocab_size) id = TOK_UNK;
        const char *s = tok->vocab[id];
        int len = (int)strlen(s);
        if (pos + len + 2 >= max_len) break;

        /* Espacio entre tokens normales (no entre especiales) */
        if (pos > 0 &&
            !tokenizer_is_special(id) &&
            !tokenizer_is_special(ids[i - 1])) {
            out_text[pos++] = ' ';
        }
        memcpy(out_text + pos, s, (size_t)len);
        pos += len;
    }
    out_text[pos] = '\0';
    return pos;
}
