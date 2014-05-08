/**
 * $Id$
 * Copyright (C) 2008 - 2014 Nils Asmussen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

#include <sys/common.h>
#include <sys/semaphore.h>

#ifdef __i386__
#include <sys/arch/i586/intrptstackframe.h>
#endif
#ifdef __eco32__
#include <sys/arch/eco32/intrptstackframe.h>
#endif
#ifdef __mmix__
#include <sys/arch/mmix/intrptstackframe.h>
#endif

#define PANIC_ON_PAGEFAULT	1

class OStream;
class Thread;

class InterruptsBase {
	InterruptsBase() = delete;

protected:
	struct Interrupt {
		irqhandler_func handler;
		char name[32];
		ulong count;
	};

public:
	static const size_t IRQ_SEM_COUNT	= 32;

	/**
	 * Initializes the interrupts
	 */
	static void init();

	/**
	 * @return the total number of interrupts so far
	 */
	static size_t getCount();

	/**
	 * @param irq the interrupt-number
	 * @return the vector for the given interrupt or -1 if unknown
	 */
	static int getVectorFor(uint8_t irq);

	/**
	 * Handles an interrupt
	 *
	 * @param number the interrupt-number
	 */
	static void handler(IntrptStackFrame *stack) asm("intrpt_handler");

	/**
	 * Installs a handler for given IRQ.
	 *
	 * @param irq the IRQ number
	 * @param name the name (just for debugging purposes)
	 * @return 0 on success
	 */
	static int installHandler(int irq,const char *name);

	/**
	 * Uninstalls the handler for given IRQ.
	 *
	 * @param irq the IRQ number
	 */
	static void uninstallHandler(int irq);

	/**
	 * Attaches the given semaphore to the given IRQ.
	 *
	 * @param sem the semaphore
	 * @param irq the IRQ
	 * @param name the name for the IRQ
	 * @return 0 on success
	 */
	static int attachSem(Semaphore *sem,size_t irq,const char *name);

	/**
	 * Detaches the given semaphore from the given IRQ.
	 *
	 * @param sem the semaphore
	 * @param irq the IRQ
	 */
	static void detachSem(Semaphore *sem,size_t irq);

	/**
	 * Fires the given IRQ, i.e. up's all attached semaphores.
	 *
	 * @param irq the IRQ
	 * @return true if at least one semaphore has been up'ed
	 */
	static bool fireIrq(size_t irq);

	/**
	 * Prints the given interrupt-stack
	 *
	 * @param os the output-stream
	 * @param stack the interrupt-stack
	 */
	static void printStackFrame(OStream &os,const IntrptStackFrame *stack);

	/**
	 * Prints statistics about the occurred interrupts
	 *
	 * @param os the output-stream
	 */
	static void print(OStream &os);

protected:
	static Interrupt intrptList[];
	static SpinLock userIrqsLock;
	static ISList<Semaphore*> userIrqs[];
};

#ifdef __i386__
#include <sys/arch/i586/interrupts.h>
#endif
#ifdef __eco32__
#include <sys/arch/eco32/interrupts.h>
#endif
#ifdef __mmix__
#include <sys/arch/mmix/interrupts.h>
#endif
