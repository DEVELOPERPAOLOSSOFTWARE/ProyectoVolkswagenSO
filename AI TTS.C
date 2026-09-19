#ifndef BYDOS_AI_TTS_H
#define BYDOS_AI_TTS_H

/*
 * ai_tts.h -- Sintetizador de voz (Text To Speech) embebido
 * ============================================================================
 * Convierte texto a audio para las bocinas del vehiculo.
 * Modelo TTS propio, ligero, optimizado para comandos cortos en ES/EN.
 * ============================================================================
 */

int  tts_init(const char *model_path);
int  tts_synthesize(const char *text, float *out_audio, int *out_samples);
void tts_free(void);

#endif /* BYDOS_AI_TTS_H */