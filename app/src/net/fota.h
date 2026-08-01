#ifndef FOTA_H
#define FOTA_H

/** @brief Log the running image's MCUboot state (swap type, version,
 * confirmation) at boot. No longer auto-confirms -- see
 * confirm_image_if_healthy(). */
void validate_image(void);

/** @brief Permanently confirm the running image, once, after the app has
 * proven it can do its day job (first successful update_stop() pass --
 * network up, Swiftly reachable, displays written). Until this runs, a
 * test-swapped image reverts to the previous slot on the next reset:
 * that is MCUboot's anti-brick safety net for FOTA'd images, and it now
 * also applies to newtmgr-uploaded ones (see README bench notes). No-op
 * when the image is already confirmed (external-programmer flashes,
 * every boot after the first healthy one). */
void confirm_image_if_healthy(void);

#endif  // FOTA_H
