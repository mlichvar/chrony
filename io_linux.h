/* Taken from <asm-$foo/ioctl.h> in the Linux kernel sources.
 * The ioctl.h file is pretty similar from one architecture to another.
 * */
#ifndef IO_LINUX_H
#define IO_LINUX_H

/* Hmm.  These constants vary a bit between systems. */
/* (__sh__ includes both sh and sh64) */
#if defined(__i386__) || defined(__sh__)
#define CHRONY_IOC_NRBITS	8
#define CHRONY_IOC_TYPEBITS	8
#define CHRONY_IOC_SIZEBITS	14
#define CHRONY_IOC_DIRBITS	2

#define CHRONY_IOC_NONE	0U
#define CHRONY_IOC_WRITE	1U
#define CHRONY_IOC_READ	2U

#elif defined(__alpha__) || defined(__sparc__) || defined(__ppc__) || defined(__ppc64__) || defined(__sparc64__)
#define CHRONY_IOC_NRBITS	8
#define CHRONY_IOC_TYPEBITS	8
#define CHRONY_IOC_SIZEBITS	13
#define CHRONY_IOC_DIRBITS	2

#define CHRONY_IOC_NONE        1U
#define CHRONY_IOC_READ        2U
#define CHRONY_IOC_WRITE       4U

#elif defined(__mips__) || defined(__mips32__)
#define CHRONY_IOC_NRBITS       8
#define CHRONY_IOC_TYPEBITS     8
#define CHRONY_IOC_SIZEBITS     13
#define CHRONY_IOC_DIRBITS      3
#define CHRONY_IOC_NONE         1U
#define CHRONY_IOC_READ         2U
#define CHRONY_IOC_WRITE        4U

#else
#error "I don't know the values of the _IOC_* constants for your architecture"
#endif

#define CHRONY_IOC_NRMASK	((1 << CHRONY_IOC_NRBITS)-1)
#define CHRONY_IOC_TYPEMASK	((1 << CHRONY_IOC_TYPEBITS)-1)
#define CHRONY_IOC_SIZEMASK	((1 << CHRONY_IOC_SIZEBITS)-1)
#define CHRONY_IOC_DIRMASK	((1 << CHRONY_IOC_DIRBITS)-1)

#define CHRONY_IOC_NRSHIFT	0
#define CHRONY_IOC_TYPESHIFT	(CHRONY_IOC_NRSHIFT+CHRONY_IOC_NRBITS)
#define CHRONY_IOC_SIZESHIFT	(CHRONY_IOC_TYPESHIFT+CHRONY_IOC_TYPEBITS)
#define CHRONY_IOC_DIRSHIFT	(CHRONY_IOC_SIZESHIFT+CHRONY_IOC_SIZEBITS)

#define CHRONY_IOC(dir,type,nr,size) \
	(((dir)  << CHRONY_IOC_DIRSHIFT) | \
	 ((type) << CHRONY_IOC_TYPESHIFT) | \
	 ((nr)   << CHRONY_IOC_NRSHIFT) | \
	 ((size) << CHRONY_IOC_SIZESHIFT))

/* used to create numbers */
#define CHRONY_IO(type,nr)		CHRONY_IOC(CHRONY_IOC_NONE,(type),(nr),0)
#define CHRONY_IOR(type,nr,size)	CHRONY_IOC(CHRONY_IOC_READ,(type),(nr),sizeof(size))
#define CHRONY_IOW(type,nr,size)	CHRONY_IOC(CHRONY_IOC_WRITE,(type),(nr),sizeof(size))
#define CHRONY_IOWR(type,nr,size)	CHRONY_IOC(CHRONY_IOC_READ|CHRONY_IOC_WRITE,(type),(nr),sizeof(size))

#define RTC_UIE_ON	CHRONY_IO('p', 0x03)	/* Update int. enable on	*/
#define RTC_UIE_OFF	CHRONY_IO('p', 0x04)	/* ... off			*/

#define RTC_RD_TIME	CHRONY_IOR('p', 0x09, struct rtc_time) /* Read RTC time   */
#define RTC_SET_TIME	CHRONY_IOW('p', 0x0a, struct rtc_time) /* Set RTC time    */

/* From mc146818.h */
#define RTC_UIE 0x10		/* update-finished interrupt enable */

#endif

