/*
 * DXE core: the protocol handler services (UEFI 2.10 7.3): handles,
 * protocol interfaces, open-protocol bookkeeping, protocol notifications,
 * LocateHandle/LocateDevicePath/LocateProtocol. Controllers are connected
 * by the drivers when they are dispatched (fw/README.md), so
 * ConnectController only reports success and DisconnectController stops
 * nothing.
 */
#include "dxe.h"

#define HANDLE_MAGIC	0x4C444E48u	/* "HNDL" */

struct open_info {
	struct open_info *next;
	EFI_HANDLE agent, controller;
	UINT32 attributes, count;
};

struct proto_iface {
	list_t link;			/* in the handle's list */
	list_t by_proto;		/* in the protocol's list */
	struct protocol *proto;
	struct handle *handle;
	void *iface;
	struct open_info *opens;
};

struct handle {
	uint32_t magic;
	list_t link;			/* all handles */
	list_t ifaces;
	UINT64 key;			/* install order (LocateHandle(ByRegisterNotify)) */
};

struct notify {
	list_t link;
	EFI_EVENT event;
	struct proto_iface *pending[32];	/* installed since the last LocateHandle */
	unsigned head, tail;
};

struct protocol {
	list_t link;
	EFI_GUID guid;
	list_t ifaces;
	list_t notifies;
};

static list_t handles = { &handles, &handles };
static list_t protocols = { &protocols, &protocols };
static UINT64 next_key = 1;

int
handle_valid(EFI_HANDLE h)
{
	struct handle *hd = h;
	if (!h)
		return 0;
	list_for_each(it, &handles)
		if (CONTAINER_OF(it, struct handle, link) == hd)
			return hd->magic == HANDLE_MAGIC;
	return 0;
}

static struct protocol *
find_protocol(const EFI_GUID *g, int create)
{
	list_for_each(it, &protocols) {
		struct protocol *p = CONTAINER_OF(it, struct protocol, link);
		if (guid_eq(&p->guid, g))
			return p;
	}
	if (!create)
		return 0;
	struct protocol *p = core_alloc(sizeof *p);
	if (!p)
		return 0;
	p->guid = *g;
	list_init(&p->ifaces);
	list_init(&p->notifies);
	list_add_tail(&protocols, &p->link);
	return p;
}

static struct proto_iface *
find_iface(struct handle *h, const EFI_GUID *g)
{
	list_for_each(it, &h->ifaces) {
		struct proto_iface *pi = CONTAINER_OF(it, struct proto_iface, link);
		if (guid_eq(&pi->proto->guid, g))
			return pi;
	}
	return 0;
}

EFI_STATUS EFIAPI
core_install_protocol(EFI_HANDLE *Handle, EFI_GUID *Protocol, EFI_INTERFACE_TYPE InterfaceType,
    VOID *Interface)
{
	if (!Handle || !Protocol || InterfaceType != EFI_NATIVE_INTERFACE)
		return EFI_INVALID_PARAMETER;
	struct handle *h = *Handle;
	if (h) {
		if (!handle_valid(h))
			return EFI_INVALID_PARAMETER;
		if (find_iface(h, Protocol))
			return EFI_INVALID_PARAMETER;
	}
	struct protocol *p = find_protocol(Protocol, 1);
	struct proto_iface *pi = core_alloc(sizeof *pi);
	if (!p || !pi)
		return EFI_OUT_OF_RESOURCES;
	if (!h) {
		if (!(h = core_alloc(sizeof *h))) {
			core_free(pi);
			return EFI_OUT_OF_RESOURCES;
		}
		h->magic = HANDLE_MAGIC;
		list_init(&h->ifaces);
		list_add_tail(&handles, &h->link);
		*Handle = h;
	}
	h->key = next_key++;
	pi->proto = p;
	pi->handle = h;
	pi->iface = Interface;
	list_add_tail(&h->ifaces, &pi->link);
	list_add_tail(&p->ifaces, &pi->by_proto);
	/* notify the registrations for this protocol */
	list_for_each(it, &p->notifies) {
		struct notify *n = CONTAINER_OF(it, struct notify, link);
		if (n->tail - n->head < ARRAY_SIZE(n->pending))
			n->pending[n->tail++ % ARRAY_SIZE(n->pending)] = pi;
		core_signal_event(n->event);
	}
	return EFI_SUCCESS;
}

static void
drop_from_notifies(struct proto_iface *pi)
{
	list_for_each(it, &pi->proto->notifies) {
		struct notify *n = CONTAINER_OF(it, struct notify, link);
		for (unsigned i = n->head; i != n->tail; i++)
			if (n->pending[i % ARRAY_SIZE(n->pending)] == pi)
				n->pending[i % ARRAY_SIZE(n->pending)] = 0;
	}
}

static void
remove_iface(struct proto_iface *pi)
{
	struct handle *h = pi->handle;
	drop_from_notifies(pi);
	list_del(&pi->link);
	list_del(&pi->by_proto);
	for (struct open_info *o = pi->opens, *nx; o; o = nx) {
		nx = o->next;
		core_free(o);
	}
	core_free(pi);
	if (list_empty(&h->ifaces)) {
		list_del(&h->link);
		h->magic = 0;
		core_free(h);
	}
}

EFI_STATUS EFIAPI
core_uninstall_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *Interface)
{
	if (!handle_valid(Handle) || !Protocol)
		return EFI_INVALID_PARAMETER;
	struct proto_iface *pi = find_iface(Handle, Protocol);
	if (!pi || pi->iface != Interface)
		return EFI_NOT_FOUND;
	for (struct open_info *o = pi->opens; o; o = o->next)
		if (o->attributes & (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE))
			return EFI_ACCESS_DENIED;
	remove_iface(pi);
	return EFI_SUCCESS;
}

void
handle_remove_all(EFI_HANDLE Handle)
{
	if (!handle_valid(Handle))
		return;
	struct handle *h = Handle;
	while (!list_empty(&h->ifaces)) {
		int last = h->ifaces.next->next == &h->ifaces;
		remove_iface(CONTAINER_OF(h->ifaces.next, struct proto_iface, link));
		if (last)
			break;		/* the handle is gone */
	}
}

static EFI_STATUS EFIAPI
core_reinstall_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID *OldInterface, VOID *NewInterface)
{
	if (!handle_valid(Handle) || !Protocol)
		return EFI_INVALID_PARAMETER;
	struct proto_iface *pi = find_iface(Handle, Protocol);
	if (!pi || pi->iface != OldInterface)
		return EFI_NOT_FOUND;
	pi->iface = NewInterface;
	list_for_each(it, &pi->proto->notifies) {
		struct notify *n = CONTAINER_OF(it, struct notify, link);
		if (n->tail - n->head < ARRAY_SIZE(n->pending))
			n->pending[n->tail++ % ARRAY_SIZE(n->pending)] = pi;
		core_signal_event(n->event);
	}
	return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
core_handle_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface)
{
	if (!handle_valid(Handle) || !Protocol || !Interface)
		return EFI_INVALID_PARAMETER;
	struct proto_iface *pi = find_iface(Handle, Protocol);
	if (!pi) {
		*Interface = 0;
		return EFI_UNSUPPORTED;
	}
	*Interface = pi->iface;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_open_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, VOID **Interface, EFI_HANDLE AgentHandle,
    EFI_HANDLE ControllerHandle, UINT32 Attributes)
{
	if (!handle_valid(Handle) || !Protocol)
		return EFI_INVALID_PARAMETER;
	if (Attributes != EFI_OPEN_PROTOCOL_TEST_PROTOCOL && !Interface)
		return EFI_INVALID_PARAMETER;
	struct proto_iface *pi = find_iface(Handle, Protocol);
	if (!pi) {
		if (Interface && Attributes != EFI_OPEN_PROTOCOL_TEST_PROTOCOL)
			*Interface = 0;
		return EFI_UNSUPPORTED;
	}
	if (Attributes == EFI_OPEN_PROTOCOL_TEST_PROTOCOL)
		return EFI_SUCCESS;
	*Interface = pi->iface;
	if (Attributes & (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE)) {
		for (struct open_info *o = pi->opens; o; o = o->next) {
			if (!(o->attributes & (EFI_OPEN_PROTOCOL_BY_DRIVER | EFI_OPEN_PROTOCOL_EXCLUSIVE)))
				continue;
			if (o->agent == AgentHandle)
				return EFI_ALREADY_STARTED;
			return EFI_ACCESS_DENIED;
		}
	}
	for (struct open_info *o = pi->opens; o; o = o->next)
		if (o->agent == AgentHandle && o->controller == ControllerHandle &&
		    o->attributes == Attributes) {
			o->count++;
			return EFI_SUCCESS;
		}
	struct open_info *o = core_alloc(sizeof *o);
	if (!o)
		return EFI_OUT_OF_RESOURCES;
	*o = (struct open_info){ pi->opens, AgentHandle, ControllerHandle, Attributes, 1 };
	pi->opens = o;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_close_protocol(EFI_HANDLE Handle, EFI_GUID *Protocol, EFI_HANDLE AgentHandle,
    EFI_HANDLE ControllerHandle)
{
	if (!handle_valid(Handle) || !Protocol)
		return EFI_INVALID_PARAMETER;
	struct proto_iface *pi = find_iface(Handle, Protocol);
	if (!pi)
		return EFI_NOT_FOUND;
	for (struct open_info **pp = &pi->opens; *pp; pp = &(*pp)->next)
		if ((*pp)->agent == AgentHandle && (*pp)->controller == ControllerHandle) {
			struct open_info *o = *pp;
			*pp = o->next;
			core_free(o);
			return EFI_SUCCESS;
		}
	return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI
core_open_protocol_information(EFI_HANDLE Handle, EFI_GUID *Protocol,
    EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **EntryBuffer, UINTN *EntryCount)
{
	if (!handle_valid(Handle) || !Protocol || !EntryBuffer || !EntryCount)
		return EFI_INVALID_PARAMETER;
	struct proto_iface *pi = find_iface(Handle, Protocol);
	if (!pi)
		return EFI_NOT_FOUND;
	UINTN n = 0;
	for (struct open_info *o = pi->opens; o; o = o->next)
		n++;
	EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e;
	if (EFI_ERROR(core_allocate_pool(EfiBootServicesData, (n ? n : 1) * sizeof *e, (void **)&e)))
		return EFI_OUT_OF_RESOURCES;
	n = 0;
	for (struct open_info *o = pi->opens; o; o = o->next, n++)
		e[n] = (EFI_OPEN_PROTOCOL_INFORMATION_ENTRY){ o->agent, o->controller, o->attributes, o->count };
	*EntryBuffer = e;
	*EntryCount = n;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_protocols_per_handle(EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer, UINTN *ProtocolBufferCount)
{
	if (!handle_valid(Handle) || !ProtocolBuffer || !ProtocolBufferCount)
		return EFI_INVALID_PARAMETER;
	struct handle *h = Handle;
	UINTN n = 0;
	list_for_each(it, &h->ifaces)
		n++;
	EFI_GUID **b;
	if (EFI_ERROR(core_allocate_pool(EfiBootServicesData, (n ? n : 1) * sizeof *b, (void **)&b)))
		return EFI_OUT_OF_RESOURCES;
	n = 0;
	list_for_each(it, &h->ifaces)
		b[n++] = &CONTAINER_OF(it, struct proto_iface, link)->proto->guid;
	*ProtocolBuffer = b;
	*ProtocolBufferCount = n;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_register_protocol_notify(EFI_GUID *Protocol, EFI_EVENT Event, VOID **Registration)
{
	if (!Protocol || !Event || !Registration)
		return EFI_INVALID_PARAMETER;
	struct protocol *p = find_protocol(Protocol, 1);
	struct notify *n = core_alloc(sizeof *n);
	if (!p || !n)
		return EFI_OUT_OF_RESOURCES;
	n->event = Event;
	list_add_tail(&p->notifies, &n->link);
	*Registration = n;
	return EFI_SUCCESS;
}

/* the handles that match a search, into buf (up to max); returns the count */
static UINTN
search(EFI_LOCATE_SEARCH_TYPE type, EFI_GUID *protocol, void *key, EFI_HANDLE *buf, UINTN max)
{
	UINTN n = 0;
	switch (type) {
	case AllHandles:
		list_for_each(it, &handles) {
			if (n < max)
				buf[n] = CONTAINER_OF(it, struct handle, link);
			n++;
		}
		break;
	case ByProtocol: {
		struct protocol *p = find_protocol(protocol, 0);
		if (!p)
			break;
		list_for_each(it, &p->ifaces) {
			if (n < max)
				buf[n] = CONTAINER_OF(it, struct proto_iface, by_proto)->handle;
			n++;
		}
		break;
	}
	case ByRegisterNotify: {
		struct notify *nt = key;
		/* one handle per call (UEFI 2.10 7.3.12) */
		while (nt->head != nt->tail) {
			struct proto_iface *pi = nt->pending[nt->head % ARRAY_SIZE(nt->pending)];
			if (pi) {
				if (max) {
					buf[0] = pi->handle;
					nt->head++;
				}
				return 1;
			}
			nt->head++;
		}
		break;
	}
	}
	return n;
}

static EFI_STATUS EFIAPI
core_locate_handle(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol, VOID *SearchKey,
    UINTN *BufferSize, EFI_HANDLE *Buffer)
{
	if (!BufferSize || (SearchType == ByProtocol && !Protocol) ||
	    (SearchType == ByRegisterNotify && !SearchKey) || SearchType > ByProtocol)
		return EFI_INVALID_PARAMETER;
	UINTN max = Buffer ? *BufferSize / sizeof(EFI_HANDLE) : 0;
	UINTN n = search(SearchType, Protocol, SearchKey, Buffer, max);
	if (!n)
		return EFI_NOT_FOUND;
	if (n > max) {
		*BufferSize = n * sizeof(EFI_HANDLE);
		return EFI_BUFFER_TOO_SMALL;
	}
	*BufferSize = n * sizeof(EFI_HANDLE);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_locate_handle_buffer(EFI_LOCATE_SEARCH_TYPE SearchType, EFI_GUID *Protocol, VOID *SearchKey,
    UINTN *NoHandles, EFI_HANDLE **Buffer)
{
	if (!NoHandles || !Buffer)
		return EFI_INVALID_PARAMETER;
	UINTN size = 0;
	EFI_STATUS s = core_locate_handle(SearchType, Protocol, SearchKey, &size, 0);
	if (s != EFI_BUFFER_TOO_SMALL) {
		*NoHandles = 0;
		*Buffer = 0;
		return EFI_ERROR(s) ? s : EFI_NOT_FOUND;
	}
	if (EFI_ERROR(core_allocate_pool(EfiBootServicesData, size, (void **)Buffer)))
		return EFI_OUT_OF_RESOURCES;
	s = core_locate_handle(SearchType, Protocol, SearchKey, &size, *Buffer);
	*NoHandles = size / sizeof(EFI_HANDLE);
	return s;
}

static EFI_STATUS EFIAPI
core_locate_protocol(EFI_GUID *Protocol, VOID *Registration, VOID **Interface)
{
	if (!Protocol || !Interface)
		return EFI_INVALID_PARAMETER;
	*Interface = 0;
	if (Registration) {
		EFI_HANDLE h;
		if (!search(ByRegisterNotify, Protocol, Registration, &h, 1))
			return EFI_NOT_FOUND;
		return core_handle_protocol(h, Protocol, Interface);
	}
	struct protocol *p = find_protocol(Protocol, 0);
	if (!p || list_empty(&p->ifaces))
		return EFI_NOT_FOUND;
	*Interface = CONTAINER_OF(p->ifaces.next, struct proto_iface, by_proto)->iface;
	return EFI_SUCCESS;
}

static const EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

/* the number of leading bytes of `path` that `prefix` (without its end node)
   matches node for node, or 0 if it doesn't */
static UINTN
dp_prefix(const EFI_DEVICE_PATH_PROTOCOL *prefix, const EFI_DEVICE_PATH_PROTOCOL *path)
{
	const UINT8 *start = (const UINT8 *)path;
	while (!dp_is_end(prefix)) {
		if (dp_is_end(path) || prefix->Type == END_DEVICE_PATH_TYPE)
			return 0;
		UINTN l = dp_node_len(prefix);
		if (l != dp_node_len(path) || memcmp(prefix, path, l))
			return 0;
		prefix = dp_next(prefix);
		path = dp_next(path);
	}
	return (UINTN)((const UINT8 *)path - start);
}

EFI_STATUS EFIAPI
core_locate_device_path(EFI_GUID *Protocol, EFI_DEVICE_PATH_PROTOCOL **DevicePath, EFI_HANDLE *Device)
{
	if (!Protocol || !DevicePath || !*DevicePath || !Device)
		return EFI_INVALID_PARAMETER;
	struct protocol *p = find_protocol(Protocol, 0);
	if (!p)
		return EFI_NOT_FOUND;
	EFI_HANDLE best = 0;
	UINTN best_len = 0;
	int found = 0;
	list_for_each(it, &p->ifaces) {
		struct handle *h = CONTAINER_OF(it, struct proto_iface, by_proto)->handle;
		struct proto_iface *dpi = find_iface(h, &dp_guid);
		if (!dpi)
			continue;
		UINTN l = dp_prefix(dpi->iface, *DevicePath);
		if (!l && !dp_is_end(dpi->iface))
			continue;
		if (!found || l > best_len) {
			best = h;
			best_len = l;
			found = 1;
		}
	}
	if (!found)
		return EFI_NOT_FOUND;
	*Device = best;
	*DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)*DevicePath + best_len);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_connect_controller(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
    EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath, BOOLEAN Recursive)
{
	return handle_valid(ControllerHandle) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI
core_disconnect_controller(EFI_HANDLE ControllerHandle, EFI_HANDLE DriverImageHandle,
    EFI_HANDLE ChildHandle)
{
	return handle_valid(ControllerHandle) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
}

static EFI_STATUS EFIAPI
core_install_multiple(EFI_HANDLE *Handle, ...)
{
	va_list ap;
	EFI_GUID *g[16];
	void *ifc[16];
	unsigned n = 0;
	if (!Handle)
		return EFI_INVALID_PARAMETER;
	va_start(ap, Handle);
	while (n < 16 && (g[n] = va_arg(ap, EFI_GUID *)))
		ifc[n] = va_arg(ap, void *), n++;
	va_end(ap);
	/* a device path already on another handle is an error (UEFI 2.10 7.3.16) */
	for (unsigned i = 0; i < n; i++)
		if (guid_eq(g[i], &dp_guid)) {
			EFI_DEVICE_PATH_PROTOCOL *dp = ifc[i];
			EFI_HANDLE h;
			if (core_locate_device_path((EFI_GUID *)&dp_guid, &dp, &h) == EFI_SUCCESS &&
			    dp_is_end(dp))
				return EFI_ALREADY_STARTED;
		}
	EFI_HANDLE h0 = *Handle;
	for (unsigned i = 0; i < n; i++) {
		EFI_STATUS s = core_install_protocol(Handle, g[i], EFI_NATIVE_INTERFACE, ifc[i]);
		if (EFI_ERROR(s)) {
			while (i--)
				core_uninstall_protocol(*Handle, g[i], ifc[i]);
			*Handle = h0;
			return s;
		}
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_uninstall_multiple(EFI_HANDLE Handle, ...)
{
	va_list ap;
	EFI_GUID *g;
	EFI_STATUS r = EFI_SUCCESS;
	va_start(ap, Handle);
	while ((g = va_arg(ap, EFI_GUID *))) {
		void *ifc = va_arg(ap, void *);
		if (EFI_ERROR(core_uninstall_protocol(Handle, g, ifc)))
			r = EFI_INVALID_PARAMETER;
	}
	va_end(ap);
	return r;
}

void
handle_init_services(EFI_BOOT_SERVICES *bs)
{
	bs->InstallProtocolInterface = core_install_protocol;
	bs->ReinstallProtocolInterface = core_reinstall_protocol;
	bs->UninstallProtocolInterface = core_uninstall_protocol;
	bs->HandleProtocol = core_handle_protocol;
	bs->RegisterProtocolNotify = core_register_protocol_notify;
	bs->LocateHandle = core_locate_handle;
	bs->LocateDevicePath = core_locate_device_path;
	bs->ConnectController = core_connect_controller;
	bs->DisconnectController = core_disconnect_controller;
	bs->OpenProtocol = core_open_protocol;
	bs->CloseProtocol = core_close_protocol;
	bs->OpenProtocolInformation = core_open_protocol_information;
	bs->ProtocolsPerHandle = core_protocols_per_handle;
	bs->LocateHandleBuffer = core_locate_handle_buffer;
	bs->LocateProtocol = core_locate_protocol;
	bs->InstallMultipleProtocolInterfaces = core_install_multiple;
	bs->UninstallMultipleProtocolInterfaces = core_uninstall_multiple;
}
