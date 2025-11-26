// move_page_probe_any.c
// kprobe: migrate the first file-backed page seen by mark_page_accessed()
// using migrate_folio(), on modern folio-based kernels.

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/migrate.h>
#include <linux/swap.h>
#include <linux/kprobes.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/highmem.h>
#include <linux/gfp.h>
#include <linux/pagemap.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/mm_types.h>   /* struct folio, page_folio(), etc */
#include <linux/mm.h>         // folio_test_*, folio_mapping(), migrate_folio
#include <linux/mm_inline.h>  // inline LRU/anon helpers on folios
#include <linux/mm_types.h>   // struct folio, page_folio()
#include <linux/pagemap.h>    // folio_mapcount, folio_index, mapping


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Farhan Tanvir Utshaw");
MODULE_DESCRIPTION("kprobe: migrate the first file-backed page seen by mark_page_accessed()");
MODULE_VERSION("1.1");

/*
 * Destination NUMA node:
 *   insmod move_page_probe_any.ko target_node=1
 */
static int target_node = 1;
module_param(target_node, int, 0444);
MODULE_PARM_DESC(target_node, "Destination NUMA node for migration");

/* Global: did we already move one page? */
static atomic_t did_move = ATOMIC_INIT(0);
static struct kprobe kp;

/* x86_64 first arg extraction for mark_page_accessed(struct page *page) */
static inline struct page *get_page_arg(struct pt_regs *regs)
{
#if defined(CONFIG_X86_64)
    return (struct page *)regs->di;
#else
# error "Unsupported arch in this module"
#endif
}

static void dump_page_info(struct page *page)
{
    struct folio *f = page_folio(page);

    pr_info("page=%p nid=%d pfn=%lu refcount=%d mapcount=%d "
            "folio_nid=%d\n",
            page,
            page_to_nid(page),
            page_to_pfn(page),
            page_ref_count(page),
            folio_mapcount(f),
            folio_nid(f));
}

/*
 * Attempt to migrate a single file-backed page to target_node.
 *
 * NOTE: This is *not* production-safe: kprobe pre_handlers must not sleep,
 * but migrate_folio() may block. This is strictly for experiments.
 */
static void try_migrate_page(struct page *page)
{
    struct folio *src;
    struct folio *dst;
    struct address_space *mapping;
    struct page *newpage = NULL;
    int rc;

    if (!page || atomic_read(&did_move))
        return;



struct folio *f = page_folio(page);

/* LRU */
if (!folio_test_lru(f)) {
    pr_debug("Skipping %p: not on LRU\n", page);
    return;
}

/* Unevictable */
if (folio_test_unevictable(f)) {
    pr_debug("Skipping %p: unevictable\n", page);
    return;
}

/* Anonymous */
if (folio_test_anon(f)) {
    pr_debug("Skipping %p: anonymous\n", page);
    return;
}

/* Mapped? */
if (folio_mapcount(f) != 0) {
    pr_debug("Skipping %p: mapped into userspace? mapcount=%d\n",
             page, folio_mapcount(f));
    return;
}



    src = page_folio(page);
    mapping = folio_mapping(src);
    if (!mapping || !mapping->host) {
        pr_debug("Skipping %p: no mapping/host (not regular file-backed)\n",
                 page);
        return;
    }

    /* Already on target node? */
    if (folio_nid(src) == target_node) {
        pr_debug("Skipping %p: already on nid=%d\n", page, target_node);
        return;
    }

    dump_page_info(page);
    pr_info("Candidate folio %p (nid=%d) mapping=%p inode=%lu -> target_node=%d\n",
            src, folio_nid(src), mapping, mapping->host->i_ino, target_node);

    /* Allocate destination page on target node. */
    newpage = alloc_pages_node(target_node,
                               GFP_HIGHUSER_MOVABLE | __GFP_MOVABLE,
                               0);
    if (!newpage) {
        pr_info("alloc_pages_node(node=%d) failed\n", target_node);
        return;
    }
    dst = page_folio(newpage);

    /*
     * Folios must be locked around migrate_folio().
     * We trylock src to avoid blocking too hard in pre_handler.
     */
    if (!folio_trylock(src)) {
        pr_info("Could not lock src folio %p, aborting this attempt\n", src);
        goto out_free;
    }
    folio_lock(dst);

    pr_info("Attempting migrate_folio(): src_nid=%d -> dst_nid=%d\n",
            folio_nid(src), target_node);

    rc = migrate_folio(mapping, dst, src, MIGRATE_SYNC);
    if (rc == MIGRATEPAGE_SUCCESS) {   /* MIGRATEPAGE_SUCCESS == 0 */
        pr_info("migrate_folio() SUCCESS: old_nid=%d new_nid=%d new_pfn=%lu inode=%lu\n",
                folio_nid(src), folio_nid(dst),
                page_to_pfn(&dst->page),
                mapping->host->i_ino);

        atomic_set(&did_move, 1);

        folio_unlock(dst);
        folio_unlock(src);

        /* We’ve done our one migration; stop probing. */
        unregister_kprobe(&kp);
        return;
    }

    pr_info("migrate_folio() FAILED rc=%d; cleaning up\n", rc);

    folio_unlock(dst);
    folio_unlock(src);

out_free:
    if (newpage)
        __free_pages(newpage, 0);
}

/* kprobe pre handler */
static int my_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
    struct page *page;

    if (atomic_read(&did_move))
        return 0;

    page = get_page_arg(regs);
    if (!page)
        return 0;

    try_migrate_page(page);
    return 0;
}

/* module init/exit */
static int __init move_single_page_init(void)
{
    int ret;

    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = "mark_page_accessed";
    kp.pre_handler = my_pre_handler;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("register_kprobe failed: %d\n", ret);
        return ret;
    }

    pr_info("kprobe registered on %s; target_node=%d\n",
            kp.symbol_name, target_node);
    return 0;
}

static void __exit move_single_page_exit(void)
{
    if (!atomic_read(&did_move))
        unregister_kprobe(&kp);
    pr_info("module exit; did_move=%d\n", atomic_read(&did_move));
}

module_init(move_single_page_init);
module_exit(move_single_page_exit);

