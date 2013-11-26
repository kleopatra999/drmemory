/* **********************************************************
 * Copyright (c) 2012-2013 Google, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* windows kernel handle leak checks */

#include "handlecheck.h"
#include "drmemory.h"
#include "callstack.h"
#include "syscall.h"
#include "drsyscall.h"
#include "../wininc/ndk_extypes.h"

#ifndef WINDOWS
# error WINDOWS-only
#endif

#ifdef STATISTICS
static uint num_handle_add;
static uint num_handle_remove;
#endif /* STATISTICS */

/* handle table payload */
typedef struct _handle_callstack_info_t {
    app_loc_t loc;
    packed_callstack_t *pcs;
} handle_callstack_info_t;

typedef struct _open_close_pair_t {
    handle_callstack_info_t open;  /* handle open info */
    handle_callstack_info_t close; /* handle close info */
} open_close_pair_t;

static handle_callstack_info_t other_proc_hci;

#define HANDLE_VERBOSE_1 1
#define HANDLE_VERBOSE_2 2
#define HANDLE_VERBOSE_3 3

/* Hashtable for handle open/close callstack */
#define HSTACK_TABLE_HASH_BITS 8
static hashtable_t handle_stack_table;
#ifdef STATISTICS
uint handle_stack_count;
#endif

/* Hashtable for handle open/close pair, synchronized by
 * explicit hashtable lock.
 */
#define OPEN_CLOSE_TABLE_BITS 8
static hashtable_t open_close_table;
#ifdef STATISTICS
uint open_close_count;
#endif

/* Table of handle entries: [handle, hci]
 * there are multiple handle namespaces: kernel object, gdi object, user object,
 * and they are disjoint, so we have different hashtables for each type.
 */
/* we use handle_stack_table lock for synchronizing all table operations */
#define HANDLE_TABLE_HASH_BITS 6
static hashtable_t kernel_handle_table;
static hashtable_t gdi_handle_table;
static hashtable_t user_handle_table;

/* handle enumeration data structures and routines */
GET_NTDLL(NtQuerySystemInformation, (IN  SYSTEM_INFORMATION_CLASS info_class,
                                     OUT PVOID  info,
                                     IN  ULONG  info_size,
                                     OUT PULONG bytes_received));

#define SYSTEM_HANDLE_INFORMATION_SIZE_INIT 0x10000
#define SYSTEM_HANDLE_INFORMATION_LIST_SIZE(x) \
    (sizeof(SYSTEM_HANDLE_INFORMATION) + sizeof(SYSTEM_HANDLE_ENTRY) * ((x) - 1))

static void
free_system_handle_list(SYSTEM_HANDLE_INFORMATION *list, uint size)
{
    global_free(list, size, HEAPSTAT_MISC);
}

static void
free_process_handle_list(SYSTEM_HANDLE_INFORMATION *list)
{
    free_system_handle_list(list, SYSTEM_HANDLE_INFORMATION_LIST_SIZE(list->Count));
}

#ifdef DEBUG
static void
print_handle_list(SYSTEM_HANDLE_INFORMATION *list)
{
    int i;
    SYSTEM_HANDLE_ENTRY *handle = &list->Handle[0];
    LOG(HANDLE_VERBOSE_1, "Total number of handles: %d\n", list->Count);
    for (i = 0; i < list->Count; i++) {
        LOG(HANDLE_VERBOSE_1,
            "handle["PFX"]: pid="PFX", value="PFX", type="PFX", obj="PFX"\n",
            i, handle[i].OwnerPid, handle[i].HandleValue,
            handle[i].ObjectType, handle[i].ObjectPointer);
    }
}
#endif

/* the caller must free the list by calling free_system_handle_list */
static SYSTEM_HANDLE_INFORMATION *
get_system_handle_list(OUT size_t *size_out)
{
    NTSTATUS res;
    SYSTEM_HANDLE_INFORMATION *list;
    size_t size = SYSTEM_HANDLE_INFORMATION_SIZE_INIT;

    if (size_out == NULL) {
        ASSERT(false, "size_out must not be NULL");
        return NULL;
    }
    do {
        list = (SYSTEM_HANDLE_INFORMATION *) global_alloc(size, HEAPSTAT_MISC);
        ASSERT(list != NULL, "failed to alloc memory for handle list");
        res = NtQuerySystemInformation(SystemHandleInformation, list, size, NULL);
        if (res == STATUS_INFO_LENGTH_MISMATCH) {
            global_free(list, size, HEAPSTAT_MISC);
            list = NULL;
            size *= 2;
        }
    } while (res == STATUS_INFO_LENGTH_MISMATCH);
    if (!NT_SUCCESS(res) && list == NULL) {
        ASSERT(false, "fail to get system handle information");
        list = NULL;
        *size_out = 0;
    } else {
        *size_out = size;
    }
    DOLOG(HANDLE_VERBOSE_3, { print_handle_list(list); });

    return list;
}

/* the caller must free the list by calling free_process_handle_list */
static SYSTEM_HANDLE_INFORMATION *
get_process_handle_list()
{
    SYSTEM_HANDLE_INFORMATION *sys_list, *our_list = NULL;
    SYSTEM_HANDLE_ENTRY *h_our, *h_sys;
    size_t sys_list_size, our_list_size, i, count;
    uint pid = dr_get_process_id();

    sys_list = get_system_handle_list(&sys_list_size);
    ASSERT(sys_list != NULL, "fail to get system handle list");
    h_sys = &sys_list->Handle[0];
    for (i = 0, count = 0; i < sys_list->Count; i++, h_sys++) {
        if (h_sys->OwnerPid == pid)
            count++;
    }

    ASSERT(count != 0, "no handle in current process!");
    our_list_size = SYSTEM_HANDLE_INFORMATION_LIST_SIZE(count);
    our_list = (SYSTEM_HANDLE_INFORMATION *)
        global_alloc(our_list_size, HEAPSTAT_MISC);
    our_list->Count = count;
    h_sys = &sys_list->Handle[0];
    h_our = &our_list->Handle[0];
    for (i = 0; i < count; h_sys++) {
        if (h_sys->OwnerPid == pid) {
            *h_our = *h_sys;
            ASSERT(h_our == &our_list->Handle[0] ||
                   h_our->HandleValue > (h_our - 1)->HandleValue,
                   "handle is not stored in the sorted order");
            h_our++;
            i++;
        }
    }
    free_system_handle_list(sys_list, sys_list_size);
    ASSERT(our_list != NULL, "fail to get process handle list");
    DOLOG(HANDLE_VERBOSE_3, { print_handle_list(our_list); });
    return our_list;
}

#ifdef DEBUG
static void
open_close_pair_print(open_close_pair_t *pair)
{
    LOG(HANDLE_VERBOSE_2, "Handle open/close pair:\n");
    LOG(HANDLE_VERBOSE_2, "Handle open stack:\n");
    DOLOG(HANDLE_VERBOSE_2, {
        packed_callstack_log(pair->open.pcs, INVALID_FILE);
    });
    LOG(HANDLE_VERBOSE_2, "Handle close stack:\n");
    DOLOG(HANDLE_VERBOSE_2, {
        packed_callstack_log(pair->close.pcs, INVALID_FILE);
    });
}
#endif

static void
open_close_pair_free(void *p)
{
    open_close_pair_t *pair = (open_close_pair_t *)p;

    DODEBUG({ open_close_pair_print(pair); });
    packed_callstack_free(pair->open.pcs);
    packed_callstack_free(pair->close.pcs);
    global_free(pair, sizeof(*pair), HEAPSTAT_CALLSTACK);
}

/* Add open/close pair into table, assuming lock is held.
 * Called from handlecheck_delete_handle_post_syscall if the handle
 * is closed successfully.
 */
static void
open_close_pair_add(handle_callstack_info_t *hci/* callstack of creation */,
                    drsys_sysnum_t sysnum,
                    dr_mcontext_t *mc/* context of close */)
{
    open_close_pair_t *pair;
    IF_DEBUG(bool res;)

    if (!options.filter_handle_leaks)
        return;
    pair = (open_close_pair_t *)hashtable_lookup(&open_close_table, hci->pcs);
    /* we only store one close pcs if there are multiple */
    if (pair != NULL)
        return;

    pair = global_alloc(sizeof(*pair), HEAPSTAT_CALLSTACK);
    /* not clone but point to the same pcs */
    pair->open = *hci;
    packed_callstack_add_ref(hci->pcs);
    IF_DEBUG(res =)
        hashtable_add(&open_close_table, (void *)hci->pcs, (void *)pair);
    ASSERT(res, "failed to add to open_close_table");
    ASSERT(packed_callstack_cmp(pair->open.pcs, hci->pcs), "pcs should be the same");
    syscall_to_loc(&pair->close.loc, sysnum, NULL);
    packed_callstack_record(&pair->close.pcs, mc, &pair->close.loc);
    pair->close.pcs = packed_callstack_add_to_table(&handle_stack_table,
                                                    pair->close.pcs
                                                    _IF_STATS(&handle_stack_count));
}

static void
handle_table_lock(void)
{
    /* we use handle_stack_table lock for synchronizing all table operations */
    hashtable_lock(&handle_stack_table);
}

static void
handle_table_unlock(void)
{
    /* we use handle_stack_table lock for synchronizing all table operations */
    hashtable_unlock(&handle_stack_table);
}

void
handle_callstack_free(void *p)
{
    packed_callstack_destroy((packed_callstack_t *)p);
}

static handle_callstack_info_t *
handle_callstack_info_clone(handle_callstack_info_t *src)
{
    handle_callstack_info_t *dst;
    dst = global_alloc(sizeof(*src), HEAPSTAT_CALLSTACK);
    *dst = *src;
    packed_callstack_add_ref(dst->pcs);
    return dst;
}

/* the caller must hold the lock */
static handle_callstack_info_t *
handle_callstack_info_alloc(drsys_sysnum_t sysnum, app_pc pc, dr_mcontext_t *mc)
{
    handle_callstack_info_t *hci;
    hci = global_alloc(sizeof(*hci), HEAPSTAT_CALLSTACK);
    /* assuming pc will never be NULL */
    if (pc == NULL)
        syscall_to_loc(&hci->loc, sysnum, NULL);
    else
        pc_to_loc(&hci->loc, pc);
    packed_callstack_record(&hci->pcs, mc, &hci->loc);
    hci->pcs = packed_callstack_add_to_table(&handle_stack_table, hci->pcs
                                             _IF_STATS(&handle_stack_count));
    return hci;
}

static void
handle_callstack_info_free(handle_callstack_info_t *hci)
{
    uint count;
    count = packed_callstack_free(hci->pcs);
    LOG(4, "%s: freed pcs "PFX" => refcount %d\n", __FUNCTION__, hci->pcs, count);
    global_free(hci, sizeof(*hci), HEAPSTAT_CALLSTACK);
}

/* the caller must hold hashtable lock */
static bool
handlecheck_handle_add(hashtable_t *table, HANDLE handle,
                       handle_callstack_info_t *hci)
{
    void *res;

    STATS_INC(num_handle_add);
    /* We replace the old callstack with new callstack if we see
     * duplicated handle in the table, b/c we might have missed a
     * close, and it's best to take the latest creation of that
     * handle value.
     */
    res = hashtable_add_replace(table, (void *)handle, (void *)hci);
    if (res != NULL) {
        DOLOG(HANDLE_VERBOSE_2, {
            handle_callstack_info_t *old;
            LOG(HANDLE_VERBOSE_2,
                "WARNING: duplicated handle "PFX"\n", handle);
            LOG(HANDLE_VERBOSE_2, "  old callstack:\n");
            old = (handle_callstack_info_t *)res;
            packed_callstack_log(old->pcs, INVALID_FILE);
            LOG(HANDLE_VERBOSE_2, "  new callstack:\n");
            packed_callstack_log(hci->pcs, INVALID_FILE);
        });
        handle_callstack_info_free(res);
        return false;
    }
    return true;
}

/* the caller must hold hashtable lock */
static bool
handlecheck_handle_remove(hashtable_t *table, HANDLE handle,
                          handle_callstack_info_t **hci OUT)
{
    bool res;

    STATS_INC(num_handle_remove);
    if (hci != NULL) {
        handle_callstack_info_t *info;
        info = hashtable_lookup(table, (void *)handle);
        if (info != NULL)
            *hci = handle_callstack_info_clone(info);
        else 
            *hci = NULL;
    }
    res = hashtable_remove(table, (void *)handle);
    return res;
}

#define HANDLECHECK_PRE_MSG_SIZE 0x100

void
handlecheck_report_leak_on_syscall(dr_mcontext_t *mc, drsys_arg_t *arg,
                                   HANDLE proc_handle)
{
    handle_callstack_info_t *hci;
    char msg[HANDLECHECK_PRE_MSG_SIZE];
    const char *name;
    /* Some system call like NtDuplicateObject may leak the handle by passing
     * NULL to the out handle argument, so we assume that the leak on syscall
     * is only caused by the arg PHANDLE being NULL.
     */
    ASSERT(arg->value == (ptr_uint_t)NULL, "syscall arg value is not NULL");
    hci = handle_callstack_info_alloc(arg->sysnum, NULL, mc);
    if (drsys_syscall_name(arg->syscall, &name) != DRMF_SUCCESS)
        name = "<unknown>";
    /* We do not have the leaked handle value, so we report leak without
     * value. We could passing our own ptr to get the value, which may have
     * transparency problems.
     */
    /* i#1380: DuplicateHandle may leak the handle in another process by setting
     * the target process handle to other than the current process. We report the
     * leak regardless of which process the handle belongs to.
     */
    dr_snprintf(msg, BUFFER_SIZE_ELEMENTS(msg),
                "Syscall %s leaks handle with NULL handle pointer in %s process "
                PFX".", name,
                is_current_process(proc_handle) ? "its own" : "another",
                proc_handle);
    report_handle_leak(arg->drcontext, msg, &hci->loc, hci->pcs,
                       NULL /* aux_pcs */, false /* potential */);
    /* add the pair info */
    handle_callstack_info_free(hci);
}

static void
handlecheck_check_open_handle(const char *name,
                              HANDLE handle,
                              handle_callstack_info_t *hci)
{
    open_close_pair_t *pair = NULL;
    bool potential = false;
    uint count;
    char msg[HANDLECHECK_PRE_MSG_SIZE];

    ASSERT(hci != NULL, "handle callstack info must not be NULL");
    count = packed_callstack_refcount(hci->pcs) - 1 /* hashtable refcount */;
    /* i#1373: use heuristics for better handle leak reports */
    if (options.filter_handle_leaks) {
        pair = hashtable_lookup(&open_close_table, (void *)hci->pcs);
        if (pair != NULL) {
            /* Heuristic 1: for each left-open-handle, we check if there is
             * any handle being opened with the same callstack and being closed
             * somewhere. If we see such cases, it means that all handles opened
             * at that site should probably be closed.
             */
            count--; /* pair table refcount */
            if (count <= 1) {
                /* Report it as a potential error if there is only one live handle
                 * from the same call site, as it could be left open on purpose.
                 * Also, we currently want to avoid noise and false positives and
                 * focus on significant leaks.
                 */
                potential = true;
            }
        } else if (count >= options.handle_leak_threshold) {
            /* Heuristic 2: if too many handles created from the same callstack
             * left open, it should be paid attention to, so report it.
             */
        } else {
            /* no heuristic is applied, report it as potential error */
            potential = true;
        }
    }
    dr_snprintf(msg, BUFFER_SIZE_ELEMENTS(msg),
                "%s Handle "PFX" and %d similar handles were opened"
                " but not closed:", name, handle, count-1/*exclude self*/);
    report_handle_leak(dr_get_current_drcontext(), msg, &hci->loc, hci->pcs,
                       (pair == NULL) ? NULL : pair->close.pcs,
                       potential);
}

/* caller must hold handle table lock and open/close table lock */
static void
handlecheck_iterate_handle_table(hashtable_t *table, const char *name)
{
    uint i;
    hash_entry_t *entry;
    for (i = 0; i < HASHTABLE_SIZE(table->table_bits); i++) {
        for (entry = table->table[i]; entry != NULL; entry = entry->next) {
            handlecheck_check_open_handle(name,
                                          (HANDLE)entry->key,
                                          (handle_callstack_info_t *)entry->payload);
        }
    }
}

/* caller must hold handle table lock and open/close table lock */
static void
handlecheck_enumerate_handles(void)
{
    SYSTEM_HANDLE_INFORMATION *list;
    SYSTEM_HANDLE_ENTRY *entry;
    uint i;
    /* i#1380: there could be handles closed by other process, i.e., some
     * handle in the table might be closed already, so we have to query the
     * existing handle list from system.
     */
    LOG(HANDLE_VERBOSE_3, "get process handle list\n");
    list = get_process_handle_list();
    /* iterate the list and report handle leaks */
    entry = &list->Handle[0];
    for (i = 0; i < list->Count; i++, entry++) {
        void *res;
        res = hashtable_lookup(&kernel_handle_table, (void *)entry->HandleValue);
        if (res == NULL) {
            /* There might be handles not in the table, for example,
             * handles created by DR or handles created before we attach.
             */
            ASSERT(hashtable_lookup(&user_handle_table,
                                    (void *)entry->HandleValue) == NULL,
                   "kernel handle in user handle table");
            ASSERT(hashtable_lookup(&gdi_handle_table,
                                    (void *)entry->HandleValue) == NULL,
                   "kernel handle in gdi handle table");
            continue;
        }
        handlecheck_check_open_handle("KERNEL", (HANDLE)entry->HandleValue,
                                      (handle_callstack_info_t *)res);
    }
    free_process_handle_list(list);
}

static void
handlecheck_iterate_handles(void)
{
    void *drcontext = dr_get_current_drcontext();

    handle_table_lock();
    hashtable_lock(&open_close_table);

    /* kernel handles */
    LOG(HANDLE_VERBOSE_3, "enumerating kernel handles");
    handlecheck_enumerate_handles();
    /* user handles */
    LOG(HANDLE_VERBOSE_3, "iterating user handles");
    handlecheck_iterate_handle_table(&user_handle_table, "USER");
    /* gdi handles */
    LOG(HANDLE_VERBOSE_3, "iterating gdi handles");
    handlecheck_iterate_handle_table(&gdi_handle_table, "GDI");

    hashtable_unlock(&open_close_table);
    handle_table_unlock();
}

static inline hashtable_t *
handlecheck_get_handle_table(int type
                             _IF_DEBUG(void *handle)
                             _IF_DEBUG(const char *msg))
{
    hashtable_t *table;
    switch (type) {
    case HANDLE_TYPE_KERNEL:
        LOG(HANDLE_VERBOSE_2, "kernel handle "PFX" is %s\n", handle, msg);
        table = &kernel_handle_table;
        break;
    case HANDLE_TYPE_GDI:
        LOG(HANDLE_VERBOSE_2, "gdi handle "PFX" is %s\n", handle, msg);
        table = &gdi_handle_table;
        break;
    case HANDLE_TYPE_USER:
        LOG(HANDLE_VERBOSE_2, "user handle "PFX" is %s\n", handle, msg);
        table = &user_handle_table;
        break;
    default:
        table = &kernel_handle_table; /* for release build */
        ASSERT(false, "wrong handle type for creation");
    }
    return table;
}

void
handlecheck_init(void)
{
    ASSERT(options.check_handle_leaks, "incorrectly called");
    hashtable_init_ex(&kernel_handle_table, HANDLE_TABLE_HASH_BITS, HASH_INTPTR,
                      false/*!str_dup*/, false/*!synch*/,
                      handle_callstack_info_free, NULL, NULL);
    hashtable_init_ex(&gdi_handle_table, HANDLE_TABLE_HASH_BITS, HASH_INTPTR,
                      false/*!str_dup*/, false/*!synch*/,
                      handle_callstack_info_free, NULL, NULL);
    hashtable_init_ex(&user_handle_table, HANDLE_TABLE_HASH_BITS, HASH_INTPTR,
                      false/*!str_dup*/, false/*!synch*/,
                      handle_callstack_info_free, NULL, NULL);
    hashtable_init_ex(&handle_stack_table, HSTACK_TABLE_HASH_BITS, HASH_CUSTOM,
                      false /*!str_dup*/, false /*!synch*/,
                      handle_callstack_free,
                      (uint (*)(void*)) packed_callstack_hash,
                      (bool (*)(void*, void *)) packed_callstack_cmp);
    hashtable_init_ex(&open_close_table, OPEN_CLOSE_TABLE_BITS, HASH_CUSTOM,
                      false /*!str_dup*/, false /*!synch*/,
                      open_close_pair_free,
                      (uint (*)(void*)) packed_callstack_hash,
                      (bool (*)(void*, void *)) packed_callstack_cmp);
}

void
handlecheck_exit(void)
{
    ASSERT(options.check_handle_leaks, "incorrectly called");
    handlecheck_iterate_handles();
    hashtable_delete_with_stats(&kernel_handle_table,  "Kernel handle table");
    hashtable_delete_with_stats(&gdi_handle_table,     "GDI handle table");
    hashtable_delete_with_stats(&user_handle_table,    "USER handle table");
    hashtable_delete_with_stats(&open_close_table, "Alloc/free pair table");
    hashtable_delete_with_stats(&handle_stack_table,   "Handle stack table");
}

void
handlecheck_create_handle(void *drcontext,
                          HANDLE proc_handle, HANDLE handle, int type,
                          drsys_sysnum_t sysnum, app_pc pc, dr_mcontext_t *mc)
{
    handle_callstack_info_t *hci;
    hashtable_t *table;

    /* i#1380: ignore the handle created in another process */
    if (proc_handle != NT_CURRENT_PROCESS && !is_current_process(proc_handle)) {
        LOG(HANDLE_VERBOSE_2, "Create handle "PFX" in another process "PFX"\n",
            handle, proc_handle);
        return;
    }
    if (handle == INVALID_HANDLE_VALUE || handle == (HANDLE) NULL) {
        ASSERT(false, "syscall succeeds but returns invalid handle value");
        return;
    }
    table = handlecheck_get_handle_table(type
                                         _IF_DEBUG((void *)handle)
                                         _IF_DEBUG("opened"));
    ASSERT(table != NULL, "fail to get handle table");
    handle_table_lock();
    hci = handle_callstack_info_alloc(sysnum, pc, mc);
    DOLOG(HANDLE_VERBOSE_3, { packed_callstack_log(hci->pcs, INVALID_FILE); });
    if (!handlecheck_handle_add(table, handle, hci))
        LOG(HANDLE_VERBOSE_1, "WARNING: fail to add handle "PFX"\n", handle);
    handle_table_unlock();
}

void *
handlecheck_delete_handle(void *drcontext,
                          HANDLE proc_handle, HANDLE handle, int type,
                          drsys_sysnum_t sysnum, app_pc pc, dr_mcontext_t *mc)
{
    hashtable_t *table;
    handle_callstack_info_t *hci;

    if (handle == INVALID_HANDLE_VALUE) {
        LOG(HANDLE_VERBOSE_1, "WARNING: deleting an invalid handle\n");
        return NULL;
    }
    /* i#1380: ignore the handle closed in another process */
    if (proc_handle != NT_CURRENT_PROCESS && !is_current_process(proc_handle)) {
        LOG(HANDLE_VERBOSE_2, "Close handle "PFX" in a different process "PFX"\n",
            handle, proc_handle);
        return (void *)&other_proc_hci;
    }
    table = handlecheck_get_handle_table(type
                                         _IF_DEBUG((void *)handle)
                                         _IF_DEBUG("deleted"));
    ASSERT(table != NULL, "fail to get handle table");
    DOLOG(HANDLE_VERBOSE_3, { report_callstack(drcontext, mc); });
    handle_table_lock();
    if (!handlecheck_handle_remove(table, handle, &hci)) {
        LOG(HANDLE_VERBOSE_1,
            "WARNING: fail to remove handle "PFX" at:\n", handle);
        DOLOG(HANDLE_VERBOSE_2, { report_callstack(drcontext, mc); });
    }
    handle_table_unlock();
    return (void *)hci;
}

void
handlecheck_delete_handle_post_syscall(void *drcontext, HANDLE handle,
                                       drsys_sysnum_t sysnum, dr_mcontext_t *mc,
                                       int type, void *handle_info, bool success)
{
    handle_callstack_info_t *hci;
    hashtable_t *table;

    hci = (handle_callstack_info_t *)handle_info;
    /* i#1380: ignore handle deleted in other process */
    if (hci == &other_proc_hci)
        return;
    if (hci == NULL) {
        if (success) {
            LOG(HANDLE_VERBOSE_2,
                "WARNING: delete handle succeeded unexpectedly\n");
        } else {
            LOG(HANDLE_VERBOSE_2,
                "WARNING: no handle info for adding back\n");
        }
        return;
    }

    if (success) {
        /* add the pair info */
        if (options.filter_handle_leaks) {
            hashtable_lock(&open_close_table);
            open_close_pair_add(hci, sysnum, mc);
            hashtable_unlock(&open_close_table);
        }
        /* closed handle successfully, free the handle info now */
        handle_callstack_info_free(hci);
    } else {
        /* failed to delete handle, add handle back */
        ASSERT(handle != INVALID_HANDLE_VALUE, "add back invalid handle value");
        table = handlecheck_get_handle_table(type
                                             _IF_DEBUG((void *)handle)
                                             _IF_DEBUG("added back"));
        ASSERT(table != NULL, "fail to get handle table");
        DOLOG(HANDLE_VERBOSE_3, { packed_callstack_log(hci->pcs, INVALID_FILE); });
        handle_table_lock();
        if (!handlecheck_handle_add(table, handle, hci)) {
            LOG(HANDLE_VERBOSE_1,
                "WARNING: failed to add handle "PFX" back\n", handle);
        }
        handle_table_unlock();
    }
}

#ifdef STATISTICS
void
handlecheck_dump_statistics(void)
{
    dr_fprintf(f_global, "handles opened: %6u, closed: %6u\n",
               num_handle_add, num_handle_remove);
}
#endif /* STATISTICS */

void
handlecheck_nudge(void *drcontext)
{
    handlecheck_iterate_handles();
}
