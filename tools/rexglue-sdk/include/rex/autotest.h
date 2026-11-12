/*
 * autotest.h — devkitnx
 * Automatización de pruebas y medición de frames para ports homebrew de Switch.
 *
 * Header único. En UN solo .c/.cpp del port:
 *
 *     #define AUTOTEST_IMPLEMENTATION
 *     #include "autotest.h"
 *
 * En el resto de los archivos, solo  #include "autotest.h".
 * Para un build final sin nada de esto:  -DAUTOTEST_DISABLE
 *
 * Todo lo que imprime empieza con "[AT] " y sale por stdout, que con
 * `nxlink -s` llega a la PC. El runner (runner/nxtest.py) lee esas líneas.
 *
 * Pensado para un loop principal de un solo hilo: llamá a las funciones
 * de frame y zona siempre desde el mismo hilo. Las únicas que se pueden
 * llamar desde OTRO hilo son at_input y at_record (el que lee el pad) y
 * at_vblank (el del tick del juego).
 */
#ifndef AUTOTEST_H
#define AUTOTEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mismos valores de bit que HidNpadButton de libnx: en Switch se pueden
 * combinar directo con lo que devuelve padGetButtons(). */
enum {
    AT_BTN_A      = 1u << 0,  AT_BTN_B      = 1u << 1,
    AT_BTN_X      = 1u << 2,  AT_BTN_Y      = 1u << 3,
    AT_BTN_LSTICK = 1u << 4,  AT_BTN_RSTICK = 1u << 5,
    AT_BTN_L      = 1u << 6,  AT_BTN_R      = 1u << 7,
    AT_BTN_ZL     = 1u << 8,  AT_BTN_ZR     = 1u << 9,
    AT_BTN_PLUS   = 1u << 10, AT_BTN_MINUS  = 1u << 11,
    AT_BTN_LEFT   = 1u << 12, AT_BTN_UP     = 1u << 13,
    AT_BTN_RIGHT  = 1u << 14, AT_BTN_DOWN   = 1u << 15
};

/* Flags de at_init */
enum {
    AT_INIT_NO_SOCKETS = 1u << 0  /* el port ya inicializa sockets/nxlinkStdio */
};

#ifndef AUTOTEST_DISABLE

void        at_init(int argc, char **argv, unsigned flags);
void        at_shutdown(void);

int         at_enabled(void);          /* se pasó --autotest o --autotest=X      */
const char *at_scenario(void);         /* X de --autotest=X, "" si no hay        */
int         at_perf_enabled(void);     /* se pasó --perf                         */
const char *at_arg(const char *key);   /* "--key=valor" -> "valor"; "--key" -> ""; NULL si no está */

void        at_mark(const char *name); /* aviso "llegué acá": [AT] mark <name>   */
void        at_log(const char *fmt, ...);

void        at_frame_begin(void);
void        at_frame_end(void);
void        at_zone_begin(const char *name); /* máx. 16 zonas, nombre <= 31 chars */
void        at_zone_end(const char *name);
void        at_zone_add(const char *name, uint64_t us); /* suma un tiempo medido aparte (ej. GPU) al frame actual */

int         at_input(uint64_t *buttons); /* 1 si el replay (--at-input) pisa los botones de este frame */
uint64_t    at_record(uint64_t buttons); /* --at-record=archivo: graba los cambios; devuelve los botones para el juego */

void        at_done(int code);         /* fin de la prueba: 0 = OK                */
void        at_fail(const char *reason);
int         at_should_quit(void);      /* el loop principal debería terminar      */
uint64_t    at_frame(void);            /* frames terminados desde at_init         */
void        at_vblank(void);           /* un tick de la lógica del juego (ej. vblank emulado): reloj de --at-clock=vblank */

#else /* AUTOTEST_DISABLE: todo queda en nada */

static inline void        at_init(int c, char **v, unsigned f) { (void)c; (void)v; (void)f; }
static inline void        at_shutdown(void) {}
static inline int         at_enabled(void) { return 0; }
static inline const char *at_scenario(void) { return ""; }
static inline int         at_perf_enabled(void) { return 0; }
static inline const char *at_arg(const char *k) { (void)k; return 0; }
static inline void        at_mark(const char *n) { (void)n; }
static inline void        at_log(const char *f, ...) { (void)f; }
static inline void        at_frame_begin(void) {}
static inline void        at_frame_end(void) {}
static inline void        at_zone_begin(const char *n) { (void)n; }
static inline void        at_zone_end(const char *n) { (void)n; }
static inline void        at_zone_add(const char *n, uint64_t u) { (void)n; (void)u; }
static inline int         at_input(uint64_t *b) { (void)b; return 0; }
static inline uint64_t    at_record(uint64_t b) { return b; }
static inline void        at_done(int c) { (void)c; }
static inline void        at_fail(const char *r) { (void)r; }
static inline int         at_should_quit(void) { return 0; }
static inline uint64_t    at_frame(void) { return 0; }
static inline void        at_vblank(void) {}

#endif /* AUTOTEST_DISABLE */

#ifdef __cplusplus
} /* extern "C" */

/* C++: zona que se cierra sola al salir del bloque.  { AT_SCOPE("sprites"); ... } */
struct AtScope {
    const char *n;
    explicit AtScope(const char *name) : n(name) { at_zone_begin(n); }
    ~AtScope() { at_zone_end(n); }
};
#define AT__CAT2(a, b) a##b
#define AT__CAT(a, b)  AT__CAT2(a, b)
#define AT_SCOPE(name) AtScope AT__CAT(at__scope_, __LINE__)(name)
#endif

#endif /* AUTOTEST_H */

/* ======================================================================== */
#if defined(AUTOTEST_IMPLEMENTATION) && !defined(AUTOTEST_DISABLE) && !defined(AUTOTEST_IMPLEMENTED)
#define AUTOTEST_IMPLEMENTED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#if defined(__SWITCH__)
#include <switch.h>
static uint64_t at__now_ns(void) { return armTicksToNs(armGetSystemTick()); }
#elif defined(_WIN32)
#include <windows.h>
static uint64_t at__now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static uint64_t at__now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

#define AT__MAX_ZONES   48
#define AT__ZONE_NAME   32
#define AT__FLUSH_EVERY 32   /* frames de perf por envío */

typedef struct { char name[AT__ZONE_NAME]; uint64_t start; uint64_t acc; int open; } at__zone_t;
typedef struct { uint64_t frame; uint64_t mask; int end; } at__step_t;
typedef struct { uint64_t frame; char name[AT__ZONE_NAME]; } at__mark_t;

static struct {
    int         argc;
    char      **argv;
    int         enabled, perf, done, quit, sockets;
    const char *scenario;
    uint64_t    t0, frame_start;
    volatile uint64_t frame;          /* lo leen at_input/at_record desde otro hilo */
    uint64_t    exit_after;           /* 0 = no */
    at__zone_t  zones[AT__MAX_ZONES];
    int         nzones, header_nzones;
    char        buf[8192];
    size_t      buflen;
    unsigned    buffered_frames;
    at__step_t *steps;
    int         nsteps, step, input_end_sent;
    at__mark_t *marks;                /* "@nombre" del replay */
    int         nmarks, next_mark;
    volatile uint64_t in_mask;        /* lo que devuelve at_input: lo publica el hilo del frame */
    volatile int      in_active;
    FILE       *rec;                  /* --at-record */
    uint64_t    rec_last, rec_mark_btn;
    int         rec_started, rec_mark_down, exit_at_end;
    char        rec_mark_name[AT__ZONE_NAME];
    /* --at-clock=vblank: el replay, las marcas y la grabación cuentan ticks del juego
     * (at_vblank) en vez de swaps, así la misma entrada cae en el mismo momento del juego
     * a cualquier fps. El paso del input lo mueve el hilo del vblank; las marcas y el fin los
     * manda el hilo del frame en el swap siguiente. */
    volatile uint64_t vblank;
    int         clock_vblank;
    volatile int end_due;
    /* --at-input-latch (con --at-clock=vblank): el paso no avanza hasta que el juego leyó el
     * actual (at_input lo marca en in_seen), y avanza de a uno. A fps bajos el juego lee el pad
     * cada 2-3 vblanks y un toque de 1 vblank se perdía: el menú navegaba distinto y la pelea del
     * bench cambiaba de escena y rival según los fps (2026-11-07). */
    int          input_latch;
    volatile int in_pub;              /* paso publicado en in_mask (-1: antes del primero) */
    volatile int in_seen;             /* último paso que at_input devolvió entero */
    FILE       *conv;                 /* --at-convert: reescribe el replay con el reloj de vblanks */
    int         conv_step, conv_end;
} at__s;

static uint64_t at__clock(void) { return at__s.clock_vblank ? at__s.vblank : at__s.frame; }

static void at__mask_text(uint64_t m, char *txt, size_t cap) {
    static const char *const names[16] = {
        "A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R",
        "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    size_t n = 0;
    int i;
    txt[0] = 0;
    for (i = 0; i < 16 && n < cap; i++) {
        if (!(m & (1u << i))) continue;
        n += (size_t)snprintf(txt + n, cap - n, "%s%s", n ? "+" : "", names[i]);
    }
    if (!n) snprintf(txt, cap, "-");
}

static double at__ms(void) { return (double)(at__now_ns() - at__s.t0) / 1e6; }

static void at__flush(void) {
    if (at__s.buflen) {
        fwrite(at__s.buf, 1, at__s.buflen, stdout);
        at__s.buflen = 0;
    }
    at__s.buffered_frames = 0;
    fflush(stdout);
}

static void at__append(const char *s, size_t n) {
    if (at__s.buflen + n > sizeof(at__s.buf)) at__flush();
    if (n > sizeof(at__s.buf)) { fwrite(s, 1, n, stdout); return; }
    memcpy(at__s.buf + at__s.buflen, s, n);
    at__s.buflen += n;
}

/* Línea inmediata (marks, logs): primero lo que había en buffer, para no desordenar. */
static void at__line(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(tmp) - 2) n = (int)sizeof(tmp) - 2;
    tmp[n++] = '\n';
    at__append(tmp, (size_t)n);
    at__flush();
}

const char *at_arg(const char *key) {
    size_t kl = strlen(key);
    int i;
    for (i = 1; i < at__s.argc; i++) {
        const char *a = at__s.argv[i];
        if (!a || a[0] != '-' || a[1] != '-') continue;
        a += 2;
        if (strncmp(a, key, kl) != 0) continue;
        if (a[kl] == '\0') return a + kl;      /* "" */
        if (a[kl] == '=')  return a + kl + 1;
    }
    return NULL;
}

/* ---- replay de inputs ---------------------------------------------------- */

/* Copia un nombre cortándolo a AT__ZONE_NAME - 1 (strncpy avisa en GCC). */
static void at__copy_name(char *dst, const char *src) {
    size_t l = strlen(src);
    if (l > AT__ZONE_NAME - 1) l = AT__ZONE_NAME - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static uint64_t at__button(const char *t) {
    static const struct { const char *n; uint64_t m; } map[] = {
        {"A", AT_BTN_A}, {"B", AT_BTN_B}, {"X", AT_BTN_X}, {"Y", AT_BTN_Y},
        {"LSTICK", AT_BTN_LSTICK}, {"RSTICK", AT_BTN_RSTICK},
        {"L", AT_BTN_L}, {"R", AT_BTN_R}, {"ZL", AT_BTN_ZL}, {"ZR", AT_BTN_ZR},
        {"PLUS", AT_BTN_PLUS}, {"MINUS", AT_BTN_MINUS},
        {"LEFT", AT_BTN_LEFT}, {"UP", AT_BTN_UP}, {"RIGHT", AT_BTN_RIGHT}, {"DOWN", AT_BTN_DOWN},
    };
    size_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(t, map[i].n) == 0) return map[i].m;
    at__line("[AT] log input: boton desconocido '%s'", t);
    return 0;
}

/* Formato: una entrada por línea, "<frame> <botones>", se mantiene hasta la siguiente.
 *   0    -          (nada)
 *   60   A
 *   62   -
 *   120  DOWN+A
 *   600  END        (fin del replay: manda [AT] mark input_end)
 *   900  @match_start  (no toca botones: manda [AT] mark match_start al llegar a ese frame)
 * '#' empieza un comentario. Los frames tienen que ir en orden creciente. */
static void at__load_input(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    int cap = 0, mcap = 0, lineno = 0;
    if (!f) { at__line("[AT] fail no se pudo abrir el input '%s'", path); return; }
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *hash = strchr(line, '#');
        char btns[200] = {0};
        unsigned long long fr;
        at__step_t st;
        lineno++;
        if (hash) *hash = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '\r') continue;
        if (sscanf(p, "%llu %199s", &fr, btns) != 2) {
            at__line("[AT] log input: linea %d invalida", lineno);
            continue;
        }
        if (btns[0] == '@') {
            if (at__s.nmarks == mcap) {
                int ncap = mcap ? mcap * 2 : 16;
                at__mark_t *nm = (at__mark_t *)realloc(at__s.marks, (size_t)ncap * sizeof(*nm));
                if (!nm) break;
                at__s.marks = nm; mcap = ncap;
            }
            at__s.marks[at__s.nmarks].frame = fr;
            at__copy_name(at__s.marks[at__s.nmarks].name, btns + 1);
            at__s.nmarks++;
            continue;
        }
        st.frame = fr; st.mask = 0; st.end = 0;
        if (strcmp(btns, "END") == 0) st.end = 1;
        else if (strcmp(btns, "-") != 0) {
            char *tok = strtok(btns, "+");
            while (tok) { st.mask |= at__button(tok); tok = strtok(NULL, "+"); }
        }
        if (at__s.nsteps == cap) {
            int ncap = cap ? cap * 2 : 64;
            at__step_t *ns = (at__step_t *)realloc(at__s.steps, (size_t)ncap * sizeof(*ns));
            if (!ns) break;
            at__s.steps = ns; cap = ncap;
        }
        at__s.steps[at__s.nsteps++] = st;
    }
    fclose(f);
    at__line("[AT] log input: %d pasos y %d marcas cargados de %s", at__s.nsteps, at__s.nmarks, path);
}

/* Lo corre el hilo del frame (at_init y at_frame_end): calcula qué devuelve at_input
 * durante el frame que empieza y manda las marcas "@nombre" que tocan. Así at_input
 * solo lee dos variables y se puede llamar desde el hilo que lee el pad. */
/* Pone in_mask/in_active para el reloj clk; marca end_due al llegar al fin del replay. */
static void at__input_step(uint64_t clk) {
    at__step_t *cur;
    if (!at__s.nsteps || at__s.end_due) { at__s.in_active = 0; return; }
    if (at__s.input_latch) {
        /* De a un paso, y solo si el juego ya leyó el estado actual (in_seen). */
        if (at__s.step + 1 < at__s.nsteps && at__s.steps[at__s.step + 1].frame <= clk &&
            at__s.in_seen == at__s.step)
            at__s.step++;
    } else {
        while (at__s.step + 1 < at__s.nsteps && at__s.steps[at__s.step + 1].frame <= clk)
            at__s.step++;
    }
    cur = &at__s.steps[at__s.step];
    if (cur->frame > clk) {  /* antes del primer paso */
        at__s.in_pub = -1; at__s.in_mask = 0; at__s.in_active = 1; return;
    }
    /* Sin END (grabación cerrada con HOME): pasado el último paso, cuenta como fin. */
    if (cur->end || (at__s.step == at__s.nsteps - 1 && clk > cur->frame)) {
        at__s.in_active = 0;
        at__s.end_due = 1;
        return;
    }
    at__s.in_pub = -1;              /* mientras cambia in_mask, at_input no confirma nada */
    __sync_synchronize();
    at__s.in_mask = cur->mask;
    __sync_synchronize();
    at__s.in_pub = at__s.step;
    at__s.in_active = 1;
}

/* Hilo del frame (at_init y at_frame_end): marcas "@nombre", paso del input con el reloj de
 * swaps, y el fin del replay. */
static void at__input_advance(void) {
    uint64_t clk = at__clock();
    while (at__s.next_mark < at__s.nmarks && at__s.marks[at__s.next_mark].frame <= clk) {
        if (at__s.conv)
            fprintf(at__s.conv, "%llu @%s\n", (unsigned long long)at__s.vblank,
                    at__s.marks[at__s.next_mark].name);
        at_mark(at__s.marks[at__s.next_mark++].name);
    }
    if (!at__s.clock_vblank) {
        at__input_step(clk);
        if (at__s.conv && !at__s.end_due && at__s.step != at__s.conv_step &&
            at__s.steps[at__s.step].frame <= clk) {
            char txt[160];
            at__s.conv_step = at__s.step;
            at__mask_text(at__s.steps[at__s.step].mask, txt, sizeof(txt));
            fprintf(at__s.conv, "%llu %s\n", (unsigned long long)at__s.vblank, txt);
        }
    }
    if (at__s.end_due && !at__s.input_end_sent) {
        if (at__s.conv && !at__s.conv_end) {
            at__s.conv_end = 1;
            fprintf(at__s.conv, "%llu END\n", (unsigned long long)at__s.vblank);
            fclose(at__s.conv);
            at__s.conv = NULL;
        }
        at__s.input_end_sent = 1;
        at_mark("input_end");
        if (at__s.exit_at_end) at_done(0);   /* --at-exit-at-end */
    }
}

/* Un tick de la lógica del juego; se puede llamar desde su propio hilo. */
void at_vblank(void) {
    at__s.vblank = at__s.vblank + 1;
    if (at__s.clock_vblank) at__input_step(at__s.vblank);
}

int at_input(uint64_t *buttons) {
    if (!at__s.in_active) return 0;
    {
        const int pub = at__s.in_pub;
        __sync_synchronize();
        *buttons = at__s.in_mask;
        __sync_synchronize();
        if (pub >= 0 && pub == at__s.in_pub) at__s.in_seen = pub;
    }
    return 1;
}

/* Graba en el mismo formato que lee --at-input, una línea por cambio. Solo los
 * 16 botones de la tabla; el stick izquierdo (los bits 16-19 de HidNpadButton)
 * queda como cruceta, el derecho y la posición exacta del stick no quedan.
 * --at-record-mark=BOTON:nombre: apretar BOTON escribe "<frame> @nombre" en vez
 * de grabarse, y no le llega al juego (el replay tampoco lo aprieta).
 * Devuelve los botones que tiene que ver el juego. Al cerrar escribe "<frame> END". */
uint64_t at_record(uint64_t buttons) {
    char txt[160];
    const uint64_t game = buttons & ~at__s.rec_mark_btn;
    if (!at__s.rec) return buttons;
    if (buttons & (1u << 16)) buttons |= AT_BTN_LEFT;   /* StickLLeft  */
    if (buttons & (1u << 17)) buttons |= AT_BTN_UP;     /* StickLUp    */
    if (buttons & (1u << 18)) buttons |= AT_BTN_RIGHT;  /* StickLRight */
    if (buttons & (1u << 19)) buttons |= AT_BTN_DOWN;   /* StickLDown  */
    buttons &= 0xFFFFu;
    if (at__s.rec_mark_btn) {
        int down = (buttons & at__s.rec_mark_btn) != 0;
        if (down && !at__s.rec_mark_down) {
            fprintf(at__s.rec, "%llu @%s\n", (unsigned long long)at__clock(), at__s.rec_mark_name);
            fflush(at__s.rec);
        }
        at__s.rec_mark_down = down;
        buttons &= ~at__s.rec_mark_btn;
    }
    if (at__s.rec_started && buttons == at__s.rec_last) return game;
    at__s.rec_started = 1;
    at__s.rec_last = buttons;
    at__mask_text(buttons, txt, sizeof(txt));
    fprintf(at__s.rec, "%llu %s\n", (unsigned long long)at__clock(), txt);
    fflush(at__s.rec);
    return game;
}

/* ---- ciclo de vida ------------------------------------------------------- */

void at_init(int argc, char **argv, unsigned flags) {
    const char *v;
    memset(&at__s, 0, sizeof(at__s));
    at__s.argc = argc;
    at__s.argv = argv;
    at__s.t0 = at__now_ns();
    at__s.scenario = "";

#if defined(__SWITCH__)
    if (!(flags & AT_INIT_NO_SOCKETS)) {
        if (R_SUCCEEDED(socketInitializeDefault())) {
            at__s.sockets = 1;
            nxlinkStdio(); /* si no vino por nxlink, falla y stdout no va a ningún lado */
        }
    }
#else
    (void)flags;
#endif

    v = at_arg("autotest");
    if (v) { at__s.enabled = 1; at__s.scenario = v; }
    at__s.perf = at_arg("perf") != NULL;
    v = at_arg("at-exit-after");
    if (v && *v) at__s.exit_after = strtoull(v, NULL, 10);
    v = at_arg("at-input");
    if (v && *v) at__load_input(v);
    at__s.exit_at_end = at_arg("at-exit-at-end") != NULL;
    v = at_arg("at-clock");
    at__s.clock_vblank = v && strcmp(v, "vblank") == 0;
    at__s.input_latch = at__s.clock_vblank && at_arg("at-input-latch") != NULL;
    at__s.in_pub = at__s.in_seen = -1;
    if (at__s.input_latch) at__line("[AT] log input: latch (cada paso espera a que el juego lo lea)");
    at__s.conv_step = -1;
    v = at_arg("at-convert");
    if (v && *v && !at__s.clock_vblank) {
        at__s.conv = fopen(v, "w");
        if (at__s.conv) fprintf(at__s.conv, "# convertido con --at-convert (vblank botones)\n");
        else at__line("[AT] log convert: no se pudo crear '%s'", v);
    }
    v = at_arg("at-record");
    if (v && *v) {
        at__s.rec = fopen(v, "w");
        if (at__s.rec) fprintf(at__s.rec, "# grabado con --at-record (frame botones)\n");
        else at__line("[AT] log record: no se pudo crear '%s'", v);
    }
    v = at_arg("at-record-mark");          /* BOTON:nombre */
    if (v && *v) {
        char btn[16] = {0};
        const char *colon = strchr(v, ':');
        size_t bl = colon ? (size_t)(colon - v) : strlen(v);
        if (bl >= sizeof(btn)) bl = sizeof(btn) - 1;
        memcpy(btn, v, bl);
        at__s.rec_mark_btn = at__button(btn);
        at__copy_name(at__s.rec_mark_name, colon ? colon + 1 : "mark");
    }

    at__line("[AT] hello v=2 autotest=%d scenario=%s perf=%d", at__s.enabled, at__s.scenario, at__s.perf);
    at_mark("boot");
    if (at__s.clock_vblank) at__input_step(0);
    at__input_advance();
}

void at_shutdown(void) {
    at__line("[AT] exit frames=%llu t=%.1f", (unsigned long long)at__s.frame, at__ms());
    at__s.in_active = 0;
    if (at__s.rec) {
        fprintf(at__s.rec, "%llu END\n", (unsigned long long)at__clock());
        fclose(at__s.rec);
        at__s.rec = NULL;
    }
    free(at__s.steps);
    at__s.steps = NULL;
    at__s.nsteps = 0;
    free(at__s.marks);
    at__s.marks = NULL;
    at__s.nmarks = 0;
#if defined(__SWITCH__)
    if (at__s.sockets) { socketExit(); at__s.sockets = 0; }
#endif
}

int         at_enabled(void)      { return at__s.enabled; }
const char *at_scenario(void)     { return at__s.scenario; }
int         at_perf_enabled(void) { return at__s.perf; }
int         at_should_quit(void)  { return at__s.quit; }
uint64_t    at_frame(void)        { return at__s.frame; }

#if defined(__SWITCH__)
/* Real clock of one pcv module in MHz (clkrst), 0 if it cannot be read. */
static double at__clock_mhz(PcvModuleId id) {
    ClkrstSession ses;
    u32 hz = 0;
    if (R_SUCCEEDED(clkrstOpenSession(&ses, id, 3))) {
        if (R_FAILED(clkrstGetClockRate(&ses, &hz))) hz = 0;
        clkrstCloseSession(&ses);
    }
    return hz / 1e6;
}
#endif

void at_mark(const char *name) {
    at__line("[AT] mark %s t=%.1f frame=%llu vb=%llu", name, at__ms(), (unsigned long long)at__s.frame,
             (unsigned long long)at__s.vblank);
#if defined(__SWITCH__)
    /* The bench numbers depend on the console clocks (sys-clk profiles, charger): log them with
     * every mark so each run says which clocks it ran at. */
    if (R_SUCCEEDED(clkrstInitialize())) {
        at__line("[AT] clocks %s cpu=%.1f gpu=%.1f emc=%.1f", name, at__clock_mhz(PcvModuleId_CpuBus),
                 at__clock_mhz(PcvModuleId_GPU), at__clock_mhz(PcvModuleId_EMC));
        clkrstExit();
    } else {
        at__line("[AT] clocks %s unavailable", name);
    }
#endif
}

void at_log(const char *fmt, ...) {
    char tmp[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    at__line("[AT] log %s", tmp);
}

void at_done(int code) {
    if (at__s.done) return;
    at__s.done = 1;
    at__s.quit = 1;
    at__line("[AT] done code=%d frames=%llu t=%.1f", code, (unsigned long long)at__s.frame, at__ms());
}

void at_fail(const char *reason) {
    at__line("[AT] fail %s", reason ? reason : "");
    at_done(1);
}

/* ---- frames y zonas ------------------------------------------------------ */

static int at__zone_id(const char *name) {
    int i;
    for (i = 0; i < at__s.nzones; i++)
        if (strcmp(at__s.zones[i].name, name) == 0) return i;
    if (at__s.nzones == AT__MAX_ZONES) return -1;
    strncpy(at__s.zones[i].name, name, AT__ZONE_NAME - 1);
    at__s.zones[i].name[AT__ZONE_NAME - 1] = '\0';
    return at__s.nzones++;
}

void at_zone_begin(const char *name) {
    int id;
    if (!at__s.perf) return;
    id = at__zone_id(name);
    if (id < 0) return;
    at__s.zones[id].start = at__now_ns();
    at__s.zones[id].open = 1;
}

void at_zone_end(const char *name) {
    int id;
    if (!at__s.perf) return;
    id = at__zone_id(name);
    if (id < 0 || !at__s.zones[id].open) return;
    at__s.zones[id].acc += at__now_ns() - at__s.zones[id].start;
    at__s.zones[id].open = 0;
}

void at_zone_add(const char *name, uint64_t us) {
    int id;
    if (!at__s.perf) return;
    id = at__zone_id(name);
    if (id < 0) return;
    at__s.zones[id].acc += us * 1000u;
}

void at_frame_begin(void) {
    at__s.frame_start = at__now_ns();
}

void at_frame_end(void) {
    if (at__s.perf && at__s.frame_start) {
        uint64_t total = at__now_ns() - at__s.frame_start;
        char row[64 + AT__MAX_ZONES * 24]; /* 24 = ",%llu" con el peor caso de dígitos */
        int n, i;
        if (at__s.nzones != at__s.header_nzones) {
            char hdr[64 + AT__MAX_ZONES * AT__ZONE_NAME];
            int h = snprintf(hdr, sizeof(hdr), "[AT] zones,frame,total_us");
            for (i = 0; i < at__s.nzones; i++)
                h += snprintf(hdr + h, sizeof(hdr) - (size_t)h, ",%s", at__s.zones[i].name);
            hdr[h++] = '\n';
            at__append(hdr, (size_t)h);
            at__s.header_nzones = at__s.nzones;
        }
        n = snprintf(row, sizeof(row), "[AT] perf,%llu,%llu",
                     (unsigned long long)at__s.frame, (unsigned long long)(total / 1000));
        for (i = 0; i < at__s.nzones; i++) {
            n += snprintf(row + n, sizeof(row) - (size_t)n, ",%llu",
                          (unsigned long long)(at__s.zones[i].acc / 1000));
            at__s.zones[i].acc = 0;
        }
        row[n++] = '\n';
        at__append(row, (size_t)n);
        if (++at__s.buffered_frames >= AT__FLUSH_EVERY) at__flush();
    }
    at__s.frame = at__s.frame + 1;   /* volatile: sin ++ (deprecado en C++20) */
    at__input_advance();
    if (at__s.exit_after && at__s.frame >= at__s.exit_after) at_done(0);
}

#endif /* AUTOTEST_IMPLEMENTATION */
