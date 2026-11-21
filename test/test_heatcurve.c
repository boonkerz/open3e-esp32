/* The heating curve: the formula against the manufacturer's app and against
 * the plant's own setpoint, and the picture's structure.
 *
 * The numbers are from a Vitocal 250-A with underfloor heating: the app's
 * chart for gradient 0.2 / level 0 at a room setpoint of 23 °C, and the flow
 * setpoint the controller reported at 4.2 °C outside.
 *
 * With a directory as argument the pictures are written there, to be looked
 * at -- whether a drawing is right is not something strstr can tell.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/heat_curve.h"

static int fail;

static void ok(const char *what, int cond)
{
    if (!cond) {
        fail++;
        printf("  FAIL %s\n", what);
    }
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) {
        n++;
    }
    return n;
}

static void save(const char *dir, const char *name, const char *svg)
{
    if (!dir || !svg) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(svg, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;

    /* --- the app's six labels --- */
    static const struct { double out; int flow; } app[] = {
        { 20, 24 }, { 10, 26 }, { 0, 28 }, { -10, 30 }, { -20, 32 }, { -30, 34 },
    };
    for (size_t i = 0; i < sizeof(app) / sizeof(app[0]); i++) {
        double f = heat_curve_flow(0.2, 0, 23, app[i].out);
        char what[64];
        snprintf(what, sizeof(what), "app label at %+.0f: %.2f vs %d", app[i].out, f, app[i].flow);
        ok(what, (int)lround(f) == app[i].flow);
    }
    /* --- the controller's own setpoint --- */
    ok("plant setpoint at 4.2 outside", fabs(heat_curve_flow(0.2, 0, 23, 4.2) - 27.2) < 0.1);
    /* --- what the two settings do --- */
    ok("level shifts the line",
       fabs(heat_curve_flow(0.2, 5, 23, 0) - heat_curve_flow(0.2, 0, 23, 0) - 5) < 1e-9);
    ok("gradient steepens it",
       heat_curve_flow(1.4, 0, 20, -10) > heat_curve_flow(0.6, 0, 20, -10));
    ok("at room temperature outside the flow is the room setpoint plus level",
       fabs(heat_curve_flow(1.4, 2, 20, 20) - 22) < 1e-9);

    /* --- the picture --- */
    heat_curve_t c = {
        .gradient = 0.2, .level = 0, .room = 23,
        .have_limits = true, .flow_min = 20, .flow_max = 40,
        .have_outside = true, .outside = 4.2, .have_flow = true, .flow = 27.2,
        .name = "Heiz-K\xc3\xbchlkreis 1",
    };
    char *svg = heat_curve_svg(&c);
    ok("a picture comes out", svg != NULL);
    if (svg) {
        ok("it is an SVG document", strncmp(svg, "<svg", 4) == 0
                                    && strcmp(svg + strlen(svg) - 6, "</svg>") == 0);
        ok("the six points are labelled like the app",
           strstr(svg, ">24\xc2\xb0<") && strstr(svg, ">26\xc2\xb0<") && strstr(svg, ">28\xc2\xb0<")
           && strstr(svg, ">30\xc2\xb0<") && strstr(svg, ">32\xc2\xb0<") && strstr(svg, ">34\xc2\xb0<"));
        ok("settings in the heading, with decimal comma",
           strstr(svg, "Neigung 0,2 ") && strstr(svg, "Niveau 0 K") && strstr(svg, "Raum-Soll 23 "));
        ok("the circuit is named", strstr(svg, "Heizkennlinie Heiz-K\xc3\xbchlkreis 1<") != NULL);
        ok("the operating point is the plant's", strstr(svg, "Vorlauf 27,2 ")
                                                 && strstr(svg, "Sollwert der Anlage"));
        ok("six labelled points and the operating point", count(svg, "<circle") == 7);
        ok("limits outside the view are not drawn", count(svg, "class=\"lim\"") == 0);
        save(dir, "curve_floor.svg", svg);
    }
    free(svg);

    /* Radiators on a cold night: steep, clipped at the limit, the plant's
     * setpoint unknown, a name that needs escaping. */
    heat_curve_t r = {
        .gradient = 1.4, .level = 3, .room = 16,
        .have_limits = true, .flow_min = 20, .flow_max = 55,
        .have_outside = true, .outside = -12.5,
        .name = "Altbau & <Keller>",
    };
    svg = heat_curve_svg(&r);
    ok("second picture", svg != NULL);
    if (svg) {
        ok("the name is escaped", strstr(svg, "Altbau &amp; &lt;Keller>") != NULL);
        ok("the line stops at the limit", strstr(svg, ">55\xc2\xb0<") != NULL);
        ok("the limit in view is drawn", count(svg, "class=\"lim\"") >= 1);
        ok("the operating point is computed", strstr(svg, "jetzt, berechnet") != NULL);
        ok("no point is labelled above the limit",
           strstr(svg, "text-anchor=\"end\">55\xc2\xb0<") != NULL);
        save(dir, "curve_radiator.svg", svg);
    }
    free(svg);

    /* Nothing known about the present: just the line. */
    heat_curve_t bare = { .gradient = 0.6, .level = 0, .room = 20 };
    svg = heat_curve_svg(&bare);
    ok("bare picture", svg && count(svg, "<circle") == 6 && !strstr(svg, "jetzt"));
    save(dir, "curve_bare.svg", svg);
    free(svg);

    puts(fail ? "FAILED" : "all checks passed");
    return fail ? 1 : 0;
}
