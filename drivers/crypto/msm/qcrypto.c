/* Qualcomm Crypto driver
 *
 * Copyright (c) 2010-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/crypto.h>
#include <linux/kernel.h>
#include <linux/rtnetlink.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/llist.h>
#include <linux/debugfs.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/cache.h>
#include <linux/platform_data/qcom_crypto_device.h>
#include <linux/msm-bus.h>
#include <linux/hardirq.h>
#include <linux/qcrypto.h>

#include <crypto/ctr.h>
#include <crypto/des.h>
#include <crypto/aes.h>
#include <crypto/sha.h>
#include <crypto/hash.h>
#include <crypto/algapi.h>
#include <crypto/aead.h>
#include <crypto/authenc.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/hash.h>
#include <crypto/internal/aead.h>

#include <linux/fips_status.h>
#include "qcryptoi.h"
#include "qce.h"

#define DEBUG_MAX_FNAME  16
#define DEBUG_MAX_RW_BUF (5 * 1024)
#define QCRYPTO_BIG_NUMBER 9999999 /* a big number */

/*
 * For crypto 5.0 which has burst size alignment requirement.
 */
#define MAX_ALIGN_SIZE  0x40

#define QCRYPTO_HIGH_BANDWIDTH_TIMEOUT 1000

/* are FIPS self tests done ?? */
static bool is_fips_qcrypto_tests_done;

/* Status of response workq */
enum resp_workq_sts {
	NOT_SCHEDULED  = 0,
	IS_SCHEDULED   = 1,
	SCHEDULE_AGAIN = 2
};

/* Status of req processing by CEs */
enum req_processing_sts {
	STOPPED     = 0,
	IN_PROGRESS = 1
};

enum qcrypto_bus_state {
	BUS_NO_BANDWIDTH = 0,
	BUS_HAS_BANDWIDTH,
	BUS_BANDWIDTH_RELEASING,
	BUS_BANDWIDTH_ALLOCATING,
	BUS_SUSPENDED,
	BUS_SUSPENDING,
};

struct crypto_stat {
	u64 aead_sha1_aes_enc;
	u64 aead_sha1_aes_dec;
	u64 aead_sha1_des_enc;
	u64 aead_sha1_des_dec;
	u64 aead_sha1_3des_enc;
	u64 aead_sha1_3des_dec;
	u64 aead_sha256_aes_enc;
	u64 aead_sha256_aes_dec;
	u64 aead_sha256_des_enc;
	u64 aead_sha256_des_dec;
	u64 aead_sha256_3des_enc;
	u64 aead_sha256_3des_dec;
	u64 aead_ccm_aes_enc;
	u64 aead_ccm_aes_dec;
	u64 aead_rfc4309_ccm_aes_enc;
	u64 aead_rfc4309_ccm_aes_dec;
	u64 aead_op_success;
	u64 aead_op_fail;
	u64 aead_bad_msg;
	u64 ablk_cipher_aes_enc;
	u64 ablk_cipher_aes_dec;
	u64 ablk_cipher_des_enc;
	u64 ablk_cipher_des_dec;
	u64 ablk_cipher_3des_enc;
	u64 ablk_cipher_3des_dec;
	u64 ablk_cipher_op_success;
	u64 ablk_cipher_op_fail;
	u64 sha1_digest;
	u64 sha256_digest;
	u64 sha1_hmac_digest;
	u64 sha256_hmac_digest;
	u64 ahash_op_success;
	u64 ahash_op_fail;
};
static struct crypto_stat _qcrypto_stat;
static struct dentry *_debug_dent;
static char _debug_read_buf[DEBUG_MAX_RW_BUF];
static bool _qcrypto_init_assign;
struct crypto_priv;

#define QCRYPTO_BUF_SZ	256

struct qcrypto_buf {
	char		buf[QCRYPTO_BUF_SZ];
	unsigned int	used;
};

struct qcrypto_req_control {
	unsigned int index;
	bool in_use;
	struct crypto_engine *pce;
	struct crypto_async_request *req;
	struct qcrypto_resp_ctx *arsp;
	int res; /* execution result */
};

#define DEFAULT_POLL_INTVAL	350
#define MICRO_TO_NS(us)		(us * 1000)
#define MAX_POLL_INTVAL		1000000
#define DEFAULT_POLL_CPU	1
#define DEFAULT_POLL_RETRY	100

struct crypto_poll_ctl {
	struct list_head	list;
	struct crypto_priv	*cp;
	struct workqueue_struct	*wq;
	enum resp_workq_sts	state;
	struct work_struct	work;
	struct tasklet_struct	task;
	struct hrtimer		timer;
	struct list_head	engine_list;
	spinlock_t		lock;
	struct crypto_engine	*first_engine_to_poll;
	bool			pm_support;

	int			cpu;		/* which CPU it is on */
	ktime_t			intval;		/* polling interval */
	u32			retry;
	u32			max_retry;

	u64	sched;
	u64	poll;
};

struct crypto_engine {
	struct list_head elist;
	struct list_head plist;
	void *qce; /* qce handle */
	struct platform_device *pdev; /* platform device */
	struct crypto_priv *pcp;
	struct crypto_poll_ctl *pctl;

	uint32_t  bus_scale_handle;
	struct crypto_queue areq_queue;	/*
					 * request queue for those requests
					 * that have this engine assigned
					 * waiting to be executed
					 */
	struct crypto_queue req_queue;	/* request queue for all the
					 * requests ready to be execute.
					 * There is no locking protection */
	u64 total_req;
	u64 err_req;
	u32 unit;
	u32 ce_device;
	u32 ce_hw_instance;
	unsigned int signature;

	enum qcrypto_bus_state bw_state;
	struct timer_list bw_reaper_timer;
	struct work_struct bw_reaper_ws;
	struct work_struct bw_allocate_ws;

	/* engine execution sequence number */
	u32    active_seq;
	/* last QCRYPTO_HIGH_BANDWIDTH_TIMEOUT active_seq */
	u32    last_active_seq;

	bool   check_flag;
	/*Added to support multi-requests*/
	unsigned int max_req;
	struct   qcrypto_req_control *preq_pool;
	unsigned int req_count;
	unsigned int max_req_used; /* debug stats */
};

#define MAX_SMP_CPU    8
#define COMP_WQ_CPU    3
#define POLLING_CPU    1

struct crypto_priv {
	/* CE features supported by target device*/
	struct msm_ce_hw_support platform_support;

	/* CE features/algorithms supported by HW engine*/
	struct ce_hw_support ce_support;

	/* the lock protects crypto queue and req */
	spinlock_t lock;

	/* list of  registered algorithms */
	struct list_head alg_list;

	/* current active request */
	struct crypto_async_request *req;

	struct work_struct unlock_ce_ws;
	struct list_head engine_list; /* list of  qcrypto engines */
	int32_t total_units;   /* total units of engines */
	struct mutex engine_lock;

	struct crypto_engine *next_engine; /* next assign engine */
	struct crypto_queue req_queue;	/*
					 * request queue for those requests
					 * that waiting for an available
					 * engine.
					 */
	struct llist_head ordered_resp_list;	/* Queue to maintain
						 * responses in sequence.
						 */
	atomic_t resp_cnt;
	struct tasklet_struct resp_wq;	/*
					 * tasklet to send responses
					 * in sequence.
					 */
	enum resp_workq_sts sched_resp_workq_status;
	enum req_processing_sts ce_req_proc_sts;
	struct crypto_engine *first_engine;

	/* polling */
	struct list_head	poll_ctl_list;
	u32			poll_ctl_num;

	/* debug stats */
	unsigned no_avail;
	unsigned resp_stop;
	unsigned resp_start;
	unsigned max_qlen;
	unsigned int max_resp_qlen;
	unsigned int max_reorder_cnt;
	u64 queue_complete_work;
	u64 req_drop_cnt;
};

static struct crypto_priv qcrypto_dev;
static struct crypto_engine *_qcrypto_static_assign_engine(
					struct crypto_priv *cp);

static inline void qcrypto_poll_ctl_sched(struct crypto_poll_ctl *ctl)
{
	if (!hrtimer_active(&ctl->timer) &&
	    (cmpxchg(&ctl->state, NOT_SCHEDULED, IS_SCHEDULED) ==
	     NOT_SCHEDULED)) {
		queue_work_on(ctl->cpu, ctl->wq, &ctl->work);
		ctl->sched++;
	}
}

static inline bool __qcrypto_engine_req_queue_empty(
	struct crypto_engine *pengine)
{
	return (!pengine->req_queue.qlen && !pengine->areq_queue.qlen);
}

static struct qcrypto_req_control *qcrypto_alloc_req_control(
						struct crypto_engine *pce)
{
	int i;
	struct qcrypto_req_control *pqcrypto_req_control = pce->preq_pool;

	for (i = 0; i < pce->max_req; i++) {
		if (!pqcrypto_req_control->in_use) {
			pqcrypto_req_control->in_use = true;
			if (++pce->req_count > pce->max_req_used)
				pce->max_req_used = pce->req_count;
			return pqcrypto_req_control;
		}
		pqcrypto_req_control++;
	}
	return NULL;
}

static void qcrypto_free_req_control(struct crypto_engine *pce,
					struct qcrypto_req_control *preq)
{
	/* do this before free req */
	preq->req = NULL;
	preq->arsp = NULL;
	/* free req */
	if (preq->in_use == false) {
		pr_warn("request info %p free already\n", preq);
	} else {
		preq->in_use = false;
		pce->req_count--;
	}
}

static struct qcrypto_req_control *find_req_control_for_areq(
					struct crypto_engine *pce,
					struct crypto_async_request *areq)
{
	int i;
	struct qcrypto_req_control *pqcrypto_req_control = pce->preq_pool;

	for (i = 0; i < pce->max_req; i++) {
		if (pqcrypto_req_control->req == areq)
			return pqcrypto_req_control;
		pqcrypto_req_control++;
	}
	return NULL;
}

static void qcrypto_init_req_control(struct crypto_engine *pce,
			struct qcrypto_req_control *pqcrypto_req_control)
{
	int i;

	pce->preq_pool = pqcrypto_req_control;
	pce->req_count = 0;
	for (i = 0; i < pce->max_req; i++) {
		pqcrypto_req_control->index = i;
		pqcrypto_req_control->in_use = false;
		pqcrypto_req_control->pce = pce;
		pqcrypto_req_control++;
	}
}

static inline void *qcrypto_kzalloc(
	struct qcrypto_buf *p,
	unsigned int size,
	gfp_t flags)
{
	char *buf;

	if (!p || ((p->used + size) > QCRYPTO_BUF_SZ))
		return kzalloc(size, flags);

	buf = (char *)p->buf + p->used;
	p->used += size;
	memset(buf, 0, size);

	return buf;
}

static inline void qcrypto_kfree(struct qcrypto_buf *p, void *buf)
{
	if (buf) {
		if (p) {
			unsigned long offset =
				(unsigned long)(buf) - (unsigned long)(p->buf);

			if (offset < QCRYPTO_BUF_SZ)
				return;
		}
		kfree(buf);
	}
}

static struct crypto_engine *_qrypto_find_pengine_device(struct crypto_priv *cp,
			 unsigned int device)
{
	struct crypto_engine *entry = NULL;
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	list_for_each_entry(entry, &cp->engine_list, elist) {
		if (entry->ce_device == device)
			break;
	}
	spin_unlock_irqrestore(&cp->lock, flags);

	if (((entry != NULL) && (entry->ce_device != device)) ||
		(entry == NULL)) {
		pr_err("Device node for CE device %d NOT FOUND!!\n",
				device);
		return NULL;
	}

	return entry;
}

static struct crypto_engine *_qrypto_find_pengine_device_hw
			(struct crypto_priv *cp,
			u32 device,
			u32 hw_instance)
{
	struct crypto_engine *entry = NULL;
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	list_for_each_entry(entry, &cp->engine_list, elist) {
		if ((entry->ce_device == device) &&
			(entry->ce_hw_instance == hw_instance))
			break;
	}
	spin_unlock_irqrestore(&cp->lock, flags);

	if (((entry != NULL) &&
		((entry->ce_device != device)
		|| (entry->ce_hw_instance != hw_instance)))
		|| (entry == NULL)) {
		pr_err("Device node for CE device %d NOT FOUND!!\n",
						 device);
		return NULL;
	}
	return entry;
}

int qcrypto_get_num_engines(void)
{
	struct crypto_priv *cp = &qcrypto_dev;
	struct crypto_engine *entry = NULL;
	int count = 0;

	list_for_each_entry(entry, &cp->engine_list, elist) {
		count++;
	}
	return count;
}
EXPORT_SYMBOL(qcrypto_get_num_engines);

void qcrypto_get_engine_list(size_t num_engines,
				struct crypto_engine_entry *arr)
{
	struct crypto_priv *cp = &qcrypto_dev;
	struct crypto_engine *entry = NULL;
	size_t arr_index = 0;

	list_for_each_entry(entry, &cp->engine_list, elist) {
			arr[arr_index].ce_device = entry->ce_device;
			arr[arr_index].hw_instance = entry->ce_hw_instance;
			arr_index++;
			if (arr_index >= num_engines)
				break;
	}
}
EXPORT_SYMBOL(qcrypto_get_engine_list);

enum qcrypto_alg_type {
	QCRYPTO_ALG_CIPHER	= 0,
	QCRYPTO_ALG_SHA	= 1,
	QCRYPTO_ALG_LAST
};

struct qcrypto_alg {
	struct list_head entry;
	struct crypto_alg cipher_alg;
	struct ahash_alg sha_alg;
	enum qcrypto_alg_type alg_type;
	struct crypto_priv *cp;
};

#define QCRYPTO_MAX_KEY_SIZE	64

/* max of AES_BLOCK_SIZE, DES3_EDE_BLOCK_SIZE */
#define QCRYPTO_MAX_IV_LENGTH	16

#define	QCRYPTO_CCM4309_NONCE_LEN	3
#define ESP_EXT_SEQ_HI_OFFSET	4
#define ESP_EXT_SEQ_HI_SIZE	4
#define ESP_EXT_SEQ_LOW_SIZE	4

struct qcrypto_cipher_ctx {
	struct list_head rsp_queue;     /* response queue */
	struct crypto_engine *pengine;  /* fixed engine assigned to this tfm */
	struct crypto_priv *cp;
	unsigned int flags;

	enum qce_hash_alg_enum  auth_alg; /* for aead */
	u8 auth_key[QCRYPTO_MAX_KEY_SIZE] __aligned(4); /* 32-bit alignment */
	u8 iv[QCRYPTO_MAX_IV_LENGTH] __aligned(4); /* 32-bit alignment */

	u8 enc_key[QCRYPTO_MAX_KEY_SIZE] __aligned(4); /* 32-bit alignment */
	unsigned int enc_key_len;

	unsigned int authsize;
	unsigned int auth_key_len;

	u8 ccm4309_nonce[QCRYPTO_CCM4309_NONCE_LEN];

	struct crypto_ablkcipher *cipher_aes192_fb;

	struct crypto_ahash *ahash_aead_aes192_fb;

	bool aead_esn;	/*
			 * Support esn or not.
			 * For aead of separate Encryption and Integrity
			 * algorithm only.
			 */
};

struct qcrypto_resp_ctx {
	struct list_head list;
	struct llist_node llist;
	struct crypto_async_request *async_req; /* async req */
	int res;                                /* execution result */
};

struct qcrypto_cipher_req_ctx {
	struct qcrypto_resp_ctx rsp_entry;/* rsp entry. */
	struct crypto_engine *pengine;  /* engine assigned to this request */
	u8 *iv;
	/* 32-bit alignment*/
	u8 rfc4309_iv[QCRYPTO_MAX_IV_LENGTH] __aligned(4);
	unsigned int ivsize;
	int  aead;
	unsigned char *assoc;		/* Pointer to formatted assoc data */
	size_t assoc_1_len;
	enum qce_cipher_alg_enum alg;
	enum qce_cipher_dir_enum dir;
	enum qce_cipher_mode_enum mode;

	struct scatterlist *orig_src;	/* Original src sg ptr  */
	struct scatterlist *orig_dst;	/* Original dst sg ptr  */
	struct scatterlist dsg;		/* Dest Data sg  */
	struct scatterlist ssg;		/* Source Data sg  */
	unsigned char *data;		/* Incoming data pointer*/

	struct aead_request *aead_req;
	struct ahash_request *fb_hash_req;
	/* 32-bit alignment*/
	uint8_t fb_ahash_digest[SHA256_DIGEST_SIZE] __aligned(4);
	struct scatterlist *fb_ahash_sg;
	char *fb_ahash_assoc_iv;
	char *fb_aes_iv;
	unsigned int  fb_ahash_length;
	struct ablkcipher_request *fb_aes_req;
	struct scatterlist *fb_aes_src;
	struct scatterlist *fb_aes_dst;
	unsigned int  fb_aes_cryptlen;
	struct qcrypto_buf qbuf;
};

#define SHA_MAX_BLOCK_SIZE      SHA256_BLOCK_SIZE
#define SHA_MAX_STATE_SIZE	(SHA256_DIGEST_SIZE / sizeof(u32))
#define SHA_MAX_DIGEST_SIZE	 SHA256_DIGEST_SIZE

#define	MSM_QCRYPTO_REQ_QUEUE_LENGTH 768
#define	COMPLETION_CB_BACKLOG_LENGTH_STOP 400
#define	COMPLETION_CB_BACKLOG_LENGTH_START \
			(COMPLETION_CB_BACKLOG_LENGTH_STOP / 2)

static uint8_t  _std_init_vector_sha1_uint8[] =   {
	0x67, 0x45, 0x23, 0x01, 0xEF, 0xCD, 0xAB, 0x89,
	0x98, 0xBA, 0xDC, 0xFE, 0x10, 0x32, 0x54, 0x76,
	0xC3, 0xD2, 0xE1, 0xF0
};

/* standard initialization vector for SHA-256, source: FIPS 180-2 */
static uint8_t _std_init_vector_sha256_uint8[] = {
	0x6A, 0x09, 0xE6, 0x67, 0xBB, 0x67, 0xAE, 0x85,
	0x3C, 0x6E, 0xF3, 0x72, 0xA5, 0x4F, 0xF5, 0x3A,
	0x51, 0x0E, 0x52, 0x7F, 0x9B, 0x05, 0x68, 0x8C,
	0x1F, 0x83, 0xD9, 0xAB, 0x5B, 0xE0, 0xCD, 0x19
};

struct qcrypto_sha_ctx {
	struct list_head rsp_queue;     /* response queue */
	struct crypto_engine *pengine;  /* fixed engine assigned to this tfm */
	struct crypto_priv *cp;
	unsigned int flags;
	enum qce_hash_alg_enum  alg;
	uint32_t		diglen;
	uint32_t		authkey_in_len;
	/* 32-bit alignment */
	uint8_t		authkey[SHA_MAX_BLOCK_SIZE] __aligned(4);
	struct ahash_request *ahash_req;
	struct completion ahash_req_complete;
};

struct qcrypto_sha_req_ctx {
	struct qcrypto_resp_ctx rsp_entry;/* rsp entry. */
	struct crypto_engine *pengine;  /* engine assigned to this request */

	struct scatterlist *src;
	uint32_t nbytes;

	struct scatterlist *orig_src;	/* Original src sg ptr  */
	struct scatterlist dsg;		/* Data sg */
	unsigned char *data;		/* Incoming data pointer*/
	unsigned char *data2;		/* Updated data pointer*/

	uint32_t byte_count[4];
	u64 count;
	uint8_t	first_blk;
	uint8_t	last_blk;
	/* 32-bit alignment */
	uint8_t trailing_buf[SHA_MAX_BLOCK_SIZE] __aligned(4);
	uint32_t trailing_buf_len;

	/* dma buffer, Internal use */
	uint8_t	staging_dmabuf
		[SHA_MAX_BLOCK_SIZE+SHA_MAX_DIGEST_SIZE+MAX_ALIGN_SIZE];

	uint8_t	digest[SHA_MAX_DIGEST_SIZE] __aligned(4); /* 32-bit alignment */
	struct scatterlist sg[2];

	struct qcrypto_buf qbuf;
};

static void _byte_stream_to_words(uint32_t *iv, unsigned char *b,
		unsigned int len)
{
	unsigned n;

	n = len  / sizeof(uint32_t);
	for (; n > 0; n--) {
		*iv =  ((*b << 24)      & 0xff000000) |
				(((*(b+1)) << 16) & 0xff0000)   |
				(((*(b+2)) << 8) & 0xff00)     |
				(*(b+3)          & 0xff);
		b += sizeof(uint32_t);
		iv++;
	}

	n = len %  sizeof(uint32_t);
	if (n == 3) {
		*iv = ((*b << 24) & 0xff000000) |
				(((*(b+1)) << 16) & 0xff0000)   |
				(((*(b+2)) << 8) & 0xff00);
	} else if (n == 2) {
		*iv = ((*b << 24) & 0xff000000) |
				(((*(b+1)) << 16) & 0xff0000);
	} else if (n == 1) {
		*iv = ((*b << 24) & 0xff000000);
	}
}

static void _words_to_byte_stream(uint32_t *iv, unsigned char *b,
		unsigned int len)
{
	unsigned n = len  / sizeof(uint32_t);

	for (; n > 0; n--) {
		*b++ = (unsigned char) ((*iv >> 24)   & 0xff);
		*b++ = (unsigned char) ((*iv >> 16)   & 0xff);
		*b++ = (unsigned char) ((*iv >> 8)    & 0xff);
		*b++ = (unsigned char) (*iv           & 0xff);
		iv++;
	}
	n = len % sizeof(uint32_t);
	if (n == 3) {
		*b++ = (unsigned char) ((*iv >> 24)   & 0xff);
		*b++ = (unsigned char) ((*iv >> 16)   & 0xff);
		*b =   (unsigned char) ((*iv >> 8)    & 0xff);
	} else if (n == 2) {
		*b++ = (unsigned char) ((*iv >> 24)   & 0xff);
		*b =   (unsigned char) ((*iv >> 16)   & 0xff);
	} else if (n == 1) {
		*b =   (unsigned char) ((*iv >> 24)   & 0xff);
	}
}

static void qcrypto_ce_set_bus(struct crypto_engine *pengine,
				 bool high_bw_req)
{
	int ret = 0;

	if (high_bw_req) {
		ret = qce_enable_clk(pengine->qce);
		if (ret) {
			pr_err("%s Unable enable clk\n", __func__);
			goto clk_err;
		}
		ret = msm_bus_scale_client_update_request(
				pengine->bus_scale_handle, 1);
		if (ret) {
			pr_err("%s Unable to set to high bandwidth\n",
						__func__);
			qce_disable_clk(pengine->qce);
			goto clk_err;
		}
	} else {
		ret = msm_bus_scale_client_update_request(
				pengine->bus_scale_handle, 0);
		if (ret) {
			pr_err("%s Unable to set to low bandwidth\n",
						__func__);
			goto clk_err;
		}
		ret = qce_disable_clk(pengine->qce);
		if (ret) {
			pr_err("%s Unable disable clk\n", __func__);
			ret = msm_bus_scale_client_update_request(
				pengine->bus_scale_handle, 1);
			if (ret)
				pr_err("%s Unable to set to high bandwidth\n",
						__func__);
			goto clk_err;
		}
	}
clk_err:
	return;

}

static void qcrypto_bw_reaper_timer_callback(unsigned long data)
{
	struct crypto_engine *pengine = (struct crypto_engine *)data;

	schedule_work(&pengine->bw_reaper_ws);

	return;
}

static void qcrypto_bw_set_timeout(struct crypto_engine *pengine)
{
	pengine->bw_reaper_timer.data =
			(unsigned long)(pengine);
	pengine->bw_reaper_timer.expires = jiffies +
			msecs_to_jiffies(QCRYPTO_HIGH_BANDWIDTH_TIMEOUT);
	mod_timer(&(pengine->bw_reaper_timer),
		pengine->bw_reaper_timer.expires);
}

static void qcrypto_ce_bw_allocate_req(struct crypto_engine *pengine)
{
	schedule_work(&pengine->bw_allocate_ws);
}

static int _start_qcrypto_process(struct crypto_priv *cp,
					struct crypto_engine *pengine);

static void qcrypto_bw_allocate_work(struct work_struct *work)
{
	struct  crypto_engine *pengine = container_of(work,
				struct crypto_engine, bw_allocate_ws);
	struct crypto_poll_ctl *ctl = pengine->pctl;

	spin_lock_bh(&ctl->lock);
	pengine->bw_state = BUS_BANDWIDTH_ALLOCATING;
	spin_unlock_bh(&ctl->lock);

	qcrypto_ce_set_bus(pengine, true);
	qcrypto_bw_set_timeout(pengine);
	spin_lock_bh(&ctl->lock);
	pengine->bw_state = BUS_HAS_BANDWIDTH;
	pengine->active_seq++;
	pengine->check_flag = true;
	spin_unlock_bh(&ctl->lock);

	qcrypto_poll_ctl_sched(ctl);
};

static void qcrypto_bw_reaper_work(struct work_struct *work)
{
	struct  crypto_engine *pengine = container_of(work,
				struct crypto_engine, bw_reaper_ws);
	struct crypto_poll_ctl *ctl = pengine->pctl;
	struct crypto_priv *cp = pengine->pcp;
	u32    active_seq;
	bool restart = false;

	spin_lock_bh(&ctl->lock);
	active_seq = pengine->active_seq;
	if (pengine->bw_state == BUS_HAS_BANDWIDTH &&
		(active_seq == pengine->last_active_seq)) {

		/* check if engine is stuck */
		if (pengine->req_count > 0) {
			if (pengine->check_flag)
				dev_warn(&pengine->pdev->dev,
				"The engine appears to be stuck seq %d.\n",
				active_seq);
			pengine->check_flag = false;
			goto ret;
		}
		if (cp->platform_support.bus_scale_table == NULL)
			goto ret;
		pengine->bw_state = BUS_BANDWIDTH_RELEASING;
		spin_unlock_bh(&ctl->lock);

		qcrypto_ce_set_bus(pengine, false);

		spin_lock_bh(&ctl->lock);

		if (!__qcrypto_engine_req_queue_empty(pengine)) {
			/* we got request while we are disabling clock */
			pengine->bw_state = BUS_BANDWIDTH_ALLOCATING;
			spin_unlock_bh(&ctl->lock);

			qcrypto_ce_set_bus(pengine, true);

			spin_lock_bh(&ctl->lock);
			pengine->bw_state = BUS_HAS_BANDWIDTH;
			restart = true;
		} else
			pengine->bw_state = BUS_NO_BANDWIDTH;
	}
ret:
	pengine->last_active_seq = active_seq;
	spin_unlock_bh(&ctl->lock);
	if (restart)
		qcrypto_poll_ctl_sched(ctl);
	if (pengine->bw_state != BUS_NO_BANDWIDTH)
		qcrypto_bw_set_timeout(pengine);
}

static inline void __qcrypto_wq_sched(struct crypto_priv *cp)
{
	while (!llist_empty(&cp->ordered_resp_list)) {
		unsigned int cpu = COMP_WQ_CPU;

		if (cmpxchg(&cp->sched_resp_workq_status, NOT_SCHEDULED,
					IS_SCHEDULED) == NOT_SCHEDULED) {
			smp_call_function_single(
				cpu,
				(smp_call_func_t)tasklet_schedule,
				&cp->resp_wq,
				0);
			cp->queue_complete_work++;
		} else if (cmpxchg(&cp->sched_resp_workq_status, IS_SCHEDULED,
					SCHEDULE_AGAIN) == NOT_SCHEDULED)
			continue;
		break;
	}
}

static inline struct crypto_engine *next_engine_to_poll(struct crypto_engine *p)
{
	struct crypto_poll_ctl *ctl = p->pctl;

	if (list_is_last(&p->plist, &ctl->engine_list))
		p =  list_first_entry(&ctl->engine_list, struct crypto_engine,
				      plist);
	else
		p = list_next_entry(p, plist);
	return p;
}

static enum hrtimer_restart qcrypto_poll_timeout(struct hrtimer *timer)
{
	struct crypto_poll_ctl *ctl = container_of(timer,
						   struct crypto_poll_ctl,
						   timer);

	hrtimer_forward_now(timer, ctl->intval);
	tasklet_schedule(&ctl->task);

	return HRTIMER_RESTART;
}

static void qcrypto_poll_task(unsigned long data)
{
	struct crypto_poll_ctl *ctl = (struct crypto_poll_ctl *)data;
	struct crypto_engine *pengine;
	u32 n = 0;
	bool restart_timer = true;

	if (ctl->pm_support)
		spin_lock(&ctl->lock);
	ctl->poll++;

	if (ctl->first_engine_to_poll == NULL)
		goto out;

	/* Check if any pending response to poll */
	for (pengine = ctl->first_engine_to_poll; pengine;) {
		switch (pengine->bw_state) {
		case BUS_HAS_BANDWIDTH:
			qce_poll_eot(pengine->qce);
			_start_qcrypto_process(pengine->pcp, pengine);
			n += pengine->req_count;
			break;
		case BUS_NO_BANDWIDTH:
			if (ctl->pm_support &&
			    !__qcrypto_engine_req_queue_empty(pengine) &&
			    !work_pending(&pengine->bw_allocate_ws))
				qcrypto_ce_bw_allocate_req(pengine);
			break;
		default:
			break;
		}
		pengine = next_engine_to_poll(pengine);
		if (pengine == ctl->first_engine_to_poll)
			break;
	}

	__qcrypto_wq_sched(ctl->cp);

out:
	if (ctl->pm_support)
		spin_unlock(&ctl->lock);

	if (!n) {
		if (++ctl->retry > ctl->max_retry) {
			ctl->retry = 0;
			restart_timer = false;
		}
	} else
		ctl->retry = 0;

	if (!restart_timer) {
		hrtimer_cancel(&ctl->timer);
		ctl->state = NOT_SCHEDULED;
	}
}

static void qcrypto_poll_work(struct work_struct *work)
{
	struct crypto_poll_ctl *ctl = container_of(work, struct crypto_poll_ctl,
						   work);

	hrtimer_start(&ctl->timer, ctl->intval,
		      HRTIMER_MODE_REL|HRTIMER_MODE_PINNED);
	ctl->state = IS_SCHEDULED;
}

static struct crypto_poll_ctl *qcrypto_poll_ctl_alloc(struct crypto_priv *cp,
						     int cpu)
{
	struct crypto_poll_ctl *ctl = NULL;

	list_for_each_entry(ctl, &cp->poll_ctl_list, list) {
		if (ctl->cpu == cpu)
			return ctl;
	}

	ctl = kzalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return NULL;

	ctl->wq = alloc_workqueue(
		"qcrypto_poll_wq",
		WQ_MEM_RECLAIM | WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!ctl->wq) {
		pr_err("Error allocating workqueue\n");
		kfree(ctl);
		return NULL;
	}

	INIT_WORK(&ctl->work, qcrypto_poll_work);
	tasklet_init(&ctl->task, qcrypto_poll_task, (unsigned long)ctl);
	hrtimer_init(&ctl->timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL|HRTIMER_MODE_PINNED);
	ctl->timer.function = qcrypto_poll_timeout;
	spin_lock_init(&ctl->lock);
	INIT_LIST_HEAD(&ctl->engine_list);

	ctl->cp = cp;
	ctl->cpu = cpu;
	ctl->intval = ns_to_ktime(MICRO_TO_NS(DEFAULT_POLL_INTVAL));
	ctl->state = NOT_SCHEDULED;
	ctl->max_retry = DEFAULT_POLL_RETRY;
	ctl->pm_support = (cp->ce_support.clk_mgmt_sus_res ||
			   cp->platform_support.bus_scale_table) ? true : false;
	list_add_tail(&ctl->list, &cp->poll_ctl_list);
	cp->poll_ctl_num++;

	return ctl;
}

static void qcrypto_poll_ctl_free(struct crypto_poll_ctl *ctl)
{
	cancel_work_sync(&ctl->work);
	destroy_workqueue(ctl->wq);
	hrtimer_cancel(&ctl->timer);
	tasklet_kill(&ctl->task);

	list_del(&ctl->list);
	ctl->cp->poll_ctl_num--;
	kfree(ctl);
}

static void qcrypto_poll_ctl_free_all(struct crypto_priv *cp)
{
	struct crypto_poll_ctl *ctl;

	while ((ctl = list_first_entry_or_null(&cp->poll_ctl_list,
					      struct crypto_poll_ctl,
					      list))) {
		qcrypto_poll_ctl_free(ctl);
	}
}

static int __qcrypto_poll_ctl_engine_attach(struct crypto_engine *pengine,
					    struct crypto_poll_ctl *ctl)
{
	list_add_tail(&pengine->plist, &ctl->engine_list);
	pengine->pctl = ctl;
	if (!ctl->first_engine_to_poll)
		ctl->first_engine_to_poll = pengine;

	return 0;
}

static void __qcrypto_poll_ctl_engine_detach(struct crypto_engine *pengine)
{
	if (pengine->pctl->first_engine_to_poll == pengine) {
		pengine->pctl->first_engine_to_poll =
			next_engine_to_poll(pengine);
		if (pengine->pctl->first_engine_to_poll == pengine)
			pengine->pctl->first_engine_to_poll = NULL;
	}
	list_del(&pengine->plist);
	pengine->pctl = NULL;
}

static inline void qcrypto_poll_ctl_engine_detach(struct crypto_engine *pengine)
{
	if (pengine->pctl) {
		spin_lock_bh(&pengine->pctl->lock);
		__qcrypto_poll_ctl_engine_detach(pengine);
		spin_unlock_bh(&pengine->pctl->lock);
	}
}

static inline int qcrypto_engine_set_polling_cpu(struct crypto_engine *pengine,
						 int cpu)
{
	struct crypto_poll_ctl *ctl = qcrypto_poll_ctl_alloc(pengine->pcp, cpu);
	int ret;

	if (!ctl)
		return -ENOMEM;

	spin_lock_bh(&ctl->lock);
	ret = __qcrypto_poll_ctl_engine_attach(pengine, ctl);
	spin_unlock_bh(&ctl->lock);
	return ret;
}

static int qcrypto_count_sg(struct scatterlist *sg, int nbytes)
{
	int i;

	for (i = 0; (nbytes > 0) && sg; i++, sg = scatterwalk_sg_next(sg))
		nbytes -= sg->length;

	return i;
}

static size_t qcrypto_sg_copy_from_buffer(struct scatterlist *sgl,
				unsigned int nents, void *buf, size_t buflen)
{
	int i;
	size_t offset, len;

	for (i = 0, offset = 0; (i < nents) && sgl; ++i) {
		len = sg_copy_from_buffer(sgl, 1, buf, buflen);
		buf += len;
		buflen -= len;
		offset += len;
		sgl = scatterwalk_sg_next(sgl);
	}

	return offset;
}

/*
 * Duplicate scatter gather list from src sgl to dst sgl for data of buflen.
 * The dst sgl is a contiguous array of at least nents entries
 * The function returns number of entries consumed in the dst sgl if >= 0,
 * or negiative error code if error.
 */
static int qcrypto_dup_sg_len(struct scatterlist *src_sgl, unsigned int nents,
		struct scatterlist *dst_sgl, size_t buflen)
{
	size_t len;
	int nents_used;

	for (nents_used = 0; nents && src_sgl && buflen; nents--) {
		len = min(src_sgl->length, buflen);
		sg_set_page(dst_sgl, sg_page(src_sgl), len, src_sgl->offset);
		nents_used++;
		buflen -= len;
		src_sgl = scatterwalk_sg_next(src_sgl);
		dst_sgl++;
	}
	if (buflen == 0)
		return nents_used;
	else
		return -EINVAL;
}

static size_t qcrypto_sg_copy_to_buffer(struct scatterlist *sgl,
				unsigned int nents, void *buf, size_t buflen)
{
	int i;
	size_t offset, len;

	for (i = 0, offset = 0; (i < nents) && sgl; ++i) {
		len = sg_copy_to_buffer(sgl, 1, buf, buflen);
		buf += len;
		buflen -= len;
		offset += len;
		sgl = scatterwalk_sg_next(sgl);
	}

	return offset;
}
static struct qcrypto_alg *_qcrypto_sha_alg_alloc(struct crypto_priv *cp,
		struct ahash_alg *template)
{
	struct qcrypto_alg *q_alg;
	q_alg = kzalloc(sizeof(struct qcrypto_alg), GFP_KERNEL);
	if (!q_alg) {
		pr_err("qcrypto Memory allocation of q_alg FAIL, error %ld\n",
				PTR_ERR(q_alg));
		return ERR_PTR(-ENOMEM);
	}

	q_alg->alg_type = QCRYPTO_ALG_SHA;
	q_alg->sha_alg = *template;
	q_alg->cp = cp;

	return q_alg;
};

static struct qcrypto_alg *_qcrypto_cipher_alg_alloc(struct crypto_priv *cp,
		struct crypto_alg *template)
{
	struct qcrypto_alg *q_alg;

	q_alg = kzalloc(sizeof(struct qcrypto_alg), GFP_KERNEL);
	if (!q_alg) {
		pr_err("qcrypto Memory allocation of q_alg FAIL, error %ld\n",
				PTR_ERR(q_alg));
		return ERR_PTR(-ENOMEM);
	}

	q_alg->alg_type = QCRYPTO_ALG_CIPHER;
	q_alg->cipher_alg = *template;
	q_alg->cp = cp;

	return q_alg;
};

static int _qcrypto_cipher_cra_init(struct crypto_tfm *tfm)
{
	struct crypto_alg *alg = tfm->__crt_alg;
	struct qcrypto_alg *q_alg;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	/* IF FIPS tests not passed, return error */
	if (((g_fips140_status == FIPS140_STATUS_FAIL) ||
		(g_fips140_status == FIPS140_STATUS_PASS_CRYPTO)) &&
		is_fips_qcrypto_tests_done)
		return -ENXIO;

	q_alg = container_of(alg, struct qcrypto_alg, cipher_alg);
	ctx->flags = 0;

	/* update context with ptr to cp */
	ctx->cp = q_alg->cp;

	/* random first IV */
	get_random_bytes(ctx->iv, QCRYPTO_MAX_IV_LENGTH);
	if (_qcrypto_init_assign) {
		ctx->pengine = _qcrypto_static_assign_engine(ctx->cp);
		if (ctx->pengine == NULL)
			return -ENODEV;
	} else
		ctx->pengine = NULL;
	INIT_LIST_HEAD(&ctx->rsp_queue);
	ctx->auth_alg = QCE_HASH_LAST;
	return 0;
};

static int _qcrypto_ahash_cra_init(struct crypto_tfm *tfm)
{
	struct crypto_ahash *ahash = __crypto_ahash_cast(tfm);
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(tfm);
	struct ahash_alg *alg =	container_of(crypto_hash_alg_common(ahash),
						struct ahash_alg, halg);
	struct qcrypto_alg *q_alg = container_of(alg, struct qcrypto_alg,
								sha_alg);

	/* IF FIPS tests not passed, return error */
	if (((g_fips140_status == FIPS140_STATUS_FAIL) ||
		(g_fips140_status == FIPS140_STATUS_PASS_CRYPTO)) &&
		is_fips_qcrypto_tests_done)
		return -ENXIO;

	crypto_ahash_set_reqsize(ahash, sizeof(struct qcrypto_sha_req_ctx));
	/* update context with ptr to cp */
	sha_ctx->cp = q_alg->cp;
	sha_ctx->flags = 0;
	sha_ctx->ahash_req = NULL;
	if (_qcrypto_init_assign) {
		sha_ctx->pengine = _qcrypto_static_assign_engine(sha_ctx->cp);
		if (sha_ctx->pengine == NULL)
			return -ENODEV;
	} else
		sha_ctx->pengine = NULL;
	INIT_LIST_HEAD(&sha_ctx->rsp_queue);
	return 0;
};

static void _qcrypto_ahash_cra_exit(struct crypto_tfm *tfm)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(tfm);

	if (!list_empty(&sha_ctx->rsp_queue))
		pr_err("_qcrypto_ahash_cra_exit: requests still outstanding");
	if (sha_ctx->ahash_req != NULL) {
		ahash_request_free(sha_ctx->ahash_req);
		sha_ctx->ahash_req = NULL;
	}
};


static void _crypto_sha_hmac_ahash_req_complete(
	struct crypto_async_request *req, int err);

static int _qcrypto_ahash_hmac_cra_init(struct crypto_tfm *tfm)
{
	struct crypto_ahash *ahash = __crypto_ahash_cast(tfm);
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(tfm);
	int ret = 0;

	ret = _qcrypto_ahash_cra_init(tfm);
	if (ret)
		return ret;
	sha_ctx->ahash_req = ahash_request_alloc(ahash, GFP_KERNEL);

	if (sha_ctx->ahash_req == NULL) {
		_qcrypto_ahash_cra_exit(tfm);
		return -ENOMEM;
	}

	init_completion(&sha_ctx->ahash_req_complete);
	ahash_request_set_callback(sha_ctx->ahash_req,
				CRYPTO_TFM_REQ_MAY_BACKLOG,
				_crypto_sha_hmac_ahash_req_complete,
				&sha_ctx->ahash_req_complete);
	crypto_ahash_clear_flags(ahash, ~0);

	return 0;
};

static int _qcrypto_cra_ablkcipher_init(struct crypto_tfm *tfm)
{
	tfm->crt_ablkcipher.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	return _qcrypto_cipher_cra_init(tfm);
};

static int _qcrypto_cra_aes_ablkcipher_init(struct crypto_tfm *tfm)
{
	const char *name = tfm->__crt_alg->cra_name;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	int ret;
	struct crypto_priv *cp = &qcrypto_dev;

	if (cp->ce_support.use_sw_aes_cbc_ecb_ctr_algo) {
		ctx->cipher_aes192_fb = NULL;
		return _qcrypto_cra_ablkcipher_init(tfm);
	}
	ctx->cipher_aes192_fb = crypto_alloc_ablkcipher(name, 0,
			CRYPTO_ALG_ASYNC | CRYPTO_ALG_NEED_FALLBACK);
	if (IS_ERR(ctx->cipher_aes192_fb)) {
		pr_err("Error allocating fallback algo %s\n", name);
		ret = PTR_ERR(ctx->cipher_aes192_fb);
		ctx->cipher_aes192_fb = NULL;
		return ret;
	}
	return _qcrypto_cra_ablkcipher_init(tfm);
};

static int _qcrypto_cra_aead_sha1_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	ctx->auth_alg = QCE_HASH_SHA1_HMAC;
	ctx->aead_esn = false;
	return rc;
}

static int _qcrypto_cra_aead_sha256_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	ctx->auth_alg = QCE_HASH_SHA256_HMAC;
	ctx->aead_esn = false;
	return rc;
}

static int _qcrypto_cra_aead_sha1_esn_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	ctx->auth_alg = QCE_HASH_SHA1_HMAC;
	ctx->aead_esn = true;
	return rc;
}

static int _qcrypto_cra_aead_sha256_esn_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	ctx->auth_alg = QCE_HASH_SHA256_HMAC;
	ctx->aead_esn = true;
	return rc;
}

static int _qcrypto_aes192fb_sha1_init(struct crypto_tfm *tfm)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_priv *cp = &qcrypto_dev;

	ctx->cipher_aes192_fb = NULL;
	ctx->ahash_aead_aes192_fb = NULL;
	if (!cp->ce_support.aes_key_192) {
		ctx->cipher_aes192_fb = crypto_alloc_ablkcipher(
							"cbc(aes)", 0, 0);
		if (IS_ERR(ctx->cipher_aes192_fb)) {
			ctx->cipher_aes192_fb = NULL;
		} else {
			ctx->ahash_aead_aes192_fb = crypto_alloc_ahash(
							"hmac(sha1)", 0, 0);
			if (IS_ERR(ctx->ahash_aead_aes192_fb)) {
				ctx->ahash_aead_aes192_fb = NULL;
				crypto_free_ablkcipher(ctx->cipher_aes192_fb);
				ctx->cipher_aes192_fb = NULL;
			}
		}
	}
	ctx->auth_alg = QCE_HASH_SHA1_HMAC;
	return 0;
}

static int _qcrypto_cra_aead_aes_sha1_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	if (rc)
		return rc;
	ctx->aead_esn = false;
	return _qcrypto_aes192fb_sha1_init(tfm);
}

static int _qcrypto_cra_aead_aes_sha1_esn_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	if (rc)
		return rc;
	ctx->aead_esn = true;
	return _qcrypto_aes192fb_sha1_init(tfm);
}

static int _qcrypto_aes192fb_sha256_init(struct crypto_tfm *tfm)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_priv *cp = &qcrypto_dev;

	ctx->cipher_aes192_fb = NULL;
	ctx->ahash_aead_aes192_fb = NULL;
	if (!cp->ce_support.aes_key_192) {
		ctx->cipher_aes192_fb = crypto_alloc_ablkcipher(
							"cbc(aes)", 0, 0);
		if (IS_ERR(ctx->cipher_aes192_fb)) {
			ctx->cipher_aes192_fb = NULL;
		} else {
			ctx->ahash_aead_aes192_fb = crypto_alloc_ahash(
							"hmac(sha256)", 0, 0);
			if (IS_ERR(ctx->ahash_aead_aes192_fb)) {
				ctx->ahash_aead_aes192_fb = NULL;
				crypto_free_ablkcipher(ctx->cipher_aes192_fb);
				ctx->cipher_aes192_fb = NULL;
			}
		}
	}
	ctx->auth_alg = QCE_HASH_SHA256_HMAC;
	return 0;
}

static int _qcrypto_cra_aead_aes_sha256_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	if (rc)
		return rc;
	ctx->aead_esn = false;
	return _qcrypto_aes192fb_sha256_init(tfm);
}

static int _qcrypto_cra_aead_aes_sha256_esn_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	if (rc)
		return rc;
	ctx->aead_esn = true;
	return _qcrypto_aes192fb_sha256_init(tfm);
}


static int _qcrypto_cra_aead_ccm_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	ctx->auth_alg =  QCE_HASH_AES_CMAC;
	return rc;
}

static int _qcrypto_cra_aead_rfc4309_ccm_init(struct crypto_tfm *tfm)
{
	int rc;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	tfm->crt_aead.reqsize = sizeof(struct qcrypto_cipher_req_ctx);
	rc = _qcrypto_cipher_cra_init(tfm);
	ctx->auth_alg =  QCE_HASH_AES_CMAC;
	return rc;
}

static void _qcrypto_cra_ablkcipher_exit(struct crypto_tfm *tfm)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	if (!list_empty(&ctx->rsp_queue))
		pr_err("_qcrypto__cra_ablkcipher_exit: requests still outstanding");
};

static void _qcrypto_cra_aes_ablkcipher_exit(struct crypto_tfm *tfm)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	_qcrypto_cra_ablkcipher_exit(tfm);
	if (ctx->cipher_aes192_fb)
		crypto_free_ablkcipher(ctx->cipher_aes192_fb);
	ctx->cipher_aes192_fb = NULL;
}

static void _qcrypto_cra_aead_exit(struct crypto_tfm *tfm)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	if (!list_empty(&ctx->rsp_queue))
		pr_err("_qcrypto__cra_aead_exit: requests still outstanding");
}

static void _qcrypto_cra_aead_aes_exit(struct crypto_tfm *tfm)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	if (!list_empty(&ctx->rsp_queue))
		pr_err("_qcrypto__cra_aead_exit: requests still outstanding");
	if (ctx->cipher_aes192_fb)
		crypto_free_ablkcipher(ctx->cipher_aes192_fb);
	if (ctx->ahash_aead_aes192_fb)
		crypto_free_ahash(ctx->ahash_aead_aes192_fb);
	ctx->cipher_aes192_fb = NULL;
	ctx->ahash_aead_aes192_fb = NULL;
}

static int _disp_stats(int id)
{
	struct crypto_stat *pstat;
	int len = 0;
	unsigned long flags;
	struct crypto_priv *cp = &qcrypto_dev;
	struct crypto_engine *pe;
	struct crypto_poll_ctl *ctl;

	pstat = &_qcrypto_stat;
	len = scnprintf(_debug_read_buf, DEBUG_MAX_RW_BUF - 1,
			"\nQualcomm crypto accelerator %d Statistics\n",
				id + 1);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER AES encryption          : %llu\n",
					pstat->ablk_cipher_aes_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER AES decryption          : %llu\n",
					pstat->ablk_cipher_aes_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER DES encryption          : %llu\n",
					pstat->ablk_cipher_des_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER DES decryption          : %llu\n",
					pstat->ablk_cipher_des_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER 3DES encryption         : %llu\n",
					pstat->ablk_cipher_3des_enc);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER 3DES decryption         : %llu\n",
					pstat->ablk_cipher_3des_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER operation success       : %llu\n",
					pstat->ablk_cipher_op_success);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   ABLK CIPHER operation fail          : %llu\n",
					pstat->ablk_cipher_op_fail);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"\n");

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA1-AES encryption            : %llu\n",
					pstat->aead_sha1_aes_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA1-AES decryption            : %llu\n",
					pstat->aead_sha1_aes_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA1-DES encryption            : %llu\n",
					pstat->aead_sha1_des_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA1-DES decryption            : %llu\n",
					pstat->aead_sha1_des_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA1-3DES encryption           : %llu\n",
					pstat->aead_sha1_3des_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA1-3DES decryption           : %llu\n",
					pstat->aead_sha1_3des_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA256-AES encryption          : %llu\n",
					pstat->aead_sha256_aes_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA256-AES decryption          : %llu\n",
					pstat->aead_sha256_aes_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA256-DES encryption          : %llu\n",
					pstat->aead_sha256_des_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA256-DES decryption          : %llu\n",
					pstat->aead_sha256_des_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA256-3DES encryption         : %llu\n",
					pstat->aead_sha256_3des_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD SHA256-3DES decryption         : %llu\n",
					pstat->aead_sha256_3des_dec);

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD CCM-AES encryption             : %llu\n",
					pstat->aead_ccm_aes_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD CCM-AES decryption             : %llu\n",
					pstat->aead_ccm_aes_dec);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD RFC4309-CCM-AES encryption     : %llu\n",
					pstat->aead_rfc4309_ccm_aes_enc);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD RFC4309-CCM-AES decryption     : %llu\n",
					pstat->aead_rfc4309_ccm_aes_dec);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD operation success              : %llu\n",
					pstat->aead_op_success);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD operation fail                 : %llu\n",
					pstat->aead_op_fail);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AEAD bad message                    : %llu\n",
					pstat->aead_bad_msg);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"\n");

	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AHASH SHA1 digest                   : %llu\n",
					pstat->sha1_digest);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AHASH SHA256 digest                 : %llu\n",
					pstat->sha256_digest);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AHASH SHA1 HMAC digest              : %llu\n",
					pstat->sha1_hmac_digest);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AHASH SHA256 HMAC digest            : %llu\n",
					pstat->sha256_hmac_digest);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AHASH operation success             : %llu\n",
					pstat->ahash_op_success);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   AHASH operation fail                : %llu\n",
					pstat->ahash_op_fail);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   resp start                          : %u\n",
					cp->resp_start);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   resp stop                           : %u\n",
					cp->resp_stop);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   max rsp queue                       : %u\n",
					cp->max_resp_qlen);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   max reorder-cnt                     : %u\n",
					cp->max_reorder_cnt);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   max queue length                    : %u\n",
					cp->max_qlen);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   not avail                           : %u\n",
					cp->no_avail);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   complete work queue scheduling      : %llu\n",
					cp->queue_complete_work);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"   dropped request                     : %llu\n",
					cp->req_drop_cnt);
	len += scnprintf(_debug_read_buf + len, DEBUG_MAX_RW_BUF - len - 1,
			"\n");
	spin_lock_irqsave(&cp->lock, flags);
	list_for_each_entry(pe, &cp->engine_list, elist) {
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"   Engine %1d: Total Req             : %llu\n",
			pe->unit,
			pe->total_req
		);
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"             Req Error             : %llu\n",
			pe->err_req
		);
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"             Max Active            : %d\n",
			pe->max_req_used
		);
		len += qce_get_driver_stats(pe->qce, _debug_read_buf + len,
					DEBUG_MAX_RW_BUF - len - 1);
	}
	spin_unlock_irqrestore(&cp->lock, flags);

	list_for_each_entry(ctl, &cp->poll_ctl_list, list) {
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"   CPU%1d PollCtl:\n"
			"             Poll                 : %llu\n",
			ctl->cpu,
			ctl->poll);
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"             Schedule             : %llu\n",
			ctl->sched);
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"             State                : %u\n",
			ctl->state);
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"             Engine               :");
		list_for_each_entry(pe, &ctl->engine_list, plist) {
			len += scnprintf(
				_debug_read_buf + len,
				DEBUG_MAX_RW_BUF - len - 1,
				" %u",
				pe->unit);
		}
		len += scnprintf(
			_debug_read_buf + len,
			DEBUG_MAX_RW_BUF - len - 1,
			"\n");
	}
	return len;
}

static void _qcrypto_remove_engine(struct crypto_engine *pengine)
{
	struct crypto_priv *cp;
	struct qcrypto_alg *q_alg;
	struct qcrypto_alg *n;
	unsigned long flags;

	cp = pengine->pcp;

	spin_lock_irqsave(&cp->lock, flags);
	list_del(&pengine->elist);
	if (cp->next_engine == pengine)
		cp->next_engine = NULL;
	spin_unlock_irqrestore(&cp->lock, flags);

	cp->total_units--;

	cancel_work_sync(&pengine->bw_reaper_ws);
	cancel_work_sync(&pengine->bw_allocate_ws);
	del_timer_sync(&pengine->bw_reaper_timer);

	if (pengine->bus_scale_handle != 0)
		msm_bus_scale_unregister_client(pengine->bus_scale_handle);
	pengine->bus_scale_handle = 0;

	kzfree(pengine->preq_pool);

	if (cp->total_units)
		return;

	list_for_each_entry_safe(q_alg, n, &cp->alg_list, entry) {
		if (q_alg->alg_type == QCRYPTO_ALG_CIPHER)
			crypto_unregister_alg(&q_alg->cipher_alg);
		if (q_alg->alg_type == QCRYPTO_ALG_SHA)
			crypto_unregister_ahash(&q_alg->sha_alg);
		list_del(&q_alg->entry);
		kzfree(q_alg);
	}
	qcrypto_poll_ctl_engine_detach(pengine);
}

static int _qcrypto_remove(struct platform_device *pdev)
{
	struct crypto_engine *pengine;
	struct crypto_priv *cp;

	pengine = platform_get_drvdata(pdev);

	if (!pengine)
		return 0;
	cp = pengine->pcp;
	mutex_lock(&cp->engine_lock);
	_qcrypto_remove_engine(pengine);
	mutex_unlock(&cp->engine_lock);
	if (pengine->qce)
		qce_close(pengine->qce);
	kzfree(pengine);
	if (list_empty(&cp->engine_list))
		qcrypto_poll_ctl_free_all(cp);
	return 0;
}

static int _qcrypto_check_aes_keylen(struct crypto_ablkcipher *cipher,
		struct crypto_priv *cp, unsigned int len)
{

	switch (len) {
	case AES_KEYSIZE_128:
	case AES_KEYSIZE_256:
		break;
	case AES_KEYSIZE_192:
		if (cp->ce_support.aes_key_192)
			break;
	default:
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	};

	return 0;
}

static int _qcrypto_setkey_aes_192_fallback(struct crypto_ablkcipher *cipher,
		const u8 *key)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	int ret;

	ctx->enc_key_len = AES_KEYSIZE_192;
	ctx->cipher_aes192_fb->base.crt_flags &= ~CRYPTO_TFM_REQ_MASK;
	ctx->cipher_aes192_fb->base.crt_flags |=
			(cipher->base.crt_flags & CRYPTO_TFM_REQ_MASK);
	ret = crypto_ablkcipher_setkey(ctx->cipher_aes192_fb, key,
			AES_KEYSIZE_192);
	if (ret) {
		tfm->crt_flags &= ~CRYPTO_TFM_RES_MASK;
		tfm->crt_flags |=
			(cipher->base.crt_flags & CRYPTO_TFM_RES_MASK);
	}
	return ret;
}

static int _qcrypto_setkey_aes(struct crypto_ablkcipher *cipher, const u8 *key,
		unsigned int len)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_priv *cp = ctx->cp;

	if ((ctx->flags & QCRYPTO_CTX_USE_HW_KEY) == QCRYPTO_CTX_USE_HW_KEY)
		return 0;

	if ((len == AES_KEYSIZE_192) && (!cp->ce_support.aes_key_192)
					&& ctx->cipher_aes192_fb)
		return _qcrypto_setkey_aes_192_fallback(cipher, key);

	if (_qcrypto_check_aes_keylen(cipher, cp, len)) {
		return -EINVAL;
	} else {
		ctx->enc_key_len = len;
		if (!(ctx->flags & QCRYPTO_CTX_USE_PIPE_KEY))  {
			if (key != NULL) {
				memcpy(ctx->enc_key, key, len);
			} else {
				pr_err("%s Inavlid key pointer\n", __func__);
				return -EINVAL;
			}
		}
	}
	return 0;
};

static int _qcrypto_setkey_aes_xts(struct crypto_ablkcipher *cipher,
		const u8 *key, unsigned int len)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_priv *cp = ctx->cp;

	if ((ctx->flags & QCRYPTO_CTX_USE_HW_KEY) == QCRYPTO_CTX_USE_HW_KEY)
		return 0;
	if (_qcrypto_check_aes_keylen(cipher, cp, len/2)) {
		return -EINVAL;
	} else {
		ctx->enc_key_len = len;
		if (!(ctx->flags & QCRYPTO_CTX_USE_PIPE_KEY))  {
			if (key != NULL) {
				memcpy(ctx->enc_key, key, len);
			} else {
				pr_err("%s Inavlid key pointer\n", __func__);
				return -EINVAL;
			}
		}
	}
	return 0;
};

static int _qcrypto_setkey_des(struct crypto_ablkcipher *cipher, const u8 *key,
		unsigned int len)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	u32 tmp[DES_EXPKEY_WORDS];
	int ret;

	if (!key) {
		pr_err("%s Inavlid key pointer\n", __func__);
		return -EINVAL;
	}

	ret = des_ekey(tmp, key);

	if ((ctx->flags & QCRYPTO_CTX_USE_HW_KEY) == QCRYPTO_CTX_USE_HW_KEY) {
		pr_err("%s HW KEY usage not supported for DES algorithm\n",
								__func__);
		return 0;
	};

	if (len != DES_KEY_SIZE) {
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	};

	if (unlikely(ret == 0) && (tfm->crt_flags & CRYPTO_TFM_REQ_WEAK_KEY)) {
		tfm->crt_flags |= CRYPTO_TFM_RES_WEAK_KEY;
		return -EINVAL;
	}

	ctx->enc_key_len = len;
	if (!(ctx->flags & QCRYPTO_CTX_USE_PIPE_KEY))
		memcpy(ctx->enc_key, key, len);

	return 0;
};

static int _qcrypto_setkey_3des(struct crypto_ablkcipher *cipher, const u8 *key,
		unsigned int len)
{
	struct crypto_tfm *tfm = crypto_ablkcipher_tfm(cipher);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);

	if ((ctx->flags & QCRYPTO_CTX_USE_HW_KEY) == QCRYPTO_CTX_USE_HW_KEY) {
		pr_err("%s HW KEY usage not supported for 3DES algorithm\n",
								__func__);
		return 0;
	};
	if (len != DES3_EDE_KEY_SIZE) {
		crypto_ablkcipher_set_flags(cipher, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	};
	ctx->enc_key_len = len;
	if (!(ctx->flags & QCRYPTO_CTX_USE_PIPE_KEY)) {
		if (key != NULL) {
			memcpy(ctx->enc_key, key, len);
		} else {
			pr_err("%s Inavlid key pointer\n", __func__);
			return -EINVAL;
		}
	}
	return 0;
};

static void seq_response(unsigned long data)
{
	struct crypto_priv *cp = (struct crypto_priv *)data;
	struct llist_node *list;
	struct llist_node *rev = NULL;

again:
	list = llist_del_all(&cp->ordered_resp_list);

	if (!list)
		goto end;

	while (list) {
		struct llist_node *t = list;
		list = llist_next(list);

		t->next = rev;
		rev = t;
	}

	while (rev) {
		struct qcrypto_resp_ctx *arsp;
		struct crypto_async_request *areq;

		arsp = container_of(rev, struct qcrypto_resp_ctx, llist);
		rev = llist_next(rev);

		areq = arsp->async_req;
		areq->complete(areq, arsp->res);
		atomic_dec(&cp->resp_cnt);
	}

	if (atomic_read(&cp->resp_cnt) < COMPLETION_CB_BACKLOG_LENGTH_START &&
		(cmpxchg(&cp->ce_req_proc_sts, STOPPED, IN_PROGRESS)
						== STOPPED))
		cp->resp_start++;

end:
	if (cmpxchg(&cp->sched_resp_workq_status, SCHEDULE_AGAIN,
				IS_SCHEDULED) == SCHEDULE_AGAIN)
		goto again;
	else if (cmpxchg(&cp->sched_resp_workq_status, IS_SCHEDULED,
				NOT_SCHEDULED) == SCHEDULE_AGAIN)
		goto end;
}

static void _qcrypto_tfm_complete(struct crypto_engine *pengine, u32 type,
					void *tfm_ctx,
					struct qcrypto_resp_ctx *cur_arsp,
					int res)
{
	struct crypto_priv *cp = pengine->pcp;
	unsigned long flags = 0;
	struct qcrypto_resp_ctx *arsp;
	struct list_head *plist;
	unsigned int resp_qlen;
	unsigned int cnt = 0;
	bool need_lock = (pengine->pcp->poll_ctl_num > 1) ? true : false;

	switch (type) {
	case CRYPTO_ALG_TYPE_AHASH:
		plist = &((struct qcrypto_sha_ctx *) tfm_ctx)->rsp_queue;
		break;
	case CRYPTO_ALG_TYPE_ABLKCIPHER:
	case CRYPTO_ALG_TYPE_AEAD:
	default:
		plist = &((struct qcrypto_cipher_ctx *) tfm_ctx)->rsp_queue;
		break;
	}

	if (need_lock)
		spin_lock_irqsave(&cp->lock, flags);

	cur_arsp->res = res;
	while (!list_empty(plist)) {
		arsp = list_first_entry(plist,
				struct qcrypto_resp_ctx, list);
		if (arsp->res == -EINPROGRESS)
			break;
		else {
			list_del(&arsp->list);
			llist_add(&arsp->llist, &cp->ordered_resp_list);
			atomic_inc(&cp->resp_cnt);
			cnt++;
		}
	}
	resp_qlen = atomic_read(&cp->resp_cnt);
	if (resp_qlen > cp->max_resp_qlen)
		cp->max_resp_qlen = resp_qlen;
	if (cnt > cp->max_reorder_cnt)
		cp->max_reorder_cnt = cnt;
	if ((resp_qlen >= COMPLETION_CB_BACKLOG_LENGTH_STOP) &&
		cmpxchg(&cp->ce_req_proc_sts, IN_PROGRESS,
						STOPPED) == IN_PROGRESS) {
		cp->resp_stop++;
	}

	if (need_lock)
		spin_unlock_irqrestore(&cp->lock, flags);
}

static void req_done(struct qcrypto_req_control *pqcrypto_req_control)
{
	struct crypto_engine *pengine;
	struct crypto_async_request *areq;
	struct crypto_priv *cp;
	struct qcrypto_resp_ctx *arsp;
	u32 type = 0;
	void *tfm_ctx = NULL;
	int res;

	pengine = pqcrypto_req_control->pce;
	cp = pengine->pcp;
	areq = pqcrypto_req_control->req;
	arsp = pqcrypto_req_control->arsp;
	res = pqcrypto_req_control->res;
	qcrypto_free_req_control(pengine, pqcrypto_req_control);

	if (areq) {
		type = crypto_tfm_alg_type(areq->tfm);
		tfm_ctx = crypto_tfm_ctx(areq->tfm);
	}
	if (areq)
		_qcrypto_tfm_complete(pengine, type, tfm_ctx, arsp, res);
}

static void _qce_ahash_complete(void *cookie, unsigned char *digest,
		unsigned char *authdata, int ret)
{
	struct ahash_request *areq = (struct ahash_request *) cookie;
	struct crypto_async_request *async_req;
	struct crypto_ahash *ahash = crypto_ahash_reqtfm(areq);
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(areq->base.tfm);
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(areq);
	struct crypto_priv *cp = sha_ctx->cp;
	struct crypto_stat *pstat;
	uint32_t diglen = crypto_ahash_digestsize(ahash);
	uint32_t *auth32 = (uint32_t *)authdata;
	struct crypto_engine *pengine;
	struct qcrypto_req_control *pqcrypto_req_control;

	async_req = &areq->base;
	pstat = &_qcrypto_stat;

	pengine = rctx->pengine;
	pqcrypto_req_control = find_req_control_for_areq(pengine,
							 async_req);
	if (pqcrypto_req_control == NULL) {
		pr_err("async request not found\n");
		return;
	}

#ifdef QCRYPTO_DEBUG
	dev_info(&pengine->pdev->dev, "_qce_ahash_complete: %p ret %d\n",
				areq, ret);
#endif
	if (digest) {
		memcpy(rctx->digest, digest, diglen);
		if (rctx->last_blk)
			memcpy(areq->result, digest, diglen);
	}
	if (authdata) {
		rctx->byte_count[0] = auth32[0];
		rctx->byte_count[1] = auth32[1];
		rctx->byte_count[2] = auth32[2];
		rctx->byte_count[3] = auth32[3];
	}
	areq->src = rctx->src;
	areq->nbytes = rctx->nbytes;

	rctx->last_blk = 0;
	rctx->first_blk = 0;

	if (ret) {
		pqcrypto_req_control->res = -ENXIO;
		pstat->ahash_op_fail++;
	} else {
		pqcrypto_req_control->res = 0;
		pstat->ahash_op_success++;
	}
	if (cp->ce_support.aligned_only)  {
		areq->src = rctx->orig_src;
		qcrypto_kfree(&rctx->qbuf, rctx->data);
	}
	req_done(pqcrypto_req_control);
};

static void _qce_ablk_cipher_complete(void *cookie, unsigned char *icb,
		unsigned char *iv, int ret)
{
	struct ablkcipher_request *areq = (struct ablkcipher_request *) cookie;
	struct crypto_async_request *async_req;
	struct crypto_ablkcipher *ablk = crypto_ablkcipher_reqtfm(areq);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(areq->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;
	struct qcrypto_cipher_req_ctx *rctx;
	struct crypto_engine *pengine;
	struct qcrypto_req_control *pqcrypto_req_control;

	async_req = &areq->base;
	pstat = &_qcrypto_stat;
	rctx = ablkcipher_request_ctx(areq);
	pengine = rctx->pengine;
	pqcrypto_req_control = find_req_control_for_areq(pengine,
							 async_req);
	if (pqcrypto_req_control == NULL) {
		pr_err("async request not found\n");
		return;
	}

#ifdef QCRYPTO_DEBUG
	dev_info(&pengine->pdev->dev, "_qce_ablk_cipher_complete: %p ret %d\n",
				areq, ret);
#endif
	if (iv)
		memcpy(ctx->iv, iv, crypto_ablkcipher_ivsize(ablk));

	if (ret) {
		pqcrypto_req_control->res = -ENXIO;
		pstat->ablk_cipher_op_fail++;
	} else {
		pqcrypto_req_control->res = 0;
		pstat->ablk_cipher_op_success++;
	}

	if (cp->ce_support.aligned_only)  {
		struct qcrypto_cipher_req_ctx *rctx;
		uint32_t num_sg = 0;
		uint32_t bytes = 0;

		rctx = ablkcipher_request_ctx(areq);
		areq->src = rctx->orig_src;
		areq->dst = rctx->orig_dst;

		num_sg = qcrypto_count_sg(areq->dst, areq->nbytes);
		bytes = qcrypto_sg_copy_from_buffer(areq->dst, num_sg,
			rctx->data, areq->nbytes);
		if (bytes != areq->nbytes)
			pr_warn("bytes copied=0x%x bytes to copy= 0x%x", bytes,
								areq->nbytes);
		qcrypto_kfree(&rctx->qbuf, rctx->data);
	}
	req_done(pqcrypto_req_control);
};


static void _qce_aead_complete(void *cookie, unsigned char *icv,
				unsigned char *iv, int ret)
{
	struct aead_request *areq = (struct aead_request *) cookie;
	struct crypto_async_request *async_req;
	struct crypto_aead *aead = crypto_aead_reqtfm(areq);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(areq->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct qcrypto_cipher_req_ctx *rctx;
	struct crypto_stat *pstat;
	struct crypto_engine *pengine;
	struct qcrypto_req_control *pqcrypto_req_control;

	async_req = &areq->base;
	pstat = &_qcrypto_stat;
	rctx = aead_request_ctx(areq);
	pengine = rctx->pengine;
	pqcrypto_req_control = find_req_control_for_areq(pengine,
							 async_req);
	if (pqcrypto_req_control == NULL) {
		pr_err("async request not found\n");
		return;
	}

	qcrypto_kfree(&rctx->qbuf, rctx->assoc);
	if (rctx->mode == QCE_MODE_CCM) {
		if (cp->ce_support.aligned_only)  {
			struct qcrypto_cipher_req_ctx *rctx;
			uint32_t bytes = 0;
			uint32_t nbytes = 0;
			uint32_t num_sg = 0;

			rctx = aead_request_ctx(areq);
			areq->src = rctx->orig_src;
			areq->dst = rctx->orig_dst;
			if (rctx->dir == QCE_ENCRYPT)
				nbytes = areq->cryptlen +
						crypto_aead_authsize(aead);
			else
				nbytes = areq->cryptlen -
						crypto_aead_authsize(aead);
			num_sg = qcrypto_count_sg(areq->dst, nbytes);
			bytes = qcrypto_sg_copy_from_buffer(areq->dst, num_sg,
					((char *)rctx->data + areq->assoclen),
					nbytes);
			if (bytes != nbytes)
				pr_warn("bytes copied=0x%x bytes to copy= 0x%x",
						bytes, nbytes);
			qcrypto_kfree(&rctx->qbuf, rctx->data);
		}
	} else {
		uint32_t ivsize = crypto_aead_ivsize(aead);

		/* for aead operations, other than aes(ccm) */
		if (cp->ce_support.aligned_only) {
			struct qcrypto_cipher_req_ctx *rctx;
			uint32_t bytes = 0;
			uint32_t nbytes = 0;
			uint32_t num_sg = 0;
			uint32_t offset;

			rctx = aead_request_ctx(areq);
			offset = rctx->assoc_1_len + ivsize;
			areq->src = rctx->orig_src;
			areq->dst = rctx->orig_dst;

			if (rctx->dir == QCE_ENCRYPT)
				nbytes = areq->cryptlen;
			else
				nbytes = areq->cryptlen -
						crypto_aead_authsize(aead);
			num_sg = qcrypto_count_sg(areq->dst, nbytes);
			bytes = qcrypto_sg_copy_from_buffer(
					areq->dst,
					num_sg,
					(char *)rctx->data + offset,
					nbytes);
			if (bytes != nbytes)
				pr_warn("bytes copied=0x%x bytes to copy= 0x%x",
						bytes, nbytes);
			qcrypto_kfree(&rctx->qbuf, rctx->data);
		}

		if (ret == 0) {
			if (rctx->dir  == QCE_ENCRYPT) {
				/* copy the icv to dst */
				scatterwalk_map_and_copy(icv, areq->dst,
						areq->cryptlen,
						ctx->authsize, 1);

			} else {
				unsigned char tmp[SHA256_DIGESTSIZE] = {0};

				/* compare icv from src */
				scatterwalk_map_and_copy(tmp,
					areq->src, areq->cryptlen -
					ctx->authsize, ctx->authsize, 0);
				ret = memcmp(icv, tmp, ctx->authsize);
				if (ret != 0)
					ret = -EBADMSG;
			}
		} else {
			ret = -ENXIO;
		}

		if (iv)
			memcpy(ctx->iv, iv, ivsize);
	}

	if (ret == (-EBADMSG))
		pstat->aead_bad_msg++;
	else if (ret)
		pstat->aead_op_fail++;
	else
		pstat->aead_op_success++;

	pqcrypto_req_control->res = ret;
	req_done(pqcrypto_req_control);
}

static int aead_ccm_set_msg_len(u8 *block, unsigned int msglen, int csize)
{
	__be32 data;

	memset(block, 0, csize);
	block += csize;

	if (csize >= 4)
		csize = 4;
	else if (msglen > (1 << (8 * csize)))
		return -EOVERFLOW;

	data = cpu_to_be32(msglen);
	memcpy(block - csize, (u8 *)&data + 4 - csize, csize);

	return 0;
}

static int qccrypto_set_aead_ccm_nonce(struct qce_req *qreq)
{
	struct aead_request *areq = (struct aead_request *) qreq->areq;
	unsigned int i = ((unsigned int)qreq->iv[0]) + 1;

	memcpy(&qreq->nonce[0] , qreq->iv, qreq->ivsize);
	/*
	 * Format control info per RFC 3610 and
	 * NIST Special Publication 800-38C
	 */
	qreq->nonce[0] |= (8 * ((qreq->authsize - 2) / 2));
	if (areq->assoclen)
		qreq->nonce[0] |= 64;

	if (i > MAX_NONCE)
		return -EINVAL;

	return aead_ccm_set_msg_len(qreq->nonce + 16 - i, qreq->cryptlen, i);
}


static int qcrypto_aead_format_adata(
	struct qce_req *qreq,
	struct qcrypto_buf *qbuf,
	size_t alen,
	struct scatterlist *asg,
	bool esn)
{
	unsigned char *adata, *adata1;
	struct scatterlist *sgl;
	size_t len;
	size_t buflen = alen;

	if (alen == 0) {
		qreq->assoc = NULL;
		qreq->assoclen = 0;
		return 0;
	} else if (esn && alen < (ESP_EXT_SEQ_HI_OFFSET + ESP_EXT_SEQ_HI_SIZE +
				 ESP_EXT_SEQ_LOW_SIZE))
		return -EINVAL;
	adata = qcrypto_kzalloc(qbuf, alen, GFP_ATOMIC);
	if (adata == NULL)
		return -ENOMEM;
	qreq->assoc = adata;
	qreq->assoclen = alen;
	qreq->trail_assoclen = 0;
	for (sgl = asg; buflen > 0 && sgl;) {
		len = sg_copy_to_buffer(sgl, 1, adata, buflen);
		adata += len;
		buflen -= len;
		sgl = scatterwalk_sg_next(sgl);
	}
	if (buflen) {
		pr_err("ill formatted association scatter gather list\n");
		qcrypto_kfree(qbuf, qreq->assoc);
		return -EINVAL;
	}
	if (esn) {
		adata = qreq->assoc;
		adata1 = qcrypto_kzalloc(qbuf, alen, GFP_ATOMIC);
		if (adata1 == NULL) {
			qcrypto_kfree(qbuf, adata);
			return -ENOMEM;
		}
		memcpy(adata1, adata, ESP_EXT_SEQ_HI_OFFSET);
		memcpy(adata1 + ESP_EXT_SEQ_HI_OFFSET,
			adata + ESP_EXT_SEQ_HI_OFFSET + ESP_EXT_SEQ_HI_SIZE,
			alen - ESP_EXT_SEQ_HI_OFFSET - ESP_EXT_SEQ_HI_SIZE);
		memcpy(adata1 + ESP_EXT_SEQ_HI_OFFSET +
			alen - ESP_EXT_SEQ_HI_OFFSET - ESP_EXT_SEQ_HI_SIZE,
			adata + ESP_EXT_SEQ_HI_OFFSET, ESP_EXT_SEQ_HI_SIZE);
		qcrypto_kfree(qbuf, adata);
		qreq->assoc = adata1;
		qreq->trail_assoclen = ESP_EXT_SEQ_HI_SIZE;
	}
	return 0;
}

static int qcrypto_aead_ccm_format_adata(
	struct qce_req *qreq,
	struct qcrypto_buf *qbuf,
	uint32_t alen,
	struct scatterlist *sg)
{
	unsigned char *adata;
	uint32_t len;
	uint32_t bytes = 0;
	uint32_t num_sg = 0;

	if (alen == 0) {
		qreq->assoc = NULL;
		qreq->assoclen = 0;
		return 0;
	}

	qreq->assoc = qcrypto_kzalloc(qbuf, (alen + 0x64), GFP_ATOMIC);
	if (!qreq->assoc) {
		pr_err("qcrypto Memory allocation of adata FAIL, error %ld\n",
				PTR_ERR(qreq->assoc));
		return -ENOMEM;
	}
	adata = qreq->assoc;
	/*
	 * Add control info for associated data
	 * RFC 3610 and NIST Special Publication 800-38C
	 */
	if (alen < 65280) {
		*(__be16 *)adata = cpu_to_be16(alen);
		len = 2;
	} else {
			if ((alen >= 65280) && (alen <= 0xffffffff)) {
				*(__be16 *)adata = cpu_to_be16(0xfffe);
				*(__be32 *)&adata[2] = cpu_to_be32(alen);
				len = 6;
		} else {
				*(__be16 *)adata = cpu_to_be16(0xffff);
				*(__be32 *)&adata[6] = cpu_to_be32(alen);
				len = 10;
		}
	}
	adata += len;
	qreq->assoclen = ALIGN((alen + len), 16);

	num_sg = qcrypto_count_sg(sg, alen);
	bytes = qcrypto_sg_copy_to_buffer(sg, num_sg, adata, alen);
	if (bytes != alen)
		pr_warn("bytes copied=0x%x bytes to copy= 0x%x", bytes, alen);

	return 0;
}

static int _qcrypto_process_ablkcipher(struct crypto_engine *pengine,
			struct qcrypto_req_control *pqcrypto_req_control)
{
	struct crypto_async_request *async_req;
	struct qce_req qreq;
	int ret;
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *cipher_ctx;
	struct ablkcipher_request *req;
	struct crypto_ablkcipher *tfm;

	async_req = pqcrypto_req_control->req;
	req = container_of(async_req, struct ablkcipher_request, base);
	cipher_ctx = crypto_tfm_ctx(async_req->tfm);
	rctx = ablkcipher_request_ctx(req);
	rctx->pengine = pengine;
	tfm = crypto_ablkcipher_reqtfm(req);
	if (pengine->pcp->ce_support.aligned_only) {
		uint32_t bytes = 0;
		uint32_t num_sg = 0;

		rctx->orig_src = req->src;
		rctx->orig_dst = req->dst;
		rctx->data = qcrypto_kzalloc(
			&rctx->qbuf, (req->nbytes + 64), GFP_ATOMIC);

		if (rctx->data == NULL) {
			pr_err("Mem Alloc fail rctx->data, err %ld for 0x%x\n",
				PTR_ERR(rctx->data), (req->nbytes + 64));
			return -ENOMEM;
		}
		num_sg = qcrypto_count_sg(req->src, req->nbytes);
		bytes = qcrypto_sg_copy_to_buffer(req->src, num_sg, rctx->data,
								req->nbytes);
		if (bytes != req->nbytes)
			pr_warn("bytes copied=0x%x bytes to copy= 0x%x", bytes,
								req->nbytes);
		sg_set_buf(&rctx->dsg, rctx->data, req->nbytes);
		sg_mark_end(&rctx->dsg);
		rctx->iv = req->info;

		req->src = &rctx->dsg;
		req->dst = &rctx->dsg;
	}
	qreq.op = QCE_REQ_ABLK_CIPHER;
	qreq.qce_cb = _qce_ablk_cipher_complete;
	qreq.areq = req;
	qreq.alg = rctx->alg;
	qreq.dir = rctx->dir;
	qreq.mode = rctx->mode;
	qreq.enckey = cipher_ctx->enc_key;
	qreq.encklen = cipher_ctx->enc_key_len;
	qreq.iv = req->info;
	qreq.ivsize = crypto_ablkcipher_ivsize(tfm);
	qreq.cryptlen = req->nbytes;
	qreq.use_pmem = 0;
	qreq.flags = cipher_ctx->flags;

	if ((cipher_ctx->enc_key_len == 0) &&
			(pengine->pcp->platform_support.hw_key_support == 0))
		ret = -EINVAL;
	else
		ret =  qce_ablk_cipher_req(pengine->qce, &qreq);

	return ret;
}

static int _qcrypto_process_ahash(struct crypto_engine *pengine,
			struct qcrypto_req_control *pqcrypto_req_control)
{
	struct crypto_async_request *async_req;
	struct ahash_request *req;
	struct qce_sha_req sreq;
	struct qcrypto_sha_req_ctx *rctx;
	struct qcrypto_sha_ctx *sha_ctx;
	int ret = 0;

	async_req = pqcrypto_req_control->req;
	req = container_of(async_req,
				struct ahash_request, base);
	rctx = ahash_request_ctx(req);
	sha_ctx = crypto_tfm_ctx(async_req->tfm);
	rctx->pengine = pengine;

	sreq.qce_cb = _qce_ahash_complete;
	sreq.digest =  &rctx->digest[0];
	sreq.src = req->src;
	sreq.auth_data[0] = rctx->byte_count[0];
	sreq.auth_data[1] = rctx->byte_count[1];
	sreq.auth_data[2] = rctx->byte_count[2];
	sreq.auth_data[3] = rctx->byte_count[3];
	sreq.first_blk = rctx->first_blk;
	sreq.last_blk = rctx->last_blk;
	sreq.size = req->nbytes;
	sreq.areq = req;
	sreq.flags = sha_ctx->flags;

	switch (sha_ctx->alg) {
	case QCE_HASH_SHA1:
		sreq.alg = QCE_HASH_SHA1;
		sreq.authkey = NULL;
		break;
	case QCE_HASH_SHA256:
		sreq.alg = QCE_HASH_SHA256;
		sreq.authkey = NULL;
		break;
	case QCE_HASH_SHA1_HMAC:
		sreq.alg = QCE_HASH_SHA1_HMAC;
		sreq.authkey = &sha_ctx->authkey[0];
		sreq.authklen = SHA_HMAC_KEY_SIZE;
		break;
	case QCE_HASH_SHA256_HMAC:
		sreq.alg = QCE_HASH_SHA256_HMAC;
		sreq.authkey = &sha_ctx->authkey[0];
		sreq.authklen = SHA_HMAC_KEY_SIZE;
		break;
	default:
		pr_err("Algorithm %d not supported, exiting", sha_ctx->alg);
		ret = -1;
		break;
	};
	ret =  qce_process_sha_req(pengine->qce, &sreq);

	return ret;
}

static int _qcrypto_process_aead(struct  crypto_engine *pengine,
			struct qcrypto_req_control *pqcrypto_req_control)
{
	struct crypto_async_request *async_req;
	struct qce_req qreq;
	int ret = 0;
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *cipher_ctx;
	struct aead_request *req;
	struct crypto_aead *aead;

	async_req = pqcrypto_req_control->req;
	req = container_of(async_req, struct aead_request, base);
	aead = crypto_aead_reqtfm(req);
	rctx = aead_request_ctx(req);
	rctx->pengine = pengine;
	cipher_ctx = crypto_tfm_ctx(async_req->tfm);

	qreq.op = QCE_REQ_AEAD;
	qreq.qce_cb = _qce_aead_complete;

	qreq.areq = req;
	qreq.alg = rctx->alg;
	qreq.dir = rctx->dir;
	qreq.mode = rctx->mode;
	qreq.iv = rctx->iv;

	qreq.enckey = cipher_ctx->enc_key;
	qreq.encklen = cipher_ctx->enc_key_len;
	qreq.authkey = cipher_ctx->auth_key;
	qreq.authklen = cipher_ctx->auth_key_len;
	qreq.authsize = crypto_aead_authsize(aead);
	qreq.auth_alg = cipher_ctx->auth_alg;
	if (qreq.mode == QCE_MODE_CCM)
		qreq.ivsize =  AES_BLOCK_SIZE;
	else
		qreq.ivsize =  crypto_aead_ivsize(aead);
	qreq.flags = cipher_ctx->flags;

	if (qreq.mode == QCE_MODE_CCM) {
		if (qreq.dir == QCE_ENCRYPT)
			qreq.cryptlen = req->cryptlen;
		else
			qreq.cryptlen = req->cryptlen -
						qreq.authsize;
		/* Get NONCE */
		ret = qccrypto_set_aead_ccm_nonce(&qreq);
		if (ret)
			return ret;

		/* Format Associated data    */
		ret = qcrypto_aead_ccm_format_adata(&qreq,
						&rctx->qbuf,
						req->assoclen,
						req->assoc);
		if (ret)
			return ret;

		rctx->assoc  = qreq.assoc;

		if (pengine->pcp->ce_support.aligned_only) {
			uint32_t bytes = 0;
			uint32_t num_sg = 0;

			rctx->orig_src = req->src;
			rctx->orig_dst = req->dst;

			if ((MAX_ALIGN_SIZE*2 > UINT_MAX - qreq.assoclen) ||
				((MAX_ALIGN_SIZE*2 + qreq.assoclen) >
						UINT_MAX - qreq.authsize) ||
				((MAX_ALIGN_SIZE*2 + qreq.assoclen +
						qreq.authsize) >
						UINT_MAX - req->cryptlen)) {
				pr_err("Integer overflow on aead req length.\n");
				qcrypto_kfree(&rctx->qbuf, qreq.assoc);
				return -EINVAL;
			}

			rctx->data = qcrypto_kzalloc(
				&rctx->qbuf,
				(req->cryptlen + qreq.assoclen +
				 qreq.authsize + MAX_ALIGN_SIZE*2),
				GFP_ATOMIC);
			if (rctx->data == NULL) {
				pr_err("Mem Alloc fail rctx->data, err %ld\n",
							PTR_ERR(rctx->data));
				qcrypto_kfree(&rctx->qbuf, qreq.assoc);
				return -ENOMEM;
			}
			if (qreq.assoclen)
				memcpy((char *)rctx->data, qreq.assoc,
						 qreq.assoclen);

			num_sg = qcrypto_count_sg(req->src, req->cryptlen);
			bytes = qcrypto_sg_copy_to_buffer(req->src, num_sg,
				rctx->data + qreq.assoclen , req->cryptlen);
			if (bytes != req->cryptlen)
				pr_warn("bytes copied=0x%x bytes to copy= 0x%x",
							bytes, req->cryptlen);
			sg_set_buf(&rctx->ssg, rctx->data, req->cryptlen +
							qreq.assoclen);
			sg_mark_end(&rctx->ssg);

			if (qreq.dir == QCE_ENCRYPT)
				sg_set_buf(&rctx->dsg, rctx->data,
					qreq.assoclen + qreq.cryptlen +
					ALIGN(qreq.authsize, 64));
			else
				sg_set_buf(&rctx->dsg, rctx->data,
						qreq.assoclen + req->cryptlen +
						qreq.authsize);
			sg_mark_end(&rctx->dsg);

			req->src = &rctx->ssg;
			req->dst = &rctx->dsg;
		}
	} else {
		size_t assoc_1_len;

		ret = qcrypto_aead_format_adata(&qreq, &rctx->qbuf,
			req->assoclen, req->assoc, cipher_ctx->aead_esn);
		if (ret)
			return ret;

		rctx->assoc  = qreq.assoc;
		if (qreq.assoclen)
			assoc_1_len = qreq.assoclen - qreq.trail_assoclen;
		else
			assoc_1_len = 0;
		rctx->assoc_1_len  = assoc_1_len;

		/* for aead operations, other than aes(ccm) */
		rctx->data = NULL;
		if (pengine->pcp->ce_support.aligned_only) {
			uint32_t bytes = 0;
			uint32_t num_sg = 0;

			rctx->orig_src = req->src;
			rctx->orig_dst = req->dst;
			/*
			 * The data area should be big enough to
			 * include  assoicated data, ciphering data stream,
			 * generated MAC, and CCM padding.
			 */
			if ((MAX_ALIGN_SIZE * 2 > UINT_MAX - req->assoclen) ||
				((MAX_ALIGN_SIZE * 2 + req->assoclen) >
						UINT_MAX - qreq.ivsize) ||
				((MAX_ALIGN_SIZE * 2 + req->assoclen
					+ qreq.ivsize)
						> UINT_MAX - req->cryptlen)) {
				pr_err("Integer overflow on aead req length.\n");
				qcrypto_kfree(&rctx->qbuf, qreq.assoc);
				return -EINVAL;
			}

			rctx->data = qcrypto_kzalloc(
				&rctx->qbuf,
				(req->cryptlen + req->assoclen +
				 qreq.ivsize + MAX_ALIGN_SIZE * 2),
				GFP_ATOMIC);
			if (rctx->data == NULL) {
				pr_err("Mem Alloc fail rctx->data, err %ld\n",
						PTR_ERR(rctx->data));
				qcrypto_kfree(&rctx->qbuf, qreq.assoc);
				return -ENOMEM;
			}

			/* copy associated data */
			if (assoc_1_len)
				memcpy(rctx->data, qreq.assoc, assoc_1_len);

			/* copy iv */
			memcpy(rctx->data + assoc_1_len, qreq.iv,
				qreq.ivsize);

			/* copy src */
			num_sg = qcrypto_count_sg(req->src, req->cryptlen);
			bytes = qcrypto_sg_copy_to_buffer(
					req->src,
					num_sg,
					rctx->data + assoc_1_len +
						qreq.ivsize,
					req->cryptlen);
			if (bytes != req->cryptlen)
				pr_warn("bytes copied=0x%x bytes to copy= 0x%x",
						bytes, req->cryptlen);
			if (qreq.trail_assoclen)
				memcpy(rctx->data + assoc_1_len + qreq.ivsize +
									bytes,
					qreq.assoc + assoc_1_len,
					qreq.trail_assoclen);

			sg_set_buf(&rctx->ssg, rctx->data,
				req->cryptlen + req->assoclen
					+ qreq.ivsize);
			sg_mark_end(&rctx->ssg);

			sg_set_buf(&rctx->dsg, rctx->data,
				req->cryptlen + req->assoclen
					+ qreq.ivsize);
			sg_mark_end(&rctx->dsg);
			req->src = &rctx->ssg;
			req->dst = &rctx->dsg;
		}
	}
	ret =  qce_aead_req(pengine->qce, &qreq);

	if (ret) {
		qcrypto_kfree(&rctx->qbuf, qreq.assoc);
		qcrypto_kfree(&rctx->qbuf, rctx->data);
	}

	return ret;
}

static struct crypto_engine *_qcrypto_static_assign_engine(
					struct crypto_priv *cp)
{
	struct crypto_engine *pengine;
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	if (cp->next_engine)
		pengine = cp->next_engine;
	else
		pengine = list_first_entry(&cp->engine_list,
				struct crypto_engine, elist);

	if (list_is_last(&pengine->elist, &cp->engine_list))
		cp->next_engine = list_first_entry(
			&cp->engine_list, struct crypto_engine, elist);
	else
		cp->next_engine = list_next_entry(pengine, elist);
	spin_unlock_irqrestore(&cp->lock, flags);
	return pengine;
}

static inline void __qcrypto_handle_backlogs(
	struct crypto_async_request *first,
	struct crypto_async_request *last)
{
	struct crypto_async_request *async_req = first;

	while (1) {
		async_req->complete(async_req, -EINPROGRESS);
		if (async_req == last)
			break;
		async_req = list_next_entry(async_req, list);
	}
}

static void _qcrypto_engine_fill_req_queue(struct crypto_engine *pengine)
{
	struct crypto_async_request *async_req = NULL;
	struct crypto_async_request *first_eng_backlog_req;
	struct crypto_async_request *last_eng_backlog_req;
	struct crypto_async_request *first_cp_backlog_req;
	struct crypto_async_request *last_cp_backlog_req;
	struct crypto_priv *cp = pengine->pcp;
	unsigned long flags;
	u32 n = pengine->max_req - pengine->req_count;

	if (pengine->req_queue.qlen >= n)
		return;

	spin_lock_irqsave(&cp->lock, flags);

	first_eng_backlog_req = crypto_get_backlog(&pengine->areq_queue);
	while (pengine->req_queue.qlen < n) {
		async_req = crypto_dequeue_request(&pengine->areq_queue);
		if (!async_req)
			break;
		crypto_enqueue_request(&pengine->req_queue, async_req);
	};
	last_eng_backlog_req = crypto_get_backlog(&pengine->areq_queue);
	if (first_eng_backlog_req) {
		if (!last_eng_backlog_req)
			last_eng_backlog_req = list_entry(
				pengine->areq_queue.list.prev,
				struct crypto_async_request,
				list);
		else
			last_eng_backlog_req = list_prev_entry(
				last_eng_backlog_req, list);
	}

	first_cp_backlog_req = crypto_get_backlog(&cp->req_queue);
	while (pengine->req_queue.qlen < n) {
		async_req = crypto_dequeue_request(&cp->req_queue);
		if (!async_req)
			break;
		crypto_enqueue_request(&pengine->req_queue, async_req);
	}
	last_cp_backlog_req = crypto_get_backlog(&cp->req_queue);
	if (first_cp_backlog_req) {
		if (!last_cp_backlog_req)
			last_cp_backlog_req = list_entry(
				cp->req_queue.list.prev,
				struct crypto_async_request,
				list);
		else
			last_cp_backlog_req = list_prev_entry(
				last_cp_backlog_req, list);
	}

	spin_unlock_irqrestore(&cp->lock, flags);

	if (first_eng_backlog_req && last_eng_backlog_req)
		__qcrypto_handle_backlogs(first_eng_backlog_req,
					  last_eng_backlog_req);

	if (first_cp_backlog_req && last_cp_backlog_req)
		__qcrypto_handle_backlogs(first_cp_backlog_req,
					  last_cp_backlog_req);
}

static int _qcrypto_engine_req(struct crypto_engine *pengine,
			       struct crypto_async_request *async_req)
{
	struct crypto_priv *cp = pengine->pcp;
	unsigned long flags = 0;
	u32 type;
	int ret = 0;
	struct crypto_stat *pstat;
	void *tfm_ctx;
	struct qcrypto_cipher_req_ctx *cipher_rctx;
	struct qcrypto_sha_req_ctx *ahash_rctx;
	struct ablkcipher_request *ablkcipher_req;
	struct ahash_request *ahash_req;
	struct aead_request *aead_req;
	struct qcrypto_resp_ctx *arsp = NULL;
	struct qcrypto_req_control *pqcrypto_req_control;
	bool need_lock = (cp->poll_ctl_num > 1) ? true : false;

	pstat = &_qcrypto_stat;

	/* add associated rsp entry to tfm response queue */
	type = crypto_tfm_alg_type(async_req->tfm);
	tfm_ctx = crypto_tfm_ctx(async_req->tfm);

	if (need_lock)
		spin_lock_irqsave(&cp->lock, flags);

	switch (type) {
	case CRYPTO_ALG_TYPE_AHASH:
		ahash_req = container_of(async_req,
			struct ahash_request, base);
		ahash_rctx = ahash_request_ctx(ahash_req);
		arsp = &ahash_rctx->rsp_entry;
		list_add_tail(
			&arsp->list,
			&((struct qcrypto_sha_ctx *)tfm_ctx)
				->rsp_queue);
		break;
	case CRYPTO_ALG_TYPE_ABLKCIPHER:
		ablkcipher_req = container_of(async_req,
			struct ablkcipher_request, base);
		cipher_rctx = ablkcipher_request_ctx(ablkcipher_req);
		arsp = &cipher_rctx->rsp_entry;
		list_add_tail(
			&arsp->list,
			&((struct qcrypto_cipher_ctx *)tfm_ctx)
				->rsp_queue);
		break;
	case CRYPTO_ALG_TYPE_AEAD:
	default:
		aead_req = container_of(async_req,
			struct aead_request, base);
		cipher_rctx = aead_request_ctx(aead_req);
		arsp = &cipher_rctx->rsp_entry;
		list_add_tail(
			&arsp->list,
			&((struct qcrypto_cipher_ctx *)tfm_ctx)
				->rsp_queue);
		break;
	}

	arsp->res = -EINPROGRESS;
	arsp->async_req = async_req;

	pqcrypto_req_control = qcrypto_alloc_req_control(pengine);
	if (pqcrypto_req_control == NULL) {
		pr_err("Allocation of request failed\n");
		ret = -ENOMEM;
		goto err;
	}

	pqcrypto_req_control->pce = pengine;
	pqcrypto_req_control->req = async_req;
	pqcrypto_req_control->arsp = arsp;
	pengine->active_seq++;
	pengine->check_flag = true;

	if (need_lock)
		spin_unlock_irqrestore(&cp->lock, flags);

	switch (type) {
	case CRYPTO_ALG_TYPE_ABLKCIPHER:
		ret = _qcrypto_process_ablkcipher(pengine,
					pqcrypto_req_control);
		break;
	case CRYPTO_ALG_TYPE_AHASH:
		ret = _qcrypto_process_ahash(pengine, pqcrypto_req_control);
		break;
	case CRYPTO_ALG_TYPE_AEAD:
		ret = _qcrypto_process_aead(pengine, pqcrypto_req_control);
		break;
	default:
		ret = -EINVAL;
	};

	pengine->total_req++;
err:
	if (ret) {
		pengine->err_req++;
		if (pqcrypto_req_control)
			qcrypto_free_req_control(pengine, pqcrypto_req_control);

		if (type == CRYPTO_ALG_TYPE_ABLKCIPHER)
			pstat->ablk_cipher_op_fail++;
		else
			if (type == CRYPTO_ALG_TYPE_AHASH)
				pstat->ahash_op_fail++;
			else
				pstat->aead_op_fail++;

		_qcrypto_tfm_complete(pengine, type, tfm_ctx, arsp, ret);
	};
	return ret;
}

static int _start_qcrypto_process(struct crypto_priv *cp,
				  struct crypto_engine *pengine)
{
	struct crypto_async_request *async_req = NULL;

	if (ACCESS_ONCE(cp->ce_req_proc_sts) == STOPPED)
		return 0;

	/* make sure it is in high bandwidth state */
	if (pengine->bw_state != BUS_HAS_BANDWIDTH)
		return 0;

	_qcrypto_engine_fill_req_queue(pengine);

	while (pengine->req_count < pengine->max_req) {
		async_req = crypto_dequeue_request(&pengine->req_queue);
		if (!async_req)
			break;
		_qcrypto_engine_req(pengine, async_req);
	}

	return 0;
}

static int _qcrypto_queue_req(struct crypto_priv *cp,
				struct crypto_engine *pengine,
				struct crypto_async_request *req)
{
	struct crypto_poll_ctl *ctl = NULL;
	int ret;
	unsigned long flags;

	if (list_empty(&cp->poll_ctl_list))
		return -EBUSY;

	spin_lock_irqsave(&cp->lock, flags);

	if (pengine) {
		ctl = pengine->pctl;
		ret = crypto_enqueue_request(&pengine->areq_queue, req);
	} else {
		ctl = list_first_entry(&cp->poll_ctl_list,
				       struct crypto_poll_ctl, list);
		ret = crypto_enqueue_request(&cp->req_queue, req);
		if (cp->req_queue.qlen > cp->max_qlen)
			cp->max_qlen = cp->req_queue.qlen;
	}
	spin_unlock_irqrestore(&cp->lock, flags);

	if (unlikely((ret == -EBUSY) &&
		     !(req->flags & CRYPTO_TFM_REQ_MAY_BACKLOG)))
		cp->req_drop_cnt++;
	else
		qcrypto_poll_ctl_sched(ctl);

	return ret;
}

static int _qcrypto_enc_aes_192_fallback(struct ablkcipher_request *req)
{
	struct crypto_tfm *tfm =
		crypto_ablkcipher_tfm(crypto_ablkcipher_reqtfm(req));
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	int err;

	ablkcipher_request_set_tfm(req, ctx->cipher_aes192_fb);
	err = crypto_ablkcipher_encrypt(req);
	ablkcipher_request_set_tfm(req, __crypto_ablkcipher_cast(tfm));
	return err;
}

static int _qcrypto_dec_aes_192_fallback(struct ablkcipher_request *req)
{
	struct crypto_tfm *tfm =
		crypto_ablkcipher_tfm(crypto_ablkcipher_reqtfm(req));
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	int err;

	ablkcipher_request_set_tfm(req, ctx->cipher_aes192_fb);
	err = crypto_ablkcipher_decrypt(req);
	ablkcipher_request_set_tfm(req, __crypto_ablkcipher_cast(tfm));
	return err;
}


static int _qcrypto_enc_aes_ecb(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev, "_qcrypto_enc_aes_ecb: %p\n", req);
#endif

	if ((ctx->enc_key_len == AES_KEYSIZE_192) &&
			(!cp->ce_support.aes_key_192) &&
				ctx->cipher_aes192_fb)
		return _qcrypto_enc_aes_192_fallback(req);

	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_ECB;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_enc_aes_cbc(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev, "_qcrypto_enc_aes_cbc: %p\n", req);
#endif

	if ((ctx->enc_key_len == AES_KEYSIZE_192) &&
			(!cp->ce_support.aes_key_192) &&
				ctx->cipher_aes192_fb)
		return _qcrypto_enc_aes_192_fallback(req);

	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_enc_aes_ctr(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
				CRYPTO_ALG_TYPE_ABLKCIPHER);
#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev, "_qcrypto_enc_aes_ctr: %p\n", req);
#endif

	if ((ctx->enc_key_len == AES_KEYSIZE_192) &&
			(!cp->ce_support.aes_key_192) &&
				ctx->cipher_aes192_fb)
		return _qcrypto_enc_aes_192_fallback(req);

	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CTR;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_enc_aes_xts(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_XTS;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_aead_encrypt_aes_ccm(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	if ((ctx->authsize > 16) || (ctx->authsize < 4) || (ctx->authsize & 1))
		return  -EINVAL;
	if ((ctx->auth_key_len != AES_KEYSIZE_128) &&
		(ctx->auth_key_len != AES_KEYSIZE_256))
		return  -EINVAL;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CCM;
	rctx->iv = req->iv;
	rctx->qbuf.used = 0;

	pstat->aead_ccm_aes_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_rfc4309_enc_aes_ccm(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CCM;
	memset(rctx->rfc4309_iv, 0, sizeof(rctx->rfc4309_iv));
	rctx->rfc4309_iv[0] = 3; /* L -1 */
	memcpy(&rctx->rfc4309_iv[1], ctx->ccm4309_nonce, 3);
	memcpy(&rctx->rfc4309_iv[4], req->iv, 8);
	rctx->iv = rctx->rfc4309_iv;
	rctx->qbuf.used = 0;
	pstat->aead_rfc4309_ccm_aes_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_enc_des_ecb(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_ECB;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_enc_des_cbc(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_enc_3des_ecb(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_ECB;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_3des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_enc_3des_cbc(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_3des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_aes_ecb(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
				CRYPTO_ALG_TYPE_ABLKCIPHER);
#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev, "_qcrypto_dec_aes_ecb: %p\n", req);
#endif

	if ((ctx->enc_key_len == AES_KEYSIZE_192) &&
			(!cp->ce_support.aes_key_192) &&
				ctx->cipher_aes192_fb)
		return _qcrypto_dec_aes_192_fallback(req);

	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_ECB;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_aes_cbc(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
				CRYPTO_ALG_TYPE_ABLKCIPHER);
#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev, "_qcrypto_dec_aes_cbc: %p\n", req);
#endif

	if ((ctx->enc_key_len == AES_KEYSIZE_192) &&
			(!cp->ce_support.aes_key_192) &&
				ctx->cipher_aes192_fb)
		return _qcrypto_dec_aes_192_fallback(req);

	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_aes_ctr(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev, "_qcrypto_dec_aes_ctr: %p\n", req);
#endif

	if ((ctx->enc_key_len == AES_KEYSIZE_192) &&
			(!cp->ce_support.aes_key_192) &&
				ctx->cipher_aes192_fb)
		return _qcrypto_dec_aes_192_fallback(req);

	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->mode = QCE_MODE_CTR;

	/* Note. There is no such thing as aes/counter mode, decrypt */
	rctx->dir = QCE_ENCRYPT;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_des_ecb(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_ECB;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_des_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_des_cbc(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_des_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_3des_ecb(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_ECB;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_3des_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_3des_cbc(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_3des_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};

static int _qcrypto_dec_aes_xts(struct ablkcipher_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	BUG_ON(crypto_tfm_alg_type(req->base.tfm) !=
					CRYPTO_ALG_TYPE_ABLKCIPHER);
	rctx = ablkcipher_request_ctx(req);
	rctx->aead = 0;
	rctx->alg = CIPHER_ALG_AES;
	rctx->mode = QCE_MODE_XTS;
	rctx->dir = QCE_DECRYPT;
	rctx->qbuf.used = 0;

	pstat->ablk_cipher_aes_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
};


static int _qcrypto_aead_decrypt_aes_ccm(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	if ((ctx->authsize > 16) || (ctx->authsize < 4) || (ctx->authsize & 1))
		return  -EINVAL;
	if ((ctx->auth_key_len != AES_KEYSIZE_128) &&
		(ctx->auth_key_len != AES_KEYSIZE_256))
		return  -EINVAL;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CCM;
	rctx->iv = req->iv;
	rctx->qbuf.used = 0;

	pstat->aead_ccm_aes_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_rfc4309_dec_aes_ccm(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;
	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CCM;
	memset(rctx->rfc4309_iv, 0, sizeof(rctx->rfc4309_iv));
	rctx->rfc4309_iv[0] = 3; /* L -1 */
	memcpy(&rctx->rfc4309_iv[1], ctx->ccm4309_nonce, 3);
	memcpy(&rctx->rfc4309_iv[4], req->iv, 8);
	rctx->iv = rctx->rfc4309_iv;
	rctx->qbuf.used = 0;
	pstat->aead_rfc4309_ccm_aes_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}
static int _qcrypto_aead_setauthsize(struct crypto_aead *authenc,
				unsigned int authsize)
{
	struct qcrypto_cipher_ctx *ctx = crypto_aead_ctx(authenc);

	ctx->authsize = authsize;
	return 0;
}

static int _qcrypto_aead_ccm_setauthsize(struct crypto_aead *authenc,
				  unsigned int authsize)
{
	struct qcrypto_cipher_ctx *ctx = crypto_aead_ctx(authenc);

	switch (authsize) {
	case 4:
	case 6:
	case 8:
	case 10:
	case 12:
	case 14:
	case 16:
		break;
	default:
		return -EINVAL;
	}
	ctx->authsize = authsize;
	return 0;
}

static int _qcrypto_aead_rfc4309_ccm_setauthsize(struct crypto_aead *authenc,
				  unsigned int authsize)
{
	struct qcrypto_cipher_ctx *ctx = crypto_aead_ctx(authenc);

	switch (authsize) {
	case 8:
	case 12:
	case 16:
		break;
	default:
		return -EINVAL;
	}
	ctx->authsize = authsize;
	return 0;
}


static int _qcrypto_aead_setkey(struct crypto_aead *tfm, const u8 *key,
			unsigned int keylen)
{
	struct qcrypto_cipher_ctx *ctx = crypto_aead_ctx(tfm);
	struct rtattr *rta = (struct rtattr *)key;
	struct crypto_authenc_key_param *param;
	int ret;

	if (!RTA_OK(rta, keylen))
		goto badkey;
	if (rta->rta_type != CRYPTO_AUTHENC_KEYA_PARAM)
		goto badkey;
	if (RTA_PAYLOAD(rta) < sizeof(*param))
		goto badkey;

	param = RTA_DATA(rta);
	ctx->enc_key_len = be32_to_cpu(param->enckeylen);

	key += RTA_ALIGN(rta->rta_len);
	keylen -= RTA_ALIGN(rta->rta_len);

	if (keylen < ctx->enc_key_len)
		goto badkey;

	ctx->auth_key_len = keylen - ctx->enc_key_len;
	if (ctx->enc_key_len >= QCRYPTO_MAX_KEY_SIZE ||
				ctx->auth_key_len >= QCRYPTO_MAX_KEY_SIZE)
		goto badkey;
	memset(ctx->auth_key, 0, QCRYPTO_MAX_KEY_SIZE);
	memcpy(ctx->enc_key, key + ctx->auth_key_len, ctx->enc_key_len);
	memcpy(ctx->auth_key, key, ctx->auth_key_len);

	if (ctx->enc_key_len == AES_KEYSIZE_192 &&  ctx->cipher_aes192_fb &&
			ctx->ahash_aead_aes192_fb) {
		crypto_ahash_clear_flags(ctx->ahash_aead_aes192_fb, ~0);
		ret = crypto_ahash_setkey(ctx->ahash_aead_aes192_fb,
					ctx->auth_key, ctx->auth_key_len);
		if (ret)
			goto badkey;
		crypto_ablkcipher_clear_flags(ctx->cipher_aes192_fb, ~0);
		ret = crypto_ablkcipher_setkey(ctx->cipher_aes192_fb,
					ctx->enc_key, ctx->enc_key_len);
		if (ret)
			goto badkey;
	}

	return 0;
badkey:
	ctx->enc_key_len = 0;
	crypto_aead_set_flags(tfm, CRYPTO_TFM_RES_BAD_KEY_LEN);
	return -EINVAL;
}

static int _qcrypto_aead_ccm_setkey(struct crypto_aead *aead, const u8 *key,
			unsigned int keylen)
{
	struct crypto_tfm *tfm = crypto_aead_tfm(aead);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	struct crypto_priv *cp = ctx->cp;

	switch (keylen) {
	case AES_KEYSIZE_128:
	case AES_KEYSIZE_256:
		break;
	case AES_KEYSIZE_192:
		if (cp->ce_support.aes_key_192)
			break;
	default:
		ctx->enc_key_len = 0;
		crypto_aead_set_flags(aead, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	};
	ctx->enc_key_len = keylen;
	memcpy(ctx->enc_key, key, keylen);
	ctx->auth_key_len = keylen;
	memcpy(ctx->auth_key, key, keylen);

	return 0;
}

static int _qcrypto_aead_rfc4309_ccm_setkey(struct crypto_aead *aead,
				 const u8 *key, unsigned int key_len)
{
	struct crypto_tfm *tfm = crypto_aead_tfm(aead);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(tfm);
	int ret;

	if (key_len < QCRYPTO_CCM4309_NONCE_LEN)
		return -EINVAL;
	key_len -= QCRYPTO_CCM4309_NONCE_LEN;
	memcpy(ctx->ccm4309_nonce, key + key_len,  QCRYPTO_CCM4309_NONCE_LEN);
	ret = _qcrypto_aead_ccm_setkey(aead, key, key_len);
	return ret;
};

static void _qcrypto_aead_aes_192_fb_a_cb(struct qcrypto_cipher_req_ctx *rctx,
								int res)
{
	struct aead_request *req;
	struct crypto_async_request *areq;

	req = rctx->aead_req;
	areq = &req->base;
	if (rctx->fb_aes_req)
		ablkcipher_request_free(rctx->fb_aes_req);
	if (rctx->fb_hash_req)
		ahash_request_free(rctx->fb_hash_req);
	rctx->fb_aes_req = NULL;
	rctx->fb_hash_req = NULL;
	qcrypto_kfree(&rctx->qbuf, rctx->fb_ahash_assoc_iv);
	qcrypto_kfree(&rctx->qbuf, rctx->fb_aes_iv);
	qcrypto_kfree(&rctx->qbuf, rctx->fb_ahash_sg);
	areq->complete(areq, res);
}

static void _aead_aes_fb_stage2_ahash_complete(
				struct crypto_async_request *base, int err)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct aead_request *req;
	struct qcrypto_cipher_ctx *ctx;

	rctx = base->data;
	req = rctx->aead_req;
	ctx = crypto_tfm_ctx(req->base.tfm);
	/* copy icv */
	if (err == 0)
		scatterwalk_map_and_copy(rctx->fb_ahash_digest,
					req->dst,
					req->cryptlen,
					ctx->authsize, 1);
	_qcrypto_aead_aes_192_fb_a_cb(rctx, err);
}


static int _start_aead_aes_fb_stage2_hmac(struct qcrypto_cipher_req_ctx *rctx)
{
	struct ahash_request *ahash_req;

	ahash_req = rctx->fb_hash_req;
	ahash_request_set_callback(ahash_req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				 _aead_aes_fb_stage2_ahash_complete, rctx);

	return crypto_ahash_digest(ahash_req);
}

static void _aead_aes_fb_stage2_decrypt_complete(
			struct crypto_async_request *base, int err)
{
	struct qcrypto_cipher_req_ctx *rctx;

	rctx = base->data;
	_qcrypto_aead_aes_192_fb_a_cb(rctx, err);
}

static int _start_aead_aes_fb_stage2_decrypt(
					struct qcrypto_cipher_req_ctx *rctx)
{
	struct ablkcipher_request *aes_req;

	aes_req = rctx->fb_aes_req;
	ablkcipher_request_set_callback(aes_req, CRYPTO_TFM_REQ_MAY_BACKLOG,
			_aead_aes_fb_stage2_decrypt_complete, rctx);
	return crypto_ablkcipher_decrypt(aes_req);
}

static void _aead_aes_fb_stage1_ahash_complete(
				struct crypto_async_request *base, int err)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct aead_request *req;
	struct qcrypto_cipher_ctx *ctx;

	rctx = base->data;
	req = rctx->aead_req;
	ctx = crypto_tfm_ctx(req->base.tfm);

	/* compare icv */
	if (err == 0) {
		unsigned char tmp[ctx->authsize];

		scatterwalk_map_and_copy(tmp, req->src,
			req->cryptlen - ctx->authsize, ctx->authsize, 0);
		if (memcmp(rctx->fb_ahash_digest, tmp, ctx->authsize) != 0)
			err = -EBADMSG;
	}
	if (err)
		_qcrypto_aead_aes_192_fb_a_cb(rctx, err);
	else {
		err = _start_aead_aes_fb_stage2_decrypt(rctx);
		if (err != -EINPROGRESS &&  err != -EBUSY)
			_qcrypto_aead_aes_192_fb_a_cb(rctx, err);
	}
}

static void _aead_aes_fb_stage1_encrypt_complete(
				struct crypto_async_request *base, int err)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct aead_request *req;
	struct qcrypto_cipher_ctx *ctx;

	rctx = base->data;
	req = rctx->aead_req;
	ctx = crypto_tfm_ctx(req->base.tfm);

	memcpy(ctx->iv, rctx->fb_aes_iv, rctx->ivsize);

	if (err) {
		_qcrypto_aead_aes_192_fb_a_cb(rctx, err);
		return;
	}

	err = _start_aead_aes_fb_stage2_hmac(rctx);

	/* copy icv */
	if (err == 0) {
		scatterwalk_map_and_copy(rctx->fb_ahash_digest,
					req->dst,
					req->cryptlen,
					ctx->authsize, 1);
	}
	if (err != -EINPROGRESS &&  err != -EBUSY)
		_qcrypto_aead_aes_192_fb_a_cb(rctx, err);
}

static int _qcrypto_aead_aes_192_fallback(struct aead_request *req,
							bool is_encrypt)
{
	struct qcrypto_cipher_req_ctx *rctx = aead_request_ctx(req);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_aead *aead_tfm = crypto_aead_reqtfm(req);
	struct ablkcipher_request *aes_req = NULL;
	struct ahash_request *ahash_req = NULL;
	int rc = -EINVAL;
	int nbytes;
	int num_sg;
	int nents;
	struct scatterlist *sg;
	char *tmp_ahash_assoc_iv;
	size_t associvlen;

	rctx->fb_ahash_assoc_iv = NULL;
	rctx->fb_aes_iv = NULL;
	rctx->fb_ahash_sg = NULL;
	aes_req = ablkcipher_request_alloc(ctx->cipher_aes192_fb, GFP_KERNEL);
	if (!aes_req)
		return -ENOMEM;
	ahash_req = ahash_request_alloc(ctx->ahash_aead_aes192_fb, GFP_KERNEL);
	if (!ahash_req)
		goto ret;
	rctx->fb_aes_req = aes_req;
	rctx->fb_hash_req = ahash_req;
	rctx->aead_req = req;
	num_sg = qcrypto_count_sg(req->assoc, req->assoclen);
	associvlen = req->assoclen + crypto_aead_ivsize(aead_tfm);
	tmp_ahash_assoc_iv = qcrypto_kzalloc(
		&rctx->qbuf, associvlen, GFP_ATOMIC);
	if (!tmp_ahash_assoc_iv) {
		rc = -ENOMEM;
		goto ret;
	}
	if (req->assoclen)
		qcrypto_sg_copy_to_buffer(req->assoc, num_sg,
			tmp_ahash_assoc_iv, req->assoclen);
	memcpy(tmp_ahash_assoc_iv + req->assoclen,
			req->iv, crypto_aead_ivsize(aead_tfm));
	if (ctx->aead_esn && req->assoclen) {
		if (req->assoclen <
			(ESP_EXT_SEQ_HI_OFFSET + ESP_EXT_SEQ_HI_SIZE +
				ESP_EXT_SEQ_LOW_SIZE)) {
			qcrypto_kfree(&rctx->qbuf, tmp_ahash_assoc_iv);
			rc = -EINVAL;
			goto ret;
		}
		rctx->fb_ahash_assoc_iv = qcrypto_kzalloc(
			&rctx->qbuf, associvlen, GFP_ATOMIC);
		if (!rctx->fb_ahash_assoc_iv) {
			qcrypto_kfree(&rctx->qbuf, tmp_ahash_assoc_iv);
			rc = -ENOMEM;
			goto ret;
		}
		memcpy(rctx->fb_ahash_assoc_iv, tmp_ahash_assoc_iv,
			ESP_EXT_SEQ_HI_OFFSET);
		memcpy(rctx->fb_ahash_assoc_iv + ESP_EXT_SEQ_HI_OFFSET,
			tmp_ahash_assoc_iv + ESP_EXT_SEQ_HI_OFFSET +
						ESP_EXT_SEQ_HI_SIZE,
			associvlen - (ESP_EXT_SEQ_HI_OFFSET +
						ESP_EXT_SEQ_HI_SIZE));
		memcpy(rctx->fb_ahash_assoc_iv + associvlen -
						ESP_EXT_SEQ_HI_SIZE,
			tmp_ahash_assoc_iv + ESP_EXT_SEQ_HI_OFFSET,
			ESP_EXT_SEQ_HI_SIZE);
		qcrypto_kfree(&rctx->qbuf, tmp_ahash_assoc_iv);
	} else
		rctx->fb_ahash_assoc_iv = tmp_ahash_assoc_iv;
	nbytes = req->cryptlen;
	if (is_encrypt)
		nents = qcrypto_count_sg(req->dst, nbytes);
	else {
		nbytes -=  ctx->authsize;
		nents = qcrypto_count_sg(req->src, nbytes);
	}
	rctx->fb_ahash_sg = qcrypto_kzalloc(
		&rctx->qbuf,
		sizeof(struct scatterlist) * (nents + 2),
		GFP_ATOMIC);
	if (rctx->fb_ahash_sg == NULL) {
		rc = -ENOMEM;
		goto ret;
	}
	sg = rctx->fb_ahash_sg;

	if (req->assoclen && ctx->aead_esn)
		sg_set_buf(sg, rctx->fb_ahash_assoc_iv,
				associvlen - ESP_EXT_SEQ_HI_SIZE);
	else
		sg_set_buf(sg, rctx->fb_ahash_assoc_iv, associvlen);
	sg++;
	if (is_encrypt)
		nents = qcrypto_dup_sg_len(req->dst, nents, sg, nbytes);
	else
		nents = qcrypto_dup_sg_len(req->src, nents, sg, nbytes);
	if (nents < 0) {
		rc = -EINVAL;
		goto ret;
	}
	sg += nents;
	if (ctx->aead_esn && req->assoclen)
		sg_set_buf(
			sg,
			rctx->fb_ahash_assoc_iv + associvlen -
					ESP_EXT_SEQ_HI_SIZE,
			ESP_EXT_SEQ_HI_SIZE
		);
	sg_mark_end(sg);

	rctx->fb_ahash_length = nbytes + crypto_aead_ivsize(aead_tfm)
							+ req->assoclen;
	rctx->fb_aes_src = req->src;
	rctx->fb_aes_dst = req->dst;
	rctx->fb_aes_cryptlen = nbytes;
	rctx->ivsize = crypto_aead_ivsize(aead_tfm);
	rctx->fb_aes_iv = qcrypto_kzalloc(
		&rctx->qbuf, rctx->ivsize, GFP_ATOMIC);
	if (!rctx->fb_aes_iv)
		goto ret;
	memcpy(rctx->fb_aes_iv, req->iv, rctx->ivsize);
	ablkcipher_request_set_crypt(aes_req, rctx->fb_aes_src,
					rctx->fb_aes_dst,
					rctx->fb_aes_cryptlen, rctx->fb_aes_iv);
	ahash_request_set_crypt(ahash_req, &rctx->fb_ahash_sg[0],
					rctx->fb_ahash_digest,
					rctx->fb_ahash_length);

	if (is_encrypt) {

		ablkcipher_request_set_callback(aes_req,
			CRYPTO_TFM_REQ_MAY_BACKLOG,
			_aead_aes_fb_stage1_encrypt_complete, rctx);

		rc = crypto_ablkcipher_encrypt(aes_req);
		if (rc == 0) {
			memcpy(ctx->iv, rctx->fb_aes_iv, rctx->ivsize);
			rc = _start_aead_aes_fb_stage2_hmac(rctx);
			if (rc == 0) {
				/* copy icv */
				scatterwalk_map_and_copy(rctx->fb_ahash_digest,
					req->dst,
					req->cryptlen,
					ctx->authsize, 1);
			}
		}
		if (rc == -EINPROGRESS || rc == -EBUSY)
			return rc;
		goto ret;

	} else {
		ahash_request_set_callback(ahash_req,
				CRYPTO_TFM_REQ_MAY_BACKLOG,
				_aead_aes_fb_stage1_ahash_complete, rctx);

		rc = crypto_ahash_digest(ahash_req);
		if (rc == 0) {
			unsigned char tmp[ctx->authsize];

			/* compare icv */
			scatterwalk_map_and_copy(tmp,
				req->src, req->cryptlen - ctx->authsize,
				ctx->authsize, 0);
			if (memcmp(rctx->fb_ahash_digest, tmp,
							ctx->authsize) != 0)
				rc = -EBADMSG;
			else
				rc = _start_aead_aes_fb_stage2_decrypt(rctx);
		}
		if (rc == -EINPROGRESS || rc == -EBUSY)
			return rc;
		goto ret;
	}
ret:
	if (aes_req)
		ablkcipher_request_free(aes_req);
	if (ahash_req)
		ahash_request_free(ahash_req);
	qcrypto_kfree(&rctx->qbuf, rctx->fb_ahash_assoc_iv);
	qcrypto_kfree(&rctx->qbuf, rctx->fb_aes_iv);
	qcrypto_kfree(&rctx->qbuf, rctx->fb_ahash_sg);
	return rc;
}

static int _qcrypto_aead_encrypt_aes_cbc(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev,
			 "_qcrypto_aead_encrypt_aes_cbc: %p\n", req);
#endif

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->iv;
	rctx->aead_req = req;
	rctx->qbuf.used = 0;
	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_aes_enc++;
	else
		pstat->aead_sha256_aes_enc++;
	if (ctx->enc_key_len == AES_KEYSIZE_192 &&  ctx->cipher_aes192_fb &&
						ctx->ahash_aead_aes192_fb)
		return _qcrypto_aead_aes_192_fallback(req, true);
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_decrypt_aes_cbc(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

#ifdef QCRYPTO_DEBUG
	dev_info(&ctx->pengine->pdev->dev,
			 "_qcrypto_aead_decrypt_aes_cbc: %p\n", req);
#endif
	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->iv;
	rctx->aead_req = req;
	rctx->qbuf.used = 0;

	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_aes_dec++;
	else
		pstat->aead_sha256_aes_dec++;

	if (ctx->enc_key_len == AES_KEYSIZE_192 &&  ctx->cipher_aes192_fb &&
						ctx->ahash_aead_aes192_fb)
		return _qcrypto_aead_aes_192_fallback(req, false);
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_givencrypt_aes_cbc(struct aead_givcrypt_request *req)
{
	struct aead_request *areq = &req->areq;
	struct crypto_aead *authenc = crypto_aead_reqtfm(areq);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(areq->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct qcrypto_cipher_req_ctx *rctx;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(areq);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_AES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->giv;	/* generated iv */
	rctx->aead_req = areq;
	rctx->qbuf.used = 0;

	memcpy(req->giv, ctx->iv, crypto_aead_ivsize(authenc));
	 /* avoid consecutive packets going out with same IV */
	*(__be64 *)req->giv ^= cpu_to_be64(req->seq);

	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_aes_enc++;
	else
		pstat->aead_sha256_aes_enc++;
	if (ctx->enc_key_len == AES_KEYSIZE_192 &&  ctx->cipher_aes192_fb &&
						ctx->ahash_aead_aes192_fb) {
		areq->iv = req->giv;
		return _qcrypto_aead_aes_192_fallback(areq, true);
	}
	return _qcrypto_queue_req(cp, ctx->pengine, &areq->base);
}

static int _qcrypto_aead_encrypt_des_cbc(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->iv;
	rctx->aead_req = req;
	rctx->qbuf.used = 0;

	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_des_enc++;
	else
		pstat->aead_sha256_des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_decrypt_des_cbc(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->iv;
	rctx->aead_req = req;
	rctx->qbuf.used = 0;

	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_des_dec++;
	else
		pstat->aead_sha256_des_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_givencrypt_des_cbc(struct aead_givcrypt_request *req)
{
	struct aead_request *areq = &req->areq;
	struct crypto_aead *authenc = crypto_aead_reqtfm(areq);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(areq->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct qcrypto_cipher_req_ctx *rctx;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(areq);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->giv;	/* generated iv */
	rctx->aead_req = areq;
	rctx->qbuf.used = 0;

	memcpy(req->giv, ctx->iv, crypto_aead_ivsize(authenc));
	 /* avoid consecutive packets going out with same IV */
	*(__be64 *)req->giv ^= cpu_to_be64(req->seq);
	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_des_enc++;
	else
		pstat->aead_sha256_des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &areq->base);
}

static int _qcrypto_aead_encrypt_3des_cbc(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->iv;
	rctx->aead_req = req;
	rctx->qbuf.used = 0;

	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_3des_enc++;
	else
		pstat->aead_sha256_3des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_decrypt_3des_cbc(struct aead_request *req)
{
	struct qcrypto_cipher_req_ctx *rctx;
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(req);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_DECRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->iv;
	rctx->aead_req = req;
	rctx->qbuf.used = 0;

	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_3des_dec++;
	else
		pstat->aead_sha256_3des_dec++;
	return _qcrypto_queue_req(cp, ctx->pengine, &req->base);
}

static int _qcrypto_aead_givencrypt_3des_cbc(struct aead_givcrypt_request *req)
{
	struct aead_request *areq = &req->areq;
	struct crypto_aead *authenc = crypto_aead_reqtfm(areq);
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(areq->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct qcrypto_cipher_req_ctx *rctx;
	struct crypto_stat *pstat;

	pstat = &_qcrypto_stat;

	rctx = aead_request_ctx(areq);
	rctx->aead = 1;
	rctx->alg = CIPHER_ALG_3DES;
	rctx->dir = QCE_ENCRYPT;
	rctx->mode = QCE_MODE_CBC;
	rctx->iv = req->giv;	/* generated iv */
	rctx->aead_req = areq;
	rctx->qbuf.used = 0;

	memcpy(req->giv, ctx->iv, crypto_aead_ivsize(authenc));
	 /* avoid consecutive packets going out with same IV */
	*(__be64 *)req->giv ^= cpu_to_be64(req->seq);
	if (ctx->auth_alg == QCE_HASH_SHA1_HMAC)
		pstat->aead_sha1_3des_enc++;
	else
		pstat->aead_sha256_3des_enc++;
	return _qcrypto_queue_req(cp, ctx->pengine, &areq->base);
}

static int _sha_init(struct ahash_request *req)
{
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	rctx->first_blk = 1;
	rctx->last_blk = 0;
	rctx->byte_count[0] = 0;
	rctx->byte_count[1] = 0;
	rctx->byte_count[2] = 0;
	rctx->byte_count[3] = 0;
	rctx->trailing_buf_len = 0;
	rctx->count = 0;
	rctx->qbuf.used = 0;

	return 0;
};

static int _sha1_init(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_stat *pstat;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	pstat = &_qcrypto_stat;

	_sha_init(req);
	sha_ctx->alg = QCE_HASH_SHA1;

	memset(&rctx->trailing_buf[0], 0x00, SHA1_BLOCK_SIZE);
	memcpy(&rctx->digest[0], &_std_init_vector_sha1_uint8[0],
						SHA1_DIGEST_SIZE);
	sha_ctx->diglen = SHA1_DIGEST_SIZE;
	pstat->sha1_digest++;
	return 0;
};

static int _sha256_init(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_stat *pstat;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	pstat = &_qcrypto_stat;

	_sha_init(req);
	sha_ctx->alg = QCE_HASH_SHA256;

	memset(&rctx->trailing_buf[0], 0x00, SHA256_BLOCK_SIZE);
	memcpy(&rctx->digest[0], &_std_init_vector_sha256_uint8[0],
						SHA256_DIGEST_SIZE);
	sha_ctx->diglen = SHA256_DIGEST_SIZE;
	pstat->sha256_digest++;
	return 0;
};


static int _sha1_export(struct ahash_request  *req, void *out)
{
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct sha1_state *out_ctx = (struct sha1_state *)out;

	out_ctx->count = rctx->count;
	_byte_stream_to_words(out_ctx->state, rctx->digest, SHA1_DIGEST_SIZE);
	memcpy(out_ctx->buffer, rctx->trailing_buf, SHA1_BLOCK_SIZE);

	return 0;
};

static int _sha1_hmac_export(struct ahash_request  *req, void *out)
{
	return _sha1_export(req, out);
}

/* crypto hw padding constant for hmac first operation */
#define HMAC_PADDING 64

static int __sha1_import_common(struct ahash_request  *req, const void *in,
				bool hmac)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct sha1_state *in_ctx = (struct sha1_state *)in;
	u64 hw_count = in_ctx->count;

	rctx->count = in_ctx->count;
	memcpy(rctx->trailing_buf, in_ctx->buffer, SHA1_BLOCK_SIZE);
	if (in_ctx->count <= SHA1_BLOCK_SIZE) {
		rctx->first_blk = 1;
	} else {
		rctx->first_blk = 0;
		/*
		 * For hmac, there is a hardware padding done
		 * when first is set. So the byte_count will be
		 * incremened by 64 after the operstion of first
		 */
		if (hmac)
			hw_count += HMAC_PADDING;
	}
	rctx->byte_count[0] =  (uint32_t)(hw_count & 0xFFFFFFC0);
	rctx->byte_count[1] =  (uint32_t)(hw_count >> 32);
	_words_to_byte_stream(in_ctx->state, rctx->digest, sha_ctx->diglen);

	rctx->trailing_buf_len = (uint32_t)(in_ctx->count &
						(SHA1_BLOCK_SIZE-1));
	return 0;
}

static int _sha1_import(struct ahash_request  *req, const void *in)
{
	return __sha1_import_common(req, in, false);
}

static int _sha1_hmac_import(struct ahash_request  *req, const void *in)
{
	return __sha1_import_common(req, in, true);
}

static int _sha256_export(struct ahash_request  *req, void *out)
{
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct sha256_state *out_ctx = (struct sha256_state *)out;

	out_ctx->count = rctx->count;
	_byte_stream_to_words(out_ctx->state, rctx->digest, SHA256_DIGEST_SIZE);
	memcpy(out_ctx->buf, rctx->trailing_buf, SHA256_BLOCK_SIZE);

	return 0;
};

static int _sha256_hmac_export(struct ahash_request  *req, void *out)
{
	return _sha256_export(req, out);
}

static int __sha256_import_common(struct ahash_request  *req, const void *in,
			bool hmac)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct sha256_state *in_ctx = (struct sha256_state *)in;
	u64 hw_count = in_ctx->count;

	rctx->count = in_ctx->count;
	memcpy(rctx->trailing_buf, in_ctx->buf, SHA256_BLOCK_SIZE);

	if (in_ctx->count <= SHA256_BLOCK_SIZE) {
		rctx->first_blk = 1;
	} else {
		rctx->first_blk = 0;
		/*
		 * for hmac, there is a hardware padding done
		 * when first is set. So the byte_count will be
		 * incremened by 64 after the operstion of first
		 */
		if (hmac)
			hw_count += HMAC_PADDING;
	}

	rctx->byte_count[0] =  (uint32_t)(hw_count & 0xFFFFFFC0);
	rctx->byte_count[1] =  (uint32_t)(hw_count >> 32);
	_words_to_byte_stream(in_ctx->state, rctx->digest, sha_ctx->diglen);

	rctx->trailing_buf_len = (uint32_t)(in_ctx->count &
						(SHA256_BLOCK_SIZE-1));


	return 0;
}

static int _sha256_import(struct ahash_request  *req, const void *in)
{
	return __sha256_import_common(req, in, false);
}

static int _sha256_hmac_import(struct ahash_request  *req, const void *in)
{
	return __sha256_import_common(req, in, true);
}

static int _copy_source(struct ahash_request  *req)
{
	struct qcrypto_sha_req_ctx *srctx = NULL;
	uint32_t bytes = 0;
	uint32_t num_sg = 0;

	srctx = ahash_request_ctx(req);
	srctx->orig_src = req->src;
	srctx->data = kzalloc((req->nbytes + 64), GFP_ATOMIC);
	if (srctx->data == NULL) {
		pr_err("Mem Alloc fail rctx->data, err %ld for 0x%x\n",
				PTR_ERR(srctx->data), (req->nbytes + 64));
		return -ENOMEM;
	}

	num_sg = qcrypto_count_sg(req->src, req->nbytes);
	bytes = qcrypto_sg_copy_to_buffer(req->src, num_sg, srctx->data,
						req->nbytes);
	if (bytes != req->nbytes)
		pr_warn("bytes copied=0x%x bytes to copy= 0x%x", bytes,
							req->nbytes);
	sg_set_buf(&srctx->dsg, srctx->data,
				req->nbytes);
	sg_mark_end(&srctx->dsg);
	req->src = &srctx->dsg;

	return 0;
}

static int _sha_update(struct ahash_request  *req, uint32_t sha_block_size)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	uint32_t total, len, num_sg;
	struct scatterlist *sg_last;
	uint8_t *k_src = NULL;
	uint32_t sha_pad_len = 0;
	uint32_t trailing_buf_len = 0;
	uint32_t nbytes;
	uint32_t offset = 0;
	uint32_t bytes = 0;
	uint8_t  *staging;
	int ret = 0;

	/* check for trailing buffer from previous updates and append it */
	total = req->nbytes + rctx->trailing_buf_len;
	len = req->nbytes;

	if (total <= sha_block_size) {
		k_src = &rctx->trailing_buf[rctx->trailing_buf_len];
		num_sg = qcrypto_count_sg(req->src, len);
		bytes = qcrypto_sg_copy_to_buffer(req->src, num_sg, k_src, len);

		rctx->trailing_buf_len = total;
		return 0;
	}

	/* save the original req structure fields*/
	rctx->src = req->src;
	rctx->nbytes = req->nbytes;

	staging = (uint8_t *)ALIGN(((uintptr_t)rctx->staging_dmabuf),
							L1_CACHE_BYTES);
	memcpy(staging, rctx->trailing_buf, rctx->trailing_buf_len);
	k_src = &rctx->trailing_buf[0];
	/*  get new trailing buffer */
	sha_pad_len = ALIGN(total, sha_block_size) - total;
	trailing_buf_len =  sha_block_size - sha_pad_len;
	offset = req->nbytes - trailing_buf_len;

	if (offset != req->nbytes)
		scatterwalk_map_and_copy(k_src, req->src, offset,
						trailing_buf_len, 0);

	nbytes = total - trailing_buf_len;
	num_sg = qcrypto_count_sg(req->src, req->nbytes);

	len = rctx->trailing_buf_len;
	sg_last = req->src;

	while ((len < nbytes) && sg_last) {
		if ((len + sg_last->length) > nbytes)
			break;
		len += sg_last->length;
		sg_last = scatterwalk_sg_next(sg_last);
	}
	if (rctx->trailing_buf_len) {
		if (cp->ce_support.aligned_only)  {
			rctx->data2 = qcrypto_kzalloc(
				&rctx->qbuf, (req->nbytes + 64), GFP_ATOMIC);
			if (rctx->data2 == NULL) {
				pr_err("Mem Alloc fail srctx->data2, err %ld\n",
							PTR_ERR(rctx->data2));
				return -ENOMEM;
			}
			memcpy(rctx->data2, staging,
						rctx->trailing_buf_len);
			memcpy((rctx->data2 + rctx->trailing_buf_len),
					rctx->data, req->src->length);
			qcrypto_kfree(&rctx->qbuf, rctx->data);
			rctx->data = rctx->data2;
			sg_set_buf(&rctx->sg[0], rctx->data,
					(rctx->trailing_buf_len +
							req->src->length));
			req->src = rctx->sg;
			sg_mark_end(&rctx->sg[0]);
		} else {
			if (sg_last)
				sg_mark_end(sg_last);
			memset(rctx->sg, 0, sizeof(rctx->sg));
			sg_set_buf(&rctx->sg[0], staging,
						rctx->trailing_buf_len);
			sg_mark_end(&rctx->sg[1]);
			sg_chain(rctx->sg, 2, req->src);
			req->src = rctx->sg;
		}
	} else {
		if (sg_last)
			sg_mark_end(sg_last);
	}

	req->nbytes = nbytes;
	rctx->trailing_buf_len = trailing_buf_len;

	ret =  _qcrypto_queue_req(cp, sha_ctx->pengine, &req->base);

	return ret;
};

static int _sha1_update(struct ahash_request  *req)
{
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;

	if (cp->ce_support.aligned_only) {
		if (_copy_source(req))
			return -ENOMEM;
	}
	rctx->count += req->nbytes;
	return _sha_update(req, SHA1_BLOCK_SIZE);
}

static int _sha256_update(struct ahash_request  *req)
{
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;

	if (cp->ce_support.aligned_only) {
		if (_copy_source(req))
			return -ENOMEM;
	}

	rctx->count += req->nbytes;
	return _sha_update(req, SHA256_BLOCK_SIZE);
}

static int _sha_finup(struct ahash_request *req, uint32_t sha_block_size)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	int ret = 0;
	uint8_t  *staging;
	uint32_t num_sg = 0;

	rctx->last_blk = 1;
	/* save the original req structure fields*/
	rctx->src = req->src;
	rctx->orig_src = req->src;
	rctx->nbytes = req->nbytes;
	staging = (uint8_t *)ALIGN(((uintptr_t)rctx->staging_dmabuf),
							L1_CACHE_BYTES);
	memcpy(staging, rctx->trailing_buf, rctx->trailing_buf_len);
	rctx->data = NULL;
	if (cp->ce_support.aligned_only)  {
		rctx->data = qcrypto_kzalloc(
			&rctx->qbuf,
			(req->nbytes + rctx->trailing_buf_len + 64),
			GFP_ATOMIC);

		if (rctx->data == NULL)
			return -ENOMEM;
		if (rctx->trailing_buf_len)
			memcpy(rctx->data, staging,
				rctx->trailing_buf_len);
		num_sg = qcrypto_count_sg(req->src, req->nbytes);
		qcrypto_sg_copy_to_buffer(req->src, num_sg,
			rctx->data + rctx->trailing_buf_len, req->nbytes);
		sg_set_buf(&rctx->sg[0], rctx->data,
			(rctx->trailing_buf_len + req->src->length));
		sg_mark_end(&rctx->sg[0]);
	} else if (rctx->trailing_buf_len) {
		sg_set_buf(&rctx->sg[0], staging, rctx->trailing_buf_len);
		sg_mark_end(&rctx->sg[0]);
		if (req->nbytes) {
			sg_unmark_end(&rctx->sg[0]);
			sg_chain(&rctx->sg[0], 2, req->src);
		}
	}
	req->src = rctx->sg;
	req->nbytes = rctx->trailing_buf_len + req->nbytes;
	ret =  _qcrypto_queue_req(cp, sha_ctx->pengine, &req->base);

	return ret;
}
static int _sha1_finup(struct ahash_request  *req)
{
	return _sha_finup(req, SHA1_BLOCK_SIZE);
}

static int _sha256_finup(struct ahash_request  *req)
{
	return _sha_finup(req, SHA256_BLOCK_SIZE);
}

static int _sha_final(struct ahash_request *req, uint32_t sha_block_size)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	int ret = 0;
	uint8_t  *staging;

	if (cp->ce_support.aligned_only) {
		if (_copy_source(req))
			return -ENOMEM;
	}

	rctx->last_blk = 1;

	/* save the original req structure fields*/
	rctx->src = req->src;
	rctx->nbytes = req->nbytes;

	staging = (uint8_t *)ALIGN(((uintptr_t)rctx->staging_dmabuf),
							L1_CACHE_BYTES);
	memcpy(staging, rctx->trailing_buf, rctx->trailing_buf_len);
	sg_set_buf(&rctx->sg[0], staging, rctx->trailing_buf_len);
	sg_mark_end(&rctx->sg[0]);

	req->src = &rctx->sg[0];
	req->nbytes = rctx->trailing_buf_len;

	ret =  _qcrypto_queue_req(cp, sha_ctx->pengine, &req->base);

	return ret;
};

static int _sha1_final(struct ahash_request  *req)
{
	return _sha_final(req, SHA1_BLOCK_SIZE);
}

static int _sha256_final(struct ahash_request  *req)
{
	return _sha_final(req, SHA256_BLOCK_SIZE);
}

static int _sha_digest(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_priv *cp = sha_ctx->cp;
	int ret = 0;

	if (cp->ce_support.aligned_only) {
		if (_copy_source(req))
			return -ENOMEM;
	}

	/* save the original req structure fields*/
	rctx->src = req->src;
	rctx->nbytes = req->nbytes;
	rctx->first_blk = 1;
	rctx->last_blk = 1;
	ret =  _qcrypto_queue_req(cp, sha_ctx->pengine, &req->base);

	return ret;
}

static int _sha1_digest(struct ahash_request *req)
{
	_sha1_init(req);
	return _sha_digest(req);
}

static int _sha256_digest(struct ahash_request *req)
{
	_sha256_init(req);
	return _sha_digest(req);
}

static void _crypto_sha_hmac_ahash_req_complete(
	struct crypto_async_request *req, int err)
{
	struct completion *ahash_req_complete = req->data;

	if (err == -EINPROGRESS)
		return;
	complete(ahash_req_complete);
}

static int _sha_hmac_setkey(struct crypto_ahash *tfm, const u8 *key,
		unsigned int len)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(&tfm->base);
	uint8_t	*in_buf;
	int ret = 0;
	struct scatterlist sg;
	struct ahash_request *ahash_req;
	struct completion ahash_req_complete;

	ahash_req = ahash_request_alloc(tfm, GFP_KERNEL);
	if (ahash_req == NULL)
		return -ENOMEM;
	init_completion(&ahash_req_complete);
	ahash_request_set_callback(ahash_req,
				CRYPTO_TFM_REQ_MAY_BACKLOG,
				_crypto_sha_hmac_ahash_req_complete,
				&ahash_req_complete);
	crypto_ahash_clear_flags(tfm, ~0);

	in_buf = kzalloc(len + 64, GFP_KERNEL);
	if (in_buf == NULL) {
		pr_err("qcrypto Can't Allocate mem: in_buf, error %ld\n",
			PTR_ERR(in_buf));
		ahash_request_free(ahash_req);
		return -ENOMEM;
	}
	memcpy(in_buf, key, len);
	sg_init_one(&sg, in_buf, len);

	ahash_request_set_crypt(ahash_req, &sg,
				&sha_ctx->authkey[0], len);

	if (sha_ctx->alg == QCE_HASH_SHA1)
		ret = _sha1_digest(ahash_req);
	else
		ret = _sha256_digest(ahash_req);
	if (ret == -EINPROGRESS || ret == -EBUSY) {
		ret =
			wait_for_completion_interruptible(
						&ahash_req_complete);
		INIT_COMPLETION(sha_ctx->ahash_req_complete);
	}

	kzfree(in_buf);
	ahash_request_free(ahash_req);

	return ret;
}

static int _sha1_hmac_setkey(struct crypto_ahash *tfm, const u8 *key,
							unsigned int len)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(&tfm->base);
	int ret = 0;
	memset(&sha_ctx->authkey[0], 0, SHA1_BLOCK_SIZE);
	if (len <= SHA1_BLOCK_SIZE) {
		memcpy(&sha_ctx->authkey[0], key, len);
		sha_ctx->authkey_in_len = len;
	} else {
		sha_ctx->alg = QCE_HASH_SHA1;
		sha_ctx->diglen = SHA1_DIGEST_SIZE;
		ret = _sha_hmac_setkey(tfm, key, len);
		if (ret)
			pr_err("SHA1 hmac setkey failed\n");
		sha_ctx->authkey_in_len = SHA1_BLOCK_SIZE;
	}
	return ret;
}

static int _sha256_hmac_setkey(struct crypto_ahash *tfm, const u8 *key,
							unsigned int len)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(&tfm->base);
	int ret = 0;

	memset(&sha_ctx->authkey[0], 0, SHA256_BLOCK_SIZE);
	if (len <= SHA256_BLOCK_SIZE) {
		memcpy(&sha_ctx->authkey[0], key, len);
		sha_ctx->authkey_in_len = len;
	} else {
		sha_ctx->alg = QCE_HASH_SHA256;
		sha_ctx->diglen = SHA256_DIGEST_SIZE;
		ret = _sha_hmac_setkey(tfm, key, len);
		if (ret)
			pr_err("SHA256 hmac setkey failed\n");
		sha_ctx->authkey_in_len = SHA256_BLOCK_SIZE;
	}

	return ret;
}

static int _sha_hmac_init_ihash(struct ahash_request *req,
						uint32_t sha_block_size)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	int i;

	for (i = 0; i < sha_block_size; i++)
		rctx->trailing_buf[i] = sha_ctx->authkey[i] ^ 0x36;
	rctx->trailing_buf_len = sha_block_size;

	return 0;
}

static int _sha1_hmac_init(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	struct crypto_stat *pstat;
	int ret = 0;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	pstat = &_qcrypto_stat;
	pstat->sha1_hmac_digest++;

	_sha_init(req);
	memset(&rctx->trailing_buf[0], 0x00, SHA1_BLOCK_SIZE);
	memcpy(&rctx->digest[0], &_std_init_vector_sha1_uint8[0],
						SHA1_DIGEST_SIZE);
	sha_ctx->diglen = SHA1_DIGEST_SIZE;

	if (cp->ce_support.sha_hmac)
			sha_ctx->alg = QCE_HASH_SHA1_HMAC;
	else {
		sha_ctx->alg = QCE_HASH_SHA1;
		ret = _sha_hmac_init_ihash(req, SHA1_BLOCK_SIZE);
	}

	return ret;
}

static int _sha256_hmac_init(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	struct crypto_stat *pstat;
	int ret = 0;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	pstat = &_qcrypto_stat;
	pstat->sha256_hmac_digest++;

	_sha_init(req);

	memset(&rctx->trailing_buf[0], 0x00, SHA256_BLOCK_SIZE);
	memcpy(&rctx->digest[0], &_std_init_vector_sha256_uint8[0],
						SHA256_DIGEST_SIZE);
	sha_ctx->diglen = SHA256_DIGEST_SIZE;

	if (cp->ce_support.sha_hmac)
		sha_ctx->alg = QCE_HASH_SHA256_HMAC;
	else {
		sha_ctx->alg = QCE_HASH_SHA256;
		ret = _sha_hmac_init_ihash(req, SHA256_BLOCK_SIZE);
	}

	return ret;
}

static int _sha1_hmac_update(struct ahash_request *req)
{
	return _sha1_update(req);
}

static int _sha256_hmac_update(struct ahash_request *req)
{
	return _sha256_update(req);
}

static int _sha_hmac_outer_hash(struct ahash_request *req,
		uint32_t sha_digest_size, uint32_t sha_block_size)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	struct crypto_priv *cp = sha_ctx->cp;
	int i;
	uint8_t  *staging;
	uint8_t *p;

	staging = (uint8_t *)ALIGN(((uintptr_t)rctx->staging_dmabuf),
							L1_CACHE_BYTES);
	p = staging;
	for (i = 0; i < sha_block_size; i++)
		*p++ = sha_ctx->authkey[i] ^ 0x5c;
	memcpy(p, &rctx->digest[0], sha_digest_size);
	sg_set_buf(&rctx->sg[0], staging, sha_block_size +
							sha_digest_size);
	sg_mark_end(&rctx->sg[0]);

	/* save the original req structure fields*/
	rctx->src = req->src;
	rctx->nbytes = req->nbytes;

	req->src = &rctx->sg[0];
	req->nbytes = sha_block_size + sha_digest_size;

	_sha_init(req);
	if (sha_ctx->alg == QCE_HASH_SHA1) {
		memcpy(&rctx->digest[0], &_std_init_vector_sha1_uint8[0],
							SHA1_DIGEST_SIZE);
		sha_ctx->diglen = SHA1_DIGEST_SIZE;
	} else {
		memcpy(&rctx->digest[0], &_std_init_vector_sha256_uint8[0],
							SHA256_DIGEST_SIZE);
		sha_ctx->diglen = SHA256_DIGEST_SIZE;
	}

	rctx->last_blk = 1;
	return  _qcrypto_queue_req(cp, sha_ctx->pengine, &req->base);
}

static int _sha_hmac_inner_hash(struct ahash_request *req,
		uint32_t sha_digest_size, uint32_t sha_block_size, bool data)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct ahash_request *areq = sha_ctx->ahash_req;
	struct crypto_priv *cp = sha_ctx->cp;
	int ret = 0;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);
	uint8_t  *staging;

	staging = (uint8_t *)ALIGN(((uintptr_t)rctx->staging_dmabuf),
							L1_CACHE_BYTES);
	memcpy(staging, rctx->trailing_buf, rctx->trailing_buf_len);
	sg_set_buf(&rctx->sg[0], staging, rctx->trailing_buf_len);
	sg_mark_end(&rctx->sg[0]);
	if (data) {
		if (req->nbytes) {
			sg_unmark_end(&rctx->sg[0]);
			sg_chain(&rctx->sg[0], 2, req->src);
		}
	};

	ahash_request_set_crypt(areq, &rctx->sg[0], &rctx->digest[0],
			(data) ? rctx->trailing_buf_len + req->nbytes :
					rctx->trailing_buf_len);
	rctx->last_blk = 1;
	ret =  _qcrypto_queue_req(cp, sha_ctx->pengine, &areq->base);

	if (ret == -EINPROGRESS || ret == -EBUSY) {
		ret =
		wait_for_completion_interruptible(&sha_ctx->ahash_req_complete);
		INIT_COMPLETION(sha_ctx->ahash_req_complete);
	}

	return ret;
}

static int _sha1_hmac_final(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	int ret = 0;

	if (cp->ce_support.sha_hmac)
		return _sha_final(req, SHA1_BLOCK_SIZE);
	else {
		ret = _sha_hmac_inner_hash(req, SHA1_DIGEST_SIZE,
						SHA1_BLOCK_SIZE, false);
		if (ret)
			return ret;
		return _sha_hmac_outer_hash(req, SHA1_DIGEST_SIZE,
							SHA1_BLOCK_SIZE);
	}
}

static int _sha256_hmac_final(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	int ret = 0;

	if (cp->ce_support.sha_hmac)
		return _sha_final(req, SHA256_BLOCK_SIZE);
	else {
		ret = _sha_hmac_inner_hash(req, SHA256_DIGEST_SIZE,
						SHA256_BLOCK_SIZE, false);
		if (ret)
			return ret;
		return _sha_hmac_outer_hash(req, SHA256_DIGEST_SIZE,
							SHA256_BLOCK_SIZE);
	}
	return 0;
}

static int _sha1_hmac_finup(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	int ret = 0;

	if (cp->ce_support.sha_hmac)
		return _sha_finup(req, SHA1_BLOCK_SIZE);
	ret = _sha_hmac_inner_hash(req, SHA1_DIGEST_SIZE,
						SHA1_BLOCK_SIZE, true);
	if (ret)
		return ret;
	return _sha_hmac_outer_hash(req, SHA1_DIGEST_SIZE, SHA1_BLOCK_SIZE);
}

static int _sha256_hmac_finup(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = sha_ctx->cp;
	int ret = 0;

	if (cp->ce_support.sha_hmac)
		return _sha_finup(req, SHA256_BLOCK_SIZE);
	ret = _sha_hmac_inner_hash(req, SHA256_DIGEST_SIZE,
						SHA256_BLOCK_SIZE, true);
	if (ret)
		return ret;
	return _sha_hmac_outer_hash(req, SHA256_DIGEST_SIZE,
							SHA256_BLOCK_SIZE);
}

/*
 * Note, sha1_hmac_digest does not support device that does not
 * do hmac operation in CE (ce_support.sha_hmac is false).
 * Since in Linux 3.10 or later, those devices (mainly equipped with crypto 3)
 * are not used, we won't fix it here.
 */
static int _sha1_hmac_digest(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_stat *pstat;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	pstat = &_qcrypto_stat;
	pstat->sha1_hmac_digest++;

	_sha_init(req);
	memcpy(&rctx->digest[0], &_std_init_vector_sha1_uint8[0],
							SHA1_DIGEST_SIZE);
	sha_ctx->diglen = SHA1_DIGEST_SIZE;
	sha_ctx->alg = QCE_HASH_SHA1_HMAC;

	return _sha_digest(req);
}

/*
 * Same note as previous note in _sha1_hmac_digest. We won't fix the issue here.
 */
static int _sha256_hmac_digest(struct ahash_request *req)
{
	struct qcrypto_sha_ctx *sha_ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_stat *pstat;
	struct qcrypto_sha_req_ctx *rctx = ahash_request_ctx(req);

	pstat = &_qcrypto_stat;
	pstat->sha256_hmac_digest++;

	_sha_init(req);
	memcpy(&rctx->digest[0], &_std_init_vector_sha256_uint8[0],
						SHA256_DIGEST_SIZE);
	sha_ctx->diglen = SHA256_DIGEST_SIZE;
	sha_ctx->alg = QCE_HASH_SHA256_HMAC;

	return _sha_digest(req);
}

static int _qcrypto_prefix_alg_cra_name(char cra_name[], unsigned int size)
{
	char new_cra_name[CRYPTO_MAX_ALG_NAME] = "qcom-";
	if (size >= CRYPTO_MAX_ALG_NAME - strlen("qcom-"))
		return -EINVAL;
	strlcat(new_cra_name, cra_name, CRYPTO_MAX_ALG_NAME);
	strlcpy(cra_name, new_cra_name, CRYPTO_MAX_ALG_NAME);
	return 0;
}

/*
 * Fill up fips_selftest_data structure
 */

static void _qcrypto_fips_selftest_d(struct fips_selftest_data *selftest_d,
					struct ce_hw_support *ce_support,
					char *prefix)
{
	strlcpy(selftest_d->algo_prefix, prefix, CRYPTO_MAX_ALG_NAME);
	selftest_d->prefix_ahash_algo = ce_support->use_sw_ahash_algo;
	selftest_d->prefix_hmac_algo = ce_support->use_sw_hmac_algo;
	selftest_d->prefix_aes_xts_algo = ce_support->use_sw_aes_xts_algo;
	selftest_d->prefix_aes_cbc_ecb_ctr_algo =
		ce_support->use_sw_aes_cbc_ecb_ctr_algo;
	selftest_d->prefix_aead_algo = ce_support->use_sw_aead_algo;
	selftest_d->ce_device = ce_support->ce_device;
}

int qcrypto_cipher_set_device(struct ablkcipher_request *req, unsigned int dev)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_engine *pengine = NULL;

	pengine = _qrypto_find_pengine_device(cp, dev);
	if (pengine == NULL)
		return -ENODEV;
	ctx->pengine = pengine;

	return 0;
};
EXPORT_SYMBOL(qcrypto_cipher_set_device);

int qcrypto_cipher_set_device_hw(struct ablkcipher_request *req, u32 dev,
			u32 hw_inst)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_engine *pengine = NULL;

	pengine = _qrypto_find_pengine_device_hw(cp, dev, hw_inst);
	if (pengine == NULL)
		return -ENODEV;
	ctx->pengine = pengine;

	return 0;
}
EXPORT_SYMBOL(qcrypto_cipher_set_device_hw);

int qcrypto_aead_set_device(struct aead_request *req, unsigned int dev)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_engine *pengine = NULL;

	pengine = _qrypto_find_pengine_device(cp, dev);
	if (pengine == NULL)
		return -ENODEV;
	ctx->pengine = pengine;

	return 0;
};
EXPORT_SYMBOL(qcrypto_aead_set_device);

int qcrypto_ahash_set_device(struct ahash_request *req, unsigned int dev)
{
	struct qcrypto_sha_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;
	struct crypto_engine *pengine = NULL;

	pengine = _qrypto_find_pengine_device(cp, dev);
	if (pengine == NULL)
		return -ENODEV;
	ctx->pengine = pengine;

	return 0;
};
EXPORT_SYMBOL(qcrypto_ahash_set_device);

int qcrypto_cipher_set_flag(struct ablkcipher_request *req, unsigned int flags)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;

	if ((flags & QCRYPTO_CTX_USE_HW_KEY) &&
		(cp->platform_support.hw_key_support == false)) {
		pr_err("%s HW key usage not supported\n", __func__);
		return -EINVAL;
	}
	if (((flags | ctx->flags) & QCRYPTO_CTX_KEY_MASK) ==
						QCRYPTO_CTX_KEY_MASK) {
		pr_err("%s Cannot set all key flags\n", __func__);
		return -EINVAL;
	}

	ctx->flags |= flags;
	return 0;
};
EXPORT_SYMBOL(qcrypto_cipher_set_flag);

int qcrypto_aead_set_flag(struct aead_request *req, unsigned int flags)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;

	if ((flags & QCRYPTO_CTX_USE_HW_KEY) &&
		(cp->platform_support.hw_key_support == false)) {
		pr_err("%s HW key usage not supported\n", __func__);
		return -EINVAL;
	}
	if (((flags | ctx->flags) & QCRYPTO_CTX_KEY_MASK) ==
						QCRYPTO_CTX_KEY_MASK) {
		pr_err("%s Cannot set all key flags\n", __func__);
		return -EINVAL;
	}

	ctx->flags |= flags;
	return 0;
};
EXPORT_SYMBOL(qcrypto_aead_set_flag);

int qcrypto_ahash_set_flag(struct ahash_request *req, unsigned int flags)
{
	struct qcrypto_sha_ctx *ctx = crypto_tfm_ctx(req->base.tfm);
	struct crypto_priv *cp = ctx->cp;

	if ((flags & QCRYPTO_CTX_USE_HW_KEY) &&
		(cp->platform_support.hw_key_support == false)) {
		pr_err("%s HW key usage not supported\n", __func__);
		return -EINVAL;
	}
	if (((flags | ctx->flags) & QCRYPTO_CTX_KEY_MASK) ==
						QCRYPTO_CTX_KEY_MASK) {
		pr_err("%s Cannot set all key flags\n", __func__);
		return -EINVAL;
	}

	ctx->flags |= flags;
	return 0;
};
EXPORT_SYMBOL(qcrypto_ahash_set_flag);

int qcrypto_cipher_clear_flag(struct ablkcipher_request *req,
							unsigned int flags)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);

	ctx->flags &= ~flags;
	return 0;

};
EXPORT_SYMBOL(qcrypto_cipher_clear_flag);

int qcrypto_aead_clear_flag(struct aead_request *req, unsigned int flags)
{
	struct qcrypto_cipher_ctx *ctx = crypto_tfm_ctx(req->base.tfm);

	ctx->flags &= ~flags;
	return 0;

};
EXPORT_SYMBOL(qcrypto_aead_clear_flag);

int qcrypto_ahash_clear_flag(struct ahash_request *req, unsigned int flags)
{
	struct qcrypto_sha_ctx *ctx = crypto_tfm_ctx(req->base.tfm);

	ctx->flags &= ~flags;
	return 0;
};
EXPORT_SYMBOL(qcrypto_ahash_clear_flag);

static struct ahash_alg _qcrypto_ahash_algos[] = {
	{
		.init		=	_sha1_init,
		.update		=	_sha1_update,
		.final		=	_sha1_final,
		.finup		=	_sha1_finup,
		.export		=	_sha1_export,
		.import		=	_sha1_import,
		.digest		=	_sha1_digest,
		.halg		= {
			.digestsize	= SHA1_DIGEST_SIZE,
			.statesize	= sizeof(struct sha1_state),
			.base	= {
				.cra_name	 = "sha1",
				.cra_driver_name = "qcrypto-sha1",
				.cra_priority	 = 300,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
							 CRYPTO_ALG_ASYNC,
				.cra_blocksize	 = SHA1_BLOCK_SIZE,
				.cra_ctxsize	 =
						sizeof(struct qcrypto_sha_ctx),
				.cra_alignmask	 = 0,
				.cra_type	 = &crypto_ahash_type,
				.cra_module	 = THIS_MODULE,
				.cra_init	 = _qcrypto_ahash_cra_init,
				.cra_exit	 = _qcrypto_ahash_cra_exit,
			},
		},
	},
	{
		.init		=	_sha256_init,
		.update		=	_sha256_update,
		.final		=	_sha256_final,
		.finup		=	_sha256_finup,
		.export		=	_sha256_export,
		.import		=	_sha256_import,
		.digest		=	_sha256_digest,
		.halg		= {
			.digestsize	= SHA256_DIGEST_SIZE,
			.statesize	= sizeof(struct sha256_state),
			.base		= {
				.cra_name	 = "sha256",
				.cra_driver_name = "qcrypto-sha256",
				.cra_priority	 = 300,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
							CRYPTO_ALG_ASYNC,
				.cra_blocksize	 = SHA256_BLOCK_SIZE,
				.cra_ctxsize	 =
						sizeof(struct qcrypto_sha_ctx),
				.cra_alignmask	 = 0,
				.cra_type	 = &crypto_ahash_type,
				.cra_module	 = THIS_MODULE,
				.cra_init	 = _qcrypto_ahash_cra_init,
				.cra_exit	 = _qcrypto_ahash_cra_exit,
			},
		},
	},
};

static struct ahash_alg _qcrypto_sha_hmac_algos[] = {
	{
		.init		=	_sha1_hmac_init,
		.update		=	_sha1_hmac_update,
		.final		=	_sha1_hmac_final,
		.finup		=	_sha1_hmac_finup,
		.export		=	_sha1_hmac_export,
		.import		=	_sha1_hmac_import,
		.digest		=	_sha1_hmac_digest,
		.setkey		=	_sha1_hmac_setkey,
		.halg		= {
			.digestsize	= SHA1_DIGEST_SIZE,
			.statesize	= sizeof(struct sha1_state),
			.base	= {
				.cra_name	 = "hmac(sha1)",
				.cra_driver_name = "qcrypto-hmac-sha1",
				.cra_priority	 = 300,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
							 CRYPTO_ALG_ASYNC,
				.cra_blocksize	 = SHA1_BLOCK_SIZE,
				.cra_ctxsize	 =
						sizeof(struct qcrypto_sha_ctx),
				.cra_alignmask	 = 0,
				.cra_type	 = &crypto_ahash_type,
				.cra_module	 = THIS_MODULE,
				.cra_init	 = _qcrypto_ahash_hmac_cra_init,
				.cra_exit	 = _qcrypto_ahash_cra_exit,
			},
		},
	},
	{
		.init		=	_sha256_hmac_init,
		.update		=	_sha256_hmac_update,
		.final		=	_sha256_hmac_final,
		.finup		=	_sha256_hmac_finup,
		.export		=	_sha256_hmac_export,
		.import		=	_sha256_hmac_import,
		.digest		=	_sha256_hmac_digest,
		.setkey		=	_sha256_hmac_setkey,
		.halg		= {
			.digestsize	= SHA256_DIGEST_SIZE,
			.statesize	= sizeof(struct sha256_state),
			.base		= {
				.cra_name	 = "hmac(sha256)",
				.cra_driver_name = "qcrypto-hmac-sha256",
				.cra_priority	 = 300,
				.cra_flags	 = CRYPTO_ALG_TYPE_AHASH |
							CRYPTO_ALG_ASYNC,
				.cra_blocksize	 = SHA256_BLOCK_SIZE,
				.cra_ctxsize	 =
						sizeof(struct qcrypto_sha_ctx),
				.cra_alignmask	 = 0,
				.cra_type	 = &crypto_ahash_type,
				.cra_module	 = THIS_MODULE,
				.cra_init	 = _qcrypto_ahash_hmac_cra_init,
				.cra_exit	 = _qcrypto_ahash_cra_exit,
			},
		},
	},
};

static struct crypto_alg _qcrypto_ablk_cipher_algos[] = {
	{
		.cra_name		= "ecb(aes)",
		.cra_driver_name	= "qcrypto-ecb-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER |
					CRYPTO_ALG_NEED_FALLBACK |
					CRYPTO_ALG_ASYNC,
		.cra_blocksize	= AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aes_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_aes_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.min_keysize	= AES_MIN_KEY_SIZE,
				.max_keysize	= AES_MAX_KEY_SIZE,
				.setkey		= _qcrypto_setkey_aes,
				.encrypt	= _qcrypto_enc_aes_ecb,
				.decrypt	= _qcrypto_dec_aes_ecb,
			},
		},
	},
	{
		.cra_name	= "cbc(aes)",
		.cra_driver_name = "qcrypto-cbc-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER |
					CRYPTO_ALG_NEED_FALLBACK |
					CRYPTO_ALG_ASYNC,
		.cra_blocksize	= AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aes_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_aes_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.ivsize		= AES_BLOCK_SIZE,
				.min_keysize	= AES_MIN_KEY_SIZE,
				.max_keysize	= AES_MAX_KEY_SIZE,
				.setkey		= _qcrypto_setkey_aes,
				.encrypt	= _qcrypto_enc_aes_cbc,
				.decrypt	= _qcrypto_dec_aes_cbc,
			},
		},
	},
	{
		.cra_name	= "ctr(aes)",
		.cra_driver_name = "qcrypto-ctr-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER |
					CRYPTO_ALG_NEED_FALLBACK |
					CRYPTO_ALG_ASYNC,
		.cra_blocksize	= AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aes_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_aes_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.ivsize		= AES_BLOCK_SIZE,
				.min_keysize	= AES_MIN_KEY_SIZE,
				.max_keysize	= AES_MAX_KEY_SIZE,
				.setkey		= _qcrypto_setkey_aes,
				.encrypt	= _qcrypto_enc_aes_ctr,
				.decrypt	= _qcrypto_dec_aes_ctr,
			},
		},
	},
	{
		.cra_name		= "ecb(des)",
		.cra_driver_name	= "qcrypto-ecb-des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
		.cra_blocksize	= DES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.min_keysize	= DES_KEY_SIZE,
				.max_keysize	= DES_KEY_SIZE,
				.setkey		= _qcrypto_setkey_des,
				.encrypt	= _qcrypto_enc_des_ecb,
				.decrypt	= _qcrypto_dec_des_ecb,
			},
		},
	},
	{
		.cra_name	= "cbc(des)",
		.cra_driver_name = "qcrypto-cbc-des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
		.cra_blocksize	= DES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.ivsize		= DES_BLOCK_SIZE,
				.min_keysize	= DES_KEY_SIZE,
				.max_keysize	= DES_KEY_SIZE,
				.setkey		= _qcrypto_setkey_des,
				.encrypt	= _qcrypto_enc_des_cbc,
				.decrypt	= _qcrypto_dec_des_cbc,
			},
		},
	},
	{
		.cra_name		= "ecb(des3_ede)",
		.cra_driver_name	= "qcrypto-ecb-3des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
		.cra_blocksize	= DES3_EDE_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.min_keysize	= DES3_EDE_KEY_SIZE,
				.max_keysize	= DES3_EDE_KEY_SIZE,
				.setkey		= _qcrypto_setkey_3des,
				.encrypt	= _qcrypto_enc_3des_ecb,
				.decrypt	= _qcrypto_dec_3des_ecb,
			},
		},
	},
	{
		.cra_name	= "cbc(des3_ede)",
		.cra_driver_name = "qcrypto-cbc-3des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
		.cra_blocksize	= DES3_EDE_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_ablkcipher_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_ablkcipher_init,
		.cra_exit	= _qcrypto_cra_ablkcipher_exit,
		.cra_u		= {
			.ablkcipher = {
				.ivsize		= DES3_EDE_BLOCK_SIZE,
				.min_keysize	= DES3_EDE_KEY_SIZE,
				.max_keysize	= DES3_EDE_KEY_SIZE,
				.setkey		= _qcrypto_setkey_3des,
				.encrypt	= _qcrypto_enc_3des_cbc,
				.decrypt	= _qcrypto_dec_3des_cbc,
			},
		},
	},
};

static struct crypto_alg _qcrypto_ablk_cipher_xts_algo = {
	.cra_name	= "xts(aes)",
	.cra_driver_name = "qcrypto-xts-aes",
	.cra_priority	= 300,
	.cra_flags	= CRYPTO_ALG_TYPE_ABLKCIPHER | CRYPTO_ALG_ASYNC,
	.cra_blocksize	= AES_BLOCK_SIZE,
	.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
	.cra_alignmask	= 0,
	.cra_type	= &crypto_ablkcipher_type,
	.cra_module	= THIS_MODULE,
	.cra_init	= _qcrypto_cra_ablkcipher_init,
	.cra_exit	= _qcrypto_cra_ablkcipher_exit,
	.cra_u		= {
		.ablkcipher = {
			.ivsize		= AES_BLOCK_SIZE,
			.min_keysize	= AES_MIN_KEY_SIZE,
			.max_keysize	= AES_MAX_KEY_SIZE,
			.setkey		= _qcrypto_setkey_aes_xts,
			.encrypt	= _qcrypto_enc_aes_xts,
			.decrypt	= _qcrypto_dec_aes_xts,
		},
	},
};

static struct crypto_alg _qcrypto_aead_sha1_hmac_algos[] = {
	{
		.cra_name	= "authenc(hmac(sha1),cbc(aes))",
		.cra_driver_name = "qcrypto-aead-hmac-sha1-cbc-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_aes_sha1_init,
		.cra_exit	= _qcrypto_cra_aead_aes_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = AES_BLOCK_SIZE,
				.maxauthsize    = SHA1_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_aes_cbc,
				.decrypt = _qcrypto_aead_decrypt_aes_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_aes_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authenc(hmac(sha1),cbc(des))",
		.cra_driver_name = "qcrypto-aead-hmac-sha1-cbc-des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha1_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES_BLOCK_SIZE,
				.maxauthsize    = SHA1_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_des_cbc,
				.decrypt = _qcrypto_aead_decrypt_des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authenc(hmac(sha1),cbc(des3_ede))",
		.cra_driver_name = "qcrypto-aead-hmac-sha1-cbc-3des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES3_EDE_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha1_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES3_EDE_BLOCK_SIZE,
				.maxauthsize    = SHA1_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_3des_cbc,
				.decrypt = _qcrypto_aead_decrypt_3des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_3des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authencesn(hmac(sha1),cbc(aes))",
		.cra_driver_name = "qcrypto-aead-hmac-sha1-cbc-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_aes_sha1_esn_init,
		.cra_exit	= _qcrypto_cra_aead_aes_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = AES_BLOCK_SIZE,
				.maxauthsize    = SHA1_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_aes_cbc,
				.decrypt = _qcrypto_aead_decrypt_aes_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_aes_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authencesn(hmac(sha1),cbc(des))",
		.cra_driver_name = "qcrypto-aead-hmac-sha1-cbc-des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha1_esn_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES_BLOCK_SIZE,
				.maxauthsize    = SHA1_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_des_cbc,
				.decrypt = _qcrypto_aead_decrypt_des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authencesn(hmac(sha1),cbc(des3_ede))",
		.cra_driver_name = "qcrypto-aead-hmac-sha1-cbc-3des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES3_EDE_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha1_esn_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES3_EDE_BLOCK_SIZE,
				.maxauthsize    = SHA1_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_3des_cbc,
				.decrypt = _qcrypto_aead_decrypt_3des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_3des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
};

static struct crypto_alg _qcrypto_aead_sha256_hmac_algos[] = {
	{
		.cra_name	= "authenc(hmac(sha256),cbc(aes))",
		.cra_driver_name = "qcrypto-aead-hmac-sha256-cbc-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_aes_sha256_init,
		.cra_exit	= _qcrypto_cra_aead_aes_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = AES_BLOCK_SIZE,
				.maxauthsize    = SHA256_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_aes_cbc,
				.decrypt = _qcrypto_aead_decrypt_aes_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_aes_cbc,
				.geniv = "<built-in>",
			}
		}
	},

	{
		.cra_name	= "authenc(hmac(sha256),cbc(des))",
		.cra_driver_name = "qcrypto-aead-hmac-sha256-cbc-des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha256_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES_BLOCK_SIZE,
				.maxauthsize    = SHA256_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_des_cbc,
				.decrypt = _qcrypto_aead_decrypt_des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authenc(hmac(sha256),cbc(des3_ede))",
		.cra_driver_name = "qcrypto-aead-hmac-sha256-cbc-3des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES3_EDE_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha256_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES3_EDE_BLOCK_SIZE,
				.maxauthsize    = SHA256_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_3des_cbc,
				.decrypt = _qcrypto_aead_decrypt_3des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_3des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authencesn(hmac(sha256),cbc(aes))",
		.cra_driver_name = "qcrypto-aead-hmac-sha256-cbc-aes",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = AES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_aes_sha256_esn_init,
		.cra_exit	= _qcrypto_cra_aead_aes_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = AES_BLOCK_SIZE,
				.maxauthsize    = SHA256_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_aes_cbc,
				.decrypt = _qcrypto_aead_decrypt_aes_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_aes_cbc,
				.geniv = "<built-in>",
			}
		}
	},

	{
		.cra_name	= "authencesn(hmac(sha256),cbc(des))",
		.cra_driver_name = "qcrypto-aead-hmac-sha256-cbc-des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha256_esn_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES_BLOCK_SIZE,
				.maxauthsize    = SHA256_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_des_cbc,
				.decrypt = _qcrypto_aead_decrypt_des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
	{
		.cra_name	= "authencesn(hmac(sha256),cbc(des3_ede))",
		.cra_driver_name = "qcrypto-aead-hmac-sha256-cbc-3des",
		.cra_priority	= 300,
		.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
		.cra_blocksize  = DES3_EDE_BLOCK_SIZE,
		.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
		.cra_alignmask	= 0,
		.cra_type	= &crypto_aead_type,
		.cra_module	= THIS_MODULE,
		.cra_init	= _qcrypto_cra_aead_sha256_esn_init,
		.cra_exit	= _qcrypto_cra_aead_exit,
		.cra_u		= {
			.aead = {
				.ivsize         = DES3_EDE_BLOCK_SIZE,
				.maxauthsize    = SHA256_DIGEST_SIZE,
				.setkey = _qcrypto_aead_setkey,
				.setauthsize = _qcrypto_aead_setauthsize,
				.encrypt = _qcrypto_aead_encrypt_3des_cbc,
				.decrypt = _qcrypto_aead_decrypt_3des_cbc,
				.givencrypt = _qcrypto_aead_givencrypt_3des_cbc,
				.geniv = "<built-in>",
			}
		}
	},
};

static struct crypto_alg _qcrypto_aead_ccm_algo = {
	.cra_name	= "ccm(aes)",
	.cra_driver_name = "qcrypto-aes-ccm",
	.cra_priority	= 300,
	.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
	.cra_blocksize  = AES_BLOCK_SIZE,
	.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
	.cra_alignmask	= 0,
	.cra_type	= &crypto_aead_type,
	.cra_module	= THIS_MODULE,
	.cra_init	= _qcrypto_cra_aead_ccm_init,
	.cra_exit	= _qcrypto_cra_aead_exit,
	.cra_u		= {
		.aead = {
			.ivsize         = AES_BLOCK_SIZE,
			.maxauthsize    = AES_BLOCK_SIZE,
			.setkey = _qcrypto_aead_ccm_setkey,
			.setauthsize = _qcrypto_aead_ccm_setauthsize,
			.encrypt = _qcrypto_aead_encrypt_aes_ccm,
			.decrypt = _qcrypto_aead_decrypt_aes_ccm,
			.geniv = "<built-in>",
		}
	}
};

static struct crypto_alg _qcrypto_aead_rfc4309_ccm_algo = {
	.cra_name	= "rfc4309(ccm(aes))",
	.cra_driver_name = "qcrypto-rfc4309-aes-ccm",
	.cra_priority	= 300,
	.cra_flags	= CRYPTO_ALG_TYPE_AEAD | CRYPTO_ALG_ASYNC,
	.cra_blocksize  = 1,
	.cra_ctxsize	= sizeof(struct qcrypto_cipher_ctx),
	.cra_alignmask	= 0,
	.cra_type	= &crypto_nivaead_type,
	.cra_module	= THIS_MODULE,
	.cra_init	= _qcrypto_cra_aead_rfc4309_ccm_init,
	.cra_exit	= _qcrypto_cra_aead_exit,
	.cra_u		= {
		.aead = {
			.ivsize         = 8,
			.maxauthsize    = 16,
			.setkey = _qcrypto_aead_rfc4309_ccm_setkey,
			.setauthsize = _qcrypto_aead_rfc4309_ccm_setauthsize,
			.encrypt = _qcrypto_aead_rfc4309_enc_aes_ccm,
			.decrypt = _qcrypto_aead_rfc4309_dec_aes_ccm,
			.geniv = "seqiv",
		}
	}
};


static int  _qcrypto_probe(struct platform_device *pdev)
{
	int rc = 0;
	void *handle;
	struct crypto_priv *cp = &qcrypto_dev;
	int i;
	struct msm_ce_hw_support *platform_support;
	struct crypto_engine *pengine;
	unsigned long flags;
	struct qcrypto_req_control *pqcrypto_req_control = NULL;

	/* For FIPS140-2 Power on self tests */
	struct fips_selftest_data selftest_d;
	char prefix[10] = "";

	pengine = kzalloc(sizeof(*pengine), GFP_KERNEL);
	if (!pengine) {
		pr_err("qcrypto Memory allocation of q_alg FAIL, error %ld\n",
				PTR_ERR(pengine));
		return -ENOMEM;
	}

	/* open qce */
	handle = qce_open(pdev, QCE_OFLAG_POLLING, &rc);
	if (handle == NULL) {
		kzfree(pengine);
		platform_set_drvdata(pdev, NULL);
		return rc;
	}

	platform_set_drvdata(pdev, pengine);
	pengine->qce = handle;
	pengine->pcp = cp;
	pengine->pdev = pdev;
	pengine->signature = 0xdeadbeef;

	init_timer(&(pengine->bw_reaper_timer));
	INIT_WORK(&pengine->bw_reaper_ws, qcrypto_bw_reaper_work);
	pengine->bw_reaper_timer.function =
			qcrypto_bw_reaper_timer_callback;
	INIT_WORK(&pengine->bw_allocate_ws, qcrypto_bw_allocate_work);
	pengine->active_seq = 0;
	pengine->last_active_seq = 0;
	pengine->check_flag = false;
	pengine->max_req_used = 0;

	crypto_init_queue(&pengine->req_queue, MSM_QCRYPTO_REQ_QUEUE_LENGTH);

	mutex_lock(&cp->engine_lock);
	cp->total_units++;
	pengine->unit = cp->total_units;

	spin_lock_irqsave(&cp->lock, flags);
	list_add_tail(&pengine->elist, &cp->engine_list);
	cp->next_engine = pengine;
	spin_unlock_irqrestore(&cp->lock, flags);

	qce_hw_support(pengine->qce, &cp->ce_support);
	pengine->ce_hw_instance = cp->ce_support.ce_hw_instance;
	pengine->max_req = cp->ce_support.max_request;
	pqcrypto_req_control = kzalloc(sizeof(struct qcrypto_req_control) *
			pengine->max_req, GFP_KERNEL);
	if (pqcrypto_req_control == NULL) {
		rc = -ENOMEM;
		goto err;
	}
	qcrypto_init_req_control(pengine, pqcrypto_req_control);
	if (qcrypto_engine_set_polling_cpu(pengine, DEFAULT_POLL_CPU)) {
		rc = -ENOMEM;
		goto err;
	}
	if (cp->ce_support.bam)	 {
		cp->platform_support.ce_shared = cp->ce_support.is_shared;
		cp->platform_support.shared_ce_resource = 0;
		cp->platform_support.hw_key_support = cp->ce_support.hw_key;
		cp->platform_support.sha_hmac = 1;

		cp->platform_support.bus_scale_table =
			(struct msm_bus_scale_pdata *)
					msm_bus_cl_get_pdata(pdev);
		if (!cp->platform_support.bus_scale_table)
			pr_warn("bus_scale_table is NULL\n");

		pengine->ce_device = cp->ce_support.ce_device;

	} else {
		platform_support =
			(struct msm_ce_hw_support *)pdev->dev.platform_data;
		cp->platform_support.ce_shared = platform_support->ce_shared;
		cp->platform_support.shared_ce_resource =
				platform_support->shared_ce_resource;
		cp->platform_support.hw_key_support =
				platform_support->hw_key_support;
		cp->platform_support.bus_scale_table =
				platform_support->bus_scale_table;
		cp->platform_support.sha_hmac = platform_support->sha_hmac;
	}

	pengine->bus_scale_handle = 0;

	if (cp->platform_support.bus_scale_table != NULL) {
		pengine->bus_scale_handle =
			msm_bus_scale_register_client(
				(struct msm_bus_scale_pdata *)
					cp->platform_support.bus_scale_table);
		if (!pengine->bus_scale_handle) {
			pr_err("%s not able to get bus scale\n",
				__func__);
			rc =  -ENOMEM;
			goto err;
		}
		pengine->bw_state = BUS_NO_BANDWIDTH;
	} else {
		pengine->bw_state = BUS_HAS_BANDWIDTH;
	}

	if (cp->total_units != 1) {
		mutex_unlock(&cp->engine_lock);
		goto fips_selftest;
	}

	/* register crypto cipher algorithms the device supports */
	for (i = 0; i < ARRAY_SIZE(_qcrypto_ablk_cipher_algos); i++) {
		struct qcrypto_alg *q_alg;

		q_alg = _qcrypto_cipher_alg_alloc(cp,
					&_qcrypto_ablk_cipher_algos[i]);
		if (IS_ERR(q_alg)) {
			rc = PTR_ERR(q_alg);
			goto err;
		}
		if (cp->ce_support.use_sw_aes_cbc_ecb_ctr_algo) {
			rc = _qcrypto_prefix_alg_cra_name(
					q_alg->cipher_alg.cra_name,
					strlen(q_alg->cipher_alg.cra_name));
			if (rc) {
				dev_err(&pdev->dev,
					"The algorithm name %s is too long.\n",
					q_alg->cipher_alg.cra_name);
				kfree(q_alg);
				goto err;
			}
		}
		rc = crypto_register_alg(&q_alg->cipher_alg);
		if (rc) {
			dev_err(&pdev->dev, "%s alg registration failed\n",
					q_alg->cipher_alg.cra_driver_name);
			kzfree(q_alg);
		} else {
			list_add_tail(&q_alg->entry, &cp->alg_list);
			dev_info(&pdev->dev, "%s\n",
					q_alg->cipher_alg.cra_driver_name);
		}
	}

	/* register crypto cipher algorithms the device supports */
	if (cp->ce_support.aes_xts) {
		struct qcrypto_alg *q_alg;

		q_alg = _qcrypto_cipher_alg_alloc(cp,
					&_qcrypto_ablk_cipher_xts_algo);
		if (IS_ERR(q_alg)) {
			rc = PTR_ERR(q_alg);
			goto err;
		}
		if (cp->ce_support.use_sw_aes_xts_algo) {
			rc = _qcrypto_prefix_alg_cra_name(
					q_alg->cipher_alg.cra_name,
					strlen(q_alg->cipher_alg.cra_name));
			if (rc) {
				dev_err(&pdev->dev,
					"The algorithm name %s is too long.\n",
					q_alg->cipher_alg.cra_name);
				kfree(q_alg);
				goto err;
			}
		}
		rc = crypto_register_alg(&q_alg->cipher_alg);
		if (rc) {
			dev_err(&pdev->dev, "%s alg registration failed\n",
					q_alg->cipher_alg.cra_driver_name);
			kzfree(q_alg);
		} else {
			list_add_tail(&q_alg->entry, &cp->alg_list);
			dev_info(&pdev->dev, "%s\n",
					q_alg->cipher_alg.cra_driver_name);
		}
	}

	/*
	 * Register crypto hash (sha1 and sha256) algorithms the
	 * device supports
	 */
	for (i = 0; i < ARRAY_SIZE(_qcrypto_ahash_algos); i++) {
		struct qcrypto_alg *q_alg = NULL;

		q_alg = _qcrypto_sha_alg_alloc(cp, &_qcrypto_ahash_algos[i]);

		if (IS_ERR(q_alg)) {
			rc = PTR_ERR(q_alg);
			goto err;
		}
		if (cp->ce_support.use_sw_ahash_algo) {
			rc = _qcrypto_prefix_alg_cra_name(
				q_alg->sha_alg.halg.base.cra_name,
				strlen(q_alg->sha_alg.halg.base.cra_name));
			if (rc) {
				dev_err(&pdev->dev,
					"The algorithm name %s is too long.\n",
					q_alg->sha_alg.halg.base.cra_name);
				kfree(q_alg);
				goto err;
			}
		}
		rc = crypto_register_ahash(&q_alg->sha_alg);
		if (rc) {
			dev_err(&pdev->dev, "%s alg registration failed\n",
				q_alg->sha_alg.halg.base.cra_driver_name);
			kzfree(q_alg);
		} else {
			list_add_tail(&q_alg->entry, &cp->alg_list);
			dev_info(&pdev->dev, "%s\n",
				q_alg->sha_alg.halg.base.cra_driver_name);
		}
	}

	/* register crypto aead (hmac-sha1) algorithms the device supports */
	if (cp->ce_support.sha1_hmac_20 || cp->ce_support.sha1_hmac
		|| cp->ce_support.sha_hmac) {
		for (i = 0; i < ARRAY_SIZE(_qcrypto_aead_sha1_hmac_algos);
									i++) {
			struct qcrypto_alg *q_alg;

			q_alg = _qcrypto_cipher_alg_alloc(cp,
					&_qcrypto_aead_sha1_hmac_algos[i]);
			if (IS_ERR(q_alg)) {
				rc = PTR_ERR(q_alg);
				goto err;
			}
			if (cp->ce_support.use_sw_aead_algo) {
				rc = _qcrypto_prefix_alg_cra_name(
					q_alg->cipher_alg.cra_name,
					strlen(q_alg->cipher_alg.cra_name));
				if (rc) {
					dev_err(&pdev->dev,
						"The algorithm name %s is too long.\n",
						q_alg->cipher_alg.cra_name);
					kfree(q_alg);
					goto err;
				}
			}
			rc = crypto_register_alg(&q_alg->cipher_alg);
			if (rc) {
				dev_err(&pdev->dev,
					"%s alg registration failed\n",
					q_alg->cipher_alg.cra_driver_name);
				kfree(q_alg);
			} else {
				list_add_tail(&q_alg->entry, &cp->alg_list);
				dev_info(&pdev->dev, "%s\n",
					q_alg->cipher_alg.cra_driver_name);
			}
		}
	}

	/* register crypto aead (hmac-sha256) algorithms the device supports */
	if (cp->ce_support.sha_hmac) {
		for (i = 0; i < ARRAY_SIZE(_qcrypto_aead_sha256_hmac_algos);
									i++) {
			struct qcrypto_alg *q_alg;

			q_alg = _qcrypto_cipher_alg_alloc(cp,
					&_qcrypto_aead_sha256_hmac_algos[i]);
			if (IS_ERR(q_alg)) {
				rc = PTR_ERR(q_alg);
				goto err;
			}
			if (cp->ce_support.use_sw_aead_algo) {
				rc = _qcrypto_prefix_alg_cra_name(
					q_alg->cipher_alg.cra_name,
					strlen(q_alg->cipher_alg.cra_name));
				if (rc) {
					dev_err(&pdev->dev,
						"The algorithm name %s is too long.\n",
						q_alg->cipher_alg.cra_name);
					kfree(q_alg);
					goto err;
				}
			}
			rc = crypto_register_alg(&q_alg->cipher_alg);
			if (rc) {
				dev_err(&pdev->dev,
					"%s alg registration failed\n",
					q_alg->cipher_alg.cra_driver_name);
				kfree(q_alg);
			} else {
				list_add_tail(&q_alg->entry, &cp->alg_list);
				dev_info(&pdev->dev, "%s\n",
					q_alg->cipher_alg.cra_driver_name);
			}
		}
	}

	if ((cp->ce_support.sha_hmac) || (cp->platform_support.sha_hmac)) {
		/* register crypto hmac algorithms the device supports */
		for (i = 0; i < ARRAY_SIZE(_qcrypto_sha_hmac_algos); i++) {
			struct qcrypto_alg *q_alg = NULL;

			q_alg = _qcrypto_sha_alg_alloc(cp,
						&_qcrypto_sha_hmac_algos[i]);

			if (IS_ERR(q_alg)) {
				rc = PTR_ERR(q_alg);
				goto err;
			}
			if (cp->ce_support.use_sw_hmac_algo) {
				rc = _qcrypto_prefix_alg_cra_name(
					q_alg->sha_alg.halg.base.cra_name,
					strlen(
					q_alg->sha_alg.halg.base.cra_name));
				if (rc) {
					dev_err(&pdev->dev,
					     "The algorithm name %s is too long.\n",
					     q_alg->sha_alg.halg.base.cra_name);
					kfree(q_alg);
					goto err;
				}
			}
			rc = crypto_register_ahash(&q_alg->sha_alg);
			if (rc) {
				dev_err(&pdev->dev,
				"%s alg registration failed\n",
				q_alg->sha_alg.halg.base.cra_driver_name);
				kzfree(q_alg);
			} else {
				list_add_tail(&q_alg->entry, &cp->alg_list);
				dev_info(&pdev->dev, "%s\n",
				q_alg->sha_alg.halg.base.cra_driver_name);
			}
		}
	}
	/*
	 * Register crypto cipher (aes-ccm) algorithms the
	 * device supports
	 */
	if (cp->ce_support.aes_ccm) {
		struct qcrypto_alg *q_alg;

		q_alg = _qcrypto_cipher_alg_alloc(cp, &_qcrypto_aead_ccm_algo);
		if (IS_ERR(q_alg)) {
			rc = PTR_ERR(q_alg);
			goto err;
		}
		if (cp->ce_support.use_sw_aes_ccm_algo) {
			rc = _qcrypto_prefix_alg_cra_name(
					q_alg->cipher_alg.cra_name,
					strlen(q_alg->cipher_alg.cra_name));
			if (rc) {
				dev_err(&pdev->dev,
						"The algorithm name %s is too long.\n",
						q_alg->cipher_alg.cra_name);
				kfree(q_alg);
				goto err;
			}
		}
		rc = crypto_register_alg(&q_alg->cipher_alg);
		if (rc) {
			dev_err(&pdev->dev, "%s alg registration failed\n",
					q_alg->cipher_alg.cra_driver_name);
			kzfree(q_alg);
		} else {
			list_add_tail(&q_alg->entry, &cp->alg_list);
			dev_info(&pdev->dev, "%s\n",
					q_alg->cipher_alg.cra_driver_name);
		}

		q_alg = _qcrypto_cipher_alg_alloc(cp,
					&_qcrypto_aead_rfc4309_ccm_algo);
		if (IS_ERR(q_alg)) {
			rc = PTR_ERR(q_alg);
			goto err;
		}

		if (cp->ce_support.use_sw_aes_ccm_algo) {
			rc = _qcrypto_prefix_alg_cra_name(
					q_alg->cipher_alg.cra_name,
					strlen(q_alg->cipher_alg.cra_name));
			if (rc) {
				dev_err(&pdev->dev,
						"The algorithm name %s is too long.\n",
						q_alg->cipher_alg.cra_name);
				kfree(q_alg);
				goto err;
			}
		}
		rc = crypto_register_alg(&q_alg->cipher_alg);
		if (rc) {
			dev_err(&pdev->dev, "%s alg registration failed\n",
					q_alg->cipher_alg.cra_driver_name);
			kfree(q_alg);
		} else {
			list_add_tail(&q_alg->entry, &cp->alg_list);
			dev_info(&pdev->dev, "%s\n",
					q_alg->cipher_alg.cra_driver_name);
		}
	}

	mutex_unlock(&cp->engine_lock);

fips_selftest:
	/*
	* FIPS140-2 Known Answer Tests :
	* IN case of any failure, do not Init the module
	*/
	is_fips_qcrypto_tests_done = false;

	if (g_fips140_status != FIPS140_STATUS_NA) {

		_qcrypto_prefix_alg_cra_name(prefix, 0);
		_qcrypto_fips_selftest_d(&selftest_d, &cp->ce_support, prefix);
		if (_fips_qcrypto_sha_selftest(&selftest_d) ||
			_fips_qcrypto_cipher_selftest(&selftest_d) ||
			_fips_qcrypto_aead_selftest(&selftest_d)) {
			pr_err("qcrypto: FIPS140-2 Known Answer Tests : Failed\n");
			panic("SYSTEM CAN NOT BOOT!!!");
			rc = -1;
			goto err;
		} else
			pr_info("qcrypto: FIPS140-2 Known Answer Tests: Successful\n");
		if (g_fips140_status != FIPS140_STATUS_PASS)
			g_fips140_status = FIPS140_STATUS_PASS_CRYPTO;

	} else
		pr_info("qcrypto: FIPS140-2 Known Answer Tests: Skipped\n");

	is_fips_qcrypto_tests_done = true;

	return 0;
err:
	_qcrypto_remove_engine(pengine);
	mutex_unlock(&cp->engine_lock);
	if (pengine->qce)
		qce_close(pengine->qce);
	if (pqcrypto_req_control)
		kzfree(pqcrypto_req_control);
	kzfree(pengine);
	return rc;
};

static int _qcrypto_engine_in_use(struct crypto_engine *pengine)
{
	struct crypto_priv *cp = pengine->pcp;

	if ((pengine->req_count > 0) || cp->req_queue.qlen ||
	    pengine->areq_queue.qlen || pengine->req_queue.qlen)
		return 1;
	return 0;
}

static void _qcrypto_do_suspending(struct crypto_engine *pengine)
{
	struct crypto_priv *cp = pengine->pcp;

	if (cp->platform_support.bus_scale_table == NULL)
		return;
	del_timer_sync(&pengine->bw_reaper_timer);
	qcrypto_ce_set_bus(pengine, false);
}

static int  _qcrypto_suspend(struct platform_device *pdev, pm_message_t state)
{
	int ret = 0;
	struct crypto_engine *pengine;
	struct crypto_poll_ctl *ctl;

	pengine = platform_get_drvdata(pdev);
	if (!pengine)
		return -EINVAL;

	/*
	 * Check if this platform supports clock management in suspend/resume
	 * If not, just simply return 0.
	 */
	if (!pengine->pcp->ce_support.clk_mgmt_sus_res)
		return 0;

	ctl = pengine->pctl;
	spin_lock_bh(&ctl->lock);
	switch (pengine->bw_state) {
	case BUS_NO_BANDWIDTH:
		if (__qcrypto_engine_req_queue_empty(pengine))
			pengine->bw_state = BUS_SUSPENDED;
		else
			ret = -EBUSY;
		break;
	case BUS_HAS_BANDWIDTH:
		if (_qcrypto_engine_in_use(pengine)) {
			ret = -EBUSY;
		} else {
			pengine->bw_state = BUS_SUSPENDING;
			spin_unlock_bh(&ctl->lock);
			_qcrypto_do_suspending(pengine);
			spin_lock_bh(&ctl->lock);
			pengine->bw_state = BUS_SUSPENDED;
		}
		break;
	case BUS_BANDWIDTH_RELEASING:
	case BUS_BANDWIDTH_ALLOCATING:
	case BUS_SUSPENDED:
	case BUS_SUSPENDING:
	default:
			ret = -EBUSY;
			break;
	}

	spin_unlock_bh(&ctl->lock);
	if (ret)
		return ret;
	else {
		if (qce_pm_table.suspend)
			qce_pm_table.suspend(pengine->qce);
		return 0;
	}
}

static int  _qcrypto_resume(struct platform_device *pdev)
{
	struct crypto_engine *pengine;
	struct crypto_poll_ctl *ctl;
	int ret = 0;

	pengine = platform_get_drvdata(pdev);

	if (!pengine)
		return -EINVAL;

	if (!pengine->pcp->ce_support.clk_mgmt_sus_res)
		return 0;

	ctl = pengine->pctl;
	spin_lock_bh(&ctl->lock);
	if (pengine->bw_state == BUS_SUSPENDED) {
		spin_unlock_bh(&ctl->lock);
		if (qce_pm_table.resume)
			qce_pm_table.resume(pengine->qce);

		spin_lock_bh(&ctl->lock);
		pengine->bw_state = BUS_NO_BANDWIDTH;
		pengine->active_seq++;
		pengine->check_flag = false;
		qcrypto_poll_ctl_sched(ctl);
	} else
		ret = -EBUSY;

	spin_unlock_bh(&ctl->lock);
	return ret;
}

static struct of_device_id qcrypto_match[] = {
	{	.compatible = "qcom,qcrypto",
	},
	{}
};

static struct platform_driver _qualcomm_crypto = {
	.probe          = _qcrypto_probe,
	.remove         = _qcrypto_remove,
	.suspend        = _qcrypto_suspend,
	.resume         = _qcrypto_resume,
	.driver         = {
		.owner  = THIS_MODULE,
		.name   = "qcrypto",
		.of_match_table = qcrypto_match,
	},
};

static int _debug_qcrypto;

static int _debug_stats_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t _debug_stats_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	int rc = -EINVAL;
	int qcrypto = *((int *) file->private_data);
	int len;

	len = _disp_stats(qcrypto);

	if (len <= count)
		rc = simple_read_from_buffer((void __user *) buf, len,
			ppos, (void *) _debug_read_buf, len);
	return rc;
}

static ssize_t _debug_stats_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	unsigned long flags;
	struct crypto_priv *cp = &qcrypto_dev;
	struct crypto_engine *pe;

	memset((char *)&_qcrypto_stat, 0, sizeof(struct crypto_stat));
	spin_lock_irqsave(&cp->lock, flags);
	list_for_each_entry(pe, &cp->engine_list, elist) {
		pe->total_req = 0;
		pe->err_req = 0;
		qce_clear_driver_stats(pe->qce);
		pe->max_req_used = 0;
	}
	cp->max_qlen = 0;
	cp->resp_start = 0;
	cp->resp_stop = 0;
	cp->no_avail = 0;
	cp->max_resp_qlen = 0;
	cp->max_reorder_cnt = 0;
	cp->queue_complete_work = 0;
	cp->req_drop_cnt = 0;
	spin_unlock_irqrestore(&cp->lock, flags);
	return count;
}

static const struct file_operations _debug_stats_ops = {
	.open =         _debug_stats_open,
	.read =         _debug_stats_read,
	.write =        _debug_stats_write,
};

static ssize_t _debug_poll_intval_read(struct file *file, char __user *buf,
			size_t count, loff_t *ppos)
{
	int rc = -EINVAL;
	struct crypto_priv *cp = &qcrypto_dev;
	struct crypto_poll_ctl *ctl;
	char s[512];
	int len = 0;


	list_for_each_entry(ctl, &cp->poll_ctl_list, list) {
		len += scnprintf(s + len, sizeof(s) - len - 1,
				 "   CPU%d: %lluus\n",
				 ctl->cpu, ktime_to_us(ctl->intval));
	}

	if (len <= count)
		rc = simple_read_from_buffer((void __user *) buf, len,
			ppos, s, len);
	return rc;
}

static ssize_t _debug_poll_intval_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	struct crypto_priv *cp = &qcrypto_dev;
	struct crypto_poll_ctl *ctl;
	unsigned int intval;

	if (kstrtouint_from_user(buf, count, 0, &intval))
		return -EINVAL;

	if (!intval || intval >= MAX_POLL_INTVAL)
		return -EINVAL;

	list_for_each_entry(ctl, &cp->poll_ctl_list, list)
		ctl->intval = ns_to_ktime(MICRO_TO_NS(intval));

	return count;
}

static const struct file_operations _debug_poll_intval_ops = {
	.open =         simple_open,
	.read =         _debug_poll_intval_read,
	.write =        _debug_poll_intval_write,
};

static int _qcrypto_debug_init(void)
{
	int rc;
	char name[DEBUG_MAX_FNAME];
	struct dentry *dent;

	_debug_dent = debugfs_create_dir("qcrypto", NULL);
	if (IS_ERR(_debug_dent)) {
		pr_err("qcrypto debugfs_create_dir fail, error %ld\n",
				PTR_ERR(_debug_dent));
		return PTR_ERR(_debug_dent);
	}

	snprintf(name, DEBUG_MAX_FNAME-1, "stats-%d", 1);
	_debug_qcrypto = 0;
	dent = debugfs_create_file(name, 0644, _debug_dent,
				&_debug_qcrypto, &_debug_stats_ops);
	if (dent == NULL) {
		pr_err("qcrypto debugfs_create_file fail, error %ld\n",
				PTR_ERR(dent));
		rc = PTR_ERR(dent);
		goto err;
	}
	dent = debugfs_create_file("poll-interval", 0644, _debug_dent,
				&_debug_qcrypto, &_debug_poll_intval_ops);
	if (dent == NULL) {
		pr_err("qcrypto debugfs_create_file fail, error %ld\n",
				PTR_ERR(dent));
		rc = PTR_ERR(dent);
		goto err;
	}
	return 0;
err:
	debugfs_remove_recursive(_debug_dent);
	return rc;
}

static int __init _qcrypto_init(void)
{
	int rc;
	struct crypto_priv *pcp = &qcrypto_dev;

	rc = _qcrypto_debug_init();
	if (rc)
		return rc;
	INIT_LIST_HEAD(&pcp->alg_list);
	INIT_LIST_HEAD(&pcp->engine_list);
	INIT_LIST_HEAD(&pcp->poll_ctl_list);
	init_llist_head(&pcp->ordered_resp_list);
	spin_lock_init(&pcp->lock);
	mutex_init(&pcp->engine_lock);
	tasklet_init(&pcp->resp_wq, seq_response, (unsigned long)pcp);
	pcp->total_units = 0;
	pcp->platform_support.bus_scale_table = NULL;
	pcp->next_engine = NULL;
	pcp->ce_req_proc_sts = IN_PROGRESS;
	crypto_init_queue(&pcp->req_queue, MSM_QCRYPTO_REQ_QUEUE_LENGTH);
	return platform_driver_register(&_qualcomm_crypto);
}

static void __exit _qcrypto_exit(void)
{
	pr_debug("%s Unregister QCRYPTO\n", __func__);
	debugfs_remove_recursive(_debug_dent);
	platform_driver_unregister(&_qualcomm_crypto);
}

module_init(_qcrypto_init);
module_exit(_qcrypto_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Crypto driver");
