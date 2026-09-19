#ifndef BYDOS_HW_CONFIG_H
#define BYDOS_HW_CONFIG_H

/*
 * hw_config.h -- FUENTE UNICA DE VERDAD para todo lo especifico de hardware
 * ============================================================================
 *
 * REGLA DEL PROYECTO: ningun otro archivo (boot.S, linker.ld, uart.c,
 * futuros drivers) debe tener una direccion de memoria, tamano de stack,
 * o numero de cores escrito a mano. TODO pasa por aqui.
 *
 * Cuando llegue el datasheet real del SoC de VW, este es el UNICO
 * archivo que se edita para portar el kernel completo. Si algun dia
 * hay que tocar 5 archivos distintos para cambiar de hardware, es
 * señal de que algo se filtro fuera de este archivo -- hay que
 * regresarlo aqui.
 *
 * ============================================================================
 * PERFIL ACTIVO: GENERICO (QEMU virt, aarch64, cortex-a72)
 * ============================================================================
 * Este es un perfil de DESARROLLO, no el hardware final. Todo lo que
 * dice "GENERICO" en los comentarios de abajo cambia cuando tengamos
 * acceso al datasheet oficial de VW.
 */

/*
 * NOTA TECNICA: estas constantes NO llevan sufijo UL (unsigned long)
 * a proposito. Este header lo consumen tanto archivos C (uart.c,
 * kernel.c) como el linker script (linker.ld.S) y como boot.S vía
 * preprocesador -- y el linker de GNU (ld) NO entiende el sufijo UL
 * de C, truena con "syntax error" si lo lleva. Los valores de abajo
 * caben todos en 32 bits sin signo, así que el sufijo no hace falta
 * para que C los interprete correctamente en las conversiones a
 * puntero que ya hacemos en uart.c.
 */

/* ---------------------------------------------------------------------
 * Direccion base de carga del kernel
 * --------------------------------------------------------------------- */
#define HW_KERNEL_BASE_ADDR   0x40080000     /* GENERICO: QEMU virt */

/* ---------------------------------------------------------------------
 * UART (puerto serie de debug)
 * --------------------------------------------------------------------- */
#define HW_UART0_BASE         0x09000000     /* GENERICO: PL011 en QEMU virt */
#define HW_UART_TYPE_PL011    1              /* Marca que driver usar */

/* ---------------------------------------------------------------------
 * Topologia de cores / stacks
 * --------------------------------------------------------------------- */
#define HW_MAX_CORES           4             /* GENERICO: ajustar a specs reales de VW */
#define HW_STACK_SIZE_PER_CORE 0x4000         /* 16KB por core */

/* ---------------------------------------------------------------------
 * Identificacion de CPU esperada (para validar en boot que estamos
 * corriendo sobre el hardware que creemos -- util una vez que
 * tengamos el MIDR_EL1 real del SoC de VW documentado)
 * --------------------------------------------------------------------- */
#define HW_EXPECTED_MIDR_KNOWN 0             /* 0 = aun no lo sabemos, no validar */
#define HW_EXPECTED_MIDR       0x0           /* TODO: llenar con MIDR real de VW */

/* ---------------------------------------------------------------------
 * Interrupt controller (GIC) -- aun no implementado, placeholder
 * documentado para cuando armemos el scheduler con IRQs reales
 * --------------------------------------------------------------------- */
#define HW_GIC_DISTRIBUTOR_BASE 0x08000000   /* GENERICO: GICv2 en QEMU virt */
#define HW_GIC_CPU_IFACE_BASE   0x08010000   /* GENERICO: GICv2 en QEMU virt */

#endif /* BYDOS_HW_CONFIG_H */