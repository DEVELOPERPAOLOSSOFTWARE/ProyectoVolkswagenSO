#include "net.h"
#include "vwfs.h"
#include "uart.h"
#include "timer.h"
#include "gic.h"
#include "hw_config.h"

/*
 * net.c -- Cliente de red minimalista para OTA
 * ============================================================================
 * Tres capas:
 *
 *   CAPA ALTA   : net_request() / net_download() -- lo que llama el OTA
 *   CAPA MEDIA  : net_hw_send() / net_hw_recv()  -- habla con el SoC
 *   CAPA BAJA   : registros del controlador de red (HW_NET_BASE)
 *
 * En desarrollo (HW_NET_TYPE_NONE / QEMU):
 *   net_available() retorna 0. El OTA detecta NET_ERR_NO_HW y reporta
 *   "Sin red disponible" sin intentar nada. Comportamiento correcto.
 *
 * En hardware real (HW_NET_TYPE_WIFI / ETH_AUTO / 5G):
 *   La capa baja lee/escribe registros del controlador del SoC.
 *   La capa media ensambla/desensambla los paquetes.
 *   La capa alta no cambia.
 *
 * Para portar a VW real:
 *   1. Cambiar HW_NET_TYPE en hw_config.h
 *   2. Implementar net_hw_init() / net_hw_send() / net_hw_recv()
 *      con los registros del datasheet del SoC
 *   3. net_request() y net_download() no se tocan
 * ============================================================================
 */

static net_state_t net_state = NET_STATE_NO_HW;

/* -------------------------------------------------------------------------
 * Utilidades internas
 * ------------------------------------------------------------------------- */
static void scopy(char *d, const char *s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]='\0';
}
static int slen(const char *s) {
    int n=0; while(s[n]) n++; return n;
}
static void uart_dec(unsigned int v) {
    char tmp[12]; int ti=0;
    if(!v){uart_putc('0');return;}
    while(v){tmp[ti++]='0'+(v%10);v/=10;}
    for(int a=0,b=ti-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
    tmp[ti]='\0'; uart_puts(tmp);
}

/* =========================================================================
 * CAPA BAJA -- hardware especifico del SoC
 * =========================================================================
 * Todo lo que toca registros fisicos esta aqui.
 * Es el UNICO bloque que cambia cuando migramos al SoC real de VW.
 * ========================================================================= */

#if HW_NET_TYPE == HW_NET_TYPE_NONE

/* Sin hardware de red -- modo desarrollo */
static int  net_hw_init(void)                                  { return -1; }
static int  net_hw_send(const void *d, unsigned int l)         { (void)d;(void)l; return -1; }
static int  net_hw_recv(void *d, unsigned int max, unsigned int timeout_ticks)
                                                               { (void)d;(void)max;(void)timeout_ticks; return -1; }
static int  net_hw_connected(void)                             { return 0; }

#elif HW_NET_TYPE == HW_NET_TYPE_WIFI

/*
 * WiFi via controlador SDIO/PCIe del SoC.
 * Patron tipico para chips Qualcomm WLAN o Broadcom BCM43xx:
 *
 *   1. net_hw_init(): resetear el chip, cargar firmware via DMA,
 *      configurar SSID/password (desde /config/wifi.cfg en VWFS),
 *      esperar asociacion al AP.
 *
 *   2. net_hw_send(): escribir el paquete en el buffer TX (HW_NET_TX_BUF_BASE),
 *      escribir el tamano en el registro de control,
 *      disparar la transmision.
 *
 *   3. net_hw_recv(): esperar IRQ o polling del registro de status,
 *      leer del buffer RX (HW_NET_RX_BUF_BASE),
 *      retornar bytes leidos.
 *
 * TODO: implementar cuando VW confirme el chip WiFi del SoC.
 * Los registros exactos vienen del datasheet del controlador.
 */
#define NET_REG(off) (*(volatile unsigned int*)((unsigned long)HW_NET_BASE+(off)))

static int net_hw_init(void)
{
    uart_puts("[net] Inicializando WiFi (TODO: datasheet del SoC de VW).\n");
    /* TODO:
     * NET_REG(0x00) = 0x01;  // reset
     * NET_REG(0x04) = ...;   // cargar firmware
     * NET_REG(0x08) = ...;   // configurar SSID
     * esperar NET_REG(0x0C) & 0x01 (asociado)
     */
    return -1;  /* sin hardware real: siempre falla */
}
static int net_hw_send(const void *d, unsigned int l)
{
    (void)d; (void)l;
    /* TODO: escribir d en HW_NET_TX_BUF_BASE y disparar TX */
    return -1;
}
static int net_hw_recv(void *d, unsigned int max, unsigned int timeout_ticks)
{
    (void)d; (void)max; (void)timeout_ticks;
    /* TODO: polling/IRQ del buffer RX */
    return -1;
}
static int net_hw_connected(void)
{
    /* TODO: leer bit de estado del controlador */
    return 0;
}

#elif HW_NET_TYPE == HW_NET_TYPE_ETH_AUTO

/*
 * Ethernet automotriz 100BASE-T1 (un solo par trenzado, full duplex).
 * Comun en vehiculos modernos para comunicacion entre dominios.
 * El controlador tipico es un MAC Ethernet integrado en el SoC
 * con un PHY 100BASE-T1 externo (ej. NXP TJA1100).
 *
 * TODO: implementar con el datasheet del SoC y PHY de VW.
 */
static int  net_hw_init(void)                                  { return -1; }
static int  net_hw_send(const void *d, unsigned int l)         { (void)d;(void)l; return -1; }
static int  net_hw_recv(void *d, unsigned int max, unsigned int to)
                                                               { (void)d;(void)max;(void)to; return -1; }
static int  net_hw_connected(void)                             { return 0; }

#elif HW_NET_TYPE == HW_NET_TYPE_5G

/*
 * Modulo 5G/LTE (ej. Quectel EM20, Sierra Wireless).
 * Tipicamente conectado via USB o PCIe, con interfaz AT commands
 * o un driver propietario.
 *
 * TODO: implementar con el datasheet del modulo celular de VW.
 */
static int  net_hw_init(void)                                  { return -1; }
static int  net_hw_send(const void *d, unsigned int l)         { (void)d;(void)l; return -1; }
static int  net_hw_recv(void *d, unsigned int max, unsigned int to)
                                                               { (void)d;(void)max;(void)to; return -1; }
static int  net_hw_connected(void)                             { return 0; }

#endif /* HW_NET_TYPE */

/* =========================================================================
 * CAPA MEDIA -- protocolo minimo sobre el hardware
 * =========================================================================
 * Ensambla peticiones y parsea respuestas.
 * Sin HTTP completo -- solo lo que el OTA necesita:
 *   - Enviar "GET /api/v1/manifest" al servidor de VW
 *   - Recibir la respuesta JSON del manifest
 *   - Descargar chunks binarios del paquete
 * ========================================================================= */

#define PROTO_BUF_SIZE    512   /* buffer para request/response headers   */
#define CHUNK_SIZE        4096  /* 4KB por chunk de descarga              */

static unsigned char proto_buf[PROTO_BUF_SIZE];
static unsigned char chunk_buf[CHUNK_SIZE];

/* Construir una peticion minima al servidor OTA de VW.
 * Formato propietario -- VW define el protocolo real bajo NDA.
 * Por ahora: cabecera simple con URL y version del OS. */
static int net_build_request(const char *url,
                              unsigned char *out, int max)
{
    /* Formato de request propietario VW OS:
     * [4 bytes magic "VWOS"] [2 bytes version] [2 bytes url_len] [url] */
    if (max < 12 + slen(url)) return -1;

    out[0]='V'; out[1]='W'; out[2]='O'; out[3]='S'; /* magic */
    out[4]=1;   out[5]=0;                             /* version 1.0 */
    int ulen = slen(url);
    out[6]=(unsigned char)(ulen & 0xFF);
    out[7]=(unsigned char)(ulen >> 8);
    for (int i=0; i<ulen && 8+i<max; i++) out[8+i] = (unsigned char)url[i];
    return 8 + ulen;
}

/* Parsear respuesta del servidor OTA.
 * El servidor responde: [4 bytes magic] [4 bytes body_len] [body...] */
static int net_parse_response(const unsigned char *data, int data_len,
                               void *out_body, unsigned int max_body)
{
    if (data_len < 8) return -1;
    if (data[0]!='V'||data[1]!='W'||data[2]!='O'||data[3]!='S') return -1;
    unsigned int body_len = (unsigned int)data[4]       |
                            ((unsigned int)data[5] << 8) |
                            ((unsigned int)data[6] << 16)|
                            ((unsigned int)data[7] << 24);
    if (body_len > max_body) body_len = max_body;
    unsigned char *body_src = (unsigned char*)data + 8;
    unsigned char *body_dst = (unsigned char*)out_body;
    for (unsigned int i=0; i<body_len && 8+(int)i<data_len; i++)
        body_dst[i] = body_src[i];
    return (int)body_len;
}

/* =========================================================================
 * CAPA ALTA -- API publica
 * ========================================================================= */

void net_init(void)
{
    uart_puts("[net] Iniciando driver de red...\n");

#if HW_NET_TYPE == HW_NET_TYPE_NONE
    uart_puts("[net] Modo: sin hardware (QEMU/desarrollo).\n");
    uart_puts("[net] OTA descargara actualizaciones cuando haya hardware de red.\n");
    net_state = NET_STATE_NO_HW;
#else
    uart_puts("[net] Tipo de red: ");
    #if HW_NET_TYPE == HW_NET_TYPE_WIFI
        uart_puts("WiFi 802.11\n");
    #elif HW_NET_TYPE == HW_NET_TYPE_ETH_AUTO
        uart_puts("Ethernet automotriz 100BASE-T1\n");
    #elif HW_NET_TYPE == HW_NET_TYPE_5G
        uart_puts("5G/LTE\n");
    #endif

    net_state = NET_STATE_CONNECTING;
    int r = net_hw_init();
    if (r == 0) {
        net_state = NET_STATE_UP;
        uart_puts("[net] Conectado.\n");
    } else {
        net_state = NET_STATE_DOWN;
        uart_puts("[net] Sin conexion -- reintentara en segundo plano.\n");
    }
#endif
}

net_state_t net_get_state(void) { return net_state; }

int net_available(void)
{
    return (net_state == NET_STATE_UP && net_hw_connected()) ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * net_request -- pedir datos al servidor OTA
 * -------------------------------------------------------------------------
 * Envia una peticion al servidor y retorna la respuesta en out_buf.
 * Retorna bytes recibidos o NET_ERR_*.
 * ------------------------------------------------------------------------- */
int net_request(const char *url, void *out_buf, unsigned int out_max)
{
    if (net_state == NET_STATE_NO_HW) return NET_ERR_NO_HW;
    if (!net_available())             return NET_ERR_NO_LINK;

    uart_puts("[net] Peticion: ");
    uart_puts(url);
    uart_puts("\n");

    /* Construir y enviar la peticion */
    int req_len = net_build_request(url, proto_buf, PROTO_BUF_SIZE);
    if (req_len < 0) return NET_ERR_SERVER;

    int r = net_hw_send(proto_buf, (unsigned int)req_len);
    if (r < 0) return NET_ERR_NO_LINK;

    /* Esperar y recibir la respuesta */
    int recv_len = net_hw_recv(proto_buf, PROTO_BUF_SIZE,
                                HW_NET_REQUEST_TIMEOUT);
    if (recv_len < 0) return NET_ERR_TIMEOUT;
    if (recv_len == 0) return NET_ERR_SERVER;

    /* Parsear y retornar el cuerpo */
    int body_len = net_parse_response(proto_buf, recv_len,
                                       out_buf, out_max);
    if (body_len < 0) return NET_ERR_SERVER;

    uart_puts("[net] Respuesta recibida: ");
    uart_dec((unsigned int)body_len);
    uart_puts(" bytes.\n");
    return body_len;
}

/* -------------------------------------------------------------------------
 * net_download -- descargar un archivo grande en chunks al VWFS
 * -------------------------------------------------------------------------
 * Descarga el paquete OTA en bloques de CHUNK_SIZE bytes y los escribe
 * directamente al VWFS. Sin bufferear todo en RAM.
 * ------------------------------------------------------------------------- */
int net_download(const char *url, const char *vwfs_path,
                  unsigned int expected_size,
                  net_progress_cb_t progress_cb)
{
    if (net_state == NET_STATE_NO_HW) {
        uart_puts("[net] Sin hardware de red -- descarga no disponible.\n");
        return NET_ERR_NO_HW;
    }
    if (!net_available()) return NET_ERR_NO_LINK;

    uart_puts("[net] Descargando: ");
    uart_puts(url);
    uart_puts(" -> ");
    uart_puts(vwfs_path);
    uart_puts("\n");
    uart_puts("[net] Tamano esperado: ");
    uart_dec(expected_size);
    uart_puts(" bytes\n");

    /* Abrir archivo de destino en VWFS */
    vwfs_file_t f;
    if (vwfs_open(vwfs_path, &f, 1) != 0) {
        uart_puts("[net] ERROR: no se pudo crear el archivo en VWFS.\n");
        return NET_ERR_SERVER;
    }

    unsigned int downloaded  = 0;
    unsigned int chunk_index = 0;

    while (downloaded < expected_size) {
        /* Pedir el siguiente chunk al servidor */
        char chunk_url[300];
        /* URL del chunk: url + "?offset=N&size=CHUNK_SIZE" */
        int ui = 0;
        while (url[ui] && ui < 256) { chunk_url[ui] = url[ui]; ui++; }
        /* Agregar parametros de rango */
        const char *sep = "?offset=";
        int si = 0;
        while (sep[si] && ui < 290) { chunk_url[ui++] = sep[si++]; }
        /* offset en decimal */
        char off_str[12]; int oi = 0;
        unsigned int off = downloaded;
        if (!off) { off_str[oi++] = '0'; }
        else { char tmp[12]; int ti=0;
               while(off){tmp[ti++]='0'+(off%10);off/=10;}
               for(int a=0,b=ti-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
               for(int j=0;j<ti;j++) off_str[oi++]=tmp[j]; }
        off_str[oi]='\0';
        si=0; while(off_str[si]&&ui<295) chunk_url[ui++]=off_str[si++];
        const char *size_param = "&size=4096";
        si=0; while(size_param[si]&&ui<298) chunk_url[ui++]=size_param[si++];
        chunk_url[ui]='\0';

        /* Enviar peticion del chunk */
        int req_len = net_build_request(chunk_url, proto_buf, PROTO_BUF_SIZE);
        if (req_len < 0) { vwfs_close(&f); return NET_ERR_SERVER; }

        if (net_hw_send(proto_buf, (unsigned int)req_len) < 0) {
            vwfs_close(&f); return NET_ERR_NO_LINK;
        }

        /* Recibir el chunk */
        int recv_len = net_hw_recv(proto_buf, PROTO_BUF_SIZE,
                                    HW_NET_CHUNK_TIMEOUT);
        if (recv_len <= 0) {
            uart_puts("[net] TIMEOUT recibiendo chunk ");
            uart_dec(chunk_index);
            uart_puts(".\n");
            vwfs_close(&f); return NET_ERR_TIMEOUT;
        }

        /* Extraer los datos del chunk de la respuesta */
        int chunk_data_len = net_parse_response(proto_buf, recv_len,
                                                  chunk_buf, CHUNK_SIZE);
        if (chunk_data_len <= 0) {
            vwfs_close(&f); return NET_ERR_SERVER;
        }

        /* Escribir al VWFS */
        int written = vwfs_write(&f, chunk_buf,
                                  (unsigned int)chunk_data_len);
        if (written <= 0) {
            uart_puts("[net] ERROR escribiendo al VWFS.\n");
            vwfs_close(&f); return NET_ERR_PARTIAL;
        }

        downloaded += (unsigned int)chunk_data_len;
        chunk_index++;

        /* Reportar progreso */
        if (progress_cb) progress_cb(downloaded, expected_size);

        /* Log cada 10 chunks */
        if (chunk_index % 10 == 0) {
            uart_puts("[net] Descargado: ");
            uart_dec(downloaded);
            uart_puts(" / ");
            uart_dec(expected_size);
            uart_puts(" bytes\n");
        }

        /* Si el chunk fue mas pequeno que lo esperado = fin del archivo */
        if ((unsigned int)chunk_data_len < CHUNK_SIZE) break;
    }

    vwfs_close(&f);

    if (downloaded < expected_size) {
        uart_puts("[net] ADVERTENCIA: descarga incompleta.\n");
        return NET_ERR_PARTIAL;
    }

    uart_puts("[net] Descarga completa: ");
    uart_dec(downloaded);
    uart_puts(" bytes.\n");
    return NET_OK;
}