/*
 * DXE core: events, timers and task priority levels (UEFI 2.10 7.1).
 *
 * The firmware runs with interrupts disabled: there is no timer interrupt.
 * Timers are checked against the TSC whenever the core is entered in a way
 * that may wait or lower the TPL (WaitForEvent, CheckEvent, RestoreTPL,
 * Stall), which is where UEFI code waits; notification functions run then,
 * in TPL order, as the specification requires.
 */
#include "dxe.h"

#define EVENT_MAGIC	0x544E5645u	/* "EVNT" */

struct event {
	uint32_t magic;
	UINT32 type;
	EFI_TPL tpl;
	EFI_EVENT_NOTIFY fn;
	void *ctx;
	EFI_GUID group;
	int has_group;
	int signaled;
	int queued;
	uint64_t trigger, period;	/* TSC; trigger 0 = not armed */
	list_t link;			/* all events */
	list_t qlink;			/* the notify queue */
};

static list_t events = { &events, &events };
static list_t queue = { &queue, &queue };
static EFI_TPL cur_tpl = TPL_APPLICATION;
static int in_tick;

static int
valid(struct event *e)
{
	return e && e->magic == EVENT_MAGIC;
}

static void
queue_notify(struct event *e)
{
	if (e->queued || !e->fn)
		return;
	e->queued = 1;
	list_add_tail(&queue, &e->qlink);
}

/* run the queued notifications whose TPL is above the current one */
static void
dispatch(void)
{
	for (;;) {
		struct event *best = 0;
		list_for_each(it, &queue) {
			struct event *e = CONTAINER_OF(it, struct event, qlink);
			if (e->tpl > cur_tpl && (!best || e->tpl > best->tpl))
				best = e;
		}
		if (!best)
			return;
		list_del(&best->qlink);
		best->queued = 0;
		EFI_TPL saved = cur_tpl;
		cur_tpl = best->tpl;
		if (best->type & EVT_NOTIFY_SIGNAL)
			best->signaled = 0;	/* signal events are consumed by their notification */
		best->fn(best, best->ctx);
		cur_tpl = saved;
	}
}

EFI_TPL EFIAPI
core_raise_tpl(EFI_TPL NewTpl)
{
	EFI_TPL old = cur_tpl;
	if (NewTpl > cur_tpl)
		cur_tpl = NewTpl;
	return old;
}

VOID EFIAPI
core_restore_tpl(EFI_TPL OldTpl)
{
	if (OldTpl > cur_tpl)
		return;
	cur_tpl = OldTpl;
	if (cur_tpl < TPL_HIGH_LEVEL)
		event_timer_tick();
	dispatch();
}

static void
signal_one(struct event *e)
{
	if (e->signaled && !(e->type & EVT_NOTIFY_SIGNAL))
		return;
	e->signaled = 1;
	if (e->type & EVT_NOTIFY_SIGNAL)
		queue_notify(e);
}

void
event_signal_group(const EFI_GUID *group)
{
	list_for_each(it, &events) {
		struct event *e = CONTAINER_OF(it, struct event, link);
		if (e->has_group && guid_eq(&e->group, group))
			signal_one(e);
	}
	dispatch();
}

EFI_STATUS EFIAPI
core_signal_event(EFI_EVENT Event)
{
	struct event *e = Event;
	if (!valid(e))
		return EFI_INVALID_PARAMETER;
	if (e->has_group) {
		event_signal_group(&e->group);
		return EFI_SUCCESS;
	}
	signal_one(e);
	dispatch();
	return EFI_SUCCESS;
}

void
event_timer_tick(void)
{
	if (in_tick)
		return;
	in_tick = 1;
	uint64_t now = rdtsc();
	list_for_each(it, &events) {
		struct event *e = CONTAINER_OF(it, struct event, link);
		if (!(e->type & EVT_TIMER) || !e->trigger || now < e->trigger)
			continue;
		e->trigger = e->period ? now + e->period : 0;
		signal_one(e);
	}
	in_tick = 0;
}

EFI_STATUS EFIAPI
core_create_event_ex(UINT32 Type, EFI_TPL NotifyTpl, EFI_EVENT_NOTIFY NotifyFunction,
    const VOID *NotifyContext, const EFI_GUID *EventGroup, EFI_EVENT *Event)
{
	static const EFI_GUID ebs = EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
	if (!Event)
		return EFI_INVALID_PARAMETER;
	if (Type == EVT_SIGNAL_EXIT_BOOT_SERVICES) {
		if (EventGroup)
			return EFI_INVALID_PARAMETER;
		EventGroup = &ebs;
		Type = EVT_NOTIFY_SIGNAL;
	} else if (Type == EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE) {
		Type = EVT_NOTIFY_SIGNAL | EVT_RUNTIME;
	}
	if ((Type & (EVT_NOTIFY_SIGNAL | EVT_NOTIFY_WAIT)) == (EVT_NOTIFY_SIGNAL | EVT_NOTIFY_WAIT))
		return EFI_INVALID_PARAMETER;
	if (Type & (EVT_NOTIFY_SIGNAL | EVT_NOTIFY_WAIT)) {
		if (!NotifyFunction || NotifyTpl <= TPL_APPLICATION || NotifyTpl >= TPL_HIGH_LEVEL)
			return EFI_INVALID_PARAMETER;
	}
	struct event *e = core_alloc(sizeof *e);
	if (!e)
		return EFI_OUT_OF_RESOURCES;
	e->magic = EVENT_MAGIC;
	e->type = Type;
	e->tpl = NotifyTpl;
	e->fn = (Type & (EVT_NOTIFY_SIGNAL | EVT_NOTIFY_WAIT)) ? NotifyFunction : 0;
	e->ctx = (void *)NotifyContext;
	if (EventGroup) {
		e->group = *EventGroup;
		e->has_group = 1;
	}
	list_init(&e->qlink);
	list_add_tail(&events, &e->link);
	*Event = e;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_create_event(UINT32 Type, EFI_TPL NotifyTpl, EFI_EVENT_NOTIFY NotifyFunction, VOID *NotifyContext,
    EFI_EVENT *Event)
{
	return core_create_event_ex(Type, NotifyTpl, NotifyFunction, NotifyContext, 0, Event);
}

static EFI_STATUS EFIAPI
core_set_timer(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 TriggerTime)
{
	struct event *e = Event;
	if (!valid(e) || !(e->type & EVT_TIMER) || Type > TimerRelative)
		return EFI_INVALID_PARAMETER;
	/* TriggerTime is in 100 ns units */
	uint64_t ticks = TriggerTime * (TSC_HZ / 1000000) / 10;
	if (Type == TimerCancel) {
		e->trigger = 0;
		return EFI_SUCCESS;
	}
	if (!ticks)
		ticks = 1;
	e->period = Type == TimerPeriodic ? ticks : 0;
	e->trigger = rdtsc() + ticks;
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_close_event(EFI_EVENT Event)
{
	struct event *e = Event;
	if (!valid(e))
		return EFI_INVALID_PARAMETER;
	if (e->queued)
		list_del(&e->qlink);
	list_del(&e->link);
	e->magic = 0;
	core_free(e);
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
core_check_event(EFI_EVENT Event)
{
	struct event *e = Event;
	if (!valid(e) || (e->type & EVT_NOTIFY_SIGNAL))
		return EFI_INVALID_PARAMETER;
	event_timer_tick();
	dispatch();
	if (!e->signaled && (e->type & EVT_NOTIFY_WAIT) && e->fn) {
		EFI_TPL saved = cur_tpl;
		if (e->tpl > cur_tpl) {
			cur_tpl = e->tpl;
			e->fn(e, e->ctx);
			cur_tpl = saved;
		}
	}
	if (e->signaled) {
		e->signaled = 0;
		return EFI_SUCCESS;
	}
	return EFI_NOT_READY;
}

static EFI_STATUS EFIAPI
core_wait_for_event(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index)
{
	if (!NumberOfEvents || !Event || !Index)
		return EFI_INVALID_PARAMETER;
	if (cur_tpl != TPL_APPLICATION)
		return EFI_UNSUPPORTED;
	for (;;) {
		for (UINTN i = 0; i < NumberOfEvents; i++) {
			EFI_STATUS s = core_check_event(Event[i]);
			if (s != EFI_NOT_READY) {
				*Index = i;
				return s;
			}
		}
		cpu_pause();
	}
}

void
event_init_services(EFI_BOOT_SERVICES *bs)
{
	bs->RaiseTPL = core_raise_tpl;
	bs->RestoreTPL = core_restore_tpl;
	bs->CreateEvent = core_create_event;
	bs->CreateEventEx = core_create_event_ex;
	bs->SetTimer = core_set_timer;
	bs->WaitForEvent = core_wait_for_event;
	bs->SignalEvent = core_signal_event;
	bs->CloseEvent = core_close_event;
	bs->CheckEvent = core_check_event;
}
