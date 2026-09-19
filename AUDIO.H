#include "audio.h"
#include "gic.h"
#include "timer.h"
#include "uart.h"
#include "hw_config.h"

/*
 * audio.c -- Driver de audio bare-metal
 * ============================================================================
 * Tres capas exactamente como net.c:
 *
 *   CAPA BAJA   : audio_hw_* -- registros I2S del SoC (datasheet de VW)
 *   CAPA MEDIA  : buffer circular, IRQ, conversión de formato
 *   CAPA ALTA   : audio_mic_start/stop, audio_spk_play (lo que llama el OS)
 *
 * En QEMU (HW_AUDIO_TYPE_NONE):
 *   audio_mic_start() retorna AUDIO_ERR_NO_HW
 *   audio_spk_play()  simula reproducción sin sonido real
 *   El agente de voz usa audio_inject_mic_data() para pruebas
 *
 * Para portar al SoC real de VW:
 *   Solo implementar la capa baja con el datasheet.
 *   La capa alta y el agente de IA no cambian.
 * ============================================================================
 */

/* =========================================================================
 * ESTADO INTERNO
 * ========================================================================= */
static audio_state_t     audio_state    = AUDIO_STATE_NO_HW;
static audio_mic_callback_t mic_callback = 0;
static unsigned int      volume_pct     = 80;

/* Buffer circular de micrófono en RAM */
static float mic_buf[AUDIO_MIC_BUF_SAMPLES];
static volatile int mic_buf_samples = 0;   /* muestras capturadas */
static volatile int mic_recording   = 0;

/* Buffer de bocinas */
static const float *spk_buf        = 0;
static int          spk_samples    = 0;
static volatile int spk_pos        = 0;    /* posición de reproducción */
static volatile int spk_playing    = 0;

/* =========================================================================
 * CAPA BAJA -- hardware I2S del SoC
 * =========================================================================
 * Todo lo que toca registros físicos está aquí.
 * Es el ÚNICO bloque que cambia con el datasheet de VW.
 * ========================================================================= */

#if HW_AUDIO_TYPE == HW_AUDIO_TYPE_NONE

/* Sin hardware -- modo desarrollo */
__attribute__((used)) static int  audio_hw_init(void)    { return -1; }
__attribute__((used)) static int  audio_hw_mic_read(float *buf, int max) { (void)buf;(void)max; return 0; }
__attribute__((used)) static int  audio_hw_spk_write(const float *buf, int n, unsigned int vol) { (void)buf;(void)n;(void)vol; return n; }
__attribute__((used)) static void audio_hw_set_volume(unsigned int pct) { (void)pct; }

#elif HW_AUDIO_TYPE == HW_AUDIO_TYPE_I2S

/*
 * I2S (Inter-IC Sound) -- el protocolo más común en SoCs automotrices.
 *
 * Registros típicos de un controlador I2S ARM (ajustar con datasheet de VW):
 *   0x00 CONTROL   : enable, reset, modo maestro/esclavo
 *   0x04 CONFIG    : sample rate, bits por muestra, canales
 *   0x08 STATUS    : TX empty, RX available, overrun, underrun
 *   0x0C TX_DATA   : escribir muestras para bocinas (32 bits = float via union)
 *   0x10 RX_DATA   : leer muestras del micrófono
 *   0x14 VOL_CTRL  : control de volumen (0x00-0xFF)
 *   0x18 INT_ENABLE: habilitar IRQ por RX_AVAILABLE y TX_EMPTY
 *   0x1C INT_STATUS: flags de interrupción (escribir 1 para limpiar)
 *
 * Conversión float32 <-> PCM16:
 *   Para el micrófono: raw_int16 / 32768.0f -> float normalizado [-1.0, 1.0]
 *   Para bocinas:      sample_float * 32768.0f -> int16 -> registro TX
 */

#define I2S_REG(off) (*(volatile unsigned int*)((unsigned long)HW_AUDIO_I2S_BASE+(off)))

/* Unión para convertir float a bits sin cast UB */
typedef union { float f; unsigned int u; } float_bits_t;

static int audio_hw_init(void)
{
    /* Reset del controlador */
    I2S_REG(0x00) = 0x80;           /* bit 7 = reset */
    /* Esperar reset */
    unsigned long t = timer_get_ticks();
    while (timer_get_ticks() - t < 10) { }
    I2S_REG(0x00) = 0x00;

    /* Configurar sample rate y formato */
    /* Bits [15:0] = divisor de clock para el sample rate deseado
     * La fórmula depende del clock del SoC -- TODO: calcular con datasheet */
    I2S_REG(0x04) = 0x0002;         /* 16 bits por muestra, 2 canales */

    /* Habilitar IRQ por RX disponible */
    I2S_REG(0x18) = 0x01;

    /* Habilitar el controlador en modo maestro */
    I2S_REG(0x00) = 0x01;

    return 0;
}

static int audio_hw_mic_read(float *buf, int max)
{
    int count = 0;
    while (count < max && (I2S_REG(0x08) & 0x02)) { /* RX_AVAILABLE */
        unsigned int raw = I2S_REG(0x10);
        /* Convertir PCM16 signed a float normalizado */
        short pcm = (short)(raw & 0xFFFF);
        buf[count++] = (float)pcm / 32768.0f;
    }
    return count;
}

static int audio_hw_spk_write(const float *buf, int n, unsigned int vol)
{
    int written = 0;
    /* Factor de volumen: 0-100% -> 0.0-1.0 */
    float vol_factor = (float)vol / 100.0f;

    while (written < n && (I2S_REG(0x08) & 0x01)) { /* TX_EMPTY */
        float sample = buf[written] * vol_factor;
        /* Limitar a [-1.0, 1.0] */
        if (sample >  1.0f) sample =  1.0f;
        if (sample < -1.0f) sample = -1.0f;
        /* Convertir float a PCM16 */
        short pcm = (short)(sample * 32767.0f);
        /* Escribir al registro TX (canal izq + canal der = mismo valor) */
        I2S_REG(0x0C) = ((unsigned int)(unsigned short)pcm << 16) |
                         (unsigned int)(unsigned short)pcm;
        written++;
    }
    return written;
}

static void audio_hw_set_volume(unsigned int pct)
{
    if (pct > 100) pct = 100;
    /* Mapear 0-100% a 0x00-0xFF del registro de volumen */
    I2S_REG(0x14) = (pct * 255) / 100;
}

#endif /* HW_AUDIO_TYPE */

/* =========================================================================
 * CAPA MEDIA -- IRQ handler y buffer circular
 * ========================================================================= */

static void audio_irq_handler(unsigned int irq_id)
{
    (void)irq_id;

    /* Leer muestras del micrófono si estamos grabando */
    if (mic_recording && mic_buf_samples < AUDIO_MIC_BUF_SAMPLES) {
        int n = audio_hw_mic_read(
            &mic_buf[mic_buf_samples],
            AUDIO_MIC_BUF_SAMPLES - mic_buf_samples
        );
        mic_buf_samples += n;

        /* Buffer lleno -- notificar al agente de IA */
        if (mic_buf_samples >= AUDIO_MIC_BUF_SAMPLES) {
            mic_recording = 0;
            audio_state   = AUDIO_STATE_IDLE;
            if (mic_callback) {
                mic_callback(mic_buf, mic_buf_samples);
            }
        }
    }

    /* Enviar muestras a las bocinas si estamos reproduciendo */
    if (spk_playing && spk_buf && spk_pos < spk_samples) {
        int to_write = spk_samples - spk_pos;
        int written  = audio_hw_spk_write(
            &spk_buf[spk_pos], to_write, volume_pct
        );
        spk_pos += written;

        if (spk_pos >= spk_samples) {
            spk_playing   = 0;
            audio_state   = AUDIO_STATE_IDLE;
        }
    }
}

/* =========================================================================
 * GENERADOR DE BEEP (sin hardware -- cálculo puro)
 * =========================================================================
 * Genera una onda senoidal simple para notificaciones del sistema.
 * Sin tabla de senos -- aproximación por polinomio de Bhaskara I.
 * Sin libc, sin math.h.
 * ========================================================================= */
static float fast_sin(float x)
{
    /* Normalizar x a [0, 2π] */
    while (x > 6.2832f) x -= 6.2832f;
    while (x < 0.0f)    x += 6.2832f;
    /* Aproximación de Bhaskara I: sin(x) ≈ 16x(π-x) / (5π²-4x(π-x)) */
    float pi = 3.14159f;
    if (x > pi) { x -= pi; return -fast_sin(x); }
    float num = 16.0f * x * (pi - x);
    float den = 5.0f * pi * pi - 4.0f * x * (pi - x);
    return num / den;
}

#define BEEP_BUF_SAMPLES  (HW_AUDIO_SAMPLE_RATE * 2)  /* max 2 segundos */
static float beep_buf[BEEP_BUF_SAMPLES];

/* =========================================================================
 * CAPA ALTA -- API pública
 * ========================================================================= */

void audio_init(void)
{
    uart_puts("[audio] Iniciando driver de audio...\n");

#if HW_AUDIO_TYPE == HW_AUDIO_TYPE_NONE
    uart_puts("[audio] Modo: sin hardware (QEMU/desarrollo).\n");
    uart_puts("[audio] Agente de voz usara audio_inject_mic_data() para pruebas.\n");
    audio_state = AUDIO_STATE_NO_HW;
#else
    uart_puts("[audio] Tipo: I2S ");
    uart_puts("(");
    char tmp[8]; int ti=0;
    unsigned int sr = HW_AUDIO_SAMPLE_RATE;
    while(sr){tmp[ti++]='0'+(sr%10);sr/=10;}
    for(int a=0,b=ti-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
    tmp[ti]='\0'; uart_puts(tmp);
    uart_puts(" Hz)\n");

    int r = audio_hw_init();
    if (r == 0) {
        /* Registrar IRQ del controlador de audio en el GIC */
        gic_register_handler(HW_AUDIO_IRQ_ID, audio_irq_handler);
        gic_enable_irq(HW_AUDIO_IRQ_ID);
        audio_hw_set_volume(volume_pct);
        audio_state = AUDIO_STATE_IDLE;
        uart_puts("[audio] Controlador I2S activo.\n");
    } else {
        audio_state = AUDIO_STATE_NO_HW;
        uart_puts("[audio] Error al inicializar I2S.\n");
    }
#endif
}

audio_state_t audio_get_state(void) { return audio_state; }

/* -------------------------------------------------------------------------
 * Micrófono
 * ------------------------------------------------------------------------- */
int audio_mic_start(audio_mic_callback_t callback)
{
    if (audio_state == AUDIO_STATE_NO_HW) return AUDIO_ERR_NO_HW;
    if (audio_state == AUDIO_STATE_RECORDING) return AUDIO_ERR_BUSY;

    mic_callback    = callback;
    mic_buf_samples = 0;
    mic_recording   = 1;
    audio_state     = AUDIO_STATE_RECORDING;

    uart_puts("[audio] Grabando voz...\n");
    return AUDIO_OK;
}

int audio_mic_stop(void)
{
    mic_recording = 0;
    if (audio_state == AUDIO_STATE_RECORDING) {
        audio_state = AUDIO_STATE_IDLE;
        /* Entregar lo que se capturó hasta ahora */
        if (mic_callback && mic_buf_samples > 0) {
            mic_callback(mic_buf, mic_buf_samples);
        }
    }
    return AUDIO_OK;
}

/* -------------------------------------------------------------------------
 * Bocinas
 * ------------------------------------------------------------------------- */
int audio_spk_play(const float *samples, int num_samples)
{
    if (audio_state == AUDIO_STATE_NO_HW) {
        /* Sin hardware: simular reproducción reportando duración */
        uart_puts("[audio] SIMULADO: reproduciendo ");
        char tmp[12]; int ti=0;
        unsigned int ms = (unsigned int)num_samples / (HW_AUDIO_SAMPLE_RATE/1000);
        while(ms){tmp[ti++]='0'+(ms%10);ms/=10;}
        for(int a=0,b=ti-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
        tmp[ti]='\0'; uart_puts(tmp);
        uart_puts(" ms de audio.\n");
        return AUDIO_OK;
    }
    if (spk_playing) return AUDIO_ERR_BUSY;

    spk_buf     = samples;
    spk_samples = num_samples;
    spk_pos     = 0;
    spk_playing = 1;
    audio_state = AUDIO_STATE_PLAYING;

    /* Arrancar la reproducción enviando el primer bloque */
    audio_irq_handler(HW_AUDIO_IRQ_ID);
    return AUDIO_OK;
}

int audio_spk_beep(unsigned int freq_hz, unsigned int duration_ms)
{
    int num_samples = (int)((unsigned long)HW_AUDIO_SAMPLE_RATE *
                             duration_ms / 1000);
    if (num_samples > BEEP_BUF_SAMPLES)
        num_samples = BEEP_BUF_SAMPLES;

    /* Generar onda senoidal a la frecuencia dada */
    float phase_inc = 6.2832f * (float)freq_hz / (float)HW_AUDIO_SAMPLE_RATE;
    float phase     = 0.0f;
    float amplitude = 0.3f;  /* volumen del beep: 30% para no asustar */

    for (int i = 0; i < num_samples; i++) {
        /* Fade in/out de 10ms para evitar clicks */
        float env = 1.0f;
        int fade_samples = HW_AUDIO_SAMPLE_RATE / 100;  /* 10ms */
        if (i < fade_samples)
            env = (float)i / (float)fade_samples;
        else if (i > num_samples - fade_samples)
            env = (float)(num_samples - i) / (float)fade_samples;

        beep_buf[i] = fast_sin(phase) * amplitude * env;
        phase += phase_inc;
        if (phase > 6.2832f) phase -= 6.2832f;
    }

    return audio_spk_play(beep_buf, num_samples);
}

int  audio_spk_busy(void)   { return spk_playing; }
void audio_spk_stop(void)   { spk_playing = 0; audio_state = AUDIO_STATE_IDLE; }
void audio_set_volume(unsigned int pct) {
    if (pct > 100) pct = 100;
    volume_pct = pct;
    audio_hw_set_volume(pct);
}

/* -------------------------------------------------------------------------
 * Inyección de datos de micrófono para pruebas sin hardware
 * -------------------------------------------------------------------------
 * En QEMU: el agente de IA llama esto con audio pregrabado para probar
 * el pipeline completo (micrófono → Whisper → agente → TTS → bocinas)
 * sin necesitar hardware físico.
 * En producción: esta función no se llama -- los datos vienen del ADC.
 * ------------------------------------------------------------------------- */
void audio_inject_mic_data(const float *samples, int num_samples)
{
    if (!mic_callback) return;
    uart_puts("[audio] Inyectando datos de micrófono para prueba.\n");
    mic_callback(samples, num_samples);
}