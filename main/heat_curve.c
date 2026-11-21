#include "heat_curve.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o3e_json.h"

/* Same palette as the plant schematic, which carries its own background for
 * the same reason: an image entity is drawn as an <img>, where neither Home
 * Assistant's theme nor its text colour reaches. */
#define C_BG    "#0b1020"
#define C_TEXT  "#e6edf3"
#define C_DIM   "#9198a1"
#define C_GRID  "#263043"
#define C_LINE  "#ef4444"
#define C_NOW   "#f97316"

#define W   640
#define H   380
#define X0  64.0
#define X1  600.0
#define Y0  84.0
#define Y1  320.0

double heat_curve_flow(double gradient, double level, double room, double outside)
{
    double d = outside - room;
    return room + level - gradient * d * (1.4347 + 0.021 * d + 247.9e-6 * d * d);
}

static double clip(const heat_curve_t *c, double flow)
{
    if (c->have_limits) {
        if (flow < c->flow_min) {
            flow = c->flow_min;
        }
        if (flow > c->flow_max) {
            flow = c->flow_max;
        }
    }
    return flow;
}

/* Formatted append. The style block is longer than any sensible stack
 * buffer, and a silently truncated element is a document no viewer opens, so
 * the length is measured and the rare long piece goes through the heap. */
static void addf(o3e_buf_t *b, const char *fmt, ...)
{
    char t[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->oom = true;
        return;
    }
    if ((size_t)n < sizeof(t)) {
        o3e_buf_add(b, t, (size_t)n);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big) {
        b->oom = true;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    o3e_buf_add(b, big, (size_t)n);
    free(big);
}

/* A number the way the rest of the interface writes it: decimal comma, and
 * no "-0". Decimals are dropped when the value has none. */
static const char *num(char *out, size_t out_sz, double x, int decimals)
{
    if (decimals > 0 && fabs(x - round(x)) < 0.05) {
        decimals = 0;
    }
    snprintf(out, out_sz, "%.*f", decimals, x);
    if (strcmp(out, "-0") == 0) {
        snprintf(out, out_sz, "0");
    }
    for (char *p = out; *p; p++) {
        if (*p == '.') {
            *p = ',';
        }
    }
    return out;
}

static void add_text(o3e_buf_t *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '<') {
            o3e_buf_adds(b, "&lt;");
        } else if (*s == '&') {
            o3e_buf_adds(b, "&amp;");
        } else {
            o3e_buf_addc(b, *s);
        }
    }
}

static double px(double outside)
{
    return X0 + (HEAT_CURVE_OUT_WARM - outside)
                / (HEAT_CURVE_OUT_WARM - HEAT_CURVE_OUT_COLD) * (X1 - X0);
}

char *heat_curve_svg(const heat_curve_t *c)
{
    char n1[24], n2[24], n3[24];

    /* Where the plant is now. Its own setpoint if it gave one: at night, or
     * with a limit in the way, that is the truth and the formula is not. */
    bool now = c->have_outside;
    double out_now = c->outside;
    if (out_now > HEAT_CURVE_OUT_WARM) {
        out_now = HEAT_CURVE_OUT_WARM;
    }
    if (out_now < HEAT_CURVE_OUT_COLD) {
        out_now = HEAT_CURVE_OUT_COLD;
    }
    double flow_now = c->have_flow
        ? c->flow
        : clip(c, heat_curve_flow(c->gradient, c->level, c->room, c->outside));

    /* Vertical range: what the line covers, rounded out to whole fives, and
     * never so narrow that a flat line looks like a steep one. */
    double lo = 1e9, hi = -1e9;
    for (double t = HEAT_CURVE_OUT_COLD; t <= HEAT_CURVE_OUT_WARM + 0.01; t += 1.0) {
        double f = clip(c, heat_curve_flow(c->gradient, c->level, c->room, t));
        lo = f < lo ? f : lo;
        hi = f > hi ? f : hi;
    }
    if (now) {
        lo = flow_now < lo ? flow_now : lo;
        hi = flow_now > hi ? flow_now : hi;
    }
    double ymin = floor((lo - 3.0) / 5.0) * 5.0;
    double ymax = ceil((hi + 3.0) / 5.0) * 5.0;
    if (ymax - ymin < 20.0) {
        ymax = ymin + 20.0;
    }
    double step = (ymax - ymin) > 45.0 ? 10.0 : 5.0;
#define PY(f) (Y1 - ((f) - ymin) / (ymax - ymin) * (Y1 - Y0))

    o3e_buf_t b;
    o3e_buf_init(&b);
    addf(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" "
             "role=\"img\" aria-label=\"Heizkennlinie\"><style>"
             "text{font:13px Arial,Helvetica,sans-serif;fill:" C_TEXT "}"
             "text.h{font-size:16px;font-weight:600}"
             "text.lbl{font-size:11px;fill:" C_DIM "}"
             "text.now{fill:" C_NOW ";font-weight:600}"
             ".grid{stroke:" C_GRID ";stroke-width:1}"
             ".lim{stroke:" C_DIM ";stroke-width:1;stroke-dasharray:6 5}"
             "</style><rect width=\"%d\" height=\"%d\" fill=\"" C_BG "\"/>",
         W, H, W, H);

    /* Heading: what is drawn, and for which settings. */
    o3e_buf_adds(&b, "<text x=\"24\" y=\"30\" class=\"h\">Heizkennlinie");
    if (c->name && c->name[0]) {
        o3e_buf_adds(&b, " ");
        add_text(&b, c->name);
    }
    o3e_buf_adds(&b, "</text>");
    addf(&b, "<text x=\"24\" y=\"50\" class=\"lbl\">Neigung %s \xc2\xb7 Niveau %s K "
             "\xc2\xb7 Raum-Soll %s \xc2\xb0""C</text>",
         num(n1, sizeof(n1), c->gradient, 1), num(n2, sizeof(n2), c->level, 1),
         num(n3, sizeof(n3), c->room, 1));
    if (now) {
        addf(&b, "<text x=\"%d\" y=\"30\" class=\"now\" text-anchor=\"end\">"
                 "au\xc3\x9f""en %s \xc2\xb0""C \xe2\x86\x92 Vorlauf %s \xc2\xb0""C</text>"
                 "<text x=\"%d\" y=\"50\" class=\"lbl\" text-anchor=\"end\">%s</text>",
             W - 24, num(n1, sizeof(n1), c->outside, 1),
             num(n2, sizeof(n2), flow_now, 1), W - 24,
             c->have_flow ? "jetzt, Sollwert der Anlage" : "jetzt, berechnet");
    }

    /* Grid and scales. */
    for (double f = ymin; f <= ymax + 0.01; f += step) {
        addf(&b, "<line class=\"grid\" x1=\"%.0f\" y1=\"%.1f\" x2=\"%.0f\" y2=\"%.1f\"/>"
                 "<text x=\"%.0f\" y=\"%.1f\" class=\"lbl\" text-anchor=\"end\">%.0f\xc2\xb0</text>",
             X0, PY(f), X1, PY(f), X0 - 8, PY(f) + 4, f);
    }
    for (double t = HEAT_CURVE_OUT_WARM; t >= HEAT_CURVE_OUT_COLD - 0.01; t -= 10.0) {
        addf(&b, "<line class=\"grid\" stroke-dasharray=\"4 5\" x1=\"%.1f\" y1=\"%.0f\" "
                 "x2=\"%.1f\" y2=\"%.0f\"/>"
                 "<text x=\"%.1f\" y=\"%.0f\" class=\"lbl\" text-anchor=\"middle\">%s%.0f\xc2\xb0</text>",
             px(t), Y0, px(t), Y1, px(t), Y1 + 18, t > 0 ? "+" : "", t);
    }
    addf(&b, "<text x=\"%.0f\" y=\"%.0f\" class=\"lbl\" text-anchor=\"middle\">"
             "Au\xc3\x9f""entemperatur</text>"
             "<text x=\"%.0f\" y=\"%.0f\" class=\"lbl\" text-anchor=\"end\">Vorlauf</text>",
         (X0 + X1) / 2, Y1 + 40, X0 - 8, Y0 - 14);

    /* The circuit's limits, where they are in view. */
    if (c->have_limits) {
        const double lim[2] = { c->flow_min, c->flow_max };
        const char *word[2] = { "min", "max" };
        for (int i = 0; i < 2; i++) {
            if (lim[i] <= ymin || lim[i] >= ymax) {
                continue;
            }
            addf(&b, "<line class=\"lim\" x1=\"%.0f\" y1=\"%.1f\" x2=\"%.0f\" y2=\"%.1f\"/>"
                     "<text x=\"%.0f\" y=\"%.1f\" class=\"lbl\">%s</text>",
                 X0, PY(lim[i]), X1, PY(lim[i]), X1 + 6, PY(lim[i]) + 4, word[i]);
        }
    }

    /* The line, with the area under it. It bends, so it is sampled rather
     * than drawn through the six labelled points. */
    o3e_buf_t pts;
    o3e_buf_init(&pts);
    for (double t = HEAT_CURVE_OUT_WARM; t >= HEAT_CURVE_OUT_COLD - 0.01; t -= 1.0) {
        double f = clip(c, heat_curve_flow(c->gradient, c->level, c->room, t));
        addf(&pts, "%.1f,%.1f ", px(t), PY(f));
    }
    if (!pts.oom && pts.buf) {
        addf(&b, "<polygon fill=\"" C_LINE "\" fill-opacity=\"0.14\" points=\"%.0f,%.0f ",
             X0, Y1);
        o3e_buf_adds(&b, pts.buf);
        addf(&b, "%.0f,%.0f\"/><polyline fill=\"none\" stroke=\"" C_LINE "\" "
                 "stroke-width=\"2.5\" stroke-linejoin=\"round\" points=\"", X1, Y1);
        o3e_buf_adds(&b, pts.buf);
        o3e_buf_adds(&b, "\"/>");
    }
    o3e_buf_free(&pts);

    /* Where the plant is now, behind the labelled points so their numbers
     * stay readable when it sits on one. */
    if (now) {
        addf(&b, "<line stroke=\"" C_NOW "\" stroke-width=\"1.5\" stroke-dasharray=\"3 4\" "
                 "x1=\"%.1f\" y1=\"%.0f\" x2=\"%.1f\" y2=\"%.0f\"/>"
                 "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"7\" fill=\"" C_NOW "\"/>",
             px(out_now), Y0, px(out_now), Y1, px(out_now), PY(flow_now));
    }

    /* The six points the app labels. */
    for (double t = HEAT_CURVE_OUT_WARM; t >= HEAT_CURVE_OUT_COLD - 0.01; t -= 10.0) {
        double f = clip(c, heat_curve_flow(c->gradient, c->level, c->room, t));
        const char *anchor = t >= HEAT_CURVE_OUT_WARM - 0.01 ? "start"
                           : t <= HEAT_CURVE_OUT_COLD + 0.01 ? "end" : "middle";
        addf(&b, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"4\" fill=\"" C_BG "\" stroke=\"" C_LINE "\" "
                 "stroke-width=\"2\"/>"
                 "<text x=\"%.1f\" y=\"%.1f\" text-anchor=\"%s\">%.0f\xc2\xb0</text>",
             px(t), PY(f), px(t), PY(f) - 11, anchor, f);
    }
#undef PY

    o3e_buf_adds(&b, "</svg>");
    if (b.oom || !b.buf) {
        o3e_buf_free(&b);
        return NULL;
    }
    return b.buf;   /* the caller's now */
}
