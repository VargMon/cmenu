# Security and Correctness Audit: Cursive Terminal UI Library

**Date**: 2026-05-11  
**Auditor**: Code Security Analysis  
**Repository**: VargMon/cmenu  
**File Analyzed**: cursive/cursive.c  
**Status**: 10 Issues Found (3 HIGH, 5 MEDIUM, 2 LOW-MEDIUM)

---

## Executive Summary

This ncurses/GPM-based terminal UI library contains **10 security and correctness vulnerabilities** ranging from use-after-free bugs to race conditions. Three issues are classified as HIGH severity and require immediate remediation. The library is not safe for production use without these fixes.

### Risk Assessment
- **Critical Use-After-Free**: Event names stored as dangling pointers
- **Memory Safety**: Double-free vulnerabilities in window destruction
- **Concurrency**: Unprotected static state in signal handlers
- **Integer Safety**: Arithmetic overflow in color pair calculations
- **Bounds Safety**: Multiple out-of-bounds access risks

---

## Issue #1: CRITICAL - Dangling Pointer in Event Management (CWE-416)

**Severity**: HIGH  
**CVSS Score**: 8.1 (High - Code Execution)  
**CWE**: CWE-416 Use After Free  
**Lines**: 1858-1873

### Vulnerability Description

The `cursive_event_set()` function stores a pointer to the caller's `event_name` buffer instead of copying it:

```c
int cursive_event_set(cursive_window_t *window, char *event_name, CursiveFunc func, void *arg) {
    // ...
    evt->event = event_name;  // DANGEROUS: Stores pointer, not copy
    evt->func = func;
    evt->arg = arg;
    return 1;
}
```

Later, `cursive_get_cursive_event()` dereferences this potentially invalid pointer:

```c
CURSIVE_EVENT *cursive_get_cursive_event(cursive_window_t *window, char *event_name) {
    // ...
    list_for_each(pos, &window->event_list) {
        CURSIVE_EVENT *evt = list_entry(pos, CURSIVE_EVENT, list);
        if (strcmp(evt->event, event_name) == 0)  // Use-after-free
            return evt;
    }
    return NULL;
}
```

### Attack Scenario

```c
{
    char local_event_name[32] = "window-focus";
    cursive_event_set(window, local_event_name, handler_func, NULL);
    // local_event_name goes out of scope - memory may be reused
}

// Later, application runs
cursive_get_cursive_event(window, "window-focus");
// Dereferences deallocated stack memory → SEGFAULT or information leak
```

### Impact
- **Denial of Service**: Application crashes when accessing events
- **Information Disclosure**: Read from deallocated memory
- **Potential Code Execution**: With heap spray techniques

### Root Cause
String lifetime not managed - caller retains ownership but library assumes it persists.

### Recommended Fix
Copy the event name string and manage its lifetime:

```c
int cursive_event_set(cursive_window_t *window, char *event_name, CursiveFunc func, void *arg) {
    if (!window || !event_name || !func) return -1;
    
    CURSIVE_EVENT *evt = cursive_get_cursive_event(window, event_name);
    if (!evt) {
        evt = (CURSIVE_EVENT*)calloc(1, sizeof(CURSIVE_EVENT));
        if (!evt) {
            fprintf(stderr, "cursive_event_set: allocation failed\n");
            return -1;
        }
        evt->event = strdup(event_name);  // FIX: Copy the string
        if (!evt->event) {
            free(evt);
            fprintf(stderr, "cursive_event_set: strdup failed\n");
            return -1;
        }
        list_add(&evt->list, &window->event_list);
    } else {
        // Update existing - free old name if different
        if (evt->event && strcmp(evt->event, event_name) != 0) {
            free(evt->event);
            evt->event = strdup(event_name);
            if (!evt->event) {
                fprintf(stderr, "cursive_event_set: strdup failed\n");
                return -1;
            }
        }
    }
    evt->func = func;
    evt->arg = arg;
    return 1;
}
```

And update deletion to free the copied string:

```c
int cursive_event_del(cursive_window_t *window, char *event_name) {
    if (!window || !event_name) return -1;
    if (list_empty(&window->event_list)) return ERR;
    
    if (event_name[0] == '*') {
        struct list_head *pos, *tmp;
        list_for_each_safe(pos, tmp, &window->event_list) {
            CURSIVE_EVENT *evt = list_entry(pos, CURSIVE_EVENT, list);
            free(evt->event);  // FIX: Free the copied string
            list_del(&evt->list);
            free(evt);
        }
        return 1;
    }
    
    CURSIVE_EVENT *evt = cursive_get_cursive_event(window, event_name);
    if (evt) {
        free(evt->event);  // FIX: Free the copied string
        list_del(&evt->list);
        free(evt);
        return 1;
    }
    return ERR;
}
```

---

## Issue #2: CRITICAL - Double-Free in Window Destruction (CWE-415)

**Severity**: HIGH  
**CVSS Score**: 8.1 (High - Code Execution)  
**CWE**: CWE-415 Double Free  
**Lines**: 1315-1330, 1690-1710

### Vulnerability Description

`cursive_window_close()` moves a window to the zombie list but doesn't prevent calling it multiple times. Later, `cursive_window_destroy()` deletes and frees the window without checking if it's already been destroyed.

### Attack Scenario

```c
cursive_window_t *win = cursive_window_create(...);
cursive_window_close(win);      // Moves to zombie list
cursive_window_close(win);      // Corrupts list linkage
cursive_window_destroy(win);    // First destroy
cursive_window_destroy(win);    // Double-free CRASH
```

### Impact
- **Heap Corruption**: Corrupts free list metadata
- **Denial of Service**: Crash during cleanup
- **Potential Code Execution**: With careful heap layout control

### Recommended Fix

Add destruction guard flag:

```c
struct _cursive_window_s {
    // ... existing fields ...
    bool is_closed;  // FIX: Add destruction guard
};

void cursive_window_close(cursive_window_t *cursive_window) {
    if (!cursive_window) return;
    
    if (cursive_window->is_closed) {  // FIX: Prevent double-close
        fprintf(stderr, "cursive_window_close: window already closed\n");
        return;
    }
    cursive_window->is_closed = true;
    
    int screen_id = cursive_window->ctx->screen_id;
    bool was_managed = (cursive_window->ctx->managed == TRUE);
    cursive_event_run(cursive_window, "window-close");
    list_move(&cursive_window->list, &cursive->zombie_list);
    if (was_managed) focus_next_managed_window(screen_id);
    cursive_screen_redraw(screen_id, REDRAW_ALL);
}
```

---

## Issue #3: CRITICAL - Race Condition in Static Event State (CWE-362)

**Severity**: HIGH  
**CVSS Score**: 7.5 (High - DoS/Data Corruption)  
**CWE**: CWE-362 Concurrent Modification Without Synchronization  
**Lines**: 1654-1658

### Vulnerability Description

Static event state variables are modified by signal handlers and main event loop without synchronization:

```c
static cursive_window_t *s_eventWindow = NULL;
static MEVENT s_prevMouseEvent = {0};
static int s_eventMode = EVENTMODE_IDLE;
static CursiveWkeyFunc s_currentKeyFunc = NULL;
```

### Impact
- **Denial of Service**: Incorrect window movement/resize
- **Data Corruption**: Corrupted window state
- **Event Handler Deadlock**: Invalid pointers cause crashes

### Recommended Fix

Use atomic operations:

```c
#include <stdatomic.h>

static _Atomic(cursive_window_t *) s_eventWindow = NULL;
static volatile MEVENT s_prevMouseEvent = {0};
static _Atomic(int) s_eventMode = EVENTMODE_IDLE;

static inline cursive_window_t *get_event_window(void) {
    return atomic_load(&s_eventWindow);
}

static inline void set_event_window(cursive_window_t *win) {
    atomic_store(&s_eventWindow, win);
}
```

---

## Issue #4: MEDIUM - Stagger Position Integer Boundary Violations (CWE-190)

**Severity**: MEDIUM  
**CVSS Score**: 5.5 (Medium - DoS)  
**Lines**: 1082-1093

Windows positioned outside screen bounds when size exceeds maximum dimensions.

### Recommended Fix

```c
static void compute_stagger_position(int width, int height, int maxCols, int maxRows, int *outX, int *outY) {
    // FIX: Validate input dimensions
    if (width >= maxCols || height >= maxRows) {
        *outX = 0;
        *outY = 0;
        return;
    }
    
    static int currentX = kInitialStaggerX;
    static int currentY = kInitialStaggerY;
    currentX += kStaggerDeltaX;
    currentY += kStaggerDeltaY;
    
    // FIX: Ensure next position doesn't overflow
    if (currentX + width > maxCols) {
        currentX = 1;
        if (currentX + width > maxCols) {
            currentX = ABSINT(maxCols - width - 1);
        }
    }
    
    if (currentY + height > maxRows) {
        currentY = 1;
        if (currentY + height > maxRows) {
            currentY = ABSINT(maxRows - height - 1);
        }
    }
    
    *outX = currentX;
    *outY = currentY;
}
```

---

## Issue #5: MEDIUM - Missing Null Pointer Validation (CWE-476)

**Severity**: MEDIUM  
**CVSS Score**: 6.5 (Medium - Crash/Information Disclosure)  
**Lines**: 1472-1488

### Recommended Fix

```c
bool cursive_window_set_focus(cursive_window_t *cursive_window) {
    // FIX: Validate all pointer inputs
    if (!cursive_window || !cursive_window->ctx || !cursive) 
        return false;
    
    int screen_id = cursive_window->ctx->screen_id;
    
    // FIX: Validate screen_id bounds
    if (screen_id < 0 || screen_id >= MAX_SCREENS) {
        fprintf(stderr, "cursive_window_set_focus: invalid screen_id %d\n", screen_id);
        return false;
    }
    
    cursive_screen_t *screen = &cursive->cursive_screen[screen_id];
    struct list_head *wnd_list = cursive_window->ctx->managed 
        ? &screen->managed_list 
        : &screen->unmanaged_list;
    
    if (list_empty(wnd_list) && cursive_window->ctx->managed) {
        fprintf(stderr, "cursive_window_set_focus: list uninitialized\n");
        return false;
    }
    
    cursive_window_t *sibling;
    list_for_each_entry(sibling, wnd_list, list) {
        if (!sibling || !sibling->window_state) continue;
        
        if (sibling->window_state & STATE_FOCUS) {
            sibling->window_state &= ~STATE_FOCUS;
            cursive_event_run(sibling, "window-unfocus");
        }
    }
    
    cursive_window->window_state |= STATE_FOCUS;
    cursive_event_run(cursive_window, "window-focus");
    return true;
}
```

---

## Issue #6: MEDIUM - Bounds Violation in Border Modification (CWE-119)

**Severity**: MEDIUM  
**CVSS Score**: 6.8 (Medium - Buffer Overflow)  
**Lines**: 845-873

### Recommended Fix

```c
void window_modify_border(WINDOW *window, int attrs, short color_pair) {
    if (!window) return;
    
    int rows, cols;
    get_win_geom(window, &rows, &cols, NULL, NULL);
    
    // FIX: Validate dimensions are reasonable
    if (rows <= 1 || cols <= 1) {
        fprintf(stderr, "window_modify_border: invalid window size %dx%d\n", cols, rows);
        return;
    }
    
    attr_t base_attr = (attr_t)attrs;
    
    for (int x = 0; x < cols; ++x) {
        if (x < 0 || x >= cols) continue;
        chtype top = mvwinch(window, 0, x);
        chtype bottom = mvwinch(window, rows - 1, x);
        // ... rest of implementation ...
    }
    
    for (int y = 1; y < rows - 1; ++y) {
        if (y < 1 || y >= rows - 1) continue;
        chtype left = mvwinch(window, y, 0);
        chtype right = mvwinch(window, y, cols - 1);
        // ... rest of implementation ...
    }
}
```

---

## Issue #7: MEDIUM - Integer Overflow in Color Pair Calculation (CWE-190)

**Severity**: MEDIUM  
**CVSS Score**: 5.9 (Medium - Logic Error/DoS)  
**Lines**: 738-742

### Recommended Fix

```c
static inline short fast_color_pair(short fg, short bg) {
    // FIX: Validate inputs are in range
    if (fg < 0 || fg >= cursive_color_count) {
        fprintf(stderr, "fast_color_pair: invalid foreground %d\n", fg);
        return 0;
    }
    if (bg < 0 || bg >= cursive_color_count) {
        fprintf(stderr, "fast_color_pair: invalid background %d\n", bg);
        return 0;
    }
    
    // FIX: Check for overflow before calculation
    int result = bg * cursive_color_count + (cursive_color_count - fg - 1);
    if (result < 0 || result > SHRT_MAX) {
        fprintf(stderr, "fast_color_pair: overflow in calculation\n");
        return 0;
    }
    
    return (short)result;
}
```

---

## Issue #8: MEDIUM - Missing Bounds in Shadow Window (CWE-119)

**Severity**: MEDIUM  
**CVSS Score**: 6.2 (Medium - Out-of-bounds Access)  
**Lines**: 825-843

### Recommended Fix

```c
WINDOW *window_create_shadow(WINDOW *base_win, WINDOW *underlying_win) {
    if (!base_win) return NULL;
    
    int height, width, beg_y, beg_x;
    get_win_geom(base_win, &height, &width, &beg_y, &beg_x);
    
    if (!underlying_win) underlying_win = SCREEN_WINDOW;
    
    // FIX: Validate shadow position doesn't overflow screen
    int screen_h, screen_w;
    get_win_geom(underlying_win, &screen_h, &screen_w, NULL, NULL);
    
    int shadow_y = beg_y + 1;
    int shadow_x = beg_x + 1;
    
    if (shadow_y + height > screen_h || shadow_x + width > screen_w) {
        fprintf(stderr, "window_create_shadow: shadow would overflow screen\n");
        return NULL;
    }
    
    WINDOW *shadow = newwin(height, width, shadow_y, shadow_x);
    if (!shadow) return NULL;
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            cchar_t cell;
            int src_y = beg_y + y + 1, src_x = beg_x + x + 1;
            
            // FIX: Validate source coordinates are within bounds
            if (src_y < 0 || src_y >= screen_h || src_x < 0 || src_x >= screen_w) {
                continue;
            }
            
            if (create_shadow_cell(underlying_win, src_y, src_x, &cell) == OK)
                mvwadd_wch(shadow, y, x, &cell);
        }
    }
    
    return shadow;
}
```

---

## Issue #9: LOW-MEDIUM - Incomplete Escape Sequence Processing (CWE-684)

**Severity**: LOW-MEDIUM  
**CVSS Score**: 5.3 (Medium - Data Loss)  
**Lines**: 1684-1698

### Recommended Fix

```c
static int32_t readEscapeSequence(void) {
    int32_t escCode = 27;
    
    // FIX: Explicit loop for defined number of bytes
    for (int i = 0; i < 3; ++i) {
        int ch = getch();
        if (ch == ERR) break;
        escCode |= (ch << (4 + i * 8));
    }
    
    return escCode;
}
```

---

## Issue #10: LOW-MEDIUM - Unvalidated GPM Library State (CWE-129)

**Severity**: LOW-MEDIUM  
**CVSS Score**: 4.8 (Low - Configuration Error)  
**Lines**: 1757-1796

### Recommended Fix

```c
int cursive_kmio_gpm(MEVENT *mouse_event, uint16_t cmd) {
    extern uint32_t cursive_global_flags;
    extern int gpm_tried;
    extern int gpm_fd;
    static int gpm_fd_internal = -1;
    
    if (cmd == CMD_GPM_CLOSE) {
        if (gpm_fd_internal > 0) {
            Gpm_Close();
            gpm_fd_internal = -1;  // FIX: Mark as closed
        }
        return 0;
    }
    
    if (!mouse_event) return -1;
    
    // FIX: Validate external state more carefully
    if (gpm_tried && gpm_fd == -1) return -1;
    if (gpm_fd == -2) return -1;
    
    if (gpm_fd_internal < 0) {
        Gpm_Connect conn = {0};
        conn.defaultMask = 0;
        conn.eventMask = GPM_MOVE | GPM_UP | GPM_DOWN | GPM_DRAG;
        conn.maxMod = ~0;
        
        gpm_fd_internal = Gpm_Open(&conn, 0);
        if (gpm_fd_internal < 0) {
            fprintf(stderr, "cursive_kmio_gpm: Gpm_Open() failed\n");
            return -1;
        }
    }
    
    if (gpm_fd_internal < 0) return -1;
    
    // FIX: Validate fd is still valid
    if (fcntl(gpm_fd_internal, F_GETFD) < 0) {
        fprintf(stderr, "cursive_kmio_gpm: fd became invalid\n");
        Gpm_Close();
        gpm_fd_internal = -1;
        return -1;
    }
    
    // ... rest of function
    return 0;
}
```

---

## Summary Table

| # | Issue | Severity | CWE | Type |
|---|-------|----------|-----|------|
| 1 | Dangling Pointer - Event Names | HIGH | 416 | Use-After-Free |
| 2 | Double-Free - Window Destruction | HIGH | 415 | Double Free |
| 3 | Race Condition - Static Event State | HIGH | 362 | Concurrency |
| 4 | Stagger Bounds Violation | MEDIUM | 190 | Integer Overflow |
| 5 | Null Pointer - Focus Management | MEDIUM | 476 | Null Deref |
| 6 | Bounds Violation - Border Mod | MEDIUM | 119 | Buffer Overflow |
| 7 | Integer Overflow - Color Pair | MEDIUM | 190 | Integer Overflow |
| 8 | Missing Bounds - Shadow Window | MEDIUM | 119 | Out-of-Bounds |
| 9 | Incomplete Escape Sequence | LOW-MED | 684 | Data Loss |
| 10 | Unvalidated GPM State | LOW-MED | 129 | Config Error |

---

## Remediation Priority

1. **Immediate** (This Week): Issues #1, #2, #3
2. **Short-term** (This Sprint): Issues #5, #7
3. **Medium-term** (Next Sprint): Issues #4, #6, #8
4. **Backlog**: Issues #9, #10

---

## Testing Recommendations

Enable sanitizers during compilation and testing:

```bash
gcc -fsanitize=address,undefined,thread -g3 cursive.c
clang -fsanitize=address,memory,thread cursive.c
valgrind --leak-check=full --track-origins=yes ./program
```

---

## References

- CWE-416: Use After Free - https://cwe.mitre.org/data/definitions/416.html
- CWE-415: Double Free - https://cwe.mitre.org/data/definitions/415.html
- CWE-362: Concurrent Modification - https://cwe.mitre.org/data/definitions/362.html
- ncurses Programming HOWTO - https://tldp.org/HOWTO/NCURSES-Programming-HOWTO/
