/*
 * Copyright 2013 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <core/os.h>
#include <core/event.h>

void
nouveau_event_put(struct nouveau_eventh *handler)
{
	struct nouveau_event *event = handler->event;
	unsigned long flags;
	u32 m, t;

	if (!__test_and_clear_bit(NVKM_EVENT_ENABLE, &handler->flags))
		return;

	spin_lock_irqsave(&event->refs_lock, flags);
	for (m = handler->types; t = __ffs(m), m; m &= ~(1 << t)) {
		if (!--event->refs[handler->index * event->types_nr + t]) {
			if (event->disable)
				event->disable(event, 1 << t, handler->index);
		}

	}
	spin_unlock_irqrestore(&event->refs_lock, flags);
}

void
nouveau_event_get(struct nouveau_eventh *handler)
{
	struct nouveau_event *event = handler->event;
	unsigned long flags;
	u32 m, t;

	if (__test_and_set_bit(NVKM_EVENT_ENABLE, &handler->flags))
		return;

	spin_lock_irqsave(&event->refs_lock, flags);
	for (m = handler->types; t = __ffs(m), m; m &= ~(1 << t)) {
		if (!event->refs[handler->index * event->types_nr + t]++) {
			if (event->enable)
				event->enable(event, 1 << t, handler->index);
		}

	}
	spin_unlock_irqrestore(&event->refs_lock, flags);
}

static void
nouveau_event_fini(struct nouveau_eventh *handler)
{
	struct nouveau_event *event = handler->event;
	unsigned long flags;
	nouveau_event_put(handler);
	spin_lock_irqsave(&event->list_lock, flags);
	list_del(&handler->head);
	spin_unlock_irqrestore(&event->list_lock, flags);
}

static int
nouveau_event_init(struct nouveau_event *event, u32 types, int index,
		   int (*func)(void *, u32, int), void *priv,
		   struct nouveau_eventh *handler)
{
	unsigned long flags;

	if (types & ~((1 << event->types_nr) - 1))
		return -EINVAL;
	if (index >= event->index_nr)
		return -EINVAL;

	handler->event = event;
	handler->flags = 0;
	handler->types = types;
	handler->index = index;
	handler->func = func;
	handler->priv = priv;

	spin_lock_irqsave(&event->list_lock, flags);
	list_add_tail(&handler->head, &event->list[index]);
	spin_unlock_irqrestore(&event->list_lock, flags);
	return 0;
}

int
nouveau_event_new(struct nouveau_event *event, u32 types, int index,
		  int (*func)(void *, u32, int), void *priv,
		  struct nouveau_eventh **phandler)
{
	struct nouveau_eventh *handler;
	int ret = -ENOMEM;

	if (event->check) {
		ret = event->check(event, types, index);
		if (ret)
			return ret;
	}

	handler = *phandler = kmalloc(sizeof(*handler), GFP_KERNEL);
	if (handler) {
		ret = nouveau_event_init(event, types, index, func, priv, handler);
		if (ret)
			kfree(handler);
	}

	return ret;
}

void
nouveau_event_ref(struct nouveau_eventh *handler, struct nouveau_eventh **ref)
{
	BUG_ON(handler != NULL);
	if (*ref) {
		nouveau_event_fini(*ref);
		kfree(*ref);
	}
	*ref = handler;
}

void
nouveau_event_trigger(struct nouveau_event *event, u32 types, int index)
{
	struct nouveau_eventh *handler;
	unsigned long flags;

	if (WARN_ON(index >= event->index_nr))
		return;

	spin_lock_irqsave(&event->list_lock, flags);
	list_for_each_entry(handler, &event->list[index], head) {
		if (!test_bit(NVKM_EVENT_ENABLE, &handler->flags))
			continue;
		if (!(handler->types & types))
			continue;
		if (handler->func(handler->priv, handler->types & types, index)
				!= NVKM_EVENT_DROP)
			continue;
		nouveau_event_put(handler);
	}
	spin_unlock_irqrestore(&event->list_lock, flags);
}

void
nouveau_event_destroy(struct nouveau_event **pevent)
{
	struct nouveau_event *event = *pevent;
	if (event) {
		kfree(event);
		*pevent = NULL;
	}
}

int
nouveau_event_create(int types_nr, int index_nr, struct nouveau_event **pevent)
{
	struct nouveau_event *event;
	int i;

	event = *pevent = kzalloc(sizeof(*event) + (index_nr * types_nr) *
				  sizeof(event->refs[0]), GFP_KERNEL);
	if (!event)
		return -ENOMEM;

	event->list = kmalloc(sizeof(*event->list) * index_nr, GFP_KERNEL);
	if (!event->list) {
		kfree(event);
		return -ENOMEM;
	}

	spin_lock_init(&event->list_lock);
	spin_lock_init(&event->refs_lock);
	for (i = 0; i < index_nr; i++)
		INIT_LIST_HEAD(&event->list[i]);
	event->types_nr = types_nr;
	event->index_nr = index_nr;
	return 0;
}
