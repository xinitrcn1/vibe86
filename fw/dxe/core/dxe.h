/*
 * The DXE core's internal interfaces (fw/dxe/core). The DXE core is the
 * UEFI boot services, the protocol database, events, the image loader and
 * the driver dispatcher (PI 1.8 Vol. 2 "DXE Core Interface"); runtime
 * services come from RuntimeDxe and the other architectural drivers.
 */
#ifndef DXE_H
#define DXE_H

#include "fwlib.h"
#include "layout.h"
#include "pe.h"
#include "vibe86fw.h"
#include "x86.h"

extern EFI_SYSTEM_TABLE *gST;
extern EFI_BOOT_SERVICES gBootServices;
extern EFI_RUNTIME_SERVICES *gRT;
extern EFI_HANDLE gDxeCoreImage;
extern int gBootServicesDone;

/* cpu.c */
void cpu_init(void);
void cpu_set_crash_hook(void (*hook)(void));

/* mem.c */
void mem_init(const void *hobs);
EFI_STATUS EFIAPI core_allocate_pages(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
    UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
EFI_STATUS EFIAPI core_free_pages(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
EFI_STATUS EFIAPI core_get_memory_map(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
EFI_STATUS EFIAPI core_allocate_pool(EFI_MEMORY_TYPE PoolType, UINTN Size, VOID **Buffer);
EFI_STATUS EFIAPI core_free_pool(VOID *Buffer);
EFI_STATUS mem_check_key(UINTN key);
void *core_alloc(UINTN size);			/* zeroed boot-services data, or NULL */
void *core_alloc_rt(UINTN size);		/* zeroed runtime-services data */
void core_free(void *p);
void mem_dump(void);

/* handle.c */
EFI_STATUS EFIAPI core_install_protocol(EFI_HANDLE *Handle, EFI_GUID *Protocol,
    EFI_INTERFACE_TYPE InterfaceType, VOID *Interface);
EFI_STATUS EFIAPI core_uninstall_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *Interface);
EFI_STATUS EFIAPI core_handle_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface);
EFI_STATUS EFIAPI core_locate_device_path(EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath,
    EFI_HANDLE *Device);
void handle_init_services(EFI_BOOT_SERVICES *bs);
int handle_valid(EFI_HANDLE h);
void handle_remove_all(EFI_HANDLE h);		/* uninstall every protocol */

/* event.c */
void event_init_services(EFI_BOOT_SERVICES *bs);
EFI_STATUS EFIAPI core_create_event_ex(UINT32 Type, EFI_TPL NotifyTpl, EFI_EVENT_NOTIFY NotifyFunction,
    const VOID *NotifyContext, const EFI_GUID *EventGroup, EFI_EVENT *Event);
EFI_STATUS EFIAPI core_signal_event(EFI_EVENT Event);
EFI_TPL EFIAPI core_raise_tpl(EFI_TPL NewTpl);
VOID EFIAPI core_restore_tpl(EFI_TPL OldTpl);
void event_signal_group(const EFI_GUID *group);
void event_timer_tick(void);

/* image.c */
void image_init(const void *hobs);
void image_init_services(EFI_BOOT_SERVICES *bs);
EFI_STATUS image_load_buffer(EFI_HANDLE parent, EFI_DEVICE_PATH_PROTOCOL *path, const void *buf,
    UINTN size, EFI_HANDLE *out);
EFI_STATUS EFIAPI core_start_image(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
const char *image_name_of(uint64_t rip, uint64_t *base);
void dispatch_drivers(const void *hobs);

/* jump.S */
typedef struct {
	uint64_t rbx, rsp, rbp, rdi, rsi, r12, r13, r14, r15, rip;
} jmp_ctx;
int core_setjmp(jmp_ctx *c);
void core_longjmp(jmp_ctx *c, int v) __attribute__((noreturn));

/* hob helpers (main.c) */
const void *hob_next(const void *h, UINT16 type);
const void *hob_guid(const void *hobs, const EFI_GUID *g);
extern const VIBE86_PLATFORM_INFO *gPlatformInfo;

/* list */
typedef struct list {
	struct list *next, *prev;
} list_t;
static inline void list_init(list_t *l) { l->next = l->prev = l; }
static inline int list_empty(const list_t *l) { return l->next == l; }
static inline void list_add_tail(list_t *l, list_t *n)
{
	n->prev = l->prev;
	n->next = l;
	l->prev->next = n;
	l->prev = n;
}
static inline void list_del(list_t *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n->prev = n;
}
#define list_for_each(it, head) for (list_t *it = (head)->next, *it##_n = it->next; it != (head); \
	it = it##_n, it##_n = it->next)

#endif
