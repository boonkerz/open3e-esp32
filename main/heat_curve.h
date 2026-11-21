/* The heating curve of a circuit, as a number and as a picture.
 *
 * Home Assistant gets two sliders for a heating curve -- gradient and level --
 * and neither means anything until one sees the line they describe. The
 * manufacturer's app draws that line; discovery cannot ship a chart, but it
 * can ship an image entity, and the plant schematic already goes that way.
 *
 * The formula is the one Viessmann publishes for its weather-compensated
 * controllers. It was checked against the app (gradient 0.2, level 0, room
 * 23 °C: 24/28/34 °C at +20/0/-30 °C outside) and against the plant itself,
 * whose own flow setpoint of 27.2 °C at 4.2 °C outside it gives as 27.24.
 *
 * No FreeRTOS, no bus, no MQTT: numbers in, SVG out, so the drawing can be
 * looked at on a workstation.
 */
#ifndef O3E_HEAT_CURVE_H
#define O3E_HEAT_CURVE_H

#include <stdbool.h>

/* Outside temperatures the picture spans, warm on the left like the app. */
#define HEAT_CURVE_OUT_WARM   20.0
#define HEAT_CURVE_OUT_COLD  -30.0

typedef struct {
    double gradient;        /* Neigung */
    double level;           /* Niveau, K */
    double room;            /* room setpoint the line is drawn for, °C */
    bool   have_limits;     /* the circuit's flow limits; the line is clipped */
    double flow_min;
    double flow_max;
    bool   have_outside;    /* where the plant is right now */
    double outside;
    bool   have_flow;       /* its own flow setpoint; the formula's otherwise */
    double flow;
    const char *name;       /* circuit name, may be NULL */
} heat_curve_t;

/* Flow temperature for an outside temperature, without limits. */
double heat_curve_flow(double gradient, double level, double room, double outside);

/* The picture. Caller frees; NULL when memory ran out. */
char *heat_curve_svg(const heat_curve_t *c);

#endif /* O3E_HEAT_CURVE_H */
