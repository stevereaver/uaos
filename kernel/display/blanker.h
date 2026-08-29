/* blanker.h — UAOS Screen Blanker
 *
 * A commodity that blanks the screen after a configurable period of
 * mouse/keyboard inactivity.  Registers with the Commodities framework
 * so it can be controlled from Exchange.
 */

#ifndef UAOS_BLANKER_H
#define UAOS_BLANKER_H

void Blanker_Init(void);
void Blanker_Tick(void);          /* call once per second */
void Blanker_OnInput(void);       /* call on any mouse/keyboard activity */

int  Blanker_IsBlanked(void);
void Blanker_SetTimeout(int seconds);
int  Blanker_GetTimeout(void);

#endif /* UAOS_BLANKER_H */
