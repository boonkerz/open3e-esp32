/* Keeps the plant schematic Home Assistant shows in step with the machine.
 *
 * The picture itself is static SVG; whether its pipes move is a matter of
 * CSS. The web interface knows the compressor and pump speed because it reads
 * them while the tab is open; Home Assistant only has the picture. So a small
 * task reads the two values every half minute and republishes the picture
 * with the idle circuits' animation switched off whenever that state changes.
 */
#ifndef O3E_PLANT_IMAGE_H
#define O3E_PLANT_IMAGE_H

void plant_image_start(void);

#endif
