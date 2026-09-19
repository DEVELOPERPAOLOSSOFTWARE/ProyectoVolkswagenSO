/*
 * ai_tokenizer.c -- Tokenizador BPE para VW OS (portado a bare-metal)
 * ============================================================================
 * Basado en el tokenizador entregado por el equipo de IA.
 * PORTADO COMPLETAMENTE A BARE-METAL: cero stdio, cero stdlib, cero libc.
 *
 * Cambios respecto al original del equipo:
 *   1. Firma ajustada a ai_tokenizer.h (unsigned short en vez de int)
 *   2. fopen/fread/fclose → vwfs_open/vwfs_read/vwfs_close
 *   3. malloc/free/calloc → pool estatico en BSS (sin heap dinamico)
 *   4. memcpy/memset/strlen/strcmp/strncmp/strchr → implementaciones propias
 *   5. isspace/atoi/sscanf/snprintf → implementaciones propias
 *   6. Vocab duplicados resueltos: primer ID gana, duplicados ignorados
 *   7. strdup sobre pool estatico en vez de malloc
 * ============================================================================
 */

#include "ai_tokenizer.h"
#include "vwfs.h"
#include "uart.h"
#include "hw_config.h"

/* =========================================================================
 * CAPA DE COMPATIBILIDAD BARE-METAL
 * =========================================================================
 * Todas las funciones de libc que el tokenizador necesita,
 * implementadas en C puro sin dependencias externas.
 * ========================================================================= */

/* --- String utils --- */
static int bm_strlen(const char *s) {
    int n=0; while(s[n]) n++; return n;
}
static void bm_memcpy(void *d, const void *s, int n) {
    unsigned char *dd=(unsigned char*)d;
    const unsigned char *ss=(const unsigned char*)s;
    for(int i=0;i<n;i++) dd[i]=ss[i];
}
static void bm_memset(void *d, int c, int n) {
    unsigned char *dd=(unsigned char*)d;
    for(int i=0;i<n;i++) dd[i]=(unsigned char)c;
}
static int bm_strcmp(const char *a, const char *b) {
    while(*a&&*b&&*a==*b){a++;b++;}
    return (unsigned char)*a-(unsigned char)*b;
}
static int bm_strncmp(const char *a, const char *b, int n) {
    for(int i=0;i<n;i++){
        if(!a[i]&&!b[i]) return 0;
        if(!a[i]) return -1;
        if(!b[i]) return  1;
        if(a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i];
    }
    return 0;
}
static char *bm_strchr(const char *s, int c) {
    while(*s){if(*s==(char)c)return(char*)s;s++;} return 0;
}
static int bm_isspace(unsigned char c) {
    return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v';
}
static int bm_atoi(const char *s) {
    while(bm_isspace((unsigned char)*s)) s++;
    int neg=0, v=0;
    if(*s=='-'){neg=1;s++;}else if(*s=='+') s++;
    while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;}
    return neg?-v:v;
}
/* snprintf minimo para fused="A"+"B" */
static void bm_strcat2(char *dst, int max, const char *a, const char *b) {
    int i=0,j=0;
    while(a[j]&&i<max-1){dst[i++]=a[j++];}
    j=0;
    while(b[j]&&i<max-1){dst[i++]=b[j++];}
    dst[i]='\0';
}

/* =========================================================================
 * ALLOCATOR ESTATICO
 * =========================================================================
 * Sin malloc/free. Un pool de strings en BSS.
 * Para VOCAB_SIZE=32016 strings de max 64 bytes = ~2MB -- entra en el OS.
 * ========================================================================= */
#define TOK_POOL_STRINGS    34000   /* un poco mas que TOKENIZER_MAX_VOCAB */
#define TOK_POOL_STR_LEN    72      /* MAX_TOKEN_LEN + margen              */

static char   str_pool[TOK_POOL_STRINGS][TOK_POOL_STR_LEN];
static int    str_pool_used = 0;

static char *pool_strdup(const char *s, int max_len) {
    if(str_pool_used >= TOK_POOL_STRINGS) return (char*)s; /* fallback */
    int n=0;
    while(s[n]&&n<max_len-1){str_pool[str_pool_used][n]=s[n];n++;}
    str_pool[str_pool_used][n]='\0';
    return str_pool[str_pool_used++];
}

/* Array de punteros al vocabulario (en pool) */
static char   *vocab_ptrs[TOKENIZER_MAX_VOCAB];
/* Array de merges: triples (id_a, id_b, id_dst) */
#define MAX_MERGES  4096
static int merges_buf[MAX_MERGES * 3];
static int num_merges_g = 0;

/* Tokenizador global -- un solo instancia en el OS */
static int tok_vocab_size = 0;
static int tok_initialized = 0;

/* =========================================================================
 * TOKENS ESPECIALES
 * ========================================================================= */
static const char *SPECIAL_TOKENS[16] = {
    "[BOS]","[EOS]","[USR]","[AST]",
    "[CMD]","[/CMD]","[VEH]","[/VEH]",
    "[ALRT]","[NAV]","[MEDIA]","[CLIMA]",
    "[CALL]","[DIAG]","[UNK]","[PAD]"
};

const char *tokenizer_special_str(int id) {
    if(id>=0&&id<16) return SPECIAL_TOKENS[id];
    return 0;
}
int tokenizer_is_special(int id) { return id>=0&&id<16; }

/* =========================================================================
 * LECTURA DESDE VWFS
 * =========================================================================
 * Reemplaza fopen/fread/fclose del original.
 * Lee el archivo completo en un buffer estatico.
 * ========================================================================= */
#define FILE_BUF_SIZE   (32 * 1024)   /* 32KB -- suficiente para vocab+merges */
static unsigned char file_buf[FILE_BUF_SIZE];

static int read_vwfs_file(const char *path, char **out_buf, int *out_len)
{
    vwfs_file_t f;
    if(vwfs_open(path, &f, 0) != 0) {
        uart_puts("[tokenizer] ERROR: no se pudo abrir ");
        uart_puts(path); uart_puts("\n");
        return -1;
    }
    int n = vwfs_read(&f, file_buf, FILE_BUF_SIZE - 1);
    vwfs_close(&f);
    if(n < 0) n = 0;
    file_buf[n] = '\0';
    *out_buf = (char *)file_buf;
    *out_len = n;
    return 0;
}

/* =========================================================================
 * CARGA DE VOCAB.JSON
 * =========================================================================
 * Parseo simple de {"token": id, ...}
 * Primer ID gana si hay duplicados (resuelve el bug del vocab.json).
 * ========================================================================= */
static int load_vocab(const char *path)
{
    char *json; int len;
    if(read_vwfs_file(path, &json, &len) != 0) return -1;

    /* Limpiar tabla */
    bm_memset(vocab_ptrs, 0, sizeof(vocab_ptrs));
    str_pool_used = 0;

    /* Los 16 especiales siempre en sus IDs fijos */
    for(int i=0;i<16;i++) {
        vocab_ptrs[i] = pool_strdup(SPECIAL_TOKENS[i], TOK_POOL_STR_LEN);
    }

    /* Parsear {"token": id, ...} */
    char *p = json;
    int loaded = 16;
    while(*p && loaded < TOKENIZER_MAX_VOCAB) {
        /* Buscar " */
        while(*p && *p!='"') p++;
        if(!*p) break;
        p++;
        char *tok_start = p;
        /* Leer hasta " de cierre */
        while(*p && *p!='"') p++;
        if(!*p) break;
        int tok_len = (int)(p - tok_start);
        p++;
        /* Saltar hasta el numero */
        while(*p&&(*p==':'||*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++;
        int id = bm_atoi(p);
        /* Validar y cargar -- primer ID gana (ignora duplicados) */
        if(id>=16 && id<TOKENIZER_MAX_VOCAB && vocab_ptrs[id]==0) {
            char tmp[TOK_POOL_STR_LEN];
            int cplen = tok_len < TOK_POOL_STR_LEN-1 ? tok_len : TOK_POOL_STR_LEN-1;
            bm_memcpy(tmp, tok_start, cplen);
            tmp[cplen] = '\0';
            vocab_ptrs[id] = pool_strdup(tmp, TOK_POOL_STR_LEN);
            loaded++;
        }
        /* Avanzar al siguiente token */
        while(*p && *p!=',' && *p!='}') p++;
        if(*p==',') p++;
    }

    /* Rellenar huecos con [UNK] */
    for(int i=0;i<TOKENIZER_MAX_VOCAB;i++) {
        if(!vocab_ptrs[i])
            vocab_ptrs[i] = pool_strdup("[UNK]", TOK_POOL_STR_LEN);
    }

    tok_vocab_size = TOKENIZER_MAX_VOCAB;
    return 0;
}

/* =========================================================================
 * CARGA DE MERGES.TXT
 * ========================================================================= */
static int load_merges(const char *path)
{
    char *data; int len;
    num_merges_g = 0;
    if(read_vwfs_file(path, &data, &len) != 0) {
        uart_puts("[tokenizer] merges.txt no encontrado -- modo sin BPE merges.\n");
        return 0;  /* fallback: funciona sin merges */
    }

    char *line = data;
    while(*line && num_merges_g < MAX_MERGES) {
        char *end = bm_strchr(line, '\n');
        if(end) *end='\0';

        if(line[0] && line[0]!='#') {
            /* Parsear "tokA tokB" sin sscanf */
            char a[TOK_POOL_STR_LEN], b[TOK_POOL_STR_LEN];
            int ai=0, bi=0;
            const char *q = line;
            while(*q && !bm_isspace((unsigned char)*q) && ai<TOK_POOL_STR_LEN-1)
                a[ai++]=*q++;
            a[ai]='\0';
            while(*q && bm_isspace((unsigned char)*q)) q++;
            while(*q && !bm_isspace((unsigned char)*q) && bi<TOK_POOL_STR_LEN-1)
                b[bi++]=*q++;
            b[bi]='\0';

            if(ai>0 && bi>0) {
                int id_a=-1, id_b=-1;
                for(int i=0;i<tok_vocab_size;i++) {
                    if(bm_strcmp(vocab_ptrs[i],a)==0) id_a=i;
                    if(bm_strcmp(vocab_ptrs[i],b)==0) id_b=i;
                    if(id_a>=0&&id_b>=0) break;
                }
                if(id_a>=0 && id_b>=0) {
                    char fused[TOK_POOL_STR_LEN*2];
                    bm_strcat2(fused, sizeof(fused), a, b);
                    int id_dst=-1;
                    for(int i=0;i<tok_vocab_size;i++) {
                        if(bm_strcmp(vocab_ptrs[i],fused)==0){id_dst=i;break;}
                    }
                    if(id_dst>=0) {
                        int *m=&merges_buf[num_merges_g*3];
                        m[0]=id_a; m[1]=id_b; m[2]=id_dst;
                        num_merges_g++;
                    }
                }
            }
        }
        if(!end) break;
        line = end+1;
    }
    return 0;
}

/* =========================================================================
 * API PUBLICA -- ai_tokenizer.h
 * =========================================================================
 * NOTA: la firma usa unsigned short* (nuestro contrato interno del OS).
 * La conversion int→unsigned short es segura porque los IDs son 0-32015,
 * que caben en unsigned short (max 65535).
 * ========================================================================= */

int tokenizer_init(const char *vocab_path, const char *merges_path)
{
    uart_puts("[tokenizer] Cargando vocabulario desde VWFS...\n");
    str_pool_used = 0;
    tok_initialized = 0;

    if(load_vocab(vocab_path) != 0) {
        uart_puts("[tokenizer] ERROR: fallo al cargar vocab.\n");
        return -1;
    }

    char buf[8]; int ti=0; int vs=tok_vocab_size;
    while(vs){buf[ti++]='0'+(vs%10);vs/=10;}
    for(int a=0,b=ti-1;a<b;a++,b--){char t=buf[a];buf[a]=buf[b];buf[b]=t;}
    buf[ti]='\0';
    uart_puts("[tokenizer] Vocabulario cargado: ");
    uart_puts(buf);
    uart_puts(" tokens.\n");

    if(load_merges(merges_path) != 0) {
        uart_puts("[tokenizer] ADVERTENCIA: merges parciales.\n");
    }

    tok_initialized = 1;
    uart_puts("[tokenizer] Tokenizador listo.\n");
    return 0;
}

/* --- find_special: detectar token especial en la posicion actual --- */
static int find_special(const char *text, int *out_id, int *out_len) {
    for(int i=0;i<16;i++) {
        int len = bm_strlen(SPECIAL_TOKENS[i]);
        if(bm_strncmp(text, SPECIAL_TOKENS[i], len)==0) {
            *out_id=i; *out_len=len; return 1;
        }
    }
    return 0;
}

/* --- bpe_encode_word: longest-match + BPE sobre una palabra --- */
static int bpe_encode_word(const char *word, unsigned short *out, int max)
{
    int n=0;
    const char *p=word;
    while(*p && n<max) {
        int best_id=TOKENIZER_MAX_VOCAB-1; /* [UNK] por defecto */
        int best_len=1;
        for(int i=0;i<tok_vocab_size;i++) {
            int len=bm_strlen(vocab_ptrs[i]);
            if(len>(int)best_len && bm_strncmp(p,vocab_ptrs[i],len)==0) {
                best_id=i; best_len=len;
            }
        }
        /* Fallback: caracter individual (soporta UTF-8 basico) */
        if(best_id==TOKENIZER_MAX_VOCAB-1) {
            unsigned char c=(unsigned char)*p;
            int clen=1;
            if(c>=0xC0){
                if((c&0xE0)==0xC0) clen=2;
                else if((c&0xF0)==0xE0) clen=3;
                else if((c&0xF8)==0xF0) clen=4;
            }
            char single[8]; bm_memcpy(single,p,clen); single[clen]='\0';
            for(int i=0;i<tok_vocab_size;i++) {
                if(bm_strcmp(vocab_ptrs[i],single)==0){
                    best_id=i; best_len=clen; break;
                }
            }
        }
        out[n++]=(unsigned short)best_id;
        p+=best_len;
    }
    return n;
}

int tokenizer_encode(const char *text, unsigned short *out_tokens, int max_tokens)
{
    if(!tok_initialized||!text||!out_tokens||max_tokens<=0) return -1;
    int n=0;
    const char *p=text;
    while(*p && n<max_tokens) {
        /* 1. Tokens especiales -- prioridad absoluta */
        int sid=0, slen=0;
        if(find_special(p,&sid,&slen)) {
            out_tokens[n++]=(unsigned short)sid;
            p+=slen; continue;
        }
        /* 2. Saltar espacios */
        if(bm_isspace((unsigned char)*p)) { p++; continue; }
        /* 3. Extraer palabra */
        const char *start=p;
        while(*p && !bm_isspace((unsigned char)*p)) {
            int did=0,dlen=0;
            if(find_special(p,&did,&dlen)) break;
            p++;
        }
        int wlen=(int)(p-start);
        if(wlen<=0){p++;continue;}
        char word[TOKENIZER_MAX_TOKEN_LEN];
        if(wlen>=TOKENIZER_MAX_TOKEN_LEN) wlen=TOKENIZER_MAX_TOKEN_LEN-1;
        bm_memcpy(word,start,wlen); word[wlen]='\0';
        /* 4. BPE encode */
        unsigned short ids[128];
        int nids=bpe_encode_word(word,ids,128);
        for(int i=0;i<nids&&n<max_tokens;i++) out_tokens[n++]=ids[i];
    }
    return n;
}

int tokenizer_decode(const unsigned short *tokens, int num_tokens,
                     char *out_text, int max_len)
{
    if(!tok_initialized||!tokens||!out_text||max_len<=0) return -1;
    out_text[0]='\0';
    int pos=0;
    for(int i=0;i<num_tokens;i++) {
        int id=(int)tokens[i];
        if(id<0||id>=tok_vocab_size) id=14; /* [UNK] */
        const char *s=vocab_ptrs[id];
        int len=bm_strlen(s);
        if(pos+len+2>=max_len) break;
        /* Espacio entre tokens normales */
        if(pos>0 && !tokenizer_is_special(id) && !tokenizer_is_special((int)tokens[i-1])) {
            out_text[pos++]=' ';
        }
        bm_memcpy(out_text+pos, s, len);
        pos+=len;
    }
    out_text[pos]='\0';
    return pos;
}

void tokenizer_prepend_special(unsigned short *tokens, int *num_tokens,
                                unsigned short special_id)
{
    if(!tokens||!num_tokens||*num_tokens>=TOKENIZER_MAX_OUTPUT) return;
    for(int i=*num_tokens;i>0;i--) tokens[i]=tokens[i-1];
    tokens[0]=special_id;
    (*num_tokens)++;
}

void tokenizer_append_special(unsigned short *tokens, int *num_tokens,
                               unsigned short special_id)
{
    if(!tokens||!num_tokens||*num_tokens>=TOKENIZER_MAX_OUTPUT) return;
    tokens[(*num_tokens)++]=special_id;
}

void tokenizer_free(void)
{
    tok_initialized=0;
    tok_vocab_size=0;
    num_merges_g=0;
    str_pool_used=0;
    bm_memset(vocab_ptrs,0,sizeof(vocab_ptrs));
}