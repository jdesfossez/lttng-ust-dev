#ifndef _UST_ALIGN_H
#define _UST_ALIGN_H

/*
 * ust/align.h
 *
 * (C) Copyright 2010-2011 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <ust/bug.h>

/*
 * Align pointer on natural object alignment.
 */
#define object_align(obj)	PTR_ALIGN(obj, __alignof__(*(obj)))
#define object_align_floor(obj)	PTR_ALIGN_FLOOR(obj, __alignof__(*(obj)))

/**
 * offset_align - Calculate the offset needed to align an object on its natural
 *                alignment towards higher addresses.
 * @align_drift:  object offset from an "alignment"-aligned address.
 * @alignment:    natural object alignment. Must be non-zero, power of 2.
 *
 * Returns the offset that must be added to align towards higher
 * addresses.
 */
#define offset_align(align_drift, alignment)				       \
	({								       \
		BUILD_RUNTIME_BUG_ON((alignment) == 0			       \
				   || ((alignment) & ((alignment) - 1)));      \
		(((alignment) - (align_drift)) & ((alignment) - 1));	       \
	})

/**
 * offset_align_floor - Calculate the offset needed to align an object
 *                      on its natural alignment towards lower addresses.
 * @align_drift:  object offset from an "alignment"-aligned address.
 * @alignment:    natural object alignment. Must be non-zero, power of 2.
 *
 * Returns the offset that must be substracted to align towards lower addresses.
 */
#define offset_align_floor(align_drift, alignment)			       \
	({								       \
		BUILD_RUNTIME_BUG_ON((alignment) == 0			       \
				   || ((alignment) & ((alignment) - 1)));      \
		(((align_drift) - (alignment)) & ((alignment) - 1);	       \
	})

#endif /* _UST_ALIGN_H */