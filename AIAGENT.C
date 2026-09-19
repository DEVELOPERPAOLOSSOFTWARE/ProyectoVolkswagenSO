#include "ai_agent.h"
#include "ai_tokenizer.h"
#include "ai_inference.h"
#include "ai_dispatcher.h"
#include "ai_tts.h"
#include "vehicle_bridge.h"
#include "vwfs.h"
#include "uart.h"
#include "timer.h"

/*
 * ai_agent.c -- Orquestador del Agente de Voz
 * ============================================================================
 * Coordina las 5 capas: STT -> Tokenizador -> Inferencia -> Dispatcher -> TTS
 *
 * En desarrollo (sin hardware de microfono real):
 *   - ai_inject_text() simula la salida del STT
 *   - El resto del pipeline corre completo
 *
 * En produccion (con Whisper + microfono real):
 *   - ai_start_listening() activa el buffer de audio
 *   - ai_stop_listening() dispara Whisper sobre el buffer
 *   - El resto del pipeline es identico
 * ============================================================================
 */

/* Estado interno del agente */
static ai_state_t        agent_state    = AI_STATE_IDLE;
static ai_result_t       last_result;
static ai_response_callback_t response_cb = 0;
static ai_state_callback_t    state_cb    = 0;

/* Buffer de tokens */
static unsigned short input_tokens[AI_MAX_TOKENS];
static unsigned short output_tokens[AI_MAX_TOKENS];

/* Flags de modulos disponibles */
static int tokenizer_ready  = 0;
static int inference_ready  = 0;
static int tts_ready        = 0;
static int models_loaded    = 0;

/* -------------------------------------------------------------------------
 * Utilidades internas
 * ------------------------------------------------------------------------- */

static void set_state(ai_state_t new_state)
{
    agent_state = new_state;
    if (state_cb) state_cb(new_state);
}

/* Construir el contexto del vehiculo en formato [VEH]...[/VEH] */
static int build_vehicle_context(char *buf, int max_len)
{
    const vb_data_t *v = vb_get_data();
    if (!v || !v->valid) {
        buf[0] = '\0';
        return 0;
    }

    /* Imprimir campos clave del vehiculo en el contexto */
    int n = 0;
    const char *prefix = "[VEH] ";
    while (*prefix && n < max_len - 1) buf[n++] = *prefix++;

    /* speed */
    const char *sp = "speed=";
    while (*sp && n < max_len - 1) buf[n++] = *sp++;
    unsigned int spd = v->speed_kmh;
    char tmp[12]; int ti = 0;
    if (!spd) { tmp[ti++] = '0'; }
    else { while (spd) { tmp[ti++] = '0' + (spd % 10); spd /= 10; } }
    for (int a=0,b=ti-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    for (int i=0; i<ti && n<max_len-1; i++) buf[n++] = tmp[i];
    if (n < max_len - 1) buf[n++] = ' ';

    /* battery */
    const char *bt = "battery=";
    while (*bt && n < max_len-1) buf[n++] = *bt++;
    unsigned int batt = v->battery_pct;
    ti = 0;
    if (!batt) { tmp[ti++] = '0'; }
    else { while (batt) { tmp[ti++] = '0' + (batt % 10); batt /= 10; } }
    for (int a=0,b=ti-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    for (int i=0; i<ti && n<max_len-1; i++) buf[n++] = tmp[i];
    if (n < max_len - 1) buf[n++] = ' ';

    /* range */
    const char *rn = "range=";
    while (*rn && n < max_len-1) buf[n++] = *rn++;
    unsigned int rng = v->range_km;
    ti = 0;
    if (!rng) { tmp[ti++] = '0'; }
    else { while (rng) { tmp[ti++] = '0' + (rng % 10); rng /= 10; } }
    for (int a=0,b=ti-1; a<b; a++,b--) { char t=tmp[a]; tmp[a]=tmp[b]; tmp[b]=t; }
    for (int i=0; i<ti && n<max_len-1; i++) buf[n++] = tmp[i];
    if (n < max_len - 1) buf[n++] = ' ';

    /* alerts */
    const char *al = "alerts=";
    while (*al && n < max_len-1) buf[n++] = *al++;
    const char *alv = (v->alerts == VB_ALERT_NONE) ? "NONE" : "ACTIVE";
    while (*alv && n < max_len-1) buf[n++] = *alv++;
    if (n < max_len - 1) buf[n++] = ' ';

    /* cierre */
    const char *suffix = "[/VEH]";
    while (*suffix && n < max_len-1) buf[n++] = *suffix++;
    buf[n] = '\0';
    return n;
}

/* str_copy simple sin libc */
static void str_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int str_len_s(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

/* -------------------------------------------------------------------------
 * Procesamiento principal del pipeline
 * -------------------------------------------------------------------------
 * Entrada: texto transcrito del conductor
 * Salida:  respuesta de voz + comando ejecutado si aplica
 * ------------------------------------------------------------------------- */
static void process_input(const char *text)
{
    unsigned long t_start = timer_get_ticks();
    set_state(AI_STATE_PROCESSING);

    /* Limpiar resultado anterior */
    volatile unsigned char *p = (volatile unsigned char *)&last_result;
    for (unsigned int i = 0; i < sizeof(ai_result_t); i++) p[i] = 0;
    str_copy(last_result.input_text, text, sizeof(last_result.input_text));

    uart_puts("[ai] Procesando: ");
    uart_puts(text);
    uart_puts("\n");

    /* ---- PASO 1: Construir prompt completo ---- */
    /* Formato: [USR] texto [EOS] [VEH] contexto [/VEH] [AST] */
    char full_prompt[1024];
    char veh_ctx[256];
    build_vehicle_context(veh_ctx, sizeof(veh_ctx));

    /* Armar el prompt */
    int pi = 0;
    const char *usr = "[USR] ";
    while (*usr && pi < 1020) full_prompt[pi++] = *usr++;
    int ti = 0;
    while (text[ti] && pi < 1020) full_prompt[pi++] = text[ti++];
    const char *eos = " [EOS] ";
    while (*eos && pi < 1020) full_prompt[pi++] = *eos++;
    int vi = 0;
    while (veh_ctx[vi] && pi < 1020) full_prompt[pi++] = veh_ctx[vi++];
    const char *ast = " [AST]";
    while (*ast && pi < 1020) full_prompt[pi++] = *ast++;
    full_prompt[pi] = '\0';

    /* ---- PASO 2: Tokenizar ---- */
    int n_tokens = 0;
    if (tokenizer_ready) {
        n_tokens = tokenizer_encode(full_prompt, input_tokens, AI_MAX_TOKENS);
        uart_puts("[ai] Tokens generados: ");
        char tbuf[8]; int tbi = 0; unsigned int tn = (unsigned int)n_tokens;
        if (!tn) { tbuf[tbi++]='0'; }
        else { while (tn) { tbuf[tbi++]='0'+(tn%10); tn/=10; } }
        for (int a=0,b=tbi-1; a<b; a++,b--) { char t=tbuf[a]; tbuf[a]=tbuf[b]; tbuf[b]=t; }
        tbuf[tbi]='\0'; uart_puts(tbuf); uart_puts("\n");
    } else {
        /* Tokenizador no disponible aun -- modo stub */
        uart_puts("[ai] STUB: tokenizador pendiente del equipo de IA.\n");
        uart_puts("[ai] Pipeline listo para conectar cuando llegue ai_tokenizer.c\n");

        /* Respuesta de stub basada en comandos simples reconocidos */
        const char *resp = 0;
        if (str_len_s(text) > 0) {
            /* Reconocimiento de palabras clave sin modelo -- temporal */
            if (text[0]=='b' || text[0]=='B') resp = "Bateria al 85 por ciento, autonomia de 340 kilometros.";
            else if (text[0]=='v' || text[0]=='V') resp = "Velocidad actual: 120 kilometros por hora.";
            else if (text[0]=='c' || text[0]=='C') resp = "Climatizador ajustado.";
            else resp = "Comando recibido. Sistema operativo VW OS activo.";
        }
        if (!resp) resp = "Sistema listo.";
        str_copy(last_result.response_text, resp, sizeof(last_result.response_text));
        goto respond;
    }

    /* ---- PASO 3: Inferencia ---- */
    int n_output = 0;
    if (inference_ready) {
        n_output = inference_generate(input_tokens, n_tokens,
                                       output_tokens, AI_MAX_NEW_TOKENS);
    } else {
        uart_puts("[ai] STUB: motor de inferencia pendiente.\n");
        str_copy(last_result.response_text,
                 "Modelo de inferencia pendiente de integracion.",
                 sizeof(last_result.response_text));
        goto respond;
    }

    /* ---- PASO 4: Detokenizar ---- */
    char raw_response[1024];
    if (tokenizer_ready && n_output > 0) {
        tokenizer_decode(output_tokens, n_output, raw_response, sizeof(raw_response));
    } else {
        str_copy(raw_response, "[AST] Sistema OK. [EOS]", sizeof(raw_response));
    }

    /* ---- PASO 5: Dispatcher -- ejecutar comandos del OS ---- */
    dispatch_result_t dispatch;
    dispatcher_process(raw_response, &dispatch);
    str_copy(last_result.response_text, dispatch.clean_text,
             sizeof(last_result.response_text));
    str_copy(last_result.command, dispatch.command,
             sizeof(last_result.command));
    last_result.command_executed = dispatch.success;

respond:
    /* ---- PASO 6: TTS ---- */
    if (tts_ready) {
        float audio_buf[HW_AUDIO_SAMPLE_RATE * 5];
        int   n_samples = 0;
        tts_synthesize(last_result.response_text, audio_buf, &n_samples);
        /* TODO: enviar audio_buf a las bocinas via driver de audio */
        uart_puts("[ai] TTS: audio generado (");
        char sbuf[8]; int sbi = 0; unsigned int sn = (unsigned int)n_samples;
        if (!sn) { sbuf[sbi++]='0'; }
        else { while (sn) { sbuf[sbi++]='0'+(sn%10); sn/=10; } }
        for (int a=0,b=sbi-1; a<b; a++,b--) { char t=sbuf[a]; sbuf[a]=sbuf[b]; sbuf[b]=t; }
        sbuf[sbi]='\0'; uart_puts(sbuf);
        uart_puts(" muestras)\n");
    }

    uart_puts("[ai] Respuesta: ");
    uart_puts(last_result.response_text);
    uart_puts("\n");

    last_result.processing_ms =
        (timer_get_ticks() - t_start) * 10;  /* ticks -> ms */
    last_result.final_state = AI_STATE_IDLE;

    set_state(AI_STATE_IDLE);

    if (response_cb) response_cb(&last_result);
}

/* -------------------------------------------------------------------------
 * API publica
 * ------------------------------------------------------------------------- */

int ai_init(void)
{
    uart_puts("[ai] Iniciando agente de voz...\n");

    /* Verificar que existen los archivos de modelo en VWFS */
    vwfs_inode_t inode;
    int has_vocab    = (vwfs_stat(AI_VOCAB_PATH,    &inode) == 0);
    int has_model    = (vwfs_stat(AI_MODEL_PATH,    &inode) == 0);
    int has_whisper  = (vwfs_stat(AI_WHISPER_PATH,  &inode) == 0);
    int has_tts      = (vwfs_stat(AI_TTS_PATH,      &inode) == 0);

    uart_puts("[ai] Modelos en VWFS:\n");
    uart_puts(has_vocab   ? "[ai]   vocab.json     : OK\n"
                          : "[ai]   vocab.json     : PENDIENTE (equipo IA)\n");
    uart_puts(has_model   ? "[ai]   model.bin      : OK\n"
                          : "[ai]   model.bin      : PENDIENTE (equipo IA)\n");
    uart_puts(has_whisper ? "[ai]   whisper.bin    : OK\n"
                          : "[ai]   whisper.bin    : PENDIENTE (equipo IA)\n");
    uart_puts(has_tts     ? "[ai]   tts.bin        : OK\n"
                          : "[ai]   tts.bin        : PENDIENTE (equipo IA)\n");

    /* Inicializar modulos disponibles */
    dispatcher_init();

    if (has_vocab) {
        /* tokenizer_init(AI_VOCAB_PATH, AI_MERGES_PATH); */
        tokenizer_ready = 0;  /* activa cuando llegue ai_tokenizer.c */
    }
    if (has_model) {
        /* inference_init(AI_MODEL_PATH, AI_CONFIG_PATH); */
        inference_ready = 0;  /* activa cuando llegue ai_inference.c */
    }
    if (has_tts) {
        /* tts_init(AI_TTS_PATH); */
        tts_ready = 0;        /* activa cuando llegue ai_tts.c */
    }

    models_loaded = 1;
    set_state(AI_STATE_IDLE);

    uart_puts("[ai] Agente listo. Pipeline completo -- esperando modelos del equipo IA.\n");
    uart_puts("[ai] Modo actual: STUB (reconocimiento de palabras clave basico).\n");
    return 0;
}

void ai_inject_text(const char *text)
{
    if (agent_state != AI_STATE_IDLE) {
        uart_puts("[ai] Ocupado, ignorando entrada.\n");
        return;
    }
    process_input(text);
}

void ai_start_listening(void)
{
    if (agent_state != AI_STATE_IDLE) return;
    set_state(AI_STATE_LISTENING);
    uart_puts("[ai] Escuchando... (microfono activo)\n");
    /* TODO: activar buffer de audio en HW_AUDIO_IN_BASE via driver */
}

void ai_stop_listening(void)
{
    if (agent_state != AI_STATE_LISTENING) return;
    uart_puts("[ai] Procesando audio con Whisper...\n");
    /* TODO: correr whisper sobre el buffer de audio grabado */
    /* Por ahora simular transcripcion */
    process_input("cuanta bateria queda");
}

void ai_register_response_callback(ai_response_callback_t cb) { response_cb = cb; }
void ai_register_state_callback(ai_state_callback_t cb)       { state_cb    = cb; }
ai_state_t ai_get_state(void)  { return agent_state; }
void ai_tick(void)             { /* tick del scheduler -- futuro: timeout de escucha */ }
void ai_free(void)
{
    if (tokenizer_ready)  tokenizer_free();
    if (inference_ready)  inference_free();
    if (tts_ready)        tts_free();
}