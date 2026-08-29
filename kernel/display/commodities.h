/* commodities.h — UAOS Commodities (CX) Framework
 *
 * Provides a simple broker registry for commodities — background tools
 * that can intercept input events or run as services.  The Exchange
 * window lists and controls registered commodities.
 */

#ifndef UAOS_COMMODITIES_H
#define UAOS_COMMODITIES_H

#include <stdint.h>

#define CX_MAX_BROKERS  16

/* Broker states */
typedef enum {
    CX_STATE_ACTIVE   = 0,
    CX_STATE_SLEEPING = 1,
    CX_STATE_DISABLED = 2,
} CxState;

/* Broker flags */
#define CX_FLAG_INPUT_HANDLER  0x01   /* intercepts input events */

/* Commodity broker — registered by tools like Blanker, FKey, etc. */
typedef struct {
    int        active;       /* slot in use */
    char       name[32];     /* commodity name (e.g. "Blanker")       */
    char       desc[64];     /* short description                      */
    int        flags;        /* CX_FLAG_* bits                         */
    CxState    state;        /* current state                          */
    void      *userdata;     /* opaque pointer for the broker owner    */
    /* Callbacks — called by Exchange when state changes */
    void     (*on_enable)(void *userdata);
    void     (*on_disable)(void *userdata);
    void     (*on_sleep)(void *userdata);
    void     (*on_wake)(void *userdata);
} CxBroker;

/* Register a commodity broker. Returns broker index (>=0) or -1 if full.
 * The broker starts in CX_STATE_ACTIVE. */
int  Cx_Register(const char *name, const char *desc, int flags,
                 void *userdata,
                 void (*on_enable)(void *),
                 void (*on_disable)(void *),
                 void (*on_sleep)(void *),
                 void (*on_wake)(void *));

/* Unregister a broker by index */
void Cx_Unregister(int idx);

/* Find a broker by name (case-insensitive). Returns index or -1. */
int  Cx_FindByName(const char *name);

/* Get broker by index (returns NULL if invalid) */
const CxBroker *Cx_GetBroker(int idx);

/* Get total number of registered brokers */
int  Cx_Count(void);

/* State transitions — called by Exchange or programmatically */
void Cx_Enable(int idx);
void Cx_Disable(int idx);
void Cx_Sleep(int idx);
void Cx_Wake(int idx);

/* Cycle state: active -> sleeping -> disabled -> active */
void Cx_CycleState(int idx);

#endif /* UAOS_COMMODITIES_H */
