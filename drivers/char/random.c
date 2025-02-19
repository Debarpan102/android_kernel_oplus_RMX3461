// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * Copyright (C) 2017-2022 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 * Copyright Matt Mackall <mpm@selenic.com>, 2003, 2004, 2005
 * Copyright Theodore Ts'o, 1994, 1995, 1996, 1997, 1998, 1999. All rights reserved.
 *
 * This driver produces cryptographically secure pseudorandom data. It is divided
 * into roughly six sections, each with a section header:
 *
 *   - Initialization and readiness waiting.
 *   - Fast key erasure RNG, the "crng".
 *   - Entropy accumulation and extraction routines.
 *   - Entropy collection routines.
 *   - Userspace reader/writer interfaces.
 *   - Sysctl interface.
 *
 * The high level overview is that there is one input pool, into which
 * various pieces of data are hashed. Prior to initialization, some of that
 * data is then "credited" as having a certain number of bits of entropy.
 * When enough bits of entropy are available, the hash is finalized and
 * handed as a key to a stream cipher that expands it indefinitely for
 * various consumers. This key is periodically refreshed as the various
 * entropy collectors, described below, add data to the input pool.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/nodemask.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/percpu.h>
#include <linux/ptrace.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/ratelimit.h>
#include <linux/syscalls.h>
#include <linux/completion.h>
#include <linux/uuid.h>
#include <linux/uaccess.h>
#include <linux/siphash.h>
#include <linux/uio.h>
#include <crypto/chacha.h>
#include <crypto/blake2s.h>
#include <asm/processor.h>
#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/io.h>

#define CREATE_TRACE_POINTS
#include <trace/events/random.h>

/* #define ADD_INTERRUPT_BENCH */

/*
 * Configuration information
 */
#define INPUT_POOL_SHIFT	12
#define INPUT_POOL_WORDS	(1 << (INPUT_POOL_SHIFT-5))
#define OUTPUT_POOL_SHIFT	10
#define OUTPUT_POOL_WORDS	(1 << (OUTPUT_POOL_SHIFT-5))
#define EXTRACT_SIZE		10


#define LONGS(x) (((x) + sizeof(unsigned long) - 1)/sizeof(unsigned long))

/*
 * To allow fractional bits to be tracked, the entropy_count field is
 * denominated in units of 1/8th bits.
 *
 * 2*(ENTROPY_SHIFT + poolbitshift) must <= 31, or the multiply in
 * credit_entropy_bits() needs to be 64 bits wide.
 */
#define ENTROPY_SHIFT 3
#define ENTROPY_BITS(r) ((r)->entropy_count >> ENTROPY_SHIFT)

/*
 * If the entropy count falls under this number of bits, then we
 * should wake up processes which are selecting or polling on write
 * access to /dev/random.
 */
static int random_write_wakeup_bits = 28 * OUTPUT_POOL_WORDS;

/*
 * Originally, we used a primitive polynomial of degree .poolwords
 * over GF(2).  The taps for various sizes are defined below.  They
 * were chosen to be evenly spaced except for the last tap, which is 1
 * to get the twisting happening as fast as possible.
 *
 * For the purposes of better mixing, we use the CRC-32 polynomial as
 * well to make a (modified) twisted Generalized Feedback Shift
 * Register.  (See M. Matsumoto & Y. Kurita, 1992.  Twisted GFSR
 * generators.  ACM Transactions on Modeling and Computer Simulation
 * 2(3):179-194.  Also see M. Matsumoto & Y. Kurita, 1994.  Twisted
 * GFSR generators II.  ACM Transactions on Modeling and Computer
 * Simulation 4:254-266)
 *
 * Thanks to Colin Plumb for suggesting this.
 *
 * The mixing operation is much less sensitive than the output hash,
 * where we use SHA-1.  All that we want of mixing operation is that
 * it be a good non-cryptographic hash; i.e. it not produce collisions
 * when fed "random" data of the sort we expect to see.  As long as
 * the pool state differs for different inputs, we have preserved the
 * input entropy and done a good job.  The fact that an intelligent
 * attacker can construct inputs that will produce controlled
 * alterations to the pool's state is not important because we don't
 * consider such inputs to contribute any randomness.  The only
 * property we need with respect to them is that the attacker can't
 * increase his/her knowledge of the pool's state.  Since all
 * additions are reversible (knowing the final state and the input,
 * you can reconstruct the initial state), if an attacker has any
 * uncertainty about the initial state, he/she can only shuffle that
 * uncertainty about, but never cause any collisions (which would
 * decrease the uncertainty).
 *
 * Our mixing functions were analyzed by Lacharme, Roeck, Strubel, and
 * Videau in their paper, "The Linux Pseudorandom Number Generator
 * Revisited" (see: http://eprint.iacr.org/2012/251.pdf).  In their
 * paper, they point out that we are not using a true Twisted GFSR,
 * since Matsumoto & Kurita used a trinomial feedback polynomial (that
 * is, with only three taps, instead of the six that we are using).
 * As a result, the resulting polynomial is neither primitive nor
 * irreducible, and hence does not have a maximal period over
 * GF(2**32).  They suggest a slight change to the generator
 * polynomial which improves the resulting TGFSR polynomial to be
 * irreducible, which we have made here.
 */
static const struct poolinfo {
	int poolbitshift, poolwords, poolbytes, poolfracbits;
#define S(x) ilog2(x)+5, (x), (x)*4, (x) << (ENTROPY_SHIFT+5)
	int tap1, tap2, tap3, tap4, tap5;
} poolinfo_table[] = {
	/* was: x^128 + x^103 + x^76 + x^51 +x^25 + x + 1 */
	/* x^128 + x^104 + x^76 + x^51 +x^25 + x + 1 */
	{ S(128),	104,	76,	51,	25,	1 },
};

/*
 * Static global variables
 */
static DECLARE_WAIT_QUEUE_HEAD(random_write_wait);
static struct fasync_struct *fasync;

static DEFINE_SPINLOCK(random_ready_list_lock);
static LIST_HEAD(random_ready_list);

struct crng_state {
	__u32		state[16];
	unsigned long	init_time;
	spinlock_t	lock;
};

static struct crng_state primary_crng = {
	.lock = __SPIN_LOCK_UNLOCKED(primary_crng.lock),
};

/*
 * crng_init =  0 --> Uninitialized
 *		1 --> Initialized
 *		2 --> Initialized from input_pool
 *
 * crng_init is protected by primary_crng->lock, and only increases
 * its value (from 0->1->2).
 */
static int crng_init = 0;
#define crng_ready() (likely(crng_init > 1))
static int crng_init_cnt = 0;
static unsigned long crng_global_init_time = 0;
#define CRNG_INIT_CNT_THRESH (2*CHACHA_KEY_SIZE)
static void _extract_crng(struct crng_state *crng, __u8 out[CHACHA_BLOCK_SIZE]);
static void _crng_backtrack_protect(struct crng_state *crng,
				    __u8 tmp[CHACHA_BLOCK_SIZE], int used);
static void process_random_ready_list(void);
static void _get_random_bytes(void *buf, int nbytes);

static struct ratelimit_state unseeded_warning =
	RATELIMIT_STATE_INIT("warn_unseeded_randomness", HZ, 3);
static struct ratelimit_state urandom_warning =
	RATELIMIT_STATE_INIT("warn_urandom_randomness", HZ, 3);

static int ratelimit_disable __read_mostly;

module_param_named(ratelimit_disable, ratelimit_disable, int, 0644);
MODULE_PARM_DESC(ratelimit_disable, "Disable random ratelimit suppression");

/**********************************************************************
 *
 * OS independent entropy store.   Here are the functions which handle
 * storing entropy in an entropy pool.
 *
 **********************************************************************/

struct entropy_store;
struct entropy_store {
	/* read-only data: */
	const struct poolinfo *poolinfo;
	__u32 *pool;
	const char *name;

	/* read-write data: */
	spinlock_t lock;
	unsigned short add_ptr;
	unsigned short input_rotate;
	int entropy_count;
	unsigned int initialized:1;
	unsigned int last_data_init:1;
	__u8 last_data[EXTRACT_SIZE];
};

static ssize_t extract_entropy(struct entropy_store *r, void *buf,
			       size_t nbytes, int min, int rsvd);
static ssize_t _extract_entropy(struct entropy_store *r, void *buf,
				size_t nbytes, int fips);

static void crng_reseed(struct crng_state *crng, struct entropy_store *r);
static __u32 input_pool_data[INPUT_POOL_WORDS] __latent_entropy;

static struct entropy_store input_pool = {
	.poolinfo = &poolinfo_table[0],
	.name = "input",
	.lock = __SPIN_LOCK_UNLOCKED(input_pool.lock),
	.pool = input_pool_data
};

static __u32 const twist_table[8] = {
	0x00000000, 0x3b6e20c8, 0x76dc4190, 0x4db26158,
	0xedb88320, 0xd6d6a3e8, 0x9b64c2b0, 0xa00ae278 };

/*
 * This function adds bytes into the entropy "pool".  It does not
 * update the entropy estimate.  The caller should call
 * credit_entropy_bits if this is appropriate.
 *
 * The pool is stirred with a primitive polynomial of the appropriate
 * degree, and then twisted.  We twist by three bits at a time because
 * it's cheap to do so and helps slightly in the expected case where
 * the entropy is concentrated in the low-order bits.
 */
static void _mix_pool_bytes(struct entropy_store *r, const void *in,
			    int nbytes)
{
	unsigned long i, tap1, tap2, tap3, tap4, tap5;
	int input_rotate;
	int wordmask = r->poolinfo->poolwords - 1;
	const char *bytes = in;
	__u32 w;

	tap1 = r->poolinfo->tap1;
	tap2 = r->poolinfo->tap2;
	tap3 = r->poolinfo->tap3;
	tap4 = r->poolinfo->tap4;
	tap5 = r->poolinfo->tap5;

	input_rotate = r->input_rotate;
	i = r->add_ptr;

	/* mix one byte at a time to simplify size handling and churn faster */
	while (nbytes--) {
		w = rol32(*bytes++, input_rotate);
		i = (i - 1) & wordmask;

		/* XOR in the various taps */
		w ^= r->pool[i];
		w ^= r->pool[(i + tap1) & wordmask];
		w ^= r->pool[(i + tap2) & wordmask];
		w ^= r->pool[(i + tap3) & wordmask];
		w ^= r->pool[(i + tap4) & wordmask];
		w ^= r->pool[(i + tap5) & wordmask];

		/* Mix the result back in with a twist */
		r->pool[i] = (w >> 3) ^ twist_table[w & 7];

		/*
		 * Normally, we add 7 bits of rotation to the pool.
		 * At the beginning of the pool, add an extra 7 bits
		 * rotation, so that successive passes spread the
		 * input bits across the pool evenly.
		 */
		input_rotate = (input_rotate + (i ? 7 : 14)) & 31;
	}

	r->input_rotate = input_rotate;
	r->add_ptr = i;
}

static void __mix_pool_bytes(struct entropy_store *r, const void *in,
			     int nbytes)
{
	trace_mix_pool_bytes_nolock(r->name, nbytes, _RET_IP_);
	_mix_pool_bytes(r, in, nbytes);
}

static void mix_pool_bytes(struct entropy_store *r, const void *in,
			   int nbytes)
{
	unsigned long flags;

	trace_mix_pool_bytes(r->name, nbytes, _RET_IP_);
	spin_lock_irqsave(&r->lock, flags);
	_mix_pool_bytes(r, in, nbytes);
	spin_unlock_irqrestore(&r->lock, flags);
}

struct fast_pool {
	__u32		pool[4];
	unsigned long	last;
	unsigned short	reg_idx;
	unsigned char	count;
};

/*
 * This is a fast mixing routine used by the interrupt randomness
 * collector.  It's hardcoded for an 128 bit pool and assumes that any
 * locks that might be needed are taken by the caller.
 */
static void fast_mix(struct fast_pool *f)
{
	__u32 a = f->pool[0],	b = f->pool[1];
	__u32 c = f->pool[2],	d = f->pool[3];

	a += b;			c += d;
	b = rol32(b, 6);	d = rol32(d, 27);
	d ^= a;			b ^= c;

	a += b;			c += d;
	b = rol32(b, 16);	d = rol32(d, 14);
	d ^= a;			b ^= c;

	a += b;			c += d;
	b = rol32(b, 6);	d = rol32(d, 27);
	d ^= a;			b ^= c;

	a += b;			c += d;
	b = rol32(b, 16);	d = rol32(d, 14);
	d ^= a;			b ^= c;

	f->pool[0] = a;  f->pool[1] = b;
	f->pool[2] = c;  f->pool[3] = d;
	f->count++;
}

static void process_random_ready_list(void)
{
	unsigned long flags;
	struct random_ready_callback *rdy, *tmp;

	spin_lock_irqsave(&random_ready_list_lock, flags);
	list_for_each_entry_safe(rdy, tmp, &random_ready_list, list) {
		struct module *owner = rdy->owner;

		list_del_init(&rdy->list);
		rdy->func(rdy);
		module_put(owner);
	}
	spin_unlock_irqrestore(&random_ready_list_lock, flags);
}

/*
 * Credit (or debit) the entropy store with n bits of entropy.
 * Use credit_entropy_bits_safe() if the value comes from userspace
 * or otherwise should be checked for extreme values.
 */
static void credit_entropy_bits(struct entropy_store *r, int nbits)
{
	int entropy_count, orig, has_initialized = 0;
	const int pool_size = r->poolinfo->poolfracbits;
	int nfrac = nbits << ENTROPY_SHIFT;

	if (!nbits)
		return;

retry:
	entropy_count = orig = READ_ONCE(r->entropy_count);
	if (nfrac < 0) {
		/* Debit */
		entropy_count += nfrac;
	} else {
		/*
		 * Credit: we have to account for the possibility of
		 * overwriting already present entropy.	 Even in the
		 * ideal case of pure Shannon entropy, new contributions
		 * approach the full value asymptotically:
		 *
		 * entropy <- entropy + (pool_size - entropy) *
		 *	(1 - exp(-add_entropy/pool_size))
		 *
		 * For add_entropy <= pool_size/2 then
		 * (1 - exp(-add_entropy/pool_size)) >=
		 *    (add_entropy/pool_size)*0.7869...
		 * so we can approximate the exponential with
		 * 3/4*add_entropy/pool_size and still be on the
		 * safe side by adding at most pool_size/2 at a time.
		 *
		 * The use of pool_size-2 in the while statement is to
		 * prevent rounding artifacts from making the loop
		 * arbitrarily long; this limits the loop to log2(pool_size)*2
		 * turns no matter how large nbits is.
		 */
		int pnfrac = nfrac;
		const int s = r->poolinfo->poolbitshift + ENTROPY_SHIFT + 2;
		/* The +2 corresponds to the /4 in the denominator */

		do {
			unsigned int anfrac = min(pnfrac, pool_size/2);
			unsigned int add =
				((pool_size - entropy_count)*anfrac*3) >> s;

			entropy_count += add;
			pnfrac -= anfrac;
		} while (unlikely(entropy_count < pool_size-2 && pnfrac));
	}

	if (WARN_ON(entropy_count < 0)) {
		pr_warn("negative entropy/overflow: pool %s count %d\n",
			r->name, entropy_count);
		entropy_count = 0;
	} else if (entropy_count > pool_size)
		entropy_count = pool_size;
	if (cmpxchg(&r->entropy_count, orig, entropy_count) != orig)
		goto retry;

	if (has_initialized) {
		r->initialized = 1;
		kill_fasync(&fasync, SIGIO, POLL_IN);
	}

	trace_credit_entropy_bits(r->name, nbits,
				  entropy_count >> ENTROPY_SHIFT, _RET_IP_);

	if (r == &input_pool) {
		int entropy_bits = entropy_count >> ENTROPY_SHIFT;

		if (crng_init < 2) {
			if (entropy_bits < 128)
				return;
			crng_reseed(&primary_crng, r);
			entropy_bits = ENTROPY_BITS(r);
		}
	}
}

static int credit_entropy_bits_safe(struct entropy_store *r, int nbits)
{
	const int nbits_max = r->poolinfo->poolwords * 32;

	if (nbits < 0)
		return -EINVAL;

	/* Cap the value to avoid overflows */
	nbits = min(nbits,  nbits_max);

	credit_entropy_bits(r, nbits);
	return 0;
}

/*********************************************************************
 *
 * Initialization and readiness waiting.
 *
 * Much of the RNG infrastructure is devoted to various dependencies
 * being able to wait until the RNG has collected enough entropy and
 * is ready for safe consumption.
 *
 *********************************************************************/

/*
 * crng_init is protected by base_crng->lock, and only increases
 * its value (from empty->early->ready).
 */
static enum {
	CRNG_EMPTY = 0, /* Little to no entropy collected */
	CRNG_EARLY = 1, /* At least POOL_EARLY_BITS collected */
	CRNG_READY = 2  /* Fully initialized with POOL_READY_BITS collected */
} crng_init __read_mostly = CRNG_EMPTY;
#define crng_ready() (likely(crng_init >= CRNG_READY))
/* Various types of waiters for crng_init->CRNG_READY transition. */
static DECLARE_WAIT_QUEUE_HEAD(crng_init_wait);
static struct fasync_struct *fasync;
static DEFINE_SPINLOCK(random_ready_chain_lock);
static RAW_NOTIFIER_HEAD(random_ready_chain);

/* Control how we warn userspace. */
static struct ratelimit_state urandom_warning =
	RATELIMIT_STATE_INIT_FLAGS("urandom_warning", HZ, 3, RATELIMIT_MSG_ON_RELEASE);
static int ratelimit_disable __read_mostly =
	IS_ENABLED(CONFIG_WARN_ALL_UNSEEDED_RANDOM);
module_param_named(ratelimit_disable, ratelimit_disable, int, 0644);
MODULE_PARM_DESC(ratelimit_disable, "Disable random ratelimit suppression");

/*
 * Returns whether or not the input pool has been seeded and thus guaranteed
 * to supply cryptographically secure random numbers. This applies to: the
 * /dev/urandom device, the get_random_bytes function, and the get_random_{u32,
 * ,u64,int,long} family of functions.
 *
 * Returns: true if the input pool has been seeded.
 *          false if the input pool has not been seeded.
 */
bool rng_is_initialized(void)
{
	return crng_ready();
}
EXPORT_SYMBOL(rng_is_initialized);

/* Used by wait_for_random_bytes(), and considered an entropy collector, below. */
static void try_to_generate_entropy(void);

/*
 * Wait for the input pool to be seeded and thus guaranteed to supply
 * cryptographically secure random numbers. This applies to: the /dev/urandom
 * device, the get_random_bytes function, and the get_random_{u32,u64,int,long}
 * family of functions. Using any of these functions without first calling
 * this function forfeits the guarantee of security.
 *
 * Returns: 0 if the input pool has been seeded.
 *          -ERESTARTSYS if the function was interrupted by a signal.
 */
int wait_for_random_bytes(void)
{
	while (!crng_ready()) {
		int ret;

		try_to_generate_entropy();
		ret = wait_event_interruptible_timeout(crng_init_wait, crng_ready(), HZ);
		if (ret)
			return ret > 0 ? 0 : ret;
	}
	return 0;
}
EXPORT_SYMBOL(wait_for_random_bytes);

/*
 * Add a callback function that will be invoked when the input
 * pool is initialised.
 *
 * returns: 0 if callback is successfully added
 *	    -EALREADY if pool is already initialised (callback not called)
 */
int __cold register_random_ready_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret = -EALREADY;

	if (crng_ready())
		return ret;

	spin_lock_irqsave(&random_ready_chain_lock, flags);
	if (!crng_ready())
		ret = raw_notifier_chain_register(&random_ready_chain, nb);
	spin_unlock_irqrestore(&random_ready_chain_lock, flags);
	return ret;
}

/*
 * Delete a previously registered readiness callback function.
 */
int __cold unregister_random_ready_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&random_ready_chain_lock, flags);
	ret = raw_notifier_chain_unregister(&random_ready_chain, nb);
	spin_unlock_irqrestore(&random_ready_chain_lock, flags);
	return ret;
}

static void __cold process_random_ready_list(void)
{
	unsigned long flags;

	spin_lock_irqsave(&random_ready_chain_lock, flags);
	raw_notifier_call_chain(&random_ready_chain, 0, NULL);
	spin_unlock_irqrestore(&random_ready_chain_lock, flags);
}

#define warn_unseeded_randomness() \
	if (IS_ENABLED(CONFIG_WARN_ALL_UNSEEDED_RANDOM) && !crng_ready()) \
		printk_deferred(KERN_NOTICE "random: %s called from %pS with crng_init=%d\n", \
				__func__, (void *)_RET_IP_, crng_init)


/*********************************************************************
 *
 * Fast key erasure RNG, the "crng".
 *
 * These functions expand entropy from the entropy extractor into
 * long streams for external consumption using the "fast key erasure"
 * RNG described at <https://blog.cr.yp.to/20170723-random.html>.
 *
 * There are a few exported interfaces for use by other drivers:
 *
 *	void get_random_bytes(void *buf, size_t len)
 *	u32 get_random_u32()
 *	u64 get_random_u64()
 *	unsigned int get_random_int()
 *	unsigned long get_random_long()
 *
 * These interfaces will return the requested number of random bytes
 * into the given buffer or as a return value. This is equivalent to
 * a read from /dev/urandom. The u32, u64, int, and long family of
 * functions may be higher performance for one-off random integers,
 * because they do a bit of buffering and do not invoke reseeding
 * until the buffer is emptied.
 *
 *********************************************************************/

enum {
	CRNG_RESEED_START_INTERVAL = HZ,
	CRNG_RESEED_INTERVAL = 60 * HZ
};

static struct {
	u8 key[CHACHA_KEY_SIZE] __aligned(__alignof__(long));
	unsigned long birth;
	unsigned long generation;
	spinlock_t lock;
} base_crng = {
	.lock = __SPIN_LOCK_UNLOCKED(base_crng.lock)
};

struct crng {
	u8 key[CHACHA_KEY_SIZE];
	unsigned long generation;
};

static DEFINE_PER_CPU(struct crng, crngs) = {
	.generation = ULONG_MAX
};

/* Used by crng_reseed() and crng_make_state() to extract a new seed from the input pool. */
static void extract_entropy(void *buf, size_t len);

/* This extracts a new crng key from the input pool. */
static void crng_reseed(void)
{
	unsigned long flags;
	unsigned long next_gen;
	u8 key[CHACHA_KEY_SIZE];

	extract_entropy(key, sizeof(key));

	/*
	 * We copy the new key into the base_crng, overwriting the old one,
	 * and update the generation counter. We avoid hitting ULONG_MAX,
	 * because the per-cpu crngs are initialized to ULONG_MAX, so this
	 * forces new CPUs that come online to always initialize.
	 */
	spin_lock_irqsave(&base_crng.lock, flags);
	memcpy(base_crng.key, key, sizeof(base_crng.key));
	next_gen = base_crng.generation + 1;
	if (next_gen == ULONG_MAX)
		++next_gen;
	WRITE_ONCE(base_crng.generation, next_gen);
	WRITE_ONCE(base_crng.birth, jiffies);
	if (!crng_ready())
		crng_init = CRNG_READY;
	spin_unlock_irqrestore(&base_crng.lock, flags);
	memzero_explicit(key, sizeof(key));
}

/*
 * This generates a ChaCha block using the provided key, and then
 * immediately overwites that key with half the block. It returns
 * the resultant ChaCha state to the user, along with the second
 * half of the block containing 32 bytes of random data that may
 * be used; random_data_len may not be greater than 32.
 *
 * The returned ChaCha state contains within it a copy of the old
 * key value, at index 4, so the state should always be zeroed out
 * immediately after using in order to maintain forward secrecy.
 * If the state cannot be erased in a timely manner, then it is
 * safer to set the random_data parameter to &chacha_state[4] so
 * that this function overwrites it before returning.
 */
static void crng_fast_key_erasure(u8 key[CHACHA_KEY_SIZE],
				  u32 chacha_state[CHACHA_BLOCK_SIZE / sizeof(u32)],
				  u8 *random_data, size_t random_data_len)
{
	u8 first_block[CHACHA_BLOCK_SIZE];

	BUG_ON(random_data_len > 32);

	chacha_init_consts(chacha_state);
	memcpy(&chacha_state[4], key, CHACHA_KEY_SIZE);
	memset(&chacha_state[12], 0, sizeof(u32) * 4);
	chacha20_block(chacha_state, first_block);

	memcpy(key, first_block, CHACHA_KEY_SIZE);
	memcpy(random_data, first_block + CHACHA_KEY_SIZE, random_data_len);
	memzero_explicit(first_block, sizeof(first_block));
}

/*
 * Return whether the crng seed is considered to be sufficiently old
 * that a reseeding is needed. This happens if the last reseeding
 * was CRNG_RESEED_INTERVAL ago, or during early boot, at an interval
 * proportional to the uptime.
 */
static bool crng_has_old_seed(void)
{
	static bool early_boot = true;
	unsigned long interval = CRNG_RESEED_INTERVAL;

	if (unlikely(READ_ONCE(early_boot))) {
		time64_t uptime = ktime_get_seconds();
		if (uptime >= CRNG_RESEED_INTERVAL / HZ * 2)
			WRITE_ONCE(early_boot, false);
		else
			interval = max_t(unsigned int, CRNG_RESEED_START_INTERVAL,
					 (unsigned int)uptime / 2 * HZ);
	}
	return time_is_before_jiffies(READ_ONCE(base_crng.birth) + interval);
}

/*
 * This function returns a ChaCha state that you may use for generating
 * random data. It also returns up to 32 bytes on its own of random data
 * that may be used; random_data_len may not be greater than 32.
 */
static void crng_make_state(u32 chacha_state[CHACHA_BLOCK_SIZE / sizeof(u32)],
			    u8 *random_data, size_t random_data_len)
{
	unsigned long flags;
	struct crng *crng;

	BUG_ON(random_data_len > 32);

	/*
	 * For the fast path, we check whether we're ready, unlocked first, and
	 * then re-check once locked later. In the case where we're really not
	 * ready, we do fast key erasure with the base_crng directly, extracting
	 * when crng_init is CRNG_EMPTY.
	 */
	if (!crng_ready()) {
		bool ready;

		spin_lock_irqsave(&base_crng.lock, flags);
		ready = crng_ready();
		if (!ready) {
			if (crng_init == CRNG_EMPTY)
				extract_entropy(base_crng.key, sizeof(base_crng.key));
			crng_fast_key_erasure(base_crng.key, chacha_state,
					      random_data, random_data_len);
		}
		spin_unlock_irqrestore(&base_crng.lock, flags);
		if (!ready)
			return;
	}

	/*
	 * If the base_crng is old enough, we reseed, which in turn bumps the
	 * generation counter that we check below.
	 */
	if (unlikely(crng_has_old_seed()))
		crng_reseed();

	local_irq_save(flags);
	crng = raw_cpu_ptr(&crngs);

	/*
	 * If our per-cpu crng is older than the base_crng, then it means
	 * somebody reseeded the base_crng. In that case, we do fast key
	 * erasure on the base_crng, and use its output as the new key
	 * for our per-cpu crng. This brings us up to date with base_crng.
	 */
	if (unlikely(crng->generation != READ_ONCE(base_crng.generation))) {
		spin_lock(&base_crng.lock);
		crng_fast_key_erasure(base_crng.key, chacha_state,
				      crng->key, sizeof(crng->key));
		crng->generation = base_crng.generation;
		spin_unlock(&base_crng.lock);
	}

	/*
	 * Finally, when we've made it this far, our per-cpu crng has an up
	 * to date key, and we can do fast key erasure with it to produce
	 * some random data and a ChaCha state for the caller. All other
	 * branches of this function are "unlikely", so most of the time we
	 * should wind up here immediately.
	 */
	crng_fast_key_erasure(crng->key, chacha_state, random_data, random_data_len);
	local_irq_restore(flags);
}

static void _get_random_bytes(void *buf, size_t len)
{
	u32 chacha_state[CHACHA_BLOCK_SIZE / sizeof(u32)];
	u8 tmp[CHACHA_BLOCK_SIZE];
	size_t first_block_len;

	if (!len)
		return;

	first_block_len = min_t(size_t, 32, len);
	crng_make_state(chacha_state, buf, first_block_len);
	len -= first_block_len;
	buf += first_block_len;

	while (len) {
		if (len < CHACHA_BLOCK_SIZE) {
			chacha20_block(chacha_state, tmp);
			memcpy(buf, tmp, len);
			memzero_explicit(tmp, sizeof(tmp));
			break;
		}

		chacha20_block(chacha_state, buf);
		if (unlikely(chacha_state[12] == 0))
			++chacha_state[13];
		len -= CHACHA_BLOCK_SIZE;
		buf += CHACHA_BLOCK_SIZE;
	}

	memzero_explicit(chacha_state, sizeof(chacha_state));
}

/*
 * This function is the exported kernel interface.  It returns some
 * number of good random numbers, suitable for key generation, seeding
 * TCP sequence numbers, etc.  It does not rely on the hardware random
 * number generator.  For random bytes direct from the hardware RNG
 * (when available), use get_random_bytes_arch(). In order to ensure
 * that the randomness provided by this function is okay, the function
 * wait_for_random_bytes() should be called and return 0 at least once
 * at any point prior.
 */
void get_random_bytes(void *buf, size_t len)
{
	warn_unseeded_randomness();
	_get_random_bytes(buf, len);
}
EXPORT_SYMBOL(get_random_bytes);

static ssize_t get_random_bytes_user(struct iov_iter *iter)
{
	u32 chacha_state[CHACHA_BLOCK_SIZE / sizeof(u32)];
	u8 block[CHACHA_BLOCK_SIZE];
	size_t ret = 0, copied;

	if (unlikely(!iov_iter_count(iter)))
		return 0;

	/*
	 * Immediately overwrite the ChaCha key at index 4 with random
	 * bytes, in case userspace causes copy_to_iter() below to sleep
	 * forever, so that we still retain forward secrecy in that case.
	 */
	crng_make_state(chacha_state, (u8 *)&chacha_state[4], CHACHA_KEY_SIZE);
	/*
	 * However, if we're doing a read of len <= 32, we don't need to
	 * use chacha_state after, so we can simply return those bytes to
	 * the user directly.
	 */
	if (iov_iter_count(iter) <= CHACHA_KEY_SIZE) {
		ret = copy_to_iter(&chacha_state[4], CHACHA_KEY_SIZE, iter);
		goto out_zero_chacha;
	}

	for (;;) {
		chacha20_block(chacha_state, block);
		if (unlikely(chacha_state[12] == 0))
			++chacha_state[13];

		copied = copy_to_iter(block, sizeof(block), iter);
		ret += copied;
		if (!iov_iter_count(iter) || copied != sizeof(block))
			break;

		BUILD_BUG_ON(PAGE_SIZE % sizeof(block) != 0);
		if (ret % PAGE_SIZE == 0) {
			if (signal_pending(current))
				break;
			cond_resched();
		}
	}

	memzero_explicit(block, sizeof(block));
out_zero_chacha:
	memzero_explicit(chacha_state, sizeof(chacha_state));
	return ret ? ret : -EFAULT;
}

/*
 * Batched entropy returns random integers. The quality of the random
 * number is good as /dev/urandom. In order to ensure that the randomness
 * provided by this function is okay, the function wait_for_random_bytes()
 * should be called and return 0 at least once at any point prior.
 */

#define DEFINE_BATCHED_ENTROPY(type)						\
struct batch_ ##type {								\
	/*									\
	 * We make this 1.5x a ChaCha block, so that we get the			\
	 * remaining 32 bytes from fast key erasure, plus one full		\
	 * block from the detached ChaCha state. We can increase		\
	 * the size of this later if needed so long as we keep the		\
	 * formula of (integer_blocks + 0.5) * CHACHA_BLOCK_SIZE.		\
	 */									\
	type entropy[CHACHA_BLOCK_SIZE * 3 / (2 * sizeof(type))];		\
	unsigned long generation;						\
	unsigned int position;							\
};										\
										\
static DEFINE_PER_CPU(struct batch_ ##type, batched_entropy_ ##type) = {	\
	.position = UINT_MAX							\
};										\
										\
type get_random_ ##type(void)							\
{										\
	type ret;								\
	unsigned long flags;							\
	struct batch_ ##type *batch;						\
	unsigned long next_gen;							\
										\
	warn_unseeded_randomness();						\
										\
	if  (!crng_ready()) {							\
		_get_random_bytes(&ret, sizeof(ret));				\
		return ret;							\
	}									\
										\
	local_irq_save(flags);		\
	batch = raw_cpu_ptr(&batched_entropy_##type);				\
										\
	next_gen = READ_ONCE(base_crng.generation);				\
	if (batch->position >= ARRAY_SIZE(batch->entropy) ||			\
	    next_gen != batch->generation) {					\
		_get_random_bytes(batch->entropy, sizeof(batch->entropy));	\
		batch->position = 0;						\
		batch->generation = next_gen;					\
	}									\
										\
	ret = batch->entropy[batch->position];					\
	batch->entropy[batch->position] = 0;					\
	++batch->position;							\
	local_irq_restore(flags);		\
	return ret;								\
}										\
EXPORT_SYMBOL(get_random_ ##type);

DEFINE_BATCHED_ENTROPY(u64)
DEFINE_BATCHED_ENTROPY(u32)

#ifdef CONFIG_SMP
/*
 * This function is called when the CPU is coming up, with entry
 * CPUHP_RANDOM_PREPARE, which comes before CPUHP_WORKQUEUE_PREP.
 */
int __cold random_prepare_cpu(unsigned int cpu)
{
	/*
	 * When the cpu comes back online, immediately invalidate both
	 * the per-cpu crng and all batches, so that we serve fresh
	 * randomness.
	 */
	per_cpu_ptr(&crngs, cpu)->generation = ULONG_MAX;
	per_cpu_ptr(&batched_entropy_u32, cpu)->position = UINT_MAX;
	per_cpu_ptr(&batched_entropy_u64, cpu)->position = UINT_MAX;
	return 0;
}
#endif

/*
 * This function will use the architecture-specific hardware random
 * number generator if it is available. It is not recommended for
 * use. Use get_random_bytes() instead. It returns the number of
 * bytes filled in.
 */
size_t __must_check get_random_bytes_arch(void *buf, size_t len)
{
	size_t left = len;
	u8 *p = buf;

	while (left) {
		unsigned long v;
		size_t block_len = min_t(size_t, left, sizeof(unsigned long));

		if (!arch_get_random_long(&v))
			break;

		memcpy(p, &v, block_len);
		p += block_len;
		left -= block_len;
	}

	return len - left;
}
EXPORT_SYMBOL(get_random_bytes_arch);


/**********************************************************************
 *
 * Entropy accumulation and extraction routines.
 *
 * Callers may add entropy via:
 *
 *     static void mix_pool_bytes(const void *buf, size_t len)
 *
 * After which, if added entropy should be credited:
 *
 *     static void credit_init_bits(size_t bits)
 *
 * Finally, extract entropy via:
 *
 *     static void extract_entropy(void *buf, size_t len)
 *
 **********************************************************************/

enum {
	POOL_BITS = BLAKE2S_HASH_SIZE * 8,
	POOL_READY_BITS = POOL_BITS, /* When crng_init->CRNG_READY */
	POOL_EARLY_BITS = POOL_READY_BITS / 2 /* When crng_init->CRNG_EARLY */
};

static struct {
	struct blake2s_state hash;
	spinlock_t lock;
	unsigned int init_bits;
} input_pool = {
	.hash.h = { BLAKE2S_IV0 ^ (0x01010000 | BLAKE2S_HASH_SIZE),
		    BLAKE2S_IV1, BLAKE2S_IV2, BLAKE2S_IV3, BLAKE2S_IV4,
		    BLAKE2S_IV5, BLAKE2S_IV6, BLAKE2S_IV7 },
	.hash.outlen = BLAKE2S_HASH_SIZE,
	.lock = __SPIN_LOCK_UNLOCKED(input_pool.lock),
};

static void _mix_pool_bytes(const void *buf, size_t len)
{
	blake2s_update(&input_pool.hash, buf, len);
}

/*
 * This function adds bytes into the input pool. It does not
 * update the initialization bit counter; the caller should call
 * credit_init_bits if this is appropriate.
 */
static void mix_pool_bytes(const void *buf, size_t len)
{
	unsigned long flags;

	spin_lock_irqsave(&input_pool.lock, flags);
	_mix_pool_bytes(buf, len);
	spin_unlock_irqrestore(&input_pool.lock, flags);
}

/*
 * This is an HKDF-like construction for using the hashed collected entropy
 * as a PRF key, that's then expanded block-by-block.
 */
static void extract_entropy(void *buf, size_t len)
{
	unsigned long flags;
	u8 seed[BLAKE2S_HASH_SIZE], next_key[BLAKE2S_HASH_SIZE];
	struct {
		unsigned long rdseed[32 / sizeof(long)];
		size_t counter;
	} block;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(block.rdseed); ++i) {
		if (!arch_get_random_seed_long(&block.rdseed[i]) &&
		    !arch_get_random_long(&block.rdseed[i]))
			block.rdseed[i] = random_get_entropy();
	}

	spin_lock_irqsave(&input_pool.lock, flags);

	/* seed = HASHPRF(last_key, entropy_input) */
	blake2s_final(&input_pool.hash, seed);

	/* next_key = HASHPRF(seed, RDSEED || 0) */
	block.counter = 0;
	blake2s(next_key, (u8 *)&block, seed, sizeof(next_key), sizeof(block), sizeof(seed));
	blake2s_init_key(&input_pool.hash, BLAKE2S_HASH_SIZE, next_key, sizeof(next_key));

	spin_unlock_irqrestore(&input_pool.lock, flags);
	memzero_explicit(next_key, sizeof(next_key));

	while (len) {
		i = min_t(size_t, len, BLAKE2S_HASH_SIZE);
		/* output = HASHPRF(seed, RDSEED || ++counter) */
		++block.counter;
		blake2s(buf, (u8 *)&block, seed, i, sizeof(block), sizeof(seed));
		len -= i;
		buf += i;
	}

	memzero_explicit(seed, sizeof(seed));
	memzero_explicit(&block, sizeof(block));
}

#define credit_init_bits(bits) if (!crng_ready()) _credit_init_bits(bits)

static void __cold _credit_init_bits(size_t bits)
{
	unsigned int new, orig, add;
	unsigned long flags;

	if (!bits)
		return;

	add = min_t(size_t, bits, POOL_BITS);

	do {
		orig = READ_ONCE(input_pool.init_bits);
		new = min_t(unsigned int, POOL_BITS, orig + add);
	} while (cmpxchg(&input_pool.init_bits, orig, new) != orig);

	if (orig < POOL_READY_BITS && new >= POOL_READY_BITS) {
		crng_reseed(); /* Sets crng_init to CRNG_READY under base_crng.lock. */
		process_random_ready_list();
		wake_up_interruptible(&crng_init_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
		pr_notice("crng init done\n");
		if (urandom_warning.missed)
			pr_notice("%d urandom warning(s) missed due to ratelimiting\n",
				  urandom_warning.missed);
	} else if (orig < POOL_EARLY_BITS && new >= POOL_EARLY_BITS) {
		spin_lock_irqsave(&base_crng.lock, flags);
		/* Check if crng_init is CRNG_EMPTY, to avoid race with crng_reseed(). */
		if (crng_init == CRNG_EMPTY) {
			extract_entropy(base_crng.key, sizeof(base_crng.key));
			crng_init = CRNG_EARLY;
		}
		spin_unlock_irqrestore(&base_crng.lock, flags);
	}
}


/**********************************************************************
 *
 * Entropy collection routines.
 *
 * The following exported functions are used for pushing entropy into
 * the above entropy accumulation routines:
 *
 *	void add_device_randomness(const void *buf, size_t len);
 *	void add_hwgenerator_randomness(const void *buf, size_t len, size_t entropy);
 *	void add_bootloader_randomness(const void *buf, size_t len);
 *	void add_interrupt_randomness(int irq);
 *	void add_input_randomness(unsigned int type, unsigned int code, unsigned int value);
 *	void add_disk_randomness(struct gendisk *disk);
 *
 * add_device_randomness() adds data to the input pool that
 * is likely to differ between two devices (or possibly even per boot).
 * This would be things like MAC addresses or serial numbers, or the
 * read-out of the RTC. This does *not* credit any actual entropy to
 * the pool, but it initializes the pool to different values for devices
 * that might otherwise be identical and have very little entropy
 * available to them (particularly common in the embedded world).
 *
 * add_hwgenerator_randomness() is for true hardware RNGs, and will credit
 * entropy as specified by the caller. If the entropy pool is full it will
 * block until more entropy is needed.
 *
 * add_bootloader_randomness() is called by bootloader drivers, such as EFI
 * and device tree, and credits its input depending on whether or not the
 * configuration option CONFIG_RANDOM_TRUST_BOOTLOADER is set.
 *
 * add_interrupt_randomness() uses the interrupt timing as random
 * inputs to the entropy pool. Using the cycle counters and the irq source
 * as inputs, it feeds the input pool roughly once a second or after 64
 * interrupts, crediting 1 bit of entropy for whichever comes first.
 *
 * add_input_randomness() uses the input layer interrupt timing, as well
 * as the event type information from the hardware.
 *
 * add_disk_randomness() uses what amounts to the seek time of block
 * layer request events, on a per-disk_devt basis, as input to the
 * entropy pool. Note that high-speed solid state drives with very low
 * seek times do not make for good sources of entropy, as their seek
 * times are usually fairly consistent.
 *
 * The last two routines try to estimate how many bits of entropy
 * to credit. They do this by keeping track of the first and second
 * order deltas of the event timings.
 *
 **********************************************************************/

static bool trust_cpu __initdata = IS_ENABLED(CONFIG_RANDOM_TRUST_CPU);
static bool trust_bootloader __initdata = IS_ENABLED(CONFIG_RANDOM_TRUST_BOOTLOADER);
static int __init parse_trust_cpu(char *arg)
{
	return kstrtobool(arg, &trust_cpu);
}
static int __init parse_trust_bootloader(char *arg)
{
	return kstrtobool(arg, &trust_bootloader);
}
early_param("random.trust_cpu", parse_trust_cpu);

static void crng_initialize(struct crng_state *crng)
{
	int		i;
	int		arch_init = 1;
	unsigned long	rv;

	memcpy(&crng->state[0], "expand 32-byte k", 16);
	if (crng == &primary_crng)
		_extract_entropy(&input_pool, &crng->state[4],
				 sizeof(__u32) * 12, 0);
	else
		_get_random_bytes(&crng->state[4], sizeof(__u32) * 12);
	for (i = 4; i < 16; i++) {
		if (!arch_get_random_seed_long(&rv) &&
		    !arch_get_random_long(&rv)) {
			rv = random_get_entropy();
			arch_init = 0;
		}
		crng->state[i] ^= rv;
	}
	if (trust_cpu && arch_init && crng == &primary_crng) {
		invalidate_batched_entropy();
		numa_crng_init();
		crng_init = 2;
		pr_notice("crng done (trusting CPU's manufacturer)\n");
	}
	crng->init_time = jiffies - CRNG_RESEED_INTERVAL - 1;
}

#ifdef CONFIG_NUMA
static void do_numa_crng_init(struct work_struct *work)
{
	int i;
	struct crng_state *crng;
	struct crng_state **pool;

	pool = kcalloc(nr_node_ids, sizeof(*pool), GFP_KERNEL|__GFP_NOFAIL);
	for_each_online_node(i) {
		crng = kmalloc_node(sizeof(struct crng_state),
				    GFP_KERNEL | __GFP_NOFAIL, i);
		spin_lock_init(&crng->lock);
		crng_initialize(crng);
		pool[i] = crng;
	}
	mb();
	if (cmpxchg(&crng_node_pool, NULL, pool)) {
		for_each_node(i)
			kfree(pool[i]);
		kfree(pool);
	}
}

static DECLARE_WORK(numa_crng_init_work, do_numa_crng_init);

static void numa_crng_init(void)
{
	schedule_work(&numa_crng_init_work);
}
#else
static void numa_crng_init(void) {}
#endif
early_param("random.trust_bootloader", parse_trust_bootloader);


/*
 * The first collection of entropy occurs at system boot while interrupts
 * are still turned off. Here we push in latent entropy, RDSEED, a timestamp,
 * utsname(), and the command line. Depending on the above configuration knob,
 * RDSEED may be considered sufficient for initialization. Note that much
 * earlier setup may already have pushed entropy into the input pool by the
 * time we get here.
 */
int __init random_init(const char *command_line)
{
	ktime_t now = ktime_get_real();
	unsigned int i, arch_bits;
	unsigned long entropy;

	if (!spin_trylock_irqsave(&primary_crng.lock, flags))
		return 0;
	if (crng_init != 0) {
		spin_unlock_irqrestore(&primary_crng.lock, flags);
		return 0;
	}
	p = (unsigned char *) &primary_crng.state[4];
	while (len > 0 && crng_init_cnt < CRNG_INIT_CNT_THRESH) {
		p[crng_init_cnt % CHACHA_KEY_SIZE] ^= *cp;
		cp++; crng_init_cnt++; len--;
	}
	spin_unlock_irqrestore(&primary_crng.lock, flags);
	if (crng_init_cnt >= CRNG_INIT_CNT_THRESH) {
		invalidate_batched_entropy();
		crng_init = 1;
		pr_notice("fast init done\n");
	}
	return 1;
}

/*
 * crng_slow_load() is called by add_device_randomness, which has two
 * attributes.  (1) We can't trust the buffer passed to it is
 * guaranteed to be unpredictable (so it might not have any entropy at
 * all), and (2) it doesn't have the performance constraints of
 * crng_fast_load().
 *
 * So we do something more comprehensive which is guaranteed to touch
 * all of the primary_crng's state, and which uses a LFSR with a
 * period of 255 as part of the mixing algorithm.  Finally, we do
 * *not* advance crng_init_cnt since buffer we may get may be something
 * like a fixed DMI table (for example), which might very well be
 * unique to the machine, but is otherwise unvarying.
 */
static int crng_slow_load(const char *cp, size_t len)
{
	unsigned long		flags;
	static unsigned char	lfsr = 1;
	unsigned char		tmp;
	unsigned		i, max = CHACHA_KEY_SIZE;
	const char *		src_buf = cp;
	char *			dest_buf = (char *) &primary_crng.state[4];

	if (!spin_trylock_irqsave(&primary_crng.lock, flags))
		return 0;
	if (crng_init != 0) {
		spin_unlock_irqrestore(&primary_crng.lock, flags);
		return 0;
	}
	if (len > max)
		max = len;

	for (i = 0; i < max ; i++) {
		tmp = lfsr;
		lfsr >>= 1;
		if (tmp & 1)
			lfsr ^= 0xE1;
		tmp = dest_buf[i % CHACHA_KEY_SIZE];
		dest_buf[i % CHACHA_KEY_SIZE] ^= src_buf[i % len] ^ lfsr;
		lfsr += (tmp << 3) | (tmp >> 5);
	}
	spin_unlock_irqrestore(&primary_crng.lock, flags);
	return 1;
}

static void crng_reseed(struct crng_state *crng, struct entropy_store *r)
{
	unsigned long	flags;
	int		i, num;
	union {
		__u8	block[CHACHA_BLOCK_SIZE];
		__u32	key[8];
	} buf;

	if (r) {
		num = extract_entropy(r, &buf, 32, 16, 0);
		if (num == 0)
			return;
	} else {
		_extract_crng(&primary_crng, buf.block);
		_crng_backtrack_protect(&primary_crng, buf.block,
					CHACHA_KEY_SIZE);
	}
	spin_lock_irqsave(&crng->lock, flags);
	for (i = 0; i < 8; i++) {
		unsigned long	rv;
		if (!arch_get_random_seed_long(&rv) &&
		    !arch_get_random_long(&rv))
			rv = random_get_entropy();
		crng->state[i+4] ^= buf.key[i] ^ rv;
	}
	memzero_explicit(&buf, sizeof(buf));
	crng->init_time = jiffies;
	spin_unlock_irqrestore(&crng->lock, flags);
	if (crng == &primary_crng && crng_init < 2) {
		invalidate_batched_entropy();
		numa_crng_init();
		crng_init = 2;
		process_random_ready_list();
		wake_up_interruptible(&crng_init_wait);
		kill_fasync(&fasync, SIGIO, POLL_IN);
		pr_notice("crng init done\n");
		if (unseeded_warning.missed) {
			pr_notice("%d get_random_xx warning(s) missed due to ratelimiting\n",
				  unseeded_warning.missed);
			unseeded_warning.missed = 0;
		}
		if (urandom_warning.missed) {
			pr_notice("%d urandom warning(s) missed due to ratelimiting\n",
				  urandom_warning.missed);
			urandom_warning.missed = 0;
		}
	}
}

static void _extract_crng(struct crng_state *crng,
			  __u8 out[CHACHA_BLOCK_SIZE])
{
	unsigned long v, flags;

	if (crng_ready() &&
	    (time_after(crng_global_init_time, crng->init_time) ||
	     time_after(jiffies, crng->init_time + CRNG_RESEED_INTERVAL)))
		crng_reseed(crng, crng == &primary_crng ? &input_pool : NULL);
	spin_lock_irqsave(&crng->lock, flags);
	if (arch_get_random_long(&v))
		crng->state[14] ^= v;
	chacha20_block(&crng->state[0], out);
	if (crng->state[12] == 0)
		crng->state[13]++;
	spin_unlock_irqrestore(&crng->lock, flags);
}

static void extract_crng(__u8 out[CHACHA_BLOCK_SIZE])
{
	struct crng_state *crng = NULL;

#ifdef CONFIG_NUMA
	if (crng_node_pool)
		crng = crng_node_pool[numa_node_id()];
	if (crng == NULL)

#if defined(LATENT_ENTROPY_PLUGIN)
	static const u8 compiletime_seed[BLAKE2S_BLOCK_SIZE] __initconst __latent_entropy;
	_mix_pool_bytes(compiletime_seed, sizeof(compiletime_seed));

#endif

	for (i = 0, arch_bits = BLAKE2S_BLOCK_SIZE * 8;
	     i < BLAKE2S_BLOCK_SIZE; i += sizeof(entropy)) {
		if (!arch_get_random_seed_long_early(&entropy) &&
		    !arch_get_random_long_early(&entropy)) {
			entropy = random_get_entropy();
			arch_bits -= sizeof(entropy) * 8;
		}
		_mix_pool_bytes(&entropy, sizeof(entropy));
	}
	_mix_pool_bytes(&now, sizeof(now));
	_mix_pool_bytes(utsname(), sizeof(*(utsname())));
	_mix_pool_bytes(command_line, strlen(command_line));
	add_latent_entropy();

	if (crng_ready())
		crng_reseed();
	else if (trust_cpu)
		_credit_init_bits(arch_bits);

	return 0;
}

/*
 * Add device- or boot-specific data to the input pool to help
 * initialize it.
 *
 * None of this adds any entropy; it is meant to avoid the problem of
 * the entropy pool having similar initial state across largely
 * identical devices.
 */
void add_device_randomness(const void *buf, size_t len)
{
	unsigned long entropy = random_get_entropy();
	unsigned long flags;

	spin_lock_irqsave(&input_pool.lock, flags);
	_mix_pool_bytes(&entropy, sizeof(entropy));
	_mix_pool_bytes(buf, len);
	spin_unlock_irqrestore(&input_pool.lock, flags);
}
EXPORT_SYMBOL(add_device_randomness);

/*
 * Interface for in-kernel drivers of true hardware RNGs.
 * Those devices may produce endless random bits and will be throttled
 * when our pool is full.
 */
void add_hwgenerator_randomness(const void *buf, size_t len, size_t entropy)
{
	mix_pool_bytes(buf, len);
	credit_init_bits(entropy);

	/*
	 * Throttle writing to once every CRNG_RESEED_INTERVAL, unless
	 * we're not yet initialized.
	 */
	if (!kthread_should_stop() && crng_ready())
		schedule_timeout_interruptible(CRNG_RESEED_INTERVAL);
}
EXPORT_SYMBOL_GPL(add_hwgenerator_randomness);

/*
 * Handle random seed passed by bootloader, and credit it if
 * CONFIG_RANDOM_TRUST_BOOTLOADER is set.
 */
void __init add_bootloader_randomness(const void *buf, size_t len)
{
	mix_pool_bytes(buf, len);
	if (trust_bootloader)
		credit_init_bits(len * 8);
}

struct fast_pool {
	unsigned long pool[4];
	unsigned long last;
	unsigned int count;
	struct timer_list mix;
};

static void mix_interrupt_randomness(struct timer_list *work);

static DEFINE_PER_CPU(struct fast_pool, irq_randomness) = {
#ifdef CONFIG_64BIT
#define FASTMIX_PERM SIPHASH_PERMUTATION
	.pool = { SIPHASH_CONST_0, SIPHASH_CONST_1, SIPHASH_CONST_2, SIPHASH_CONST_3 },
#else
#define FASTMIX_PERM HSIPHASH_PERMUTATION
	.pool = { HSIPHASH_CONST_0, HSIPHASH_CONST_1, HSIPHASH_CONST_2, HSIPHASH_CONST_3 },
#endif
	.mix = __TIMER_INITIALIZER(mix_interrupt_randomness, 0)
};

/*
 * This is [Half]SipHash-1-x, starting from an empty key. Because
 * the key is fixed, it assumes that its inputs are non-malicious,
 * and therefore this has no security on its own. s represents the
 * four-word SipHash state, while v represents a two-word input.
 */
static void fast_mix(unsigned long s[4], unsigned long v1, unsigned long v2)
{
	s[3] ^= v1;
	FASTMIX_PERM(s[0], s[1], s[2], s[3]);
	s[0] ^= v1;
	s[3] ^= v2;
	FASTMIX_PERM(s[0], s[1], s[2], s[3]);
	s[0] ^= v2;
}

#ifdef CONFIG_SMP
/*
 * This function is called when the CPU has just come online, with
 * entry CPUHP_AP_RANDOM_ONLINE, just after CPUHP_AP_WORKQUEUE_ONLINE.
 */
int __cold random_online_cpu(unsigned int cpu)
{
	/*
	 * During CPU shutdown and before CPU onlining, add_interrupt_
	 * randomness() may schedule mix_interrupt_randomness(), and
	 * set the MIX_INFLIGHT flag. However, because the worker can
	 * be scheduled on a different CPU during this period, that
	 * flag will never be cleared. For that reason, we zero out
	 * the flag here, which runs just after workqueues are onlined
	 * for the CPU again. This also has the effect of setting the
	 * irq randomness count to zero so that new accumulated irqs
	 * are fresh.
	 */
	per_cpu_ptr(&irq_randomness, cpu)->count = 0;
	return 0;
}
#endif

static void mix_interrupt_randomness(struct timer_list *work)
{
	struct fast_pool *fast_pool = container_of(work, struct fast_pool, mix);
	/*
	 * The size of the copied stack pool is explicitly 2 longs so that we
	 * only ever ingest half of the siphash output each time, retaining
	 * the other half as the next "key" that carries over. The entropy is
	 * supposed to be sufficiently dispersed between bits so on average
	 * we don't wind up "losing" some.
	 */
	unsigned long pool[2];
	unsigned int count;

	/* Check to see if we're running on the wrong CPU due to hotplug. */
	local_irq_disable();
	if (fast_pool != this_cpu_ptr(&irq_randomness)) {
		local_irq_enable();
		return;
	}

	/*
	 * Copy the pool to the stack so that the mixer always has a
	 * consistent view, before we reenable irqs again.
	 */
	memcpy(pool, fast_pool->pool, sizeof(pool));
	count = fast_pool->count;
	fast_pool->count = 0;
	fast_pool->last = jiffies;
	local_irq_enable();

	mix_pool_bytes(pool, sizeof(pool));
	credit_init_bits(clamp_t(unsigned int, (count & U16_MAX) / 64, 1, sizeof(pool) * 8));

	memzero_explicit(pool, sizeof(pool));
}

void add_interrupt_randomness(int irq)
{
	enum { MIX_INFLIGHT = 1U << 31 };
	unsigned long entropy = random_get_entropy();
	struct fast_pool *fast_pool = this_cpu_ptr(&irq_randomness);
	struct pt_regs *regs = get_irq_regs();
	unsigned int new_count;

	fast_mix(fast_pool->pool, entropy,
		 (regs ? instruction_pointer(regs) : _RET_IP_) ^ swab(irq));
	new_count = ++fast_pool->count;

	if (new_count & MIX_INFLIGHT)
		return;

	if (new_count < 1024 && !time_is_before_jiffies(fast_pool->last + HZ))
		return;

	fast_pool->count |= MIX_INFLIGHT;
	if (!timer_pending(&fast_pool->mix)) {
		fast_pool->mix.expires = jiffies;
		add_timer_on(&fast_pool->mix, raw_smp_processor_id());
	}
}
EXPORT_SYMBOL_GPL(add_interrupt_randomness);

/* There is one of these per entropy source */
struct timer_rand_state {
	unsigned long last_time;
	long last_delta, last_delta2;
};

/*
 * This function adds entropy to the entropy "pool" by using timing
 * delays. It uses the timer_rand_state structure to make an estimate
 * of how many bits of entropy this call has added to the pool. The
 * value "num" is also added to the pool; it should somehow describe
 * the type of event that just happened.
 */
static void add_timer_randomness(struct timer_rand_state *state, unsigned int num)
{
	unsigned long entropy = random_get_entropy(), now = jiffies, flags;
	long delta, delta2, delta3;
	unsigned int bits;

	/*
	 * If we're in a hard IRQ, add_interrupt_randomness() will be called
	 * sometime after, so mix into the fast pool.
	 */
	if (in_irq()) {
		fast_mix(this_cpu_ptr(&irq_randomness)->pool, entropy, num);
	} else {
		spin_lock_irqsave(&input_pool.lock, flags);
		_mix_pool_bytes(&entropy, sizeof(entropy));
		_mix_pool_bytes(&num, sizeof(num));
		spin_unlock_irqrestore(&input_pool.lock, flags);
	}

	if (crng_ready())
		return;

	/*
	 * Calculate number of bits of randomness we probably added.
	 * We take into account the first, second and third-order deltas
	 * in order to make our estimate.
	 */
	delta = now - READ_ONCE(state->last_time);
	WRITE_ONCE(state->last_time, now);

	delta2 = delta - READ_ONCE(state->last_delta);
	WRITE_ONCE(state->last_delta, delta);

	delta3 = delta2 - READ_ONCE(state->last_delta2);
	WRITE_ONCE(state->last_delta2, delta2);

	if (delta < 0)
		delta = -delta;
	if (delta2 < 0)
		delta2 = -delta2;
	if (delta3 < 0)
		delta3 = -delta3;
	if (delta > delta2)
		delta = delta2;
	if (delta > delta3)
		delta = delta3;

	/*
<<<<<<< HEAD
	 * delta is now minimum absolute delta.
	 * Round down by 1 bit on general principles,
	 * and limit entropy estimate to 12 bits.
=======
	 * delta is now minimum absolute delta. Round down by 1 bit
	 * on general principles, and limit entropy estimate to 11 bits.
>>>>>>> 9ff959f9401b53ef1514579ac6d29545649db553
	 */
	bits = min(fls(delta >> 1), 11);

	/*
	 * As mentioned above, if we're in a hard IRQ, add_interrupt_randomness()
	 * will run after this, which uses a different crediting scheme of 1 bit
	 * per every 64 interrupts. In order to let that function do accounting
	 * close to the one in this function, we credit a full 64/64 bit per bit,
	 * and then subtract one to account for the extra one added.
	 */
	if (in_irq())
		this_cpu_ptr(&irq_randomness)->count += max(1u, bits * 64) - 1;
	else
		_credit_init_bits(bits);
}

void add_input_randomness(unsigned int type, unsigned int code, unsigned int value)
{
	static unsigned char last_value;
	static struct timer_rand_state input_timer_state = { INITIAL_JIFFIES };

	/* Ignore autorepeat and the like. */
	if (value == last_value)
		return;

	last_value = value;
	add_timer_randomness(&input_timer_state,
			     (type << 4) ^ code ^ (code >> 4) ^ value);
}
EXPORT_SYMBOL_GPL(add_input_randomness);

#ifdef CONFIG_BLOCK
void add_disk_randomness(struct gendisk *disk)
{
	if (!disk || !disk->random)
		return;
	/* First major is 1, so we get >= 0x200 here. */
	add_timer_randomness(disk->random, 0x100 + disk_devt(disk));
}
EXPORT_SYMBOL_GPL(add_disk_randomness);


/*********************************************************************
 *
 * Entropy extraction routines
 *
 *********************************************************************/

/*
 * This function decides how many bytes to actually take from the
 * given pool, and also debits the entropy count accordingly.
 */
static size_t account(struct entropy_store *r, size_t nbytes, int min,
		      int reserved)
{
	int entropy_count, orig, have_bytes;
	size_t ibytes, nfrac;

	BUG_ON(r->entropy_count > r->poolinfo->poolfracbits);

	/* Can we pull enough? */
retry:
	entropy_count = orig = READ_ONCE(r->entropy_count);
	ibytes = nbytes;
	/* never pull more than available */
	have_bytes = entropy_count >> (ENTROPY_SHIFT + 3);

	if ((have_bytes -= reserved) < 0)
		have_bytes = 0;
	ibytes = min_t(size_t, ibytes, have_bytes);
	if (ibytes < min)
		ibytes = 0;

	if (WARN_ON(entropy_count < 0)) {
		pr_warn("negative entropy count: pool %s count %d\n",
			r->name, entropy_count);
		entropy_count = 0;
	}
	nfrac = ibytes << (ENTROPY_SHIFT + 3);
	if ((size_t) entropy_count > nfrac)
		entropy_count -= nfrac;
	else
		entropy_count = 0;

	if (cmpxchg(&r->entropy_count, orig, entropy_count) != orig)
		goto retry;

	trace_debit_entropy(r->name, 8 * ibytes);
	if (ibytes && ENTROPY_BITS(r) < random_write_wakeup_bits) {
		wake_up_interruptible(&random_write_wait);
		kill_fasync(&fasync, SIGIO, POLL_OUT);
	}

	return ibytes;
}

/*
 * This function does the actual extraction for extract_entropy and
 * extract_entropy_user.
 *
 * Note: we assume that .poolwords is a multiple of 16 words.
 */
static void extract_buf(struct entropy_store *r, __u8 *out)
{
	int i;
	union {
		__u32 w[5];
		unsigned long l[LONGS(20)];
	} hash;
	__u32 workspace[SHA_WORKSPACE_WORDS];
	unsigned long flags;

	/*
	 * If we have an architectural hardware random number
	 * generator, use it for SHA's initial vector
	 */
	sha_init(hash.w);
	for (i = 0; i < LONGS(20); i++) {
		unsigned long v;
		if (!arch_get_random_long(&v))
			break;
		hash.l[i] = v;
	}

	/* Generate a hash across the pool, 16 words (512 bits) at a time */
	spin_lock_irqsave(&r->lock, flags);
	for (i = 0; i < r->poolinfo->poolwords; i += 16)
		sha_transform(hash.w, (__u8 *)(r->pool + i), workspace);

	/*
	 * We mix the hash back into the pool to prevent backtracking
	 * attacks (where the attacker knows the state of the pool
	 * plus the current outputs, and attempts to find previous
	 * ouputs), unless the hash function can be inverted. By
	 * mixing at least a SHA1 worth of hash data back, we make
	 * brute-forcing the feedback as hard as brute-forcing the
	 * hash.
	 */
	__mix_pool_bytes(r, hash.w, sizeof(hash.w));
	spin_unlock_irqrestore(&r->lock, flags);

	memzero_explicit(workspace, sizeof(workspace));

	/*
	 * In case the hash function has some recognizable output
	 * pattern, we fold it in half. Thus, we always feed back
	 * twice as much data as we output.
	 */
	hash.w[0] ^= hash.w[3];
	hash.w[1] ^= hash.w[4];
	hash.w[2] ^= rol32(hash.w[2], 16);

	memcpy(out, &hash, EXTRACT_SIZE);
	memzero_explicit(&hash, sizeof(hash));
}

static ssize_t _extract_entropy(struct entropy_store *r, void *buf,
				size_t nbytes, int fips)
{
	ssize_t ret = 0, i;
	__u8 tmp[EXTRACT_SIZE];
	unsigned long flags;

	while (nbytes) {
		extract_buf(r, tmp);

		if (fips) {
			spin_lock_irqsave(&r->lock, flags);
			if (!memcmp(tmp, r->last_data, EXTRACT_SIZE))
				panic("Hardware RNG duplicated output!\n");
			memcpy(r->last_data, tmp, EXTRACT_SIZE);
			spin_unlock_irqrestore(&r->lock, flags);
		}
		i = min_t(int, nbytes, EXTRACT_SIZE);
		memcpy(buf, tmp, i);
		nbytes -= i;
		buf += i;
		ret += i;
	}

	/* Wipe data just returned from memory */
	memzero_explicit(tmp, sizeof(tmp));

	return ret;
}

/*
 * This function extracts randomness from the "entropy pool", and
 * returns it in a buffer.
 *
 * The min parameter specifies the minimum amount we can pull before
 * failing to avoid races that defeat catastrophic reseeding while the
 * reserved parameter indicates how much entropy we must leave in the
 * pool after each pull to avoid starving other readers.
 */
static ssize_t extract_entropy(struct entropy_store *r, void *buf,
				 size_t nbytes, int min, int reserved)
{
	__u8 tmp[EXTRACT_SIZE];
	unsigned long flags;

	/* if last_data isn't primed, we need EXTRACT_SIZE extra bytes */
	if (fips_enabled) {
		spin_lock_irqsave(&r->lock, flags);
		if (!r->last_data_init) {
			r->last_data_init = 1;
			spin_unlock_irqrestore(&r->lock, flags);
			trace_extract_entropy(r->name, EXTRACT_SIZE,
					      ENTROPY_BITS(r), _RET_IP_);
			extract_buf(r, tmp);
			spin_lock_irqsave(&r->lock, flags);
			memcpy(r->last_data, tmp, EXTRACT_SIZE);
		}
		spin_unlock_irqrestore(&r->lock, flags);
	}

	trace_extract_entropy(r->name, nbytes, ENTROPY_BITS(r), _RET_IP_);
	nbytes = account(r, nbytes, min, reserved);

	return _extract_entropy(r, buf, nbytes, fips_enabled);
}

#define warn_unseeded_randomness(previous) \
	_warn_unseeded_randomness(__func__, (void *) _RET_IP_, (previous))

static void _warn_unseeded_randomness(const char *func_name, void *caller,
				      void **previous)
{
#ifdef CONFIG_WARN_ALL_UNSEEDED_RANDOM
	const bool print_once = false;
#else
	static bool print_once __read_mostly;
#endif

	if (print_once ||
	    crng_ready() ||
	    (previous && (caller == READ_ONCE(*previous))))
		return;
	WRITE_ONCE(*previous, caller);
#ifndef CONFIG_WARN_ALL_UNSEEDED_RANDOM
	print_once = true;
#endif
	if (__ratelimit(&unseeded_warning))
		printk_deferred(KERN_NOTICE "random: %s called from %pS "
				"with crng_init=%d\n", func_name, caller,
				crng_init);
}

/*
 * This function is the exported kernel interface.  It returns some
 * number of good random numbers, suitable for key generation, seeding
 * TCP sequence numbers, etc.  It does not rely on the hardware random
 * number generator.  For random bytes direct from the hardware RNG
 * (when available), use get_random_bytes_arch(). In order to ensure
 * that the randomness provided by this function is okay, the function
 * wait_for_random_bytes() should be called and return 0 at least once
 * at any point prior.
 */
static void _get_random_bytes(void *buf, int nbytes)
{
	__u8 tmp[CHACHA_BLOCK_SIZE] __aligned(4);

	trace_get_random_bytes(nbytes, _RET_IP_);

	while (nbytes >= CHACHA_BLOCK_SIZE) {
		extract_crng(buf);
		buf += CHACHA_BLOCK_SIZE;
		nbytes -= CHACHA_BLOCK_SIZE;
	}

	if (nbytes > 0) {
		extract_crng(tmp);
		memcpy(buf, tmp, nbytes);
		crng_backtrack_protect(tmp, nbytes);
	} else
		crng_backtrack_protect(tmp, CHACHA_BLOCK_SIZE);
	memzero_explicit(tmp, sizeof(tmp));
}

void get_random_bytes(void *buf, int nbytes)
{
	static void *previous;

	warn_unseeded_randomness(&previous);
	_get_random_bytes(buf, nbytes);
}
EXPORT_SYMBOL(get_random_bytes);


/*
 * Each time the timer fires, we expect that we got an unpredictable
 * jump in the cycle counter. Even if the timer is running on another
 * CPU, the timer activity will be touching the stack of the CPU that is
 * generating entropy..
 *
 * Note that we don't re-arm the timer in the timer itself - we are
 * happy to be scheduled away, since that just makes the load more
 * complex, but we do not want the timer to keep ticking unless the
 * entropy loop is running.
 *
 * So the re-arming always happens in the entropy loop itself.
 */
static void entropy_timer(struct timer_list *t)
{
	credit_entropy_bits(&input_pool, 1);
}

/*
 * If we have an actual cycle counter, see if we can
 * generate enough entropy with timing noise
 */
static void try_to_generate_entropy(void)
{
	struct {
		unsigned long now;
		struct timer_list timer;
	} stack;

	stack.now = random_get_entropy();

	/* Slow counter - or none. Don't even bother */
	if (stack.now == random_get_entropy())
		return;

	timer_setup_on_stack(&stack.timer, entropy_timer, 0);
	while (!crng_ready()) {
		if (!timer_pending(&stack.timer))
			mod_timer(&stack.timer, jiffies+1);
		mix_pool_bytes(&input_pool, &stack.now, sizeof(stack.now));
		schedule();
		stack.now = random_get_entropy();
	}

	del_timer_sync(&stack.timer);
	destroy_timer_on_stack(&stack.timer);
	mix_pool_bytes(&input_pool, &stack.now, sizeof(stack.now));
}

/*
 * Wait for the urandom pool to be seeded and thus guaranteed to supply
 * cryptographically secure random numbers. This applies to: the /dev/urandom
 * device, the get_random_bytes function, and the get_random_{u32,u64,int,long}
 * family of functions. Using any of these functions without first calling
 * this function forfeits the guarantee of security.
 *
 * Returns: 0 if the urandom pool has been seeded.
 *          -ERESTARTSYS if the function was interrupted by a signal.
 */
int wait_for_random_bytes(void)
{
	if (likely(crng_ready()))
		return 0;

	do {
		int ret;
		ret = wait_event_interruptible_timeout(crng_init_wait, crng_ready(), HZ);
		if (ret)
			return ret > 0 ? 0 : ret;

		try_to_generate_entropy();
	} while (!crng_ready());

	return 0;
}
EXPORT_SYMBOL(wait_for_random_bytes);

/*
 * Returns whether or not the urandom pool has been seeded and thus guaranteed
 * to supply cryptographically secure random numbers. This applies to: the
 * /dev/urandom device, the get_random_bytes function, and the get_random_{u32,
 * ,u64,int,long} family of functions.
 *
 * Returns: true if the urandom pool has been seeded.
 *          false if the urandom pool has not been seeded.
 */
bool rng_is_initialized(void)
{
	return crng_ready();
}
EXPORT_SYMBOL(rng_is_initialized);

/*
 * Add a callback function that will be invoked when the nonblocking
 * pool is initialised.
 *
 * returns: 0 if callback is successfully added
 *	    -EALREADY if pool is already initialised (callback not called)
 *	    -ENOENT if module for callback is not alive
 */
int add_random_ready_callback(struct random_ready_callback *rdy)
{
	struct module *owner;
	unsigned long flags;
	int err = -EALREADY;

	if (crng_ready())
		return err;

	owner = rdy->owner;
	if (!try_module_get(owner))
		return -ENOENT;

	spin_lock_irqsave(&random_ready_list_lock, flags);
	if (crng_ready())
		goto out;

	owner = NULL;

	list_add(&rdy->list, &random_ready_list);
	err = 0;

out:
	spin_unlock_irqrestore(&random_ready_list_lock, flags);

	module_put(owner);

	return err;
}
EXPORT_SYMBOL(add_random_ready_callback);

/*
 * Delete a previously registered readiness callback function.
 */
void del_random_ready_callback(struct random_ready_callback *rdy)
{
	unsigned long flags;
	struct module *owner = NULL;

	spin_lock_irqsave(&random_ready_list_lock, flags);
	if (!list_empty(&rdy->list)) {
		list_del_init(&rdy->list);
		owner = rdy->owner;
	}
	spin_unlock_irqrestore(&random_ready_list_lock, flags);

	module_put(owner);
}
EXPORT_SYMBOL(del_random_ready_callback);

/*
 * This function will use the architecture-specific hardware random
 * number generator if it is available.  The arch-specific hw RNG will
 * almost certainly be faster than what we can do in software, but it
 * is impossible to verify that it is implemented securely (as
 * opposed, to, say, the AES encryption of a sequence number using a
 * key known by the NSA).  So it's useful if we need the speed, but
 * only if we're willing to trust the hardware manufacturer not to
 * have put in a back door.
 *
 * Return number of bytes filled in.
 */
int __must_check get_random_bytes_arch(void *buf, int nbytes)
{
	int left = nbytes;
	char *p = buf;

	trace_get_random_bytes_arch(left, _RET_IP_);
	while (left) {
		unsigned long v;
		int chunk = min_t(int, left, sizeof(unsigned long));

		if (!arch_get_random_long(&v))
			break;

		memcpy(p, &v, chunk);
		p += chunk;
		left -= chunk;
	}

	return nbytes - left;
}
EXPORT_SYMBOL(get_random_bytes_arch);

/*
 * init_std_data - initialize pool with system data
 *
 * @r: pool to initialize
 *
 * This function clears the pool's entropy count and mixes some system
 * data into the pool to prepare it for use. The pool is not cleared
 * as that can only decrease the entropy in the pool.
 */
static void __init init_std_data(struct entropy_store *r)
{
	int i;
	ktime_t now = ktime_get_real();
	unsigned long rv;

	mix_pool_bytes(r, &now, sizeof(now));
	for (i = r->poolinfo->poolbytes; i > 0; i -= sizeof(rv)) {
		if (!arch_get_random_seed_long(&rv) &&
		    !arch_get_random_long(&rv))
			rv = random_get_entropy();
		mix_pool_bytes(r, &rv, sizeof(rv));
	}
	mix_pool_bytes(r, utsname(), sizeof(*(utsname())));
}

/*
 * Note that setup_arch() may call add_device_randomness()
 * long before we get here. This allows seeding of the pools
 * with some platform dependent data very early in the boot
 * process. But it limits our options here. We must use
 * statically allocated structures that already have all
 * initializations complete at compile time. We should also
 * take care not to overwrite the precious per platform data
 * we were given.
 */
int __init rand_initialize(void)
{
	init_std_data(&input_pool);
	crng_initialize(&primary_crng);
	crng_global_init_time = jiffies;
	if (ratelimit_disable) {
		urandom_warning.interval = 0;
		unseeded_warning.interval = 0;
	}
	return 0;
}

#ifdef CONFIG_BLOCK
void rand_initialize_disk(struct gendisk *disk)
void __cold rand_initialize_disk(struct gendisk *disk)
{
	struct timer_rand_state *state;

	/*
	 * If kzalloc returns null, we just won't use that entropy
	 * source.
	 */
	state = kzalloc(sizeof(struct timer_rand_state), GFP_KERNEL);
	if (state) {
		state->last_time = INITIAL_JIFFIES;
		disk->random = state;
	}
}
#endif

static ssize_t
urandom_read_nowarn(struct file *file, char __user *buf, size_t nbytes,
		    loff_t *ppos)
{
	int ret;

	nbytes = min_t(size_t, nbytes, INT_MAX >> (ENTROPY_SHIFT + 3));
	ret = extract_crng_user(buf, nbytes);
	trace_urandom_read(8 * nbytes, 0, ENTROPY_BITS(&input_pool));
	return ret;
}

/*
 * Each time the timer fires, we expect that we got an unpredictable
 * jump in the cycle counter. Even if the timer is running on another
 * CPU, the timer activity will be touching the stack of the CPU that is
 * generating entropy..
 *
 * Note that we don't re-arm the timer in the timer itself - we are
 * happy to be scheduled away, since that just makes the load more
 * complex, but we do not want the timer to keep ticking unless the
 * entropy loop is running.
 *
 * So the re-arming always happens in the entropy loop itself.
 */
static void __cold entropy_timer(struct timer_list *t)
{
	credit_init_bits(1);
}

/*
 * If we have an actual cycle counter, see if we can
 * generate enough entropy with timing noise
 */
static void __cold try_to_generate_entropy(void)
{
	struct {
		unsigned long entropy;
		struct timer_list timer;
	} stack;

	stack.entropy = random_get_entropy();

	/* Slow counter - or none. Don't even bother */
	if (stack.entropy == random_get_entropy())
		return;

	timer_setup_on_stack(&stack.timer, entropy_timer, 0);
	while (!crng_ready() && !signal_pending(current)) {
		if (!timer_pending(&stack.timer))
			mod_timer(&stack.timer, jiffies + 1);
		mix_pool_bytes(&stack.entropy, sizeof(stack.entropy));
		schedule();
		stack.entropy = random_get_entropy();
	}

	del_timer_sync(&stack.timer);
	destroy_timer_on_stack(&stack.timer);
	mix_pool_bytes(&stack.entropy, sizeof(stack.entropy));
}



/**********************************************************************
 *
 * Userspace reader/writer interfaces.
 *
 * getrandom(2) is the primary modern interface into the RNG and should
 * be used in preference to anything else.
 *
 * Reading from /dev/random has the same functionality as calling
 * getrandom(2) with flags=0. In earlier versions, however, it had
 * vastly different semantics and should therefore be avoided, to
 * prevent backwards compatibility issues.
 *
 * Reading from /dev/urandom has the same functionality as calling
 * getrandom(2) with flags=GRND_INSECURE. Because it does not block
 * waiting for the RNG to be ready, it should not be used.
 *
 * Writing to either /dev/random or /dev/urandom adds entropy to
 * the input pool but does not credit it.
 *
 * Polling on /dev/random indicates when the RNG is initialized, on
 * the read side, and when it wants new entropy, on the write side.
 *
 * Both /dev/random and /dev/urandom have the same set of ioctls for
 * adding entropy, getting the entropy count, zeroing the count, and
 * reseeding the crng.
 *
 **********************************************************************/

SYSCALL_DEFINE3(getrandom, char __user *, ubuf, size_t, len, unsigned int, flags)
{

	unsigned long flags;
	static int maxwarn = 10;

	if (!crng_ready() && maxwarn > 0) {
		maxwarn--;
		if (__ratelimit(&urandom_warning))
			pr_notice("%s: uninitialized urandom read (%zd bytes read)\n",
				  current->comm, nbytes);
		spin_lock_irqsave(&primary_crng.lock, flags);
		crng_init_cnt = 0;
		spin_unlock_irqrestore(&primary_crng.lock, flags);
	}

	return urandom_read_nowarn(file, buf, nbytes, ppos);
}

static ssize_t
random_read(struct file *file, char __user *buf, size_t nbytes, loff_t *ppos)
{
	int ret;

	ret = wait_for_random_bytes();
	if (ret != 0)
		return ret;
	return urandom_read_nowarn(file, buf, nbytes, ppos);
}
	struct iov_iter iter;
	struct iovec iov;
	int ret;

	if (flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
		return -EINVAL;


	/*
	 * Requesting insecure and blocking randomness at the same time makes
	 * no sense.
	 */
	if ((flags & (GRND_INSECURE | GRND_RANDOM)) == (GRND_INSECURE | GRND_RANDOM))
		return -EINVAL;


	poll_wait(file, &crng_init_wait, wait);
	poll_wait(file, &random_write_wait, wait);
	mask = 0;
	if (crng_ready())
		mask |= EPOLLIN | EPOLLRDNORM;
	if (ENTROPY_BITS(&input_pool) < random_write_wakeup_bits)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static int
write_pool(struct entropy_store *r, const char __user *buffer, size_t count)
{
	size_t bytes;
	__u32 t, buf[16];
	const char __user *p = buffer;

	while (count > 0) {
		int b, i = 0;

		bytes = min(count, sizeof(buf));
		if (copy_from_user(&buf, p, bytes))
			return -EFAULT;

		for (b = bytes ; b > 0 ; b -= sizeof(__u32), i++) {
			if (!arch_get_random_int(&t))
				break;
			buf[i] ^= t;
		}

		count -= bytes;
		p += bytes;

		mix_pool_bytes(r, buf, bytes);
		cond_resched();

	if (!crng_ready() && !(flags & GRND_INSECURE)) {
		if (flags & GRND_NONBLOCK)
			return -EAGAIN;
		ret = wait_for_random_bytes();
		if (unlikely(ret))
			return ret;
	}

	ret = import_single_range(READ, ubuf, len, &iov, &iter);
	if (unlikely(ret))
		return ret;
	return get_random_bytes_user(&iter);
}

static __poll_t random_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &crng_init_wait, wait);
	return crng_ready() ? EPOLLIN | EPOLLRDNORM : EPOLLOUT | EPOLLWRNORM;
}

static ssize_t write_pool_user(struct iov_iter *iter)
{
	u8 block[BLAKE2S_BLOCK_SIZE];
	ssize_t ret = 0;
	size_t copied;

	if (unlikely(!iov_iter_count(iter)))
		return 0;

	for (;;) {
		copied = copy_from_iter(block, sizeof(block), iter);
		ret += copied;
		mix_pool_bytes(block, copied);
		if (!iov_iter_count(iter) || copied != sizeof(block))
			break;

		BUILD_BUG_ON(PAGE_SIZE % sizeof(block) != 0);
		if (ret % PAGE_SIZE == 0) {
			if (signal_pending(current))
				break;
			cond_resched();
		}
	}

	memzero_explicit(block, sizeof(block));
	return ret ? ret : -EFAULT;
}

static ssize_t random_write_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	return write_pool_user(iter);
}

static ssize_t urandom_read_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	static int maxwarn = 10;

	if (!crng_ready()) {
		if (!ratelimit_disable && maxwarn <= 0)
			++urandom_warning.missed;
		else if (ratelimit_disable || __ratelimit(&urandom_warning)) {
			--maxwarn;
			pr_notice("%s: uninitialized urandom read (%zu bytes read)\n",
				  current->comm, iov_iter_count(iter));
		}
	}

	return get_random_bytes_user(iter);
}

static ssize_t random_read_iter(struct kiocb *kiocb, struct iov_iter *iter)
{
	int ret;

	if (!crng_ready() &&
	    ((kiocb->ki_flags & IOCB_NOWAIT) ||
	     (kiocb->ki_filp->f_flags & O_NONBLOCK)))
		return -EAGAIN;

	ret = wait_for_random_bytes();
	if (ret != 0)
		return ret;
	return get_random_bytes_user(iter);
}

static long random_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int __user *p = (int __user *)arg;
	int ent_count;

	switch (cmd) {
	case RNDGETENTCNT:
		/* Inherently racy, no point locking. */
		if (put_user(input_pool.init_bits, p))
			return -EFAULT;
		return 0;
	case RNDADDTOENTCNT:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p))
			return -EFAULT;
		if (ent_count < 0)
			return -EINVAL;
		credit_init_bits(ent_count);
		return 0;
	case RNDADDENTROPY: {
		struct iov_iter iter;
		struct iovec iov;
		ssize_t ret;
		int len;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(ent_count, p++))
			return -EFAULT;
		if (ent_count < 0)
			return -EINVAL;
		if (get_user(len, p++))
			return -EFAULT;
		ret = import_single_range(WRITE, p, len, &iov, &iter);
		if (unlikely(ret))
			return ret;
		ret = write_pool_user(&iter);
		if (unlikely(ret < 0))
			return ret;
		/* Since we're crediting, enforce that it was all written into the pool. */
		if (unlikely(ret != len))
			return -EFAULT;
		credit_init_bits(ent_count);
		return 0;
	}
	case RNDZAPENTCNT:
	case RNDCLEARPOOL:
		/* No longer has any effect. */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
			
		input_pool.entropy_count = 0;

		return 0;
	case RNDRESEEDCRNG:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (!crng_ready())
			return -ENODATA;
		crng_reseed();
		return 0;
	default:
		return -EINVAL;
	}
}

static int random_fasync(int fd, struct file *filp, int on)
{
	return fasync_helper(fd, filp, on, &fasync);
}

const struct file_operations random_fops = {
	.read_iter = random_read_iter,
	.write_iter = random_write_iter,
	.poll = random_poll,
	.unlocked_ioctl = random_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fasync = random_fasync,
	.llseek = noop_llseek,
	.splice_read = generic_file_splice_read,
	.splice_write = iter_file_splice_write,
};

const struct file_operations urandom_fops = {
	.read_iter = urandom_read_iter,
	.write_iter = random_write_iter,
	.unlocked_ioctl = random_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fasync = random_fasync,
	.llseek = noop_llseek,
	.splice_read = generic_file_splice_read,
	.splice_write = iter_file_splice_write,
};


SYSCALL_DEFINE3(getrandom, char __user *, buf, size_t, count,
		unsigned int, flags)
{
	int ret;

	if (flags & ~(GRND_NONBLOCK|GRND_RANDOM|GRND_INSECURE))
		return -EINVAL;

	/*
	 * Requesting insecure and blocking randomness at the same time makes
	 * no sense.
	 */
	if ((flags & (GRND_INSECURE|GRND_RANDOM)) == (GRND_INSECURE|GRND_RANDOM))
		return -EINVAL;

	if (count > INT_MAX)
		count = INT_MAX;

	if (!(flags & GRND_INSECURE) && !crng_ready()) {
		if (flags & GRND_NONBLOCK)
			return -EAGAIN;
		ret = wait_for_random_bytes();
		if (unlikely(ret))
			return ret;
	}
	return urandom_read_nowarn(NULL, buf, count, NULL);
}


/********************************************************************
 *
 * Sysctl interface.
 *
 * These are partly unused legacy knobs with dummy values to not break
 * userspace and partly still useful things. They are usually accessible
 * in /proc/sys/kernel/random/ and are as follows:
 *
 * - boot_id - a UUID representing the current boot.
 *
 * - uuid - a random UUID, different each time the file is read.
 *
 * - poolsize - the number of bits of entropy that the input pool can
 *   hold, tied to the POOL_BITS constant.
 *
 * - entropy_avail - the number of bits of entropy currently in the
 *   input pool. Always <= poolsize.
 *
 * - write_wakeup_threshold - the amount of entropy in the input pool
 *   below which write polls to /dev/random will unblock, requesting
 *   more entropy, tied to the POOL_READY_BITS constant. It is writable
 *   to avoid breaking old userspaces, but writing to it does not
 *   change any behavior of the RNG.
 *
 * - urandom_min_reseed_secs - fixed to the value CRNG_RESEED_INTERVAL.
 *   It is writable to avoid breaking old userspaces, but writing
 *   to it does not change any behavior of the RNG.
 *
 ********************************************************************/

#ifdef CONFIG_SYSCTL

#include <linux/sysctl.h>


static int min_write_thresh;
static int max_write_thresh = INPUT_POOL_WORDS * 32;
static int random_min_urandom_seed = 60;
static char sysctl_bootid[16];
static int sysctl_random_min_urandom_seed = CRNG_RESEED_INTERVAL / HZ;
static int sysctl_random_write_wakeup_bits = POOL_READY_BITS;
static int sysctl_poolsize = POOL_BITS;
static u8 sysctl_bootid[UUID_SIZE];


/*
 * This function is used to return both the bootid UUID, and random
 * UUID. The difference is in whether table->data is NULL; if it is,
 * then a new UUID is generated and returned to the user.
 */
static int proc_do_uuid(struct ctl_table *table, int write, void __user *buf,
			size_t *lenp, loff_t *ppos)
{
	u8 tmp_uuid[UUID_SIZE], *uuid;
	char uuid_string[UUID_STRING_LEN + 1];
	struct ctl_table fake_table = {
		.data = uuid_string,
		.maxlen = UUID_STRING_LEN
	};

	if (write)
		return -EPERM;

	uuid = table->data;
	if (!uuid) {
		uuid = tmp_uuid;
		generate_random_uuid(uuid);
	} else {
		static DEFINE_SPINLOCK(bootid_spinlock);

		spin_lock(&bootid_spinlock);
		if (!uuid[8])
			generate_random_uuid(uuid);
		spin_unlock(&bootid_spinlock);
	}

	snprintf(uuid_string, sizeof(uuid_string), "%pU", uuid);
	return proc_dostring(&fake_table, 0, buf, lenp, ppos);
}

/* The same as proc_dointvec, but writes don't change anything. */
static int proc_do_rointvec(struct ctl_table *table, int write, void __user *buf,
			    size_t *lenp, loff_t *ppos)
{
	return write ? 0 : proc_dointvec(table, 0, buf, lenp, ppos);
}

extern struct ctl_table random_table[];
struct ctl_table random_table[] = {
	{
		.procname	= "poolsize",
		.data		= &sysctl_poolsize,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "entropy_avail",
		.data		= &input_pool.init_bits,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= proc_do_entropy,
		.data		= &input_pool.entropy_count,
=======
		.proc_handler	= proc_dointvec,
>>>>>>> 9ff959f9401b53ef1514579ac6d29545649db553
	},
	{
		.procname	= "write_wakeup_threshold",
		.data		= &sysctl_random_write_wakeup_bits,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_do_rointvec,
	},
	{
		.procname	= "urandom_min_reseed_secs",
		.data		= &sysctl_random_min_urandom_seed,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_do_rointvec,
	},
	{
		.procname	= "boot_id",
		.data		= &sysctl_bootid,
		.mode		= 0444,
		.proc_handler	= proc_do_uuid,
	},
	{
		.procname	= "uuid",
		.mode		= 0444,
		.proc_handler	= proc_do_uuid,
	},
	{ }
};
#endif	/* CONFIG_SYSCTL */
