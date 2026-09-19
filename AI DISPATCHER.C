#include "ai_dispatcher.h"
#include "vehicle_bridge.h"
#include "uart.h"

static inline void scopy(char *d, const char *s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]='\0';
}
static inline void scat(char *d, const char *s, int max) {
    int i=0; while(d[i]) i++;
    int j=0; while(s[j]&&i<max-1){d[i]=s[j];i++;j++;} d[i]='\0';
}
static inline int seq(const char *a, const char *b) {
    int i=0; while(a[i]&&b[i]){if(a[i]!=b[i])return 0;i++;} return a[i]==b[i];
}
static void uint_to_str(unsigned int v, char *buf, int max) {
    char tmp[12]; int ti=0;
    if(!v){tmp[ti++]='0';}else{while(v){tmp[ti++]='0'+(v%10);v/=10;}}
    for(int a=0,b=ti-1;a<b;a++,b--){char t=tmp[a];tmp[a]=tmp[b];tmp[b]=t;}
    int i=0; while(i<ti&&i<max-1){buf[i]=tmp[i];i++;} buf[i]='\0';
}

#define MAX_HANDLERS 64
typedef struct { char name[64]; cmd_handler_t handler; } cmd_entry_t;
static cmd_entry_t handlers[MAX_HANDLERS];
static int handler_count = 0;

static int cmd_query_battery(const char *a, char *out, int max) {
    (void)a;
    const vb_data_t *v = vb_get_data();
    if(!v||!v->valid){scopy(out,"Sin datos del vehiculo.",max);return 0;}
    char buf[256]; char tmp[12];
    scopy(buf,"Bateria al ",sizeof(buf));
    uint_to_str(v->battery_pct,tmp,sizeof(tmp));
    scat(buf,tmp,sizeof(buf));
    scat(buf," por ciento. Autonomia de ",sizeof(buf));
    uint_to_str(v->range_km,tmp,sizeof(tmp));
    scat(buf,tmp,sizeof(buf));
    scat(buf," kilometros.",sizeof(buf));
    scopy(out,buf,max); return 1;
}
static int cmd_query_speed(const char *a, char *out, int max) {
    (void)a;
    const vb_data_t *v = vb_get_data();
    if(!v||!v->valid){scopy(out,"Sin datos.",max);return 0;}
    char buf[128]; char tmp[12];
    scopy(buf,"Velocidad actual: ",sizeof(buf));
    uint_to_str(v->speed_kmh,tmp,sizeof(tmp));
    scat(buf,tmp,sizeof(buf));
    scat(buf," kilometros por hora.",sizeof(buf));
    scopy(out,buf,max); return 1;
}
static int cmd_query_alerts(const char *a, char *out, int max) {
    (void)a;
    const vb_data_t *v = vb_get_data();
    if(!v){scopy(out,"Sin datos.",max);return 0;}
    if(v->alerts==VB_ALERT_NONE) scopy(out,"No hay alertas activas.",max);
    else if(v->alerts&VB_ALERT_BATTERY_LOW) scopy(out,"Alerta: bateria baja.",max);
    else if(v->alerts&VB_ALERT_SEATBELT) scopy(out,"Alerta: cinturon desabrochado.",max);
    else scopy(out,"Hay alertas activas.",max);
    return 1;
}
static int cmd_set_climate(const char *a, char *out, int max) {
    int temp=22;
    if(a&&a[0]){temp=0;for(int i=0;a[i]>='0'&&a[i]<='9';i++)temp=temp*10+(a[i]-'0');}
    char buf[64]; char tmp[8];
    scopy(buf,"Climatizador ajustado a ",sizeof(buf));
    uint_to_str((unsigned int)temp,tmp,sizeof(tmp));
    scat(buf,tmp,sizeof(buf));
    scat(buf," grados.",sizeof(buf));
    scopy(out,buf,max); return 1;
}
static int cmd_media_play(const char *a, char *out, int max) {
    (void)a; scopy(out,"Reproduciendo musica.",max); return 1;
}
static int cmd_nav_home(const char *a, char *out, int max) {
    (void)a; scopy(out,"Iniciando navegacion a casa.",max); return 1;
}

void dispatcher_init(void) {
    uart_puts("[dispatcher] Registrando comandos del OS...\n");
    handler_count=0;
    dispatcher_register("query_battery", cmd_query_battery);
    dispatcher_register("query_speed",   cmd_query_speed);
    dispatcher_register("query_alerts",  cmd_query_alerts);
    dispatcher_register("set_climate",   cmd_set_climate);
    dispatcher_register("media_play",    cmd_media_play);
    dispatcher_register("nav_home",      cmd_nav_home);
    uart_puts("[dispatcher] Comandos registrados.\n");
}

void dispatcher_register(const char *cmd, cmd_handler_t h) {
    if(handler_count>=MAX_HANDLERS) return;
    scopy(handlers[handler_count].name,cmd,64);
    handlers[handler_count].handler=h;
    handler_count++;
}

int dispatcher_process(const char *resp, dispatch_result_t *r) {
    r->command[0]='\0'; r->args[0]='\0';
    r->success=0; r->clean_text[0]='\0';

    /* Buscar [CMD]...[/CMD] */
    const char *cs=0, *ce=0;
    for(int i=0;resp[i];i++) {
        if(resp[i]=='['&&resp[i+1]=='C'&&resp[i+2]=='M'&&resp[i+3]=='D'&&resp[i+4]==']')
            cs=&resp[i+5];
        if(resp[i]=='['&&resp[i+1]=='/'&&resp[i+2]=='C'&&resp[i+3]=='M'&&resp[i+4]=='D'&&resp[i+5]==']')
            ce=&resp[i];
    }

    if(cs&&ce&&ce>cs) {
        char cmd_full[256]; int ci=0;
        while(*cs==' ') cs++;
        while(cs<ce&&ci<255){cmd_full[ci]=*cs;ci++;cs++;} cmd_full[ci]='\0';
        while(ci>0&&cmd_full[ci-1]==' ') {ci--;cmd_full[ci]='\0';}
        int si=0;
        while(cmd_full[si]&&cmd_full[si]!=' '&&si<63){r->command[si]=cmd_full[si];si++;}
        r->command[si]='\0';
        if(cmd_full[si]==' ') scopy(r->args,&cmd_full[si+1],256);
        for(int h=0;h<handler_count;h++) {
            if(seq(handlers[h].name,r->command)) {
                char hr[512]; r->success=handlers[h].handler(r->args,hr,512);
                scopy(r->clean_text,hr,1024); return r->success;
            }
        }
        scopy(r->clean_text,"Comando no reconocido.",1024);
    } else {
        /* Sin comando -- extraer texto despues de [AST] */
        const char *pos=resp;
        for(int i=0;resp[i];i++) {
            if(resp[i]=='['&&resp[i+1]=='A'&&resp[i+2]=='S'&&resp[i+3]=='T'&&resp[i+4]==']') {
                pos=&resp[i+5]; while(*pos==' ') pos++; break;
            }
        }
        int ci=0;
        while(*pos&&*pos!='['&&ci<1023){r->clean_text[ci]=*pos;ci++;pos++;}
        while(ci>0&&r->clean_text[ci-1]==' ') ci--;
        r->clean_text[ci]='\0';
        if(ci==0) scopy(r->clean_text,resp,1024);
    }
    return r->success;
}