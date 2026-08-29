/* requester.h — UAOS AmigaOS-style EasyRequest / file requester system
 *
 * Provides modal dialog windows for:
 *   - Confirmation (Yes/No / OK/Cancel)
 *   - String input (rename, new drawer name)
 *   - Information display (file info)
 *
 * Only one requester is active at a time (modal).
 */

#ifndef UAOS_REQUESTER_H
#define UAOS_REQUESTER_H

/* Requester button IDs (returned to callback) */
#define REQ_BTN_NONE   -1
#define REQ_BTN_OK      0
#define REQ_BTN_CANCEL  1
#define REQ_BTN_YES     0
#define REQ_BTN_NO      1

/* Callback type — called when a button is pressed.
 * For string requesters, text is the current input buffer.
 * For confirm requesters, text is NULL. */
typedef void (*ReqCallback)(int button, const char *text, void *user_data);

/* Open a confirmation requester with up to 2 buttons.
 * title: window title bar text
 * body:  message text (1-3 lines, \n separated)
 * btn1:  left button label (e.g., "Yes")
 * btn2:  right button label (e.g., "No"), or NULL for single-button
 * cb:    callback on button press (button = REQ_BTN_OK or REQ_BTN_CANCEL)
 * user_data: opaque pointer passed to callback */
void Requester_Confirm(const char *title, const char *body,
                       const char *btn1, const char *btn2,
                       ReqCallback cb, void *user_data);

/* Open a string input requester.
 * title: window title bar text
 * prompt: label text above the input field
 * initial: pre-filled text (or NULL for empty)
 * max_chars: maximum input length
 * cb: callback on button press (button = REQ_BTN_OK or REQ_BTN_CANCEL)
 * user_data: opaque pointer passed to callback */
void Requester_String(const char *title, const char *prompt,
                      const char *initial, int max_chars,
                      ReqCallback cb, void *user_data);

/* Open an information requester with a single OK button.
 * title: window title bar text
 * lines: array of text lines (NULL-terminated)
 * cb: optional callback (NULL is OK)
 * user_data: opaque pointer passed to callback */
void Requester_Info(const char *title, const char **lines,
                    ReqCallback cb, void *user_data);

/* Close any active requester immediately (e.g., on Escape). */
void Requester_Close(void);

/* Returns 1 if a requester is currently active. */
int  Requester_IsActive(void);

#endif /* UAOS_REQUESTER_H */
