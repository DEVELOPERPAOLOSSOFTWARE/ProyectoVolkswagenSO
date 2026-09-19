# VW OS Tokenizer (BPE)

## Formato recomendado (listo para usar)

```
tokenizer/
├── vocab.json      ← Vocabulario (tokens especiales + base)
├── merges.txt      ← Reglas BPE
├── tokenizer.h     ← Header C
└── tokenizer.c     ← Implementación C
```

## Tokens especiales (IDs fijos 0-15)

| ID | Token     | Uso                          |
|----|-----------|------------------------------|
| 0  | [BOS]     | Inicio de conversación       |
| 1  | [EOS]     | Fin de respuesta             |
| 2  | [USR]     | Turno del conductor          |
| 3  | [AST]     | Turno del asistente          |
| 4  | [CMD]     | Inicio comando OS            |
| 5  | [/CMD]    | Fin comando OS               |
| 6  | [VEH]     | Inicio datos vehículo        |
| 7  | [/VEH]    | Fin datos vehículo           |
| 8  | [ALRT]    | Alerta sistema               |
| 9  | [NAV]     | Navegación                   |
| 10 | [MEDIA]   | Multimedia                   |
| 11 | [CLIMA]   | Climatizador                 |
| 12 | [CALL]    | Llamadas                     |
| 13 | [DIAG]    | Diagnóstico                  |
| 14 | [UNK]     | Desconocido                  |
| 15 | [PAD]     | Padding                      |

## Uso rápido (C)

```c
#include "tokenizer.h"

Tokenizer tok;
tokenizer_init(&tok, "vocab.json", "merges.txt");

int ids[2048];
int n = tokenizer_encode(&tok, "[USR] Pon el clima a 22 [EOS]", ids, 2048);

char text[1024];
tokenizer_decode(&tok, ids, n, text, sizeof(text));

tokenizer_free(&tok);
```

## Notas

- Los tokens especiales tienen **prioridad absoluta** en el encode.
- Este es un vocabulario base (listo para empezar). 
  Cuando tengas el dataset completo se regenera a 32.000 tokens.
- Formato 100% compatible con el estilo GPT-2 / LLaMA.
