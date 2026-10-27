/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_CORO_H
#define MW_CORO_H

/* coro.h -- one function: switch the CPU from one stack to another. That is all a cooperative "thread" needs.
 *
 * mw_coro_switch(&from, to) pushes the callee-saved registers (the cdecl ABI says a function may use eax/ecx/edx freely, so only
 * ebp, ebx, esi, edi have to survive), stores the stack pointer into *from, loads `to` as the new stack pointer, pops the other
 * context's registers and returns -- on ITS stack, into whatever it was doing when it last switched away.
 *
 * Starting a context that has never run: coro_prepare() builds the stack it would have had if it had switched away from the first
 * instruction of `entry`: four zeroed register slots, then `entry` as the return address, so the first switch "returns" into it.
 * `entry` must never return (there is nothing under it); progwin.h's trampoline switches away for good instead.
 *
 * Needs nothing: 32-bit x86, the kernel's one translation unit. Not preemptive: a context runs until it switches. */
void mw_coro_switch(u32 *from_esp, u32 to_esp);
__asm__(
    ".text\n"
    ".globl mw_coro_switch\n"
    "mw_coro_switch:\n"
    "    movl 4(%esp), %eax\n"          /* where to store the old stack pointer */
    "    movl 8(%esp), %edx\n"          /* the new stack pointer */
    "    pushl %ebp\n    pushl %ebx\n    pushl %esi\n    pushl %edi\n"
    "    movl %esp, (%eax)\n"
    "    movl %edx, %esp\n"
    "    popl %edi\n    popl %esi\n    popl %ebx\n    popl %ebp\n"
    "    ret\n"
);

/* The stack pointer value to hand to mw_coro_switch() as `to` for a fresh context that will start in `entry`. */
static u32 coro_prepare(void *stack_base, u32 stack_bytes, void (*entry)(void)) {
    u32 *top = (u32 *)((u8 *)stack_base + stack_bytes);
    *--top = 0;                          /* a return address for `entry` itself (never used: it must not return) */
    *--top = (u32)(unsigned long)entry;  /* what the first switch's `ret` pops */
    *--top = 0; *--top = 0; *--top = 0; *--top = 0;     /* ebp ebx esi edi, popped in the opposite order they are pushed */
    return (u32)(unsigned long)top;
}

#endif
