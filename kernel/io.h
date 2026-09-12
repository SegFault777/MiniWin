#ifndef IO_H
#define IO_H
/* The absolute bedrock of this whole operation: talking to hardware
 * ports directly, because we don't have an OS underneath us to do it for
 * us. Everything else in this kernel is built on these few lines. No
 * pressure. */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int             i32;

static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline u16 inw(u16 port) {
    u16 ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* 32-bit port I/O -- PCI configuration space access (0xCF8/0xCFC) needs
 * these; nothing before this point in the kernel did. */
static inline void outl(u16 port, u32 val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u32 inl(u16 port) {
    u32 ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    /* Classic trick: write garbage to an unused port (0x80, usually POST
     * codes) just to burn a few cycles so slow old hardware can catch its
     * breath. Nobody actually reads what we write here. It's a nap, not
     * a message. */
    outb(0x80, 0);
}

#endif
