#define	a0calloc JEMALLOC_N(a0calloc)
#define	a0free JEMALLOC_N(a0free)
#define	a0malloc JEMALLOC_N(a0malloc)
#define	arena_alloc_junk_small JEMALLOC_N(arena_alloc_junk_small)
#define	arena_bin_index JEMALLOC_N(arena_bin_index)
#define	arena_bin_info JEMALLOC_N(arena_bin_info)
#define	arena_boot JEMALLOC_N(arena_boot)
#define	arena_dalloc JEMALLOC_N(arena_dalloc)
#define	arena_dalloc_bin JEMALLOC_N(arena_dalloc_bin)
#define	arena_dalloc_junk_small JEMALLOC_N(arena_dalloc_junk_small)
#define	arena_dalloc_large JEMALLOC_N(arena_dalloc_large)
#define	arena_malloc JEMALLOC_N(arena_malloc)
#define	arena_malloc_large JEMALLOC_N(arena_malloc_large)
#define	arena_malloc_small JEMALLOC_N(arena_malloc_small)
#define	arena_maxclass JEMALLOC_N(arena_maxclass)
#define	arena_new JEMALLOC_N(arena_new)
#define	arena_palloc JEMALLOC_N(arena_palloc)
#define	arena_postfork_child JEMALLOC_N(arena_postfork_child)
#define	arena_postfork_parent JEMALLOC_N(arena_postfork_parent)
#define	arena_prefork JEMALLOC_N(arena_prefork)
#define	arena_prof_accum JEMALLOC_N(arena_prof_accum)
#define	arena_prof_ctx_get JEMALLOC_N(arena_prof_ctx_get)
#define	arena_prof_ctx_set JEMALLOC_N(arena_prof_ctx_set)
#define	arena_prof_promoted JEMALLOC_N(arena_prof_promoted)
#define	arena_purge_all JEMALLOC_N(arena_purge_all)
#define	arena_ralloc JEMALLOC_N(arena_ralloc)
#define	arena_ralloc_no_move JEMALLOC_N(arena_ralloc_no_move)
#define	arena_run_regind JEMALLOC_N(arena_run_regind)
#define	arena_salloc JEMALLOC_N(arena_salloc)
#define	arena_stats_merge JEMALLOC_N(arena_stats_merge)
#define	arena_tcache_fill_small JEMALLOC_N(arena_tcache_fill_small)
#define	arenas JEMALLOC_N(arenas)
#define	arenas_bin_i_index JEMALLOC_N(arenas_bin_i_index)
#define	arenas_booted JEMALLOC_N(arenas_booted)
#define	arenas_cleanup JEMALLOC_N(arenas_cleanup)
#define	arenas_extend JEMALLOC_N(arenas_extend)
#define	arenas_initialized JEMALLOC_N(arenas_initialized)
#define	arenas_lock JEMALLOC_N(arenas_lock)
#define	arenas_lrun_i_index JEMALLOC_N(arenas_lrun_i_index)
#define	arenas_tls JEMALLOC_N(arenas_tls)
#define	arenas_tsd_boot JEMALLOC_N(arenas_tsd_boot)
#define	arenas_tsd_cleanup_wrapper JEMALLOC_N(arenas_tsd_cleanup_wrapper)
#define	arenas_tsd_get JEMALLOC_N(arenas_tsd_get)
#define	arenas_tsd_set JEMALLOC_N(arenas_tsd_set)
#define	atomic_add_u JEMALLOC_N(atomic_add_u)
#define	atomic_add_uint32 JEMALLOC_N(atomic_add_uint32)
#define	atomic_add_uint64 JEMALLOC_N(atomic_add_uint64)
#define	atomic_add_z JEMALLOC_N(atomic_add_z)
#define	atomic_sub_u JEMALLOC_N(atomic_sub_u)
#define	atomic_sub_uint32 JEMALLOC_N(atomic_sub_uint32)
#define	atomic_sub_uint64 JEMALLOC_N(atomic_sub_uint64)
#define	atomic_sub_z JEMALLOC_N(atomic_sub_z)
#define	base_alloc JEMALLOC_N(base_alloc)
#define	base_boot JEMALLOC_N(base_boot)
#define	base_calloc JEMALLOC_N(base_calloc)
#define	base_node_alloc JEMALLOC_N(base_node_alloc)
#define	base_node_dealloc JEMALLOC_N(base_node_dealloc)
#define	base_postfork_child JEMALLOC_N(base_postfork_child)
#define	base_postfork_parent JEMALLOC_N(base_postfork_parent)
#define	base_prefork JEMALLOC_N(base_prefork)
#define	bitmap_full JEMALLOC_N(bitmap_full)
#define	bitmap_get JEMALLOC_N(bitmap_get)
#define	bitmap_info_init JEMALLOC_N(bitmap_info_init)
#define	bitmap_info_ngroups JEMALLOC_N(bitmap_info_ngroups)
#define	bitmap_init JEMALLOC_N(bitmap_init)
#define	bitmap_set JEMALLOC_N(bitmap_set)
#define	bitmap_sfu JEMALLOC_N(bitmap_sfu)
#define	bitmap_size JEMALLOC_N(bitmap_size)
#define	bitmap_unset JEMALLOC_N(bitmap_unset)
#define	bt_init JEMALLOC_N(bt_init)
#define	buferror JEMALLOC_N(buferror)
#define	choose_arena JEMALLOC_N(choose_arena)
#define	choose_arena_hard JEMALLOC_N(choose_arena_hard)
#define	chunk_alloc JEMALLOC_N(chunk_alloc)
#define	chunk_alloc_dss JEMALLOC_N(chunk_alloc_dss)
#define	chunk_alloc_mmap JEMALLOC_N(chunk_alloc_mmap)
#define	chunk_boot JEMALLOC_N(chunk_boot)
#define	chunk_dealloc JEMALLOC_N(chunk_dealloc)
#define	chunk_dealloc_mmap JEMALLOC_N(chunk_dealloc_mmap)
#define	chunk_dss_boot JEMALLOC_N(chunk_dss_boot)
#define	chunk_dss_postfork_child JEMALLOC_N(chunk_dss_postfork_child)
#define	chunk_dss_postfork_parent JEMALLOC_N(chunk_dss_postfork_parent)
#define	chunk_dss_prefork JEMALLOC_N(chunk_dss_prefork)
#define	chunk_in_dss JEMALLOC_N(chunk_in_dss)
#define	chunk_npages JEMALLOC_N(chunk_npages)
#define	chunks_mtx JEMALLOC_N(chunks_mtx)
#define	chunks_rtree JEMALLOC_N(chunks_rtree)
#define	chunksize JEMALLOC_N(chunksize)
#define	chunksize_mask JEMALLOC_N(chunksize_mask)
#define	ckh_bucket_search JEMALLOC_N(ckh_bucket_search)
#define	ckh_count JEMALLOC_N(ckh_count)
#define	ckh_delete JEMALLOC_N(ckh_delete)
#define	ckh_evict_reloc_insert JEMALLOC_N(ckh_evict_reloc_insert)
#define	ckh_insert JEMALLOC_N(ckh_insert)
#define	ckh_isearch JEMALLOC_N(ckh_isearch)
#define	ckh_iter JEMALLOC_N(ckh_iter)
#define	ckh_new JEMALLOC_N(ckh_new)
#define	ckh_pointer_hash JEMALLOC_N(ckh_pointer_hash)
#define	ckh_pointer_keycomp JEMALLOC_N(ckh_pointer_keycomp)
#define	ckh_rebuild JEMALLOC_N(ckh_rebuild)
#define	ckh_remove JEMALLOC_N(ckh_remove)
#define	ckh_search JEMALLOC_N(ckh_search)
#define	ckh_string_hash JEMALLOC_N(ckh_string_hash)
#define	ckh_string_keycomp JEMALLOC_N(ckh_string_keycomp)
#define	ckh_try_bucket_insert JEMALLOC_N(ckh_try_bucket_insert)
#define	ckh_try_insert JEMALLOC_N(ckh_try_insert)
#define	ctl_boot JEMALLOC_N(ctl_boot)
#define	ctl_bymib JEMALLOC_N(ctl_bymib)
#define	ctl_byname JEMALLOC_N(ctl_byname)
#define	ctl_nametomib JEMALLOC_N(ctl_nametomib)
#define	extent_tree_ad_first JEMALLOC_N(extent_tree_ad_first)
#define	extent_tree_ad_insert JEMALLOC_N(extent_tree_ad_insert)
#define	extent_tree_ad_iter JEMALLOC_N(extent_tree_ad_iter)
#define	extent_tree_ad_iter_recurse JEMALLOC_N(extent_tree_ad_iter_recurse)
#define	extent_tree_ad_iter_start JEMALLOC_N(extent_tree_ad_iter_start)
#define	extent_tree_ad_last JEMALLOC_N(extent_tree_ad_last)
#define	extent_tree_ad_new JEMALLOC_N(extent_tree_ad_new)
#define	extent_tree_ad_next JEMALLOC_N(extent_tree_ad_next)
#define	extent_tree_ad_nsearch JEMALLOC_N(extent_tree_ad_nsearch)
#define	extent_tree_ad_prev JEMALLOC_N(extent_tree_ad_prev)
#define	extent_tree_ad_psearch JEMALLOC_N(extent_tree_ad_psearch)
#define	extent_tree_ad_remove JEMALLOC_N(extent_tree_ad_remove)
#define	extent_tree_ad_reverse_iter JEMALLOC_N(extent_tree_ad_reverse_iter)
#define	extent_tree_ad_reverse_iter_recurse JEMALLOC_N(extent_tree_ad_reverse_iter_recurse)
#define	extent_tree_ad_reverse_iter_start JEMALLOC_N(extent_tree_ad_reverse_iter_start)
#define	extent_tree_ad_search JEMALLOC_N(extent_tree_ad_search)
#define	extent_tree_szad_first JEMALLOC_N(extent_tree_szad_first)
#define	extent_tree_szad_insert JEMALLOC_N(extent_tree_szad_insert)
#define	extent_tree_szad_iter JEMALLOC_N(extent_tree_szad_iter)
#define	extent_tree_szad_iter_recurse JEMALLOC_N(extent_tree_szad_iter_recurse)
#define	extent_tree_szad_iter_start JEMALLOC_N(extent_tree_szad_iter_start)
#define	extent_tree_szad_last JEMALLOC_N(extent_tree_szad_last)
#define	extent_tree_szad_new JEMALLOC_N(extent_tree_szad_new)
#define	extent_tree_szad_next JEMALLOC_N(extent_tree_szad_next)
#define	extent_tree_szad_nsearch JEMALLOC_N(extent_tree_szad_nsearch)
#define	extent_tree_szad_prev JEMALLOC_N(extent_tree_szad_prev)
#define	extent_tree_szad_psearch JEMALLOC_N(extent_tree_szad_psearch)
#define	extent_tree_szad_remove JEMALLOC_N(extent_tree_szad_remove)
#define	extent_tree_szad_reverse_iter JEMALLOC_N(extent_tree_szad_reverse_iter)
#define	extent_tree_szad_reverse_iter_recurse JEMALLOC_N(extent_tree_szad_reverse_iter_recurse)
#define	extent_tree_szad_reverse_iter_start JEMALLOC_N(extent_tree_szad_reverse_iter_start)
#define	extent_tree_szad_search JEMALLOC_N(extent_tree_szad_search)
#define	hash JEMALLOC_N(hash)
#define	huge_allocated JEMALLOC_N(huge_allocated)
#define	huge_boot JEMALLOC_N(huge_boot)
#define	huge_dalloc JEMALLOC_N(huge_dalloc)
#define	huge_malloc JEMALLOC_N(huge_malloc)
#define	huge_mtx JEMALLOC_N(huge_mtx)
#define	huge_ndalloc JEMALLOC_N(huge_ndalloc)
#define	huge_nmalloc JEMALLOC_N(huge_nmalloc)
#define	huge_palloc JEMALLOC_N(huge_palloc)
#define	huge_postfork_child JEMALLOC_N(huge_postfork_child)
#define	huge_postfork_parent JEMALLOC_N(huge_postfork_parent)
#define	huge_prefork JEMALLOC_N(huge_prefork)
#define	huge_prof_ctx_get JEMALLOC_N(huge_prof_ctx_get)
#define	huge_prof_ctx_set JEMALLOC_N(huge_prof_ctx_set)
#define	huge_ralloc JEMALLOC_N(huge_ralloc)
#define	huge_ralloc_no_move JEMALLOC_N(huge_ralloc_no_move)
#define	huge_salloc JEMALLOC_N(huge_salloc)
#define	iallocm JEMALLOC_N(iallocm)
#define	icalloc JEMALLOC_N(icalloc)
#define	idalloc JEMALLOC_N(idalloc)
#define	imalloc JEMALLOC_N(imalloc)
#define	ipalloc JEMALLOC_N(ipalloc)
#define	iqalloc JEMALLOC_N(iqalloc)
#define	iralloc JEMALLOC_N(iralloc)
#define	isalloc JEMALLOC_N(isalloc)
#define	ivsalloc JEMALLOC_N(ivsalloc)
#define	jemalloc_postfork_child JEMALLOC_N(jemalloc_postfork_child)
#define	jemalloc_postfork_parent JEMALLOC_N(jemalloc_postfork_parent)
#define	jemalloc_prefork JEMALLOC_N(jemalloc_prefork)
#define	malloc_cprintf JEMALLOC_N(malloc_cprintf)
#define	malloc_mutex_init JEMALLOC_N(malloc_mutex_init)
#define	malloc_mutex_lock JEMALLOC_N(malloc_mutex_lock)
#define	malloc_mutex_postfork_child JEMALLOC_N(malloc_mutex_postfork_child)
#define	malloc_mutex_postfork_parent JEMALLOC_N(malloc_mutex_postfork_parent)
#define	malloc_mutex_prefork JEMALLOC_N(malloc_mutex_prefork)
#define	malloc_mutex_unlock JEMALLOC_N(malloc_mutex_unlock)
#define	malloc_printf JEMALLOC_N(malloc_printf)
#define	malloc_snprintf JEMALLOC_N(malloc_snprintf)
#define	malloc_strtoumax JEMALLOC_N(malloc_strtoumax)
#define	malloc_tsd_boot JEMALLOC_N(malloc_tsd_boot)
#define	malloc_tsd_cleanup_register JEMALLOC_N(malloc_tsd_cleanup_register)
#define	malloc_tsd_dalloc JEMALLOC_N(malloc_tsd_dalloc)
#define	malloc_tsd_malloc JEMALLOC_N(malloc_tsd_malloc)
#define	malloc_tsd_no_cleanup JEMALLOC_N(malloc_tsd_no_cleanup)
#define	malloc_vcprintf JEMALLOC_N(malloc_vcprintf)
#define	malloc_vsnprintf JEMALLOC_N(malloc_vsnprintf)
#define	malloc_write JEMALLOC_N(malloc_write)
#define	map_bias JEMALLOC_N(map_bias)
#define	mb_write JEMALLOC_N(mb_write)
#define	mutex_boot JEMALLOC_N(mutex_boot)
#define	narenas JEMALLOC_N(narenas)
#define	ncpus JEMALLOC_N(ncpus)
#define	nhbins JEMALLOC_N(nhbins)
#define	opt_abort JEMALLOC_N(opt_abort)
#define	opt_junk JEMALLOC_N(opt_junk)
#define	opt_lg_chunk JEMALLOC_N(opt_lg_chunk)
#define	opt_lg_dirty_mult JEMALLOC_N(opt_lg_dirty_mult)
#define	opt_lg_prof_interval JEMALLOC_N(opt_lg_prof_interval)
#define	opt_lg_prof_sample JEMALLOC_N(opt_lg_prof_sample)
#define	opt_lg_tcache_max JEMALLOC_N(opt_lg_tcache_max)
#define	opt_narenas JEMALLOC_N(opt_narenas)
#define	opt_prof JEMALLOC_N(opt_prof)
#define	opt_prof_accum JEMALLOC_N(opt_prof_accum)
#define	opt_prof_active JEMALLOC_N(opt_prof_active)
#define	opt_prof_final JEMALLOC_N(opt_prof_final)
#define	opt_prof_gdump JEMALLOC_N(opt_prof_gdump)
#define	opt_prof_leak JEMALLOC_N(opt_prof_leak)
#define	opt_prof_prefix JEMALLOC_N(opt_prof_prefix)
#define	opt_quarantine JEMALLOC_N(opt_quarantine)
#define	opt_redzone JEMALLOC_N(opt_redzone)
#define	opt_stats_print JEMALLOC_N(opt_stats_print)
#define	opt_tcache JEMALLOC_N(opt_tcache)
#define	opt_utrace JEMALLOC_N(opt_utrace)
#define	opt_valgrind JEMALLOC_N(opt_valgrind)
#define	opt_xmalloc JEMALLOC_N(opt_xmalloc)
#define	opt_zero JEMALLOC_N(opt_zero)
#define	p2rz JEMALLOC_N(p2rz)
#define	pages_purge JEMALLOC_N(pages_purge)
#define	pow2_ceil JEMALLOC_N(pow2_ceil)
#define	prof_backtrace JEMALLOC_N(prof_backtrace)
#define	prof_boot0 JEMALLOC_N(prof_boot0)
#define	prof_boot1 JEMALLOC_N(prof_boot1)
#define	prof_boot2 JEMALLOC_N(prof_boot2)
#define	prof_ctx_get JEMALLOC_N(prof_ctx_get)
#define	prof_ctx_set JEMALLOC_N(prof_ctx_set)
#define	prof_free JEMALLOC_N(prof_free)
#define	prof_gdump JEMALLOC_N(prof_gdump)
#define	prof_idump JEMALLOC_N(prof_idump)
#define	prof_interval JEMALLOC_N(prof_interval)
#define	prof_lookup JEMALLOC_N(prof_lookup)
#define	prof_malloc JEMALLOC_N(prof_malloc)
#define	prof_mdump JEMALLOC_N(prof_mdump)
#define	prof_lookup JEMALLOC_N(prof_lookup)
#define	prof_promote JEMALLOC_N(prof_promote)
#define	prof_realloc JEMALLOC_N(prof_realloc)
#define	prof_sample_accum_update JEMALLOC_N(prof_sample_accum_update)
#define	prof_sample_threshold_update JEMALLOC_N(prof_sample_threshold_update)
#define	prof_tdata_booted JEMALLOC_N(prof_tdata_booted)
#define	prof_tdata_cleanup JEMALLOC_N(prof_tdata_cleanup)
#define	prof_tdata_init JEMALLOC_N(prof_tdata_init)
#define	prof_tdata_initialized JEMALLOC_N(prof_tdata_initialized)
#define	prof_tdata_tls JEMALLOC_N(prof_tdata_tls)
#define	prof_tdata_tsd_boot JEMALLOC_N(prof_tdata_tsd_boot)
#define	prof_tdata_tsd_cleanup_wrapper JEMALLOC_N(prof_tdata_tsd_cleanup_wrapper)
#define	prof_tdata_tsd_get JEMALLOC_N(prof_tdata_tsd_get)
#define	prof_tdata_tsd_set JEMALLOC_N(prof_tdata_tsd_set)
#define	quarantine JEMALLOC_N(quarantine)
#define	quarantine_boot JEMALLOC_N(quarantine_boot)
#define	quarantine_tsd_boot JEMALLOC_N(quarantine_tsd_boot)
#define	quarantine_tsd_cleanup_wrapper JEMALLOC_N(quarantine_tsd_cleanup_wrapper)
#define	quarantine_tsd_get JEMALLOC_N(quarantine_tsd_get)
#define	quarantine_tsd_set JEMALLOC_N(quarantine_tsd_set)
#define	register_zone JEMALLOC_N(register_zone)
#define	rtree_get JEMALLOC_N(rtree_get)
#define	rtree_get_locked JEMALLOC_N(rtree_get_locked)
#define	rtree_new JEMALLOC_N(rtree_new)
#define	rtree_set JEMALLOC_N(rtree_set)
#define	s2u JEMALLOC_N(s2u)
#define	sa2u JEMALLOC_N(sa2u)
#define	stats_arenas_i_bins_j_index JEMALLOC_N(stats_arenas_i_bins_j_index)
#define	stats_arenas_i_index JEMALLOC_N(stats_arenas_i_index)
#define	stats_arenas_i_lruns_j_index JEMALLOC_N(stats_arenas_i_lruns_j_index)
#define	stats_cactive JEMALLOC_N(stats_cactive)
#define	stats_cactive_add JEMALLOC_N(stats_cactive_add)
#define	stats_cactive_get JEMALLOC_N(stats_cactive_get)
#define	stats_cactive_sub JEMALLOC_N(stats_cactive_sub)
#define	stats_chunks JEMALLOC_N(stats_chunks)
#define	stats_print JEMALLOC_N(stats_print)
#define	tcache_alloc_easy JEMALLOC_N(tcache_alloc_easy)
#define	tcache_alloc_large JEMALLOC_N(tcache_alloc_large)
#define	tcache_alloc_small JEMALLOC_N(tcache_alloc_small)
#define	tcache_alloc_small_hard JEMALLOC_N(tcache_alloc_small_hard)
#define	tcache_arena_associate JEMALLOC_N(tcache_arena_associate)
#define	tcache_arena_dissociate JEMALLOC_N(tcache_arena_dissociate)
#define	tcache_bin_flush_large JEMALLOC_N(tcache_bin_flush_large)
#define	tcache_bin_flush_small JEMALLOC_N(tcache_bin_flush_small)
#define	tcache_bin_info JEMALLOC_N(tcache_bin_info)
#define	tcache_boot0 JEMALLOC_N(tcache_boot0)
#define	tcache_boot1 JEMALLOC_N(tcache_boot1)
#define	tcache_booted JEMALLOC_N(tcache_booted)
#define	tcache_create JEMALLOC_N(tcache_create)
#define	tcache_dalloc_large JEMALLOC_N(tcache_dalloc_large)
#define	tcache_dalloc_small JEMALLOC_N(tcache_dalloc_small)
#define	tcache_destroy JEMALLOC_N(tcache_destroy)
#define	tcache_enabled_booted JEMALLOC_N(tcache_enabled_booted)
#define	tcache_enabled_get JEMALLOC_N(tcache_enabled_get)
#define	tcache_enabled_initialized JEMALLOC_N(tcache_enabled_initialized)
#define	tcache_enabled_set JEMALLOC_N(tcache_enabled_set)
#define	tcache_enabled_tls JEMALLOC_N(tcache_enabled_tls)
#define	tcache_enabled_tsd_boot JEMALLOC_N(tcache_enabled_tsd_boot)
#define	tcache_enabled_tsd_cleanup_wrapper JEMALLOC_N(tcache_enabled_tsd_cleanup_wrapper)
#define	tcache_enabled_tsd_get JEMALLOC_N(tcache_enabled_tsd_get)
#define	tcache_enabled_tsd_set JEMALLOC_N(tcache_enabled_tsd_set)
#define	tcache_event JEMALLOC_N(tcache_event)
#define	tcache_initialized JEMALLOC_N(tcache_initialized)
#define	tcache_flush JEMALLOC_N(tcache_flush)
#define	tcache_get JEMALLOC_N(tcache_get)
#define	tcache_maxclass JEMALLOC_N(tcache_maxclass)
#define	tcache_stats_merge JEMALLOC_N(tcache_stats_merge)
#define	tcache_salloc JEMALLOC_N(tcache_salloc)
#define	tcache_thread_cleanup JEMALLOC_N(tcache_thread_cleanup)
#define	tcache_tls JEMALLOC_N(tcache_tls)
#define	tcache_tsd_boot JEMALLOC_N(tcache_tsd_boot)
#define	tcache_tsd_cleanup_wrapper JEMALLOC_N(tcache_tsd_cleanup_wrapper)
#define	tcache_tsd_get JEMALLOC_N(tcache_tsd_get)
#define	tcache_tsd_set JEMALLOC_N(tcache_tsd_set)
#define	thread_allocated_booted JEMALLOC_N(thread_allocated_booted)
#define	thread_allocated_initialized JEMALLOC_N(thread_allocated_initialized)
#define	thread_allocated_tls JEMALLOC_N(thread_allocated_tls)
#define	thread_allocated_tsd_boot JEMALLOC_N(thread_allocated_tsd_boot)
#define	thread_allocated_tsd_cleanup_wrapper JEMALLOC_N(thread_allocated_tsd_cleanup_wrapper)
#define	thread_allocated_tsd_get JEMALLOC_N(thread_allocated_tsd_get)
#define	thread_allocated_tsd_set JEMALLOC_N(thread_allocated_tsd_set)
#define	u2rz JEMALLOC_N(u2rz)
