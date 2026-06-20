# Amiga Handler System Implementation Plan

## Executive Summary

**Feasibility: HIGH**

The UAOS kernel already has approximately 70% of the infrastructure needed for a full Amiga handler system. This document outlines the implementation plan for adding dynamic handler loading from `L:` directory, supporting both filesystem handlers (FastFileSystem, CDFilesystem) and non-filesystem handlers (aux-handler, port-handler, queue-handler).

---

## Current State Analysis

### Existing Infrastructure (Already Implemented)

| Component | Status | Location |
|-----------|--------|----------|
| Packet-based handler system | **Complete** | `kernel/dos/handler.h`, `kernel/dos/handler.c` |
| DosPacket action codes | **Complete** | `kernel/dos/dospacket.h` |
| RAMFS handler | **Complete** | `kernel/dos/ram_handler.c` |
| FAT32 handler | **Skeleton** | `kernel/dos/fat_handler.c` |
| M68k emulation layer | **Complete** | `emulation/uaos_emu.h` |
| LoadSeg/CreateProc | **Complete** | `kernel/exec/dos_lib.c` |
| SystemTagList | **Complete** | `kernel/exec/dos_lib.c` |
| L: assign | **Configured** | `kernel/dos/vfs.c` |

### Handler Architecture

Handlers are objects with an embedded `MsgPort` that process `DosPacket` messages:

```c
typedef struct Handler {
    MsgPort         port;       // embedded packet port
    const char     *name;       // human-readable name
    void           *private;    // filesystem-specific state
    void (*ProcessPacket)(struct Handler *h, DosPacket *pkt);
} Handler;
```

Current limitations:
- Static pool of 16 handlers (`MAX_HANDLERS = 16`)
- Synchronous dispatch only (`DoPkt()` blocks until complete)
- No dynamic loading from `L:` directory

---

## Amiga Handler System Requirements

In classic AmigaOS, handlers are M68k processes that:

1. **Reside in L:** as loadable binaries (e.g., `L:aux-handler`, `L:port-handler`)
2. **Are launched by `Mount` command** or filesystem access
3. **Run as separate processes** with their own MsgPort
4. **Process DosPackets** sent via `PutMsg()/GetMsg()`
5. **Can be filesystems** (FastFileSystem, CDFilesystem) or **non-filesystems** (aux, port, queue)

### Example Classic Handlers

| Handler | Type | Purpose |
|---------|------|---------|
| `aux-handler` | Device | Serial/aux device I/O |
| `port-handler` | Device | Parallel port I/O |
| `queue-handler` | Spooler | Print queue management |
| `pipe-handler` | Pipe | Inter-process pipes |
| `FastFileSystem` | Filesystem | FFS for hard disks |
| `CDFilesystem` | Filesystem | CD-ROM filesystem |
| `CrossDOSFilesystem` | Filesystem | PC-compatible disks |
| `Smartfile-System` | Filesystem | Third-party filesystem |

---

## Implementation Plan

### Phase 1: Dynamic Handler Loader (Foundation)

#### 1.1 Handler Registration System

**New file: `kernel/dos/handler_loader.h`**

```c
#ifndef UAOS_HANDLER_LOADER_H
#define UAOS_HANDLER_LOADER_H

#include "dos/handler.h"
#include <stdint.h>

typedef struct LHandlerEntry {
    char      name[32];        // "aux-handler"
    char      device_name[16]; // "AUX:"
    uint32_t  seglist_bptr;    // BPTR to loaded seglist (0 if not loaded)
    Handler  *handler;         // NULL if not running
    int       is_running;      // 1 = process active
    int       is_filesystem;   // 1 = filesystem handler, 0 = device handler
    MsgPort  *port;           // Handler's message port (when running)
    void     *device_ref;     // BlockDev* for filesystems, NULL for devices
} LHandlerEntry;

#define MAX_L_HANDLERS 16

/* Initialize handler loader - call at boot after VFS_Init */
void HandlerLoader_Init(void);

/* Scan L: directory for handler binaries (.handler extension or known names) */
int HandlerLoader_ScanLDirectory(void);

/* Load and start a handler by name (e.g., "aux-handler") */
LHandlerEntry *HandlerLoader_Load(const char *name);

/* Unload and stop a handler */
void HandlerLoader_Unload(const char *name);

/* Find handler entry by device name (e.g., "AUX:") */
LHandlerEntry *HandlerLoader_FindByDevice(const char *device_name);

/* Get handler entry by handler name (e.g., "aux-handler") */
LHandlerEntry *HandlerLoader_FindByName(const char *name);

/* List all registered handlers */
int HandlerLoader_ListAll(LHandlerEntry *out[], int max_count);

/* Check if a handler is a filesystem */
int HandlerLoader_IsFilesystem(const char *name);

#endif
```

**New file: `kernel/dos/handler_loader.c`**

Core implementation includes:
- Static table of `MAX_L_HANDLERS` entries
- Directory scanning logic using `VFS_OpenDir("Workbench:L")`
- Handler binary loading via existing `LoadSeg` infrastructure
- Process creation via `CreateProc` or `SystemTagList`

#### 1.2 Handler Process Bootstrap

The handler loading flow:

1. Load handler binary from `L:` using existing `LoadSeg()` infrastructure
2. Create process using existing `CreateProc()` / `SystemTagList()` with `NP_Seglist`
3. Handler's startup code calls `AddPort()` to register its MsgPort
4. Store mapping between device name and handler's MsgPort

Key code pattern:

```c
LHandlerEntry *HandlerLoader_Load(const char *name)
{
    // 1. Build path: "Workbench:L/<name>"
    char path[128];
    // ... construct path

    // 2. Load the seglist
    uint32_t seglist = loadseg_from_file(path);
    if (!seglist) return NULL;

    // 3. Build tag list for SystemTagList
    TagItem_t tags[] = {
        { NP_Seglist, seglist },
        { NP_Name, (uint32_t)name },
        { NP_StackSize, 4096 },
        { NP_Priority, 0 },
        { SYS_Asynch, 1 },  // Run asynchronously
        { TAG_DONE, 0 }
    };

    // 4. Create the process
    // (Call SystemTagList equivalent)

    // 5. Wait for handler to register its port
    // (Handler calls AddPort() on startup)

    // 6. Store in handler table
    // ...
}
```

#### 1.3 Integration Points

- Hook into `VFS_SetupWorkbenchAssigns()` to scan `L:` after boot
- Add `C:mount` command enhancement to trigger handler loading
- Add `C:avail` handlers listing command

### Phase 2: Non-Filesystem Handlers

#### 2.1 Device Handler Types

| Handler | Purpose | Required Actions |
|---------|---------|------------------|
| `aux-handler` | Serial/aux device | ACTION_READ, ACTION_WRITE, ACTION_WAIT_CHAR |
| `port-handler` | Parport device | ACTION_READ, ACTION_WRITE |
| `queue-handler` | Print spooling | ACTION_WRITE, queue management |
| `pipe-handler` | Inter-process pipes | Full file-like semantics |

#### 2.2 Non-Filesystem Handler Structure

**New file: `kernel/dos/device_handler.h`**

```c
#ifndef UAOS_DEVICE_HANDLER_H
#define UAOS_DEVICE_HANDLER_H

#include "dos/handler.h"

/* Device handler types */
#define DEV_TYPE_AUX     1  // Serial/aux
#define DEV_TYPE_PAR     2  // Parallel port
#define DEV_TYPE_QUEUE   3  // Print queue
#define DEV_TYPE_PIPE    4  // Pipe

typedef struct DeviceHandler {
    Handler  base;
    int      device_type;
    int      is_open;
    void    *device_state;  // UART buffer, queue, pipe state, etc.
    
    /* Device-specific operations */
    int32_t (*dev_read)(struct DeviceHandler *dh, void *buf, uint32_t len);
    int32_t (*dev_write)(struct DeviceHandler *dh, const void *buf, uint32_t len);
    int32_t (*dev_wait_char)(struct DeviceHandler *dh, uint32_t timeout);
    int32_t (*dev_flush)(struct DeviceHandler *dh);
} DeviceHandler;

/* Create a device handler */
DeviceHandler *DeviceHandler_Create(const char *name, int device_type);

/* Process device-specific packets */
void DeviceHandler_ProcessPacket(Handler *h, DosPacket *pkt);

#endif
```

#### 2.3 Aux Handler Implementation

**New file: `kernel/dos/aux_handler.c`**

Basic structure:
- Registers as "AUX:" device
- Handles serial-style I/O packets
- Can be backed by UART or virtual serial port

### Phase 3: Filesystem Handler Enhancements

#### 3.1 Foreign Filesystem Support

| Filesystem | Current Status | Completion Work |
|------------|---------------|-----------------|
| PFS3 | Skeleton exists (`kernel/dos/pfs3.h`) | Complete implementation |
| ext4 | Skeleton exists (`kernel/dos/ext4.h`) | Complete implementation |
| CrossDOS | New | FAT12/16 with Amiga attributes |
| SmartFile | New | Third-party handler support |

#### 3.2 Handler-based Mount System

Replace static `VFS_MountFat()` with dynamic handler dispatch:

```c
int VFS_MountWithHandler(const char *vol_name, const char *handler_name, BlockDev *bdev)
{
    // 1. Load the filesystem handler from L:
    LHandlerEntry *entry = HandlerLoader_Load(handler_name);
    if (!entry) return -1;

    // 2. Mark as filesystem type
    entry->is_filesystem = 1;
    entry->device_ref = bdev;

    // 3. Send ACTION_STARTUP or equivalent to initialize
    DosPacket startup_pkt;
    startup_pkt.dp_Type = ACTION_STARTUP;  // Custom action
    startup_pkt.dp_Arg1 = (int32_t)((uint64_t)bdev >> 2);  // BPTR to blockdev info
    // ... send packet to handler

    // 4. Register in mount table
    // ...
}
```

### Phase 4: Handler Message Infrastructure

#### 4.1 Async Packet Support

Current system is synchronous. True handlers need async operations:

**Extension to `kernel/dos/handler.h`**

```c
/* Async packet support */
typedef struct PendingPacket {
    DosPacket *pkt;
    MsgPort   *reply_port;
    uint32_t   timestamp;
    struct PendingPacket *next;
} PendingPacket;

/* Send packet asynchronously - returns immediately */
int32_t SendPktAsync(MsgPort *port, DosPacket *pkt, MsgPort *reply_port);

/* Check for pending replies */
void Handler_CheckReplies(void);

/* Wait for specific packet reply */
void WaitForReply(DosPacket *pkt);
```

#### 4.2 Handler Lifecycle

```c
/* Handler lifecycle actions */
#define ACTION_STARTUP    2000  // Initialize handler with device ref
#define ACTION_SHUTDOWN   2001  // Graceful shutdown request
#define ACTION_DIE        5     // Terminate (existing)

/* Inhibit/Uninhibit for device lock during media change */
int Handler_Inhibit(const char *device_name);
int Handler_Uninhibit(const char *device_name);

/* Handler restart on crash/exit */
int HandlerLoader_Restart(const char *name);
```

---

## Detailed Implementation Steps

| Step | Component | Effort | Files to Modify/Create |
|------|-----------|--------|---------------------|
| 1 | Handler loader scanner | Medium | `kernel/dos/handler_loader.c` (new) |
| 2 | Handler loader header | Low | `kernel/dos/handler_loader.h` (new) |
| 3 | L: directory scan at boot | Low | `kernel/dos/vfs.c` (hook into init) |
| 4 | Dynamic handler process creation | Medium | `kernel/exec/dos_lib.c` extensions |
| 5 | Handler registration in DosList | Medium | `kernel/dos/dos_list.c` (new) |
| 6 | C:mount command | Low | `kernel/shell/cmd_mount.c` (new) |
| 7 | C:avail handlers command | Low | `kernel/shell/cmd_avail.c` (extend) |
| 8 | Device handler base | Medium | `kernel/dos/device_handler.c` (new) |
| 9 | aux-handler implementation | High | `kernel/dos/aux_handler.c` (new) |
| 10 | port-handler implementation | Medium | `kernel/dos/port_handler.c` (new) |
| 11 | Async packet dispatch | High | `kernel/dos/handler.c` refactoring |
| 12 | PFS3 completion | High | `kernel/dos/pfs3.c` completion |
| 13 | Handler unmount/shutdown | Medium | `kernel/dos/handler_loader.c` |
| 14 | Handler restart on crash | Medium | `kernel/exec/task.c` extensions |

---

## Technical Considerations

### Handler Pool Expansion

Current `MAX_HANDLERS = 16` is sufficient for basic use but may need growth:

```c
/* In kernel/dos/handler.c */
#define MAX_HANDLERS 32  // Increased from 16
```

### M68k Handler Support

The `CreateProc()` implementation already launches M68k tasks from seglists:

```c
/* From kernel/exec/dos_lib.c */
UaosTask *new_task = Task_CreateM68k(proc_name, pri, seglist_buf, 
                                     first_seg_size, NULL, NULL);
```

### MsgPort Integration

Handlers need true MsgPorts for async operation. Current synchronous `DoPkt()` needs async companion.

### DosList Integration

Handlers must register in AmigaDOS `DosList` structure for system visibility:

```c
typedef struct DosList {
    struct Node ln;
    uint8_t     type;      // DLT_DEVICE, DLT_VOLUME, DLT_LOCK
    void       *dol_misc;
    char       *dol_name;
    union {
        struct {
            struct MsgPort *dol_task;
            BPTR            dol_lock;
        } dol_handler;
        // ... volume, lock variants
    } dol_u;
} DosList;
```

### L: Directory

Already assigned at boot to `Workbench:L` - just needs scanning logic.

---

## Example Handler Loading Flow

```
User runs:  mount AUX: from L:aux-handler

1. mount command calls HandlerLoader_Load("aux-handler")
   |
   +--> 2. Loader builds path "Workbench:L/aux-handler"
   |
   +--> 3. Loader calls loadseg_from_file() to load binary
   |       (Uses existing LoadSeg infrastructure)
   |
   +--> 4. Loader builds TagList with NP_Seglist, NP_Name, etc.
   |
   +--> 5. Loader calls SystemTagList() equivalent
   |       (Creates M68k process from seglist)
   |
   +--> 6. Handler process starts, calls AddPort("AUX")
   |       (Registers its MsgPort)
   |
   +--> 7. Loader stores mapping: "AUX:" -> MsgPort*

8. User opens AUX:
   |
   +--> 9. VFS routes via DoPkt(handler_port, ACTION_FINDINPUT, ...)
   |
   +--> 10. Handler processes packets until ACTION_DIE received
```

---

## Files to Create

```
kernel/dos/
├── handler_loader.h       # Handler loader public API
├── handler_loader.c       # Handler loader implementation
├── device_handler.h       # Device handler base class
├── device_handler.c       # Device handler implementation
├── aux_handler.h          # Aux handler public API
├── aux_handler.c          # Aux handler implementation
├── port_handler.h         # Parport handler public API
├── port_handler.c         # Parport handler implementation
├── dos_list.h             # DosList structure definitions
└── dos_list.c             # DosList implementation

kernel/shell/
├── cmd_mount.c            # Mount command (enhance or create)
└── cmd_avail.c            # Extend to list handlers
```

---

## Dependencies

| Dependency | Status | Notes |
|------------|--------|-------|
| LoadSeg | Complete | `kernel/exec/dos_lib.c` |
| CreateProc | Complete | `kernel/exec/dos_lib.c` |
| SystemTagList | Complete | `kernel/exec/dos_lib.c` |
| VFS | Complete | `kernel/dos/vfs.c` |
| Handler framework | Complete | `kernel/dos/handler.c` |
| M68k emulation | Complete | `emulation/uaos_emu.c` |
| Task creation | Complete | `kernel/exec/task.c` |
| Block device layer | Complete | `kernel/dos/blockdev.c` |

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Async dispatch complexity | Medium | High | Implement incrementally, keep sync path as fallback |
| M68k handler compatibility | Medium | Medium | Test with known handler binaries |
| Memory pressure from handlers | Low | Medium | Limit concurrent handlers, implement unload |
| Port registration race | Medium | Medium | Timeout/retry in loader, sync point before use |

---

## Success Criteria

1. **Basic Loading**: Can load aux-handler from L: and register AUX: device
2. **Mount Integration**: C:mount command can launch handlers dynamically
3. **Async Operation**: Handlers can process multiple packets concurrently
4. **Filesystem Handlers**: Can mount FFS volumes via handler from L:
5. **Lifecycle**: Handlers can be unloaded and reloaded cleanly
6. **List Command**: C:avail can list loaded handlers with status

---

## Conclusion

The Amiga handler system is highly feasible given the existing infrastructure. The implementation can proceed incrementally:

1. **Week 1-2**: Handler loader foundation (scanner, loading, registration)
2. **Week 3-4**: Non-filesystem handlers (aux, port)
3. **Week 5-6**: Async packet infrastructure
4. **Week 7-8**: Filesystem handler integration and foreign FS completion

The foundation exists. The main work items are the handler loader, async dispatch, and specific handler implementations.
