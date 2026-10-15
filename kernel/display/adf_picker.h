/* adf_picker.h — double-click .adf → "Insert Disk" drive picker (UAOS-158)
 *
 * A Workbench affordance: double-clicking an .adf file (in a browser or
 * via a Leave Out desktop shortcut) opens a Requester_Choice listing
 * DF0:-DF3: + Cancel; picking a free drive inserts the image via
 * AdfMount_Insert. */

#ifndef UAOS_ADF_PICKER_H
#define UAOS_ADF_PICKER_H

/* 1 if path ends in ".adf" (case-insensitive; extension only, no
 * content sniffing). */
int  AdfPicker_IsAdfPath(const char *path);

/* Open the drive-picker requester for the given .adf path.  The path is
 * copied, so the caller's buffer may be reused after return. */
void AdfPicker_Open(const char *path);

#endif /* UAOS_ADF_PICKER_H */
